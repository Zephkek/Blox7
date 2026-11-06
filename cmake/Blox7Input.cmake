# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

include_guard(GLOBAL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
find_package(SDL3_ttf REQUIRED CONFIG)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

get_filename_component(BLOX7_INPUT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.."
  ABSOLUTE
)

add_library(blox7_input_runtime STATIC
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_gamepad_input.cc
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_input_native_adapter.cc
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_input_router.cc
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_native_text_box_info_reader.cc
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_text_display_state.cc
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_text_editor.cc
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_text_font_resolver.cc
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_text_surface_overlay.cc
  ${BLOX7_INPUT_ROOT}/src/runtime/roblox_window_input_runtime.cc
)
add_library(Blox7::InputRuntime ALIAS blox7_input_runtime)
target_include_directories(blox7_input_runtime PUBLIC
  ${BLOX7_INPUT_ROOT}/include
)
target_include_directories(blox7_input_runtime PRIVATE
  ${BLOX7_INPUT_ROOT}/src
)
target_link_libraries(blox7_input_runtime PUBLIC
  Blox7::PlatformSdl
  Blox7::Runtime
  blox7_window
  PkgConfig::UTF8PROC
  PRIVATE
    PkgConfig::FONTCONFIG
    SDL3_ttf::SDL3_ttf
    nlohmann_json::nlohmann_json
)
target_compile_features(blox7_input_runtime PUBLIC cxx_std_17)
blox7_apply_compile_options(blox7_input_runtime)

if(BUILD_TESTING AND TARGET GTest::gtest_main)
  add_executable(roblox_input_router_test
    ${BLOX7_INPUT_ROOT}/tests/roblox_gamepad_input_test.cc
    ${BLOX7_INPUT_ROOT}/tests/roblox_input_router_test.cc
  )
  target_link_libraries(roblox_input_router_test PRIVATE
    Blox7::InputRuntime
    GTest::gtest_main
  )
  blox7_apply_compile_options(roblox_input_router_test)

  add_executable(roblox_text_editor_test
    ${BLOX7_INPUT_ROOT}/tests/roblox_text_editor_test.cc
  )
  target_link_libraries(roblox_text_editor_test PRIVATE
    Blox7::InputRuntime
    GTest::gtest_main
  )
  blox7_apply_compile_options(roblox_text_editor_test)

  add_executable(roblox_text_display_state_test
    ${BLOX7_INPUT_ROOT}/tests/roblox_text_display_state_test.cc
  )
  target_link_libraries(roblox_text_display_state_test PRIVATE
    Blox7::InputRuntime
    GTest::gtest_main
  )
  blox7_apply_compile_options(roblox_text_display_state_test)

  add_executable(roblox_text_font_resolver_test
    ${BLOX7_INPUT_ROOT}/tests/roblox_text_font_resolver_test.cc
  )
  target_link_libraries(roblox_text_font_resolver_test PRIVATE
    Blox7::InputRuntime
    GTest::gtest_main
  )
  blox7_apply_compile_options(roblox_text_font_resolver_test)

  add_executable(roblox_input_native_adapter_test
    ${BLOX7_INPUT_ROOT}/tests/roblox_input_native_adapter_test.cc
  )
  target_link_libraries(roblox_input_native_adapter_test PRIVATE
    Blox7::InputRuntime
    Blox7::LegacyJni
    GTest::gtest_main
  )
  blox7_apply_compile_options(roblox_input_native_adapter_test)

  add_executable(roblox_native_text_box_info_reader_test
    ${BLOX7_INPUT_ROOT}/tests/roblox_native_text_box_info_reader_test.cc
  )
  target_link_libraries(roblox_native_text_box_info_reader_test PRIVATE
    Blox7::InputRuntime
    Blox7::LegacyJni
    GTest::gtest_main
  )
  blox7_apply_compile_options(roblox_native_text_box_info_reader_test)

  include(GoogleTest)
  gtest_discover_tests(roblox_input_router_test)
  gtest_discover_tests(roblox_text_editor_test)
  gtest_discover_tests(roblox_text_display_state_test)
  gtest_discover_tests(roblox_text_font_resolver_test)
  gtest_discover_tests(roblox_input_native_adapter_test)
  gtest_discover_tests(roblox_native_text_box_info_reader_test)
endif()
