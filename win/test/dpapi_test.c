// seals a fake cookie with libc's __w32_protect and opens it again. run it through blox7-console --w32-run
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*crypt_fn)(const void*, size_t, void**, size_t*);

int main(void) {
  crypt_fn protect = (crypt_fn)dlsym(RTLD_DEFAULT, "__w32_protect");
  crypt_fn unprotect = (crypt_fn)dlsym(RTLD_DEFAULT, "__w32_unprotect");
  if (!protect || !unprotect) {
    printf("FAIL no __w32_protect in libc\n");
    return 1;
  }
  const char secret[] = ".ROBLOSECURITY=not-a-real-cookie-just-a-test";
  const size_t length = sizeof secret - 1;
  void* sealed = NULL;
  size_t sealed_size = 0;
  int r = protect(secret, length, &sealed, &sealed_size);
  printf("check protect %d, %zu bytes\n", r, sealed_size);
  if (r != 0) return 1;
  printf("check sealed hides the cookie: %d\n", memmem(sealed, sealed_size, "not-a-real", 10) == NULL);
  void* plain = NULL;
  size_t plain_size = 0;
  r = unprotect(sealed, sealed_size, &plain, &plain_size);
  printf("check unprotect %d, round trip %d\n", r, r == 0 && plain_size == length && memcmp(plain, secret, length) == 0);
  ((unsigned char*)sealed)[sealed_size / 2] ^= 0x55;
  void* bad = NULL;
  size_t bad_size = 0;
  printf("check tampered blob refused: %d\n", unprotect(sealed, sealed_size, &bad, &bad_size) != 0);
  free(sealed);
  free(plain);
  return 0;
}
