#include "layer.h"
#include <objbase.h>

typedef struct {
    UINT32 flags, length, pad1, pad2;
    const WCHAR *text;
} StringHeader;

typedef struct {
    StringHeader header;
    volatile LONG refs;
    WCHAR text[1];
} String;

HRESULT WINAPI s7_RoInitialize(DWORD type) {
    return CoInitializeEx(NULL, type ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
}

void WINAPI s7_RoUninitialize(void) {
    CoUninitialize();
}

HRESULT WINAPI s7_RoActivateInstance(StringHeader *class_id, void **instance) {
    (void)class_id;
    if (instance) *instance = NULL;
    return REGDB_E_CLASSNOTREG;
}

HRESULT WINAPI s7_RoGetActivationFactory(StringHeader *class_id, REFIID iid, void **factory) {
    (void)class_id;
    (void)iid;
    if (factory) *factory = NULL;
    return REGDB_E_CLASSNOTREG;
}

HRESULT WINAPI s7_RoRegisterActivationFactories(StringHeader **class_ids, void **callbacks, UINT32 count, void **cookie) {
    (void)class_ids;
    (void)callbacks;
    (void)count;
    if (cookie) *cookie = NULL;
    return E_NOTIMPL;
}

void WINAPI s7_RoRevokeActivationFactories(void *cookie) {
    (void)cookie;
}

HRESULT WINAPI s7_RoGetApartmentIdentifier(UINT64 *id) {
    if (id) *id = 0;
    return E_NOTIMPL;
}

HRESULT WINAPI s7_RoRegisterForApartmentShutdown(void *callback, UINT64 *id, void **cookie) {
    (void)callback;
    if (id) *id = 0;
    if (cookie) *cookie = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI s7_RoUnregisterForApartmentShutdown(void *cookie) {
    (void)cookie;
    return E_NOTIMPL;
}

BOOL WINAPI s7_RoOriginateError(HRESULT error, StringHeader *message) {
    (void)error;
    (void)message;
    return FALSE;
}

BOOL WINAPI s7_RoOriginateErrorW(HRESULT error, UINT length, const WCHAR *message) {
    (void)error;
    (void)length;
    (void)message;
    return FALSE;
}

BOOL WINAPI s7_RoTransformError(HRESULT old_error, HRESULT new_error, StringHeader *message) {
    (void)old_error;
    (void)new_error;
    (void)message;
    return FALSE;
}

BOOL WINAPI s7_RoTransformErrorW(HRESULT old_error, HRESULT new_error, UINT length, const WCHAR *message) {
    (void)old_error;
    (void)new_error;
    (void)length;
    (void)message;
    return FALSE;
}

HRESULT WINAPI s7_GetRestrictedErrorInfo(void **info) {
    if (info) *info = NULL;
    return S_FALSE;
}

HRESULT WINAPI s7_SetRestrictedErrorInfo(void *info) {
    (void)info;
    return S_OK;
}

void WINAPI s7_RoFailFastWithErrorContext(HRESULT error) {
    (void)error;
    RaiseFailFastException(NULL, NULL, 0);
}

HRESULT WINAPI s7_WindowsCreateString(const WCHAR *text, UINT32 length, StringHeader **out) {
    String *s;

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!text && length) return E_POINTER;
    if (!length) return S_OK;
    s = HeapAlloc(GetProcessHeap(), 0, sizeof(String) + (SIZE_T)length * sizeof(WCHAR));
    if (!s) return E_OUTOFMEMORY;
    memcpy(s->text, text, (SIZE_T)length * sizeof(WCHAR));
    s->text[length] = 0;
    s->header.flags = 0;
    s->header.length = length;
    s->header.pad1 = s->header.pad2 = 0;
    s->header.text = s->text;
    s->refs = 1;
    *out = &s->header;
    return S_OK;
}

HRESULT WINAPI s7_WindowsCreateStringReference(const WCHAR *text, UINT32 length, StringHeader *header,
                                               StringHeader **out) {
    if (!out || !header) return E_INVALIDARG;
    *out = NULL;
    if (!text && length) return E_POINTER;
    if (text && text[length]) return E_INVALIDARG;
    if (!length) return S_OK;
    header->flags = 1;
    header->length = length;
    header->pad1 = header->pad2 = 0;
    header->text = text;
    *out = header;
    return S_OK;
}

HRESULT WINAPI s7_WindowsDeleteString(StringHeader *s) {
    if (s && !(s->flags & 1) && !InterlockedDecrement(&((String *)s)->refs)) HeapFree(GetProcessHeap(), 0, s);
    return S_OK;
}

HRESULT WINAPI s7_WindowsDuplicateString(StringHeader *s, StringHeader **out) {
    if (!out) return E_INVALIDARG;
    if (!s) {
        *out = NULL;
        return S_OK;
    }
    if (s->flags & 1) return s7_WindowsCreateString(s->text, s->length, out);
    InterlockedIncrement(&((String *)s)->refs);
    *out = s;
    return S_OK;
}

UINT32 WINAPI s7_WindowsGetStringLen(StringHeader *s) {
    return s ? s->length : 0;
}

const WCHAR *WINAPI s7_WindowsGetStringRawBuffer(StringHeader *s, UINT32 *length) {
    if (length) *length = s ? s->length : 0;
    return s ? s->text : L"";
}

BOOL WINAPI s7_WindowsIsStringEmpty(StringHeader *s) {
    return !s || !s->length;
}

HRESULT WINAPI s7_WindowsStringHasEmbeddedNull(StringHeader *s, BOOL *has) {
    UINT32 i;

    if (!has) return E_INVALIDARG;
    *has = FALSE;
    for (i = 0; s && i < s->length; i++) {
        if (!s->text[i]) {
            *has = TRUE;
            break;
        }
    }
    return S_OK;
}

HRESULT WINAPI s7_WindowsCompareStringOrdinal(StringHeader *a, StringHeader *b, INT32 *result) {
    if (!result) return E_INVALIDARG;
    *result = CompareStringOrdinal(a ? a->text : L"", a ? (int)a->length : 0, b ? b->text : L"",
                                   b ? (int)b->length : 0, FALSE) - CSTR_EQUAL;
    return S_OK;
}
