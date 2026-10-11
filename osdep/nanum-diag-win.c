/*
 * NanumPlayer: bounded normal / diagnostic logging (Windows). See nanum-diag.h.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <winioctl.h>
#include <tlhelp32.h>
#include <winevt.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mpv/client.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "common/msg_control.h"
#include "misc/node.h"
#include "misc/path_utils.h"
#include "options/m_property.h"
#include "player/command.h"
#include "player/core.h"
#include "osdep/io.h"
#include "osdep/nanum-diag.h"
#include "osdep/threads.h"
#include "osdep/timer.h"

#define DIAG_LOG_MAX_BYTES   (8 * 1024 * 1024)
#define DIAG_LOG_SOFT_BYTES  (6 * 1024 * 1024)   // above: only important lines
#define DIAG_LOG_KEEP        8
#define APP_LOG_MAX_BYTES    (8 * 1024 * 1024)
#define APP_LOG_KEEP         2
#define LINE_MAX_BYTES       1000
#define COLLECT_INTERVAL_MS  30000
#define EVENT_WINDOW_MS      120000
#define DIAG_EVENT_LIMIT     8
#define MAKEMKV_TAIL_BYTES   (32 * 1024)
#define MAKEMKV_MAX_LINES    24

// Disc modules that are also written to the application log down to debug
// while diagnostics are on.
#define DIAG_APP_LOG_MODULES "bd,bdmv,disc,stream"

static mp_static_mutex g_lock = MP_STATIC_MUTEX_INITIALIZER;
static atomic_bool g_enabled;

static struct {
    struct mpv_global *global;
    struct mp_log *log;
    char *dir;              // diagnostic log folder, NULL if unknown
    char *app_log;
    char *file;             // diagnostic log of this session, NULL until created
    HANDLE h;               // INVALID_HANDLE_VALUE while closed
    int64_t bytes;
    bool capped;
    int64_t dropped;
    char *makemkv_log;      // path announced by libmmbd, UTF-8
    ULONGLONG last_collect; // GetTickCount64() of the last collection
    bool collecting;
} g = {.h = INVALID_HANDLE_VALUE};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void w2u(const wchar_t *w, char *out, size_t size)
{
    if (!size)
        return;
    out[0] = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, NULL, NULL))
        out[size - 1] = 0;
}

void nanum_diag_wall_clock(char *out, size_t size)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, size, "%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth,
             t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

void nanum_diag_os_error_text(unsigned long code, char *out, size_t size)
{
    wchar_t w[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)code,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                             w, MP_ARRAY_SIZE(w), NULL);
    if (!n) {
        n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, (DWORD)code, 0, w, MP_ARRAY_SIZE(w), NULL);
    }
    if (!n) {
        snprintf(out, size, "error %lu", code);
        return;
    }
    while (n && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L' ' ||
                 w[n - 1] == L'.'))
        w[--n] = 0;
    w2u(w, out, size);
}

// Product/file version of a file ("a.b.c.d"), "" if unavailable. version.dll is
// loaded on demand from the system folder.
static void file_version(const wchar_t *path, char *out, size_t size)
{
    out[0] = 0;
    HMODULE v = LoadLibraryExW(L"version.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!v)
        return;
    typedef DWORD (WINAPI *size_fn)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *info_fn)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL (WINAPI *query_fn)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
    size_fn get_size = (size_fn)(void *)GetProcAddress(v, "GetFileVersionInfoSizeW");
    info_fn get_info = (info_fn)(void *)GetProcAddress(v, "GetFileVersionInfoW");
    query_fn query = (query_fn)(void *)GetProcAddress(v, "VerQueryValueW");
    if (get_size && get_info && query) {
        DWORD ignored;
        DWORD len = get_size(path, &ignored);
        void *data = len ? malloc(len) : NULL;
        if (data && get_info(path, 0, len, data)) {
            VS_FIXEDFILEINFO *fi = NULL;
            UINT fl = 0;
            if (query(data, L"\\", (LPVOID *)&fi, &fl) && fi && fl >= sizeof(*fi)) {
                snprintf(out, size, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS),
                         LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                         LOWORD(fi->dwFileVersionLS));
            }
        }
        free(data);
    }
    FreeLibrary(v);
}

// ---------------------------------------------------------------------------
// The diagnostic log file
// ---------------------------------------------------------------------------

static int cmp_name_desc(const void *a, const void *b)
{
    return wcscmp((const wchar_t *)b, (const wchar_t *)a);
}

// Keeps the newest `keep` diagnostic logs of the folder. The names contain
// their creation time, so name order is age order.
static void prune_logs(const wchar_t *wdir, int keep)
{
    enum { MAX_NAMES = 256, NAME_LEN = 64 };
    wchar_t *pattern = _wcsdup(wdir);
    if (!pattern)
        return;
    size_t plen = wcslen(pattern) + 16;
    pattern = realloc(pattern, plen * sizeof(wchar_t));
    wchar_t (*names)[NAME_LEN] = calloc(MAX_NAMES, sizeof(*names));
    if (!pattern || !names) {
        free(pattern);
        free(names);
        return;
    }
    wcscat(pattern, L"\\disc-*.log");

    int count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                wcslen(fd.cFileName) < NAME_LEN && count < MAX_NAMES)
                wcscpy(names[count++], fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    qsort(names, count, sizeof(*names), cmp_name_desc);
    for (int i = keep; i < count; i++) {
        size_t n = wcslen(wdir) + wcslen(names[i]) + 2;
        wchar_t *full = malloc(n * sizeof(wchar_t));
        if (full) {
            swprintf(full, n, L"%ls\\%ls", wdir, names[i]);
            DeleteFileW(full);
            free(full);
        }
    }
    free(pattern);
    free(names);
}

static void write_raw_locked(const char *text, size_t len)
{
    DWORD written;
    if (g.h != INVALID_HANDLE_VALUE && WriteFile(g.h, text, (DWORD)len, &written, NULL))
        g.bytes += written;
}

static void write_locked(bool important, const char *text, size_t len)
{
    if (g.h == INVALID_HANDLE_VALUE)
        return;
    int64_t limit = important ? DIAG_LOG_MAX_BYTES : DIAG_LOG_SOFT_BYTES;
    if (g.bytes + (int64_t)len > limit) {
        g.dropped++;
        if (!g.capped) {
            g.capped = true;
            static const char note[] =
                "-- size limit reached: further lines are dropped "
                "(failures and seek results are kept until the hard limit) --\n";
            write_raw_locked(note, sizeof(note) - 1);
        }
        if (g.bytes + (int64_t)len > DIAG_LOG_MAX_BYTES)
            return;
        if (!important)
            return;
    }
    write_raw_locked(text, len);
}

static void vlog(bool important, const char *fmt, va_list ap)
{
    if (!atomic_load(&g_enabled))
        return;
    char body[LINE_MAX_BYTES + 4];
    int n = vsnprintf(body, LINE_MAX_BYTES + 1, fmt, ap);
    if (n < 0)
        return;
    if (n > LINE_MAX_BYTES) {
        memcpy(body + LINE_MAX_BYTES - 3, "...", 4);
        n = LINE_MAX_BYTES;
    }
    for (int i = 0; i < n; i++) {
        if (body[i] == '\r' || body[i] == '\n')
            body[i] = ' ';
    }
    while (n > 0 && body[n - 1] == ' ')
        body[--n] = 0;

    char wall[40];
    nanum_diag_wall_clock(wall, sizeof(wall));
    char line[LINE_MAX_BYTES + 96];
    int len = snprintf(line, sizeof(line), "%s [%9.3f] [T%05lu] %s\n", wall,
                       mp_time_sec(), (unsigned long)GetCurrentThreadId(), body);
    if (len <= 0)
        return;
    if ((size_t)len >= sizeof(line))
        len = sizeof(line) - 1;

    mp_mutex_lock(&g_lock);
    write_locked(important, line, len);
    mp_mutex_unlock(&g_lock);
}

void nanum_diag_log(bool important, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(important, fmt, ap);
    va_end(ap);
}

bool nanum_diag_enabled(void)
{
    return atomic_load_explicit(&g_enabled, memory_order_relaxed);
}

static void exe_description(char *out, size_t size)
{
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MP_ARRAY_SIZE(exe));
    out[0] = 0;
    if (!n || n >= MP_ARRAY_SIZE(exe))
        return;
    char path[MAX_PATH * 3], ver[48];
    w2u(exe, path, sizeof(path));
    file_version(exe, ver, sizeof(ver));
    snprintf(out, size, "%s%s%s", path, ver[0] ? " " : "", ver);
}

// Creates the diagnostic log of this session (once). Called with g_lock held.
static bool open_log_locked(struct mp_log *log)
{
    if (g.h != INVALID_HANDLE_VALUE)
        return true;
    if (!g.dir) {
        mp_err(log, "Diagnostics: the settings folder is unknown, no log can be created.\n");
        return false;
    }

    mp_mkdirp(g.dir);
    wchar_t *wdir = mp_from_utf8(NULL, g.dir);
    prune_logs(wdir, DIAG_LOG_KEEP - 1);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[96];
    snprintf(name, sizeof(name), "disc-%04u%02u%02u-%02u%02u%02u-%lu.log", t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             (unsigned long)GetCurrentProcessId());
    char *path = talloc_asprintf(NULL, "%s\\%s", g.dir, name);
    wchar_t *wpath = mp_from_utf8(NULL, path);
    HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();
    talloc_free(wpath);
    talloc_free(wdir);
    if (h == INVALID_HANDLE_VALUE) {
        char text[256];
        nanum_diag_os_error_text(err, text, sizeof(text));
        mp_err(log, "Diagnostics: cannot create '%s': %s (%lu)\n", path, text,
               (unsigned long)err);
        talloc_free(path);
        return false;
    }

    g.h = h;
    g.bytes = 0;
    g.capped = false;
    g.dropped = 0;
    talloc_free(g.file);
    g.file = path;

    char wall[40], exe[MAX_PATH * 3 + 64];
    nanum_diag_wall_clock(wall, sizeof(wall));
    exe_description(exe, sizeof(exe));
    char head[MAX_PATH * 4 + 256];
    int n = snprintf(head, sizeof(head),
        "NanumPlayer diagnostic log (disc I/O). Local time %s = player clock %.3f s.\n"
        "Lines: <local time> [<player clock, same as the application log>] [T<thread>] text.\n"
        "Application: %s (pid %lu)\n"
        "Application log: %s\n"
        "Read errors carry the Win32 error captured by the failing ReadFile; the drive's\n"
        "own sense data is not exposed by Win32 file I/O (see the event log / MakeMKV\n"
        "lines collected after a failure). Size limit %d MiB; the newest %d logs are kept.\n\n",
        wall, mp_time_sec(), exe, (unsigned long)GetCurrentProcessId(),
        g.app_log ? g.app_log : "?", DIAG_LOG_MAX_BYTES / (1024 * 1024),
        DIAG_LOG_KEEP);
    if (n > 0)
        write_raw_locked(head, (size_t)MPMIN((size_t)n, sizeof(head) - 1));
    return true;
}

// ---------------------------------------------------------------------------
// Mode, property, command
// ---------------------------------------------------------------------------

static bool apply_mode(struct MPContext *mpctx, enum nanum_diag_mode mode, bool publish)
{
    bool on = atomic_load(&g_enabled);
    bool want = mode == NANUM_DIAG_TOGGLE ? !on : mode == NANUM_DIAG_ON;
    if (want != on) {
        if (want) {
            mp_mutex_lock(&g_lock);
            bool ok = open_log_locked(mpctx->log);
            char *file = ok ? talloc_strdup(NULL, g.file) : NULL;
            mp_mutex_unlock(&g_lock);
            if (!ok)
                return false;
            atomic_store(&g_enabled, true);
            mp_msg_set_log_diagnostics(mpctx->global, true);
            nanum_diag_log(true, "diagnostics ON");
            mp_msg(mpctx->log, MSGL_INFO,
                   "Diagnostics on: disc I/O details go to %s\n", file);
            talloc_free(file);
        } else {
            mp_mutex_lock(&g_lock);
            int64_t dropped = g.dropped;
            mp_mutex_unlock(&g_lock);
            nanum_diag_log(true, "diagnostics OFF, %" PRId64
                           " lines were dropped by the size limit", dropped);
            atomic_store(&g_enabled, false);
            mp_msg_set_log_diagnostics(mpctx->global, false);
            mp_msg(mpctx->log, MSGL_INFO, "Diagnostics off.\n");
        }
    }
    if (publish)
        nanum_diag_publish(mpctx);
    return true;
}

void nanum_diag_startup(struct MPContext *mpctx, const char *settings_dir,
                        const char *app_log)
{
    mp_mutex_lock(&g_lock);
    g.global = mpctx->global;
    g.log = mpctx->log;
    talloc_free(g.dir);
    g.dir = settings_dir ? talloc_asprintf(NULL, "%s\\diagnostics", settings_dir) : NULL;
    talloc_free(g.app_log);
    g.app_log = app_log ? talloc_strdup(NULL, app_log) : NULL;
    mp_mutex_unlock(&g_lock);

    // The application's own log: info and above, bounded. A --log-file chosen
    // by the user is not touched.
    if (app_log) {
        mp_msg_set_log_policy(mpctx->global, app_log, MSGL_INFO, DIAG_APP_LOG_MODULES,
                              APP_LOG_MAX_BYTES, APP_LOG_KEEP);
    }

    const char *env = getenv("NANUMPLAYER_DIAGNOSTICS");
    if (env && env[0] && strcmp(env, "0"))
        apply_mode(mpctx, NANUM_DIAG_ON, false);
}

bool nanum_diag_command(struct MPContext *mpctx, enum nanum_diag_mode mode)
{
    return apply_mode(mpctx, mode, true);
}

void nanum_diag_publish(struct MPContext *mpctx)
{
    mp_mutex_lock(&g_lock);
    char *file = talloc_strdup(NULL, g.file ? g.file : "");
    char *dir = talloc_strdup(NULL, g.dir ? g.dir : "");
    char *app = talloc_strdup(NULL, g.app_log ? g.app_log : "");
    mp_mutex_unlock(&g_lock);

    mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_flag(&root, "enabled", nanum_diag_enabled());
    node_map_add_string(&root, "file", file);
    node_map_add_string(&root, "directory", dir);
    node_map_add_string(&root, "normal_log", app);
    node_map_add_int64(&root, "max_bytes", DIAG_LOG_MAX_BYTES);
    node_map_add_int64(&root, "keep", DIAG_LOG_KEEP);
    node_map_add_int64(&root, "slow_read_ms", nanum_diag_enabled()
                       ? NANUM_DIAG_SLOW_READ_MS_DIAG : NANUM_DIAG_SLOW_READ_MS);
    node_map_add_int64(&root, "slow_seek_ms", nanum_diag_enabled() ? 0
                       : NANUM_DIAG_SLOW_SEEK_MS);
    mp_property_do("user-data/nanum/diagnostics", M_PROPERTY_SET_NODE, &root, mpctx);
    talloc_free(root.u.list);
    talloc_free(file);
    talloc_free(dir);
    talloc_free(app);
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

// libbluray's debug stream, see stream_bluray.c.
void nanum_diag_libbluray_message(const char *msg)
{
    static const char marker[] = "log will be saved as file://";
    const char *p = strstr(msg, marker);
    if (p) {
        p += sizeof(marker) - 1;
        size_t len = strcspn(p, "\r\n");
        char *path = talloc_strndup(NULL, p, len);
        mp_mutex_lock(&g_lock);
        talloc_free(g.makemkv_log);
        g.makemkv_log = path;
        mp_mutex_unlock(&g_lock);
    }
    // The HDMV/BD-J virtual machine traces are not disc I/O.
    if (nanum_diag_enabled() && !strstr(msg, "/hdmv/") && !strstr(msg, "/bdj/"))
        nanum_diag_log(false, "libbluray: %s", msg);
}

static bool is_wanted_module(const wchar_t *name)
{
    static const wchar_t *const prefix[] = {
        L"libbluray", L"libaacs", L"libmmbd", L"libbdplus", L"libudfread",
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(prefix); i++) {
        if (!_wcsnicmp(name, prefix[i], wcslen(prefix[i])))
            return true;
    }
    return false;
}

// "name version" of every loaded libbluray/AACS/BD+ library, ", " separated.
static char *describe_modules(void *ctx)
{
    char *brief = talloc_strdup(ctx, "");

    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 5; tries++) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                        GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH)
            break;
    }
    if (snap == INVALID_HANDLE_VALUE)
        return talloc_strdup(ctx, "(module list unavailable)");

    MODULEENTRY32W me = {.dwSize = sizeof(me)};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (!is_wanted_module(me.szModule))
            continue;
        char name[MAX_PATH * 3], path[MAX_PATH * 3], ver[48];
        w2u(me.szModule, name, sizeof(name));
        w2u(me.szExePath, path, sizeof(path));
        file_version(me.szExePath, ver, sizeof(ver));
        brief = talloc_asprintf_append(brief, "%s%s%s%s", brief[0] ? ", " : "", name,
                                       ver[0] ? " " : "", ver);
    }
    CloseHandle(snap);
    if (!brief[0])
        brief = talloc_strdup(ctx, "(none loaded)");
    return brief;
}

// The drive letter a device string refers to: "E:", "E:\", "\\.\E:" -> 'E'.
static wchar_t drive_letter(const char *device)
{
    if (!strncmp(device, "\\\\.\\", 4))
        device += 4;
    if (isalpha((unsigned char)device[0]) && device[1] == ':' &&
        (!device[2] || ((device[2] == '\\' || device[2] == '/') && !device[3])))
        return (wchar_t)toupper((unsigned char)device[0]);
    return 0;
}

static void trim_field(const char *base, DWORD off, char *out, size_t size)
{
    out[0] = 0;
    if (!off)
        return;
    snprintf(out, size, "%s", base + off);
    for (size_t n = strlen(out); n && out[n - 1] == ' '; n--)
        out[n - 1] = 0;
}

// Drive model/firmware of the volume, via a query-only handle (no media access).
static char *describe_drive(void *ctx, const char *device)
{
    wchar_t letter = drive_letter(device);
    if (!letter)
        return talloc_asprintf(ctx, "not a drive letter (folder or image file): %s", device);

    wchar_t root[4] = {letter, L':', L'\\', 0};
    wchar_t dev[8] = {L'\\', L'\\', L'.', L'\\', letter, L':', 0};
    UINT type = GetDriveTypeW(root);
    const char *type_name = type == DRIVE_CDROM ? "optical drive" :
                            type == DRIVE_FIXED ? "fixed disk" :
                            type == DRIVE_REMOVABLE ? "removable" :
                            type == DRIVE_REMOTE ? "network" : "unknown type";

    char model[256] = "";
    HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q = {.PropertyId = StorageDeviceProperty,
                                    .QueryType = PropertyStandardQuery};
        char buf[1024] = {0};
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf,
                            sizeof(buf), &got, NULL) &&
            got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
            char vendor[64], product[128], rev[64];
            trim_field(buf, d->VendorIdOffset < got ? d->VendorIdOffset : 0, vendor, sizeof(vendor));
            trim_field(buf, d->ProductIdOffset < got ? d->ProductIdOffset : 0, product, sizeof(product));
            trim_field(buf, d->ProductRevisionOffset < got ? d->ProductRevisionOffset : 0, rev, sizeof(rev));
            snprintf(model, sizeof(model), "%s%s%s, firmware %s, bus type %u", vendor,
                     vendor[0] ? " " : "", product, rev[0] ? rev : "?",
                     (unsigned)d->BusType);
        }
        CloseHandle(h);
    }
    return talloc_asprintf(ctx, "%ls (%s)%s%s", root, type_name, model[0] ? ": " : "",
                           model[0] ? model : ": model not readable");
}

void nanum_diag_describe_bluray(struct mp_log *log, const char *device,
                                const char *libbluray_version)
{
    void *tmp = talloc_new(NULL);
    char *modules = describe_modules(tmp);
    char *drive = describe_drive(tmp, device);

    // One concise line for the application log: enough to tell setups apart.
    mp_msg(log, MSGL_INFO, "Blu-ray environment: libbluray %s; %s; %s\n",
           libbluray_version, modules, drive);

    if (nanum_diag_enabled()) {
        nanum_diag_log(true, "env: device=%s", device);
        nanum_diag_log(true, "env: drive=%s", drive);
        nanum_diag_log(true, "env: libbluray runtime version %s", libbluray_version);
        nanum_diag_log(true, "env: libraries=%s", modules);

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me = {.dwSize = sizeof(me)};
            for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
                if (!is_wanted_module(me.szModule))
                    continue;
                char path[MAX_PATH * 3];
                w2u(me.szExePath, path, sizeof(path));
                nanum_diag_log(true, "env: loaded %s", path);
            }
            CloseHandle(snap);
        }
        static const char *const vars[] = {"LIBAACS_PATH", "LIBBDPLUS_PATH"};
        for (int i = 0; i < MP_ARRAY_SIZE(vars); i++) {
            const char *v = getenv(vars[i]);
            nanum_diag_log(true, "env: %s=%s", vars[i], v ? v : "(not set)");
        }
        mp_mutex_lock(&g_lock);
        char *mm = g.makemkv_log ? talloc_strdup(tmp, g.makemkv_log) : NULL;
        mp_mutex_unlock(&g_lock);
        nanum_diag_log(true, "env: MakeMKV (libmmbd) debug log=%s", mm ? mm : "(not announced)");
    }
    talloc_free(tmp);
}

// ---------------------------------------------------------------------------
// Failure context: System event log + MakeMKV log tail
// ---------------------------------------------------------------------------

static bool xml_text(const char *xml, const char *open_tag, char *out, size_t size)
{
    out[0] = 0;
    const char *p = strstr(xml, open_tag);
    if (!p)
        return false;
    p = strchr(p, '>');
    if (!p)
        return false;
    p++;
    size_t len = strcspn(p, "<");
    snprintf(out, size, "%.*s", (int)MPMIN(len, size - 1), p);
    return true;
}

static bool xml_attr(const char *xml, const char *open_tag, const char *attr,
                     char *out, size_t size)
{
    out[0] = 0;
    const char *p = strstr(xml, open_tag);
    if (!p)
        return false;
    const char *end = strchr(p, '>');
    p = strstr(p, attr);
    if (!p || (end && p > end))
        return false;
    p += strlen(attr);
    if (*p != '=')
        return false;
    char q = p[1];
    if (q != '\'' && q != '"')
        return false;
    p += 2;
    size_t len = strcspn(p, q == '"' ? "\"" : "'");
    snprintf(out, size, "%.*s", (int)MPMIN(len, size - 1), p);
    return true;
}

static void collect_event_log(void)
{
    HMODULE m = LoadLibraryExW(L"wevtapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) {
        nanum_diag_log(true, "event-log: wevtapi.dll not available");
        return;
    }
    typedef EVT_HANDLE (WINAPI *query_fn)(EVT_HANDLE, LPCWSTR, LPCWSTR, DWORD);
    typedef BOOL (WINAPI *next_fn)(EVT_HANDLE, DWORD, EVT_HANDLE *, DWORD, DWORD, PDWORD);
    typedef BOOL (WINAPI *render_fn)(EVT_HANDLE, EVT_HANDLE, DWORD, DWORD, PVOID, PDWORD, PDWORD);
    typedef BOOL (WINAPI *close_fn)(EVT_HANDLE);
    query_fn query = (query_fn)(void *)GetProcAddress(m, "EvtQuery");
    next_fn next = (next_fn)(void *)GetProcAddress(m, "EvtNext");
    render_fn render = (render_fn)(void *)GetProcAddress(m, "EvtRender");
    close_fn evt_close = (close_fn)(void *)GetProcAddress(m, "EvtClose");
    if (!query || !next || !render || !evt_close) {
        FreeLibrary(m);
        return;
    }

    wchar_t xpath[768];
    swprintf(xpath, MP_ARRAY_SIZE(xpath),
             L"*[System[(Provider[@Name='cdrom'] or Provider[@Name='Disk'] or "
             L"Provider[@Name='atapi'] or Provider[@Name='storahci'] or "
             L"Provider[@Name='stornvme'] or Provider[@Name='storport'] or "
             L"Provider[@Name='usbstor'] or Provider[@Name='udfs']) and "
             L"TimeCreated[timediff(@SystemTime) <= %d]]]", EVENT_WINDOW_MS);

    EVT_HANDLE q = query(NULL, L"System", xpath,
                         EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q) {
        char text[200];
        DWORD err = GetLastError();
        nanum_diag_os_error_text(err, text, sizeof(text));
        nanum_diag_log(true, "event-log: System log not readable: %s (%lu)", text,
                       (unsigned long)err);
        FreeLibrary(m);
        return;
    }

    int shown = 0;
    EVT_HANDLE ev[DIAG_EVENT_LIMIT];
    DWORD returned = 0;
    if (next(q, DIAG_EVENT_LIMIT, ev, 1000, 0, &returned) && returned) {
        for (DWORD i = 0; i < returned; i++) {
            DWORD used = 0, props = 0;
            render(NULL, ev[i], EvtRenderEventXml, 0, NULL, &used, &props);
            if (used && used <= 32768) {
                wchar_t *w = malloc(used + sizeof(wchar_t));
                if (w && render(NULL, ev[i], EvtRenderEventXml, used, w, &used, &props)) {
                    size_t cap = (size_t)used * 2 + 8;
                    char *xml = malloc(cap);
                    if (xml) {
                        w2u(w, xml, cap);
                        char prov[64], id[16], level[8], when[40], data[220] = "";
                        xml_attr(xml, "<Provider", "Name", prov, sizeof(prov));
                        xml_text(xml, "<EventID", id, sizeof(id));
                        xml_text(xml, "<Level>", level, sizeof(level));
                        xml_attr(xml, "<TimeCreated", "SystemTime", when, sizeof(when));
                        const char *d = xml;
                        for (int k = 0; k < 4 && (d = strstr(d, "<Data")); k++) {
                            char v[96];
                            if (xml_text(d, "<Data", v, sizeof(v)) && v[0]) {
                                size_t have = strlen(data);
                                snprintf(data + have, sizeof(data) - have, "%s%s",
                                         have ? " | " : "", v);
                            }
                            d += 5;
                        }
                        nanum_diag_log(true, "event-log: %s id=%s level=%s time=%s(UTC) data=[%s]",
                                       prov, id, level, when, data);
                        shown++;
                        free(xml);
                    }
                }
                free(w);
            }
            evt_close(ev[i]);
        }
    }
    if (!shown)
        nanum_diag_log(true, "event-log: no cdrom/Disk/atapi/storahci/udfs events in the last %d s",
                       EVENT_WINDOW_MS / 1000);
    evt_close(q);
    FreeLibrary(m);
}

static bool has_long_hex(const char *s, int run)
{
    int n = 0;
    for (; *s; s++) {
        n = isxdigit((unsigned char)*s) ? n + 1 : 0;
        if (n >= run)
            return true;
    }
    return false;
}

static bool contains_ci(const char *s, const char *needle)
{
    size_t nl = strlen(needle);
    for (; *s; s++) {
        if (!_strnicmp(s, needle, nl))
            return true;
    }
    return false;
}

// Only drive/I-O error context leaves the MakeMKV log: anything that can carry
// keys, certificates or authentication data, and any long hex dump, is dropped.
static bool makemkv_line_ok(const char *line)
{
    static const char *const deny[] = {
        "key", "vuk", "cert", "nonce", "secret", "password", "auth", "pmsn",
        "mkb", "binding", "token", "serial", "challenge", "signature",
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(deny); i++) {
        if (contains_ci(line, deny[i]))
            return false;
    }
    if (has_long_hex(line, 16))
        return false;
    static const char *const allow[] = {
        "scsi", "sense", "error", "fail", "retry", "timeout", "sector", "i/o",
        "medium", "read", "drive",
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(allow); i++) {
        if (contains_ci(line, allow[i]))
            return true;
    }
    return false;
}

static void collect_makemkv_tail(void)
{
    mp_mutex_lock(&g_lock);
    char *path = g.makemkv_log ? talloc_strdup(NULL, g.makemkv_log) : NULL;
    mp_mutex_unlock(&g_lock);
    if (!path) {
        nanum_diag_log(true, "makemkv-log: libmmbd announced no debug log (not in use, or logging off)");
        return;
    }
    wchar_t *wpath = mp_from_utf8(NULL, path);
    HANDLE h = CreateFileW(wpath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();
    talloc_free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        char text[200];
        nanum_diag_os_error_text(err, text, sizeof(text));
        nanum_diag_log(true, "makemkv-log: cannot open %s: %s (%lu)", path, text,
                       (unsigned long)err);
        talloc_free(path);
        return;
    }
    LARGE_INTEGER size;
    char *buf = malloc(MAKEMKV_TAIL_BYTES + 1);
    DWORD got = 0;
    if (buf && GetFileSizeEx(h, &size)) {
        LARGE_INTEGER off = {.QuadPart = size.QuadPart > MAKEMKV_TAIL_BYTES
                                         ? size.QuadPart - MAKEMKV_TAIL_BYTES : 0};
        if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
            ReadFile(h, buf, MAKEMKV_TAIL_BYTES, &got, NULL);
    }
    CloseHandle(h);
    if (!buf || !got) {
        nanum_diag_log(true, "makemkv-log: %s is empty or unreadable", path);
        free(buf);
        talloc_free(path);
        return;
    }
    buf[got] = 0;

    // Newest lines last; keep the last MAKEMKV_MAX_LINES that pass the filter.
    char *lines[MAKEMKV_MAX_LINES];
    int count = 0, first = 0;
    for (char *s = buf; *s;) {
        char *e = s + strcspn(s, "\r\n");
        char saved = *e;
        *e = 0;
        if (e != s && makemkv_line_ok(s)) {
            lines[(first + count) % MAKEMKV_MAX_LINES] = s;
            if (count < MAKEMKV_MAX_LINES)
                count++;
            else
                first = (first + 1) % MAKEMKV_MAX_LINES;
        }
        if (!saved)
            break;
        s = e + 1;
    }
    nanum_diag_log(true, "makemkv-log: %s, %d filtered lines of the last %lu bytes "
                   "(key/certificate/authentication lines and hex dumps are never copied)",
                   path, count, (unsigned long)got);
    for (int i = 0; i < count; i++) {
        char *l = lines[(first + i) % MAKEMKV_MAX_LINES];
        for (char *c = l; *c; c++) {
            if ((unsigned char)*c < 0x20)
                *c = ' ';
        }
        nanum_diag_log(true, "makemkv-log> %.220s", l);
    }
    free(buf);
    talloc_free(path);
}

static unsigned __stdcall collect_thread(void *arg)
{
    nanum_diag_log(true, "failure context: collecting event log and MakeMKV log tail");
    collect_event_log();
    collect_makemkv_tail();
    mp_mutex_lock(&g_lock);
    g.collecting = false;
    mp_mutex_unlock(&g_lock);
    return 0;
}

void nanum_diag_collect_failure_context(void)
{
    if (!nanum_diag_enabled())
        return;
    ULONGLONG now = GetTickCount64();
    mp_mutex_lock(&g_lock);
    bool run = !g.collecting && (!g.last_collect || now - g.last_collect >= COLLECT_INTERVAL_MS);
    if (run) {
        g.collecting = true;
        g.last_collect = now;
    }
    mp_mutex_unlock(&g_lock);
    if (!run)
        return;

    mp_thread t;
    if (mp_thread_create(&t, collect_thread, NULL)) {
        mp_mutex_lock(&g_lock);
        g.collecting = false;
        mp_mutex_unlock(&g_lock);
        return;
    }
    mp_thread_detach(t);
}
