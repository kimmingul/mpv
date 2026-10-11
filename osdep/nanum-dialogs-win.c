/*
 * NanumPlayer: native Windows open dialogs. See nanum-dialogs.h.
 *
 * This file is part of mpv, distributed under the GNU General Public License
 * version 2 or later (see LICENSE.GPL).
 */

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <wchar.h>

#include "mpv/client.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/path_utils.h"
#include "options/m_property.h"
#include "player/command.h"
#include "osdep/io.h"
#include "osdep/nanum-dialogs.h"
#include "osdep/threads.h"
#include "player/client.h"
#include "player/core.h"

// Defined here (not taken from libuuid) so the build does not depend on which
// GUIDs the MinGW import libraries happen to carry.
static const GUID nd_CLSID_FileOpenDialog =
    {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
static const GUID nd_IID_IFileOpenDialog =
    {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
static const GUID nd_IID_IShellItem =
    {0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};
// Identify the dialogs to the shell, so each remembers its own last folder.
static const GUID nd_guid_files =
    {0x7a4b1e52, 0x3c0d, 0x4f6a, {0x9b, 0x8e, 0x2d, 0x5c, 0x71, 0xa0, 0xf3, 0xe4}};
static const GUID nd_guid_disc =
    {0xb2c6d3f1, 0x58a7, 0x4e09, {0x8d, 0x14, 0x6f, 0x0e, 0x9a, 0x7c, 0x5b, 0x21}};

#define MAX_DRIVES 26
#define ID_DRIVE_BASE 1000
#define ID_BROWSE 2000

static volatile LONG dialog_busy;

struct request {
    struct MPContext *mpctx;
    enum nanum_dialog kind;
    HWND owner;
};

struct drive {
    wchar_t root[4];        // "E:\"
    wchar_t label[64];
};

static char *to_utf8(const wchar_t *w)
{
    return mp_to_utf8(NULL, w);
}

static void message(HWND owner, const wchar_t *text)
{
    MessageBoxW(owner, text, L"NanumPlayer", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// Loading: through a regular client handle, so the selection takes the same
// `loadfile` path as a file given on the command line or dropped on the window.
// ---------------------------------------------------------------------------

static int run_loadfile(mpv_handle *h, const char *url, const char *flag,
                        const char *bluray_device)
{
    mpv_node args[5] = {0};
    int n = 0;
    args[n++] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = "loadfile"};
    args[n++] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = (char *)url};
    args[n++] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = (char *)flag};

    char *key = "bluray-device";
    mpv_node value = {.format = MPV_FORMAT_STRING, .u.string = (char *)bluray_device};
    mpv_node_list options = {.num = 1, .values = &value, .keys = &key};
    if (bluray_device) {
        args[n++] = (mpv_node){.format = MPV_FORMAT_INT64, .u.int64 = -1};
        args[n++] = (mpv_node){.format = MPV_FORMAT_NODE_MAP, .u.list = &options};
    }

    mpv_node_list list = {.num = n, .values = args};
    mpv_node cmd = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &list};
    mpv_node result = {0};
    int r = mpv_command_node(h, &cmd, &result);
    mpv_free_node_contents(&result);
    return r;
}

// Loads the files (first replaces the playlist), or the disc if `device` is set.
static void load_selection(struct MPContext *mpctx, char **files, int num,
                           const char *device)
{
    mpv_handle *h = mp_new_client(mpctx->clients, "nanum_dialog");
    if (!h)
        return;     // shutting down
    struct mp_log *log = mp_client_get_log(h);
    if (device) {
        int r = run_loadfile(h, "bd://longest", "replace", device);
        if (r < 0)
            mp_err(log, "cannot open the disc '%s': %s\n", device, mpv_error_string(r));
    } else {
        for (int i = 0; i < num; i++) {
            int r = run_loadfile(h, files[i], i ? "append" : "replace", NULL);
            if (r < 0)
                mp_err(log, "cannot load '%s': %s\n", files[i], mpv_error_string(r));
        }
    }
    mpv_destroy(h);
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

static bool create_dialog(IFileOpenDialog **dlg)
{
    return SUCCEEDED(CoCreateInstance(&nd_CLSID_FileOpenDialog, NULL,
                                      CLSCTX_INPROC_SERVER,
                                      &nd_IID_IFileOpenDialog, (void **)dlg));
}

static void set_start_folder(IFileOpenDialog *dlg, const wchar_t *path)
{
    IShellItem *folder = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(path, NULL, &nd_IID_IShellItem,
                                              (void **)&folder)))
    {
        IFileOpenDialog_SetFolder(dlg, folder);
        IShellItem_Release(folder);
    }
}

