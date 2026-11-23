#include "../boot/isa.c"

#include <stdio.h>
#include <stdlib.h>

#include "isa_names.h"

_Static_assert(sizeof(insn) <= 32, "insn must fit the generic stub slot");

typedef struct {
    v128 x[16];
    uint64_t g[16];
    uint64_t flags;
    uint32_t mxcsr, pad;
} cpu;

enum { OFF_G = 256, OFF_FLAGS = 384, OFF_MXCSR = 392 };
enum { SITE_OFF = 4096, SLOT_OFF = 8192, DATA_OFF = 65536, DATA_SIZE = 8192, STACK_OFF = 131072, STACK_SIZE = 65536 };
enum { S_RSP, S_CPU, S_TMP, S_MX };

static uint8_t *g_region, *g_site, *g_data, *g_stack;
static uint8_t g_data_init[DATA_SIZE], g_stack_init[STACK_SIZE];
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;

static uint64_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static uint8_t *slotp(int i) { return g_region + SLOT_OFF + 8 * i; }

static mref at_reg(int r, int64_t disp) {
    mref m = {r, -1, 0, disp};
    return m;
}

static void build_entry(void) {
    static const uint8_t pushes[] = {0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                     0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00};
    tpl t;
    mref m;
    int i;
    memset(&t, 0, sizeof t);
    t.p = g_region;
    raw(&t, pushes, sizeof pushes);
    for (i = 0; i < 10; i++) {
        m = at_rsp(16 * i);
        enc(&t, 0xF3, 0, 0, 0x7F, 6 + i, &m, 0, -1, 0);
    }
    m = at_rip(slotp(S_RSP));
    enc(&t, 0, 1, -1, 0x89, 4, &m, 0, -1, 0);
    m = at_rip(slotp(S_CPU));
    enc(&t, 0, 1, -1, 0x89, 1, &m, 0, -1, 0);
    m = at_rip(slotp(S_MX));
    enc(&t, 0, 0, 0, 0xAE, 3, &m, 0, -1, 0);
    m = at_reg(1, OFF_MXCSR);
    enc(&t, 0, 0, 0, 0xAE, 2, &m, 0, -1, 0);
    for (i = 0; i < 16; i++) {
        m = at_reg(1, 16 * i);
        enc(&t, 0xF3, 0, 0, 0x6F, i, &m, 0, -1, 0);
    }
    m = at_reg(1, OFF_FLAGS);
    enc(&t, 0, 0, -1, 0xFF, 6, &m, 0, -1, 0);
    put(&t, 0x9D);
    m = at_reg(1, OFF_G + 32);
    enc(&t, 0, 1, -1, 0x8B, 4, &m, 0, -1, 0);
    for (i = 0; i < 16; i++) {
        if (i == 1 || i == 4) continue;
        m = at_reg(1, OFF_G + 8 * i);
        enc(&t, 0, 1, -1, 0x8B, i, &m, 0, -1, 0);
    }
    m = at_reg(1, OFF_G + 8);
    enc(&t, 0, 1, -1, 0x8B, 1, &m, 0, -1, 0);
    put(&t, 0xE9);
    put32(&t, (uint32_t)(int32_t)(g_site - (t.p + 4)));
}

static void build_exit(uint8_t *at) {
    static const uint8_t tail[] = {0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00, 0x41, 0x5F, 0x41, 0x5E, 0x41,
                                   0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B, 0xC3};
    tpl t;
    mref m;
    int i;
    memset(&t, 0, sizeof t);
    t.p = at;
    m = at_rip(slotp(S_TMP));
    enc(&t, 0, 1, -1, 0x89, 0, &m, 0, -1, 0);
    m = at_rip(slotp(S_CPU));
    enc(&t, 0, 1, -1, 0x8B, 0, &m, 0, -1, 0);
    for (i = 1; i < 16; i++) {
        m = at_reg(0, OFF_G + 8 * i);
        enc(&t, 0, 1, -1, 0x89, i, &m, 0, -1, 0);
    }
    m = at_rip(slotp(S_TMP));
    enc(&t, 0, 1, -1, 0x8B, 1, &m, 0, -1, 0);
    m = at_reg(0, OFF_G);
    enc(&t, 0, 1, -1, 0x89, 1, &m, 0, -1, 0);
    for (i = 0; i < 16; i++) {
        m = at_reg(0, 16 * i);
        enc(&t, 0xF3, 0, 0, 0x7F, i, &m, 0, -1, 0);
    }
    m = at_reg(0, OFF_MXCSR);
    enc(&t, 0, 0, 0, 0xAE, 3, &m, 0, -1, 0);
    m = at_rip(slotp(S_RSP));
    enc(&t, 0, 1, -1, 0x8B, 4, &m, 0, -1, 0);
    put(&t, 0x9C);
    put(&t, 0x59);
    m = at_reg(0, OFF_FLAGS);
    enc(&t, 0, 1, -1, 0x89, 1, &m, 0, -1, 0);
    m = at_rip(slotp(S_MX));
    enc(&t, 0, 0, 0, 0xAE, 2, &m, 0, -1, 0);
    for (i = 0; i < 10; i++) {
        m = at_rsp(16 * i);
        enc(&t, 0xF3, 0, 0, 0x6F, 6 + i, &m, 0, -1, 0);
    }
    raw(&t, tail, sizeof tail);
}

typedef struct {
    int kind, pfx, map, op, w, imm, reg, rm, mem, base, index, scale, form, rexpad, op16;
    int32_t disp;
    uint64_t target;
} tins;

