// Evict for Windows: the shared page (index.html) in a WebView2 window, with a native bridge for what a web page
// can't do: Recycle Bin and deletion, Explorer, previews, archives, duplicates and Claude Code answers.
// A script injected at document start gives the page the bridge object it uses on macOS and Linux
// (window.webkit.messageHandlers.evict), forwarding to WebView2's chrome.webview.postMessage.
// Report paths look like POSIX ones (/C:/Users/me) so the page runs unchanged; they're converted at the edges.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <urlmon.h>
#include <wchar.h>
#include "WebView2.h"

#ifndef VERSION
#define VERSION "0"
#endif
#define WIDE2(s) L##s
#define WIDE(s) WIDE2(s)
#define PLACEHOLDER "window.DATA = null; /* __DATA__ */"
#define RELEASES L"https://api.github.com/repos/vassilidev/evict/releases?per_page=10"
#define DOWNLOADS L"https://vassilidev.github.io/evict/download.html"
#define RUNTIME L"https://go.microsoft.com/fwlink/p/?LinkId=2124703"  // Microsoft's WebView2 Runtime installer
#define SETUP "Evict-Setup.exe"
#define BG RGB(12, 14, 18)  // the page's background, #0c0e12
#define LONG_PATH 32768

enum { WM_JS = WM_APP, WM_SCANNED, WM_CHECKED, WM_FETCHED };
enum { B_MAIN = 100, B_DELETE, B_INSTALL };

static HWND win;
static ICoreWebView2Controller *ctrl;
static ICoreWebView2 *web;
static WCHAR dir[MAX_PATH], data[MAX_PATH], home[MAX_PATH], scanner[MAX_PATH], claude[MAX_PATH], *trash, *smoke;
static BOOL scanning, admin_scan, has_page;
static HANDLE job, scan_job;  // the running Claude Code or tar, and the scanner: closing a job kills its processes
static volatile LONG gen;     // bumped when a request replaces the previous one, whose results are then dropped
static BCRYPT_ALG_HANDLE sha;

// ---- Strings

typedef struct { WCHAR *s; size_t n, cap; } Str;

static void add(Str *b, const WCHAR *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        if (!(b->s = realloc(b->s, b->cap * sizeof *b->s))) abort();
    }
    memcpy(b->s + b->n, s, n * sizeof *s);
    b->n += n;
    b->s[b->n] = 0;
}

static void adds(Str *b, const WCHAR *s) { add(b, s, wcslen(s)); }

static void addf(Str *b, const WCHAR *fmt, ...) {
    WCHAR tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(tmp, 2047, fmt, ap);
    va_end(ap);
    tmp[2047] = 0;
    adds(b, tmp);
}

// A string as a JavaScript string literal
static void add_js(Str *b, const WCHAR *s) {
    add(b, L"\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') add(b, L"\\", 1);
        if (*s < 0x20 || *s == 0x2028 || *s == 0x2029) addf(b, L"\\u%04x", *s);
        else add(b, s, 1);
    }
    add(b, L"\"", 1);
}

static WCHAR *wide(const char *s, int n, UINT cp) {
    int len = MultiByteToWideChar(cp, 0, s, n, NULL, 0);
    WCHAR *w = calloc(len + 1, sizeof *w);
    if (!w) abort();
    MultiByteToWideChar(cp, 0, s, n, w, len);
    return w;
}

static char *utf8(const WCHAR *w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = calloc(len + 1, 1);
    if (!s) abort();
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL);
    return s;
}

// Decimal units like the page, always in English
static void add_bytes(Str *b, double n) {
    static const WCHAR *units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    int i = 0;
    while (n >= 1000 && i < 4) n /= 1000, i++;
    addf(b, i && n < 10 ? L"%.1f %ls" : L"%.0f %ls", n, units[i]);
}

// ---- Files

static char *read_file(const WCHAR *path, DWORD *len) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    char *s = size == INVALID_FILE_SIZE ? NULL : malloc(size + 1);
    if (s && ReadFile(f, s, size, &got, NULL)) s[got] = 0;
    else free(s), s = NULL;
    CloseHandle(f);
    if (len) *len = got;
    return s;
}

static BOOL write_file(const WCHAR *path, const void *bytes, DWORD len) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD done = 0;
    BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, bytes, len, &done, NULL) && done == len;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok;
}

static void in_data(WCHAR *out, const WCHAR *name) { swprintf(out, MAX_PATH, L"%ls\\%ls", data, name); }

// ---- Paths: /C:/Users/me in the report, C:\Users\me for Windows

static WCHAR *native(const WCHAR *p) {
    size_t n = wcslen(p);
    WCHAR *w = calloc(n + 2, sizeof *w);
    if (!w) abort();
    for (size_t i = 1; i <= n; i++) w[i - 1] = p[i] == '/' ? '\\' : p[i];
    if (n == 3) w[2] = '\\';  // a drive alone: C:\ */
    return w;
}

static WCHAR *report(const WCHAR *w) {
    Str b = {0};
    add(&b, L"/", 1);
    for (; *w; w++) add(&b, *w == '\\' ? L"/" : w, 1);
    while (b.n > 1 && b.s[b.n - 1] == '/') b.s[--b.n] = 0;
    return b.s;
}

// \\?\ in front: Windows APIs then take paths longer than 260 characters (deep node_modules)
static void long_path(WCHAR *out, const WCHAR *report_path) {
    WCHAR *n = native(report_path);
    swprintf(out, LONG_PATH, L"\\\\?\\%ls", n);
    free(n);
}

static size_t comp(const WCHAR *p) { return wcscspn(p, L"/"); }

static BOOL named(const WCHAR *p, size_t n, const WCHAR *name) { return wcslen(name) == n && !_wcsnicmp(p, name, n); }

static BOOL listed(const WCHAR *p, const WCHAR *const *list, BOOL exact) {
    for (; *list; list++) {
        size_t l = wcslen(*list);
        if (!_wcsnicmp(p, *list, l) && (!p[l] || (!exact && p[l] == '/'))) return TRUE;
    }
    return FALSE;
}

// Same rules as canRm() in index.html: never a drive or its top-level folders, system folders, a home folder and its
// AppData, a whole Recycle Bin, or where keys and passwords live
static BOOL can_remove(const WCHAR *p) {
    static const WCHAR *const sys[] = {L"Windows", L"Windows.old", L"System Volume Information", L"Recovery", L"Boot", L"EFI",
                                       L"PerfLogs", L"Config.Msi", L"pagefile.sys", L"hiberfil.sys", L"swapfile.sys", NULL};
    static const WCHAR *const homes[] = {L"/AppData", L"/AppData/Local", L"/AppData/Roaming", L"/AppData/LocalLow",
                                         L"/AppData/Local/Temp", NULL};
    static const WCHAR *const secrets[] = {L"/.ssh", L"/.gnupg", L"/.aws", L"/.azure", L"/.kube", L"/AppData/Roaming/gnupg",
                                           L"/AppData/Roaming/Microsoft/Credentials", L"/AppData/Roaming/Microsoft/Protect",
                                           L"/AppData/Roaming/Microsoft/Crypto", L"/AppData/Roaming/Microsoft/SystemCertificates",
                                           L"/AppData/Local/Microsoft/Credentials", L"/AppData/Local/Microsoft/Vault", NULL};
    size_t len = wcslen(p), depth = 0;
    if (len < 5 || p[0] != '/' || p[2] != ':' || p[3] != '/' || p[len - 1] == '/' || wcsstr(p, L"//") ||
        wcsstr(p, L"/./") || wcsstr(p, L"/../") || !wcscmp(p + len - 2, L"/.") || !wcscmp(p + len - 3, L"/.."))
        return FALSE;
    for (const WCHAR *c = p; *c; c++) depth += *c == '/';
    const WCHAR *top = p + 4;
    size_t n = comp(top);
    if (depth < 3 || (top[0] == '$' && !named(top, n, L"$Recycle.Bin"))) return FALSE;
    for (int i = 0; sys[i]; i++)
        if (named(top, n, sys[i])) return FALSE;
    if (named(top, n, L"$Recycle.Bin") && depth == 3) return FALSE;
    if (named(top, n, L"Users")) {
        const WCHAR *user = top + n + 1, *rest = user + comp(user);
        if (!*rest || listed(rest, homes, TRUE) || listed(rest, secrets, FALSE)) return FALSE;
    }
    return TRUE;
}

// Inside a Recycle Bin: /C:/$Recycle.Bin/<SID>/…
static BOOL in_trash(const WCHAR *p) {
    if (wcslen(p) < 5 || p[3] != '/' || !named(p + 4, comp(p + 4), L"$Recycle.Bin")) return FALSE;
    const WCHAR *sid = p + 4 + comp(p + 4);
    return *sid && sid[1 + comp(sid + 1)] == '/';
}

// ---- Talking to the page

static void run_js(Str *js) {
    if (web) ICoreWebView2_ExecuteScript(web, js->s, NULL);
    free(js->s);
}

// From a worker thread: the UI thread runs it
static void post_js(Str *js) {
    if (!PostMessageW(win, WM_JS, 0, (LPARAM)js->s)) free(js->s);
}

static void answer(int id, const WCHAR *text, const WCHAR *kind) {
    Str js = {0};
    addf(&js, L"evictAnswer(%d, ", id);
    add_js(&js, text);
    addf(&js, L", '%ls')", kind);
    run_js(&js);
}

static void removed(WCHAR **paths, int n, BOOL freed) {
    if (!n) return;
    Str js = {0};
    adds(&js, L"evictRemoved([");
    for (int i = 0; i < n; i++) {
        if (i) add(&js, L",", 1);
        add_js(&js, paths[i]);
    }
    addf(&js, L"], %ls)", freed ? L"true" : L"false");
    run_js(&js);
}

