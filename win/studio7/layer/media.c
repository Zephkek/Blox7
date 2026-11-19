#include "layer.h"

typedef struct {
    GUID major, subtype;
} TypeInfo;

typedef struct {
    void *const *vtable;
    void *inner;
    void *attributes;
    volatile LONG refs;
} Wrapper;

typedef HRESULT (WINAPI *QueryFn)(void *, const GUID *, void **);
typedef ULONG (WINAPI *CountFn)(void *);
typedef HRESULT (WINAPI *AttributesFn)(void *, void **);

HRESULT WINAPI MFTEnumEx(GUID category, UINT32 flags, const TypeInfo *input, const TypeInfo *output, void ***activates, UINT32 *count);
HRESULT WINAPI MFCreateAttributes(void **attributes, UINT32 initial);

enum { SLOT_QUERY, SLOT_ADDREF, SLOT_RELEASE, SLOT_TRANSFORM_ATTRIBUTES = 8, SLOT_ACTIVATE_OBJECT = 33 };

static const GUID g_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID g_attributes = {0x2cd2d921, 0xc447, 0x44a7, {0xa1, 0x3c, 0x4a, 0xda, 0xbf, 0xc2, 0x47, 0xe3}};
static const GUID g_activate = {0x7fee9e9a, 0x4a89, 0x47a6, {0x89, 0x9c, 0xb6, 0xa5, 0x3a, 0x70, 0xfb, 0x67}};
static const GUID g_transform = {0xbf94c121, 0x5b05, 0x4e6f, {0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d}};

static void *slot(void *object, int index) {
    return (*(void ***)object)[index];
}

static void release(void *object) {
    ((CountFn)slot(object, SLOT_RELEASE))(object);
}

static BOOL same(const GUID *a, const GUID *b) {
    const UINT64 *x = (const UINT64 *)a, *y = (const UINT64 *)b;
    return x[0] == y[0] && x[1] == y[1];
}

#define FORWARD(index) \
    __attribute__((naked)) static void forward_##index(void) { \
        __asm__("movq 8(%rcx), %rcx\n\tmovq (%rcx), %rax\n\tjmpq *8*" #index "(%rax)"); \
    }

FORWARD(3) FORWARD(4) FORWARD(5) FORWARD(6) FORWARD(7) FORWARD(8) FORWARD(9) FORWARD(10) FORWARD(11) FORWARD(12) FORWARD(13)
FORWARD(14) FORWARD(15) FORWARD(16) FORWARD(17) FORWARD(18) FORWARD(19) FORWARD(20) FORWARD(21) FORWARD(22) FORWARD(23)
FORWARD(24) FORWARD(25) FORWARD(26) FORWARD(27) FORWARD(28) FORWARD(29) FORWARD(30) FORWARD(31) FORWARD(32) FORWARD(34)
FORWARD(35)

static Wrapper *wrap(void *const *vtable, void *inner) {
    Wrapper *w = (Wrapper *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));

    if (!w) return NULL;
    w->vtable = vtable;
    w->inner = inner;
    w->refs = 1;
    return w;
}

static HRESULT query(Wrapper *self, const GUID *iid, void **out, const GUID *first, const GUID *second) {
    if (!out) return E_POINTER;
    if (same(iid, &g_unknown) || same(iid, first) || same(iid, second)) {
        InterlockedIncrement(&self->refs);
        *out = self;
        return S_OK;
    }
    return ((QueryFn)slot(self->inner, SLOT_QUERY))(self->inner, iid, out);
}

static ULONG WINAPI wrapper_addref(Wrapper *self) {
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG WINAPI wrapper_release(Wrapper *self) {
    LONG left = InterlockedDecrement(&self->refs);

    if (!left) {
        if (self->attributes) release(self->attributes);
        release(self->inner);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)left;
}

static HRESULT WINAPI transform_query(Wrapper *self, const GUID *iid, void **out) {
    return query(self, iid, out, &g_transform, &g_transform);
}

static HRESULT WINAPI transform_attributes(Wrapper *self, void **out) {
    void *made = NULL;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!self->attributes) {
        hr = MFCreateAttributes(&made, 4);
        if (FAILED(hr)) return hr;
        if (InterlockedCompareExchangePointer(&self->attributes, made, NULL)) release(made);
    }
    ((CountFn)slot(self->attributes, SLOT_ADDREF))(self->attributes);
    *out = self->attributes;
    return S_OK;
}

static void *const g_transform_vtable[] = {
    transform_query, wrapper_addref, wrapper_release, forward_3, forward_4, forward_5, forward_6, forward_7,
    transform_attributes, forward_9, forward_10, forward_11, forward_12, forward_13, forward_14, forward_15, forward_16,
    forward_17, forward_18, forward_19, forward_20, forward_21, forward_22, forward_23, forward_24, forward_25,
};

static HRESULT WINAPI activate_query(Wrapper *self, const GUID *iid, void **out) {
    return query(self, iid, out, &g_attributes, &g_activate);
}

static HRESULT WINAPI activate_object(Wrapper *self, const GUID *iid, void **out) {
    void *attributes = NULL;
    Wrapper *w;
    HRESULT hr = ((QueryFn)slot(self->inner, SLOT_ACTIVATE_OBJECT))(self->inner, iid, out);

    if (FAILED(hr) || !out || !*out || !same(iid, &g_transform)) return hr;
    if (((AttributesFn)slot(*out, SLOT_TRANSFORM_ATTRIBUTES))(*out, &attributes) != E_NOTIMPL) {
        if (attributes) release(attributes);
        return hr;
    }
    w = wrap(g_transform_vtable, *out);
    if (w) *out = w;
    return hr;
}

static void *const g_activate_vtable[] = {
    activate_query, wrapper_addref, wrapper_release, forward_3, forward_4, forward_5, forward_6, forward_7, forward_8,
    forward_9, forward_10, forward_11, forward_12, forward_13, forward_14, forward_15, forward_16, forward_17, forward_18,
    forward_19, forward_20, forward_21, forward_22, forward_23, forward_24, forward_25, forward_26, forward_27, forward_28,
    forward_29, forward_30, forward_31, forward_32, activate_object, forward_34, forward_35,
};

HRESULT WINAPI s7_MFTEnumEx(GUID category, UINT32 flags, const TypeInfo *input, const TypeInfo *output, void ***activates,
                            UINT32 *count) {
    HRESULT hr = MFTEnumEx(category, flags, input, output, activates, count);
    UINT32 i;

    if (FAILED(hr) || !activates || !*activates || !count) return hr;
    for (i = 0; i < *count; i++) {
        Wrapper *w = (*activates)[i] ? wrap(g_activate_vtable, (*activates)[i]) : NULL;
        if (w) (*activates)[i] = w;
    }
    return hr;
}

HRESULT WINAPI s7_MFCreateDXGIDeviceManager(UINT *token, void **manager) {
    if (token) *token = 0;
    if (manager) *manager = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI s7_MFCreateDXGISurfaceBuffer(REFIID iid, void *surface, UINT index, BOOL bottom_up, void **buffer) {
    (void)iid;
    (void)surface;
    (void)index;
    (void)bottom_up;
    if (buffer) *buffer = NULL;
    return E_NOTIMPL;
}
