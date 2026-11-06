// poll, select, epoll, eventfd, timerfd: everything waitable is a socket underneath, one select() covers it all
// epoll reports edge-triggered as level-triggered, callers that read until EAGAIN can't tell

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <time.h>

#include "w32_fd.h"
#include "w32_syscalls.h"

long w32_pipe_pair(int* fds, int flags);

namespace {

constexpr int64_t kNever = INT64_MAX;

struct WinTimeval {
  int32_t sec, usec;
};

struct WinFdSet {
  uint32_t count;
  SOCKET fd[1];
};

class WaitSets {
 public:
  WaitSets() = default;
  WaitSets(const WaitSets&) = delete;
  WaitSets& operator=(const WaitSets&) = delete;
  ~WaitSets() { free(heap_); }
  bool init(size_t cap) {
    const size_t one = sizeof(uint64_t) + cap * sizeof(SOCKET);
    char* base = inline_;
    if (cap > kInline) {
      heap_ = static_cast<char*>(malloc(3 * one));
      if (!heap_) return false;
      base = heap_;
    }
    rd = reinterpret_cast<WinFdSet*>(base);
    wr = reinterpret_cast<WinFdSet*>(base + one);
    ex = reinterpret_cast<WinFdSet*>(base + 2 * one);
    return true;
  }
  WinFdSet *rd = nullptr, *wr = nullptr, *ex = nullptr;

 private:
  static constexpr size_t kInline = 32;
  alignas(8) char inline_[3 * (sizeof(uint64_t) + kInline * sizeof(SOCKET))];
  char* heap_ = nullptr;
};

bool in_set(const WinFdSet* s, SOCKET x) {
  for (uint32_t i = 0; i < s->count; i++)
    if (s->fd[i] == x) return true;
  return false;
}

WinTimeval to_timeval(int64_t ns) {
  const int64_t us = (ns + 999) / 1000;
  if (us >= 0x7fffffffLL * 1000000) return WinTimeval{0x7fffffff, 0};
  return WinTimeval{static_cast<int32_t>(us / 1000000), static_cast<int32_t>(us % 1000000)};
}

bool wait_readable(SOCKET s, int64_t ns) {
  struct { uint32_t count; SOCKET fd[1]; } set{1, {s}};
  WinTimeval tv = to_timeval(ns);
  return w32.select(0, &set, nullptr, nullptr, ns < 0 ? nullptr : &tv) > 0;
}

struct Lock {
  W32_SRWLOCK* l;
  explicit Lock(W32_SRWLOCK* lock) : l(lock) { w32.AcquireSRWLockExclusive(l); }
  ~Lock() { w32.ReleaseSRWLockExclusive(l); }
};

long internal_pair(SOCKET* r, SOCKET* w) {
  int pair[2];
  long res = sys_socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, reinterpret_cast<long>(pair), 0, 0);
  if (res < 0) return res;
  for (int i = 0; i < 2; i++) {
    W32File* s = w32_fd_get(pair[i]);
    (i == 0 ? *r : *w) = reinterpret_cast<SOCKET>(s->handle);
    s->handle = reinterpret_cast<HANDLE>(INVALID_SOCKET);
    w32_file_put(s);
    sys_close(pair[i], 0, 0, 0, 0, 0);
  }
  return 0;
}

struct Counter {
  W32_SRWLOCK lock;
  uint64_t count;
  bool semaphore;
  bool byte_queued;
  SOCKET r, w;
};

void counter_add(Counter* c, uint64_t v) {
  Lock lock(&c->lock);
  c->count += v;
  if (c->count && !c->byte_queued) {
    char b = 1;
    w32.send(c->w, &b, 1, 0);
    c->byte_queued = true;
  }
}

uint64_t counter_take(Counter* c) {
  Lock lock(&c->lock);
  if (c->count == 0) return 0;
  uint64_t v = c->semaphore ? 1 : c->count;
  c->count -= v;
  if (c->count == 0 && c->byte_queued) {
    char b;
    w32.recv(c->r, &b, 1, 0);
    c->byte_queued = false;
  }
  return v;
}

void destroy_counter(W32File* f) {
  Counter* c = static_cast<Counter*>(f->extra);
  if (!c) return;
  w32.closesocket(c->r);
  w32.closesocket(c->w);
  free(c);
}

