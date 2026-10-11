#include <windows.h>
#include <shellapi.h>

#include "common/common.h"
#include "osdep/io.h"
#include "osdep/terminal.h"
#include "osdep/main-fn.h"
#include "osdep/nanum-startup.h"

#ifndef HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION

#define HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION  1
#define HeapOptimizeResources ((HEAP_INFORMATION_CLASS)3)

typedef struct HEAP_OPTIMIZE_RESOURCES_INFORMATION {
    DWORD Version;
    DWORD Flags;
} HEAP_OPTIMIZE_RESOURCES_INFORMATION;

#endif

static bool is_valid_handle(HANDLE h)
{
    return h != INVALID_HANDLE_VALUE && h != NULL &&
           GetFileType(h) != FILE_TYPE_UNKNOWN;
}

static bool has_redirected_stdio(void)
{
    return is_valid_handle(GetStdHandle(STD_INPUT_HANDLE)) ||
           is_valid_handle(GetStdHandle(STD_OUTPUT_HANDLE)) ||
           is_valid_handle(GetStdHandle(STD_ERROR_HANDLE));
}

static void microsoft_nonsense(void)
{
    // stop Windows from showing all kinds of annoying error dialogs
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    // Enable heap corruption detection
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);

    // Allow heap cache optimization and memory decommit
    HEAP_OPTIMIZE_RESOURCES_INFORMATION heap_info = {
        .Version = HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION
    };
    HeapSetInformation(NULL, HeapOptimizeResources, &heap_info,
                       sizeof(heap_info));

    // Always use safe search paths for DLLs and other files, ie. never use the
    // current directory
    SetDllDirectoryW(L"");
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE |
                      BASE_SEARCH_PATH_PERMANENT);
}

int main(void)
{
    microsoft_nonsense();

    // NanumPlayer.exe is the player itself (no launcher process): connect the
    // Blu-ray decryption libraries before libbluray can look for them.
    nanum_startup_environment();

    // If the caller asked for console output by setting _started_from_console=yes
    // (NanumPlayer has no console wrapper), attach to its console and set up the
    // standard IO handles
    bool has_console = terminal_try_attach();

    // If NanumPlayer is started from Explorer, the Run dialog or the Start Menu,
    // it has no console and no standard IO handles. In this case enable the
    // pseudo-GUI profile (terminal=no). The NanumPlayer option defaults
    // (osc=no, idle, force-window, engine paths) are applied separately by
    // nanum_startup_defaults(), below config files and the command line.
    bool gui = !has_console && !has_redirected_stdio();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    int argv_len = 0;
    char **argv_u8 = NULL;

    // Build mpv's UTF-8 argv, and add the pseudo-GUI profile if necessary
    if (argc > 0 && argv[0])
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len, mp_to_utf8(argv_u8, argv[0]));
    if (gui) {
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len,
                         "--player-operation-mode=pseudo-gui");
    }
    for (int i = 1; i < argc; i++)
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len, mp_to_utf8(argv_u8, argv[i]));
    MP_TARRAY_APPEND(NULL, argv_u8, argv_len, NULL);

    int ret = mpv_main(argv_len - 1, argv_u8);

    talloc_free(argv_u8);
    return ret;
}

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hInstPrev, LPSTR cmdline, int cmdshow)
{
    return main();
}
