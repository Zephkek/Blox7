#ifndef BLOX7_RUNTIME_ROBLOX_NATIVE_TEXT_BOX_INFO_READER_H_
#define BLOX7_RUNTIME_ROBLOX_NATIVE_TEXT_BOX_INFO_READER_H_

#include <jni.h>

#include "blox7/status.h"

namespace blox7 {
namespace runtime {

struct RobloxNativeTextBoxInfoSnapshot {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  float font_size = 0.0F;
  bool multiline = false;
  int x_alignment = 0;
  int y_alignment = 0;
  int text_color = 0;
  int font = 0;
  int text_input_type = 0;
  int return_key_type = 0;
  bool manual_focus_release = false;
  bool text_wrapped = false;
};

struct RobloxNativeTextBoxInfoQueryResult {
  bool available = false;
  RobloxNativeTextBoxInfoSnapshot info;
};

using RobloxNativeGetTextBoxInfoFn = jobject (*)(JNIEnv*, jclass);

Status QueryRobloxNativeTextBoxInfo(
    JNIEnv* env, jclass native_gl_class,
    RobloxNativeGetTextBoxInfoFn native_get_text_box_info,
    RobloxNativeTextBoxInfoQueryResult* result);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_NATIVE_TEXT_BOX_INFO_READER_H_
