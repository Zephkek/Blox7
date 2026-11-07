// af_unix is loopback tcp with the port in a file at the path, pipes are socket pairs so one select() waits on all
// dns: bionic asks netd over a socket, a thread on the other end answers with GetAddrInfoW

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "private/NetdClientDispatch.h"
#include "w32_fd.h"
#include "w32_syscalls.h"

namespace {

constexpr int W_AF_INET = 2, W_AF_INET6 = 23;
constexpr int W_SOL_SOCKET = 0xffff;
constexpr int W_SO_REUSEADDR = 4, W_SO_KEEPALIVE = 8, W_SO_DONTROUTE = 0x10,
              W_SO_BROADCAST = 0x20, W_SO_LINGER = 0x80, W_SO_OOBINLINE = 0x100, W_SO_SNDBUF = 0x1001,
              W_SO_RCVBUF = 0x1002, W_SO_SNDTIMEO = 0x1005,
              W_SO_RCVTIMEO = 0x1006, W_SO_ERROR = 0x1007, W_SO_TYPE = 0x1008, W_SO_EXCLUSIVEADDRUSE = ~W_SO_REUSEADDR;
constexpr int W_IP_TOS = 3, W_IP_TTL = 4, W_IP_MULTICAST_IF = 9, W_IP_MULTICAST_TTL = 10, W_IP_MULTICAST_LOOP = 11,
              W_IP_ADD_MEMBERSHIP = 12, W_IP_DROP_MEMBERSHIP = 13, W_IP_DONTFRAGMENT = 14, W_IP_PKTINFO = 19,
              W_IP_TCLASS = 39, W_IP_RECVTOS = 40, W_IPV6_RECVTCLASS = 40;
constexpr int W_IPV6_UNICAST_HOPS = 4, W_IPV6_MULTICAST_IF = 9, W_IPV6_MULTICAST_HOPS = 10, W_IPV6_MULTICAST_LOOP = 11,
              W_IPV6_ADD_MEMBERSHIP = 12, W_IPV6_DROP_MEMBERSHIP = 13, W_IPV6_DONTFRAG = 14, W_IPV6_PKTINFO = 19,
              W_IPV6_V6ONLY = 27, W_IPV6_UNICAST_IF = 31, W_IPV6_TCLASS = 39, W_IPV6_MTU = 72;
constexpr int W_IP_UNICAST_IF = 31, W_IP_MTU = 73, W_TCP_MAXRT = 5;
constexpr int W_MSG_OOB = 1, W_MSG_PEEK = 2, W_MSG_DONTROUTE = 4, W_MSG_WAITALL = 8;
constexpr uint32_t W_MSG_TRUNC = 0x100;
constexpr long W_FIONBIO = static_cast<long>(0x8004667e);
constexpr long W_FIONREAD = 0x4004667f;
constexpr DWORD W_SIO_KEEPALIVE_VALS = 0x98000004, W_SIO_UDP_CONNRESET = 0x9800000c, W_SIO_UDP_NETRESET = 0x9800000f,
                W_SIO_GET_EXTENSION_FUNCTION_POINTER = 0xc8000006;
constexpr int W_WSAEINVAL = 10022, W_WSAEWOULDBLOCK = 10035, W_WSAEMSGSIZE = 10040, W_WSAENETRESET = 10052,
              W_WSAECONNABORTED = 10053, W_WSAECONNRESET = 10054, W_WSAESHUTDOWN = 10058, W_WSAETIMEDOUT = 10060,
              W_WSA_IO_PENDING = 997;

constexpr int kLinuxRmemDefault = 212992, kLinuxWmemDefault = 212992;
constexpr int kLinuxTcpRmemDefault = 131072, kLinuxTcpWmemDefault = 16384;
constexpr uint32_t kLinuxRmemMax = 33554432, kLinuxWmemMax = 16777216;
constexpr int kLinuxMinRcvbuf = 2304, kLinuxMinSndbuf = 4608;
constexpr int kLinuxKeepIdle = 7200, kLinuxKeepIntvl = 75, kLinuxKeepCnt = 9;

constexpr int kUdpMaxSegments = 64;
constexpr int64_t kSegmentStallNs = 20 * 1000000;

struct LinuxInPktinfo {
  int ifindex;
  uint32_t spec_dst, addr;
};
struct LinuxIn6Pktinfo {
  uint8_t addr[16];
  int ifindex;
};

struct SocketInfo {
  int domain, type, protocol;
  bool unix_emulated;
  char unix_path[sizeof(sockaddr_un::sun_path) + 1];
  char unix_peer[sizeof(sockaddr_un::sun_path) + 1];
  W32_SRWLOCK send_lock, recv_lock;
  int udp_segment;
  int udp_gro;
  int mtu_discover, mtu_discover6;
  bool connected;
  int sk_err;
  bool listening;
  bool listen_shut;
  long accepting;
  bool shut_wr;
  // winsock's own SO_RCVTIMEO runs ~500 ms late, so we wait ourselves
  int64_t rcvtimeo_ns, sndtimeo_ns;
  bool pktinfo, pktinfo6;
  bool recvtos, recvtclass;
  bool keepalive;
  int recverr, recverr6;
  int rcvbuf, sndbuf;
  int keepidle, keepintvl, keepcnt;
  bool reuseaddr, reuseport;
  uint8_t timestamp;
  bool dontfrag6;
  int tclass6;
  int bound_if;  // SO_BINDTODEVICE
  struct { int16_t level, name; int value; } kept[24];
  uint8_t nkept;
  SOCKET nl_kernel;
  int nl_portid;
  bool nl_bound;
  uint64_t nl_groups;
  int nl_max_recv;
  uint8_t nl_flags[16];
};

SocketInfo* new_info(int domain, int type, int protocol, bool unix_emulated) {
  auto* s = static_cast<SocketInfo*>(calloc(1, sizeof(SocketInfo)));
  if (s) {
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->unix_emulated = unix_emulated;
  }
  return s;
}

bool udp_socket(const SocketInfo* s) {
  return s && !s->unix_emulated && s->type == SOCK_DGRAM && (s->domain == AF_INET || s->domain == AF_INET6);
}

bool inet_socket(const SocketInfo* s) { return s && !s->unix_emulated && (s->domain == AF_INET || s->domain == AF_INET6); }

bool tcp_socket(const SocketInfo* s) { return inet_socket(s) && s->type == SOCK_STREAM; }

bool netlink_socket(const SocketInfo* s) { return s && s->domain == AF_NETLINK; }

bool socket_ready(SOCKET s, bool write, int64_t wait_ns);
long next_datagram(W32File* f, SocketInfo* s, int* out);
long apply_keepalive(SOCKET sk, const SocketInfo* s);
void nl_release(SocketInfo* s);

void destroy_socket(W32File* f) {
  w32.closesocket(reinterpret_cast<SOCKET>(f->handle));
  SocketInfo* s = static_cast<SocketInfo*>(f->extra);
  if (s && s->unix_emulated && s->unix_path[0] && s->unix_path[0] != '@') unlink(s->unix_path);
  if (netlink_socket(s)) nl_release(s);
  free(s);
}

long wsa_fail() { return -w32_errno_from_wsa(w32.WSAGetLastError()); }

SOCKET sock(W32File* f) { return reinterpret_cast<SOCKET>(f->handle); }

template <typename Fn>
long with_socket(long fd, Fn fn) {
  W32File* f = w32_fd_get(static_cast<int>(fd));
  if (!f) return -EBADF;
  long r = f->kind == W32Kind::kSocket ? fn(f, static_cast<SocketInfo*>(f->extra)) : -ENOTSOCK;
  w32_file_put(f);
  return r;
}

int to_winsock(const void* addr, socklen_t len, void* out, int out_size) {
  if (!addr || len < sizeof(sa_family_t)) return -EINVAL;
  if (static_cast<int>(len) > out_size) len = static_cast<socklen_t>(out_size);
  memcpy(out, addr, len);
  uint16_t& family = *static_cast<uint16_t*>(out);
  if (family == AF_INET6) {
    family = W_AF_INET6;
    if (len > sizeof(sockaddr_in6)) len = sizeof(sockaddr_in6);
  } else if (family == AF_INET) {
    if (len > sizeof(sockaddr_in)) len = sizeof(sockaddr_in);
  } else if (family != AF_UNSPEC) {
    return -EAFNOSUPPORT;
  }
  return static_cast<int>(len);
}

void from_winsock(void* buf, int wlen, sockaddr* addr, socklen_t* len) {
  if (!addr || !len) return;
  uint16_t& family = *static_cast<uint16_t*>(buf);
  if (family == W_AF_INET6) family = AF_INET6;
  socklen_t n = static_cast<socklen_t>(wlen) < *len ? static_cast<socklen_t>(wlen) : *len;
  memcpy(addr, buf, n);
  *len = static_cast<socklen_t>(wlen);
}

int to_winsock_family(int domain) { return domain == AF_INET6 ? W_AF_INET6 : domain; }

int to_winsock_msg_flags(int flags) {
  int w = 0;
  if (flags & MSG_OOB) w |= W_MSG_OOB;
  if (flags & MSG_PEEK) w |= W_MSG_PEEK;
  if (flags & MSG_DONTROUTE) w |= W_MSG_DONTROUTE;
  if (flags & MSG_WAITALL) w |= W_MSG_WAITALL;
  return w;
}

SOCKET raw_socket(int family, int type, int protocol) {
  SOCKET s = w32.socket(family, type, protocol);
  // winsock sockets are inheritable by default, children only get what posix_spawn hands them
  if (s != INVALID_SOCKET) w32.SetHandleInformation(reinterpret_cast<HANDLE>(s), 1 /* HANDLE_FLAG_INHERIT */, 0);
  return s;
}

// winsock reports icmp errors on any udp socket, linux only on connected ones
void udp_report_icmp(SOCKET s, bool connected) {
  uint32_t reset = connected ? 1 : 0, net = 0;
  DWORD done = 0;
  w32.WSAIoctl(s, W_SIO_UDP_CONNRESET, &reset, sizeof reset, nullptr, 0, &done, nullptr, nullptr);
  w32.WSAIoctl(s, W_SIO_UDP_NETRESET, &net, sizeof net, nullptr, 0, &done, nullptr, nullptr);
}

void linux_defaults(SOCKET s, int domain, int type) {
  if (domain == AF_INET6) {
    const uint32_t off = 0;
    w32.setsockopt(s, IPPROTO_IPV6, W_IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
  }
  if (type == SOCK_DGRAM) {
    const int rcv = kLinuxRmemDefault, snd = kLinuxWmemDefault;
    w32.setsockopt(s, W_SOL_SOCKET, W_SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof rcv);
    w32.setsockopt(s, W_SOL_SOCKET, W_SO_SNDBUF, reinterpret_cast<const char*>(&snd), sizeof snd);
    udp_report_icmp(s, false);
  }
}

// accept()ed sockets inherit the listener's blocking mode on windows, not on linux
long install(SOCKET s, SocketInfo* info, int flags, bool set_mode) {
  W32File* f = w32_file_new(W32Kind::kSocket, reinterpret_cast<HANDLE>(s), O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
  if (!f || !info) {
    w32.closesocket(s);
    free(info);
    free(f);
    return -ENOMEM;
  }
  f->extra = info;
  f->destroy = destroy_socket;
  if ((flags & SOCK_NONBLOCK) || set_mode) {
    uint32_t nb = (flags & SOCK_NONBLOCK) ? 1 : 0;
    w32.ioctlsocket(s, W_FIONBIO, &nb);
  }
  int fd = w32_fd_install(f, flags & SOCK_CLOEXEC);
  w32_file_put(f);
  return fd;
}

long install_socket(SOCKET s, int domain, int type, int protocol, int flags) {
  return install(s, new_info(domain, type & 0xf, protocol, false), flags, false);
}

long loopback_pair(int type, SOCKET out[2]) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int alen = sizeof a;
  if (type == SOCK_DGRAM) {
    SOCKET x = raw_socket(W_AF_INET, SOCK_DGRAM, 0), y = raw_socket(W_AF_INET, SOCK_DGRAM, 0);
    sockaddr_in ax = a, ay = a;
    int lx = sizeof ax, ly = sizeof ay;
    if (x == INVALID_SOCKET || y == INVALID_SOCKET || w32.bind(x, &ax, sizeof ax) || w32.bind(y, &ay, sizeof ay) ||
        w32.getsockname(x, &ax, &lx) || w32.getsockname(y, &ay, &ly) || w32.connect(x, &ay, ly) || w32.connect(y, &ax, lx)) {
      long r = wsa_fail();
      if (x != INVALID_SOCKET) w32.closesocket(x);
      if (y != INVALID_SOCKET) w32.closesocket(y);
      return r;
    }
    out[0] = x;
    out[1] = y;
    return 0;
  }
  SOCKET listener = raw_socket(W_AF_INET, SOCK_STREAM, 0);
  if (listener == INVALID_SOCKET) return wsa_fail();
  if (w32.bind(listener, &a, sizeof a) || w32.getsockname(listener, &a, &alen) || w32.listen(listener, 1)) {
    long r = wsa_fail();
    w32.closesocket(listener);
    return r;
  }
  SOCKET x = raw_socket(W_AF_INET, SOCK_STREAM, 0);
  if (x == INVALID_SOCKET || w32.connect(x, &a, alen)) {
    long r = wsa_fail();
    if (x != INVALID_SOCKET) w32.closesocket(x);
    w32.closesocket(listener);
    return r;
  }
  SOCKET y = w32.accept(listener, nullptr, nullptr);
  if (y != INVALID_SOCKET) w32.SetHandleInformation(reinterpret_cast<HANDLE>(y), 1, 0);
  w32.closesocket(listener);
  if (y == INVALID_SOCKET) {
    long r = wsa_fail();
    w32.closesocket(x);
    return r;
  }
  int one = 1;
  w32.setsockopt(x, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
  w32.setsockopt(y, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
  out[0] = x;
  out[1] = y;
  return 0;
}

void unix_port_file(const sockaddr_un* sun, socklen_t len, char* out) {
  size_t n = len > offsetof(sockaddr_un, sun_path) ? len - offsetof(sockaddr_un, sun_path) : 0;
  if (n > sizeof(sun->sun_path)) n = sizeof(sun->sun_path);
  if (n && sun->sun_path[0] == 0) {
    char name[sizeof(sun->sun_path)];
    size_t k = 0;
    for (size_t i = 1; i < n && k + 1 < sizeof name; i++) name[k++] = (sun->sun_path[i] == '/' || !sun->sun_path[i]) ? '_' : sun->sun_path[i];
    name[k] = 0;
    snprintf(out, sizeof(sun->sun_path) + 1, "/tmp/.abstract-%s", name);
  } else {
    snprintf(out, sizeof(sun->sun_path) + 1, "%.*s", static_cast<int>(n), sun->sun_path);
  }
}

}  // namespace

void w32_init_net();

W32File* w32_socket_file(SOCKET s) {
  int type = 0, len = sizeof type;
  if (w32.getsockopt(s, W_SOL_SOCKET, W_SO_TYPE, reinterpret_cast<char*>(&type), &len) != 0) return nullptr;
  W32File* f = w32_file_new(W32Kind::kSocket, reinterpret_cast<HANDLE>(s), O_RDWR);
  SocketInfo* info = new_info(AF_INET, type, 0, false);
  if (!f || !info) {
    free(f);
    free(info);
    return nullptr;
  }
  f->extra = info;
  f->destroy = destroy_socket;
  return f;
}

namespace {

struct WinUnicast {  // IP_ADAPTER_UNICAST_ADDRESS_LH
  uint32_t length, flags;
  WinUnicast* next;
  const uint8_t* sockaddr;
  int32_t sockaddr_len, pad;
  int32_t prefix_origin, suffix_origin, dad_state;
  uint32_t valid, preferred, lease;
  uint8_t prefix_len;
};
static_assert(offsetof(WinUnicast, prefix_origin) == 32 && offsetof(WinUnicast, prefix_len) == 56, "IP_ADAPTER_UNICAST_ADDRESS_LH");

struct WinAdapter {  // IP_ADAPTER_ADDRESSES_LH
  uint32_t length, if_index;
  WinAdapter* next;
  char* adapter_name;
  WinUnicast* unicast;
  void *anycast, *multicast, *dns;
  WCHAR *dns_suffix, *description, *friendly_name;
  uint8_t phys[8];
  uint32_t phys_len, flags, mtu, if_type, oper_status, ipv6_if_index;
  uint32_t zone_indices[16];
  void* prefix;
  uint64_t tx_speed, rx_speed;
  void *wins, *gateway;
  uint32_t ipv4_metric, ipv6_metric;
  uint64_t luid;
  void* dhcp4_server;
  int32_t dhcp4_server_len, pad;
  uint32_t compartment;
  uint8_t network_guid[16];
  uint32_t connection_type, tunnel_type;
};
static_assert(offsetof(WinAdapter, phys_len) == 88 && offsetof(WinAdapter, luid) == 224 && offsetof(WinAdapter, tunnel_type) == 272,
              "IP_ADAPTER_ADDRESSES_LH");

using GetAdaptersAddressesFn = DWORD(WINAPI*)(DWORD, DWORD, void*, void*, DWORD*);
using GetBestInterfaceExFn = DWORD(WINAPI*)(const void*, DWORD*);

struct IpHelper {
  GetAdaptersAddressesFn adapters;
  GetBestInterfaceExFn best_interface;
};

const IpHelper& iphlpapi() {
  static IpHelper api;
  static int loaded;
  if (!__atomic_load_n(&loaded, __ATOMIC_ACQUIRE)) {
    IpHelper a{};
    if (void* dll = w32_boot->LoadLibraryW(reinterpret_cast<const uint16_t*>(u"iphlpapi.dll"))) {
      a.adapters = reinterpret_cast<GetAdaptersAddressesFn>(w32_boot->GetProcAddress(dll, "GetAdaptersAddresses"));
      a.best_interface = reinterpret_cast<GetBestInterfaceExFn>(w32_boot->GetProcAddress(dll, "GetBestInterfaceEx"));
    }
    api = a;
    __atomic_store_n(&loaded, 1, __ATOMIC_RELEASE);
  }
  return api;
}

constexpr int kMaxIfaces = 64, kMaxIfAddrs = 256;

struct IfAddr {
  int index;
  uint8_t family, prefixlen, scope;
  bool has_broadcast;
  uint32_t flags, valid, preferred;
  uint8_t addr[16], broadcast[4];
};

struct Iface {
  int index;
  uint64_t luid;
  char name[IFNAMSIZ];
  uint32_t flags, mtu, txqlen;
};

struct IfTable {
  int count, addr_count;
  uint32_t stamp;
  Iface ifaces[kMaxIfaces];
  IfAddr addrs[kMaxIfAddrs];
};

struct IfBuild {
  IfTable t;
  const char* prefix[kMaxIfaces];
  int fixed[kMaxIfaces];
};

W32_SRWLOCK g_if_lock;
IfTable g_ifaces;
int64_t g_ifaces_ns;
struct {
  uint64_t luid;
  char name[IFNAMSIZ];
} g_if_names[kMaxIfaces];
int g_if_name_count;

const WCHAR* find_word(const WCHAR* s, const char* word) {
  const size_t n = strlen(word);
  for (; s && *s; s++) {
    size_t k = 0;
    while (k < n && s[k] == static_cast<unsigned char>(word[k])) k++;
    if (k == n) return s + n;
  }
  return nullptr;
}

enum class Link { kLoopback, kEther, kPpp, kTun, kCellular };

Link linux_link(const WinAdapter* a, const char** prefix, int* fixed) {
  *fixed = -1;
  switch (a->if_type) {
    case 24: *prefix = "lo"; return Link::kLoopback;
    case 71: *prefix = "wlan"; return Link::kEther;
    case 23: *prefix = "ppp"; return Link::kPpp;
    case 243:
    case 244: *prefix = "rmnet_data"; return Link::kCellular;
    case 53:
    case 131: *prefix = "tun"; return Link::kTun;
    case 6: break;
    default:
      *prefix = a->phys_len == 6 ? "eth" : "tun";
      return a->phys_len == 6 ? Link::kEther : Link::kTun;
  }
  const WCHAR* vm = find_word(a->friendly_name, "VMnet");
  if (!vm) vm = find_word(a->description, "VMnet");
  if (vm && *vm >= '0' && *vm <= '9') {
    *fixed = 0;
    for (; *vm >= '0' && *vm <= '9'; vm++) *fixed = *fixed * 10 + (*vm - '0');
    *prefix = "vmnet";
  } else if (find_word(a->description, "VirtualBox")) {
    *prefix = "vboxnet";
  } else if (find_word(a->description, "Hyper-V")) {
    *prefix = "virbr";
  } else if (find_word(a->description, "TAP-")) {
    *prefix = "tap";
  } else if (find_word(a->description, "Bluetooth")) {
    *prefix = "bnep";
  } else {
    *prefix = "eth";
  }
  return Link::kEther;
}

void add_address(IfBuild* b, const Iface& i, const WinUnicast* u, bool p2p) {
  if (b->t.addr_count >= kMaxIfAddrs || !u->sockaddr) return;
  uint16_t family;
  memcpy(&family, u->sockaddr, 2);
  IfAddr a{};
  a.index = i.index;
  if (family == W_AF_INET) {
    if (u->dad_state != 3 && u->dad_state != 4) return;  // Windows lists the 169.254 an unplugged adapter would try
    a.family = AF_INET;
    memcpy(a.addr, u->sockaddr + 4, 4);
    a.prefixlen = u->prefix_len > 32 ? 32 : u->prefix_len;
    a.scope = a.addr[0] == 127 ? RT_SCOPE_HOST : a.addr[0] == 169 && a.addr[1] == 254 ? RT_SCOPE_LINK : RT_SCOPE_UNIVERSE;
    if (!p2p && a.scope != RT_SCOPE_HOST && a.prefixlen <= 30) {
      uint32_t addr, mask = a.prefixlen ? htonl(~0u << (32 - a.prefixlen)) : 0;
      memcpy(&addr, a.addr, 4);
      const uint32_t brd = addr | ~mask;
      memcpy(a.broadcast, &brd, 4);
      a.has_broadcast = true;
    }
  } else if (family == W_AF_INET6) {
    a.family = AF_INET6;
    memcpy(a.addr, u->sockaddr + 8, 16);
    a.prefixlen = u->prefix_len > 128 ? 128 : u->prefix_len;
    static const uint8_t kLoopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(a.addr, kLoopback6, 16) == 0) a.scope = RT_SCOPE_HOST;
    else if (a.addr[0] == 0xfe && (a.addr[1] & 0xc0) == 0x80) a.scope = RT_SCOPE_LINK;
    else if (a.addr[0] == 0xfe && (a.addr[1] & 0xc0) == 0xc0) a.scope = RT_SCOPE_SITE;
    if (u->dad_state <= 1) a.flags |= IFA_F_TENTATIVE;
    if (u->dad_state == 2) a.flags |= IFA_F_TENTATIVE | IFA_F_DADFAILED;
  } else {
    return;
  }
  if (u->valid == 0xffffffffu) a.flags |= IFA_F_PERMANENT;
  if (u->dad_state == 3 || u->preferred == 0) a.flags |= IFA_F_DEPRECATED;
  a.valid = u->valid;
  a.preferred = (a.flags & IFA_F_PERMANENT) ? 0xffffffffu : u->preferred;
  b->t.addrs[b->t.addr_count++] = a;
}

void add_loopback(IfBuild* b) {
  if (b->t.count >= kMaxIfaces) return;
  Iface& i = b->t.ifaces[b->t.count];
  i = Iface{};
  i.index = 1;
  i.flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING | IFF_LOWER_UP;
  i.mtu = 65536;
  i.txqlen = 1000;
  b->prefix[b->t.count] = "lo";
  b->fixed[b->t.count] = -1;
  b->t.count++;
  struct {
    uint16_t family;
    uint8_t rest[26];
  } v4{W_AF_INET, {0, 0, 127, 0, 0, 1}}, v6{W_AF_INET6, {}};
  v6.rest[21] = 1;
  WinUnicast u{};
  u.dad_state = 4;
  u.valid = u.preferred = 0xffffffffu;
  u.sockaddr = reinterpret_cast<const uint8_t*>(&v4);
  u.prefix_len = 8;
  add_address(b, i, &u, false);
  u.sockaddr = reinterpret_cast<const uint8_t*>(&v6);
  u.prefix_len = 128;
  add_address(b, i, &u, false);
}

void build_ifaces(IfBuild* b) {
  memset(b, 0, sizeof *b);
  const IpHelper& api = iphlpapi();
  char* buf = nullptr;
  DWORD size = 32768, rc = 1;
  for (int tries = 0; api.adapters && tries < 4; tries++) {
    free(buf);
    buf = static_cast<char*>(malloc(size));
    if (!buf) break;
    rc = api.adapters(0, 0x2 | 0x4 | 0x8, nullptr, buf, &size);  // GAA_FLAG_SKIP_ANYCAST | _MULTICAST | _DNS_SERVER
    if (rc != 111) break;                                        // ERROR_BUFFER_OVERFLOW
  }
  bool have_loopback = false;
  for (auto* a = rc == 0 ? reinterpret_cast<WinAdapter*>(buf) : nullptr; a && b->t.count < kMaxIfaces; a = a->next) {
    const int index = static_cast<int>(a->if_index ? a->if_index : a->ipv6_if_index);
    // 6to4, isatap, teredo, ip-https: windows-only tunnels, no linux box has these
    if (index <= 0 || a->tunnel_type == 11 || a->tunnel_type == 13 || a->tunnel_type == 14 || a->tunnel_type == 15) continue;
    const char* prefix;
    int fixed;
    const Link link = linux_link(a, &prefix, &fixed);
    if (link == Link::kLoopback) {
      if (have_loopback) continue;
      have_loopback = true;
    }
    const int k = b->t.count;
    Iface& i = b->t.ifaces[k];
    i.index = index;
    i.luid = a->luid;
    b->prefix[k] = prefix;
    b->fixed[k] = fixed;
    const bool present = a->oper_status != 6, carrier = a->oper_status == 1 || a->oper_status == 4;
    i.flags = (present ? IFF_UP : 0) | (carrier ? IFF_RUNNING | IFF_LOWER_UP : 0) | (a->oper_status == 5 ? IFF_DORMANT : 0);
    i.mtu = a->mtu && a->mtu < 65536 ? a->mtu : 1500;
    i.txqlen = 1000;
    switch (link) {
      case Link::kLoopback:
        i.flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING | IFF_LOWER_UP;
        i.mtu = 65536;
        break;
      case Link::kEther:
        i.flags |= IFF_BROADCAST | ((a->flags & 0x10) ? 0 : IFF_MULTICAST);  // 0x10: IP_ADAPTER_NO_MULTICAST
        break;
      case Link::kPpp:
        i.flags |= IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
        i.txqlen = 3;
        break;
      case Link::kTun:
        i.flags |= IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
        i.txqlen = 500;
        break;
      case Link::kCellular:
        i.flags |= IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
        break;
    }
    b->t.count++;
    for (const WinUnicast* u = a->unicast; u; u = u->next) add_address(b, i, u, link != Link::kLoopback && link != Link::kEther);
  }
  free(buf);
  if (!have_loopback) add_loopback(b);
  for (int x = 1; x < b->t.count; x++) {
    for (int y = x; y > 0 && b->t.ifaces[y - 1].index > b->t.ifaces[y].index; y--) {
      Iface ti = b->t.ifaces[y];
      b->t.ifaces[y] = b->t.ifaces[y - 1];
      b->t.ifaces[y - 1] = ti;
      const char* tp = b->prefix[y];
      b->prefix[y] = b->prefix[y - 1];
      b->prefix[y - 1] = tp;
      const int tf = b->fixed[y];
      b->fixed[y] = b->fixed[y - 1];
      b->fixed[y - 1] = tf;
    }
  }
  auto rank = [](const IfAddr& a) { return a.family != AF_INET6 ? 0 : a.scope == RT_SCOPE_UNIVERSE ? 1 : a.scope == RT_SCOPE_SITE ? 2 : a.scope == RT_SCOPE_LINK ? 3 : 4; };
  for (int x = 1; x < b->t.addr_count; x++) {
    for (int y = x; y > 0; y--) {
      const IfAddr &p = b->t.addrs[y - 1], &q = b->t.addrs[y];
      if (p.index < q.index || (p.index == q.index && rank(p) <= rank(q))) break;
      IfAddr t = b->t.addrs[y];
      b->t.addrs[y] = b->t.addrs[y - 1];
      b->t.addrs[y - 1] = t;
    }
  }
}

void assign_name(Iface* i, const char* prefix, int fixed) {
  for (int k = 0; k < g_if_name_count; k++) {
    if (g_if_names[k].luid == i->luid) {
      memcpy(i->name, g_if_names[k].name, IFNAMSIZ);
      return;
    }
  }
  if (strcmp(prefix, "lo") == 0) {
    snprintf(i->name, IFNAMSIZ, "lo");
  } else {
    for (int n = fixed >= 0 ? fixed : 0;; n++) {
      snprintf(i->name, IFNAMSIZ, "%s%d", prefix, n);
      bool taken = false;
      for (int k = 0; k < g_if_name_count && !taken; k++) taken = strcmp(g_if_names[k].name, i->name) == 0;
      if (!taken) break;
    }
  }
  if (g_if_name_count < kMaxIfaces) {
    g_if_names[g_if_name_count].luid = i->luid;
    memcpy(g_if_names[g_if_name_count].name, i->name, IFNAMSIZ);
    g_if_name_count++;
  }
}

void refresh_ifaces() {
  const int64_t now = w32_monotonic_ns() | 1;
  const int64_t built = __atomic_load_n(&g_ifaces_ns, __ATOMIC_ACQUIRE);
  if (built && now - built < 1000000000) return;
  auto* b = static_cast<IfBuild*>(malloc(sizeof(IfBuild)));
  if (!b) return;
  build_ifaces(b);
  w32.AcquireSRWLockExclusive(&g_if_lock);
  for (int k = 0; k < b->t.count; k++) assign_name(&b->t.ifaces[k], b->prefix[k], b->fixed[k]);
  b->t.stamp = g_ifaces_ns ? g_ifaces.stamp : static_cast<uint32_t>(now / 10000000);
  memcpy(&g_ifaces, &b->t, sizeof g_ifaces);
  __atomic_store_n(&g_ifaces_ns, now, __ATOMIC_RELEASE);
  w32.ReleaseSRWLockExclusive(&g_if_lock);
  free(b);
}

template <typename Fn>
long with_ifaces(Fn fn) {
  refresh_ifaces();
  w32.AcquireSRWLockShared(&g_if_lock);
  const long r = fn(static_cast<const IfTable&>(g_ifaces));
  w32.ReleaseSRWLockShared(&g_if_lock);
  return r;
}

const Iface* iface_by_index(const IfTable& t, int index) {
  for (int k = 0; k < t.count; k++)
    if (t.ifaces[k].index == index) return &t.ifaces[k];
  return nullptr;
}

const Iface* iface_by_name(const IfTable& t, const char* name) {
  for (int k = 0; k < t.count; k++)
    if (strcmp(t.ifaces[k].name, name) == 0) return &t.ifaces[k];
  return nullptr;
}

// hack: netlink is a loopback udp pair and we play kernel on the other end

constexpr size_t kNlGoodSize = 3776;      // NLMSG_GOODSIZE
constexpr int kNlMaxRecv = 32768 - 320;   // SKB_WITH_OVERHEAD(32768)
constexpr int kRtmLastMapped = RTM_GETVLAN;  // the last type the phone's SELinux table knows

W32_SRWLOCK g_nl_lock;
int g_nl_ports[256];
int g_nl_port_count;

void nl_autobind(SocketInfo* s) {
  w32.AcquireSRWLockExclusive(&g_nl_lock);
  auto used = [](int p) {
    for (int k = 0; k < g_nl_port_count; k++)
      if (g_nl_ports[k] == p) return true;
    return false;
  };
  int port = getpid();
  while (used(port)) {
    uint32_t rnd = 0;
    w32.SystemFunction036(&rnd, sizeof rnd);
    port = static_cast<int>(INT32_MIN + static_cast<int64_t>(rnd % 2147479552u));
  }
  if (g_nl_port_count < static_cast<int>(sizeof g_nl_ports / sizeof g_nl_ports[0])) g_nl_ports[g_nl_port_count++] = port;
  s->nl_portid = port;
  s->nl_bound = true;
  w32.ReleaseSRWLockExclusive(&g_nl_lock);
}

void nl_release(SocketInfo* s) {
  if (s->nl_kernel) w32.closesocket(s->nl_kernel);
  if (!s->nl_bound) return;
  w32.AcquireSRWLockExclusive(&g_nl_lock);
  for (int k = 0; k < g_nl_port_count; k++) {
    if (g_nl_ports[k] == s->nl_portid) {
      g_nl_ports[k] = g_nl_ports[--g_nl_port_count];
      break;
    }
  }
  w32.ReleaseSRWLockExclusive(&g_nl_lock);
}

struct NlMsg {
  char data[1024];
  size_t len = 0;
  void start(int type, int flags, uint32_t seq, uint32_t pid, const void* body, size_t n) {
    nlmsghdr h{};
    h.nlmsg_type = static_cast<uint16_t>(type);
    h.nlmsg_flags = static_cast<uint16_t>(flags);
    h.nlmsg_seq = seq;
    h.nlmsg_pid = pid;
    memset(data, 0, NLMSG_ALIGN(NLMSG_HDRLEN + n));
    memcpy(data, &h, sizeof h);
    memcpy(data + NLMSG_HDRLEN, body, n);
    len = NLMSG_ALIGN(NLMSG_HDRLEN + n);
  }
  void attr(int type, const void* v, size_t n) {
    if (len + RTA_SPACE(n) > sizeof data) return;
    const rtattr a{static_cast<unsigned short>(RTA_LENGTH(n)), static_cast<unsigned short>(type)};
    memset(data + len, 0, RTA_SPACE(n));
    memcpy(data + len, &a, sizeof a);
    memcpy(data + len + RTA_LENGTH(0), v, n);
    len += RTA_SPACE(n);
  }
  void u32(int type, uint32_t v) { attr(type, &v, 4); }
  void str(int type, const char* s) { attr(type, s, strlen(s) + 1); }
  void finish() {
    const uint32_t l = static_cast<uint32_t>(len);
    memcpy(data, &l, 4);
  }
};

void nl_emit(SocketInfo* s, const void* data, size_t len) {
  w32.send(s->nl_kernel, static_cast<const char*>(data), static_cast<int>(len), 0);
}

class NlDump {
 public:
  explicit NlDump(SocketInfo* s) : s_(s) {
    limit_ = s->nl_max_recv > static_cast<int>(kNlGoodSize) ? static_cast<size_t>(s->nl_max_recv) : kNlGoodSize;
    buf_ = static_cast<char*>(malloc(limit_));
  }
  ~NlDump() { free(buf_); }
  void add(const NlMsg& m) {
    if (!buf_) return;
    if (len_ && len_ + m.len > limit_) flush();
    if (m.len > limit_) return;
    memcpy(buf_ + len_, m.data, m.len);
    len_ += m.len;
  }
  void flush() {
    if (len_) nl_emit(s_, buf_, len_);
    len_ = 0;
  }
  void done(uint32_t seq) {
    flush();
    NlMsg d;
    const int zero = 0;
    d.start(NLMSG_DONE, NLM_F_MULTI, seq, static_cast<uint32_t>(s_->nl_portid), &zero, sizeof zero);
    d.finish();
    nl_emit(s_, d.data, d.len);
  }

 private:
  SocketInfo* s_;
  char* buf_;
  size_t limit_, len_ = 0;
};

void addr_msg(NlMsg* m, const IfAddr& a, const Iface& i, uint32_t seq, uint32_t pid, int flags, uint32_t stamp) {
  ifaddrmsg im{};
  im.ifa_family = a.family;
  im.ifa_prefixlen = a.prefixlen;
  im.ifa_flags = static_cast<uint8_t>(a.flags);
  im.ifa_scope = a.scope;
  im.ifa_index = static_cast<uint32_t>(a.index);
  m->start(RTM_NEWADDR, flags, seq, pid, &im, sizeof im);
  const ifa_cacheinfo ci{a.preferred, a.valid, stamp, stamp};
  if (a.family == AF_INET) {
    m->attr(IFA_ADDRESS, a.addr, 4);
    m->attr(IFA_LOCAL, a.addr, 4);
    if (a.has_broadcast) m->attr(IFA_BROADCAST, a.broadcast, 4);
    m->str(IFA_LABEL, i.name);
    m->u32(IFA_FLAGS, a.flags);
    m->attr(IFA_CACHEINFO, &ci, sizeof ci);
  } else {
    m->attr(IFA_ADDRESS, a.addr, 16);
    m->attr(IFA_CACHEINFO, &ci, sizeof ci);
    m->u32(IFA_FLAGS, a.flags);
  }
  m->finish();
}

void nl_ack(SocketInfo* s, const char* req, const nlmsghdr& h, int err) {
  const bool capped = !err || s->nl_flags[NETLINK_CAP_ACK];
  const size_t echo = capped ? NLMSG_HDRLEN : h.nlmsg_len, len = NLMSG_HDRLEN + sizeof(int) + echo;
  char* out = static_cast<char*>(calloc(1, NLMSG_ALIGN(len)));
  if (!out) return;
  nlmsghdr r{};
  r.nlmsg_len = static_cast<uint32_t>(len);
  r.nlmsg_type = NLMSG_ERROR;
  r.nlmsg_flags = capped ? NLM_F_CAPPED : 0;
  r.nlmsg_seq = h.nlmsg_seq;
  r.nlmsg_pid = static_cast<uint32_t>(s->nl_portid);
  memcpy(out, &r, sizeof r);
  memcpy(out + NLMSG_HDRLEN, &err, sizeof err);
  memcpy(out + NLMSG_HDRLEN + sizeof err, req, echo);
  nl_emit(s, out, NLMSG_ALIGN(len));
  free(out);
}

void nl_dump_addrs(SocketInfo* s, const nlmsghdr& h, int family) {
  NlDump out(s);
  with_ifaces([&](const IfTable& t) -> long {
    static const int kFamilies[2] = {AF_INET, AF_INET6};
    for (int fam : kFamilies) {
      if ((family == AF_INET || family == AF_INET6) && family != fam) continue;
      for (int k = 0; k < t.addr_count; k++) {
        const Iface* i = t.addrs[k].family == fam ? iface_by_index(t, t.addrs[k].index) : nullptr;
        if (!i) continue;
        NlMsg m;
        addr_msg(&m, t.addrs[k], *i, h.nlmsg_seq, static_cast<uint32_t>(s->nl_portid), NLM_F_MULTI, t.stamp);
        out.add(m);
      }
      out.flush();
    }
    return 0;
  });
  out.done(h.nlmsg_seq);
}

void note_first(int what, const char* text) {
  static unsigned seen;
  if (what < 32 && !(__atomic_fetch_or(&seen, 1u << what, __ATOMIC_RELAXED) & (1u << what))) w32_debug("net: first %s", text);
}

int nl_request(SocketInfo* s, const char* msg, const nlmsghdr& h, bool* dumped) {
  const int type = h.nlmsg_type;
  if (type == RTM_GETADDR) note_first(0, "netlink RTM_GETADDR");
  if (type > RTM_MAX) return -EOPNOTSUPP;
  if (h.nlmsg_len < NLMSG_HDRLEN + 1) return 0;
  const int family = static_cast<uint8_t>(msg[NLMSG_HDRLEN]);
  if (((type - RTM_BASE) & 3) != 2) return -EPERM;  // RTM_NEW*, RTM_DEL*, RTM_SET*: CAP_NET_ADMIN
  if (h.nlmsg_flags & NLM_F_DUMP) {
    if (type != RTM_GETADDR) return -EOPNOTSUPP;
    nl_dump_addrs(s, h, family);
    *dumped = true;
    return 0;
  }
  return type == RTM_GETADDR && family == AF_INET6 ? -EINVAL : -EOPNOTSUPP;
}

// selinux like an api 35 app gets it: no rtm_getlink since android 11, no rtm_getneigh since 13
long nl_policy(const char* data, size_t len) {
  while (len >= NLMSG_HDRLEN) {
    nlmsghdr h;
    memcpy(&h, data, sizeof h);
    if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > len) return 0;
    const int type = h.nlmsg_type;
    if (type < RTM_BASE || type > kRtmLastMapped) return -EINVAL;
    if (((type - RTM_BASE) & 3) != 2 || type == RTM_GETLINK || type == RTM_GETNEIGH) return -EACCES;
    const size_t step = NLMSG_ALIGN(h.nlmsg_len);
    if (step >= len) return 0;
    data += step;
    len -= step;
  }
  return 0;
}

void nl_process(SocketInfo* s, const char* data, size_t len) {
  while (len >= NLMSG_HDRLEN) {
    nlmsghdr h;
    memcpy(&h, data, sizeof h);
    if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > len) return;
    int err = 0;
    bool dumped = false;
    if ((h.nlmsg_flags & NLM_F_REQUEST) && h.nlmsg_type >= NLMSG_MIN_TYPE) err = nl_request(s, data, h, &dumped);
    if (!dumped && ((h.nlmsg_flags & NLM_F_ACK) || err)) nl_ack(s, data, h, err);
    const size_t step = NLMSG_ALIGN(h.nlmsg_len) < len ? NLMSG_ALIGN(h.nlmsg_len) : len;
    data += step;
    len -= step;
  }
}

long nl_socket(int type, int flags) {
  SOCKET pair[2];
  long r = loopback_pair(SOCK_DGRAM, pair);
  if (r < 0) return r;
  const int big = 1 << 20;
  w32.setsockopt(pair[0], W_SOL_SOCKET, W_SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof big);
  w32.setsockopt(pair[1], W_SOL_SOCKET, W_SO_SNDBUF, reinterpret_cast<const char*>(&big), sizeof big);
  SocketInfo* info = new_info(AF_NETLINK, type, NETLINK_ROUTE, false);
  if (!info) {
    w32.closesocket(pair[0]);
    w32.closesocket(pair[1]);
    return -ENOMEM;
  }
  info->nl_kernel = pair[1];
  return install(pair[0], info, flags, false);
}

long nl_connect(SocketInfo* s, const void* addr, socklen_t len) {
  if (!addr || len < sizeof(sa_family_t)) return -EINVAL;
  sockaddr_nl a{};
  memcpy(&a, addr, len < sizeof a ? len : sizeof a);
  if (a.nl_family == AF_UNSPEC) return 0;
  if (a.nl_family != AF_NETLINK || len < sizeof(sockaddr_nl)) return -EINVAL;
  if (a.nl_pid || a.nl_groups) return -EPERM;
  if (!s->nl_bound) nl_autobind(s);
  return 0;
}

long nl_setsockopt(SocketInfo* s, int name, const void* val, socklen_t len) {
  if (len < sizeof(int)) return -EINVAL;
  int v;
  memcpy(&v, val, sizeof v);
  switch (name) {
    case NETLINK_ADD_MEMBERSHIP:
    case NETLINK_DROP_MEMBERSHIP: {
      if (v < 1 || v > 64) return -EINVAL;
      const uint64_t bit = 1ull << (v - 1);
      s->nl_groups = name == NETLINK_ADD_MEMBERSHIP ? s->nl_groups | bit : s->nl_groups & ~bit;
      return 0;
    }
    case NETLINK_LISTEN_ALL_NSID:
      return -EPERM;  // CAP_NET_BROADCAST
    case NETLINK_PKTINFO:
    case NETLINK_BROADCAST_ERROR:
    case NETLINK_NO_ENOBUFS:
    case NETLINK_CAP_ACK:
    case NETLINK_EXT_ACK:
    case NETLINK_GET_STRICT_CHK:
      s->nl_flags[name] = v != 0;
      return 0;
    default:
      return -ENOPROTOOPT;
  }
}

long nl_getsockopt(const SocketInfo* s, int name, void* val, socklen_t* len) {
  if (static_cast<int>(*len) < 0) return -EINVAL;
  if (name == NETLINK_LIST_MEMBERSHIPS) {
    const uint32_t groups[2] = {static_cast<uint32_t>(s->nl_groups), static_cast<uint32_t>(s->nl_groups >> 32)};
    memcpy(val, groups, *len < sizeof groups ? *len : sizeof groups);
    *len = sizeof groups;
    return 0;
  }
  if (name != NETLINK_PKTINFO && name != NETLINK_BROADCAST_ERROR && name != NETLINK_NO_ENOBUFS && name != NETLINK_LISTEN_ALL_NSID &&
      name != NETLINK_CAP_ACK && name != NETLINK_EXT_ACK && name != NETLINK_GET_STRICT_CHK) {
    return -ENOPROTOOPT;
  }
  if (*len < sizeof(int)) return -EINVAL;
  const int v = s->nl_flags[name];
  memcpy(val, &v, sizeof v);
  *len = sizeof v;
  return 0;
}

long if_conf(ifconf* c) {
  return with_ifaces([&](const IfTable& t) -> long {
    int used = 0, need = 0;
    for (int k = 0; k < t.count; k++) {
      for (int a = 0; a < t.addr_count; a++) {
        const IfAddr& ad = t.addrs[a];
        if (ad.family != AF_INET || ad.index != t.ifaces[k].index) continue;
        need += sizeof(ifreq);
        if (!c->ifc_buf || c->ifc_len - used < static_cast<int>(sizeof(ifreq))) continue;
        ifreq r{};
        memcpy(r.ifr_name, t.ifaces[k].name, IFNAMSIZ);
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        memcpy(&sin.sin_addr, ad.addr, 4);
        memcpy(&r.ifr_addr, &sin, sizeof sin);
        memcpy(c->ifc_buf + used, &r, sizeof r);
        used += sizeof r;
      }
    }
    c->ifc_len = c->ifc_buf ? used : need;
    return 0;
  });
}

long if_inet(unsigned cmd, ifreq* user) {
  ifreq r;
  memcpy(&r, user, sizeof r);
  r.ifr_name[IFNAMSIZ - 1] = 0;
  sockaddr_in orig;
  memcpy(&orig, &r.ifr_addr, sizeof orig);
  char dev[IFNAMSIZ];
  memcpy(dev, r.ifr_name, IFNAMSIZ);
  if (char* colon = strchr(dev, ':')) *colon = 0;
  const long rc = with_ifaces([&](const IfTable& t) -> long {
    const Iface* i = iface_by_name(t, dev);
    if (!i) return -ENODEV;
    const IfAddr* found = nullptr;
    for (int pass = orig.sin_family == AF_INET ? 0 : 1; pass < 2 && !found; pass++) {
      for (int a = 0; a < t.addr_count && !found; a++) {
        const IfAddr& ad = t.addrs[a];
        if (ad.family != AF_INET || ad.index != i->index || strcmp(r.ifr_name, i->name) != 0) continue;
        if (pass == 0 && memcmp(ad.addr, &orig.sin_addr, 4) != 0) continue;
        found = &ad;
      }
    }
    if (!found) return -EADDRNOTAVAIL;
    sockaddr_in out{};
    out.sin_family = AF_INET;
    if (cmd == SIOCGIFBRDADDR) {
      if (found->has_broadcast) memcpy(&out.sin_addr, found->broadcast, 4);
    } else if (cmd == SIOCGIFNETMASK) {
      out.sin_addr.s_addr = found->prefixlen ? htonl(~0u << (32 - found->prefixlen)) : 0;
    } else {
      memcpy(&out.sin_addr, found->addr, 4);
    }
    memcpy(&r.ifr_addr, &out, sizeof out);
    return 0;
  });
  if (rc == 0) memcpy(user, &r, sizeof r);
  return rc;
}

long if_dev(unsigned cmd, ifreq* r) {
  return with_ifaces([&](const IfTable& t) -> long {
    if (cmd == SIOCGIFNAME) {
      const Iface* i = iface_by_index(t, r->ifr_ifindex);
      if (!i) return -ENODEV;
      strcpy(r->ifr_name, i->name);
      return 0;
    }
    r->ifr_name[IFNAMSIZ - 1] = 0;
    char dev[IFNAMSIZ];
    memcpy(dev, r->ifr_name, IFNAMSIZ);
    if (char* colon = strchr(dev, ':')) *colon = 0;
    const Iface* i = iface_by_name(t, dev);
    if (!i) return -ENODEV;
    if (cmd == SIOCGIFFLAGS) r->ifr_flags = static_cast<short>(i->flags);
    else if (cmd == SIOCGIFMTU) r->ifr_mtu = static_cast<int>(i->mtu);
    else if (cmd == SIOCGIFINDEX) r->ifr_ifindex = i->index;
    else r->ifr_qlen = static_cast<int>(i->txqlen);
    return 0;
  });
}

}  // namespace

