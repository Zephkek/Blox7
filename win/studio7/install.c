#include "studio7.h"
#include <wincrypt.h>

static const struct {
    const char *package;
    const WCHAR *folder;
} g_folders[] = {
    {"RobloxStudio.zip", L""},
    {"Libraries.zip", L""},
    {"LibrariesQt5.zip", L""},
    {"redist.zip", L""},
    {"WebView2.zip", L""},
    {"WebView2RuntimeInstaller.zip", L"WebView2RuntimeInstaller"},
    {"ApplicationConfig.zip", L"ApplicationConfig"},
    {"BuiltInPlugins.zip", L"BuiltInPlugins"},
    {"BuiltInStandalonePlugins.zip", L"BuiltInStandalonePlugins"},
    {"Plugins.zip", L"Plugins"},
    {"Qml.zip", L"Qml"},
    {"StudioFonts.zip", L"StudioFonts"},
    {"shaders.zip", L"shaders"},
    {"ssl.zip", L"ssl"},
    {"content-avatar.zip", L"content\\avatar"},
    {"content-configs.zip", L"content\\configs"},
    {"content-fonts.zip", L"content\\fonts"},
    {"content-models.zip", L"content\\models"},
    {"content-qt_translations.zip", L"content\\qt_translations"},
    {"content-sky.zip", L"content\\sky"},
    {"content-sounds.zip", L"content\\sounds"},
    {"content-studio_svg_textures.zip", L"content\\studio_svg_textures"},
    {"content-api-docs.zip", L"content\\api_docs"},
    {"content-textures2.zip", L"content\\textures"},
    {"content-textures3.zip", L"PlatformContent\\pc\\textures"},
    {"content-terrain.zip", L"PlatformContent\\pc\\terrain"},
    {"content-platform-fonts.zip", L"PlatformContent\\pc\\fonts"},
    {"content-platform-dictionaries.zip", L"PlatformContent\\pc\\shared_compression_dictionaries"},
    {"extracontent-scripts.zip", L"ExtraContent\\scripts"},
    {"extracontent-luapackages.zip", L"ExtraContent\\LuaPackages"},
    {"extracontent-translations.zip", L"ExtraContent\\translations"},
    {"extracontent-models.zip", L"ExtraContent\\models"},
    {"extracontent-textures.zip", L"ExtraContent\\textures"},
    {"extracontent-places.zip", L"ExtraContent\\places"},
    {"studiocontent-models.zip", L"StudioContent\\models"},
    {"studiocontent-textures.zip", L"StudioContent\\textures"},
};

BOOL package_folder(const char *package, WCHAR *out) {
    size_t i;

    for (i = 0; i < sizeof(g_folders) / sizeof(g_folders[0]); i++) {
        if (!_stricmp(g_folders[i].package, package)) {
            wcscpy(out, g_folders[i].folder);
            return TRUE;
        }
    }
    return FALSE;
}

int manifest_parse(const char *text, Package *out, int max) {
    char line[4][160];
    int count = 0, field = 0, n = 0;
    BOOL first = TRUE;
    const char *p;

    for (p = text;; p++) {
        if (*p && *p != '\n') {
            if (*p != '\r' && n < 159) line[field][n++] = *p;
            continue;
        }
        line[field][n] = 0;
        n = 0;
        if (first) {
            if (strcmp(line[0], "v0")) return -1;
            first = FALSE;
        } else if (line[field][0] && ++field == 4) {
            if (count == max || strlen(line[0]) > 63 || strlen(line[1]) != 32) return -1;
            strcpy(out[count].name, line[0]);
            strcpy(out[count].md5, line[1]);
            out[count].packed = _strtoui64(line[2], NULL, 10);
            out[count].unpacked = _strtoui64(line[3], NULL, 10);
            count++;
            field = 0;
        }
        if (!*p) break;
    }
    return field ? -1 : count;
}

BOOL file_md5(const WCHAR *path, char *hex) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    BYTE *buffer = malloc(1 << 20), digest[16];
    DWORD got, size = sizeof(digest), i;
    BOOL ok = FALSE;

    if (file != INVALID_HANDLE_VALUE && buffer &&
        CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) {
        ok = TRUE;
        while (ok && ReadFile(file, buffer, 1 << 20, &got, NULL) && got) ok = CryptHashData(hash, buffer, got, 0);
        ok = ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0);
        for (i = 0; ok && i < 16; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(buffer);
    return ok;
}

