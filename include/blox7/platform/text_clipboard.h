#ifndef BLOX7_PLATFORM_TEXT_CLIPBOARD_H_
#define BLOX7_PLATFORM_TEXT_CLIPBOARD_H_

#include <memory>
#include <string>

#include "blox7/status.h"

namespace blox7 {
namespace platform {

class TextClipboard {
 public:
  virtual ~TextClipboard() = default;

  virtual Status ReadText(std::string* text) = 0;
  virtual Status WriteText(const std::string& text) = 0;
};

std::unique_ptr<TextClipboard> CreateSdlTextClipboard();

}  // namespace platform
}  // namespace blox7

#endif  // BLOX7_PLATFORM_TEXT_CLIPBOARD_H_
