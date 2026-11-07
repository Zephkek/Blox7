# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

cmake_minimum_required(VERSION 3.20)
project(Blox7
  VERSION 1.0.5
  DESCRIPTION "Android Roblox compatibility runtime for Linux"
  LANGUAGES CXX
)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(BLOX7_BUILD_GIT_COMMIT "unknown")
set(BLOX7_BUILD_GIT_DIRTY "unknown")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE BLOX7_GIT_REV_PARSE_RESULT
    OUTPUT_VARIABLE BLOX7_GIT_REV_PARSE_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(BLOX7_GIT_REV_PARSE_RESULT EQUAL 0 AND
     BLOX7_GIT_REV_PARSE_OUTPUT MATCHES "^[0-9a-fA-F]+$")
    string(TOLOWER "${BLOX7_GIT_REV_PARSE_OUTPUT}"
      BLOX7_BUILD_GIT_COMMIT)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE BLOX7_GIT_STATUS_RESULT
      OUTPUT_VARIABLE BLOX7_GIT_STATUS_OUTPUT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(BLOX7_GIT_STATUS_RESULT EQUAL 0)
      if(BLOX7_GIT_STATUS_OUTPUT STREQUAL "")
        set(BLOX7_BUILD_GIT_DIRTY "false")
      else()
        set(BLOX7_BUILD_GIT_DIRTY "true")
      endif()
    endif()
  endif()
endif()

set(BLOX7_DEFAULT_COMPATIBILITY_MANIFEST
  "${CMAKE_SOURCE_DIR}/config/roblox_compatibility.json"
  CACHE FILEPATH
  "Fallback compatibility manifest used outside an installed runtime"
)
set(BLOX7_DEFAULT_SIGNING_TRUST_MANIFEST
  "${CMAKE_SOURCE_DIR}/config/roblox_signing_certificates.json"
  CACHE FILEPATH
  "Fallback signing trust manifest used outside an installed runtime"
)
set(BLOX7_DEFAULT_DOWNLOAD_SOURCES_MANIFEST
  "${CMAKE_SOURCE_DIR}/config/roblox_download_sources.json"
  CACHE FILEPATH "Pinned Roblox APK download sources"
)
set(BLOX7_DISCORD_APPLICATION_ID "1537088975720812655" CACHE STRING
  "Discord application ID baked into Blox7 Rich Presence releases"
)
string(LENGTH "${BLOX7_DISCORD_APPLICATION_ID}"
  BLOX7_DISCORD_APPLICATION_ID_LENGTH)
