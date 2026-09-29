cmake_minimum_required(VERSION 3.21)

# Independent out-of-tree consumer of the installed Liquid Cooling Control
# package.  This project is deliberately not part of the library build: it proves
# that the installed package, not the build tree, is usable.

project(LiquidCoolingDownstreamConsumer
        VERSION 1.0.0
        DESCRIPTION "Out-of-tree consumer of the installed Liquid Cooling Control package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LiquidCoolingControl 1.0 REQUIRED CONFIG)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE LiquidCooling::lcc)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Werror)
endif()

message(STATUS "LiquidCoolingControl ${LiquidCoolingControl_VERSION} found at ${LiquidCoolingControl_DIR}")
