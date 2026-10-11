/*
 * NanumPlayer: AI engine selection and optional CUDA component manager
 * (Windows). See nanum-accel.h.
 *
 * The CPU engine nanum_ai_cpu.dll ships with the application. Optional downloads are the
 * CUDA engine (cuda_engine -> nanum_ai_cuda.dll) and NVIDIA's cuBLAS runtimes
 * (cublas -> cublas64_13.dll, cublas_lt -> cublasLt64_13.dll). Their names, sizes,
 * SHA-256 values and pinned HTTPS URLs come from <app>\components\catalog.json:
 *   { "schema": 1, "cuda_major": 13, "min_compute": 86,
 *     "components": [ {id, label, filename, size, sha256, url}, ... ] }
 * (the catalog is independent of the application version; a new engine is a new pin).
 * No remote manifest is ever trusted. Without the catalog nothing CUDA related can be
 * loaded (the CPU engine is not affected).
 *
 * Where a component comes from ("source"):
 *   toolkit  cublas / cublas_lt only: a compatible installed CUDA Toolkit, used in place
 *            and never copied (nothing of NVIDIA's is redistributed, no license step).
 *            Found via CUDA_PATH_V13_* / CUDA_PATH of the process, the same variables in
 *            the registry's environment (HKLM Session Manager\Environment, HKCU
 *            Environment; a Toolkit installed after Explorer started is not in the
 *            inherited environment), and %ProgramFiles%\NVIDIA GPU Computing
 *            Toolkit\CUDA\v13.*; newest first. NANUMPLAYER_CUDA_TOOLKIT_ROOT
 *            (tests) replaces all of that by the folder holding v13.* Toolkits.
 *            A Toolkit directory is accepted only if BOTH runtime DLLs have the
 *            catalog's ABI-specific filenames, are x64 DLLs and carry identical
 *            readable vendor file-version resources. The pair comes from ONE directory.
 *   user     %LOCALAPPDATA%\NanumPlayer\accelerators\cuda\<cuda major>\<file> (engine and
 *            runtimes; downloaded by this manager, size/SHA-256 verified, independent of
 *            the app version). cuBLAS is only downloaded after the NVIDIA license was
 *            accepted (the UI's job); the engine contains nothing of NVIDIA's.
 *   reused   a "user" file copied (no network, size + SHA-256 verified, atomic rename)
 *            from an older per-app-version directory cuda\<version>\ (migration;
 *            marked by <file>.reused)
 * The cached engine is hashed once per file identity; a file that is not the catalog's
 * engine (an older one) counts as missing and sets "update_required".
 *
 * Loading (nanum_accel_load_engine): the runtime DLLs are loaded first, by
 * their full paths with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | SYSTEM32, and the
 * engine is loaded next from its own folder; the engine's imports of the
 * runtimes bind to the modules already loaded under those names. No directory
 * is added to the process search path (AddDllDirectory / PATH / SetDllDirectory),
 * so a Toolkit is never visible to unrelated modules. The handles live in
 * struct nanum_engine until the engine is unloaded.
 *
 * Published as the property user-data/nanum/accelerator (a map):
 *   mode            auto | cpu | cuda   (option --nanum-acceleration)
 *   backend         cpu | cuda | unavailable: what the next opened file will use
 *   cpu_available   the CPU engine exists
 *   cuda_available  engine and runtimes are present and the engine+GPU probe succeeded
 *   toolkit, toolkit_directory, toolkit_version   a compatible CUDA Toolkit was found
 *                   (newest one; version like "13.4", "" if unknown)
 *   directory       the user component directory (UTF-8)
 *   busy            true while the worker runs (download, migration or background probe)
 *   status          idle | checking | downloading | verifying | done | cancelled | error
 *   current         id of the component being processed, "" if none
 *   received, total bytes of the current file written / hashed / copied, its size
 *   error           message, "" unless status is error
 *   reason          why CUDA is not usable / not used (see below), "" otherwise
 *   download_size   bytes that still have to be downloaded (engine, runtimes)
 *   update_required an engine is installed (or in an old folder) but it is not the catalog's
 *   catalog, catalog_error, active_backend (extra)
 *   components      [{id, label, filename, size, installed, path, source}]: cuda_engine,
 *                   cublas, cublas_lt (source toolkit | user | reused)
 *
 * Threads: commands and publishing run on the core thread. The single worker
 * does network, file, hash and probe work and talks to the core only by
 * enqueueing a mergeable callback on the core's dispatch queue. Engine loading
 * runs on the demuxer's thread and only reads shared state under g_lock.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <winver.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <ctype.h>
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
#include "options/m_config.h"
#include "options/m_config_frontend.h"
#include "options/m_property.h"
#include "player/command.h"
#include "player/core.h"
#include "osdep/io.h"
#include "osdep/nanum-accel.h"
#include "osdep/nanum-download.h"
#include "osdep/threads.h"

#define CPU_ENGINE_FILE "nanum_ai_cpu.dll"
#define PROGRESS_MS 200
#define MAX_CAND 12
#define MAX_PROBE 12
#define DEFAULT_CUDA_MAJOR 13
#define MAX_OTHER 16
#define MAX_TK_ROOTS 24

enum { C_ENGINE, C_CUBLAS, C_CUBLAS_LT, C_COUNT };

static const struct {
    const char *id;
    const char *file;
    const char *label;      // used while the catalog is unavailable
} KNOWN[C_COUNT] = {
    [C_ENGINE]     = {"cuda_engine", "nanum_ai_cuda.dll", "NanumPlayer CUDA engine"},
    [C_CUBLAS]     = {"cublas",      "cublas64_13.dll",   "NVIDIA cuBLAS"},
    [C_CUBLAS_LT]  = {"cublas_lt",   "cublasLt64_13.dll", "NVIDIA cuBLASLt"},
};

static const char *const MODE_NAME[] = {"auto", "cpu", "cuda"};

enum state {
    ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING, ST_DONE, ST_CANCELLED, ST_ERROR,
};
static const char *const STATE_NAME[] = {
    "idle", "checking", "downloading", "verifying", "done", "cancelled", "error",
};

enum job_kind { JOB_DOWNLOAD, JOB_PROBE, JOB_REUSE };

struct catalog {
    bool ok;
    char err[512];
    int cuda_major;
    int min_compute;
    struct nanum_dl_item item[C_COUNT];
};

struct probe_entry {
    bool used;
    bool ok;
    char key[1700];
    char reason[640];
};

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;

static struct {
    // Paths and catalog: written once by ensure_init() / the core thread,
    // under g_lock; the strings are never freed.
    bool paths;
    char *root;                 // application directory (UTF-8)
    char *exe_dir;
    char *cachedir;             // component directory or NULL
    char *cudaroot;             // the folder holding one directory per app version
    struct catalog cat;

    // Core thread only.
    bool init;
    struct MPContext *mpctx;
    struct mp_log *log;
    bool thread_valid;
    mp_thread thread;

    // Guarded by g_lock.
    struct probe_entry probe[MAX_PROBE];
    int probe_next;
    char last_backend[16];      // backend of the engine loaded last
    bool reuse_tried[C_COUNT];  // components looked for in the other version directories
    char eng_key[1700];         // identity (path|size|mtime) of the cached engine that was hashed
    bool eng_good;              // ... and whether it matched the catalog SHA-256
    bool shutting_down;
    bool running;
    enum job_kind running_kind;
    enum state st;
    int cur;
    int64_t recv, total;
    char err[640];
    ULONGLONG last_pub;

    struct nanum_dl dl;
} g;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static wchar_t *W(void *ctx, const char *s)
{
    return mp_from_utf8(ctx, s);
}

static const char *comp_file(const struct catalog *c, int i)
{
    return c->item[i].valid ? c->item[i].file : KNOWN[i].file;
}

static char *join(void *ctx, const char *a, const char *b)
{
    return nanum_dl_join(ctx, a, b);
}

static int64_t mib_up(int64_t bytes)
{
    return (bytes + (1 << 20) - 1) >> 20;
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

static void cat_fail(struct catalog *c, const char *fmt, ...) MP_PRINTF_ATTRIBUTE(2, 3);
static void cat_fail(struct catalog *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err), fmt, ap);
    va_end(ap);
    c->ok = false;
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
        cat_fail(c, "the component catalog components\\catalog.json is missing or unreadable%s%s",
                 path ? ": " : "", path ? path : "");
        goto done;
    }
    char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    mpv_node doc;
    if (json_parse(tmp, &doc, &p, MAX_JSON_DEPTH) < 0 || doc.format != MPV_FORMAT_NODE_MAP) {
        cat_fail(c, "the component catalog %s is not valid JSON", path);
        goto done;
    }
    int64_t schema = 0, major = 0, compute = 0;
    mpv_node *comps = node_map_get(&doc, "components");
    if (!nanum_dl_node_int(&doc, "schema", &schema) || schema != 1 || !comps ||
        comps->format != MPV_FORMAT_NODE_ARRAY)
    {
        cat_fail(c, "the component catalog %s has an unsupported format (schema 1 expected)", path);
        goto done;
    }
    if (!nanum_dl_node_int(&doc, "cuda_major", &major) || major != DEFAULT_CUDA_MAJOR ||
        !nanum_dl_node_int(&doc, "min_compute", &compute) || compute <= 0 || compute > 1000)
    {
        cat_fail(c, "the component catalog %s does not describe CUDA %d components",
                 path, DEFAULT_CUDA_MAJOR);
        goto done;
    }
    c->cuda_major = (int)major;
    c->min_compute = (int)compute;

    for (int k = 0; k < comps->u.list->num; k++) {
        mpv_node *m = &comps->u.list->values[k];
        if (m->format != MPV_FORMAT_NODE_MAP)
            continue;
        const char *id = nanum_dl_node_str(m, "id");
        int i = 0;
        while (i < C_COUNT && (!id || strcmp(id, KNOWN[i].id)))
            i++;
        if (i == C_COUNT)
            continue;
        struct nanum_dl_item e;
        char why[256];
        if (!nanum_dl_parse_item(m, KNOWN[i].label, &e, why, sizeof(why))) {
            cat_fail(c, "%s", why);
            continue;
        }
        // The engine imports the runtimes by these exact names.
        if (_stricmp(e.file, KNOWN[i].file)) {
            cat_fail(c, "component '%s' must be the file %s, not %s", id, KNOWN[i].file, e.file);
            continue;
        }
        c->item[i] = e;
    }
    bool all = true;
    for (int i = 0; i < C_COUNT; i++) {
        if (!c->item[i].valid) {
            all = false;
            if (!c->err[0])
                cat_fail(c, "the component catalog has no entry for '%s'", KNOWN[i].id);
        }
    }
    c->ok = all;
done:
    talloc_free(tmp);
}

static void catalog_get(struct catalog *out)
{
    mp_mutex_lock(&g_lock);
    *out = g.cat;
    mp_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// Idempotent, any thread. `root` (the application directory, NULL = derive it)
// only matters for the first call.
static void ensure_init(const char *root)
{
    mp_mutex_lock(&g_lock);
    if (g.paths) {
        mp_mutex_unlock(&g_lock);
        return;
    }
    void *tmp = talloc_new(NULL);

    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MP_ARRAY_SIZE(exe));
    if (n > 0 && n < MP_ARRAY_SIZE(exe)) {
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash)
            *slash = 0;
        g.exe_dir = mp_to_utf8(NULL, exe);
    }

    wchar_t env[MAX_PATH * 2];
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
    n = GetEnvironmentVariableW(L"NANUMPLAYER_ACCEL_DIR", env, MP_ARRAY_SIZE(env));
    if (n > 0 && n < MP_ARRAY_SIZE(env)) {
        base = mp_to_utf8(tmp, env);
        nanum_dl_trim_slashes(base);
    } else {
        wchar_t *local = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) {
            char *l = mp_to_utf8(tmp, local);
            base = talloc_asprintf(tmp, "%s\\NanumPlayer\\accelerators", l);
            CoTaskMemFree(local);
        }
    }
    if (base) {
        g.cudaroot = talloc_asprintf(NULL, "%s\\cuda", base);
        g.cachedir = talloc_asprintf(NULL, "%s\\%d", g.cudaroot, DEFAULT_CUDA_MAJOR);
    }

    read_catalog(&g.cat);
    g.paths = true;
    talloc_free(tmp);
    mp_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// DLL inspection: architecture and version
// ---------------------------------------------------------------------------

static bool pe_check_x64_dll(const char *path, char *why, size_t whysz)
{
    void *tmp = talloc_new(NULL);
    HANDLE f = CreateFileW(W(tmp, path), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    talloc_free(tmp);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, whysz, "%s cannot be read", path);
        return false;
    }
    uint8_t b[1024];
    DWORD got = 0;
    bool read = ReadFile(f, b, sizeof(b), &got, NULL);
    CloseHandle(f);
    uint32_t off = 0;
    uint16_t machine = 0, chars = 0;
    if (!read || got < 64 || b[0] != 'M' || b[1] != 'Z') {
        snprintf(why, whysz, "%s is not a Windows DLL", path);
        return false;
    }
    memcpy(&off, b + 0x3c, sizeof(off));
    if (off > got - 24 || memcmp(b + off, "PE\0\0", 4)) {
        snprintf(why, whysz, "%s is not a Windows DLL", path);
        return false;
    }
    memcpy(&machine, b + off + 4, sizeof(machine));
    memcpy(&chars, b + off + 22, sizeof(chars));
    if (machine != 0x8664 || !(chars & 0x2000)) {
        snprintf(why, whysz, "%s is not a 64-bit (x64) DLL", path);
        return false;
    }
    return true;
}

typedef DWORD (WINAPI *get_ver_size_fn)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI *get_ver_info_fn)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL (WINAPI *ver_query_fn)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

// File version from the version resource (v = major, minor, build, revision).
static bool file_version(const char *path, uint16_t v[4])
{
    bool ok = false;
    HMODULE ver = LoadLibraryExW(L"version.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ver)
        return false;
    get_ver_size_fn size_fn = (get_ver_size_fn)(void *)GetProcAddress(ver, "GetFileVersionInfoSizeW");
    get_ver_info_fn info_fn = (get_ver_info_fn)(void *)GetProcAddress(ver, "GetFileVersionInfoW");
    ver_query_fn query_fn = (ver_query_fn)(void *)GetProcAddress(ver, "VerQueryValueW");
    void *tmp = talloc_new(NULL);
    if (size_fn && info_fn && query_fn) {
        wchar_t *wpath = W(tmp, path);
        DWORD handle = 0, sz = size_fn(wpath, &handle);
        if (sz) {
            void *data = talloc_size(tmp, sz);
            VS_FIXEDFILEINFO *fi = NULL;
            UINT len = 0;
            if (info_fn(wpath, 0, sz, data) && query_fn(data, L"\\", (LPVOID *)&fi, &len) &&
                fi && len >= sizeof(*fi) && fi->dwSignature == 0xFEEF04BD)
            {
                v[0] = HIWORD(fi->dwFileVersionMS);
                v[1] = LOWORD(fi->dwFileVersionMS);
                v[2] = HIWORD(fi->dwFileVersionLS);
                v[3] = LOWORD(fi->dwFileVersionLS);
                ok = true;
            }
        }
    }
    talloc_free(tmp);
    FreeLibrary(ver);
    return ok;
}

// ---------------------------------------------------------------------------
// Scan: what is installed, where the runtimes come from
// ---------------------------------------------------------------------------

struct comp {
    bool installed;
    bool toolkit;               // source: toolkit (else user)
    char *path;                 // the file, or where it will be installed
};

struct cand {
    char *dir;                  // runtime directory
    bool toolkit;
};

struct scan {
    void *ctx;
    struct catalog cat;
    char *cachedir;
    char *cpu_path;
    bool cpu_ok;
    char *engine_path;          // the cached CUDA engine (where it is or will be installed)
    struct comp c[C_COUNT];
    struct nanum_dl_fst engine_st;
    struct cand cand[MAX_CAND]; // usable runtime directories, best first
    int ncand;
    bool tk_found;
    char *tk_dir;
    char *tk_ver;               // folder-name version of the chosen Toolkit ("13.4") or ""
    char *tk_note;              // why a Toolkit was rejected
    int64_t need;               // bytes still to download
    // Older per-app-version directories under the cuda folder (newest first): the migration
    // source of a runtime that the current directory lacks.
    char *cudaroot;
    struct {
        char name[32];
        char *dir;
    } other[MAX_OTHER];
    int nother;
    bool reused[C_COUNT];       // installed by copying from another directory
    bool work_wanted;           // the worker has something to do without network: copy a file
                                // from another directory, or hash the cached engine once
    bool update_required;       // an engine is there (or in an old directory) but not the one
                                // of the catalog
};

static bool file_has_size(const char *path, int64_t size)
{
    struct nanum_dl_fst f;
    return size > 0 && nanum_dl_stat(path, &f) && f.size == size;
}

// > 0 if version a is newer than b (dotted numbers; names that are no version rank last).
static int ver_cmp(const char *a, const char *b)
{
    int x[4] = {-1, -1, -1, -1}, y[4] = {-1, -1, -1, -1};
    sscanf(a, "%d.%d.%d.%d", &x[0], &x[1], &x[2], &x[3]);
    sscanf(b, "%d.%d.%d.%d", &y[0], &y[1], &y[2], &y[3]);
    for (int i = 0; i < 4; i++) {
        if (x[i] != y[i])
            return x[i] > y[i] ? 1 : -1;
    }
    return strcmp(a, b);
}

static char *marker_path(void *ctx, const char *file)
{
    return talloc_asprintf(ctx, "%s.reused", file);
}

// The other directories under the cuda folder (the old per-app-version ones), newest first.
static void scan_other_versions(struct scan *s)
{
    if (!s->cudaroot)
        return;
    char current[16];
    snprintf(current, sizeof(current), "%d", DEFAULT_CUDA_MAJOR);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(s->ctx, join(s->ctx, s->cudaroot, "*")), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
            continue;
        char *name = mp_to_utf8(s->ctx, fd.cFileName);
        if (strlen(name) >= sizeof(s->other[0].name) || !_stricmp(name, current))
            continue;
        int at = s->nother;
        if (at == MAX_OTHER && ver_cmp(name, s->other[MAX_OTHER - 1].name) <= 0)
            continue;
        if (at == MAX_OTHER)
            at--;
        else
            s->nother++;
        while (at > 0 && ver_cmp(name, s->other[at - 1].name) > 0) {
            s->other[at] = s->other[at - 1];
            at--;
        }
        snprintf(s->other[at].name, sizeof(s->other[at].name), "%s", name);
        s->other[at].dir = join(s->ctx, s->cudaroot, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Validated Toolkit directories, keyed by the identity of the two DLLs: the
// UI refreshes often and a validation reads both files' headers and versions.
struct tk_entry {
    bool used;
    char dir[1024];
    int64_t size[2];
    uint64_t mtime[2];
    bool ok;
    char why[512];
    char ver[32];
};
static struct tk_entry tk_cache[4];     // g_lock
static int tk_cache_next;

static bool check_toolkit_files(struct scan *s, const char *dir, char *const file[2],
                                char *ver, size_t versz, char *why, size_t whysz)
{
    uint16_t v[2][4];
    for (int i = 0; i < 2; i++) {
        if (!pe_check_x64_dll(file[i], why, whysz))
            return false;
        if (!file_version(file[i], v[i])) {
            snprintf(why, whysz, "%s has no readable version information", file[i]);
            return false;
        }
    }
    if (memcmp(v[0], v[1], sizeof(v[0]))) {
        snprintf(why, whysz, "the cuBLAS libraries in %s are from different releases "
                 "(%d.%d.%d.%d and %d.%d.%d.%d)", dir,
                 v[0][0], v[0][1], v[0][2], v[0][3],
                 v[1][0], v[1][1], v[1][2], v[1][3]);
        return false;
    }
    // NVIDIA's file version (e.g. 6.14.11.1310) is not the CUDA/ABI major.
    snprintf(ver, versz, "%d.%d.%d.%d", v[0][0], v[0][1], v[0][2], v[0][3]);
    return true;
}

// Validates a Toolkit runtime directory. true: both DLLs are usable.
// *present: at least one of the runtime DLLs is there (so a rejection is worth
// reporting).
static bool check_toolkit_dir(struct scan *s, const char *dir, char **ver_out, bool *present,
                              char *why, size_t whysz)
{
    char *file[2] = {join(s->ctx, dir, comp_file(&s->cat, C_CUBLAS)),
                     join(s->ctx, dir, comp_file(&s->cat, C_CUBLAS_LT))};
    struct nanum_dl_fst f[2];
    bool have[2] = {nanum_dl_stat(file[0], &f[0]), nanum_dl_stat(file[1], &f[1])};
    *present = have[0] || have[1];
    for (int i = 0; i < 2; i++) {
        if (!have[i]) {
            snprintf(why, whysz, "%s is missing from %s (incomplete CUDA Toolkit)",
                     comp_file(&s->cat, i == 0 ? C_CUBLAS : C_CUBLAS_LT), dir);
            return false;
        }
    }

    bool hit = false, ok = false;
    char ver[32] = "", reason[512] = "";
    mp_mutex_lock(&g_lock);
    for (int i = 0; i < MP_ARRAY_SIZE(tk_cache) && !hit; i++) {
        struct tk_entry *e = &tk_cache[i];
        if (e->used && !strcmp(e->dir, dir) && e->size[0] == f[0].size &&
            e->size[1] == f[1].size && e->mtime[0] == f[0].mtime && e->mtime[1] == f[1].mtime)
        {
            hit = true;
            ok = e->ok;
            snprintf(ver, sizeof(ver), "%s", e->ver);
            snprintf(reason, sizeof(reason), "%s", e->why);
        }
    }
    mp_mutex_unlock(&g_lock);
    if (!hit) {
        ok = check_toolkit_files(s, dir, file, ver, sizeof(ver), reason, sizeof(reason));
        mp_mutex_lock(&g_lock);
        struct tk_entry *e = &tk_cache[tk_cache_next];
        tk_cache_next = (tk_cache_next + 1) % MP_ARRAY_SIZE(tk_cache);
        *e = (struct tk_entry){.used = true, .ok = ok};
        snprintf(e->dir, sizeof(e->dir), "%s", dir);
        e->size[0] = f[0].size;
        e->size[1] = f[1].size;
        e->mtime[0] = f[0].mtime;
        e->mtime[1] = f[1].mtime;
        snprintf(e->ver, sizeof(e->ver), "%s", ver);
        snprintf(e->why, sizeof(e->why), "%s", reason);
        mp_mutex_unlock(&g_lock);
    }
    if (!ok) {
        snprintf(why, whysz, "%s", reason);
        return false;
    }
    *ver_out = talloc_strdup(s->ctx, ver);
    return true;
}

// The minor number of a Toolkit root named like "...\v13.4" (-1 if it is not named that way).
static int minor_of_root(const char *root, int major)
{
    const char *base = strrchr(root, '\\');
    int ma = 0, mi = 0;
    if (sscanf(base ? base + 1 : root, "v%d.%d", &ma, &mi) == 2 && ma == major)
        return mi;
    return -1;
}

struct tk_root {
    char *path;
    int minor;                  // -1: unknown, sorts last
};

static void tk_add(struct scan *s, struct tk_root *roots, int *n, const char *path, int minor)
{
    if (!path || !path[0] || *n >= MAX_TK_ROOTS)
        return;
    char *root = talloc_strdup(s->ctx, path);
    nanum_dl_trim_slashes(root);
    for (int i = 0; i < *n; i++) {
        if (nanum_dl_same_path(roots[i].path, root))
            return;
    }
    roots[*n].path = root;
    roots[*n].minor = minor >= 0 ? minor : minor_of_root(root, s->cat.cuda_major);
    (*n)++;
}

// Name/value pairs of an environment-like source that matter: CUDA_PATH_V<major>_<minor> and
// CUDA_PATH.
static void tk_add_var(struct scan *s, struct tk_root *roots, int *n, const char *name,
                       const char *value)
{
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "CUDA_PATH_V%d_", s->cat.cuda_major);
    size_t plen = strlen(prefix);
    if (!_strnicmp(name, prefix, plen) && isdigit((unsigned char)name[plen]))
        tk_add(s, roots, n, value, atoi(name + plen));
    else if (!_stricmp(name, "CUDA_PATH"))
        tk_add(s, roots, n, value, -1);
}

// A registry key holding environment variables (REG_SZ / REG_EXPAND_SZ values).
static void tk_add_registry(struct scan *s, struct tk_root *roots, int *n, HKEY top,
                            const wchar_t *subkey)
{
    HKEY key;
    if (RegOpenKeyExW(top, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; i++) {
        wchar_t name[256], data[2048], expanded[2048];
        DWORD nl = MP_ARRAY_SIZE(name), dl = sizeof(data) - sizeof(wchar_t), type = 0;
        LONG r = RegEnumValueW(key, i, name, &nl, NULL, &type, (BYTE *)data, &dl);
        if (r == ERROR_NO_MORE_ITEMS)
            break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            continue;
        data[dl / sizeof(wchar_t)] = 0;
        const wchar_t *value = data;
        if (type == REG_EXPAND_SZ && ExpandEnvironmentStringsW(data, expanded, MP_ARRAY_SIZE(expanded)))
            value = expanded;
        tk_add_var(s, roots, n, mp_to_utf8(s->ctx, name), mp_to_utf8(s->ctx, value));
    }
    RegCloseKey(key);
}

// <parent>\v<major>.*
static void tk_add_folder_scan(struct scan *s, struct tk_root *roots, int *n, const char *parent)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "v%d.*", s->cat.cuda_major);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(s->ctx, join(s->ctx, parent, pattern)), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            tk_add(s, roots, n, join(s->ctx, parent, mp_to_utf8(s->ctx, fd.cFileName)), -1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void scan_toolkits(struct scan *s)
{
    struct tk_root roots[MAX_TK_ROOTS];
    int nroots = 0;

    wchar_t over[MAX_PATH * 2];
    DWORD on = GetEnvironmentVariableW(L"NANUMPLAYER_CUDA_TOOLKIT_ROOT", over, MP_ARRAY_SIZE(over));
    if (on > 0 && on < MP_ARRAY_SIZE(over)) {
        // Tests: only the Toolkits below this folder exist.
        tk_add_folder_scan(s, roots, &nroots, mp_to_utf8(s->ctx, over));
    } else {
        // The environment of this process ...
        wchar_t *block = GetEnvironmentStringsW();
        if (block) {
            for (wchar_t *e = block; *e; e += wcslen(e) + 1) {
                wchar_t *eq = wcschr(e + 1, L'=');
                if (!eq)
                    continue;
                *eq = 0;
                tk_add_var(s, roots, &nroots, mp_to_utf8(s->ctx, e), mp_to_utf8(s->ctx, eq + 1));
                *eq = L'=';
            }
            FreeEnvironmentStringsW(block);
        }
        // ... the persistent environment (a Toolkit installed after Explorer started is not in
        // the inherited one) ...
        tk_add_registry(s, roots, &nroots, HKEY_CURRENT_USER, L"Environment");
        tk_add_registry(s, roots, &nroots, HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
        // ... and the installer's default location.
        wchar_t *pf = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_ProgramFiles, 0, NULL, &pf))) {
            char *dir = join(s->ctx, mp_to_utf8(s->ctx, pf), "NVIDIA GPU Computing Toolkit\\CUDA");
            tk_add_folder_scan(s, roots, &nroots, dir);
            CoTaskMemFree(pf);
        }
    }
    for (int i = 1; i < nroots; i++) {          // insertion sort: newest minor first, stable
        struct tk_root t = roots[i];
        int k = i;
        while (k > 0 && roots[k - 1].minor < t.minor) {
            roots[k] = roots[k - 1];
            k--;
        }
        roots[k] = t;
    }

    static const char *const subs[] = {"bin\\x64", "bin"};
    char *seen[2 * MAX_TK_ROOTS];
    int nseen = 0;
    for (int r = 0; r < nroots; r++) {
        char *root = roots[r].path;
        bool root_present = false, root_ok = false;
        char note[640] = "";
        for (int k = 0; k < 2 && s->ncand < MAX_CAND; k++) {
            char *dir = join(s->ctx, root, subs[k]);
            bool dup = false;
            for (int d = 0; d < nseen; d++)
                dup |= nanum_dl_same_path(seen[d], dir);
            if (dup)
                continue;
            if (nseen < MP_ARRAY_SIZE(seen))
                seen[nseen++] = dir;
            char why[512] = "";
            char *ver = NULL;
            bool present = false;
            if (check_toolkit_dir(s, dir, &ver, &present, why, sizeof(why))) {
                s->cand[s->ncand++] = (struct cand){dir, true};
                if (!s->tk_found) {
                    s->tk_found = true;
                    s->tk_dir = dir;
                    s->tk_ver = roots[r].minor >= 0
                                    ? talloc_asprintf(s->ctx, "%d.%d", s->cat.cuda_major,
                                                      roots[r].minor)
                                    : talloc_strdup(s->ctx, "");
                }
                root_ok = true;
                break;      // bin\x64 wins over bin
            }
            if (present) {
                root_present = true;
                if (!note[0])
                    snprintf(note, sizeof(note), "%s", why);
            }
        }
        if (!root_ok && !s->tk_note) {
            if (root_present)
                s->tk_note = talloc_asprintf(s->ctx, "the CUDA Toolkit at %s was rejected: %s",
                                             root, note);
            else
                s->tk_note = talloc_asprintf(s->ctx, "the CUDA Toolkit at %s has no %s "
                                             "(CUDA %d is required)", root,
                                             comp_file(&s->cat, C_CUBLAS), s->cat.cuda_major);
        }
    }
}

static void scan_all(void *ctx, struct scan *s)
{
    *s = (struct scan){.ctx = ctx};
    catalog_get(&s->cat);
    if (!s->cat.cuda_major)
        s->cat.cuda_major = DEFAULT_CUDA_MAJOR;
    mp_mutex_lock(&g_lock);
    const char *root = g.root, *cachedir = g.cachedir, *cudaroot = g.cudaroot;
    bool tried[C_COUNT];
    memcpy(tried, g.reuse_tried, sizeof(tried));
    s->cpu_path = root ? join(ctx, root, CPU_ENGINE_FILE) : NULL;
    s->cachedir = cachedir ? talloc_strdup(ctx, cachedir) : NULL;
    s->cudaroot = cudaroot ? talloc_strdup(ctx, cudaroot) : NULL;
    mp_mutex_unlock(&g_lock);
    struct nanum_dl_fst f;
    s->cpu_ok = s->cpu_path && nanum_dl_stat(s->cpu_path, &f) && f.size > 0;

    bool cache_ok[C_COUNT] = {0};
    for (int i = 0; i < C_COUNT; i++) {
        s->c[i].path = s->cachedir ? join(ctx, s->cachedir, comp_file(&s->cat, i))
                                   : talloc_strdup(ctx, comp_file(&s->cat, i));
        cache_ok[i] = s->cat.ok && s->cachedir &&
                      file_has_size(s->c[i].path, s->cat.item[i].size);
    }

    // The engine lives in the cache. Its SHA-256 is checked once per file identity by the worker
    // (until then the matching size counts); a mismatch means it is an older engine.
    s->engine_path = s->c[C_ENGINE].path;
    bool engine_file = nanum_dl_stat(s->engine_path, &s->engine_st);
    bool engine_unverified = false, engine_bad = false;
    if (cache_ok[C_ENGINE]) {
        char key[1700];
        snprintf(key, sizeof(key), "%s|%" PRId64 "|%" PRIu64, s->engine_path, s->engine_st.size,
                 s->engine_st.mtime);
        mp_mutex_lock(&g_lock);
        bool known = !strcmp(key, g.eng_key);
        bool good = g.eng_good;
        mp_mutex_unlock(&g_lock);
        engine_unverified = !known;
        engine_bad = known && !good;
        s->c[C_ENGINE].installed = !engine_bad;
    }

    if (cache_ok[C_CUBLAS] && cache_ok[C_CUBLAS_LT])
        s->cand[s->ncand++] = (struct cand){s->cachedir, false};
    scan_toolkits(s);

    if (s->ncand > 0) {
        // The runtime pair comes from the best directory, as a whole.
        for (int i = C_CUBLAS; i <= C_CUBLAS_LT; i++) {
            s->c[i].installed = true;
            s->c[i].toolkit = s->cand[0].toolkit;
            s->c[i].path = join(ctx, s->cand[0].dir, comp_file(&s->cat, i));
        }
    } else {
        for (int i = C_CUBLAS; i <= C_CUBLAS_LT; i++)
            s->c[i].installed = cache_ok[i];
    }
    for (int i = 0; i < C_COUNT; i++) {
        s->reused[i] = s->c[i].installed && !s->c[i].toolkit &&
                       nanum_dl_stat(marker_path(ctx, s->c[i].path), &f);
    }
    // Migration from an old per-app-version directory: only for what nothing else (the cache,
    // a Toolkit for the runtimes) provides.
    bool engine_migrates = false, engine_old = engine_file && !s->c[C_ENGINE].installed;
    if (s->cat.ok && s->cachedir) {
        scan_other_versions(s);
        for (int i = 0; i < C_COUNT; i++) {
            if (s->c[i].installed || tried[i] || (i != C_ENGINE && s->ncand > 0))
                continue;
            for (int k = 0; k < s->nother; k++) {
                char *src = join(ctx, s->other[k].dir, comp_file(&s->cat, i));
                if (file_has_size(src, s->cat.item[i].size)) {
                    s->work_wanted = true;
                    engine_migrates |= i == C_ENGINE;
                    break;
                }
            }
        }
        for (int k = 0; k < s->nother && !engine_old; k++)
            engine_old = nanum_dl_stat(join(ctx, s->other[k].dir, KNOWN[C_ENGINE].file), &f);
    }
    s->work_wanted |= engine_unverified;
    s->update_required = s->cat.ok && !s->c[C_ENGINE].installed && engine_old && !engine_migrates;
    if (s->cat.ok) {
        for (int i = 0; i < C_COUNT; i++) {
            if (!s->c[i].installed)
                s->need += s->cat.item[i].size;
        }
    }
}

// Why the CUDA files are not there (NULL: everything is in place).
static char *missing_text(struct scan *s)
{
    if (!s->cat.ok)
        return talloc_asprintf(s->ctx, "CUDA acceleration is unavailable: %s. Reinstall "
                               "NanumPlayer to restore it.", s->cat.err);
    char *list = NULL;
    bool runtime_missing = false;
    for (int i = 0; i < C_COUNT; i++) {
        if (s->c[i].installed)
            continue;
        runtime_missing |= i != C_ENGINE;
        list = list ? talloc_asprintf(s->ctx, "%s, %s", list, s->cat.item[i].file)
                    : talloc_strdup(s->ctx, s->cat.item[i].file);
    }
    if (!list)
        return NULL;
    return talloc_asprintf(s->ctx, "CUDA acceleration is not installed: %s missing (%" PRId64
                           " MiB to download%s%s).", list, mib_up(s->need),
                           runtime_missing ? "; " : "",
                           runtime_missing ? (s->tk_note ? s->tk_note
                                              : "no compatible CUDA Toolkit was found") : "");
}

// The text for the user while auto mode runs on the CPU for lack of files.
static char *auto_notice(struct scan *s)
{
    if (!s->cat.ok)
        return missing_text(s);
    int64_t engine = (s->cat.item[C_ENGINE].size + (1 << 19)) >> 20;
    int64_t pair = (s->cat.item[C_CUBLAS].size + s->cat.item[C_CUBLAS_LT].size + (1 << 19)) >> 20;
    bool runtime = s->c[C_CUBLAS].installed && s->c[C_CUBLAS_LT].installed;
    if (s->update_required)
        return talloc_asprintf(s->ctx, "CUDA 엔진 업데이트 필요 (약 %" PRId64 " MiB)", engine);
    if (!s->c[C_ENGINE].installed) {
        if (runtime)
            return talloc_asprintf(s->ctx, "CUDA 가속: 엔진(약 %" PRId64 " MiB)을 받으면 GPU를 "
                                   "사용합니다", engine);
        return talloc_asprintf(s->ctx, "CUDA 가속: 엔진(약 %" PRId64 " MiB)과 cuBLAS(약 %" PRId64
                               " MiB)를 받으면 GPU를 사용합니다 (CUDA %d Toolkit이 있으면 엔진만)",
                               engine, pair, s->cat.cuda_major);
    }
    if (!runtime)
        return talloc_asprintf(s->ctx, "CUDA 가속: CUDA %d Toolkit을 설치하거나 cuBLAS(약 %"
                               PRId64 " MiB)를 받으세요", s->cat.cuda_major, pair);
    return missing_text(s);
}

// ---------------------------------------------------------------------------
// Engine probe cache (key = engine file identity + runtime directory)
// ---------------------------------------------------------------------------

static void probe_key(const struct scan *s, int cand, char *out, size_t sz)
{
    snprintf(out, sz, "%s|%s|%" PRId64 "|%" PRIu64, s->engine_path ? s->engine_path : "",
             s->cand[cand].dir, s->engine_st.size, s->engine_st.mtime);
}

static void probe_store(const char *key, bool ok, const char *reason)
{
    mp_mutex_lock(&g_lock);
    struct probe_entry *e = NULL;
    for (int i = 0; i < MAX_PROBE && !e; i++) {
        if (g.probe[i].used && !strcmp(g.probe[i].key, key))
            e = &g.probe[i];
    }
    if (!e) {
        e = &g.probe[g.probe_next];
        g.probe_next = (g.probe_next + 1) % MAX_PROBE;
    }
    e->used = true;
    e->ok = ok;
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->reason, sizeof(e->reason), "%s", reason);
    mp_mutex_unlock(&g_lock);
}

// 1 ok, 0 failed, -1 never probed
static int probe_lookup(const char *key, char *reason, size_t sz)
{
    int r = -1;
    mp_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_PROBE; i++) {
        if (g.probe[i].used && !strcmp(g.probe[i].key, key)) {
            r = g.probe[i].ok;
            snprintf(reason, sz, "%s", g.probe[i].reason);
            break;
        }
    }
    mp_mutex_unlock(&g_lock);
    return r;
}

enum cuda_state { CUDA_NO_FILES, CUDA_UNPROBED, CUDA_BAD, CUDA_OK };

// Decision from files and cached probes only (never loads anything). *why is
// the text for everything but CUDA_OK.
static enum cuda_state cuda_state_of(struct scan *s, char **why)
{
    *why = missing_text(s);
    if (*why)
        return CUDA_NO_FILES;
    char first[640] = "";
    for (int i = 0; i < s->ncand; i++) {
        char key[1700], reason[640];
        probe_key(s, i, key, sizeof(key));
        int p = probe_lookup(key, reason, sizeof(reason));
        if (p == 1)
            return CUDA_OK;
        if (p < 0) {
            *why = talloc_strdup(s->ctx, "Checking the CUDA installation...");
            return CUDA_UNPROBED;
        }
        if (!first[0])
            snprintf(first, sizeof(first), "%s", reason);
    }
    *why = talloc_asprintf(s->ctx, "CUDA acceleration is installed but cannot be used: %s",
                           first[0] ? first : "the CUDA engine could not be loaded");
    return CUDA_BAD;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

typedef const char *(*backend_fn)(void);
typedef int (*gpu_fn)(void);
typedef const char *(*lasterr_fn)(void);

#define SEARCH_APP_ENGINE (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | \
                           LOAD_LIBRARY_SEARCH_SYSTEM32)
#define SEARCH_RUNTIME (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)

static void load_error(char *out, size_t sz, const char *what, const char *path, DWORD code)
{
    char why[1024];
    nanum_dl_error_text(why, sizeof(why), code);
    snprintf(out, sz, "cannot load %s (%s): %s%s", what, path, why,
             code == ERROR_MOD_NOT_FOUND
                 ? " A DLL it depends on was not found (for the CUDA engine: the NVIDIA "
                   "display driver or the Visual C++ runtime)." : "");
}

// MSVC OpenMP owns idle worker threads beyond an AI context's lifetime.
// Unloading its last importing engine can unmap VCOMP140 while those threads
// are still returning through it. Keep the runtime (not either engine) resident
// until Windows terminates the process; engines and CUDA libraries still unload.
static bool pin_openmp_runtime(char *err, size_t errsz)
{
    static bool pinned; // guarded by g_lock, shared by all engines/probes
    bool ok = true;
    mp_mutex_lock(&g_lock);
    if (!pinned) {
        HMODULE omp = GetModuleHandleW(L"VCOMP140.DLL");
        if (omp) {
            HMODULE retained;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                  (LPCWSTR)omp, &retained)) {
                pinned = true;
            } else {
                DWORD code = GetLastError();
                nanum_dl_error_text(err, errsz, code);
                ok = false;
            }
        }
    }
    mp_mutex_unlock(&g_lock);
    return ok;
}

// Every managed engine implements the current backend/probe contract.
// CUDA additionally requires a successful GPU probe.
static bool query_engine(HMODULE h, bool want_cuda, char *err, size_t errsz)
{
    backend_fn backend = (backend_fn)(void *)GetProcAddress(h, "nanum_ai_backend");
    gpu_fn gpu = (gpu_fn)(void *)GetProcAddress(h, "nanum_ai_gpu_available");
    lasterr_fn last = (lasterr_fn)(void *)GetProcAddress(h, "nanum_ai_last_error");
    if (!backend || !gpu) {
        snprintf(err, errsz, "the engine does not export nanum_ai_backend / "
                 "nanum_ai_gpu_available (it is not a NanumPlayer engine)");
        return false;
    }
    const char *b = backend();
    if (!b || strcmp(b, want_cuda ? "cuda" : "cpu")) {
        snprintf(err, errsz, "the engine reports the backend '%s', expected '%s'",
                 b ? b : "", want_cuda ? "cuda" : "cpu");
        return false;
    }
    if (want_cuda && !gpu()) {
        const char *why = last ? last() : NULL;
        snprintf(err, errsz, "the CUDA engine cannot use the GPU: %s",
                 why && why[0] ? why : "no compatible NVIDIA GPU (compute capability 8.6 or "
                                       "newer) and driver were found");
        return false;
    }
    return true;
}

// Loads the runtime pair from rtdir, then the engine from its own folder.
// keep == NULL: probe only, nothing stays loaded.
static bool load_cuda_pair(const char *engine, const char *rtdir, struct nanum_engine *keep,
                           char *err, size_t errsz)
{
    void *tmp = talloc_new(NULL);
    HMODULE rt[2] = {0};
    HMODULE eng = NULL;
    // Loaded in dependency order: cublas imports cublasLt.
    static const int order[2] = {C_CUBLAS_LT, C_CUBLAS};
    DWORD old_mode = 0;
    bool mode_set = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    bool ok = false;

    for (int i = 0; i < 2; i++) {
        char *path = join(tmp, rtdir, KNOWN[order[i]].file);
        rt[i] = LoadLibraryExW(W(tmp, path), NULL, SEARCH_RUNTIME);
        if (!rt[i]) {
            load_error(err, errsz, KNOWN[order[i]].file, path, GetLastError());
            goto out;
        }
    }
    eng = LoadLibraryExW(W(tmp, engine), NULL, SEARCH_APP_ENGINE);
    if (!eng) {
        load_error(err, errsz, "the CUDA engine", engine, GetLastError());
        goto out;
    }
    if (!query_engine(eng, true, err, errsz))
        goto out;
    ok = true;
out:
    if (ok && keep) {
        keep->module = eng;
        keep->cuda = true;
        keep->use_gpu = 1;
        snprintf(keep->backend, sizeof(keep->backend), "cuda");
        snprintf(keep->path, sizeof(keep->path), "%s", engine);
        keep->runtime[0] = rt[0];
        keep->runtime[1] = rt[1];
        keep->num_runtime = 2;
    } else {
        if (eng)
            FreeLibrary(eng);
        for (int i = 1; i >= 0; i--) {
            if (rt[i])
                FreeLibrary(rt[i]);
        }
    }
    if (mode_set)
        SetThreadErrorMode(old_mode, NULL);
    talloc_free(tmp);
    return ok;
}

// Tries the runtime directories in order and records every outcome.
static bool try_cuda(struct scan *s, struct nanum_engine *keep, char *err, size_t errsz)
{
    err[0] = 0;
    for (int i = 0; i < s->ncand; i++) {
        char key[1700], why[640] = "";
        probe_key(s, i, key, sizeof(key));
        if (load_cuda_pair(s->engine_path, s->cand[i].dir, keep, why, sizeof(why))) {
            probe_store(key, true, "");
            return true;
        }
        probe_store(key, false, why);
        size_t len = strlen(err);
        if (s->ncand == 1)
            snprintf(err, errsz, "%s", why);
        else
            snprintf(err + len, errsz - len, "%s%s: %s", len ? "; " : "", s->cand[i].dir, why);
    }
    return false;
}

static void remember_backend(const char *backend)
{
    mp_mutex_lock(&g_lock);
    snprintf(g.last_backend, sizeof(g.last_backend), "%s", backend);
    mp_mutex_unlock(&g_lock);
}

enum cuda_load { CL_OK, CL_ABSENT, CL_BROKEN };

// required: the CUDA mode (the text says what to do); else the auto-mode notice.
static enum cuda_load load_cuda(void *ctx, struct nanum_engine *out, char *err, size_t errsz,
                                bool required)
{
    // The files of an older version are being copied into place (see run_reuse): the first
    // file after startup waits for that (bounded) instead of silently using the CPU.
    for (int w = 0; w < 600; w++) {
        mp_mutex_lock(&g_lock);
        bool copying = g.running && g.running_kind == JOB_REUSE && !g.shutting_down;
        mp_mutex_unlock(&g_lock);
        if (!copying)
            break;
        Sleep(50);
    }
    struct scan *s = talloc_zero(ctx, struct scan);
    scan_all(ctx, s);
    s->ctx = ctx;
    char *missing = required ? missing_text(s) : auto_notice(s);
    if (missing) {
        snprintf(err, errsz, "%s", missing);
        return CL_ABSENT;
    }
    if (try_cuda(s, out, err, errsz))
        return CL_OK;
    char tmp[sizeof(((struct nanum_engine *)0)->notice)];
    snprintf(tmp, sizeof(tmp), "CUDA acceleration is installed but cannot be used: %s", err);
    snprintf(err, errsz, "%s", tmp);
    return CL_BROKEN;
}

static bool load_cpu(void *ctx, struct nanum_engine *out, char *err, size_t errsz)
{
    char *path = NULL;
    mp_mutex_lock(&g_lock);
    if (g.root)
        path = join(ctx, g.root, CPU_ENGINE_FILE);
    mp_mutex_unlock(&g_lock);
    struct nanum_dl_fst f;
    if (!path || !nanum_dl_stat(path, &f)) {
        snprintf(err, errsz, "the CPU AI engine %s was not found%s%s. Reinstall NanumPlayer.",
                 CPU_ENGINE_FILE, path ? " at " : "", path ? path : "");
        return false;
    }
    DWORD old_mode = 0;
    bool mode_set = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    HMODULE h = LoadLibraryExW(W(ctx, path), NULL, SEARCH_APP_ENGINE);
    bool ok = false;
    if (!h) {
        load_error(err, errsz, "the CPU AI engine", path, GetLastError());
    } else if (!query_engine(h, false, err, errsz)) {
        FreeLibrary(h);
    } else {
        out->module = h;
        out->cuda = false;
        out->use_gpu = 0;
        snprintf(out->backend, sizeof(out->backend), "cpu");
        snprintf(out->path, sizeof(out->path), "%s", path);
        ok = true;
    }
    if (mode_set)
        SetThreadErrorMode(old_mode, NULL);
    return ok;
}

// --nanum-ai-dll: an explicit user override, never silently replaced.
// Custom engines must implement the current backend/probe contract too.
static bool load_custom(void *ctx, enum nanum_accel_mode mode, const char *dll,
                        struct nanum_engine *out, char *err, size_t errsz)
{
    HMODULE h = LoadLibraryW(W(ctx, dll));
    if (!h) {
        load_error(err, errsz, "the engine set by --nanum-ai-dll", dll, GetLastError());
        return false;
    }
    backend_fn backend = (backend_fn)(void *)GetProcAddress(h, "nanum_ai_backend");
    gpu_fn gpu = (gpu_fn)(void *)GetProcAddress(h, "nanum_ai_gpu_available");
    lasterr_fn last = (lasterr_fn)(void *)GetProcAddress(h, "nanum_ai_last_error");
    const char *b = backend ? backend() : NULL;
    bool cuda = b && !strcmp(b, "cuda");
    if (!backend || !gpu || !last || !b || (strcmp(b, "cpu") && strcmp(b, "cuda"))) {
        snprintf(err, errsz, "The engine set by --nanum-ai-dll (%s) does not implement "
                 "the current CPU/CUDA backend contract. Rebuild it for this NanumPlayer version.", dll);
        FreeLibrary(h);
        return false;
    }
    bool gpu_ok = cuda && mode != NANUM_ACCEL_CPU && gpu();
    int use_gpu;
    if (mode == NANUM_ACCEL_CUDA && !(cuda && gpu_ok)) {
        const char *why = last ? last() : NULL;
        snprintf(err, errsz, "CUDA acceleration is required but the engine set by "
                 "--nanum-ai-dll (%s) cannot provide it: %s", dll,
                 !cuda ? "it is a CPU engine" : why && why[0] ? why : "the GPU cannot be used");
        FreeLibrary(h);
        return false;
    } else {
        use_gpu = mode != NANUM_ACCEL_CPU && cuda && gpu_ok;
        if (mode == NANUM_ACCEL_AUTO && cuda && !gpu_ok) {
            const char *why = last ? last() : NULL;
            snprintf(out->notice, sizeof(out->notice), "CUDA is not used by the engine set by "
                     "--nanum-ai-dll: %s", why && why[0] ? why : "the GPU cannot be used");
        }
        cuda = use_gpu;
    }
    out->module = h;
    out->cuda = cuda && use_gpu;
    out->use_gpu = use_gpu;
    snprintf(out->backend, sizeof(out->backend), "%s", out->cuda ? "cuda" : "cpu");
    snprintf(out->path, sizeof(out->path), "%s", dll);
    return true;
}

bool nanum_accel_load_engine(struct mp_log *log, enum nanum_accel_mode mode,
                             const char *custom_dll, struct nanum_engine *out,
                             char *err, size_t errsz)
{
    *out = (struct nanum_engine){0};
    err[0] = 0;
    ensure_init(NULL);
    void *tmp = talloc_new(NULL);
    bool ok = false;

    if (custom_dll && custom_dll[0]) {
        ok = load_custom(tmp, mode, custom_dll, out, err, errsz);
        goto done;
    }

    char cuda_err[sizeof(out->notice)] = "";
    if (mode != NANUM_ACCEL_CPU) {
        enum cuda_load r = load_cuda(tmp, out, cuda_err, sizeof(cuda_err),
                                     mode == NANUM_ACCEL_CUDA);
        if (r == CL_OK) {
            ok = true;
            goto done;
        }
        if (mode == NANUM_ACCEL_CUDA) {
            snprintf(err, errsz, "CUDA acceleration is required (acceleration mode \"cuda\") "
                     "but unavailable, so no AI subtitles will be produced. %s %s", cuda_err,
                     r == CL_ABSENT ? "Install it in the AI settings, or set the acceleration "
                                      "mode to Auto or CPU."
                                    : "Set the acceleration mode to Auto or CPU to use the "
                                      "CPU engine.");
            goto done;
        }
        if (r == CL_BROKEN)
            mp_warn(log, "%s Using the CPU engine.\n", cuda_err);
        else
            mp_verbose(log, "%s Using the CPU engine.\n", cuda_err);
    }
    if (!load_cpu(tmp, out, err, errsz)) {
        if (cuda_err[0]) {
            size_t len = strlen(err);
            snprintf(err + len, errsz - len, " (%s)", cuda_err);
        }
        goto done;
    }
    snprintf(out->notice, sizeof(out->notice), "%s", cuda_err);
    ok = true;
done:
    if (ok && !pin_openmp_runtime(err, errsz)) {
        nanum_accel_unload_engine(out);
        ok = false;
    }
    if (ok)
        remember_backend(out->backend);
    else
        *out = (struct nanum_engine){0};
    talloc_free(tmp);
    return ok;
}

void nanum_accel_unload_engine(struct nanum_engine *e)
{
    if (e->module)
        FreeLibrary((HMODULE)e->module);
    for (int i = e->num_runtime - 1; i >= 0; i--) {
        if (e->runtime[i])
            FreeLibrary((HMODULE)e->runtime[i]);
    }
    *e = (struct nanum_engine){0};
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
static void accel_phase(struct nanum_dl *dl, enum nanum_dl_phase ph, int64_t recv,
                        int64_t total)
{
    static const enum state map[] = {ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING};
    set_state(map[ph], dl->tag, recv, total, NULL);
}

static void accel_progress(struct nanum_dl *dl, int64_t recv)
{
    set_progress(recv, false);
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

struct jcomp {
    int idx;
    char *path;             // final file (UTF-8)
    struct nanum_dl_item e;
};

struct job {
    enum job_kind kind;
    int n;
    struct jcomp c[C_COUNT];
};


// Records that the cached engine in its current form was (or was not) the catalog's file.
static void eng_mark(const char *path, bool good)
{
    struct nanum_dl_fst f;
    if (!nanum_dl_stat(path, &f))
        return;
    mp_mutex_lock(&g_lock);
    snprintf(g.eng_key, sizeof(g.eng_key), "%s|%" PRId64 "|%" PRIu64, path, f.size, f.mtime);
    g.eng_good = good;
    mp_mutex_unlock(&g_lock);
}

static enum nanum_dl_res process_component(struct jcomp *c)
{
    struct nanum_dl *dl = &g.dl;
    dl->tag = c->idx;
    set_state(ST_CHECKING, c->idx, 0, c->e.size, NULL);
    if (nanum_dl_cancelled(dl))
        return NANUM_DL_CANCEL;

    struct nanum_dl_fst f;
    if (nanum_dl_stat(c->path, &f) && f.size == c->e.size) {
        // Appeared since the plan was made (the installer, another player).
        bool match = false;
        enum nanum_dl_res h = nanum_dl_hash(dl, &c->e, c->path, &match);
        if (h != NANUM_DL_OK)
            return h;
        if (match) {
            mp_info(g.log, "component '%s': %s is already installed and verified\n",
                    KNOWN[c->idx].id, c->path);
            if (c->idx == C_ENGINE)
                eng_mark(c->path, true);
            return NANUM_DL_OK;
        }
    }

    if (!nanum_dl_prepare_dir(dl, c->path, c->e.size, c->e.file))
        return NANUM_DL_FAIL;

    // One .part per process: concurrent writers never share a temporary file.
    void *tmp = talloc_new(NULL);
    // A component that is downloaded again is no longer a copy of an older one.
    DeleteFileW(W(tmp, marker_path(tmp, c->path)));
    char *part = talloc_asprintf(tmp, "%s.%lu.part", c->path, (unsigned long)GetCurrentProcessId());
    mp_info(g.log, "component '%s': downloading %s\n", KNOWN[c->idx].id, c->e.file);
    enum nanum_dl_res r = nanum_dl_fetch(dl, &c->e, part);
    if (r == NANUM_DL_OK)
        r = nanum_dl_install(dl, &c->e, c->path, part, &f);
    if (r != NANUM_DL_OK)
        DeleteFileW(W(tmp, part));
    if (r == NANUM_DL_OK && c->idx == C_ENGINE)
        eng_mark(c->path, true);
    talloc_free(tmp);
    return r;
}

static DWORD CALLBACK copy_progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER s1,
                                    LARGE_INTEGER s2, DWORD stream, DWORD reason, HANDLE src,
                                    HANDLE dst, LPVOID data)
{
    set_progress(done.QuadPart, false);
    return nanum_dl_cancelled(&g.dl) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

// The NVIDIA license text is accepted once; the same text in an old directory carries that
// acceptance over together with the migrated runtimes.
static void copy_license(void *ctx, struct scan *s)
{
    static const char *const name = "LICENSE-NVIDIA-CUDA.txt";
    char *dst = join(ctx, s->cachedir, name);
    struct nanum_dl_fst f;
    if (nanum_dl_stat(dst, &f) ||
        !file_has_size(join(ctx, s->cachedir, comp_file(&s->cat, C_CUBLAS)),
                       s->cat.item[C_CUBLAS].size))
        return;
    mp_mutex_lock(&g_lock);
    char *root = g.root ? talloc_strdup(ctx, g.root) : NULL;
    mp_mutex_unlock(&g_lock);
    char *bundled = root ? nanum_dl_read_text(ctx, join(ctx, root, name)) : NULL;
    for (int k = 0; bundled && k < s->nother; k++) {
        char *src = join(ctx, s->other[k].dir, name);
        char *text = nanum_dl_read_text(ctx, src);
        if (text && !strcmp(text, bundled)) {
            CopyFileW(W(ctx, src), W(ctx, dst), TRUE);
            return;
        }
    }
}

// Components that this version's directory lacks but another version's directory has with
// exactly the catalog size and SHA-256 are copied (temp file, verify, atomic rename): an app
// update with an unchanged engine needs no download. No network. Failures are not fatal.
static enum nanum_dl_res run_reuse(void)
{
    struct nanum_dl *dl = &g.dl;
    void *tmp = talloc_new(NULL);
    struct scan *s = talloc_zero(tmp, struct scan);
    scan_all(tmp, s);
    enum nanum_dl_res r = NANUM_DL_OK;
    if (!s->cat.ok || !s->cachedir)
        goto out;
    // The cached engine is hashed once per file identity: a file of the catalog's size that is
    // not the catalog's file is an older engine.
    if (s->c[C_ENGINE].installed && file_has_size(s->c[C_ENGINE].path, s->cat.item[C_ENGINE].size)) {
        char key[1700];
        snprintf(key, sizeof(key), "%s|%" PRId64 "|%" PRIu64, s->engine_path, s->engine_st.size,
                 s->engine_st.mtime);
        mp_mutex_lock(&g_lock);
        bool known = !strcmp(key, g.eng_key);
        mp_mutex_unlock(&g_lock);
        if (!known) {
            bool match = false;
            dl->tag = C_ENGINE;
            set_state(ST_CHECKING, C_ENGINE, 0, s->cat.item[C_ENGINE].size, NULL);
            r = nanum_dl_hash(dl, &s->cat.item[C_ENGINE], s->engine_path, &match);
            if (r == NANUM_DL_FAIL) {
                mp_warn(g.log, "cannot verify %s: %s\n", s->engine_path, dl->err);
                r = NANUM_DL_OK;
            } else if (r == NANUM_DL_OK) {
                eng_mark(s->engine_path, match);
                if (!match)
                    mp_warn(g.log, "%s is not the engine of the catalog (older version); "
                            "update required\n", s->engine_path);
            }
        }
    }
    for (int i = 0; i < C_COUNT && r == NANUM_DL_OK; i++) {
        if (s->c[i].installed)
            continue;
        const struct nanum_dl_item *e = &s->cat.item[i];
        for (int k = 0; k < s->nother && r == NANUM_DL_OK; k++) {
            char *src = join(tmp, s->other[k].dir, e->file);
            if (!file_has_size(src, e->size))
                continue;
            dl->tag = i;
            set_state(ST_CHECKING, i, 0, e->size, NULL);
            if (!nanum_dl_prepare_dir(dl, s->c[i].path, e->size, e->file)) {
                mp_warn(g.log, "cannot reuse %s: %s\n", src, dl->err);
                break;
            }
            char *part = talloc_asprintf(tmp, "%s.%lu.part", s->c[i].path,
                                         (unsigned long)GetCurrentProcessId());
            mp_info(g.log, "component '%s': copying %s from version %s\n", KNOWN[i].id, e->file,
                    s->other[k].name);
            bool ok = CopyFileExW(W(tmp, src), W(tmp, part), copy_progress, NULL, NULL, 0);
            if (!ok && nanum_dl_cancelled(dl))
                r = NANUM_DL_CANCEL;
            bool match = false;
            if (ok)
                r = nanum_dl_hash(dl, e, part, &match);
            if (ok && r == NANUM_DL_OK && match) {
                struct nanum_dl_fst f;
                nanum_dl_stat(s->c[i].path, &f);
                r = nanum_dl_install(dl, e, s->c[i].path, part, &f);
                if (r == NANUM_DL_OK) {
                    char *marker = marker_path(tmp, s->c[i].path);
                    HANDLE h = CreateFileW(W(tmp, marker), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, NULL);
                    if (h != INVALID_HANDLE_VALUE) {
                        DWORD n;
                        WriteFile(h, s->other[k].name, (DWORD)strlen(s->other[k].name), &n, NULL);
                        CloseHandle(h);
                    }
                    mp_info(g.log, "component '%s': reused from version %s\n", KNOWN[i].id,
                            s->other[k].name);
                    break;
                }
            }
            DeleteFileW(W(tmp, part));
            if (r == NANUM_DL_FAIL) {
                mp_warn(g.log, "cannot reuse %s: %s\n", src, dl->err);
                r = NANUM_DL_OK;
            } else if (r == NANUM_DL_OK && ok && !match) {
                mp_warn(g.log, "%s does not match the catalog, not reused\n", src);
            }
        }
    }
    if (r == NANUM_DL_OK) {
        copy_license(tmp, s);
        mp_mutex_lock(&g_lock);
        for (int i = 0; i < C_COUNT; i++)
            g.reuse_tried[i] = true;
        mp_mutex_unlock(&g_lock);
    }
out:
    talloc_free(tmp);
    return r;
}

// Loads what is installed exactly as an engine would, so "cuda_available" is
// decided by the real thing (engine + GPU probe), never by file names.
static void probe_installed(void)
{
    void *tmp = talloc_new(NULL);
    struct scan *s = talloc_zero(tmp, struct scan);
    scan_all(tmp, s);
    char *missing = missing_text(s);
    if (!missing) {
        char err[1024];
        if (try_cuda(s, NULL, err, sizeof(err)))
            mp_info(g.log, "CUDA acceleration is ready\n");
        else
            mp_warn(g.log, "CUDA acceleration is installed but cannot be used: %s\n", err);
    }
    talloc_free(tmp);
}

static MP_THREAD_VOID worker_main(void *p)
{
    struct job *j = p;
    mp_thread_set_name("nanum/accel");

    enum nanum_dl_res r = NANUM_DL_OK;
    int failed = -1;
    if (j->kind != JOB_PROBE)
        r = run_reuse();
    for (int i = 0; i < j->n && r == NANUM_DL_OK; i++) {
        r = process_component(&j->c[i]);
        if (r == NANUM_DL_FAIL)
            failed = j->c[i].idx;
        if (r == NANUM_DL_OK && nanum_dl_cancelled(&g.dl))
            r = NANUM_DL_CANCEL;
    }
    if (r == NANUM_DL_OK) {
        // Downloaded files are used from the next opened file on; check them
        // (and the GPU) now so the state is known when the UI looks.
        set_state(ST_CHECKING, -1, 0, 0, NULL);
        probe_installed();
    }

    mp_mutex_lock(&g_lock);
    if (j->kind != JOB_DOWNLOAD && r == NANUM_DL_OK)
        g.st = ST_IDLE;
    else
        g.st = r == NANUM_DL_OK ? ST_DONE : r == NANUM_DL_CANCEL ? ST_CANCELLED : ST_ERROR;
    g.cur = failed;
    if (r != NANUM_DL_FAIL)
        g.recv = g.total = 0;
    snprintf(g.err, sizeof(g.err), "%s", r == NANUM_DL_FAIL ? g.dl.err : "");
    g.running = false;
    mp_mutex_unlock(&g_lock);
    if (r == NANUM_DL_FAIL)
        mp_err(g.log, "accelerator download failed: %s\n", g.dl.err);
    else if (j->kind == JOB_DOWNLOAD)
        mp_info(g.log, "accelerator download %s\n", r == NANUM_DL_OK ? "finished" : "cancelled");
    request_publish();

    nanum_dl_end(&g.dl);
    for (int i = 0; i < j->n; i++)
        talloc_free(j->c[i].path);
    talloc_free(j);
    MP_THREAD_RETURN();
}

// ---------------------------------------------------------------------------
// Core thread: option lookup, status, commands
// ---------------------------------------------------------------------------

static struct m_config_option *option(struct MPContext *mpctx, const char *name)
{
    struct m_config_option *co = m_config_get_co(mpctx->mconfig, bstr0(name));
    return co && co->data ? co : NULL;
}

static enum nanum_accel_mode option_mode(struct MPContext *mpctx)
{
    struct m_config_option *co = option(mpctx, "nanum-acceleration");
    int v = co ? *(int *)co->data : NANUM_ACCEL_AUTO;
    return v == NANUM_ACCEL_CPU || v == NANUM_ACCEL_CUDA ? v : NANUM_ACCEL_AUTO;
}

static const char *option_custom_dll(struct MPContext *mpctx)
{
    struct m_config_option *co = option(mpctx, "nanum-ai-dll");
    const char *s = co ? *(char **)co->data : NULL;
    return s && s[0] ? s : NULL;
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
    char err[sizeof(g.err)], last_backend[sizeof(g.last_backend)];
    memcpy(err, g.err, sizeof(err));
    memcpy(last_backend, g.last_backend, sizeof(last_backend));
    mp_mutex_unlock(&g_lock);

    struct scan *s = talloc_zero(tmp, struct scan);
    scan_all(tmp, s);
    enum nanum_accel_mode mode = option_mode(mpctx);
    const char *custom = option_custom_dll(mpctx);

    char *cuda_why = NULL;
    enum cuda_state cs = cuda_state_of(s, &cuda_why);
    bool cuda_ok = cs == CUDA_OK;
    // The user-facing text of auto mode when files are missing (cuda mode keeps the
    // detailed one).
    char *auto_why = cs == CUDA_NO_FILES ? auto_notice(s) : cuda_why;

    const char *backend;
    char *reason = (char *)"";
    bool cpu_ok = s->cpu_ok;
    if (custom) {
        struct nanum_dl_fst f;
        cpu_ok = nanum_dl_stat(custom, &f) && f.size > 0;
        cuda_ok = !strcmp(last_backend, "cuda");
        backend = !cpu_ok ? "unavailable" : last_backend[0] ? last_backend : "cpu";
        if (!cpu_ok)
            reason = talloc_asprintf(tmp, "The engine set by --nanum-ai-dll does not exist: %s",
                                     custom);
    } else if (mode == NANUM_ACCEL_CPU) {
        backend = cpu_ok ? "cpu" : "unavailable";
    } else if (mode == NANUM_ACCEL_AUTO) {
        backend = cuda_ok ? "cuda" : cpu_ok ? "cpu" : "unavailable";
        if (!cuda_ok)
            reason = auto_why;
    } else {
        backend = cuda_ok ? "cuda" : "unavailable";
        if (!cuda_ok)
            reason = talloc_asprintf(tmp, "CUDA acceleration is required (mode \"cuda\"): %s",
                                     cuda_why);
    }
    if (!custom && !cpu_ok && mode != NANUM_ACCEL_CUDA)
        reason = talloc_asprintf(tmp, "The CPU AI engine %s is missing%s%s. Reinstall "
                                 "NanumPlayer.%s%s", CPU_ENGINE_FILE,
                                 s->cpu_path ? ": " : "", s->cpu_path ? s->cpu_path : "",
                                 mode == NANUM_ACCEL_AUTO && !cuda_ok ? " " : "",
                                 mode == NANUM_ACCEL_AUTO && !cuda_ok ? auto_why : "");

    mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_string(&root, "mode", MODE_NAME[mode]);
    node_map_add_string(&root, "backend", backend);
    node_map_add_flag(&root, "cpu_available", cpu_ok);
    node_map_add_flag(&root, "cuda_available", cuda_ok);
    node_map_add_flag(&root, "toolkit", s->tk_found);
    node_map_add_string(&root, "toolkit_directory", s->tk_found ? s->tk_dir : "");
    node_map_add_string(&root, "toolkit_version", s->tk_found ? s->tk_ver : "");
    node_map_add_string(&root, "directory", s->cachedir ? s->cachedir : "");
    node_map_add_flag(&root, "busy", running);
    node_map_add_string(&root, "status", STATE_NAME[st]);
    node_map_add_string(&root, "current", cur >= 0 && cur < C_COUNT ? KNOWN[cur].id : "");
    node_map_add_int64(&root, "received", recv);
    node_map_add_int64(&root, "total", total);
    node_map_add_string(&root, "error", err);
    node_map_add_string(&root, "reason", reason);
    node_map_add_int64(&root, "download_size", s->need);
    node_map_add_flag(&root, "catalog", s->cat.ok);
    node_map_add_string(&root, "catalog_error", s->cat.ok ? "" : s->cat.err);
    node_map_add_string(&root, "active_backend", last_backend);

    node_map_add_flag(&root, "update_required", s->update_required);
    mpv_node *arr = node_map_add(&root, "components", MPV_FORMAT_NODE_ARRAY);
    for (int i = 0; i < C_COUNT; i++) {
        mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
        node_map_add_string(m, "id", KNOWN[i].id);
        node_map_add_string(m, "label", s->cat.item[i].valid ? s->cat.item[i].label
                                                             : KNOWN[i].label);
        node_map_add_string(m, "filename", comp_file(&s->cat, i));
        node_map_add_int64(m, "size", s->cat.item[i].valid ? s->cat.item[i].size : 0);
        node_map_add_flag(m, "installed", s->c[i].installed);
        node_map_add_string(m, "path", s->c[i].path);
        node_map_add_string(m, "source", s->c[i].toolkit ? "toolkit"
                                         : s->reused[i] ? "reused" : "user");
    }

    mp_property_do("user-data/nanum/accelerator", M_PROPERTY_SET_NODE, &root, mpctx);
    talloc_free(root.u.list);
    talloc_free(tmp);
}

void nanum_accel_startup(struct MPContext *mpctx, const char *root)
{
    g.mpctx = mpctx;
    g.log = mpctx->log;
    g.cur = -1;
    nanum_dl_init(&g.dl, &g, accel_phase, accel_progress);
    ensure_init(root);
    if (!g.cat.ok)
        mp_warn(g.log, "CUDA acceleration is unavailable: %s\n", g.cat.err);
    g.init = true;
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

static void reload_catalog_if_needed(void)
{
    mp_mutex_lock(&g_lock);
    if (!g.cat.ok)
        read_catalog(&g.cat);
    mp_mutex_unlock(&g_lock);
}

// Core thread. `j` is consumed. Returns false if the worker cannot start.
static bool start_worker(struct MPContext *mpctx, struct job *j, int first, int64_t first_size)
{
    if (!nanum_dl_begin(&g.dl)) {
        talloc_free(j);
        return reject(mpctx, "Out of memory.");
    }
    mp_mutex_lock(&g_lock);
    g.running = true;
    g.running_kind = j->kind;
    g.st = ST_CHECKING;
    g.cur = first;
    g.recv = 0;
    g.total = first_size;
    g.err[0] = 0;
    g.last_pub = GetTickCount64();
    mp_mutex_unlock(&g_lock);

    if (mp_thread_create(&g.thread, worker_main, j)) {
        mp_mutex_lock(&g_lock);
        g.running = false;
        mp_mutex_unlock(&g_lock);
        nanum_dl_end(&g.dl);
        for (int i = 0; i < j->n; i++)
            talloc_free(j->c[i].path);
        talloc_free(j);
        return reject(mpctx, "Cannot start the accelerator thread.");
    }
    g.thread_valid = true;
    publish(mpctx);
    return true;
}

// Core thread: joins a finished worker. false if the worker still runs or the
// module shuts down.
static bool worker_idle(void)
{
    mp_mutex_lock(&g_lock);
    bool down = g.shutting_down, busy = g.running;
    mp_mutex_unlock(&g_lock);
    if (down || busy)
        return false;
    if (g.thread_valid) {
        mp_thread_join(g.thread);   // finished: running is false
        g.thread_valid = false;
    }
    return true;
}

void nanum_accel_refresh(struct MPContext *mpctx)
{
    if (!g.init) {
        MP_ERR(mpctx, "The accelerator manager is not initialized.\n");
        return;
    }
    reload_catalog_if_needed();

    if (worker_idle()) {
        // Files are there but never checked: do it off the core thread.
        void *tmp = talloc_new(NULL);
        struct scan *s = talloc_zero(tmp, struct scan);
        scan_all(tmp, s);
        char *why;
        enum cuda_state cs = cuda_state_of(s, &why);
        bool reuse = s->work_wanted;
        talloc_free(tmp);
        if (reuse || cs == CUDA_UNPROBED) {
            struct job *j = talloc_zero(NULL, struct job);
            j->kind = reuse ? JOB_REUSE : JOB_PROBE;
            start_worker(mpctx, j, -1, 0);
            return;     // published by start_worker
        }
    }
    publish(mpctx);
}

bool nanum_accel_download(struct MPContext *mpctx)
{
    if (!g.init)
        return reject(mpctx, "The accelerator manager is not initialized.");

    mp_mutex_lock(&g_lock);
    bool down = g.shutting_down, busy = g.running;
    mp_mutex_unlock(&g_lock);
    if (down)
        return false;
    if (busy) {
        MP_WARN(mpctx, "An accelerator download or check is already running; "
                "cancel it first.\n");
        return false;
    }
    worker_idle();

    reload_catalog_if_needed();
    void *tmp = talloc_new(NULL);
    struct scan *s = talloc_zero(tmp, struct scan);
    scan_all(tmp, s);
    if (!s->cat.ok) {
        bool r = reject(mpctx, "CUDA acceleration cannot be installed: %s.", s->cat.err);
        talloc_free(tmp);
        return r;
    }
    if (!s->cachedir) {
        talloc_free(tmp);
        return reject(mpctx, "The CUDA component folder cannot be determined "
                      "(%%LOCALAPPDATA%% is unavailable).");
    }

    struct job *j = talloc_zero(NULL, struct job);
    j->kind = JOB_DOWNLOAD;
    for (int i = 0; i < C_COUNT; i++) {
        if (s->c[i].installed)
            continue;       // a valid cached file or a compatible Toolkit
        struct jcomp *c = &j->c[j->n++];
        c->idx = i;
        c->path = talloc_strdup(NULL, s->c[i].path);
        c->e = s->cat.item[i];
    }
    talloc_free(tmp);

    if (!j->n) {
        // Everything is in place: nothing to fetch, but the result of the
        // check is what the caller wants to see.
        talloc_free(j);
        set_state(ST_DONE, -1, 0, 0, NULL);
        publish(mpctx);
        nanum_accel_refresh(mpctx);
        return true;
    }
    return start_worker(mpctx, j, j->c[0].idx, j->c[0].e.size);
}

void nanum_accel_cancel(struct MPContext *mpctx)
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

void nanum_accel_shutdown(struct MPContext *mpctx)
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
