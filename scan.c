// Fast parallel disk usage scanner for macOS, Linux and Windows. Prints a pruned JSON tree on stdout.
//
// Why it is fast:
// - macOS: getattrlistbulk(2), one syscall returns name + type + allocated size for a whole batch of
//   directory entries, so files are never opened or stat'ed one by one.
// - Linux: getdents64(2) with a large buffer, and d_type tells folders apart without any stat;
//   only files get an fstatat for their allocated size.
// - Windows: NtQueryDirectoryFile returns name, attributes, allocated size and file id for a whole batch.
// - openat(parent_fd, name), NtCreateFile relative to the parent on Windows: no full path lookup, so threads
//   don't fight over the same top-level vnodes in the kernel.
// - each thread walks depth-first on its own stack and only hands work out when a thread is idle.
// - sealed system folders are walked once: their totals are kept until the system is updated.
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <io.h>
#include <sddl.h>
#include <shellapi.h>
#define OS "win"
typedef HANDLE fd_t;
#define BAD_FD(fd) ((fd) == NULL)
#define close_fd CloseHandle
#define path_cmp _stricmp
#define path_ncmp _strnicmp
#else
#define path_cmp strcmp
#define path_ncmp strncmp
#include <sys/resource.h>
#include <sys/statvfs.h>
typedef int fd_t;
#define BAD_FD(fd) ((fd) < 0)
#define close_fd close
#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#define OS "mac"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#define OS "linux"
#endif
#endif

#define MIN_SIZE 1000000ULL  // smaller items are merged into one "others" entry
#define MAX_KIDS 300
#define BUF_SIZE (128 * 1024)
#define MAX_THREADS 8  // measured fastest on macOS; more threads only add APFS lock contention in the kernel

typedef struct Node {
    char *name;
    char *path;  // only set on ancestors of skipped mount points
    uint64_t size, small;
    union {
        struct Node **kids;
        struct Frozen *ice;  // a sealed folder being walked has no kids
    };
    uint32_t nkids, cap, nsmall;
    uint8_t dir, err, frozen;
} Node;

// An open directory kept alive until all its subdirectories have been opened from it
typedef struct Dir {
    fd_t fd;
    _Atomic int refs;
#ifdef _WIN32
    uint64_t vol;  // the drive letter, mixed into file ids
    int ntfs;      // file ids are unique there, so hard links can be told apart
#endif
} Dir;

// Inside a dependency folder (node_modules, vendor, .venv…) sizes are only added up into its node (sum):
// such jobs carry their own name instead of a tree node
typedef struct { Node *node; Dir *parent; Node *sum; char *name; } Job;
typedef struct { Job *v; size_t n, cap; } Stack;

static Stack shared;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;
static _Atomic long pending;  // jobs not finished yet, anywhere
static _Atomic int idle;
static _Atomic uint64_t scanned;
static _Atomic int stopping;  // set when the scan gives up on folders stuck behind a privacy prompt
static _Atomic uint64_t counted;  // bytes so far, for the app's progress line
static int live;  // EVICT_PROGRESS: the app reads one JSON progress line on stderr every 300 ms
static const char *root;

static char **skips;
static int nskips;

static uint64_t *links;
static size_t nlinks, caplinks;
static pthread_mutex_t llock = PTHREAD_MUTEX_INITIALIZER;

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) { perror("realloc"); exit(1); }
    return p;
}

static Node *new_node(const char *name, size_t len, int dir) {
    Node *n = calloc(1, sizeof *n);
    if (!n || !(n->name = malloc(len + 1))) { perror("alloc"); exit(1); }
    memcpy(n->name, name, len + 1);
    n->dir = dir;
    return n;
}

static void add_kid(Node *p, Node *k) {
    if (p->nkids == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->kids = xrealloc(p->kids, p->cap * sizeof *p->kids);
    }
    p->kids[p->nkids++] = k;
}

static void push(Stack *s, Job j) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->v = xrealloc(s->v, s->cap * sizeof *s->v);
    }
    s->v[s->n++] = j;
}

static void release(Dir *d) {
    if (atomic_fetch_sub_explicit(&d->refs, 1, memory_order_acq_rel) == 1) {
        close_fd(d->fd);
        free(d);
    }
}

// Hard links must be counted once: open-addressing set of (dev, inode) keys
static int link_seen(uint64_t key) {
    if (!key) key = 1;
    pthread_mutex_lock(&llock);
    if (nlinks * 2 >= caplinks) {
        size_t oldcap = caplinks;
        uint64_t *old = links;
        caplinks = caplinks ? caplinks * 2 : 1 << 16;
        if (!(links = calloc(caplinks, sizeof *links))) { perror("calloc"); exit(1); }
        for (size_t i = 0; i < oldcap; i++)
            if (old[i]) {
                size_t j = (old[i] * 0x9E3779B97F4A7C15ULL >> 20) & (caplinks - 1);
                while (links[j]) j = (j + 1) & (caplinks - 1);
                links[j] = old[i];
            }
        free(old);
    }
    size_t i = (key * 0x9E3779B97F4A7C15ULL >> 20) & (caplinks - 1);
    while (links[i]) {
        if (links[i] == key) { pthread_mutex_unlock(&llock); return 1; }
        i = (i + 1) & (caplinks - 1);
    }
    links[i] = key;
    nlinks++;
    pthread_mutex_unlock(&llock);
    return 0;
}

#ifndef _WIN32  // on Windows, other volumes are other drives, and mount points are reparse points, never followed
static void add_skip(const char *path) {
    skips = xrealloc(skips, (nskips + 1) * sizeof *skips);
    skips[nskips++] = strdup(path);
}
#endif

// Sealed system folders only change when the system is updated: each one is walked once, shown as one block, and
// its total is reused from the cache file until the key (the system's version) changes.
// sealed: "/dir" is that folder, "/dir/" is every folder directly inside. lives: folders that do change; neither
// they nor any folder above them are ever taken from the cache.
// Hard links: the cached folders count theirs among themselves, and the file keeps their ids, so a link to the same
// file elsewhere on the disk (most of System32 is linked from WinSxS) isn't counted again.
typedef struct Frozen {
    char *path;
    uint64_t size, *keys, owed;  // owed: links the rest of this scan counted first, part of the folder's total
    uint32_t count, nkeys, capkeys;
    struct Node *node;
} Frozen;
static char *sealed[8], **lives, cache_key[128], cache_file[4200];
static int nsealed, nlives, nfrozen, nfresh;
static Frozen *frozen, **fresh;  // read from the file, walked by this scan
static pthread_mutex_t ilock = PTHREAD_MUTEX_INITIALIZER;

static void add_live(const char *path) {
    lives = xrealloc(lives, (nlives + 1) * sizeof *lives);
    lives[nlives++] = strdup(path);
}

static int is_sealed(const char *path) {
    for (int i = 0; i < nsealed; i++) {
        size_t l = strlen(sealed[i]);
        if (sealed[i][l - 1] == '/' ? !path_ncmp(sealed[i], path, l) && !strchr(path + l, '/') : !path_cmp(sealed[i], path))
            return 1;
    }
    return 0;
}

static Frozen *find_frozen(const char *path) {
    for (int i = 0; i < nfrozen; i++)
        if (!path_cmp(frozen[i].path, path)) return &frozen[i];
    return NULL;
}

static void add_key(Frozen *z, uint64_t key) {
    if (z->nkeys == z->capkeys) {
        z->capkeys = z->capkeys ? z->capkeys * 2 : 16;
        z->keys = xrealloc(z->keys, z->capkeys * sizeof *z->keys);
    }
    z->keys[z->nkeys++] = key;
}

