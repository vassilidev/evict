// Evict for Linux: the shared page (index.html) in a WebKitGTK window, with a native bridge for what a web page
// can't do: Trash and deletion, the file manager, previews, archives, duplicates and Claude Code answers.
// WebKitGTK exposes the same bridge as the macOS app (window.webkit.messageHandlers.evict), so the page runs as is.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <ftw.h>
#include <gtk/gtk.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <webkit2/webkit2.h>

#ifndef VERSION
#define VERSION "0"
#endif
#define APP_ID "com.vassilidev.evict"
#define PLACEHOLDER "window.DATA = null; /* __DATA__ */"
#define INSTALL "curl -fsSL https://vassilidev.github.io/evict/install.sh | sh"
#define RELEASES "https://api.github.com/repos/vassilidev/evict/releases?per_page=10"

enum { R_MAIN = 1, R_DELETE, R_COPY };

// WebKitGTK 4.1 (libsoup 3) on recent distributions, 4.0 (libsoup 2) on older ones like RHEL 9, Debian 11 and Ubuntu
// 20.04: the same API, so Evict loads whichever is installed instead of linking one
#define WEBKIT(X)                                                                                                      \
    X(webkit_web_view_get_type) X(webkit_web_view_run_javascript) X(webkit_web_view_new_with_user_content_manager)      \
    X(webkit_web_view_load_uri) X(webkit_web_view_load_html) X(webkit_web_view_set_background_color)                    \
    X(webkit_user_content_manager_new) X(webkit_user_content_manager_register_script_message_handler)                  \
    X(webkit_user_content_manager_add_script) X(webkit_user_script_new) X(webkit_user_script_unref)                     \
    X(webkit_javascript_result_get_js_value) X(jsc_value_object_get_property) X(jsc_value_object_get_property_at_index) \
    X(jsc_value_to_string) X(jsc_value_is_string) X(jsc_value_to_double) X(jsc_value_is_number) X(jsc_value_to_int32)   \
    X(jsc_value_is_object) X(jsc_value_is_array)
#define POINTER(f) static __typeof__(f) *p_##f;
WEBKIT(POINTER)

static const char *load_webkit(void) {
    static const char *libs[] = {"libwebkit2gtk-4.1.so.0", "libwebkit2gtk-4.0.so.37"};
    for (size_t i = 0; i < G_N_ELEMENTS(libs); i++) {
        void *lib = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
        gboolean all = lib != NULL;
#define RESOLVE(f) all = all && (p_##f = (__typeof__(p_##f))dlsym(lib, #f));
        WEBKIT(RESOLVE)
        if (all) return libs[i];
    }
    return NULL;
}
#define webkit_web_view_get_type (*p_webkit_web_view_get_type)
#define webkit_web_view_run_javascript (*p_webkit_web_view_run_javascript)
#define webkit_web_view_new_with_user_content_manager (*p_webkit_web_view_new_with_user_content_manager)
#define webkit_web_view_load_uri (*p_webkit_web_view_load_uri)
#define webkit_web_view_load_html (*p_webkit_web_view_load_html)
#define webkit_web_view_set_background_color (*p_webkit_web_view_set_background_color)
#define webkit_user_content_manager_new (*p_webkit_user_content_manager_new)
#define webkit_user_content_manager_register_script_message_handler (*p_webkit_user_content_manager_register_script_message_handler)
#define webkit_user_content_manager_add_script (*p_webkit_user_content_manager_add_script)
#define webkit_user_script_new (*p_webkit_user_script_new)
#define webkit_user_script_unref (*p_webkit_user_script_unref)
#define webkit_javascript_result_get_js_value (*p_webkit_javascript_result_get_js_value)
#define jsc_value_object_get_property (*p_jsc_value_object_get_property)
#define jsc_value_object_get_property_at_index (*p_jsc_value_object_get_property_at_index)
#define jsc_value_to_string (*p_jsc_value_to_string)
#define jsc_value_is_string (*p_jsc_value_is_string)
#define jsc_value_to_double (*p_jsc_value_to_double)
#define jsc_value_is_number (*p_jsc_value_is_number)
#define jsc_value_to_int32 (*p_jsc_value_to_int32)
#define jsc_value_is_object (*p_jsc_value_is_object)
#define jsc_value_is_array (*p_jsc_value_is_array)

static GtkWidget *win, *update_btn;
static WebKitWebView *view;
static GSimpleAction *rescan_action, *admin_action;
static char *dir, *cache, *home, *trash_dir, *trash_files, *claude, *page, *latest, *scan_error;
static GSubprocess *running;  // Claude Code or tar: a new request stops the previous one
static GCancellable *job;     // duplicate check
static gboolean scanning, admin_scan, gnu_tar;
static regex_t re_bad, re_sys, re_homes, re_secrets, re_secret, re_example, re_heavy;

// ---- Talking to the page

// A C string as a JavaScript string literal
static void js_str(GString *out, const char *s) {
    g_string_append_c(out, '"');
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') g_string_append_c(out, '\\');
        if (*c < 0x20) g_string_append_printf(out, "\\u%04x", *c);
        else g_string_append_c(out, (char)*c);
    }
    g_string_append_c(out, '"');
}

static void run_js(GString *js) {
    webkit_web_view_run_javascript(view, js->str, NULL, NULL, NULL);
    g_string_free(js, TRUE);
}

static void answer(int id, const char *text, const char *kind) {
    GString *js = g_string_new(NULL);
    g_string_printf(js, "evictAnswer(%d, ", id);
    js_str(js, text);
    g_string_append_printf(js, ", '%s')", kind);
    run_js(js);
}

static void removed(GPtrArray *paths, gboolean freed) {
    if (!paths->len) return;
    GString *js = g_string_new("evictRemoved([");
    for (guint i = 0; i < paths->len; i++) {
        if (i) g_string_append_c(js, ',');
        js_str(js, paths->pdata[i]);
    }
    g_string_append_printf(js, "], %s)", freed ? "true" : "false");
    run_js(js);
}

static char *get_str(JSCValue *o, const char *key) {
    JSCValue *v = jsc_value_object_get_property(o, key);
    char *s = jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
    g_object_unref(v);
    return s;
}

static double get_num(JSCValue *o, const char *key) {
    JSCValue *v = jsc_value_object_get_property(o, key);
    double d = jsc_value_is_number(v) ? jsc_value_to_double(v) : 0;
    g_object_unref(v);
    return d;
}

static int length_of(JSCValue *a) {
    if (!jsc_value_is_array(a)) return 0;
    JSCValue *l = jsc_value_object_get_property(a, "length");
    int n = jsc_value_to_int32(l);
    g_object_unref(l);
    return n;
}

static GPtrArray *strs_of(JSCValue *a) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0, n = length_of(a); i < n; i++) {
        JSCValue *v = jsc_value_object_get_property_at_index(a, i);
        if (jsc_value_is_string(v)) g_ptr_array_add(out, jsc_value_to_string(v));
        g_object_unref(v);
    }
    return out;
}

