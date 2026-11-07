// the attribute and file-action functions are bionic's (BSD licence, bionic/libc/NOTICE)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "w32_fd.h"
#include "w32_syscalls.h"

enum Action {
  kOpen,
  kClose,
  kDup2,
  kChdir,
  kFchdir,
};

struct __posix_spawn_file_action {
  __posix_spawn_file_action* next;
  Action what;
  int fd;
  int new_fd;
  char* path;
  int flags;
  mode_t mode;
};

struct __posix_spawn_file_actions {
  __posix_spawn_file_action* head;
  __posix_spawn_file_action* last;
};

struct __posix_spawnattr {
  short flags;
  pid_t pgroup;
  sched_param schedparam;
  int schedpolicy;
  union {
    sigset_t sigset;
    sigset64_t sigset64;
  } sigmask, sigdefault;
};

int posix_spawnattr_init(posix_spawnattr_t* attr) {
  *attr = reinterpret_cast<__posix_spawnattr*>(calloc(1, sizeof(__posix_spawnattr)));
  return (*attr == nullptr) ? errno : 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t* attr) {
  free(*attr);
  *attr = nullptr;
  return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t* attr, short flags) {
  if ((flags & ~(POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                 POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER |
                 POSIX_SPAWN_USEVFORK | POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT)) != 0) {
    return EINVAL;
  }
  (*attr)->flags = flags;
  return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t* attr, short* flags) {
  *flags = (*attr)->flags;
  return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t* attr, pid_t pgroup) {
  (*attr)->pgroup = pgroup;
  return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t* attr, pid_t* pgroup) {
  *pgroup = (*attr)->pgroup;
  return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t* attr, const sigset_t* mask) {
  (*attr)->sigmask.sigset = *mask;
  return 0;
}

int posix_spawnattr_setsigmask64(posix_spawnattr_t* attr, const sigset64_t* mask) {
  (*attr)->sigmask.sigset64 = *mask;
  return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t* attr, sigset_t* mask) {
  *mask = (*attr)->sigmask.sigset;
  return 0;
}

int posix_spawnattr_getsigmask64(const posix_spawnattr_t* attr, sigset64_t* mask) {
  *mask = (*attr)->sigmask.sigset64;
  return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t* attr, const sigset_t* mask) {
  (*attr)->sigdefault.sigset = *mask;
  return 0;
}

int posix_spawnattr_setsigdefault64(posix_spawnattr_t* attr, const sigset64_t* mask) {
  (*attr)->sigdefault.sigset64 = *mask;
  return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t* attr, sigset_t* mask) {
  *mask = (*attr)->sigdefault.sigset;
  return 0;
}

int posix_spawnattr_getsigdefault64(const posix_spawnattr_t* attr, sigset64_t* mask) {
  *mask = (*attr)->sigdefault.sigset64;
  return 0;
}

int posix_spawnattr_setschedparam(posix_spawnattr_t* attr, const struct sched_param* param) {
  (*attr)->schedparam = *param;
  return 0;
}

int posix_spawnattr_getschedparam(const posix_spawnattr_t* attr, struct sched_param* param) {
  *param = (*attr)->schedparam;
  return 0;
}

int posix_spawnattr_setschedpolicy(posix_spawnattr_t* attr, int policy) {
  (*attr)->schedpolicy = policy;
  return 0;
}

int posix_spawnattr_getschedpolicy(const posix_spawnattr_t* attr, int* policy) {
  *policy = (*attr)->schedpolicy;
  return 0;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t* actions) {
  *actions = reinterpret_cast<__posix_spawn_file_actions*>(calloc(1, sizeof(**actions)));
  return (*actions == nullptr) ? errno : 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* actions) {
  __posix_spawn_file_action* a = (*actions)->head;
  while (a) {
    __posix_spawn_file_action* last = a;
    a = a->next;
    free(last->path);
    free(last);
  }
  free(*actions);
  *actions = nullptr;
  return 0;
}

