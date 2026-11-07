#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "compat/elf_build_id.h"

#include "compat/host_abi_profile.h"
#include "compat/payload_compatibility.h"
#include "legacy/legacy_runtime.h"
#include "libc_shim/libc_shim.h"
#include "blox7/audio/fmod_jni_audio_bridge.h"
#include "blox7/audio/webrtc_jni_audio_bridge.h"
#include "blox7/audio/roblox_output_device_bridge.h"
#include "runtime/auth_runtime_composition.h"
#include "runtime/command_line.h"
#include "runtime/crash_report_policy.h"
#include "runtime/environment.h"
#include "runtime/external_launch_broker.h"
#include "runtime/failure_dialog.h"
#include "runtime/fleasion.h"
#include "runtime/game_mode.h"
#include "runtime/graphics_launch_policy.h"
#include "runtime/memory_limit.h"
#include "runtime/payload_update_preflight.h"
#include "runtime/performance_policy.h"
#include "runtime/platform_cache_migration.h"
#include "runtime/process_diagnostics.h"
#include "runtime/process_launch_policy.h"
#include "runtime/roblox_cookie_store.h"
#include "runtime/roblox_desktop_app_policy.h"
#include "runtime/roblox_experience_launch_bridge.h"
#include "runtime/roblox_fullscreen_runtime_bridge.h"
#include "runtime/roblox_user_agent.h"
#include "runtime/runtime_config_bootstrap.h"
#include "runtime/runtime_config_file.h"
#include "runtime/runtime_paths.h"
#include "runtime/session_log.h"
#include "runtime/single_instance_lock.h"
#include "runtime/support_bundle.h"
#include "runtime/supported_launch_policy.h"
#include "runtime/system_proxy.h"
#include "runtime/webview_helper_launcher.h"
#include "services/auth_service.h"
#include "services/browser_tracker_service.h"
#include "services/client_settings_service.h"
#include "services/http_client.h"
#include "window/window.h"

namespace {

struct AndroidWindowBridgeContext {};

class ExternalLaunchBrokerScope final {
 public:
  ~ExternalLaunchBrokerScope() { (void)Shutdown(); }

  std::shared_ptr<blox7::runtime::ExternalLaunchBroker>& broker() {
    return broker_;
  }

  blox7::Status Shutdown() {
    if (broker_ == nullptr) {
      return blox7::Status::Ok();
    }
    blox7::runtime::ClearActiveExternalLaunchBroker(broker_.get());
    blox7::Status status = broker_->Shutdown();
    broker_.reset();
    return status;
  }

 private:
  std::shared_ptr<blox7::runtime::ExternalLaunchBroker> broker_;
};

void SecureErase(char* data, std::size_t size) {
  volatile char* bytes = data;
  for (std::size_t index = 0; bytes != nullptr && index < size; ++index) {
    bytes[index] = '\0';
  }
}

void SecureEraseArguments(std::vector<std::string>* arguments) {
  if (arguments == nullptr) return;
  for (std::string& argument : *arguments) {
    SecureErase(argument.data(), argument.size());
  }
  arguments->clear();
}

bool QueueAndroidWindowFlags(void*, int flags, int mask) {
  return blox7::window::RequestFullscreenFromAndroidWindowFlags(flags, mask);
}

blox7::Status ShutdownPlatformBridges(jnivm::VM* vm) {
  if (vm == nullptr) {
    return blox7::Status::Error(blox7::StatusCode::kInvalidArgument,
                                   "platform bridge shutdown requires a VM");
  }
  vm->ClearAndroidWindowCallbacks();
  const blox7::Status voice_status =
      blox7::audio::ShutdownWebRtcJniAudioBridge(vm);
  const blox7::Status playback_status =
      blox7::audio::ShutdownFmodJniAudioBridge(vm);
  return !voice_status.ok() ? voice_status : playback_status;
}

void PromptFirstLaunchSignIn(
    const blox7::runtime::ProcessEnvironment& environment,
    const blox7::runtime::RuntimePaths& paths,
    blox7::services::AuthService& auth_service,
    const std::shared_ptr<blox7::services::HttpClient>& http_client,
    blox7::runtime::AuthRuntimeComposition* composition) {
  if (composition == nullptr ||
      composition->status != blox7::runtime::AuthRuntimeStatus::kGuest ||
      environment.Get("BLOX7_GUEST") == "1" ||
      environment.Get("BLOX7_SKIP_FIRST_LAUNCH_LOGIN") == "1") {
    return;
  }
  if (environment.Get("BLOX7_NATIVE_LOGIN") != "0") {
    std::cout << "  [auth] native sign-in selected; opening Roblox welcome screen\n";
    return;
  }

  std::filesystem::path helper;
  const char* helper_override = std::getenv("BLOX7_WEBVIEW_HELPER");
  if (helper_override != nullptr && helper_override[0] != '\0') {
    helper = helper_override;
  } else {
    std::error_code error;
    const std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !executable.empty()) {
      helper = executable.parent_path() / "blox7_webview_helper";
    }
  }
  std::error_code exists_error;
  if (helper.empty() || !std::filesystem::exists(helper, exists_error)) {
    return;
  }

  std::cout << "\n======================================================\n"
            << "  First-Time Setup: Roblox Sign-In\n"
            << "======================================================\n"
            << "  [auth] no saved session found; opening desktop sign-in window...\n"
            << "  [auth] tip: you can use Quick Log In (QR code) or username/password\n"
            << "  [auth] (close the sign-in window to play as guest)\n\n";

  struct FirstLaunchContext {
    std::mutex mutex;
    std::string captured_cookie;
    bool finished = false;
  };
  auto context = std::make_shared<FirstLaunchContext>();

  blox7::runtime::WebViewHelperExitObserver exit_observer;
  exit_observer.context = context;
  exit_observer.on_exit = [](void* ctx) {
    auto* c = static_cast<FirstLaunchContext*>(ctx);
    std::lock_guard<std::mutex> lock(c->mutex);
    c->finished = true;
  };

  const auto launched = blox7::runtime::LaunchWebViewHelper(
      helper, "https://www.roblox.com/login", exit_observer);
  if (!launched || launched.process == nullptr) {
    return;
  }
  if (!launched.process->WaitUntilReady(std::chrono::milliseconds(5000))) {
    std::cerr << "  [auth] sign-in window did not become ready\n";
    (void)launched.process->RequestClose();
    return;
  }
  (void)launched.process->SetRobloxCookie("");
  (void)launched.process->SetTitle("Roblox sign in");
  (void)launched.process->SetVisible(true);