static GPtrArray *get_strs(JSCValue *o, const char *key) {
    JSCValue *a = jsc_value_object_get_property(o, key);
    GPtrArray *out = strs_of(a);
    g_object_unref(a);
    return out;
}

// ---- Dialogs

// Buttons are (label, response) pairs ending with NULL; the last one is the default
static int dialog(GtkMessageType type, const char *title, const char *text, ...) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, type,
                                          GTK_BUTTONS_NONE, "%s", title);
    if (text) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    va_list ap;
    va_start(ap, text);
    int last = GTK_RESPONSE_CLOSE;
    for (const char *label; (label = va_arg(ap, const char *));) {
        last = va_arg(ap, int);
        GtkWidget *b = gtk_dialog_add_button(GTK_DIALOG(d), label, last);
        if (last == R_DELETE) gtk_style_context_add_class(gtk_widget_get_style_context(b), "destructive-action");
    }
    va_end(ap);
    gtk_dialog_set_default_response(GTK_DIALOG(d), last);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r;
}

static void inform(const char *title, const char *text) {
    dialog(GTK_MESSAGE_INFO, title, text, "OK", GTK_RESPONSE_OK, NULL);
}

// Decimal units like the page, always in English
static char *bytes(double n) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1000 && i < 4) n /= 1000, i++;
    return g_strdup_printf(i && n < 10 ? "%.1f %s" : "%.0f %s", n, units[i]);
}

// ---- Deletion

// Same rules as canRm() in index.html: never the disk root, top-level folders, homes, system paths or secrets
static gboolean can_remove(const char *p) {
    return p[0] == '/' && regexec(&re_bad, p, 0, NULL, 0) && regexec(&re_sys, p, 0, NULL, 0) &&
           regexec(&re_homes, p, 0, NULL, 0) && regexec(&re_secrets, p, 0, NULL, 0);
}

static gboolean in_trash(const char *p) {
    size_t n = strlen(trash_files);
    return !strncmp(p, trash_files, n) && p[n] == '/';
}

// An item deleted from the Trash leaves its .trashinfo behind
static void forget_trashed(const char *p) {
    const char *name = p + strlen(trash_files) + 1;
    if (strchr(name, '/')) return;
    char *info = g_strdup_printf("%s/info/%s.trashinfo", trash_dir, name);
    unlink(info);
    g_free(info);
}

static int rm_entry(const char *p, const struct stat *st, int type, struct FTW *f) {
    (void)st, (void)type, (void)f;
    return remove(p) ? -1 : 0;
}

static gboolean rm_tree(const char *p, char **reason) {
    if (!nftw(p, rm_entry, 32, FTW_DEPTH | FTW_PHYS)) return TRUE;
    g_free(*reason);
    *reason = g_strdup(g_strerror(errno));
    return FALSE;
}

static gboolean is_secret(const char *name) {
    return !regexec(&re_secret, name, 0, NULL, 0) && regexec(&re_example, name, 0, NULL, 0);
}

// Secrets (.env, private keys, credentials) in what's about to be deleted. A quick look: heavy dependency
// folders are skipped and the walk stops after a second, so a huge folder can hide some
static GPtrArray *found;
static gint64 deadline;

static int secret_entry(const char *p, const struct stat *st, int type, struct FTW *f) {
    (void)st;
    const char *name = p + f->base;
    if (type == FTW_D && f->level && !regexec(&re_heavy, name, 0, NULL, 0)) return FTW_SKIP_SUBTREE;
    if (is_secret(name)) g_ptr_array_add(found, g_strdup(p));
    return found->len >= 50 || g_get_monotonic_time() > deadline ? FTW_STOP : FTW_CONTINUE;
}

static GPtrArray *secrets(GPtrArray *paths) {
    found = g_ptr_array_new_with_free_func(g_free);
    deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    for (guint i = 0; i < paths->len && found->len < 50 && g_get_monotonic_time() < deadline; i++)
        nftw(paths->pdata[i], secret_entry, 32, FTW_PHYS | FTW_ACTIONRETVAL);
    return found;
}

// Files owned by root or another user: retry through pkexec, the desktop's administrator password prompt
static void offer_admin(GPtrArray *paths, const char *reason) {
    char *text = g_strdup_printf("%s\n\nDelete %s permanently using an administrator password?", reason,
                                 paths->len > 1 ? "these items" : "it");
    int r = dialog(GTK_MESSAGE_WARNING, "Permission denied", text, "Cancel", GTK_RESPONSE_CANCEL,
                   "Delete as Administrator", R_DELETE, NULL);
    g_free(text);
    if (r != R_DELETE) return;
    char *rm = g_find_program_in_path("rm");
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, "pkexec");
    g_ptr_array_add(argv, rm ? rm : "/bin/rm");
    g_ptr_array_add(argv, "-rf");
    g_ptr_array_add(argv, "--");
    for (guint i = 0; i < paths->len; i++) g_ptr_array_add(argv, paths->pdata[i]);
    g_ptr_array_add(argv, NULL);
    char *err = NULL;
    int status = 0;
    GError *e = NULL;
    gboolean ok = g_spawn_sync(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &err, &status, &e);
    g_ptr_array_free(argv, TRUE);
    g_free(rm);
    if (!ok) {
        inform("Couldn’t delete", e->message);
        g_error_free(e);
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        removed(paths, TRUE);
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 126) {  // 126: the password prompt was dismissed
        inform("Couldn’t delete", err && *g_strstrip(err) ? err : "The administrator password wasn’t accepted.");
    }
    g_free(err);
}

