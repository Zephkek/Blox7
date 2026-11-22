# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

include_guard(GLOBAL)

get_filename_component(BLOX7_PLATFORM_GRAPHICS_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
)

find_package(SDL3 3.4 REQUIRED CONFIG)
find_path(BLOX7_EGL_INCLUDE_DIR EGL/egl.h REQUIRED)
find_path(BLOX7_GLES3_INCLUDE_DIR GLES3/gl3.h REQUIRED)

set(BLOX7_WINDOW_ICON_PNG "${BLOX7_PLATFORM_GRAPHICS_ROOT}/win/blox7/blox7.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${BLOX7_WINDOW_ICON_PNG}"
)
file(READ "${BLOX7_WINDOW_ICON_PNG}" BLOX7_WINDOW_ICON_HEX HEX)
string(LENGTH "${BLOX7_WINDOW_ICON_HEX}" BLOX7_WINDOW_ICON_HEX_LENGTH)
set(BLOX7_WINDOW_ICON_BYTES "")
set(BLOX7_WINDOW_ICON_HEX_OFFSET 0)
set(BLOX7_WINDOW_ICON_COLUMN 0)
while(BLOX7_WINDOW_ICON_HEX_OFFSET LESS BLOX7_WINDOW_ICON_HEX_LENGTH)
  string(SUBSTRING "${BLOX7_WINDOW_ICON_HEX}"
    ${BLOX7_WINDOW_ICON_HEX_OFFSET} 2 BLOX7_WINDOW_ICON_BYTE
  )
  string(APPEND BLOX7_WINDOW_ICON_BYTES
    "0x${BLOX7_WINDOW_ICON_BYTE}, "
  )
  math(EXPR BLOX7_WINDOW_ICON_HEX_OFFSET
    "${BLOX7_WINDOW_ICON_HEX_OFFSET} + 2"
  )
  math(EXPR BLOX7_WINDOW_ICON_COLUMN
    "${BLOX7_WINDOW_ICON_COLUMN} + 1"
  )
  if(BLOX7_WINDOW_ICON_COLUMN EQUAL 12)
    string(APPEND BLOX7_WINDOW_ICON_BYTES "\n    ")
    set(BLOX7_WINDOW_ICON_COLUMN 0)
  endif()
endwhile()

set(BLOX7_PLATFORM_GENERATED_INCLUDE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
file(MAKE_DIRECTORY
  "${BLOX7_PLATFORM_GENERATED_INCLUDE_DIR}/blox7/platform"
)
configure_file(
  "${BLOX7_PLATFORM_GRAPHICS_ROOT}/cmake/templates/sdl_window_icon_data.h.in"
  "${BLOX7_PLATFORM_GENERATED_INCLUDE_DIR}/blox7/platform/sdl_window_icon_data.h"
  @ONLY
)

set(BLOX7_ANGLE_HEADERS_INCLUDE_DIR
  "${BLOX7_PLATFORM_GRAPHICS_ROOT}/third_party/angle_headers/include"
)
if(NOT EXISTS
    "${BLOX7_ANGLE_HEADERS_INCLUDE_DIR}/EGL/eglext_angle.h")
  message(FATAL_ERROR "pinned ANGLE EGL extension header is unavailable")
endif()
add_library(blox7_angle_headers INTERFACE)
target_include_directories(blox7_angle_headers SYSTEM INTERFACE
  "${BLOX7_ANGLE_HEADERS_INCLUDE_DIR}"
)
add_library(Blox7::AngleHeaders ALIAS blox7_angle_headers)

add_library(blox7_platform_sdl STATIC
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_application_metadata.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_display_refresh_capabilities.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_event_converter.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_gamepad_manager.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_platform_runtime.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_text_clipboard.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_window_icon.cc
)
target_include_directories(blox7_platform_sdl PUBLIC
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/include
)
target_include_directories(blox7_platform_sdl PRIVATE
  ${BLOX7_PLATFORM_GENERATED_INCLUDE_DIR}
)
target_link_libraries(blox7_platform_sdl PUBLIC SDL3::SDL3)
target_compile_features(blox7_platform_sdl PUBLIC cxx_std_17)
target_compile_definitions(blox7_platform_sdl PRIVATE
  BLOX7_PROJECT_VERSION="${PROJECT_VERSION}"
)
add_library(Blox7::PlatformSdl ALIAS blox7_platform_sdl)

add_library(blox7_graphics_foundation STATIC
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/graphics/bionic_egl_bridge.cc
)
target_include_directories(blox7_graphics_foundation
  PUBLIC ${BLOX7_PLATFORM_GRAPHICS_ROOT}/include
  PRIVATE ${BLOX7_EGL_INCLUDE_DIR}
)
target_link_libraries(blox7_graphics_foundation PRIVATE
  Blox7::AngleHeaders
  ${CMAKE_DL_LIBS}
)
target_compile_features(blox7_graphics_foundation PUBLIC cxx_std_17)
add_library(Blox7::GraphicsFoundation ALIAS blox7_graphics_foundation)

add_library(blox7_gles_text_overlay STATIC
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/graphics/gles_text_overlay_compositor.cc
)
target_include_directories(blox7_gles_text_overlay
  PUBLIC ${BLOX7_PLATFORM_GRAPHICS_ROOT}/include
  PRIVATE ${BLOX7_GLES3_INCLUDE_DIR}
)
target_link_libraries(blox7_gles_text_overlay PUBLIC SDL3::SDL3)
target_compile_features(blox7_gles_text_overlay PUBLIC cxx_std_17)
blox7_apply_compile_options(blox7_gles_text_overlay)
add_library(Blox7::GlesTextOverlay ALIAS blox7_gles_text_overlay)

add_library(blox7_sdl_vulkan_wsi STATIC
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/graphics/sdl_vulkan_wsi.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/graphics/android_vulkan_wsi_adapter.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/graphics/present_mode_policy.cc
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/src/graphics/roblox_renderer_checks.cc
)
set_target_properties(blox7_sdl_vulkan_wsi PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(blox7_sdl_vulkan_wsi PUBLIC
  ${BLOX7_PLATFORM_GRAPHICS_ROOT}/include
)
target_link_libraries(blox7_sdl_vulkan_wsi PUBLIC SDL3::SDL3)
target_link_libraries(blox7_sdl_vulkan_wsi PUBLIC Vulkan::Headers)
target_compile_features(blox7_sdl_vulkan_wsi PUBLIC cxx_std_17)
add_library(Blox7::SdlVulkanWsi ALIAS blox7_sdl_vulkan_wsi)
