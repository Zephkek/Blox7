// what roblox's microprofiler web server does to stop: accept blocked in a thread, shutdown, close, join.
// plain linux calls, the same program runs on a phone and the check lines have to match
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int listener;
static volatile int accept_result = -2, accept_errno;

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void* server(void* arg) {
  (void)arg;
  int c = accept(listener, NULL, NULL);
  accept_errno = c < 0 ? errno : 0;
  accept_result = c;
  return NULL;
}

static int listening_socket(void) {
  int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr*)&a, sizeof a) || listen(s, 8)) printf("setup failed: %s\n", strerror(errno));
  return s;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  // 1. shutdown(SHUT_WR) on a listener: Linux answers 0 and the listener stays
  int s = listening_socket();
  int r = shutdown(s, SHUT_WR);
  printf("check shutdown(SHUT_WR) on a listener: %d %s\n", r, r ? strerror(errno) : "");
  struct pollfd p = {s, POLLIN, 0};
  r = poll(&p, 1, 0);
  printf("check poll after SHUT_WR: %d revents %#x\n", r, p.revents);
  close(s);

  // 2. the MicroProfiler stop: accept blocked in a thread, shutdown(SHUT_RDWR), close, join
  listener = listening_socket();
  pthread_t t;
  pthread_create(&t, NULL, server, NULL);
  usleep(300 * 1000);
  printf("check accept still waiting before shutdown: %s\n", accept_result == -2 ? "yes" : "no");
  double t0 = now_ms();
  r = shutdown(listener, SHUT_RDWR);
  printf("check shutdown(SHUT_RDWR) on the listener: %d %s\n", r, r ? strerror(errno) : "");
  p.fd = listener;
  p.events = POLLIN | POLLOUT | POLLRDHUP;
  p.revents = 0;
  r = poll(&p, 1, 0);
  printf("check poll on the shut listener: %d revents %#x\n", r, p.revents);
  close(listener);
  for (int i = 0; i < 300 && accept_result == -2; i++) usleep(10 * 1000);
  printf("check accept woken: result %d errno %s\n", accept_result, accept_result < 0 && accept_result != -2 ? strerror(accept_errno) : "-");
  if (accept_result != -2) {
    pthread_join(t, NULL);
    printf("check joined\n");
    printf("time shutdown to join: %.1f ms\n", now_ms() - t0);
  } else {
    printf("check joined: no (accept still blocked after 3 s)\n");
  }

  // 3. accept on a listener already shut down (non-blocking: where the shutdown did nothing, EAGAIN, not a hang)
  s = listening_socket();
  shutdown(s, SHUT_RD);
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
  r = accept(s, NULL, NULL);
  printf("check accept after SHUT_RD: %d %s\n", r, r < 0 ? strerror(errno) : "");
  close(s);
  printf("done\n");
  return 0;
}
