#include "layer.h"

void *memset(void *to, int value, size_t count) {
    __stosb((unsigned char *)to, (unsigned char)value, count);
    return to;
}

void *memcpy(void *to, const void *from, size_t count) {
    __movsb((unsigned char *)to, (const unsigned char *)from, count);
    return to;
}
