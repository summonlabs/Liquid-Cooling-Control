cmake_minimum_required(VERSION 3.21)

project(LiquidCoolingControl
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral liquid-cooling control runtime"
        HOMEPAGE_URL "https://github.com/summonlabs/Liquid-Cooling-Control"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LCC_BUILD_TESTS "Build the Liquid Cooling Control test suite" ON)
option(LCC_BUILD_CLI "Build the lccctl administration and inspection CLI" ON)
option(LCC_BUILD_EXAMPLES "Build the Liquid Cooling Control examples" ON)
option(LCC_BUILD_BENCHMARKS "Build the Liquid Cooling Control benchmark" ON)
option(LCC_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(LCC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# First-party warning policy.  Applied to every target built from this tree.
# ---------------------------------------------------------------------------
function(lcc_apply_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /Zc:preprocessor
      /wd4996)
    if(LCC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
      -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion)
    if(LCC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(LCC_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(LCC_LIBRARY_SOURCES
  src/status.cpp
  src/units.cpp
  src/ids.cpp
  src/time.cpp
  src/model.cpp
  src/evidence.cpp
  src/authority.cpp
  src/policy.cpp
  src/transition.cpp
  src/adapter.cpp
  src/synthetic_adapter.cpp
  src/crc32c.cpp
  src/platform.cpp
  src/store_framing.cpp
  src/store.cpp
  src/store_io.cpp
  src/runtime.cpp
)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/liquidcooling/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/liquidcooling/version.hpp"
  @ONLY)

add_library(lcc STATIC ${LCC_LIBRARY_SOURCES})
add_library(LiquidCooling::lcc ALIAS lcc)

set_target_properties(lcc PROPERTIES
  OUTPUT_NAME liquidcooling
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(lcc
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(lcc PUBLIC cxx_std_20)
lcc_apply_strict_warnings(lcc)

# ---------------------------------------------------------------------------
# CLI, examples, benchmark
# ---------------------------------------------------------------------------
if(LCC_BUILD_CLI)
  add_executable(lccctl cli/lccctl.cpp)
  target_link_libraries(lccctl PRIVATE LiquidCooling::lcc)
  lcc_apply_strict_warnings(lccctl)
endif()

if(LCC_BUILD_EXAMPLES)
  add_executable(lcc_example_quickstart examples/quickstart.cpp)
  target_link_libraries(lcc_example_quickstart PRIVATE LiquidCooling::lcc)
  lcc_apply_strict_warnings(lcc_example_quickstart)

  add_executable(lcc_example_leak_isolation examples/leak_isolation.cpp)
  target_link_libraries(lcc_example_leak_isolation PRIVATE LiquidCooling::lcc)
  lcc_apply_strict_warnings(lcc_example_leak_isolation)
endif()

if(LCC_BUILD_BENCHMARKS)
  add_executable(lcc_bench bench/transition_bench.cpp)
  target_link_libraries(lcc_bench PRIVATE LiquidCooling::lcc)
  lcc_apply_strict_warnings(lcc_bench)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LCC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS lcc
  EXPORT LiquidCoolingControlTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/liquidcooling"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/liquidcooling/version.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/liquidcooling)

install(EXPORT LiquidCoolingControlTargets
  FILE LiquidCoolingControlTargets.cmake
  NAMESPACE LiquidCooling::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiquidCoolingControl)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LiquidCoolingControlConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/LiquidCoolingControlConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiquidCoolingControl)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LiquidCoolingControlConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/LiquidCoolingControlConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LiquidCoolingControlConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiquidCoolingControl)

message(STATUS "Liquid Cooling Control ${PROJECT_VERSION}: tests=${LCC_BUILD_TESTS} cli=${LCC_BUILD_CLI} examples=${LCC_BUILD_EXAMPLES} bench=${LCC_BUILD_BENCHMARKS} asan=${LCC_ENABLE_ASAN}")
