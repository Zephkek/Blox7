#include <winsock2.h>
#include <ws2tcpip.h>

#include "wv2setup.h"

#include <bearssl_hash.h>
#include <wininet.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "../studio7/studio7.h"
#include "wv2unpack.h"

namespace {

constexpr int kAllFiles = 0;
constexpr ULONGLONG kRoomToFetch = 700000000;
constexpr ULONGLONG kRoomToUnpack = 550000000;
constexpr DWORD kBlock = 256 << 10;
constexpr DWORD kLogLimit = 256 << 10;
constexpr char kPath[] =
    "/c/msdownload/update/software/updt/2023/09/"
    "microsoftedgestandaloneinstallerx64_1c890b4b8dd6b7c93da98ebdc08ecdc5e30e50cb.exe";

struct Source {
  bool https;
  bool own_tls;
  const char* host;
};

constexpr Source kSources[] = {{false, false, "catalog.s.download.windowsupdate.com"},
                               {false, false, "download.windowsupdate.com"},
                               {true, false, "catalog.s.download.windowsupdate.com"},
                               {true, true, "catalog.s.download.windowsupdate.com"}};
constexpr int kSourceCount = sizeof kSources / sizeof kSources[0];

enum Outcome { kNone, kGot, kNetwork, kBlocked, kProxy, kGone, kBad, kDisk };
enum Lookup { kNoAnswer, kNowhere, kAnswered };

struct Job {
  std::wstring home;
  HANDLE log = INVALID_HANDLE_VALUE;
  HANDLE hurry = nullptr;
  bool background = false;
  volatile wv2_status* status = nullptr;
  DWORD win32 = 0;
};

struct Net {
  HMODULE dll = nullptr;
  decltype(&InternetOpenW) open = nullptr;
  decltype(&InternetOpenUrlW) open_url = nullptr;
  decltype(&HttpQueryInfoW) query = nullptr;
  decltype(&InternetReadFile) read = nullptr;
  decltype(&InternetSetOptionW) set_option = nullptr;
  decltype(&InternetCloseHandle) close = nullptr;
};

struct Download {
  HANDLE file = INVALID_HANDLE_VALUE;
  ULONGLONG have = 0;
  ULONGLONG received = 0;
  br_sha256_context hash;
  std::vector<unsigned char> block = std::vector<unsigned char>(kBlock);
};

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::string narrow(const wchar_t* text) {
  std::string out;
  for (; *text; ++text) out += *text < 128 ? static_cast<char>(*text) : '?';
  return out;
}

double seconds(ULONGLONG since) { return static_cast<double>(GetTickCount64() - since) / 1000.0; }

void make_dirs(const std::wstring& path) {
  for (size_t i = 3; i < path.size(); ++i)
    if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
  CreateDirectoryW(path.c_str(), nullptr);
}

void remove_tree(const std::wstring& dir) {
  const DWORD attributes = GetFileAttributesW(dir.c_str());
  std::vector<std::wstring> files, dirs;
  WIN32_FIND_DATAW found;
  HANDLE search = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                      ? FindFirstFileW((dir + L"\\*").c_str(), &found)
                      : INVALID_HANDLE_VALUE;
  if (search != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      if (name == L"." || name == L"..") continue;
      (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? dirs : files).push_back(dir + L"\\" + name);
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  for (const std::wstring& path : files) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path.c_str());
  }
  for (const std::wstring& path : dirs) remove_tree(path);
  RemoveDirectoryW(dir.c_str());
}

void remove_partials(const std::wstring& home) {
  std::vector<std::wstring> stale;
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((home + L"\\.partial-*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) stale.push_back(home + L"\\" + found.cFileName);
  } while (FindNextFileW(search, &found));
  FindClose(search);
  for (const std::wstring& dir : stale) remove_tree(dir);
}

void note(Job& job, const char* format, ...) {
  if (job.log == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME now;
  GetLocalTime(&now);
  char line[512];
  int used = std::snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay,
                           now.wHour, now.wMinute, now.wSecond);
  va_list args;
  va_start(args, format);
  const int more = std::vsnprintf(line + used, sizeof line - used - 2, format, args);
  va_end(args);
  if (more > 0) used += std::min(more, static_cast<int>(sizeof line) - used - 3);
  line[used++] = '\r';
  line[used++] = '\n';
  DWORD done = 0;
  WriteFile(job.log, line, static_cast<DWORD>(used), &done, nullptr);
}

void open_log(Job& job) {
  const std::wstring path = job.home + L"\\setup.log";
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
      (data.nFileSizeHigh || data.nFileSizeLow > kLogLimit))
    DeleteFileW(path.c_str());
  job.log = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void show(Job& job, LONG state, ULONGLONG done, ULONGLONG total) {
  if (job.background && job.hurry != nullptr && WaitForSingleObject(job.hurry, 0) == WAIT_OBJECT_0) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    job.background = false;
    note(job, "a page is waiting, no longer in the background");
  }
  if (job.status == nullptr) return;
  InterlockedExchange64(&job.status->total, static_cast<LONGLONG>(total));
  InterlockedExchange64(&job.status->done, static_cast<LONGLONG>(done));
  InterlockedExchange(&job.status->state, state);
}

