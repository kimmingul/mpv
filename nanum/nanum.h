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
#include <stddef.h>

struct mp_log;
struct mpv_global;
struct sh_stream;
struct demux_packet;
struct demux_packet_pool;
struct nanum;

struct m_sub_options;
extern const struct m_sub_options nanum_conf;

// The AI inputs: the primary one follows the playback/pinned audio track (or a text
// subtitle); the secondary one translates the original-language audio track of a media
// with several audio languages; the PGS one reads a disc image subtitle track by OCR; the
// import one shows a subtitle file the user provided. Each has its own virtual subtitle
// stream.
#define NANUM_INPUTS 4
enum { NANUM_INPUT_PRIMARY, NANUM_INPUT_SECONDARY, NANUM_INPUT_PGS, NANUM_INPUT_IMPORT };
// True for the streams the demuxer delivers only to be tapped, never queued for playback
// when the player does not play them: audio, and the image subtitles the AI reads by OCR.
bool nanum_tap_only_stream(const struct sh_stream *sh);

// Snapshot of the full pre-translation state, published in the
// "demuxer-cache-state" property.
struct nanum_status {
    bool valid;                 // nanum is active for this demuxer
    const char *full_state;     // "off" | "scanning" | "translating" | "done" | "error"
    double full_progress;       // 0..1
    char full_file[1024];       // finished .ass file, "" until done
    char full_error[256];       // message if full_state == "error"
    const char *input_active;   // "stt" | "sub"
    bool text_complete;        // entire external text input has finished translating

    // Media library (demuxer-cache-state "nanum-library-*").
    const char *lib_state;      // loading|none|available|using|working|error
    char lib_source_id[40];     // opaque identity of the AI input, "" if none
    char lib_source_lang[16];   // normalized language of that input
    bool lib_has_source;
    bool lib_has_translation;
    char lib_summary[256];      // human readable: language/model/date of the saved result
    char lib_error[256];

    // AI engine of this file (demuxer-cache-state "nanum-ai-*").
    const char *ai_state;       // "ready" | "unavailable" | "failed"
    char ai_backend[16];        // "cpu" | "cuda" | "" while unavailable
    char ai_reason[512];        // why it is unavailable / failed, "" if ready
    char ai_notice[768];        // non-fatal: why CUDA is not used (auto fell back to CPU)

    // Virtual subtitle streams and the secondary (original-language) input.
    int track_primary;          // demuxer src-id of the primary virtual stream
    int track_secondary;        // src-id of the secondary stream, -1 if none
    const char *sec_state;      // "off" | "waiting" | "running" | "done" | "unavailable"
    char sec_lang[16];          // ISO 639-1, "" if none
    bool sec_ready_valid;
    double sec_ready;           // the secondary input has analysed up to here
    char sec_reason[256];

    // AI profile of the work (demuxer-cache-state "nanum-profile-*", "nanum-glossary*").
    const char *profile_state;  // "off" | "running" | "ready" | "error"
    char profile_work[256];
    char original_lang[16];     // ISO 639-1, "" if unknown
    char glossary[16384];       // lines "source => target"
    int glossary_count;

    // Retranslation of the existing cues after the glossary changed
    // (demuxer-cache-state "nanum-glossary-apply-*").
    const char *gl_state;       // "idle" | "running" | "done" | "error"
    double gl_progress;         // 0..1
    int gl_changed;             // cues whose translation changed
    char gl_error[256];

    // Disc image subtitles read by OCR (demuxer-cache-state "nanum-pgs-*", "nanum-track-pgs").
    int track_pgs;              // src-id of the virtual stream, -1 if none
    const char *pgs_state;      // "off" | "waiting" | "running" | "done" | "unavailable"
    char pgs_lang[16];          // ISO 639-1 of the chosen image subtitle track, ""
    char pgs_reason[256];
    bool pgs_ready_valid;
    double pgs_ready;           // the track has been read up to here

