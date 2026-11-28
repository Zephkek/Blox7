#include <windows.h>
#include <psapi.h>
#include <winioctl.h>

#include <bearssl_hash.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../webview2/wv2files.h"
#include "../webview2/wv2setup.h"
#include "../webview2/wv2unpack.h"

namespace {

struct Entry {
  ULONGLONG size;
};

double since(ULONGLONG start) { return static_cast<double>(GetTickCount64() - start) / 1000.0; }

std::string narrow(const std::wstring& text) {
  std::string out;
  for (wchar_t c : text) out += c < 128 ? static_cast<char>(c) : '?';
  return out;
}

void remove_tree(const std::wstring& dir) {
  WIN32_FIND_DATAW found;
  std::vector<std::wstring> names;
  HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      if (name != L"." && name != L"..")
        names.push_back(name + (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? L"\\" : L""));
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  for (const std::wstring& name : names) {
    if (name.back() == L'\\') {
      remove_tree(dir + L"\\" + name.substr(0, name.size() - 1));
    } else {
      SetFileAttributesW((dir + L"\\" + name).c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW((dir + L"\\" + name).c_str());
    }
  }
  RemoveDirectoryW(dir.c_str());
}

void walk(const std::wstring& root, const std::wstring& rel, std::map<std::wstring, Entry>& out) {
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((root + (rel.empty() ? L"" : L"\\" + rel) + L"\\*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring name = found.cFileName;
    if (name == L"." || name == L"..") continue;
    const std::wstring path = rel.empty() ? name : rel + L"\\" + name;
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      walk(root, path, out);
    } else {
      out[path] = {(static_cast<ULONGLONG>(found.nFileSizeHigh) << 32) | found.nFileSizeLow};
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);
}

std::string digest_of(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return "unreadable";
  br_sha256_context hash;
  br_sha256_init(&hash);
  std::vector<unsigned char> block(1 << 20);
  DWORD got = 0;
  while (ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &got, nullptr) && got)
    br_sha256_update(&hash, block.data(), got);
  CloseHandle(file);
  unsigned char digest[32];
  br_sha256_out(&hash, digest);
  char text[65];
  for (int i = 0; i < 32; ++i) std::snprintf(text + 2 * i, 3, "%02x", digest[i]);
  return text;
}

void report_memory() {
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof counters;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters))
    std::printf("peak working set %.1f MB, peak commit %.1f MB\n", counters.PeakWorkingSetSize / 1048576.0,
                counters.PeakPagefileUsage / 1048576.0);
}

void progress(void* context, ULONGLONG done, ULONGLONG total) {
  ULONGLONG* calls = static_cast<ULONGLONG*>(context);
  if (++calls[0] == 1 || done == total) calls[1] = done;
}

int unpack(const wchar_t* package, const wchar_t* out, bool all) {
  if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
    std::printf("unpack: %s already exists\n", narrow(out).c_str());
    return 2;
  }
  CreateDirectoryW(out, nullptr);
  ULONGLONG calls[2] = {};
  DWORD win32 = 0;
  const ULONGLONG start = GetTickCount64();
  const int result = wv2_unpack(package, out, all ? 1 : 0, progress, calls, &win32);
  std::printf("unpack %s: result %d (%s), win32 %lu, %.1f s, %I64u progress calls, last at %I64u of %I64u\n",
              all ? "all" : "fixed", result, narrow(wv2_error_text(result)).c_str(), win32, since(start), calls[0],
              calls[1], wv2_unpack_bytes(all ? 1 : 0));
  report_memory();
  return result;
}

