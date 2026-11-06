#include <cstddef>
#include <cstdlib>

extern "C" {

__attribute__((visibility("default"), noinline, used)) void*
Blox7FixtureSmallAllocate(std::size_t size) {
  return std::malloc(size);
}

__attribute__((visibility("default"), noinline, used)) void*
Blox7FixtureAllocate(std::size_t size) {
  return std::malloc(size);
}

__attribute__((visibility("default"), noinline, used)) void*
Blox7FixtureReallocate(void* pointer, std::size_t size) {
  return std::realloc(pointer, size);
}

__attribute__((visibility("default"), noinline, used)) void*
Blox7FixtureAlignedAllocate(std::size_t alignment, std::size_t size) {
  void* pointer = nullptr;
  return posix_memalign(&pointer, alignment, size) == 0 ? pointer : nullptr;
}

__attribute__((visibility("default"), noinline, used)) void Blox7FixtureFree(
    void* pointer) {
  std::free(pointer);
}

__attribute__((visibility("default"), noinline, used)) std::size_t
Blox7FixtureUsableSize(void*) {
  return 1;
}

__attribute__((visibility("default"), noinline, used)) void
Blox7FixtureArenaInitialize() {}

__attribute__((visibility("default"), noinline, used)) void
Blox7FixtureThreadInitialize() {}

__attribute__((visibility("default"), noinline, used)) void
Blox7FixtureRegistryInitialize() {}

__attribute__((visibility("default"), constructor(101), used)) void
Blox7FixtureConstructorTwo() {}

__attribute__((visibility("default"), constructor(102), used)) void
Blox7FixtureConstructorThree() {}

__attribute__((visibility("default"), constructor(103), used)) void
Blox7FixtureConstructorFour() {}

__attribute__((visibility("default"), constructor(104), used)) void
Blox7FixtureConstructorFive() {}

__attribute__((visibility("default"),
               used)) void* blox7_fixture_allocator_slot = nullptr;
__attribute__((visibility("default"),
               used)) void* blox7_fixture_empty_string_slot = nullptr;
__attribute__((visibility("default"), used)) void* blox7_fixture_jni_slot =
    nullptr;
__attribute__((visibility("default"),
               used)) void* blox7_fixture_arena_guard_slot = nullptr;
__attribute__((visibility("default"),
               used)) void* blox7_fixture_arena_table_slot = nullptr;
__attribute__((visibility("default"),
               used)) void* blox7_fixture_registry_slot = nullptr;

}  // extern "C"

namespace {

struct FixtureConstructor {
  FixtureConstructor() { blox7_fixture_arena_guard_slot = nullptr; }
};

FixtureConstructor g_fixture_constructor;

}  // namespace