static int tencode(const tins *t, uint8_t *out, uint64_t at) {
    uint8_t b[16];
    int n = 0, fix = -1, rex;
    if (t->op16) b[n++] = 0x66;
    b[n++] = (uint8_t)t->pfx;
    rex = 0x40 | t->w << 3 | (t->reg & 8 ? 4 : 0);
    if (t->mem) rex |= (t->index >= 0 && (t->index & 8) ? 2 : 0) | (t->base >= 0 && t->base < 16 && (t->base & 8) ? 1 : 0);
    else rex |= t->rm & 8 ? 1 : 0;
    if (rex != 0x40 || t->rexpad) b[n++] = (uint8_t)rex;
    b[n++] = 0x0F;
    if (t->map == 0xB8) {
        b[n++] = 0xB8;
    } else {
        b[n++] = (uint8_t)t->map;
        b[n++] = (uint8_t)t->op;
    }
    if (!t->mem) {
        b[n++] = (uint8_t)(0xC0 | (t->reg & 7) << 3 | (t->rm & 7));
    } else if (t->base == 16) {
        b[n++] = (uint8_t)(0x05 | (t->reg & 7) << 3);
        fix = n;
        n += 4;
    } else {
        int sib = t->index >= 0 || t->base < 0 || (t->base & 7) == 4, mod = t->base < 0 ? 0 : t->form;
        int ss = t->scale == 8 ? 3 : t->scale == 4 ? 2 : t->scale == 2 ? 1 : 0;
        if (mod == 0 && t->base >= 0 && (t->base & 7) == 5) mod = 1;
        b[n++] = (uint8_t)(mod << 6 | (t->reg & 7) << 3 | (sib ? 4 : (t->base & 7)));
        if (sib) b[n++] = (uint8_t)(ss << 6 | (t->index < 0 ? 4 : (t->index & 7)) << 3 | (t->base < 0 ? 5 : (t->base & 7)));
        if (t->base < 0 || mod == 2) {
            memcpy(b + n, &t->disp, 4);
            n += 4;
        } else if (mod == 1) {
            b[n++] = (uint8_t)(int8_t)t->disp;
        }
    }
    if (t->imm >= 0) b[n++] = (uint8_t)t->imm;
    if (fix >= 0) {
        int32_t d = (int32_t)((int64_t)t->target - (int64_t)(at + (uint64_t)n));
        memcpy(b + fix, &d, 4);
    }
    memcpy(out, b, (size_t)n);
    return n;
}

typedef struct {
    int kind, pfx, map, op;
} kspec;

static kspec g_specs[96];
static int g_nspecs;

static void list_specs(void) {
    int op;
    for (op = 0; op < 256; op++)
        if (op38[op]) g_specs[g_nspecs++] = (kspec){op38[op], 0x66, 0x38, op};
    for (op = 0; op < 256; op++)
        if (op3a[op]) g_specs[g_nspecs++] = (kspec){op3a[op], 0x66, 0x3A, op};
    g_specs[g_nspecs++] = (kspec){K_CRC32, 0xF2, 0x38, 0xF0};
    g_specs[g_nspecs++] = (kspec){K_CRC32, 0xF2, 0x38, 0xF1};
    g_specs[g_nspecs++] = (kspec){K_POPCNT, 0xF3, 0xB8, 0xB8};
}

static int is_float(int k) { return k >= K_ROUNDPS && k <= K_ROUNDSD; }

static uint32_t rf32(void) {
    static const uint32_t special[] = {0, 0x80000000u, 0x3F000000u, 0xBF000000u, 0x3FC00000u, 0xC0200000u, 0x4B000000u,
                                       0xCB000000u, 0x4AFFFFFFu, 0x4B7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u,
                                       0xFFC12345u, 0x00000001u, 0x807FFFFFu, 0x3F7FFFFFu, 0x40400000u, 0x3EFFFFFFu};
    uint64_t r = rnd();
    switch (r % 6) {
    case 0: return special[(r >> 8) % (sizeof special / sizeof special[0])];
    case 1: {
        float f = (float)((int64_t)(r >> 16) % 4096) / 8.0f;
        uint32_t u;
        memcpy(&u, &f, 4);
        return u;
    }
    case 2: {
        float f = (float)((int64_t)(r >> 16) % 2000001 - 1000000) / 1000.0f;
        uint32_t u;
        memcpy(&u, &f, 4);
        return u;
    }
    case 3: return (uint32_t)(r >> 32) & 0xFFBFFFFFu;
    default: {
        uint32_t u = (uint32_t)(r >> 24);
        if ((u & 0x7F800000u) == 0x7F800000u) u |= 0x00400000u;
        return u;
    }
    }
}

