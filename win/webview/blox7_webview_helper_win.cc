// the web view on windows (webview2), same job as the gtk helper: roblox's sign-in and challenge pages
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../webview2/wv2setup.h"
#include "WebView2.h"
#include "delayload_guard.h"
#include "runtime/display_size.h"
#include "runtime/webview_helper_launcher.h"
#include "webview/webview_helper_policy.h"

namespace {

using blox7::runtime::WebViewHelperControlOperation;
using blox7::runtime::WebViewHelperEventType;
using blox7::webview::CaptchaEvent;
using blox7::webview::CaptchaEventType;
using blox7::webview::EvaluateNavigationUri;

constexpr UINT kControlMessage = WM_APP + 1;
constexpr UINT kControlClosed = WM_APP + 2;
constexpr UINT_PTR kCookieTimer = 1;
constexpr UINT_PTR kSetupTimer = 2;
constexpr UINT_PTR kFailureTimer = 3;
constexpr char kControlMagic[] = "MWVC";
constexpr char kEventMagic[] = "MWVE";
constexpr unsigned char kProtocolVersion = 1;
constexpr std::size_t kHeaderBytes = 12;

// the apk's js bridge: one __globalRobloxAndroidBridge__ with executeRoblox(String), sent over postMessage
constexpr char kAndroidBridge[] = R"JS(
(() => {
  "use strict";
  if (window !== window.top || !window.chrome || !window.chrome.webview) return;
  const webview = window.chrome.webview;
  const bridge = {};
  Object.defineProperty(bridge, "executeRoblox", {
    value: (query) => webview.postMessage({ handler: "executeRoblox", message: String(query) }),
    enumerable: true, writable: false, configurable: false
  });
  try {
    Object.defineProperty(window, "__globalRobloxAndroidBridge__", {
      value: bridge, enumerable: true, writable: false, configurable: false
    });
  } catch (_) {}
})();
)JS";

FILE* g_log_file;
void TraceLogLine(const char* line);

void Log(const char* format, ...) {
  char line[16384];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  fprintf(stderr, "[webview] %s\n", line);
  fflush(stderr);
  if (g_log_file != nullptr) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(g_log_file, "%02u:%02u:%02u.%03u [%lu] %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            GetCurrentProcessId(), line);
    fflush(g_log_file);
  }
  TraceLogLine(line);
}

std::wstring Wide(std::string_view text) {
  if (text.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}

std::string Utf8(const wchar_t* text) {
  if (text == nullptr || *text == 0) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::string TakeString(LPWSTR text) {
  std::string out = Utf8(text);
  CoTaskMemFree(text);
  return out;
}

void ClearSensitive(std::string* value) {
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t i = 0; i < value->size(); i++) bytes[i] = '\0';
  value->clear();
}

uint32_t ReadBigEndian32(const char* bytes) {
  const auto* b = reinterpret_cast<const unsigned char*>(bytes);
  return (uint32_t{b[0]} << 24) | (uint32_t{b[1]} << 16) | (uint32_t{b[2]} << 8) | uint32_t{b[3]};
}

void WriteBigEndian32(char* bytes, uint32_t value) {
  bytes[0] = static_cast<char>(value >> 24);
  bytes[1] = static_cast<char>(value >> 16);
  bytes[2] = static_cast<char>(value >> 8);
  bytes[3] = static_cast<char>(value);
}

template <typename Interface, typename... Args>
class Handler final : public Interface {
 public:
  explicit Handler(std::function<HRESULT(Args...)> function) : function_(std::move(function)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (riid == IID_IUnknown) {
      *object = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *object = static_cast<Interface*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return function_(args...); }

 private:
  std::function<HRESULT(Args...)> function_;
  LONG refs_ = 1;
};

template <typename Interface, typename... Args, typename Function>
Interface* MakeHandler(Function function) {
  return new Handler<Interface, Args...>(std::function<HRESULT(Args...)>(std::move(function)));
}

struct ExternalUriArgs : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_InitiatingOrigin(LPWSTR* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsUserInitiated(BOOL* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Cancel(BOOL* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Cancel(BOOL value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeferral(ICoreWebView2Deferral** value) = 0;
};

struct ExternalUriHandler : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ExternalUriArgs* args) = 0;
};

struct WebView17 : public ICoreWebView2_16 {
  virtual HRESULT STDMETHODCALLTYPE PostSharedBufferToScript(IUnknown* buffer, int access, LPCWSTR json) = 0;
};

struct WebView18 : public WebView17 {
  virtual HRESULT STDMETHODCALLTYPE add_LaunchingExternalUriScheme(ExternalUriHandler* handler,
                                                                   EventRegistrationToken* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_LaunchingExternalUriScheme(EventRegistrationToken token) = 0;
};

constexpr IID kIidWebView18 = {0x7a626017, 0x28be, 0x49b2, {0xb8, 0x65, 0x3b, 0xa2, 0xb3, 0x52, 0x2d, 0x90}};

template <typename T>
void SafeRelease(T** pointer) {
  if (*pointer != nullptr) {
    (*pointer)->Release();
    *pointer = nullptr;
  }
}

enum class CookieOperation { kNone, kRetainPersistent, kSet, kClear };

struct Surface {
  HWND window = nullptr;
  ICoreWebView2Controller* controller = nullptr;
  ICoreWebView2* web_view = nullptr;
  bool primary = false;
};

struct App {
  SOCKET control = INVALID_SOCKET;
  std::mutex send_lock;
  HWND message_window = nullptr;
  HWND owner = nullptr;
  HICON icon = nullptr;
  std::wstring data_directory;
  ICoreWebView2Environment* environment = nullptr;
  std::vector<Surface*> surfaces;
  Surface* primary = nullptr;
  std::string pending_url;
  std::vector<std::string> pending_scripts;
  std::string pending_cookie;
  std::string last_reported_cookie;
  std::wstring browser_arguments;
  std::wstring runtime_folder;
  bool webview2_requested = false;
  bool app_user_agent = false;
  CookieOperation cookie_operation = CookieOperation::kNone;
  bool cookie_sync_received = false;
  bool browser_login_mode = false;
  bool back_navigation_disabled = false;
  bool show_domain_as_title = false;
  bool terminating = false;
  bool startup_failed = false;
  HRESULT surface_result = S_OK;
  bool security_check = false;
  bool setting_up = false;
  bool setup_seen = false;
  ULONGLONG setup_since = 0;
  ULONGLONG setup_deadline = 0;
  std::wstring setup_line;
  const char* failure = nullptr;
  HRESULT failure_result = S_OK;
  bool visibility_set = false;
  bool visible = false;
  bool signed_in = false;
};

App g_app;

void BeginTermination(const char* reason);
Surface* CreateSurfaceWindow(bool primary, HWND owner);
void AttachWebView(Surface* surface, std::function<void(Surface*)> ready);
ICoreWebView2CookieManager* CookieManager(Surface* surface);
void WaitForSetup();
void PollSetup();
void WebView2Failed(const char* reason, HRESULT result);
void FinishFailure();

// BLOX7_WEBVIEW_TRACE=<dir> (or 1 for <data>\trace-<time>): one jsonl timeline of everything the helper sees,
// plus chromium's own netlog and log, to diff against the phone. passive only, no Runtime/Page domain,
// so pages can't tell. the .ROBLOSECURITY value never lands in our files, the netlog keeps it though
struct TraceState {
  bool on = false;
  std::wstring dir;
  FILE* file = nullptr;
  std::mutex lock;
  LARGE_INTEGER frequency{}, origin{};
  ULONGLONG origin_filetime = 0;
  HHOOK mouse_hook = nullptr;
  bool button_down = false;
  unsigned saved = 0;
  std::unordered_map<std::string, nlohmann::json> requests;
  std::string window_state;
};

TraceState g_trace;
constexpr UINT_PTR kTraceTimer = 7;

double TraceSeconds() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart - g_trace.origin.QuadPart) / static_cast<double>(g_trace.frequency.QuadPart);
}

std::string TraceUtc(double seconds) {
  const ULONGLONG time = g_trace.origin_filetime + static_cast<ULONGLONG>(seconds * 1e7);
  FILETIME file{static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)};
  SYSTEMTIME utc;
  FileTimeToSystemTime(&file, &utc);
  char text[48];
  snprintf(text, sizeof text, "%04u-%02u-%02uT%02u:%02u:%02u.%06uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
           utc.wMinute, utc.wSecond, static_cast<unsigned>((time / 10) % 1000000));
  return text;
}

// cookie values die here, whatever shape they come in (header text, cookie objects, getCookies)
std::string TraceRedact(std::string text) {
  for (std::string_view marker : {std::string_view("_|WARNING:-DO-NOT-SHARE-THIS"), std::string_view(".ROBLOSECURITY=")}) {
    const bool header = marker.front() == '.';
    for (std::size_t at = 0; (at = text.find(marker, at)) != std::string::npos;) {
      const std::size_t value = header ? at + marker.size() : at;
      std::size_t end = value;
      while (end < text.size() && strchr(";\",\\ \r\n", text[end]) == nullptr) end++;
      if (header && text.compare(value, 1, "<") == 0) {
        at = end;
        continue;
      }
      const std::string label = "<roblosecurity " + std::to_string(end - value) + " bytes>";
      text.replace(value, end - value, label);
      at = value + label.size();
    }
  }
  return text;
}

void TraceWrite(const char* kind, const nlohmann::json& record = nlohmann::json::object()) {
  if (!g_trace.on) return;
  const double seconds = TraceSeconds();
  char head[160];
  snprintf(head, sizeof head, "{\"t\":\"%s\",\"mono\":%.6f,\"k\":\"%s\"", TraceUtc(seconds).c_str(), seconds, kind);
  std::string body = record.is_object() && !record.empty()
                         ? record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
                         : std::string("{}");
  std::string line = std::string(head) + (body.size() > 2 ? "," + body.substr(1) : "}");
  line = TraceRedact(std::move(line));
  std::lock_guard<std::mutex> hold(g_trace.lock);
  if (g_trace.file == nullptr) return;
  fwrite(line.data(), 1, line.size(), g_trace.file);
  fputc('\n', g_trace.file);
}

void TraceLogLine(const char* line) {
  if (g_trace.on) TraceWrite("log", {{"msg", line}});
}

void MakeDirectories(const std::wstring& path) {
  for (std::size_t i = 3; i <= path.size(); i++) {
    if (i == path.size() || path[i] == L'\\' || path[i] == L'/') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
  }
}

std::string DecodeBase64(std::string_view text) {
  std::string out;
  unsigned value = 0;
  int bits = -8;
  for (const char c : text) {
    const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char* at = c != '\0' ? strchr(digits, c) : nullptr;
    if (at == nullptr) continue;
    value = (value << 6) | static_cast<unsigned>(at - digits);
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  return out;
}

// bodies go next to the timeline, named by order so a directory listing reads like the network tab
std::string TraceSave(const std::string& request_id, const std::string& url, const char* suffix, const std::string& data) {
  std::string tail = url.substr(0, url.find('?'));
  tail = tail.substr(tail.find_last_of('/') + 1);
  if (tail.size() > 60) tail.resize(60);
  for (char& c : tail) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') c = '_';
  }
  char name[160];
  snprintf(name, sizeof name, "%05u-%s-%s%s", ++g_trace.saved, request_id.c_str(), tail.c_str(), suffix);
  const std::wstring path = g_trace.dir + L"\\bodies\\" + Wide(name);
  FILE* file = _wfopen(path.c_str(), L"wb");
  if (file == nullptr) return {};
  fwrite(data.data(), 1, data.size(), file);
  fclose(file);
  return name;
}

std::string HeaderText(ICoreWebView2HttpHeadersCollectionIterator* iterator) {
  std::string text;
  BOOL has = FALSE;
  while (iterator != nullptr && SUCCEEDED(iterator->get_HasCurrentHeader(&has)) && has) {
    LPWSTR name = nullptr, value = nullptr;
    if (SUCCEEDED(iterator->GetCurrentHeader(&name, &value))) text += TakeString(name) + ": " + TakeString(value) + "\n";
    BOOL more = FALSE;
    if (FAILED(iterator->MoveNext(&more)) || !more) break;
  }
  if (iterator != nullptr) iterator->Release();
  return text;
}

std::string RequestHeaders(ICoreWebView2HttpRequestHeaders* headers) {
  ICoreWebView2HttpHeadersCollectionIterator* iterator = nullptr;
  if (headers != nullptr) headers->GetIterator(&iterator);
  return HeaderText(iterator);
}

std::string ResponseHeaders(ICoreWebView2HttpResponseHeaders* headers) {
  ICoreWebView2HttpHeadersCollectionIterator* iterator = nullptr;
  if (headers != nullptr) headers->GetIterator(&iterator);
  return HeaderText(iterator);
}

const char* MouseMessageName(WPARAM message) {
  switch (message) {
    case WM_MOUSEMOVE: return "move";
    case WM_LBUTTONDOWN: return "ldown";
    case WM_LBUTTONUP: return "lup";
    case WM_RBUTTONDOWN: return "rdown";
    case WM_RBUTTONUP: return "rup";
    case WM_MBUTTONDOWN: return "mdown";
    case WM_MBUTTONUP: return "mup";
    case WM_MOUSEWHEEL: return "wheel";
    case WM_MOUSEHWHEEL: return "hwheel";
    case WM_XBUTTONDOWN: return "xdown";
    case WM_XBUTTONUP: return "xup";
    default: return "other";
  }
}

bool IsOurWindow(HWND window) {
  for (Surface* surface : g_app.surfaces) {
    if (window == surface->window || IsChild(surface->window, window)) return true;
  }
  return false;
}

// raw mouse before chromium touches it: what vmware hands windows, the injected flag, the event clock
LRESULT CALLBACK TraceMouseHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && g_trace.on) {
    const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    const bool down = wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN || wparam == WM_MBUTTONDOWN;
    const bool up = wparam == WM_LBUTTONUP || wparam == WM_RBUTTONUP || wparam == WM_MBUTTONUP;
    const bool ours = IsOurWindow(WindowFromPoint(event->pt));
    if (ours || g_trace.button_down || down || up) {
      nlohmann::json record = {{"m", MouseMessageName(wparam)}, {"x", event->pt.x}, {"y", event->pt.y},
                               {"time", event->time}, {"ours", ours},
                               {"injected", (event->flags & LLMHF_INJECTED) != 0},
                               {"lower_il", (event->flags & LLMHF_LOWER_IL_INJECTED) != 0}};
      if (wparam == WM_MOUSEWHEEL || wparam == WM_MOUSEHWHEEL) {
        record["delta"] = static_cast<short>(HIWORD(event->mouseData));
      }
      if (event->dwExtraInfo != 0) record["extra"] = static_cast<unsigned long long>(event->dwExtraInfo);
      TraceWrite("mouse", record);
    }
    if (down) g_trace.button_down = true;
    if (up) g_trace.button_down = false;
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

std::string WindowClass(HWND window) {
  wchar_t name[128] = {};
  if (window != nullptr) GetClassNameW(window, name, 128);
  return Utf8(name);
}

void TraceWindowState() {
  if (!g_trace.on) return;
  HWND foreground = GetForegroundWindow();
  HWND focus = nullptr;
  GUITHREADINFO gui{sizeof gui};
  if (GetGUIThreadInfo(0, &gui)) focus = gui.hwndFocus;
  nlohmann::json state = {{"foreground_ours", IsOurWindow(foreground)},
                          {"foreground_class", WindowClass(foreground)},
                          {"focus_ours", focus != nullptr && IsOurWindow(focus)},
                          {"focus_class", WindowClass(focus)},
                          {"capture_ours", gui.hwndCapture != nullptr && IsOurWindow(gui.hwndCapture)}};
  if (g_app.primary != nullptr && g_app.primary->window != nullptr) {
    RECT rect{};
    GetWindowRect(g_app.primary->window, &rect);
    state["visible"] = IsWindowVisible(g_app.primary->window) != FALSE;
    state["iconic"] = IsIconic(g_app.primary->window) != FALSE;
    state["rect"] = {rect.left, rect.top, rect.right, rect.bottom};
  }
  const std::string text = state.dump();
  if (text != g_trace.window_state) {
    g_trace.window_state = text;
    TraceWrite("window_state", state);
  }
}

void TraceWindowMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (!g_trace.on) return;
  const char* name = nullptr;
  switch (message) {
    case WM_ACTIVATE: name = "WM_ACTIVATE"; break;
    case WM_ACTIVATEAPP: name = "WM_ACTIVATEAPP"; break;
    case WM_NCACTIVATE: name = "WM_NCACTIVATE"; break;
    case WM_MOUSEACTIVATE: name = "WM_MOUSEACTIVATE"; break;
    case WM_SETFOCUS: name = "WM_SETFOCUS"; break;
    case WM_KILLFOCUS: name = "WM_KILLFOCUS"; break;
    case WM_SHOWWINDOW: name = "WM_SHOWWINDOW"; break;
    case WM_SIZE: name = "WM_SIZE"; break;
    case WM_WINDOWPOSCHANGED: name = "WM_WINDOWPOSCHANGED"; break;
    case WM_ENTERSIZEMOVE: name = "WM_ENTERSIZEMOVE"; break;
    case WM_EXITSIZEMOVE: name = "WM_EXITSIZEMOVE"; break;
    case WM_DISPLAYCHANGE: name = "WM_DISPLAYCHANGE"; break;
    case 0x02E0: name = "WM_DPICHANGED"; break;
    case WM_CAPTURECHANGED: name = "WM_CAPTURECHANGED"; break;
    case WM_CLOSE: name = "WM_CLOSE"; break;
    case WM_DESTROY: name = "WM_DESTROY"; break;
    default: return;
  }
  nlohmann::json record = {{"msg", name}, {"wparam", static_cast<unsigned long long>(wparam)},
                           {"lparam", static_cast<long long>(lparam)}, {"window", WindowClass(window)}};
  if (message == WM_WINDOWPOSCHANGED) {
    const auto* position = reinterpret_cast<const WINDOWPOS*>(lparam);
    record["pos"] = {position->x, position->y, position->cx, position->cy, position->flags};
  }
  TraceWrite("window", record);
}