if(NOT BLOX7_DISCORD_APPLICATION_ID STREQUAL "" AND
   (NOT BLOX7_DISCORD_APPLICATION_ID MATCHES "^[0-9]+$" OR
    BLOX7_DISCORD_APPLICATION_ID_LENGTH LESS 17 OR
    BLOX7_DISCORD_APPLICATION_ID_LENGTH GREATER 20))
  message(FATAL_ERROR
    "BLOX7_DISCORD_APPLICATION_ID must be empty or a Discord snowflake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libjnivm uses C++ exceptions, so these cannot be directory-wide.
add_library(blox7_compile_options INTERFACE)
target_compile_options(blox7_compile_options INTERFACE
  "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter;-fno-exceptions;-g>"
)

function(blox7_apply_compile_options target_name)
  target_link_libraries(${target_name} PRIVATE blox7_compile_options)
endfunction()

option(BLOX7_ENABLE_UPSTREAM_JNIVM
  "Build upstream libjnivm for the incremental JNI runtime migration" ON)
if(BLOX7_ENABLE_UPSTREAM_JNIVM)
  set(JNIVM_ENABLE_TRACE OFF CACHE BOOL
    "Enable tracing inside upstream libjnivm")
  set(JNIVM_ENABLE_GC ON CACHE BOOL
    "Enable local/global reference tracking inside upstream libjnivm")
  set(JNIVM_ENABLE_DEBUG OFF CACHE BOOL
    "Enable upstream libjnivm class-dump/code-generation support")
  set(JNIVM_ENABLE_RETURN_NON_ZERO OFF CACHE BOOL
    "Allow upstream libjnivm to fabricate unresolved object return values")
  set(JNIVM_FAKE_JNI_MINECRAFT_LINUX_COMPAT OFF CACHE BOOL
    "Enable legacy fake-jni static method promotion")
  set(JNIVM_ENABLE_TESTS OFF CACHE BOOL
    "Build upstream libjnivm's own test suite")
  set(JNIVM_BUILD_EXAMPLES OFF CACHE BOOL
    "Build upstream libjnivm examples")

  add_subdirectory(third_party/libjnivm EXCLUDE_FROM_ALL)
  # Work around a missing include in libjnivm.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(jnivm PRIVATE -include string)
  endif()
  target_link_libraries(jnivm PUBLIC Threads::Threads)
  add_library(Blox7::UpstreamJni ALIAS jnivm)
endif()

option(BLOX7_WIN32 "Build for the Windows port (win/): Android ELF on the port's libc.so" OFF)
if(BLOX7_WIN32)
  # The port's loader (in its libc.so) provides mcpelauncher-linker's API natively.
  add_library(linker STATIC win/runtime/linker_win.cc)
  target_include_directories(linker PUBLIC third_party/mcpelauncher-linker/public_include)
  target_compile_definitions(linker PUBLIC BLOX7_WIN32=1)
else()
add_subdirectory(third_party/mcpelauncher-linker)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(linker PRIVATE -w)
endif()
endif()

set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL
  "Build Khronos Vulkan-Headers tests" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL
  "Install vendored Vulkan-Headers" FORCE)
add_subdirectory(third_party/Vulkan-Headers EXCLUDE_FROM_ALL)

include(CTest)
if(BUILD_TESTING)
  # Package builds disable tests and do not fetch Google Test.
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

find_package(OpenSSL REQUIRED)

include(${CMAKE_SOURCE_DIR}/cmake/Blox7PlatformGraphics.cmake)
foreach(foundation_target IN ITEMS
    blox7_platform_sdl
    blox7_graphics_foundation
    blox7_sdl_vulkan_wsi)
  blox7_apply_compile_options(${foundation_target})
endforeach()
foreach(graphics_test_target IN ITEMS
    bionic_egl_bridge_test
    platform_graphics_foundation_test)
  if(TARGET ${graphics_test_target})
    blox7_apply_compile_options(${graphics_test_target})
  endif()
endforeach()

include(${CMAKE_SOURCE_DIR}/cmake/Blox7Audio.cmake)
foreach(audio_target IN ITEMS
    blox7_audio_core
    blox7_audio_sdl
    blox7_audio_fmod_java_runtime
    blox7_fmod_jni_audio_bridge
    blox7_audio_opensl_adapter
    blox7_opensles)
  blox7_apply_compile_options(${audio_target})
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  # GCC 16 reports a false array-bounds warning in this translation unit.
  target_compile_options(blox7_audio_opensl_adapter PRIVATE
    -Wno-array-bounds
  )
endif()
if(TARGET audio_foundation_test)
  blox7_apply_compile_options(audio_foundation_test)
endif()
if(TARGET opensl_playback_runtime_test)
  blox7_apply_compile_options(opensl_playback_runtime_test)
endif()
if(TARGET fmod_java_audio_runtime_test)
  blox7_apply_compile_options(fmod_java_audio_runtime_test)
endif()
if(TARGET fmod_jni_audio_bridge_test)
  blox7_apply_compile_options(fmod_jni_audio_bridge_test)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/Blox7ElfCompatibility.cmake)