// One or many items: everything goes to the Trash, or everything is deleted for good (items already in the Trash always are)
static void confirm_delete(GPtrArray *all, GArray *sizes) {
    GPtrArray *items = g_ptr_array_new();
    double total = 0;
    gboolean all_in_trash = TRUE;
    for (guint i = 0; i < all->len; i++)
        if (can_remove(all->pdata[i])) {
            g_ptr_array_add(items, all->pdata[i]);
            total += g_array_index(sizes, double, i);
            all_in_trash &= in_trash(all->pdata[i]);
        }
    if (!items->len) { g_ptr_array_free(items, TRUE); return; }
    guint n = items->len;
    char *size = bytes(total), *title;
    GString *text = g_string_new(NULL);
    if (n == 1) {
        char *name = g_path_get_basename(items->pdata[0]);
        title = g_strdup_printf("Delete “%s”?", name);
        g_string_printf(text, "%s · %s\n\n", size, (char *)items->pdata[0]);
        g_free(name);
    } else {
        title = g_strdup_printf("Delete %u items?", n);
        g_string_printf(text, "%s in total\n\n", size);
        for (guint i = 0; i < n && i < 8; i++) {
            char *name = g_path_get_basename(items->pdata[i]);
            g_string_append_printf(text, "• %s\n", name);
            g_free(name);
        }
        if (n > 8) g_string_append_printf(text, "• and %u more\n", n - 8);
        g_string_append_c(text, '\n');
    }
    g_string_append(text, all_in_trash ? "Already in the Trash: deleted permanently."
                          : n > 1 ? "The Trash keeps them recoverable, but the space is only freed once the Trash is emptied."
                                  : "The Trash keeps it recoverable, but the space is only freed once the Trash is emptied.");
    GPtrArray *keys = secrets(items);
    if (keys->len) {
        g_string_append(text, "\n\n⚠️ Secrets inside, often in no git repository or backup:");
        for (guint i = 0; i < keys->len && i < 6; i++) {
            const char *p = keys->pdata[i];
            size_t h = strlen(home);
            if (!strncmp(p, home, h) && p[h] == '/') g_string_append_printf(text, "\n• ~%s", p + h);
            else g_string_append_printf(text, "\n• %s", p);
        }
        if (keys->len > 6) g_string_append(text, "\n• and more");
    }
    g_ptr_array_free(keys, TRUE);
    int r = all_in_trash ? dialog(GTK_MESSAGE_WARNING, title, text->str, "Cancel", GTK_RESPONSE_CANCEL,
                                  "Delete Permanently", R_DELETE, NULL)
                         : dialog(GTK_MESSAGE_WARNING, title, text->str, "Cancel", GTK_RESPONSE_CANCEL,
                                  "Delete Permanently", R_DELETE, "Move to Trash", R_MAIN, NULL);
    g_free(title), g_free(size), g_string_free(text, TRUE);
    if (r == R_MAIN || r == R_DELETE) {
        GPtrArray *trashed = g_ptr_array_new(), *deleted = g_ptr_array_new(), *failed = g_ptr_array_new();
        char *reason = NULL;
        for (guint i = 0; i < n; i++) {
            char *p = items->pdata[i];
            if (r == R_DELETE || in_trash(p)) {
                if (rm_tree(p, &reason)) {
                    if (in_trash(p)) forget_trashed(p);
                    g_ptr_array_add(deleted, p);
                } else g_ptr_array_add(failed, p);
                continue;
            }
            GFile *f = g_file_new_for_path(p);
            GError *e = NULL;
            if (g_file_trash(f, NULL, &e)) g_ptr_array_add(trashed, p);
            else {
                g_ptr_array_add(failed, p);
                g_free(reason);
                reason = g_strdup(e->message);
                g_error_free(e);
            }
            g_object_unref(f);
        }
        removed(trashed, FALSE);
        removed(deleted, TRUE);
        if (failed->len) offer_admin(failed, reason);
        g_free(reason);
        g_ptr_array_free(trashed, TRUE), g_ptr_array_free(deleted, TRUE), g_ptr_array_free(failed, TRUE);
    }
    g_ptr_array_free(items, TRUE);
}

// Empties the user's Trash item by item; the page can only ask for this exact folder
static void confirm_empty_trash(const char *path, double size) {
    if (strcmp(path, trash_files)) return;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(path, 0, NULL);
    for (const char *name; d && (name = g_dir_read_name(d));) g_ptr_array_add(paths, g_build_filename(path, name, NULL));
    if (d) g_dir_close(d);
    if (!paths->len) {
        inform("The Trash is already empty", "Rescan to update the map.");
        g_ptr_array_free(paths, TRUE);
        return;
    }
    char *s = bytes(size);
    char *text = g_strdup_printf("%u item%s, %s, will be deleted permanently. This can’t be undone.", paths->len,
                                 paths->len > 1 ? "s" : "", s);
    int r = dialog(GTK_MESSAGE_WARNING, "Empty the Trash?", text, "Cancel", GTK_RESPONSE_CANCEL, "Empty Trash", R_DELETE, NULL);
    g_free(s), g_free(text);
    if (r == R_DELETE) {
        GPtrArray *deleted = g_ptr_array_new();
        char *reason = NULL;
        for (guint i = 0; i < paths->len; i++)
            if (rm_tree(paths->pdata[i], &reason)) {
                forget_trashed(paths->pdata[i]);
                g_ptr_array_add(deleted, paths->pdata[i]);
            }
        if (deleted->len == paths->len) {
            GString *js = g_string_new("evictEmptied(");
            js_str(js, path);
            g_string_append_c(js, ')');
            run_js(js);
        } else {
            removed(deleted, TRUE);
            guint left = paths->len - deleted->len;
            char *msg = g_strdup_printf("%u item%s to another user or in use. Empty the Trash from your file manager to remove them.",
                                        left, left > 1 ? "s belong" : " belongs");
            inform("Some items couldn’t be deleted", msg);
            g_free(msg);
        }
        g_free(reason);
        g_ptr_array_free(deleted, TRUE);
    }
    g_ptr_array_free(paths, TRUE);
}

// ---- Files, previews, clipboard

static void open_uri(const char *uri) {
    GError *e = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &e)) {
        inform("Couldn’t open it", e->message);
        g_error_free(e);
    }
}

static void open_path(const char *path) {
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) open_uri(uri);
    g_free(uri);
}

static GVariant *bus_call(const char *name, const char *object, const char *iface, const char *method, GVariant *args) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) { g_variant_unref(g_variant_ref_sink(args)); return NULL; }
    GVariant *r = g_dbus_connection_call_sync(bus, name, object, iface, method, args, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    g_object_unref(bus);
    return r;
}

// The file manager (Files, Dolphin, Nemo, Caja…) with the items selected, or their folder as a fallback
static void reveal(GPtrArray *paths) {
    if (!paths->len) return;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    for (guint i = 0; i < paths->len; i++) {
        char *uri = g_filename_to_uri(paths->pdata[i], NULL, NULL);
        if (uri) g_variant_builder_add(&b, "s", uri);
        g_free(uri);
    }
    GVariant *r = bus_call("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1",
                           "ShowItems", g_variant_new("(ass)", &b, ""));
    if (r) { g_variant_unref(r); return; }
    char *parent = g_path_get_dirname(paths->pdata[0]);
    open_path(parent);
    g_free(parent);
}

// Space: GNOME's quick previewer (Sushi) when it's there, otherwise the default app.
// shortcut: only the first item of a selection is shown, to avoid opening dozens of apps; a preview panel would show them all
static void preview(GPtrArray *paths) {
    if (!paths->len) return;
    char *uri = g_filename_to_uri(paths->pdata[0], NULL, NULL);
    if (!uri) return;
    GVariant *r = bus_call("org.gnome.NautilusPreviewer", "/org/gnome/NautilusPreviewer", "org.gnome.NautilusPreviewer2",
                           "ShowFile", g_variant_new("(ssb)", uri, "", FALSE));
    if (r) g_variant_unref(r);
    else open_uri(uri);
    g_free(uri);
}

// ---- Background jobs: Claude Code, archives, duplicates

static void cancel(void) {
    if (job) g_cancellable_cancel(job);
    g_clear_object(&job);
    if (running) g_subprocess_force_exit(running);
    g_clear_object(&running);
}

static GSubprocess *spawn(GSubprocessFlags flags, const char *const *argv, GError **e) {
    GSubprocessLauncher *l = g_subprocess_launcher_new(flags);
    g_subprocess_launcher_set_cwd(l, g_get_tmp_dir());
    GSubprocess *p = g_subprocess_launcher_spawnv(l, argv, e);
    g_object_unref(l);
    return p;
}