long w32_socket_ioctl(W32File* f, unsigned long request, void* arg) {
  auto* s = static_cast<SocketInfo*>(f->extra);
  const unsigned cmd = static_cast<unsigned>(request);
  if (cmd == FIONREAD && s && s->type == SOCK_DGRAM && !netlink_socket(s)) return next_datagram(f, s, static_cast<int*>(arg));
  if (!inet_socket(s) && !netlink_socket(s)) return 1;
  if (cmd == FIONREAD && netlink_socket(s)) return -ENOTTY;
  switch (cmd) {
    case SIOCGIFCONF:
      note_first(2, "SIOCGIFCONF");
      return if_conf(static_cast<ifconf*>(arg));
    case SIOCGIFADDR:
    case SIOCGIFBRDADDR:
    case SIOCGIFNETMASK:
    case SIOCGIFDSTADDR:
      return s->domain == AF_INET ? if_inet(cmd, static_cast<ifreq*>(arg)) : -ENOTTY;
    case SIOCGIFNAME:
    case SIOCGIFFLAGS:
    case SIOCGIFMTU:
    case SIOCGIFINDEX:
    case SIOCGIFTXQLEN:
      note_first(3, "SIOCGIFNAME/FLAGS/MTU/INDEX");
      return if_dev(cmd, static_cast<ifreq*>(arg));
    case SIOCGSTAMP:
    case SIOCGSTAMPNS:
    case SIOCATMARK:
      return 1;
    default:
      // selinux: apps don't get the rest of the socket ioctls
      return (cmd & 0xff00) == 0x8900 ? -EACCES : 1;
  }
}