void unpacked(void* context, ULONGLONG done, ULONGLONG total) {
  show(*static_cast<Job*>(context), WV2_STATE_UNPACKING, done, total);
}

std::string windows_version() {
  using GetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  const auto get = reinterpret_cast<GetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  OSVERSIONINFOEXW info{};
  info.dwOSVersionInfoSize = sizeof info;
  if (get == nullptr || get(&info) != 0) return "unknown";
  char text[64];
  std::snprintf(text, sizeof text, "%lu.%lu.%lu sp%u", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber,
                info.wServicePackMajor);
  return text;
}

int write_failure(DWORD error) {
  return error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL ? WV2_ERROR_DISK_SPACE : WV2_ERROR_BLOCKED_FILE;
}

bool on_local_drive(const std::wstring& path) {
  wchar_t root[MAX_PATH + 1];
  return GetVolumePathNameW(path.c_str(), root, MAX_PATH + 1) && GetDriveTypeW(root) != DRIVE_REMOTE;
}

ULONGLONG free_bytes(const std::wstring& path) {
  ULARGE_INTEGER available{};
  return GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr) ? available.QuadPart : 0;
}

bool matches(const br_sha256_context& hash) {
  unsigned char digest[br_sha256_SIZE];
  br_sha256_out(&hash, digest);
  char text[2 * br_sha256_SIZE + 1];
  for (int i = 0; i < br_sha256_SIZE; ++i) std::snprintf(text + 2 * i, 3, "%02x", digest[i]);
  return std::strcmp(text, wv2_package_sha256()) == 0;
}

bool package_ok(Job& job, const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const ULONGLONG total = wv2_package_bytes();
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && static_cast<ULONGLONG>(size.QuadPart) == total;
  br_sha256_context hash;
  br_sha256_init(&hash);
  std::vector<unsigned char> block(kBlock);
  ULONGLONG done = 0;
  while (ok && done < total) {
    DWORD got = 0;
    ok = ReadFile(file, block.data(), kBlock, &got, nullptr) && got > 0;
    br_sha256_update(&hash, block.data(), got);
    done += got;
    show(job, WV2_STATE_DOWNLOADING, done, total);
  }
  CloseHandle(file);
  ok = ok && done == total && matches(hash);
  note(job, ok ? "package already downloaded" : "package on disk is damaged");
  return ok;
}

bool load(Net& net) {
  wchar_t system[MAX_PATH];
  const UINT length = GetSystemDirectoryW(system, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return false;
  net.dll = LoadLibraryW((std::wstring(system, length) + L"\\wininet.dll").c_str());
  if (net.dll == nullptr) return false;
  net.open = reinterpret_cast<decltype(net.open)>(GetProcAddress(net.dll, "InternetOpenW"));
  net.open_url = reinterpret_cast<decltype(net.open_url)>(GetProcAddress(net.dll, "InternetOpenUrlW"));
  net.query = reinterpret_cast<decltype(net.query)>(GetProcAddress(net.dll, "HttpQueryInfoW"));
  net.read = reinterpret_cast<decltype(net.read)>(GetProcAddress(net.dll, "InternetReadFile"));
  net.set_option = reinterpret_cast<decltype(net.set_option)>(GetProcAddress(net.dll, "InternetSetOptionW"));
  net.close = reinterpret_cast<decltype(net.close)>(GetProcAddress(net.dll, "InternetCloseHandle"));
  return net.open && net.open_url && net.query && net.read && net.set_option && net.close;
}

Lookup look_up(const char* host) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return kNoAnswer;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  Lookup answer = kNoAnswer;
  if (getaddrinfo(host, nullptr, &hints, &found) == 0) {
    answer = kNowhere;
    for (const addrinfo* at = found; at != nullptr; at = at->ai_next) {
      if (at->ai_family == AF_INET) {
        const ULONG ip = ntohl(reinterpret_cast<const sockaddr_in*>(at->ai_addr)->sin_addr.s_addr);
        if (ip != 0 && ip >> 24 != 127) answer = kAnswered;
      } else if (at->ai_family == AF_INET6) {
        const in6_addr* ip = &reinterpret_cast<const sockaddr_in6*>(at->ai_addr)->sin6_addr;
        if (!IN6_IS_ADDR_UNSPECIFIED(ip) && !IN6_IS_ADDR_LOOPBACK(ip)) answer = kAnswered;
      }
    }
    freeaddrinfo(found);
  }
  WSACleanup();
  return answer;
}

