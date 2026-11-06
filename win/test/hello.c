// the first thing that ever ran on this libc
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int counter;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* arg) {
  for (int i = 0; i < 100000; i++) {
    pthread_mutex_lock(&lock);
    counter++;
    pthread_mutex_unlock(&lock);
  }
  return arg;
}

int main(int argc, char** argv) {
  printf("hello from Android code on Windows: argc=%d argv[0]=%s\n", argc, argv[0]);
  char* p = malloc(1 << 20);
  memset(p, 1, 1 << 20);
  free(p);
  printf("malloc ok\n");

  pthread_t t[8];
  for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, worker, NULL);
  for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
  printf("threads ok: counter=%d (want 800000)\n", counter);

  int fd = open("/tmp/hello.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) { printf("open failed: %s\n", strerror(errno)); return 1; }
  write(fd, "written\n", 8);
  lseek(fd, 0, SEEK_SET);
  char buf[16] = {0};
  read(fd, buf, sizeof buf - 1);
  struct stat st;
  fstat(fd, &st);
  close(fd);
  printf("file ok: read \"%.7s\", size %lld\n", buf, (long long)st.st_size);
  unlink("/tmp/hello.txt");

  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  usleep(20000);
  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("sleep ok: %.2f ms (want ~20)\n", (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
  time_t now = time(NULL);
  printf("time: %s", ctime(&now));
  char cwd[256];
  printf("cwd: %s\n", getcwd(cwd, sizeof cwd));
  printf("ALL DONE\n");
  return 0;
}
