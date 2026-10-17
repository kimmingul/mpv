/*
 * NanumPlayer: opening of Blu-ray discs with automatic AACS key handling.
 *
 * nanum_disc_bd_open() replaces bd_open() for every consumer of libbluray
 * (playback in stream/stream_bluray.c, ripping). It does not decrypt anything
 * itself: decryption is libbluray's, through the independent libaacs/libbdplus
 * the player ships (LIBAACS_PATH/LIBBDPLUS_PATH, see osdep/nanum-startup-win.c).
 * What it adds is the key database libaacs needs, so that a cold profile plays
 * an encrypted disc with nothing prepared by the user:
 *
 *   1. bd_open(device, %APPDATA%\NanumPlayer\disc\KEYDB.cfg). A disc without
 *      AACS and BD+ never touches the network, and neither does a disc the
 *      cached database already opens.
 *   2. AACS detected but not handled for a reason a database can cure (no
 *      file, no processing key, no host certificate, revoked certificate, no
 *      device key): the public FindVUK database
 *          https://fvonline-db.bplaced.net/fv_download.php?lang=eng
 *      is downloaded (HTTPS ZIP holding only keydb.cfg), verified and
 *      atomically published as KEYDB.cfg, and the disc is opened again. An
 *      existing KEYDB.cfg younger than NANUM_DISC_KEYS_MIN_REFRESH_S is not
 *      downloaded again, so a disc the database does not know costs one
 *      download per hour, not one per attempt.
 *   3. Anything still unhandled is an explicit failure (NULL). Nothing is
 *      prompted, no other decryptor is tried, no keys are built in.
 *
 * The key database is data that nothing authenticates but the TLS connection,
 * so it is bounded (archive <= 64 MiB, keydb.cfg <= 128 MiB), unpacked by a
 * strict single-entry ZIP reader (no extraction paths, no encryption, no
 * zip64, no multi-volume), checked against the CRC-32 and length of the archive
 * and for plausible content, and replaces the previous cache only when all of
 * that holds. Key values are never logged. The previous cache survives every
 * failure. Concurrent openers (several players, playback plus rip) serialize
 * the refresh on a named mutex and never see a half-written file.
 *
 * Environment: NANUMPLAYER_DISC_KEYS_DIR replaces %APPDATA%\NanumPlayer\disc
 * (the folder of KEYDB.cfg).
 *
 * Failure messages (log level error, one per line; the UI classifies them):
 *   "AACS encryption detected but cannot find libaacs."   backend missing
 *   "BD+ encryption detected but cannot find libbdplus."  backend missing
 *   "AACS key database unavailable: <reason>"             download/verify failed
 *   "AACS error: <libaacs reason>"                        final, disc unsupported
 *       followed by "No keys for this disc: ..." when the database was current
 *   "Cannot decrypt BD+ encryption."                      BD+ is not supported
 *   "Couldn't open Blu-ray device: <device>"              not a readable disc
 * A cancelled open returns NULL silently (mp_cancel_test(cancel) is true).
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_DISC_KEYS_H_
#define MP_NANUM_DISC_KEYS_H_

#include "config.h"

#if HAVE_LIBBLURAY

#include <libbluray/bluray.h>

struct mp_log;
struct mp_cancel;

// A cached key database older than this is downloaded again when a disc it
// cannot open needs keys.
#define NANUM_DISC_KEYS_MIN_REFRESH_S (60 * 60)

// Opens `device` (drive letter, folder or image, UTF-8) and returns a disc
// whose AACS and BD+ protection, if any, is handled; see above. The result
// may still be a non-Blu-ray (bd_get_disc_info()->bluray_detected == 0, nothing
// to decrypt): the caller rejects that as before. NULL on error or when
// `cancel` (may be NULL) triggers; the error is logged to `log` (non-NULL).
// May block for a download; callable from any thread; `cancel` must outlive
// the call and its own callback is never touched.
#ifdef _WIN32
BLURAY *nanum_disc_bd_open(const char *device, struct mp_log *log, struct mp_cancel *cancel);
#else
static inline BLURAY *nanum_disc_bd_open(const char *device, struct mp_log *log,
                                         struct mp_cancel *cancel)
{
    // Other platforms use the system libaacs and its own configuration.
    return bd_open(device, NULL);
}
#endif

#endif

#endif
