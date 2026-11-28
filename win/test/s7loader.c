#include <windows.h>

typedef HMODULE(WINAPI *LoadFn)(LPCWSTR, HANDLE, DWORD);

__declspec(dllexport) HMODULE WINAPI s7_load_as_loader(LPCWSTR name, DWORD flags) {
    LoadFn ours = (LoadFn)(void *)GetProcAddress(GetModuleHandleW(L"s7kernel.dll"), "LoadLibraryExW");
    HMODULE module;

    if (!ours) return NULL;
    module = ours(name, NULL, flags);
    __asm__ volatile("" ::: "memory");
    return module;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return TRUE;
}
