/*
 * NanumPlayer: native disc listing and lossless MKV remux (no external ripper).
 *
 * Commands (player/command.c; both run on a command worker thread and are
 * abortable, e.g. mp.abort_async_command()):
 *
 *   nanum-disc-list [device]
 *       No device: lists the optical drives. A device ("E:" or a BDMV/VIDEO_TS
 *       folder): opens the disc (AACS/BD+ handled through nanum_disc_bd_open())
 *       and lists its titles.
 *
 *   nanum-disc-rip <device> <title-id|all> <destination> [expected-disc-id]
 *       Remuxes the title(s) without re-encoding into Matroska files in
 *       <destination>, using the player's own disc demuxer (demux_disc) so the
 *       multi-clip timeline, TrueHD+core, PGS and track languages match what
 *       playback shows. Existing files are never overwritten; each file is
 *       written to a unique ".part" file and renamed atomically when complete.
 *
 * Both return one structured result map (schema in nanum-disc.c). Failure,
 * cancellation and unsupported media are reported in the result (ok, status,
 * error, error_code), never as success. While a disc operation runs, its
 * progress is published as user-data/nanum/rip.
 *
 * Only one disc operation runs at a time; another one is rejected with
 * error_code "busy" and does not touch the published state.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_DISC_H_
#define MP_NANUM_DISC_H_

#include <stdbool.h>

struct mp_cmd_ctx;

// Both are command handlers for commands defined with .spawn_thread = true and
// .can_abort = true. Called with the core locked; they unlock it while they
// work and return with it locked again, having filled cmd->result.
// Strings may be NULL or empty where documented. cmd->success stays true
// whenever a result map was produced (check result.ok). Outside Windows the
// commands fail (cmd->success = false).
void nanum_disc_list(struct mp_cmd_ctx *cmd, const char *device);
void nanum_disc_rip(struct mp_cmd_ctx *cmd, const char *device, const char *title,
                    const char *destination, const char *expected_disc_id);

#endif
