#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define WaitOnAddress sdk_WaitOnAddress
#define WakeByAddressSingle sdk_WakeByAddressSingle
#define WakeByAddressAll sdk_WakeByAddressAll
#define GetSystemTimePreciseAsFileTime sdk_GetSystemTimePreciseAsFileTime
#define GetCurrentThreadStackLimits sdk_GetCurrentThreadStackLimits
#define CreateFile2 sdk_CreateFile2
#define CopyFile2 sdk_CopyFile2
#define PrefetchVirtualMemory sdk_PrefetchVirtualMemory
#define GetDpiForWindow sdk_GetDpiForWindow
#define GetDpiForSystem sdk_GetDpiForSystem
#define GetSystemMetricsForDpi sdk_GetSystemMetricsForDpi
#define AdjustWindowRectExForDpi sdk_AdjustWindowRectExForDpi
#define GetCurrentInputMessageSource sdk_GetCurrentInputMessageSource
#define GetAddrInfoExCancel sdk_GetAddrInfoExCancel
#define ProcessPrng sdk_ProcessPrng
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#undef WaitOnAddress
#undef WakeByAddressSingle
#undef WakeByAddressAll
#undef GetSystemTimePreciseAsFileTime
#undef GetCurrentThreadStackLimits
#undef CreateFile2
#undef CopyFile2
#undef PrefetchVirtualMemory
#undef GetDpiForWindow
#undef GetDpiForSystem
#undef GetSystemMetricsForDpi
#undef AdjustWindowRectExForDpi
#undef GetCurrentInputMessageSource
#undef GetAddrInfoExCancel
#undef ProcessPrng
#include <process.h>
#include <stdio.h>
#include <string.h>