long sys_socket(long domain, long type_l, long protocol, long, long, long) {
  int type = static_cast<int>(type_l) & 0xf, flags = static_cast<int>(type_l) & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (domain == AF_UNIX) {
    SOCKET s = raw_socket(W_AF_INET, type == SOCK_DGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return wsa_fail();
    return install(s, new_info(AF_UNIX, type, 0, true), flags, false);
  }
  if (domain == AF_NETLINK) {
    if (protocol != NETLINK_ROUTE) return -EACCES;
    if (type != SOCK_RAW && type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
    return nl_socket(type, flags);
  }
  if (domain != AF_INET && domain != AF_INET6) return -EAFNOSUPPORT;
  if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW) return -EINVAL;
  SOCKET s = raw_socket(to_winsock_family(static_cast<int>(domain)), type, static_cast<int>(protocol));
  if (s == INVALID_SOCKET) return wsa_fail();
  linux_defaults(s, static_cast<int>(domain), type);
  return install_socket(s, static_cast<int>(domain), type, static_cast<int>(protocol), flags);
}

long sys_socketpair(long domain, long type_l, long protocol, long fds_l, long, long) {
  if (domain == AF_NETLINK) return -EOPNOTSUPP;
  if (domain != AF_UNIX && domain != AF_INET) return -EAFNOSUPPORT;
  int type = static_cast<int>(type_l) & 0xf, flags = static_cast<int>(type_l) & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  SOCKET pair[2];
  long r = loopback_pair(type == SOCK_DGRAM ? SOCK_DGRAM : SOCK_STREAM, pair);
  if (r < 0) return r;
  int* fds = reinterpret_cast<int*>(fds_l);
  long a = install_socket(pair[0], AF_UNIX, type, static_cast<int>(protocol), flags);
  if (a < 0) {
    w32.closesocket(pair[1]);
    return a;
  }
  long b = install_socket(pair[1], AF_UNIX, type, static_cast<int>(protocol), flags);
  if (b < 0) {
    sys_close(a, 0, 0, 0, 0, 0);
    return b;
  }
  fds[0] = static_cast<int>(a);
  fds[1] = static_cast<int>(b);
  return 0;
}

long w32_pipe_pair(int* fds, int flags) {
  SOCKET pair[2];
  long r = loopback_pair(SOCK_STREAM, pair);
  if (r < 0) return r;
  long a = install_socket(pair[0], AF_UNIX, SOCK_STREAM, 0, (flags & O_NONBLOCK ? SOCK_NONBLOCK : 0) | (flags & O_CLOEXEC ? SOCK_CLOEXEC : 0));
  if (a < 0) {
    w32.closesocket(pair[1]);
    return a;
  }
  long b = install_socket(pair[1], AF_UNIX, SOCK_STREAM, 0, (flags & O_NONBLOCK ? SOCK_NONBLOCK : 0) | (flags & O_CLOEXEC ? SOCK_CLOEXEC : 0));
  if (b < 0) {
    sys_close(a, 0, 0, 0, 0, 0);
    return b;
  }
  // no shutdown() for one-way: windows kills an end idle in fin-wait-2 for 2 min, the access modes do it instead
  const int modes[2] = {O_RDONLY, O_WRONLY};
  for (int i = 0; i < 2; i++) {
    W32File* end = w32_fd_get(static_cast<int>(i ? b : a));
    if (end) end->flags = (end->flags & ~O_ACCMODE) | modes[i];
    w32_file_put(end);
  }
  fds[0] = static_cast<int>(a);
  fds[1] = static_cast<int>(b);
  return 0;
}

long sys_bind(long fd, long addr_l, long len, long, long, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    if (netlink_socket(s)) return -EACCES;
    if (s->unix_emulated) {
      char path[sizeof(sockaddr_un::sun_path) + 1];
      unix_port_file(reinterpret_cast<const sockaddr_un*>(addr_l), static_cast<socklen_t>(len), path);
      if (access(path, F_OK) == 0) return -EADDRINUSE;
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      int alen = sizeof a;
      if (w32.bind(sock(f), &a, sizeof a) || w32.getsockname(sock(f), &a, &alen)) return wsa_fail();
      WCHAR win[W32_PATH_MAX];
      long r = w32_path(AT_FDCWD, path, win, W32_PATH_MAX);
      if (r < 0) return r;
      HANDLE pf = w32.CreateFileW(win, 0x40000000 /* GENERIC_WRITE */, 7, nullptr, 1 /* CREATE_NEW */, 0x4 /* SYSTEM */, nullptr);
      if (pf == INVALID_HANDLE_VALUE) return w32_fail();
      char port[32];
      int n = snprintf(port, sizeof port, "!<socket >%u\n", ntohs(a.sin_port));
      DWORD done;
      w32.WriteFile(pf, port, static_cast<DWORD>(n), &done, nullptr);
      w32.CloseHandle(pf);
      snprintf(s->unix_path, sizeof s->unix_path, "%s", path);
      return 0;
    }
    char buf[128];
    int wlen = to_winsock(reinterpret_cast<const void*>(addr_l), static_cast<socklen_t>(len), buf, sizeof buf);
    if (wlen < 0) return wlen;
    if (w32.bind(sock(f), buf, wlen) == 0) return 0;
    const int err = w32.WSAGetLastError();
    return err == 10013 ? -EADDRINUSE : -w32_errno_from_wsa(err);
  });
}

