// socket, poll and timer behaviour roblox leans on. run it on a phone and on the port, check lines must match
// build with the ndk's clang and -rdynamic. on the phone run it as an app (netcheck_apk.py), not from adb
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void check(const char* what, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(const char* what, const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("check %-44s %s\n", what, buf);
  fflush(stdout);
}

static void timing(const char* what, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void timing(const char* what, const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("time  %-44s %s\n", what, buf);
  fflush(stdout);
}

static const char* err_name(int e) {
  switch (e) {
    case 0: return "ok";
    case EAGAIN: return "EAGAIN";
    case EINVAL: return "EINVAL";
    case EPERM: return "EPERM";
    case ENOPROTOOPT: return "ENOPROTOOPT";
    case EOPNOTSUPP: return "EOPNOTSUPP";
    case ECONNREFUSED: return "ECONNREFUSED";
    case ECONNRESET: return "ECONNRESET";
    case ENOTCONN: return "ENOTCONN";
    case EPIPE: return "EPIPE";
    case EMSGSIZE: return "EMSGSIZE";
    case EDOM: return "EDOM";
    case ETIMEDOUT: return "ETIMEDOUT";
    case EAFNOSUPPORT: return "EAFNOSUPPORT";
    case ENODEV: return "ENODEV";
    case ENOTTY: return "ENOTTY";
    case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
    case EADDRINUSE: return "EADDRINUSE";
    case EACCES: return "EACCES";
    case ESOCKTNOSUPPORT: return "ESOCKTNOSUPPORT";
    case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
    case EFAULT: return "EFAULT";
    case ENOENT: return "ENOENT";
    default: {
      static char b[32];
      snprintf(b, sizeof b, "errno%d", e);
      return b;
    }
  }
}

#define K(x) ({ long k_v_ = (long)(x); k_v_ < 0 ? -1000L - errno : k_v_; })

static char* rotating(void) {
  static char bufs[16][64];
  static int next;
  return bufs[next++ & 15];
}

static const char* res(long r) {
  char* b = rotating();
  if (r <= -1000) return err_name((int)(-(r + 1000)));
  snprintf(b, 64, "%ld", r);
  return b;
}

static int get_int(int fd, int level, int name) {
  int v = -12345;
  socklen_t len = sizeof v;
  if (getsockopt(fd, level, name, &v, &len) != 0) return -10000 - errno;
  return v;
}

static const char* opt(int fd, int level, int name) {
  char* b = rotating();
  int v = get_int(fd, level, name);
  if (v <= -10000) return err_name(-(v + 10000));
  snprintf(b, 64, "%d", v);
  return b;
}

static long set_int(int fd, int level, int name, int v) { return K(setsockopt(fd, level, name, &v, sizeof v)); }

static struct sockaddr_in loop4(int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

static int port_of(int fd) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof ss;
  getsockname(fd, (struct sockaddr*)&ss, &len);
  return ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6*)&ss)->sin6_port : ((struct sockaddr_in*)&ss)->sin_port);
}

static int udp4_bound(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a = loop4(0);
  bind(fd, (struct sockaddr*)&a, sizeof a);
  return fd;
}

static int wait_in(int fd, int ms) {
  struct pollfd p = {fd, POLLIN, 0};
  return poll(&p, 1, ms);
}

static const char* poll_bits(short re) {
  static char b[96];
  b[0] = 0;
  if (re & POLLIN) strcat(b, "IN|");
  if (re & POLLOUT) strcat(b, "OUT|");
  if (re & POLLPRI) strcat(b, "PRI|");
  if (re & POLLERR) strcat(b, "ERR|");
  if (re & POLLHUP) strcat(b, "HUP|");
  if (re & POLLRDHUP) strcat(b, "RDHUP|");
  if (re & POLLNVAL) strcat(b, "NVAL|");
  size_t n = strlen(b);
  if (n) b[n - 1] = 0;
  else strcpy(b, "-");
  return b;
}

static void defaults(void) {
  int u4 = socket(AF_INET, SOCK_DGRAM, 0), u6 = socket(AF_INET6, SOCK_DGRAM, 0), t4 = socket(AF_INET, SOCK_STREAM, 0);
  printf("info  udp4 SO_RCVBUF default %s (kernel tunable: rmem_default)\n", opt(u4, SOL_SOCKET, SO_RCVBUF));
  printf("info  udp4 SO_SNDBUF default %s (kernel tunable: wmem_default)\n", opt(u4, SOL_SOCKET, SO_SNDBUF));
  printf("info  tcp4 SO_RCVBUF default %s (kernel tunable: tcp_rmem)\n", opt(t4, SOL_SOCKET, SO_RCVBUF));
  printf("info  tcp4 SO_SNDBUF default %s (kernel tunable: tcp_wmem)\n", opt(t4, SOL_SOCKET, SO_SNDBUF));
  check("udp6 IPV6_V6ONLY default", "%s", opt(u6, IPPROTO_IPV6, IPV6_V6ONLY));
  check("udp4 IP_MTU_DISCOVER default", "%s", opt(u4, IPPROTO_IP, IP_MTU_DISCOVER));
  check("tcp4 SO_KEEPALIVE default", "%s", opt(t4, SOL_SOCKET, SO_KEEPALIVE));
  check("tcp4 TCP_KEEPIDLE default", "%s", opt(t4, IPPROTO_TCP, TCP_KEEPIDLE));
  check("tcp4 TCP_KEEPINTVL default", "%s", opt(t4, IPPROTO_TCP, TCP_KEEPINTVL));
  check("tcp4 TCP_KEEPCNT default", "%s", opt(t4, IPPROTO_TCP, TCP_KEEPCNT));
  check("udp4 IP_PKTINFO default", "%s", opt(u4, IPPROTO_IP, IP_PKTINFO));
  check("udp4 IP_RECVERR default", "%s", opt(u4, IPPROTO_IP, IP_RECVERR));

  struct sockaddr_storage ss;
  socklen_t len = sizeof ss;
  memset(&ss, 0x55, sizeof ss);
  long r = K(getsockname(u4, (struct sockaddr*)&ss, &len));
  struct sockaddr_in* a = (struct sockaddr_in*)&ss;
  check("udp4 getsockname unbound", "%s len=%u family=%d port=%d addr=%08x", res(r), (unsigned)len, a->sin_family,
        ntohs(a->sin_port), ntohl(a->sin_addr.s_addr));
  len = sizeof ss;
  r = K(getsockname(u6, (struct sockaddr*)&ss, &len));
  check("udp6 getsockname unbound", "%s len=%u family=%d port=%d", res(r), (unsigned)len, ss.ss_family,
        ntohs(((struct sockaddr_in6*)&ss)->sin6_port));
  len = sizeof ss;
  r = K(getpeername(u4, (struct sockaddr*)&ss, &len));
  check("udp4 getpeername unconnected", "%s", res(r));

  check("udp4 SO_RCVBUF=262144", "%s -> %s", res(set_int(u4, SOL_SOCKET, SO_RCVBUF, 262144)), opt(u4, SOL_SOCKET, SO_RCVBUF));
  check("udp4 SO_SNDBUF=16384", "%s -> %s", res(set_int(u4, SOL_SOCKET, SO_SNDBUF, 16384)), opt(u4, SOL_SOCKET, SO_SNDBUF));
  check("udp4 SO_RCVBUF=1", "%s -> %s", res(set_int(u4, SOL_SOCKET, SO_RCVBUF, 1)), opt(u4, SOL_SOCKET, SO_RCVBUF));
  check("udp4 SO_SNDBUF=1", "%s -> %s", res(set_int(u4, SOL_SOCKET, SO_SNDBUF, 1)), opt(u4, SOL_SOCKET, SO_SNDBUF));
  check("udp4 SO_RCVBUF=524288", "%s -> %s", res(set_int(u4, SOL_SOCKET, SO_RCVBUF, 524288)), opt(u4, SOL_SOCKET, SO_RCVBUF));
  check("udp4 SO_RCVBUFFORCE (no CAP_NET_ADMIN)", "%s", res(set_int(u4, SOL_SOCKET, SO_RCVBUFFORCE, 1 << 20)));
  check("udp4 IP_PKTINFO=1", "%s -> %s", res(set_int(u4, IPPROTO_IP, IP_PKTINFO, 1)), opt(u4, IPPROTO_IP, IP_PKTINFO));
  check("udp4 IP_RECVERR=1", "%s -> %s", res(set_int(u4, IPPROTO_IP, IP_RECVERR, 1)), opt(u4, IPPROTO_IP, IP_RECVERR));
  check("udp4 IPV6_RECVPKTINFO (an IPv4 socket)", "%s", res(set_int(u4, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1)));
  check("udp6 IPV6_RECVPKTINFO=1", "%s -> %s", res(set_int(u6, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1)), opt(u6, IPPROTO_IPV6, IPV6_RECVPKTINFO));
  check("udp4 IP_MTU_DISCOVER=PROBE", "%s -> %s", res(set_int(u4, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_PROBE)), opt(u4, IPPROTO_IP, IP_MTU_DISCOVER));
  check("udp4 TCP_NODELAY (a UDP socket)", "%s", res(set_int(u4, IPPROTO_TCP, TCP_NODELAY, 1)));
  check("udp4 TCP_KEEPIDLE (a UDP socket)", "%s", res(set_int(u4, IPPROTO_TCP, TCP_KEEPIDLE, 60)));

  check("tcp4 SO_KEEPALIVE=1", "%s -> %s", res(set_int(t4, SOL_SOCKET, SO_KEEPALIVE, 1)), opt(t4, SOL_SOCKET, SO_KEEPALIVE));
  check("tcp4 TCP_KEEPIDLE=60", "%s -> %s", res(set_int(t4, IPPROTO_TCP, TCP_KEEPIDLE, 60)), opt(t4, IPPROTO_TCP, TCP_KEEPIDLE));
  check("tcp4 TCP_KEEPINTVL=10", "%s -> %s", res(set_int(t4, IPPROTO_TCP, TCP_KEEPINTVL, 10)), opt(t4, IPPROTO_TCP, TCP_KEEPINTVL));
  check("tcp4 TCP_KEEPCNT=5", "%s -> %s", res(set_int(t4, IPPROTO_TCP, TCP_KEEPCNT, 5)), opt(t4, IPPROTO_TCP, TCP_KEEPCNT));
  check("tcp4 TCP_KEEPIDLE=0", "%s", res(set_int(t4, IPPROTO_TCP, TCP_KEEPIDLE, 0)));
  check("tcp4 TCP_KEEPCNT=200", "%s", res(set_int(t4, IPPROTO_TCP, TCP_KEEPCNT, 200)));
  check("tcp4 TCP_NODELAY=1", "%s -> %s", res(set_int(t4, IPPROTO_TCP, TCP_NODELAY, 1)), opt(t4, IPPROTO_TCP, TCP_NODELAY) );
  struct timeval tv = {0, 2000000};
  check("tcp4 SO_RCVTIMEO usec=2000000", "%s", res(K(setsockopt(t4, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv))));
  tv.tv_usec = 250000;
  check("tcp4 SO_RCVTIMEO short optlen", "%s", res(K(setsockopt(t4, SOL_SOCKET, SO_RCVTIMEO, &tv, 4))));
  check("tcp4 SO_RCVTIMEO 0.25s", "%s", res(K(setsockopt(t4, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv))));
  struct timeval got;
  socklen_t gl = sizeof got;
  getsockopt(t4, SOL_SOCKET, SO_RCVTIMEO, &got, &gl);
  check("tcp4 SO_RCVTIMEO read back", "%ld.%06ld len=%u", (long)got.tv_sec, (long)got.tv_usec, (unsigned)gl);
  close(u4);
  close(u6);
  close(t4);
}

