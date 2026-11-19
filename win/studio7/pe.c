#include "studio7.h"

BOOL pe_open(const WCHAR *path, Pe *pe) {
    const IMAGE_DOS_HEADER *dos;
    LARGE_INTEGER size;

    memset(pe, 0, sizeof(*pe));
    lstrcpynW(pe->path, path, MAX_PATH);
    pe->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (pe->file == INVALID_HANDLE_VALUE) {
        pe->file = NULL;
        return FALSE;
    }
    if (!GetFileSizeEx(pe->file, &size) || size.QuadPart < 0x200 || size.QuadPart > 0x7FFFFFFF) goto fail;
    pe->size = (DWORD)size.QuadPart;
    pe->mapping = CreateFileMappingW(pe->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!pe->mapping) goto fail;
    pe->data = MapViewOfFile(pe->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!pe->data) goto fail;
    dos = (const IMAGE_DOS_HEADER *)pe->data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        (DWORD)dos->e_lfanew > pe->size - sizeof(IMAGE_NT_HEADERS64))
        goto fail;
    pe->nt = (const IMAGE_NT_HEADERS64 *)(pe->data + dos->e_lfanew);
    if (pe->nt->Signature != IMAGE_NT_SIGNATURE || pe->nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe->nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        goto fail;
    pe->sections = (const IMAGE_SECTION_HEADER *)((const BYTE *)&pe->nt->OptionalHeader +
                                                   pe->nt->FileHeader.SizeOfOptionalHeader);
    pe->section_count = pe->nt->FileHeader.NumberOfSections;
    if ((const BYTE *)(pe->sections + pe->section_count) > pe->data + pe->size) goto fail;
    return TRUE;
fail:
    pe_close(pe);
    return FALSE;
}

void pe_close(Pe *pe) {
    if (pe->data) UnmapViewOfFile(pe->data);
    if (pe->mapping) CloseHandle(pe->mapping);
    if (pe->file) CloseHandle(pe->file);
    pe->data = NULL;
    pe->mapping = NULL;
    pe->file = NULL;
}

static DWORD offset_of(const Pe *pe, DWORD rva, DWORD size) {
    int i;

    for (i = 0; i < pe->section_count; i++) {
        const IMAGE_SECTION_HEADER *s = &pe->sections[i];
        DWORD span = s->Misc.VirtualSize > s->SizeOfRawData ? s->SizeOfRawData : s->Misc.VirtualSize;

        if (!span) span = s->SizeOfRawData;
        if (rva >= s->VirtualAddress && rva - s->VirtualAddress < span) {
            DWORD offset = rva - s->VirtualAddress + s->PointerToRawData;

            if (span - (rva - s->VirtualAddress) < size || offset > pe->size || pe->size - offset < size) return 0;
            return offset;
        }
    }
    return 0;
}

const void *pe_at(const Pe *pe, DWORD rva, DWORD size) {
    DWORD offset = offset_of(pe, rva, size);

    return offset ? pe->data + offset : NULL;
}

const char *pe_text(const Pe *pe, DWORD rva) {
    DWORD offset = offset_of(pe, rva, 1), end;

    if (!offset) return NULL;
    for (end = offset; end < pe->size; end++)
        if (!pe->data[end]) return (const char *)pe->data + offset;
    return NULL;
}

static const IMAGE_DATA_DIRECTORY *directory(const Pe *pe, int index) {
    if ((DWORD)index >= pe->nt->OptionalHeader.NumberOfRvaAndSizes) return NULL;
    return pe->nt->OptionalHeader.DataDirectory[index].VirtualAddress ? &pe->nt->OptionalHeader.DataDirectory[index]
                                                                        : NULL;
}