    // User-provided subtitle file (demuxer-cache-state "nanum-import-*", "nanum-track-import").
    int track_import;           // src-id of the virtual stream, -1 if none
    const char *imp_state;      // "off" | "aligning" | "translating" | "done" | "error"
    double imp_progress;        // 0..1
    char imp_error[256];
    char imp_note[256];         // status in words (e.g. why the alignment is not trusted)
    double imp_offset;          // seconds added to the file's times (alignment + adjustments)
    double imp_scale;           // factor applied to the file's times before the offset
    double imp_confidence;      // 0..1
    bool imp_low_confidence;    // the alignment was not trusted: offset kept 0
    char imp_lang[16];          // ISO 639-1 of the file
    char imp_origin[64];
    int imp_lines;
};

// Control chunk of the virtual subtitle stream: tells the ASS decoder to drop
// everything it has shown so far (the display presentation changed and the
// queued events are re-injected). Never a dialogue line.
#define NANUM_FLUSH_CHUNK "\x01NANUM-FLUSH\x01"

// Per-demuxer, per-input injection cursor, owned by demux.c (guarded by the demux
// lock).
struct nanum_cursor {
    int input;          // NANUM_INPUT_*
    double from;        // earliest time still relevant (MP_NOPTS_VALUE: unset)
    double last_pts;    // pts of the last injected event
    unsigned view_gen;  // presentation generation the queued packets belong to
    bool flush_pending; // NANUM_FLUSH_CHUNK must precede the next event
};

// The video id of a YouTube url (watch?v=, youtu.be/, /shorts/, /live/, /embed/, /v/; the
// ytdl:// prefix is accepted); false if the url is not one. id: 11 characters + NUL.
bool nanum_youtube_id(const char *url, char *id, size_t sz);

// Returns NULL if --nanum is not enabled.
struct nanum *nanum_create(void *parent, struct mpv_global *global,
                           struct mp_log *log);
// Called from the AI thread (and other non-demux threads) whenever new events
// are available; must be cheap. Set once, before the engine runs.
void nanum_set_wakeup(struct nanum *n, void (*cb)(void *ctx), void *ctx);

// The demuxer is about to seek to 'pts' (demuxer timestamps): queued audio is obsolete.
void nanum_notify_seek(struct nanum *n, double pts);

// The demuxer reached the end of the stream: analyse what is still buffered.
void nanum_notify_eof(struct nanum *n);

// Seconds up to which the primary AI input has recognised and translated the audio
// (saved results count), or MP_NOPTS_VALUE if unknown (no engine, or right after a
// seek into audio that has not been analysed).
double nanum_ready_until(struct nanum *n);

// Stops all threads and the AI engine. Must be called without the demux lock
// held and before the demuxer's lock is destroyed. Idempotent.
void nanum_shutdown(struct nanum *n);


// Initializes the cursor of an input; the cursor stays bound to it.
void nanum_cursor_init(struct nanum_cursor *c, int input);
// Resets the cursor after a demuxer seek and emits an ASS flush before replaying
// retained cues. 'from' is the seek target; MP_NOPTS_VALUE initializes a new cursor.
void nanum_cursor_reset(struct nanum_cursor *c, double from);

// Allocates the virtual ASS subtitle stream of an input. The caller adds it with
// demux_add_sh_stream().
struct sh_stream *nanum_alloc_stream(struct nanum *n, int input);
// A virtual stream created later (the secondary input once its language is known) that
// the demuxer must add now, or NULL. *input is the input it belongs to.
struct sh_stream *nanum_pending_stream(struct nanum *n, int *input);
// Changes whenever the title of a virtual stream changed (the track list is stale).
unsigned nanum_title_gen(struct nanum *n);