static void datagrams(void) {
  int rx = udp4_bound(), tx = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in to = loop4(port_of(rx));
  char a[10], b[20], c[30], big[2048];
  memset(a, 'a', sizeof a);
  memset(b, 'b', sizeof b);
  memset(c, 'c', sizeof c);

  struct iovec v3[3] = {{a, sizeof a}, {b, sizeof b}, {c, sizeof c}};
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_name = &to;
  m.msg_namelen = sizeof to;
  m.msg_iov = v3;
  m.msg_iovlen = 3;
  long r = K(sendmsg(tx, &m, 0));
  wait_in(rx, 1000);
  long n = K(recv(rx, big, sizeof big, MSG_DONTWAIT));
  long more = K(recv(rx, big + 100, sizeof big - 100, MSG_DONTWAIT));
  check("sendmsg 3 buffers -> datagrams", "sent %s, first %ld bytes [%c%c%c], then %s", res(r), n, big[0], big[10], big[30], res(more));

  connect(tx, (struct sockaddr*)&to, sizeof to);
  struct iovec w2[2] = {{a, sizeof a}, {c, sizeof c}};
  r = K(writev(tx, w2, 2));
  wait_in(rx, 1000);
  char r1[5], r2[100];
  memset(r2, 0, sizeof r2);
  struct iovec rv[2] = {{r1, sizeof r1}, {r2, sizeof r2}};
  n = K(readv(rx, rv, 2));
  more = K(recv(rx, big, sizeof big, MSG_DONTWAIT));
  check("writev 2 buffers / readv 2 buffers", "sent %s, read %ld [%c..%c|%c..%c] then %s", res(r), n, r1[0], r1[4], r2[0], r2[n > 5 ? n - 6 : 0], res(more));

  send(tx, big, 600, 0);
  wait_in(rx, 1000);
  char small[16];
  struct iovec sv = {small, sizeof small};
  memset(&m, 0, sizeof m);
  m.msg_iov = &sv;
  m.msg_iovlen = 1;
  n = K(recvmsg(rx, &m, 0));
  check("recvmsg 600-byte datagram into 16", "%s flags=%s", res(n), (m.msg_flags & MSG_TRUNC) ? "MSG_TRUNC" : "0");
  send(tx, big, 600, 0);
  wait_in(rx, 1000);
  n = K(recv(rx, small, sizeof small, MSG_TRUNC));
  check("recv(MSG_TRUNC) 600-byte datagram into 16", "%s", res(n));

  send(tx, a, sizeof a, 0);
  wait_in(rx, 1000);
  char ctl[256];
  memset(&m, 0, sizeof m);
  sv.iov_base = big;
  sv.iov_len = sizeof big;
  m.msg_iov = &sv;
  m.msg_iovlen = 1;
  m.msg_control = ctl;
  m.msg_controllen = sizeof ctl;
  n = K(recvmsg(rx, &m, MSG_ERRQUEUE | MSG_DONTWAIT));
  long data = K(recv(rx, big, sizeof big, MSG_DONTWAIT));
  check("recvmsg(MSG_ERRQUEUE) with data queued", "%s, then data %s", res(n), res(data));

  int q[4] = {-1, -1, -1, -1};
  long qr = K(ioctl(rx, FIONREAD, &q[0]));
  send(tx, big, 100, 0);
  send(tx, big, 200, 0);
  send(tx, big, 300, 0);
  wait_in(rx, 1000);
  usleep(20000);
  ioctl(rx, FIONREAD, &q[1]);
  recv(rx, big, sizeof big, MSG_DONTWAIT);
  ioctl(rx, FIONREAD, &q[2]);
  recv(rx, big, sizeof big, MSG_DONTWAIT);
  recv(rx, big, sizeof big, MSG_DONTWAIT);
  ioctl(rx, FIONREAD, &q[3]);
  check("UDP FIONREAD: none, 100+200+300, after 1, after 3", "%s %d, %d, %d, %d", res(qr), q[0], q[1], q[2], q[3]);
  send(tx, big, 0, 0);
  wait_in(rx, 1000);
  q[0] = -1;
  ioctl(rx, FIONREAD, &q[0]);
  n = K(recv(rx, big, sizeof big, MSG_DONTWAIT));
  check("UDP FIONREAD, a zero-length datagram", "%d, recv %s", q[0], res(n));
  int dp[2];
  socketpair(AF_UNIX, SOCK_DGRAM, 0, dp);
  send(dp[0], big, 10, 0);
  send(dp[0], big, 20, 0);
  wait_in(dp[1], 1000);
  usleep(20000);
  q[0] = -1;
  ioctl(dp[1], FIONREAD, &q[0]);
  check("AF_UNIX SOCK_DGRAM FIONREAD, 10+20 queued", "%d", q[0]);
  close(dp[0]);
  close(dp[1]);

  set_int(rx, IPPROTO_IP, IP_PKTINFO, 1);
  send(tx, a, sizeof a, 0);
  wait_in(rx, 1000);
  memset(&m, 0, sizeof m);
  m.msg_iov = &sv;
  m.msg_iovlen = 1;
  m.msg_control = ctl;
  m.msg_controllen = sizeof ctl;
  struct sockaddr_in from;
  m.msg_name = &from;
  m.msg_namelen = sizeof from;
  n = K(recvmsg(rx, &m, 0));
  char desc[160] = "none";
  for (struct cmsghdr* cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm)) {
    if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO) {
      struct in_pktinfo pi;
      memcpy(&pi, CMSG_DATA(cm), sizeof pi);
      snprintf(desc, sizeof desc, "IP_PKTINFO len=%zu ifindex=%d spec_dst=%08x addr=%08x", (size_t)cm->cmsg_len, pi.ipi_ifindex,
               ntohl(pi.ipi_spec_dst.s_addr), ntohl(pi.ipi_addr.s_addr));
    }
  }
  check("IP_PKTINFO recvmsg", "%s controllen=%zu %s from=%08x namelen=%u", res(n), (size_t)m.msg_controllen, desc,
        ntohl(from.sin_addr.s_addr), (unsigned)m.msg_namelen);
  send(tx, a, sizeof a, 0);
  wait_in(rx, 1000);
  m.msg_control = ctl;
  m.msg_controllen = 8;
  m.msg_name = NULL;
  m.msg_namelen = 0;
  n = K(recvmsg(rx, &m, 0));
  check("IP_PKTINFO recvmsg, 8 bytes of room", "%s controllen=%zu flags=%s", res(n), (size_t)m.msg_controllen,
        (m.msg_flags & MSG_CTRUNC) ? "MSG_CTRUNC" : "0");
  set_int(rx, IPPROTO_IP, IP_PKTINFO, 0);

  int rx6 = socket(AF_INET6, SOCK_DGRAM, 0);
  struct sockaddr_in6 any6;
  memset(&any6, 0, sizeof any6);
  any6.sin6_family = AF_INET6;
  bind(rx6, (struct sockaddr*)&any6, sizeof any6);
  set_int(rx6, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1);
  struct sockaddr_in to6as4 = loop4(port_of(rx6));
  r = K(sendto(tx, a, sizeof a, 0, (struct sockaddr*)&to6as4, sizeof to6as4));
  int tx2 = socket(AF_INET, SOCK_DGRAM, 0);
  r = K(sendto(tx2, a, sizeof a, 0, (struct sockaddr*)&to6as4, sizeof to6as4));
  long ready = wait_in(rx6, 1000);
  struct sockaddr_in6 from6;
  memset(&m, 0, sizeof m);
  m.msg_iov = &sv;
  m.msg_iovlen = 1;
  m.msg_control = ctl;
  m.msg_controllen = sizeof ctl;
  m.msg_name = &from6;
  m.msg_namelen = sizeof from6;
  long got = 0;
  strcpy(desc, "none");
  while (ready > 0 && (n = K(recvmsg(rx6, &m, MSG_DONTWAIT))) >= 0) {
    got++;
    for (struct cmsghdr* cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm)) {
      if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_PKTINFO) {
        struct in6_pktinfo pi;
        memcpy(&pi, CMSG_DATA(cm), sizeof pi);
        const uint8_t* x = pi.ipi6_addr.s6_addr;
        snprintf(desc, sizeof desc, "IPV6_PKTINFO len=%zu ifindex=%d addr=%02x%02x:..:%02x%02x:%d.%d.%d.%d", (size_t)cm->cmsg_len,
                 pi.ipi6_ifindex, x[0], x[1], x[10], x[11], x[12], x[13], x[14], x[15]);
      }
    }
    m.msg_controllen = sizeof ctl;
    m.msg_namelen = sizeof from6;
  }
  check("IPv4 into dual-stack IPv6, IPV6_RECVPKTINFO", "%ld datagrams, %s, from family=%d %02x%02x", got, desc, from6.sin6_family,
        from6.sin6_addr.s6_addr[10], from6.sin6_addr.s6_addr[11]);
  close(rx6);
  close(tx2);

  int dead = udp4_bound();
  struct sockaddr_in dead_to = loop4(port_of(dead));
  close(dead);
  int un = udp4_bound();
  sendto(un, a, sizeof a, 0, (struct sockaddr*)&dead_to, sizeof dead_to);
  usleep(50000);
  struct pollfd p = {un, POLLIN, 0};
  long pr = K(poll(&p, 1, 50));
  n = K(recv(un, big, sizeof big, MSG_DONTWAIT));
  long s2 = K(sendto(un, a, sizeof a, 0, (struct sockaddr*)&dead_to, sizeof dead_to));
  check("unconnected UDP to a closed port", "poll %ld [%s], recv %s, send again %s", pr, poll_bits(p.revents), res(n), res(s2));
  close(un);
  int cn = udp4_bound();
  connect(cn, (struct sockaddr*)&dead_to, sizeof dead_to);
  send(cn, a, sizeof a, 0);
  usleep(50000);
  p.fd = cn;
  p.events = POLLIN;
  pr = K(poll(&p, 1, 50));
  n = K(recv(cn, big, sizeof big, MSG_DONTWAIT));
  long n2 = K(recv(cn, big, sizeof big, MSG_DONTWAIT));
  check("connected UDP to a closed port", "poll %ld [%s], recv %s, again %s, SO_ERROR %s", pr, poll_bits(p.revents), res(n),
        res(n2), opt(cn, SOL_SOCKET, SO_ERROR));
  send(cn, a, sizeof a, 0);
  usleep(50000);
  p.events = POLLIN | POLLOUT;
  pr = K(poll(&p, 1, 0));
  char bits[32];
  snprintf(bits, sizeof bits, "%s", poll_bits(p.revents));
  int inq = -1;
  long ir = K(ioctl(cn, FIONREAD, &inq));
  p.events = POLLIN;
  long pr2 = K(poll(&p, 1, 0));
  check("closed port: poll IN|OUT, FIONREAD, poll IN", "%ld [%s], %s %d, %ld [%s]", pr, bits, res(ir), inq, pr2, poll_bits(p.revents));
  const char* soerr = opt(cn, SOL_SOCKET, SO_ERROR);
  pr = K(poll(&p, 1, 0));
  n = K(recv(cn, big, sizeof big, MSG_DONTWAIT));
  check("closed port: SO_ERROR takes the error", "SO_ERROR %s, then poll %ld [%s], recv %s", soerr, pr, poll_bits(p.revents), res(n));
  send(cn, a, sizeof a, 0);
  usleep(50000);
  pr = K(poll(&p, 1, 0));
  snprintf(bits, sizeof bits, "%s", poll_bits(p.revents));
  long s3 = K(send(cn, a, sizeof a, 0));
  usleep(50000);
  n = K(recv(cn, big, sizeof big, MSG_DONTWAIT));
  check("closed port: send after poll takes the error", "poll %ld [%s], send %s, then recv %s", pr, bits, res(s3), res(n));
  send(cn, a, sizeof a, 0);
  usleep(50000);
  long pk = K(recv(cn, big, sizeof big, MSG_PEEK | MSG_DONTWAIT));
  n = K(recv(cn, big, sizeof big, MSG_DONTWAIT));
  check("closed port: recv(MSG_PEEK) takes the error", "%s, then recv %s", res(pk), res(n));
  int ep = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event ev = {EPOLLIN, {.fd = cn}}, evs[2];
  epoll_ctl(ep, EPOLL_CTL_ADD, cn, &ev);
  send(cn, a, sizeof a, 0);
  usleep(50000);
  long en = K(epoll_wait(ep, evs, 2, 0));
  unsigned eev = en > 0 ? evs[0].events : 0;
  fd_set rs;
  FD_ZERO(&rs);
  FD_SET(cn, &rs);
  struct timeval zero = {0, 0};
  long sr = K(select(cn + 1, &rs, NULL, NULL, &zero));
  int in_rs = FD_ISSET(cn, &rs);
  n = K(recv(cn, big, sizeof big, MSG_DONTWAIT));
  long en2 = K(epoll_wait(ep, evs, 2, 0));
  check("closed port: epoll EPOLLIN, select", "%ld event(s) 0x%x, select %ld read %d, recv %s, then epoll %ld", en, eev, sr, in_rs,
        res(n), en2);
  close(ep);
  int peer = udp4_bound(), mm = udp4_bound();
  struct sockaddr_in peer_to = loop4(port_of(peer)), mm_to = loop4(port_of(mm));
  connect(mm, (struct sockaddr*)&peer_to, sizeof peer_to);
  sendto(peer, a, sizeof a, 0, (struct sockaddr*)&mm_to, sizeof mm_to);
  sendto(peer, a, sizeof a, 0, (struct sockaddr*)&mm_to, sizeof mm_to);
  wait_in(mm, 1000);
  usleep(20000);
  close(peer);
  send(mm, a, sizeof a, 0);
  usleep(50000);
  int datagrams = 0, errors = 0;
  char errname[32] = "none";
  long mr = 0;
  for (int k = 0; k < 3; k++) {
    struct mmsghdr mv[4];
    struct iovec mio[4];
    char mbuf[4][64];
    memset(mv, 0, sizeof mv);
    for (int i = 0; i < 4; i++) {
      mio[i].iov_base = mbuf[i];
      mio[i].iov_len = sizeof mbuf[i];
      mv[i].msg_hdr.msg_iov = &mio[i];
      mv[i].msg_hdr.msg_iovlen = 1;
    }
    mr = K(recvmmsg(mm, mv, 4, MSG_DONTWAIT, NULL));
    if (mr >= 0) {
      datagrams += (int)mr;
    } else if (mr != -1000 - EAGAIN) {
      errors++;
      snprintf(errname, sizeof errname, "%s", res(mr));
    }
  }
  check("recvmmsg, 2 datagrams and an ICMP error", "%d datagrams, %d error(s) %s, then %s", datagrams, errors, errname, res(mr));
  close(mm);
  struct sockaddr unspec;
  memset(&unspec, 0, sizeof unspec);
  unspec.sa_family = AF_UNSPEC;
  r = K(connect(cn, &unspec, sizeof unspec));
  socklen_t pl = sizeof from;
  long gp = K(getpeername(cn, (struct sockaddr*)&from, &pl));
  check("connect(AF_UNSPEC) on UDP", "%s, getpeername %s", res(r), res(gp));
  close(cn);

  int blk = udp4_bound();
  n = K(recv(blk, big, sizeof big, MSG_DONTWAIT));
  check("blocking UDP recv(MSG_DONTWAIT), nothing", "%s", res(n));
  struct timeval tv = {0, 150000};
  setsockopt(blk, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  int64_t t0 = now_ns();
  n = K(recv(blk, big, sizeof big, 0));
  int64_t ms = (now_ns() - t0) / 1000000;
  check("blocking UDP recv, SO_RCVTIMEO 150 ms", "%s after %s", res(n), ms >= 140 && ms < 400 ? "~150 ms" : "the wrong time");
  timing("blocking UDP recv, SO_RCVTIMEO 150 ms", "%lld ms", (long long)ms);
  close(blk);

  int seg = socket(AF_INET, SOCK_DGRAM, 0);
  set_int(seg, SOL_UDP, UDP_SEGMENT, 1200);
  static char payload[5000];
  r = K(sendto(seg, payload, sizeof payload, 0, (struct sockaddr*)&to, sizeof to));
  char sizes[128] = "";
  for (int k = 0; k < 10 && wait_in(rx, 200) > 0; k++) {
    n = K(recv(rx, big, sizeof big, MSG_DONTWAIT));
    if (n < 0) break;
    char one[16];
    snprintf(one, sizeof one, "%ld ", n);
    strcat(sizes, one);
  }
  check("UDP_SEGMENT 5000 as 1200", "sent %s: %s", res(r), sizes);
  close(seg);
  close(tx);
  close(rx);
}

