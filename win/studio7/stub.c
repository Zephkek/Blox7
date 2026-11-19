#include "studio7.h"

#define FILE_ALIGN 0x200u
#define SECTION_ALIGN 0x1000u
#define HEADERS 0x400u
#define RDATA 0x1000u

static DWORD up(DWORD value, DWORD align) {
    return (value + align - 1) & ~(align - 1);
}

static int by_name(const void *a, const void *b) {
    return strcmp(((const StubExport *)a)->name, ((const StubExport *)b)->name);
}

BOOL stub_write(const WCHAR *path, const char *dll_name, const StubExport *exports, int count) {
    StubExport *sorted;
    DWORD size, at, name_at, functions, names, ordinals, export_end, import_at, lookup_at, iat_at, hint_at,
          kernel_at, rdata_size, reloc_rva, file_size, written = 0;
    BYTE *image, *r;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *section;
    IMAGE_EXPORT_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *import;
    HANDLE file;
    BOOL ok;
    int i;

    sorted = malloc(sizeof(StubExport) * (size_t)(count ? count : 1));
    if (!sorted) return FALSE;
    memcpy(sorted, exports, sizeof(StubExport) * (size_t)count);
    qsort(sorted, (size_t)count, sizeof(StubExport), by_name);

    size = sizeof(IMAGE_EXPORT_DIRECTORY);
    name_at = size;
    size += (DWORD)strlen(dll_name) + 1;
    size = up(size, 4);
    functions = size;
    size += 4 * (DWORD)count;
    names = size;
    size += 4 * (DWORD)count;
    ordinals = size;
    size += 2 * (DWORD)count;
    for (i = 0; i < count; i++) size += (DWORD)strlen(sorted[i].name) + 1 + (DWORD)strlen(sorted[i].target) + 1;
    export_end = size;
    size = up(size, 8);
    import_at = size;
    size += 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    lookup_at = size;
    size += 16;
    iat_at = size;
    size += 16;
    hint_at = size;
    size += 2 + sizeof(S7_ANCHOR);
    size = up(size, 2);
    kernel_at = size;
    size += sizeof(S7_KERNEL);
    rdata_size = size;
    reloc_rva = RDATA + up(rdata_size, SECTION_ALIGN);
    file_size = HEADERS + up(rdata_size, FILE_ALIGN) + FILE_ALIGN;

    image = calloc(1, file_size);
    if (!image) {
        free(sorted);
        return FALSE;
    }
    dos = (IMAGE_DOS_HEADER *)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x40;
    nt = (IMAGE_NT_HEADERS64 *)(image + 0x40);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.MajorLinkerVersion = 14;
    nt->OptionalHeader.SizeOfInitializedData = up(rdata_size, FILE_ALIGN) + FILE_ALIGN;
    nt->OptionalHeader.ImageBase = 0x180000000ull;
    nt->OptionalHeader.SectionAlignment = SECTION_ALIGN;
    nt->OptionalHeader.FileAlignment = FILE_ALIGN;
    nt->OptionalHeader.MajorOperatingSystemVersion = 6;
    nt->OptionalHeader.MinorOperatingSystemVersion = 1;
    nt->OptionalHeader.MajorSubsystemVersion = 6;
    nt->OptionalHeader.MinorSubsystemVersion = 1;
    nt->OptionalHeader.SizeOfImage = reloc_rva + SECTION_ALIGN;
    nt->OptionalHeader.SizeOfHeaders = HEADERS;
    nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
    nt->OptionalHeader.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_NX_COMPAT |
                                            IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA;
    nt->OptionalHeader.SizeOfStackReserve = 0x100000;
    nt->OptionalHeader.SizeOfStackCommit = 0x1000;
    nt->OptionalHeader.SizeOfHeapReserve = 0x100000;
    nt->OptionalHeader.SizeOfHeapCommit = 0x1000;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = RDATA;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = export_end;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = RDATA + import_at;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress = RDATA + iat_at;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size = 16;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = reloc_rva;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 12;
    section = (IMAGE_SECTION_HEADER *)(nt + 1);
    memcpy(section[0].Name, ".rdata", 6);
    section[0].Misc.VirtualSize = rdata_size;
    section[0].VirtualAddress = RDATA;
    section[0].SizeOfRawData = up(rdata_size, FILE_ALIGN);
    section[0].PointerToRawData = HEADERS;
    section[0].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
    memcpy(section[1].Name, ".reloc", 6);
    section[1].Misc.VirtualSize = 12;
    section[1].VirtualAddress = reloc_rva;
    section[1].SizeOfRawData = FILE_ALIGN;
    section[1].PointerToRawData = HEADERS + up(rdata_size, FILE_ALIGN);
    section[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_DISCARDABLE;

    r = image + HEADERS;
    dir = (IMAGE_EXPORT_DIRECTORY *)r;
    dir->Name = RDATA + name_at;
    dir->Base = 1;
    dir->NumberOfFunctions = (DWORD)count;
    dir->NumberOfNames = (DWORD)count;
    dir->AddressOfFunctions = RDATA + functions;
    dir->AddressOfNames = RDATA + names;
    dir->AddressOfNameOrdinals = RDATA + ordinals;
    strcpy((char *)r + name_at, dll_name);
    at = ordinals + 2 * (DWORD)count;
    for (i = 0; i < count; i++) {
        *(DWORD *)(r + names + 4 * i) = RDATA + at;
        strcpy((char *)r + at, sorted[i].name);
        at += (DWORD)strlen(sorted[i].name) + 1;
        *(DWORD *)(r + functions + 4 * i) = RDATA + at;
        strcpy((char *)r + at, sorted[i].target);
        at += (DWORD)strlen(sorted[i].target) + 1;
        *(WORD *)(r + ordinals + 2 * i) = (WORD)i;
    }
    import = (IMAGE_IMPORT_DESCRIPTOR *)(r + import_at);
    import->OriginalFirstThunk = RDATA + lookup_at;
    import->Name = RDATA + kernel_at;
    import->FirstThunk = RDATA + iat_at;
    *(ULONGLONG *)(r + lookup_at) = RDATA + hint_at;
    *(ULONGLONG *)(r + iat_at) = RDATA + hint_at;
    strcpy((char *)r + hint_at + 2, S7_ANCHOR);
    strcpy((char *)r + kernel_at, S7_KERNEL);
    *(DWORD *)(image + section[1].PointerToRawData) = RDATA;
    *(DWORD *)(image + section[1].PointerToRawData + 4) = 12;

    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = file != INVALID_HANDLE_VALUE && WriteFile(file, image, file_size, &written, NULL) && written == file_size;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(image);
    free(sorted);
    return ok;
}
