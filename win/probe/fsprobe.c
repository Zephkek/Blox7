/* does fs:0x28 work in 64-bit win7 threads? spoiler: only if you map the page yourself */
#include <windows.h>
#include <stdio.h>

unsigned __int64 read_fs(unsigned offset);

#define CANARY 0x5a17c0deb1a2f00dULL
#define THREADS 200

static volatile LONG g_ok, g_bad, g_nomap;

static LONG CALLBACK onfault(EXCEPTION_POINTERS *e) {
    printf("fault 0x%08lx at %p, address %p\n", e->ExceptionRecord->ExceptionCode,
           e->ExceptionRecord->ExceptionAddress, (void *)e->ExceptionRecord->ExceptionInformation[1]);
    ExitProcess(1);
}

#define FS_REGION_START 0xFF000000UL
#define FS_REGION_END   0xFFFF0000UL

static BOOL map_fs_page(void) {
    ULONG_PTR base = (ULONG)((ULONG_PTR)NtCurrentTeb() + 0x2000);
    BYTE *page;
    if (base < FS_REGION_START || base + 4096 > FS_REGION_END) return FALSE;
    page = VirtualAlloc((void *)base, 4096, MEM_COMMIT, PAGE_READWRITE);
    if (page != (BYTE *)base) return FALSE;
    *(unsigned __int64 *)(page + 0x28) = CANARY;
    return TRUE;
}

static DWORD WINAPI worker(LPVOID arg) {
    int i;
    (void)arg;
    if (!map_fs_page()) { InterlockedIncrement(&g_nomap); return 0; }
    for (i = 0; i < 2000; i++) {
        if (read_fs(0x28) != CANARY) { InterlockedIncrement(&g_bad); return 0; }
        if ((i & 63) == 0) Sleep(0);
    }
    InterlockedIncrement(&g_ok);
    return 0;
}

int main(void) {
    HANDLE t[THREADS];
    int i;
    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, onfault);
    if (!VirtualAlloc((void *)(ULONG_PTR)FS_REGION_START, FS_REGION_END - FS_REGION_START, MEM_RESERVE, PAGE_READWRITE))
        printf("could not reserve the fs region: %lu\n", GetLastError());
    printf("main TEB %p -> fs base %08lx\n", NtCurrentTeb(), (ULONG)((ULONG_PTR)NtCurrentTeb() + 0x2000));
    for (i = 0; i < THREADS; i++) t[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    for (i = 0; i < THREADS; i += 64) WaitForMultipleObjects(min(64, THREADS - i), t + i, TRUE, INFINITE);
    worker(NULL);
    printf("threads ok %ld, wrong value %ld, could not map %ld\n", g_ok, g_bad, g_nomap);
    return 0;
}