#define IMP __declspec(dllimport)
IMP BOOL WINAPI WaitOnAddress(volatile VOID *, PVOID, SIZE_T, DWORD);
IMP VOID WINAPI WakeByAddressSingle(PVOID);
IMP VOID WINAPI WakeByAddressAll(PVOID);
IMP VOID WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME);
IMP VOID WINAPI GetCurrentThreadStackLimits(PULONG_PTR, PULONG_PTR);
typedef struct { DWORD size, attributes, flags, qos; LPSECURITY_ATTRIBUTES security; HANDLE template_file; } CF2;
IMP HANDLE WINAPI CreateFile2(LPCWSTR, DWORD, DWORD, DWORD, CF2 *);
typedef struct { DWORD size, flags; BOOL *cancel; PVOID progress, context; } CP2;
IMP HRESULT WINAPI CopyFile2(LPCWSTR, LPCWSTR, CP2 *);
IMP BOOL WINAPI PrefetchVirtualMemory(HANDLE, ULONG_PTR, PVOID, ULONG);
IMP UINT WINAPI GetDpiForWindow(HWND);
IMP UINT WINAPI GetDpiForSystem(void);
IMP int WINAPI GetSystemMetricsForDpi(int, UINT);
IMP BOOL WINAPI AdjustWindowRectExForDpi(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef struct { DWORD device, origin; } IMS;
IMP BOOL WINAPI GetCurrentInputMessageSource(IMS *);
IMP INT WSAAPI GetAddrInfoExCancel(LPHANDLE);
IMP BOOL WINAPI ProcessPrng(PBYTE, SIZE_T);
typedef struct HS *HS;
typedef struct { void *a, *b, *c; } HSH;
IMP HRESULT WINAPI WindowsCreateString(const WCHAR *, UINT32, HS *);
IMP HRESULT WINAPI WindowsCreateStringReference(const WCHAR *, UINT32, HSH *, HS *);
IMP HRESULT WINAPI WindowsDeleteString(HS);
IMP HRESULT WINAPI WindowsDuplicateString(HS, HS *);
IMP UINT32 WINAPI WindowsGetStringLen(HS);
IMP const WCHAR *WINAPI WindowsGetStringRawBuffer(HS, UINT32 *);
IMP HRESULT WINAPI WindowsCompareStringOrdinal(HS, HS, INT32 *);
IMP HRESULT WINAPI RoInitialize(DWORD);
IMP void WINAPI RoUninitialize(void);
IMP HRESULT WINAPI RoActivateInstance(HS, void **);

static int passed, failed;

static void check(int ok, const char *what, const char *format, ...) {
    va_list args;

    if (ok) passed++; else failed++;
    printf("%s %-34s ", ok ? "ok  " : "FAIL", what);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static double now_ms(void) {
    LARGE_INTEGER f, c;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static volatile LONG g_turn;
static volatile LONG64 g_handoffs;
static LONG g_limit;

static unsigned __stdcall ping(void *arg) {
    LONG me = (LONG)(LONG_PTR)arg, other = 1 - me;

    for (;;) {
        LONG seen = g_turn;
        if (seen == 2) break;
        if (seen != me) {
            WaitOnAddress(&g_turn, &seen, sizeof(seen), INFINITE);
            continue;
        }
        if (InterlockedIncrement64(&g_handoffs) >= g_limit) {
            g_turn = 2;
            WakeByAddressAll((PVOID)&g_turn);
            break;
        }
        g_turn = other;
        WakeByAddressSingle((PVOID)&g_turn);
    }
    return 0;
}

static volatile LONG g_gate, g_released;

static unsigned __stdcall gated(void *arg) {
    LONG closed = 0;

    (void)arg;
    while (g_gate == 0) WaitOnAddress(&g_gate, &closed, sizeof(closed), INFINITE);
    InterlockedIncrement(&g_released);
    return 0;
}

#define WORKERS 8
static volatile LONG g_lock, g_counter_guarded;
static LONG g_rounds;

static void futex_lock(void) {
    LONG c;

    if ((c = InterlockedCompareExchange(&g_lock, 1, 0)) == 0) return;
    do {
        if (c == 2 || InterlockedCompareExchange(&g_lock, 2, 1) != 0) {
            LONG two = 2;
            WaitOnAddress(&g_lock, &two, sizeof(two), INFINITE);
        }
    } while ((c = InterlockedCompareExchange(&g_lock, 2, 0)) != 0);
}

static void futex_unlock(void) {
    if (InterlockedExchange(&g_lock, 0) == 2) WakeByAddressSingle((PVOID)&g_lock);
}

static unsigned __stdcall locker(void *arg) {
    LONG i;

    (void)arg;
    for (i = 0; i < g_rounds; i++) {
        futex_lock();
        g_counter_guarded = g_counter_guarded + 1;
        futex_unlock();
    }
    return 0;
}

static void test_wait(void) {
    HANDLE t[64];
    LONG value = 5, same = 5, other = 6;
    UCHAR b = 1, bsame = 1;
    ULONG64 q = 7, qsame = 7;
    double start, ms;
    BOOL r;
    DWORD e;
    int i;

    r = WaitOnAddress(&value, &other, sizeof(value), 1000);
    check(r, "wait: value already differs", "returned %d at once", r);
    start = now_ms();
    SetLastError(0);
    r = WaitOnAddress(&value, &same, sizeof(value), 50);
    e = GetLastError();
    ms = now_ms() - start;
    check(!r && e == ERROR_TIMEOUT && ms >= 40 && ms < 200, "wait: timeout", "returned %d error %lu after %.1f ms", r, e, ms);
    r = WaitOnAddress(&b, &bsame, 1, 10);
    check(!r, "wait: 1-byte value", "timed out as it should");
    r = WaitOnAddress(&q, &qsame, 8, 10);
    check(!r, "wait: 8-byte value", "timed out as it should");
    SetLastError(0);
    r = WaitOnAddress(&value, &same, 3, 10);
    check(!r && GetLastError() == ERROR_INVALID_PARAMETER, "wait: bad size", "error %lu", GetLastError());

    g_turn = 0;
    g_handoffs = 0;
    g_limit = 200000;
    start = now_ms();
    t[0] = (HANDLE)_beginthreadex(NULL, 0, ping, (void *)0, 0, NULL);
    t[1] = (HANDLE)_beginthreadex(NULL, 0, ping, (void *)1, 0, NULL);
    r = WaitForMultipleObjects(2, t, TRUE, 60000) == WAIT_OBJECT_0;
    ms = now_ms() - start;
    check(r && g_handoffs >= g_limit, "wait: two-thread hand-off", "%lld hand-offs in %.0f ms, %.2f us each",
          g_handoffs, ms, ms * 1000.0 / (double)g_handoffs);
    CloseHandle(t[0]);
    CloseHandle(t[1]);

    g_turn = 0;
    g_handoffs = 0;
    g_limit = 100000;
    t[0] = (HANDLE)_beginthreadex(NULL, 0, ping, (void *)0, CREATE_SUSPENDED, NULL);
    t[1] = (HANDLE)_beginthreadex(NULL, 0, ping, (void *)1, CREATE_SUSPENDED, NULL);
    SetThreadAffinityMask(t[0], 1);
    SetThreadAffinityMask(t[1], 1);
    start = now_ms();
    ResumeThread(t[0]);
    ResumeThread(t[1]);
    r = WaitForMultipleObjects(2, t, TRUE, 120000) == WAIT_OBJECT_0;
    ms = now_ms() - start;
    check(r && g_handoffs >= g_limit, "wait: hand-off on one core", "%lld hand-offs in %.0f ms, %.2f us each",
          g_handoffs, ms, ms * 1000.0 / (double)g_handoffs);
    CloseHandle(t[0]);
    CloseHandle(t[1]);

    g_gate = 0;
    g_released = 0;
    for (i = 0; i < 32; i++) t[i] = (HANDLE)_beginthreadex(NULL, 0, gated, NULL, 0, NULL);
    Sleep(200);
    g_gate = 1;
    WakeByAddressAll((PVOID)&g_gate);
    r = WaitForMultipleObjects(32, t, TRUE, 10000) == WAIT_OBJECT_0;
    check(r && g_released == 32, "wait: wake all", "%ld of 32 waiters released", g_released);
    for (i = 0; i < 32; i++) CloseHandle(t[i]);

    g_gate = 0;
    g_released = 0;
    for (i = 0; i < 4; i++) t[i] = (HANDLE)_beginthreadex(NULL, 0, gated, NULL, 0, NULL);
    Sleep(200);
    WakeByAddressSingle((PVOID)&g_gate);
    Sleep(100);
    check(g_released == 0, "wait: wake without change", "%ld waiters left (they re-check and wait again)", g_released);
    g_gate = 1;
    WakeByAddressSingle((PVOID)&g_gate);
    Sleep(200);
    check(g_released == 1, "wait: wake single", "%ld waiter released", g_released);
    WakeByAddressAll((PVOID)&g_gate);
    WaitForMultipleObjects(4, t, TRUE, 10000);
    for (i = 0; i < 4; i++) CloseHandle(t[i]);

    g_lock = 0;
    g_counter_guarded = 0;
    g_rounds = 200000;
    start = now_ms();
    for (i = 0; i < WORKERS; i++) t[i] = (HANDLE)_beginthreadex(NULL, 0, locker, NULL, 0, NULL);
    r = WaitForMultipleObjects(WORKERS, t, TRUE, 120000) == WAIT_OBJECT_0;
    ms = now_ms() - start;
    check(r && g_counter_guarded == WORKERS * g_rounds, "wait: 8-thread lock", "count %ld of %ld in %.0f ms",
          g_counter_guarded, WORKERS * g_rounds, ms);
    for (i = 0; i < WORKERS; i++) CloseHandle(t[i]);

    {
        FILETIME c0, e0, k0, u0, k1, u1;
        ULONG64 used;

        GetThreadTimes(GetCurrentThread(), &c0, &e0, &k0, &u0);
        start = now_ms();
        for (i = 0; i < 2000; i++) WaitOnAddress(&value, &same, sizeof(value), 1);
        ms = now_ms() - start;
        GetThreadTimes(GetCurrentThread(), &c0, &e0, &k1, &u1);
        used = (((ULONG64)k1.dwHighDateTime << 32) | k1.dwLowDateTime) + (((ULONG64)u1.dwHighDateTime << 32) | u1.dwLowDateTime) -
               (((ULONG64)k0.dwHighDateTime << 32) | k0.dwLowDateTime) - (((ULONG64)u0.dwHighDateTime << 32) | u0.dwLowDateTime);
        check(1, "wait: 2000 one-millisecond waits", "%.0f ms wall, %.1f us of CPU each", ms, (double)used / 10.0 / 2000.0);
    }
    start = now_ms();
    for (i = 0; i < 10000000; i++) WakeByAddressSingle(&value);
    ms = now_ms() - start;
    check(1, "wait: wake with nobody waiting", "%.1f ns each", ms * 0.1);
}

static volatile LONG g_time_bad;

static unsigned __stdcall time_reader(void *arg) {
    FILETIME ft;
    ULONG64 last = 0, t;
    int i;

    (void)arg;
    for (i = 0; i < 2000000; i++) {
        GetSystemTimePreciseAsFileTime(&ft);
        t = ((ULONG64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        if (t < last) InterlockedIncrement(&g_time_bad);
        last = t;
    }
    return 0;
}

static ULONG64 ticks(const FILETIME *t) {
    return ((ULONG64)t->dwHighDateTime << 32) | t->dwLowDateTime;
}

static void test_time(void) {
    FILETIME p, before, after;
    ULONG64 a, last = 0, distinct = 0;
    LONG64 worst_low = 0, worst_high = 0, low, high;
    HANDLE t[4];
    double start, ms;
    int i;

    for (i = 0; i < 200000; i++) {
        GetSystemTimeAsFileTime(&before);
        GetSystemTimePreciseAsFileTime(&p);
        GetSystemTimeAsFileTime(&after);
        a = ticks(&p);
        low = (LONG64)(a - ticks(&before));
        high = (LONG64)(a - ticks(&after));
        if (low < worst_low) worst_low = low;
        if (high > worst_high) worst_high = high;
        if (a != last) distinct++;
        last = a;
        if (i % 20000 == 0) Sleep(3);
    }
    check(worst_low >= 0 && worst_high < 170000, "time: stays within one tick of the clock",
          "never behind the clock by more than %.3f ms, never ahead by more than %.3f ms", -worst_low / 10000.0,
          worst_high / 10000.0);
    check(distinct > 10000, "time: resolution", "%llu distinct values in 200000 reads", distinct);
    for (i = 0; i < 4; i++) t[i] = (HANDLE)_beginthreadex(NULL, 0, time_reader, NULL, 0, NULL);
    WaitForMultipleObjects(4, t, TRUE, 120000);
    for (i = 0; i < 4; i++) CloseHandle(t[i]);
    check(g_time_bad == 0, "time: never steps back per thread", "%ld backward steps in 8000000 reads", g_time_bad);
    start = now_ms();
    for (i = 0; i < 5000000; i++) GetSystemTimePreciseAsFileTime(&p);
    ms = now_ms() - start;
    check(1, "time: cost", "%.1f ns per call", ms * 0.2);
    Sleep(1500);
    GetSystemTimeAsFileTime(&before);
    GetSystemTimePreciseAsFileTime(&p);
    GetSystemTimeAsFileTime(&after);
    low = (LONG64)(ticks(&p) - ticks(&before));
    high = (LONG64)(ticks(&p) - ticks(&after));
    check(low >= 0 && high < 170000, "time: after a sleep", "%.3f ms past the clock read before it", low / 10000.0);
}

static void test_stack(void) {
    ULONG_PTR low = 0, high = 0, here = (ULONG_PTR)&low;
    MEMORY_BASIC_INFORMATION mbi;

    GetCurrentThreadStackLimits(&low, &high);
    VirtualQuery(&low, &mbi, sizeof(mbi));
    check(low < here && here < high && low == (ULONG_PTR)mbi.AllocationBase, "stack limits",
          "%p..%p (%llu KB), allocation base %p", (void *)low, (void *)high, (unsigned long long)(high - low) / 1024,
          mbi.AllocationBase);
}

static int g_progress_calls;

static int CALLBACK copy_progress(const void *message, PVOID context) {
    (void)message;
    (void)context;
    g_progress_calls++;
    return 0;
}

static void test_files(void) {
    WCHAR dir[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    CF2 params;
    CP2 copy;
    HANDLE h;
    DWORD n = 0, i;
    static char data[300000], back[300000];
    HRESULT hr;
    char page[8192];
    struct { PVOID address; SIZE_T size; } range = {page, sizeof(page)};

    GetTempPathW(MAX_PATH, dir);
    wsprintfW(a, L"%ss7test-%lu-a.bin", dir, GetCurrentProcessId());
    wsprintfW(b, L"%ss7test-%lu-b.bin", dir, GetCurrentProcessId());
    for (i = 0; i < sizeof(data); i++) data[i] = (char)(i * 7 + (i >> 8));
    memset(&params, 0, sizeof(params));
    params.size = sizeof(params);
    params.attributes = FILE_ATTRIBUTE_NORMAL;
    params.flags = FILE_FLAG_SEQUENTIAL_SCAN;
    h = CreateFile2(a, GENERIC_WRITE, 0, CREATE_ALWAYS, &params);
    check(h != INVALID_HANDLE_VALUE, "CreateFile2: create", "handle %p error %lu", h, GetLastError());
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, data, sizeof(data), &n, NULL);
        CloseHandle(h);
    }
    h = CreateFile2(a, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    check(h != INVALID_HANDLE_VALUE && GetFileSize(h, NULL) == sizeof(data), "CreateFile2: no parameters", "size %lu",
          h != INVALID_HANDLE_VALUE ? GetFileSize(h, NULL) : 0);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = CreateFile2(L"C:\\no-such-folder-s7\\x.bin", GENERIC_READ, 0, OPEN_EXISTING, NULL);
    check(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND, "CreateFile2: missing path", "error %lu",
          GetLastError());
    memset(&copy, 0, sizeof(copy));
    copy.size = sizeof(copy);
    copy.progress = (PVOID)copy_progress;
    hr = CopyFile2(a, b, &copy);
    h = CreateFileW(b, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, back, sizeof(back), &n, NULL);
        CloseHandle(h);
    }
    check(hr == S_OK && n == sizeof(data) && !memcmp(data, back, sizeof(data)), "CopyFile2: copy",
          "hr %08lx, %lu bytes equal, %d progress calls", (unsigned long)hr, n, g_progress_calls);
    copy.flags = COPY_FILE_FAIL_IF_EXISTS;
    copy.progress = NULL;
    hr = CopyFile2(a, b, &copy);
    check(hr == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), "CopyFile2: fail if exists", "hr %08lx", (unsigned long)hr);
    hr = CopyFile2(a, b, NULL);
    check(hr == S_OK, "CopyFile2: no parameters", "hr %08lx", (unsigned long)hr);
    DeleteFileW(a);
    DeleteFileW(b);
    check(PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0), "PrefetchVirtualMemory", "accepted");
}

static int lookup(const WCHAR *name, int family, DWORD wait_ms, BOOL cancel, int *error, int *count) {
    ADDRINFOEXW hints, *found = NULL, *p;
    OVERLAPPED ov;
    HANDLE handle = NULL;
    DWORD waited;
    int r;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_flags = AI_ADDRCONFIG;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    r = GetAddrInfoExW(name, NULL, NS_ALL, NULL, &hints, &found, NULL, &ov, NULL, &handle);
    *count = 0;
    if (r != WSA_IO_PENDING) {
        *error = r;
        CloseHandle(ov.hEvent);
        if (found) FreeAddrInfoExW(found);
        return 0;
    }
    if (cancel) GetAddrInfoExCancel(&handle);
    waited = WaitForSingleObject(ov.hEvent, wait_ms);
    *error = waited == WAIT_OBJECT_0 ? (int)ov.Internal : -1;
    for (p = found; p; p = p->ai_next) (*count)++;
    if (found) FreeAddrInfoExW(found);
    CloseHandle(ov.hEvent);
    return 1;
}

static volatile LONG g_done;
static int g_routine_error = -2;

static void CALLBACK lookup_done(DWORD error, DWORD bytes, LPWSAOVERLAPPED ov) {
    (void)bytes;
    (void)ov;
    g_routine_error = (int)error;
    InterlockedExchange(&g_done, 1);
}

static void test_dns(void) {
    WSADATA wsa;
    ADDRINFOEXW hints, *found = NULL;
    OVERLAPPED ov;
    HANDLE handles[64], h;
    OVERLAPPED ovs[64];
    ADDRINFOEXW *results[64];
    int pending, error, count, i, r, good = 0;
    double start, ms;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    start = now_ms();
    pending = lookup(L"www.roblox.com", AF_UNSPEC, 15000, FALSE, &error, &count);
    ms = now_ms() - start;
    check(pending && error == 0 && count > 0, "lookup: pending, then answered", "pending %d error %d, %d address(es), %.0f ms",
          pending, error, count, ms);
    pending = lookup(L"localhost", AF_INET, 15000, FALSE, &error, &count);
    check(error == 0 && count > 0, "lookup: localhost", "pending %d error %d, %d address(es)", pending, error, count);
    pending = lookup(L"no-such-host-s7.invalid", AF_UNSPEC, 20000, FALSE, &error, &count);
    check(error == WSAHOST_NOT_FOUND || error == WSANO_DATA || error == WSATRY_AGAIN, "lookup: unknown name",
          "pending %d error %d", pending, error);
    start = now_ms();
    pending = lookup(L"slow-cancel-s7.invalid", AF_UNSPEC, 20000, TRUE, &error, &count);
    ms = now_ms() - start;
    check(pending && (error == WSA_E_CANCELLED || error == WSAHOST_NOT_FOUND), "lookup: cancel",
          "error %d after %.0f ms", error, ms);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    memset(&ov, 0, sizeof(ov));
    g_done = 0;
    h = NULL;
    r = GetAddrInfoExW(L"www.roblox.com", NULL, NS_ALL, NULL, &hints, &found, NULL, &ov, lookup_done, &h);
    for (i = 0; i < 1500 && !g_done; i++) SleepEx(10, TRUE);
    check(r == WSA_IO_PENDING && g_done && g_routine_error == 0 && found, "lookup: completion routine",
          "returned %d, routine error %d", r, g_routine_error);
    if (found) FreeAddrInfoExW(found);

    found = NULL;
    r = GetAddrInfoExW(L"www.roblox.com", NULL, NS_ALL, NULL, &hints, &found, NULL, NULL, NULL, NULL);
    check(r == 0 && found, "lookup: plain call", "returned %d", r);
    if (found) FreeAddrInfoExW(found);

    start = now_ms();
    for (i = 0; i < 64; i++) {
        memset(&ovs[i], 0, sizeof(ovs[i]));
        ovs[i].hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        results[i] = NULL;
        handles[i] = NULL;
        r = GetAddrInfoExW(i & 1 ? L"www.roblox.com" : L"setup.rbxcdn.com", NULL, NS_ALL, NULL, &hints, &results[i], NULL,
                           &ovs[i], NULL, &handles[i]);
        if (r != WSA_IO_PENDING && r != 0) ovs[i].Internal = (ULONG_PTR)r, SetEvent(ovs[i].hEvent);
        if (r == 0) SetEvent(ovs[i].hEvent);
    }
    for (i = 0; i < 64; i++) {
        if (WaitForSingleObject(ovs[i].hEvent, 30000) == WAIT_OBJECT_0 && ovs[i].Internal == 0 && results[i]) good++;
        if (results[i]) FreeAddrInfoExW(results[i]);
        CloseHandle(ovs[i].hEvent);
    }
    ms = now_ms() - start;
    check(good == 64, "lookup: 64 at once", "%d answered in %.0f ms", good, ms);
    h = (HANDLE)(ULONG_PTR)0x12345;
    r = GetAddrInfoExCancel(&h);
    check(r != 0, "lookup: cancel of a stale handle", "returned %d", r);
}

static void test_user(void) {
    RECT a = {100, 100, 500, 400}, b = {100, 100, 500, 400}, c = {100, 100, 500, 400};
    HWND desktop = GetDesktopWindow();
    UINT dpi = GetDpiForSystem(), wdpi = GetDpiForWindow(desktop);
    IMS source = {99, 99};
    BOOL r;

    check(dpi >= 96 && wdpi >= dpi, "dpi: system and window", "%u and %u", dpi, wdpi);
    SetLastError(0);
    check(GetDpiForWindow((HWND)(ULONG_PTR)0x7FFFFFF0) == 0 && GetLastError() == ERROR_INVALID_PARAMETER,
          "dpi: bad window", "error %lu", GetLastError());
    check(GetSystemMetricsForDpi(SM_CYCAPTION, wdpi) >= GetSystemMetrics(SM_CYCAPTION), "metrics: same dpi",
          "caption %d", GetSystemMetricsForDpi(SM_CYCAPTION, dpi));
    check(GetSystemMetricsForDpi(SM_CYCAPTION, dpi * 2) >= GetSystemMetrics(SM_CYCAPTION) * 2 - 2 &&
          GetSystemMetricsForDpi(SM_CMONITORS, dpi * 2) == GetSystemMetrics(SM_CMONITORS),
          "metrics: doubled dpi", "caption %d, monitors %d", GetSystemMetricsForDpi(SM_CYCAPTION, dpi * 2),
          GetSystemMetricsForDpi(SM_CMONITORS, dpi * 2));
    AdjustWindowRectEx(&a, WS_OVERLAPPEDWINDOW, TRUE, 0);
    AdjustWindowRectExForDpi(&b, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
    AdjustWindowRectExForDpi(&c, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi * 2);
    check(EqualRect(&a, &b) && c.top < b.top, "adjust rect", "frame top %ld at dpi %u, %ld at %u", 100 - b.top, dpi,
          100 - c.top, dpi * 2);
    r = GetCurrentInputMessageSource(&source);
    check(source.origin != 2, "input source", "returned %d, device %lu origin %lu", r, source.device, source.origin);
}

typedef BOOL (WINAPI *VerifyFn)(LPOSVERSIONINFOEXW, DWORD, DWORDLONG);

static BOOL at_least(VerifyFn verify, DWORD major, DWORD minor, DWORD build) {
    OSVERSIONINFOEXW v;
    DWORD mask = VER_MAJORVERSION | VER_MINORVERSION | (build ? VER_BUILDNUMBER : VER_SERVICEPACKMAJOR);
    DWORDLONG ge = VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_MINORVERSION,
                                       VER_GREATER_EQUAL);

    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    v.dwMajorVersion = major;
    v.dwMinorVersion = minor;
    v.dwBuildNumber = build;
    ge = VerSetConditionMask(ge, build ? VER_BUILDNUMBER : VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
    return verify(&v, mask, ge);
}

static void test_misc(void) {
    BYTE a[64], b[64];
    HS s = NULL, d = NULL, ref = NULL, empty = (HS)1;
    HSH header;
    UINT32 len = 0;
    INT32 order = 5;
    HRESULT hr;
    void *instance = (void *)1;
    VerifyFn windows = (VerifyFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VerifyVersionInfoW");
    BOOL ten, ten_build, eleven, eight, seven;

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    check(ProcessPrng(a, sizeof(a)) && ProcessPrng(b, sizeof(b)) && memcmp(a, b, sizeof(a)), "ProcessPrng",
          "two different 64-byte blocks, first bytes %02x %02x %02x %02x", a[0], a[1], a[2], a[3]);
    hr = WindowsCreateString(L"Windows.UI.ViewManagement.UISettings", 36, &s);
    check(hr == S_OK && WindowsGetStringLen(s) == 36 && !lstrcmpW(WindowsGetStringRawBuffer(s, &len), L"Windows.UI.ViewManagement.UISettings") && len == 36,
          "string: create", "hr %08lx length %u", (unsigned long)hr, WindowsGetStringLen(s));
    hr = WindowsDuplicateString(s, &d);
    WindowsCreateStringReference(L"Windows.UI.ViewManagement.UISettings", 36, &header, &ref);
    WindowsCompareStringOrdinal(d, ref, &order);
    check(hr == S_OK && order == 0, "string: duplicate, reference, compare", "order %d", order);
    WindowsCreateString(L"", 0, &empty);
    check(empty == NULL && WindowsGetStringLen(NULL) == 0 && *WindowsGetStringRawBuffer(NULL, NULL) == 0, "string: empty",
          "null handle");
    WindowsDeleteString(d);
    WindowsDeleteString(s);
    hr = RoInitialize(1);
    check(hr == S_OK || hr == S_FALSE, "RoInitialize", "hr %08lx", (unsigned long)hr);
    WindowsCreateStringReference(L"Windows.UI.ViewManagement.UISettings", 36, &header, &ref);
    hr = RoActivateInstance(ref, &instance);
    check(1, "RoActivateInstance", "hr %08lx instance %p", (unsigned long)hr, instance);
    if (hr >= 0 && instance) (*(void (**)(void *))(*(void ***)instance + 2))(instance);
    RoUninitialize();

    ten = at_least(VerifyVersionInfoW, 10, 0, 0);
    ten_build = at_least(VerifyVersionInfoW, 10, 0, 16299);
    eleven = at_least(VerifyVersionInfoW, 11, 0, 0);
    eight = at_least(VerifyVersionInfoW, 6, 2, 0);
    seven = at_least(VerifyVersionInfoW, 6, 1, 0);
    check(ten && seven && ten_build == at_least(windows, 10, 0, 16299) && eleven == at_least(windows, 11, 0, 0) &&
              eight == at_least(windows, 6, 2, 0),
          "version questions", "at least 10.0: %d (Windows says %d), 10.0 build 16299: %d, 11: %d, 6.2: %d, 6.1: %d", ten,
          at_least(windows, 10, 0, 0), ten_build, eleven, eight, seven);
}

int main(int argc, char **argv) {
    static const struct { const char *name; void (*run)(void); } tests[] = {
        {"wait", test_wait}, {"time", test_time}, {"stack", test_stack}, {"files", test_files},
        {"dns", test_dns}, {"user", test_user}, {"misc", test_misc},
    };
    int i, j;

    for (i = 0; i < (int)(sizeof(tests) / sizeof(tests[0])); i++) {
        int wanted = argc < 2;
        for (j = 1; j < argc; j++) wanted |= !strcmp(argv[j], tests[i].name);
        if (wanted) tests[i].run();
    }
    printf("passed %d failed %d\n", passed, failed);
    return failed ? 1 : 0;
}
