#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "w32_kernel.h"

W32Api w32;
struct w32boot* w32_boot;

namespace {

struct ApiEntry {
  const WCHAR* module;
  const char* name;
  size_t offset;
};

const ApiEntry kApi[] = {
#define W32_MODULE(dll) {reinterpret_cast<const WCHAR*>(dll), nullptr, 0},
#define W32_FN(ret, name, ...) {nullptr, #name, offsetof(W32Api, name)},
#include "w32_api.inc"
#undef W32_FN
};

int g_log_fd_ready;
HANDLE g_log_file = INVALID_HANDLE_VALUE;

}  // namespace

bool w32_log_wanted() { return g_log_fd_ready >= 0; }

extern "C" long __w32_enosys(const char* name) {
  if (g_log_fd_ready >= 0) w32_debug("syscall %s: not implemented (ENOSYS)", name);
  return -38;
}

static_assert(sizeof(WCHAR) == sizeof(char16_t));

void w32_init_api() {
  void* module = nullptr;
  for (const ApiEntry& e : kApi) {
    if (e.name == nullptr) {
      module = w32_boot->LoadLibraryW(reinterpret_cast<const uint16_t*>(e.module));
      continue;
    }
    void* fn = module ? w32_boot->GetProcAddress(module, e.name) : nullptr;
    if (fn == nullptr) {
      // nothing works yet so a message box it is
      void* user32 = w32_boot->LoadLibraryW(reinterpret_cast<const uint16_t*>(u"user32.dll"));
      auto box = reinterpret_cast<int(WINAPI*)(HANDLE, const char*, const char*, DWORD)>(
          w32_boot->GetProcAddress(user32, "MessageBoxA"));
      if (box) box(nullptr, e.name, "Blox7: missing Windows function", 0x10);
      __builtin_trap();
    }
    *reinterpret_cast<void**>(reinterpret_cast<char*>(&w32) + e.offset) = fn;
  }
}

static const WCHAR* env_match(const WCHAR* entry, const char16_t* key) {
  size_t k = 0;
  for (; key[k]; k++) {
    WCHAR a = entry[k], b = static_cast<WCHAR>(key[k]);
    if (a >= 'a' && a <= 'z') a = static_cast<WCHAR>(a - 32);
    if (b >= 'a' && b <= 'z') b = static_cast<WCHAR>(b - 32);
    if (a != b) return nullptr;
  }
  return entry + k;
}

static void log_write(const char* text, size_t length) {
  if (g_log_fd_ready == 0) {
    g_log_fd_ready = -1;
    WCHAR name[1024];
    WCHAR* env = w32.GetEnvironmentStringsW();
    for (WCHAR* p = env; p && *p; p += [&] { size_t n = 0; while (p[n]) n++; return n + 1; }()) {
      if (const WCHAR* value = env_match(p, u"BLOX7_W32_LOG=")) {
        size_t n = 0;
        while (value[n] && n < 1023) { name[n] = value[n]; n++; }
        name[n] = 0;
        g_log_file = w32.CreateFileW(name, 4 /* FILE_APPEND_DATA */, 3, nullptr, 4 /* OPEN_ALWAYS */, 0x80, nullptr);
        if (g_log_file != INVALID_HANDLE_VALUE) g_log_fd_ready = 1;
        break;
      }
    }
    if (env) w32.FreeEnvironmentStringsW(env);
  }
  if (g_log_fd_ready == 1) {
    DWORD done;
    w32.WriteFile(g_log_file, text, static_cast<DWORD>(length), &done, nullptr);
  }
  if (w32.IsDebuggerPresent()) w32.OutputDebugStringA(text);
}

void w32_debug(const char* format, ...) {
  char buffer[2048];
  va_list ap;
  int prefix = snprintf(buffer, 16, "[%lu] ", static_cast<unsigned long>(w32.GetCurrentProcessId()));
  va_start(ap, format);
  int n = vsnprintf(buffer + prefix, sizeof(buffer) - 1 - prefix, format, ap);
  va_end(ap);
  if (n < 0) return;
  n += prefix;
  size_t length = static_cast<size_t>(n) < sizeof(buffer) - 2 ? static_cast<size_t>(n) : sizeof(buffer) - 2;
  if (length == 0 || buffer[length - 1] != '\n') buffer[length++] = '\n';
  buffer[length] = 0;
  log_write(buffer, length);
}

