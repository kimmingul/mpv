/*
 * NanumPlayer: the application version. The one place to change it for a release.
 *
 * Used by osdep/mpv.rc (file and product version of NanumPlayer.exe), the startup log and
 * the property user-data/nanum/app-version (osdep/nanum-startup-win.c), and read by
 * tools/package.py (default release name). Only #define lines: the resource compiler
 * includes this file too.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_VERSION_H_
#define MP_NANUM_VERSION_H_

#define NANUM_VERSION_MAJOR 1
#define NANUM_VERSION_MINOR 13
#define NANUM_VERSION_PATCH 3

#define NANUM_VERSION "1.13.3"

#endif