# Bionic stdio symbols must exist before the linker target.
add_library(blox7_bionic_stdio_runtime STATIC
  src/compat/bionic_stdio_runtime.cc
)
add_library(Blox7::BionicStdio ALIAS blox7_bionic_stdio_runtime)
target_include_directories(blox7_bionic_stdio_runtime PUBLIC include)
blox7_apply_compile_options(blox7_bionic_stdio_runtime)

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip)
pkg_check_modules(CAPSTONE REQUIRED IMPORTED_TARGET capstone)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET "libadwaita-1>=1.6")
pkg_check_modules(WEBKITGTK6 REQUIRED IMPORTED_TARGET webkitgtk-6.0)

add_library(blox7_update STATIC
  src/update/android_manifest.cc
  src/update/apk_bundle.cc
  src/update/apk_provider.cc
  src/update/apk_signature.cc
  src/update/apkpure_provider.cc
  src/update/candidate_approval.cc
  src/update/compatibility_catalog.cc
  src/update/github_releases_provider.cc
  src/update/http_download.cc
  src/update/host_abi_deriver.cc
  src/update/local_apk_provider.cc
  src/update/payload_integrity.cc
  src/update/payload_store.cc
  src/update/pinned_apk_provider.cc
  src/update/readiness_canary.cc
  src/update/state_file.cc
  src/update/unsafe_latest_runner.cc
  src/update/update_config.cc
  src/update/update_coordinator.cc
  src/update/zip_archive.cc
)
add_library(Blox7::Update ALIAS blox7_update)
target_include_directories(blox7_update PUBLIC include)
target_link_libraries(blox7_update PUBLIC
  Blox7::Compat
  CURL::libcurl
  OpenSSL::Crypto
  PkgConfig::LIBYAML
  PkgConfig::MINIZIP
  PkgConfig::CAPSTONE
  nlohmann_json::nlohmann_json
  Threads::Threads
)
blox7_apply_compile_options(blox7_update)

add_executable(blox7_updater src/update/main.cc)
target_link_libraries(blox7_updater PRIVATE Blox7::Update)
target_compile_definitions(blox7_updater PRIVATE
  "BLOX7_DEFAULT_COMPATIBILITY_MANIFEST=\"${BLOX7_DEFAULT_COMPATIBILITY_MANIFEST}\""
  "BLOX7_DEFAULT_SIGNING_TRUST_MANIFEST=\"${BLOX7_DEFAULT_SIGNING_TRUST_MANIFEST}\""
  "BLOX7_DEFAULT_DOWNLOAD_SOURCES_MANIFEST=\"${BLOX7_DEFAULT_DOWNLOAD_SOURCES_MANIFEST}\""
)
blox7_apply_compile_options(blox7_updater)

add_library(blox7_legacy_jnivm STATIC
  src/jnivm/jnivm.cc
  src/jnivm/jni_references.cc
  src/jnivm/jni_fields.cc
  src/jnivm/jni_strings.cc
)
add_library(Blox7::LegacyJni ALIAS blox7_legacy_jnivm)
target_include_directories(blox7_legacy_jnivm PUBLIC include)
blox7_apply_compile_options(blox7_legacy_jnivm)
# A JDK is not required for these ABI-only headers.
set(BLOX7_JNI_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/third_party/jni/include
)
target_include_directories(blox7_legacy_jnivm PUBLIC
  ${BLOX7_JNI_INCLUDE_DIRS}
)