static int tcp_pair(int* client, int* server, int nonblocking_listener) {
  int l = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = loop4(0);
  bind(l, (struct sockaddr*)&a, sizeof a);
  listen(l, 4);
  a = loop4(port_of(l));
  *client = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(*client, (struct sockaddr*)&a, sizeof a) != 0) return -1;
  if (nonblocking_listener) {
    fcntl(l, F_SETFL, fcntl(l, F_GETFL) | O_NONBLOCK);
    wait_in(l, 1000);
  }
  *server = accept4(l, NULL, NULL, 0);
  close(l);
  return *server < 0 ? -1 : 0;
}

static short poll_now(int fd, short events) {
  struct pollfd p = {fd, events, 0};
  poll(&p, 1, 200);
  return p.revents;
}

static void streams(void) {
  int c, s;
  if (tcp_pair(&c, &s, 1) != 0) {
    check("tcp pair", "failed %s", err_name(errno));
    return;
  }
  check("accept4 from a non-blocking listener", "O_NONBLOCK %s", (fcntl(s, F_GETFL) & O_NONBLOCK) ? "set" : "clear");
  struct timeval tv = {0, 100000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char buf[256];
  int64_t t0 = now_ns();
  long n = K(recv(s, buf, sizeof buf, 0));
  int64_t ms = (now_ns() - t0) / 1000000;
  check("accepted socket recv, SO_RCVTIMEO 100 ms", "%s after %s", res(n), ms >= 90 && ms < 400 ? "~100 ms" : ms < 10 ? "0 ms" : "the wrong time");

  struct iovec w3[3] = {{"abc", 3}, {"defg", 4}, {"h", 1}};
  long w = K(writev(c, w3, 3));
  wait_in(s, 1000);
  usleep(20000);
  struct sockaddr_in from;
  memset(&from, 0x55, sizeof from);
  socklen_t fl = sizeof from;
  n = K(recvfrom(s, buf, sizeof buf, 0, (struct sockaddr*)&from, &fl));
  check("tcp writev 3 buffers, recvfrom", "%s -> %s [%.*s] addrlen=%u", res(w), res(n), n > 0 ? (int)n : 0, buf, (unsigned)fl);
  send(c, "0123456789", 10, 0);
  send(c, "01234567890123456789", 20, 0);
  wait_in(s, 1000);
  usleep(20000);
  int inq = -1;
  long ir = K(ioctl(s, FIONREAD, &inq));
  recv(s, buf, sizeof buf, 0);
  check("tcp FIONREAD, 10+20 bytes sent", "%s %d", res(ir), inq);

  check("tcp poll, idle", "%s", poll_bits(poll_now(s, POLLIN | POLLOUT | POLLRDHUP)));
  shutdown(c, SHUT_WR);
  usleep(50000);
  check("tcp poll, peer shut down writing", "%s", poll_bits(poll_now(s, POLLIN | POLLOUT | POLLRDHUP)));
  check("tcp poll, peer shut down, POLLIN only", "%s", poll_bits(poll_now(s, POLLIN)));
  n = K(recv(s, buf, sizeof buf, MSG_DONTWAIT));
  check("tcp recv at the peer's end", "%s", res(n));
  shutdown(s, SHUT_WR);
  usleep(50000);
  check("tcp poll, both shut down", "%s", poll_bits(poll_now(s, POLLIN | POLLOUT | POLLRDHUP)));
  close(c);
  close(s);

  if (tcp_pair(&c, &s, 0) == 0) {
    struct linger lg = {1, 0};
    setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(c);  // RST
    usleep(50000);
    check("tcp poll, reset by peer", "%s", poll_bits(poll_now(s, POLLIN | POLLOUT | POLLRDHUP)));
    n = K(recv(s, buf, sizeof buf, MSG_DONTWAIT));
    check("tcp recv, reset by peer", "%s", res(n));
    n = K(send(s, buf, 10, MSG_NOSIGNAL));
    check("tcp send, reset by peer", "%s", res(n));
    close(s);
  }
}

struct arm_later {
  int tfd;
  int delay_ms, fire_ms;
};

static void* arm_thread(void* p) {
  struct arm_later* a = p;
  usleep((useconds_t)a->delay_ms * 1000);
  struct itimerspec its;
  memset(&its, 0, sizeof its);
  its.it_value.tv_nsec = (long)a->fire_ms * 1000000;
  timerfd_settime(a->tfd, 0, &its, NULL);
  return NULL;
}

static int cmp64(const void* x, const void* y) {
  int64_t a = *(const int64_t*)x, b = *(const int64_t*)y;
  return a < b ? -1 : a > b;
}

static void timers(void) {
  int t = timerfd_create(CLOCK_MONOTONIC, 0);
  struct itimerspec its;
  memset(&its, 0, sizeof its);
  its.it_value.tv_nsec = 20 * 1000000;
  timerfd_settime(t, 0, &its, NULL);
  uint64_t x = 0;
  int64_t t0 = now_ns();
  long n = K(read(t, &x, sizeof x));
  int64_t ms = (now_ns() - t0) / 1000000;
  check("timerfd 20 ms, blocking read", "%s, %llu expiration(s) after %s", res(n), (unsigned long long)x, ms >= 19 && ms < 60 ? "~20 ms" : "the wrong time");

  its.it_value.tv_nsec = 5 * 1000000;
  its.it_interval.tv_nsec = 5 * 1000000;
  timerfd_settime(t, 0, &its, NULL);
  usleep(52000);
  n = K(read(t, &x, sizeof x));
  check("timerfd every 5 ms, read after 52 ms", "%s, %s expirations", res(n), x >= 9 && x <= 11 ? "~10" : "the wrong number of");
  struct itimerspec cur;
  timerfd_gettime(t, &cur);
  check("timerfd every 5 ms, gettime", "value %s, interval %ld ms", cur.it_value.tv_nsec > 0 && cur.it_value.tv_nsec <= 5000000 ? "<=5 ms" : "wrong",
        cur.it_interval.tv_nsec / 1000000);
  memset(&its, 0, sizeof its);
  timerfd_settime(t, 0, &its, NULL);
  int nb = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  n = K(read(nb, &x, sizeof x));
  check("timerfd disarmed, non-blocking read", "%s", res(n));
  n = K(read(nb, &x, 4));
  check("timerfd read of 4 bytes", "%s", res(n));
  struct pollfd tp = {nb, POLLIN | POLLOUT, 0};
  its.it_value.tv_nsec = 1000000;
  timerfd_settime(nb, 0, &its, NULL);
  usleep(5000);
  poll(&tp, 1, 100);
  check("timerfd poll IN|OUT after expiry", "%s", poll_bits(tp.revents));

  int ep = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event ev = {EPOLLIN, {.fd = nb}};
  epoll_ctl(ep, EPOLL_CTL_ADD, nb, &ev);
  enum { kRuns = 300 };
  int64_t late[kRuns];
  int fired = 0;
  for (int i = 0; i < kRuns; i++) {
    int64_t want = now_ns() + 1000000 + (i % 5) * 300000;
    want = (want + 199999) / 200000 * 200000;
    struct itimerspec abs;
    memset(&abs, 0, sizeof abs);
    abs.it_value.tv_sec = want / 1000000000;
    abs.it_value.tv_nsec = want % 1000000000;
    timerfd_settime(nb, TFD_TIMER_ABSTIME, &abs, NULL);
    struct epoll_event out;
    int k = epoll_wait(ep, &out, 1, 1000);
    late[i] = now_ns() - want;
    if (k == 1) {
      read(nb, &x, sizeof x);
      fired++;
    }
  }
  qsort(late, kRuns, sizeof late[0], cmp64);
  check("epoll + absolute timerfd, 300 deadlines", "%d fired, %s early", fired, late[0] < 0 ? "some" : "none");
  timing("epoll + timerfd lateness", "p50 %lld us, p90 %lld us, p99 %lld us, max %lld us", (long long)late[kRuns / 2] / 1000,
         (long long)late[kRuns * 9 / 10] / 1000, (long long)late[kRuns * 99 / 100] / 1000, (long long)late[kRuns - 1] / 1000);

  memset(&its, 0, sizeof its);
  timerfd_settime(nb, 0, &its, NULL);
  read(nb, &x, sizeof x);
  struct arm_later al = {nb, 10, 10};
  pthread_t th;
  pthread_create(&th, NULL, arm_thread, &al);
  t0 = now_ns();
  struct epoll_event out;
  int k = epoll_wait(ep, &out, 1, 2000);
  ms = (now_ns() - t0) / 1000000;
  pthread_join(th, NULL);
  check("epoll on a timerfd armed by another thread", "%d event(s) after %s", k, ms >= 18 && ms < 80 ? "~20 ms" : "the wrong time");
  timing("timerfd armed by another thread (10+10 ms)", "%lld ms", (long long)ms);

  read(nb, &x, sizeof x);
  int64_t total = 0;
  for (int i = 0; i < 100; i++) {
    t0 = now_ns();
    epoll_wait(ep, &out, 1, 1);
    total += now_ns() - t0;
  }
  timing("epoll_wait(1 ms) on nothing, average", "%lld us", (long long)total / 100 / 1000);
  int pfd = udp4_bound();
  struct pollfd pp = {pfd, POLLIN, 0};
  total = 0;
  for (int i = 0; i < 100; i++) {
    t0 = now_ns();
    poll(&pp, 1, 2);
    total += now_ns() - t0;
  }
  timing("poll(2 ms) on an idle socket, average", "%lld us", (long long)total / 100 / 1000);

  fd_set rs;
  FD_ZERO(&rs);
  FD_SET(pfd, &rs);
  struct timeval stv = {0, 30000};
  n = K(select(pfd + 1, &rs, NULL, NULL, &stv));
  check("select 30 ms on nothing", "%s, timeout left %ld.%06ld", res(n), (long)stv.tv_sec, (long)stv.tv_usec);
  close(pfd);
  close(ep);
  close(nb);
  close(t);
}

struct echo {
  int fd;
  int count;
};

static void* echo_thread(void* p) {
  struct echo* e = p;
  char buf[1500];
  for (int i = 0; i < e->count;) {
    struct pollfd pf = {e->fd, POLLIN, 0};
    if (poll(&pf, 1, 100) != 1) continue;
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    long n = K(recvfrom(e->fd, buf, 1492, 0, (struct sockaddr*)&from, &fl));
    if (n <= 0) continue;
    sendto(e->fd, buf, (size_t)n, 0, (struct sockaddr*)&from, fl);
    i++;
  }
  return NULL;
}

static void throughput(void) {
  int server = udp4_bound(), client = udp4_bound();
  struct sockaddr_in to = loop4(port_of(server));
  enum { kTrips = 20000 };
  struct echo e = {server, kTrips};
  pthread_t th;
  pthread_create(&th, NULL, echo_thread, &e);
  char buf[1500];
  memset(buf, 7, sizeof buf);
  int64_t t0 = now_ns();
  int lost = 0;
  for (int i = 0; i < kTrips; i++) {
    sendto(client, buf, 200, 0, (struct sockaddr*)&to, sizeof to);
    struct pollfd pf = {client, POLLIN, 0};
    if (poll(&pf, 1, 1000) != 1 || recv(client, buf, sizeof buf, 0) <= 0) lost++;
  }
  int64_t ns = now_ns() - t0;
  pthread_join(th, NULL);
  check("UDP round trips, poll + recvfrom", "%d lost", lost);
  timing("UDP round trip (200 bytes, poll+recvfrom)", "%lld ns", (long long)ns / kTrips);
  close(server);
  close(client);
  server = udp4_bound();
  client = udp4_bound();
  to = loop4(port_of(server));
  connect(client, (struct sockaddr*)&to, sizeof to);
  e.fd = server;
  pthread_create(&th, NULL, echo_thread, &e);
  t0 = now_ns();
  lost = 0;
  for (int i = 0; i < kTrips; i++) {
    send(client, buf, 200, 0);
    struct pollfd pf = {client, POLLIN, 0};
    if (poll(&pf, 1, 1000) != 1 || recv(client, buf, sizeof buf, 0) <= 0) lost++;
  }
  ns = now_ns() - t0;
  pthread_join(th, NULL);
  check("UDP round trips, connected client", "%d lost", lost);
  timing("UDP round trip, connected client", "%lld ns", (long long)ns / kTrips);
  close(server);
  close(client);

  int rx = udp4_bound(), tx = socket(AF_INET, SOCK_DGRAM, 0);
  to = loop4(port_of(rx));
  for (int i = 0; i < 400; i++) sendto(tx, buf, 1200, 0, (struct sockaddr*)&to, sizeof to);
  usleep(100000);
  int kept = 0;
  while (recv(rx, buf, sizeof buf, MSG_DONTWAIT) > 0) kept++;
  timing("burst of 400 x 1200 B, default buffer, kept", "%d", kept);
  set_int(rx, SOL_SOCKET, SO_RCVBUF, 262144);
  for (int i = 0; i < 400; i++) sendto(tx, buf, 1200, 0, (struct sockaddr*)&to, sizeof to);
  usleep(100000);
  kept = 0;
  while (recv(rx, buf, sizeof buf, MSG_DONTWAIT) > 0) kept++;
  timing("burst of 400 x 1200 B, SO_RCVBUF 256 KB, kept", "%d", kept);

  fcntl(rx, F_SETFL, fcntl(rx, F_GETFL) | O_NONBLOCK);
  enum { kBatch = 16, kBuf = 96000, kSend = 4000 };
  static char bufs[kBatch][kBuf];
  struct iovec iov[kBatch];
  struct mmsghdr mm[kBatch];
  int received = 0;
  int64_t busy = 0;
  for (int sent = 0; sent < kSend;) {
    for (int k = 0; k < 64 && sent < kSend; k++, sent++) sendto(tx, buf, 1200, 0, (struct sockaddr*)&to, sizeof to);
    for (;;) {
      memset(mm, 0, sizeof mm);
      for (int k = 0; k < kBatch; k++) {
        iov[k].iov_base = bufs[k];
        iov[k].iov_len = kBuf;
        mm[k].msg_hdr.msg_iov = &iov[k];
        mm[k].msg_hdr.msg_iovlen = 1;
      }
      int64_t b0 = now_ns();
      int got = recvmmsg(rx, mm, kBatch, 0, NULL);
      busy += now_ns() - b0;
      if (got <= 0) break;
      received += got;
    }
  }
  usleep(20000);
  for (;;) {
    int got = recvmmsg(rx, mm, kBatch, 0, NULL);
    if (got <= 0) break;
    received += got;
  }
  check("recvmmsg 16 x 96000, 4000 datagrams in bursts of 64", "%s", received == kSend ? "all received" : "some lost");
  timing("recvmmsg 16 x 96000: received, time in recvmmsg", "%d of %d, %lld us per datagram", received, kSend,
         (long long)busy / (received ? received : 1) / 1000);
  close(rx);
  close(tx);
}

static long nl_send(int fd, int type, int flags, unsigned seq, int family, int len) {
  struct {
    struct nlmsghdr h;
    struct ifinfomsg m;
  } req;
  memset(&req, 0, sizeof req);
  req.h.nlmsg_len = (unsigned)len;
  req.h.nlmsg_type = (unsigned short)type;
  req.h.nlmsg_flags = (unsigned short)flags;
  req.h.nlmsg_seq = seq;
  req.m.ifi_family = (unsigned char)family;
  return K(send(fd, &req, (size_t)len, 0));
}

static long nl_recv(int fd, char* buf, size_t cap, int ms, int* msg_flags) {
  struct pollfd p = {fd, POLLIN, 0};
  if (poll(&p, 1, ms) <= 0) return 0;
  struct iovec v = {buf, cap};
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_iov = &v;
  m.msg_iovlen = 1;
  long n = K(recvmsg(fd, &m, MSG_DONTWAIT));
  if (msg_flags) *msg_flags = m.msg_flags;
  return n;
}

static void attr_list(char* out, size_t cap, const struct rtattr* a, int len) {
  size_t n = 0;
  out[0] = 0;
  for (; RTA_OK(a, len) && n + 16 < cap; a = RTA_NEXT(a, len)) n += (size_t)snprintf(out + n, cap - n, "%s%u:%u", n ? "," : "", a->rta_type, a->rta_len - 4u);
}

static void describe_addr(char* out, size_t cap, const struct nlmsghdr* h) {
  const struct ifaddrmsg* m = NLMSG_DATA(h);
  char attrs[160], address[64] = "-", local[64] = "-", brd[64] = "-", label[32] = "-", life[16] = "-";
  unsigned flags32 = 0;
  attr_list(attrs, sizeof attrs, IFA_RTA(m), (int)IFA_PAYLOAD(h));
  int len = (int)IFA_PAYLOAD(h);
  for (const struct rtattr* a = IFA_RTA(m); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
    if (a->rta_type == IFA_ADDRESS) inet_ntop(m->ifa_family, RTA_DATA(a), address, sizeof address);
    if (a->rta_type == IFA_LOCAL) inet_ntop(m->ifa_family, RTA_DATA(a), local, sizeof local);
    if (a->rta_type == IFA_BROADCAST) inet_ntop(m->ifa_family, RTA_DATA(a), brd, sizeof brd);
    if (a->rta_type == IFA_LABEL) snprintf(label, sizeof label, "%.*s", (int)RTA_PAYLOAD(a), (const char*)RTA_DATA(a));
    if (a->rta_type == IFA_FLAGS) memcpy(&flags32, RTA_DATA(a), 4);
    if (a->rta_type == IFA_CACHEINFO) {
      struct ifa_cacheinfo ci;
      memcpy(&ci, RTA_DATA(a), sizeof ci);
      snprintf(life, sizeof life, "%s", ci.ifa_valid == 0xffffffffu && ci.ifa_prefered == 0xffffffffu ? "forever" : "finite");
    }
  }
  snprintf(out, cap, "len=%u fam=%u plen=%u flags=0x%x scope=%u attrs=[%s] address=%s local=%s brd=%s label=%s IFA_FLAGS=0x%x life=%s",
           h->nlmsg_len, m->ifa_family, m->ifa_prefixlen, m->ifa_flags, m->ifa_scope, attrs, address, local, brd, label, flags32, life);
}

static void describe_link(char* out, size_t cap, const struct nlmsghdr* h, char* attrs, size_t attrs_cap) {
  const struct ifinfomsg* m = NLMSG_DATA(h);
  char name[32] = "-", qdisc[32] = "-";
  unsigned mtu = 0, txqlen = 0;
  int oper = -1, addrlen = -1, brdlen = -1;
  attr_list(attrs, attrs_cap, IFLA_RTA(m), (int)IFLA_PAYLOAD(h));
  int len = (int)IFLA_PAYLOAD(h);
  for (const struct rtattr* a = IFLA_RTA(m); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
    if (a->rta_type == IFLA_IFNAME) snprintf(name, sizeof name, "%.*s", (int)RTA_PAYLOAD(a), (const char*)RTA_DATA(a));
    if (a->rta_type == IFLA_QDISC) snprintf(qdisc, sizeof qdisc, "%.*s", (int)RTA_PAYLOAD(a), (const char*)RTA_DATA(a));
    if (a->rta_type == IFLA_MTU) memcpy(&mtu, RTA_DATA(a), 4);
    if (a->rta_type == IFLA_TXQLEN) memcpy(&txqlen, RTA_DATA(a), 4);
    if (a->rta_type == IFLA_OPERSTATE) oper = *(const unsigned char*)RTA_DATA(a);
    if (a->rta_type == IFLA_ADDRESS) addrlen = (int)RTA_PAYLOAD(a);
    if (a->rta_type == IFLA_BROADCAST) brdlen = (int)RTA_PAYLOAD(a);
  }
  snprintf(out, cap, "type=%u flags=0x%x change=0x%x name=%s mtu=%u txqlen=%u operstate=%d addrlen=%d brdlen=%d qdisc=%s", m->ifi_type,
           m->ifi_flags, m->ifi_change, name, mtu, txqlen, oper, addrlen, brdlen, qdisc);
}

struct dump {
  int datagrams, msgs, done, errors, bad, v4, v6, links, order_ok, truncated;
  int done_len, done_err, done_flags, err_value;
  char shape[96];
  char lo4[400], lo6[400], lolink[300], lolink_attrs[300];
};

static void read_dump(int fd, size_t cap, unsigned seq, int portid, struct dump* d, int print_info) {
  static char buf[65536];
  memset(d, 0, sizeof *d);
  d->order_ok = 1;
  int seen6 = 0;
  for (int k = 0; k < 64 && !d->done && !d->errors; k++) {
    int mf = 0;
    long n = nl_recv(fd, buf, cap, 1000, &mf);
    if (n <= 0) break;
    if (mf & MSG_TRUNC) d->truncated++;
    d->datagrams++;
    char kinds[8] = "";
    long left = n;
    for (struct nlmsghdr* h = (struct nlmsghdr*)buf; NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
      d->msgs++;
      char kind = '?';
      if (h->nlmsg_seq != seq || (int)h->nlmsg_pid != portid) d->bad++;
      if (h->nlmsg_type == NLMSG_DONE) {
        kind = 'D';
        d->done = 1;
        d->done_len = (int)h->nlmsg_len;
        d->done_flags = h->nlmsg_flags;
        memcpy(&d->done_err, NLMSG_DATA(h), 4);
      } else if (h->nlmsg_type == NLMSG_ERROR) {
        kind = 'E';
        d->errors++;
        memcpy(&d->err_value, NLMSG_DATA(h), 4);
      } else {
        if (!(h->nlmsg_flags & NLM_F_MULTI)) d->bad++;
        if (h->nlmsg_type == RTM_NEWADDR) {
          const struct ifaddrmsg* m = NLMSG_DATA(h);
          kind = m->ifa_family == AF_INET6 ? '6' : '4';
          if (kind == '6') seen6 = 1, d->v6++;
          else d->v4++, d->order_ok &= !seen6;
          char desc[400];
          describe_addr(desc, sizeof desc, h);
          if (m->ifa_index == 1 && kind == '4') snprintf(d->lo4, sizeof d->lo4, "%s", desc);
          else if (m->ifa_index == 1) snprintf(d->lo6, sizeof d->lo6, "%s", desc);
          else if (print_info) printf("info  RTM_NEWADDR index=%u %s\n", m->ifa_index, desc);
        } else if (h->nlmsg_type == RTM_NEWLINK) {
          const struct ifinfomsg* m = NLMSG_DATA(h);
          kind = 'L';
          d->links++;
          char desc[300], attrs[300];
          describe_link(desc, sizeof desc, h, attrs, sizeof attrs);
          if (m->ifi_index == 1) {
            snprintf(d->lolink, sizeof d->lolink, "%s", desc);
            snprintf(d->lolink_attrs, sizeof d->lolink_attrs, "%s", attrs);
          } else if (print_info) {
            printf("info  RTM_NEWLINK index=%d %s attrs=[%s]\n", m->ifi_index, desc, attrs);
          }
        }
      }
      if (!strchr(kinds, kind) && strlen(kinds) < sizeof kinds - 1) strncat(kinds, &kind, 1);
    }
    size_t s = strlen(d->shape);
    if (s + 2 < sizeof d->shape) {
      d->shape[s] = strlen(kinds) == 1 ? kinds[0] : 'M';
      d->shape[s + 1] = 0;
    }
  }
}

static int nl_portid(int fd) {
  struct sockaddr_nl a;
  socklen_t len = sizeof a;
  memset(&a, 0, sizeof a);
  getsockname(fd, (struct sockaddr*)&a, &len);
  return (int)a.nl_pid;
}

static const char* nl_answer(int fd) {
  static char out[128];
  char buf[512];
  long n = nl_recv(fd, buf, sizeof buf, 300, NULL);
  if (n <= 0) return n == 0 ? "none" : res(n);
  struct nlmsghdr* h = (struct nlmsghdr*)buf;
  int err = 0;
  if (h->nlmsg_type == NLMSG_ERROR) memcpy(&err, NLMSG_DATA(h), 4);
  snprintf(out, sizeof out, "%ld bytes: type=%u len=%u flags=0x%x seq=%u%s err=%d", n, h->nlmsg_type, h->nlmsg_len, h->nlmsg_flags,
           h->nlmsg_seq, (int)h->nlmsg_pid == nl_portid(fd) ? " pid=port" : " pid=other", err);
  return out;
}

static void netlink(void) {
  long r;
  check("netlink socket SOCK_RAW", "%s", res(r = K(socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE))) ), close((int)r);
  check("netlink socket SOCK_DGRAM", "%s", res(r = K(socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE))) ), close((int)r);
  check("netlink socket SOCK_STREAM", "%s", res(K(socket(AF_NETLINK, SOCK_STREAM, NETLINK_ROUTE))));
  check("netlink socket protocol 99", "%s", res(K(socket(AF_NETLINK, SOCK_RAW, 99))));

  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  struct sockaddr_nl sa;
  socklen_t sl = sizeof sa;
  memset(&sa, 0x55, sizeof sa);
  r = K(getsockname(fd, (struct sockaddr*)&sa, &sl));
  check("netlink getsockname before sending", "%s len=%u family=%u pid=%u groups=%u", res(r), (unsigned)sl, sa.nl_family, sa.nl_pid,
        sa.nl_groups);
  r = nl_send(fd, RTM_GETADDR, NLM_F_REQUEST | NLM_F_ROOT, 0, AF_UNSPEC, NLMSG_LENGTH(sizeof(struct ifaddrmsg)));
  int port = nl_portid(fd);
  check("netlink pid after the first send", "%s, pid %s", res(r), port == getpid() ? "= getpid()" : port == 0 ? "0" : "other");
  struct pollfd p = {fd, POLLIN | POLLOUT, 0};
  poll(&p, 1, 1000);
  check("netlink poll after the request", "%s", poll_bits(p.revents));
  struct dump d;
  read_dump(fd, 4096, 0, port, &d, 1);
  check("RTM_GETADDR dump (WebRTC), 4096-byte recv", "shape %s, done %d len %d flags 0x%x err %d, bad %d, v4 before v6 %s, cut %d", d.shape,
        d.done, d.done_len, d.done_flags, d.done_err, d.bad, d.order_ok ? "yes" : "no", d.truncated);
  printf("info  RTM_GETADDR dump: %d datagrams, %d IPv4 + %d IPv6 addresses\n", d.datagrams, d.v4, d.v6);
  check("RTM_NEWADDR lo IPv4", "%s", d.lo4[0] ? d.lo4 : "missing");
  check("RTM_NEWADDR lo IPv6", "%s", d.lo6[0] ? d.lo6 : "missing");
  int mf = 0;
  char tiny[8];
  r = nl_recv(fd, tiny, sizeof tiny, 0, &mf);
  check("netlink recv after the dump, nothing left", "%s", r == 0 ? "nothing" : res(r));

  r = nl_send(fd, RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP, 7, AF_INET, 20);
  read_dump(fd, 8192, 7, port, &d, 0);
  check("RTM_GETADDR dump AF_INET, seq 7", "%s: shape %s, v6 %d, bad %d, done %d", res(r), d.shape, d.v6, d.bad, d.done);
  r = nl_send(fd, RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP, 8, AF_INET6, 20);
  read_dump(fd, 8192, 8, port, &d, 0);
  check("RTM_GETADDR dump AF_INET6, seq 8", "%s: shape %s, v4 %d, bad %d, done %d", res(r), d.shape, d.v4, d.bad, d.done);
  r = nl_send(fd, RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP, 9, AF_UNSPEC, 20);
  read_dump(fd, 8192, 9, port, &d, 1);
  check("RTM_GETLINK dump", "%s: done %d, err %d, bad %d, links %s", res(r), d.done, d.errors ? d.err_value : 0, d.bad, d.links ? ">0" : "0");
  check("RTM_NEWLINK lo", "%s", d.lolink[0] ? d.lolink : "missing");
  printf("info  RTM_NEWLINK lo attributes [%s]; %d links, shape %s\n", d.lolink_attrs, d.links, d.shape);

  nl_send(fd, RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP, 10, AF_UNSPEC, 20);
  char small[64], big[16384];
  struct pollfd q = {fd, POLLIN, 0};
  poll(&q, 1, 1000);
  long whole = K(recv(fd, small, 0, MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT));
  r = nl_recv(fd, small, sizeof small, 1000, &mf);
  struct nlmsghdr* h = (struct nlmsghdr*)small;
  check("netlink recv 64 of a datagram", "%s flags=%s first type=%u, peek|trunc said %s", res(r), (mf & MSG_TRUNC) ? "MSG_TRUNC" : "0",
        r > 0 ? h->nlmsg_type : 0, whole > 64 ? ">64" : res(whole));
  struct sockaddr_nl from;
  memset(&from, 0x55, sizeof from);
  socklen_t fl = sizeof from;
  poll(&q, 1, 1000);
  r = K(recvfrom(fd, big, sizeof big, MSG_DONTWAIT, (struct sockaddr*)&from, &fl));
  check("netlink recvfrom: the sender", "%s len=%u family=%u pid=%u groups=%u", r > 0 ? "ok" : res(r), (unsigned)fl, from.nl_family,
        from.nl_pid, from.nl_groups);
  while (nl_recv(fd, big, sizeof big, 200, NULL) > 0) {
  }

  nl_send(fd, RTM_GETADDR, NLM_F_REQUEST, 21, AF_UNSPEC, NLMSG_LENGTH(sizeof(struct ifaddrmsg)));
  check("RTM_GETADDR without NLM_F_DUMP", "%s", nl_answer(fd));
  r = nl_send(fd, RTM_NEWADDR, NLM_F_REQUEST | NLM_F_ACK, 22, AF_INET, NLMSG_LENGTH(sizeof(struct ifaddrmsg)));
  check("RTM_NEWADDR (needs CAP_NET_ADMIN)", "send %s, %s", res(r), nl_answer(fd));
  r = nl_send(fd, NLMSG_NOOP, NLM_F_REQUEST | NLM_F_ACK, 23, 0, NLMSG_LENGTH(4));
  check("NLMSG_NOOP with NLM_F_ACK", "send %s, %s", res(r), nl_answer(fd));
  r = nl_send(fd, 200, NLM_F_REQUEST, 24, 0, NLMSG_LENGTH(4));
  check("message type 200", "send %s, %s", res(r), nl_answer(fd));
  r = nl_send(fd, RTM_GETROUTE, NLM_F_REQUEST | NLM_F_DUMP, 30, AF_INET, 20);
  read_dump(fd, 8192, 30, port, &d, 0);
  printf("info  RTM_GETROUTE dump AF_INET: send %s, shape %s, err %d\n", res(r), d.shape, d.errors ? d.err_value : 0);
  r = nl_send(fd, RTM_GETNEIGH, NLM_F_REQUEST | NLM_F_DUMP, 31, AF_INET, 20);
  read_dump(fd, 8192, 31, port, &d, 0);
  printf("info  RTM_GETNEIGH dump AF_INET: send %s, shape %s, err %d\n", res(r), d.shape, d.errors ? d.err_value : 0);
  int avail = -1;
  nl_send(fd, RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP, 32, AF_INET, 20);
  poll(&q, 1, 1000);
  r = K(ioctl(fd, FIONREAD, &avail));
  check("netlink FIONREAD after a dump request", "%s %s", res(r), avail > 0 ? ">0" : "0");
  while (nl_recv(fd, big, sizeof big, 200, NULL) > 0) {
  }
  nl_send(fd, RTM_GETADDR, NLM_F_ACK, 25, 0, NLMSG_LENGTH(4));
  check("RTM_GETADDR without NLM_F_REQUEST, NLM_F_ACK", "%s", nl_answer(fd));
  nl_send(fd, RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP | NLM_F_ACK, 26, AF_INET, 20);
  read_dump(fd, 8192, 26, port, &d, 0);
  check("RTM_GETADDR dump with NLM_F_ACK", "shape %s, then %s", d.shape, nl_answer(fd));
  r = K(send(fd, small, 8, 0));
  check("netlink send of 8 bytes", "%s, answer %s", res(r), nl_answer(fd));
  nl_send(fd, RTM_GETLINK, NLM_F_REQUEST, 27, AF_UNSPEC, NLMSG_LENGTH(sizeof(struct ifinfomsg)));
  check("RTM_GETLINK without NLM_F_DUMP, no index", "%s", nl_answer(fd));
  struct {
    struct nlmsghdr h;
    struct ifinfomsg m;
  } one;
  memset(&one, 0, sizeof one);
  one.h.nlmsg_len = sizeof one;
  one.h.nlmsg_type = RTM_GETLINK;
  one.h.nlmsg_flags = NLM_F_REQUEST;
  one.h.nlmsg_seq = 28;
  one.m.ifi_index = 1;
  send(fd, &one, sizeof one, 0);
  r = nl_recv(fd, big, sizeof big, 300, NULL);
  char ldesc[300] = "none", lattrs[300];
  h = (struct nlmsghdr*)big;
  if (r > 0 && h->nlmsg_type == RTM_NEWLINK) describe_link(ldesc, sizeof ldesc, h, lattrs, sizeof lattrs);
  else if (r > 0) snprintf(ldesc, sizeof ldesc, "type %u", h->nlmsg_type);
  check("RTM_GETLINK index 1", "%s flags=0x%x %s", r > 0 ? "ok" : res(r), r > 0 ? h->nlmsg_flags : 0, ldesc);
  one.m.ifi_index = 0x7fff0000;
  one.h.nlmsg_seq = 29;
  send(fd, &one, sizeof one, 0);
  check("RTM_GETLINK unknown index", "%s", nl_answer(fd));
  struct sockaddr_nl to = {AF_NETLINK, 0, 12345, 0};
  r = K(sendto(fd, &one, sizeof one, 0, (struct sockaddr*)&to, sizeof to));
  check("netlink sendto pid 12345 (no such socket)", "%s", res(r));
  to.nl_family = AF_INET;
  to.nl_pid = 0;
  r = K(sendto(fd, &one, sizeof one, 0, (struct sockaddr*)&to, sizeof to));
  check("netlink sendto an AF_INET address", "%s", res(r));

  check("SOL_NETLINK NETLINK_EXT_ACK", "%s -> %s", res(set_int(fd, SOL_NETLINK, NETLINK_EXT_ACK, 1)), opt(fd, SOL_NETLINK, NETLINK_EXT_ACK));
  check("SOL_NETLINK NETLINK_GET_STRICT_CHK default", "%s", opt(fd, SOL_NETLINK, NETLINK_GET_STRICT_CHK));
  check("SOL_NETLINK option 99", "%s / %s", res(set_int(fd, SOL_NETLINK, 99, 1)), opt(fd, SOL_NETLINK, 99));
  check("netlink SO_TYPE / SO_PROTOCOL / SO_DOMAIN", "%s %s %s", opt(fd, SOL_SOCKET, SO_TYPE), opt(fd, SOL_SOCKET, SO_PROTOCOL),
        opt(fd, SOL_SOCKET, SO_DOMAIN));
  check("netlink listen", "%s", res(K(listen(fd, 1))));
  check("netlink shutdown", "%s", res(K(shutdown(fd, SHUT_RDWR))));
  r = K(recv(fd, small, sizeof small, MSG_DONTWAIT));
  check("netlink recv, nothing queued", "%s", res(r));
  int second = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
  struct sockaddr_nl b = {AF_NETLINK, 0, 0, 0};
  r = K(bind(second, (struct sockaddr*)&b, sizeof b));
  int p2 = nl_portid(second);
  check("second netlink socket bind(pid 0)", "%s, pid %s", res(r), p2 == getpid() ? "= getpid()" : p2 < 0 ? "negative" : p2 == 0 ? "0" : "other");
  b.nl_pid = (unsigned)port;
  int third = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
  check("netlink bind to a pid in use", "%s", res(K(bind(third, (struct sockaddr*)&b, sizeof b))));
  b.nl_pid = 0;
  b.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
  r = K(bind(third, (struct sockaddr*)&b, sizeof b));
  memset(&sa, 0, sizeof sa);
  sl = sizeof sa;
  getsockname(third, (struct sockaddr*)&sa, &sl);
  check("netlink bind with groups", "%s, groups 0x%x", res(r), sa.nl_groups);
  r = K(recv(third, small, sizeof small, MSG_DONTWAIT));
  check("netlink group socket, no change happened", "%s", res(r));
  close(third);
  close(second);
  close(fd);
}

