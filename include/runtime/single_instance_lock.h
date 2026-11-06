#ifndef BLOX7_RUNTIME_SINGLE_INSTANCE_LOCK_H_
#define BLOX7_RUNTIME_SINGLE_INSTANCE_LOCK_H_

#include <filesystem>
#include <string>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace blox7 {
namespace runtime {

std::string CurrentExecutableInstanceKey();

class SingleInstanceLock final {
 public:
  enum class Status { kAcquired, kAlreadyRunning, kError };

  ~SingleInstanceLock();

  SingleInstanceLock(const SingleInstanceLock&) = delete;
  SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;
  SingleInstanceLock(SingleInstanceLock&& other) noexcept;
  SingleInstanceLock& operator=(SingleInstanceLock&& other) noexcept;

  static SingleInstanceLock AcquireForCurrentUser();
  static SingleInstanceLock AcquireForLaunch(const Environment& environment,
                                             const RuntimePaths& paths);
  static SingleInstanceLock Acquire(const std::filesystem::path& lock_file);

  Status status() const { return status_; }
  bool acquired() const { return status_ == Status::kAcquired; }
  bool already_running() const { return status_ == Status::kAlreadyRunning; }
  const std::filesystem::path& path() const { return path_; }
  const std::string& error() const { return error_; }

 private:
  SingleInstanceLock(int descriptor, Status status, std::filesystem::path path,
                     std::string error);

  int descriptor_ = -1;
  Status status_ = Status::kError;
  std::filesystem::path path_;
  std::string error_;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_SINGLE_INSTANCE_LOCK_H_