add_library(blox7_runtime STATIC
  src/runtime/command_line.cc
  src/runtime/crash_report_policy.cc
  src/runtime/discord_rpc.cc
  src/runtime/device_memory_profile.cc
  src/runtime/device_profile.cc
  src/runtime/environment.cc
  src/runtime/external_launch_broker.cc
  src/runtime/frame_rate_policy.cc
  src/runtime/fleasion.cc
  src/runtime/game_mode.cc
  src/runtime/game_session_coordinator.cc
  src/runtime/graphics_launch_policy.cc
  src/runtime/http_client_policy.cc
  src/runtime/memory_limit.cc
  src/runtime/owned_pthread.cc
  src/runtime/performance_policy.cc
  src/runtime/platform_cache_migration.cc
  src/runtime/roblox_desktop_app_policy.cc
  src/runtime/roblox_app_lifecycle.cc
  src/runtime/roblox_browser_service_bridge.cc
  src/runtime/roblox_capability_resolver.cc
  src/runtime/roblox_experience_launch_bridge.cc
  src/runtime/roblox_launch_uri.cc
  src/runtime/roblox_presence_resolver.cc
  src/runtime/roblox_cookie_store.cc
  src/runtime/roblox_game_session_native_adapter.cc
  src/runtime/roblox_web_view_bridge.cc
  src/runtime/roblox_permissions_bridge.cc
  src/runtime/roblox_message_bus_request_bridge.cc
  src/runtime/roblox_call_protocol_bridge.cc
  src/runtime/roblox_account_bridge.cc
  src/runtime/payload_update_preflight.cc
  src/runtime/process_launch_policy.cc
  src/runtime/process_diagnostics.cc
  src/runtime/runtime_config.cc
  src/runtime/runtime_config_bootstrap.cc
  src/runtime/runtime_config_file.cc
  src/runtime/runtime_paths.cc
  src/runtime/session_log.cc
  src/runtime/single_instance_lock.cc
  src/runtime/startup_state_machine.cc
  src/runtime/supported_launch_policy.cc
  src/runtime/support_bundle.cc
  src/runtime/system_proxy.cc
  src/runtime/texture_memory_policy.cc
)
add_library(Blox7::Runtime ALIAS blox7_runtime)
target_include_directories(blox7_runtime PUBLIC include)
target_include_directories(blox7_runtime PUBLIC
  ${BLOX7_JNI_INCLUDE_DIRS}
)
target_compile_definitions(blox7_runtime PRIVATE
  "BLOX7_DISCORD_APPLICATION_ID=\"${BLOX7_DISCORD_APPLICATION_ID}\""
  "BLOX7_INSTALL_LIBDIR=\"${CMAKE_INSTALL_LIBDIR}\""
)
set_property(SOURCE src/runtime/session_log.cc APPEND PROPERTY
  COMPILE_DEFINITIONS
  "BLOX7_PROJECT_VERSION=\"${PROJECT_VERSION}\""
  "BLOX7_BUILD_GIT_COMMIT=\"${BLOX7_BUILD_GIT_COMMIT}\""
  "BLOX7_BUILD_GIT_DIRTY=\"${BLOX7_BUILD_GIT_DIRTY}\""
  "BLOX7_BUILD_TYPE=\"$<CONFIG>\""
  "BLOX7_BUILD_COMPILER_ID=\"${CMAKE_CXX_COMPILER_ID}\""
  "BLOX7_BUILD_COMPILER_VERSION=\"${CMAKE_CXX_COMPILER_VERSION}\""
  "BLOX7_BUILD_TARGET_SYSTEM=\"${CMAKE_SYSTEM_NAME}\""
  "BLOX7_BUILD_TARGET_PROCESSOR=\"${CMAKE_SYSTEM_PROCESSOR}\""
)
blox7_apply_compile_options(blox7_runtime)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  # GCC 16 reports a false array-bounds warning in nlohmann::json::dump().
  set_property(SOURCE src/runtime/roblox_launch_uri.cc APPEND PROPERTY
    COMPILE_OPTIONS -Wno-array-bounds
  )
endif()
target_link_libraries(blox7_runtime PUBLIC Threads::Threads)
target_link_libraries(blox7_runtime PRIVATE
  OpenSSL::Crypto
  CURL::libcurl
  nlohmann_json::nlohmann_json
  PkgConfig::GIO
  PkgConfig::LIBYAML
  ${CMAKE_DL_LIBS}
)

# Keep GTK out of the process that loads Android libraries.
add_library(blox7_failure_dialog_runtime STATIC
  src/runtime/failure_dialog.cc
)
add_library(Blox7::FailureDialog ALIAS blox7_failure_dialog_runtime)
target_include_directories(blox7_failure_dialog_runtime PUBLIC include)
target_compile_definitions(blox7_failure_dialog_runtime PRIVATE
  "BLOX7_INSTALL_LIBDIR=\"${CMAKE_INSTALL_LIBDIR}\""
)
blox7_apply_compile_options(blox7_failure_dialog_runtime)