int pe_imports(const Pe *pe, PeImport *out, int max) {
    const IMAGE_DATA_DIRECTORY *dir = directory(pe, IMAGE_DIRECTORY_ENTRY_IMPORT);
    int count = 0;
    DWORD rva;

    if (dir) {
        for (rva = dir->VirtualAddress;; rva += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            const IMAGE_IMPORT_DESCRIPTOR *d = pe_at(pe, rva, sizeof(*d));
            const char *name;

            if (!d || !d->Name) break;
            name = pe_text(pe, d->Name);
            if (!name || count == max) return -1;
            out[count].dll = name;
            out[count].name_offset = (DWORD)((const BYTE *)name - pe->data);
            out[count].thunks = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
            out[count].delay = FALSE;
            count++;
        }
    }
    dir = directory(pe, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (dir) {
        for (rva = dir->VirtualAddress;; rva += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
            const IMAGE_DELAYLOAD_DESCRIPTOR *d = pe_at(pe, rva, sizeof(*d));
            const char *name;

            if (!d || !d->DllNameRVA) break;
            name = pe_text(pe, d->DllNameRVA);
            if (!name || count == max) return -1;
            out[count].dll = name;
            out[count].name_offset = (DWORD)((const BYTE *)name - pe->data);
            out[count].thunks = d->ImportNameTableRVA;
            out[count].delay = TRUE;
            count++;
        }
    }
    return count;
}

const char *pe_import_function(const Pe *pe, const PeImport *import, int index, char *ordinal_text) {
    const ULONGLONG *thunk = pe_at(pe, import->thunks + (DWORD)index * 8, 8);

    if (!thunk || !*thunk) return NULL;
    if (*thunk & IMAGE_ORDINAL_FLAG64) {
        sprintf(ordinal_text, "#%u", (unsigned)(*thunk & 0xFFFF));
        return ordinal_text;
    }
    return pe_text(pe, (DWORD)(*thunk & 0x7FFFFFFF) + 2);
}

static const IMAGE_EXPORT_DIRECTORY *exports_of(const Pe *pe, const IMAGE_DATA_DIRECTORY **dir) {
    *dir = directory(pe, IMAGE_DIRECTORY_ENTRY_EXPORT);
    return *dir ? pe_at(pe, (*dir)->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)) : NULL;
}

int pe_export_count(const Pe *pe) {
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_EXPORT_DIRECTORY *e = exports_of(pe, &dir);

    return e ? (int)e->NumberOfNames : 0;
}

BOOL pe_export_at(const Pe *pe, int index, PeExport *out) {
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_EXPORT_DIRECTORY *e = exports_of(pe, &dir);
    const DWORD *name, *function;
    const WORD *ordinal;

    if (!e || (DWORD)index >= e->NumberOfNames) return FALSE;
    name = pe_at(pe, e->AddressOfNames + (DWORD)index * 4, 4);
    ordinal = pe_at(pe, e->AddressOfNameOrdinals + (DWORD)index * 2, 2);
    if (!name || !ordinal || *ordinal >= e->NumberOfFunctions) return FALSE;
    function = pe_at(pe, e->AddressOfFunctions + (DWORD)*ordinal * 4, 4);
    out->name = pe_text(pe, *name);
    if (!function || !out->name) return FALSE;
    out->ordinal = (WORD)(e->Base + *ordinal);
    out->forwarder = NULL;
    if (*function >= dir->VirtualAddress && *function - dir->VirtualAddress < dir->Size)
        out->forwarder = pe_text(pe, *function);
    return TRUE;
}

BOOL pe_export_find(const Pe *pe, const char *name, PeExport *out) {
    int low = 0, high = pe_export_count(pe) - 1;

    while (low <= high) {
        int middle = (low + high) / 2, order;

        if (!pe_export_at(pe, middle, out)) return FALSE;
        order = strcmp(name, out->name);
        if (!order) return TRUE;
        if (order < 0) high = middle - 1; else low = middle + 1;
    }
    return FALSE;
}

BOOL pe_export_has_ordinal(const Pe *pe, DWORD ordinal) {
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_EXPORT_DIRECTORY *e = exports_of(pe, &dir);
    const DWORD *function;

    if (!e || ordinal < e->Base || ordinal - e->Base >= e->NumberOfFunctions) return FALSE;
    function = pe_at(pe, e->AddressOfFunctions + (ordinal - e->Base) * 4, 4);
    return function && *function;
}
