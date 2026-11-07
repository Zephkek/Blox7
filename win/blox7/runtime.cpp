#include "blox7.h"

#include <fdi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "payload_id.h"

namespace blox7 {
namespace {

constexpr wchar_t kComplete[] = L".blox7-complete";
constexpr wchar_t kFileList[] = L".blox7-files";

std::wstring env(const wchar_t* name) {
  std::wstring value(32767, L'\0');
  const DWORD n = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
  if (n == 0 || n >= value.size()) return {};
  value.resize(n);
  return value;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring parent_of(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

void make_dirs(const std::wstring& path) {
  for (size_t i = 3; i < path.size(); ++i)
    if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
  CreateDirectoryW(path.c_str(), nullptr);
}

void remove_tree(const std::wstring& dir) {
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring path = dir + L"\\" + name;
      if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        remove_tree(path);
      } else {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path.c_str());
      }
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  RemoveDirectoryW(dir.c_str());
}

std::wstring trim(std::wstring text) {
  auto blank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0xFEFF; };
  while (!text.empty() && blank(text.back())) text.pop_back();
  size_t start = 0;
  while (start < text.size() && blank(text[start])) ++start;
  return text.substr(start);
}

std::string read_file(const std::wstring& path, size_t limit) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string text;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
    const uint64_t total = static_cast<uint64_t>(size.QuadPart);
    const uint64_t start = total > limit ? total - limit : 0;
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(start);
    SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    text.resize(static_cast<size_t>(total - start));
    DWORD got = 0;
    if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
    text.resize(got);
  }
  CloseHandle(file);
  return text;
}

std::vector<std::wstring> lines_of(const std::wstring& text) {
  std::vector<std::wstring> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(L'\n', pos);
    if (end == std::wstring::npos) end = text.size();
    std::wstring line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    lines.push_back(std::move(line));
    pos = end + 1;
  }
  return lines;
}

std::map<std::wstring, std::wstring> read_ini(const std::wstring& path) {
  std::map<std::wstring, std::wstring> values;
  for (std::wstring line : lines_of(widen(read_file(path, 64 << 10)))) {
    line = trim(line);
    if (line.empty() || line[0] == L'#' || line[0] == L';' || line[0] == L'[') continue;
    const size_t eq = line.find(L'=');
    if (eq != std::wstring::npos) values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return values;
}

std::wstring json_string(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t at = json.find(needle);
  if (at == std::string::npos) return {};
  at = json.find(':', at + needle.size());
  if (at == std::string::npos) return {};
  at = json.find_first_not_of(" \t\r\n", at + 1);
  if (at == std::string::npos || json[at] != '"') return {};
  std::string value;
  for (size_t i = at + 1; i < json.size() && json[i] != '"'; ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      const char escaped = json[++i];
      value += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
    } else {
      value += json[i];
    }
  }
  return widen(value);
}

std::wstring blox7_dir(const Paths& paths, const wchar_t* tail) { return paths.data + L"\\root\\" + tail; }

bool same_size(const std::wstring& a, const std::wstring& b) {
  WIN32_FILE_ATTRIBUTE_DATA x, y;
  return GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &x) &&
         GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &y) && x.nFileSizeLow == y.nFileSizeLow &&
         x.nFileSizeHigh == y.nFileSizeHigh;
}

