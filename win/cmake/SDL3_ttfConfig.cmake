# same deal for SDL3_ttf.dll
set(SDL3_ttf_FOUND TRUE)
if(NOT TARGET SDL3_ttf::SDL3_ttf)
  add_library(SDL3_ttf::SDL3_ttf SHARED IMPORTED GLOBAL)
  set_target_properties(SDL3_ttf::SDL3_ttf PROPERTIES
    IMPORTED_LOCATION "${BLOX7_WIN_OUT}/libSDL3_ttf.so"
    IMPORTED_SONAME "libSDL3_ttf.so"
    INTERFACE_INCLUDE_DIRECTORIES "${BLOX7_WIN_SDL_TTF_SOURCE}/include"
    INTERFACE_LINK_LIBRARIES SDL3::SDL3)
endif()
