#include "runtime/album_saver.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" void* __w32_pe_open(const char* utf8_path);
extern "C" void* __w32_pe_symbol(void* module, const char* name);
extern "C" int __w32_win32_path(const char* path, char* out, size_t capacity);

namespace blox7::runtime {
namespace {

#define BLOX7_MS __attribute__((ms_abi))

struct Guid {
  std::uint32_t a;
  std::uint16_t b, c;
  std::uint8_t d[8];
};

constexpr Guid kPicturesFolder = {
    0x33E28130, 0x4E1E, 0x4676, {0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB}};
constexpr Guid kVideosFolder = {
    0x18989B1D, 0x99B5, 0x455B, {0x84, 0x1C, 0xAB, 0x7C, 0x74, 0xE4, 0xDD, 0xFC}};
constexpr Guid kDownloadsFolder = {
    0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}};
constexpr std::uint32_t kKnownFolderCreate = 0x00008000;
constexpr std::uint32_t kUtf8 = 65001;
constexpr std::uint32_t kGenericWrite = 0x40000000;
constexpr std::uint32_t kCreateNew = 1;
constexpr std::uint32_t kCreateAlways = 2;
constexpr std::uint32_t kMoveReplace = 0x1;
constexpr std::uint32_t kMoveCopyAllowed = 0x2;
constexpr std::uint32_t kAttributeNormal = 0x80;
constexpr std::uint32_t kErrorFileExists = 80;
constexpr std::uint32_t kErrorAlreadyExists = 183;
constexpr std::uint32_t kGenericRead = 0x80000000;
constexpr std::uint32_t kOpenExisting = 3;
constexpr std::uint32_t kAttributeDirectory = 0x10;
constexpr std::uint32_t kErrorSharingViolation = 32;
constexpr std::uint32_t kNotifyFileName = 0x1;
constexpr std::uint32_t kInfinite = 0xFFFFFFFF;
constexpr std::uint32_t kWaitSignaled = 0;
constexpr std::uint32_t kWaitFailed = 0xFFFFFFFF;
constexpr char kFailure[] = "saveImageToAlbum: Failed to save image with exception: \n";

struct Win32 {
  std::int32_t(BLOX7_MS* get_known_folder)(const Guid*, std::uint32_t, void*,
                                           char16_t**) = nullptr;
  void(BLOX7_MS* free_task_memory)(void*) = nullptr;
  std::int32_t(BLOX7_MS* to_wide)(std::uint32_t, std::uint32_t, const char*,
                                  std::int32_t, char16_t*, std::int32_t) =
      nullptr;
  std::int32_t(BLOX7_MS* to_utf8)(std::uint32_t, std::uint32_t,
                                  const char16_t*, std::int32_t, char*,
                                  std::int32_t, const char*, std::int32_t*) =
      nullptr;
  std::int32_t(BLOX7_MS* create_directories)(void*, const char16_t*, void*) = nullptr;
  void*(BLOX7_MS* create_file)(const char16_t*, std::uint32_t, std::uint32_t,
                               void*, std::uint32_t, std::uint32_t, void*) =
      nullptr;
  std::int32_t(BLOX7_MS* write_file)(void*, const void*, std::uint32_t,
                                     std::uint32_t*, void*) = nullptr;
  std::int32_t(BLOX7_MS* close_handle)(void*) = nullptr;
  std::int32_t(BLOX7_MS* delete_file)(const char16_t*) = nullptr;
  std::int32_t(BLOX7_MS* move_file)(const char16_t*, const char16_t*,
                                    std::uint32_t) = nullptr;
  std::uint32_t(BLOX7_MS* last_error)() = nullptr;
  void*(BLOX7_MS* find_first_change)(const char16_t*, std::int32_t, std::uint32_t) = nullptr;
  std::int32_t(BLOX7_MS* find_next_change)(void*) = nullptr;
  std::int32_t(BLOX7_MS* find_close_change)(void*) = nullptr;
  void(BLOX7_MS* sleep)(std::uint32_t) = nullptr;
  std::uint32_t(BLOX7_MS* wait)(void*, std::uint32_t) = nullptr;
  void*(BLOX7_MS* find_first_file)(const char16_t*, void*) = nullptr;
  std::int32_t(BLOX7_MS* find_next_file)(void*, void*) = nullptr;
  std::int32_t(BLOX7_MS* find_close)(void*) = nullptr;
  void(BLOX7_MS* local_time)(void*) = nullptr;
  void(BLOX7_MS* system_time)(void*) = nullptr;
};

struct SystemTime {
  std::uint16_t year, month, day_of_week, day, hour, minute, second, milliseconds;
};

struct FindData {
  std::uint32_t attributes;
  std::uint32_t times[6];
  std::uint32_t size_high, size_low;
  std::uint32_t reserved[2];
  char16_t name[260];
  char16_t short_name[14];
};

template <typename Fn>
bool Bind(void* module, const char* name, Fn* slot) {
  *slot = module != nullptr ? reinterpret_cast<Fn>(__w32_pe_symbol(module, name))
                            : nullptr;
  return *slot != nullptr;
}

const Win32* Api() {
  static const Win32* const api = [] {
    auto loaded = std::make_unique<Win32>();
    void* shell32 = __w32_pe_open("shell32.dll");
    void* ole32 = __w32_pe_open("ole32.dll");
    void* kernel32 = __w32_pe_open("kernel32.dll");
    const bool ok =
        Bind(shell32, "SHGetKnownFolderPath", &loaded->get_known_folder) &&
        Bind(ole32, "CoTaskMemFree", &loaded->free_task_memory) &&
        Bind(kernel32, "MultiByteToWideChar", &loaded->to_wide) &&
        Bind(kernel32, "WideCharToMultiByte", &loaded->to_utf8) &&
        Bind(shell32, "SHCreateDirectoryExW", &loaded->create_directories) &&
        Bind(kernel32, "CreateFileW", &loaded->create_file) &&
        Bind(kernel32, "WriteFile", &loaded->write_file) &&
        Bind(kernel32, "CloseHandle", &loaded->close_handle) &&
        Bind(kernel32, "DeleteFileW", &loaded->delete_file) &&
        Bind(kernel32, "MoveFileExW", &loaded->move_file) &&
        Bind(kernel32, "GetLastError", &loaded->last_error) &&
        Bind(kernel32, "FindFirstChangeNotificationW", &loaded->find_first_change) &&
        Bind(kernel32, "FindNextChangeNotification", &loaded->find_next_change) &&
        Bind(kernel32, "FindCloseChangeNotification", &loaded->find_close_change) &&
        Bind(kernel32, "Sleep", &loaded->sleep) &&
        Bind(kernel32, "WaitForSingleObject", &loaded->wait) &&
        Bind(kernel32, "FindFirstFileW", &loaded->find_first_file) &&
        Bind(kernel32, "FindNextFileW", &loaded->find_next_file) &&
        Bind(kernel32, "FindClose", &loaded->find_close) &&
        Bind(kernel32, "GetLocalTime", &loaded->local_time) &&
        Bind(kernel32, "GetSystemTime", &loaded->system_time);
    return ok ? loaded.release() : nullptr;
  }();
  return api;
}

std::u16string Wide(const Win32& w, const std::string& text) {
  const std::int32_t size = w.to_wide(kUtf8, 0, text.data(),
                                      static_cast<std::int32_t>(text.size()),
                                      nullptr, 0);
  std::u16string wide(static_cast<std::size_t>(std::max(size, 0)), u'\0');
  if (size > 0) {
    w.to_wide(kUtf8, 0, text.data(), static_cast<std::int32_t>(text.size()),
              wide.data(), size);
  }
  return wide;
}

std::string Utf8(const Win32& w, const std::u16string& wide) {
  const std::int32_t size =
      w.to_utf8(kUtf8, 0, wide.data(), static_cast<std::int32_t>(wide.size()),
                nullptr, 0, nullptr, nullptr);
  std::string text(static_cast<std::size_t>(std::max(size, 0)), '\0');
  if (size > 0) {
    w.to_utf8(kUtf8, 0, wide.data(), static_cast<std::int32_t>(wide.size()),
              text.data(), size, nullptr, nullptr);
  }
  return text;
}

std::string MimeType(const std::string& name) {
  const std::size_t dot = name.find_last_of('.');
  std::string extension = dot == std::string::npos ? "" : name.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static constexpr struct {
    const char* extension;
    const char* mime;
  } kTypes[] = {{"png", "image/png"},   {"jpg", "image/jpeg"},
                {"jpeg", "image/jpeg"}, {"gif", "image/gif"},
                {"webp", "image/webp"}, {"bmp", "image/bmp"},
                {"mp4", "video/mp4"},   {"3gp", "video/3gpp"},
                {"webm", "video/webm"}, {"mkv", "video/x-matroska"},
                {"mov", "video/quicktime"}};
  for (const auto& type : kTypes) {
    if (extension == type.extension) {
      return type.mime;
    }
  }
  return "application/octet-stream";
}

}  // namespace

