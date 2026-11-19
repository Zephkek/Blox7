#include "layer.h"

enum { PENDING, DONE, CANCELLED };

typedef struct Query {
    struct Query *next;
    HANDLE id;
    volatile LONG refs;
    SRWLOCK lock;
    int state;
    volatile DWORD delivering;
    WCHAR *name, *service;
    DWORD space;
    BOOL has_provider, has_hints;
    GUID provider, hint_provider;
    ADDRINFOEXW hints;
    PADDRINFOEXW *result;
    LPOVERLAPPED overlapped;
    LPLOOKUPSERVICE_COMPLETION_ROUTINE routine;
} Query;

static SRWLOCK g_lock;
static Query *g_queries;
static ULONG_PTR g_last_id;

static WCHAR *copy_text(const WCHAR *text) {
    SIZE_T bytes;
    WCHAR *copy;

    if (!text) return NULL;
    bytes = ((SIZE_T)lstrlenW(text) + 1) * sizeof(WCHAR);
    copy = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (copy) memcpy(copy, text, bytes);
    return copy;
}

static void release(Query *q) {
    if (InterlockedDecrement(&q->refs)) return;
    if (q->name) HeapFree(GetProcessHeap(), 0, q->name);
    if (q->service) HeapFree(GetProcessHeap(), 0, q->service);
    HeapFree(GetProcessHeap(), 0, q);
}

static void forget(Query *q) {
    Query **link;

    AcquireSRWLockExclusive(&g_lock);
    for (link = &g_queries; *link; link = &(*link)->next) {
        if (*link == q) {
            *link = q->next;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    release(q);
}

static void finish(Query *q, INT error, PADDRINFOEXW found) {
    HANDLE event = NULL;

    q->state = error == WSA_E_CANCELLED ? CANCELLED : DONE;
    q->delivering = GetCurrentThreadId();
    if (found) *q->result = found;
    if (q->overlapped) {
        event = q->overlapped->hEvent;
        q->overlapped->Internal = (ULONG_PTR)error;
        q->overlapped->InternalHigh = 0;
    }
    if (q->routine) q->routine((DWORD)error, 0, q->overlapped);
    if (event) SetEvent(event);
    q->delivering = 0;
}

static INT ask(PCWSTR name, PCWSTR service, DWORD space, LPGUID provider, const ADDRINFOEXW *hints,
               PADDRINFOEXW *result) {
    INT error = GetAddrInfoExW(name, service, space, provider, hints, result, NULL, NULL, NULL, NULL);

    return error == WSASERVICE_NOT_FOUND && name ? WSAHOST_NOT_FOUND : error;
}

static VOID CALLBACK resolve(PTP_CALLBACK_INSTANCE instance, PVOID context) {
    Query *q = context;
    PADDRINFOEXW found = NULL;
    INT error;

    CallbackMayRunLong(instance);
    error = ask(q->name, q->service, q->space, q->has_provider ? &q->provider : NULL,
                q->has_hints ? &q->hints : NULL, &found);
    AcquireSRWLockExclusive(&q->lock);
    if (q->state == PENDING) {
        finish(q, error, found);
        found = NULL;
    }
    ReleaseSRWLockExclusive(&q->lock);
    if (found) FreeAddrInfoExW(found);
    forget(q);
    release(q);
}

INT WSAAPI s7_GetAddrInfoExW(PCWSTR name, PCWSTR service, DWORD space, LPGUID provider, const ADDRINFOEXW *hints,
                             PADDRINFOEXW *result, struct timeval *timeout, LPOVERLAPPED overlapped,
                             LPLOOKUPSERVICE_COMPLETION_ROUTINE routine, LPHANDLE handle) {
    Query *q;

    (void)timeout;
    if (handle) *handle = NULL;
    if (!overlapped && !routine) return ask(name, service, space, provider, hints, result);
    if (!result) return WSAEFAULT;
    q = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*q));
    if (!q) return WSA_NOT_ENOUGH_MEMORY;
    q->refs = 1;
    q->name = copy_text(name);
    q->service = copy_text(service);
    if ((name && !q->name) || (service && !q->service)) {
        release(q);
        return WSA_NOT_ENOUGH_MEMORY;
    }
    q->space = space;
    if (provider) {
        q->provider = *provider;
        q->has_provider = TRUE;
    }
    if (hints) {
        q->hints = *hints;
        q->hints.ai_canonname = NULL;
        q->hints.ai_addr = NULL;
        q->hints.ai_blob = NULL;
        q->hints.ai_bloblen = 0;
        q->hints.ai_next = NULL;
        if (hints->ai_provider) {
            q->hint_provider = *hints->ai_provider;
            q->hints.ai_provider = &q->hint_provider;
        }
        q->has_hints = TRUE;
    }
    *result = NULL;
    q->result = result;
    q->overlapped = overlapped;
    q->routine = routine;
    q->refs = 2;
    if (overlapped) {
        overlapped->Internal = (ULONG_PTR)WSAEINPROGRESS;
        overlapped->InternalHigh = 0;
    }
    AcquireSRWLockExclusive(&g_lock);
    q->id = (HANDLE)++g_last_id;
    q->next = g_queries;
    g_queries = q;
    ReleaseSRWLockExclusive(&g_lock);
    if (handle) *handle = q->id;
    if (!TrySubmitThreadpoolCallback(resolve, q, NULL)) {
        if (handle) *handle = NULL;
        forget(q);
        release(q);
        return WSA_NOT_ENOUGH_MEMORY;
    }
    WSASetLastError(WSA_IO_PENDING);
    return WSA_IO_PENDING;
}

INT WSAAPI s7_GetAddrInfoExCancel(LPHANDLE handle) {
    Query *q;

    if (!handle || !*handle) return WSA_INVALID_HANDLE;
    AcquireSRWLockExclusive(&g_lock);
    for (q = g_queries; q && q->id != *handle; q = q->next)
        ;
    if (q) InterlockedIncrement(&q->refs);
    ReleaseSRWLockExclusive(&g_lock);
    if (!q) return WSA_INVALID_HANDLE;
    if (q->delivering != GetCurrentThreadId()) {
        AcquireSRWLockExclusive(&q->lock);
        if (q->state == PENDING) finish(q, WSA_E_CANCELLED, NULL);
        ReleaseSRWLockExclusive(&q->lock);
    }
    release(q);
    return NO_ERROR;
}

INT WSAAPI s7_GetAddrInfoExOverlappedResult(LPOVERLAPPED overlapped) {
    return overlapped ? (INT)overlapped->Internal : WSAEINVAL;
}
