/*
 * NanumPlayer: the optional Blu-ray/DVD decryption component (Windows).
 * See nanum-disc-component.h.
 *
 * Threads: commands and publishing run on the core thread. One worker does the
 * network, file and hash work (download or local ZIP, unpacking, verification) and
 * talks to the core only by enqueueing a mergeable publication on its dispatch
 * queue. nanum_disc_component_ready() runs on whatever thread opens a disc.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mpv/client.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "misc/dispatch.h"
#include "misc/json.h"
#include "misc/node.h"
#include "options/m_property.h"
#include "player/command.h"
#include "player/core.h"
#include "osdep/io.h"
#include "osdep/nanum-disc-component.h"
#include "osdep/nanum-download.h"
#include "osdep/threads.h"
#include "nanum/i18n.h"

#define PROGRESS_MS 200
#define MAX_FILES 40
#define MAX_ZIP_SIZE (INT64_C(64) << 20)
#define MAX_FILE_SIZE (INT64_C(32) << 20)
#define CONSENT_FILE "keydb-consent"

// The libraries the player loads; every catalog must list them.
static const char *const RUNTIME_FILES[] = {"libaacs.dll", "libbdplus.dll", "libdvdcss-2.dll"};

struct pinned_file {
    char name[96];          // path inside the ZIP and the folder, '/' separated
    int64_t size;
    char sha[65];
};

struct catalog {
    bool ok;
    char err[512];
    char version[40];
    struct nanum_dl_item zip;
    int nfiles;
    struct pinned_file files[MAX_FILES];
};

enum state {
    ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING, ST_EXTRACTING, ST_DONE, ST_CANCELLED,
    ST_ERROR,
};
static const char *const STATE_NAME[] = {
    "idle", "checking", "downloading", "verifying", "extracting", "done", "cancelled", "error",
};

enum inst { INST_UNKNOWN, INST_YES, INST_NO };

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;
static mp_static_mutex g_verify_lock = MP_STATIC_MUTEX_INITIALIZER;

static struct {
    // Written once under g_lock (strings are never freed).
    bool paths;
    char *root;
    char *exe_dir;
    char *base;                 // ...\components\disc (NULL: unknown)
    char *dir;                  // base\<version> (NULL: unknown or no catalog)
    struct catalog cat;

    // Core thread only.
    bool init;
    struct MPContext *mpctx;
    struct mp_log *log;
    bool thread_valid;
    mp_thread thread;
    struct nanum_dl dl;         // the worker's downloader

    // Guarded by g_lock.
    enum inst inst;
    bool running;
    bool shutting_down;
    enum state st;
    int64_t recv, total;
    char err[640];              // stored message form
    ULONGLONG last_pub;
} g;

#define MSG(buf, size, ...) nanum_msg((buf), (size), __VA_ARGS__, (const char *)NULL)

static wchar_t *W(void *ctx, const char *s)
{
    return mp_from_utf8(ctx, s);
}

static char *join(void *ctx, const char *a, const char *b)
{
    return nanum_dl_join(ctx, a, b);
}

static char *rtext(void *ctx, const char *stored)
{
    char *out = talloc_array(ctx, char, 1024);
    nanum_msg_render(stored, out, 1024);
    return out;
}

// '/' -> '\\' in a pinned relative name.
static char *native_name(void *ctx, const char *name)
{
    char *r = talloc_strdup(ctx, name);
    for (char *s = r; *s; s++) {
        if (*s == '/')
            *s = '\\';
    }
    return r;
}

// ---------------------------------------------------------------------------
// Catalog (components\catalog.json, key "disc_components")
// ---------------------------------------------------------------------------

#define cat_fail(c, ...) \
    do { \
        (c)->ok = false; \
        nanum_msg((c)->err, sizeof((c)->err), __VA_ARGS__, (const char *)NULL); \
    } while (0)

static bool name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= sizeof(((struct pinned_file *)0)->name) || s[0] == '.' || s[0] == '/' ||
        s[n - 1] == '/')
        return false;
    int slashes = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '/') {
            // At most one sub folder; "/." would start a hidden/relative part.
            if (++slashes > 1 || s[i + 1] == '.' || s[i + 1] == '/')
                return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                     (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-' || c == '+'))
        {
            return false;
        }
    }
    return !strstr(s, "..");
}

static bool version_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= sizeof(((struct catalog *)0)->version))
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '.' || c == '-' || c == '_'))
            return false;
    }
    return s[0] != '.';
}

static bool hex64(const char *s, char *out)
{
    if (!s || strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'F')
            c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        out[i] = c;
    }
    out[64] = 0;
    return true;
}

static void read_catalog(struct catalog *c)
{
    memset(c, 0, sizeof(*c));
    void *tmp = talloc_new(NULL);
    const char *dirs[2] = {g.root, g.exe_dir};
    char *text = NULL, *path = NULL;
    for (int i = 0; i < 2 && !text; i++) {
        if (!dirs[i] || (i == 1 && g.root && nanum_dl_same_path(dirs[0], dirs[1])))
            continue;
        path = join(tmp, join(tmp, dirs[i], "components"), "catalog.json");
        text = nanum_dl_read_text(tmp, path);
    }
    if (!text) {
        cat_fail(c, N_("the component catalog components\\catalog.json is missing or "
                       "unreadable"));
        goto done;
    }
    char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    mpv_node doc;
    if (json_parse(tmp, &doc, &p, MAX_JSON_DEPTH) < 0 || doc.format != MPV_FORMAT_NODE_MAP) {
        cat_fail(c, N_("the component catalog %s is not valid JSON"), path);
        goto done;
    }
    mpv_node *d = node_map_get(&doc, "disc_components");
    if (!d || d->format != MPV_FORMAT_NODE_MAP) {
        cat_fail(c, N_("the component catalog has no entry for '%s'"), "disc_components");
        goto done;
    }
    const char *version = nanum_dl_node_str(d, "version");
    char why[256];
    if (!version_ok(version)) {
        cat_fail(c, N_("catalog entry '%s' is invalid"), "disc_components");
        goto done;
    }
    snprintf(c->version, sizeof(c->version), "%s", version);
    if (!nanum_dl_parse_item(d, "Blu-ray/DVD decryption components", false, &c->zip, why,
                             sizeof(why)) || c->zip.size > MAX_ZIP_SIZE)
    {
        cat_fail(c, N_("catalog entry '%s' is invalid"), "disc_components");
        goto done;
    }
    mpv_node *files = node_map_get(d, "files");
    if (!files || files->format != MPV_FORMAT_NODE_ARRAY || files->u.list->num <= 0 ||
        files->u.list->num > MAX_FILES)
    {
        cat_fail(c, N_("catalog entry '%s' is invalid"), "disc_components");
        goto done;
    }
    for (int i = 0; i < files->u.list->num; i++) {
        mpv_node *m = &files->u.list->values[i];
        const char *name = m->format == MPV_FORMAT_NODE_MAP ? nanum_dl_node_str(m, "name") : NULL;
        int64_t size = 0;
        struct pinned_file *f = &c->files[c->nfiles];
        if (!name || !name_ok(name) || !nanum_dl_node_int(m, "size", &size) || size <= 0 ||
            size > MAX_FILE_SIZE || !hex64(nanum_dl_node_str(m, "sha256"), f->sha))
        {
            cat_fail(c, N_("catalog entry '%s' is invalid"), "disc_components");
            goto done;
        }
        for (int k = 0; k < c->nfiles; k++) {
            if (!_stricmp(c->files[k].name, name)) {
                cat_fail(c, N_("catalog entry '%s' is invalid"), "disc_components");
                goto done;
            }
        }
        snprintf(f->name, sizeof(f->name), "%s", name);
        f->size = size;
        c->nfiles++;
    }
    for (size_t r = 0; r < MP_ARRAY_SIZE(RUNTIME_FILES); r++) {
        bool found = false;
        for (int k = 0; k < c->nfiles; k++)
            found |= !strcmp(c->files[k].name, RUNTIME_FILES[r]);
        if (!found) {
            cat_fail(c, N_("catalog entry '%s' is invalid"), "disc_components");
            goto done;
        }
    }
    c->ok = true;
done:
    talloc_free(tmp);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

static void ensure_init(const char *root)
{
    mp_mutex_lock(&g_lock);
    if (g.paths) {
        mp_mutex_unlock(&g_lock);
        return;
    }
    void *tmp = talloc_new(NULL);
    wchar_t env[MAX_PATH * 2];
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MP_ARRAY_SIZE(exe));
    if (n > 0 && n < MP_ARRAY_SIZE(exe)) {
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash)
            *slash = 0;
        g.exe_dir = mp_to_utf8(NULL, exe);
    }
    if (root) {
        g.root = talloc_strdup(NULL, root);
    } else {
        n = GetEnvironmentVariableW(L"NANUMPLAYER_ROOT", env, MP_ARRAY_SIZE(env));
        if (n > 0 && n < MP_ARRAY_SIZE(env))
            g.root = mp_to_utf8(NULL, env);
        else if (g.exe_dir)
            g.root = talloc_strdup(NULL, g.exe_dir);
    }
    if (g.root)
        nanum_dl_trim_slashes(g.root);

    char *base = NULL;
    n = GetEnvironmentVariableW(L"NANUMPLAYER_DISC_COMPONENT_DIR", env, MP_ARRAY_SIZE(env));
    if (n > 0 && n < MP_ARRAY_SIZE(env)) {
        base = mp_to_utf8(tmp, env);
        nanum_dl_trim_slashes(base);
    } else {
        wchar_t *local = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) {
            base = talloc_asprintf(tmp, "%s\\NanumPlayer\\components\\disc",
                                   mp_to_utf8(tmp, local));
            CoTaskMemFree(local);
        }
    }
    if (base)
        g.base = talloc_strdup(NULL, base);
    read_catalog(&g.cat);
    if (g.cat.ok && g.base)
        g.dir = talloc_asprintf(NULL, "%s\\%s", g.base, g.cat.version);
    g.paths = true;
    talloc_free(tmp);
    mp_mutex_unlock(&g_lock);
}

static bool path_exists(const char *path)
{
    void *tmp = talloc_new(NULL);
    bool r = GetFileAttributesW(W(tmp, path)) != INVALID_FILE_ATTRIBUTES;
    talloc_free(tmp);
    return r;
}

// Folder of the key database and of the consent marker.
char *nanum_disc_keys_dir(void *ctx)
{
    wchar_t env[MAX_PATH * 2];
    char *dir = NULL;
    DWORD n = GetEnvironmentVariableW(L"NANUMPLAYER_DISC_KEYS_DIR", env, MP_ARRAY_SIZE(env));
    if (n && n < MP_ARRAY_SIZE(env)) {
        dir = mp_to_utf8(ctx, env);
    } else {
        char *base = NULL;
        n = GetEnvironmentVariableW(L"APPDATA", env, MP_ARRAY_SIZE(env));
        if (n && n < MP_ARRAY_SIZE(env)) {
            base = mp_to_utf8(ctx, env);
        } else {
            wchar_t *roaming = NULL;
            if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, KF_FLAG_CREATE,
                                               NULL, &roaming)))
                base = mp_to_utf8(ctx, roaming);
            CoTaskMemFree(roaming);
        }
        if (base && base[0])
            dir = talloc_asprintf(ctx, "%s\\NanumPlayer\\disc", base);
    }
    if (!dir || !dir[0])
        return NULL;
    dir = talloc_strdup(ctx, dir);
    for (char *s = dir; *s; s++) {
        if (*s == '/')
            *s = '\\';
    }
    nanum_dl_trim_slashes(dir);
    return dir;
}

// ---------------------------------------------------------------------------
// Consent for the key database download
// ---------------------------------------------------------------------------

bool nanum_disc_keys_consent(void)
{
    void *tmp = talloc_new(NULL);
    bool r = false;
    char *dir = nanum_disc_keys_dir(tmp);
    if (dir) {
        char *text = nanum_dl_read_text(tmp, join(tmp, dir, CONSENT_FILE));
        r = text && text[0] == '1';
    }
    talloc_free(tmp);
    return r;
}

static void publish(struct MPContext *mpctx);

bool nanum_disc_keys_set_consent(struct MPContext *mpctx, bool agreed)
{
    void *tmp = talloc_new(NULL);
    bool ok = false;
    char *dir = nanum_disc_keys_dir(tmp);
    if (dir) {
        char *file = join(tmp, dir, CONSENT_FILE);
        wchar_t *wfile = W(tmp, file);
        if (!agreed) {
            ok = DeleteFileW(wfile) || GetLastError() == ERROR_FILE_NOT_FOUND ||
                 GetLastError() == ERROR_PATH_NOT_FOUND;
        } else {
            int ec = SHCreateDirectoryExW(NULL, W(tmp, dir), NULL);
            if (ec == ERROR_SUCCESS || ec == ERROR_ALREADY_EXISTS || ec == ERROR_FILE_EXISTS) {
                HANDLE h = CreateFileW(wfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD w = 0;
                    ok = WriteFile(h, "1\n", 2, &w, NULL) && w == 2;
                    CloseHandle(h);
                }
            }
        }
    }
    talloc_free(tmp);
    if (mpctx && g.init)
        publish(mpctx);
    return ok;
}

// ---------------------------------------------------------------------------
// Verification and activation
// ---------------------------------------------------------------------------

static bool verify_dir(const char *dir, const struct catalog *cat)
{
    struct nanum_dl dl;
    nanum_dl_init(&dl, NULL, NULL, NULL);
    if (!nanum_dl_begin(&dl))
        return false;
    void *tmp = talloc_new(NULL);
    bool ok = true;
    for (int i = 0; i < cat->nfiles && ok; i++) {
        const struct pinned_file *f = &cat->files[i];
        char *path = join(tmp, dir, native_name(tmp, f->name));
        struct nanum_dl_item item = {.valid = true, .size = f->size};
        snprintf(item.file, sizeof(item.file), "%s", f->name);
        snprintf(item.sha, sizeof(item.sha), "%s", f->sha);
        bool match = false;
        ok = path_exists(path) && nanum_dl_hash(&dl, &item, path, &match) == NANUM_DL_OK && match;
    }
    talloc_free(tmp);
    nanum_dl_end(&dl);
    return ok;
}

static void activate(const char *dir, struct mp_log *log)
{
    void *tmp = talloc_new(NULL);
    // libbluray appends ".dll" and loads these by absolute path.
    _wputenv_s(L"LIBAACS_PATH", W(tmp, join(tmp, dir, "libaacs")));
    _wputenv_s(L"LIBBDPLUS_PATH", W(tmp, join(tmp, dir, "libbdplus")));
    // libdvdread loads libdvdcss by name: a module of that name that is already loaded wins.
    HMODULE css = LoadLibraryExW(W(tmp, join(tmp, dir, "libdvdcss-2.dll")), NULL,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    if (log)
        mp_verbose(log, "Disc decryption component verified: %s%s\n", dir,
                   css ? "" : " (libdvdcss could not be loaded)");
    talloc_free(tmp);
}

bool nanum_disc_component_ready(struct mp_log *log)
{
    ensure_init(NULL);
    mp_mutex_lock(&g_verify_lock);
    mp_mutex_lock(&g_lock);
    bool ok = g.inst == INST_YES;
    char *dir = g.dir;
    struct catalog *cat = &g.cat;     // immutable after ensure_init()
    mp_mutex_unlock(&g_lock);
    if (!ok) {
        ok = dir && path_exists(dir) && verify_dir(dir, cat);
        if (ok)
            activate(dir, log);
        mp_mutex_lock(&g_lock);
        g.inst = ok ? INST_YES : INST_NO;
        mp_mutex_unlock(&g_lock);
    }
    mp_mutex_unlock(&g_verify_lock);
    return ok;
}

void nanum_disc_component_environment(const wchar_t *root)
{
    if (!root)
        return;
    // Nothing exists below this name: without the component libbluray finds no library,
    // whatever is lying around in the application folder.
    size_t n = wcslen(root) + 64;
    wchar_t *none = malloc(n * sizeof(wchar_t));
    if (!none)
        return;
    for (int i = 0; i < 2; i++) {
        swprintf(none, n, L"%ls\\disc\\component-not-installed\\%ls", root,
                 i ? L"libbdplus" : L"libaacs");
        _wputenv_s(i ? L"LIBBDPLUS_PATH" : L"LIBAACS_PATH", none);
    }
    free(none);
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

static void publish_cb(void *p)
{
    if (g.mpctx)
        publish(g.mpctx);
}

static void request_publish(void)
{
    struct MPContext *mpctx = g.mpctx;
    if (mpctx)
        mp_dispatch_enqueue_notify(mpctx->dispatch, publish_cb, &g);
}

static void set_state(enum state st, int64_t recv, int64_t total, const char *err)
{
    mp_mutex_lock(&g_lock);
    g.st = st;
    g.recv = recv;
    g.total = total;
    snprintf(g.err, sizeof(g.err), "%s", err ? err : "");
    g.last_pub = GetTickCount64();
    mp_mutex_unlock(&g_lock);
    request_publish();
}

static void set_progress(int64_t recv, bool force)
{
    ULONGLONG now = GetTickCount64();
    mp_mutex_lock(&g_lock);
    g.recv = recv;
    bool pub = force || now - g.last_pub >= PROGRESS_MS;
    if (pub)
        g.last_pub = now;
    mp_mutex_unlock(&g_lock);
    if (pub)
        request_publish();
}

static void dl_phase(struct nanum_dl *dl, enum nanum_dl_phase ph, int64_t recv, int64_t total)
{
    static const enum state map[] = {ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING};
    set_state(map[ph], recv, total, NULL);
}

static void dl_progress(struct nanum_dl *dl, int64_t recv)
{
    set_progress(recv, false);
}

static void publish(struct MPContext *mpctx)
{
    if (!g.init || !mpctx)
        return;
    void *tmp = talloc_new(NULL);

    mp_mutex_lock(&g_lock);
    enum state st = g.st;
    enum inst inst = g.inst;
    int64_t recv = g.recv, total = g.total;
    bool running = g.running;
    char err[sizeof(g.err)];
    memcpy(err, g.err, sizeof(err));
    mp_mutex_unlock(&g_lock);

    mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_flag(&root, "installed", inst == INST_YES);
    node_map_add_flag(&root, "checked", inst != INST_UNKNOWN);
    node_map_add_string(&root, "version", g.cat.ok ? g.cat.version : "");
    node_map_add_string(&root, "directory", g.dir ? g.dir : "");
    node_map_add_int64(&root, "size", g.cat.ok ? g.cat.zip.size : 0);
    node_map_add_string(&root, "url", g.cat.ok ? g.cat.zip.url : "");
    node_map_add_string(&root, "filename", g.cat.ok ? g.cat.zip.file : "");
    node_map_add_flag(&root, "busy", running);
    node_map_add_string(&root, "status", STATE_NAME[st]);
    node_map_add_int64(&root, "received", recv);
    node_map_add_int64(&root, "total", total);
    node_map_add_string(&root, "error", rtext(tmp, err));
    node_map_add_flag(&root, "consent", nanum_disc_keys_consent());
    node_map_add_flag(&root, "catalog", g.cat.ok);
    node_map_add_string(&root, "catalog_error", g.cat.ok ? "" : rtext(tmp, g.cat.err));
    mp_property_do("user-data/nanum/disc-component", M_PROPERTY_SET_NODE, &root, mpctx);
    talloc_free(root.u.list);
    talloc_free(tmp);
}

// ---------------------------------------------------------------------------
// Worker: ZIP, unpacking, installation
// ---------------------------------------------------------------------------

struct job {
    bool verify_only;
    char *zip;              // local ZIP (NULL: download)
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct zent {
    const uint8_t *data;
    int file;
};

// Strict reader of the one ZIP layout the pinned asset has: stored entries only, no
// encryption, no zip64, no extra volumes, entries exactly the pinned files.
static bool zip_parse(const uint8_t *z, size_t n, const struct catalog *cat, struct zent *out,
                      char *err, size_t errsz)
{
    if (n < 22) {
        MSG(err, errsz, N_("The ZIP file is not a valid component package."));
        return false;
    }
    size_t eocd = SIZE_MAX;
    size_t from = n - 22, low = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t i = from + 1; i-- > low;) {
        if (rd32(z + i) == 0x06054b50 && i + 22 + rd16(z + i + 20) == n) {
            eocd = i;
            break;
        }
    }
    if (eocd == SIZE_MAX || rd16(z + eocd + 4) || rd16(z + eocd + 6) ||
        rd16(z + eocd + 8) != rd16(z + eocd + 10) || rd16(z + eocd + 10) != cat->nfiles)
        goto bad;
    uint32_t cd_size = rd32(z + eocd + 12), cd_off = rd32(z + eocd + 16);
    if ((uint64_t)cd_off + cd_size > eocd)
        goto bad;
    bool seen[MAX_FILES] = {0};
    size_t p = cd_off;
    for (int i = 0; i < cat->nfiles; i++) {
        if (p + 46 > cd_off + cd_size || rd32(z + p) != 0x02014b50)
            goto bad;
        uint16_t flags = rd16(z + p + 8), method = rd16(z + p + 10);
        uint32_t csize = rd32(z + p + 20), usize = rd32(z + p + 24);
        uint16_t nl = rd16(z + p + 28), el = rd16(z + p + 30), cl = rd16(z + p + 32);
        uint32_t lho = rd32(z + p + 42);
        if (p + 46 + (size_t)nl + el + cl > cd_off + cd_size)
            goto bad;
        if ((flags & 1) || method != 0 || csize != usize || csize == 0xFFFFFFFFu)
            goto bad;
        int idx = -1;
        for (int k = 0; k < cat->nfiles; k++) {
            if (strlen(cat->files[k].name) == nl && !memcmp(cat->files[k].name, z + p + 46, nl))
                idx = k;
        }
        if (idx < 0 || seen[idx] || usize != (uint64_t)cat->files[idx].size)
            goto bad;
        seen[idx] = true;
        if ((uint64_t)lho + 30 > cd_off || rd32(z + lho) != 0x04034b50)
            goto bad;
        size_t data = (size_t)lho + 30 + rd16(z + lho + 26) + rd16(z + lho + 28);
        if ((uint64_t)data + usize > cd_off)
            goto bad;
        out[idx] = (struct zent){z + data, idx};
        p += 46 + (size_t)nl + el + cl;
    }
    return true;
bad:
    MSG(err, errsz, N_("The ZIP file is not the component package this version of NanumPlayer "
                       "expects."));
    return false;
}

static void remove_tree(const wchar_t *dir, int depth)
{
    size_t n = wcslen(dir);
    wchar_t *pattern = malloc((n + 3) * sizeof(wchar_t));
    if (!pattern)
        return;
    swprintf(pattern, n + 3, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            size_t m = n + wcslen(fd.cFileName) + 2;
            wchar_t *child = malloc(m * sizeof(wchar_t));
            if (!child)
                continue;
            swprintf(child, m, L"%ls\\%ls", dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                if (depth < 6)
                    remove_tree(child, depth + 1);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(child))
                    RemoveDirectoryW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void remove_tree_utf8(const char *dir)
{
    void *tmp = talloc_new(NULL);
    remove_tree(W(tmp, dir), 0);
    talloc_free(tmp);
}

// The whole file in memory (it was size-checked against the pin before).
static uint8_t *read_all(void *ctx, const char *path, int64_t expect)
{
    HANDLE f = CreateFileW(W(ctx, path), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    uint8_t *buf = NULL;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart == expect) {
        buf = talloc_size(ctx, (size_t)expect + 1);
        int64_t done = 0;
        while (buf && done < expect) {
            DWORD got = 0, want = (DWORD)MPMIN(expect - done, 1 << 20);
            if (!ReadFile(f, buf + done, want, &got, NULL) || !got) {
                talloc_free(buf);
                buf = NULL;
                break;
            }
            done += got;
        }
    }
    CloseHandle(f);
    return buf;
}

static bool write_file(struct nanum_dl *dl, const char *path, const uint8_t *data, int64_t size)
{
    void *tmp = talloc_new(NULL);
    HANDLE f = CreateFileW(W(tmp, path), GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = f != INVALID_HANDLE_VALUE;
    int64_t done = 0;
    while (ok && done < size) {
        DWORD put = 0, want = (DWORD)MPMIN(size - done, 1 << 20);
        ok = WriteFile(f, data + done, want, &put, NULL) && put == want;
        done += put;
    }
    if (ok)
        ok = FlushFileBuffers(f);
    DWORD ec = GetLastError();
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (!ok) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), ec);
        nanum_dl_fail(dl, nanum_tr("Cannot install %s: %s"), path, why);
    }
    talloc_free(tmp);
    return ok;
}

static enum nanum_dl_res res_fail(struct nanum_dl *dl, const char *stored)
{
    // The worker's error text is raw (already localized when it was recorded).
    char text[640];
    nanum_msg_render(stored, text, sizeof(text));
    nanum_dl_fail(dl, "%s", text);
    return NANUM_DL_FAIL;
}

// Unpacks the verified ZIP into `stage` and verifies every file again. Never leaves
// anything behind that is not exactly the pinned list.
static enum nanum_dl_res unpack(struct nanum_dl *dl, const struct catalog *cat, const char *zip,
                                const char *stage)
{
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r = NANUM_DL_FAIL;
    char err[640];
    uint8_t *z = read_all(tmp, zip, cat->zip.size);
    if (!z) {
        nanum_dl_fail(dl, nanum_tr("Cannot read %s"), zip);
        goto out;
    }
    struct zent ents[MAX_FILES] = {0};
    if (!zip_parse(z, (size_t)cat->zip.size, cat, ents, err, sizeof(err))) {
        r = res_fail(dl, err);
        goto out;
    }
    if (!CreateDirectoryW(W(tmp, stage), NULL)) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        nanum_dl_fail(dl, nanum_tr("Cannot create the folder %s: %s"), stage, why);
        goto out;
    }
    int64_t total = 0, done = 0;
    for (int i = 0; i < cat->nfiles; i++)
        total += cat->files[i].size;
    set_state(ST_EXTRACTING, 0, total, NULL);
    for (int i = 0; i < cat->nfiles; i++) {
        if (nanum_dl_cancelled(dl)) {
            r = NANUM_DL_CANCEL;
            goto out;
        }
        const struct pinned_file *f = &cat->files[i];
        char *path = join(tmp, stage, native_name(tmp, f->name));
        char *slash = strrchr(path, '\\');
        if (slash && slash > path + strlen(stage)) {
            *slash = 0;
            CreateDirectoryW(W(tmp, path), NULL);
            *slash = '\\';
        }
        if (!write_file(dl, path, ents[i].data, f->size))
            goto out;
        done += f->size;
        set_progress(done, false);
    }
    // What is on disk, not what was in memory, must be the pinned content.
    set_state(ST_VERIFYING, 0, cat->nfiles, NULL);
    for (int i = 0; i < cat->nfiles; i++) {
        const struct pinned_file *f = &cat->files[i];
        struct nanum_dl_item item = {.valid = true, .size = f->size};
        snprintf(item.file, sizeof(item.file), "%s", f->name);
        snprintf(item.sha, sizeof(item.sha), "%s", f->sha);
        bool match = false;
        enum nanum_dl_res h = nanum_dl_hash(dl, &item, join(tmp, stage, native_name(tmp, f->name)),
                                            &match);
        if (h != NANUM_DL_OK) {
            r = h;
            goto out;
        }
        if (!match) {
            nanum_dl_fail(dl, nanum_tr("Integrity check failed for %s (size or SHA-256 differs)."),
                          f->name);
            goto out;
        }
    }
    r = NANUM_DL_OK;
out:
    talloc_free(tmp);
    return r;
}

// Moves the staged folder into place. A folder that already verifies is kept.
static enum nanum_dl_res finalize(struct nanum_dl *dl, const struct catalog *cat,
                                  const char *stage, const char *final_dir)
{
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r = NANUM_DL_FAIL;
    char *old = NULL;
    if (path_exists(final_dir)) {
        if (verify_dir(final_dir, cat)) {       // another player installed it meanwhile
            remove_tree_utf8(stage);
            r = NANUM_DL_OK;
            goto out;
        }
        old = talloc_asprintf(tmp, "%s.old-%lu", final_dir, (unsigned long)GetCurrentProcessId());
        if (!MoveFileExW(W(tmp, final_dir), W(tmp, old), 0)) {
            char why[1024];
            nanum_dl_error_text(why, sizeof(why), GetLastError());
            nanum_dl_fail(dl, nanum_tr("Cannot install %s: %s"), final_dir, why);
            goto out;
        }
    }
    if (!MoveFileExW(W(tmp, stage), W(tmp, final_dir), MOVEFILE_WRITE_THROUGH)) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        nanum_dl_fail(dl, nanum_tr("Cannot install %s: %s"), final_dir, why);
        if (old)
            MoveFileExW(W(tmp, old), W(tmp, final_dir), 0);
        goto out;
    }
    if (old)
        remove_tree_utf8(old);
    r = NANUM_DL_OK;
out:
    talloc_free(tmp);
    return r;
}

// Other versions and leftovers of earlier attempts below the base folder (best effort).
static void remove_other_versions(const char *base, const char *version)
{
    void *tmp = talloc_new(NULL);
    wchar_t *pattern = W(tmp, join(tmp, base, "*"));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            char *name = mp_to_utf8(tmp, fd.cFileName);
            if (!strcmp(name, version) || strstr(name, ".part-"))
                continue;       // the current one; staging of a running install
            remove_tree_utf8(join(tmp, base, name));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    talloc_free(tmp);
}

static enum nanum_dl_res install_job(struct job *j)
{
    struct nanum_dl *dl = &g.dl;
    const struct catalog *cat = &g.cat;
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r = NANUM_DL_FAIL;
    char *part = NULL;
    char *stage = talloc_asprintf(tmp, "%s.part-%lu", g.dir, (unsigned long)GetCurrentProcessId());
    char *zip = j->zip;

    int64_t need = cat->zip.size;
    for (int i = 0; i < cat->nfiles; i++)
        need += cat->files[i].size;
    if (!nanum_dl_prepare_dir(dl, g.dir, need, "the disc components"))
        goto out;

    if (zip) {
        struct nanum_dl_fst st;
        if (!nanum_dl_stat(zip, &st)) {
            nanum_dl_fail(dl, nanum_tr("Cannot read %s"), zip);
            goto out;
        }
        bool match = false;
        r = nanum_dl_hash(dl, &cat->zip, zip, &match);
        if (r != NANUM_DL_OK)
            goto out;
        if (!match) {
            r = NANUM_DL_FAIL;
            char want[160];
            snprintf(want, sizeof(want), "%" PRId64 " bytes, SHA-256 %s", cat->zip.size,
                     cat->zip.sha);
            nanum_dl_fail(dl, nanum_tr("The ZIP file is not the component package this version "
                                       "of NanumPlayer expects (%s)."), want);
            goto out;
        }
    } else {
        part = talloc_asprintf(tmp, "%s\\%s.%lu.part", g.base, cat->zip.file,
                               (unsigned long)GetCurrentProcessId());
        DeleteFileW(W(tmp, part));
        r = nanum_dl_fetch(dl, &cat->zip, part);
        if (r != NANUM_DL_OK)
            goto out;
        zip = part;
    }
    remove_tree_utf8(stage);        // a leftover of an earlier attempt of this process id
    r = unpack(dl, cat, zip, stage);
    if (r != NANUM_DL_OK)
        goto out;
    r = finalize(dl, cat, stage, g.dir);
    if (r == NANUM_DL_OK)
        remove_other_versions(g.base, cat->version);
out:
    if (r != NANUM_DL_OK && stage)
        remove_tree_utf8(stage);
    if (part)
        DeleteFileW(W(tmp, part));
    talloc_free(tmp);
    return r;
}

static MP_THREAD_VOID worker_main(void *p)
{
    struct job *j = p;
    mp_thread_set_name("nanum/disc");

    enum nanum_dl_res r = NANUM_DL_OK;
    if (j->verify_only) {
        nanum_disc_component_ready(g.log);
    } else {
        r = install_job(j);
        if (r == NANUM_DL_OK) {
            mp_mutex_lock(&g_lock);
            g.inst = INST_UNKNOWN;
            mp_mutex_unlock(&g_lock);
            set_state(ST_VERIFYING, 0, 0, NULL);
            if (!nanum_disc_component_ready(g.log)) {
                nanum_dl_fail(&g.dl, "%s", nanum_tr("The installed component could not be "
                                                    "verified."));
                r = NANUM_DL_FAIL;
            }
        }
    }

    mp_mutex_lock(&g_lock);
    if (j->verify_only)
        g.st = ST_IDLE;
    else
        g.st = r == NANUM_DL_OK ? ST_DONE : r == NANUM_DL_CANCEL ? ST_CANCELLED : ST_ERROR;
    if (r != NANUM_DL_FAIL)
        g.recv = g.total = 0;
    snprintf(g.err, sizeof(g.err), "%s", r == NANUM_DL_FAIL ? g.dl.err : "");
    g.running = false;
    mp_mutex_unlock(&g_lock);
    if (r == NANUM_DL_FAIL)
        mp_err(g.log, "disc component installation failed: %s\n", g.dl.err);
    else if (!j->verify_only)
        mp_info(g.log, "disc component installation %s\n",
                r == NANUM_DL_OK ? "finished" : "cancelled");
    request_publish();

    nanum_dl_end(&g.dl);
    talloc_free(j);
    MP_THREAD_RETURN();
}

// ---------------------------------------------------------------------------
// Core thread: startup, commands
// ---------------------------------------------------------------------------

void nanum_disc_component_startup(struct MPContext *mpctx, const char *root)
{
    g.mpctx = mpctx;
    g.log = mpctx->log;
    nanum_dl_init(&g.dl, &g, dl_phase, dl_progress);
    ensure_init(root);
    if (!g.cat.ok) {
        char en[640];
        nanum_msg_render_lang(g.cat.err, en, sizeof(en), NANUM_UI_EN);
        mp_warn(g.log, "The disc component cannot be installed: %s\n", en);
    }
    g.init = true;
}

static bool reject(struct MPContext *mpctx, const char *key, const char *arg)
{
    char msg[640], en[640];
    nanum_msg(msg, sizeof(msg), key, arg, (const char *)NULL);
    nanum_msg_render_lang(msg, en, sizeof(en), NANUM_UI_EN);
    MP_ERR(mpctx, "%s\n", en);
    mp_mutex_lock(&g_lock);
    if (!g.running) {
        g.st = ST_ERROR;
        g.recv = g.total = 0;
        snprintf(g.err, sizeof(g.err), "%s", msg);
    }
    mp_mutex_unlock(&g_lock);
    publish(mpctx);
    return false;
}

static bool worker_idle(void)
{
    mp_mutex_lock(&g_lock);
    bool down = g.shutting_down, busy = g.running;
    mp_mutex_unlock(&g_lock);
    if (down || busy)
        return false;
    if (g.thread_valid) {
        mp_thread_join(g.thread);
        g.thread_valid = false;
    }
    return true;
}

// Core thread. `j` is consumed.
static bool start_worker(struct MPContext *mpctx, struct job *j)
{
    if (!nanum_dl_begin(&g.dl)) {
        talloc_free(j);
        return reject(mpctx, N_("Out of memory."), NULL);
    }
    mp_mutex_lock(&g_lock);
    g.running = true;
    g.st = ST_CHECKING;
    g.recv = 0;
    g.total = g.cat.ok ? g.cat.zip.size : 0;
    g.err[0] = 0;
    g.last_pub = GetTickCount64();
    mp_mutex_unlock(&g_lock);
    if (mp_thread_create(&g.thread, worker_main, j)) {
        mp_mutex_lock(&g_lock);
        g.running = false;
        mp_mutex_unlock(&g_lock);
        nanum_dl_end(&g.dl);
        talloc_free(j);
        return reject(mpctx, N_("Cannot start the disc component thread."), NULL);
    }
    g.thread_valid = true;
    publish(mpctx);
    return true;
}

void nanum_disc_component_refresh(struct MPContext *mpctx)
{
    if (!g.init) {
        MP_ERR(mpctx, "The disc component manager is not initialized.\n");
        return;
    }
    if (worker_idle()) {
        mp_mutex_lock(&g_lock);
        enum inst inst = g.inst;
        mp_mutex_unlock(&g_lock);
        if (inst == INST_UNKNOWN) {
            if (!g.dir || !path_exists(g.dir)) {
                mp_mutex_lock(&g_lock);
                g.inst = INST_NO;
                mp_mutex_unlock(&g_lock);
            } else {
                // Hashing the files is not done on the core thread.
                struct job *j = talloc_zero(NULL, struct job);
                j->verify_only = true;
                start_worker(mpctx, j);
                return;
            }
        }
    }
    publish(mpctx);
}

bool nanum_disc_component_install(struct MPContext *mpctx, const char *zip)
{
    if (!g.init)
        return reject(mpctx, N_("The disc component manager is not initialized."), NULL);
    mp_mutex_lock(&g_lock);
    bool down = g.shutting_down, busy = g.running;
    mp_mutex_unlock(&g_lock);
    if (down)
        return false;
    if (busy) {
        MP_WARN(mpctx, "A disc component installation or check is already running; "
                "cancel it first.\n");
        return false;
    }
    worker_idle();
    if (!g.cat.ok) {
        char text[1024];
        nanum_msg_render(g.cat.err, text, sizeof(text));
        return reject(mpctx, N_("The disc components cannot be installed: %s."),
                      text);
    }
    if (!g.dir || !g.base)
        return reject(mpctx, N_("The disc component folder cannot be determined "
                                "(%%LOCALAPPDATA%% is unavailable)."), NULL);
    mp_mutex_lock(&g_lock);
    bool have = g.inst == INST_YES;
    mp_mutex_unlock(&g_lock);
    if (have) {
        set_state(ST_DONE, 0, 0, NULL);
        publish(mpctx);
        return true;
    }
    struct job *j = talloc_zero(NULL, struct job);
    if (zip && zip[0])
        j->zip = talloc_strdup(j, zip);
    return start_worker(mpctx, j);
}

void nanum_disc_component_cancel(struct MPContext *mpctx)
{
    if (!g.init)
        return;
    mp_mutex_lock(&g_lock);
    bool running = g.running;
    mp_mutex_unlock(&g_lock);
    if (running)
        nanum_dl_abort(&g.dl);
}

void nanum_disc_component_shutdown(struct MPContext *mpctx)
{
    if (!g.init || mpctx != g.mpctx)
        return;
    mp_mutex_lock(&g_lock);
    g.shutting_down = true;
    mp_mutex_unlock(&g_lock);
    nanum_dl_abort(&g.dl);
    if (g.thread_valid) {
        mp_thread_join(g.thread);
        g.thread_valid = false;
    }
    mp_dispatch_cancel_fn(mpctx->dispatch, publish_cb, &g);
    g.mpctx = NULL;
}
