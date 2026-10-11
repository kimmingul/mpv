/*
 * NanumPlayer: durable media library (SQLite).
 *
 * Stores, per identified media and per AI input (audio track / text subtitle),
 * the source transcript revisions and the translation revisions made from
 * them, plus job status. Cue timestamps are the container timestamps seen by
 * the scan; the timeline origin of the source is stored with each revision.
 * Schema v2 adds per-media properties (AI profile, glossary) and the partial
 * results of the live pipelines with the time ranges they analysed.
 * No credentials are ever stored.
 *
 * Media identity (lib_fingerprint_file): size + SHA-256 over file content.
 * Files up to LIB_FP_FULL_MAX bytes are hashed completely. Larger files hash
 * the first/last LIB_FP_EDGE bytes plus LIB_FP_SAMPLES evenly spaced blocks of
 * LIB_FP_BLOCK bytes, each together with its offset. Limit: a change that keeps
 * the size and lies entirely outside the sampled blocks of a larger file is not
 * detected. Streams with no readable local file or disc metadata have no
 * identity and are never served from the library.
 *
 * All functions are thread-safe (one connection, serialized by a mutex).
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation;
 * either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef MP_NANUM_LIBRARY_H
#define MP_NANUM_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIB_FP_FULL_MAX (16 * 1024 * 1024)
#define LIB_FP_EDGE     (4 * 1024 * 1024)
#define LIB_FP_BLOCK    (256 * 1024)
#define LIB_FP_SAMPLES  16

struct nanum_lib;

// One subtitle cue. tr == NULL: no translation stored; "": translation failed.
struct lib_cue {
    double pts, dur;
    char *src;
    char *tr;
};

struct lib_found {
    bool has_source, has_tr;
    int64_t source_id;          // source row
    int64_t source_rev;         // newest source revision (row id)
    int64_t tr_id;              // newest translation for the target language
    int64_t tr_source_rev;      // source revision that translation was made from
    double origin;              // timeline origin of source_rev
    int src_lines, tr_failed;
    int64_t src_created, tr_created;
    char src_lang[16];
    char src_engine[64];
    char tr_model[128], tr_engine[16], tr_target[64];
};

// path NULL/"": %LOCALAPPDATA%/NanumPlayer/library.sqlite. NULL on failure
// (err filled).
struct nanum_lib *lib_open(const char *path, char *err, size_t errsz);
void lib_close(struct nanum_lib *l);
const char *lib_error(struct nanum_lib *l);

void lib_sha256_hex(const void *data, size_t len, char out[65]);
// Content fingerprint of a local file (see above). false + err on failure.
bool lib_fingerprint_file(const char *path, char out_hex[65], int64_t *size,
                          char *err, size_t errsz);

// Returns the media row id (0 on failure).
int64_t lib_media_ensure(struct nanum_lib *l, const char *key, const char *kind,
                         const char *name, int64_t size, double duration);

bool lib_lookup(struct nanum_lib *l, int64_t media_id, const char *source_key,
                const char *target, struct lib_found *out);

// Loads the cues of a source revision; tr_id != 0 joins that translation
// (tr = "" where the translation of a cue failed). Cues are talloc children
// of ctx.
bool lib_load_cues(struct nanum_lib *l, void *ctx, int64_t source_rev, int64_t tr_id,
                   struct lib_cue **cues, size_t *n, double *origin);

// Stores a new complete source revision in one transaction. Returns the
// revision row id (0 on failure). Older revisions stay untouched.
int64_t lib_save_source(struct nanum_lib *l, int64_t media_id, const char *source_key,
                        const char *input, int track_id, const char *language,
                        const char *engine, double origin,
                        const struct lib_cue *cues, size_t n);

// Stores a new complete translation revision of source_rev in one
// transaction (cues[i].tr, "" = failed) and, once it is committed, drops source
// revisions older than source_rev that hold nothing the new one does not
// cover. Returns the translation row id (0 on failure).
int64_t lib_save_translation(struct nanum_lib *l, int64_t source_rev, const char *target,
                             const char *engine, const char *model,
                             const struct lib_cue *cues, size_t n);

// Job status rows (informational): state running|done|failed|cancelled.
int64_t lib_job_begin(struct nanum_lib *l, int64_t source_id, const char *kind);
void lib_job_end(struct nanum_lib *l, int64_t job, const char *state, const char *error);

// Per-media key/value properties (schema v2): the AI profile of the work (original
// language, work title, glossary per target language) lives here. NULL if absent.
// The value is a talloc child of ctx.
char *lib_prop_get(struct nanum_lib *l, void *ctx, int64_t media_id, const char *key);
// value == NULL removes the property.
bool lib_prop_set(struct nanum_lib *l, int64_t media_id, const char *key, const char *value);

// Time range (container timestamps) a live AI input has analysed.
struct lib_range {
    double a, b;
};

// Results of the live pipeline of one AI input of one media (schema v2): the cues it
// produced so far and the time ranges they cover. Separate from the complete
// source/translation revisions above: a live result is only valid inside its ranges.
// lib_live_save replaces everything stored under (media_id, key) in one transaction.
bool lib_live_save(struct nanum_lib *l, int64_t media_id, const char *key,
                   const struct lib_cue *cues, size_t n,
                   const struct lib_range *ranges, size_t nr);
// ok == false: the query failed (lib_error()); n == nr == 0: nothing saved. Arrays are
// talloc children of ctx.
bool lib_live_load(struct nanum_lib *l, void *ctx, int64_t media_id, const char *key,
                   struct lib_cue **cues, size_t *n, struct lib_range **ranges,
                   size_t *nr);

#endif
