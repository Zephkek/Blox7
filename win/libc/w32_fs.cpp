// names windows forbids map to U+F000 + c, like cygwin
// FILE_SHARE_DELETE everywhere and unlink parks files in <root>\.deleted, so open files can go like on linux

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/magic.h>
#include <linux/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <unistd.h>

#include "w32_fd.h"
#include "w32_syscalls.h"

namespace {

constexpr DWORD GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
constexpr DWORD FILE_SHARE_ALL = 7;
constexpr DWORD CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5;
constexpr DWORD FILE_ATTRIBUTE_READONLY = 0x1, FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD FILE_ATTRIBUTE_REPARSE_POINT = 0x400;
constexpr DWORD FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xffffffff;
constexpr DWORD FILE_TYPE_DISK = 1, FILE_TYPE_CHAR = 2, FILE_TYPE_PIPE = 3;
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_COPY_ALLOWED = 2;
constexpr DWORD STD_INPUT = 0xfffffff6, STD_OUTPUT = 0xfffffff5, STD_ERROR = 0xfffffff4;
constexpr int kMaxFds = 65536;

struct Slot {
  W32File* file;
  bool cloexec;
};

Slot g_fds[kMaxFds];
int g_fd_hint;
W32_SRWLOCK g_fd_lock;
W32_SRWLOCK g_cwd_lock;
char g_cwd[W32_PATH_MAX] = "/";
char g_root_utf8[W32_PATH_MAX];
WCHAR g_root[W32_PATH_MAX];
size_t g_root_len;
volatile int g_deleted_counter;

struct Lock {
  W32_SRWLOCK* l;
  explicit Lock(W32_SRWLOCK* lock) : l(lock) { w32.AcquireSRWLockExclusive(l); }
  ~Lock() { w32.ReleaseSRWLockExclusive(l); }
};

char* dup_string(const char* s) {
  size_t n = strlen(s) + 1;
  char* p = static_cast<char*>(malloc(n));
  if (p) memcpy(p, s, n);
  return p;
}

WCHAR* dup_wstring(const WCHAR* s) {
  size_t n = 0;
  while (s[n]) n++;
  WCHAR* p = static_cast<WCHAR*>(malloc((n + 1) * sizeof(WCHAR)));
  if (p) memcpy(p, s, (n + 1) * sizeof(WCHAR));
  return p;
}

}  // namespace

W32File* w32_file_new(W32Kind kind, HANDLE handle, int flags) {
  W32File* f = static_cast<W32File*>(calloc(1, sizeof(W32File)));
  if (!f) return nullptr;
  f->refs = 1;
  f->kind = kind;
  f->handle = handle;
  f->flags = flags;
  return f;
}

void w32_file_put(W32File* f) {
  if (!f || __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
  if (f->destroy) f->destroy(f);
  switch (f->kind) {
    case W32Kind::kFile:
    case W32Kind::kDirectory:
    case W32Kind::kPipe:
      if (f->handle && f->handle != INVALID_HANDLE_VALUE) w32.CloseHandle(f->handle);
      break;
    default:
      break;
  }
  free(f->path);
  free(f->upath);
  free(f);
}

W32File* w32_fd_get(int fd) {
  if (fd < 0 || fd >= kMaxFds) return nullptr;
  Lock lock(&g_fd_lock);
  W32File* f = g_fds[fd].file;
  if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
  return f;
}

int w32_fd_install(W32File* f, bool cloexec, int min_fd) {
  Lock lock(&g_fd_lock);
  int start = min_fd > g_fd_hint ? min_fd : g_fd_hint;
  for (int fd = start; fd < kMaxFds; fd++) {
    if (!g_fds[fd].file) {
      g_fds[fd] = Slot{f, cloexec};
      __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
      if (min_fd <= g_fd_hint) g_fd_hint = fd + 1;
      return fd;
    }
  }
  return -EMFILE;
}

static long install_new(W32File* f, bool cloexec) {
  int fd = w32_fd_install(f, cloexec);
  w32_file_put(f);
  return fd;
}

const char* w32_root_utf8() { return g_root_utf8; }

static bool forbidden(uint32_t c) {
  return c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\\';
}

static void normalize(char* path) {
  char* out = path;
  const char* in = path;
  while (*in) {
    while (*in == '/') in++;
    const char* seg = in;
    while (*in && *in != '/') in++;
    size_t n = static_cast<size_t>(in - seg);
    if (n == 0 || (n == 1 && seg[0] == '.')) continue;
    if (n == 2 && seg[0] == '.' && seg[1] == '.') {
      while (out > path && *--out != '/') {}
      continue;
    }
    *out++ = '/';
    memmove(out, seg, n);
    out += n;
  }
  if (out == path) *out++ = '/';
  *out = 0;
}

static long absolute(int dirfd, const char* path, char* out) {
  if (!path) return -EFAULT;
  if (path[0] == 0) return -ENOENT;
  size_t len = strlen(path);
  if (path[0] == '/') {
    if (len >= W32_PATH_MAX) return -ENAMETOOLONG;
    memcpy(out, path, len + 1);
  } else {
    char base[W32_PATH_MAX];
    if (dirfd == AT_FDCWD) {
      Lock lock(&g_cwd_lock);
      strcpy(base, g_cwd);
    } else {
      W32File* f = w32_fd_get(dirfd);
      if (!f) return -EBADF;
      bool dir = f->kind == W32Kind::kDirectory && f->upath;
      if (dir) strcpy(base, f->upath);
      w32_file_put(f);
      if (!dir) return -ENOTDIR;
    }
    size_t blen = strlen(base);
    if (blen + 1 + len >= W32_PATH_MAX) return -ENAMETOOLONG;
    memcpy(out, base, blen);
    out[blen] = '/';
    memcpy(out + blen + 1, path, len + 1);
  }
  normalize(out);
  w32_proc_alias(out);
  return 0;
}

static long append_tail(WCHAR* out, size_t pos, size_t cap, const char* tail) {
  WCHAR tmp[W32_PATH_MAX];
  if (w32_utf8_to_utf16(tail, tmp, W32_PATH_MAX) < 0) return -ENAMETOOLONG;
  for (WCHAR* p = tmp; *p; p++) {
    if (pos + 2 >= cap) return -ENAMETOOLONG;
    WCHAR c = *p;
    out[pos++] = c == '/' ? '\\' : forbidden(c) ? static_cast<WCHAR>(0xf000 + c) : c;
  }
  out[pos] = 0;
  return static_cast<long>(pos);
}

static long to_win32(const char* abs, WCHAR* out, size_t cap) {
  if (strncmp(abs, "/mnt/", 5) == 0 && ((abs[5] | 0x20) >= 'a' && (abs[5] | 0x20) <= 'z') && (abs[6] == 0 || abs[6] == '/')) {
    static const WCHAR kPrefix[] = {'\\', '\\', '?', '\\'};
    memcpy(out, kPrefix, sizeof(kPrefix));
    out[4] = static_cast<WCHAR>(abs[5] & ~0x20);
    out[5] = ':';
    out[6] = '\\';
    out[7] = 0;
    long r = abs[6] ? append_tail(out, 7, cap, abs + 7) : 7;
    return r < 0 ? r : 0;
  }
  if (g_root_len + 1 >= cap) return -ENAMETOOLONG;
  memcpy(out, g_root, g_root_len * sizeof(WCHAR));
  out[g_root_len] = 0;
  if (abs[1] == 0) return 0;
  long r = append_tail(out, g_root_len, cap, abs);
  return r < 0 ? r : 0;
}

long w32_path(int dirfd, const char* path, WCHAR* out, size_t out_units) {
  char abs[W32_PATH_MAX];
  long r = absolute(dirfd, path, abs);
  return r < 0 ? r : to_win32(abs, out, out_units);
}

long w32_unix_path(const WCHAR* win, char* out, size_t cap) {
  const WCHAR* p = win;
  if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\') p += 4;
  const WCHAR* root = g_root + 4;
  size_t rlen = g_root_len - 4, i = 0;
  while (i < rlen && p[i] && (p[i] | 0x20) == (root[i] | 0x20)) i++;
  char tail[W32_PATH_MAX];
  size_t n = 0;
  if (i == rlen && (p[i] == 0 || p[i] == '\\')) {
    p += rlen;
  } else if (p[0] && p[1] == ':') {
    n = static_cast<size_t>(snprintf(tail, sizeof tail, "/mnt/%c", static_cast<char>(p[0] | 0x20)));
    p += 2;
  } else {
    return -ENOENT;
  }
  WCHAR conv[W32_PATH_MAX];
  size_t k = 0;
  for (; *p && k + 1 < W32_PATH_MAX; p++) {
    WCHAR c = *p;
    conv[k++] = c == '\\' ? '/' : (c >= 0xf000 && c < 0xf080 && forbidden(c - 0xf000)) ? static_cast<WCHAR>(c - 0xf000) : c;
  }
  conv[k] = 0;
  if (w32_utf16_to_utf8(conv, tail + n, sizeof(tail) - n) < 0) return -ENAMETOOLONG;
  if (tail[0] == 0) strcpy(tail, "/");
  if (strlen(tail) >= cap) return -ENAMETOOLONG;
  strcpy(out, tail);
  return 0;
}

namespace {

constexpr DWORD FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000;
constexpr DWORD IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003, IO_REPARSE_TAG_SYMLINK = 0xA000000C;
constexpr DWORD FSCTL_SET_REPARSE_POINT = 0x000900A4, FSCTL_GET_REPARSE_POINT = 0x000900A8;
constexpr DWORD SYMBOLIC_LINK_FLAG_DIRECTORY = 1, SYMLINK_FLAG_RELATIVE = 1;
constexpr DWORD FILE_ATTRIBUTE_SYSTEM = 0x4;
constexpr char kLinkFileMagic[] = "!<symlink>\xff\xfe";  // cygwin's link file, then the utf-16 target

constexpr char kSocketFileMagic[] = "!<socket >";  // then the loopback port (w32_net.cpp)

bool may_be_link_file(DWORD attrs) {
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_SYSTEM) &&
         !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

struct ReparseBuffer {
  DWORD tag;
  uint16_t data_length, reserved;
  uint16_t substitute_offset, substitute_length, print_offset, print_length;
  uint8_t rest[16 * 1024];
};

bool is_link_tag(DWORD tag) { return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT; }

}  // namespace

static long read_link_file(const WCHAR* win, char* out, size_t cap) {
  HANDLE h = w32.CreateFileW(win, GENERIC_READ, FILE_SHARE_ALL, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return w32_fail();
  constexpr size_t kMagic = sizeof(kLinkFileMagic) - 1;
  uint8_t data[kMagic + 2 * W32_PATH_MAX];
  DWORD got = 0;
  BOOL ok = w32.ReadFile(h, data, sizeof data - 2, &got, nullptr);
  w32.CloseHandle(h);
  if (!ok || got < kMagic + 2 || memcmp(data, kLinkFileMagic, kMagic) != 0) return -EINVAL;
  data[got] = data[got + 1] = 0;
  WCHAR text[W32_PATH_MAX];
  size_t units = (got - kMagic) / 2;
  if (units >= W32_PATH_MAX) units = W32_PATH_MAX - 1;
  memcpy(text, data + kMagic, units * 2);
  text[units] = 0;
  return w32_utf16_to_utf8(text, out, cap) < 0 ? -ENAMETOOLONG : 0;
}

static long read_link(const WCHAR* win, char* out, size_t cap) {
  if (may_be_link_file(w32.GetFileAttributesW(win))) return read_link_file(win, out, cap);
  HANDLE h = w32.CreateFileW(win, 0, FILE_SHARE_ALL, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return w32_fail();
  auto* rb = static_cast<ReparseBuffer*>(malloc(sizeof(ReparseBuffer)));
  if (!rb) {
    w32.CloseHandle(h);
    return -ENOMEM;
  }
  DWORD got = 0;
  BOOL ok = w32.DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, rb, sizeof(ReparseBuffer), &got, nullptr);
  w32.CloseHandle(h);
  long r = -EINVAL;
  if (ok && is_link_tag(rb->tag)) {
    bool symlink = rb->tag == IO_REPARSE_TAG_SYMLINK;
    DWORD flags = 0;
    const uint8_t* names = rb->rest;
    if (symlink) {
      memcpy(&flags, rb->rest, 4);
      names += 4;
    }
    uint16_t off = rb->print_length ? rb->print_offset : rb->substitute_offset;
    uint16_t len = rb->print_length ? rb->print_length : rb->substitute_length;
    WCHAR name[W32_PATH_MAX];
    size_t n = len / 2 < W32_PATH_MAX - 1 ? len / 2 : W32_PATH_MAX - 1;
    memcpy(name, names + off, n * 2);
    name[n] = 0;
    const WCHAR* p = name;
    if (p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\') p += 4;
    if (symlink && (flags & SYMLINK_FLAG_RELATIVE)) {
      WCHAR conv[W32_PATH_MAX];
      size_t k = 0;
      for (; p[k] && k + 1 < W32_PATH_MAX; k++) conv[k] = p[k] == '\\' ? '/' : p[k];
      conv[k] = 0;
      r = w32_utf16_to_utf8(conv, out, cap) < 0 ? -ENAMETOOLONG : 0;
    } else {
      r = w32_unix_path(p, out, cap);
    }
  }
  free(rb);
  return r;
}

static long make_junction(const WCHAR* link_w, const WCHAR* target_w) {
  const WCHAR* plain = target_w;
  if (plain[0] == '\\' && plain[1] == '\\' && plain[2] == '?' && plain[3] == '\\') plain += 4;
  size_t plain_len = 0;
  while (plain[plain_len]) plain_len++;
  size_t sub_bytes = (4 + plain_len) * 2, print_bytes = plain_len * 2;
  if (8 + sub_bytes + 2 + print_bytes + 2 > sizeof(ReparseBuffer::rest)) return -ENAMETOOLONG;
  auto* rb = static_cast<ReparseBuffer*>(calloc(1, sizeof(ReparseBuffer)));
  if (!rb) return -ENOMEM;
  rb->tag = IO_REPARSE_TAG_MOUNT_POINT;
  rb->substitute_offset = 0;
  rb->substitute_length = static_cast<uint16_t>(sub_bytes);
  rb->print_offset = static_cast<uint16_t>(sub_bytes + 2);
  rb->print_length = static_cast<uint16_t>(print_bytes);
  WCHAR* names = reinterpret_cast<WCHAR*>(rb->rest);
  static const WCHAR kNt[] = {'\\', '?', '?', '\\'};
  memcpy(names, kNt, sizeof kNt);
  memcpy(names + 4, plain, plain_len * 2);
  memcpy(names + 4 + plain_len + 1, plain, plain_len * 2);
  rb->data_length = static_cast<uint16_t>(8 + sub_bytes + 2 + print_bytes + 2);
  long r = 0;
  if (!w32.CreateDirectoryW(link_w, nullptr)) {
    r = w32_fail();
  } else {
    HANDLE h = w32.CreateFileW(link_w, GENERIC_WRITE, FILE_SHARE_ALL, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) {
      r = w32_fail();
    } else {
      if (!w32.DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, rb, 8 + rb->data_length, nullptr, 0, &got, nullptr)) r = w32_fail();
      w32.CloseHandle(h);
    }
    if (r < 0) w32.RemoveDirectoryW(link_w);
  }
  free(rb);
  return r;
}

static bool is_link(const WCHAR* win, DWORD attrs) {
  if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_REPARSE_POINT || may_be_link_file(attrs))) return false;
  char target[W32_PATH_MAX];
  return read_link(win, target, sizeof target) == 0;
}

