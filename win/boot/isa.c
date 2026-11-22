#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <cpuid.h>

#include "../include/w32boot.h"
#include "isa.h"

typedef union {
    uint8_t b[16];
    int8_t sb[16];
    uint16_t w[8];
    int16_t sw[8];
    uint32_t d[4];
    int32_t sd[4];
    uint64_t q[2];
    int64_t sq[2];
} v128;

enum {
    K_NONE,
    K_PSHUFB, K_PHADDW, K_PHADDD, K_PHADDSW, K_PMADDUBSW, K_PHSUBW, K_PHSUBD, K_PHSUBSW,
    K_PSIGNB, K_PSIGNW, K_PSIGND, K_PMULHRSW, K_PABSB, K_PABSW, K_PABSD, K_PALIGNR,
    K_PBLENDVB, K_BLENDVPS, K_BLENDVPD, K_PTEST,
    K_PMOVSXBW, K_PMOVSXBD, K_PMOVSXBQ, K_PMOVSXWD, K_PMOVSXWQ, K_PMOVSXDQ,
    K_PMULDQ, K_PCMPEQQ, K_MOVNTDQA, K_PACKUSDW,
    K_PMOVZXBW, K_PMOVZXBD, K_PMOVZXBQ, K_PMOVZXWD, K_PMOVZXWQ, K_PMOVZXDQ,
    K_PCMPGTQ, K_PMINSB, K_PMINSD, K_PMINUW, K_PMINUD, K_PMAXSB, K_PMAXSD, K_PMAXUW, K_PMAXUD,
    K_PMULLD, K_PHMINPOSUW,
    K_ROUNDPS, K_ROUNDPD, K_ROUNDSS, K_ROUNDSD, K_BLENDPS, K_BLENDPD, K_PBLENDW,
    K_PEXTRB, K_PEXTRW, K_PEXTRD, K_EXTRACTPS, K_PINSRB, K_INSERTPS, K_PINSRD,
    K_CRC32, K_POPCNT,
    K_COUNT
};

static const uint8_t op38[256] = {
    [0x00] = K_PSHUFB, [0x01] = K_PHADDW, [0x02] = K_PHADDD, [0x03] = K_PHADDSW, [0x04] = K_PMADDUBSW,
    [0x05] = K_PHSUBW, [0x06] = K_PHSUBD, [0x07] = K_PHSUBSW, [0x08] = K_PSIGNB, [0x09] = K_PSIGNW,
    [0x0A] = K_PSIGND, [0x0B] = K_PMULHRSW, [0x10] = K_PBLENDVB, [0x14] = K_BLENDVPS, [0x15] = K_BLENDVPD,
    [0x17] = K_PTEST, [0x1C] = K_PABSB, [0x1D] = K_PABSW, [0x1E] = K_PABSD,
    [0x20] = K_PMOVSXBW, [0x21] = K_PMOVSXBD, [0x22] = K_PMOVSXBQ, [0x23] = K_PMOVSXWD, [0x24] = K_PMOVSXWQ,
    [0x25] = K_PMOVSXDQ, [0x28] = K_PMULDQ, [0x29] = K_PCMPEQQ, [0x2A] = K_MOVNTDQA, [0x2B] = K_PACKUSDW,
    [0x30] = K_PMOVZXBW, [0x31] = K_PMOVZXBD, [0x32] = K_PMOVZXBQ, [0x33] = K_PMOVZXWD, [0x34] = K_PMOVZXWQ,
    [0x35] = K_PMOVZXDQ, [0x37] = K_PCMPGTQ, [0x38] = K_PMINSB, [0x39] = K_PMINSD, [0x3A] = K_PMINUW,
    [0x3B] = K_PMINUD, [0x3C] = K_PMAXSB, [0x3D] = K_PMAXSD, [0x3E] = K_PMAXUW, [0x3F] = K_PMAXUD,
    [0x40] = K_PMULLD, [0x41] = K_PHMINPOSUW,
};

static const uint8_t op3a[256] = {
    [0x08] = K_ROUNDPS, [0x09] = K_ROUNDPD, [0x0A] = K_ROUNDSS, [0x0B] = K_ROUNDSD, [0x0C] = K_BLENDPS,
    [0x0D] = K_BLENDPD, [0x0E] = K_PBLENDW, [0x0F] = K_PALIGNR, [0x14] = K_PEXTRB, [0x15] = K_PEXTRW,
    [0x16] = K_PEXTRD, [0x17] = K_EXTRACTPS, [0x20] = K_PINSRB, [0x21] = K_INSERTPS, [0x22] = K_PINSRD,
};

enum { RIP = 16 };

typedef struct {
    uint8_t kind, len, msize, w, op16, reg, rm, mem, scale, imm, a32, rex;
    int8_t base, index;
    int32_t disp;
    uint64_t next;
} insn;

static uint8_t memsize(int kind) {
    switch (kind) {
    case K_PMOVSXBW: case K_PMOVSXWD: case K_PMOVSXDQ: case K_PMOVZXBW: case K_PMOVZXWD: case K_PMOVZXDQ:
    case K_ROUNDSD:
        return 8;
    case K_PMOVSXBD: case K_PMOVSXWQ: case K_PMOVZXBD: case K_PMOVZXWQ: case K_ROUNDSS: case K_EXTRACTPS:
    case K_INSERTPS: case K_PEXTRD: case K_PINSRD:
        return 4;
    case K_PMOVSXBQ: case K_PMOVZXBQ: case K_PEXTRW:
        return 2;
    case K_PEXTRB: case K_PINSRB:
        return 1;
    default:
        return 16;
    }
}

static int decode(const uint8_t *c, uint64_t at, insn *in) {
    int i = 0, p66 = 0, rep = 0, rex = 0, imm = 0, mod, rm;
    uint8_t modrm;
    memset(in, 0, sizeof *in);
    in->base = in->index = -1;
    for (;; i++) {
        if (i > 4) return 0;
        if (c[i] == 0x66) p66 = 1;
        else if (c[i] == 0xF2 || c[i] == 0xF3) rep = c[i];
        else if (c[i] == 0x67) in->a32 = 1;
        else if (c[i] != 0x2E && c[i] != 0x3E && c[i] != 0x26 && c[i] != 0x36) break;
    }
    if ((c[i] & 0xF0) == 0x40) rex = c[i++];
    if (c[i++] != 0x0F) return 0;
    in->w = (uint8_t)(rex >> 3 & 1);
    in->rex = rex != 0;
    if (c[i] == 0xB8) {
        i++;
        if (rep != 0xF3) return 0;
        in->kind = K_POPCNT;
        in->op16 = (uint8_t)p66;
        in->msize = p66 ? 2 : in->w ? 8 : 4;
    } else if (c[i] == 0x38) {
        uint8_t op = c[i + 1];
        i += 2;
        if (rep == 0xF2 && (op == 0xF0 || op == 0xF1)) {
            in->kind = K_CRC32;
            in->op16 = (uint8_t)p66;
            in->msize = op == 0xF0 ? 1 : p66 ? 2 : in->w ? 8 : 4;
        } else if (p66 && !rep) {
            in->kind = op38[op];
        }
    } else if (c[i] == 0x3A) {
        uint8_t op = c[i + 1];
        i += 2;
        if (p66 && !rep) in->kind = op3a[op];
        imm = 1;
    }
    if (!in->kind) return 0;
    if (!in->msize) in->msize = (in->kind == K_PEXTRD || in->kind == K_PINSRD) && in->w ? 8 : memsize(in->kind);
    modrm = c[i++];
    mod = modrm >> 6;
    rm = modrm & 7;
    in->reg = (uint8_t)((modrm >> 3 & 7) | (rex & 4) << 1);
    if (mod == 3) {
        in->rm = (uint8_t)(rm | (rex & 1) << 3);
    } else {
        in->mem = 1;
        if (rm == 4) {
            uint8_t sib = c[i++];
            int index = (sib >> 3 & 7) | (rex & 2) << 2;
            if (index != 4) {
                in->index = (int8_t)index;
                in->scale = (uint8_t)(1 << (sib >> 6));
            }
            if ((sib & 7) == 5 && mod == 0) {
                memcpy(&in->disp, c + i, 4);
                i += 4;
            } else {
                in->base = (int8_t)((sib & 7) | (rex & 1) << 3);
            }
        } else if (rm == 5 && mod == 0) {
            in->base = RIP;
            memcpy(&in->disp, c + i, 4);
            i += 4;
        } else {
            in->base = (int8_t)(rm | (rex & 1) << 3);
        }
        if (mod == 1) {
            in->disp = (int8_t)c[i++];
        } else if (mod == 2) {
            memcpy(&in->disp, c + i, 4);
            i += 4;
        }
    }
    if (imm) in->imm = c[i++];
    if (i > 15 || (in->kind == K_MOVNTDQA && !in->mem)) return 0;
    in->len = (uint8_t)i;
    in->next = at + (uint64_t)i;
    return 1;
}

enum { L_M = 1, L_I8 = 2, L_IZ = 4, L_I16 = 8, L_IV = 16, L_MOFFS = 32, L_BAD = 64, L_I32 = 128 };

static const uint8_t k_map1[256] = {
    [0x00 ... 0x03] = L_M, [0x04] = L_I8, [0x05] = L_IZ, [0x06 ... 0x07] = L_BAD,
    [0x08 ... 0x0B] = L_M, [0x0C] = L_I8, [0x0D] = L_IZ, [0x0E] = L_BAD,
    [0x10 ... 0x13] = L_M, [0x14] = L_I8, [0x15] = L_IZ, [0x16 ... 0x17] = L_BAD,
    [0x18 ... 0x1B] = L_M, [0x1C] = L_I8, [0x1D] = L_IZ, [0x1E ... 0x1F] = L_BAD,
    [0x20 ... 0x23] = L_M, [0x24] = L_I8, [0x25] = L_IZ, [0x27] = L_BAD,
    [0x28 ... 0x2B] = L_M, [0x2C] = L_I8, [0x2D] = L_IZ, [0x2F] = L_BAD,
    [0x30 ... 0x33] = L_M, [0x34] = L_I8, [0x35] = L_IZ, [0x37] = L_BAD,
    [0x38 ... 0x3B] = L_M, [0x3C] = L_I8, [0x3D] = L_IZ, [0x3F] = L_BAD,
    [0x60 ... 0x61] = L_BAD, [0x63] = L_M, [0x68] = L_IZ, [0x69] = L_M | L_IZ, [0x6A] = L_I8, [0x6B] = L_M | L_I8,
    [0x70 ... 0x7F] = L_I8, [0x80] = L_M | L_I8, [0x81] = L_M | L_IZ, [0x82] = L_BAD, [0x83] = L_M | L_I8,
    [0x84 ... 0x8F] = L_M, [0x9A] = L_BAD, [0xA0 ... 0xA3] = L_MOFFS, [0xA8] = L_I8, [0xA9] = L_IZ,
    [0xB0 ... 0xB7] = L_I8, [0xB8 ... 0xBF] = L_IV, [0xC0 ... 0xC1] = L_M | L_I8, [0xC2] = L_I16,
    [0xC6] = L_M | L_I8, [0xC7] = L_M | L_IZ, [0xC8] = L_I16 | L_I8, [0xCA] = L_I16, [0xCD] = L_I8,
    [0xCE] = L_BAD, [0xD0 ... 0xD3] = L_M, [0xD4 ... 0xD6] = L_BAD, [0xD8 ... 0xDF] = L_M,
    [0xE0 ... 0xE7] = L_I8, [0xE8 ... 0xE9] = L_I32, [0xEA] = L_BAD, [0xEB] = L_I8,
    [0xF6 ... 0xF7] = L_M, [0xFE ... 0xFF] = L_M,
};

static const uint8_t k_map2[256] = {
    [0x00 ... 0x03] = L_M, [0x04] = L_BAD, [0x0A] = L_BAD, [0x0C] = L_BAD, [0x0D] = L_M, [0x0F] = L_M | L_I8,
    [0x10 ... 0x23] = L_M, [0x24 ... 0x27] = L_BAD, [0x28 ... 0x2F] = L_M, [0x36] = L_BAD, [0x39] = L_BAD,
    [0x3B ... 0x3F] = L_BAD, [0x40 ... 0x6F] = L_M, [0x70 ... 0x73] = L_M | L_I8, [0x74 ... 0x76] = L_M,
    [0x78 ... 0x7F] = L_M, [0x80 ... 0x8F] = L_I32, [0x90 ... 0x9F] = L_M, [0xA3] = L_M, [0xA4] = L_M | L_I8,
    [0xA5] = L_M, [0xAB] = L_M, [0xAC] = L_M | L_I8, [0xAD ... 0xAF] = L_M, [0xB0 ... 0xB9] = L_M,
    [0xBA] = L_M | L_I8, [0xBB ... 0xC1] = L_M, [0xC2] = L_M | L_I8, [0xC3] = L_M, [0xC4 ... 0xC6] = L_M | L_I8,
    [0xC7] = L_M, [0xD0 ... 0xFF] = L_M,
};