bool name_blocked(const Source& source) {
  const Lookup host = look_up(source.host);
  return host == kNowhere || (host == kNoAnswer && look_up("roblox.com") == kAnswered);
}

Outcome lost(Job& job, const Source& source, DWORD error) {
  job.win32 = error;
  const bool blocked = name_blocked(source);
  note(job, "no answer, error %lu%s", error, blocked ? ", the name is blocked" : "");
  return blocked ? kBlocked : kNetwork;
}

bool restart(Download& d) {
  LARGE_INTEGER zero{};
  br_sha256_init(&d.hash);
  d.have = 0;
  return SetFilePointerEx(d.file, zero, nullptr, FILE_BEGIN) && SetEndOfFile(d.file);
}

bool resume(Job& job, Download& d) {
  const ULONGLONG total = wv2_package_bytes();
  LARGE_INTEGER size{};
  br_sha256_init(&d.hash);
  d.have = 0;
  if (!GetFileSizeEx(d.file, &size)) return false;
  if (static_cast<ULONGLONG>(size.QuadPart) > total) return restart(d);
  while (d.have < static_cast<ULONGLONG>(size.QuadPart)) {
    const DWORD want = static_cast<DWORD>(std::min<ULONGLONG>(kBlock, size.QuadPart - d.have));
    DWORD got = 0;
    if (!ReadFile(d.file, d.block.data(), want, &got, nullptr) || got != want) return restart(d);
    br_sha256_update(&d.hash, d.block.data(), got);
    d.have += got;
    show(job, WV2_STATE_DOWNLOADING, d.have, total);
  }
  if (d.have == total && !matches(d.hash)) return restart(d);
  return true;
}

bool range_starts_at(const Net& net, HINTERNET request, ULONGLONG at) {
  wchar_t range[128] = {};
  DWORD size = sizeof range - sizeof range[0];
  if (!net.query(request, HTTP_QUERY_CONTENT_RANGE, range, &size, nullptr)) return false;
  const wchar_t* first = std::wcschr(range, L' ');
  const wchar_t* whole = std::wcschr(range, L'/');
  return first != nullptr && whole != nullptr && std::wcstoull(first + 1, nullptr, 10) == at &&
         std::wcstoull(whole + 1, nullptr, 10) == wv2_package_bytes();
}

Outcome receive(Job& job, const Net& net, HINTERNET request, Download& d) {
  const ULONGLONG total = wv2_package_bytes();
  DWORD status = 0;
  DWORD size = sizeof status;
  if (!net.query(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr)) {
    job.win32 = GetLastError();
    return kNetwork;
  }
  DWORD length = 0;
  size = sizeof length;
  const bool sized = net.query(request, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &length, &size, nullptr);
  if (sized) {
    note(job, "http %lu, %lu bytes", status, length);
  } else {
    note(job, "http %lu", status);
  }
  if (status == 407) return kProxy;
  if (status == 404 || status == 410) return kGone;
  if (status == 416) return kBad;
  if (status == 200) {
    if (sized && length != total) return kBad;
    if (d.have != 0 && !restart(d)) {
      job.win32 = GetLastError();
      return kDisk;
    }
  } else if (status == 206) {
    if (!range_starts_at(net, request, d.have) || (sized && d.have + length != total)) return kNetwork;
  } else {
    return kNetwork;
  }
  for (;;) {
    DWORD got = 0;
    if (!net.read(request, d.block.data(), kBlock, &got)) {
      job.win32 = GetLastError();
      note(job, "stopped at %I64u bytes, error %lu", d.have, job.win32);
      return kNetwork;
    }
    if (got == 0) break;
    if (d.have + got > total) {
      restart(d);
      return kBad;
    }
    DWORD wrote = 0;
    if (!WriteFile(d.file, d.block.data(), got, &wrote, nullptr) || wrote != got) {
      job.win32 = GetLastError();
      return kDisk;
    }
    br_sha256_update(&d.hash, d.block.data(), got);
    d.have += got;
    d.received += got;
    show(job, WV2_STATE_DOWNLOADING, d.have, total);
  }
  if (d.have < total) {
    note(job, "the server stopped at %I64u bytes", d.have);
    return kNetwork;
  }
  if (!matches(d.hash)) {
    note(job, "the file isn't Microsoft's");
    restart(d);
    return kBad;
  }
  return kGot;
}

