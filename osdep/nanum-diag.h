/*
 * NanumPlayer: explicit disc diagnostics (Windows).
 *
 * Nothing is logged by default: the player has no application log (see
 * nanum-private.h). Diagnostic mode (explicit, per session; only disc I/O is
 * recorded, never a web address, cookie or header) writes timestamps, every seek
 * request/result, the first reads after each reposition, periodic read
 * statistics, the environment (libbluray, AACS/BD+ library versions and paths,
 * drive model/firmware) and, after a read failure, the Windows System event log
 * entries of the storage stack, to a file
 *     %APPDATA%\NanumPlayer\diagnostics\disc-<date>-<time>-<pid>.log
 * of at most 8 MiB, the newest 8 of which are kept. Video payload, keys and
 * authentication data are never written.
 *
 * Exposed to scripts as the command
 *     nanum-diagnostics [toggle|on|off]
 * and as the property user-data/nanum/diagnostics (a map):
 *     enabled        bool    diagnostic mode is on
 *     file           string  diagnostic log of this session ("" until first enabled)
 *     directory      string  folder of the diagnostic logs
 *     max_bytes      int     size limit of one diagnostic log
 *     keep           int     diagnostic logs kept
 *     slow_read_ms   int     reads slower than this are recorded
 *     slow_seek_ms   int     seeks slower than this are recorded
 * NANUMPLAYER_DIAGNOSTICS=1 in the environment starts the session with
 * diagnostics on.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_DIAG_H_
#define MP_NANUM_DIAG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct MPContext;
struct mp_log;

enum nanum_diag_mode {
    NANUM_DIAG_OFF,
    NANUM_DIAG_ON,
    NANUM_DIAG_TOGGLE,
};

// Normal mode records reads/seeks slower than these; diagnostic mode records
// every seek and reads slower than the second values.
#define NANUM_DIAG_SLOW_READ_MS       2000
#define NANUM_DIAG_SLOW_READ_MS_DIAG  250
#define NANUM_DIAG_SLOW_SEEK_MS       3000

#ifdef _WIN32

// Called once by nanum_startup_defaults() with the settings directory (UTF-8,
// may be NULL): where the diagnostic logs go if they are switched on. Starts
// the session with diagnostics on if NANUMPLAYER_DIAGNOSTICS is set.
void nanum_diag_startup(struct MPContext *mpctx, const char *settings_dir);

// Publishes user-data/nanum/diagnostics (core thread).
void nanum_diag_publish(struct MPContext *mpctx);

// The command. Returns false if the log cannot be created (the mode is then
// unchanged and the reason is logged). Core thread.
bool nanum_diag_command(struct MPContext *mpctx, enum nanum_diag_mode mode);

// Any thread; cheap.
bool nanum_diag_enabled(void);

// Appends one timestamped line to the diagnostic log (no-op while diagnostics
// are off, or after the size limit; important lines may use the reserve above
// the soft limit). Any thread. The line is cut at 1000 bytes; no newline needed.
void nanum_diag_log(bool important, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Mirrors libbluray disc I/O messages into the diagnostic log. Any thread.
void nanum_diag_libbluray_message(const char *msg);

// Session start of a Blu-ray stream (after bd_open succeeded): one concise
// info line into the application log; the details into the diagnostic log.
void nanum_diag_describe_bluray(struct mp_log *log, const char *device,
                                const char *libbluray_version);

// A disc read failed: if diagnostics are on, collect (at most every 30 s, on a
// short-lived thread) the System event log entries of the storage stack from
// the last two minutes into the diagnostic log.
void nanum_diag_collect_failure_context(void);

// Windows error code -> short UTF-8 text without trailing newline.
void nanum_diag_os_error_text(unsigned long code, char *out, size_t size);

// Local wall-clock "YYYY-MM-DD HH:MM:SS.mmm" into out (>= 32 bytes).
void nanum_diag_wall_clock(char *out, size_t size);

#else

static inline void nanum_diag_startup(struct MPContext *mpctx, const char *settings_dir) {}
static inline void nanum_diag_publish(struct MPContext *mpctx) {}
static inline bool nanum_diag_command(struct MPContext *mpctx, enum nanum_diag_mode mode)
{
    return false;
}
static inline bool nanum_diag_enabled(void) { return false; }
#define nanum_diag_log(...) ((void)0)
static inline void nanum_diag_libbluray_message(const char *msg) {}
static inline void nanum_diag_describe_bluray(struct mp_log *log, const char *device,
                                              const char *libbluray_version) {}
static inline void nanum_diag_collect_failure_context(void) {}
static inline void nanum_diag_os_error_text(unsigned long code, char *out, size_t size)
{
    if (size)
        out[0] = 0;
}
static inline void nanum_diag_wall_clock(char *out, size_t size)
{
    if (size)
        out[0] = 0;
}

#endif

#endif
