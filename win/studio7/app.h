#pragma once

#include <string>

#include "../update/update.h"
#include "studio7.h"

namespace studio7 {

std::wstring widen(const char* text);
bool windows_seven();
bool launcher_open();
void start_background();
std::wstring start_failure(const Run& run);
bool self_updates(const Paths& paths);
bool update_self(const Paths& paths);
int run_launcher(HINSTANCE instance, int show, const Paths& paths, const std::wstring& arguments, bool open_when_ready,
                 const std::wstring& failure, bool webview_started);
int render_ui(const std::wstring& dir);
std::wstring check_cursor(const std::wstring& file);
std::wstring check_font(const std::wstring& file);
std::wstring check_icon(const std::wstring& file);
std::wstring save_icon(const std::wstring& source, const std::wstring& target);

}  // namespace studio7
