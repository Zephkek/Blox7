#include "studio7.h"

#define MAX_UNITS 1024
#define MAX_SHIMS 8
#define MAX_STUBS 128
#define MAX_PATCHES 256
#define MAX_DESCRIPTORS 512

typedef struct {
    char name[96];
    WCHAR path[MAX_PATH];
    Pe pe;
    int state, depth;
    BOOL local;
} Unit;

typedef struct {
    char name[32], base[32], system[32];
    Pe pe;
} Shim;

typedef struct {
    char name[96];
    char **functions;
    int count, capacity;
} Stub;

typedef struct {
    WCHAR path[MAX_PATH];
    DWORD offset;
    char to[40], from[40];
} Patch;

typedef struct {
    char name[96];
    int hashed, first, count;
} SchemaSet;

typedef struct {
    char importer[64], host[64];
} SchemaValue;

typedef struct {
    const LayerJob *job;
    Unit *units;
    int unit_count;
    Shim shims[MAX_SHIMS];
    int shim_count;
    Stub stubs[MAX_STUBS];
    int stub_count;
    Patch patches[MAX_PATCHES];
    int patch_count;
    int missing, failures;
    SchemaSet *sets;
    SchemaValue *values;
    int set_count, value_count;
    DWORD schema;
    BOOL seven;
} World;

static const char *g_homes[] = {"kernel32.dll", "advapi32.dll", "ole32.dll", "user32.dll", "gdi32.dll", "shlwapi.dll",
                                "shell32.dll", "oleaut32.dll", "version.dll", "ws2_32.dll", "ntdll.dll"};
static const char *g_side_by_side[] = {"comctl32.dll", "gdiplus.dll"};

void say(const LayerJob *job, const char *format, ...) {
    char text[1024];
    va_list args;

    va_start(args, format);
    _vsnprintf(text, sizeof(text) - 1, format, args);
    text[sizeof(text) - 1] = 0;
    va_end(args);
    if (job->log) job->log(text);
}

static void lower(char *to, const char *from, size_t room) {
    size_t i;

    for (i = 0; i + 1 < room && from[i]; i++) to[i] = (char)(from[i] >= 'A' && from[i] <= 'Z' ? from[i] + 32 : from[i]);
    to[i] = 0;
}

static void narrow(char *to, const WCHAR *from, size_t room) {
    size_t i;

    for (i = 0; i + 1 < room && from[i]; i++) to[i] = from[i] < 128 ? (char)from[i] : '?';
    to[i] = 0;
}

static BOOL is_set(const char *name) {
    return !strncmp(name, "api-ms-win-", 11) || !strncmp(name, "ext-ms-", 7);
}

static BOOL is_crt_set(const char *name) {
    return !strncmp(name, "api-ms-win-crt-", 15);
}

static const char *schema_host(const World *w, const char *name, const char *importer) {
    char key[96];
    const char *dash;
    size_t length = strlen(name), prefix;
    int i, j;

    if (strncmp(name, "api-", 4) && (w->schema < 4 || strncmp(name, "ext-", 4))) return NULL;
    if (length > 4 && !strcmp(name + length - 4, ".dll")) length -= 4;
    if (length >= sizeof(key)) return NULL;
    memcpy(key, name, length);
    key[length] = 0;
    dash = strrchr(key, '-');
    prefix = dash ? (size_t)(dash - key) : length;
    for (i = 0; i < w->set_count; i++) {
        const SchemaSet *s = &w->sets[i];

        if (w->schema >= 6 ? (size_t)s->hashed != prefix || strncmp(s->name, key, prefix) : strcmp(s->name, key + 4))
            continue;
        for (j = 0; importer && j < s->count; j++)
            if (!strcmp(w->values[s->first + j].importer, importer)) return w->values[s->first + j].host;
        for (j = 0; j < s->count; j++)
            if (!w->values[s->first + j].importer[0]) return w->values[s->first + j].host;
        return "";
    }
    return NULL;
}

