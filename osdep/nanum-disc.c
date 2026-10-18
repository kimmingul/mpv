/*
 * NanumPlayer: native disc listing and lossless MKV remux. See nanum-disc.h.
 *
 * No external program is involved. A disc is opened with the player's own
 * stack: libbluray (AACS/BD+ through nanum_disc_bd_open()) or libdvdread /
 * libdvdnav (+libdvdcss), titles are read through the regular "disc" demuxer
 * (demux_disc.c), so the multi-clip timeline, the TrueHD+AC-3 core pair, PGS
 * subtitles, track languages/names/flags and chapters are the ones playback
 * shows. The packets are copied unchanged into a Matroska file by libavformat
 * (no re-encoding).
 *
 * ---------------------------------------------------------------------------
 * Command results (a map; every field is always present unless noted)
 *
 *  nanum-disc-list [device]
 *    ok, status ("done"|"error"|"cancelled"), error, error_code, device
 *    drives[]   (device == "")  {device:"E:", drive_type:"cdrom", has_media,
 *                                label, kind:"bluray"|"dvd"|"unknown"|"none"}
 *    disc       (device given, ok) {device, kind:"bluray"|"dvd", id,
 *                volume_id, label, protection[] ("aacs","bdplus","css"),
 *                titles[] {id, playlist (-1 for DVD), name, duration (s),
 *                size (source bytes, estimate), chapters, angles, clips,
 *                video, audio, subtitle}}
 *    "id" is the libbluray disc id (40 hex digits) or, when the disc has none
 *    (unencrypted), "vol:<volume id>"; for DVD the 32 digit libdvdread disc id.
 *
 *  nanum-disc-rip <device> <title-id|all> <destination> [expected-disc-id]
 *    ok, status, error, error_code, device, disc_id, disc_label, destination,
 *    requested, failed_id (-1 if none),
 *    outputs[] {id, playlist, file, size, duration (s, muxed), chapters,
 *               streams, timestamp_fixes, dropped_packets}  (completed only)
 *
 *  error_code: bad-args, busy, bad-device, no-media, not-a-disc,
 *              unsupported-protected, component-missing (copy protected and the
 *              optional decryption component is not installed, see
 *              nanum-disc-component.h), dvd-unavailable, disc-mismatch,
 *              bad-title, bad-destination, io, mux, cancelled,
 *              internal
 *
 * Progress, property user-data/nanum/rip (published while a disc operation
 * runs and kept after it ended):
 *    op ("idle"|"list"|"rip"), status ("idle"|"scanning"|"ripping"|
 *    "finalizing"|"done"|"cancelled"|"error"), busy, device, disc_id,
 *    disc_label, destination, title_count, title_index (1-based), title_id,
 *    playlist, file, title_progress, progress (0..1), bytes_done, bytes_total
 *    (source size estimates), elapsed (s), error, error_code,
 *    completed[] {id, file, size}
 * ---------------------------------------------------------------------------
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include "config.h"

#include "mpv/client.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "player/command.h"
#include "osdep/nanum-disc.h"
#include "osdep/nanum-disc-component.h"

#ifdef _WIN32

#include <windows.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "common/av_common.h"
#include "common/global.h"
#include "common/msg.h"
#include "common/tags.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "misc/dispatch.h"
#include "misc/node.h"
#include "misc/thread_tools.h"
#include "options/m_config.h"
#include "options/m_property.h"
#include "options/options.h"
#include "player/core.h"
#include "stream/stream.h"
#include "osdep/io.h"
#include "osdep/threads.h"

#if HAVE_LIBBLURAY
#include <libbluray/bluray.h>
#include <libbluray/meta_data.h>
#include "osdep/nanum-disc-keys.h"
#endif

#include "nanum/i18n.h"

#define NANUM_DISC_DVD (HAVE_DVDNAV && HAVE_DVDA)
#if NANUM_DISC_DVD
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_types.h>
#endif

// Titles shorter than this are not offered (menus, trailers, logos).
#define MIN_TITLE_SECONDS 120
// Progress is published at most this often.
#define PROGRESS_MS 200
// Packets/bytes held back until every audio/video stream has delivered its
// first packet (needed to pick the time origin and write the header).
#define PREBUF_MAX_PACKETS 4096
#define PREBUF_MAX_BYTES ((size_t)256 << 20)

// ---------------------------------------------------------------------------
// Errors and small helpers
// ---------------------------------------------------------------------------

struct err {
    char code[32];
    char msg[640];
};

static bool fail(struct err *e, const char *code, const char *fmt, ...) MP_PRINTF_ATTRIBUTE(3, 4);
static bool fail(struct err *e, const char *code, const char *fmt, ...)
{
    snprintf(e->code, sizeof(e->code), "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), nanum_tr(fmt), ap);
    va_end(ap);
    return false;
}

static void copy_str(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src ? src : "");
}

static wchar_t *W(void *ctx, const char *s)
{
    return mp_from_utf8(ctx, s);
}

// "E:", "E:\", "e:/" -> "E:". Anything else (folder, image file) is kept.
static bool is_drive_device(const char *s, char *letter)
{
    if (!s || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) || s[1] != ':')
        return false;
    if (s[2] && !((s[2] == '\\' || s[2] == '/') && !s[3]))
        return false;
    if (letter)
        *letter = (char)((s[0] >= 'a' && s[0] <= 'z') ? s[0] - 32 : s[0]);
    return true;
}

static void normalize_device(char *dst, size_t size, const char *src)
{
    char l;
    if (is_drive_device(src, &l)) {
        snprintf(dst, size, "%c:", l);
        return;
    }
    copy_str(dst, size, src);
    size_t n = strlen(dst);
    while (n > 3 && (dst[n - 1] == '\\' || dst[n - 1] == '/'))
        dst[--n] = 0;
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

static bool path_is_dir(void *ctx, const char *path)
{
    DWORD a;
    return path_attr(ctx, path, &a) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool path_exists_or_error(void *ctx, const char *path)
{
    wchar_t *w = W(ctx, path);
    if (GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES)
        return true;
    DWORD e = GetLastError();
    return e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
}

static char *join_path(void *ctx, const char *a, const char *b)
{
    size_t n = strlen(a);
    bool sep = n && (a[n - 1] == '\\' || a[n - 1] == '/');
    return talloc_asprintf(ctx, "%s%s%s", a, sep ? "" : "\\", b);
}

static bool cancelled(struct mp_cancel *c)
{
    return c && mp_cancel_test(c);
}

static void hex(char *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++)
        snprintf(dst + i * 2, 3, "%02x", src[i]);
}

// Volume serial/label of a drive-letter device; used to notice swapped media.
struct vol_id {
    bool valid;
    DWORD serial;
    char label[128];
};

static bool read_volume(void *ctx, const char *device, struct vol_id *v)
{
    *v = (struct vol_id){0};
    char l;
    if (!is_drive_device(device, &l))
        return false;
    wchar_t root[4] = {(wchar_t)l, L':', L'\\', 0};
    wchar_t label[128] = {0};
    DWORD serial = 0;
    DWORD old = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old);
    BOOL ok = GetVolumeInformationW(root, label, MP_ARRAY_SIZE(label), &serial, NULL, NULL,
                                    NULL, 0);
    SetThreadErrorMode(old, NULL);
    if (!ok)
        return false;
    char *u = mp_to_utf8(ctx, label);
    v->valid = true;
    v->serial = serial;
    copy_str(v->label, sizeof(v->label), u);
    return true;
}

// ---------------------------------------------------------------------------
// Disc model
// ---------------------------------------------------------------------------

enum kind { K_NONE, K_BD, K_DVD };

struct title {
    int id;                 // stable index of this listing (what rip takes)
    int playlist;           // mpls number, -1 for DVD
    char name[48];
    double duration;        // seconds
    int64_t size;           // source bytes (estimate)
    int chapters, angles, clips;
    int video, audio, subtitle;
    int dvd_vts;            // DVD only: title set number
    uint32_t dvd_first_sector;
    double dvd_span_max;    // DVD only: longest reading of the IFO playback time (s)
};

struct disc {
    enum kind kind;
    char device[1024];
    char id[96];
    char volume_id[160];
    char label[256];
    bool aacs, bdplus, css;
    struct vol_id vol;
    struct title *titles;
    int num_titles;
};

static const char *kind_name(enum kind k)
{
    return k == K_BD ? "bluray" : k == K_DVD ? "dvd" : "unknown";
}

// ---------------------------------------------------------------------------
// Published progress
// ---------------------------------------------------------------------------

struct done_item {
    int id;
    char file[1024];
    int64_t size;
};

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;

static struct {
    struct MPContext *mpctx;
    bool busy;
    bool published;                 // something was ever published
    // Published state (guarded by g_lock).
    const char *op;                 // "idle" | "list" | "rip"
    const char *status;
    char device[1024], disc_id[96], disc_label[256], destination[1024];
    int title_count, title_index, title_id, playlist;
    char file[1024];
    double title_progress, progress;
    int64_t bytes_done, bytes_total;
    ULONGLONG t0;
    double elapsed_final;           // < 0 while running
    char error[640], error_code[32];
    struct done_item *done;
    int num_done;
    ULONGLONG last_pub;
} g = {.op = "idle", .status = "idle", .elapsed_final = -1};

static void publish(struct MPContext *mpctx)
{
    if (!mpctx)
        return;
    mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);

    mp_mutex_lock(&g_lock);
    node_map_add_string(&root, "op", g.op);
    node_map_add_string(&root, "status", g.status);
    node_map_add_flag(&root, "busy", g.busy);
    node_map_add_string(&root, "device", g.device);
    node_map_add_string(&root, "disc_id", g.disc_id);
    node_map_add_string(&root, "disc_label", g.disc_label);
    node_map_add_string(&root, "destination", g.destination);
    node_map_add_int64(&root, "title_count", g.title_count);
    node_map_add_int64(&root, "title_index", g.title_index);
    node_map_add_int64(&root, "title_id", g.title_id);
    node_map_add_int64(&root, "playlist", g.playlist);
    node_map_add_string(&root, "file", g.file);
    node_map_add_double(&root, "title_progress", g.title_progress);
    node_map_add_double(&root, "progress", g.progress);
    node_map_add_int64(&root, "bytes_done", g.bytes_done);
    node_map_add_int64(&root, "bytes_total", g.bytes_total);
    double el = g.elapsed_final >= 0 ? g.elapsed_final
                                     : g.t0 ? (GetTickCount64() - g.t0) / 1000.0 : 0;
    node_map_add_double(&root, "elapsed", el);
    node_map_add_string(&root, "error", g.error);
    node_map_add_string(&root, "error_code", g.error_code);
    mpv_node *arr = node_map_add(&root, "completed", MPV_FORMAT_NODE_ARRAY);
    for (int i = 0; i < g.num_done; i++) {
        mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
        node_map_add_int64(m, "id", g.done[i].id);
        node_map_add_string(m, "file", g.done[i].file);
        node_map_add_int64(m, "size", g.done[i].size);
    }
    g.published = true;
    mp_mutex_unlock(&g_lock);

    mp_property_do("user-data/nanum/rip", M_PROPERTY_SET_NODE, &root, mpctx);
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

// Claims the single disc operation slot. Called on the core thread.
static bool begin_op(struct MPContext *mpctx, const char *op, const char *status)
{
    mp_mutex_lock(&g_lock);
    if (g.busy) {
        mp_mutex_unlock(&g_lock);
        return false;
    }
    g.num_done = 0;
    talloc_free(g.done);
    g.done = NULL;
    g.mpctx = mpctx;
    g.busy = true;
    g.op = op;
    g.status = status;
    g.device[0] = g.disc_id[0] = g.disc_label[0] = g.destination[0] = 0;
    g.file[0] = 0;
    g.title_count = g.title_index = 0;
    g.title_id = g.playlist = -1;
    g.title_progress = g.progress = 0;
    g.bytes_done = g.bytes_total = 0;
    g.t0 = GetTickCount64();
    g.elapsed_final = -1;
    g.error[0] = g.error_code[0] = 0;
    g.last_pub = 0;
    mp_mutex_unlock(&g_lock);
    return true;
}

// Core thread, core locked: publishes the final state and releases the slot.
static void end_op(struct MPContext *mpctx, const char *status, const struct err *e)
{
    mp_mutex_lock(&g_lock);
    g.status = status;
    g.busy = false;
    g.elapsed_final = (GetTickCount64() - g.t0) / 1000.0;
    if (e) {
        copy_str(g.error, sizeof(g.error), e->msg);
        copy_str(g.error_code, sizeof(g.error_code), e->code);
    }
    mp_mutex_unlock(&g_lock);
    // Drop a queued publication: the final state is published right here and
    // the dispatch queue must be empty when the core is destroyed.
    mp_dispatch_cancel_fn(mpctx->dispatch, publish_cb, &g);
    publish(mpctx);
}

static void set_disc(const struct disc *d)
{
    mp_mutex_lock(&g_lock);
    copy_str(g.device, sizeof(g.device), d->device);
    copy_str(g.disc_id, sizeof(g.disc_id), d->id);
    copy_str(g.disc_label, sizeof(g.disc_label), d->label);
    mp_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// Optical drives
// ---------------------------------------------------------------------------

struct drive {
    char device[4];
    bool has_media;
    char label[128];
    enum kind kind;
};

static enum kind detect_kind(void *ctx, const char *device)
{
    DWORD a;
    char *p = join_path(ctx, device, "BDMV");
    if (path_attr(ctx, p, &a) && (a & FILE_ATTRIBUTE_DIRECTORY))
        return K_BD;
    p = join_path(ctx, device, "VIDEO_TS");
    if (path_attr(ctx, p, &a) && (a & FILE_ATTRIBUTE_DIRECTORY))
        return K_DVD;
    return K_NONE;
}

static int enumerate_drives(void *ctx, struct drive **out)
{
    DWORD mask = GetLogicalDrives();
    struct drive *drives = NULL;
    int n = 0;
    DWORD old = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old);
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i)))
            continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_CDROM)
            continue;
        struct drive d = {0};
        snprintf(d.device, sizeof(d.device), "%c:", 'A' + i);
        struct vol_id v;
        d.has_media = read_volume(ctx, d.device, &v);
        if (d.has_media) {
            copy_str(d.label, sizeof(d.label), v.label);
            d.kind = detect_kind(ctx, d.device);
        }
        MP_TARRAY_APPEND(ctx, drives, n, d);
    }
    SetThreadErrorMode(old, NULL);
    *out = drives;
    return n;
}

// ---------------------------------------------------------------------------
// Blu-ray enumeration
// ---------------------------------------------------------------------------

#if HAVE_LIBBLURAY

static bool enumerate_bd(void *ctx, struct mp_log *log, struct mp_cancel *cancel,
                         const char *device, struct disc *d, struct err *e)
{
    BLURAY *bd = nanum_disc_bd_open(device, log, cancel);
    if (!bd) {
        if (cancelled(cancel))
            return fail(e, "cancelled", N_("Cancelled."));
        char *aacs = join_path(ctx, device, "AACS");
        if (path_is_dir(ctx, aacs) && !nanum_disc_component_ready(log))
            return fail(e, "component-missing",
                        N_("This Blu-ray is copy protected. The optional Blu-ray/DVD "
                        "decryption component is not installed (Ctrl+N > Blu-ray/DVD "
                        "decryption components)."));
        if (path_is_dir(ctx, aacs))
            return fail(e, "unsupported-protected",
                        N_("This Blu-ray is AACS protected and could not be decrypted with "
                        "the built-in AACS support (the disc is not supported or its key "
                        "could not be obtained). See the log for details."));
        return fail(e, "io", N_("The Blu-ray could not be opened (see the log for details)."));
    }

    const BLURAY_DISC_INFO *info = bd_get_disc_info(bd);
    if (!info || !info->bluray_detected) {
        bd_close(bd);
        return fail(e, "not-a-disc", N_("%s is not a Blu-ray disc."), device);
    }
    // nanum_disc_bd_open() only returns a disc whose protection is handled;
    // verify instead of assuming.
    if ((info->aacs_detected && !info->aacs_handled) ||
        (info->bdplus_detected && !info->bdplus_handled))
    {
        bd_close(bd);
        return fail(e, "unsupported-protected",
                    N_("The disc's %s protection is not handled; it cannot be copied."),
                    info->aacs_detected && !info->aacs_handled ? "AACS" : "BD+");
    }
    d->aacs = info->aacs_detected;
    d->bdplus = info->bdplus_detected;

    bool any = false;
    uint8_t zero[20] = {0};
    any = memcmp(info->disc_id, zero, sizeof(zero)) != 0;
    copy_str(d->volume_id, sizeof(d->volume_id), info->udf_volume_id);
    if (any) {
        hex(d->id, info->disc_id, 20);
    } else if (d->volume_id[0]) {
        snprintf(d->id, sizeof(d->id), "vol:%s", d->volume_id);
    }

    const struct meta_dl *meta = bd_get_meta(bd);
    if (meta && meta->di_name && meta->di_name[0])
        copy_str(d->label, sizeof(d->label), meta->di_name);

    uint32_t n = bd_get_titles(bd, TITLES_RELEVANT, MIN_TITLE_SECONDS);
    for (uint32_t i = 0; i < n; i++) {
        if (cancelled(cancel)) {
            bd_close(bd);
            return fail(e, "cancelled", N_("Cancelled."));
        }
        BLURAY_TITLE_INFO *ti = bd_get_title_info(bd, i, 0);
        if (!ti)
            continue;
        struct title t = {
            .id = (int)i,
            .playlist = (int)ti->playlist,
            .duration = ti->duration / 90000.0,
            .chapters = (int)ti->chapter_count,
            .angles = ti->angle_count,
            .clips = (int)ti->clip_count,
        };
        snprintf(t.name, sizeof(t.name), "%05u.mpls", (unsigned)ti->playlist);
        for (uint32_t c = 0; c < ti->clip_count; c++)
            t.size += (int64_t)ti->clips[c].pkt_count * 192;
        if (ti->clip_count) {
            const BLURAY_CLIP_INFO *c0 = &ti->clips[0];
            t.video = c0->video_stream_count + c0->sec_video_stream_count;
            t.audio = c0->audio_stream_count + c0->sec_audio_stream_count;
            t.subtitle = c0->pg_stream_count;
        }
        bd_free_title_info(ti);
        MP_TARRAY_APPEND(d, d->titles, d->num_titles, t);
    }
    bd_close(bd);
    d->kind = K_BD;
    return true;
}

#else

static bool enumerate_bd(void *ctx, struct mp_log *log, struct mp_cancel *cancel,
                         const char *device, struct disc *d, struct err *e)
{
    return fail(e, "unsupported-protected", N_("This build has no Blu-ray support."));
}

#endif

// ---------------------------------------------------------------------------
// DVD enumeration (libdvdread)
// ---------------------------------------------------------------------------

#if NANUM_DISC_DVD

static void dvd_log(void *priv, dvd_logger_level_t level, const char *fmt, va_list ap)
{
    struct mp_log *log = priv;
    int lev = level == DVD_LOGGER_LEVEL_ERROR ? MSGL_ERR : level == DVD_LOGGER_LEVEL_WARN
              ? MSGL_WARN : level == DVD_LOGGER_LEVEL_INFO ? MSGL_V : MSGL_DEBUG;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    mp_msg(log, lev, "libdvdread: %s\n", buf);
}

static int bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 15);
}

static int64_t dvd_time_ms(const dvd_time_t *t)
{
    int fps_code = (t->frame_u >> 6) & 3;
    int frames = bcd(t->frame_u & 0x3f);
    int64_t ms = ((int64_t)bcd(t->hour) * 3600 + bcd(t->minute) * 60 + bcd(t->second)) * 1000;
    if (fps_code == 1)
        ms += frames * 1000 / 25;
    else if (fps_code == 3)
        ms += frames * 1001 / 30;
    return ms;
}

// The IFO time of an NTSC cell is a frame count: some authoring tools (dvdauthor)
// count 30 frames per second, other tools real seconds plus 29.97 fps frames.
// dvd_time_ms() is the second reading; this is the first one, the longer of the two.
static int64_t dvd_time_ms_max(const dvd_time_t *t)
{
    int64_t ms = dvd_time_ms(t);
    if (((t->frame_u >> 6) & 3) == 3)
        ms += (int64_t)bcd(t->hour) * 3600 + bcd(t->minute) * 60 + bcd(t->second);
    return ms;
}

// True if the first block of the title's first cell is still CSS scrambled
// after libdvdread (and libdvdcss, if present) handled it.
static bool dvd_block_scrambled(dvd_reader_t *dvd, int vts, uint32_t sector, bool *read_ok)
{
    *read_ok = false;
    dvd_file_t *f = DVDOpenFile(dvd, vts, DVD_READ_TITLE_VOBS);
    if (!f)
        return false;
    unsigned char block[2048];
    ssize_t r = DVDReadBlocks(f, (int)sector, 1, block);
    DVDCloseFile(f);
    if (r != 1)
        return false;
    *read_ok = true;
    // MPEG-2 program stream pack header: bits 5..4 of byte 0x14 are the PES
    // scrambling control of the first packet.
    return block[0] == 0 && block[1] == 0 && block[2] == 1 && block[3] == 0xBA &&
           (block[0x14] & 0x30) != 0;
}

static bool enumerate_dvd(void *ctx, struct mp_log *log, struct mp_cancel *cancel,
                          const char *device, struct disc *d, struct err *e)
{
    dvd_logger_cb logger = {.pf_log = dvd_log};
    nanum_disc_component_ready(log);    // libdvdcss, if the optional component is installed
    dvd_reader_t *dvd = DVDOpen2(log, &logger, device);
    if (!dvd)
        return fail(e, "io", N_("The DVD could not be opened (see the log for details)."));

    bool ok = false;
    ifo_handle_t *vmg = ifoOpen(dvd, 0);
    if (!vmg || !vmg->tt_srpt) {
        fail(e, "not-a-disc", N_("%s is not a DVD-Video disc."), device);
        goto done;
    }

    unsigned char id[16];
    if (DVDDiscID(dvd, id) == 0) {
        hex(d->id, id, 16);
    } else {
        fail(e, "io", N_("The DVD disc id could not be read."));
        goto done;
    }
    char vol[33] = {0};
    unsigned char volset[128];
    if (DVDUDFVolumeInfo(dvd, vol, 32, volset, sizeof(volset)) < 0 &&
        DVDISOVolumeInfo(dvd, vol, 32, volset, sizeof(volset)) < 0)
        vol[0] = 0;
    copy_str(d->volume_id, sizeof(d->volume_id), vol);

    int num = vmg->tt_srpt->nr_of_srpts;
    for (int i = 0; i < num; i++) {
        if (cancelled(cancel)) {
            fail(e, "cancelled", N_("Cancelled."));
            goto done;
        }
        const title_info_t *ti = &vmg->tt_srpt->title[i];
        int vts_nr = ti->title_set_nr;
        if (!vts_nr || !ti->vts_ttn)
            continue;
        ifo_handle_t *vts = ifoOpen(dvd, vts_nr);
        if (!vts || !vts->vts_ptt_srpt || !vts->vts_pgcit ||
            ti->vts_ttn > vts->vts_ptt_srpt->nr_of_srpts)
        {
            if (vts)
                ifoClose(vts);
            continue;
        }
        const ttu_t *ttu = &vts->vts_ptt_srpt->title[ti->vts_ttn - 1];
        if (!ttu->nr_of_ptts) {
            ifoClose(vts);
            continue;
        }
        // The title plays every PGC its chapters point into, each from its first
        // chapter's program to the end of the PGC (the VM leaves a PGC at its end).
        int64_t ms = 0, ms_max = 0, sectors = 0;
        uint32_t first_sector = 0, audio_mask = 0, sub_mask = 0;
        bool have_first = false, bad = false;
        for (int p = 0; p < ttu->nr_of_ptts && !bad; p++) {
            int pgcn = ttu->ptt[p].pgcn, pgn = ttu->ptt[p].pgn;
            bool done = false;
            for (int q = 0; q < p && !done; q++)
                done = ttu->ptt[q].pgcn == pgcn;
            if (done)
                continue;
            for (int q = p + 1; q < ttu->nr_of_ptts; q++) {
                if (ttu->ptt[q].pgcn == pgcn && ttu->ptt[q].pgn >= 1 && ttu->ptt[q].pgn < pgn)
                    pgn = ttu->ptt[q].pgn;
            }
            if (pgcn < 1 || pgcn > vts->vts_pgcit->nr_of_pgci_srp) {
                bad = true;
                break;
            }
            const pgc_t *pgc = vts->vts_pgcit->pgci_srp[pgcn - 1].pgc;
            if (!pgc || pgn < 1 || pgn > pgc->nr_of_programs || !pgc->nr_of_cells) {
                bad = true;
                break;
            }
            for (int a = 0; a < 8; a++)
                audio_mask |= (pgc->audio_control[a] & 0x8000) ? 1u << a : 0;
            for (int s = 0; s < 32; s++)
                sub_mask |= (pgc->subp_control[s] & 0x80000000u) ? 1u << s : 0;
            int first_cell = pgc->program_map[pgn - 1] - 1;
            for (int c = first_cell; c < pgc->nr_of_cells; c++) {
                const cell_playback_t *cp = &pgc->cell_playback[c];
                // Of an angle block only the first angle plays.
                if (cp->block_type == 1 && cp->block_mode != 1 && cp->block_mode != 0) {
                    if (cp->block_mode == 2 || cp->block_mode == 3)
                        continue;
                }
                ms += dvd_time_ms(&cp->playback_time);
                ms_max += dvd_time_ms_max(&cp->playback_time);
                sectors += (int64_t)cp->last_sector - cp->first_sector + 1;
                if (!have_first) {
                    first_sector = cp->first_sector;
                    have_first = true;
                }
            }
        }
        if (bad) {
            ifoClose(vts);
            continue;
        }
        struct title t = {
            .id = i,
            .playlist = -1,
            .duration = ms / 1000.0,
            .dvd_span_max = ms_max / 1000.0,
            .size = sectors * 2048,
            .chapters = ti->nr_of_ptts,
            .angles = ti->nr_of_angles,
            .clips = 1,
            .video = 1,
            .dvd_vts = vts_nr,
            .dvd_first_sector = first_sector,
        };
        snprintf(t.name, sizeof(t.name), nanum_tr("Title %d"), i + 1);
        for (int a = 0; a < 8; a++)
            t.audio += (audio_mask >> a) & 1;
        for (int s = 0; s < 32; s++)
            t.subtitle += (sub_mask >> s) & 1;
        ifoClose(vts);
        if (t.duration < MIN_TITLE_SECONDS || !have_first)
            continue;

        bool read_ok;
        if (dvd_block_scrambled(dvd, vts_nr, first_sector, &read_ok)) {
            if (!nanum_disc_component_ready(log))
                fail(e, "component-missing",
                     N_("This DVD is CSS protected. The optional Blu-ray/DVD decryption "
                     "component is not installed (Ctrl+N > Blu-ray/DVD decryption "
                     "components)."));
            else
                fail(e, "unsupported-protected",
                     N_("This DVD is CSS protected and could not be decrypted (libdvdcss "
                     "is unavailable or failed)."));
            goto done;
        }
        if (!read_ok) {
            fail(e, "io", N_("DVD title %d could not be read (sector %u of title set %d)."),
                 i + 1, (unsigned)first_sector, vts_nr);
            goto done;
        }
        MP_TARRAY_APPEND(d, d->titles, d->num_titles, t);
    }
    d->kind = K_DVD;
    ok = true;
done:
    if (vmg)
        ifoClose(vmg);
    DVDClose(dvd);
    return ok;
}

// Does this DVD need the (not installed) decryption component? Used by playback
// (stream/stream_dvdnav.c) before libdvdnav opens the disc: without libdvdcss a CSS disc
// would only fail later with unreadable or garbled blocks. A disc is judged protected when
// the first audio/video packets of its first title set are scrambled, or cannot be read at all
// although the disc's IFO files can.
static bool block_scrambled(const unsigned char *b, bool *decided)
{
    *decided = false;
    if (b[0] || b[1] || b[2] != 1 || b[3] != 0xBA)
        return false;
    size_t p = 14 + (b[13] & 7);
    while (p + 7 <= 2048 && !b[p] && !b[p + 1] && b[p + 2] == 1) {
        unsigned id = b[p + 3];
        size_t len = ((size_t)b[p + 4] << 8) | b[p + 5];
        if (id == 0xBD || (id >= 0xC0 && id <= 0xEF)) {
            *decided = true;
            return (b[p + 6] & 0x30) != 0;
        }
        p += 6 + len;
    }
    return false;
}

bool nanum_disc_dvd_needs_component(struct mp_log *log, const char *device)
{
    if (nanum_disc_component_ready(log))
        return false;
    dvd_logger_cb logger = {.pf_log = dvd_log};
    dvd_reader_t *dvd = DVDOpen2(log, &logger, device);
    if (!dvd)
        return false;
    bool blocked = false;
    ifo_handle_t *vmg = ifoOpen(dvd, 0);
    if (vmg) {
        for (int vts = 1; vts <= 99; vts++) {
            dvd_file_t *f = DVDOpenFile(dvd, vts, DVD_READ_TITLE_VOBS);
            if (!f)
                continue;
            unsigned char block[2048];
            bool decided = false;
            for (int sector = 0; sector < 1024 && !decided; sector++) {
                if (DVDReadBlocks(f, sector, 1, block) != 1) {
                    blocked = sector == 0;      // nothing readable at all
                    break;
                }
                bool scrambled = block_scrambled(block, &decided);
                if (decided)
                    blocked = scrambled;
            }
            DVDCloseFile(f);
            if (decided || blocked)
                break;
        }
        ifoClose(vmg);
    }
    DVDClose(dvd);
    return blocked;
}

#else

static bool enumerate_dvd(void *ctx, struct mp_log *log, struct mp_cancel *cancel,
                          const char *device, struct disc *d, struct err *e)
{
    return fail(e, "dvd-unavailable",
                N_("This build has no DVD support (libdvdnav/libdvdread not enabled)."));
}

#endif

// Opens the disc and lists its titles. Never returns a disc without titles.
static bool open_disc(void *ctx, struct mp_log *log, struct mp_cancel *cancel,
                      const char *device_in, struct disc *d, struct err *e)
{
    normalize_device(d->device, sizeof(d->device), device_in);
    if (!d->device[0])
        return fail(e, "bad-device", N_("No disc device was given."));

    char letter;
    bool drive = is_drive_device(d->device, &letter);
    enum kind want = K_NONE;
    if (drive) {
        wchar_t root[4] = {(wchar_t)letter, L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN)
            return fail(e, "bad-device", N_("%s is not a drive."), d->device);
        if (!read_volume(ctx, d->device, &d->vol))
            return fail(e, "no-media", N_("There is no readable disc in %s."), d->device);
        want = detect_kind(ctx, d->device);
        if (want == K_NONE)
            return fail(e, "not-a-disc", N_("%s does not contain a Blu-ray or DVD-Video disc."),
                        d->device);
        copy_str(d->label, sizeof(d->label), d->vol.label);
    } else {
        DWORD a;
        if (!path_attr(ctx, d->device, &a))
            return fail(e, "bad-device", N_("%s does not exist."), d->device);
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            want = detect_kind(ctx, d->device);
            if (want == K_NONE)
                return fail(e, "not-a-disc", N_("%s has no BDMV or VIDEO_TS folder."), d->device);
        }
        // A disc image: tried as Blu-ray first, then as DVD.
    }

    char *label_from_drive = talloc_strdup(ctx, d->label);
    struct err e2 = {0};
    bool ok;
    if (want == K_DVD) {
        ok = enumerate_dvd(ctx, log, cancel, d->device, d, e);
    } else if (want == K_BD) {
        ok = enumerate_bd(ctx, log, cancel, d->device, d, e);
    } else {
        ok = enumerate_bd(ctx, log, cancel, d->device, d, &e2);
        if (!ok && strcmp(e2.code, "cancelled") && strcmp(e2.code, "unsupported-protected") &&
            strcmp(e2.code, "component-missing"))
            ok = enumerate_dvd(ctx, log, cancel, d->device, d, e);
        else if (!ok)
            *e = e2;
    }
    if (!ok)
        return false;

    if (!d->label[0])
        copy_str(d->label, sizeof(d->label), d->volume_id);
    if (!d->label[0])
        copy_str(d->label, sizeof(d->label), label_from_drive);
    if (!d->label[0])
        copy_str(d->label, sizeof(d->label), "DISC");
    if (!d->num_titles)
        return fail(e, "not-a-disc", N_("The disc has no title of at least %d seconds."),
                    MIN_TITLE_SECONDS);
    return true;
}

// ---------------------------------------------------------------------------
// Result nodes
// ---------------------------------------------------------------------------

static void result_common(mpv_node *res, bool ok, const char *status, const struct err *e,
                          const char *device)
{
    node_init(res, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_flag(res, "ok", ok);
    node_map_add_string(res, "status", status);
    node_map_add_string(res, "error", ok ? "" : e->msg);
    node_map_add_string(res, "error_code", ok ? "" : e->code);
    node_map_add_string(res, "device", device ? device : "");
}

static void array_add_string(mpv_node *arr, const char *s)
{
    mpv_node *n = node_array_add(arr, MPV_FORMAT_NONE);
    n->format = MPV_FORMAT_STRING;
    n->u.string = talloc_strdup(arr->u.list, s);
}

static void add_title_node(mpv_node *arr, const struct title *t)
{
    mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
    node_map_add_int64(m, "id", t->id);
    node_map_add_int64(m, "playlist", t->playlist);
    node_map_add_string(m, "name", t->name);
    node_map_add_double(m, "duration", t->duration);
    node_map_add_int64(m, "size", t->size);
    node_map_add_int64(m, "chapters", t->chapters);
    node_map_add_int64(m, "angles", t->angles);
    node_map_add_int64(m, "clips", t->clips);
    node_map_add_int64(m, "video", t->video);
    node_map_add_int64(m, "audio", t->audio);
    node_map_add_int64(m, "subtitle", t->subtitle);
}

static void fail_result(struct mp_cmd_ctx *cmd, const char *status, const struct err *e,
                        const char *device)
{
    result_common(&cmd->result, false, status, e, device);
}

static void busy_result(struct mp_cmd_ctx *cmd, const char *device)
{
    struct err e = {0};
    fail(&e, "busy", N_("Another disc operation is still running."));
    fail_result(cmd, "error", &e, device);
}

// ---------------------------------------------------------------------------
// nanum-disc-list
// ---------------------------------------------------------------------------

void nanum_disc_list(struct mp_cmd_ctx *cmd, const char *device)
{
    struct MPContext *mpctx = cmd->mpctx;
    struct mp_cancel *cancel = cmd->abort->cancel;
    device = device ? device : "";

    if (!begin_op(mpctx, "list", "scanning")) {
        busy_result(cmd, device);
        return;
    }
    mp_mutex_lock(&g_lock);
    copy_str(g.device, sizeof(g.device), device);
    mp_mutex_unlock(&g_lock);
    publish(mpctx);

    void *tmp = talloc_new(NULL);
    struct mp_log *log = mp_log_new(tmp, mpctx->log, "nanum-disc");
    mp_core_unlock(mpctx);

    struct err e = {0};
    struct drive *drives = NULL;
    int num_drives = 0;
    struct disc *d = talloc_zero(tmp, struct disc);
    bool ok = true;
    if (!device[0]) {
        num_drives = enumerate_drives(tmp, &drives);
    } else {
        ok = open_disc(tmp, log, cancel, device, d, &e);
        if (ok && cancelled(cancel))
            ok = fail(&e, "cancelled", N_("Cancelled."));
    }

    mp_core_lock(mpctx);

    bool was_cancelled = !ok && !strcmp(e.code, "cancelled");
    const char *status = ok ? "done" : was_cancelled ? "cancelled" : "error";
    if (ok && d->num_titles)
        set_disc(d);
    end_op(mpctx, status, ok ? NULL : &e);

    mpv_node *res = &cmd->result;
    result_common(res, ok, status, &e, device[0] ? d->device : "");
    if (!device[0]) {
        mpv_node *arr = node_map_add(res, "drives", MPV_FORMAT_NODE_ARRAY);
        for (int i = 0; i < num_drives; i++) {
            mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
            node_map_add_string(m, "device", drives[i].device);
            node_map_add_string(m, "drive_type", "cdrom");
            node_map_add_flag(m, "has_media", drives[i].has_media);
            node_map_add_string(m, "label", drives[i].label);
            node_map_add_string(m, "kind", !drives[i].has_media ? "none"
                                           : kind_name(drives[i].kind));
        }
    } else if (ok) {
        mpv_node *dn = node_map_add(res, "disc", MPV_FORMAT_NODE_MAP);
        node_map_add_string(dn, "device", d->device);
        node_map_add_string(dn, "kind", kind_name(d->kind));
        node_map_add_string(dn, "id", d->id);
        node_map_add_string(dn, "volume_id", d->volume_id);
        node_map_add_string(dn, "label", d->label);
        mpv_node *prot = node_map_add(dn, "protection", MPV_FORMAT_NODE_ARRAY);
        if (d->aacs)
            array_add_string(prot, "aacs");
        if (d->bdplus)
            array_add_string(prot, "bdplus");
        if (d->css)
            array_add_string(prot, "css");
        mpv_node *titles = node_map_add(dn, "titles", MPV_FORMAT_NODE_ARRAY);
        for (int i = 0; i < d->num_titles; i++)
            add_title_node(titles, &d->titles[i]);
    }
    talloc_free(tmp);
}

// ---------------------------------------------------------------------------
// Output files
// ---------------------------------------------------------------------------

static const wchar_t *const RESERVED[] = {
    L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6",
    L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7",
    L"LPT8", L"LPT9",
};

static wchar_t *wcs_underscore_prefix(void *ctx, const wchar_t *s)
{
    size_t n = wcslen(s);
    wchar_t *o = talloc_array(ctx, wchar_t, n + 2);
    o[0] = L'_';
    memcpy(o + 1, s, (n + 1) * sizeof(wchar_t));
    return o;
}

// A file name stem that is valid on Windows for any disc label.
static char *sanitize_stem(void *ctx, const char *label)
{
    wchar_t *w = W(ctx, label);
    size_t n = wcslen(w);
    if (n > 80) {
        n = 80;
        if (w[n - 1] >= 0xD800 && w[n - 1] <= 0xDBFF)
            n--;
    }
    wchar_t *o = talloc_array(ctx, wchar_t, n + 2);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = w[i];
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c))
            c = L'_';
        o[k++] = c;
    }
    while (k && (o[k - 1] == L'.' || o[k - 1] == L' '))
        k--;
    size_t start = 0;
    while (start < k && o[start] == L' ')
        start++;
    o[k] = 0;
    wchar_t *s = o + start;
    if (!*s)
        return talloc_strdup(ctx, "DISC");
    for (size_t i = 0; i < MP_ARRAY_SIZE(RESERVED); i++) {
        size_t rl = wcslen(RESERVED[i]);
        if (!_wcsnicmp(s, RESERVED[i], rl) && (!s[rl] || s[rl] == L'.' || s[rl] == L' ')) {
            s = wcs_underscore_prefix(ctx, s);
            break;
        }
    }
    return mp_to_utf8(ctx, s);
}

// Creates the directory and every missing parent; returns the full path.
static bool ensure_dir(void *ctx, const char *path, char **full, struct err *e)
{
    wchar_t *w = W(ctx, path);
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(w, MP_ARRAY_SIZE(buf), buf, NULL);
    if (!n || n >= MP_ARRAY_SIZE(buf))
        return fail(e, "bad-destination", N_("The destination path is invalid: %s"), path);
    size_t len = wcslen(buf);
    while (len > 3 && (buf[len - 1] == L'\\' || buf[len - 1] == L'/'))
        buf[--len] = 0;
    // Skip the drive root or the UNC server\share, then create each component.
    size_t start = 3;
    if (buf[0] == L'\\' && buf[1] == L'\\') {
        int seps = 0;
        start = 2;
        while (buf[start] && seps < 2) {
            if (buf[start] == L'\\')
                seps++;
            start++;
        }
    }
    for (size_t i = start; i <= len; i++) {
        if (buf[i] != L'\\' && buf[i] != 0)
            continue;
        wchar_t save = buf[i];
        buf[i] = 0;
        DWORD cur = GetFileAttributesW(buf);
        if (cur == INVALID_FILE_ATTRIBUTES || !(cur & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!CreateDirectoryW(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                DWORD err = GetLastError();
                char *p = mp_to_utf8(ctx, buf);
                return fail(e, "bad-destination", N_("Cannot create the folder %s (error %lu)."),
                            p, (unsigned long)err);
            }
        }
        buf[i] = save;
        if (!save)
            break;
    }
    DWORD a = GetFileAttributesW(buf);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
        return fail(e, "bad-destination", N_("%s is not a folder."), path);
    *full = mp_to_utf8(ctx, buf);
    return true;
}

// <dir>\<stem>.mkv, or "<stem> (2).mkv" ... if a file with that name exists.
static char *pick_final_path(void *ctx, const char *dir, const char *stem)
{
    for (int i = 1; i < 10000; i++) {
        char *name = i == 1 ? talloc_asprintf(ctx, "%s.mkv", stem)
                            : talloc_asprintf(ctx, "%s (%d).mkv", stem, i);
        char *p = join_path(ctx, dir, name);
        if (!path_exists_or_error(ctx, p))
            return p;
    }
    return NULL;
}

static LONG g_part_seq;

// Reserves a unique ".part" file next to the final target.
static char *reserve_part(void *ctx, const char *final_path)
{
    for (int i = 0; i < 100; i++) {
        LONG seq = InterlockedIncrement(&g_part_seq);
        char *p = talloc_asprintf(ctx, "%s.nanum-rip-%lu-%ld.part", final_path,
                                  (unsigned long)GetCurrentProcessId(), (long)seq);
        HANDLE h = CreateFileW(W(ctx, p), GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return p;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            return NULL;
    }
    return NULL;
}

static void delete_file(void *ctx, const char *path)
{
    if (path)
        DeleteFileW(W(ctx, path));
}

static void flush_to_disk(void *ctx, const char *path)
{
    HANDLE h = CreateFileW(W(ctx, path), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    FlushFileBuffers(h);
    CloseHandle(h);
}

static int64_t file_size(void *ctx, const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(W(ctx, path), GetFileExInfoStandard, &a))
        return -1;
    return ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

// ---------------------------------------------------------------------------
// Remux of one title
// ---------------------------------------------------------------------------

struct out_stream {
    struct sh_stream *sh;
    AVStream *av;
    int64_t last_dts;       // in av->time_base, AV_NOPTS_VALUE before the first packet
    bool is_av;             // audio/video (drives time origin and end detection)
    bool seen;
};

struct remux {
    struct mp_log *log;
    struct mp_cancel *cancel;
    AVFormatContext *fc;
    AVPacket *tmp;
    struct out_stream *os;
    int num_os;
    int *by_index;              // demux stream index -> os index or -1
    int num_by_index;
    double base;                // timestamp subtracted from every packet
    bool header_done;
    int64_t fixes, dropped, packets;
    double max_end;             // highest muxed timestamp (relative to base)
    struct err *e;
};

static void remux_free(struct remux *r)
{
    if (r->fc) {
        if (r->fc->pb)
            avio_closep(&r->fc->pb);
        avformat_free_context(r->fc);
        r->fc = NULL;
    }
    av_packet_free(&r->tmp);
}

static int av_interrupt(void *opaque)
{
    return cancelled(opaque);
}

static int mp_to_av_disposition(const struct sh_stream *sh)
{
    int d = 0;
    if (sh->default_track)
        d |= AV_DISPOSITION_DEFAULT;
    if (sh->forced_track)
        d |= AV_DISPOSITION_FORCED;
    if (sh->hearing_impaired_track)
        d |= AV_DISPOSITION_HEARING_IMPAIRED;
    if (sh->visual_impaired_track)
        d |= AV_DISPOSITION_VISUAL_IMPAIRED;
    if (sh->original_track)
        d |= AV_DISPOSITION_ORIGINAL;
    if (sh->commentary_track)
        d |= AV_DISPOSITION_COMMENT;
    if (sh->dependent_track)
        d |= AV_DISPOSITION_DEPENDENT;
    return d;
}

static const char *av_err(int ret, char *buf, size_t size)
{
    if (av_strerror(ret, buf, size) < 0)
        snprintf(buf, size, "error %d", ret);
    return buf;
}

// Registers one demuxer stream as an output stream. Returns 1 added, 0 skipped
// (a stream the media itself marks absent), -1 error (e set). Every present
// stream must be representable: nothing is dropped silently.
static int add_out_stream(struct remux *r, struct sh_stream *sh,
                          const struct sh_stream *first_video, struct err *e)
{
    if (sh->absent)
        return 0;
    enum AVMediaType type = mp_to_av_stream_type(sh->type);
    if (type == AVMEDIA_TYPE_UNKNOWN || !sh->codec) {
        fail(e, "mux", N_("Stream %d (id 0x%x) has an unsupported type."), sh->index,
             sh->demuxer_id);
        return -1;
    }
    const char *cname = sh->codec->codec ? sh->codec->codec : "?";
    AVCodecParameters *avp = mp_codec_params_to_av(sh->codec);
    if (!avp || avp->codec_id == AV_CODEC_ID_NONE) {
        avcodec_parameters_free(&avp);
        fail(e, "mux", N_("The %s stream %d (id 0x%x, codec '%s') cannot be copied without "
             "re-encoding."), stream_type_name(sh->type), sh->index, sh->demuxer_id, cname);
        return -1;
    }
    if (av_codec_get_id(r->fc->oformat->codec_tag, avp->codec_tag) != avp->codec_id)
        avp->codec_tag = 0;

    AVStream *st = avformat_new_stream(r->fc, NULL);
    if (!st || avcodec_parameters_copy(st->codecpar, avp) < 0) {
        avcodec_parameters_free(&avp);
        fail(e, "internal", N_("Out of memory."));
        return -1;
    }
    avcodec_parameters_free(&avp);
    st->time_base = mp_get_codec_timebase(sh->codec);

    // VobSub extradata: the player's palette line; the .idx convention also
    // carries the frame size.
    if (st->codecpar->codec_id == AV_CODEC_ID_DVD_SUBTITLE && first_video &&
        first_video->codec && first_video->codec->disp_w > 0 && first_video->codec->disp_h > 0 &&
        st->codecpar->extradata_size > 0)
    {
        char size_line[48];
        int l = snprintf(size_line, sizeof(size_line), "size: %dx%d\n",
                         first_video->codec->disp_w, first_video->codec->disp_h);
        int old = st->codecpar->extradata_size;
        uint8_t *nd = av_mallocz(old + l + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!nd) {
            fail(e, "internal", N_("Out of memory."));
            return -1;
        }
        memcpy(nd, size_line, l);
        memcpy(nd + l, st->codecpar->extradata, old);
        av_freep(&st->codecpar->extradata);
        st->codecpar->extradata = nd;
        st->codecpar->extradata_size = old + l;
    }

    if (sh->lang && sh->lang[0])
        av_dict_set(&st->metadata, "language", sh->lang, 0);
    if (sh->title && sh->title[0])
        av_dict_set(&st->metadata, "title", sh->title, 0);
    st->disposition = mp_to_av_disposition(sh);

    struct out_stream o = {
        .sh = sh,
        .av = st,
        .last_dts = AV_NOPTS_VALUE,
        .is_av = sh->type == STREAM_VIDEO || sh->type == STREAM_AUDIO,
    };
    MP_TARRAY_APPEND(NULL, r->os, r->num_os, o);
    return 1;
}

// AVStream pointers do not move; the os array may, so index by number.
static struct out_stream *os_for(struct remux *r, int demux_index)
{
    if (demux_index < 0 || demux_index >= r->num_by_index || r->by_index[demux_index] < 0)
        return NULL;
    return &r->os[r->by_index[demux_index]];
}

static void demux_buf_free(void *opaque, uint8_t *data)
{
    talloc_free(opaque);
}

// Takes ownership of pkt. The demuxer's buffer is handed to libavformat without
// a copy (reference or ownership transfer), so the packet outlives the muxer's
// interleaving queue safely.
static bool write_packet(struct remux *r, struct out_stream *o, struct demux_packet *pkt)
{
    double dts = pkt->dts, pts = pkt->pts;
    if (dts == MP_NOPTS_VALUE && pts == MP_NOPTS_VALUE) {
        talloc_free(pkt);
        return fail(r->e, "mux", N_("A packet of stream %d (%s) has no timestamp; it cannot be "
                    "stored without losing it."), o->sh->index,
                    stream_type_name(o->sh->type));
    }
    if (dts == MP_NOPTS_VALUE || pts == MP_NOPTS_VALUE) {
        if (dts == MP_NOPTS_VALUE)
            dts = pts;
        else
            pts = dts;
        r->fixes++;
    }
    dts -= r->base;
    pts -= r->base;
    if (pts < dts) {
        dts = pts;
        r->fixes++;
    }
    double duration = pkt->duration;
    bool key = pkt->keyframe;
    size_t len = pkt->len;
    if (!len) {
        // Nothing to store.
        r->dropped++;
        mp_warn(r->log, "empty packet in stream %d skipped\n", o->sh->index);
        talloc_free(pkt);
        return true;
    }

    AVPacket *out = r->tmp;
    av_packet_unref(out);
    const AVPacket *src = pkt->avpacket;
    int ret = 0;
    if (src && src->buf && src->data == pkt->buffer) {
        ret = av_packet_ref(out, src);      // shares the buffer, copies side data
        if (ret >= 0)
            out->size = (int)len;
        talloc_free(pkt);
    } else {
        AVBufferRef *buf = av_buffer_create(pkt->buffer, len, demux_buf_free, pkt, 0);
        if (!buf) {
            talloc_free(pkt);
            return fail(r->e, "internal", N_("Out of memory."));
        }
        out->buf = buf;                     // owns pkt from here on
        out->data = buf->data;
        out->size = (int)len;
        for (int i = 0; src && i < src->side_data_elems && ret >= 0; i++) {
            const AVPacketSideData *sd = &src->side_data[i];
            uint8_t *p = av_packet_new_side_data(out, sd->type, sd->size);
            if (p)
                memcpy(p, sd->data, sd->size);
            else
                ret = AVERROR(ENOMEM);
        }
        // pkt may be freed by av_packet_unref(out) below: not touched anymore.
    }
    if (ret < 0) {
        av_packet_unref(out);
        return fail(r->e, "internal", N_("Out of memory."));
    }

    AVRational tb = o->av->time_base;
    out->pts = mp_pts_to_av(pts, &tb);
    out->dts = mp_pts_to_av(dts, &tb);
    out->duration = duration > 0 ? llrint(duration / av_q2d(tb)) : 0;
    out->stream_index = o->av->index;
    out->time_base = tb;
    if (key)
        out->flags |= AV_PKT_FLAG_KEY;
    if (o->last_dts != AV_NOPTS_VALUE && out->dts < o->last_dts) {
        out->dts = o->last_dts;
        r->fixes++;
    }
    if (out->pts < out->dts) {
        out->pts = out->dts;
        r->fixes++;
    }
    o->last_dts = out->dts;

    ret = av_interleaved_write_frame(r->fc, out);
    av_packet_unref(out);
    if (ret < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE];
        bool io = r->fc->pb && r->fc->pb->error < 0;
        return fail(r->e, io ? "io" : "mux", N_("Writing the output file failed: %s"),
                    av_err(io ? r->fc->pb->error : ret, b, sizeof(b)));
    }
    r->packets++;
    double end = dts + MPMAX(duration, 0);
    if (o->is_av && end > r->max_end)
        r->max_end = end;
    return true;
}

// The set of streams must not change after the output was planned: a stream that
// appears later would lose its first packets (the demuxer drops data of
// unselected streams) and could not be stored anyway.
static bool streams_stable(struct remux *r, struct demuxer *d, int known, struct err *e)
{
    int n = demux_get_num_stream(d);
    for (int i = known; i < n; i++) {
        const struct sh_stream *sh = demux_get_stream(d, i);
        if (sh->absent)
            continue;
        return fail(e, "mux", N_("A %s stream (id 0x%x) appeared after the title was opened; a "
                    "lossless copy cannot include it."), stream_type_name(sh->type),
                    sh->demuxer_id);
    }
    return true;
}

struct title_result {
    int64_t size;
    double duration;
    int streams;
    int64_t fixes, dropped;
    int chapters;
};

enum rip_status { RIP_OK, RIP_CANCELLED, RIP_ERROR };

struct job {
    struct MPContext *mpctx;
    struct mp_log *log;
    struct mp_cancel *cancel;
    struct disc *disc;
    int *ids;                   // selected title indexes
    int num_ids;
    char *dest;
    double total_duration;
    double done_duration;
    struct {
        int id, playlist;
        char file[1024];
        int64_t size;
        double duration;
        int chapters, streams;
        int64_t fixes, dropped;
    } *outs;
    int num_outs;
    int failed_id;
    struct err e;
};

static void set_progress(const struct job *j, const struct title *t, double frac, bool force)
{
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    double overall = j->total_duration > 0
        ? (j->done_duration + frac * t->duration) / j->total_duration : frac;
    mp_mutex_lock(&g_lock);
    g.title_progress = frac;
    g.progress = overall > 1 ? 1 : overall;
    g.bytes_total = t->size;
    g.bytes_done = (int64_t)(frac * t->size);
    mp_mutex_unlock(&g_lock);
    if (force)
        request_publish();
    else
        request_publish_throttled();
}

static void set_status(const char *status)
{
    mp_mutex_lock(&g_lock);
    g.status = status;
    mp_mutex_unlock(&g_lock);
    request_publish();
}

static void add_chapters(struct remux *r, struct demuxer *d, double title_duration)
{
    int n = d->num_chapters;
    for (int i = 0; i < n; i++) {
        double start = d->chapters[i].pts - r->base;
        if (start < 0)
            start = 0;
        double end = i + 1 < n ? d->chapters[i + 1].pts - r->base : title_duration;
        if (end <= start)
            end = start + 0.001;
        AVChapter *ch = av_mallocz(sizeof(*ch));
        if (!ch)
            return;
        ch->id = i + 1;
        ch->time_base = (AVRational){1, 1000};
        ch->start = llround(start * 1000);
        ch->end = llround(end * 1000);
        if (ch->end <= ch->start)
            ch->end = ch->start + 1;
        char name[32];
        snprintf(name, sizeof(name), "Chapter %02d", i + 1);
        const char *title = d->chapters[i].metadata ? mp_tags_get_str(d->chapters[i].metadata, "TITLE")
                                                    : NULL;
        av_dict_set(&ch->metadata, "title", title && title[0] ? title : name, 0);
        if (av_dynarray_add_nofree(&r->fc->chapters, (int *)&r->fc->nb_chapters, ch) < 0) {
            av_dict_free(&ch->metadata);
            av_free(ch);
            return;
        }
    }
}

static bool volume_unchanged(void *ctx, const struct disc *d)
{
    if (!d->vol.valid)
        return true;
    struct vol_id v;
    if (!read_volume(ctx, d->device, &v))
        return false;
    return v.serial == d->vol.serial && !strcmp(v.label, d->vol.label);
}

// Remuxes one title into a fresh ".part" file and renames it to the final name.
static enum rip_status rip_title(struct job *j, const struct title *t, int index_in_job,
                                 const char *dest, const char *stem)
{
    struct disc *disc = j->disc;
    void *tmp = talloc_new(NULL);
    struct remux r = {.log = j->log, .cancel = j->cancel, .base = MP_NOPTS_VALUE, .e = &j->e};
    struct err *e = &j->e;
    enum rip_status st = RIP_ERROR;
    struct demuxer *d = NULL;
    char *part = NULL, *final_path = NULL;
    struct demux_packet **buf = NULL;
    int nbuf = 0;
    size_t buf_bytes = 0;
    bool eof = false;
    struct mpv_global *pg = NULL;

    char *base_name = talloc_asprintf(tmp, "%s_t%02d", stem, t->id + 1);
    final_path = pick_final_path(tmp, dest, base_name);
    if (!final_path) {
        fail(e, "bad-destination", N_("No free file name for %s in %s."), base_name, dest);
        goto done;
    }

    mp_mutex_lock(&g_lock);
    g.title_index = index_in_job;
    g.title_id = t->id;
    g.playlist = t->playlist;
    copy_str(g.file, sizeof(g.file), final_path);
    g.status = "ripping";
    mp_mutex_unlock(&g_lock);
    set_progress(j, t, 0, true);

    if (!volume_unchanged(tmp, disc)) {
        fail(e, "disc-mismatch", N_("The disc in %s was changed (or removed) during the copy."),
             disc->device);
        goto done;
    }

    // Free space (estimate: the remuxed file is not larger than the source).
    {
        ULARGE_INTEGER avail;
        if (GetDiskFreeSpaceExW(W(tmp, dest), &avail, NULL, NULL)) {
            uint64_t need = (uint64_t)t->size / 100 * 95 + ((uint64_t)64 << 20);
            if (avail.QuadPart < need) {
                fail(e, "io", N_("Not enough free space in %s: about %llu MB needed, "
                     "%llu MB available."), dest, (unsigned long long)(need >> 20),
                     (unsigned long long)((uint64_t)avail.QuadPart >> 20));
                goto done;
            }
        }
    }

    // The URL is built from the explicit device; nothing global is touched.
    char *url;
    if (disc->kind == K_BD)
        url = talloc_asprintf(tmp, "bd://mpls/%05d/%s", t->playlist, disc->device);
    else
        url = talloc_asprintf(tmp, "dvd://%d/%s", t->id, disc->device);
    // The disc is read with a private option snapshot (all defaults): the
    // player's own settings (--edition, --bluray-device, angle, ...) can neither
    // redirect this copy to another title nor are they modified by it. The log,
    // packet pool and statistics are shared with the player.
    pg = talloc_zero(tmp, struct mpv_global);
    *pg = *j->mpctx->global;
    pg->config = m_config_shadow_new(&mp_opt_root);

    // DVD: pictures that start inside a PES packet have no PTS; libavformat
    // generates them by look-ahead (the stream is copied, not decoded).
    struct demuxer_params params = {.stream_flags = STREAM_ORIGIN_DIRECT,
                                    .genpts = disc->kind == K_DVD};
    d = demux_open_url(url, &params, j->cancel, pg);
    if (!d) {
        if (cancelled(j->cancel)) {
            st = RIP_CANCELLED;
            fail(e, "cancelled", N_("Cancelled."));
        } else {
            fail(e, "io", N_("The title could not be opened for reading (%s); see the log for "
                 "the cause."), url);
        }
        goto done;
    }
    if (disc->kind == K_DVD) {
        int title_no = t->id + 1;
        if (stream_control(d->stream, STREAM_CTRL_NAV_STOP_AT_TITLE_END, &title_no) <= 0) {
            fail(e, "io", N_("The end of DVD title %d could not be located (the disc navigation "
                 "was not in that title after opening it); a lossless copy needs it."),
                 title_no);
            goto done;
        }
    }

    // Output context and streams.
    int ret = avformat_alloc_output_context2(&r.fc, NULL, "matroska", NULL);
    if (ret < 0 || !r.fc) {
        fail(e, "internal", N_("The Matroska muxer is not available."));
        goto done;
    }
    r.tmp = av_packet_alloc();
    if (!r.tmp) {
        fail(e, "internal", N_("Out of memory."));
        goto done;
    }

    int num = demux_get_num_stream(d);
    struct sh_stream *first_video = NULL;
    for (int i = 0; i < num && !first_video; i++) {
        struct sh_stream *sh = demux_get_stream(d, i);
        if (sh->type == STREAM_VIDEO && !sh->absent)
            first_video = sh;
    }
    r.by_index = talloc_array(tmp, int, MPMAX(num, 1));
    r.num_by_index = num;
    for (int i = 0; i < num; i++) {
        struct sh_stream *sh = demux_get_stream(d, i);
        r.by_index[i] = -1;
        int a = add_out_stream(&r, sh, first_video, e);
        if (a < 0)
            goto done;
        if (a > 0) {
            r.by_index[i] = r.num_os - 1;
            demuxer_select_track(d, sh, MP_NOPTS_VALUE, true);
        }
    }
    int num_av = 0;
    for (int i = 0; i < r.num_os; i++)
        num_av += r.os[i].is_av;
    if (!num_av) {
        fail(e, "unsupported-protected", N_("The title delivered no audio or video stream. If the "
             "disc is protected, it could not be decrypted."));
        goto done;
    }

    part = reserve_part(tmp, final_path);
    if (!part) {
        fail(e, "bad-destination", N_("Cannot create a temporary file in %s."), dest);
        goto done;
    }
    AVIOInterruptCB icb = {.callback = av_interrupt, .opaque = j->cancel};
    ret = avio_open2(&r.fc->pb, part, AVIO_FLAG_WRITE, &icb, NULL);
    if (ret < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE];
        fail(e, "io", N_("Cannot write %s: %s"), part, av_err(ret, b, sizeof(b)));
        goto done;
    }
    r.fc->interrupt_callback = icb;

    const char *tag = disc->label;
    av_dict_set(&r.fc->metadata, "title", tag, 0);

    // Phase 1: read until every audio/video stream delivered a packet, so the
    // header (and the time origin) can be written.
    int seen_av = 0;
    // (the set of streams is fixed: see streams_stable())
    // The DVD stream ends exactly where the selected title does (the VM leaves it
    // for a menu, another title or title set): everything it delivered before
    // belongs to the title, nothing is cut by read-ahead. The time limit is only a
    // runaway guard (a title that loops); reaching it is an error, never an end.
    double limit = disc->kind == K_DVD ? 2 * t->dvd_span_max + 60 : -1;
    double origin = MP_NOPTS_VALUE;    // presentation time of the title's start
    for (;;) {
        if (cancelled(j->cancel)) {
            st = RIP_CANCELLED;
            fail(e, "cancelled", N_("Cancelled."));
            goto done;
        }
        struct demux_packet *pkt = demux_read_any_packet(d);
        if (!streams_stable(&r, d, num, e)) {
            talloc_free(pkt);
            goto done;
        }
        if (!pkt) {
            eof = true;
            break;
        }
        struct out_stream *o = os_for(&r, pkt->stream);
        if (!o) {
            fail(e, "mux", N_("The disc delivered data of stream %d, which is not part of the "
                 "output; refusing a lossy copy."), pkt->stream);
            talloc_free(pkt);
            goto done;
        }
        MP_TARRAY_APPEND(tmp, buf, nbuf, pkt);
        buf_bytes += pkt->len;
        if (o->is_av && !o->seen) {
            o->seen = true;
            seen_av++;
        }
        if (seen_av >= num_av || nbuf >= PREBUF_MAX_PACKETS || buf_bytes >= PREBUF_MAX_BYTES)
            break;
    }
    if (!nbuf) {
        fail(e, "io", N_("No data could be read from the title (the disc may be unreadable)."));
        goto done;
    }

    // Time origin: first audio/video timestamp. Chapters follow it.
    for (int i = 0; i < nbuf; i++) {
        struct out_stream *o = os_for(&r, buf[i]->stream);
        if (!o->is_av)
            continue;
        double ts = buf[i]->dts != MP_NOPTS_VALUE ? buf[i]->dts : buf[i]->pts;
        if (buf[i]->pts != MP_NOPTS_VALUE && (ts == MP_NOPTS_VALUE || buf[i]->pts < ts))
            ts = buf[i]->pts;
        if (ts != MP_NOPTS_VALUE && (r.base == MP_NOPTS_VALUE || ts < r.base))
            r.base = ts;
    }
    if (r.base == MP_NOPTS_VALUE)
        r.base = 0;

    // The IFO times count from the first presented video frame (first audio
    // sample without video), not from the earlier decode timestamp.
    for (int pass = 0; pass < 2 && origin == MP_NOPTS_VALUE; pass++) {
        enum stream_type want = pass == 0 ? STREAM_VIDEO : STREAM_AUDIO;
        for (int i = 0; i < nbuf; i++) {
            struct out_stream *o = os_for(&r, buf[i]->stream);
            if (o->sh->type == want && buf[i]->pts != MP_NOPTS_VALUE &&
                (origin == MP_NOPTS_VALUE || buf[i]->pts < origin))
                origin = buf[i]->pts;
        }
    }
    if (origin == MP_NOPTS_VALUE)
        origin = r.base;

    add_chapters(&r, d, t->duration > 0 ? (disc->kind == K_DVD ? origin - r.base + t->dvd_span_max
                                                              : t->duration)
                                        : r.base + 0.001);

    for (int i = 0; i < r.num_os; i++) {
        AVCodecParameters *cp = r.os[i].av->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->extradata_size <= 0 &&
            (cp->codec_id == AV_CODEC_ID_H264 || cp->codec_id == AV_CODEC_ID_HEVC))
        {
            fail(e, "mux", N_("The %s video stream has no codec configuration; it cannot be "
                 "stored in Matroska."), avcodec_get_name(cp->codec_id));
            goto done;
        }
    }

    // FlagDefault: keep the disc's flags; if a type has none, the first video/audio
    // track becomes the default (never a subtitle track).
    AVDictionary *mux_opts = NULL;
    av_dict_set(&mux_opts, "default_mode", "infer_no_subs", 0);
    ret = avformat_write_header(r.fc, &mux_opts);
    av_dict_free(&mux_opts);
    if (ret < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE];
        fail(e, cancelled(j->cancel) ? "cancelled" : "mux",
             N_("Matroska header could not be written: %s"), av_err(ret, b, sizeof(b)));
        if (cancelled(j->cancel))
            st = RIP_CANCELLED;
        goto done;
    }
    r.header_done = true;

    // Phase 2: the buffered packets, then the rest of the title.
    int64_t next_check = 0;
    ULONGLONG last_vol_check = GetTickCount64();
    int bi = 0;
    for (;;) {
        struct demux_packet *pkt;
        if (bi < nbuf) {
            pkt = buf[bi];
            buf[bi++] = NULL;
        } else {
            if (eof)
                break;
            pkt = demux_read_any_packet(d);
            if (!streams_stable(&r, d, num, e)) {
                talloc_free(pkt);
                goto done;
            }
            if (!pkt)
                break;
        }
        if (cancelled(j->cancel)) {
            talloc_free(pkt);
            st = RIP_CANCELLED;
            fail(e, "cancelled", N_("Cancelled."));
            goto done;
        }
        struct out_stream *o = os_for(&r, pkt->stream);
        if (!o) {
            fail(e, "mux", N_("The disc delivered data of stream %d, which is not part of the "
                 "output; refusing a lossy copy."), pkt->stream);
            talloc_free(pkt);
            goto done;
        }
        if (limit > 0) {
            double ts = 0;
            if (pkt->pts != MP_NOPTS_VALUE)
                ts = pkt->pts - origin;
            else if (pkt->dts != MP_NOPTS_VALUE)
                ts = pkt->dts - origin;
            if (ts >= limit) {
                talloc_free(pkt);
                fail(e, "io", N_("The title did not end after %.0f s although the disc announces "
                     "%.0f s; the disc navigation does not leave it."), ts, t->duration);
                goto done;
            }
        }
        if (!write_packet(&r, o, pkt)) {
            if (cancelled(j->cancel)) {
                st = RIP_CANCELLED;
                fail(e, "cancelled", N_("Cancelled."));
            }
            goto done;
        }
        if (++next_check % 64 == 0) {
            set_progress(j, t, t->duration > 0 ? r.max_end / t->duration : 0, false);
            ULONGLONG now = GetTickCount64();
            if (now - last_vol_check >= 2000 && (last_vol_check = now) &&
                !volume_unchanged(tmp, disc))
            {
                fail(e, "disc-mismatch", N_("The disc in %s was changed (or removed) during "
                     "the copy."), disc->device);
                goto done;
            }
        }
    }

    if (cancelled(j->cancel)) {
        st = RIP_CANCELLED;
        fail(e, "cancelled", N_("Cancelled."));
        goto done;
    }

    // The demuxer reports a read failure as the end of the data; a title that
    // ends far before its announced length is incomplete.
    {
        double tol = MPMAX(5.0, t->duration * 0.02);
        if (t->duration > 0 && r.max_end < t->duration - tol) {
            fail(e, "io", N_("The title ended after %.0f s of %.0f s; the disc could not be read "
                 "completely."), r.max_end, t->duration);
            goto done;
        }
    }
    if (!volume_unchanged(tmp, disc)) {
        fail(e, "disc-mismatch", N_("The disc in %s was changed (or removed) during the copy."),
             disc->device);
        goto done;
    }

    set_status("finalizing");
    set_progress(j, t, 1, true);
    ret = av_write_trailer(r.fc);
    if (ret < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE];
        bool io = r.fc->pb && r.fc->pb->error < 0;
        fail(e, io ? "io" : "mux", N_("Finishing the Matroska file failed: %s"),
             av_err(ret, b, sizeof(b)));
        goto done;
    }
    {
        int perr = r.fc->pb ? r.fc->pb->error : 0;
        ret = avio_closep(&r.fc->pb);
        if (ret >= 0 && perr < 0)
            ret = perr;
        if (ret < 0) {
            char b[AV_ERROR_MAX_STRING_SIZE];
            fail(e, "io", N_("Closing the output file failed: %s"), av_err(ret, b, sizeof(b)));
            goto done;
        }
    }
    flush_to_disk(tmp, part);
    int64_t size = file_size(tmp, part);
    if (size <= 0) {
        fail(e, "io", N_("The output file is empty or unreadable: %s"), part);
        goto done;
    }

    // Atomic finalization; never replaces an existing file.
    {
        bool moved = false;
        wchar_t *wpart = W(tmp, part);
        for (int i = 0; i < 100 && !moved; i++) {
            wchar_t *wfinal = W(tmp, final_path);
            if (MoveFileExW(wpart, wfinal, MOVEFILE_WRITE_THROUGH)) {
                moved = true;
                break;
            }
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) {
                fail(e, "io", N_("Cannot move %s to %s (error %lu)."), part, final_path,
                     (unsigned long)err);
                goto done;
            }
            final_path = pick_final_path(tmp, dest, base_name);
            if (!final_path) {
                fail(e, "bad-destination", N_("No free file name for %s in %s."), base_name, dest);
                goto done;
            }
        }
        if (!moved) {
            fail(e, "io", N_("Cannot move %s into place."), part);
            goto done;
        }
        part = NULL;
    }

    {
        struct title_result tr = {
            .size = size, .duration = r.max_end, .streams = r.num_os,
            .fixes = r.fixes, .dropped = r.dropped, .chapters = (int)r.fc->nb_chapters,
        };
        MP_TARRAY_GROW(j, j->outs, j->num_outs);
        j->outs[j->num_outs].id = t->id;
        j->outs[j->num_outs].playlist = t->playlist;
        copy_str(j->outs[j->num_outs].file, sizeof(j->outs[0].file), final_path);
        j->outs[j->num_outs].size = tr.size;
        j->outs[j->num_outs].duration = tr.duration;
        j->outs[j->num_outs].chapters = tr.chapters;
        j->outs[j->num_outs].streams = tr.streams;
        j->outs[j->num_outs].fixes = tr.fixes;
        j->outs[j->num_outs].dropped = tr.dropped;
        j->num_outs++;
        mp_mutex_lock(&g_lock);
        struct done_item it = {.id = t->id, .size = size};
        copy_str(it.file, sizeof(it.file), final_path);
        MP_TARRAY_APPEND(NULL, g.done, g.num_done, it);
        mp_mutex_unlock(&g_lock);
        mp_info(r.log, "title %d: %s (%"PRId64" bytes, %d streams, %d chapters)\n", t->id,
                final_path, size, r.num_os, tr.chapters);
        if (tr.fixes || tr.dropped)
            mp_warn(r.log, "title %d: %"PRId64" timestamp corrections, %"PRId64" dropped "
                    "packets\n", t->id, tr.fixes, tr.dropped);
    }
    st = RIP_OK;

done:
    for (int i = 0; i < nbuf; i++)
        talloc_free(buf[i]);
    remux_free(&r);
    talloc_free(r.os);
    if (d)
        demux_free(d);
    // The private option snapshot outlives everything that registered on it.
    if (pg)
        talloc_free(pg->config);
    if (st != RIP_OK && part)
        delete_file(tmp, part);
    talloc_free(tmp);
    return st;
}

// ---------------------------------------------------------------------------
// nanum-disc-rip
// ---------------------------------------------------------------------------

static bool parse_title_arg(const struct disc *d, const char *arg, int **ids, int *num,
                            void *ctx, struct err *e)
{
    if (!strcasecmp(arg, "all")) {
        *ids = talloc_array(ctx, int, d->num_titles);
        for (int i = 0; i < d->num_titles; i++)
            (*ids)[i] = i;
        *num = d->num_titles;
        return true;
    }
    char *end = NULL;
    long v = strtol(arg, &end, 10);
    if (!*arg || *end || v < 0 || v > INT_MAX)
        return fail(e, "bad-title", N_("'%s' is not a title id or \"all\"."), arg);
    for (int i = 0; i < d->num_titles; i++) {
        if (d->titles[i].id == v) {
            *ids = talloc_array(ctx, int, 1);
            (*ids)[0] = i;
            *num = 1;
            return true;
        }
    }
    return fail(e, "bad-title", N_("The disc has no title %lld (use the ids of nanum-disc-list)."), (long long)v);
}

void nanum_disc_rip(struct mp_cmd_ctx *cmd, const char *device, const char *title_arg,
                    const char *destination, const char *expected_id)
{
    struct MPContext *mpctx = cmd->mpctx;
    struct mp_cancel *cancel = cmd->abort->cancel;
    device = device ? device : "";
    title_arg = title_arg ? title_arg : "";
    destination = destination ? destination : "";
    expected_id = expected_id ? expected_id : "";

    if (!begin_op(mpctx, "rip", "scanning")) {
        busy_result(cmd, device);
        return;
    }
    mp_mutex_lock(&g_lock);
    copy_str(g.device, sizeof(g.device), device);
    copy_str(g.destination, sizeof(g.destination), destination);
    mp_mutex_unlock(&g_lock);
    publish(mpctx);

    void *tmp = talloc_new(NULL);
    struct mp_log *log = mp_log_new(tmp, mpctx->log, "nanum-disc");
    struct job *j = talloc_zero(tmp, struct job);
    j->mpctx = mpctx;
    j->log = log;
    j->cancel = cancel;
    j->failed_id = -1;
    struct disc *d = talloc_zero(tmp, struct disc);
    j->disc = d;
    struct err *e = &j->e;
    enum rip_status result = RIP_ERROR;

    mp_core_unlock(mpctx);

    bool ok = false;
    do {
        if (!device[0] || !title_arg[0] || !destination[0]) {
            fail(e, "bad-args", N_("device, title and destination are required."));
            break;
        }
        char *dest = NULL;
        if (!ensure_dir(tmp, destination, &dest, e))
            break;
        j->dest = dest;
        mp_mutex_lock(&g_lock);
        copy_str(g.destination, sizeof(g.destination), dest);
        mp_mutex_unlock(&g_lock);

        if (!open_disc(tmp, log, cancel, device, d, e)) {
            if (!strcmp(e->code, "cancelled"))
                result = RIP_CANCELLED;
            break;
        }
        set_disc(d);
        request_publish();

        if (expected_id[0]) {
            if (!d->id[0] || strcasecmp(expected_id, d->id)) {
                fail(e, "disc-mismatch", N_("The disc in %s is not the one that was listed "
                     "(expected %s, found %s)."), d->device, expected_id,
                     d->id[0] ? d->id : nanum_tr("an unidentifiable disc"));
                break;
            }
        }
        int *ids = NULL, num = 0;
        if (!parse_title_arg(d, title_arg, &ids, &num, tmp, e))
            break;
        j->ids = ids;
        j->num_ids = num;
        for (int i = 0; i < num; i++)
            j->total_duration += d->titles[ids[i]].duration;

        mp_mutex_lock(&g_lock);
        g.title_count = num;
        mp_mutex_unlock(&g_lock);
        request_publish();

        char *stem = sanitize_stem(tmp, d->label);
        ok = true;
        for (int i = 0; i < num && ok; i++) {
            const struct title *t = &d->titles[ids[i]];
            enum rip_status s = rip_title(j, t, i + 1, j->dest, stem);
            if (s == RIP_OK) {
                j->done_duration += t->duration;
            } else {
                ok = false;
                result = s;
                j->failed_id = t->id;
            }
        }
        if (ok)
            result = RIP_OK;
    } while (0);

    mp_core_lock(mpctx);

    bool done = ok && result == RIP_OK;
    bool was_cancelled = !done && (result == RIP_CANCELLED || !strcmp(e->code, "cancelled"));
    const char *status = done ? "done" : was_cancelled ? "cancelled" : "error";
    if (was_cancelled && strcmp(e->code, "cancelled"))
        fail(e, "cancelled", N_("Cancelled."));
    if (done) {
        mp_mutex_lock(&g_lock);
        g.progress = 1;
        g.title_progress = 1;
        mp_mutex_unlock(&g_lock);
    }
    end_op(mpctx, status, done ? NULL : e);

    mpv_node *res = &cmd->result;
    result_common(res, done, status, e, d->device[0] ? d->device : device);
    node_map_add_string(res, "disc_id", d->id);
    node_map_add_string(res, "disc_label", d->label);
    node_map_add_string(res, "destination", j->dest ? j->dest : destination);
    node_map_add_int64(res, "requested", j->num_ids);
    node_map_add_int64(res, "failed_id", j->failed_id);
    mpv_node *arr = node_map_add(res, "outputs", MPV_FORMAT_NODE_ARRAY);
    for (int i = 0; i < j->num_outs; i++) {
        mpv_node *m = node_array_add(arr, MPV_FORMAT_NODE_MAP);
        node_map_add_int64(m, "id", j->outs[i].id);
        node_map_add_int64(m, "playlist", j->outs[i].playlist);
        node_map_add_string(m, "file", j->outs[i].file);
        node_map_add_int64(m, "size", j->outs[i].size);
        node_map_add_double(m, "duration", j->outs[i].duration);
        node_map_add_int64(m, "chapters", j->outs[i].chapters);
        node_map_add_int64(m, "streams", j->outs[i].streams);
        node_map_add_int64(m, "timestamp_fixes", j->outs[i].fixes);
        node_map_add_int64(m, "dropped_packets", j->outs[i].dropped);
    }
    talloc_free(tmp);
}

#else // !_WIN32

void nanum_disc_list(struct mp_cmd_ctx *cmd, const char *device)
{
    cmd->success = false;
}

void nanum_disc_rip(struct mp_cmd_ctx *cmd, const char *device, const char *title,
                    const char *destination, const char *expected_disc_id)
{
    cmd->success = false;
}

#endif