static char *item_path(IShellItem *item)
{
    wchar_t *w = NULL;
    if (FAILED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &w)))
        return NULL;
    char *r = to_utf8(w);
    CoTaskMemFree(w);
    return r;
}

// Returns the selected paths (talloc array of talloc strings under ctx).
static char **pick_files(void *ctx, HWND owner, int *num)
{
    *num = 0;
    int count_out = 0;
    IFileOpenDialog *dlg;
    if (!create_dialog(&dlg))
        return NULL;

    static const COMDLG_FILTERSPEC types[] = {
        {L"동영상 · 오디오 파일",
         L"*.mkv;*.mp4;*.m4v;*.mov;*.avi;*.wmv;*.flv;*.webm;*.ts;*.m2ts;*.mts;"
         L"*.mpg;*.mpeg;*.vob;*.ogv;*.3gp;*.mp3;*.flac;*.wav;*.ogg;*.opus;"
         L"*.m4a;*.aac;*.wma;*.ac3;*.dts;*.mka"},
        {L"재생 목록", L"*.m3u;*.m3u8;*.pls"},
        {L"모든 파일", L"*.*"},
    };

    DWORD flags = 0;
    IFileOpenDialog_GetOptions(dlg, &flags);
    IFileOpenDialog_SetOptions(dlg, flags | FOS_FORCEFILESYSTEM |
                               FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST |
                               FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, L"파일 열기");
    IFileOpenDialog_SetOkButtonLabel(dlg, L"열기");
    IFileOpenDialog_SetFileTypes(dlg, MP_ARRAY_SIZE(types), types);
    IFileOpenDialog_SetFileTypeIndex(dlg, 1);
    IFileOpenDialog_SetClientGuid(dlg, &nd_guid_files);

    char **files = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner))) {
        IShellItemArray *results = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResults(dlg, &results))) {
            DWORD count = 0;
            IShellItemArray_GetCount(results, &count);
            for (DWORD i = 0; i < count; i++) {
                IShellItem *item = NULL;
                if (FAILED(IShellItemArray_GetItemAt(results, i, &item)))
                    continue;
                char *p = item_path(item);
                IShellItem_Release(item);
                if (p)
                    MP_TARRAY_APPEND(ctx, files, count_out, talloc_steal(ctx, p));
            }
            IShellItemArray_Release(results);
        }
    }
    IFileOpenDialog_Release(dlg);
    *num = count_out;
    return files;
}

// Returns a selected folder, or NULL if cancelled.
static wchar_t *pick_folder(void *ctx, HWND owner, const wchar_t *start)
{
    IFileOpenDialog *dlg;
    if (!create_dialog(&dlg))
        return NULL;

    DWORD flags = 0;
    IFileOpenDialog_GetOptions(dlg, &flags);
    IFileOpenDialog_SetOptions(dlg, flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, L"Blu-ray 드라이브 또는 폴더 선택");
    IFileOpenDialog_SetOkButtonLabel(dlg, L"재생");
    IFileOpenDialog_SetClientGuid(dlg, &nd_guid_disc);
    if (start)
        set_start_folder(dlg, start);

    wchar_t *result = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            wchar_t *w = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &w))) {
                result = talloc_array(ctx, wchar_t, wcslen(w) + 1);
                wcscpy(result, w);
                CoTaskMemFree(w);
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dlg);
    return result;
}

