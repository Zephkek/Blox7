// mcpelauncher-linker's api, but fake
#include <dlfcn.h>
#include <link.h>
#include <mcpelauncher/linker.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
void* __w32_ns_dlopen(const char* name, int flags);
void* __w32_ns_dlsym(void* handle, const char* symbol, const void* caller);
void __w32_ns_set_library_path(const char* path);
void* __w32_ns_synthetic(const char* soname);
void __w32_ns_add_symbols(void* handle, const char* const* names, void* const* addrs, size_t count);
uintptr_t __w32_ns_base(void* handle);
void __w32_ns_code_region(void* handle, uintptr_t* base, size_t* size);
void __w32_ns_set_constructor_hooks(void (*before)(const char*, uintptr_t), bool (*skip)(const char*));

void blox7_before_soinfo_constructors(const char* realpath, uintptr_t base) __attribute__((weak));
bool blox7_should_skip_soinfo_constructors(const char* realpath) __attribute__((weak));
}

namespace {

void add_symbols(void* handle, const std::unordered_map<std::string, void*>& symbols) {
  std::vector<const char*> names;
  std::vector<void*> addrs;
  names.reserve(symbols.size());
  addrs.reserve(symbols.size());
  for (const auto& [name, address] : symbols) {
    names.push_back(name.c_str());
    addrs.push_back(address);
  }
  __w32_ns_add_symbols(handle, names.data(), addrs.data(), names.size());
}

void* guest_dlopen(const char* name, int flags) { return __w32_ns_dlopen(name, flags); }
void* guest_dlsym(void* handle, const char* symbol) {
  return __w32_ns_dlsym(handle, symbol, __builtin_return_address(0));
}
int guest_dlclose(void* handle) { return ::dlclose(handle); }
char* guest_dlerror() { return ::dlerror(); }
int guest_dladdr(const void* addr, Dl_info* info) { return ::dladdr(addr, info); }
void* guest_android_dlopen_ext(const char* name, int flags, const void*) { return __w32_ns_dlopen(name, flags); }
int guest_dl_iterate_phdr(int (*cb)(dl_phdr_info*, size_t, void*), void* data) { return ::dl_iterate_phdr(cb, data); }

}  // namespace

extern "C" {

void* __loader_dlopen(const char* filename, int flags, const void*) { return __w32_ns_dlopen(filename, flags); }
void* __loader_dlsym(void* handle, const char* symbol, const void* caller) { return __w32_ns_dlsym(handle, symbol, caller); }
int __loader_dladdr(const void* addr, Dl_info* info) { return ::dladdr(addr, info); }
int __loader_dlclose(void* handle) { return ::dlclose(handle); }
char* __loader_dlerror() { return ::dlerror(); }
int __loader_dl_iterate_phdr(int (*cb)(dl_phdr_info*, size_t, void*), void* data) { return ::dl_iterate_phdr(cb, data); }
void __loader_android_update_LD_LIBRARY_PATH(const char* path) { __w32_ns_set_library_path(path); }
void* __loader_android_dlopen_ext(const char* filename, int flags, const android_dlextinfo*, const void*) {
  return __w32_ns_dlopen(filename, flags);
}

}  // extern "C"

namespace linker {

namespace libdl {
std::unordered_map<std::string, void*> get_dl_symbols() {
  return {
      {"dlopen", reinterpret_cast<void*>(guest_dlopen)},
      {"dlsym", reinterpret_cast<void*>(guest_dlsym)},
      {"dlclose", reinterpret_cast<void*>(guest_dlclose)},
      {"dlerror", reinterpret_cast<void*>(guest_dlerror)},
      {"dladdr", reinterpret_cast<void*>(guest_dladdr)},
      {"android_dlopen_ext", reinterpret_cast<void*>(guest_android_dlopen_ext)},
      {"dl_iterate_phdr", reinterpret_cast<void*>(guest_dl_iterate_phdr)},
  };
}
}  // namespace libdl

void init() {
  __w32_ns_set_constructor_hooks(blox7_before_soinfo_constructors, blox7_should_skip_soinfo_constructors);
  load_library("libdl.so", libdl::get_dl_symbols());
}

void* load_library(const char* name, const std::unordered_map<std::string, void*>& symbols) {
  void* handle = __w32_ns_synthetic(name);
  add_symbols(handle, symbols);
  return handle;
}

int unload_library(void*) { return 0; }

void relocate(void* handle, const std::unordered_map<std::string, void*>& symbols) { add_symbols(handle, symbols); }

size_t get_library_base(void* handle) { return __w32_ns_base(handle); }

void get_library_code_region(void* handle, size_t& base, size_t& size) {
  uintptr_t b = 0;
  size_t s = 0;
  __w32_ns_code_region(handle, &b, &s);
  base = b;
  size = s;
}

int dlclose_unlocked(void* handle) { return ::dlclose(handle); }

}  // namespace linker