void w32_fatal(const char* message) {
  log_write("fatal: ", 7);
  log_write(message, strlen(message));
  log_write("\n", 1);
  HANDLE err = w32.GetStdHandle(0xfffffff4);
  DWORD done;
  if (err && err != INVALID_HANDLE_VALUE) {
    w32.WriteFile(err, "blox7: ", 7, &done, nullptr);
    w32.WriteFile(err, message, static_cast<DWORD>(strlen(message)), &done, nullptr);
    w32.WriteFile(err, "\n", 1, &done, nullptr);
  }
  bool dialogs = true;
  WCHAR* env = w32.GetEnvironmentStringsW();
  for (WCHAR* p = env; p && *p;) {
    if (env_match(p, u"BLOX7_NO_DIALOGS=")) dialogs = false;
    while (*p) p++;
    p++;
  }
  if (env) w32.FreeEnvironmentStringsW(env);
  if (dialogs) {
    WCHAR text[1024];
    if (w32_utf8_to_utf16(message, text, 1024) < 0) text[0] = 0;
    w32.MessageBoxW(nullptr, text, reinterpret_cast<const WCHAR*>(u"Blox7"), 0x10);
  }
  w32.TerminateProcess(w32.GetCurrentProcess(), 127);  // no dll cleanup, nobody gets to hang us on the way out
  __builtin_trap();
}

int w32_utf8_to_utf16(const char* in, WCHAR* out, size_t out_units) {
  const unsigned char* s = reinterpret_cast<const unsigned char*>(in);
  size_t n = 0;
  while (*s) {
    uint32_t c = *s++;
    if (c >= 0x80) {
      int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
      uint32_t min = extra == 3 ? 0x10000 : extra == 2 ? 0x800 : 0x80;
      c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : 0x1f;
      for (int i = 0; i < extra; i++) {
        if ((*s & 0xc0) != 0x80) { extra = -1; break; }
        c = (c << 6) | (*s++ & 0x3f);
      }
      if (extra < 0 || c < min || c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
    }
    if (c >= 0x10000) {
      if (n + 2 >= out_units) return -1;
      c -= 0x10000;
      out[n++] = static_cast<WCHAR>(0xd800 + (c >> 10));
      out[n++] = static_cast<WCHAR>(0xdc00 + (c & 0x3ff));
    } else {
      if (n + 1 >= out_units) return -1;
      out[n++] = static_cast<WCHAR>(c);
    }
  }
  if (n >= out_units) return -1;
  out[n] = 0;
  return static_cast<int>(n);
}

int w32_utf16_to_utf8(const WCHAR* in, char* out, size_t out_bytes) {
  size_t n = 0;
  while (*in) {
    uint32_t c = *in++;
    if (c >= 0xd800 && c < 0xdc00 && *in >= 0xdc00 && *in < 0xe000) {
      c = 0x10000 + ((c - 0xd800) << 10) + (*in++ - 0xdc00);
    } else if (c >= 0xd800 && c < 0xe000) {
      c = 0xfffd;
    }
    char buf[4];
    size_t len;
    if (c < 0x80) { buf[0] = static_cast<char>(c); len = 1; }
    else if (c < 0x800) { buf[0] = static_cast<char>(0xc0 | (c >> 6)); buf[1] = static_cast<char>(0x80 | (c & 0x3f)); len = 2; }
    else if (c < 0x10000) {
      buf[0] = static_cast<char>(0xe0 | (c >> 12)); buf[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      buf[2] = static_cast<char>(0x80 | (c & 0x3f)); len = 3;
    } else {
      buf[0] = static_cast<char>(0xf0 | (c >> 18)); buf[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3f));
      buf[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3f)); buf[3] = static_cast<char>(0x80 | (c & 0x3f)); len = 4;
    }
    if (n + len >= out_bytes) return -1;
    memcpy(out + n, buf, len);
    n += len;
  }
  if (n >= out_bytes) return -1;
  out[n] = 0;
  return static_cast<int>(n);
}

