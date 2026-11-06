#ifndef BLOX7_COMPAT_BIONIC_SYSCONF_H_
#define BLOX7_COMPAT_BIONIC_SYSCONF_H_

namespace blox7::compat {

enum class BionicSysconfName : int {
  kArgMax = 0x0000,
  kChildMax = 0x0005,
  kClockTicks = 0x0006,
  kOpenMax = 0x000b,
  kIovMax = 0x0026,
  kPageSize = 0x0027,
  kPageSizeAlias = 0x0028,
  kProcessorsConfigured = 0x0060,
  kProcessorsOnline = 0x0061,
  kPhysicalPages = 0x0062,
  kAvailablePhysicalPages = 0x0063,
};

long BionicSysconf(int name) noexcept;

}  // namespace blox7::compat

extern "C" long blox7_bionic_sysconf(int name);

#endif  // BLOX7_COMPAT_BIONIC_SYSCONF_H_