static int modrm_len(const uint8_t *p) {
    int mod = p[0] >> 6, rm = p[0] & 7, n = 1;
    if (mod == 3) return 1;
    if (rm == 4) {
        n++;
        if (mod == 0 && (p[1] & 7) == 5) n += 4;
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    return n + (mod == 1 ? 1 : mod == 2 ? 4 : 0);
}

static int insn_len(const uint8_t *p) {
    int i = 0, o16 = 0, a32 = 0, w = 0, f, op;
    for (;; i++) {
        if (i >= 14) return 0;
        if (p[i] == 0x66) o16 = 1;
        else if (p[i] == 0x67) a32 = 1;
        else if (p[i] != 0xF0 && p[i] != 0xF2 && p[i] != 0xF3 && p[i] != 0x2E && p[i] != 0x36 && p[i] != 0x3E &&
                 p[i] != 0x26 && p[i] != 0x64 && p[i] != 0x65)
            break;
    }
    if ((p[i] & 0xF0) == 0x40) w = p[i++] >> 3 & 1;
    op = p[i++];
    if (op == 0xC4 || op == 0xC5 || op == 0x62) {
        int mm = op == 0xC5 ? 1 : op == 0xC4 ? p[i] & 0x1F : p[i] & 7, vex = op != 0x62, imm;
        i += op == 0xC5 ? 1 : op == 0xC4 ? 2 : 3;
        op = p[i++];
        if (mm == 1) {
            if (vex && op == 0x77) return i;
            imm = (op >= 0x70 && op <= 0x73) || op == 0xC2 || (op >= 0xC4 && op <= 0xC6);
        } else if (mm == 2 || (!vex && (mm == 5 || mm == 6))) {
            imm = 0;
        } else if (mm == 3) {
            imm = 1;
        } else {
            return 0;
        }
        i += modrm_len(p + i) + imm;
        return i <= 15 ? i : 0;
    }
    if (op == 0x0F) {
        op = p[i++];
        if (op == 0x38) {
            i++;
            f = L_M;
        } else if (op == 0x3A) {
            i++;
            f = L_M | L_I8;
        } else {
            f = k_map2[op];
        }
    } else {
        f = k_map1[op];
        if ((op == 0xF6 || op == 0xF7) && (p[i] >> 3 & 7) < 2) f |= op == 0xF6 ? L_I8 : L_IZ;
        if (op == 0x8F && (p[i] >> 3 & 7)) return 0;
    }
    if (f & L_BAD) return 0;
    if (f & L_M) i += modrm_len(p + i);
    if (f & L_I8) i += 1;
    if (f & L_I16) i += 2;
    if (f & L_I32) i += 4;
    if (f & L_IZ) i += o16 && !w ? 2 : 4;
    if (f & L_IV) i += w ? 8 : o16 ? 2 : 4;
    if (f & L_MOFFS) i += a32 ? 4 : 8;
    return i <= 15 ? i : 0;
}

static void refs(const uint8_t *p, int len, unsigned *xm, unsigned *gm) {
    int i = 0, rex = 0, op, map = 0, sse, mod, at;
    unsigned x = 0, g = 0;
    while (i < len && (p[i] == 0x66 || p[i] == 0x67 || p[i] == 0xF2 || p[i] == 0xF3 || p[i] == 0xF0 || p[i] == 0x2E ||
                       p[i] == 0x3E || p[i] == 0x26 || p[i] == 0x36 || p[i] == 0x64 || p[i] == 0x65))
        i++;
    if (i < len && (p[i] & 0xF0) == 0x40) rex = p[i++];
    if (i >= len) return;
    op = p[i];
    if (op == 0xC4 || op == 0xC5 || op == 0x62) {
        unsigned r, b, xx, v, mm, l;
        if (op == 0xC5) {
            r = !(p[i + 1] & 0x80), xx = 0, b = 0, mm = 1, v = ~p[i + 1] >> 3 & 15, l = p[i + 1] >> 2 & 1;
            i += 2;
        } else {
            r = !(p[i + 1] & 0x80), xx = !(p[i + 1] & 0x40), b = !(p[i + 1] & 0x20), v = ~p[i + 2] >> 3 & 15;
            mm = op == 0xC4 ? p[i + 1] & 0x1Fu : p[i + 1] & 7u, l = p[i + 2] >> 2 & 1;
            i += op == 0xC4 ? 3 : 4;
        }
        op = p[i++];
        if (mm == 1 && op == 0x77) {
            if (l) *xm |= 0xFFFFu;
            return;
        }
        x = g = 1u << v;
        if (i < len) {
            x |= 1u << ((p[i] >> 3 & 7) | r << 3);
            g |= 1u << ((p[i] >> 3 & 7) | r << 3);
            if (p[i] >> 6 == 3) {
                x |= 1u << ((p[i] & 7) | b << 3);
                g |= 1u << ((p[i] & 7) | b << 3);
            } else if ((p[i] & 7) == 4 && i + 1 < len) {
                g |= 1u << ((p[i + 1] & 7) | b << 3) | 1u << ((p[i + 1] >> 3 & 7) | xx << 3);
            } else {
                g |= 1u << ((p[i] & 7) | b << 3);
            }
        }
        if (mm == 3) x |= 1u << (p[len - 1] >> 4);
        *xm |= x;
        *gm |= g;
        return;
    }
    if (op == 0x0F) {
        op = p[++i];
        if (op == 0x38 || op == 0x3A) {
            map = op;
            op = p[++i];
        }
        if (!map && op == 0x05) {
            *gm |= 1u << 1 | 1u << 11;
            return;
        }
        if (!map && (op == 0xAE || op == 0xC7) && i + 1 < len) {
            int r = p[i + 1] >> 3 & 7;
            if ((op == 0xAE && (r == 0 || r == 1 || r == 4 || r == 5 || r == 6)) || (op == 0xC7 && r >= 3 && r <= 5)) {
                *xm |= 0xFFFFu;
                *gm |= 0xFFFFu;
            }
        }
        if (!map && op >= 0xC8 && op <= 0xCF) {
            *gm |= 1u << ((op & 7) | (rex & 1) << 3);
            return;
        }
        sse = map == 0x3A || (map == 0x38 && op < 0xF0) ||
              (!map && ((op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2F) || (op >= 0x50 && op <= 0x7F) ||
                        (op >= 0xC2 && op <= 0xC6) || op >= 0xD0));
        if (!(map || (k_map2[op] & L_M))) return;
    } else {
        sse = 0;
        if ((op >= 0x50 && op <= 0x5F) || (op >= 0x90 && op <= 0x97) || (op >= 0xB0 && op <= 0xBF)) {
            *gm |= 1u << ((op & 7) | (rex & 1) << 3);
            return;
        }
        if (!(k_map1[op] & L_M)) return;
    }
    at = i + 1;
    if (at >= len) return;
    mod = p[at] >> 6;
    g |= 1u << ((p[at] >> 3 & 7) | (rex & 4) << 1);
    if (sse) x |= 1u << ((p[at] >> 3 & 7) | (rex & 4) << 1);
    if (mod == 3) {
        g |= 1u << ((p[at] & 7) | (rex & 1) << 3);
        if (sse) x |= 1u << ((p[at] & 7) | (rex & 1) << 3);
    } else if ((p[at] & 7) == 4 && at + 1 < len) {
        g |= 1u << ((p[at + 1] & 7) | (rex & 1) << 3) | 1u << ((p[at + 1] >> 3 & 7) | (rex & 2) << 2);
    } else {
        g |= 1u << ((p[at] & 7) | (rex & 1) << 3);
    }
    *xm |= x;
    *gm |= g;
}

enum { G_SSSE3 = 1, G_SSE41 = 2, G_SSE42 = 4, G_POPCNT = 8 };

static int group_of(int kind) {
    if (kind >= K_PSHUFB && kind <= K_PALIGNR) return G_SSSE3;
    if (kind == K_PCMPGTQ || kind == K_CRC32) return G_SSE42;
    if (kind == K_POPCNT) return G_POPCNT;
    return G_SSE41;
}

typedef struct {
    uint64_t *g;
    v128 *x;
    DWORD *flags;
    uint32_t mxcsr;
} regs;

typedef struct {
    uintptr_t addr;
    uint32_t code, write;
} memfault;

uint64_t isa_mem(void *dst, const void *src, uint64_t size, memfault *fault);
extern const char isa_mem_begin[], isa_mem_end[], isa_mem_fault[];

__asm__(".intel_syntax noprefix\n"
        ".text\n"
        ".p2align 4\n"
        ".globl isa_mem\n"
        ".globl isa_mem_begin\n"
        ".globl isa_mem_end\n"
        ".globl isa_mem_fault\n"
        "isa_mem:\n"
        "isa_mem_begin:\n"
        "    cmp r8, 2\n"
        "    jb .Lisa_mem1\n"
        "    je .Lisa_mem2\n"
        "    cmp r8, 8\n"
        "    jb .Lisa_mem4\n"
        "    je .Lisa_mem8\n"
        "    movdqu xmm0, xmmword ptr [rdx]\n"
        "    movdqu xmmword ptr [rcx], xmm0\n"
        "    xor eax, eax\n"
        "    ret\n"
        ".Lisa_mem1:\n"
        "    mov al, byte ptr [rdx]\n"
        "    mov byte ptr [rcx], al\n"
        "    xor eax, eax\n"
        "    ret\n"
        ".Lisa_mem2:\n"
        "    mov ax, word ptr [rdx]\n"
        "    mov word ptr [rcx], ax\n"
        "    xor eax, eax\n"
        "    ret\n"
        ".Lisa_mem4:\n"
        "    mov eax, dword ptr [rdx]\n"
        "    mov dword ptr [rcx], eax\n"
        "    xor eax, eax\n"
        "    ret\n"
        ".Lisa_mem8:\n"
        "    mov rax, qword ptr [rdx]\n"
        "    mov qword ptr [rcx], rax\n"
        "    xor eax, eax\n"
        "    ret\n"
        "isa_mem_end:\n"
        "isa_mem_fault:\n"
        "    ret\n"
        ".att_syntax prefix\n");

static int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static uint16_t satu16(int32_t v) { return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }

static uint32_t round32(uint32_t x, int mode, uint32_t mxcsr) {
    uint32_t sign = x & 0x80000000u, a = x & 0x7FFFFFFFu, e = a >> 23, t;
    float fa, ft;
    int up;
    if (e == 0xFF) return a > 0x7F800000u ? x | 0x00400000u : x;
    if (e == 0 && (mxcsr & 0x40)) a = 0;
    if (e >= 150) return x;
    t = a < 0x3F800000u ? 0 : a & ~((1u << (150 - e)) - 1);
    if (t == a) return sign | a;
    memcpy(&fa, &a, 4);
    memcpy(&ft, &t, 4);
    if (mode == 0) {
        float h = fa - ft;
        up = h > 0.5f || (h == 0.5f && ((int32_t)ft & 1));
    } else {
        up = mode == 1 ? sign != 0 : mode == 2 ? sign == 0 : 0;
    }
    if (up) ft += 1.0f;
    memcpy(&t, &ft, 4);
    return sign | t;
}

static uint64_t round64(uint64_t x, int mode, uint32_t mxcsr) {
    uint64_t sign = x & 0x8000000000000000ull, a = x & 0x7FFFFFFFFFFFFFFFull, e = a >> 52, t;
    double fa, ft;
    int up;
    if (e == 0x7FF) return a > 0x7FF0000000000000ull ? x | 0x0008000000000000ull : x;
    if (e == 0 && (mxcsr & 0x40)) a = 0;
    if (e >= 1075) return x;
    t = a < 0x3FF0000000000000ull ? 0 : a & ~((1ull << (1075 - e)) - 1);
    if (t == a) return sign | a;
    memcpy(&fa, &a, 8);
    memcpy(&ft, &t, 8);
    if (mode == 0) {
        double h = fa - ft;
        up = h > 0.5 || (h == 0.5 && ((int64_t)ft & 1));
    } else {
        up = mode == 1 ? sign != 0 : mode == 2 ? sign == 0 : 0;
    }
    if (up) ft += 1.0;
    memcpy(&t, &ft, 8);
    return sign | t;
}

static uint32_t crc32c(uint32_t crc, uint64_t v, int n) {
    int i, k;
    for (i = 0; i < n; i++) {
        crc ^= (uint8_t)(v >> 8 * i);
        for (k = 0; k < 8; k++) crc = crc >> 1 ^ (0x82F63B78u & (0u - (crc & 1)));
    }
    return crc;
}

static uint64_t ea(const insn *in, const uint64_t *g) {
    uint64_t a = (uint64_t)(int64_t)in->disp;
    if (in->base == RIP) a += in->next;
    else if (in->base >= 0) a += g[in->base];
    if (in->index >= 0) a += g[in->index] * in->scale;
    return in->a32 ? (uint32_t)a : a;
}

static int run(const insn *in, regs *r, memfault *f) {
    v128 *d = &r->x[in->reg], s, t;
    uint64_t a = in->mem ? ea(in, r->g) : 0, v = 0;
    int i, k = in->kind, mode;
    memset(&s, 0, sizeof s);
    switch (k) {
    case K_POPCNT:
    case K_CRC32:
        if (in->mem) {
            if (isa_mem(&v, (const void *)a, in->msize, f)) return 1;
        } else {
            v = in->msize == 1 && !in->rex && in->rm >= 4 && in->rm < 8 ? r->g[in->rm - 4] >> 8 : r->g[in->rm];
        }
        if (in->msize < 8) v &= (1ull << 8 * in->msize) - 1;
        if (k == K_CRC32) {
            r->g[in->reg] = crc32c((uint32_t)r->g[in->reg], v, in->msize);
        } else {
            uint64_t n = (uint64_t)__builtin_popcountll(v);
            r->g[in->reg] = in->op16 ? (r->g[in->reg] & ~0xFFFFull) | n : n;
            *r->flags = (*r->flags & ~0x8D5u) | (v ? 0 : 0x40u);
        }
        return 0;
    case K_PEXTRB:
    case K_PEXTRW:
    case K_PEXTRD:
    case K_EXTRACTPS:
        if (k == K_PEXTRB) v = d->b[in->imm & 15];
        else if (k == K_PEXTRW) v = d->w[in->imm & 7];
        else if (k == K_PEXTRD && in->w) v = d->q[in->imm & 1];
        else v = d->d[in->imm & 3];
        if (in->mem) return isa_mem((void *)a, &v, in->msize, f) != 0;
        r->g[in->rm] = v;
        return 0;
    case K_PINSRB:
    case K_PINSRD:
    case K_INSERTPS:
        if (in->mem) {
            if (isa_mem(&v, (const void *)a, in->msize, f)) return 1;
        } else {
            v = k == K_INSERTPS ? r->x[in->rm].d[in->imm >> 6] : r->g[in->rm];
        }
        if (k == K_PINSRB) {
            d->b[in->imm & 15] = (uint8_t)v;
        } else if (k == K_PINSRD && in->w) {
            d->q[in->imm & 1] = v;
        } else if (k == K_PINSRD) {
            d->d[in->imm & 3] = (uint32_t)v;
        } else {
            d->d[in->imm >> 4 & 3] = (uint32_t)v;
            for (i = 0; i < 4; i++)
                if (in->imm >> i & 1) d->d[i] = 0;
        }
        return 0;
    }
    if (in->mem) {
        if (isa_mem(&s, (const void *)a, in->msize, f)) return 1;
    } else {
        s = r->x[in->rm];
    }
    t = *d;
    mode = in->imm & 4 ? (int)(r->mxcsr >> 13 & 3) : in->imm & 3;
    switch (k) {
    case K_PSHUFB:
        for (i = 0; i < 16; i++) t.b[i] = s.b[i] & 0x80 ? 0 : d->b[s.b[i] & 15];
        break;
    case K_PHADDW:
        for (i = 0; i < 4; i++) {
            t.w[i] = (uint16_t)(d->w[2 * i] + d->w[2 * i + 1]);
            t.w[i + 4] = (uint16_t)(s.w[2 * i] + s.w[2 * i + 1]);
        }
        break;
    case K_PHADDD:
        for (i = 0; i < 2; i++) {
            t.d[i] = d->d[2 * i] + d->d[2 * i + 1];
            t.d[i + 2] = s.d[2 * i] + s.d[2 * i + 1];
        }
        break;
    case K_PHADDSW:
        for (i = 0; i < 4; i++) {
            t.sw[i] = sat16(d->sw[2 * i] + d->sw[2 * i + 1]);
            t.sw[i + 4] = sat16(s.sw[2 * i] + s.sw[2 * i + 1]);
        }
        break;
    case K_PMADDUBSW:
        for (i = 0; i < 8; i++) t.sw[i] = sat16(d->b[2 * i] * s.sb[2 * i] + d->b[2 * i + 1] * s.sb[2 * i + 1]);
        break;
    case K_PHSUBW:
        for (i = 0; i < 4; i++) {
            t.w[i] = (uint16_t)(d->w[2 * i] - d->w[2 * i + 1]);
            t.w[i + 4] = (uint16_t)(s.w[2 * i] - s.w[2 * i + 1]);
        }
        break;
    case K_PHSUBD:
        for (i = 0; i < 2; i++) {
            t.d[i] = d->d[2 * i] - d->d[2 * i + 1];
            t.d[i + 2] = s.d[2 * i] - s.d[2 * i + 1];
        }
        break;
    case K_PHSUBSW:
        for (i = 0; i < 4; i++) {
            t.sw[i] = sat16(d->sw[2 * i] - d->sw[2 * i + 1]);
            t.sw[i + 4] = sat16(s.sw[2 * i] - s.sw[2 * i + 1]);
        }
        break;
    case K_PSIGNB:
        for (i = 0; i < 16; i++) t.b[i] = s.sb[i] < 0 ? (uint8_t)(0u - d->b[i]) : s.sb[i] ? d->b[i] : 0;
        break;
    case K_PSIGNW:
        for (i = 0; i < 8; i++) t.w[i] = s.sw[i] < 0 ? (uint16_t)(0u - d->w[i]) : s.sw[i] ? d->w[i] : 0;
        break;
    case K_PSIGND:
        for (i = 0; i < 4; i++) t.d[i] = s.sd[i] < 0 ? 0u - d->d[i] : s.sd[i] ? d->d[i] : 0;
        break;
    case K_PMULHRSW:
        for (i = 0; i < 8; i++) t.sw[i] = (int16_t)((((int32_t)d->sw[i] * s.sw[i] >> 14) + 1) >> 1);
        break;
    case K_PABSB:
        for (i = 0; i < 16; i++) t.b[i] = s.sb[i] < 0 ? (uint8_t)(0u - s.b[i]) : s.b[i];
        break;
    case K_PABSW:
        for (i = 0; i < 8; i++) t.w[i] = s.sw[i] < 0 ? (uint16_t)(0u - s.w[i]) : s.w[i];
        break;
    case K_PABSD:
        for (i = 0; i < 4; i++) t.d[i] = s.sd[i] < 0 ? 0u - s.d[i] : s.d[i];
        break;
    case K_PALIGNR: {
        uint8_t cat[32];
        memcpy(cat, &s, 16);
        memcpy(cat + 16, d, 16);
        for (i = 0; i < 16; i++) t.b[i] = i + in->imm < 32 ? cat[i + in->imm] : 0;
        break;
    }
    case K_PBLENDVB:
        for (i = 0; i < 16; i++) t.b[i] = r->x[0].b[i] & 0x80 ? s.b[i] : d->b[i];
        break;
    case K_BLENDVPS:
        for (i = 0; i < 4; i++) t.d[i] = r->x[0].d[i] >> 31 ? s.d[i] : d->d[i];
        break;
    case K_BLENDVPD:
        for (i = 0; i < 2; i++) t.q[i] = r->x[0].q[i] >> 63 ? s.q[i] : d->q[i];
        break;
    case K_PTEST:
        *r->flags = (*r->flags & ~0x8D5u) | ((d->q[0] & s.q[0]) | (d->q[1] & s.q[1]) ? 0 : 0x40u) |
                    ((~d->q[0] & s.q[0]) | (~d->q[1] & s.q[1]) ? 0 : 1u);
        return 0;
    case K_PMOVSXBW:
        for (i = 0; i < 8; i++) t.sw[i] = s.sb[i];
        break;
    case K_PMOVSXBD:
        for (i = 0; i < 4; i++) t.sd[i] = s.sb[i];
        break;
    case K_PMOVSXBQ:
        for (i = 0; i < 2; i++) t.sq[i] = s.sb[i];
        break;
    case K_PMOVSXWD:
        for (i = 0; i < 4; i++) t.sd[i] = s.sw[i];
        break;
    case K_PMOVSXWQ:
        for (i = 0; i < 2; i++) t.sq[i] = s.sw[i];
        break;
    case K_PMOVSXDQ:
        for (i = 0; i < 2; i++) t.sq[i] = s.sd[i];
        break;
    case K_PMOVZXBW:
        for (i = 0; i < 8; i++) t.w[i] = s.b[i];
        break;
    case K_PMOVZXBD:
        for (i = 0; i < 4; i++) t.d[i] = s.b[i];
        break;
    case K_PMOVZXBQ:
        for (i = 0; i < 2; i++) t.q[i] = s.b[i];
        break;
    case K_PMOVZXWD:
        for (i = 0; i < 4; i++) t.d[i] = s.w[i];
        break;
    case K_PMOVZXWQ:
        for (i = 0; i < 2; i++) t.q[i] = s.w[i];
        break;
    case K_PMOVZXDQ:
        for (i = 0; i < 2; i++) t.q[i] = s.d[i];
        break;
    case K_PMULDQ:
        t.sq[0] = (int64_t)d->sd[0] * s.sd[0];
        t.sq[1] = (int64_t)d->sd[2] * s.sd[2];
        break;
    case K_PCMPEQQ:
        for (i = 0; i < 2; i++) t.q[i] = d->q[i] == s.q[i] ? ~0ull : 0;
        break;
    case K_MOVNTDQA:
        t = s;
        break;
    case K_PACKUSDW:
        for (i = 0; i < 4; i++) {
            t.w[i] = satu16(d->sd[i]);
            t.w[i + 4] = satu16(s.sd[i]);
        }
        break;
    case K_PCMPGTQ:
        for (i = 0; i < 2; i++) t.q[i] = d->sq[i] > s.sq[i] ? ~0ull : 0;
        break;
    case K_PMINSB:
        for (i = 0; i < 16; i++) t.sb[i] = d->sb[i] < s.sb[i] ? d->sb[i] : s.sb[i];
        break;
    case K_PMINSD:
        for (i = 0; i < 4; i++) t.sd[i] = d->sd[i] < s.sd[i] ? d->sd[i] : s.sd[i];
        break;
    case K_PMINUW:
        for (i = 0; i < 8; i++) t.w[i] = d->w[i] < s.w[i] ? d->w[i] : s.w[i];
        break;
    case K_PMINUD:
        for (i = 0; i < 4; i++) t.d[i] = d->d[i] < s.d[i] ? d->d[i] : s.d[i];
        break;
    case K_PMAXSB:
        for (i = 0; i < 16; i++) t.sb[i] = d->sb[i] > s.sb[i] ? d->sb[i] : s.sb[i];
        break;
    case K_PMAXSD:
        for (i = 0; i < 4; i++) t.sd[i] = d->sd[i] > s.sd[i] ? d->sd[i] : s.sd[i];
        break;
    case K_PMAXUW:
        for (i = 0; i < 8; i++) t.w[i] = d->w[i] > s.w[i] ? d->w[i] : s.w[i];
        break;
    case K_PMAXUD:
        for (i = 0; i < 4; i++) t.d[i] = d->d[i] > s.d[i] ? d->d[i] : s.d[i];
        break;
    case K_PMULLD:
        for (i = 0; i < 4; i++) t.d[i] = d->d[i] * s.d[i];
        break;
    case K_PHMINPOSUW: {
        int m = 0;
        for (i = 1; i < 8; i++)
            if (s.w[i] < s.w[m]) m = i;
        memset(&t, 0, sizeof t);
        t.w[0] = s.w[m];
        t.w[1] = (uint16_t)m;
        break;
    }
    case K_ROUNDPS:
        for (i = 0; i < 4; i++) t.d[i] = round32(s.d[i], mode, r->mxcsr);
        break;
    case K_ROUNDPD:
        for (i = 0; i < 2; i++) t.q[i] = round64(s.q[i], mode, r->mxcsr);
        break;
    case K_ROUNDSS:
        t.d[0] = round32(s.d[0], mode, r->mxcsr);
        break;
    case K_ROUNDSD:
        t.q[0] = round64(s.q[0], mode, r->mxcsr);
        break;
    case K_BLENDPS:
        for (i = 0; i < 4; i++)
            if (in->imm >> i & 1) t.d[i] = s.d[i];
        break;
    case K_BLENDPD:
        for (i = 0; i < 2; i++)
            if (in->imm >> i & 1) t.q[i] = s.q[i];
        break;
    case K_PBLENDW:
        for (i = 0; i < 8; i++)
            if (in->imm >> i & 1) t.w[i] = s.w[i];
        break;
    default:
        return 2;
    }
    *d = t;
    return 0;
}

enum {
    C_SIGN_PS, C_ABS_PS, C_MAGIC_PS, C_ONE_PS, C_SIGN_PD, C_ABS_PD, C_MAGIC_PD, C_ONE_PD,
    C_D8000, C_W8000, C_POP55, C_POP33, C_POP0F, C_POP01, C_ONEQ, C_B7F, C_B80, C_ONES, C_Q80, C_Q8000,
    C_Q80000000, C_SWAPB, C_ZXBW, C_ZXBD, C_ZXBQ, C_ZXWD, C_ZXWQ, C_ZXDQ, C_SXBD, C_COUNT
};

enum {
    O_PUNPCKLBW = 0x60, O_PUNPCKLWD = 0x61, O_PUNPCKLDQ = 0x62, O_PCMPGTB = 0x64, O_PCMPGTD = 0x66,
    O_PACKSSDW = 0x6B, O_PUNPCKLQDQ = 0x6C, O_MOVD = 0x6E, O_MOVDQA = 0x6F, O_PSHUFD = 0x70,
    O_SH16 = 0x71, O_SH32 = 0x72, O_SH64 = 0x73, O_PCMPEQB = 0x74, O_PCMPEQD = 0x76, O_MOVDST = 0x7E,
    O_PINSRW = 0xC4, O_PEXTRW = 0xC5, O_PADDQ = 0xD4, O_MOVQST = 0xD6, O_PMOVMSKB = 0xD7, O_PSUBUSW = 0xD9,
    O_PAND = 0xDB, O_PANDN = 0xDF, O_POR = 0xEB, O_PXOR = 0xEF, O_PMULUDQ = 0xF4, O_PSUBW = 0xF9,
    O_PSUBD = 0xFA, O_PSUBQ = 0xFB, O_PADDW = 0xFD, O_PADDD = 0xFE
};

enum { SH_SRL = 2, SH_SRLDQ = 3, SH_SRA = 4, SH_SLL = 6, SH_SLLDQ = 7 };

enum { F_MOV = 0x10, F_MOVAPS = 0x28, F_AND = 0x54, F_ANDN = 0x55, F_OR = 0x56, F_ADD = 0x58, F_SUB = 0x5C, F_CMP = 0xC2 };

typedef struct {
    int base, index, scale;
    int64_t disp;
} mref;

typedef struct {
    uint8_t *p;
    int bad;
    const insn *in;
    const uint8_t *k;
    int delta, nx, ng, align;
    int xs[4], gs[6];
    unsigned xbusy, gbusy, xfree, gfree;
    uint8_t *entry, *body, *refault, *desc, *tail, *rest;
    const uint8_t *moved;
    int nmoved;
    uint64_t resume;
    mref m;
#ifdef BLOX7_ISA_COUNT
    uint8_t *counter;
#endif
} tpl;


static void put(tpl *t, unsigned v) { *t->p++ = (uint8_t)v; }

static void put32(tpl *t, uint32_t v) {
    memcpy(t->p, &v, 4);
    t->p += 4;
}

static void raw(tpl *t, const uint8_t *bytes, size_t n) {
    memcpy(t->p, bytes, n);
    t->p += n;
}

static void enc(tpl *t, int pfx, int w, int esc, int op, int reg, const mref *m, int rm, int imm, int force) {
    int x = m && m->index >= 0 ? m->index : 0, b = m ? (m->base >= 0 && m->base < RIP ? m->base : 0) : rm;
    unsigned rex = 0x40u | (w ? 8u : 0) | (reg & 8 ? 4u : 0) | (x & 8 ? 2u : 0) | (b & 8 ? 1u : 0);
    unsigned r = (unsigned)(reg & 7) << 3;
    if (pfx) put(t, (unsigned)pfx);
    if (rex != 0x40u || force) put(t, rex);
    if (esc >= 0) {
        put(t, 0x0F);
        if (esc) put(t, (unsigned)esc);
    }
    put(t, (unsigned)op);
    if (!m) {
        put(t, 0xC0u | r | (unsigned)(rm & 7));
    } else if (m->base == RIP) {
        int64_t rel;
        put(t, 0x05u | r);
        rel = m->disp - (int64_t)((uintptr_t)t->p + 4 + (imm >= 0));
        if (rel != (int32_t)rel) t->bad = 1;
        put32(t, (uint32_t)rel);
    } else {
        if (m->index < 0 && m->base >= 0 && (m->base & 7) != 4) {
            put(t, 0x80u | r | (unsigned)(m->base & 7));
        } else {
            unsigned ss = m->scale == 8 ? 3u : m->scale == 4 ? 2u : m->scale == 2 ? 1u : 0;
            unsigned idx = m->index < 0 ? 4u : (unsigned)(m->index & 7);
            put(t, (m->base < 0 ? 0x04u : 0x84u) | r);
            put(t, ss << 6 | idx << 3 | (m->base < 0 ? 5u : (unsigned)(m->base & 7)));
        }
        if (m->disp != (int32_t)m->disp) t->bad = 1;
        put32(t, (uint32_t)m->disp);
    }
    if (imm >= 0) put(t, (unsigned)imm);
}

static void xr(tpl *t, int op, int dst, int src) { enc(t, 0x66, 0, 0, op, dst, NULL, src, -1, 0); }

static void xri(tpl *t, int op, int dst, int src, int imm) { enc(t, 0x66, 0, 0, op, dst, NULL, src, imm, 0); }

static void xm(tpl *t, int op, int dst, const mref *m) { enc(t, 0x66, 0, 0, op, dst, m, 0, -1, 0); }

static void xsh(tpl *t, int op, int digit, int x, int imm) { enc(t, 0x66, 0, 0, op, digit, NULL, x, imm, 0); }

static void fr(tpl *t, int pfx, int op, int dst, int src) { enc(t, pfx, 0, 0, op, dst, NULL, src, -1, 0); }

static void fm(tpl *t, int pfx, int op, int dst, const mref *m, int imm) { enc(t, pfx, 0, 0, op, dst, m, 0, imm, 0); }

static mref at_rip(const void *p) {
    mref m = {RIP, -1, 0, (int64_t)(uintptr_t)p};
    return m;
}

static mref at_rsp(int off) {
    mref m = {4, -1, 0, off};
    return m;
}

static mref kref(const tpl *t, int i) { return at_rip(t->k + 16 * i); }

static uint8_t *consts(tpl *t, int n) {
    uint8_t *c = t->p;
    t->p += 16 * n;
    return c;
}

static int xscratch(tpl *t) {
    int r;
    for (r = 15; r >= 8; r--)
        if ((t->xfree >> r & 1) && !(t->xbusy >> r & 1)) {
            t->xbusy |= 1u << r;
            return r;
        }
    for (r = 15; r >= 0; r--)
        if (!(t->xbusy >> r & 1)) {
            t->xbusy |= 1u << r;
            t->xs[t->nx++] = r;
            return r;
        }
    return -1;
}

static int gscratch(tpl *t) {
    static const int order[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 5};
    size_t i;
    int r;
    for (r = 11; r >= 10; r--)
        if ((t->gfree >> r & 1) && !(t->gbusy >> r & 1)) {
            t->gbusy |= 1u << r;
            return r;
        }
    for (i = 0; i < sizeof order / sizeof order[0]; i++)
        if (!(t->gbusy >> order[i] & 1)) {
            t->gbusy |= 1u << order[i];
            t->gs[t->ng++] = order[i];
            return order[i];
        }
    return -1;
}

static int g_slow_unaligned;

static void spill_x(tpl *t, int x, int off, int store) {
    mref m = at_rsp(off), h = at_rsp(off + 8);
    if (t->align >= 0) {
        enc(t, 0x66, 0, 0, store ? 0x7F : 0x6F, x, &m, 0, -1, 0);
    } else if (g_slow_unaligned) {
        if (store) enc(t, 0x66, 0, 0, 0xD6, x, &m, 0, -1, 0);
        else enc(t, 0xF3, 0, 0, 0x7E, x, &m, 0, -1, 0);
        enc(t, 0, 0, 0, store ? 0x17 : 0x16, x, &h, 0, -1, 0);
    } else {
        enc(t, 0xF3, 0, 0, store ? 0x7F : 0x6F, x, &m, 0, -1, 0);
    }
}

static void prologue(tpl *t) {
    int i;
#ifdef BLOX7_ISA_COUNT
    int cr = xscratch(t);
#endif
    t->entry = t->p;
    t->delta = t->nx || t->ng ? 128 + 16 * t->nx + 8 * t->ng : 0;
    if (!t->nx) t->align = -1;
    if (t->align >= 0) t->delta += (t->align - t->delta) & 15;
    if (t->delta) {
        mref m = at_rsp(-t->delta);
        enc(t, 0, 1, -1, 0x8D, 4, &m, 0, -1, 0);
    }
    for (i = 0; i < t->nx; i++) spill_x(t, t->xs[i], 16 * i, 1);
    for (i = 0; i < t->ng; i++) {
        mref m = at_rsp(16 * t->nx + 8 * i);
        enc(t, 0, 1, -1, 0x89, t->gs[i], &m, 0, -1, 0);
    }
    t->body = t->p;
#ifdef BLOX7_ISA_COUNT
    {
        mref c = at_rip(t->counter), one = kref(t, C_ONEQ);
        enc(t, 0xF3, 0, 0, 0x7E, cr, &c, 0, -1, 0);
        xm(t, O_PADDQ, cr, &one);
        enc(t, 0x66, 0, 0, O_MOVQST, cr, &c, 0, -1, 0);
    }
#endif
    t->m.base = t->in->base;
    t->m.index = t->in->index;
    t->m.scale = t->in->scale;
    t->m.disp = t->in->disp;
    if (t->in->base == RIP) t->m.disp += (int64_t)t->in->next;
    else if (t->in->base == 4) t->m.disp += t->delta;
}

static void epilogue(tpl *t) {
    int i;
    int64_t rel;
    t->rest = t->p;
    for (i = 0; i < t->ng; i++) {
        mref m = at_rsp(16 * t->nx + 8 * i);
        enc(t, 0, 1, -1, 0x8B, t->gs[i], &m, 0, -1, 0);
    }
    for (i = 0; i < t->nx; i++) spill_x(t, t->xs[i], 16 * i, 0);
    if (t->delta) {
        mref m = at_rsp(t->delta);
        enc(t, 0, 1, -1, 0x8D, 4, &m, 0, -1, 0);
    }
    t->tail = t->p;
    if (t->nmoved) raw(t, t->moved, (size_t)t->nmoved);
    put(t, 0xE9);
    rel = (int64_t)(t->nmoved ? t->resume : t->in->next) - (int64_t)((uintptr_t)t->p + 4);
    if (rel != (int32_t)rel) t->bad = 1;
    put32(t, (uint32_t)rel);
}

static int g_pshufb;

enum { O_PMINUB = 0xDA, O_PADDUSB = 0xDC, O_PMAXUB = 0xDE, O_PMAXSW = 0xEE, O_PSUBB = 0xF8, O_PADDB = 0xFC };

enum { F_MOVLPS = 0x12, F_MOVHPS = 0x16, F_MOVHPS_ST = 0x17, F_SHUF = 0xC6 };

static int xfree_count(const tpl *t) {
    int r, n = 0;
    for (r = 15; r >= 8; r--)
        if ((t->xfree >> r & 1) && !(t->xbusy >> r & 1)) n++;
    return n;
}

static int xfree_left(const tpl *t) { return xfree_count(t) > 0; }

static int gscratch_low(tpl *t) {
    int r;
    for (r = 0; r < 4; r++)
        if (!(t->gbusy >> r & 1)) {
            t->gbusy |= 1u << r;
            t->gs[t->ng++] = r;
            return r;
        }
    return -1;
}

static int rexfree_mem(const insn *in) { return (in->base < 8 || in->base == RIP) && in->index < 8; }

static void load16(tpl *t, int x) { enc(t, 0x66, 0, 0, O_MOVDQA, x, &t->m, 0, -1, 0); }

static void xs(tpl *t, int op, int dst) {
    if (t->in->mem) xm(t, op, dst, &t->m);
    else xr(t, op, dst, t->in->rm);
}

static void pshufb_k(tpl *t, int x, int c) {
    mref k = kref(t, c);
    enc(t, 0x66, 0, 0x38, 0x00, x, &k, 0, -1, 0);
}

static int swap0(int lane) {
    int i, imm = 0;
    for (i = 0; i < 4; i++) imm |= (i == 0 ? lane : i == lane ? 0 : i) << 2 * i;
    return imm;
}

static uint8_t *lanemask(tpl *t, int lanes) {
    uint8_t *c = consts(t, 1);
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t v = lanes >> i & 1 ? ~0u : 0;
        memcpy(c + 4 * i, &v, 4);
    }
    return c;
}

