#ifndef S7_LAYER_H
#define S7_LAYER_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <intrin.h>

void s7_time_init(void);
void s7_sync_init(void);

#endif
