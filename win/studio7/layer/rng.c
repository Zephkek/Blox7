#include "layer.h"
#include <bcrypt.h>

BOOL WINAPI s7_ProcessPrng(PBYTE data, SIZE_T size) {
    while (size) {
        ULONG part = size > 0x40000000 ? 0x40000000 : (ULONG)size;

        if (BCryptGenRandom(NULL, data, part, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return FALSE;
        data += part;
        size -= part;
    }
    return TRUE;
}