// Called from the demux thread for every packet read from the real demuxer,
// before it is queued. Of the audio streams only the AI inputs (the streams
// nanum_wants_stream() accepts) are analysed; the others are ignored. Must not
// block and must not keep a reference to pkt.
void nanum_tap(struct nanum *n, const struct sh_stream *src,
               const struct demux_packet *pkt);

// The player selected this audio stream for playback (demux lock held). It becomes the
// primary AI input unless one is pinned, and the audio streams that may become the secondary
// input are delivered from now on (nanum_wants_stream()); the demuxer then calls
// its selection sync, so the audio the demuxer reads before the ctl thread has decided
// which stream is the secondary input is kept for it.
void nanum_audio_selected(struct nanum *n, const struct sh_stream *sh);

// Returns the next subtitle packet of the cursor's input that lies in the already
// demuxed window (pts < to) and is still relevant (ends after 'from'), or NULL.
struct demux_packet *nanum_next_packet(struct nanum *n,
                                       struct demux_packet_pool *pool,
                                       int stream_index,
                                       struct nanum_cursor *c,
                                       double from, double to);

// Thread-safe. Inserts a finished cue (source text, translation or NULL) of an input.
// Speech-recognition inputs drop or replace re-analysed cues (see add_cue_locked()).
// The presentation (nanum-display) is applied when the cue is injected, so
// switching it needs no new engine work. Entry point for the AI pipeline.
void nanum_add_cue(struct nanum *n, int input, double pts, double duration,
                   const char *src, const char *tr);

// The presentation changed, or cues of the input were replaced, since 'c' last saw it:
// the demuxer must drop the virtual stream's queued packets, then call
// nanum_cursor_flush().
bool nanum_view_pending(struct nanum *n, const struct nanum_cursor *c);
// Re-injects from 'from' on, starting with a NANUM_FLUSH_CHUNK packet.
void nanum_cursor_flush(struct nanum *n, struct nanum_cursor *c, double from);

// Called with the demux lock held for every stream as it is added, so the
// stream's id/language/codec are known before its packets arrive.
void nanum_register_stream(struct nanum *n, const struct sh_stream *sh);
// Identity of the disc/title being played (stream metadata), NULL if the
// source is not a disc. Called once after the demuxer was opened.
void nanum_set_disc_identity(struct nanum *n, const char *identity);
// Names the work for the AI profile: the disc name and the container's TITLE tag
// (either may be NULL).
void nanum_set_work_titles(struct nanum *n, const char *disc_name, const char *meta_title);

// Media information of the top-level demuxer, used by the full scan (which
// opens its own second demuxer). Thread-safe; may be called repeatedly.
void nanum_set_media(struct nanum *n, const char *url, double duration);
void nanum_set_duration(struct nanum *n, double duration);
// Offset the player adds to demuxer timestamps (see demux_set_ts_offset()).
void nanum_set_ts_offset(struct nanum *n, double offset);
// Current playback position in demuxer timestamps (before the ts offset). Returns
// true if the demuxer should inject again (a cue held back for the engine is due).
bool nanum_set_playhead(struct nanum *n, double pts);

// True if the demuxer must deliver packets of this stream even if the player
// did not select it (the text subtitle track and the pinned audio tracks used
// as AI input). An audio input is one stream: several streams can share a
// demuxer id (a Blu-ray TrueHD PID and its AC-3 core), the first announced wins.
bool nanum_wants_stream(struct nanum *n, const struct sh_stream *sh);
// Called (from a nanum thread, without any nanum lock held) when the set of
// wanted streams may have changed. Also runs once right after being set.
void nanum_set_select_cb(struct nanum *n, void (*cb)(void *ctx), void *ctx);
// Consumes the request to feed the AI input of this stream the audio packets the player
// already buffered (a newly started input). Called by the selection callback with the
// demux lock held.
bool nanum_take_audio_replay(struct nanum *n, const struct sh_stream *sh);

void nanum_get_status(struct nanum *n, struct nanum_status *st);

