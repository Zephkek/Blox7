#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "jnivm/jnivm.h"
#include "runtime/roblox_experience_composition.h"
#include "runtime/roblox_web_view_bridge.h"

namespace blox7::runtime {

class RobloxExperienceCompositionWebSurfaceTest {
 public:
  static RobloxWebViewSink Sink(RobloxExperienceComposition* composition) {
    RobloxWebViewSink sink;
    sink.context = composition;
    sink.dispatch_open = &RobloxExperienceComposition::DispatchWebViewOpen;
    sink.dispatch_mutate = &RobloxExperienceComposition::DispatchWebViewMutate;
    sink.dispatch_close = &RobloxExperienceComposition::DispatchWebViewClose;
    sink.dispatch_cookie = &RobloxExperienceComposition::DispatchWebViewCookie;
    return sink;
  }

  static Status OpenBrowser(RobloxExperienceComposition* composition, const std::string& url,
                            WebViewHelperExitObserver observer) {
    RobloxBrowserServiceOpenRequest request;
    request.url = url;
    return RobloxExperienceComposition::DispatchBrowserServiceOpen(composition, request, std::move(observer));
  }

  static bool HasProcess(RobloxExperienceComposition* composition) {
    std::lock_guard<std::mutex> lock(composition->mutex_);
    return composition->web_surface_process_ != nullptr;
  }

  static void ExhaustLogical(RobloxExperienceComposition* composition) {
    std::lock_guard<std::mutex> lock(composition->mutex_);
    composition->next_web_surface_logical_generation_ = std::numeric_limits<uint64_t>::max();
  }

  static void ExhaustProcess(RobloxExperienceComposition* composition) {
    std::lock_guard<std::mutex> lock(composition->mutex_);
    composition->next_web_surface_process_generation_ = std::numeric_limits<uint64_t>::max();
  }
};

}  // namespace blox7::runtime

namespace {

using blox7::Status;
using blox7::StatusCode;
using blox7::runtime::JniEnvironmentProvider;
using blox7::runtime::RobloxExperienceComposition;
using blox7::runtime::RobloxWebViewBridge;
using blox7::runtime::RobloxWebViewMessageBusObjects;
using blox7::runtime::RobloxWebViewMessageBusSymbols;
using blox7::runtime::RobloxWebViewMutationRequest;
using blox7::runtime::RobloxWebViewOpenRequest;
using blox7::runtime::RobloxWebViewSink;
using blox7::runtime::SecureRobloxCredential;
using blox7::runtime::WebViewHelperExitObserver;
using Peek = blox7::runtime::RobloxExperienceCompositionWebSurfaceTest;

constexpr char kHelperMode[] = "BLOX7_OPEN_TEST_HELPER";
constexpr char kServers[] = R"({"url":"https://www.roblox.com/games/1/servers"})";
constexpr char kElsewhere[] = R"({"url":"https://example.com/"})";

int g_failures = 0;

void Expect(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  std::fflush(stdout);
  if (!ok) ++g_failures;
}

enum class Answer { kOpen, kFail, kFailAfterExit };

struct Probe {
  int closes = 0;
  std::string focus;
  int opens = 0;
  int close_requests = 0;
  Answer answer = Answer::kOpen;
  RobloxWebViewOpenRequest request;
  WebViewHelperExitObserver observer;
};

Probe* g_probe = nullptr;
jlong g_next_connection = 90;

std::string Text(JNIEnv* env, jstring value) {
  if (value == nullptr) return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) return {};
  std::string text(chars);
  env->ReleaseStringUTFChars(value, chars);
  return text;
}

jstring OpenId(JNIEnv* env, jclass) { return env->NewStringUTF("openWindow"); }
jstring MutateId(JNIEnv* env, jclass) { return env->NewStringUTF("mutateWindow"); }
jstring CloseId(JNIEnv* env, jclass) { return env->NewStringUTF("closeWindow"); }
jstring HandleCloseId(JNIEnv* env, jclass) { return env->NewStringUTF("handleWindowClose"); }
jstring ProtocolName(JNIEnv* env, jclass) { return env->NewStringUTF("WebView"); }
jstring AvailableId(JNIEnv* env, jclass) { return env->NewStringUTF("isAvailable"); }