// The file: the key on the first line, then for each folder a "size count path" line and its "+link id" lines
static void load_cache(void) {
    FILE *f = cache_key[0] ? fopen(cache_file, "r") : NULL;
    char line[4400];
    size_t kl = strlen(cache_key), rl = strlen(root);
    if (!f) return;
    if (fgets(line, sizeof line, f) && !strncmp(line, cache_key, kl) && line[kl] == '\n')
        while (fgets(line, sizeof line, f)) {
            unsigned long long size;
            unsigned count;
            int at = 0;
            size_t l = strlen(line);
            if (!l || line[l - 1] != '\n') continue;
            line[l - 1] = 0;
            if (line[0] == '+') {
                if (nfrozen) add_key(&frozen[nfrozen - 1], strtoull(line + 1, NULL, 16));
            } else if (sscanf(line, "%llu %u %n", &size, &count, &at) >= 2 && line[at] == '/') {
                frozen = xrealloc(frozen, (nfrozen + 1) * sizeof *frozen);
                frozen[nfrozen++] = (Frozen){.path = strdup(line + at), .size = size, .count = count};
            }
        }
    fclose(f);
    // The folders this scan will take from the file have already counted these links
    for (int i = 0; i < nfrozen; i++)
        if (rl == 1 || (!path_ncmp(frozen[i].path, root, rl) && frozen[i].path[rl] == '/'))
            for (uint32_t k = 0; k < frozen[i].nkeys; k++) link_seen(frozen[i].keys[k]), link_seen(~frozen[i].keys[k]);
}

static void save_one(FILE *f, const Frozen *z, uint64_t size, uint32_t count) {
    fprintf(f, "%llu %u %s\n", (unsigned long long)size, count, z->path);
    for (uint32_t k = 0; k < z->nkeys; k++) fprintf(f, "+%llx\n", (unsigned long long)z->keys[k]);
}

static void save_cache(void) {
    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", cache_file);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%s\n", cache_key);
    for (int i = 0; i < nfrozen; i++) save_one(f, &frozen[i], frozen[i].size, frozen[i].count);
    for (int i = 0; i < nfresh; i++) {
        Node *n = fresh[i]->node;
        if (!n->err && !strchr(fresh[i]->path, '\n')) save_one(f, fresh[i], n->small + fresh[i]->owed, n->nsmall);
    }
    if (fclose(f)) return;
    remove(cache_file);  // Windows doesn't rename over a file
    rename(tmp, cache_file);
}

#ifdef __APPLE__
// Skip every mount point outside the boot APFS container, plus the firmlinked Data volume
// (already reached through /Users, /Applications...) and the sealed system snapshot copy.
static void init_skips(void) {
    struct statfs *m;
    int n = getmntinfo(&m, MNT_NOWAIT);
    char container[64] = "";
    for (int i = 0; i < n; i++)
        if (!strcmp(m[i].f_mntonname, "/")) {
            const char *s = m[i].f_mntfromname;
            size_t k = strncmp(s, "/dev/disk", 9) ? 0 : 9;
            while (s[k] >= '0' && s[k] <= '9') k++;
            snprintf(container, sizeof container, "%.*s", (int)k, s);
        }
    size_t cl = strlen(container);
    for (int i = 0; i < n; i++) {
        const char *on = m[i].f_mntonname, *from = m[i].f_mntfromname;
        if (!strcmp(on, "/") || !strcmp(on, root)) continue;
        int same = cl > 9 && !strncmp(from, container, cl) && from[cl] == 's';
        if (!same || !strcmp(on, "/System/Volumes/Data") || !strcmp(on, "/System/Volumes/Update/mnt1"))
            add_skip(on);
        else add_live(on);
    }
    add_skip("/Volumes");

    // The sealed system volume changes with the build only. Firmlinks are the folders that look like part of it
    // but live on the Data volume: without their list, nothing is cached.
    FILE *f = fopen("/usr/share/firmlinks", "r");
    char line[2048];
    size_t kl = sizeof cache_key - 2;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\t\n")] = 0;
        if (line[0] == '/') add_live(line);
    }
    fclose(f);
    memcpy(cache_key, "1 ", 2);
    if (sysctlbyname("kern.osversion", cache_key + 2, &kl, NULL, 0)) cache_key[0] = 0;
    static char *dirs[] = {"/System/", "/System/Library/", "/usr/", "/bin", "/sbin"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) sealed[nsealed++] = dirs[i];
}
#elif defined(_WIN32)
// Paths in the report look like POSIX ones, so the page works unchanged: C:\Users\me is /C:/Users/me, and "/" is
// every fixed drive when there are several. The app turns them back into Windows paths.
#ifndef FILE_OPEN
#define FILE_OPEN 1
#endif
#define DIR_FLAGS 0x4021   // FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT
#define NO_FOLLOW 0x200000  // FILE_OPEN_REPARSE_POINT
#define ID_FULL_DIR_INFO 38  // FileIdFullDirectoryInformation

typedef struct {
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength, EaSize;  // EaSize holds the reparse tag of a reparse point
    LARGE_INTEGER FileId;
    WCHAR FileName[1];
} DirInfo;

typedef NTSTATUS(NTAPI *CreateFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                  ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS(NTAPI *QueryFn)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, int, BOOLEAN,
                                 PUNICODE_STRING, BOOLEAN);
static CreateFn nt_create;
static QueryFn nt_query;
static uint64_t disk_total, disk_free;
static char drives[26][4];
static int ndrives;

static void init_skips(void) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    nt_create = (CreateFn)(void *)GetProcAddress(nt, "NtCreateFile");
    nt_query = (QueryFn)(void *)GetProcAddress(nt, "NtQueryDirectoryFile");
    // As administrator, backup intent reads every folder whatever its permissions
    HANDLE token;
    TOKEN_PRIVILEGES tp = {1, {{{0, 0}, SE_PRIVILEGE_ENABLED}}};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
        if (LookupPrivilegeValueW(NULL, L"SeBackupPrivilege", &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(token);
    }
}

static char *to_report(const char *native);

// Windows and System32 change with updates and with components added or removed (the WinSxS folder's date then
// moves), except the folders listed here, which programs, drivers and the system itself keep writing to.
static void init_sealed(void) {
    static const char *live[] = {"WinSxS", "servicing", "Temp", "Logs", "Prefetch", "SoftwareDistribution", "Installer",
        "Minidump", "LiveKernelReports", "ServiceProfiles", "Panther", "INF", "Fonts", "assembly", "Microsoft.NET",
        "appcompat", "CSC", "debug", "Downloaded Program Files", "System32/config", "System32/winevt",
        "System32/LogFiles", "System32/DriverStore", "System32/drivers", "System32/spool", "System32/Tasks",
        "System32/sru", "System32/wbem", "System32/catroot", "System32/catroot2", "System32/WDI",
        "System32/Microsoft", "System32/CodeIntegrity", "System32/SleepStudy", "System32/GroupPolicy"};
    char native[MAX_PATH], build[32] = "", buf[MAX_PATH + 64];
    DWORD ubr = 0, n = sizeof build, un = sizeof ubr;
    WIN32_FILE_ATTRIBUTE_DATA sxs;
    const char *key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    UINT l = GetWindowsDirectoryA(native, MAX_PATH);
    if (!l || l >= MAX_PATH) return;
    snprintf(buf, sizeof buf, "%s\\WinSxS", native);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "CurrentBuildNumber", RRF_RT_REG_SZ, NULL, build, &n) ||
        RegGetValueA(HKEY_LOCAL_MACHINE, key, "UBR", RRF_RT_REG_DWORD, NULL, &ubr, &un) ||
        !GetFileAttributesExA(buf, GetFileExInfoStandard, &sxs))
        return;
    snprintf(cache_key, sizeof cache_key, "1 %s.%lu %lx%08lx", build, (unsigned long)ubr,
             (unsigned long)sxs.ftLastWriteTime.dwHighDateTime, (unsigned long)sxs.ftLastWriteTime.dwLowDateTime);
    char *win = to_report(native);
    snprintf(buf, sizeof buf, "%s/", win);
    sealed[nsealed++] = strdup(buf);
    snprintf(buf, sizeof buf, "%s/System32/", win);
    sealed[nsealed++] = strdup(buf);
    for (size_t i = 0; i < sizeof live / sizeof *live; i++) {
        snprintf(buf, sizeof buf, "%s/%s", win, live[i]);
        add_live(buf);
    }
}

