/*
 * NanumPlayer: native application startup (Windows).
 *
 * NanumPlayer.exe is the modified mpv itself. Everything the old launcher did
 * (config dir, engine/model paths, disc libraries, auto-loaded scripts) is
 * done here, in process.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_STARTUP_H_
#define MP_NANUM_STARTUP_H_

#include <stdbool.h>

struct MPContext;

#ifdef _WIN32

// Very first call from main(): process environment only (no mpv state yet).
// Pins libbluray to the independent libraries in <app>\disc. No MakeMKV
// discovery, console process, or legacy renamed libmmbd fallback is used.
void nanum_startup_environment(void);

// Called by mp_initialize() for the CLI player after the paths are known and
// before any config file is read. Installs the NanumPlayer option defaults, so
// precedence is: these defaults < mpv.conf / profiles < command line.
// Also creates the default mpv.conf in the config dir on first run.
void nanum_startup_defaults(struct MPContext *mpctx);

// Loads scripts/nanum.lua and scripts/nanum-fluent.lua from the application
// root, independent of --no-config / --load-scripts. A script that the user
// already passed explicitly with --script is not loaded twice.
void nanum_startup_scripts(struct MPContext *mpctx);

#else

static inline void nanum_startup_environment(void) {}
static inline void nanum_startup_defaults(struct MPContext *mpctx) {}
static inline void nanum_startup_scripts(struct MPContext *mpctx) {}

#endif

#endif
