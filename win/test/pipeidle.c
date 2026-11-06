// pipes idle for minutes: the session log's tee lost everything once the updater ran past fin-wait-2 (2 min)
// usage: pipeidle [seconds]   (default 150; 0 runs only the quick one-way checks)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

static int secs = 150;

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static const char* err_name(int e) { return e ? strerror(e) : "ok"; }

static void* idle_pipe(void* arg) {
  (void)arg;
  int p[2];
  pipe2(p, O_CLOEXEC);
  sleep(secs);
  int w = (int)write(p[1], "x", 1);
  int we = w < 0 ? errno : 0;
  char c = 0;
  int r = (int)read(p[0], &c, 1);
  int re = r < 0 ? errno : 0;
  printf("check idle pipe write after the wait: %d %s\n", w, err_name(we));
  printf("check idle pipe read after the wait: %d %s\n", r, err_name(re));
  close(p[0]);
  close(p[1]);
  return NULL;
}

static void* trickle_pipe(void* arg) {
  (void)arg;
  int p[2];
  pipe2(p, O_CLOEXEC);
  int bad = 0;
  for (int t = 0; t < secs; t += 10) {
    sleep(10);
    char c = 'x';
    if (write(p[1], &c, 1) != 1 || read(p[0], &c, 1) != 1) bad++;
  }
  printf("check trickled pipe: %s\n", bad ? "broke" : "all bytes through");
  close(p[0]);
  close(p[1]);
  return NULL;
}

static void one_way(void) {
  int p[2];
  pipe2(p, O_CLOEXEC);
  printf("check access modes: read end %d, write end %d\n", fcntl(p[0], F_GETFL) & O_ACCMODE, fcntl(p[1], F_GETFL) & O_ACCMODE);
  errno = 0;
  int r = (int)write(p[0], "x", 1);
  printf("check write on the read end: %d %s\n", r, err_name(r < 0 ? errno : 0));
  char c = 0;
  r = (int)read(p[1], &c, 1);
  printf("check read on the write end: %d %s\n", r, err_name(r < 0 ? errno : 0));
  struct pollfd out = {p[1], POLLOUT, 0};
  r = poll(&out, 1, 1000);
  printf("check poll on the write end: %d revents %#x\n", r, out.revents);
  int w = (int)write(p[1], "yz", 2);
  char buf[8] = {0};
  r = (int)read(p[0], buf, sizeof buf);
  printf("check data: wrote %d, read %d '%s'\n", w, r, buf);
  close(p[1]);
  r = (int)read(p[0], buf, sizeof buf);
  printf("check read once the writer closed: %d %s\n", r, err_name(r < 0 ? errno : 0));
  close(p[0]);
}

struct Tee {
  int fd;
  char text[4096];
  size_t len;
  int error;
  double error_at;
};

static void* tee_thread(void* arg) {
  struct Tee* t = arg;
  for (;;) {
    struct pollfd p = {t->fd, POLLIN, 0};
    if (poll(&p, 1, -1) < 0) {
      if (errno == EINTR) continue;
      t->error = errno;
      break;
    }
    ssize_t n = read(t->fd, t->text + t->len, sizeof t->text - 1 - t->len);
    if (n > 0) {
      t->len += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      t->error = errno;
      t->error_at = now_s();
    }
    break;
  }
  t->text[t->len] = 0;
  return NULL;
}

int main(int argc, char** argv) {
  if (argc >= 3 && strcmp(argv[1], "child") == 0) {
    sleep(atoi(argv[2]));
    const char line[] = "child line\n";
    return write(STDOUT_FILENO, line, sizeof line - 1) == (ssize_t)(sizeof line - 1) ? 0 : 1;
  }
  if (argc >= 2) secs = atoi(argv[1]);
  setvbuf(stdout, NULL, _IOLBF, 0);
  one_way();
  if (secs <= 0) return 0;
  printf("waiting %d s\n", secs);
  pthread_t idle, trickle, tee;
  pthread_create(&idle, NULL, idle_pipe, NULL);
  pthread_create(&trickle, NULL, trickle_pipe, NULL);

  int p[2];
  pipe2(p, O_CLOEXEC);
  struct Tee t = {p[0], {0}, 0, 0, 0};
  pthread_create(&tee, NULL, tee_thread, &t);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, p[1], STDOUT_FILENO);
  char seconds[16];
  snprintf(seconds, sizeof seconds, "%d", secs);
  char self[4096] = {0};
  if (readlink("/proc/self/exe", self, sizeof self - 1) <= 0) snprintf(self, sizeof self, "%s", argv[0]);
  char* child_argv[] = {self, "child", seconds, NULL};
  pid_t child = -1;
  double start = now_s();
  int sr = posix_spawn(&child, self, &actions, NULL, child_argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  printf("check child started: %s\n", err_name(sr));
  int status = 0;
  if (sr == 0) waitpid(child, &status, 0);
  printf("check child exit status: %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  const char line[] = "parent line\n";
  ssize_t w = write(p[1], line, sizeof line - 1);
  int we = w < 0 ? errno : 0;
  printf("check parent write after the child: %s\n", w == (ssize_t)(sizeof line - 1) ? "ok" : err_name(we));
  close(p[1]);
  pthread_join(tee, NULL);
  close(p[0]);
  printf("check tee got the child line: %s\n", strstr(t.text, "child line\n") ? "yes" : "no");
  printf("check tee got the parent line: %s\n", strstr(t.text, "parent line\n") ? "yes" : "no");
  printf("check tee ended with: %s\n", err_name(t.error));
  if (t.error) printf("tee error after %.1f s\n", t.error_at - start);

  pthread_join(idle, NULL);
  pthread_join(trickle, NULL);
  return 0;
}