struct Timer {
  W32_SRWLOCK lock;
  int clock;
  int64_t next_ns;
  int64_t interval_ns;
  SOCKET r, w;
  int waiters;
  bool kicked;
};

int64_t clock_now(int clock) { return clock == CLOCK_REALTIME ? w32_realtime_ns() : w32_monotonic_ns(); }

Timer* timer_of(W32File* f) { return static_cast<Timer*>(f->extra); }

int64_t timer_due(Timer* t) {
  Lock lock(&t->lock);
  return t->next_ns ? t->next_ns - clock_now(t->clock) : kNever;
}

uint64_t timer_take(Timer* t) {
  Lock lock(&t->lock);
  if (!t->next_ns) return 0;
  const int64_t now = clock_now(t->clock);
  if (now < t->next_ns) return 0;
  uint64_t n = 1;
  if (t->interval_ns > 0) {
    n += static_cast<uint64_t>((now - t->next_ns) / t->interval_ns);
    t->next_ns += static_cast<int64_t>(n) * t->interval_ns;
  } else {
    t->next_ns = 0;
  }
  return n;
}

bool timer_wait_begin(Timer* t) {
  Lock lock(&t->lock);
  return ++t->waiters > 1;
}

void timer_drain(Timer* t) {
  Lock lock(&t->lock);
  if (t->kicked) {
    char b[16];
    w32.recv(t->r, b, sizeof b, 0);
    t->kicked = false;
  }
}

void timer_wait_end(Timer* t) {
  Lock lock(&t->lock);
  if (--t->waiters == 0 && t->kicked) {
    char b[16];
    w32.recv(t->r, b, sizeof b, 0);
    t->kicked = false;
  }
}

void destroy_timer(W32File* f) {
  Timer* t = timer_of(f);
  if (!t) return;
  w32.closesocket(t->r);
  w32.closesocket(t->w);
  free(t);
}

struct Item {
  W32File* f;
  short events;
  short revents;
};

bool check_other(Item& it) {
  W32File* f = it.f;
  switch (f->kind) {
    case W32Kind::kFile:
    case W32Kind::kNull:
    case W32Kind::kRandom:
    case W32Kind::kMemory:
    case W32Kind::kDirectory:
      it.revents |= it.events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
      return false;
    case W32Kind::kPipe: {
      if (it.events & (POLLOUT | POLLWRNORM)) it.revents |= it.events & (POLLOUT | POLLWRNORM);
      if ((f->flags & O_ACCMODE) != O_WRONLY) {
        DWORD avail = 0;
        if (!w32.PeekNamedPipe(f->handle, nullptr, 0, nullptr, &avail, nullptr)) it.revents |= POLLHUP;
        else if (avail) it.revents |= it.events & (POLLIN | POLLRDNORM);
      }
      return (it.events & (POLLIN | POLLRDNORM)) && !it.revents;
    }
    case W32Kind::kConsole:
      it.revents |= it.events & (POLLOUT | POLLWRNORM);
      return false;
    default:
      return false;
  }
}

bool wants_read(short events) { return events & (POLLIN | POLLRDNORM); }