static int if_req(int fd, unsigned long req, const char* name, struct ifreq* ifr) {
  memset(ifr, 0, sizeof *ifr);
  if (name) snprintf(ifr->ifr_name, sizeof ifr->ifr_name, "%s", name);
  return (int)K(ioctl(fd, req, ifr));
}

static const char* sin_str(const struct sockaddr* sa) {
  char* b = rotating();
  const struct sockaddr_in* s = (const struct sockaddr_in*)sa;
  char a[32];
  inet_ntop(AF_INET, &s->sin_addr, a, sizeof a);
  snprintf(b, 64, "fam=%u %s", s->sin_family, a);
  return b;
}

static int avail_dummy;

static void ifioctls(void) {
  int u4 = socket(AF_INET, SOCK_DGRAM, 0), u6 = socket(AF_INET6, SOCK_DGRAM, 0), un = socket(AF_UNIX, SOCK_STREAM, 0);
  struct ifreq ifr;
  long r;
  struct ifconf ifc;
  memset(&ifc, 0, sizeof ifc);
  r = K(ioctl(u4, SIOCGIFCONF, &ifc));
  check("SIOCGIFCONF, no buffer", "%s, length %s", res(r), ifc.ifc_len > 0 && ifc.ifc_len % (int)sizeof(struct ifreq) == 0 ? "n * 40" : "wrong");
  int want = ifc.ifc_len;
  static char space[400];
  memset(space, 0, sizeof space);
  ifc.ifc_len = sizeof space;
  ifc.ifc_buf = space;
  r = K(ioctl(u4, SIOCGIFCONF, &ifc));
  struct ifreq* first = (struct ifreq*)space;
  check("SIOCGIFCONF 400 bytes", "%s, length %s, first %s %s", res(r), ifc.ifc_len == (want < 400 ? want : 400 / 40 * 40) ? "as asked" : "wrong",
        first->ifr_name, sin_str(&first->ifr_addr));
  for (int i = 0; i < ifc.ifc_len / (int)sizeof(struct ifreq); i++) {
    struct ifreq* e = (struct ifreq*)space + i;
    struct ifreq a = *e;
    long ar = K(ioctl(u4, SIOCGIFADDR, &a));
    printf("info  SIOCGIFCONF %-12s %s, SIOCGIFADDR %s %s\n", e->ifr_name, sin_str(&e->ifr_addr), res(ar), sin_str(&a.ifr_addr));
  }
  int tail_clean = 1;
  for (int i = ifc.ifc_len; i < (int)sizeof space; i++) tail_clean &= space[i] == 0;
  check("SIOCGIFCONF 400 bytes, past ifc_len", "%s", tail_clean ? "untouched" : "written");
  ifc.ifc_len = 39;
  r = K(ioctl(u4, SIOCGIFCONF, &ifc));
  check("SIOCGIFCONF 39 bytes", "%s, length %d", res(r), ifc.ifc_len);
  ifc.ifc_len = sizeof space;
  r = K(ioctl(u6, SIOCGIFCONF, &ifc));
  check("SIOCGIFCONF on an IPv6 socket", "%s, first %s", res(r), first->ifr_name);

  r = if_req(u4, SIOCGIFADDR, "lo", &ifr);
  check("SIOCGIFADDR lo", "%s %s", res(r), sin_str(&ifr.ifr_addr));
  r = if_req(u4, SIOCGIFNETMASK, "lo", &ifr);
  check("SIOCGIFNETMASK lo", "%s %s", res(r), sin_str(&ifr.ifr_netmask));
  r = if_req(u4, SIOCGIFBRDADDR, "lo", &ifr);
  check("SIOCGIFBRDADDR lo", "%s %s", res(r), sin_str(&ifr.ifr_broadaddr));
  r = if_req(u4, SIOCGIFDSTADDR, "lo", &ifr);
  check("SIOCGIFDSTADDR lo", "%s %s", res(r), sin_str(&ifr.ifr_dstaddr));
  r = if_req(u4, SIOCGIFFLAGS, "lo", &ifr);
  check("SIOCGIFFLAGS lo", "%s 0x%x", res(r), (unsigned short)ifr.ifr_flags);
  r = if_req(u4, SIOCGIFMTU, "lo", &ifr);
  check("SIOCGIFMTU lo", "%s %d", res(r), ifr.ifr_mtu);
  r = if_req(u4, SIOCGIFINDEX, "lo", &ifr);
  check("SIOCGIFINDEX lo", "%s %d", res(r), ifr.ifr_ifindex);
  r = if_req(u4, SIOCGIFTXQLEN, "lo", &ifr);
  check("SIOCGIFTXQLEN lo", "%s %d", res(r), ifr.ifr_qlen);
  r = if_req(u4, SIOCGIFMETRIC, "lo", &ifr);
  check("SIOCGIFMETRIC lo", "%s %d", res(r), ifr.ifr_metric);
  r = if_req(u4, SIOCGIFHWADDR, "lo", &ifr);
  check("SIOCGIFHWADDR lo", "%s family %u %02x%02x%02x%02x%02x%02x", res(r), ifr.ifr_hwaddr.sa_family, (unsigned char)ifr.ifr_hwaddr.sa_data[0],
        (unsigned char)ifr.ifr_hwaddr.sa_data[1], (unsigned char)ifr.ifr_hwaddr.sa_data[2], (unsigned char)ifr.ifr_hwaddr.sa_data[3],
        (unsigned char)ifr.ifr_hwaddr.sa_data[4], (unsigned char)ifr.ifr_hwaddr.sa_data[5]);
  memset(&ifr, 0, sizeof ifr);
  ifr.ifr_ifindex = 1;
  r = K(ioctl(u4, SIOCGIFNAME, &ifr));
  check("SIOCGIFNAME 1", "%s %s", res(r), ifr.ifr_name);
  memset(&ifr, 0, sizeof ifr);
  ifr.ifr_ifindex = 0x7fff0000;
  check("SIOCGIFNAME unknown index", "%s", res(K(ioctl(u4, SIOCGIFNAME, &ifr))));
  check("SIOCGIFINDEX nope0", "%s", res(if_req(u4, SIOCGIFINDEX, "nope0", &ifr)));
  check("SIOCGIFFLAGS nope0", "%s", res(if_req(u4, SIOCGIFFLAGS, "nope0", &ifr)));
  check("SIOCGIFADDR nope0", "%s", res(if_req(u4, SIOCGIFADDR, "nope0", &ifr)));
  r = if_req(u4, SIOCGIFFLAGS, "lo:1", &ifr);
  check("SIOCGIFFLAGS lo:1", "%s 0x%x", res(r), (unsigned short)ifr.ifr_flags);
  check("SIOCGIFADDR lo:1", "%s", res(if_req(u4, SIOCGIFADDR, "lo:1", &ifr)));
  r = if_req(u6, SIOCGIFFLAGS, "lo", &ifr);
  check("SIOCGIFFLAGS lo, IPv6 socket", "%s 0x%x", res(r), (unsigned short)ifr.ifr_flags);
  check("SIOCGIFADDR lo, IPv6 socket", "%s", res(if_req(u6, SIOCGIFADDR, "lo", &ifr)));
  check("ioctl 0x89ff on a socket", "%s", res(if_req(u4, 0x89ff, "lo", &ifr)));
  r = if_req(un, SIOCGIFINDEX, "lo", &ifr);
  printf("info  SIOCGIFINDEX / SIOCGIFADDR lo on AF_UNIX, ioctl 0x1234 on UDP: %s %s %s\n", res(r), res(if_req(un, SIOCGIFADDR, "lo", &ifr)),
         res(if_req(u4, 0x1234, "lo", &ifr)));
  errno = 0;
  int tty = isatty(u4);
  printf("info  isatty(udp socket) %d %s, SIOCGSTAMP %s, TIOCOUTQ %s\n", tty, err_name(errno), res(K(ioctl(u4, SIOCGSTAMP, &ifr))),
         res(K(ioctl(u4, TIOCOUTQ, &avail_dummy))));

  char name[IF_NAMESIZE] = "";
  errno = 0;
  unsigned idx = if_nametoindex("lo");
  check("if_nametoindex lo / if_indextoname 1", "%u / %s", idx, if_indextoname(1, name) ? name : err_name(errno));
  errno = 0;
  idx = if_nametoindex("nope0");
  check("if_nametoindex nope0", "%u %s", idx, err_name(errno));

  struct ifaddrs* list = NULL;
  r = K(getifaddrs(&list));
  int n = 0, lo6 = 0, packet = 0;
  char lodesc[128] = "missing";
  for (struct ifaddrs* a = list; a; a = a->ifa_next) {
    n++;
    int fam = a->ifa_addr ? a->ifa_addr->sa_family : -1;
    if (fam == AF_PACKET) packet++;
    if (strcmp(a->ifa_name, "lo") == 0 && fam == AF_INET) {
      char ad[32], mask[32];
      inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, ad, sizeof ad);
      inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_netmask)->sin_addr, mask, sizeof mask);
      snprintf(lodesc, sizeof lodesc, "%s/%s UP|LOOPBACK|RUNNING %s", ad, mask,
               (a->ifa_flags & (IFF_UP | IFF_LOOPBACK | IFF_RUNNING)) == (IFF_UP | IFF_LOOPBACK | IFF_RUNNING) ? "set" : "missing");
    }
    if (strcmp(a->ifa_name, "lo") == 0 && fam == AF_INET6) lo6++;
    char ad[64] = "-";
    if (fam == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, ad, sizeof ad);
    if (fam == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6*)a->ifa_addr)->sin6_addr, ad, sizeof ad);
    printf("info  getifaddrs %-12s family %2d flags 0x%05x %s\n", a->ifa_name, fam, a->ifa_flags, ad);
  }
  check("getifaddrs lo", "%s: inet %s, %d inet6", res(r), lodesc, lo6);
  printf("info  getifaddrs: %d entries, %d AF_PACKET (uid %d)\n", n, packet, (int)getuid());
  if (list) freeifaddrs(list);
  close(u4);
  close(u6);
  close(un);
}