// stream-json lines go to the page as they come, it keeps the text deltas (see the script added in activate)
static void claude_line(GObject *src, GAsyncResult *res, gpointer data) {
    GDataInputStream *in = G_DATA_INPUT_STREAM(src);
    char *line = g_data_input_stream_read_line_finish(in, res, NULL, NULL);
    GString *js = g_string_new(NULL);
    if (!line) {
        g_string_printf(js, "evictClaudeEnd(%d)", GPOINTER_TO_INT(data));
        run_js(js);
        g_object_unref(in);
        return;
    }
    g_string_printf(js, "evictClaudeLine(%d, ", GPOINTER_TO_INT(data));
    js_str(js, line);
    g_string_append_c(js, ')');
    run_js(js);
    g_free(line);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, claude_line, data);
}

static void ask(int id, const char *prompt) {
    cancel();
    if (!claude) return answer(id, "Claude Code isn’t installed.", "error");
    // No tools at all: Claude can only answer, it can't read, run or delete anything.
    // Opus at low effort, measured: a quick wins plan in ~20 s (minutes at default effort), smarter advice than Sonnet
    const char *argv[] = {claude, "-p", "--model", "opus", "--effort", "low", "--tools", "", "--no-session-persistence",
                          "--output-format", "stream-json", "--include-partial-messages", "--verbose", prompt, NULL};
    GError *e = NULL;
    GSubprocess *p = spawn(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, argv, &e);
    if (!p) {
        answer(id, e->message, "error");
        g_error_free(e);
        return;
    }
    g_output_stream_close(g_subprocess_get_stdin_pipe(p), NULL, NULL);
    running = p;
    GDataInputStream *in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(p));
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, claude_line, GINT_TO_POINTER(id));
}

typedef struct {
    char *name;
    gint64 size;
} Entry;

static int by_size(const void *a, const void *b) {
    gint64 x = ((const Entry *)a)->size, y = ((const Entry *)b)->size;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void call_page(const char *fn, int id, GString *payload) {
    GString *js = g_string_new(NULL);
    g_string_printf(js, "%s(%d, %s)", fn, id, payload->str);
    run_js(js);
    g_string_free(payload, TRUE);
}

static void archive_error(int id, const char *message) {
    GString *json = g_string_new("{\"error\":");
    js_str(json, message);
    g_string_append_c(json, '}');
    call_page("evictArchive", id, json);
}

// Splits off space-separated fields; the last one keeps the rest of the line (names can hold spaces)
static int fields(char *line, char **f, int n) {
    int k = 0;
    while (k < n) {
        while (*line == ' ') line++;
        if (!*line) break;
        f[k++] = line;
        if (k == n) break;
        while (*line && *line != ' ') line++;
        if (*line) *line++ = 0;
    }
    return k;
}

static void archive_listed(GObject *src, GAsyncResult *res, gpointer data) {
    int id = GPOINTER_TO_INT(data);
    GBytes *out = NULL, *err = NULL;
    if (!g_subprocess_communicate_finish(G_SUBPROCESS(src), res, &out, &err, NULL)) return;  // cancelled
    gsize n;
    char *text = g_strndup(g_bytes_get_data(out, &n), n);
    GArray *files = g_array_new(FALSE, FALSE, sizeof(Entry));
    gint64 total = 0;
    // bsdtar: mode links owner group size month day time name; GNU tar: mode owner/group size date time name
    int want = gnu_tar ? 6 : 9, at = gnu_tar ? 2 : 4;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *f[9];
        if (fields(line, f, want) != want || f[0][0] == 'd') continue;
        char *end;
        gint64 size = g_ascii_strtoll(f[at], &end, 10);
        if (*end) continue;
        Entry e = {f[want - 1], size};
        g_array_append_val(files, e);
        total += size;
    }
    if (!files->len) {
        char *log = g_strndup(g_bytes_get_data(err, &n), n), *last = "", *line;
        for (line = strtok(log, "\n"); line; line = strtok(NULL, "\n")) last = line;
        if (g_str_has_prefix(last, "tar: ")) last += 5;
        if (g_str_has_prefix(last, "bsdtar: ")) last += 8;
        char *msg = g_strdup_printf("%s%s", *last ? last : "This archive is empty.",
                                    gnu_tar ? " Install bsdtar (libarchive-tools) to read zip, 7z and rar archives." : "");
        archive_error(id, msg);
        g_free(msg), g_free(log);
    } else {
        g_array_sort(files, by_size);
        GString *json = g_string_new(NULL);
        g_string_printf(json, "{\"count\":%u,\"total\":%" G_GINT64_FORMAT ",\"entries\":[", files->len, total);
        for (guint i = 0; i < files->len && i < 500; i++) {
            Entry *e = &g_array_index(files, Entry, i);
            g_string_append(json, i ? ",[" : "[");
            js_str(json, e->name);
            g_string_append_printf(json, ",%" G_GINT64_FORMAT "]", e->size);
        }
        g_string_append(json, "]}");
        call_page("evictArchive", id, json);
    }
    g_array_free(files, TRUE);
    g_free(text);
    g_bytes_unref(out), g_bytes_unref(err);
}

// bsdtar (libarchive) reads zip, rar, 7z, tar.*, iso, cpio…; GNU tar only tar archives
static void list_archive(int id, const char *path) {
    cancel();
    char *tool = g_find_program_in_path("bsdtar");
    gnu_tar = !tool;
    const char *argv[] = {tool ? tool : "tar", "-tvf", path, NULL};
    GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_setenv(l, "LC_ALL", "C.UTF-8", TRUE);  // names in UTF-8, not escaped
    GError *e = NULL;
    GSubprocess *p = g_subprocess_launcher_spawnv(l, argv, &e);
    g_object_unref(l);
    g_free(tool);
    if (!p) {
        archive_error(id, e->message);
        g_error_free(e);
        return;
    }
    running = p;
    job = g_cancellable_new();
    g_subprocess_communicate_async(p, NULL, job, archive_listed, GINT_TO_POINTER(id));
}

// SHA-256 of a whole file, or of its first, middle and last megabyte (sample): enough to tell copies apart without reading them whole
static char *hash_file(const char *path, gboolean sample, GCancellable *c) {
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (!f) return NULL;
    if (fstat(fileno(f), &st)) { fclose(f); return NULL; }
    const off_t chunk = 1 << 20, size = st.st_size;
    GChecksum *h = g_checksum_new(G_CHECKSUM_SHA256);
    guchar *buf = g_malloc(chunk);
    size_t got;
    if (sample && size > 3 * chunk) {
        off_t at[] = {0, size / 2, size - chunk};
        for (int i = 0; i < 3; i++)
            if (!fseeko(f, at[i], SEEK_SET) && (got = fread(buf, 1, chunk, f))) g_checksum_update(h, buf, got);
    } else {
        while (!g_cancellable_is_cancelled(c) && (got = fread(buf, 1, chunk, f))) g_checksum_update(h, buf, got);
    }
    char *key = g_strdup_printf("%lld:%s", (long long)size, g_checksum_get_string(h));
    g_free(buf);
    g_checksum_free(h);
    fclose(f);
    return key;
}

