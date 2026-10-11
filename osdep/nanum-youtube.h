/*
 * NanumPlayer: YouTube playback components (yt-dlp + JavaScript runtime).
 *
 * YouTube playback goes through mpv's ytdl_hook, which needs yt-dlp, and
 * yt-dlp needs an external JavaScript runtime (Deno, found next to yt-dlp.exe)
 * to solve YouTube's challenges. Both are fetched on request from their
 * official GitHub releases into %LOCALAPPDATA%\NanumPlayer\youtube\bin
 * (NANUMPLAYER_YOUTUBE_DIR replaces ...\youtube), over HTTPS (WinHTTP,
 * certificate validation always on), by one background worker. yt-dlp.exe is
 * verified against the SHA2-256SUMS of the same release, the Deno zip against
 * its published .sha256sum; nothing is installed unverified.
 *
 * Exposed to scripts as the commands
 *     nanum-youtube-refresh             rescan files, check the latest yt-dlp
 *     nanum-download-youtube-tools      install what is missing, update yt-dlp
 *     nanum-cancel-youtube-download
 * and as the property user-data/nanum/youtube (see nanum-youtube-win.c).
 * All commands return at once; the worker publishes through the core's
 * dispatch queue.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_YOUTUBE_H_
#define MP_NANUM_YOUTUBE_H_

#include <stdbool.h>

struct MPContext;

#ifdef _WIN32

// Called once by nanum_startup_defaults(). Returns the managed yt-dlp.exe path
// (a talloc string under ctx; the file may not exist yet), NULL if the folder
// cannot be determined. Nothing is created or downloaded here.
char *nanum_youtube_startup(struct MPContext *mpctx, void *ctx);

// Publishes user-data/nanum/youtube from the file system (no network) and
// starts the check for the latest yt-dlp release. With a running worker it
// only publishes.
void nanum_youtube_refresh(struct MPContext *mpctx);

// Publishes user-data/nanum/youtube from the file system only.
void nanum_youtube_publish(struct MPContext *mpctx);

// Starts the worker that installs missing components and updates yt-dlp to the
// latest release. false (error logged and published) if it cannot start,
// including "already running".
bool nanum_youtube_download(struct MPContext *mpctx);

// Cancels the running download or check, if any. Returns at once.
void nanum_youtube_cancel(struct MPContext *mpctx);

// Cancels and joins the worker. Called from mp_destroy().
void nanum_youtube_shutdown(struct MPContext *mpctx);

#else

static inline void nanum_youtube_refresh(struct MPContext *mpctx) {}
static inline void nanum_youtube_publish(struct MPContext *mpctx) {}
static inline bool nanum_youtube_download(struct MPContext *mpctx) { return false; }
static inline void nanum_youtube_cancel(struct MPContext *mpctx) {}
static inline void nanum_youtube_shutdown(struct MPContext *mpctx) {}

#endif

#endif