long wait_items(Item* items, size_t n, int64_t deadline, SOCKET extra_wake = INVALID_SOCKET, bool* woken = nullptr) {
  WaitSets sets;
  if (!sets.init(n + 1)) return -ENOMEM;
  bool timers = false, shared_timer = false;
  for (size_t i = 0; i < n; i++) {
    if (items[i].f && items[i].f->kind == W32Kind::kTimerFd && items[i].events >= 0) {
      shared_timer |= timer_wait_begin(timer_of(items[i].f));
      timers = true;
    }
  }
  WinFdSet *rd = sets.rd, *wr = sets.wr, *ex = sets.ex;
  long ready = 0;
  for (;;) {
    rd->count = wr->count = ex->count = 0;
    bool repeat = false, any = false;
    const int64_t start = w32_monotonic_ns();
    int64_t wake_at = deadline;
    for (size_t i = 0; i < n; i++) {
      Item& it = items[i];
      it.revents = 0;
      if (!it.f) {
        if (it.events >= 0) it.revents = POLLNVAL;
        any |= it.revents != 0;
        continue;
      }
      switch (it.f->kind) {
        case W32Kind::kSocket: {
          const SOCKET s = reinterpret_cast<SOCKET>(it.f->handle);
          if ((it.revents = w32_socket_listener_shut(it.f, it.events)) != 0) break;
          it.revents = w32_socket_error(it.f);
          if (it.events & (POLLIN | POLLRDNORM | POLLPRI | POLLRDHUP)) rd->fd[rd->count++] = s;
          if (it.events & (POLLOUT | POLLWRNORM)) wr->fd[wr->count++] = s;
          ex->fd[ex->count++] = s;
          break;
        }
        case W32Kind::kEventFd:
          if (wants_read(it.events)) rd->fd[rd->count++] = static_cast<Counter*>(it.f->extra)->r;
          it.revents |= it.events & (POLLOUT | POLLWRNORM);
          break;
        case W32Kind::kTimerFd: {
          if (!wants_read(it.events)) break;
          Timer* t = timer_of(it.f);
          const int64_t due = timer_due(t);
          if (due <= 0) {
            it.revents |= it.events & (POLLIN | POLLRDNORM);
            break;
          }
          if (due != kNever && (wake_at < 0 || start + due < wake_at)) wake_at = start + due;
          rd->fd[rd->count++] = t->r;
          break;
        }
        default:
          repeat |= check_other(it);
      }
      any |= it.revents != 0;
    }
    if (extra_wake != INVALID_SOCKET) rd->fd[rd->count++] = extra_wake;

    int64_t wait = any ? 0 : wake_at < 0 ? -1 : wake_at > start ? wake_at - start : 0;
    const int64_t cap = repeat ? 10000000 : shared_timer ? 50000000 : -1;
    if (cap >= 0 && (wait < 0 || wait > cap)) wait = cap;
    int got = 0;
    if (rd->count || wr->count || ex->count) {
      WinTimeval tv = to_timeval(wait);
      got = w32.select(0, rd->count ? rd : nullptr, wr->count ? wr : nullptr, ex->count ? ex : nullptr, wait < 0 ? nullptr : &tv);
      if (got < 0) {
        ready = -w32_errno_from_wsa(w32.WSAGetLastError());
        break;
      }
    } else if (wait != 0) {
      w32.Sleep(wait < 0 ? 0xfffffffe : static_cast<DWORD>((wait + 999999) / 1000000));
    }
    w32_fs_rearm();
    if (got > 0) {
      for (size_t i = 0; i < n; i++) {
        Item& it = items[i];
        if (!it.f) continue;
        switch (it.f->kind) {
          case W32Kind::kSocket: {
            const SOCKET s = reinterpret_cast<SOCKET>(it.f->handle);
            if (in_set(rd, s)) it.revents |= w32_socket_readable(it.f, it.events);
            if (in_set(wr, s)) it.revents |= it.events & (POLLOUT | POLLWRNORM);
            if (in_set(ex, s)) it.revents |= POLLERR | (it.events & (POLLOUT | POLLWRNORM));
            break;
          }
          case W32Kind::kEventFd:
            if (wants_read(it.events) && in_set(rd, static_cast<Counter*>(it.f->extra)->r)) it.revents |= it.events & (POLLIN | POLLRDNORM);
            break;
          case W32Kind::kTimerFd:
            if (wants_read(it.events) && in_set(rd, timer_of(it.f)->r)) timer_drain(timer_of(it.f));
            break;
          default:
            break;
        }
      }
      if (woken && extra_wake != INVALID_SOCKET && in_set(rd, extra_wake)) *woken = true;
    }
    if (timers) {
      for (size_t i = 0; i < n; i++) {
        Item& it = items[i];
        if (it.f && it.f->kind == W32Kind::kTimerFd && wants_read(it.events) && !(it.revents & POLLIN) &&
            timer_due(timer_of(it.f)) <= 0) {
          it.revents |= it.events & (POLLIN | POLLRDNORM);
        }
      }
    }
    ready = 0;
    for (size_t i = 0; i < n; i++) ready += items[i].revents != 0;
    if (ready || (woken && *woken)) break;
    if (deadline >= 0 && w32_monotonic_ns() >= deadline) break;
  }
  if (timers) {
    for (size_t i = 0; i < n; i++)
      if (items[i].f && items[i].f->kind == W32Kind::kTimerFd && items[i].events >= 0) timer_wait_end(timer_of(items[i].f));
  }
  return ready;
}

