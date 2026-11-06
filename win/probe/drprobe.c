#include <windows.h>
#include <stdio.h>

static volatile LONG g_hits;

__attribute__((noinline)) static void target(void) { __asm__ volatile("nop"); }

static LONG CALLBACK on_step(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && e->ContextRecord->Rip == (DWORD64)target) {
        InterlockedIncrement(&g_hits);
        e->ContextRecord->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static HANDLE g_go;

static DWORD WINAPI worker(LPVOID arg) {
    int i;
    if (arg) WaitForSingleObject(g_go, INFINITE);
    for (i = 0; i < 5; i++) target();
    return 0;
}

static BOOL arm(HANDLE thread, BOOL read_first) {
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (read_first && !GetThreadContext(thread, &c)) return FALSE;
    c.Dr0 = (DWORD64)target;
    c.Dr7 = 1;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    return SetThreadContext(thread, &c);
}

int main(void) {
    HANDLE t;
    BOOL ok;
    OSVERSIONINFOW v = {sizeof v};
    setvbuf(stdout, NULL, _IONBF, 0);
    GetVersionExW(&v);
    printf("windows %lu.%lu.%lu (no manifest: 8+ reports 6.2)\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    AddVectoredExceptionHandler(1, on_step);

    g_hits = 0;
    t = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
    ok = arm(t, FALSE);
    ResumeThread(t);
    WaitForSingleObject(t, INFINITE);
    printf("new suspended thread, set only:      SetThreadContext %d, hits %ld of 5\n", ok, g_hits);

    g_hits = 0;
    t = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
    ok = arm(t, TRUE);
    ResumeThread(t);
    WaitForSingleObject(t, INFINITE);
    printf("new suspended thread, get then set:  SetThreadContext %d, hits %ld of 5\n", ok, g_hits);

    g_hits = 0;
    g_go = CreateEventW(NULL, TRUE, FALSE, NULL);
    t = CreateThread(NULL, 0, worker, (LPVOID)1, 0, NULL);
    Sleep(50);
    SuspendThread(t);
    ok = arm(t, TRUE);
    ResumeThread(t);
    SetEvent(g_go);
    WaitForSingleObject(t, INFINITE);
    printf("running thread, suspend+get+set:     SetThreadContext %d, hits %ld of 5\n", ok, g_hits);
    return 0;
}