// ---------------------------------------------------------------------------
// Blu-ray discovery
// ---------------------------------------------------------------------------

static bool is_directory(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Any local drive (optical, removable, a mounted disc image, a ripped fixed
// folder) whose root holds a BDMV tree. Network drives are skipped: probing
// them can stall.
static int find_bdmv_drives(struct drive *out, wchar_t *first_optical)
{
    first_optical[0] = 0;
    int n = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26 && n < MAX_DRIVES; i++) {
        if (!(mask & (1u << i)))
            continue;
        struct drive d = {.root = {L'A' + i, L':', L'\\', 0}};
        UINT type = GetDriveTypeW(d.root);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN || type == DRIVE_REMOTE)
            continue;
        if (type == DRIVE_CDROM && !first_optical[0])
            wcscpy(first_optical, d.root);

        wchar_t bdmv[16];
        swprintf(bdmv, MP_ARRAY_SIZE(bdmv), L"%ls" L"BDMV", d.root);
        if (!is_directory(bdmv))
            continue;
        if (!GetVolumeInformationW(d.root, d.label, MP_ARRAY_SIZE(d.label),
                                   NULL, NULL, NULL, NULL, 0))
            d.label[0] = 0;
        out[n++] = d;
    }
    return n;
}

typedef HRESULT (WINAPI *TaskDialogIndirectFn)(const TASKDIALOGCONFIG *, int *,
                                               int *, BOOL *);