long sys_listen(long fd, long backlog, long, long, long, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    if (netlink_socket(s)) return -EACCES;
    if (w32.listen(sock(f), static_cast<int>(backlog))) return wsa_fail();
    s->listening = true;
    return 0;
  });
}

long sys_connect(long fd, long addr_l, long len, long, long, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    if (netlink_socket(s)) return nl_connect(s, reinterpret_cast<const void*>(addr_l), static_cast<socklen_t>(len));
    char buf[128];
    int wlen;
    if (s->unix_emulated) {
      char path[sizeof(sockaddr_un::sun_path) + 1];
      unix_port_file(reinterpret_cast<const sockaddr_un*>(addr_l), static_cast<socklen_t>(len), path);
      int pf = open(path, O_RDONLY | O_CLOEXEC);
      if (pf < 0) return errno == ENOENT ? -ENOENT : -ECONNREFUSED;
      char port[32] = {};
      read(pf, port, sizeof port - 1);
      close(pf);
      const char* digits = strncmp(port, "!<socket >", 10) == 0 ? port + 10 : port;
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      a.sin_port = htons(static_cast<uint16_t>(atoi(digits)));
      memcpy(buf, &a, sizeof a);
      wlen = sizeof a;
      snprintf(s->unix_peer, sizeof s->unix_peer, "%s", path);
    } else {
      wlen = to_winsock(reinterpret_cast<const void*>(addr_l), static_cast<socklen_t>(len), buf, sizeof buf);
      if (wlen < 0) return wlen;
      if (udp_socket(s) && *reinterpret_cast<uint16_t*>(buf) == AF_UNSPEC) {
        sockaddr_in6 zero{};
        zero.sin6_family = static_cast<sa_family_t>(to_winsock_family(s->domain));
        w32.connect(sock(f), &zero, s->domain == AF_INET6 ? static_cast<int>(sizeof(sockaddr_in6)) : static_cast<int>(sizeof(sockaddr_in)));
        s->connected = false;
        udp_report_icmp(sock(f), false);
        return 0;
      }
    }
    if (w32.connect(sock(f), buf, wlen) == 0) {
      if (udp_socket(s) && !s->connected) {
        s->connected = true;
        udp_report_icmp(sock(f), true);
      }
      return 0;
    }
    int err = w32.WSAGetLastError();
    if (err == W_WSAEWOULDBLOCK) return -EINPROGRESS;
    return -w32_errno_from_wsa(err);
  });
}

long sys_accept4(long fd, long addr_l, long len_l, long flags, long, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    if (netlink_socket(s)) return -EACCES;
    struct Count {
      SocketInfo* s;
      ~Count() { __atomic_sub_fetch(&s->accepting, 1, __ATOMIC_SEQ_CST); }
    } count{s};
    __atomic_add_fetch(&s->accepting, 1, __ATOMIC_SEQ_CST);
    auto shut = [s] { return __atomic_load_n(&s->listen_shut, __ATOMIC_SEQ_CST); };
    if (shut()) return -EINVAL;
    if (!(f->flags & O_NONBLOCK) && s->rcvtimeo_ns > 0 && !socket_ready(sock(f), false, s->rcvtimeo_ns)) {
      return shut() ? -EINVAL : -EAGAIN;
    }
    char buf[128];
    int wlen = sizeof buf;
    SOCKET c = w32.accept(sock(f), buf, &wlen);
    if (c == INVALID_SOCKET) return shut() ? -EINVAL : wsa_fail();
    w32.SetHandleInformation(reinterpret_cast<HANDLE>(c), 1, 0);
    SocketInfo* info = new_info(s->domain, s->type, s->protocol, s->unix_emulated);
    if (info) {
      info->rcvbuf = s->rcvbuf;
      info->sndbuf = s->sndbuf;
      info->keepalive = s->keepalive;
      info->keepidle = s->keepidle;
      info->keepintvl = s->keepintvl;
      info->keepcnt = s->keepcnt;
      info->rcvtimeo_ns = s->rcvtimeo_ns;
      info->sndtimeo_ns = s->sndtimeo_ns;
      info->reuseaddr = s->reuseaddr;
      info->reuseport = s->reuseport;
      info->timestamp = s->timestamp;
      info->bound_if = s->bound_if;
      memcpy(info->kept, s->kept, sizeof info->kept);
      info->nkept = s->nkept;
      if (info->keepalive) apply_keepalive(c, info);
    }
    long nfd = install(c, info, static_cast<int>(flags), true);
    if (nfd >= 0 && s->unix_emulated) {
      if (addr_l && len_l) {
        auto* sun = reinterpret_cast<sockaddr_un*>(addr_l);
        if (*reinterpret_cast<socklen_t*>(len_l) >= sizeof(sa_family_t)) sun->sun_family = AF_UNIX;
        *reinterpret_cast<socklen_t*>(len_l) = sizeof(sa_family_t);
      }
    } else if (nfd >= 0) {
      from_winsock(buf, wlen, reinterpret_cast<sockaddr*>(addr_l), reinterpret_cast<socklen_t*>(len_l));
    }
    return nfd;
  });
}

long sys_accept(long fd, long addr, long len, long, long, long) { return sys_accept4(fd, addr, len, 0, 0, 0); }

static long name_of(long fd, long addr_l, long len_l, bool peer) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    if (netlink_socket(s)) {
      auto* len = reinterpret_cast<socklen_t*>(len_l);
      const sockaddr_nl a{AF_NETLINK, 0, peer ? 0u : static_cast<uint32_t>(s->nl_portid), peer ? 0u : static_cast<uint32_t>(s->nl_groups)};
      memcpy(reinterpret_cast<void*>(addr_l), &a, *len < sizeof a ? *len : sizeof a);
      *len = sizeof a;
      return 0;
    }
    if (s->unix_emulated) {
      auto* sun = reinterpret_cast<sockaddr_un*>(addr_l);
      auto* len = reinterpret_cast<socklen_t*>(len_l);
      sockaddr_un out{};
      out.sun_family = AF_UNIX;
      const char* p = peer ? s->unix_peer : s->unix_path;
      snprintf(out.sun_path, sizeof out.sun_path, "%s", p);
      socklen_t full = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + strlen(p) + (p[0] ? 1 : 0));
      memcpy(sun, &out, *len < full ? *len : full);
      *len = full;
      return 0;
    }
    char buf[128];
    int wlen = sizeof buf;
    if ((peer ? w32.getpeername(sock(f), buf, &wlen) : w32.getsockname(sock(f), buf, &wlen))) {
      if (peer || w32.WSAGetLastError() != W_WSAEINVAL || !inet_socket(s)) return wsa_fail();
      memset(buf, 0, sizeof buf);
      *reinterpret_cast<uint16_t*>(buf) = static_cast<uint16_t>(to_winsock_family(s->domain));
      wlen = s->domain == AF_INET6 ? static_cast<int>(sizeof(sockaddr_in6)) : static_cast<int>(sizeof(sockaddr_in));
    }
    from_winsock(buf, wlen, reinterpret_cast<sockaddr*>(addr_l), reinterpret_cast<socklen_t*>(len_l));
    return 0;
  });
}

long sys_getsockname(long fd, long addr, long len, long, long, long) { return name_of(fd, addr, len, false); }
long sys_getpeername(long fd, long addr, long len, long, long, long) { return name_of(fd, addr, len, true); }

long sys_shutdown(long fd, long how, long, long, long, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    if (netlink_socket(s)) return -EOPNOTSUPP;
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return -EINVAL;
    if (s->listening) {
      // linux wakes every accept on a listener shut for reading (roblox's microprofiler stops that way), winsock won't
      if (how == SHUT_WR) return 0;
      if (how == SHUT_RDWR) s->shut_wr = true;
      __atomic_store_n(&s->listen_shut, true, __ATOMIC_SEQ_CST);
      for (int tries = 0; __atomic_load_n(&s->accepting, __ATOMIC_SEQ_CST) > 0 && tries < 2000; tries++) {
        w32.CancelIoEx(reinterpret_cast<HANDLE>(sock(f)), nullptr);
        w32.Sleep(1);
      }
      return 0;
    }
    if (w32.shutdown(sock(f), static_cast<int>(how))) return wsa_fail();
    if (how == SHUT_WR || how == SHUT_RDWR) s->shut_wr = true;
    return 0;
  });
}

namespace {

bool socket_ready(SOCKET s, bool write, int64_t wait_ns) {
  struct { uint32_t count; SOCKET fd[1]; } set{1, {s}};
  struct { int32_t sec, usec; } tv{0, 0};
  if (wait_ns > 0) {
    const int64_t us = (wait_ns + 999) / 1000;
    tv.sec = static_cast<int32_t>(us / 1000000);
    tv.usec = static_cast<int32_t>(us % 1000000);
  }
  return w32.select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, wait_ns < 0 ? nullptr : &tv) > 0;
}

bool readable_now(SOCKET s) { return socket_ready(s, false, 0); }
bool writable_now(SOCKET s) { return socket_ready(s, true, 0); }

constexpr uint32_t kMaximumPacket = 64u << 20;

bool is_seqpacket(W32File* f) {
  auto* info = static_cast<SocketInfo*>(f->extra);
  return info != nullptr && info->domain == AF_UNIX && info->type == SOCK_SEQPACKET;
}

void wait_socket(SOCKET s, bool for_write) { socket_ready(s, for_write, -1); }

long recv_all(SOCKET s, char* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    int n = w32.recv(s, buf + got, static_cast<int>(len - got), 0);
    if (n == 0) return got ? -ECONNRESET : 0;
    if (n < 0) {
      int err = w32.WSAGetLastError();
      if (err != W_WSAEWOULDBLOCK) return -w32_errno_from_wsa(err);
      wait_socket(s, false);
      continue;
    }
    got += static_cast<size_t>(n);
  }
  return static_cast<long>(len);
}

uint32_t pending_bytes(SOCKET s) {
  uint32_t available = 0;
  return w32.ioctlsocket(s, W_FIONREAD, &available) == 0 ? available : 0;
}

long recv_packet(W32File* f, void* buf, size_t len, int flags) {
  SOCKET s = sock(f);
  auto* info = static_cast<SocketInfo*>(f->extra);
  const bool nonblocking = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
  w32.AcquireSRWLockExclusive(&info->recv_lock);
  long r = 0;
  uint32_t size = 0;
  for (;;) {
    const uint32_t available = pending_bytes(s);
    if (available >= 4) {
      w32.recv(s, reinterpret_cast<char*>(&size), 4, W_MSG_PEEK);
      if (size > kMaximumPacket) {
        r = -EMSGSIZE;
        break;
      }
      if (available >= 4 + size) break;
    } else if (available == 0 && readable_now(s)) {
      char probe;
      if (w32.recv(s, &probe, 1, W_MSG_PEEK) == 0) break;
    }
    if (nonblocking) {
      r = -EAGAIN;
      break;
    }
    wait_socket(s, false);
  }
  if (r == 0 && pending_bytes(s) >= 4 + size) {
    const size_t take = size < len ? size : len;
    if (flags & MSG_PEEK) {
      char* frame = static_cast<char*>(malloc(4 + size));
      r = frame ? w32.recv(s, frame, static_cast<int>(4 + size), W_MSG_PEEK) : -ENOMEM;
      if (r >= 0) {
        memcpy(buf, frame + 4, take);
        r = static_cast<long>(take);
      }
      free(frame);
    } else {
      r = recv_all(s, reinterpret_cast<char*>(&size), 4);
      if (r > 0) r = recv_all(s, static_cast<char*>(buf), take);
      for (size_t left = size - take; r >= 0 && left > 0;) {
        char scratch[4096];
        const size_t chunk = left < sizeof scratch ? left : sizeof scratch;
        long n = recv_all(s, scratch, chunk);
        if (n <= 0) {
          r = n < 0 ? n : -ECONNRESET;
          break;
        }
        left -= chunk;
      }
      if (r >= 0) r = static_cast<long>(take);
    }
    if (r >= 0 && (flags & MSG_TRUNC)) r = static_cast<long>(size);
  }
  w32.ReleaseSRWLockExclusive(&info->recv_lock);
  return r;
}

long send_packet(W32File* f, const void* buf, size_t len, int flags) {
  if (len > kMaximumPacket) return -EMSGSIZE;
  SOCKET s = sock(f);
  auto* info = static_cast<SocketInfo*>(f->extra);
  const bool nonblocking = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
  char stack[4096];
  char* frame = 4 + len <= sizeof stack ? stack : static_cast<char*>(malloc(4 + len));
  if (!frame) return -ENOMEM;
  const uint32_t size = static_cast<uint32_t>(len);
  memcpy(frame, &size, 4);
  memcpy(frame + 4, buf, len);
  w32.AcquireSRWLockExclusive(&info->send_lock);
  long r = static_cast<long>(len);
  for (size_t sent = 0; sent < 4 + len;) {
    int n = w32.send(s, frame + sent, static_cast<int>(4 + len - sent), 0);
    if (n < 0) {
      int err = w32.WSAGetLastError();
      if (err == W_WSAEWOULDBLOCK) {
        if (sent == 0 && nonblocking) {
          r = -EAGAIN;
          break;
        }
        wait_socket(s, true);
        continue;
      }
      r = (err == W_WSAECONNABORTED || err == W_WSAECONNRESET || err == W_WSAESHUTDOWN) ? -EPIPE : -w32_errno_from_wsa(err);
      break;
    }
    sent += static_cast<size_t>(n);
  }
  w32.ReleaseSRWLockExclusive(&info->send_lock);
  if (frame != stack) free(frame);
  return r;
}

int recv_now(W32File* f, char* buf, uint32_t len, DWORD flags, int* err) {
  SOCKET sk = sock(f);
  W32_WSABUF b{len, buf};
  DWORD got = 0, wflags = flags;
  if (f->flags & O_NONBLOCK) {
    if (w32.WSARecv(sk, &b, 1, &got, &wflags, nullptr, nullptr) == 0) return static_cast<int>(got);
    *err = w32.WSAGetLastError();
    return -1;
  }
  constexpr uintptr_t kPending = 0x103;
  W32_OVERLAPPED ov{};
  ov.internal = kPending;
  if (w32.WSARecv(sk, &b, 1, &got, &wflags, &ov, nullptr) == 0) return static_cast<int>(got);
  if ((*err = w32.WSAGetLastError()) == W_WSA_IO_PENDING) {
    w32.CancelIoEx(reinterpret_cast<HANDLE>(sk), &ov);
    while (__atomic_load_n(&ov.internal, __ATOMIC_ACQUIRE) == kPending) w32.SwitchToThread();
    *err = W_WSAEWOULDBLOCK;
  }
  return -1;
}

// winsock queues each icmp error in the receive queue, linux keeps one ahead of data
bool drop_icmp_entries(W32File* f) {
  bool any = false;
  int err = 0;
  char none;
  for (int i = 0; i < 64 && recv_now(f, &none, 0, W_MSG_PEEK, &err) < 0 && err == W_WSAECONNRESET; i++) {
    recv_now(f, &none, 0, 0, &err);
    any = true;
  }
  return any;
}

void pull_icmp(W32File* f, SocketInfo* s) {
  if (udp_socket(s) && s->connected && drop_icmp_entries(f)) __atomic_store_n(&s->sk_err, ECONNREFUSED, __ATOMIC_RELAXED);
}

