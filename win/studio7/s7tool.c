#include "studio7.h"

static void print(const char *text) {
    printf("%s\n", text);
    fflush(stdout);
}

void ui_status(const char *format, ...) {
    va_list args;

    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void ui_progress(double fraction) {
    (void)fraction;
}

BOOL ui_cancelled(void) {
    return FALSE;
}


static int usage(void) {
    print("s7tool latest");
    print("s7tool get <address> <file>");
    print("s7tool unpack <packages folder> <studio folder> [--version <id>] [--no-verify]");
    print("s7tool bind <studio folder> --layer <folder> [--system <folder>]");
    print("s7tool unbind <studio folder> --layer <folder>");
    print("s7tool check <studio folder> [--system <folder>]");
    print("s7tool links on|off|show");
    print("s7tool mods show|apply|undo");
    print("s7tool mods arrow|hand|font|icon <file or nothing>");
    print("s7tool remove");
    return 2;
}

static const WCHAR *option(int argc, WCHAR **argv, const WCHAR *name) {
    int i;

    for (i = 2; i + 1 < argc; i++)
        if (!wcscmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static BOOL flag(int argc, WCHAR **argv, const WCHAR *name) {
    int i;

    for (i = 2; i < argc; i++)
        if (!wcscmp(argv[i], name)) return TRUE;
    return FALSE;
}

static void full(WCHAR *out, const WCHAR *path) {
    size_t length;

    if (!GetFullPathNameW(path, MAX_PATH, out, NULL)) lstrcpynW(out, path, MAX_PATH);
    length = wcslen(out);
    if (length > 3 && out[length - 1] == L'\\') out[length - 1] = 0;
}

static char *read_text(const WCHAR *path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *text;

    if (file == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(file, NULL);
    text = size < (1 << 20) ? malloc(size + 1) : NULL;
    if (text && ReadFile(file, text, size, &got, NULL)) text[got] = 0;
    CloseHandle(file);
    return text;
}

static double seconds(void) {
    LARGE_INTEGER frequency, now;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}

static BOOL to_file(void *context, const BYTE *data, DWORD size) {
    DWORD done = 0;

    return WriteFile((HANDLE)context, data, size, &done, NULL) && done == size;
}

static int get(int argc, WCHAR **argv) {
    HttpResult result;
    char url[1024];
    HANDLE file;
    double start = seconds(), took;
    BOOL ok;

    if (argc < 4) return usage();
    utf8(argv[2], url, sizeof(url));
    file = CreateFileW(argv[3], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 1;
    ok = http_get(url, to_file, file, 0, &result);
    CloseHandle(file);
    took = seconds() - start;
    printf("%s: status %d, %.0f bytes in %.2f s (%.1f MB/s)%s%s\n", ok ? "ok" : "FAILED", result.status,
           (double)result.received, took, (double)result.received / 1048576.0 / took, result.error[0] ? ": " : "",
           result.error);
    return ok ? 0 : 1;
}

static int unpack(int argc, WCHAR **argv) {
    static Package packages[128];
    InstallJob job;
    ZipStats stats;
    WCHAR path[MAX_PATH];
    const WCHAR *version = option(argc, argv, L"--version");
    char *text;
    double start = seconds();
    int count;
    BOOL ok;

    if (argc < 4) return usage();
    memset(&job, 0, sizeof(job));
    job.log = print;
    job.verify = !flag(argc, argv, L"--no-verify");
    full(job.packages, argv[2]);
    full(job.to, argv[3]);
    if (version) utf8(version, job.version, sizeof(job.version));
    wide_format(path, MAX_PATH, L"%ls\\%hs%lsrbxPkgManifest.txt", job.packages, job.version, version ? L"-" : L"");
    text = read_text(path);
    if (!text) {
        wide_format(path, MAX_PATH, L"%ls\\rbxPkgManifest.txt", job.packages);
        text = read_text(path);
    }
    count = text ? manifest_parse(text, packages, 128) : -1;
    free(text);
    if (count <= 0) {
        print("no readable rbxPkgManifest.txt in the packages folder");
        return 1;
    }
    ok = install_unpack(&job, packages, count, &stats);
    printf("%d packages, %ld files, %.0f MB in %.1f s\n", count, stats.files, (double)stats.bytes / 1048576.0,
           seconds() - start);
    return ok ? 0 : 1;
}

static int latest(void) {
    Paths paths;
    Release release;
    int answer;

    if (!paths_init(&paths)) return 1;
    answer = studio_check(&paths, &release, TRUE);
    if (answer == CHECK_FAILED) {
        print("FAILED (see studio7.log)");
        return 1;
    }
    printf("%s %s, %.0f MB to download, %s\n", release.version, release.name, (double)release.download / 1048576.0,
           answer == CHECK_CURRENT ? "installed" : answer == CHECK_REFUSED ? "refused" : "not installed");
    return 0;
}

static int links(int argc, WCHAR **argv) {
    Paths paths;
    WCHAR version[64];

    if (argc < 3 || !paths_init(&paths)) return usage();
    if (!wcscmp(argv[2], L"on") || !wcscmp(argv[2], L"off")) {
        if (!links_set(&paths, !wcscmp(argv[2], L"on"))) {
            print("FAILED");
            return 1;
        }
    }
    printf("links %s, WebView2 %ls\n", links_wanted(&paths) ? "on" : "off", webview_version(version, 64) ? version : L"missing");
    return 0;
}

static int mods(int argc, WCHAR **argv) {
    static const WCHAR *names[MOD_COUNT] = {L"arrow", L"hand", L"font", L"icon"};
    Paths paths;
    WCHAR value[MAX_PATH], studio[MAX_PATH];
    char version[96];
    int i, done;

    if (argc < 3 || !paths_init(&paths)) return usage();
    for (i = 0; i < MOD_COUNT; i++) {
        if (wcscmp(argv[2], names[i])) continue;
        mods_set(&paths, i, argc > 3 ? argv[3] : NULL);
        mods_get(&paths, i, value);
        printf("%ls = %ls\n", names[i], value);
        return 0;
    }
    if (!wcscmp(argv[2], L"show")) {
        for (i = 0; i < MOD_COUNT; i++) {
            mods_get(&paths, i, value);
            printf("%ls = %ls\n", names[i], value);
        }
        mods_folder(&paths, value);
        printf("folder = %ls\n", value);
        return 0;
    }
    if (!studio_current(&paths, version, sizeof(version))) {
        print("FAILED (no Studio is installed here)");
        return 1;
    }
    if (!wcscmp(argv[2], L"apply")) {
        done = mods_apply(&paths, version);
        printf("%d\n", done);
        return done < 0;
    }
    if (!wcscmp(argv[2], L"undo")) {
        studio_folder(&paths, version, studio);
        done = mods_undo(studio);
        print(done ? "ok" : "FAILED (a replaced file is in use)");
        return !done;
    }
    return usage();
}

static int remove_all(void) {
    Paths paths;
    BOOL ok = paths_init(&paths) && studio_remove(&paths);

    print(ok ? "ok" : "FAILED (Roblox Studio is open, or another Studio7 is busy)");
    return ok ? 0 : 1;
}

int wmain(int argc, WCHAR **argv) {
    LayerJob job;
    const WCHAR *value;
    int problems;

    if (argc < 2) return usage();
    if (!wcscmp(argv[1], L"latest")) return latest();
    if (!wcscmp(argv[1], L"remove")) return remove_all();
    if (!wcscmp(argv[1], L"links")) return links(argc, argv);
    if (!wcscmp(argv[1], L"mods")) return mods(argc, argv);
    if (!wcscmp(argv[1], L"get")) return get(argc, argv);
    if (!wcscmp(argv[1], L"unpack")) {
        problems = unpack(argc, argv);
        if (problems != 2) print(problems ? "FAILED" : "ok");
        return problems;
    }
    if (argc < 3) return usage();
    memset(&job, 0, sizeof(job));
    job.log = print;
    full(job.studio, argv[2]);
    value = option(argc, argv, L"--system");
    if (value) full(job.system, value);
    else GetSystemDirectoryW(job.system, MAX_PATH);
    value = option(argc, argv, L"--layer");
    if (value) full(job.layer, value);
    if (!wcscmp(argv[1], L"check")) problems = layer_check(&job);
    else if (!value) return usage();
    else if (!wcscmp(argv[1], L"bind")) problems = layer_bind(&job);
    else if (!wcscmp(argv[1], L"unbind")) problems = layer_unbind(&job);
    else return usage();
    print(problems ? "FAILED" : "ok");
    return problems ? 1 : 0;
}