jstring MessageId(JNIEnv* env, jclass, jstring protocol, jstring method) {
  return env->NewStringUTF((Text(env, protocol) + "." + Text(env, method)).c_str());
}

void InitializeProtocol(JNIEnv*, jclass) {}

jobject Subscribe(JNIEnv* env, jobject, jstring, jobject, jboolean) {
  jclass connection = env->FindClass("com/roblox/universalapp/messagebus/Connection");
  jmethodID constructor = env->GetMethodID(connection, "<init>", "(J)V");
  return env->NewObject(connection, constructor, ++g_next_connection);
}

void Disconnect(JNIEnv*, jobject, jlong) {}
void SetHandler(JNIEnv*, jobject, jstring, jstring, jobject) {}
void ClearHandler(JNIEnv*, jobject, jstring, jstring) {}

void Publish(JNIEnv* env, jobject, jstring id, jstring) {
  if (g_probe != nullptr && Text(env, id) == "WebView.handleWindowClose") ++g_probe->closes;
}

void Focus(JNIEnv* env, jclass, jstring, jstring, jstring state) {
  if (g_probe != nullptr) g_probe->focus += Text(env, state) + " ";
}

void SignalJavascript(JNIEnv*, jclass, jstring) {}
void CookieHandler(JNIEnv*, jobject, jobject) {}

jobject CreateRawCallback(void* context, std::shared_ptr<void> target, void (*run)(void*, JNIEnv*, jstring)) {
  return static_cast<jnivm::VM*>(context)->CreateMessageBusRawCallback(std::move(target),
                                                                       jnivm::MessageBusRawCallbacks{run});
}

void ClearRawCallback(void* context, jobject callback) {
  static_cast<jnivm::VM*>(context)->ClearMessageBusRawCallback(callback);
}

jobject CreateRequestHandler(void* context, std::shared_ptr<void> target, std::string (*run)(void*, JNIEnv*, jstring)) {
  return static_cast<jnivm::VM*>(context)->CreateMessageBusRequestHandler(
      std::move(target), jnivm::MessageBusRequestHandlerCallbacks{run});
}

void ClearRequestHandler(void* context, jobject handler) {
  static_cast<jnivm::VM*>(context)->ClearMessageBusRequestHandler(handler);
}

bool SetPlatformCallbacks(void* context, std::shared_ptr<void> target,
                          void (*notification)(void*, JNIEnv*, jstring, jstring),
                          void (*app_bridge)(void*, JNIEnv*, jstring, jstring),
                          void (*overlay)(void*, JNIEnv*, jstring, jstring),
                          void (*web_activity)(void*, JNIEnv*, jstring, jstring), void (*sync)(void*, JNIEnv*, jstring),
                          void (*set)(void*, JNIEnv*, jstring, jstring)) {
  static_cast<jnivm::VM*>(context)->SetRobloxDataModelNotificationCallbacks(
      std::move(target),
      jnivm::RobloxDataModelNotificationCallbacks{notification, app_bridge, overlay, web_activity, sync, set});
  return true;
}

void ClearPlatformCallbacks(void* context) {
  static_cast<jnivm::VM*>(context)->ClearRobloxDataModelNotificationCallbacks();
}

Status SinkOpen(void* context, const RobloxWebViewOpenRequest& request, WebViewHelperExitObserver observer) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->opens;
  probe->request = request;
  if (probe->answer == Answer::kFailAfterExit) {
    observer.on_exit(observer.context.get());
  }
  if (probe->answer != Answer::kOpen) {
    return Status::Error(StatusCode::kUnavailable, "test sink refused the window");
  }
  probe->observer = std::move(observer);
  return Status::Ok();
}

Status SinkMutate(void*, const RobloxWebViewMutationRequest&) { return Status::Ok(); }

Status SinkClose(void* context) {
  ++static_cast<Probe*>(context)->close_requests;
  return Status::Ok();
}