void TraceCookies(Surface* surface, const char* reason) {
  if (!g_trace.on) return;
  ICoreWebView2CookieManager* manager = CookieManager(surface);
  if (manager == nullptr) return;
  const std::string why = reason;
  manager->GetCookies(
      L"", MakeHandler<ICoreWebView2GetCookiesCompletedHandler, HRESULT, ICoreWebView2CookieList*>(
               [why](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                 if (FAILED(result) || list == nullptr) return S_OK;
                 UINT count = 0;
                 list->get_Count(&count);
                 nlohmann::json cookies = nlohmann::json::array();
                 for (UINT i = 0; i < count; i++) {
                   ICoreWebView2Cookie* cookie = nullptr;
                   if (FAILED(list->GetValueAtIndex(i, &cookie)) || cookie == nullptr) continue;
                   LPWSTR name = nullptr, value = nullptr, domain = nullptr, path = nullptr;
                   BOOL secure = FALSE, http_only = FALSE, session = FALSE;
                   double expires = 0;
                   COREWEBVIEW2_COOKIE_SAME_SITE_KIND same_site = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
                   cookie->get_Name(&name);
                   cookie->get_Value(&value);
                   cookie->get_Domain(&domain);
                   cookie->get_Path(&path);
                   cookie->get_IsSecure(&secure);
                   cookie->get_IsHttpOnly(&http_only);
                   cookie->get_IsSession(&session);
                   cookie->get_Expires(&expires);
                   cookie->get_SameSite(&same_site);
                   const std::string cookie_name = TakeString(name);
                   std::string cookie_value = TakeString(value);
                   if (cookie_name == ".ROBLOSECURITY") {
                     const std::size_t size = cookie_value.size();
                     ClearSensitive(&cookie_value);
                     cookie_value = "<roblosecurity " + std::to_string(size) + " bytes>";
                   }
                   cookies.push_back({{"name", cookie_name}, {"value", cookie_value}, {"domain", TakeString(domain)},
                                      {"path", TakeString(path)}, {"secure", secure != FALSE},
                                      {"http_only", http_only != FALSE}, {"session", session != FALSE},
                                      {"expires", expires}, {"same_site", static_cast<int>(same_site)}});
                   ClearSensitive(&cookie_value);
                   cookie->Release();
                 }
                 TraceWrite("cookies", {{"reason", why}, {"cookies", cookies}});
                 return S_OK;
               }));
  manager->Release();
}

void TraceProcesses(ICoreWebView2Environment* environment, const char* reason) {
  if (!g_trace.on || environment == nullptr) return;
  ICoreWebView2Environment8* environment8 = nullptr;
  if (FAILED(environment->QueryInterface(IID_ICoreWebView2Environment8, reinterpret_cast<void**>(&environment8))) ||
      environment8 == nullptr) {
    return;
  }
  ICoreWebView2ProcessInfoCollection* processes = nullptr;
  nlohmann::json list = nlohmann::json::array();
  if (SUCCEEDED(environment8->GetProcessInfos(&processes)) && processes != nullptr) {
    UINT count = 0;
    processes->get_Count(&count);
    for (UINT i = 0; i < count; i++) {
      ICoreWebView2ProcessInfo* process = nullptr;
      if (FAILED(processes->GetValueAtIndex(i, &process)) || process == nullptr) continue;
      INT32 id = 0;
      COREWEBVIEW2_PROCESS_KIND kind = COREWEBVIEW2_PROCESS_KIND_BROWSER;
      process->get_ProcessId(&id);
      process->get_Kind(&kind);
      list.push_back({{"pid", id}, {"kind", static_cast<int>(kind)}});
      process->Release();
    }
    processes->Release();
  }
  environment8->Release();
  TraceWrite("processes", {{"reason", reason}, {"list", list}});
}

void TraceEnvironment(ICoreWebView2Environment* environment) {
  if (!g_trace.on || environment == nullptr) return;
  LPWSTR version = nullptr;
  environment->get_BrowserVersionString(&version);
  TraceWrite("browser", {{"version", TakeString(version)}});
  TraceProcesses(environment, "ready");
}

// one cdp event straight into the timeline, extra gets the parsed params for follow-up calls
void TraceDevToolsEvent(ICoreWebView2* web_view, const wchar_t* event,
                        std::function<void(ICoreWebView2*, const nlohmann::json&)> extra = nullptr) {
  ICoreWebView2DevToolsProtocolEventReceiver* receiver = nullptr;
  if (FAILED(web_view->GetDevToolsProtocolEventReceiver(event, &receiver)) || receiver == nullptr) return;
  const std::string name = Utf8(event);
  EventRegistrationToken token;
  receiver->add_DevToolsProtocolEventReceived(
      MakeHandler<ICoreWebView2DevToolsProtocolEventReceivedEventHandler, ICoreWebView2*,
                  ICoreWebView2DevToolsProtocolEventReceivedEventArgs*>(
          [name, extra = std::move(extra)](ICoreWebView2* view, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
            std::string session;
            ICoreWebView2DevToolsProtocolEventReceivedEventArgs2* args2 = nullptr;
            if (SUCCEEDED(args->QueryInterface(IID_ICoreWebView2DevToolsProtocolEventReceivedEventArgs2,
                                               reinterpret_cast<void**>(&args2))) &&
                args2 != nullptr) {
              LPWSTR id = nullptr;
              if (SUCCEEDED(args2->get_SessionId(&id))) session = TakeString(id);
              args2->Release();
            }
            LPWSTR json = nullptr;
            if (FAILED(args->get_ParameterObjectAsJson(&json))) return S_OK;
            const nlohmann::json params = nlohmann::json::parse(TakeString(json), nullptr, false);
            nlohmann::json record = {{"e", name}, {"p", params}};
            if (!session.empty()) record["session"] = session;
            TraceWrite("cdp", record);
            if (extra && params.is_object()) extra(view, params);
            return S_OK;
          }),
      &token);
  receiver->Release();
}

bool SkipBody(const std::string& mime) {
  return mime.rfind("image/", 0) == 0 || mime.rfind("font/", 0) == 0 || mime.rfind("video/", 0) == 0 ||
         mime.rfind("audio/", 0) == 0;
}

void TraceNetwork(ICoreWebView2* web_view) {
  TraceDevToolsEvent(web_view, L"Network.requestWillBeSent", [](ICoreWebView2* view, const nlohmann::json& p) {
    const std::string id = p.value("requestId", std::string());
    const nlohmann::json& request = p.value("request", nlohmann::json::object());
    g_trace.requests[id] = {{"url", request.value("url", std::string())}};
    if (!request.value("hasPostData", false) || request.contains("postData")) return;
    const std::wstring query = Wide(nlohmann::json{{"requestId", id}}.dump());
    view->CallDevToolsProtocolMethod(
        L"Network.getRequestPostData", query.c_str(),
        MakeHandler<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, HRESULT, LPCWSTR>(
            [id](HRESULT result, LPCWSTR json) -> HRESULT {
              if (FAILED(result) || json == nullptr) return S_OK;
              const nlohmann::json reply = nlohmann::json::parse(Utf8(json), nullptr, false);
              const std::string data = reply.value("postData", std::string());
              const std::string url = g_trace.requests[id].value("url", std::string());
              TraceWrite("post_data", {{"requestId", id}, {"bytes", data.size()},
                                       {"file", TraceSave(id, url, ".req", TraceRedact(data))}});
              return S_OK;
            }));
  });
  TraceDevToolsEvent(web_view, L"Network.responseReceived", [](ICoreWebView2*, const nlohmann::json& p) {
    const nlohmann::json& response = p.value("response", nlohmann::json::object());
    nlohmann::json& known = g_trace.requests[p.value("requestId", std::string())];
    known["url"] = response.value("url", std::string());
    known["mime"] = response.value("mimeType", std::string());
  });
  TraceDevToolsEvent(web_view, L"Network.loadingFinished", [](ICoreWebView2* view, const nlohmann::json& p) {
    const std::string id = p.value("requestId", std::string());
    const auto found = g_trace.requests.find(id);
    const nlohmann::json known = found != g_trace.requests.end() ? found->second : nlohmann::json::object();
    if (SkipBody(known.value("mime", std::string())) || p.value("encodedDataLength", 0.0) > 64.0 * 1024 * 1024) return;
    const std::wstring query = Wide(nlohmann::json{{"requestId", id}}.dump());
    view->CallDevToolsProtocolMethod(
        L"Network.getResponseBody", query.c_str(),
        MakeHandler<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, HRESULT, LPCWSTR>(
            [id, known](HRESULT result, LPCWSTR json) -> HRESULT {
              if (FAILED(result) || json == nullptr) {
                TraceWrite("body", {{"requestId", id}, {"error", static_cast<unsigned long>(result)}});
                return S_OK;
              }
              const nlohmann::json reply = nlohmann::json::parse(Utf8(json), nullptr, false);
              std::string data = reply.value("body", std::string());
              if (reply.value("base64Encoded", false)) data = DecodeBase64(data);
              TraceWrite("body", {{"requestId", id}, {"url", known.value("url", std::string())},
                                  {"bytes", data.size()},
                                  {"file", TraceSave(id, known.value("url", std::string()), ".body", data)}});
              return S_OK;
            }));
  });
  for (const wchar_t* event :
       {L"Network.requestWillBeSentExtraInfo", L"Network.responseReceivedExtraInfo", L"Network.loadingFailed",
        L"Network.requestServedFromCache", L"Network.resourceChangedPriority", L"Network.dataReceived",
        L"Network.webSocketCreated", L"Network.webSocketWillSendHandshakeRequest",
        L"Network.webSocketHandshakeResponseReceived", L"Network.webSocketFrameSent", L"Network.webSocketFrameReceived",
        L"Network.webSocketFrameError", L"Network.webSocketClosed", L"Network.eventSourceMessageReceived",
        L"Network.signedExchangeReceived", L"Network.trustTokenOperationDone", L"Log.entryAdded",
        L"Target.targetCreated", L"Target.targetInfoChanged", L"Target.targetDestroyed", L"Target.targetCrashed",
        L"Inspector.targetCrashed", L"Inspector.detached"}) {
    TraceDevToolsEvent(web_view, event);
  }
  web_view->CallDevToolsProtocolMethod(
      L"Network.enable", L"{\"maxTotalBufferSize\":268435456,\"maxResourceBufferSize\":67108864,\"maxPostDataSize\":1048576}",
      nullptr);
  web_view->CallDevToolsProtocolMethod(L"Log.enable", L"{}", nullptr);
  web_view->CallDevToolsProtocolMethod(L"Target.setDiscoverTargets", L"{\"discover\":true}", nullptr);
}

void TraceNavigationArgs(nlohmann::json* record, ICoreWebView2NavigationStartingEventArgs* args) {
  LPWSTR uri = nullptr;
  BOOL user = FALSE, redirected = FALSE;
  UINT64 navigation = 0;
  args->get_Uri(&uri);
  args->get_IsUserInitiated(&user);
  args->get_IsRedirected(&redirected);
  args->get_NavigationId(&navigation);
  ICoreWebView2HttpRequestHeaders* headers = nullptr;
  args->get_RequestHeaders(&headers);
  (*record)["uri"] = TakeString(uri);
  (*record)["user"] = user != FALSE;
  (*record)["redirected"] = redirected != FALSE;
  (*record)["navigation"] = navigation;
  (*record)["headers"] = RequestHeaders(headers);
  if (headers != nullptr) headers->Release();
}

void TraceCompletedArgs(nlohmann::json* record, ICoreWebView2NavigationCompletedEventArgs* args) {
  BOOL success = FALSE;
  UINT64 navigation = 0;
  COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
  args->get_IsSuccess(&success);
  args->get_NavigationId(&navigation);
  args->get_WebErrorStatus(&status);
  (*record)["success"] = success != FALSE;
  (*record)["navigation"] = navigation;
  (*record)["error"] = static_cast<int>(status);
  ICoreWebView2NavigationCompletedEventArgs2* args2 = nullptr;
  if (SUCCEEDED(args->QueryInterface(IID_ICoreWebView2NavigationCompletedEventArgs2, reinterpret_cast<void**>(&args2))) &&
      args2 != nullptr) {
    int code = 0;
    args2->get_HttpStatusCode(&code);
    (*record)["http"] = code;
    args2->Release();
  }
}

void TraceFrame(ICoreWebView2Frame* frame) {
  const unsigned long long id = reinterpret_cast<unsigned long long>(frame);
  LPWSTR name = nullptr;
  frame->get_Name(&name);
  TraceWrite("frame", {{"event", "created"}, {"frame", id}, {"name", TakeString(name)}});
  EventRegistrationToken token;
  frame->add_Destroyed(MakeHandler<ICoreWebView2FrameDestroyedEventHandler, ICoreWebView2Frame*, IUnknown*>(
                           [id](ICoreWebView2Frame*, IUnknown*) -> HRESULT {
                             TraceWrite("frame", {{"event", "destroyed"}, {"frame", id}});
                             return S_OK;
                           }),
                       &token);
  ICoreWebView2Frame2* frame2 = nullptr;
  if (FAILED(frame->QueryInterface(IID_ICoreWebView2Frame2, reinterpret_cast<void**>(&frame2))) || frame2 == nullptr) {
    return;
  }
  frame2->add_NavigationStarting(
      MakeHandler<ICoreWebView2FrameNavigationStartingEventHandler, ICoreWebView2Frame*,
                  ICoreWebView2NavigationStartingEventArgs*>(
          [id](ICoreWebView2Frame*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            nlohmann::json record = {{"event", "navigation_starting"}, {"frame", id}};
            TraceNavigationArgs(&record, args);
            TraceWrite("frame", record);
            return S_OK;
          }),
      &token);
  frame2->add_ContentLoading(MakeHandler<ICoreWebView2FrameContentLoadingEventHandler, ICoreWebView2Frame*,
                                         ICoreWebView2ContentLoadingEventArgs*>(
                                 [id](ICoreWebView2Frame*, ICoreWebView2ContentLoadingEventArgs* args) -> HRESULT {
                                   BOOL error_page = FALSE;
                                   args->get_IsErrorPage(&error_page);
                                   TraceWrite("frame", {{"event", "content_loading"}, {"frame", id},
                                                        {"error_page", error_page != FALSE}});
                                   return S_OK;
                                 }),
                             &token);
  frame2->add_DOMContentLoaded(MakeHandler<ICoreWebView2FrameDOMContentLoadedEventHandler, ICoreWebView2Frame*,
                                           ICoreWebView2DOMContentLoadedEventArgs*>(
                                   [id](ICoreWebView2Frame*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
                                     TraceWrite("frame", {{"event", "dom_content_loaded"}, {"frame", id}});
                                     return S_OK;
                                   }),
                               &token);
  frame2->add_NavigationCompleted(
      MakeHandler<ICoreWebView2FrameNavigationCompletedEventHandler, ICoreWebView2Frame*,
                  ICoreWebView2NavigationCompletedEventArgs*>(
          [id](ICoreWebView2Frame*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            nlohmann::json record = {{"event", "navigation_completed"}, {"frame", id}};
            TraceCompletedArgs(&record, args);
            TraceWrite("frame", record);
            return S_OK;
          }),
      &token);
  frame2->Release();
}