static uint64_t rf64(void) {
    static const uint64_t special[] = {0, 0x8000000000000000ull, 0x3FE0000000000000ull, 0xBFE0000000000000ull,
                                       0x3FF8000000000000ull, 0x4330000000000000ull, 0xC330000000000000ull,
                                       0x432FFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF8000000000123ull,
                                       0x0000000000000001ull, 0x41E0000000000000ull, 0xC1E0000000000001ull};
    uint64_t r = rnd();
    switch (r % 6) {
    case 0: return special[(r >> 8) % (sizeof special / sizeof special[0])];
    case 1: {
        double f = (double)((int64_t)(r >> 16) % 4096) / 8.0;
        uint64_t u;
        memcpy(&u, &f, 8);
        return u;
    }
    case 2: {
        double f = (double)((int64_t)(r >> 12) % 20000000001ll - 10000000000ll) / 1000.0;
        uint64_t u;
        memcpy(&u, &f, 8);
        return u;
    }
    default: {
        uint64_t u = rnd();
        if ((u & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) u |= 0x0008000000000000ull;
        return u;
    }
    }
}

static void rand_x(v128 *v, int kind) {
    int i;
    if (kind == K_ROUNDPS || kind == K_ROUNDSS) {
        for (i = 0; i < 4; i++) v->d[i] = rf32();
    } else if (kind == K_ROUNDPD || kind == K_ROUNDSD) {
        for (i = 0; i < 2; i++) v->q[i] = rf64();
    } else if (kind == K_PACKUSDW && rnd() % 2) {
        static const uint32_t edge[] = {0, 1, 65535, 65536, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 32768, 32767};
        for (i = 0; i < 4; i++) v->d[i] = rnd() % 3 ? edge[rnd() % 9] : (uint32_t)rnd();
    } else {
        v->q[0] = rnd();
        v->q[1] = rnd();
        if (rnd() % 4 == 0) v->q[rnd() % 2] = 0;
    }
}

typedef struct {
    cpu c;
    uint8_t data[DATA_SIZE], stack[STACK_SIZE];
} snap;

static int pick_gpr(int avoid1, int avoid2) {
    for (;;) {
        int r = (int)(rnd() % 16);
        if (r != 4 && r != avoid1 && r != avoid2) return r;
    }
}

static int gen(tins *t, cpu *c, const kspec *k) {
    int i, kind = k->kind, gpr_reg = kind == K_POPCNT || kind == K_CRC32;
    int gpr_rm = kind == K_PEXTRB || kind == K_PEXTRW || kind == K_PEXTRD || kind == K_EXTRACTPS || kind == K_PINSRB ||
                 kind == K_PINSRD || gpr_reg;
    int size;
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->pfx = k->pfx;
    t->map = k->map;
    t->op = k->op;
    t->imm = k->map == 0x3A ? (int)(is_float(kind) ? rnd() % 16 : rnd() % 256) : -1;
    t->w = (int)(rnd() % 2);
    t->rexpad = rnd() % 8 == 0;
    t->op16 = gpr_reg && k->op != 0xF0 && rnd() % 4 == 0;
    t->reg = gpr_reg ? pick_gpr(-1, -1) : (int)(rnd() % 16);
    t->mem = kind == K_MOVNTDQA || rnd() % 2;
    t->base = t->index = -1;
    for (i = 0; i < 16; i++) rand_x(&c->x[i], kind);
    for (i = 0; i < 16; i++) c->g[i] = rnd();
    if (kind == K_POPCNT && rnd() % 4 == 0) c->g[rnd() % 16] = 0;
    c->flags = (rnd() & 0x8D5) | 0x202;
    c->mxcsr = rnd() % 5 == 0 ? (uint32_t)(0x1F80 | (rnd() % 4) << 13) : rnd() % 7 == 0 ? 0x9FC0 : 0x1F80;
    c->g[4] = (uint64_t)(uintptr_t)(g_stack + STACK_SIZE / 2 - (rnd() % 2) * 8);
    if (!t->mem) {
        t->rm = gpr_rm ? pick_gpr(-1, -1) : (int)(rnd() % 16);
        if (kind == K_CRC32 && k->op == 0xF0 && !t->rexpad && !t->w && !(t->reg & 8) && rnd() % 2) t->rm = 4 + (int)(rnd() % 4);
        if (!gpr_rm && rnd() % 4 == 0) t->rm = t->reg;
        return 1;
    }
    if (kind == K_POPCNT || kind == K_CRC32)
        size = kind == K_CRC32 && k->op == 0xF0 ? 1 : t->w ? 8 : t->op16 ? 2 : 4;
    else
        size = (kind == K_PEXTRD || kind == K_PINSRD) && t->w ? 8 : memsize(kind);
    {
        int align = size == 16 && !gpr_reg ? 16 : 1, choice = (int)(rnd() % 8);
        uint64_t off = (64 + rnd() % (DATA_SIZE - 256)) & ~(uint64_t)(align - 1), ea;
        if (choice == 0) {
            t->base = 16;
            t->target = (uint64_t)(uintptr_t)(g_data + off);
        } else if (choice == 1) {
            int64_t d = ((int64_t)(rnd() % 400) - 120) & ~(int64_t)(align - 1);
            t->base = 4;
            t->form = d == 0 ? 0 : (d >= -128 && d < 128 && rnd() % 2) ? 1 : 2;
            t->disp = (int32_t)d;
            if (rnd() % 3 == 0) {
                t->index = pick_gpr(t->reg, -1);
                t->scale = 1 << (rnd() % 4);
                c->g[t->index] = (uint64_t)(rnd() % 4) * 16u / (unsigned)t->scale * (unsigned)t->scale;
            }
        } else {
            t->base = pick_gpr(gpr_reg ? t->reg : -1, -1);
            t->form = (int)(rnd() % 3);
            t->disp = t->form == 0 ? 0 : t->form == 1 ? (int32_t)((int64_t)(rnd() % 256) - 128) : (int32_t)((int64_t)(rnd() % 4000) - 2000);
            if (t->form == 0 && (t->base & 7) == 5) t->disp = (int32_t)((int64_t)(rnd() % 256) - 128);
            if (rnd() % 2) {
                t->index = pick_gpr(t->base, gpr_reg ? t->reg : -1);
                t->scale = 1 << (rnd() % 4);
                c->g[t->index] = rnd() % 32;
            }
            ea = (uint64_t)(uintptr_t)(g_data + off);
            c->g[t->base] = ea - (uint64_t)(int64_t)t->disp - (t->index >= 0 ? c->g[t->index] * (uint64_t)t->scale : 0);
        }
        if (choice == 1) {
            uint64_t a = c->g[4] + (uint64_t)(int64_t)t->disp + (t->index >= 0 ? c->g[t->index] * (uint64_t)t->scale : 0);
            if (a & (uint64_t)(align - 1)) c->g[4] -= a & (uint64_t)(align - 1);
        }
    }
    return 1;
}

static void save(snap *s, const cpu *c) {
    s->c = *c;
    memcpy(s->data, g_data, DATA_SIZE);
    memcpy(s->stack, g_stack, STACK_SIZE);
}

static void restore_mem(void) {
    memcpy(g_data, g_data_init, DATA_SIZE);
    memcpy(g_stack, g_stack_init, STACK_SIZE);
}

static void run_native(cpu *c) {
    ((void (*)(cpu *))(void *)g_region)(c);
}

static int g_failures[K_COUNT], g_runs[K_COUNT], g_shown[K_COUNT];
static unsigned g_dead_x, g_dead_g;

static int same(const snap *a, const snap *b, int kind, int stub, int skip_redzone, const char *what, const uint8_t *code,
                int len) {
    int i, ok = 1;
    uint32_t fmask = (kind == K_POPCNT || kind == K_PTEST) && stub ? 0x8C1u : 0x8D5u;
    size_t from = (size_t)(a->c.g[4] - (uint64_t)(uintptr_t)g_stack) - (skip_redzone ? 0 : 128);
    for (i = 0; i < 16; i++)
        if (!(g_dead_x >> i & 1) && memcmp(&a->c.x[i], &b->c.x[i], 16)) ok = 0;
    for (i = 0; i < 16; i++)
        if (!(g_dead_g >> i & 1) && a->c.g[i] != b->c.g[i]) ok = 0;
    if ((a->c.flags & fmask) != (b->c.flags & fmask)) ok = 0;
    if ((a->c.mxcsr & ~0x3Fu) != (b->c.mxcsr & ~0x3Fu)) ok = 0;
    if (memcmp(a->data, b->data, DATA_SIZE)) ok = 0;
    if (memcmp(a->stack + from, b->stack + from, STACK_SIZE - from)) ok = 0;
    if (ok) return 1;
    g_failures[kind]++;
    if (g_shown[kind]++ < 3) {
        printf("MISMATCH %s %s:", what, k_kind_names[kind]);
        for (i = 0; i < len; i++) printf(" %02x", code[i]);
        printf("\n");
        for (i = 0; i < 16; i++)
            if (memcmp(&a->c.x[i], &b->c.x[i], 16))
                printf("  xmm%d native %016llx%016llx got %016llx%016llx\n", i, (unsigned long long)a->c.x[i].q[1],
                       (unsigned long long)a->c.x[i].q[0], (unsigned long long)b->c.x[i].q[1],
                       (unsigned long long)b->c.x[i].q[0]);
        for (i = 0; i < 16; i++)
            if (a->c.g[i] != b->c.g[i])
                printf("  r%d native %016llx got %016llx\n", i, (unsigned long long)a->c.g[i], (unsigned long long)b->c.g[i]);
        if ((a->c.flags & fmask) != (b->c.flags & fmask))
            printf("  flags native %llx got %llx\n", (unsigned long long)a->c.flags, (unsigned long long)b->c.flags);
        if ((a->c.mxcsr & ~0x3Fu) != (b->c.mxcsr & ~0x3Fu)) printf("  mxcsr native %x got %x\n", a->c.mxcsr, b->c.mxcsr);
        if (memcmp(a->data, b->data, DATA_SIZE)) printf("  data differs\n");
        if (memcmp(a->stack + from, b->stack + from, STACK_SIZE - from)) printf("  stack differs\n");
    }
    return 0;
}

static void reset_patching(size_t used0[], size_t nrec0[], int narenas0) {
    int a;
    if (g_slots) memset(g_slots, 0, g_cap * sizeof *g_slots);
    g_used = 0;
    for (a = 0; a < g_narenas; a++) {
        g_arenas[a].used = a < narenas0 ? used0[a] : 16 * C_COUNT;
        g_arenas[a].nrec = a < narenas0 ? nrec0[a] : 0;
    }
}

static int differential(int iterations, const char *mode) {
    int s, it, total = 0, bad = 0;
    size_t used0[64], nrec0[64];
    int narenas0, a;
    int free_mode = !strcmp(mode, "stubfree"), heal_mode = !strcmp(mode, "stubheal");
    int stub_mode = !strcmp(mode, "stub") || free_mode || heal_mode;
    int veh_mode = !strcmp(mode, "veh"), c_mode = !strcmp(mode, "c");
    for (s = 0; s < g_nspecs; s++) {
        for (it = 0; it < iterations; it++) {
            tins t;
            cpu c, start;
            snap native, other;
            uint8_t code[16];
            int len;
            insn in;
            gen(&t, &c, &g_specs[s]);
            len = tencode(&t, code, (uint64_t)(uintptr_t)g_site);
            if (!decode(code, (uint64_t)(uintptr_t)g_site, &in) || in.len != len || in.kind != t.kind) {
                printf("DECODE %s len %d:", k_kind_names[t.kind], len);
                for (a = 0; a < len; a++) printf(" %02x", code[a]);
                printf("\n");
                bad++;
                continue;
            }
            if (insn_len(code) != len) {
                printf("LENGTH %s insn_len %d want %d\n", k_kind_names[t.kind], insn_len(code), len);
                bad++;
            }
            start = c;
            memcpy(g_site, code, (size_t)len);
            build_exit(g_site + len);
            FlushInstructionCache(GetCurrentProcess(), g_region, 65536);
            restore_mem();
            run_native(&c);
            save(&native, &c);
            restore_mem();
            c = start;
            if (c_mode) {
                regs r;
                memfault f;
                r.g = c.g;
                r.x = c.x;
                r.flags = (DWORD *)&c.flags;
                r.mxcsr = c.mxcsr;
                memset(&f, 0, sizeof f);
                if (run(&in, &r, &f)) printf("RUN failed %s\n", k_kind_names[t.kind]);
                save(&other, &c);
            } else {
                narenas0 = g_narenas;
                for (a = 0; a < g_narenas; a++) {
                    used0[a] = g_arenas[a].used;
                    nrec0[a] = g_arenas[a].nrec;
                }
                if (stub_mode) {
                    if (in.len < 5) {
                        reset_patching(used0, nrec0, narenas0);
                        continue;
                    }
                    uint64_t entry = 0;
                    int al = heal_mode ? (int)(c.g[4] & 15) ^ 8 : rnd() % 3 ? (int)(c.g[4] & 15) : -1;
                    g_dead_x = g_dead_g = 0;
                    if (free_mode) {
                        tpl probe;
                        memset(&probe, 0, sizeof probe);
                        probe.in = &in;
                        busy(&probe);
                        g_dead_x = (unsigned)rnd() & (rnd() % 2 ? 0xFFFFu : 0xFF00u) & ~probe.xbusy;
                        g_dead_g = (unsigned)rnd() & (rnd() % 2 ? 0xFFFFu : 1u << 10 | 1u << 11) & ~probe.gbusy;
                        entry = make_stub((uint64_t)(uintptr_t)g_site, &in, NULL, 0, 0, 0, g_dead_x, g_dead_g, al, NULL);
                        if (entry) {
                            int32_t rel = (int32_t)(entry - ((uint64_t)(uintptr_t)g_site + 5));
                            g_site[0] = 0xE9;
                            memcpy(g_site + 1, &rel, 4);
                        }
                    }
                    if (free_mode ? !entry : !patch((uint64_t)(uintptr_t)g_site, &in, al)) {
                        printf("PATCH failed %s\n", k_kind_names[t.kind]);
                        bad++;
                        reset_patching(used0, nrec0, narenas0);
                        continue;
                    }
                } else {
#ifdef BLOX7_ISA_TEST
                    if (g_poison) memset(g_poison, 0, g_poison_cap * sizeof *g_poison);
                    g_poison_n = 0;
                    poison_add((uint64_t)(uintptr_t)g_site, (uint64_t)(uintptr_t)g_site + 16);
                    g_site[0] = 0x0F;
                    g_site[1] = 0x0B;
#endif
                }
                FlushInstructionCache(GetCurrentProcess(), g_region, 65536);
                run_native(&c);
                save(&other, &c);
                reset_patching(used0, nrec0, narenas0);
            }
            g_runs[t.kind]++;
            total++;
            if (!same(&native, &other, t.kind, stub_mode || (veh_mode && g_patching), veh_mode, mode, code, len)) bad++;
        }
    }
    printf("%s: %d runs, %d bad, %ld heals\n", mode, total, bad, g_heals);
    return bad;
}

static uint8_t *map_elf(const char *path, uintptr_t *lo, uintptr_t *hi) {
    FILE *f = fopen(path, "rb");
    uint8_t *file, *base;
    long size;
    uint64_t phoff, top = 0;
    uint16_t phnum, i;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc((size_t)size);
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) return NULL;
    fclose(f);
    memcpy(&phoff, file + 0x20, 8);
    memcpy(&phnum, file + 0x38, 2);
    for (i = 0; i < phnum; i++) {
        const elfphdr *ph = (const elfphdr *)(file + phoff) + i;
        if (ph->type == 1 && ph->vaddr + ph->memsz > top) top = ph->vaddr + ph->memsz;
    }
    base = VirtualAlloc(NULL, (size_t)top, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!base) return NULL;
    for (i = 0; i < phnum; i++) {
        const elfphdr *ph = (const elfphdr *)(file + phoff) + i;
        if (ph->type != 1) continue;
        memcpy(base + ph->vaddr, file + ph->offset, (size_t)ph->filesz);
        if (ph->flags & 1) {
            *lo = (uintptr_t)base + (ph->vaddr & ~0xFFFull);
            *hi = (uintptr_t)base + ((ph->vaddr + ph->memsz + 0xFFF) & ~0xFFFull);
        }
    }
    free(file);
    return base;
}

