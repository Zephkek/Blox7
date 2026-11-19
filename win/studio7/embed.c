#include "studio7.h"

#define RESOURCE_DATA MAKEINTRESOURCEW(10)

static BOOL same_content(const WCHAR *path, const void *data, DWORD size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    BYTE *have;
    DWORD got = 0;
    BOOL same = FALSE;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileSize(file, NULL) == size) {
        have = malloc(size ? size : 1);
        same = have && ReadFile(file, have, size, &got, NULL) && got == size && !memcmp(have, data, size);
        free(have);
    }
    CloseHandle(file);
    return same;
}

static BOOL write_file(const WCHAR *path, const void *data, DWORD size) {
    HANDLE file;
    DWORD done = 0;
    BOOL ok;

    if (same_content(path, data, size)) return TRUE;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, data, size, &done, NULL) && done == size;
    CloseHandle(file);
    return ok;
}

static const void *resource(const WCHAR *name, DWORD *size) {
    HRSRC found = FindResourceW(NULL, name, RESOURCE_DATA);
    HGLOBAL loaded = found ? LoadResource(NULL, found) : NULL;

    *size = found ? SizeofResource(NULL, found) : 0;
    return loaded ? LockResource(loaded) : NULL;
}

static BOOL CALLBACK write_shim(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR context) {
    const WCHAR *folder = (const WCHAR *)context;
    const void *data;
    DWORD size;
    WCHAR path[MAX_PATH], lower[64];
    size_t i, length;

    (void)module;
    (void)type;
    if (IS_INTRESOURCE(name)) return TRUE;
    length = wcslen(name);
    if (length < 5 || length > 60 || _wcsicmp(name + length - 4, L".dll")) return TRUE;
    data = resource(name, &size);
    if (!data || !size) return FALSE;
    for (i = 0; i <= length; i++) lower[i] = (WCHAR)(name[i] >= L'A' && name[i] <= L'Z' ? name[i] + 32 : name[i]);
    wide_format(path, MAX_PATH, L"%ls\\%ls", folder, lower);
    return write_file(path, data, size);
}

BOOL layer_unpack(const WCHAR *folder) {
    CreateDirectoryW(folder, NULL);
    return EnumResourceNamesW(NULL, RESOURCE_DATA, write_shim, (LONG_PTR)folder) != 0;
}

static BOOL same_files(const WCHAR *a, const WCHAR *b) {
    HANDLE file = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    BYTE *data;
    BOOL same = FALSE;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(file, NULL);
    data = malloc(size ? size : 1);
    if (data && ReadFile(file, data, size, &got, NULL) && got == size) same = same_content(b, data, size);
    free(data);
    CloseHandle(file);
    return same;
}

BOOL layer_current(const Paths *paths, const char *version) {
    WCHAR pattern[MAX_PATH], from[MAX_PATH], to[MAX_PATH], folder[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    BOOL current = TRUE;

    studio_folder(paths, version, folder);
    wide_format(pattern, MAX_PATH, L"%ls\\s7*.dll", paths->layer);
    find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return TRUE;
    do {
        wide_format(from, MAX_PATH, L"%ls\\%ls", paths->layer, found.cFileName);
        wide_format(to, MAX_PATH, L"%ls\\%ls", folder, found.cFileName);
        if (!same_files(from, to)) current = FALSE;
    } while (current && FindNextFileW(find, &found));
    FindClose(find);
    return current;
}

BOOL notices_write(const Paths *paths, WCHAR *file) {
    DWORD size;
    const void *text = resource(L"NOTICES", &size);

    wide_format(file, MAX_PATH, L"%ls\\licenses.txt", paths->base);
    return text && write_file(file, text, size);
}
