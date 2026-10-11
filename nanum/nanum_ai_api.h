/*
 * Function-pointer view of nanum_ai.dll (see nanum_ai/nanum_ai.h in the
 * NanumPlayer repository, which is the authoritative definition).
 * The DLL is loaded at runtime because it is built with a different
 * toolchain (MSVC + CUDA) than mpv (clang/MinGW).
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation;
 * either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef MP_NANUM_AI_API_H
#define MP_NANUM_AI_API_H

#include <stddef.h>

#define NANUM_AI_ABI_VERSION 7

struct nanum_ai;

struct nanum_ai_config {
    int abi_version;
    const char *stt_model_path;
    const char *vad_model_path;
    const char *stt_model_multilingual_path;
    const char *language;
    int use_gpu;
    int n_threads;
    const char *translate_endpoint;
    const char *translate_model;
    const char *target_language;
    int translate_timeout_ms;
    int translate_context_lines;
    const char *cache_path;
    const char *translate_engine;   // translate_all(): "ollama" (NULL/"" = ollama) or "omp"
    const char *omp_model;          // omp --model selector
    const char *omp_command;        // omp executable, NULL/"" -> "omp"
    int scan_only;                  // 1: lines are only collected into the transcript
    int translate_only;             // 1: no speech recognition (stt/vad may be NULL); independent of scan_only
    const char *translate_style;    // natural (NULL/"") | polite | casual | literal | custom
    const char *translate_style_text; // instruction for "custom"
    const char *glossary;           // initial glossary, lines "source => target"
};

typedef void (*nanum_ai_event_cb)(void *user, double pts, double duration,
                                  const char *source, const char *translation);

struct nanum_ai_line {
    double pts;
    double duration;
    const char *source;
    const char *translation;
};

// progress in [0,1] while running (lines == NULL, n == 0); exactly one final
// call with all lines (error == NULL) or error != NULL. Internal thread; the
// pointers are valid only during the call.
typedef void (*nanum_ai_full_cb)(void *user, double progress,
                                 const struct nanum_ai_line *lines, size_t n,
                                 const char *error);

// One snapshot of the collected transcript (sorted by pts, translation NULL);
// pointers valid only during the call.
typedef void (*nanum_ai_transcript_cb)(void *user, const struct nanum_ai_line *lines,
                                       size_t n);

struct nanum_ai_api {
    struct nanum_ai *(*create)(const struct nanum_ai_config *cfg,
                               nanum_ai_event_cb cb, void *user);
    void (*destroy)(struct nanum_ai *ai);
    void (*push_audio)(struct nanum_ai *ai, const float *samples, size_t n,
                       double start_pts);
    void (*flush)(struct nanum_ai *ai);
    void (*reset)(struct nanum_ai *ai);
    void (*set_language)(struct nanum_ai *ai, const char *language);
    double (*processed_until)(struct nanum_ai *ai);
    const char *(*last_error)(void);
    int (*wait_idle)(struct nanum_ai *ai, int timeout_ms);
    void (*push_text)(struct nanum_ai *ai, double pts, double duration,
                      const char *text);
    void (*set_playhead)(struct nanum_ai *ai, double pts);
    int (*translate_all)(struct nanum_ai *ai, nanum_ai_full_cb cb, void *user);
    void (*cancel_full)(struct nanum_ai *ai);
    size_t (*transcript_size)(struct nanum_ai *ai);
    size_t (*transcript_export)(struct nanum_ai *ai, nanum_ai_transcript_cb cb, void *user);
    void (*set_glossary)(struct nanum_ai *ai, const char *text);
    void (*set_style)(struct nanum_ai *ai, const char *style, const char *text);
    size_t (*get_glossary)(struct nanum_ai *ai, char *out, size_t outsz);
    int (*profile)(const struct nanum_ai_config *cfg, const char *title,
                   const char *audio_languages, char *out, size_t outsz);
    // ABI 7: OCR of image subtitles (Windows.Media.Ocr).
    int (*ocr)(const unsigned char *gray, int width, int height, int stride,
               const char *lang, char *out, size_t outsz);
    size_t (*ocr_languages)(char *out, size_t outsz);
};

#endif
