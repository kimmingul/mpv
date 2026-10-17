/*
 * NanumPlayer: nothing about what was played is kept (Windows).
 *
 * Playback never writes history. The application log, the translation cache,
 * the subtitle library, crash dumps and the disk shader/ICC caches are off
 * unless the user switches them on explicitly (--log-file, --nanum-cache,
 * --nanum-library-db, --nanum-full-dir, ...). What the AI engine has to put on
 * disk to hand a subtitle file to the player lives in a transient folder of
 * this process (%TEMP%\NanumPlayer-session-<pid>-<start time>): it is emptied
 * whenever a media ends, removed when the process exits and swept at the next
 * start if the process was killed. The operating system may still page memory
 * or leave deleted files recoverable; that is outside what this can prevent.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_PRIVATE_H_
#define MP_NANUM_PRIVATE_H_

#include <stdbool.h>

#ifdef _WIN32

// The transient folder of this process (UTF-8, '/' separators, a talloc string
// under ctx), created on first use. NULL if it cannot be created. Thread safe.
char *nanum_private_dir(void *ctx);

// Deletes what the folder holds (files still open stay until the process
// exits). Thread safe, best effort.
void nanum_private_clear(void);

// Removes the folders of processes that are gone (killed, crashed).
void nanum_private_sweep(void);

// What earlier versions kept by default. settings_dir: the folder of
// mpv.conf (UTF-8, may be NULL). Each keep_* says the user chose a place of
// their own for that kind of data, so the default place is not touched:
// nothing of it is then ours to delete.
//   <settings>\last-playback*.log (keep_log), <settings>\tr_cache.jsonl (keep_cache)
//   %LOCALAPPDATA%\NanumPlayer\library.sqlite* (keep_library), full\ (keep_full),
//   cache\, dumps\
// Explicit files and everything else (models, accelerators, disc keys, settings,
// the YouTube component folder) are never touched.
void nanum_private_purge_legacy(const char *settings_dir, bool keep_library,
                                bool keep_full, bool keep_cache, bool keep_log);

#else

static inline char *nanum_private_dir(void *ctx) { return NULL; }
static inline void nanum_private_clear(void) {}
static inline void nanum_private_sweep(void) {}
static inline void nanum_private_purge_legacy(const char *settings_dir, bool keep_library,
                                              bool keep_full, bool keep_cache,
                                              bool keep_log) {}

#endif

#endif