Status SinkCookie(void*, std::string_view) { return Status::Ok(); }

RobloxWebViewMessageBusSymbols Symbols() {
  RobloxWebViewMessageBusSymbols symbols;
  symbols.get_open_window_id = &OpenId;
  symbols.get_handle_window_close_id = &HandleCloseId;
  symbols.get_protocol_name = &ProtocolName;
  symbols.get_is_available_id = &AvailableId;
  symbols.get_message_id = &MessageId;
  symbols.initialize_android_web_view_protocol = &InitializeProtocol;
  symbols.subscribe_raw = &Subscribe;
  symbols.delete_connection = &Disconnect;
  symbols.set_request_handler_raw = &SetHandler;
  symbols.clear_request_handler = &ClearHandler;
  symbols.publish_raw = &Publish;
  symbols.broadcast_data_model_focus = &Focus;
  symbols.get_mutate_window_id = &MutateId;
  symbols.get_close_window_id = &CloseId;
  symbols.signal_javascript_callback = &SignalJavascript;
  symbols.update_cookie_set_handler = &CookieHandler;
  return symbols;
}

RobloxWebViewMessageBusObjects Objects(jnivm::VM* vm, jobject bus) {
  RobloxWebViewMessageBusObjects objects;
  objects.message_bus = bus;
  objects.callback_factory_context = vm;
  objects.create_raw_callback = &CreateRawCallback;
  objects.clear_raw_callback = &ClearRawCallback;
  objects.create_request_handler = &CreateRequestHandler;
  objects.clear_request_handler = &ClearRequestHandler;
  objects.set_platform_web_callbacks = &SetPlatformCallbacks;
  objects.clear_platform_web_callbacks = &ClearPlatformCallbacks;
  return objects;
}

RobloxWebViewSink FakeSink(Probe* probe) {
  RobloxWebViewSink sink;
  sink.context = probe;
  sink.dispatch_open = &SinkOpen;
  sink.dispatch_mutate = &SinkMutate;
  sink.dispatch_close = &SinkClose;
  sink.dispatch_cookie = &SinkCookie;
  return sink;
}

class Harness {
 public:
  Harness(jnivm::VM* vm, jobject bus, RobloxExperienceComposition* composition)
      : bridge_(JniEnvironmentProvider{vm->GetJavaVM(), nullptr, nullptr}, Symbols(), Objects(vm, bus),
                composition != nullptr ? Peek::Sink(composition) : FakeSink(&probe)) {
    g_probe = &probe;
    initialized = bridge_.Initialize().ok();
  }

  ~Harness() {
    (void)bridge_.Shutdown();
    g_probe = nullptr;
  }

  Status Open(const std::string& json) { return bridge_.HandleOwnedMessage(json); }

  int Closes() {
    (void)bridge_.DrainHostWindowEvents();
    return probe.closes;
  }

  void Exit() {
    WebViewHelperExitObserver observer = std::move(probe.observer);
    if (observer.valid()) observer.on_exit(observer.context.get());
  }

  RobloxWebViewBridge& bridge() { return bridge_; }

  Probe probe;
  bool initialized = false;

 private:
  RobloxWebViewBridge bridge_;
};

bool Hidden(const RobloxWebViewOpenRequest& request) {
  return request.is_visible.has_value() && !*request.is_visible;
}

bool Shown(const RobloxWebViewOpenRequest& request) {
  return request.is_visible.has_value() && *request.is_visible;
}

bool Says(const Status& status, const char* text) {
  return !status.ok() && status.message().find(text) != std::string::npos;
}

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

void SetEnv(const char* name, const char* value) {
  if (value != nullptr) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
}