long take_error(SocketInfo* s) {
  return __atomic_load_n(&s->sk_err, __ATOMIC_RELAXED) ? -__atomic_exchange_n(&s->sk_err, 0, __ATOMIC_RELAXED) : 0;
}

// FIONREAD: linux says the next datagram's size, winsock all of them
long next_datagram(W32File* f, SocketInfo* s, int* out) {
  pull_icmp(f, s);
  const uint32_t queued = pending_bytes(sock(f));
  int size = 0;
  if (queued) {
    char stack[1536];
    const uint32_t room = queued < 65536 ? queued : 65536;
    char* buf = room <= sizeof stack ? stack : static_cast<char*>(malloc(room));
    if (!buf) return -ENOMEM;
    int err = 0;
    const int r = recv_now(f, buf, room, W_MSG_PEEK, &err);
    size = r >= 0 ? r : err == W_WSAEMSGSIZE ? static_cast<int>(room) : 0;
    if (buf != stack) free(buf);
  }
  *out = size;
  return 0;
}

char* gather(const iovec* iov, size_t count, size_t total) {
  char* buf = static_cast<char*>(malloc(total ? total : 1));
  if (!buf) return nullptr;
  size_t at = 0;
  for (size_t i = 0; i < count; i++) {
    memcpy(buf + at, iov[i].iov_base, iov[i].iov_len);
    at += iov[i].iov_len;
  }
  return buf;
}

void scatter(const iovec* iov, size_t count, const char* buf, size_t len) {
  for (size_t i = 0, at = 0; i < count && at < len; i++) {
    const size_t n = iov[i].iov_len < len - at ? iov[i].iov_len : len - at;
    memcpy(iov[i].iov_base, buf + at, n);
    at += n;
  }
}

class Buffers {
 public:
  Buffers() = default;
  Buffers(const Buffers&) = delete;
  Buffers& operator=(const Buffers&) = delete;
  ~Buffers() {
    if (list_ != inline_) free(list_);
  }
  long set(const iovec* iov, size_t count) {
    if (count == 0) {
      inline_[0] = W32_WSABUF{0, nullptr};
      count_ = 1;
      return 0;
    }
    if (count > kInline) {
      list_ = static_cast<W32_WSABUF*>(malloc(count * sizeof(W32_WSABUF)));
      if (!list_) {
        list_ = inline_;
        return -ENOMEM;
      }
    }
    for (size_t i = 0; i < count; i++) {
      size_t len = iov[i].iov_len;
      if (len > 0x7fffffff - total_) len = 0x7fffffff - total_;
      list_[i] = W32_WSABUF{static_cast<uint32_t>(len), static_cast<char*>(iov[i].iov_base)};
      total_ += len;
    }
    count_ = static_cast<DWORD>(count);
    return 0;
  }
  W32_WSABUF* list() { return list_; }
  DWORD count() const { return count_; }
  size_t total() const { return total_; }

 private:
  static constexpr size_t kInline = 8;
  W32_WSABUF inline_[kInline];
  W32_WSABUF* list_ = inline_;
  DWORD count_ = 0;
  size_t total_ = 0;
};

long io_errno(const SocketInfo* s, int err, bool sending, bool* again) {
  *again = false;
  if (udp_socket(s) && (err == W_WSAECONNRESET || err == W_WSAENETRESET)) {
    if (err == W_WSAECONNRESET && s->connected) return -ECONNREFUSED;
    *again = true;
    return -EAGAIN;
  }
  if (err == W_WSAETIMEDOUT && (sending ? s->sndtimeo_ns : s->rcvtimeo_ns)) return -EAGAIN;
  if (sending && (err == W_WSAECONNABORTED || err == W_WSAECONNRESET || err == W_WSAESHUTDOWN)) {
    return -EPIPE;
  }
  return -w32_errno_from_wsa(err);
}

bool ready_in_time(W32File* f, int64_t timeout_ns, int flags, bool write) {
  if (timeout_ns <= 0 || (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return true;
  return socket_ready(sock(f), write, timeout_ns);
}

long recv_buffers(W32File* f, const SocketInfo* s, Buffers& b, int flags, void* from, int* fromlen, int* msg_flags) {
  SOCKET sk = sock(f);
  const bool emulate_dontwait = (flags & MSG_DONTWAIT) && !(f->flags & O_NONBLOCK);
  const int from_size = fromlen ? *fromlen : 0;
  if (!ready_in_time(f, s->rcvtimeo_ns, flags, false)) return -EAGAIN;
  for (int tries = 0;; tries++) {
    if (emulate_dontwait && !readable_now(sk)) return -EAGAIN;
    DWORD got = 0, wflags = static_cast<DWORD>(to_winsock_msg_flags(flags));
    int rc;
    if (from) {
      *fromlen = from_size;
      rc = w32.WSARecvFrom(sk, b.list(), b.count(), &got, &wflags, from, fromlen, nullptr, nullptr);
    } else {
      rc = w32.WSARecv(sk, b.list(), b.count(), &got, &wflags, nullptr, nullptr);
    }
    if (rc == 0) return static_cast<long>(got);
    const int err = w32.WSAGetLastError();
    if (err == W_WSAEMSGSIZE && s->type != SOCK_STREAM) {
      if (msg_flags) *msg_flags |= MSG_TRUNC;
      return static_cast<long>(b.total());
    }
    bool again;
    long r = io_errno(s, err, false, &again);
    if (r == -ECONNREFUSED) drop_icmp_entries(f);
    if (!again || tries >= 16) return r;
  }
}

long send_buffers(W32File* f, const SocketInfo* s, W32_WSABUF* list, DWORD count, int flags, const void* to, int tolen) {
  SOCKET sk = sock(f);
  const bool emulate_dontwait = (flags & MSG_DONTWAIT) && !(f->flags & O_NONBLOCK);
  const DWORD wflags = static_cast<DWORD>(to_winsock_msg_flags(flags));
  if (!ready_in_time(f, s->sndtimeo_ns, flags, true)) return -EAGAIN;
  for (int tries = 0;; tries++) {
    if (emulate_dontwait && !writable_now(sk)) return -EAGAIN;
    DWORD sent = 0;
    int rc = to ? w32.WSASendTo(sk, list, count, &sent, wflags, to, tolen, nullptr, nullptr)
                : w32.WSASend(sk, list, count, &sent, wflags, nullptr, nullptr);
    if (rc == 0) return static_cast<long>(sent);
    bool again;
    long r = io_errno(s, w32.WSAGetLastError(), true, &again);
    if (!again || tries >= 16) return r;
  }
}

// UDP_SEGMENT without offload on win7: a send per segment, a tail that finds no room drops like a full queue
long send_segments(W32File* f, const SocketInfo* s, const char* data, size_t len, int flags, const void* to, int tolen,
                   size_t segment) {
  if (len > segment * kUdpMaxSegments) return -EINVAL;
  static bool logged;
  if (!logged) {
    logged = true;
    w32_debug("net: first segmented send: %zu bytes as %zu-byte datagrams", len, segment);
  }
  int64_t give_up = 0;
  for (size_t at = 0; at < len; at += segment) {
    const size_t n = len - at < segment ? len - at : segment;
    W32_WSABUF one{static_cast<uint32_t>(n), const_cast<char*>(data + at)};
    long r = send_buffers(f, s, &one, 1, flags, to, tolen);
    if (r == -EAGAIN && at > 0) {
      int64_t now = w32_monotonic_ns();
      if (give_up == 0) give_up = now + kSegmentStallNs;
      while (r == -EAGAIN && now < give_up) {
        socket_ready(sock(f), true, give_up - now);
        r = send_buffers(f, s, &one, 1, flags, to, tolen);
        now = w32_monotonic_ns();
      }
    }
    if (r < 0) {
      if (at == 0) return r;
      break;
    }
  }
  return static_cast<long>(len);
}

long segment_size(const SocketInfo* s, const msghdr* m, size_t* segment) {
  *segment = 0;
  if (!udp_socket(s)) return 0;
  *segment = static_cast<size_t>(s->udp_segment);
  if (!m->msg_control || m->msg_controllen < sizeof(cmsghdr)) return 0;
  msghdr* mm = const_cast<msghdr*>(m);
  const char* end = static_cast<const char*>(m->msg_control) + m->msg_controllen;
  for (cmsghdr* c = CMSG_FIRSTHDR(mm); c; c = CMSG_NXTHDR(mm, c)) {
    if (c->cmsg_len < sizeof(cmsghdr) || c->cmsg_len > static_cast<size_t>(end - reinterpret_cast<const char*>(c)))
      return -EINVAL;
    if (c->cmsg_level != SOL_UDP) continue;
    if (c->cmsg_type != UDP_SEGMENT || c->cmsg_len != CMSG_LEN(sizeof(uint16_t))) return -EINVAL;
    uint16_t size;
    memcpy(&size, CMSG_DATA(c), sizeof size);
    *segment = size;
  }
  return 0;
}

struct W32_WSAMSG {
  void* name;
  int32_t namelen;
  W32_WSABUF* buffers;
  uint32_t buffer_count;
  W32_WSABUF control;
  uint32_t flags;
};
using WsaRecvMsgFn = int(WINAPI*)(SOCKET, W32_WSAMSG*, DWORD*, void*, void*);

WsaRecvMsgFn recvmsg_function(SOCKET s) {
  static WsaRecvMsgFn cached;
  WsaRecvMsgFn fn = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
  if (fn) return fn;
  // WSAID_WSARECVMSG {f689d7c8-6f1f-436b-8a53-e54fe351c322}
  static const uint8_t kGuid[16] = {0xc8, 0xd7, 0x89, 0xf6, 0x1f, 0x6f, 0x6b, 0x43,
                                    0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22};
  DWORD done = 0;
  if (w32.WSAIoctl(s, W_SIO_GET_EXTENSION_FUNCTION_POINTER, const_cast<uint8_t*>(kGuid), sizeof kGuid, &fn, sizeof fn,
                   &done, nullptr, nullptr) != 0) {
    return nullptr;
  }
  __atomic_store_n(&cached, fn, __ATOMIC_RELEASE);
  return fn;
}

struct Cmsgs {
  void* out;
  size_t room, used;
  int* msg_flags;
  void put(int level, int type, const void* data, size_t len) {
    const size_t left = room - used;
    if (!out || left < sizeof(cmsghdr)) {
      *msg_flags |= MSG_CTRUNC;
      return;
    }
    size_t cmlen = CMSG_LEN(len);
    if (left < cmlen) {
      *msg_flags |= MSG_CTRUNC;
      cmlen = left;
    }
    cmsghdr header{};
    header.cmsg_len = cmlen;
    header.cmsg_level = level;
    header.cmsg_type = type;
    char* at = static_cast<char*>(out) + used;
    memcpy(at, &header, sizeof header);
    memcpy(at + CMSG_LEN(0), data, cmlen - CMSG_LEN(0));
    const size_t space = CMSG_SPACE(len);
    used += space < left ? space : left;
  }
};

void put_timestamp(Cmsgs& c, int type) {
  static int64_t offset;
  int64_t o = __atomic_load_n(&offset, __ATOMIC_RELAXED);
  if (!o) {
    o = (w32_realtime_ns() - w32_monotonic_ns()) | 1;
    __atomic_store_n(&offset, o, __ATOMIC_RELAXED);
  }
  const int64_t ns = w32_monotonic_ns() + o;
  const bool usec = type == SCM_TIMESTAMP || type == SO_TIMESTAMP_NEW;
  const int64_t t[2] = {ns / 1000000000, usec ? ns % 1000000000 / 1000 : ns % 1000000000};
  c.put(SOL_SOCKET, type, t, sizeof t);
}

void ip_cmsgs_to_linux(const SocketInfo* s, const char* wc, size_t wlen, Cmsgs& c) {
  bool v4 = false, v6 = false, tos = false, tclass = false;
  uint32_t addr = 0, ifindex = 0;
  LinuxIn6Pktinfo p6{};
  uint8_t tos_byte = 0;
  int tclass_value = 0;
  for (size_t at = 0; at + 16 <= wlen;) {
    uint64_t clen;
    int32_t level, type;
    memcpy(&clen, wc + at, 8);
    memcpy(&level, wc + at + 8, 4);
    memcpy(&type, wc + at + 12, 4);
    if (clen < 16 || clen > wlen - at) break;
    const char* data = wc + at + 16;
    if (level == IPPROTO_IP && type == W_IP_PKTINFO && clen >= 16 + 8) {
      memcpy(&addr, data, 4);
      memcpy(&ifindex, data + 4, 4);
      v4 = true;
    } else if (level == IPPROTO_IP && (type == W_IP_TOS || type == W_IP_TCLASS) && clen >= 16 + 1) {
      tos_byte = static_cast<uint8_t>(data[0]);
      tos = true;
    } else if (level == IPPROTO_IPV6 && type == W_IPV6_PKTINFO && clen >= 16 + 20) {
      memcpy(&p6, data, sizeof p6);
      v6 = true;
    } else if (level == IPPROTO_IPV6 && type == W_IPV6_TCLASS && clen >= 16 + 1) {
      tclass_value = static_cast<uint8_t>(data[0]);
      tclass = true;
    }
    at += (clen + 7) & ~static_cast<uint64_t>(7);
  }
  if (v4 && s->domain == AF_INET6 && s->pktinfo6) {
    LinuxIn6Pktinfo p{};
    p.addr[10] = p.addr[11] = 0xff;
    memcpy(p.addr + 12, &addr, 4);
    p.ifindex = static_cast<int>(ifindex);
    c.put(SOL_IPV6, IPV6_PKTINFO, &p, sizeof p);
  }
  if (v4 && s->pktinfo) {
    const LinuxInPktinfo p{static_cast<int>(ifindex), addr, addr};
    c.put(SOL_IP, IP_PKTINFO, &p, sizeof p);
  }
  if (tos && s->recvtos) c.put(SOL_IP, IP_TOS, &tos_byte, 1);
  if (v6 && s->pktinfo6) c.put(SOL_IPV6, IPV6_PKTINFO, &p6, sizeof p6);
  if (tclass && s->recvtclass) c.put(SOL_IPV6, IPV6_TCLASS, &tclass_value, sizeof tclass_value);
}

long recv_with_control(W32File* f, const SocketInfo* s, WsaRecvMsgFn fn, Buffers& b, int flags, void* from, int* fromlen,
                       char* wcontrol, size_t wroom, size_t* wlen, int* msg_flags) {
  SOCKET sk = sock(f);
  const bool emulate_dontwait = (flags & MSG_DONTWAIT) && !(f->flags & O_NONBLOCK);
  if (!ready_in_time(f, s->rcvtimeo_ns, flags, false)) return -EAGAIN;
  for (int tries = 0;; tries++) {
    if (emulate_dontwait && !readable_now(sk)) return -EAGAIN;
    W32_WSAMSG m{};
    m.name = from;
    m.namelen = from ? *fromlen : 0;
    m.buffers = b.list();
    m.buffer_count = b.count();
    m.control = W32_WSABUF{static_cast<uint32_t>(wroom), wcontrol};
    m.flags = static_cast<uint32_t>(to_winsock_msg_flags(flags));
    DWORD got = 0;
    long r;
    if (fn(sk, &m, &got, nullptr, nullptr) == 0) {
      r = static_cast<long>(got);
    } else {
      const int err = w32.WSAGetLastError();
      if (err != W_WSAEMSGSIZE) {
        bool again;
        r = io_errno(s, err, false, &again);
        if (r == -ECONNREFUSED) drop_icmp_entries(f);
        if (again && tries < 16) continue;
        return r;
      }
      *msg_flags |= MSG_TRUNC;
      r = static_cast<long>(b.total());
    }
    if (m.flags & W_MSG_TRUNC) *msg_flags |= MSG_TRUNC;
    if (from) *fromlen = m.namelen;
    *wlen = m.control.len;
    return r;
  }
}

void report_sender(const SocketInfo* s, void* wbuf, int wlen, void* name, socklen_t* namelen) {
  if (!name || !namelen) return;
  if (inet_socket(s) && s->type == SOCK_STREAM) {
    *namelen = 0;
    return;
  }
  if (netlink_socket(s)) {
    const sockaddr_nl kernel{AF_NETLINK, 0, 0, 0};
    memcpy(name, &kernel, *namelen < sizeof kernel ? *namelen : sizeof kernel);
    *namelen = sizeof kernel;
    return;
  }
  from_winsock(wbuf, wlen, static_cast<sockaddr*>(name), namelen);
}

long nl_sendmsg(SocketInfo* s, const msghdr* m, size_t total) {
  if (m->msg_name && m->msg_namelen) {
    if (m->msg_namelen < sizeof(sockaddr_nl)) return -EINVAL;
    sockaddr_nl to;
    memcpy(&to, m->msg_name, sizeof to);
    if (to.nl_family != AF_NETLINK) return -EINVAL;
    if (to.nl_pid || to.nl_groups) return -EPERM;
  }
  if (total + 32 > static_cast<size_t>(s->sndbuf ? s->sndbuf : kLinuxWmemDefault)) return -EMSGSIZE;
  if (!s->nl_bound) nl_autobind(s);
  char* buf = gather(m->msg_iov, m->msg_iovlen, total);
  if (!buf) return -ENOMEM;
  long r = nl_policy(buf, total);
  if (r == 0) {
    nl_process(s, buf, total);
    r = static_cast<long>(total);
  }
  free(buf);
  return r;
}

long msg_send(W32File* f, SocketInfo* s, const msghdr* m, int flags) {
  if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
  if (m->msg_iovlen > UIO_MAXIOV) return -EMSGSIZE;
  size_t total = 0;
  for (size_t i = 0; i < m->msg_iovlen; i++) total += m->msg_iov[i].iov_len;
  if (netlink_socket(s)) return nl_sendmsg(s, m, total);
  if (is_seqpacket(f)) {
    char* buf = gather(m->msg_iov, m->msg_iovlen, total);
    if (!buf) return -ENOMEM;
    long r = send_packet(f, buf, total, flags);
    free(buf);
    return r;
  }
  size_t segment = 0;
  long bad = segment_size(s, m, &segment);
  if (bad < 0) return bad;
  char to[128];
  int tolen = 0;
  if (m->msg_name && m->msg_namelen && !s->unix_emulated) {
    tolen = to_winsock(m->msg_name, m->msg_namelen, to, sizeof to);
    if (tolen < 0) return tolen;
  }
  Buffers b;
  long r = b.set(m->msg_iov, m->msg_iovlen);
  if (r < 0) return r;
  if (udp_socket(s) && (r = take_error(s)) != 0) return r;
  if (segment == 0 || b.total() <= segment) return send_buffers(f, s, b.list(), b.count(), flags, tolen ? to : nullptr, tolen);
  if (b.count() == 1) return send_segments(f, s, b.list()[0].buf, b.total(), flags, tolen ? to : nullptr, tolen, segment);
  char* joined = gather(m->msg_iov, m->msg_iovlen, b.total());
  if (!joined) return -ENOMEM;
  r = send_segments(f, s, joined, b.total(), flags, tolen ? to : nullptr, tolen, segment);
  free(joined);
  return r;
}

long msg_recv(W32File* f, SocketInfo* s, msghdr* m, int flags) {
  if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
  if (m->msg_iovlen > UIO_MAXIOV) return -EMSGSIZE;
  if (flags & MSG_ERRQUEUE) return -EAGAIN;
  const size_t room = m->msg_control ? m->msg_controllen : 0;
  m->msg_controllen = 0;
  m->msg_flags = 0;
  if (is_seqpacket(f)) {
    if (m->msg_name && m->msg_namelen >= sizeof(sa_family_t)) {
      static_cast<sockaddr_un*>(m->msg_name)->sun_family = AF_UNIX;
      m->msg_namelen = sizeof(sa_family_t);
    }
    size_t total = 0;
    for (size_t i = 0; i < m->msg_iovlen; i++) total += m->msg_iov[i].iov_len;
    char* buf = static_cast<char*>(malloc(total ? total : 1));
    if (!buf) return -ENOMEM;
    long r = recv_packet(f, buf, total, flags);
    if (r > 0) scatter(m->msg_iov, m->msg_iovlen, buf, static_cast<size_t>(r) < total ? static_cast<size_t>(r) : total);
    free(buf);
    return r;
  }
  Buffers b;
  long r = b.set(m->msg_iov, m->msg_iovlen);
  if (r < 0) return r;
  if (udp_socket(s) && (r = take_error(s)) != 0) return r;
  if (netlink_socket(s)) {
    const int asked = b.total() < static_cast<size_t>(kNlMaxRecv) ? static_cast<int>(b.total()) : kNlMaxRecv;
    if (asked > s->nl_max_recv) s->nl_max_recv = asked;
  }
  long whole = -1;
  if ((udp_socket(s) || netlink_socket(s)) && (flags & MSG_TRUNC)) {
    char* scratch = static_cast<char*>(malloc(65536));
    if (!scratch) return -ENOMEM;
    iovec sv{scratch, 65536};
    Buffers sb;
    sb.set(&sv, 1);
    int ignored = 0;
    whole = recv_buffers(f, s, sb, (flags | MSG_PEEK) & ~MSG_TRUNC, nullptr, nullptr, &ignored);
    free(scratch);
    if (whole < 0) return whole;
  }
  char from[128];
  int fromlen = sizeof from;
  void* from_buf = m->msg_name ? from : nullptr;
  int msg_flags = 0;
  WsaRecvMsgFn fn = nullptr;
  if (udp_socket(s) && (s->pktinfo || s->pktinfo6 || s->recvtos || s->recvtclass)) fn = recvmsg_function(sock(f));
  alignas(8) char wcontrol[256];
  size_t wlen = 0;
  if (fn) r = recv_with_control(f, s, fn, b, flags, from_buf, &fromlen, wcontrol, sizeof wcontrol, &wlen, &msg_flags);
  else r = recv_buffers(f, s, b, flags, from_buf, &fromlen, &msg_flags);
  if (r >= 0) {
    Cmsgs c{m->msg_control, room, 0, &msg_flags};
    if (s->timestamp && inet_socket(s) && (r > 0 || s->type != SOCK_STREAM)) put_timestamp(c, s->timestamp);
    if (fn) ip_cmsgs_to_linux(s, wcontrol, wlen, c);
    m->msg_controllen = c.used;
    report_sender(s, from, fromlen, m->msg_name, &m->msg_namelen);
    if (whole >= 0) {
      if (static_cast<size_t>(whole) > b.total()) msg_flags |= MSG_TRUNC;
      r = whole;
    }
  }
  m->msg_flags = msg_flags;
  return r;
}

msghdr one_buffer(iovec* v, void* buf, size_t len) {
  v->iov_base = buf;
  v->iov_len = len;
  msghdr m{};
  m.msg_iov = v;
  m.msg_iovlen = 1;
  return m;
}

}  // namespace

long w32_socket_read(W32File* f, void* buf, size_t len) {
  iovec v;
  msghdr m = one_buffer(&v, buf, len);
  return msg_recv(f, static_cast<SocketInfo*>(f->extra), &m, 0);
}

long w32_socket_write(W32File* f, const void* buf, size_t len) {
  iovec v;
  msghdr m = one_buffer(&v, const_cast<void*>(buf), len);
  return msg_send(f, static_cast<SocketInfo*>(f->extra), &m, 0);
}

long w32_socket_vector_io(W32File* f, const iovec* iov, int count, bool writing) {
  msghdr m{};
  m.msg_iov = const_cast<iovec*>(iov);
  m.msg_iovlen = static_cast<size_t>(count);
  auto* s = static_cast<SocketInfo*>(f->extra);
  return writing ? msg_send(f, s, &m, 0) : msg_recv(f, s, &m, 0);
}

short w32_socket_listener_shut(W32File* f, short events) {
  auto* s = static_cast<SocketInfo*>(f->extra);
  if (!s || !s->listening || !__atomic_load_n(&s->listen_shut, __ATOMIC_SEQ_CST)) return 0;
  return static_cast<short>(POLLHUP | (events & (POLLIN | POLLRDNORM | POLLRDHUP)) |
                            (s->shut_wr ? 0 : events & (POLLOUT | POLLWRNORM)));
}

short w32_socket_readable(W32File* f, short events) {
  auto* s = static_cast<SocketInfo*>(f->extra);
  const short in = static_cast<short>(events & (POLLIN | POLLRDNORM));
  if (udp_socket(s) && s->connected) {
    pull_icmp(f, s);
    if (!__atomic_load_n(&s->sk_err, __ATOMIC_RELAXED)) return in;
    return static_cast<short>(POLLERR | (readable_now(sock(f)) ? in : 0));
  }
  if (!s || s->listening || (s->type != SOCK_STREAM && s->type != SOCK_SEQPACKET)) return in;
  if (pending_bytes(sock(f)) > 0) return in;
  char b;
  int err = 0;
  const int r = recv_now(f, &b, 1, W_MSG_PEEK, &err);
  const short rdhup = static_cast<short>(events & POLLRDHUP);
  if (r == 0) return static_cast<short>(in | rdhup | ((s->domain == AF_UNIX || s->shut_wr) ? POLLHUP : 0));
  if (r < 0 && err != W_WSAEWOULDBLOCK) return static_cast<short>(in | POLLERR | POLLHUP | rdhup);
  return in;
}

short w32_socket_error(W32File* f) {
  auto* s = static_cast<SocketInfo*>(f->extra);
  return s && __atomic_load_n(&s->sk_err, __ATOMIC_RELAXED) ? static_cast<short>(POLLERR) : 0;
}

long sys_recvfrom(long fd, long buf, long len, long flags, long addr, long alen) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    iovec v;
    msghdr m = one_buffer(&v, reinterpret_cast<void*>(buf), static_cast<size_t>(len));
    auto* addrlen = reinterpret_cast<socklen_t*>(alen);
    if (addr && addrlen) {
      m.msg_name = reinterpret_cast<void*>(addr);
      m.msg_namelen = *addrlen;
    }
    long r = msg_recv(f, s, &m, static_cast<int>(flags));
    if (r >= 0 && m.msg_name) *addrlen = m.msg_namelen;
    return r;
  });
}