static long make_link_file(const WCHAR* link_w, const char* target) {
  WCHAR text[W32_PATH_MAX];
  int units = w32_utf8_to_utf16(target, text, W32_PATH_MAX);
  if (units < 0) return -ENAMETOOLONG;
  HANDLE h = w32.CreateFileW(link_w, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_SYSTEM, nullptr);
  if (h == INVALID_HANDLE_VALUE) return w32_fail();
  DWORD done;
  bool ok = w32.WriteFile(h, kLinkFileMagic, sizeof(kLinkFileMagic) - 1, &done, nullptr) &&
            w32.WriteFile(h, text, static_cast<DWORD>(units + 1) * 2, &done, nullptr);
  w32.CloseHandle(h);
  if (!ok) {
    w32.DeleteFileW(link_w);
    return -EIO;
  }
  return 0;
}

static bool is_socket_file(const WCHAR* win) {
  HANDLE h = w32.CreateFileW(win, GENERIC_READ, FILE_SHARE_ALL, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char head[sizeof(kSocketFileMagic) - 1];
  DWORD got = 0;
  bool ok = w32.ReadFile(h, head, sizeof head, &got, nullptr) && got == sizeof head && memcmp(head, kSocketFileMagic, sizeof head) == 0;
  w32.CloseHandle(h);
  return ok;
}

static long follow_link_files(char* abs, WCHAR* win, DWORD* attrs) {
  for (int depth = 0; depth < 40; depth++) {
    if (!may_be_link_file(*attrs)) return 0;
    char target[W32_PATH_MAX], joined[W32_PATH_MAX];
    if (read_link_file(win, target, sizeof target) < 0) return 0;
    if (target[0] == '/') {
      snprintf(joined, sizeof joined, "%s", target);
    } else {
      snprintf(joined, sizeof joined, "%s", abs);
      char* slash = strrchr(joined, '/');
      snprintf(slash + 1, sizeof joined - static_cast<size_t>(slash + 1 - joined), "%s", target);
    }
    long r = absolute(AT_FDCWD, joined, abs);
    if (r == 0) r = to_win32(abs, win, W32_PATH_MAX);
    if (r < 0) return r;
    *attrs = w32.GetFileAttributesW(win);
  }
  return -ELOOP;
}

void w32_init_files() {
  const char* env_root = nullptr;
  for (char** e = environ; e && *e; e++)
    if (strncmp(*e, "BLOX7_ROOT=", 11) == 0) env_root = *e + 11;
  char root[W32_PATH_MAX];
  if (env_root && *env_root) {
    snprintf(root, sizeof root, "%s", env_root);
  } else {
    snprintf(root, sizeof root, "%s", w32_boot->exe_dir);
    size_t len = strlen(root);
    if (len > 5 && strcasecmp(root + len - 5, "\\bin\\") == 0) root[len - 4] = 0;
    snprintf(root + strlen(root), sizeof root - strlen(root), "root");
  }
  size_t n = strlen(root);
  while (n > 3 && (root[n - 1] == '\\' || root[n - 1] == '/')) root[--n] = 0;
  for (char* c = root; *c; c++)
    if (*c == '/') *c = '\\';
  snprintf(g_root_utf8, sizeof g_root_utf8, "%s", root);
  static const WCHAR kPrefix[] = {'\\', '\\', '?', '\\'};
  memcpy(g_root, kPrefix, sizeof(kPrefix));
  int len = w32_utf8_to_utf16(root, g_root + 4, W32_PATH_MAX - 4);
  if (len < 0) w32_fatal("BLOX7_ROOT is too long");
  g_root_len = static_cast<size_t>(len) + 4;
  w32.CreateDirectoryW(g_root, nullptr);
  WCHAR deleted[W32_PATH_MAX];
  to_win32("/.deleted", deleted, W32_PATH_MAX);
  w32.CreateDirectoryW(deleted, nullptr);
  static const char* const kDirs[] = {"/tmp", "/data", "/dev", "/proc"};
  for (const char* d : kDirs) {
    WCHAR w[W32_PATH_MAX];
    to_win32(d, w, W32_PATH_MAX);
    w32.CreateDirectoryW(w, nullptr);
  }

  static const DWORD kStd[3] = {STD_INPUT, STD_OUTPUT, STD_ERROR};
  for (int i = 0; i < 3; i++) {
    HANDLE h = w32.GetStdHandle(kStd[i]);
    W32File* f;
    DWORD mode;
    if (!h || h == INVALID_HANDLE_VALUE) {
      f = w32_file_new(W32Kind::kNull, nullptr, O_RDWR);
    } else if (w32.GetConsoleMode(h, &mode)) {
      f = w32_file_new(W32Kind::kConsole, h, i == 0 ? O_RDONLY : O_WRONLY);
    } else if ((f = w32_socket_file(reinterpret_cast<SOCKET>(h))) != nullptr) {
    } else {
      DWORD type = w32.GetFileType(h);
      // redirected stdout/stderr are shared (2>&1), so append like the shell does
      f = w32_file_new(type == FILE_TYPE_PIPE ? W32Kind::kPipe : W32Kind::kFile, nullptr, i == 0 ? O_RDONLY : (O_WRONLY | O_APPEND));
      w32.DuplicateHandle(w32.GetCurrentProcess(), h, w32.GetCurrentProcess(), &f->handle, 0, 0, 2 /* SAME_ACCESS */);
    }
    // bionic's start-up has already put /dev/null on descriptors it found closed: replace them
    W32File* old;
    {
      Lock lock(&g_fd_lock);
      old = g_fds[i].file;
      g_fds[i] = Slot{f, false};
    }
    w32_file_put(old);
  }
}

namespace {

struct MemoryFile {
  char* data;
  size_t size;
};

void destroy_memory(W32File* f) {
  MemoryFile* m = static_cast<MemoryFile*>(f->extra);
  if (m) free(m->data);
  free(m);
}

struct Listing;
void destroy_listing(W32File* f);
Listing* proc_listing(const char* abs);

long open_special(const char* abs, int flags, bool* handled) {
  *handled = true;
  W32File* f = nullptr;
  if (strcmp(abs, "/dev/null") == 0 || strcmp(abs, "/dev/zero") == 0) {
    f = w32_file_new(W32Kind::kNull, nullptr, flags);
    if (f && abs[5] == 'z') f->pos = 1;
  } else if (strcmp(abs, "/dev/urandom") == 0 || strcmp(abs, "/dev/random") == 0) {
    f = w32_file_new(W32Kind::kRandom, nullptr, flags);
  } else if (strcmp(abs, "/dev/tty") == 0) {
    return -ENXIO;
  } else {
    switch (w32_proc_node(abs)) {
      case W32ProcNode::kNone:
        *handled = false;
        return 0;
      case W32ProcNode::kFile: {
        if ((flags & O_ACCMODE) != O_RDONLY) return -EACCES;
        char* data = nullptr;
        size_t size = 0;
        if (!w32_proc_read(abs, &data, &size)) return -ENOMEM;
        f = w32_file_new(W32Kind::kMemory, nullptr, flags);
        MemoryFile* m = static_cast<MemoryFile*>(malloc(sizeof(MemoryFile)));
        if (!f || !m) {
          free(m);
          free(data);
          w32_file_put(f);
          return -ENOMEM;
        }
        *m = MemoryFile{data, size};
        f->extra = m;
        f->destroy = destroy_memory;
        break;
      }
      case W32ProcNode::kDirectory: {
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT)) return -EISDIR;
        Listing* l = proc_listing(abs);
        if (!l) return -ENOMEM;
        f = w32_file_new(W32Kind::kDirectory, nullptr, flags);
        if (!f) {
          W32File tmp{};
          tmp.extra = l;
          destroy_listing(&tmp);
          return -ENOMEM;
        }
        f->extra = l;
        f->destroy = destroy_listing;
        break;
      }
      case W32ProcNode::kLink: {
        if (strncmp(abs, "/proc/self/fd/", 14) == 0) {
          return sys_fcntl(atoi(abs + 14), (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0, 0, 0, 0);
        }
        char target[W32_PATH_MAX];
        if (!w32_proc_readlink(abs, target, sizeof target) || target[0] != '/') return -ENXIO;
        return sys_openat(AT_FDCWD, reinterpret_cast<long>(target), flags, 0, 0, 0);
      }
    }
  }
  if (!f) return -ENOMEM;
  f->upath = dup_string(abs);
  return install_new(f, flags & O_CLOEXEC);
}

bool is_dev_or_proc(const char* abs) {
  return strncmp(abs, "/dev/", 5) == 0 || strncmp(abs, "/proc/", 6) == 0 || strncmp(abs, "/sys/", 5) == 0 ||
         strcmp(abs, "/proc") == 0 || strcmp(abs, "/sys") == 0;
}

void proc_stat(W32ProcNode node, const char* abs, struct stat* st) {
  memset(st, 0, sizeof(*st));
  st->st_dev = strncmp(abs, "/sys", 4) == 0 ? makedev(0, 20) : makedev(0, 21);
  uint64_t h = 1469598103934665603ull;
  for (const char* c = abs; *c; c++) h = (h ^ static_cast<unsigned char>(*c)) * 1099511628211ull;
  st->st_ino = h & 0xffffffff;
  st->st_uid = st->st_gid = 10000;
  st->st_blksize = 1024;
  st->st_nlink = node == W32ProcNode::kDirectory ? 2 : 1;
  st->st_mode = node == W32ProcNode::kDirectory ? (S_IFDIR | 0555) : node == W32ProcNode::kLink ? (S_IFLNK | 0777)
                                                                                                 : (S_IFREG | 0444);
  const int64_t now = w32_realtime_ns();
  st->st_atim = st->st_mtim = st->st_ctim = timespec{static_cast<time_t>(now / 1000000000), now % 1000000000};
}

}  // namespace