int64_t deadline_after(int64_t ns) {
  if (ns < 0) return -1;
  const int64_t now = w32_monotonic_ns();
  return ns > kNever - now ? kNever : now + ns;
}

int64_t timespec_ns(const timespec* ts) {
  if (ts->tv_sec < 0 || ts->tv_nsec < 0) return 0;
  if (ts->tv_sec >= kNever / 1000000000 - 1) return kNever / 2;
  return static_cast<int64_t>(ts->tv_sec) * 1000000000 + ts->tv_nsec;
}

int64_t remaining(int64_t deadline) {
  const int64_t left = deadline - w32_monotonic_ns();
  return left > 0 ? left : 0;
}

}  // namespace

long sys_ppoll(long fds_l, long nfds, long timeout_l, long, long, long) {
  auto* fds = reinterpret_cast<pollfd*>(fds_l);
  auto* ts = reinterpret_cast<timespec*>(timeout_l);
  if (nfds < 0 || nfds > 1024 * 1024) return -EINVAL;
  if (ts && (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)) return -EINVAL;
  const int64_t deadline = ts ? deadline_after(timespec_ns(ts)) : -1;
  Item stack_items[64];
  Item* items = nfds <= 64 ? stack_items : static_cast<Item*>(malloc(static_cast<size_t>(nfds) * sizeof(Item)));
  if (!items) return -ENOMEM;
  for (long i = 0; i < nfds; i++) {
    items[i].f = fds[i].fd >= 0 ? w32_fd_get(fds[i].fd) : nullptr;
    items[i].events = fds[i].fd >= 0 ? fds[i].events : -1;
    items[i].revents = 0;
  }
  long r = wait_items(items, static_cast<size_t>(nfds), deadline);
  for (long i = 0; i < nfds; i++) {
    fds[i].revents = items[i].events < 0 ? 0 : items[i].revents;
    w32_file_put(items[i].f);
  }
  if (items != stack_items) free(items);
  if (ts && r >= 0) {
    const int64_t left = remaining(deadline);
    ts->tv_sec = static_cast<time_t>(left / 1000000000);
    ts->tv_nsec = static_cast<long>(left % 1000000000);
  }
  return r;
}

long sys_poll(long fds, long nfds, long timeout_ms, long, long, long) {
  const int timeout = static_cast<int>(timeout_ms);
  timespec ts{static_cast<time_t>(timeout / 1000), static_cast<long>(timeout % 1000) * 1000000};
  return sys_ppoll(fds, nfds, timeout < 0 ? 0 : reinterpret_cast<long>(&ts), 0, 0, 0);
}

static long select_ns(long nfds, fd_set* rd, fd_set* wr, fd_set* ex, int64_t deadline) {
  if (nfds < 0 || nfds > FD_SETSIZE) return -EINVAL;
  Item items[FD_SETSIZE];
  int map[FD_SETSIZE];
  size_t n = 0;
  for (int fd = 0; fd < nfds; fd++) {
    short ev = 0;
    if (rd && FD_ISSET(fd, rd)) ev |= POLLIN;
    if (wr && FD_ISSET(fd, wr)) ev |= POLLOUT;
    if (ex && FD_ISSET(fd, ex)) ev |= POLLPRI;
    if (!ev) continue;
    W32File* f = w32_fd_get(fd);
    if (!f) {
      for (size_t i = 0; i < n; i++) w32_file_put(items[i].f);
      return -EBADF;
    }
    items[n] = Item{f, ev, 0};
    map[n++] = fd;
  }
  long r = wait_items(items, n, deadline);
  if (rd) FD_ZERO(rd);
  if (wr) FD_ZERO(wr);
  if (ex) FD_ZERO(ex);
  long count = 0;
  for (size_t i = 0; i < n; i++) {
    short re = items[i].revents;
    if (rd && (re & (POLLIN | POLLHUP | POLLERR)) && (items[i].events & POLLIN)) { FD_SET(map[i], rd); count++; }
    if (wr && (re & (POLLOUT | POLLERR)) && (items[i].events & POLLOUT)) { FD_SET(map[i], wr); count++; }
    if (ex && (re & POLLPRI) && (items[i].events & POLLPRI)) { FD_SET(map[i], ex); count++; }
    w32_file_put(items[i].f);
  }
  return r < 0 ? r : count;
}