void copy_tree(const std::wstring& from, const std::wstring& to) {
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((from + L"\\*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  make_dirs(to);
  do {
    const std::wstring name = found.cFileName;
    if (name == L"." || name == L"..") continue;
    const std::wstring source = from + L"\\" + name, target = to + L"\\" + name;
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      copy_tree(source, target);
    else if (!same_size(source, target))
      CopyFileW(source.c_str(), target.c_str(), FALSE);
  } while (FindNextFileW(search, &found));
  FindClose(search);
}

struct Stream {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  HANDLE file = INVALID_HANDLE_VALUE;
};

struct Extraction {
  const uint8_t* cab = nullptr;
  size_t cab_size = 0;
  std::wstring dir;
  uint64_t total = 0;
  uint64_t written = 0;
  uint64_t reported = 0;
  const Progress* progress = nullptr;
  std::wstring error;
};

// fdi callbacks get no context, so a global. one at a time, as god intended
Extraction* g_extraction = nullptr;

FNALLOC(fdi_alloc) { return std::malloc(cb); }
FNFREE(fdi_free) { std::free(pv); }

FNOPEN(fdi_open) {
  (void)pszFile;
  (void)oflag;
  (void)pmode;
  auto* stream = new Stream;
  stream->data = g_extraction->cab;
  stream->size = g_extraction->cab_size;
  return reinterpret_cast<INT_PTR>(stream);
}

FNREAD(fdi_read) {
  auto* stream = reinterpret_cast<Stream*>(hf);
  if (!stream->data) return static_cast<UINT>(-1);
  const size_t n = std::min<size_t>(cb, stream->size - stream->pos);
  std::memcpy(pv, stream->data + stream->pos, n);
  stream->pos += n;
  return static_cast<UINT>(n);
}

FNWRITE(fdi_write) {
  auto* stream = reinterpret_cast<Stream*>(hf);
  DWORD done = 0;
  if (stream->file == INVALID_HANDLE_VALUE || !WriteFile(stream->file, pv, cb, &done, nullptr) || done != cb)
    return static_cast<UINT>(-1);
  Extraction& x = *g_extraction;
  x.written += cb;
  if (x.progress && *x.progress && x.total && x.written - x.reported >= (1u << 20)) {
    x.reported = x.written;
    (*x.progress)(std::min(1.0, static_cast<double>(x.written) / static_cast<double>(x.total)));
  }
  return cb;
}

FNCLOSE(fdi_close) {
  auto* stream = reinterpret_cast<Stream*>(hf);
  if (stream->file != INVALID_HANDLE_VALUE) CloseHandle(stream->file);
  delete stream;
  return 0;
}

FNSEEK(fdi_seek) {
  auto* stream = reinterpret_cast<Stream*>(hf);
  if (stream->data) {
    const long base = seektype == SEEK_SET   ? 0
                      : seektype == SEEK_CUR ? static_cast<long>(stream->pos)
                                             : static_cast<long>(stream->size);
    const long to = base + dist;
    if (to < 0 || static_cast<size_t>(to) > stream->size) return -1;
    stream->pos = static_cast<size_t>(to);
    return to;
  }
  LARGE_INTEGER move, now;
  move.QuadPart = dist;
  const DWORD how = seektype == SEEK_SET ? FILE_BEGIN : seektype == SEEK_CUR ? FILE_CURRENT : FILE_END;
  if (!SetFilePointerEx(stream->file, move, &now, how)) return -1;
  return static_cast<long>(now.QuadPart);
}

FNFDINOTIFY(fdi_notify) {
  Extraction& x = *g_extraction;
  if (fdint == fdintCOPY_FILE) {
    const std::wstring path = x.dir + L"\\" + widen(pfdin->psz1);
    make_dirs(parent_of(path));
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      x.error = L"cannot write " + path;
      return -1;
    }
    auto* stream = new Stream;
    stream->file = file;
    return reinterpret_cast<INT_PTR>(stream);
  }
  if (fdint == fdintCLOSE_FILE_INFO) {
    fdi_close(pfdin->hf);
    return TRUE;
  }
  return 0;
}

using FdiCreate = HFDI(DIAMONDAPI*)(PFNALLOC, PFNFREE, PFNOPEN, PFNREAD, PFNWRITE, PFNCLOSE, PFNSEEK, int, PERF);
using FdiCopy = BOOL(DIAMONDAPI*)(HFDI, char*, char*, int, PFNFDINOTIFY, PFNFDIDECRYPT, void*);
using FdiDestroy = BOOL(DIAMONDAPI*)(HFDI);

std::wstring check_file_list(const std::wstring& dir) {
  const std::vector<std::wstring> lines = lines_of(widen(read_file(dir + L"\\" + kFileList, 1 << 20)));
  size_t checked = 0;
  for (const std::wstring& line : lines) {
    const size_t space = line.find(L' ');
    if (space == std::wstring::npos) continue;
    const uint64_t want = std::wcstoull(line.c_str(), nullptr, 10);
    const std::wstring path = dir + L"\\" + line.substr(space + 1);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        ((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) != want)
      return L"the unpacked runtime is incomplete: " + line.substr(space + 1);
    ++checked;
  }
  return checked ? std::wstring() : L"the runtime has no file list";
}

// we used to have another name. moving day, once, so nobody gets signed out
const wchar_t kOldName[] = L"mock" L"tail";

void rename_old_files(const std::wstring& dir) {
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((dir + L"\\*" + kOldName + L"*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    std::wstring name = found.cFileName;
    const size_t at = name.find(kOldName);
    if (at == std::wstring::npos) continue;
    const std::wstring from = dir + L"\\" + name;
    MoveFileExW(from.c_str(), (dir + L"\\" + name.replace(at, wcslen(kOldName), L"blox7")).c_str(), 0);
  } while (FindNextFileW(search, &found));
  FindClose(search);
}

void fix_approvals(const std::wstring& dir) {
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((dir + L"\\*.json").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  const std::string before = "/" + narrow(kOldName) + "/payloads/", after = "/blox7/payloads/";
  do {
    const std::wstring path = dir + L"\\" + found.cFileName;
    std::string text = read_file(path, 16 << 20);
    bool moved = false;
    for (size_t at; (at = text.find(before)) != std::string::npos; moved = true) text.replace(at, before.size(), after);
    if (!moved) continue;
    // evidence is read-only and has to stay that way, the runtime refuses writable approvals
    const DWORD attributes = GetFileAttributesW(path.c_str());
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    write_all(path, text);
    SetFileAttributesW(path.c_str(), attributes);
  } while (FindNextFileW(search, &found));
  FindClose(search);
}

void move_old_folders(const Paths& paths) {
  const std::wstring root = paths.data + L"\\root\\";
  auto move_over = [&](const wchar_t* base) {
    const std::wstring old = root + base + kOldName, fresh = root + base + L"blox7";
    return exists(old) && !exists(fresh) && MoveFileExW(old.c_str(), fresh.c_str(), 0);
  };
  move_over(L".config\\");
  move_over(L".cache\\");
  if (move_over(L".local\\share\\")) {
    const std::wstring data = root + L".local\\share\\blox7";
    fix_approvals(data + L"\\approvals");
    for (const wchar_t* dir : {L"", L"\\auth", L"\\android\\data"}) rename_old_files(data + dir);
  }
  if (move_over(L".local\\state\\")) rename_old_files(root + L".local\\state\\blox7\\support");
}

}  // namespace

std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
  return out;
}

bool write_all(const std::wstring& path, const std::string& text) {
  const std::wstring temp = path + L".blox7-new";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD done = 0;
  const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &done, nullptr) && done == text.size();
  CloseHandle(file);
  return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

const wchar_t* payload_id() { return BLOX7_PAYLOAD_ID; }

OsInfo os_info() {
  OsInfo info;
  RTL_OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof version;
  using RtlGetVersion = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
  auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (get && get(&version) == 0) {
    info.major = version.dwMajorVersion;
    info.minor = version.dwMinorVersion;
    info.build = version.dwBuildNumber;
  }
  return info;
}

Paths resolve_paths() {
  Paths paths;
  std::wstring exe(32768, L'\0');
  exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
  if (exe.rfind(L"\\\\?\\", 0) == 0 && exe.size() > 6 && exe[5] == L':') exe.erase(0, 4);
  paths.exe = exe;
  std::wstring base = env(L"LOCALAPPDATA");
  if (base.empty()) base = env(L"USERPROFILE") + L"\\AppData\\Local";
  paths.home = base + L"\\Blox7";
  make_dirs(paths.home);
  const auto settings = read_ini(paths.home + L"\\launcher.ini");
  paths.runtime = paths.home + L"\\runtime\\" + payload_id();
  const auto data = settings.find(L"data_dir");
  paths.data = data != settings.end() && !data->second.empty() ? data->second : paths.home + L"\\data";
  while (paths.data.size() > 3 && (paths.data.back() == L'\\' || paths.data.back() == L'/')) paths.data.pop_back();
  const auto webview = settings.find(L"webview2_runtime");
  if (webview != settings.end()) paths.webview2 = webview->second;
  move_old_folders(paths);
  return paths;
}

bool runtime_ready(const Paths& paths) { return exists(paths.runtime + L"\\" + kComplete); }

std::wstring install_runtime(const Paths& paths, const Progress& progress) {
  if (runtime_ready(paths)) return {};
  HRSRC resource = FindResourceW(nullptr, L"PAYLOAD", MAKEINTRESOURCEW(10));
  HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
  const void* cab = loaded ? LockResource(loaded) : nullptr;
  const DWORD cab_size = resource ? SizeofResource(nullptr, resource) : 0;
  if (!cab || !cab_size) return L"this Blox7.exe carries no runtime";

  HMODULE cabinet = LoadLibraryW(L"cabinet.dll");
  const auto create = reinterpret_cast<FdiCreate>(GetProcAddress(cabinet, "FDICreate"));
  const auto copy = reinterpret_cast<FdiCopy>(GetProcAddress(cabinet, "FDICopy"));
  const auto destroy = reinterpret_cast<FdiDestroy>(GetProcAddress(cabinet, "FDIDestroy"));
  if (!create || !copy || !destroy) return L"cabinet.dll has no FDI";

  wchar_t suffix[48];
  std::swprintf(suffix, 48, L".partial-%lu", GetCurrentProcessId());
  const std::wstring partial = paths.runtime + suffix;
  remove_tree(partial);
  make_dirs(partial);

  Extraction x;
  x.cab = static_cast<const uint8_t*>(cab);
  x.cab_size = cab_size;
  x.dir = partial;
  x.total = BLOX7_PAYLOAD_BYTES;
  x.progress = &progress;
  g_extraction = &x;
  ERF erf{};
  HFDI fdi = create(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write, fdi_close, fdi_seek, cpuUNKNOWN, &erf);
  char name[] = "payload.cab", folder[] = "";
  const BOOL copied = fdi && copy(fdi, name, folder, 0, fdi_notify, nullptr, nullptr);
  if (fdi) destroy(fdi);
  g_extraction = nullptr;
  if (!copied) {
    remove_tree(partial);
    return x.error.empty() ? L"the runtime could not be unpacked (cabinet error " + std::to_wstring(erf.erfOper) + L")"
                           : x.error;
  }
  std::wstring problem = check_file_list(partial);
  if (problem.empty()) {
    HANDLE marker = CreateFileW((partial + L"\\" + kComplete).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker == INVALID_HANDLE_VALUE) problem = L"cannot finish the runtime folder";
    else CloseHandle(marker);
  }
  if (!problem.empty()) {
    remove_tree(partial);
    return problem;
  }
  if (!MoveFileExW(partial.c_str(), paths.runtime.c_str(), 0)) {
    const DWORD error = GetLastError();
    remove_tree(partial);
    if (!runtime_ready(paths)) return L"cannot install the runtime (error " + std::to_wstring(error) + L")";
  }
  if (progress) progress(1.0);
  return {};
}

void prepare_data(const Paths& paths) {
  make_dirs(paths.data);
  copy_tree(paths.runtime + L"\\root", paths.data);
}

void hold_runtime(const Paths& paths) {
  CreateFileW((paths.runtime + L"\\" + kComplete).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
}

static bool runtime_busy(const std::wstring& dir) {
  HANDLE marker = CreateFileW((dir + L"\\" + kComplete).c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
  if (marker == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
  CloseHandle(marker);
  return false;
}

// old builds don't hold their runtime, so hands off while another Blox7.exe runs
static bool another_blox7_running() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return true;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  bool found = false;
  for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
    found = entry.th32ProcessID != GetCurrentProcessId() && _wcsicmp(entry.szExeFile, L"Blox7.exe") == 0;
  CloseHandle(snapshot);
  return found;
}

void remove_old_runtimes(const Paths& paths) {
  if (another_blox7_running()) return;
  const std::wstring dir = parent_of(paths.runtime);
  std::vector<std::wstring> old;
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring name = found.cFileName;
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L".." && name != payload_id() &&
        name.find(L".partial-") == std::wstring::npos)
      old.push_back(dir + L"\\" + name);
  } while (FindNextFileW(search, &found));
  FindClose(search);
  for (const std::wstring& runtime : old)
    if (!runtime_busy(runtime)) remove_tree(runtime);
}

void set_runtime_environment(const Paths& paths) {
  SetEnvironmentVariableW(L"BLOX7_ROOT", paths.data.c_str());
  for (const wchar_t* name : {L"HOME", L"XDG_CONFIG_HOME", L"XDG_DATA_HOME", L"XDG_CACHE_HOME", L"XDG_STATE_HOME",
                              L"XDG_RUNTIME_DIR"})
    SetEnvironmentVariableW(name, nullptr);
  if (!paths.webview2.empty()) SetEnvironmentVariableW(L"BLOX7_WEBVIEW2_RUNTIME", paths.webview2.c_str());
  const std::string icon = posix_path(paths.runtime + L"\\share\\blox7\\blox7.png");
  SetEnvironmentVariableW(L"BLOX7_WINDOW_ICON_FILE", icon.empty() ? nullptr : widen(icon).c_str());
}

std::string posix_path(const std::wstring& path) {
  std::wstring p = path;
  if (p.rfind(L"\\\\?\\", 0) == 0) p.erase(0, 4);
  if (p.size() < 3 || p[1] != L':' || (p[2] != L'\\' && p[2] != L'/')) return {};
  std::wstring out = L"/mnt/";
  out += static_cast<wchar_t>(towlower(p[0]));
  for (size_t i = 2; i < p.size(); ++i) out += p[i] == L'\\' ? L'/' : p[i];
  return narrow(out);
}

std::wstring roblox_version(const Paths& paths) {
  const std::string current = read_file(blox7_dir(paths, L".local\\share\\blox7\\current.json"), 1 << 20);
  std::wstring version = json_string(current, "version_name");
  if (version.empty()) {
    const std::string update = read_file(blox7_dir(paths, L".local\\share\\blox7\\update.json"), 1 << 20);
    const size_t at = update.find("\"current\"");
    if (at != std::string::npos) version = json_string(update.substr(at), "version_name");
  }
  return version;
}

std::wstring graphics_backend(const Paths& paths) {
  std::wstring section, backend;
  for (std::wstring line : lines_of(widen(read_file(blox7_dir(paths, L".config\\blox7\\config.yaml"), 1 << 20)))) {
    const size_t hash = line.find(L'#');
    if (hash != std::wstring::npos) line.erase(hash);
    if (trim(line).empty()) continue;
    if (line[0] != L' ' && line[0] != L'\t') {
      section = trim(line);
      continue;
    }
    if (section == L"graphics:") {
      const std::wstring item = trim(line);
      if (item.rfind(L"backend:", 0) == 0) backend = trim(item.substr(8));
    }
  }
  if (backend.size() >= 2 && (backend.front() == L'"' || backend.front() == L'\'')) backend = backend.substr(1, backend.size() - 2);
  if (backend.empty() || backend == L"direct-vulkan" || backend == L"vulkan" || backend == L"native-vulkan") return L"Vulkan";
  if (backend == L"opengl" || backend == L"gles" || backend == L"system") return L"OpenGL ES";
  if (backend == L"auto") return L"Auto renderer";
  return backend;
}

UpdateState update_state(const Paths& paths) {
  UpdateState state;
  const std::wstring path = blox7_dir(paths, L".local\\share\\blox7\\update.json");
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return state;
  state.written = info.ftLastWriteTime;
  const std::string json = read_file(path, 1 << 20);
  state.phase = json_string(json, "phase");
  state.message = json_string(json, "message");
  return state;
}

std::wstring logs_dir(const Paths& paths) { return blox7_dir(paths, L".local\\state\\blox7\\logs"); }

std::vector<std::wstring> log_tail(const Paths& paths, size_t count) {
  const std::wstring dir = logs_dir(paths) + L"\\sessions";
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((dir + L"\\*.log").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return {};
  std::wstring newest;
  FILETIME newest_time{};
  do {
    if (CompareFileTime(&found.ftLastWriteTime, &newest_time) > 0) {
      newest_time = found.ftLastWriteTime;
      newest = found.cFileName;
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);
  std::vector<std::wstring> lines;
  for (const std::wstring& line : lines_of(widen(read_file(dir + L"\\" + newest, 256 << 10)))) {
    std::wstring lower = line;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    if (trim(line).empty() || lower.find(L"password") != std::wstring::npos || lower.find(L"token") != std::wstring::npos ||
        lower.find(L"cookie") != std::wstring::npos || lower.find(L"roblosecurity") != std::wstring::npos ||
        lower.find(L"ticket") != std::wstring::npos)
      continue;
    lines.push_back(trim(line));
  }
  if (lines.size() > count) lines.erase(lines.begin(), lines.end() - static_cast<long>(count));
  return lines;
}

}  // namespace blox7