static void andmask(tpl *t, int x, const uint8_t *c) {
    mref m = at_rip(c);
    fm(t, 0, F_AND, x, &m, -1);
}

static void rot_insert(tpl *t, int d, int src, int lane) {
    if (lane) xri(t, O_PSHUFD, d, d, swap0(lane));
    fr(t, 0xF3, F_MOV, d, src);
    if (lane) xri(t, O_PSHUFD, d, d, swap0(lane));
}

static void load_words(tpl *t, const int *g, const int *off, int n) {
    int i;
    for (i = 0; i < n; i++) {
        mref m = t->m;
        m.disp += off[i];
        enc(t, 0, 0, 0, 0xB7, g[i], &m, 0, -1, 0);
    }
}

static int src_x(tpl *t, int tmp) {
    const insn *in = t->in;
    if (!in->mem) return in->rm;
    if (in->msize == 16) load16(t, tmp);
    else if (in->msize == 8) enc(t, 0xF3, 0, 0, 0x7E, tmp, &t->m, 0, -1, 0);
    else if (in->msize == 4) enc(t, 0x66, 0, 0, O_MOVD, tmp, &t->m, 0, -1, 0);
    else enc(t, 0x66, 0, 0, O_PINSRW, tmp, &t->m, 0, 0, 0);
    return tmp;
}

static int t_pmov(tpl *t) {
    const insn *in = t->in;
    int k = in->kind, d = in->reg, zx = k >= K_PMOVZXBW && k <= K_PMOVZXDQ, s = -1, shuf = -1;
    int from = k == K_PMOVSXBW || k == K_PMOVSXBD || k == K_PMOVSXBQ || k == K_PMOVZXBW || k == K_PMOVZXBD ||
                       k == K_PMOVZXBQ
                   ? 8
               : k == K_PMOVSXDQ || k == K_PMOVZXDQ ? 32
                                                    : 16;
    int to = k == K_PMOVSXBW || k == K_PMOVZXBW                                                 ? 16
             : k == K_PMOVSXBD || k == K_PMOVSXWD || k == K_PMOVZXBD || k == K_PMOVZXWD ? 32
                                                                                          : 64;
    int bias = !zx && to == 64;
    if (g_pshufb) {
        if (zx || to == 64)
            shuf = from == 8    ? (to == 16 ? C_ZXBW : to == 32 ? C_ZXBD : C_ZXBQ)
                   : from == 16 ? (to == 32 ? C_ZXWD : C_ZXWQ)
                                : C_ZXDQ;
        else if (from == 8 && to == 32)
            shuf = C_SXBD;
    }
    if (bias && shuf < 0 && from == 32 && xfree_left(t)) {
        bias = 0;
        s = xscratch(t);
    }
    prologue(t);
    if (in->mem) src_x(t, d);
    else if (in->rm != d) xr(t, O_MOVDQA, d, in->rm);
    if (shuf >= 0) {
        pshufb_k(t, d, shuf);
        if (shuf == C_SXBD) xsh(t, O_SH32, SH_SRA, d, 24);
    } else if (s >= 0) {
        xr(t, O_MOVDQA, s, d);
        xsh(t, O_SH32, SH_SRA, s, 31);
        xr(t, O_PUNPCKLDQ, d, s);
    } else {
        if (from == 8) xr(t, O_PUNPCKLBW, d, d);
        if (from <= 16 && to >= 32) xr(t, O_PUNPCKLWD, d, d);
        if (to == 64) xr(t, O_PUNPCKLDQ, d, d);
        xsh(t, to == 16 ? O_SH16 : to == 32 ? O_SH32 : O_SH64, zx || bias ? SH_SRL : SH_SRA, d, to - from);
    }
    if (bias) {
        mref c = kref(t, from == 8 ? C_Q80 : from == 16 ? C_Q8000 : C_Q80000000);
        xm(t, O_PXOR, d, &c);
        xm(t, O_PSUBQ, d, &c);
    }
    epilogue(t);
    return 1;
}

static int t_blendi(tpl *t) {
    const insn *in = t->in;
    int k = in->kind, d = in->reg, s = in->rm, i, t0 = -1, n = 0, g[8], off[8], word[8];
    unsigned wm = 0;
    uint8_t *c;
    v128 m;
    mref keep;
    if (k == K_BLENDPS) {
        for (i = 0; i < 4; i++)
            if (in->imm >> i & 1) wm |= 3u << 2 * i;
    } else if (k == K_BLENDPD) {
        for (i = 0; i < 2; i++)
            if (in->imm >> i & 1) wm |= 15u << 4 * i;
    } else {
        wm = in->imm;
    }
    if (!in->mem && (s == d || !wm || wm == 0xFF || wm == 0x03 || wm == 0x0F || wm == 0xF0)) {
        prologue(t);
        if (s == d || !wm) {
        } else if (wm == 0xFF) {
            xr(t, O_MOVDQA, d, s);
        } else if (wm != 0xF0) {
            fr(t, wm == 0x03 ? 0xF3 : 0xF2, F_MOV, d, s);
        } else {
            enc(t, 0x66, 0, 0, F_SHUF, d, NULL, s, 2, 0);
        }
        epilogue(t);
        return 1;
    }
    if (in->mem && (wm == 0xFF || wm == 0x0F || wm == 0xF0)) {
        prologue(t);
        if (wm == 0xFF) {
            load16(t, d);
        } else {
            mref h = t->m;
            if (wm == 0xF0) h.disp += 8;
            fm(t, 0x66, wm == 0x0F ? F_MOVLPS : F_MOVHPS, d, &h, -1);
        }
        epilogue(t);
        return 1;
    }
    if (in->mem && __builtin_popcount(wm) <= 2 && !xfree_left(t)) {
        for (i = 0; i < 8; i++)
            if (wm >> i & 1) {
                word[n] = i;
                off[n] = 2 * i;
                g[n++] = gscratch(t);
            }
        prologue(t);
        load_words(t, g, off, n);
        for (i = 0; i < n; i++) xri(t, O_PINSRW, d, g[i], word[i]);
        epilogue(t);
        return 1;
    }
    memset(&m, 0, sizeof m);
    for (i = 0; i < 8; i++) m.w[i] = wm >> i & 1 ? 0 : 0xFFFF;
    if (in->mem || xfree_left(t)) {
        mref sel;
        c = consts(t, 2);
        memcpy(c + 16, &m, 16);
        m.q[0] = ~m.q[0];
        m.q[1] = ~m.q[1];
        memcpy(c, &m, 16);
        t0 = xscratch(t);
        prologue(t);
        if (in->mem) load16(t, t0);
        else xr(t, O_MOVDQA, t0, s);
        sel = at_rip(c);
        keep = at_rip(c + 16);
        xm(t, O_PAND, t0, &sel);
        xm(t, O_PAND, d, &keep);
        xr(t, O_POR, d, t0);
        epilogue(t);
        return 1;
    }
    c = consts(t, 1);
    memcpy(c, &m, 16);
    prologue(t);
    keep = at_rip(c);
    xr(t, O_PXOR, d, s);
    xm(t, O_PAND, d, &keep);
    xr(t, O_PXOR, d, s);
    epilogue(t);
    return 1;
}