static long webrtc_getifaddrs(char* lo_name) {
  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (fd < 0) return -1000 - errno;
  struct {
    struct nlmsghdr h;
    struct ifaddrmsg m;
  } req;
  memset(&req, 0, sizeof req);
  req.h.nlmsg_flags = NLM_F_ROOT | NLM_F_REQUEST;
  req.h.nlmsg_type = RTM_GETADDR;
  req.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
  if (sendto(fd, &req, req.h.nlmsg_len, 0, NULL, 0) != (ssize_t)req.h.nlmsg_len) {
    long e = -1000 - errno;
    close(fd);
    return e;
  }
  static char buf[4096];
  long found = 0;
  for (int k = 0; k < 64; k++) {
    if (wait_in(fd, 1000) <= 0) break;
    long amount = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
    if (amount <= 0) break;
    for (struct nlmsghdr* h = (struct nlmsghdr*)buf; NLMSG_OK(h, amount); h = NLMSG_NEXT(h, amount)) {
      if (h->nlmsg_type == NLMSG_DONE) {
        close(fd);
        return found;
      }
      if (h->nlmsg_type == NLMSG_ERROR) {
        close(fd);
        return -2;
      }
      if (h->nlmsg_type != RTM_NEWADDR) continue;
      struct ifaddrmsg* m = NLMSG_DATA(h);
      int plen = (int)IFA_PAYLOAD(h);
      for (struct rtattr* a = IFA_RTA(m); RTA_OK(a, plen); a = RTA_NEXT(a, plen)) {
        if (!((m->ifa_family == AF_INET && a->rta_type == IFA_LOCAL) || (m->ifa_family == AF_INET6 && a->rta_type == IFA_ADDRESS))) continue;
        char name[IF_NAMESIZE] = "";
        if (!if_indextoname(m->ifa_index, name)) {
          close(fd);
          return -1000 - errno;
        }
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
        int r = ioctl(s, SIOCGIFFLAGS, &ifr);
        int e = errno;
        close(s);
        if (r == -1) {
          close(fd);
          return -1000 - e;
        }
        if (m->ifa_index == 1 && m->ifa_family == AF_INET) snprintf(lo_name, IF_NAMESIZE, "%s 0x%x", name, (unsigned short)ifr.ifr_flags);
        found++;
      }
    }
  }
  close(fd);
  return -3;
}

