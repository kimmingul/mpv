/*
 * NanumPlayer: shared download engine of the optional-component managers
 * (speech models: nanum-models-win.c, CUDA accelerator: nanum-accel-win.c).
 *
 * One struct nanum_dl is the state of one downloader: its cancel flag, the
 * in-flight WinHTTP request a cancel has to abort, the I/O buffer and the last
 * error text. The owner drives it from its own worker thread:
 *
 *   hash      verify an existing file against a catalog item (SHA-256)
 *   fetch     stream the pinned HTTPS URL into a .part file, size and SHA-256
 *             verified before success
 *   install   atomically move the verified .part into place
 *
 * Every function returns NANUM_DL_OK, NANUM_DL_CANCEL (cancel requested) or
 * NANUM_DL_FAIL (nanum_dl.err holds the message). Nothing here talks to the
 * player: progress is reported through the owner's callbacks, which run on the
 * worker thread.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_DOWNLOAD_H_
#define MP_NANUM_DOWNLOAD_H_

#ifdef _WIN32
#include "common/common.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <windows.h>
#include <winhttp.h>

#include "mpv/client.h"
#include "nanum/i18n.h"
#include "osdep/threads.h"

#define NANUM_DL_MAX_URL 1024
#define NANUM_DL_IO_BUF (1024 * 1024)

// One downloadable file as described by a catalog.
struct nanum_dl_item {
    bool valid;
    char label[160];
    char file[128];
    int64_t size;
    char sha[65];       // lowercase hex
    char url[NANUM_DL_MAX_URL];
};

enum nanum_dl_res { NANUM_DL_OK, NANUM_DL_CANCEL, NANUM_DL_FAIL };
enum nanum_dl_phase { NANUM_DL_CHECKING, NANUM_DL_DOWNLOADING, NANUM_DL_VERIFYING };

struct nanum_dl_fst {
    bool exists;
    int64_t size;
    uint64_t mtime;
};

struct nanum_dl {
    // Set by the owner (nanum_dl_init) and the owner only.
    void *user;
    int tag;                    // the owner's id of the item being processed
    void (*on_phase)(struct nanum_dl *dl, enum nanum_dl_phase phase,
                     int64_t recv, int64_t total);
    void (*on_progress)(struct nanum_dl *dl, int64_t recv);   // may be throttled

    // Private.
    atomic_bool cancel;
    mp_mutex lock;              // guards req
    HINTERNET req;
    uint8_t *buf;               // NANUM_DL_IO_BUF bytes while a job runs
    char err[640];              // message of the last NANUM_DL_FAIL
};

void nanum_dl_init(struct nanum_dl *dl, void *user,
                   void (*on_phase)(struct nanum_dl *, enum nanum_dl_phase, int64_t, int64_t),
                   void (*on_progress)(struct nanum_dl *, int64_t));

// Starts a job: clears cancel and the error, allocates the I/O buffer.
// false: out of memory.
bool nanum_dl_begin(struct nanum_dl *dl);
// Ends a job: releases the I/O buffer.
void nanum_dl_end(struct nanum_dl *dl);

// Any thread: requests cancellation and aborts the blocking request.
void nanum_dl_abort(struct nanum_dl *dl);
bool nanum_dl_cancelled(struct nanum_dl *dl);

// Hashes `path`, reporting NANUM_DL_VERIFYING. NANUM_DL_OK: *match tells whether
// size and digest equal the item.
enum nanum_dl_res nanum_dl_hash(struct nanum_dl *dl, const struct nanum_dl_item *item,
                                const char *path, bool *match);

// Creates the folder of `final_path` and checks that `needed` bytes (plus a
// headroom) are free there.
bool nanum_dl_prepare_dir(struct nanum_dl *dl, const char *final_path, int64_t needed,
                          const char *what);

// Downloads item->url into `part` (created exclusively, caller deletes it on
// failure). On NANUM_DL_OK the file is complete and verified.
enum nanum_dl_res nanum_dl_fetch(struct nanum_dl *dl, const struct nanum_dl_item *item,
                                 const char *part);

// Small HTTPS GET (release metadata, checksum files; same TLS/redirect policy as
// fetch). The body, at most `max` bytes, is returned as a NUL-terminated talloc
// string under ctx. Needs a running job (nanum_dl_begin).
enum nanum_dl_res nanum_dl_get_text(struct nanum_dl *dl, const char *url, int64_t max,
                                    void *ctx, char **out);

// Receives the body of nanum_dl_stream_unpinned in order, in chunks of at most
// NANUM_DL_IO_BUF bytes. false aborts the transfer (the sink records its
// message with nanum_dl_fail; without one a generic message is set).
typedef bool (*nanum_dl_sink_fn)(void *user, const void *data, size_t size);

// HTTPS GET of dynamic data that has NO pinned size or digest (for example a
// public key database that changes every day). Same TLS/redirect/timeout/cancel
// policy as fetch, but because nothing authenticates the content the caller
// MUST validate it itself; the body is bounded by `max` bytes (a larger
// Content-Length or body fails) and handed to `sink` without being kept in
// memory. Pinned assets MUST keep using nanum_dl_fetch. *received (optional)
// gets the body size on NANUM_DL_OK. Needs a running job (nanum_dl_begin).
enum nanum_dl_res nanum_dl_stream_unpinned(struct nanum_dl *dl, const char *url, int64_t max,
                                           nanum_dl_sink_fn sink, void *sink_user,
                                           int64_t *received);

// Moves the verified `part` to `final_path`. An existing file there is replaced
// only if it is not the verified item (a valid file that another process
// installed meanwhile is kept). `seen` is what the caller saw at `final_path`
// before downloading (may be NULL).
enum nanum_dl_res nanum_dl_install(struct nanum_dl *dl, const struct nanum_dl_item *item,
                                   const char *final_path, const char *part,
                                   const struct nanum_dl_fst *seen);

// Records a failure message (printf style) in dl->err.
void nanum_dl_fail(struct nanum_dl *dl, const char *fmt, ...) MP_PRINTF_ATTRIBUTE(2, 3);

// ---- Small shared helpers --------------------------------------------------

bool nanum_dl_stat(const char *path, struct nanum_dl_fst *out);
// Message for a Win32 / WinHTTP error code.
void nanum_dl_error_text(char *out, size_t sz, DWORD code);
void nanum_dl_trim_slashes(char *s);
bool nanum_dl_same_path(const char *a, const char *b);
char *nanum_dl_join(void *ctx, const char *a, const char *b);

// Whole text file (<= 1 MiB) as a talloc string under ctx, NULL on failure.
char *nanum_dl_read_text(void *ctx, const char *path);
const char *nanum_dl_node_str(mpv_node *map, const char *key);
bool nanum_dl_node_int(mpv_node *map, const char *key, int64_t *out);

// Validates one catalog entry (filename, sha256, https url, size) into *out.
// false (message in err): the entry is unusable.
// Bundled entries must have no URL; other entries require HTTPS.
bool nanum_dl_parse_item(mpv_node *entry, const char *fallback_label, bool bundled,
                            struct nanum_dl_item *out, char *err, size_t errsz);

#endif

#endif
