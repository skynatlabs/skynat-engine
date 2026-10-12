# skynat-engine — headless live engine built from OBS Studio's libobs.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Configure against a libobs build tree (see scripts/build-macos.sh and
# scripts/build-windows.ps1):
#   cmake -S . -B build -Dlibobs_DIR=<obs-studio>/build_<platform>/libobs
cmake_minimum_required(VERSION 3.22)

project(skynat-engine VERSION 0.2.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(libobs REQUIRED)

add_executable(
  skynat-engine
  src/main.cpp
  src/engine.cpp
  src/engine.hpp
  src/io.cpp
  src/io.hpp
  src/json.hpp
  src/output.cpp
  src/output.hpp
  src/platform.hpp
  src/projectors.cpp
  src/projectors.hpp
  src/protocol.hpp
  src/sources.cpp
  src/sources.hpp
  src/thumbnails.cpp
  src/thumbnails.hpp
)

target_link_libraries(skynat-engine PRIVATE OBS::libobs)

if(APPLE)
  target_sources(skynat-engine PRIVATE src/platform-macos.mm)
  set_source_files_properties(src/platform-macos.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(
    skynat-engine
    PRIVATE
      "-framework AppKit"
      "-framework Foundation"
      "-framework CoreFoundation"
      "-framework AVFoundation"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework VideoToolbox"
  )
  # dist layout: bin/skynat-engine next to Frameworks/ (libobs, renderer,
  # FFmpeg ...) and PlugIns/ — the same relative path libobs and its plugins
  # already carry (@executable_path/../Frameworks), so nothing is patched.
  set_target_properties(
    skynat-engine
    PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path/../Frameworks"
  )
  target_compile_options(skynat-engine PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
elseif(WIN32)
  target_sources(skynat-engine PRIVATE src/platform-windows.cpp)
  target_compile_definitions(skynat-engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(skynat-engine PRIVATE psapi user32 gdi32)
  # A console-subsystem program: Skynat Live spawns it with windowsHide, so
  # no console window ever appears.
endif()