// Paths grouped by the key of each one
static GHashTable *group_by(GPtrArray *paths, gboolean sample, GCancellable *c) {
    GHashTable *by = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < paths->len && !g_cancellable_is_cancelled(c); i++) {
        char *key = hash_file(paths->pdata[i], sample, c);
        if (!key) continue;
        GPtrArray *same = g_hash_table_lookup(by, key);
        if (!same) g_hash_table_insert(by, g_strdup(key), same = g_ptr_array_new());
        g_ptr_array_add(same, paths->pdata[i]);
        g_free(key);
    }
    return by;
}

// Candidate sets from the page (same size and type) checked for real: the sample narrows them down, then copies
// are confirmed on their whole content. Private sizes are unknown on Linux (-1): reflinks aren't told apart.
static void compare(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src;
    GPtrArray *groups = data;
    GString *json = g_string_new("{\"sets\":[");
    gboolean first = TRUE;
    for (guint g = 0; g < groups->len && !g_cancellable_is_cancelled(c); g++) {
        GHashTable *samples = group_by(groups->pdata[g], TRUE, c);
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, samples);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            GPtrArray *same = value;
            if (same->len < 2) continue;
            GHashTable *contents = group_by(same, FALSE, c);
            GHashTableIter it2;
            g_hash_table_iter_init(&it2, contents);
            while (g_hash_table_iter_next(&it2, &key, &value)) {
                GPtrArray *copies = value;
                if (copies->len < 2) continue;
                g_string_append(json, first ? "[" : ",[");
                first = FALSE;
                for (guint i = 0; i < copies->len; i++) {
                    g_string_append(json, i ? ",[" : "[");
                    js_str(json, copies->pdata[i]);
                    g_string_append(json, ",-1]");
                }
                g_string_append_c(json, ']');
            }
            g_hash_table_unref(contents);
        }
        g_hash_table_unref(samples);
    }
    g_string_append(json, "]}");
    g_task_return_pointer(task, g_string_free(json, FALSE), g_free);
}

static void compared(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    GTask *task = G_TASK(res);
    char *json = g_task_propagate_pointer(task, NULL);
    if (json && !g_cancellable_is_cancelled(g_task_get_cancellable(task))) call_page("evictDupes", GPOINTER_TO_INT(data), g_string_new(json));
    g_free(json);
}

static void find_duplicates(int id, GPtrArray *groups) {
    cancel();
    job = g_cancellable_new();
    GTask *task = g_task_new(NULL, job, compared, GINT_TO_POINTER(id));
    g_task_set_task_data(task, groups, (GDestroyNotify)g_ptr_array_unref);
    g_task_run_in_thread(task, compare);
    g_object_unref(task);
}

// ---- Background scans: a systemd user timer runs "evict --background", which posts the scanner's alert, if any

static char *unit_file(const char *name) { return g_build_filename(home, ".config/systemd/user", name, NULL); }

static gboolean systemctl(const char *a, const char *b, const char *c) {
    const char *argv[] = {"systemctl", "--user", a, b, c, NULL};
    int status = 1;
    return g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, NULL, NULL, &status, NULL) && status == 0;
}

// Read through the link enabling the timer: a timer that turned itself off (app uninstalled) reads as off
static void send_schedule(gboolean failed) {
    char *path = unit_file("timers.target.wants/evict-scan.timer"), *text = NULL;
    g_file_get_contents(path, &text, NULL, NULL);
    GString *js = g_string_new(NULL);
    g_string_printf(js, "window.evictSchedule && evictSchedule('%s'%s)", !text ? "off" : strstr(text, "OnCalendar=Mon") ? "week" : "day",
                    failed ? ", 'Background scans need systemd'" : "");
    run_js(js);
    g_free(path), g_free(text);
}

// Around midday; Persistent runs a missed scan at the next login. The service runs Evict through sh so that once
// Evict is uninstalled, the timer turns itself off
static void set_schedule(const char *every) {
    char *timer = unit_file("evict-scan.timer"), *service = unit_file("evict-scan.service");
    gboolean on = !strcmp(every, "day") || !strcmp(every, "week"), ok = TRUE;
    if (on) {
        char *exe = g_file_read_link("/proc/self/exe", NULL), *quoted = g_strescape(exe ? exe : "", NULL), *units = g_path_get_dirname(timer);
        char *s = g_strdup_printf("[Unit]\nDescription=Evict background scan\n\n[Service]\nType=oneshot\nNice=19\nIOSchedulingClass=idle\n"
                                  "ExecStart=/bin/sh -c 'if [ -x \"$$0\" ]; then exec \"$$0\" --background; fi; "
                                  "systemctl --user disable --now evict-scan.timer' \"%s\"\n", quoted);
        char *t = g_strdup_printf("[Unit]\nDescription=Evict background scan\n\n[Timer]\nOnCalendar=%s 12:00\nRandomizedDelaySec=1h\n"
                                  "Persistent=true\n\n[Install]\nWantedBy=timers.target\n", strcmp(every, "week") ? "*-*-*" : "Mon *-*-*");
        ok = !g_mkdir_with_parents(units, 0755) && g_file_set_contents(service, s, -1, NULL) && g_file_set_contents(timer, t, -1, NULL) &&
             systemctl("daemon-reload", NULL, NULL) && systemctl("enable", "--now", "evict-scan.timer");
        g_free(exe), g_free(quoted), g_free(units), g_free(s), g_free(t);
    }
    if ((!on || !ok) && g_file_test(timer, G_FILE_TEST_EXISTS)) {
        systemctl("disable", "--now", "evict-scan.timer");
        unlink(timer), unlink(service);
        systemctl("daemon-reload", NULL, NULL);
    }
    g_free(timer), g_free(service);
    send_schedule(!ok);
}

static guint32 note;