long openat_impl(long dirfd, long path_l, long flags_l, long mode) {
  const char* path = reinterpret_cast<const char*>(path_l);
  int flags = static_cast<int>(flags_l);
  char abs[W32_PATH_MAX];
  long r = absolute(static_cast<int>(dirfd), path, abs);
  if (r < 0) return r;
  if (is_dev_or_proc(abs)) {
    bool handled;
    r = open_special(abs, flags, &handled);
    if (handled) return r;
  }
  WCHAR win[W32_PATH_MAX];
  r = to_win32(abs, win, W32_PATH_MAX);
  if (r < 0) return r;

  DWORD attrs = w32.GetFileAttributesW(win);
  bool exists = attrs != INVALID_FILE_ATTRIBUTES;
  if ((flags & O_CREAT) && (flags & O_EXCL) && exists) return -EEXIST;
  if (may_be_link_file(attrs)) {
    char target[W32_PATH_MAX];
    if ((flags & O_NOFOLLOW) && read_link_file(win, target, sizeof target) == 0) return -ELOOP;
    r = follow_link_files(abs, win, &attrs);
    if (r < 0) return r;
    exists = attrs != INVALID_FILE_ATTRIBUTES;
  }
  if (exists && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT)) return -EISDIR;
    HANDLE h = w32.CreateFileW(win, 0, FILE_SHARE_ALL, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return w32_fail();
    W32File* f = w32_file_new(W32Kind::kDirectory, h, flags);
    if (!f) return -ENOMEM;
    f->path = dup_wstring(win);
    f->upath = dup_string(abs);
    return install_new(f, flags & O_CLOEXEC);
  }
  if (flags & O_DIRECTORY) return exists ? -ENOTDIR : -ENOENT;
  if (!exists && !(flags & O_CREAT)) {
    DWORD err = w32.GetLastError();
    return err == W32_ERROR_PATH_NOT_FOUND ? -ENOENT : -w32_errno_from_win32(err);
  }

  DWORD access = 0;
  switch (flags & O_ACCMODE) {
    case O_RDONLY: access = GENERIC_READ; break;
    case O_WRONLY: access = GENERIC_WRITE; break;
    default: access = GENERIC_READ | GENERIC_WRITE; break;
  }
  if (flags & O_PATH) access = 0;
  DWORD disposition;
  if (flags & O_CREAT) {
    disposition = (flags & O_EXCL) ? CREATE_NEW : (flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
  } else {
    disposition = (flags & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
  }
  DWORD attributes = (flags & O_CREAT) && !(mode & 0222) ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
  HANDLE h = w32.CreateFileW(win, access, FILE_SHARE_ALL, nullptr, disposition, attributes, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = w32.GetLastError();
    if (err == W32_ERROR_FILE_EXISTS) return -EEXIST;
    if (err == W32_ERROR_ACCESS_DENIED && exists && (attrs & FILE_ATTRIBUTE_READONLY)) return -EACCES;
    return -w32_errno_from_win32(err);
  }
  W32Kind kind = W32Kind::kFile;
  DWORD type = w32.GetFileType(h);
  if (type == FILE_TYPE_PIPE) kind = W32Kind::kPipe;
  W32File* f = w32_file_new(kind, h, flags);
  if (!f) {
    w32.CloseHandle(h);
    return -ENOMEM;
  }
  f->path = dup_wstring(win);
  f->upath = dup_string(abs);
  return install_new(f, flags & O_CLOEXEC);
}

long sys_close(long fd_l, long, long, long, long, long) {
  int fd = static_cast<int>(fd_l);
  if (fd < 0 || fd >= kMaxFds) return -EBADF;
  W32File* f;
  {
    Lock lock(&g_fd_lock);
    f = g_fds[fd].file;
    if (!f) return -EBADF;
    g_fds[fd].file = nullptr;
    if (fd < g_fd_hint) g_fd_hint = fd;
  }
  w32_file_put(f);
  return 0;
}

long sys_close_range(long first, long last, long, long, long, long) {
  for (long fd = first; fd <= last && fd < kMaxFds; fd++) sys_close(fd, 0, 0, 0, 0, 0);
  return 0;
}

long sys_dup3(long old_fd, long new_fd, long flags, long, long, long) {
  if (old_fd == new_fd) return -EINVAL;
  if (new_fd < 0 || new_fd >= kMaxFds) return -EBADF;
  W32File* f = w32_fd_get(static_cast<int>(old_fd));
  if (!f) return -EBADF;
  W32File* old = nullptr;
  {
    Lock lock(&g_fd_lock);
    old = g_fds[new_fd].file;
    g_fds[new_fd] = Slot{f, (flags & O_CLOEXEC) != 0};
  }
  w32_file_put(old);
  return new_fd;
}

long sys_dup(long fd, long, long, long, long, long) {
  W32File* f = w32_fd_get(static_cast<int>(fd));
  if (!f) return -EBADF;
  return install_new(f, false);
}

long sys_fcntl(long fd_l, long cmd, long arg, long, long, long) {
  int fd = static_cast<int>(fd_l);
  W32File* f = w32_fd_get(fd);
  if (!f) return -EBADF;
  long r = 0;
  switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
      r = w32_fd_install(f, cmd == F_DUPFD_CLOEXEC, static_cast<int>(arg));
      break;
    case F_GETFD: {
      Lock lock(&g_fd_lock);
      r = g_fds[fd].cloexec ? FD_CLOEXEC : 0;
      break;
    }
    case F_SETFD: {
      Lock lock(&g_fd_lock);
      g_fds[fd].cloexec = (arg & FD_CLOEXEC) != 0;
      break;
    }
    case F_GETFL:
      r = f->flags & ~O_CLOEXEC;
      break;
    case F_SETFL: {
      int changeable = O_APPEND | O_NONBLOCK;
      f->flags = (f->flags & ~changeable) | (static_cast<int>(arg) & changeable);
      if (f->kind == W32Kind::kSocket) {
        uint32_t nb = (f->flags & O_NONBLOCK) ? 1 : 0;
        w32.ioctlsocket(reinterpret_cast<SOCKET>(f->handle), static_cast<long>(0x8004667e) /* FIONBIO */, &nb);
      }
      break;
    }
    case F_GETLK:
      reinterpret_cast<struct flock*>(arg)->l_type = F_UNLCK;
      break;
    case F_SETLK:
    case F_SETLKW:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
      break;
    case F_GETPIPE_SZ:
      r = 65536;
      break;
    case F_SETPIPE_SZ:
      r = arg;
      break;
    case F_ADD_SEALS:
    case F_GET_SEALS:
      r = 0;
      break;
    default:
      r = -EINVAL;
  }
  w32_file_put(f);
  return r;
}

