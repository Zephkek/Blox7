#include <fcntl.h>
#include <sys/statfs.h>
#include <sys/syscall.h>

__attribute__((noinline)) static long raw3(long nr, long a, long b, long c) {
  long ret;
  __asm__ volatile("xor %%r10d, %%r10d\n\txor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\tsyscall"
                   : "=a"(ret)
                   : "a"(nr), "D"(a), "S"(b), "d"(c)
                   : "rcx", "r8", "r9", "r10", "r11", "memory");
  return ret;
}

long fake_open_stat_read(const char* path, char* buf, long size, long* fs_type) {
  long fd = raw3(SYS_openat, AT_FDCWD, (long)path, O_RDONLY);
  if (fd < 0) return fd;
  struct statfs st;
  long r = raw3(SYS_fstatfs, fd, (long)&st, 0);
  *fs_type = r < 0 ? r : (long)st.f_type;
  long got = raw3(SYS_read, fd, (long)buf, size);
  raw3(SYS_close, fd, 0, 0);
  return got;
}