void TraceWebView(Surface* surface) {
  if (!g_trace.on) return;
  ICoreWebView2* web_view = surface->web_view;
  const bool primary = surface->primary;
  TraceWrite("surface", {{"event", "attached"}, {"primary", primary}});
  TraceNetwork(web_view);
  EventRegistrationToken token;
  web_view->add_NavigationStarting(
      MakeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
          [primary](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            nlohmann::json record = {{"event", "navigation_starting"}, {"primary", primary}};
            TraceNavigationArgs(&record, args);
            TraceWrite("nav", record);
            return S_OK;
          }),
      &token);
  web_view->add_ContentLoading(
      MakeHandler<ICoreWebView2ContentLoadingEventHandler, ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs*>(
          [primary](ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs* args) -> HRESULT {
            BOOL error_page = FALSE;
            UINT64 navigation = 0;
            args->get_IsErrorPage(&error_page);
            args->get_NavigationId(&navigation);
            TraceWrite("nav", {{"event", "content_loading"}, {"primary", primary}, {"navigation", navigation},
                               {"error_page", error_page != FALSE}});
            return S_OK;
          }),
      &token);
  web_view->add_SourceChanged(
      MakeHandler<ICoreWebView2SourceChangedEventHandler, ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*>(
          [primary](ICoreWebView2* view, ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
            BOOL new_document = FALSE;
            args->get_IsNewDocument(&new_document);
            LPWSTR uri = nullptr;
            view->get_Source(&uri);
            TraceWrite("nav", {{"event", "source_changed"}, {"primary", primary}, {"uri", TakeString(uri)},
                               {"new_document", new_document != FALSE}});
            return S_OK;
          }),
      &token);
  web_view->add_HistoryChanged(MakeHandler<ICoreWebView2HistoryChangedEventHandler, ICoreWebView2*, IUnknown*>(
                                   [primary](ICoreWebView2* view, IUnknown*) -> HRESULT {
                                     BOOL back = FALSE, forward = FALSE;
                                     view->get_CanGoBack(&back);
                                     view->get_CanGoForward(&forward);
                                     TraceWrite("nav", {{"event", "history_changed"}, {"primary", primary},
                                                        {"back", back != FALSE}, {"forward", forward != FALSE}});
                                     return S_OK;
                                   }),
                               &token);
  web_view->add_NavigationCompleted(
      MakeHandler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
          [surface, primary](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            nlohmann::json record = {{"event", "navigation_completed"}, {"primary", primary}};
            TraceCompletedArgs(&record, args);
            TraceWrite("nav", record);
            TraceCookies(surface, "navigation_completed");
            return S_OK;
          }),
      &token);
  web_view->add_DocumentTitleChanged(MakeHandler<ICoreWebView2DocumentTitleChangedEventHandler, ICoreWebView2*, IUnknown*>(
                                         [primary](ICoreWebView2* view, IUnknown*) -> HRESULT {
                                           LPWSTR title = nullptr;
                                           view->get_DocumentTitle(&title);
                                           TraceWrite("nav", {{"event", "title"}, {"primary", primary},
                                                              {"title", TakeString(title)}});
                                           return S_OK;
                                         }),
                                     &token);
  web_view->add_ContainsFullScreenElementChanged(
      MakeHandler<ICoreWebView2ContainsFullScreenElementChangedEventHandler, ICoreWebView2*, IUnknown*>(
          [](ICoreWebView2* view, IUnknown*) -> HRESULT {
            BOOL full = FALSE;
            view->get_ContainsFullScreenElement(&full);
            TraceWrite("page", {{"event", "fullscreen"}, {"on", full != FALSE}});
            return S_OK;
          }),
      &token);
  web_view->add_PermissionRequested(
      MakeHandler<ICoreWebView2PermissionRequestedEventHandler, ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs*>(
          [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
            BOOL user = FALSE;
            args->get_Uri(&uri);
            args->get_PermissionKind(&kind);
            args->get_IsUserInitiated(&user);
            TraceWrite("page", {{"event", "permission"}, {"uri", TakeString(uri)}, {"kind", static_cast<int>(kind)},
                                {"user", user != FALSE}});
            return S_OK;
          }),
      &token);
  web_view->add_NewWindowRequested(
      MakeHandler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
          [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            BOOL user = FALSE;
            args->get_Uri(&uri);
            args->get_IsUserInitiated(&user);
            TraceWrite("page", {{"event", "new_window"}, {"uri", TakeString(uri)}, {"user", user != FALSE}});
            return S_OK;
          }),
      &token);
  web_view->add_WindowCloseRequested(MakeHandler<ICoreWebView2WindowCloseRequestedEventHandler, ICoreWebView2*, IUnknown*>(
                                         [primary](ICoreWebView2*, IUnknown*) -> HRESULT {
                                           TraceWrite("page", {{"event", "close_requested"}, {"primary", primary}});
                                           return S_OK;
                                         }),
                                     &token);
  web_view->add_ProcessFailed(
      MakeHandler<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
          [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            args->get_ProcessFailedKind(&kind);
            nlohmann::json record = {{"event", "process_failed"}, {"kind", static_cast<int>(kind)}};
            ICoreWebView2ProcessFailedEventArgs2* args2 = nullptr;
            if (SUCCEEDED(args->QueryInterface(IID_ICoreWebView2ProcessFailedEventArgs2,
                                               reinterpret_cast<void**>(&args2))) &&
                args2 != nullptr) {
              COREWEBVIEW2_PROCESS_FAILED_REASON reason = COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED;
              int exit_code = 0;
              LPWSTR description = nullptr;
              args2->get_Reason(&reason);
              args2->get_ExitCode(&exit_code);
              args2->get_ProcessDescription(&description);
              record["reason"] = static_cast<int>(reason);
              record["exit_code"] = exit_code;
              record["description"] = TakeString(description);
              args2->Release();
            }
            TraceWrite("page", record);
            TraceProcesses(g_app.environment, "process_failed");
            return S_OK;
          }),
      &token);
  web_view->add_WebMessageReceived(
      MakeHandler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
          [primary](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR source = nullptr, json = nullptr;
            args->get_Source(&source);
            args->get_WebMessageAsJson(&json);
            std::string text = TakeString(json);
            TraceWrite("bridge_in", {{"primary", primary}, {"source", TakeString(source)},
                                     {"message", nlohmann::json::parse(text, nullptr, false)}});
            ClearSensitive(&text);
            return S_OK;
          }),
      &token);
  ICoreWebView2_2* web_view2 = nullptr;
  if (SUCCEEDED(web_view->QueryInterface(IID_ICoreWebView2_2, reinterpret_cast<void**>(&web_view2))) && web_view2 != nullptr) {
    web_view2->add_DOMContentLoaded(
        MakeHandler<ICoreWebView2DOMContentLoadedEventHandler, ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*>(
            [primary](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs* args) -> HRESULT {
              UINT64 navigation = 0;
              args->get_NavigationId(&navigation);
              TraceWrite("nav", {{"event", "dom_content_loaded"}, {"primary", primary}, {"navigation", navigation}});
              return S_OK;
            }),
        &token);
    web_view2->add_WebResourceResponseReceived(
        MakeHandler<ICoreWebView2WebResourceResponseReceivedEventHandler, ICoreWebView2*,
                    ICoreWebView2WebResourceResponseReceivedEventArgs*>(
            [](ICoreWebView2*, ICoreWebView2WebResourceResponseReceivedEventArgs* args) -> HRESULT {
              ICoreWebView2WebResourceRequest* request = nullptr;
              ICoreWebView2WebResourceResponseView* response = nullptr;
              args->get_Request(&request);
              args->get_Response(&response);
              nlohmann::json record;
              if (request != nullptr) {
                LPWSTR uri = nullptr, method = nullptr;
                ICoreWebView2HttpRequestHeaders* headers = nullptr;
                request->get_Uri(&uri);
                request->get_Method(&method);
                request->get_Headers(&headers);
                record["uri"] = TakeString(uri);
                record["method"] = TakeString(method);
                record["request_headers"] = RequestHeaders(headers);
                if (headers != nullptr) headers->Release();
                request->Release();
              }
              if (response != nullptr) {
                int status = 0;
                LPWSTR reason = nullptr;
                ICoreWebView2HttpResponseHeaders* headers = nullptr;
                response->get_StatusCode(&status);
                response->get_ReasonPhrase(&reason);
                response->get_Headers(&headers);
                record["status"] = status;
                record["reason"] = TakeString(reason);
                record["response_headers"] = ResponseHeaders(headers);
                if (headers != nullptr) headers->Release();
                response->Release();
              }
              TraceWrite("resource", record);
              return S_OK;
            }),
        &token);
    web_view2->Release();
  }
  ICoreWebView2_4* web_view4 = nullptr;
  if (SUCCEEDED(web_view->QueryInterface(IID_ICoreWebView2_4, reinterpret_cast<void**>(&web_view4))) && web_view4 != nullptr) {
    web_view4->add_FrameCreated(
        MakeHandler<ICoreWebView2FrameCreatedEventHandler, ICoreWebView2*, ICoreWebView2FrameCreatedEventArgs*>(
            [](ICoreWebView2*, ICoreWebView2FrameCreatedEventArgs* args) -> HRESULT {
              ICoreWebView2Frame* frame = nullptr;
              if (SUCCEEDED(args->get_Frame(&frame)) && frame != nullptr) {
                TraceFrame(frame);
                frame->Release();
              }
              return S_OK;
            }),
        &token);
    web_view4->Release();
  }
  ICoreWebView2_8* web_view8 = nullptr;
  if (SUCCEEDED(web_view->QueryInterface(IID_ICoreWebView2_8, reinterpret_cast<void**>(&web_view8))) && web_view8 != nullptr) {
    web_view8->add_IsDocumentPlayingAudioChanged(
        MakeHandler<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler, ICoreWebView2*, IUnknown*>(
            [](ICoreWebView2* view, IUnknown*) -> HRESULT {
              BOOL playing = FALSE;
              ICoreWebView2_8* view8 = nullptr;
              if (SUCCEEDED(view->QueryInterface(IID_ICoreWebView2_8, reinterpret_cast<void**>(&view8))) && view8) {
                view8->get_IsDocumentPlayingAudio(&playing);
                view8->Release();
              }
              TraceWrite("page", {{"event", "audio"}, {"playing", playing != FALSE}});
              return S_OK;
            }),
        &token);
    web_view8->Release();
  }
  ICoreWebView2Controller* controller = surface->controller;
  controller->add_GotFocus(MakeHandler<ICoreWebView2FocusChangedEventHandler, ICoreWebView2Controller*, IUnknown*>(
                               [primary](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                                 TraceWrite("focus", {{"event", "got"}, {"primary", primary}});
                                 return S_OK;
                               }),
                           &token);
  controller->add_LostFocus(MakeHandler<ICoreWebView2FocusChangedEventHandler, ICoreWebView2Controller*, IUnknown*>(
                                [primary](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                                  TraceWrite("focus", {{"event", "lost"}, {"primary", primary}});
                                  return S_OK;
                                }),
                            &token);
  controller->add_ZoomFactorChanged(MakeHandler<ICoreWebView2ZoomFactorChangedEventHandler, ICoreWebView2Controller*, IUnknown*>(
                                        [](ICoreWebView2Controller* sender, IUnknown*) -> HRESULT {
                                          double zoom = 0;
                                          sender->get_ZoomFactor(&zoom);
                                          TraceWrite("focus", {{"event", "zoom"}, {"zoom", zoom}});
                                          return S_OK;
                                        }),
                                    &token);
  ICoreWebView2Controller3* controller3 = nullptr;
  if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller3, reinterpret_cast<void**>(&controller3))) &&
      controller3 != nullptr) {
    double scale = 0;
    controller3->get_RasterizationScale(&scale);
    TraceWrite("surface", {{"event", "rasterization_scale"}, {"scale", scale}});
    controller3->Release();
  }
  TraceCookies(surface, "attached");
}

std::string ControlName(WebViewHelperControlOperation operation) {
  using Op = WebViewHelperControlOperation;
  switch (operation) {
    case Op::kLoadUrl: return "load_url";
    case Op::kSetTitle: return "set_title";
    case Op::kSetVisible: return "set_visible";
    case Op::kEvaluateJavaScript: return "evaluate_javascript";
    case Op::kClose: return "close";
    case Op::kSetBackNavigationDisabled: return "set_back_navigation_disabled";
    case Op::kSetShowDomainAsTitle: return "set_show_domain_as_title";
    case Op::kSetRobloxCookie: return "set_roblox_cookie";
    case Op::kClearRobloxCookie: return "clear_roblox_cookie";
    case Op::kRetainPersistentRobloxCookie: return "retain_persistent_roblox_cookie";
  }
  return "op" + std::to_string(static_cast<int>(operation));
}

void TraceControl(std::string_view packet) {
  if (!g_trace.on) return;
  if (packet.size() < kHeaderBytes) {
    TraceWrite("control", {{"error", "short packet"}, {"bytes", packet.size()}});
    return;
  }
  const auto operation = static_cast<WebViewHelperControlOperation>(static_cast<unsigned char>(packet[5]));
  const std::string_view payload = packet.substr(kHeaderBytes);
  nlohmann::json record = {{"op", ControlName(operation)}, {"bytes", payload.size()}};
  if (operation == WebViewHelperControlOperation::kSetRobloxCookie) {
    record["payload"] = "<roblosecurity " + std::to_string(payload.size()) + " bytes>";
  } else if (operation == WebViewHelperControlOperation::kSetVisible ||
             operation == WebViewHelperControlOperation::kSetBackNavigationDisabled ||
             operation == WebViewHelperControlOperation::kSetShowDomainAsTitle) {
    record["flag"] = !payload.empty() && payload[0] != '\0';
  } else {
    record["payload"] = std::string(payload);
  }
  TraceWrite("control", record);
}

void TraceEvent(WebViewHelperEventType type, std::string_view payload, bool sent) {
  if (!g_trace.on) return;
  const bool cookie = type == WebViewHelperEventType::kRobloxCookie;
  TraceWrite("event_out", {{"type", static_cast<int>(type)}, {"sent", sent}, {"bytes", payload.size()},
                           {"payload", cookie ? "<roblosecurity " + std::to_string(payload.size()) + " bytes>"
                                              : std::string(payload)}});
}

