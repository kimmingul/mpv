/*
 * NanumPlayer: opening media from outside the player (Windows). See nanum-open.h.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
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
#include "osdep/io.h"
#include "osdep/nanum-open.h"
#include "nanum/nanum.h"
#include "player/core.h"

#define PIPE_PREFIX L"\\\\.\\pipe\\"
#define YTDL_FORMAT "bestvideo[height<=?1080]+bestaudio/best"

static wchar_t *wdup(void *ctx, const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *r = talloc_array(ctx, wchar_t, n);
    memcpy(r, s, n * sizeof(wchar_t));
    return r;
}

static wchar_t *wfmt(void *ctx, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vscwprintf(fmt, ap);
    va_end(ap);
    wchar_t *r = talloc_array(ctx, wchar_t, n + 1);
    va_start(ap, fmt);
    _vsnwprintf(r, n + 1, fmt, ap);
    va_end(ap);
    return r;
}

// ---------------------------------------------------------------------------
// Default IPC pipe

// "\\.\pipe\nanumplayer-<user>" (a wide string under ctx).
static wchar_t *default_pipe(void *ctx)
{
    wchar_t env[256];
    DWORD n = GetEnvironmentVariableW(L"NANUMPLAYER_IPC_PIPE", env, MP_ARRAY_SIZE(env));
    if (n && n < MP_ARRAY_SIZE(env)) {
        if (!wcsncmp(env, PIPE_PREFIX, 9))
            return wdup(ctx, env);
        return wfmt(ctx, PIPE_PREFIX L"%ls", env);
    }
    wchar_t user[128] = L"user";
    DWORD len = MP_ARRAY_SIZE(user);
    GetUserNameW(user, &len);
    wchar_t *name = wfmt(ctx, PIPE_PREFIX L"nanumplayer-%ls", user);
    // A pipe name has no backslashes beyond the prefix; keep it to plain characters.
    for (wchar_t *p = name + 9; *p; p++) {
        if (!iswalnum(*p) && *p != L'-' && *p != L'_')
            *p = L'_';
    }
    return name;
}

// Whether a pipe of that name exists (somebody listens).
static bool pipe_exists(const wchar_t *name)
{
    if (WaitNamedPipeW(name, 0))
        return true;
    return GetLastError() != ERROR_FILE_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// nanumplayer:// requests

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// The value of the query parameter "url" of a nanumplayer: argument, percent-decoded, or
// NULL if the request is not valid. Only absolute http(s) urls without control characters
// or spaces are accepted: a web page can start this handler.
static char *request_url(void *ctx, const char *arg)
{
    if (strncasecmp(arg, "nanumplayer:", 12))
        return NULL;
    const char *q = strchr(arg, '?');
    while (q) {
        q++;
        if (!strncasecmp(q, "url=", 4))
            break;
        q = strchr(q, '&');
    }
    if (!q)
        return NULL;
    q += 4;
    // encodeURIComponent output ends at the next "&"; an url passed unencoded keeps its own.
    size_t len = strncasecmp(q, "http", 4) ? strcspn(q, "&#") : strcspn(q, "#");
    char *out = talloc_array(ctx, char, len + 1);
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (q[i] == '%') {
            if (i + 2 >= len)
                return NULL;
            int h = hexval(q[i + 1]), l = hexval(q[i + 2]);
            if (h < 0 || l < 0)
                return NULL;
            out[o++] = (char)(h * 16 + l);
            i += 2;
        } else {
            out[o++] = q[i];
        }
    }
    out[o] = '\0';
    if (strncasecmp(out, "http://", 7) && strncasecmp(out, "https://", 8))
        return NULL;
    for (size_t i = 0; i < o; i++) {
        unsigned char c = out[i];
        if (c <= ' ' || c == 0x7f)
            return NULL;
    }
    return out;
}

static char *json_string(void *ctx, const char *s)
{
    char *out = talloc_array(ctx, char, strlen(s) * 2 + 3);
    size_t o = 0;
    out[o++] = '"';
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            out[o++] = '\\';
        out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = '\0';
    return out;
}

struct find_win {
    DWORD pid;
    HWND hwnd;
};

static BOOL CALLBACK find_window(HWND h, LPARAM lp)
{
    struct find_win *f = (struct find_win *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
        f->hwnd = h;
        return FALSE;
    }
    return TRUE;
}

// Brings the running player's window to the front (best effort).
static void raise_player(HANDLE pipe)
{
    struct find_win f = {0};
    if (!GetNamedPipeServerProcessId(pipe, &f.pid))
        return;
    AllowSetForegroundWindow(f.pid);
    EnumWindows(find_window, (LPARAM)&f);
    if (!f.hwnd)
        return;
    if (IsIconic(f.hwnd))
        ShowWindow(f.hwnd, SW_RESTORE);
    SetForegroundWindow(f.hwnd);
}

// Hands the url to the running player. false if there is none or it did not answer.
static bool forward_url(const char *url)
{
    void *tmp = talloc_new(NULL);
    wchar_t *pipe = default_pipe(tmp);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 8; i++) {
        h = CreateFileW(pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY)
            break;
        WaitNamedPipeW(pipe, 300);
    }
    if (h == INVALID_HANDLE_VALUE) {
        talloc_free(tmp);
        return false;
    }
    char *js = json_string(tmp, url);
    char *msg = talloc_asprintf(tmp, "{\"command\":[\"script-message\",\"nanum-youtube-open\",%s],"
                                     "\"request_id\":1}\n", js);
    DWORD written = 0;
    bool ok = WriteFile(h, msg, (DWORD)strlen(msg), &written, NULL) && written == strlen(msg);
    if (ok) {
        // The player answers once it has queued the command.
        char reply[256];
        DWORD got = 0;
        ReadFile(h, reply, sizeof(reply), &got, NULL);
        raise_player(h);
    }
    CloseHandle(h);
    talloc_free(tmp);
    return ok;
}

int nanum_open_forward(char **options)
{
    for (int i = 0; options && options[i]; i++) {
        if (strncasecmp(options[i], "nanumplayer:", 12))
            continue;
        char *url = request_url(NULL, options[i]);
        if (!url)
            return 2;
        if (forward_url(url)) {
            talloc_free(url);
            return 1;
        }
        // No running player: this one plays the url (the pointer lives until exit).
        options[i] = url;
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Startup

static wchar_t *protocol_root(void *ctx)
{
    wchar_t env[256];
    DWORD n = GetEnvironmentVariableW(L"NANUMPLAYER_PROTOCOL_KEY", env, MP_ARRAY_SIZE(env));
    if (n && n < MP_ARRAY_SIZE(env))
        return wdup(ctx, env);
    return NULL;
}

static bool reg_set(HKEY key, const wchar_t *name, const wchar_t *value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
                          (DWORD)((wcslen(value) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// Registers nanumplayer:// for this installation. The application folder is recognised by
// its scripts (a development build next to nothing is never registered); the key under HKCU
// can be moved with NANUMPLAYER_PROTOCOL_KEY (tests).
static void register_protocol(struct MPContext *mpctx, void *tmp)
{
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MP_ARRAY_SIZE(exe));
    if (!n || n >= MP_ARRAY_SIZE(exe))
        return;
    wchar_t *base = protocol_root(tmp);
    if (!base) {
        wchar_t *dir = wdup(tmp, exe);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash)
            *slash = 0;
        wchar_t *marker = wfmt(tmp, L"%ls\\scripts\\nanum.lua", dir);
        if (GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES)
            return;
        base = wdup(tmp, L"Software\\Classes");
    }
    wchar_t *cmd_key = wfmt(tmp, L"%ls\\nanumplayer\\shell\\open\\command", base);
    wchar_t *command = wfmt(tmp, L"\"%ls\" \"%%1\"", exe);

    wchar_t current[1024] = L"";
    DWORD len = sizeof(current) - sizeof(wchar_t);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, cmd_key, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, NULL, NULL, NULL, (BYTE *)current, &len) != ERROR_SUCCESS)
            current[0] = 0;
        RegCloseKey(k);
    }
    if (!wcscmp(current, command))
        return;
    // Another installation that still exists keeps the registration.
    if (current[0] == L'"') {
        wchar_t *end = wcschr(current + 1, L'"');
        if (end) {
            *end = 0;
            if (GetFileAttributesW(current + 1) != INVALID_FILE_ATTRIBUTES)
                return;
        }
    }
    wchar_t *root_key = wfmt(tmp, L"%ls\\nanumplayer", base);
    HKEY root;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, root_key, 0, NULL, 0, KEY_WRITE, NULL, &root,
                        NULL) != ERROR_SUCCESS)
        return;
    reg_set(root, NULL, L"URL:NanumPlayer");
    reg_set(root, L"URL Protocol", L"");
    RegCloseKey(root);
    wchar_t *icon_key = wfmt(tmp, L"%ls\\DefaultIcon", root_key);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, icon_key, 0, NULL, 0, KEY_WRITE, NULL, &k,
                        NULL) == ERROR_SUCCESS) {
        reg_set(k, NULL, wfmt(tmp, L"%ls,0", exe));
        RegCloseKey(k);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, cmd_key, 0, NULL, 0, KEY_WRITE, NULL, &k,
                        NULL) == ERROR_SUCCESS) {
        reg_set(k, NULL, command);
        RegCloseKey(k);
        MP_INFO(mpctx, "registered the nanumplayer:// protocol\n");
    }
}

static void add_opt(char **text, const char *name, const char *value)
{
    *text = talloc_asprintf_append(*text, "%s=%%%d%%%s\n", name, (int)strlen(value), value);
}

void nanum_open_startup(struct MPContext *mpctx)
{
    void *tmp = talloc_new(NULL);
    char *text = talloc_strdup(tmp, "");
    add_opt(&text, "ytdl-format", YTDL_FORMAT);
    // Only the chosen video and audio stream: every other format of the video would be a
    // track of its own (and an AI input candidate) for nothing.
    add_opt(&text, "script-opts-append", "ytdl_hook-all_formats=no");
    // The first player listens on the default pipe; one that finds it taken (a second
    // window, or the user's own --input-ipc-server) does not.
    wchar_t *pipe = default_pipe(tmp);
    if (!pipe_exists(pipe))
        add_opt(&text, "input-ipc-server", mp_to_utf8(tmp, pipe));
    if (m_config_parse(mpctx->mconfig, "NanumPlayer open defaults", bstr0(text), NULL, 0) < 0)
        MP_ERR(mpctx, "Cannot apply the NanumPlayer open defaults.\n");
    register_protocol(mpctx, tmp);
    talloc_free(tmp);
}

// ---------------------------------------------------------------------------
// Per file

// <youtube dir>\cookies.txt (utf-8, talloc under ctx).
static char *cookies_path(void *ctx)
{
    wchar_t env[MAX_PATH];
    wchar_t *dir;
    DWORD n = GetEnvironmentVariableW(L"NANUMPLAYER_YOUTUBE_DIR", env, MP_ARRAY_SIZE(env));
    if (n && n < MP_ARRAY_SIZE(env)) {
        dir = wdup(ctx, env);
    } else {
        wchar_t base[MAX_PATH];
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MP_ARRAY_SIZE(base));
        if (!n || n >= MP_ARRAY_SIZE(base))
            return NULL;
        dir = wfmt(ctx, L"%ls\\NanumPlayer\\youtube", base);
    }
    wchar_t *file = wfmt(ctx, L"%ls\\cookies.txt", dir);
    if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES)
        return NULL;
    return mp_to_utf8(ctx, file);
}

void nanum_open_prepare_file(struct MPContext *mpctx, const char *url)
{
    char id[16];
    if (!nanum_youtube_id(url, id, sizeof(id)))
        return;
    void *tmp = talloc_new(NULL);
    // The library identifies the video by its id, not by the stream urls ytdl_hook opens.
    m_config_set_option_cli(mpctx->mconfig, bstr0("nanum-origin-url"), bstr0(url),
                            M_SETOPT_BACKUP);
    // yt-dlp reads the login cookies the YouTube window exported (never logged).
    char *cookies = cookies_path(tmp);
    if (cookies) {
        char *value = talloc_asprintf(tmp, "cookies=%s", cookies);
        m_config_set_option_cli(mpctx->mconfig, bstr0("ytdl-raw-options-append"),
                                bstr0(value), M_SETOPT_BACKUP);
    }
    talloc_free(tmp);
}