// ---- Messages from the page: a small JSON reader

typedef struct J {
    char t;  // o(bject), a(rray), s(tring), n(umber), x (true, false, null)
    int n;
    double d;
    WCHAR *s, **k;
    struct J *v;
} J;

static const WCHAR *skip_ws(const WCHAR *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const WCHAR *parse(const WCHAR *p, J *out) {
    memset(out, 0, sizeof *out);
    p = skip_ws(p);
    if (*p == '{' || *p == '[') {
        WCHAR end = *p == '{' ? '}' : ']';
        out->t = *p == '{' ? 'o' : 'a';
        for (p = skip_ws(p + 1); *p && *p != end;) {
            if (!(out->v = realloc(out->v, (out->n + 1) * sizeof *out->v))) abort();
            if (out->t == 'o') {
                J key;
                if (!(out->k = realloc(out->k, (out->n + 1) * sizeof *out->k))) abort();
                if (!(p = parse(p, &key)) || key.t != 's') return NULL;
                out->k[out->n] = key.s;
                p = skip_ws(p);
                if (*p++ != ':') return NULL;
            }
            if (!(p = parse(p, &out->v[out->n++]))) return NULL;
            p = skip_ws(p);
            if (*p == ',') p = skip_ws(p + 1);
        }
        return *p ? p + 1 : NULL;
    }
    if (*p == '"') {
        Str b = {0};
        add(&b, L"", 0);
        for (p++; *p && *p != '"'; p++) {
            WCHAR c = *p;
            if (c == '\\' && p[1]) {
                c = *++p;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c == 'r') c = '\r';
                else if (c == 'b') c = '\b';
                else if (c == 'f') c = '\f';
                else if (c == 'u' && wcslen(p) > 4) {
                    WCHAR hex[5] = {p[1], p[2], p[3], p[4], 0};
                    c = (WCHAR)wcstoul(hex, NULL, 16);
                    p += 4;
                }
            }
            add(&b, &c, 1);
        }
        out->t = 's';
        out->s = b.s;
        return *p ? p + 1 : NULL;
    }
    WCHAR *end;
    out->t = 'n';
    out->d = wcstod(p, &end);
    if (end == p) {
        out->t = 'x';
        while (iswalpha(*end)) end++;
    }
    return end == p ? NULL : end;
}

static void free_j(J *j) {
    for (int i = 0; i < j->n; i++) {
        free_j(&j->v[i]);
        if (j->k) free(j->k[i]);
    }
    free(j->v), free(j->k), free(j->s);
}

static J *get(J *o, const WCHAR *key) {
    for (int i = 0; o && o->t == 'o' && i < o->n; i++)
        if (!wcscmp(o->k[i], key)) return &o->v[i];
    return NULL;
}

static WCHAR *get_str(J *o, const WCHAR *key) {
    J *v = get(o, key);
    return v && v->t == 's' ? v->s : NULL;
}

static double get_num(J *o, const WCHAR *key) {
    J *v = get(o, key);
    return v && v->t == 'n' ? v->d : 0;
}

// ---- Dialogs

// Buttons are (label, id) pairs ending with NULL; the last one is the default
static int dialog(PCWSTR icon, const WCHAR *title, const WCHAR *text, ...) {
    TASKDIALOG_BUTTON b[4];
    int n = 0, r = IDCANCEL;
    va_list ap;
    va_start(ap, text);
    for (const WCHAR *label; n < 4 && (label = va_arg(ap, const WCHAR *));) {
        b[n].pszButtonText = label;
        b[n++].nButtonID = va_arg(ap, int);
    }
    va_end(ap);
    TASKDIALOGCONFIG c = {sizeof c};
    c.hwndParent = win;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    c.pszWindowTitle = L"Evict";
    c.pszMainIcon = icon;
    c.pszMainInstruction = title;
    c.pszContent = text;
    c.cButtons = n;
    c.pButtons = b;
    c.nDefaultButton = n ? b[n - 1].nButtonID : IDOK;
    if (FAILED(TaskDialogIndirect(&c, &r, NULL, NULL))) r = IDCANCEL;
    return r;
}

static void inform(const WCHAR *title, const WCHAR *text) { dialog(TD_INFORMATION_ICON, title, text, L"OK", IDOK, NULL); }

static void error_text(Str *b, DWORD code) {
    WCHAR *msg = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                   (WCHAR *)&msg, 0, NULL);
    if (msg) {
        StrTrimW(msg, L" \r\n");
        adds(b, msg);
        LocalFree(msg);
    } else addf(b, L"Error %lu.", code);
}

// ---- Deletion

// Deletes a file, or a folder and everything inside; junctions and links are removed, never followed
static BOOL rm_tree(WCHAR *path, size_t len) {
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path);
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT) && len + 3 < LONG_PATH) {
        WIN32_FIND_DATAW f;
        wcscpy(path + len, L"\\*");
        HANDLE h = FindFirstFileExW(path, FindExInfoBasic, &f, FindExSearchNameMatch, NULL, 0);
        path[len] = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                size_t l = wcslen(f.cFileName);
                if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..") || len + l + 2 >= LONG_PATH) continue;
                path[len] = '\\';
                wcscpy(path + len + 1, f.cFileName);
                rm_tree(path, len + 1 + l);
                path[len] = 0;
            } while (FindNextFileW(h, &f));
            FindClose(h);
        }
    }
    return RemoveDirectoryW(path);
}

static BOOL rm_report(const WCHAR *p, DWORD *err) {
    WCHAR *buf = malloc(LONG_PATH * sizeof *buf);
    if (!buf) abort();
    long_path(buf, p);
    BOOL ok = rm_tree(buf, wcslen(buf));
    if (!ok) *err = GetLastError();
    // The Recycle Bin keeps each item's name and origin in a $I file next to its $R file
    const WCHAR *name = wcsrchr(p, '/') + 1;
    if (ok && in_trash(p) && name[0] == '$' && name[1] == 'R') {
        WCHAR *slash = wcsrchr(buf, '\\');
        slash[2] = 'I';
        DeleteFileW(buf);
    }
    free(buf);
    return ok;
}

// 1: in the Recycle Bin, 0: failed, -1: the user kept it (Windows asked to delete it for good, too big to recycle)
static int recycle(const WCHAR *p) {
    WCHAR *n = native(p);
    size_t len = wcslen(n);
    WCHAR *from = calloc(len + 2, sizeof *from);  // a list ending with two NULs
    if (!from) abort();
    wcscpy(from, n);
    SHFILEOPSTRUCTW op = {win, FO_DELETE, from, NULL, FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_NOERRORUI | FOF_SILENT};
    int r = SHFileOperationW(&op), done = op.fAnyOperationsAborted ? -1 : !r && GetFileAttributesW(n) == INVALID_FILE_ATTRIBUTES;
    free(from), free(n);
    return done;
}

static BOOL is_secret(const WCHAR *name) {
    static const WCHAR *const exact[] = {L"id_rsa", L"id_dsa", L"id_ecdsa", L"id_ed25519", L".npmrc", L".netrc", L".pgpass",
                                         L"credentials", L"credentials.json", NULL};
    static const WCHAR *const exts[] = {L".pem", L".key", L".p12", L".pfx", L".keystore", L".jks", L".kdbx", L".ovpn", NULL};
    const WCHAR *ext = PathFindExtensionW(name);
    if (!wcscmp(ext, L".example") || !wcscmp(ext, L".sample") || !wcscmp(ext, L".template")) return FALSE;
    for (int i = 0; exact[i]; i++)
        if (!_wcsicmp(name, exact[i])) return TRUE;
    if (!_wcsnicmp(name, L".env", 4) && (!name[4] || (name[4] == '.' && name[5]))) return TRUE;
    for (int i = 0; exts[i]; i++)
        if (ext != name && !_wcsicmp(ext, exts[i])) return TRUE;
    return FALSE;
}

static BOOL heavy(const WCHAR *name) {
    static const WCHAR *const names[] = {L"node_modules", L".git", L"vendor", L"Pods", L"DerivedData", L".venv", L"venv",
                                         L"__pycache__", L".next", L"target", NULL};
    for (int i = 0; names[i]; i++)
        if (!wcscmp(name, names[i])) return TRUE;
    return FALSE;
}

// Secrets (.env, private keys, credentials) in what's about to be deleted. A quick look: heavy dependency
// folders are skipped and the walk stops after a second, so a huge folder can hide some
typedef struct { Str text; int n; ULONGLONG deadline; } Secrets;

static void found_secret(Secrets *s, const WCHAR *path) {
    if (s->n++ >= 6) return;
    const WCHAR *p = path + 4;  // past \\?\ */
    size_t h = wcslen(home);
    if (!_wcsnicmp(p, home, h) && p[h] == '\\') addf(&s->text, L"\n• ~%ls", p + h);
    else addf(&s->text, L"\n• %ls", p);
}

static void walk_secrets(WCHAR *path, size_t len, Secrets *s) {
    WIN32_FIND_DATAW f;
    if (len + 3 >= LONG_PATH) return;
    wcscpy(path + len, L"\\*");
    HANDLE h = FindFirstFileExW(path, FindExInfoBasic, &f, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    path[len] = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        size_t l = wcslen(f.cFileName);
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..") || len + l + 2 >= LONG_PATH) continue;
        path[len] = '\\';
        wcscpy(path + len + 1, f.cFileName);
        if (is_secret(f.cFileName)) found_secret(s, path);
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !heavy(f.cFileName))
            walk_secrets(path, len + 1 + l, s);
        path[len] = 0;
    } while (s->n < 50 && GetTickCount64() < s->deadline && FindNextFileW(h, &f));
    FindClose(h);
}

