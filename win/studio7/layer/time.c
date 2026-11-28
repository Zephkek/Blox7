#include "layer.h"

static ULONG64 g_scale;
static LONG64 g_tick;
static volatile LONG64 g_offset, g_last;

void s7_time_init(void) {
    LARGE_INTEGER frequency;
    DWORD adjustment, increment;
    BOOL disabled;

    QueryPerformanceFrequency(&frequency);
    g_scale = (10000000ull << 32) / (ULONG64)frequency.QuadPart;
    if (!GetSystemTimeAdjustment(&adjustment, &increment, &disabled) || !increment) increment = 156250;
    g_tick = (LONG64)increment + 1000;
}

VOID WINAPI s7_GetSystemTimePreciseAsFileTime(LPFILETIME out) {
    FILETIME ft, again;
    LARGE_INTEGER counter;
    ULONG64 high, low_part;
    LONG64 coarse, count, offset, low, last;

    do {
        GetSystemTimeAsFileTime(&ft);
        QueryPerformanceCounter(&counter);
        GetSystemTimeAsFileTime(&again);
    } while (ft.dwLowDateTime != again.dwLowDateTime);
    low_part = _umul128((ULONG64)counter.QuadPart, g_scale, &high);
    count = (LONG64)((low_part >> 32) | (high << 32));
    coarse = ((LONG64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    offset = g_offset;
    low = coarse - count;
    if (offset < low) g_offset = offset = low;
    else if (offset > low + g_tick) g_offset = offset = low + g_tick;
    count += offset;
    for (;;) {
        last = g_last;
        if (count < last && last - count <= g_tick) {
            count = last;
            break;
        }
        if (count == last || InterlockedCompareExchange64(&g_last, count, last) == last) break;
    }
    out->dwLowDateTime = (DWORD)count;
    out->dwHighDateTime = (DWORD)(count >> 32);
}
