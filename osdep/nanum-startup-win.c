/*
 * NanumPlayer: native application startup (Windows). See nanum-startup.h.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/path_utils.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/parse_configfile.h"
#include "options/path.h"
#include "osdep/io.h"
#include "osdep/nanum-startup.h"
#include "osdep/nanum-models.h"
#include "osdep/nanum-youtube.h"
#include "osdep/nanum-accel.h"
#include "osdep/nanum-diag.h"
#include "player/core.h"

// Written to the config dir on first run only. Never touched afterwards.
static const char DEFAULT_CONF[] =
    "# NanumPlayer settings (edit freely; Ctrl+N inside the player changes the common ones)\n"
    "# Paths of the engine/models are set by NanumPlayer.exe and need not be set here.\n"
    "demuxer-readahead-secs=120\n"
    "demuxer-max-bytes=1500MiB\n"
    "slang=kor\n"
    "# nanum-translate-endpoint=http://127.0.0.1:11434\n"
    "# nanum-translate-model=gemma4:latest\n"
    "# nanum-target-language=Korean\n"
    "# Blu-ray: set the drive, e.g.  bluray-device=E:\n";

static bool is_file(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static wchar_t *wjoin(void *ctx, const wchar_t *a, const wchar_t *b)
{
    size_t la = wcslen(a), lb = wcslen(b);
    wchar_t *r = talloc_array(ctx, wchar_t, la + lb + 2);
    wcscpy(r, a);
    r[la] = L'\\';
    wcscpy(r + la + 1, b);
    return r;
}

// Directory of the executable: where the Windows loader looks first for DLLs.
static wchar_t *exe_dir(void *ctx)
{
    DWORD cap = MAX_PATH;
    wchar_t *buf = talloc_array(ctx, wchar_t, cap);
    for (;;) {
        DWORD len = GetModuleFileNameW(NULL, buf, cap);
        if (!len) {
            talloc_free(buf);
            return NULL;
        }
        if (len < cap)
            break;
        cap *= 2;
        buf = talloc_realloc(ctx, buf, wchar_t, cap);
    }
    wchar_t *slash = wcsrchr(buf, L'\\');
    if (slash)
        *slash = 0;
    return buf;
}

// Directory holding the application files (models\, scripts\, nanum_ai.dll):
// the folder of the executable. NANUMPLAYER_ROOT overrides it, so a build tree
// can be run against a staged resource folder.
static wchar_t *app_root(void *ctx)
{
    DWORD n = GetEnvironmentVariableW(L"NANUMPLAYER_ROOT", NULL, 0);
    if (n > 1) {
        wchar_t *r = talloc_array(ctx, wchar_t, n);
        if (GetEnvironmentVariableW(L"NANUMPLAYER_ROOT", r, n) == n - 1) {
            size_t len = wcslen(r);
            while (len > 3 && (r[len - 1] == L'\\' || r[len - 1] == L'/'))
                r[--len] = 0;
            return r;
        }
        talloc_free(r);
    }
    return exe_dir(ctx);
}

void nanum_startup_environment(void)
{
    void *tmp = talloc_new(NULL);
    wchar_t *exe = exe_dir(tmp);
    wchar_t *root = app_root(tmp);

    // libbluray's Windows dl_dlopen appends ".dll" even to absolute paths, so
    // LIB*_PATH values are written without extension. They are read with the C
    // runtime's getenv(), which does not see SetEnvironmentVariableW() changes
    // made after startup (the old launcher set them for a child process):
    // _wputenv_s() updates both.
    static const struct {
        const wchar_t *variable;
        const wchar_t *name;
    } libs[] = {
        {L"LIBAACS_PATH", L"libaacs"},
        {L"LIBBDPLUS_PATH", L"libbdplus"},
    };

    // The user's MakeMKV (libmmbd is API compatible with libaacs/libbdplus),
    // used in place only, never copied.
    wchar_t *mmbd = NULL;
    static const wchar_t *const folders[] =
        {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"};
    for (size_t i = 0; i < MP_ARRAY_SIZE(folders) && !mmbd; i++) {
        DWORD n = GetEnvironmentVariableW(folders[i], NULL, 0);
        if (n <= 1)
            continue;
        wchar_t *dir = talloc_array(tmp, wchar_t, n);
        if (GetEnvironmentVariableW(folders[i], dir, n) != n - 1)
            continue;
        wchar_t *make = wjoin(tmp, dir, L"MakeMKV");
        wchar_t *file = wjoin(tmp, make, L"libmmbd64.dll");
        if (is_file(file)) {
            file[wcslen(file) - 4] = 0;
            mmbd = file;
        }
    }

    for (size_t i = 0; i < MP_ARRAY_SIZE(libs); i++) {
        // 1. An explicit override always wins.
        if (GetEnvironmentVariableW(libs[i].variable, NULL, 0))
            continue;

        wchar_t file[64];
        swprintf(file, MP_ARRAY_SIZE(file), L"%ls.dll", libs[i].name);

        // 2. A library next to the executable: the loader finds it.
        if (exe && is_file(wjoin(tmp, exe, file)))
            continue;
        if (root) {
            // 3. Legacy layout (<root>\bin\libaacs.dll, kept by the installer
            //    from versions before 1.1): point libbluray at it.
            wchar_t *legacy = wjoin(tmp, root, L"bin");
            legacy = wjoin(tmp, legacy, file);
            if (is_file(legacy)) {
                legacy[wcslen(legacy) - 4] = 0;
                _wputenv_s(libs[i].variable, legacy);
                continue;
            }
        }
        // 4. MakeMKV.
        if (mmbd)
            _wputenv_s(libs[i].variable, mmbd);
    }

    talloc_free(tmp);
}

static void add_opt(char **text, const char *name, const char *value)
{
    // %n%value is the config file's verbatim syntax: no quoting problems with
    // backslashes, spaces, '#' or '$' in the value.
    *text = talloc_asprintf_append(*text, "%s=%%%d%%%s\n", name,
                                   (int)strlen(value), value);
}

static void add_path_opt(char **text, const char *name, const char *dir,
                         const char *file)
{
    char *value = talloc_asprintf(*text, "%s\\%s", dir, file);
    add_opt(text, name, value);
    talloc_free(value);
}

// The directory of the user's settings. Normally the config dir resolved by
// mpv (%APPDATA%\NanumPlayer, --config-dir, portable_config); with --no-config
// it is still %APPDATA%\NanumPlayer, so the log and translation cache keep one
// stable location.
static char *settings_dir(void *ctx, struct mpv_global *global, bool *configured)
{
    char *dir = mp_find_user_file(ctx, global, "home", "");
    *configured = dir && dir[0];
    if (*configured) {
        size_t len = strlen(dir);
        while (len > 1 && (dir[len - 1] == '/' || dir[len - 1] == '\\'))
            dir[--len] = 0;
        return dir;
    }

    wchar_t *roaming = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, KF_FLAG_CREATE,
                                    NULL, &roaming)))
        return NULL;
    char *r = talloc_asprintf(ctx, "%s\\NanumPlayer", mp_to_utf8(ctx, roaming));
    CoTaskMemFree(roaming);
    return r;
}

static void create_default_conf(const char *dir)
{
    char *path = talloc_asprintf(NULL, "%s\\mpv.conf", dir);
    wchar_t *wpath = mp_from_utf8(path, path);
    HANDLE f = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(f, DEFAULT_CONF, (DWORD)(sizeof(DEFAULT_CONF) - 1), &written, NULL);
        CloseHandle(f);
    }
    talloc_free(path);
}

void nanum_startup_defaults(struct MPContext *mpctx)
{
    void *tmp = talloc_new(NULL);

    MP_INFO(mpctx, "NanumPlayer 1.6.0 (%s)\n", mpv_version);

    bool configured = false;
    char *settings = settings_dir(tmp, mpctx->global, &configured);
    if (settings) {
        mp_mkdirp(settings);
        if (configured) {
            char *conf = talloc_asprintf(tmp, "%s\\mpv.conf", settings);
            if (!mp_path_exists(conf))
                create_default_conf(settings);
        }
    }

    wchar_t *wroot = app_root(tmp);
    char *root = wroot ? mp_to_utf8(tmp, wroot) : NULL;

    char *text = talloc_strdup(tmp, "");
    // Application behaviour. The UI is drawn by scripts/nanum-fluent.lua, so
    // the stock OSC (and with it the mpv idle logo) stays off.
    add_opt(&text, "osc", "no");
    add_opt(&text, "idle", "yes");
    add_opt(&text, "force-window", "yes");
    add_opt(&text, "window-corners", "round");
    add_opt(&text, "title", "${?media-title:${media-title} - }NanumPlayer");

    // AI engine.
    add_opt(&text, "nanum", "yes");
    // The AI engine is chosen per file by osdep/nanum-accel-win.c (CPU engine in
    // the application folder, optional CUDA components). --nanum-ai-dll stays
    // empty here: setting it is an explicit user override of that choice.
    nanum_accel_startup(mpctx, root);
    // Models are optional downloads. A model file next to the application
    // stays in use; otherwise the persistent user model folder is the default
    // (see osdep/nanum-models-win.c), so a later download is picked up by the
    // next opened file without a restart. mpv.conf and the command line still
    // override these.
    char *models[NANUM_MODEL_PATH_COUNT] = {0};
    nanum_models_startup(mpctx, root, tmp, models);
    if (models[NANUM_MODEL_PATH_ENGLISH])
        add_opt(&text, "nanum-stt-model", models[NANUM_MODEL_PATH_ENGLISH]);
    if (models[NANUM_MODEL_PATH_MULTILINGUAL])
        add_opt(&text, "nanum-stt-model-multilingual",
                models[NANUM_MODEL_PATH_MULTILINGUAL]);
    if (models[NANUM_MODEL_PATH_VAD])
        add_opt(&text, "nanum-vad-model", models[NANUM_MODEL_PATH_VAD]);
    // YouTube playback: mpv's ytdl_hook runs the managed yt-dlp (see
    // osdep/nanum-youtube-win.c), falling back to a yt-dlp in PATH while that
    // is not installed.
    char *ytdlp = nanum_youtube_startup(mpctx, tmp);
    if (ytdlp) {
        char *opt = talloc_asprintf(tmp, "ytdl_hook-ytdl_path=%s;yt-dlp", ytdlp);
        add_opt(&text, "script-opts-append", opt);
    }
    char *app_log = NULL;
    if (settings) {
        add_path_opt(&text, "nanum-cache", settings, "tr_cache.jsonl");
        add_path_opt(&text, "log-file", settings, "last-playback.log");
        app_log = talloc_asprintf(tmp, "%s\\last-playback.log", settings);
    }
    // The application log is bounded (info and above, previous sessions kept,
    // size limit) and diagnostic mode is set up; both apply to this default
    // log file only, a --log-file of the user is left as it was.
    nanum_diag_startup(mpctx, settings, app_log);

    // Same level as mpv's built-in profile defaults: below mpv.conf and the
    // command line.
    if (m_config_parse(mpctx->mconfig, "NanumPlayer defaults", bstr0(text),
                       NULL, 0) < 0)
        MP_ERR(mpctx, "Cannot apply the NanumPlayer defaults.\n");

    talloc_free(tmp);
}

static bool script_given(struct MPContext *mpctx, const char *name)
{
    char **files = mpctx->opts->script_files;
    for (int n = 0; files && files[n]; n++) {
        const char *base = files[n];
        for (const char *p = files[n]; *p; p++) {
            if (*p == '/' || *p == '\\')
                base = p + 1;
        }
        if (!_stricmp(base, name))
            return true;
    }
    return false;
}

void nanum_startup_scripts(struct MPContext *mpctx)
{
    void *tmp = talloc_new(NULL);
    // Scripts find user-data/nanum/models already filled when they load.
    nanum_models_refresh(mpctx);
    nanum_accel_refresh(mpctx);
    nanum_youtube_publish(mpctx);
    nanum_diag_publish(mpctx);
    wchar_t *wroot = app_root(tmp);
    if (wroot) {
        char *root = mp_to_utf8(tmp, wroot);
        // Order matters: the control script first, the interface that talks
        // to it second.
        static const char *const scripts[] = {"nanum.lua", "nanum-fluent.lua"};
        for (int i = 0; i < MP_ARRAY_SIZE(scripts); i++) {
            if (script_given(mpctx, scripts[i]))
                continue;
            char *path = talloc_asprintf(tmp, "%s\\scripts\\%s", root, scripts[i]);
            if (!mp_path_exists(path)) {
                MP_WARN(mpctx, "Script '%s' not found.\n", path);
                continue;
            }
            if (mp_load_user_script(mpctx, path) < 0)
                MP_ERR(mpctx, "Failed to load script '%s'.\n", path);
        }
    }
    talloc_free(tmp);
}