bool SaveToWindowsMediaFolder(const std::string& path, const std::string& name, std::string* message) {
  const Win32* w = Api();
  const std::size_t dot = name.find_last_of('.');
  const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string mime = MimeType(name);
  const bool image = mime.rfind("image/", 0) == 0;
  const bool video = mime.rfind("video/", 0) == 0;
  if (w == nullptr || name.empty()) {
    *message = std::string(kFailure) + "no Windows shell functions";
    return false;
  }

  const char* chosen = image ? std::getenv("BLOX7_SCREENSHOTS_DIR")
                       : video ? std::getenv("BLOX7_VIDEOS_DIR")
                               : nullptr;
  std::u16string directory;
  if (chosen != nullptr && *chosen != '\0') {
    directory = Wide(*w, chosen);
    while (directory.size() > 3 && (directory.back() == u'\\' || directory.back() == u'/')) {
      directory.pop_back();
    }
  } else {
    char16_t* known = nullptr;
    const Guid* folder = image ? &kPicturesFolder : video ? &kVideosFolder : &kDownloadsFolder;
    if (w->get_known_folder(folder, kKnownFolderCreate, nullptr, &known) < 0 || known == nullptr) {
      if (known != nullptr) {
        w->free_task_memory(known);
      }
      *message = std::string(kFailure) + "the media folder is unavailable";
      return false;
    }
    directory = known;
    w->free_task_memory(known);
    if (image || video) {
      directory += u"\\Roblox";
    }
  }
  if (image || video) {
    const std::int32_t made = w->create_directories(nullptr, directory.c_str(), nullptr);
    if (made != 0 && made != static_cast<std::int32_t>(kErrorAlreadyExists) &&
        made != static_cast<std::int32_t>(kErrorFileExists)) {
      *message = std::string(kFailure) + "cannot create " + Utf8(*w, directory);
      return false;
    }
  }

  std::u16string target;
  void* out = nullptr;
  for (int copy = 0; copy < 1000; ++copy) {
    const std::string file = copy == 0 ? name
                                       : stem + " (" + std::to_string(copy) + ")" +
                                             (dot == std::string::npos ? "" : name.substr(dot));
    target = directory + u"\\" + Wide(*w, file);
    out = w->create_file(target.c_str(), kGenericWrite, 0, nullptr, kCreateNew,
                         kAttributeNormal, nullptr);
    if (out != reinterpret_cast<void*>(-1) || w->last_error() != kErrorFileExists) {
      break;
    }
  }
  const auto invalid = [](void* handle) { return handle == reinterpret_cast<void*>(-1) || handle == nullptr; };
  if (invalid(out)) {
    *message = std::string(kFailure) + "cannot create " + Utf8(*w, target);
    return false;
  }

  const auto saved = [&] {
    std::fprintf(stderr, "  [captures] saved %s to %s\n", name.c_str(), Utf8(*w, target).c_str());
    message->clear();
    return true;
  };
  const bool moves = mime == "video/mp4";
  if (moves) {
    w->close_handle(out);
    std::vector<char> native(32768);
    if (__w32_win32_path(path.c_str(), native.data(), native.size()) == 0 &&
        w->move_file(Wide(*w, native.data()).c_str(), target.c_str(), kMoveReplace | kMoveCopyAllowed) != 0) {
      return saved();
    }
    out = w->create_file(target.c_str(), kGenericWrite, 0, nullptr, kCreateAlways, kAttributeNormal, nullptr);
    if (invalid(out)) {
      w->delete_file(target.c_str());
      *message = std::string(kFailure) + "cannot write " + Utf8(*w, target);
      return false;
    }
  }

  std::unique_ptr<FILE, int (*)(FILE*)> in(std::fopen(path.c_str(), "rb"), &std::fclose);
  std::vector<char> buffer(1 << 20);
  bool copied = in != nullptr;
  for (std::size_t got; copied && (got = std::fread(buffer.data(), 1, buffer.size(), in.get())) > 0;) {
    std::uint32_t written = 0;
    if (w->write_file(out, buffer.data(), static_cast<std::uint32_t>(got), &written, nullptr) == 0 ||
        written != got) {
      copied = false;
    }
  }
  copied = copied && std::ferror(in.get()) == 0;
  w->close_handle(out);
  in.reset();
  if (!copied) {
    w->delete_file(target.c_str());
    *message = std::string(kFailure) + path + ": copy to " + Utf8(*w, target) + " failed";
    return false;
  }
  if (moves) {
    std::remove(path.c_str());
  }
  return saved();
}

