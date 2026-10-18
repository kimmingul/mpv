/*
 * NanumPlayer: playback-free generation of subtitle files (SRT) for a file or a disc title.
 *
 * Command (player/command.c; runs on a command worker thread and is abortable, e.g. with
 * mp.abort_async_command()):
 *
 *   nanum-generate <spec>
 *       spec is one JSON object:
 *         url          file path, URL, or disc title: "bd://longest/E:" / "bd://mpls/00800/E:"
 *         source       "stt" (speech recognition of an audio track), "text" (a text subtitle
 *                      track or subtitle file) or "ocr" (an image subtitle track, PGS/DVD, read
 *                      by the Windows OCR)
 *         track        demuxer id (track-list "src-id") of the track, omitted/-1: choose
 *         source_lang  ISO 639-1 of the source (omitted: the track's language)
 *         targets      [{"code":"ko","name":"Korean"}, ...] languages to translate into; a
 *                      target the source already is in gets no translation
 *         write_source true (default): keep the source-language file as well
 *         directory    where the files go; omitted: next to the file (local files only)
 *         name         file name stem; omitted: the media's file name
 *         title        what the work is called (the translator looks it up for a glossary)
 *         limit        seconds: only the first part of the media (a preview); omitted/0: all
 *         glossary     lines "source => target" to translate with instead
 *       Nothing is rendered or played: the job opens a demuxer of its own, reads the one stream
 *       it needs as fast as the source delivers it, and runs the AI engine of the player
 *       (nanum-* options) on it. The player keeps playing whatever it plays.
 *
 * Files are <name>.<language>.srt in <directory>. Nothing is overwritten: a name that is taken
 * gets ".2", ".3", ... before the extension. Each file is written to a ".part" file next to it
 * and renamed when complete, so a cancelled or failed job never leaves a half file. The source
 * file is written before the translations start; cancelling keeps whatever was finished.
 *
 * Result (a map): ok, status ("done"|"cancelled"|"error"), error, error_code ("busy", "bad-args",
 * "bad-destination", "open", "no-track", "engine", "no-model", "stt", "ocr", "no-text",
 * "translate", "write", "cancelled"), warning, url, directory, name, source, source_lang,
 * source_lines, duration, targets_done, targets_failed,
 * outputs[] {role:"source"|"translation", language, name, file, size, lines},
 * targets[] {index, code, name, state:"done"|"same-language"|"failed"|"not-started", error}.
 *
 * Progress, property user-data/nanum/generate (published while the job runs and kept after it
 * ended): busy, status ("idle"|"opening"|"scanning"|"glossary"|"translating"|"done"|
 * "cancelled"|"error"), source, url, directory, name, progress (0..1 of the whole job),
 * phase_progress (0..1), position, duration, lines, target (index), num_targets, target_name,
 * elapsed, error, error_code, warning, outputs[] (the files written so far, as in the result).
 *
 * One job runs at a time; another is rejected with error_code "busy".
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_GENERATE_H_
#define MP_NANUM_GENERATE_H_

struct mp_cmd_ctx;

// Command handler for nanum-generate (.spawn_thread = true, .can_abort = true). Called with
// the core locked; it unlocks the core while it works and returns with it locked again, having
// filled cmd->result. cmd->success stays true whenever a result map was produced.
void nanum_generate_command(struct mp_cmd_ctx *cmd, const char *spec);

#endif
