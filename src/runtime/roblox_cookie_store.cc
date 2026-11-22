#include "runtime/roblox_cookie_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "runtime/environment.h"

extern "C" int __w32_protect(const void* in, size_t size, void** out, size_t* out_size);
extern "C" int __w32_unprotect(const void* in, size_t size, void** out, size_t* out_size);

namespace blox7::runtime {
namespace {

constexpr size_t kMaximumCookieBytes = 1024 * 1024;

void ClearString(std::string* value) {
  volatile char* bytes = value->data();
  for (size_t i = 0; i < value->size(); ++i) bytes[i] = '\0';
  value->clear();
}

// no keyring on windows, dpapi ties the cookie to the windows account instead
class DpapiSecretStore final : public RobloxSecretStore {
 public:
  StoredCookie Load(const std::filesystem::path& profile) override {
    StoredCookie result;
    result.from_secret_store = true;
    std::string sealed;
    const int error = ReadSealed(Sealed(profile), &sealed);
    if (error == ENOENT) return result;
    void* plain = nullptr;
    size_t size = 0;
    if (error != 0 || __w32_unprotect(sealed.data(), sealed.size(), &plain, &size) != 0) {
      result.status = StoredCookieStatus::kUnavailable;
      result.error = "the saved Roblox session belongs to another Windows account";
      return result;
    }
    result.status = StoredCookieStatus::kFound;
    result.value.assign(static_cast<const char*>(plain), size);
    volatile char* wipe = static_cast<char*>(plain);
    for (size_t i = 0; i < size; ++i) wipe[i] = '\0';
    free(plain);
    return result;
  }

  bool Store(const std::filesystem::path& profile, const std::string& cookie) override {
    void* sealed = nullptr;
    size_t size = 0;
    if (__w32_protect(cookie.data(), cookie.size(), &sealed, &size) != 0) return false;
    const bool written = WriteSealed(Sealed(profile), sealed, size);
    free(sealed);
    return written;
  }

  bool Clear(const std::filesystem::path& profile) override {
    return unlink(Sealed(profile).c_str()) == 0 || errno == ENOENT;
  }

 private:
  static std::filesystem::path Sealed(const std::filesystem::path& profile) { return profile.string() + ".dpapi"; }

  static int ReadSealed(const std::filesystem::path& path, std::string* out) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return errno;
    struct stat metadata{};
    int error = 0;
    if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
        metadata.st_size < 0 || static_cast<uintmax_t>(metadata.st_size) > kMaximumCookieBytes * 2) {
      error = EPERM;
    } else {
      out->resize(static_cast<size_t>(metadata.st_size));
      size_t done = 0;
      while (done < out->size()) {
        const ssize_t count = read(descriptor, out->data() + done, out->size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
          error = EIO;
          break;
        }
        done += static_cast<size_t>(count);
      }
    }
    close(descriptor);
    return error;
  }

  static bool WriteSealed(const std::filesystem::path& path, const void* data, size_t size) {
    const std::filesystem::path temporary = path.string() + ".tmp-" + std::to_string(getpid());
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) return false;
    size_t done = 0;
    while (done < size) {
      const ssize_t count = write(descriptor, static_cast<const char*>(data) + done, size - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      done += static_cast<size_t>(count);
    }
    const bool written = done == size && fsync(descriptor) == 0;
    close(descriptor);
    if (written && rename(temporary.c_str(), path.c_str()) == 0) return true;
    unlink(temporary.c_str());
    return false;
  }
};

}  // namespace

StoredCookie RobloxSecretStore::Load(const std::filesystem::path&) {
  return {StoredCookieStatus::kUnavailable,
          {},
          "secure Roblox cookie storage is unavailable",
          true};
}

bool RobloxSecretStore::Store(const std::filesystem::path&,
                              const std::string&) {
  return false;
}
bool RobloxSecretStore::Clear(const std::filesystem::path&) { return false; }

void RobloxSecretStore::Warn(std::string_view message) {
  std::fprintf(stderr, "  [auth] %.*s\n", static_cast<int>(message.size()),
               message.data());
}

std::shared_ptr<RobloxSecretStore> OpenRobloxSecretStore() {
  return OpenRobloxSecretStore(ProcessEnvironment());
}

std::shared_ptr<RobloxSecretStore> OpenRobloxSecretStore(
    const Environment& environment) {
  (void)environment;
  return std::make_shared<DpapiSecretStore>();
}

StoredCookie ReadRobloxCookieFile(const std::filesystem::path& path,
                                  bool required) {
  const int descriptor =
      open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT && !required) return {};
    return {StoredCookieStatus::kUnavailable,
            {},
            "configured Roblox cookie file is unavailable"};
  }
  struct CloseFile {
    int descriptor;
    ~CloseFile() { close(descriptor); }
  } file{descriptor};
  struct stat metadata{};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != geteuid())
    return {StoredCookieStatus::kUnavailable,
            {},
            "Roblox cookie source is not a regular file"};
  if ((metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0)
    return {StoredCookieStatus::kUnavailable,
            {},
            "Roblox cookie file permissions are not private"};
  if (metadata.st_size < 0 ||
      static_cast<uintmax_t>(metadata.st_size) > kMaximumCookieBytes)
    return {StoredCookieStatus::kUnavailable,
            {},
            "Roblox cookie file exceeds the safe read limit"};

  StoredCookie result{StoredCookieStatus::kFound, {}, {}};
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = read(descriptor, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || result.value.size() + static_cast<size_t>(count) >
                         kMaximumCookieBytes) {
      result.error = "Roblox cookie file could not be read safely";
      break;
    }
    result.value.append(buffer.data(), static_cast<size_t>(count));
  }
  volatile char* bytes = buffer.data();
  for (size_t i = 0; i < buffer.size(); ++i) bytes[i] = '\0';
  struct stat after{};
  if (fstat(descriptor, &after) != 0 || metadata.st_size != after.st_size ||
      metadata.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      metadata.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      metadata.st_ctim.tv_sec != after.st_ctim.tv_sec ||
      metadata.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
      static_cast<uintmax_t>(after.st_size) != result.value.size())
    result.error = "Roblox cookie file changed while it was read";
  if (required && result.value.empty())
    result.error = "configured Roblox cookie file is empty";
  if (!result.error.empty()) {
    ClearString(&result.value);
    result.status = StoredCookieStatus::kUnavailable;
  }
  return result;
}

StoredCookie LoadManagedRobloxCookie(const std::filesystem::path& path,
                                     RobloxSecretStore& secrets) {
  StoredCookie file = ReadRobloxCookieFile(path);
  return file.status == StoredCookieStatus::kMissing ? secrets.Load(path)
                                                     : std::move(file);
}

bool RemoveLegacyWebViewCookies(const std::filesystem::path& directory) {
  if (directory.empty()) return true;
  const int fd =
      open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT;
  struct stat metadata{};
  bool removed = fstat(fd, &metadata) == 0 && metadata.st_uid == geteuid();
  for (const char* name : {"cookies.sqlite", "cookies.sqlite-wal",
                           "cookies.sqlite-shm", "cookies.sqlite-journal"}) {
    if (!removed) break;
    if (fstatat(fd, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
      removed = errno == ENOENT;
      continue;
    }
    removed = S_ISREG(metadata.st_mode) && metadata.st_uid == geteuid() &&
              metadata.st_nlink == 1 && unlinkat(fd, name, 0) == 0;
  }
  if (removed) removed = fsync(fd) == 0;
  close(fd);
  return removed;
}

}  // namespace blox7::runtime