typedef struct {
    const InstallJob *job;
    const Package *packages;
    int count;
    volatile LONG next, failed;
    ZipStats stats;
    CRITICAL_SECTION lock;
} Unpack;

static void report(Unpack *u, const char *format, ...) {
    char text[600];
    va_list args;

    va_start(args, format);
    _vsnprintf(text, sizeof(text) - 1, format, args);
    text[sizeof(text) - 1] = 0;
    va_end(args);
    EnterCriticalSection(&u->lock);
    if (u->job->log) u->job->log(text);
    LeaveCriticalSection(&u->lock);
}

static BOOL find_package(const InstallJob *job, const char *name, WCHAR *path) {
    wide_format(path, MAX_PATH, L"%ls\\%hs-%hs", job->packages, job->version, name);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return TRUE;
    wide_format(path, MAX_PATH, L"%ls\\%hs", job->packages, name);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI unpack_worker(LPVOID context) {
    Unpack *u = context;

    for (;;) {
        LONG i = InterlockedIncrement(&u->next) - 1;
        const Package *p;
        WCHAR zip[MAX_PATH], folder[160], to[MAX_PATH];
        char error[400], md5[33];

        if (i >= u->count || u->failed || ui_cancelled()) break;
        p = &u->packages[i];
        if (!package_folder(p->name, folder)) {
            report(u, "this Studio build has a package Studio7 does not know: %s", p->name);
            InterlockedExchange(&u->failed, 1);
            break;
        }
        if (!find_package(u->job, p->name, zip)) {
            report(u, "package %s is missing", p->name);
            InterlockedExchange(&u->failed, 1);
            break;
        }
        if (u->job->verify && (!file_md5(zip, md5) || _stricmp(md5, p->md5))) {
            report(u, "package %s is damaged (checksum differs)", p->name);
            InterlockedExchange(&u->failed, 1);
            break;
        }
        if (folder[0]) wide_format(to, MAX_PATH, L"%ls\\%ls", u->job->to, folder);
        else lstrcpynW(to, u->job->to, MAX_PATH);
        if (!zip_extract(zip, to, &u->stats, error, sizeof(error))) {
            report(u, "%s: %s", p->name, error);
            InterlockedExchange(&u->failed, 1);
            break;
        }
    }
    return 0;
}

static BOOL write_text(const WCHAR *path, const char *text) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done = 0;
    BOOL ok;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, text, (DWORD)strlen(text), &done, NULL);
    CloseHandle(file);
    return ok;
}

BOOL install_unpack(const InstallJob *job, const Package *packages, int count, ZipStats *stats) {
    Unpack u;
    HANDLE threads[8];
    SYSTEM_INFO info;
    WCHAR path[MAX_PATH];
    ULONG64 total = 0;
    int wanted, workers = 0, i;

    memset(&u, 0, sizeof(u));
    u.job = job;
    u.packages = packages;
    u.count = count;
    for (i = 0; i < count; i++) total += packages[i].unpacked;
    InitializeCriticalSection(&u.lock);
    GetSystemInfo(&info);
    wanted = (int)info.dwNumberOfProcessors;
    if (wanted > 6) wanted = 6;
    if (wanted < 1) wanted = 1;
    if (wanted > count) wanted = count;
    for (i = 0; i < wanted; i++) {
        threads[workers] = CreateThread(NULL, 0, unpack_worker, &u, 0, NULL);
        if (threads[workers]) workers++;
    }
    if (!workers) InterlockedExchange(&u.failed, 1);
    while (workers && WaitForMultipleObjects((DWORD)workers, threads, TRUE, 100) == WAIT_TIMEOUT)
        if (total) ui_progress(0.9 + 0.09 * (double)u.stats.bytes / (double)total);
    for (i = 0; i < workers; i++) CloseHandle(threads[i]);
    DeleteCriticalSection(&u.lock);
    if (stats) *stats = u.stats;
    if (u.failed || ui_cancelled()) return FALSE;
    wide_format(path, MAX_PATH, L"%ls\\AppSettings.xml", job->to);
    return write_text(path, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<Settings>\r\n"
                            "\t<ContentFolder>content</ContentFolder>\r\n"
                            "\t<BaseUrl>http://www.roblox.com</BaseUrl>\r\n</Settings>\r\n");
}