static int t_blendv(tpl *t) {
    const insn *in = t->in;
    int k = in->kind, d = in->reg, s = in->rm, t1, t0 = -1;
    if (!in->mem && s == d) {
        prologue(t);
        epilogue(t);
        return 1;
    }
    t->xbusy |= 1;
    t1 = xscratch(t);
    if (in->mem) t0 = xscratch(t);
    prologue(t);
    if (in->mem) load16(t, t0);
    if (k == K_PBLENDVB) {
        xr(t, O_PXOR, t1, t1);
        xr(t, O_PCMPGTB, t1, 0);
    } else if (k == K_BLENDVPD) {
        xri(t, O_PSHUFD, t1, 0, 0xF5);
        xsh(t, O_SH32, SH_SRA, t1, 31);
    } else {
        xr(t, O_MOVDQA, t1, 0);
        xsh(t, O_SH32, SH_SRA, t1, 31);
    }
    if (in->mem) {
        xr(t, O_PXOR, t0, d);
        xr(t, O_PAND, t0, t1);
        xr(t, O_PXOR, d, t0);
    } else {
        xr(t, O_PXOR, d, s);
        xr(t, O_PAND, t1, d);
        xr(t, O_PXOR, d, s);
        xr(t, O_PXOR, d, t1);
    }
    epilogue(t);
    return 1;
}

static int t_ptest(tpl *t) {
    const insn *in = t->in;
    int d = in->reg, t1, t0 = -1, g1, g2;
    mref c7f = kref(t, C_B7F);
    if (!in->mem && in->rm == d) {
        mref m;
        if (xfree_left(t)) {
            t1 = xscratch(t);
            g1 = gscratch(t);
            prologue(t);
            xr(t, O_MOVDQA, t1, d);
            xm(t, O_PADDUSB, t1, &c7f);
            xr(t, O_PMOVMSKB, g1, t1);
        } else {
            g1 = gscratch(t);
            g2 = gscratch(t);
            prologue(t);
            xr(t, O_PMOVMSKB, g1, d);
            xm(t, O_PADDB, d, &c7f);
            xr(t, O_PMOVMSKB, g2, d);
            xm(t, O_PSUBB, d, &c7f);
            enc(t, 0, 0, -1, 0x09, g2, NULL, g1, -1, 0);
        }
        m.base = g1;
        m.index = g1;
        m.scale = 1;
        m.disp = 1;
        enc(t, 0, 0, -1, 0x8D, g1, &m, 0, -1, 0);
        enc(t, 0, 0, -1, 0xD1, 5, NULL, g1, -1, 0);
        epilogue(t);
        return 1;
    }
    t1 = xscratch(t);
    if (in->mem) t0 = xscratch(t);
    g1 = gscratch(t);
    g2 = gscratch(t);
    prologue(t);
    if (in->mem) {
        load16(t, t0);
        xr(t, O_MOVDQA, t1, d);
        xr(t, O_PAND, t1, t0);
        xr(t, O_PXOR, t0, t1);
        xm(t, O_PADDUSB, t1, &c7f);
        xr(t, O_PMOVMSKB, g1, t1);
        xm(t, O_PADDUSB, t0, &c7f);
        xr(t, O_PMOVMSKB, g2, t0);
    } else {
        xr(t, O_MOVDQA, t1, d);
        xr(t, O_PAND, t1, in->rm);
        xm(t, O_PADDUSB, t1, &c7f);
        xr(t, O_PMOVMSKB, g1, t1);
        xr(t, O_MOVDQA, t1, d);
        xr(t, O_PANDN, t1, in->rm);
        xm(t, O_PADDUSB, t1, &c7f);
        xr(t, O_PMOVMSKB, g2, t1);
    }
    enc(t, 0, 0, -1, 0x83, 7, NULL, g2, 1, 0);
    enc(t, 0, 0, -1, 0x13, g1, NULL, g1, -1, 0);
    enc(t, 0, 0, -1, 0xD1, 5, NULL, g1, -1, 0);
    epilogue(t);
    return 1;
}

static int t_pcmpq(tpl *t) {
    const insn *in = t->in;
    int d = in->reg, s = in->rm, t1, t2, t0;
    if (!in->mem && s == d) {
        prologue(t);
        xr(t, in->kind == K_PCMPEQQ ? O_PCMPEQD : O_PXOR, d, d);
        epilogue(t);
        return 1;
    }
    if (in->kind == K_PCMPEQQ) {
        t1 = xscratch(t);
        prologue(t);
        xs(t, O_PCMPEQD, d);
        xri(t, O_PSHUFD, t1, d, 0xB1);
        xr(t, O_PAND, d, t1);
        epilogue(t);
        return 1;
    }
    t1 = xscratch(t);
    t2 = xscratch(t);
    if (in->mem) {
        t0 = xscratch(t);
        prologue(t);
        load16(t, t0);
        xr(t, O_MOVDQA, t1, d);
        xr(t, O_PCMPGTD, t1, t0);
        xr(t, O_MOVDQA, t2, d);
        xr(t, O_PCMPEQD, t2, t0);
        xr(t, O_PSUBQ, t0, d);
        xr(t, O_PAND, t0, t2);
        xr(t, O_POR, t0, t1);
        xsh(t, O_SH32, SH_SRA, t0, 31);
        xri(t, O_PSHUFD, d, t0, 0xF5);
    } else {
        prologue(t);
        xr(t, O_MOVDQA, t1, s);
        xr(t, O_PSUBQ, t1, d);
        xr(t, O_MOVDQA, t2, d);
        xr(t, O_PCMPEQD, t2, s);
        xr(t, O_PAND, t1, t2);
        xr(t, O_MOVDQA, t2, d);
        xr(t, O_PCMPGTD, t2, s);
        xr(t, O_POR, t1, t2);
        xsh(t, O_SH32, SH_SRA, t1, 31);
        xri(t, O_PSHUFD, d, t1, 0xF5);
    }
    epilogue(t);
    return 1;
}

static int t_minmax(tpl *t) {
    const insn *in = t->in;
    int k = in->kind, d = in->reg, s = in->rm, t1, t0;
    int min = k == K_PMINSB || k == K_PMINSD || k == K_PMINUD || k == K_PMINUW;
    if (!in->mem && s == d) {
        prologue(t);
        epilogue(t);
        return 1;
    }
    if (k == K_PMINSB || k == K_PMAXSB) {
        mref b = kref(t, C_B80);
        t1 = xscratch(t);
        prologue(t);
        if (in->mem) load16(t, t1);
        else xr(t, O_MOVDQA, t1, s);
        xm(t, O_PXOR, t1, &b);
        xm(t, O_PXOR, d, &b);
        xr(t, min ? O_PMINUB : O_PMAXUB, d, t1);
        xm(t, O_PXOR, d, &b);
    } else if (k == K_PMAXUW) {
        t0 = in->mem ? xscratch(t) : s;
        prologue(t);
        if (in->mem) load16(t, t0);
        xr(t, O_PSUBUSW, d, t0);
        xr(t, O_PADDW, d, t0);
    } else if (k == K_PMINUW) {
        t1 = xscratch(t);
        prologue(t);
        xr(t, O_MOVDQA, t1, d);
        xs(t, O_PSUBUSW, t1);
        xr(t, O_PSUBW, d, t1);
    } else if (k == K_PMINSD || k == K_PMAXSD) {
        t1 = xscratch(t);
        t0 = in->mem ? xscratch(t) : -1;
        prologue(t);
        if (in->mem) {
            load16(t, t0);
            xr(t, O_MOVDQA, t1, min ? d : t0);
            xr(t, O_PCMPGTD, t1, min ? t0 : d);
            xr(t, O_PXOR, t0, d);
            xr(t, O_PAND, t0, t1);
            xr(t, O_PXOR, d, t0);
        } else {
            xr(t, O_MOVDQA, t1, min ? d : s);
            xr(t, O_PCMPGTD, t1, min ? s : d);
            xr(t, O_PXOR, d, s);
            xr(t, O_PAND, t1, d);
            xr(t, O_PXOR, d, s);
            xr(t, O_PXOR, d, t1);
        }
    } else if (!in->mem || xfree_count(t) >= 3) {
        mref b = kref(t, C_SIGN_PS);
        int t2;
        t1 = xscratch(t);
        t2 = xscratch(t);
        t0 = in->mem ? xscratch(t) : s;
        prologue(t);
        if (in->mem) load16(t, t0);
        xm(t, O_MOVDQA, t1, &b);
        xr(t, O_MOVDQA, t2, t1);
        xr(t, O_PXOR, t1, min ? d : t0);
        xr(t, O_PXOR, t2, min ? t0 : d);
        xr(t, O_PCMPGTD, t1, t2);
        xr(t, O_MOVDQA, t2, d);
        xr(t, O_PXOR, t2, t0);
        xr(t, O_PAND, t2, t1);
        xr(t, O_PXOR, d, t2);
    } else {
        mref b = kref(t, C_SIGN_PS);
        t0 = xscratch(t);
        t1 = xscratch(t);
        prologue(t);
        load16(t, t0);
        xm(t, O_PXOR, t0, &b);
        xm(t, O_PXOR, d, &b);
        xr(t, O_MOVDQA, t1, min ? d : t0);
        xr(t, O_PCMPGTD, t1, min ? t0 : d);
        xr(t, O_PXOR, t0, d);
        xr(t, O_PAND, t0, t1);
        xr(t, O_PXOR, d, t0);
        xm(t, O_PXOR, d, &b);
    }
    epilogue(t);
    return 1;
}

static int t_pmulld(tpl *t) {
    const insn *in = t->in;
    int d = in->reg, s = in->rm, same = !in->mem && s == d, t1, t2 = -1;
    t1 = xscratch(t);
    if (!same) t2 = xscratch(t);
    prologue(t);
    if (same) {
        xri(t, O_PSHUFD, t1, d, 0xF5);
        xr(t, O_PMULUDQ, t1, t1);
        xr(t, O_PMULUDQ, d, d);
    } else if (in->mem) {
        load16(t, t2);
        xri(t, O_PSHUFD, t1, d, 0xF5);
        xr(t, O_PMULUDQ, d, t2);
        xri(t, O_PSHUFD, t2, t2, 0xF5);
        xr(t, O_PMULUDQ, t1, t2);
    } else {
        xri(t, O_PSHUFD, t1, d, 0xF5);
        xri(t, O_PSHUFD, t2, s, 0xF5);
        xr(t, O_PMULUDQ, t1, t2);
        xr(t, O_PMULUDQ, d, s);
    }
    xri(t, O_PSHUFD, d, d, 0x08);
    xri(t, O_PSHUFD, t1, t1, 0x08);
    xr(t, O_PUNPCKLDQ, d, t1);
    epilogue(t);
    return 1;
}

static int t_pmuldq(tpl *t) {
    const insn *in = t->in;
    int d = in->reg, t1, t2, t0 = -1, s;
    t1 = xscratch(t);
    t2 = xscratch(t);
    if (in->mem) t0 = xscratch(t);
    prologue(t);
    s = src_x(t, t0);
    xr(t, O_MOVDQA, t1, d);
    xsh(t, O_SH32, SH_SRA, t1, 31);
    xr(t, O_PAND, t1, s);
    xr(t, O_MOVDQA, t2, s);
    xsh(t, O_SH32, SH_SRA, t2, 31);
    xr(t, O_PAND, t2, d);
    xr(t, O_PADDD, t1, t2);
    xsh(t, O_SH64, SH_SLL, t1, 32);
    xr(t, O_PMULUDQ, d, s);
    xr(t, O_PSUBQ, d, t1);
    epilogue(t);
    return 1;
}

static int t_packusdw(tpl *t) {
    const insn *in = t->in;
    int d = in->reg, s = in->rm, same = !in->mem && s == d, t1, t2;
    mref half = kref(t, C_D8000), flip = kref(t, C_W8000), ones = kref(t, C_ONES);
    t1 = xscratch(t);
    t2 = in->mem ? xscratch(t) : t1;
    prologue(t);
    if (in->mem) {
        load16(t, t1);
        xr(t, O_MOVDQA, t2, t1);
        xm(t, O_PCMPGTD, t2, &ones);
        xr(t, O_PAND, t1, t2);
        xm(t, O_PSUBD, t1, &half);
    }
    xr(t, O_MOVDQA, t2, d);
    xm(t, O_PCMPGTD, t2, &ones);
    xr(t, O_PAND, d, t2);
    xm(t, O_PSUBD, d, &half);
    if (!in->mem && !same) {
        xr(t, O_MOVDQA, t1, s);
        xsh(t, O_SH32, SH_SRA, t1, 31);
        xr(t, O_PANDN, t1, s);
        xm(t, O_PSUBD, t1, &half);
    }
    xr(t, O_PACKSSDW, d, same ? d : t1);
    xm(t, O_PXOR, d, &flip);
    epilogue(t);
    return 1;
}

static int t_movntdqa(tpl *t) {
    prologue(t);
    load16(t, t->in->reg);
    epilogue(t);
    return 1;
}

static int t_round(tpl *t) {
    const insn *in = t->in;
    int k = in->kind, dbl = k == K_ROUNDPD || k == K_ROUNDSD, scal = k == K_ROUNDSS || k == K_ROUNDSD;
    int mode = in->imm & 4 ? 4 : in->imm & 3, ap = scal ? (dbl ? 0xF2 : 0xF3) : (dbl ? 0x66 : 0);
    int c = dbl ? C_SIGN_PD : C_SIGN_PS, d = in->reg, x, t1, t2, t3, t0 = -1, r, a;
    int rd = !scal && (in->mem || in->rm != d);
    mref sign = kref(t, c), abs = kref(t, c + 1), magic = kref(t, c + 2), one = kref(t, c + 3);
    if (!mode) return 0;
    t1 = xscratch(t);
    if (mode == 3) {
        t2 = xscratch(t);
        t3 = rd ? d : xscratch(t);
    } else {
        t3 = xscratch(t);
        t2 = rd ? d : xscratch(t);
    }
    if (in->mem) t0 = xscratch(t);
    prologue(t);
    if (in->mem) {
        if (scal) fm(t, ap, F_MOV, t0, &t->m, -1);
        else fm(t, 0, F_MOVAPS, t0, &t->m, -1);
        x = t0;
    } else {
        x = in->rm;
    }
    if (mode == 3) {
        fr(t, 0, F_MOVAPS, t2, x);
        fm(t, 0, F_AND, t2, &abs, -1);
        fr(t, 0, F_MOVAPS, t3, t2);
        fm(t, ap, F_ADD, t3, &magic, -1);
        fm(t, ap, F_SUB, t3, &magic, -1);
        fr(t, 0, F_MOVAPS, t1, t2);
        enc(t, ap, 0, 0, F_CMP, t1, NULL, t3, 1, 0);
        fm(t, 0, F_AND, t1, &one, -1);
        fr(t, ap, F_SUB, t3, t1);
        r = t3;
        a = t2;
    } else {
        fr(t, 0, F_MOVAPS, t1, x);
        fm(t, 0, F_AND, t1, &sign, -1);
        fm(t, 0, F_OR, t1, &magic, -1);
        fr(t, 0, F_MOVAPS, t2, x);
        fr(t, ap, F_ADD, t2, t1);
        fr(t, ap, F_SUB, t2, t1);
        if (mode == 1) {
            fr(t, 0, F_MOVAPS, t1, x);
            enc(t, ap, 0, 0, F_CMP, t1, NULL, t2, 1, 0);
            fm(t, 0, F_AND, t1, &one, -1);
            fr(t, ap, F_SUB, t2, t1);
        } else if (mode == 2) {
            fr(t, 0, F_MOVAPS, t1, t2);
            enc(t, ap, 0, 0, F_CMP, t1, NULL, x, 1, 0);
            fm(t, 0, F_AND, t1, &one, -1);
            fr(t, ap, F_ADD, t2, t1);
        }
        r = t2;
        a = t3;
        fr(t, 0, F_MOVAPS, a, x);
        fm(t, 0, F_AND, a, &abs, -1);
    }
    fm(t, 0, F_MOVAPS, t1, &magic, -1);
    enc(t, ap, 0, 0, F_CMP, t1, NULL, a, 6, 0);
    fr(t, 0, F_AND, r, t1);
    fr(t, 0, F_ANDN, t1, x);
    fr(t, 0, F_OR, r, t1);
    fm(t, 0, F_AND, r, &abs, -1);
    fr(t, 0, F_MOVAPS, t1, x);
    fm(t, 0, F_AND, t1, &sign, -1);
    fr(t, 0, F_OR, r, t1);
    if (scal) fr(t, ap, F_MOV, d, r);
    else if (r != d) fr(t, 0, F_MOVAPS, d, r);
    epilogue(t);
    return 1;
}

static int t_insertps(tpl *t) {
    const insn *in = t->in;
    int imm = in->imm, cd = imm >> 4 & 3, cs = imm >> 6 & 3, zm = imm & 15, d = in->reg, s = in->rm, t0 = -1, i;
    int live = 15 & ~zm, keep = live & ~(1 << cd);
    uint8_t *c = NULL;
    if (!(live >> cd & 1)) {
        if (keep) c = lanemask(t, keep);
        prologue(t);
        if (keep) andmask(t, d, c);
        else xr(t, O_PXOR, d, d);
        epilogue(t);
        return 1;
    }
    if (in->mem) {
        int hi = cd >= 2 && !(live >> (5 - cd) & 1), x = xfree_left(t), need;
        if (!keep) {
            prologue(t);
            fm(t, 0xF3, F_MOV, d, &t->m, -1);
            if (cd) {
                int p = 0;
                for (i = 0; i < 4; i++) p |= (i == cd ? 0 : 1) << 2 * i;
                xri(t, O_PSHUFD, d, d, p);
            }
            epilogue(t);
            return 1;
        }
        need = zm & ~(hi && x ? 1 << (5 - cd) : 0);
        if (need) c = lanemask(t, live);
        if (x) {
            t0 = xscratch(t);
            prologue(t);
            fm(t, 0xF3, F_MOV, t0, &t->m, -1);
            if (hi) enc(t, 0, 0, 0, F_SHUF, d, NULL, t0, cd == 2 ? 0x44 : 0x14, 0);
            else rot_insert(t, d, t0, cd);
        } else {
            int g[2], off[2] = {0, 2};
            g[0] = gscratch(t);
            g[1] = gscratch(t);
            prologue(t);
            load_words(t, g, off, 2);
            xri(t, O_PINSRW, d, g[0], 2 * cd);
            xri(t, O_PINSRW, d, g[1], 2 * cd + 1);
        }
        if (need) andmask(t, d, c);
        epilogue(t);
        return 1;
    }
    if (!keep) {
        c = lanemask(t, 1 << cd);
        prologue(t);
        xri(t, O_PSHUFD, d, s, cs * 0x55);
        andmask(t, d, c);
        epilogue(t);
        return 1;
    }
    if (cd >= 2 && !(live >> (5 - cd) & 1)) {
        c = lanemask(t, live);
        prologue(t);
        enc(t, 0, 0, 0, F_SHUF, d, NULL, s, 0x04 | cs << 4 | cs << 6, 0);
        andmask(t, d, c);
        epilogue(t);
        return 1;
    }
    if (s == d || (xfree_left(t) && (cd || cs))) {
        v128 kp, pt;
        mref a, b;
        c = consts(t, 2);
        for (i = 0; i < 4; i++) {
            kp.d[i] = keep >> i & 1 ? ~0u : 0;
            pt.d[i] = i == cd ? ~0u : 0;
        }
        memcpy(c, &kp, 16);
        memcpy(c + 16, &pt, 16);
        t0 = xscratch(t);
        prologue(t);
        xri(t, O_PSHUFD, t0, s, cs * 0x55);
        a = at_rip(c);
        b = at_rip(c + 16);
        xm(t, O_PAND, t0, &b);
        xm(t, O_PAND, d, &a);
        xr(t, O_POR, d, t0);
        epilogue(t);
        return 1;
    }
    if (zm) c = lanemask(t, live);
    if (cs && xfree_left(t)) t0 = xscratch(t);
    prologue(t);
    if (t0 >= 0) xri(t, O_PSHUFD, t0, s, cs);
    else if (cs) xri(t, O_PSHUFD, s, s, swap0(cs));
    rot_insert(t, d, t0 >= 0 ? t0 : s, cd);
    if (cs && t0 < 0) xri(t, O_PSHUFD, s, s, swap0(cs));
    if (zm) andmask(t, d, c);
    epilogue(t);
    return 1;
}