static WCHAR *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = malloc(n * sizeof *w);
    if (!w) { perror("malloc"); exit(1); }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *narrow(const WCHAR *w, int len) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
    char *s = malloc(n + 1);
    if (!s) { perror("malloc"); exit(1); }
    WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
    s[n] = 0;
    return s;
}

// C:\Users\me (or C:/Users/me) → /C:/Users/me
static char *to_report(const char *native) {
    size_t l = strlen(native);
    char *p = malloc(l + 2);
    if (!p) { perror("malloc"); exit(1); }
    p[0] = '/';
    for (size_t i = 0; i <= l; i++) p[i + 1] = native[i] == '\\' ? '/' : native[i];
    for (l++; l > 1 && p[l - 1] == '/'; l--) p[l - 1] = 0;
    return p;
}

// /C:/Users/me → \\?\C:\Users\me, /C: → \\?\C:\ (a drive root needs its backslash)
static WCHAR *to_native(const char *report) {
    char buf[4200];
    int n = snprintf(buf, sizeof buf, "\\\\?\\%s%s", report + 1, strlen(report) == 3 ? "\\" : "");
    if (n < 0 || n >= (int)sizeof buf) return NULL;
    for (char *c = buf + 4; *c; c++) if (*c == '/') *c = '\\';
    return widen(buf);
}

static HANDLE open_path(const char *report) {
    WCHAR *w = to_native(report);
    if (!w) return NULL;
    HANDLE h = CreateFileW(w, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    return h == INVALID_HANDLE_VALUE ? NULL : h;
}

// The Windows openat: the name is looked up in the parent's handle only, and reparse points aren't followed
static HANDLE open_at(HANDLE parent, const char *name) {
    WCHAR w[600];
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 600);
    if (n <= 1) return NULL;
    UNICODE_STRING us = {(USHORT)((n - 1) * 2), (USHORT)(n * 2), w};
    OBJECT_ATTRIBUTES oa = {.Length = sizeof oa, .RootDirectory = parent, .ObjectName = &us};
    IO_STATUS_BLOCK io;
    HANDLE h;
    NTSTATUS st = nt_create(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &io, NULL, 0,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, DIR_FLAGS | NO_FOLLOW, NULL, 0);
    return st >= 0 ? h : NULL;
}

// Every fixed drive (internal disks), like the real disks read on Linux
static void find_drives(void) {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        WCHAR d[] = {(WCHAR)('A' + i), ':', '\\', 0};
        if (mask >> i & 1 && GetDriveTypeW(d) == DRIVE_FIXED && GetVolumeInformationW(d, NULL, 0, NULL, NULL, NULL, NULL, 0))
            snprintf(drives[ndrives++], 4, "%c:", 'A' + i);
    }
}

static void add_space(char drive) {
    WCHAR d[] = {(WCHAR)drive, ':', '\\', 0};
    ULARGE_INTEGER avail, size;
    if (GetDiskFreeSpaceExW(d, &avail, &size, NULL)) disk_total += size.QuadPart, disk_free += avail.QuadPart;
}

// The user's Recycle Bin on the home drive: /C:/$Recycle.Bin/<user SID>
static char *recycle_bin(const char *home) {
    HANDLE token;
    char *out = NULL;
    if (!home[0] || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    union { TOKEN_USER user; char buf[256]; } u;
    DWORD n;
    LPSTR sid;
    if (GetTokenInformation(token, TokenUser, &u, sizeof u, &n) && ConvertSidToStringSidA(u.user.User.Sid, &sid)) {
        out = malloc(strlen(sid) + 20);
        if (out) sprintf(out, "/%.2s/$Recycle.Bin/%s", home + 1, sid);
        LocalFree(sid);
    }
    CloseHandle(token);
    return out;
}
#else
static uint64_t disk_total, disk_free;

// mountinfo escapes spaces, tabs, newlines and backslashes as \ooo
static void unescape(char *s) {
    char *o = s;
    for (; *s; s++, o++)
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
            *o = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else *o = *s;
    *o = 0;
}

// Only real disks are read: pseudo filesystems (proc, sysfs, tmpfs, overlay…), network shares, FUSE, snap images
// and removable media are skipped, and a filesystem mounted twice (bind mounts) is read once. The disk size is
// the sum of every disk read, so a separate /home partition counts.
static void init_skips(void) {
    static const char *disks[] = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "zfs", "bcachefs", "jfs",
                                  "reiserfs", "nilfs2", "vfat", "exfat", "ntfs", "ntfs3", "fuseblk"};
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return;
    char line[16384], fsroot[4096], on[4096], type[256], src[4096], **seen = NULL;
    unsigned *devs = NULL;
    size_t nseen = 0, ndevs = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned maj, min;
        char *dash = strstr(line, " - ");
        if (!dash || sscanf(line, "%*d %*d %u:%u %4095s %4095s", &maj, &min, fsroot, on) != 4 ||
            sscanf(dash + 3, "%255s %4095s", type, src) != 2)
            continue;
        unescape(fsroot);
        unescape(on);
        int keep = 0;
        for (size_t i = 0; i < sizeof disks / sizeof *disks; i++) keep |= !strcmp(type, disks[i]);
        if (!strncmp(on, "/media/", 7) || !strncmp(on, "/run/media/", 11)) keep = 0;
        char key[8192];
        snprintf(key, sizeof key, "%u:%u %s", maj, min, fsroot);
        for (size_t i = 0; keep && i < nseen; i++) keep = strcmp(seen[i], key) != 0;
        if (keep) {
            seen = xrealloc(seen, (nseen + 1) * sizeof *seen);
            seen[nseen++] = strdup(key);
            unsigned dev = maj << 20 | min, counted = 0;
            for (size_t i = 0; i < ndevs; i++) counted |= devs[i] == dev;
            struct statvfs v;
            // shortcut: ZFS datasets share their pool's free space, so it's counted once per dataset; fine for ext4/btrfs/xfs
            if (!counted && !statvfs(on, &v)) {
                devs = xrealloc(devs, (ndevs + 1) * sizeof *devs);
                devs[ndevs++] = dev;
                disk_total += (uint64_t)(v.f_blocks - v.f_bfree + v.f_bavail) * v.f_frsize;
                disk_free += (uint64_t)v.f_bavail * v.f_frsize;
            }
            if (strcmp(on, "/")) add_live(on);
        } else if (strcmp(on, "/") && strcmp(on, root)) {
            add_skip(on);
        }
    }
    fclose(f);

    // /usr belongs to the package manager: the key is the state of its database, and without a known one nothing
    // is cached. On a merged /usr, /bin and /lib are links and never match.
    // shortcut: files put in /usr by hand (sudo pip, make install outside /usr/local) are seen at the next package
    // change; add a date check of the folders if that matters
    static const char *dbs[] = {"/var/lib/dpkg/status", "/var/lib/rpm/rpmdb.sqlite", "/var/lib/rpm/rpmdb.sqlite-wal",
                                "/var/lib/rpm/Packages", "/usr/lib/sysimage/rpm/rpmdb.sqlite",
                                "/usr/lib/sysimage/rpm/rpmdb.sqlite-wal", "/usr/lib/sysimage/rpm/Packages.db",
                                "/var/lib/pacman/local", "/lib/apk/db/installed"};
    uint64_t h = 0;
    struct stat st;
    for (size_t i = 0; i < sizeof dbs / sizeof *dbs; i++)
        if (!stat(dbs[i], &st))
            h = (h ^ (uint64_t)st.st_mtim.tv_sec ^ (uint64_t)st.st_mtim.tv_nsec << 32 ^ (uint64_t)st.st_size ^ (i + 1)) *
                0x9E3779B97F4A7C15ULL;
    if (!h) return;
    snprintf(cache_key, sizeof cache_key, "1 %llx", (unsigned long long)h);
    static char *dirs[] = {"/usr/", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) sealed[nsealed++] = dirs[i];
    add_live("/usr/local");
}
#endif