add_executable(blox7_failure_dialog
  src/ui/blox7_failure_dialog.cc
)
target_link_libraries(blox7_failure_dialog PRIVATE PkgConfig::LIBADWAITA)
blox7_apply_compile_options(blox7_failure_dialog)

add_executable(blox7_cookie_helper src/runtime/blox7_cookie_helper.cc)
if(LIBSECRET_FOUND)
  target_link_libraries(blox7_cookie_helper PRIVATE PkgConfig::LIBSECRET)
  target_compile_definitions(blox7_cookie_helper PRIVATE BLOX7_HAVE_LIBSECRET=1)
else()
  message(WARNING "libsecret-1 headers missing: Roblox cookies will use private plaintext files")
endif()
blox7_apply_compile_options(blox7_cookie_helper)

add_library(blox7_services STATIC
  src/services/http_client.cc
  src/services/auth_service.cc
  src/services/browser_tracker_service.cc
  src/services/client_settings_service.cc
)
add_library(Blox7::Services ALIAS blox7_services)
target_include_directories(blox7_services PUBLIC include)
target_link_libraries(blox7_services PUBLIC
  blox7_runtime
  CURL::libcurl
  nlohmann_json::nlohmann_json
)
blox7_apply_compile_options(blox7_services)

add_library(blox7_webview_launcher STATIC
  src/runtime/webview_helper_launcher.cc
)
add_library(Blox7::WebViewLauncher ALIAS blox7_webview_launcher)
target_include_directories(blox7_webview_launcher PUBLIC include)
target_link_libraries(blox7_webview_launcher PUBLIC CURL::libcurl)
blox7_apply_compile_options(blox7_webview_launcher)

add_library(blox7_webview_helper_policy STATIC
  src/webview/webview_helper_policy.cc
)
add_library(Blox7::WebViewHelperPolicy ALIAS blox7_webview_helper_policy)
target_include_directories(blox7_webview_helper_policy PUBLIC src)
target_link_libraries(blox7_webview_helper_policy PUBLIC
  PkgConfig::WEBKITGTK6
  nlohmann_json::nlohmann_json
)
blox7_apply_compile_options(blox7_webview_helper_policy)

add_executable(blox7_webview_helper
  src/webview/blox7_webview_helper.cc
)
target_link_libraries(blox7_webview_helper PRIVATE
  Blox7::WebViewLauncher
  Blox7::WebViewHelperPolicy
  Blox7::Runtime
  PkgConfig::GTK4
  PkgConfig::WEBKITGTK6
)
blox7_apply_compile_options(blox7_webview_helper)

add_library(blox7_auth_runtime_composition STATIC
  src/runtime/auth_runtime_composition.cc
  src/runtime/webview_roblox_cookie.cc
)
add_library(Blox7::AuthRuntimeComposition ALIAS
  blox7_auth_runtime_composition)
target_include_directories(blox7_auth_runtime_composition PUBLIC include)
target_link_libraries(blox7_auth_runtime_composition PUBLIC
  blox7_legacy_jnivm
  blox7_runtime
  blox7_services
)
blox7_apply_compile_options(blox7_auth_runtime_composition)

add_library(blox7_linker STATIC
  src/linker/linker.cc
)
target_include_directories(blox7_linker PUBLIC include)
target_link_libraries(blox7_linker PUBLIC
  dl
  linker
  blox7_compat
  blox7_bionic_stdio_runtime
  blox7_bionic_abi_exports
)
blox7_apply_compile_options(blox7_linker)
target_compile_definitions(blox7_linker PUBLIC BLOX7_USE_BIONIC_LINKER=1)
# The Bionic linker expects IPLT boundaries that host executables do not have.
target_link_options(blox7_linker INTERFACE
  -Wl,--defsym=__rela_iplt_start=0
  -Wl,--defsym=__rela_iplt_end=0
)

