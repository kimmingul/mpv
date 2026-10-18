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

#include "mpv/client.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/node.h"
#include "misc/path_utils.h"
#include "options/m_config.h"
#include "options/m_property.h"
#include "options/options.h"
#include "options/parse_configfile.h"
#include "options/path.h"
#include "osdep/io.h"
#include "osdep/nanum-startup.h"
#include "osdep/nanum-version.h"
#include "osdep/nanum-private.h"
#include "osdep/nanum-models.h"
#include "osdep/nanum-youtube.h"
#include "osdep/nanum-accel.h"
#include "osdep/nanum-disc-component.h"
#include "osdep/nanum-diag.h"
#include "player/command.h"
#include "player/core.h"
#include "nanum/i18n.h"

// Written to the config dir on first run only. Never touched afterwards.
static const char DEFAULT_CONF[] =
    "# NanumPlayer settings (edit freely; Ctrl+N inside the player changes the common ones)\n"
    "# Paths of the engine/models are set by NanumPlayer.exe and need not be set here.\n"
    "demuxer-readahead-secs=120\n"
    "demuxer-max-bytes=1500MiB\n"
    "# nanum-translate-endpoint=http://127.0.0.1:11434\n"
    "# nanum-translate-model=gemma4:latest\n"
    "# Language of the AI subtitles; default: your Windows display language. Ctrl+N changes it too.\n"
    "# nanum-target-language=Korean\n"
    "# Blu-ray: set the drive, e.g.  bluray-device=E:\n";

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
    wchar_t *root = app_root(tmp);
    if (root) {
        // libbluray appends ".dll". Both loaders are pinned to a place where nothing
        // exists until the optional disc component is verified (osdep/nanum-disc-component-win.c);
        // old renamed libmmbd copies and installation discovery must never reintroduce a
        // console-process dependency, and no decryption library is picked up by accident.
        nanum_disc_component_environment(root);
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

// The directory of the user's settings. Normally the config dir resolved by
// mpv (%APPDATA%\NanumPlayer, --config-dir, portable_config); with --no-config
// it is still %APPDATA%\NanumPlayer, so the diagnostics folder keeps one stable
// location.
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

// The Windows display language as the translation model is told its name (nanum-target-language). The
// first of the user's preferred display languages, else the default UI language of the account;
// English when the language is not in the list.
static void os_language(char code[8], char *name, size_t name_size)
{
    static const struct { const char *code, *name; } langs[] = {
        {"ko", "Korean"}, {"en", "English"}, {"ja", "Japanese"}, {"es", "Spanish"}, {"fr", "French"},
        {"de", "German"}, {"it", "Italian"}, {"pt", "Portuguese"}, {"ru", "Russian"}, {"ar", "Arabic"},
        {"hi", "Hindi"}, {"th", "Thai"}, {"vi", "Vietnamese"}, {"id", "Indonesian"}, {"ms", "Malay"},
        {"fil", "Filipino"}, {"tl", "Filipino"}, {"bn", "Bengali"}, {"nl", "Dutch"}, {"pl", "Polish"},
        {"tr", "Turkish"}, {"sv", "Swedish"}, {"da", "Danish"}, {"nb", "Norwegian"}, {"nn", "Norwegian"},
        {"no", "Norwegian"}, {"fi", "Finnish"}, {"cs", "Czech"}, {"el", "Greek"}, {"he", "Hebrew"},
        {"hu", "Hungarian"}, {"ro", "Romanian"}, {"uk", "Ukrainian"}, {"fa", "Persian"},
    };
    char tag[64] = "";
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, NULL, &size) && size) {
        wchar_t *list = calloc(size, sizeof(wchar_t));
        if (list && GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list, &size) && count > 0)
            WideCharToMultiByte(CP_UTF8, 0, list, -1, tag, sizeof(tag), NULL, NULL);
        free(list);
    }
    if (!tag[0]) {
        wchar_t wname[LOCALE_NAME_MAX_LENGTH];
        if (LCIDToLocaleName(GetUserDefaultUILanguage(), wname, MP_ARRAY_SIZE(wname), 0))
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, tag, sizeof(tag), NULL, NULL);
    }
    // "zh-Hant-TW" / "ko-KR": primary subtag lower-case, the rest upper-cased for the script / region test
    char primary[8] = "";
    size_t n = 0;
    while (tag[n] && tag[n] != '-' && tag[n] != '_' && n < sizeof(primary) - 1) {
        primary[n] = (char)(tag[n] >= 'A' && tag[n] <= 'Z' ? tag[n] + 32 : tag[n]);
        n++;
    }
    primary[n] = 0;
    const char *lang = "English";
    const char *iso = "en";
    if (!strcmp(primary, "zh")) {
        char rest[64];
        snprintf(rest, sizeof(rest), "%s", tag + n);
        for (char *p = rest; *p; p++)
            *p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        bool traditional = strstr(rest, "HANT") || strstr(rest, "-TW") || strstr(rest, "-HK") ||
                           strstr(rest, "-MO");
        lang = traditional ? "Traditional Chinese" : "Simplified Chinese";
        iso = "zh";
    } else {
        for (size_t i = 0; i < MP_ARRAY_SIZE(langs); i++) {
            if (!strcmp(primary, langs[i].code)) {
                lang = langs[i].name;
                iso = langs[i].code;
                break;
            }
        }
    }
    snprintf(code, 8, "%s", iso);
    snprintf(name, name_size, "%s", lang);
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

    MP_INFO(mpctx, "NanumPlayer " NANUM_VERSION " (%s)\n", mpv_version);

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
    add_opt(&text, "title", "${?media-title:${media-title} - NanumPlayer " NANUM_VERSION "}${!media-title:NanumPlayer}");

    // AI engine.
    add_opt(&text, "nanum", "yes");
    // The AI engine is chosen per file by osdep/nanum-accel-win.c (CPU engine in
    // the application folder, optional CUDA components). --nanum-ai-dll stays
    // empty here: setting it is an explicit user override of that choice.
    nanum_accel_startup(mpctx, root);
    // Blu-ray/DVD decryption is an optional component (osdep/nanum-disc-component-win.c).
    nanum_disc_component_startup(mpctx, root);
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
    // Nothing about what is played is kept (see osdep/nanum-private.h): no
    // application log, no translation cache, no subtitle library, no resume or
    // watch history, no disk shader/ICC cache, and yt-dlp keeps no cache folder.
    // They are all still available when asked for explicitly (--log-file,
    // --nanum-cache, --nanum-library-db, --save-position-on-quit, ...).
    add_opt(&text, "resume-playback", "no");
    add_opt(&text, "save-position-on-quit", "no");
    add_opt(&text, "save-watch-history", "no");
    add_opt(&text, "gpu-shader-cache", "no");
    add_opt(&text, "icc-cache", "no");
    add_opt(&text, "ytdl-raw-options-append", "no-cache-dir=");
    // Uploader subtitles only (no write-auto-subs): ytdl_hook adds them as external tracks, the subtitle list
    // shows them as originals; automatic captions are never fetched.
    add_opt(&text, "ytdl-raw-options-append", "write-subs=");
    // The disc diagnostics (explicit, see nanum-diag.h) are the only log written.
    nanum_diag_startup(mpctx, settings);
    // The language the AI subtitles are made in: the Windows display language (mpv.conf, the command line and
    // the choice saved by the player's menu still override it).
    char os_code[8], os_name[48];
    os_language(os_code, os_name, sizeof(os_name));
    add_opt(&text, "nanum-target-language", os_name);

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