static const char* my_network_address(void) {
  static char out[96];
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return "failed on socket";
  static char space[400];
  memset(space, 0, sizeof space);
  struct ifconf ifc;
  ifc.ifc_len = sizeof space;
  ifc.ifc_buf = space;
  if (ioctl(fd, SIOCGIFCONF, &ifc) == -1) {
    snprintf(out, sizeof out, "failed on ioctl(SIOCGIFCONF) errno %s", err_name(errno));
    close(fd);
    return out;
  }
  snprintf(out, sizeof out, "no address");
  for (int i = 0; i < 10; i++) {
    struct ifreq* r = (struct ifreq*)space + i;
    if (r->ifr_addr.sa_family != AF_INET) continue;
    if (ioctl(fd, SIOCGIFADDR, r) == -1) {
      snprintf(out, sizeof out, "failed on ioctl(SIOCGIFADDR) errno %s", err_name(errno));
      continue;
    }
    char host[1025];
    if (getnameinfo(&r->ifr_addr, sizeof(struct sockaddr_in), host, sizeof host, NULL, 0, NI_NUMERICHOST) != 0) continue;
    if (strcmp(host, "127.0.0.1") == 0) continue;
    snprintf(out, sizeof out, "an address (%s)", r->ifr_name);
    break;
  }
  close(fd);
  return out;
}

static void roblox(void) {
  char lo[IF_NAMESIZE + 16] = "missing";
  long n = webrtc_getifaddrs(lo);
  check("WebRTC getifaddrs (ifaddrs_android.cc)", "%s, lo %s", n >= 0 ? "ok" : n == -2 ? "NLMSG_ERROR" : n == -3 ? "no NLMSG_DONE" : res(n), lo);
  printf("info  WebRTC getifaddrs: %ld addresses\n", n);
  const char* addr = my_network_address();
  check("BandwidthTracker GetMyNetworkAddress", "%s", strncmp(addr, "an address", 10) == 0 ? "an address" : addr);
  printf("info  GetMyNetworkAddress: %s\n", addr);
}

static const char* cmsgs(struct msghdr* m, int64_t* ts_us) {
  static char b[160];
  b[0] = 0;
  for (struct cmsghdr* c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
    char one[32];
    snprintf(one, sizeof one, "%s%d/%d:%zu", b[0] ? "," : "", c->cmsg_level, c->cmsg_type, (size_t)c->cmsg_len);
    strncat(b, one, sizeof b - strlen(b) - 1);
    if (ts_us && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP && c->cmsg_len >= CMSG_LEN(sizeof(struct timeval))) {
      struct timeval tv;
      memcpy(&tv, CMSG_DATA(c), sizeof tv);
      *ts_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    }
    if (ts_us && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS && c->cmsg_len >= CMSG_LEN(sizeof(struct timespec))) {
      struct timespec ts;
      memcpy(&ts, CMSG_DATA(c), sizeof ts);
      *ts_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    }
  }
  return b[0] ? b : "none";
}

static int tos_of(struct msghdr* m) {
  for (struct cmsghdr* c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
    if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_TOS && c->cmsg_len >= CMSG_LEN(1)) return *CMSG_DATA(c);
    if (c->cmsg_level == SOL_IPV6 && c->cmsg_type == IPV6_TCLASS && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
      int v;
      memcpy(&v, CMSG_DATA(c), sizeof v);
      return v;
    }
  }
  return -1;
}

static long recv_ctl(int fd, size_t room, struct msghdr* m, char* ctl) {
  static char data[2048];
  static struct iovec v;
  v.iov_base = data;
  v.iov_len = sizeof data;
  memset(m, 0, sizeof *m);
  m->msg_iov = &v;
  m->msg_iovlen = 1;
  m->msg_control = room ? ctl : NULL;
  m->msg_controllen = room;
  wait_in(fd, 1000);
  return K(recvmsg(fd, m, MSG_DONTWAIT));
}