namespace {

long file_read(W32File* f, void* buf, size_t len, int64_t offset) {
  if (len > 0x7ffff000) len = 0x7ffff000;
  W32_OVERLAPPED ov{};
  ov.offset = static_cast<DWORD>(offset);
  ov.offset_high = static_cast<DWORD>(offset >> 32);
  DWORD done = 0;
  if (!w32.ReadFile(f->handle, buf, static_cast<DWORD>(len), &done, &ov)) {
    DWORD err = w32.GetLastError();
    if (err == W32_ERROR_HANDLE_EOF) return 0;
    return -w32_errno_from_win32(err);
  }
  return static_cast<long>(done);
}

long file_write(W32File* f, const void* buf, size_t len, int64_t offset) {
  if (len > 0x7ffff000) len = 0x7ffff000;
  W32_OVERLAPPED ov{};
  if (offset < 0) {
    ov.offset = ov.offset_high = 0xffffffff;
  } else {
    ov.offset = static_cast<DWORD>(offset);
    ov.offset_high = static_cast<DWORD>(offset >> 32);
  }
  DWORD done = 0;
  if (!w32.WriteFile(f->handle, buf, static_cast<DWORD>(len), &done, &ov)) return w32_fail();
  return static_cast<long>(done);
}

long stream_read(W32File* f, void* buf, size_t len) {
  if (len > 0x7ffff000) len = 0x7ffff000;
  if ((f->flags & O_NONBLOCK) && f->kind == W32Kind::kPipe) {
    DWORD avail = 0;
    if (!w32.PeekNamedPipe(f->handle, nullptr, 0, nullptr, &avail, nullptr)) {
      return w32.GetLastError() == W32_ERROR_BROKEN_PIPE ? 0 : w32_fail();
    }
    if (avail == 0) return -EAGAIN;
    if (len > avail) len = avail;
  }
  DWORD done = 0;
  if (!w32.ReadFile(f->handle, buf, static_cast<DWORD>(len), &done, nullptr)) {
    DWORD err = w32.GetLastError();
    if (err == W32_ERROR_BROKEN_PIPE || err == W32_ERROR_HANDLE_EOF) return 0;
    return -w32_errno_from_win32(err);
  }
  return static_cast<long>(done);
}

long console_write(W32File* f, const void* buf, size_t len) {
  const char* p = static_cast<const char*>(buf);
  size_t left = len;
  while (left) {
    char chunk[1024];
    size_t n = left < sizeof(chunk) - 1 ? left : sizeof(chunk) - 1;
    memcpy(chunk, p, n);
    chunk[n] = 0;
    WCHAR wide[1024];
    int units = w32_utf8_to_utf16(chunk, wide, 1024);
    DWORD done;
    if (units > 0) w32.WriteConsoleW(f->handle, wide, static_cast<DWORD>(units), &done, nullptr);
    p += n;
    left -= n;
  }
  return static_cast<long>(len);
}

long do_read(W32File* f, void* buf, size_t len, int64_t offset) {
  switch (f->kind) {
    case W32Kind::kFile: {
      if (offset >= 0) return file_read(f, buf, len, offset);
      Lock lock(&f->lock);
      long r = file_read(f, buf, len, f->pos);
      if (r > 0) f->pos += r;
      return r;
    }
    case W32Kind::kPipe:
    case W32Kind::kConsole:
      return offset >= 0 ? -ESPIPE : stream_read(f, buf, len);
    case W32Kind::kNull:
      if (f->pos == 1) {
        memset(buf, 0, len);
        return static_cast<long>(len);
      }
      return 0;
    case W32Kind::kRandom: {
      size_t done = 0;
      while (done < len) {
        DWORD n = len - done > 0x10000000 ? 0x10000000 : static_cast<DWORD>(len - done);
        if (!w32.SystemFunction036(static_cast<char*>(buf) + done, n)) return -EIO;
        done += n;
      }
      return static_cast<long>(len);
    }
    case W32Kind::kMemory: {
      MemoryFile* m = static_cast<MemoryFile*>(f->extra);
      Lock lock(&f->lock);
      int64_t at = offset >= 0 ? offset : f->pos;
      if (at >= static_cast<int64_t>(m->size)) return 0;
      size_t n = m->size - static_cast<size_t>(at);
      if (n > len) n = len;
      memcpy(buf, m->data + at, n);
      if (offset < 0) f->pos += static_cast<int64_t>(n);
      return static_cast<long>(n);
    }
    case W32Kind::kSocket:
      return offset >= 0 ? -ESPIPE : w32_socket_read(f, buf, len);
    case W32Kind::kEventFd:
    case W32Kind::kTimerFd:
      return w32_special_read(f, buf, len);
    case W32Kind::kDirectory:
      return -EISDIR;
    default:
      return -EINVAL;
  }
}

long do_write(W32File* f, const void* buf, size_t len, int64_t offset) {
  if ((f->flags & O_ACCMODE) == O_RDONLY && f->kind != W32Kind::kSocket && f->kind != W32Kind::kEventFd) return -EBADF;
  switch (f->kind) {
    case W32Kind::kFile: {
      if (offset >= 0) return file_write(f, buf, len, offset);
      Lock lock(&f->lock);
      if (f->flags & O_APPEND) {
        long r = file_write(f, buf, len, -1);
        int64_t size;
        if (r > 0 && w32.GetFileSizeEx(f->handle, &size)) f->pos = size;
        return r;
      }
      long r = file_write(f, buf, len, f->pos);
      if (r > 0) f->pos += r;
      return r;
    }
    case W32Kind::kPipe: {
      if (offset >= 0) return -ESPIPE;
      DWORD done = 0;
      if (!w32.WriteFile(f->handle, buf, len > 0x7ffff000 ? 0x7ffff000 : static_cast<DWORD>(len), &done, nullptr)) {
        DWORD err = w32.GetLastError();
        return (err == W32_ERROR_BROKEN_PIPE || err == W32_ERROR_NO_DATA) ? -EPIPE : -w32_errno_from_win32(err);
      }
      return static_cast<long>(done);
    }
    case W32Kind::kConsole:
      return console_write(f, buf, len);
    case W32Kind::kNull:
    case W32Kind::kRandom:
      return static_cast<long>(len);
    case W32Kind::kSocket:
      return offset >= 0 ? -ESPIPE : w32_socket_write(f, buf, len);
    case W32Kind::kEventFd:
    case W32Kind::kTimerFd:
      return w32_special_write(f, buf, len);
    case W32Kind::kDirectory:
      return -EISDIR;
    default:
      return -EBADF;
  }
}

template <typename Fn>
long with_file(long fd, Fn fn) {
  W32File* f = w32_fd_get(static_cast<int>(fd));
  if (!f) return -EBADF;
  long r = fn(f);
  w32_file_put(f);
  return r;
}

}  // namespace

long sys_read(long fd, long buf, long len, long, long, long) {
  return with_file(fd, [&](W32File* f) { return do_read(f, reinterpret_cast<void*>(buf), static_cast<size_t>(len), -1); });
}

long sys_write(long fd, long buf, long len, long, long, long) {
  return with_file(fd, [&](W32File* f) { return do_write(f, reinterpret_cast<const void*>(buf), static_cast<size_t>(len), -1); });
}

long sys_pread64(long fd, long buf, long len, long offset, long, long) {
  if (offset < 0) return -EINVAL;
  return with_file(fd, [&](W32File* f) { return do_read(f, reinterpret_cast<void*>(buf), static_cast<size_t>(len), offset); });
}

long sys_pwrite64(long fd, long buf, long len, long offset, long, long) {
  if (offset < 0) return -EINVAL;
  return with_file(fd, [&](W32File* f) { return do_write(f, reinterpret_cast<const void*>(buf), static_cast<size_t>(len), offset); });
}

static long vector_io(long fd, long iov_l, long count, int64_t offset, bool writing) {
  auto iov = reinterpret_cast<const iovec*>(iov_l);
  if (count < 0 || count > IOV_MAX) return -EINVAL;
  return with_file(fd, [&](W32File* f) {
    if (f->kind == W32Kind::kSocket) return offset >= 0 ? -ESPIPE : w32_socket_vector_io(f, iov, static_cast<int>(count), writing);
    long total = 0;
    for (long i = 0; i < count; i++) {
      if (iov[i].iov_len == 0) continue;
      int64_t at = offset >= 0 ? offset + total : -1;
      long r = writing ? do_write(f, iov[i].iov_base, iov[i].iov_len, at) : do_read(f, iov[i].iov_base, iov[i].iov_len, at);
      if (r < 0) return total ? total : r;
      total += r;
      if (static_cast<size_t>(r) < iov[i].iov_len) break;
    }
    return total;
  });
}

