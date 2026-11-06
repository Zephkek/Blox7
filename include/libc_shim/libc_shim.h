#ifndef BLOX7_LIBC_SHIM_LIBC_SHIM_H_
#define BLOX7_LIBC_SHIM_LIBC_SHIM_H_

#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>

namespace libc_shim {

using GuestAllocator = void* (*)(size_t);

enum class HostCaBundleStatus {
  kReady,
  kInvalidOverride,
  kUnavailable,
};

struct HostCaBundleResolution {
  HostCaBundleStatus status = HostCaBundleStatus::kUnavailable;
  std::string host_path;
  bool from_override = false;

  bool ok() const { return status == HostCaBundleStatus::kReady; }
};

void ConfigureGuestAllocator(GuestAllocator allocator) noexcept;

void Install();

HostCaBundleResolution ResolveHostCaBundle();

HostCaBundleResolution ConfigureHostCaBundlePathMappings();

const char* HostCaBundleStatusName(HostCaBundleStatus status);

std::string TranslatePath(const std::string& android_path);

void RegisterPathMapping(const std::string& android_prefix,
                         const std::string& host_prefix);

void ClearPathMappings();

}  // namespace libc_shim

extern "C" {
int blox7_open(const char* path, int flags, ...);
int blox7___open_2(const char* path, int flags);
FILE* blox7_fopen(const char* path, const char* mode);
int blox7_access(const char* path, int mode);
int blox7_stat(const char* path, struct stat* statbuf);
int blox7_lstat(const char* path, struct stat* statbuf);
int blox7_statvfs(const char* path, struct statvfs* statbuf);
int blox7_statfs(const char* path, struct statfs* statbuf);
int blox7_mkdir(const char* path, mode_t mode);
DIR* blox7_opendir(const char* path);
int blox7_rename(const char* old_path, const char* new_path);
int blox7_unlink(const char* path);
int blox7_rmdir(const char* path);
char* blox7_realpath(const char* path, char* resolved_path);
ssize_t blox7_readlink(const char* path, char* buf, size_t bufsiz);
ssize_t blox7___readlink_chk(const char* path, char* buf, size_t len,
                                size_t buf_len);
}

#endif  // BLOX7_LIBC_SHIM_LIBC_SHIM_H_
