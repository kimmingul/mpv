/*
 * NanumPlayer: libbluray file access with Win32 error capture (Windows).
 * See bluray_file_win32.h.
 *
 * This file is part of mpv, distributed under the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 */

#include <windows.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libbluray/filesystem.h>

#include "common/common.h"
#include "common/msg.h"
#include "osdep/nanum-diag.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "stream/bluray_file_win32.h"

// Application log: at most this many failure/slow records per session.
#define MAX_NORMAL_RECORDS   200
// Diagnostic log: the first reads after an open/seek are recorded in full,
// the rest as a statistic of this period.
#define TRACE_READS          3
#define STATS_PERIOD_NS      (5 * INT64_C(1000000000))

enum kind {
    KIND_META,      // small database files (index.bdmv, .mpls, .clpi, ...)
    KIND_STREAM,    // .m2ts / .ssif
    KIND_IMAGE,     // disc image read through libbluray's UDF reader
};

struct bd_io {
    struct bd_io *next;
    int refs;                   // the stream's one + one per open file
    bool ended;
    struct mp_log *log;         // NULL once ended
    unsigned id;

    char cur_file[24];
    int64_t cur_pos, cur_size;

    uint64_t reads, bytes, failures, slow, logged, not_shown;
    uint64_t seek_id;           // tag of the reads after the latest seek request

    int64_t win_start_ns;
    uint64_t win_reads, win_bytes;
    int64_t win_total_ns, win_max_ns;

    uint64_t failure_serial;
    char failure_file[24];
    int64_t failure_offset, failure_length, failure_got, failure_ms;
    unsigned long failure_os_error;
    double failure_time;
};

struct io_file {
    BD_FILE_H pub;              // first: libbluray frees/passes this pointer
    HANDLE h;
    struct bd_io *sess;         // reference held, may be NULL
    enum kind kind;
    int64_t pos, size;
    bool eof;
    int trace_left;             // reads still recorded in full (diagnostics)
    char name[24];              // base name
};

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;
static struct bd_io *g_sessions;        // newest first
static BD_FILE_OPEN g_prev_open;        // libbluray's default, while hooked
static int g_hook_refs;
static unsigned g_next_id;

static BD_FILE_H *io_open(const char *filename, const char *mode);

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

// level >= 0: also into the application log (g_lock held, which protects
// s->log). The diagnostic log gets every record while diagnostics are on.
static void record(struct bd_io *s, int level, bool important, const char *fmt, ...)
    MP_PRINTF_ATTRIBUTE(4, 5);
static void record(struct bd_io *s, int level, bool important, const char *fmt, ...)
{
    char text[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (level >= 0 && s && s->log)
        mp_msg(s->log, level, "%s\n", text);
    nanum_diag_log(important, "io: %s", text);
}

static bool normal_allowed(struct bd_io *s)
{
    if (!s)
        return false;
    if (s->logged >= MAX_NORMAL_RECORDS) {
        s->not_shown++;
        return false;
    }
    s->logged++;
    return true;
}

static void base_name(const char *path, char *out, size_t size)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            b = p + 1;
    }
    snprintf(out, size, "%s", b);
}

static enum kind classify(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return KIND_META;
    if (!_stricmp(dot, ".m2ts") || !_stricmp(dot, ".ssif"))
        return KIND_STREAM;
    if (!_stricmp(dot, ".iso") || !_stricmp(dot, ".img") || !_stricmp(dot, ".udf"))
        return KIND_IMAGE;
    return KIND_META;
}

