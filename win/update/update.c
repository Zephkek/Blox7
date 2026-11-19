#include "update.h"

#include <bearssl.h>

#include "../studio7/studio7.h"

#ifndef UPDATE_FEED
#define UPDATE_FEED "https://api.github.com/repos/Zephkek/Blox7/releases/latest"
#endif
#ifndef UPDATE_FILES
#define UPDATE_FILES "https://github.com/Zephkek/Blox7/releases/download/"
#endif
#ifndef UPDATE_RETRY_MS
#define UPDATE_RETRY_MS 15000
#endif

typedef struct {
    char *data;
    size_t size, room;
} Text;

typedef struct {
    HANDLE file;
    br_sha256_context hash;
    ULONG64 written;
} Download;

static BOOL collect(void *context, const BYTE *data, DWORD size) {
    Text *t = context;

    if (t->size + size + 1 > t->room) {
        size_t room = (t->size + size + 1) * 2;
        char *grown = room > (2u << 20) ? NULL : realloc(t->data, room);

        if (!grown) return FALSE;
        t->data = grown;
        t->room = room;
    }
    memcpy(t->data + t->size, data, size);
    t->size += size;
    t->data[t->size] = 0;
    return TRUE;
}

static BOOL to_file(void *context, const BYTE *data, DWORD size) {
    Download *d = context;
    DWORD done = 0;

    if (!WriteFile(d->file, data, size, &done, NULL) || done != size) return FALSE;
    br_sha256_update(&d->hash, data, size);
    d->written += size;
    return TRUE;
}

static void hex(const BYTE *digest, char *out) {
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
}

static void link_time(const BYTE *head, DWORD size, FILETIME *out) {
    DWORD at, stamp;
    ULONG64 ticks;

    if (size < 0x40 || head[0] != 'M' || head[1] != 'Z') return;
    memcpy(&at, head + 0x3c, 4);
    if (at > size - 12 || memcmp(head + at, "PE\0", 4)) return;
    memcpy(&stamp, head + at + 8, 4);
    if (!stamp) return;
    ticks = (ULONG64)stamp * 10000000 + 116444736000000000ull;
    out->dwLowDateTime = (DWORD)ticks;
    out->dwHighDateTime = (DWORD)(ticks >> 32);
}

static BOOL file_digest(const WCHAR *path, char *out, FILETIME *built) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    br_sha256_context hash;
    BYTE *block, digest[32];
    DWORD got = 0;
    BOOL ok = TRUE, first = TRUE;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    block = malloc(1 << 20);
    if (!block) {
        CloseHandle(file);
        return FALSE;
    }
    br_sha256_init(&hash);
    GetFileTime(file, NULL, NULL, built);
    while ((ok = ReadFile(file, block, 1 << 20, &got, NULL)) && got) {
        if (first) link_time(block, got, built);
        first = FALSE;
        br_sha256_update(&hash, block, got);
    }
    CloseHandle(file);
    free(block);
    br_sha256_out(&hash, digest);
    hex(digest, out);
    return ok;
}

static const char *value_of(const char *from, const char *until, const char *key) {
    const char *at = strstr(from, key);

    return at && at < until ? at + strlen(key) : NULL;
}

static BOOL copy_text(const char *at, char *out, size_t room) {
    size_t n = 0;

    if (!at) return FALSE;
    for (; *at && *at != '"'; at++) {
        if (n + 1 >= room || *at == '\\' || (unsigned char)*at < 32) return FALSE;
        out[n++] = *at;
    }
    out[n] = 0;
    return *at == '"' && n > 0;
}

static BOOL moment(const char *text, FILETIME *out) {
    SYSTEMTIME t;
    int year, month, day, hour, minute, second;

    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day, &hour, &minute, &second) != 6) return FALSE;
    memset(&t, 0, sizeof(t));
    t.wYear = (WORD)year;
    t.wMonth = (WORD)month;
    t.wDay = (WORD)day;
    t.wHour = (WORD)hour;
    t.wMinute = (WORD)minute;
    t.wSecond = (WORD)second;
    return SystemTimeToFileTime(&t, out) != 0;
}