long sys_readv(long fd, long iov, long count, long, long, long) { return vector_io(fd, iov, count, -1, false); }
long sys_writev(long fd, long iov, long count, long, long, long) { return vector_io(fd, iov, count, -1, true); }
long sys_preadv(long fd, long iov, long count, long lo, long, long) { return vector_io(fd, iov, count, lo, false); }
long sys_pwritev(long fd, long iov, long count, long lo, long, long) { return vector_io(fd, iov, count, lo, true); }
long sys_preadv2(long fd, long iov, long count, long lo, long, long) { return vector_io(fd, iov, count, lo, false); }
long sys_pwritev2(long fd, long iov, long count, long lo, long, long) { return vector_io(fd, iov, count, lo, true); }

long sys_lseek(long fd, long offset, long whence, long, long, long) {
  return with_file(fd, [&](W32File* f) -> long {
    if (f->kind == W32Kind::kPipe || f->kind == W32Kind::kSocket || f->kind == W32Kind::kConsole) return -ESPIPE;
    Lock lock(&f->lock);
    int64_t base = 0;
    if (whence == SEEK_CUR) {
      base = f->pos;
    } else if (whence == SEEK_END) {
      if (f->kind == W32Kind::kFile) {
        if (!w32.GetFileSizeEx(f->handle, &base)) return w32_fail();
      } else if (f->kind == W32Kind::kMemory) {
        base = static_cast<int64_t>(static_cast<MemoryFile*>(f->extra)->size);
      }
    } else if (whence != SEEK_SET) {
      return -EINVAL;
    }
    int64_t pos = base + offset;
    if (pos < 0) return -EINVAL;
    if (f->kind == W32Kind::kDirectory && pos == 0 && f->extra) {
      if (f->destroy) f->destroy(f);
      f->extra = nullptr;
      f->destroy = nullptr;
    }
    f->pos = pos;
    return static_cast<long>(pos);
  });
}

long sys_fsync(long fd, long, long, long, long, long) {
  return with_file(fd, [](W32File* f) -> long {
    if (f->kind == W32Kind::kFile) w32.FlushFileBuffers(f->handle);
    return 0;
  });
}
long sys_fdatasync(long fd, long, long, long, long, long) { return sys_fsync(fd, 0, 0, 0, 0, 0); }
long sys_sync(long, long, long, long, long, long) { return 0; }
long sys_syncfs(long, long, long, long, long, long) { return 0; }
long sys_sync_file_range(long, long, long, long, long, long) { return 0; }
long sys_fadvise64(long, long, long, long, long, long) { return 0; }
long sys_readahead(long, long, long, long, long, long) { return 0; }

static long set_size(HANDLE h, int64_t size) {
  int64_t old_pos;
  if (!w32.SetFilePointerEx(h, size, &old_pos, 0) || !w32.SetEndOfFile(h)) return w32_fail();
  return 0;
}

long sys_ftruncate(long fd, long length, long, long, long, long) {
  if (length < 0) return -EINVAL;
  return with_file(fd, [&](W32File* f) -> long {
    if (f->kind != W32Kind::kFile) return -EINVAL;
    return set_size(f->handle, length);
  });
}

long sys_truncate(long path, long length, long, long, long, long) {
  long fd = sys_openat(AT_FDCWD, path, O_WRONLY | O_CLOEXEC, 0, 0, 0);
  if (fd < 0) return fd;
  long r = sys_ftruncate(fd, length, 0, 0, 0, 0);
  sys_close(fd, 0, 0, 0, 0, 0);
  return r;
}

long sys_fallocate(long fd, long mode, long offset, long len, long, long) {
  if (mode & ~FALLOC_FL_KEEP_SIZE) return -EOPNOTSUPP;
  return with_file(fd, [&](W32File* f) -> long {
    if (f->kind != W32Kind::kFile) return -ENODEV;
    int64_t size;
    if (!w32.GetFileSizeEx(f->handle, &size)) return w32_fail();
    if (!(mode & FALLOC_FL_KEEP_SIZE) && offset + len > size) return set_size(f->handle, offset + len);
    return 0;
  });
}

namespace {

void fill_stat(const W32_BY_HANDLE_FILE_INFORMATION& info, struct stat* st) {
  memset(st, 0, sizeof(*st));
  st->st_dev = info.volume_serial;
  st->st_ino = (static_cast<uint64_t>(info.index_hi) << 32) | info.index_lo;
  st->st_nlink = info.links ? info.links : 1;
  bool dir = info.attributes & FILE_ATTRIBUTE_DIRECTORY;
  // owner-only modes, careful linux code checks its private files for that
  unsigned perms = 0700;
  if ((info.attributes & FILE_ATTRIBUTE_READONLY) && !dir) perms &= ~0222u;
  st->st_mode = (dir ? S_IFDIR : S_IFREG) | perms;
  st->st_uid = st->st_gid = 10000;
  st->st_size = dir ? 4096 : static_cast<off_t>((static_cast<uint64_t>(info.size_hi) << 32) | info.size_lo);
  st->st_blksize = 4096;
  st->st_blocks = (st->st_size + 511) / 512;
  int64_t a = w32_filetime_to_ns(info.access), m = w32_filetime_to_ns(info.write), c = w32_filetime_to_ns(info.creation);
  st->st_atim = timespec{static_cast<time_t>(a / 1000000000), a % 1000000000};
  st->st_mtim = timespec{static_cast<time_t>(m / 1000000000), m % 1000000000};
  st->st_ctim = m > c ? st->st_mtim : timespec{static_cast<time_t>(c / 1000000000), c % 1000000000};
}

void special_stat(W32Kind kind, struct stat* st) {
  memset(st, 0, sizeof(*st));
  st->st_nlink = 1;
  st->st_uid = st->st_gid = 10000;
  st->st_blksize = 4096;
  switch (kind) {
    case W32Kind::kPipe: st->st_mode = S_IFIFO | 0600; break;
    case W32Kind::kSocket: st->st_mode = S_IFSOCK | 0600; break;
    case W32Kind::kMemory: st->st_mode = S_IFREG | 0444; break;
    default: st->st_mode = S_IFCHR | 0666; break;
  }
}

}  // namespace

long sys_fstat(long fd, long st_l, long, long, long, long) {
  auto st = reinterpret_cast<struct stat*>(st_l);
  return with_file(fd, [&](W32File* f) -> long {
    if (f->kind == W32Kind::kDirectory && !f->handle) {
      proc_stat(W32ProcNode::kDirectory, f->upath ? f->upath : "/proc", st);
      return 0;
    }
    if (f->kind == W32Kind::kFile || f->kind == W32Kind::kDirectory) {
      W32_BY_HANDLE_FILE_INFORMATION info;
      if (!w32.GetFileInformationByHandle(f->handle, &info)) return w32_fail();
      fill_stat(info, st);
      return 0;
    }
    special_stat(f->kind, st);
    if (f->kind == W32Kind::kMemory) st->st_size = 0;
    return 0;
  });
}

long newfstatat_impl(long dirfd, long path_l, long st_l, long flags);

long sys_newfstatat(long dirfd, long path_l, long st_l, long flags, long, long) {
  return newfstatat_impl(dirfd, path_l, st_l, flags);
}

long newfstatat_impl(long dirfd, long path_l, long st_l, long flags) {
  const char* path = reinterpret_cast<const char*>(path_l);
  if ((flags & AT_EMPTY_PATH) && path && path[0] == 0) return sys_fstat(dirfd, st_l, 0, 0, 0, 0);
  char abs[W32_PATH_MAX];
  long r = absolute(static_cast<int>(dirfd), path, abs);
  if (r < 0) return r;
  auto st = reinterpret_cast<struct stat*>(st_l);
  if (is_dev_or_proc(abs)) {
    if (strcmp(abs, "/dev/null") == 0 || strcmp(abs, "/dev/zero") == 0 || strcmp(abs, "/dev/urandom") == 0 ||
        strcmp(abs, "/dev/random") == 0) {
      special_stat(W32Kind::kNull, st);
      return 0;
    }
    const W32ProcNode node = w32_proc_node(abs);
    if (node == W32ProcNode::kLink && !(flags & AT_SYMLINK_NOFOLLOW)) {
      if (strncmp(abs, "/proc/self/fd/", 14) == 0) return sys_fstat(atoi(abs + 14), st_l, 0, 0, 0, 0);
      char target[W32_PATH_MAX];
      if (!w32_proc_readlink(abs, target, sizeof target) || target[0] != '/') return -ENOENT;
      return newfstatat_impl(AT_FDCWD, reinterpret_cast<long>(target), st_l, flags);
    }
    if (node != W32ProcNode::kNone) {
      proc_stat(node, abs, st);
      return 0;
    }
  }
  WCHAR win[W32_PATH_MAX];
  r = to_win32(abs, win, W32_PATH_MAX);
  if (r < 0) return r;
  DWORD open_flags = FILE_FLAG_BACKUP_SEMANTICS;
  size_t link_size = 0;
  DWORD attrs = w32.GetFileAttributesW(win);
  if (flags & AT_SYMLINK_NOFOLLOW) {
    char target[W32_PATH_MAX];
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) && read_link(win, target, sizeof target) == 0) {
      open_flags |= FILE_FLAG_OPEN_REPARSE_POINT;
      link_size = strlen(target);
    } else if (may_be_link_file(attrs) && read_link_file(win, target, sizeof target) == 0) {
      link_size = strlen(target);
    }
  } else if (may_be_link_file(attrs)) {
    r = follow_link_files(abs, win, &attrs);
    if (r < 0) return r;
  }
  HANDLE h = w32.CreateFileW(win, 0, FILE_SHARE_ALL, nullptr, OPEN_EXISTING, open_flags, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = w32.GetLastError();
    return -(err == W32_ERROR_PATH_NOT_FOUND || err == W32_ERROR_INVALID_NAME ? ENOENT : w32_errno_from_win32(err));
  }
  W32_BY_HANDLE_FILE_INFORMATION info;
  bool ok = w32.GetFileInformationByHandle(h, &info);
  w32.CloseHandle(h);
  if (!ok) return w32_fail();
  fill_stat(info, st);
  if (link_size) {
    st->st_mode = S_IFLNK | 0777;
    st->st_size = static_cast<off_t>(link_size);
  } else if (may_be_link_file(attrs) && is_socket_file(win)) {
    st->st_mode = S_IFSOCK | (st->st_mode & 0600);
    st->st_size = 0;
  }
  return 0;
}