  std::vector<blox7::runtime::WebViewHelperEvent> events;
  while (true) {
    {
      std::lock_guard<std::mutex> lock(context->mutex);
      if (context->finished) {
        break;
      }
    }
    if (launched.process->DrainEvents(&events)) {
      for (const auto& event : events) {
        if (event.type ==
            blox7::runtime::WebViewHelperEventType::kRobloxCookie) {
          std::lock_guard<std::mutex> lock(context->mutex);
          context->captured_cookie = event.payload;
          (void)launched.process->RequestClose();
          break;
        }
      }
      events.clear();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  if (!context->captured_cookie.empty()) {
    if (blox7::runtime::PersistRobloxCookie(paths.cookie_file(),
                                               context->captured_cookie)) {
      blox7::runtime::AuthRuntimeComposition new_comp =
          blox7::runtime::ComposeAuthRuntime(environment, paths,
                                                auth_service, http_client);
      if (new_comp.status ==
          blox7::runtime::AuthRuntimeStatus::kAuthenticated) {
        *composition = std::move(new_comp);
        std::cout << "  [auth] desktop sign-in successful; launching authenticated session\n";
      }
    }
  } else {
    std::cout << "  [auth] desktop sign-in window closed; continuing as guest\n";
  }
}

void ConfigureHostDriverEnvironment() {
  auto set_if_unset = [](const char* name, const char* value) {
    if (std::getenv(name) == nullptr) {
      setenv(name, value, 1);
    }
  };
  set_if_unset("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "1");
  set_if_unset("__GL_VRR_ALLOWED", "1");
  set_if_unset("__GL_SHADER_DISK_CACHE", "1");
  set_if_unset("__GL_SHADER_DISK_CACHE_SIZE", "2147483648");
  set_if_unset("__GL_YIELD", "USLEEP");
  set_if_unset("__GL_THREADED_OPTIMIZATIONS", "1");
}

}  // namespace

int main(int argc, char* argv[]) {
  ConfigureHostDriverEnvironment();
  const auto process_started_at = std::chrono::system_clock::now();
  blox7::runtime::CommandLineParseResult command_line =
      blox7::runtime::ParseCommandLine(argc, argv);
  if (!command_line) {
    std::cerr << command_line.error << "\n\n"
              << blox7::runtime::CommandLineUsage(
                     command_line.options.program_name);
    return EXIT_FAILURE;
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kHelp) {
    std::cout << blox7::runtime::CommandLineUsage(
        command_line.options.program_name);
    return EXIT_SUCCESS;
  }
  const std::string process_launch_diagnostics =
      command_line.options.mode == blox7::runtime::CommandMode::kRun
          ? blox7::runtime::ApplyInteractiveProcessLaunchPolicy()
          : std::string{};
  std::string command_line_error;
  if (!blox7::runtime::ApplySupportedLaunchPolicy(
          command_line.options.mode == blox7::runtime::CommandMode::kRun,
          &command_line_error)) {
    std::cerr << "[FATAL] " << command_line_error << '\n';
    return EXIT_FAILURE;
  }
  if (!blox7::runtime::ApplyCommandLineEnvironment(command_line.options,
                                                      &command_line_error)) {
    std::cerr << command_line_error << '\n';
    return EXIT_FAILURE;
  }

  const blox7::runtime::ProcessEnvironment environment;
  blox7::runtime::SetRobloxCookieWarningHandler(
      [](std::string_view message) {
        (void)blox7::runtime::ShowWarningDialog(
            blox7::runtime::ProcessEnvironment(), message);
      });
  const std::string built_in_compatibility_manifest = environment.GetOr(
      "BLOX7_UPDATE_COMPATIBILITY_PATH",
      environment.GetOr("BLOX7_COMPATIBILITY_MANIFEST",
                        BLOX7_DEFAULT_COMPATIBILITY_MANIFEST));
  const blox7::runtime::RuntimePaths paths =
      blox7::runtime::RuntimePaths::FromEnvironment(environment);
  if (command_line.options.force_run_latest) {
    std::cerr << "[runtime] WARNING: latest Roblox will run once without "
                 "approval and will not become the active payload\n";
    const blox7::runtime::PayloadUpdatePreflightResult force_latest =
        blox7::runtime::RunPayloadUpdatePreflight(environment, paths, true);
    if (!force_latest) {
      std::cerr << "[FATAL] " << force_latest.error;
      if (!force_latest.details.empty()) {
        std::cerr << ": " << force_latest.details;
      }
      std::cerr << '\n';
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  std::optional<blox7::runtime::RobloxExperienceLaunchRequest>
      external_launch_request;
  if (!command_line.options.launch_request_json.empty()) {
    external_launch_request.emplace();
    const blox7::Status launch_status =
        blox7::runtime::ParseRobloxExperienceLaunchJson(
            command_line.options.launch_request_json,
            &*external_launch_request);
    if (!launch_status.ok()) {
      std::cerr << "[FATAL] Invalid controlled website launch request\n";
      return EXIT_FAILURE;
    }
  }
  std::vector<std::string> cgroup_reexec_arguments;
  if (!blox7::runtime::BuildCommandLineReexecArguments(
          command_line.options, argc, argv, &cgroup_reexec_arguments,
          &command_line_error)) {
    std::cerr << "[FATAL] " << command_line_error << '\n';
    return EXIT_FAILURE;
  }
  blox7::runtime::ScrubCommandLineLaunchArguments(&command_line.options,
                                                     argc, argv);
  blox7::runtime::SessionLog session_log;
  blox7::runtime::FailureSupportBundleGuard support_bundle_guard(
      environment, paths,
      command_line.options.mode == blox7::runtime::CommandMode::kRun);
  std::optional<blox7::runtime::SingleInstanceLock> instance_lock;
  ExternalLaunchBrokerScope external_launch_broker;
  blox7::runtime::ExternalLaunchBrokerOptions broker_options;
  const bool isolated_canary =
      environment.GetOr("BLOX7_ISOLATED_CANARY", "0") == "1";
  if (isolated_canary && external_launch_request.has_value()) {
    std::cerr << "[FATAL] Website launches are unavailable inside isolated "
                 "canary\n";
    return EXIT_FAILURE;
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    instance_lock.emplace(
        blox7::runtime::SingleInstanceLock::AcquireForLaunch(environment,
                                                                paths));
    if (!instance_lock->acquired()) {
      if (!isolated_canary && instance_lock->already_running() &&
          external_launch_request.has_value()) {
        const blox7::Status forward_status =
            blox7::runtime::ExternalLaunchBroker::ForwardToOwner(
                broker_options, *external_launch_request);
        if (forward_status.ok()) {
          std::cout << "  [launch] sent website join to running Blox7\n";
          support_bundle_guard.SetExitCode(EXIT_SUCCESS);
          return EXIT_SUCCESS;
        }
        std::cerr << "[FATAL] Cannot send website join to running Blox7: "
                  << forward_status.message() << '\n';
        (void)blox7::runtime::ShowFailureDialog(
            environment,
            "Blox7 is already running, but the requested experience "
            "could not be sent to it.");
      } else if (instance_lock->already_running()) {
        std::cerr << "[FATAL] Blox7 is already running for this user\n";
        (void)blox7::runtime::ShowFailureDialog(
            environment, "An instance of Blox7 is already running.");
      } else {
        std::cerr << "[FATAL] " << instance_lock->error() << '\n';
        (void)blox7::runtime::ShowFailureDialog(
            environment, "Blox7 could not acquire its launch lock.");
      }
      return EXIT_FAILURE;
    }
  }
  const blox7::runtime::RuntimeConfigBootstrapResult config_bootstrap =
      blox7::runtime::EnsureRuntimeConfigFile(paths.config_file());
  if (!config_bootstrap) {
    std::cerr << "[FATAL] Cannot prepare " << paths.config_file() << ": "
              << config_bootstrap.error << '\n';
    if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
      (void)blox7::runtime::ShowFailureDialog(
          environment, "Blox7 could not prepare its configuration.");
    }
    return EXIT_FAILURE;
  }
  blox7::runtime::RuntimeConfigLoadResult runtime_config =
      blox7::runtime::LoadRuntimeConfig(environment, paths.config_file());
  if (!runtime_config) {
    std::cerr << "[FATAL] Cannot load " << paths.config_file() << ": "
              << runtime_config.error << '\n';
    if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
      (void)blox7::runtime::ShowFailureDialog(
          environment, "Blox7 could not load its configuration.");
    }
    return EXIT_FAILURE;
  }
  if (runtime_config.config.use_system_proxy()) {
    const blox7::runtime::SystemProxyResult system_proxy =
        blox7::runtime::ResolveSystemProxy();
    const std::string proxy_host = system_proxy.proxy.has_value()
                                       ? system_proxy.proxy->host
                                       : std::string();
    const std::string proxy_port =
        system_proxy.proxy.has_value()
            ? std::to_string(system_proxy.proxy->port)
            : std::string();
    const std::string proxy_scheme = system_proxy.proxy.has_value()
                                         ? system_proxy.proxy->scheme
                                         : std::string();
    if (!system_proxy ||
        setenv("BLOX7_HTTP_PROXY_HOST", proxy_host.c_str(), 1) != 0 ||
        setenv("BLOX7_HTTP_PROXY_PORT", proxy_port.c_str(), 1) != 0 ||
        setenv("BLOX7_HTTP_PROXY_SCHEME", proxy_scheme.c_str(), 1) != 0 ||
        setenv("BLOX7_HTTP_PROXY_USERNAME", "", 1) != 0 ||
        setenv("BLOX7_HTTP_PROXY_PASSWORD", "", 1) != 0) {
      std::cerr << "[FATAL] Cannot resolve host system proxy";
      if (!system_proxy.error.empty()) {
        std::cerr << ": " << system_proxy.error;
      }
      std::cerr << '\n';
      return EXIT_FAILURE;
    }
    runtime_config = blox7::runtime::LoadRuntimeConfig(
        environment, paths.config_file());
    if (!runtime_config) {
      std::cerr << "[FATAL] Cannot apply host system proxy: "
                << runtime_config.error << '\n';
      return EXIT_FAILURE;
    }
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun &&
      runtime_config.config.has_unsafe_detached_thread_overrides()) {
    std::cerr << "[FATAL] Unsupported detached legacy thread overrides:\n";
    for (const std::string& name :
         runtime_config.config.unsafe_detached_thread_overrides()) {
      std::cerr << "  - " << name << '\n';
    }
    std::cerr << "  Supported runtime requires synchronous or owned worker "
                 "execution.\n";
    (void)blox7::runtime::ShowFailureDialog(
        environment,
        "Blox7 cannot start with unsupported legacy thread "
        "overrides enabled.");
    return EXIT_FAILURE;
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun &&
      !blox7::runtime::ApplyGraphicsLaunchPolicy(runtime_config.config,
                                                     &command_line_error)) {
    std::cerr << "[FATAL] " << command_line_error << '\n';
    return EXIT_FAILURE;
  }
  blox7::runtime::FailureDialogMonitor failure_dialog;
  blox7::runtime::MemoryLimitWatchdog memory_limit_watchdog;
  const bool use_memory_limit =
      command_line.options.mode == blox7::runtime::CommandMode::kRun &&
      runtime_config.config.performance().memory_limit_enabled();
  const std::uint64_t memory_limit_bytes =
      use_memory_limit
          ? runtime_config.config.performance().memory_limit_bytes()
          : 0;
  blox7::runtime::CgroupMemoryLimitResult cgroup_limit;
  if (use_memory_limit) {
    cgroup_limit = blox7::runtime::MaybeReexecWithCgroupMemoryLimit(
        argc, argv, memory_limit_bytes, &cgroup_reexec_arguments);
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    session_log = blox7::runtime::SessionLog::Start(
        environment, paths, process_started_at);
    if (session_log) {
      std::cout << session_log.Header(
                       environment, paths,
                       runtime_config.config.graphics_backend_name())
                << std::flush;
    } else if (session_log.attempted()) {
      std::cerr << "  [session] automatic logging unavailable: "
                << session_log.error() << '\n';
    }
    std::cout << process_launch_diagnostics << std::flush;
    blox7::runtime::InstallCpuLimitDiagnostics();
    blox7::runtime::LogProcessDiagnostics(
        blox7::runtime::ProcessDiagnosticStage::kStartup);
  }
  if (runtime_config.config.fleasion_enabled()) {
    const auto fleasion = blox7::runtime::PrepareFleasion(
        runtime_config.config, environment, paths);
    if (!fleasion ||
        setenv("BLOX7_CA_BUNDLE", fleasion.bundle.c_str(), 1) != 0 ||
        setenv("BLOX7_FLEASION_BASE_CA_BUNDLE", fleasion.base_bundle.c_str(), 1) != 0 ||
        setenv("BLOX7_FLEASION_GENERATED_BUNDLE", fleasion.bundle.c_str(), 1) != 0 ||
        setenv("BLOX7_FLEASION_CA_CERTIFICATE", fleasion.certificate.c_str(), 1) != 0) {
      std::cerr << "[FATAL] Cannot prepare Fleasion: "
                << (!fleasion ? fleasion.error : "cannot export certificate paths") << '\n';
      return EXIT_FAILURE;
    }
    if (const auto& proxy = runtime_config.config.network_proxy(); proxy &&
        (setenv("BLOX7_HTTP_PROXY_HOST", proxy->host.c_str(), 1) != 0 ||
         setenv("BLOX7_HTTP_PROXY_PORT", std::to_string(proxy->port).c_str(), 1) != 0 ||
         setenv("BLOX7_HTTP_PROXY_SCHEME", proxy->scheme.c_str(), 1) != 0)) {
      std::cerr << "[FATAL] Cannot export Fleasion proxy\n";
      return EXIT_FAILURE;
    }
    runtime_config = blox7::runtime::LoadRuntimeConfig(environment, paths.config_file());
    if (!runtime_config) {
      std::cerr << "[FATAL] Cannot apply Fleasion: " << runtime_config.error << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "  [fleasion] mode=" << runtime_config.config.fleasion_proxy_mode()
              << " certificate=" << fleasion.certificate
              << " trust_bundle=" << fleasion.bundle << '\n';
    if (runtime_config.config.network_proxy()) {
      std::cout << "  [fleasion] proxy=" << blox7::runtime::BuildNetworkProxyUrl(
          *runtime_config.config.network_proxy()) << "; start Fleasion before Roblox\n";
    }
  }
  if (!blox7::runtime::ExportNetworkProxyEnvironment(
          runtime_config.config, &command_line_error)) {
    std::cerr << "[FATAL] " << command_line_error << '\n';
    return EXIT_FAILURE;
  }
  if (config_bootstrap.created()) {
    std::cout << "  [runtime] created first-run configuration: "
              << paths.config_file() << '\n';
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun &&
      runtime_config.config.use_system_proxy()) {
    if (runtime_config.config.network_proxy().has_value()) {
      std::cout << "  [network] system proxy="
                << blox7::runtime::BuildNetworkProxyUrl(
                       *runtime_config.config.network_proxy())
                << '\n';
    } else {
      std::cout << "  [network] system proxy=direct\n";
    }
  }
  if (use_memory_limit) {
    if (cgroup_limit.active()) {
      std::cout << "  [memory] hard process-tree limit active: "
                << runtime_config.config.performance().memory_limit_mb
                << " MiB RAM, swap disabled\n";
    }
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    std::cout << "  [runtime] graphics backend="
              << runtime_config.config.graphics_backend_name()
              << (runtime_config.config.graphics_backend() ==
                          blox7::runtime::GraphicsBackend::kSystem
                      ? " (EGL/OpenGL ES)"
                      : "")
              << '\n';
  }

  const blox7::runtime::GameModePolicy game_mode_policy =
      runtime_config.config.performance().game_mode;
  blox7::runtime::GameModeSession game_mode_session =
      command_line.options.mode == blox7::runtime::CommandMode::kRun
          ? blox7::runtime::GameModeSession::Start(game_mode_policy)
          : blox7::runtime::GameModeSession();
  switch (game_mode_session.state()) {
    case blox7::runtime::GameModeSessionState::kActive:
      std::cout << "  [gamemode] performance request active\n";
      break;
    case blox7::runtime::GameModeSessionState::kAlreadyActive:
      std::cout << "  [gamemode] already active for this process\n";
      break;
    case blox7::runtime::GameModeSessionState::kUnavailable:
    case blox7::runtime::GameModeSessionState::kRequestFailed:
      if (game_mode_policy == blox7::runtime::GameModePolicy::kOn) {
        std::cerr << "  [gamemode] requested but unavailable: "
                  << game_mode_session.detail()
                  << "; continuing without GameMode\n";
      } else {
        std::cout << "  [gamemode] unavailable; continuing normally\n";
      }
      break;
    case blox7::runtime::GameModeSessionState::kDisabled:
      if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
        std::cout << "  [gamemode] disabled by runtime policy\n";
      }
      break;
    case blox7::runtime::GameModeSessionState::kStopped:
    case blox7::runtime::GameModeSessionState::kStopFailed:
      break;
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    failure_dialog = blox7::runtime::FailureDialogMonitor::Start(
        environment,
        "Blox7 could not start Roblox because of an internal error.");
  }
  if (use_memory_limit) {
    if (!memory_limit_watchdog.Start(memory_limit_bytes, &command_line_error)) {
      failure_dialog.SetMessage(
          "Blox7 could not start its memory safety monitor.");
      std::cerr << "[FATAL] " << command_line_error << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "  [memory] RSS+swap watchdog active at "
              << runtime_config.config.performance().memory_limit_mb
              << " MiB\n";
  }
  SecureEraseArguments(&cgroup_reexec_arguments);
  const bool uses_managed_payload =
      !environment.HasNonEmpty("ROBLOX_LIB_PATH") &&
      runtime_config.config.roblox_library_path() ==
          std::filesystem::path("rbx_bin/libroblox.so");
  if (!uses_managed_payload &&
      !environment.HasNonEmpty("BLOX7_ASSET_PATH")) {
    const std::filesystem::path adjacent_assets =
        blox7::runtime::ResolveAdjacentRobloxAssetPath(
            runtime_config.config.roblox_library_path(),
            paths.working_directory());
    if (adjacent_assets.empty() ||
        setenv("BLOX7_ASSET_PATH", adjacent_assets.c_str(), 1) != 0) {
      std::cerr << "[FATAL] Cannot bind assets to explicit Roblox library\n";
      return EXIT_FAILURE;
    }
    std::cout << "  [runtime] explicit Roblox library uses adjacent assets: "
              << adjacent_assets << '\n';
  }
  blox7::runtime::BackgroundPayloadUpdate background_update;
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun &&
      uses_managed_payload) {
    if (setenv("BLOX7_GRAPHICS_BACKEND",
               runtime_config.config.graphics_backend_name().c_str(), 1) != 0) {
      std::cerr << "[FATAL] Cannot configure the Roblox update graphics backend\n";
      return EXIT_FAILURE;
    }
    failure_dialog.SetMessage(
        "Blox7 could not update or verify the Roblox installation.");
    const blox7::runtime::PayloadUpdatePreflightResult update_preflight =
        blox7::runtime::RunPayloadUpdatePreflight(environment, paths);
    if (!update_preflight) {
      std::cerr << "[FATAL] " << update_preflight.error;
      if (!update_preflight.details.empty()) {
        std::cerr << ": " << update_preflight.details;
      }
      failure_dialog.SetMessage(
          "Blox7 could not update or verify the Roblox installation.\n\n" +
          (update_preflight.details.empty() ? update_preflight.error
                                           : update_preflight.details) +
          "\n\nSession log: " + (paths.logs_root() / "sessions").string());
      std::cerr << '\n';
      return EXIT_FAILURE;
    }
    failure_dialog.SetMessage(
        "Blox7 could not finish starting Roblox because of an internal "
        "error.");
    background_update.Start(environment, paths);
  }
  if (!blox7::runtime::ExportRuntimePathEnvironment(paths,
                                                       &command_line_error)) {
    std::cerr << "[FATAL] " << command_line_error << '\n';
    return EXIT_FAILURE;
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    const blox7::Status window_state_status =
        blox7::window::ConfigureWindowStatePersistence(paths.state_root() /
                                                          "window-state.json");
    if (!window_state_status.ok()) {
      std::cerr << "[FATAL] Cannot configure window state persistence: "
                << window_state_status.message() << '\n';
      return EXIT_FAILURE;
    }
  }
  std::filesystem::path app_storage_file;
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    const blox7::runtime::InputCapabilityConfig& input =
        runtime_config.config.input_capabilities();
    const std::string platform_profile =
        blox7::runtime::BuildPlatformProfileRevision(
            runtime_config.config.device_profile().cache_key,
            input.touch_enabled, input.mouse_enabled, input.keyboard_enabled);
    const blox7::runtime::PlatformCacheMigrationResult cache_migration =
        blox7::runtime::MigratePlatformProfileCaches(environment, paths,
                                                        platform_profile);
    if (!cache_migration) {
      std::cerr << "[FATAL] Platform cache migration failed: "
                << cache_migration.error << '\n';
      return EXIT_FAILURE;
    }
    app_storage_file = cache_migration.app_storage_file;
    setenv("BLOX7_APP_STORAGE_FILE_INTERNAL", app_storage_file.c_str(), 1);
    if (cache_migration.transitioned) {
      std::cout << "  [runtime] platform cache profile transitioned; "
                << "refreshable identity and policy caches invalidated="
                << (cache_migration.app_storage_updated ? 1 : 0) << '\n';
    }
  }
  const bool needs_active_library =
      !environment.HasNonEmpty("ROBLOX_LIB_PATH") &&
      runtime_config.config.roblox_library_path() ==
          std::filesystem::path("rbx_bin/libroblox.so");
  const bool needs_active_assets =
      !environment.HasNonEmpty("BLOX7_ASSET_PATH");
  if (needs_active_library || needs_active_assets) {
    const blox7::runtime::ActivePayloadPaths active =
        paths.ResolveActivePayload();
    if (!active) {
      std::cerr << "[FATAL] Cannot resolve " << paths.active_payload_manifest()
                << ": " << active.error << '\n';
      return EXIT_FAILURE;
    }
    if (active.active) {
      if (uses_managed_payload &&
          !blox7::runtime::PrepareManagedPayloadWorkingDirectory(
              paths, active, &command_line_error)) {
        std::cerr << "[FATAL] " << command_line_error << '\n';
        return EXIT_FAILURE;
      }
      {
        std::ifstream manifest(active.root / "roblox_payload.json");
        const std::string text((std::istreambuf_iterator<char>(manifest)),
                               std::istreambuf_iterator<char>());
        const std::size_t key = text.find("\"version_name\"");
        const std::size_t open =
            key == std::string::npos ? key : text.find('"', text.find(':', key) + 1);
        const std::size_t close =
            open == std::string::npos ? open : text.find('"', open + 1);
        if (close != std::string::npos && close > open + 1)
          setenv("BLOX7_ROBLOX_VERSION",
                 text.substr(open + 1, close - open - 1).c_str(), 1);
      }
      if ((needs_active_library &&
           setenv("ROBLOX_LIB_PATH", active.roblox_library.c_str(), 1) != 0) ||
          (needs_active_assets &&
           setenv("BLOX7_ASSET_PATH", active.assets_content.c_str(), 1) !=
               0)) {
        std::cerr << "[FATAL] Cannot activate resolved Roblox payload\n";
        return EXIT_FAILURE;
      }
      if (uses_managed_payload) {
        bool use_external_profile = !active.compatibility_manifest.empty();
        if (use_external_profile) {
          const blox7::compat::BuildIdResult active_build_id =
              blox7::compat::ReadElfBuildId(active.roblox_library.string());
          if (!active_build_id) {
            std::cerr << "[FATAL] Cannot identify active Roblox payload: "
                      << active_build_id.error << '\n';
            return EXIT_FAILURE;
          }
          use_external_profile = blox7::compat::FindHostAbiProfile(
                                     active_build_id.build_id) == nullptr;
        }
        if (use_external_profile) {
          if (setenv("BLOX7_COMPATIBILITY_MANIFEST",
                     active.compatibility_manifest.c_str(), 1) != 0 ||
              setenv("BLOX7_HOST_ABI_PROFILE_FILE",
                     active.host_abi_profile.c_str(), 1) != 0 ||
              setenv("BLOX7_HOST_ABI_APPROVAL_RECEIPT",
                     active.host_abi_approval_receipt.c_str(), 1) != 0) {
            std::cerr
                << "[FATAL] Cannot activate approved Roblox ABI profile\n";
            return EXIT_FAILURE;
          }
        } else if (setenv("BLOX7_COMPATIBILITY_MANIFEST",
                          built_in_compatibility_manifest.c_str(), 1) != 0 ||
                   unsetenv("BLOX7_HOST_ABI_PROFILE_FILE") != 0 ||
                   unsetenv("BLOX7_HOST_ABI_APPROVAL_RECEIPT") != 0) {
          std::cerr << "[FATAL] Cannot activate built-in Roblox ABI profile\n";
          return EXIT_FAILURE;
        }
      }
      runtime_config = blox7::runtime::LoadRuntimeConfig(
          environment, paths.config_file());
      if (!runtime_config) {
        std::cerr << "[FATAL] Cannot resolve runtime configuration: "
                  << runtime_config.error << '\n';
        return EXIT_FAILURE;
      }
    }
  }
  if (!blox7::runtime::ExportRuntimeConfigEnvironment(runtime_config.config,
                                                         &command_line_error)) {
    std::cerr << command_line_error << '\n';
    return EXIT_FAILURE;
  }
  if (const char* user_agent = std::getenv("BLOX7_USER_AGENT");
      user_agent != nullptr &&
      std::string_view(user_agent).find(
          blox7::runtime::kRobloxVersionPlaceholder) != std::string_view::npos) {
    const std::string version = blox7::runtime::ActiveRobloxVersion();
    if (version.empty()) unsetenv("BLOX7_USER_AGENT");
    else setenv("BLOX7_USER_AGENT",
                blox7::runtime::FillRobloxVersion(user_agent, version).c_str(), 1);
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun &&
      !environment.HasNonEmpty("BLOX7_NATIVE_SET_DEFAULT_POLICY_FILE") &&
      setenv("BLOX7_NATIVE_SET_DEFAULT_POLICY_FILE", "1", 1) != 0) {
    std::cerr << "[FATAL] Cannot enable Roblox default app-policy file\n";
    return EXIT_FAILURE;
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    std::cout << "  [runtime] device profile="
              << runtime_config.config.device_profile().name << " class="
              << blox7::runtime::DeviceClassName(
                     runtime_config.config.device_profile().device_class)
              << " model=\""
              << runtime_config.config.device_profile().display_name
              << "\" touch="
              << runtime_config.config.input_capabilities().touch_enabled
              << " mouse="
              << runtime_config.config.input_capabilities().mouse_enabled
              << " keyboard="
              << runtime_config.config.input_capabilities().keyboard_enabled
              << '\n';
    if (const char* user_agent = std::getenv("BLOX7_USER_AGENT")) {
      std::cout << "  [runtime] user agent=" << user_agent << '\n';
    }
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    const std::filesystem::path fflags_path =
        paths.config_root() / "fflags.json";
    const auto fflags = blox7::services::LoadAndMergeFflagsFile(
        fflags_path,
        environment.GetOr("BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON", "{}"));
    if (!fflags.error.empty()) {
      std::cerr << "[FATAL] Cannot load FFlag overrides from " << fflags_path
                << ": " << fflags.error << '\n';
      return EXIT_FAILURE;
    }
    if (fflags.loaded) {
      std::cout << "  [runtime] loaded " << fflags.count
                << " FFlag overrides from " << fflags_path << '\n';
    }
    if (const char* quality = std::getenv("BLOX7_GRAPHICS_QUALITY")) {
      std::cout << "  [runtime] graphics quality=" << quality
                << (std::string_view(quality) == "high" ? " (memory savers off)" : "")
                << '\n';
    }
    std::string client_settings_overrides;
    if (!blox7::runtime::MergeRuntimeClientSettingsOverrides(
            runtime_config.config.frame_rate(),
            runtime_config.config.performance(), fflags.json,
            &client_settings_overrides, &command_line_error)) {
      std::cerr << "[FATAL] Cannot apply runtime client-settings policy: "
                << command_line_error << '\n';
      return EXIT_FAILURE;
    }
    std::string audio_capture_overrides;
    if (!blox7::runtime::MergeAudioCaptureClientSettingsOverrides(
            runtime_config.config.microphone_enabled(),
            client_settings_overrides, &audio_capture_overrides,
            &command_line_error)) {
      std::cerr << "[FATAL] Cannot apply microphone permission policy: "
                << command_line_error << '\n';
      return EXIT_FAILURE;
    }
    if (setenv("BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON",
               audio_capture_overrides.c_str(), 1) != 0) {
      std::cerr << "[FATAL] Cannot export runtime client-settings policy\n";
      return EXIT_FAILURE;
    }
    std::cout << "  [runtime] HttpClient compatibility mode enabled: "
                 "generic LuaApp retry and RuntimeMutexRv disabled\n";
    std::string fast_flags_overrides;
    if (!blox7::runtime::MergeCrashReportFastFlagsOverrides(
            environment.GetOr("BLOX7_FAST_FLAGS_JSON", "{}"),
            &fast_flags_overrides, &command_line_error)) {
      std::cerr << "[FATAL] Cannot apply crash-report fast-flags policy: "
                << command_line_error << '\n';
      return EXIT_FAILURE;
    }
    if (setenv("BLOX7_FAST_FLAGS_JSON", fast_flags_overrides.c_str(), 1) !=
        0) {
      std::cerr << "[FATAL] Cannot export crash-report fast-flags policy\n";
      return EXIT_FAILURE;
    }
    std::cout << "  [runtime] Roblox crash-report uploads disabled by "
                 "mandatory policy\n";
    const auto physics_worker_mode =
        runtime_config.config.performance().physics_worker_mode;
    const int physical_core_count =
        runtime_config.config.performance().physical_core_count;
    const int engine_worker_count =
        physics_worker_mode == blox7::runtime::PhysicsWorkerMode::kThroughput
            ? blox7::runtime::CalculateThroughputWorkerCount(
                  physical_core_count)
            : physical_core_count;
    if (physics_worker_mode == blox7::runtime::PhysicsWorkerMode::kLatency) {
      std::cout << "  [runtime] physics worker mode=latency: Roblox-managed "
                   "worker pools, midphase batch=128\n";
    } else if (physics_worker_mode ==
               blox7::runtime::PhysicsWorkerMode::kThroughput) {
      std::cout << "  [runtime] physics worker mode=throughput: "
                << engine_worker_count << " scheduler workers, async minimum="
                << std::min(engine_worker_count, 3) << ", midphase batch=128\n";
    }
    if ((runtime_config.config.performance().multithreaded_rendering ||
         physics_worker_mode ==
             blox7::runtime::PhysicsWorkerMode::kThroughput) &&
        physics_worker_mode != blox7::runtime::PhysicsWorkerMode::kLatency) {
      std::cout << "  [runtime] full multithreaded engine queues enabled: "
                << engine_worker_count << " scheduler workers, "
                << std::max(1, engine_worker_count / 2)
                << " occlusion workers\n";
    } else if (runtime_config.config.performance().multithreaded_rendering &&
               physics_worker_mode ==
                   blox7::runtime::PhysicsWorkerMode::kLatency) {
      std::cout << "  [runtime] latency mode leaves scheduler/render worker "
                   "counts under Roblox client-settings control\n";
    }
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun &&
      runtime_config.config.frame_rate().mode ==
          blox7::runtime::FrameRateLimitMode::kUnlimited) {
    std::cout << "  [runtime] frame-rate mode=unlimited scheduler_target="
              << blox7::runtime::kMaximumSupportedRobloxSchedulerFps
              << " vsync=" << runtime_config.config.vsync_mode() << '\n';
  }

  const libc_shim::HostCaBundleResolution ca_bundle =
      libc_shim::ResolveHostCaBundle();
  if (ca_bundle.status == libc_shim::HostCaBundleStatus::kInvalidOverride) {
    std::cerr << "Invalid BLOX7_CA_BUNDLE: expected a readable, non-empty "
                 "absolute regular file\n";
    return 2;
  }

  blox7::runtime::RobloxFullscreenRuntimeBridge fullscreen_bridge;
  blox7::audio::RobloxOutputDeviceBridge output_device_bridge;
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    const std::string compatibility_manifest =
        environment.GetOr("BLOX7_COMPATIBILITY_MANIFEST",
                          BLOX7_DEFAULT_COMPATIBILITY_MANIFEST);
    const blox7::compat::PayloadCompatibilityResult compatibility =
        blox7::compat::CheckPayloadCompatibility(
            runtime_config.config.roblox_library_path().string(),
            compatibility_manifest,
            command_line.options.allow_unverified_build);
    if (!compatibility) {
      std::cerr << "[FATAL] " << compatibility.error << '\n';
      return EXIT_FAILURE;
    }
    const blox7::Status fullscreen_status =
        fullscreen_bridge.Install(compatibility.profile);
    if (!fullscreen_status.ok()) {
      std::cerr << "[FATAL] Cannot install fullscreen runtime bridge: "
                << fullscreen_status.message() << '\n';
      return EXIT_FAILURE;
    }
    if (compatibility.profile.fmod_output_device_bridge &&
        compatibility.profile.fmod_output_device_bridge->has_input_devices()) {
      std::string menu_settings;
      const char *settings =
          std::getenv("BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON");
      if (!blox7::runtime::MergeAudioDeviceMenuClientSettingsOverrides(
              settings ? settings : "{}", &menu_settings,
              &command_line_error) ||
          setenv("BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON",
                 menu_settings.c_str(), 1) != 0) {
        std::cerr << "[FATAL] Cannot configure host audio device menu: "
                  << command_line_error << '\n';
        return EXIT_FAILURE;
      }
    }
    const blox7::Status output_device_status =
        output_device_bridge.Install(compatibility.profile);
    if (!output_device_status.ok()) {
      std::cerr << "[FATAL] Cannot install Roblox output-device bridge: "
                << output_device_status.message() << '\n';
      return EXIT_FAILURE;
    }
  }

