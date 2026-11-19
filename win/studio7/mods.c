#include "studio7.h"
#include <shlobj.h>

#define MODS_JOURNAL L"studio7.mods"
#define MODS_KEEP L"studio7.keep"
#define MAX_SWAPS 4096
#define MAX_DEPTH 12

typedef struct {
    WCHAR relative[MAX_PATH], source[MAX_PATH];
    BOOL added;
} Swap;

typedef struct {
    Swap *items;
    int count;
} Swaps;

static const WCHAR *const g_keys[MOD_COUNT] = {L"arrow", L"hand", L"font", L"icon"};
static const WCHAR *const g_arrow[] = {L"content\\textures\\Cursors\\KeyboardMouse\\ArrowFarCursor.png",
                                       L"content\\textures\\ArrowFarCursor.png", NULL};
static const WCHAR *const g_hand[] = {L"content\\textures\\Cursors\\KeyboardMouse\\ArrowCursor.png",
                                      L"content\\textures\\ArrowCursor.png", NULL};
static const WCHAR *const g_font[] = {L"content\\fonts\\BuilderSans-Regular.otf", L"content\\fonts\\BuilderSans-Medium.otf",
                                      L"content\\fonts\\BuilderSans-Bold.otf", L"content\\fonts\\BuilderSans-ExtraBold.otf", NULL};
static const WCHAR *const g_media[] = {L".png", L".jpg", L".jpeg", L".tga", L".dds", L".ktx", L".ttf", L".otf",
                                       L".ogg", L".mp3", L".wav"};

static BOOL exists(const WCHAR *path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL join(WCHAR *out, const WCHAR *folder, const WCHAR *name) {
    if (wcslen(folder) + wcslen(name) + 2 > MAX_PATH) return FALSE;
    wide_format(out, MAX_PATH, L"%ls\\%ls", folder, name);
    return TRUE;
}

static void make_parent(const WCHAR *file) {
    WCHAR folder[MAX_PATH];
    WCHAR *slash;

    lstrcpynW(folder, file, MAX_PATH);
    slash = wcsrchr(folder, L'\\');
    if (!slash) return;
    *slash = 0;
    SHCreateDirectoryExW(NULL, folder, NULL);
}

void mods_folder(const Paths *paths, WCHAR *out) {
    wide_format(out, MAX_PATH, L"%ls\\Mods", paths->base);
}

void mods_icon(const Paths *paths, WCHAR *out) {
    wide_format(out, MAX_PATH, L"%ls\\window.ico", paths->base);
}

void mods_get(const Paths *paths, int which, WCHAR *out) {
    GetPrivateProfileStringW(L"mods", g_keys[which], L"", out, MAX_PATH, paths->state);
}

void mods_set(const Paths *paths, int which, const WCHAR *file) {
    WritePrivateProfileStringW(L"mods", g_keys[which], file && file[0] ? file : NULL, paths->state);
}

static BOOL safe(const WCHAR *relative) {
    return relative[0] && relative[0] != L'\\' && !wcschr(relative, L':') && !wcsstr(relative, L"..");
}

BOOL mods_undo(const WCHAR *studio) {
    WCHAR journal[MAX_PATH], keep[MAX_PATH], relative[MAX_PATH], target[MAX_PATH], kept[MAX_PATH];
    char line[MAX_PATH * 3 + 8];
    int stuck = 0;
    FILE *f;

    if (!join(journal, studio, MODS_JOURNAL) || !join(keep, studio, MODS_KEEP)) return FALSE;
    f = _wfopen(journal, L"rb");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            size_t length = strlen(line);

            while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
            if (length < 2 || !MultiByteToWideChar(CP_UTF8, 0, line + 1, -1, relative, MAX_PATH) || !safe(relative)) continue;
            if (!join(target, studio, relative) || !join(kept, keep, relative)) continue;
            if (line[0] == '+') {
                stuck += exists(target) && !DeleteFileW(target);
            } else if (exists(kept)) {
                SetFileAttributesW(target, FILE_ATTRIBUTE_NORMAL);
                stuck += !MoveFileExW(kept, target, MOVEFILE_REPLACE_EXISTING);
            }
        }
        fclose(f);
        if (stuck) return FALSE;
        DeleteFileW(journal);
    }
    if (exists(keep)) tree_remove(keep);
    return TRUE;
}

