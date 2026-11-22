#ifndef BLOX7_ISA_NAMES_H_
#define BLOX7_ISA_NAMES_H_

static const char *const k_kind_names[K_COUNT] = {
    "?", "pshufb", "phaddw", "phaddd", "phaddsw", "pmaddubsw", "phsubw", "phsubd", "phsubsw", "psignb", "psignw",
    "psignd", "pmulhrsw", "pabsb", "pabsw", "pabsd", "palignr", "pblendvb", "blendvps", "blendvpd", "ptest",
    "pmovsxbw", "pmovsxbd", "pmovsxbq", "pmovsxwd", "pmovsxwq", "pmovsxdq", "pmuldq", "pcmpeqq", "movntdqa",
    "packusdw", "pmovzxbw", "pmovzxbd", "pmovzxbq", "pmovzxwd", "pmovzxwq", "pmovzxdq", "pcmpgtq", "pminsb",
    "pminsd", "pminuw", "pminud", "pmaxsb", "pmaxsd", "pmaxuw", "pmaxud", "pmulld", "phminposuw", "roundps",
    "roundpd", "roundss", "roundsd", "blendps", "blendpd", "pblendw", "pextrb", "pextrw", "pextrd", "extractps",
    "pinsrb", "insertps", "pinsrd", "crc32", "popcnt"};

#endif
