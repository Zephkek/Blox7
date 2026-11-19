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

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        s7_time_init();
        s7_sync_init();
    }
    return TRUE;
}
