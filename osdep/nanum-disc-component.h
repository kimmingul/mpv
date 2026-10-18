/*
 * NanumPlayer: the optional Blu-ray/DVD decryption component (Windows).
 *
 * The default download contains libbluray, libdvdread and libdvdnav (they read
 * and play unprotected discs) but none of the libraries that circumvent copy
 * protection (libaacs, libbdplus, libdvdcss). Those are a separate, optional,
 * pinned asset, NanumPlayer-disc-components-<version>.zip, that the user chooses
 * to install: the player downloads it (https, size and SHA-256 pinned in
 * components\catalog.json, key "disc_components") or installs it from a ZIP the
 * user saved earlier (the same pin applies). The installer offers the same as an
 * unchecked task.
 *
 * Location: %LOCALAPPDATA%\NanumPlayer\components\disc\<version>\ (the base folder
 * is NANUMPLAYER_DISC_COMPONENT_DIR when set). The folder is written only by the
 * install: the ZIP is checked against the pin, unpacked into a staging folder, every
 * file is checked against the pinned list (name, size, SHA-256) and the folder is
 * renamed into place. Nothing of it is loaded before nanum_disc_component_ready()
 * has hashed every file again, so a damaged or replaced file is never executed.
 *
 * AACS key database: the public FindVUK database is downloaded only when the
 * component is installed AND the user agreed once (nanum_disc_keys_consent(),
 * remembered in <keys folder>\keydb-consent).
 *
 * Scripts use the commands (player/command.c)
 *     nanum-disc-component-refresh
 *     nanum-disc-component-install [zip]     (no argument: download)
 *     nanum-disc-component-cancel
 *     nanum-disc-consent yes|no
 * and the property user-data/nanum/disc-component:
 *   installed       the verified component of this version is in place
 *   version, directory, size (download size), url (pinned source)
 *   status          idle | checking | downloading | verifying | extracting | done |
 *                   cancelled | error
 *   busy, received, total, error (localized text, "" unless status is error)
 *   consent         the user agreed to the key database download
 *   catalog, catalog_error   the pin could be read
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_DISC_COMPONENT_H_
#define MP_NANUM_DISC_COMPONENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

struct MPContext;
struct mp_log;

#ifdef _WIN32

// Environment of the disc libraries before anything is opened: libbluray is
// pointed at a place where no library exists, so a stray libaacs.dll in the
// application folder is never picked up. Called by nanum_startup_environment().
void nanum_disc_component_environment(const wchar_t *root);

// Remembers the application root, reads the pin from <root>\components\catalog.json.
// Nothing is created, loaded or downloaded here.
void nanum_disc_component_startup(struct MPContext *mpctx, const char *root);

// Any thread. Hashes the installed component (once per process while it is intact),
// points libbluray (LIBAACS_PATH, LIBBDPLUS_PATH) at it and loads libdvdcss so that
// libdvdread finds it. false: not installed or not intact (nothing is loaded).
// `log` (may be NULL) gets one verbose line.
bool nanum_disc_component_ready(struct mp_log *log);

// Publishes user-data/nanum/disc-component; a component folder that exists is
// verified on the worker thread first.
void nanum_disc_component_refresh(struct MPContext *mpctx);
// Starts the worker: `zip` empty = download, else install that local file. false (error
// published) if it cannot start or one is already running.
bool nanum_disc_component_install(struct MPContext *mpctx, const char *zip);
void nanum_disc_component_cancel(struct MPContext *mpctx);
// Cancels and joins the worker. Called from mp_destroy().
void nanum_disc_component_shutdown(struct MPContext *mpctx);

// Has the user agreed to the automatic key database download?
bool nanum_disc_keys_consent(void);
// Saves (or withdraws) the agreement and publishes the state. false: not saved.
bool nanum_disc_keys_set_consent(struct MPContext *mpctx, bool agreed);
// Folder of KEYDB.cfg: NANUMPLAYER_DISC_KEYS_DIR, else %APPDATA%\NanumPlayer\disc (NULL if unknown).
char *nanum_disc_keys_dir(void *ctx);

#else

static inline bool nanum_disc_component_ready(struct mp_log *log) { return false; }
static inline void nanum_disc_component_refresh(struct MPContext *mpctx) {}
static inline bool nanum_disc_component_install(struct MPContext *mpctx, const char *zip)
{
    return false;
}
static inline void nanum_disc_component_cancel(struct MPContext *mpctx) {}
static inline void nanum_disc_component_shutdown(struct MPContext *mpctx) {}
static inline bool nanum_disc_keys_set_consent(struct MPContext *mpctx, bool agreed)
{
    return false;
}

#endif

#endif
