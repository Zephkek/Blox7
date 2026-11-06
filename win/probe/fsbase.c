#include <windows.h>
#include <cpuid.h>
#include <stdio.h>

#define CANARY 0x5a17c0deb1a2f00dULL
#define THREADS 32

static volatile LONG g_illegal;

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == STATUS_ILLEGAL_INSTRUCTION) {
        g_illegal = 1;
        e->ContextRecord->Rip += 5;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode == 0xE0000001u) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

static unsigned long long read_base(void) {
    unsigned long long v = 0;
    __asm__ volatile("rdfsbase %%rax" : "=a"(v));
    return v;
}

static void write_base(unsigned long long v) { __asm__ volatile("wrfsbase %%rax" ::"a"(v)); }

static unsigned long long read_canary(void) {
    unsigned long long v;
    __asm__ volatile("movq %%fs:0x28, %0" : "=r"(v));
    return v;
}

static unsigned char *fresh_page(void) {
    unsigned char *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *(unsigned long long *)(page + 0x28) = CANARY;
    return page;
}

typedef struct {
    unsigned char *page;
    LONG losses, wrong, checks;
    HANDLE ready;
} Worker;

static DWORD WINAPI worker(LPVOID arg) {
    Worker *w = arg;
    int i;
    write_base((unsigned long long)w->page);
    SetEvent(w->ready);
    for (i = 0; i < 400000; i++) {
        if (read_base() != (unsigned long long)w->page) {
            w->losses++;
            write_base((unsigned long long)w->page);
        } else if (read_canary() != CANARY) {
            w->wrong++;
        }
        w->checks++;
        if ((i & 255) == 0) SwitchToThread();
        if ((i & 65535) == 0) Sleep(3);
    }
    return 0;
}

int main(void) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    unsigned short selector = 0;
    unsigned long long original, now;
    unsigned char *page;
    Worker workers[THREADS];
    HANDLE threads[THREADS];
    LONG losses = 0, wrong = 0, checks = 0;
    int i;
    OSVERSIONINFOW v = {sizeof v};
    setvbuf(stdout, NULL, _IONBF, 0);
    GetVersionExW(&v);
    printf("windows %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    AddVectoredExceptionHandler(1, on_fault);
    __cpuid_count(7, 0, a, b, c, d);
    printf("cpu fsgsbase: %u\n", b & 1);
    __asm__ volatile("movw %%fs, %0" : "=r"(selector));
    printf("fs selector: 0x%x\n", selector);

    g_illegal = 0;
    original = read_base();
    printf("rdfsbase: %s, base 0x%llx\n", g_illegal ? "faults (#UD)" : "allowed", g_illegal ? 0ULL : original);
    if (g_illegal) return 0;

    page = fresh_page();
    g_illegal = 0;
    write_base((unsigned long long)page);
    if (g_illegal) {
        printf("wrfsbase: faults (#UD)\n");
        return 0;
    }
    now = read_base();
    printf("wrfsbase: allowed, base now %p (%s), fs:0x28 %s\n", (void *)now, now == (unsigned long long)page ? "ours" : "not ours",
           now == (unsigned long long)page && read_canary() == CANARY ? "reads our canary" : "-");

    Sleep(100);
    printf("after Sleep(100): %s\n", read_base() == (unsigned long long)page ? "kept" : "lost");
    RaiseException(0xE0000001u, 0, 0, NULL);
    printf("after a handled exception: %s\n", read_base() == (unsigned long long)page ? "kept" : "lost");

    for (i = 0; i < THREADS; i++) {
        workers[i].page = fresh_page();
        workers[i].losses = workers[i].wrong = workers[i].checks = 0;
        workers[i].ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        threads[i] = CreateThread(NULL, 0, worker, &workers[i], 0, NULL);
    }
    for (i = 0; i < THREADS; i++) WaitForSingleObject(workers[i].ready, INFINITE);
    for (i = 0; i < THREADS; i += 4) {
        CONTEXT ctx;
        SuspendThread(threads[i]);
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(threads[i], &ctx);
        ctx.Dr7 = 0;
        SetThreadContext(threads[i], &ctx);
        ResumeThread(threads[i]);
    }
    WaitForMultipleObjects(THREADS, threads, TRUE, INFINITE);
    for (i = 0; i < THREADS; i++) {
        losses += workers[i].losses;
        wrong += workers[i].wrong;
        checks += workers[i].checks;
    }
    printf("%d threads, %ld checks with yields, sleeps and context edits: base lost %ld times, wrong canary %ld\n", THREADS,
           checks, losses, wrong);
    write_base(original);
    printf("restored base 0x%llx\n", read_base());
    return 0;
}
