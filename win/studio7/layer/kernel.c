#include "layer.h"
#include <bcrypt.h>

NTSTATUS WINAPI GetRngInterface(LPCWSTR provider, void **functions, ULONG flags);

__attribute__((used)) static void *const g_started_before_us[] = {BCryptGenRandom, GetRngInterface};

void WINAPI S7Anchor(void) {
}

VOID WINAPI s7_GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    *low = *(ULONG_PTR *)((BYTE *)tib + 0x1478);
    *high = (ULONG_PTR)tib->StackBase;
}

BOOL WINAPI s7_PrefetchVirtualMemory(HANDLE process, ULONG_PTR count, PVOID ranges, ULONG flags) {
    (void)process;
    (void)count;
    (void)ranges;
    (void)flags;
    return TRUE;
}

static BOOL search_flags_missing(void) {
    static volatile LONG state;

    if (!state) state = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory") ? 1 : 2;
    return state == 2;
}

static BOOL from_webview2_loader(void *at) {
    HMODULE caller = NULL, loader = GetModuleHandleW(L"WebView2Loader.dll");

    return loader &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)at, &caller) &&
           caller == loader;
}

HMODULE WINAPI s7_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    WCHAR path[MAX_PATH];
    UINT length, i;

    if (flags != LOAD_LIBRARY_SEARCH_SYSTEM32 || file || !name || !name[0] || !search_flags_missing() ||
        !from_webview2_loader(__builtin_return_address(0)))
        return LoadLibraryExW(name, file, flags);
    for (i = 0; name[i]; i++)
        if (name[i] == L'\\' || name[i] == L'/' || name[i] == L':') return LoadLibraryExW(name, file, flags);
    length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length + 1 + i >= MAX_PATH) return LoadLibraryExW(name, file, flags);
    path[length] = L'\\';
    memcpy(path + length + 1, name, (i + 1) * sizeof(WCHAR));
    return LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        s7_time_init();
        s7_sync_init();
    }
    return TRUE;
}
