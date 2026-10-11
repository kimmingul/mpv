/*
 * NanumPlayer: optional speech-model manager (Windows). See nanum-models.h.
 *
 * Model files (id -> file):  english -> Whisper small.en, multilingual ->
 * Whisper small, multilingual_large -> Whisper large-v3-turbo (optional, used
 * for --nanum-stt-model-multilingual once installed), vad -> Silero VAD. Their names, sizes, SHA-256 values and the
 * pinned HTTPS URLs come from <app>\models\catalog.json, a file shipped with
 * the application; no remote manifest is ever trusted.
 *
 * Published as the property user-data/nanum/models (a map):
 *   status     idle | checking | downloading | verifying | done | cancelled | error
 *   current    id of the model being processed, "" if none (kept on error)
 *   received   bytes of the current file written (or hashed while verifying)
 *   total      size of the current file
 *   error      message, "" unless status is error
 *   directory  the user model directory (UTF-8)
 *   busy       true while the worker runs
 *   catalog    true if the catalog was read; catalog_error describes why not
 *   all_installed
 *   models     [{id, label, filename, size, installed, path, source}]
 *              source: "user" (user directory), "app" (<app>\models) or
 *              "custom" (explicit --nanum-*-model option; never modified)
 *
 * Threads: commands and publishing run on the core thread. The single worker
 * does network, file and hash work and talks to the core only by enqueueing a
 * mergeable callback on the core's dispatch queue.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
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
#include "options/m_config.h"
#include "options/m_config_frontend.h"
#include "options/m_property.h"
#include "player/command.h"
#include "player/core.h"
#include "osdep/io.h"
#include "osdep/nanum-models.h"
#include "osdep/nanum-download.h"
#include "osdep/threads.h"

#define PROGRESS_MS 200

// M_MULTI_LARGE has no option of its own: it replaces the value of M_MULTI's.
// It stays last (the startup defaults only cover the first
// NANUM_MODEL_PATH_COUNT entries) and is not part of "all".
enum { M_EN, M_MULTI, M_VAD, M_MULTI_LARGE, M_COUNT };

static const struct {
    const char *id;
    const char *option;
    const char *file;       // used only while the catalog is unavailable
    const char *label;
} KNOWN[M_COUNT] = {
    [M_EN]    = {"english",      "nanum-stt-model",              "ggml-small.en.bin",      "Whisper small.en"},
    [M_MULTI] = {"multilingual", "nanum-stt-model-multilingual", "ggml-small.bin",         "Whisper small"},
    [M_VAD]   = {"vad",          "nanum-vad-model",              "ggml-silero-v5.1.2.bin", "Silero VAD"},
    [M_MULTI_LARGE] = {"multilingual_large", "nanum-stt-model-multilingual", "ggml-large-v3-turbo-q5_0.bin",
                       "Whisper large-v3-turbo"},
};

enum kind { KIND_USER, KIND_APP, KIND_CUSTOM };
static const char *const KIND_NAME[] = {"user", "app", "custom"};

enum state {
    ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING, ST_DONE, ST_CANCELLED, ST_ERROR,
};
static const char *const STATE_NAME[] = {
    "idle", "checking", "downloading", "verifying", "done", "cancelled", "error",
};

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;

static struct {
    // Written by nanum_models_startup() and the core thread only.
    bool init;
    struct MPContext *mpctx;
    struct mp_log *log;
    char *root;                 // application directory (UTF-8) or NULL
    char *exe_dir;              // directory of the executable or NULL
    char *userdir;              // user model directory or NULL
    char *def[M_COUNT];         // default option values chosen at startup
    bool def_app[M_COUNT];
    struct nanum_dl_item cat[M_COUNT];
    bool cat_ok;
    char cat_err[512];

    // Worker interface; guarded by g_lock unless noted.
    struct nanum_dl dl;         // shared downloader: cancel flag and request are
                                // synchronized by the downloader itself
    bool shutting_down;
    bool running;
    bool thread_valid;          // core thread only
    mp_thread thread;           // core thread only
    enum state st;
    int cur;
    int64_t recv, total;
    char err[640];
    ULONGLONG last_pub;
} g;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static wchar_t *W(void *ctx, const char *s)
{
    return mp_from_utf8(ctx, s);
}

static const char *model_file(int i)
{
    return g.cat[i].valid ? g.cat[i].file : KNOWN[i].file;
}

static char *user_path(void *ctx, int i)
{
    return g.userdir ? nanum_dl_join(ctx, g.userdir, model_file(i)) : NULL;
}

static char *app_path(void *ctx, int i)
{
    if (!g.root)
        return NULL;
    char *dir = nanum_dl_join(ctx, g.root, "models");
    return nanum_dl_join(ctx, dir, model_file(i));
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

static void catalog_fail(const char *fmt, ...) MP_PRINTF_ATTRIBUTE(1, 2);
static void catalog_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.cat_err, sizeof(g.cat_err), fmt, ap);
    va_end(ap);
    g.cat_ok = false;
}

static void load_catalog(void)
{
    memset(g.cat, 0, sizeof(g.cat));
    g.cat_ok = false;
    g.cat_err[0] = 0;

    void *tmp = talloc_new(NULL);
    const char *dirs[2] = {g.root, g.exe_dir};
    char *text = NULL, *path = NULL;
    for (int i = 0; i < 2 && !text; i++) {
        if (!dirs[i] || (i == 1 && g.root && nanum_dl_same_path(dirs[0], dirs[1])))
            continue;
        path = nanum_dl_join(tmp, nanum_dl_join(tmp, dirs[i], "models"), "catalog.json");
        text = nanum_dl_read_text(tmp, path);
    }
    if (!text) {
        catalog_fail("model catalog models\\catalog.json not found or unreadable%s%s",
                     path ? ": " : "", path ? path : "");
        goto done;
    }

    char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    mpv_node doc;
    if (json_parse(tmp, &doc, &p, MAX_JSON_DEPTH) < 0 || doc.format != MPV_FORMAT_NODE_MAP) {
        catalog_fail("model catalog %s is not valid JSON", path);
        goto done;
    }
    int64_t schema = 0;
    mpv_node *models = node_map_get(&doc, "models");
    if (!nanum_dl_node_int(&doc, "schema", &schema) || schema != 1 || !models ||
        models->format != MPV_FORMAT_NODE_ARRAY)
    {
        catalog_fail("model catalog %s has an unsupported format (schema 1 expected)", path);
        goto done;
    }

    for (int k = 0; k < models->u.list->num; k++) {
        mpv_node *m = &models->u.list->values[k];
        if (m->format != MPV_FORMAT_NODE_MAP)
            continue;
        const char *id = nanum_dl_node_str(m, "id");
        int i = 0;
        while (i < M_COUNT && (!id || strcmp(id, KNOWN[i].id)))
            i++;
        if (i == M_COUNT)
            continue;
        struct nanum_dl_item e;
        char why[256];
        if (!nanum_dl_parse_item(m, KNOWN[i].label, &e, why, sizeof(why))) {
            catalog_fail("model %s", why);
            continue;
        }
        g.cat[i] = e;
    }
    // Entries that were absent are reported too.
    bool all = true;
    for (int i = 0; i < M_COUNT; i++) {
        if (!g.cat[i].valid) {
            all = false;
            if (!g.cat_err[0])
                catalog_fail("model catalog has no entry for '%s'", KNOWN[i].id);
        }
    }
    g.cat_ok = all;
done:
    talloc_free(tmp);
}

// ---------------------------------------------------------------------------
// State shared with the worker
// ---------------------------------------------------------------------------

static void publish(struct MPContext *mpctx);

static void publish_cb(void *p)
{
    if (g.mpctx)
        publish(g.mpctx);
}

// Any thread. Merges with an already queued publication.
static void request_publish(void)
{
    struct MPContext *mpctx = g.mpctx;
    if (mpctx)
        mp_dispatch_enqueue_notify(mpctx->dispatch, publish_cb, &g);
}

static void set_state(enum state st, int cur, int64_t recv, int64_t total, const char *err)
{
    mp_mutex_lock(&g_lock);
    g.st = st;
    g.cur = cur;
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

// Downloader callbacks (worker thread).
static void models_phase(struct nanum_dl *dl, enum nanum_dl_phase ph, int64_t recv,
                         int64_t total)
{
    static const enum state map[] = {ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING};
    set_state(map[ph], dl->tag, recv, total, NULL);
}

static void models_progress(struct nanum_dl *dl, int64_t recv)
{
    set_progress(recv, false);
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

struct jmodel {
    int idx;
    enum kind kind;
    char *path;             // final file (UTF-8)
    struct nanum_dl_item e;
};

struct job {
    int n;
    struct jmodel m[M_COUNT];
};

static enum nanum_dl_res process_model(struct jmodel *m)
{
    struct nanum_dl *dl = &g.dl;
    dl->tag = m->idx;
    set_state(ST_CHECKING, m->idx, 0, m->e.size, NULL);
    if (nanum_dl_cancelled(dl))
        return NANUM_DL_CANCEL;

    struct nanum_dl_fst f;
    bool exists = nanum_dl_stat(m->path, &f);

    if (m->kind == KIND_CUSTOM) {
        // The user pointed --%s at their own file: never touched.
        if (exists && f.size > 0) {
            mp_info(g.log, "model '%s': using the file set by --%s\n", KNOWN[m->idx].id,
                    KNOWN[m->idx].option);
            return NANUM_DL_OK;
        }
        nanum_dl_fail(dl, "--%s is set to %s, which does not exist. Fix or remove that option "
                      "(downloads never overwrite custom model paths).",
                      KNOWN[m->idx].option, m->path);
        return NANUM_DL_FAIL;
    }

    if (exists) {
        if (f.size == m->e.size) {
            bool match = false;
            enum nanum_dl_res h = nanum_dl_hash(dl, &m->e, m->path, &match);
            if (h != NANUM_DL_OK)
                return h;
            if (match) {
                mp_info(g.log, "model '%s': %s is already installed and verified\n",
                        KNOWN[m->idx].id, m->path);
                return NANUM_DL_OK;
            }
        }
        if (m->kind == KIND_APP) {
            nanum_dl_fail(dl, "%s does not match the pinned model (wrong size or SHA-256). It is "
                          "not modified; replace or delete it to use the downloaded model.",
                          m->path);
            return NANUM_DL_FAIL;
        }
    } else if (m->kind == KIND_APP) {
        nanum_dl_fail(dl, "%s is in use but no longer exists.", m->path);
        return NANUM_DL_FAIL;
    }

    // Destination directory and free space.
    if (!nanum_dl_prepare_dir(dl, m->path, m->e.size, m->e.file))
        return NANUM_DL_FAIL;

    // One .part per process: concurrent writers (the installer, a second
    // player) never share a temporary file.
    void *tmp = talloc_new(NULL);
    char *part = talloc_asprintf(tmp, "%s.%lu.part", m->path, (unsigned long)GetCurrentProcessId());

    mp_info(g.log, "model '%s': downloading %s\n", KNOWN[m->idx].id, m->e.file);
    enum nanum_dl_res r = nanum_dl_fetch(dl, &m->e, part);
    if (r == NANUM_DL_OK)
        r = nanum_dl_install(dl, &m->e, m->path, part, &f);
    if (r != NANUM_DL_OK)
        DeleteFileW(W(tmp, part));
    talloc_free(tmp);
    return r;
}

static MP_THREAD_VOID worker_main(void *p)
{
    struct job *j = p;
    mp_thread_set_name("nanum/models");

    enum nanum_dl_res r = NANUM_DL_OK;
    int failed = -1;
    for (int i = 0; i < j->n && r == NANUM_DL_OK; i++) {
        r = process_model(&j->m[i]);
        if (r == NANUM_DL_FAIL)
            failed = j->m[i].idx;
        if (r == NANUM_DL_OK && nanum_dl_cancelled(&g.dl))
            r = NANUM_DL_CANCEL;
    }

    mp_mutex_lock(&g_lock);
    g.st = r == NANUM_DL_OK ? ST_DONE : r == NANUM_DL_CANCEL ? ST_CANCELLED : ST_ERROR;
    g.cur = failed;
    if (r != NANUM_DL_FAIL)
        g.recv = g.total = 0;
    snprintf(g.err, sizeof(g.err), "%s", r == NANUM_DL_FAIL ? g.dl.err : "");
    g.running = false;
    mp_mutex_unlock(&g_lock);
    if (r == NANUM_DL_FAIL)
        mp_err(g.log, "model download failed: %s\n", g.dl.err);
    else
        mp_info(g.log, "model download %s\n", r == NANUM_DL_OK ? "finished" : "cancelled");
    request_publish();

    nanum_dl_end(&g.dl);
    for (int i = 0; i < j->n; i++) {
        talloc_free(j->m[i].path);
    }
    talloc_free(j);
    MP_THREAD_RETURN();
}

// ---------------------------------------------------------------------------
// Core thread: option lookup, status, commands
// ---------------------------------------------------------------------------

// Value of the option the model is selected through. The large model has none
// of its own, and the multilingual option may hold the large model's path
// (see adopt_large()); that counts as the small model's default here.
static char *option_path(void *ctx, struct MPContext *mpctx, int i)
{
    if (i == M_MULTI_LARGE)
        return NULL;
    struct m_config_option *co = m_config_get_co(mpctx->mconfig, bstr0(KNOWN[i].option));
    if (!co || !co->data)
        return NULL;
    const char *s = *(char **)co->data;
    if (!s || !s[0])
        return NULL;
    if (i == M_MULTI) {
        char *lu = user_path(ctx, M_MULTI_LARGE), *la = app_path(ctx, M_MULTI_LARGE);
        if ((lu && nanum_dl_same_path(s, lu)) || (la && nanum_dl_same_path(s, la)))
            return g.def[i] ? talloc_strdup(ctx, g.def[i]) : NULL;
    }
    return talloc_strdup(ctx, s);
}

struct minfo {
    enum kind kind;
    char *path;         // the file the engine uses (or would use)
    bool installed;
};

static void model_info(void *ctx, struct MPContext *mpctx, int i, struct minfo *mi)
{
    char *eff = option_path(ctx, mpctx, i);
    char *userp = user_path(ctx, i), *appp = app_path(ctx, i);
    enum kind kind = KIND_CUSTOM;
    if (!eff)
        kind = i == M_MULTI_LARGE && g.def_app[i] ? KIND_APP : KIND_USER;
    else if (userp && nanum_dl_same_path(eff, userp))
        kind = KIND_USER;
    else if (appp && nanum_dl_same_path(eff, appp))
        kind = KIND_APP;
    else if (g.def[i] && nanum_dl_same_path(eff, g.def[i]))
        kind = g.def_app[i] ? KIND_APP : KIND_USER;

    mi->kind = kind;
    mi->path = kind == KIND_CUSTOM ? eff : kind == KIND_APP ? (eff ? eff : appp)
                                                           : (userp ? userp : (char *)"");
    struct nanum_dl_fst f;
    mi->installed = mi->path[0] && nanum_dl_stat(mi->path, &f) && f.size > 0;
    if (mi->installed && kind != KIND_CUSTOM && g.cat[i].valid)
        mi->installed = f.size == g.cat[i].size;
}

// Core thread. Once the large model is installed and the multilingual option
// still has its default (small model) value, the next opened file uses the large
// model: this is what a download during the session needs. An option the user
// set explicitly is never replaced.
static void adopt_large(struct MPContext *mpctx, void *ctx)
{
    struct minfo large, small;
    model_info(ctx, mpctx, M_MULTI_LARGE, &large);
    if (!large.installed)
        return;
    struct m_config_option *co = m_config_get_co(mpctx->mconfig,
                                                 bstr0(KNOWN[M_MULTI].option));
    const char *cur = co && co->data ? *(char **)co->data : NULL;
    if (!cur || !cur[0] || nanum_dl_same_path(cur, large.path))
        return;
    model_info(ctx, mpctx, M_MULTI, &small);
    if (small.kind == KIND_CUSTOM)
        return;
    if (mp_property_do("options/nanum-stt-model-multilingual", M_PROPERTY_SET_STRING,
                       large.path, mpctx) > 0)
        mp_info(g.log, "--nanum-stt-model-multilingual: using %s\n", large.path);
}

static void publish(struct MPContext *mpctx)
{
    if (!g.init || !mpctx)
        return;
    void *tmp = talloc_new(NULL);

    mp_mutex_lock(&g_lock);
    enum state st = g.st;
    int cur = g.cur;
    int64_t recv = g.recv, total = g.total;
    bool running = g.running;
    char err[sizeof(g.err)];
    memcpy(err, g.err, sizeof(err));
    mp_mutex_unlock(&g_lock);

    adopt_large(mpctx, tmp);

    mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_string(&root, "status", STATE_NAME[st]);
    node_map_add_string(&root, "current", cur >= 0 && cur < M_COUNT ? KNOWN[cur].id : "");
    node_map_add_int64(&root, "received", recv);
    node_map_add_int64(&root, "total", total);
    node_map_add_string(&root, "error", err);
    node_map_add_string(&root, "directory", g.userdir ? g.userdir : "");
    node_map_add_flag(&root, "busy", running);
    node_map_add_flag(&root, "catalog", g.cat_ok);
    node_map_add_string(&root, "catalog_error", g.cat_err);

    mpv_node *arr = node_map_add(&root, "models", MPV_FORMAT_NODE_ARRAY);
    bool all = true;
    for (int i = 0; i < M_COUNT; i++) {
        struct minfo mi;
        model_info(tmp, mpctx, i, &mi);
        if (i != M_MULTI_LARGE)
            all &= mi.installed;
        mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
        node_map_add_string(m, "id", KNOWN[i].id);
        node_map_add_string(m, "label", g.cat[i].valid ? g.cat[i].label : KNOWN[i].label);
        node_map_add_string(m, "filename", model_file(i));
        node_map_add_int64(m, "size", g.cat[i].valid ? g.cat[i].size : 0);
        node_map_add_flag(m, "installed", mi.installed);
        node_map_add_string(m, "path", mi.path);
        node_map_add_string(m, "source", KIND_NAME[mi.kind]);
    }
    node_map_add_flag(&root, "all_installed", all);

    mp_property_do("user-data/nanum/models", M_PROPERTY_SET_NODE, &root, mpctx);
    talloc_free(root.u.list);
    talloc_free(tmp);
}

void nanum_models_startup(struct MPContext *mpctx, const char *root, void *ctx,
                          char *defaults[NANUM_MODEL_PATH_COUNT])
{
    g.mpctx = mpctx;
    g.log = mpctx->log;
    g.cur = -1;
    nanum_dl_init(&g.dl, &g, models_phase, models_progress);
    g.root = root ? talloc_strdup(NULL, root) : NULL;
    if (g.root)
        nanum_dl_trim_slashes(g.root);

    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MP_ARRAY_SIZE(exe));
    if (n > 0 && n < MP_ARRAY_SIZE(exe)) {
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash)
            *slash = 0;
        g.exe_dir = mp_to_utf8(NULL, exe);
    }

    wchar_t env[MAX_PATH * 2];
    n = GetEnvironmentVariableW(L"NANUMPLAYER_MODELS_DIR", env, MP_ARRAY_SIZE(env));
    if (n > 0 && n < MP_ARRAY_SIZE(env)) {
        g.userdir = mp_to_utf8(NULL, env);
        nanum_dl_trim_slashes(g.userdir);
    } else {
        wchar_t *local = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) {
            char *base = mp_to_utf8(NULL, local);
            g.userdir = talloc_asprintf(NULL, "%s\\NanumPlayer\\models", base);
            talloc_free(base);
            CoTaskMemFree(local);
        }
    }

    load_catalog();
    if (!g.cat_ok)
        mp_warn(g.log, "%s\n", g.cat_err);

    void *tmp = talloc_new(NULL);
    for (int i = 0; i < M_COUNT; i++) {
        char *app = app_path(tmp, i), *user = user_path(tmp, i);
        struct nanum_dl_fst f;
        bool app_ok = app && nanum_dl_stat(app, &f) &&
                      (!g.cat[i].valid || f.size == g.cat[i].size);
        if (app && !app_ok && f.exists)
            mp_warn(g.log, "ignoring %s: size does not match the model catalog\n", app);
        const char *def = app_ok ? app : user ? user : app;
        g.def_app[i] = def && def == app;
        g.def[i] = def ? talloc_strdup(NULL, def) : NULL;
        if (i < NANUM_MODEL_PATH_COUNT)
            defaults[i] = g.def[i] ? talloc_strdup(ctx, g.def[i]) : NULL;
    }
    // The large multilingual model, when installed, serves
    // --nanum-stt-model-multilingual instead of the small one.
    if (g.def[M_MULTI_LARGE] && g.cat[M_MULTI_LARGE].valid) {
        struct nanum_dl_fst f;
        if (nanum_dl_stat(g.def[M_MULTI_LARGE], &f) && f.size == g.cat[M_MULTI_LARGE].size)
            defaults[NANUM_MODEL_PATH_MULTILINGUAL] = talloc_strdup(ctx, g.def[M_MULTI_LARGE]);
    }
    talloc_free(tmp);
    g.init = true;
}

void nanum_models_refresh(struct MPContext *mpctx)
{
    if (!g.init) {
        MP_ERR(mpctx, "The model manager is not initialized.\n");
        return;
    }
    if (!g.cat_ok)
        load_catalog();
    publish(mpctx);
}

// Records a rejected request as the (terminal) state, unless a worker runs.
static bool reject(struct MPContext *mpctx, const char *fmt, ...) MP_PRINTF_ATTRIBUTE(2, 3);
static bool reject(struct MPContext *mpctx, const char *fmt, ...)
{
    char msg[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    MP_ERR(mpctx, "%s\n", msg);
    mp_mutex_lock(&g_lock);
    if (!g.running) {
        g.st = ST_ERROR;
        g.cur = -1;
        g.recv = g.total = 0;
        snprintf(g.err, sizeof(g.err), "%s", msg);
    }
    mp_mutex_unlock(&g_lock);
    publish(mpctx);
    return false;
}

bool nanum_models_download(struct MPContext *mpctx, enum nanum_model_target target)
{
    if (!g.init)
        return reject(mpctx, "The model manager is not initialized.");

    mp_mutex_lock(&g_lock);
    bool down = g.shutting_down, busy = g.running;
    mp_mutex_unlock(&g_lock);
    if (down)
        return false;
    if (busy) {
        MP_WARN(mpctx, "A model download is already running; cancel it first.\n");
        return false;
    }
    if (g.thread_valid) {
        mp_thread_join(g.thread);   // finished: running is false
        g.thread_valid = false;
    }

    // A Whisper model without the VAD model cannot be used: always ensure it.
    static const int plan[][M_COUNT] = {
        [NANUM_MODELS_ALL]          = {M_VAD, M_EN, M_MULTI, -1},
        [NANUM_MODELS_ENGLISH]      = {M_VAD, M_EN, -1, -1},
        [NANUM_MODELS_MULTILINGUAL] = {M_VAD, M_MULTI, -1, -1},
        [NANUM_MODELS_VAD]          = {M_VAD, -1, -1, -1},
        [NANUM_MODELS_MULTILINGUAL_LARGE] = {M_VAD, M_MULTI_LARGE, -1, -1},
    };
    if ((unsigned)target >= MP_ARRAY_SIZE(plan))
        return reject(mpctx, "Unknown model download target.");

    if (!g.cat_ok)
        load_catalog();

    void *tmp = talloc_new(NULL);
    struct job *j = talloc_zero(NULL, struct job);
    for (int k = 0; k < M_COUNT && plan[target][k] >= 0; k++) {
        int i = plan[target][k];
        if (!g.cat[i].valid) {
            talloc_free(tmp);
            talloc_free(j);
            return reject(mpctx, "%s", g.cat_err[0] ? g.cat_err
                                                    : "The model catalog is unavailable.");
        }
        struct minfo mi;
        model_info(tmp, mpctx, i, &mi);
        if (!mi.path[0]) {
            talloc_free(tmp);
            talloc_free(j);
            return reject(mpctx, "The user model folder cannot be determined "
                          "(%%LOCALAPPDATA%% is unavailable).");
        }
        struct jmodel *m = &j->m[j->n++];
        m->idx = i;
        m->kind = mi.kind;
        m->path = talloc_strdup(NULL, mi.path);
        m->e = g.cat[i];
    }
    talloc_free(tmp);

    if (!nanum_dl_begin(&g.dl)) {
        talloc_free(j);
        return reject(mpctx, "Out of memory.");
    }
    mp_mutex_lock(&g_lock);
    g.running = true;
    g.st = ST_CHECKING;
    g.cur = j->m[0].idx;
    g.recv = 0;
    g.total = j->m[0].e.size;
    g.err[0] = 0;
    g.last_pub = GetTickCount64();
    mp_mutex_unlock(&g_lock);

    if (mp_thread_create(&g.thread, worker_main, j)) {
        mp_mutex_lock(&g_lock);
        g.running = false;
        mp_mutex_unlock(&g_lock);
        nanum_dl_end(&g.dl);
        talloc_free(j);
        return reject(mpctx, "Cannot start the model download thread.");
    }
    g.thread_valid = true;
    publish(mpctx);
    return true;
}

void nanum_models_cancel(struct MPContext *mpctx)
{
    if (!g.init)
        return;
    mp_mutex_lock(&g_lock);
    bool running = g.running;
    mp_mutex_unlock(&g_lock);
    if (!running)
        return;
    nanum_dl_abort(&g.dl);
}

void nanum_models_shutdown(struct MPContext *mpctx)
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
    // No sender is left: drop a publication that is still queued, then stop
    // touching the context (the dispatch queue must be empty when it dies).
    mp_dispatch_cancel_fn(mpctx->dispatch, publish_cb, &g);
    g.mpctx = NULL;
}
