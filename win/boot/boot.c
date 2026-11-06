/* maps our libc.so (an android elf, yes) and jumps in */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include "../include/w32boot.h"
#include "boot.h"

#define MAIN_STACK_SIZE (16u << 20)

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { int64_t tag; uint64_t val; } Dyn;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_PHDR = 6, PF_X = 1, PF_W = 2, PF_R = 4 };
enum { DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23 };
enum { R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7, R_X86_64_RELATIVE = 8 };

static struct w32boot g_boot;
static w32_libc_start_fn g_start;
static int g_exit_code = 1;

static const wchar_t *g_product = L"Blox7";

void boot_set_product(const wchar_t *name) { g_product = name; }

__attribute__((noreturn)) void boot_fail(const wchar_t *what, DWORD error) {
    wchar_t text[512];
    _snwprintf(text, 511, L"%ls could not start: %ls (code %lu).", g_product, what, error);
    text[511] = 0;
    if (GetEnvironmentVariableW(L"BLOX7_NO_DIALOGS", NULL, 0)) {
        DWORD done;
        char line[1024];
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, line, sizeof line - 2, NULL, NULL);
        if (n > 0) { line[n - 1] = '\n'; WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)n, &done, NULL); }
    } else {
        MessageBoxW(NULL, text, g_product, MB_ICONERROR);
    }
    ExitProcess(1);
}

static DWORD protection(uint32_t flags) {
    if (flags & PF_X) return (flags & PF_W) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return (flags & PF_W) ? PAGE_READWRITE : PAGE_READONLY;
}

static uint8_t *read_file(const wchar_t *path, size_t *size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER length;
    uint8_t *data;
    DWORD got;
    if (file == INVALID_HANDLE_VALUE) boot_fail(L"libc.so is missing", GetLastError());
    if (!GetFileSizeEx(file, &length) || length.QuadPart > (1 << 30)) boot_fail(L"libc.so is unreadable", GetLastError());
    data = HeapAlloc(GetProcessHeap(), 0, (size_t)length.QuadPart);
    if (!data || !ReadFile(file, data, (DWORD)length.QuadPart, &got, NULL) || got != (DWORD)length.QuadPart)
        boot_fail(L"libc.so is unreadable", GetLastError());
    CloseHandle(file);
    *size = (size_t)length.QuadPart;
    return data;
}

static void map_libc(const wchar_t *path) {
    size_t size, i;
    uint8_t *file = read_file(path, &size), *base;
    const Ehdr *eh = (const Ehdr *)file;
    const Phdr *ph;
    const Dyn *dyn = NULL;
    uint64_t lo = UINT64_MAX, hi = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    DWORD old;

    if (size < sizeof(Ehdr) || memcmp(eh->ident, "\177ELF\2\1", 6) || eh->machine != 62 ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(Phdr) > size)
        boot_fail(L"libc.so is not an x86_64 ELF file", 0);
    ph = (const Phdr *)(file + eh->phoff);
    for (i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD) {
            if ((ph[i].vaddr & ~0xfffull) < lo) lo = ph[i].vaddr & ~0xfffull;
            if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
        }
    if (lo != 0) boot_fail(L"libc.so must be linked at address 0", 0);
    base = VirtualAlloc(NULL, (size_t)hi, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) boot_fail(L"no memory for libc.so", GetLastError());
    for (i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD) {
            if (ph[i].offset + ph[i].filesz > size) boot_fail(L"libc.so is truncated", 0);
            memcpy(base + ph[i].vaddr, file + ph[i].offset, (size_t)ph[i].filesz);
        } else if (ph[i].type == PT_DYNAMIC) {
            dyn = (const Dyn *)(base + ph[i].vaddr);
        }
    }
    if (!dyn) boot_fail(L"libc.so has no dynamic section", 0);
    for (; dyn->tag != DT_NULL; dyn++) {
        if (dyn->tag == DT_RELA) rela = dyn->val;
        else if (dyn->tag == DT_RELASZ) relasz = dyn->val;
        else if (dyn->tag == DT_JMPREL) jmprel = dyn->val;
        else if (dyn->tag == DT_PLTRELSZ) pltrelsz = dyn->val;
        else if (dyn->tag == DT_NEEDED) boot_fail(L"libc.so must be self-contained", (DWORD)dyn->tag);
    }
    for (i = 0; i < relasz / sizeof(Rela); i++) {
        const Rela *r = (const Rela *)(base + rela) + i;
        if ((uint32_t)r->info == R_X86_64_GLOB_DAT) *(uint64_t *)(base + r->offset) = 0;
        else if ((uint32_t)r->info == R_X86_64_RELATIVE) *(uint64_t *)(base + r->offset) = (uint64_t)(base + r->addend);
        else boot_fail(L"libc.so has a relocation the bootstrap cannot apply", (DWORD)r->info);
    }
    for (i = 0; i < pltrelsz / sizeof(Rela); i++) {
        const Rela *r = (const Rela *)(base + jmprel) + i;
        if ((uint32_t)r->info != R_X86_64_JUMP_SLOT) boot_fail(L"libc.so has a PLT relocation the bootstrap cannot apply", (DWORD)r->info);
        *(uint64_t *)(base + r->offset) = 0;
    }
    for (i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD) {
            uint64_t start = ph[i].vaddr & ~0xfffull;
            if (!VirtualProtect(base + start, (size_t)(ph[i].vaddr + ph[i].memsz - start), protection(ph[i].flags), &old))
                boot_fail(L"could not protect libc.so", GetLastError());
        }
    FlushInstructionCache(GetCurrentProcess(), base, (size_t)hi);

    g_boot.libc_base = (uintptr_t)base;
    g_boot.libc_phdr = base + eh->phoff;
    g_boot.libc_phnum = eh->phnum;
    g_start = (w32_libc_start_fn)(base + eh->entry);
    HeapFree(GetProcessHeap(), 0, file);
}

