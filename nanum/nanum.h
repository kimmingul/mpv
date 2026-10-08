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

#ifndef MP_NANUM_H
#define MP_NANUM_H

#include <stdbool.h>

struct mp_log;
struct mpv_global;
struct sh_stream;
struct demux_packet;
struct demux_packet_pool;
struct nanum;

struct m_sub_options;
extern const struct m_sub_options nanum_conf;

// Per-demuxer injection cursor, owned by demux.c (guarded by the demux lock).
struct nanum_cursor {
    double from;        // earliest time still relevant (MP_NOPTS_VALUE: unset)
    double last_pts;    // pts of the last injected event
};

// Returns NULL if --nanum is not enabled.
struct nanum *nanum_create(void *parent, struct mpv_global *global,
                           struct mp_log *log);
// Called from the AI thread (and other non-demux threads) whenever new events
// are available; must be cheap. Set once, before the engine runs.
void nanum_set_wakeup(struct nanum *n, void (*cb)(void *ctx), void *ctx);

// The demuxer is about to seek: queued audio is obsolete.
void nanum_notify_seek(struct nanum *n);

// The demuxer reached the end of the stream: analyse what is still buffered.
void nanum_notify_eof(struct nanum *n);

// Stops all threads and the AI engine. Must be called without the demux lock
// held and before the demuxer's lock is destroyed. Idempotent.
void nanum_shutdown(struct nanum *n);


// Resets the cursor. Call after every demuxer seek. 'from' is the seek target.
void nanum_cursor_reset(struct nanum_cursor *c, double from);

// Allocates the virtual "ko (AI)" ASS subtitle stream. The caller adds it with
// demux_add_sh_stream().
struct sh_stream *nanum_alloc_stream(struct nanum *n);

// Called from the demux thread for every packet read from the real demuxer,
// before it is queued. Must not block and must not keep a reference to pkt.
void nanum_tap(struct nanum *n, const struct sh_stream *src,
               const struct demux_packet *pkt);

// Returns the next subtitle packet that lies in the already demuxed window
// (pts < to) and is still relevant (ends after 'from'), or NULL if none.
struct demux_packet *nanum_next_packet(struct nanum *n,
                                       struct demux_packet_pool *pool,
                                       int stream_index,
                                       struct nanum_cursor *c,
                                       double from, double to);

// Thread-safe. Inserts a subtitle event (ASS "Text" field content, UTF-8).
// This is the entry point for the AI pipeline thread.
void nanum_add_event(struct nanum *n, double pts, double duration,
                     const char *ass_text);

#endif
