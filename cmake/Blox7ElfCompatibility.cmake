# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

include_guard(GLOBAL)

get_filename_component(BLOX7_ELF_COMPAT_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBELF REQUIRED IMPORTED_TARGET libelf)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(blox7_compat STATIC
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_atfork_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_dns_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_host_libc_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_large_file_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_prctl_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_rwlock_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_signal_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_socket_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_semaphore_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_pthread_create_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_pthread_key_runtime.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/bionic_sysconf.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/elf_build_id.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/build_profile.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/payload_compatibility.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/host_allocator_bridge.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/host_abi_experiment.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/host_abi_profile.cc
  ${BLOX7_ELF_COMPAT_ROOT}/src/compat/host_abi_profile_loader.cc
)
add_library(Blox7::Compat ALIAS blox7_compat)
target_include_directories(blox7_compat PUBLIC
  ${BLOX7_ELF_COMPAT_ROOT}/include
)
target_compile_definitions(blox7_compat PRIVATE
  "BLOX7_INSTALL_LIBDIR=\"${CMAKE_INSTALL_LIBDIR}\""
)
target_link_libraries(blox7_compat PUBLIC
  PkgConfig::LIBELF
  nlohmann_json::nlohmann_json
  Threads::Threads
)
target_link_libraries(blox7_compat PRIVATE OpenSSL::Crypto)
target_compile_features(blox7_compat PUBLIC cxx_std_17)

if(COMMAND blox7_apply_compile_options)
  blox7_apply_compile_options(blox7_compat)
endif()