static BOOL read_feed(const char *json, const char *asset, UpdateInfo *info) {
    char key[96], stamp[32], digest[80];
    const char *entry, *end, *at;

    memset(info, 0, sizeof(*info));
    if (!copy_text(value_of(json, json + strlen(json), "\"tag_name\":\""), info->tag, sizeof(info->tag))) return FALSE;
    text_format(key, sizeof(key), "\"name\":\"%s\"", asset);
    entry = strstr(json, key);
    end = entry ? strstr(entry, "\"browser_download_url\":\"") : NULL;
    if (!end) return FALSE;
    if (!copy_text(end + strlen("\"browser_download_url\":\""), info->url, sizeof(info->url))) return FALSE;
    if (strncmp(info->url, UPDATE_FILES, strlen(UPDATE_FILES))) return FALSE;
    at = value_of(entry, end, "\"size\":");
    if (!at) return FALSE;
    info->size = _strtoui64(at, NULL, 10);
    if (!copy_text(value_of(entry, end, "\"digest\":\""), digest, sizeof(digest)) || strncmp(digest, "sha256:", 7) ||
        strlen(digest) != 7 + 64)
        return FALSE;
    strcpy(info->digest, digest + 7);
    return copy_text(value_of(entry, end, "\"updated_at\":\""), stamp, sizeof(stamp)) && moment(stamp, &info->uploaded) && info->size > 0;
}

static BOOL fetch(const char *url, HttpSink sink, void *context, HttpResult *result) {
    int tries;

    for (tries = 1; !http_get(url, sink, context, 0, result); tries++) {
        if (result->status || tries == 3 || ui_cancelled()) return FALSE;
        Sleep(UPDATE_RETRY_MS);
    }
    return TRUE;
}

int update_check(const char *asset, const WCHAR *exe, UpdateInfo *info, char *error, size_t room) {
    Text feed = {NULL, 0, 0};
    HttpResult result;
    FILETIME built;
    char ours[65];
    BOOL read;

    if (!fetch(UPDATE_FEED, collect, &feed, &result) || !feed.data) {
        text_format(error, room, "%s", result.error[0] ? result.error : "no answer");
        free(feed.data);
        return UPDATE_FAILED;
    }
    read = read_feed(feed.data, asset, info);
    free(feed.data);
    if (!read) {
        text_format(error, room, "the release list has no usable entry for %s", asset);
        return UPDATE_FAILED;
    }
    if (!file_digest(exe, ours, &built)) {
        text_format(error, room, "cannot read this program's own file (error %lu)", GetLastError());
        return UPDATE_FAILED;
    }
    if (!_stricmp(ours, info->digest) || CompareFileTime(&info->uploaded, &built) <= 0) return UPDATE_CURRENT;
    return UPDATE_NEWER;
}

void update_tidy(const WCHAR *exe) {
    WCHAR path[MAX_PATH + 8];

    if (wcslen(exe) >= MAX_PATH) return;
    wide_format(path, MAX_PATH + 8, L"%ls.old", exe);
    DeleteFileW(path);
    wide_format(path, MAX_PATH + 8, L"%ls.new", exe);
    DeleteFileW(path);
}

BOOL update_install(const UpdateInfo *info, const WCHAR *exe, char *error, size_t room) {
    WCHAR fresh[MAX_PATH + 8], old[MAX_PATH + 8];
    Download d;
    HttpResult result;
    BYTE digest[32], head[2] = {0, 0};
    char got[65];
    DWORD done = 0;
    BOOL fetched, saved = FALSE;

    if (wcslen(exe) >= MAX_PATH) return FALSE;
    wide_format(fresh, MAX_PATH + 8, L"%ls.new", exe);
    wide_format(old, MAX_PATH + 8, L"%ls.old", exe);
    memset(&d, 0, sizeof(d));
    d.file = CreateFileW(fresh, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (d.file == INVALID_HANDLE_VALUE) {
        text_format(error, room, "cannot write next to the program (error %lu)", GetLastError());
        return FALSE;
    }
    br_sha256_init(&d.hash);
    fetched = fetch(info->url, to_file, &d, &result);
    br_sha256_out(&d.hash, digest);
    hex(digest, got);
    if (fetched) {
        SetFilePointer(d.file, 0, NULL, FILE_BEGIN);
        ReadFile(d.file, head, 2, &done, NULL);
        saved = FlushFileBuffers(d.file);
    }
    CloseHandle(d.file);
    if (!fetched) {
        text_format(error, room, "%s", result.error[0] ? result.error : "the download stopped");
    } else if (d.written != info->size || _stricmp(got, info->digest) || head[0] != 'M' || head[1] != 'Z') {
        text_format(error, room, "the downloaded file is not the published one (%I64u of %I64u bytes)", d.written, info->size);
        fetched = FALSE;
    } else if (!saved) {
        text_format(error, room, "the new program could not be written to the disk");
        fetched = FALSE;
    }
    if (!fetched) {
        DeleteFileW(fresh);
        return FALSE;
    }
    DeleteFileW(old);
    if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING)) {
        text_format(error, room, "cannot move the running program aside (error %lu)", GetLastError());
        DeleteFileW(fresh);
        return FALSE;
    }
    if (!MoveFileExW(fresh, exe, 0)) {
        text_format(error, room, "cannot put the new program in place (error %lu)", GetLastError());
        MoveFileExW(old, exe, 0);
        DeleteFileW(fresh);
        return FALSE;
    }
    return TRUE;
}