static int scan_check(const char *elf, const char *census) {
    uintptr_t lo = 0, hi = 0;
    uint8_t *base = map_elf(elf, &lo, &hi);
    FILE *f = fopen(census, "r");
    char line[512];
    int want = 0, found = 0, hops = 0, missed_shown = 0, a, generic = 0, templated = 0, nospill = 0, xspill = 0, xaligned = 0;
    size_t patched = 0, i, j;
    LARGE_INTEGER t0, t1, freq;
    if (!base || !f) {
        printf("cannot open %s or %s\n", elf, census);
        return 1;
    }
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    code_range((uintptr_t)base, lo, hi, elf);
    QueryPerformanceCounter(&t1);
    for (i = 0; i < g_cap; i++)
        if (g_slots && g_slots[i].site && g_slots[i].state == S_PATCHED) {
            patched++;
            if (((uint8_t *)(uintptr_t)g_slots[i].site)[0] == 0xEB) hops++;
        }
    for (a = 0; a < g_narenas; a++)
        for (j = 0; j < g_arenas[a].nrec; j++) {
            if (g_arenas[a].recs[j].generic) generic++;
            else templated++;
            if (!g_arenas[a].recs[j].generic && !g_arenas[a].recs[j].nx && !g_arenas[a].recs[j].ng) nospill++;
            if (!g_arenas[a].recs[j].generic && g_arenas[a].recs[j].nx) {
                uint64_t at = g_arenas[a].recs[j].start;
                xspill++;
                while (at < g_arenas[a].recs[j].body) {
                    if (is_movdqa((const uint8_t *)(uintptr_t)at)) {
                        xaligned++;
                        break;
                    }
                    at += (uint64_t)insn_len((const uint8_t *)(uintptr_t)at);
                }
                if (at >= g_arenas[a].recs[j].body && xspill - xaligned <= 3) {
                    const uint8_t *q = (const uint8_t *)(uintptr_t)g_arenas[a].recs[j].start;
                    printf("  unaligned spill stub at site %llx delta %d:",
                           (unsigned long long)(g_arenas[a].recs[j].site - (uint64_t)(uintptr_t)base),
                           g_arenas[a].recs[j].delta);
                    while (q < (const uint8_t *)(uintptr_t)g_arenas[a].recs[j].body) printf(" %02x", *q++);
                    printf("\n");
                }
            }
        }
    printf("xmm-spilling stubs %d, with aligned spills %d\n", xspill, xaligned);
    while (fgets(line, sizeof line, f)) {
        unsigned long long addr;
        unsigned len;
        char mn[32];
        insn in;
        uint8_t *p;
        if (sscanf(line, "%llx\t%u\t%*s\t%*s\t%*s\t%31s", &addr, &len, mn) != 3) continue;
        p = base + addr;
        if (p[0] != 0xE9 && p[0] != 0xEB) {
            if (!decode(p, (uint64_t)(uintptr_t)p, &in) || !(group_of(in.kind) & g_missing)) continue;
            want++;
            if (missed_shown++ < 10) printf("  not patched: %llx %s len %u\n", addr, mn, len);
            continue;
        }
        want++;
        found++;
    }
    fclose(f);
    printf("scan: %.0f ms, %zu sites patched (%d via hops, %d templated stubs, %d without spills, %d generic), "
           "census sites %d, patched %d\n",
           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart, patched, hops, templated, nospill, generic,
           want, found);
    return 0;
}