// Asks which of several Blu-ray drives to play. Returns the index, -2 for
// "browse for a folder", -1 for cancel.
static int choose_drive(HWND owner, const struct drive *drives, int num)
{
    HMODULE comctl = LoadLibraryExW(L"comctl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    TaskDialogIndirectFn task_dialog = comctl
        ? (TaskDialogIndirectFn)(void *)GetProcAddress(comctl, "TaskDialogIndirect")
        : NULL;
    if (!task_dialog)
        return -2;

    wchar_t text[MAX_DRIVES][128];
    TASKDIALOG_BUTTON buttons[MAX_DRIVES + 1];
    for (int i = 0; i < num; i++) {
        swprintf(text[i], MP_ARRAY_SIZE(text[i]), L"%.2ls  %ls\nBlu-ray 디스크 (BDMV)",
                 drives[i].root, drives[i].label[0] ? drives[i].label : L"(이름 없음)");
        buttons[i] = (TASKDIALOG_BUTTON){ID_DRIVE_BASE + i, text[i]};
    }
    buttons[num] = (TASKDIALOG_BUTTON){ID_BROWSE, L"다른 폴더 선택…\nBDMV 폴더가 있는 위치를 직접 선택합니다"};

    TASKDIALOGCONFIG cfg = {
        .cbSize = sizeof(cfg),
        .hwndParent = owner,
        .dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW,
        .dwCommonButtons = TDCBF_CANCEL_BUTTON,
        .pszWindowTitle = L"NanumPlayer",
        .pszMainInstruction = L"재생할 Blu-ray를 선택하세요",
        .pszContent = L"BDMV 폴더가 있는 드라이브가 여러 개 있습니다.",
        .cButtons = num + 1,
        .pButtons = buttons,
    };
    int pressed = IDCANCEL;
    HRESULT hr = task_dialog(&cfg, &pressed, NULL, NULL);
    FreeLibrary(comctl);
    if (FAILED(hr))
        return -2;
    if (pressed == ID_BROWSE)
        return -2;
    if (pressed >= ID_DRIVE_BASE && pressed < ID_DRIVE_BASE + num)
        return pressed - ID_DRIVE_BASE;
    return -1;
}

// Turns a picked folder into a bluray-device value (UTF-8), or NULL if it holds
// no BDMV tree. A drive root stays "E:", the form the player always used.
static char *disc_device_from_folder(void *ctx, const wchar_t *folder)
{
    size_t len = wcslen(folder);
    wchar_t *dir = talloc_array(ctx, wchar_t, len + 16);
    wcscpy(dir, folder);
    while (len > 3 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/'))
        dir[--len] = 0;

    // The BDMV folder itself was picked: use its parent.
    const wchar_t *base = wcsrchr(dir, L'\\');
    base = base ? base + 1 : dir;
    if (!_wcsicmp(base, L"BDMV")) {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash && slash != dir) {
            *slash = 0;
            len = wcslen(dir);
            if (len == 2 && dir[1] == L':') {
                dir[2] = L'\\';
                dir[3] = 0;
                len = 3;
            }
        }
    }

    wchar_t *probe = talloc_array(ctx, wchar_t, len + 16);
    swprintf(probe, len + 16, L"%ls%lsBDMV", dir, dir[len - 1] == L'\\' ? L"" : L"\\");
    if (!is_directory(probe))
        return NULL;

    if (len == 3 && dir[1] == L':' && dir[2] == L'\\')
        dir[2] = 0;
    else if (len > 3 && dir[len - 1] == L'\\')
        dir[len - 1] = 0;
    return mp_to_utf8(ctx, dir);
}

static void open_disc(struct request *req, void *ctx)
{
    struct drive drives[MAX_DRIVES];
    wchar_t first_optical[4];
    int num = find_bdmv_drives(drives, first_optical);

    const wchar_t *start = first_optical[0] ? first_optical : NULL;
    if (num == 1) {
        char *device = to_utf8(drives[0].root);
        device[2] = 0;      // "E:\" -> "E:"
        load_selection(req->mpctx, NULL, 0, device);
        talloc_free(device);
        return;
    }
    if (num > 1) {
        int choice = choose_drive(req->owner, drives, num);
        if (choice == -1)
            return;
        if (choice >= 0) {
            char *device = to_utf8(drives[choice].root);
            device[2] = 0;
            load_selection(req->mpctx, NULL, 0, device);
            talloc_free(device);
            return;
        }
        start = drives[0].root;
    }

    wchar_t *folder = pick_folder(ctx, req->owner, start);
    if (!folder)
        return;
    char *device = disc_device_from_folder(ctx, folder);
    if (!device) {
        message(req->owner, L"선택한 위치에서 Blu-ray(BDMV) 폴더를 찾을 수 없습니다.\n"
                            L"디스크가 들어 있는 드라이브나 BDMV 폴더가 있는 폴더를 선택하세요.");
        return;
    }
    load_selection(req->mpctx, NULL, 0, device);
}

static MP_THREAD_VOID dialog_thread(void *p)
{
    struct request *req = p;
    mp_thread_set_name("nanum/dialog");

    bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
                                              COINIT_DISABLE_OLE1DDE));
    void *ctx = talloc_new(NULL);

    if (req->kind == NANUM_DIALOG_OPEN_FILE) {
        int num = 0;
        char **files = pick_files(ctx, req->owner, &num);
        if (files && num > 0)
            load_selection(req->mpctx, files, num, NULL);
    } else {
        open_disc(req, ctx);
    }

    talloc_free(ctx);
    if (com)
        CoUninitialize();
    talloc_free(req);
    InterlockedExchange(&dialog_busy, 0);
    MP_THREAD_RETURN();
}

bool nanum_dialog_start(struct MPContext *mpctx, enum nanum_dialog kind)
{
    if (InterlockedCompareExchange(&dialog_busy, 1, 0) != 0)
        return true;    // one dialog at a time; the open one is the answer

    struct request *req = talloc_zero(NULL, struct request);
    req->mpctx = mpctx;
    req->kind = kind;

    // The player window (if there is one) owns the dialog, so the dialog is
    // modal to it and centred on it.
    int64_t wid = 0;
    if (mp_property_do("window-id", M_PROPERTY_GET, &wid, mpctx) == M_PROPERTY_OK)
        req->owner = (HWND)(intptr_t)wid;

    mp_thread thread;
    if (mp_thread_create(&thread, dialog_thread, req)) {
        talloc_free(req);
        InterlockedExchange(&dialog_busy, 0);
        return false;
    }
    mp_thread_detach(thread);
    return true;
}
