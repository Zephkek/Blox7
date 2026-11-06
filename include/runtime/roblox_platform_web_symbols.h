#ifndef BLOX7_RUNTIME_ROBLOX_PLATFORM_WEB_SYMBOLS_H_
#define BLOX7_RUNTIME_ROBLOX_PLATFORM_WEB_SYMBOLS_H_

#include "runtime/roblox_account_bridge.h"
#include "runtime/roblox_browser_service_bridge.h"
#include "runtime/roblox_permissions_bridge.h"
#include "runtime/roblox_web_view_bridge.h"

namespace blox7 {
namespace runtime {

struct RobloxPlatformWebSymbols {
  RobloxWebViewMessageBusSymbols web_view;
  RobloxBrowserServiceSymbols browser_service;
  RobloxPermissionsMessageBusSymbols permissions;
  RobloxAccountProtocolSymbols account;

  bool complete() const {
    return web_view.complete() && browser_service.complete() &&
           permissions.complete();
  }
};

RobloxPlatformWebSymbols ResolveRobloxPlatformWebSymbols(
    void* roblox_library, SubscribeWebViewRawFn subscribe_raw,
    DeleteWebViewConnectionFn delete_connection);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_PLATFORM_WEB_SYMBOLS_H_
