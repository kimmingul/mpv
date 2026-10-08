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

#define NANUM_AI_ABI_VERSION 2

struct nanum_ai;

struct nanum_ai_config {
    int abi_version;
    const char *stt_model_path;
    const char *vad_model_path;
    const char *language;
    int use_gpu;
    int n_threads;
    const char *translate_endpoint;
    const char *translate_model;
    const char *target_language;
    int translate_timeout_ms;
    int translate_context_lines;
    const char *cache_path;
};

typedef void (*nanum_ai_event_cb)(void *user, double pts, double duration,
                                  const char *source, const char *translation);

struct nanum_ai_api {
    struct nanum_ai *(*create)(const struct nanum_ai_config *cfg,
                               nanum_ai_event_cb cb, void *user);
    void (*destroy)(struct nanum_ai *ai);
    void (*push_audio)(struct nanum_ai *ai, const float *samples, size_t n,
                       double start_pts);
    void (*flush)(struct nanum_ai *ai);
    void (*reset)(struct nanum_ai *ai);
    const char *(*last_error)(void);
};

#endif
