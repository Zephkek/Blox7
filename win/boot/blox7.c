#include <windows.h>
#include <shellapi.h>

#include "boot.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    wchar_t exe[MAX_PATH], *slash, **argv;
    int argc;
    (void)instance; (void)previous; (void)command_line; (void)show;

    boot_prepare();
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) boot_fail(L"no module name", GetLastError());
    if (wcsncmp(exe, L"\\\\?\\", 4) == 0 && exe[5] == L':') memmove(exe, exe + 4, (wcslen(exe) - 3) * sizeof(wchar_t));
    slash = wcsrchr(exe, L'\\');
    if (!slash) boot_fail(L"odd module name", 0);
    slash[1] = 0;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) boot_fail(L"no command line", GetLastError());
    return boot_run(exe, argc, argv);
}