add_library(libc_shim STATIC
  src/libc_shim/libc_shim.cc
)
target_include_directories(libc_shim PUBLIC include)
blox7_apply_compile_options(libc_shim)
target_link_libraries(blox7_linker PUBLIC libc_shim)

add_subdirectory(stubs)
foreach(stub_target IN ITEMS
    stub_sys_m stub_sys_z stub_sys_dl stub_sys_c
    stub_android stub_log stub_mediandk stub_openmax
    stub_egl stub_glesv2 stub_vulkan)
  blox7_apply_compile_options(${stub_target})
endforeach()

add_library(blox7_window STATIC
  src/window/roblox_fullscreen_menu_request_gate.cc
  src/window/video_driver_policy.cc
  src/window/window.cc
  src/window/window_creation_policy.cc
  src/window/window_fullscreen_request_gate.cc
  src/window/window_game_surface_bridge.cc
  src/window/window_pointer_capture_owner.cc
  src/window/window_resize_readiness_gate.cc
  src/window/window_state_store.cc
  src/window/window_surface_lifecycle.cc
  src/window/window_text_input_owner.cc
)
target_include_directories(blox7_window PUBLIC src)
blox7_apply_compile_options(blox7_window)
target_link_libraries(blox7_window PUBLIC
  Blox7::PlatformSdl
  Blox7::SdlVulkanWsi
  SDL3::SDL3
  ${CMAKE_DL_LIBS}
  PRIVATE Blox7::Runtime Blox7::GlesTextOverlay Blox7::GraphicsFoundation
)

add_library(blox7_roblox_output_device_runtime STATIC
  src/audio/roblox_output_device_bridge.cc
)
target_include_directories(blox7_roblox_output_device_runtime PUBLIC include)
target_include_directories(blox7_roblox_output_device_runtime PRIVATE src)
target_link_libraries(blox7_roblox_output_device_runtime PUBLIC
  blox7_compat
  Blox7::AudioSdl
  PRIVATE blox7_linker
)
blox7_apply_compile_options(blox7_roblox_output_device_runtime)

add_library(blox7_roblox_fullscreen_runtime STATIC
  src/runtime/roblox_fullscreen_runtime_bridge.cc
)
target_include_directories(blox7_roblox_fullscreen_runtime PUBLIC include)
target_include_directories(blox7_roblox_fullscreen_runtime PRIVATE src)
target_link_libraries(blox7_roblox_fullscreen_runtime PUBLIC
  blox7_compat
  PRIVATE
    blox7_linker
    blox7_window
    stub_log
)
blox7_apply_compile_options(blox7_roblox_fullscreen_runtime)

include(${CMAKE_SOURCE_DIR}/cmake/Blox7Input.cmake)

add_library(blox7_experience_runtime STATIC
  src/runtime/jnivm_platform_web_callbacks.cc
  src/runtime/roblox_experience_composition.cc
  src/runtime/roblox_fresh_game_launch_controller.cc
  src/runtime/roblox_platform_web_symbols.cc
)
add_library(Blox7::ExperienceRuntime ALIAS blox7_experience_runtime)
target_include_directories(blox7_experience_runtime PUBLIC include)
target_include_directories(blox7_experience_runtime PRIVATE src)
target_link_libraries(blox7_experience_runtime PUBLIC
  Blox7::Runtime
  Blox7::WebViewLauncher
  PkgConfig::UTF8PROC
  PRIVATE
    Blox7::AuthRuntimeComposition
    Blox7::LegacyJni
    blox7_linker
    blox7_window
)
blox7_apply_compile_options(blox7_experience_runtime)

