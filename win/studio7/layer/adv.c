#include "layer.h"

typedef ULONG(WINAPI *EventSetInformationFn)(ULONGLONG, int, PVOID, ULONG);

ULONG WINAPI s7_EventSetInformation(ULONGLONG handle, int information, PVOID data, ULONG size) {
    static volatile LONG_PTR found;
    LONG_PTR real = found;

    if (!real) {
        HMODULE advapi = GetModuleHandleW(L"advapi32.dll");
        FARPROC function = advapi ? GetProcAddress(advapi, "EventSetInformation") : NULL;

        found = real = function ? (LONG_PTR)function : 1;
    }
    if (real == 1) return ERROR_NOT_SUPPORTED;
    return ((EventSetInformationFn)real)(handle, information, data, size);
}
