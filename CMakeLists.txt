# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
project(mouselaser CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # lets clangd find openvr_driver.h via build/compile_commands.json
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Header-only OpenVR driver API, as shipped with SteamVR's hellovr sample.
set(OPENVR_HEADERS /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
    CACHE PATH "Directory containing openvr_driver.h")

add_library(driver_mouselaser SHARED src/driver.cpp)
target_include_directories(driver_mouselaser PRIVATE ${OPENVR_HEADERS})
target_compile_options(driver_mouselaser PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_libraries(driver_mouselaser PRIVATE pthread)
set_target_properties(driver_mouselaser PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/driver/mouselaser/bin/linuxarm64)

# Optional offline check of the wheel logic: cmake -DMOUSELASER_TESTS=ON ... && ./build/wheel_sim
option(MOUSELASER_TESTS "Build the offline wheel simulation" OFF)
if(MOUSELASER_TESTS)
  add_executable(wheel_sim tests/wheel_sim.cpp)
  target_include_directories(wheel_sim PRIVATE ${OPENVR_HEADERS})
  target_link_libraries(wheel_sim PRIVATE pthread)
endif()