long sys_statx(long dirfd, long path_l, long flags, long, long buf_l, long) {
  struct stat st;
  const long r = sys_newfstatat(dirfd, path_l, reinterpret_cast<long>(&st), flags & (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH),
                                0, 0);
  if (r < 0) return r;
  auto* x = reinterpret_cast<struct statx*>(buf_l);
  memset(x, 0, sizeof(*x));
  x->stx_mask = STATX_BASIC_STATS;
  x->stx_blksize = static_cast<uint32_t>(st.st_blksize);
  x->stx_nlink = static_cast<uint32_t>(st.st_nlink);
  x->stx_uid = st.st_uid;
  x->stx_gid = st.st_gid;
  x->stx_mode = static_cast<uint16_t>(st.st_mode);
  x->stx_ino = st.st_ino;
  x->stx_size = static_cast<uint64_t>(st.st_size);
  x->stx_blocks = static_cast<uint64_t>(st.st_blocks);
  x->stx_atime.tv_sec = st.st_atim.tv_sec;
  x->stx_atime.tv_nsec = static_cast<uint32_t>(st.st_atim.tv_nsec);
  x->stx_mtime.tv_sec = st.st_mtim.tv_sec;
  x->stx_mtime.tv_nsec = static_cast<uint32_t>(st.st_mtim.tv_nsec);
  x->stx_ctime.tv_sec = st.st_ctim.tv_sec;
  x->stx_ctime.tv_nsec = static_cast<uint32_t>(st.st_ctim.tv_nsec);
  x->stx_rdev_major = major(st.st_rdev);
  x->stx_rdev_minor = minor(st.st_rdev);
  x->stx_dev_major = major(st.st_dev);
  x->stx_dev_minor = minor(st.st_dev);
  return 0;
}

long sys_faccessat(long dirfd, long path_l, long mode, long, long, long) {
  struct stat st;
  long r = sys_newfstatat(dirfd, path_l, reinterpret_cast<long>(&st), 0, 0, 0);
  if (r < 0) return r;
  if ((mode & W_OK) && !(st.st_mode & 0200) && !S_ISDIR(st.st_mode)) return -EACCES;
  return 0;
}

long sys_fchmod(long fd, long mode, long, long, long, long) {
  return with_file(fd, [&](W32File* f) -> long {
    if (f->path && f->kind == W32Kind::kFile) {
      DWORD a = w32.GetFileAttributesW(f->path);
      if (a != INVALID_FILE_ATTRIBUTES)
        w32.SetFileAttributesW(f->path, (mode & 0222) ? (a & ~FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY));
    }
    return 0;
  });
}

long sys_fchmodat(long dirfd, long path, long mode, long, long, long) {
  WCHAR win[W32_PATH_MAX];
  long r = w32_path(static_cast<int>(dirfd), reinterpret_cast<const char*>(path), win, W32_PATH_MAX);
  if (r < 0) return r;
  DWORD a = w32.GetFileAttributesW(win);
  if (a == INVALID_FILE_ATTRIBUTES) return w32_fail();
  if (!(a & FILE_ATTRIBUTE_DIRECTORY))
    w32.SetFileAttributesW(win, (mode & 0222) ? (a & ~FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY));
  return 0;
}

long sys_fchown(long, long, long, long, long, long) { return 0; }
long sys_fchownat(long, long, long, long, long, long) { return 0; }
long sys_umask(long mask, long, long, long, long, long) {
  static int g_umask = 022;
  int old = g_umask;
  g_umask = static_cast<int>(mask) & 0777;
  return old;
}

long sys_utimensat(long dirfd, long path_l, long times_l, long flags, long, long) {
  auto times = reinterpret_cast<const timespec*>(times_l);
  HANDLE h;
  bool close = true;
  W32File* f = nullptr;
  if (path_l == 0) {
    f = w32_fd_get(static_cast<int>(dirfd));
    if (!f) return -EBADF;
    h = f->handle;
    close = false;
  } else {
    WCHAR win[W32_PATH_MAX];
    long r = w32_path(static_cast<int>(dirfd), reinterpret_cast<const char*>(path_l), win, W32_PATH_MAX);
    if (r < 0) return r;
    h = w32.CreateFileW(win, 0x100 /* FILE_WRITE_ATTRIBUTES */, FILE_SHARE_ALL, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return w32_fail();
  }
  (void)flags;
  int64_t now = w32_realtime_ns();
  W32_FILETIME ft[2];
  const W32_FILETIME* set[2] = {nullptr, nullptr};
  for (int i = 0; i < 2; i++) {
    if (times && times[i].tv_nsec == UTIME_OMIT) continue;
    int64_t ns = (!times || times[i].tv_nsec == UTIME_NOW) ? now : static_cast<int64_t>(times[i].tv_sec) * 1000000000 + times[i].tv_nsec;
    ft[i] = w32_ns_to_filetime(ns);
    set[i] = &ft[i];
  }
  long r = w32.SetFileTime(h, nullptr, set[0], set[1]) ? 0 : w32_fail();
  if (close) w32.CloseHandle(h);
  w32_file_put(f);
  return r;
}

namespace {

constexpr unsigned long kStNosuid = 0x2, kStNodev = 0x4, kStNoexec = 0x8, kStValid = 0x20, kStRelatime = 0x1000;

bool pseudo_fs(const char* abs, unsigned long* type, unsigned long* flags) {
  auto under = [abs](const char* dir) {
    size_t n = strlen(dir);
    return strncmp(abs, dir, n) == 0 && (abs[n] == 0 || abs[n] == '/');
  };
  if (under("/proc")) {
    *type = PROC_SUPER_MAGIC;
    *flags = kStValid | kStNosuid | kStNodev | kStNoexec | kStRelatime;
  } else if (under("/sys")) {
    *type = SYSFS_MAGIC;
    *flags = kStValid | kStNosuid | kStNodev | kStNoexec | kStRelatime;
  } else if (under("/dev/pts")) {
    *type = DEVPTS_SUPER_MAGIC;
    *flags = kStValid | kStNosuid | kStNoexec | kStRelatime;
  } else if (under("/dev")) {
    *type = TMPFS_MAGIC;
    *flags = kStValid | kStNosuid | kStRelatime;
  } else {
    return false;
  }
  return true;
}

bool proc_entry(const char* abs) { return w32_proc_node(abs) != W32ProcNode::kNone; }

}  // namespace

long statfs_impl(long path, long buf) {
  char abs[W32_PATH_MAX];
  long r = absolute(AT_FDCWD, reinterpret_cast<const char*>(path), abs);
  if (r < 0) return r;
  auto s = reinterpret_cast<struct statfs*>(buf);
  unsigned long type = 0, flags = 0;
  if (pseudo_fs(abs, &type, &flags)) {
    if (!proc_entry(abs)) {
      struct stat st;
      r = sys_newfstatat(AT_FDCWD, reinterpret_cast<long>(abs), reinterpret_cast<long>(&st), 0, 0, 0);
      if (r < 0) return r;
    }
    memset(s, 0, sizeof(*s));
    s->f_type = type;
    s->f_bsize = 4096;
    s->f_frsize = 4096;
    s->f_namelen = 255;
    s->f_flags = flags;
    return 0;
  }
  WCHAR win[W32_PATH_MAX];
  r = to_win32(abs, win, W32_PATH_MAX);
  if (r < 0) return r;
  const DWORD attrs = w32.GetFileAttributesW(win);
  if (attrs == INVALID_FILE_ATTRIBUTES) return w32_fail();
  size_t len = 0;
  while (win[len]) ++len;
  if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    while (len > 0 && win[len - 1] != '\\') --len;
    win[len] = 0;
  }
  if (len > 0 && win[len - 1] != '\\' && len + 1 < W32_PATH_MAX) {
    win[len++] = '\\';
    win[len] = 0;
  }
  uint64_t avail = 0, total = 0, free_bytes = 0;
  if (!w32.GetDiskFreeSpaceExW(win, &avail, &total, &free_bytes)) return w32_fail();
  memset(s, 0, sizeof(*s));
  s->f_type = EXT4_SUPER_MAGIC;
  s->f_bsize = 4096;
  s->f_frsize = 4096;
  s->f_blocks = total / 4096;
  s->f_bfree = free_bytes / 4096;
  s->f_bavail = avail / 4096;
  s->f_files = 1 << 24;
  s->f_ffree = 1 << 23;
  s->f_namelen = 255;
  s->f_flags = kStValid | kStRelatime;
  return 0;
}

bool ends_with(const char* text, const char* tail) {
  const size_t n = strlen(text), m = strlen(tail);
  return n >= m && strcmp(text + n - m, tail) == 0;
}

long sys_openat(long dirfd, long path_l, long flags_l, long mode, long, long) {
  const long r = openat_impl(dirfd, path_l, flags_l, mode);
  if (__builtin_expect(w32_log_wanted(), 0)) {
    const char* path = reinterpret_cast<const char*>(path_l);
    if (r < 0 && r != -ENOENT) w32_debug("fs: openat(%s, 0x%lx) = %ld", path ? path : "(null)", flags_l, r);
  }
  return r;
}

long sys_statfs(long path, long buf, long, long, long, long) {
  const long r = statfs_impl(path, buf);
  if (__builtin_expect(w32_log_wanted(), 0) && r < 0)
    w32_debug("fs: statfs(%s) = %ld", reinterpret_cast<const char*>(path), r);
  return r;
}

