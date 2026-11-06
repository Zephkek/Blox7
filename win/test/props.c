// what system properties answer in here
#include <stdio.h>
#include <sys/system_properties.h>

static int count;
static void each(const prop_info* pi, void* cookie) {
  (void)pi;
  (void)cookie;
  count++;
}

int main(void) {
  const char* keys[] = {"ro.build.version.sdk", "ro.build.version.release", "ro.product.model", "ro.product.manufacturer",
                        "ro.product.brand", "ro.product.device", "ro.hardware", "ro.product.cpu.abi",
                        "ro.build.fingerprint", "ro.debuggable", "ro.secure", "ro.build.tags", "ro.kernel.qemu",
                        "ro.boot.qemu", "ro.build.type", "ro.serialno"};
  for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
    char value[PROP_VALUE_MAX] = "";
    int n = __system_property_get(keys[i], value);
    printf("%-26s = \"%s\" (%d)  find=%p\n", keys[i], value, n, (const void*)__system_property_find(keys[i]));
  }
  int r = __system_property_foreach(each, NULL);
  printf("foreach: %d properties, result %d\n", count, r);
  return 0;
}
