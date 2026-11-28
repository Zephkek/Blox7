#include "studio7.h"
#include <cpuid.h>
#include <shlobj.h>

#define LUAU_SETTINGS L"ClientAppSettings.json"
#define LUAU_LOAN L"studio7.luau"
#define LUAU_KEYS 2

typedef struct {
    size_t start, end, value;
    int key;
} Member;

typedef struct {
    char *data;
    size_t size, body, open;
    Member *members;
    int count;
    BOOL blank;
} Json;

typedef struct {
    size_t from, to;
    const char *text;
} Edit;

typedef struct {
    WCHAR file[MAX_PATH];
    BOOL created, had[LUAU_KEYS];
    char *was[LUAU_KEYS];
} Loan;

static const char *const g_names[LUAU_KEYS] = {"FFlagDebugDisableCodegen", "FFlagLuauCodegen"};
static const char *const g_values[LUAU_KEYS] = {"\"True\"", "\"False\""};

static BOOL avx_off(DWORD *osxsave, ULONG64 *xcr0) {
    unsigned a, b, c = 0, d, low = 0, high = 0;

    if (!__get_cpuid(1, &a, &b, &c, &d)) return FALSE;
    *osxsave = c >> 27 & 1;
    if (*osxsave) __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    *xcr0 = (ULONG64)high << 32 | low;
    return (c >> 28 & 1) && !(*osxsave && (low & 6) == 6);
}

static BOOL in(const char *set, char c) {
    return c && strchr(set, c);
}

static size_t space(const char *s, size_t n, size_t i) {
    while (i < n && in(" \t\r\n", s[i])) i++;
    return i;
}

static size_t text_end(const char *s, size_t n, size_t i) {
    int k;

    for (i++; i < n; i++) {
        if ((unsigned char)s[i] < 0x20) return 0;
        if (s[i] == '"') return i + 1;
        if (s[i] != '\\') continue;
        if (++i >= n) return 0;
        if (s[i] != 'u') {
            if (!in("\"\\/bfnrt", s[i])) return 0;
            continue;
        }
        for (k = 1; k <= 4; k++)
            if (i + k >= n || !in("0123456789abcdefABCDEF", s[i + k])) return 0;
        i += 4;
    }
    return 0;
}

static size_t digits_end(const char *s, size_t n, size_t i) {
    if (i >= n || !in("0123456789", s[i])) return 0;
    while (i < n && in("0123456789", s[i])) i++;
    return i;
}

static size_t number_end(const char *s, size_t n, size_t i) {
    if (i < n && s[i] == '-') i++;
    if (i < n && s[i] == '0') i++;
    else if (!(i = digits_end(s, n, i))) return 0;
    if (i < n && s[i] == '.' && !(i = digits_end(s, n, i + 1))) return 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        if (++i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (!(i = digits_end(s, n, i))) return 0;
    }
    return i;
}

static size_t value_end(const char *s, size_t n, size_t i, int depth) {
    static const char *const words[] = {"true", "false", "null"};
    char close;
    size_t k;

    if (i >= n || depth > 64) return 0;
    if (s[i] == '"') return text_end(s, n, i);
    for (k = 0; k < 3; k++)
        if (n - i >= strlen(words[k]) && !memcmp(s + i, words[k], strlen(words[k]))) return i + strlen(words[k]);
    if (s[i] != '{' && s[i] != '[') return number_end(s, n, i);
    close = s[i] == '{' ? '}' : ']';
    i = space(s, n, i + 1);
    if (i < n && s[i] == close) return i + 1;
    for (;;) {
        if (close == '}') {
            if (i >= n || s[i] != '"' || !(i = text_end(s, n, i))) return 0;
            i = space(s, n, i);
            if (i >= n || s[i] != ':') return 0;
            i = space(s, n, i + 1);
        }
        if (!(i = value_end(s, n, i, depth + 1))) return 0;
        i = space(s, n, i);
        if (i < n && s[i] == close) return i + 1;
        if (i >= n || s[i] != ',') return 0;
        i = space(s, n, i + 1);
    }
}

static int key_of(const char *name, size_t length) {
    int k;

    for (k = 0; k < LUAU_KEYS; k++)
        if (strlen(g_names[k]) == length && !memcmp(name, g_names[k], length)) return k;
    return -1;
}

