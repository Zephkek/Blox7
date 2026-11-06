#ifndef BLOX7_COMPAT_BIONIC_DNS_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_DNS_RUNTIME_H_

#include <netdb.h>
#include <sys/socket.h>

#include <cstdint>
#include <string_view>

namespace blox7 {
namespace compat {

struct BionicAddressInfo {
  int ai_flags = 0;
  int ai_family = 0;
  int ai_socktype = 0;
  int ai_protocol = 0;
  socklen_t ai_addrlen = 0;
  char* ai_canonname = nullptr;
  sockaddr* ai_addr = nullptr;
  BionicAddressInfo* ai_next = nullptr;
};

constexpr int kBionicAddressInfoNameNotFound = 8;

bool IsBlockedCrashReportUploadHost(std::string_view host) noexcept;

int BionicGetAddressInfo(const char* node, const char* service,
                         const BionicAddressInfo* hints,
                         BionicAddressInfo** result) noexcept;
void BionicFreeAddressInfo(BionicAddressInfo* info) noexcept;
hostent* BionicGetHostByName(const char* name) noexcept;

}  // namespace compat
}  // namespace blox7

extern "C" {

int blox7_bionic_getaddrinfo(
    const char* node, const char* service,
    const blox7::compat::BionicAddressInfo* hints,
    blox7::compat::BionicAddressInfo** result);
void blox7_bionic_freeaddrinfo(blox7::compat::BionicAddressInfo* info);
hostent* blox7_bionic_gethostbyname(const char* name);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_DNS_RUNTIME_H_
