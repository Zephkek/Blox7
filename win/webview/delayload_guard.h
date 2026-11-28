#ifndef BLOX7_DELAYLOAD_GUARD_H
#define BLOX7_DELAYLOAD_GUARD_H

#include <windows.h>
#include <delayimp.h>
#include <evntprov.h>
#include <string.h>

static HMODULE delayload_guard_loader;

static ULONG WINAPI delayload_guard_stub(REGHANDLE handle, EVENT_INFO_CLASS information_class, PVOID information,
                                         ULONG length) {
  (void)handle;
  (void)information_class;
  (void)information;
  (void)length;
  return ERROR_NOT_SUPPORTED;
}

static int delayload_guard_inside(HMODULE module, const void* address) {
  const BYTE* base = (const BYTE*)module;
  if (base == NULL) return 0;
  const IMAGE_NT_HEADERS* headers = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
  return (const BYTE*)address >= base && (const BYTE*)address < base + headers->OptionalHeader.SizeOfImage;
}

static LONG CALLBACK delayload_guard_handler(EXCEPTION_POINTERS* exception) {
  const EXCEPTION_RECORD* record = exception->ExceptionRecord;
  if (record->ExceptionCode != (DWORD)VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND) ||
      record->NumberParameters < 1) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  DelayLoadInfo* info = (DelayLoadInfo*)record->ExceptionInformation[0];
  if (info == NULL || info->cb != sizeof *info || !delayload_guard_inside(delayload_guard_loader, info->ppfn) ||
      info->szDll == NULL || _stricmp(info->szDll, "ADVAPI32.dll") != 0 || !info->dlp.fImportByName ||
      info->dlp.szProcName == NULL || strcmp(info->dlp.szProcName, "EventSetInformation") != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  info->pfnCur = (FARPROC)(void (*)(void))delayload_guard_stub;
  return EXCEPTION_CONTINUE_EXECUTION;
}

static int delayload_guard_needed(void) {
  HMODULE advapi = LoadLibraryW(L"advapi32.dll");
  return advapi != NULL && GetProcAddress(advapi, "EventSetInformation") == NULL;
}

#endif