// 0 = nothing to do, 1 = skip this path, 2 = keep its path (a skipped mount point or a live folder lies below,
// or it is a live folder)
static int skip_state(const char *path) {
    size_t l = strlen(path);
    int below = 0;
    for (int i = 0; i < nskips; i++) {
        if (!strcmp(path, skips[i])) return 1;
        if (!strncmp(skips[i], path, l) && (skips[i][l] == '/' || l == 1)) below = 1;
    }
    for (int i = 0; i < nlives && !below; i++)
        below = !path_ncmp(lives[i], path, l) && (!lives[i][l] || lives[i][l] == '/' || l == 1);
    return below ? 2 : 0;
}

// Package and dependency folders: their total matters, the thousands of folders inside don't
static int is_deps(const char *name) {
    static const char *names[] = {"node_modules", "vendor", ".venv", "venv", "site-packages", "__pycache__", "Pods",
                                  "bower_components", ".tox", ".bundle", ".pnpm-store", ".gradle", ".m2", ".cargo"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(name, names[i])) return 1;
    return 0;
}

// One folder being read: its tree node, or inside a dependency folder the node that adds everything up (sum)
typedef struct {
    Node *dir, *sum;
    Dir *me;
    Stack *local;
    uint64_t count, bytes, deep, ndeep, owed;
} Scan;

static void add_dir(Scan *s, const char *name, size_t nlen) {
    if (s->sum) {
        char *copy = malloc(nlen + 1);
        if (!copy) { perror("malloc"); exit(1); }
        memcpy(copy, name, nlen + 1);
        push(s->local, (Job){NULL, s->me, s->sum, copy});
        return;
    }
    char *path = NULL;
    if (s->dir->path) {
        size_t pl = strlen(s->dir->path) == 1 ? 0 : strlen(s->dir->path);
        path = malloc(pl + nlen + 2);
        if (!path) { perror("malloc"); exit(1); }
        memcpy(path, s->dir->path, pl);
        path[pl] = '/';
        memcpy(path + pl + 1, name, nlen + 1);
        int st = skip_state(path);
        if (st == 1) { free(path); return; }
        if (st == 0 && cache_key[0] && is_sealed(path)) {
            Node *k = new_node(name, nlen, 1);
            k->path = path;
            k->frozen = 1;
            add_kid(s->dir, k);
            Frozen *f = find_frozen(path);
            if (f) {
                k->small = f->size;
                k->nsmall = f->count;
                s->bytes += f->size;
                s->count += f->count;
                return;
            }
            if (!(k->ice = calloc(1, sizeof *k->ice))) { perror("calloc"); exit(1); }
            k->ice->path = path;
            k->ice->node = k;
            pthread_mutex_lock(&ilock);
            fresh = xrealloc(fresh, (nfresh + 1) * sizeof *fresh);
            fresh[nfresh++] = k->ice;
            pthread_mutex_unlock(&ilock);
            push(s->local, (Job){k, s->me, NULL, NULL});
            return;
        }
        if (st == 0) { free(path); path = NULL; }
    }
    Node *k = new_node(name, nlen, 1);
    k->path = path;
    add_kid(s->dir, k);
    push(s->local, (Job){k, s->me, NULL, NULL});
}

static void add_file(Scan *s, const char *name, size_t nlen, uint64_t alloc, uint32_t nlink, uint64_t key) {
    if (nlink > 1 && s->sum && s->sum->frozen) {
        if (link_seen(~key)) return;
        pthread_mutex_lock(&ilock);
        add_key(s->sum->ice, key);
        pthread_mutex_unlock(&ilock);
        if (link_seen(key)) { s->owed += alloc; return; }
    } else if (nlink > 1 && link_seen(key)) return;
    s->bytes += alloc;
    if (s->sum) {
        s->deep += alloc;
        s->ndeep++;
    } else if (alloc >= MIN_SIZE) {
        Node *k = new_node(name, nlen, 0);
        k->size = alloc;
        add_kid(s->dir, k);
    } else {
        s->dir->small += alloc;
        s->dir->nsmall++;
    }
}