long sys_pselect6(long nfds, long rd_l, long wr_l, long ex_l, long timeout_l, long) {
  auto* ts = reinterpret_cast<timespec*>(timeout_l);
  if (ts && (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)) return -EINVAL;
  const int64_t deadline = ts ? deadline_after(timespec_ns(ts)) : -1;
  long r = select_ns(nfds, reinterpret_cast<fd_set*>(rd_l), reinterpret_cast<fd_set*>(wr_l), reinterpret_cast<fd_set*>(ex_l), deadline);
  if (ts && r >= 0) {
    const int64_t left = remaining(deadline);
    ts->tv_sec = static_cast<time_t>(left / 1000000000);
    ts->tv_nsec = static_cast<long>(left % 1000000000);
  }
  return r;
}

long sys_select(long nfds, long rd, long wr, long ex, long tv_l, long) {
  auto* tv = reinterpret_cast<timeval*>(tv_l);
  if (tv && (tv->tv_sec < 0 || tv->tv_usec < 0)) return -EINVAL;
  const int64_t deadline = tv ? deadline_after(static_cast<int64_t>(tv->tv_sec) * 1000000000 + static_cast<int64_t>(tv->tv_usec) * 1000) : -1;
  long r = select_ns(nfds, reinterpret_cast<fd_set*>(rd), reinterpret_cast<fd_set*>(wr), reinterpret_cast<fd_set*>(ex), deadline);
  if (tv && r >= 0) {
    const int64_t left = remaining(deadline);
    tv->tv_sec = static_cast<time_t>(left / 1000000000);
    tv->tv_usec = static_cast<suseconds_t>(left % 1000000000 / 1000);
  }
  return r;
}

namespace {

struct Entry {
  int fd;
  W32File* file;
  uint32_t events;
  epoll_data_t data;
  bool disabled;
};

struct Epoll {
  W32_SRWLOCK lock;
  Entry* entries;
  size_t count, cap;
  SOCKET wake_r, wake_w;
  volatile int waiters;
};

void destroy_epoll(W32File* f) {
  Epoll* e = static_cast<Epoll*>(f->extra);
  if (!e) return;
  free(e->entries);
  w32.closesocket(e->wake_r);
  w32.closesocket(e->wake_w);
  free(e);
}

void wake(Epoll* e) {
  if (__atomic_load_n(&e->waiters, __ATOMIC_ACQUIRE)) {
    char b = 1;
    w32.send(e->wake_w, &b, 1, 0);
  }
}

}  // namespace

long sys_epoll_create1(long flags, long, long, long, long, long) {
  if (flags & ~EPOLL_CLOEXEC) return -EINVAL;
  W32File* f = w32_file_new(W32Kind::kEpoll, nullptr, O_RDWR);
  Epoll* e = static_cast<Epoll*>(calloc(1, sizeof(Epoll)));
  if (!f || !e || internal_pair(&e->wake_r, &e->wake_w) < 0) {
    free(f);
    free(e);
    return -ENOMEM;
  }
  uint32_t nb = 1;
  w32.ioctlsocket(e->wake_r, static_cast<long>(0x8004667e), &nb);
  f->extra = e;
  f->destroy = destroy_epoll;
  int fd = w32_fd_install(f, flags & EPOLL_CLOEXEC);
  w32_file_put(f);
  return fd;
}

long sys_epoll_create(long size, long, long, long, long, long) {
  return size <= 0 ? -EINVAL : sys_epoll_create1(0, 0, 0, 0, 0, 0);
}

