#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <string>

#include "app.h"

namespace studio7 {

std::wstring widen(const char* text) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  std::wstring out(length > 0 ? static_cast<size_t>(length - 1) : 0, L'\0');
  if (length > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), length);
  return out;
}

bool windows_seven() {
  DWORD major = 0, minor = 0, build = 0;
  windows_version(&major, &minor, &build);
  return major == 6 && minor == 1;
}

void start_background() {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  self[MAX_PATH - 1] = 0;
  std::wstring command = L"\"" + std::wstring(self) + L"\" --background";
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS, nullptr, nullptr,
                      &startup, &process))
    return;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

bool self_updates(const Paths& paths) {
  char value[8];
  state_get(&paths, L"self", value, sizeof value);
  return strcmp(value, "off") != 0;
}

bool update_self(const Paths& paths) {
  wchar_t self[MAX_PATH];
  if (paths.custom || !windows_seven() || !self_updates(paths)) return false;
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (!length || length >= MAX_PATH) return false;
  UpdateInfo info;
  char error[400] = "";
  const int answer = update_check("Studio7.exe", self, &info, error, sizeof error);
  if (answer == UPDATE_FAILED) note("could not look for a newer Studio7: %s", error);
  if (answer != UPDATE_NEWER) return false;
  if (!update_install(&info, self, error, sizeof error)) {
    note("could not get the newer Studio7: %s", error);
    return false;
  }
  note("Studio7.exe replaced by the one published in %s (sha256 %.12s...)", info.tag, info.digest);
  return true;
}

bool launcher_open() {
  HANDLE launcher = OpenMutexW(SYNCHRONIZE, FALSE, S7_LAUNCHER);
  if (launcher) CloseHandle(launcher);
  return launcher != nullptr;
}

std::wstring start_failure(const Run& run) {
  wchar_t text[200];
  if (run.ran) std::swprintf(text, 200, L"It closed right after it started (code 0x%lX).", run.code);
  else std::swprintf(text, 200, L"Windows could not start Roblox Studio (error %lu).", run.code);
  return text;
}

}  // namespace studio7

namespace {

const wchar_t* after_program(const wchar_t* line) {
  bool quoted = false;
  for (; *line; ++line) {
    if (*line == L'"') quoted = !quoted;
    else if (!quoted && (*line == L' ' || *line == L'\t')) break;
  }
  while (*line == L' ' || *line == L'\t') ++line;
  return line;
}

bool open_now(const Paths& paths, const std::wstring& arguments, std::wstring* failure) {
  Run run;
  if (!run_start(&paths, arguments.c_str(), &run)) {
    if (run.code) *failure = studio7::start_failure(run);
    return false;
  }
  studio7::start_background();
  while (!run.trusted) {
    const int state = run_poll(&paths, &run);
    if (state == RUN_OVER || (state == RUN_STARTING && GetTickCount() - run.started > 180000)) break;
    if (state == RUN_FAILED || (state == RUN_AGAIN && !run_start(&paths, arguments.c_str(), &run))) {
      *failure = studio7::start_failure(run);
      return false;
    }
    Sleep(250);
  }
  run_forget(&run);
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && argc >= 2 && wcscmp(argv[1], WV2_SETUP_SWITCH) == 0)
    return wv2_setup_run(argc >= 3 && wcscmp(argv[2], L"--now") == 0);
  if (argv && argc == 3 && wcscmp(argv[1], L"--render-ui") == 0) return studio7::render_ui(argv[2]);
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (length && length < MAX_PATH) update_tidy(self);
  Paths paths;
  if (!paths_init(&paths)) {
    MessageBoxW(nullptr, L"Studio7 could not create its folder under AppData\\Local.", L"Studio7", MB_ICONERROR);
    return 1;
  }
  if (argv && argc == 2 && wcscmp(argv[1], L"--background") == 0) {
    if (!studio7::windows_seven()) return 0;
    links_refresh(&paths);
    studio_background(&paths);
    studio7::update_self(paths);
    return 0;
  }
  const bool webview_started =
      length && length < MAX_PATH && studio7::windows_seven() && webview_wanted() && wv2_setup_spawn(self, 0);
  std::wstring arguments, failure;
  bool open = false;
  if (argv && argc == 2 && is_link(argv[1])) {
    arguments = L"\"" + std::wstring(argv[1]) + L"\"";
    open = true;
  } else if (argv && argc >= 2 && wcscmp(argv[1], L"--open") == 0) {
    arguments = after_program(after_program(GetCommandLineW()));
    open = true;
  }
  const bool wait = open && webview_started && !studio_running(&paths) && !studio7::launcher_open();
  if (open && studio7::windows_seven() && !wait && open_now(paths, arguments, &failure)) return 0;
  return studio7::run_launcher(instance, show, paths, arguments, open && failure.empty(), failure, webview_started);
}