std::string EnvironmentDump() {
  nlohmann::json values = nlohmann::json::object();
  LPWCH block = GetEnvironmentStringsW();
  for (LPWCH entry = block; entry != nullptr && *entry != 0; entry += wcslen(entry) + 1) {
    const std::string line = Utf8(entry);
    const std::size_t equals = line.find('=', 1);
    if (equals == std::string::npos) continue;
    const std::string name = line.substr(0, equals);
    if (name.rfind("BLOX7_", 0) != 0 && name.rfind("WEBVIEW2_", 0) != 0) continue;
    std::string upper = name;
    for (char& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    const bool secret = upper.find("TOKEN") != std::string::npos || upper.find("COOKIE") != std::string::npos ||
                        upper.find("SECRET") != std::string::npos || upper.find("PASSWORD") != std::string::npos;
    values[name] = secret ? std::string("<redacted>") : line.substr(equals + 1);
  }
  if (block != nullptr) FreeEnvironmentStringsW(block);
  return values.dump();
}

void TraceStart() {
  const char* value = getenv("BLOX7_WEBVIEW_TRACE");
  if (value == nullptr || *value == '\0' || strcmp(value, "0") == 0) return;
  QueryPerformanceFrequency(&g_trace.frequency);
  QueryPerformanceCounter(&g_trace.origin);
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  g_trace.origin_filetime = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  if (strcmp(value, "1") == 0) {
    SYSTEMTIME local;
    GetLocalTime(&local);
    wchar_t stamp[32];
    _snwprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", local.wYear, local.wMonth, local.wDay, local.wHour,
               local.wMinute, local.wSecond);
    g_trace.dir = g_app.data_directory + L"\\trace-" + stamp;
  } else {
    g_trace.dir = Wide(value);
  }
  while (!g_trace.dir.empty() && (g_trace.dir.back() == L'\\' || g_trace.dir.back() == L'/')) g_trace.dir.pop_back();
  MakeDirectories(g_trace.dir + L"\\bodies");
  g_trace.file = _wfopen((g_trace.dir + L"\\helper-trace.jsonl").c_str(), L"ab");
  if (g_trace.file == nullptr) return;
  setvbuf(g_trace.file, nullptr, _IOFBF, 1 << 16);
  g_trace.on = true;

  OSVERSIONINFOEXW os{sizeof os};
  if (auto get = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOEXW*)>(
          GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
    get(&os);
  }
  TIME_ZONE_INFORMATION zone{};
  const DWORD zone_id = GetTimeZoneInformation(&zone);
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {}, system_locale[LOCALE_NAME_MAX_LENGTH] = {};
  GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
  GetSystemDefaultLocaleName(system_locale, LOCALE_NAME_MAX_LENGTH);
  MEMORYSTATUSEX memory{sizeof memory};
  GlobalMemoryStatusEx(&memory);
  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  HDC screen = GetDC(nullptr);
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode);
  std::string command_line = Utf8(GetCommandLineW());
  if (const std::size_t token = command_line.find("--token "); token != std::string::npos) {
    const std::size_t start = token + 8, end = command_line.find(' ', start);
    command_line.replace(start, (end == std::string::npos ? command_line.size() : end) - start, "<redacted>");
  }
  TraceWrite("start", {{"dir", Utf8(g_trace.dir.c_str())},
                       {"pid", GetCurrentProcessId()},
                       {"command_line", command_line},
                       {"os", std::to_string(os.dwMajorVersion) + "." + std::to_string(os.dwMinorVersion) + "." +
                                  std::to_string(os.dwBuildNumber) + " sp" + std::to_string(os.wServicePackMajor)},
                       {"timezone", {{"id", zone_id}, {"bias", zone.Bias}, {"standard", Utf8(zone.StandardName)},
                                     {"daylight", Utf8(zone.DaylightName)}, {"daylight_bias", zone.DaylightBias}}},
                       {"locale", Utf8(locale)},
                       {"system_locale", Utf8(system_locale)},
                       {"ui_language", GetUserDefaultUILanguage()},
                       {"memory_mb", memory.ullTotalPhys / (1024 * 1024)},
                       {"cpus", system.dwNumberOfProcessors},
                       {"dpi", dpi},
                       {"display", {mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency, mode.dmBitsPerPel}},
                       {"metrics", {{"digitizer", GetSystemMetrics(SM_DIGITIZER)},
                                    {"max_touches", GetSystemMetrics(SM_MAXIMUMTOUCHES)},
                                    {"mouse_present", GetSystemMetrics(SM_MOUSEPRESENT)},
                                    {"mouse_buttons", GetSystemMetrics(SM_CMOUSEBUTTONS)},
                                    {"double_click_ms", GetDoubleClickTime()}}},
                       {"qpc_frequency", g_trace.frequency.QuadPart},
                       {"environment", nlohmann::json::parse(EnvironmentDump(), nullptr, false)}});
  g_trace.mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, TraceMouseHook, GetModuleHandleW(nullptr), 0);
  TraceWrite("mouse_hook", {{"installed", g_trace.mouse_hook != nullptr}});
}

std::wstring TraceBrowserArguments() {
  if (!g_trace.on) return {};
  return L"--log-net-log=\"" + g_trace.dir + L"\\netlog.json\" --net-log-capture-mode=Everything --enable-logging --v=0 "
         L"--log-file=\"" + g_trace.dir + L"\\chromium.log\"";
}

void TraceStop(const char* reason) {
  if (!g_trace.on) return;
  TraceWrite("stop", {{"reason", reason}});
  if (g_trace.mouse_hook != nullptr) UnhookWindowsHookEx(g_trace.mouse_hook);
  g_trace.mouse_hook = nullptr;
  std::lock_guard<std::mutex> hold(g_trace.lock);
  fflush(g_trace.file);
}