struct Direct {
  Job& job;
  Download& d;
  const HttpResult& result;
  Outcome outcome;
  bool answered;
};

BOOL direct_sink(void* context, const BYTE* data, DWORD size) {
  Direct& c = *static_cast<Direct*>(context);
  const ULONGLONG total = wv2_package_bytes();
  if (!c.answered) {
    c.answered = true;
    if (c.result.length != 0) {
      note(c.job, "http %d, %I64u bytes", c.result.status, c.result.length);
    } else {
      note(c.job, "http %d", c.result.status);
    }
    if (c.result.length != 0 && c.d.have + c.result.length != total) {
      c.outcome = c.d.have != 0 ? kNetwork : kBad;
      return FALSE;
    }
  }
  if (c.d.have + size > total) {
    restart(c.d);
    c.outcome = kBad;
    return FALSE;
  }
  DWORD wrote = 0;
  if (!WriteFile(c.d.file, data, size, &wrote, nullptr) || wrote != size) {
    c.job.win32 = GetLastError();
    c.outcome = kDisk;
    return FALSE;
  }
  br_sha256_update(&c.d.hash, data, size);
  c.d.have += size;
  c.d.received += size;
  show(c.job, WV2_STATE_DOWNLOADING, c.d.have, total);
  return TRUE;
}

Outcome direct(Job& job, const Source& source, const std::string& url, Download& d) {
  HttpResult result{};
  Direct c{job, d, result, kNone, false};
  BOOL done = http_get(url.c_str(), direct_sink, &c, d.have, &result);
  if (!done && !c.answered && result.status == 200 && d.have != 0) {
    note(job, "http 200 instead of the rest, from the start again");
    if (!restart(d)) {
      job.win32 = GetLastError();
      return kDisk;
    }
    done = http_get(url.c_str(), direct_sink, &c, 0, &result);
  }
  if (c.outcome != kNone) return c.outcome;
  if (!c.answered && result.status != 0) note(job, "http %d", result.status);
  if (!done && result.status == 0) {
    const bool blocked = name_blocked(source);
    note(job, "no answer, %s%s", result.error, blocked ? ", the name is blocked" : "");
    return blocked ? kBlocked : kNetwork;
  }
  if (!done) {
    note(job, "%s", result.error);
    if (result.status == 404 || result.status == 410) return kGone;
    return result.status == 416 ? kBad : kNetwork;
  }
  if (d.have < wv2_package_bytes()) {
    note(job, "the server stopped at %I64u bytes", d.have);
    return kNetwork;
  }
  if (!matches(d.hash)) {
    note(job, "the file isn't Microsoft's");
    restart(d);
    return kBad;
  }
  return kGot;
}