// ---------------------------------------------------------------------------
// Playback-free subtitle generation (nanum_generate(), driven by the commands of
// osdep/nanum-generate-win.c). No struct nanum and no player state: the job opens its own demuxer
// on the media, reads the one stream it needs at full speed (nothing is rendered or played),
// and hands the finished subtitle files to the caller.

// Where the source subtitle comes from.
enum nanum_gen_source {
    NANUM_GEN_STT,      // speech recognition of an audio track
    NANUM_GEN_TEXT,     // a text subtitle track (or a subtitle file) of the media
    NANUM_GEN_OCR,      // an image subtitle track (PGS, DVD) read by the Windows OCR
};

struct nanum_gen_target {
    const char *code;   // ISO 639-1 ("ko"), may be empty if only the name is known
    const char *name;   // the language as the translator is told ("Korean"), may be empty
};

// A subtitle line, in media time (seconds since the start of the media).
struct nanum_gen_cue {
    double start, end;
    const char *text;   // lines separated by '\n'
};

enum nanum_gen_phase {
    NANUM_GEN_OPEN,         // opening the media
    NANUM_GEN_SCAN,         // reading the source (speech recognition / text / OCR)
    NANUM_GEN_GLOSSARY,     // asking the translator about the work
    NANUM_GEN_TRANSLATE,    // translating into targets[target]
};

enum nanum_gen_role { NANUM_GEN_ROLE_SOURCE, NANUM_GEN_ROLE_TRANSLATION };

struct nanum_gen_progress {
    int phase;              // enum nanum_gen_phase
    double fraction;        // 0..1 within the phase
    int target;             // NANUM_GEN_TRANSLATE: index into targets
    double position;        // NANUM_GEN_SCAN: media time reached
    double duration;        // length of the media, 0 if unknown
    size_t lines;           // NANUM_GEN_SCAN: lines found so far
};

struct nanum_gen_request {
    struct mpv_global *global;  // the player's settings (nanum-* options)
    struct mp_log *log;
    struct mp_cancel *cancel;   // triggered to stop the job
    const char *url;            // file path, URL, or bd://longest/E: / bd://mpls/00800/E:
    int source;                 // enum nanum_gen_source
    int track;                  // demuxer id (track-list "src-id") of the track; -1: choose
    const char *source_lang;    // ISO 639-1 of the source; NULL/"": the track's, else detected
    const struct nanum_gen_target *targets;
    int num_targets;
    const char *title;          // what the work is called (glossary), may be NULL
    const char *glossary;       // lines "source => target" to translate with, may be NULL
    double limit;               // > 0: only the first 'limit' seconds of the media (a preview)
    void *user;
    void (*progress)(void *user, const struct nanum_gen_progress *p);
    // A file is complete: write it. target: index of the target (-1: the source). false: it
    // could not be written (err: why, rendered).
    bool (*emit)(void *user, int role, int target, const char *code, const char *name,
                 const struct nanum_gen_cue *cues, size_t num, char *err, size_t err_size);
    // The outcome of a target without a file: "same-language" (the source is already in it) or
    // "failed" (msg: why). Targets that got a file are reported by emit().
    void (*target_done)(void *user, int target, const char *state, const char *msg);
};

struct nanum_gen_result {
    bool ok;                    // every requested file was written
    bool cancelled;
    char error[640];            // rendered in the interface language
    char error_code[24];        // "" or: open, no-track, engine, no-model, stt, ocr, no-text,
                                // translate, write, cancelled, internal
    char source_lang[8];        // what the source turned out to be, "" if unknown
    char warning[640];          // non-fatal problems (rendered), e.g. no glossary
    size_t source_lines;
    double duration;
    int targets_done, targets_failed;
};

// Blocking; call it from a worker thread. Safe to cancel at any time: files already
// emitted stay.
void nanum_generate(const struct nanum_gen_request *rq, struct nanum_gen_result *res);

#endif