bool SendFrame(std::string_view packet) {
  std::lock_guard<std::mutex> lock(g_app.send_lock);
  if (g_app.control == INVALID_SOCKET) return false;
  std::string framed(4, '\0');
  WriteBigEndian32(framed.data(), static_cast<uint32_t>(packet.size()));
  framed.append(packet);
  std::size_t sent = 0;
  while (sent < framed.size()) {
    int n = send(g_app.control, framed.data() + sent, static_cast<int>(framed.size() - sent), 0);
    if (n <= 0) {
      ClearSensitive(&framed);
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  ClearSensitive(&framed);
  return true;
}

bool SendEvent(WebViewHelperEventType type, std::string_view payload) {
  const bool cookie = type == WebViewHelperEventType::kRobloxCookie;
  if ((type != WebViewHelperEventType::kReady && payload.empty()) ||
      payload.size() > (cookie ? blox7::runtime::kMaximumWebViewCookieBytes
                               : blox7::runtime::kMaximumWebViewHybridEventBytes) ||
      payload.find('\0') != std::string_view::npos) {
    return false;
  }
  std::string packet(kHeaderBytes, '\0');
  std::memcpy(packet.data(), kEventMagic, 4);
  packet[4] = static_cast<char>(kProtocolVersion);
  packet[5] = static_cast<char>(type);
  WriteBigEndian32(packet.data() + 8, static_cast<uint32_t>(payload.size()));
  packet.append(payload);
  const bool sent = SendFrame(packet);
  ClearSensitive(&packet);
  TraceEvent(type, payload, sent);
  return sent;
}

DWORD WINAPI ControlReader(void*) {
  std::string buffer;
  char chunk[8192];
  for (;;) {
    const int n = recv(g_app.control, chunk, sizeof chunk, 0);
    if (n <= 0) break;
    buffer.append(chunk, static_cast<std::size_t>(n));
    while (buffer.size() >= 4) {
      const uint32_t size = ReadBigEndian32(buffer.data());
      if (size > blox7::runtime::kMaximumWebViewControlPacketBytes) {
        Log("rejected oversized control frame");
        PostMessageW(g_app.message_window, kControlClosed, 0, 0);
        return 0;
      }
      if (buffer.size() < 4 + std::size_t{size}) break;
      auto* packet = new std::string(buffer, 4, size);
      buffer.erase(0, 4 + std::size_t{size});
      if (!PostMessageW(g_app.message_window, kControlMessage, 0, reinterpret_cast<LPARAM>(packet))) {
        ClearSensitive(packet);
        delete packet;
      }
    }
  }
  ClearSensitive(&buffer);
  PostMessageW(g_app.message_window, kControlClosed, 0, 0);
  return 0;
}

struct ControlCommand {
  WebViewHelperControlOperation operation = WebViewHelperControlOperation::kClose;
  std::string payload;
  bool flag = false;
};

bool IsCookieOctet(unsigned char byte) {
  return byte == 0x21 || (byte >= 0x23 && byte <= 0x2b) || (byte >= 0x2d && byte <= 0x3a) ||
         (byte >= 0x3c && byte <= 0x5b) || (byte >= 0x5d && byte <= 0x7e);
}

bool HasControlBytes(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

bool DecodeControl(std::string_view packet, ControlCommand* command, const char** error) {
  if (packet.size() < kHeaderBytes || packet.compare(0, 4, kControlMagic) != 0 ||
      static_cast<unsigned char>(packet[4]) != kProtocolVersion || packet[6] != '\0' || packet[7] != '\0') {
    *error = "invalid header";
    return false;
  }
  if (ReadBigEndian32(packet.data() + 8) != packet.size() - kHeaderBytes) {
    *error = "invalid payload length";
    return false;
  }
  const auto operation = static_cast<WebViewHelperControlOperation>(static_cast<unsigned char>(packet[5]));
  const std::string_view payload = packet.substr(kHeaderBytes);
  using Op = WebViewHelperControlOperation;
  switch (operation) {
    case Op::kLoadUrl: {
      const auto policy = EvaluateNavigationUri(std::string(payload).c_str());
      if (payload.empty() || payload.size() > blox7::runtime::kMaximumWebViewUrlBytes || HasControlBytes(payload) ||
          !policy.privileged_bridge_allowed) {
        *error = "URL is not an HTTPS roblox.com address";
        return false;
      }
      break;
    }
    case Op::kSetTitle:
      if (payload.size() > blox7::runtime::kMaximumWebViewTitleBytes || HasControlBytes(payload)) {
        *error = "title is oversized or unsafe";
        return false;
      }
      break;
    case Op::kSetVisible:
    case Op::kSetBackNavigationDisabled:
    case Op::kSetShowDomainAsTitle:
      if (payload.size() != 1 || (payload[0] != 0 && payload[0] != 1)) {
        *error = "boolean payload is invalid";
        return false;
      }
      break;
    case Op::kEvaluateJavaScript:
      if (payload.size() > blox7::runtime::kMaximumWebViewJavaScriptBytes ||
          payload.find('\0') != std::string_view::npos) {
        *error = "JavaScript is oversized or contains NUL";
        return false;
      }
      break;
    case Op::kSetRobloxCookie:
      if (payload.empty() || payload.size() > blox7::runtime::kMaximumWebViewCookieBytes ||
          !std::all_of(payload.begin(), payload.end(), [](char c) { return IsCookieOctet(static_cast<unsigned char>(c)); })) {
        *error = "Roblox cookie is oversized or unsafe";
        return false;
      }
      break;
    case Op::kClearRobloxCookie:
    case Op::kRetainPersistentRobloxCookie:
    case Op::kClose:
      if (!payload.empty()) {
        *error = "payload must be empty";
        return false;
      }
      break;
    default:
      *error = "unsupported operation";
      return false;
  }
  command->operation = operation;
  command->payload.assign(payload);
  command->flag = !payload.empty() && payload[0] != '\0';
  return true;
}

ICoreWebView2CookieManager* CookieManager(Surface* surface) {
  if (surface == nullptr || surface->web_view == nullptr) return nullptr;
  ICoreWebView2_2* web_view2 = nullptr;
  if (FAILED(surface->web_view->QueryInterface(IID_ICoreWebView2_2, reinterpret_cast<void**>(&web_view2)))) return nullptr;
  ICoreWebView2CookieManager* manager = nullptr;
  web_view2->get_CookieManager(&manager);
  web_view2->Release();
  return manager;
}

void RequestRobloxCookie() {
  if (!g_app.browser_login_mode || g_app.terminating) return;
  ICoreWebView2CookieManager* manager = CookieManager(g_app.primary);
  if (manager == nullptr) return;
  manager->GetCookies(
      L"https://www.roblox.com/",
      MakeHandler<ICoreWebView2GetCookiesCompletedHandler, HRESULT, ICoreWebView2CookieList*>(
          [](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
            if (FAILED(result) || list == nullptr || !g_app.browser_login_mode) return S_OK;
            UINT count = 0;
            list->get_Count(&count);
            std::string candidate;
            for (UINT i = 0; i < count && candidate.empty(); i++) {
              ICoreWebView2Cookie* cookie = nullptr;
              if (FAILED(list->GetValueAtIndex(i, &cookie)) || cookie == nullptr) continue;
              LPWSTR name = nullptr, value = nullptr, domain = nullptr, path = nullptr;
              BOOL secure = FALSE, http_only = FALSE;
              cookie->get_Name(&name);
              cookie->get_Value(&value);
              cookie->get_Domain(&domain);
              cookie->get_Path(&path);
              cookie->get_IsSecure(&secure);
              cookie->get_IsHttpOnly(&http_only);
              const std::string n = TakeString(name), d = TakeString(domain), p = TakeString(path);
              std::string v = TakeString(value);
              const bool roblox_domain = d == ".roblox.com" || d == "roblox.com" || d == "www.roblox.com";
              if (n == ".ROBLOSECURITY" && p == "/" && roblox_domain && secure && http_only && !v.empty() &&
                  v.size() <= blox7::runtime::kMaximumWebViewCookieBytes &&
                  std::all_of(v.begin(), v.end(), [](char c) { return IsCookieOctet(static_cast<unsigned char>(c)); })) {
                candidate = v;
              }
              ClearSensitive(&v);
              cookie->Release();
            }
            if (!candidate.empty() && candidate != g_app.last_reported_cookie) {
              if (SendEvent(WebViewHelperEventType::kRobloxCookie, candidate)) {
                ClearSensitive(&g_app.last_reported_cookie);
                g_app.last_reported_cookie = candidate;
                Log("browser sign-in session captured");
              } else {
                Log("could not forward browser sign-in session");
              }
            }
            ClearSensitive(&candidate);
            return S_OK;
          }));
  manager->Release();
}

void StartInitialLoad() {
  if (!g_app.cookie_sync_received || g_app.primary == nullptr || g_app.primary->web_view == nullptr ||
      g_app.pending_url.empty()) {
    return;
  }
  g_app.browser_login_mode = blox7::webview::IsBrowserLoginUrl(g_app.pending_url);
  if (g_app.browser_login_mode == g_app.app_user_agent) {
    Log("this page loads with the user agent of the first one (%s)", g_app.app_user_agent ? "app" : "browser");
  }
  if (g_app.browser_login_mode) {
    SetTimer(g_app.message_window, kCookieTimer, 1000, nullptr);
    RequestRobloxCookie();
  } else {
    KillTimer(g_app.message_window, kCookieTimer);
  }
  g_app.primary->web_view->Navigate(Wide(g_app.pending_url).c_str());
  ClearSensitive(&g_app.pending_url);
}

void ApplyPendingCookie() {
  if (!g_app.cookie_sync_received || g_app.primary == nullptr || g_app.primary->web_view == nullptr) return;
  const CookieOperation operation = g_app.cookie_operation;
  g_app.cookie_operation = CookieOperation::kNone;
  if (operation == CookieOperation::kNone || operation == CookieOperation::kRetainPersistent) {
    if (operation == CookieOperation::kRetainPersistent) Log("Roblox cookies retained before guest navigation");
    StartInitialLoad();
    return;
  }
  ICoreWebView2CookieManager* manager = CookieManager(g_app.primary);
  if (manager == nullptr) {
    BeginTermination("cookie manager unavailable");
    return;
  }
  if (operation == CookieOperation::kClear) {
    const bool ok = SUCCEEDED(manager->DeleteCookiesWithDomainAndPath(L".ROBLOSECURITY", L".roblox.com", L"/"));
    manager->Release();
    if (!ok) {
      BeginTermination("stale Roblox cookie deletion failed");
      return;
    }
    Log("Roblox cookie deleted before navigation");
    StartInitialLoad();
    return;
  }
  ICoreWebView2Cookie* cookie = nullptr;
  std::wstring value = Wide(g_app.pending_cookie);
  ClearSensitive(&g_app.pending_cookie);
  bool ok = SUCCEEDED(manager->CreateCookie(L".ROBLOSECURITY", value.c_str(), L".roblox.com", L"/", &cookie)) &&
            cookie != nullptr;
  std::fill(value.begin(), value.end(), L'\0');
  if (ok) {
    cookie->put_IsSecure(TRUE);
    cookie->put_IsHttpOnly(TRUE);
    cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX);
    ok = SUCCEEDED(manager->AddOrUpdateCookie(cookie));
    cookie->Release();
  }
  manager->Release();
  if (!ok) {
    BeginTermination("Roblox cookie installation failed");
    return;
  }
  Log("Roblox cookie installed before navigation");
  StartInitialLoad();
}

void ConfirmCallback(Surface* surface, const CaptchaEvent& event) {
  if (event.callback_id.empty()) {
    Log("hybrid callback not confirmed feature=%s reason=missing-id", blox7::webview::CaptchaEventName(event.type));
    return;
  }
  const std::string script = blox7::webview::BuildCallbackScript(event.callback_id);
  const CaptchaEventType type = event.type;
  HWND window = surface->window;
  TraceWrite("callback", {{"feature", blox7::webview::CaptchaEventName(type)}, {"script", script}});
  surface->web_view->ExecuteScript(
      Wide(script).c_str(),
      MakeHandler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
          [type, window](HRESULT result, LPCWSTR json) -> HRESULT {
            const bool confirmed = SUCCEEDED(result) && json != nullptr && std::wstring_view(json) == L"true";
            TraceWrite("callback_result", {{"feature", blox7::webview::CaptchaEventName(type)},
                                           {"hr", static_cast<unsigned long>(result)},
                                           {"result", json != nullptr ? Utf8(json) : std::string()},
                                           {"closes_window", confirmed && type == CaptchaEventType::kSuccess}});
            if (FAILED(result)) {
              Log("hybrid callback failed feature=%s code=0x%08lx", blox7::webview::CaptchaEventName(type),
                  static_cast<unsigned long>(result));
            } else {
              Log("hybrid callback feature=%s confirmed=%s", blox7::webview::CaptchaEventName(type),
                  confirmed ? "yes" : "no");
            }
            if (confirmed && type == CaptchaEventType::kSuccess && IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
            return S_OK;
          }));
}

void HandleHybridCommand(Surface* surface, const std::string& command) {
  CaptchaEvent event;
  if (!blox7::webview::ParseCaptchaEvent(command, &event)) return;
  Log("hybrid event feature=%s callback=%s", blox7::webview::CaptchaEventName(event.type),
      event.callback_id.empty() ? "missing" : "yes");
  ConfirmCallback(surface, event);
}

void LogBridgeCommand(const std::string& handler, const std::string& command) {
  const nlohmann::json message = nlohmann::json::parse(command, nullptr, false);
  auto text = [&](const nlohmann::json& object, const char* key) -> std::string {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) return "-";
    std::string value = object[key].get<std::string>();
    if (value.size() > 64) value.resize(64);
    for (char& c : value) {
      if (static_cast<unsigned char>(c) < 0x20) c = '?';
    }
    return value;
  };
  std::string feature = "-";
  if (message.is_object() && message.contains("params") && message["params"].is_object()) {
    const nlohmann::json& params = message["params"];
    feature = text(params.contains("params") ? params["params"] : params, "feature");
  }
  Log("bridge %s %s.%s feature=%s callback=%s", handler.c_str(), text(message, "moduleID").c_str(),
      text(message, "functionName").c_str(), feature.c_str(),
      message.is_object() && message.contains("callbackID") ? "yes" : "no");
}

void OnBridgeMessage(Surface* surface, ICoreWebView2WebMessageReceivedEventArgs* args) {
  LPWSTR source = nullptr;
  args->get_Source(&source);
  const std::string origin = TakeString(source);
  if (!EvaluateNavigationUri(origin.c_str()).privileged_bridge_allowed) {
    Log("rejected bridge message from untrusted origin");
    return;
  }
  LPWSTR json_text = nullptr;
  if (FAILED(args->get_WebMessageAsJson(&json_text))) return;
  std::string text = TakeString(json_text);
  const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
  ClearSensitive(&text);
  if (!message.is_object() || !message.contains("handler") || !message["handler"].is_string()) return;
  const std::string handler = message["handler"].get<std::string>();
  const nlohmann::json& value = message.contains("message") ? message["message"] : nlohmann::json();
  std::string command;
  WebViewHelperEventType type = WebViewHelperEventType::kExecuteRoblox;
  if (handler == blox7::webview::kExecuteRobloxHandler) {
    if (value.is_string()) {
      command = value.get<std::string>();
      if (!nlohmann::json::parse(command, nullptr, false).is_object()) {
        Log("rejected executeRoblox command that is not a json object");
        return;
      }
    } else if (value.is_object()) {
      command = value.dump();
    } else {
      Log("rejected executeRoblox object command");
      return;
    }
  } else if (handler == blox7::webview::kRobloxWkHybridHandler) {
    if (!value.is_object() || !value.contains("command") || !value["command"].is_string()) {
      Log("rejected RobloxWKHybrid command envelope");
      return;
    }
    command = value["command"].get<std::string>();
    type = WebViewHelperEventType::kRobloxWkHybrid;
  } else if (handler == blox7::webview::kCompatibilityHandler) {
    if (!value.is_string()) {
      Log("rejected compatibility bridge command");
      return;
    }
    command = value.get<std::string>();
  } else {
    return;
  }
  if (command.empty() || command.size() > blox7::webview::kMaximumHybridCommandBytes) {
    Log("rejected empty or oversized bridge command");
    return;
  }
  LogBridgeCommand(handler, command);
  if (!SendEvent(type, command)) Log("could not forward %s command", handler.c_str());
  HandleHybridCommand(surface, command);
  ClearSensitive(&command);
}

void UpdateDomainTitle(Surface* surface) {
  if (!g_app.show_domain_as_title || surface->web_view == nullptr) return;
  LPWSTR uri = nullptr;
  surface->web_view->get_Source(&uri);
  const auto policy = EvaluateNavigationUri(TakeString(uri).c_str());
  if (policy.allowed && !policy.host.empty()) SetWindowTextW(surface->window, Wide(policy.host).c_str());
}

void FocusPage(Surface* surface) {
  if (surface == nullptr || surface->controller == nullptr || g_app.terminating) return;
  if (GetForegroundWindow() != surface->window) return;
  surface->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void ResizeToWindow(Surface* surface) {
  if (surface->controller == nullptr) return;
  RECT bounds;
  GetClientRect(surface->window, &bounds);
  surface->controller->put_Bounds(bounds);
}

void DestroySurface(Surface* surface) {
  if (surface->controller != nullptr) surface->controller->Close();
  SafeRelease(&surface->web_view);
  SafeRelease(&surface->controller);
  auto found = std::find(g_app.surfaces.begin(), g_app.surfaces.end(), surface);
  if (found != g_app.surfaces.end()) g_app.surfaces.erase(found);
  if (g_app.primary == surface) g_app.primary = nullptr;
  delete surface;
}

void BeginTermination(const char* reason) {
  if (g_app.terminating) return;
  g_app.terminating = true;
  TraceWrite("terminate", {{"reason", reason}});
  Log("%s; closing WebView2 surfaces", reason);
  KillTimer(g_app.message_window, kCookieTimer);
  {
    std::lock_guard<std::mutex> lock(g_app.send_lock);
    if (g_app.control != INVALID_SOCKET) shutdown(g_app.control, SD_SEND);
  }
  std::vector<Surface*> surfaces = g_app.surfaces;
  for (Surface* surface : surfaces) DestroyWindow(surface->window);
  PostQuitMessage(0);
}

void PaintSetup(HDC dc, const RECT& client) {
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  HFONT font = CreateFontW(-MulDiv(12, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HGDIOBJ previous = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  const int line = metrics.tmHeight + metrics.tmExternalLeading;
  const int middle = (client.top + client.bottom) / 2;
  RECT first = {client.left, middle - line, client.right, middle};
  RECT second = {client.left, middle, client.right, middle + line};
  DrawTextW(dc, g_app.security_check ? L"Setting up WebView2 for this check" : L"Setting up WebView2 for this page", -1,
            &first, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
  DrawTextW(dc, g_app.setup_line.c_str(), -1, &second, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, previous);
  DeleteObject(font);
}

LRESULT CALLBACK SurfaceProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* surface = reinterpret_cast<Surface*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  TraceWindowMessage(window, message, wparam, lparam);
  const bool setup_shown = surface != nullptr && surface->primary && g_app.setting_up;
  switch (message) {
    case WM_PAINT: {
      if (!setup_shown) break;
      PAINTSTRUCT paint;
      HDC dc = BeginPaint(window, &paint);
      RECT client;
      GetClientRect(window, &client);
      PaintSetup(dc, client);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_SIZE:
      if (surface != nullptr) ResizeToWindow(surface);
      if (setup_shown) InvalidateRect(window, nullptr, FALSE);
      return 0;
    case WM_SETFOCUS:
      if (surface != nullptr && surface->controller != nullptr) {
        surface->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
      }
      break;
    case WM_MOVE:
    case WM_MOVING:
      if (surface != nullptr && surface->controller != nullptr) surface->controller->NotifyParentWindowPositionChanged();
      break;
    case WM_CLOSE:
      if (surface != nullptr && surface->primary && !g_app.terminating) {
        BeginTermination("primary WebView window closed");
        return 0;
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      if (surface != nullptr) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroySurface(surface);
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

Surface* CreateSurfaceWindow(bool primary, HWND owner) {
  auto* surface = new Surface;
  surface->primary = primary;
  HDC screen = GetDC(nullptr);
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  RECT game{};
  const bool over_game = g_app.owner != nullptr && IsWindow(g_app.owner) && !IsIconic(g_app.owner) &&
                         GetWindowRect(g_app.owner, &game) && game.right > game.left && game.bottom > game.top;
  MONITORINFO monitor{sizeof monitor};
  GetMonitorInfoW(MonitorFromWindow(over_game ? g_app.owner : nullptr, MONITOR_DEFAULTTOPRIMARY), &monitor);
  const RECT& work = monitor.rcWork;
  const int room_w = over_game ? game.right - game.left : work.right - work.left;
  const int room_h = over_game ? game.bottom - game.top : work.bottom - work.top;
  RECT rect = {0, 0, std::max(MulDiv(960, dpi, 96), room_w * 4 / 5),
               std::max(MulDiv(640, dpi, 96), room_h * 4 / 5)};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  int width = std::min<int>(rect.right - rect.left, work.right - work.left);
  int height = std::min<int>(rect.bottom - rect.top, work.bottom - work.top);
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  if (over_game) {
    x = std::clamp<int>((game.left + game.right - width) / 2, work.left, work.right - width);
    y = std::clamp<int>((game.top + game.bottom - height) / 2, work.top, work.bottom - height);
  }
  surface->window = CreateWindowExW(0, L"Blox7WebView", L"Roblox", WS_OVERLAPPEDWINDOW, x, y, width, height, owner,
                                    nullptr, GetModuleHandleW(nullptr), nullptr);
  if (surface->window == nullptr) {
    delete surface;
    return nullptr;
  }
  SetWindowLongPtrW(surface->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(surface));
  g_app.surfaces.push_back(surface);
  return surface;
}

// BLOX7_WEBVIEW_CONSOLE. "runtime" is visible to page scripts, so debugging only
enum class ConsoleCapture { kOff, kBrowserLog, kRuntime };

ConsoleCapture ConsoleCaptureMode() {
  const char* value = getenv("BLOX7_WEBVIEW_CONSOLE");
  if (value == nullptr || *value == '\0' || strcmp(value, "0") == 0) return ConsoleCapture::kOff;
  return strcmp(value, "runtime") == 0 ? ConsoleCapture::kRuntime : ConsoleCapture::kBrowserLog;
}

void WatchConsole(ICoreWebView2* web_view) {
  static const ConsoleCapture mode = ConsoleCaptureMode();
  if (mode == ConsoleCapture::kOff) return;
  const bool runtime = mode == ConsoleCapture::kRuntime;
  static bool announced = false;
  if (!announced) {
    announced = true;
    Log(runtime ? "console capture: browser log and Runtime domain (pages can detect it; bot checks fail)"
                : "console capture: browser log only");
  }
  for (const wchar_t* event : {L"Log.entryAdded", L"Runtime.consoleAPICalled", L"Runtime.exceptionThrown"}) {
    const bool browser_log = wcscmp(event, L"Log.entryAdded") == 0;
    if (!browser_log && !runtime) continue;
    ICoreWebView2DevToolsProtocolEventReceiver* receiver = nullptr;
    if (FAILED(web_view->GetDevToolsProtocolEventReceiver(event, &receiver)) || receiver == nullptr) continue;
    const bool exception = wcscmp(event, L"Runtime.exceptionThrown") == 0;
    EventRegistrationToken token;
    receiver->add_DevToolsProtocolEventReceived(
        MakeHandler<ICoreWebView2DevToolsProtocolEventReceivedEventHandler, ICoreWebView2*,
                    ICoreWebView2DevToolsProtocolEventReceivedEventArgs*>(
            [exception, browser_log](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
              bool from_worker = false;
              ICoreWebView2DevToolsProtocolEventReceivedEventArgs2* args2 = nullptr;
              if (SUCCEEDED(args->QueryInterface(IID_ICoreWebView2DevToolsProtocolEventReceivedEventArgs2,
                                                 reinterpret_cast<void**>(&args2))) &&
                  args2 != nullptr) {
                LPWSTR session = nullptr;
                if (SUCCEEDED(args2->get_SessionId(&session))) from_worker = !TakeString(session).empty();
                args2->Release();
              }
              LPWSTR json = nullptr;
              if (FAILED(args->get_ParameterObjectAsJson(&json))) return S_OK;
              const nlohmann::json event = nlohmann::json::parse(TakeString(json), nullptr, false);
              std::string line;
              if (browser_log) {
                const nlohmann::json& entry = event.value("entry", nlohmann::json::object());
                line = "log." + entry.value("level", std::string()) + " [" + entry.value("source", std::string()) +
                       "]: " + entry.value("text", std::string());
                const std::string url = entry.value("url", std::string());
                if (!url.empty()) line += " " + url.substr(0, url.find('?'));
              } else if (exception) {
                const nlohmann::json& details = event.value("exceptionDetails", nlohmann::json::object());
                line = "exception: " + details.value("text", std::string());
                if (details.contains("exception") && details["exception"].is_object())
                  line += " " + details["exception"].value("description", std::string());
              } else {
                line = "console." + event.value("type", std::string("log")) + ":";
                for (const auto& arg : event.value("args", nlohmann::json::array())) {
                  if (!arg.is_object()) continue;
                  if (arg.contains("value")) {
                    line += " " + (arg["value"].is_string() ? arg["value"].get<std::string>() : arg["value"].dump());
                  } else {
                    line += " " + arg.value("description", arg.value("type", std::string()));
                  }
                }
              }
              if (line.size() > 960) line.resize(960);
              for (char& c : line) {
                if (static_cast<unsigned char>(c) < 0x20) c = ' ';
              }
              Log("%s %s", from_worker ? "worker" : "page", line.c_str());
              return S_OK;
            }),
        &token);
    receiver->Release();
  }
  web_view->CallDevToolsProtocolMethod(L"Log.enable", L"{}", nullptr);
  if (!runtime) return;
  ICoreWebView2DevToolsProtocolEventReceiver* attached = nullptr;
  if (SUCCEEDED(web_view->GetDevToolsProtocolEventReceiver(L"Target.attachedToTarget", &attached)) &&
      attached != nullptr) {
    EventRegistrationToken token;
    attached->add_DevToolsProtocolEventReceived(
        MakeHandler<ICoreWebView2DevToolsProtocolEventReceivedEventHandler, ICoreWebView2*,
                    ICoreWebView2DevToolsProtocolEventReceivedEventArgs*>(
            [](ICoreWebView2* view, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
              LPWSTR json = nullptr;
              if (FAILED(args->get_ParameterObjectAsJson(&json))) return S_OK;
              const nlohmann::json event = nlohmann::json::parse(TakeString(json), nullptr, false);
              const std::string session = event.value("sessionId", std::string());
              const nlohmann::json& info = event.value("targetInfo", nlohmann::json::object());
              const std::string url = info.value("url", std::string());
              Log("page target attached type=%s url=%s", info.value("type", std::string()).c_str(),
                  url.substr(0, url.find('?')).c_str());
              ICoreWebView2_11* view11 = nullptr;
              if (!session.empty() &&
                  SUCCEEDED(view->QueryInterface(IID_ICoreWebView2_11, reinterpret_cast<void**>(&view11))) &&
                  view11 != nullptr) {
                const std::wstring id = Wide(session);
                view11->CallDevToolsProtocolMethodForSession(id.c_str(), L"Runtime.enable", L"{}", nullptr);
                view11->CallDevToolsProtocolMethodForSession(id.c_str(), L"Log.enable", L"{}", nullptr);
                view11->CallDevToolsProtocolMethodForSession(id.c_str(), L"Runtime.runIfWaitingForDebugger", L"{}",
                                                             nullptr);
                view11->Release();
              }
              return S_OK;
            }),
        &token);
    attached->Release();
  }
  web_view->CallDevToolsProtocolMethod(L"Runtime.enable", L"{}", nullptr);
  web_view->CallDevToolsProtocolMethod(
      L"Target.setAutoAttach", L"{\"autoAttach\":true,\"waitForDebuggerOnStart\":false,\"flatten\":true}",
      nullptr);
}

// BLOX7_WEBVIEW_NETWORK_PROBE diagnostic: credentials redacted, the roblox cookie never logged
bool NetworkProbeEnabled() {
  const char* value = getenv("BLOX7_WEBVIEW_NETWORK_PROBE");
  return value != nullptr && *value != '\0' && strcmp(value, "0") != 0;
}

bool ProbeUrl(const std::string& url) {
  return url.find("collector") != std::string::npos || url.find("px-cloud") != std::string::npos ||
         url.find("px-cdn") != std::string::npos || url.find("/v2/captcha") != std::string::npos ||
         url.find("challenge/cdn/hybrid") != std::string::npos;
}

std::string RedactSecrets(std::string text) {
  for (const char* key : {"token", "captchaToken", "challengeToken", "solution", "pxhd"}) {
    const std::string marker = std::string("\"") + key + "\"";
    std::size_t pos = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
      const std::size_t colon = text.find(':', pos + marker.size());
      if (colon == std::string::npos) break;
      const std::size_t quote = text.find('"', colon + 1);
      if (quote == std::string::npos) break;
      const std::size_t end = text.find('"', quote + 1);
      if (end == std::string::npos) break;
      text.replace(quote + 1, end - quote - 1, "<redacted>");
      pos = quote + 1;
    }
  }
  return text;
}

void WatchNetwork(ICoreWebView2* web_view) {
  if (!NetworkProbeEnabled()) return;
  static bool announced = false;
  if (!announced) {
    announced = true;
    Log("network probe: HUMAN collector and /v2/captcha requests enabled");
  }
  web_view->CallDevToolsProtocolMethod(L"Network.enable", L"{}", nullptr);
  ICoreWebView2DevToolsProtocolEventReceiver* receiver = nullptr;
  if (SUCCEEDED(web_view->GetDevToolsProtocolEventReceiver(L"Network.requestWillBeSent", &receiver)) &&
      receiver != nullptr) {
    EventRegistrationToken token;
    receiver->add_DevToolsProtocolEventReceived(
        MakeHandler<ICoreWebView2DevToolsProtocolEventReceivedEventHandler, ICoreWebView2*,
                    ICoreWebView2DevToolsProtocolEventReceivedEventArgs*>(
            [](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
              LPWSTR json = nullptr;
              if (FAILED(args->get_ParameterObjectAsJson(&json))) return S_OK;
              const nlohmann::json event = nlohmann::json::parse(TakeString(json), nullptr, false);
              const nlohmann::json& request = event.value("request", nlohmann::json::object());
              const std::string url = request.value("url", std::string());
              if (!ProbeUrl(url)) return S_OK;
              std::string post = request.value("postData", std::string());
              if (post.size() > 3200) post.resize(3200);
              bool has_csrf = false;
              try {
                const nlohmann::json& headers = request.value("headers", nlohmann::json::object());
                has_csrf = headers.contains("x-csrf-token") || headers.contains("X-CSRF-TOKEN");
              } catch (...) {
              }
              Log("net request %s %s csrf=%s post=%s", request.value("method", std::string()).c_str(),
                  url.substr(0, 220).c_str(), has_csrf ? "yes" : "no",
                  RedactSecrets(std::move(post)).c_str());
              return S_OK;
            }),
        &token);
    receiver->Release();
  }
  if (SUCCEEDED(web_view->GetDevToolsProtocolEventReceiver(L"Network.responseReceived", &receiver)) &&
      receiver != nullptr) {
    EventRegistrationToken token;
    receiver->add_DevToolsProtocolEventReceived(
        MakeHandler<ICoreWebView2DevToolsProtocolEventReceivedEventHandler, ICoreWebView2*,
                    ICoreWebView2DevToolsProtocolEventReceivedEventArgs*>(
            [](ICoreWebView2* view, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
              LPWSTR json = nullptr;
              if (FAILED(args->get_ParameterObjectAsJson(&json))) return S_OK;
              const nlohmann::json event = nlohmann::json::parse(TakeString(json), nullptr, false);
              const nlohmann::json& response = event.value("response", nlohmann::json::object());
              const std::string url = response.value("url", std::string());
              if (!ProbeUrl(url)) return S_OK;
              std::string headers;
              const nlohmann::json& all = response.value("headers", nlohmann::json::object());
              for (auto it = all.begin(); it != all.end(); ++it) {
                const std::string& name = it.key();
                if (name.rfind("x-px", 0) == 0 || name == "content-type" || name == "www-authenticate") {
                  std::string value = it.value().is_string() ? it.value().get<std::string>() : std::string();
                  if (value.size() > 160) value.resize(160);
                  headers += " " + name + "=" + value;
                } else if (name == "set-cookie") {
                  headers += " set-cookie=present";
                }
              }
              Log("net response %d %s%s", response.value("status", 0), url.substr(0, 220).c_str(),
                  headers.c_str());
              if (url.find("/v2/captcha") == std::string::npos &&
                  url.find("collector") == std::string::npos) {
                return S_OK;
              }
              const std::string request_id = event.value("requestId", std::string());
              if (request_id.empty()) return S_OK;
              auto* id = new std::string(request_id);
              const std::wstring parameters = L"{\"requestId\":\"" + Wide(*id) + L"\"}";
              view->CallDevToolsProtocolMethod(
                  L"Network.getResponseBody", parameters.c_str(),
                  MakeHandler<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, HRESULT, LPCWSTR>(
                      [id, url](HRESULT result, LPCWSTR json) -> HRESULT {
                        const std::string request = *id;
                        delete id;
                        if (FAILED(result) || json == nullptr) return S_OK;
                        const nlohmann::json parsed = nlohmann::json::parse(Utf8(json), nullptr, false);
                        std::string body = parsed.value("body", std::string());
                        if (body.size() > 3200) body.resize(3200);
                        Log("net body %s %s: %s", url.substr(0, 180).c_str(), request.c_str(),
                            RedactSecrets(std::move(body)).c_str());
                        return S_OK;
                      }));
              return S_OK;
            }),
        &token);
    receiver->Release();
  }
}

std::string LinkScheme(const std::string& uri) {
  std::string scheme = uri.substr(0, std::min(uri.find(':'), std::size_t{32}));
  for (char& c : scheme) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') c = '?';
  }
  return scheme;
}

bool IsRobloxLaunchLink(const std::string& uri) {
  const std::string scheme = LinkScheme(uri);
  return scheme == "roblox" || scheme == "roblox-player";
}

void HandleExternalUri(std::string* uri) {
  const std::string scheme = LinkScheme(*uri);
  if (IsRobloxLaunchLink(*uri)) {
    if (SendEvent(WebViewHelperEventType::kRobloxLaunchUri, *uri)) {
      Log("%s link handed to Blox7", scheme.c_str());
    } else {
      Log("could not hand a %s link to Blox7", scheme.c_str());
    }
  } else {
    Log("external link blocked scheme=%s", scheme.c_str());
  }
  ClearSensitive(uri);
}

void ConfigureWebView(Surface* surface) {
  ICoreWebView2* web_view = surface->web_view;
  TraceWebView(surface);
  ICoreWebView2Settings* settings = nullptr;
  if (SUCCEEDED(web_view->get_Settings(&settings))) {
    settings->put_IsScriptEnabled(TRUE);
    settings->put_IsWebMessageEnabled(TRUE);
    settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->Release();
  }
  WatchConsole(web_view);
  WatchNetwork(web_view);
  if (g_app.app_user_agent) {
    const std::wstring bridge = Wide(kAndroidBridge);
    web_view->AddScriptToExecuteOnDocumentCreated(bridge.c_str(), nullptr);
  }

  EventRegistrationToken token;
  web_view->add_NavigationStarting(
      MakeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
          [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            std::string target = TakeString(uri);
            const auto policy = EvaluateNavigationUri(target.c_str());
            if (!policy.allowed) {
              args->put_Cancel(TRUE);
              if (IsRobloxLaunchLink(target)) {
                HandleExternalUri(&target);
              } else {
                Log("navigation blocked scheme=%s host=%s", policy.scheme.c_str(), policy.host.c_str());
              }
            }
            return S_OK;
          }),
      &token);
  web_view->add_FrameNavigationStarting(
      MakeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
          [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            std::string target = TakeString(uri);
            if (IsRobloxLaunchLink(target)) {
              args->put_Cancel(TRUE);
              HandleExternalUri(&target);
            }
            return S_OK;
          }),
      &token);
  WebView18* web_view18 = nullptr;
  if (SUCCEEDED(web_view->QueryInterface(kIidWebView18, reinterpret_cast<void**>(&web_view18))) &&
      web_view18 != nullptr) {
    web_view18->add_LaunchingExternalUriScheme(
        MakeHandler<ExternalUriHandler, ICoreWebView2*, ExternalUriArgs*>(
            [](ICoreWebView2*, ExternalUriArgs* args) -> HRESULT {
              LPWSTR uri = nullptr;
              args->get_Uri(&uri);
              std::string target = TakeString(uri);
              args->put_Cancel(TRUE);
              HandleExternalUri(&target);
              return S_OK;
            }),
        &token);
    web_view18->Release();
  }
  web_view->add_NavigationCompleted(
      MakeHandler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
          [surface](ICoreWebView2* view, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            LPWSTR uri = nullptr;
            view->get_Source(&uri);
            const auto policy = EvaluateNavigationUri(TakeString(uri).c_str());
            if (success) {
              Log("load stage=finished scheme=%s host=%s", policy.scheme.c_str(), policy.host.c_str());
            } else {
              COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
              args->get_WebErrorStatus(&status);
              Log("load failed scheme=%s host=%s status=%d", policy.scheme.c_str(), policy.host.c_str(),
                  static_cast<int>(status));
            }
            UpdateDomainTitle(surface);
            RequestRobloxCookie();
            return S_OK;
          }),
      &token);
  web_view->add_WebMessageReceived(
      MakeHandler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
          [surface](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            OnBridgeMessage(surface, args);
            return S_OK;
          }),
      &token);
  web_view->add_WindowCloseRequested(
      MakeHandler<ICoreWebView2WindowCloseRequestedEventHandler, ICoreWebView2*, IUnknown*>(
          [surface](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (surface->primary) {
              BeginTermination("primary WebView requested close");
            } else {
              PostMessageW(surface->window, WM_CLOSE, 0, 0);
            }
            return S_OK;
          }),
      &token);
  web_view->add_ProcessFailed(
      MakeHandler<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
          [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            args->get_ProcessFailedKind(&kind);
            Log("web process terminated kind=%d", static_cast<int>(kind));
            return S_OK;
          }),
      &token);
  web_view->add_NewWindowRequested(
      MakeHandler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
          [surface](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            ICoreWebView2Deferral* deferral = nullptr;
            if (FAILED(args->GetDeferral(&deferral))) return S_OK;
            args->AddRef();
            Surface* popup = CreateSurfaceWindow(false, surface->window);
            if (popup == nullptr) {
              args->Release();
              deferral->Complete();
              deferral->Release();
              return S_OK;
            }
            AttachWebView(popup, [args, deferral](Surface* ready) {
              if (ready != nullptr && ready->web_view != nullptr) {
                args->put_NewWindow(ready->web_view);
                args->put_Handled(TRUE);
                ShowWindow(ready->window, SW_SHOW);
              }
              deferral->Complete();
              deferral->Release();
              args->Release();
            });
            return S_OK;
          }),
      &token);
  surface->controller->add_AcceleratorKeyPressed(
      MakeHandler<ICoreWebView2AcceleratorKeyPressedEventHandler, ICoreWebView2Controller*,
                  ICoreWebView2AcceleratorKeyPressedEventArgs*>(
          [surface](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_KEY_EVENT_KIND kind;
            UINT key = 0;
            args->get_KeyEventKind(&kind);
            args->get_VirtualKey(&key);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
              return S_OK;
            }
            const bool control = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
            if (key == VK_F5 || (control && key == 'R')) {
              Log("page reload requested");
              surface->web_view->Reload();
              args->put_Handled(TRUE);
            } else if (g_app.back_navigation_disabled && (key == VK_BROWSER_BACK || (alt && key == VK_LEFT))) {
              args->put_Handled(TRUE);
            }
            return S_OK;
          }),
      &token);
}