int compare(const wchar_t* out, const wchar_t* reference, const wchar_t* names) {
  std::map<std::wstring, Entry> got, want;
  walk(out, L"", got);
  walk(reference, L"", want);
  std::set<std::wstring> expected;
  if (names != nullptr) {
    FILE* list = _wfopen(names, L"rb");
    if (list == nullptr) {
      std::printf("compare: no names file\n");
      return 2;
    }
    char line[1024];
    while (std::fgets(line, sizeof line, list)) {
      char* tab = std::strchr(line, '\t');
      if (tab == nullptr) continue;
      *tab = 0;
      std::wstring name;
      for (char* c = line; *c; ++c) name += *c == '/' ? L'\\' : static_cast<wchar_t>(*c);
      expected.insert(name);
    }
    std::fclose(list);
  } else {
    for (const auto& [name, entry] : want) expected.insert(name);
  }
  int problems = 0;
  ULONGLONG bytes = 0;
  for (const std::wstring& name : expected) {
    const auto mine = got.find(name);
    const auto theirs = want.find(name);
    if (theirs == want.end()) {
      std::printf("compare: %s is not in the reference\n", narrow(name).c_str());
      ++problems;
    } else if (mine == got.end()) {
      std::printf("compare: %s is missing\n", narrow(name).c_str());
      ++problems;
    } else if (mine->second.size != theirs->second.size) {
      std::printf("compare: %s is %I64u bytes, the reference %I64u\n", narrow(name).c_str(), mine->second.size,
                  theirs->second.size);
      ++problems;
    } else if (digest_of(std::wstring(out) + L"\\" + name) != digest_of(std::wstring(reference) + L"\\" + name)) {
      std::printf("compare: %s differs\n", narrow(name).c_str());
      ++problems;
    } else {
      bytes += mine->second.size;
    }
  }
  for (const auto& [name, entry] : got) {
    if (!expected.count(name)) {
      std::printf("compare: extra file %s\n", narrow(name).c_str());
      ++problems;
    }
  }
  std::printf("compare: %zu expected, %zu written, %I64u bytes equal, %d problems\n", expected.size(), got.size(),
              bytes, problems);
  return problems ? 1 : 0;
}

bool copy_package(const wchar_t* from, const std::wstring& to) {
  return CopyFileW(from, to.c_str(), FALSE) != 0;
}

bool flip(const std::wstring& path, ULONGLONG at) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  OVERLAPPED where{};
  where.Offset = static_cast<DWORD>(at);
  where.OffsetHigh = static_cast<DWORD>(at >> 32);
  unsigned char byte = 0;
  DWORD done = 0;
  bool ok = ReadFile(file, &byte, 1, &done, &where) && done == 1;
  byte ^= 0x01;
  ok = ok && WriteFile(file, &byte, 1, &done, &where) && done == 1;
  CloseHandle(file);
  return ok;
}

bool cut(const std::wstring& path, ULONGLONG size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER at;
  at.QuadPart = static_cast<LONGLONG>(size);
  const bool ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && SetEndOfFile(file);
  CloseHandle(file);
  return ok;
}

int refuse(const wchar_t* package, const wchar_t* scratch) {
  struct Case {
    const char* name;
    ULONGLONG at;
    bool truncate;
    bool accept;
  };
  const Case cases[] = {
      {"main stream, middle byte", WV2_MAIN_AT + WV2_MAIN_PACKED / 2, false, false},
      {"main stream, last byte", WV2_MAIN_AT + WV2_MAIN_PACKED - 1, false, false},
      {"range coder stream, first byte", WV2_RC_AT, false, false},
      {"range coder stream, middle byte", WV2_RC_AT + WV2_RC_BYTES / 2, false, false},
      {"call stream, middle byte", WV2_CALL_AT + WV2_CALL_PACKED / 2, false, false},
      {"jump stream, middle byte", WV2_JUMP_AT + WV2_JUMP_PACKED / 2, false, false},
      {"jump stream, last byte", WV2_JUMP_AT + WV2_JUMP_PACKED - 1, false, false},
      {"outside the streams, the pe header", 0x80, false, true},
      {"one byte short", WV2_PACKAGE_BYTES - 1, true, false},
      {"cut in the main stream", WV2_MAIN_AT + WV2_MAIN_PACKED / 2, true, false},
  };
  CreateDirectoryW(scratch, nullptr);
  const std::wstring copy = std::wstring(scratch) + L"\\damaged.exe";
  const std::wstring out = std::wstring(scratch) + L"\\out";
  int wrong = 0;
  for (const Case& c : cases) {
    if (!copy_package(package, copy) || !(c.truncate ? cut(copy, c.at) : flip(copy, c.at))) {
      std::printf("refuse: could not prepare %s\n", c.name);
      return 2;
    }
    remove_tree(out);
    CreateDirectoryW(out.c_str(), nullptr);
    DWORD win32 = 0;
    const ULONGLONG start = GetTickCount64();
    const int result = wv2_unpack(copy.c_str(), out.c_str(), 0, nullptr, nullptr, &win32);
    const bool right = c.accept ? result == WV2_OK : result != WV2_OK;
    std::printf("refuse: %-36s at %9I64u -> %2d (%s) in %.1f s %s\n", c.name, c.at, result,
                narrow(wv2_error_text(result)).c_str(), since(start), right ? "ok" : "WRONG");
    wrong += !right;
    remove_tree(out);
  }
  DeleteFileW(copy.c_str());
  std::printf("refuse: %d of %zu cases wrong\n", wrong, sizeof cases / sizeof cases[0]);
  return wrong ? 1 : 0;
}

