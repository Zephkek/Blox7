# minizip (zlib contrib) and our read-only libelf, deps.py builds these for android
cmake_minimum_required(VERSION 3.20)
project(Blox7WinExtraDeps C)

set(ZLIB_SOURCE_DIR "" CACHE PATH "zlib source tree")
find_package(ZLIB REQUIRED)

set(MINIZIP_DIR "${ZLIB_SOURCE_DIR}/contrib/minizip")
add_library(minizip STATIC
  ${MINIZIP_DIR}/ioapi.c
  ${MINIZIP_DIR}/mztools.c
  ${MINIZIP_DIR}/unzip.c
  ${MINIZIP_DIR}/zip.c
)
target_include_directories(minizip PUBLIC ${MINIZIP_DIR})
target_compile_definitions(minizip PRIVATE NOCRYPT=0)
target_link_libraries(minizip PUBLIC ZLIB::ZLIB)
install(TARGETS minizip ARCHIVE DESTINATION lib)
install(FILES
    ${MINIZIP_DIR}/crypt.h ${MINIZIP_DIR}/ioapi.h ${MINIZIP_DIR}/mztools.h ${MINIZIP_DIR}/unzip.h ${MINIZIP_DIR}/zip.h
    ${MINIZIP_DIR}/ints.h ${MINIZIP_DIR}/skipset.h
  DESTINATION include/minizip)

add_library(elf STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../../minilibelf/libelf.c)
target_include_directories(elf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../minilibelf)
install(TARGETS elf ARCHIVE DESTINATION lib)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../minilibelf/libelf.h ${CMAKE_CURRENT_SOURCE_DIR}/../../minilibelf/gelf.h
  DESTINATION include)