static void scan_dir(Job j, char *buf, Stack *local) {
    Node *dir = j.node, *sum = j.sum ? j.sum : dir && (dir->frozen || (!dir->path && is_deps(dir->name))) ? dir : NULL;
    const char *dname = dir ? dir->name : j.name;
#ifdef _WIN32
    // Without a parent: the scan root, or one of the drives under "/"
    char top[8];
    if (!j.parent && dname[0] != '/') snprintf(top, sizeof top, "/%s", dname);
    fd_t fd = j.parent ? open_at(j.parent->fd, dname) : open_path(dname[0] == '/' ? dname : top);
    Dir *from = j.parent;
    uint64_t vol = 0;
    int ntfs = 0;
    if (from) vol = from->vol, ntfs = from->ntfs;
    else if (!BAD_FD(fd)) {
        WCHAR fs[32] = L"";
        GetVolumeInformationByHandleW(fd, NULL, 0, NULL, NULL, NULL, fs, 32);
        // An NTFS file id is a 48-bit record number under 2^32 (the most files a volume holds) and a 16-bit sequence:
        // the drive letter goes in the record bits NTFS never uses, so files of two drives can't share a key
        vol = (uint64_t)(unsigned char)(dname[0] == '/' ? dname[1] : dname[0]) << 40;
        ntfs = !wcscmp(fs, L"NTFS");
    }
#else
    fd_t fd = j.parent ? openat(j.parent->fd, dname, O_RDONLY | O_DIRECTORY | O_NOFOLLOW) : open(root, O_RDONLY | O_DIRECTORY);
#endif
    free(j.name);
    // The tree is being printed: a thread released late by a privacy prompt must not touch it
    if (atomic_load(&stopping)) { if (!BAD_FD(fd)) close_fd(fd); return; }
    if (j.parent) release(j.parent);
    Node *owner = dir ? dir : sum;  // a folder inside a dependency folder reports its errors on that folder
    if (BAD_FD(fd)) { __atomic_store_n(&owner->err, 1, __ATOMIC_RELAXED); return; }
    Dir *me = malloc(sizeof *me);
    if (!me) { perror("malloc"); exit(1); }
    me->fd = fd;
    atomic_init(&me->refs, 1);
#ifdef _WIN32
    me->vol = vol;
    me->ntfs = ntfs;
#endif
    Scan s = {dir, sum, me, local, 0, 0, 0, 0, 0};
    size_t first = local->n;

#ifdef _WIN32
    IO_STATUS_BLOCK io;
    for (;;) {
        NTSTATUS st = nt_query(fd, NULL, NULL, NULL, &io, buf, BUF_SIZE, ID_FULL_DIR_INFO, FALSE, NULL, FALSE);
        // STATUS_NO_MORE_FILES, or STATUS_NO_SUCH_FILE for an empty drive root (FAT)
        if (st < 0) {
            if (st != (NTSTATUS)0x80000006 && st != (NTSTATUS)0xC000000F) __atomic_store_n(&owner->err, 1, __ATOMIC_RELAXED);
            break;
        }
        for (char *p = buf;;) {
            DirInfo *e = (DirInfo *)p;
            int wl = (int)(e->FileNameLength / 2);
            const WCHAR *wn = e->FileName;
            if (!(wn[0] == '.' && (wl == 1 || (wl == 2 && wn[1] == '.')))) {
                // shortcut: a name with a lone UTF-16 surrogate becomes U+FFFD and such a folder can't be reopened
                char ename[1024];
                int nlen = WideCharToMultiByte(CP_UTF8, 0, wn, wl, ename, sizeof ename - 1, NULL, NULL);
                ename[nlen] = 0;
                s.count++;
                ULONG a = e->FileAttributes;
                if (a & FILE_ATTRIBUTE_DIRECTORY) {
                    // Junctions and symbolic links aren't followed; other reparse points (OneDrive folders) are real folders
                    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT) ||
                        (e->EaSize != IO_REPARSE_TAG_MOUNT_POINT && e->EaSize != IO_REPARSE_TAG_SYMLINK))
                        add_dir(&s, ename, nlen);
                } else {
                    // Listings don't give the link count: on NTFS every file goes through the hard link set, unless it has
                    // no real id (0 or -1, as some file system filters report): those would all look like one file
                    uint64_t id = (uint64_t)e->FileId.QuadPart;
                    add_file(&s, ename, nlen, (uint64_t)e->AllocationSize.QuadPart, ntfs && id + 1 > 1 ? 2 : 1, id ^ vol);
                }
            }
            if (!e->NextEntryOffset) break;
            p += e->NextEntryOffset;
        }
        if (atomic_load_explicit(&stopping, memory_order_relaxed)) break;
    }
#elif defined(__APPLE__)
    struct attrlist al = {
        .bitmapcount = ATTR_BIT_MAP_COUNT,
        .commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_DEVID |
                      ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID,
        .fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE,
    };
    for (;;) {
        int r = getattrlistbulk(fd, &al, buf, BUF_SIZE, 0);
        if (r <= 0) { if (r < 0) __atomic_store_n(&owner->err, 1, __ATOMIC_RELAXED); break; }
        char *next = buf;
        for (int i = 0; i < r; i++) {
            if (atomic_load_explicit(&stopping, memory_order_relaxed)) break;
            // Entry layout: length, returned attrs, error, then requested attrs in bit order
            char *cur = next;
            uint32_t len;
            memcpy(&len, cur, 4);
            next = cur + len;
            cur += 4;
            attribute_set_t ret;
            memcpy(&ret, cur, sizeof ret);
            cur += sizeof ret;
            if (ret.commonattr & ATTR_CMN_ERROR) {
                uint32_t e;
                memcpy(&e, cur, 4);
                cur += 4;
                if (e) continue;
            }
            if (!(ret.commonattr & ATTR_CMN_NAME)) continue;
            attrreference_t ref;
            memcpy(&ref, cur, sizeof ref);
            const char *name = cur + ref.attr_dataoffset;
            size_t nlen = ref.attr_length ? ref.attr_length - 1 : 0;
            cur += sizeof ref;
            dev_t dev = 0;
            fsobj_type_t type = VNON;
            uint64_t fileid = 0;
            if (ret.commonattr & ATTR_CMN_DEVID) { memcpy(&dev, cur, sizeof dev); cur += sizeof dev; }
            if (ret.commonattr & ATTR_CMN_OBJTYPE) { memcpy(&type, cur, sizeof type); cur += sizeof type; }
            if (ret.commonattr & ATTR_CMN_FILEID) { memcpy(&fileid, cur, 8); cur += 8; }
            s.count++;
            if (type == VDIR) { add_dir(&s, name, nlen); continue; }
            uint32_t nlink = 1;
            uint64_t alloc = 0;
            if (ret.fileattr & ATTR_FILE_LINKCOUNT) { memcpy(&nlink, cur, 4); cur += 4; }
            if (ret.fileattr & ATTR_FILE_ALLOCSIZE) { memcpy(&alloc, cur, 8); cur += 8; }
            add_file(&s, name, nlen, alloc, nlink, fileid ^ ((uint64_t)(uint32_t)dev << 48));
        }
        if (atomic_load_explicit(&stopping, memory_order_relaxed)) break;
    }
#else
    struct stat st;
    // Folders take blocks too on Linux filesystems, and du counts them
    if (!fstat(fd, &st)) {
        uint64_t own = (uint64_t)st.st_blocks * 512;
        s.bytes += own;
        if (sum) s.deep += own; else dir->small += own;
    }
    for (;;) {
        long r = syscall(SYS_getdents64, fd, buf, BUF_SIZE);
        if (r <= 0) { if (r < 0) __atomic_store_n(&owner->err, 1, __ATOMIC_RELAXED); break; }
        for (long off = 0; off < r;) {
            // linux_dirent64: inode, offset, record length, type, name
            const char *ent = buf + off, *name = ent + 19;
            unsigned short reclen;
            memcpy(&reclen, ent + 16, 2);
            off += reclen;
            if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
            size_t nlen = strlen(name);
            s.count++;
            if (ent[18] == DT_DIR) { add_dir(&s, name, nlen); continue; }
            if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) continue;
            if (S_ISDIR(st.st_mode)) add_dir(&s, name, nlen);  // DT_UNKNOWN on some filesystems
            else add_file(&s, name, nlen, (uint64_t)st.st_blocks * 512, (uint32_t)st.st_nlink,
                          (uint64_t)st.st_ino ^ ((uint64_t)(uint32_t)st.st_dev << 48));
        }
        if (atomic_load_explicit(&stopping, memory_order_relaxed)) break;
    }
#endif

    // Several threads add up the same dependency folder
    if (sum) {
        __atomic_fetch_add(&sum->small, s.deep, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sum->nsmall, (uint32_t)s.ndeep, __ATOMIC_RELAXED);
        if (s.owed) __atomic_fetch_add(&sum->ice->owed, s.owed, __ATOMIC_RELAXED);
    }
    size_t added = local->n - first;
    atomic_fetch_add_explicit(&me->refs, (int)added, memory_order_relaxed);
    atomic_fetch_add_explicit(&pending, (long)added, memory_order_relaxed);
    release(me);
    atomic_fetch_add_explicit(&scanned, s.count, memory_order_relaxed);
    if (live) atomic_fetch_add_explicit(&counted, s.bytes, memory_order_relaxed);
}

static void progress_line(void) {
    fprintf(stderr, "{\"count\":%llu,\"bytes\":%llu}\n", (unsigned long long)atomic_load(&scanned),
            (unsigned long long)atomic_load(&counted));
    fflush(stderr);
}