void AttachWebView(Surface* surface, std::function<void(Surface*)> ready) {
  HWND window = surface->window;
  g_app.environment->CreateCoreWebView2Controller(
      window, MakeHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
                  [window, ready = std::move(ready)](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    auto* surface = reinterpret_cast<Surface*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                    if (surface == nullptr || FAILED(result) || controller == nullptr) {
                      Log("cannot create a WebView2 surface: 0x%08lx", static_cast<unsigned long>(result));
                      g_app.surface_result = FAILED(result) ? result : E_FAIL;
                      ready(nullptr);
                      return S_OK;
                    }
                    controller->AddRef();
                    surface->controller = controller;
                    controller->get_CoreWebView2(&surface->web_view);
                    ConfigureWebView(surface);
                    ResizeToWindow(surface);
                    controller->put_IsVisible(TRUE);
                    FocusPage(surface);
                    ready(surface);
                    return S_OK;
                  }));
}

// app pages get the apk's own web view ua, the website's sign-in keeps webview2's. the first page picks

using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                                     ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
using BrowserVersionFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);
CreateEnvironmentFn g_create_environment;
BrowserVersionFn g_browser_version;

bool RuntimeSetting() {
  const wchar_t* chosen = _wgetenv(L"BLOX7_WEBVIEW2_RUNTIME");
  return chosen != nullptr && *chosen != 0 && wcscmp(chosen, L"installed") != 0;
}

bool RuntimeInstalled() {
  if (!g_app.runtime_folder.empty()) return wv2_folder_usable(g_app.runtime_folder.c_str()) != 0;
  if (g_browser_version == nullptr) return true;
  LPWSTR version = nullptr;
  const HRESULT result = g_browser_version(nullptr, &version);
  const bool found = SUCCEEDED(result) && version != nullptr;
  if (version != nullptr) CoTaskMemFree(version);
  return found;
}

std::string VersionText(const unsigned version[4]) {
  return std::to_string(version[0]) + "." + std::to_string(version[1]) + "." + std::to_string(version[2]) + "." +
         std::to_string(version[3]);
}

