/*
 * NanumPlayer: AI subtitle bridge for the demuxer layer.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation;
 * either version 2 of the License, or (at your option)
 * any later version.
 */

#include <math.h>
#include <string.h>

#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "osdep/threads.h"
#include "nanum/nanum.h"

#define OPT_BASE_STRUCT struct nanum_opts
struct nanum_opts {
    bool enable;
    bool test_subs;
};

const struct m_sub_options nanum_conf = {
    .opts = (const m_option_t[]) {
        {"nanum", OPT_BOOL(enable)},
        {"nanum-test-subs", OPT_BOOL(test_subs)},
        {0}
    },
    .size = sizeof(struct nanum_opts),
};

struct nanum_event {
    double pts;
    double duration;
    int id;             // stable ASS ReadOrder; libass drops duplicates by it
    char *text;
};

struct nanum {
    struct mp_log *log;
    struct nanum_opts *opts;

    mp_mutex lock;
    struct nanum_event *events;     // sorted by pts
    int num_events;
    int next_id;

    // Tap statistics (demux thread only).
    int64_t audio_packets;
    double next_log_pts;
};

static const char ass_header[] =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 1920\n"
    "PlayResY: 1080\n"
    "WrapStyle: 0\n"
    "ScaledBorderAndShadow: yes\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
    "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
    "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
    "MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Malgun Gothic,46,&H00FFFFFF,&H000000FF,&H00000000,"
    "&H80000000,-1,0,0,0,100,100,0,0,1,3,2,2,20,20,45,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
    "Effect, Text\n";

static void nanum_destroy(void *p)
{
    struct nanum *n = p;
    mp_mutex_destroy(&n->lock);
}

void nanum_add_event(struct nanum *n, double pts, double duration,
                     const char *ass_text)
{
    mp_mutex_lock(&n->lock);

    // First index whose pts is greater than the new one (keeps order stable
    // for equal timestamps).
    int lo = 0, hi = n->num_events;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (n->events[mid].pts <= pts)
            lo = mid + 1;
        else
            hi = mid;
    }

    MP_TARRAY_GROW(n, n->events, n->num_events);
    memmove(&n->events[lo + 1], &n->events[lo],
            (n->num_events - lo) * sizeof(n->events[0]));
    n->events[lo] = (struct nanum_event){
        .pts = pts,
        .duration = duration,
        .id = n->next_id++,
        .text = talloc_strdup(n, ass_text),
    };
    n->num_events++;

    mp_mutex_unlock(&n->lock);
}

struct nanum *nanum_create(void *parent, struct mpv_global *global,
                           struct mp_log *log)
{
    struct nanum_opts *opts = mp_get_config_group(NULL, global, &nanum_conf);
    if (!opts->enable) {
        talloc_free(opts);
        return NULL;
    }

    struct nanum *n = talloc_zero(parent, struct nanum);
    n->log = mp_log_new(n, log, "nanum");
    n->opts = talloc_steal(n, opts);
    n->next_log_pts = 0;
    mp_mutex_init(&n->lock);
    talloc_set_destructor(n, nanum_destroy);

    if (n->opts->test_subs) {
        // Fixed test lines every 5 s for 4 h. Goes through the same entry
        // point the AI thread will use.
        for (int k = 0; k < 4 * 3600 / 5; k++) {
            int s = k * 5;
            char *t = talloc_asprintf(NULL,
                "[NanumPlayer 테스트] 자막 #%d (%02d:%02d)",
                k, s / 60, s % 60);
            nanum_add_event(n, s, 3.0, t);
            talloc_free(t);
        }
        mp_verbose(n->log, "test subtitles generated (%d events)\n",
                   n->num_events);
    }

    mp_info(n->log, "enabled\n");
    return n;
}

void nanum_cursor_reset(struct nanum_cursor *c, double from)
{
    c->from = from;
    c->last_pts = -INFINITY;
}

struct sh_stream *nanum_alloc_stream(struct nanum *n)
{
    struct sh_stream *sh = demux_alloc_sh_stream(STREAM_SUB);
    sh->demuxer_id = 0x4e414e;      // "NAN": not a real container id
    sh->title = talloc_strdup(sh, "ko (AI)");
    sh->lang = talloc_strdup(sh, "kor");
    sh->codec->codec = "ass";
    sh->codec->extradata = talloc_memdup(sh->codec, ass_header,
                                         sizeof(ass_header) - 1);
    sh->codec->extradata_size = sizeof(ass_header) - 1;
    return sh;
}

void nanum_tap(struct nanum *n, const struct sh_stream *src,
               const struct demux_packet *pkt)
{
    if (src->type != STREAM_AUDIO)
        return;

    n->audio_packets++;
    double ts = pkt->pts != MP_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts != MP_NOPTS_VALUE && ts >= n->next_log_pts) {
        mp_verbose(n->log, "tap: audio stream %d (%s) pts=%.2f len=%zu "
                   "packets=%lld\n", src->index, src->codec->codec, ts,
                   pkt->len, (long long)n->audio_packets);
        n->next_log_pts = ts + 30;
    }
}

struct demux_packet *nanum_next_packet(struct nanum *n,
                                       struct demux_packet_pool *pool,
                                       int stream_index,
                                       struct nanum_cursor *c,
                                       double from, double to)
{
    struct demux_packet *dp = NULL;

    mp_mutex_lock(&n->lock);

    // First event with pts > last injected pts.
    int lo = 0, hi = n->num_events;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (n->events[mid].pts <= c->last_pts)
            lo = mid + 1;
        else
            hi = mid;
    }

    for (int i = lo; i < n->num_events; i++) {
        struct nanum_event *ev = &n->events[i];
        if (ev->pts >= to)
            break;
        c->last_pts = ev->pts;
        if (ev->pts + ev->duration <= from)
            continue;       // already over at the start position

        // Matroska-style ASS chunk, as consumed by ass_process_chunk().
        char *chunk = talloc_asprintf(NULL, "%d,0,Default,,0,0,0,,%s",
                                      ev->id, ev->text);
        dp = new_demux_packet_from(pool, chunk, strlen(chunk));
        talloc_free(chunk);
        if (!dp)
            break;
        dp->pts = dp->dts = ev->pts;
        dp->duration = ev->duration;
        dp->keyframe = true;
        dp->stream = stream_index;
        dp->pos = -1;
        break;
    }

    mp_mutex_unlock(&n->lock);
    return dp;
}
