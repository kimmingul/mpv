/*
 * NanumPlayer: AI subtitle bridge for the demuxer layer.
 *
 * Live path: audio packets seen by the demuxer are copied to a worker thread,
 * decoded to 16 kHz mono, and fed to the selected CPU/CUDA AI engine (speech-to-text +
 * translation). Alternatively (nanum-input=sub) the packets of a text subtitle
 * track are pushed as source lines. Finished lines are stored here as subtitle
 * events; demux.c injects them into the virtual ASS streams.
 *
 * Two live inputs run side by side (struct input), each with its own engine instance,
 * worker, decoder, event store and virtual subtitle stream: the PRIMARY input (the
 * playback/pinned audio track, or a text subtitle) and, for media with several audio
 * languages, the SECONDARY input that translates the original-language audio track.
 * The secondary reads the same demuxed packets (no extra disc reads) and buffers the
 * decoded audio, but only feeds its engine while the primary is well ahead of the
 * playhead or idle. After a seek the recognition may produce the same utterance again
 * with slightly different text: a cue that re-analyses audio which already produced
 * cues is dropped (or replaces them if it is clearly more complete), so one track
 * never holds overlapping speech cues (add_cue_locked()).
 *
 * The cues of the speech inputs are saved per media together with the time ranges
 * they analysed (library tables live_cue/live_range). Opening the media again shows
 * them at once, counts them as ready and does not analyse those ranges again.
 *
 * Profile: for a media with a known identity and a translation endpoint the engine
 * identifies the work (original language, glossary of names), which is saved in the
 * library (media_prop), applied to every translation and used to pick the secondary
 * input.
 *
 * Full mode (nanum-full=yes): a scan job opens a second demuxer for the same
 * media, collects the whole transcript in a second engine instance and
 * translates it in one go into <full-dir>/<key>.ass.
 *
 * Library (nanum/library.c, SQLite): scans save their source transcript first and
 * the translation after it, as revisions per media and AI input. Opening a
 * known media offers the saved material (nanum-library-action: use / retranslate
 * the saved source / regenerate / disable). Media identity is the file content
 * (size + SHA-256 of sampled content) or disc metadata from libbluray, never a
 * path or url; without an identity nothing is reused. The AI audio input is
 * pinned by demuxer id (nanum-audio-id): playing another track neither restarts
 * nor redirects it. An id can be shared by several streams (a Blu-ray TrueHD
 * PID and its AC-3 core are two): the first one announced is the input and the
 * others are ignored. nanum-display only changes how stored cues are rendered.
 *
 * Threads and locks (leaf locks never call out while held):
 *   demux thread   nanum_tap()                (demux lock -> cmutex -> alock; slock)
 *   audio workers  decode + engine calls      (alock only around the queue; one
 *                                              per input)
 *   ctl thread     options, jobs, restarts,   (never holds a lock while it
 *                  library, profile           takes the demux lock)
 *   scan/ext job   second demuxer + engine    (no demux lock of the player)
 *   profile job    one blocking LLM query     (own engine handle, never joined)
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation;
 * either version 2 of the License, or (at your option)
 * any later version.
 */

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

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
#include "misc/bstr.h"
#include "misc/json.h"
#include "misc/node.h"
#include <mpv/client.h>
#include "misc/path_utils.h"
#include "misc/thread_tools.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/m_option.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "osdep/nanum-accel.h"
#include "stream/stream.h"
#include "nanum/nanum.h"
#include "nanum/nanum_ai_api.h"
#include "nanum/library.h"

#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif
#include <stdlib.h>

enum { INPUT_STT, INPUT_SUB };
enum { ENGINE_OLLAMA, ENGINE_OMP };
// How cues are presented (nanum-display). Never part of any cache identity.
enum { DISP_TRANSLATION, DISP_BILINGUAL, DISP_SOURCE };
// nanum-library-action
enum { LA_ASK, LA_USE, LA_RETRANSLATE, LA_REGENERATE, LA_DISABLE };
// nanum-translate-style (the engine owns the wording of the presets)
enum { STYLE_NATURAL, STYLE_POLITE, STYLE_CASUAL, STYLE_LITERAL, STYLE_CUSTOM };
static const char *const style_names[] = {
    [STYLE_NATURAL] = "natural", [STYLE_POLITE] = "polite", [STYLE_CASUAL] = "casual",
    [STYLE_LITERAL] = "literal", [STYLE_CUSTOM] = "custom",
};

#define IN_PRI NANUM_INPUT_PRIMARY
#define IN_SEC NANUM_INPUT_SECONDARY
#define IN_PGS NANUM_INPUT_PGS
#define IN_IMP NANUM_INPUT_IMPORT

#define OPT_BASE_STRUCT struct nanum_opts
struct nanum_opts {
    bool enable;
    bool test_subs;
    char *ai_dll;
    int acceleration;       // enum nanum_accel_mode
    char *stt_model;
    char *vad_model;
    char *language;
    char *tr_endpoint;
    char *tr_model;
    char *target_language;
    char *cache;
    char *stt_model_multi;
    char *lib_db;
    // Runtime-changeable (read through an m_config_cache by the ctl thread).
    int input;
    int sub_id;
    char *sub_file;
    bool full;
    char *full_dir;
    int full_engine;
    char *omp_model;
    char *omp_command;
    int display;
    int audio_id;
    int lib_action;
    bool lib_auto_use;
    int style;
    char *style_text;
    bool secondary;
    char *glossary_req;
    bool pgs;
    char *import_req;       // "<abs path>|<lang or empty>|<origin label>"
    double import_adjust;   // seconds to add to the import track's offset (one-shot request)
    char *origin_url;       // the url the user asked for (set per file for ytdl urls)
};

const struct m_sub_options nanum_conf = {
    .opts = (const m_option_t[]) {
        {"nanum", OPT_BOOL(enable)},
        {"nanum-test-subs", OPT_BOOL(test_subs)},
        {"nanum-ai-dll", OPT_STRING(ai_dll)},
        {"nanum-acceleration", OPT_CHOICE(acceleration, {"auto", NANUM_ACCEL_AUTO},
                                          {"cpu", NANUM_ACCEL_CPU},
                                          {"cuda", NANUM_ACCEL_CUDA})},
        {"nanum-stt-model", OPT_STRING(stt_model)},
        {"nanum-vad-model", OPT_STRING(vad_model)},
        {"nanum-language", OPT_STRING(language)},
        {"nanum-translate-endpoint", OPT_STRING(tr_endpoint)},
        {"nanum-translate-model", OPT_STRING(tr_model)},
        {"nanum-target-language", OPT_STRING(target_language)},
        {"nanum-cache", OPT_STRING(cache)},
        {"nanum-stt-model-multilingual", OPT_STRING(stt_model_multi)},
        {"nanum-library-db", OPT_STRING(lib_db)},
        {"nanum-input", OPT_CHOICE(input, {"stt", INPUT_STT}, {"sub", INPUT_SUB})},
        {"nanum-sub-id", OPT_INT(sub_id)},
        {"nanum-sub-file", OPT_STRING(sub_file)},
        {"nanum-full", OPT_BOOL(full)},
        {"nanum-full-dir", OPT_STRING(full_dir)},
        {"nanum-full-engine", OPT_CHOICE(full_engine, {"ollama", ENGINE_OLLAMA},
                                         {"omp", ENGINE_OMP})},
        {"nanum-omp-model", OPT_STRING(omp_model)},
        {"nanum-omp-command", OPT_STRING(omp_command)},
        {"nanum-display", OPT_CHOICE(display, {"translation", DISP_TRANSLATION},
                                     {"bilingual", DISP_BILINGUAL}, {"source", DISP_SOURCE})},
        {"nanum-audio-id", OPT_INT(audio_id)},
        {"nanum-library-action", OPT_CHOICE(lib_action, {"ask", LA_ASK}, {"use", LA_USE},
                                            {"retranslate", LA_RETRANSLATE},
                                            {"regenerate", LA_REGENERATE},
                                            {"disable", LA_DISABLE})},
        {"nanum-library-auto-use", OPT_BOOL(lib_auto_use)},
        {"nanum-translate-style", OPT_CHOICE(style, {"natural", STYLE_NATURAL},
                                             {"polite", STYLE_POLITE},
                                             {"casual", STYLE_CASUAL},
                                             {"literal", STYLE_LITERAL},
                                             {"custom", STYLE_CUSTOM})},
        {"nanum-translate-style-text", OPT_STRING(style_text)},
        {"nanum-secondary", OPT_BOOL(secondary)},
        {"nanum-glossary-request", OPT_STRING(glossary_req)},
        {"nanum-pgs", OPT_BOOL(pgs)},
        {"nanum-import-sub", OPT_STRING(import_req)},
        {"nanum-import-adjust", OPT_DOUBLE(import_adjust)},
        {"nanum-origin-url", OPT_STRING(origin_url)},
        {0}
    },
    .size = sizeof(struct nanum_opts),
    .defaults = &(const struct nanum_opts){
        .ai_dll = "",               // "": the managed CPU / CUDA engines (osdep/nanum-accel-win.c)
        .language = "auto",         // follow the audio track's language
        .tr_endpoint = "http://127.0.0.1:11434",
        .tr_model = "gemma4:latest",
        .target_language = "Korean",
        .sub_id = -1,
        .omp_command = "omp",
        .audio_id = -1,
        .secondary = true,
        .pgs = true,
    },
};

#define STT_RATE 16000
// Audio the worker may fall behind by before the backlog is dropped. The demuxer
// reads far ahead of the player (demuxer-readahead-secs): two minutes of lossless
// 5.1 audio, read in a burst, are about 32 MB.
#define MAX_QUEUED_BYTES (128 * 1024 * 1024)
// Decoded audio the secondary input keeps while it waits for the primary: 15 minutes
// of 16 kHz mono floats are about 58 MB (the demuxer reads at most a couple of minutes
// ahead of the playhead). Beyond that new audio is dropped, see pcm_enqueue().
#define MAX_PCM_SAMPLES ((size_t)15 * 60 * STT_RATE)
// Compressed audio of the secondary candidates the demuxer delivered before the ctl thread
// decided which stream is the secondary input (all candidates together).
#define MAX_CAND_BYTES (96 * 1024 * 1024)
// The secondary input only works while the primary has analysed this far ahead of the
// playhead (or is idle).
#define SEC_LEAD_SECS 30.0
// Audio the secondary may have pushed into its engine beyond what the engine finished:
// bounds how long a primary that needs the engine again has to wait for it.
#define SEC_ENGINE_AHEAD 45.0
// Image subtitles: a display set is read this long before its display time at the latest
// relative to the demuxer position, so coverage trails the position by this much.
#define PGS_LAG 1.5
// Seconds between position markers the demuxer thread queues for the image subtitle worker.
#define PGS_MARK_STEP 1.0
// Longest an image subtitle without a closing display set stays on screen.
#define PGS_MAX_DUR 8.0
// Analysed ranges closer together than this are one range (audio frames are not
// perfectly contiguous).
#define COV_JOIN 0.5
// The engine delivers every line before processed_until(); this much stays uncovered
// below it for safety.
#define COV_MARGIN 0.3
// A playhead this close before a range counts as inside it (seeks land near audio).
#define COV_REF_TOL 2.0
#define SAVE_INTERVAL_S 30
// sh_stream.demuxer_id of the virtual tracks: "NAN" and the next one. Not real
// container ids.
#define ASS_SOURCE_ID 0x4e414e

static int virtual_id(int input)
{
    return ASS_SOURCE_ID + input;
}

static bool is_virtual_id(int id)
{
    return id >= ASS_SOURCE_ID && id < ASS_SOURCE_ID + NANUM_INPUTS;
}

// Identity of a track inside its container. demuxer_id (track-list "src-id") is
// only set by demuxers that have stream ids; WAV and the like have none, so the
// stream's position in the container stands in for it (the scan's second demuxer
// opens the same container and numbers the streams the same way).
#define NO_ID_BASE 0x40000000
static int stream_key(const struct sh_stream *sh)
{
    return sh->demuxer_id >= 0 ? sh->demuxer_id : NO_ID_BASE + sh->index;
}

// How the live path is fed (effective, derived from the options).
enum { MODE_STT, MODE_SUB_EMB, MODE_SUB_EXT };
enum { FS_OFF, FS_SCANNING, FS_TRANSLATING, FS_DONE, FS_ERROR };
static const char *const fs_names[] = {
    [FS_OFF] = "off", [FS_SCANNING] = "scanning",
    [FS_TRANSLATING] = "translating", [FS_DONE] = "done", [FS_ERROR] = "error",
};
enum { LS_LOADING, LS_NONE, LS_AVAILABLE, LS_USING, LS_WORKING, LS_ERROR };
static const char *const ls_names[] = {
    [LS_LOADING] = "loading", [LS_NONE] = "none", [LS_AVAILABLE] = "available",
    [LS_USING] = "using", [LS_WORKING] = "working", [LS_ERROR] = "error",
};
// State of the AI engine of this file (demuxer-cache-state "nanum-ai-*").
enum { AI_UNAVAILABLE, AI_READY, AI_FAILED };
static const char *const ai_names[] = {
    [AI_UNAVAILABLE] = "unavailable", [AI_READY] = "ready", [AI_FAILED] = "failed",
};
// Secondary input (demuxer-cache-state "nanum-secondary-state").
enum { SS_OFF, SS_WAITING, SS_RUNNING, SS_DONE, SS_UNAVAILABLE };
static const char *const ss_names[] = {
    [SS_OFF] = "off", [SS_WAITING] = "waiting", [SS_RUNNING] = "running",
    [SS_DONE] = "done", [SS_UNAVAILABLE] = "unavailable",
};
// User-provided subtitle file (demuxer-cache-state "nanum-import-state").
enum { IS_OFF, IS_ALIGNING, IS_TRANSLATING, IS_DONE, IS_ERROR };
static const char *const is_names[] = {
    [IS_OFF] = "off", [IS_ALIGNING] = "aligning", [IS_TRANSLATING] = "translating",
    [IS_DONE] = "done", [IS_ERROR] = "error",
};
// Retranslation after a glossary edit (demuxer-cache-state "nanum-glossary-apply-state").
enum { GS_IDLE, GS_RUNNING, GS_DONE, GS_ERROR };
static const char *const gs_names[] = {
    [GS_IDLE] = "idle", [GS_RUNNING] = "running", [GS_DONE] = "done", [GS_ERROR] = "error",
};
// AI profile (demuxer-cache-state "nanum-profile-state").
enum { PS_OFF, PS_RUNNING, PS_READY, PS_ERROR };
static const char *const ps_names[] = {
    [PS_OFF] = "off", [PS_RUNNING] = "running", [PS_READY] = "ready", [PS_ERROR] = "error",
};

// Snapshot of the runtime-changeable options (owned by whoever holds it).
struct rt_cfg {
    int input;
    int sub_id;
    char *sub_file;     // "" = none
    bool full;
    char *full_dir;     // "" = default
    int engine;
    char *omp_model;
    char *omp_command;
    int display;
    int audio_id;       // demuxer id the AI audio input is pinned to, -1: follow the first
    int lib_action;
    bool lib_auto_use;
    int style;
    char *style_text;   // "" = none
    bool secondary;
    char *glossary_req; // "" = none
    bool pgs;
    char *import_req;   // "" = none
    double import_adjust;
};

// A finished cue as the AI pipeline delivered it. The presentation is applied
// when the cue is injected, so it can change without new engine work.
struct nanum_event {
    double pts;
    double duration;
    int id;             // stable ASS ReadOrder base; libass drops duplicates by it
    char *src;
    char *tr;           // NULL: no translation (yet / failed)
    double inj_end;     // end time of the last injection (0: not injected yet)
    double raw_pts;     // import: time in the subtitle file (pts = raw_pts * scale + offset)
    int64_t born;       // mp_time_ns() when the cue entered the store
};

// A time range in demuxer timestamps.
struct range {
    double a, b;
};

// Item of the live worker queue: an audio packet, an image subtitle packet, a position
// marker of the demuxer (PGS) or a subtitle line.
struct apkt {
    struct apkt *next;
    struct demux_packet *dp;    // audio / image subtitle
    char *text;                 // subtitle line (dp == NULL)
    double pts, dur;
    size_t size;
    int img;                    // 1: image subtitle packet (dp), 2: marker (pts)
    const struct mp_codec_params *codec;    // img == 1
};

// Decoded audio of the secondary input that waits for the primary.
struct pcm_chunk {
    struct pcm_chunk *next;
    double pts;
    int n;
    float s[];
};

// Audio decoder + resampler to the engine's input format. One per engine.
struct adec {
    struct mp_log *log;
    const struct nanum_ai_api *api;
    struct nanum_ai *ai;
    // Receives the decoded 16 kHz mono audio; NULL: pushed straight into 'ai'.
    void (*sink)(void *user, const float *pcm, int n, double pts);
    void *sink_user;
    const struct mp_codec_params *codec;
    AVCodecContext *avctx;
    SwrContext *swr;
    int swr_rate, swr_fmt;
    AVChannelLayout swr_layout;
    float *obuf;
    int obuf_cap;
    double last_pkt_pts;
};

// What a job needs to know about the media/AI input it works for, and where
// its results go. save == false: no usable identity, nothing is stored or reused.
struct libsrc {
    bool save;
    int64_t media_id;
    int64_t source_id;
    char *media_key;            // media identity, e.g. "file|<sha256>"
    char *source_key;
    char *input;                // "stt" | "sub"
    int track_id;
    char lang[16];              // normalized language of the source, "und" if unknown
    char engine[64];            // what produced the source (STT model / "subtitle")
    char target[64];
    char tr_engine[16];         // "ollama" | "omp"
    char tr_model[128];
    int64_t source_rev;         // JOB_RETR: the saved source revision to translate
    double origin;
    char warn[200];             // why results are not saved ("" if they are)
};

enum { JOB_SCAN, JOB_EXT, JOB_RETR, JOB_GLOSS, JOB_IMPORT };

// Background job with its own demuxer/engine: the full scan, a retranslation of
// a saved transcript, or loading an external subtitle file into the live engine.
struct job {
    struct nanum *n;
    int kind;
    mp_thread thread;
    atomic_bool cancel;
    atomic_bool finished;
    struct mp_cancel *mpcancel;
    struct rt_cfg *cfg;
    char *url;                  // media url
    char *dir;                  // output directory
    double duration;
    int a_srcid;                // audio stream (demuxer id) the scan reads
    struct libsrc ls;
    int64_t job_row;

    // Result of nanum_ai_translate_all() (written by the engine's thread).
    mp_mutex dlock;
    mp_cond dcond;
    bool done;
    char *err;
    struct nanum_ai_line *lines;
    size_t nlines;

    // Source transcript (export of the scan / loaded from the library).
    struct lib_cue *src;
    size_t nsrc;

    // JOB_GLOSS: what to translate again (see gloss_run()). The job loads its own
    // engine handle, so it does not depend on the live engine.
    struct nanum_engine eng;
    struct nanum_ai_api api;
    char *gl_target;            // the glossary the cues are translated with
    char gl_err[320];

    // JOB_IMPORT: the request and its result (see imp_run()).
    char *imp_path, *imp_origin;
    char imp_lang[16];          // requested language, "" = detect; the result's language
    char imp_enc[24];           // detected encoding
    char imp_hash[17];          // identity of the file's content
    struct lib_cue *imp_cues;   // result: file times, tr = translation ("" failed)
    size_t nimp;
    char imp_err[256];
};

// Stream facts recorded when the demuxer announces a stream.
struct sinfo {
    int index;
    int id;                     // stream_key(): sh_stream.demuxer_id, or a stand-in
    int type;
    bool def;                   // marked as the default track
    bool image;                 // an image subtitle track the AI can read (PGS / DVD)
    bool forced;
    char lang[8];
    char codec[24];
};

struct nanum;

// One live AI input: its event store, virtual subtitle stream, audio worker with its
// own engine instance, and the library binding of its results.
struct input {
    struct nanum *n;
    int idx;                    // IN_PRI | IN_SEC | IN_PGS | IN_IMP

    // Event store, sorted by pts, and the time ranges the input has analysed
    // (guarded by nanum.lock).
    struct nanum_event *events;
    int num_events;
    int next_id;
    struct range *cov;          // sorted, disjoint
    int ncov;
    double inj_pts;             // pts of the last event the demuxer injected
    bool dirty;                 // events or ranges changed since they were saved
    atomic_uint gen;            // changes when injected events must be injected again
    atomic_bool events_seen;    // the live path produced a line at least once
    atomic_bool held;           // injection waits for the engine (see event_settled())
    _Atomic double held_pts;

    // Virtual subtitle stream (nanum_alloc_stream())
    struct sh_stream *sh;
    bool sh_requested;          // demux thread only

    // AI engine and its audio worker
    struct nanum_ai *ai;
    mp_thread athread;
    atomic_bool athread_started;
    atomic_bool ai_failed;
    atomic_bool active;         // the worker is (about to be) running
    mp_mutex alock;             // guards the worker queue and the flags below
    mp_cond acond;
    struct apkt *ahead, *atail;
    size_t abytes;
    bool astop;
    bool aflush;                // decoder must be flushed (seek)
    bool areset;                // engine must drop everything (input changed)
    bool aswitch;               // audio track changed: reinit decoder/language
    const struct mp_codec_params *acodec_next;
    char alang[8];              // ISO 639-1 code of the track, or "auto"
    bool follow_track;          // language comes from the audio track
    bool aeof;                  // demuxer reached EOF: flush the engine
    atomic_bool reset_pending;  // areset not carried out by the worker yet
    _Atomic double ready_until; // analysed up to here at the playhead, or MP_NOPTS_VALUE
    _Atomic double engine_until;// processed_until() of the engine, or MP_NOPTS_VALUE
    atomic_bool idle;           // nothing queued and the engine has nothing to do
    atomic_bool done;           // demuxer EOF reached and everything analysed
    atomic_bool replay_audio;   // a newly selected input needs cached audio too
    atomic_bool save_now;       // a run finished: save the results soon
    atomic_int pin_id;          // demuxer id of the audio input, -1: none yet
    atomic_int pin_idx;         // sh_stream.index the pin resolves to, -1: not announced
    int astream;                // audio stream index being analysed (demux thread)

    // Text input (primary only)
    atomic_bool text_eof;       // external subtitle file has queued every cue
    atomic_bool text_complete;  // every queued text cue has finished translating
    atomic_bool rebind;         // tap: re-announce the audio stream
    atomic_bool rebind_sub;     // tap: re-announce the subtitle language
    int sub_lang_idx;           // subtitle stream whose language was announced
    uint64_t *seen;             // hashes of subtitle lines already queued
    size_t nseen;
    void *seen_ctx;

    // Worker-owned
    struct adec dec;
    unsigned tr_applied;        // translation settings generation the engine has
    double last_playhead;
    int64_t next_progress;      // throttles the progress poll
    int64_t idle_since;
    bool run_open;              // audio is being pushed to the engine: [run_a, run_b]
    double run_a, run_b;
    struct pcm_chunk *pcm_head, *pcm_tail;      // secondary backlog
    size_t pcm_samples;
    bool pcm_full;              // the backlog is full: new audio is being dropped

    // Image subtitles (IN_PGS; worker-owned except where noted)
    AVCodecContext *vctx;       // libavcodec subtitle decoder
    const struct mp_codec_params *vcodec;
    bool pgs_pending;           // a decoded subtitle waits for the display set that ends it
    double pgs_start;
    char *pgs_text;
    int ocr_fail;               // consecutive OCR failures
    char ocr_lang[8];           // the OCR language used (may differ from lang)
    double mark_last;           // demux thread: position of the last marker queued

    // The source language of the cues (ISO 639-1, "" if unknown) and, for IN_IMP, what the
    // library stores them under.
    char lang[16];

    // Library binding (the ctl thread; key under slock)
    atomic_bool bound;          // the library was consulted: audio may flow
    bool persist;
    int64_t media_id;
    char *key;
};

struct nanum {
    struct mp_log *log;
    struct mpv_global *global;
    struct nanum_opts *opts;    // start-up snapshot (static options)

    // Event stores of both inputs.
    mp_mutex lock;
    void (*wakeup)(void *ctx);
    void *wakeup_ctx;

    // Presentation. Changing it bumps every input's gen; the demuxer re-injects.
    atomic_int view;
    bool tr_enabled;            // a translation endpoint is configured

    // Tap statistics (demux thread only).
    int64_t audio_packets;
    double next_log_pts;

    // AI engine DLL (shared) and the inputs.
    atomic_bool ai_enabled;
    bool shut_down;
    struct nanum_engine engine;     // loaded engine DLL and the runtimes it needs
    struct nanum_ai_api api;
    struct input in[NANUM_INPUTS];
    atomic_bool stream_wanted[NANUM_INPUTS];    // the demuxer must create this input's stream
    atomic_bool pgs_enabled;        // nanum-pgs
    atomic_bool pgs_wanted;         // an image subtitle stream is the OCR input
    double read_pos;                // demux thread: demuxer position of the last packet

    // Audio streams that may become the secondary input. Their packets are kept until the
    // ctl thread has decided which one it is (it needs the profile and the language of the
    // playback input), so the audio the demuxer reads meanwhile is not lost. Streams
    // already selected for playback are only tapped; the others are delivered for this
    // purpose (nanum_wants_stream()).
    _Atomic uint64_t cand_mask;     // bit i: stream index i is a candidate
    atomic_bool audio_played;       // the player selected an audio stream
    atomic_bool sec_enabled;        // nanum-secondary
    atomic_bool sec_dead;           // the plan has no secondary input (for now)
    atomic_bool eof_seen;           // the demuxer reached its end since the last seek
    bool multi_lang;                // the media has audio in two or more languages (slock)
    mp_mutex cmutex;                // guards cand[] and the secondary's tap (demux lock first)
    struct cand {
        int index;
        const struct sh_stream *sh;
        struct apkt *head, *tail;
        size_t bytes;
    } cand[8];
    int ncand;
    size_t cand_bytes;

    // Effective primary input and playback info (atomics: read by the demuxer).
    atomic_int a_mode;
    atomic_int a_sub_id;
    atomic_bool gen_off;        // library: a saved result is used / generation disabled
    _Atomic double playhead;
    _Atomic double duration;
    _Atomic double ts_offset;
    atomic_bool ts_known;       // demux_set_ts_offset() was called

    // Media info, full-mode and library state (slock, a leaf lock).
    mp_mutex slock;
    int fstate;
    double fprogress;
    char ffile[1024];
    char ferror[256];
    char *url;
    char *disc_identity;        // stream-provided identity of a disc, NULL otherwise
    char *disc_name;            // disc name / container TITLE tag (work titles)
    char *meta_title;
    struct sinfo *streams;
    int nstreams;
    int lstate;
    char lsrc_id[40];
    char lsrc_lang[16];
    bool lhas_src, lhas_tr;
    char lsummary[256];
    char lerror[256];
    int ai_state;               // AI_*
    char ai_backend[16];        // "cpu" | "cuda" | ""
    char ai_reason[512];        // why the AI is unavailable / failed
    char ai_notice[768];        // non-fatal: why CUDA is not used

    // Secondary input, profile and translation settings (slock)
    int sec_state;              // SS_*
    char sec_lang[16];
    char sec_reason[256];
    int prof_state;             // PS_*
    char prof_work[256];
    char orig_lang[16];         // ISO 639-1 from the profile, ""
    char *glossary;             // lines "source => target", never NULL
    int glossary_count;
    int64_t glossary_changed;   // mp_time_ns() of the last change
    // Glossary the cues of the live tracks / of the full result were translated with; a
    // difference to 'glossary' means they are translated again (gloss_run()).
    char *gl_base;
    bool gl_base_set;
    int gl_state;               // GS_*
    double gl_progress;
    int gl_changed;
    char gl_error[256];
    int style;                  // STYLE_*
    char *style_text;
    atomic_uint tr_gen;         // changes with style/glossary: workers apply it
    char ext_lang[8];           // language of the external subtitle file

    // Image subtitles and imported subtitle file (slock)
    int pgs_state;              // SS_*
    char pgs_lang[16];
    char pgs_reason[256];
    char ocr_langs[128];        // OCR languages installed on this PC, comma separated
    bool ocr_known;
    int imp_state;              // IS_*
    double imp_progress;
    char imp_error[256];
    char imp_note[256];
    double imp_offset, imp_scale, imp_conf;
    bool imp_low;
    char imp_lang[16];
    char imp_origin[64];
    int imp_lines;
    // Titles of the virtual tracks. The player keeps the pointers of the titles it has
    // seen, so replaced ones are never freed before the demuxer goes away.
    char *titles[NANUM_INPUTS];
    char **title_pool;
    int ntitle_pool;
    atomic_uint title_gen;

    // Library and the finished result kept for re-rendering (rlock).
    struct nanum_lib *lib;
    char lib_open_err[256];
    mp_mutex rlock;
    void *dctx;                 // owns dcues and the strings below
    struct lib_cue *dcues;
    size_t ndcues;
    char *ddir, *dstem;         // output directory and file stem of the result
    char *drendered;            // file written for the current presentation
    struct libsrc dls;          // how the result is stored in the library (dls.save)
    int64_t dsource_rev;        // source revision it was translated from
    char *dgl;                  // glossary it was translated with

    // Control thread: option changes, job lifecycle.
    struct m_config_cache *cache;
    struct rt_cfg *cfg;         // initial snapshot (ctl thread takes over)
    mp_thread cthread;
    bool cthread_started;
    mp_mutex clock;
    mp_cond ccond;
    bool ctl_stop;
    bool ctl_kick;
    bool select_pending;
    void (*select_cb)(void *ctx);
    void *select_ctx;
};

#define PRI(n) (&(n)->in[IN_PRI])
#define SEC(n) (&(n)->in[IN_SEC])
#define PGS(n) (&(n)->in[IN_PGS])
#define IMPT(n) (&(n)->in[IN_IMP])

// Which image subtitle track the OCR input reads (see pgs_replan()).
struct pgs_plan {
    bool exists;                // the media has image subtitle tracks the AI could read
    bool ok;                    // one of them is usable
    int id, idx;                // demuxer id / stream index
    char lang[8];               // ISO 639-1 of the track
    char ocr[8];                // the OCR language that reads it
    char note[256];             // non-fatal remark (a substitute OCR language)
    char reason[256];           // why none is usable
};
static void pgs_replan(struct nanum *n, struct pgs_plan *out);
static const char *lang_name_ko(const char *iso1);

static void set_ai(struct nanum *n, int state, const char *backend, const char *reason,
                   const char *notice)
{
    mp_mutex_lock(&n->slock);
    n->ai_state = state;
    snprintf(n->ai_backend, sizeof(n->ai_backend), "%s", backend ? backend : "");
    snprintf(n->ai_reason, sizeof(n->ai_reason), "%s", reason ? reason : "");
    snprintf(n->ai_notice, sizeof(n->ai_notice), "%s", notice ? notice : "");
    mp_mutex_unlock(&n->slock);
}

// The engine of an input stopped working: the preroll must not wait for it any more.
// (The secondary input only loses itself; the file keeps its AI state.)
static void ai_fail(struct input *in, const char *fmt, ...) MP_PRINTF_ATTRIBUTE(2, 3);
static void ai_fail(struct input *in, const char *fmt, ...)
{
    struct nanum *n = in->n;
    char msg[sizeof(n->ai_reason)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    mp_mutex_lock(&n->slock);
    if (in->idx == IN_PRI) {
        n->ai_state = AI_FAILED;
        snprintf(n->ai_reason, sizeof(n->ai_reason), "%s", msg);
    } else if (in->idx == IN_PGS) {
        n->pgs_state = SS_UNAVAILABLE;
        snprintf(n->pgs_reason, sizeof(n->pgs_reason), "%s", msg);
    } else {
        n->sec_state = SS_UNAVAILABLE;
        snprintf(n->sec_reason, sizeof(n->sec_reason), "%s", msg);
    }
    mp_mutex_unlock(&n->slock);
    atomic_store(&in->ai_failed, true);
}

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

// Wakes the demuxer so it injects what changed. Called without any nanum lock held.
static void wake_demux(struct nanum *n)
{
    mp_mutex_lock(&n->lock);
    void (*wakeup)(void *) = n->wakeup;
    void *ctx = n->wakeup_ctx;
    mp_mutex_unlock(&n->lock);
    if (wakeup)
        wakeup(ctx);
}

static bool str_eq(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

// Inputs that read speech (the playback audio, the secondary audio): their cues are
// re-analysed after seeks, see add_cue_locked().
static bool is_stt_input(const struct input *in)
{
    return in->idx == IN_SEC ||
           (in->idx == IN_PRI && atomic_load(&in->n->a_mode) == MODE_STT);
}

// Inputs whose results come with the time ranges they have analysed.
static bool has_ranges(const struct input *in)
{
    return is_stt_input(in) || in->idx == IN_PGS;
}

// The input takes the demuxer's packets: its worker runs, or (image subtitles) packets are
// already being queued for the worker the ctl thread is about to start.
static bool input_live(const struct input *in)
{
    return atomic_load(&in->active) ||
           (in->idx == IN_PGS && atomic_load(&in->n->pgs_wanted));
}

// First index whose pts is >= / > the value (events are sorted by pts).
static int event_lower_bound(const struct input *in, double pts)
{
    int lo = 0, hi = in->num_events;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (in->events[mid].pts < pts)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int event_upper_bound(const struct input *in, double pts)
{
    int lo = 0, hi = in->num_events;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (in->events[mid].pts <= pts)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// Roughly how much is said: words, and every second Chinese/Japanese/Korean
// character (those scripts have no spaces).
static int text_units(const char *s)
{
    int words = 0, wide = 0;
    bool in_word = false;
    struct bstr str = bstr0(s);
    while (str.len) {
        struct bstr next;
        int c = bstr_decode_utf8(str, &next);
        if (c < 0) {
            c = '?';
            next = bstr_cut(str, 1);
        }
        str = next;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x3000) {
            in_word = false;
        } else if ((c >= 0x3040 && c <= 0x30ff) || (c >= 0x3400 && c <= 0x9fff) ||
                   (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff)) {
            wide++;
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    return words + (wide + 1) / 2;
}

// Inserts into the sorted store without any plausibility rule; returns the index.
static int insert_sorted_locked(struct input *in, double pts, double duration,
                                 const char *src, const char *tr)
{
    struct nanum *n = in->n;
    int lo = event_upper_bound(in, pts);
    MP_TARRAY_GROW(n, in->events, in->num_events);
    memmove(&in->events[lo + 1], &in->events[lo],
            (in->num_events - lo) * sizeof(in->events[0]));
    in->events[lo] = (struct nanum_event){
        .pts = pts,
        .duration = duration,
        .id = in->next_id++,
        .src = talloc_strdup(n, src),
        .tr = tr ? talloc_strdup(n, tr) : NULL,
        .born = mp_time_ns(),
    };
    in->num_events++;
    in->dirty = true;
    return lo;
}

static void remove_event_locked(struct input *in, int i)
{
    talloc_free(in->events[i].src);
    talloc_free(in->events[i].tr);
    memmove(&in->events[i], &in->events[i + 1],
            (in->num_events - i - 1) * sizeof(in->events[0]));
    in->num_events--;
    in->dirty = true;
}

// Two cues of a speech input that share this much speech are the same utterance:
// speech does not overlap on one track.
static bool same_speech(const struct nanum_event *e, double pts, double dur)
{
    double ov = fmin(e->pts + e->duration, pts + dur) - fmax(e->pts, pts);
    return ov >= fmax(0.1, 0.4 * fmin(e->duration, dur));
}

// The exact repeat of a text cue (src and translation) that starts within 0.25 s of the
// stored one and overlaps it: reanalysing the same line, not a second line.
static bool text_repeats(const struct nanum_event *e, double pts, double dur,
                         const char *src, const char *tr)
{
    return !strcmp(e->src, src) && str_eq(e->tr, tr) &&
           (e->pts == pts || (e->pts < pts + dur && pts < e->pts + e->duration));
}

// Decides if a finished cue enters the store. Speech inputs: after a seek (also the
// preroll's own resync seeks, or an in-cache seek) the recognition reads audio again
// that already produced cues and words it slightly differently, or cuts it differently.
// A cue that shares speech with stored ones is therefore dropped, unless it clearly
// completes them: it covers their time span within 0.35 s and says at least 1.25 times
// as much (then it replaces them and the demuxer injects the track again so the old
// events leave the screen). Text inputs legitimately overlap (two speakers); only
// exact repeats are dropped.
static bool add_cue_locked(struct input *in, double pts, double dur, const char *src,
                           const char *tr)
{
    struct nanum *n = in->n;
    bool injected_removed = false;

    if (is_stt_input(in)) {
        double d = fmax(dur, 0.05);
        double min_start = INFINITY, max_end = -INFINITY;
        int old_units = 0, overlapped = 0;
        bool old_tr = false;
        // No speech cue is longer than half a minute.
        for (int i = event_lower_bound(in, pts - 60.0);
             i < in->num_events && in->events[i].pts < pts + d; i++)
        {
            const struct nanum_event *e = &in->events[i];
            if (!same_speech(e, pts, d))
                continue;
            overlapped++;
            min_start = fmin(min_start, e->pts);
            max_end = fmax(max_end, e->pts + e->duration);
            old_units += text_units(e->src);
            old_tr |= e->tr && e->tr[0];
        }
        if (overlapped) {
            bool better = pts <= min_start + 0.35 && pts + d >= max_end - 0.35 &&
                          text_units(src) * 4 >= old_units * 5 &&
                          ((tr && tr[0]) || !old_tr);
            if (!better) {
                mp_verbose(n->log, "cue dropped (re-analysis, %d stored) %.2f +%.2f: %s\n",
                           overlapped, pts, dur, src);
                return false;
            }
            for (int i = event_upper_bound(in, pts + d); i-- > 0; ) {
                if (in->events[i].pts < pts - 60.0)
                    break;
                if (same_speech(&in->events[i], pts, d)) {
                    mp_verbose(n->log, "cue replaced %.2f +%.2f: %s\n", in->events[i].pts,
                               in->events[i].duration, in->events[i].src);
                    injected_removed |= in->events[i].inj_end > 0;
                    remove_event_locked(in, i);
                }
            }
        }
    } else {
        int lo = event_upper_bound(in, pts);
        // Reanalysis after a seek can shift the same line by tens of ms.
        for (int i = lo - 1; i >= 0 && pts - in->events[i].pts <= 0.25 + 1e-6; i--) {
            if (text_repeats(&in->events[i], pts, dur, src, tr))
                return false;
        }
        for (int i = lo; i < in->num_events && in->events[i].pts - pts <= 0.25 + 1e-6; i++) {
            if (text_repeats(&in->events[i], pts, dur, src, tr))
                return false;
        }
    }

    int at = insert_sorted_locked(in, pts, dur, src, tr);

    // Speech does not overlap on a track: where two cues of different utterances still
    // overlap a little (the recogniser's timestamps are generous), the earlier one ends
    // where the later one starts.
    if (is_stt_input(in)) {
        if (at > 0) {
            struct nanum_event *p = &in->events[at - 1];
            if (p->pts + p->duration > pts + 1e-6 && pts > p->pts + 0.05)
                p->duration = pts - p->pts;
        }
        if (at + 1 < in->num_events) {
            const struct nanum_event *s = &in->events[at + 1];
            if (pts + in->events[at].duration > s->pts + 1e-6 && s->pts > pts + 0.05)
                in->events[at].duration = s->pts - pts;
        }
    }

    // The demuxer injected from its cursor on: a cue behind it would never be seen, a
    // replaced cue is still queued, and a predecessor injected with a display end past
    // this cue's start has to be injected again with the clipped end.
    bool again = injected_removed || pts <= in->inj_pts;
    if (!again && at > 0) {
        const struct nanum_event *p = &in->events[at - 1];
        again = p->inj_end > fmax(pts, p->pts + p->duration) + 1e-3;
    }
    if (again)
        atomic_fetch_add(&in->gen, 1);
    return true;
}

void nanum_add_cue(struct nanum *n, int input, double pts, double duration, const char *src,
                   const char *tr)
{
    struct input *in = &n->in[input];
    mp_mutex_lock(&n->lock);
    bool stored = add_cue_locked(in, pts, duration, src, tr);
    mp_mutex_unlock(&n->lock);
    if (!stored)
        return;
    atomic_store(&in->events_seen, true);
    wake_demux(n);
}

// Drops every stored event and analysed range (the live input changed; they belong to
// the old one) and makes the demuxer forget the injected ones. What was saved stays.
static void events_clear(struct input *in)
{
    struct nanum *n = in->n;
    mp_mutex_lock(&n->lock);
    for (int i = 0; i < in->num_events; i++) {
        talloc_free(in->events[i].src);
        talloc_free(in->events[i].tr);
    }
    in->num_events = 0;
    in->ncov = 0;
    in->inj_pts = -INFINITY;
    in->dirty = false;
    atomic_store(&in->held, false);
    mp_mutex_unlock(&n->lock);
    atomic_fetch_add(&in->gen, 1);
    wake_demux(n);
}

// ---------------------------------------------------------------------------
// Analysed ranges

// Adds [a, b] to the analysed ranges (merging neighbours).
static void cov_add_locked(struct input *in, double a, double b)
{
    struct nanum *n = in->n;
    if (!(b > a))
        return;
    int i = 0;
    while (i < in->ncov && in->cov[i].b < a - COV_JOIN)
        i++;
    int j = i;
    double na = a, nb = b;
    while (j < in->ncov && in->cov[j].a <= b + COV_JOIN) {
        na = fmin(na, in->cov[j].a);
        nb = fmax(nb, in->cov[j].b);
        j++;
    }
    if (j == i + 1 && in->cov[i].a == na && in->cov[i].b == nb)
        return;
    if (j == i) {
        MP_TARRAY_GROW(n, in->cov, in->ncov);
        memmove(&in->cov[i + 1], &in->cov[i], (in->ncov - i) * sizeof(in->cov[0]));
        in->ncov++;
    } else if (j > i + 1) {
        memmove(&in->cov[i + 1], &in->cov[j], (in->ncov - j) * sizeof(in->cov[0]));
        in->ncov -= j - i - 1;
    }
    in->cov[i] = (struct range){na, nb};
    in->dirty = true;
}

static bool cov_has(struct input *in, double t)
{
    struct nanum *n = in->n;
    bool r = false;
    mp_mutex_lock(&n->lock);
    for (int i = 0; i < in->ncov && in->cov[i].a <= t; i++) {
        if (t <= in->cov[i].b) {
            r = true;
            break;
        }
    }
    mp_mutex_unlock(&n->lock);
    return r;
}

// Shows cues and ranges of an earlier session (library) as if they had just been
// produced. They are consistent with each other, so no re-analysis rule applies.
static void input_preload(struct input *in, const struct lib_cue *cues, size_t nc,
                          const struct lib_range *ranges, size_t nr)
{
    struct nanum *n = in->n;
    mp_mutex_lock(&n->lock);
    for (size_t i = 0; i < nc; i++)
        insert_sorted_locked(in, cues[i].pts, cues[i].dur, cues[i].src, cues[i].tr);
    for (size_t i = 0; i < nr; i++)
        cov_add_locked(in, ranges[i].a, ranges[i].b);
    in->dirty = false;
    mp_mutex_unlock(&n->lock);
    atomic_fetch_add(&in->gen, 1);
    if (nc)
        atomic_store(&in->events_seen, true);
    wake_demux(n);
}

// Replaces the translation of stored cues (by event id; glossary changes). The demuxer
// injects the track again if a replaced cue was already queued.
static int input_apply_translations(struct input *in, size_t cnt, const int *ids,
                                    const double *pts, char *const *tr)
{
    struct nanum *n = in->n;
    int changed = 0;
    bool injected = false;
    mp_mutex_lock(&n->lock);
    for (size_t k = 0; k < cnt; k++) {
        for (int i = event_lower_bound(in, pts[k] - 0.001);
             i < in->num_events && in->events[i].pts <= pts[k] + 0.001; i++)
        {
            struct nanum_event *e = &in->events[i];
            if (e->id != ids[k])
                continue;
            if (!str_eq(e->tr, tr[k])) {
                talloc_free(e->tr);
                e->tr = talloc_strdup(n, tr[k]);
                injected |= e->inj_end > 0;
                in->dirty = true;
                changed++;
            }
            break;
        }
    }
    mp_mutex_unlock(&n->lock);
    if (injected)
        atomic_fetch_add(&in->gen, 1);
    if (changed) {
        atomic_store(&in->save_now, true);
        wake_demux(n);
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Runtime options

static struct rt_cfg *rt_read(const struct nanum_opts *o)
{
    struct rt_cfg *c = talloc_zero(NULL, struct rt_cfg);
    c->input = o->input;
    c->sub_id = o->sub_id;
    c->sub_file = talloc_strdup(c, o->sub_file ? o->sub_file : "");
    c->full = o->full;
    c->full_dir = talloc_strdup(c, o->full_dir ? o->full_dir : "");
    c->engine = o->full_engine;
    c->omp_model = talloc_strdup(c, o->omp_model ? o->omp_model : "");
    c->omp_command = talloc_strdup(c, o->omp_command && o->omp_command[0]
                                      ? o->omp_command : "omp");
    c->display = o->display;
    c->audio_id = o->audio_id;
    c->lib_action = o->lib_action;
    c->lib_auto_use = o->lib_auto_use;
    c->style = o->style;
    c->style_text = talloc_strdup(c, o->style_text ? o->style_text : "");
    c->secondary = o->secondary;
    c->glossary_req = talloc_strdup(c, o->glossary_req ? o->glossary_req : "");
    c->pgs = o->pgs;
    c->import_req = talloc_strdup(c, o->import_req ? o->import_req : "");
    c->import_adjust = o->import_adjust;
    return c;
}

static struct rt_cfg *rt_dup(void *parent, const struct rt_cfg *s)
{
    struct rt_cfg *c = talloc_zero(parent, struct rt_cfg);
    *c = *s;
    c->sub_file = talloc_strdup(c, s->sub_file);
    c->full_dir = talloc_strdup(c, s->full_dir);
    c->omp_model = talloc_strdup(c, s->omp_model);
    c->omp_command = talloc_strdup(c, s->omp_command);
    c->style_text = talloc_strdup(c, s->style_text);
    c->glossary_req = talloc_strdup(c, s->glossary_req);
    c->import_req = talloc_strdup(c, s->import_req);
    return c;
}

// The text track is only used if there is one to read.
static int rt_input(const struct rt_cfg *c)
{
    return c->input == INPUT_SUB && (c->sub_id >= 0 || c->sub_file[0])
           ? INPUT_SUB : INPUT_STT;
}

static int rt_mode(const struct rt_cfg *c)
{
    if (rt_input(c) != INPUT_SUB)
        return MODE_STT;
    return c->sub_file[0] ? MODE_SUB_EXT : MODE_SUB_EMB;
}

// An explicit change of the AI input. Switching the track that is merely
// being played never changes any of these.
static bool rt_live_differs(const struct rt_cfg *a, const struct rt_cfg *b)
{
    return a->input != b->input || a->sub_id != b->sub_id ||
           strcmp(a->sub_file, b->sub_file) ||
           (b->audio_id >= 0 && a->audio_id != b->audio_id);
}

static bool rt_scan_differs(const struct rt_cfg *a, const struct rt_cfg *b)
{
    return rt_live_differs(a, b) || a->full != b->full || a->engine != b->engine ||
           strcmp(a->full_dir, b->full_dir) || strcmp(a->omp_model, b->omp_model) ||
           strcmp(a->omp_command, b->omp_command) || a->lib_auto_use != b->lib_auto_use;
}

static bool rt_style_differs(const struct rt_cfg *a, const struct rt_cfg *b)
{
    return a->style != b->style || strcmp(a->style_text, b->style_text);
}

// Translation settings every engine is created with and keeps up to date (slock).
struct tr_snap {
    char *style;
    char *style_text;
    char *glossary;
    unsigned gen;
};

static void tr_snapshot(struct nanum *n, void *ctx, struct tr_snap *s)
{
    mp_mutex_lock(&n->slock);
    s->style = talloc_strdup(ctx, style_names[n->style]);
    s->style_text = talloc_strdup(ctx, n->style_text ? n->style_text : "");
    s->glossary = talloc_strdup(ctx, n->glossary ? n->glossary : "");
    s->gen = atomic_load(&n->tr_gen);
    mp_mutex_unlock(&n->slock);
}

// ---------------------------------------------------------------------------
// Subtitle text

static bool is_text_codec(const char *c)
{
    static const char *const names[] = {
        "subrip", "srt", "ass", "ssa", "mov_text", "webvtt", "text", "microdvd",
        "subviewer", "subviewer1", "vplayer", "pjs", "mpl2", "stl", "realtext",
        "jacosub", "sami",
    };
    for (size_t i = 0; c && i < MP_ARRAY_SIZE(names); i++) {
        if (!strcmp(c, names[i]))
            return true;
    }
    return false;
}

// The dialogue text of a subtitle packet (NULL if there is none). ASS packets
// are Matroska-style "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,
// Effect,Text": the text is what follows the 8th comma.
static char *packet_text(void *ctx, const char *codec, const struct demux_packet *pkt)
{
    const char *s = (const char *)pkt->buffer;
    size_t len = pkt->len;
    if (!s || !len)
        return NULL;
    if (!strcmp(codec, "mov_text")) {
        if (len < 2)
            return NULL;
        size_t l = ((unsigned char)s[0] << 8) | (unsigned char)s[1];
        s += 2;
        len -= 2;
        if (l < len)
            len = l;
    } else if (!strcmp(codec, "ass") || !strcmp(codec, "ssa")) {
        int commas = 0;
        while (len && commas < 8) {
            if (*s == ',')
                commas++;
            s++;
            len--;
        }
        if (commas < 8)
            return NULL;
    }
    while (len && (s[len - 1] == '\0' || s[len - 1] == '\n' || s[len - 1] == '\r'))
        len--;
    if (!len)
        return NULL;
    return talloc_strndup(ctx, s, len);
}

// ---------------------------------------------------------------------------
// AI engine glue

static void ass_escape(char **out, const char *s)
{
    char *p = *out;
    for (; *s; s++) {
        if (*s == '{' || *s == '}')
            *p++ = '\\';
        *p++ = (*s == '\n' || *s == '\r') ? ' ' : *s;
    }
    *out = p;
}

// ASS text of one cue for a presentation, NULL if the cue shows nothing:
//   DISP_TRANSLATION  the translation only; a cue without one stays empty
//                     (the source is never shown as a stand-in)
//   DISP_BILINGUAL    translation on top, source below in a smaller grey font;
//                     source only if there is no translation
//   DISP_SOURCE       the source only
static char *build_ass_text(void *ctx, int view, const char *src, const char *tr)
{
    bool have_tr = tr && tr[0];
    bool have_src = src && src[0];
    if ((view == DISP_TRANSLATION && !have_tr) ||
        (view == DISP_SOURCE && !have_src) || (!have_tr && !have_src))
        return NULL;
    size_t cap = 64 + (have_tr ? strlen(tr) * 2 : 0) + (have_src ? strlen(src) * 2 : 0);
    char *out = talloc_array(ctx, char, cap), *p = out;
    switch (view) {
    case DISP_TRANSLATION:
        ass_escape(&p, tr);
        break;
    case DISP_SOURCE:
        ass_escape(&p, src);
        break;
    default:
        // A source that is its own translation (a Korean subtitle file) shows once.
        if (have_tr && have_src && !strcmp(src, tr))
            have_src = false;
        if (have_tr) {
            ass_escape(&p, tr);
            if (have_src) {
                p += sprintf(p, "\\N{\\fs30\\c&H00CCCCCC&}");
                ass_escape(&p, src);
            }
        } else {
            ass_escape(&p, src);
        }
    }
    *p = '\0';
    return out;
}

// Without a translation endpoint there is nothing but the source to show.
static int effective_view(struct nanum *n)
{
    return n->tr_enabled ? atomic_load(&n->view) : DISP_SOURCE;
}

// A line must stay readable: at least 1.5 s on screen.
static double shown_duration(double dur)
{
    return fmax(dur + 0.4, 1.5);
}

// When a cue leaves the screen: its extension (shown_duration()) ends where the next cue
// of the track starts, but never before the cue's own duration (text cues may overlap).
static double display_end(double pts, double dur, double next_pts)
{
    return fmax(fmin(pts + shown_duration(dur), next_pts), pts + dur);
}

static void on_ai_event(void *user, double pts, double dur, const char *src,
                        const char *ko)
{
    struct input *in = user;
    // The secondary input logs as "event2": scripts that read the log for the playback
    // input's lines match "event ".
    static const char *const tags[NANUM_INPUTS] = {"", "2", "-pgs", "-import"};
    mp_verbose(in->n->log, "event%s %.2f +%.2f: %s | %s\n", tags[in->idx],
               pts, dur, src, ko ? ko : "(no translation)");
    nanum_add_cue(in->n, in->idx, pts, dur, src, ko);
}

static void init_swr(struct adec *d, AVFrame *f)
{
    swr_free(&d->swr);
    av_channel_layout_uninit(&d->swr_layout);

    AVChannelLayout out = {0};
    av_channel_layout_default(&out, 1);
    if (swr_alloc_set_opts2(&d->swr, &out, AV_SAMPLE_FMT_FLT, STT_RATE,
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
        swr_set_matrix(d->swr, m, ch);
    av_free(m);

    if (swr_init(d->swr) < 0) {
        swr_free(&d->swr);
        return;
    }
    d->swr_rate = f->sample_rate;
    d->swr_fmt = f->format;
    av_channel_layout_copy(&d->swr_layout, &f->ch_layout);
}

static void process_frame(struct adec *d, AVFrame *f)
{
    if (!d->swr || d->swr_rate != f->sample_rate || d->swr_fmt != f->format ||
        av_channel_layout_compare(&d->swr_layout, &f->ch_layout))
        init_swr(d, f);
    if (!d->swr)
        return;

    int cap = (int)av_rescale_rnd(swr_get_delay(d->swr, f->sample_rate) + f->nb_samples,
                                  STT_RATE, f->sample_rate, AV_ROUND_UP) + 32;
    if (cap > d->obuf_cap) {
        av_free(d->obuf);
        d->obuf = av_malloc_array(cap, sizeof(float));
        d->obuf_cap = d->obuf ? cap : 0;
        if (!d->obuf)
            return;
    }
    int got = swr_convert(d->swr, (uint8_t **)&d->obuf, cap,
                          (const uint8_t **)f->extended_data, f->nb_samples);
    if (got <= 0)
        return;

    double pts = d->last_pkt_pts;
    if (f->pts != AV_NOPTS_VALUE) {
        AVRational tb = f->time_base.num > 0 ? f->time_base : (AVRational){1, 1000000};
        pts = f->pts * av_q2d(tb);
    }
    if (d->sink)
        d->sink(d->sink_user, d->obuf, got, pts);
    else
        d->api->push_audio(d->ai, d->obuf, got, pts);
}

static void decode_packet(struct adec *d, struct demux_packet *dp)
{
    AVPacket *avpkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!avpkt || !frame)
        goto done;

    AVRational tb = {1, 1000000};
    mp_set_av_packet(avpkt, dp, &tb);
    d->last_pkt_pts = dp->pts != MP_NOPTS_VALUE ? dp->pts : d->last_pkt_pts;

    if (avcodec_send_packet(d->avctx, avpkt) < 0)
        goto done;
    while (avcodec_receive_frame(d->avctx, frame) >= 0) {
        process_frame(d, frame);
        av_frame_unref(frame);
    }
done:
    av_frame_free(&frame);
    mp_free_av_packet(&avpkt);      // buf/side_data are borrowed from dp
}

static bool open_decoder(struct adec *d)
{
    const struct mp_codec_params *c = d->codec;
    const AVCodec *codec = avcodec_find_decoder(mp_codec_to_av_codec_id(c->codec));
    if (!codec) {
        mp_err(d->log, "no decoder for audio codec '%s'\n", c->codec);
        return false;
    }
    d->avctx = avcodec_alloc_context3(codec);
    if (!d->avctx || mp_set_avctx_codec_headers(d->avctx, c) < 0) {
        avcodec_free_context(&d->avctx);
        return false;
    }
    d->avctx->pkt_timebase = (AVRational){1, 1000000};
    if (avcodec_open2(d->avctx, codec, NULL) < 0) {
        mp_err(d->log, "cannot open audio decoder '%s'\n", c->codec);
        avcodec_free_context(&d->avctx);
        return false;
    }
    mp_verbose(d->log, "analysing audio stream: %s %d Hz, %d ch\n", c->codec,
               c->samplerate, c->channels.num);
    return true;
}

static void adec_flush(struct adec *d)
{
    if (d->avctx)
        avcodec_flush_buffers(d->avctx);
    if (d->swr)
        swr_close(d->swr), swr_init(d->swr);
}

// Forget the codec (the audio track changed).
static void adec_drop_codec(struct adec *d)
{
    avcodec_free_context(&d->avctx);
    swr_free(&d->swr);
}

static void adec_free(struct adec *d)
{
    adec_drop_codec(d);
    av_channel_layout_uninit(&d->swr_layout);
    av_freep(&d->obuf);
    d->obuf_cap = 0;
}

// mpv reports ISO 639-2 codes ("eng", "jpn"); the engine wants ISO 639-1.
static void iso2_from_mp(const char *lang, char *out, size_t sz)
{
    static const char *const map[][2] = {
        {"eng", "en"}, {"jpn", "ja"}, {"kor", "ko"}, {"zho", "zh"}, {"chi", "zh"},
        {"fra", "fr"}, {"fre", "fr"}, {"deu", "de"}, {"ger", "de"}, {"spa", "es"},
        {"ita", "it"}, {"rus", "ru"}, {"por", "pt"},
    };
    snprintf(out, sz, "auto");
    for (size_t i = 0; lang && i < MP_ARRAY_SIZE(map); i++) {
        if (!strcmp(lang, map[i][0])) {
            snprintf(out, sz, "%s", map[i][1]);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Live worker (one per input)

// Analysed up to where at the playhead: the end of the analysed range that contains
// it (saved results count). A text input has no ranges: its engine's own horizon.
static void publish_ready(struct input *in)
{
    struct nanum *n = in->n;
    double ref = atomic_load(&n->playhead);
    double r = MP_NOPTS_VALUE;
    if (ref == MP_NOPTS_VALUE || !has_ranges(in)) {
        r = atomic_load(&in->engine_until);
    } else {
        mp_mutex_lock(&n->lock);
        for (int i = 0; i < in->ncov; i++) {
            if (in->cov[i].a - COV_REF_TOL <= ref && ref <= in->cov[i].b) {
                r = in->cov[i].b;
                break;
            }
        }
        mp_mutex_unlock(&n->lock);
    }
    atomic_store(&in->ready_until, r);
}

// Brings the engine's style and glossary up to date (worker thread).
static void apply_tr_settings(struct input *in, unsigned *applied)
{
    struct nanum *n = in->n;
    if (atomic_load(&n->tr_gen) == *applied)
        return;
    void *tmp = talloc_new(NULL);
    struct tr_snap s;
    tr_snapshot(n, tmp, &s);
    n->api.set_style(in->ai, s.style, s.style_text);
    n->api.set_glossary(in->ai, s.glossary);
    *applied = s.gen;
    talloc_free(tmp);
}

// Audio worker: copy the engine's progress into atomics the demuxer can read without
// touching the engine (no lock ordering issues with its threads), and keep the
// analysed ranges up to date.
static void publish_progress(struct input *in)
{
    struct nanum *n = in->n;
    int64_t now = mp_time_ns();
    if (now < in->next_progress)
        return;
    in->next_progress = now + MP_TIME_MS_TO_NS(150);
    apply_tr_settings(in, &in->tr_applied);
    double v = n->api.processed_until(in->ai);
    double pu = isnan(v) ? MP_NOPTS_VALUE : v;
    atomic_store(&in->engine_until, pu);
    int mode = atomic_load(&n->a_mode);

    // Every line before processed_until() is delivered: that audio counts as analysed.
    if (is_stt_input(in) && in->run_open && pu != MP_NOPTS_VALUE &&
        pu - COV_MARGIN > in->run_a)
    {
        mp_mutex_lock(&n->lock);
        cov_add_locked(in, in->run_a, fmin(pu - COV_MARGIN, in->run_b));
        mp_mutex_unlock(&n->lock);
    }

    if (in->idx == IN_PRI && mode == MODE_SUB_EXT && atomic_load(&in->text_eof) &&
        n->api.wait_idle(in->ai, 0))
    {
        mp_mutex_lock(&in->alock);
        if (atomic_load(&in->text_eof) && !in->ahead && !in->areset)
            atomic_store(&in->text_complete, true);
        mp_mutex_unlock(&in->alock);
    }

    // Text input: translate the lines around the playback position first.
    double p = atomic_load(&n->playhead);
    if (((in->idx == IN_PRI && mode != MODE_STT) || in->idx == IN_PGS) &&
        p != in->last_playhead && p != MP_NOPTS_VALUE)
    {
        in->last_playhead = p;
        n->api.set_playhead(in->ai, p);
    }

    // Idle: nothing queued for a second and the engine has nothing to do.
    mp_mutex_lock(&in->alock);
    bool empty = !in->ahead && !in->aeof;
    mp_mutex_unlock(&in->alock);
    bool engine_idle = empty && !in->pcm_head && n->api.wait_idle(in->ai, 0);
    if (!engine_idle)
        in->idle_since = 0;
    else if (!in->idle_since)
        in->idle_since = now;
    atomic_store(&in->idle, engine_idle && now - in->idle_since >= MP_TIME_S_TO_NS(1));

    publish_ready(in);
    if (atomic_load(&in->held))
        wake_demux(n);
}

static bool worker_interrupted(struct input *in)
{
    mp_mutex_lock(&in->alock);
    bool r = in->astop || in->aflush || in->areset || in->aswitch;
    mp_mutex_unlock(&in->alock);
    return r;
}

// The audio run [run_a, run_b] ends (a gap, a covered stretch, the end of the media): the
// engine analyses what it still holds first, because audio that does not continue the
// previous chunk makes it discard that. The run then counts as analysed in full.
static void close_run(struct input *in)
{
    struct nanum *n = in->n;
    if (!in->run_open)
        return;
    n->api.flush(in->ai);
    bool idle;
    while (!(idle = n->api.wait_idle(in->ai, 100))) {
        if (worker_interrupted(in))
            break;
        publish_progress(in);
    }
    if (idle) {
        mp_mutex_lock(&n->lock);
        cov_add_locked(in, in->run_a, in->run_b);
        mp_mutex_unlock(&n->lock);
        atomic_store(&in->save_now, true);
    }
    in->run_open = false;
    in->next_progress = 0;
    publish_progress(in);
}

// The run ends without being finished (seek, input change): what the engine delivered
// so far counts.
static void commit_run(struct input *in)
{
    struct nanum *n = in->n;
    if (!in->run_open)
        return;
    double pu = n->api.processed_until(in->ai);
    if (!isnan(pu)) {
        mp_mutex_lock(&n->lock);
        cov_add_locked(in, in->run_a, fmin(pu - COV_MARGIN, in->run_b));
        mp_mutex_unlock(&n->lock);
    }
    in->run_open = false;
}

// Decoded audio reaches the engine here. Audio the input has analysed before (this
// session or a saved one) is not analysed again: the engine is flushed before such a
// stretch and the audio after it starts a new run.
static void push_pcm(struct input *in, const float *pcm, int cnt, double pts)
{
    struct nanum *n = in->n;
    double end = pts + (double)cnt / STT_RATE;
    if (cov_has(in, (pts + end) / 2)) {
        close_run(in);
        return;
    }
    if (in->run_open && fabs(pts - in->run_b) > 0.25)
        close_run(in);
    if (!in->run_open) {
        in->run_open = true;
        in->run_a = pts;
    }
    n->api.push_audio(in->ai, pcm, cnt, pts);
    in->run_b = end;
}

static void pcm_clear(struct input *in)
{
    while (in->pcm_head) {
        struct pcm_chunk *c = in->pcm_head;
        in->pcm_head = c->next;
        free(c);
    }
    in->pcm_tail = NULL;
    in->pcm_samples = 0;
}

// The backlog is bounded. When it is full the NEW audio is dropped: what is kept stays
// contiguous from the earliest unanalysed audio (the part the viewer needs first), and the
// dropped span never counts as analysed, so a later seek analyses it.
static void pcm_enqueue(struct input *in, const float *pcm, int cnt, double pts)
{
    if (in->pcm_samples + (size_t)cnt > MAX_PCM_SAMPLES) {
        if (!in->pcm_full) {
            in->pcm_full = true;
            mp_verbose(in->n->log, "secondary: audio backlog full, not buffering from %.2f\n",
                       pts);
        }
        return;
    }
    in->pcm_full = false;
    struct pcm_chunk *c = malloc(sizeof(*c) + (size_t)cnt * sizeof(float));
    if (!c)
        return;
    c->next = NULL;
    c->pts = pts;
    c->n = cnt;
    memcpy(c->s, pcm, (size_t)cnt * sizeof(float));
    if (in->pcm_tail)
        in->pcm_tail->next = c;
    else
        in->pcm_head = c;
    in->pcm_tail = c;
    in->pcm_samples += cnt;
}

// The secondary input works while the primary has analysed SEC_LEAD_SECS ahead of the
// playhead, or has nothing (more) to do.
static bool sec_gate_open(struct nanum *n)
{
    struct input *p = PRI(n);
    if (!atomic_load(&p->active) || atomic_load(&p->ai_failed) || atomic_load(&n->gen_off))
        return true;
    double ready = atomic_load(&p->ready_until);
    double ph = atomic_load(&n->playhead);
    if (ready != MP_NOPTS_VALUE && ph != MP_NOPTS_VALUE && ready >= ph + SEC_LEAD_SECS)
        return true;
    return atomic_load(&p->idle);
}

// Feeds the buffered audio of the secondary input to its engine while the gate is open,
// keeping the engine's backlog short so that the primary is not kept waiting long when
// it needs the engine again.
static void sec_pump(struct input *in)
{
    while (in->pcm_head) {
        if (!sec_gate_open(in->n))
            return;
        if (in->run_open) {
            double pu = atomic_load(&in->engine_until);
            double base = pu != MP_NOPTS_VALUE && pu > in->run_a ? pu : in->run_a;
            if (in->run_b - base > SEC_ENGINE_AHEAD)
                return;
        }
        struct pcm_chunk *c = in->pcm_head;
        in->pcm_head = c->next;
        if (!in->pcm_head)
            in->pcm_tail = NULL;
        in->pcm_samples -= c->n;
        push_pcm(in, c->s, c->n, c->pts);
        free(c);
    }
}

static void input_pcm(void *user, const float *pcm, int cnt, double pts)
{
    struct input *in = user;
    if (in->idx == IN_SEC) {
        pcm_enqueue(in, pcm, cnt, pts);
        sec_pump(in);
    } else {
        push_pcm(in, pcm, cnt, pts);
    }
}

// Worker queue. 'all' also drops subtitle lines (those are not tied to a
// position: after a seek they are still valid and are not sent twice).
static void clear_queue_locked(struct input *in, bool all)
{
    struct apkt *keep = NULL, *keep_tail = NULL;
    size_t bytes = 0;
    for (struct apkt *a = in->ahead, *next; a; a = next) {
        next = a->next;
        if (!all && a->text) {
            a->next = NULL;
            if (keep_tail)
                keep_tail->next = a;
            else
                keep = a;
            keep_tail = a;
            bytes += a->size;
        } else {
            talloc_free(a->dp);
            talloc_free(a);
        }
    }
    in->ahead = keep;
    in->atail = keep_tail;
    in->abytes = bytes;
}

static void clear_audio_queue_locked(struct input *in)
{
    clear_queue_locked(in, false);
}

// Inserts into the sorted set; false if the hash was already there.
static bool seen_add_locked(struct input *in, uint64_t h)
{
    size_t lo = 0, hi = in->nseen;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (in->seen[mid] < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < in->nseen && in->seen[lo] == h)
        return false;
    MP_TARRAY_GROW(in->seen_ctx, in->seen, in->nseen);
    memmove(&in->seen[lo + 1], &in->seen[lo], (in->nseen - lo) * sizeof(in->seen[0]));
    in->seen[lo] = h;
    in->nseen++;
    return true;
}

static uint64_t fnv1a(uint64_t h, const void *p, size_t len)
{
    const unsigned char *s = p;
    for (size_t i = 0; i < len; i++) {
        h ^= s[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

#define FNV_BASIS UINT64_C(14695981039346656037)

// Tells the live engine the language of the subtitle lines that follow.
static void live_queue_lang(struct input *in, const char *lang)
{
    struct apkt *a = talloc_zero(NULL, struct apkt);
    a->text = talloc_strdup(a, lang);
    a->dur = -1;
    a->size = strlen(lang) + sizeof(*a);
    mp_mutex_lock(&in->alock);
    if (in->atail)
        in->atail->next = a;
    else
        in->ahead = a;
    in->atail = a;
    in->abytes += a->size;
    mp_cond_signal(&in->acond);
    mp_mutex_unlock(&in->alock);
}

// Queues a subtitle line for the live engine. Safe from the demux thread (only
// copies) and from jobs; the same line is only queued once.
static void live_queue_text(struct input *in, double pts, double dur, const char *text)
{
    int64_t ms = llround(pts * 1000);
    uint64_t h = fnv1a(fnv1a(FNV_BASIS, &ms, sizeof(ms)), text, strlen(text));

    struct apkt *a = talloc_zero(NULL, struct apkt);
    a->text = talloc_strdup(a, text);
    a->pts = pts;
    a->dur = dur;
    a->size = strlen(text) + sizeof(*a);

    mp_mutex_lock(&in->alock);
    if (!seen_add_locked(in, h)) {
        mp_mutex_unlock(&in->alock);
        talloc_free(a);
        return;
    }
    if (in->atail)
        in->atail->next = a;
    else
        in->ahead = a;
    in->atail = a;
    in->abytes += a->size;
    mp_cond_signal(&in->acond);
    mp_mutex_unlock(&in->alock);
}

// ---------------------------------------------------------------------------
// Image subtitles (disc PGS / DVD): decoded with libavcodec, recognised by the engine's OCR
// (Windows.Media.Ocr), the text is translated by a translate-only engine like a text input.

static bool is_dvd_sub(const struct mp_codec_params *c)
{
    return c->codec && !strcmp(c->codec, "dvd_subtitle");
}

static void pgs_close_decoder(struct input *in)
{
    avcodec_free_context(&in->vctx);
    in->vcodec = NULL;
}

static bool pgs_open_decoder(struct input *in, const struct mp_codec_params *c)
{
    pgs_close_decoder(in);
    const AVCodec *codec = avcodec_find_decoder(mp_codec_to_av_codec_id(c->codec));
    if (!codec)
        return false;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx || mp_set_avctx_codec_headers(ctx, c) < 0) {
        avcodec_free_context(&ctx);
        return false;
    }
    ctx->pkt_timebase = mp_get_codec_timebase(c);
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return false;
    }
    in->vctx = ctx;
    in->vcodec = c;
    mp_verbose(in->n->log, "reading image subtitles: %s\n", c->codec);
    return true;
}

// A bitmap of the subtitle for the OCR: dark text on white, with a white border, enlarged
// when the text is small. Subtitles are light text with a dark outline: the "ink" is what
// is light and opaque, so the outline turns into background.
static uint8_t *rect_to_gray(const AVSubtitleRect *r, int *out_w, int *out_h)
{
    if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1])
        return NULL;
    const uint32_t *pal = (const uint32_t *)r->data[1];
    int ncol = r->nb_colors > 0 && r->nb_colors <= 256 ? r->nb_colors : 256;
    uint8_t lum[256] = {0}, alpha[256] = {0};
    for (int i = 0; i < ncol; i++) {
        uint32_t c = pal[i];
        alpha[i] = c >> 24;
        lum[i] = (299 * ((c >> 16) & 255) + 587 * ((c >> 8) & 255) + 114 * (c & 255)) / 1000;
    }
    // Light text unless no opaque pixel is light.
    bool light_text = false;
    for (int y = 0; y < r->h && !light_text; y++) {
        const uint8_t *row = r->data[0] + (size_t)y * r->linesize[0];
        for (int x = 0; x < r->w; x++) {
            if (alpha[row[x]] >= 128 && lum[row[x]] >= 150) {
                light_text = true;
                break;
            }
        }
    }
    uint8_t ink[256];
    for (int i = 0; i < 256; i++) {
        int v = light_text ? (lum[i] - 110) * 255 / 90 : (150 - lum[i]) * 255 / 90;
        v = MPCLAMP(v, 0, 255) * alpha[i] / 255;
        ink[i] = 255 - v;
    }
    int pad = 10;
    int sw = r->w + 2 * pad, sh = r->h + 2 * pad;
    uint8_t *src = malloc((size_t)sw * sh);
    if (!src)
        return NULL;
    memset(src, 255, (size_t)sw * sh);
    for (int y = 0; y < r->h; y++) {
        const uint8_t *row = r->data[0] + (size_t)y * r->linesize[0];
        uint8_t *dst = src + (size_t)(y + pad) * sw + pad;
        for (int x = 0; x < r->w; x++)
            dst[x] = ink[row[x]];
    }
    int s = r->h < 40 ? 3 : r->h < 64 ? 2 : 1;
    if (s == 1) {
        *out_w = sw;
        *out_h = sh;
        return src;
    }
    int ow = sw * s, oh = sh * s;
    uint8_t *out = malloc((size_t)ow * oh);
    if (!out) {
        free(src);
        return NULL;
    }
    for (int y = 0; y < oh; y++) {
        float fy = (y + 0.5f) / s - 0.5f;
        int y0 = (int)floorf(fy);
        float wy = fy - y0;
        int y1 = MPMIN(y0 + 1, sh - 1);
        y0 = MPMAX(y0, 0);
        for (int x = 0; x < ow; x++) {
            float fx = (x + 0.5f) / s - 0.5f;
            int x0 = (int)floorf(fx);
            float wx = fx - x0;
            int x1 = MPMIN(x0 + 1, sw - 1);
            x0 = MPMAX(x0, 0);
            float top = src[(size_t)y0 * sw + x0] * (1 - wx) + src[(size_t)y0 * sw + x1] * wx;
            float bot = src[(size_t)y1 * sw + x0] * (1 - wx) + src[(size_t)y1 * sw + x1] * wx;
            out[(size_t)y * ow + x] = (uint8_t)(top * (1 - wy) + bot * wy + 0.5f);
        }
    }
    free(src);
    *out_w = ow;
    *out_h = oh;
    return out;
}

static bool is_cjk_lang(const char *l)
{
    return !strcmp(l, "ja") || !strcmp(l, "zh");
}

// ---------------------------------------------------------------------------
// Cleaning the OCR text. The recogniser reads a subtitle font as best it can: I and l and 1
// and | are one shape, so are O and 0, S and 5, G and 9, B and 6; the apostrophe comes out as
// l, I, a bullet or a backtick; a capital after a lower-case letter is a case slip. English
// text is repaired with a list of common words: a token that is not a word but becomes
// exactly one of them when those shapes are exchanged is that word. Nothing else is touched,
// names (not in the list) stay as they were read.

static const char ocr_word_list[] =
    "a able about above act add after again against age ago agree ahead air all almost alone "
    "along already also although always am among an and angry another answer any anyone "
    "anything anyway appear are area around arm as ask asked at away baby back bad bag ball "
    "be beautiful because become bed been before began begin behind being believe best better "
    "between big bit black blood blue body book born both boy bring brother brought but buy "
    "by call called came can cannot car care carry case catch cause chance change child "
    "children city class clear close cold come common company could country course cut dad "
    "dark daughter day dead dear death decide deep did die different do does dog done door "
    "down dream dreams drink drive during each early earth easy eat else end enough even "
    "evening ever every everyone everything eye eyes face fact fall family far fast father "
    "fear feel feeling feet fell few field fight find fine finish fire first five floor "
    "follow food for forget form found four free friend friends from front full fun game "
    "gave get girl give glad go god going gone good got great green ground group grow guess "
    "guy had hair half hand happen happy hard has hate have he head hear heard heart heavy "
    "hello help her here herself hide high him himself his history hit hold home hope hot "
    "hour house how however huge hundred hurry husband idea if important in inside instead "
    "into is it its itself job just keep kept kid kill kind knew know known lady language "
    "large last late laugh learn least leave left leg less let letter life light like line "
    "list listen little live long look lose lost lot love low luck mad made make man many "
    "may maybe me mean meet men might mind mine minute miss money month more morning most "
    "mother move much music must my myself name near need never new next nice night no "
    "nobody none nor not nothing notice now number obviously of off often oh ok okay old "
    "on once one only open or order other our out outside over own page paper parent part "
    "party pass past pay people perhaps person phone pick picture place plan planning plans "
    "play please point police poor power pretty probably problem promise put question quick "
    "quiet quite rain ran read ready real really reason red remember rest return rich right "
    "river road room run sad safe said same sat save saw say school sea second see seem seen "
    "sell send sense set seven several shall she ship short should shoulder show shut sick "
    "side sign simple since sing sister sit six sleep slow small smile so some someone "
    "something sometimes son song soon sorry sound speak special spend stand star start stay "
    "step still stone stop store story strange street strong student study stuff such summer "
    "sun suppose sure surprise sweet table take talk tall tea teach tell ten than thank "
    "thanks that the their them then there these they thing things think third this those "
    "though thought thousand three through time tired to today together told tomorrow "
    "tonight too took top toward town tree tried true try turn twenty two under understand "
    "until up upon us use used usual very view voice wait walk wall want war warm was wash "
    "watch water way we wear weather week weird well went were what when where whether "
    "which while white who whole whom whose why wife will win wind window wish with without "
    "woman women wonder wonderful wood word work world worry would write wrong year years "
    "yes yesterday yet you young your yourself held weld howls";

// Contractions: stem and the endings it takes.
static const struct { const char *stem, *sfx; } ocr_contr[] = {
    {"i", "m ll ve d"}, {"you", "re ll ve d"}, {"we", "re ll ve d"}, {"they", "re ll ve d"},
    {"he", "s ll d"}, {"she", "s ll d"}, {"it", "s ll d"}, {"that", "s ll d"},
    {"there", "s ll d re"}, {"who", "s ll d re ve"}, {"what", "s re ll"}, {"let", "s"},
    {"here", "s"}, {"how", "s"}, {"where", "s"}, {"when", "s"}, {"why", "s"},
    {"don", "t"}, {"didn", "t"}, {"doesn", "t"}, {"can", "t"}, {"couldn", "t"},
    {"wouldn", "t"}, {"shouldn", "t"}, {"won", "t"}, {"isn", "t"}, {"aren", "t"},
    {"wasn", "t"}, {"weren", "t"}, {"hasn", "t"}, {"haven", "t"}, {"hadn", "t"},
    {"ain", "t"}, {"mustn", "t"},
};

// Short words that are only capitalised at the start of a sentence.
static const char ocr_small_words[] = " of to in on at the and or a an is it as be by for but with "
                                      "from are was were if ";

struct ocr_dict_entry {
    char word[24];
    char canon[24];
};

static struct ocr_dict_entry *ocr_dict;
static size_t ocr_ndict;

// The shape class of a character: the letter its look-alikes stand for.
static char ocr_shape(char c)
{
    switch (c) {
    case 'I': case 'l': case '1': case '|': return 'l';
    case 'O': case 'o': case '0': return 'o';
    case 'S': case 's': case '5': return 's';
    case 'G': case 'g': case '9': return 'g';
    case 'B': case 'b': case '6': return 'b';
    default: return (char)tolower((unsigned char)c);
    }
}

// The canonical spelling of a token; the first letter of a word may be a capital I.
static void ocr_canon(const char *w, bool word, char *out, size_t cap)
{
    size_t i = 0;
    for (; w[i] && i + 1 < cap; i++)
        out[i] = (word && i == 0 && w[i] == 'i') ? 'l' : ocr_shape(w[i]);
    out[i] = '\0';
}

static int ocr_cmp_canon(const void *a, const void *b)
{
    return strcmp(((const struct ocr_dict_entry *)a)->canon,
                  ((const struct ocr_dict_entry *)b)->canon);
}

static void ocr_dict_init(void)
{
    if (ocr_dict)
        return;
    size_t n = 0;
    for (const char *p = ocr_word_list; *p; ) {
        p += strspn(p, " ");
        if (*p) {
            n++;
            p += strcspn(p, " ");
        }
    }
    struct ocr_dict_entry *d = calloc(n, sizeof(*d));
    size_t k = 0;
    for (const char *p = ocr_word_list; d && *p; ) {
        p += strspn(p, " ");
        size_t l = strcspn(p, " ");
        if (l && l < sizeof(d[0].word)) {
            memcpy(d[k].word, p, l);
            ocr_canon(d[k].word, true, d[k].canon, sizeof(d[k].canon));
            k++;
        }
        p += l;
    }
    if (!d)
        return;
    qsort(d, k, sizeof(*d), ocr_cmp_canon);
    ocr_ndict = k;
    ocr_dict = d;
}

static bool ocr_is_word(const char *lower)
{
    for (size_t i = 0; i < ocr_ndict; i++) {
        if (!strcmp(ocr_dict[i].word, lower))
            return true;
    }
    return false;
}

// The one word of the list whose canonical spelling is that of tok; NULL if none or several.
static const char *ocr_lookup_canon(const char *tok)
{
    struct ocr_dict_entry key;
    ocr_canon(tok, false, key.canon, sizeof(key.canon));
    struct ocr_dict_entry *e = bsearch(&key, ocr_dict, ocr_ndict, sizeof(*ocr_dict),
                                       ocr_cmp_canon);
    if (!e)
        return NULL;
    const struct ocr_dict_entry *lo = e;
    if ((lo > ocr_dict && !strcmp(lo[-1].canon, key.canon)) ||
        (lo + 1 < ocr_dict + ocr_ndict && !strcmp(lo[1].canon, key.canon)))
        return NULL;
    return e->word;
}

static void ocr_lower(const char *s, char *out, size_t cap)
{
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = '\0';
}

// Letters and digits of a token with at least one letter, no longer than 20.
static bool ocr_wordlike(const char *t, size_t *len)
{
    size_t n = strlen(t);
    bool alpha = false;
    for (size_t i = 0; i < n; i++) {
        if (isalpha((unsigned char)t[i]))
            alpha = true;
        else if (!isdigit((unsigned char)t[i]))
            return false;
    }
    *len = n;
    return alpha && n >= 2 && n <= 20;
}

// Applies the case of a repaired word: capitalised if the token's first character was
// clearly a capital, or at the start of a sentence.
static void ocr_apply_case(const char *word, const char *tok, bool sentence_start, char *out)
{
    size_t n = strlen(word);
    memcpy(out, word, n + 1);
    bool cap = sentence_start || (isupper((unsigned char)tok[0]) && !strchr("IOSGB", tok[0]));
    if (cap)
        out[0] = (char)toupper((unsigned char)out[0]);
}

// Replaces the confusable separator of a contraction by its apostrophe ("donlt", "youlre",
// "we\xe2\x80\xa2re" -> "don't", "you're", "we're"). core is rewritten in place (it only
// shrinks or stays as long); returns whether it changed.
static bool ocr_fix_contraction(char *core, bool sentence_start)
{
    size_t len = strlen(core);
    if (len < 3 || len > 12)
        return false;
    for (size_t p = 1; p + 1 < len; p++) {
        // The separator: a shape of l, or a mark the recogniser puts for the apostrophe.
        size_t sep = 1;
        unsigned char c = core[p];
        if (!(c == 'l' || c == 'I' || c == '1' || c == '|' || c == '`' ||
              (c == 0xc2 && (unsigned char)core[p + 1] == 0xb4) ||
              (c == 0xe2 && (unsigned char)core[p + 1] == 0x80 &&
               ((unsigned char)core[p + 2] == 0xa2 || (unsigned char)core[p + 2] == 0x99 ||
                (unsigned char)core[p + 2] == 0x98))))
            continue;
        if (c == 0xc2)
            sep = 2;
        else if (c == 0xe2)
            sep = 3;
        if (p + sep >= len)
            continue;
        char stem[16], sfx[16], cs[16], csf[16];
        if (p >= sizeof(stem) || len - p - sep >= sizeof(sfx))
            continue;
        memcpy(stem, core, p);
        stem[p] = '\0';
        memcpy(sfx, core + p + sep, len - p - sep + 1);
        ocr_canon(stem, true, cs, sizeof(cs));
        ocr_canon(sfx, false, csf, sizeof(csf));
        for (size_t i = 0; i < MP_ARRAY_SIZE(ocr_contr); i++) {
            char cw[16];
            ocr_canon(ocr_contr[i].stem, true, cw, sizeof(cw));
            if (strcmp(cw, cs))
                continue;
            for (const char *s = ocr_contr[i].sfx; *s; ) {
                s += strspn(s, " ");
                size_t sl = strcspn(s, " ");
                char sc[8];
                ocr_canon(s, false, sc, sizeof(sc));      // (only the first sl chars matter)
                sc[sl] = '\0';
                bool letter_sep = strchr("lI1|", c) != NULL;
                // "held" and "weld" are words, not "he'd" and "we'd".
                if (letter_sep && sl == 1 && *s == 'd' &&
                    (!strcmp(ocr_contr[i].stem, "he") || !strcmp(ocr_contr[i].stem, "we"))) {
                    s += sl;
                    continue;
                }
                if (sl == strlen(csf) && !strncmp(sc, csf, sl)) {
                    char out[32];
                    bool upper = !strcmp(ocr_contr[i].stem, "i") ||
                                 (isupper((unsigned char)core[0]) && !strchr("IlOSGB", core[0])) ||
                                 (strchr("lI1|", core[0]) && sentence_start);
                    snprintf(out, sizeof(out), "%s'%.*s", ocr_contr[i].stem, (int)sl, s);
                    if (upper)
                        out[0] = (char)toupper((unsigned char)out[0]);
                    if (strlen(out) <= len) {
                        strcpy(core, out);
                        return true;
                    }
                }
                s += sl;
            }
        }
    }
    return false;
}

static bool ocr_word_in(const char *list, const char *lower)
{
    size_t l = strlen(lower);
    for (const char *p = list; l && *p; ) {
        p += strspn(p, " ");
        size_t n = strcspn(p, " ");
        if (n == l && !strncmp(p, lower, l))
            return true;
        p += n;
    }
    return false;
}

struct ocr_tok {
    char lead[32], core[128], trail[32];
    bool start;                 // the first token of the text
    bool eol;                   // the last token of its line
};

// Words that make a number a number: "page 50", "about 50", "50 years".
static const char ocr_num_before[] = " page pages chapter room number no at about over under than "
    "for in from to every nearly almost only just around age aged floor route highway channel "
    "episode volume vol top pg p ";
static const char ocr_num_after[] = " years year days day percent minutes minute hours hour "
    "dollars dollar bucks cents miles feet people times men women kids guys thousand million "
    "billion hundred pounds meters seconds weeks months yards more less mph km kg yen won euros "
    "points percent ";

static bool ocr_upper_initial(const char *s)
{
    for (; *s; s++) {
        if (isalpha((unsigned char)*s))
            return isupper((unsigned char)*s);
    }
    return false;
}

// Bullets at the end of a word: "\xe2\x80\xa2\xe2\x80\xa2" is an ellipsis, "in\xe2\x80\xa2" a dropped g,
// else a full stop.
static void ocr_fix_bullets(struct ocr_tok *t, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char *b = strstr(t[i].trail, "\xe2\x80\xa2");
        if (!b)
            continue;
        size_t cnt = 0;
        for (const char *q = b; !strncmp(q, "\xe2\x80\xa2", 3); q += 3)
            cnt++;
        size_t cl = strlen(t[i].core);
        char low[128];
        ocr_lower(t[i].core, low, sizeof(low));
        if (cnt == 1 && cl >= 5 && !strcmp(t[i].core + cl - 2, "in") && !ocr_is_word(low))
            strcpy(b, "'");
        else
            strcpy(b, cnt >= 2 ? "..." : ".");
    }
}

// Repairs the tokens of a subtitle (English); returns their number (merges can remove some).
static size_t ocr_fix_line(struct ocr_tok *t, size_t n)
{
    // "1 6th" -> "16th"
    for (size_t i = 0; i + 1 < n; i++) {
        const char *nx = t[i + 1].core;
        size_t dl = strspn(nx, "0123456789");
        if (strlen(t[i].core) == 1 && isdigit((unsigned char)t[i].core[0]) && !t[i].trail[0] &&
            dl && strlen(nx) - dl == 2 && strchr("snrt", nx[dl]) &&
            (!strcmp(nx + dl, "st") || !strcmp(nx + dl, "nd") || !strcmp(nx + dl, "rd") ||
             !strcmp(nx + dl, "th")) && strlen(t[i].core) + strlen(nx) < sizeof(t[i].core)) {
            char merged[128];
            snprintf(merged, sizeof(merged), "%s%s", t[i].core, nx);
            strcpy(t[i].core, merged);
            strcpy(t[i].trail, t[i + 1].trail);
            memmove(&t[i + 1], &t[i + 2], (n - i - 2) * sizeof(t[0]));
            n--;
        }
    }
    size_t orig_n = n;
    size_t lowers = 0, uppers = 0;
    for (size_t i = 0; i < n; i++) {
        if (isalpha((unsigned char)t[i].core[0])) {
            if (!strcmp(t[i].core, "I"))
                continue;
            isupper((unsigned char)t[i].core[0]) ? uppers++ : lowers++;
        }
    }
    for (size_t i = 0; i < n; i++) {
        struct ocr_tok *k = &t[i];
        char lower[128];
        bool ss = k->start;
        if (i) {
            size_t pl = strlen(t[i - 1].trail);
            ss = pl && strchr(".?!:", t[i - 1].trail[0]) != NULL;
            if (!ss && !t[i - 1].core[0] && t[i - 1].lead[0] && !strcmp(t[i - 1].lead, "-"))
                ss = true;
        }
        const char *prev = i ? t[i - 1].core : "";
        const char *next = i + 1 < n ? t[i + 1].core : "";
        char plow[128] = "", nlow[128] = "";
        ocr_lower(prev, plow, sizeof(plow));
        ocr_lower(next, nlow, sizeof(nlow));
        // A lone mark where the pronoun I should be: "` knew it".
        if (!k->core[0] && (!strcmp(k->lead, "`") || !strcmp(k->lead, "'")) &&
            islower((unsigned char)next[0]) && strlen(next) >= 2) {
            strcpy(k->core, "I");
            k->lead[0] = '\0';
            continue;
        }
        if (!k->core[0])
            continue;
        if (!strcmp(k->core, "IIII")) {
            strcpy(k->core, "I'll");
            continue;
        }
        ocr_lower(k->core, lower, sizeof(lower));
        if (!ocr_is_word(lower) && ocr_fix_contraction(k->core, ss))
            continue;
        size_t len;
        // Numbers read for words.
        if (!strcmp(k->core, "50") && !strchr(k->lead, '$') && !strchr(k->lead, '#') &&
            !strchr(k->trail, '%') && !(prev[0] && !prev[strspn(prev, "0123456789")]) &&
            !ocr_word_in(ocr_num_before, plow) &&
            ((isalpha((unsigned char)next[0]) && !ocr_word_in(ocr_num_after, nlow) &&
              !ocr_word_in(" and or but to of in on at from ", nlow)) ||
             k->trail[0] == ',')) {
            strcpy(k->core, ss ? "So" : "so");
            continue;
        }
        if (!strcmp(k->core, "505") && !ocr_word_in(ocr_num_before, plow) &&
            (ocr_upper_initial(next) || ocr_upper_initial(prev))) {
            strcpy(k->core, "SOS");
            continue;
        }
        if (!ocr_wordlike(k->core, &len)) {
            continue;
        }
        bool known = ocr_is_word(lower);
        if (known) {
            // Case slips of a known word.
            size_t ups = 0, lows = 0;
            for (size_t j = 0; j < len; j++) {
                if (isupper((unsigned char)k->core[j]))
                    ups++;
                else if (islower((unsigned char)k->core[j]))
                    lows++;
            }
            bool irregular = ups && lows && !(ups == 1 && isupper((unsigned char)k->core[0]));
            bool shout_start = ups == len && len >= 2 && ss && i + 1 < n &&
                               islower((unsigned char)next[0]);
            bool small_cap = ups == 1 && isupper((unsigned char)k->core[0]) && !ss && lowers > uppers &&
                             ocr_word_in(ocr_small_words, lower);
            if (irregular || shout_start || small_cap) {
                strcpy(k->core, lower);
                if (ss && !small_cap)
                    k->core[0] = (char)toupper((unsigned char)k->core[0]);
            }
            continue;
        }
        const char *fix = ocr_lookup_canon(k->core);
        if (fix) {
            char out[128];
            ocr_apply_case(fix, k->core, ss, out);
            if (strlen(out) <= sizeof(k->core) - 1)
                strcpy(k->core, out);
            continue;
        }
        // A name or a rare word with zeros and ones in it: "K0izumi".
        size_t letters = 0;
        bool digits_ok = true, any_digit = false;
        for (size_t j = 0; j < len; j++) {
            if (isalpha((unsigned char)k->core[j]))
                letters++;
            else {
                any_digit = true;
                digits_ok &= k->core[j] == '0' || k->core[j] == '1';
            }
        }
        if (any_digit && digits_ok && letters >= 3 && isalpha((unsigned char)k->core[0])) {
            for (size_t j = 0; j < len; j++) {
                if (k->core[j] == '0')
                    k->core[j] = 'o';
                else if (k->core[j] == '1')
                    k->core[j] = 'l';
            }
            continue;
        }
        // A capital I at the end of the line for the exclamation mark: "Party!".
        if (k->eol && len >= 4 && k->core[len - 1] == 'I' &&
            islower((unsigned char)k->core[len - 2]) && !k->trail[0]) {
            char head[128];
            memcpy(head, k->core, len - 1);
            head[len - 1] = '\0';
            ocr_lower(head, lower, sizeof(lower));
            if (ocr_is_word(lower) || (isupper((unsigned char)head[0]) &&
                                       !strpbrk(head + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"))) {
                strcpy(k->core, head);
                strcpy(k->trail, "!");
            }
        }
    }
    ocr_fix_bullets(t, n);
    (void)orig_n;
    return n;
}

// The cleaned text (a talloc child of ctx): lines merged into one, English repaired.
static char *ocr_clean(void *ctx, const char *lang, const char *text)
{
    ocr_dict_init();
    bool en = !strcmp(lang, "en") && ocr_dict;
    bool cjk = is_cjk_lang(lang);
    bool latin = !cjk && strcmp(lang, "ru") != 0;
    size_t tcap = strlen(text) / 2 + 8, nt = 0, cap = strlen(text) * 2 + 16, o = 0;
    struct ocr_tok *tk = latin ? calloc(tcap, sizeof(*tk)) : NULL;
    char *out = malloc(cap);
    if (!out || (latin && !tk)) {
        free(out);
        free(tk);
        return talloc_strdup(ctx, "");
    }
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = malloc(len + 1);
        memcpy(line, p, len);
        line[len] = '\0';
        p += len + (nl ? 1 : 0);
        char *a = line, *b = line + len;
        while (a < b && isspace((unsigned char)*a))
            a++;
        while (b > a && isspace((unsigned char)b[-1]))
            b--;
        *b = '\0';
        if (a == b) {
            free(line);
            continue;
        }
        if (en) {
            // A bar for the pronoun and a lone l for I.
            for (char *q = a; *q; q++) {
                char prev = q > a ? q[-1] : ' ', next = q[1] ? q[1] : ' ';
                bool pa = isalpha((unsigned char)prev), na = isalpha((unsigned char)next);
                bool pn = isalnum((unsigned char)prev);
                if (*q == '|' && ((pa && na) || (!pn && (next == ' ' || next == '\'' ||
                                                         next == ',' || next == '.' || !q[1]))))
                    *q = 'I';
                else if (*q == 'l' && !pn && (next == ' ' || !q[1]))
                    *q = 'I';
                else if (*q == 'l' && !pn && next == '\'' &&
                         (!strncmp(q + 1, "'m", 2) || !strncmp(q + 1, "'d", 2) ||
                          !strncmp(q + 1, "'ll", 3) || !strncmp(q + 1, "'ve", 3)))
                    *q = 'I';
            }
        }
        if (latin) {
            // Tokens: leading marks, the word (marks between letters belong to it), trailing
            // marks.
            for (char *q = a; *q && nt < tcap; ) {
                q += strspn(q, " \t");
                if (!*q)
                    break;
                size_t tl = strcspn(q, " \t");
                char *e = q + tl;
                char *w0 = q, *w1 = e;
                while (w0 < e && !isalnum((unsigned char)*w0))
                    w0++;
                while (w1 > w0 && !isalnum((unsigned char)w1[-1]))
                    w1--;
                struct ocr_tok *k = &tk[nt++];
                if (w0 == e) {      // no letter or digit: all of it is "lead"
                    snprintf(k->lead, sizeof(k->lead), "%.*s", (int)MPMIN(tl, sizeof(k->lead) - 1), q);
                } else {
                    snprintf(k->lead, sizeof(k->lead), "%.*s",
                             (int)MPMIN((size_t)(w0 - q), sizeof(k->lead) - 1), q);
                    snprintf(k->core, sizeof(k->core), "%.*s",
                             (int)MPMIN((size_t)(w1 - w0), sizeof(k->core) - 1), w0);
                    snprintf(k->trail, sizeof(k->trail), "%.*s",
                             (int)MPMIN((size_t)(e - w1), sizeof(k->trail) - 1), w1);
                }
                k->start = nt == 1;
                q = e;
            }
            if (nt)
                tk[nt - 1].eol = true;
        } else {
            size_t l = strlen(a);
            if (o + l + 2 > cap) {
                cap = (o + l) * 2 + 16;
                out = realloc(out, cap);
            }
            if (o && !cjk)
                out[o++] = ' ';
            memcpy(out + o, a, l);
            o += l;
        }
        free(line);
    }
    if (latin) {
        if (en)
            nt = ocr_fix_line(tk, nt);
        else
            ocr_fix_bullets(tk, nt);
        size_t need = 0;
        for (size_t i = 0; i < nt; i++)
            need += strlen(tk[i].lead) + strlen(tk[i].core) + strlen(tk[i].trail) + 1;
        if (need + 2 > cap) {
            cap = need * 2 + 16;
            out = realloc(out, cap);
        }
        for (size_t i = 0; i < nt; i++)
            o += sprintf(out + o, "%s%s%s%s", i ? " " : "", tk[i].lead, tk[i].core, tk[i].trail);
    }
    out[o] = '\0';
    // No space before closing punctuation, no doubled spaces.
    char *w = out;
    for (char *q = out; *q; q++) {
        if (*q == ' ' && (q[1] == ' ' || (q > out && q[1] && strchr(",.!?:;", q[1]))))
            continue;
        *w++ = *q;
    }
    *w = '\0';
    char *res = talloc_strdup(ctx, out);
    free(out);
    free(tk);
    return res;
}

// The text of a decoded subtitle (a talloc child of ctx), NULL if it has none or the OCR
// failed. Rectangles are read top to bottom.
static char *pgs_ocr(void *ctx, struct input *in, const AVSubtitle *sub)
{
    struct nanum *n = in->n;
    unsigned order[16];
    unsigned nr = MPMIN(sub->num_rects, 16u);
    for (unsigned i = 0; i < nr; i++)
        order[i] = i;
    for (unsigned i = 1; i < nr; i++) {     // insertion sort by y, then x
        unsigned v = order[i], j = i;
        while (j > 0 && (sub->rects[order[j - 1]]->y > sub->rects[v]->y ||
                         (sub->rects[order[j - 1]]->y == sub->rects[v]->y &&
                          sub->rects[order[j - 1]]->x > sub->rects[v]->x))) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }
    char *all = talloc_strdup(ctx, "");
    for (unsigned i = 0; i < nr; i++) {
        int w, h;
        uint8_t *g = rect_to_gray(sub->rects[order[i]], &w, &h);
        if (!g)
            continue;
        char buf[4096];
        int ok = n->api.ocr(g, w, h, w, in->ocr_lang, buf, sizeof(buf));
        free(g);
        if (!ok) {
            if (++in->ocr_fail <= 3)
                mp_warn(n->log, "OCR failed: %s\n", n->api.last_error());
            if (in->ocr_fail >= 5)
                ai_fail(in, "the OCR does not work: %s", n->api.last_error());
            continue;
        }
        in->ocr_fail = 0;
        if (buf[0])
            all = talloc_asprintf_append(all, "%s%s", all[0] ? "\n" : "", buf);
    }
    if (!all[0])
        return NULL;
    char *clean = ocr_clean(ctx, in->lang, all);
    return clean[0] ? clean : NULL;
}

// Coverage of the run so far: what the demuxer has read, as far as every subtitle in it has
// reached the translator (and, while that is busy, as far as it has translated).
static void pgs_cover(struct input *in)
{
    struct nanum *n = in->n;
    if (!in->run_open)
        return;
    double end = in->run_b;
    if (in->pgs_pending)
        end = fmin(end, in->pgs_start);
    if (!n->api.wait_idle(in->ai, 0)) {
        double pu = n->api.processed_until(in->ai);
        if (isnan(pu))
            return;
        end = fmin(end, pu - COV_MARGIN);
    }
    if (end > in->run_a) {
        mp_mutex_lock(&n->lock);
        cov_add_locked(in, in->run_a, end);
        mp_mutex_unlock(&n->lock);
    }
}

static void pgs_emit(struct input *in, double start, double end, const char *text)
{
    struct nanum *n = in->n;
    double dur = MPCLAMP(end - start, 0.3, PGS_MAX_DUR);
    mp_verbose(n->log, "image subtitle %.2f +%.2f: %s\n", start, dur, text);
    n->api.push_text(in->ai, start, dur, text);
}

static void pgs_decode(struct input *in, struct apkt *a)
{
    if (!a->dp || a->dp->pts == MP_NOPTS_VALUE)
        return;
    if ((!in->vctx || in->vcodec != a->codec) && !pgs_open_decoder(in, a->codec)) {
        ai_fail(in, "cannot open the image subtitle decoder");
        return;
    }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return;
    AVRational tb = in->vctx->pkt_timebase;
    mp_set_av_packet(pkt, a->dp, &tb);
    AVSubtitle sub;
    int got = 0;
    int r = avcodec_decode_subtitle2(in->vctx, &sub, &got, pkt);
    mp_free_av_packet(&pkt);
    if (r < 0 || !got)
        return;
    double start = a->dp->pts + sub.start_display_time / 1000.0;
    // A display set ends the subtitle that is on screen.
    if (in->pgs_pending) {
        pgs_emit(in, in->pgs_start, start, in->pgs_text);
        talloc_free(in->pgs_text);
        in->pgs_text = NULL;
        in->pgs_pending = false;
    }
    if (sub.num_rects && !cov_has(in, start)) {
        void *tmp = talloc_new(NULL);
        char *text = pgs_ocr(tmp, in, &sub);
        if (text) {
            if (is_dvd_sub(a->codec) && sub.end_display_time > sub.start_display_time &&
                sub.end_display_time != UINT32_MAX) {
                pgs_emit(in, start, a->dp->pts + sub.end_display_time / 1000.0, text);
            } else {
                in->pgs_pending = true;
                in->pgs_start = start;
                in->pgs_text = talloc_strdup(NULL, text);
            }
        }
        talloc_free(tmp);
    }
    avsubtitle_free(&sub);
}

// The demuxer's position: all subtitles that start before it have been delivered.
static void pgs_marker(struct input *in, double pos)
{
    if (!in->run_open) {
        in->run_open = true;
        in->run_a = pos;
        in->run_b = pos - PGS_LAG;
    }
    in->run_b = fmax(in->run_b, pos - PGS_LAG);
    if (in->pgs_pending && pos - in->pgs_start > PGS_MAX_DUR + 4) {
        pgs_emit(in, in->pgs_start, in->pgs_start + PGS_MAX_DUR, in->pgs_text);
        talloc_free(in->pgs_text);
        in->pgs_text = NULL;
        in->pgs_pending = false;
    }
    pgs_cover(in);
}

// The demuxer jumped: nothing of the old position continues.
static void pgs_seek(struct input *in)
{
    pgs_cover(in);
    in->run_open = false;
    if (in->vctx)
        avcodec_flush_buffers(in->vctx);
    talloc_free(in->pgs_text);
    in->pgs_text = NULL;
    in->pgs_pending = false;
    atomic_store(&in->done, false);
    in->idle_since = 0;
    publish_ready(in);
}

// The demuxer reached the end: the last subtitle ends, the translator finishes, the run
// covers the rest of the media.
static void pgs_finish(struct input *in)
{
    struct nanum *n = in->n;
    if (in->pgs_pending) {
        pgs_emit(in, in->pgs_start, in->pgs_start + 3.0, in->pgs_text);
        talloc_free(in->pgs_text);
        in->pgs_text = NULL;
        in->pgs_pending = false;
    }
    n->api.flush(in->ai);
    bool idle;
    while (!(idle = n->api.wait_idle(in->ai, 100))) {
        if (worker_interrupted(in))
            break;
        publish_progress(in);
    }
    if (!idle)
        return;
    if (in->run_open) {
        double d = atomic_load(&n->duration);
        in->run_b = fmax(in->run_b, d > 0 ? d : in->run_b);
        pgs_cover(in);
    }
    in->next_progress = 0;
    publish_progress(in);
    atomic_store(&in->done, true);
    atomic_store(&in->save_now, true);
}

static MP_THREAD_VOID audio_thread(void *p)
{
    struct input *in = p;
    struct nanum *n = in->n;
    bool primary = in->idx == IN_PRI;
    mp_thread_set_name(primary ? "nanum-audio" : in->idx == IN_PGS ? "nanum-pgs" : "nanum-audio2");

    // Streams are not known when the primary worker starts. Wait for the first input
    // so Japanese-only installations do not try to load the English model.
    char initial_language[sizeof(in->alang)] = "en";
    mp_mutex_lock(&in->alock);
    if (primary) {
        while (!in->astop && !in->aswitch && !in->ahead)
            mp_cond_wait(&in->acond, &in->alock);
        if (in->astop) {
            mp_mutex_unlock(&in->alock);
            MP_THREAD_RETURN();
        }
        if (in->aswitch && in->alang[0] && strcmp(in->alang, "auto"))
            snprintf(initial_language, sizeof(initial_language), "%s", in->alang);
        else if (in->ahead && in->ahead->text && in->ahead->dur < 0)
            snprintf(initial_language, sizeof(initial_language), "%s", in->ahead->text);
    } else {
        snprintf(initial_language, sizeof(initial_language), "%s", in->alang);
    }
    mp_mutex_unlock(&in->alock);

    // Model loading stays off the demux/player threads.
    struct nanum_opts *o = n->opts;
    void *tmp = talloc_new(NULL);
    struct tr_snap ts;
    tr_snapshot(n, tmp, &ts);
    in->tr_applied = ts.gen;
    struct nanum_ai_config cfg = {
        .abi_version = NANUM_AI_ABI_VERSION,
        .stt_model_path = o->stt_model,
        .vad_model_path = o->vad_model,
        .stt_model_multilingual_path = o->stt_model_multi,
        .language = primary && strcmp(o->language, "auto") ? o->language : initial_language,
        .use_gpu = n->engine.use_gpu,
        .translate_endpoint = o->tr_endpoint,
        .translate_model = o->tr_model,
        .target_language = o->target_language,
        .cache_path = o->cache,
        .translate_only = (primary && atomic_load(&n->a_mode) != MODE_STT) || in->idx == IN_PGS,
        .translate_style = ts.style,
        .translate_style_text = ts.style_text,
        .glossary = ts.glossary,
    };
    in->follow_track = primary && !strcmp(o->language, "auto");
    in->ai = n->api.create(&cfg, on_ai_event, in);
    talloc_free(tmp);
    if (!in->ai) {
        mp_err(n->log, "cannot start the AI engine%s: %s\n", primary ? "" : " (secondary)",
               n->api.last_error());
        ai_fail(in, "cannot start the AI engine (%s): %s", n->engine.backend,
                n->api.last_error());
        MP_THREAD_RETURN();
    }
    in->dec.ai = in->ai;
    in->dec.sink = input_pcm;
    in->dec.sink_user = in;
    const char *language = primary && strcmp(o->language, "auto") ? o->language
                                                                   : initial_language;
    const char *model = strcmp(language, "en") && o->stt_model_multi && o->stt_model_multi[0]
                        ? o->stt_model_multi : o->stt_model;
    bool text_only = (primary && atomic_load(&n->a_mode) != MODE_STT) || in->idx == IN_PGS;
    mp_info(n->log, "AI engine ready%s (backend: %s, STT: %s, language: %s, translation: %s)\n",
            primary ? "" : in->idx == IN_PGS ? " [image subtitles]" : " [secondary]",
            n->engine.backend, text_only ? "off (text input)" : model,
            language, o->tr_endpoint && o->tr_endpoint[0] ? o->tr_model : "off");

    mp_mutex_lock(&in->alock);
    while (!in->astop) {
        if (in->areset) {
            in->areset = false;
            mp_mutex_unlock(&in->alock);
            n->api.reset(in->ai);
            pcm_clear(in);
            in->run_open = false;
            events_clear(in);
            in->last_playhead = MP_NOPTS_VALUE;
            atomic_store(&in->done, false);
            atomic_store(&in->reset_pending, false);
            mp_mutex_lock(&in->alock);
            continue;
        }
        if (in->aflush) {
            in->aflush = false;
            mp_mutex_unlock(&in->alock);
            adec_flush(&in->dec);
            // What follows is not a continuation of what the engine has seen.
            // It would notice a jump by itself, but only one of more than a
            // quarter second, and until the first new window is done it would
            // keep reporting the old position as recognised.
            if (in->idx == IN_PGS)
                pgs_seek(in);
            if (is_stt_input(in)) {
                commit_run(in);
                n->api.reset(in->ai);
                pcm_clear(in);
                atomic_store(&in->engine_until, MP_NOPTS_VALUE);
                atomic_store(&in->done, false);
                atomic_store(&in->idle, false);
                in->idle_since = 0;
                publish_ready(in);
            }
            mp_verbose(n->log, "%saudio decoder flushed\n", primary ? "" : "secondary: ");
            mp_mutex_lock(&in->alock);
            continue;
        }
        if (in->aswitch) {
            in->aswitch = false;
            in->dec.codec = in->acodec_next;
            char lang[sizeof(in->alang)];
            snprintf(lang, sizeof(lang), "%s", in->alang);
            mp_mutex_unlock(&in->alock);
            adec_drop_codec(&in->dec);
            if (in->follow_track) {
                mp_info(n->log, "audio language: %s\n", lang);
                if (is_stt_input(in)) {
                    commit_run(in);
                    pcm_clear(in);
                }
                n->api.set_language(in->ai, lang);
            }
            mp_mutex_lock(&in->alock);
            continue;
        }
        // The library is consulted before the first audio is analysed: what it holds
        // must not be analysed again.
        if (!atomic_load(&in->bound)) {
            mp_cond_timedwait(&in->acond, &in->alock, MP_TIME_MS_TO_NS(100));
            continue;
        }
        // Image subtitles wait for the playback input like the secondary one.
        if (in->idx == IN_PGS && (in->ahead || in->aeof) && !sec_gate_open(n)) {
            mp_cond_timedwait(&in->acond, &in->alock, MP_TIME_MS_TO_NS(200));
            mp_mutex_unlock(&in->alock);
            publish_progress(in);
            mp_mutex_lock(&in->alock);
            continue;
        }
        struct apkt *a = in->ahead;
        if (!a) {
            if (in->aeof && !in->pcm_head) {
                in->aeof = false;
                mp_mutex_unlock(&in->alock);
                if (is_stt_input(in))
                    close_run(in);
                else if (in->idx == IN_PGS)
                    pgs_finish(in);
                else
                    n->api.flush(in->ai);   // analyse the audio left in the buffer
                if (in->idx == IN_SEC)
                    atomic_store(&in->done, true);
                mp_mutex_lock(&in->alock);
                continue;
            }
            // Wake up regularly even when idle so the progress stays current.
            mp_cond_timedwait(&in->acond, &in->alock, MP_TIME_MS_TO_NS(200));
            mp_mutex_unlock(&in->alock);
            if (in->idx == IN_SEC)
                sec_pump(in);
            publish_progress(in);
            mp_mutex_lock(&in->alock);
            continue;
        }
        in->ahead = a->next;
        if (!in->ahead)
            in->atail = NULL;
        in->abytes -= a->size;
        mp_mutex_unlock(&in->alock);

        if (a->text && a->dur < 0) {        // language of the subtitle lines
            n->api.set_language(in->ai, a->text);
            talloc_free(a);
            mp_mutex_lock(&in->alock);
            continue;
        }
        if (a->text) {
            n->api.push_text(in->ai, a->pts, a->dur, a->text);
            publish_progress(in);
            talloc_free(a);
            mp_mutex_lock(&in->alock);
            continue;
        }
        if (a->img) {
            if (atomic_load(&in->done))
                atomic_store(&in->done, false);     // more after the end (a seek)
            if (a->img == 2)
                pgs_marker(in, a->pts);
            else
                pgs_decode(in, a);
            publish_progress(in);
            talloc_free(a->dp);
            talloc_free(a);
            mp_mutex_lock(&in->alock);
            continue;
        }

        if (!in->dec.avctx && !open_decoder(&in->dec)) {
            ai_fail(in, "cannot open the audio decoder of the AI input");
            talloc_free(a->dp);
            talloc_free(a);
            mp_mutex_lock(&in->alock);
            break;
        }
        if (atomic_load(&in->done))
            atomic_store(&in->done, false);     // more audio after the end (a seek)
        decode_packet(&in->dec, a->dp);
        publish_progress(in);
        talloc_free(a->dp);
        talloc_free(a);

        mp_mutex_lock(&in->alock);
    }
    mp_mutex_unlock(&in->alock);
    MP_THREAD_RETURN();
}

// The live input changed: discard everything that is pending and let the
// engine start fresh (like a seek, but also for already queued text).
static void live_restart(struct input *in)
{
    if (!atomic_load(&in->athread_started))
        return;
    mp_mutex_lock(&in->alock);
    atomic_store(&in->text_eof, false);
    atomic_store(&in->text_complete, false);
    clear_queue_locked(in, true);
    in->nseen = 0;
    in->aflush = true;
    in->areset = true;
    atomic_store(&in->reset_pending, true);
    mp_cond_signal(&in->acond);
    mp_mutex_unlock(&in->alock);
    atomic_store(&in->rebind, true);
    atomic_store(&in->rebind_sub, true);
    atomic_store(&in->ready_until, MP_NOPTS_VALUE);
}

// An id does not name one stream: the TrueHD stream of a Blu-ray PID and its
// embedded AC-3 compatibility core are two streams with the same demuxer id
// (demux_disc.c keeps them apart by ordinal). The AI input is the first one
// announced; the scan's own demuxer picks the same one (open_source()). Packets
// of the others are ignored. Taking turns with them switched the decoder
// between two codecs every few milliseconds, which dropped the queued audio and
// the decoder state each time.
static int resolve_stream_idx(struct nanum *n, int id, char codec[16])
{
    int idx = -1;
    codec[0] = '\0';
    mp_mutex_lock(&n->slock);
    for (int i = 0; id >= 0 && i < n->nstreams; i++) {
        const struct sinfo *s = &n->streams[i];
        if (s->type == STREAM_AUDIO && s->id == id && (idx < 0 || s->index < idx)) {
            idx = s->index;
            snprintf(codec, 16, "%s", s->codec);
        }
    }
    mp_mutex_unlock(&n->slock);
    return idx;
}

static bool lang_known(const char *mp_lang)
{
    char l[8];
    iso2_from_mp(mp_lang, l, sizeof(l));
    return strcmp(l, "auto") != 0;
}

static bool same_lang(const char *a, const char *b)
{
    char la[8], lb[8];
    iso2_from_mp(a, la, sizeof(la));
    iso2_from_mp(b, lb, sizeof(lb));
    return !strcmp(la, lb);
}

static void cand_free_locked(struct nanum *n, int i)
{
    struct cand *c = &n->cand[i];
    for (struct apkt *a = c->head, *next; a; a = next) {
        next = a->next;
        talloc_free(a->dp);
        talloc_free(a);
    }
    n->cand_bytes -= c->bytes;
    memmove(c, c + 1, (n->ncand - i - 1) * sizeof(*c));
    n->ncand--;
}

// Recomputes which audio streams may become the secondary input (see struct nanum): those
// of a media with several audio languages, in a known language that is not the one the
// playback input reads, once the player plays an audio stream (so the demuxer has not
// read any yet) and until the secondary input runs or is known not to exist. The kept
// packets of streams that stopped being candidates are dropped. true if the set changed.
static bool cand_recompute(struct nanum *n)
{
    uint64_t mask = 0;
    struct input *pri = PRI(n);
    if (atomic_load(&n->ai_enabled) && atomic_load(&n->sec_enabled) &&
        atomic_load(&n->audio_played) && !atomic_load(&n->sec_dead) &&
        !atomic_load(&n->gen_off) && !atomic_load(&SEC(n)->active))
    {
        bool stt = atomic_load(&n->a_mode) == MODE_STT;
        int pin = stt ? atomic_load(&pri->pin_idx) : -1;
        mp_mutex_lock(&n->slock);
        const char *pin_lang = NULL;
        for (int i = 0; pin >= 0 && i < n->nstreams; i++) {
            if (n->streams[i].index == pin)
                pin_lang = n->streams[i].lang;
        }
        for (int i = 0; n->multi_lang && i < n->nstreams; i++) {
            const struct sinfo *s = &n->streams[i];
            if (s->type != STREAM_AUDIO || s->index >= 64 || s->index == pin ||
                !lang_known(s->lang) || (pin_lang && same_lang(pin_lang, s->lang)))
                continue;
            bool first = true;      // one stream per id: the first announced
            for (int k = 0; k < n->nstreams; k++) {
                const struct sinfo *o = &n->streams[k];
                if (o->type == STREAM_AUDIO && o->id == s->id && o->index < s->index)
                    first = false;
            }
            if (first)
                mask |= UINT64_C(1) << s->index;
        }
        mp_mutex_unlock(&n->slock);
    }
    uint64_t prev = atomic_exchange(&n->cand_mask, mask);
    if (prev & ~mask) {
        mp_mutex_lock(&n->cmutex);
        for (int i = n->ncand; i-- > 0; ) {
            if (!((mask >> n->cand[i].index) & 1))
                cand_free_locked(n, i);
        }
        mp_mutex_unlock(&n->cmutex);
    }
    return prev != mask;
}

static void resolve_pin(struct nanum *n)
{
    char codec[16];
    struct input *in = PRI(n);
    int pin = atomic_load(&in->pin_id);
    int idx = resolve_stream_idx(n, pin, codec);
    int prev = atomic_exchange(&in->pin_idx, idx);
    if (prev != idx && idx >= 0)
        mp_verbose(n->log, "AI audio input: stream %d (id %d, %s)\n", idx, pin, codec);
    cand_recompute(n);
}

static void apply_mode(struct nanum *n, const struct rt_cfg *c)
{
    atomic_store(&n->a_sub_id, c->sub_id);
    atomic_store(&n->a_mode, rt_mode(c));
    // Once pinned, the AI audio input never follows the playback track again.
    if (c->audio_id >= 0) {
        atomic_store(&PRI(n)->pin_id, c->audio_id);
        resolve_pin(n);
    }
}

#ifdef _WIN32
// Loads the engine DLL (and its runtimes) into eng and resolves the API into api.
static bool load_api(struct nanum *n, enum nanum_accel_mode mode, struct nanum_engine *eng,
                     struct nanum_ai_api *api, char *why, size_t whysz)
{
    char err[1024];
    if (!nanum_accel_load_engine(n->log, mode, n->opts->ai_dll, eng, err, sizeof(err))) {
        snprintf(why, whysz, "%s", err);
        return false;
    }
    HMODULE h = eng->module;
    api->create = (void *)GetProcAddress(h, "nanum_ai_create");
    api->destroy = (void *)GetProcAddress(h, "nanum_ai_destroy");
    api->push_audio = (void *)GetProcAddress(h, "nanum_ai_push_audio");
    api->flush = (void *)GetProcAddress(h, "nanum_ai_flush");
    api->reset = (void *)GetProcAddress(h, "nanum_ai_reset");
    api->last_error = (void *)GetProcAddress(h, "nanum_ai_last_error");
    api->set_language = (void *)GetProcAddress(h, "nanum_ai_set_language");
    api->processed_until = (void *)GetProcAddress(h, "nanum_ai_processed_until");
    api->wait_idle = (void *)GetProcAddress(h, "nanum_ai_wait_idle");
    api->push_text = (void *)GetProcAddress(h, "nanum_ai_push_text");
    api->set_playhead = (void *)GetProcAddress(h, "nanum_ai_set_playhead");
    api->translate_all = (void *)GetProcAddress(h, "nanum_ai_translate_all");
    api->cancel_full = (void *)GetProcAddress(h, "nanum_ai_cancel_full");
    api->transcript_size = (void *)GetProcAddress(h, "nanum_ai_transcript_size");
    api->transcript_export = (void *)GetProcAddress(h, "nanum_ai_transcript_export");
    api->set_glossary = (void *)GetProcAddress(h, "nanum_ai_set_glossary");
    api->set_style = (void *)GetProcAddress(h, "nanum_ai_set_style");
    api->get_glossary = (void *)GetProcAddress(h, "nanum_ai_get_glossary");
    api->profile = (void *)GetProcAddress(h, "nanum_ai_profile");
    api->ocr = (void *)GetProcAddress(h, "nanum_ai_ocr");
    api->ocr_languages = (void *)GetProcAddress(h, "nanum_ai_ocr_languages");
    if (!api->create || !api->destroy || !api->push_audio ||
        !api->flush || !api->reset || !api->set_language ||
        !api->processed_until || !api->last_error || !api->wait_idle ||
        !api->push_text || !api->set_playhead || !api->translate_all ||
        !api->cancel_full || !api->transcript_size || !api->transcript_export ||
        !api->set_glossary || !api->set_style || !api->get_glossary || !api->profile ||
        !api->ocr || !api->ocr_languages)
    {
        snprintf(why, whysz, "'%s' does not export the nanum_ai API (ABI %d)",
                 eng->path, NANUM_AI_ABI_VERSION);
        nanum_accel_unload_engine(eng);
        return false;
    }
    return true;
}

static bool load_engine(struct nanum *n)
{
    char why[1024];
    enum nanum_accel_mode mode = atomic_load(&n->a_mode) == MODE_STT
                                ? n->opts->acceleration : NANUM_ACCEL_CPU;
    if (!load_api(n, mode, &n->engine, &n->api, why, sizeof(why))) {
        mp_err(n->log, "cannot start the AI engine: %s\n", why);
        set_ai(n, AI_UNAVAILABLE, "", why, "");
        return false;
    }
    set_ai(n, AI_READY, n->engine.backend, "", n->engine.notice);
    char langs[sizeof(n->ocr_langs)] = "";
    n->api.ocr_languages(langs, sizeof(langs));
    mp_mutex_lock(&n->slock);
    snprintf(n->ocr_langs, sizeof(n->ocr_langs), "%s", langs);
    n->ocr_known = true;
    mp_mutex_unlock(&n->slock);
    mp_verbose(n->log, "OCR languages: %s\n", langs[0] ? langs : "(none)");
    if (n->engine.notice[0])
        mp_verbose(n->log, "%s\n", n->engine.notice);
    return true;
}

static void unload_api(struct nanum_engine *eng)
{
    nanum_accel_unload_engine(eng);
}

static void unload_engine(struct nanum *n)
{
    unload_api(&n->engine);
}
#else
static bool load_api(struct nanum *n, enum nanum_accel_mode mode, struct nanum_engine *eng,
                     struct nanum_ai_api *api, char *why, size_t whysz)
{
    snprintf(why, whysz, "the AI engine is only available on Windows");
    return false;
}

static bool load_engine(struct nanum *n)
{
    mp_err(n->log, "the AI engine is only available on Windows\n");
    set_ai(n, AI_UNAVAILABLE, "", "the AI engine is only available on Windows", "");
    return false;
}
static void unload_api(struct nanum_engine *eng) {}
static void unload_engine(struct nanum *n) {}
#endif

// Stops the worker of an input and releases its engine instance.
static void input_stop(struct input *in)
{
    struct nanum *n = in->n;
    atomic_store(&in->active, false);
    atomic_store(&in->text_eof, false);
    atomic_store(&in->text_complete, false);
    mp_mutex_lock(&in->alock);
    in->astop = true;
    mp_cond_signal(&in->acond);
    mp_mutex_unlock(&in->alock);
    if (atomic_load(&in->athread_started)) {
        mp_thread_join(in->athread);
        atomic_store(&in->athread_started, false);
    }
    if (in->ai)
        n->api.destroy(in->ai);
    in->ai = NULL;
    in->dec.ai = NULL;
    adec_free(&in->dec);
    pcm_clear(in);
    in->run_open = false;
    pgs_close_decoder(in);
    talloc_free(in->pgs_text);
    in->pgs_text = NULL;
    in->pgs_pending = false;
    in->ocr_fail = 0;
    mp_mutex_lock(&in->alock);
    clear_queue_locked(in, true);
    in->nseen = 0;
    in->astop = in->aflush = in->areset = in->aeof = false;
    in->aswitch = false;
    in->acodec_next = NULL;
    mp_mutex_unlock(&in->alock);
    atomic_store(&in->ai_failed, false);
    atomic_store(&in->reset_pending, false);
    atomic_store(&in->ready_until, MP_NOPTS_VALUE);
    atomic_store(&in->engine_until, MP_NOPTS_VALUE);
    atomic_store(&in->idle, false);
    atomic_store(&in->done, false);
    atomic_store(&in->rebind, true);
    atomic_store(&in->rebind_sub, true);
}

// Starts the worker of an input (the engine DLL is loaded).
static bool input_start(struct input *in)
{
    atomic_store(&in->active, true);
    if (mp_thread_create(&in->athread, audio_thread, in) == 0) {
        atomic_store(&in->athread_started, true);
        return true;
    }
    atomic_store(&in->active, false);
    ai_fail(in, "cannot start the audio thread");
    return false;
}

// Input changes stop all scan jobs before replacing their shared engine API.
static void stop_live(struct nanum *n)
{
    atomic_store(&n->ai_enabled, false);
    input_stop(PGS(n));
    input_stop(SEC(n));
    input_stop(PRI(n));
    unload_engine(n);
}

static void start_live(struct nanum *n)
{
    if (atomic_load(&n->a_mode) == MODE_STT) {
        bool have_stt = n->opts->stt_model && n->opts->stt_model[0];
        bool have_vad = n->opts->vad_model && n->opts->vad_model[0];
        if (!have_stt) {
            set_ai(n, AI_UNAVAILABLE, "", "no speech model is configured (nanum-stt-model)", "");
            return;
        }
        if (!have_vad) {
            set_ai(n, AI_UNAVAILABLE, "", "nanum-stt-model needs nanum-vad-model", "");
            return;
        }
        bool stt_ok = mp_path_exists(n->opts->stt_model) ||
                      (n->opts->stt_model_multi && n->opts->stt_model_multi[0] &&
                       mp_path_exists(n->opts->stt_model_multi));
        bool vad_ok = mp_path_exists(n->opts->vad_model);
        if (!stt_ok || !vad_ok) {
            char why[sizeof(n->ai_reason)];
            snprintf(why, sizeof(why), "The speech models are not installed (missing: %s%s%s). "
                     "Download them in the AI settings.", stt_ok ? "" : "Whisper speech model",
                     !stt_ok && !vad_ok ? ", " : "", vad_ok ? "" : "voice detection model");
            mp_info(n->log, "AI subtitles are off: %s\n", why);
            set_ai(n, AI_UNAVAILABLE, "", why, "");
            return;
        }
    }
    if (!load_engine(n))
        return;
    atomic_store(&n->ai_enabled, true);
    atomic_store(&PRI(n)->bound, atomic_load(&n->a_mode) != MODE_STT);
    input_start(PRI(n));
}

// ---------------------------------------------------------------------------
// Full-mode state

static void set_full(struct nanum *n, int state, double progress, const char *file,
                     const char *err)
{
    mp_mutex_lock(&n->slock);
    n->fstate = state;
    n->fprogress = progress;
    snprintf(n->ffile, sizeof(n->ffile), "%s", file ? file : "");
    snprintf(n->ferror, sizeof(n->ferror), "%s", err ? err : "");
    mp_mutex_unlock(&n->slock);
}

// Progress only; progress is rounded so the property does not change needlessly.
static void set_progress(struct nanum *n, int state, double progress)
{
    progress = floor(MPCLAMP(progress, 0.0, 0.999) * 1000.0) / 1000.0;
    mp_mutex_lock(&n->slock);
    n->fstate = state;
    n->fprogress = progress;
    mp_mutex_unlock(&n->slock);
}

// Copies as many whole lines as fit.
static void copy_lines(char *dst, size_t sz, const char *src)
{
    size_t len = strlen(src);
    if (len < sz) {
        memcpy(dst, src, len + 1);
        return;
    }
    len = sz - 1;
    while (len > 0 && src[len] != '\n')
        len--;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void nanum_get_status(struct nanum *n, struct nanum_status *st)
{
    mp_mutex_lock(&n->slock);
    st->valid = true;
    st->full_state = fs_names[n->fstate];
    st->full_progress = n->fprogress;
    memcpy(st->full_file, n->ffile, sizeof(st->full_file));
    memcpy(st->full_error, n->ferror, sizeof(st->full_error));
    st->lib_state = ls_names[n->lstate];
    memcpy(st->lib_source_id, n->lsrc_id, sizeof(st->lib_source_id));
    memcpy(st->lib_source_lang, n->lsrc_lang, sizeof(st->lib_source_lang));
    st->lib_has_source = n->lhas_src;
    st->lib_has_translation = n->lhas_tr;
    memcpy(st->lib_summary, n->lsummary, sizeof(st->lib_summary));
    memcpy(st->lib_error, n->lerror, sizeof(st->lib_error));
    st->ai_state = ai_names[n->ai_state];
    memcpy(st->ai_backend, n->ai_backend, sizeof(st->ai_backend));
    memcpy(st->ai_reason, n->ai_reason, sizeof(st->ai_reason));
    memcpy(st->ai_notice, n->ai_notice, sizeof(st->ai_notice));
    st->sec_state = ss_names[n->sec_state];
    memcpy(st->sec_lang, n->sec_lang, sizeof(st->sec_lang));
    memcpy(st->sec_reason, n->sec_reason, sizeof(st->sec_reason));
    st->profile_state = ps_names[n->prof_state];
    memcpy(st->profile_work, n->prof_work, sizeof(st->profile_work));
    memcpy(st->original_lang, n->orig_lang, sizeof(st->original_lang));
    copy_lines(st->glossary, sizeof(st->glossary), n->glossary);
    st->glossary_count = n->glossary_count;
    st->gl_state = gs_names[n->gl_state];
    st->gl_progress = n->gl_progress;
    st->gl_changed = n->gl_changed;
    memcpy(st->gl_error, n->gl_error, sizeof(st->gl_error));
    st->pgs_state = ss_names[n->pgs_state];
    memcpy(st->pgs_lang, n->pgs_lang, sizeof(st->pgs_lang));
    memcpy(st->pgs_reason, n->pgs_reason, sizeof(st->pgs_reason));
    st->imp_state = is_names[n->imp_state];
    st->imp_progress = n->imp_progress;
    memcpy(st->imp_error, n->imp_error, sizeof(st->imp_error));
    memcpy(st->imp_note, n->imp_note, sizeof(st->imp_note));
    st->imp_offset = n->imp_offset;
    st->imp_scale = n->imp_scale;
    st->imp_confidence = n->imp_conf;
    st->imp_low_confidence = n->imp_low;
    memcpy(st->imp_lang, n->imp_lang, sizeof(st->imp_lang));
    memcpy(st->imp_origin, n->imp_origin, sizeof(st->imp_origin));
    st->imp_lines = n->imp_lines;
    mp_mutex_unlock(&n->slock);
    st->input_active = atomic_load(&n->a_mode) == MODE_STT ? "stt" : "sub";
    st->text_complete = atomic_load(&PRI(n)->text_complete);
    st->track_primary = virtual_id(IN_PRI);
    st->track_secondary = SEC(n)->sh ? virtual_id(IN_SEC) : -1;
    double r = atomic_load(&SEC(n)->ready_until);
    st->sec_ready_valid = r != MP_NOPTS_VALUE;
    st->sec_ready = r;
    st->track_pgs = PGS(n)->sh ? virtual_id(IN_PGS) : -1;
    st->track_import = IMPT(n)->sh ? virtual_id(IN_IMP) : -1;
    r = atomic_load(&PGS(n)->ready_until);
    st->pgs_ready_valid = r != MP_NOPTS_VALUE;
    st->pgs_ready = r;
}

// Library state as shown in demuxer-cache-state (nanum-library-*).
static void set_lib(struct nanum *n, int state, const char *source_id,
                    const char *lang, bool has_src, bool has_tr, const char *summary,
                    const char *err)
{
    mp_mutex_lock(&n->slock);
    n->lstate = state;
    snprintf(n->lsrc_id, sizeof(n->lsrc_id), "%s", source_id ? source_id : "");
    snprintf(n->lsrc_lang, sizeof(n->lsrc_lang), "%s", lang ? lang : "");
    n->lhas_src = has_src;
    n->lhas_tr = has_tr;
    snprintf(n->lsummary, sizeof(n->lsummary), "%s", summary ? summary : "");
    snprintf(n->lerror, sizeof(n->lerror), "%s", err ? err : "");
    mp_mutex_unlock(&n->slock);
}

void nanum_set_media(struct nanum *n, const char *url, double duration)
{
    mp_mutex_lock(&n->slock);
    talloc_free(n->url);
    n->url = url ? talloc_strdup(NULL, url) : NULL;    // not a child of n: other
                                                       // threads allocate there
    mp_mutex_unlock(&n->slock);
    atomic_store(&n->duration, duration);
}

void nanum_set_duration(struct nanum *n, double duration)
{
    atomic_store(&n->duration, duration);
}

void nanum_set_ts_offset(struct nanum *n, double offset)
{
    atomic_store(&n->ts_offset, offset);
    atomic_store(&n->ts_known, true);
}

bool nanum_set_playhead(struct nanum *n, double pts)
{
    if (pts == MP_NOPTS_VALUE)
        return false;
    atomic_store(&n->playhead, pts);
    // A cue that waits for the engine is due once playback gets there.
    for (int i = 0; i < NANUM_INPUTS; i++) {
        struct input *in = &n->in[i];
        if (atomic_load(&in->held) && pts >= atomic_load(&in->held_pts) - 0.25)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Image subtitle tracks (disc PGS / DVD): which one is read by OCR

static bool is_image_codec(const char *c)
{
    return c && (!strcmp(c, "hdmv_pgs_subtitle") || !strcmp(c, "dvd_subtitle"));
}

bool nanum_tap_only_stream(const struct sh_stream *sh)
{
    return sh->type == STREAM_AUDIO ||
           (sh->type == STREAM_SUB && sh->codec && is_image_codec(sh->codec->codec));
}

static bool csv_has(const char *list, const char *item)
{
    size_t l = strlen(item);
    for (const char *p = list; *p; ) {
        size_t len = strcspn(p, ",");
        if (len == l && !strncasecmp(p, item, l))
            return true;
        p += len + (p[len] ? 1 : 0);
    }
    return false;
}

static bool is_latin_lang(const char *l)
{
    static const char *const latin[] = {"en", "fr", "de", "es", "it", "pt", "nl", "sv", "da",
                                        "no", "fi", "pl", "cs", "hu", "ro", "tr", "id", "vi"};
    for (size_t i = 0; i < MP_ARRAY_SIZE(latin); i++) {
        if (!strcmp(l, latin[i]))
            return true;
    }
    return false;
}

// The OCR language that reads a track of the language 'lang': its own pack, for Latin
// scripts any installed pack that reads Latin letters (the Korean one does) with a note, and
// for other scripts nothing but their own pack.
static bool pick_ocr_lang(const char *installed, const char *lang, char ocr[8], char *note,
                          size_t nsz, char *reason, size_t rsz)
{
    note[0] = reason[0] = '\0';
    if (csv_has(installed, lang)) {
        snprintf(ocr, 8, "%s", lang);
        return true;
    }
    if (is_latin_lang(lang)) {
        static const char *const fallback[] = {"en", "fr", "de", "es", "it", "pt", "ko"};
        for (size_t i = 0; i < MP_ARRAY_SIZE(fallback); i++) {
            if (csv_has(installed, fallback[i])) {
                snprintf(ocr, 8, "%s", fallback[i]);
                snprintf(note, nsz, "%s OCR 팩 없음: %s 인식기로 읽는 중", lang_name_ko(lang),
                         lang_name_ko(fallback[i]));
                return true;
            }
        }
    }
    snprintf(reason, rsz, "%s OCR 언어 팩이 없습니다 (Windows 설정 > 시간 및 언어 > 언어 및 지역에서 추가)",
             lang_name_ko(lang));
    return false;
}

// Chooses the image subtitle track: non-forced English, else the work's original language,
// else any other non-forced one, then the forced ones in the same order. Only tracks an OCR
// pack can read count. Called from the demux thread (stream registration) and the ctl thread;
// it publishes the choice for nanum_wants_stream().
static void pgs_replan(struct nanum *n, struct pgs_plan *p)
{
    *p = (struct pgs_plan){.id = -1, .idx = -1};
    struct input *in = PGS(n);
    int best_score = -1, bad_score = -1;
    char bad_reason[256] = "";
    mp_mutex_lock(&n->slock);
    for (int i = 0; i < n->nstreams; i++) {
        const struct sinfo *s = &n->streams[i];
        if (s->type != STREAM_SUB || !s->image)
            continue;
        p->exists = true;
        char l[8], ocr[8], note[256], reason[256];
        iso2_from_mp(s->lang, l, sizeof(l));
        int score = (!strcmp(l, "en") ? 100 : !strcmp(l, n->orig_lang) ? 60 : 40) -
                    (s->forced ? 70 : 0);
        if (!strcmp(l, "auto")) {
            if (score > bad_score) {
                bad_score = score;
                snprintf(bad_reason, sizeof(bad_reason), "the image subtitle has no language");
            }
            continue;
        }
        if (!n->ocr_known ||
            !pick_ocr_lang(n->ocr_langs, l, ocr, note, sizeof(note), reason, sizeof(reason))) {
            if (score > bad_score) {
                bad_score = score;
                snprintf(bad_reason, sizeof(bad_reason), "%s",
                         n->ocr_known ? reason : "the OCR is not available");
            }
            continue;
        }
        if (score > best_score) {
            best_score = score;
            p->ok = true;
            p->id = s->id;
            p->idx = s->index;
            snprintf(p->lang, sizeof(p->lang), "%s", l);
            snprintf(p->ocr, sizeof(p->ocr), "%s", ocr);
            snprintf(p->note, sizeof(p->note), "%s", note);
        }
    }
    mp_mutex_unlock(&n->slock);
    if (!p->ok)
        snprintf(p->reason, sizeof(p->reason), "%s",
                 bad_reason[0] ? bad_reason : "no image subtitle track");
    bool enabled = atomic_load(&n->pgs_enabled) && atomic_load(&n->ai_enabled) && p->ok;
    atomic_store(&in->pin_id, enabled ? p->id : -1);
    atomic_store(&in->pin_idx, enabled ? p->idx : -1);
    atomic_store(&n->pgs_wanted, enabled);
}

// Streams the demuxer must deliver even if the player does not play them: the
// text subtitle track and the audio tracks the AI inputs are pinned to (and, until the
// secondary input is decided, the audio tracks that may become it).
bool nanum_wants_stream(struct nanum *n, const struct sh_stream *sh)
{
    if (is_virtual_id(sh->demuxer_id))
        return false;
    if (sh->type == STREAM_AUDIO && sh->index < 64 &&
        ((atomic_load(&n->cand_mask) >> sh->index) & 1))
        return true;
    // One stream per id (see resolve_stream_idx()).
    if (sh->type == STREAM_AUDIO && atomic_load(&n->ai_enabled)) {
        struct input *sec = SEC(n);
        if (atomic_load(&sec->active) && sh->index == atomic_load(&sec->pin_idx) &&
            stream_key(sh) == atomic_load(&sec->pin_id))
            return true;
    }
    if (sh->type == STREAM_SUB && atomic_load(&n->pgs_wanted) &&
        sh->index == atomic_load(&PGS(n)->pin_idx))
        return true;
    if (atomic_load(&n->gen_off))
        return false;
    int mode = atomic_load(&n->a_mode);
    if (mode == MODE_SUB_EMB && sh->type == STREAM_SUB &&
        sh->demuxer_id == atomic_load(&n->a_sub_id) && is_text_codec(sh->codec->codec))
        return true;
    struct input *pri = PRI(n);
    int pin = atomic_load(&pri->pin_id);
    return mode == MODE_STT && n->ai_enabled && sh->type == STREAM_AUDIO && pin >= 0 &&
           stream_key(sh) == pin && sh->index == atomic_load(&pri->pin_idx);
}

void nanum_register_stream(struct nanum *n, const struct sh_stream *sh)
{
    if ((sh->type != STREAM_AUDIO && sh->type != STREAM_SUB) ||
        is_virtual_id(sh->demuxer_id))
        return;
    mp_mutex_lock(&n->slock);
    for (int i = 0; i < n->nstreams; i++) {
        if (n->streams[i].index == sh->index) {
            mp_mutex_unlock(&n->slock);
            return;
        }
    }
    // Plain realloc: this runs under the demux lock, from the demux thread.
    struct sinfo *s = realloc(n->streams, (n->nstreams + 1) * sizeof(*s));
    if (s) {
        n->streams = s;
        s = &n->streams[n->nstreams++];
        *s = (struct sinfo){.index = sh->index, .id = stream_key(sh), .type = sh->type,
                            .def = sh->default_track, .forced = sh->forced_track};
        snprintf(s->lang, sizeof(s->lang), "%s", sh->lang ? sh->lang : "");
        snprintf(s->codec, sizeof(s->codec), "%s",
                 sh->codec && sh->codec->codec ? sh->codec->codec : "");
        s->image = sh->type == STREAM_SUB && is_image_codec(s->codec);
    }
    // Several audio languages? (The candidates of the secondary input need that.)
    char first[8] = "";
    n->multi_lang = false;
    for (int i = 0; i < n->nstreams; i++) {
        const struct sinfo *o = &n->streams[i];
        char l[8];
        if (o->type != STREAM_AUDIO)
            continue;
        iso2_from_mp(o->lang, l, sizeof(l));
        if (!strcmp(l, "auto"))
            continue;
        if (!first[0])
            snprintf(first, sizeof(first), "%s", l);
        else if (strcmp(first, l))
            n->multi_lang = true;
    }
    mp_mutex_unlock(&n->slock);
    resolve_pin(n);
    if (sh->type == STREAM_SUB) {
        struct pgs_plan plan;
        pgs_replan(n, &plan);
    }
    // The pinned/subtitle input may have just become known.
    mp_mutex_lock(&n->clock);
    n->ctl_kick = true;
    mp_cond_signal(&n->ccond);
    mp_mutex_unlock(&n->clock);
}

// The player plays this audio stream: the playback input of the AI, unless one is pinned.
// The streams that may become the secondary input are then delivered from the start (see
// struct nanum), so none of the audio the demuxer reads before the ctl thread has decided
// is lost.
void nanum_audio_selected(struct nanum *n, const struct sh_stream *sh)
{
    if (is_virtual_id(sh->demuxer_id))
        return;
    atomic_store(&n->audio_played, true);
    struct input *pri = PRI(n);
    int none = -1;
    if (atomic_load(&n->a_mode) == MODE_STT &&
        atomic_compare_exchange_strong(&pri->pin_id, &none, stream_key(sh))) {
        mp_mutex_lock(&n->clock);
        n->ctl_kick = true;
        mp_cond_signal(&n->ccond);
        mp_mutex_unlock(&n->clock);
    }
    resolve_pin(n);
}

void nanum_set_disc_identity(struct nanum *n, const char *identity)
{
    mp_mutex_lock(&n->slock);
    free(n->disc_identity);
    n->disc_identity = identity ? strdup(identity) : NULL;
    mp_mutex_unlock(&n->slock);
}

void nanum_set_work_titles(struct nanum *n, const char *disc_name, const char *meta_title)
{
    mp_mutex_lock(&n->slock);
    free(n->disc_name);
    free(n->meta_title);
    n->disc_name = disc_name && disc_name[0] ? strdup(disc_name) : NULL;
    n->meta_title = meta_title && meta_title[0] ? strdup(meta_title) : NULL;
    mp_mutex_unlock(&n->slock);
}

static void ctl_kick(struct nanum *n)
{
    mp_mutex_lock(&n->clock);
    n->ctl_kick = true;
    mp_cond_signal(&n->ccond);
    mp_mutex_unlock(&n->clock);
}

void nanum_set_select_cb(struct nanum *n, void (*cb)(void *ctx), void *ctx)
{
    mp_mutex_lock(&n->clock);
    n->select_cb = cb;
    n->select_ctx = ctx;
    n->select_pending = true;
    n->ctl_kick = true;
    mp_cond_signal(&n->ccond);
    mp_mutex_unlock(&n->clock);
}

// Called by the options cache (with its lock held): only wake the ctl thread.
static void opts_wakeup(void *p)
{
    ctl_kick(p);
}

// ---------------------------------------------------------------------------
// Library glue: summaries, publishing, finished results

static const char *base_name(const char *p)
{
    const char *b = p;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\')
            b = p + 1;
    }
    return b;
}

// Opaque identity of one AI input of one media, for nanum-library-source-id.
static void src_pub_id(const struct libsrc *ls, char out[40])
{
    char hex[65];
    char *k = talloc_asprintf(NULL, "%s|%s", ls->media_key, ls->source_key);
    lib_sha256_hex(k, strlen(k), hex);
    talloc_free(k);
    snprintf(out, 40, "%.32s", hex);
}

static void make_summary(const struct lib_found *f, char *buf, size_t sz)
{
    char date[40] = "";
    time_t t = (time_t)(f->has_tr ? f->tr_created : f->src_created);
    struct tm *tm = localtime(&t);
    if (tm)
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", tm);
    if (f->has_tr) {
        int n = snprintf(buf, sz, "%s -> %s | %s | %s", f->src_lang, f->tr_target,
                         f->tr_model, date);
        if (f->tr_failed > 0 && n > 0 && (size_t)n < sz)
            snprintf(buf + n, sz - n, " | %d untranslated", f->tr_failed);
    } else if (f->has_source) {
        snprintf(buf, sz, "%s source, not translated | %s | %s", f->src_lang,
                 f->src_engine, date);
    } else {
        buf[0] = '\0';
    }
}

// Looks the input up again and publishes it with the given state.
static void lib_publish(struct nanum *n, const struct libsrc *ls, int state,
                        const char *err)
{
    if (!ls->save || !n->lib) {
        // Results that cannot be stored are never silent: the state is "error"
        // with the reason, even though the translation itself may be in use.
        set_lib(n, ls->warn[0] ? LS_ERROR : state, "", ls->lang[0] ? ls->lang : "", false,
                false, "", err ? err : ls->warn);
        return;
    }
    struct lib_found f;
    char id[40], sum[256];
    src_pub_id(ls, id);
    if (!lib_lookup(n->lib, ls->media_id, ls->source_key, ls->target, &f)) {
        set_lib(n, LS_ERROR, id, ls->lang, false, false, "", lib_error(n->lib));
        return;
    }
    make_summary(&f, sum, sizeof(sum));
    set_lib(n, state, id, f.has_source ? f.src_lang : ls->lang, f.has_source, f.has_tr,
            sum, err);
}

static void fmt_ass_time(char *buf, size_t sz, double t)
{
    int64_t cs = llround(MPMAX(t, 0.0) * 100);
    snprintf(buf, sz, "%d:%02d:%02d.%02d", (int)(cs / 360000),
             (int)(cs / 6000 % 60), (int)(cs / 100 % 60), (int)(cs % 100));
}

static const char *view_tag(int view)
{
    return view == DISP_BILINGUAL ? "bi" : view == DISP_SOURCE ? "src" : "tr";
}

// Writes the cues for one presentation (tmp file + rename). The player shifts
// demuxer timestamps by 'off', so the file is relative to playback, not to the
// container timeline the cues are stored on.
static const char *write_ass_file(const char *dir, const char *path,
                                  const struct lib_cue *cues, size_t nc, double off,
                                  int view)
{
    mp_mkdirp(dir);
    void *tmpctx = talloc_new(NULL);
    char *tmp = talloc_asprintf(tmpctx, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        talloc_free(tmpctx);
        return "cannot write the translation file";
    }
    fwrite(ass_header, 1, sizeof(ass_header) - 1, f);
    for (size_t i = 0; i < nc; i++) {
        char *text = build_ass_text(tmpctx, view, cues[i].src, cues[i].tr);
        if (!text)
            continue;
        double st = cues[i].pts + off;
        char t1[32], t2[32];
        fmt_ass_time(t1, sizeof(t1), st);
        fmt_ass_time(t2, sizeof(t2),
                     display_end(st, cues[i].dur,
                                 i + 1 < nc ? cues[i + 1].pts + off : INFINITY));
        fprintf(f, "Dialogue: 0,%s,%s,Default,,0,0,0,,%s\n", t1, t2, text);
        talloc_free(text);
    }
    bool bad = fflush(f) != 0 || ferror(f);
    bad |= fclose(f) != 0;
    const char *err = NULL;
    if (bad) {
        unlink(tmp);
        err = "cannot write the translation file";
    } else if (rename(tmp, path) != 0) {
        unlink(tmp);
        err = "cannot move the translation file into place";
    }
    talloc_free(tmpctx);
    return err;
}

// Writes the finished result in the current presentation and timeline offset
// and, if announce (or the full state is already "done"), publishes the file.
// A file named after (result, presentation, offset) is immutable: it is reused.
static void render_done(struct nanum *n, bool announce)
{
    mp_mutex_lock(&n->rlock);
    if (!n->dcues) {
        mp_mutex_unlock(&n->rlock);
        return;
    }
    int view = effective_view(n);
    double off = atomic_load(&n->ts_offset);
    char *path = talloc_asprintf(NULL, "%s/%s-%s-o%lld.ass", n->ddir, n->dstem,
                                 view_tag(view), (long long)llround(off * 1000));
    const char *err = NULL;
    if (!n->drendered || strcmp(n->drendered, path)) {
        if (!mp_path_exists(path))
            err = write_ass_file(n->ddir, path, n->dcues, n->ndcues, off, view);
        if (!err) {
            talloc_free(n->drendered);
            n->drendered = talloc_strdup(n->dctx, path);
        }
    }
    bool done;
    mp_mutex_lock(&n->slock);
    done = n->fstate == FS_DONE;
    mp_mutex_unlock(&n->slock);
    if (err) {
        mp_err(n->log, "%s: %s\n", err, path);
        if (announce || done)
            set_full(n, FS_ERROR, 0, "", err);
    } else if (announce || done) {
        set_full(n, FS_DONE, 1.0, path, "");
    }
    mp_mutex_unlock(&n->rlock);
    talloc_free(path);
}

// Keeps the finished result (for re-rendering when only the presentation or the
// timeline offset changes) and publishes its file. ls says how it is stored in the
// library and source_rev what it was translated from; glossary is the glossary it was
// translated with (NULL: the one the library loads).
static void publish_done(struct nanum *n, const char *dir, const char *stem,
                         const struct lib_cue *cues, size_t nc, const struct libsrc *ls,
                         int64_t source_rev, const char *glossary)
{
    void *ctx = talloc_new(NULL);
    struct lib_cue *copy = talloc_array(ctx, struct lib_cue, nc);
    for (size_t i = 0; i < nc; i++) {
        copy[i] = (struct lib_cue){
            .pts = cues[i].pts,
            .dur = cues[i].dur,
            .src = talloc_strdup(copy, cues[i].src),
            .tr = cues[i].tr ? talloc_strdup(copy, cues[i].tr) : NULL,
        };
    }
    char *d = talloc_strdup(ctx, dir);
    char *s = talloc_strdup(ctx, stem);
    struct libsrc keep = *ls;
    keep.media_key = talloc_strdup(ctx, ls->media_key ? ls->media_key : "");
    keep.source_key = talloc_strdup(ctx, ls->source_key ? ls->source_key : "");
    keep.input = talloc_strdup(ctx, ls->input ? ls->input : "");
    char *gl = glossary ? talloc_strdup(ctx, glossary) : NULL;
    mp_mutex_lock(&n->rlock);
    talloc_free(n->dctx);
    n->dctx = ctx;
    n->dcues = copy;
    n->ndcues = nc;
    n->ddir = d;
    n->dstem = s;
    n->drendered = NULL;
    n->dls = keep;
    n->dsource_rev = source_rev;
    n->dgl = gl;
    mp_mutex_unlock(&n->rlock);
    render_done(n, true);
}

static char *result_dir(void *ctx, const struct rt_cfg *c)
{
    char *dir;
    if (c->full_dir[0]) {
        dir = talloc_strdup(ctx, c->full_dir);
    } else {
        const char *base = getenv("LOCALAPPDATA");
        dir = talloc_asprintf(ctx, "%s/NanumPlayer/full", base && base[0] ? base : ".");
    }
    for (char *s = dir; *s; s++) {
        if (*s == '\\')
            *s = '/';
    }
    size_t l = strlen(dir);
    while (l > 1 && dir[l - 1] == '/')
        dir[--l] = '\0';
    return dir;
}

// <media>-t<translation row>: a saved result is immutable, so its files are too.
// Results without a library identity get a name nothing will ever look up again.
static char *result_stem(void *ctx, const struct libsrc *ls, int64_t tr_id)
{
    if (ls->save && tr_id) {
        char hex[65];
        lib_sha256_hex(ls->media_key, strlen(ls->media_key), hex);
        return talloc_asprintf(ctx, "%.16s-t%lld", hex, (long long)tr_id);
    }
    static atomic_uint seq;
    return talloc_asprintf(ctx, "unsaved-%lld-%u", (long long)(mp_time_ns() / 1000000),
                           atomic_fetch_add(&seq, 1));
}

// ---------------------------------------------------------------------------
// Jobs (full scan, retranslation of a saved transcript, external subtitle file)

static bool job_cancelled(struct job *j)
{
    return atomic_load(&j->cancel);
}

static const char *job_name(const struct job *j)
{
    return j->kind == JOB_SCAN ? "full scan" : j->kind == JOB_RETR ? "retranslation"
           : j->kind == JOB_GLOSS ? "glossary" : j->kind == JOB_IMPORT ? "import"
                                                                        : "subtitle file";
}

static void job_error(struct job *j, const char *fmt, ...) MP_PRINTF_ATTRIBUTE(2, 3);
static void job_error(struct job *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = talloc_vasprintf(NULL, fmt, ap);
    va_end(ap);
    for (char *s = msg; *s; s++) {
        if (*s == '\n' || *s == '\r')
            *s = ' ';
    }
    mp_err(j->n->log, "%s: %s\n", job_name(j), msg);
    if (j->kind != JOB_EXT && !job_cancelled(j)) {
        set_full(j->n, FS_ERROR, 0, "", msg);
        // Whatever was saved before stays available.
        lib_publish(j->n, &j->ls, LS_ERROR, msg);
        if (j->job_row)
            lib_job_end(j->n->lib, j->job_row, "failed", msg);
        j->job_row = 0;
    }
    talloc_free(msg);
}

// Opens the media/subtitle file with a demuxer of our own and selects the one
// stream to read. Returns an error message or NULL.
static const char *open_source(struct job *j, const char *url, bool audio,
                               bool external, struct demuxer **out_d,
                               struct sh_stream **out_sh)
{
    struct nanum *n = j->n;
    const struct rt_cfg *c = j->cfg;
    struct demuxer_params p = {
        .is_top_level = false,
        .stream_flags = STREAM_ORIGIN_DIRECT,
        .allow_playlist_create = false,
    };
    struct demuxer *d = demux_open_url(url, &p, j->mpcancel, n->global);
    if (!d)
        return "cannot open the source";

    struct sh_stream *pick = NULL, *fallback = NULL;
    int num = demux_get_num_stream(d);
    for (int i = 0; i < num; i++) {
        struct sh_stream *sh = demux_get_stream(d, i);
        if (audio) {
            if (sh->type != STREAM_AUDIO)
                continue;
            if (j->a_srcid >= 0 && stream_key(sh) == j->a_srcid) {
                pick = sh;
                break;
            }
            if (!fallback || (sh->default_track && !fallback->default_track))
                fallback = sh;
        } else {
            if (sh->type != STREAM_SUB || !is_text_codec(sh->codec->codec))
                continue;
            if (c->sub_id >= 0 && sh->demuxer_id == c->sub_id) {
                pick = sh;
                break;
            }
            if (external && !fallback)
                fallback = sh;
        }
    }
    // A pinned audio track is never replaced by another one: that would store
    // the wrong language under the pinned identity.
    if (!pick && !(audio && j->a_srcid >= 0))
        pick = fallback;
    if (!pick) {
        demux_free(d);
        return audio ? (j->a_srcid >= 0 ? "the pinned audio track was not found"
                                        : "the media has no audio track")
                     : "the text subtitle track was not found";
    }
    demuxer_select_track(d, pick, MP_NOPTS_VALUE, true);
    *out_d = d;
    *out_sh = pick;
    return NULL;
}

static void scan_event_cb(void *user, double pts, double duration,
                          const char *source, const char *translation)
{
    // scan_only engines never call this.
}

static void imp_progress(struct nanum *n, int state, double progress);

// Translation progress: of the full translation, or of an import.
static void job_progress(struct job *j, double progress)
{
    if (j->kind == JOB_IMPORT)
        imp_progress(j->n, IS_TRANSLATING, progress);
    else
        set_progress(j->n, FS_TRANSLATING, progress);
}

static void full_cb(void *user, double progress, const struct nanum_ai_line *lines,
                    size_t nl, const char *error)
{
    struct job *j = user;
    if (!error && !(lines && nl)) {
        if (!job_cancelled(j))
            job_progress(j, progress);
        return;
    }
    // Final call: keep a copy, the engine owns the pointers only for now.
    mp_mutex_lock(&j->dlock);
    if (error) {
        j->err = talloc_strdup(j, error);
    } else {
        j->lines = talloc_array(j, struct nanum_ai_line, nl);
        for (size_t i = 0; i < nl; i++) {
            j->lines[i] = (struct nanum_ai_line){
                .pts = lines[i].pts,
                .duration = lines[i].duration,
                .source = talloc_strdup(j->lines, lines[i].source ? lines[i].source : ""),
                .translation = talloc_strdup(j->lines,
                                    lines[i].translation ? lines[i].translation : ""),
            };
        }
        j->nlines = nl;
    }
    j->done = true;
    mp_cond_signal(&j->dcond);
    mp_mutex_unlock(&j->dlock);
}

// One consistent snapshot of the engine's transcript (see nanum_ai.h).
static void export_cb(void *user, const struct nanum_ai_line *lines, size_t nl)
{
    struct job *j = user;
    j->src = talloc_array(j, struct lib_cue, nl);
    for (size_t i = 0; i < nl; i++) {
        j->src[i] = (struct lib_cue){
            .pts = lines[i].pts,
            .dur = lines[i].duration,
            .src = talloc_strdup(j->src, lines[i].source ? lines[i].source : ""),
        };
    }
    j->nsrc = nl;
}

// Wait for the live path to produce its first lines (or 10 s), so the scan
// does not compete with the start-up of playback for the disc and the GPU.
// Nothing to wait for if the live path is off.
static bool scan_throttle(struct job *j)
{
    int64_t t0 = mp_time_ns();
    while (!job_cancelled(j)) {
        if (atomic_load(&j->n->gen_off) || atomic_load(&PRI(j->n)->events_seen) ||
            mp_time_ns() - t0 >= MP_TIME_S_TO_NS(10))
            return true;
        mp_sleep_ns(MP_TIME_MS_TO_NS(100));
    }
    return false;
}

// translate_all() on the collected transcript. 0: finished, 1: cancelled,
// -1: failed (errbuf).
static int job_translate(struct job *j, struct nanum_ai *ai, char *errbuf, size_t esz)
{
    struct nanum *n = j->n;
    const struct nanum_ai_api *api = j->kind == JOB_IMPORT ? &j->api : &n->api;
    job_progress(j, 0);
    if (api->translate_all(ai, full_cb, j)) {
        snprintf(errbuf, esz, "cannot start the translation");
        return -1;
    }
    mp_mutex_lock(&j->dlock);
    while (!j->done && !job_cancelled(j))
        mp_cond_timedwait(&j->dcond, &j->dlock, MP_TIME_MS_TO_NS(200));
    bool done = j->done;
    mp_mutex_unlock(&j->dlock);
    if (!done)
        return 1;
    if (j->err) {
        snprintf(errbuf, esz, "%s", j->err);
        return -1;
    }
    return 0;
}

static void glossary_apply(struct nanum *n, const char *text);

// The full run extracted names on top of the glossary it was given: the merged list is
// the glossary of the whole media from now on. Returns it (a child of ctx).
static char *job_keep_glossary(struct job *j, struct nanum_ai *ai, void *ctx)
{
    struct nanum *n = j->n;
    const struct nanum_ai_api *api = j->kind == JOB_IMPORT ? &j->api : &n->api;
    size_t cap = 16384;
    char *buf = talloc_array(ctx, char, cap);
    size_t len = api->get_glossary(ai, buf, cap);
    if (len >= cap) {
        cap = len + 1;
        buf = talloc_realloc(ctx, buf, char, cap);
        api->get_glossary(ai, buf, cap);
    }
    glossary_apply(n, buf);
    if (j->ls.save && n->lib) {
        char *key = talloc_asprintf(ctx, "glossary.%s", j->ls.target);
        lib_prop_set(n->lib, j->ls.media_id, key, buf);
    }
    return buf;
}

// The translation finished: store it, keep it for rendering and publish it. A
// failure to store never loses the result of the work, it is just not saved.
static const char *job_finish(struct job *j, struct nanum_ai *ai)
{
    struct nanum *n = j->n;
    if (j->nlines != j->nsrc)
        return "the translation does not match the transcript";
    struct lib_cue *cues = talloc_array(j, struct lib_cue, j->nlines);
    for (size_t i = 0; i < j->nlines; i++) {
        cues[i] = (struct lib_cue){
            .pts = j->src[i].pts,
            .dur = j->src[i].dur,
            .src = j->src[i].src,
            .tr = (char *)j->lines[i].translation,
        };
    }
    int64_t tr_id = 0;
    char warn[256] = "";
    if (j->ls.save) {
        tr_id = lib_save_translation(n->lib, j->ls.source_rev, j->ls.target,
                                     j->ls.tr_engine, j->ls.tr_model, cues, j->nlines);
        if (!tr_id) {
            snprintf(warn, sizeof(warn), "cannot save the translation: %s",
                     lib_error(n->lib));
            mp_err(n->log, "%s\n", warn);
        }
    }
    void *tmp = talloc_new(NULL);
    char *dir = result_dir(tmp, j->cfg);
    char *stem = result_stem(tmp, &j->ls, tr_id);
    // The job row is closed ("done") before anything is published, so a cancel
    // arriving afterwards (e.g. quitting) cannot turn a finished job into a
    // cancelled one. The library state comes next: a poller that sees full-state
    // "done" must find the matching library state already.
    if (j->job_row) {
        lib_job_end(n->lib, j->job_row, "done", "");
        j->job_row = 0;
    }
    lib_publish(n, &j->ls, warn[0] ? LS_ERROR : LS_USING, warn[0] ? warn : NULL);
    char *glossary = job_keep_glossary(j, ai, tmp);
    publish_done(n, dir, stem, cues, j->nlines, &j->ls, j->ls.source_rev, glossary);
    talloc_free(tmp);
    mp_info(n->log, "translation ready: %zu lines%s\n", j->nlines,
            tr_id ? " (saved)" : " (not saved)");
    return NULL;
}

static void scan_run(struct job *j)
{
    struct nanum *n = j->n;
    const struct rt_cfg *c = j->cfg;
    bool sub = rt_input(c) == INPUT_SUB;
    struct demuxer *d = NULL;
    struct sh_stream *sh = NULL;
    struct nanum_ai *ai = NULL;
    struct adec dec = {.log = n->log, .api = &n->api};
    const char *err = NULL;
    char errbuf[320];
    char lang[8] = "auto";
    double dur = 0, scanned = MP_NOPTS_VALUE;
    int64_t next_pace = 0, next_prog = 0;
    double base = 0;

    if (!scan_throttle(j))
        return;

    const char *src = sub && c->sub_file[0] ? c->sub_file : j->url;
    mp_info(n->log, "full scan of '%.100s%s' (%s)\n", src, strlen(src) > 100 ? "..." : "",
            sub ? "subtitle track" : "audio");
    err = open_source(j, src, !sub, sub && c->sub_file[0], &d, &sh);
    if (err)
        goto fail;
    dur = j->duration > 0 ? j->duration : d->duration;
    if (d->start_time != MP_NOPTS_VALUE && d->start_time > 0)
        base = d->start_time;       // e.g. a disc segment starting at 496 s
    j->ls.origin = base;

    bool follow = !strcmp(n->opts->language, "auto");
    iso2_from_mp(sh->lang, lang, sizeof(lang));
    if (!strcmp(j->ls.lang, "und") && strcmp(lang, "auto"))
        snprintf(j->ls.lang, sizeof(j->ls.lang), "%s", lang);
    struct nanum_ai_config cfg = {
        .abi_version = NANUM_AI_ABI_VERSION,
        .stt_model_path = n->opts->stt_model,
        .vad_model_path = n->opts->vad_model,
        .stt_model_multilingual_path = n->opts->stt_model_multi,
        .language = sub ? (strcmp(lang, "auto") ? lang : "en")
                        : (follow ? lang : n->opts->language),
        .use_gpu = sub ? 0 : n->engine.use_gpu,
        .translate_endpoint = n->opts->tr_endpoint,
        .translate_model = n->opts->tr_model,
        .target_language = n->opts->target_language,
        .translate_engine = c->engine == ENGINE_OMP ? "omp" : "ollama",
        .omp_model = c->omp_model,
        .omp_command = c->omp_command,
        .scan_only = 1,
        .translate_only = sub,
    };
    struct tr_snap ts;
    tr_snapshot(n, j, &ts);
    cfg.translate_style = ts.style;
    cfg.translate_style_text = ts.style_text;
    cfg.glossary = ts.glossary;
    ai = n->api.create(&cfg, scan_event_cb, j);
    if (!ai) {
        snprintf(errbuf, sizeof(errbuf), "cannot start the scan engine: %s",
                 n->api.last_error());
        err = errbuf;
        goto fail;
    }
    dec.ai = ai;
    dec.codec = sh->codec;

    while (!job_cancelled(j)) {
        struct demux_packet *pkt = NULL;
        int r = demux_read_packet_async(sh, &pkt);
        if (r < 0)
            break;
        if (r == 0 || !pkt) {
            mp_sleep_ns(MP_TIME_MS_TO_NS(10));
            continue;
        }
        if (pkt->pts != MP_NOPTS_VALUE) {
            if (sub) {
                char *text = packet_text(NULL, sh->codec->codec, pkt);
                if (text) {
                    n->api.push_text(ai, pkt->pts, pkt->duration > 0 ? pkt->duration : 3.0,
                                     text);
                    talloc_free(text);
                }
            } else {
                if (!dec.avctx && !open_decoder(&dec)) {
                    talloc_free(pkt);
                    err = "cannot decode the audio";
                    goto fail;
                }
                decode_packet(&dec, pkt);
            }
            scanned = pkt->pts;
        }
        talloc_free(pkt);

        int64_t now = mp_time_ns();
        if (sub) {
            if (now >= next_prog && dur > 0 && scanned != MP_NOPTS_VALUE) {
                next_prog = now + MP_TIME_MS_TO_NS(250);
                set_progress(n, FS_SCANNING, (scanned - base) / dur);
            }
            continue;
        }
        if (scanned == MP_NOPTS_VALUE || now < next_pace)
            continue;
        next_pace = now + MP_TIME_MS_TO_NS(100);

        // The engine copies audio without limit: do not run more than 90 s
        // ahead of what it has recognised. (If it reports nothing for 20 s,
        // carry on.)
        int64_t nan_since = 0;
        while (!job_cancelled(j)) {
            double pu = n->api.processed_until(ai);
            if (dur > 0)
                set_progress(n, FS_SCANNING, (isnan(pu) ? 0 : pu - base) / dur);
            if (isnan(pu) ? scanned - base < 90 : scanned - pu < 90)
                break;
            if (isnan(pu)) {
                now = mp_time_ns();
                if (!nan_since)
                    nan_since = now;
                else if (now - nan_since > MP_TIME_S_TO_NS(20))
                    break;
            }
            mp_sleep_ns(MP_TIME_MS_TO_NS(100));
        }
    }
    if (job_cancelled(j))
        goto out;

    // The demuxer is not needed anymore: release the file before the long part.
    demux_free(d);
    d = NULL;
    adec_free(&dec);

    if (!sub) {
        n->api.flush(ai);
        while (!job_cancelled(j) && !n->api.wait_idle(ai, 200)) {
            double pu = n->api.processed_until(ai);
            if (dur > 0)
                set_progress(n, FS_SCANNING, (isnan(pu) ? 0 : pu - base) / dur);
        }
        if (job_cancelled(j))
            goto out;
    }

    // The source transcript is stored BEFORE the translation starts: if the
    // translation fails or is cancelled it can be retranslated later.
    n->api.transcript_export(ai, export_cb, j);
    if (j->nsrc == 0) {
        err = sub ? "the subtitle track has no text" : "no speech was recognised";
        goto fail;
    }
    mp_info(n->log, "transcript: %zu lines, translating\n", j->nsrc);
    if (j->ls.save) {
        j->ls.source_rev = lib_save_source(n->lib, j->ls.media_id, j->ls.source_key,
                                           j->ls.input, j->ls.track_id, j->ls.lang,
                                           j->ls.engine, j->ls.origin, j->src, j->nsrc);
        if (!j->ls.source_rev) {
            mp_err(n->log, "cannot save the transcript: %s\n", lib_error(n->lib));
            snprintf(j->ls.warn, sizeof(j->ls.warn), "results are not saved: %s",
                     lib_error(n->lib));
            j->ls.save = false;     // translate anyway, just do not store anything
        } else {
            lib_publish(n, &j->ls, LS_WORKING, NULL);
        }
    }

    int r = job_translate(j, ai, errbuf, sizeof(errbuf));
    if (r > 0)
        goto out;       // cancelled
    if (r < 0) {
        err = errbuf;
        goto fail;
    }
    err = job_finish(j, ai);
    if (err)
        goto fail;
    goto out;

fail:
    job_error(j, "%s", err);
out:
    if (ai) {
        n->api.cancel_full(ai);     // no callback after this
        n->api.destroy(ai);
    }
    adec_free(&dec);
    if (d)
        demux_free(d);
}

// Translates the saved source transcript again. No demuxer, no decoder, no
// speech recognition: the lines come from the library.
static void retr_run(struct job *j)
{
    struct nanum *n = j->n;
    const struct rt_cfg *c = j->cfg;
    struct nanum_ai *ai = NULL;
    const char *err = NULL;
    char errbuf[320];

    mp_info(n->log, "retranslating the saved transcript (revision %lld)\n",
            (long long)j->ls.source_rev);
    if (!lib_load_cues(n->lib, j, j->ls.source_rev, 0, &j->src, &j->nsrc, NULL)) {
        snprintf(errbuf, sizeof(errbuf), "cannot load the saved transcript: %s",
                 lib_error(n->lib));
        err = errbuf;
        goto fail;
    }
    struct nanum_ai_config cfg = {
        .abi_version = NANUM_AI_ABI_VERSION,
        .language = strcmp(j->ls.lang, "und") ? j->ls.lang : "en",
        .translate_endpoint = n->opts->tr_endpoint,
        .translate_model = n->opts->tr_model,
        .target_language = n->opts->target_language,
        .translate_engine = c->engine == ENGINE_OMP ? "omp" : "ollama",
        .omp_model = c->omp_model,
        .omp_command = c->omp_command,
        .scan_only = 1,
        .translate_only = 1,
    };
    struct tr_snap ts;
    tr_snapshot(n, j, &ts);
    cfg.translate_style = ts.style;
    cfg.translate_style_text = ts.style_text;
    cfg.glossary = ts.glossary;
    ai = n->api.create(&cfg, scan_event_cb, j);
    if (!ai) {
        snprintf(errbuf, sizeof(errbuf), "cannot start the translation engine: %s",
                 n->api.last_error());
        err = errbuf;
        goto fail;
    }
    for (size_t i = 0; i < j->nsrc; i++)
        n->api.push_text(ai, j->src[i].pts, j->src[i].dur, j->src[i].src);
    if (n->api.transcript_size(ai) != j->nsrc) {
        snprintf(errbuf, sizeof(errbuf),
                 "the saved transcript could not be restored (%zu of %zu lines)",
                 n->api.transcript_size(ai), j->nsrc);
        err = errbuf;
        goto fail;
    }
    int r = job_translate(j, ai, errbuf, sizeof(errbuf));
    if (r > 0)
        goto out;
    if (r < 0) {
        err = errbuf;
        goto fail;
    }
    err = job_finish(j, ai);
    if (err)
        goto fail;
    goto out;

fail:
    job_error(j, "%s", err);
out:
    if (ai) {
        n->api.cancel_full(ai);
        n->api.destroy(ai);
    }
}

// Reads a whole external subtitle file into the live engine.
static void ext_run(struct job *j)
{
    struct nanum *n = j->n;
    struct demuxer *d = NULL;
    struct sh_stream *sh = NULL;
    const char *err = open_source(j, j->cfg->sub_file, false, true, &d, &sh);
    if (err) {
        // (a job that was replaced while it opened the file is not a failure)
        if (!job_cancelled(j))
            job_error(j, "%s (%.100s)", err, j->cfg->sub_file);
        return;
    }
    {
        char lang[sizeof(PRI(n)->alang)];
        iso2_from_mp(sh->lang, lang, sizeof(lang));
        if (strcmp(lang, "auto")) {
            live_queue_lang(PRI(n), lang);
            mp_mutex_lock(&n->slock);
            snprintf(n->ext_lang, sizeof(n->ext_lang), "%s", lang);
            mp_mutex_unlock(&n->slock);
        }
    }
    int lines = 0;
    while (!job_cancelled(j) && !atomic_load(&PRI(n)->ai_failed)) {
        struct demux_packet *pkt = NULL;
        int r = demux_read_packet_async(sh, &pkt);
        if (r < 0)
            break;
        if (r == 0 || !pkt) {
            mp_sleep_ns(MP_TIME_MS_TO_NS(10));
            continue;
        }
        char *text = pkt->pts != MP_NOPTS_VALUE
                     ? packet_text(NULL, sh->codec->codec, pkt) : NULL;
        if (text) {
            live_queue_text(PRI(n), pkt->pts, pkt->duration > 0 ? pkt->duration : 3.0,
                            text);
            lines++;
            talloc_free(text);
        }
        talloc_free(pkt);
    }
    if (!job_cancelled(j) && !atomic_load(&PRI(n)->ai_failed))
        atomic_store(&PRI(n)->text_eof, true);
    mp_info(n->log, "subtitle file loaded: %d lines (%.100s%s)\n", lines, j->cfg->sub_file,
            strlen(j->cfg->sub_file) > 100 ? "..." : "");
    demux_free(d);
}

static void gloss_run(struct job *j);
static void imp_run(struct job *j);

static MP_THREAD_VOID job_thread(void *p)
{
    struct job *j = p;
    struct nanum *n = j->n;
    mp_thread_set_name(j->kind == JOB_SCAN ? "nanum-scan" :
                       j->kind == JOB_RETR ? "nanum-retr" :
                       j->kind == JOB_GLOSS ? "nanum-gloss" :
                       j->kind == JOB_IMPORT ? "nanum-import" : "nanum-subfile");
    if ((j->kind == JOB_SCAN || j->kind == JOB_RETR) && j->ls.save)
        j->job_row = lib_job_begin(n->lib, j->ls.source_id,
                                   j->kind == JOB_SCAN ? "scan" : "retranslate");
    if (j->kind == JOB_SCAN)
        scan_run(j);
    else if (j->kind == JOB_RETR)
        retr_run(j);
    else if (j->kind == JOB_GLOSS)
        gloss_run(j);
    else if (j->kind == JOB_IMPORT)
        imp_run(j);
    else
        ext_run(j);
    // Finished and failed jobs closed their row already; one still open here
    // was interrupted.
    if (j->job_row)
        lib_job_end(n->lib, j->job_row, job_cancelled(j) ? "cancelled" : "failed", "");
    atomic_store(&j->finished, true);
    MP_THREAD_RETURN();
}

static void imp_split_request(struct job *j, const char *req);

static struct job *job_start(struct nanum *n, int kind, const struct rt_cfg *cfg,
                             const char *url, double duration, int a_srcid,
                             const struct libsrc *ls, const char *arg)
{
    struct job *j = talloc_zero(NULL, struct job);
    j->n = n;
    j->kind = kind;
    j->cfg = rt_dup(j, cfg);
    j->url = talloc_strdup(j, url ? url : "");
    j->duration = duration;
    j->a_srcid = a_srcid;
    // arg: JOB_GLOSS the glossary to translate with, JOB_IMPORT "<path>|<lang>|<origin>".
    if (arg && kind == JOB_IMPORT)
        imp_split_request(j, arg);
    else if (arg)
        j->gl_target = talloc_strdup(j, arg);
    if (ls) {
        j->ls = *ls;
        j->ls.media_key = talloc_strdup(j, ls->media_key ? ls->media_key : "");
        j->ls.source_key = talloc_strdup(j, ls->source_key ? ls->source_key : "");
        j->ls.input = talloc_strdup(j, ls->input ? ls->input : "");
    }
    atomic_init(&j->cancel, false);
    atomic_init(&j->finished, false);
    j->mpcancel = mp_cancel_new(j);
    mp_mutex_init(&j->dlock);
    mp_cond_init(&j->dcond);
    if (mp_thread_create(&j->thread, job_thread, j)) {
        mp_mutex_destroy(&j->dlock);
        mp_cond_destroy(&j->dcond);
        talloc_free(j);
        return NULL;
    }
    return j;
}

// Cancels the job and waits for it. The engine's final callback cannot run
// after this returns. A cancelled job leaves everything already saved untouched.
static void job_stop(struct job **pj)
{
    struct job *j = *pj;
    if (!j)
        return;
    *pj = NULL;
    atomic_store(&j->cancel, true);
    mp_cancel_trigger(j->mpcancel);
    mp_thread_join(j->thread);
    mp_mutex_destroy(&j->dlock);
    mp_cond_destroy(&j->dcond);
    talloc_free(j);
}

// ---------------------------------------------------------------------------
// Control thread: follows option changes, resolves the library state and owns
// the jobs

// Local file behind a url, if it is one (never a network or disc url).
static bool local_path(void *ctx, const char *url, char **path)
{
    if (!strncmp(url, "file://", 7)) {
        const char *p = url + 7;
        if (p[0] == '/' && p[1] && p[2] == ':')
            p++;                    // file:///C:/x
        *path = talloc_strdup(ctx, p);
        return true;
    }
    if (!url[0] || strstr(url, "://"))
        return false;
    *path = talloc_strdup(ctx, url);
    return true;
}

struct pjob;

// A time alignment of an imported subtitle file (see align_fit()).
struct fit {
    bool ok;
    double offset, scale, conf;
    int matched;
};

// The imported subtitle file between the end of its job and the moment it is shown: it waits
// for enough speech recognition cues to be aligned against.
struct imp_ctl {
    bool waiting;               // a result is held here until it is aligned
    void *ctx;                  // owns cues
    struct lib_cue *cues;       // the file's cues (file times), tr = translation
    size_t ncues;
    int64_t since;              // when the result arrived
    bool direct;                // the file is in the target language
    char lang[16], origin[64], enc[24], hash[17];
    // The shown import
    bool shown;
    double offset, scale, conf;
    int matched;
    bool low, adjusted;
    size_t fit_refs;            // number of reference cues the last fit used
    struct fit last;            // that fit
    char key[96];
};

struct ctl {
    struct nanum *n;
    struct rt_cfg *cur;
    struct job *scan, *ext;
    bool ext_loaded;

    // AI profile of the work and its glossary
    bool prof_loaded;
    struct pjob *pjob;
    char last_req[1100];        // the last nanum-glossary-request handled
    struct job *gjob;           // retranslation after a glossary change
    char *gl_fail;              // glossary the retranslation failed for

    // Secondary input
    int sec_id;                 // demuxer id of its audio track while it runs
    char sec_lang[8];
    int64_t next_save;

    // Image subtitles
    int pgs_id;                 // demuxer id of the image subtitle track while it is read
    char pgs_lang[8];
    int pgs_seen_idx;           // stream the demuxer was last told about

    // Imported subtitle file
    struct job *ijob;
    char imp_req[1400];         // the last nanum-import-sub handled
    double imp_adjust_req;      // the last nanum-import-adjust handled
    bool imp_loaded;            // the saved import of this media was looked up
    struct imp_ctl imp;

    // Media identity (once per file)
    bool ident_done, ident_ok;
    void *ictx;
    char *media_key, *media_name;
    const char *media_kind;
    int64_t media_size;
    int64_t media_id;
    char ident_err[256];

    // Evaluation of the library for the current AI input
    bool resolved, decided;
    int64_t eval_since, act_after;
    void *sctx;                 // owns the strings of ls
    struct libsrc ls;
    struct lib_found found;
};

// ---------------------------------------------------------------------------
// YouTube

static bool yt_id_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

// The 11 character video id at s, followed by the end, "?", "&", "#", "/" or ":".
static bool yt_take_id(const char *s, char *id, size_t sz)
{
    size_t l = 0;
    while (yt_id_char(s[l]))
        l++;
    if (l != 11 || sz < 12 || (s[l] && !strchr("?&#/:", s[l])))
        return false;
    memcpy(id, s, 11);
    id[11] = '\0';
    return true;
}

// host is domain or a subdomain of it.
static bool yt_host(const char *host, const char *domain)
{
    size_t h = strlen(host), d = strlen(domain);
    return h >= d && !strcmp(host + h - d, domain) && (h == d || host[h - d - 1] == '.');
}

bool nanum_youtube_id(const char *url, char *id, size_t sz)
{
    if (!url)
        return false;
    if (!strncmp(url, "ytdl://", 7))
        url += 7;
    const char *p;
    if (!strncasecmp(url, "https://", 8))
        p = url + 8;
    else if (!strncasecmp(url, "http://", 7))
        p = url + 7;
    else
        return false;
    size_t hl = strcspn(p, "/?#");
    char host[64];
    if (hl >= sizeof(host))
        return false;
    for (size_t i = 0; i < hl; i++)
        host[i] = (char)tolower((unsigned char)p[i]);
    host[hl] = '\0';
    char *colon = strchr(host, ':');
    if (colon)
        *colon = '\0';
    const char *rest = p + hl;
    if (!strcmp(host, "youtu.be")) {
        return *rest == '/' && yt_take_id(rest + 1, id, sz);
    }
    if (!yt_host(host, "youtube.com") && !yt_host(host, "youtube-nocookie.com"))
        return false;
    if (!strncmp(rest, "/watch", 6)) {
        for (const char *q = strchr(rest, '?'); q; q = strchr(q + 1, '&')) {
            if (!strncmp(q + 1, "v=", 2))
                return yt_take_id(q + 3, id, sz);
        }
        return false;
    }
    static const char *const prefixes[] = {"/shorts/", "/live/", "/embed/", "/v/"};
    for (size_t i = 0; i < MP_ARRAY_SIZE(prefixes); i++) {
        if (!strncmp(rest, prefixes[i], strlen(prefixes[i])))
            return yt_take_id(rest + strlen(prefixes[i]), id, sz);
    }
    return false;
}

static char *media_title_opt(void *ctx, struct nanum *n);

static void ctl_ident(struct ctl *c)
{
    struct nanum *n = c->n;
    c->ident_done = true;
    c->ident_ok = false;
    c->ident_err[0] = '\0';
    talloc_free(c->ictx);
    c->ictx = talloc_new(NULL);
    if (!n->lib) {
        snprintf(c->ident_err, sizeof(c->ident_err), "%s", n->lib_open_err);
        return;
    }
    // A YouTube video is the same media whichever stream urls yt-dlp resolves today.
    char vid[16];
    if (nanum_youtube_id(n->opts->origin_url, vid, sizeof(vid))) {
        char *title = media_title_opt(c->ictx, n);
        c->media_kind = "youtube";
        c->media_name = title && title[0] ? title : talloc_strdup(c->ictx, vid);
        c->media_size = 0;
        c->media_key = talloc_asprintf(c->ictx, "youtube|%s", vid);
        c->media_id = lib_media_ensure(n->lib, c->media_key, c->media_kind, c->media_name, 0,
                                       atomic_load(&n->duration));
        if (!c->media_id) {
            snprintf(c->ident_err, sizeof(c->ident_err), "%s", lib_error(n->lib));
            return;
        }
        mp_verbose(n->log, "youtube identity: %s\n", c->media_key);
        c->ident_ok = true;
        return;
    }
    mp_mutex_lock(&n->slock);
    char *url = talloc_strdup(c->ictx, n->url ? n->url : "");
    char *disc = n->disc_identity ? talloc_strdup(c->ictx, n->disc_identity) : NULL;
    mp_mutex_unlock(&n->slock);

    char hex[65];
    if (disc) {
        // Disc-specific metadata of the stream (libbluray): disc id, volume,
        // selected playlist, clips, track languages.
        lib_sha256_hex(disc, strlen(disc), hex);
        mp_verbose(n->log, "disc identity: %s\n", disc);
        c->media_kind = "disc";
        c->media_name = talloc_strdup(c->ictx, url);
        c->media_size = 0;
    } else {
        char *path;
        char err[160];
        if (!local_path(c->ictx, url, &path)) {
            static const char *const disc_urls[] = {"bd://", "bluray://", "br://",
                                                    "dvd://", "dvdnav://", "cdda://"};
            for (size_t i = 0; i < MP_ARRAY_SIZE(disc_urls); i++) {
                if (!strncmp(url, disc_urls[i], strlen(disc_urls[i]))) {
                    // A disc must be identified by its own metadata; without it
                    // nothing is reused or stored, and that is reported.
                    snprintf(c->ident_err, sizeof(c->ident_err),
                             "the disc could not be identified, results are not saved");
                    return;
                }
            }
            // Network stream: no identity, the library is not consulted.
            mp_info(n->log, "no library identity for '%.100s%s'\n", url,
                    strlen(url) > 100 ? "..." : "");
            return;
        }
        if (!lib_fingerprint_file(path, hex, &c->media_size, err, sizeof(err))) {
            snprintf(c->ident_err, sizeof(c->ident_err),
                     "%s, results are not saved", err);
            return;
        }
        c->media_kind = "file";
        c->media_name = talloc_strdup(c->ictx, base_name(path));
    }
    c->media_key = talloc_asprintf(c->ictx, "%s|%s", c->media_kind, hex);
    c->media_id = lib_media_ensure(n->lib, c->media_key, c->media_kind, c->media_name,
                                   c->media_size, atomic_load(&n->duration));
    if (!c->media_id) {
        snprintf(c->ident_err, sizeof(c->ident_err), "%s", lib_error(n->lib));
        return;
    }
    c->ident_ok = true;
}

static bool find_stream(struct nanum *n, int type, int id, struct sinfo *out)
{
    // Streams can share an id: the first one is the input (see resolve_pin()).
    bool found = false;
    mp_mutex_lock(&n->slock);
    for (int i = 0; i < n->nstreams; i++) {
        const struct sinfo *s = &n->streams[i];
        if (s->type == type && s->id == id && (!found || s->index < out->index)) {
            *out = *s;
            found = true;
        }
    }
    mp_mutex_unlock(&n->slock);
    return found;
}

static void norm_lang(const char *mp_lang, char *out, size_t sz)
{
    char l[8];
    iso2_from_mp(mp_lang, l, sizeof(l));
    snprintf(out, sz, "%s", strcmp(l, "auto") ? l : "und");
}

// The identity of a YouTube caption track given by its timedtext url (the signed parts of
// the url change on every open), NULL if the file is not one.
static char *yt_caption_key(void *ctx, const char *file)
{
    const char *q = strstr(file, "youtube.com/api/timedtext?");
    if (!q)
        return NULL;
    q = strchr(q, '?') + 1;
    char *key = talloc_strdup(ctx, "subnet|youtube");
    bool lang = false;
    while (*q) {
        size_t len = strcspn(q, "&;");
        for (size_t i = 0; i < 4; i++) {
            static const char *const stable[] = {"lang=", "name=", "kind=", "tlang="};
            size_t kl = strlen(stable[i]);
            if (len >= kl && !strncmp(q, stable[i], kl)) {
                key = talloc_asprintf_append(key, "|%.*s", (int)len, q);
                lang |= i == 0;
            }
        }
        q += len;
        if (*q)
            q++;
    }
    return lang ? key : NULL;
}

// Builds the identity of the current AI input. false: not known yet.
static bool ctl_source(struct ctl *c)
{
    struct nanum *n = c->n;
    const struct rt_cfg *cur = c->cur;
    struct libsrc *ls = &c->ls;
    struct sinfo si;
    ls->save = true;
    ls->media_id = c->media_id;
    ls->media_key = c->media_key;
    snprintf(ls->target, sizeof(ls->target), "%s", n->opts->target_language);
    snprintf(ls->tr_engine, sizeof(ls->tr_engine), "%s",
             cur->engine == ENGINE_OMP ? "omp" : "ollama");
    snprintf(ls->tr_model, sizeof(ls->tr_model), "%s",
             cur->engine == ENGINE_OMP ? cur->omp_model : n->opts->tr_model);
    if (rt_input(cur) == INPUT_STT) {
        int pin = atomic_load(&PRI(n)->pin_id);
        if (pin < 0 || !find_stream(n, STREAM_AUDIO, pin, &si))
            return false;
        if (!strcmp(n->opts->language, "auto"))
            norm_lang(si.lang, ls->lang, sizeof(ls->lang));
        else
            snprintf(ls->lang, sizeof(ls->lang), "%s", n->opts->language);
        const char *model = !strcmp(ls->lang, "en") || !n->opts->stt_model_multi ||
                            !n->opts->stt_model_multi[0]
                            ? n->opts->stt_model : n->opts->stt_model_multi;
        snprintf(ls->engine, sizeof(ls->engine), "%s", model ? base_name(model) : "");
        ls->input = "stt";
        ls->track_id = pin;
        ls->source_key = talloc_asprintf(c->sctx, "stt|id=%d|lang=%s|model=%s", pin,
                                         ls->lang, ls->engine);
    } else if (cur->sub_file[0]) {
        char hex[65], err[160];
        int64_t size;
        char *net = yt_caption_key(c->sctx, cur->sub_file);
        if (net) {
            ls->source_key = net;
        } else {
            if (!lib_fingerprint_file(cur->sub_file, hex, &size, err, sizeof(err)))
                return false;
            ls->source_key = talloc_asprintf(c->sctx, "subfile|%s", hex);
        }
        snprintf(ls->lang, sizeof(ls->lang), "und");    // known once the file is read
        snprintf(ls->engine, sizeof(ls->engine), "subtitle");
        ls->input = "sub";
        ls->track_id = -1;
    } else {
        if (!find_stream(n, STREAM_SUB, cur->sub_id, &si))
            return false;
        norm_lang(si.lang, ls->lang, sizeof(ls->lang));
        snprintf(ls->engine, sizeof(ls->engine), "subtitle");
        ls->input = "sub";
        ls->track_id = cur->sub_id;
        ls->source_key = talloc_asprintf(c->sctx, "sub|id=%d|lang=%s", cur->sub_id,
                                         ls->lang);
    }
    return true;
}

// Switches generation on the live path off/on. true if it changed.
static bool set_gen_off(struct nanum *n, bool off)
{
    bool was = atomic_exchange(&n->gen_off, off);
    if (was && !off) {
        // Fresh start with the pinned input (and the library consulted again).
        atomic_store(&PRI(n)->bound, atomic_load(&n->a_mode) != MODE_STT);
        PRI(n)->persist = false;
        live_restart(PRI(n));
    }
    return was != off;
}

enum { D_DEFAULT, D_HOLD, D_USE, D_RETR, D_REGEN, D_DISABLE };

// Takes the decision for the evaluated library state and acts on it. false:
// not possible yet (try again).
static bool lib_act(struct ctl *c, int64_t now, bool *sel)
{
    struct nanum *n = c->n;
    const struct rt_cfg *cur = c->cur;
    const struct lib_found *f = &c->found;
    bool mat = c->ls.save && (f->has_source || f->has_tr);
    int d;
    switch (cur->lib_action) {
    case LA_ASK:
        d = !mat ? D_DEFAULT : cur->lib_auto_use && f->has_tr ? D_USE : D_HOLD;
        break;
    case LA_USE:
        d = !mat ? D_DEFAULT : f->has_tr ? D_USE : cur->full ? D_RETR : D_DEFAULT;
        break;
    case LA_RETRANSLATE:
        d = mat && f->has_source ? D_RETR : D_REGEN;
        break;
    case LA_REGENERATE:
        d = D_REGEN;
        break;
    default:
        d = D_DISABLE;
    }

    mp_mutex_lock(&n->slock);
    char *url = talloc_strdup(NULL, n->url ? n->url : "");
    mp_mutex_unlock(&n->slock);
    double dur = atomic_load(&n->duration);
    bool job_wanted = d == D_REGEN || d == D_RETR || (d == D_DEFAULT && cur->full);
    // The duration arrives from the demuxer a moment after the engine starts.
    if (job_wanted && d != D_RETR && (!url[0] || dur <= 0) &&
        now - c->eval_since < MP_TIME_S_TO_NS(8)) {
        talloc_free(url);
        return false;
    }

    // Nothing is generated live while a saved result is used or generation is
    // disabled or being redone in the background.
    bool off = d == D_USE || d == D_DISABLE || d == D_RETR || d == D_REGEN;
    if (set_gen_off(n, off))
        *sel = true;

    switch (d) {
    case D_HOLD:
    case D_DISABLE:
        set_full(n, FS_OFF, 0, "", "");
        break;
    case D_USE: {
        struct lib_cue *cues = NULL;
        size_t nc = 0;
        if (lib_load_cues(n->lib, NULL, f->tr_source_rev, f->tr_id, &cues, &nc, NULL)) {
            void *tmp = talloc_new(NULL);
            char *dir = result_dir(tmp, cur);
            char *stem = result_stem(tmp, &c->ls, f->tr_id);
            lib_publish(n, &c->ls, LS_USING, NULL);
            publish_done(n, dir, stem, cues, nc, &c->ls, f->tr_source_rev, NULL);
            talloc_free(tmp);
            talloc_free(cues);
            mp_info(n->log, "using the saved translation (%zu lines)\n", nc);
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg), "cannot load the saved translation: %s",
                     lib_error(n->lib));
            mp_err(n->log, "%s\n", msg);
            set_full(n, FS_ERROR, 0, "", msg);
            lib_publish(n, &c->ls, LS_ERROR, msg);
            if (set_gen_off(n, false))
                *sel = true;
        }
        break;
    }
    default:
        if (d == D_DEFAULT && !cur->full) {
            set_full(n, FS_OFF, 0, "", "");
            break;
        }
        if (!n->ai_enabled) {
            char why[sizeof(n->ai_reason)];
            mp_mutex_lock(&n->slock);
            snprintf(why, sizeof(why), "%s", n->ai_reason[0] ? n->ai_reason
                                             : "the AI engine is not available");
            mp_mutex_unlock(&n->slock);
            set_full(n, FS_ERROR, 0, "", why);
            lib_publish(n, &c->ls, LS_ERROR, why);
            break;
        }
        if (!url[0]) {
            set_full(n, FS_ERROR, 0, "", "the media has no url");
            break;
        }
        struct libsrc ls = c->ls;
        ls.source_rev = d == D_RETR ? f->source_rev : 0;
        ls.source_id = f->source_id;
        ls.origin = d == D_RETR ? f->origin : 0;
        if (d == D_RETR)
            snprintf(ls.lang, sizeof(ls.lang), "%s", f->src_lang);
        set_full(n, d == D_RETR ? FS_TRANSLATING : FS_SCANNING, 0, "", "");
        lib_publish(n, &c->ls, LS_WORKING, NULL);
        c->scan = job_start(n, d == D_RETR ? JOB_RETR : JOB_SCAN, cur, url, dur,
                            atomic_load(&PRI(n)->pin_id), &ls, NULL);
        if (!c->scan) {
            set_full(n, FS_ERROR, 0, "", "cannot start the scan thread");
            lib_publish(n, &c->ls, LS_ERROR, "cannot start the scan thread");
        }
    }
    talloc_free(url);
    return true;
}

// Resolves media identity, AI input and saved material; then acts (after the
// debounce for option changes). Called every tick until it has decided.
static void lib_step(struct ctl *c, int64_t now, bool *sel)
{
    struct nanum *n = c->n;
    if (!c->resolved) {
        mp_mutex_lock(&n->slock);
        bool have_url = n->url && n->url[0];
        mp_mutex_unlock(&n->slock);
        int64_t waited = now - c->eval_since;
        if (!have_url && waited < MP_TIME_S_TO_NS(8))
            return;
        if (!c->ident_done)
            ctl_ident(c);
        bool known = false;
        if (c->ident_ok) {
            known = ctl_source(c);
            if (!known && waited < MP_TIME_S_TO_NS(10))
                return;     // the pinned track/subtitle is not announced yet
            if (!known)
                snprintf(c->ident_err, sizeof(c->ident_err),
                         "the AI input track could not be identified, results are not saved");
        }
        c->resolved = true;
        if (known && c->ident_ok) {
            bool ok = lib_lookup(n->lib, c->media_id, c->ls.source_key, c->ls.target,
                                 &c->found);
            if (!ok) {
                c->ls.save = false;
                set_lib(n, LS_ERROR, "", "", false, false, "", lib_error(n->lib));
            } else {
                c->ls.source_id = c->found.source_id;
                lib_publish(n, &c->ls,
                            c->found.has_source || c->found.has_tr ? LS_AVAILABLE : LS_NONE,
                            NULL);
            }
        } else {
            // Unsupported or unreadable identity: decline, never invent one.
            c->ls = (struct libsrc){0};
            snprintf(c->ls.warn, sizeof(c->ls.warn), "%s", c->ident_err);
            set_lib(n, c->ident_err[0] ? LS_ERROR : LS_NONE, "", "", false, false, "",
                    c->ident_err);
        }
    }
    if (now < c->act_after)
        return;
    if (lib_act(c, now, sel))
        c->decided = true;
}

// ---------------------------------------------------------------------------
// Names of the virtual tracks

// Korean UI names of the languages the player knows by name; others show their code.
static const char *lang_name_ko(const char *iso1)
{
    static const char *const names[][2] = {
        {"en", "영어"}, {"ja", "일본어"}, {"ko", "한국어"}, {"zh", "중국어"},
        {"fr", "프랑스어"}, {"de", "독일어"}, {"es", "스페인어"}, {"it", "이탈리아어"},
        {"ru", "러시아어"}, {"pt", "포르투갈어"},
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(names); i++) {
        if (!strcmp(iso1, names[i][0]))
            return names[i][1];
    }
    return iso1;
}

// ISO 639-1 language the primary input reads, "" if it is not known (yet).
static void primary_lang(struct nanum *n, const struct rt_cfg *cur, char out[8])
{
    char l[8] = "auto";
    struct sinfo si;
    int mode = rt_mode(cur);
    if (mode == MODE_STT) {
        if (strcmp(n->opts->language, "auto")) {
            snprintf(l, sizeof(l), "%s", n->opts->language);
        } else {
            int pin = atomic_load(&PRI(n)->pin_id);
            if (pin >= 0 && find_stream(n, STREAM_AUDIO, pin, &si))
                iso2_from_mp(si.lang, l, sizeof(l));
        }
    } else if (mode == MODE_SUB_EMB) {
        if (find_stream(n, STREAM_SUB, cur->sub_id, &si))
            iso2_from_mp(si.lang, l, sizeof(l));
    } else {
        mp_mutex_lock(&n->slock);
        snprintf(l, sizeof(l), "%s", n->ext_lang[0] ? n->ext_lang : "auto");
        mp_mutex_unlock(&n->slock);
    }
    snprintf(out, 8, "%s", strcmp(l, "auto") ? l : "");
}

// "한국어 (영어 음성 기반)": target language and what the cues come from.
static void make_title(struct nanum *n, bool audio, const char *lang, char *out, size_t sz)
{
    const char *target = !strcmp(n->opts->target_language, "Korean")
                         ? "한국어" : n->opts->target_language;
    if (lang[0])
        snprintf(out, sz, "%s (%s %s 기반)", target, lang_name_ko(lang), audio ? "음성" : "자막");
    else
        snprintf(out, sz, "%s (AI)", target);
}

// "한국어 (영어 디스크 자막 기반)"
static void make_title_pgs(struct nanum *n, const char *lang, char *out, size_t sz)
{
    const char *target = !strcmp(n->opts->target_language, "Korean")
                         ? "한국어" : n->opts->target_language;
    snprintf(out, sz, "%s (%s 디스크 자막 기반)", target, lang_name_ko(lang));
}

// "한국어 (OpenSubtitles 영어 자막 기반)", or "한국어 (OpenSubtitles)" for a file that already
// is in the target language.
static void make_title_import(struct nanum *n, const char *origin, const char *lang,
                              bool direct, char *out, size_t sz)
{
    const char *target = !strcmp(n->opts->target_language, "Korean")
                         ? "한국어" : n->opts->target_language;
    if (direct)
        snprintf(out, sz, "%s (%s)", target, origin);
    else
        snprintf(out, sz, "%s (%s %s 자막 기반)", target, origin, lang_name_ko(lang));
}

// Sets the title of the virtual track of an input. The player keeps the pointer of
// the title of every stream it has listed, so a changed title is a new string and the
// old one stays valid; demux.c tells the player to look again (nanum_title_gen()).
static void set_title(struct nanum *n, int input, const char *text)
{
    struct input *in = &n->in[input];
    bool changed = false;
    mp_mutex_lock(&n->slock);
    if (!n->titles[input] || strcmp(n->titles[input], text)) {
        char *s = strdup(text);
        char **pool = s ? realloc(n->title_pool, (n->ntitle_pool + 1) * sizeof(*pool)) : NULL;
        if (pool) {
            n->title_pool = pool;
            pool[n->ntitle_pool++] = s;
            n->titles[input] = s;
            changed = true;
        } else {
            free(s);
        }
    }
    char *t = n->titles[input];
    mp_mutex_unlock(&n->slock);
    struct sh_stream *sh = __atomic_load_n(&in->sh, __ATOMIC_ACQUIRE);
    if (changed && sh) {
        __atomic_store_n(&sh->title, t, __ATOMIC_RELEASE);
        atomic_fetch_add(&n->title_gen, 1);
        wake_demux(n);
    }
}

static void ctl_titles(struct ctl *c)
{
    struct nanum *n = c->n;
    char lang[8], title[96];
    primary_lang(n, c->cur, lang);
    make_title(n, rt_mode(c->cur) == MODE_STT, lang, title, sizeof(title));
    set_title(n, IN_PRI, title);
}

// ---------------------------------------------------------------------------
// Library binding of the live results

static const char *in_label(int idx)
{
    static const char *const l[NANUM_INPUTS] = {"", " (secondary)", " (image subtitles)",
                                                " (import)"};
    return l[idx];
}

// Shows what the library holds for this input and key, and starts saving under it.
static void bind_input(struct nanum *n, struct input *in, int64_t media_id, const char *key)
{
    void *tmp = talloc_new(NULL);
    struct lib_cue *cues;
    struct lib_range *ranges;
    size_t nc, nr;
    events_clear(in);
    if (!lib_live_load(n->lib, tmp, media_id, key, &cues, &nc, &ranges, &nr)) {
        mp_err(n->log, "cannot load the saved results: %s\n", lib_error(n->lib));
    } else if (nc || nr) {
        input_preload(in, cues, nc, ranges, nr);
        mp_info(n->log, "saved results%s: %zu cues, %zu analysed ranges\n", in_label(in->idx),
                nc, nr);
    }
    free(in->key);
    in->key = strdup(key);
    in->media_id = media_id;
    in->persist = in->key != NULL;
    talloc_free(tmp);
}

// Saves the cues and ranges of an input if they changed.
static void persist_input(struct nanum *n, struct input *in)
{
    if (!in->persist || !n->lib)
        return;
    void *tmp = talloc_new(NULL);
    mp_mutex_lock(&n->lock);
    if (!in->dirty) {
        mp_mutex_unlock(&n->lock);
        talloc_free(tmp);
        return;
    }
    size_t nc = in->num_events, nr = in->ncov;
    struct lib_cue *cues = talloc_array(tmp, struct lib_cue, nc);
    struct lib_range *ranges = talloc_array(tmp, struct lib_range, nr);
    double scale = 1;
    if (in->idx == IN_IMP)
        scale = n->imp_scale > 0 ? n->imp_scale : 1;    // (a stale read is harmless)
    for (size_t i = 0; i < nc; i++) {
        bool raw = in->idx == IN_IMP;   // an import is stored in the subtitle file's times
        cues[i] = (struct lib_cue){
            .pts = raw ? in->events[i].raw_pts : in->events[i].pts,
            .dur = raw ? in->events[i].duration / scale : in->events[i].duration,
            .src = talloc_strdup(tmp, in->events[i].src),
            .tr = in->events[i].tr ? talloc_strdup(tmp, in->events[i].tr) : NULL,
        };
    }
    for (size_t i = 0; i < nr; i++)
        ranges[i] = (struct lib_range){in->cov[i].a, in->cov[i].b};
    in->dirty = false;
    mp_mutex_unlock(&n->lock);
    if (lib_live_save(n->lib, in->media_id, in->key, cues, nc, ranges, nr)) {
        mp_verbose(n->log, "saved%s: %zu cues, %zu analysed ranges\n", in_label(in->idx), nc,
                   nr);
    } else {
        mp_err(n->log, "cannot save the live results: %s\n", lib_error(n->lib));
        mp_mutex_lock(&n->lock);
        in->dirty = true;
        mp_mutex_unlock(&n->lock);
    }
    talloc_free(tmp);
}

// Binds the primary input once the library decision is made (the worker holds its audio
// until then, so what is saved is not analysed again).
static void ctl_bind(struct ctl *c, int64_t now)
{
    struct nanum *n = c->n;
    struct input *in = PRI(n);
    if (!atomic_load(&n->ai_enabled) || atomic_load(&in->bound) ||
        atomic_load(&in->reset_pending))
        return;
    if (!c->decided && now - c->eval_since < MP_TIME_S_TO_NS(15))
        return;
    if (c->decided && n->lib && c->ident_ok && c->ls.save && rt_mode(c->cur) == MODE_STT &&
        !atomic_load(&n->gen_off))
    {
        char *key = talloc_asprintf(NULL, "live|%s|tgt=%s", c->ls.source_key, c->ls.target);
        bind_input(n, in, c->media_id, key);
        talloc_free(key);
    } else {
        in->persist = false;
    }
    atomic_store(&in->bound, true);
    mp_mutex_lock(&in->alock);
    mp_cond_signal(&in->acond);
    mp_mutex_unlock(&in->alock);
}

// ---------------------------------------------------------------------------
// Secondary input

struct sec_plan {
    bool ok;
    bool pending;               // not decided yet, not "unavailable"
    int id, idx;                // audio stream (demuxer id, index)
    char lang[8];
    char reason[256];
};

static void sec_set(struct nanum *n, int state, const char *lang, const char *reason)
{
    mp_mutex_lock(&n->slock);
    n->sec_state = state;
    snprintf(n->sec_lang, sizeof(n->sec_lang), "%s", lang);
    snprintf(n->sec_reason, sizeof(n->sec_reason), "%s", reason);
    mp_mutex_unlock(&n->slock);
}

static const char *sec_models_missing(struct nanum *n, const char *lang)
{
    const struct nanum_opts *o = n->opts;
    if (!o->vad_model || !o->vad_model[0] || !mp_path_exists(o->vad_model))
        return "The voice detection model is not installed";
    bool en = !strcmp(lang, "en");
    const char *m = en ? o->stt_model : o->stt_model_multi;
    if (!m || !m[0] || !mp_path_exists(m)) {
        return en ? "The English speech model is not installed"
                  : "The multilingual speech model is not installed";
    }
    return NULL;
}

// Decides which audio track the secondary input reads: the original language of the
// work (profile) if the media has it as an audio track and it is not what the primary
// reads; without a profile answer, the other one of exactly two audio languages.
static void sec_plan(struct nanum *n, const char *prim_lang, struct sec_plan *p)
{
    *p = (struct sec_plan){.id = -1, .idx = -1};
    char langs[16][8];
    int nl = 0;
    char orig[16];
    bool profiling;
    struct sinfo audio[64];
    int na = 0;

    mp_mutex_lock(&n->slock);
    for (int i = 0; i < n->nstreams && na < 64; i++) {
        const struct sinfo *s = &n->streams[i];
        char l[8];
        if (s->type != STREAM_AUDIO)
            continue;
        iso2_from_mp(s->lang, l, sizeof(l));
        if (!strcmp(l, "auto"))
            continue;
        audio[na++] = *s;
        bool known = false;
        for (int k = 0; k < nl; k++)
            known |= !strcmp(langs[k], l);
        if (!known && nl < 16)
            snprintf(langs[nl++], sizeof(langs[0]), "%s", l);
    }
    snprintf(orig, sizeof(orig), "%s", n->orig_lang);
    profiling = n->prof_state == PS_RUNNING;
    mp_mutex_unlock(&n->slock);

    if (nl < 2) {
        snprintf(p->reason, sizeof(p->reason), "the media has only one audio language");
        return;
    }
    bool orig_in_set = false;
    for (int k = 0; k < nl; k++)
        orig_in_set |= orig[0] && !strcmp(langs[k], orig);

    const char *want = "";
    if (orig_in_set) {
        if (!strcmp(orig, prim_lang)) {
            snprintf(p->reason, sizeof(p->reason),
                     "the original language is the language of the playback audio");
            return;
        }
        want = orig;
    } else if (nl == 2 && prim_lang[0]) {
        want = strcmp(langs[0], prim_lang) ? langs[0] : langs[1];
    } else if (profiling && nl > 2) {
        p->pending = true;
        snprintf(p->reason, sizeof(p->reason), "waiting for the AI profile");
        return;
    } else {
        p->pending = !prim_lang[0] && nl == 2;
        snprintf(p->reason, sizeof(p->reason), prim_lang[0]
                 ? "the original language of the work is not known"
                 : "waiting for the language of the AI input");
        return;
    }
    // Several tracks of the language: the default one, else the first.
    const struct sinfo *pick = NULL;
    for (int i = 0; i < na; i++) {
        char l[8];
        iso2_from_mp(audio[i].lang, l, sizeof(l));
        if (strcmp(l, want))
            continue;
        if (!pick || (audio[i].def && !pick->def))
            pick = &audio[i];
    }
    if (!pick)
        return;
    char codec[16];
    p->id = pick->id;
    p->idx = resolve_stream_idx(n, p->id, codec);
    snprintf(p->lang, sizeof(p->lang), "%s", want);
    const char *missing = sec_models_missing(n, want);
    if (missing) {
        snprintf(p->reason, sizeof(p->reason), "%s", missing);
        return;
    }
    p->ok = p->idx >= 0;
}

static void sec_stop(struct ctl *c, bool *sel)
{
    struct nanum *n = c->n;
    struct input *in = SEC(n);
    if (!atomic_load(&in->active))
        return;
    persist_input(n, in);
    in->persist = false;
    input_stop(in);
    atomic_store(&in->pin_id, -1);
    atomic_store(&in->pin_idx, -1);
    c->sec_id = -1;
    *sel = true;
    mp_info(n->log, "secondary input stopped\n");
}

static bool cand_handover(struct nanum *n, int idx, int id);

static bool sec_start(struct ctl *c, const struct sec_plan *p, bool *sel)
{
    struct nanum *n = c->n;
    struct input *in = SEC(n);
    snprintf(in->alang, sizeof(in->alang), "%s", p->lang);
    in->persist = false;
    if (n->lib && c->ident_ok) {
        const char *model = !strcmp(p->lang, "en") ? n->opts->stt_model
                                                    : n->opts->stt_model_multi;
        char *key = talloc_asprintf(NULL, "live|stt|id=%d|lang=%s|model=%s|tgt=%s", p->id,
                                    p->lang, model ? base_name(model) : "",
                                    n->opts->target_language);
        bind_input(n, in, c->media_id, key);
        talloc_free(key);
    } else {
        events_clear(in);
    }
    atomic_store(&in->bound, true);
    c->sec_id = p->id;
    snprintf(c->sec_lang, sizeof(c->sec_lang), "%s", p->lang);
    char title[96];
    make_title(n, true, p->lang, title, sizeof(title));
    set_title(n, IN_SEC, title);
    atomic_store(&n->stream_wanted[IN_SEC], true);
    // The packets the demuxer delivered since the start are kept for this stream (they are
    // not replayed); a stream that was not kept starts from what the player buffered.
    atomic_store(&in->replay_audio, !cand_handover(n, p->idx, p->id));
    if (!input_start(in))
        return false;
    *sel = true;
    mp_info(n->log, "secondary input: audio stream %d (id %d), language %s\n", p->idx, p->id,
            p->lang);
    wake_demux(n);
    return true;
}

// Starts, stops and reports the secondary input. Returns true if the plan has no secondary
// input (the demuxer need not keep candidate audio then).
static bool ctl_secondary_step(struct ctl *c, bool *sel)
{
    struct nanum *n = c->n;
    struct input *in = SEC(n);
    struct sec_plan p;

    if (!c->cur->secondary) {
        sec_stop(c, sel);
        sec_set(n, SS_OFF, "", "");
        return false;
    }
    if (atomic_load(&n->gen_off)) {
        sec_stop(c, sel);
        sec_set(n, SS_UNAVAILABLE, "", "AI subtitles are generated from saved results or are off");
        return false;
    }
    if (!atomic_load(&n->ai_enabled)) {
        char why[sizeof(n->ai_reason)];
        mp_mutex_lock(&n->slock);
        snprintf(why, sizeof(why), "%s", n->ai_reason[0] ? n->ai_reason
                                         : "the AI engine is not available");
        mp_mutex_unlock(&n->slock);
        sec_stop(c, sel);
        sec_set(n, SS_UNAVAILABLE, "", why);
        return false;
    }
    if (!c->ident_done) {
        sec_set(n, SS_WAITING, "", "waiting for the media to be identified");
        return false;
    }
    char prim[8];
    primary_lang(n, c->cur, prim);
    sec_plan(n, prim, &p);
    if (!p.ok) {
        sec_stop(c, sel);
        sec_set(n, p.pending ? SS_WAITING : SS_UNAVAILABLE, "", p.reason);
        return !p.pending;
    }
    bool running = atomic_load(&in->active);
    if (running && (p.id != c->sec_id || strcmp(p.lang, c->sec_lang))) {
        sec_stop(c, sel);
        running = false;
    }
    if (!running && !sec_start(c, &p, sel))
        return false;
    if (atomic_load(&in->ai_failed))
        return false;                   // ai_fail() published why
    int st = atomic_load(&in->done) ? SS_DONE : sec_gate_open(n) ? SS_RUNNING : SS_WAITING;
    sec_set(n, st, p.lang, st == SS_WAITING ? "the playback input is analysed first" : "");
    return false;
}

static void ctl_secondary(struct ctl *c, bool *sel)
{
    struct nanum *n = c->n;
    atomic_store(&n->sec_enabled, c->cur->secondary);
    atomic_store(&n->sec_dead, ctl_secondary_step(c, sel));
    // The streams the demuxer delivers for the secondary input may have changed.
    if (cand_recompute(n))
        *sel = true;
}

// ---------------------------------------------------------------------------
// Image subtitle input

static void pgs_set(struct nanum *n, int state, const char *lang, const char *reason)
{
    mp_mutex_lock(&n->slock);
    n->pgs_state = state;
    snprintf(n->pgs_lang, sizeof(n->pgs_lang), "%s", lang);
    snprintf(n->pgs_reason, sizeof(n->pgs_reason), "%s", reason);
    mp_mutex_unlock(&n->slock);
}

static void pgs_stop(struct ctl *c, bool *sel)
{
    struct nanum *n = c->n;
    struct input *in = PGS(n);
    if (!atomic_load(&in->active)) {
        mp_mutex_lock(&in->alock);      // packets queued for a worker that will not start
        clear_queue_locked(in, true);
        mp_mutex_unlock(&in->alock);
        return;
    }
    persist_input(n, in);
    in->persist = false;
    input_stop(in);
    c->pgs_id = -1;
    *sel = true;
    mp_info(n->log, "image subtitle input stopped\n");
}

static bool pgs_start(struct ctl *c, const struct pgs_plan *p, bool *sel)
{
    struct nanum *n = c->n;
    struct input *in = PGS(n);
    snprintf(in->alang, sizeof(in->alang), "%s", p->lang);
    snprintf(in->lang, sizeof(in->lang), "%s", p->lang);
    snprintf(in->ocr_lang, sizeof(in->ocr_lang), "%s", p->ocr);
    in->persist = false;
    if (n->lib && c->ident_ok) {
        char *key = talloc_asprintf(NULL, "live|pgs|id=%d|lang=%s|tgt=%s", p->id, p->lang,
                                    n->opts->target_language);
        bind_input(n, in, c->media_id, key);
        talloc_free(key);
    } else {
        events_clear(in);
    }
    atomic_store(&in->bound, true);
    c->pgs_id = p->id;
    snprintf(c->pgs_lang, sizeof(c->pgs_lang), "%s", p->lang);
    char title[96];
    make_title_pgs(n, p->lang, title, sizeof(title));
    set_title(n, IN_PGS, title);
    atomic_store(&n->stream_wanted[IN_PGS], true);
    if (!input_start(in))
        return false;
    *sel = true;
    mp_info(n->log, "image subtitle input: stream %d (id %d), language %s, OCR %s\n", p->idx,
            p->id, p->lang, p->ocr);
    wake_demux(n);
    return true;
}

// Starts, stops and reports the image subtitle input.
static void ctl_pgs(struct ctl *c, bool *sel)
{
    struct nanum *n = c->n;
    struct input *in = PGS(n);
    atomic_store(&n->pgs_enabled, c->cur->pgs);
    struct pgs_plan p;
    pgs_replan(n, &p);
    int idx = atomic_load(&in->pin_idx);
    if (idx != c->pgs_seen_idx) {
        c->pgs_seen_idx = idx;
        *sel = true;            // the demuxer delivers another stream now
    }
    if (!c->cur->pgs) {
        pgs_stop(c, sel);
        pgs_set(n, SS_OFF, "", "");
        return;
    }
    if (!atomic_load(&n->ai_enabled)) {
        pgs_stop(c, sel);
        pgs_set(n, p.exists ? SS_UNAVAILABLE : SS_OFF, "", p.exists ? "the AI engine is not available" : "");
        return;
    }
    if (!p.exists) {
        pgs_stop(c, sel);
        pgs_set(n, SS_OFF, "", "");
        return;
    }
    if (!p.ok) {
        pgs_stop(c, sel);
        pgs_set(n, SS_UNAVAILABLE, "", p.reason);
        return;
    }
    if (!c->ident_done) {
        pgs_set(n, SS_WAITING, p.lang, "waiting for the media to be identified");
        return;
    }
    bool running = atomic_load(&in->active);
    if (running && (p.id != c->pgs_id || strcmp(p.lang, c->pgs_lang))) {
        pgs_stop(c, sel);
        running = false;
    }
    if (!running && !pgs_start(c, &p, sel))
        return;
    if (atomic_load(&in->ai_failed))
        return;                         // ai_fail() published why
    int st = atomic_load(&in->done) ? SS_DONE : sec_gate_open(n) ? SS_RUNNING : SS_WAITING;
    pgs_set(n, st, p.lang, st == SS_WAITING ? "the playback input is analysed first" : p.note);
}

// ---------------------------------------------------------------------------
// AI profile of the work and its glossary

static int glossary_lines(const char *text)
{
    int count = 0;
    for (const char *p = text; *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *line = talloc_strndup(NULL, p, len);
        if (strstr(line, "=>"))
            count++;
        talloc_free(line);
        p += len + (e ? 1 : 0);
    }
    return count;
}

// Makes the glossary the one every engine translates with.
static void glossary_apply(struct nanum *n, const char *text)
{
    mp_mutex_lock(&n->slock);
    if (!n->glossary || strcmp(n->glossary, text)) {
        char *g = strdup(text);
        if (g) {
            free(n->glossary);
            n->glossary = g;
            n->glossary_count = glossary_lines(text);
            n->glossary_changed = mp_time_ns();
            atomic_fetch_add(&n->tr_gen, 1);
        }
    }
    mp_mutex_unlock(&n->slock);
}

// The glossary the cues are known to be translated with: whatever is loaded now.
static void gloss_set_base(struct nanum *n)
{
    mp_mutex_lock(&n->slock);
    char *cur = n->glossary ? talloc_strdup(NULL, n->glossary) : talloc_strdup(NULL, "");
    free(n->gl_base);
    n->gl_base = strdup(cur);
    n->gl_base_set = true;
    mp_mutex_unlock(&n->slock);
    // A result shown from the library (published before the glossary was known) agrees
    // with it as well.
    mp_mutex_lock(&n->rlock);
    if (n->dcues && !n->dgl)
        n->dgl = talloc_strdup(n->dctx, cur);
    mp_mutex_unlock(&n->rlock);
    talloc_free(cur);
}

static const char *find_arrow(const char *s, size_t len);

static char *prop_key(void *ctx, const char *prefix, const char *target)
{
    return talloc_asprintf(ctx, "%s.%s", prefix, target);
}

// Appends the lines of 'added' whose source key (the text left of "=>", compared
// case-insensitively) is not in 'base' yet: what is already known wins.
static char *glossary_merge(void *ctx, const char *base, const char *added)
{
    char *out = talloc_strdup(ctx, base);
    for (const char *p = added; *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *line = talloc_strndup(ctx, p, len);
        p += len + (e ? 1 : 0);
        char *arrow = strstr(line, "=>");
        if (!arrow) {
            talloc_free(line);
            continue;
        }
        size_t klen = (size_t)(arrow - line);
        bool known = false;
        for (const char *q = out; *q && !known; ) {
            const char *qe = strchr(q, '\n');
            size_t ql = qe ? (size_t)(qe - q) : strlen(q);
            const char *qa = find_arrow(q, ql);
            if (qa && (size_t)(qa - q) == klen && !strncasecmp(q, line, klen))
                known = true;
            q += ql + (qe ? 1 : 0);
        }
        if (!known)
            out = talloc_asprintf_append(out, "%s%s", out[0] ? "\n" : "", line);
        talloc_free(line);
    }
    return out;
}

// Result of nanum_ai_profile(), run on its own thread because it blocks on an LLM query
// for many seconds. The thread is never joined: quitting does not wait for it (the job
// owns its own engine handle and touches nothing else); whoever finishes last frees it.
enum { PJ_RUNNING, PJ_DONE, PJ_ORPHAN };

struct pjob {
    atomic_int st;
    struct nanum_engine eng;
    struct nanum_ai_api api;
    struct nanum_ai_config cfg;
    char *title, *langs;
    bool replace;               // regenerate: the result replaces the glossary
    bool ok;
    char out[32768];
    char err[256];
};

static void pjob_free(struct pjob *pj)
{
    unload_api(&pj->eng);
    talloc_free(pj);
}

static MP_THREAD_VOID profile_thread(void *p)
{
    struct pjob *pj = p;
    mp_thread_set_name("nanum-profile");
    pj->ok = pj->api.profile(&pj->cfg, pj->title, pj->langs, pj->out, sizeof(pj->out)) != 0;
    if (!pj->ok)
        snprintf(pj->err, sizeof(pj->err), "%s", pj->api.last_error());
    if (atomic_exchange(&pj->st, PJ_DONE) == PJ_ORPHAN)
        pjob_free(pj);
    MP_THREAD_RETURN();
}

static void pjob_abandon(struct pjob *pj)
{
    if (atomic_exchange(&pj->st, PJ_ORPHAN) == PJ_DONE)
        pjob_free(pj);
}

static void set_profile(struct nanum *n, int state, const char *work, const char *orig)
{
    mp_mutex_lock(&n->slock);
    n->prof_state = state;
    if (work)
        snprintf(n->prof_work, sizeof(n->prof_work), "%s", work);
    if (orig)
        snprintf(n->orig_lang, sizeof(n->orig_lang), "%s", orig);
    mp_mutex_unlock(&n->slock);
}

// The name the work is looked up by: the disc name, else the container's TITLE tag,
// else the volume id of a disc, else the file name without extension.
// The title the player was given for the media (--force-media-title, or the one ytdl_hook
// took from yt-dlp), NULL if there is none.
static char *media_title_opt(void *ctx, struct nanum *n)
{
    struct MPOpts *mo = mp_get_config_group(NULL, n->global, &mp_opt_root);
    char *t = mo && mo->media_title && mo->media_title[0] ? talloc_strdup(ctx, mo->media_title)
                                                          : NULL;
    talloc_free(mo);
    return t;
}

static char *work_title(void *ctx, struct nanum *n)
{
    char *title = media_title_opt(ctx, n);
    mp_mutex_lock(&n->slock);
    if (title) {
        // given by the player
    } else if (n->disc_name) {
        title = talloc_strdup(ctx, n->disc_name);
    } else if (n->meta_title) {
        title = talloc_strdup(ctx, n->meta_title);
    } else if (n->disc_identity && strstr(n->disc_identity, "|vol=")) {
        const char *v = strstr(n->disc_identity, "|vol=") + 5;
        title = talloc_strndup(ctx, v, strcspn(v, "|"));
    } else if (n->url) {
        const char *b = base_name(n->url);
        const char *dot = strrchr(b, '.');
        title = talloc_strndup(ctx, b, dot && dot != b ? (size_t)(dot - b) : strlen(b));
    }
    mp_mutex_unlock(&n->slock);
    if (!title)
        title = talloc_strdup(ctx, "");
    for (char *s = title; *s; s++) {
        if (*s == '_')
            *s = ' ';
    }
    return title;
}

// Comma separated ISO 639-1 codes of the audio tracks, e.g. "en,ja".
static char *audio_languages(void *ctx, struct nanum *n)
{
    char *out = talloc_strdup(ctx, "");
    mp_mutex_lock(&n->slock);
    for (int i = 0; i < n->nstreams; i++) {
        const struct sinfo *s = &n->streams[i];
        char l[8];
        if (s->type != STREAM_AUDIO)
            continue;
        iso2_from_mp(s->lang, l, sizeof(l));
        if (!strcmp(l, "auto"))
            continue;
        bool known = false;
        for (const char *q = out; *q && !known; ) {
            size_t len = strcspn(q, ",");
            known = len == strlen(l) && !strncmp(q, l, len);
            q += len + (q[len] ? 1 : 0);
        }
        if (!known)
            out = talloc_asprintf_append(out, "%s%s", out[0] ? "," : "", l);
    }
    mp_mutex_unlock(&n->slock);
    return out;
}

static void prof_start(struct ctl *c, bool replace)
{
    struct nanum *n = c->n;
    if (c->pjob) {
        pjob_abandon(c->pjob);
        c->pjob = NULL;
    }
    if (!n->tr_enabled) {
        set_profile(n, PS_OFF, NULL, NULL);
        return;
    }
    char why[1024];
    struct pjob *pj = talloc_zero(NULL, struct pjob);
    if (!load_api(n, NANUM_ACCEL_CPU, &pj->eng, &pj->api, why, sizeof(why))) {
        mp_err(n->log, "profile: %s\n", why);
        set_profile(n, PS_ERROR, NULL, NULL);
        talloc_free(pj);
        return;
    }
    pj->cfg = (struct nanum_ai_config){
        .abi_version = NANUM_AI_ABI_VERSION,
        .translate_endpoint = n->opts->tr_endpoint,
        .translate_model = n->opts->tr_model,
        .target_language = n->opts->target_language,
        .translate_engine = c->cur->engine == ENGINE_OMP ? "omp" : "ollama",
        .omp_model = talloc_strdup(pj, c->cur->omp_model),
        .omp_command = talloc_strdup(pj, c->cur->omp_command),
    };
    pj->title = work_title(pj, n);
    pj->langs = audio_languages(pj, n);
    pj->replace = replace;
    atomic_init(&pj->st, PJ_RUNNING);
    mp_thread t;
    if (mp_thread_create(&t, profile_thread, pj)) {
        mp_err(n->log, "profile: cannot start the thread\n");
        set_profile(n, PS_ERROR, NULL, NULL);
        pjob_free(pj);
        return;
    }
    mp_thread_detach(t);
    c->pjob = pj;
    set_profile(n, PS_RUNNING, NULL, NULL);
    mp_info(n->log, "profile: asking about '%s' (audio: %s)\n", pj->title, pj->langs);
}

// ISO 639-1 from whatever the model answered ("ja", "jpn", "Japanese").
static void profile_language(const char *in, char *out, size_t sz)
{
    static const char *const names[][2] = {
        {"english", "en"}, {"japanese", "ja"}, {"korean", "ko"}, {"chinese", "zh"},
        {"french", "fr"}, {"german", "de"}, {"spanish", "es"}, {"italian", "it"},
        {"russian", "ru"}, {"portuguese", "pt"},
    };
    char l[32];
    snprintf(l, sizeof(l), "%s", in ? in : "");
    for (char *s = l; *s; s++)
        *s = (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
    out[0] = '\0';
    if (strlen(l) == 2) {
        snprintf(out, sz, "%s", l);
        return;
    }
    char two[8];
    iso2_from_mp(l, two, sizeof(two));
    if (strcmp(two, "auto")) {
        snprintf(out, sz, "%s", two);
        return;
    }
    for (size_t i = 0; i < MP_ARRAY_SIZE(names); i++) {
        if (!strcmp(l, names[i][0]))
            snprintf(out, sz, "%s", names[i][1]);
    }
}

static const char *node_str(struct mpv_node *map, const char *key)
{
    struct mpv_node *v = node_map_get(map, key);
    return v && v->format == MPV_FORMAT_STRING ? v->u.string : "";
}

// The profile query finished: publish, apply and save what it found.
static void prof_finish(struct ctl *c, struct pjob *pj)
{
    struct nanum *n = c->n;
    if (!pj->ok) {
        mp_err(n->log, "profile failed: %s\n", pj->err);
        set_profile(n, PS_ERROR, NULL, NULL);
        return;
    }
    void *tmp = talloc_new(NULL);
    char *json = talloc_strdup(tmp, pj->out);
    char *s = json;
    struct mpv_node root;
    if (json_parse(tmp, &root, &s, MAX_JSON_DEPTH) < 0 || root.format != MPV_FORMAT_NODE_MAP) {
        mp_err(n->log, "profile failed: the answer is not valid JSON\n");
        set_profile(n, PS_ERROR, NULL, NULL);
        talloc_free(tmp);
        return;
    }
    char orig[16];
    const char *work = node_str(&root, "work");
    profile_language(node_str(&root, "original_language"), orig, sizeof(orig));
    const char *found = node_str(&root, "glossary");
    char *glossary;
    mp_mutex_lock(&n->slock);
    char *current = talloc_strdup(tmp, n->glossary ? n->glossary : "");
    mp_mutex_unlock(&n->slock);
    glossary = pj->replace ? talloc_strdup(tmp, found) : glossary_merge(tmp, current, found);
    set_profile(n, PS_READY, work, orig);
    glossary_apply(n, glossary);
    if (n->lib && c->ident_ok) {
        const char *target = n->opts->target_language;
        char *k = prop_key(tmp, "profile", target);
        lib_prop_set(n->lib, c->media_id, "profile.original_language", orig);
        lib_prop_set(n->lib, c->media_id, "profile.work", work);
        lib_prop_set(n->lib, c->media_id, prop_key(tmp, "glossary", target), glossary);
        lib_prop_set(n->lib, c->media_id, k, "1");
    }
    mp_info(n->log, "profile: work '%s', original language '%s', %d glossary entries\n", work,
            orig, glossary_lines(glossary));
    talloc_free(tmp);
}

// Reads the saved profile of the media and, if there is none, asks for one.
static void prof_load(struct ctl *c)
{
    struct nanum *n = c->n;
    c->prof_loaded = true;
    if (!c->ident_ok || !n->lib) {
        set_profile(n, PS_OFF, "", "");
        gloss_set_base(n);
        return;
    }
    void *tmp = talloc_new(NULL);
    const char *target = n->opts->target_language;
    char *done = lib_prop_get(n->lib, tmp, c->media_id, prop_key(tmp, "profile", target));
    char *gl = lib_prop_get(n->lib, tmp, c->media_id, prop_key(tmp, "glossary", target));
    char *orig = lib_prop_get(n->lib, tmp, c->media_id, "profile.original_language");
    char *work = lib_prop_get(n->lib, tmp, c->media_id, "profile.work");
    if (gl)
        glossary_apply(n, gl);
    gloss_set_base(n);
    if (done) {
        set_profile(n, PS_READY, work ? work : "", orig ? orig : "");
        mp_info(n->log, "saved profile: work '%s', original language '%s', %d glossary entries\n",
                work ? work : "", orig ? orig : "", glossary_lines(gl ? gl : ""));
    } else {
        set_profile(n, n->tr_enabled ? PS_RUNNING : PS_OFF, "", "");
        prof_start(c, false);
    }
    talloc_free(tmp);
}

// nanum-glossary-request=file:<path>: the lines "source => target" of a UTF-8 file.
static char *glossary_read_file(void *ctx, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *out = talloc_strdup(ctx, "");
    char line[2048];
    bool first = true;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        if (first && !memcmp(p, "\xEF\xBB\xBF", 3))
            p += 3;
        first = false;
        size_t len = strlen(p);
        while (len && (p[len - 1] == '\n' || p[len - 1] == '\r'))
            p[--len] = '\0';
        if (strstr(p, "=>"))
            out = talloc_asprintf_append(out, "%s%s", out[0] ? "\n" : "", p);
    }
    fclose(f);
    return out;
}

static void ctl_glossary_request(struct ctl *c)
{
    struct nanum *n = c->n;
    const char *req = c->cur->glossary_req;
    if (!req[0]) {
        c->last_req[0] = '\0';
        return;
    }
    if (!strcmp(req, c->last_req))
        return;
    snprintf(c->last_req, sizeof(c->last_req), "%s", req);
    if (!strcmp(req, "regenerate")) {
        mp_info(n->log, "glossary: regenerating\n");
        prof_start(c, true);
    } else if (!strncmp(req, "file:", 5)) {
        void *tmp = talloc_new(NULL);
        char *text = glossary_read_file(tmp, req + 5);
        if (!text) {
            mp_err(n->log, "glossary: cannot read '%s'\n", req + 5);
        } else {
            glossary_apply(n, text);
            if (n->lib && c->ident_ok) {
                lib_prop_set(n->lib, c->media_id,
                             prop_key(tmp, "glossary", n->opts->target_language), text);
            }
            mp_info(n->log, "glossary: %d entries from '%s'\n", glossary_lines(text), req + 5);
        }
        talloc_free(tmp);
    } else {
        mp_err(n->log, "glossary: unknown request '%s'\n", req);
    }
}

// Profile: load the saved one once the media is identified, collect a running query.
static void ctl_profile(struct ctl *c)
{
    struct nanum *n = c->n;
    if (!c->prof_loaded && c->ident_done)
        prof_load(c);
    if (c->pjob && atomic_load(&c->pjob->st) == PJ_DONE) {
        struct pjob *pj = c->pjob;
        c->pjob = NULL;
        prof_finish(c, pj);
        pjob_free(pj);
    }
    ctl_glossary_request(c);
    (void)n;
}

// ---------------------------------------------------------------------------
// Glossary changes: translating the existing cues again
//
// When the glossary changes (profile, regeneration, an edited file), every cue whose
// source text contains a key that was added, removed or changed is translated again
// with the new glossary, so the whole media agrees with it and not only the lines that
// come after. The cues of both live tracks (including the ones preloaded from the
// library) and the finished full translation each remember the glossary they were
// translated with (nanum.gl_base / nanum.dgl); a difference starts a JOB_GLOSS job. It
// works through bounded batches with the preceding lines as context, on its own
// translate-only engine, and applies each batch as it is done.

struct gl_entry {
    char *key, *val;
};

static void trim_span(const char **a, const char **b)
{
    while (*a < *b && (**a == ' ' || **a == '\t' || **a == '\r'))
        (*a)++;
    while (*b > *a && ((*b)[-1] == ' ' || (*b)[-1] == '\t' || (*b)[-1] == '\r'))
        (*b)--;
}

// The "=>" in the first len bytes of s, or NULL.
static const char *find_arrow(const char *s, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (s[i] == '=' && s[i + 1] == '>')
            return s + i;
    }
    return NULL;
}

static struct gl_entry *glossary_parse(void *ctx, const char *text, size_t *count)
{
    struct gl_entry *e = NULL;
    size_t cnt = 0;
    for (const char *p = text; *p; ) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *arrow = find_arrow(p, len);
        if (arrow) {
            const char *k0 = p, *k1 = arrow, *v0 = arrow + 2, *v1 = p + len;
            trim_span(&k0, &k1);
            trim_span(&v0, &v1);
            if (k1 > k0) {
                MP_TARRAY_APPEND(ctx, e, cnt, ((struct gl_entry){
                    .key = talloc_strndup(ctx, k0, k1 - k0),
                    .val = talloc_strndup(ctx, v0, v1 - v0),
                }));
            }
        }
        p += len + (nl ? 1 : 0);
    }
    *count = cnt;
    return e;
}

static int gl_find(const struct gl_entry *e, size_t cnt, const char *key)
{
    for (size_t i = 0; i < cnt; i++) {
        if (!strcasecmp(e[i].key, key))
            return (int)i;
    }
    return -1;
}

// The keys added, removed or given another target between two glossaries.
static char **glossary_diff(void *ctx, const char *old, const char *new, size_t *nkeys)
{
    size_t no, nn, nk = 0;
    struct gl_entry *o = glossary_parse(ctx, old, &no);
    struct gl_entry *w = glossary_parse(ctx, new, &nn);
    char **keys = NULL;
    for (size_t i = 0; i < nn; i++) {
        int j = gl_find(o, no, w[i].key);
        if (j < 0 || strcmp(o[j].val, w[i].val))
            MP_TARRAY_APPEND(ctx, keys, nk, w[i].key);
    }
    for (size_t j = 0; j < no; j++) {
        if (gl_find(w, nn, o[j].key) < 0)
            MP_TARRAY_APPEND(ctx, keys, nk, o[j].key);
    }
    *nkeys = nk;
    return keys;
}

static bool ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl)
        return false;
    for (; *hay; hay++) {
        if (!strncasecmp(hay, needle, nl))
            return true;
    }
    return false;
}

static bool has_key(const char *text, char **keys, size_t nk)
{
    for (size_t i = 0; i < nk; i++) {
        if (ci_contains(text, keys[i]))
            return true;
    }
    return false;
}

// One list of cues the engine translates again: a snapshot of a live input's store or of
// the finished full result, with the cues that contain an affected key marked.
struct gl_set {
    int kind;                   // an input (IN_*) or GSET_FULL
    char lang[16];
    struct lib_cue *cues;       // all cues, sorted by pts
    int *ids;                   // live: event id; full: index into the result
    bool *hit;                  // the cue is translated again
    size_t n, nhits;
    char **out;                 // new translations (malloc), NULL: not delivered
    const void *base;           // full: the result the snapshot comes from
};
#define GSET_FULL NANUM_INPUTS

static void gl_set_state(struct nanum *n, int state, double progress, int changed,
                         const char *err)
{
    mp_mutex_lock(&n->slock);
    n->gl_state = state;
    n->gl_progress = progress;
    n->gl_changed = changed;
    snprintf(n->gl_error, sizeof(n->gl_error), "%s", err ? err : "");
    mp_mutex_unlock(&n->slock);
}

static void gl_set_progress(struct nanum *n, double progress, int changed)
{
    mp_mutex_lock(&n->slock);
    n->gl_progress = floor(MPCLAMP(progress, 0.0, 0.999) * 1000.0) / 1000.0;
    n->gl_changed = changed;
    mp_mutex_unlock(&n->slock);
}

static void gl_set_alloc(struct job *j, struct gl_set *s, size_t cap)
{
    s->cues = talloc_array(j, struct lib_cue, cap ? cap : 1);
    s->ids = talloc_array(j, int, cap ? cap : 1);
    s->hit = talloc_zero_array(j, bool, cap ? cap : 1);
}

// The cues of a live input. Those containing an affected key that entered the store in
// (since, deadline] are marked: cues that came later were translated with the new
// glossary already (the engines had it by then).
static bool lang_is_target(struct nanum *n, const char *lang);

static void gl_snap_live(struct job *j, struct gl_set *s, int input, char **keys, size_t nk,
                         int64_t since, int64_t deadline)
{
    struct nanum *n = j->n;
    struct input *in = &n->in[input];
    s->kind = input;
    mp_mutex_lock(&n->lock);
    int cap = in->num_events;
    mp_mutex_unlock(&n->lock);
    gl_set_alloc(j, s, cap);
    mp_mutex_lock(&n->lock);
    for (int i = 0; i < in->num_events && (int)s->n < cap; i++) {
        const struct nanum_event *e = &in->events[i];
        s->cues[s->n] = (struct lib_cue){
            .pts = e->pts, .dur = e->duration,
            .src = talloc_strdup(j, e->src),
            .tr = e->tr ? talloc_strdup(j, e->tr) : NULL,
        };
        s->ids[s->n] = e->id;
        if (e->born > since && e->born <= deadline && has_key(e->src, keys, nk)) {
            s->hit[s->n] = true;
            s->nhits++;
        }
        s->n++;
    }
    mp_mutex_unlock(&n->lock);
    s->out = talloc_zero_array(j, char *, s->n ? s->n : 1);
    mp_mutex_lock(&n->slock);
    snprintf(s->lang, sizeof(s->lang), "%s", input == IN_SEC ? n->sec_lang : in->lang);
    mp_mutex_unlock(&n->slock);
    if (input == IN_PRI)
        primary_lang(n, j->cfg, s->lang);
    if (!s->lang[0])
        snprintf(s->lang, sizeof(s->lang), "en");
    // A subtitle file already in the target language has nothing to translate again.
    if (input == IN_IMP && lang_is_target(n, s->lang))
        s->nhits = 0;
}

static void gl_snap_full(struct job *j, struct gl_set *s, char **keys, size_t nk)
{
    struct nanum *n = j->n;
    s->kind = GSET_FULL;
    mp_mutex_lock(&n->rlock);
    s->base = n->dcues;
    size_t cap = n->dcues ? n->ndcues : 0;
    gl_set_alloc(j, s, cap);
    for (size_t i = 0; i < cap; i++) {
        const struct lib_cue *c = &n->dcues[i];
        s->cues[s->n] = (struct lib_cue){
            .pts = c->pts, .dur = c->dur,
            .src = talloc_strdup(j, c->src),
            .tr = c->tr ? talloc_strdup(j, c->tr) : NULL,
        };
        s->ids[s->n] = (int)i;
        if (has_key(c->src, keys, nk)) {
            s->hit[s->n] = true;
            s->nhits++;
        }
        s->n++;
    }
    snprintf(s->lang, sizeof(s->lang), "%s",
             strcmp(n->dls.lang, "und") && n->dls.lang[0] ? n->dls.lang : "en");
    mp_mutex_unlock(&n->rlock);
    s->out = talloc_zero_array(j, char *, s->n ? s->n : 1);
}

struct gl_cb {
    struct job *j;
    struct gl_set *s;
};

// Engine event: the translation of a pushed line (matched by its time).
static void gloss_event_cb(void *user, double pts, double dur, const char *src,
                           const char *tr)
{
    struct gl_cb *cb = user;
    struct gl_set *s = cb->s;
    if (!tr || !tr[0])
        return;
    mp_mutex_lock(&cb->j->dlock);
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->cues[mid].pts < pts - 0.002)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < s->n && s->cues[i].pts <= pts + 0.002; i++) {
        if (!s->out[i]) {
            s->out[i] = strdup(tr);
            break;
        }
    }
    mp_mutex_unlock(&cb->j->dlock);
}

#define GLOSS_BATCH 40
#define GLOSS_CONTEXT 3

// Translates the marked cues of a set with the target glossary, GLOSS_BATCH at a time,
// each led by the GLOSS_CONTEXT lines before it. Live sets are applied to the store
// after every batch. Returns an error message or NULL.
static const char *gloss_translate(struct job *j, struct gl_set *s, size_t *done,
                                   size_t total, int *changed)
{
    struct nanum *n = j->n;
    void *tmp = talloc_new(NULL);
    struct tr_snap ts;
    tr_snapshot(n, tmp, &ts);
    struct gl_cb cb = {j, s};
    struct nanum_ai_config cfg = {
        .abi_version = NANUM_AI_ABI_VERSION,
        .language = s->lang,
        .translate_endpoint = n->opts->tr_endpoint,
        .translate_model = n->opts->tr_model,
        .target_language = n->opts->target_language,
        .cache_path = n->opts->cache,
        .translate_only = 1,
        .translate_style = ts.style,
        .translate_style_text = ts.style_text,
        .glossary = j->gl_target,
    };
    struct nanum_ai *ai = j->api.create(&cfg, gloss_event_cb, &cb);
    if (!ai) {
        snprintf(j->gl_err, sizeof(j->gl_err), "cannot start the translation engine: %s",
                 j->api.last_error());
        talloc_free(tmp);
        return j->gl_err;
    }
    size_t delivered = 0, attempted = 0;
    size_t *idx = talloc_array(tmp, size_t, s->nhits ? s->nhits : 1);
    size_t nidx = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (s->hit[i])
            idx[nidx++] = i;
    }
    char *mark = talloc_zero_array(tmp, char, s->n ? s->n : 1);
    for (size_t pos = 0; pos < nidx && !job_cancelled(j); pos += GLOSS_BATCH) {
        size_t end = MPMIN(pos + GLOSS_BATCH, nidx);
        size_t lo = idx[pos] > GLOSS_CONTEXT ? idx[pos] - GLOSS_CONTEXT : 0;
        memset(mark + lo, 0, idx[end - 1] - lo + 1);
        for (size_t k = pos; k < end; k++) {
            size_t c0 = idx[k] > GLOSS_CONTEXT ? idx[k] - GLOSS_CONTEXT : 0;
            for (size_t i = c0; i <= idx[k]; i++)
                mark[i] = 1;
        }
        for (size_t i = lo; i <= idx[end - 1]; i++) {
            if (mark[i]) {
                j->api.push_text(ai, s->cues[i].pts, s->cues[i].dur > 0 ? s->cues[i].dur : 2.0,
                                 s->cues[i].src);
            }
        }
        j->api.flush(ai);
        while (!job_cancelled(j) && !j->api.wait_idle(ai, 200))
            ;
        if (job_cancelled(j))
            break;

        int cnt = 0;
        double *pts = talloc_array(tmp, double, end - pos);
        int *ids = talloc_array(tmp, int, end - pos);
        char **tr = talloc_array(tmp, char *, end - pos);
        mp_mutex_lock(&j->dlock);
        for (size_t k = pos; k < end; k++) {
            size_t i = idx[k];
            attempted++;
            if (!s->out[i])
                continue;
            delivered++;
            pts[cnt] = s->cues[i].pts;
            ids[cnt] = s->ids[i];
            tr[cnt] = talloc_strdup(tmp, s->out[i]);
            cnt++;
        }
        mp_mutex_unlock(&j->dlock);
        if (s->kind != GSET_FULL && cnt)
            *changed += input_apply_translations(&n->in[s->kind], cnt, ids, pts, tr);
        *done += end - pos;
        gl_set_progress(n, (double)*done / total, *changed);
        talloc_free(pts);
        talloc_free(ids);
        talloc_free(tr);
    }
    const char *err = NULL;
    if (!job_cancelled(j) && attempted && !delivered)
        err = "the translation model did not answer";
    j->api.cancel_full(ai);
    j->api.destroy(ai);
    talloc_free(tmp);
    return err;
}

// The full result gets the new translations: stored as a new translation of its source
// revision and written (and published) as a new file, so the "AI full" track shows them.
static void gloss_apply_full(struct job *j, struct gl_set *s, int *changed)
{
    struct nanum *n = j->n;
    void *tmp = talloc_new(NULL);
    mp_mutex_lock(&n->rlock);
    if (n->dcues != s->base) {          // replaced by a newer result meanwhile
        mp_mutex_unlock(&n->rlock);
        talloc_free(tmp);
        return;
    }
    size_t nc = n->ndcues;
    struct lib_cue *cues = talloc_array(tmp, struct lib_cue, nc ? nc : 1);
    for (size_t i = 0; i < nc; i++) {
        cues[i] = (struct lib_cue){
            .pts = n->dcues[i].pts, .dur = n->dcues[i].dur,
            .src = talloc_strdup(tmp, n->dcues[i].src),
            .tr = n->dcues[i].tr ? talloc_strdup(tmp, n->dcues[i].tr) : NULL,
        };
    }
    struct libsrc ls = n->dls;
    ls.media_key = talloc_strdup(tmp, n->dls.media_key ? n->dls.media_key : "");
    ls.source_key = talloc_strdup(tmp, n->dls.source_key ? n->dls.source_key : "");
    ls.input = talloc_strdup(tmp, n->dls.input ? n->dls.input : "");
    int64_t rev = n->dsource_rev;
    mp_mutex_unlock(&n->rlock);

    int ch = 0;
    mp_mutex_lock(&j->dlock);
    for (size_t k = 0; k < s->n; k++) {
        if (!s->hit[k] || !s->out[k])
            continue;
        int i = s->ids[k];
        if (!str_eq(cues[i].tr, s->out[k])) {
            cues[i].tr = talloc_strdup(tmp, s->out[k]);
            ch++;
        }
    }
    mp_mutex_unlock(&j->dlock);
    if (ch) {
        int64_t tr_id = 0;
        if (ls.save && n->lib) {
            tr_id = lib_save_translation(n->lib, rev, ls.target, ls.tr_engine, ls.tr_model,
                                         cues, nc);
            if (!tr_id)
                mp_err(n->log, "cannot save the translation: %s\n", lib_error(n->lib));
        }
        char *dir = result_dir(tmp, j->cfg);
        char *stem = result_stem(tmp, &ls, tr_id);
        if (ls.save && tr_id)
            lib_publish(n, &ls, LS_USING, NULL);
        publish_done(n, dir, stem, cues, nc, &ls, rev, j->gl_target);
        *changed += ch;
        mp_info(n->log, "glossary: full translation updated (%d lines)\n", ch);
    }
    talloc_free(tmp);
}

// JOB_GLOSS: translates again the cues the glossary change affects.
static void gloss_run(struct job *j)
{
    struct nanum *n = j->n;
    void *tmp = talloc_new(NULL);
    char why[1024];
    const char *err = NULL;
    int changed = 0;
    bool any = false;
    if (!load_api(n, NANUM_ACCEL_CPU, &j->eng, &j->api, why, sizeof(why))) {
        mp_err(n->log, "glossary: %s\n", why);
        gl_set_state(n, GS_ERROR, 0, 0, why);
        goto out;
    }
    mp_mutex_lock(&n->slock);
    char *live_base = talloc_strdup(tmp, n->gl_base ? n->gl_base : "");
    int64_t changed_at = n->glossary_changed;
    mp_mutex_unlock(&n->slock);
    mp_mutex_lock(&n->rlock);
    char *full_base = n->dgl && n->dcues ? talloc_strdup(tmp, n->dgl) : NULL;
    mp_mutex_unlock(&n->rlock);

    size_t nk_live, nk_full = 0;
    char **k_live = glossary_diff(tmp, live_base, j->gl_target, &nk_live);
    char **k_full = full_base ? glossary_diff(tmp, full_base, j->gl_target, &nk_full) : NULL;
    // Lines that were in flight when the engines got the new glossary can still carry
    // the old terms: everything that entered the store shortly after counts too.
    int64_t deadline = (changed_at ? changed_at : mp_time_ns()) + MP_TIME_S_TO_NS(30);
    int64_t since = 0;

    for (int round = 0; round < 3 && !job_cancelled(j) && !err; round++) {
        struct gl_set sets[NANUM_INPUTS + 1] = {0};
        size_t total = 0, done = 0;
        int64_t snap = mp_time_ns();
        for (int k = 0; k < NANUM_INPUTS; k++)
            gl_snap_live(j, &sets[k], k, k_live, nk_live, since, deadline);
        if (round == 0 && full_base)
            gl_snap_full(j, &sets[GSET_FULL], k_full, nk_full);
        for (int k = 0; k <= NANUM_INPUTS; k++)
            total += sets[k].nhits;
        if (!total)
            break;
        if (!any) {
            any = true;
            gl_set_state(n, GS_RUNNING, 0, 0, "");
            mp_info(n->log, "glossary changed: translating %zu cues again\n", total);
        }
        for (int k = 0; k <= NANUM_INPUTS && !err && !job_cancelled(j); k++) {
            if (!sets[k].nhits)
                continue;
            err = gloss_translate(j, &sets[k], &done, total, &changed);
            if (!err && !job_cancelled(j) && k == GSET_FULL)
                gloss_apply_full(j, &sets[k], &changed);
        }
        since = snap;
    }
    if (job_cancelled(j))
        goto out;
    if (err) {
        mp_err(n->log, "glossary: %s\n", err);
        gl_set_state(n, GS_ERROR, 0, changed, err);
        goto out;
    }
    // Every cue now agrees with the target glossary.
    mp_mutex_lock(&n->slock);
    free(n->gl_base);
    n->gl_base = strdup(j->gl_target);
    mp_mutex_unlock(&n->slock);
    mp_mutex_lock(&n->rlock);
    if (n->dcues && n->dgl && strcmp(n->dgl, j->gl_target)) {
        talloc_free(n->dgl);
        n->dgl = talloc_strdup(n->dctx, j->gl_target);
    }
    mp_mutex_unlock(&n->rlock);
    if (any) {
        gl_set_state(n, GS_DONE, 1.0, changed, "");
        mp_info(n->log, "glossary: %d translations changed\n", changed);
    } else {
        gl_set_state(n, GS_IDLE, 0, 0, "");
    }
out:
    unload_api(&j->eng);
    talloc_free(tmp);
}

// ---------------------------------------------------------------------------
// Imported subtitle files (OpenSubtitles, Jimaku, any file the user provides)
//
// nanum-import-sub="<path>|<lang or empty>|<origin>" starts a job that reads the file
// (encoding and language detected), and translates it unless it already is in the target
// language. The ctl thread then aligns its times to the speech recognition cues of the
// playback input (best offset and, among the usual frame-rate ratios, scale) and shows the
// result as a track of its own. The cues are stored in the library in the file's times
// together with the alignment, so reopening the media shows them at once.

static void imp_set(struct nanum *n, int state, double progress, const char *err,
                    const char *note)
{
    mp_mutex_lock(&n->slock);
    n->imp_state = state;
    n->imp_progress = floor(MPCLAMP(progress, 0.0, 1.0) * 1000.0) / 1000.0;
    snprintf(n->imp_error, sizeof(n->imp_error), "%s", err ? err : "");
    snprintf(n->imp_note, sizeof(n->imp_note), "%s", note ? note : "");
    mp_mutex_unlock(&n->slock);
}

static void imp_progress(struct nanum *n, int state, double progress)
{
    mp_mutex_lock(&n->slock);
    if (n->imp_state == state) {
        n->imp_progress = floor(MPCLAMP(progress, 0.0, 0.999) * 1000.0) / 1000.0;
    }
    mp_mutex_unlock(&n->slock);
}

// "<path>|<lang>|<origin>": the last two fields are optional.
static void imp_parse(void *ctx, const char *req, char **path, char **lang, char **origin)
{
    char *s = talloc_strdup(ctx, req);
    *lang = talloc_strdup(ctx, "");
    *origin = talloc_strdup(ctx, "");
    char *bar = strrchr(s, '|');
    if (bar) {
        *bar = '\0';
        *origin = talloc_strdup(ctx, bar + 1);
        bar = strrchr(s, '|');
        if (bar) {
            *bar = '\0';
            *lang = talloc_strdup(ctx, bar + 1);
        }
    }
    *path = s;
    if (!(*origin)[0]) {
        talloc_free(*origin);
        *origin = talloc_strdup(ctx, "파일");
    }
}

static void imp_split_request(struct job *j, const char *req)
{
    char *lang;
    imp_parse(j, req, &j->imp_path, &lang, &j->imp_origin);
    snprintf(j->imp_lang, sizeof(j->imp_lang), "%s", lang);
}

static bool lang_is_target(struct nanum *n, const char *lang)
{
    static const char *const names[][2] = {
        {"ko", "Korean"}, {"en", "English"}, {"ja", "Japanese"}, {"zh", "Chinese"},
        {"fr", "French"}, {"de", "German"}, {"es", "Spanish"}, {"it", "Italian"},
        {"ru", "Russian"}, {"pt", "Portuguese"},
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(names); i++) {
        if (!strcmp(lang, names[i][0]) && !strcasecmp(n->opts->target_language, names[i][1]))
            return true;
    }
    return !strcasecmp(n->opts->target_language, lang);
}

// ---- encoding and language detection

static bool utf8_valid(const unsigned char *s, size_t n)
{
    struct bstr b = {(unsigned char *)s, n};
    return bstr_validate_utf8(b) >= 0;
}

struct char_classes {
    size_t hangul, kana, cjk, cyrillic, latin, other, letters;
};

static void count_classes(const char *text, struct char_classes *c)
{
    *c = (struct char_classes){0};
    struct bstr str = bstr0(text);
    while (str.len) {
        struct bstr next;
        int ch = bstr_decode_utf8(str, &next);
        if (ch < 0) {
            str = bstr_cut(str, 1);
            continue;
        }
        str = next;
        if (ch >= 0xac00 && ch <= 0xd7a3) {
            c->hangul++;
        } else if (ch >= 0x3040 && ch <= 0x30ff) {
            c->kana++;
        } else if ((ch >= 0x4e00 && ch <= 0x9fff) || (ch >= 0x3400 && ch <= 0x4dbf)) {
            c->cjk++;
        } else if (ch >= 0x400 && ch <= 0x4ff) {
            c->cyrillic++;
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                   (ch >= 0xc0 && ch <= 0x24f)) {
            c->latin++;
        } else {
            continue;
        }
        c->letters++;
    }
}

// ISO 639-1 of subtitle text: Hangul -> ko, kana -> ja, ideographs alone -> zh, Cyrillic -> ru,
// otherwise en.
static const char *detect_language(const char *text)
{
    struct char_classes c;
    count_classes(text, &c);
    if (!c.letters)
        return "en";
    if (c.hangul * 100 >= c.letters * 15)
        return "ko";
    if (c.kana * 100 >= c.letters * 5)
        return "ja";
    if (c.cjk * 100 >= c.letters * 30)
        return "zh";
    if (c.cyrillic * 100 >= c.letters * 30)
        return "ru";
    return "en";
}

#ifdef _WIN32
static char *wide_to_utf8(void *ctx, const wchar_t *w, int wl)
{
    int ul = WideCharToMultiByte(CP_UTF8, 0, w, wl, NULL, 0, NULL, NULL);
    if (ul <= 0)
        return NULL;
    char *u = talloc_array(ctx, char, ul + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, wl, u, ul, NULL, NULL);
    u[ul] = '\0';
    return u;
}

static char *cp_to_utf8(void *ctx, unsigned cp, const unsigned char *s, size_t n, bool strict)
{
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    int wl = MultiByteToWideChar(cp, flags, (const char *)s, (int)n, NULL, 0);
    if (wl <= 0)
        return NULL;
    wchar_t *w = talloc_array(ctx, wchar_t, wl);
    MultiByteToWideChar(cp, flags, (const char *)s, (int)n, w, wl);
    return wide_to_utf8(ctx, w, wl);
}

static char *utf16_to_utf8(void *ctx, const unsigned char *s, size_t n, bool big)
{
    size_t wl = n / 2;
    wchar_t *w = talloc_array(ctx, wchar_t, wl + 1);
    for (size_t i = 0; i < wl; i++)
        w[i] = big ? (s[2 * i] << 8 | s[2 * i + 1]) : (s[2 * i + 1] << 8 | s[2 * i]);
    return wide_to_utf8(ctx, w, (int)wl);
}
#endif

// The text of a subtitle file as UTF-8 (a talloc child of ctx) and the name of the encoding it
// was in. A byte order mark decides; else UTF-8 if the bytes are valid UTF-8; else the
// Korean (CP949) or Japanese (Shift-JIS) code page if the text then reads as that language;
// else Windows-1252.
static char *decode_text(void *ctx, const unsigned char *s, size_t n, char enc[24])
{
    if (n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) {
        snprintf(enc, 24, "UTF-8");
        return talloc_strndup(ctx, (const char *)s + 3, n - 3);
    }
#ifdef _WIN32
    if (n >= 2 && s[0] == 0xff && s[1] == 0xfe) {
        snprintf(enc, 24, "UTF-16LE");
        return utf16_to_utf8(ctx, s + 2, n - 2, false);
    }
    if (n >= 2 && s[0] == 0xfe && s[1] == 0xff) {
        snprintf(enc, 24, "UTF-16BE");
        return utf16_to_utf8(ctx, s + 2, n - 2, true);
    }
    size_t m = MPMIN(n, 4096), odd = 0, even = 0;
    for (size_t i = 0; i < m; i++) {
        if (!s[i])
            (i & 1) ? odd++ : even++;
    }
    if (odd * 4 > m / 2 && even == 0) {
        snprintf(enc, 24, "UTF-16LE");
        return utf16_to_utf8(ctx, s, n, false);
    }
    if (even * 4 > m / 2 && odd == 0) {
        snprintf(enc, 24, "UTF-16BE");
        return utf16_to_utf8(ctx, s, n, true);
    }
#endif
    if (utf8_valid(s, n)) {
        snprintf(enc, 24, "UTF-8");
        return talloc_strndup(ctx, (const char *)s, n);
    }
#ifdef _WIN32
    char *ko = cp_to_utf8(ctx, 949, s, n, true);
    char *ja = cp_to_utf8(ctx, 932, s, n, true);
    struct char_classes ck = {0}, cj = {0};
    if (ko)
        count_classes(ko, &ck);
    if (ja)
        count_classes(ja, &cj);
    if (ko && ck.hangul * 100 >= ck.letters * 60 && ck.hangul >= cj.kana) {
        snprintf(enc, 24, "CP949");
        return ko;
    }
    if (ja && cj.kana * 100 >= cj.letters * 12) {
        snprintf(enc, 24, "Shift-JIS");
        return ja;
    }
    char *w = cp_to_utf8(ctx, 1252, s, n, false);
    if (w) {
        snprintf(enc, 24, "CP1252");
        return w;
    }
#endif
    // Latin-1.
    snprintf(enc, 24, "CP1252");
    char *out = talloc_array(ctx, char, n * 2 + 1), *p = out;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x80) {
            *p++ = s[i];
        } else {
            *p++ = 0xc0 | (s[i] >> 6);
            *p++ = 0x80 | (s[i] & 0x3f);
        }
    }
    *p = '\0';
    return out;
}

// The dialogue of a cue without markup: {override tags}, <html tags>, line breaks.
static char *sub_clean(void *ctx, const char *t)
{
    char *out = talloc_array(ctx, char, strlen(t) + 1);
    size_t o = 0;
    bool space = true;
    for (const char *p = t; *p; p++) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (e) {
                p = e;
                continue;
            }
        } else if (*p == '<' && (isalpha((unsigned char)p[1]) || p[1] == '/')) {
            const char *e = strchr(p, '>');
            if (e) {
                p = e;
                continue;
            }
        } else if (*p == '\\' && (p[1] == 'N' || p[1] == 'n' || p[1] == 'h')) {
            p++;
            if (!space)
                out[o++] = ' ';
            space = true;
            continue;
        }
        char c = *p;
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ') {
            if (space)
                continue;
            space = true;
        } else {
            space = false;
        }
        out[o++] = c;
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
    return out;
}

static int cmp_cue(const void *a, const void *b)
{
    const struct lib_cue *x = a, *y = b;
    return x->pts < y->pts ? -1 : x->pts > y->pts ? 1 : 0;
}

// Job thread: reads the file, finds its language and translates it.
static void imp_run(struct job *j)
{
    struct nanum *n = j->n;
    void *tmp = talloc_new(NULL);
    struct demuxer *d = NULL;
    struct nanum_ai *ai = NULL;
    bool eng = false;
    char *tmpfile = NULL;
    char errbuf[320] = "";

    imp_set(n, IS_TRANSLATING, 0, "", "자막을 읽는 중");
    FILE *f = fopen(j->imp_path, "rb");
    if (!f) {
        snprintf(j->imp_err, sizeof(j->imp_err), "cannot open the subtitle file");
        goto out;
    }
    fseeko(f, 0, SEEK_END);
    int64_t size = ftello(f);
    fseeko(f, 0, SEEK_SET);
    if (size <= 0 || size > 32 * 1024 * 1024) {
        fclose(f);
        snprintf(j->imp_err, sizeof(j->imp_err), "the subtitle file is empty or too large");
        goto out;
    }
    unsigned char *raw = talloc_array(tmp, unsigned char, size);
    size_t got = fread(raw, 1, size, f);
    fclose(f);
    if (got != (size_t)size) {
        snprintf(j->imp_err, sizeof(j->imp_err), "cannot read the subtitle file");
        goto out;
    }
    char hex[65];
    lib_sha256_hex(raw, size, hex);
    snprintf(j->imp_hash, sizeof(j->imp_hash), "%.16s", hex);
    char *text = decode_text(tmp, raw, size, j->imp_enc);
    if (!text) {
        snprintf(j->imp_err, sizeof(j->imp_err), "cannot decode the subtitle file");
        goto out;
    }

    // The demuxer reads the UTF-8 copy (it would guess the encoding itself).
    const char *ext = strrchr(j->imp_path, '.');
    if (!ext || strlen(ext) > 8 || strpbrk(ext, "/\\"))
        ext = ".srt";
    const char *tdir = getenv("TEMP");
    tmpfile = talloc_asprintf(NULL, "%s/nanum-import-%lld%s", tdir && tdir[0] ? tdir : ".",
                              (long long)(mp_time_ns() / 1000), ext);
    FILE *tf = fopen(tmpfile, "wb");
    if (!tf || fwrite(text, 1, strlen(text), tf) != strlen(text)) {
        if (tf)
            fclose(tf);
        snprintf(j->imp_err, sizeof(j->imp_err), "cannot write the temporary subtitle file");
        goto out;
    }
    fclose(tf);

    struct sh_stream *sh = NULL;
    j->cfg->sub_id = -1;
    const char *err = open_source(j, tmpfile, false, true, &d, &sh);
    if (err) {
        snprintf(j->imp_err, sizeof(j->imp_err), "%s: this is not a subtitle file the player can read", err);
        goto out;
    }
    struct lib_cue *cues = NULL;
    size_t nc = 0;
    while (!job_cancelled(j)) {
        struct demux_packet *pkt = NULL;
        int r = demux_read_packet_async(sh, &pkt);
        if (r < 0)
            break;
        if (r == 0 || !pkt) {
            mp_sleep_ns(MP_TIME_MS_TO_NS(5));
            continue;
        }
        char *t = pkt->pts != MP_NOPTS_VALUE ? packet_text(tmp, sh->codec->codec, pkt) : NULL;
        if (t) {
            char *clean = sub_clean(tmp, t);
            if (clean[0]) {
                MP_TARRAY_APPEND(j, cues, nc, ((struct lib_cue){
                    .pts = pkt->pts,
                    .dur = pkt->duration > 0 ? pkt->duration : 2.5,
                    .src = talloc_strdup(j, clean),
                }));
            }
        }
        talloc_free(pkt);
    }
    demux_free(d);
    d = NULL;
    if (job_cancelled(j))
        goto out;
    if (!nc) {
        snprintf(j->imp_err, sizeof(j->imp_err), "the subtitle file has no text");
        goto out;
    }
    qsort(cues, nc, sizeof(*cues), cmp_cue);
    // Exact repeats (the same line twice at the same time) are one line.
    size_t w = 0;
    for (size_t i = 0; i < nc; i++) {
        if (w && cues[w - 1].pts == cues[i].pts && !strcmp(cues[w - 1].src, cues[i].src))
            continue;
        cues[w++] = cues[i];
    }
    nc = w;

    if (!j->imp_lang[0]) {
        char *sample = talloc_strdup(tmp, "");
        for (size_t i = 0; i < nc && i < 300; i++)
            sample = talloc_asprintf_append(sample, "%s\n", cues[i].src);
        snprintf(j->imp_lang, sizeof(j->imp_lang), "%s", detect_language(sample));
    }
    mp_info(n->log, "import: %zu cues, encoding %s, language %s\n", nc, j->imp_enc, j->imp_lang);
    j->imp_cues = cues;
    j->nimp = nc;

    if (lang_is_target(n, j->imp_lang)) {
        for (size_t i = 0; i < nc; i++)
            cues[i].tr = talloc_strdup(j, cues[i].src);
        goto out;
    }
    if (!n->tr_enabled)
        goto out;               // nothing to translate with: the file's text is shown

    char why[1024];
    if (!load_api(n, NANUM_ACCEL_CPU, &j->eng, &j->api, why, sizeof(why))) {
        snprintf(j->imp_err, sizeof(j->imp_err), "%s", why);
        goto out;
    }
    eng = true;
    struct tr_snap ts;
    tr_snapshot(n, tmp, &ts);
    struct nanum_ai_config cfg = {
        .abi_version = NANUM_AI_ABI_VERSION,
        .language = j->imp_lang,
        .translate_endpoint = n->opts->tr_endpoint,
        .translate_model = n->opts->tr_model,
        .target_language = n->opts->target_language,
        .translate_engine = j->cfg->engine == ENGINE_OMP ? "omp" : "ollama",
        .omp_model = j->cfg->omp_model,
        .omp_command = j->cfg->omp_command,
        .scan_only = 1,
        .translate_only = 1,
        .translate_style = ts.style,
        .translate_style_text = ts.style_text,
        .glossary = ts.glossary,
    };
    ai = j->api.create(&cfg, scan_event_cb, j);
    if (!ai) {
        snprintf(j->imp_err, sizeof(j->imp_err), "cannot start the translation engine: %s",
                 j->api.last_error());
        goto out;
    }
    for (size_t i = 0; i < nc; i++)
        j->api.push_text(ai, cues[i].pts, cues[i].dur, cues[i].src);
    if (j->api.transcript_size(ai) == 0) {
        snprintf(j->imp_err, sizeof(j->imp_err), "the subtitle file has nothing to translate");
        goto out;
    }
    int r = job_translate(j, ai, errbuf, sizeof(errbuf));
    if (r > 0)
        goto out;               // cancelled
    if (r < 0) {
        snprintf(j->imp_err, sizeof(j->imp_err), "%s", errbuf);
        goto out;
    }
    // The translation comes back for the lines the engine kept (it drops sound-only
    // lines): match them by time and drop the rest.
    size_t ci = 0, kept = 0;
    for (size_t k = 0; k < j->nlines; k++) {
        while (ci < nc && cues[ci].pts < j->lines[k].pts - 0.002)
            ci++;
        if (ci < nc && fabs(cues[ci].pts - j->lines[k].pts) <= 0.002) {
            cues[ci].tr = talloc_strdup(j, j->lines[k].translation);
            ci++;
            kept++;
        }
    }
    w = 0;
    for (size_t i = 0; i < nc; i++) {
        if (cues[i].tr)
            cues[w++] = cues[i];
    }
    j->nimp = w;
    mp_info(n->log, "import: translated %zu of %zu cues\n", kept, nc);
    job_keep_glossary(j, ai, tmp);
out:
    if (ai) {
        j->api.cancel_full(ai);
        j->api.destroy(ai);
    }
    if (eng)
        unload_api(&j->eng);
    if (d)
        demux_free(d);
    if (tmpfile) {
        unlink(tmpfile);
        talloc_free(tmpfile);
    }
    talloc_free(tmp);
}

// ---- alignment against the speech recognition cues

#define IMP_MAXOFF 240.0        // the largest shift searched, seconds
#define IMP_BIN 0.1
#define IMP_TOL 0.5             // a cue start within this of a reference start matches
#define IMP_MIN_REF 8           // reference cues needed for a fit
#define IMP_ACCEPT_CONF 0.35
#define IMP_GOOD_CONF 0.6

static const double imp_scales[] = {
    1.0, 23.976 / 24.0, 24.0 / 23.976, 25.0 / 23.976, 23.976 / 25.0,
};

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Index of the first element >= v.
static int lower_idx(const double *v, int n, double x)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (v[mid] < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// How well the file's cue starts b (times of the file) map onto the reference starts a with
// time = scale * b + offset: the number of reference cues with a file cue within IMP_TOL, and
// the number expected by chance in the overlap of both.
static void fit_score(const double *a, int na, const double *b, int nb, double s, double o,
                      int *matched, double *expected, double *conf)
{
    int m = 0;
    for (int j = 0; j < na; j++) {
        double target = (a[j] - o) / s;
        int i = lower_idx(b, nb, target - IMP_TOL / s);
        if (i < nb && b[i] <= target + IMP_TOL / s)
            m++;
    }
    double b0 = s * b[0] + o, b1 = s * b[nb - 1] + o;
    int na_ov = 0, nb_ov = 0;
    for (int j = 0; j < na; j++) {
        if (a[j] >= b0 - IMP_TOL && a[j] <= b1 + IMP_TOL)
            na_ov++;
    }
    for (int i = 0; i < nb; i++) {
        double t = s * b[i] + o;
        if (t >= a[0] - IMP_TOL && t <= a[na - 1] + IMP_TOL)
            nb_ov++;
    }
    double span = fmax(1.0, fmin(a[na - 1], b1) - fmax(a[0], b0));
    double exp = na_ov * fmin(1.0, nb_ov / span);
    double nn = MPMIN(na_ov, nb_ov);
    *matched = m;
    *expected = exp;
    *conf = nn > exp + 0.5 ? MPCLAMP((m - exp) / (nn - exp), 0.0, 1.0) : 0.0;
}

// Best offset and scale putting the file's cue starts b onto the reference starts a (both
// sorted). The offset is found from the histogram of differences, refined to the median of
// the differences that agree.
static void align_fit(const double *a, int na, const double *b, int nb, struct fit *f)
{
    *f = (struct fit){.scale = 1};
    if (na < IMP_MIN_REF || nb < IMP_MIN_REF)
        return;
    int nbins = (int)(2 * IMP_MAXOFF / IMP_BIN) + 1;
    int *h = calloc(nbins + 12, sizeof(int));
    double *diffs = malloc(sizeof(double) * (size_t)(na + nb + 16) * 8);
    if (!h || !diffs)
        goto done;
    double best_score = -1e9;
    for (size_t si = 0; si < MP_ARRAY_SIZE(imp_scales); si++) {
        double s = imp_scales[si];
        memset(h, 0, sizeof(int) * (nbins + 12));
        for (int j = 0; j < na; j++) {
            int i = lower_idx(b, nb, (a[j] - IMP_MAXOFF) / s);
            for (; i < nb && s * b[i] <= a[j] + IMP_MAXOFF; i++) {
                int bin = (int)((a[j] - s * b[i] + IMP_MAXOFF) / IMP_BIN);
                if (bin >= 0 && bin < nbins)
                    h[bin]++;
            }
        }
        // Window of +-IMP_TOL.
        int w = (int)(IMP_TOL / IMP_BIN), best_bin = 0, best_cnt = -1, sum = 0;
        for (int k = 0; k < nbins; k++) {
            sum += h[k];
            if (k - 2 * w - 1 >= 0)
                sum -= h[k - 2 * w - 1];
            int center = k - w;
            if (center >= 0 && sum > best_cnt) {
                best_cnt = sum;
                best_bin = center;
            }
        }
        if (best_cnt < 2)
            continue;
        double d0 = (best_bin + 0.5) * IMP_BIN - IMP_MAXOFF;
        // Refine: the median of the differences within IMP_TOL of d0.
        size_t nd = 0;
        for (int j = 0; j < na && nd < (size_t)(na + nb + 16) * 8 - 1; j++) {
            double lo = a[j] - d0 - IMP_TOL;
            int i = lower_idx(b, nb, lo / s);
            for (; i < nb && s * b[i] <= a[j] - d0 + IMP_TOL; i++) {
                if (nd < (size_t)(na + nb + 16) * 8 - 1)
                    diffs[nd++] = a[j] - s * b[i];
            }
        }
        double o = d0;
        if (nd) {
            qsort(diffs, nd, sizeof(double), cmp_double);
            o = diffs[nd / 2];
        }
        int m;
        double exp, conf;
        fit_score(a, na, b, nb, s, o, &m, &exp, &conf);
        double score = m - exp - (si ? 1.0 : 0.0);     // the unscaled fit wins ties
        if (score > best_score) {
            best_score = score;
            f->offset = o;
            f->scale = s;
            f->conf = conf;
            f->matched = m;
        }
    }
    f->ok = f->matched >= 5 && f->conf >= IMP_ACCEPT_CONF;
done:
    free(h);
    free(diffs);
}

// The starts of the playback input's cues (the reference) as a sorted array (malloc).
static double *imp_refs(struct nanum *n, int *count)
{
    struct input *in = PRI(n);
    mp_mutex_lock(&n->lock);
    double *a = malloc(sizeof(double) * (in->num_events + 1));
    int na = 0;
    for (int i = 0; a && i < in->num_events; i++)
        a[na++] = in->events[i].pts;
    mp_mutex_unlock(&n->lock);
    *count = na;
    return a;
}

// ---- showing the import

static void imp_publish(struct ctl *c, const char *note)
{
    struct nanum *n = c->n;
    mp_mutex_lock(&n->slock);
    n->imp_offset = c->imp.offset;
    n->imp_scale = c->imp.scale;
    n->imp_conf = c->imp.conf;
    n->imp_low = c->imp.low;
    snprintf(n->imp_lang, sizeof(n->imp_lang), "%s", c->imp.lang);
    snprintf(n->imp_origin, sizeof(n->imp_origin), "%s", c->imp.origin);
    if (note)
        snprintf(n->imp_note, sizeof(n->imp_note), "%s", note);
    mp_mutex_unlock(&n->slock);
}

static void imp_persist_props(struct ctl *c)
{
    struct nanum *n = c->n;
    if (!n->lib || !c->ident_ok || !c->imp.key[0])
        return;
    char k1[128], k2[160], v[256];
    snprintf(k1, sizeof(k1), "import.cur.%s", n->opts->target_language);
    snprintf(k2, sizeof(k2), "import.align.%s", c->imp.key);
    snprintf(v, sizeof(v), "%.4f|%.6f|%.3f|%d|%d|%s|%s", c->imp.offset, c->imp.scale,
             c->imp.conf, c->imp.low, c->imp.direct, c->imp.lang, c->imp.origin);
    lib_prop_set(n->lib, c->media_id, k1, c->imp.key);
    lib_prop_set(n->lib, c->media_id, k2, v);
}

// Replaces the import track's cues. The cues are in the file's times.
static void imp_fill(struct nanum *n, const struct lib_cue *cues, size_t nc, double off,
                     double scale)
{
    struct input *in = IMPT(n);
    mp_mutex_lock(&n->lock);
    for (int i = 0; i < in->num_events; i++) {
        talloc_free(in->events[i].src);
        talloc_free(in->events[i].tr);
    }
    in->num_events = 0;
    in->inj_pts = -INFINITY;
    for (size_t i = 0; i < nc; i++) {
        int at = insert_sorted_locked(in, cues[i].pts * scale + off, cues[i].dur * scale,
                                      cues[i].src, cues[i].tr);
        in->events[at].raw_pts = cues[i].pts;
    }
    in->dirty = true;
    mp_mutex_unlock(&n->lock);
    atomic_fetch_add(&in->gen, 1);
    atomic_store(&in->events_seen, nc > 0);
    wake_demux(n);
}

// A new alignment of the shown cues.
static void imp_retransform(struct nanum *n, double old_scale, double off, double scale)
{
    struct input *in = IMPT(n);
    mp_mutex_lock(&n->lock);
    for (int i = 0; i < in->num_events; i++) {
        struct nanum_event *e = &in->events[i];
        e->pts = e->raw_pts * scale + off;
        e->duration = e->duration / old_scale * scale;
    }
    in->inj_pts = -INFINITY;
    in->dirty = true;
    mp_mutex_unlock(&n->lock);
    atomic_fetch_add(&in->gen, 1);
    atomic_store(&in->save_now, true);
    wake_demux(n);
}

static void imp_show_track(struct ctl *c)
{
    struct nanum *n = c->n;
    char title[160];
    make_title_import(n, c->imp.origin, c->imp.lang, c->imp.direct, title, sizeof(title));
    mp_mutex_lock(&n->slock);
    snprintf(IMPT(n)->lang, sizeof(IMPT(n)->lang), "%s", c->imp.lang);
    mp_mutex_unlock(&n->slock);
    set_title(n, IN_IMP, title);
    atomic_store(&n->stream_wanted[IN_IMP], true);
    wake_demux(n);
}

// Shows the waiting result with the given alignment.
static void imp_place(struct ctl *c, const struct fit *f, bool low, const char *note, int refs)
{
    struct nanum *n = c->n;
    struct input *in = IMPT(n);
    persist_input(n, in);       // what the previous import changed
    c->imp.offset = low ? 0 : f->offset;
    c->imp.scale = low ? 1 : f->scale;
    c->imp.conf = f->conf;
    c->imp.matched = f->matched;
    c->imp.low = low;
    c->imp.adjusted = false;
    c->imp.fit_refs = refs;
    mp_mutex_lock(&n->slock);
    n->imp_scale = c->imp.scale;
    mp_mutex_unlock(&n->slock);
    imp_fill(n, c->imp.cues, c->imp.ncues, c->imp.offset, c->imp.scale);
    snprintf(c->imp.key, sizeof(c->imp.key), "import|%s|tgt=%s", c->imp.hash,
             n->opts->target_language);
    free(in->key);
    in->key = strdup(c->imp.key);
    in->media_id = c->media_id;
    in->persist = n->lib && c->ident_ok && in->key;
    atomic_store(&in->save_now, true);
    imp_show_track(c);
    imp_persist_props(c);
    mp_mutex_lock(&n->slock);
    n->imp_lines = in->num_events;
    mp_mutex_unlock(&n->slock);
    imp_publish(c, note);
    imp_set(n, IS_DONE, 1.0, "", note);
    c->imp.shown = true;
    c->imp.waiting = false;
    talloc_free(c->imp.ctx);
    c->imp.ctx = NULL;
    c->imp.cues = NULL;
    c->imp.ncues = 0;
    mp_info(n->log, "import shown: %d cues, offset %.2f s, scale %.5f, confidence %.2f%s\n",
            in->num_events, c->imp.offset, c->imp.scale, c->imp.conf, low ? " (low)" : "");
}

// Waits for enough reference cues, then fits the waiting import.
static void imp_try_place(struct ctl *c, int64_t now)
{
    struct nanum *n = c->n;
    int na;
    double *a = imp_refs(n, &na);
    if (a && (size_t)na != c->imp.fit_refs) {
        c->imp.fit_refs = na;
        double *b = malloc(sizeof(double) * (c->imp.ncues + 1));
        size_t nb = 0;
        for (size_t i = 0; b && i < c->imp.ncues; i++)
            b[nb++] = c->imp.cues[i].pts;
        if (b)
            align_fit(a, na, b, (int)nb, &c->imp.last);
        free(b);
    }
    free(a);
    struct fit f = c->imp.last;
    double waited = (now - c->imp.since) / 1e9;
    bool dead = !atomic_load(&n->ai_enabled) || atomic_load(&PRI(n)->ai_failed) ||
                (atomic_load(&n->gen_off) && na < IMP_MIN_REF);
    // Placed at once when the fit is convincing: many cues agree (a few can agree by chance).
    if (f.ok && ((f.conf >= IMP_GOOD_CONF && f.matched >= 10) ||
                 (f.conf >= 0.9 && f.matched >= 7))) {
        imp_place(c, &f, false, "", na);
        return;
    }
    if (dead || waited > 150) {
        char note[256];
        if (f.ok) {
            snprintf(note, sizeof(note), "정렬 신뢰도가 낮습니다 (%.0f%%)", f.conf * 100);
            imp_place(c, &f, false, note, na);
        } else {
            snprintf(note, sizeof(note), na < IMP_MIN_REF
                     ? "음성 인식 자막이 부족해 정렬하지 못했습니다: 시간 이동 0초 유지"
                     : "정렬 신뢰도가 낮아 시간 이동 0초를 유지합니다");
            struct fit none = {.scale = 1, .conf = f.conf, .matched = f.matched};
            imp_place(c, &none, true, note, na);
        }
        return;
    }
    imp_set(n, IS_ALIGNING, MPMIN(0.95, na / 24.0), "", "음성 인식 자막과 시간 맞추는 중");
}

// More reference cues than at the last fit: a better alignment replaces a weak one.
static void imp_refine(struct ctl *c)
{
    struct nanum *n = c->n;
    struct input *in = IMPT(n);
    if (c->imp.adjusted || (!c->imp.low && c->imp.conf >= 0.75))
        return;
    int na;
    double *a = imp_refs(n, &na);
    if (!a || na < IMP_MIN_REF || na < (int)(c->imp.fit_refs * 13 / 10) + 4) {
        free(a);
        return;
    }
    c->imp.fit_refs = na;
    mp_mutex_lock(&n->lock);
    int nb = in->num_events;
    double *b = malloc(sizeof(double) * (nb + 1));
    for (int i = 0; b && i < nb; i++)
        b[i] = in->events[i].raw_pts;
    mp_mutex_unlock(&n->lock);
    struct fit f;
    if (b)
        align_fit(a, na, b, nb, &f);
    else
        f = (struct fit){0};
    free(a);
    free(b);
    if (!f.ok || !(c->imp.low || f.conf > c->imp.conf + 0.1))
        return;
    double old_scale = c->imp.scale;
    c->imp.offset = f.offset;
    c->imp.scale = f.scale;
    c->imp.conf = f.conf;
    c->imp.matched = f.matched;
    c->imp.low = false;
    mp_mutex_lock(&n->slock);
    n->imp_scale = f.scale;
    mp_mutex_unlock(&n->slock);
    imp_retransform(n, old_scale, f.offset, f.scale);
    imp_persist_props(c);
    imp_publish(c, f.conf >= IMP_GOOD_CONF ? "" : "정렬 신뢰도가 낮습니다");
    mp_info(n->log, "import realigned: offset %.2f s, scale %.5f, confidence %.2f\n", f.offset,
            f.scale, f.conf);
}

// nanum-import-adjust: moves the import track by delta seconds (kept per media).
static void imp_adjust(struct ctl *c, double delta)
{
    struct nanum *n = c->n;
    if (!c->imp.shown)
        return;
    c->imp.offset = round((c->imp.offset + delta) * 1000.0) / 1000.0;
    c->imp.adjusted = true;
    imp_retransform(n, c->imp.scale, c->imp.offset, c->imp.scale);
    imp_persist_props(c);
    imp_publish(c, NULL);
    mp_info(n->log, "import offset adjusted by %+.2f s: %.2f s\n", delta, c->imp.offset);
}

// The saved import of this media (and target language), if there is one.
static void imp_load_saved(struct ctl *c)
{
    struct nanum *n = c->n;
    if (!n->lib || !c->ident_ok)
        return;
    void *tmp = talloc_new(NULL);
    char k1[128];
    snprintf(k1, sizeof(k1), "import.cur.%s", n->opts->target_language);
    char *key = lib_prop_get(n->lib, tmp, c->media_id, k1);
    char *k2 = key ? talloc_asprintf(tmp, "import.align.%s", key) : NULL;
    char *v = k2 ? lib_prop_get(n->lib, tmp, c->media_id, k2) : NULL;
    struct lib_cue *cues = NULL;
    struct lib_range *ranges = NULL;
    size_t nc = 0, nr = 0;
    double off = 0, scale = 1, conf = 0;
    int low = 0, direct = 0;
    char lang[16] = "", origin[64] = "";
    if (!v || sscanf(v, "%lf|%lf|%lf|%d|%d|%15[^|]|%63[^\n]", &off, &scale, &conf, &low,
                     &direct, lang, origin) < 6 ||
        !lib_live_load(n->lib, tmp, c->media_id, key, &cues, &nc, &ranges, &nr) || !nc) {
        talloc_free(tmp);
        return;
    }
    if (scale <= 0)
        scale = 1;
    c->imp = (struct imp_ctl){.offset = off, .scale = scale, .conf = conf, .low = low,
                              .direct = direct, .shown = true};
    snprintf(c->imp.lang, sizeof(c->imp.lang), "%s", lang);
    snprintf(c->imp.origin, sizeof(c->imp.origin), "%s", origin[0] ? origin : "파일");
    snprintf(c->imp.key, sizeof(c->imp.key), "%s", key);
    mp_mutex_lock(&n->slock);
    n->imp_scale = scale;
    mp_mutex_unlock(&n->slock);
    struct input *in = IMPT(n);
    imp_fill(n, cues, nc, off, scale);
    mp_mutex_lock(&n->lock);
    in->dirty = false;
    mp_mutex_unlock(&n->lock);
    free(in->key);
    in->key = strdup(key);
    in->media_id = c->media_id;
    in->persist = in->key != NULL;
    imp_show_track(c);
    imp_publish(c, low ? "정렬 신뢰도가 낮아 시간 이동 0초를 유지합니다" : "");
    mp_mutex_lock(&n->slock);
    n->imp_state = IS_DONE;
    n->imp_progress = 1.0;
    n->imp_lines = (int)nc;
    mp_mutex_unlock(&n->slock);
    mp_info(n->log, "saved import: %zu cues (%s), offset %.2f s, scale %.5f\n", nc, origin, off,
            scale);
    talloc_free(tmp);
}

// Takes the result of the import job.
static void imp_absorb(struct ctl *c, struct job *j, int64_t now)
{
    struct nanum *n = c->n;
    if (job_cancelled(j))
        return;
    if (j->imp_err[0]) {
        mp_err(n->log, "import: %s\n", j->imp_err);
        imp_set(n, IS_ERROR, 0, j->imp_err, "");
        return;
    }
    talloc_free(c->imp.ctx);
    c->imp = (struct imp_ctl){.ctx = talloc_new(NULL), .waiting = true, .since = now,
                              .scale = 1.0};
    c->imp.cues = talloc_array(c->imp.ctx, struct lib_cue, j->nimp ? j->nimp : 1);
    for (size_t i = 0; i < j->nimp; i++) {
        c->imp.cues[i] = (struct lib_cue){
            .pts = j->imp_cues[i].pts,
            .dur = j->imp_cues[i].dur,
            .src = talloc_strdup(c->imp.ctx, j->imp_cues[i].src),
            .tr = j->imp_cues[i].tr ? talloc_strdup(c->imp.ctx, j->imp_cues[i].tr) : NULL,
        };
    }
    c->imp.ncues = j->nimp;
    c->imp.direct = lang_is_target(n, j->imp_lang);
    snprintf(c->imp.lang, sizeof(c->imp.lang), "%s", j->imp_lang);
    snprintf(c->imp.origin, sizeof(c->imp.origin), "%s", j->imp_origin);
    snprintf(c->imp.enc, sizeof(c->imp.enc), "%s", j->imp_enc);
    snprintf(c->imp.hash, sizeof(c->imp.hash), "%s", j->imp_hash);
    mp_mutex_lock(&n->slock);
    n->imp_lines = (int)j->nimp;
    mp_mutex_unlock(&n->slock);
    imp_publish(c, NULL);
    imp_set(n, IS_ALIGNING, 0, "", "음성 인식 자막과 시간 맞추는 중");
    // The glossary the translation extracted is kept with the media.
    if (n->lib && c->ident_ok) {
        mp_mutex_lock(&n->slock);
        char *gl = talloc_strdup(NULL, n->glossary ? n->glossary : "");
        mp_mutex_unlock(&n->slock);
        char key[96];
        snprintf(key, sizeof(key), "glossary.%s", n->opts->target_language);
        lib_prop_set(n->lib, c->media_id, key, gl);
        talloc_free(gl);
    }
}

// nanum-import-sub, nanum-import-adjust, the saved import, the running job and the alignment.
static void ctl_import(struct ctl *c, int64_t now)
{
    struct nanum *n = c->n;
    const char *req = c->cur->import_req;
    if (!req[0]) {
        c->imp_req[0] = '\0';
    } else if (strcmp(req, c->imp_req)) {
        snprintf(c->imp_req, sizeof(c->imp_req), "%s", req);
        job_stop(&c->ijob);
        void *tmp = talloc_new(NULL);
        char *path, *lang, *origin;
        imp_parse(tmp, req, &path, &lang, &origin);
        // The newest request replaces an import that still waits to be shown.
        c->imp.waiting = false;
        talloc_free(c->imp.ctx);
        c->imp.ctx = NULL;
        c->imp.cues = NULL;
        if (!mp_path_exists(path)) {
            imp_set(n, IS_ERROR, 0, "the subtitle file does not exist", "");
            mp_err(n->log, "import: '%s' does not exist\n", path);
        } else {
            imp_set(n, IS_TRANSLATING, 0, "", "");
            c->ijob = job_start(n, JOB_IMPORT, c->cur, "", 0, -1, NULL, req);
            if (!c->ijob)
                imp_set(n, IS_ERROR, 0, "cannot start the import thread", "");
            else
                mp_info(n->log, "import: '%s' (%s)\n", path, origin);
        }
        talloc_free(tmp);
        c->imp_loaded = true;
    }
    double adj = c->cur->import_adjust;
    if (adj == 0.0) {
        c->imp_adjust_req = 0.0;
    } else if (adj != c->imp_adjust_req) {
        c->imp_adjust_req = adj;
        imp_adjust(c, adj);
    }
    if (!c->imp_loaded && c->ident_done) {
        c->imp_loaded = true;
        if (!c->ijob && !c->imp.waiting && !c->imp.shown)
            imp_load_saved(c);
    }
    if (c->ijob && atomic_load(&c->ijob->finished)) {
        struct job *j = c->ijob;
        imp_absorb(c, j, now);
        job_stop(&c->ijob);
    }
    if (c->imp.waiting)
        imp_try_place(c, now);
    else if (c->imp.shown)
        imp_refine(c);
}

static void ctl_apply_view(struct nanum *n, int display)
{
    if (atomic_exchange(&n->view, display) != display) {
        // The demuxer re-injects the queued events in the new presentation.
        for (int i = 0; i < NANUM_INPUTS; i++)
            atomic_fetch_add(&n->in[i].gen, 1);
        wake_demux(n);
    }
}

// nanum-translate-style(-text): lines translated from now on use it, in every engine.
static void ctl_style(struct nanum *n, const struct rt_cfg *nc)
{
    mp_mutex_lock(&n->slock);
    n->style = nc->style;
    free(n->style_text);
    n->style_text = strdup(nc->style_text);
    atomic_fetch_add(&n->tr_gen, 1);
    mp_mutex_unlock(&n->slock);
    mp_info(n->log, "translation style: %s\n", style_names[nc->style]);
}

// Runs the glossary job whenever the glossary differs from the one the cues were
// translated with; a newer glossary replaces a running job.
static void ctl_gloss(struct ctl *c)
{
    struct nanum *n = c->n;
    if (c->gjob && atomic_load(&c->gjob->finished)) {
        mp_mutex_lock(&n->slock);
        bool failed = n->gl_state == GS_ERROR;
        mp_mutex_unlock(&n->slock);
        if (failed) {
            talloc_free(c->gl_fail);
            c->gl_fail = talloc_strdup(NULL, c->gjob->gl_target);
        }
        job_stop(&c->gjob);
    }
    mp_mutex_lock(&n->slock);
    bool ready = n->gl_base_set && n->tr_enabled;
    char *cur = talloc_strdup(NULL, n->glossary ? n->glossary : "");
    bool stale = ready && n->gl_base && strcmp(cur, n->gl_base);
    mp_mutex_unlock(&n->slock);
    if (ready && !stale) {
        mp_mutex_lock(&n->rlock);
        stale = n->dcues && n->dgl && strcmp(n->dgl, cur);
        mp_mutex_unlock(&n->rlock);
    }
    if (c->gjob && (!stale || strcmp(cur, c->gjob->gl_target)))
        job_stop(&c->gjob);
    if (!c->gjob && stale && !(c->gl_fail && !strcmp(cur, c->gl_fail))) {
        gl_set_state(n, GS_RUNNING, 0, 0, "");
        c->gjob = job_start(n, JOB_GLOSS, c->cur, "", 0, -1, NULL, cur);
        if (!c->gjob)
            gl_set_state(n, GS_ERROR, 0, 0, "cannot start the glossary thread");
    }
    talloc_free(cur);
}

static void ctl_save(struct ctl *c, int64_t now)
{
    struct nanum *n = c->n;
    bool all = now >= c->next_save;
    if (all)
        c->next_save = now + MP_TIME_S_TO_NS(SAVE_INTERVAL_S);
    for (int i = 0; i < NANUM_INPUTS; i++) {
        struct input *in = &n->in[i];
        if (atomic_exchange(&in->save_now, false) || all)
            persist_input(n, in);
    }
}

static MP_THREAD_VOID ctl_thread(void *p)
{
    struct nanum *n = p;
    mp_thread_set_name("nanum-ctl");

    struct ctl c = {.n = n, .cur = rt_dup(NULL, n->cfg), .sctx = talloc_new(NULL),
                    .sec_id = -1, .next_save = mp_time_ns() + MP_TIME_S_TO_NS(SAVE_INTERVAL_S)};
    bool first = true;

    char err[256] = "";
    n->lib = lib_open(n->opts->lib_db, err, sizeof(err));
    if (!n->lib) {
        mp_err(n->log, "media library unavailable: %s\n", err);
        snprintf(n->lib_open_err, sizeof(n->lib_open_err), "%s", err);
    }
    set_lib(n, LS_LOADING, "", "", false, false, "", "");

    while (1) {
        mp_mutex_lock(&n->clock);
        if (!first && !n->ctl_kick && !n->ctl_stop)
            mp_cond_timedwait(&n->ccond, &n->clock, MP_TIME_MS_TO_NS(250));
        n->ctl_kick = false;
        bool stop = n->ctl_stop;
        bool sel = n->select_pending;
        n->select_pending = false;
        void (*selcb)(void *) = n->select_cb;
        void *selctx = n->select_ctx;
        mp_mutex_unlock(&n->clock);
        if (stop)
            break;

        bool live_changed = false, dirty = first;
        if (m_config_cache_update(n->cache)) {
            struct rt_cfg *nc = rt_read(n->cache->opts);
            // Pinning the track that is already the AI input changes nothing.
            if (nc->audio_id >= 0 && nc->audio_id == atomic_load(&PRI(n)->pin_id))
                c.cur->audio_id = nc->audio_id;
            live_changed = rt_live_differs(c.cur, nc);
            dirty |= rt_scan_differs(c.cur, nc) || c.cur->lib_action != nc->lib_action;
            if (rt_style_differs(c.cur, nc))
                ctl_style(n, nc);
            ctl_apply_view(n, nc->display);
            talloc_free(c.cur);
            c.cur = nc;
        }

        if (live_changed) {
            job_stop(&c.ext);
            c.ext_loaded = false;
            // What the old input produced is saved before the new one starts.
            persist_input(n, PRI(n));
            PRI(n)->persist = false;
            bool restart = (atomic_load(&n->a_mode) == MODE_STT) !=
                           (rt_mode(c.cur) == MODE_STT) || !n->ai_enabled;
            if (restart) {
                job_stop(&c.scan);
                sec_stop(&c, &sel);
                pgs_stop(&c, &sel);
                stop_live(n);
                events_clear(PRI(n));
            }
            apply_mode(n, c.cur);
            atomic_store(&PRI(n)->bound, rt_mode(c.cur) != MODE_STT);
            if (restart)
                start_live(n);
            else
                live_restart(PRI(n));
            atomic_store(&PRI(n)->replay_audio, rt_mode(c.cur) == MODE_STT && n->ai_enabled);
            mp_info(n->log, "live input: %s\n",
                    rt_mode(c.cur) == MODE_STT ? "stt" : "sub");
            sel = true;
        }

        int64_t now = mp_time_ns();
        if (dirty) {
            // A new evaluation: what was running belongs to the old input or
            // decision. Results already saved are never touched by this.
            job_stop(&c.scan);
            talloc_free(c.sctx);
            c.sctx = talloc_new(NULL);
            c.ls = (struct libsrc){0};
            c.found = (struct lib_found){0};
            c.resolved = c.decided = false;
            c.eval_since = now;
            c.act_after = now + (first ? 0 : MP_TIME_MS_TO_NS(400));
            set_lib(n, LS_LOADING, "", "", false, false, "", "");
            set_full(n, FS_OFF, 0, "", "");
        }
        if (!c.decided)
            lib_step(&c, now, &sel);
        ctl_bind(&c, now);

        // External subtitle file as live input.
        bool want_ext = rt_mode(c.cur) == MODE_SUB_EXT && n->ai_enabled &&
                        !atomic_load(&n->gen_off);
        if (!want_ext && c.ext)
            job_stop(&c.ext);
        else if (want_ext && !c.ext && !c.ext_loaded)
            c.ext = job_start(n, JOB_EXT, c.cur, NULL, 0, -1, NULL, NULL);
        if (c.ext && atomic_load(&c.ext->finished)) {
            job_stop(&c.ext);
            c.ext_loaded = true;
        }

        ctl_titles(&c);
        ctl_profile(&c);
        ctl_secondary(&c, &sel);
        ctl_pgs(&c, &sel);
        ctl_import(&c, now);
        ctl_gloss(&c);
        ctl_save(&c, now);

        if (sel && selcb)
            selcb(selctx);

        if (c.scan && atomic_load(&c.scan->finished))
            job_stop(&c.scan);

        // The presentation or the player's timeline offset changed: the
        // finished result is rendered again from the retained cues (no network).
        render_done(n, false);
        first = false;
    }

    job_stop(&c.scan);
    job_stop(&c.ext);
    job_stop(&c.gjob);
    job_stop(&c.ijob);
    talloc_free(c.imp.ctx);
    if (c.pjob) {
        pjob_abandon(c.pjob);
        c.pjob = NULL;
    }
    for (int i = 0; i < NANUM_INPUTS; i++)
        persist_input(n, &n->in[i]);
    talloc_free(c.sctx);
    talloc_free(c.ictx);
    talloc_free(c.cur);
    talloc_free(c.gl_fail);
    MP_THREAD_RETURN();
}

// ---------------------------------------------------------------------------
// Lifecycle

void nanum_shutdown(struct nanum *n)
{
    if (!n || n->shut_down)
        return;
    n->shut_down = true;

    // Jobs first: they use the engine DLL and take no demux lock.
    if (n->cthread_started) {
        m_config_cache_set_wakeup_cb(n->cache, NULL, NULL);
        mp_mutex_lock(&n->clock);
        n->ctl_stop = true;
        mp_cond_signal(&n->ccond);
        mp_mutex_unlock(&n->clock);
        mp_thread_join(n->cthread);
        n->cthread_started = false;
    }
    // The ctl thread stopped every job and saved the results, nothing uses the
    // library anymore.
    if (n->lib) {
        lib_close(n->lib);
        n->lib = NULL;
    }

    stop_live(n);

    mp_mutex_lock(&n->lock);
    n->wakeup = NULL;
    mp_mutex_unlock(&n->lock);

}

static void nanum_destroy(void *p)
{
    struct nanum *n = p;
    nanum_shutdown(n);
    for (int i = 0; i < NANUM_INPUTS; i++) {
        mp_mutex_destroy(&n->in[i].alock);
        mp_cond_destroy(&n->in[i].acond);
        free(n->in[i].key);
    }
    mp_mutex_destroy(&n->lock);
    mp_mutex_destroy(&n->slock);
    mp_mutex_destroy(&n->rlock);
    mp_mutex_destroy(&n->clock);
    while (n->ncand)
        cand_free_locked(n, n->ncand - 1);
    mp_mutex_destroy(&n->cmutex);
    mp_cond_destroy(&n->ccond);
    talloc_free(n->dctx);
    talloc_free(n->url);
    free(n->streams);
    free(n->disc_identity);
    free(n->disc_name);
    free(n->meta_title);
    free(n->glossary);
    free(n->gl_base);
    free(n->style_text);
    for (int i = 0; i < n->ntitle_pool; i++)
        free(n->title_pool[i]);
    free(n->title_pool);
}

#ifdef _WIN32
// Minidump on unhandled exceptions. Unlike WER LocalDumps this needs no admin
// rights. Heap corruption (0xC0000374) is a fast-fail and bypasses it.
static wchar_t crash_dir[MAX_PATH];
static LPTOP_LEVEL_EXCEPTION_FILTER prev_filter;

typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    MiniDumpWriteDumpFn write = dbg ? (MiniDumpWriteDumpFn)(void *)
                                      GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
    if (write) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t path[MAX_PATH + 64];
        swprintf(path, MAX_PATH + 64, L"%ls\\mpv-%04d%02d%02d-%02d%02d%02d-%lu.dmp",
                 crash_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond, (unsigned long)GetCurrentProcessId());
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION ei = {GetCurrentThreadId(), ep, FALSE};
            write(GetCurrentProcess(), GetCurrentProcessId(), f,
                  (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
                                  MiniDumpWithThreadInfo | MiniDumpWithDataSegs),
                  &ei, NULL, NULL);
            CloseHandle(f);
            fprintf(stderr, "nanum: crash dump written to %ls\n", path);
        }
    }
    return prev_filter ? prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static void install_crash_handler(struct nanum *n)
{
    static bool done;
    if (done)
        return;
    done = true;
    wchar_t base[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!len || len >= MAX_PATH)
        return;
    swprintf(crash_dir, MAX_PATH, L"%ls\\NanumPlayer", base);
    CreateDirectoryW(crash_dir, NULL);
    swprintf(crash_dir, MAX_PATH, L"%ls\\NanumPlayer\\dumps", base);
    CreateDirectoryW(crash_dir, NULL);
    prev_filter = SetUnhandledExceptionFilter(crash_filter);
    mp_verbose(n->log, "crash dumps go to %ls\n", crash_dir);
}
#else
static void install_crash_handler(struct nanum *n) {}
#endif

static void input_init(struct nanum *n, int idx)
{
    struct input *in = &n->in[idx];
    in->n = n;
    in->idx = idx;
    in->seen_ctx = talloc_new(n);
    in->inj_pts = -INFINITY;
    in->astream = -1;
    in->sub_lang_idx = -1;
    in->last_playhead = MP_NOPTS_VALUE;
    in->dec = (struct adec){.log = n->log, .api = &n->api, .last_pkt_pts = MP_NOPTS_VALUE};
    atomic_store(&in->pin_id, -1);
    atomic_store(&in->pin_idx, -1);
    atomic_store(&in->ready_until, MP_NOPTS_VALUE);
    atomic_store(&in->engine_until, MP_NOPTS_VALUE);
    atomic_store(&in->held_pts, MP_NOPTS_VALUE);
    mp_mutex_init(&in->alock);
    mp_cond_init(&in->acond);
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
    n->global = global;
    n->opts = talloc_steal(n, opts);
    n->tr_enabled = n->opts->tr_endpoint && n->opts->tr_endpoint[0];
    atomic_store(&n->playhead, MP_NOPTS_VALUE);
    atomic_store(&n->duration, -1.0);
    atomic_store(&n->ts_offset, 0.0);
    n->lstate = LS_LOADING;
    n->glossary = strdup("");
    n->style_text = strdup("");
    snprintf(n->sec_reason, sizeof(n->sec_reason), "waiting for the media to be identified");
    n->sec_state = SS_WAITING;
    n->pgs_state = SS_OFF;
    n->imp_state = IS_OFF;
    n->imp_scale = 1.0;
    n->read_pos = MP_NOPTS_VALUE;
    PGS(n)->mark_last = -INFINITY;
    mp_mutex_init(&n->lock);
    mp_mutex_init(&n->slock);
    mp_mutex_init(&n->rlock);
    mp_mutex_init(&n->clock);
    mp_mutex_init(&n->cmutex);
    mp_cond_init(&n->ccond);
    for (int i = 0; i < NANUM_INPUTS; i++)
        input_init(n, i);
    talloc_set_destructor(n, nanum_destroy);

    n->cache = m_config_cache_alloc(n, global, &nanum_conf);
    n->cfg = talloc_steal(n, rt_read(n->cache->opts));
    atomic_store(&n->view, n->cfg->display);
    atomic_store(&n->sec_enabled, n->cfg->secondary);
    atomic_store(&n->pgs_enabled, n->cfg->pgs);
    n->style = n->cfg->style;
    free(n->style_text);
    n->style_text = strdup(n->cfg->style_text);
    apply_mode(n, n->cfg);

    if (n->opts->test_subs) {
        // Fixed test lines every 5 s for 4 h. Goes through the same entry
        // point the AI thread uses.
        for (int k = 0; k < 4 * 3600 / 5; k++) {
            int s = k * 5;
            char *t = talloc_asprintf(NULL,
                "[NanumPlayer 테스트] 자막 #%d (%02d:%02d)",
                k, s / 60, s % 60);
            nanum_add_cue(n, IN_PRI, s, 3.0, t, t);
            talloc_free(t);
        }
        mp_verbose(n->log, "test subtitles generated (%d events)\n",
                   PRI(n)->num_events);
        // Not an engine event: do not let it trigger the full scan's start.
        atomic_store(&PRI(n)->events_seen, false);
    }

    // Text translation needs neither speech models nor CUDA.
    start_live(n);

    // Follows runtime option changes and runs the full scan.
    m_config_cache_set_wakeup_cb(n->cache, opts_wakeup, n);
    if (mp_thread_create(&n->cthread, ctl_thread, n) == 0)
        n->cthread_started = true;
    else
        mp_err(n->log, "cannot start the control thread\n");

    install_crash_handler(n);
    mp_info(n->log, "enabled\n");
    return n;
}

void nanum_cursor_init(struct nanum_cursor *c, int input)
{
    *c = (struct nanum_cursor){.input = input};
    nanum_cursor_reset(c, MP_NOPTS_VALUE);
}

void nanum_cursor_reset(struct nanum_cursor *c, double from)
{
    c->from = from;
    c->last_pts = -INFINITY;
    // Retained cues keep their ReadOrder IDs. The ASS decoder must forget its
    // duplicate cache before those cues are replayed after a demuxer seek.
    c->flush_pending = from != MP_NOPTS_VALUE;
}

bool nanum_view_pending(struct nanum *n, const struct nanum_cursor *c)
{
    return atomic_load(&n->in[c->input].gen) != c->view_gen;
}

void nanum_cursor_flush(struct nanum *n, struct nanum_cursor *c, double from)
{
    c->view_gen = atomic_load(&n->in[c->input].gen);
    c->last_pts = -INFINITY;
    c->flush_pending = true;
    if (from != MP_NOPTS_VALUE)
        c->from = from;
}

double nanum_ready_until(struct nanum *n)
{
    return atomic_load(&PRI(n)->ready_until);
}

bool nanum_take_audio_replay(struct nanum *n, const struct sh_stream *sh)
{
    for (int i = 0; i < NANUM_INPUTS; i++) {
        struct input *in = &n->in[i];
        if (sh->index == atomic_load(&in->pin_idx) && atomic_load(&in->replay_audio))
            return atomic_exchange(&in->replay_audio, false);
    }
    return false;
}

void nanum_notify_eof(struct nanum *n)
{
    atomic_store(&n->eof_seen, true);
    for (int i = 0; i < NANUM_INPUTS; i++) {
        struct input *in = &n->in[i];
        if (!input_live(in))
            continue;
        mp_mutex_lock(&in->alock);
        in->aeof = true;
        mp_cond_signal(&in->acond);
        mp_mutex_unlock(&in->alock);
    }
}

void nanum_notify_seek(struct nanum *n, double pts)
{
    // Where playback restarts: the analysed ranges are judged from here.
    if (pts != MP_NOPTS_VALUE)
        atomic_store(&n->playhead, pts);
    atomic_store(&n->eof_seen, false);
    // The kept packets of the secondary candidates belong to the old position.
    mp_mutex_lock(&n->cmutex);
    while (n->ncand)
        cand_free_locked(n, n->ncand - 1);
    mp_mutex_unlock(&n->cmutex);
    bool any = false;
    n->read_pos = MP_NOPTS_VALUE;
    PGS(n)->mark_last = -INFINITY;
    for (int i = 0; i < NANUM_INPUTS; i++) {
        struct input *in = &n->in[i];
        if (!input_live(in))
            continue;
        any = true;
        mp_mutex_lock(&in->alock);
        clear_audio_queue_locked(in);
        in->aflush = true;
        in->aeof = false;       // the demuxer reports the end again if it gets there
        mp_cond_signal(&in->acond);
        mp_mutex_unlock(&in->alock);
        // Nothing is recognised at the new position yet. (The audio worker makes
        // the engine forget the old one and publishes what is analysed already, see
        // audio_thread().)
        if (is_stt_input(in))
            atomic_store(&in->ready_until, MP_NOPTS_VALUE);
    }
    if (!any)
        return;
    n->next_log_pts = 0;        // demux thread only: report the tap again
    mp_verbose(n->log, "seek: audio restarts\n");
}

// The title of a virtual stream: what the ctl thread decided, "<target> (AI)" until it
// knows the language of the input.
struct sh_stream *nanum_alloc_stream(struct nanum *n, int input)
{
    struct input *in = &n->in[input];
    struct sh_stream *sh = demux_alloc_sh_stream(STREAM_SUB);
    sh->demuxer_id = virtual_id(input);
    sh->lang = talloc_strdup(sh, "kor");
    sh->codec->codec = "ass";
    sh->codec->extradata = talloc_memdup(sh->codec, (void *)ass_header,
                                         sizeof(ass_header) - 1);
    sh->codec->extradata_size = sizeof(ass_header) - 1;
    // Published before the title is read: set_title() either sees the stream or its
    // new title is seen here.
    __atomic_store_n(&in->sh, sh, __ATOMIC_RELEASE);
    mp_mutex_lock(&n->slock);
    if (!n->titles[input]) {
        char title[96];
        make_title(n, true, "", title, sizeof(title));
        char *s = strdup(title);
        char **pool = s ? realloc(n->title_pool, (n->ntitle_pool + 1) * sizeof(*pool)) : NULL;
        if (pool) {
            n->title_pool = pool;
            pool[n->ntitle_pool++] = s;
            n->titles[input] = s;
        } else {
            free(s);
        }
    }
    __atomic_store_n(&sh->title, n->titles[input], __ATOMIC_RELEASE);
    mp_mutex_unlock(&n->slock);
    return sh;
}

struct sh_stream *nanum_pending_stream(struct nanum *n, int *input)
{
    for (int i = IN_SEC; i < NANUM_INPUTS; i++) {
        struct input *in = &n->in[i];
        if (!atomic_load(&n->stream_wanted[i]) || in->sh_requested)
            continue;
        in->sh_requested = true;
        *input = i;
        return nanum_alloc_stream(n, i);
    }
    return NULL;
}

unsigned nanum_title_gen(struct nanum *n)
{
    return atomic_load(&n->title_gen);
}

static void tap_text(struct nanum *n, const struct sh_stream *src,
                     const struct demux_packet *pkt)
{
    struct input *in = PRI(n);
    if (atomic_load(&n->a_mode) != MODE_SUB_EMB || !atomic_load(&in->athread_started) ||
        atomic_load(&n->gen_off) ||
        atomic_load(&in->ai_failed) || pkt->pts == MP_NOPTS_VALUE ||
        !nanum_wants_stream(n, src))
        return;
    if (atomic_exchange(&in->rebind_sub, false))
        in->sub_lang_idx = -1;
    if (in->sub_lang_idx != src->index) {
        in->sub_lang_idx = src->index;
        char lang[sizeof(in->alang)];
        iso2_from_mp(src->lang, lang, sizeof(lang));
        if (strcmp(lang, "auto"))
            live_queue_lang(in, lang);
    }
    char *text = packet_text(NULL, src->codec->codec, pkt);
    if (!text)
        return;
    live_queue_text(in, pkt->pts, pkt->duration > 0 ? pkt->duration : 3.0, text);
    talloc_free(text);
}

// Hands an audio packet (a->dp, owned by a) of an AI input to its worker. The first packet
// of a stream tells the worker its codec and language. Called with the demux lock held (or
// from the secondary's handover): it only queues.
static void queue_audio(struct input *in, const struct sh_stream *src, struct apkt *a)
{
    struct nanum *n = in->n;
    bool primary = in->idx == IN_PRI;
    if (atomic_exchange(&in->rebind, false))
        in->astream = -1;
    mp_mutex_lock(&in->alock);
    if (!atomic_load(&n->ai_enabled) || !atomic_load(&in->active) ||
        (primary && atomic_load(&n->a_mode) != MODE_STT)) {
        mp_mutex_unlock(&in->alock);
        talloc_free(a->dp);
        talloc_free(a);
        return;
    }
    if (src->index != in->astream) {
        in->astream = src->index;
        char lang[sizeof(in->alang)];
        iso2_from_mp(src->lang, lang, sizeof(lang));
        clear_audio_queue_locked(in);
        in->acodec_next = src->codec;
        snprintf(in->alang, sizeof(in->alang), "%s", lang);
        in->aswitch = true;
    }
    if (in->abytes + a->size > MAX_QUEUED_BYTES) {
        // The worker fell far behind: drop the backlog, the engine treats the
        // gap like a seek.
        clear_audio_queue_locked(in);
        in->aflush = true;
    }
    if (in->atail)
        in->atail->next = a;
    else
        in->ahead = a;
    in->atail = a;
    in->abytes += a->size;
    mp_cond_signal(&in->acond);
    mp_mutex_unlock(&in->alock);
}

// Copies an audio packet (this runs with the demux lock held: only copy and queue).
static struct apkt *copy_audio(const struct demux_packet *pkt)
{
    if (pkt->pts == MP_NOPTS_VALUE)
        return NULL;
    struct apkt *a = talloc_zero(NULL, struct apkt);
    a->dp = demux_copy_packet(NULL, (struct demux_packet *)pkt);
    if (!a->dp) {
        talloc_free(a);
        return NULL;
    }
    a->size = a->dp->len;
    return a;
}

static void tap_audio(struct input *in, const struct sh_stream *src,
                      const struct demux_packet *pkt)
{
    if (!atomic_load(&in->active) || atomic_load(&in->ai_failed) ||
        (in->idx == IN_PRI && atomic_load(&in->n->gen_off)))
        return;
    struct apkt *a = copy_audio(pkt);
    if (a)
        queue_audio(in, src, a);
}

// A packet of a stream that is, or may become, the secondary input: queued for it, or kept
// (bounded; when full the earliest packets stay and the rest is dropped, which leaves that
// span unanalysed) until it is decided which stream is the secondary input.
static void tap_secondary(struct nanum *n, const struct sh_stream *src,
                          const struct demux_packet *pkt)
{
    struct input *sec = SEC(n);
    mp_mutex_lock(&n->cmutex);
    if (atomic_load(&sec->active)) {
        if (src->index == atomic_load(&sec->pin_idx))
            tap_audio(sec, src, pkt);
    } else if (src->index < 64 && ((atomic_load(&n->cand_mask) >> src->index) & 1)) {
        struct cand *c = NULL;
        for (int i = 0; i < n->ncand; i++) {
            if (n->cand[i].index == src->index)
                c = &n->cand[i];
        }
        if (!c && n->ncand < MP_ARRAY_SIZE(n->cand)) {
            c = &n->cand[n->ncand++];
            *c = (struct cand){.index = src->index, .sh = src};
        }
        struct apkt *a = c && n->cand_bytes + pkt->len <= MAX_CAND_BYTES ? copy_audio(pkt)
                                                                          : NULL;
        if (a) {
            if (c->tail)
                c->tail->next = a;
            else
                c->head = a;
            c->tail = a;
            c->bytes += a->size;
            n->cand_bytes += a->size;
        }
    }
    mp_mutex_unlock(&n->cmutex);
}

// The secondary input starts on stream idx (demuxer id id): it takes over the packets kept
// for it and gets the demuxer's packets directly from now on. Returns whether the stream
// was a candidate that had been kept (then nothing since the start of the media is missing).
static bool cand_handover(struct nanum *n, int idx, int id)
{
    struct input *sec = SEC(n);
    bool had = false;
    mp_mutex_lock(&n->cmutex);
    sec->astream = -1;
    atomic_store(&sec->pin_id, id);
    atomic_store(&sec->pin_idx, idx);
    atomic_store(&sec->active, true);
    for (int i = 0; i < n->ncand; i++) {
        struct cand *c = &n->cand[i];
        if (c->index != idx)
            continue;
        had = true;
        for (struct apkt *a = c->head, *next; a; a = next) {
            next = a->next;
            a->next = NULL;
            queue_audio(sec, c->sh, a);
        }
        c->head = c->tail = NULL;
        cand_free_locked(n, i);
        break;
    }
    mp_mutex_unlock(&n->cmutex);
    // The demuxer already reached its end: the engine must analyse what it was given.
    if (had && atomic_load(&n->eof_seen)) {
        mp_mutex_lock(&sec->alock);
        sec->aeof = true;
        mp_cond_signal(&sec->acond);
        mp_mutex_unlock(&sec->alock);
    }
    return had;
}

// An image subtitle packet for the OCR worker. Called with the demux lock held: it copies and
// queues (the worker may not run yet).
static void tap_image(struct nanum *n, const struct sh_stream *src,
                      const struct demux_packet *pkt)
{
    struct input *in = PGS(n);
    if (atomic_load(&in->ai_failed))
        return;
    struct apkt *a = copy_audio(pkt);
    if (!a)
        return;
    a->img = 1;
    a->codec = src->codec;
    mp_mutex_lock(&in->alock);
    if (in->abytes + a->size > MAX_QUEUED_BYTES) {
        mp_mutex_unlock(&in->alock);
        talloc_free(a->dp);
        talloc_free(a);
        return;
    }
    if (in->atail)
        in->atail->next = a;
    else
        in->ahead = a;
    in->atail = a;
    in->abytes += a->size;
    mp_cond_signal(&in->acond);
    mp_mutex_unlock(&in->alock);
}

// The demuxer position (of any stream but the subtitles) is queued for the image subtitle
// worker once per PGS_MARK_STEP: everything before it has reached the worker, even if no
// subtitle was among it.
static void track_position(struct nanum *n, const struct demux_packet *pkt)
{
    double ts = pkt->pts != MP_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts == MP_NOPTS_VALUE)
        return;
    if (n->read_pos == MP_NOPTS_VALUE || ts > n->read_pos)
        n->read_pos = ts;
    struct input *in = PGS(n);
    if (n->read_pos < in->mark_last + PGS_MARK_STEP || atomic_load(&in->ai_failed))
        return;
    in->mark_last = n->read_pos;
    struct apkt *a = talloc_zero(NULL, struct apkt);
    a->img = 2;
    a->pts = n->read_pos;
    a->size = sizeof(*a);
    mp_mutex_lock(&in->alock);
    if (in->atail)
        in->atail->next = a;
    else
        in->ahead = a;
    in->atail = a;
    in->abytes += a->size;
    mp_cond_signal(&in->acond);
    mp_mutex_unlock(&in->alock);
}

void nanum_tap(struct nanum *n, const struct sh_stream *src,
               const struct demux_packet *pkt)
{
    if (src->type != STREAM_SUB && atomic_load(&n->pgs_wanted))
        track_position(n, pkt);
    if (src->type == STREAM_SUB) {
        if (atomic_load(&n->pgs_wanted) && src->index == atomic_load(&PGS(n)->pin_idx))
            tap_image(n, src, pkt);
        else
            tap_text(n, src, pkt);
        return;
    }
    if (src->type != STREAM_AUDIO)
        return;

    n->audio_packets++;
    // Diagnostic: exercises the crash-dump path (NANUM_CRASH_TEST=1).
    if (n->audio_packets == 3000 && getenv("NANUM_CRASH_TEST"))
        *(volatile int *)0 = 1;

    // The original-language audio track of the secondary input, or a stream that may become
    // it. It never becomes the playback input's pin.
    struct input *sec = SEC(n);
    if (src->index == atomic_load(&sec->pin_idx) ||
        (src->index < 64 && ((atomic_load(&n->cand_mask) >> src->index) & 1))) {
        tap_secondary(n, src, pkt);
        return;
    }

    struct input *pri = PRI(n);
    if (atomic_load(&n->a_mode) != MODE_STT)
        return;
    // The AI analyses the audio track it is pinned to, whichever track is
    // played. Unpinned, it follows the track that is selected first and then
    // stays on it: switching the playback track never moves the AI input. (The
    // pin also identifies the input in the library, so it is resolved even if
    // no engine is running.)
    if (atomic_load(&pri->pin_id) < 0) {
        int none = -1;
        if (atomic_compare_exchange_strong(&pri->pin_id, &none, stream_key(src))) {
            atomic_store(&n->audio_played, true);
            resolve_pin(n);
            ctl_kick(n);
        }
    }
    // Compared by index: streams can share an id, but only one of them is the
    // input (see resolve_stream_idx()).
    if (src->index != atomic_load(&pri->pin_idx))
        return;

    double ts = pkt->pts != MP_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts != MP_NOPTS_VALUE && ts >= n->next_log_pts) {
        mp_verbose(n->log, "tap: AI input audio stream %d (%s) pts=%.2f len=%zu "
                   "packets=%lld\n", src->index, src->codec->codec, ts,
                   pkt->len, (long long)n->audio_packets);
        n->next_log_pts = ts + 30;
    }
    tap_audio(pri, src, pkt);
}

static struct demux_packet *make_packet(struct demux_packet_pool *pool, int stream_index,
                                        const char *chunk, double pts, double dur)
{
    struct demux_packet *dp = new_demux_packet_from(pool, (void *)chunk, strlen(chunk));
    if (dp) {
        dp->pts = dp->dts = pts;
        dp->duration = dur;
        dp->keyframe = true;
        dp->stream = stream_index;
        dp->pos = -1;
    }
    return dp;
}

// Where cue i leaves the screen (see display_end()). Events lock held.
static double event_end(const struct input *in, int i)
{
    const struct nanum_event *e = &in->events[i];
    return display_end(e->pts, e->duration,
                       i + 1 < in->num_events ? in->events[i + 1].pts : INFINITY);
}

// A speech cue is injected once the engine can no longer produce a cue right behind it
// that would cut its extension short: it has a successor, the engine has analysed past
// its extension or has nothing left to do, or playback has reached it. The last cue of a
// batch is therefore held back for a moment instead of being injected twice.
static bool event_settled(struct nanum *n, struct input *in, int i)
{
    if (i + 1 < in->num_events)
        return true;
    const struct nanum_event *e = &in->events[i];
    double ready = atomic_load(&in->engine_until);
    if (ready != MP_NOPTS_VALUE && ready >= e->pts + shown_duration(e->duration))
        return true;
    if (atomic_load(&in->idle) || atomic_load(&in->done))
        return true;
    double ph = atomic_load(&n->playhead);
    return ph != MP_NOPTS_VALUE && ph >= e->pts - 0.25;
}

struct demux_packet *nanum_next_packet(struct nanum *n,
                                       struct demux_packet_pool *pool,
                                       int stream_index,
                                       struct nanum_cursor *c,
                                       double from, double to)
{
    struct input *in = &n->in[c->input];
    struct demux_packet *dp = NULL;

    mp_mutex_lock(&n->lock);
    int view = effective_view(n);
    unsigned gen = atomic_load(&in->gen);
    bool stt = is_stt_input(in);
    atomic_store(&in->held, false);

    // First event with pts > last injected pts.
    int lo = event_upper_bound(in, c->last_pts);

    if (c->flush_pending) {
        // The read position or presentation changed: clear the ASS decoder,
        // then inject the retained events again. Use the timestamp of the
        // first event that follows so the queue stays ordered.
        double mpts = isfinite(from) ? from : 0.0;
        for (int i = lo; i < in->num_events && in->events[i].pts < to; i++) {
            if (event_end(in, i) > from) {
                mpts = in->events[i].pts;
                break;
            }
        }
        c->flush_pending = false;
        in->inj_pts = -INFINITY;
        for (int i = 0; i < in->num_events; i++)
            in->events[i].inj_end = 0;
        dp = make_packet(pool, stream_index, NANUM_FLUSH_CHUNK, mpts, 0);
        mp_mutex_unlock(&n->lock);
        return dp;
    }

    for (int i = lo; i < in->num_events; i++) {
        struct nanum_event *ev = &in->events[i];
        if (ev->pts >= to)
            break;
        if (stt && !event_settled(n, in, i)) {
            atomic_store(&in->held_pts, ev->pts);
            atomic_store(&in->held, true);
            break;
        }
        c->last_pts = ev->pts;
        in->inj_pts = fmax(in->inj_pts, ev->pts);
        double end = event_end(in, i);
        if (end <= from)
            continue;       // already over at the start position

        void *tmp = talloc_new(NULL);
        char *text = build_ass_text(tmp, view, ev->src, ev->tr);
        if (text) {
            // Matroska-style ASS chunk, as consumed by ass_process_chunk(). The
            // ReadOrder differs per presentation generation.
            char *chunk = talloc_asprintf(tmp, "%d,0,Default,,0,0,0,,%s",
                                          ev->id + (int)(gen % 1000) * 1000000, text);
            dp = make_packet(pool, stream_index, chunk, ev->pts, end - ev->pts);
            ev->inj_end = end;
        }
        talloc_free(tmp);
        if (dp)
            break;
    }

    mp_mutex_unlock(&n->lock);
    return dp;
}