Outcome wininet(Job& job, const Net& net, const Source& source, const std::string& url, Download& d) {
  HINTERNET session = net.open(L"Blox7", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
  if (session == nullptr) return lost(job, source, GetLastError());
  DWORD connect = 30000;
  DWORD wait = 60000;
  net.set_option(session, INTERNET_OPTION_CONNECT_TIMEOUT, &connect, sizeof connect);
  net.set_option(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &wait, sizeof wait);
  wchar_t range[64] = {};
  if (d.have != 0) std::swprintf(range, 64, L"Range: bytes=%I64u-\r\n", d.have);
  const DWORD flags = INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_COOKIES |
                      INTERNET_FLAG_NO_UI | (source.https ? INTERNET_FLAG_SECURE : 0);
  HINTERNET request = net.open_url(session, widen(url).c_str(), d.have != 0 ? range : nullptr,
                                   d.have != 0 ? static_cast<DWORD>(-1) : 0, flags, 0);
  const Outcome outcome = request != nullptr ? receive(job, net, request, d) : lost(job, source, GetLastError());
  if (request != nullptr) net.close(request);
  net.close(session);
  return outcome;
}

Outcome attempt(Job& job, const Net& net, const Source& source, Download& d) {
  const std::string url = std::string(source.https ? "https://" : "http://") + source.host + kPath;
  note(job, "source %s from %I64u%s", url.c_str(), d.have, source.own_tls ? " with our own tls" : "");
  const ULONGLONG started = GetTickCount64();
  d.received = 0;
  const Outcome outcome = source.own_tls ? direct(job, source, url, d) : wininet(job, net, source, url, d);
  if (d.received != 0) note(job, "got %I64u bytes in %.1f s", d.received, seconds(started));
  return outcome;
}

int verdict(const Outcome* last) {
  for (int i = 0; i < kSourceCount; ++i)
    if (last[i] == kProxy) return WV2_ERROR_PROXY_AUTH;
  for (int i = 0; i < kSourceCount; ++i)
    if (last[i] == kBlocked) return WV2_ERROR_BLOCKED;
  int answers = 0;
  bool gone = true;
  bool answered = true;
  for (int i = 0; i < kSourceCount; ++i) {
    if (last[i] == kNetwork) continue;
    ++answers;
    gone = gone && last[i] == kGone;
    answered = answered && (last[i] == kGone || last[i] == kBad);
  }
  if (answers < 2) return WV2_ERROR_NETWORK;
  return gone ? WV2_ERROR_GONE : answered ? WV2_ERROR_MISMATCH : WV2_ERROR_NETWORK;
}

HANDLE open_part(const std::wstring& path) {
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION ||
        GetTickCount64() - started >= 10000)
      return file;
    Sleep(250);
  }
}

