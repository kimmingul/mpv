/*
 * NanumPlayer: opening media from outside the player (Windows).
 *
 *  - Default IPC pipe. The first NanumPlayer.exe listens on a per-user pipe
 *    (\\.\pipe\nanumplayer-<user>, NANUMPLAYER_IPC_PIPE replaces the name), unless the user
 *    passes --input-ipc-server. The YouTube window and the protocol handler talk to
 *    the running player through it.
 *  - nanumplayer://open?url=<percent-encoded http(s) url> as the argument of
 *    NanumPlayer.exe (the registered URL protocol of the app). If a player is already
 *    running the url is handed to it ({"command":["script-message","nanum-youtube-open",
 *    "<url>"]}) and this process exits; else the url is played like any other argument.
 *  - Per-file preparation of YouTube urls (login cookies for yt-dlp, the origin url the
 *    library identifies the video by).
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_OPEN_H_
#define MP_NANUM_OPEN_H_

#include <stdbool.h>

struct MPContext;

#ifdef _WIN32

// Before anything else in main(). Replaces a nanumplayer:// argument by the url it
// carries. Returns 0 to go on starting, 1 if the url was handed to the running player (exit
// with 0), 2 if the argument is not a valid nanumplayer:// request (exit with 1).
int nanum_open_forward(char **options);

// Called by mp_initialize() right after the NanumPlayer defaults: format preference for
// YouTube, the default IPC pipe, and the registration of the nanumplayer:// protocol of this
// per-user installation (HKCU) if it is not registered or points to a missing file.
void nanum_open_startup(struct MPContext *mpctx);

// Called for every file right before the on_load hooks (ytdl_hook) run, after the
// per-file options were applied: sets file-local options for YouTube urls.
void nanum_open_prepare_file(struct MPContext *mpctx, const char *url);

#else

static inline int nanum_open_forward(char **options) { return 0; }
static inline void nanum_open_startup(struct MPContext *mpctx) {}
static inline void nanum_open_prepare_file(struct MPContext *mpctx, const char *url) {}

#endif

#endif