  blox7::legacy::RuntimeDependencies dependencies;
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    auto http_client = std::make_shared<blox7::services::CurlHttpClient>();
    blox7::services::BrowserTrackerService browser_tracker(*http_client);
    std::filesystem::path tracker_cookie_file =
        paths.data_root() / "browser_tracker.cookie";
    bool tracker_cookie_owned = true;
    if (environment.HasNonEmpty("BLOX7_COOKIE_FILE")) {
      tracker_cookie_file = environment.GetOr("BLOX7_COOKIE_FILE", "");
      tracker_cookie_owned = false;
    }
    const blox7::services::BrowserTrackerResult browser_tracker_result =
        browser_tracker.EnsureInitialized(app_storage_file, tracker_cookie_file,
                                          tracker_cookie_owned);
    if (!browser_tracker_result) {
      std::cerr << "  [runtime] BrowserTracker bootstrap skipped: "
                << browser_tracker_result.error;
      if (browser_tracker_result.http_status != 0) {
        std::cerr << " (HTTP " << browser_tracker_result.http_status << ')';
      }
      std::cerr << '\n';
    } else {
      std::cout << "  [runtime] BrowserTracker identity "
                << (browser_tracker_result.status ==
                            blox7::services::BrowserTrackerStatus::kCreated
                        ? "initialized"
                        : "loaded")
                << " before engine startup\n";
    }
    blox7::services::AuthService auth_service(*http_client);
    blox7::runtime::AuthRuntimeComposition composition =
        blox7::runtime::ComposeAuthRuntime(environment, paths, auth_service,
                                              http_client);
    if (!external_launch_request.has_value() &&
        command_line.options.window_mode !=
            blox7::runtime::WindowMode::kHeadless) {
      PromptFirstLaunchSignIn(environment, paths, auth_service, http_client,
                              &composition);
    }
    if (!composition) {
      std::cerr << "[FATAL] Typed Roblox authentication preflight failed: "
                << composition.error;
      if (composition.http_status != 0) {
        std::cerr << " (HTTP " << composition.http_status << ')';
      }
      std::cerr << '\n';
      return EXIT_FAILURE;
    }
    composition.jni_vm->SetPlatformIdentity(jnivm::PlatformIdentity{
        runtime_config.config.input_capabilities().touch_enabled,
        runtime_config.config.input_capabilities().mouse_enabled,
        runtime_config.config.input_capabilities().keyboard_enabled,
        runtime_config.config.device_profile().pc_hardware,
        std::string(runtime_config.config.device_profile().platform_name),
        std::string(runtime_config.config.device_profile().display_name),
        std::string(runtime_config.config.device_profile().manufacturer),
        std::string(runtime_config.config.device_profile().model),
        std::string(runtime_config.config.device_profile().brand),
        std::string(runtime_config.config.device_profile().device_code),
        std::string(runtime_config.config.device_profile().device_sku),
        std::string(runtime_config.config.device_profile().soc_model),
    });
    if (composition.rejected_credential_retired) {
      constexpr std::string_view kSignedOutMessage =
          "Your saved Roblox session is no longer valid. Sign in again to "
          "continue.";
      std::cout << "  [auth] saved Roblox session expired; continuing with "
                   "native sign-in\n";
      (void)blox7::runtime::ShowWarningDialog(environment,
                                                 kSignedOutMessage);
    }
    if (composition.status ==
        blox7::runtime::AuthRuntimeStatus::kAuthenticated) {
      if (setenv("BLOX7_NATIVE_SET_USER_ID", "1", 1) != 0) {
        std::cerr << "[FATAL] Cannot enable authenticated NativeSettings "
                     "identity\n";
        return EXIT_FAILURE;
      }
      std::cout
          << "  [auth] typed Roblox identity resolved for production VM\n";
    } else {
      std::cout
          << "  [auth] explicit guest identity selected for production VM\n";
    }
    if (runtime_config.config.desktop_playability()) {
      std::filesystem::path assets = paths.DefaultAssetPath();
      if (environment.HasNonEmpty("BLOX7_ASSET_PATH")) {
        assets = environment.GetOr("BLOX7_ASSET_PATH", "");
      }
      const std::filesystem::path default_app_policy =
          assets / "guac/defaultConfigs/GuacDefaultPolicy-GlobalDist.json";
      const std::int64_t app_policy_user_id =
          composition.status ==
                  blox7::runtime::AuthRuntimeStatus::kAuthenticated
              ? composition.account_identity.user_id
              : -1;
      const blox7::runtime::DesktopAppPolicyResult desktop_policy =
          blox7::runtime::ApplyDesktopAppPolicy(
              app_storage_file, default_app_policy, app_policy_user_id,
              runtime_config.config.theme_mode());
      if (!desktop_policy) {
        std::cerr << "[FATAL] Desktop Roblox app-policy failed: "
                  << desktop_policy.error << '\n';
        return EXIT_FAILURE;
      }
      std::string desktop_client_settings;
      if (!blox7::runtime::MergeDesktopAppPolicyClientSettingsOverride(
              desktop_policy.policy_json,
              environment.GetOr("BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON",
                                "{}"),
              &desktop_client_settings, &command_line_error)) {
        std::cerr << "[FATAL] Cannot activate desktop Roblox app-policy: "
                  << command_line_error << '\n';
        return EXIT_FAILURE;
      }
      if (setenv("BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON",
                 desktop_client_settings.c_str(), 1) != 0) {
        std::cerr << "[FATAL] Cannot export desktop Roblox app-policy\n";
        return EXIT_FAILURE;
      }
      std::cout << "  [runtime] desktop app-policy ready: normalized="
                << desktop_policy.normalized_policy_count
                << " updated=" << (desktop_policy.updated ? 1 : 0)
                << " runtime_override=1\n";
    }
    auto android_window_context =
        std::make_shared<AndroidWindowBridgeContext>();
    jnivm::AndroidWindowCallbacks android_window_callbacks;
    android_window_callbacks.set_flags = &QueueAndroidWindowFlags;
    composition.jni_vm->SetAndroidWindowCallbacks(
        std::move(android_window_context), android_window_callbacks);
    const blox7::Status audio_status =
        blox7::audio::InstallFmodJniAudioBridge(composition.jni_vm.get());
    if (!audio_status.ok()) {
      std::cerr << "[FATAL] Typed FMOD Java audio composition failed: "
                << audio_status.message() << '\n';
      return EXIT_FAILURE;
    }
    const blox7::Status voice_status =
        blox7::audio::InstallWebRtcJniAudioBridge(
            composition.jni_vm.get());
    if (!voice_status.ok()) {
      (void)blox7::audio::ShutdownFmodJniAudioBridge(
          composition.jni_vm.get());
      std::cerr << "[FATAL] WebRTC voice audio composition failed: "
                << voice_status.message() << '\n';
      return EXIT_FAILURE;
    }
    dependencies = blox7::legacy::RuntimeDependencies(
        std::move(composition), &ShutdownPlatformBridges);
  }
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun &&
      !isolated_canary) {
    blox7::Status broker_status =
        blox7::runtime::ExternalLaunchBroker::StartOwnerAfterLockAcquired(
            broker_options, &external_launch_broker.broker(),
            external_launch_request.has_value() ? &*external_launch_request
                                                : nullptr);
    if (broker_status.ok()) external_launch_request.reset();
    if (broker_status.ok()) {
      broker_status = blox7::runtime::InstallActiveExternalLaunchBroker(
          external_launch_broker.broker());
    }
    if (!broker_status.ok()) {
      std::cerr << "[FATAL] Cannot activate website launch bridge: "
                << broker_status.message() << '\n';
      return EXIT_FAILURE;
    }
  }
  failure_dialog.SetMessage(
      "Roblox closed unexpectedly because of an internal error.");
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    blox7::runtime::LogProcessDiagnostics(
        blox7::runtime::ProcessDiagnosticStage::kNativeRuntime);
  }
  int runtime_status =
      blox7::legacy::Run(command_line.options, std::move(dependencies));
  background_update.Stop();
  if (command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    blox7::runtime::LogProcessDiagnostics(
        blox7::runtime::ProcessDiagnosticStage::kShutdown);
    std::cerr << "[diagnostic] native runtime exit_status=" << runtime_status
              << '\n';
  }
  const blox7::Status launch_broker_shutdown_status =
      external_launch_broker.Shutdown();
  if (!launch_broker_shutdown_status.ok()) {
    std::cerr << "  [launch] website bridge shutdown failed: "
              << launch_broker_shutdown_status.message() << '\n';
    runtime_status = EXIT_FAILURE;
  }
  if (runtime_status != EXIT_SUCCESS) {
    failure_dialog.SetMessage("Roblox stopped with error code " +
                              std::to_string(runtime_status) + ".");
  }
  support_bundle_guard.SetExitCode(runtime_status);
  output_device_bridge.Shutdown();
  fullscreen_bridge.Shutdown();
  memory_limit_watchdog.Stop();
  const blox7::Status game_mode_stop_status = game_mode_session.Stop();
  if (!game_mode_stop_status.ok()) {
    std::cerr << "  [gamemode] release failed: "
              << game_mode_stop_status.message() << '\n';
  }
  if (runtime_status == EXIT_SUCCESS &&
      command_line.options.mode == blox7::runtime::CommandMode::kRun) {
    failure_dialog.MarkSuccessful();
    support_bundle_guard.Disarm();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(EXIT_SUCCESS);
  }
  if (runtime_status == EXIT_SUCCESS) {
    failure_dialog.MarkSuccessful();
    support_bundle_guard.Disarm();
  }
  return runtime_status;
}