int fetch(Job& job, const std::wstring& part, const std::wstring& pkg) {
  Net net;
  if (!load(net)) {
    job.win32 = GetLastError();
    note(job, "wininet.dll didn't load, error %lu", job.win32);
    if (net.dll != nullptr) FreeLibrary(net.dll);
    return WV2_ERROR_NETWORK;
  }
  Download d;
  d.file = open_part(part);
  if (d.file == INVALID_HANDLE_VALUE) {
    job.win32 = GetLastError();
    FreeLibrary(net.dll);
    return write_failure(job.win32);
  }
  int result = WV2_OK;
  if (!resume(job, d)) {
    job.win32 = GetLastError();
    result = write_failure(job.win32);
  }
  bool got = result == WV2_OK && d.have == wv2_package_bytes();
  constexpr DWORD kPauses[] = {5000, 15000, 30000};
  Outcome last[kSourceCount] = {};
  int source = 0;
  int misses = 0;
  size_t pauses = 0;
  while (result == WV2_OK && !got) {
    if (misses >= kSourceCount) {
      bool offline = true;
      for (const Outcome outcome : last) offline = offline && outcome == kNetwork;
      if (!offline || pauses == sizeof kPauses / sizeof kPauses[0]) break;
      note(job, "no source answered, trying again in %lu s", kPauses[pauses] / 1000);
      Sleep(kPauses[pauses++]);
      misses = 0;
    }
    const ULONGLONG before = d.have;
    const Outcome outcome = attempt(job, net, kSources[source], d);
    if (outcome == kGot) {
      got = true;
    } else if (outcome == kDisk) {
      result = write_failure(job.win32);
    } else if (outcome == kNetwork && d.have > before) {
      last[source] = outcome;
      misses = 0;
      pauses = 0;
    } else {
      last[source] = outcome;
      ++misses;
      source = (source + 1) % kSourceCount;
    }
  }
  CloseHandle(d.file);
  FreeLibrary(net.dll);
  if (result != WV2_OK) return result;
  if (!got) return verdict(last);
  if (!MoveFileExW(part.c_str(), pkg.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    job.win32 = GetLastError();
    return WV2_ERROR_BLOCKED_FILE;
  }
  return WV2_OK;
}

int mark(const std::wstring& dir, DWORD* win32) {
  HANDLE marker = CreateFileW((dir + L"\\" + WV2_MARKER).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (marker == INVALID_HANDLE_VALUE) {
    *win32 = GetLastError();
    return write_failure(*win32);
  }
  CloseHandle(marker);
  return WV2_OK;
}

int move_in(const std::wstring& partial, const std::wstring& folder, DWORD* win32) {
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    if (exists(folder)) remove_tree(folder);
    if (MoveFileExW(partial.c_str(), folder.c_str(), 0)) return WV2_OK;
    *win32 = GetLastError();
    const bool busy =
        *win32 == ERROR_ACCESS_DENIED || *win32 == ERROR_SHARING_VIOLATION || *win32 == ERROR_ALREADY_EXISTS;
    if (!busy || GetTickCount64() - started >= 10000) return WV2_ERROR_BLOCKED_FILE;
    Sleep(250);
  }
}

int install(Job& job, const std::wstring& pkg, const std::wstring& folder) {
  wchar_t suffix[32];
  std::swprintf(suffix, 32, L"\\.partial-%lu", GetCurrentProcessId());
  const std::wstring partial = job.home + suffix;
  if (!CreateDirectoryW(partial.c_str(), nullptr)) {
    job.win32 = GetLastError();
    return write_failure(job.win32);
  }
  const ULONGLONG total = wv2_unpack_bytes(kAllFiles);
  show(job, WV2_STATE_UNPACKING, 0, total);
  note(job, "unpacking %I64u bytes", total);
  const ULONGLONG started = GetTickCount64();
  DWORD win32 = 0;
  int result = wv2_unpack(pkg.c_str(), partial.c_str(), kAllFiles, unpacked, &job, &win32);
  if (result == WV2_OK) result = mark(partial, &win32);
  if (result == WV2_OK) result = move_in(partial, folder, &win32);
  if (result != WV2_OK) {
    job.win32 = win32;
    remove_tree(partial);
    return result;
  }
  note(job, "unpacked in %.1f s", seconds(started));
  DeleteFileW(pkg.c_str());
  RemoveDirectoryW((job.home + L"\\download").c_str());
  return wv2_folder_complete() ? WV2_OK : WV2_ERROR_INTERNAL;
}

int setup(Job& job) {
  const std::wstring folder = job.home + L"\\" + WV2_FOLDER;
  remove_partials(job.home);
  if (wv2_folder_complete()) {
    note(job, "already set up");
    return WV2_OK;
  }
  if (exists(folder)) remove_tree(folder);
  if (!on_local_drive(job.home)) {
    note(job, "AppData is on a network drive");
    return WV2_ERROR_NETWORK_DRIVE;
  }
  const std::wstring download = job.home + L"\\download";
  const std::wstring stem = download + L"\\" + widen(wv2_package_sha256());
  const std::wstring part = stem + L".part";
  const std::wstring pkg = stem + L".pkg";
  if (folder.size() + 1 + wv2_unpack_longest(kAllFiles) >= MAX_PATH || part.size() >= MAX_PATH) {
    note(job, "paths too long, %u characters to the runtime folder", static_cast<unsigned>(folder.size()));
    return WV2_ERROR_PATH_TOO_LONG;
  }
  const bool fetched = exists(pkg) && package_ok(job, pkg);
  if (!fetched) DeleteFileW(pkg.c_str());
  const ULONGLONG room = free_bytes(job.home);
  const ULONGLONG needed = fetched ? kRoomToUnpack : kRoomToFetch;
  if (room < needed) {
    note(job, "%I64u MB free, %I64u MB needed", room / 1000000, needed / 1000000);
    return WV2_ERROR_DISK_SPACE;
  }
  if (!fetched) {
    CreateDirectoryW(download.c_str(), nullptr);
    const int result = fetch(job, part, pkg);
    if (result != WV2_OK) return result;
  }
  return install(job, pkg, folder);
}

void finish(Job& job, int result, ULONGLONG started) {
  const std::wstring error_file = job.home + L"\\setup-error.txt";
  if (result == WV2_OK) {
    DeleteFileW(error_file.c_str());
    note(job, "done in %.1f s", seconds(started));
  } else {
    if (FILE* file = _wfopen(error_file.c_str(), L"wb")) {
      std::fprintf(file, "%d %lu\r\n", result, job.win32);
      std::fclose(file);
    }
    note(job, "failed in %.1f s: %s, error %lu", seconds(started), narrow(wv2_error_text(result)).c_str(), job.win32);
  }
  if (job.status == nullptr) return;
  InterlockedExchange(&job.status->error, result);
  InterlockedExchange(reinterpret_cast<volatile LONG*>(&job.status->win32), static_cast<LONG>(job.win32));
  InterlockedExchange(&job.status->state, result == WV2_OK ? WV2_STATE_DONE : WV2_STATE_FAILED);
}

int run_locked(const std::wstring& home, int now) {
  Job job;
  job.home = home;
  make_dirs(home);
  open_log(job);
  job.hurry = CreateEventW(nullptr, TRUE, FALSE, WV2_HURRY);
  HANDLE mapping =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(wv2_status), WV2_STATUS);
  void* view = mapping != nullptr ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(wv2_status)) : nullptr;
  job.status = static_cast<volatile wv2_status*>(view);
  if (job.status != nullptr) {
    InterlockedExchange(&job.status->error, WV2_OK);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&job.status->win32), 0);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&job.status->pid), static_cast<LONG>(GetCurrentProcessId()));
  }
  job.background = !now && SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  show(job, WV2_STATE_PREPARING, 0, 0);
  note(job, "start, Windows %s, %s", windows_version().c_str(), now ? "now" : "in the background");
  const ULONGLONG started = GetTickCount64();
  const int result = setup(job);
  finish(job, result, started);
  if (job.background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
  if (job.hurry != nullptr) CloseHandle(job.hurry);
  if (view != nullptr) UnmapViewOfFile(view);
  if (mapping != nullptr) CloseHandle(mapping);
  if (job.log != INVALID_HANDLE_VALUE) CloseHandle(job.log);
  return result;
}

