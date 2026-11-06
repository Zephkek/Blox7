#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <string>
#include <vector>

#include "../boot/boot.h"
#include "blox7.h"

// laptops read these off the exe and hand us the big gpu
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

// win11 throttles minimized windows and 1 ms sleeps turn into 15. no thanks
void opt_out_of_power_throttling() {
  struct {
    ULONG version, control_mask, state_mask;
  } state{1, 0x1 | 0x4, 0};
  using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, void*, DWORD);
  const auto set = reinterpret_cast<SetProcessInformationFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")));
  if (set) set(GetCurrentProcess(), 4, &state, sizeof state);
}

bool starts_with(const wchar_t* text, const wchar_t* prefix) { return _wcsnicmp(text, prefix, wcslen(prefix)) == 0; }

bool wants_play(int argc, wchar_t** argv) {
  if (argc < 2) return false;
  return wcscmp(argv[1], L"--play") == 0 || wcscmp(argv[1], L"--w32-run") == 0 || starts_with(argv[1], L"roblox:") ||
         starts_with(argv[1], L"roblox-player:");
}

int play(int argc, wchar_t** argv) {
  boot_set_product(L"Blox7");
  boot_prepare();
  opt_out_of_power_throttling();
  const blox7::Paths paths = blox7::resolve_paths();
  const std::wstring error = blox7::install_runtime(paths, nullptr);
  if (!error.empty()) boot_fail(error.c_str(), 0);
  blox7::hold_runtime(paths);
  blox7::prepare_data(paths);
  blox7::set_runtime_environment(paths);
  blox7::apply_launch_settings(paths);

  std::vector<std::wstring> args{paths.exe};
  if (wcscmp(argv[1], L"--w32-run") == 0) {
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  } else {
    const std::string program = blox7::posix_path(paths.runtime + L"\\bin\\blox7.so");
    if (program.empty()) boot_fail(L"the runtime folder is not on a drive letter", 0);
    args.emplace_back(L"--w32-run");
    args.emplace_back(blox7::widen(program));
    if (wcscmp(argv[1], L"--play") == 0) {
      for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);
    } else {
      args.emplace_back(argv[1]);  // links travel alone, a stray quote in one doesn't get to sneak options in
    }
  }
  std::vector<wchar_t*> raw;
  for (std::wstring& arg : args) raw.push_back(arg.data());
  raw.push_back(nullptr);
  const std::wstring bin = paths.runtime + L"\\bin\\";
  return boot_run(bin.c_str(), static_cast<int>(args.size()), raw.data());
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && wants_play(argc, argv)) return play(argc, argv);
  if (argv && argc == 3 && wcscmp(argv[1], L"--render-ui") == 0) return blox7::render_ui(argv[2]);
  return blox7::run_launcher(instance, show);
}
