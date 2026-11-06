# sdl's own headers plus our libSDL3.so, which forwards to the real SDL3.dll
set(SDL3_VERSION 3.4.20)
set(SDL3_FOUND TRUE)
if(NOT TARGET SDL3::SDL3)
  add_library(SDL3::SDL3 SHARED IMPORTED GLOBAL)
  set_target_properties(SDL3::SDL3 PROPERTIES
    IMPORTED_LOCATION "${BLOX7_WIN_OUT}/libSDL3.so"
    IMPORTED_SONAME "libSDL3.so"
    INTERFACE_INCLUDE_DIRECTORIES "${BLOX7_WIN_SDL_SOURCE}/include")
  add_library(SDL3::SDL3-shared ALIAS SDL3::SDL3)
endif()