// The string value of an option ("" if unset).
static char *option_string(struct MPContext *mpctx, void *ctx, const char *name)
{
    char *prop = talloc_asprintf(ctx, "options/%s", name);
    char *val = NULL;
    char *res = talloc_strdup(ctx, "");
    if (mp_property_do(prop, M_PROPERTY_GET_STRING, &val, mpctx) > 0 && val) {
        talloc_free(res);
        res = talloc_strdup(ctx, val);
    }
    talloc_free(val);
    talloc_free(prop);
    return res;
}

// Nothing about what is played is kept (see nanum-private.h). Runs after the
// config files and command line are known: removes what earlier versions kept by
// default (unless the user chose a place for it), sweeps the transient folders of
// killed sessions and publishes user-data/nanum/privacy for the scripts.
static void nanum_privacy_startup(struct MPContext *mpctx, void *tmp)
{
    bool configured = false;
    char *settings = settings_dir(tmp, mpctx->global, &configured);
    bool library = option_string(mpctx, tmp, "nanum-library-db")[0];
    bool cache = option_string(mpctx, tmp, "nanum-cache")[0];
    bool full = option_string(mpctx, tmp, "nanum-full-dir")[0];
    bool log = option_string(mpctx, tmp, "log-file")[0];
    char dumps[4];
    bool dump = GetEnvironmentVariableA("NANUMPLAYER_CRASH_DUMP_DIR", dumps, sizeof(dumps)) > 0;

    nanum_private_sweep();
    nanum_private_purge_legacy(settings, library, full, cache, log);

    mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_flag(&root, "library", library);
    node_map_add_flag(&root, "translation_cache", cache);
    node_map_add_string(&root, "result_dir", full ? "custom" : "session");
    node_map_add_flag(&root, "crash_dumps", dump);
    node_map_add_flag(&root, "app_log", log);
    mp_property_do("user-data/nanum/privacy", M_PROPERTY_SET_NODE, &root, mpctx);
    talloc_free(root.u.list);
}

void nanum_startup_scripts(struct MPContext *mpctx)
{
    void *tmp = talloc_new(NULL);
    // The interface language is known before any script (or native dialog) runs:
    // --nanum-ui-language, else ui-language.conf in the settings directory, else the
    // Windows display language.
    bool configured = false;
    char *settings = settings_dir(tmp, mpctx->global, &configured);
    nanum_ui_language_startup(mpctx, settings, option_string(mpctx, tmp, "nanum-ui-language"));
    nanum_privacy_startup(mpctx, tmp);
    // Scripts find user-data/nanum/app-version and user-data/nanum/models already filled
    // when they load.
    mpv_node version = {.format = MPV_FORMAT_STRING, .u.string = (char *)NANUM_VERSION};
    mp_property_do("user-data/nanum/app-version", M_PROPERTY_SET_NODE, &version, mpctx);
    nanum_models_refresh(mpctx);
    nanum_accel_refresh(mpctx);
    nanum_youtube_publish(mpctx);
    nanum_diag_publish(mpctx);
    {
        // The Windows display language as the model names it, for the player's language menu ("automatic").
        char os_code[8], os_name[48];
        os_language(os_code, os_name, sizeof(os_name));
        mpv_node code = {.format = MPV_FORMAT_STRING, .u.string = os_code};
        mpv_node name = {.format = MPV_FORMAT_STRING, .u.string = os_name};
        mp_property_do("user-data/nanum/os-language", M_PROPERTY_SET_NODE, &code, mpctx);
        mp_property_do("user-data/nanum/os-language-name", M_PROPERTY_SET_NODE, &name, mpctx);
    }
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