static void *worker(void *arg) {
    (void)arg;
    char *buf = malloc(BUF_SIZE);
    Stack local = {0};
    if (!buf) { perror("malloc"); exit(1); }
    for (;;) {
        if (!local.n) {
            pthread_mutex_lock(&qlock);
            atomic_fetch_add(&idle, 1);
            while (!shared.n && atomic_load(&pending)) pthread_cond_wait(&qcond, &qlock);
            atomic_fetch_sub(&idle, 1);
            if (!shared.n) { pthread_mutex_unlock(&qlock); break; }
            push(&local, shared.v[--shared.n]);
            pthread_mutex_unlock(&qlock);
        }
        scan_dir(local.v[--local.n], buf, &local);

        // Hand the shallowest half (biggest subtrees) to idle threads
        if (local.n > 1 && atomic_load_explicit(&idle, memory_order_relaxed)) {
            size_t give = local.n / 2;
            pthread_mutex_lock(&qlock);
            for (size_t i = 0; i < give; i++) push(&shared, local.v[i]);
            pthread_cond_broadcast(&qcond);
            pthread_mutex_unlock(&qlock);
            memmove(local.v, local.v + give, (local.n - give) * sizeof *local.v);
            local.n -= give;
        }
        if (atomic_fetch_sub(&pending, 1) == 1) {
            pthread_mutex_lock(&qlock);
            pthread_cond_broadcast(&qcond);
            pthread_mutex_unlock(&qlock);
        }
    }
    free(buf);
    free(local.v);
    return NULL;
}

static uint64_t total(Node *n) {
    if (!n->dir) return n->size;
    n->size = n->small;
    for (uint32_t i = 0; i < n->nkids; i++) n->size += total(n->kids[i]);
    return n->size;
}

static void emit_str(const char *s) {
    putchar('"');
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') { putchar('\\'); putchar(*c); }
        else if (*c < 0x20 || *c == '<') printf("\\u%04x", *c);  // '<' keeps "</script>" out of the page
        else putchar(*c);
    }
    putchar('"');
}

static int by_size(const void *a, const void *b) {
    uint64_t x = (*(Node *const *)a)->size, y = (*(Node *const *)b)->size;
    return x < y ? 1 : x > y ? -1 : 0;
}

// [name,size] for files, [name,size,[kids],err?] for dirs, [null,size,count] for merged small items
static void emit(Node *n) {
    putchar('[');
    emit_str(n->name);
    printf(",%llu", (unsigned long long)n->size);
    if (n->dir) {
        qsort(n->kids, n->nkids, sizeof *n->kids, by_size);
        fputs(",[", stdout);
        uint32_t keep = 0;
        while (keep < n->nkids && keep < MAX_KIDS && n->kids[keep]->size >= MIN_SIZE) {
            if (keep) putchar(',');
            emit(n->kids[keep++]);
        }
        uint64_t rest = n->small;
        uint32_t nrest = n->nsmall + (n->nkids - keep);
        for (uint32_t i = keep; i < n->nkids; i++) rest += n->kids[i]->size;
        if (nrest) printf("%s[null,%llu,%u]", keep ? "," : "", (unsigned long long)rest, nrest);
        putchar(']');
        if (n->err) fputs(",1", stdout);
    }
    putchar(']');
}

// History: each scan leaves one JSON line in a file of the user's (disk size, free space, size of the tree and of every
// folder of HIST_DIR or more), so the page can chart the disk over time and tell what grew between two scans
#define HIST_DIR 500000000ULL
#define HIST_LINES 400  // about a year of daily scans
#define HIST_DIRS 30    // the latest lines keep their folders, older ones only their totals

typedef struct { char *s; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) b->s = xrealloc(b->s, b->cap = (b->n + n + 1) * 2);
    memcpy(b->s + b->n, s, n);
    b->s[b->n += n] = 0;
}

static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }

// Escaped like emit_str()
static void bstr(Buf *b, const char *s) {
    bputs(b, "\"");
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        char e[8];
        if (*c == '"' || *c == '\\') e[0] = '\\', e[1] = *c, e[2] = 0;
        else if (*c < 0x20 || *c == '<') snprintf(e, sizeof e, "\\u%04x", *c);
        else e[0] = *c, e[1] = 0;
        bputs(b, e);
    }
    bputs(b, "\"");
}

static void hist_dirs(Buf *b, Node *n, const char *path, int *first) {
    if (!n->dir || n->size < HIST_DIR) return;
    bputs(b, *first ? "[" : ",[");
    *first = 0;
    bstr(b, path);
    char num[32];
    snprintf(num, sizeof num, ",%llu]", (unsigned long long)n->size);
    bputs(b, num);
    for (uint32_t i = 0; i < n->nkids; i++) {
        Node *k = n->kids[i];
        if (!k->dir || k->size < HIST_DIR) continue;
        size_t len = strlen(path) + strlen(k->name) + 2;
        char *kp = malloc(len);
        if (!kp) continue;
        snprintf(kp, len, "%s/%s", strcmp(path, "/") ? path : "", k->name);
        hist_dirs(b, k, kp, first);
        free(kp);
    }
}

static uint64_t num_after(const char *line, const char *key) {
    const char *p = strstr(line, key);
    return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}

// A string written by bstr(), at s (on its opening quote): returns it unescaped and moves s past it
static char *read_str(const char **s) {
    const char *p = *s + 1;
    char *out = malloc(strlen(p) + 1), *o = out;
    if (!out) return NULL;
    for (; *p && *p != '"'; p++) {
        if (*p != '\\') { *o++ = *p; continue; }
        p++;
        if (*p == 'u') { *o++ = (char)strtol((char[]){p[1], p[2], p[3], p[4], 0}, NULL, 16); p += 4; }
        else if (*p) *o++ = *p;
        else break;
    }
    *o = 0;
    *s = *p ? p + 1 : p;
    return out;
}

static const char *dirs_of(const char *line) {
    const char *p = strstr(line, "\"dirs\":[");
    return p ? p + 8 : "";
}

// Reads the [path,size] pair at p and moves p to the next one; NULL after the last
static char *next_dir(const char **p, uint64_t *size) {
    if (**p != '[') return NULL;
    const char *q = *p + 1;
    char *name = q[0] == '"' ? read_str(&q) : NULL;
    if (!name || *q != ',') { free(name); return NULL; }
    *size = strtoull(q + 1, (char **)&q, 10);
    if (*q != ']') { free(name); return NULL; }
    *p = q + 1 + (q[1] == ',');
    return name;
}

// Size of path in a history line, 0 when it wasn't among its folders
static uint64_t dir_size(const char *line, const char *path) {
    const char *p = dirs_of(line);
    uint64_t v, found = 0;
    for (char *name; !found && (name = next_dir(&p, &v)); free(name))
        if (!strcmp(name, path)) found = v;
    return found;
}