static BOOL parse(Json *j) {
    const char *s = j->data;
    size_t n = j->size, i, end;
    Member m, *grown;
    int room = 0;

    j->count = 0;
    j->body = n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3) ? 3 : 0;
    i = space(s, n, j->body);
    j->blank = i == n;
    if (j->blank) return TRUE;
    if (s[i] != '{') return FALSE;
    j->open = i;
    i = space(s, n, i + 1);
    while (i < n && s[i] != '}') {
        if (s[i] != '"' || !(end = text_end(s, n, i))) return FALSE;
        m.start = i;
        m.key = key_of(s + i + 1, end - i - 2);
        i = space(s, n, end);
        if (i >= n || s[i] != ':') return FALSE;
        m.value = space(s, n, i + 1);
        if (!(m.end = value_end(s, n, m.value, 1))) return FALSE;
        if (j->count == room) {
            room = room ? room * 2 : 32;
            grown = realloc(j->members, sizeof(Member) * room);
            if (!grown) return FALSE;
            j->members = grown;
        }
        j->members[j->count++] = m;
        i = space(s, n, m.end);
        if (i < n && s[i] == ',') {
            i = space(s, n, i + 1);
            if (i >= n || s[i] == '}') return FALSE;
        } else if (i >= n || s[i] != '}') {
            return FALSE;
        }
    }
    return i < n && space(s, n, i + 1) == n;
}

static BOOL is(const Json *j, const Member *m, const char *text) {
    return m->end - m->value == strlen(text) && !memcmp(j->data + m->value, text, m->end - m->value);
}

static char *copy(const char *s, size_t n) {
    char *out = malloc(n + 1);

    if (out) {
        memcpy(out, s, n);
        out[n] = 0;
    }
    return out;
}

static DWORD load(const WCHAR *path, Json *j) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    DWORD done = 0, error = 0;

    memset(j, 0, sizeof(*j));
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    if (!GetFileSizeEx(file, &size) || size.QuadPart > (16 << 20)) error = ERROR_FILE_TOO_LARGE;
    else if (!(j->data = malloc((size_t)size.QuadPart + 1))) error = ERROR_NOT_ENOUGH_MEMORY;
    else if (!ReadFile(file, j->data, (DWORD)size.QuadPart, &done, NULL)) error = GetLastError();
    else if (done != size.QuadPart) error = ERROR_READ_FAULT;
    CloseHandle(file);
    if (j->data) j->data[done] = 0;
    j->size = done;
    return error;
}

static void unload(Json *j) {
    free(j->data);
    free(j->members);
    memset(j, 0, sizeof(*j));
}