static void note_signal(GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
                        GVariant *args, gpointer loop) {
    (void)bus, (void)sender, (void)path, (void)iface;
    guint32 id;
    g_variant_get_child(args, 0, "u", &id);
    gboolean click = !strcmp(signal, "ActionInvoked");
    if (id != note || (!click && strcmp(signal, "NotificationClosed"))) return;  // KDE's ActivationToken comes first
    if (click) {
        // In a unit of its own: this service's processes are killed when it ends, and run at idle priority
        char *exe = g_file_read_link("/proc/self/exe", NULL);
        const char *run[] = {"systemd-run", "--user", "--quiet", "--collect", exe, NULL}, *direct[] = {exe, NULL};
        int status = 1;
        if (!g_spawn_sync(NULL, (char **)run, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL) || status)
            g_spawn_async(NULL, (char **)direct, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
        g_free(exe);
    }
    g_main_loop_quit(loop);
}

static gboolean stop_waiting(gpointer loop) {
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

// No window: a scan of the disk, then a notification when the scanner has something to say. Without a session bus or
// a notification server, the scan is still kept in the history
static int background(void) {
    setpriority(PRIO_PROCESS, 0, 19);
    char *scanner = g_build_filename(dir, "evict-scan", NULL), *text = NULL;
    const char *argv[] = {scanner, "--alert", "--home", home, "/", NULL};
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &text, NULL, NULL, NULL);
    char *body = text ? g_utf8_make_valid(g_strstrip(text), -1) : NULL;  // folder names can be any bytes, D-Bus wants UTF-8
    GDBusConnection *bus = body && *body ? g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL) : NULL;
    if (!bus) return 0;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_dbus_connection_signal_subscribe(bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications", NULL,
                                       "/org/freedesktop/Notifications", NULL, G_DBUS_SIGNAL_FLAGS_NONE, note_signal, loop, NULL);
    const char *actions[] = {"default", "Open Evict", NULL};
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                              "org.freedesktop.Notifications", "Notify",
                                              g_variant_new("(susss^asa{sv}i)", "Evict", 0, APP_ID, "Evict", body, actions, NULL, -1),
                                              G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    if (!r) return 0;
    g_variant_get(r, "(u)", &note);
    g_variant_unref(r);
    g_timeout_add_seconds(600, stop_waiting, loop);
    g_main_loop_run(loop);
    return 0;
}

// ---- The bridge

static void on_message(WebKitUserContentManager *m, WebKitJavascriptResult *r, gpointer u) {
    (void)m, (void)u;
    JSCValue *body = webkit_javascript_result_get_js_value(r);
    char *action = jsc_value_is_object(body) ? get_str(body, "action") : NULL;
    if (!action) return;
    char *path = get_str(body, "path");
    int id = (int)get_num(body, "id");
    if (!strcmp(action, "copy")) {
        char *text = get_str(body, "text");
        if (text) gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
        g_free(text);
    } else if (!strcmp(action, "reveal") || !strcmp(action, "preview")) {
        GPtrArray *paths = get_strs(body, "paths");
        if (path) g_ptr_array_add(paths, g_strdup(path));
        if (action[0] == 'r') reveal(paths); else preview(paths);
        g_ptr_array_unref(paths);
    } else if (!strcmp(action, "open") && path) {
        open_path(path);
    } else if ((!strcmp(action, "delete") && path) || !strcmp(action, "deleteMany")) {
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
        GArray *sizes = g_array_new(FALSE, FALSE, sizeof(double));
        if (path) {
            double size = get_num(body, "size");
            g_ptr_array_add(paths, g_strdup(path));
            g_array_append_val(sizes, size);
        }
        JSCValue *items = jsc_value_object_get_property(body, "items");
        for (int i = 0, n = length_of(items); i < n; i++) {
            JSCValue *item = jsc_value_object_get_property_at_index(items, i);
            JSCValue *p = jsc_value_object_get_property_at_index(item, 0), *s = jsc_value_object_get_property_at_index(item, 1);
            if (jsc_value_is_string(p)) {
                double size = jsc_value_is_number(s) ? jsc_value_to_double(s) : 0;
                g_ptr_array_add(paths, jsc_value_to_string(p));
                g_array_append_val(sizes, size);
            }
            g_object_unref(p), g_object_unref(s), g_object_unref(item);
        }
        g_object_unref(items);
        confirm_delete(paths, sizes);
        g_ptr_array_unref(paths);
        g_array_free(sizes, TRUE);
    } else if (!strcmp(action, "emptyTrash") && path) {
        confirm_empty_trash(path, get_num(body, "size"));
    } else if (!strcmp(action, "archive") && path) {
        list_archive(id, path);
    } else if (!strcmp(action, "dupes")) {
        GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
        JSCValue *a = jsc_value_object_get_property(body, "groups");
        for (int i = 0, n = length_of(a); i < n; i++) {
            JSCValue *g = jsc_value_object_get_property_at_index(a, i);
            g_ptr_array_add(groups, strs_of(g));
            g_object_unref(g);
        }
        g_object_unref(a);
        find_duplicates(id, groups);
    } else if (!strcmp(action, "ask")) {
        char *prompt = get_str(body, "prompt");
        if (prompt) ask(id, prompt);
        g_free(prompt);
    } else if (!strcmp(action, "cancel")) {
        cancel();
    } else if (!strcmp(action, "ready")) {
        send_schedule(FALSE);
    } else if (!strcmp(action, "schedule")) {
        char *every = get_str(body, "every");
        if (every) set_schedule(every);
        g_free(every);
    }
    g_free(path);
    g_free(action);
}

// ---- Scanning

static char *read_template(void) {
    char *path = g_build_filename(dir, "index.html", NULL), *html = NULL;
    g_file_get_contents(path, &html, NULL, NULL);
    g_free(path);
    return html && strstr(html, PLACEHOLDER) ? html : (g_free(html), NULL);
}

static void load_file(const char *path) {
    char *uri = g_filename_to_uri(path, NULL, NULL);
    webkit_web_view_load_uri(view, uri);
    g_free(uri);
}

static void show_failure(const char *message) {
    char *html = g_markup_printf_escaped(
        "<body style='margin:0;height:100vh;display:grid;place-items:center;background:#0c0e12;color:#e8eaf0;"
        "font:15px system-ui,sans-serif'><div style='text-align:center;max-width:520px'><h2>The scan failed</h2>"
        "<p style='color:#8b93a1'>%s</p><p style='color:#8b93a1'>Press Rescan to try again.</p></div></body>", message);
    webkit_web_view_load_html(view, html, NULL);
    g_free(html);
}

static void set_scanning(gboolean on) {
    scanning = on;
    g_simple_action_set_enabled(rescan_action, !on);
    g_simple_action_set_enabled(admin_action, !on);
}

// With --progress the scanner writes a JSON progress line on stderr every 300 ms, for the scan screen
static void scan_progress(GObject *src, GAsyncResult *res, gpointer data) {
    GDataInputStream *in = G_DATA_INPUT_STREAM(src);
    char *line = g_data_input_stream_read_line_finish(in, res, NULL, NULL);
    if (!line) { g_object_unref(in); return; }
    if (line[0] == '{') {
        GString *js = g_string_new(NULL);
        g_string_printf(js, "window.evictLive && evictLive(%s)", line);
        run_js(js);
    } else if (*g_strstrip(line)) {
        g_free(scan_error);
        scan_error = g_strdup(line);
    }
    g_free(line);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, scan_progress, data);
}