// Files of another user or of the system: Evict starts itself as administrator (UAC) to delete them for good
static void offer_admin(WCHAR **paths, int n, Str *reason) {
    Str text = {0};
    addf(&text, L"%ls\n\nDelete %ls permanently as administrator?", reason->s ? reason->s : L"Access is denied.",
         n > 1 ? L"these items" : L"it");
    int r = dialog(TD_WARNING_ICON, L"Permission denied", text.s, L"Cancel", IDCANCEL, L"Delete as Administrator", B_DELETE, NULL);
    free(text.s);
    if (r != B_DELETE) return;
    Str list = {0};
    for (int i = 0; i < n; i++) {
        WCHAR *w = native(paths[i]);
        addf(&list, L"%ls\n", w);
        free(w);
    }
    WCHAR file[MAX_PATH], exe[MAX_PATH], args[MAX_PATH + 16];
    in_data(file, L"delete.txt");
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(args, MAX_PATH + 16, L"--delete \"%ls\"", file);
    BOOL ok = write_file(file, list.s, (DWORD)(list.n * sizeof(WCHAR)));
    free(list.s);
    SHELLEXECUTEINFOW sei = {sizeof sei, SEE_MASK_NOCLOSEPROCESS, win, L"runas", exe, args, NULL, SW_HIDE};
    if (!ok || !ShellExecuteExW(&sei)) return;  // the UAC prompt was dismissed
    // Keeps the window drawn while the elevated copy works
    while (MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) TranslateMessage(&m), DispatchMessageW(&m);
    }
    CloseHandle(sei.hProcess);
    DeleteFileW(file);
    WCHAR **gone = calloc(n, sizeof *gone), buf[LONG_PATH];
    int ngone = 0;
    for (int i = 0; i < n; i++) {
        long_path(buf, paths[i]);
        if (GetFileAttributesW(buf) == INVALID_FILE_ATTRIBUTES) gone[ngone++] = paths[i];
    }
    removed(gone, ngone, TRUE);
    if (ngone < n) inform(L"Couldn’t delete everything", L"Some items are in use by another app. Close it and try again.");
    free(gone);
}

// The elevated copy: Evict.exe --delete <list>, one Windows path per line
static int delete_listed(const WCHAR *file) {
    DWORD len;
    char *bytes = read_file(file, &len);
    if (!bytes) return 1;
    WCHAR *list = calloc(len / 2 + 1, sizeof *list), *buf = malloc(LONG_PATH * sizeof *buf);
    if (!list || !buf) return 1;
    memcpy(list, bytes, len / 2 * 2);
    int failed = 0;
    for (WCHAR *line = list, *end; *line; line = end + 1) {
        if (!(end = wcschr(line, '\n'))) end = line + wcslen(line);
        WCHAR keep = *end;
        *end = 0;
        if (wcslen(line) > 3) {
            swprintf(buf, LONG_PATH, L"\\\\?\\%ls", line);
            failed += !rm_tree(buf, wcslen(buf));
        }
        if (!keep) break;
    }
    return failed;
}

// One or many items: everything goes to the Recycle Bin, or everything is deleted for good (items already in the
// Recycle Bin always are)
static void confirm_delete(WCHAR **all, double *sizes, int count) {
    WCHAR **items = calloc(count + 1, sizeof *items);
    int n = 0;
    double total = 0;
    BOOL all_in_trash = TRUE;
    for (int i = 0; i < count; i++)
        if (can_remove(all[i])) {
            items[n++] = all[i];
            total += sizes[i];
            all_in_trash &= in_trash(all[i]);
        }
    if (!n) return free(items);
    Str title = {0}, text = {0};
    if (n == 1) {
        WCHAR *path = native(items[0]);
        addf(&title, L"Delete “%ls”?", PathFindFileNameW(path));
        add_bytes(&text, total);
        addf(&text, L" · %ls\n\n", path);
        free(path);
    } else {
        addf(&title, L"Delete %d items?", n);
        add_bytes(&text, total);
        adds(&text, L" in total\n\n");
        for (int i = 0; i < n && i < 8; i++) addf(&text, L"• %ls\n", wcsrchr(items[i], '/') + 1);
        if (n > 8) addf(&text, L"• and %d more\n", n - 8);
        adds(&text, L"\n");
    }
    adds(&text, all_in_trash ? L"Already in the Recycle Bin: deleted permanently."
                : n > 1      ? L"The Recycle Bin keeps them recoverable, but the space is only freed once it’s emptied."
                             : L"The Recycle Bin keeps it recoverable, but the space is only freed once it’s emptied.");
    Secrets s = {{0}, 0, GetTickCount64() + 1000};
    WCHAR *buf = malloc(LONG_PATH * sizeof *buf);
    if (!buf) abort();
    for (int i = 0; i < n && s.n < 50 && GetTickCount64() < s.deadline; i++) {
        long_path(buf, items[i]);
        if (is_secret(PathFindFileNameW(buf))) found_secret(&s, buf);
        walk_secrets(buf, wcslen(buf), &s);
    }
    if (s.n) {
        adds(&text, L"\n\n⚠️ Secrets inside, often in no git repository or backup:");
        adds(&text, s.text.s);
        if (s.n > 6) adds(&text, L"\n• and more");
    }
    free(s.text.s);
    int r = all_in_trash ? dialog(TD_WARNING_ICON, title.s, text.s, L"Cancel", IDCANCEL, L"Delete Permanently", B_DELETE, NULL)
                         : dialog(TD_WARNING_ICON, title.s, text.s, L"Cancel", IDCANCEL, L"Delete Permanently", B_DELETE,
                                  L"Move to Recycle Bin", B_MAIN, NULL);
    free(title.s), free(text.s), free(buf);
    if (r == B_MAIN || r == B_DELETE) {
        WCHAR **trashed = calloc(n, sizeof *trashed), **deleted = calloc(n, sizeof *deleted), **failed = calloc(n, sizeof *failed);
        int nt = 0, nd = 0, nf = 0;
        DWORD err = 0;
        for (int i = 0; i < n; i++) {
            if (r == B_DELETE || in_trash(items[i])) {
                if (rm_report(items[i], &err)) deleted[nd++] = items[i];
                else failed[nf++] = items[i];
                continue;
            }
            int done = recycle(items[i]);
            if (done > 0) trashed[nt++] = items[i];
            else if (!done) failed[nf++] = items[i], err = ERROR_ACCESS_DENIED;
        }
        removed(trashed, nt, FALSE);
        removed(deleted, nd, TRUE);
        if (nf) {
            Str reason = {0};
            error_text(&reason, err);
            offer_admin(failed, nf, &reason);
            free(reason.s);
        }
        free(trashed), free(deleted), free(failed);
    }
    free(items);
}

// Empties the Recycle Bin of that drive; the page can only ask for the user's own Recycle Bin
static void confirm_empty_trash(const WCHAR *path, double size) {
    if (!trash || _wcsicmp(path, trash)) return;
    WCHAR *folder = native(path), pattern[MAX_PATH + 8], drive[] = {path[1], ':', '\\', 0};
    swprintf(pattern, MAX_PATH + 8, L"%ls\\$R*", folder);
    free(folder);
    int n = 0;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &f, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do n++;
        while (FindNextFileW(h, &f));
        FindClose(h);
    }
    if (!n) return inform(L"The Recycle Bin is already empty", L"Rescan to update the map.");
    Str text = {0};
    addf(&text, L"%d item%ls, ", n, n > 1 ? L"s" : L"");
    add_bytes(&text, size);
    adds(&text, L", will be deleted permanently. This can’t be undone.");
    int r = dialog(TD_WARNING_ICON, L"Empty the Recycle Bin?", text.s, L"Cancel", IDCANCEL, L"Empty Recycle Bin", B_DELETE, NULL);
    free(text.s);
    if (r != B_DELETE) return;
    if (FAILED(SHEmptyRecycleBinW(win, drive, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND)))
        return inform(L"Couldn’t empty the Recycle Bin", L"Empty it from its icon on the desktop.");
    Str js = {0};
    adds(&js, L"evictEmptied(");
    add_js(&js, path);
    adds(&js, L")");
    run_js(&js);
}

// ---- Files, previews, clipboard

static void copy_text(const WCHAR *text) {
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!g || !OpenClipboard(win)) { if (g) GlobalFree(g); return; }
    memcpy(GlobalLock(g), text, bytes);
    GlobalUnlock(g);
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    CloseClipboard();
}

static void open_native(const WCHAR *path) {
    if ((INT_PTR)ShellExecuteW(win, NULL, path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        Str b = {0};
        error_text(&b, GetLastError());
        inform(L"Couldn’t open it", b.s);
        free(b.s);
    }
}

static void open_report(const WCHAR *p) {
    WCHAR *n = native(p);
    open_native(!wcscmp(p, L"/") ? L"shell:MyComputerFolder" : n);
    free(n);
}

// C:\Users\me → C:\Users, C:\x → C:\ */
static WCHAR *parent_of(const WCHAR *p) {
    WCHAR *n = native(p);
    *wcsrchr(n, '\\') = 0;
    if (wcslen(n) == 2) wcscat(n, L"\\");
    return n;
}

// Explorer with the items selected, one window per folder
static void reveal(WCHAR **paths, int n) {
    BOOL *done = calloc(n + 1, sizeof *done);
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        if (wcslen(paths[i]) <= 3) {  // "/" or a drive: just open it
            open_report(paths[i]);
            continue;
        }
        WCHAR *parent = parent_of(paths[i]);
        PIDLIST_ABSOLUTE folder = ILCreateFromPathW(parent), *full = calloc(n, sizeof *full);
        LPCITEMIDLIST *kids = calloc(n, sizeof *kids);
        int k = 0;
        for (int j = i; j < n; j++) {
            if (done[j] || wcslen(paths[j]) <= 3) continue;
            WCHAR *item = native(paths[j]), *up = parent_of(paths[j]);
            if (!_wcsicmp(up, parent)) {
                done[j] = TRUE;
                if ((full[k] = ILCreateFromPathW(item))) kids[k] = ILFindLastID(full[k]), k++;
            }
            free(item), free(up);
        }
        if (!folder || !k || FAILED(SHOpenFolderAndSelectItems(folder, k, kids, 0))) open_native(parent);
        for (int j = 0; j < k; j++) ILFree(full[j]);
        if (folder) ILFree(folder);
        free(full), free(kids), free(parent);
    }
    free(done);
}