static int posix_spawn_add_file_action(posix_spawn_file_actions_t* actions, Action what, int fd, int new_fd,
                                       const char* path, int flags, mode_t mode) {
  __posix_spawn_file_action* action = reinterpret_cast<__posix_spawn_file_action*>(malloc(sizeof(*action)));
  if (action == nullptr) return errno;
  action->next = nullptr;
  if (what == kOpen || what == kChdir) {
    action->path = strdup(path);
    if (action->path == nullptr) {
      free(action);
      return errno;
    }
  } else {
    action->path = nullptr;
  }
  action->what = what;
  action->fd = fd;
  action->new_fd = new_fd;
  action->flags = flags;
  action->mode = mode;
  if ((*actions)->head == nullptr) {
    (*actions)->head = (*actions)->last = action;
  } else {
    (*actions)->last->next = action;
    (*actions)->last = action;
  }
  return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* actions, int fd, const char* path, int flags,
                                     mode_t mode) {
  if (fd < 0) return EBADF;
  return posix_spawn_add_file_action(actions, kOpen, -1, fd, path, flags, mode);
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* actions, int fd) {
  if (fd < 0) return EBADF;
  return posix_spawn_add_file_action(actions, kClose, fd, -1, nullptr, 0, 0);
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* actions, int fd, int new_fd) {
  if (fd < 0 || new_fd < 0) return EBADF;
  return posix_spawn_add_file_action(actions, kDup2, fd, new_fd, nullptr, 0, 0);
}

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t* actions, const char* path) {
  return posix_spawn_add_file_action(actions, kChdir, -1, -1, path, 0, 0);
}

int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t* actions, int fd) {
  if (fd < 0) return EBADF;
  return posix_spawn_add_file_action(actions, kFchdir, fd, -1, nullptr, 0, 0);
}