int w32_errno_from_win32(DWORD error) {
  switch (error) {
    case W32_ERROR_SUCCESS: return 0;
    case W32_ERROR_FILE_NOT_FOUND: case W32_ERROR_PATH_NOT_FOUND: case W32_ERROR_INVALID_NAME:
    case 53 /* BAD_NETPATH */: case 161 /* BAD_PATHNAME */: return ENOENT;
    case W32_ERROR_ACCESS_DENIED: case 1314 /* PRIVILEGE_NOT_HELD */: return EACCES;
    case W32_ERROR_SHARING_VIOLATION: case 33 /* LOCK_VIOLATION */: return EBUSY;
    case W32_ERROR_INVALID_HANDLE: return EBADF;
    case W32_ERROR_NOT_ENOUGH_MEMORY: case 14 /* OUTOFMEMORY */: case 1455 /* COMMITMENT_LIMIT */: return ENOMEM;
    case W32_ERROR_FILE_EXISTS: case W32_ERROR_ALREADY_EXISTS: return EEXIST;
    case W32_ERROR_INVALID_PARAMETER: return EINVAL;
    case W32_ERROR_BROKEN_PIPE: case W32_ERROR_NO_DATA: return EPIPE;
    case W32_ERROR_DISK_FULL: case 39 /* HANDLE_DISK_FULL */: return ENOSPC;
    case W32_ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case W32_ERROR_DIRECTORY: return ENOTDIR;
    case 206 /* FILENAME_EXCED_RANGE */: return ENAMETOOLONG;
    case 17 /* NOT_SAME_DEVICE */: return EXDEV;
    case 4390 /* NOT_A_REPARSE_POINT */: return EINVAL;
    case W32_ERROR_TIMEOUT: case W32_WAIT_TIMEOUT: return ETIMEDOUT;
    case 50 /* NOT_SUPPORTED */: case 120 /* CALL_NOT_IMPLEMENTED */: return ENOSYS;
    default: return EIO;
  }
}

int w32_errno_from_wsa(int error) {
  switch (error) {
    case 0: return 0;
    case 10004: return EINTR;
    case 10009: return EBADF;
    case 10013: return EACCES;
    case 10014: return EFAULT;
    case 10022: return EINVAL;
    case 10024: return EMFILE;
    case 10035: return EAGAIN;        // WSAEWOULDBLOCK
    case 10036: return EINPROGRESS;
    case 10037: return EALREADY;
    case 10038: return ENOTSOCK;
    case 10039: return EDESTADDRREQ;
    case 10040: return EMSGSIZE;
    case 10041: return EPROTOTYPE;
    case 10042: return ENOPROTOOPT;
    case 10043: return EPROTONOSUPPORT;
    case 10044: return ESOCKTNOSUPPORT;
    case 10045: return EOPNOTSUPP;
    case 10046: return EPFNOSUPPORT;
    case 10047: return EAFNOSUPPORT;
    case 10048: return EADDRINUSE;
    case 10049: return EADDRNOTAVAIL;
    case 10050: return ENETDOWN;
    case 10051: return ENETUNREACH;
    case 10052: return ENETRESET;
    case 10053: return ECONNABORTED;
    case 10054: return ECONNRESET;
    case 10055: return ENOBUFS;
    case 10056: return EISCONN;
    case 10057: return ENOTCONN;
    case 10058: return ESHUTDOWN;
    case 10060: return ETIMEDOUT;
    case 10061: return ECONNREFUSED;
    case 10063: return ENAMETOOLONG;
    case 10064: return EHOSTDOWN;
    case 10065: return EHOSTUNREACH;
    default: return EIO;
  }
}

static constexpr int64_t kEpochDelta100ns = 116444736000000000LL;

int64_t w32_filetime_to_ns(W32_FILETIME ft) {
  int64_t t = (static_cast<int64_t>(ft.hi) << 32) | ft.lo;
  return (t - kEpochDelta100ns) * 100;
}

W32_FILETIME w32_ns_to_filetime(int64_t ns) {
  uint64_t t = static_cast<uint64_t>(ns / 100 + kEpochDelta100ns);
  return W32_FILETIME{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
}