static DWORD save(const WCHAR *path, const char *data, size_t size) {
    WCHAR temporary[MAX_PATH];
    HANDLE file;
    DWORD done = 0, error = 0;

    if (wcslen(path) + 5 > MAX_PATH) return ERROR_FILENAME_EXCED_RANGE;
    wide_format(temporary, MAX_PATH, L"%ls.new", path);
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    if (!WriteFile(file, data, (DWORD)size, &done, NULL) || done != size || !FlushFileBuffers(file))
        error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    CloseHandle(file);
    if (!error && !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = GetLastError();
    if (error) DeleteFileW(temporary);
    return error;
}

static char *rewrite(const Json *j, const Edit *edits, int count, size_t *size) {
    size_t total = j->size - j->body, at = j->body;
    char *out, *p;
    int e;

    for (e = 0; e < count; e++) total += strlen(edits[e].text);
    out = malloc(total + 1);
    if (!out) return NULL;
    for (p = out, e = 0; e < count; e++) {
        memcpy(p, j->data + at, edits[e].from - at);
        p += edits[e].from - at;
        memcpy(p, edits[e].text, strlen(edits[e].text));
        p += strlen(edits[e].text);
        at = edits[e].to;
    }
    memcpy(p, j->data + at, j->size - at);
    p += j->size - at;
    *p = 0;
    *size = (size_t)(p - out);
    return out;
}

static void parent(const WCHAR *file, WCHAR *out) {
    WCHAR *slash;

    lstrcpynW(out, file, MAX_PATH);
    slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
}

static void loan_free(Loan *loan) {
    int k;

    for (k = 0; k < LUAU_KEYS; k++) free(loan->was[k]);
    memset(loan, 0, sizeof(*loan));
}

static BOOL loan_read(const WCHAR *path, Loan *loan) {
    Json all, was;
    char *second, *third;
    BOOL ok = FALSE;
    int i, k;

    memset(loan, 0, sizeof(*loan));
    memset(&was, 0, sizeof(was));
    if (!load(path, &all) && (second = strchr(all.data, '\n')) && (second[1] == '0' || second[1] == '1') &&
        second[2] == '\n') {
        third = second + 2;
        *second = 0;
        was.data = third + 1;
        was.size = all.size - (size_t)(was.data - all.data);
        ok = MultiByteToWideChar(CP_UTF8, 0, all.data, -1, loan->file, MAX_PATH) && loan->file[0] && parse(&was) &&
             !was.blank;
        loan->created = second[1] == '1';
        for (i = 0; ok && i < was.count; i++) {
            k = was.members[i].key;
            if (k < 0) continue;
            free(loan->was[k]);
            loan->had[k] = TRUE;
            loan->was[k] = copy(was.data + was.members[i].value, was.members[i].end - was.members[i].value);
            ok = loan->was[k] != NULL;
        }
    }
    free(was.members);
    unload(&all);
    if (!ok) loan_free(loan);
    return ok;
}

static DWORD loan_write(const WCHAR *path, const Loan *loan) {
    char file[MAX_PATH * 3], *text;
    size_t room = sizeof(file) + 16, length;
    DWORD error;
    int k;

    for (k = 0; k < LUAU_KEYS; k++)
        if (loan->had[k]) room += strlen(g_names[k]) + strlen(loan->was[k]) + 4;
    text = malloc(room);
    if (!text) return ERROR_NOT_ENOUGH_MEMORY;
    length = (size_t)sprintf(text, "%s\n%d\n{", utf8(loan->file, file, sizeof(file)), loan->created ? 1 : 0);
    for (k = 0; k < LUAU_KEYS; k++)
        if (loan->had[k])
            length += (size_t)sprintf(text + length, "%s\"%s\":%s", text[length - 1] == '{' ? "" : ",", g_names[k],
                                      loan->was[k]);
    length += (size_t)sprintf(text + length, "}\n");
    error = save(path, text, length);
    free(text);
    return error;
}

static void give_back(const Loan *loan, const WCHAR *loan_path) {
    char shown[MAX_PATH * 3], *text = NULL;
    WCHAR folder[MAX_PATH];
    Json j;
    Edit *edits = NULL;
    BOOL *gone = NULL;
    DWORD error = load(loan->file, &j);
    size_t size = 0;
    int i, k, first, count = 0, kept = 0;

    utf8(loan->file, shown, sizeof(shown));
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || (!error && !parse(&j))) {
        if (!error) note("%s is no longer JSON Studio7 can read, so it stays as it is", shown);
        unload(&j);
        DeleteFileW(loan_path);
        return;
    }
    if (!error && (!(edits = malloc(sizeof(Edit) * (j.count + 1))) || !(gone = calloc(j.count + 1, sizeof(BOOL)))))
        error = ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; !error && i < j.count; i++) {
        k = j.members[i].key;
        gone[i] = k >= 0 && !loan->had[k] && is(&j, &j.members[i], g_values[k]);
        kept += !gone[i];
    }
    for (i = 0; !error && i < j.count;) {
        k = j.members[i].key;
        if (!gone[i]) {
            if (k >= 0 && loan->had[k] && is(&j, &j.members[i], g_values[k]) && strcmp(loan->was[k], g_values[k]))
                edits[count++] = (Edit){j.members[i].value, j.members[i].end, loan->was[k]};
            i++;
            continue;
        }
        first = i;
        while (i < j.count && gone[i]) i++;
        if (first > 0) edits[count++] = (Edit){j.members[first - 1].end, j.members[i - 1].end, ""};
        else if (i < j.count) edits[count++] = (Edit){j.members[first].start, j.members[i].start, ""};
        else edits[count++] = (Edit){j.members[first].start, j.members[i - 1].end, ""};
    }
    if (!error && count && !kept && loan->created) {
        if (!DeleteFileW(loan->file)) error = GetLastError();
        parent(loan->file, folder);
        if (!error) RemoveDirectoryW(folder);
    } else if (!error && count) {
        text = rewrite(&j, edits, count, &size);
        error = text ? save(loan->file, text, size) : ERROR_NOT_ENOUGH_MEMORY;
    }
    if (error) {
        note("cannot give Luau native code back to Studio in %s (error %lu)", shown, error);
    } else {
        DeleteFileW(loan_path);
        if (count) note("Luau native code is up to Studio again in %s", shown);
    }
    free(text);
    free(edits);
    free(gone);
    unload(&j);
}

