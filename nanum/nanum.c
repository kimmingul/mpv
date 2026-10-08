/*
 * NanumPlayer: AI subtitle bridge for the demuxer layer.
 *
 * Audio packets seen by the demuxer are copied to a worker thread, decoded
 * to 16 kHz mono, and fed to nanum_ai.dll (speech-to-text + translation).
 * Finished lines are stored here as subtitle events; demux.c injects them
 * into the virtual "ko (AI)" ASS stream.
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
#include <stdatomic.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#include "mpv_talloc.h"
#include "common/av_common.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "nanum/nanum.h"
#include "nanum/nanum_ai_api.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define OPT_BASE_STRUCT struct nanum_opts
struct nanum_opts {
    bool enable;
    bool test_subs;
    char *ai_dll;
    char *stt_model;
    char *vad_model;
    char *language;
    char *tr_endpoint;
    char *tr_model;
    char *target_language;
    char *cache;
};

const struct m_sub_options nanum_conf = {
    .opts = (const m_option_t[]) {
        {"nanum", OPT_BOOL(enable)},
        {"nanum-test-subs", OPT_BOOL(test_subs)},
        {"nanum-ai-dll", OPT_STRING(ai_dll)},
        {"nanum-stt-model", OPT_STRING(stt_model)},
        {"nanum-vad-model", OPT_STRING(vad_model)},
        {"nanum-language", OPT_STRING(language)},
        {"nanum-translate-endpoint", OPT_STRING(tr_endpoint)},
        {"nanum-translate-model", OPT_STRING(tr_model)},
        {"nanum-target-language", OPT_STRING(target_language)},
        {"nanum-cache", OPT_STRING(cache)},
        {0}
    },
    .size = sizeof(struct nanum_opts),
    .defaults = &(const struct nanum_opts){
        .ai_dll = "nanum_ai.dll",
        .language = "en",
        .tr_endpoint = "http://127.0.0.1:11434",
        .tr_model = "gemma4:latest",
        .target_language = "Korean",
    },
};

#define STT_RATE 16000
#define MAX_QUEUED_BYTES (32 * 1024 * 1024)

struct nanum_event {
    double pts;
    double duration;
    int id;             // stable ASS ReadOrder; libass drops duplicates by it
    char *text;
};

struct apkt {
    struct apkt *next;
    struct demux_packet *dp;
};

struct nanum {
    struct mp_log *log;
    struct nanum_opts *opts;

    // Event store, sorted by pts.
    mp_mutex lock;
    struct nanum_event *events;
    int num_events;
    int next_id;
    void (*wakeup)(void *ctx);
    void *wakeup_ctx;

    // Tap statistics (demux thread only).
    int64_t audio_packets;
    double next_log_pts;

    // AI engine and its audio worker.
    bool ai_enabled;
    bool shut_down;
    void *dll;
    struct nanum_ai_api api;
    struct nanum_ai *ai;
    mp_thread athread;
    bool athread_started;
    atomic_bool ai_failed;

    mp_mutex alock;             // guards the audio queue
    mp_cond acond;
    struct apkt *ahead, *atail;
    size_t abytes;
    bool astop;
    bool aflush;                // decoder must be flushed (seek)
    bool aeof;                  // demuxer reached EOF: flush the engine
    int astream;                // audio stream being analysed (demux thread)
    const struct mp_codec_params *acodec;

    // Worker-owned decode state.
    AVCodecContext *avctx;
    SwrContext *swr;
    int swr_rate, swr_fmt;
    AVChannelLayout swr_layout;
    float *obuf;
    int obuf_cap;
    double last_pkt_pts;
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

void nanum_set_wakeup(struct nanum *n, void (*cb)(void *ctx), void *ctx)
{
    mp_mutex_lock(&n->lock);
    n->wakeup = cb;
    n->wakeup_ctx = ctx;
    mp_mutex_unlock(&n->lock);
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

    // The same line is produced again when a region is analysed twice (seek
    // back): ignore exact repeats.
    for (int i = lo - 1; i >= 0 && pts - n->events[i].pts < 0.05; i--) {
        if (!strcmp(n->events[i].text, ass_text)) {
            mp_mutex_unlock(&n->lock);
            return;
        }
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

    void (*wakeup)(void *) = n->wakeup;
    void *ctx = n->wakeup_ctx;
    mp_mutex_unlock(&n->lock);

    if (wakeup)
        wakeup(ctx);
}

// ---------------------------------------------------------------------------
// AI engine glue

// ASS text of one line: translation on top, source below in a smaller grey font.
static char *build_ass_text(void *ctx, const char *src, const char *ko)
{
    const char *parts[2] = {ko, src};
    size_t cap = 64;
    for (int i = 0; i < 2; i++) {
        if (parts[i])
            cap += strlen(parts[i]) * 2;       // worst case: every char escaped
    }
    char *out = talloc_array(ctx, char, cap), *p = out;
    for (int i = 0; i < 2; i++) {
        if (!parts[i])
            continue;
        if (i == 1 && ko)
            p += sprintf(p, "\\N{\\fs30\\c&H00CCCCCC&}");
        for (const char *s = parts[i]; *s; s++) {
            if (*s == '{' || *s == '}')
                *p++ = '\\';
            *p++ = (*s == '\n' || *s == '\r') ? ' ' : *s;
        }
    }
    *p = '\0';
    return out;
}

static void on_ai_event(void *user, double pts, double dur, const char *src,
                        const char *ko)
{
    struct nanum *n = user;
    void *tmp = talloc_new(NULL);
    char *text = build_ass_text(tmp, src, ko);
    // Keep a line readable: at least 1.5 s on screen.
    double shown = fmax(dur + 0.4, 1.5);
    mp_verbose(n->log, "event %.2f +%.2f: %s | %s\n", pts, shown, src,
               ko ? ko : "(no translation)");
    nanum_add_event(n, pts, shown, text);
    talloc_free(tmp);
}

static void init_swr(struct nanum *n, AVFrame *f)
{
    swr_free(&n->swr);
    av_channel_layout_uninit(&n->swr_layout);

    AVChannelLayout out = {0};
    av_channel_layout_default(&out, 1);
    if (swr_alloc_set_opts2(&n->swr, &out, AV_SAMPLE_FMT_FLT, STT_RATE,
                            &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0)
        return;

    // Dialogue lives in the centre channel; keep the music/effects of the
    // other channels low so they do not drown the speech.
    int ch = f->ch_layout.nb_channels;
    double *m = av_calloc(ch, sizeof(double));
    bool has_center = av_channel_layout_index_from_channel(&f->ch_layout,
                                                           AV_CHAN_FRONT_CENTER) >= 0;
    for (int i = 0; m && i < ch; i++) {
        enum AVChannel id = av_channel_layout_channel_from_index(&f->ch_layout, i);
        if (ch == 1 || id == AV_CHAN_FRONT_CENTER)
            m[i] = 1.0;
        else if (id == AV_CHAN_FRONT_LEFT || id == AV_CHAN_FRONT_RIGHT)
            m[i] = has_center ? 0.3 : 0.7;
        else if (id == AV_CHAN_LOW_FREQUENCY || id == AV_CHAN_LOW_FREQUENCY_2)
            m[i] = 0.0;
        else
            m[i] = has_center ? 0.05 : 0.3;
    }
    if (m)
        swr_set_matrix(n->swr, m, ch);
    av_free(m);

    if (swr_init(n->swr) < 0) {
        swr_free(&n->swr);
        return;
    }
    n->swr_rate = f->sample_rate;
    n->swr_fmt = f->format;
    av_channel_layout_copy(&n->swr_layout, &f->ch_layout);
}

static void process_frame(struct nanum *n, AVFrame *f)
{
    if (!n->swr || n->swr_rate != f->sample_rate || n->swr_fmt != f->format ||
        av_channel_layout_compare(&n->swr_layout, &f->ch_layout))
        init_swr(n, f);
    if (!n->swr)
        return;

    int cap = (int)av_rescale_rnd(swr_get_delay(n->swr, f->sample_rate) + f->nb_samples,
                                  STT_RATE, f->sample_rate, AV_ROUND_UP) + 32;
    if (cap > n->obuf_cap) {
        av_free(n->obuf);
        n->obuf = av_malloc_array(cap, sizeof(float));
        n->obuf_cap = n->obuf ? cap : 0;
        if (!n->obuf)
            return;
    }
    int got = swr_convert(n->swr, (uint8_t **)&n->obuf, cap,
                          (const uint8_t **)f->extended_data, f->nb_samples);
    if (got <= 0)
        return;

    double pts = n->last_pkt_pts;
    if (f->pts != AV_NOPTS_VALUE) {
        AVRational tb = f->time_base.num > 0 ? f->time_base : (AVRational){1, 1000000};
        pts = f->pts * av_q2d(tb);
    }
    n->api.push_audio(n->ai, n->obuf, got, pts);
}

static void decode_packet(struct nanum *n, struct demux_packet *dp)
{
    AVPacket *avpkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!avpkt || !frame)
        goto done;

    AVRational tb = {1, 1000000};
    mp_set_av_packet(avpkt, dp, &tb);
    n->last_pkt_pts = dp->pts != MP_NOPTS_VALUE ? dp->pts : n->last_pkt_pts;

    if (avcodec_send_packet(n->avctx, avpkt) < 0)
        goto done;
    while (avcodec_receive_frame(n->avctx, frame) >= 0) {
        process_frame(n, frame);
        av_frame_unref(frame);
    }
done:
    av_frame_free(&frame);
    mp_free_av_packet(&avpkt);      // buf/side_data are borrowed from dp
}

static bool open_decoder(struct nanum *n)
{
    const struct mp_codec_params *c = n->acodec;
    const AVCodec *codec = avcodec_find_decoder(mp_codec_to_av_codec_id(c->codec));
    if (!codec) {
        mp_err(n->log, "no decoder for audio codec '%s'\n", c->codec);
        return false;
    }
    n->avctx = avcodec_alloc_context3(codec);
    if (!n->avctx || mp_set_avctx_codec_headers(n->avctx, c) < 0) {
        avcodec_free_context(&n->avctx);
        return false;
    }
    n->avctx->pkt_timebase = (AVRational){1, 1000000};
    if (avcodec_open2(n->avctx, codec, NULL) < 0) {
        mp_err(n->log, "cannot open audio decoder '%s'\n", c->codec);
        avcodec_free_context(&n->avctx);
        return false;
    }
    mp_verbose(n->log, "analysing audio stream: %s %d Hz, %d ch\n", c->codec,
               c->samplerate, c->channels.num);
    return true;
}

static MP_THREAD_VOID audio_thread(void *p)
{
    struct nanum *n = p;
    mp_thread_set_name("nanum-audio");

    // Load and initialise the engine here so opening the file is not delayed
    // by the model load.
    struct nanum_opts *o = n->opts;
    struct nanum_ai_config cfg = {
        .abi_version = NANUM_AI_ABI_VERSION,
        .stt_model_path = o->stt_model,
        .vad_model_path = o->vad_model,
        .language = o->language,
        .use_gpu = 1,
        .translate_endpoint = o->tr_endpoint,
        .translate_model = o->tr_model,
        .target_language = o->target_language,
        .cache_path = o->cache,
    };
    n->ai = n->api.create(&cfg, on_ai_event, n);
    if (!n->ai) {
        mp_err(n->log, "cannot start the AI engine: %s\n", n->api.last_error());
        atomic_store(&n->ai_failed, true);
        MP_THREAD_RETURN();
    }
    mp_info(n->log, "AI engine ready (STT: %s, translation: %s)\n", o->stt_model,
            o->tr_endpoint && o->tr_endpoint[0] ? o->tr_model : "off");

    mp_mutex_lock(&n->alock);
    while (!n->astop) {
        if (n->aflush) {
            n->aflush = false;
            if (n->avctx)
                avcodec_flush_buffers(n->avctx);
            if (n->swr)
                swr_close(n->swr), swr_init(n->swr);
        }
        struct apkt *a = n->ahead;
        if (!a) {
            if (n->aeof) {
                n->aeof = false;
                mp_mutex_unlock(&n->alock);
                n->api.flush(n->ai);    // analyse the audio left in the buffer
                mp_mutex_lock(&n->alock);
                continue;
            }
            mp_cond_wait(&n->acond, &n->alock);
            continue;
        }
        n->ahead = a->next;
        if (!n->ahead)
            n->atail = NULL;
        n->abytes -= a->dp->len;
        mp_mutex_unlock(&n->alock);

        if (!n->avctx && !open_decoder(n)) {
            atomic_store(&n->ai_failed, true);
            talloc_free(a->dp);
            talloc_free(a);
            mp_mutex_lock(&n->alock);
            break;
        }
        decode_packet(n, a->dp);
        talloc_free(a->dp);
        talloc_free(a);

        mp_mutex_lock(&n->alock);
    }
    mp_mutex_unlock(&n->alock);
    MP_THREAD_RETURN();
}

static void clear_audio_queue_locked(struct nanum *n)
{
    while (n->ahead) {
        struct apkt *a = n->ahead;
        n->ahead = a->next;
        talloc_free(a->dp);
        talloc_free(a);
    }
    n->atail = NULL;
    n->abytes = 0;
}

#ifdef _WIN32
static bool load_engine(struct nanum *n)
{
    wchar_t *path = mp_from_utf8(n, n->opts->ai_dll);
    n->dll = (void *)LoadLibraryW(path);
    talloc_free(path);
    if (!n->dll) {
        mp_err(n->log, "cannot load '%s' (error %lu)\n", n->opts->ai_dll,
               (unsigned long)GetLastError());
        return false;
    }
    HMODULE h = n->dll;
    n->api.create = (void *)GetProcAddress(h, "nanum_ai_create");
    n->api.destroy = (void *)GetProcAddress(h, "nanum_ai_destroy");
    n->api.push_audio = (void *)GetProcAddress(h, "nanum_ai_push_audio");
    n->api.flush = (void *)GetProcAddress(h, "nanum_ai_flush");
    n->api.reset = (void *)GetProcAddress(h, "nanum_ai_reset");
    n->api.last_error = (void *)GetProcAddress(h, "nanum_ai_last_error");
    if (!n->api.create || !n->api.destroy || !n->api.push_audio ||
        !n->api.flush || !n->api.reset || !n->api.last_error) {
        mp_err(n->log, "'%s' does not export the nanum_ai API\n", n->opts->ai_dll);
        return false;
    }
    return true;
}

static void unload_engine(struct nanum *n)
{
    if (n->dll)
        FreeLibrary((HMODULE)n->dll);
    n->dll = NULL;
}
#else
static bool load_engine(struct nanum *n)
{
    mp_err(n->log, "the AI engine is only available on Windows\n");
    return false;
}
static void unload_engine(struct nanum *n) {}
#endif

void nanum_shutdown(struct nanum *n)
{
    if (!n || n->shut_down)
        return;
    n->shut_down = true;

    if (n->athread_started) {
        mp_mutex_lock(&n->alock);
        n->astop = true;
        mp_cond_signal(&n->acond);
        mp_mutex_unlock(&n->alock);
        mp_thread_join(n->athread);
        n->athread_started = false;
    }
    // Joins the engine's threads; after this no callback can run.
    if (n->ai)
        n->api.destroy(n->ai);
    n->ai = NULL;
    unload_engine(n);

    mp_mutex_lock(&n->lock);
    n->wakeup = NULL;
    mp_mutex_unlock(&n->lock);

    if (n->avctx)
        avcodec_free_context(&n->avctx);
    swr_free(&n->swr);
    av_channel_layout_uninit(&n->swr_layout);
    av_freep(&n->obuf);
    mp_mutex_lock(&n->alock);
    clear_audio_queue_locked(n);
    mp_mutex_unlock(&n->alock);
}

static void nanum_destroy(void *p)
{
    struct nanum *n = p;
    nanum_shutdown(n);
    mp_mutex_destroy(&n->alock);
    mp_cond_destroy(&n->acond);
    mp_mutex_destroy(&n->lock);
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
    n->astream = -1;
    n->last_pkt_pts = MP_NOPTS_VALUE;
    mp_mutex_init(&n->lock);
    mp_mutex_init(&n->alock);
    mp_cond_init(&n->acond);
    talloc_set_destructor(n, nanum_destroy);

    if (n->opts->test_subs) {
        // Fixed test lines every 5 s for 4 h. Goes through the same entry
        // point the AI thread uses.
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

    if (n->opts->stt_model && n->opts->stt_model[0]) {
        if (!n->opts->vad_model || !n->opts->vad_model[0]) {
            mp_err(n->log, "--nanum-stt-model needs --nanum-vad-model\n");
        } else if (load_engine(n)) {
            n->ai_enabled = true;
            if (mp_thread_create(&n->athread, audio_thread, n) == 0)
                n->athread_started = true;
            else
                mp_err(n->log, "cannot start the audio thread\n");
        }
    }

    mp_info(n->log, "enabled\n");
    return n;
}

void nanum_cursor_reset(struct nanum_cursor *c, double from)
{
    c->from = from;
    c->last_pts = -INFINITY;
}

void nanum_notify_eof(struct nanum *n)
{
    if (!n->athread_started)
        return;
    mp_mutex_lock(&n->alock);
    n->aeof = true;
    mp_cond_signal(&n->acond);
    mp_mutex_unlock(&n->alock);
}

void nanum_notify_seek(struct nanum *n)
{
    if (!n->athread_started)
        return;
    mp_mutex_lock(&n->alock);
    clear_audio_queue_locked(n);
    n->aflush = true;
    mp_cond_signal(&n->acond);
    mp_mutex_unlock(&n->alock);
}

struct sh_stream *nanum_alloc_stream(struct nanum *n)
{
    struct sh_stream *sh = demux_alloc_sh_stream(STREAM_SUB);
    sh->demuxer_id = 0x4e414e;      // "NAN": not a real container id
    sh->title = talloc_strdup(sh, "ko (AI)");
    sh->lang = talloc_strdup(sh, "kor");
    sh->codec->codec = "ass";
    sh->codec->extradata = talloc_memdup(sh->codec, (void *)ass_header,
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

    if (!n->athread_started || atomic_load(&n->ai_failed))
        return;

    // Analyse the first audio stream we see.
    if (n->astream < 0) {
        n->astream = src->index;
        n->acodec = src->codec;
    }
    if (src->index != n->astream || pkt->pts == MP_NOPTS_VALUE)
        return;

    // This runs with the demux lock held: only copy and queue.
    struct apkt *a = talloc_zero(NULL, struct apkt);
    a->dp = demux_copy_packet(NULL, (struct demux_packet *)pkt);
    if (!a->dp) {
        talloc_free(a);
        return;
    }

    mp_mutex_lock(&n->alock);
    if (n->abytes + a->dp->len > MAX_QUEUED_BYTES) {
        // The worker fell far behind: drop the backlog, the engine treats the
        // gap like a seek.
        clear_audio_queue_locked(n);
        n->aflush = true;
    }
    if (n->atail)
        n->atail->next = a;
    else
        n->ahead = a;
    n->atail = a;
    n->abytes += a->dp->len;
    mp_cond_signal(&n->acond);
    mp_mutex_unlock(&n->alock);
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