DWORD owner() {
  wv2_status status;
  wv2_status_read(&status);
  return status.pid != GetCurrentProcessId() ? status.pid : 0;
}

void wait_for_exit(DWORD pid) {
  HANDLE process = pid != 0 ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
  if (process == nullptr) return;
  WaitForSingleObject(process, 10000);
  CloseHandle(process);
}

int run(int now) {
  std::wstring home(32768, L'\0');
  if (!wv2_home(home.data(), static_cast<DWORD>(home.size()))) return WV2_ERROR_INTERNAL;
  home.resize(std::wcslen(home.c_str()));
  HANDLE mutex = CreateMutexW(nullptr, FALSE, WV2_MUTEX);
  if (mutex == nullptr) return WV2_ERROR_INTERNAL;
  DWORD wait = WaitForSingleObject(mutex, 2000);
  DWORD previous = wait == WAIT_ABANDONED ? owner() : 0;
  if (wait == WAIT_TIMEOUT) {
    previous = owner();
    if (now) {
      if (HANDLE hurry = OpenEventW(EVENT_MODIFY_STATE, FALSE, WV2_HURRY)) {
        SetEvent(hurry);
        CloseHandle(hurry);
      }
    }
    wait = WaitForSingleObject(mutex, INFINITE);
    const bool complete = wv2_folder_complete();
    const int last = wv2_last_error(nullptr);
    if (wait == WAIT_OBJECT_0 && (complete || last != WV2_OK)) {
      ReleaseMutex(mutex);
      CloseHandle(mutex);
      return complete ? WV2_OK : last;
    }
  }
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
    CloseHandle(mutex);
    return WV2_ERROR_INTERNAL;
  }
  if (wait == WAIT_ABANDONED) wait_for_exit(previous);
  const int result = run_locked(home, now);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return result;
}

}  // namespace

extern "C" int wv2_setup_run(int now) { return run(now); }

extern "C" int wv2_leftover_remove(void) { return wv2_windows_major() >= 10 && wv2_copy_remove(); }

extern "C" int wv2_copy_remove(void) {
  std::wstring home(32768, L'\0');
  if (!wv2_home(home.data(), static_cast<DWORD>(home.size()))) return 0;
  home.resize(std::wcslen(home.c_str()));
  HANDLE mutex = CreateMutexW(nullptr, FALSE, WV2_MUTEX);
  if (mutex == nullptr) return 0;
  const DWORD wait = WaitForSingleObject(mutex, 0);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
    CloseHandle(mutex);
    return 0;
  }
  remove_tree(home + L"\\" + WV2_FOLDER);
  remove_tree(home + L"\\download");
  remove_partials(home);
  DeleteFileW((home + L"\\setup-error.txt").c_str());
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 1;
}
