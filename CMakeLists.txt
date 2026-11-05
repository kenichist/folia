cmake_minimum_required(VERSION 3.21)
# Must precede project(). Homebrew's Qt 6.12 frameworks are built with
# minos 14.4 (SDK 26.5), so that is the effective minimum macOS for this build.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.4" CACHE STRING "Minimum macOS")
project(Folia VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

# Portable baseline: C++17, Qt 6 Widgets (Core/Gui/Widgets only), embedded Lua, SQLite.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(FOLIA_WITH_MATH "Compile the native math renderer" ON)
option(FOLIA_BUILD_TESTS "Build unit/integration tests (links Qt6::Test)" ON)
option(FOLIA_BUILD_TOOLS "Build fixture generator and measurement tools" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
if(FOLIA_BUILD_TESTS)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
  enable_testing()
endif()

set(CMAKE_AUTOMOC ON)

add_subdirectory(third_party)
add_subdirectory(src/code)
if(FOLIA_WITH_MATH)
  add_subdirectory(src/math)
endif()
add_subdirectory(src)
if(FOLIA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(FOLIA_BUILD_TESTS)
  add_subdirectory(tests)
endif()
