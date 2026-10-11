/*
 * NanumPlayer: AI engine selection and the optional CUDA component manager.
 *
 * The CPU engine (nanum_ai_cpu.dll) and the CUDA engine (nanum_ai_cuda.dll)
 * ship with the application. CUDA also needs NVIDIA's cuBLAS runtimes
 * (cublas64_13.dll, cublasLt64_13.dll), which come from
 *
 *   toolkit   the bin\x64 / bin folder of an installed, compatible CUDA Toolkit
 *             (CUDA_PATH_V13_*, CUDA_PATH of the process or of the registry's
 *             environment, %ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v13.*).
 *             Toolkit files are used in place; they are never copied, replaced
 *             or removed, and their folders are never added to a process wide
 *             search path. No license step is needed.
 *   download  %LOCALAPPDATA%\NanumPlayer\accelerators\cuda\<CUDA major>\
 *             (NANUMPLAYER_ACCEL_DIR overrides the base folder), after the
 *             NVIDIA license was accepted. Independent of the app version;
 *             files of the old per-version folders are copied (verified).
 *
 * Names, sizes, SHA-256 values and pinned HTTPS URLs of the runtimes come from
 * <app>\components\catalog.json (schema 1), a file shipped with the
 * application. Downloads reuse the models manager's engine
 * (osdep/nanum-download-win.c): size and digest verified, one .part per
 * process, atomic rename, cancel leaves existing components untouched.
 *
 * Exposed to scripts as the commands
 *     nanum-accelerator-refresh
 *     nanum-download-accelerator
 *     nanum-cancel-accelerator-download
 * and the property user-data/nanum/accelerator (see nanum-accel-win.c).
 * A finished download is used from the next opened file on: the engine DLL of
 * a file that is playing is never replaced.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#ifndef MP_NANUM_ACCEL_H_
#define MP_NANUM_ACCEL_H_

#include <stdbool.h>
#include <stddef.h>

struct MPContext;
struct mp_log;

// --nanum-acceleration
enum nanum_accel_mode {
    NANUM_ACCEL_AUTO,       // CUDA if it is installed and usable, else CPU
    NANUM_ACCEL_CPU,
    NANUM_ACCEL_CUDA,       // CUDA or no AI at all; never falls back
};

// One loaded engine DLL. The handles (the engine and the runtime DLLs it
// needs) stay loaded until nanum_accel_unload_engine().
struct nanum_engine {
    void *module;               // HMODULE of the engine, NULL if none
    bool cuda;                  // the engine runs on CUDA
    int use_gpu;                // value for nanum_ai_config.use_gpu
    char backend[16];           // "cpu" | "cuda"
    char path[1024];            // engine file (UTF-8)
    char notice[768];           // auto: why CUDA is not used ("" if it is, or in cpu mode)
    void *runtime[2];           // runtime DLL handles kept for the engine's lifetime
    int num_runtime;
};

#ifdef _WIN32

// Called once by nanum_startup_defaults(): remembers the application root,
// loads <root>\components\catalog.json and computes the user folder. Nothing
// is created, loaded or downloaded here.
void nanum_accel_startup(struct MPContext *mpctx, const char *root);

// Publishes user-data/nanum/accelerator from the file system. If the CUDA
// files are present but were never checked, a background probe is started
// (status "checking", no download).
void nanum_accel_refresh(struct MPContext *mpctx);

// Starts the worker that downloads the components that are missing (the
// engine and, unless a compatible Toolkit provides them, the runtimes).
// false (error logged and published) if it cannot start or is already running.
bool nanum_accel_download(struct MPContext *mpctx);

// Cancels the running download or probe, if any. Returns at once.
void nanum_accel_cancel(struct MPContext *mpctx);

// Cancels and joins the worker. Called from mp_destroy().
void nanum_accel_shutdown(struct MPContext *mpctx);

// Chooses and loads the engine DLL for a file (any thread). `custom_dll`
// (--nanum-ai-dll, "" = managed) is an explicit user override and is loaded
// as it is. On false, err says what is missing and what to do.
bool nanum_accel_load_engine(struct mp_log *log, enum nanum_accel_mode mode,
                             const char *custom_dll, struct nanum_engine *out,
                             char *err, size_t errsz);

// Releases the engine and its runtime DLLs. Safe on a zeroed struct.
void nanum_accel_unload_engine(struct nanum_engine *e);

#else

static inline void nanum_accel_refresh(struct MPContext *mpctx) {}
static inline bool nanum_accel_download(struct MPContext *mpctx) { return false; }
static inline void nanum_accel_cancel(struct MPContext *mpctx) {}
static inline void nanum_accel_shutdown(struct MPContext *mpctx) {}

#endif

#endif
