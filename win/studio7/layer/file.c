#include "layer.h"

typedef struct {
    DWORD size, attributes, flags, qos;
    LPSECURITY_ATTRIBUTES security;
    HANDLE template_file;
} CreateFile2Params;

typedef struct {
    DWORD size, flags;
    BOOL *cancel;
    PVOID progress, context;
} CopyFile2Params;

typedef struct {
    int type;
    DWORD padding;
    union {
        struct {
            DWORD stream, flags;
            HANDLE source, destination;
            ULARGE_INTEGER chunk, chunk_size, stream_size, stream_done, total_size, total_done;
        } chunk_finished;
        struct {
            DWORD stream, reserved;
            HANDLE source, destination;
            ULARGE_INTEGER stream_size, total_size;
        } stream_started;
    } info;
} CopyFile2Message;

typedef int (CALLBACK *CopyFile2Progress)(const CopyFile2Message *, PVOID);

typedef struct {
    CopyFile2Progress progress;
    PVOID context;
    ULONG64 chunk, last_done;
} CopyRelay;

HANDLE WINAPI s7_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition, CreateFile2Params *p) {
    if (!p) return CreateFileW(name, access, share, NULL, disposition, 0, NULL);
    return CreateFileW(name, access, share, p->security, disposition, p->attributes | p->flags | p->qos,
                       p->template_file);
}

static DWORD CALLBACK relay_progress(LARGE_INTEGER total_size, LARGE_INTEGER total_done, LARGE_INTEGER stream_size,
                                     LARGE_INTEGER stream_done, DWORD stream, DWORD reason, HANDLE source,
                                     HANDLE destination, LPVOID data) {
    CopyRelay *relay = data;
    CopyFile2Message m;
    int action;

    memset(&m, 0, sizeof(m));
    if (reason == CALLBACK_STREAM_SWITCH) {
        m.type = 3;
        m.info.stream_started.stream = stream;
        m.info.stream_started.source = source;
        m.info.stream_started.destination = destination;
        m.info.stream_started.stream_size.QuadPart = (ULONG64)stream_size.QuadPart;
        m.info.stream_started.total_size.QuadPart = (ULONG64)total_size.QuadPart;
        relay->chunk = 0;
        relay->last_done = 0;
    } else {
        m.type = 2;
        m.info.chunk_finished.stream = stream;
        m.info.chunk_finished.source = source;
        m.info.chunk_finished.destination = destination;
        m.info.chunk_finished.chunk.QuadPart = relay->chunk++;
        m.info.chunk_finished.chunk_size.QuadPart = (ULONG64)stream_done.QuadPart - relay->last_done;
        m.info.chunk_finished.stream_size.QuadPart = (ULONG64)stream_size.QuadPart;
        m.info.chunk_finished.stream_done.QuadPart = (ULONG64)stream_done.QuadPart;
        m.info.chunk_finished.total_size.QuadPart = (ULONG64)total_size.QuadPart;
        m.info.chunk_finished.total_done.QuadPart = (ULONG64)total_done.QuadPart;
        relay->last_done = (ULONG64)stream_done.QuadPart;
    }
    action = relay->progress(&m, relay->context);
    return action == 4 ? PROGRESS_STOP : (DWORD)action;
}

HRESULT WINAPI s7_CopyFile2(LPCWSTR from, LPCWSTR to, CopyFile2Params *p) {
    CopyRelay relay;
    DWORD flags = p ? p->flags & 0x180F : 0;
    BOOL ok;

    if (p && p->progress) {
        relay.progress = (CopyFile2Progress)p->progress;
        relay.context = p->context;
        relay.chunk = relay.last_done = 0;
        ok = CopyFileExW(from, to, relay_progress, &relay, p->cancel, flags);
    } else {
        ok = CopyFileExW(from, to, NULL, NULL, p ? p->cancel : NULL, flags);
    }
    return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
