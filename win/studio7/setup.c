#include "studio7.h"
#include <share.h>
#include <shlobj.h>

#define VERSION_URL "https://clientsettingscdn.roblox.com/v2/client-version/WindowsStudio64"
#define PACKAGE_HOST "https://setup.rbxcdn.com/"
#define MAX_PACKAGES 128
#define DOWNLOADERS 4

typedef struct {
    char *data;
    size_t size, room;
} Text;

typedef struct {
    const Paths *paths;
    const char *version;
    const Package *packages;
    int count;
    volatile LONG next, failed;
    volatile LONG64 done, fetched;
    ULONG64 total;
    char error[400];
    CRITICAL_SECTION lock;
} Download;

typedef struct {
    Download *download;
    HANDLE file;
} Part;

static FILE *g_log;
static CRITICAL_SECTION g_log_lock;
static BOOL g_log_ready;
static HANDLE g_turn;

const char *utf8(const WCHAR *text, char *out, size_t room) {
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)room, NULL, NULL)) out[room - 1] = 0;
    return out;
}

void text_format(char *out, size_t room, const char *format, ...) {
    va_list args;

    va_start(args, format);
    _vsnprintf(out, room - 1, format, args);
    va_end(args);
    out[room - 1] = 0;
}

void wide_format(WCHAR *out, size_t room, const WCHAR *format, ...) {
    va_list args;

    va_start(args, format);
    _vsnwprintf(out, room - 1, format, args);
    va_end(args);
    out[room - 1] = 0;
}

void note(const char *format, ...) {
    char text[900];
    SYSTEMTIME t;
    va_list args;

    va_start(args, format);
    _vsnprintf(text, sizeof(text) - 1, format, args);
    text[sizeof(text) - 1] = 0;
    va_end(args);
    if (!g_log_ready) return;
    EnterCriticalSection(&g_log_lock);
    if (g_log) {
        GetLocalTime(&t);
        fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds, text);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_log_lock);
}

static void note_line(const char *text) {
    note("%s", text);
}

static void join(WCHAR *out, const WCHAR *folder, const WCHAR *name) {
    wide_format(out, MAX_PATH, L"%ls\\%ls", folder, name);
}

static BOOL exists(const WCHAR *path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

BOOL paths_init(Paths *p) {
    static const WCHAR section[] = L"\xFEFF[studio]\r\n";
    WCHAR local[MAX_PATH], old[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA info;
    DWORD length = GetEnvironmentVariableW(L"STUDIO7_ROOT", p->base, MAX_PATH - 120), done;
    HANDLE file;

    p->custom = length > 0 && length < MAX_PATH - 120;
    if (!p->custom) {
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, local)) ||
            wcslen(local) > MAX_PATH - 130)
            return FALSE;
        join(p->base, local, L"Studio7");
    }
    join(p->versions, p->base, L"Versions");
    join(p->downloads, p->base, L"Downloads");
    join(p->layer, p->base, L"layer");
    join(p->state, p->base, L"studio7.ini");
    join(p->log, p->base, L"studio7.log");
    CreateDirectoryW(p->base, NULL);
    CreateDirectoryW(p->versions, NULL);
    file = CreateFileW(p->state, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, section, sizeof(section) - sizeof(WCHAR), &done, NULL);
        CloseHandle(file);
    }
    if (!g_log_ready) {
        InitializeCriticalSection(&g_log_lock);
        g_log_ready = TRUE;
        if (GetFileAttributesExW(p->log, GetFileExInfoStandard, &info) && info.nFileSizeLow > 1024 * 1024) {
            join(old, p->base, L"studio7.old.log");
            MoveFileExW(p->log, old, MOVEFILE_REPLACE_EXISTING);
        }
        g_log = _wfsopen(p->log, L"a", _SH_DENYNO);
    }
    return exists(p->versions);
}

void windows_version(DWORD *major, DWORD *minor, DWORD *build) {
    typedef LONG(WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn get = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW info;

    memset(&info, 0, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    if (get) get(&info);
    *major = info.dwMajorVersion;
    *minor = info.dwMinorVersion;
    *build = info.dwBuildNumber;
}

void state_get(const Paths *p, const WCHAR *key, char *out, size_t room) {
    WCHAR value[160];

    GetPrivateProfileStringW(L"studio", key, L"", value, 160, p->state);
    utf8(value, out, room);
}

void state_set(const Paths *p, const WCHAR *key, const char *value) {
    WCHAR wide[160];

    if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, wide, 160)) wide[0] = 0;
    WritePrivateProfileStringW(L"studio", key, wide[0] ? wide : NULL, p->state);
}