# Compatibility exports still used by the old startup path.
add_library(blox7_bionic_abi_exports STATIC
  src/compat/bionic_abi_exports.cc
  src/compat/http_client_spin_guard.cc
)
target_include_directories(blox7_bionic_abi_exports
  PUBLIC include
  PRIVATE src
)
blox7_apply_compile_options(blox7_bionic_abi_exports)
# These exported Bionic FORTIFY symbols intentionally overlap glibc's
# __*_chk entry points.  When the bridge is visible to whole-program LTO,
# GCC can fold a fortified host call back into its own compatibility wrapper
# and turn the resulting tail recursion into an infinite loop.  Keep this
# translation unit opaque to interprocedural optimization and make its calls
# to the host C library explicit external calls.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_property(SOURCE src/compat/bionic_abi_exports.cc APPEND PROPERTY
    COMPILE_OPTIONS
      -fno-lto
      -fno-builtin
  )
endif()
target_link_libraries(blox7_bionic_abi_exports PRIVATE
  blox7_bionic_stdio_runtime
  blox7_window
  Threads::Threads
)

add_library(blox7_legacy_runtime STATIC
  src/legacy/legacy_runtime.cc
  src/runtime/roblox_text_input_jni_bridge.cc
)
target_include_directories(blox7_legacy_runtime PRIVATE include src)
blox7_apply_compile_options(blox7_legacy_runtime)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # Diagnostics from signal handlers cannot handle a failed write.
  set_property(SOURCE src/legacy/legacy_runtime.cc APPEND PROPERTY
    COMPILE_OPTIONS -Wno-unused-result
  )
endif()
target_link_libraries(blox7_legacy_runtime PRIVATE
  blox7_legacy_jnivm
  blox7_compat
  blox7_runtime
  blox7_input_runtime
  blox7_experience_runtime
  blox7_services
  blox7_linker
  blox7_bionic_abi_exports
  libc_shim
  blox7_window
  blox7_graphics_foundation
)
target_compile_definitions(blox7_legacy_runtime PRIVATE
  "BLOX7_DEFAULT_COMPATIBILITY_MANIFEST=\"${BLOX7_DEFAULT_COMPATIBILITY_MANIFEST}\""
)

add_executable(blox7 src/main.cc)
target_include_directories(blox7 PRIVATE include src)
blox7_apply_compile_options(blox7)
target_compile_definitions(blox7 PRIVATE
  "BLOX7_DEFAULT_COMPATIBILITY_MANIFEST=\"${BLOX7_DEFAULT_COMPATIBILITY_MANIFEST}\""
)
target_link_libraries(blox7 PRIVATE
  blox7_legacy_runtime
  blox7_auth_runtime_composition
  blox7_failure_dialog_runtime
  blox7_fmod_jni_audio_bridge
  blox7_roblox_output_device_runtime
  blox7_roblox_fullscreen_runtime
  blox7_window
  libc_shim
)

# Libraries loaded with dlopen() need the stub symbols.
target_link_options(blox7 PRIVATE
  -Wl,--export-dynamic
)
if(BLOX7_WIN32)
  # Windows port: the process C library is bionic itself. Blox7's stdio runtime for glibc hosts
  # (__sF) must not be exported over it. (bionic_abi_exports.cc leaves out its libc-named
  # definitions there; the rest, blox7_egl_display and the like, are what the stubs look up.)
  target_link_options(blox7 PRIVATE
    -Wl,--exclude-libs,libblox7_bionic_stdio_runtime.a
  )
  # the focused TextBox gets its own layer over the game window
  target_sources(blox7 PRIVATE win/runtime/text_layer_win.cc)
  target_link_libraries(blox7 PRIVATE Blox7::Update SDL3::SDL3)
endif()

# Native FreeBSD helper for Linuxulator's unsupported DF socket option.
# The helper is x86-64 assembly; other host architectures have nothing to
# assemble.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(BLOX7_BUILD_FREEBSD_SOCKET_HELPER OFF)
endif()
option(BLOX7_BUILD_FREEBSD_SOCKET_HELPER
  "Build the native FreeBSD Linuxulator socket helper" ON)
