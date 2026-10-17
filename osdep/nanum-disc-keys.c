/*
 * NanumPlayer: Blu-ray open with automatic AACS key database. See
 * nanum-disc-keys.h for the contract.
 *
 * The database download reuses the shared download engine (nanum-download-win.c:
 * WinHTTP, HTTPS only, certificate validation never relaxed, finite timeouts,
 * abortable request) through nanum_dl_stream_unpinned(), the one entry of that
 * engine that does NOT verify a pinned digest. Pinned assets keep going
 * through nanum_dl_fetch(); nothing here relaxes them.
 *
 * Pipeline of one refresh (everything under a named mutex, so a second player
 * or a rip waits instead of racing):
 *   HTTPS stream -> KEYDB.<pid>.<tick>.<n>.zip.part   (exclusive, delete-on-close)
 *   central directory checked, deflate stream -> KEYDB.<..>.cfg.part
 *     (CRC-32 and length from the archive, plausible text content)
 *   MoveFileEx(.cfg.part, KEYDB.cfg, REPLACE_EXISTING)  (atomic; retried while a
 *     reader still has the old file open)
 * A failure at any step deletes the temporaries and leaves the previous
 * KEYDB.cfg untouched.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include "config.h"

#if HAVE_LIBBLURAY && defined(_WIN32)

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include <zlib.h>

#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/thread_tools.h"
#include "osdep/io.h"
#include "osdep/nanum-disc-keys.h"
#include "osdep/nanum-download.h"
#include "osdep/threads.h"

#define KEYDB_URL "https://fvonline-db.bplaced.net/fv_download.php?lang=eng"
#define KEYDB_ENTRY "keydb.cfg"
#define KEYDB_FILE "KEYDB.cfg"

// Bounds of the untrusted data.
#define MAX_ARCHIVE (INT64_C(64) << 20)
#define MAX_KEYDB (INT64_C(128) << 20)
// A real database has ~190000 disc entries; anything below this is not one.
#define MIN_ENTRIES 100

#define IO_CHUNK NANUM_DL_IO_BUF
#define LOCK_WAIT_MS (15 * 60 * 1000)
#define PUBLISH_WAIT_MS 20000

// libaacs error codes (aacs.h of libaacs 0.12, mirrored by libbluray's
// BLURAY_DISC_INFO.aacs_error_code).
#define AACS_E_CORRUPTED_DISC   (-1)
#define AACS_E_NO_CONFIG        (-2)
#define AACS_E_NO_PK            (-3)
#define AACS_E_NO_CERT          (-4)
#define AACS_E_CERT_REVOKED     (-5)
#define AACS_E_MMC_OPEN         (-6)
#define AACS_E_MMC_FAILURE      (-7)
#define AACS_E_NO_DK            (-8)
#define AACS_E_UNKNOWN          (-9)
#define AACS_E_UNSUPPORTED_DISC (-10)

// ---------------------------------------------------------------------------
// Logging (log may be NULL for the helpers)
// ---------------------------------------------------------------------------

#define LOG_ERR(log, ...)  do { if (log) mp_err(log, __VA_ARGS__); } while (0)
#define LOG_WARN(log, ...) do { if (log) mp_warn(log, __VA_ARGS__); } while (0)
#define LOG_INFO(log, ...) do { if (log) mp_info(log, __VA_ARGS__); } while (0)
#define LOG_VERB(log, ...) do { if (log) mp_verbose(log, __VA_ARGS__); } while (0)

static wchar_t *W(void *ctx, const char *s)
{
    return mp_from_utf8(ctx, s);
}

static void win_fail(struct nanum_dl *dl, const char *what, const char *path)
{
    char why[1024];
    nanum_dl_error_text(why, sizeof(why), GetLastError());
    nanum_dl_fail(dl, "%s %s: %s", what, path, why);
}

// ---------------------------------------------------------------------------
// Locations and cache state
// ---------------------------------------------------------------------------

// Folder of KEYDB.cfg: NANUMPLAYER_DISC_KEYS_DIR, else %APPDATA%\NanumPlayer\disc.
static char *keys_dir(void *ctx)
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

// Age of the cache file in seconds; negative when its time lies in the future.
static int64_t file_age_s(const struct nanum_dl_fst *st)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t now = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return ((int64_t)now - (int64_t)st->mtime) / 10000000;
}

static bool same_state(const struct nanum_dl_fst *a, const struct nanum_dl_fst *b)
{
    return a->exists == b->exists && (!a->exists ||
           (a->size == b->size && a->mtime == b->mtime));
}

// ---------------------------------------------------------------------------
// Cross-process / cross-thread serialization of refreshes
// ---------------------------------------------------------------------------

enum lock_res { LOCK_OK, LOCK_CANCEL, LOCK_FAIL };

static enum lock_res lock_acquire(void *ctx, const char *dir, struct mp_cancel *cancel,
                                  struct mp_log *log, HANDLE *out, char *err, size_t errsz)
{
    // FNV-1a of the lowercase folder: one lock per key folder.
    uint32_t hash = 2166136261u;
    for (const wchar_t *p = W(ctx, dir); *p; p++) {
        wchar_t c = (wchar_t)towlower(*p);
        hash = (hash ^ (uint32_t)c) * 16777619u;
    }
    wchar_t name[80];
    swprintf(name, MP_ARRAY_SIZE(name), L"Local\\NanumPlayerDiscKeys-%08x", hash);
    HANDLE m = CreateMutexW(NULL, FALSE, name);
    if (!m) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        snprintf(err, errsz, "Cannot create the key database lock: %s", why);
        return LOCK_FAIL;
    }
    HANDLE h[2] = {m, cancel ? (HANDLE)mp_cancel_get_event(cancel) : NULL};
    DWORD count = h[1] ? 2 : 1;
    bool announced = false;
    for (DWORD waited = 0; waited <= LOCK_WAIT_MS; waited += 1000) {
        DWORD r = WaitForMultipleObjects(count, h, FALSE, waited ? 1000 : 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0) {
            *out = m;
            return LOCK_OK;
        }
        if (r == WAIT_OBJECT_0 + 1) {
            CloseHandle(m);
            return LOCK_CANCEL;
        }
        if (r != WAIT_TIMEOUT) {
            CloseHandle(m);
            snprintf(err, errsz, "Waiting for the key database lock failed (error %lu)",
                     (unsigned long)GetLastError());
            return LOCK_FAIL;
        }
        if (!announced) {
            announced = true;
            LOG_INFO(log, "Another NanumPlayer task is updating the AACS key database; "
                          "waiting for it.\n");
        }
    }
    CloseHandle(m);
    snprintf(err, errsz, "Another NanumPlayer task has been updating the key database for "
             "more than %d minutes", LOCK_WAIT_MS / 60000);
    return LOCK_FAIL;
}

static void lock_release(HANDLE m)
{
    ReleaseMutex(m);
    CloseHandle(m);
}

// ---------------------------------------------------------------------------
// Cancellation bridge: the player's mp_cancel has ONE callback slot that
// belongs to its owner, so it is only observed, from a watcher thread that
// aborts this call's private downloader.
// ---------------------------------------------------------------------------

struct watch {
    struct nanum_dl *dl;
    HANDLE cancel_ev;
    HANDLE stop;
    mp_thread thread;
    bool running;
};

static MP_THREAD_VOID watch_main(void *p)
{
    struct watch *w = p;
    HANDLE h[2] = {w->stop, w->cancel_ev};
    if (WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
        nanum_dl_abort(w->dl);
    MP_THREAD_RETURN();
}

static void watch_start(struct watch *w, struct nanum_dl *dl, struct mp_cancel *cancel)
{
    *w = (struct watch){.dl = dl};
    if (!cancel)
        return;
    w->cancel_ev = mp_cancel_get_event(cancel);
    w->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (w->cancel_ev && w->stop && !mp_thread_create(&w->thread, watch_main, w))
        w->running = true;
}

static void watch_stop(struct watch *w)
{
    if (w->running) {
        SetEvent(w->stop);
        mp_thread_join(w->thread);
    }
    if (w->stop)
        CloseHandle(w->stop);
    *w = (struct watch){0};
}

// ---------------------------------------------------------------------------
// ZIP: the one accepted layout is a single regular deflated "keydb.cfg",
// no encryption, no zip64, no multi-volume, local header at offset 0, the
// central directory right behind the data and the end record at the very end.
// ---------------------------------------------------------------------------

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool read_at(struct nanum_dl *dl, HANDLE f, int64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER pos = {.QuadPart = off};
    DWORD got = 0;
    if (!SetFilePointerEx(f, pos, NULL, FILE_BEGIN) || !ReadFile(f, buf, n, &got, NULL)) {
        win_fail(dl, "Cannot read", "the downloaded archive");
        return false;
    }
    if (got != n) {
        nanum_dl_fail(dl, "The downloaded archive is truncated.");
        return false;
    }
    return true;
}

#define ZIP_BAD(dl, ...) \
    do { \
        char zwhy_[400]; \
        snprintf(zwhy_, sizeof(zwhy_), __VA_ARGS__); \
        nanum_dl_fail((dl), "The key database archive is not a plain ZIP holding only " \
                      KEYDB_ENTRY " (%s).", zwhy_); \
        return NANUM_DL_FAIL; \
    } while (0)

struct zip_entry {
    int64_t data_start;
    int64_t csize;
    int64_t usize;
    uint32_t crc;
};

static bool entry_name_ok(const uint8_t *name, size_t len)
{
    return len == sizeof(KEYDB_ENTRY) - 1 && !_strnicmp((const char *)name, KEYDB_ENTRY, len);
}

static enum nanum_dl_res zip_locate(struct nanum_dl *dl, HANDLE zip, int64_t zsize,
                                    struct zip_entry *e)
{
    uint8_t *buf = dl->buf;     // NANUM_DL_IO_BUF bytes
    enum { EOCD = 22, MAX_COMMENT = 65535 };
    if (zsize < EOCD + 46 + 30)
        ZIP_BAD(dl, "too small");

    // End of central directory: the record must end exactly at the end of the
    // file (its comment length says so), nothing is appended.
    int64_t tl = zsize < EOCD + MAX_COMMENT ? zsize : EOCD + MAX_COMMENT;
    if (!read_at(dl, zip, zsize - tl, buf, (DWORD)tl))
        return NANUM_DL_FAIL;
    int64_t at = -1;
    for (int64_t i = tl - EOCD; i >= 0; i--) {
        if (rd32(buf + i) == 0x06054b50 && i + EOCD + rd16(buf + i + 20) == tl) {
            at = i;
            break;
        }
    }
    if (at < 0)
        ZIP_BAD(dl, "no end-of-archive record");
    const uint8_t *eo = buf + at;
    int64_t eocd_abs = zsize - tl + at;
    if (at >= 20 && rd32(eo - 20) == 0x07064b50)
        ZIP_BAD(dl, "zip64");
    if (rd16(eo + 4) || rd16(eo + 6))
        ZIP_BAD(dl, "multi-volume");
    uint16_t entries = rd16(eo + 10);
    uint32_t cd_size = rd32(eo + 12), cd_off = rd32(eo + 16);
    if (rd16(eo + 8) == 0xFFFF || entries == 0xFFFF || cd_size == 0xFFFFFFFFu ||
        cd_off == 0xFFFFFFFFu)
        ZIP_BAD(dl, "zip64");
    if (rd16(eo + 8) != 1 || entries != 1)
        ZIP_BAD(dl, "%u entries", (unsigned)entries);
    if (cd_size < 46 || cd_size > 4096 || (int64_t)cd_off + cd_size != eocd_abs)
        ZIP_BAD(dl, "unexpected directory layout");

    // The single central directory record.
    if (!read_at(dl, zip, cd_off, buf, cd_size))
        return NANUM_DL_FAIL;
    uint16_t made_by = rd16(buf + 4), flags = rd16(buf + 8), method = rd16(buf + 10);
    uint32_t crc = rd32(buf + 16), csize = rd32(buf + 20), usize = rd32(buf + 24);
    uint16_t nlen = rd16(buf + 28), elen = rd16(buf + 30), clen = rd16(buf + 32);
    uint16_t disk = rd16(buf + 34);
    uint32_t eattr = rd32(buf + 38), lho = rd32(buf + 42);
    if (rd32(buf) != 0x02014b50 || 46u + nlen + elen + clen != cd_size)
        ZIP_BAD(dl, "unexpected directory record");
    if (flags & (1 | (1 << 6) | (1 << 13)))
        ZIP_BAD(dl, "encrypted");
    if (method != 8)
        ZIP_BAD(dl, "compression method %u", (unsigned)method);
    if (disk || lho)
        ZIP_BAD(dl, "entry is not at the start of the archive");
    if (!entry_name_ok(buf + 46, nlen))
        ZIP_BAD(dl, "the entry is not named " KEYDB_ENTRY);
    for (const uint8_t *x = buf + 46 + nlen, *end = x + elen; x < end;) {
        if (end - x < 4 || x + 4 + rd16(x + 2) > end)
            ZIP_BAD(dl, "malformed extra field");
        if (rd16(x) == 1)
            ZIP_BAD(dl, "zip64");
        x += 4 + rd16(x + 2);
    }
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu)
        ZIP_BAD(dl, "zip64");
    if ((eattr & 0x10) || ((made_by >> 8) == 3 && (eattr >> 16) &&
                           ((eattr >> 16) & 0170000) != 0100000))
        ZIP_BAD(dl, "not a regular file");
    if (!csize || !usize || usize > MAX_KEYDB || csize > cd_off)
        ZIP_BAD(dl, "entry sizes %u/%u out of bounds", (unsigned)csize, (unsigned)usize);

    // Its local header, directly followed by the data.
    uint8_t lh[30 + sizeof(KEYDB_ENTRY)];
    if (!read_at(dl, zip, 0, lh, sizeof(lh)))
        return NANUM_DL_FAIL;
    uint16_t lflags = rd16(lh + 6);
    uint16_t lnlen = rd16(lh + 26), lelen = rd16(lh + 28);
    if (rd32(lh) != 0x04034b50 || lnlen != nlen || !entry_name_ok(lh + 30, lnlen))
        ZIP_BAD(dl, "unexpected local header");
    if ((lflags & (1 | (1 << 6) | (1 << 13))) || rd16(lh + 8) != 8)
        ZIP_BAD(dl, "local header is encrypted or not deflated");
    if (!(lflags & 8) && (rd32(lh + 14) != crc || rd32(lh + 18) != csize ||
                          rd32(lh + 22) != usize))
        ZIP_BAD(dl, "local header disagrees with the directory");
    int64_t data_start = 30 + nlen + lelen;
    int64_t data_end = data_start + csize;
    int64_t gap = (int64_t)cd_off - data_end;
    bool has_dd = lflags & 8;
    if (gap < 0 || (has_dd ? (gap != 12 && gap != 16) : gap != 0))
        ZIP_BAD(dl, "unexpected data layout");
    if (gap) {
        // Data descriptor: optional signature, then crc and both sizes.
        uint8_t dd[16];
        if (!read_at(dl, zip, data_end, dd, (DWORD)gap))
            return NANUM_DL_FAIL;
        const uint8_t *d = dd;
        if (gap == 16) {
            if (rd32(d) != 0x08074b50)
                ZIP_BAD(dl, "bad data descriptor");
            d += 4;
        }
        if (rd32(d) != crc || rd32(d + 4) != csize || rd32(d + 8) != usize)
            ZIP_BAD(dl, "data descriptor disagrees with the directory");
    }
    e->data_start = data_start;
    e->csize = csize;
    e->usize = usize;
    e->crc = crc;
    return NANUM_DL_OK;
}

// Plausible-content scan of the unpacked database: text without NUL bytes and
// with disc entries ("0x<disc id> = <title> | ...") in bulk.
struct scan {
    int col;
    bool zero_first;
    bool nul;
    int64_t entries;
};

static void scan_feed(struct scan *s, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (!b)
            s->nul = true;
        if (b == '\n') {
            s->col = 0;
        } else if (s->col == 0) {
            s->zero_first = b == '0';
            s->col = 1;
        } else if (s->col == 1) {
            if (s->zero_first && b == 'x')
                s->entries++;
            s->col = 2;
        }
    }
}

static enum nanum_dl_res zip_inflate(struct nanum_dl *dl, struct mp_cancel *cancel,
                                     HANDLE zip, const struct zip_entry *e, HANDLE out,
                                     const char *out_path)
{
    enum nanum_dl_res r = NANUM_DL_FAIL;
    uint8_t *obuf = malloc(IO_CHUNK);
    z_stream zs = {0};
    bool inited = false;
    if (!obuf) {
        nanum_dl_fail(dl, "Out of memory");
        goto out;
    }
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        nanum_dl_fail(dl, "zlib could not be initialised");
        goto out;
    }
    inited = true;

    struct scan scan = {0};
    uLong crc = crc32(0L, Z_NULL, 0);
    int64_t remaining = e->csize, off = e->data_start, produced_total = 0;
    int rc;
    do {
        if (nanum_dl_cancelled(dl) || mp_cancel_test(cancel)) {
            r = NANUM_DL_CANCEL;
            goto out;
        }
        if (!zs.avail_in && remaining > 0) {
            DWORD n = remaining < IO_CHUNK ? (DWORD)remaining : IO_CHUNK;
            if (!read_at(dl, zip, off, dl->buf, n))
                goto out;
            zs.next_in = dl->buf;
            zs.avail_in = n;
            off += n;
            remaining -= n;
        }
        zs.next_out = obuf;
        zs.avail_out = IO_CHUNK;
        rc = inflate(&zs, Z_NO_FLUSH);
        size_t produced = IO_CHUNK - zs.avail_out;
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            nanum_dl_fail(dl, "The compressed " KEYDB_ENTRY " is corrupt (zlib error %d).", rc);
            goto out;
        }
        if (rc == Z_BUF_ERROR && !produced && !zs.avail_in && !remaining) {
            nanum_dl_fail(dl, "The compressed " KEYDB_ENTRY " is truncated.");
            goto out;
        }
        if (produced) {
            produced_total += produced;
            if (produced_total > e->usize) {
                nanum_dl_fail(dl, "The unpacked " KEYDB_ENTRY " is larger than the %"
                              PRId64 " bytes the archive announces.", e->usize);
                goto out;
            }
            crc = crc32(crc, obuf, (uInt)produced);
            scan_feed(&scan, obuf, produced);
            DWORD written = 0;
            if (!WriteFile(out, obuf, (DWORD)produced, &written, NULL) || written != produced) {
                win_fail(dl, "Cannot write", out_path);
                goto out;
            }
        }
    } while (rc != Z_STREAM_END);

    if (remaining || zs.avail_in) {
        nanum_dl_fail(dl, "The archive has data behind the end of the compressed " KEYDB_ENTRY ".");
        goto out;
    }
    if (produced_total != e->usize) {
        nanum_dl_fail(dl, "The unpacked " KEYDB_ENTRY " has %" PRId64 " bytes, the archive "
                      "announces %" PRId64 ".", produced_total, e->usize);
        goto out;
    }
    if ((uint32_t)crc != e->crc) {
        nanum_dl_fail(dl, "The unpacked " KEYDB_ENTRY " fails its CRC-32 check.");
        goto out;
    }
    if (scan.nul || scan.entries < MIN_ENTRIES) {
        nanum_dl_fail(dl, "The unpacked " KEYDB_ENTRY " is not an AACS key database "
                      "(%" PRId64 " disc entries%s).", scan.entries,
                      scan.nul ? ", binary data" : "");
        goto out;
    }
    if (!FlushFileBuffers(out)) {
        win_fail(dl, "Cannot write", out_path);
        goto out;
    }
    r = NANUM_DL_OK;
out:
    if (inited)
        inflateEnd(&zs);
    free(obuf);
    return r;
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

struct archive_sink {
    struct nanum_dl *dl;
    HANDLE file;
    const char *path;
};

static bool archive_write(void *user, const void *data, size_t size)
{
    struct archive_sink *s = user;
    DWORD written = 0;
    if (!WriteFile(s->file, data, (DWORD)size, &written, NULL) || written != size) {
        win_fail(s->dl, "Cannot write", s->path);
        return false;
    }
    return true;
}

// Exclusive, uniquely named temporary file of this refresh.
static HANDLE create_part(struct nanum_dl *dl, void *ctx, const char *dir, const char *kind,
                          DWORD access, DWORD flags, char **path)
{
    for (int i = 0; i < 100; i++) {
        *path = talloc_asprintf(ctx, "%s\\KEYDB.%lu.%llu.%d.%s.part", dir,
                                (unsigned long)GetCurrentProcessId(),
                                (unsigned long long)GetTickCount64(), i, kind);
        HANDLE h = CreateFileW(W(ctx, *path), access, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | flags, NULL);
        if (h != INVALID_HANDLE_VALUE)
            return h;
        DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
            break;
    }
    win_fail(dl, "Cannot create", *path);
    return INVALID_HANDLE_VALUE;
}

// Holding the lock no other refresh runs, so every *.part is left over from a
// crashed one.
static void remove_stale_parts(void *ctx, const char *dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(ctx, talloc_asprintf(ctx, "%s\\KEYDB.*.part", dir)), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char *name = mp_to_utf8(ctx, fd.cFileName);
            DeleteFileW(W(ctx, talloc_asprintf(ctx, "%s\\%s", dir, name)));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Atomic replacement. Readers (libaacs through fopen) do not share-delete, so
// the move is retried for a while when somebody is reading the old file.
static enum nanum_dl_res publish(struct nanum_dl *dl, struct mp_cancel *cancel, void *ctx,
                                 const char *part, const char *final_path)
{
    wchar_t *wp = W(ctx, part), *wf = W(ctx, final_path);
    DWORD err = 0;
    for (DWORD waited = 0; waited <= PUBLISH_WAIT_MS; waited += 250) {
        if (MoveFileExW(wp, wf, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return NANUM_DL_OK;
        err = GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION)
            break;
        if (nanum_dl_cancelled(dl) || mp_cancel_test(cancel))
            return NANUM_DL_CANCEL;
        Sleep(250);
    }
    char why[1024];
    nanum_dl_error_text(why, sizeof(why), err);
    nanum_dl_fail(dl, "Cannot replace %s: %s.%s The previous database was kept.", final_path, why,
                  err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION
                  ? " The file is in use by another player or rip." : "");
    return NANUM_DL_FAIL;
}

enum refresh_res { REFRESH_UPDATED, REFRESH_FRESH, REFRESH_CANCEL, REFRESH_FAIL };

// Downloads and installs the database unless the cache is younger than
// NANUM_DISC_KEYS_MIN_REFRESH_S (REFRESH_FRESH: the caller compares the cache
// with what it saw to learn whether somebody else just updated it). On
// REFRESH_FAIL `err` holds the reason.
static enum refresh_res refresh_keydb(struct mp_log *log, struct mp_cancel *cancel,
                                      const char *dir, const char *keydb,
                                      char *err, size_t errsz)
{
    void *ctx = talloc_new(NULL);
    enum refresh_res res = REFRESH_FAIL;
    struct nanum_dl dl;
    nanum_dl_init(&dl, NULL, NULL, NULL);
    struct watch watch = {0};
    HANDLE lock = NULL;
    HANDLE zip = INVALID_HANDLE_VALUE, out = INVALID_HANDLE_VALUE;
    char *zip_path = NULL, *out_path = NULL;
    bool job = false, published = false;

    if (!nanum_dl_begin(&dl)) {
        snprintf(err, errsz, "Out of memory");
        goto done;
    }
    job = true;

    switch (lock_acquire(ctx, dir, cancel, log, &lock, err, errsz)) {
    case LOCK_OK: break;
    case LOCK_CANCEL: res = REFRESH_CANCEL; goto done;
    default: goto done;
    }

    // Somebody else (or an earlier attempt) may just have refreshed it.
    struct nanum_dl_fst st;
    if (nanum_dl_stat(keydb, &st)) {
        int64_t age = file_age_s(&st);
        if (age >= 0 && age < NANUM_DISC_KEYS_MIN_REFRESH_S) {
            res = REFRESH_FRESH;
            goto done;
        }
    }

    enum nanum_dl_res r = NANUM_DL_FAIL;
    struct archive_sink sink = {&dl, INVALID_HANDLE_VALUE, NULL};
    struct zip_entry ze = {0};
    int64_t zsize = 0;

    // Archive plus unpacked file, bounded by the limits (the real ones are ~90 MB).
    if (!nanum_dl_prepare_dir(&dl, keydb, MAX_KEYDB, "the AACS key database"))
        goto result;
    remove_stale_parts(ctx, dir);

    LOG_INFO(log, "Downloading the public AACS key database (about 25 MB, "
                  "fvonline-db.bplaced.net) ...\n");

    // The archive disappears with its handle, whatever happens.
    zip = create_part(&dl, ctx, dir, "zip", GENERIC_READ | GENERIC_WRITE | DELETE,
                      FILE_FLAG_DELETE_ON_CLOSE, &zip_path);
    if (zip == INVALID_HANDLE_VALUE)
        goto result;

    watch_start(&watch, &dl, cancel);
    sink.file = zip;
    sink.path = zip_path;
    r = nanum_dl_stream_unpinned(&dl, KEYDB_URL, MAX_ARCHIVE, archive_write, &sink, &zsize);
    if (r == NANUM_DL_OK && (nanum_dl_cancelled(&dl) || mp_cancel_test(cancel)))
        r = NANUM_DL_CANCEL;
    if (r != NANUM_DL_OK)
        goto result;

    r = zip_locate(&dl, zip, zsize, &ze);
    if (r != NANUM_DL_OK)
        goto result;

    out = create_part(&dl, ctx, dir, "cfg", GENERIC_WRITE, 0, &out_path);
    if (out == INVALID_HANDLE_VALUE) {
        r = NANUM_DL_FAIL;
        goto result;
    }
    r = zip_inflate(&dl, cancel, zip, &ze, out, out_path);
    CloseHandle(out);
    out = INVALID_HANDLE_VALUE;
    if (r != NANUM_DL_OK)
        goto result;

    r = publish(&dl, cancel, ctx, out_path, keydb);
    published = r == NANUM_DL_OK;
    if (published)
        LOG_INFO(log, "AACS key database updated (%.1f MiB).\n", ze.usize / 1048576.0);

result:
    if (r == NANUM_DL_OK) {
        res = REFRESH_UPDATED;
    } else if (r == NANUM_DL_CANCEL) {
        res = REFRESH_CANCEL;
    } else {
        snprintf(err, errsz, "%s", dl.err[0] ? dl.err : "Unknown error");
    }

done:
    watch_stop(&watch);
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    if (out_path && !published)
        DeleteFileW(W(ctx, out_path));
    if (zip != INVALID_HANDLE_VALUE)
        CloseHandle(zip);   // delete-on-close
    if (lock)
        lock_release(lock);
    if (job)
        nanum_dl_end(&dl);
    mp_mutex_destroy(&dl.lock);
    talloc_free(ctx);
    return res;
}

// ---------------------------------------------------------------------------
// The open
// ---------------------------------------------------------------------------

static const char *aacs_strerr(int code)
{
    switch (code) {
    case AACS_E_CORRUPTED_DISC:   return "opening or reading of AACS files failed";
    case AACS_E_NO_CONFIG:        return "missing config file";
    case AACS_E_NO_PK:            return "no matching processing key";
    case AACS_E_NO_CERT:          return "no valid certificate";
    case AACS_E_CERT_REVOKED:     return "certificate has been revoked";
    case AACS_E_MMC_OPEN:         return "MMC open failed (maybe no MMC drive?)";
    case AACS_E_MMC_FAILURE:      return "MMC failed";
    case AACS_E_NO_DK:            return "no matching device key";
    case AACS_E_UNKNOWN:          return "unknown failure";
    case AACS_E_UNSUPPORTED_DISC: return "unsupported AACS version";
    default:                      return "unknown error";
    }
}

// Errors a (newer) key database can cure.
static bool keys_can_help(int code)
{
    switch (code) {
    case AACS_E_NO_CONFIG:
    case AACS_E_NO_PK:
    case AACS_E_NO_CERT:
    case AACS_E_CERT_REVOKED:
    case AACS_E_NO_DK:
        return true;
    default:
        return false;
    }
}

// What the final failure means, for the user.
static const char *aacs_hint(int code, bool db_current)
{
    switch (code) {
    case AACS_E_NO_CONFIG:
        return "The AACS key database is missing or unreadable.";
    case AACS_E_NO_PK:
    case AACS_E_NO_DK:
        return db_current
            ? "No keys for this disc: the public key database has no processing or device "
              "key for this disc's AACS media key block, so it cannot be decrypted. "
              "Discs newer than the database are not supported until it is updated."
            : "The key database has no processing or device key for this disc.";
    case AACS_E_NO_CERT:
        return db_current
            ? "No keys for this disc: it is not in the public key database and no usable "
              "host certificate is available to read its keys from the drive."
            : "The key database has no entry for this disc.";
    case AACS_E_CERT_REVOKED:
        return "No keys for this disc: every host certificate of the key database has been "
               "revoked for this disc.";
    case AACS_E_MMC_OPEN:
    case AACS_E_MMC_FAILURE:
        return "The drive could not be used for AACS authentication; the disc has to be read "
               "from a Blu-ray drive.";
    case AACS_E_CORRUPTED_DISC:
        return "The AACS files of the disc could not be read.";
    case AACS_E_UNSUPPORTED_DISC:
        return "This AACS version (for example AACS 2 of Ultra HD Blu-ray) is not supported.";
    default:
        return "The disc could not be decrypted.";
    }
}

BLURAY *nanum_disc_bd_open(const char *device, struct mp_log *log, struct mp_cancel *cancel)
{
    void *ctx = talloc_new(NULL);
    char *dir = keys_dir(ctx);
    char *keydb = dir ? nanum_dl_join(ctx, dir, KEYDB_FILE) : NULL;
    bool refreshed = false;

    if (!dir)
        LOG_WARN(log, "The AACS key database folder cannot be determined "
                      "(%%APPDATA%% is unavailable).\n");

    // What the cache was when this open began: a refresh by somebody else
    // in between shows as a change.
    struct nanum_dl_fst seen = {0};
    if (keydb)
        nanum_dl_stat(keydb, &seen);

    BLURAY *bd = bd_open(device, keydb);
    for (;;) {
        if (!bd)
            break;
        if (mp_cancel_test(cancel)) {
            bd_close(bd);
            bd = NULL;
            break;
        }
        const BLURAY_DISC_INFO *info = bd_get_disc_info(bd);
        if (!info || !info->bluray_detected)
            break;      // nothing to decrypt; the caller rejects non-Blu-rays

        if (info->aacs_detected) {
            if (!info->libaacs_detected) {
                LOG_ERR(log, "AACS encryption detected but cannot find libaacs.\n");
                bd_close(bd);
                bd = NULL;
                break;
            }
            if (!info->aacs_handled) {
                int code = info->aacs_error_code;
                int mkbv = info->aacs_mkbv;
                bool db_current = refreshed;
                if (!refreshed && keydb && keys_can_help(code)) {
                    refreshed = true;
                    // The device must not stay open while the database changes.
                    bd_close(bd);
                    bd = NULL;
                    char err[700] = "";
                    enum refresh_res rr = refresh_keydb(log, cancel, dir, keydb, err,
                                                        sizeof(err));
                    if (rr == REFRESH_CANCEL)
                        break;
                    struct nanum_dl_fst now;
                    nanum_dl_stat(keydb, &now);
                    if (rr == REFRESH_FAIL) {
                        LOG_ERR(log, "AACS key database unavailable: %s%s\n", err,
                                now.exists ? " The previous database was kept; it has no "
                                             "keys for this disc." :
                                             " Without it this disc cannot be decrypted; "
                                             "check the internet connection and open it again.");
                        break;
                    }
                    if (rr == REFRESH_UPDATED || !same_state(&seen, &now)) {
                        seen = now;
                        bd = bd_open(device, keydb);
                        if (!bd)
                            LOG_ERR(log, "Couldn't open Blu-ray device: %s\n", device);
                        continue;   // re-evaluate the reopened disc
                    }
                    // Downloaded recently and still without keys for this disc.
                    db_current = true;
                }
                if (bd) {
                    bd_close(bd);
                    bd = NULL;
                }
                LOG_ERR(log, "AACS error: %s\n", aacs_strerr(code));
                if (mkbv > 0)
                    LOG_ERR(log, "The disc uses AACS MKB version %d.\n", mkbv);
                LOG_ERR(log, "%s\n", aacs_hint(code, db_current));
                break;
            }
            LOG_VERB(log, "AACS handled (MKB version %d).\n", info->aacs_mkbv);
        }

        if (info->bdplus_detected) {
            if (!info->libbdplus_detected) {
                LOG_ERR(log, "BD+ encryption detected but cannot find libbdplus.\n");
                bd_close(bd);
                bd = NULL;
            } else if (!info->bdplus_handled) {
                LOG_ERR(log, "Cannot decrypt BD+ encryption.\n");
                LOG_ERR(log, "This disc uses BD+ and libbdplus could not set up its virtual "
                             "machine for it. The AACS key database does not help with BD+.\n");
                bd_close(bd);
                bd = NULL;
            }
        }
        break;
    }

    talloc_free(ctx);
    return bd;
}

#endif