long sys_epoll_ctl(long epfd, long op, long fd_l, long event_l, long, long) {
  int fd = static_cast<int>(fd_l);
  W32File* ef = w32_fd_get(static_cast<int>(epfd));
  if (!ef) return -EBADF;
  if (ef->kind != W32Kind::kEpoll || epfd == fd_l) {
    w32_file_put(ef);
    return -EINVAL;
  }
  W32File* target = w32_fd_get(fd);
  if (!target) {
    w32_file_put(ef);
    return -EBADF;
  }
  Epoll* e = static_cast<Epoll*>(ef->extra);
  auto* ev = reinterpret_cast<const epoll_event*>(event_l);
  long r = 0;
  {
    Lock lock(&e->lock);
    size_t i = 0;
    while (i < e->count && !(e->entries[i].fd == fd && e->entries[i].file == target)) i++;
    bool found = i < e->count;
    switch (op) {
      case EPOLL_CTL_ADD:
        if (found) {
          r = -EEXIST;
          break;
        }
        if (e->count == e->cap) {
          size_t cap = e->cap ? e->cap * 2 : 16;
          Entry* p = static_cast<Entry*>(realloc(e->entries, cap * sizeof(Entry)));
          if (!p) {
            r = -ENOMEM;
            break;
          }
          e->entries = p;
          e->cap = cap;
        }
        e->entries[e->count++] = Entry{fd, target, ev->events, ev->data, false};
        break;
      case EPOLL_CTL_MOD:
        if (!found) {
          r = -ENOENT;
          break;
        }
        e->entries[i].events = ev->events;
        e->entries[i].data = ev->data;
        e->entries[i].disabled = false;
        break;
      case EPOLL_CTL_DEL:
        if (!found) {
          r = -ENOENT;
          break;
        }
        e->entries[i] = e->entries[--e->count];
        break;
      default:
        r = -EINVAL;
    }
  }
  if (r == 0) wake(e);
  w32_file_put(target);
  w32_file_put(ef);
  return r;
}

static long epoll_wait_until(long epfd, epoll_event* out, int max, int64_t deadline) {
  if (max <= 0) return -EINVAL;
  W32File* ef = w32_fd_get(static_cast<int>(epfd));
  if (!ef) return -EBADF;
  if (ef->kind != W32Kind::kEpoll) {
    w32_file_put(ef);
    return -EINVAL;
  }
  Epoll* e = static_cast<Epoll*>(ef->extra);
  long result = 0;
  constexpr size_t kInline = 32;
  Item inline_items[kInline];
  Entry inline_snap[kInline];
  __atomic_add_fetch(&e->waiters, 1, __ATOMIC_ACQ_REL);
  for (;;) {
    size_t n;
    Item* items = inline_items;
    Entry* snap = inline_snap;
    {
      Lock lock(&e->lock);
      for (size_t i = 0; i < e->count;) {
        W32File* now = w32_fd_get(e->entries[i].fd);
        bool same = now == e->entries[i].file;
        w32_file_put(now);
        if (!same) e->entries[i] = e->entries[--e->count];
        else i++;
      }
      n = e->count;
      if (n > kInline) {
        items = static_cast<Item*>(malloc(n * sizeof(Item)));
        snap = static_cast<Entry*>(malloc(n * sizeof(Entry)));
        if (!items || !snap) {
          free(items);
          free(snap);
          result = -ENOMEM;
          break;
        }
      }
      for (size_t i = 0; i < n; i++) {
        snap[i] = e->entries[i];
        short ev = 0;
        uint32_t want = snap[i].disabled ? 0 : snap[i].events;
        if (want & EPOLLIN) ev |= POLLIN;
        if (want & EPOLLOUT) ev |= POLLOUT;
        if (want & EPOLLPRI) ev |= POLLPRI;
        if (want & EPOLLRDHUP) ev |= POLLRDHUP;
        items[i] = Item{ev ? w32_fd_get(snap[i].fd) : nullptr, ev ? ev : static_cast<short>(-1), 0};
      }
    }
    bool woken = false;
    long r = wait_items(items, n, deadline, e->wake_r, &woken);
    if (woken) {
      char buf[64];
      while (w32.recv(e->wake_r, buf, sizeof buf, 0) > 0) {}
    }
    long count = 0;
    if (r > 0) {
      Lock lock(&e->lock);
      for (size_t i = 0; i < n && count < max; i++) {
        short re = items[i].events < 0 ? 0 : items[i].revents;
        if (!re) continue;
        uint32_t ev = 0;
        if (re & POLLIN) ev |= EPOLLIN;
        if (re & POLLOUT) ev |= EPOLLOUT;
        if (re & POLLPRI) ev |= EPOLLPRI;
        if (re & POLLERR) ev |= EPOLLERR;
        if (re & POLLHUP) ev |= EPOLLHUP;
        if (re & POLLRDHUP) ev |= EPOLLRDHUP;
        out[count].events = ev;
        out[count].data = snap[i].data;
        count++;
        if (snap[i].events & EPOLLONESHOT) {
          for (size_t k = 0; k < e->count; k++)
            if (e->entries[k].fd == snap[i].fd && e->entries[k].file == snap[i].file) e->entries[k].disabled = true;
        }
      }
    }
    for (size_t i = 0; i < n; i++) w32_file_put(items[i].f);
    if (items != inline_items) free(items);
    if (snap != inline_snap) free(snap);
    if (r < 0) {
      result = r;
      break;
    }
    if (count > 0) {
      result = count;
      break;
    }
    if (deadline >= 0 && w32_monotonic_ns() >= deadline) break;
  }
  __atomic_sub_fetch(&e->waiters, 1, __ATOMIC_ACQ_REL);
  w32_file_put(ef);
  return result;
}