static void scan_done(GObject *src, GAsyncResult *res, gpointer data) {
    GSubprocess *p = G_SUBPROCESS(src);
    char *json_path = data, *json = NULL;
    gsize len = 0;
    gboolean ok = g_subprocess_wait_check_finish(p, res, NULL);
    int status = g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
    set_scanning(FALSE);
    char *html = read_template();
    if (ok && html && g_file_get_contents(json_path, &json, &len, NULL) && len) {
        char *slot = strstr(html, PLACEHOLDER);
        *slot = 0;
        char *full = g_strconcat(html, "window.DATA=", json, slot + strlen(PLACEHOLDER), NULL);
        g_free(page);
        page = g_build_filename(cache, "evict.html", NULL);
        if (g_file_set_contents(page, full, -1, NULL)) load_file(page);
        else show_failure("Couldn’t write the report in ~/.cache/evict.");
        g_free(full);
    } else if (admin_scan && (status == 126 || status == 127) && page) {
        load_file(page);  // password prompt dismissed: back to the last map
    } else {
        char *msg = !html ? g_strdup("App files are missing, please reinstall Evict.")
                          : g_strdup_printf("The scanner stopped (code %d). %s", status, scan_error ? scan_error : "");
        show_failure(msg);
        g_free(msg);
    }
    unlink(json_path);
    g_free(json), g_free(html), g_free(json_path);
    g_clear_pointer(&scan_error, g_free);
    g_object_unref(p);
}

// As administrator, the scanner runs through pkexec and reads what this user can't (Docker, system logs, other homes)
static void scan(gboolean admin) {
    if (scanning) return;
    char *html = read_template();
    if (!html) return show_failure("App files are missing, please reinstall Evict.");
    set_scanning(TRUE);
    admin_scan = admin;
    char *slot = strstr(html, PLACEHOLDER), *live_path = g_build_filename(cache, "evict-live.html", NULL);
    *slot = 0;
    char *live = g_strconcat(html, "window.DATA = null; window.LIVE = {root: '/', os: 'linux'};", slot + strlen(PLACEHOLDER), NULL);
    if (g_file_set_contents(live_path, live, -1, NULL)) load_file(live_path);
    g_free(live), g_free(live_path), g_free(html);

    char *scanner = g_build_filename(dir, "evict-scan", NULL), *json_path = g_build_filename(cache, "scan.json", NULL);
    const char *user[] = {scanner, "--progress", "--history", "--home", home, "/", NULL};
    const char *root[] = {"pkexec", scanner, "--progress", "--history", "--home", home, "/", NULL};
    GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_stdout_file_path(l, json_path);
    GError *e = NULL;
    GSubprocess *p = g_subprocess_launcher_spawnv(l, admin ? root : user, &e);
    g_object_unref(l);
    g_free(scanner);
    if (!p) {
        set_scanning(FALSE);
        show_failure(e->message);
        g_error_free(e);
        g_free(json_path);
        return;
    }
    GDataInputStream *in = g_data_input_stream_new(g_subprocess_get_stderr_pipe(p));
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, scan_progress, NULL);
    g_subprocess_wait_check_async(p, NULL, scan_done, json_path);
}

// ---- Updates: Linux packages come from GitHub releases, the install command updates them

static void prompt_update(void) {
    if (!latest) return;
    char *title = g_strdup_printf("Evict %s is available", latest);
    char *text = g_strdup_printf("You’re running version %s. To update, run the install command again in a terminal:\n\n%s",
                                 VERSION, INSTALL);
    if (dialog(GTK_MESSAGE_INFO, title, text, "Later", GTK_RESPONSE_CANCEL, "Copy Command", R_COPY, NULL) == R_COPY)
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), INSTALL, -1);
    g_free(title), g_free(text);
}

// Releases come newest first: the first one with Linux packages is the latest for Linux, even while its DMG waits on Apple
static char *newest_linux_version(char *json) {
    for (char *t = strstr(json, "\"tag_name\""); t;) {
        char *next = strstr(t + 1, "\"tag_name\""), *v = strchr(t + 10, ':');
        if (next) *next = 0;
        gboolean has = strstr(t, "evict-linux-") != NULL;
        if (next) *next = '"';
        if (has && v && (v = strchr(v, '"'))) {
            v += v[1] == 'v' ? 2 : 1;
            return g_strndup(v, strcspn(v, "\""));
        }
        t = next;
    }
    return NULL;
}

static void update_checked(GObject *src, GAsyncResult *res, gpointer data) {
    gboolean manual = GPOINTER_TO_INT(data);
    GBytes *out = NULL;
    gboolean ok = g_subprocess_communicate_finish(G_SUBPROCESS(src), res, &out, NULL, NULL) &&
                  g_subprocess_get_successful(G_SUBPROCESS(src));
    gsize n = 0;
    char *json = out ? g_strndup(g_bytes_get_data(out, &n), n) : NULL, *version = ok ? newest_linux_version(json) : NULL;
    if (!ok) {
        if (manual) inform("Couldn’t check for updates", "GitHub isn’t responding, try again later.");
    } else if (version && strverscmp(version, VERSION) > 0) {
        g_free(latest);
        latest = g_strdup(version);
        char *label = g_strdup_printf("Update to %s", latest);
        gtk_button_set_label(GTK_BUTTON(update_btn), label);
        gtk_widget_show(update_btn);
        g_free(label);
        // Each new version is announced once; the header bar button stays as the reminder
        char *seen_path = g_build_filename(cache, "announced", NULL), *seen = NULL;
        g_file_get_contents(seen_path, &seen, NULL, NULL);
        if (manual || g_strcmp0(seen, latest)) {
            g_file_set_contents(seen_path, latest, -1, NULL);
            prompt_update();
        }
        g_free(seen), g_free(seen_path);
    } else if (manual) {
        char *text = g_strdup_printf("You’re running the latest version (%s).", VERSION);
        inform("Evict is up to date", text);
        g_free(text);
    }
    g_free(version), g_free(json);
    if (out) g_bytes_unref(out);
}

static void check_updates(gboolean manual) {
    const char *argv[] = {"curl", "-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json", RELEASES, NULL};
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (!p) {
        if (manual) inform("Couldn’t check for updates", "Evict needs curl to reach GitHub.");
        return;
    }
    g_subprocess_communicate_async(p, NULL, NULL, update_checked, GINT_TO_POINTER(manual));
    g_object_unref(p);
}

static gboolean periodic_check(gpointer data) {
    (void)data;
    check_updates(FALSE);
    return G_SOURCE_CONTINUE;
}

// ---- App

// GUI sessions don't always get the shell's PATH (npm prefixes, nvm): look where installers put the CLI, then ask a login shell
static char *find_claude(void) {
    char *p = g_find_program_in_path("claude");
    if (p) return p;
    const char *known[] = {".local/bin/claude", ".claude/local/claude", ".npm-global/bin/claude", NULL};
    for (int i = 0; known[i]; i++) {
        p = g_build_filename(home, known[i], NULL);
        if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE)) return p;
        g_free(p);
    }
    const char *shell = g_getenv("SHELL") ? g_getenv("SHELL") : "/bin/sh";
    const char *argv[] = {shell, "-lc", "command -v claude", NULL};
    char *out = NULL;
    if (g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL) && out &&
        g_strstrip(out)[0] == '/' && g_file_test(out, G_FILE_TEST_IS_EXECUTABLE))
        return out;
    g_free(out);
    return NULL;
}

