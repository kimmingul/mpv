/*
 * NanumPlayer: optional speech-model manager.
 *
 * The Whisper / Silero models are not shipped with the application. They live
 * in %LOCALAPPDATA%\NanumPlayer\models (NANUMPLAYER_MODELS_DIR overrides it)
 * and are fetched on request from the pinned URLs of models\catalog.json, over
 * HTTPS (WinHTTP, certificate validation always on), by one background worker.
 * Size and SHA-256 are verified before the file is renamed into place.
 *
 * Exposed to scripts as the commands
 *     nanum-models-refresh              publish the current state
 *     nanum-download-models <target>    all | english | multilingual | multilingual_large | vad
 *     nanum-cancel-model-download
 * and as the property user-data/nanum/models (see nanum-models-win.c).
 * All commands return at once; the worker publishes through the core's
 * dispatch queue.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_MODELS_H_
#define MP_NANUM_MODELS_H_

#include <stdbool.h>

struct MPContext;

enum nanum_model_target {
    NANUM_MODELS_ALL,
    NANUM_MODELS_ENGLISH,
    NANUM_MODELS_MULTILINGUAL,
    NANUM_MODELS_VAD,
    NANUM_MODELS_MULTILINGUAL_LARGE,   // not part of "all"
};

// Indices of the default model paths returned by nanum_models_startup().
enum {
    NANUM_MODEL_PATH_ENGLISH,       // --nanum-stt-model
    NANUM_MODEL_PATH_MULTILINGUAL,  // --nanum-stt-model-multilingual (large-v3-turbo when installed)
    NANUM_MODEL_PATH_VAD,           // --nanum-vad-model
    NANUM_MODEL_PATH_COUNT,
};

#ifdef _WIN32

// Called once by nanum_startup_defaults(). Loads <root>\models\catalog.json and
// returns the default model paths (talloc strings under ctx): a model file next
// to the application (<root>\models, size-checked against the catalog) stays in
// use, otherwise the persistent user directory is used, even while the file
// does not exist yet. Nothing is created or downloaded here.
void nanum_models_startup(struct MPContext *mpctx, const char *root, void *ctx,
                          char *defaults[NANUM_MODEL_PATH_COUNT]);

// Publishes user-data/nanum/models from the file system (never hashes).
void nanum_models_refresh(struct MPContext *mpctx);

// Starts the worker. false (error logged and published) if it cannot start,
// including "already running".
bool nanum_models_download(struct MPContext *mpctx, enum nanum_model_target target);

// Cancels the running download, if any. Returns at once.
void nanum_models_cancel(struct MPContext *mpctx);

// Cancels and joins the worker. Called from mp_destroy(); no download can be
// started afterwards.
void nanum_models_shutdown(struct MPContext *mpctx);

#else

static inline void nanum_models_refresh(struct MPContext *mpctx) {}
static inline bool nanum_models_download(struct MPContext *mpctx,
                                         enum nanum_model_target target)
{
    return false;
}
static inline void nanum_models_cancel(struct MPContext *mpctx) {}
static inline void nanum_models_shutdown(struct MPContext *mpctx) {}

#endif

#endif
