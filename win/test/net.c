// networking and waiting: dns, tcp, poll, epoll with eventfd, pipes, timerfd
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void* poke(void* arg) {
  usleep(50000);
  uint64_t one = 1;
  write(*(int*)arg, &one, 8);
  return NULL;
}

int main(void) {
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int e = getaddrinfo("example.com", "80", &hints, &res);
  if (e) { printf("getaddrinfo failed: %d %s\n", e, gai_strerror(e)); return 1; }
  char text[64];
  struct addrinfo* ai;
  for (ai = res; ai; ai = ai->ai_next) {
    void* a = ai->ai_family == AF_INET ? (void*)&((struct sockaddr_in*)ai->ai_addr)->sin_addr : (void*)&((struct sockaddr_in6*)ai->ai_addr)->sin6_addr;
    printf("dns: example.com -> %s (family %d)\n", inet_ntop(ai->ai_family, a, text, sizeof text), ai->ai_family);
  }
  struct hostent* he = gethostbyname("example.com");
  printf("gethostbyname: %s\n", he ? inet_ntoa(*(struct in_addr*)he->h_addr_list[0]) : "failed");

  for (ai = res; ai && ai->ai_family != AF_INET; ai = ai->ai_next) {}
  int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int r = connect(s, ai->ai_addr, ai->ai_addrlen);
  printf("connect: %d (%s)\n", r, r ? strerror(errno) : "ok");
  struct pollfd p = {s, POLLOUT, 0};
  r = poll(&p, 1, 5000);
  int err = 0;
  socklen_t len = sizeof err;
  getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
  printf("poll for connect: %d revents 0x%x so_error %d\n", r, p.revents, err);
  const char* req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
  send(s, req, strlen(req), MSG_NOSIGNAL);
  char buf[512];
  size_t total = 0;
  for (;;) {
    p.events = POLLIN;
    if (poll(&p, 1, 5000) <= 0) break;
    ssize_t n = recv(s, buf, sizeof buf - 1, 0);
    if (n <= 0) break;
    if (total == 0) { buf[n] = 0; char* nl = strchr(buf, '\r'); if (nl) *nl = 0; printf("http: %s\n", buf); }
    total += (size_t)n;
  }
  printf("http: %zu bytes\n", total);
  close(s);
  freeaddrinfo(res);

  int ep = epoll_create1(EPOLL_CLOEXEC);
  int ev = eventfd(0, EFD_NONBLOCK);
  struct epoll_event ee = {EPOLLIN, {.u64 = 42}};
  epoll_ctl(ep, EPOLL_CTL_ADD, ev, &ee);
  pthread_t t;
  pthread_create(&t, NULL, poke, &ev);
  double t0 = now_ms();
  struct epoll_event got;
  r = epoll_wait(ep, &got, 1, 2000);
  uint64_t value = 0;
  read(ev, &value, 8);
  printf("epoll+eventfd: %d event(s), data %llu, value %llu after %.0f ms (want ~50)\n", r, (unsigned long long)got.data.u64,
         (unsigned long long)value, now_ms() - t0);
  pthread_join(t, NULL);

  int pp[2];
  pipe(pp);
  write(pp[1], "x", 1);
  struct pollfd pq = {pp[0], POLLIN, 0};
  printf("pipe poll: %d revents 0x%x\n", poll(&pq, 1, 1000), pq.revents);

  int tf = timerfd_create(CLOCK_MONOTONIC, 0);
  struct itimerspec its = {{0, 0}, {0, 30000000}};
  timerfd_settime(tf, 0, &its, NULL);
  t0 = now_ms();
  uint64_t exp = 0;
  read(tf, &exp, 8);
  printf("timerfd: %llu expiration after %.1f ms (want ~30)\n", (unsigned long long)exp, now_ms() - t0);
  printf("NET DONE\n");
  return 0;
}