void BridgeCases(jnivm::VM* vm, jobject bus) {
  SetEnv("BLOX7_NATIVE_LOGIN", nullptr);
  {
    Harness h(vm, bus, nullptr);
    Expect(h.initialized, "bridge starts with a fake sink");

    Status status = h.Open(
        R"({"url":"https://www.roblox.com/challenge/cdn/hybrid","title":"Prewarm WebView","isVisible":false})");
    Expect(status.ok() && Hidden(h.probe.request) && h.probe.request.title == "Prewarm WebView" &&
               h.probe.request.url == "https://www.roblox.com/challenge/cdn/hybrid",
           "roblox's prewarm challenge stays hidden and untouched");

    status = h.Open(R"({"url":"https://www.roblox.com/challenge/cdn/hybrid","title":"Security","isVisible":false})");
    Expect(status.ok() && Shown(h.probe.request) && h.probe.request.title == "Security",
           "a challenge opened hidden is shown");

    status = h.Open(R"({"url":"www:captcha/app/login?hybrid-return-token=1","isVisible":false})");
    Expect(status.ok() && Shown(h.probe.request) && h.probe.request.title == "Roblox verification",
           "a challenge without a title is shown as roblox verification");

    status = h.Open(
        R"({"url":"https://www.roblox.com/challenge/cdn/hybrid","title":"prewarm webview","isVisible":false})");
    Expect(status.ok() && Shown(h.probe.request), "only roblox's exact prewarm title stays hidden");

    status = h.Open(R"({"url":"https://www.roblox.com/games/1/servers","isVisible":false})");
    Expect(status.ok() && Hidden(h.probe.request), "a hidden page that isn't a challenge stays hidden");
    Expect(h.Closes() == 0, "no close while the windows are open");
  }

  SetEnv("BLOX7_NATIVE_LOGIN", "0");
  {
    Harness h(vm, bus, nullptr);
    Status status = h.Open(R"({"url":"https://www.roblox.com/challenge/cdn/hybrid","isVisible":false})");
    Expect(status.ok() && h.probe.request.url == "https://www.roblox.com/login" &&
               h.probe.request.title == "Roblox sign in" && Shown(h.probe.request),
           "browser sign-in mode routes a challenge to the sign-in page");
    Expect(h.bridge().HandleCloseWindow().ok() && h.probe.close_requests == 0,
           "browser sign-in mode keeps that page open when roblox closes the challenge");

    status = h.Open(
        R"({"url":"https://www.roblox.com/challenge/cdn/hybrid","title":"Prewarm WebView","isVisible":false})");
    Expect(status.ok() && h.probe.request.url == "https://www.roblox.com/challenge/cdn/hybrid" &&
               Hidden(h.probe.request),
           "browser sign-in mode leaves the prewarm alone");
    Expect(h.bridge().HandleCloseWindow().ok() && h.probe.close_requests == 1,
           "roblox's close of the prewarm closes it");
  }
  SetEnv("BLOX7_NATIVE_LOGIN", nullptr);

  {
    Harness h(vm, bus, nullptr);
    Expect(h.Open(kServers).ok() && h.Closes() == 0, "an open window gives no close");
    h.probe.answer = Answer::kFailAfterExit;
    Expect(!h.Open(kServers).ok(), "a window the sink can't open fails");
    Expect(h.Closes() == 1, "that failure gives roblox one close");
    h.Exit();
    Expect(h.Closes() == 1, "the window it replaced closing later gives no second close");
  }

  {
    Harness h(vm, bus, nullptr);
    Expect(h.Open(kServers).ok(), "an open window");
    h.probe.answer = Answer::kFail;
    Expect(!h.Open(kServers).ok() && h.Closes() == 0, "a refusal that closed nothing gives no close");
    h.Exit();
    Expect(h.Closes() == 1, "the window still open closing later gives its close");
  }

  {
    Harness h(vm, bus, nullptr);
    h.probe.answer = Answer::kFailAfterExit;
    JNIEnv* env = vm->GetJNIEnv();
    const bool dispatched = vm->DispatchRobloxDataModelNotification(
        env, env->NewStringUTF("OPEN_CUSTOM_WEBVIEW"), env->NewStringUTF(kServers));
    Expect(dispatched && h.probe.opens == 1 && h.Closes() == 0 && h.probe.focus == "Focused ",
           "a data model window that can't open gives focus back to roblox");
  }
}

struct Composed {
  Composed(jnivm::VM* vm, jobject bus, bool signed_in)
      : credential(".ROBLOSECURITY=_|blox7-open-test"),
        composition(JniEnvironmentProvider{}, blox7::runtime::RobloxExperienceMessageBusSymbols{},
                    RobloxWebViewMessageBusSymbols{}, blox7::runtime::RobloxBrowserServiceSymbols{},
                    blox7::runtime::RobloxPermissionsMessageBusSymbols{},
                    blox7::runtime::RobloxGameSessionSymbols{}, blox7::runtime::RobloxExperienceJniFactory{},
                    blox7::runtime::RobloxFreshLaunchPresentBoundary{},
                    blox7::runtime::RobloxGameSurfaceJniConfig{}, signed_in ? &credential : nullptr),
        harness(vm, bus, &composition) {}

  bool HasProcess() { return Peek::HasProcess(&composition); }

  SecureRobloxCredential credential;
  RobloxExperienceComposition composition;
  Harness harness;
};

void CountExit(void* context) { ++*static_cast<int*>(context); }

void CompositionCases(jnivm::VM* vm, jobject bus, const std::string& self) {
  const std::string missing = self + ".missing";
  SetEnv(kHelperMode, nullptr);
  SetEnv("BLOX7_WEBVIEW_HELPER", self.c_str());

  {
    Composed c(vm, bus, false);
    Expect(c.harness.initialized, "bridge starts on the real composition");
    const Status status = c.harness.Open(kServers);
    Expect(Says(status, "not synchronized") && c.harness.Closes() == 1,
           "a window before the sign-in state is known gives one close");
  }

  {
    Composed c(vm, bus, true);
    SetEnv("BLOX7_WEBVIEW_HELPER", missing.c_str());
    const Status status = c.harness.Open(kServers);
    Expect(Says(status, "could not display Roblox web surface") && c.harness.Closes() == 1,
           "a missing helper gives one close");
    SetEnv("BLOX7_WEBVIEW_HELPER", self.c_str());
  }

  {
    Composed c(vm, bus, true);
    const Status status = c.harness.Open(kElsewhere);
    Expect(Says(status, "could not display Roblox web surface") && c.harness.Closes() == 1 && !c.HasProcess(),
           "a page the helper refuses gives one close");
  }

  {
    Composed c(vm, bus, true);
    Peek::ExhaustLogical(&c.composition);
    const Status status = c.harness.Open(kServers);
    Expect(Says(status, "logical surface generation is exhausted") && c.harness.Closes() == 1,
           "running out of window numbers gives one close");
  }

  {
    Composed c(vm, bus, true);
    Peek::ExhaustProcess(&c.composition);
    const Status status = c.harness.Open(kServers);
    Expect(Says(status, "process generation is exhausted") && c.harness.Closes() == 1,
           "running out of helper numbers gives one close");
  }

  {
    Composed c(vm, bus, true);
    SetEnv(kHelperMode, "silent");
    const auto started = std::chrono::steady_clock::now();
    const Status status = c.harness.Open(kServers);
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    Expect(Says(status, "did not become ready") && c.harness.Closes() == 1,
           "a helper that never gets ready gives one close (" + std::to_string(waited.count()) + " ms)");
  }

  {
    Composed c(vm, bus, true);
    SetEnv(kHelperMode, "quit");
    const Status status = c.harness.Open(kServers);
    const bool configuring =
        Says(status, "could not configure reusable Roblox web surface") || Says(status, "exited during configuration");
    Expect(configuring && c.harness.Closes() == 1,
           "a helper that quits while it's set up gives one close (" + status.message() + ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    Expect(c.harness.Closes() == 1, "its exit adds nothing");
  }

  {
    Composed c(vm, bus, true);
    SetEnv(kHelperMode, "hangup");
    const Status status = c.harness.Open(kServers);
    Expect(Says(status, "exited during configuration") && c.harness.Closes() == 1,
           "a helper that hangs up while it's set up gives one close");
  }

  {
    Composed c(vm, bus, true);
    SetEnv(kHelperMode, "ready");
    Status status = c.harness.Open(kServers);
    Expect(status.ok() && c.HasProcess() && c.harness.Closes() == 0, "a working helper opens the window");
    status = c.harness.Open(kElsewhere);
    Expect(Says(status, "could not configure reusable Roblox web surface") && c.harness.Closes() == 1,
           "a page the open helper can't load gives one close");
    Expect(WaitFor([&] { return !c.HasProcess(); }, std::chrono::seconds(15)), "that helper is closed and gone");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Expect(c.harness.Closes() == 1, "the closed helper's exit gives no second close");
  }

  {
    Composed c(vm, bus, true);
    SetEnv(kHelperMode, "linger");
    const Status status = c.harness.Open(kServers);
    Expect(status.ok() && c.HasProcess() && c.harness.Closes() == 0, "a helper that closes later opens the window");
    Expect(WaitFor([&] { return c.harness.Closes() > 0; }, std::chrono::seconds(15)), "its exit gives a close");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Expect(c.harness.Closes() == 1 && !c.HasProcess(), "exactly one");
  }

  {
    Composed c(vm, bus, true);
    SetEnv("BLOX7_WEBVIEW_HELPER", missing.c_str());
    auto exits = std::make_shared<int>(0);
    const Status status = Peek::OpenBrowser(&c.composition, "https://www.roblox.com/home",
                                            WebViewHelperExitObserver{exits, &CountExit});
    Expect(!status.ok() && *exits == 1, "a browser service window that can't open reports one exit");
    SetEnv("BLOX7_WEBVIEW_HELPER", self.c_str());
  }
  SetEnv(kHelperMode, nullptr);
}

std::string Frame(const std::string& packet) {
  std::string out(4, '\0');
  for (int i = 0; i < 4; ++i) out[i] = static_cast<char>((packet.size() >> (24 - 8 * i)) & 0xff);
  return out + packet;
}

int RunHelper(int argc, char** argv) {
  int port = 0;
  std::string token;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0) port = std::atoi(argv[i + 1]);
    if (std::strcmp(argv[i], "--token") == 0) token = argv[i + 1];
  }
  const char* variable = std::getenv(kHelperMode);
  const std::string mode = variable != nullptr ? variable : "ready";
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) return 2;
  std::string ready = "MWVE";
  ready += '\x01';
  ready += '\x03';
  ready.append(6, '\0');
  std::string hello = Frame(token);
  if (mode != "silent") hello += Frame(ready);
  for (std::size_t sent = 0; sent < hello.size();) {
    const ssize_t count = send(fd, hello.data() + sent, hello.size() - sent, 0);
    if (count <= 0) return 3;
    sent += static_cast<std::size_t>(count);
  }
  if (mode == "quit") {
    close(fd);
    return 0;
  }
  if (mode == "hangup") shutdown(fd, SHUT_WR);
  const int lifetime = mode == "linger" ? 1500 : mode == "hangup" ? 2000 : 20000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(lifetime);
  std::string pending;
  bool closing = false;
  while (!closing && std::chrono::steady_clock::now() < deadline) {
    pollfd readable = {fd, POLLIN, 0};
    if (poll(&readable, 1, 50) <= 0) continue;
    char chunk[4096];
    const ssize_t count = read(fd, chunk, sizeof chunk);
    if (count <= 0) break;
    pending.append(chunk, static_cast<std::size_t>(count));
    while (pending.size() >= 4) {
      std::size_t size = 0;
      for (int i = 0; i < 4; ++i) size = (size << 8) | static_cast<unsigned char>(pending[i]);
      if (pending.size() < 4 + size) break;
      closing = closing || (size > 5 && pending[4 + 5] == 5);
      pending.erase(0, 4 + size);
    }
  }
  close(fd);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--port") == 0) return RunHelper(argc, argv);
  const std::string self = argc > 1 ? argv[1] : argv[0];
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jobject bus = env->AllocObject(env->FindClass("com/roblox/universalapp/messagebus/MessageBus"));
  BridgeCases(&vm, bus);
  CompositionCases(&vm, bus, self);
  std::printf("%s\n", g_failures == 0 ? "all passed" : "FAILED");
  return g_failures == 0 ? 0 : 1;
}
