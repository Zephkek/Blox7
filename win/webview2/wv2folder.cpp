#include "wv2setup.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kClients[] = L"\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

std::wstring env(const wchar_t* name) {
  std::wstring value(32768, L'\0');
  const DWORD size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
  value.resize(size < value.size() ? size : 0);
  return value;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool copy_out(const std::wstring& text, wchar_t* out, DWORD capacity) {
  if (out == nullptr || capacity == 0 || text.size() >= capacity) return false;
  wmemcpy(out, text.c_str(), text.size() + 1);
  return true;
}

std::wstring home() {
  std::wstring base = env(L"LOCALAPPDATA");
  if (base.empty()) {
    const std::wstring profile = env(L"USERPROFILE");
    if (profile.empty()) return {};
    base = profile + L"\\AppData\\Local";
  }
  while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
  return base + L"\\Blox7\\webview2";
}

std::wstring registered_version(HKEY root, const wchar_t* software, REGSAM view) {
  HKEY key = nullptr;
  const std::wstring path = std::wstring(software) + kClients;
  if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return {};
  wchar_t value[64] = {};
  DWORD size = sizeof value - sizeof(wchar_t);
  DWORD type = 0;
  const LONG read = RegQueryValueExW(key, L"pv", nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
  RegCloseKey(key);
  if (read != ERROR_SUCCESS || type != REG_SZ) return {};
  return value;
}

bool version_runs_here(const std::wstring& version) {
  const wchar_t* at = version.c_str();
  unsigned long major = 0;
  for (int i = 0; i < 4; ++i) {
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(at, &end, 10);
    if (end == at || *end != (i == 3 ? L'\0' : L'.')) return false;
    if (i == 0) major = value;
    at = end + 1;
  }
  if (major < 86) return false;
  return wv2_windows_major() >= 10 || major <= 109;
}

bool read_status(wv2_status* status) {
  ZeroMemory(status, sizeof *status);
  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, WV2_STATUS);
  if (mapping == nullptr) return false;
  if (const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof *status)) {
    const volatile wv2_status* shared = static_cast<const volatile wv2_status*>(view);
    status->state = shared->state;
    status->error = shared->error;
    status->win32 = shared->win32;
    status->pid = shared->pid;
    status->done = shared->done;
    status->total = shared->total;
    UnmapViewOfFile(view);
  }
  CloseHandle(mapping);
  return status->state != WV2_STATE_DONE && status->state != WV2_STATE_FAILED;
}

bool installed_at(const std::wstring& base, const std::wstring& version) {
  return !base.empty() && exists(base + L"\\Microsoft\\EdgeWebView\\Application\\" + version + L"\\msedgewebview2.exe");
}

}  // namespace