static int64_t ms_deadline(long timeout_ms) {
  const int ms = static_cast<int>(timeout_ms);
  return ms < 0 ? -1 : deadline_after(static_cast<int64_t>(ms) * 1000000);
}

long sys_epoll_pwait(long epfd, long events, long max, long timeout_ms, long, long) {
  return epoll_wait_until(epfd, reinterpret_cast<epoll_event*>(events), static_cast<int>(max), ms_deadline(timeout_ms));
}

long sys_epoll_wait(long epfd, long events, long max, long timeout_ms, long, long) {
  return epoll_wait_until(epfd, reinterpret_cast<epoll_event*>(events), static_cast<int>(max), ms_deadline(timeout_ms));
}

long sys_epoll_pwait2(long epfd, long events, long max, long timeout_l, long, long) {
  auto* ts = reinterpret_cast<const timespec*>(timeout_l);
  if (ts && (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)) return -EINVAL;
  return epoll_wait_until(epfd, reinterpret_cast<epoll_event*>(events), static_cast<int>(max),
                          ts ? deadline_after(timespec_ns(ts)) : -1);
}

long sys_eventfd2(long initial, long flags, long, long, long, long) {
  if (flags & ~(EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE)) return -EINVAL;
  W32File* f = w32_file_new(W32Kind::kEventFd, nullptr, O_RDWR | ((flags & EFD_NONBLOCK) ? O_NONBLOCK : 0));
  Counter* c = static_cast<Counter*>(calloc(1, sizeof(Counter)));
  if (!f || !c) {
    free(f);
    free(c);
    return -ENOMEM;
  }
  long r = internal_pair(&c->r, &c->w);
  if (r < 0) {
    free(f);
    free(c);
    return r;
  }
  c->semaphore = flags & EFD_SEMAPHORE;
  f->extra = c;
  f->destroy = destroy_counter;
  if (initial) counter_add(c, static_cast<uint32_t>(initial));
  int fd = w32_fd_install(f, flags & EFD_CLOEXEC);
  w32_file_put(f);
  return fd;
}

long sys_eventfd(long initial, long, long, long, long, long) { return sys_eventfd2(initial, 0, 0, 0, 0, 0); }

long w32_special_read(W32File* f, void* buf, size_t len) {
  if (len < 8) return -EINVAL;
  if (f->kind == W32Kind::kTimerFd) {
    Timer* t = timer_of(f);
    uint64_t n = timer_take(t);
    if (!n && !(f->flags & O_NONBLOCK)) {
      const bool shared = timer_wait_begin(t);
      while (!(n = timer_take(t))) {
        int64_t due = timer_due(t);
        if (shared && (due == kNever || due > 50000000)) due = 50000000;
        if (wait_readable(t->r, due == kNever ? -1 : due > 0 ? due : 0)) timer_drain(t);
      }
      timer_wait_end(t);
    }
    if (!n) return -EAGAIN;
    memcpy(buf, &n, 8);
    return 8;
  }
  Counter* c = static_cast<Counter*>(f->extra);
  for (;;) {
    uint64_t v = counter_take(c);
    if (v) {
      memcpy(buf, &v, 8);
      return 8;
    }
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wait_readable(c->r, -1);
  }
}