static int t_pinsrb(tpl *t) {
    const insn *in = t->in;
    int lane = in->imm & 15, d = in->reg, w = lane >> 1, odd = lane & 1, r = in->rm, t0, g, x = xfree_left(t);
    uint8_t *c;
    v128 m;
    mref keep;
    if (!x && (!odd || (!in->mem && g_pshufb))) {
        g = gscratch(t);
        prologue(t);
        if (odd) pshufb_k(t, d, C_SWAPB);
        xri(t, O_PEXTRW, g, d, w);
        if (in->mem) enc(t, 0, 0, -1, 0x8A, g, &t->m, 0, -1, g >= 4 && g < 8);
        else enc(t, 0, 0, -1, 0x88, r, NULL, g, -1, (r >= 4 && r < 8) || (g >= 4 && g < 8));
        xri(t, O_PINSRW, d, g, w);
        if (odd) pshufb_k(t, d, C_SWAPB);
        epilogue(t);
        return 1;
    }
    if (!x && (in->mem ? rexfree_mem(in) : r < 4) && (g = gscratch_low(t)) >= 0) {
        prologue(t);
        xri(t, O_PEXTRW, g, d, w);
        if (in->mem) enc(t, 0, 0, -1, 0x8A, g + 4, &t->m, 0, -1, 0);
        else enc(t, 0, 0, -1, 0x88, r, NULL, g + 4, -1, 0);
        xri(t, O_PINSRW, d, g, w);
        epilogue(t);
        return 1;
    }
    c = consts(t, 1);
    memset(&m, 0xFF, sizeof m);
    m.b[lane] = 0;
    memcpy(c, &m, 16);
    t0 = xscratch(t);
    g = in->mem ? gscratch(t) : -1;
    prologue(t);
    if (in->mem) {
        enc(t, 0, 0, 0, 0xB6, g, &t->m, 0, -1, 0);
        enc(t, 0x66, 0, 0, O_MOVD, t0, NULL, g, -1, 0);
        xsh(t, O_SH64, SH_SLLDQ, t0, lane);
    } else {
        enc(t, 0x66, 0, 0, O_MOVD, t0, NULL, r, -1, 0);
        xsh(t, O_SH64, SH_SLLDQ, t0, 15);
        if (lane != 15) xsh(t, O_SH64, SH_SRLDQ, t0, 15 - lane);
    }
    keep = at_rip(c);
    xm(t, O_PAND, d, &keep);
    xr(t, O_POR, d, t0);
    epilogue(t);
    return 1;
}

static int t_pinsrd(tpl *t) {
    const insn *in = t->in;
    int q = in->w, lane = in->imm & (q ? 1 : 3), d = in->reg, t0;
    uint8_t *c = NULL;
    if (in->mem && q) {
        prologue(t);
        fm(t, 0, lane ? F_MOVHPS : F_MOVLPS, d, &t->m, -1);
        epilogue(t);
        return 1;
    }
    if (in->mem && !xfree_left(t)) {
        int g[2], off[2] = {0, 2};
        g[0] = gscratch(t);
        g[1] = gscratch(t);
        prologue(t);
        load_words(t, g, off, 2);
        xri(t, O_PINSRW, d, g[0], 2 * lane);
        xri(t, O_PINSRW, d, g[1], 2 * lane + 1);
        epilogue(t);
        return 1;
    }
    if (!q && lane) {
        v128 m;
        c = consts(t, 1);
        memset(&m, 0xFF, sizeof m);
        m.d[lane] = 0;
        memcpy(c, &m, 16);
    }
    t0 = xscratch(t);
    prologue(t);
    if (q) {
        enc(t, 0x66, 1, 0, O_MOVD, t0, NULL, in->rm, -1, 0);
        if (lane) xr(t, O_PUNPCKLQDQ, d, t0);
        else fr(t, 0xF2, F_MOV, d, t0);
    } else {
        enc(t, 0x66, 0, 0, O_MOVD, t0, in->mem ? &t->m : NULL, in->rm, -1, 0);
        if (!lane) {
            fr(t, 0xF3, F_MOV, d, t0);
        } else {
            mref keep = at_rip(c);
            xsh(t, O_SH64, SH_SLLDQ, t0, 4 * lane);
            xm(t, O_PAND, d, &keep);
            xr(t, O_POR, d, t0);
        }
    }
    epilogue(t);
    return 1;
}

static int t_pextr(tpl *t) {
    const insn *in = t->in;
    int k = in->kind, x = in->reg, q = k == K_PEXTRD && in->w, t0, g = -1, w, odd, r = in->rm;
    int lane = k == K_PEXTRB ? in->imm & 15 : k == K_PEXTRW ? in->imm & 7 : q ? in->imm & 1 : in->imm & 3;
    if (k == K_PEXTRW) {
        if (in->mem) g = gscratch(t);
        prologue(t);
        xri(t, O_PEXTRW, in->mem ? g : r, x, lane);
        if (in->mem) enc(t, 0x66, 0, -1, 0x89, g, &t->m, 0, -1, 0);
        epilogue(t);
        return 1;
    }
    if (k != K_PEXTRB) {
        if (q && lane && in->mem) {
            prologue(t);
            fm(t, 0, F_MOVHPS_ST, x, &t->m, -1);
            epilogue(t);
            return 1;
        }
        if (!lane || xfree_left(t)) {
            int src = x;
            t0 = lane ? xscratch(t) : -1;
            prologue(t);
            if (lane) {
                xri(t, O_PSHUFD, t0, x, q ? 0xEE : lane);
                src = t0;
            }
            if (in->mem) enc(t, 0x66, 0, 0, q ? O_MOVQST : O_MOVDST, src, &t->m, 0, -1, 0);
            else enc(t, 0x66, q, 0, O_MOVDST, src, NULL, r, -1, 0);
            epilogue(t);
            return 1;
        }
        {
            int sw = q ? 0x4E : swap0(lane);
            if (in->mem) r = gscratch(t);
            prologue(t);
            xri(t, O_PSHUFD, x, x, sw);
            enc(t, 0x66, q, 0, O_MOVDST, x, NULL, r, -1, 0);
            xri(t, O_PSHUFD, x, x, sw);
            if (in->mem) enc(t, 0, 0, -1, 0x89, r, &t->m, 0, -1, 0);
            epilogue(t);
            return 1;
        }
    }
    w = lane >> 1;
    odd = lane & 1;
    if (!in->mem && (!odd || r < 4)) {
        prologue(t);
        xri(t, O_PEXTRW, r, x, w);
        if (odd) enc(t, 0, 0, 0, 0xB6, r, NULL, r + 4, -1, 0);
        else enc(t, 0, 0, 0, 0xB6, r, NULL, r, -1, r >= 4 && r < 8);
        epilogue(t);
        return 1;
    }
    if (in->mem && (!odd || (g_pshufb && !xfree_left(t)))) {
        g = gscratch(t);
        prologue(t);
        if (odd) pshufb_k(t, x, C_SWAPB);
        xri(t, O_PEXTRW, g, x, w);
        if (odd) pshufb_k(t, x, C_SWAPB);
        enc(t, 0, 0, -1, 0x88, g, &t->m, 0, -1, g >= 4 && g < 8);
        epilogue(t);
        return 1;
    }
    if (!in->mem && g_pshufb && !xfree_left(t)) {
        prologue(t);
        pshufb_k(t, x, C_SWAPB);
        xri(t, O_PEXTRW, r, x, w);
        pshufb_k(t, x, C_SWAPB);
        enc(t, 0, 0, 0, 0xB6, r, NULL, r, -1, r >= 4 && r < 8);
        epilogue(t);
        return 1;
    }
    if (!xfree_left(t) && (!in->mem || rexfree_mem(in)) && (g = gscratch_low(t)) >= 0) {
        prologue(t);
        xri(t, O_PEXTRW, g, x, w);
        if (in->mem) {
            enc(t, 0, 0, -1, 0x88, g + 4, &t->m, 0, -1, 0);
        } else if (r < 8) {
            enc(t, 0, 0, 0, 0xB6, r, NULL, g + 4, -1, 0);
        } else {
            enc(t, 0, 0, 0, 0xB6, g, NULL, g + 4, -1, 0);
            enc(t, 0, 0, -1, 0x8B, r, NULL, g, -1, 0);
        }
        epilogue(t);
        return 1;
    }
    t0 = xscratch(t);
    if (in->mem) g = gscratch(t);
    prologue(t);
    xr(t, O_MOVDQA, t0, x);
    xsh(t, O_SH64, SH_SRLDQ, t0, lane);
    if (in->mem) r = g;
    enc(t, 0x66, 0, 0, O_MOVDST, t0, NULL, r, -1, 0);
    if (in->mem) enc(t, 0, 0, -1, 0x88, r, &t->m, 0, -1, r >= 4 && r < 8);
    else enc(t, 0, 0, 0, 0xB6, r, NULL, r, -1, r >= 4 && r < 8);
    epilogue(t);
    return 1;
}

static int t_popcnt(tpl *t) {
    const insn *in = t->in;
    int n = in->msize, w = n == 8, g0, g1;
    mref c55 = kref(t, C_POP55), c33 = kref(t, C_POP33), c0f = kref(t, C_POP0F), c01 = kref(t, C_POP01);
    g0 = n == 2 ? gscratch(t) : in->reg;
    g1 = gscratch(t);
    prologue(t);
    if (n == 2) enc(t, 0, 0, 0, 0xB7, g0, in->mem ? &t->m : NULL, in->rm, -1, 0);
    else if (in->mem || in->rm != g0) enc(t, 0, w, -1, 0x8B, g0, in->mem ? &t->m : NULL, in->rm, -1, 0);
    enc(t, 0, w, -1, 0x8B, g1, NULL, g0, -1, 0);
    enc(t, 0, w, -1, 0xC1, 5, NULL, g1, 1, 0);
    enc(t, 0, w, -1, 0x23, g1, &c55, 0, -1, 0);
    enc(t, 0, w, -1, 0x2B, g0, NULL, g1, -1, 0);
    enc(t, 0, w, -1, 0x8B, g1, NULL, g0, -1, 0);
    enc(t, 0, w, -1, 0xC1, 5, NULL, g1, 2, 0);
    enc(t, 0, w, -1, 0x23, g0, &c33, 0, -1, 0);
    enc(t, 0, w, -1, 0x23, g1, &c33, 0, -1, 0);
    enc(t, 0, w, -1, 0x03, g0, NULL, g1, -1, 0);
    enc(t, 0, w, -1, 0x8B, g1, NULL, g0, -1, 0);
    enc(t, 0, w, -1, 0xC1, 5, NULL, g1, 4, 0);
    enc(t, 0, w, -1, 0x03, g0, NULL, g1, -1, 0);
    enc(t, 0, w, -1, 0x23, g0, &c0f, 0, -1, 0);
    enc(t, 0, w, 0, 0xAF, g0, &c01, 0, -1, 0);
    enc(t, 0, w, -1, 0xC1, 5, NULL, g0, w ? 56 : 24, 0);
    if (n == 2) enc(t, 0x66, 0, -1, 0x89, g0, NULL, in->reg, -1, 0);
    enc(t, 0, w, -1, 0x85, g0, NULL, g0, -1, 0);
    epilogue(t);
    return 1;
}

static int t_palignr(tpl *t) {
    const insn *in = t->in;
    int d = in->reg, k = in->imm, t0 = -1, s;
    if (!in->mem && in->rm == d && k > 0 && k < 16 && !(k & 3)) {
        prologue(t);
        xri(t, O_PSHUFD, d, d, k == 4 ? 0x39 : k == 8 ? 0x4E : 0x93);
        epilogue(t);
        return 1;
    }
    if (k == 8) {
        prologue(t);
        if (in->mem) {
            mref h = t->m;
            h.disp += 8;
            fm(t, 0, F_MOVHPS, d, &h, -1);
            xri(t, O_PSHUFD, d, d, 0x4E);
        } else {
            xri(t, O_PSHUFD, d, d, 0x4E);
            fr(t, 0, F_MOVLPS, d, in->rm);
        }
        epilogue(t);
        return 1;
    }
    if ((k > 0 && k < 16) || (in->mem && k < 16)) t0 = xscratch(t);
    prologue(t);
    if (k >= 32) {
        xr(t, O_PXOR, d, d);
    } else if (k > 16) {
        xsh(t, O_SH64, SH_SRLDQ, d, k - 16);
    } else if (k == 0) {
        s = src_x(t, t0);
        if (s != d) xr(t, O_MOVDQA, d, s);
    } else if (k < 16) {
        s = src_x(t, t0);
        if (s != t0) xr(t, O_MOVDQA, t0, s);
        xsh(t, O_SH64, SH_SRLDQ, t0, k);
        xsh(t, O_SH64, SH_SLLDQ, d, 16 - k);
        xr(t, O_POR, d, t0);
    }
    epilogue(t);
    return 1;
}

static int t_pabs(tpl *t) {
    const insn *in = t->in;
    int d = in->reg, t0, s;
    if (in->kind != K_PABSD && !in->mem && in->rm != d) {
        prologue(t);
        xr(t, O_PXOR, d, d);
        xr(t, in->kind == K_PABSB ? O_PSUBB : O_PSUBW, d, in->rm);
        xr(t, in->kind == K_PABSB ? O_PMINUB : O_PMAXSW, d, in->rm);
        epilogue(t);
        return 1;
    }
    t0 = xscratch(t);
    prologue(t);
    s = src_x(t, t0);
    if (in->kind == K_PABSD) {
        if (s != t0) xr(t, O_MOVDQA, t0, s);
        if (d != s) xr(t, O_MOVDQA, d, s);
        xsh(t, O_SH32, SH_SRA, t0, 31);
        xr(t, O_PXOR, d, t0);
        xr(t, O_PSUBD, d, t0);
    } else {
        if (s == t0) {
            xr(t, O_MOVDQA, d, t0);
            xr(t, O_PXOR, t0, t0);
            xr(t, in->kind == K_PABSB ? O_PSUBB : O_PSUBW, t0, d);
        } else {
            xr(t, O_PXOR, t0, t0);
            xr(t, in->kind == K_PABSB ? O_PSUBB : O_PSUBW, t0, s);
            if (d != s) xr(t, O_MOVDQA, d, s);
        }
        xr(t, in->kind == K_PABSB ? O_PMINUB : O_PMAXSW, d, t0);
    }
    epilogue(t);
    return 1;
}

static int t_phaddd(tpl *t) {
    const insn *in = t->in;
    int d = in->reg, t1, t0 = -1, s, op = in->kind == K_PHADDD ? O_PADDD : O_PSUBD;
    t1 = xscratch(t);
    if (!in->mem && in->rm == d) {
        prologue(t);
        xri(t, O_PSHUFD, t1, d, 0xB1);
        xr(t, op, d, t1);
        xri(t, O_PSHUFD, d, d, 0x88);
        epilogue(t);
        return 1;
    }
    if (in->mem) t0 = xscratch(t);
    prologue(t);
    s = src_x(t, t0);
    fr(t, 0, F_MOVAPS, t1, d);
    enc(t, 0, 0, 0, F_SHUF, t1, NULL, s, 0xDD, 0);
    enc(t, 0, 0, 0, F_SHUF, d, NULL, s, 0x88, 0);
    xr(t, op, d, t1);
    epilogue(t);
    return 1;
}

static int frame_run(uint64_t *g, v128 *x, const insn *in, const uint32_t *mxcsr);

static const uint8_t k_gen_head[] = {0x48, 0x8D, 0x64, 0x24, 0x80, 0x9C, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                     0x41, 0x53, 0x41, 0x52, 0x41, 0x51, 0x41, 0x50, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52,
                                     0x51, 0x50, 0x48, 0x89, 0xE3, 0x48, 0x8D, 0x83, 0x08, 0x01, 0x00, 0x00, 0x48, 0x89,
                                     0x43, 0x20, 0x48, 0x83, 0xE4, 0xF0, 0x48, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00};
static const uint8_t k_gen_args[] = {0x0F, 0xAE, 0x5C, 0x24, 0x20, 0x48, 0x89, 0xD9, 0x48, 0x8D, 0x54, 0x24, 0x30};
static const uint8_t k_gen_r9[] = {0x4C, 0x8D, 0x4C, 0x24, 0x20};
static const uint8_t k_gen_pops[] = {0x48, 0x89, 0xDC, 0x58, 0x59, 0x5A, 0x5B, 0x48, 0x8D, 0x64, 0x24, 0x08, 0x5D, 0x5E,
                                     0x5F, 0x41, 0x58, 0x41, 0x59, 0x41, 0x5A, 0x41, 0x5B, 0x41, 0x5C, 0x41, 0x5D, 0x41,
                                     0x5E, 0x41, 0x5F, 0x75, 0x0E, 0x9D, 0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00};
static const uint8_t k_gen_refault[] = {0x9D, 0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x0B};

static int t_generic(tpl *t) {
    uint8_t *c = consts(t, 3);
    uint64_t fn = (uint64_t)(uintptr_t)frame_run;
    mref desc = at_rip(c), ptr = at_rip(c + 32);
    int64_t rel;
    int i;
    memcpy(c, t->in, sizeof(insn));
    memcpy(c + 32, &fn, 8);
    memset(c + 40, 0, 8);
    t->entry = t->p;
    raw(t, k_gen_head, sizeof k_gen_head);
    for (i = 0; i < 16; i++) {
        mref m = at_rsp(48 + 16 * i);
        enc(t, 0x66, 0, 0, 0x7F, i, &m, 0, -1, 0);
    }
    raw(t, k_gen_args, sizeof k_gen_args);
    enc(t, 0, 1, -1, 0x8D, 8, &desc, 0, -1, 0);
    raw(t, k_gen_r9, sizeof k_gen_r9);
    enc(t, 0, 0, -1, 0xFF, 2, &ptr, 0, -1, 0);
    put(t, 0x85);
    put(t, 0xC0);
    for (i = 0; i < 16; i++) {
        mref m = at_rsp(48 + 16 * i);
        enc(t, 0x66, 0, 0, 0x6F, i, &m, 0, -1, 0);
    }
    raw(t, k_gen_pops, sizeof k_gen_pops);
    put(t, 0xE9);
    rel = (int64_t)t->in->next - (int64_t)((uintptr_t)t->p + 4);
    if (rel != (int32_t)rel) t->bad = 1;
    put32(t, (uint32_t)rel);
    t->refault = t->p;
    raw(t, k_gen_refault, sizeof k_gen_refault);
    t->body = t->p;
    t->desc = c;
    return 1;
}

static uint32_t g_crc_tab[8][256];
static int g_crc_ready, g_lahf;

