/*
 * NanumPlayer: nothing about what was played is kept (Windows). See
 * nanum-private.h.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mpv_talloc.h"
#include "common/common.h"
#include "osdep/io.h"
#include "osdep/nanum-private.h"
#include "osdep/threads.h"

#define SESSION_PREFIX L"NanumPlayer-session-"

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;
static wchar_t *g_dir;          // malloc'd, NULL until created
static bool g_failed, g_atexit;

static wchar_t *wjoin(const wchar_t *a, const wchar_t *b)
{
    size_t n = wcslen(a) + wcslen(b) + 2;
    wchar_t *r = malloc(n * sizeof(wchar_t));
    if (r)
        swprintf(r, n, L"%ls\\%ls", a, b);
    return r;
}

static uint64_t ft_to_u64(FILETIME ft)
{
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// Deletes a file, or a directory with everything below it. Links are removed,
// never followed. Best effort: what cannot be deleted (still open) is left.
static void remove_tree(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES)
        return;
    if (a & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
    bool dir = a & FILE_ATTRIBUTE_DIRECTORY;
    if (!dir) {
        DeleteFileW(path);
        return;
    }
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t *pattern = wjoin(path, L"*");
        if (pattern) {
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW(pattern, &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                        continue;
                    wchar_t *child = wjoin(path, fd.cFileName);
                    if (child) {
                        remove_tree(child);
                        free(child);
                    }
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            free(pattern);
        }
    }
    RemoveDirectoryW(path);
}

// Deletes the entries of `dir` that match `pattern` (files and folders).
static void remove_matching(const wchar_t *dir, const wchar_t *pattern)
{
    wchar_t *full = wjoin(dir, pattern);
    if (!full)
        return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(full, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            wchar_t *child = wjoin(dir, fd.cFileName);
            if (child) {
                remove_tree(child);
                free(child);
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    free(full);
}

static wchar_t *temp_root(void)
{
    wchar_t buf[MAX_PATH + 2];
    DWORD n = GetTempPathW(MP_ARRAY_SIZE(buf), buf);
    if (!n || n >= MP_ARRAY_SIZE(buf))
        return NULL;
    while (n > 3 && (buf[n - 1] == L'\\' || buf[n - 1] == L'/'))
        buf[--n] = 0;
    return _wcsdup(buf);
}

static void session_remove_at_exit(void)
{
    mp_mutex_lock(&g_lock);
    if (g_dir) {
        remove_tree(g_dir);
        free(g_dir);
        g_dir = NULL;
    }
    g_failed = true;            // nothing is created anymore
    mp_mutex_unlock(&g_lock);
}

char *nanum_private_dir(void *ctx)
{
    char *out = NULL;
    mp_mutex_lock(&g_lock);
    if (!g_dir && !g_failed) {
        wchar_t *root = temp_root();
        FILETIME created, exited, kernel, user;
        if (root && GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            wchar_t name[96];
            swprintf(name, MP_ARRAY_SIZE(name), SESSION_PREFIX L"%lu-%lx%08lx",
                     (unsigned long)GetCurrentProcessId(),
                     (unsigned long)created.dwHighDateTime,
                     (unsigned long)created.dwLowDateTime);
            wchar_t *dir = wjoin(root, name);
            if (dir && (CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)) {
                g_dir = dir;
                if (!g_atexit) {
                    g_atexit = true;
                    atexit(session_remove_at_exit);
                }
            } else {
                free(dir);
                g_failed = true;
            }
        } else {
            g_failed = true;
        }
        free(root);
    }
    if (g_dir) {
        out = mp_to_utf8(ctx, g_dir);
        for (char *s = out; out && *s; s++) {
            if (*s == '\\')
                *s = '/';
        }
    }
    mp_mutex_unlock(&g_lock);
    return out;
}

void nanum_private_clear(void)
{
    mp_mutex_lock(&g_lock);
    if (g_dir)
        remove_matching(g_dir, L"*");
    mp_mutex_unlock(&g_lock);
}

// Whether the process of a session folder name still runs.
static bool session_alive(const wchar_t *name)
{
    const wchar_t *p = name + wcslen(SESSION_PREFIX);
    wchar_t *end;
    unsigned long pid = wcstoul(p, &end, 10);
    if (end == p || *end != L'-')
        return true;            // not ours, leave it
    p = end + 1;
    unsigned long long started = wcstoull(p, &end, 16);
    if (end == p || *end)
        return true;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return GetLastError() != ERROR_INVALID_PARAMETER;
    bool alive = false;
    FILETIME created, exited, kernel, user;
    DWORD code = 0;
    if (GetProcessTimes(h, &created, &exited, &kernel, &user) &&
        GetExitCodeProcess(h, &code))
        alive = code == STILL_ACTIVE && ft_to_u64(created) == started;
    else
        alive = true;           // cannot tell: leave it
    CloseHandle(h);
    return alive;
}

void nanum_private_sweep(void)
{
    wchar_t *root = temp_root();
    if (!root)
        return;
    wchar_t *pattern = wjoin(root, SESSION_PREFIX L"*");
    if (pattern) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pattern, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    session_alive(fd.cFileName))
                    continue;
                wchar_t *dir = wjoin(root, fd.cFileName);
                if (dir) {
                    remove_tree(dir);
                    free(dir);
                }
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        free(pattern);
    }
    free(root);
}

void nanum_private_purge_legacy(const char *settings_dir, bool keep_library,
                                bool keep_full, bool keep_cache, bool keep_log)
{
    if (settings_dir && settings_dir[0]) {
        wchar_t *dir = mp_from_utf8(NULL, settings_dir);
        if (dir) {
            if (!keep_log)
                remove_matching(dir, L"last-playback*.log");
            if (!keep_cache)
                remove_matching(dir, L"tr_cache.jsonl");
            talloc_free(dir);
        }
    }
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MP_ARRAY_SIZE(base));
    if (!n || n >= MP_ARRAY_SIZE(base))
        return;
    wchar_t *dir = wjoin(base, L"NanumPlayer");
    if (!dir)
        return;
    if (!keep_library)
        remove_matching(dir, L"library.sqlite*");
    if (!keep_full)
        remove_matching(dir, L"full");
    remove_matching(dir, L"cache");
    remove_matching(dir, L"dumps");
    free(dir);
}
