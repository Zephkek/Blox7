#include <stdio.h>
#include <windows.h>
int main(void) {
  wchar_t v[260];
  printf("plain mingw exe runs; BLOX7_BOOT_LOG=%s\n", GetEnvironmentVariableW(L"BLOX7_BOOT_LOG", v, 260) ? "set" : "unset");
  return 5;
}
