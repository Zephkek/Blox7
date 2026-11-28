#ifndef BLOX7_WV2UNPACK_H
#define BLOX7_WV2UNPACK_H

#include <windows.h>

#include <cstddef>

typedef void (*wv2_progress)(void* context, ULONGLONG done, ULONGLONG total);

ULONGLONG wv2_package_bytes();
const char* wv2_package_sha256();
ULONGLONG wv2_unpack_bytes(int all);
size_t wv2_unpack_longest(int all);
int wv2_unpack(const wchar_t* package, const wchar_t* out_dir, int all, wv2_progress progress, void* context,
               DWORD* win32);

#endif
