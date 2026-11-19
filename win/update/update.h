#ifndef BLOX7_WIN_UPDATE_H_
#define BLOX7_WIN_UPDATE_H_

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char tag[40], digest[65], url[512];
    ULONG64 size;
    FILETIME uploaded;
} UpdateInfo;

enum { UPDATE_FAILED, UPDATE_CURRENT, UPDATE_NEWER };

int update_check(const char *asset, const WCHAR *exe, UpdateInfo *info, char *error, size_t room);
BOOL update_install(const UpdateInfo *info, const WCHAR *exe, char *error, size_t room);
void update_tidy(const WCHAR *exe);

#ifdef __cplusplus
}
#endif

#endif