long sys_sendto(long fd, long buf, long len, long flags, long addr, long alen) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    iovec v;
    msghdr m = one_buffer(&v, reinterpret_cast<void*>(buf), static_cast<size_t>(len));
    m.msg_name = reinterpret_cast<void*>(addr);
    m.msg_namelen = static_cast<socklen_t>(alen);
    return msg_send(f, s, &m, static_cast<int>(flags));
  });
}

long sys_recvmsg(long fd, long msg, long flags, long, long, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) {
    return msg_recv(f, s, reinterpret_cast<msghdr*>(msg), static_cast<int>(flags));
  });
}

long sys_sendmsg(long fd, long msg, long flags, long, long, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) {
    return msg_send(f, s, reinterpret_cast<const msghdr*>(msg), static_cast<int>(flags));
  });
}

long sys_recvmmsg(long fd, long msgs_l, long count, long flags, long, long) {
  auto* msgs = reinterpret_cast<mmsghdr*>(msgs_l);
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    long done = 0;
    for (long i = 0; i < count; i++) {
      long r = msg_recv(f, s, &msgs[i].msg_hdr, static_cast<int>(flags) | (i ? MSG_DONTWAIT : 0));
      if (r < 0 && done && r != -EAGAIN && udp_socket(s)) __atomic_store_n(&s->sk_err, static_cast<int>(-r), __ATOMIC_RELAXED);
      if (r < 0) return done ? done : r;
      msgs[i].msg_len = static_cast<unsigned>(r);
      done++;
      if (flags & MSG_WAITFORONE) flags |= MSG_DONTWAIT;
    }
    return done;
  });
}

long sys_sendmmsg(long fd, long msgs_l, long count, long flags, long, long) {
  auto* msgs = reinterpret_cast<mmsghdr*>(msgs_l);
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    long done = 0;
    for (long i = 0; i < count; i++) {
      long r = msg_send(f, s, &msgs[i].msg_hdr, static_cast<int>(flags));
      if (r < 0) return done ? done : r;
      msgs[i].msg_len = static_cast<unsigned>(r);
      done++;
    }
    return done;
  });
}