namespace {

constexpr DWORD kExtendedStartupInfoPresent = 0x00080000, kCreateUnicodeEnvironment = 0x400;
constexpr DWORD kUseStdHandles = 0x100;
constexpr uintptr_t kHandleListAttribute = 0x20002;
constexpr DWORD kDuplicateSameAccess = 2;

struct StartupInfoEx {
  DWORD cb;
  WCHAR *reserved, *desktop, *title;
  DWORD x, y, x_size, y_size, x_chars, y_chars, fill, flags;
  uint16_t show_window, reserved2_size;
  uint8_t* reserved2;
  HANDLE std_input, std_output, std_error;
  void* attribute_list;
};

struct ProcessInformation {
  HANDLE process, thread;
  DWORD pid, tid;
};

struct Child {
  pid_t pid;
  HANDLE process;
};

Child g_children[256];
W32_SRWLOCK g_children_lock;

struct Lock {
  W32_SRWLOCK* l;
  explicit Lock(W32_SRWLOCK* lock) : l(lock) { w32.AcquireSRWLockExclusive(l); }
  ~Lock() { w32.ReleaseSRWLockExclusive(l); }
};

void append_arg(WCHAR* out, size_t* n, size_t cap, const WCHAR* s) {
  auto put = [&](WCHAR c) {
    if (*n + 1 < cap) out[(*n)++] = c;
  };
  if (*n) put(' ');
  bool quote = !*s;
  for (const WCHAR* p = s; *p; p++) quote |= *p == ' ' || *p == '\t' || *p == '"';
  if (!quote) {
    for (const WCHAR* p = s; *p; p++) put(*p);
    return;
  }
  put('"');
  size_t backslashes = 0;
  for (const WCHAR* p = s;; p++) {
    if (*p == '\\') {
      backslashes++;
      continue;
    }
    if (*p == 0 || *p == '"') {
      for (size_t i = 0; i < backslashes * 2; i++) put('\\');
      if (*p == 0) break;
      put('\\');
      put('"');
    } else {
      for (size_t i = 0; i < backslashes; i++) put('\\');
      put(*p);
    }
    backslashes = 0;
  }
  put('"');
}

void append_utf8_arg(WCHAR* out, size_t* n, size_t cap, const char* s) {
  WCHAR w[W32_PATH_MAX];
  if (w32_utf8_to_utf16(s, w, W32_PATH_MAX) < 0) w[0] = 0;
  append_arg(out, n, cap, w);
}

HANDLE inheritable(W32File* f) {
  HANDLE src = nullptr;
  if (!f) return nullptr;
  switch (f->kind) {
    case W32Kind::kFile:
    case W32Kind::kPipe:
    case W32Kind::kConsole:
    case W32Kind::kSocket:
      src = f->handle;
      break;
    default:
      return nullptr;
  }
  HANDLE dup = nullptr;
  if (!w32.DuplicateHandle(w32.GetCurrentProcess(), src, w32.GetCurrentProcess(), &dup, 0, 1, kDuplicateSameAccess)) return nullptr;
  return dup;
}

// windows programs can't WriteFile into our socket pipes, so they get a real pipe and a thread copies it over
struct OutputRelay {
  HANDLE read;
  W32File* file;
};

void* relay_output(void* param) {
  OutputRelay* relay = static_cast<OutputRelay*>(param);
  char buffer[16384];
  bool sink = true;
  DWORD got = 0;
  while (w32.ReadFile(relay->read, buffer, sizeof buffer, &got, nullptr) && got != 0) {
    for (DWORD done = 0; sink && done < got;) {
      long n = w32_socket_write(relay->file, buffer + done, got - done);
      if (n == -EAGAIN || n == -EINTR) {
        usleep(1000);
      } else if (n <= 0) {
        sink = false;
      } else {
        done += static_cast<DWORD>(n);
      }
    }
  }
  w32.CloseHandle(relay->read);
  w32_file_put(relay->file);
  free(relay);
  return nullptr;
}

HANDLE relayed_output(W32File* f) {
  if (!f || f->kind != W32Kind::kSocket) return nullptr;
  void* sa[3] = {reinterpret_cast<void*>(24), nullptr, reinterpret_cast<void*>(1)};  // SECURITY_ATTRIBUTES, inherit
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  if (!w32.CreatePipe(&read, &write, sa, 0)) return nullptr;
  w32.SetHandleInformation(read, 1 /* HANDLE_FLAG_INHERIT */, 0);
  OutputRelay* relay = static_cast<OutputRelay*>(malloc(sizeof(OutputRelay)));
  pthread_attr_t attr;
  pthread_t thread;
  bool started = false;
  if (relay) {
    relay->read = read;
    relay->file = f;
    __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    started = pthread_create(&thread, &attr, relay_output, relay) == 0;
    pthread_attr_destroy(&attr);
    if (!started) {
      w32_file_put(f);
      free(relay);
    }
  }
  if (!started) {
    w32.CloseHandle(read);
    w32.CloseHandle(write);
    return nullptr;
  }
  return write;
}

bool find_in_path(const char* file, char* const env[], char* out) {
  if (strchr(file, '/')) {
    snprintf(out, W32_PATH_MAX, "%s", file);
    return true;
  }
  const char* path = nullptr;
  for (char* const* e = env ? env : environ; e && *e; e++)
    if (strncmp(*e, "PATH=", 5) == 0) path = *e + 5;
  if (!path) path = "/system/bin";
  char buf[W32_PATH_MAX];
  snprintf(buf, sizeof buf, "%s", path);
  for (char* dir = strtok(buf, ":"); dir; dir = strtok(nullptr, ":")) {
    snprintf(out, W32_PATH_MAX, "%s/%s", dir, file);
    if (access(out, X_OK) == 0) return true;
  }
  return false;
}

bool is_elf(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char magic[4] = {};
  bool elf = read(fd, magic, 4) == 4 && memcmp(magic, "\177ELF", 4) == 0;
  close(fd);
  return elf;
}

WCHAR upper(WCHAR c) { return c >= 'a' && c <= 'z' ? static_cast<WCHAR>(c - 32) : c; }

bool carried(const WCHAR* entry) {
  static const char* const kNames[] = {
      "SYSTEMROOT=", "SYSTEMDRIVE=", "WINDIR=", "COMSPEC=", "PATHEXT=", "TEMP=", "TMP=", "USERPROFILE=", "APPDATA=",
      "LOCALAPPDATA=", "PROGRAMDATA=", "ALLUSERSPROFILE=", "PUBLIC=", "PROGRAMFILES=", "PROGRAMFILES(X86)=",
      "PROGRAMW6432=", "COMMONPROGRAMFILES=", "COMMONPROGRAMFILES(X86)=", "COMMONPROGRAMW6432=", "COMPUTERNAME=",
      "USERNAME=", "USERDOMAIN=", "HOMEDRIVE=", "HOMEPATH=", "OS=", "NUMBER_OF_PROCESSORS=", "PROCESSOR_ARCHITECTURE=",
      "PROCESSOR_IDENTIFIER=", "PROCESSOR_LEVEL=", "PROCESSOR_REVISION=", "BLOX7_ROOT=", "BLOX7_BOOT_LOG=",
      "BLOX7_NO_DIALOGS=", "BLOX7_W32_"};
  for (const char* name : kNames) {
    size_t k = 0;
    while (name[k] && upper(entry[k]) == static_cast<WCHAR>(name[k])) k++;
    if (!name[k]) return true;
  }
  return false;
}

bool has_name(char* const* env, const WCHAR* entry) {
  for (char* const* e = env; e && *e; e++) {
    size_t k = 0;
    while (entry[k] && entry[k] != '=' && (*e)[k] && upper(static_cast<unsigned char>((*e)[k])) == upper(entry[k])) k++;
    if (entry[k] == '=' && (*e)[k] == '=') return true;
  }
  return false;
}

void add_child(pid_t pid, HANDLE process) {
  Lock lock(&g_children_lock);
  for (Child& c : g_children) {
    if (!c.pid) {
      c = Child{pid, process};
      return;
    }
  }
  w32.CloseHandle(process);
}

int start_process(const char* path, char* const argv[], char* const env[], W32File* const std[3], const char* cwd,
                  pid_t* pid_out) {
  if (access(path, F_OK) != 0) return errno;
  WCHAR* cmd = static_cast<WCHAR*>(malloc(32768 * sizeof(WCHAR)));
  if (!cmd) return ENOMEM;
  size_t n = 0;
  WCHAR application[W32_PATH_MAX];
  if (is_elf(path)) {
    char exe[W32_PATH_MAX];
    snprintf(exe, sizeof exe, "%sblox7.exe", w32_boot->exe_dir);
    w32_utf8_to_utf16(exe, application, W32_PATH_MAX);
    append_arg(cmd, &n, 32768, application);
    append_utf8_arg(cmd, &n, 32768, "--w32-run");
    append_utf8_arg(cmd, &n, 32768, path);
  } else {
    long r = w32_path(AT_FDCWD, path, application, W32_PATH_MAX);
    if (r < 0) {
      free(cmd);
      return static_cast<int>(-r);
    }
    size_t len = 0;
    while (application[len]) len++;
    if (len < 4 + 260 && application[0] == '\\' && application[1] == '\\' && application[2] == '?' && application[5] == ':')
      memmove(application, application + 4, (len - 3) * sizeof(WCHAR));
    append_arg(cmd, &n, 32768, application);
  }
  for (int i = 1; argv && argv[i]; i++) append_utf8_arg(cmd, &n, 32768, argv[i]);
  cmd[n] = 0;

  size_t cap = 65536, used = 0;
  WCHAR* block = static_cast<WCHAR*>(malloc(cap * sizeof(WCHAR)));
  auto add_env = [&](const char* entry) {
    WCHAR w[8192];
    int len = w32_utf8_to_utf16(entry, w, 8192);
    if (len < 0 || used + static_cast<size_t>(len) + 2 >= cap) return;
    memcpy(block + used, w, static_cast<size_t>(len + 1) * sizeof(WCHAR));
    used += static_cast<size_t>(len) + 1;
  };
  char* const* child_env = env ? env : environ;
  for (char* const* e = child_env; e && *e; e++)
    if (strncmp(*e, "BLOX7_W32_CWD=", 14) != 0) add_env(*e);
  char cwd_entry[W32_PATH_MAX + 32];
  snprintf(cwd_entry, sizeof cwd_entry, "BLOX7_W32_CWD=%s", cwd);
  add_env(cwd_entry);
  // winsock fails without SystemRoot, so these follow every child
  WCHAR* own = w32.GetEnvironmentStringsW();
  for (WCHAR* p = own; p && *p;) {
    size_t len = 0;
    while (p[len]) len++;
    if (carried(p) && !has_name(child_env, p) && used + len + 2 < cap) {
      memcpy(block + used, p, (len + 1) * sizeof(WCHAR));
      used += len + 1;
    }
    p += len + 1;
  }
  if (own) w32.FreeEnvironmentStringsW(own);
  block[used++] = 0;

  HANDLE handles[3];
  HANDLE nul = INVALID_HANDLE_VALUE;
  int count = 0;
  for (int i = 0; i < 3; i++) {
    handles[i] = i > 0 ? relayed_output(std[i]) : nullptr;
    if (!handles[i]) handles[i] = inheritable(std[i]);
    if (!handles[i]) {
      if (nul == INVALID_HANDLE_VALUE) {
        static const char16_t kNul[] = u"NUL";
        void* sa[3] = {reinterpret_cast<void*>(24), nullptr, reinterpret_cast<void*>(1)};  // SECURITY_ATTRIBUTES, inherit
        nul = w32.CreateFileW(reinterpret_cast<const WCHAR*>(kNul), 0xC0000000, 3, sa, 3, 0, nullptr);
      }
      handles[i] = nul;
    }
  }
  HANDLE list[3];
  for (int i = 0; i < 3; i++) {
    bool seen = false;
    for (int k = 0; k < count; k++) seen |= list[k] == handles[i];
    if (!seen && handles[i] != INVALID_HANDLE_VALUE) list[count++] = handles[i];
  }
  // plain inheritance, win7's PROC_THREAD_ATTRIBUTE_HANDLE_LIST refuses sockets
  (void)list;
  (void)kHandleListAttribute;
  (void)kExtendedStartupInfoPresent;
  StartupInfoEx si{};
  si.cb = sizeof(si) - sizeof(si.attribute_list);
  si.flags = kUseStdHandles;
  si.std_input = handles[0];
  si.std_output = handles[1];
  si.std_error = handles[2];
  ProcessInformation pi{};
  BOOL ok = w32.CreateProcessW(application, cmd, nullptr, nullptr, 1, kCreateUnicodeEnvironment, block, nullptr, &si, &pi);
  DWORD error = w32.GetLastError();
  for (int i = 0; i < 3; i++)
    if (handles[i] != nul && handles[i] != INVALID_HANDLE_VALUE) w32.CloseHandle(handles[i]);
  if (nul != INVALID_HANDLE_VALUE) w32.CloseHandle(nul);
  free(block);
  free(cmd);
  if (!ok) {
    char app8[W32_PATH_MAX];
    w32_utf16_to_utf8(application, app8, sizeof app8);
    w32_debug("posix_spawn: CreateProcess(%s) failed: Windows error %lu", app8, static_cast<unsigned long>(error));
    return w32_errno_from_win32(error);
  }
  w32.CloseHandle(pi.thread);
  add_child(static_cast<pid_t>(pi.pid), pi.process);
  if (pid_out) *pid_out = static_cast<pid_t>(pi.pid);
  return 0;
}

int spawn(pid_t* pid, const char* file, const posix_spawn_file_actions_t* actions, char* const argv[], char* const env[],
          bool search) {
  char path[W32_PATH_MAX];
  if (search ? !find_in_path(file, env, path) : snprintf(path, sizeof path, "%s", file) < 0) return ENOENT;

  W32File* std[3] = {};
  W32File* opened[8] = {};
  int opened_count = 0;
  for (int i = 0; i < 3; i++) {
    int fd_flags = fcntl(i, F_GETFD);
    if (fd_flags >= 0 && !(fd_flags & FD_CLOEXEC)) std[i] = w32_fd_get(i);
  }
  char cwd[W32_PATH_MAX];
  if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
  int result = 0;
  for (__posix_spawn_file_action* a = actions ? (*actions)->head : nullptr; a && !result; a = a->next) {
    switch (a->what) {
      case kOpen: {
        int fd = open(a->path, a->flags | O_CLOEXEC, a->mode);
        if (fd < 0) {
          result = errno;
          break;
        }
        W32File* f = w32_fd_get(fd);
        close(fd);
        if (opened_count < 8) opened[opened_count++] = f;
        if (a->new_fd < 3) {
          w32_file_put(std[a->new_fd]);
          std[a->new_fd] = f;
          __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
        }
        break;
      }
      case kClose:
        if (a->fd < 3) {
          w32_file_put(std[a->fd]);
          std[a->fd] = nullptr;
        }
        break;
      case kDup2:
        if (a->new_fd < 3) {
          W32File* f = a->fd < 3 ? std[a->fd] : w32_fd_get(a->fd);
          if (a->fd < 3 && f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
          if (!f) {
            result = EBADF;
            break;
          }
          w32_file_put(std[a->new_fd]);
          std[a->new_fd] = f;
        }
        break;
      case kChdir:
        if (a->path[0] == '/') snprintf(cwd, sizeof cwd, "%s", a->path);
        else snprintf(cwd + strlen(cwd), sizeof cwd - strlen(cwd), "/%s", a->path);
        break;
      case kFchdir: {
        W32File* f = w32_fd_get(a->fd);
        if (f && f->upath) snprintf(cwd, sizeof cwd, "%s", f->upath);
        w32_file_put(f);
        break;
      }
    }
  }
  if (!result) result = start_process(path, argv, env, std, cwd, pid);
  for (int i = 0; i < 3; i++) w32_file_put(std[i]);
  for (int i = 0; i < opened_count; i++) w32_file_put(opened[i]);
  return result;
}

}  // namespace

int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions, const posix_spawnattr_t*,
                char* const argv[], char* const env[]) {
  return spawn(pid, path, actions, argv, env, false);
}

