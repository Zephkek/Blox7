#ifndef BLOX7_GRAPHICS_GRAPHICS_BACKEND_H_
#define BLOX7_GRAPHICS_GRAPHICS_BACKEND_H_

#include <string>
#include <vector>

#include "blox7/status.h"

namespace blox7 {
namespace graphics {

enum class GraphicsBackendKind {
  kAngleVulkan = 0,
  kDirectVulkan,
  kSystemEgl,
};

enum class CapabilityState {
  kUnavailable = 0,
  kLoadable,
  kReady,
};

enum class HardwareAcceleration {
  kUnknown = 0,
  kSoftware,
  kHardware,
};

struct BackendCapability {
  GraphicsBackendKind backend = GraphicsBackendKind::kAngleVulkan;
  CapabilityState state = CapabilityState::kUnavailable;
  HardwareAcceleration acceleration = HardwareAcceleration::kUnknown;
  std::string detail;
};

struct BackendSelectionPolicy {
  GraphicsBackendKind requested = GraphicsBackendKind::kAngleVulkan;

  bool allow_fallback = false;
  bool require_hardware_acceleration = true;
  bool require_runtime_validation = true;
};

struct BackendSelection {
  Status status;
  GraphicsBackendKind backend = GraphicsBackendKind::kAngleVulkan;
  std::string detail;
};

BackendSelection SelectGraphicsBackend(
    const BackendSelectionPolicy& policy,
    const std::vector<BackendCapability>& capabilities);

const char* GraphicsBackendName(GraphicsBackendKind backend);

}  // namespace graphics
}  // namespace blox7

#endif  // BLOX7_GRAPHICS_GRAPHICS_BACKEND_H_