namespace {

bool map_option(int level, int name, int* wlevel, int* wname) {
  *wlevel = level;
  if (level == SOL_SOCKET) {
    *wlevel = W_SOL_SOCKET;
    switch (name) {
      case SO_DONTROUTE: *wname = W_SO_DONTROUTE; return true;
      case SO_BROADCAST: *wname = W_SO_BROADCAST; return true;
      case SO_LINGER: *wname = W_SO_LINGER; return true;
      case SO_OOBINLINE: *wname = W_SO_OOBINLINE; return true;
      case SO_SNDTIMEO: *wname = W_SO_SNDTIMEO; return true;
      case SO_RCVTIMEO: *wname = W_SO_RCVTIMEO; return true;
      case SO_ERROR: *wname = W_SO_ERROR; return true;
      default: return false;
    }
  }
  if (level == IPPROTO_IP) {
    switch (name) {
      case IP_TOS: *wname = W_IP_TOS; return true;
      case IP_TTL: *wname = W_IP_TTL; return true;
      case IP_MULTICAST_IF: *wname = W_IP_MULTICAST_IF; return true;
      case IP_MULTICAST_TTL: *wname = W_IP_MULTICAST_TTL; return true;
      case IP_MULTICAST_LOOP: *wname = W_IP_MULTICAST_LOOP; return true;
      case IP_ADD_MEMBERSHIP: *wname = W_IP_ADD_MEMBERSHIP; return true;
      case IP_DROP_MEMBERSHIP: *wname = W_IP_DROP_MEMBERSHIP; return true;
      default: return false;
    }
  }
  if (level == IPPROTO_IPV6) {
    switch (name) {
      case IPV6_UNICAST_HOPS: *wname = W_IPV6_UNICAST_HOPS; return true;
      case IPV6_MULTICAST_IF: *wname = W_IPV6_MULTICAST_IF; return true;
      case IPV6_MULTICAST_HOPS: *wname = W_IPV6_MULTICAST_HOPS; return true;
      case IPV6_MULTICAST_LOOP: *wname = W_IPV6_MULTICAST_LOOP; return true;
      case IPV6_ADD_MEMBERSHIP: *wname = W_IPV6_ADD_MEMBERSHIP; return true;
      case IPV6_DROP_MEMBERSHIP: *wname = W_IPV6_DROP_MEMBERSHIP; return true;
      case IPV6_V6ONLY: *wname = W_IPV6_V6ONLY; return true;
      default: return false;
    }
  }
  if (level == IPPROTO_TCP) {
    *wname = name;
    return name == TCP_NODELAY;
  }
  return false;
}

enum : uint8_t { kBool, kInt, kIgnore, kGetOnly, kDenyAll, kDenyNonzero };
struct OptRule {
  int16_t level, name;
  uint8_t kind, err;
  int def, lo, hi;
};
constexpr OptRule kRules[] = {
    {SOL_SOCKET, SO_DEBUG, kDenyNonzero, EACCES, 0, 0, 0},
    {SOL_SOCKET, SO_NO_CHECK, kBool, 0, 0, 0, 0},
    {SOL_SOCKET, SO_PRIORITY, kInt, EPERM, 0, 0, 6},
    {SOL_SOCKET, SO_BSDCOMPAT, kIgnore, 0, 0, 0, 0},
    {SOL_SOCKET, SO_PASSCRED, kBool, 0, 0, 0, 0},
    {SOL_SOCKET, SO_SNDLOWAT, kGetOnly, 0, 1, 0, 0},
    {SOL_SOCKET, SO_PASSSEC, kBool, 0, 0, 0, 0},
    {SOL_SOCKET, SO_MARK, kDenyAll, EPERM, 0, 0, 0},
    {SOL_SOCKET, SO_RXQ_OVFL, kBool, 0, 0, 0, 0},
    {SOL_SOCKET, SO_WIFI_STATUS, kBool, 0, 0, 0, 0},
    {SOL_SOCKET, SO_NOFCS, kBool, 0, 0, 0, 0},
    {SOL_SOCKET, SO_SELECT_ERR_QUEUE, kBool, 0, 0, 0, 0},
    {SOL_SOCKET, SO_MAX_PACING_RATE, kInt, 0, -1, 0, 0},
    {SOL_SOCKET, SO_INCOMING_CPU, kInt, 0, -1, 0, 0},
    {SOL_SOCKET, SO_CNX_ADVICE, kIgnore, 0, 0, 0, 0},
    {IPPROTO_IP, IP_RECVOPTS, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_RETOPTS, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_RECVTTL, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_FREEBIND, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_PASSSEC, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_TRANSPARENT, kDenyNonzero, EPERM, 0, 0, 0},
    {IPPROTO_IP, IP_RECVORIGDSTADDR, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_MINTTL, kInt, EINVAL, 0, 0, 255},
    {IPPROTO_IP, IP_CHECKSUM, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_RECVFRAGSIZE, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_RECVERR_RFC4884, kBool, 0, 0, 0, 0},
    {IPPROTO_IP, IP_MULTICAST_ALL, kBool, 0, 1, 0, 0},
    {IPPROTO_IP, IP_UNICAST_IF, kInt, 0, 0, 0, 0},
    {IPPROTO_IP, IP_IPSEC_POLICY, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_IP, IP_XFRM_POLICY, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_2292PKTINFO, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_2292HOPOPTS, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_2292DSTOPTS, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_2292RTHDR, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_2292HOPLIMIT, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_MULTICAST_ALL, kBool, 0, 1, 0, 0},
    {IPPROTO_IPV6, IPV6_ROUTER_ALERT_ISOLATE, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_RECVERR_RFC4884, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_RECVHOPLIMIT, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_RECVHOPOPTS, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_RECVRTHDR, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_RECVDSTOPTS, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_RECVPATHMTU, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_AUTOFLOWLABEL, kBool, 0, 1, 0, 0},
    {IPPROTO_IPV6, IPV6_MINHOPCOUNT, kInt, EINVAL, 0, 0, 255},
    {IPPROTO_IPV6, IPV6_RECVORIGDSTADDR, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_TRANSPARENT, kDenyNonzero, EPERM, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_UNICAST_IF, kInt, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_RECVFRAGSIZE, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_FREEBIND, kBool, 0, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_IPSEC_POLICY, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_IPV6, IPV6_XFRM_POLICY, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_TCP, TCP_MAXSEG, kInt, EINVAL, 536, 0, 32767},
    {IPPROTO_TCP, TCP_CORK, kBool, 0, 0, 0, 0},
    {IPPROTO_TCP, TCP_SYNCNT, kInt, EINVAL, 6, 1, 127},
    {IPPROTO_TCP, TCP_LINGER2, kInt, 0, 60, 0, 0},
    {IPPROTO_TCP, TCP_DEFER_ACCEPT, kInt, 0, 0, 0, 0},
    {IPPROTO_TCP, TCP_WINDOW_CLAMP, kInt, EINVAL, 0, 0, INT_MAX},
    {IPPROTO_TCP, TCP_QUICKACK, kBool, 0, 1, 0, 0},
    {IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, kBool, 0, 0, 0, 0},
    {IPPROTO_TCP, TCP_THIN_DUPACK, kBool, 0, 0, 0, 0},
    {IPPROTO_TCP, TCP_USER_TIMEOUT, kInt, EINVAL, 0, 0, INT_MAX},
    {IPPROTO_TCP, TCP_FASTOPEN, kInt, EINVAL, 0, 0, INT_MAX},
    {IPPROTO_TCP, TCP_NOTSENT_LOWAT, kInt, 0, -1, 0, 0},
    {IPPROTO_TCP, TCP_SAVE_SYN, kInt, EINVAL, 0, 0, 2},
    {IPPROTO_TCP, TCP_FASTOPEN_CONNECT, kInt, EINVAL, 0, 0, 1},
    {IPPROTO_TCP, TCP_FASTOPEN_NO_COOKIE, kInt, EINVAL, 0, 0, 1},
    {IPPROTO_TCP, TCP_INQ, kInt, EINVAL, 0, 0, 1},
    {IPPROTO_TCP, TCP_TX_DELAY, kInt, 0, 0, 0, 0},
    {IPPROTO_TCP, TCP_REPAIR, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_TCP, TCP_REPAIR_QUEUE, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_TCP, TCP_QUEUE_SEQ, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_TCP, TCP_REPAIR_OPTIONS, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_TCP, TCP_REPAIR_WINDOW, kDenyAll, EPERM, 0, 0, 0},
    {IPPROTO_TCP, TCP_FASTOPEN_KEY, kDenyAll, EPERM, 0, 0, 0},
    {SOL_UDP, UDP_NO_CHECK6_TX, kBool, 0, 0, 0, 0},
    {SOL_UDP, UDP_NO_CHECK6_RX, kBool, 0, 0, 0, 0},
};

const OptRule* find_rule(int level, int name) {
  for (const OptRule& r : kRules)
    if (r.level == level && r.name == name) return &r;
  return nullptr;
}

int kept(const SocketInfo* s, int level, int name, int def) {
  for (int k = 0; k < s->nkept; k++)
    if (s->kept[k].level == level && s->kept[k].name == name) return s->kept[k].value;
  return def;
}

void keep(SocketInfo* s, int level, int name, int v) {
  for (int k = 0; k < s->nkept; k++) {
    if (s->kept[k].level == level && s->kept[k].name == name) {
      s->kept[k].value = v;
      return;
    }
  }
  if (s->nkept < sizeof s->kept / sizeof s->kept[0]) s->kept[s->nkept++] = {static_cast<int16_t>(level), static_cast<int16_t>(name), v};
}

long level_gate(const SocketInfo* s, int level) {
  if (level == SOL_SOCKET) return 0;
  if (netlink_socket(s)) return level == SOL_NETLINK ? 0 : -ENOPROTOOPT;
  if (s->domain == AF_UNIX) return -EOPNOTSUPP;
  if (level == IPPROTO_IP) return 0;
  if (level == IPPROTO_IPV6) return s->domain == AF_INET6 ? 0 : -ENOPROTOOPT;
  if (level == IPPROTO_TCP) return tcp_socket(s) ? 0 : -ENOPROTOOPT;
  if (level == SOL_UDP) return udp_socket(s) ? 0 : -ENOPROTOOPT;
  return -ENOPROTOOPT;
}

bool read_int(int level, const void* val, socklen_t len, int* out) {
  *out = 0;
  if (len >= sizeof(int)) {
    memcpy(out, val, sizeof(int));
    return true;
  }
  if (level == IPPROTO_IP) {
    if (len >= 1) *out = *static_cast<const unsigned char*>(val);
    return true;
  }
  return level == IPPROTO_IPV6;
}

long put_int(int level, int v, void* out, socklen_t* len) {
  const int room = static_cast<int>(*len);
  if (room < 0) return -EINVAL;
  if (level == IPPROTO_IP && room > 0 && room < 4 && v >= 0 && v <= 255) {
    const unsigned char b = static_cast<unsigned char>(v);
    memcpy(out, &b, 1);
    *len = 1;
    return 0;
  }
  const size_t n = room < 4 ? static_cast<size_t>(room) : sizeof v;
  memcpy(out, &v, n);
  *len = static_cast<socklen_t>(n);
  return 0;
}

void windows_pktinfo(W32File* f, const SocketInfo* s) {
  const uint32_t v4 = s->pktinfo || (s->domain == AF_INET6 && s->pktinfo6), v6 = s->pktinfo6;
  w32.setsockopt(sock(f), IPPROTO_IP, W_IP_PKTINFO, reinterpret_cast<const char*>(&v4), sizeof v4);
  if (s->domain == AF_INET6) w32.setsockopt(sock(f), IPPROTO_IPV6, W_IPV6_PKTINFO, reinterpret_cast<const char*>(&v6), sizeof v6);
}

void windows_recvtos(W32File* f, const SocketInfo* s) {
  if (s->type != SOCK_DGRAM) return;
  const uint32_t v4 = s->recvtos, v6 = s->recvtclass;
  w32.setsockopt(sock(f), IPPROTO_IP, W_IP_RECVTOS, reinterpret_cast<const char*>(&v4), sizeof v4);
  if (s->domain == AF_INET6) w32.setsockopt(sock(f), IPPROTO_IPV6, W_IPV6_RECVTCLASS, reinterpret_cast<const char*>(&v6), sizeof v6);
}

// win7 always sends 10 keep-alive probes, TCP_KEEPCNT is just remembered
long apply_keepalive(SOCKET sk, const SocketInfo* s) {
  if (s->type != SOCK_STREAM) return 0;
  const uint32_t on = s->keepalive ? 1 : 0;
  if (w32.setsockopt(sk, W_SOL_SOCKET, W_SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof on)) return wsa_fail();
  if (!tcp_socket(s) || !on) return 0;
  struct { uint32_t onoff, time_ms, interval_ms; } v{
      1, static_cast<uint32_t>(s->keepidle ? s->keepidle : kLinuxKeepIdle) * 1000,
      static_cast<uint32_t>(s->keepintvl ? s->keepintvl : kLinuxKeepIntvl) * 1000};
  DWORD done = 0;
  return w32.WSAIoctl(sk, W_SIO_KEEPALIVE_VALS, &v, sizeof v, nullptr, 0, &done, nullptr, nullptr) ? wsa_fail() : 0;
}

// winsock's SO_REUSEADDR is linux's SO_REUSEPORT, and linux's tcp one is windows' default
void apply_reuse(W32File* f, const SocketInfo* s) {
  if (!inet_socket(s)) return;
  const uint32_t on = s->reuseport || (s->type == SOCK_DGRAM && s->reuseaddr);
  w32.setsockopt(sock(f), W_SOL_SOCKET, W_SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
}

void apply_dontfrag6(W32File* f, const SocketInfo* s) {
  if (s->type != SOCK_DGRAM) return;
  const int mode = s->mtu_discover6 - 1;
  const uint32_t df = s->dontfrag6 || (s->mtu_discover6 && (mode == IP_PMTUDISC_WANT || mode == IP_PMTUDISC_DO || mode == IP_PMTUDISC_PROBE));
  w32.setsockopt(sock(f), IPPROTO_IPV6, W_IPV6_DONTFRAG, reinterpret_cast<const char*>(&df), sizeof df);
}

long set_mtu_discover(W32File* f, SocketInfo* s, bool v6, int mode) {
  if (mode < IP_PMTUDISC_DONT || mode > IP_PMTUDISC_OMIT) return -EINVAL;
  (v6 ? s->mtu_discover6 : s->mtu_discover) = mode + 1;
  if (v6) {
    apply_dontfrag6(f, s);
  } else if (s->type == SOCK_DGRAM) {
    const uint32_t df = mode == IP_PMTUDISC_WANT || mode == IP_PMTUDISC_DO || mode == IP_PMTUDISC_PROBE;
    w32.setsockopt(sock(f), IPPROTO_IP, W_IP_DONTFRAGMENT, reinterpret_cast<const char*>(&df), sizeof df);
  }
  return 0;
}

long path_mtu(W32File* f, bool v6, int* out) {
  note_first(4, "IP_MTU / IPV6_MTU");
  char peer[128];
  int plen = sizeof peer;
  if (w32.getpeername(sock(f), peer, &plen) != 0) return -ENOTCONN;
  uint16_t family;
  memcpy(&family, peer, sizeof family);
  static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  if (family == W_AF_INET6 && memcmp(peer + 8, kMapped, sizeof kMapped) == 0) {
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    memcpy(&v4.sin_addr, peer + 20, 4);
    memcpy(peer, &v4, sizeof v4);
    family = W_AF_INET;
  }
  DWORD index = 0;
  if (iphlpapi().best_interface) iphlpapi().best_interface(peer, &index);
  bool loopback = false;
  int mtu = static_cast<int>(with_ifaces([&](const IfTable& t) -> long {
    const Iface* i = iface_by_index(t, static_cast<int>(index));
    loopback = i && (i->flags & IFF_LOOPBACK);
    return i ? static_cast<long>(i->mtu) : 1500;
  }));
  DWORD w = 0;
  int wl = sizeof w;
  const bool w6 = family == W_AF_INET6;
  if (!loopback && w32.getsockopt(sock(f), w6 ? IPPROTO_IPV6 : IPPROTO_IP, w6 ? W_IPV6_MTU : W_IP_MTU, reinterpret_cast<char*>(&w), &wl) == 0 &&
      w >= 68 && w < static_cast<DWORD>(mtu)) {
    mtu = static_cast<int>(w);
  }
  if (!v6 && mtu > 65535) mtu = 65535;
  *out = mtu;
  return 0;
}

long bind_to(W32File* f, SocketInfo* s, int index) {
  if (s->bound_if) return -EPERM;
  if (index < 0) return -EINVAL;
  s->bound_if = index;
  if (index && inet_socket(s)) {
    const uint32_t v4 = htonl(static_cast<uint32_t>(index)), v6 = static_cast<uint32_t>(index);
    w32.setsockopt(sock(f), IPPROTO_IP, W_IP_UNICAST_IF, reinterpret_cast<const char*>(&v4), sizeof v4);
    if (s->domain == AF_INET6) w32.setsockopt(sock(f), IPPROTO_IPV6, W_IPV6_UNICAST_IF, reinterpret_cast<const char*>(&v6), sizeof v6);
  }
  return 0;
}

long bind_device(W32File* f, SocketInfo* s, const void* val, socklen_t len) {
  if (static_cast<int>(len) < 0) return -EINVAL;
  char name[IFNAMSIZ] = {};
  memcpy(name, val, len < IFNAMSIZ - 1 ? len : IFNAMSIZ - 1);
  int index = 0;
  if (name[0]) {
    index = static_cast<int>(with_ifaces([&](const IfTable& t) -> long {
      const Iface* i = iface_by_name(t, name);
      return i ? i->index : -ENODEV;
    }));
    if (index < 0) return index;
  }
  return bind_to(f, s, index);
}

long get_device(const SocketInfo* s, void* out, socklen_t* len) {
  if (*len < IFNAMSIZ) return -EINVAL;
  if (!s->bound_if) {
    *len = 0;
    return 0;
  }
  char name[IFNAMSIZ] = "";
  const long found = with_ifaces([&](const IfTable& t) -> long {
    const Iface* i = iface_by_index(t, s->bound_if);
    if (i) memcpy(name, i->name, IFNAMSIZ);
    return i != nullptr;
  });
  if (!found) return -ENODEV;
  const size_t n = strlen(name) + 1;
  memcpy(out, name, n);
  *len = static_cast<socklen_t>(n);
  return 0;
}

long own_setsockopt(W32File* f, SocketInfo* s, int level, int name, const void* val, socklen_t len) {
  int v = 0;
  if (level == SOL_NETLINK) return nl_setsockopt(s, name, val, len);
  if (level == SOL_SOCKET) {
    switch (name) {
      case SO_TYPE:
      case SO_PROTOCOL:
      case SO_DOMAIN:
      case SO_ERROR:
      case SO_ACCEPTCONN:
      case SO_PEERCRED:
      case SO_PEERNAME:
      case SO_PEERSEC:
        return -ENOPROTOOPT;
      case SO_BINDTODEVICE:
        return bind_device(f, s, val, len);
      case SO_RCVTIMEO_NEW:
        name = SO_RCVTIMEO;
        break;
      case SO_SNDTIMEO_NEW:
        name = SO_SNDTIMEO;
        break;
      default:
        break;
    }
  }
  if ((level == SOL_SOCKET || level == IPPROTO_TCP || level == SOL_UDP) && len < sizeof(int)) return -EINVAL;
  if (level == SOL_SOCKET) {
    memcpy(&v, val, sizeof v);
    switch (name) {
      case SO_RCVBUF:
      case SO_SNDBUF: {
        const bool rcv = name == SO_RCVBUF;
        uint32_t size = static_cast<uint32_t>(v);
        const uint32_t ceiling = rcv ? kLinuxRmemMax : kLinuxWmemMax;
        if (size > ceiling) size = ceiling;
        const int wsize = static_cast<int>(size);
        if (!netlink_socket(s) && w32.setsockopt(sock(f), W_SOL_SOCKET, rcv ? W_SO_RCVBUF : W_SO_SNDBUF,
                                                 reinterpret_cast<const char*>(&wsize), sizeof wsize)) {
          return wsa_fail();
        }
        const int floor = rcv ? kLinuxMinRcvbuf : kLinuxMinSndbuf;
        (rcv ? s->rcvbuf : s->sndbuf) = wsize * 2 > floor ? wsize * 2 : floor;
        return 0;
      }
      case SO_RCVBUFFORCE:
      case SO_SNDBUFFORCE:
        return -EPERM;
      case SO_RCVTIMEO:
      case SO_SNDTIMEO: {
        if (len < sizeof(timeval)) return -EINVAL;
        timeval tv;
        memcpy(&tv, val, sizeof tv);
        if (tv.tv_usec < 0 || tv.tv_usec >= 1000000) return -EDOM;
        int64_t ms = tv.tv_sec < 0 ? 0 : static_cast<int64_t>(tv.tv_sec) * 1000 + (tv.tv_usec + 999) / 1000;
        if (ms > 0x7fffffff) ms = 0x7fffffff;
        const DWORD wms = static_cast<DWORD>(ms);
        if (w32.setsockopt(sock(f), W_SOL_SOCKET, name == SO_RCVTIMEO ? W_SO_RCVTIMEO : W_SO_SNDTIMEO,
                           reinterpret_cast<const char*>(&wms), sizeof wms)) {
          return wsa_fail();
        }
        (name == SO_RCVTIMEO ? s->rcvtimeo_ns : s->sndtimeo_ns) = static_cast<int64_t>(wms) * 1000000;
        return 0;
      }
      case SO_KEEPALIVE:
        s->keepalive = v != 0;
        return apply_keepalive(sock(f), s);
      case SO_REUSEADDR:
        s->reuseaddr = v != 0;
        apply_reuse(f, s);
        return 0;
      case SO_REUSEPORT:
        s->reuseport = v != 0;
        if (v) note_first(6, "SO_REUSEPORT");
        apply_reuse(f, s);
        return 0;
      case SO_TIMESTAMP_OLD:
      case SO_TIMESTAMPNS_OLD:
      case SO_TIMESTAMP_NEW:
      case SO_TIMESTAMPNS_NEW:
        s->timestamp = v ? static_cast<uint8_t>(name) : 0;
        if (v) note_first(5, "SO_TIMESTAMP");
        return 0;
      case SO_RCVLOWAT:
        keep(s, level, name, v < 0 ? INT_MAX : v ? v : 1);
        return 0;
      case SO_BUSY_POLL:
        return v < 0 ? -EINVAL : v > 0 ? -EPERM : 0;
      case SO_BINDTOIFINDEX:
        return bind_to(f, s, v);
      default:
        break;
    }
  } else if (level == SOL_UDP) {
    memcpy(&v, val, sizeof v);
    if (name == UDP_SEGMENT) {
      if (v < 0 || v > 0xffff) return -EINVAL;
      s->udp_segment = v;
      return 0;
    }
    if (name == UDP_GRO) {
      s->udp_gro = v != 0;
      return 0;
    }
  } else if (level == IPPROTO_TCP) {
    memcpy(&v, val, sizeof v);
    switch (name) {
      case TCP_KEEPIDLE:
      case TCP_KEEPINTVL:
      case TCP_KEEPCNT:
        if (v < 1 || v > (name == TCP_KEEPCNT ? 127 : 32767)) return -EINVAL;  // MAX_TCP_KEEPCNT, MAX_TCP_KEEPIDLE / _KEEPINTVL
        if (name == TCP_KEEPCNT) {
          s->keepcnt = v;
          return 0;
        }
        (name == TCP_KEEPIDLE ? s->keepidle : s->keepintvl) = v;
        return s->keepalive ? apply_keepalive(sock(f), s) : 0;
      case TCP_USER_TIMEOUT:
        if (v < 0) return -EINVAL;
        if (v > 0) {
          const uint32_t seconds = static_cast<uint32_t>((static_cast<int64_t>(v) + 999) / 1000);
          w32.setsockopt(sock(f), IPPROTO_TCP, W_TCP_MAXRT, reinterpret_cast<const char*>(&seconds), sizeof seconds);
        }
        keep(s, level, name, v);
        return 0;
      case TCP_FASTOPEN_CONNECT: {
        if (v < 0 || v > 1) return -EINVAL;
        char peer[128];
        int plen = sizeof peer;
        if (s->listening || w32.getpeername(sock(f), peer, &plen) == 0) return -EINVAL;
        keep(s, level, name, v);
        return 0;
      }
      default:
        break;
    }
  } else if (level == IPPROTO_IP) {
    read_int(level, val, len, &v);
    switch (name) {
      case IP_PKTINFO:
        s->pktinfo = v != 0;
        windows_pktinfo(f, s);
        return 0;
      case IP_RECVTOS:
        s->recvtos = v != 0;
        windows_recvtos(f, s);
        return 0;
      case IP_RECVERR:
        s->recverr = v != 0;
        return 0;
      case IP_MTU_DISCOVER:
        return set_mtu_discover(f, s, false, v);
      case IP_MTU:
        return -ENOPROTOOPT;
      default:
        break;
    }
  } else if (level == IPPROTO_IPV6) {
    read_int(level, val, len, &v);
    switch (name) {
      case IPV6_RECVPKTINFO:
        s->pktinfo6 = v != 0;
        windows_pktinfo(f, s);
        return 0;
      case IPV6_RECVTCLASS:
        s->recvtclass = v != 0;
        windows_recvtos(f, s);
        return 0;
      case IPV6_RECVERR:
        s->recverr6 = v != 0;
        return 0;
      case IPV6_MTU_DISCOVER:
        return set_mtu_discover(f, s, true, v);
      case IPV6_MTU:
        if (v && v < 1280) return -EINVAL;  // IPV6_MIN_MTU
        keep(s, level, name, v);
        return 0;
      case IPV6_DONTFRAG:
        s->dontfrag6 = v != 0;
        apply_dontfrag6(f, s);
        return 0;
      case IPV6_TCLASS: {
        if (v < -1 || v > 0xff) return -EINVAL;
        s->tclass6 = v < 0 ? 0 : v;
        const uint32_t w = static_cast<uint32_t>(s->tclass6);
        w32.setsockopt(sock(f), IPPROTO_IPV6, W_IPV6_TCLASS, reinterpret_cast<const char*>(&w), sizeof w);
        return 0;
      }
      default:
        break;
    }
  }
  int wlevel, wname;
  if (map_option(level, name, &wlevel, &wname)) return 1;
  const OptRule* r = find_rule(level, name);
  if (!r) return -ENOPROTOOPT;
  if (!read_int(level, val, len, &v)) return -EINVAL;
  switch (r->kind) {
    case kGetOnly: return -ENOPROTOOPT;
    case kDenyAll: return -r->err;
    case kDenyNonzero: return v ? -r->err : 0;
    case kIgnore: return 0;
    case kBool: keep(s, level, name, v != 0); return 0;
    default:
      if (r->err && (v < r->lo || v > r->hi)) return -r->err;
      keep(s, level, name, v);
      return 0;
  }
}

long own_getsockopt(W32File* f, const SocketInfo* s, int level, int name, int* value) {
  if (level == SOL_SOCKET) {
    switch (name) {
      case SO_RCVBUF:
        *value = s->rcvbuf ? s->rcvbuf : tcp_socket(s) ? kLinuxTcpRmemDefault : kLinuxRmemDefault;
        return 0;
      case SO_SNDBUF:
        *value = s->sndbuf ? s->sndbuf : tcp_socket(s) ? kLinuxTcpWmemDefault : kLinuxWmemDefault;
        return 0;
      case SO_KEEPALIVE:
        *value = s->keepalive;
        return 0;
      case SO_REUSEADDR:
        *value = s->reuseaddr;
        return 0;
      case SO_REUSEPORT:
        *value = s->reuseport;
        return 0;
      case SO_ACCEPTCONN:
        *value = s->listening && !__atomic_load_n(&s->listen_shut, __ATOMIC_SEQ_CST);
        return 0;
      case SO_TIMESTAMP_OLD:
      case SO_TIMESTAMPNS_OLD:
      case SO_TIMESTAMP_NEW:
      case SO_TIMESTAMPNS_NEW:
        *value = s->timestamp == name;
        return 0;
      case SO_RCVLOWAT:
        *value = kept(s, level, name, 1);
        return 0;
      case SO_BUSY_POLL:
        *value = 0;
        return 0;
      case SO_BINDTOIFINDEX:
        *value = s->bound_if;
        return 0;
      default:
        break;
    }
  } else if (level == SOL_UDP) {
    if (name == UDP_SEGMENT || name == UDP_GRO) {
      *value = name == UDP_SEGMENT ? s->udp_segment : s->udp_gro;
      return 0;
    }
  } else if (level == IPPROTO_TCP) {
    switch (name) {
      case TCP_KEEPIDLE: *value = s->keepidle ? s->keepidle : kLinuxKeepIdle; return 0;
      case TCP_KEEPINTVL: *value = s->keepintvl ? s->keepintvl : kLinuxKeepIntvl; return 0;
      case TCP_KEEPCNT: *value = s->keepcnt ? s->keepcnt : kLinuxKeepCnt; return 0;
      default: break;
    }
  } else if (level == IPPROTO_IP) {
    switch (name) {
      case IP_PKTINFO: *value = s->pktinfo; return 0;
      case IP_RECVTOS: *value = s->recvtos; return 0;
      case IP_RECVERR: *value = s->recverr; return 0;
      case IP_MTU_DISCOVER: *value = s->mtu_discover ? s->mtu_discover - 1 : IP_PMTUDISC_WANT; return 0;
      case IP_MTU: return path_mtu(f, false, value);
      default: break;
    }
  } else if (level == IPPROTO_IPV6) {
    switch (name) {
      case IPV6_RECVPKTINFO: *value = s->pktinfo6; return 0;
      case IPV6_RECVTCLASS: *value = s->recvtclass; return 0;
      case IPV6_RECVERR: *value = s->recverr6; return 0;
      case IPV6_MTU_DISCOVER: *value = s->mtu_discover6 ? s->mtu_discover6 - 1 : IPV6_PMTUDISC_WANT; return 0;
      case IPV6_MTU: return path_mtu(f, true, value);
      case IPV6_DONTFRAG: *value = s->dontfrag6; return 0;
      case IPV6_TCLASS: *value = s->tclass6; return 0;
      default: break;
    }
  }
  int wlevel, wname;
  if (map_option(level, name, &wlevel, &wname)) return 1;
  const OptRule* r = find_rule(level, name);
  if (!r) return -ENOPROTOOPT;
  *value = r->kind == kBool || r->kind == kInt ? kept(s, level, name, r->def) : r->def;
  return 0;
}

bool int_for_winsock(int level, int name) {
  return level == IPPROTO_IP && (name == IP_TOS || name == IP_TTL || name == IP_MULTICAST_TTL || name == IP_MULTICAST_LOOP);
}

}  // namespace

long sys_setsockopt(long fd, long level_l, long name_l, long val_l, long len_l, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    const int level = static_cast<int>(level_l), name = static_cast<int>(name_l);
    const auto len = static_cast<socklen_t>(len_l);
    const void* val = reinterpret_cast<const void*>(val_l);
    if (static_cast<int>(len) < 0) return -EINVAL;
    const long gate = level_gate(s, level);
    if (gate) return gate;
    const long own = own_setsockopt(f, s, level, name, val, len);
    if (own != 1) return own;
    int wlevel, wname;
    map_option(level, name, &wlevel, &wname);
    const char* wval = static_cast<const char*>(val);
    int wlen = static_cast<int>(len);
    struct { uint16_t onoff, linger; } wl;
    int v;
    if (wlevel == W_SOL_SOCKET && wname == W_SO_LINGER) {
      if (len < sizeof(linger)) return -EINVAL;
      linger l;
      memcpy(&l, val, sizeof l);
      wl = {static_cast<uint16_t>(l.l_onoff), static_cast<uint16_t>(l.l_linger)};
      wval = reinterpret_cast<const char*>(&wl);
      wlen = sizeof wl;
    } else if (int_for_winsock(level, name)) {
      read_int(level, val, len, &v);
      wval = reinterpret_cast<const char*>(&v);
      wlen = sizeof v;
    }
    return w32.setsockopt(sock(f), wlevel, wname, wval, wlen) ? wsa_fail() : 0;
  });
}

