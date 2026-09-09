# ~~~
# slang-blocks -- block-diagram model extractor built against an installed slang.
#
# This is an out-of-tree tool: slang itself is unmodified and consumed as a
# CMake package, so there is no fork to rebase.  Point it at the slang built by
# Makefile.eda:
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<repo>/eda/slang
#   cmake --build build -j
#   cmake --install build
#
# SPDX-License-Identifier: MIT
# ~~~

cmake_minimum_required(VERSION 3.20)

project(
  slang-blocks
  VERSION 0.1.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "" FORCE)
endif()

find_package(slang REQUIRED)
find_package(fmt QUIET)

add_executable(slang_blocks src/main.cpp)
set_target_properties(slang_blocks PROPERTIES OUTPUT_NAME "slang-blocks")
target_link_libraries(slang_blocks PRIVATE slang::slang)

if(fmt_FOUND)
  target_link_libraries(slang_blocks PRIVATE fmt::fmt)
endif()

include(GNUInstallDirs)
install(TARGETS slang_blocks RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