static BOOL exists(const WCHAR *path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void join(WCHAR *out, const WCHAR *dir, const char *name) {
    WCHAR wide[128];
    int i;

    for (i = 0; i < 127 && name[i]; i++) wide[i] = (WCHAR)(unsigned char)name[i];
    wide[i] = 0;
    wide_format(out, MAX_PATH, L"%ls\\%ls", dir, wide);
}

static Unit *add_unit(World *w, const char *name, const WCHAR *path, BOOL local, int depth) {
    Unit *u;

    if (w->unit_count == MAX_UNITS) return NULL;
    u = &w->units[w->unit_count++];
    memset(u, 0, sizeof(*u));
    lower(u->name, name, sizeof(u->name));
    lstrcpynW(u->path, path, MAX_PATH);
    u->local = local;
    u->depth = depth;
    return u;
}

static void scan(World *w, const WCHAR *dir, int depth) {
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;

    wide_format(pattern, MAX_PATH, L"%ls\\*", dir);
    find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        size_t length = wcslen(found.cFileName);

        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        wide_format(path, MAX_PATH, L"%ls\\%ls", dir, found.cFileName);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 6) scan(w, path, depth + 1);
        } else if (length > 4 && (!_wcsicmp(found.cFileName + length - 4, L".dll") ||
                                  !_wcsicmp(found.cFileName + length - 4, L".exe"))) {
            char name[96];

            narrow(name, found.cFileName, sizeof(name));
            add_unit(w, name, path, TRUE, depth);
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

static BOOL open_unit(Unit *u) {
    if (!u->state) u->state = pe_open(u->path, &u->pe) ? 1 : 2;
    return u->state == 1;
}

static Unit *local_unit(World *w, const char *name) {
    Unit *best = NULL;
    int i;

    for (i = 0; i < w->unit_count; i++)
        if (w->units[i].local && !strcmp(w->units[i].name, name) && (!best || w->units[i].depth < best->depth))
            best = &w->units[i];
    return best;
}

static Unit *system_unit(World *w, const char *name) {
    WCHAR path[MAX_PATH];
    int i;

    for (i = 0; i < w->unit_count; i++)
        if (!w->units[i].local && !strcmp(w->units[i].name, name)) return &w->units[i];
    join(path, w->job->system, name);
    if (!exists(path)) return NULL;
    return add_unit(w, name, path, FALSE, 0);
}

static Unit *find_unit(World *w, const char *name, const char *importer) {
    const char *host = is_set(name) ? schema_host(w, name, importer) : NULL;
    Unit *u;

    if (host) return *host ? system_unit(w, host) : NULL;
    u = local_unit(w, name);
    return u ? u : system_unit(w, name);
}

static BOOL resolves(World *w, const char *dll, const char *function, const char *importer, int depth) {
    Unit *u = find_unit(w, dll, importer);
    PeExport e;
    char module[96], *dot;

    if (!u || depth > 8 || !open_unit(u)) return FALSE;
    if (function[0] == '#') return pe_export_has_ordinal(&u->pe, (DWORD)atoi(function + 1));
    if (!pe_export_find(&u->pe, function, &e)) return FALSE;
    if (!e.forwarder) return TRUE;
    lower(module, e.forwarder, sizeof(module) - 4);
    dot = strrchr(module, '.');
    if (!dot) return FALSE;
    strcpy(dot, ".dll");
    return resolves(w, module, strrchr(e.forwarder, '.') + 1, u->name, depth + 1);
}

static BOOL is_ours(Unit *u) {
    PeImport imports[4];
    char ordinal[16];

    if (!strncmp(u->name, "s7", 2) && strstr(u->name, ".dll")) return TRUE;
    if (!open_unit(u) || pe_imports(&u->pe, imports, 4) != 1) return FALSE;
    return !_stricmp(imports[0].dll, S7_KERNEL) && pe_import_function(&u->pe, &imports[0], 0, ordinal) &&
           !pe_import_function(&u->pe, &imports[0], 1, ordinal);
}

static BOOL load_shims(World *w) {
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;

    wide_format(pattern, MAX_PATH, L"%ls\\s7*.dll", w->job->layer);
    find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    do {
        Shim *s = &w->shims[w->shim_count];
        PeExport e;
        char *dot;
        int i;

        if (w->shim_count == MAX_SHIMS) break;
        memset(s, 0, sizeof(*s));
        wide_format(path, MAX_PATH, L"%ls\\%ls", w->job->layer, found.cFileName);
        if (!pe_open(path, &s->pe)) continue;
        narrow(s->name, found.cFileName, sizeof(s->name));
        lower(s->name, s->name, sizeof(s->name));
        strcpy(s->base, s->name);
        dot = strrchr(s->base, '.');
        if (dot) *dot = 0;
        for (i = 0; i < pe_export_count(&s->pe); i++) {
            if (!pe_export_at(&s->pe, i, &e) || !e.forwarder) continue;
            lower(s->system, e.forwarder, sizeof(s->system) - 4);
            dot = strrchr(s->system, '.');
            if (dot) strcpy(dot, ".dll");
            break;
        }
        w->shim_count++;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return w->shim_count > 0;
}

static Shim *shim_owning(World *w, const char *function, const char *system) {
    PeExport e;
    int i;

    if (function[0] == '#' || !strcmp(function, S7_ANCHOR)) return NULL;
    for (i = 0; i < w->shim_count; i++) {
        if (system && strcmp(w->shims[i].system, system)) continue;
        if (pe_export_find(&w->shims[i].pe, function, &e) && !e.forwarder) return &w->shims[i];
    }
    return NULL;
}

static Shim *shim_named(World *w, const char *name) {
    int i;

    for (i = 0; i < w->shim_count; i++)
        if (!strcmp(w->shims[i].name, name)) return &w->shims[i];
    return NULL;
}

static Shim *shim_for(World *w, const char *system) {
    int i;

    for (i = 0; i < w->shim_count; i++)
        if (!strcmp(w->shims[i].system, system)) return &w->shims[i];
    return NULL;
}

static void want(World *w, const char *dll, const char *function) {
    Stub *s = NULL;
    int i;

    for (i = 0; i < w->stub_count; i++)
        if (!strcmp(w->stubs[i].name, dll)) s = &w->stubs[i];
    if (!s) {
        if (w->stub_count == MAX_STUBS) return;
        s = &w->stubs[w->stub_count++];
        memset(s, 0, sizeof(*s));
        strcpy(s->name, dll);
    }
    if (!function || function[0] == '#') return;
    for (i = 0; i < s->count; i++)
        if (!strcmp(s->functions[i], function)) return;
    if (s->count == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 32;
        s->functions = realloc(s->functions, sizeof(char *) * (size_t)s->capacity);
    }
    s->functions[s->count++] = _strdup(function);
}

static BOOL side_by_side(const char *dll) {
    int i;

    for (i = 0; i < (int)(sizeof(g_side_by_side) / sizeof(g_side_by_side[0])); i++)
        if (!strcmp(dll, g_side_by_side[i])) return TRUE;
    return FALSE;
}

static BOOL always_ours(const World *w, const char *function, const char *module) {
    if (!strcmp(function, "VerifyVersionInfoW")) return !strcmp(module, "robloxstudiobeta.exe");
    if (!strcmp(function, "SendMessageW") || !strcmp(function, "RegisterClassExW")) return !strcmp(module, "qwindows.dll");
    if (!w->seven) return FALSE;
    if (!strcmp(function, "LoadLibraryExW")) return !strcmp(module, "webview2loader.dll");
    return !strcmp(function, "GetAddrInfoExW") || !strcmp(function, "WakeConditionVariable") ||
           !strcmp(function, "WakeAllConditionVariable");
}

static BOOL journal_find(const LayerJob *job, const WCHAR *file, DWORD offset, char *original, size_t room);

static BOOL needs_stub(World *w, const char *dll) {
    if (is_crt_set(dll) || shim_named(w, dll) || side_by_side(dll)) return FALSE;
    if (is_set(dll)) return !schema_host(w, dll, NULL);
    return !local_unit(w, dll) && !system_unit(w, dll);
}

static void plan_module(World *w, Unit *u) {
    PeImport imports[MAX_DESCRIPTORS];
    int count = pe_imports(&u->pe, imports, MAX_DESCRIPTORS), i, j;
    char ordinal[16];

    for (i = 0; i < count; i++) {
        char dll[96], wanted[96], original[40];
        Shim *bound, *shim;
        const char *function;
        BOOL use = FALSE;
        size_t length = strlen(imports[i].dll);

        lower(dll, imports[i].dll, sizeof(dll));
        bound = shim_named(w, dll);
        if (bound && bound->system[0]) strcpy(dll, bound->system);
        shim = shim_for(w, dll);
        for (j = 0; (function = pe_import_function(&u->pe, &imports[i], j, ordinal)) != NULL; j++) {
            if (shim && shim_owning(w, function, dll) &&
                (always_ours(w, function, u->name) || !resolves(w, dll, function, u->name, 0)))
                use = TRUE;
            if (needs_stub(w, dll)) want(w, dll, function);
        }
        if (!shim && !bound) continue;
        strcpy(wanted, use ? shim->name : dll);
        if (!_stricmp(wanted, imports[i].dll)) continue;
        if (!use && journal_find(w->job, u->path, imports[i].name_offset, original, sizeof(original))) strcpy(wanted, original);
        if (w->patch_count == MAX_PATCHES) {
            w->failures++;
            continue;
        }
        lstrcpynW(w->patches[w->patch_count].path, u->path, MAX_PATH);
        w->patches[w->patch_count].offset = imports[i].name_offset;
        strcpy(w->patches[w->patch_count].to, wanted);
        w->patches[w->patch_count].from[0] = 0;
        if (!bound && length < sizeof(w->patches[0].from)) strcpy(w->patches[w->patch_count].from, imports[i].dll);
        w->patch_count++;
    }
}

static BOOL apply_patch(const LayerJob *job, const Patch *p) {
    HANDLE file = CreateFileW(p->path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    char now[48], shown[MAX_PATH * 3];
    DWORD done = 0, length = (DWORD)strlen(p->to), room, i;
    BOOL ok = FALSE;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    memset(now, 0, sizeof(now));
    if (SetFilePointer(file, (LONG)p->offset, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(file, now, sizeof(now) - 1, &done, NULL)) {
        room = (DWORD)strlen(now);
        ok = length < sizeof(now) - 1;
        for (i = room; ok && i <= length; i++)
            if (now[i]) ok = FALSE;
        if (ok) {
            memset(now, 0, sizeof(now));
            memcpy(now, p->to, length);
            ok = SetFilePointer(file, (LONG)p->offset, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
                 WriteFile(file, now, (length > room ? length : room) + 1, &done, NULL);
        }
    }
    CloseHandle(file);
    if (!ok) say(job, "could not rename an import to %s in %s", p->to, utf8(p->path, shown, sizeof(shown)));
    return ok;
}

static void journal_path(const LayerJob *job, WCHAR *out) {
    wide_format(out, MAX_PATH, L"%ls\\%ls", job->studio, S7_JOURNAL);
}

static void journal_add(const LayerJob *job, const Patch *p) {
    WCHAR path[MAX_PATH];
    char file[MAX_PATH * 3];
    FILE *f;

    if (!p->from[0]) return;
    journal_path(job, path);
    f = _wfopen(path, L"ab");
    if (!f) return;
    fprintf(f, "%s|%lu|%s\n", utf8(p->path + wcslen(job->studio) + 1, file, sizeof(file)), p->offset, p->from);
    fclose(f);
}

static BOOL journal_find(const LayerJob *job, const WCHAR *file, DWORD offset, char *original, size_t room) {
    WCHAR path[MAX_PATH];
    char wanted[MAX_PATH * 3], line[MAX_PATH * 3 + 80];
    BOOL found = FALSE;
    FILE *f;

    journal_path(job, path);
    f = _wfopen(path, L"rb");
    if (!f) return FALSE;
    utf8(file + wcslen(job->studio) + 1, wanted, sizeof(wanted));
    while (!found && fgets(line, sizeof(line), f)) {
        char *bar = strchr(line, '|'), *second = bar ? strchr(bar + 1, '|') : NULL;
        size_t i;

        if (!second) continue;
        *bar = 0;
        *second = 0;
        if (_stricmp(line, wanted) || strtoul(bar + 1, NULL, 10) != offset) continue;
        for (i = 0; i + 1 < room && (unsigned char)second[1 + i] >= 32; i++) original[i] = second[1 + i];
        original[i] = 0;
        found = i > 0;
    }
    fclose(f);
    return found;
}

static void close_units(World *w) {
    int i;

    for (i = 0; i < w->unit_count; i++)
        if (w->units[i].state == 1) pe_close(&w->units[i].pe);
    w->unit_count = 0;
}

static DWORD read32(const BYTE *base, DWORD size, DWORD at) {
    return at <= size && size - at >= 4 ? *(const DWORD *)(base + at) : 0;
}

static BOOL schema_text(const BYTE *base, DWORD size, DWORD at, DWORD length, char *out, size_t room) {
    DWORD i;

    if ((length & 1) || at > size || length > size - at || length / 2 >= room) return FALSE;
    for (i = 0; i < length / 2; i++) {
        WCHAR c = (WCHAR)(base[at + 2 * i] | base[at + 2 * i + 1] << 8);

        if (c >= 128) return FALSE;
        out[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    out[i] = 0;
    return TRUE;
}

static BOOL load_schema(World *w) {
    WCHAR path[MAX_PATH];
    const BYTE *base = NULL;
    DWORD size = 0, count, i, j;
    int capacity = 0, k;
    Pe pe;

    join(path, w->job->system, "apisetschema.dll");
    if (!pe_open(path, &pe)) return FALSE;
    for (k = 0; k < pe.section_count; k++) {
        const IMAGE_SECTION_HEADER *s = &pe.sections[k];

        if (memcmp(s->Name, ".apiset", 8) || s->PointerToRawData > pe.size) continue;
        base = pe.data + s->PointerToRawData;
        size = s->SizeOfRawData < pe.size - s->PointerToRawData ? s->SizeOfRawData : pe.size - s->PointerToRawData;
    }
    w->schema = base ? read32(base, size, 0) : 0;
    count = w->schema == 2 ? read32(base, size, 4) : w->schema == 4 || w->schema == 6 ? read32(base, size, 12) : 0;
    w->sets = count && count < 65536 ? calloc(count, sizeof(SchemaSet)) : NULL;
    for (i = 0; w->sets && i < count; i++) {
        SchemaSet *s = &w->sets[w->set_count];
        DWORD at, values, n, step;

        if (w->schema == 2) {
            at = 8 + 12 * i;
            values = read32(base, size, at + 8) + 4;
            n = read32(base, size, values - 4);
            step = 16;
        } else if (w->schema == 4) {
            at = 16 + 24 * i;
            values = read32(base, size, at + 20) + 8;
            n = read32(base, size, values - 4);
            step = 20;
        } else {
            at = read32(base, size, 16) + 24 * i;
            s->hashed = (int)(read32(base, size, at + 12) / 2);
            values = read32(base, size, at + 16);
            n = read32(base, size, at + 20);
            step = 20;
        }
        if (!schema_text(base, size, read32(base, size, at + (w->schema == 2 ? 0 : 4)),
                         read32(base, size, at + (w->schema == 2 ? 4 : 8)), s->name, sizeof(s->name)))
            continue;
        s->first = w->value_count;
        for (j = 0; j < n && j < 64; j++) {
            DWORD v = values + step * j + step - 16;
            SchemaValue *value;

            if (w->value_count == capacity) {
                SchemaValue *more = realloc(w->values, sizeof(SchemaValue) * (size_t)(capacity + 512));

                if (!more) break;
                w->values = more;
                capacity += 512;
            }
            value = &w->values[w->value_count];
            if (!schema_text(base, size, read32(base, size, v), read32(base, size, v + 4), value->importer,
                             sizeof(value->importer)) ||
                !schema_text(base, size, read32(base, size, v + 8), read32(base, size, v + 12), value->host,
                             sizeof(value->host)))
                continue;
            w->value_count++;
            s->count++;
        }
        w->set_count++;
    }
    pe_close(&pe);
    return w->set_count > 0;
}

static BOOL seven_loader(const LayerJob *job) {
    WCHAR path[MAX_PATH];
    VS_FIXEDFILEINFO *info;
    DWORD size, ignored;
    UINT length;
    void *data;
    BOOL seven = TRUE;

    wide_format(path, MAX_PATH, L"%ls\\ntdll.dll", job->system);
    size = GetFileVersionInfoSizeW(path, &ignored);
    data = size ? malloc(size) : NULL;
    if (data && GetFileVersionInfoW(path, 0, size, data) && VerQueryValueW(data, L"\\", (void **)&info, &length) &&
        length >= sizeof(*info))
        seven = info->dwFileVersionMS <= MAKELONG(1, 6);
    free(data);
    return seven;
}

static World *world_new(const LayerJob *job, BOOL with_shims) {
    World *w = calloc(1, sizeof(World));

    if (!w) return NULL;
    w->job = job;
    w->units = calloc(MAX_UNITS, sizeof(Unit));
    if (!w->units || (with_shims && !load_shims(w)) || !load_schema(w)) {
        if (!w->units) say(job, "out of memory");
        else if (with_shims && !w->shim_count) say(job, "the layer DLLs are missing");
        else say(job, "this Windows' API set schema could not be read");
        free(w->sets);
        free(w->values);
        free(w->units);
        free(w);
        return NULL;
    }
    w->seven = seven_loader(job);
    scan(w, job->studio, 0);
    return w;
}

static void world_free(World *w) {
    int i, j;

    close_units(w);
    for (i = 0; i < w->shim_count; i++) pe_close(&w->shims[i].pe);
    for (i = 0; i < w->stub_count; i++) {
        for (j = 0; j < w->stubs[i].count; j++) free(w->stubs[i].functions[j]);
        free(w->stubs[i].functions);
    }
    free(w->sets);
    free(w->values);
    free(w->units);
    free(w);
}

static BOOL home_of(World *w, const char *function, char *target, size_t room) {
    Shim *shim = shim_owning(w, function, NULL);
    int i;

    if (shim) {
        text_format(target, room, "%s.%s", shim->base, function);
        return TRUE;
    }
    for (i = 0; i < (int)(sizeof(g_homes) / sizeof(g_homes[0])); i++) {
        if (resolves(w, g_homes[i], function, NULL, 0)) {
            text_format(target, room, "%.*s.%s", (int)strlen(g_homes[i]) - 4, g_homes[i], function);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL make_stub(World *w, Stub *s) {
    WCHAR path[MAX_PATH], keep[MAX_PATH];
    StubExport *exports;
    Unit *existing = local_unit(w, s->name);
    Pe original;
    BOOL have_original = FALSE, ok;
    char base[96], *dot;
    int total, count = 0, i, wanted = s->count;

    join(path, w->job->studio, s->name);
    wide_format(keep, MAX_PATH, L"%ls%ls", path, S7_KEEP);
    if (existing && existing->depth == 0 && !is_ours(existing) && !exists(keep)) {
        pe_close(&existing->pe);
        existing->state = 0;
        if (!MoveFileW(path, keep)) {
            say(w->job, "could not set %s aside", s->name);
            return FALSE;
        }
    } else if (existing && existing->state == 1) {
        pe_close(&existing->pe);
        existing->state = 0;
    }
    if (exists(keep) && pe_open(keep, &original)) {
        have_original = TRUE;
        for (i = 0; i < pe_export_count(&original); i++) {
            PeExport e;

            if (pe_export_at(&original, i, &e)) want(w, s->name, e.name);
        }
    }
    total = s->count;
    exports = calloc((size_t)(total ? total : 1), sizeof(StubExport));
    for (i = 0; exports && i < total; i++) {
        char target[160];

        if (home_of(w, s->functions[i], target, sizeof(target))) {
            exports[count].name = s->functions[i];
            exports[count].target = _strdup(target);
            count++;
        } else if (i < wanted) {
            say(w->job, "nothing on this Windows provides %s!%s", s->name, s->functions[i]);
            w->missing++;
        }
    }
    if (have_original) pe_close(&original);
    strcpy(base, s->name);
    dot = strrchr(base, '.');
    if (dot && !_stricmp(dot, ".dll")) strcpy(dot, ".dll");
    ok = exports && stub_write(path, base, exports, count);
    for (i = 0; exports && i < count; i++) free((void *)exports[i].target);
    free(exports);
    if (!ok) say(w->job, "could not write %s", s->name);
    return ok;
}

static int verify(const LayerJob *job) {
    World *w = world_new(job, FALSE);
    Unit *runtime, *kernel;
    int problems = 0, modules = 0, i, j, k, total;
    BOOL anchored = FALSE;

    if (!w) return -1;
    total = w->unit_count;
    for (i = 0; i < total; i++) {
        Unit *u = &w->units[i];
        PeImport imports[MAX_DESCRIPTORS];
        char ordinal[16], dll[96];
        const char *function;
        int count;

        if (!open_unit(u)) continue;
        modules++;
        count = pe_imports(&u->pe, imports, MAX_DESCRIPTORS);
        for (j = 0; j < count; j++) {
            lower(dll, imports[j].dll, sizeof(dll));
            if (side_by_side(dll)) continue;
            for (k = 0; (function = pe_import_function(&u->pe, &imports[j], k, ordinal)) != NULL; k++) {
                if (resolves(w, dll, function, u->name, 0)) continue;
                say(job, "unresolved: %s needs %s!%s%s", u->name, dll, function, imports[j].delay ? " (delay load)" : "");
                problems++;
            }
        }
    }
    runtime = local_unit(w, "ucrtbase.dll");
    kernel = local_unit(w, S7_KERNEL);
    if (w->seven && runtime && open_unit(runtime)) {
        PeImport imports[MAX_DESCRIPTORS];
        int count = pe_imports(&runtime->pe, imports, MAX_DESCRIPTORS);
        char dll[96];

        for (j = 0; j < count; j++) {
            Unit *u;

            lower(dll, imports[j].dll, sizeof(dll));
            u = is_set(dll) && !schema_host(w, dll, NULL) ? local_unit(w, dll) : NULL;
            if (u && is_ours(u)) anchored = TRUE;
        }
        if (!anchored || !kernel || !open_unit(kernel) || !resolves(w, "bcrypt.dll", "BCryptGenRandom", NULL, 0)) {
            say(job, "ucrtbase.dll would start before bcrypt.dll");
            problems++;
        } else {
            PeImport imports2[16];
            BOOL has_bcrypt = FALSE;

            count = pe_imports(&kernel->pe, imports2, 16);
            for (j = 0; j < count; j++)
                if (!_stricmp(imports2[j].dll, "bcrypt.dll")) has_bcrypt = TRUE;
            if (!has_bcrypt) {
                say(job, "s7kernel.dll does not import bcrypt.dll");
                problems++;
            }
        }
    }
    say(job, "%d files checked, %d unresolved", modules, problems);
    world_free(w);
    return problems;
}

int layer_check(const LayerJob *job) {
    return verify(job);
}

int layer_bind(const LayerJob *job) {
    World *w = world_new(job, TRUE);
    int missing, failures, unresolved, i, total, bound = 0;

    if (!w) return -1;
    total = w->unit_count;
    for (i = 0; i < total; i++) {
        Unit *u = &w->units[i];

        if (!open_unit(u) || is_ours(u)) continue;
        plan_module(w, u);
    }
    for (i = 0; i < w->shim_count; i++) {
        WCHAR to[MAX_PATH];
        Unit *old = local_unit(w, w->shims[i].name);

        if (old && old->state == 1) {
            pe_close(&old->pe);
            old->state = 0;
        }
        join(to, job->studio, w->shims[i].name);
        if (!CopyFileW(w->shims[i].pe.path, to, FALSE)) {
            say(job, "could not install %s (error %lu)", w->shims[i].name, GetLastError());
            w->failures++;
        } else if (!old) {
            add_unit(w, w->shims[i].name, to, TRUE, 0);
        }
    }
    for (i = 0; i < w->stub_count; i++)
        if (!make_stub(w, &w->stubs[i])) w->failures++;
    say(job, "%d layer DLLs, %d API-set stubs", w->shim_count, w->stub_count);
    close_units(w);
    for (i = 0; i < w->patch_count; i++) {
        journal_add(job, &w->patches[i]);
        if (apply_patch(job, &w->patches[i])) bound++;
        else w->failures++;
    }
    say(job, "%d import tables updated", bound);
    missing = w->missing;
    failures = w->failures;
    world_free(w);
    unresolved = verify(job);
    return failures || unresolved < 0 ? -1 : missing + unresolved;
}

int layer_unbind(const LayerJob *job) {
    World *w = world_new(job, TRUE);
    int i, total, problems = 0, restored = 0, removed = 0;

    if (!w) return 1;
    total = w->unit_count;
    for (i = 0; i < total; i++) {
        Unit *u = &w->units[i];
        PeImport imports[MAX_DESCRIPTORS];
        int count, j;

        if (!open_unit(u) || is_ours(u)) continue;
        count = pe_imports(&u->pe, imports, MAX_DESCRIPTORS);
        for (j = 0; j < count; j++) {
            char dll[96];
            Shim *bound;

            lower(dll, imports[j].dll, sizeof(dll));
            bound = shim_named(w, dll);
            if (!bound || !bound->system[0] || w->patch_count == MAX_PATCHES) continue;
            lstrcpynW(w->patches[w->patch_count].path, u->path, MAX_PATH);
            w->patches[w->patch_count].offset = imports[j].name_offset;
            if (!journal_find(job, u->path, imports[j].name_offset, w->patches[w->patch_count].to,
                              sizeof(w->patches[0].to)))
                strcpy(w->patches[w->patch_count].to, bound->system);
            w->patch_count++;
        }
    }
    for (i = 0; i < total; i++) {
        Unit *u = &w->units[i];
        WCHAR keep[MAX_PATH];
        BOOL ours = u->depth == 0 && open_unit(u) && is_ours(u);

        if (!ours) continue;
        pe_close(&u->pe);
        u->state = 0;
        wide_format(keep, MAX_PATH, L"%ls%ls", u->path, S7_KEEP);
        if (!DeleteFileW(u->path)) {
            problems++;
            continue;
        }
        removed++;
        if (exists(keep)) {
            if (MoveFileW(keep, u->path)) restored++; else problems++;
        }
    }
    close_units(w);
    for (i = 0; i < w->patch_count; i++)
        if (!apply_patch(job, &w->patches[i])) problems++;
    if (!problems) {
        WCHAR path[MAX_PATH];

        journal_path(job, path);
        DeleteFileW(path);
    }
    say(job, "%d import tables restored, %d layer files removed, %d originals put back", w->patch_count, removed, restored);
    world_free(w);
    return problems;
}
