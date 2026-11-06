#ifndef BLOX7_GRAPHICS_ANGLE_PROBE_H_
#define BLOX7_GRAPHICS_ANGLE_PROBE_H_

#include <string>

#include "blox7/graphics/graphics_backend.h"

namespace blox7 {
namespace graphics {

struct AngleProbeOptions {
  std::string egl_library_path;
  std::string gles_library_path;
  bool allow_software_device = false;
};

BackendCapability InspectAngleLibraries(const AngleProbeOptions& options);

BackendCapability ProbeAngleVulkan(const AngleProbeOptions& options);

}  // namespace graphics
}  // namespace blox7

#endif  // BLOX7_GRAPHICS_ANGLE_PROBE_H_