BOOL updates_automatic(const Paths *p) {
    char value[8];

    state_get(p, L"updates", value, sizeof(value));
    return strcmp(value, "off") != 0;
}

void studio_folder(const Paths *p, const char *version, WCHAR *out) {
    wide_format(out, MAX_PATH, L"%ls\\%hs", p->versions, version);
}

BOOL studio_ready(const Paths *p, const char *version) {
    WCHAR folder[MAX_PATH], marker[MAX_PATH];

    if (!version[0]) return FALSE;
    studio_folder(p, version, folder);
    join(marker, folder, S7_READY);
    return exists(marker);
}

BOOL studio_current(const Paths *p, char *version, size_t room) {
    state_get(p, L"version", version, room);
    return studio_ready(p, version);
}

static BOOL take_turn(BOOL wait) {
    DWORD result = WAIT_TIMEOUT;
    BOOL told = FALSE;

    if (!g_turn) g_turn = CreateMutexW(NULL, FALSE, L"Local\\Studio7.Setup");
    if (!g_turn) return TRUE;
    while (!ui_cancelled()) {
        result = WaitForSingleObject(g_turn, wait ? 250 : 0);
        if (result != WAIT_TIMEOUT || !wait) break;
        if (!told) ui_status("Another Studio7 is setting up Roblox Studio, waiting for it...");
        told = TRUE;
    }
    return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

static void give_turn(void) {
    if (g_turn) ReleaseMutex(g_turn);
}

static BOOL collect(void *context, const BYTE *data, DWORD size) {
    Text *t = context;

    if (t->size + size + 1 > t->room) {
        size_t room = (t->size + size + 1) * 2;
        char *grown = room > (4u << 20) ? NULL : realloc(t->data, room);

        if (!grown) return FALSE;
        t->data = grown;
        t->room = room;
    }
    memcpy(t->data + t->size, data, size);
    t->size += size;
    t->data[t->size] = 0;
    return TRUE;
}

static char *fetch_text(const char *url, char *error, size_t room) {
    Text t = {NULL, 0, 0};
    HttpResult result;
    int attempt;

    result.error[0] = 0;
    for (attempt = 0; attempt < 3 && !ui_cancelled(); attempt++) {
        t.size = 0;
        if (http_get(url, collect, &t, 0, &result) && t.data) return t.data;
        if (attempt < 2) Sleep(700 * (attempt + 1));
    }
    text_format(error, room, "%s", result.error[0] ? result.error : "stopped");
    free(t.data);
    return NULL;
}

static BOOL field(const char *json, const char *name, char *out, size_t room) {
    char key[64];
    const char *at;
    size_t n = 0;

    text_format(key, sizeof(key), "\"%s\":\"", name);
    at = strstr(json, key);
    if (!at) return FALSE;
    for (at += strlen(key); *at && *at != '"'; at++) {
        char c = *at;

        if (n + 1 >= room || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
            return FALSE;
        out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

static int fetch_packages(const char *version, Package *packages, char *error, size_t room) {
    char url[300], *text;
    int count;

    text_format(url, sizeof(url), PACKAGE_HOST "%s-rbxPkgManifest.txt", version);
    text = fetch_text(url, error, room);
    if (!text) return 0;
    count = manifest_parse(text, packages, MAX_PACKAGES);
    free(text);
    if (count <= 0) {
        text_format(error, room, "Roblox's file list for %s could not be read", version);
        return 0;
    }
    return count;
}

static BOOL latest_release(Release *out, BOOL with_size, char *error, size_t room) {
    Package *packages;
    char *json = fetch_text(VERSION_URL, error, room);
    BOOL ok;
    int count, i;

    memset(out, 0, sizeof(*out));
    if (!json) return FALSE;
    ok = field(json, "clientVersionUpload", out->version, sizeof(out->version)) && !strncmp(out->version, "version-", 8);
    if (!field(json, "version", out->name, sizeof(out->name))) strcpy(out->name, "(unnamed)");
    free(json);
    if (!ok) {
        text_format(error, room, "Roblox's answer about the current version could not be read");
        return FALSE;
    }
    packages = with_size ? malloc(sizeof(Package) * MAX_PACKAGES) : NULL;
    if (packages) {
        count = fetch_packages(out->version, packages, error, room);
        for (i = 0; i < count; i++) out->download += packages[i].packed;
        free(packages);
    }
    return TRUE;
}

static BOOL refused(const Paths *paths, const char *version) {
    char which[96], by[16];

    state_get(paths, L"refused", which, sizeof(which));
    state_get(paths, L"refused_by", by, sizeof(by));
    return which[0] && !strcmp(which, version) && !strcmp(by, S7_VERSION);
}

static void refuse(const Paths *paths, const char *version) {
    state_set(paths, L"refused", version);
    state_set(paths, L"refused_by", S7_VERSION);
}

void studio_forgive(const Paths *paths) {
    state_set(paths, L"refused", "");
    state_set(paths, L"refused_by", "");
}

int studio_check(const Paths *paths, Release *latest, BOOL with_size) {
    char current[96], error[400];

    if (!latest_release(latest, with_size, error, sizeof(error))) {
        note("could not ask Roblox for the current version: %s", error);
        return CHECK_FAILED;
    }
    if (studio_current(paths, current, sizeof(current)) && !strcmp(current, latest->version)) return CHECK_CURRENT;
    return refused(paths, latest->version) ? CHECK_REFUSED : CHECK_NEWER;
}

static BOOL to_part(void *context, const BYTE *data, DWORD size) {
    Part *part = context;
    DWORD done = 0;

    if (ui_cancelled() || part->download->failed) return FALSE;
    if (!WriteFile(part->file, data, size, &done, NULL) || done != size) return FALSE;
    InterlockedExchangeAdd64(&part->download->done, size);
    InterlockedExchangeAdd64(&part->download->fetched, size);
    return TRUE;
}

static void fail(Download *d, const char *format, ...) S7_PRINTF(2, 3);

static void fail(Download *d, const char *format, ...) {
    va_list args;

    EnterCriticalSection(&d->lock);
    if (!d->failed) {
        va_start(args, format);
        _vsnprintf(d->error, sizeof(d->error) - 1, format, args);
        va_end(args);
        d->failed = 1;
    }
    LeaveCriticalSection(&d->lock);
}

static DWORD WINAPI download_worker(LPVOID context) {
    Download *d = context;

    for (;;) {
        LONG i = InterlockedIncrement(&d->next) - 1;
        const Package *p;
        WCHAR final[MAX_PATH], partial[MAX_PATH];
        char url[400], md5[33], shown[MAX_PATH * 3];
        WIN32_FILE_ATTRIBUTE_DATA info;
        int attempt;
        BOOL ok = FALSE;

        if (i >= d->count || d->failed || ui_cancelled()) break;
        p = &d->packages[i];
        wide_format(final, MAX_PATH - 6, L"%ls\\%hs-%hs", d->paths->downloads, d->version, p->name);
        wide_format(partial, MAX_PATH, L"%ls.part", final);
        text_format(url, sizeof(url), PACKAGE_HOST "%s-%s", d->version, p->name);
        if (GetFileAttributesExW(final, GetFileExInfoStandard, &info) &&
            (((ULONG64)info.nFileSizeHigh << 32) | info.nFileSizeLow) == p->packed && file_md5(final, md5) &&
            !_stricmp(md5, p->md5)) {
            InterlockedExchangeAdd64(&d->done, (LONG64)p->packed);
            continue;
        }
        for (attempt = 0; attempt < 4 && !ok && !d->failed && !ui_cancelled(); attempt++) {
            HttpResult result;
            Part part;
            ULONG64 have = 0;
            LARGE_INTEGER size;

            result.received = 0;
            result.error[0] = 0;
            part.download = d;
            part.file = CreateFileW(partial, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (part.file == INVALID_HANDLE_VALUE) {
                fail(d, "cannot write to %s (error %lu)", utf8(d->paths->downloads, shown, sizeof(shown)), GetLastError());
                break;
            }
            if (GetFileSizeEx(part.file, &size) && size.QuadPart > 0 && (ULONG64)size.QuadPart < p->packed && attempt < 3) {
                have = (ULONG64)size.QuadPart;
                SetFilePointerEx(part.file, size, NULL, FILE_BEGIN);
            } else {
                SetFilePointer(part.file, 0, NULL, FILE_BEGIN);
                SetEndOfFile(part.file);
            }
            InterlockedExchangeAdd64(&d->done, (LONG64)have);
            ok = http_get(url, to_part, &part, have, &result);
            size.QuadPart = 0;
            GetFileSizeEx(part.file, &size);
            CloseHandle(part.file);
            if (ok && ((ULONG64)size.QuadPart != p->packed || !file_md5(partial, md5) || _stricmp(md5, p->md5))) {
                note("%s arrived damaged, fetching it again", p->name);
                ok = FALSE;
                DeleteFileW(partial);
            }
            if (ok) continue;
            InterlockedExchangeAdd64(&d->done, -(LONG64)(result.received + have));
            if (ui_cancelled()) break;
            if (attempt == 3) fail(d, "%s: %s", p->name, result.error[0] ? result.error : "the file arrived damaged");
            else Sleep(800 * (attempt + 1));
        }
        if (ok && !MoveFileExW(partial, final, MOVEFILE_REPLACE_EXISTING))
            fail(d, "cannot finish %s (error %lu)", p->name, GetLastError());
    }
    return 0;
}

static void drop_other_downloads(const Paths *paths, const char *version) {
    WCHAR pattern[MAX_PATH], path[MAX_PATH], prefix[100];
    WIN32_FIND_DATAW found;
    HANDLE find;

    wide_format(prefix, 100, L"%hs-", version);
    join(pattern, paths->downloads, L"version-*");
    find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!_wcsnicmp(found.cFileName, prefix, wcslen(prefix))) continue;
        join(path, paths->downloads, found.cFileName);
        DeleteFileW(path);
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

static BOOL download_all(const Paths *paths, const char *version, const Package *packages, int count, char *error,
                         size_t room) {
    Download d;
    HANDLE threads[DOWNLOADERS];
    DWORD started = GetTickCount();
    int i, running = 0;

    memset(&d, 0, sizeof(d));
    d.paths = paths;
    d.version = version;
    d.packages = packages;
    d.count = count;
    for (i = 0; i < count; i++) d.total += packages[i].packed;
    InitializeCriticalSection(&d.lock);
    CreateDirectoryW(paths->downloads, NULL);
    drop_other_downloads(paths, version);
    for (i = 0; i < DOWNLOADERS; i++) {
        threads[running] = CreateThread(NULL, 0, download_worker, &d, 0, NULL);
        if (threads[running]) running++;
    }
    if (!running) fail(&d, "cannot start a download thread (error %lu)", GetLastError());
    while (running && WaitForMultipleObjects((DWORD)running, threads, TRUE, 200) == WAIT_TIMEOUT) {
        double seconds = (GetTickCount() - started) / 1000.0, done = (double)d.done;

        ui_progress(d.total ? done / (double)d.total * 0.9 : 0);
        ui_status("Downloading, %.0f of %.0f MB (%.1f MB/s)", done / 1048576.0, (double)d.total / 1048576.0,
                  seconds > 0.5 ? (double)d.fetched / 1048576.0 / seconds : 0.0);
    }
    for (i = 0; i < running; i++) CloseHandle(threads[i]);
    DeleteCriticalSection(&d.lock);
    if (ui_cancelled()) return FALSE;
    if (d.failed) {
        text_format(error, room, "%s", d.error);
        return FALSE;
    }
    note("downloaded %.0f MB in %.1f s", (double)d.total / 1048576.0, (GetTickCount() - started) / 1000.0);
    return TRUE;
}

void tree_remove(const WCHAR *dir) {
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;

    join(pattern, dir, L"*");
    find = FindFirstFileW(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            join(path, dir, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(path);
            else if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(path);
            else tree_remove(path);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    RemoveDirectoryW(dir);
}

static BOOL in_use(const WCHAR *folder) {
    WCHAR exe[MAX_PATH];
    HANDLE file;

    join(exe, folder, S7_STUDIO_EXE);
    file = CreateFileW(exe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(file);
    return FALSE;
}

static int bind(const Paths *paths, const char *version) {
    LayerJob job;
    WCHAR marker[MAX_PATH];
    HANDLE file;
    int missing;

    memset(&job, 0, sizeof(job));
    job.log = note_line;
    studio_folder(paths, version, job.studio);
    GetSystemDirectoryW(job.system, MAX_PATH);
    lstrcpynW(job.layer, paths->layer, MAX_PATH);
    join(marker, job.studio, S7_READY);
    DeleteFileW(marker);
    if (!layer_unpack(paths->layer)) return -1;
    missing = layer_bind(&job);
    if (missing) return missing;
    file = CreateFileW(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(file);
    return 0;
}

static void refresh(const Paths *paths, const char *version) {
    WCHAR folder[MAX_PATH];

    studio_folder(paths, version, folder);
    if (!layer_unpack(paths->layer) || layer_current(paths, version) || in_use(folder) || !take_turn(FALSE)) return;
    if (studio_ready(paths, version) && !layer_current(paths, version)) {
        note("the layer changed, preparing Studio %s again", version);
        if (!bind(paths, version)) studio_forgive(paths);
    }
    give_turn();
}

static BOOL room_for(const Paths *paths, const Package *packages, int count, char *error, size_t room) {
    ULARGE_INTEGER available;
    ULONG64 needed = 64u << 20;
    int i;

    for (i = 0; i < count; i++) needed += packages[i].packed + packages[i].unpacked;
    if (!GetDiskFreeSpaceExW(paths->base, &available, NULL, NULL) || available.QuadPart >= needed) return TRUE;
    text_format(error, room, "the disk is too full: %.1f GB needed, %.1f GB free", (double)needed / 1073741824.0,
                (double)available.QuadPart / 1073741824.0);
    return FALSE;
}

static int install(const Paths *paths, const Release *release, char *error, size_t room) {
    Package *packages = malloc(sizeof(Package) * MAX_PACKAGES);
    InstallJob job;
    ZipStats stats;
    WCHAR known[160];
    DWORD started;
    int count = 0, i, missing = 0, result = SETUP_FAILED;

    memset(&job, 0, sizeof(job));
    job.log = note_line;
    lstrcpynA(job.version, release->version, sizeof(job.version));
    lstrcpynW(job.packages, paths->downloads, MAX_PATH);
    studio_folder(paths, release->version, job.to);
    if (!packages) {
        text_format(error, room, "out of memory");
    } else if (in_use(job.to)) {
        text_format(error, room, "Roblox Studio %s is open from the folder it would go into, close it first", release->name);
    } else {
        ui_status("Getting the list of files...");
        ui_progress(-1);
        count = fetch_packages(release->version, packages, error, room);
    }
    for (i = 0; i < count; i++) {
        if (package_folder(packages[i].name, known)) continue;
        note("Studio %s has a package Studio7 does not know: %s", release->name, packages[i].name);
        missing++;
    }
    if (count && !missing && room_for(paths, packages, count, error, room)) {
        ui_status("Downloading...");
        ui_progress(0);
        if (download_all(paths, release->version, packages, count, error, room)) {
            ui_status("Unpacking...");
            tree_remove(job.to);
            started = GetTickCount();
            if (install_unpack(&job, packages, count, &stats)) {
                note("unpacked %ld files, %.0f MB in %.1f s", stats.files, (double)stats.bytes / 1048576.0,
                     (GetTickCount() - started) / 1000.0);
                ui_status("Preparing Roblox Studio for this Windows...");
                ui_progress(0.99);
                missing = bind(paths, release->version);
                if (!missing) result = SETUP_OK;
                else if (missing < 0) text_format(error, room, "Roblox Studio could not be prepared (see the log)");
            } else {
                text_format(error, room, "unpacking failed (see the log)");
            }
        }
    }
    free(packages);
    if (missing > 0) {
        refuse(paths, release->version);
        text_format(error, room, "Roblox Studio %s does not work with this Studio7 yet", release->name);
    }
    if (result != SETUP_OK && count) tree_remove(job.to);
    if (result == SETUP_OK || missing > 0) tree_remove(paths->downloads);
    if (result != SETUP_OK && ui_cancelled()) return SETUP_STOPPED;
    ui_progress(1.0);
    return result;
}

static int each_version(const Paths *paths, const char *keep, const char *keep_too, BOOL remove) {
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    int busy = 0;

    join(pattern, paths->versions, L"version-*");
    find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        char name[96];

        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        utf8(found.cFileName, name, sizeof(name));
        join(path, paths->versions, found.cFileName);
        if (in_use(path)) {
            busy++;
        } else if (remove && _stricmp(name, keep) && _stricmp(name, keep_too)) {
            note("removing Studio %s", name);
            tree_remove(path);
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return busy;
}

BOOL studio_running(const Paths *paths) {
    return each_version(paths, "", "", FALSE) > 0;
}

static void remove_old_versions(const Paths *paths) {
    char current[96], previous[96];

    state_get(paths, L"version", current, sizeof(current));
    state_get(paths, L"previous", previous, sizeof(previous));
    if (current[0]) each_version(paths, current, previous, TRUE);
}

static BOOL proven(const Paths *paths, const char *version) {
    char which[96];

    state_get(paths, L"proven", which, sizeof(which));
    return !strcmp(which, version);
}

static void switch_to(const Paths *paths, const Release *release) {
    char current[96], name[64];

    state_get(paths, L"version", current, sizeof(current));
    if (!strcmp(current, release->version)) return;
    state_get(paths, L"name", name, sizeof(name));
    if (proven(paths, current) && studio_ready(paths, current)) {
        state_set(paths, L"previous", current);
        state_set(paths, L"previous_name", name);
    }
    state_set(paths, L"version", release->version);
    state_set(paths, L"name", release->name);
}

static BOOL fall_back(const Paths *paths, const char *failed) {
    char current[96], previous[96], name[64];

    state_get(paths, L"version", current, sizeof(current));
    state_get(paths, L"previous", previous, sizeof(previous));
    state_get(paths, L"previous_name", name, sizeof(name));
    if (strcmp(current, failed)) return studio_ready(paths, current);
    if (!previous[0] || !strcmp(previous, failed) || !studio_ready(paths, previous)) return FALSE;
    note("going back to Studio %s (%s)", name, previous);
    refuse(paths, failed);
    state_set(paths, L"version", previous);
    state_set(paths, L"name", name);
    state_set(paths, L"previous", "");
    state_set(paths, L"previous_name", "");
    return TRUE;
}

static void trust(const Paths *paths, const Run *run) {
    char current[96];

    note("Studio %s runs on this Windows", run->name);
    state_get(paths, L"version", current, sizeof(current));
    state_set(paths, L"proven", run->version);
    state_set(paths, L"previous", strcmp(current, run->version) ? run->version : "");
    state_set(paths, L"previous_name", strcmp(current, run->version) ? run->name : "");
}

int studio_get(const Paths *paths, const Release *wanted, BOOL wait, char *error, size_t room) {
    Release release;
    int result = SETUP_OK;

    error[0] = 0;
    if (!take_turn(wait)) return SETUP_STOPPED;
    if (wanted) {
        release = *wanted;
    } else {
        ui_status("Asking Roblox for the current version...");
        ui_progress(-1);
        if (!latest_release(&release, FALSE, error, room)) result = ui_cancelled() ? SETUP_STOPPED : SETUP_FAILED;
    }
    if (result == SETUP_OK && refused(paths, release.version)) {
        text_format(error, room, "Roblox Studio %s does not work with this Studio7 yet", release.name);
        result = SETUP_FAILED;
    }
    if (result == SETUP_OK && !studio_ready(paths, release.version)) {
        note("getting Studio %s (%s)", release.name, release.version);
        result = install(paths, &release, error, room);
    }
    if (result == SETUP_OK) {
        switch_to(paths, &release);
        note("Studio %s (%s) is ready", release.name, release.version);
        remove_old_versions(paths);
    } else if (result == SETUP_FAILED) {
        note("setup failed: %s", error);
    }
    give_turn();
    return result;
}

void studio_background(const Paths *paths) {
    Release latest;
    char error[400];
    int answer;

    if (!updates_automatic(paths)) return;
    answer = studio_check(paths, &latest, FALSE);
    if (answer == CHECK_NEWER) {
        studio_get(paths, &latest, FALSE, error, sizeof(error));
    } else if (answer != CHECK_FAILED && take_turn(FALSE)) {
        remove_old_versions(paths);
        give_turn();
    }
}

BOOL run_start(const Paths *paths, const WCHAR *arguments, Run *run) {
    WCHAR folder[MAX_PATH], exe[MAX_PATH], *command;
    char shown[MAX_PATH * 3];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    size_t room;

    memset(run, 0, sizeof(*run));
    if (!studio_current(paths, run->version, sizeof(run->version))) return FALSE;
    refresh(paths, run->version);
    if (!studio_ready(paths, run->version)) return FALSE;
    state_get(paths, L"name", run->name, sizeof(run->name));
    studio_folder(paths, run->version, folder);
    if (!in_use(folder)) mods_apply(paths, run->version);
    join(exe, folder, S7_STUDIO_EXE);
    room = wcslen(exe) + (arguments ? wcslen(arguments) : 0) + 8;
    command = malloc(sizeof(WCHAR) * room);
    if (!command) return FALSE;
    wide_format(command, room, L"\"%ls\"%ls%ls", exe, arguments && arguments[0] ? L" " : L"", arguments ? arguments : L"");
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, folder, &startup, &process)) {
        CloseHandle(process.hThread);
        run->process = process.hProcess;
        run->started = GetTickCount();
        run->ran = TRUE;
        run->trusted = proven(paths, run->version);
        run->plain = !arguments || !arguments[0];
    } else {
        run->code = GetLastError();
        note("could not start %s (error %lu)", utf8(exe, shown, sizeof(shown)), run->code);
    }
    free(command);
    return run->process != NULL;
}

static BOOL CALLBACK real_window(HWND window, LPARAM context) {
    WCHAR kind[32];
    DWORD pid = 0;

    GetWindowThreadProcessId(window, &pid);
    if (pid != (DWORD)context || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    if (!GetClassNameW(window, kind, 32) || !wcscmp(kind, L"#32770")) return TRUE;
    SetLastError(ERROR_SUCCESS);
    return FALSE;
}

void run_forget(Run *run) {
    if (run->process) CloseHandle(run->process);
    run->process = NULL;
}

int run_poll(const Paths *paths, Run *run) {
    DWORD now = GetTickCount();

    if (!run->process) return RUN_OVER;
    if (WaitForSingleObject(run->process, 0) == WAIT_OBJECT_0) {
        GetExitCodeProcess(run->process, &run->code);
        run_forget(run);
        if (run->trusted) return RUN_OVER;
        if (!run->code && (run->shown || !run->plain || studio_running(paths))) return RUN_OVER;
        note("Studio %s stopped %.1f s after it was started (code 0x%lX)", run->name, (now - run->started) / 1000.0,
             run->code);
        return fall_back(paths, run->version) ? RUN_AGAIN : RUN_FAILED;
    }
    if (!run->shown) {
        SetLastError(ERROR_INVALID_DATA);
        if (EnumWindows(real_window, (LPARAM)GetProcessId(run->process)) || GetLastError() != ERROR_SUCCESS)
            return RUN_STARTING;
        run->shown = TRUE;
        run->shown_at = now;
    }
    if (!run->trusted && now - run->shown_at >= 20000) {
        trust(paths, run);
        run->trusted = TRUE;
    }
    return RUN_SHOWN;
}

BOOL studio_remove(const Paths *paths) {
    static const WCHAR *files[] = {L"studio7.ini", L"studio7.log", L"studio7.old.log", L"licenses.txt", L"window.ico"};
    static const WCHAR *empty[] = {L"Mods\\content", L"Mods"};
    WCHAR path[MAX_PATH];
    size_t i;

    if (!take_turn(FALSE)) return FALSE;
    if (studio_running(paths)) {
        give_turn();
        return FALSE;
    }
    links_set(paths, FALSE);
    EnterCriticalSection(&g_log_lock);
    if (g_log) fclose(g_log);
    g_log = NULL;
    LeaveCriticalSection(&g_log_lock);
    tree_remove(paths->versions);
    tree_remove(paths->downloads);
    tree_remove(paths->layer);
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        join(path, paths->base, files[i]);
        DeleteFileW(path);
    }
    for (i = 0; i < sizeof(empty) / sizeof(empty[0]); i++) {
        join(path, paths->base, empty[i]);
        RemoveDirectoryW(path);
    }
    RemoveDirectoryW(paths->base);
    give_turn();
    return TRUE;
}

int log_tail(const Paths *paths, char lines[][200], int max) {
    HANDLE file = CreateFileW(paths->log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    char text[4096], *at, *end;
    LARGE_INTEGER size, from;
    DWORD got = 0;
    int count = 0;

    if (file == INVALID_HANDLE_VALUE) return 0;
    from.QuadPart = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > (LONGLONG)sizeof(text) - 1) from.QuadPart = size.QuadPart - (sizeof(text) - 1);
    SetFilePointerEx(file, from, NULL, FILE_BEGIN);
    if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL)) got = 0;
    CloseHandle(file);
    text[got] = 0;
    end = text + got;
    while (end > text && count < max) {
        while (end > text && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
        for (at = end; at > text && at[-1] != '\n';) at--;
        if (at == text && from.QuadPart) break;
        if (end - at > 24) lstrcpynA(lines[count++], at + 24, 200);
        end = at;
    }
    return count;
}