// Space: QuickLook (QL-Win) when it's installed, otherwise the default app.
// shortcut: only the first item of a selection is shown, to avoid opening dozens of apps; QuickLook shows one at a time anyway
static void preview(WCHAR **paths, int n) {
    if (!n) return;
    WCHAR *item = native(paths[0]), ql[MAX_PATH] = L"";
    const WCHAR *roots[] = {_wgetenv(L"LOCALAPPDATA"), _wgetenv(L"ProgramFiles")};
    for (int i = 0; i < 2 && !ql[0]; i++) {
        if (!roots[i]) continue;
        swprintf(ql, MAX_PATH, i ? L"%ls\\QuickLook\\QuickLook.exe" : L"%ls\\Programs\\QuickLook\\QuickLook.exe", roots[i]);
        if (GetFileAttributesW(ql) == INVALID_FILE_ATTRIBUTES) ql[0] = 0;
    }
    WCHAR args[MAX_PATH * 2 + 4];
    swprintf(args, MAX_PATH * 2 + 4, L"\"%ls\"", item);
    if (!ql[0] || (INT_PTR)ShellExecuteW(win, NULL, ql, args, NULL, SW_SHOWNORMAL) <= 32) open_native(item);
    free(item);
}

// ---- Child processes: Claude Code, tar and the scanner

typedef struct { HANDLE proc, out; int id; LONG gen; } Child;

// Starts a command line with stdout (or a file) and stderr in one pipe; stdin gets `input`, then closes.
// Each child gets its own job: closing the job kills it, and Windows closes the job when Evict quits.
static BOOL spawn(WCHAR *cmd, const char *input, HANDLE file, Child *c, HANDLE *job_out) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE r, w, in_r = NULL, in_w = NULL;
    if (!CreatePipe(&r, &w, &sa, 0)) return FALSE;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    if (input && CreatePipe(&in_r, &in_w, &sa, (DWORD)strlen(input) + 4096)) SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = file ? file : w;
    si.hStdError = w;
    PROCESS_INFORMATION pi;
    WCHAR tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);  // like a terminal opened anywhere: no project around for Claude Code
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                             NULL, tmp, &si, &pi);
    CloseHandle(w);
    if (in_r) CloseHandle(in_r);
    if (!ok) {
        CloseHandle(r);
        if (in_w) CloseHandle(in_w);
        return FALSE;
    }
    HANDLE j = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (j) SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li), AssignProcessToJobObject(j, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (in_w) {
        DWORD done;
        WriteFile(in_w, input, (DWORD)strlen(input), &done, NULL);
        CloseHandle(in_w);
    }
    c->proc = pi.hProcess;
    c->out = r;
    if (*job_out) CloseHandle(*job_out);
    *job_out = j;
    return TRUE;
}

// Calls `line` for each line of the child's output, then returns once it has quit
static void read_lines(Child *c, void (*line)(Child *, char *)) {
    char *buf = malloc(65536), *acc = NULL;
    size_t n = 0;
    DWORD got;
    while (buf && ReadFile(c->out, buf, 65536, &got, NULL) && got) {
        if (!(acc = realloc(acc, n + got + 1))) abort();
        memcpy(acc + n, buf, got);
        n += got;
        acc[n] = 0;
        char *start = acc, *nl;
        while ((nl = memchr(start, '\n', n - (start - acc)))) {
            *nl = 0;
            if (nl > start && nl[-1] == '\r') nl[-1] = 0;
            line(c, start);
            start = nl + 1;
        }
        n -= start - acc;
        memmove(acc, start, n);
    }
    if (n) acc[n] = 0, line(c, acc);
    free(buf), free(acc);
    WaitForSingleObject(c->proc, INFINITE);
}

static void cancel(void) {
    InterlockedIncrement(&gen);
    if (job) CloseHandle(job), job = NULL;
}

// The text of a child's output: UTF-8 when it's valid, the ANSI code page otherwise (tar names)
static WCHAR *decode(const char *s) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) ? wide(s, -1, CP_UTF8) : wide(s, -1, CP_ACP);
}

// ---- Claude Code: stream-json lines go to the page as they come, it keeps the text deltas (see `bridge`)

static void claude_line(Child *c, char *line) {
    if (c->gen != gen) return;
    WCHAR *w = decode(line);
    Str js = {0};
    addf(&js, L"evictClaudeLine(%d, ", c->id);
    add_js(&js, w);
    adds(&js, L")");
    post_js(&js);
    free(w);
}

static DWORD WINAPI claude_thread(void *arg) {
    Child *c = arg;
    read_lines(c, claude_line);
    if (c->gen == gen) {
        Str js = {0};
        addf(&js, L"evictClaudeEnd(%d)", c->id);
        post_js(&js);
    }
    CloseHandle(c->proc), CloseHandle(c->out), free(c);
    return 0;
}

static void ask(int id, const WCHAR *prompt) {
    cancel();
    if (!claude[0]) return answer(id, L"Claude Code isn’t installed.", L"error");
    // No tools at all: Claude can only answer, it can't read, run or delete anything. Opus at low effort, measured:
    // a quick wins plan in ~20 s, smarter advice than Sonnet. The prompt goes through stdin, nothing to escape.
    const WCHAR *flags = L"-p --model opus --effort low --tools \"\" --no-session-persistence --output-format stream-json "
                         L"--include-partial-messages --verbose";
    WCHAR cmd[MAX_PATH * 2 + 512];
    if (!_wcsicmp(PathFindExtensionW(claude), L".exe")) swprintf(cmd, MAX_PATH * 2 + 512, L"\"%ls\" %ls", claude, flags);
    else swprintf(cmd, MAX_PATH * 2 + 512, L"cmd.exe /d /s /c \"\"%ls\" %ls\"", claude, flags);  // npm's claude.cmd
    Child *c = calloc(1, sizeof *c);
    char *input = utf8(prompt);
    c->id = id;
    c->gen = gen;
    if (!spawn(cmd, input, NULL, c, &job)) {
        Str b = {0};
        error_text(&b, GetLastError());
        answer(id, b.s, L"error");
        free(b.s), free(c);
    } else {
        HANDLE t = CreateThread(NULL, 0, claude_thread, c, 0, NULL);
        if (t) CloseHandle(t);
    }
    free(input);
}

// ---- Archives: tar.exe, built into Windows 10 since 1803, is bsdtar (libarchive): zip, 7z, rar, tar.*, iso, cab…

typedef struct { WCHAR *name; long long size; } Entry;
typedef struct { Child c; Str log; Entry *v; int n, cap; long long total; } Listing;

static int by_size(const void *a, const void *b) {
    long long x = ((const Entry *)a)->size, y = ((const Entry *)b)->size;
    return x < y ? 1 : x > y ? -1 : 0;
}