static void human(char *out, size_t len, uint64_t b) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1000 && i < 4) v /= 1000, i++;
    snprintf(out, len, i && v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

// One line for a notification, or nothing: the disk grew fast since the previous scan, or free space fell under 10%
static void alert(const char *prev, const char *cur, uint64_t disk, uint64_t avail, uint64_t used, const char *home) {
    uint64_t low = disk / 10, pfree = prev ? num_after(prev, "\"free\":") : UINT64_MAX;
    uint64_t pused = prev ? num_after(prev, "\"total\":") - pfree : used;  // --alert is for disk scans: used = total - free
    uint64_t grew = used > pused ? used - pused : 0, min = disk / 50 > 10000000000ULL ? disk / 50 : 10000000000ULL;
    char a[32], b[32], text[8400] = "";
    if (prev && grew >= min) {
        // The deepest folder that holds at least half of the growth
        const char *p = dirs_of(cur);
        char *best = NULL;
        uint64_t best_d = 0, v;
        int best_depth = 0;
        for (char *name; (name = next_dir(&p, &v)); ) {
            uint64_t was = dir_size(prev, name);
            int depth = 0;
            for (const char *c = name; *c; c++) depth += *c == '/';
            if (v > was && v - was >= grew / 2 && strcmp(name, root) && depth > best_depth) {
                free(best);
                best = name, best_d = v - was, best_depth = depth;
            } else free(name);
        }
        char date[32] = "";
        const char *d = strstr(prev, "\"d\":\"");
        if (d) snprintf(date, sizeof date, "%.10s", d + 5);
        human(a, sizeof a, grew);
        snprintf(text, sizeof text, "Your disk grew by %s since the last scan, on %s.", a, date);
        if (best) {
            size_t hl = home ? strlen(home) : 0;
            int tilde = hl && !strncmp(best, home, hl) && best[hl] == '/';
            char *shown = best + (tilde ? hl : 0);
#ifdef _WIN32
            // As Explorer writes it: C:\Users\me\AppData, not /C:/Users/me/AppData
            tilde = 0, shown = best + 1;
            for (char *c = shown; *c; c++) if (*c == '/') *c = '\\';
#endif
            human(b, sizeof b, best_d);
            snprintf(text + strlen(text), sizeof text - strlen(text), " Mostly %s%s (+%s).", tilde ? "~" : "", shown, b);
            free(best);
        }
    }
    if (low && avail < low && (pfree >= low || (pfree > avail && pfree - avail >= disk / 50))) {
        human(a, sizeof a, avail);
        snprintf(text + strlen(text), sizeof text - strlen(text), "%sOnly %s left on the disk (%.0f%%).", text[0] ? " " : "", a, avail * 100.0 / disk);
    }
    puts(text);
}

int main(int argc, char **argv) {
    static char rootbuf[4096];
    const char *arg = "/", *home = getenv("HOME"), *out_path = NULL;
#ifdef _WIN32
    // Arguments in UTF-8 whatever the code page: user names and folders are often not ASCII
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    argv = malloc((argc + 1) * sizeof *argv);
    if (!wargv || !argv) return 1;
    for (int i = 0; i < argc; i++) argv[i] = narrow(wargv[i], -1);
    const WCHAR *profile = _wgetenv(L"USERPROFILE");
    home = profile ? narrow(profile, -1) : NULL;
#endif
    live = getenv("EVICT_PROGRESS") != NULL;
    int history = 0, alert_only = 0;
    // evict-scan [--progress] [--home DIR] [--out FILE] [--history] [--alert] [folder]: flags too, since pkexec clears
    // the environment. --history keeps this scan in the history file and adds the history to the report; --alert
    // (background scans of the disk) keeps it too, then prints a line for a notification, or an empty line, instead
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--progress")) live = 1;
        else if (!strcmp(argv[i], "--history")) history = 1;
        else if (!strcmp(argv[i], "--alert")) history = alert_only = 1;
        else if (!strcmp(argv[i], "--home") && i + 1 < argc) home = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else arg = argv[i];
    }
    // --out FILE: the report in FILE and the messages in FILE.log, for a scanner started elevated (no pipes then)
    if (out_path) {
        char log[4200];
        snprintf(log, sizeof log, "%s.log", out_path);
#ifdef _WIN32
        if (!_wfreopen(widen(out_path), L"wb", stdout) || !_wfreopen(widen(log), L"w", stderr)) return 1;
#else
        if (!freopen(out_path, "w", stdout) || !freopen(log, "w", stderr)) return 1;
#endif
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    if (home && home[0] != '/') home = to_report(home);
    // "/" is every fixed drive; one drive alone is the root itself
    if (!strcmp(arg, "/")) {
        find_drives();
        static char one[8];
        if (ndrives == 1) snprintf(one, sizeof one, "/%s", drives[0]), arg = one;
    } else if (arg[0] != '/') {
        WCHAR full[4096], *w = widen(arg);
        if (GetFullPathNameW(w, 4096, full, NULL)) arg = to_report(narrow(full, -1));
        free(w);
    }
#endif
    snprintf(rootbuf, sizeof rootbuf, "%s", arg);
    for (size_t l = strlen(rootbuf); l > 1 && rootbuf[l - 1] == '/'; l--) rootbuf[l - 1] = 0;
    root = rootbuf;

#ifdef __APPLE__
    // Never download iCloud/Drive placeholder files while scanning
    setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_PROCESS,
                   IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
#endif
#ifndef _WIN32
    // Parents stay open while their subdirectories wait in a stack
    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) {
        rl.rlim_cur = rl.rlim_max < 1 << 20 ? rl.rlim_max : 1 << 20;
#ifdef OPEN_MAX
        if (setrlimit(RLIMIT_NOFILE, &rl)) { rl.rlim_cur = OPEN_MAX; setrlimit(RLIMIT_NOFILE, &rl); }
#else
        setrlimit(RLIMIT_NOFILE, &rl);
#endif
    }
#endif
    init_skips();
#ifdef _WIN32
    // shortcut: the system folder cache stays off on Windows: on Windows Server 2025 (CI) the cached total still comes
    // out 2 to 3 GB above a full walk, while Windows 11 and Server 2022 match; call init_sealed() here once 2025 does
    (void)init_sealed;
#endif
    // In a folder the uninstaller already removes. A scan as root (pkexec on Linux) keeps its own file: it reads
    // more folders, and root must not write in a folder the user controls.
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA"), *dir = "\\Evict", *name = "\\system.cache";
#elif defined(__APPLE__)
    const char *base = geteuid() ? home : "", *dir = "/Library/Caches/com.vassilidev.evict", *name = "/system.cache";
#else
    const char *base = geteuid() ? home : "", *dir = geteuid() ? "/.cache/evict" : "/var/cache", *name = geteuid() ? "/system.cache" : "/evict-system.cache";
#endif
    if (base) {
#ifdef _WIN32
        snprintf(cache_file, sizeof cache_file, "%s%s", base, dir);
        CreateDirectoryA(cache_file, NULL);
#else
        snprintf(cache_file, sizeof cache_file, "%s/.cache", base);
        if (geteuid() && strstr(dir, "/.cache")) mkdir(cache_file, 0700);  // a new Linux account has none yet
        snprintf(cache_file, sizeof cache_file, "%s%s", base, dir);
        mkdir(cache_file, 0700);
#endif
        snprintf(cache_file, sizeof cache_file, "%s%s%s", base, dir, name);
    }
    if (!cache_file[0]) cache_key[0] = 0;
    load_cache();
    static char out[1 << 20];
    setvbuf(stdout, out, _IOFBF, sizeof out);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    Node *top = new_node(root, strlen(root), 1);
    if (skip_state(root) == 2) top->path = strdup(root);
#ifdef _WIN32
    if (!strcmp(root, "/")) {
        for (int i = 0; i < ndrives; i++) {
            Node *d = new_node(drives[i], 2, 1);
            char dpath[8];
            snprintf(dpath, sizeof dpath, "/%s", drives[i]);
            if (skip_state(dpath) == 2) d->path = strdup(dpath);
            add_kid(top, d);
            push(&shared, (Job){d, NULL, NULL, NULL});
            add_space(drives[i][0]);
        }
        atomic_store(&pending, ndrives);
    } else {
        push(&shared, (Job){top, NULL, NULL, NULL});
        atomic_store(&pending, 1);
        add_space(root[1]);
    }
    // shortcut: 8 threads whatever the core count, since a cold scan waits on the disk and NVMe drives like many
    // requests in flight; measure on real PCs (CI runners have throttled cloud disks) before tuning further
    long ncpu = MAX_THREADS;
#else
    push(&shared, (Job){top, NULL, NULL, NULL});
    atomic_store(&pending, 1);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    int nth = ncpu < 1 ? 1 : ncpu > MAX_THREADS ? MAX_THREADS : (int)ncpu;
    pthread_t th[MAX_THREADS];
    for (int i = 0; i < nth; i++) pthread_create(&th[i], NULL, worker, NULL);
    int tty = isatty(2) && !live;
    uint64_t last = 0;
    int still = 0, tick = 0;
    while (atomic_load(&pending)) {
        uint64_t now = atomic_load(&scanned);
        if (tty) fprintf(stderr, "\r%llu items…", (unsigned long long)now);
        if (live && tick++ % 3 == 0) progress_line();
        // No progress for 30 s: the remaining threads wait on macOS privacy prompts (or a dead disk), keep what we have
        if (now != last) { last = now; still = 0; }
        else if (++still >= 300) {
            atomic_store(&stopping, 1);
            usleep(200000);  // lets a thread in the middle of a directory see the flag before the tree is read
#ifdef __APPLE__
            fprintf(stderr, "\nScan stalled on a macOS permission prompt: partial results. "
                            "Give Full Disk Access to see everything.\n");
#else
            fprintf(stderr, "\nScan stalled on an unresponsive folder: partial results.\n");
#endif
            break;
        }
        usleep(100000);
    }
    if (!atomic_load(&stopping))
        for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    if (live) progress_line();
    if (nfresh && !atomic_load(&stopping)) save_cache();
    total(top);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

#ifdef _WIN32
    uint64_t disk = disk_total, avail = disk_free;
#else
    struct statvfs vfs;
    statvfs(root, &vfs);
    // Like df: blocks reserved for root (5% on ext4) are neither used nor free, so they're left out of the size
    uint64_t disk = (uint64_t)(vfs.f_blocks - vfs.f_bfree + vfs.f_bavail) * vfs.f_frsize, avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
#ifndef __APPLE__
    if (!strcmp(root, "/") && disk_total) disk = disk_total, avail = disk_free;
#endif
#endif
    char date[32];
    time_t now = time(NULL);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime(&now));

    // The history file is in the user's data folder, not with the caches: cleaning caches must not lose it. A scan
    // that stalled would chart a drop that never happened, and root (pkexec) must not write in the user's folder
    char hist_file[4300] = "";
    int writer = !atomic_load(&stopping);
    if (history) {
#ifdef _WIN32
        if (cache_file[0]) snprintf(hist_file, sizeof hist_file, "%s\\Evict\\history.jsonl", getenv("LOCALAPPDATA"));
#else
        const char *xdg = getenv("XDG_DATA_HOME");
        writer = writer && geteuid();
#ifdef __APPLE__
        const char *dirs[] = {"/Library", "/Library/Application Support", "/Library/Application Support/Evict", NULL};
        xdg = NULL;
#else
        const char *dirs[] = {"/.local", "/.local/share", "/.local/share/evict", NULL};
        if (xdg && xdg[0] == '/') dirs[0] = "/evict", dirs[1] = NULL;
        else xdg = NULL;
#endif
        const char *base = xdg ? xdg : home;
        for (int i = 0; base && dirs[i]; i++) {
            snprintf(hist_file, sizeof hist_file, "%s%s", base, dirs[i]);
            if (writer) mkdir(hist_file, 0700);
        }
        if (base) strcat(hist_file, "/history.jsonl");
#endif
    }
    Buf old = {0}, cur = {0}, kept = {0}, shown = {0};
    char **lines = NULL, *prev = NULL;
    size_t nlines = 0;
    if (hist_file[0]) {
        FILE *f = fopen(hist_file, "rb");
        char chunk[65536];
        for (size_t got; f && (got = fread(chunk, 1, sizeof chunk, f)) > 0; ) bput(&old, chunk, got);
        if (f) fclose(f);
        Buf prefix = {0};
        bputs(&prefix, "{\"r\":");
        bstr(&prefix, root);
        bputs(&prefix, ",");
        for (char *l = old.s, *end; l && *l; l = end ? end + 1 : l + strlen(l)) {
            if ((end = strchr(l, '\n'))) *end = 0;
            if (*l != '{') continue;
            lines = xrealloc(lines, (nlines + 2) * sizeof *lines);
            lines[nlines++] = l;
            if (!strncmp(l, prefix.s, prefix.n)) prev = l;
        }
        char num[160];
        int first = 1;
        bput(&cur, prefix.s, prefix.n - 1);
        snprintf(num, sizeof num, ",\"d\":\"%s\",\"total\":%llu,\"free\":%llu,\"size\":%llu,\"dirs\":[", date,
                 (unsigned long long)disk, (unsigned long long)avail, (unsigned long long)top->size);
        bputs(&cur, num);
        hist_dirs(&cur, top, root, &first);
        bputs(&cur, "]}");
        // The file again, this scan added; old lines keep only their totals
        lines = xrealloc(lines, (nlines + 1) * sizeof *lines);
        if (writer) lines[nlines++] = cur.s;
        size_t from = nlines > HIST_LINES ? nlines - HIST_LINES : 0;
        for (size_t i = from; i < nlines; i++) {
            char *dirs = i + HIST_DIRS < nlines ? strstr(lines[i], ",\"dirs\":[") : NULL;
            bput(&kept, lines[i], dirs ? (size_t)(dirs - lines[i]) : strlen(lines[i]));
            bputs(&kept, dirs ? "}\n" : "\n");
        }
        if (writer) {
            char tmp[4400];
            snprintf(tmp, sizeof tmp, "%s.tmp", hist_file);
            if ((f = fopen(tmp, "wb"))) {
                int ok = fwrite(kept.s, 1, kept.n, f) == kept.n;
                if (!fclose(f) && ok) {
                    remove(hist_file);  // Windows doesn't rename over a file
                    rename(tmp, hist_file);
                }
            }
        }
        if (!writer) lines[nlines++] = cur.s;  // still in the report, not in the file
        // The report gets this root's lines only
        for (size_t i = from; i < nlines; i++)
            if (!strncmp(lines[i], prefix.s, prefix.n)) {
                char *dirs = i + HIST_DIRS < nlines ? strstr(lines[i], ",\"dirs\":[") : NULL;
                bputs(&shown, shown.n ? "," : "");
                bput(&shown, lines[i], dirs ? (size_t)(dirs - lines[i]) : strlen(lines[i]));
                if (dirs) bputs(&shown, "}");
            }
    }
    if (alert_only) {
        alert(prev, cur.s ? cur.s : "", disk, avail, disk - avail, home);
        return 0;
    }

    printf("{\"os\":\"" OS "\",\"total\":%llu,\"free\":%llu,\"count\":%llu,\"secs\":%.1f,\"date\":\"%s\",\"home\":",
           (unsigned long long)disk, (unsigned long long)avail,
           (unsigned long long)atomic_load(&scanned), secs, date);
    emit_str(home ? home : "");
#ifdef _WIN32
    char *bin = home ? recycle_bin(home) : NULL;
    if (bin) fputs(",\"trash\":", stdout), emit_str(bin);
#endif
    fputs(",\"tree\":", stdout);
    emit(top);
    if (hist_file[0]) printf(",\"history\":[%s]", shown.n ? shown.s : "");
    puts("}");
    fprintf(stderr, "\r%llu items scanned in %.2f s (%d threads)\n",
            (unsigned long long)atomic_load(&scanned), secs, nth);
    return 0;
}