if(BLOX7_BUILD_FREEBSD_SOCKET_HELPER)
find_program(BLOX7_LLD_EXECUTABLE NAMES ld.lld REQUIRED)
find_program(BLOX7_ELFEDIT_EXECUTABLE NAMES elfedit REQUIRED)
get_filename_component(BLOX7_LLD_DIRECTORY
  "${BLOX7_LLD_EXECUTABLE}" DIRECTORY)
set(BLOX7_FREEBSD_SOCKET_HELPER
  "${CMAKE_BINARY_DIR}/blox7_freebsd_socket_helper")
add_custom_command(
  OUTPUT "${BLOX7_FREEBSD_SOCKET_HELPER}"
  COMMAND "${CMAKE_CXX_COMPILER}"
    -x assembler-with-cpp
    -nostdlib
    -static
    -no-pie
    "-B${BLOX7_LLD_DIRECTORY}/"
    -fuse-ld=lld
    -Wl,-e,_start,--build-id=none,-z,noexecstack
    -o "${BLOX7_FREEBSD_SOCKET_HELPER}"
    "${CMAKE_SOURCE_DIR}/src/compat/freebsd_socket_helper.S"
  COMMAND "${BLOX7_ELFEDIT_EXECUTABLE}"
    --output-osabi FreeBSD "${BLOX7_FREEBSD_SOCKET_HELPER}"
  DEPENDS "${CMAKE_SOURCE_DIR}/src/compat/freebsd_socket_helper.S"
  COMMENT "Building native FreeBSD Linuxulator socket helper"
  VERBATIM
)
add_custom_target(blox7_freebsd_socket_helper_build ALL
  DEPENDS "${BLOX7_FREEBSD_SOCKET_HELPER}"
)
add_dependencies(blox7 blox7_freebsd_socket_helper_build)
endif()

set_target_properties(blox7 PROPERTIES
  BUILD_RPATH   "\$ORIGIN"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/blox7"
)

set(BLOX7_INSTALL_RUNTIME_DIR "${CMAKE_INSTALL_LIBDIR}/blox7")
set(BLOX7_INSTALL_DATA_DIR "${CMAKE_INSTALL_DATADIR}/blox7")

install(TARGETS blox7
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(TARGETS
    blox7
    blox7_updater
    blox7_failure_dialog
    blox7_webview_helper
    blox7_cookie_helper
    blox7_audio_sdl
    blox7_opensles
    stub_sys_m
    stub_sys_z
    stub_sys_dl
    stub_sys_c
    stub_android
    stub_log
    stub_mediandk
    stub_openmax
    stub_egl
    stub_glesv2
    stub_vulkan
  RUNTIME DESTINATION "${BLOX7_INSTALL_RUNTIME_DIR}"
  LIBRARY DESTINATION "${BLOX7_INSTALL_RUNTIME_DIR}"
)
if(BLOX7_BUILD_FREEBSD_SOCKET_HELPER)
install(PROGRAMS "${BLOX7_FREEBSD_SOCKET_HELPER}"
  DESTINATION "${BLOX7_INSTALL_RUNTIME_DIR}"
)
endif()
install(FILES
    config/roblox_compatibility.json
    config/roblox_bootstrap_sources.json
    config/roblox_host_abi_reference.json
    config/roblox_signing_certificates.json
    config/roblox_download_sources.json
  DESTINATION "${BLOX7_INSTALL_DATA_DIR}/metadata"
)

if(BLOX7_WIN32)
  # Windows port: no GTK/WebKit helpers
  add_dependencies(blox7 stub_sys_m stub_sys_z stub_sys_dl stub_sys_c stub_android stub_log
    stub_mediandk blox7_opensles stub_openmax stub_egl stub_glesv2 stub_vulkan)
else()
add_dependencies(blox7 all_stubs)
add_dependencies(blox7 blox7_failure_dialog)
add_dependencies(blox7 blox7_webview_helper blox7_cookie_helper)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