std::string FolderVersion(const std::wstring& folder) {
  static constexpr std::wstring_view kStem = L"Microsoft.WebView2.FixedVersionRuntime.";
  const std::size_t slash = folder.find_last_of(L"\\/");
  const std::wstring name = folder.substr(slash == std::wstring::npos ? 0 : slash + 1);
  unsigned version[4] = {};
  if (name.size() > kStem.size() && _wcsnicmp(name.c_str(), kStem.data(), kStem.size()) == 0 &&
      swscanf(name.c_str() + kStem.size(), L"%u.%u.%u.%u", &version[0], &version[1], &version[2], &version[3]) == 4) {
    return VersionText(version);
  }
  const std::wstring exe = folder + L"\\msedgewebview2.exe";
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
  std::vector<BYTE> data(size);
  VS_FIXEDFILEINFO* info = nullptr;
  UINT length = 0;
  if (size == 0 || !GetFileVersionInfoW(exe.c_str(), 0, size, data.data()) ||
      !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || info == nullptr ||
      length < sizeof *info) {
    return {};
  }
  const unsigned file[4] = {HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                            HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
  return VersionText(file);
}

std::string RuntimeVersion() {
  if (!g_app.runtime_folder.empty()) return FolderVersion(g_app.runtime_folder);
  LPWSTR version = nullptr;
  if (g_browser_version == nullptr || FAILED(g_browser_version(nullptr, &version)) || version == nullptr) return {};
  return TakeString(version);
}

// 7, 8 and 8.1 stop at webview2 109, so those get pointed at the update catalog
OSVERSIONINFOW WindowsVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW os{};
  os.dwOSVersionInfoSize = sizeof os;
  if (const auto get = reinterpret_cast<RtlGetVersionFn>(
          reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
    get(&os);
  return os;
}

const char* StartFailure(HRESULT result) {
  if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return "start";
  if (RuntimeSetting()) return "setting";
  return g_app.runtime_folder.empty() ? "missing" : "start";
}

bool IsUsableUserAgent(const std::string& agent) {
  return !agent.empty() && agent.size() <= 1024 && !HasControlBytes(agent) && agent.find('"') == std::string::npos;
}

bool IsDesktopProfile() {
  const char* device_class = getenv("BLOX7_DEVICE_CLASS");
  return device_class != nullptr && strcmp(device_class, "pc") == 0;
}

std::wstring WithEdgeRootStore(std::wstring arguments) {
  constexpr wchar_t kFeature[] = L"msMicrosoftRootStoreUsed";
  constexpr wchar_t kSwitch[] = L"--enable-features=";
  if (wv2_windows_major() >= 10 || g_app.runtime_folder.empty() || arguments.find(kFeature) != std::wstring::npos)
    return arguments;
  const size_t at = arguments.find(kSwitch);
  if (at == std::wstring::npos) return arguments + (arguments.empty() ? L"" : L" ") + kSwitch + kFeature;
  const size_t end = arguments.find(L' ', at);
  arguments.insert(end == std::wstring::npos ? arguments.size() : end, std::wstring(L",") + kFeature);
  return arguments;
}

void CreatePageBrowser(const std::wstring& user_agent_argument) {
  std::wstring arguments = WithEdgeRootStore(g_app.browser_arguments);
  if (arguments != g_app.browser_arguments) Log("certificates are checked with Edge's own root store");
  if (!user_agent_argument.empty()) arguments += (arguments.empty() ? L"" : L" ") + user_agent_argument;
  SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", arguments.empty() ? nullptr : arguments.c_str());
  if (!RuntimeInstalled()) {
    const HRESULT missing = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    WebView2Failed(StartFailure(missing), missing);
    return;
  }
  const HRESULT started = g_create_environment(
      g_app.runtime_folder.empty() ? nullptr : g_app.runtime_folder.c_str(), g_app.data_directory.c_str(), nullptr,
      MakeHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
          [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || environment == nullptr) {
              WebView2Failed(StartFailure(result), result);
              return S_OK;
            }
            environment->AddRef();
            g_app.environment = environment;
            TraceEnvironment(environment);
            AttachWebView(g_app.primary, [](Surface* surface) {
              if (surface == nullptr) {
                WebView2Failed("start", g_app.surface_result);
                return;
              }
              Log("WebView2 ready");
              ApplyPendingCookie();
              for (std::string& script : g_app.pending_scripts) {
                surface->web_view->ExecuteScript(Wide(script).c_str(), nullptr);
                ClearSensitive(&script);
              }
              g_app.pending_scripts.clear();
            });
            return S_OK;
          }));
  if (FAILED(started)) WebView2Failed(StartFailure(started), started);
}

std::string BrowserUserAgentKey() {
  const std::string version = RuntimeVersion();
  if (version.empty()) return {};
  const OSVERSIONINFOW os = WindowsVersion();
  return version + " " + std::to_string(os.dwMajorVersion) + "." + std::to_string(os.dwMinorVersion) +
         "." + std::to_string(os.dwBuildNumber);
}

std::wstring UserAgentCachePath() { return g_app.data_directory + L"\\user-agent.txt"; }

std::string CachedBrowserUserAgent(const std::string& key) {
  if (key.empty()) return {};
  FILE* file = _wfopen(UserAgentCachePath().c_str(), L"rb");
  if (file == nullptr) return {};
  char text[2048];
  const std::size_t n = fread(text, 1, sizeof text, file);
  fclose(file);
  const std::string_view all(text, n);
  const std::size_t newline = all.find('\n');
  if (newline == std::string_view::npos || all.substr(0, newline) != key) return {};
  std::string agent(all.substr(newline + 1));
  while (!agent.empty() && (agent.back() == '\n' || agent.back() == '\r')) agent.pop_back();
  return IsUsableUserAgent(agent) ? agent : std::string();
}

void CacheBrowserUserAgent(const std::string& key, const std::string& agent) {
  if (key.empty()) return;
  if (FILE* file = _wfopen(UserAgentCachePath().c_str(), L"wb")) {
    fprintf(file, "%s\n%s\n", key.c_str(), agent.c_str());
    fclose(file);
  }
}

std::string UserAgentOf(ICoreWebView2Controller* controller) {
  std::string agent;
  ICoreWebView2* view = nullptr;
  ICoreWebView2Settings* settings = nullptr;
  ICoreWebView2Settings2* settings2 = nullptr;
  LPWSTR text = nullptr;
  if (SUCCEEDED(controller->get_CoreWebView2(&view)) && view != nullptr && SUCCEEDED(view->get_Settings(&settings)) &&
      settings != nullptr &&
      SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings2, reinterpret_cast<void**>(&settings2))) &&
      settings2 != nullptr && SUCCEEDED(settings2->get_UserAgent(&text))) {
    agent = TakeString(text);
  }
  SafeRelease(&settings2);
  SafeRelease(&settings);
  SafeRelease(&view);
  return agent;
}

void ReadBrowserUserAgent(std::function<void(std::string)> done) {
  SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", nullptr);
  const HRESULT started = g_create_environment(
      g_app.runtime_folder.empty() ? nullptr : g_app.runtime_folder.c_str(),
      (g_app.data_directory + L"\\ua-probe").c_str(), nullptr,
      MakeHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
          [done](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || environment == nullptr || g_app.primary == nullptr || g_app.terminating) {
              done({});
              return S_OK;
            }
            environment->AddRef();
            environment->CreateCoreWebView2Controller(
                g_app.primary->window,
                MakeHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT,
                            ICoreWebView2Controller*>(
                    [done, environment](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                      const bool ready = SUCCEEDED(result) && controller != nullptr;
                      const std::string agent = ready ? UserAgentOf(controller) : std::string();
                      if (controller != nullptr) controller->Close();
                      environment->Release();
                      done(agent);
                      return S_OK;
                    }));
            return S_OK;
          }));
  if (FAILED(started)) done({});
}

void WithBrowserUserAgent(std::function<void(std::string)> done) {
  if (!RuntimeInstalled()) {
    done({});
    return;
  }
  const std::string key = BrowserUserAgentKey();
  if (std::string cached = CachedBrowserUserAgent(key); !cached.empty()) {
    done(cached);
    return;
  }
  ReadBrowserUserAgent([key, done](std::string agent) {
    if (!IsUsableUserAgent(agent)) agent.clear();
    if (!agent.empty()) CacheBrowserUserAgent(key, agent);
    done(agent);
  });
}

bool IsSecurityCheck(const std::string& url) {
  for (std::size_t at = url.find('?'); at != std::string::npos; at = url.find('&', at + 1)) {
    if (_strnicmp(url.c_str() + at + 1, "credentialsType=authToken", 25) == 0) return false;
  }
  if (blox7::webview::IsRobloxChallengePage(url)) return true;
  for (const char* base : {"https://www.roblox.com/captcha/", "https://roblox.com/captcha/"}) {
    const std::size_t size = strlen(base);
    if (_strnicmp(url.c_str(), base, size) == 0) return _strnicmp(url.c_str() + size, "app/signup", 10) != 0;
  }
  return false;
}

bool NeedsSetup() {
  const wchar_t* chosen = _wgetenv(L"BLOX7_WEBVIEW2_RUNTIME");
  return wv2_windows_major() < 10 && (chosen == nullptr || *chosen == 0) && g_app.runtime_folder.empty() &&
         !RuntimeInstalled();
}

void StartWebView2(const std::string& first_url) {
  if (g_app.webview2_requested || g_app.terminating || g_app.failure != nullptr) return;
  g_app.webview2_requested = true;
  if (NeedsSetup()) {
    WaitForSetup();
    return;
  }
  if (IsDesktopProfile() && !blox7::webview::IsRobloxChallengePage(first_url)) {
    g_app.app_user_agent = false;
    Log("starting WebView2 with the desktop user agent");
    CreatePageBrowser(L"");
    return;
  }
  g_app.app_user_agent = true;
  WithBrowserUserAgent([](std::string browser) {
    if (g_app.terminating) return;
    const std::string agent = browser.empty()
                                  ? blox7::webview::BuildRobloxAndroidUserAgent()
                                  : browser + "  " + blox7::webview::BuildRobloxAppUserAgentTokens();
    if (!IsUsableUserAgent(agent)) {
      g_app.startup_failed = true;
      BeginTermination("the app's user agent cannot be passed to WebView2");
      return;
    }
    Log("starting WebView2 with the app user agent: %s", agent.c_str());
    CreatePageBrowser(Wide("--user-agent=\"" + agent + "\""));
  });
}

void ApplyControl(const ControlCommand& command) {
  using Op = WebViewHelperControlOperation;
  Surface* primary = g_app.primary;
  switch (command.operation) {
    case Op::kLoadUrl:
      ClearSensitive(&g_app.pending_url);
      g_app.pending_url = command.payload;
      g_app.security_check = IsSecurityCheck(command.payload);
      if (g_app.setting_up && primary != nullptr) InvalidateRect(primary->window, nullptr, FALSE);
      StartWebView2(g_app.pending_url);
      StartInitialLoad();
      break;
    case Op::kSetTitle:
      if (primary != nullptr) SetWindowTextW(primary->window, Wide(command.payload).c_str());
      break;
    case Op::kSetVisible:
      g_app.visibility_set = true;
      g_app.visible = command.flag;
      if (g_app.failure != nullptr) {
        FinishFailure();
      } else if (primary != nullptr) {
        if (command.flag) {
          ShowWindow(primary->window, SW_SHOW);
          SetForegroundWindow(primary->window);
          FocusPage(primary);
        } else {
          ShowWindow(primary->window, SW_HIDE);
        }
      }
      break;
    case Op::kEvaluateJavaScript: {
      std::string head = command.payload.substr(0, command.payload.find('\''));
      const std::size_t call = head.rfind("Bridge.");
      head = call == std::string::npos ? std::string("script") : head.substr(call, 40);
      Log("controlled JavaScript %s (%zu bytes)", head.c_str(), command.payload.size());
    }
      if (primary != nullptr && primary->web_view == nullptr) {
        g_app.pending_scripts.push_back(command.payload);
      } else if (primary != nullptr && primary->web_view != nullptr) {
        primary->web_view->ExecuteScript(
            Wide(command.payload).c_str(),
            MakeHandler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>([](HRESULT result, LPCWSTR json) -> HRESULT {
              TraceWrite("script_result", {{"hr", static_cast<unsigned long>(result)},
                                           {"result", json != nullptr ? Utf8(json).substr(0, 8192) : std::string()}});
              if (FAILED(result)) {
                Log("controlled JavaScript failed: 0x%08lx", static_cast<unsigned long>(result));
              } else {
                Log("controlled JavaScript completed");
              }
              return S_OK;
            }));
      }
      break;
    case Op::kSetBackNavigationDisabled:
      g_app.back_navigation_disabled = command.flag;
      break;
    case Op::kSetShowDomainAsTitle:
      g_app.show_domain_as_title = command.flag;
      if (primary != nullptr) UpdateDomainTitle(primary);
      break;
    case Op::kSetRobloxCookie:
    case Op::kClearRobloxCookie:
    case Op::kRetainPersistentRobloxCookie:
      ClearSensitive(&g_app.pending_cookie);
      g_app.signed_in = command.operation == Op::kSetRobloxCookie;
      if (command.operation == Op::kSetRobloxCookie) {
        g_app.pending_cookie = command.payload;
        g_app.cookie_operation = CookieOperation::kSet;
      } else {
        g_app.cookie_operation =
            command.operation == Op::kClearRobloxCookie ? CookieOperation::kClear : CookieOperation::kRetainPersistent;
      }
      g_app.cookie_sync_received = true;
      ApplyPendingCookie();
      break;
    case Op::kClose:
      BeginTermination("control close received");
      break;
  }
}

LRESULT CALLBACK MessageProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case kControlMessage: {
      auto* packet = reinterpret_cast<std::string*>(lparam);
      TraceControl(*packet);
      ControlCommand command;
      const char* error = "";
      if (DecodeControl(*packet, &command, &error)) {
        ApplyControl(command);
      } else {
        Log("rejected malformed control packet: %s", error);
      }
      ClearSensitive(&command.payload);
      ClearSensitive(packet);
      delete packet;
      return 0;
    }
    case kControlClosed:
      BeginTermination("control channel closed");
      return 0;
    case WM_TIMER:
      if (wparam == kCookieTimer) {
        RequestRobloxCookie();
      } else if (wparam == kSetupTimer) {
        PollSetup();
      } else if (wparam == kFailureTimer) {
        g_app.visible = true;
        FinishFailure();
      } else if (wparam == kTraceTimer) {
        TraceWindowState();
        std::lock_guard<std::mutex> hold(g_trace.lock);
        if (g_trace.file != nullptr) fflush(g_trace.file);
      }
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

std::string DropForeignVariables() {
  std::string dropped;
  for (const wchar_t* name : {L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", L"WEBVIEW2_USER_DATA_FOLDER",
                              L"WEBVIEW2_RELEASE_CHANNEL_PREFERENCE"}) {
    if (GetEnvironmentVariableW(name, nullptr, 0) == 0) continue;
    SetEnvironmentVariableW(name, nullptr);
    dropped += (dropped.empty() ? "" : ", ") + Utf8(name);
  }
  return dropped;
}

