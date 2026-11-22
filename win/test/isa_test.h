#include <stdio.h>

#include "isa_names.h"

typedef struct {
    uint64_t at;
    uint8_t bytes[16];
} poisonslot;

typedef struct {
    uint64_t site;
    uint64_t hits;
    uint8_t kind, len;
} trapslot;

static poisonslot *g_poison;
static size_t g_poison_cap, g_poison_n;
static wchar_t g_poison_dir[MAX_PATH];
static trapslot *g_traps;
static size_t g_traps_cap = 1u << 16, g_traps_n;
static SRWLOCK g_trap_lock = SRWLOCK_INIT;
static wchar_t g_test_log[MAX_PATH];
static volatile LONG g_cpuid_calls;
static uint64_t g_test_t0;

static int poisoned(uint64_t at, uint8_t *out) {
    size_t i;
    if (!g_poison_cap) return 0;
    for (i = slot_hash(at, g_poison_cap);; i = (i + 1) & (g_poison_cap - 1)) {
        if (g_poison[i].at == at) {
            memcpy(out, g_poison[i].bytes, 16);
            return 1;
        }
        if (!g_poison[i].at) return 0;
    }
}

static void poison_add(uint64_t at, uint64_t hi) {
    size_t i;
    if ((g_poison_n + 1) * 2 > g_poison_cap) {
        size_t cap = g_poison_cap ? g_poison_cap * 2 : 1u << 16, j;
        poisonslot *n = VirtualAlloc(NULL, cap * sizeof *n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!n) return;
        for (j = 0; j < g_poison_cap; j++) {
            if (!g_poison[j].at) continue;
            for (i = slot_hash(g_poison[j].at, cap); n[i].at; i = (i + 1) & (cap - 1)) {
            }
            n[i] = g_poison[j];
        }
        if (g_poison) VirtualFree(g_poison, 0, MEM_RELEASE);
        g_poison = n;
        g_poison_cap = cap;
    }
    for (i = slot_hash(at, g_poison_cap); g_poison[i].at && g_poison[i].at != at; i = (i + 1) & (g_poison_cap - 1)) {
    }
    if (g_poison[i].at) return;
    g_poison[i].at = at;
    memcpy(g_poison[i].bytes, (const void *)(uintptr_t)at, hi - at < 15 ? (size_t)(hi - at) : 15);
    g_poison_n++;
}

static void poison(uintptr_t base, uintptr_t lo, uintptr_t hi, const char *path) {
    const char *slash = strrchr(path, '/');
    wchar_t file[MAX_PATH + 64];
    char *text;
    HANDLE h;
    DWORD size, got, old, tmp;
    size_t i;
    if (!g_poison_dir[0]) return;
    _snwprintf(file, MAX_PATH + 63, L"%ls\\%hs.txt", g_poison_dir, slash ? slash + 1 : path);
    file[MAX_PATH + 63] = 0;
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(h, NULL);
    text = HeapAlloc(GetProcessHeap(), 0, (size_t)size + 1);
    if (!text || !ReadFile(h, text, size, &got, NULL)) {
        CloseHandle(h);
        return;
    }
    CloseHandle(h);
    text[got] = 0;
    if (!VirtualProtect((void *)lo, hi - lo, PAGE_EXECUTE_READWRITE, &old)) return;
    AcquireSRWLockExclusive(&g_lock);
    g_owner = GetCurrentThreadId();
    for (i = 0; i < got;) {
        uint64_t vaddr = 0;
        unsigned len = 0;
        while (i < got && (text[i] == '\n' || text[i] == '\r' || text[i] == ' ')) i++;
        for (; i < got && text[i] != ' '; i++)
            vaddr = vaddr * 16 + (uint64_t)(text[i] <= '9' ? text[i] - '0' : (text[i] | 0x20) - 'a' + 10);
        for (i++; i < got && text[i] >= '0' && text[i] <= '9'; i++) len = len * 10 + (unsigned)(text[i] - '0');
        if (vaddr && len >= 2 && base + vaddr >= lo && base + vaddr + len <= hi) {
            uint8_t *p = (uint8_t *)(base + vaddr);
            insn in;
            int cpuid = len == 2 && p[0] == 0x0F && p[1] == 0xA2;
            if (!cpuid && (!decode(p, base + vaddr, &in) || in.len != len || !(group_of(in.kind) & g_missing))) continue;
            poison_add(base + vaddr, hi);
            p[0] = 0x0F;
            p[1] = 0x0B;
        }
    }
    g_owner = 0;
    ReleaseSRWLockExclusive(&g_lock);
    VirtualProtect((void *)lo, hi - lo, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void *)lo, hi - lo);
    HeapFree(GetProcessHeap(), 0, text);
}

