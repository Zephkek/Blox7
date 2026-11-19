#include "studio7.h"
#include <zlib.h>

#define OUT_BUFFER (256 * 1024)

static DWORD u32(const BYTE *p) {
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static WORD u16(const BYTE *p) {
    return (WORD)(p[0] | (p[1] << 8));
}

static BOOL safe_name(const BYTE *name, DWORD length, WCHAR *out, BOOL *is_dir) {
    DWORD i = 0, n = 0, part = 0;

    while (i < length && (name[i] == '/' || name[i] == '\\')) i++;
    *is_dir = length && (name[length - 1] == '/' || name[length - 1] == '\\');
    for (; i < length; i++) {
        BYTE c = name[i];

        if (c == '/' || c == '\\') {
            if (!part || (part <= 2 && out[n - 1] == L'.' && (part == 1 || out[n - 2] == L'.'))) return FALSE;
            if (i + 1 == length) break;
            out[n++] = L'\\';
            part = 0;
            continue;
        }
        if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c >= 128)
            return FALSE;
        if (n >= MAX_PATH - 80) return FALSE;
        out[n++] = c;
        part++;
    }
    if (part && part <= 2 && out[n - 1] == L'.' && (part == 1 || out[n - 2] == L'.')) return FALSE;
    out[n] = 0;
    return TRUE;
}

static BOOL make_dirs(WCHAR *path) {
    WCHAR *p;

    if (CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
    for (p = path + 3; *p; p++) {
        if (*p != L'\\') continue;
        *p = 0;
        if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            *p = L'\\';
            return FALSE;
        }
        *p = L'\\';
    }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static BOOL write_all(HANDLE file, const BYTE *data, DWORD size) {
    DWORD done = 0;

    return !size || (WriteFile(file, data, size, &done, NULL) && done == size);
}

static BOOL unpack_entry(HANDLE file, const BYTE *data, DWORD packed, DWORD unpacked, WORD method, DWORD crc,
                         BYTE *buffer) {
    z_stream z;
    uLong sum = crc32(0, NULL, 0);
    DWORD total = 0;
    int status;

    if (method == 0) {
        if (packed != unpacked || !write_all(file, data, packed)) return FALSE;
        return (DWORD)crc32(sum, data, packed) == crc;
    }
    if (method != 8) return FALSE;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return FALSE;
    z.next_in = (Bytef *)data;
    z.avail_in = packed;
    do {
        DWORD got;

        z.next_out = buffer;
        z.avail_out = OUT_BUFFER;
        status = inflate(&z, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) break;
        got = OUT_BUFFER - z.avail_out;
        if (got) {
            sum = crc32(sum, buffer, got);
            total += got;
            if (!write_all(file, buffer, got)) {
                status = Z_ERRNO;
                break;
            }
        } else if (status == Z_OK && !z.avail_in) {
            status = Z_DATA_ERROR;
            break;
        }
    } while (status != Z_STREAM_END);
    inflateEnd(&z);
    return status == Z_STREAM_END && total == unpacked && (DWORD)sum == crc;
}

BOOL zip_extract(const WCHAR *zip_path, const WCHAR *to, ZipStats *stats, char *error, size_t error_size) {
    HANDLE file, mapping = NULL, out;
    const BYTE *data = NULL, *end, *entry;
    BYTE *buffer = NULL;
    LARGE_INTEGER size;
    WCHAR path[MAX_PATH], relative[MAX_PATH], last_dir[MAX_PATH] = L"";
    char shown[MAX_PATH * 3];
    DWORD count, i, directory, problem;
    BOOL ok = FALSE;
    size_t root;

    if (error_size) error[0] = 0;
    file = CreateFileW(zip_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        text_format(error, error_size, "cannot open the package (error %lu)", GetLastError());
        return FALSE;
    }
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 22 || size.QuadPart > 0xFFFFFFFFll) goto bad;
    mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    data = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : NULL;
    buffer = malloc(OUT_BUFFER);
    if (!data || !buffer) goto bad;
    end = data + size.QuadPart;
    for (entry = end - 22; entry >= data && entry > end - 66000; entry--)
        if (u32(entry) == 0x06054b50) break;
    if (entry < data || u32(entry) != 0x06054b50) goto bad;
    count = u16(entry + 10);
    directory = u32(entry + 16);
    if (directory >= (DWORD)size.QuadPart) goto bad;
    lstrcpynW(path, to, MAX_PATH - 100);
    root = wcslen(path);
    if (!make_dirs(path)) {
        text_format(error, error_size, "cannot create the folder (error %lu)", GetLastError());
        goto done;
    }
    entry = data + directory;
    for (i = 0; i < count; i++) {
        DWORD name_length, extra, comment, packed, unpacked, crc, local;
        WORD method, flags;
        const BYTE *header;
        WCHAR *slash;
        BOOL is_dir;

        if (entry + 46 > end || u32(entry) != 0x02014b50) goto bad;
        flags = u16(entry + 8);
        method = u16(entry + 10);
        crc = u32(entry + 16);
        packed = u32(entry + 20);
        unpacked = u32(entry + 24);
        name_length = u16(entry + 28);
        extra = u16(entry + 30);
        comment = u16(entry + 32);
        local = u32(entry + 42);
        if (entry + 46 + name_length > end || (flags & 1)) goto bad;
        if (!safe_name(entry + 46, name_length, relative, &is_dir)) {
            text_format(error, error_size, "the package has a file name that is not allowed");
            goto done;
        }
        entry += 46 + name_length + extra + comment;
        if (!relative[0]) continue;
        if (root + wcslen(relative) + 2 > MAX_PATH) {
            text_format(error, error_size, "the folder's path is too long for %s", utf8(relative, shown, sizeof(shown)));
            goto done;
        }
        path[root] = L'\\';
        lstrcpynW(path + root + 1, relative, (int)(MAX_PATH - root - 1));
        if (is_dir) {
            if (!make_dirs(path)) goto cannot_write;
            continue;
        }
        header = data + local;
        if (local > (DWORD)size.QuadPart - 30 || u32(header) != 0x04034b50) goto bad;
        header += 30 + u16(header + 26) + u16(header + 28);
        if (header > end || packed > (DWORD)(end - header)) goto bad;
        slash = wcsrchr(path, L'\\');
        *slash = 0;
        if (wcscmp(path, last_dir)) {
            if (!make_dirs(path)) goto cannot_write;
            lstrcpynW(last_dir, path, MAX_PATH);
        }
        *slash = L'\\';
        out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) goto cannot_write;
        is_dir = unpack_entry(out, header, packed, unpacked, method, crc, buffer);
        CloseHandle(out);
        if (!is_dir) {
            DeleteFileW(path);
            text_format(error, error_size, "%s did not unpack correctly", utf8(relative, shown, sizeof(shown)));
            goto done;
        }
        if (stats) {
            InterlockedIncrement(&stats->files);
            InterlockedExchangeAdd64(&stats->bytes, unpacked);
        }
    }
    ok = TRUE;
    goto done;
cannot_write:
    problem = GetLastError();
    text_format(error, error_size, "cannot write %s (error %lu)", utf8(path, shown, sizeof(shown)), problem);
    goto done;
bad:
    text_format(error, error_size, "the package is not a readable zip file");
done:
    free(buffer);
    if (data) UnmapViewOfFile(data);
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    return ok;
}