extern "C" {

int wv2_windows_major(void) { return static_cast<int>(*reinterpret_cast<const volatile ULONG*>(0x7FFE026C)); }

int wv2_home(wchar_t* out, DWORD capacity) {
  const std::wstring path = home();
  return !path.empty() && copy_out(path, out, capacity);
}

int wv2_folder(wchar_t* out, DWORD capacity) {
  const std::wstring path = home();
  return !path.empty() && copy_out(path + L"\\" + WV2_FOLDER, out, capacity);
}

int wv2_folder_usable(const wchar_t* folder) {
  if (folder == nullptr || *folder == 0) return 0;
  const std::wstring base = folder;
  return exists(base + L"\\msedgewebview2.exe") && exists(base + L"\\msedge.dll") &&
         exists(base + L"\\EBWebView\\x64\\EmbeddedBrowserWebView.dll");
}

int wv2_folder_complete(void) {
  const std::wstring base = home();
  if (base.empty()) return 0;
  const std::wstring folder = base + L"\\" + WV2_FOLDER;
  return exists(folder + L"\\" + WV2_MARKER) && wv2_folder_usable(folder.c_str());
}

int wv2_system_usable(void) {
  struct Place {
    HKEY root;
    const wchar_t* software;
    REGSAM view;
    const wchar_t* base;
  };
  const Place places[] = {{HKEY_LOCAL_MACHINE, L"SOFTWARE", KEY_WOW64_32KEY, L"ProgramFiles(x86)"},
                          {HKEY_LOCAL_MACHINE, L"SOFTWARE", KEY_WOW64_64KEY, L"ProgramFiles(x86)"},
                          {HKEY_CURRENT_USER, L"Software", 0, L"LOCALAPPDATA"}};
  for (const Place& place : places) {
    const std::wstring version = registered_version(place.root, place.software, place.view);
    if (version.empty() || !version_runs_here(version)) continue;
    if (installed_at(env(place.base), version) || installed_at(env(L"ProgramFiles"), version)) return 1;
  }
  return 0;
}

int wv2_needs_setup(void) { return wv2_windows_major() < 10 && !wv2_folder_complete() && !wv2_system_usable(); }

int wv2_setup_running(void) {
  wv2_status status;
  return read_status(&status);
}

int wv2_status_read(wv2_status* status) {
  if (status == nullptr) return 0;
  return read_status(status);
}

int wv2_last_error(DWORD* win32) {
  if (win32 != nullptr) *win32 = 0;
  const std::wstring base = home();
  if (base.empty()) return WV2_OK;
  FILE* file = _wfopen((base + L"\\setup-error.txt").c_str(), L"rb");
  if (file == nullptr) return WV2_OK;
  char line[64] = {};
  fread(line, 1, sizeof line - 1, file);
  fclose(file);
  char* end = nullptr;
  long code = strtol(line, &end, 10);
  if (end == line) code = WV2_ERROR_INTERNAL;
  const unsigned long error = strtoul(end, nullptr, 10);
  if (win32 != nullptr) *win32 = static_cast<DWORD>(error);
  return code < WV2_OK || code > WV2_ERROR_INTERNAL ? WV2_ERROR_INTERNAL : static_cast<int>(code);
}

const wchar_t* wv2_error_text(int error) {
  switch (error) {
    case WV2_OK:
      return L"it's ready";
    case WV2_ERROR_NETWORK:
      return L"Microsoft's server couldn't be reached";
    case WV2_ERROR_BLOCKED:
      return L"this PC blocks Microsoft's download servers";
    case WV2_ERROR_PROXY_AUTH:
      return L"the proxy wants a password";
    case WV2_ERROR_GONE:
      return L"Microsoft no longer serves the file";
    case WV2_ERROR_MISMATCH:
      return L"the download didn't match Microsoft's file";
    case WV2_ERROR_DISK_SPACE:
      return L"there isn't enough free disk space";
    case WV2_ERROR_NETWORK_DRIVE:
      return L"AppData is on a network drive";
    case WV2_ERROR_PATH_TOO_LONG:
      return L"the AppData path is too long";
    case WV2_ERROR_MEMORY:
      return L"there isn't enough free memory";
    case WV2_ERROR_BLOCKED_FILE:
      return L"Windows or an antivirus blocked the files";
    default:
      return L"something went wrong";
  }
}

int wv2_setup_spawn(const wchar_t* exe, int now) {
  if (wv2_setup_running()) {
    if (now) {
      if (HANDLE hurry = OpenEventW(EVENT_MODIFY_STATE, FALSE, WV2_HURRY)) {
        SetEvent(hurry);
        CloseHandle(hurry);
      }
    }
    return 1;
  }
  if (exe == nullptr || *exe == 0) return 0;
  std::wstring command = L"\"" + std::wstring(exe) + L"\" " + WV2_SETUP_SWITCH + (now ? L" --now" : L"");
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_FORCEOFFFEEDBACK;
  PROCESS_INFORMATION process{};
  const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (now ? 0 : BELOW_NORMAL_PRIORITY_CLASS);
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &process)) {
    return 0;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return 1;
}

}