static BOOL add(Swaps *swaps, const WCHAR *relative, const WCHAR *source, BOOL added) {
    int i;

    if (swaps->count == MAX_SWAPS || !safe(relative) || wcslen(relative) >= MAX_PATH || wcslen(source) >= MAX_PATH) return FALSE;
    for (i = 0; i < swaps->count; i++)
        if (!_wcsicmp(swaps->items[i].relative, relative)) return FALSE;
    lstrcpynW(swaps->items[swaps->count].relative, relative, MAX_PATH);
    lstrcpynW(swaps->items[swaps->count].source, source, MAX_PATH);
    swaps->items[swaps->count].added = added;
    swaps->count++;
    return TRUE;
}

static void add_setting(Swaps *swaps, const Paths *paths, const WCHAR *studio, int which, const WCHAR *const *targets) {
    WCHAR source[MAX_PATH], target[MAX_PATH];

    mods_get(paths, which, source);
    if (!source[0] || !exists(source)) return;
    for (; *targets; targets++)
        if (join(target, studio, *targets) && exists(target)) add(swaps, *targets, source, FALSE);
}

static BOOL media(const WCHAR *name) {
    const WCHAR *dot = wcsrchr(name, L'.');
    size_t i;

    if (!dot) return FALSE;
    for (i = 0; i < sizeof(g_media) / sizeof(g_media[0]); i++)
        if (!_wcsicmp(dot, g_media[i])) return TRUE;
    return FALSE;
}

static void add_folder(Swaps *swaps, const WCHAR *studio, const WCHAR *mods, const WCHAR *under, int depth) {
    WCHAR pattern[MAX_PATH], relative[MAX_PATH], source[MAX_PATH], target[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;

    if (under[0] ? !join(source, mods, under) : !lstrcpynW(source, mods, MAX_PATH)) return;
    if (!join(pattern, source, L"*")) return;
    find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (under[0] ? !join(relative, under, found.cFileName) : !lstrcpynW(relative, found.cFileName, MAX_PATH)) continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < MAX_DEPTH) add_folder(swaps, studio, mods, relative, depth + 1);
        } else if (media(found.cFileName) && join(source, mods, relative) && join(target, studio, relative) && exists(target)) {
            add(swaps, relative, source, FALSE);
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

static BOOL swap_in(const WCHAR *studio, const WCHAR *journal, const Swap *swap) {
    WCHAR target[MAX_PATH], kept[MAX_PATH], keep[MAX_PATH], fresh[MAX_PATH];
    char relative[MAX_PATH * 3];
    FILE *f;

    if (!join(target, studio, swap->relative) || !join(keep, studio, MODS_KEEP) || !join(kept, keep, swap->relative)) return FALSE;
    if (wcslen(kept) + 5 > MAX_PATH) return FALSE;
    f = _wfopen(journal, L"ab");
    if (!f) return FALSE;
    fprintf(f, "%c%s\n", swap->added ? '+' : '=', utf8(swap->relative, relative, sizeof(relative)));
    fclose(f);
    if (!swap->added) {
        wide_format(fresh, MAX_PATH, L"%ls.new", kept);
        make_parent(kept);
        if (!CopyFileW(target, fresh, FALSE) || !MoveFileExW(fresh, kept, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(fresh);
            return FALSE;
        }
        SetFileAttributesW(target, FILE_ATTRIBUTE_NORMAL);
    }
    return CopyFileW(swap->source, target, FALSE) != 0;
}

int mods_apply(const Paths *paths, const char *version) {
    WCHAR studio[MAX_PATH], journal[MAX_PATH], folder[MAX_PATH], icon[MAX_PATH], chosen[MAX_PATH];
    Swaps swaps = {NULL, 0};
    int i, done = 0;

    studio_folder(paths, version, studio);
    if (!join(journal, studio, MODS_JOURNAL)) return 0;
    if (!mods_undo(studio)) {
        note("mods: a replaced file is in use, nothing changed");
        return -1;
    }
    swaps.items = calloc(MAX_SWAPS, sizeof(Swap));
    if (!swaps.items) return 0;
    add_setting(&swaps, paths, studio, MOD_ARROW, g_arrow);
    add_setting(&swaps, paths, studio, MOD_HAND, g_hand);
    add_setting(&swaps, paths, studio, MOD_FONT, g_font);
    mods_get(paths, MOD_ICON, chosen);
    mods_icon(paths, icon);
    if (chosen[0] && exists(icon)) add(&swaps, S7_ICON, icon, TRUE);
    mods_folder(paths, folder);
    if (exists(folder)) add_folder(&swaps, studio, folder, L"", 0);
    for (i = 0; i < swaps.count; i++) done += swap_in(studio, journal, &swaps.items[i]);
    if (swaps.count) note("mods: %d of %d files in place", done, swaps.count);
    free(swaps.items);
    return done;
}