int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* actions, const posix_spawnattr_t*,
                 char* const argv[], char* const env[]) {
  return spawn(pid, file, actions, argv, env, true);
}

// no swapping a process image on windows: run it as a child and exit with its status
long sys_execve(long path, long argv, long envp, long, long, long) {
  pid_t pid;
  int r = spawn(&pid, reinterpret_cast<const char*>(path), nullptr, reinterpret_cast<char* const*>(argv),
                reinterpret_cast<char* const*>(envp), false);
  if (r) return -r;
  int status = 0;
  waitpid(pid, &status, 0);
  _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

long sys_wait4(long pid_l, long status_l, long options, long, long, long) {
  pid_t want = static_cast<pid_t>(pid_l);
  HANDLE handles[64];
  pid_t pids[64];
  DWORD n = 0;
  {
    Lock lock(&g_children_lock);
    for (Child& c : g_children) {
      if (c.pid && (want <= 0 || c.pid == want) && n < 64) {
        handles[n] = c.process;
        pids[n++] = c.pid;
      }
    }
  }
  if (n == 0) return -ECHILD;
  DWORD w = w32.WaitForMultipleObjects(n, handles, 0, (options & WNOHANG) ? 0 : 0xffffffff);
  if (w == W32_WAIT_TIMEOUT) return 0;
  if (w >= n) return w32_fail();
  DWORD code = 0;
  w32.GetExitCodeProcess(handles[w], &code);
  if (status_l) {
    // exit codes 128+N from the port's default signal action read back as death by signal N
    int status = code > 128 && code < 128 + 65 ? static_cast<int>(code - 128) : static_cast<int>((code & 0xff) << 8);
    *reinterpret_cast<int*>(status_l) = status;
  }
  {
    Lock lock(&g_children_lock);
    for (Child& c : g_children) {
      if (c.pid == pids[w]) {
        w32.CloseHandle(c.process);
        c = Child{};
      }
    }
  }
  return pids[w];
}

long sys_waitid(long, long, long, long, long, long) { return -ENOSYS; }

long w32_kill_child(pid_t pid, int sig) {
  Lock lock(&g_children_lock);
  for (Child& c : g_children) {
    if (c.pid == pid) {
      if (sig == 0) return 0;
      return w32.TerminateProcess(c.process, 128 + static_cast<DWORD>(sig)) ? 0 : w32_fail();
    }
  }
  return -ESRCH;
}
