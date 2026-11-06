#include <windows.h>
#include <stdio.h>

#define CANARY 0x5a17c0deb1a2f00dULL

static DWORD g_slot_page, g_slot_rip, g_slot_save;
static unsigned char *g_rearm;
static volatile LONG g_faults;

static unsigned long long read_base(void) {
    unsigned long long v;
    __asm__ volatile("rdfsbase %%rax" : "=a"(v));
    return v;
}

static void write_base(unsigned long long v) { __asm__ volatile("wrfsbase %%rax" ::"a"(v)); }

static unsigned long long read_canary(void) {
    unsigned long long v;
    __asm__ volatile("movq %%fs:0x28, %0" : "=r"(v));
    return v;
}

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static unsigned slot_offset(DWORD slot) { return 0x1480 + slot * 8; }

static void emit_gs(unsigned char **p, const unsigned char *op, int n, unsigned disp) {
    *(*p)++ = 0x65;
    memcpy(*p, op, n);
    *p += n;
    memcpy(*p, &disp, 4);
    *p += 4;
}

static void build_rearm(void) {
    static const unsigned char store_rax[] = {0x48, 0x89, 0x04, 0x25}, load_rax[] = {0x48, 0x8B, 0x04, 0x25},
                               jump[] = {0xFF, 0x24, 0x25}, wrfs[] = {0xF3, 0x48, 0x0F, 0xAE, 0xD0};
    unsigned char *p = g_rearm = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    emit_gs(&p, store_rax, 4, slot_offset(g_slot_save));
    emit_gs(&p, load_rax, 4, slot_offset(g_slot_page));
    memcpy(p, wrfs, 5);
    p += 5;
    emit_gs(&p, load_rax, 4, slot_offset(g_slot_save));
    emit_gs(&p, jump, 3, slot_offset(g_slot_rip));
}

static int fs_prefixed(const unsigned char *ip) {
    int i;
    for (i = 0; i < 4; i++) {
        if (ip[i] == 0x64) return 1;
        if (ip[i] != 0x66 && ip[i] != 0xF2 && ip[i] != 0xF3 && (ip[i] & 0xF0) != 0x40) return 0;
    }
    return 0;
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        e->ExceptionRecord->ExceptionInformation[1] < 0x10000 && fs_prefixed((const unsigned char *)e->ContextRecord->Rip)) {
        InterlockedIncrement(&g_faults);
        TlsSetValue(g_slot_rip, (void *)e->ContextRecord->Rip);
        e->ContextRecord->Rip = (DWORD64)g_rearm;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static unsigned char *arm_thread(void) {
    unsigned char *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *(unsigned long long *)(page + 0x28) = CANARY;
    TlsSetValue(g_slot_page, page);
    write_base((unsigned long long)page);
    return page;
}

static HANDLE g_ping, g_pong;
static DWORD WINAPI ponger(LPVOID arg) {
    int i;
    (void)arg;
    for (i = 0; i < 20000; i++) {
        WaitForSingleObject(g_ping, INFINITE);
        SetEvent(g_pong);
    }
    return 0;
}

int main(void) {
    unsigned char *page;
    double t0;
    long i, lost;
    volatile unsigned long long sink = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    g_slot_page = TlsAlloc();
    g_slot_rip = TlsAlloc();
    g_slot_save = TlsAlloc();
    if (g_slot_save >= 64) return 1;
    build_rearm();
    AddVectoredExceptionHandler(1, on_fault);
    page = arm_thread();

    t0 = now_ms();
    lost = 0;
    while (now_ms() - t0 < 3000) {
        for (i = 0; i < 100000; i++) sink += i * 7;
        if (read_base() != (unsigned long long)page) {
            lost++;
            write_base((unsigned long long)page);
        }
    }
    printf("cpu-bound 3 s: base lost %ld times (preemption)\n", lost);

    lost = 0;
    for (i = 0; i < 100000; i++) {
        GetFileAttributesW(L"C:\\nonexistent-blox7-probe");
        if (read_base() != (unsigned long long)page) {
            lost++;
            write_base((unsigned long long)page);
        }
    }
    printf("100000 non-blocking syscalls: base lost %ld times\n", lost);

    g_ping = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_pong = CreateEventW(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, ponger, NULL, 0, NULL);
    lost = 0;
    t0 = now_ms();
    for (i = 0; i < 20000; i++) {
        SetEvent(g_ping);
        WaitForSingleObject(g_pong, INFINITE);
        if (read_base() != (unsigned long long)page) {
            lost++;
            write_base((unsigned long long)page);
        }
    }
    printf("20000 blocking ping-pong waits: base lost %ld times (%.2f us per round trip)\n", lost,
           (now_ms() - t0) * 1000.0 / 20000);

    g_faults = 0;
    t0 = now_ms();
    for (i = 0; i < 200000; i++) {
        write_base(0);
        if (read_canary() != CANARY) {
            printf("wrong canary after re-arm\n");
            return 1;
        }
    }
    printf("fault + re-arm: %ld faults, %.2f us each\n", g_faults, (now_ms() - t0) * 1000.0 / 200000);

    t0 = now_ms();
    for (i = 0; i < 10000000; i++) {
        if (read_base() != (unsigned long long)page) write_base((unsigned long long)page);
    }
    printf("rdfsbase check: %.2f ns each\n", (now_ms() - t0) * 1e6 / 10000000);
    write_base(0);
    printf("done (sink %llu)\n", sink & 1);
    return 0;
}
