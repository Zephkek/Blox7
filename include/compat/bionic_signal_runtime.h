#ifndef BLOX7_COMPAT_BIONIC_SIGNAL_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_SIGNAL_RUNTIME_H_

#include <signal.h>

#include <cstdint>

namespace blox7::compat {

struct BionicSigaction {
  int flags = 0;
  int padding = 0;
  union {
    void (*handler)(int);
    void (*action)(int, siginfo_t*, void*);
  } callback{};
  std::uint64_t mask = 0;
  void (*restorer)() = nullptr;
};

static_assert(sizeof(BionicSigaction) == 32,
              "Android x86-64 sigaction ABI changed");

int BionicSigactionCall(int signal_number, const BionicSigaction* action,
                        BionicSigaction* old_action) noexcept;

}  // namespace blox7::compat

extern "C" int blox7_bionic_sigaction(
    int signal_number, const blox7::compat::BionicSigaction* action,
    blox7::compat::BionicSigaction* old_action);

#endif  // BLOX7_COMPAT_BIONIC_SIGNAL_RUNTIME_H_
