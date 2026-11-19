#include "layer.h"

BOOL WINAPI s7_VerifyVersionInfoW(LPOSVERSIONINFOEXW info, DWORD mask, DWORDLONG conditions) {
    static const DWORD release = VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR;

    if (VerifyVersionInfoW(info, mask, conditions)) return TRUE;
    if (GetLastError() != ERROR_OLD_WIN_VERSION) return FALSE;
    if ((mask & VER_MAJORVERSION) && !(mask & ~release) && ((conditions >> 3) & 7) == VER_GREATER_EQUAL &&
        info->dwMajorVersion == 10 && (!(mask & VER_MINORVERSION) || !info->dwMinorVersion) &&
        (!(mask & VER_SERVICEPACKMAJOR) || !info->wServicePackMajor) &&
        (!(mask & VER_SERVICEPACKMINOR) || !info->wServicePackMinor))
        return TRUE;
    SetLastError(ERROR_OLD_WIN_VERSION);
    return FALSE;
}
