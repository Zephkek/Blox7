#include "layer.h"

typedef struct {
    DWORD device, origin;
} InputSource;

static const UCHAR g_scaled[] = {
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 21, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 57, 58, 68, 69, 71, 72, 83, 84, 92,
};

static UINT system_dpi(void) {
    static volatile UINT cached;
    UINT dpi = cached;
    HDC dc;

    if (dpi) return dpi;
    dc = GetDC(NULL);
    dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 0;
    if (dc) ReleaseDC(NULL, dc);
    if (!dpi) dpi = 96;
    if (IsProcessDPIAware()) cached = dpi;
    return dpi;
}

static BOOL scaled(int index) {
    int i;

    for (i = 0; i < (int)sizeof(g_scaled); i++)
        if (g_scaled[i] == index) return TRUE;
    return FALSE;
}

UINT WINAPI s7_GetDpiForSystem(void) {
    return system_dpi();
}

UINT WINAPI s7_GetDpiForWindow(HWND window) {
    if (!IsWindow(window)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return system_dpi();
}

int WINAPI s7_GetSystemMetricsForDpi(int index, UINT dpi) {
    int value = GetSystemMetrics(index);
    UINT base = system_dpi();

    if (!dpi || dpi == base || !scaled(index)) return value;
    return MulDiv(value, (int)dpi, (int)base);
}

BOOL WINAPI s7_AdjustWindowRectExForDpi(LPRECT rect, DWORD style, BOOL menu, DWORD ex_style, UINT dpi) {
    UINT base = system_dpi();
    RECT frame = {0, 0, 0, 0};

    if (!dpi || dpi == base) return AdjustWindowRectEx(rect, style, menu, ex_style);
    if (!AdjustWindowRectEx(&frame, style, menu, ex_style)) return FALSE;
    rect->left += MulDiv(frame.left, (int)dpi, (int)base);
    rect->top += MulDiv(frame.top, (int)dpi, (int)base);
    rect->right += MulDiv(frame.right, (int)dpi, (int)base);
    rect->bottom += MulDiv(frame.bottom, (int)dpi, (int)base);
    return TRUE;
}

BOOL WINAPI s7_GetCurrentInputMessageSource(InputSource *source) {
    DWORD extra;

    if (!source) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    extra = (DWORD)GetMessageExtraInfo();
    source->device = 0;
    if ((extra & 0xFFFFFF00) == 0xFF515700) source->device = (extra & 0x80) ? 4 : 8;
    source->origin = 0;
    return TRUE;
}

static HICON window_icon(WPARAM which) {
    static HICON icons[2];
    static volatile LONG state;
    WCHAR path[MAX_PATH];
    DWORD length;

    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        length = GetModuleFileNameW(NULL, path, MAX_PATH);
        while (length && path[length - 1] != L'\\') length--;
        if (length && length + 12 < MAX_PATH) {
            lstrcpyW(path + length, L"studio7.ico");
            icons[0] = (HICON)LoadImageW(NULL, path, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                         LR_LOADFROMFILE);
            icons[1] = (HICON)LoadImageW(NULL, path, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                         LR_LOADFROMFILE);
        }
        InterlockedExchange(&state, 2);
    }
    return state == 2 ? icons[which == ICON_BIG] : NULL;
}

LRESULT WINAPI s7_SendMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SETICON && lparam) {
        HICON ours = window_icon(wparam);
        if (ours) lparam = (LPARAM)ours;
    }
    return SendMessageW(window, message, wparam, lparam);
}

ATOM WINAPI s7_RegisterClassExW(const WNDCLASSEXW *wanted) {
    WNDCLASSEXW ours;
    HICON big, small;

    if (!wanted || wanted->cbSize != sizeof(ours) || !wanted->hIcon) return RegisterClassExW(wanted);
    big = window_icon(ICON_BIG);
    small = window_icon(ICON_SMALL);
    if (!big) return RegisterClassExW(wanted);
    ours = *wanted;
    ours.hIcon = big;
    if (small) ours.hIconSm = small;
    return RegisterClassExW(&ours);
}
