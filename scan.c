// Fast parallel disk usage scanner for macOS. Prints a pruned JSON tree on stdout.
//
// Why it is fast:
// - getattrlistbulk(2): one syscall returns name + type + allocated size for a whole batch of
//   directory entries, so files are never opened or stat'ed one by one.
// - openat(parent_fd, name): no full path lookup, so threads don't fight over the same top-level
//   vnodes in the kernel.
// - each thread walks depth-first on its own stack and only hands work out when a thread is idle.
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/vnode.h>
#include <time.h>
#include <unistd.h>

#define MIN_SIZE 1000000ULL  // smaller items are merged into one "others" entry
#define MAX_KIDS 300
#define BUF_SIZE (128 * 1024)
#define MAX_THREADS 8  // measured fastest; more threads only add APFS lock contention in the kernel

typedef struct Node {
    char *name;
    char *path;  // only set on ancestors of skipped mount points
    uint64_t size, small;
    struct Node **kids;
    uint32_t nkids, cap, nsmall;
    uint8_t dir, err;
} Node;

// An open directory kept alive until all its subdirectories have been opened from it
typedef struct Dir {
    int fd;
    _Atomic int refs;
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

static char *skips[64];
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
        close(d->fd);
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
    for (int i = 0; i < n && nskips < 62; i++) {
        const char *on = m[i].f_mntonname, *from = m[i].f_mntfromname;
        if (!strcmp(on, "/") || !strcmp(on, root)) continue;
        int same = cl > 9 && !strncmp(from, container, cl) && from[cl] == 's';
        if (!same || !strcmp(on, "/System/Volumes/Data") || !strcmp(on, "/System/Volumes/Update/mnt1"))
            skips[nskips++] = strdup(on);
    }
    skips[nskips++] = "/Volumes";
}

// 0 = nothing to do, 1 = skip this path, 2 = keep its path (a skipped mount point lies below)
static int skip_state(const char *path) {
    size_t l = strlen(path);
    int below = 0;
    for (int i = 0; i < nskips; i++) {
        if (!strcmp(path, skips[i])) return 1;
        if (!strncmp(skips[i], path, l) && (skips[i][l] == '/' || l == 1)) below = 1;
    }
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

static void scan_dir(Job j, char *buf, Stack *local) {
    Node *dir = j.node, *sum = j.sum ? j.sum : dir && !dir->path && is_deps(dir->name) ? dir : NULL;
    int fd = j.parent ? openat(j.parent->fd, dir ? dir->name : j.name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)
                      : open(root, O_RDONLY | O_DIRECTORY);
    free(j.name);
    // The tree is being printed: a thread released late by a privacy prompt must not touch it
    if (atomic_load(&stopping)) { if (fd >= 0) close(fd); return; }
    if (j.parent) release(j.parent);
    Node *owner = dir ? dir : sum;  // a folder inside a dependency folder reports its errors on that folder
    if (fd < 0) { __atomic_store_n(&owner->err, 1, __ATOMIC_RELAXED); return; }
    Dir *me = malloc(sizeof *me);
    if (!me) { perror("malloc"); exit(1); }
    me->fd = fd;
    atomic_init(&me->refs, 1);

    struct attrlist al = {
        .bitmapcount = ATTR_BIT_MAP_COUNT,
        .commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_DEVID |
                      ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID,
        .fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE,
    };
    uint64_t count = 0, bytes = 0, deep = 0, ndeep = 0;
    size_t first = local->n;
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
            count++;
            if (type == VDIR && sum) {
                char *copy = malloc(nlen + 1);
                if (!copy) { perror("malloc"); exit(1); }
                memcpy(copy, name, nlen + 1);
                push(local, (Job){NULL, me, sum, copy});
            } else if (type == VDIR) {
                char *path = NULL;
                if (dir->path) {
                    size_t pl = strlen(dir->path) == 1 ? 0 : strlen(dir->path);
                    path = malloc(pl + nlen + 2);
                    if (!path) { perror("malloc"); exit(1); }
                    memcpy(path, dir->path, pl);
                    path[pl] = '/';
                    memcpy(path + pl + 1, name, nlen + 1);
                    int s = skip_state(path);
                    if (s == 1) { free(path); continue; }
                    if (s == 0) { free(path); path = NULL; }
                }
                Node *k = new_node(name, nlen, 1);
                k->path = path;
                add_kid(dir, k);
                push(local, (Job){k, me, NULL, NULL});
            } else {
                uint32_t nlink = 1;
                uint64_t alloc = 0;
                if (ret.fileattr & ATTR_FILE_LINKCOUNT) { memcpy(&nlink, cur, 4); cur += 4; }
                if (ret.fileattr & ATTR_FILE_ALLOCSIZE) { memcpy(&alloc, cur, 8); cur += 8; }
                if (nlink > 1 && link_seen(fileid ^ ((uint64_t)(uint32_t)dev << 48))) continue;
                bytes += alloc;
                if (sum) {
                    deep += alloc;
                    ndeep++;
                } else if (alloc >= MIN_SIZE) {
                    Node *k = new_node(name, nlen, 0);
                    k->size = alloc;
                    add_kid(dir, k);
                } else {
                    dir->small += alloc;
                    dir->nsmall++;
                }
            }
        }
        if (atomic_load_explicit(&stopping, memory_order_relaxed)) break;
    }
    // Several threads add up the same dependency folder
    if (sum) {
        __atomic_fetch_add(&sum->small, deep, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sum->nsmall, (uint32_t)ndeep, __ATOMIC_RELAXED);
    }
    size_t added = local->n - first;
    atomic_fetch_add_explicit(&me->refs, (int)added, memory_order_relaxed);
    atomic_fetch_add_explicit(&pending, (long)added, memory_order_relaxed);
    release(me);
    atomic_fetch_add_explicit(&scanned, count, memory_order_relaxed);
    if (live) atomic_fetch_add_explicit(&counted, bytes, memory_order_relaxed);
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