long sys_fstatfs(long fd, long buf, long, long, long, long) {
  W32File* f = w32_fd_get(static_cast<int>(fd));
  if (!f) return -EBADF;
  char path[W32_PATH_MAX];
  snprintf(path, sizeof path, "%s", f->upath ? f->upath : "/");
  w32_file_put(f);
  return sys_statfs(reinterpret_cast<long>(path), buf, 0, 0, 0, 0);
}

namespace {

struct Listing {
  char* data;
  size_t size;
};

void destroy_listing(W32File* f) {
  Listing* l = static_cast<Listing*>(f->extra);
  if (l) free(l->data);
  free(l);
}

struct linux_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

bool add_record(Listing* l, size_t* cap, const char* name, unsigned char type, uint64_t ino) {
  size_t len = strlen(name);
  size_t reclen = (offsetof(linux_dirent64, d_name) + len + 1 + 7) & ~size_t{7};
  if (l->size + reclen > *cap) {
    size_t want = (*cap + reclen) * 2;
    char* p = static_cast<char*>(realloc(l->data, want));
    if (!p) return false;
    l->data = p;
    *cap = want;
  }
  auto d = reinterpret_cast<linux_dirent64*>(l->data + l->size);
  d->d_ino = ino;
  d->d_off = static_cast<int64_t>(l->size + reclen);
  d->d_reclen = static_cast<unsigned short>(reclen);
  d->d_type = type;
  memcpy(d->d_name, name, len + 1);
  l->size += reclen;
  return true;
}

struct ProcListing {
  Listing* l;
  size_t cap;
  uint64_t ino;
};

Listing* proc_listing(const char* abs) {
  Listing* l = static_cast<Listing*>(calloc(1, sizeof(Listing)));
  if (!l) return nullptr;
  ProcListing p{l, 0, 1};
  if (!w32_proc_list(abs,
                     [](void* ctx, const char* name, unsigned char type) {
                       auto* p = static_cast<ProcListing*>(ctx);
                       add_record(p->l, &p->cap, name, type, p->ino++);
                     },
                     &p)) {
    free(l->data);
    free(l);
    return nullptr;
  }
  return l;
}

Listing* list_directory(const WCHAR* dir) {
  WCHAR pattern[W32_PATH_MAX];
  size_t n = 0;
  while (dir[n] && n < W32_PATH_MAX - 3) {
    pattern[n] = dir[n];
    n++;
  }
  if (n && pattern[n - 1] != '\\') pattern[n++] = '\\';
  pattern[n++] = '*';
  pattern[n] = 0;
  Listing* l = static_cast<Listing*>(calloc(1, sizeof(Listing)));
  if (!l) return nullptr;
  size_t cap = 0;
  W32_WIN32_FIND_DATAW fd;
  HANDLE h = w32.FindFirstFileW(pattern, &fd);
  uint64_t ino = 1;
  if (h != INVALID_HANDLE_VALUE) {
    do {
      WCHAR conv[260];
      size_t k = 0;
      for (; fd.name[k] && k < 259; k++) {
        WCHAR c = fd.name[k];
        conv[k] = (c >= 0xf000 && c < 0xf080 && forbidden(c - 0xf000)) ? static_cast<WCHAR>(c - 0xf000) : c;
      }
      conv[k] = 0;
      char name[1024];
      if (w32_utf16_to_utf8(conv, name, sizeof name) < 0) continue;
      size_t len = strlen(name);
      size_t reclen = (offsetof(linux_dirent64, d_name) + len + 1 + 7) & ~size_t{7};
      if (l->size + reclen > cap) {
        cap = (cap + reclen) * 2;
        char* p = static_cast<char*>(realloc(l->data, cap));
        if (!p) break;
        l->data = p;
      }
      auto d = reinterpret_cast<linux_dirent64*>(l->data + l->size);
      d->d_ino = ino++;
      d->d_off = static_cast<int64_t>(l->size + reclen);
      d->d_reclen = static_cast<unsigned short>(reclen);
      d->d_type = (fd.attributes & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(fd.reserved0) ? DT_LNK
                  : (fd.attributes & FILE_ATTRIBUTE_DIRECTORY)                                 ? DT_DIR
                  : may_be_link_file(fd.attributes)                                            ? DT_UNKNOWN
                                                                                               : DT_REG;
      memcpy(d->d_name, name, len + 1);
      l->size += reclen;
    } while (w32.FindNextFileW(h, &fd));
    w32.FindClose(h);
  }
  return l;
}

}  // namespace

long sys_getdents64(long fd, long buf_l, long count, long, long, long) {
  return with_file(fd, [&](W32File* f) -> long {
    if (f->kind != W32Kind::kDirectory) return -ENOTDIR;
    Lock lock(&f->lock);
    if (!f->extra) {
      f->extra = list_directory(f->path);
      if (!f->extra) return -ENOMEM;
      f->destroy = destroy_listing;
    }
    Listing* l = static_cast<Listing*>(f->extra);
    char* buf = reinterpret_cast<char*>(buf_l);
    size_t out = 0;
    while (f->pos < static_cast<int64_t>(l->size)) {
      auto d = reinterpret_cast<linux_dirent64*>(l->data + f->pos);
      if (out + d->d_reclen > static_cast<size_t>(count)) {
        if (out == 0) return -EINVAL;
        break;
      }
      memcpy(buf + out, d, d->d_reclen);
      out += d->d_reclen;
      f->pos += d->d_reclen;
    }
    return static_cast<long>(out);
  });
}

long sys_mkdirat(long dirfd, long path, long, long, long, long) {
  WCHAR win[W32_PATH_MAX];
  long r = w32_path(static_cast<int>(dirfd), reinterpret_cast<const char*>(path), win, W32_PATH_MAX);
  if (r < 0) return r;
  if (!w32.CreateDirectoryW(win, nullptr)) {
    DWORD err = w32.GetLastError();
    return err == W32_ERROR_ALREADY_EXISTS ? -EEXIST : -w32_errno_from_win32(err);
  }
  return 0;
}

long sys_unlinkat(long dirfd, long path, long flags, long, long, long) {
  WCHAR win[W32_PATH_MAX];
  long r = w32_path(static_cast<int>(dirfd), reinterpret_cast<const char*>(path), win, W32_PATH_MAX);
  if (r < 0) return r;
  DWORD attrs = w32.GetFileAttributesW(win);
  if (attrs == INVALID_FILE_ATTRIBUTES) return w32_fail();
  if (is_link(win, attrs)) {
    if (flags & AT_REMOVEDIR) return -ENOTDIR;
    bool ok = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? w32.RemoveDirectoryW(win) : w32.DeleteFileW(win);
    return ok ? 0 : w32_fail();
  }
  if (flags & AT_REMOVEDIR) {
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return -ENOTDIR;
    return w32.RemoveDirectoryW(win) ? 0 : w32_fail();
  }
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) return -EISDIR;
  if (attrs & FILE_ATTRIBUTE_READONLY) w32.SetFileAttributesW(win, attrs & ~FILE_ATTRIBUTE_READONLY);
  // move it out of the way first: an open file is only deleted when its last handle closes
  char trash[64];
  snprintf(trash, sizeof trash, "/.deleted/%lu-%d", static_cast<unsigned long>(w32.GetCurrentProcessId()),
           __atomic_add_fetch(&g_deleted_counter, 1, __ATOMIC_RELAXED));
  WCHAR trash_w[W32_PATH_MAX];
  if (to_win32(trash, trash_w, W32_PATH_MAX) == 0 && w32.MoveFileExW(win, trash_w, MOVEFILE_REPLACE_EXISTING)) {
    w32.DeleteFileW(trash_w);
    return 0;
  }
  return w32.DeleteFileW(win) ? 0 : w32_fail();
}

long sys_renameat2(long olddir, long oldpath, long newdir, long newpath, long flags, long) {
  WCHAR from[W32_PATH_MAX], to[W32_PATH_MAX];
  long r = w32_path(static_cast<int>(olddir), reinterpret_cast<const char*>(oldpath), from, W32_PATH_MAX);
  if (r == 0) r = w32_path(static_cast<int>(newdir), reinterpret_cast<const char*>(newpath), to, W32_PATH_MAX);
  if (r < 0) return r;
  DWORD fa = w32.GetFileAttributesW(from);
  if (fa == INVALID_FILE_ATTRIBUTES) return w32_fail();
  DWORD ta = w32.GetFileAttributesW(to);
  if ((flags & RENAME_NOREPLACE) && ta != INVALID_FILE_ATTRIBUTES) return -EEXIST;
  if (ta != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY) && (ta & FILE_ATTRIBUTE_DIRECTORY) && !w32.RemoveDirectoryW(to))
    return -ENOTEMPTY;
  // linux happily renames over a read-only file, windows wants the bit off first
  const bool unlock = ta != INVALID_FILE_ATTRIBUTES && (ta & FILE_ATTRIBUTE_READONLY) && !(ta & FILE_ATTRIBUTE_DIRECTORY);
  if (unlock) w32.SetFileAttributesW(to, ta & ~FILE_ATTRIBUTE_READONLY);
  if (!w32.MoveFileExW(from, to, (flags & RENAME_NOREPLACE) ? MOVEFILE_COPY_ALLOWED : (MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))) {
    const long err = w32_fail();
    if (unlock) w32.SetFileAttributesW(to, ta);
    return err;
  }
  return 0;
}

long sys_linkat(long olddir, long oldpath, long newdir, long newpath, long, long) {
  WCHAR from[W32_PATH_MAX], to[W32_PATH_MAX];
  long r = w32_path(static_cast<int>(olddir), reinterpret_cast<const char*>(oldpath), from, W32_PATH_MAX);
  if (r == 0) r = w32_path(static_cast<int>(newdir), reinterpret_cast<const char*>(newpath), to, W32_PATH_MAX);
  if (r < 0) return r;
  return w32.CreateHardLinkW(to, from, nullptr) ? 0 : w32_fail();
}

