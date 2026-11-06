#include <cstdio>
#include <cstdlib>

namespace {

__attribute__((constructor)) void FailIfLoaded() {
  std::fputs("unknown Roblox fixture reached native loading\n", stderr);
  std::abort();
}

}  // namespace

extern "C" int JNI_OnLoad(void*, void*) { return 0x10006; }
