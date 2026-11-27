#define _GNU_SOURCE
#include <dlfcn.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef unsigned(__attribute__((ms_abi)) * get_variable_fn)(const unsigned short*, unsigned short*, unsigned);

static int windows_has(const char* name) {
  void* (*pe_open)(const char*) = (void* (*)(const char*))dlsym(RTLD_DEFAULT, "__w32_pe_open");
  void* (*pe_symbol)(void*, const char*) = (void* (*)(void*, const char*))dlsym(RTLD_DEFAULT, "__w32_pe_symbol");
  if (!pe_open || !pe_symbol) return -1;
  void* kernel32 = pe_open("kernel32.dll");
  get_variable_fn get = kernel32 ? (get_variable_fn)pe_symbol(kernel32, "GetEnvironmentVariableW") : NULL;
  if (!get) return -1;
  unsigned short wide[64];
  size_t i = 0;
  for (; name[i] && i < 63; i++) wide[i] = (unsigned char)name[i];
  wide[i] = 0;
  return get(wide, NULL, 0) != 0;
}

static int child_sees(const char* self, int with) {
  char* argv[] = {(char*)self, "child", NULL};
  char* env[] = {"PATH=/usr/bin:/bin", "BLOX7_SHOW_WINDOW_ON_FIRST_SWAP=0", with ? "BLOX7_NO_DIALOGS=1" : NULL, NULL};
  pid_t pid;
  if (posix_spawn(&pid, self, NULL, NULL, argv, env) != 0) return -1;
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "child") == 0) return windows_has("BLOX7_NO_DIALOGS") == 1 ? 10 : 20;
  if (argc < 2) {
    printf("usage: nodialog_test <path of this program>\n");
    return 2;
  }
  const int own = windows_has("BLOX7_NO_DIALOGS");
  if (own != 0) {
    printf("%s\n", own < 0 ? "cannot read the Windows environment" : "BLOX7_NO_DIALOGS is set here already");
    return 2;
  }
  const int with = child_sees(argv[1], 1);
  const int without = child_sees(argv[1], 0);
  printf("child spawned with BLOX7_NO_DIALOGS=1: %s\n", with == 10 ? "no dialogs" : with == 20 ? "DIALOGS" : "failed");
  printf("child spawned without it: %s\n", without == 20 ? "dialogs" : without == 10 ? "NO DIALOGS" : "failed");
  return with == 10 && without == 20 ? 0 : 1;
}
