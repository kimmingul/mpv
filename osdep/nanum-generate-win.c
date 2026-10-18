/*
 * NanumPlayer: playback-free generation of subtitle files (SRT). See nanum-generate.h.
 *
 * The work itself (reading the source, speech recognition / OCR, translation) is
 * nanum_generate() in nanum/nanum.c. This file is the command around it: it parses the request,
 * decides where files go, writes them atomically as SRT, and publishes the progress.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include "config.h"

#include "mpv/client.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "player/command.h"
#include "osdep/nanum-generate.h"

#ifdef _WIN32

#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/global.h"
#include "common/msg.h"
#include "misc/dispatch.h"
#include "misc/json.h"
#include "misc/node.h"
#include "misc/thread_tools.h"
#include "options/m_property.h"
#include "player/core.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "nanum/i18n.h"
#include "nanum/nanum.h"

// Progress is published at most this often.
#define PROGRESS_MS 250
#define MAX_TARGETS 24
// Lines longer than this are broken in two at a space (SRT is read on a TV, too).
#define WRAP_CHARS 42

// ---------------------------------------------------------------------------
// Small helpers

static void copy_str(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src ? src : "");
}

static wchar_t *W(void *ctx, const char *s)
{
    return mp_from_utf8(ctx, s);
}

static bool path_attr(void *ctx, const char *path, DWORD *attr)
{
    DWORD a = GetFileAttributesW(W(ctx, path));
    if (a == INVALID_FILE_ATTRIBUTES)
        return false;
    if (attr)
        *attr = a;
    return true;
}

static char *join_path(void *ctx, const char *a, const char *b)
{
    size_t n = strlen(a);
    bool sep = n && (a[n - 1] == '\\' || a[n - 1] == '/');
    return talloc_asprintf(ctx, "%s%s%s", a, sep ? "" : "\\", b);
}

static const char *file_part(const char *p)
{
    const char *b = p;
    for (; *p; p++) {
        if (*p == '\\' || *p == '/')
            b = p + 1;
    }
    return b;
}

// The file behind a url, if it is one (never a disc or network url).
static char *local_file(void *ctx, const char *url)
{
    if (!strncmp(url, "file://", 7)) {
        const char *p = url + 7;
        if (p[0] == '/' && p[1] && p[2] == ':')
            p++;                    // file:///C:/x
        return talloc_strdup(ctx, p);
    }
    if (!url[0] || strstr(url, "://"))
        return NULL;
    return talloc_strdup(ctx, url);
}

// A name that is safe as part of a Windows file name.
static char *sanitize_stem(void *ctx, const char *in)
{
    char *s = talloc_strdup(ctx, in ? in : "");
    for (char *p = s; *p; p++) {
        if ((unsigned char)*p < 0x20 || strchr("<>:\"/\\|?*", *p))
            *p = '_';
    }
    size_t l = strlen(s);
    if (l > 120) {
        l = 120;
        while (l && ((unsigned char)s[l] & 0xc0) == 0x80)   // not inside a UTF-8 sequence
            l--;
        s[l] = '\0';
    }
    while (l && (s[l - 1] == ' ' || s[l - 1] == '.'))
        s[--l] = '\0';
    while (*s == ' ')
        s++;
    return s[0] ? s : talloc_strdup(ctx, "subtitles");
}

// The part of the file name that says which language it is in: "ko", "zh-TW", "und". A code is
// used as it is; a language name only has its letters and digits, in lower case.
static char *lang_suffix(void *ctx, const char *code, const char *name)
{
    bool from_code = code && code[0];
    char *s = talloc_strdup(ctx, from_code ? code : name ? name : "");
    size_t w = 0;
    for (size_t i = 0; s[i]; i++) {
        if (isalnum((unsigned char)s[i]) || s[i] == '-')
            s[w++] = from_code ? s[i] : (char)tolower((unsigned char)s[i]);
    }
    s[w] = '\0';
    return s[0] ? s : talloc_strdup(ctx, "und");
}

// ---------------------------------------------------------------------------
// SRT

struct sb {
    void *ctx;
    char *p;
    size_t len, cap;
};

static void sb_add(struct sb *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2 + 4096;
        b->p = talloc_realloc(b->ctx, b->p, char, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void fmt_time(char *buf, double t)
{
    int64_t ms = (int64_t)(fmax(t, 0) * 1000.0 + 0.5);
    snprintf(buf, 16, "%02d:%02d:%02d,%03d", (int)(ms / 3600000), (int)(ms / 60000 % 60),
             (int)(ms / 1000 % 60), (int)(ms % 1000));
}

// One cue's text as SRT wants it: no empty lines (they end the cue), one line broken in two
// when it is long and has a space near the middle.
static char *srt_text(void *ctx, const char *text)
{
    char *s = talloc_strdup(ctx, text);
    size_t w = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\r')
            continue;
        if (c == '\n' && (w == 0 || s[w - 1] == '\n'))
            continue;
        s[w++] = c;
    }
    while (w && (s[w - 1] == '\n' || s[w - 1] == ' '))
        w--;
    s[w] = '\0';
    if (strchr(s, '\n'))
        return s;
    size_t chars = 0;
    for (size_t i = 0; i < w; i++)
        chars += ((unsigned char)s[i] & 0xc0) != 0x80;
    if (chars <= WRAP_CHARS)
        return s;
    // The space nearest to the middle (in characters).
    size_t best = 0, seen = 0;
    double best_d = 1e9;
    for (size_t i = 0; i < w; i++) {
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            seen++;
        if (s[i] == ' ' && fabs((double)seen - chars / 2.0) < best_d) {
            best_d = fabs((double)seen - chars / 2.0);
            best = i;
        }
    }
    if (best && best_d < chars / 4.0)
        s[best] = '\n';
    return s;
}

static char *srt_build(void *ctx, const struct nanum_gen_cue *cues, size_t n, size_t *lines)
{
    struct sb b = {.ctx = ctx};
    sb_add(&b, "", 0);
    size_t k = 0;
    void *tmp = talloc_new(NULL);
    for (size_t i = 0; i < n; i++) {
        char *text = srt_text(tmp, cues[i].text);
        if (!text[0]) {
            talloc_free(text);
            continue;
        }
        char t0[16], t1[16], head[64];
        fmt_time(t0, cues[i].start);
        fmt_time(t1, fmax(cues[i].end, cues[i].start + 0.1));
        int hl = snprintf(head, sizeof(head), "%zu\r\n%s --> %s\r\n", ++k, t0, t1);
        sb_add(&b, head, (size_t)hl);
        // CRLF line breaks: what the SRT specification and every Windows tool expects.
        for (const char *p = text; *p; p++) {
            if (*p == '\n')
                sb_add(&b, "\r\n", 2);
            else
                sb_add(&b, p, 1);
        }
        sb_add(&b, "\r\n\r\n", 4);
        talloc_free(text);
    }
    talloc_free(tmp);
    *lines = k;
    return b.p;
}

// ---------------------------------------------------------------------------
// Files

// Creates the folder (and the ones above it) if it is missing. false if it cannot be had.
static bool ensure_dir(void *ctx, const char *dir, char **out, char *err, size_t errsz)
{
    char *d = talloc_strdup(ctx, dir);
    for (char *s = d; *s; s++) {
        if (*s == '/')
            *s = '\\';
    }
    size_t l = strlen(d);
    while (l > 3 && d[l - 1] == '\\')
        d[--l] = '\0';
    DWORD a;
    if (!path_attr(ctx, d, &a)) {
        for (char *s = d + (l > 2 && d[1] == ':' ? 3 : 1); *s; s++) {
            if (*s != '\\')
                continue;
            *s = '\0';
            if (!path_attr(ctx, d, NULL))
                CreateDirectoryW(W(ctx, d), NULL);
            *s = '\\';
        }
        if (!CreateDirectoryW(W(ctx, d), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            snprintf(err, errsz, nanum_tr("The folder %s cannot be created."), d);
            return false;
        }
        if (!path_attr(ctx, d, &a))
            return false;
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        snprintf(err, errsz, nanum_tr("%s is not a folder."), d);
        return false;
    }
    *out = d;
    return true;
}

// A folder that exists can still refuse files: find out before a long job starts, not when its
// first file is written. The probe file deletes itself when it is closed.
static bool dir_writable(void *ctx, const char *dir, char *err, size_t errsz)
{
    char *probe = talloc_asprintf(ctx, "%s\\.nanum-write-test-%lu.tmp", dir,
                                  (unsigned long)GetCurrentProcessId());
    HANDLE h = CreateFileW(W(ctx, probe), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, errsz, nanum_tr("%s cannot be written (error %lu)."), dir,
                 (unsigned long)GetLastError());
        return false;
    }
    CloseHandle(h);
    return true;
}

// Writes content as <dir>\<stem>.<suffix>.srt, or .2.srt, .3.srt, ... if that is taken: an
// existing file is never touched. The data goes to a ".part" file that is renamed when it is
// complete. Returns the final path (talloc child of ctx) or NULL (err).
static char *write_new_file(void *ctx, const char *dir, const char *stem, const char *suffix,
                            const char *data, size_t len, char *err, size_t errsz)
{
    void *tmp = talloc_new(NULL);
    char *result = NULL;
    for (int n = 1; n < 1000 && !result; n++) {
        char *fin = n == 1 ? talloc_asprintf(tmp, "%s.%s.srt", stem, suffix)
                           : talloc_asprintf(tmp, "%s.%s.%d.srt", stem, suffix, n);
        char *path = join_path(tmp, dir, fin);
        char *part = talloc_asprintf(tmp, "%s.part", path);
        if (path_attr(tmp, path, NULL) || path_attr(tmp, part, NULL))
            continue;
        HANDLE h = CreateFileW(W(tmp, part), GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS)
                continue;
            snprintf(err, errsz, nanum_tr("%s cannot be written (error %lu)."), part,
                     (unsigned long)e);
            break;
        }
        bool ok = true;
        const char *p = data;
        size_t left = len;
        DWORD werr = 0;
        while (left && ok) {
            DWORD chunk = (DWORD)MPMIN(left, (size_t)1 << 20), got = 0;
            ok = WriteFile(h, p, chunk, &got, NULL) && got == chunk;
            if (!ok)
                werr = GetLastError();
            p += chunk;
            left -= chunk;
        }
        if (ok && !FlushFileBuffers(h)) {
            ok = false;
            werr = GetLastError();
        }
        CloseHandle(h);
        if (!ok) {
            DeleteFileW(W(tmp, part));
            snprintf(err, errsz, nanum_tr("%s cannot be written (error %lu)."), part,
                     (unsigned long)werr);
            break;
        }
        // Without MOVEFILE_REPLACE_EXISTING: if the name was taken meanwhile, this fails and
        // the next number is tried.
        if (MoveFileExW(W(tmp, part), W(tmp, path), MOVEFILE_WRITE_THROUGH)) {
            result = talloc_strdup(ctx, path);
            break;
        }
        DWORD e = GetLastError();
        DeleteFileW(W(tmp, part));
        if (e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) {
            snprintf(err, errsz, nanum_tr("%s cannot be written (error %lu)."), path,
                     (unsigned long)e);
            break;
        }
    }
    if (!result && !err[0])
        snprintf(err, errsz, nanum_tr("No free file name for %s in %s."), stem, dir);
    talloc_free(tmp);
    return result;
}

// ---------------------------------------------------------------------------
// Request

struct spec {
    const char *url, *directory, *name, *title, *glossary, *source_lang;
    int source;
    int track;
    bool write_source;
    double limit;
    struct nanum_gen_target targets[MAX_TARGETS];
    int num_targets;
};

static const char *node_string(struct mpv_node *map, const char *key)
{
    struct mpv_node *v = node_map_get(map, key);
    return v && v->format == MPV_FORMAT_STRING ? v->u.string : "";
}

// false: the request is not usable (err: why).
static bool parse_spec(void *ctx, const char *text, struct spec *s, char *err, size_t errsz)
{
    *s = (struct spec){.track = -1, .write_source = true};
    char *json = talloc_strdup(ctx, text ? text : "");
    char *p = json;
    struct mpv_node root;
    if (json_parse(ctx, &root, &p, MAX_JSON_DEPTH) < 0 || root.format != MPV_FORMAT_NODE_MAP) {
        snprintf(err, errsz, "%s", nanum_tr("The request is not a valid JSON object."));
        return false;
    }
    s->url = node_string(&root, "url");
    s->directory = node_string(&root, "directory");
    s->name = node_string(&root, "name");
    s->title = node_string(&root, "title");
    s->glossary = node_string(&root, "glossary");
    s->source_lang = node_string(&root, "source_lang");
    const char *src = node_string(&root, "source");
    s->source = !strcmp(src, "stt") ? NANUM_GEN_STT : !strcmp(src, "text") ? NANUM_GEN_TEXT
                : !strcmp(src, "ocr") ? NANUM_GEN_OCR : -1;
    struct mpv_node *v = node_map_get(&root, "track");
    if (v && v->format == MPV_FORMAT_INT64)
        s->track = (int)v->u.int64;
    else if (v && v->format == MPV_FORMAT_DOUBLE)
        s->track = (int)v->u.double_;
    v = node_map_get(&root, "write_source");
    if (v && v->format == MPV_FORMAT_FLAG)
        s->write_source = v->u.flag;
    v = node_map_get(&root, "limit");
    if (v && v->format == MPV_FORMAT_INT64)
        s->limit = (double)v->u.int64;
    else if (v && v->format == MPV_FORMAT_DOUBLE)
        s->limit = v->u.double_;
    v = node_map_get(&root, "targets");
    if (v && v->format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < v->u.list->num; i++) {
            struct mpv_node *t = &v->u.list->values[i];
            if (t->format != MPV_FORMAT_NODE_MAP)
                continue;
            const char *code = node_string(t, "code"), *name = node_string(t, "name");
            if (!code[0] && !name[0])
                continue;
            bool dup = false;
            for (int k = 0; k < s->num_targets; k++) {
                dup |= code[0] ? !strcasecmp(s->targets[k].code, code)
                               : !strcasecmp(s->targets[k].name, name);
            }
            if (dup)
                continue;
            if (s->num_targets >= MAX_TARGETS) {
                snprintf(err, errsz, "%s", nanum_tr("Too many target languages."));
                return false;
            }
            s->targets[s->num_targets++] = (struct nanum_gen_target){.code = code, .name = name};
        }
    }
    if (!s->url[0] || s->source < 0) {
        snprintf(err, errsz, "%s", nanum_tr("url and source (stt, text or ocr) are required."));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The running job and its published state

struct out_file {
    char role[16];              // "source" | "translation"
    char language[24];
    char name[64];
    char file[1024];
    int64_t size;
    size_t lines;
};

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;

static struct {
    struct MPContext *mpctx;
    bool busy;
    const char *status;
    const char *source;
    char url[1024], directory[1024], name[256];
    double progress, phase_progress, position, duration;
    size_t lines;
    int target, num_targets;
    char target_name[64];
    ULONGLONG t0;
    double elapsed_final;           // < 0 while running
    char error[640], error_code[24], warning[640];
    struct out_file outs[MAX_TARGETS + 1];
    int num_outs;
    ULONGLONG last_pub;
} g = {.status = "idle", .source = "", .elapsed_final = -1};

static void add_outputs(mpv_node *parent, const char *key, const struct out_file *outs, int n)
{
    mpv_node *arr = node_map_add(parent, key, MPV_FORMAT_NODE_ARRAY);
    for (int i = 0; i < n; i++) {
        mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
        node_map_add_string(m, "role", outs[i].role);
        node_map_add_string(m, "language", outs[i].language);
        node_map_add_string(m, "name", outs[i].name);
        node_map_add_string(m, "file", outs[i].file);
        node_map_add_int64(m, "size", outs[i].size);
        node_map_add_int64(m, "lines", (int64_t)outs[i].lines);
    }
}

static void publish(struct MPContext *mpctx)
{
    if (!mpctx)
        return;
    mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    mp_mutex_lock(&g_lock);
    node_map_add_flag(&root, "busy", g.busy);
    node_map_add_string(&root, "status", g.status);
    node_map_add_string(&root, "source", g.source);
    node_map_add_string(&root, "url", g.url);
    node_map_add_string(&root, "directory", g.directory);
    node_map_add_string(&root, "name", g.name);
    node_map_add_double(&root, "progress", g.progress);
    node_map_add_double(&root, "phase_progress", g.phase_progress);
    node_map_add_double(&root, "position", g.position);
    node_map_add_double(&root, "duration", g.duration);
    node_map_add_int64(&root, "lines", (int64_t)g.lines);
    node_map_add_int64(&root, "target", g.target);
    node_map_add_int64(&root, "num_targets", g.num_targets);
    node_map_add_string(&root, "target_name", g.target_name);
    double el = g.elapsed_final >= 0 ? g.elapsed_final
                                     : g.t0 ? (GetTickCount64() - g.t0) / 1000.0 : 0;
    node_map_add_double(&root, "elapsed", el);
    node_map_add_string(&root, "error", g.error);
    node_map_add_string(&root, "error_code", g.error_code);
    node_map_add_string(&root, "warning", g.warning);
    add_outputs(&root, "outputs", g.outs, g.num_outs);
    mp_mutex_unlock(&g_lock);

    mp_property_do("user-data/nanum/generate", M_PROPERTY_SET_NODE, &root, mpctx);
    talloc_free(root.u.list);
}

static void publish_cb(void *p)
{
    mp_mutex_lock(&g_lock);
    struct MPContext *mpctx = g.mpctx;
    mp_mutex_unlock(&g_lock);
    publish(mpctx);
}

// Any thread. Merges with an already queued publication.
static void request_publish(void)
{
    mp_mutex_lock(&g_lock);
    struct MPContext *mpctx = g.mpctx;
    g.last_pub = GetTickCount64();
    mp_mutex_unlock(&g_lock);
    if (mpctx)
        mp_dispatch_enqueue_notify(mpctx->dispatch, publish_cb, &g);
}

static void request_publish_throttled(void)
{
    mp_mutex_lock(&g_lock);
    bool due = GetTickCount64() - g.last_pub >= PROGRESS_MS;
    mp_mutex_unlock(&g_lock);
    if (due)
        request_publish();
}

// Claims the one job slot. Called on the core thread.
static bool begin_job(struct MPContext *mpctx, const struct spec *s)
{
    mp_mutex_lock(&g_lock);
    if (g.busy) {
        mp_mutex_unlock(&g_lock);
        return false;
    }
    g.mpctx = mpctx;
    g.busy = true;
    g.status = "opening";
    g.source = s->source == NANUM_GEN_STT ? "stt" : s->source == NANUM_GEN_TEXT ? "text" : "ocr";
    copy_str(g.url, sizeof(g.url), s->url);
    g.directory[0] = g.name[0] = g.target_name[0] = '\0';
    g.progress = g.phase_progress = g.position = g.duration = 0;
    g.lines = 0;
    g.target = 0;
    g.num_targets = s->num_targets;
    g.t0 = GetTickCount64();
    g.elapsed_final = -1;
    g.error[0] = g.error_code[0] = g.warning[0] = '\0';
    g.num_outs = 0;
    g.last_pub = 0;
    mp_mutex_unlock(&g_lock);
    return true;
}

// Core thread, core locked: publishes the final state and releases the slot.
static void end_job(struct MPContext *mpctx, const char *status, const char *code,
                    const char *error, const char *warning)
{
    mp_mutex_lock(&g_lock);
    g.status = status;
    g.busy = false;
    g.elapsed_final = (GetTickCount64() - g.t0) / 1000.0;
    copy_str(g.error, sizeof(g.error), error);
    copy_str(g.error_code, sizeof(g.error_code), code);
    copy_str(g.warning, sizeof(g.warning), warning);
    if (!strcmp(status, "done"))
        g.progress = g.phase_progress = 1;
    mp_mutex_unlock(&g_lock);
    // The final state is published right here; a queued publication would only be stale (and
    // the dispatch queue must be empty when the core is destroyed).
    mp_dispatch_cancel_fn(mpctx->dispatch, publish_cb, &g);
    publish(mpctx);
}

struct job {
    struct nanum_gen_request rq;
    const struct spec *spec;
    char *dir, *stem;
    char tstate[MAX_TARGETS][16];
    char terr[MAX_TARGETS][256];
    struct out_file outs[MAX_TARGETS + 1];
    int num_outs;
};

// Share of the whole job each part is expected to take: reading the source (speech recognition
// and OCR take long, a text track is read in seconds) and, evenly split, the translations.
static double scan_weight(const struct job *j)
{
    if (j->spec->num_targets == 0)
        return 1.0;
    return j->spec->source == NANUM_GEN_TEXT ? 0.1 : 0.5;
}

static void job_progress(void *user, const struct nanum_gen_progress *p)
{
    struct job *j = user;
    double w = scan_weight(j), overall;
    const char *status;
    switch (p->phase) {
    case NANUM_GEN_OPEN:
        status = "opening";
        overall = 0;
        break;
    case NANUM_GEN_SCAN:
        status = "scanning";
        overall = w * p->fraction;
        break;
    case NANUM_GEN_GLOSSARY:
        status = "glossary";
        overall = w + (1 - w) * p->target / MPMAX(j->spec->num_targets, 1);
        break;
    default:
        status = "translating";
        overall = w + (1 - w) * (p->target + p->fraction) / MPMAX(j->spec->num_targets, 1);
    }
    mp_mutex_lock(&g_lock);
    g.status = status;
    g.progress = MPCLAMP(overall, 0, 1);
    g.phase_progress = p->fraction;
    g.position = p->position;
    g.duration = p->duration;
    if (p->phase == NANUM_GEN_SCAN)
        g.lines = p->lines;
    if (p->phase >= NANUM_GEN_GLOSSARY) {
        g.target = p->target;
        const struct nanum_gen_target *t = &j->spec->targets[p->target];
        copy_str(g.target_name, sizeof(g.target_name), t->name[0] ? t->name : t->code);
    }
    mp_mutex_unlock(&g_lock);
    request_publish_throttled();
}

static void job_target_done(void *user, int target, const char *state, const char *msg)
{
    struct job *j = user;
    if (target < 0 || target >= MAX_TARGETS)
        return;
    copy_str(j->tstate[target], sizeof(j->tstate[target]), state);
    copy_str(j->terr[target], sizeof(j->terr[target]), msg);
    request_publish();
}

static bool job_emit(void *user, int role, int target, const char *code, const char *name,
                     const struct nanum_gen_cue *cues, size_t num, char *err, size_t errsz)
{
    struct job *j = user;
    if (role == NANUM_GEN_ROLE_SOURCE && !j->spec->write_source)
        return true;
    void *tmp = talloc_new(NULL);
    size_t lines;
    char *data = srt_build(tmp, cues, num, &lines);
    char *suffix = lang_suffix(tmp, code, name);
    char *path = write_new_file(tmp, j->dir, j->stem, suffix, data, strlen(data), err, errsz);
    bool ok = path != NULL;
    if (ok && j->num_outs < (int)MP_ARRAY_SIZE(j->outs)) {
        struct out_file *o = &j->outs[j->num_outs++];
        copy_str(o->role, sizeof(o->role), role == NANUM_GEN_ROLE_SOURCE ? "source" : "translation");
        copy_str(o->language, sizeof(o->language), code && code[0] ? code : suffix);
        copy_str(o->name, sizeof(o->name), name);
        copy_str(o->file, sizeof(o->file), path);
        o->size = (int64_t)strlen(data);
        o->lines = lines;
        mp_mutex_lock(&g_lock);
        g.outs[g.num_outs++] = *o;
        mp_mutex_unlock(&g_lock);
        if (target >= 0 && target < MAX_TARGETS)
            copy_str(j->tstate[target], sizeof(j->tstate[target]), "done");
        request_publish();
    }
    talloc_free(tmp);
    return ok;
}

static void result_error(struct mp_cmd_ctx *cmd, const char *status, const char *code,
                         const char *msg)
{
    mpv_node *res = &cmd->result;
    node_init(res, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_flag(res, "ok", false);
    node_map_add_string(res, "status", status);
    node_map_add_string(res, "error", msg);
    node_map_add_string(res, "error_code", code);
}

void nanum_generate_command(struct mp_cmd_ctx *cmd, const char *spec_text)
{
    struct MPContext *mpctx = cmd->mpctx;
    void *tmp = talloc_new(NULL);
    struct spec *s = talloc_zero(tmp, struct spec);
    char perr[320] = "";
    if (!parse_spec(tmp, spec_text, s, perr, sizeof(perr))) {
        result_error(cmd, "error", "bad-args", perr);
        talloc_free(tmp);
        return;
    }
    if (!begin_job(mpctx, s)) {
        result_error(cmd, "error", "busy", nanum_tr("Another subtitle generation is still running."));
        talloc_free(tmp);
        return;
    }
    request_publish();

    struct job *j = talloc_zero(tmp, struct job);
    j->spec = s;
    struct nanum_gen_result res;
    char derr[400] = "";
    char *local = local_file(tmp, s->url);

    // Where the files go and what they are called.
    const char *want_dir = s->directory;
    if (!want_dir[0] && local) {
        char *d = talloc_strdup(tmp, local);
        char *b = (char *)file_part(d);
        if (b > d)
            b[-1] = '\0';
        else
            d[0] = '\0';
        want_dir = d;
    }
    char *stem_src = s->name[0] ? talloc_strdup(tmp, s->name) : NULL;
    if (!stem_src && local) {
        stem_src = talloc_strdup(tmp, file_part(local));
        char *dot = strrchr(stem_src, '.');
        if (dot && dot != stem_src)
            *dot = '\0';
    }
    j->stem = sanitize_stem(tmp, stem_src);
    bool placed = want_dir[0] && ensure_dir(tmp, want_dir, &j->dir, derr, sizeof(derr)) &&
                  dir_writable(tmp, j->dir, derr, sizeof(derr));
    if (placed) {
        mp_mutex_lock(&g_lock);
        copy_str(g.directory, sizeof(g.directory), j->dir);
        copy_str(g.name, sizeof(g.name), j->stem);
        mp_mutex_unlock(&g_lock);
        request_publish();
    }
    for (int i = 0; i < s->num_targets; i++)
        copy_str(j->tstate[i], sizeof(j->tstate[i]), "not-started");

    mp_core_unlock(mpctx);

    if (placed) {
        j->rq = (struct nanum_gen_request){
            .global = mpctx->global,
            .log = mp_log_new(tmp, mpctx->log, "nanum-generate"),
            .cancel = cmd->abort->cancel,
            .url = s->url,
            .source = s->source,
            .track = s->track,
            .source_lang = s->source_lang,
            .targets = s->targets,
            .num_targets = s->num_targets,
            .title = s->title,
            .glossary = s->glossary,
            .limit = s->limit,
            .user = j,
            .progress = job_progress,
            .emit = job_emit,
            .target_done = job_target_done,
        };
        nanum_generate(&j->rq, &res);
    } else {
        res = (struct nanum_gen_result){0};
        snprintf(res.error_code, sizeof(res.error_code), "bad-destination");
        snprintf(res.error, sizeof(res.error), "%s",
                 derr[0] ? derr : nanum_tr("Choose a folder for the subtitle files."));
    }

    mp_core_lock(mpctx);

    // A source that got no translation for a reason other than failing is not an error: the
    // source file is the result. Failed targets make the job an error (the files stay).
    const char *status = res.ok ? "done" : res.cancelled ? "cancelled" : "error";
    end_job(mpctx, status, res.error_code, res.error, res.warning);

    mpv_node *out = &cmd->result;
    node_init(out, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_flag(out, "ok", res.ok);
    node_map_add_string(out, "status", status);
    node_map_add_string(out, "error", res.error);
    node_map_add_string(out, "error_code", res.error_code);
    node_map_add_string(out, "warning", res.warning);
    node_map_add_string(out, "url", s->url);
    node_map_add_string(out, "directory", j->dir ? j->dir : "");
    node_map_add_string(out, "name", j->stem);
    node_map_add_string(out, "source", g.source);
    node_map_add_string(out, "source_lang", res.source_lang);
    node_map_add_int64(out, "source_lines", (int64_t)res.source_lines);
    node_map_add_double(out, "duration", res.duration);
    node_map_add_int64(out, "targets_done", res.targets_done);
    node_map_add_int64(out, "targets_failed", res.targets_failed);
    add_outputs(out, "outputs", j->outs, j->num_outs);
    mpv_node *arr = node_map_add(out, "targets", MPV_FORMAT_NODE_ARRAY);
    for (int i = 0; i < s->num_targets; i++) {
        mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
        node_map_add_int64(m, "index", i);
        node_map_add_string(m, "code", s->targets[i].code);
        node_map_add_string(m, "name", s->targets[i].name);
        node_map_add_string(m, "state", j->tstate[i]);
        node_map_add_string(m, "error", j->terr[i]);
    }
    talloc_free(tmp);
}

#else // !_WIN32

void nanum_generate_command(struct mp_cmd_ctx *cmd, const char *spec)
{
}

#endif
