#ifndef BLOX7_WV2SETUP_H
#define BLOX7_WV2SETUP_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WV2_VERSION L"109.0.1518.140"
#define WV2_FOLDER L"Microsoft.WebView2.FixedVersionRuntime.109.0.1518.140.x64"
#define WV2_MARKER L".blox7-complete"
#define WV2_MUTEX L"Local\\Blox7.WebView2Setup"
#define WV2_STATUS L"Local\\Blox7.WebView2Setup.Status"
#define WV2_HURRY L"Local\\Blox7.WebView2Setup.Hurry"
#define WV2_SETUP_SWITCH L"--webview2-setup"

enum wv2_state {
  WV2_STATE_IDLE = 0,
  WV2_STATE_PREPARING,
  WV2_STATE_DOWNLOADING,
  WV2_STATE_UNPACKING,
  WV2_STATE_DONE,
  WV2_STATE_FAILED
};

enum wv2_error {
  WV2_OK = 0,
  WV2_ERROR_NETWORK,
  WV2_ERROR_BLOCKED,
  WV2_ERROR_PROXY_AUTH,
  WV2_ERROR_GONE,
  WV2_ERROR_MISMATCH,
  WV2_ERROR_DISK_SPACE,
  WV2_ERROR_NETWORK_DRIVE,
  WV2_ERROR_PATH_TOO_LONG,
  WV2_ERROR_MEMORY,
  WV2_ERROR_BLOCKED_FILE,
  WV2_ERROR_INTERNAL
};

typedef struct wv2_status {
  LONG state;
  LONG error;
  DWORD win32;
  DWORD pid;
  LONGLONG done;
  LONGLONG total;
} wv2_status;

int wv2_windows_major(void);
int wv2_home(wchar_t* out, DWORD capacity);
int wv2_folder(wchar_t* out, DWORD capacity);
int wv2_folder_usable(const wchar_t* folder);
int wv2_folder_complete(void);
int wv2_system_usable(void);
int wv2_needs_setup(void);
int wv2_leftover(void);
int wv2_leftover_remove(void);
int wv2_setup_process(DWORD pid);
int wv2_setup_running(void);
int wv2_status_read(wv2_status* status);
int wv2_last_error(DWORD* win32);
const wchar_t* wv2_error_text(int error);
int wv2_setup_spawn(const wchar_t* exe, int now);
int wv2_setup_run(int now);

#ifdef __cplusplus
}
#endif

#endif