// bsdtar -tv: mode links owner group size month day time name; the name keeps its spaces
static void tar_line(Child *c, char *line) {
    Listing *l = (Listing *)c;
    WCHAR *w = decode(line), *f[9];
    int k = 0;
    for (WCHAR *p = w; k < 9 && *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        f[k++] = p;
        if (k == 9) break;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    WCHAR *end;
    long long size = k == 9 ? wcstoll(f[4], &end, 10) : 0;
    if (k == 9 && f[0][0] != 'd' && !*end) {
        if (l->n == l->cap && !(l->v = realloc(l->v, (l->cap = l->cap * 2 + 64) * sizeof *l->v))) abort();
        l->v[l->n++] = (Entry){_wcsdup(f[8]), size};
        l->total += size;
    } else if (*line) {
        l->log.n = 0;
        adds(&l->log, w);
    }
    free(w);
}

static void archive_result(int id, Str *json) {
    Str js = {0};
    addf(&js, L"evictArchive(%d, %ls)", id, json->s);
    post_js(&js);
    free(json->s);
}

static void archive_error(int id, const WCHAR *message) {
    Str json = {0};
    adds(&json, L"{\"error\":");
    add_js(&json, message);
    adds(&json, L"}");
    archive_result(id, &json);
}

static DWORD WINAPI tar_thread(void *arg) {
    Listing *l = arg;
    read_lines(&l->c, tar_line);
    if (l->c.gen == gen) {
        if (!l->n) {
            const WCHAR *last = l->log.s ? l->log.s : L"This archive is empty.";
            if (!wcsncmp(last, L"tar.exe: ", 9)) last += 9;
            else if (!wcsncmp(last, L"tar: ", 5)) last += 5;
            archive_error(l->c.id, last);
        } else {
            qsort(l->v, l->n, sizeof *l->v, by_size);
            Str json = {0};
            addf(&json, L"{\"count\":%d,\"total\":%lld,\"entries\":[", l->n, l->total);
            for (int i = 0; i < l->n && i < 500; i++) {
                adds(&json, i ? L",[" : L"[");
                add_js(&json, l->v[i].name);
                addf(&json, L",%lld]", l->v[i].size);
            }
            adds(&json, L"]}");
            archive_result(l->c.id, &json);
        }
    }
    for (int i = 0; i < l->n; i++) free(l->v[i].name);
    CloseHandle(l->c.proc), CloseHandle(l->c.out), free(l->v), free(l->log.s), free(l);
    return 0;
}

static void list_archive(int id, const WCHAR *path) {
    cancel();
    WCHAR tar[MAX_PATH], cmd[MAX_PATH * 3];
    GetSystemDirectoryW(tar, MAX_PATH);
    wcscat(tar, L"\\tar.exe");
    if (GetFileAttributesW(tar) == INVALID_FILE_ATTRIBUTES)
        return archive_error(id, L"Listing archives needs tar, built into Windows 10 since version 1803.");
    WCHAR *file = native(path);
    swprintf(cmd, MAX_PATH * 3, L"\"%ls\" -tvf \"%ls\"", tar, file);
    free(file);
    Listing *l = calloc(1, sizeof *l);
    l->c.id = id;
    l->c.gen = gen;
    if (!spawn(cmd, NULL, NULL, &l->c, &job)) {
        free(l);
        return archive_error(id, L"Couldn’t start tar.");
    }
    HANDLE t = CreateThread(NULL, 0, tar_thread, l, 0, NULL);
    if (t) CloseHandle(t);
}

// ---- Duplicates

typedef struct { long long size; BYTE hash[32]; int i; BOOL ok; } Key;
typedef struct { int id; LONG gen; J groups; } Dupes;

// SHA-256 of a whole file, or of its first, middle and last megabyte (sample): enough to tell copies apart without reading them whole
static void hash_file(const WCHAR *path, BOOL sample, LONG g, Key *k) {
    WCHAR buf[LONG_PATH];
    long_path(buf, path);
    k->ok = FALSE;
    HANDLE f = CreateFileW(buf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER size;
    if (f == INVALID_HANDLE_VALUE) return;
    BCRYPT_HASH_HANDLE h = NULL;
    BYTE *chunk = malloc(1 << 20);
    if (chunk && GetFileSizeEx(f, &size) && BCRYPT_SUCCESS(BCryptCreateHash(sha, &h, NULL, 0, NULL, 0, 0))) {
        const LONGLONG mb = 1 << 20;
        DWORD got;
        if (sample && size.QuadPart > 3 * mb) {
            LONGLONG at[] = {0, size.QuadPart / 2, size.QuadPart - mb};
            for (int i = 0; i < 3; i++) {
                LARGE_INTEGER pos = {.QuadPart = at[i]};
                if (SetFilePointerEx(f, pos, NULL, FILE_BEGIN) && ReadFile(f, chunk, (DWORD)mb, &got, NULL)) BCryptHashData(h, chunk, got, 0);
            }
        } else {
            while (g == gen && ReadFile(f, chunk, (DWORD)mb, &got, NULL) && got) BCryptHashData(h, chunk, got, 0);
        }
        k->ok = BCRYPT_SUCCESS(BCryptFinishHash(h, k->hash, 32, 0));
        k->size = size.QuadPart;
    }
    if (h) BCryptDestroyHash(h);
    free(chunk);
    CloseHandle(f);
}

static int by_key(const void *a, const void *b) {
    const Key *x = a, *y = b;
    if (x->ok != y->ok) return y->ok - x->ok;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return memcmp(x->hash, y->hash, 32);
}

static BOOL same(const Key *a, const Key *b) { return a->ok && b->ok && !by_key(a, b); }

// Candidate sets from the page (same size and type) checked for real: the sample narrows them down, then copies are
// confirmed on their whole content. Private sizes are unknown here (-1).
static DWORD WINAPI dupes_thread(void *arg) {
    Dupes *d = arg;
    Str json = {0};
    adds(&json, L"{\"sets\":[");
    BOOL first = TRUE;
    for (int g = 0; g < d->groups.n && d->gen == gen; g++) {
        J *paths = &d->groups.v[g];
        int n = paths->t == 'a' ? paths->n : 0;
        Key *keys = calloc(n + 1, sizeof *keys), *full = calloc(n + 1, sizeof *full);
        for (int i = 0; i < n && d->gen == gen; i++) {
            keys[i].i = i;
            if (paths->v[i].t == 's') hash_file(paths->v[i].s, TRUE, d->gen, &keys[i]);
        }
        qsort(keys, n, sizeof *keys, by_key);
        for (int a = 0, b; a < n; a = b) {
            for (b = a + 1; b < n && same(&keys[a], &keys[b]); b++) {}
            if (b - a < 2 || !keys[a].ok) continue;
            for (int i = a; i < b && d->gen == gen; i++) full[i - a].i = keys[i].i, hash_file(paths->v[keys[i].i].s, FALSE, d->gen, &full[i - a]);
            qsort(full, b - a, sizeof *full, by_key);
            for (int x = 0, y; x < b - a; x = y) {
                for (y = x + 1; y < b - a && same(&full[x], &full[y]); y++) {}
                if (y - x < 2 || !full[x].ok) continue;
                adds(&json, first ? L"[" : L",[");
                first = FALSE;
                for (int i = x; i < y; i++) {
                    adds(&json, i > x ? L",[" : L"[");
                    add_js(&json, paths->v[full[i].i].s);
                    adds(&json, L",-1]");
                }
                adds(&json, L"]");
            }
        }
        free(keys), free(full);
    }
    adds(&json, L"]}");
    if (d->gen == gen) {
        Str js = {0};
        addf(&js, L"evictDupes(%d, ", d->id);
        adds(&js, json.s);
        adds(&js, L")");
        post_js(&js);
    }
    free(json.s);
    free_j(&d->groups);
    free(d);
    return 0;
}

static void find_duplicates(int id, J *groups) {
    cancel();
    Dupes *d = calloc(1, sizeof *d);
    d->id = id;
    d->gen = gen;
    d->groups = *groups;
    memset(groups, 0, sizeof *groups);  // the thread owns them now
    HANDLE t = CreateThread(NULL, 0, dupes_thread, d, 0, NULL);
    if (t) CloseHandle(t);
    else free_j(&d->groups), free(d);
}

// ---- Scanning

static char *read_template(DWORD *len) {
    WCHAR path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\index.html", dir);
    char *html = read_file(path, len);
    if (html && strstr(html, PLACEHOLDER)) return html;
    free(html);
    return NULL;
}

static void load(const WCHAR *path) {
    WCHAR uri[MAX_PATH * 3];
    DWORD n = MAX_PATH * 3;
    if (web && SUCCEEDED(UrlCreateFromPathW(path, uri, &n, 0))) ICoreWebView2_Navigate(web, uri);
}

static void show_failure(const WCHAR *message) {
    Str html = {0};
    adds(&html, L"<body style='margin:0;height:100vh;display:grid;place-items:center;background:#0c0e12;color:#e8eaf0;"
                L"font:15px system-ui,sans-serif'><div style='text-align:center;max-width:520px'><h2>The scan failed</h2>"
                L"<p style='color:#8b93a1'>");
    for (const WCHAR *c = message; *c; c++)
        adds(&html, *c == '<' ? L"&lt;" : *c == '&' ? L"&amp;" : (WCHAR[]){*c, 0});
    adds(&html, L"</p><p style='color:#8b93a1'>Press Ctrl+R to try again.</p></div></body>");
    if (web) ICoreWebView2_NavigateToString(web, html.s);
    free(html.s);
}

// The page with `text` in place of the data placeholder, written in Evict's data folder and opened
static BOOL show_page(const WCHAR *name, const char *text, DWORD text_len) {
    DWORD len;
    char *html = read_template(&len);
    if (!html) return FALSE;
    char *slot = strstr(html, PLACEHOLDER), *after = slot + strlen(PLACEHOLDER);
    size_t head = slot - html, tail = len - (after - html);
    char *page = malloc(head + text_len + tail);
    if (!page) abort();
    memcpy(page, html, head);
    memcpy(page + head, text, text_len);
    memcpy(page + head + text_len, after, tail);
    WCHAR path[MAX_PATH];
    in_data(path, name);
    BOOL ok = write_file(path, page, (DWORD)(head + text_len + tail));
    if (ok) load(path);
    free(page), free(html);
    return ok;
}

static void progress(const char *line) {
    Str js = {0};
    WCHAR *w = wide(line, -1, CP_UTF8);
    addf(&js, L"window.evictLive && evictLive(%ls)", w);
    post_js(&js);
    free(w);
}

typedef struct { Child c; char *error; } Scan;

static void scan_line(Child *c, char *line) {
    Scan *s = (Scan *)c;
    if (line[0] == '{') progress(line);
    else if (line[strspn(line, " \r\t")]) free(s->error), s->error = _strdup(line);
}

static DWORD WINAPI scan_thread(void *arg) {
    Scan *s = arg;
    DWORD code = 1;
    read_lines(&s->c, scan_line);
    GetExitCodeProcess(s->c.proc, &code);
    CloseHandle(s->c.proc), CloseHandle(s->c.out);
    if (!PostMessageW(win, WM_SCANNED, code, (LPARAM)s->error)) free(s->error);
    free(s);
    return 0;
}

// The elevated scanner can't share pipes: it writes its messages in a .log file, read here as it grows
static DWORD WINAPI admin_thread(void *arg) {
    HANDLE proc = arg;
    WCHAR path[MAX_PATH];
    in_data(path, L"scan.json.log");
    char *error = NULL;
    size_t seen = 0;
    for (BOOL done = FALSE; !done;) {
        done = WaitForSingleObject(proc, 300) == WAIT_OBJECT_0;
        DWORD len;
        char *log = read_file(path, &len);
        if (!log) continue;
        char *last = NULL;
        for (char *line = log + seen, *nl; (nl = strchr(line, '\n')); line = nl + 1) {
            *nl = 0;
            seen = nl + 1 - log;
            if (line[0] == '{') last = line;
            else if (line[strspn(line, " \r\t")]) free(error), error = _strdup(line);
        }
        if (last) progress(last);
        free(log);
    }
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    if (!PostMessageW(win, WM_SCANNED, code, (LPARAM)error)) free(error);
    return 0;
}

static void scan(BOOL admin) {
    if (scanning) return;
    const char *live = "window.DATA = null; window.LIVE = {root: '/', os: 'win'};";
    if (!show_page(L"evict-live.html", live, (DWORD)strlen(live))) return show_failure(L"App files are missing, please reinstall Evict.");
    WCHAR json[MAX_PATH], log[MAX_PATH], args[MAX_PATH * 3];
    in_data(json, L"scan.json");
    in_data(log, L"scan.json.log");
    DeleteFileW(json), DeleteFileW(log);
    scanning = TRUE;
    admin_scan = admin;
    if (admin) {
        swprintf(args, MAX_PATH * 3, L"--progress --home \"%ls\" --out \"%ls\" /", home, json);
        SHELLEXECUTEINFOW sei = {sizeof sei, SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC, win, L"runas", scanner, args, NULL, SW_HIDE};
        HANDLE t = NULL;
        if (ShellExecuteExW(&sei) && sei.hProcess && (t = CreateThread(NULL, 0, admin_thread, sei.hProcess, 0, NULL))) return (void)CloseHandle(t);
        PostMessageW(win, WM_SCANNED, ERROR_CANCELLED, 0);  // the UAC prompt was dismissed: back to the last map
        return;
    }
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out = CreateFileW(json, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
    swprintf(args, MAX_PATH * 3, L"\"%ls\" --progress --home \"%ls\" /", scanner, home);
    Scan *s = calloc(1, sizeof *s);
    HANDLE t = NULL;
    BOOL ok = out != INVALID_HANDLE_VALUE && spawn(args, NULL, out, &s->c, &scan_job);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (ok && (t = CreateThread(NULL, 0, scan_thread, s, 0, NULL))) return (void)CloseHandle(t);
    free(s);
    scanning = FALSE;
    Str b = {0};
    adds(&b, L"Couldn’t start the scanner: ");
    error_text(&b, GetLastError());
    show_failure(b.s);
    free(b.s);
}

static void scanned(DWORD code, char *error) {
    scanning = FALSE;
    WCHAR json_path[MAX_PATH];
    in_data(json_path, L"scan.json");
    DWORD len = 0;
    char *json = code ? NULL : read_file(json_path, &len);
    if (json && len) {
        const char *prefix = "window.DATA=";
        size_t pl = strlen(prefix);
        char *text = malloc(pl + len);
        if (!text) abort();
        memcpy(text, prefix, pl);
        memcpy(text + pl, json, len);
        if (show_page(L"evict.html", text, (DWORD)(pl + len))) has_page = TRUE;
        else show_failure(L"Couldn’t write the report in Evict’s data folder.");
        free(text);
    } else if (admin_scan && code == ERROR_CANCELLED && has_page) {
        WCHAR page[MAX_PATH];
        in_data(page, L"evict.html");
        load(page);
    } else {
        Str msg = {0};
        addf(&msg, L"The scanner stopped (code %lu). ", code);
        if (error) {
            WCHAR *w = wide(error, -1, CP_UTF8);
            adds(&msg, w);
            free(w);
        }
        show_failure(msg.s);
        free(msg.s);
    }
    DeleteFileW(json_path);
    free(json), free(error);
}

// ---- Updates: the latest release's Evict-Setup.exe, checked against GitHub's SHA-256, installs over this copy

typedef struct { char version[32], url[512], digest[80]; } Update;
static Update *latest;
static int ver(const char *v) {
    int a = 0, b = 0, c = 0;
    sscanf(v, "%d.%d.%d", &a, &b, &c);
    return a * 1000000 + b * 1000 + c;
}

// The string value of the first "key" after p
static BOOL value_after(const char *p, const char *key, char *out, size_t size) {
    const char *k = strstr(p, key), *q = k ? strchr(k + strlen(key), '"') : NULL;
    size_t n = q ? strcspn(q + 1, "\"") : 0;
    if (!q || n >= size) return FALSE;
    memcpy(out, q + 1, n);
    out[n] = 0;
    return TRUE;
}

// Releases come newest first: the first one with the Windows installer is the latest for Windows
static Update *newest(char *json) {
    for (char *t = strstr(json, "\"tag_name\""); t;) {
        char *next = strstr(t + 1, "\"tag_name\""), *asset;
        if (next) *next = 0;
        Update u = {0};
        BOOL found = (asset = strstr(t, "\"" SETUP "\"")) && value_after(t, "\"tag_name\"", u.version, sizeof u.version) &&
                     value_after(asset, "\"digest\"", u.digest, sizeof u.digest) &&
                     value_after(asset, "\"browser_download_url\"", u.url, sizeof u.url);
        if (next) *next = '"';
        if (found) {
            Update *out = malloc(sizeof *out);
            if (!out) abort();
            *out = u;
            if (out->version[0] == 'v') memmove(out->version, out->version + 1, strlen(out->version));
            return out;
        }
        t = next;
    }
    return NULL;
}

static DWORD WINAPI check_thread(void *manual) {
    WCHAR url[256], file[MAX_PATH];
    swprintf(url, 256, L"%ls&t=%llu", RELEASES, GetTickCount64());  // never from the cache
    in_data(file, L"releases.json");
    char *json = SUCCEEDED(URLDownloadToFileW(NULL, url, file, 0, NULL)) ? read_file(file, NULL) : NULL;
    Update *u = json ? newest(json) : NULL;
    DeleteFileW(file);
    free(json);
    if (!PostMessageW(win, WM_CHECKED, (WPARAM)manual | (json ? 0 : 2), (LPARAM)u)) free(u);
    return 0;
}

static void check_updates(BOOL manual) {
    HANDLE t = CreateThread(NULL, 0, check_thread, (void *)(INT_PTR)manual, 0, NULL);
    if (t) CloseHandle(t);
}

static void show_update_button(const WCHAR *label) {
    Str js = {0};
    adds(&js, L"window.evictUpdate && evictUpdate(");
    add_js(&js, label);
    adds(&js, L")");
    run_js(&js);
}

static void update_button(void) {
    WCHAR label[64], *v = wide(latest->version, -1, CP_UTF8);
    swprintf(label, 64, L"Update to %ls", v);
    show_update_button(label);
    free(v);
}

static BOOL sha256_of(const WCHAR *path, char *hex) {
    Key k;
    WCHAR *r = report(path);
    hash_file(r, FALSE, gen, &k);
    free(r);
    for (int i = 0; k.ok && i < 32; i++) sprintf(hex + i * 2, "%02x", k.hash[i]);
    return k.ok;
}

static DWORD WINAPI fetch_thread(void *arg) {
    Update *u = arg;
    WCHAR *url = wide(u->url, -1, CP_UTF8), *file = malloc(MAX_PATH * sizeof *file);
    GetTempPathW(MAX_PATH, file);
    wcscat(file, WIDE(SETUP));
    char hex[65] = "";
    BOOL ok = SUCCEEDED(URLDownloadToFileW(NULL, url, file, 0, NULL)) && sha256_of(file, hex) && !strcmp(u->digest + 7, hex) &&
              !strncmp(u->digest, "sha256:", 7);
    free(url), free(u);
    if (!PostMessageW(win, WM_FETCHED, ok, (LPARAM)file)) free(file);
    return 0;
}

static void prompt_update(void) {
    if (!latest) return;
    Str title = {0}, text = {0};
    WCHAR *v = wide(latest->version, -1, CP_UTF8);
    addf(&title, L"Evict %ls is available", v);
    addf(&text, L"You’re running version %ls. Evict downloads the new version, installs it and opens again.", WIDE(VERSION));
    int r = dialog(TD_INFORMATION_ICON, title.s, text.s, L"Later", IDCANCEL, L"Install and Relaunch", B_INSTALL, NULL);
    free(title.s), free(text.s), free(v);
    if (r != B_INSTALL) return;
    show_update_button(L"Downloading…");
    Update *copy = malloc(sizeof *copy);  // the next check may replace `latest` meanwhile
    if (!copy) abort();
    *copy = *latest;
    HANDLE t = CreateThread(NULL, 0, fetch_thread, copy, 0, NULL);
    if (t) CloseHandle(t);
    else free(copy);
}

static void checked(int flags, Update *u) {
    BOOL manual = flags & 1, failed = flags & 2;
    if (failed) {
        if (manual) inform(L"Couldn’t check for updates", L"GitHub isn’t responding, try again later.");
    } else if (u && ver(u->version) > ver(VERSION)) {
        free(latest);
        latest = u;
        u = NULL;
        WCHAR seen_path[MAX_PATH];
        update_button();
        // Each new version is announced once; the toolbar button stays as the reminder
        in_data(seen_path, L"announced");
        char *seen = read_file(seen_path, NULL);
        if (manual || !seen || strcmp(seen, latest->version)) {
            write_file(seen_path, latest->version, (DWORD)strlen(latest->version));
            prompt_update();
        }
        free(seen);
    } else if (manual) {
        inform(L"Evict is up to date", L"You’re running the latest version (" WIDE(VERSION) L").");
    }
    free(u);
}

static void fetched(BOOL ok, WCHAR *file) {
    WCHAR args[MAX_PATH + 32];
    swprintf(args, MAX_PATH + 32, L"/S /relaunch /D=%ls", dir);  // /D= must come last, unquoted
    if (ok && (INT_PTR)ShellExecuteW(NULL, L"open", file, args, NULL, SW_SHOWNORMAL) > 32) {
        DestroyWindow(win);  // the installer waits for Evict to quit, then reopens it
    } else {
        show_update_button(L"Update");
        if (dialog(TD_ERROR_ICON, L"Couldn’t install the update", L"The download failed or didn’t match the release. "
                   L"You can download Evict from its website.", L"Later", IDCANCEL, L"Open the Website", B_MAIN, NULL) == B_MAIN)
            open_native(DOWNLOADS);
    }
    free(file);
}

// ---- The bridge

static WCHAR **strs(J *a, int *n) {
    *n = 0;
    WCHAR **out = calloc((a && a->t == 'a' ? a->n : 0) + 2, sizeof *out);
    for (int i = 0; a && a->t == 'a' && i < a->n; i++)
        if (a->v[i].t == 's') out[(*n)++] = a->v[i].s;
    return out;
}

static void on_message(J *body) {
    WCHAR *action = get_str(body, L"action"), *path = get_str(body, L"path");
    int id = (int)get_num(body, L"id");
    if (!action) return;
    if (!wcscmp(action, L"copy")) {
        WCHAR *text = get_str(body, L"text");
        if (text) copy_text(text);
    } else if (!wcscmp(action, L"reveal") || !wcscmp(action, L"preview")) {
        int n;
        WCHAR **paths = strs(get(body, L"paths"), &n);
        if (path) paths[n++] = path;
        if (action[0] == 'r') reveal(paths, n);
        else preview(paths, n);
        free(paths);
    } else if (!wcscmp(action, L"open") && path) {
        open_report(path);
    } else if ((!wcscmp(action, L"delete") && path) || !wcscmp(action, L"deleteMany")) {
        J *items = get(body, L"items");
        int max = (items && items->t == 'a' ? items->n : 0) + 1, n = 0;
        WCHAR **paths = calloc(max, sizeof *paths);
        double *sizes = calloc(max, sizeof *sizes);
        if (path) paths[n] = path, sizes[n++] = get_num(body, L"size");
        for (int i = 0; items && items->t == 'a' && i < items->n; i++) {
            J *it = &items->v[i];
            if (it->t == 'a' && it->n >= 2 && it->v[0].t == 's') paths[n] = it->v[0].s, sizes[n++] = it->v[1].d;
        }
        confirm_delete(paths, sizes, n);
        free(paths), free(sizes);
    } else if (!wcscmp(action, L"emptyTrash") && path) {
        confirm_empty_trash(path, get_num(body, L"size"));
    } else if (!wcscmp(action, L"archive") && path) {
        list_archive(id, path);
    } else if (!wcscmp(action, L"dupes")) {
        J *groups = get(body, L"groups");
        if (groups && groups->t == 'a') find_duplicates(id, groups);
    } else if (!wcscmp(action, L"ask")) {
        WCHAR *prompt = get_str(body, L"prompt");
        if (prompt) ask(id, prompt);
    } else if (!wcscmp(action, L"cancel")) {
        cancel();
    } else if (!wcscmp(action, L"menu")) {
        WCHAR *what = get_str(body, L"id");
        if (!what) return;
        if (!wcscmp(what, L"rescan")) scan(FALSE);
        else if (!wcscmp(what, L"admin")) scan(TRUE);
        else if (!wcscmp(what, L"updates")) check_updates(TRUE);
        else if (!wcscmp(what, L"update")) prompt_update();
    } else if (!wcscmp(action, L"ready")) {
        // The map is shown: the update button comes back, and a smoke test gets its proof
        if (latest) update_button();
        if (smoke) {
            Str js = {0};
            adds(&js, L"chrome.webview.postMessage({action: 'smoke', boxes: document.querySelectorAll('#map .box').length})");
            run_js(&js);
        }
    } else if (!wcscmp(action, L"smoke") && smoke) {
        char line[64];
        int len = snprintf(line, sizeof line, "boxes=%d\n", (int)get_num(body, L"boxes"));
        write_file(smoke, line, len);
    }
}

// ---- WebView2: COM handlers written out in C. Each is a static object whose vtable holds QueryInterface, AddRef,
// Release and Invoke; they live as long as the app, so reference counting is moot.
#define HANDLER(Iface, name, ...)                                                                                      \
    static HRESULT STDMETHODCALLTYPE name##_qi(Iface *self, REFIID riid, void **out) { (void)riid; *out = self; return S_OK; } \
    static ULONG STDMETHODCALLTYPE name##_ref(Iface *self) { (void)self; return 1; }                                   \
    static HRESULT STDMETHODCALLTYPE name(Iface *self, __VA_ARGS__);                                                   \
    static Iface##Vtbl name##_vtbl = {name##_qi, name##_ref, name##_ref, name};                                        \
    static Iface name##_obj = {&name##_vtbl};

HANDLER(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, env_ready, HRESULT, ICoreWebView2Environment *)
HANDLER(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, ctrl_ready, HRESULT, ICoreWebView2Controller *)
HANDLER(ICoreWebView2WebMessageReceivedEventHandler, message, ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *)

static HRESULT STDMETHODCALLTYPE message(ICoreWebView2WebMessageReceivedEventHandler *self, ICoreWebView2 *sender,
                                         ICoreWebView2WebMessageReceivedEventArgs *args) {
    (void)self, (void)sender;
    LPWSTR text = NULL;
    if (FAILED(ICoreWebView2WebMessageReceivedEventArgs_get_WebMessageAsJson(args, &text)) || !text) return S_OK;
    J body;
    if (parse(text, &body)) on_message(&body);
    free_j(&body);
    CoTaskMemFree(text);
    return S_OK;
}

// Claude Code's stream-json lines, read by the page: text deltas stream into the panel, the result line says how it ended
static const WCHAR *bridge =
    L"window.webkit = {messageHandlers: {evict: {postMessage: m => chrome.webview.postMessage(m)}}};"
    L"window.evictMenu = [['Rescan', 'rescan', 'Ctrl+R'], ['Rescan as Administrator…', 'admin'], ['Check for Updates…', 'updates']];"
    L"addEventListener('keydown', e => { if (e.key === 'F5' || (e.ctrlKey && !e.altKey && e.key.toLowerCase() === 'r')) {"
    L" e.preventDefault(); e.stopPropagation(); chrome.webview.postMessage({action: 'menu', id: 'rescan'}) } }, true);"
    L"(() => { const st = {};"
    L" window.evictClaudeLine = (id, line) => { const s = st[id] ?\?= {done: false, problem: ''}; let e;"
    L"  try { e = JSON.parse(line) } catch { s.problem += line + '\\n'; return }"
    L"  const t = e.event?.delta?.text;"
    L"  if (t) evictAnswer(id, t, 'delta');"
    L"  else if (e.type === 'result') { s.done = e.is_error !== true; if (!s.done) s.problem += e.result || '' } };"
    L" window.evictClaudeEnd = id => { const s = st[id] || {}; delete st[id]; evictAnswer(id, s.problem || '', s.done ? 'done' : 'error') };"
    L"})();";

static HRESULT STDMETHODCALLTYPE ctrl_ready(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *self, HRESULT result,
                                            ICoreWebView2Controller *controller) {
    (void)self;
    if (FAILED(result) || !controller) {
        MessageBoxW(win, L"Microsoft Edge WebView2 couldn’t start. Restart your computer, then open Evict again.", L"Evict", MB_ICONERROR);
        DestroyWindow(win);
        return S_OK;
    }
    ctrl = controller;
    ICoreWebView2Controller_AddRef(ctrl);
    ICoreWebView2Controller_get_CoreWebView2(ctrl, &web);
    ICoreWebView2Controller2 *c2;
    if (SUCCEEDED(ICoreWebView2Controller_QueryInterface(ctrl, &IID_ICoreWebView2Controller2, (void **)&c2))) {
        COREWEBVIEW2_COLOR bg = {255, 12, 14, 18};  // no white flash before the page paints
        ICoreWebView2Controller2_put_DefaultBackgroundColor(c2, bg);
        ICoreWebView2Controller2_Release(c2);
    }
    ICoreWebView2Controller4 *c4;
    if (SUCCEEDED(ICoreWebView2Controller_QueryInterface(ctrl, &IID_ICoreWebView2Controller4, (void **)&c4))) {
        ICoreWebView2Controller4_put_AllowExternalDrop(c4, FALSE);  // a file dropped on the window would replace the page
        ICoreWebView2Controller4_Release(c4);
    }
    ICoreWebView2Settings *s;
    if (SUCCEEDED(ICoreWebView2_get_Settings(web, &s))) {
        BOOL debug = _wgetenv(L"EVICT_DEBUG") != NULL;
        ICoreWebView2Settings_put_AreDefaultContextMenusEnabled(s, debug);  // the page has its own menus
        ICoreWebView2Settings_put_AreDevToolsEnabled(s, debug);
        ICoreWebView2Settings_put_IsStatusBarEnabled(s, FALSE);
        ICoreWebView2Settings3 *s3;
        // F5 and Ctrl+R rescan instead of reloading, Ctrl+F searches the page's own way
        if (SUCCEEDED(ICoreWebView2Settings_QueryInterface(s, &IID_ICoreWebView2Settings3, (void **)&s3))) {
            ICoreWebView2Settings3_put_AreBrowserAcceleratorKeysEnabled(s3, FALSE);
            ICoreWebView2Settings3_Release(s3);
        }
        ICoreWebView2Settings_Release(s);
    }
    WCHAR lang[LOCALE_NAME_MAX_LENGTH] = L"en";
    GetUserDefaultLocaleName(lang, LOCALE_NAME_MAX_LENGTH);
    lang[wcscspn(lang, L"-_")] = 0;
    Str script = {0};
    addf(&script, L"window.evictClaude = %ls; window.evictLang = '%ls'; window.evictVersion = '%ls';", claude[0] ? L"true" : L"false",
         lang, WIDE(VERSION));
    adds(&script, bridge);
    ICoreWebView2_AddScriptToExecuteOnDocumentCreated(web, script.s, NULL);
    free(script.s);
    EventRegistrationToken token;
    ICoreWebView2_add_WebMessageReceived(web, &message_obj, &token);
    RECT r;
    GetClientRect(win, &r);
    ICoreWebView2Controller_put_Bounds(ctrl, r);
    ICoreWebView2Controller_MoveFocus(ctrl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    scan(FALSE);
    check_updates(FALSE);
    SetTimer(win, 1, 6 * 3600 * 1000, NULL);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE env_ready(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *self, HRESULT result,
                                           ICoreWebView2Environment *env) {
    (void)self;
    if (FAILED(result) || !env || FAILED(ICoreWebView2Environment_CreateCoreWebView2Controller(env, win, &ctrl_ready_obj))) {
        MessageBoxW(win, L"Microsoft Edge WebView2 couldn’t start. Restart your computer, then open Evict again.", L"Evict", MB_ICONERROR);
        DestroyWindow(win);
    }
    return S_OK;
}

// ---- App

static LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (ctrl) {
            RECT r;
            GetClientRect(h, &r);
            ICoreWebView2Controller_put_Bounds(ctrl, r);
        }
        return 0;
    case WM_MOVE:
        if (ctrl) ICoreWebView2Controller_NotifyParentWindowPositionChanged(ctrl);
        return 0;
    case WM_SETFOCUS:
        if (ctrl) ICoreWebView2Controller_MoveFocus(ctrl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TIMER:
        check_updates(FALSE);
        return 0;
    case WM_JS:
        if (web) ICoreWebView2_ExecuteScript(web, (WCHAR *)l, NULL);
        free((void *)l);
        return 0;
    case WM_SCANNED:
        scanned((DWORD)w, (char *)l);
        return 0;
    case WM_CHECKED:
        checked((int)w, (Update *)l);
        return 0;
    case WM_FETCHED:
        fetched((BOOL)w, (WCHAR *)l);
        return 0;
    case WM_DESTROY:
        if (ctrl) ICoreWebView2Controller_Close(ctrl);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// GUI apps get the user's PATH, but installers also put the CLI in known places
static void find_claude(void) {
    const WCHAR *names[] = {L"claude.exe", L"claude.cmd"};
    for (int i = 0; i < 2; i++)
        if (SearchPathW(NULL, names[i], NULL, MAX_PATH, claude, NULL)) return;
    const WCHAR *known[][2] = {{L"USERPROFILE", L"\\.local\\bin\\claude.exe"}, {L"APPDATA", L"\\npm\\claude.cmd"}};
    for (int i = 0; i < 2; i++) {
        const WCHAR *root = _wgetenv(known[i][0]);
        if (!root) continue;
        swprintf(claude, MAX_PATH, L"%ls%ls", root, known[i][1]);
        if (GetFileAttributesW(claude) != INVALID_FILE_ATTRIBUTES) return;
    }
    claude[0] = 0;
}

// The user's Recycle Bin on the home drive, as the page sees it: /C:/$Recycle.Bin/<SID>
static void find_trash(void) {
    HANDLE token;
    union { TOKEN_USER user; BYTE buf[256]; } u;
    DWORD n;
    LPWSTR sid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
    if (GetTokenInformation(token, TokenUser, &u, sizeof u, &n) && ConvertSidToStringSidW(u.user.User.Sid, &sid)) {
        trash = calloc(wcslen(sid) + 24, sizeof *trash);
        if (trash) swprintf(trash, wcslen(sid) + 24, L"/%lc:/$Recycle.Bin/%ls", home[0], sid);
        LocalFree(sid);
    }
    CloseHandle(token);
}

typedef HRESULT(STDAPICALLTYPE *CreateEnv)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                           ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
typedef HRESULT(STDAPICALLTYPE *RuntimeVersion)(PCWSTR, LPWSTR *);

// Windows 11 and up-to-date Windows 10 come with the WebView2 Runtime; elsewhere Evict installs it from Microsoft
static BOOL runtime_ready(RuntimeVersion version) {
    LPWSTR v = NULL;
    if (SUCCEEDED(version(NULL, &v)) && v) return CoTaskMemFree(v), TRUE;
    if (dialog(TD_INFORMATION_ICON, L"Evict needs Microsoft Edge WebView2",
               L"Windows uses it to show app windows made of web pages, like Evict’s map. It comes with Windows 11; "
               L"on this computer it’s missing. Download and install it from Microsoft now (about 2 MB)?",
               L"Quit", IDCANCEL, L"Install", B_INSTALL, NULL) != B_INSTALL)
        return FALSE;
    WCHAR setup[MAX_PATH];
    GetTempPathW(MAX_PATH, setup);
    wcscat(setup, L"MicrosoftEdgeWebview2Setup.exe");
    SHELLEXECUTEINFOW sei = {sizeof sei, SEE_MASK_NOCLOSEPROCESS, NULL, L"open", setup, L"/install", NULL, SW_SHOWNORMAL};
    if (FAILED(URLDownloadToFileW(NULL, RUNTIME, setup, 0, NULL)) || !ShellExecuteExW(&sei)) {
        if (dialog(TD_ERROR_ICON, L"Couldn’t install WebView2", L"Check your Internet connection, or install it from Microsoft’s website.",
                   L"Quit", IDCANCEL, L"Open the Website", B_MAIN, NULL) == B_MAIN)
            ShellExecuteW(NULL, NULL, L"https://developer.microsoft.com/microsoft-edge/webview2/", NULL, NULL, SW_SHOWNORMAL);
        return FALSE;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);
    v = NULL;
    if (SUCCEEDED(version(NULL, &v)) && v) return CoTaskMemFree(v), TRUE;
    inform(L"WebView2 isn’t installed", L"Install it, then open Evict again.");
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev, (void)cmdline;
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 3 && !wcscmp(argv[1], L"--delete")) return delete_listed(argv[2]);

    GetModuleFileNameW(NULL, dir, MAX_PATH);
    *wcsrchr(dir, '\\') = 0;
    swprintf(scanner, MAX_PATH, L"%ls\\evict-scan.exe", dir);
    const WCHAR *local = _wgetenv(L"LOCALAPPDATA"), *profile = _wgetenv(L"USERPROFILE");
    swprintf(data, MAX_PATH, L"%ls\\Evict", local ? local : L".");
    CreateDirectoryW(data, NULL);
    wcsncpy(home, profile ? profile : L"C:\\", MAX_PATH - 1);
    smoke = _wgetenv(L"EVICT_SMOKE");
    find_claude();
    find_trash();
    BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, NULL, 0);

    // One window: opening Evict again brings it to the front
    CreateMutexW(NULL, TRUE, L"Local\\com.vassilidev.evict");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"Evict", NULL);
        if (other) ShowWindow(other, IsIconic(other) ? SW_RESTORE : SW_SHOW), SetForegroundWindow(other);
        return 0;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WCHAR loader_path[MAX_PATH];
    swprintf(loader_path, MAX_PATH, L"%ls\\WebView2Loader.dll", dir);
    HMODULE loader = LoadLibraryW(loader_path);
    CreateEnv create = loader ? (CreateEnv)(void *)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions") : NULL;
    RuntimeVersion version = loader ? (RuntimeVersion)(void *)GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString") : NULL;
    if (!create || !version) {
        MessageBoxW(NULL, L"App files are missing, please reinstall Evict.", L"Evict", MB_ICONERROR);
        return 1;
    }
    if (!runtime_ready(version)) return 1;

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(BG);
    wc.lpszClassName = L"Evict";
    RegisterClassExW(&wc);
    // 1280×820 at 100%, scaled to the screen and kept inside it
    HDC screen = GetDC(NULL);
    int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = MulDiv(1280, dpi, 96), h = MulDiv(820, dpi, 96);
    if (w > work.right - work.left) w = work.right - work.left;
    if (h > work.bottom - work.top) h = work.bottom - work.top;
    win = CreateWindowExW(0, L"Evict", L"Evict", WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - w) / 2,
                          work.top + (work.bottom - work.top - h) / 2, w, h, NULL, NULL, inst, NULL);
    // A dark title bar in the page's color (Windows 10 2004+ and 11; older versions keep their own)
    BOOL dark = TRUE;
    COLORREF caption = BG, text = RGB(232, 234, 240);
    if (FAILED(DwmSetWindowAttribute(win, 20, &dark, sizeof dark))) DwmSetWindowAttribute(win, 19, &dark, sizeof dark);
    DwmSetWindowAttribute(win, 35, &caption, sizeof caption);
    DwmSetWindowAttribute(win, 36, &text, sizeof text);
    ShowWindow(win, show);
    UpdateWindow(win);

    WCHAR profile_dir[MAX_PATH];
    in_data(profile_dir, L"WebView2");
    if (FAILED(create(NULL, profile_dir, NULL, &env_ready_obj))) {
        MessageBoxW(win, L"Microsoft Edge WebView2 couldn’t start. Restart your computer, then open Evict again.", L"Evict", MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
