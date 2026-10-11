/*
 * NanumPlayer: native Windows open dialogs, run inside the player process.
 *
 * Exposed to scripts as the commands `nanum-open-file` and `nanum-open-disc`
 * (player/command.c). Both return immediately; the dialog runs on its own
 * thread, owned by the player window, and the selection is loaded with an
 * ordinary `loadfile`, so load errors surface exactly as for any other file.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_DIALOGS_H_
#define MP_NANUM_DIALOGS_H_

#include <stdbool.h>

struct MPContext;

enum nanum_dialog {
    // Common "Open" dialog (multi-select; the first file replaces the
    // playlist, the others are appended).
    NANUM_DIALOG_OPEN_FILE,
    // Blu-ray: plays the only drive/folder holding a BDMV tree, otherwise lets
    // the user pick one.
    NANUM_DIALOG_OPEN_DISC,
};

// Returns false if the dialog could not be started or is not supported on this
// platform. Returns true when it was started or one of the dialogs is already
// open (the request is then ignored). Call from the core thread.
#ifdef _WIN32
bool nanum_dialog_start(struct MPContext *mpctx, enum nanum_dialog kind);
#else
static inline bool nanum_dialog_start(struct MPContext *mpctx, enum nanum_dialog kind)
{
    return false;
}
#endif

#endif