long w32_special_write(W32File* f, const void* buf, size_t len) {
  if (f->kind != W32Kind::kEventFd) return -EINVAL;
  if (len < 8) return -EINVAL;
  uint64_t v;
  memcpy(&v, buf, 8);
  if (v == ~0ull) return -EINVAL;
  counter_add(static_cast<Counter*>(f->extra), v);
  return 8;
}

long sys_timerfd_create(long clock, long flags, long, long, long, long) {
  if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME && clock != CLOCK_BOOTTIME) return -EINVAL;
  if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC)) return -EINVAL;
  W32File* f = w32_file_new(W32Kind::kTimerFd, nullptr, O_RDWR | ((flags & TFD_NONBLOCK) ? O_NONBLOCK : 0));
  Timer* t = static_cast<Timer*>(calloc(1, sizeof(Timer)));
  if (!f || !t) {
    free(f);
    free(t);
    return -ENOMEM;
  }
  long r = internal_pair(&t->r, &t->w);
  if (r < 0) {
    free(f);
    free(t);
    return r;
  }
  t->clock = static_cast<int>(clock);
  f->extra = t;
  f->destroy = destroy_timer;
  int fd = w32_fd_install(f, flags & TFD_CLOEXEC);
  w32_file_put(f);
  return fd;
}

namespace {

int64_t itimer_ns(const timespec& ts) { return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec; }

timespec ns_timespec(int64_t ns) { return timespec{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)}; }

void timer_current(Timer* t, itimerspec* out) {
  int64_t left = 0;
  if (t->next_ns) {
    left = t->next_ns - clock_now(t->clock);
    if (left <= 0) left = t->interval_ns > 0 ? t->interval_ns - (-left) % t->interval_ns : 0;
  }
  out->it_value = ns_timespec(left);
  out->it_interval = ns_timespec(t->interval_ns);
}

}  // namespace

long sys_timerfd_settime(long fd, long flags, long new_l, long old_l, long, long) {
  W32File* f = w32_fd_get(static_cast<int>(fd));
  if (!f) return -EBADF;
  if (f->kind != W32Kind::kTimerFd) {
    w32_file_put(f);
    return -EINVAL;
  }
  auto* nv = reinterpret_cast<const itimerspec*>(new_l);
  if (nv->it_value.tv_sec < 0 || nv->it_value.tv_nsec < 0 || nv->it_value.tv_nsec >= 1000000000 ||
      nv->it_interval.tv_sec < 0 || nv->it_interval.tv_nsec < 0 || nv->it_interval.tv_nsec >= 1000000000) {
    w32_file_put(f);
    return -EINVAL;
  }
  Timer* t = timer_of(f);
  {
    Lock lock(&t->lock);
    if (old_l) timer_current(t, reinterpret_cast<itimerspec*>(old_l));
    const int64_t value = itimer_ns(nv->it_value);
    t->interval_ns = itimer_ns(nv->it_interval);
    if (value == 0) {
      t->next_ns = 0;
    } else if (flags & TFD_TIMER_ABSTIME) {
      t->next_ns = value;
    } else {
      const int64_t now = clock_now(t->clock);
      t->next_ns = value > kNever - now ? kNever : now + value;
    }
    if (t->waiters > 0 && !t->kicked) {
      char b = 1;
      w32.send(t->w, &b, 1, 0);
      t->kicked = true;
    }
  }
  w32_file_put(f);
  return 0;
}

long sys_timerfd_gettime(long fd, long cur_l, long, long, long, long) {
  W32File* f = w32_fd_get(static_cast<int>(fd));
  if (!f) return -EBADF;
  if (f->kind != W32Kind::kTimerFd) {
    w32_file_put(f);
    return -EINVAL;
  }
  Timer* t = timer_of(f);
  {
    Lock lock(&t->lock);
    timer_current(t, reinterpret_cast<itimerspec*>(cur_l));
  }
  w32_file_put(f);
  return 0;
}
