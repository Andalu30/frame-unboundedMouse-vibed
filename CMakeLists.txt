# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
# The driver's version. It is logged at startup ("mouselaser: version ..."), so bump it on every
# behaviour change to tell builds apart on the headset.
project(mouselaser VERSION 0.7.0 LANGUAGES CXX)
set(MOUSELASER_VERSION "${PROJECT_VERSION}-experimental")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # lets clangd find openvr_driver.h via build/compile_commands.json
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Header-only OpenVR driver API, as shipped with SteamVR's hellovr sample.
set(OPENVR_HEADERS /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
    CACHE PATH "Directory containing openvr_driver.h")

# Warnings for our code, for the driver and the tests alike. CI builds with -DMOUSELASER_WERROR=ON.
option(MOUSELASER_WERROR "Treat warnings as errors" OFF)
set(MOUSELASER_WARNINGS -Wall -Wextra -Wshadow -Wconversion -Wold-style-cast -Woverloaded-virtual -Wformat=2
    -Wnull-dereference)
if(MOUSELASER_WERROR)
  list(APPEND MOUSELASER_WARNINGS -Werror)
endif()

# SteamVR loads a driver from bin/<platform>: linuxarm64 on the Frame, linux64 on an x86_64 PC.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(MOUSELASER_PLATFORM linuxarm64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set(MOUSELASER_PLATFORM linux64)
else()
  set(MOUSELASER_PLATFORM linux-${CMAKE_SYSTEM_PROCESSOR})
  message(WARNING "SteamVR has no driver folder for ${CMAKE_SYSTEM_PROCESSOR}; using bin/${MOUSELASER_PLATFORM}")
endif()

add_library(driver_mouselaser SHARED src/driver.cpp)
# SYSTEM: warnings from inside Valve's header are not ours to fix, so the compiler skips them.
target_include_directories(driver_mouselaser SYSTEM PRIVATE ${OPENVR_HEADERS})
target_compile_options(driver_mouselaser PRIVATE ${MOUSELASER_WARNINGS} -fvisibility=hidden)
target_compile_definitions(driver_mouselaser PRIVATE MOUSELASER_VERSION="${MOUSELASER_VERSION}")
target_link_libraries(driver_mouselaser PRIVATE pthread)
set_target_properties(driver_mouselaser PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/driver/mouselaser/bin/${MOUSELASER_PLATFORM})

# Tests: unit tests of src/core.h plus a check of the resource files. They don't need SteamVR,
# so they run on any Linux machine. See docs/testing.md.
#   cmake -S . -B build -DMOUSELASER_TESTS=ON && cmake --build build --target unit_tests wheel_sim
#   ctest --test-dir build --output-on-failure
option(MOUSELASER_TESTS "Build the unit tests and the wheel_sim tool" OFF)
set(MOUSELASER_SANITIZE "" CACHE STRING "Sanitizers for the tests: address,undefined or thread (empty: none)")
if(MOUSELASER_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  # doctest: a single-header test framework, used by the tests only (the driver itself has no
  # dependencies). Uses an installed copy if there is one, otherwise downloads a pinned release
  # and checks its SHA-256.
  find_path(DOCTEST_INCLUDE_DIR doctest/doctest.h DOC "Directory containing doctest/doctest.h")
  if(DOCTEST_INCLUDE_DIR)
    set(doctest_dir ${DOCTEST_INCLUDE_DIR})
  else()
    set(doctest_dir ${CMAKE_BINARY_DIR}/_deps/doctest)
    set(doctest_h ${doctest_dir}/doctest/doctest.h)
    set(doctest_sha256 cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421)
    if(NOT EXISTS ${doctest_h})
      message(STATUS "Downloading doctest 2.5.3 (test-only dependency)")
      file(DOWNLOAD https://raw.githubusercontent.com/doctest/doctest/v2.5.3/doctest/doctest.h
           ${doctest_h}.part STATUS status TLS_VERIFY ON)
      list(GET status 0 status_code)
      if(status_code EQUAL 0)
        file(SHA256 ${doctest_h}.part got_sha256)
      endif()
      if(NOT status_code EQUAL 0 OR NOT got_sha256 STREQUAL doctest_sha256)
        file(REMOVE ${doctest_h}.part)
        message(FATAL_ERROR "Couldn't download doctest (${status}). "
                            "Install it, or point -DDOCTEST_INCLUDE_DIR at a directory containing doctest/doctest.h.")
      endif()
      file(RENAME ${doctest_h}.part ${doctest_h})
    endif()
  endif()

  add_executable(unit_tests tests/unit_tests.cpp tests/doctest_main.cpp)
  add_executable(wheel_sim tools/wheel_sim.cpp) # prints wheel timelines, for tuning; not a test
  foreach(target unit_tests wheel_sim)
    target_include_directories(${target} PRIVATE src)
    target_include_directories(${target} SYSTEM PRIVATE ${doctest_dir}) # no warnings from its header
    target_compile_options(${target} PRIVATE ${MOUSELASER_WARNINGS})
    target_compile_definitions(${target} PRIVATE MOUSELASER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(MOUSELASER_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=${MOUSELASER_SANITIZE} -fno-sanitize-recover=all
                                               -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=${MOUSELASER_SANITIZE})
    endif()
  endforeach()

  add_test(NAME unit_tests COMMAND unit_tests)
  add_test(NAME resources COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_resources.py
                                  ${CMAKE_SOURCE_DIR})
endif()
