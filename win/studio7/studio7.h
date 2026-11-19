#ifndef BLOX7_WIN_STUDIO7_H_
#define BLOX7_WIN_STUDIO7_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S7_VERSION "0.1"
#define S7_KEEP L".s7orig"
#define S7_JOURNAL L"studio7.bind"
#define S7_READY L"studio7.ready"
#define S7_ANCHOR "S7Anchor"
#define S7_KERNEL "s7kernel.dll"
#define S7_STUDIO_EXE L"RobloxStudioBeta.exe"
#define S7_PRINTF(at, first) __attribute__((format(printf, at, first)))

typedef struct {
    WCHAR path[MAX_PATH];
    HANDLE file, mapping;
    const BYTE *data;
    DWORD size;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *sections;
    int section_count;
} Pe;

typedef struct {
    const char *dll;
    DWORD name_offset;
    DWORD thunks;
    BOOL delay;
} PeImport;

typedef struct {
    const char *name;
    WORD ordinal;
    const char *forwarder;
} PeExport;

BOOL pe_open(const WCHAR *path, Pe *pe);
void pe_close(Pe *pe);
const void *pe_at(const Pe *pe, DWORD rva, DWORD size);
const char *pe_text(const Pe *pe, DWORD rva);
int pe_imports(const Pe *pe, PeImport *out, int max);
const char *pe_import_function(const Pe *pe, const PeImport *import, int index, char *ordinal_text);
int pe_export_count(const Pe *pe);
BOOL pe_export_at(const Pe *pe, int index, PeExport *out);
BOOL pe_export_find(const Pe *pe, const char *name, PeExport *out);
BOOL pe_export_has_ordinal(const Pe *pe, DWORD ordinal);

typedef struct {
    const char *name;
    const char *target;
} StubExport;

BOOL stub_write(const WCHAR *path, const char *dll_name, const StubExport *exports, int count);

typedef void (*LogFn)(const char *text);

typedef struct {
    WCHAR studio[MAX_PATH];
    WCHAR system[MAX_PATH];
    WCHAR layer[MAX_PATH];
    LogFn log;
} LayerJob;

int layer_bind(const LayerJob *job);
int layer_unbind(const LayerJob *job);
int layer_check(const LayerJob *job);
void say(const LayerJob *job, const char *format, ...) S7_PRINTF(2, 3);
const char *utf8(const WCHAR *text, char *out, size_t room);
void text_format(char *out, size_t room, const char *format, ...) S7_PRINTF(3, 4);
void wide_format(WCHAR *out, size_t room, const WCHAR *format, ...);

typedef struct {
    volatile LONG files;
    volatile LONG64 bytes;
} ZipStats;

BOOL zip_extract(const WCHAR *zip_path, const WCHAR *to, ZipStats *stats, char *error, size_t error_size);

typedef struct {
    char name[64], md5[33];
    ULONG64 packed, unpacked;
} Package;

typedef struct {
    char version[64];
    WCHAR packages[MAX_PATH];
    WCHAR to[MAX_PATH];
    BOOL verify;
    LogFn log;
} InstallJob;

BOOL package_folder(const char *package, WCHAR *out);
int manifest_parse(const char *text, Package *out, int max);
BOOL file_md5(const WCHAR *path, char *hex);
BOOL install_unpack(const InstallJob *job, const Package *packages, int count, ZipStats *stats);

typedef BOOL (*HttpSink)(void *context, const BYTE *data, DWORD size);

typedef struct {
    int status;
    ULONG64 length, received;
    char error[300];
} HttpResult;

BOOL http_get(const char *url, HttpSink sink, void *context, ULONG64 resume_from, HttpResult *result);

typedef struct {
    WCHAR base[MAX_PATH], versions[MAX_PATH], downloads[MAX_PATH], layer[MAX_PATH], state[MAX_PATH], log[MAX_PATH];
    BOOL custom;
} Paths;

typedef struct {
    char version[96], name[64];
    ULONG64 download;
} Release;

typedef struct {
    HANDLE process;
    char version[96], name[64];
    DWORD started, shown_at, code;
    BOOL ran, shown, trusted, plain;
} Run;

enum { SETUP_OK, SETUP_FAILED, SETUP_STOPPED };
enum { CHECK_FAILED, CHECK_CURRENT, CHECK_NEWER, CHECK_REFUSED };
enum { RUN_STARTING, RUN_SHOWN, RUN_OVER, RUN_AGAIN, RUN_FAILED };

void note(const char *format, ...) S7_PRINTF(1, 2);
BOOL paths_init(Paths *p);
void windows_version(DWORD *major, DWORD *minor, DWORD *build);
void state_get(const Paths *p, const WCHAR *key, char *out, size_t room);
void state_set(const Paths *p, const WCHAR *key, const char *value);
void studio_folder(const Paths *p, const char *version, WCHAR *out);
BOOL studio_ready(const Paths *p, const char *version);
BOOL studio_running(const Paths *p);
BOOL studio_current(const Paths *paths, char *version, size_t room);
int studio_check(const Paths *paths, Release *latest, BOOL with_size);
int studio_get(const Paths *paths, const Release *wanted, BOOL wait, char *error, size_t room);
void studio_background(const Paths *paths);
void studio_forgive(const Paths *paths);
BOOL run_start(const Paths *paths, const WCHAR *arguments, Run *run);
int run_poll(const Paths *paths, Run *run);
void run_forget(Run *run);
BOOL studio_remove(const Paths *paths);
BOOL updates_automatic(const Paths *paths);
int log_tail(const Paths *paths, char lines[][200], int max);
BOOL layer_unpack(const WCHAR *folder);
BOOL layer_current(const Paths *paths, const char *version);
BOOL notices_write(const Paths *paths, WCHAR *file);

BOOL links_wanted(const Paths *paths);
BOOL links_set(const Paths *paths, BOOL on);
void links_refresh(const Paths *paths);
BOOL is_link(const WCHAR *text);
BOOL webview_version(WCHAR *out, DWORD room);

void ui_status(const char *format, ...) S7_PRINTF(1, 2);

void ui_progress(double fraction);
BOOL ui_cancelled(void);

#ifdef __cplusplus
}
#endif

#endif