static int stk_dump(const char *elf, uint64_t want, int all) {
    uintptr_t lo = 0, hi = 0;
    uint8_t *base = map_elf(elf, &lo, &hi);
    uint32_t n = 0, i;
    const uint8_t *h = base ? eh_table((uintptr_t)base, &n) : NULL;
    uint64_t s, e, at, site = (uint64_t)(uintptr_t)base + want;
    if (!h) return 1;
    for (i = 0; i < n; i++) {
        stktrack k;
        if (!fde_range(h, i, &s, &e) || site < s || site >= e) continue;
        trk_init(&k, s, e);
        printf("function %llx..%llx\n", (unsigned long long)(s - (uint64_t)(uintptr_t)base),
               (unsigned long long)(e - (uint64_t)(uintptr_t)base));
        for (at = s; at < e;) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)at;
            int len = insn_len(p), j, before;
            if (!len) break;
            before = trk_enter(&k, at);
            trk_leave(&k, p, len, at);
            if (all || k.st.sp != before || at == site || k.dead) {
                printf("%8llx sp %2d bp %2d%s |", (unsigned long long)(at - (uint64_t)(uintptr_t)base), before, k.st.bp,
                       at == site ? " <== site" : "");
                for (j = 0; j < len; j++) printf(" %02x", p[j]);
                printf("\n");
            }
            if (at == site && !all) break;
            at += (uint64_t)len;
        }
        trk_free(&k);
        return 0;
    }
    printf("no FDE covers %llx\n", (unsigned long long)want);
    return 1;
}