static bool not_found(DWORD err)
{
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

static int64_t slow_limit_ns(void)
{
    return (nanum_diag_enabled() ? NANUM_DIAG_SLOW_READ_MS_DIAG
                                 : NANUM_DIAG_SLOW_READ_MS) * INT64_C(1000000);
}

static void os_text(DWORD err, char *out, size_t size)
{
    nanum_diag_os_error_text(err, out, size);
}

// ---------------------------------------------------------------------------
// BD_FILE_H callbacks
// ---------------------------------------------------------------------------

static void io_close(BD_FILE_H *fh)
{
    struct io_file *f = (struct io_file *)fh;
    if (!f)
        return;
    if (f->kind != KIND_META && nanum_diag_enabled()) {
        mp_mutex_lock(&g_lock);
        record(f->sess, -1, false, "close file=%s pos=%" PRId64 " size=%" PRId64,
               f->name, f->pos, f->size);
        mp_mutex_unlock(&g_lock);
    }
    CloseHandle(f->h);

    struct bd_io *s = f->sess;
    if (s) {
        mp_mutex_lock(&g_lock);
        if (f->kind == KIND_STREAM && !strcmp(s->cur_file, f->name))
            s->cur_file[0] = 0;
        bool last = --s->refs == 0;
        mp_mutex_unlock(&g_lock);
        if (last)
            free(s);
    }
    free(f);
}

static int64_t io_seek(BD_FILE_H *fh, int64_t offset, int32_t origin)
{
    struct io_file *f = (struct io_file *)fh;
    DWORD method = origin == SEEK_SET ? FILE_BEGIN :
                   origin == SEEK_CUR ? FILE_CURRENT :
                   origin == SEEK_END ? FILE_END : 0xFFFFFFFF;
    LARGE_INTEGER to = {.QuadPart = offset}, now;
    int64_t t0 = mp_time_ns();
    bool ok = method != 0xFFFFFFFF && SetFilePointerEx(f->h, to, &now, method);
    DWORD err = ok ? 0 : GetLastError();
    int64_t dt = mp_time_ns() - t0;
    int64_t from = f->pos;
    if (ok) {
        f->pos = now.QuadPart;
        f->eof = false;
        f->trace_left = TRACE_READS;
    }

    if (f->kind == KIND_META && ok)
        return 0;

    mp_mutex_lock(&g_lock);
    struct bd_io *s = f->sess;
    if (ok) {
        if (s && f->kind == KIND_STREAM) {
            s->cur_pos = f->pos;
            snprintf(s->cur_file, sizeof(s->cur_file), "%s", f->name);
            s->cur_size = f->size;
        }
        if (nanum_diag_enabled()) {
            record(s, -1, false, "seek file=%s origin=%d offset=%" PRId64 ": %" PRId64
                   " -> %" PRId64 " in %.3f ms (seek request #%" PRIu64 ")", f->name,
                   (int)origin, offset, from, f->pos, dt / 1e6, s ? s->seek_id : 0);
        }
        if (dt >= slow_limit_ns() && normal_allowed(s)) {
            record(s, MSGL_WARN, true, "Slow disc seek: file=%s to offset %" PRId64
                   " took %.0f ms", f->name, f->pos, dt / 1e6);
        }
    } else {
        char text[256];
        os_text(err, text, sizeof(text));
        if (normal_allowed(s)) {
            record(s, MSGL_WARN, true, "Disc file seek failed: file=%s origin=%d "
                   "offset=%" PRId64 " os_error=%lu (%s) [source: SetFilePointerEx]",
                   f->name, (int)origin, offset, (unsigned long)err, text);
        }
    }
    mp_mutex_unlock(&g_lock);
    return ok ? 0 : -1;
}

static int64_t io_tell(BD_FILE_H *fh)
{
    return ((struct io_file *)fh)->pos;
}

static int io_eof(BD_FILE_H *fh)
{
    return ((struct io_file *)fh)->eof ? 1 : 0;
}

static int64_t io_write(BD_FILE_H *fh, const uint8_t *buf, int64_t size)
{
    return -1;      // files are opened for reading only
}

static void read_failed(struct io_file *f, struct bd_io *s, int64_t start,
                        int64_t want, int64_t got, DWORD err, int64_t dt,
                        int64_t pos_after)
{
    s->failures++;
    s->failure_serial++;
    snprintf(s->failure_file, sizeof(s->failure_file), "%s", f->name);
    s->failure_offset = start;
    s->failure_length = want;
    s->failure_got = got;
    s->failure_os_error = err;
    s->failure_time = mp_time_sec();
    s->failure_ms = dt / 1000000;

    char text[256];
    if (err)
        os_text(err, text, sizeof(text));
    if (normal_allowed(s)) {
        if (err) {
            record(s, MSGL_WARN, true,
                   "Disc read failed: file=%s offset=%" PRId64 " length=%" PRId64
                   " got=%" PRId64 " os_error=%lu (%s) after %" PRId64 " ms "
                   "[source: ReadFile in the libbluray file callback; the drive's "
                   "sense data is not available at this level]",
                   f->name, start, want, got, (unsigned long)err, text, dt / 1000000);
        } else {
            record(s, MSGL_WARN, true,
                   "Disc read short without OS error: file=%s offset=%" PRId64
                   " length=%" PRId64 " got=%" PRId64 " size=%" PRId64 " after %"
                   PRId64 " ms [source: ReadFile returned 0 bytes before the end of the file]",
                   f->name, start, want, got, f->size, dt / 1000000);
        }
    } else {
        nanum_diag_log(true, "io: read failed (application log limit): file=%s offset=%"
                       PRId64 " length=%" PRId64 " got=%" PRId64 " os_error=%lu after %"
                       PRId64 " ms", f->name, start, want, got, (unsigned long)err,
                       dt / 1000000);
    }
    nanum_diag_log(true, "io: file pointer after the failure: %" PRId64
                   " (requested start %" PRId64 ")", pos_after, start);
}

static int64_t io_read(BD_FILE_H *fh, uint8_t *buf, int64_t size)
{
    struct io_file *f = (struct io_file *)fh;
    // Same as libbluray's default: invalid sizes are ignored.
    if (size <= 0 || size >= (INT64_MAX >> 1))
        return 0;

    int64_t start = f->pos;
    int64_t t0 = mp_time_ns();
    int64_t total = 0;
    DWORD err = 0;
    while (total < size) {
        DWORD chunk = (DWORD)MPMIN(size - total, (int64_t)0x40000000);
        DWORD got = 0;
        if (!ReadFile(f->h, buf + total, chunk, &got, NULL)) {
            err = GetLastError();      // the failing call's error, nothing in between
            total += got;
            break;
        }
        if (!got)
            break;                     // end of file
        total += got;
    }
    int64_t dt = mp_time_ns() - t0;
    f->pos += total;

    int64_t pos_after = f->pos;
    if (err) {
        LARGE_INTEGER zero = {0}, now;
        if (SetFilePointerEx(f->h, zero, &now, FILE_CURRENT)) {
            f->pos = now.QuadPart;
            pos_after = f->pos;
        }
    } else if (total < size) {
        f->eof = true;
    }

    bool failed = err || (total < size && start + total < f->size);
    bool slow = dt >= slow_limit_ns();
    bool trace = f->kind != KIND_META && nanum_diag_enabled();
    bool wants_stats = trace;
    if (!failed && !slow && !trace && !(f->kind == KIND_STREAM && f->sess))
        return total;

    mp_mutex_lock(&g_lock);
    struct bd_io *s = f->sess;
    if (s && f->kind != KIND_META) {
        s->reads++;
        s->bytes += total;
        if (f->kind == KIND_STREAM) {
            s->cur_pos = f->pos;
            if (strcmp(s->cur_file, f->name)) {
                snprintf(s->cur_file, sizeof(s->cur_file), "%s", f->name);
                s->cur_size = f->size;
            }
        }
    }
    if (s && failed) {
        read_failed(f, s, start, size, total, err, dt, pos_after);
    } else if (s && slow) {
        s->slow++;
        if (normal_allowed(s)) {
            record(s, MSGL_WARN, true, "Slow disc read: file=%s offset=%" PRId64
                   " length=%" PRId64 " got=%" PRId64 " took %" PRId64 " ms",
                   f->name, start, size, total, dt / 1000000);
        }
    }
    if (s && wants_stats) {
        if (f->trace_left > 0 && !failed) {
            f->trace_left--;
            record(s, -1, false, "read file=%s offset=%" PRId64 " length=%" PRId64
                   " got=%" PRId64 " in %.3f ms (seek request #%" PRIu64 ")", f->name,
                   start, size, total, dt / 1e6, s->seek_id);
        }
        s->win_reads++;
        s->win_bytes += total;
        s->win_total_ns += dt;
        s->win_max_ns = MPMAX(s->win_max_ns, dt);
        int64_t now = t0 + dt;
        if (!s->win_start_ns)
            s->win_start_ns = now;
        if (now - s->win_start_ns >= STATS_PERIOD_NS) {
            record(s, -1, false, "stats %.1f s: reads=%" PRIu64 " bytes=%" PRIu64
                   " avg=%.3f ms max=%.3f ms file=%s offset=%" PRId64,
                   (now - s->win_start_ns) / 1e9, s->win_reads, s->win_bytes,
                   s->win_reads ? s->win_total_ns / 1e6 / s->win_reads : 0.0,
                   s->win_max_ns / 1e6, f->name, f->pos);
            s->win_start_ns = now;
            s->win_reads = s->win_bytes = 0;
            s->win_total_ns = s->win_max_ns = 0;
        }
    }
    mp_mutex_unlock(&g_lock);

    if (failed)
        nanum_diag_collect_failure_context();
    return total;
}

static BD_FILE_H *io_open(const char *filename, const char *mode)
{
    mp_mutex_lock(&g_lock);
    BD_FILE_OPEN prev = g_prev_open;
    mp_mutex_unlock(&g_lock);

    // Only plain read access is replaced; writes (BD-J caches) stay libbluray's.
    if (!mode || mode[0] != 'r' || strchr(mode, '+'))
        return prev ? prev(filename, mode) : NULL;

    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, NULL, 0);
    wchar_t *wname = wlen > 0 ? malloc(wlen * sizeof(wchar_t)) : NULL;
    if (!wname || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, wname, wlen)) {
        free(wname);
        return NULL;
    }

    char base[24];
    base_name(filename, base, sizeof(base));
    enum kind kind = classify(base);

    int64_t t0 = mp_time_ns();
    // Same access and sharing as the CRT's fopen(path, "rb").
    HANDLE h = CreateFileW(wname, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    int64_t dt = mp_time_ns() - t0;
    free(wname);

    mp_mutex_lock(&g_lock);
    struct bd_io *s = g_sessions;       // newest; NULL outside any session
    if (h == INVALID_HANDLE_VALUE) {
        // A missing optional file is ordinary for libbluray; a missing clip
        // or any other refusal (sharing, access, device) is not.
        if (s && (kind == KIND_STREAM || !not_found(err))) {
            char text[256];
            os_text(err, text, sizeof(text));
            if (normal_allowed(s)) {
                record(s, MSGL_WARN, true, "Disc file open failed: %s os_error=%lu (%s) "
                       "after %" PRId64 " ms [source: CreateFileW]", filename,
                       (unsigned long)err, text, dt / 1000000);
            }
        } else if (s && nanum_diag_enabled()) {
            record(s, -1, false, "open %s: not found (optional)", filename);
        }
        mp_mutex_unlock(&g_lock);
        return NULL;
    }

    struct io_file *f = calloc(1, sizeof(*f));
    if (!f) {
        mp_mutex_unlock(&g_lock);
        CloseHandle(h);
        return NULL;
    }
    LARGE_INTEGER size = {0};
    GetFileSizeEx(h, &size);
    f->h = h;
    f->kind = kind;
    f->size = size.QuadPart;
    f->trace_left = TRACE_READS;
    snprintf(f->name, sizeof(f->name), "%s", base);
    f->pub.internal = f;
    f->pub.close = io_close;
    f->pub.seek = io_seek;
    f->pub.tell = io_tell;
    f->pub.eof = io_eof;
    f->pub.read = io_read;
    f->pub.write = io_write;
    if (s) {
        f->sess = s;
        s->refs++;
        if (kind == KIND_STREAM) {
            snprintf(s->cur_file, sizeof(s->cur_file), "%s", f->name);
            s->cur_pos = 0;
            s->cur_size = f->size;
        }
        if (kind != KIND_META && nanum_diag_enabled()) {
            record(s, -1, false, "open %s (size %" PRId64 ") in %.3f ms", filename,
                   f->size, dt / 1e6);
        }
    }
    mp_mutex_unlock(&g_lock);
    return &f->pub;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

struct bd_io *bd_io_begin(struct mp_log *log)
{
    struct bd_io *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->refs = 1;
    s->log = log;

    mp_mutex_lock(&g_lock);
    s->id = ++g_next_id;
    s->next = g_sessions;
    g_sessions = s;
    if (g_hook_refs++ == 0)
        g_prev_open = bd_register_file(io_open);
    mp_mutex_unlock(&g_lock);
    return s;
}

void bd_io_end(struct bd_io *s)
{
    if (!s)
        return;
    mp_mutex_lock(&g_lock);
    if (s->failures || s->slow || s->not_shown) {
        record(s, MSGL_WARN, true, "Disc I/O summary: %" PRIu64 " stream reads, %"
               PRIu64 " bytes, %" PRIu64 " failed, %" PRIu64 " slow, %" PRIu64
               " records not shown (limit %d)", s->reads, s->bytes, s->failures,
               s->slow, s->not_shown, MAX_NORMAL_RECORDS);
    } else {
        record(s, -1, true, "summary: %" PRIu64 " stream reads, %" PRIu64
               " bytes, no failures", s->reads, s->bytes);
    }
    s->ended = true;
    s->log = NULL;
    for (struct bd_io **p = &g_sessions; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    if (--g_hook_refs == 0) {
        bd_register_file(g_prev_open);
        g_prev_open = NULL;
    }
    bool last = --s->refs == 0;
    mp_mutex_unlock(&g_lock);
    if (last)
        free(s);
}

void bd_io_info(struct bd_io *s, struct bd_io_info *out)
{
    *out = (struct bd_io_info){0};
    if (!s)
        return;
    mp_mutex_lock(&g_lock);
    snprintf(out->file, sizeof(out->file), "%s", s->cur_file);
    out->file_pos = s->cur_pos;
    out->file_size = s->cur_size;
    out->reads = s->reads;
    out->bytes = s->bytes;
    out->failures = s->failures;
    out->slow = s->slow;
    out->failure_serial = s->failure_serial;
    snprintf(out->failure_file, sizeof(out->failure_file), "%s", s->failure_file);
    out->failure_offset = s->failure_offset;
    out->failure_length = s->failure_length;
    out->failure_got = s->failure_got;
    out->failure_os_error = s->failure_os_error;
    out->failure_time = s->failure_time;
    out->failure_ms = s->failure_ms;
    mp_mutex_unlock(&g_lock);
}

void bd_io_mark_reposition(struct bd_io *s, uint64_t seek_id)
{
    if (!s)
        return;
    mp_mutex_lock(&g_lock);
    s->seek_id = seek_id;
    mp_mutex_unlock(&g_lock);
}
