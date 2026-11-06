// the time zone from windows, and SOCK_SEQPACKET message boundaries on AF_UNIX sockets
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static void boundaries(int a, int b, const char* kind) {
  char buf[64], what[96];
  check(send(a, "one", 3, 0) == 3 && send(a, "second message", 14, 0) == 14 && send(a, "3", 1, 0) == 1,
        "three sends");
  ssize_t n = recv(b, buf, sizeof buf, MSG_PEEK);
  snprintf(what, sizeof what, "%s: MSG_PEEK sees the first message only", kind);
  check(n == 3 && memcmp(buf, "one", 3) == 0, what);
  n = recv(b, buf, sizeof buf, 0);
  snprintf(what, sizeof what, "%s: first message", kind);
  check(n == 3 && memcmp(buf, "one", 3) == 0, what);
  n = recv(b, buf, 6, MSG_TRUNC);
  snprintf(what, sizeof what, "%s: truncated second message reports its length", kind);
  check(n == 14 && memcmp(buf, "second", 6) == 0, what);
  n = recv(b, buf, sizeof buf, 0);
  snprintf(what, sizeof what, "%s: third message, nothing of the second left", kind);
  check(n == 1 && buf[0] == '3', what);
  n = recv(b, buf, sizeof buf, MSG_DONTWAIT);
  snprintf(what, sizeof what, "%s: empty queue is EAGAIN", kind);
  check(n < 0 && errno == EAGAIN, what);
}

int main(void) {
  const char* tz = getenv("TZ");
  printf("     TZ=%s\n", tz ? tz : "(unset)");
  check(tz != NULL && tz[0] == '<', "TZ comes from Windows");
  time_t now = time(NULL);
  struct tm local, utc;
  localtime_r(&now, &local);
  gmtime_r(&now, &utc);
  char text[64];
  strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S %Z", &local);
  printf("     local %s, UTC offset %ld s\n", text, (long)local.tm_gmtoff);
  check(local.tm_zone != NULL && local.tm_zone[0] != '\0', "local zone has a name");

  int pair[2];
  check(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0, "SEQPACKET socketpair");
  boundaries(pair[0], pair[1], "pair");

  struct sockaddr_un address = {AF_UNIX};
  strcpy(address.sun_path, "/tmp/tzseq.sock");
  unlink(address.sun_path);
  int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  check(bind(listener, (struct sockaddr*)&address, sizeof address) == 0 && listen(listener, 1) == 0, "named listener");
  struct stat st;
  check(lstat(address.sun_path, &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 077) == 0, "socket file is a private socket");
  int client = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  check(connect(client, (struct sockaddr*)&address, sizeof address) == 0, "connect");
  int server = accept(listener, NULL, NULL);
  check(server >= 0, "accept");
  boundaries(client, server, "named");
  close(client);
  char buf[8];
  check(recv(server, buf, sizeof buf, 0) == 0, "end of stream after the peer closes");
  printf("%d failures\n", failures);
  return failures != 0;
}