long sys_symlinkat(long target_l, long newdirfd, long linkpath_l, long, long, long) {
  const char* target = reinterpret_cast<const char*>(target_l);
  if (!target) return -EFAULT;
  if (!target[0]) return -ENOENT;
  char link_abs[W32_PATH_MAX];
  long r = absolute(static_cast<int>(newdirfd), reinterpret_cast<const char*>(linkpath_l), link_abs);
  if (r < 0) return r;
  WCHAR link_w[W32_PATH_MAX];
  r = to_win32(link_abs, link_w, W32_PATH_MAX);
  if (r < 0) return r;
  if (w32.GetFileAttributesW(link_w) != INVALID_FILE_ATTRIBUTES) return -EEXIST;
  char joined[W32_PATH_MAX], target_abs[W32_PATH_MAX];
  if (target[0] == '/') {
    snprintf(joined, sizeof joined, "%s", target);
  } else {
    snprintf(joined, sizeof joined, "%s", link_abs);
    char* slash = strrchr(joined, '/');
    snprintf(slash + 1, sizeof joined - static_cast<size_t>(slash + 1 - joined), "%s", target);
  }
  r = absolute(AT_FDCWD, joined, target_abs);
  WCHAR target_w[W32_PATH_MAX];
  if (r == 0) r = to_win32(target_abs, target_w, W32_PATH_MAX);
  if (r < 0) return r;
  DWORD ta = w32.GetFileAttributesW(target_w);
  bool directory = ta == INVALID_FILE_ATTRIBUTES || (ta & FILE_ATTRIBUTE_DIRECTORY);
  WCHAR text[W32_PATH_MAX];
  if (target[0] == '/') {
    memcpy(text, target_w, sizeof text);
  } else {
    if (w32_utf8_to_utf16(target, text, W32_PATH_MAX) < 0) return -ENAMETOOLONG;
    for (WCHAR* c = text; *c; c++)
      if (*c == '/') *c = '\\';
  }
  if (w32.CreateSymbolicLinkW(link_w, text, directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0)) return 0;
  return directory ? make_junction(link_w, target_w) : make_link_file(link_w, target);
}

long sys_readlinkat(long dirfd, long path_l, long buf_l, long size, long, long) {
  char abs[W32_PATH_MAX];
  long r = absolute(static_cast<int>(dirfd), reinterpret_cast<const char*>(path_l), abs);
  if (r < 0) return r;
  char target[W32_PATH_MAX];
  if (strncmp(abs, "/proc/", 6) == 0 && w32_proc_readlink(abs, target, sizeof target)) {
  } else if (is_dev_or_proc(abs) && w32_proc_node(abs) != W32ProcNode::kNone) {
    return -EINVAL;
  } else if (strncmp(abs, "/proc/", 6) == 0) {
    return -ENOENT;
  } else {
    WCHAR win[W32_PATH_MAX];
    r = to_win32(abs, win, W32_PATH_MAX);
    if (r < 0) return r;
    if (w32.GetFileAttributesW(win) == INVALID_FILE_ATTRIBUTES) return w32_fail();
    r = read_link(win, target, sizeof target);
    if (r < 0) return r;
  }
  size_t n = strlen(target);
  if (n > static_cast<size_t>(size)) n = static_cast<size_t>(size);
  memcpy(reinterpret_cast<char*>(buf_l), target, n);
  return static_cast<long>(n);
}

long sys_getcwd(long buf, long size, long, long, long, long) {
  Lock lock(&g_cwd_lock);
  size_t n = strlen(g_cwd) + 1;
  if (n > static_cast<size_t>(size)) return -ERANGE;
  memcpy(reinterpret_cast<char*>(buf), g_cwd, n);
  return static_cast<long>(n);
}

static long change_dir(const char* abs) {
  WCHAR win[W32_PATH_MAX];
  long r = to_win32(abs, win, W32_PATH_MAX);
  if (r < 0) return r;
  DWORD a = w32.GetFileAttributesW(win);
  if (a == INVALID_FILE_ATTRIBUTES) return w32_fail();
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return -ENOTDIR;
  Lock lock(&g_cwd_lock);
  strcpy(g_cwd, abs);
  return 0;
}

long sys_chdir(long path, long, long, long, long, long) {
  char abs[W32_PATH_MAX];
  long r = absolute(AT_FDCWD, reinterpret_cast<const char*>(path), abs);
  return r < 0 ? r : change_dir(abs);
}

long sys_fchdir(long fd, long, long, long, long, long) {
  W32File* f = w32_fd_get(static_cast<int>(fd));
  if (!f) return -EBADF;
  char abs[W32_PATH_MAX];
  bool dir = f->kind == W32Kind::kDirectory && f->upath;
  if (dir) strcpy(abs, f->upath);
  w32_file_put(f);
  return dir ? change_dir(abs) : -ENOTDIR;
}

long w32_pipe_pair(int* fds, int flags);

long sys_pipe2(long fds_l, long flags, long, long, long, long) {
  return w32_pipe_pair(reinterpret_cast<int*>(fds_l), static_cast<int>(flags));
}

long sys_flock(long fd, long op, long, long, long, long) {
  return with_file(fd, [&](W32File* f) -> long {
    if (f->kind != W32Kind::kFile) return 0;
    W32_OVERLAPPED ov{};
    if ((op & ~LOCK_NB) == LOCK_UN) return w32.UnlockFileEx(f->handle, 0, 0xffffffff, 0xffffffff, &ov) ? 0 : 0;
    DWORD flags = ((op & ~LOCK_NB) == LOCK_EX ? 2 /* EXCLUSIVE */ : 0) | ((op & LOCK_NB) ? 1 /* FAIL_IMMEDIATELY */ : 0);
    if (!w32.LockFileEx(f->handle, flags, 0, 0xffffffff, 0xffffffff, &ov)) return (op & LOCK_NB) ? -EWOULDBLOCK : w32_fail();
    return 0;
  });
}

long w32_socket_ioctl(W32File* f, unsigned long request, void* arg);

long sys_ioctl(long fd, long request, long arg, long, long, long) {
  return with_file(fd, [&](W32File* f) -> long {
    if (f->kind == W32Kind::kSocket) {
      const long r = w32_socket_ioctl(f, static_cast<unsigned long>(request), reinterpret_cast<void*>(arg));
      if (r != 1) return r;
    }
    switch (static_cast<unsigned long>(request)) {
      case FIONBIO: {
        int nb = *reinterpret_cast<int*>(arg);
        f->flags = nb ? (f->flags | O_NONBLOCK) : (f->flags & ~O_NONBLOCK);
        if (f->kind == W32Kind::kSocket) {
          uint32_t v = nb ? 1 : 0;
          w32.ioctlsocket(reinterpret_cast<SOCKET>(f->handle), static_cast<long>(0x8004667e), &v);
        }
        return 0;
      }
      case FIONREAD: {
        long n = w32_readable(f);
        if (n < 0) return n;
        *reinterpret_cast<int*>(arg) = static_cast<int>(n);
        return 0;
      }
      case FIOCLEX:
      case FIONCLEX:
        return 0;
      default:
        return -ENOTTY;
    }
  });
}

long w32_readable(W32File* f) {
  switch (f->kind) {
    case W32Kind::kPipe: {
      DWORD avail = 0;
      if (!w32.PeekNamedPipe(f->handle, nullptr, 0, nullptr, &avail, nullptr)) return 0;
      return static_cast<long>(avail);
    }
    case W32Kind::kFile: {
      int64_t size;
      if (!w32.GetFileSizeEx(f->handle, &size)) return 0;
      return size > f->pos ? static_cast<long>(size - f->pos) : 0;
    }
    case W32Kind::kMemory: {
      MemoryFile* m = static_cast<MemoryFile*>(f->extra);
      return static_cast<int64_t>(m->size) > f->pos ? static_cast<long>(static_cast<int64_t>(m->size) - f->pos) : 0;
    }
    case W32Kind::kSocket: {
      uint32_t n = 0;
      w32.ioctlsocket(reinterpret_cast<SOCKET>(f->handle), 0x4004667f /* FIONREAD */, &n);
      return static_cast<long>(n);
    }
    default:
      return 0;
  }
}

long sys_inotify_init1(long, long, long, long, long, long) { return -ENOSYS; }
long sys_inotify_add_watch(long, long, long, long, long, long) { return -ENOSYS; }
long sys_inotify_rm_watch(long, long, long, long, long, long) { return -EINVAL; }
long sys_memfd_create(long, long, long, long, long, long) { return -ENOSYS; }
long sys_mknodat(long, long, long, long, long, long) { return -EPERM; }
long sys_mount(long, long, long, long, long, long) { return -EPERM; }
long sys_umount2(long, long, long, long, long, long) { return -EPERM; }
long sys_chroot(long, long, long, long, long, long) { return -EPERM; }
long sys_copy_file_range(long, long, long, long, long, long) { return -EXDEV; }
long sys_sendfile(long, long, long, long, long, long) { return -EINVAL; }

long sys_fsetxattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }
long sys_fgetxattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }
long sys_flistxattr(long, long, long, long, long, long) { return 0; }
long sys_fremovexattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }
long sys_setxattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }
long sys_lsetxattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }
long sys_getxattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }
long sys_lgetxattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }
long sys_listxattr(long, long, long, long, long, long) { return 0; }
long sys_llistxattr(long, long, long, long, long, long) { return 0; }
long sys_removexattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }
long sys_lremovexattr(long, long, long, long, long, long) { return -EOPNOTSUPP; }

__attribute__((weak)) long w32_socket_read(W32File*, void*, size_t) { return -ENOTSOCK; }
__attribute__((weak)) long w32_socket_write(W32File*, const void*, size_t) { return -ENOTSOCK; }
__attribute__((weak)) long w32_special_read(W32File*, void*, size_t) { return -EINVAL; }
__attribute__((weak)) long w32_special_write(W32File*, const void*, size_t) { return -EINVAL; }

void w32_each_fd(void (*fn)(void*, int), void* ctx) {
  Lock lock(&g_fd_lock);
  for (int i = 0; i < kMaxFds; i++)
    if (g_fds[i].file) fn(ctx, i);
}