HRESULT LoadWebView2() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (wchar_t* slash = wcsrchr(path, L'\\')) wcscpy(slash + 1, L"WebView2Loader.dll");
  HMODULE loader = LoadLibraryW(path);
  if (loader == nullptr) return HRESULT_FROM_WIN32(GetLastError());
  delayload_guard_loader = loader;
  g_create_environment =
      reinterpret_cast<CreateEnvironmentFn>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
  g_browser_version =
      reinterpret_cast<BrowserVersionFn>(GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString"));
  return g_create_environment != nullptr ? S_OK : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

std::wstring OwnRuntimeFolder() {
  wchar_t folder[MAX_PATH];
  if (wv2_windows_major() >= 10 || !wv2_folder(folder, MAX_PATH) || !wv2_folder_complete()) return {};
  return folder;
}

// the last webview2 for win7 is 109, a newer fixed-version runtime in <prefix>\webview2 wins when there
std::wstring FindRuntimeFolder() {
  if (const wchar_t* chosen = _wgetenv(L"BLOX7_WEBVIEW2_RUNTIME"); chosen != nullptr && *chosen != 0) {
    return wcscmp(chosen, L"installed") == 0 ? std::wstring() : std::wstring(chosen);
  }
  wchar_t path[MAX_PATH];
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return OwnRuntimeFolder();
  std::wstring prefix = path;
  for (int i = 0; i < 3; i++) {
    const std::size_t slash = prefix.find_last_of(L'\\');
    if (slash == std::wstring::npos) return OwnRuntimeFolder();
    prefix.resize(slash);
  }
  static constexpr std::wstring_view kStem = L"Microsoft.WebView2.FixedVersionRuntime.";
  std::wstring best;
  unsigned best_version[4] = {};
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((prefix + L"\\webview2\\" + std::wstring(kStem) + L"*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return OwnRuntimeFolder();
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
    const std::wstring folder = prefix + L"\\webview2\\" + found.cFileName;
    if (!wv2_folder_usable(folder.c_str())) continue;
    unsigned version[4] = {};
    if (swscanf(found.cFileName + kStem.size(), L"%u.%u.%u.%u", &version[0], &version[1], &version[2], &version[3]) != 4) {
      continue;
    }
    if (best.empty() || std::lexicographical_compare(best_version, best_version + 4, version, version + 4)) {
      best = folder;
      std::copy(version, version + 4, best_version);
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);
  return best.empty() ? OwnRuntimeFolder() : best;
}

// chromium's blocklist turns the gpu off on every vmware adapter (entry 68), 109's didn't
bool DisplayAdapterIsVmware() {
  DISPLAY_DEVICEW device{};
  device.cb = sizeof device;
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); i++) {
    if ((device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0 && wcsstr(device.DeviceID, L"VEN_15AD") != nullptr) {
      return true;
    }
  }
  return false;
}

ULONGLONG FileTimeValue(const FILETIME& time) {
  return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool SetupFailedSince(ULONGLONG since) {
  wchar_t home[MAX_PATH];
  WIN32_FILE_ATTRIBUTE_DATA data;
  return wv2_home(home, MAX_PATH) &&
         GetFileAttributesExW((std::wstring(home) + L"\\setup-error.txt").c_str(), GetFileExInfoStandard, &data) &&
         FileTimeValue(data.ftLastWriteTime) >= since;
}

void WaitForSetup() {
  const wchar_t* launcher = _wgetenv(L"BLOX7_LAUNCHER_EXE");
  g_app.setup_seen = wv2_setup_running() != 0;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  g_app.setup_since = FileTimeValue(now);
  if (!g_app.setup_seen && (launcher == nullptr || *launcher == 0 || !wv2_setup_spawn(launcher, 1))) {
    WebView2Failed("missing", HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    return;
  }
  g_app.setting_up = true;
  g_app.setup_deadline = GetTickCount64() + 30000;
  SetTimer(g_app.message_window, kSetupTimer, 250, nullptr);
  if (g_app.primary != nullptr) InvalidateRect(g_app.primary->window, nullptr, FALSE);
}

void PollSetup() {
  if (!g_app.setting_up || g_app.terminating) return;
  wv2_status status;
  if (wv2_status_read(&status)) {
    g_app.setup_seen = true;
    std::wstring line;
    if (status.state == WV2_STATE_DOWNLOADING && status.total > 0) {
      line = std::to_wstring(status.done / 1000000) + L" of " + std::to_wstring(status.total / 1000000) + L" MB";
    } else if (status.state == WV2_STATE_UNPACKING && status.total > 0) {
      line = std::to_wstring(std::min<LONGLONG>(status.done * 100 / status.total, 100)) + L"%";
    }
    if (line != g_app.setup_line) {
      g_app.setup_line = line;
      if (g_app.primary != nullptr) InvalidateRect(g_app.primary->window, nullptr, FALSE);
    }
    return;
  }
  const bool complete = wv2_folder_complete() != 0;
  if (!complete && !g_app.setup_seen && !SetupFailedSince(g_app.setup_since) &&
      GetTickCount64() < g_app.setup_deadline) {
    return;
  }
  KillTimer(g_app.message_window, kSetupTimer);
  g_app.setting_up = false;
  if (g_app.primary != nullptr) InvalidateRect(g_app.primary->window, nullptr, TRUE);
  const std::wstring folder = complete ? OwnRuntimeFolder() : std::wstring();
  if (folder.empty()) {
    DWORD win32 = 0;
    wv2_last_error(&win32);
    WebView2Failed("setup", HRESULT_FROM_WIN32(win32));
    return;
  }
  g_app.runtime_folder = folder;
  if (DisplayAdapterIsVmware()) {
    g_app.browser_arguments += g_app.browser_arguments.empty() ? L"--ignore-gpu-blocklist" : L" --ignore-gpu-blocklist";
  }
  Log("WebView2 runtime: %s", Utf8(g_app.runtime_folder.c_str()).c_str());
  g_app.webview2_requested = false;
  StartWebView2(g_app.pending_url);
}

const char* RuntimeSourceName() {
  if (RuntimeSetting()) return "setting";
  if (g_app.runtime_folder.empty()) return "installed";
  wchar_t own[MAX_PATH];
  return wv2_folder(own, MAX_PATH) && _wcsicmp(own, g_app.runtime_folder.c_str()) == 0 ? "blox7" : "bundled";
}

void LogFailure(const char* reason, HRESULT result) {
  OSVERSIONINFOEXW os{};
  os.dwOSVersionInfoSize = sizeof os;
  if (const auto get = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOEXW*)>(
          reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) {
    get(&os);
  }
  const HMODULE advapi = GetModuleHandleW(L"advapi32.dll");
  const bool add_dll_directory = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory") != nullptr;
  const bool event_set_information = advapi != nullptr && GetProcAddress(advapi, "EventSetInformation") != nullptr;
  const std::string error = strcmp(reason, "setup") == 0 ? " error=" + std::to_string(wv2_last_error(nullptr)) : "";
  const std::string version = RuntimeVersion();
  Log("cannot start WebView2: 0x%08lx reason=%s%s windows=%d.%lu.%lu sp%u AddDllDirectory=%s EventSetInformation=%s "
      "runtime=%s %s",
      static_cast<unsigned long>(result), reason, error.c_str(), wv2_windows_major(),
      *reinterpret_cast<const volatile ULONG*>(0x7FFE0270), os.dwBuildNumber,
      static_cast<unsigned>(os.wServicePackMajor), add_dll_directory ? "yes" : "no",
      event_set_information ? "yes" : "no", RuntimeSourceName(), version.empty() ? "none" : version.c_str());
}

bool FirstTimeTold(const char* reason) {
  DWORD parent = 0;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && parent == 0; more = Process32NextW(snapshot, &entry)) {
      if (entry.th32ProcessID == GetCurrentProcessId()) parent = entry.th32ParentProcessID;
    }
    CloseHandle(snapshot);
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent)) {
    GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
  }
  const std::wstring session =
      L"told-" + std::to_wstring(parent) + L"-" + std::to_wstring(FileTimeValue(created)) + L"-";
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW((g_app.data_directory + L"\\told-*").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (wcsncmp(found.cFileName, session.c_str(), session.size()) != 0) {
        DeleteFileW((g_app.data_directory + L"\\" + found.cFileName).c_str());
      }
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  HANDLE marker = CreateFileW((g_app.data_directory + L"\\" + session + Wide(reason)).c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (marker == INVALID_HANDLE_VALUE) return GetLastError() != ERROR_FILE_EXISTS;
  CloseHandle(marker);
  return true;
}

void ShowExplanation() {
  wchar_t exe[MAX_PATH];
  const DWORD size = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (size == 0 || size >= MAX_PATH) return;
  wchar_t result[16];
  _snwprintf(result, 16, L"0x%08lX", static_cast<unsigned long>(g_app.failure_result));
  std::wstring command = L"\"" + std::wstring(exe) + L"\" --explain " + Wide(g_app.failure) + L" --hr " + result +
                         L" --purpose " + (g_app.security_check ? L"check" : L"page") + L" --signed-in " +
                         (g_app.signed_in ? L"1" : L"0");
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup,
                      &process)) {
    return;
  }
  AllowSetForegroundWindow(process.dwProcessId);
  ResumeThread(process.hThread);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

void WebView2Failed(const char* reason, HRESULT result) {
  if (g_app.failure != nullptr || g_app.terminating) return;
  g_app.failure = reason;
  g_app.failure_result = result;
  g_app.startup_failed = true;
  LogFailure(reason, result);
  if (g_app.visibility_set) {
    FinishFailure();
  } else {
    SetTimer(g_app.message_window, kFailureTimer, 5000, nullptr);
  }
}

void FinishFailure() {
  if (g_app.terminating || g_app.failure == nullptr) return;
  KillTimer(g_app.message_window, kFailureTimer);
  if (g_app.visible && GetEnvironmentVariableW(L"BLOX7_NO_DIALOGS", nullptr, 0) == 0 && FirstTimeTold(g_app.failure)) {
    ShowExplanation();
  }
  BeginTermination("WebView2 start-up failed");
}

struct Explanation {
  std::wstring reason;
  HRESULT result = S_OK;
  bool check = false;
  bool signed_in = false;
};

std::wstring ExplanationText(const Explanation& explanation, bool* ask) {
  const std::wstring quick =
      L"You can still sign in: press Quick sign-in in the Roblox window and approve the code in the Roblox app on "
      L"your phone (More > Quick sign in).";
  const bool offer_quick = explanation.check && !explanation.signed_in;
  if (explanation.reason == L"setup") {
    const int error = wv2_last_error(nullptr);
    const std::wstring why = wv2_error_text(error == WV2_OK ? WV2_ERROR_INTERNAL : error);
    const std::wstring needs = L"WebView2, which Blox7 couldn't set up on this PC: " + why + L".\n\n";
    if (offer_quick) return L"Roblox wants to check it's you, and that check needs " + needs + quick;
    return L"This Roblox page needs " + needs + L"Open Blox7 and it will try again.";
  }
  if (explanation.reason == L"start") {
    wchar_t text[128];
    _snwprintf(text, 128, L"WebView2 couldn't start (error 0x%08lX). Windows or an antivirus may be blocking it.",
               static_cast<unsigned long>(explanation.result));
    return offer_quick ? std::wstring(text) + L"\n\n" + quick : std::wstring(text);
  }
  if (explanation.reason == L"loader") {
    return L"Part of Blox7 is missing (WebView2Loader.dll). An antivirus may have removed it.";
  }
  if (explanation.reason == L"setting") {
    const wchar_t* folder = _wgetenv(L"BLOX7_WEBVIEW2_RUNTIME");
    return L"The webview2_runtime folder in launcher.ini isn't a WebView2 runtime:\n" +
           std::wstring(folder != nullptr ? folder : L"");
  }
  *ask = wv2_windows_major() >= 10;
  return std::wstring(L"This Roblox page needs Microsoft Edge WebView2, which isn't installed.") +
         (*ask ? L"\n\nOpen Microsoft's WebView2 page?" : L"");
}

int Explain(const Explanation& explanation) {
  SetProcessDPIAware();
  bool ask = false;
  const std::wstring text = ExplanationText(explanation, &ask);
  const UINT style = (ask ? MB_YESNO : MB_OK) | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND;
  if (MessageBoxW(nullptr, text.c_str(), L"Blox7", style) == IDYES && ask &&
      SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
    ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr,
                  SW_SHOWNORMAL);
    CoUninitialize();
  }
  return EXIT_SUCCESS;
}

bool Connect(int port, const std::string& token) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  // overlapped, or the reader's blocking recv holds up every send from the ui thread
  g_app.control = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (g_app.control == INVALID_SOCKET) {
    g_app.control = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_app.control == INVALID_SOCKET) return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  BOOL no_delay = TRUE;
  setsockopt(g_app.control, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof no_delay);
  if (connect(g_app.control, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) return false;
  return SendFrame(token);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  int port = 0;
  std::string token;
  Explanation explanation;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::wstring_view option = argv[i];
    if (option == L"--port") {
      port = _wtoi(argv[i + 1]);
    } else if (option == L"--token") {
      token = Utf8(argv[i + 1]);
    } else if (option == L"--data") {
      g_app.data_directory = argv[i + 1];
    } else if (option == L"--icon") {
      g_app.icon = static_cast<HICON>(LoadImageW(nullptr, argv[i + 1], IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    } else if (option == L"--owner") {
      g_app.owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(argv[i + 1], nullptr, 10)));
    } else if (option == L"--explain") {
      explanation.reason = argv[i + 1];
    } else if (option == L"--hr") {
      explanation.result = static_cast<HRESULT>(wcstoul(argv[i + 1], nullptr, 16));
    } else if (option == L"--purpose") {
      explanation.check = wcscmp(argv[i + 1], L"check") == 0;
    } else if (option == L"--signed-in") {
      explanation.signed_in = wcscmp(argv[i + 1], L"1") == 0;
    }
  }
  if (!explanation.reason.empty()) return Explain(explanation);
  if (port <= 0 || port > 65535 || token.empty() || g_app.data_directory.empty()) {
    fprintf(stderr, "usage: blox7_webview_helper.exe --port <port> --token <token> --data <dir> [--icon <file>]\n");
    return EXIT_FAILURE;
  }
  g_log_file = _wfopen((g_app.data_directory + L"\\helper.log").c_str(), L"a");
  TraceStart();
  Log("starting: port %d", port);
  if (!Connect(port, token)) {
    Log("cannot connect to Blox7");
    return EXIT_FAILURE;
  }
  ClearSensitive(&token);
  Log("connected to Blox7");
  SetProcessDPIAware();
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return EXIT_FAILURE;

  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW surface_class{sizeof surface_class};
  surface_class.lpfnWndProc = SurfaceProc;
  surface_class.hInstance = instance;
  surface_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  surface_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  surface_class.hIcon = g_app.icon;
  surface_class.hIconSm = g_app.icon;
  surface_class.lpszClassName = L"Blox7WebView";
  WNDCLASSEXW message_class{sizeof message_class};
  message_class.lpfnWndProc = MessageProc;
  message_class.hInstance = instance;
  message_class.lpszClassName = L"Blox7WebViewControl";
  if (!RegisterClassExW(&surface_class) || !RegisterClassExW(&message_class)) return EXIT_FAILURE;
  g_app.message_window = CreateWindowExW(0, L"Blox7WebViewControl", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
  if (g_app.message_window == nullptr) return EXIT_FAILURE;
  HANDLE reader = CreateThread(nullptr, 0, ControlReader, nullptr, 0, nullptr);
  if (reader == nullptr) return EXIT_FAILURE;
  CloseHandle(reader);
  if (g_trace.on) SetTimer(g_app.message_window, kTraceTimer, 250, nullptr);

  Surface* primary = CreateSurfaceWindow(true, nullptr);
  if (delayload_guard_needed()) AddVectoredExceptionHandler(1, delayload_guard_handler);
  const std::string ignored = DropForeignVariables();
  if (primary == nullptr) {
    Log("WebView2 is unavailable: WebView2Loader.dll or the window could not be loaded");
    return EXIT_FAILURE;
  }
  const HRESULT loaded = LoadWebView2();
  g_app.primary = primary;
  // ready once the window exists, webview2's first start can blow the readiness deadline
  if (!SendEvent(WebViewHelperEventType::kReady, {})) {
    Log("could not publish helper readiness");
    return EXIT_FAILURE;
  }
  Log("window ready");
  wchar_t inherited[4096];
  const DWORD inherited_size = GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", inherited, 4096);
  if (inherited_size > 0 && inherited_size < 4096) g_app.browser_arguments = inherited;
  if (const char* no_gpu = getenv("BLOX7_WEBVIEW_DISABLE_GPU"); no_gpu != nullptr && strcmp(no_gpu, "0") != 0) {
    g_app.browser_arguments += g_app.browser_arguments.empty() ? L"--disable-gpu" : L" --disable-gpu";
  }
  g_app.runtime_folder = FindRuntimeFolder();
  if (!g_app.runtime_folder.empty() && DisplayAdapterIsVmware()) {
    g_app.browser_arguments += g_app.browser_arguments.empty() ? L"--ignore-gpu-blocklist" : L" --ignore-gpu-blocklist";
  }
  Log("WebView2 runtime: %s%s%s%s",
      g_app.runtime_folder.empty() ? "installed" : Utf8(g_app.runtime_folder.c_str()).c_str(),
      ignored.empty() ? "" : " (ignored ", ignored.c_str(), ignored.empty() ? "" : ")");
  if (const std::wstring trace = TraceBrowserArguments(); !trace.empty()) {
    g_app.browser_arguments += (g_app.browser_arguments.empty() ? L"" : L" ") + trace;
  }
  TraceWrite("browser_arguments", {{"runtime", Utf8(g_app.runtime_folder.c_str())},
                                   {"arguments", Utf8(g_app.browser_arguments.c_str())}});
  SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                          g_app.browser_arguments.empty() ? nullptr : g_app.browser_arguments.c_str());
  if (FAILED(loaded)) WebView2Failed("loader", loaded);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  TraceStop(g_app.startup_failed ? "startup failed" : "message loop ended");
  SafeRelease(&g_app.environment);
  {
    std::lock_guard<std::mutex> lock(g_app.send_lock);
    closesocket(g_app.control);
    g_app.control = INVALID_SOCKET;
  }
  ClearSensitive(&g_app.pending_url);
  ClearSensitive(&g_app.pending_cookie);
  ClearSensitive(&g_app.last_reported_cookie);
  CoUninitialize();
  return g_app.startup_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