static void crc_tables(void) {
    int i, k;
    for (i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0x82F63B78u : c >> 1;
        g_crc_tab[0][i] = c;
    }
    for (k = 1; k < 8; k++)
        for (i = 0; i < 256; i++) g_crc_tab[k][i] = g_crc_tab[k - 1][i] >> 8 ^ g_crc_tab[0][g_crc_tab[k - 1][i] & 0xFF];
    g_crc_ready = 1;
}

static void crc_step(tpl *t, int op, int gr, int gt, int idx, int k) {
    mref m;
    m.base = gt;
    m.index = idx;
    m.scale = 4;
    m.disp = 1024 * k;
    enc(t, 0, 0, -1, op, gr, &m, 0, -1, 0);
}

static void crc_bytes(tpl *t, int gx, int gi, int gr, int gt, int nb, int top, int first) {
    int i;
    for (i = 0; i < nb; i++) {
        int last = i == nb - 1 && nb == 4;
        if (i) enc(t, 0, 0, -1, 0xC1, 5, NULL, gx, 8, 0);
        if (!last) enc(t, 0, 0, 0, 0xB6, gi, NULL, gx, -1, gx >= 4 && gx < 8);
        crc_step(t, first && !i ? 0x8B : 0x33, gr, gt, last ? gx : gi, top - i);
    }
}

static int t_crc32(tpl *t) {
    const insn *in = t->in;
    int n = in->msize, d = in->reg, gx, gy = -1, gi, gt, gr, i;
    uint64_t tab = (uint64_t)(uintptr_t)g_crc_tab;
    if (!g_lahf || (!in->mem && n == 1 && !in->rex && in->rm >= 4 && in->rm < 8)) return 0;
    if (!g_crc_ready) crc_tables();
    if (d) {
        t->gbusy |= 1;
        t->gs[t->ng++] = 0;
    }
    gx = gscratch(t);
    if (n == 8) gy = gscratch(t);
    gi = gscratch(t);
    gt = gscratch(t);
    gr = d ? d : gscratch(t);
    if (gx < 0 || gi < 0 || gt < 0 || gr < 0 || (n == 8 && gy < 0)) return 0;
    prologue(t);
    if (n == 8) {
        enc(t, 0, 1, -1, 0x8B, gy, in->mem ? &t->m : NULL, in->rm, -1, 0);
        enc(t, 0, 0, -1, 0x8B, gx, NULL, d, -1, 0);
    } else if (n == 4) {
        enc(t, 0, 0, -1, 0x8B, gx, in->mem ? &t->m : NULL, in->rm, -1, 0);
    } else {
        enc(t, 0, 0, 0, n == 1 ? 0xB6 : 0xB7, gx, in->mem ? &t->m : NULL, in->rm, -1,
            n == 1 && !in->mem && in->rm >= 4 && in->rm < 8);
    }
    if (gr != d) enc(t, 0, 0, -1, 0x8B, gr, NULL, d, -1, 0);
    put(t, 0x9F);
    put(t, 0x0F);
    put(t, 0x90);
    put(t, 0xC0);
    if (n == 8) {
        enc(t, 0, 0, -1, 0x33, gx, NULL, gy, -1, 0);
        enc(t, 0, 1, -1, 0xC1, 5, NULL, gy, 32, 0);
    } else {
        enc(t, 0, 0, -1, 0x33, gx, NULL, gr, -1, 0);
    }
    put(t, 0x48 | (gt >= 8));
    put(t, 0xB8 + (gt & 7));
    for (i = 0; i < 8; i++) put(t, (unsigned)(tab >> 8 * i) & 0xFF);
    if (n < 4) {
        enc(t, 0, 0, -1, 0xC1, 5, NULL, gr, 8 * n, 0);
        crc_bytes(t, gx, gi, gr, gt, n, n - 1, 0);
    } else {
        crc_bytes(t, gx, gi, gr, gt, 4, n - 1, 1);
        if (n == 8) crc_bytes(t, gy, gi, gr, gt, 4, 3, 0);
    }
    put(t, 0x04);
    put(t, 0x7F);
    put(t, 0x9E);
    if (!d) enc(t, 0, 0, -1, 0x8B, 0, NULL, gr, -1, 0);
    epilogue(t);
    return 1;
}

typedef int (*tplfn)(tpl *);

static tplfn template_for(int k) {
    switch (k) {
    case K_PMOVSXBW: case K_PMOVSXBD: case K_PMOVSXBQ: case K_PMOVSXWD: case K_PMOVSXWQ: case K_PMOVSXDQ:
    case K_PMOVZXBW: case K_PMOVZXBD: case K_PMOVZXBQ: case K_PMOVZXWD: case K_PMOVZXWQ: case K_PMOVZXDQ:
        return t_pmov;
    case K_BLENDPS: case K_BLENDPD: case K_PBLENDW:
        return t_blendi;
    case K_PBLENDVB: case K_BLENDVPS: case K_BLENDVPD:
        return t_blendv;
    case K_PTEST:
        return t_ptest;
    case K_PCMPEQQ: case K_PCMPGTQ:
        return t_pcmpq;
    case K_PMINSB: case K_PMINSD: case K_PMINUW: case K_PMINUD: case K_PMAXSB: case K_PMAXSD: case K_PMAXUW:
    case K_PMAXUD:
        return t_minmax;
    case K_PMULLD:
        return t_pmulld;
    case K_PMULDQ:
        return t_pmuldq;
    case K_PACKUSDW:
        return t_packusdw;
    case K_MOVNTDQA:
        return t_movntdqa;
    case K_ROUNDPS: case K_ROUNDPD: case K_ROUNDSS: case K_ROUNDSD:
        return t_round;
    case K_INSERTPS:
        return t_insertps;
    case K_PINSRB:
        return t_pinsrb;
    case K_PINSRD:
        return t_pinsrd;
    case K_PEXTRB: case K_PEXTRW: case K_PEXTRD: case K_EXTRACTPS:
        return t_pextr;
    case K_POPCNT:
        return t_popcnt;
    case K_PALIGNR:
        return t_palignr;
    case K_PABSB: case K_PABSW: case K_PABSD:
        return t_pabs;
    case K_PHADDD: case K_PHSUBD:
        return t_phaddd;
    case K_CRC32:
        return t_crc32;
    default:
        return NULL;
    }
}

static void busy(tpl *t) {
    const insn *in = t->in;
    int k = in->kind;
    t->gbusy = 1u << 4;
    if (in->mem && in->base >= 0 && in->base < RIP) t->gbusy |= 1u << in->base;
    if (in->mem && in->index >= 0) t->gbusy |= 1u << in->index;
    if (k == K_POPCNT || k == K_CRC32) {
        t->gbusy |= 1u << in->reg;
        if (!in->mem) t->gbusy |= 1u << in->rm;
        return;
    }
    t->xbusy |= 1u << in->reg;
    if (!in->mem) {
        if (k == K_PEXTRB || k == K_PEXTRW || k == K_PEXTRD || k == K_EXTRACTPS || k == K_PINSRB || k == K_PINSRD)
            t->gbusy |= 1u << in->rm;
        else
            t->xbusy |= 1u << in->rm;
    }
}

#define ARENA_SIZE (2u << 20)
#define STUB_MAX 768u
#define REACH (1ull << 30)

typedef struct {
    uint64_t start, body, end, site, refault, tail, rest;
    const insn *desc;
    int32_t delta;
    uint8_t nx, ng, xs[4], gs[6], generic;
#ifdef BLOX7_ISA_TEST
    uint64_t *counter;
    uint8_t kind;
#endif
} stubrec;

typedef struct {
    uint8_t *base;
    size_t used;
    stubrec *recs;
    size_t nrec, caprec;
} arena;

typedef struct {
    uint64_t site;
    uint32_t state, hits;
} slot;

enum { S_PENDING = 1, S_PATCHED, S_TRAP };

typedef struct {
    uintptr_t base, lo, hi;
#ifdef BLOX7_ISA_TEST
    char name[48];
#endif
} coderange;

#ifdef BLOX7_ISA_TEST
#define ARENA_PROTECT PAGE_EXECUTE_READWRITE
#else
#define ARENA_PROTECT PAGE_EXECUTE_READ
#endif

static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile DWORD g_owner;
static void *g_veh;
static int g_patching = 1, g_bulk, g_missing;
static arena g_arenas[64];
static int g_narenas;
static coderange g_code[256];
static int g_ncode;
static slot *g_slots;
static size_t g_cap, g_used;

#ifdef BLOX7_ISA_TEST
static void poison(uintptr_t base, uintptr_t lo, uintptr_t hi, const char *path);
static int poisoned(uint64_t at, uint8_t *out);
static void test_count(uint64_t site, const insn *in);
static int test_cpuid(CONTEXT *c);
#endif

static size_t slot_hash(uint64_t site, size_t cap) {
    return (size_t)((site * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
}

static slot *slot_find(uint64_t site) {
    size_t i;
    if (!g_cap) return NULL;
    for (i = slot_hash(site, g_cap);; i = (i + 1) & (g_cap - 1)) {
        if (g_slots[i].site == site) return &g_slots[i];
        if (!g_slots[i].site) return NULL;
    }
}

static slot *slot_add(uint64_t site) {
    size_t i;
    if ((g_used + 1) * 2 > g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 4096, j;
        slot *n = VirtualAlloc(NULL, cap * sizeof(slot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!n) return NULL;
        for (j = 0; j < g_cap; j++) {
            if (!g_slots[j].site) continue;
            for (i = slot_hash(g_slots[j].site, cap); n[i].site; i = (i + 1) & (cap - 1)) {
            }
            n[i] = g_slots[j];
        }
        if (g_slots) VirtualFree(g_slots, 0, MEM_RELEASE);
        g_slots = n;
        g_cap = cap;
    }
    for (i = slot_hash(site, g_cap); g_slots[i].site && g_slots[i].site != site; i = (i + 1) & (g_cap - 1)) {
    }
    if (!g_slots[i].site) {
        g_slots[i].site = site;
        g_used++;
    }
    return &g_slots[i];
}

static int in_code(uint64_t site, unsigned len) {
    int i;
    for (i = 0; i < g_ncode; i++)
        if (site >= g_code[i].lo && site + len <= g_code[i].hi) return 1;
    return 0;
}

static void fill_pmov(uint8_t *c, int from, int to, int top) {
    int i, j;
    memset(c, 0x80, 16);
    for (i = 0; i < 16 / to; i++)
        for (j = 0; j < from; j++) c[i * to + (top ? to - from : 0) + j] = (uint8_t)(i * from + j);
}

static void fill_consts(uint8_t *k) {
    uint32_t *d = (uint32_t *)k;
    uint64_t *q = (uint64_t *)k;
    int i;
    for (i = 0; i < 4; i++) {
        d[C_SIGN_PS * 4 + i] = 0x80000000u;
        d[C_ABS_PS * 4 + i] = 0x7FFFFFFFu;
        d[C_MAGIC_PS * 4 + i] = 0x4B000000u;
        d[C_ONE_PS * 4 + i] = 0x3F800000u;
        d[C_D8000 * 4 + i] = 0x8000u;
        d[C_W8000 * 4 + i] = 0x80008000u;
    }
    for (i = 0; i < 2; i++) {
        q[C_SIGN_PD * 2 + i] = 0x8000000000000000ull;
        q[C_ABS_PD * 2 + i] = 0x7FFFFFFFFFFFFFFFull;
        q[C_MAGIC_PD * 2 + i] = 0x4330000000000000ull;
        q[C_ONE_PD * 2 + i] = 0x3FF0000000000000ull;
        q[C_POP55 * 2 + i] = 0x5555555555555555ull;
        q[C_POP33 * 2 + i] = 0x3333333333333333ull;
        q[C_POP0F * 2 + i] = 0x0F0F0F0F0F0F0F0Full;
        q[C_POP01 * 2 + i] = 0x0101010101010101ull;
    }
    q[C_ONEQ * 2] = 1;
    q[C_ONEQ * 2 + 1] = 0;
    memset(k + 16 * C_B7F, 0x7F, 16);
    memset(k + 16 * C_B80, 0x80, 16);
    memset(k + 16 * C_ONES, 0xFF, 16);
    for (i = 0; i < 2; i++) {
        q[C_Q80 * 2 + i] = 0x80;
        q[C_Q8000 * 2 + i] = 0x8000;
        q[C_Q80000000 * 2 + i] = 0x80000000ull;
    }
    for (i = 0; i < 16; i++) k[16 * C_SWAPB + i] = (uint8_t)(i ^ 1);
    fill_pmov(k + 16 * C_ZXBW, 1, 2, 0);
    fill_pmov(k + 16 * C_ZXBD, 1, 4, 0);
    fill_pmov(k + 16 * C_ZXBQ, 1, 8, 0);
    fill_pmov(k + 16 * C_ZXWD, 2, 4, 0);
    fill_pmov(k + 16 * C_ZXWQ, 2, 8, 0);
    fill_pmov(k + 16 * C_ZXDQ, 4, 8, 0);
    fill_pmov(k + 16 * C_SXBD, 1, 4, 1);
}

static uint8_t *alloc_near(uint64_t site) {
    MEMORY_BASIC_INFORMATION m;
    uint64_t q, lo = site > REACH ? site - REACH : 0x10000, hi = site + REACH;
    for (q = (site - ARENA_SIZE) & ~0xFFFFull; q > lo;) {
        if (!VirtualQuery((void *)(uintptr_t)q, &m, sizeof m)) break;
        if (m.State != MEM_FREE) {
            q = ((uint64_t)(uintptr_t)m.AllocationBase - ARENA_SIZE) & ~0xFFFFull;
            continue;
        }
        if ((uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize >= q + ARENA_SIZE) {
            void *p = VirtualAlloc((void *)(uintptr_t)q, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return p;
        }
        q -= 0x10000;
    }
    for (q = (site + 0xFFFF) & ~0xFFFFull; q + ARENA_SIZE < hi;) {
        uint64_t start, end;
        if (!VirtualQuery((void *)(uintptr_t)q, &m, sizeof m)) break;
        start = ((uint64_t)(uintptr_t)m.BaseAddress + 0xFFFF) & ~0xFFFFull;
        end = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
        if (m.State == MEM_FREE && start + ARENA_SIZE <= end) {
            void *p = VirtualAlloc((void *)(uintptr_t)start, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return p;
        }
        q = end;
    }
    return NULL;
}

static arena *arena_for(uint64_t site) {
    int i;
    arena *a;
    DWORD old;
    for (i = 0; i < g_narenas; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_arenas[i].base, e = b + ARENA_SIZE;
        uint64_t d1 = b > site ? b - site : site - b, d2 = e > site ? e - site : site - e;
        if (g_arenas[i].used + STUB_MAX <= ARENA_SIZE && d1 <= REACH && d2 <= REACH) return &g_arenas[i];
    }
    if (g_narenas == (int)(sizeof g_arenas / sizeof g_arenas[0])) return NULL;
    a = &g_arenas[g_narenas];
    a->base = alloc_near(site);
    if (!a->base) return NULL;
    fill_consts(a->base);
    if (!VirtualProtect(a->base, ARENA_SIZE, g_bulk ? PAGE_EXECUTE_READWRITE : ARENA_PROTECT, &old)) {
        VirtualFree(a->base, 0, MEM_RELEASE);
        a->base = NULL;
        return NULL;
    }
    a->used = 16 * C_COUNT;
    a->recs = NULL;
    a->nrec = a->caprec = 0;
    g_narenas++;
    return a;
}

static void stub_begin(tpl *t, uint8_t *at, const insn *in, const uint8_t *k, const uint8_t *moved, int nmoved,
                       uint64_t resume, unsigned xfree, unsigned gfree, int align) {
    memset(t, 0, sizeof *t);
    t->align = align;
    t->p = at;
    t->in = in;
    t->k = k;
    t->xfree = xfree;
    t->gfree = gfree;
    t->moved = moved;
    t->nmoved = nmoved;
    t->resume = resume;
    busy(t);
#ifdef BLOX7_ISA_COUNT
    t->counter = consts(t, 1);
    memset(t->counter, 0, 16);
#endif
}

static uint64_t make_stub(uint64_t site, const insn *in, const uint8_t *moved, int nmoved, uint64_t resume,
                          unsigned xfree, unsigned gfree, int align) {
    tplfn fn = template_for(in->kind);
    arena *a;
    stubrec *rec;
    tpl t;
    DWORD old;
    uint8_t *at;
    int ok, i, generic = 0, toggle;
    if (in->a32) return 0;
    a = arena_for(site);
    if (!a) return 0;
    if (a->nrec == a->caprec) {
        size_t cap = a->caprec ? a->caprec * 2 : 1024;
        stubrec *n = a->recs ? HeapReAlloc(GetProcessHeap(), 0, a->recs, cap * sizeof *n)
                             : HeapAlloc(GetProcessHeap(), 0, cap * sizeof *n);
        if (!n) return 0;
        a->recs = n;
        a->caprec = cap;
    }
    at = a->base + a->used;
    toggle = !g_bulk && ARENA_PROTECT != PAGE_EXECUTE_READWRITE;
    if (toggle && !VirtualProtect(at, STUB_MAX, PAGE_EXECUTE_READWRITE, &old)) return 0;
    stub_begin(&t, at, in, a->base, moved, nmoved, resume, xfree, gfree, align);
    ok = fn && fn(&t) && !t.bad && t.p <= at + STUB_MAX;
    if (!ok && !nmoved) {
        stub_begin(&t, at, in, a->base, NULL, 0, 0, 0, 0, -1);
        ok = t_generic(&t) && !t.bad && t.p <= at + STUB_MAX;
        generic = 1;
    }
    if (toggle) VirtualProtect(at, STUB_MAX, PAGE_EXECUTE_READ, &old);
    if (!ok) return 0;
    rec = &a->recs[a->nrec++];
    memset(rec, 0, sizeof *rec);
    rec->start = (uint64_t)(uintptr_t)t.entry;
    rec->body = (uint64_t)(uintptr_t)t.body;
    rec->end = (uint64_t)(uintptr_t)t.p;
    rec->site = site;
    rec->delta = t.delta;
    rec->nx = (uint8_t)t.nx;
    rec->ng = (uint8_t)t.ng;
    rec->generic = (uint8_t)generic;
    rec->refault = (uint64_t)(uintptr_t)t.refault;
    rec->tail = generic ? rec->end : (uint64_t)(uintptr_t)t.tail;
    rec->rest = generic ? rec->end : (uint64_t)(uintptr_t)t.rest;
    rec->desc = (const insn *)t.desc;
    for (i = 0; i < t.nx; i++) rec->xs[i] = (uint8_t)t.xs[i];
    for (i = 0; i < t.ng; i++) rec->gs[i] = (uint8_t)t.gs[i];
#ifdef BLOX7_ISA_COUNT
    rec->counter = generic ? (uint64_t *)(t.desc + 40) : (uint64_t *)t.counter;
#endif
#ifdef BLOX7_ISA_TEST
    rec->kind = in->kind;
#endif
    a->used = ((size_t)(t.p - a->base) + 15) & ~(size_t)15;
    if (!g_bulk) FlushInstructionCache(GetCurrentProcess(), at, (size_t)(t.p - at));
    return (uint64_t)(uintptr_t)t.entry;
}

static int frame_run(uint64_t *g, v128 *x, const insn *in, const uint32_t *mxcsr) {
    regs r;
    memfault f;
    r.g = g;
    r.x = x;
    r.flags = (DWORD *)&g[16];
    r.mxcsr = *mxcsr;
    memset(&f, 0, sizeof f);
#ifdef BLOX7_ISA_COUNT
    (*(uint64_t *)((uintptr_t)in + 40))++;
#endif
    return run(in, &r, &f) != 0;
}

static int write_jump(uint64_t site, uint64_t entry) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)site;
    void *page1 = (void *)(uintptr_t)(site & ~0xFFFull), *page2 = (void *)(uintptr_t)((site + 4) & ~0xFFFull);
    int32_t rel = (int32_t)(entry - (site + 5));
    DWORD old1, old2 = 0, tmp;
    int i;
    if (!VirtualProtect(page1, 1, PAGE_EXECUTE_READWRITE, &old1)) return 0;
    if (page2 != page1 && !VirtualProtect(page2, 1, PAGE_EXECUTE_READWRITE, &old2)) {
        VirtualProtect(page1, 1, old1, &tmp);
        return 0;
    }
    p[0] = 0xCC;
    FlushProcessWriteBuffers();
    for (i = 0; i < 4; i++) p[1 + i] = (uint8_t)((uint32_t)rel >> 8 * i);
    FlushProcessWriteBuffers();
    p[0] = 0xE9;
    FlushProcessWriteBuffers();
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)site, 5);
    if (page2 != page1) VirtualProtect(page2, 1, old2, &tmp);
    VirtualProtect(page1, 1, old1, &tmp);
    return 1;
}

static int patch(uint64_t site, const insn *in, int align) {
    slot *s;
    uint64_t entry = in->len >= 5 && in_code(site, in->len) ? make_stub(site, in, NULL, 0, 0, 0, 0, align) : 0;
    s = slot_add(site);
    if (!s) return 0;
    if (!entry) {
        s->state = S_TRAP;
        return 0;
    }
    s->state = write_jump(site, entry) ? S_PATCHED : S_TRAP;
    return s->state == S_PATCHED;
}

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elfphdr;

static const uint8_t *eh_table(uintptr_t base, uint32_t *count) {
    const uint8_t *e = (const uint8_t *)base;
    uint64_t phoff;
    uint16_t phnum, i;
    if (memcmp(e, "\177ELF\2\1", 6) != 0) return NULL;
    memcpy(&phoff, e + 0x20, 8);
    memcpy(&phnum, e + 0x38, 2);
    for (i = 0; i < phnum; i++) {
        const elfphdr *ph = (const elfphdr *)(e + phoff) + i;
        if (ph->type == 0x6474E550u) {
            const uint8_t *h = e + ph->vaddr;
            if (h[0] != 1 || h[2] != 0x03 || h[3] != 0x3B) return NULL;
            memcpy(count, h + 8, 4);
            return h;
        }
    }
    return NULL;
}

static int fde_range(const uint8_t *h, uint32_t i, uint64_t *start, uint64_t *end) {
    int32_t loc, off, begin;
    uint32_t length, range;
    const uint8_t *f;
    memcpy(&loc, h + 12 + 8 * (size_t)i, 4);
    memcpy(&off, h + 16 + 8 * (size_t)i, 4);
    f = h + off;
    memcpy(&length, f, 4);
    if (length == 0xFFFFFFFFu || length < 12) return 0;
    memcpy(&begin, f + 8, 4);
    memcpy(&range, f + 12, 4);
    if (f + 8 + begin != h + loc) return 0;
    *start = (uint64_t)(uintptr_t)(h + loc);
    *end = *start + range;
    return 1;
}

typedef struct {
    uint64_t lo, hi;
} deadrun;

static int is_padding(const uint8_t *p, int len) {
    int i = 0;
    while (i < len && (p[i] == 0x66 || p[i] == 0x2E || p[i] == 0x3E)) i++;
    if (i + 1 == len && (p[i] == 0x90 || p[i] == 0xCC)) return 1;
    return i + 1 < len && p[i] == 0x0F && p[i + 1] == 0x1F;
}

static int is_terminal(const uint8_t *p, int len) {
    int i = 0;
    while (i < len && (p[i] == 0x66 || p[i] == 0xF2 || p[i] == 0xF3 || p[i] == 0x2E || p[i] == 0x3E)) i++;
    if (i < len && (p[i] & 0xF0) == 0x40) i++;
    if (i >= len) return 0;
    if (p[i] == 0xC3 || p[i] == 0xC2 || p[i] == 0xE9 || p[i] == 0xEB || p[i] == 0xCC) return 1;
    if (p[i] == 0x0F) return i + 1 < len && p[i + 1] == 0x0B;
    return p[i] == 0xFF && i + 1 < len && ((p[i + 1] >> 3 & 7) == 4 || (p[i + 1] >> 3 & 7) == 5);
}

static int all_padding(uint64_t lo, uint64_t hi) {
    while (lo < hi) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)lo;
        int len = insn_len(p);
        if (!len || !is_padding(p, len)) return 0;
        lo += (uint64_t)len;
    }
    return lo == hi;
}

