#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "../update/update.h"
#include "blox7.h"

extern "C" {

void text_format(char* out, size_t room, const char* format, ...) {
  va_list args;
  va_start(args, format);
  _vsnprintf(out, room - 1, format, args);
  va_end(args);
  out[room - 1] = 0;
}

void wide_format(WCHAR* out, size_t room, const WCHAR* format, ...) {
  va_list args;
  va_start(args, format);
  _vsnwprintf(out, room - 1, format, args);
  va_end(args);
  out[room - 1] = 0;
}

BOOL ui_cancelled(void) { return FALSE; }

}  // extern "C"

namespace blox7 {

void tidy_update(const Paths& paths) { update_tidy(paths.exe.c_str()); }

bool update_self(const Paths& paths) {
  UpdateInfo info;
  char error[400] = "";
  if (update_check("Blox7.exe", paths.exe.c_str(), &info, error, sizeof error) != UPDATE_NEWER) return false;
  return update_install(&info, paths.exe.c_str(), error, sizeof error) != FALSE;
}

}  // namespace blox7
