/*
 * NanumPlayer: YouTube component manager (Windows). See nanum-youtube.h.
 *
 * Components (id -> file in <youtube dir>\bin):
 *   yt-dlp  yt-dlp.exe  official PyInstaller build (bundles the EJS solver
 *                       scripts; verified against the release's SHA2-256SUMS)
 *   deno    deno.exe    JavaScript runtime yt-dlp runs the solver in (it finds
 *                       it next to yt-dlp.exe); extracted from the release zip
 *                       verified against its .sha256sum
 * Each installed file gets <file>.version ("<tag>\n<size>\n").
 *
 * Published as the property user-data/nanum/youtube (a map):
 *   status     idle | checking | downloading | verifying | extracting | done |
 *              cancelled | error
 *   current    "yt-dlp" | "deno" while one is processed, else ""
 *   received   bytes of the current file written / hashed / extracted
 *   total      size of the current file
 *   error      message, "" unless status is error
 *   directory  the youtube folder (profile\, cookies.txt, bin\); bin  its bin\
 *   busy       true while the worker runs
 *   installed  yt-dlp and deno are both present
 *   version    installed yt-dlp release tag ("" if absent or unknown)
 *   latest     latest yt-dlp release tag ("" until a check succeeded)
 *   update_available  yt-dlp is installed and differs from latest
 *   ytdl_path, js_runtime_path  where the files are (or will be)
 *   components [{id, label, filename, size, installed, version, path}]
 *
 * Threads: commands and publishing run on the core thread. The single worker
 * does network, file and hash work and talks to the core only by enqueueing a
 * mergeable callback on the core's dispatch queue.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <zlib.h>

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
#include "osdep/nanum-youtube.h"
#include "osdep/nanum-download.h"
#include "osdep/threads.h"

#define PROGRESS_MS 200
#define MAX_JSON_BYTES (INT64_C(8) << 20)
#define MAX_SUMS_BYTES (INT64_C(1) << 20)

enum { C_YTDLP, C_DENO, C_COUNT };

static const struct {
    const char *id;
    const char *label;
    const char *file;       // installed name
    const char *api;        // latest release (GitHub API)
    const char *asset;      // release asset that is downloaded
    const char *sums;       // release asset holding its SHA-256; NULL: asset + ".sha256sum"
} KNOWN[C_COUNT] = {
    [C_YTDLP] = {"yt-dlp", "yt-dlp", "yt-dlp.exe",
                 "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest",
                 "yt-dlp.exe", "SHA2-256SUMS"},
    [C_DENO]  = {"deno", "Deno (JavaScript 실행기)", "deno.exe",
                 "https://api.github.com/repos/denoland/deno/releases/latest",
                 "deno-x86_64-pc-windows-msvc.zip", NULL},
};

enum state {
    ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING, ST_EXTRACTING, ST_DONE, ST_CANCELLED,
    ST_ERROR,
};
static const char *const STATE_NAME[] = {
    "idle", "checking", "downloading", "verifying", "extracting", "done", "cancelled", "error",
};

enum mode { MODE_CHECK, MODE_INSTALL };

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;

static struct {
    // Written by nanum_youtube_startup() and the core thread only.
    bool init;
    struct MPContext *mpctx;
    struct mp_log *log;
    char *dir;                  // youtube folder or NULL
    char *bin;
    bool thread_valid;
    mp_thread thread;

    // Worker interface; guarded by g_lock unless noted.
    struct nanum_dl dl;
    bool shutting_down;
    bool running;
    enum mode mode;
    enum state st;
    int cur;
    int64_t recv, total;
    char err[640];
    char latest[64];            // latest yt-dlp tag
    int64_t asset_size[C_COUNT];    // of the last release lookup
    ULONGLONG last_pub;
} g;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static wchar_t *W(void *ctx, const char *s)
{
    return mp_from_utf8(ctx, s);
}

static char *comp_path(void *ctx, int i)
{
    return g.bin ? nanum_dl_join(ctx, g.bin, KNOWN[i].file) : NULL;
}

static char *stamp_path(void *ctx, int i)
{
    char *p = comp_path(ctx, i);
    return p ? talloc_asprintf(ctx, "%s.version", p) : NULL;
}

static bool installed(int i)
{
    void *tmp = talloc_new(NULL);
    char *p = comp_path(tmp, i);
    struct nanum_dl_fst f;
    bool ok = p && nanum_dl_stat(p, &f) && f.size > 0;
    talloc_free(tmp);
    return ok;
}

// Release tag of an installed component ("" if there is no valid stamp).
static void installed_version(int i, char out[64])
{
    out[0] = 0;
    void *tmp = talloc_new(NULL);
    char *sp = stamp_path(tmp, i), *p = comp_path(tmp, i);
    char *text = sp ? nanum_dl_read_text(tmp, sp) : NULL;
    struct nanum_dl_fst f;
    if (text && p && nanum_dl_stat(p, &f)) {
        char tag[64];
        long long size = 0;
        if (sscanf(text, "%63s %lld", tag, &size) == 2 && size == f.size)
            snprintf(out, 64, "%s", tag);
    }
    talloc_free(tmp);
}

static void write_stamp(int i, const char *tag)
{
    void *tmp = talloc_new(NULL);
    char *sp = stamp_path(tmp, i), *p = comp_path(tmp, i);
    struct nanum_dl_fst f;
    if (sp && p && nanum_dl_stat(p, &f)) {
        char *text = talloc_asprintf(tmp, "%s\n%" PRId64 "\n", tag, f.size);
        HANDLE h = CreateFileW(W(tmp, sp), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD n;
            WriteFile(h, text, (DWORD)strlen(text), &n, NULL);
            CloseHandle(h);
        }
    }
    talloc_free(tmp);
}

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
static void youtube_phase(struct nanum_dl *dl, enum nanum_dl_phase ph, int64_t recv,
                          int64_t total)
{
    static const enum state map[] = {ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING};
    set_state(map[ph], dl->tag, recv, total, NULL);
}

static void youtube_progress(struct nanum_dl *dl, int64_t recv)
{
    set_progress(recv, false);
}

// ---------------------------------------------------------------------------
// Release lookup
// ---------------------------------------------------------------------------

struct rel {
    char tag[64];
    struct nanum_dl_item item;      // the asset to download (size, url, sha)
};

static bool valid_tag(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= sizeof(((struct rel *)0)->tag))
        return false;
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') ||
              (*s >= 'A' && *s <= 'Z') || *s == '.' || *s == '-' || *s == '_'))
            return false;
    }
    return true;
}

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// SHA-256 (lowercase hex) out of a checksum file. With a name: the
// "<hex> [*]<name>" line (sha256sum format) of that file. Without: the first
// 64 hex digit token, which is what the single-file checksum of a release
// holds (Deno's is certutil style: "Hash : <hex>").
static bool find_sha(const char *text, const char *name, char out[65])
{
    for (const char *p = text; *p;) {
        if (!is_hex(*p)) {
            p++;
            continue;
        }
        const char *q = p;
        while (is_hex(*q))
            q++;
        if (q - p == 64) {
            bool ok = !name;
            if (name) {
                const char *eol = q + strcspn(q, "\r\n");
                while (q < eol && (*q == ' ' || *q == '\t' || *q == '*'))
                    q++;
                ok = (size_t)(eol - q) == strlen(name) && !strncmp(q, name, eol - q);
            }
            if (ok) {
                for (int k = 0; k < 64; k++)
                    out[k] = (char)(p[k] >= 'A' && p[k] <= 'F' ? p[k] + 32 : p[k]);
                out[64] = 0;
                return true;
            }
        }
        p = q;
    }
    return false;
}

static mpv_node *find_asset(mpv_node *assets, const char *name)
{
    for (int k = 0; k < assets->u.list->num; k++) {
        mpv_node *a = &assets->u.list->values[k];
        const char *n = a->format == MPV_FORMAT_NODE_MAP ? nanum_dl_node_str(a, "name") : NULL;
        if (n && !strcmp(n, name))
            return a;
    }
    return NULL;
}

// Latest release of component i: its tag and the verified description of the
// asset to download. Worker thread.
static enum nanum_dl_res get_release(int i, struct rel *rel)
{
    struct nanum_dl *dl = &g.dl;
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r;
    char *json = NULL, *sums = NULL;
    dl->tag = i;
    if ((r = nanum_dl_get_text(dl, KNOWN[i].api, MAX_JSON_BYTES, tmp, &json)) != NANUM_DL_OK)
        goto out;

    char *p = json;
    mpv_node doc;
    if (json_parse(tmp, &doc, &p, MAX_JSON_DEPTH) < 0 || doc.format != MPV_FORMAT_NODE_MAP) {
        nanum_dl_fail(dl, "The release information of %s is not valid JSON.", KNOWN[i].id);
        r = NANUM_DL_FAIL;
        goto out;
    }
    const char *tag = nanum_dl_node_str(&doc, "tag_name");
    mpv_node *assets = node_map_get(&doc, "assets");
    if (!tag || !valid_tag(tag) || !assets || assets->format != MPV_FORMAT_NODE_ARRAY) {
        nanum_dl_fail(dl, "The latest %s release has an unexpected format.", KNOWN[i].id);
        r = NANUM_DL_FAIL;
        goto out;
    }
    memset(rel, 0, sizeof(*rel));
    snprintf(rel->tag, sizeof(rel->tag), "%s", tag);

    char *sums_name = KNOWN[i].sums ? talloc_strdup(tmp, KNOWN[i].sums)
                                    : talloc_asprintf(tmp, "%s.sha256sum", KNOWN[i].asset);
    mpv_node *asset = find_asset(assets, KNOWN[i].asset);
    mpv_node *sumsa = find_asset(assets, sums_name);
    const char *url = asset ? nanum_dl_node_str(asset, "browser_download_url") : NULL;
    const char *surl = sumsa ? nanum_dl_node_str(sumsa, "browser_download_url") : NULL;
    int64_t size = 0;
    if (!asset || !sumsa || !url || !surl || !nanum_dl_node_int(asset, "size", &size) ||
        size <= 0 || strlen(url) >= NANUM_DL_MAX_URL)
    {
        nanum_dl_fail(dl, "The %s release %s has no %s with a checksum (%s).", KNOWN[i].id,
                      tag, KNOWN[i].asset, sums_name);
        r = NANUM_DL_FAIL;
        goto out;
    }
    if ((r = nanum_dl_get_text(dl, surl, MAX_SUMS_BYTES, tmp, &sums)) != NANUM_DL_OK)
        goto out;
    struct nanum_dl_item *it = &rel->item;
    if (!find_sha(sums, KNOWN[i].sums ? KNOWN[i].asset : NULL, it->sha)) {
        nanum_dl_fail(dl, "The checksum file %s of the %s release %s has no SHA-256 for %s.",
                      sums_name, KNOWN[i].id, tag, KNOWN[i].asset);
        r = NANUM_DL_FAIL;
        goto out;
    }
    it->valid = true;
    it->size = size;
    snprintf(it->label, sizeof(it->label), "%s", KNOWN[i].label);
    snprintf(it->file, sizeof(it->file), "%s", KNOWN[i].asset);
    snprintf(it->url, sizeof(it->url), "%s", url);
    mp_mutex_lock(&g_lock);
    g.asset_size[i] = size;
    mp_mutex_unlock(&g_lock);
    r = NANUM_DL_OK;
out:
    talloc_free(tmp);
    return r;
}

// ---------------------------------------------------------------------------
// Zip (the Deno release): one stored/deflated entry, inflated with zlib
// ---------------------------------------------------------------------------

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

static bool read_at(HANDLE f, int64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li = {.QuadPart = off};
    DWORD got = 0;
    return SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) &&
           got == n;
}

// Writes entry `name` of the zip into `out` (created exclusively), verifying
// size and CRC-32. Worker thread; reports ST_EXTRACTING.
static enum nanum_dl_res unzip_entry(struct nanum_dl *dl, int comp, const char *zip,
                                     const char *name, const char *out)
{
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r = NANUM_DL_FAIL;
    HANDLE in = INVALID_HANDLE_VALUE, of = INVALID_HANDLE_VALUE;
    bool zinit = false;
    z_stream z = {0};
    uint8_t *obuf = NULL;

    in = CreateFileW(W(tmp, zip), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER zsize;
    if (in == INVALID_HANDLE_VALUE || !GetFileSizeEx(in, &zsize) || zsize.QuadPart < 22) {
        nanum_dl_fail(dl, "Cannot read %s.", zip);
        goto out;
    }
    // End of central directory (no zip64: the entry is far below 4 GiB).
    uint8_t tail[1024 + 22];
    DWORD tn = (DWORD)MPMIN((int64_t)sizeof(tail), zsize.QuadPart);
    int64_t tail_off = zsize.QuadPart - tn;
    if (!read_at(in, tail_off, tail, tn)) {
        nanum_dl_fail(dl, "Cannot read %s.", zip);
        goto out;
    }
    int e = (int)tn - 22;
    while (e >= 0 && le32(tail + e) != 0x06054b50)
        e--;
    if (e < 0) {
        nanum_dl_fail(dl, "%s is not a zip file.", zip);
        goto out;
    }
    uint32_t cd_size = le32(tail + e + 12), cd_off = le32(tail + e + 16);
    if (cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu || cd_size > (1u << 20) ||
        (int64_t)cd_off + cd_size > zsize.QuadPart)
    {
        nanum_dl_fail(dl, "The zip directory of %s is unsupported.", zip);
        goto out;
    }
    uint8_t *cd = talloc_size(tmp, cd_size + 1);
    if (!read_at(in, cd_off, cd, cd_size)) {
        nanum_dl_fail(dl, "Cannot read %s.", zip);
        goto out;
    }
    uint32_t method = 0, crc = 0, csize = 0, usize = 0, lho = 0;
    bool found = false;
    for (uint32_t o = 0; o + 46 <= cd_size && le32(cd + o) == 0x02014b50;) {
        uint32_t nl = le16(cd + o + 28), xl = le16(cd + o + 30), cl = le16(cd + o + 32);
        if (o + 46 + nl > cd_size)
            break;
        if (nl == strlen(name) && !memcmp(cd + o + 46, name, nl)) {
            method = le16(cd + o + 10);
            crc = le32(cd + o + 16);
            csize = le32(cd + o + 20);
            usize = le32(cd + o + 24);
            lho = le32(cd + o + 42);
            found = true;
            break;
        }
        o += 46 + nl + xl + cl;
    }
    uint8_t lh[30];
    if (!found || (method != 0 && method != 8) || csize == 0xFFFFFFFFu ||
        usize == 0xFFFFFFFFu || !read_at(in, lho, lh, sizeof(lh)) || le32(lh) != 0x04034b50)
    {
        nanum_dl_fail(dl, "%s has no usable %s.", zip, name);
        goto out;
    }
    int64_t data = (int64_t)lho + 30 + le16(lh + 26) + le16(lh + 28);
    if (data + csize > zsize.QuadPart) {
        nanum_dl_fail(dl, "%s is truncated.", zip);
        goto out;
    }
    if (!nanum_dl_prepare_dir(dl, out, usize, name))
        goto out;
    of = CreateFileW(W(tmp, out), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (of == INVALID_HANDLE_VALUE) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        nanum_dl_fail(dl, "Cannot create %s: %s", out, why);
        goto out;
    }
    obuf = talloc_size(tmp, NANUM_DL_IO_BUF);
    if (method == 8) {
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
            nanum_dl_fail(dl, "zlib failed to start.");
            goto out;
        }
        zinit = true;
    }

    set_state(ST_EXTRACTING, comp, 0, usize, NULL);
    uint32_t done_in = 0;
    uint64_t total_out = 0;
    uint32_t sum = crc32(0L, Z_NULL, 0);
    bool end = false;
    while (!end) {
        if (nanum_dl_cancelled(dl)) {
            r = NANUM_DL_CANCEL;
            goto out;
        }
        DWORD want = 0;
        size_t produced;
        if (method == 0) {
            want = (DWORD)MPMIN((uint32_t)NANUM_DL_IO_BUF, csize - done_in);
            if (!want)
                break;
            if (!read_at(in, data + done_in, obuf, want)) {
                nanum_dl_fail(dl, "Cannot read %s.", zip);
                goto out;
            }
            done_in += want;
            produced = want;
        } else {
            if (!z.avail_in) {
                want = (DWORD)MPMIN((uint32_t)NANUM_DL_IO_BUF, csize - done_in);
                if (!want) {
                    nanum_dl_fail(dl, "%s is corrupt (unexpected end of %s).", zip, name);
                    goto out;
                }
                if (!read_at(in, data + done_in, dl->buf, want)) {
                    nanum_dl_fail(dl, "Cannot read %s.", zip);
                    goto out;
                }
                done_in += want;
                z.next_in = dl->buf;
                z.avail_in = want;
            }
            z.next_out = obuf;
            z.avail_out = NANUM_DL_IO_BUF;
            int zr = inflate(&z, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) {
                nanum_dl_fail(dl, "%s is corrupt (zlib error %d in %s).", zip, zr, name);
                goto out;
            }
            end = zr == Z_STREAM_END;
            produced = NANUM_DL_IO_BUF - z.avail_out;
        }
        if (produced) {
            DWORD written = 0;
            sum = crc32(sum, obuf, (uInt)produced);
            total_out += produced;
            if (total_out > usize || !WriteFile(of, obuf, (DWORD)produced, &written, NULL) ||
                written != produced)
            {
                char why[1024];
                nanum_dl_error_text(why, sizeof(why), GetLastError());
                nanum_dl_fail(dl, total_out > usize ? "%s: %s is larger than announced."
                                                    : "Cannot write %s: %s",
                              total_out > usize ? zip : out, total_out > usize ? name : why);
                goto out;
            }
            set_progress((int64_t)total_out, false);
        }
    }
    if (total_out != usize || sum != crc) {
        nanum_dl_fail(dl, "%s from %s failed the size/CRC check; nothing was installed.", name,
                      zip);
        goto out;
    }
    if (!FlushFileBuffers(of)) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        nanum_dl_fail(dl, "Cannot write %s: %s", out, why);
        goto out;
    }
    r = NANUM_DL_OK;
out:
    if (zinit)
        inflateEnd(&z);
    if (in != INVALID_HANDLE_VALUE)
        CloseHandle(in);
    if (of != INVALID_HANDLE_VALUE)
        CloseHandle(of);
    if (r != NANUM_DL_OK)
        DeleteFileW(W(tmp, out));
    talloc_free(tmp);
    return r;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

static enum nanum_dl_res install_ytdlp(const struct rel *rel)
{
    struct nanum_dl *dl = &g.dl;
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r;
    char *path = comp_path(tmp, C_YTDLP);
    dl->tag = C_YTDLP;
    set_state(ST_CHECKING, C_YTDLP, 0, rel->item.size, NULL);

    struct nanum_dl_fst f;
    bool exists = nanum_dl_stat(path, &f);
    if (exists && f.size == rel->item.size) {
        bool match = false;
        if ((r = nanum_dl_hash(dl, &rel->item, path, &match)) != NANUM_DL_OK)
            goto out;
        if (match) {
            mp_info(g.log, "yt-dlp %s is already installed and verified\n", rel->tag);
            write_stamp(C_YTDLP, rel->tag);
            r = NANUM_DL_OK;
            goto out;
        }
    }
    if (!nanum_dl_prepare_dir(dl, path, rel->item.size, rel->item.file)) {
        r = NANUM_DL_FAIL;
        goto out;
    }
    char *part = talloc_asprintf(tmp, "%s.%lu.part", path, (unsigned long)GetCurrentProcessId());
    mp_info(g.log, "downloading yt-dlp %s\n", rel->tag);
    r = nanum_dl_fetch(dl, &rel->item, part);
    if (r == NANUM_DL_OK)
        r = nanum_dl_install(dl, &rel->item, path, part, &f);
    if (r != NANUM_DL_OK)
        DeleteFileW(W(tmp, part));
    else
        write_stamp(C_YTDLP, rel->tag);
out:
    talloc_free(tmp);
    return r;
}

static enum nanum_dl_res install_deno(const struct rel *rel)
{
    struct nanum_dl *dl = &g.dl;
    void *tmp = talloc_new(NULL);
    enum nanum_dl_res r;
    char *path = comp_path(tmp, C_DENO);
    dl->tag = C_DENO;
    set_state(ST_CHECKING, C_DENO, 0, rel->item.size, NULL);

    // The zip needs room next to the extracted runtime (~3x its size).
    if (!nanum_dl_prepare_dir(dl, path, rel->item.size * 4, rel->item.file)) {
        r = NANUM_DL_FAIL;
        goto out;
    }
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    char *zip = talloc_asprintf(tmp, "%s.%lu.zip.part", path, pid);
    char *exe = talloc_asprintf(tmp, "%s.%lu.part", path, pid);
    mp_info(g.log, "downloading deno %s\n", rel->tag);
    r = nanum_dl_fetch(dl, &rel->item, zip);
    if (r == NANUM_DL_OK)
        r = unzip_entry(dl, C_DENO, zip, "deno.exe", exe);
    DeleteFileW(W(tmp, zip));
    if (r != NANUM_DL_OK)
        goto out;
    if (!MoveFileExW(W(tmp, exe), W(tmp, path), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        char why[1024];
        nanum_dl_error_text(why, sizeof(why), GetLastError());
        nanum_dl_fail(dl, "Cannot install %s: %s", path, why);
        DeleteFileW(W(tmp, exe));
        r = NANUM_DL_FAIL;
        goto out;
    }
    write_stamp(C_DENO, rel->tag);
out:
    talloc_free(tmp);
    return r;
}

static MP_THREAD_VOID worker_main(void *p)
{
    mp_thread_set_name("nanum/youtube");
    enum mode mode = g.mode;

    enum nanum_dl_res r;
    int failed = -1;
    void *tmp = talloc_new(NULL);
    struct rel rel;

    r = get_release(C_YTDLP, &rel);
    if (r == NANUM_DL_OK) {
        mp_mutex_lock(&g_lock);
        snprintf(g.latest, sizeof(g.latest), "%s", rel.tag);
        mp_mutex_unlock(&g_lock);
        request_publish();
        if (mode == MODE_INSTALL)
            r = install_ytdlp(&rel);
    }
    if (r == NANUM_DL_FAIL)
        failed = C_YTDLP;
    if (mode == MODE_INSTALL && r == NANUM_DL_OK && !installed(C_DENO)) {
        r = get_release(C_DENO, &rel);
        if (r == NANUM_DL_OK)
            r = install_deno(&rel);
        if (r == NANUM_DL_FAIL)
            failed = C_DENO;
    }
    if (r == NANUM_DL_OK && nanum_dl_cancelled(&g.dl))
        r = NANUM_DL_CANCEL;

    mp_mutex_lock(&g_lock);
    g.st = r == NANUM_DL_OK ? ST_DONE : r == NANUM_DL_CANCEL ? ST_CANCELLED : ST_ERROR;
    g.cur = failed;
    if (r != NANUM_DL_FAIL)
        g.recv = g.total = 0;
    snprintf(g.err, sizeof(g.err), "%s", r == NANUM_DL_FAIL ? g.dl.err : "");
    g.running = false;
    mp_mutex_unlock(&g_lock);
    if (r == NANUM_DL_FAIL)
        mp_err(g.log, "YouTube component %s failed: %s\n", mode == MODE_CHECK ? "check" : "download",
               g.dl.err);
    else
        mp_info(g.log, "YouTube component %s %s\n", mode == MODE_CHECK ? "check" : "download",
                r == NANUM_DL_OK ? "finished" : "cancelled");
    request_publish();

    nanum_dl_end(&g.dl);
    talloc_free(tmp);
    MP_THREAD_RETURN();
}

// ---------------------------------------------------------------------------
// Core thread: state, commands
// ---------------------------------------------------------------------------

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
    char err[sizeof(g.err)], latest[sizeof(g.latest)];
    memcpy(err, g.err, sizeof(err));
    memcpy(latest, g.latest, sizeof(latest));
    int64_t asize[C_COUNT];
    memcpy(asize, g.asset_size, sizeof(asize));
    mp_mutex_unlock(&g_lock);

    bool inst[C_COUNT];
    char ver[C_COUNT][64];
    for (int i = 0; i < C_COUNT; i++) {
        inst[i] = installed(i);
        installed_version(i, ver[i]);
    }

    mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_string(&root, "status", STATE_NAME[st]);
    node_map_add_string(&root, "current", cur >= 0 && cur < C_COUNT ? KNOWN[cur].id : "");
    node_map_add_int64(&root, "received", recv);
    node_map_add_int64(&root, "total", total);
    node_map_add_string(&root, "error", err);
    node_map_add_string(&root, "directory", g.dir ? g.dir : "");
    node_map_add_string(&root, "bin", g.bin ? g.bin : "");
    node_map_add_flag(&root, "busy", running);
    node_map_add_flag(&root, "installed", inst[C_YTDLP] && inst[C_DENO]);
    node_map_add_string(&root, "version", ver[C_YTDLP]);
    node_map_add_string(&root, "latest", latest);
    node_map_add_flag(&root, "update_available",
                      inst[C_YTDLP] && latest[0] && strcmp(ver[C_YTDLP], latest));
    char *yp = comp_path(tmp, C_YTDLP), *dp = comp_path(tmp, C_DENO);
    node_map_add_string(&root, "ytdl_path", yp ? yp : "");
    node_map_add_string(&root, "js_runtime_path", dp ? dp : "");

    mpv_node *arr = node_map_add(&root, "components", MPV_FORMAT_NODE_ARRAY);
    for (int i = 0; i < C_COUNT; i++) {
        char *p = comp_path(tmp, i);
        struct nanum_dl_fst f;
        bool have = p && nanum_dl_stat(p, &f);
        mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
        node_map_add_string(m, "id", KNOWN[i].id);
        node_map_add_string(m, "label", KNOWN[i].label);
        node_map_add_string(m, "filename", KNOWN[i].file);
        node_map_add_int64(m, "size", inst[i] && have ? f.size : asize[i]);
        node_map_add_flag(m, "installed", inst[i]);
        node_map_add_string(m, "version", ver[i]);
        node_map_add_string(m, "path", p ? p : "");
    }

    mp_property_do("user-data/nanum/youtube", M_PROPERTY_SET_NODE, &root, mpctx);
    talloc_free(root.u.list);
    talloc_free(tmp);
}

char *nanum_youtube_startup(struct MPContext *mpctx, void *ctx)
{
    g.mpctx = mpctx;
    g.log = mpctx->log;
    g.cur = -1;
    nanum_dl_init(&g.dl, &g, youtube_phase, youtube_progress);

    wchar_t env[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"NANUMPLAYER_YOUTUBE_DIR", env, MP_ARRAY_SIZE(env));
    if (n > 0 && n < MP_ARRAY_SIZE(env)) {
        g.dir = mp_to_utf8(NULL, env);
        nanum_dl_trim_slashes(g.dir);
    } else {
        wchar_t *local = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) {
            char *base = mp_to_utf8(NULL, local);
            g.dir = talloc_asprintf(NULL, "%s\\NanumPlayer\\youtube", base);
            talloc_free(base);
            CoTaskMemFree(local);
        }
    }
    if (g.dir)
        g.bin = nanum_dl_join(NULL, g.dir, "bin");
    g.init = true;
    char *p = comp_path(NULL, C_YTDLP);
    char *r = p ? talloc_strdup(ctx, p) : NULL;
    talloc_free(p);
    return r;
}

void nanum_youtube_publish(struct MPContext *mpctx)
{
    if (!g.init) {
        MP_ERR(mpctx, "The YouTube component manager is not initialized.\n");
        return;
    }
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

static bool start_job(struct MPContext *mpctx, enum mode mode)
{
    if (!g.init)
        return reject(mpctx, "The YouTube component manager is not initialized.");
    mp_mutex_lock(&g_lock);
    bool down = g.shutting_down, busy = g.running;
    mp_mutex_unlock(&g_lock);
    if (down)
        return false;
    if (busy) {
        MP_WARN(mpctx, "A YouTube component job is already running; cancel it first.\n");
        return false;
    }
    if (g.thread_valid) {
        mp_thread_join(g.thread);
        g.thread_valid = false;
    }
    if (!g.bin)
        return reject(mpctx, "The YouTube component folder cannot be determined "
                      "(%%LOCALAPPDATA%% is unavailable).");
    if (!nanum_dl_begin(&g.dl))
        return reject(mpctx, "Out of memory.");
    mp_mutex_lock(&g_lock);
    g.running = true;
    g.mode = mode;
    g.st = ST_CHECKING;
    g.cur = C_YTDLP;
    g.recv = g.total = 0;
    g.err[0] = 0;
    g.last_pub = GetTickCount64();
    mp_mutex_unlock(&g_lock);

    if (mp_thread_create(&g.thread, worker_main, NULL)) {
        mp_mutex_lock(&g_lock);
        g.running = false;
        mp_mutex_unlock(&g_lock);
        nanum_dl_end(&g.dl);
        return reject(mpctx, "Cannot start the YouTube component thread.");
    }
    g.thread_valid = true;
    publish(mpctx);
    return true;
}

void nanum_youtube_refresh(struct MPContext *mpctx)
{
    if (!start_job(mpctx, MODE_CHECK))
        publish(mpctx);
}

bool nanum_youtube_download(struct MPContext *mpctx)
{
    return start_job(mpctx, MODE_INSTALL);
}

void nanum_youtube_cancel(struct MPContext *mpctx)
{
    if (!g.init)
        return;
    mp_mutex_lock(&g_lock);
    bool running = g.running;
    mp_mutex_unlock(&g_lock);
    if (running)
        nanum_dl_abort(&g.dl);
}

void nanum_youtube_shutdown(struct MPContext *mpctx)
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
