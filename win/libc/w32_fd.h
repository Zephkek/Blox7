#pragma once

#include <stdint.h>

#include "w32_kernel.h"

enum class W32Kind : uint8_t {
  kFile,
  kDirectory,
  kPipe,
  kConsole,
  kSocket,
  kNull,
  kRandom,
  kEventFd,
  kEpoll,
  kTimerFd,
  kMemory,
};

struct W32File {
  volatile int refs;
  W32Kind kind;
  int flags;
  HANDLE handle;
  int64_t pos;
  W32_SRWLOCK lock;
  WCHAR* path;
  char* upath;
  void* extra;
  void (*destroy)(W32File*);
};

long w32_socket_read(W32File* f, void* buf, size_t len);
long w32_socket_write(W32File* f, const void* buf, size_t len);
long w32_special_read(W32File* f, void* buf, size_t len);
long w32_special_write(W32File* f, const void* buf, size_t len);
long w32_readable(W32File* f);
struct iovec;
long w32_socket_vector_io(W32File* f, const struct iovec* iov, int count, bool writing);
short w32_socket_readable(W32File* f, short events);
short w32_socket_error(W32File* f);
short w32_socket_listener_shut(W32File* f, short events);

W32File* w32_socket_file(SOCKET s);

W32File* w32_fd_get(int fd);
void w32_file_put(W32File* f);

W32File* w32_file_new(W32Kind kind, HANDLE handle, int flags);

int w32_fd_install(W32File* f, bool cloexec, int min_fd = 0);