static int test_cpuid(CONTEXT *c) {
    uint8_t b[16];
    unsigned r[4], leaf = (unsigned)c->Rax, sub = (unsigned)c->Rcx;
    if (!poisoned(c->Rip, b) || b[0] != 0x0F || b[1] != 0xA2) return 0;
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
    if (leaf == 1) {
        r[2] &= ~(1u << 1 | 1u << 12 | 1u << 22 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28 | 1u << 29 | 1u << 30);
        if (g_missing & G_SSSE3) r[2] &= ~(1u << 9);
        if (g_missing & G_SSE41) r[2] &= ~(1u << 19);
        if (g_missing & G_SSE42) r[2] &= ~(1u << 20);
        if (g_missing & G_POPCNT) r[2] &= ~(1u << 23);
    } else if (leaf == 7) {
        r[0] = r[1] = r[2] = r[3] = 0;
    } else if (leaf == 0x80000001u) {
        r[2] &= ~(1u << 8);
        if (g_missing & G_POPCNT) r[2] &= ~(1u << 5);
    }
    c->Rax = r[0];
    c->Rbx = r[1];
    c->Rcx = r[2];
    c->Rdx = r[3];
    c->Rip += 2;
    InterlockedIncrement(&g_cpuid_calls);
    return 1;
}

