#include "layer.h"

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
