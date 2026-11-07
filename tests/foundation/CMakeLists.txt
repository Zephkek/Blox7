# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

cmake_minimum_required(VERSION 3.20)
project(Blox7PlatformGraphicsFoundation LANGUAGES CXX)

# compile smoke for the graphics foundation module on its own
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/Blox7PlatformGraphics.cmake)