static char *utf8(const wchar_t *text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *out = HeapAlloc(GetProcessHeap(), 0, (size_t)n);
    if (!out) boot_fail(L"out of memory", 0);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out, n, NULL, NULL);
    return out;
}

/* libc wants one exact TEB slot, so keep grabbing slots until windows hands it over */
static void claim_tls_slot(void) {
    DWORD taken[W32BOOT_TLS_SLOT + 1], n = 0, i, slot = 0;
    while (n <= W32BOOT_TLS_SLOT) {
        slot = TlsAlloc();
        if (slot == TLS_OUT_OF_INDEXES || slot > W32BOOT_TLS_SLOT) break;
        taken[n++] = slot;
        if (slot == W32BOOT_TLS_SLOT) break;
    }
    for (i = 0; i < n; i++)
        if (taken[i] != W32BOOT_TLS_SLOT) TlsFree(taken[i]);
    if (slot != W32BOOT_TLS_SLOT) boot_fail(L"Windows TLS slot 63 is taken", slot);
}

static HANDLE g_boot_log = INVALID_HANDLE_VALUE;

static void boot_log(const char *format, ...) {
    char line[512];
    va_list ap;
    int n;
    DWORD done;
    int prefix;
    if (g_boot_log == INVALID_HANDLE_VALUE) return;
    prefix = _snprintf(line, 16, "[%lu] ", GetCurrentProcessId());
    va_start(ap, format);
    n = _vsnprintf(line + prefix, sizeof line - 2 - prefix, format, ap);
    va_end(ap);
    n = n < 0 ? (int)sizeof line - 2 : n + prefix;
    line[n++] = '\r';
    line[n++] = '\n';
    WriteFile(g_boot_log, line, (DWORD)n, &done, NULL);
}

static int elf_module(uintptr_t addr, char *name, size_t name_size, uintptr_t *offset) {
    MEMORY_BASIC_INFORMATION m;
    const unsigned char *base;
    const Ehdr *eh;
    const Phdr *ph;
    int i;
    if (!VirtualQuery((void *)addr, &m, sizeof m) || !m.AllocationBase) return 0;
    base = (const unsigned char *)m.AllocationBase;
    if (!VirtualQuery(base, &m, sizeof m) || m.State != MEM_COMMIT || memcmp(base, "\177ELF", 4)) return 0;
    eh = (const Ehdr *)base;
    ph = (const Phdr *)(base + eh->phoff);
    *offset = addr - (uintptr_t)base;
    _snprintf(name, name_size, "(unnamed ELF)");
    for (i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) {
            const Dyn *d = (const Dyn *)(base + ph[i].vaddr);
            uint64_t strtab = 0, soname = 0;
            for (; d->tag != DT_NULL; d++) {
                if (d->tag == 5) strtab = d->val;       /* DT_STRTAB */
                else if (d->tag == 14) soname = d->val; /* DT_SONAME */
            }
            if (strtab && soname) _snprintf(name, name_size, "%s", (const char *)base + strtab + soname);
            else if (strtab) _snprintf(name, name_size, "(no SONAME, maybe the program)");
        }
    }
    name[name_size - 1] = 0;
    return 1;
}

