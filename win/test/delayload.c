#include "../webview/delayload_guard.h"

#include <stdio.h>
#include <stdlib.h>

typedef ULONG(WINAPI* event_set_information_fn)(REGHANDLE, EVENT_INFO_CLASS, PVOID, ULONG);

static const DWORD kProcNotFound = VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND);
static const DWORD kModuleNotFound = VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND);

static FARPROC g_slot;
static int g_passed_on, g_passed, g_failed;

static LONG CALLBACK catch_rest(EXCEPTION_POINTERS* exception) {
  const DWORD code = exception->ExceptionRecord->ExceptionCode;
  if (code != kProcNotFound && code != kModuleNotFound) return EXCEPTION_CONTINUE_SEARCH;
  g_passed_on++;
  return EXCEPTION_CONTINUE_EXECUTION;
}

static void check(int ok, const char* what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (ok) {
    g_passed++;
  } else {
    g_failed++;
  }
}

static DelayLoadInfo record(const char* dll, const char* name, FARPROC* slot) {
  DelayLoadInfo info;
  memset(&info, 0, sizeof info);
  info.cb = sizeof info;
  info.ppfn = slot;
  info.szDll = dll;
  info.dlp.fImportByName = name != NULL;
  if (name != NULL) {
    info.dlp.szProcName = name;
  } else {
    info.dlp.dwOrdinal = 7;
  }
  return info;
}

static FARPROC raise_failure(DWORD code, DelayLoadInfo* info, DWORD arguments) {
  ULONG_PTR argument = (ULONG_PTR)info;
  g_passed_on = 0;
  info->pfnCur = NULL;
  RaiseException(code, 0, arguments, &argument);
  return info->pfnCur;
}

static void passed_on(const char* what, DWORD code, DelayLoadInfo info, DWORD arguments) {
  const FARPROC got = raise_failure(code, &info, arguments);
  check(got == NULL && g_passed_on == 1, what);
}

int main(void) {
  const FARPROC stub = (FARPROC)(void (*)(void))delayload_guard_stub;
  FARPROC* outside = (FARPROC*)malloc(sizeof *outside);
  if (outside == NULL) return 2;
  const ULONG major = *(const volatile ULONG*)0x7FFE026C, minor = *(const volatile ULONG*)0x7FFE0270;
  printf("windows %lu.%lu, advapi32 EventSetInformation %s, guard needed %s\n", major, minor,
         GetProcAddress(LoadLibraryW(L"advapi32.dll"), "EventSetInformation") != NULL ? "present" : "missing",
         delayload_guard_needed() ? "yes" : "no");
  check(sizeof(DelayLoadInfo) == 0x48, "DelayLoadInfo is the 0x48 bytes webview2loader puts in cb");
  if (major > 6 || (major == 6 && minor >= 2)) check(!delayload_guard_needed(), "not needed on windows 8 and newer");

  AddVectoredExceptionHandler(1, delayload_guard_handler);
  AddVectoredExceptionHandler(0, catch_rest);

  DelayLoadInfo info = record("ADVAPI32.dll", "EventSetInformation", &g_slot);
  passed_on("loader image not known yet: passed on", kProcNotFound, info, 1);

  delayload_guard_loader = GetModuleHandleW(NULL);
  info = record("ADVAPI32.dll", "EventSetInformation", &g_slot);
  g_slot = raise_failure(kProcNotFound, &info, 1);
  check(g_slot == stub && g_passed_on == 0, "advapi32 EventSetInformation in the loader's iat gets the stand-in");
  check(g_slot != NULL && ((event_set_information_fn)(void (*)(void))g_slot)(1, (EVENT_INFO_CLASS)2, NULL, 0) ==
                              ERROR_NOT_SUPPORTED,
        "the stand-in returns ERROR_NOT_SUPPORTED");

  info = record("advapi32.DLL", "EventSetInformation", &g_slot);
  check(raise_failure(kProcNotFound, &info, 1) == stub && g_passed_on == 0, "dll name matches in any case");

  passed_on("wrong dll: passed on", kProcNotFound, record("KERNEL32.dll", "EventSetInformation", &g_slot), 1);
  passed_on("dll name without .dll: passed on", kProcNotFound, record("ADVAPI32", "EventSetInformation", &g_slot), 1);
  passed_on("wrong name: passed on", kProcNotFound, record("ADVAPI32.dll", "EventRegister", &g_slot), 1);
  passed_on("name in other case: passed on", kProcNotFound, record("ADVAPI32.dll", "eventsetinformation", &g_slot), 1);
  passed_on("by ordinal: passed on", kProcNotFound, record("ADVAPI32.dll", NULL, &g_slot), 1);
  passed_on("slot outside the loader image: passed on", kProcNotFound,
            record("ADVAPI32.dll", "EventSetInformation", outside), 1);
  passed_on("module not found code: passed on", kModuleNotFound,
            record("ADVAPI32.dll", "EventSetInformation", &g_slot), 1);
  passed_on("no exception argument: passed on", kProcNotFound,
            record("ADVAPI32.dll", "EventSetInformation", &g_slot), 0);
  info = record("ADVAPI32.dll", "EventSetInformation", &g_slot);
  info.cb = 0x40;
  passed_on("other cb: passed on", kProcNotFound, info, 1);

  free(outside);
  printf("passed %d failed %d\n", g_passed, g_failed);
  return g_failed ? 1 : 0;
}