// "fr_FR.UTF-8" → "fr", for the language Claude answers in
static char *language(void) {
    const char *l = g_get_language_names()[0];
    size_t n = 0;
    while (g_ascii_isalpha(l[n])) n++;
    return n && strcmp(l, "C") && strcmp(l, "POSIX") ? g_strndup(l, n) : g_strdup("en");
}

// Claude Code's stream-json lines, read by the page: text deltas stream into the panel, the result line says how it ended
static const char *claude_script =
    "(() => { const st = {};"
    " window.evictClaudeLine = (id, line) => { const s = st[id] ?\?= {done: false, problem: ''}; let e;"
    "  try { e = JSON.parse(line) } catch { s.problem += line + '\\n'; return }"
    "  const t = e.event?.delta?.text;"
    "  if (t) evictAnswer(id, t, 'delta');"
    "  else if (e.type === 'result') { s.done = e.is_error !== true; if (!s.done) s.problem += e.result || '' } };"
    " window.evictClaudeEnd = id => { const s = st[id] || {}; delete st[id]; evictAnswer(id, s.problem || '', s.done ? 'done' : 'error') };"
    "})();";

static void on_action(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action, (void)param;
    const char *name = data;
    if (!strcmp(name, "rescan")) scan(FALSE);
    else if (!strcmp(name, "rescan-admin")) scan(TRUE);
    else check_updates(TRUE);
}

static void activate(GtkApplication *app) {
    if (win) return gtk_window_present(GTK_WINDOW(win));
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    // The header bar in the page's color, so the window reads as one surface
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "headerbar { background: #0c0e12; border-color: #0c0e12; box-shadow: none; color: #e8eaf0; }"
                                         "headerbar button { background: none; border-color: transparent; box-shadow: none; color: #e8eaf0;"
                                         " text-shadow: none; -gtk-icon-shadow: none; }"
                                         "headerbar button:hover { background: #1f2430; }"
                                         "headerbar button.suggested-action { background: #e0473d; color: #fff; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Evict");
    gtk_window_set_default_size(GTK_WINDOW(win), 1280, 820);
    gtk_window_set_icon_name(GTK_WINDOW(win), APP_ID);

    GtkWidget *bar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(bar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(bar), "Evict");
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Rescan as Administrator…", "app.rescan-admin");
    g_menu_append(menu, "Check for Updates…", "app.updates");
    GtkWidget *more = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(more), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(more), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(more, "More");
    g_object_unref(menu);
    GtkWidget *rescan = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(rescan), "app.rescan");
    gtk_widget_set_tooltip_text(rescan, "Rescan the disk (Ctrl+R)");
    update_btn = gtk_button_new_with_label("");
    gtk_style_context_add_class(gtk_widget_get_style_context(update_btn), "suggested-action");
    g_signal_connect(update_btn, "clicked", G_CALLBACK(prompt_update), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), more);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), rescan);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), update_btn);
    gtk_window_set_titlebar(GTK_WINDOW(win), bar);

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    g_signal_connect(ucm, "script-message-received::evict", G_CALLBACK(on_message), NULL);
    webkit_user_content_manager_register_script_message_handler(ucm, "evict");
    char *lang = language();
    char *flags = g_strdup_printf("window.evictClaude = %s; window.evictLang = '%s'; %s", claude ? "true" : "false", lang, claude_script);
    WebKitUserScript *script = webkit_user_script_new(flags, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    g_free(flags), g_free(lang);
    view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
    g_object_unref(ucm);
    webkit_web_view_set_background_color(view, &(GdkRGBA){0.047, 0.055, 0.071, 1});  // no white flash before the page paints
    g_signal_connect(view, "context-menu", G_CALLBACK(gtk_true), NULL);              // the page has its own menus
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(view));
    gtk_widget_show_all(win);
    gtk_widget_hide(update_btn);

    scan(FALSE);
    check_updates(FALSE);
    g_timeout_add_seconds(6 * 3600, periodic_check, NULL);
}

static regex_t compile(const char *pattern, int flags) {
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB | flags)) g_error("bad pattern %s", pattern);
    return re;
}

#define HOME_RE "^(/(Users|home)/[^/]+|/root)"

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--version")) return puts("Evict " VERSION), 0;
    // shortcut: WebKitGTK's DMA-BUF renderer draws a blank window on some GPU drivers (NVIDIA); the page is light enough without it
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", FALSE);
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    dir = exe ? g_path_get_dirname(exe) : g_strdup("/usr/lib/evict");
    g_free(exe);
    home = g_strdup(g_get_home_dir());
    if (argc > 1 && !strcmp(argv[1], "--background")) return background();
    trash_dir = g_build_filename(g_get_user_data_dir(), "Trash", NULL);
    trash_files = g_build_filename(trash_dir, "files", NULL);
    cache = g_build_filename(g_get_user_cache_dir(), "evict", NULL);
    g_mkdir_with_parents(cache, 0700);
    re_bad = compile("(^|/)\\.\\.?(/|$)|//|/$|^/[^/]*$", 0);
    re_sys = compile("^/(System|private|usr|bin|sbin|cores|proc|sys|dev|run|boot|etc|lib|lib32|lib64|libx32|snap)(/|$)", 0);
    re_homes = compile(HOME_RE "(/Library|/\\.Trash|/\\.config|/\\.local|/\\.local/share|/\\.local/share/Trash(/files)?)?$", 0);
    re_secrets = compile(HOME_RE "/(\\.ssh|\\.gnupg|\\.aws|\\.azure|\\.kube|\\.password-store|\\.config/gcloud|"
                                 "Library/Keychains|\\.local/share/keyrings)(/|$)", 0);
    re_secret = compile("^(\\.env(\\..+)?|id_(rsa|dsa|ecdsa|ed25519)|\\.npmrc|\\.netrc|\\.pgpass|credentials(\\.json)?|"
                        ".+\\.(pem|key|p12|pfx|keystore|jks|kdbx|ovpn))$", REG_ICASE);
    re_example = compile("\\.(example|sample|template)$", 0);
    re_heavy = compile("^(node_modules|\\.git|vendor|Pods|DerivedData|\\.venv|venv|__pycache__|\\.next|target)$", 0);
    claude = find_claude();
    if (!load_webkit()) {
        gtk_init(&argc, &argv);
        GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Evict needs WebKitGTK");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Install libwebkit2gtk-4.1 (or 4.0) with your package manager, "
                                                 "or run the install command again: it brings it along.");
        gtk_dialog_run(GTK_DIALOG(d));
        return 1;
    }

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_FLAGS_NONE);
    const char *actions[] = {"rescan", "rescan-admin", "updates"};
    for (int i = 0; i < 3; i++) {
        GSimpleAction *a = g_simple_action_new(actions[i], NULL);
        g_signal_connect(a, "activate", G_CALLBACK(on_action), (gpointer)actions[i]);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(a));
        if (i == 0) rescan_action = a;
        else if (i == 1) admin_action = a;
        else g_object_unref(a);
    }
    gtk_application_set_accels_for_action(app, "app.rescan", (const char *[]){"<Control>r", NULL});
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