struct Kill {
  LONGLONG at;
  LONG state;
};

DWORD WINAPI die_at(void* context) {
  const Kill kill = *static_cast<Kill*>(context);
  for (;;) {
    wv2_status status;
    wv2_status_read(&status);
    if (status.state == kill.state && status.done >= kill.at && status.pid == GetCurrentProcessId()) {
      std::printf("setup: killing the job in state %ld at %I64d of %I64d bytes\n", status.state, status.done,
                  status.total);
      std::fflush(stdout);
      TerminateProcess(GetCurrentProcess(), 99);
    }
    Sleep(5);
  }
}

bool present(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool put_file(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD done = 0;
  const bool ok = WriteFile(file, "blox7", 5, &done, nullptr) && done == 5;
  CloseHandle(file);
  return ok;
}

bool make_junction(const std::wstring& link, const std::wstring& target) {
  if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
  HANDLE dir = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (dir == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + target;
  const USHORT substitute_bytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
  const USHORT print_bytes = static_cast<USHORT>(target.size() * sizeof(wchar_t));
  std::vector<BYTE> buffer(16 + substitute_bytes + 2 + print_bytes + 2);
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  const USHORT fields[] = {static_cast<USHORT>(buffer.size() - 8), 0, 0, substitute_bytes,
                           static_cast<USHORT>(substitute_bytes + 2), print_bytes};
  std::memcpy(buffer.data(), &tag, sizeof tag);
  std::memcpy(buffer.data() + 4, fields, sizeof fields);
  std::memcpy(buffer.data() + 16, substitute.c_str(), substitute_bytes);
  std::memcpy(buffer.data() + 16 + substitute_bytes + 2, target.c_str(), print_bytes);
  DWORD done = 0;
  const bool ok = DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  nullptr, 0, &done, nullptr) != 0;
  CloseHandle(dir);
  return ok;
}

struct Hold {
  HANDLE taken;
  HANDLE release;
};

DWORD WINAPI hold_mutex(void* context) {
  const Hold* hold = static_cast<Hold*>(context);
  HANDLE mutex = CreateMutexW(nullptr, FALSE, WV2_MUTEX);
  WaitForSingleObject(mutex, INFINITE);
  SetEvent(hold->taken);
  WaitForSingleObject(hold->release, INFINITE);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}

int leftover(const wchar_t* scratch) {
  const std::wstring root = scratch;
  if (present(root)) {
    std::printf("leftover: %s already exists\n", narrow(root).c_str());
    return 2;
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
  const std::wstring home = root + L"\\Blox7\\webview2";
  const std::wstring folder = home + L"\\" + WV2_FOLDER;
  const std::wstring keep = root + L"\\keep";
  for (const std::wstring& dir : {root, root + L"\\Blox7", home, folder, folder + L"\\EBWebView", keep,
                                  home + L"\\download", home + L"\\.partial-1"})
    CreateDirectoryW(dir.c_str(), nullptr);
  int failed = 0;
  const auto check = [&failed](bool ok, const char* what) {
    std::printf("leftover: %s %s\n", ok ? "pass" : "FAIL", what);
    failed += !ok;
  };
  check(put_file(folder + L"\\msedge.dll") && put_file(folder + L"\\EBWebView\\a.dll") &&
            put_file(home + L"\\download\\x.part") && put_file(home + L"\\.partial-1\\f") &&
            put_file(home + L"\\setup.log") && put_file(keep + L"\\sentinel.txt"),
        "files made");
  check(make_junction(folder + L"\\link", keep), "junction to a folder outside made");
  check(present(folder + L"\\link\\sentinel.txt"), "the junction leads outside");
  const bool modern = wv2_windows_major() >= 10;
  check(wv2_leftover() == (modern ? 1 : 0), "a leftover counts only on windows 10 and later");
  if (modern) {
    Hold hold = {CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE holder = CreateThread(nullptr, 0, hold_mutex, &hold, 0, nullptr);
    WaitForSingleObject(hold.taken, INFINITE);
    check(wv2_leftover_remove() == 0, "nothing removed while a job holds the mutex");
    check(present(folder + L"\\msedge.dll"), "the copy stays while a job runs");
    SetEvent(hold.release);
    WaitForSingleObject(holder, INFINITE);
    CloseHandle(holder);
    CloseHandle(hold.taken);
    CloseHandle(hold.release);
    check(wv2_leftover_remove() == 1, "removed once the mutex is free");
    check(!present(folder), "the copy is gone");
    check(!present(home + L"\\download"), "the download folder is gone");
    check(!present(home + L"\\.partial-1"), "partial folders are gone");
    check(present(keep + L"\\sentinel.txt"), "the junction's target is untouched");
    check(present(home + L"\\setup.log"), "setup.log stays");
    check(wv2_leftover() == 0, "no leftover afterwards");
  } else {
    check(wv2_leftover_remove() == 0, "nothing removed below windows 10");
    check(present(folder + L"\\msedge.dll"), "the copy stays below windows 10");
  }
  RemoveDirectoryW((folder + L"\\link").c_str());
  remove_tree(root);
  std::printf("leftover: %d failed\n", failed);
  return failed ? 1 : 0;
}

int watch(int seconds) {
  for (int i = 0; i < seconds * 4; ++i) {
    wv2_status status;
    const int running = wv2_status_read(&status);
    std::printf("watch: running %d state %ld error %ld win32 %lu pid %lu done %I64d total %I64d\n", running,
                status.state, status.error, status.win32, status.pid, status.done, status.total);
    std::fflush(stdout);
    Sleep(250);
  }
  return 0;
}

int setup(bool now, LONGLONG kill_at, bool in_unpack) {
  static Kill kill = {};
  if (kill_at > 0) {
    kill = {kill_at, static_cast<LONG>(in_unpack ? WV2_STATE_UNPACKING : WV2_STATE_DOWNLOADING)};
    CloseHandle(CreateThread(nullptr, 0, die_at, &kill, 0, nullptr));
  }
  const ULONGLONG start = GetTickCount64();
  const int result = wv2_setup_run(now ? 1 : 0);
  DWORD win32 = 0;
  const int last = wv2_last_error(&win32);
  wv2_status status;
  const int running = wv2_status_read(&status);
  std::printf("setup %s: result %d (%s) in %.1f s, folder complete %d, last error %d win32 %lu, job running %d\n",
              now ? "now" : "background", result, narrow(wv2_error_text(result)).c_str(), since(start),
              wv2_folder_complete(), last, win32, running);
  report_memory();
  return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const std::wstring what = argc > 1 ? argv[1] : L"";
  if (what == L"unpack" && argc == 5) return unpack(argv[2], argv[3], std::wstring(argv[4]) == L"all");
  if (what == L"compare" && (argc == 4 || argc == 5)) return compare(argv[2], argv[3], argc == 5 ? argv[4] : nullptr);
  if (what == L"refuse" && argc == 4) return refuse(argv[2], argv[3]);
  if (what == L"setup" && argc >= 3 && argc <= 5)
    return setup(std::wstring(argv[2]) == L"now", argc >= 4 ? _wtoi64(argv[3]) : 0,
                 argc == 5 && std::wstring(argv[4]) == L"unpack");
  if (what == L"watch" && argc == 3) return watch(_wtoi(argv[2]));
  if (what == L"leftover" && argc == 3) return leftover(argv[2]);
  std::printf("usage: wv2setup_test unpack <package> <out> fixed|all\n"
              "       wv2setup_test compare <out> <reference> [names]\n"
              "       wv2setup_test refuse <package> <scratch>\n"
              "       wv2setup_test setup now|background [kill at byte [download|unpack]]\n"
              "       wv2setup_test watch <seconds>\n"
              "       wv2setup_test leftover <scratch>\n");
  return 2;
}
