/*
 * NanumPlayer: libbluray file access with Win32 error capture (Windows).
 *
 * libbluray reads the disc through a replaceable file-open function
 * (bd_register_file(), the process-wide hook libbluray offers for this; unlike
 * bd_open_files() it keeps bd_open(device path) and so the device path that
 * libaacs/libmmbd (MakeMKV) use for decryption). The default implementation is
 * fread(), which reports a failure only as a short count: the OS error is lost
 * before libbluray can log "Read unit at N failed".
 *
 * The functions installed here are a transparent replacement of that default
 * for files opened for reading: same share mode, same UTF-8 file names, same
 * return conventions (read returns the byte count, 0 or less than requested on
 * EOF/error; seek 0/-1). They add: the Win32 error code taken right at the
 * failing ReadFile, exact offsets and elapsed times, EOF tracking, and the
 * bookkeeping behind the failure/slow-read records of the application log and
 * the per-read records of the diagnostic log (osdep/nanum-diag.h). Nothing is
 * retried, skipped or suppressed. Files opened for writing are passed to
 * libbluray's default function.
 *
 * This file is part of mpv, distributed under the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 */

#ifndef MP_BLURAY_FILE_WIN32_H_
#define MP_BLURAY_FILE_WIN32_H_

#include <stdbool.h>
#include <stdint.h>

struct mp_log;
struct bd_io;

struct bd_io_info {
    // The stream file (.m2ts) libbluray is reading.
    char file[24];                  // "" if none
    int64_t file_pos;               // offset of its next read
    int64_t file_size;
    uint64_t reads, bytes;          // session totals, stream files
    uint64_t failures;              // reads that failed (OS error or short)
    uint64_t slow;                  // reads slower than the threshold
    // The most recent failed read.
    uint64_t failure_serial;        // 0: none so far
    char failure_file[24];
    int64_t failure_offset;         // file offset of the failing ReadFile
    int64_t failure_length;         // requested
    int64_t failure_got;            // delivered before failing
    unsigned long failure_os_error; // GetLastError() at the failing ReadFile; 0: none
    double failure_time;            // mp_time_sec() when it returned
    int64_t failure_ms;             // how long it blocked
};

#ifdef _WIN32

// Installs the file hook (reference counted: several streams may exist, e.g. an
// ISO probe). `log` is the stream's log; it must outlive bd_io_end().
struct bd_io *bd_io_begin(struct mp_log *log);

// Writes the session summary and removes the hook when it was the last session.
void bd_io_end(struct bd_io *io);

void bd_io_info(struct bd_io *io, struct bd_io_info *out);

// A seek request with this id was issued: the next reads of the stream file are
// recorded in the diagnostic log, tagged with the id. id 0 clears the tag.
void bd_io_mark_reposition(struct bd_io *io, uint64_t seek_id);

#else

static inline struct bd_io *bd_io_begin(struct mp_log *log) { return 0; }
static inline void bd_io_end(struct bd_io *io) {}
static inline void bd_io_info(struct bd_io *io, struct bd_io_info *out)
{
    *out = (struct bd_io_info){0};
}
static inline void bd_io_mark_reposition(struct bd_io *io, uint64_t seek_id) {}

#endif

#endif