static double time_loop(uint8_t *fn, uint64_t n, uint8_t *data) {
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    ((void (*)(uint64_t, uint8_t *))(void *)fn)(n / 10, data);
    QueryPerformanceCounter(&t0);
    ((void (*)(uint64_t, uint8_t *))(void *)fn)(n, data);
    QueryPerformanceCounter(&t1);
    return (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart / (double)n;
}

static int perf(uint64_t n, int with_free) {
    uint8_t *fn = VirtualAlloc(NULL, 1 << 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    uint8_t *data = VirtualAlloc(NULL, 1 << 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    double empty;
    int s, form, al = getenv("ISATEST_ALIGN") ? atoi(getenv("ISATEST_ALIGN")) : -1;
    for (s = 0; s < 4096; s++) data[s] = (uint8_t)rnd();
    code_range((uintptr_t)fn, (uintptr_t)fn, (uintptr_t)fn + (1 << 16), "perf");
    {
        uint8_t *p = fn;
        *p++ = 0x48; *p++ = 0xFF; *p++ = 0xC9;
        *p++ = 0x75; *p++ = 0xFB;
        *p++ = 0xC3;
        empty = time_loop(fn, n, data);
    }
    printf("%-11s %-4s %8s %8s %8s  %s\n", "kind", "form", "native", "stub", "extra", "stub type");
    for (s = 0; s < g_nspecs; s++)
        for (form = 0; form < 3; form++) {
            const kspec *k = &g_specs[s];
            int gpr_reg = k->kind == K_POPCNT || k->kind == K_CRC32;
            int gpr_rm = k->kind == K_PEXTRB || k->kind == K_PEXTRW || k->kind == K_PEXTRD || k->kind == K_EXTRACTPS ||
                         k->kind == K_PINSRB || k->kind == K_PINSRD || gpr_reg;
            tins t;
            uint8_t code[16], *p = fn;
            int len, i;
            insn in;
            double native, stub;
            size_t used0[64], nrec0[64];
            int narenas0 = g_narenas;
            if (k->kind == K_MOVNTDQA && !form) continue;
            if (form == 2 && k->kind != K_PTEST && k->kind != K_PEXTRB) continue;
            memset(&t, 0, sizeof t);
            t.kind = k->kind;
            t.pfx = k->pfx;
            t.map = k->map;
            t.op = k->op;
            t.imm = k->map == 0x3A ? (is_float(k->kind) ? 9 : 1) : -1;
            t.reg = gpr_reg ? 0 : 1;
            t.rm = gpr_rm ? 8 : 2;
            t.mem = form == 1;
            if (form == 2) {
                t.rm = k->kind == K_PTEST ? t.reg : 0;
                t.imm = k->kind == K_PEXTRB ? 2 : t.imm;
            }
            if (getenv("ISATEST_KIND") && strcmp(getenv("ISATEST_KIND"), k_kind_names[k->kind])) continue;
            if (getenv("ISATEST_IMM") && k->map == 0x3A) t.imm = (int)strtol(getenv("ISATEST_IMM"), NULL, 0);
            if (getenv("ISATEST_RM") && form != 1) t.rm = atoi(getenv("ISATEST_RM"));
            t.base = 2;
            t.index = -1;
            t.form = 0;
            len = tencode(&t, code, (uint64_t)(uintptr_t)p);
            memcpy(p, code, (size_t)len);
            p += len;
            *p++ = 0x48; *p++ = 0xFF; *p++ = 0xC9;
            *p++ = 0x75;
            *p = (uint8_t)(int8_t)(fn - (p + 1));
            p++;
            *p++ = 0xC3;
            for (i = 0; i < 2048; i++) data[i] = (uint8_t)(i * 37 + 11);
            native = time_loop(fn, n, data);
            decode(code, (uint64_t)(uintptr_t)fn, &in);
            for (i = 0; i < g_narenas; i++) {
                used0[i] = g_arenas[i].used;
                nrec0[i] = g_arenas[i].nrec;
            }
            if (with_free && in.len < 5) {
                printf("%-11s %-4s %8.2f %8s\n", k_kind_names[k->kind], form == 1 ? "mem" : form == 2 ? "fast" : "reg", native - empty, "-");
                continue;
            }
            if (with_free) {
                tpl probe;
                uint64_t entry;
                memset(&probe, 0, sizeof probe);
                probe.in = &in;
                busy(&probe);
                entry = make_stub((uint64_t)(uintptr_t)fn, &in, NULL, 0, 0, 0, 0xFF00u & ~probe.xbusy,
                                  (1u << 10 | 1u << 11) & ~probe.gbusy, al, NULL);
                if (entry) {
                    int32_t rel = (int32_t)(entry - ((uint64_t)(uintptr_t)fn + 5));
                    fn[0] = 0xE9;
                    memcpy(fn + 1, &rel, 4);
                }
            }
            if (!with_free && !patch((uint64_t)(uintptr_t)fn, &in, al)) {
                printf("%-11s %-4s %8.2f %8s\n", k_kind_names[k->kind], form == 1 ? "mem" : form == 2 ? "fast" : "reg", native - empty, "-");
                reset_patching(used0, nrec0, narenas0);
                continue;
            }
            stub = time_loop(fn, n, data);
            {
                int generic = 0, a;
                size_t j;
                for (a = 0; a < g_narenas; a++)
                    for (j = 0; j < g_arenas[a].nrec; j++)
                        if (g_arenas[a].recs[j].site == (uint64_t)(uintptr_t)fn) generic = g_arenas[a].recs[j].generic;
                printf("%-11s %-4s %8.2f %8.2f %8.2f  %s%s\n", k_kind_names[k->kind], form == 1 ? "mem" : form == 2 ? "fast" : "reg", native - empty,
                       stub - empty, stub - native, generic ? "generic" : "sse2", k->op == 0xF0 ? " (r/m8)" : "");
            }
            reset_patching(used0, nrec0, narenas0);
        }
    return 0;
}

static int len_check(const char *elf) {
    uintptr_t lo = 0, hi = 0;
    uint8_t *base = map_elf(elf, &lo, &hi);
    uint32_t n = 0, i, bad = 0, total = 0;
    const uint8_t *h = base ? eh_table((uintptr_t)base, &n) : NULL;
    if (!h) return 1;
    for (i = 0; i < n; i++) {
        uint64_t s, e, at;
        if (!fde_range(h, i, &s, &e) || s < lo || e > hi || e <= s) continue;
        total++;
        for (at = s; at < e;) {
            int len = insn_len((const uint8_t *)(uintptr_t)at), k;
            if (!len || at + (uint64_t)len > e) {
                if (bad++ < 25) {
                    printf("fn %llx+%llx (size %llx): len %d at", (unsigned long long)(s - (uint64_t)(uintptr_t)base),
                           (unsigned long long)(at - s), (unsigned long long)(e - s), len);
                    for (k = 0; k < 12; k++) printf(" %02x", ((const uint8_t *)(uintptr_t)at)[k]);
                    printf("\n");
                }
                break;
            }
            at += (uint64_t)len;
        }
    }
    printf("%u functions, %u rejected\n", total, bad);
    return 0;
}

int main(int argc, char **argv) {
    int iterations = argc > 2 ? atoi(argv[2]) : 2000, i, bad = 0;
    const char *mode = argc > 1 ? argv[1] : "c", *prof = getenv("ISATEST_CPU");
    g_missing = G_SSE41 | G_SSE42 | G_POPCNT;
    g_pshufb = !prof || !strcmp(prof, "penryn");
    g_slow_unaligned = !prof || strcmp(prof, "k10");
    g_lahf = !getenv("ISATEST_NOLAHF");
    if (prof && !strcmp(prof, "k10")) g_missing = G_SSSE3 | G_SSE41 | G_SSE42;
    if (argc > 3) g_rng ^= (uint64_t)strtoull(argv[3], NULL, 0);
    if (!strcmp(mode, "len")) return len_check(argv[2]);
    if (!strcmp(mode, "stk")) return stk_dump(argv[2], strtoull(argv[3], NULL, 16), argc > 4);
    if (!strcmp(mode, "perf")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (getenv("ISATEST_CORE")) {
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << atoi(getenv("ISATEST_CORE")));
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        }
        list_specs();
        return perf(argc > 2 ? strtoull(argv[2], NULL, 0) : 20000000, argc > 3);
    }
#ifndef BLOX7_ISA_TEST
    if (!strcmp(mode, "probe")) {
        static const uint8_t ud2[] = {0x0F, 0x0B};
        uint8_t *page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        void *h = AddVectoredExceptionHandler(1, probe_veh);
        int caught = traps(page, ud2, sizeof ud2);
        RemoveVectoredExceptionHandler(h);
        printf("really_missing(all) on this cpu: %d, ud2 caught: %d\n",
               really_missing(G_SSSE3 | G_SSE41 | G_SSE42 | G_POPCNT), caught);
        return 0;
    }
#endif
    if (!strcmp(mode, "bench")) {
        uint8_t code[16] = {0x66, 0x0F, 0x3A, 0x21, 0xC1, 0x10};
        insn in;
        LARGE_INTEGER t0, t1, freq;
        int n;
        uint8_t *buf = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        QueryPerformanceFrequency(&freq);
        decode(code, (uint64_t)(uintptr_t)buf, &in);
        g_bulk = 1;
        QueryPerformanceCounter(&t0);
        for (n = 0; n < 20000; n++) make_stub((uint64_t)(uintptr_t)buf, &in, NULL, 0, 0, 0, 0, 0, -1, NULL);
        QueryPerformanceCounter(&t1);
        printf("make_stub %.2f us each, %d arenas\n",
               (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)freq.QuadPart / 20000, g_narenas);
        return 0;
    }
    if (!strcmp(mode, "scan")) {
        if (argc > 4 && argv[4][0] == 'k') g_missing = G_SSSE3 | G_SSE41 | G_SSE42;
        if (argc > 4 && argv[4][0] == 'n') g_missing = 0;
        if (argc > 4 && argv[4][0] == 'p') g_missing = G_POPCNT;
        if (argc > 4 && argv[4][0] == '2') g_missing = G_SSE42;
        return scan_check(argv[2], argv[3]);
    }
    g_region = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    g_site = g_region + SITE_OFF;
    g_data = g_region + DATA_OFF;
    g_stack = g_region + STACK_OFF;
    for (i = 0; i < DATA_SIZE; i++) g_data_init[i] = (uint8_t)rnd();
    for (i = 0; i < STACK_SIZE; i++) g_stack_init[i] = (uint8_t)rnd();
    list_specs();
    build_entry();
    code_range((uintptr_t)g_region, (uintptr_t)g_region, (uintptr_t)g_region + 65536, "isatest");
#ifdef BLOX7_ISA_TEST
    if (!strcmp(mode, "cpuid")) {
        cpu c;
        unsigned a, b, cc, d;
        memset(&c, 0, sizeof c);
        c.g[0] = 1;
        c.g[4] = (uint64_t)(uintptr_t)(g_stack + STACK_SIZE / 2);
        c.flags = 0x202;
        c.mxcsr = 0x1F80;
        g_site[0] = 0x0F;
        g_site[1] = 0xA2;
        build_exit(g_site + 2);
        poison_add((uint64_t)(uintptr_t)g_site, (uint64_t)(uintptr_t)g_site + 16);
        g_site[0] = 0x0F;
        g_site[1] = 0x0B;
        g_veh = AddVectoredExceptionHandler(1, veh);
        g_missing = G_SSE41 | G_SSE42 | G_POPCNT;
        run_native(&c);
        __cpuid(1, a, b, cc, d);
        printf("leaf 1 ecx: real %08x, masked %08x (sse4.1 %d sse4.2 %d popcnt %d avx %d ssse3 %d), cpuid traps %ld\n", cc,
               (unsigned)c.g[1], (int)(c.g[1] >> 19 & 1), (int)(c.g[1] >> 20 & 1), (int)(c.g[1] >> 23 & 1),
               (int)(c.g[1] >> 28 & 1), (int)(c.g[1] >> 9 & 1), g_cpuid_calls);
        return 0;
    }
    if (!strcmp(mode, "stubheal")) g_veh = AddVectoredExceptionHandler(1, veh);
    if (!strcmp(mode, "veh") || !strcmp(mode, "trap")) {
        g_veh = AddVectoredExceptionHandler(1, veh);
        g_patching = !strcmp(mode, "veh");
        mode = "veh";
    }
#endif
    bad = differential(iterations, mode);
    for (i = 1; i < K_COUNT; i++)
        if (g_failures[i]) printf("  %-10s %d/%d bad\n", k_kind_names[i], g_failures[i], g_runs[i]);
    return bad != 0;
}