int main(int argc, char **argv) {
    static char rootbuf[4096];
    snprintf(rootbuf, sizeof rootbuf, "%s", argc > 1 ? argv[1] : "/");
    for (size_t l = strlen(rootbuf); l > 1 && rootbuf[l - 1] == '/'; l--) rootbuf[l - 1] = 0;
    root = rootbuf;

    // Never download iCloud/Drive placeholder files while scanning
    setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_PROCESS,
                   IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
    // Parents stay open while their subdirectories wait in a stack
    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) {
        rl.rlim_cur = rl.rlim_max < 1 << 20 ? rl.rlim_max : 1 << 20;
        if (setrlimit(RLIMIT_NOFILE, &rl)) { rl.rlim_cur = OPEN_MAX; setrlimit(RLIMIT_NOFILE, &rl); }
    }
    init_skips();
    static char out[1 << 20];
    setvbuf(stdout, out, _IOFBF, sizeof out);
    live = getenv("EVICT_PROGRESS") != NULL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    Node *top = new_node(root, strlen(root), 1);
    if (skip_state(root) == 2) top->path = strdup(root);
    push(&shared, (Job){top, NULL, NULL, NULL});
    atomic_store(&pending, 1);

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
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
        // No progress for 30 s means the remaining threads wait on macOS privacy prompts: keep what we have
        if (now != last) { last = now; still = 0; }
        else if (++still >= 300) {
            atomic_store(&stopping, 1);
            usleep(200000);  // lets a thread in the middle of a directory see the flag before the tree is read
            fprintf(stderr, "\nScan stalled on a macOS permission prompt: partial results. "
                            "Give Full Disk Access to see everything.\n");
            break;
        }
        usleep(100000);
    }
    if (!atomic_load(&stopping))
        for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    if (live) progress_line();
    total(top);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    struct statvfs vfs;
    statvfs(root, &vfs);
    char date[32];
    time_t now = time(NULL);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime(&now));
    const char *home = getenv("HOME");

    printf("{\"total\":%llu,\"free\":%llu,\"count\":%llu,\"secs\":%.1f,\"date\":\"%s\",\"home\":",
           (unsigned long long)vfs.f_blocks * vfs.f_frsize, (unsigned long long)vfs.f_bavail * vfs.f_frsize,
           (unsigned long long)atomic_load(&scanned), secs, date);
    emit_str(home ? home : "");
    fputs(",\"tree\":", stdout);
    emit(top);
    puts("}");
    fprintf(stderr, "\r%llu items scanned in %.2f s (%d threads)\n",
            (unsigned long long)atomic_load(&scanned), secs, nth);
    return 0;
}