bool SaveCaptureToWindowsMediaFolder(const std::string& path, std::string* message) {
  const std::size_t slash = path.find_last_of('/');
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::size_t dot = name.find_last_of('.');
  const std::string mime = MimeType(name);
  const bool image = mime.rfind("image/", 0) == 0;
  const Win32* w = Api();
  if (w == nullptr || dot == std::string::npos || (!image && mime.rfind("video/", 0) != 0)) {
    return SaveToWindowsMediaFolder(path, name, message);
  }
  SystemTime t{};
  char renamed[64];
  if (image) {
    w->local_time(&t);
    std::snprintf(renamed, sizeof renamed, "RobloxScreenShot%04d%02d%02d_%02d%02d%02d%03d", t.year, t.month, t.day,
                  t.hour, t.minute, t.second, t.milliseconds);
  } else {
    w->system_time(&t);
    std::snprintf(renamed, sizeof renamed, "Roblox-%04d-%02d-%02dT%02d_%02d_%02d.%03dZ", t.year, t.month, t.day,
                  t.hour, t.minute, t.second, t.milliseconds);
  }
  return SaveToWindowsMediaFolder(path, renamed + name.substr(dot), message);
}

namespace {

bool MoveClosedVideos(const Win32& w, const std::string& directory, const std::u16string& folder) {
  FindData found{};
  void* search = w.find_first_file((folder + u"\\*.mp4").c_str(), &found);
  if (search == reinterpret_cast<void*>(-1)) {
    return false;
  }
  bool writing = false;
  do {
    if ((found.attributes & kAttributeDirectory) != 0) {
      continue;
    }
    const std::u16string name = found.name;
    void* probe = w.create_file((folder + u"\\" + name).c_str(), kGenericRead, 0, nullptr, kOpenExisting,
                                kAttributeNormal, nullptr);
    if (probe == reinterpret_cast<void*>(-1)) {
      writing = writing || w.last_error() == kErrorSharingViolation;
      continue;
    }
    w.close_handle(probe);
    const std::string file = Utf8(w, name);
    std::string message;
    if (!SaveToWindowsMediaFolder(directory + "/" + file, file, &message)) {
      std::fprintf(stderr, "  [captures] could not save %s: %s\n", file.c_str(),
                   message.c_str() + sizeof(kFailure) - 1);
    }
  } while (w.find_next_file(search, &found) != 0);
  w.find_close(search);
  return writing;
}

void WatchVideos(const std::string& directory) {
  const Win32* w = Api();
  std::vector<char> native(32768);
  if (w == nullptr || __w32_win32_path(directory.c_str(), native.data(), native.size()) != 0) {
    return;
  }
  const std::u16string folder = Wide(*w, native.data());
  for (;;) {
    w->create_directories(nullptr, folder.c_str(), nullptr);
    void* change = w->find_first_change(folder.c_str(), 0, kNotifyFileName);
    while (change != reinterpret_cast<void*>(-1)) {
      const bool writing = MoveClosedVideos(*w, directory, folder);
      const std::uint32_t waited = w->wait(change, writing ? 1000 : kInfinite);
      if (waited == kWaitFailed || (waited == kWaitSignaled && w->find_next_change(change) == 0)) {
        w->find_close_change(change);
        break;
      }
    }
    w->sleep(1000);
  }
}

}  // namespace

void MoveFinishedVideos(const std::string& directory) {
  static std::once_flag started;
  std::call_once(started, [&directory] { std::thread(WatchVideos, directory).detach(); });
}

}  // namespace blox7::runtime
