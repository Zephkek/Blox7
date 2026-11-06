// UDP_SEGMENT like linux: a send bigger than the segment leaves as segment-sized datagrams, the last one shorter.
// roblox batches packets this way; sent as one fat datagram the server never reads it. prints size@offset per datagram
#include <errno.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif

static int rx, tx;
static struct sockaddr_in to;
static unsigned char data[2048];

static void drain(const char* what) {
  static unsigned char buf[70000];
  printf("%-34s:", what);
  for (int first = 1;; first = 0) {
    struct pollfd p = {rx, POLLIN, 0};
    if (poll(&p, 1, first ? 500 : 100) <= 0) break;
    ssize_t n = recv(rx, buf, sizeof buf, 0);
    if (n < 0) break;
    printf(" %zd@%u", n, buf[0] | (unsigned)buf[1] << 8);
  }
  printf("\n");
}

static void result(const char* what, long r) {
  if (r < 0) printf("%-34s: -1 %s\n", what, strerror(errno));
  else printf("%-34s: %ld\n", what, r);
}

static int get_int(int fd, int level, int name) {
  int v = -1;
  socklen_t len = sizeof v;
  if (getsockopt(fd, level, name, &v, &len) != 0) return -errno;
  return v;
}

static int set_int(int fd, int level, int name, int v) {
  return setsockopt(fd, level, name, &v, sizeof v) == 0 ? 0 : -errno;
}

static long send_segmented(size_t len, int segment, int type) {
  struct iovec iov[2] = {{data, len / 2}, {data + len / 2, len - len / 2}};
  union {
    char buf[CMSG_SPACE(sizeof(uint16_t))];
    struct cmsghdr align;
  } control;
  struct msghdr m = {&to, sizeof to, iov, 2, NULL, 0, 0};
  if (segment > 0) {
    memset(&control, 0, sizeof control);
    m.msg_control = control.buf;
    m.msg_controllen = sizeof control.buf;
    struct cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_UDP;
    c->cmsg_type = type;
    c->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t size = (uint16_t)segment;
    memcpy(CMSG_DATA(c), &size, sizeof size);
  }
  return sendmsg(tx, &m, 0);
}

int main(void) {
  for (unsigned i = 0; i < sizeof data; i += 2) {
    data[i] = (unsigned char)i;
    data[i + 1] = (unsigned char)(i >> 8);
  }
  rx = socket(AF_INET, SOCK_DGRAM, 0);
  tx = socket(AF_INET, SOCK_DGRAM, 0);
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof to;
  if (bind(rx, (struct sockaddr*)&to, sizeof to) != 0 || getsockname(rx, (struct sockaddr*)&to, &len) != 0) {
    perror("bind");
    return 1;
  }

  printf("defaults: UDP_SEGMENT %d UDP_GRO %d IP_MTU_DISCOVER %d\n", get_int(tx, SOL_UDP, UDP_SEGMENT),
         get_int(tx, SOL_UDP, UDP_GRO), get_int(tx, IPPROTO_IP, IP_MTU_DISCOVER));
  sendto(tx, data, 250, 0, (struct sockaddr*)&to, sizeof to);
  drain("sendto 250, no segment size");

  result("setsockopt UDP_SEGMENT 100", set_int(tx, SOL_UDP, UDP_SEGMENT, 100));
  printf("getsockopt UDP_SEGMENT            : %d\n", get_int(tx, SOL_UDP, UDP_SEGMENT));
  result("sendto 250", sendto(tx, data, 250, 0, (struct sockaddr*)&to, sizeof to));
  drain("  arrived");
  result("sendto 100 (one segment)", sendto(tx, data, 100, 0, (struct sockaddr*)&to, sizeof to));
  drain("  arrived");
  set_int(tx, SOL_UDP, UDP_SEGMENT, 0);

  result("sendmsg 200, cmsg segment 64", send_segmented(200, 64, UDP_SEGMENT));
  drain("  arrived");
  result("sendmsg 640, cmsg segment 10", send_segmented(640, 10, UDP_SEGMENT));
  drain("  arrived (64 datagrams)");
  result("sendmsg 650, cmsg segment 10", send_segmented(650, 10, UDP_SEGMENT));
  result("sendmsg, SOL_UDP cmsg type 99", send_segmented(200, 64, 99));
  result("setsockopt UDP_SEGMENT 70000", set_int(tx, SOL_UDP, UDP_SEGMENT, 70000));
  result("setsockopt UDP_SEGMENT -1", set_int(tx, SOL_UDP, UDP_SEGMENT, -1));

  struct iovec iov[2] = {{data, 100}, {data + 100, 100}};
  union {
    char buf[CMSG_SPACE(sizeof(uint16_t))];
    struct cmsghdr align;
  } control[2];
  struct mmsghdr mm[2];
  memset(mm, 0, sizeof mm);
  memset(control, 0, sizeof control);
  for (int i = 0; i < 2; i++) {
    struct msghdr* m = &mm[i].msg_hdr;
    m->msg_name = &to;
    m->msg_namelen = sizeof to;
    m->msg_iov = &iov[i];
    m->msg_iovlen = 1;
    m->msg_control = control[i].buf;
    m->msg_controllen = sizeof control[i].buf;
    struct cmsghdr* c = CMSG_FIRSTHDR(m);
    c->cmsg_level = SOL_UDP;
    c->cmsg_type = UDP_SEGMENT;
    c->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t size = 40;
    memcpy(CMSG_DATA(c), &size, sizeof size);
  }
  int sent = sendmmsg(tx, mm, 2, 0);
  printf("sendmmsg 2 x 100, segment 40      : %d (%u, %u)\n", sent, mm[0].msg_len, mm[1].msg_len);
  drain("  arrived");

  result("setsockopt UDP_GRO 1", set_int(rx, SOL_UDP, UDP_GRO, 1));
  printf("getsockopt UDP_GRO                : %d\n", get_int(rx, SOL_UDP, UDP_GRO));
  result("IP_MTU_DISCOVER IP_PMTUDISC_DO", set_int(tx, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DO));
  printf("getsockopt IP_MTU_DISCOVER        : %d\n", get_int(tx, IPPROTO_IP, IP_MTU_DISCOVER));
  result("sendto 1000 with DF", sendto(tx, data, 1000, 0, (struct sockaddr*)&to, sizeof to));
  drain("  arrived");
  result("IP_MTU_DISCOVER 9", set_int(tx, IPPROTO_IP, IP_MTU_DISCOVER, 9));
  int tcp = socket(AF_INET, SOCK_STREAM, 0);
  result("TCP: setsockopt UDP_SEGMENT", set_int(tcp, SOL_UDP, UDP_SEGMENT, 100));
  return 0;
}
