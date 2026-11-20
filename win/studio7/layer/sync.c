#include "layer.h"

NTSYSAPI LONG NTAPI NtWaitForKeyedEvent(HANDLE event, PVOID key, BOOLEAN alertable, PLARGE_INTEGER timeout);
NTSYSAPI LONG NTAPI NtReleaseKeyedEvent(HANDLE event, PVOID key, BOOLEAN alertable, PLARGE_INTEGER timeout);
NTSYSAPI BOOLEAN NTAPI RtlDllShutdownInProgress(void);

enum { QUEUED, SLEEPING, WOKEN };

typedef struct Waiter {
    struct Waiter *next, *prev;
    const void *address;
    volatile LONG state;
    BOOL taken;
} Waiter;

typedef struct DECLSPEC_ALIGN(64) Bucket {
    SRWLOCK lock;
    Waiter *first, *last;
    volatile LONG waiting;
    char pad[36];
} Bucket;

_Static_assert(sizeof(Bucket) == 64, "one bucket per cache line");

static Bucket g_buckets[256];
static int g_spins;

void s7_sync_init(void) {
    SYSTEM_INFO info;
    LARGE_INTEGER frequency, begin, end;
    int i;

    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors < 2) return;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    for (i = 0; i < 4096; i++) YieldProcessor();
    QueryPerformanceCounter(&end);
    if (end.QuadPart <= begin.QuadPart) {
        g_spins = 64;
        return;
    }
    g_spins = (int)(4096 * 3 * frequency.QuadPart / ((end.QuadPart - begin.QuadPart) * 1000000));
    if (g_spins < 16) g_spins = 16;
    if (g_spins > 1024) g_spins = 1024;
}

static Bucket *bucket_for(const void *address) {
    return &g_buckets[(((ULONG_PTR)address >> 2) * 0x9E3779B97F4A7C15ull) >> 56];
}

static BOOL unchanged(const volatile void *address, const void *compare, SIZE_T size) {
    switch (size) {
    case 1: return *(const volatile UCHAR *)address == *(const UCHAR *)compare;
    case 2: return *(const volatile USHORT *)address == *(const USHORT *)compare;
    case 4: return *(const volatile ULONG *)address == *(const ULONG *)compare;
    default: return *(const volatile ULONG64 *)address == *(const ULONG64 *)compare;
    }
}

static void unlink_waiter(Bucket *b, Waiter *w) {
    if (w->prev) w->prev->next = w->next; else b->first = w->next;
    if (w->next) w->next->prev = w->prev; else b->last = w->prev;
    InterlockedDecrement(&b->waiting);
}

BOOL WINAPI s7_WaitOnAddress(volatile VOID *address, PVOID compare, SIZE_T size, DWORD ms) {
    Bucket *b;
    Waiter w;
    LARGE_INTEGER timeout;
    int spins;

    if (size != 1 && size != 2 && size != 4 && size != 8) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!unchanged(address, compare, size)) return TRUE;
    b = bucket_for((const void *)address);
    AcquireSRWLockExclusive(&b->lock);
    InterlockedIncrement(&b->waiting);
    if (!unchanged(address, compare, size)) {
        InterlockedDecrement(&b->waiting);
        ReleaseSRWLockExclusive(&b->lock);
        return TRUE;
    }
    w.address = (const void *)address;
    w.state = QUEUED;
    w.taken = FALSE;
    w.next = NULL;
    w.prev = b->last;
    if (b->last) b->last->next = &w; else b->first = &w;
    b->last = &w;
    ReleaseSRWLockExclusive(&b->lock);
    for (spins = ms ? g_spins : 0; spins; spins--) {
        if (w.state == WOKEN) return TRUE;
        YieldProcessor();
    }
    if (InterlockedCompareExchange(&w.state, SLEEPING, QUEUED) == WOKEN) return TRUE;
    timeout.QuadPart = -(LONGLONG)ms * 10000;
    if (NtWaitForKeyedEvent(NULL, &w, FALSE, ms == INFINITE ? NULL : &timeout) == STATUS_TIMEOUT) {
        AcquireSRWLockExclusive(&b->lock);
        if (!w.taken) {
            unlink_waiter(b, &w);
            ReleaseSRWLockExclusive(&b->lock);
            SetLastError(ERROR_TIMEOUT);
            return FALSE;
        }
        ReleaseSRWLockExclusive(&b->lock);
        NtWaitForKeyedEvent(NULL, &w, FALSE, NULL);
    }
    return TRUE;
}

static void wake(const void *address, BOOL all) {
    Bucket *b = bucket_for(address);
    Waiter *w, *next, *taken = NULL, **tail = &taken;

    __sync_synchronize();
    if (!b->waiting || RtlDllShutdownInProgress()) return;
    AcquireSRWLockExclusive(&b->lock);
    for (w = b->first; w; w = next) {
        next = w->next;
        if (w->address != address) continue;
        unlink_waiter(b, w);
        w->taken = TRUE;
        w->next = NULL;
        *tail = w;
        tail = &w->next;
        if (!all) break;
    }
    ReleaseSRWLockExclusive(&b->lock);
    for (w = taken; w; w = next) {
        next = w->next;
        if (InterlockedExchange(&w->state, WOKEN) == SLEEPING) NtReleaseKeyedEvent(NULL, w, FALSE, NULL);
    }
}

VOID WINAPI s7_WakeByAddressSingle(PVOID address) {
    wake(address, FALSE);
}

VOID WINAPI s7_WakeByAddressAll(PVOID address) {
    wake(address, TRUE);
}

VOID WINAPI s7_WakeConditionVariable(PCONDITION_VARIABLE variable) {
    if (!RtlDllShutdownInProgress()) WakeConditionVariable(variable);
}

VOID WINAPI s7_WakeAllConditionVariable(PCONDITION_VARIABLE variable) {
    if (!RtlDllShutdownInProgress()) WakeAllConditionVariable(variable);
}