static uint64_t take_hop(uint64_t site, deadrun *runs, int nruns) {
    int i;
    for (i = 0; i < nruns; i++) {
        uint64_t g = runs[i].lo > site + 2 - 128 ? runs[i].lo : site + 2 - 128;
        if (g + 5 <= runs[i].hi && g <= site + 2 + 127) {
            runs[i].lo = g + 5;
            return g;
        }
    }
    return 0;
}

typedef struct {
    uint64_t start, end;
    uint8_t *targets;
    int indirect;
    unsigned xused, gused;
    deadrun runs[32];
    int nruns;
} fninfo;

static int branch_target(const uint8_t *p, int len, uint64_t at, uint64_t *target) {
    int i = 0;
    int32_t rel;
    while (i < len && (p[i] == 0x66 || p[i] == 0xF2 || p[i] == 0xF3 || p[i] == 0x2E || p[i] == 0x3E)) i++;
    if (i < len && (p[i] & 0xF0) == 0x40) i++;
    if (i >= len) return 0;
    if ((p[i] >= 0x70 && p[i] <= 0x7F) || p[i] == 0xEB || (p[i] >= 0xE0 && p[i] <= 0xE3)) {
        *target = at + (uint64_t)len + (uint64_t)(int64_t)(int8_t)p[len - 1];
        return 1;
    }
    if (p[i] == 0xE8 || p[i] == 0xE9 || (p[i] == 0x0F && i + 1 < len && p[i + 1] >= 0x80 && p[i + 1] <= 0x8F)) {
        memcpy(&rel, p + len - 4, 4);
        *target = at + (uint64_t)len + (uint64_t)(int64_t)rel;
        return 1;
    }
    return 0;
}

static int movable(const uint8_t *p, int len) {
    int i = 0, op;
    while (i < len && (p[i] == 0x66 || p[i] == 0x67 || p[i] == 0xF2 || p[i] == 0xF3 || p[i] == 0x2E || p[i] == 0x3E ||
                       p[i] == 0x26 || p[i] == 0x36 || p[i] == 0x64 || p[i] == 0x65 || p[i] == 0xF0))
        i++;
    if (i < len && (p[i] & 0xF0) == 0x40) i++;
    if (i >= len) return 0;
    op = p[i];
    if (op == 0xC4 || op == 0xC5 || op == 0x62 || (op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) ||
        op == 0xE8 || op == 0xE9 || op == 0xEB || op == 0xC2 || op == 0xC3 || op == 0xCA || op == 0xCB || op == 0xCC ||
        op == 0xCD || op == 0xCF || op == 0xFF || op == 0x9C || op == 0x9D || (op >= 0x50 && op <= 0x5F) ||
        op == 0x8F || (op >= 0xA0 && op <= 0xA3))
        return 0;
    if (op == 0x0F) {
        int op2 = i + 1 < len ? p[i + 1] : -1;
        if ((op2 >= 0x80 && op2 <= 0x8F) || op2 == 0x05 || op2 == 0x0B || op2 == 0x38 || op2 == 0x3A || op2 == 0xB8)
            return 0;
        if (k_map2[op2 & 0xFF] & L_M) return i + 2 < len && !((p[i + 2] & 0xC7) == 0x05);
        return 1;
    }
    if (k_map1[op] & L_M) return i + 1 < len && !((p[i + 1] & 0xC7) == 0x05);
    return 1;
}

enum { A_UNK = -1 };

typedef struct {
    int sp, bp;
} stk;

static int a_add(int a, int64_t v) { return a < 0 || (v & 7) ? A_UNK : (int)((a + v) & 8); }

static uint8_t stk_code(stk s) { return (uint8_t)(0x80 | (s.sp < 0 ? 2 : s.sp >> 3) | (s.bp < 0 ? 2 : s.bp >> 3) << 2); }

static stk stk_decode(uint8_t c) {
    stk s;
    s.sp = (c & 3) == 2 ? A_UNK : (c & 3) << 3;
    s.bp = (c >> 2 & 3) == 2 ? A_UNK : (c >> 2 & 3) << 3;
    return s;
}

static stk stk_merge(stk a, stk b) {
    if (a.sp != b.sp) a.sp = A_UNK;
    if (a.bp != b.bp) a.bp = A_UNK;
    return a;
}

static void stk_lea(const uint8_t *p, int len, int at, int rex, int mrm, stk *s, int *a) {
    int mod = mrm >> 6, base, idx = -1;
    int64_t disp = 0;
    if ((mrm & 7) == 4) {
        int sib = at < len ? p[at] : 0;
        at++;
        idx = (sib >> 3 & 7) | (rex & 2) << 2;
        base = (sib & 7) | (rex & 1) << 3;
        if (idx == 4) idx = -1;
        if ((sib & 7) == 5 && mod == 0) base = -1;
    } else if (mod == 0 && (mrm & 7) == 5) {
        base = -1;
    } else {
        base = (mrm & 7) | (rex & 1) << 3;
    }
    if (mod == 1 && at < len) {
        disp = (int8_t)p[at];
    } else if ((mod == 2 || base < 0) && at + 4 <= len) {
        int32_t d32;
        memcpy(&d32, p + at, 4);
        disp = d32;
    }
    *a = !(rex & 8) || idx >= 0 ? A_UNK : base == 4 ? a_add(s->sp, disp) : base == 5 ? a_add(s->bp, disp) : A_UNK;
}

static void stk_step(const uint8_t *p, int len, stk *s, int *terminal) {
    int i = 0, rex = 0, o16 = 0, f3 = 0, op, mrm, mod, reg, rm, dig, dst = -1, w;
    *terminal = 0;
    while (i < len && (p[i] == 0x66 || p[i] == 0x67 || p[i] == 0xF2 || p[i] == 0xF3 || p[i] == 0x2E || p[i] == 0x3E ||
                       p[i] == 0x26 || p[i] == 0x36 || p[i] == 0x64 || p[i] == 0x65 || p[i] == 0xF0)) {
        if (p[i] == 0x66) o16 = 1;
        if (p[i] == 0xF3) f3 = 1;
        i++;
    }
    if (i < len && (p[i] & 0xF0) == 0x40) rex = p[i++];
    if (i >= len) return;
    op = p[i];
    w = rex >> 3 & 1;
    if (op >= 0x50 && op <= 0x5F) {
        int r = (op & 7) | (rex & 1) << 3;
        if (o16 || (op >= 0x58 && r == 4)) s->sp = A_UNK;
        else s->sp = a_add(s->sp, op < 0x58 ? -8 : 8);
        if (op >= 0x58 && r == 5) s->bp = A_UNK;
        return;
    }
    if (op == 0x68 || op == 0x6A || op == 0x9C || op == 0x9D) {
        s->sp = o16 ? A_UNK : a_add(s->sp, op == 0x9D ? 8 : -8);
        return;
    }
    if (op == 0xC9) {
        s->sp = a_add(s->bp, 8);
        s->bp = A_UNK;
        return;
    }
    if (op == 0xC8) {
        s->sp = s->bp = A_UNK;
        return;
    }
    if (op == 0xC3 || op == 0xC2 || op == 0xE9 || op == 0xEB || op == 0xCC || op == 0xCF) {
        *terminal = 1;
        return;
    }
    if ((op >= 0xB8 && op <= 0xBF) || (op >= 0x91 && op <= 0x97)) {
        dst = (op & 7) | (rex & 1) << 3;
    } else if (op == 0x0F) {
        int op2 = i + 1 < len ? p[i + 1] : -1;
        if (op2 == 0x0B) {
            *terminal = 1;
            return;
        }
        if (op2 >= 0xC8 && op2 <= 0xCF) {
            dst = (op2 & 7) | (rex & 1) << 3;
        } else if ((op2 == 0x38 || op2 == 0x3A) && i + 3 < len) {
            int op3 = p[i + 2];
            mrm = p[i + 3];
            if (op2 == 0x38 && (op3 == 0xF0 || op3 == 0xF1)) dst = (mrm >> 3 & 7) | (rex & 4) << 1;
            else if (op2 == 0x3A && op3 >= 0x14 && op3 <= 0x17 && mrm >> 6 == 3) dst = (mrm & 7) | (rex & 1) << 3;
        } else if (op2 >= 0 && i + 2 < len && (k_map2[op2] & L_M)) {
            mrm = p[i + 2];
            mod = mrm >> 6;
            reg = (mrm >> 3 & 7) | (rex & 4) << 1;
            rm = (mrm & 7) | (rex & 1) << 3;
            if ((op2 >= 0x40 && op2 <= 0x4F) || op2 == 0xAF || (op2 >= 0xB6 && op2 <= 0xB8) ||
                (op2 >= 0xBC && op2 <= 0xBF) || op2 == 0xC5 || op2 == 0xD7 || op2 == 0x50 || op2 == 0x2C || op2 == 0x2D) {
                dst = reg;
            } else if (mod == 3 && ((op2 == 0x7E && !f3) || (op2 >= 0x90 && op2 <= 0x9F) || op2 == 0xA4 || op2 == 0xA5 ||
                                    op2 == 0xAC || op2 == 0xAD || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB ||
                                    op2 == 0xBA || op2 == 0xB0 || op2 == 0xB1)) {
                dst = rm;
            } else if (op2 == 0xC0 || op2 == 0xC1) {
                if (reg == 4 || (mod == 3 && rm == 4)) s->sp = A_UNK;
                if (reg == 5 || (mod == 3 && rm == 5)) s->bp = A_UNK;
            }
        }
    } else if ((k_map1[op] & L_M) && i + 1 < len) {
        mrm = p[i + 1];
        mod = mrm >> 6;
        dig = mrm >> 3 & 7;
        reg = dig | (rex & 4) << 1;
        rm = (mrm & 7) | (rex & 1) << 3;
        if ((op == 0x81 || op == 0x83) && mod == 3 && (rm == 4 || rm == 5)) {
            int *a = rm == 4 ? &s->sp : &s->bp;
            int64_t v;
            if (op == 0x83) {
                v = (int8_t)p[len - 1];
            } else {
                int32_t v32;
                memcpy(&v32, p + len - 4, 4);
                v = v32;
            }
            if (dig == 7) return;
            if (!w) *a = A_UNK;
            else if (dig == 0) *a = a_add(*a, v);
            else if (dig == 5) *a = a_add(*a, -v);
            else if (dig == 4) *a = v & 15 ? A_UNK : 0;
            else *a = A_UNK;
            return;
        }
        if (op == 0x8D && (reg == 4 || reg == 5)) {
            stk_lea(p, len, i + 2, rex, mrm, s, reg == 4 ? &s->sp : &s->bp);
            if (o16) {
                if (reg == 4) s->sp = A_UNK;
                else s->bp = A_UNK;
            }
            return;
        }
        if ((op == 0x89 || op == 0x8B) && mod == 3) {
            int d = op == 0x89 ? rm : reg, src = op == 0x89 ? reg : rm;
            if (d == 4 || d == 5) {
                int v = !w ? A_UNK : src == 4 ? s->sp : src == 5 ? s->bp : A_UNK;
                if (d == 4) s->sp = v;
                else s->bp = v;
            }
            return;
        }
        if (op < 0x40 && (op & 7) < 4) {
            if ((op & 0x38) != 0x38) dst = op & 2 ? reg : (mod == 3 ? rm : -1);
        } else if (op >= 0x80 && op <= 0x83) {
            if (dig != 7 && mod == 3) dst = rm;
        } else if (op == 0x86 || op == 0x87) {
            if (reg == 4) s->sp = A_UNK;
            if (reg == 5) s->bp = A_UNK;
            dst = mod == 3 ? rm : -1;
        } else if (op == 0x88 || op == 0x89 || op == 0xC6 || op == 0xC7 || op == 0xC0 || op == 0xC1 ||
                   (op >= 0xD0 && op <= 0xD3)) {
            dst = mod == 3 ? rm : -1;
        } else if (op == 0x8A || op == 0x8B || op == 0x8D || op == 0x63 || op == 0x69 || op == 0x6B) {
            dst = reg;
        } else if (op == 0xF6 || op == 0xF7) {
            if ((dig == 2 || dig == 3) && mod == 3) dst = rm;
        } else if (op == 0xFE || op == 0xFF) {
            if (dig <= 1 && mod == 3) dst = rm;
            else if (dig == 6) s->sp = a_add(s->sp, -8);
            else if (dig == 4 || dig == 5) *terminal = 1;
        } else if (op == 0x8F) {
            s->sp = a_add(s->sp, 8);
            if (mod == 3) dst = rm;
        }
    }
    if (dst == 4) s->sp = A_UNK;
    if (dst == 5) s->bp = A_UNK;
}

typedef struct {
    uint8_t *tg;
    uint64_t start, end;
    stk st, body, sw;
    int dead, framed;
} stktrack;

static void trk_init(stktrack *k, uint64_t start, uint64_t end) {
    memset(k, 0, sizeof *k);
    k->start = start;
    k->end = end;
    k->tg = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)(end - start));
    k->st.sp = 8;
    k->st.bp = k->body.sp = k->body.bp = k->sw.sp = k->sw.bp = A_UNK;
}

static void trk_free(stktrack *k) {
    if (k->tg) HeapFree(GetProcessHeap(), 0, k->tg);
}

static int trk_enter(stktrack *k, uint64_t at) {
    uint8_t c = k->tg ? k->tg[at - k->start] : 0;
    if (c) {
        k->st = k->dead ? stk_decode(c) : stk_merge(k->st, stk_decode(c));
    } else if (k->dead) {
        k->st = k->sw.sp >= 0 ? k->sw : k->body;
    }
    return k->tg ? k->st.sp : A_UNK;
}

static void trk_leave(stktrack *k, const uint8_t *p, int len, uint64_t at) {
    uint64_t target;
    int i = 0;
    if (k->tg && p[0] != 0xE8 && branch_target(p, len, at, &target) && target > at && target < k->end) {
        uint8_t *c = &k->tg[target - k->start];
        *c = *c ? stk_code(stk_merge(stk_decode(*c), k->st)) : stk_code(k->st);
    }
    while (i < len && (p[i] == 0x66 || p[i] == 0xF2 || p[i] == 0xF3 || p[i] == 0x2E || p[i] == 0x3E)) i++;
    if (i < len && (p[i] & 0xF0) == 0x40) i++;
    if (i + 1 < len && p[i] == 0xFF && (p[i + 1] >> 3 & 7) == 4) k->sw = k->st;
    stk_step(p, len, &k->st, &k->dead);
    if (!k->framed && i + 1 < len && (p[i] == 0x81 || p[i] == 0x83) && p[i + 1] == 0xEC) {
        k->framed = 1;
        k->body = k->st;
    }
}

