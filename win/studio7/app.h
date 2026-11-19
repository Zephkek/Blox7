#pragma once

#include <string>

#include "studio7.h"

namespace studio7 {

std::wstring widen(const char* text);
bool windows_seven();
void start_background();
std::wstring start_failure(const Run& run);
int run_launcher(HINSTANCE instance, int show, const Paths& paths, const std::wstring& arguments, bool open_when_ready,
                 const std::wstring& failure);
int render_ui(const std::wstring& dir);

}  // namespace studio7