static int64_t wall_us(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void sockopts(void) {
  int u4 = socket(AF_INET, SOCK_DGRAM, 0), u6 = socket(AF_INET6, SOCK_DGRAM, 0);
  check("udp4 IP_MTU unconnected", "%s", opt(u4, IPPROTO_IP, IP_MTU));
  check("udp6 IPV6_MTU unconnected", "%s", opt(u6, IPPROTO_IPV6, IPV6_MTU));
  check("udp4 IP_MTU set", "%s", res(set_int(u4, IPPROTO_IP, IP_MTU, 1400)));
  check("udp6 IPV6_MTU set 1000 / 1400", "%s / %s", res(set_int(u6, IPPROTO_IPV6, IPV6_MTU, 1000)), res(set_int(u6, IPPROTO_IPV6, IPV6_MTU, 1400)));
  struct sockaddr_in to4 = loop4(9);
  connect(u4, (struct sockaddr*)&to4, sizeof to4);
  struct sockaddr_in6 to6;
  memset(&to6, 0, sizeof to6);
  to6.sin6_family = AF_INET6;
  to6.sin6_port = htons(9);
  to6.sin6_addr = in6addr_loopback;
  connect(u6, (struct sockaddr*)&to6, sizeof to6);
  check("udp4 IP_MTU connected to 127.0.0.1", "%s", opt(u4, IPPROTO_IP, IP_MTU));
  check("udp6 IPV6_MTU connected to ::1", "%s", opt(u6, IPPROTO_IPV6, IPV6_MTU));
  int c, s;
  if (tcp_pair(&c, &s, 0) == 0) {
    check("tcp4 IP_MTU connected to 127.0.0.1", "%s / accepted %s", opt(c, IPPROTO_IP, IP_MTU), opt(s, IPPROTO_IP, IP_MTU));
    close(c);
    close(s);
  }
  close(u4);
  close(u6);

  u4 = socket(AF_INET, SOCK_DGRAM, 0);
  u6 = socket(AF_INET6, SOCK_DGRAM, 0);
  check("udp6 IPV6_DONTFRAG default", "%s", opt(u6, IPPROTO_IPV6, IPV6_DONTFRAG));
  check("udp6 IPV6_DONTFRAG=1", "%s -> %s", res(set_int(u6, IPPROTO_IPV6, IPV6_DONTFRAG, 1)), opt(u6, IPPROTO_IPV6, IPV6_DONTFRAG));
  check("udp6 IPV6_DONTFRAG=0", "%s -> %s", res(set_int(u6, IPPROTO_IPV6, IPV6_DONTFRAG, 0)), opt(u6, IPPROTO_IPV6, IPV6_DONTFRAG));
  check("udp4 IPV6_DONTFRAG (an IPv4 socket)", "%s", res(set_int(u4, IPPROTO_IPV6, IPV6_DONTFRAG, 1)));
  check("udp6 IPV6_MTU_DISCOVER default", "%s", opt(u6, IPPROTO_IPV6, IPV6_MTU_DISCOVER));
  check("udp6 IPV6_TCLASS=0x88", "%s -> %s", res(set_int(u6, IPPROTO_IPV6, IPV6_TCLASS, 0x88)), opt(u6, IPPROTO_IPV6, IPV6_TCLASS));
  check("udp6 IPV6_TCLASS=-1 / 256", "%s -> %s / %s", res(set_int(u6, IPPROTO_IPV6, IPV6_TCLASS, -1)), opt(u6, IPPROTO_IPV6, IPV6_TCLASS),
        res(set_int(u6, IPPROTO_IPV6, IPV6_TCLASS, 256)));
  check("udp6 IPV6_RECVTCLASS=1", "%s -> %s", res(set_int(u6, IPPROTO_IPV6, IPV6_RECVTCLASS, 1)), opt(u6, IPPROTO_IPV6, IPV6_RECVTCLASS));
  check("udp4 IP_TOS=0xb8", "%s -> %s", res(set_int(u4, IPPROTO_IP, IP_TOS, 0xb8)), opt(u4, IPPROTO_IP, IP_TOS));
  check("udp4 IP_RECVTOS=1", "%s -> %s", res(set_int(u4, IPPROTO_IP, IP_RECVTOS, 1)), opt(u4, IPPROTO_IP, IP_RECVTOS));
  check("udp4 IP_TTL=33", "%s -> %s", res(set_int(u4, IPPROTO_IP, IP_TTL, 33)), opt(u4, IPPROTO_IP, IP_TTL));
  close(u4);
  close(u6);

  int rx = udp4_bound(), tx = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in to = loop4(port_of(rx));
  check("udp4 SO_TIMESTAMP default", "%s", opt(rx, SOL_SOCKET, SO_TIMESTAMP));
  check("udp4 SO_TIMESTAMP=1", "%s -> %s", res(set_int(rx, SOL_SOCKET, SO_TIMESTAMP, 1)), opt(rx, SOL_SOCKET, SO_TIMESTAMP));
  struct msghdr m;
  char ctl[256];
  int64_t ts = 0, before = wall_us();
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  long n = recv_ctl(rx, 56, &m, ctl);
  const char* list = cmsgs(&m, &ts);
  int64_t after = wall_us();
  check("SO_TIMESTAMP recvmsg, 56 bytes of room", "%s controllen=%zu [%s] time %s", res(n), (size_t)m.msg_controllen, list,
        ts >= before - 20000 && ts <= after + 20000 ? "now" : "wrong");
  set_int(rx, IPPROTO_IP, IP_PKTINFO, 1);
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, sizeof ctl, &m, ctl);
  check("SO_TIMESTAMP + IP_PKTINFO", "%s controllen=%zu [%s]", res(n), (size_t)m.msg_controllen, cmsgs(&m, NULL));
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, CMSG_SPACE(sizeof(struct timeval)), &m, ctl);
  check("SO_TIMESTAMP + IP_PKTINFO, room for one", "%s controllen=%zu flags=%s [%s]", res(n), (size_t)m.msg_controllen,
        (m.msg_flags & MSG_CTRUNC) ? "MSG_CTRUNC" : "0", cmsgs(&m, NULL));
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, 0, &m, ctl);
  check("SO_TIMESTAMP, recv without control room", "%s flags=%s", res(n), (m.msg_flags & MSG_CTRUNC) ? "MSG_CTRUNC" : "0");
  set_int(rx, IPPROTO_IP, IP_PKTINFO, 0);
  set_int(rx, SOL_SOCKET, SO_TIMESTAMP, 0);
  check("udp4 SO_TIMESTAMPNS=1", "%s -> %s, SO_TIMESTAMP %s", res(set_int(rx, SOL_SOCKET, SO_TIMESTAMPNS, 1)), opt(rx, SOL_SOCKET, SO_TIMESTAMPNS),
        opt(rx, SOL_SOCKET, SO_TIMESTAMP));
  ts = 0;
  before = wall_us();
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, sizeof ctl, &m, ctl);
  list = cmsgs(&m, &ts);
  after = wall_us();
  check("SO_TIMESTAMPNS recvmsg", "%s [%s] time %s", res(n), list, ts >= before - 20000 && ts <= after + 20000 ? "now" : "wrong");
  close(rx);
  close(tx);

  rx = udp4_bound();
  tx = socket(AF_INET, SOCK_DGRAM, 0);
  to = loop4(port_of(rx));
  set_int(rx, IPPROTO_IP, IP_RECVTOS, 1);
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, sizeof ctl, &m, ctl);
  check("IP_RECVTOS recvmsg", "%s controllen=%zu [%s] tos %d", res(n), (size_t)m.msg_controllen, cmsgs(&m, NULL), tos_of(&m));
  set_int(rx, SOL_SOCKET, SO_TIMESTAMP, 1);
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, 56, &m, ctl);
  check("IP_RECVTOS + SO_TIMESTAMP, 56 bytes (WebRTC)", "%s controllen=%zu flags=%s [%s]", res(n), (size_t)m.msg_controllen,
        (m.msg_flags & MSG_CTRUNC) ? "MSG_CTRUNC" : "0", cmsgs(&m, NULL));
  set_int(rx, IPPROTO_IP, IP_PKTINFO, 1);
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, sizeof ctl, &m, ctl);
  check("IP_RECVTOS + SO_TIMESTAMP + IP_PKTINFO", "%s controllen=%zu [%s]", res(n), (size_t)m.msg_controllen, cmsgs(&m, NULL));
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, 56, &m, ctl);
  check("IP_RECVTOS + SO_TIMESTAMP + IP_PKTINFO, 56 bytes", "%s controllen=%zu flags=%s [%s]", res(n), (size_t)m.msg_controllen,
        (m.msg_flags & MSG_CTRUNC) ? "MSG_CTRUNC" : "0", cmsgs(&m, NULL));
  set_int(rx, SOL_SOCKET, SO_TIMESTAMP, 0);
  set_int(rx, IPPROTO_IP, IP_PKTINFO, 0);
  set_int(tx, IPPROTO_IP, IP_TOS, 0xb8);
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, sizeof ctl, &m, ctl);
  printf("info  IP_RECVTOS, sender IP_TOS 0xb8: %s tos 0x%x\n", res(n), tos_of(&m));
  set_int(tx, IPPROTO_IP, IP_TOS, 0);
  set_int(rx, IPPROTO_IP, IP_RECVTOS, 0);
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to, sizeof to);
  n = recv_ctl(rx, sizeof ctl, &m, ctl);
  check("IP_RECVTOS=0 again", "%s controllen=%zu [%s]", res(n), (size_t)m.msg_controllen, cmsgs(&m, NULL));
  close(rx);

  u6 = socket(AF_INET6, SOCK_DGRAM, 0);
  struct sockaddr_in6 any6;
  memset(&any6, 0, sizeof any6);
  any6.sin6_family = AF_INET6;
  bind(u6, (struct sockaddr*)&any6, sizeof any6);
  int t6 = socket(AF_INET6, SOCK_DGRAM, 0);
  to6.sin6_port = htons((uint16_t)port_of(u6));
  struct sockaddr_in to64 = loop4(port_of(u6));
  set_int(u6, IPPROTO_IPV6, IPV6_RECVTCLASS, 1);
  sendto(t6, "abc", 3, 0, (struct sockaddr*)&to6, sizeof to6);
  n = recv_ctl(u6, sizeof ctl, &m, ctl);
  check("IPV6_RECVTCLASS recvmsg, IPv6", "%s controllen=%zu [%s] tclass %d", res(n), (size_t)m.msg_controllen, cmsgs(&m, NULL), tos_of(&m));
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to64, sizeof to64);
  n = recv_ctl(u6, sizeof ctl, &m, ctl);
  check("IPV6_RECVTCLASS, IPv4 on dual-stack", "%s controllen=%zu [%s]", res(n), (size_t)m.msg_controllen, cmsgs(&m, NULL));
  set_int(u6, IPPROTO_IP, IP_RECVTOS, 1);
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to64, sizeof to64);
  n = recv_ctl(u6, sizeof ctl, &m, ctl);
  char v4list[96];
  snprintf(v4list, sizeof v4list, "%s", cmsgs(&m, NULL));
  sendto(t6, "abc", 3, 0, (struct sockaddr*)&to6, sizeof to6);
  long n6 = recv_ctl(u6, sizeof ctl, &m, ctl);
  check("dual-stack + IP_RECVTOS, IPv4 / IPv6", "%s [%s] / %s [%s]", res(n), v4list, res(n6), cmsgs(&m, NULL));
  set_int(u6, IPPROTO_IPV6, IPV6_RECVTCLASS, 0);
  sendto(t6, "abc", 3, 0, (struct sockaddr*)&to6, sizeof to6);
  n = recv_ctl(u6, sizeof ctl, &m, ctl);
  check("dual-stack, IP_RECVTOS only, IPv6", "%s controllen=%zu [%s]", res(n), (size_t)m.msg_controllen, cmsgs(&m, NULL));
  set_int(u6, IPPROTO_IPV6, IPV6_RECVTCLASS, 1);
  set_int(u6, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1);
  set_int(u6, IPPROTO_IP, IP_PKTINFO, 1);
  sendto(tx, "abc", 3, 0, (struct sockaddr*)&to64, sizeof to64);
  n = recv_ctl(u6, sizeof ctl, &m, ctl);
  snprintf(v4list, sizeof v4list, "%s", cmsgs(&m, NULL));
  sendto(t6, "abc", 3, 0, (struct sockaddr*)&to6, sizeof to6);
  n6 = recv_ctl(u6, sizeof ctl, &m, ctl);
  check("dual-stack, both PKTINFO and TOS options", "IPv4 %s [%s] / IPv6 %s [%s]", res(n), v4list, res(n6), cmsgs(&m, NULL));
  set_int(t6, IPPROTO_IPV6, IPV6_TCLASS, 0x88);
  sendto(t6, "abc", 3, 0, (struct sockaddr*)&to6, sizeof to6);
  n = recv_ctl(u6, sizeof ctl, &m, ctl);
  printf("info  IPV6_RECVTCLASS, sender IPV6_TCLASS 0x88: %s tclass 0x%x\n", res(n), tos_of(&m));
  close(t6);
  close(u6);
  close(tx);

  int t1 = socket(AF_INET, SOCK_STREAM, 0), t2 = socket(AF_INET, SOCK_STREAM, 0);
  check("tcp4 SO_REUSEADDR default / =1", "%s / %s -> %s", opt(t1, SOL_SOCKET, SO_REUSEADDR), res(set_int(t1, SOL_SOCKET, SO_REUSEADDR, 1)),
        opt(t1, SOL_SOCKET, SO_REUSEADDR));
  check("tcp4 SO_REUSEPORT default / =1", "%s / %s -> %s", opt(t2, SOL_SOCKET, SO_REUSEPORT), res(set_int(t2, SOL_SOCKET, SO_REUSEPORT, 1)),
        opt(t2, SOL_SOCKET, SO_REUSEPORT));
  set_int(t2, SOL_SOCKET, SO_REUSEPORT, 0);
  struct sockaddr_in a = loop4(0);
  bind(t1, (struct sockaddr*)&a, sizeof a);
  listen(t1, 4);
  a = loop4(port_of(t1));
  set_int(t2, SOL_SOCKET, SO_REUSEADDR, 1);
  check("tcp4 bind to a listening port, SO_REUSEADDR on both", "%s", res(K(bind(t2, (struct sockaddr*)&a, sizeof a))));
  close(t2);
  t2 = socket(AF_INET, SOCK_STREAM, 0);
  check("tcp4 bind to a listening port, no options", "%s", res(K(bind(t2, (struct sockaddr*)&a, sizeof a))));
  close(t2);
  int cl = socket(AF_INET, SOCK_STREAM, 0);
  connect(cl, (struct sockaddr*)&a, sizeof a);
  int sv = accept(t1, NULL, NULL);
  close(t1);
  close(sv);
  usleep(50000);
  close(cl);
  usleep(50000);
  t2 = socket(AF_INET, SOCK_STREAM, 0);
  printf("info  tcp4 rebind over TIME_WAIT, no options: %s\n", res(K(bind(t2, (struct sockaddr*)&a, sizeof a))));
  close(t2);
  int t3 = socket(AF_INET, SOCK_STREAM, 0);
  set_int(t3, SOL_SOCKET, SO_REUSEADDR, 1);
  check("tcp4 rebind over TIME_WAIT, SO_REUSEADDR", "%s", res(K(bind(t3, (struct sockaddr*)&a, sizeof a))));
  close(t3);

  int r1 = socket(AF_INET, SOCK_DGRAM, 0), r2 = socket(AF_INET, SOCK_DGRAM, 0);
  a = loop4(0);
  bind(r1, (struct sockaddr*)&a, sizeof a);
  a = loop4(port_of(r1));
  set_int(r2, SOL_SOCKET, SO_REUSEADDR, 1);
  check("udp4 bind to a bound port, SO_REUSEADDR on the second only", "%s", res(K(bind(r2, (struct sockaddr*)&a, sizeof a))));
  close(r1);
  close(r2);
  r1 = socket(AF_INET, SOCK_DGRAM, 0);
  r2 = socket(AF_INET, SOCK_DGRAM, 0);
  set_int(r1, SOL_SOCKET, SO_REUSEADDR, 1);
  set_int(r2, SOL_SOCKET, SO_REUSEADDR, 1);
  a = loop4(0);
  bind(r1, (struct sockaddr*)&a, sizeof a);
  a = loop4(port_of(r1));
  check("udp4 bind to a bound port, SO_REUSEADDR on both", "%s", res(K(bind(r2, (struct sockaddr*)&a, sizeof a))));
  close(r1);
  close(r2);
  r1 = socket(AF_INET, SOCK_DGRAM, 0);
  r2 = socket(AF_INET, SOCK_DGRAM, 0);
  set_int(r1, SOL_SOCKET, SO_REUSEPORT, 1);
  set_int(r2, SOL_SOCKET, SO_REUSEPORT, 1);
  a = loop4(0);
  bind(r1, (struct sockaddr*)&a, sizeof a);
  a = loop4(port_of(r1));
  check("udp4 bind to a bound port, SO_REUSEPORT on both", "%s", res(K(bind(r2, (struct sockaddr*)&a, sizeof a))));
  close(r1);
  close(r2);

  int u = socket(AF_INET, SOCK_DGRAM, 0), t = socket(AF_INET, SOCK_STREAM, 0);
  u6 = socket(AF_INET6, SOCK_DGRAM, 0);
  check("udp4 SOL_SOCKET option 77", "%s / %s", res(set_int(u, SOL_SOCKET, 77, 1)), opt(u, SOL_SOCKET, 77));
  // 1000 is samsung's own option, not linux's
  printf("info  udp4 SOL_SOCKET options 100/999/1000/0x7fff set: %s %s %s %s\n", res(set_int(u, SOL_SOCKET, 100, 1)),
         res(set_int(u, SOL_SOCKET, 999, 1)), res(set_int(u, SOL_SOCKET, 1000, 1)), res(set_int(u, SOL_SOCKET, 0x7fff, 1)));
  check("udp4 IPPROTO_IP option 200", "%s / %s", res(set_int(u, IPPROTO_IP, 200, 1)), opt(u, IPPROTO_IP, 200));
  check("udp6 IPPROTO_IPV6 option 200", "%s / %s", res(set_int(u6, IPPROTO_IPV6, 200, 1)),
        opt(u6, IPPROTO_IPV6, 200));
  check("tcp4 IPPROTO_TCP option 200", "%s / %s", res(set_int(t, IPPROTO_TCP, 200, 1)), opt(t, IPPROTO_TCP, 200));
  check("udp4 SOL_UDP option 200", "%s / %s", res(set_int(u, SOL_UDP, 200, 1)), opt(u, SOL_UDP, 200));
  check("udp4 SO_PRIORITY=5 / =7", "%s -> %s / %s", res(set_int(u, SOL_SOCKET, SO_PRIORITY, 5)), opt(u, SOL_SOCKET, SO_PRIORITY),
        res(set_int(u, SOL_SOCKET, SO_PRIORITY, 7)));
  check("udp4 SO_MARK=1", "%s, get %s", res(set_int(u, SOL_SOCKET, SO_MARK, 1)), opt(u, SOL_SOCKET, SO_MARK));
  check("udp4 SO_DEBUG=1 / =0", "%s / %s -> %s", res(set_int(u, SOL_SOCKET, SO_DEBUG, 1)), res(set_int(u, SOL_SOCKET, SO_DEBUG, 0)),
        opt(u, SOL_SOCKET, SO_DEBUG));
  check("udp4 SO_PASSCRED=1", "%s -> %s", res(set_int(u, SOL_SOCKET, SO_PASSCRED, 1)), opt(u, SOL_SOCKET, SO_PASSCRED));
  check("udp4 SO_RCVLOWAT default / =10", "%s / %s -> %s", opt(u, SOL_SOCKET, SO_RCVLOWAT), res(set_int(u, SOL_SOCKET, SO_RCVLOWAT, 10)),
        opt(u, SOL_SOCKET, SO_RCVLOWAT));
  check("udp4 SO_SNDLOWAT get / set", "%s / %s", opt(u, SOL_SOCKET, SO_SNDLOWAT), res(set_int(u, SOL_SOCKET, SO_SNDLOWAT, 10)));
  check("udp4 SO_ERROR / SO_TYPE set", "%s / %s", res(set_int(u, SOL_SOCKET, SO_ERROR, 0)), res(set_int(u, SOL_SOCKET, SO_TYPE, 1)));
  check("udp4 SO_BINDTODEVICE lo", "%s", res(K(setsockopt(u, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3))));
  {
    char dev[IFNAMSIZ] = "";
    socklen_t dl = sizeof dev;
    long br = K(getsockopt(u, SOL_SOCKET, SO_BINDTODEVICE, dev, &dl));
    check("udp4 SO_BINDTODEVICE read back", "%s len=%u %s", res(br), (unsigned)dl, dev);
    check("udp4 SO_BINDTODEVICE nope0 / again lo", "%s / %s", res(K(setsockopt(u, SOL_SOCKET, SO_BINDTODEVICE, "nope0", 6))),
          res(K(setsockopt(u, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3))));
    check("udp4 SO_BINDTODEVICE \"\"", "%s", res(K(setsockopt(u, SOL_SOCKET, SO_BINDTODEVICE, "", 0))));
  }
  check("udp4 SO_NO_CHECK=1", "%s -> %s", res(set_int(u, SOL_SOCKET, SO_NO_CHECK, 1)), opt(u, SOL_SOCKET, SO_NO_CHECK));
  check("udp4 SO_BUSY_POLL=0 / =50", "%s / %s", res(set_int(u, SOL_SOCKET, SO_BUSY_POLL, 0)), res(set_int(u, SOL_SOCKET, SO_BUSY_POLL, 50)));
  printf("info  udp4 SO_ZEROCOPY=1 %s, UDP_CORK=1 %s\n", res(set_int(u, SOL_SOCKET, SO_ZEROCOPY, 1)), res(set_int(u, SOL_UDP, UDP_CORK, 1)));
  check("udp4 SO_ACCEPTCONN", "%s", opt(u, SOL_SOCKET, SO_ACCEPTCONN));
  check("udp4 IP_RECVTTL=1", "%s -> %s", res(set_int(u, IPPROTO_IP, IP_RECVTTL, 1)), opt(u, IPPROTO_IP, IP_RECVTTL));
  check("udp4 IP_FREEBIND=1", "%s -> %s", res(set_int(u, IPPROTO_IP, IP_FREEBIND, 1)), opt(u, IPPROTO_IP, IP_FREEBIND));
  check("udp4 IP_TRANSPARENT=1", "%s", res(set_int(u, IPPROTO_IP, IP_TRANSPARENT, 1)));
  check("udp4 IP_HDRINCL=1", "%s", res(set_int(u, IPPROTO_IP, IP_HDRINCL, 1)));
  check("udp4 IP_MULTICAST_ALL default", "%s", opt(u, IPPROTO_IP, IP_MULTICAST_ALL));
  check("udp4 IP_TOS read with 1-byte optlen", "%s", ({
          unsigned char b = 0x55;
          socklen_t bl = 1;
          set_int(u, IPPROTO_IP, IP_TOS, 0x20);
          long gr = K(getsockopt(u, IPPROTO_IP, IP_TOS, &b, &bl));
          char* o = rotating();
          snprintf(o, 64, "%s len=%u value=0x%x", res(gr), (unsigned)bl, b);
          o;
        }));
  check("tcp4 TCP_FASTOPEN_CONNECT=1", "%s -> %s", res(set_int(t, IPPROTO_TCP, TCP_FASTOPEN_CONNECT, 1)), opt(t, IPPROTO_TCP, TCP_FASTOPEN_CONNECT));
  check("tcp4 TCP_USER_TIMEOUT=5000", "%s -> %s", res(set_int(t, IPPROTO_TCP, TCP_USER_TIMEOUT, 5000)), opt(t, IPPROTO_TCP, TCP_USER_TIMEOUT));
  check("tcp4 TCP_USER_TIMEOUT=-1", "%s", res(set_int(t, IPPROTO_TCP, TCP_USER_TIMEOUT, -1)));
  check("tcp4 TCP_NODELAY default", "%s", opt(t, IPPROTO_TCP, TCP_NODELAY));
  check("tcp4 TCP_CORK=1", "%s -> %s", res(set_int(t, IPPROTO_TCP, TCP_CORK, 1)), opt(t, IPPROTO_TCP, TCP_CORK));
  check("tcp4 TCP_QUICKACK default", "%s", opt(t, IPPROTO_TCP, TCP_QUICKACK));
  check("tcp4 TCP_SYNCNT default", "%s", opt(t, IPPROTO_TCP, TCP_SYNCNT));
  struct ucred cr;
  socklen_t crl = sizeof cr;
  memset(&cr, 0x55, sizeof cr);
  long gr = K(getsockopt(t, SOL_SOCKET, SO_PEERCRED, &cr, &crl));
  check("tcp4 SO_PEERCRED", "%s len=%u pid=%d uid=%d gid=%d", res(gr), (unsigned)crl, cr.pid, (int)cr.uid, (int)cr.gid);
  int sp[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
  crl = sizeof cr;
  gr = K(getsockopt(sp[0], SOL_SOCKET, SO_PEERCRED, &cr, &crl));
  check("socketpair SO_PEERCRED", "%s len=%u pid %s uid %s", res(gr), (unsigned)crl, cr.pid == getpid() ? "= getpid()" : "other",
        cr.uid == getuid() ? "= getuid()" : "other");
  close(sp[0]);
  close(sp[1]);
  close(u);
  close(u6);
  close(t);
}