static int patch_now(uint64_t site, const insn *in, fninfo *fi, int align) {
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    uint64_t entry, hop = 0, next = site + in->len;
    slot *s;
    int32_t rel;
    int nlen = 0;
    if (in->len < 5 && !(hop = take_hop(site, fi->runs, fi->nruns))) {
        insn other;
        if (fi->indirect || !fi->targets || next >= fi->end) return 0;
        nlen = insn_len((const uint8_t *)(uintptr_t)next);
        if (!nlen || next + (uint64_t)nlen > fi->end || in->len + nlen < 5) return 0;
        if (fi->targets[(next - fi->start) >> 3] >> ((next - fi->start) & 7) & 1) return 0;
        if (!movable((const uint8_t *)(uintptr_t)next, nlen)) return 0;
        if (decode((const uint8_t *)(uintptr_t)next, next, &other)) return 0;
    }
    entry = make_stub(site, in, nlen ? (const uint8_t *)(uintptr_t)next : NULL, nlen, next + (uint64_t)nlen,
                      ~fi->xused & 0xFF00u, ~fi->gused & (1u << 10 | 1u << 11), align);
    s = entry ? slot_add(site) : NULL;
    if (!s) return 0;
    if (hop) {
        uint8_t *q = (uint8_t *)(uintptr_t)hop;
        rel = (int32_t)(entry - (hop + 5));
        q[0] = 0xE9;
        memcpy(q + 1, &rel, 4);
        p[0] = 0xEB;
        p[1] = (uint8_t)(int8_t)(hop - (site + 2));
    } else {
        rel = (int32_t)(entry - (site + 5));
        p[0] = 0xE9;
        memcpy(p + 1, &rel, 4);
    }
    s->state = S_PATCHED;
    return nlen;
}

static int has_escape(const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i + 1 < n; i++)
        if (p[i] == 0x0F && (p[i + 1] == 0x38 || p[i + 1] == 0x3A || p[i + 1] == 0xB8)) return 1;
    return 0;
}

static void scan_function(uint64_t start, uint64_t end, uint64_t next) {
    fninfo fi;
    int terminal = 0, shorts = 0;
    uint64_t at, run = 0, target;
    if (!has_escape((const uint8_t *)(uintptr_t)start, (size_t)(end - start))) return;
    memset(&fi, 0, sizeof fi);
    fi.start = start;
    fi.end = end;
    for (at = start; at < end;) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)at;
        int len = insn_len(p);
        if (!len || at + (uint64_t)len > end) return;
        if (len == 4 && p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0xB8) shorts = 1;
        refs(p, len, &fi.xused, &fi.gused);
        if (is_padding(p, len) && (run || terminal)) {
            if (!run) run = at;
        } else {
            if (run && at - run >= 5 && fi.nruns < 32) fi.runs[fi.nruns++] = (deadrun){run, at};
            run = 0;
        }
        terminal = is_terminal(p, len);
        at += (uint64_t)len;
    }
    if (run && end - run >= 5 && fi.nruns < 32) fi.runs[fi.nruns++] = (deadrun){run, end};
    if (next > end && next - end >= 5 && next - end <= 64 && fi.nruns < 32 && all_padding(end, next))
        fi.runs[fi.nruns++] = (deadrun){end, next};
    if (shorts) {
        fi.targets = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)((end - start) / 8 + 1));
        for (at = start; fi.targets && at < end;) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)at;
            int len = insn_len(p), i = 0;
            if (branch_target(p, len, at, &target) && target >= start && target < end)
                fi.targets[(target - start) >> 3] |= (uint8_t)(1u << ((target - start) & 7));
            while (i < len && (p[i] == 0x66 || p[i] == 0xF2 || p[i] == 0xF3 || p[i] == 0x2E || p[i] == 0x3E)) i++;
            if (i < len && (p[i] & 0xF0) == 0x40) i++;
            if (i + 1 < len && p[i] == 0xFF && ((p[i + 1] >> 3 & 7) == 4 || (p[i + 1] >> 3 & 7) == 5)) fi.indirect = 1;
            at += (uint64_t)len;
        }
    }
    {
        stktrack k;
        trk_init(&k, start, end);
        for (at = start; at < end;) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)at;
            int len = insn_len(p), moved = 0, site, nlen = 0, unused, align = trk_enter(&k, at);
            stk after;
            insn in;
            site = has_escape(p, (size_t)(len < 7 ? len : 7)) && decode(p, at, &in) && in.len == len &&
                   (group_of(in.kind) & g_missing);
            trk_leave(&k, p, len, at);
            after = k.st;
            if (site && len < 5 && at + (uint64_t)len < end && (nlen = insn_len(p + len)) > 0)
                stk_step(p + len, nlen, &after, &unused);
            if (site) moved = patch_now(at, &in, &fi, align);
            if (moved) {
                k.st = after;
                len += moved;
            }
            at += (uint64_t)len;
        }
        trk_free(&k);
    }
    if (fi.targets) HeapFree(GetProcessHeap(), 0, fi.targets);
}

static void scan_module(uintptr_t base, uintptr_t lo, uintptr_t hi) {
    uint32_t n = 0, i;
    const uint8_t *h = eh_table(base, &n);
    int a;
    DWORD old;
    uint64_t s, e, ns = 0, ne;
    if (!h) return;
    g_bulk = 1;
    for (a = 0; a < g_narenas; a++) VirtualProtect(g_arenas[a].base, ARENA_SIZE, PAGE_EXECUTE_READWRITE, &old);
    for (i = 0; i < n; i++) {
        if (!fde_range(h, i, &s, &e) || s < lo || e > hi || e <= s) continue;
        if (i + 1 >= n || !fde_range(h, i + 1, &ns, &ne)) ns = 0;
        scan_function(s, e, ns);
    }
    for (a = 0; a < g_narenas; a++) {
        VirtualProtect(g_arenas[a].base, ARENA_SIZE, ARENA_PROTECT, &old);
        FlushInstructionCache(GetCurrentProcess(), g_arenas[a].base, ARENA_SIZE);
    }
    g_bulk = 0;
}

static void fetch(uint64_t site, uint8_t *out) {
    memfault f;
    size_t n = 0x1000 - (size_t)(site & 0xFFF), i;
    memset(out, 0, 16);
#ifdef BLOX7_ISA_TEST
    if (poisoned(site, out)) return;
#endif
    if (n > 15) n = 15;
    memcpy(out, (const void *)(uintptr_t)site, n);
    for (i = n; i < 15; i++)
        if (isa_mem(out + i, (const void *)(uintptr_t)(site + i), 1, &f)) break;
}

static const stubrec *stub_at(uint64_t rip);

static LONG emulate(EXCEPTION_RECORD *rec, CONTEXT *c, const insn *in) {
    regs r;
    memfault f;
    r.g = (uint64_t *)&c->Rax;
    r.x = (v128 *)c->FltSave.XmmRegisters;
    r.flags = &c->EFlags;
    r.mxcsr = c->MxCsr;
    memset(&f, 0, sizeof f);
    switch (run(in, &r, &f)) {
    case 0:
        c->Rip = in->next;
        return EXCEPTION_CONTINUE_EXECUTION;
    case 1:
        rec->ExceptionCode = f.code;
        rec->ExceptionAddress = (void *)(uintptr_t)c->Rip;
        rec->NumberParameters = 2;
        rec->ExceptionInformation[0] = f.write;
        rec->ExceptionInformation[1] = f.addr;
        return EXCEPTION_CONTINUE_SEARCH;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

static LONG on_ud(EXCEPTION_RECORD *rec, CONTEXT *c) {
    uint64_t site = c->Rip;
    uint8_t code[16];
    insn in;
    const stubrec *st;
    slot *s;
    int known = 0;
    AcquireSRWLockShared(&g_lock);
    st = stub_at(site);
    if (st) {
        int refault = st->generic && st->refault == site;
        if (refault) {
            in = *st->desc;
            c->Rip = st->site;
        }
        ReleaseSRWLockShared(&g_lock);
        return refault ? emulate(rec, c, &in) : EXCEPTION_CONTINUE_SEARCH;
    }
#ifdef BLOX7_ISA_TEST
    if (test_cpuid(c)) {
        ReleaseSRWLockShared(&g_lock);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif
    s = slot_find(site);
    if (s && s->state != S_TRAP) {
        ReleaseSRWLockShared(&g_lock);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    known = s != NULL;
    fetch(site, code);
    ReleaseSRWLockShared(&g_lock);
    if (!decode(code, site, &in)) return EXCEPTION_CONTINUE_SEARCH;
    if (!known && g_patching && TryAcquireSRWLockExclusive(&g_lock)) {
        int done;
        g_owner = GetCurrentThreadId();
        s = slot_find(site);
        done = s ? s->state == S_PATCHED : patch(site, &in, c->Rsp & 7 ? -1 : (int)(c->Rsp & 8));
        g_owner = 0;
        ReleaseSRWLockExclusive(&g_lock);
        if (done) return EXCEPTION_CONTINUE_EXECUTION;
    }
#ifdef BLOX7_ISA_TEST
    test_count(site, &in);
#endif
    return emulate(rec, c, &in);
}

static LONG on_int3(CONTEXT *c) {
    uint64_t at = c->Rip;
    int i;
    for (i = 0; i < 2; i++) {
        slot *s;
        uint32_t state = 0;
        AcquireSRWLockShared(&g_lock);
        s = slot_find(at - (uint64_t)i);
        if (s) state = s->state;
        ReleaseSRWLockShared(&g_lock);
        if (state == S_PATCHED || state == S_PENDING) {
            c->Rip = at - (uint64_t)i;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static const stubrec *stub_at(uint64_t rip) {
    int i;
    for (i = 0; i < g_narenas; i++) {
        const arena *a = &g_arenas[i];
        size_t lo = 0, hi = a->nrec;
        if (rip < (uint64_t)(uintptr_t)a->base || rip >= (uint64_t)(uintptr_t)a->base + ARENA_SIZE) continue;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (a->recs[mid].end <= rip) lo = mid + 1;
            else hi = mid;
        }
        if (lo < a->nrec && rip >= a->recs[lo].start) return &a->recs[lo];
        return NULL;
    }
    return NULL;
}

static volatile LONG g_heals;

static int is_movdqa(const uint8_t *x) {
    int i = (x[1] & 0xF0) == 0x40 ? 2 : 1;
    return x[0] == 0x66 && x[i] == 0x0F && (x[i + 1] == 0x7F || x[i + 1] == 0x6F);
}

static int heal(const stubrec *s) {
    uint64_t r[2][2] = {{s->start, s->body}, {s->rest, s->tail}};
    uint8_t *b = (uint8_t *)(uintptr_t)s->start;
    size_t n = (size_t)(s->tail - s->start);
    DWORD old, tmp;
    int i;
    if (!VirtualProtect(b, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (i = 0; i < 2; i++) {
        uint64_t at = r[i][0];
        while (at < r[i][1]) {
            uint8_t *x = (uint8_t *)(uintptr_t)at;
            int len = insn_len(x);
            if (!len) break;
            if (is_movdqa(x)) x[0] = 0xF3;
            at += (uint64_t)len;
        }
    }
    VirtualProtect(b, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), b, n);
    return 1;
}

static LONG on_fault(EXCEPTION_RECORD *rec, CONTEXT *c) {
    uint64_t rip = c->Rip;
    stubrec s;
    int found = 0, i;
    AcquireSRWLockShared(&g_lock);
    {
        const stubrec *p = stub_at(rip);
        if (p) {
            s = *p;
            found = 1;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (!found || s.generic || rip >= s.tail) return EXCEPTION_CONTINUE_SEARCH;
    if (rec->ExceptionCode == STATUS_ACCESS_VIOLATION && ((rip >= s.start && rip < s.body) || rip >= s.rest) &&
        is_movdqa((const uint8_t *)(uintptr_t)rip)) {
        AcquireSRWLockExclusive(&g_lock);
        g_owner = GetCurrentThreadId();
        if (is_movdqa((const uint8_t *)(uintptr_t)rip)) heal(&s);
        g_owner = 0;
        ReleaseSRWLockExclusive(&g_lock);
        InterlockedIncrement(&g_heals);
        if (!is_movdqa((const uint8_t *)(uintptr_t)rip)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rip >= s.body) {
        for (i = 0; i < s.nx; i++)
            memcpy(&c->FltSave.XmmRegisters[s.xs[i]], (const void *)(uintptr_t)(c->Rsp + 16u * (unsigned)i), 16);
        for (i = 0; i < s.ng; i++)
            (&c->Rax)[s.gs[i]] = *(const uint64_t *)(uintptr_t)(c->Rsp + 16u * s.nx + 8u * (unsigned)i);
    }
    c->Rsp += (uint64_t)(int64_t)s.delta;
    c->Rip = s.site;
    rec->ExceptionAddress = (void *)(uintptr_t)s.site;
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *rec = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;
    DWORD code = rec->ExceptionCode;
    int memory = code == STATUS_ACCESS_VIOLATION || code == STATUS_GUARD_PAGE_VIOLATION || code == STATUS_IN_PAGE_ERROR;
    if (memory && c->Rip >= (uint64_t)(uintptr_t)isa_mem_begin && c->Rip < (uint64_t)(uintptr_t)isa_mem_end) {
        memfault *f = (memfault *)(uintptr_t)c->R9;
        f->addr = rec->NumberParameters >= 2 ? (uintptr_t)rec->ExceptionInformation[1] : 0;
        f->write = rec->NumberParameters >= 1 && rec->ExceptionInformation[0] == 1;
        f->code = code;
        c->Rax = 1;
        c->Rip = (uint64_t)(uintptr_t)isa_mem_fault;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_owner == GetCurrentThreadId()) return EXCEPTION_CONTINUE_SEARCH;
    if (code == STATUS_ILLEGAL_INSTRUCTION) return on_ud(rec, c);
    if (code == STATUS_BREAKPOINT) return on_int3(c);
    if (memory) return on_fault(rec, c);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void HOSTCALL front(void) {
    void *h = AddVectoredExceptionHandler(1, veh);
    if (!h) return;
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = h;
}

static void HOSTCALL code_range(uintptr_t base, uintptr_t lo, uintptr_t hi, const char *path) {
    DWORD old, tmp;
    AcquireSRWLockExclusive(&g_lock);
    g_owner = GetCurrentThreadId();
    if (g_ncode < (int)(sizeof g_code / sizeof g_code[0])) {
        g_code[g_ncode].base = base;
        g_code[g_ncode].lo = lo;
        g_code[g_ncode].hi = hi;
#ifdef BLOX7_ISA_TEST
        {
            const char *slash = strrchr(path, '/');
            strncpy(g_code[g_ncode].name, slash ? slash + 1 : path, sizeof g_code[g_ncode].name - 1);
        }
#endif
        g_ncode++;
    }
    if (g_patching && VirtualProtect((void *)lo, hi - lo, PAGE_EXECUTE_READWRITE, &old)) {
        scan_module(base, lo, hi);
        VirtualProtect((void *)lo, hi - lo, old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), (void *)lo, hi - lo);
    }
    g_owner = 0;
    ReleaseSRWLockExclusive(&g_lock);
#ifdef BLOX7_ISA_TEST
    poison(base, lo, hi, path);
#else
    (void)path;
#endif
}

#ifdef BLOX7_ISA_TEST
#include "../test/isa_test.h"
#endif

#ifndef BLOX7_ISA_TEST
static uint8_t *volatile g_probe_end;
static volatile int g_probe_trapped;

static LONG CALLBACK probe_veh(EXCEPTION_POINTERS *e) {
    uint64_t rip = e->ContextRecord->Rip, end = (uint64_t)(uintptr_t)g_probe_end;
    if (e->ExceptionRecord->ExceptionCode != STATUS_ILLEGAL_INSTRUCTION || !end || rip < end - 16 || rip >= end)
        return EXCEPTION_CONTINUE_SEARCH;
    g_probe_trapped = 1;
    e->ContextRecord->Rip = end;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int traps(uint8_t *page, const uint8_t *code, size_t n) {
    memcpy(page + 16 - n, code, n);
    page[16] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), page, 17);
    g_probe_trapped = 0;
    g_probe_end = page + 16;
    ((void (*)(void))(void *)(page + 16 - n))();
    g_probe_end = NULL;
    return g_probe_trapped;
}

static int really_missing(int groups) {
    static const uint8_t k_pshufb[] = {0x66, 0x0F, 0x38, 0x00, 0xC0}, k_pminsd[] = {0x66, 0x0F, 0x38, 0x39, 0xC0},
                         k_pcmpgtq[] = {0x66, 0x0F, 0x38, 0x37, 0xC0}, k_popcnt[] = {0xF3, 0x0F, 0xB8, 0xC0};
    uint8_t *page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    void *h = page ? AddVectoredExceptionHandler(1, probe_veh) : NULL;
    int out = groups;
    if (!h) {
        if (page) VirtualFree(page, 0, MEM_RELEASE);
        return groups;
    }
    if ((groups & G_SSSE3) && !traps(page, k_pshufb, sizeof k_pshufb)) out &= ~G_SSSE3;
    if ((groups & G_SSE41) && !traps(page, k_pminsd, sizeof k_pminsd)) out &= ~G_SSE41;
    if ((groups & G_SSE42) && !traps(page, k_pcmpgtq, sizeof k_pcmpgtq)) out &= ~G_SSE42;
    if ((groups & G_POPCNT) && !traps(page, k_popcnt, sizeof k_popcnt)) out &= ~G_POPCNT;
    RemoveVectoredExceptionHandler(h);
    VirtualFree(page, 0, MEM_RELEASE);
    return out;
}
#endif

void isa_start(struct w32boot *boot, uintptr_t libc_base, uintptr_t code_lo, uintptr_t code_hi) {
    unsigned a = 0, b, c = 0, d, fam, model;
    if (!__get_cpuid(1, &a, &b, &c, &d)) a = c = 0;
    g_missing = (c >> 9 & 1 ? 0 : G_SSSE3) | (c >> 19 & 1 ? 0 : G_SSE41) | (c >> 20 & 1 ? 0 : G_SSE42) |
                (c >> 23 & 1 ? 0 : G_POPCNT);
#ifdef BLOX7_ISA_TEST
    if (!test_start()) return;
#else
    if (g_missing) g_missing = really_missing(g_missing);
    if (!g_missing) return;
#endif
    fam = a >> 8 & 15;
    model = (a >> 4 & 15) | (fam == 6 || fam == 15 ? a >> 12 & 0xF0 : 0);
    g_pshufb = !(g_missing & G_SSSE3) && !(fam == 6 && (model == 0x0F || model == 0x16));
    g_lahf = __get_cpuid(0x80000001u, &a, &b, &c, &d) && (c & 1);
    __get_cpuid(0, &a, &b, &c, &d);
    g_slow_unaligned = fam == 15 || (b == 0x756E6547u && fam == 6 &&
                                     (model == 0x0F || model == 0x16 || model == 0x17 || model == 0x1D || model == 0x1C ||
                                      model == 0x26 || model == 0x27 || model == 0x35 || model == 0x36));
    g_veh = AddVectoredExceptionHandler(1, veh);
    if (!g_veh) return;
    boot->isa_front = front;
    boot->isa_code = code_range;
    code_range(libc_base, code_lo, code_hi, "libc.so");
}