static BOOL take(const WCHAR *target, const Loan *before, const WCHAR *loan_path) {
    char shown[MAX_PATH * 3], added[200], *text = NULL;
    WCHAR folder[MAX_PATH];
    Json j;
    Edit *edits = NULL;
    Loan loan;
    DWORD error = load(target, &j);
    BOOL missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    size_t size = 0, used = 0, at;
    int last[LUAU_KEYS] = {-1, -1}, i, k, count = 0;

    utf8(target, shown, sizeof(shown));
    memset(&loan, 0, sizeof(loan));
    if (missing) {
        error = 0;
        j.blank = TRUE;
    } else if (!error && !parse(&j)) {
        note("cannot turn Luau native code off: %s is not JSON Studio7 can read", shown);
        unload(&j);
        return FALSE;
    }
    if (!error && !(edits = malloc(sizeof(Edit) * (j.count + 1)))) error = ERROR_NOT_ENOUGH_MEMORY;
    lstrcpynW(loan.file, target, MAX_PATH);
    loan.created = missing || (before && before->created);
    for (i = 0; !error && i < j.count; i++) {
        k = j.members[i].key;
        if (k < 0) continue;
        last[k] = i;
        if (!is(&j, &j.members[i], g_values[k]))
            edits[count++] = (Edit){j.members[i].value, j.members[i].end, g_values[k]};
    }
    added[0] = 0;
    for (k = 0; !error && k < LUAU_KEYS; k++) {
        const Member *m = last[k] >= 0 ? &j.members[last[k]] : NULL;

        if (!m) {
            used += (size_t)sprintf(added + used, "%s\"%s\":%s", j.count || used ? "," : "", g_names[k], g_values[k]);
        } else if (before && is(&j, m, g_values[k])) {
            loan.had[k] = before->had[k];
            loan.was[k] = before->had[k] ? copy(before->was[k], strlen(before->was[k])) : NULL;
        } else {
            loan.had[k] = TRUE;
            loan.was[k] = copy(j.data + m->value, m->end - m->value);
        }
        if (loan.had[k] && !loan.was[k]) error = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!error && used && !j.blank) {
        at = j.count ? j.members[j.count - 1].end : j.open + 1;
        edits[count++] = (Edit){at, at, added};
    }
    if (!error && (count || used)) {
        error = loan_write(loan_path, &loan);
        if (!error && j.blank && (text = malloc(used + 3))) size = (size_t)sprintf(text, "{%s}", added);
        else if (!error && !j.blank) text = rewrite(&j, edits, count, &size);
        if (!error && !text) error = ERROR_NOT_ENOUGH_MEMORY;
        if (!error && missing) {
            parent(target, folder);
            CreateDirectoryW(folder, NULL);
        }
        if (!error) error = save(target, text, size);
    }
    if (error) note("cannot turn Luau native code off in %s (error %lu)", shown, error);
    free(text);
    free(edits);
    loan_free(&loan);
    unload(&j);
    return !error;
}

static BOOL sync(const WCHAR *target, const WCHAR *loan_path) {
    Loan loan;
    BOOL loaned = loan_read(loan_path, &loan), ok = TRUE;

    if (loaned && (!target || _wcsicmp(loan.file, target))) {
        give_back(&loan, loan_path);
        loaned = FALSE;
    }
    if (target) ok = take(target, loaned ? &loan : NULL, loan_path);
    loan_free(&loan);
    return ok;
}

BOOL luau_sync(const Paths *paths, const char *version) {
    WCHAR local[MAX_PATH], user[MAX_PATH], studio[MAX_PATH], legacy[MAX_PATH], loan_path[MAX_PATH];
    const WCHAR *target = NULL;
    char shown[MAX_PATH * 3];
    DWORD osxsave = 0;
    ULONG64 xcr0 = 0;

    wide_format(loan_path, MAX_PATH, L"%ls\\" LUAU_LOAN, paths->base);
    if (version && avx_off(&osxsave, &xcr0)) {
        user[0] = 0;
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local)) &&
            wcslen(local) < MAX_PATH - 48)
            wide_format(user, MAX_PATH, L"%ls\\Roblox\\ClientSettings\\" LUAU_SETTINGS, local);
        studio_folder(paths, version, studio);
        wide_format(legacy, MAX_PATH, L"%ls\\ClientSettings\\" LUAU_SETTINGS, studio);
        target = user[0] && GetFileAttributesW(user) != INVALID_FILE_ATTRIBUTES ? user : legacy;
        note("Luau native code off for Studio in %s: the CPU has AVX, Windows has AVX state off "
             "(osxsave=%lu xcr0=0x%I64X)", utf8(target, shown, sizeof(shown)), osxsave, xcr0);
    }
    return sync(target, loan_path);
}