static void names(void) {
  struct addrinfo hints, *res0 = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  int r = getaddrinfo("localhost", "80", &hints, &res0);
  char desc[64] = "none";
  if (r == 0 && res0) {
    struct sockaddr_in* a = (struct sockaddr_in*)res0->ai_addr;
    snprintf(desc, sizeof desc, "%08x:%d", ntohl(a->sin_addr.s_addr), ntohs(a->sin_port));
  }
  check("getaddrinfo localhost:80 (AF_INET)", "%d %s", r, desc);
  if (res0) freeaddrinfo(res0);
  res0 = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_DGRAM;
  r = getaddrinfo("www.roblox.com", "443", &hints, &res0);
  int n = 0;
  for (struct addrinfo* ai = res0; ai; ai = ai->ai_next) n++;
  check("getaddrinfo www.roblox.com:443", "%s", r == 0 && n > 0 ? "resolved" : "failed");
  if (res0) freeaddrinfo(res0);
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char* only = argc > 1 ? argv[1] : "";
  if (!*only || strstr(only, "defaults")) defaults();
  if (!*only || strstr(only, "datagrams")) datagrams();
  if (!*only || strstr(only, "streams")) streams();
  if (!*only || strstr(only, "timers")) timers();
  if (!*only || strstr(only, "throughput")) throughput();
  if (!*only || strstr(only, "netlink")) netlink();
  if (!*only || strstr(only, "ifioctls")) ifioctls();
  if (!*only || strstr(only, "roblox")) roblox();
  if (!*only || strstr(only, "sockopts")) sockopts();
  if (!*only || strstr(only, "names")) names();
  printf("done\n");
  return 0;
}