static LONG CALLBACK last_chance(EXCEPTION_POINTERS *e) {
    CONTEXT *c = e->ContextRecord;
    uintptr_t rip = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    uintptr_t *sp = (uintptr_t *)c->Rsp;
    int i, shown = 0;
    if (e->ExceptionRecord->ExceptionCode < 0xc0000000u) return EXCEPTION_CONTINUE_SEARCH;
    char module[128];
    uintptr_t offset;
    if (elf_module(rip, module, sizeof module, &offset))
        boot_log("exception 0x%08lx at %p (%s+0x%llx) address %p, thread %lu", e->ExceptionRecord->ExceptionCode,
                 (void *)rip, module, (unsigned long long)offset, (void *)e->ExceptionRecord->ExceptionInformation[1],
                 GetCurrentThreadId());
    else
        boot_log("exception 0x%08lx at %p (not ELF code) address %p, thread %lu", e->ExceptionRecord->ExceptionCode,
                 (void *)rip, (void *)e->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    boot_log("rax %016llx rbx %016llx rcx %016llx rdx %016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    boot_log("rsi %016llx rdi %016llx rbp %016llx rsp %016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    boot_log("r8  %016llx r9  %016llx r10 %016llx r11 %016llx", c->R8, c->R9, c->R10, c->R11);
    boot_log("r12 %016llx r13 %016llx r14 %016llx r15 %016llx", c->R12, c->R13, c->R14, c->R15);
    for (i = 0; i < 2048 && shown < 24; i++) {
        MEMORY_BASIC_INFORMATION m, code;
        if (!VirtualQuery(sp + i, &m, sizeof m) || m.State != MEM_COMMIT) break;
        if (sp[i] > 0x10000 && VirtualQuery((void *)(sp[i] - 1), &code, sizeof code) && code.State == MEM_COMMIT &&
            (code.Protect & 0xf0) && elf_module(sp[i], module, sizeof module, &offset)) {
            boot_log("  stack[%d] %s+0x%llx", i, module, (unsigned long long)offset);
            shown++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI main_thread(LPVOID unused) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    (void)unused;
    g_boot.stack_low = (uintptr_t)tib->StackLimit;
    g_boot.stack_high = (uintptr_t)tib->StackBase;
    boot_log("boot: libc.so at %p, calling its entry", (void *)g_boot.libc_base);
    g_exit_code = g_start(&g_boot);
    boot_log("boot: libc.so returned %d", g_exit_code);
    return (DWORD)g_exit_code;
}


void boot_prepare(void) {
    wchar_t log_path[MAX_PATH];
    if (GetEnvironmentVariableW(L"BLOX7_BOOT_LOG", log_path, MAX_PATH)) {
        g_boot_log = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        AddVectoredExceptionHandler(0, last_chance);
        boot_log("boot: started");
    }
    if (!VirtualAlloc((void *)(uintptr_t)W32BOOT_FS_REGION_START,
                      W32BOOT_FS_REGION_END - W32BOOT_FS_REGION_START, MEM_RESERVE, PAGE_READWRITE))
        boot_fail(L"could not reserve the stack-guard region", GetLastError());
    claim_tls_slot();
}

int boot_run(const wchar_t *bin_dir, int argc, wchar_t **argv) {
    wchar_t path[MAX_PATH + 16];
    HANDLE thread;
    int i;
    if (wcslen(bin_dir) + 8 >= MAX_PATH) boot_fail(L"the runtime folder path is too long", 0);
    wcscpy(path, bin_dir);
    wcscat(path, L"libc.so");
    map_libc(path);

    g_boot.version = W32BOOT_VERSION;
    g_boot.size = sizeof g_boot;
    g_boot.GetModuleHandleW = (void *)GetModuleHandleW;
    g_boot.LoadLibraryW = (void *)LoadLibraryW;
    g_boot.GetProcAddress = (void *)GetProcAddress;
    g_boot.exe_dir = utf8(bin_dir);
    g_boot.argc = argc;
    g_boot.argv = HeapAlloc(GetProcessHeap(), 0, sizeof(char *) * ((size_t)argc + 1));
    if (!g_boot.argv) boot_fail(L"out of memory", 0);
    for (i = 0; i < argc; i++) g_boot.argv[i] = utf8(argv[i]);
    g_boot.argv[argc] = NULL;

    thread = CreateThread(NULL, MAIN_STACK_SIZE, main_thread, NULL, 0, NULL);
    if (!thread) boot_fail(L"could not start the main thread", GetLastError());
    WaitForSingleObject(thread, INFINITE);
    return g_exit_code;
}