static void test_count(uint64_t site, const insn *in) {
    size_t i;
    AcquireSRWLockExclusive(&g_trap_lock);
    if (!g_traps) g_traps = VirtualAlloc(NULL, g_traps_cap * sizeof *g_traps, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (g_traps && (g_traps_n + 1) * 2 <= g_traps_cap) {
        for (i = slot_hash(site, g_traps_cap); g_traps[i].site && g_traps[i].site != site; i = (i + 1) & (g_traps_cap - 1)) {
        }
        if (!g_traps[i].site) {
            g_traps[i].site = site;
            g_traps[i].kind = in->kind;
            g_traps[i].len = in->len;
            g_traps_n++;
        }
        g_traps[i].hits++;
    }
    ReleaseSRWLockExclusive(&g_trap_lock);
}

typedef struct {
    uint64_t site, hits;
    uint8_t kind, len, stub;
} hot;

static void describe(char *out, size_t size, uint64_t site) {
    int i;
    for (i = 0; i < g_ncode; i++)
        if (site >= g_code[i].lo && site < g_code[i].hi) {
            _snprintf(out, size, "%s+0x%llx", g_code[i].name, (unsigned long long)(site - g_code[i].base));
            out[size - 1] = 0;
            return;
        }
    _snprintf(out, size, "%p", (void *)(uintptr_t)site);
    out[size - 1] = 0;
}

static DWORD WINAPI test_stats(LPVOID unused) {
    enum { TOP = 80 };
    (void)unused;
    for (;;) {
        hot top[TOP];
        uint64_t stub_hits[K_COUNT], trap_hits[K_COUNT], stub_sites[K_COUNT], trap_sites[K_COUNT], all_stub = 0, all_trap = 0;
        uint64_t spill_hits[K_COUNT], all_spill = 0, all_generic = 0;
        size_t nstubs = 0, ntop = 0, i, j;
        int a;
        char line[256];
        HANDLE h;
        DWORD done;
        Sleep(10000);
        memset(stub_hits, 0, sizeof stub_hits);
        memset(spill_hits, 0, sizeof spill_hits);
        memset(trap_hits, 0, sizeof trap_hits);
        memset(stub_sites, 0, sizeof stub_sites);
        memset(trap_sites, 0, sizeof trap_sites);
        AcquireSRWLockShared(&g_lock);
        for (a = 0; a < g_narenas; a++)
            for (j = 0; j < g_arenas[a].nrec; j++) {
                const stubrec *r = &g_arenas[a].recs[j];
                hot e = {r->site, r->counter ? *r->counter : 0, r->kind, 0,
                         (uint8_t)(r->generic ? 3 : r->nx || r->ng ? 2 : 1)};
                nstubs++;
                stub_hits[r->kind] += e.hits;
                stub_sites[r->kind]++;
                all_stub += e.hits;
                if (e.stub == 2) {
                    spill_hits[r->kind] += e.hits;
                    all_spill += e.hits;
                }
                if (e.stub == 3) all_generic += e.hits;
                for (i = ntop < TOP ? ntop++ : TOP; i > 0 && top[i - 1].hits < e.hits; i--)
                    if (i < TOP) top[i] = top[i - 1];
                if (i < TOP) top[i] = e;
            }
        AcquireSRWLockShared(&g_trap_lock);
        for (j = 0; g_traps && j < g_traps_cap; j++) {
            hot e;
            if (!g_traps[j].site) continue;
            e.site = g_traps[j].site;
            e.hits = g_traps[j].hits;
            e.kind = g_traps[j].kind;
            e.len = g_traps[j].len;
            e.stub = 0;
            trap_hits[e.kind] += e.hits;
            trap_sites[e.kind]++;
            all_trap += e.hits;
            for (i = ntop < TOP ? ntop++ : TOP; i > 0 && top[i - 1].hits < e.hits; i--)
                if (i < TOP) top[i] = top[i - 1];
            if (i < TOP) top[i] = e;
        }
        ReleaseSRWLockShared(&g_trap_lock);
        ReleaseSRWLockShared(&g_lock);
        h = CreateFileW(g_test_log, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            int n = _snprintf(line, sizeof line,
                              "%.0f s: %llu stubs run %llu times (%llu spilling, %llu generic), %llu trap sites trapped %llu "
                              "times, %llu poisoned, %ld cpuid, %ld heals, slow unaligned %d, pshufb %d\r\n",
                              (double)(GetTickCount64() - g_test_t0) / 1000.0, (unsigned long long)nstubs,
                              (unsigned long long)all_stub, (unsigned long long)all_spill, (unsigned long long)all_generic,
                              (unsigned long long)g_traps_n, (unsigned long long)all_trap, (unsigned long long)g_poison_n,
                              g_cpuid_calls, g_heals, g_slow_unaligned, g_pshufb);
            WriteFile(h, line, (DWORD)n, &done, NULL);
            for (i = 1; i < K_COUNT; i++) {
                if (!stub_sites[i] && !trap_sites[i]) continue;
                n = _snprintf(line, sizeof line, "  %-10s stubs %6llu run %12llu (spilling %12llu)   traps %5llu hit %10llu\r\n",
                              k_kind_names[i], (unsigned long long)stub_sites[i], (unsigned long long)stub_hits[i],
                              (unsigned long long)spill_hits[i], (unsigned long long)trap_sites[i],
                              (unsigned long long)trap_hits[i]);
                WriteFile(h, line, (DWORD)n, &done, NULL);
            }
            for (i = 0; i < ntop; i++) {
                char where[96];
                describe(where, sizeof where, top[i].site);
                n = _snprintf(line, sizeof line, "  %12llu %s %-10s %s\r\n", (unsigned long long)top[i].hits,
                              top[i].stub == 1 ? "stub" : top[i].stub == 2 ? "spill" : top[i].stub == 3 ? "gen" : "trap",
                              k_kind_names[top[i].kind], where);
                WriteFile(h, line, (DWORD)n, &done, NULL);
            }
            CloseHandle(h);
        }
    }
    return 0;
}

static int test_start(void) {
    wchar_t v[16];
    g_test_t0 = GetTickCount64();
    if (!GetEnvironmentVariableW(L"BLOX7_ISA_POISON", g_poison_dir, MAX_PATH)) g_poison_dir[0] = 0;
    if (GetEnvironmentVariableW(L"BLOX7_ISA_TRAP", v, 16)) g_patching = 0;
    g_missing = GetEnvironmentVariableW(L"BLOX7_ISA_PROFILE", v, 16) && v[0] == L'k' ? G_SSSE3 | G_SSE41 | G_SSE42
                                                                                   : G_SSE41 | G_SSE42 | G_POPCNT;
    if (GetEnvironmentVariableW(L"BLOX7_ISA_LOG", v, 2)) {
        wchar_t base[MAX_PATH];
        GetEnvironmentVariableW(L"BLOX7_ISA_LOG", base, MAX_PATH);
        _snwprintf(g_test_log, MAX_PATH - 1, L"%ls.%lu.txt", base, GetCurrentProcessId());
        CloseHandle(CreateThread(NULL, 0, test_stats, NULL, 0, NULL));
    }
    return 1;
}