long sys_getsockopt(long fd, long level_l, long name_l, long val_l, long len_l, long) {
  return with_socket(fd, [&](W32File* f, SocketInfo* s) -> long {
    const int level = static_cast<int>(level_l);
    int name = static_cast<int>(name_l);
    auto* len = reinterpret_cast<socklen_t*>(len_l);
    void* out = reinterpret_cast<void*>(val_l);
    if (static_cast<int>(*len) < 0) return -EINVAL;
    const long gate = level_gate(s, level);
    if (gate) return gate;
    if (level == SOL_NETLINK) return nl_getsockopt(s, name, out, len);
    if (level == SOL_SOCKET) {
      if (name == SO_TYPE) return put_int(level, s->type, out, len);
      if (name == SO_DOMAIN) return put_int(level, s->domain, out, len);
      if (name == SO_PROTOCOL) return put_int(level, s->protocol, out, len);
      if (name == SO_BINDTODEVICE) return get_device(s, out, len);
      if (name == SO_ERROR && udp_socket(s)) {
        pull_icmp(f, s);
        const long e = take_error(s);
        if (e) return put_int(level, static_cast<int>(-e), out, len);
      }
      if (name == SO_PEERCRED) {
        struct { int32_t pid; uint32_t uid, gid; } cred{0, ~0u, ~0u};
        if (s->domain == AF_UNIX) cred = {getpid(), getuid(), getgid()};
        const size_t n = *len < sizeof cred ? *len : sizeof cred;
        memcpy(out, &cred, n);
        *len = static_cast<socklen_t>(n);
        return 0;
      }
      if (name == SO_RCVTIMEO_NEW) name = SO_RCVTIMEO;
      if (name == SO_SNDTIMEO_NEW) name = SO_SNDTIMEO;
    }
    int own_value = 0;
    const long own = own_getsockopt(f, s, level, name, &own_value);
    if (own < 0) return own;
    if (own == 0) return put_int(level, own_value, out, len);
    int wlevel, wname;
    map_option(level, name, &wlevel, &wname);
    char buf[64] = {};
    int wlen = sizeof buf;
    if (w32.getsockopt(sock(f), wlevel, wname, buf, &wlen)) return wsa_fail();
    if (wlevel == W_SOL_SOCKET && wname == W_SO_ERROR) return put_int(level, w32_errno_from_wsa(*reinterpret_cast<int*>(buf)), out, len);
    if (wlevel == W_SOL_SOCKET && (wname == W_SO_SNDTIMEO || wname == W_SO_RCVTIMEO)) {
      DWORD ms = *reinterpret_cast<DWORD*>(buf);
      timeval tv{static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
      memcpy(out, &tv, *len < sizeof tv ? *len : sizeof tv);
      *len = sizeof tv;
      return 0;
    }
    if (wlevel == W_SOL_SOCKET && wname == W_SO_LINGER) {
      auto* wl = reinterpret_cast<uint16_t*>(buf);
      linger l{wl[0], wl[1]};
      memcpy(out, &l, *len < sizeof l ? *len : sizeof l);
      *len = sizeof l;
      return 0;
    }
    const int v = wlen == 1 ? static_cast<unsigned char>(buf[0]) : *reinterpret_cast<int*>(buf);
    return put_int(level, v, out, len);
  });
}

namespace {

struct Out {
  char data[16384];
  size_t n = 0;
  bool ok = true;
  void bytes(const void* p, size_t len) {
    if (n + len > sizeof data) {
      ok = false;
      return;
    }
    memcpy(data + n, p, len);
    n += len;
  }
  void be32(int32_t v) {
    uint32_t b = htonl(static_cast<uint32_t>(v));
    bytes(&b, 4);
  }
  void code(int c) {
    char s[4] = {static_cast<char>('0' + c / 100), static_cast<char>('0' + c / 10 % 10), static_cast<char>('0' + c % 10), 0};
    bytes(s, 4);
  }
};

int words(char* line, char** out, int max) {
  int n = 0;
  for (char* p = line; *p && n < max;) {
    while (*p == ' ') p++;
    if (!*p) break;
    out[n++] = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
  }
  return n;
}

int parse_int(const char* s) {
  int sign = 1, v = 0;
  if (*s == '-') {
    sign = -1;
    s++;
  }
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return sign * v;
}

bool widen(const char* s, WCHAR* out, size_t n) { return w32_utf8_to_utf16(s, out, n) >= 0; }

void answer_getaddrinfo(char** w, int n, Out& out) {
  if (n < 7) {
    out.code(500);
    out.be32(EAI_FAIL);
    return;
  }
  WCHAR host[1024], service[256];
  bool has_host = strcmp(w[1], "^") != 0, has_service = strcmp(w[2], "^") != 0;
  if ((has_host && !widen(w[1], host, 1024)) || (has_service && !widen(w[2], service, 256))) {
    out.code(401);
    out.be32(EAI_NONAME);
    return;
  }
  W32_ADDRINFOW hints{};
  int flags = parse_int(w[3]), family = parse_int(w[4]), socktype = parse_int(w[5]), protocol = parse_int(w[6]);
  hints.flags = flags < 0 ? 0 : flags;
  hints.family = family < 0 ? 0 : to_winsock_family(family);
  hints.socktype = socktype < 0 ? 0 : socktype;
  hints.protocol = protocol < 0 ? 0 : protocol;
  W32_ADDRINFOW* res = nullptr;
  int err = w32.GetAddrInfoW(has_host ? host : nullptr, has_service ? service : nullptr, &hints, &res);
  if (err) {
    int eai = err == 11001 /* WSAHOST_NOT_FOUND */ || err == 11004 ? EAI_NODATA : err == 11002 ? EAI_AGAIN : EAI_FAIL;
    out.code(401);
    out.be32(eai);
    return;
  }
  out.code(222);
  for (W32_ADDRINFOW* ai = res; ai; ai = ai->next) {
    char addr[128];
    int alen = static_cast<int>(ai->addrlen) < static_cast<int>(sizeof addr) ? static_cast<int>(ai->addrlen) : static_cast<int>(sizeof addr);
    memcpy(addr, ai->addr, static_cast<size_t>(alen));
    uint16_t& fam = *reinterpret_cast<uint16_t*>(addr);
    if (fam == W_AF_INET6) fam = AF_INET6;
    out.be32(1);
    out.be32(ai->flags);
    out.be32(ai->family == W_AF_INET6 ? AF_INET6 : ai->family);
    out.be32(ai->socktype);
    out.be32(ai->protocol);
    out.be32(alen);
    out.bytes(addr, static_cast<size_t>(alen));
    char canon[512];
    if (ai->canonname && w32_utf16_to_utf8(ai->canonname, canon, sizeof canon) >= 0) {
      out.be32(static_cast<int32_t>(strlen(canon) + 1));
      out.bytes(canon, strlen(canon) + 1);
    } else {
      out.be32(0);
    }
  }
  out.be32(0);
  w32.FreeAddrInfoW(res);
}

void answer_hostent(const char* name, int af, Out& out) {
  WCHAR host[1024];
  W32_ADDRINFOW hints{};
  hints.family = af == AF_INET6 ? W_AF_INET6 : W_AF_INET;
  hints.flags = 2;  // AI_CANONNAME
  W32_ADDRINFOW* res = nullptr;
  if (!widen(name, host, 1024) || w32.GetAddrInfoW(host, nullptr, &hints, &res) || !res) {
    out.code(401);
    out.be32(HOST_NOT_FOUND);
    return;
  }
  out.code(222);
  char canon[512];
  if (!res->canonname || w32_utf16_to_utf8(res->canonname, canon, sizeof canon) < 0) snprintf(canon, sizeof canon, "%s", name);
  out.be32(static_cast<int32_t>(strlen(canon) + 1));
  out.bytes(canon, strlen(canon) + 1);
  out.be32(0);
  out.be32(af);
  int length = af == AF_INET6 ? 16 : 4;
  out.be32(length);
  for (W32_ADDRINFOW* ai = res; ai; ai = ai->next) {
    if (ai->socktype && ai->socktype != SOCK_STREAM) continue;
    const char* a = static_cast<const char*>(ai->addr);
    out.be32(length);
    out.bytes(af == AF_INET6 ? a + 8 : a + 4, static_cast<size_t>(length));
  }
  out.be32(0);
  w32.FreeAddrInfoW(res);
}

__attribute__((ms_abi)) DWORD dns_serve(void* param) {
  SOCKET s = reinterpret_cast<SOCKET>(param);
  char line[2048];
  size_t n = 0;
  while (n < sizeof line - 1) {
    int r = w32.recv(s, line + n, 1, 0);
    if (r <= 0) break;
    if (line[n] == 0) break;
    n++;
  }
  line[n] = 0;
  char* w[16];
  int count = words(line, w, 16);
  // VirtualAlloc: this thread isn't bionic's, so no malloc
  Out* out = static_cast<Out*>(w32.VirtualAlloc(nullptr, sizeof(Out), 0x3000, 0x04));
  if (out) {
    out->n = 0;
    out->ok = true;
    if (count >= 1 && strcmp(w[0], "getaddrinfo") == 0) {
      answer_getaddrinfo(w, count, *out);
    } else if (count >= 4 && strcmp(w[0], "gethostbyname") == 0) {
      answer_hostent(w[2], parse_int(w[3]), *out);
    } else {
      out->code(500);
      out->be32(0);
    }
    for (size_t sent = 0; out->ok && sent < out->n;) {
      int r = w32.send(s, out->data + sent, static_cast<int>(out->n - sent), 0);
      if (r <= 0) break;
      sent += static_cast<size_t>(r);
    }
    w32.VirtualFree(out, 0, 0x8000);
  }
  w32.shutdown(s, 1);
  w32.closesocket(s);
  return 0;
}

int dns_open_proxy() {
  SOCKET pair[2];
  if (loopback_pair(SOCK_STREAM, pair) < 0) return -1;
  HANDLE t = w32.CreateThread(nullptr, 256 * 1024, reinterpret_cast<DWORD(WINAPI*)(void*)>(dns_serve),
                              reinterpret_cast<void*>(pair[1]), 0x10000, nullptr);
  if (!t) {
    w32.closesocket(pair[0]);
    w32.closesocket(pair[1]);
    return -1;
  }
  w32.CloseHandle(t);
  long fd = install_socket(pair[0], AF_UNIX, SOCK_STREAM, 0, SOCK_CLOEXEC);
  if (fd < 0) return -1;
  return static_cast<int>(fd);
}

}  // namespace

void w32_init_net() {
  char data[512];
  if (w32.WSAStartup(0x0202, data)) w32_fatal("Winsock could not start");
  __netdClientDispatch.dnsOpenProxy = dns_open_proxy;
}
