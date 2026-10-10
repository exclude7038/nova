cmake_minimum_required(VERSION 4.3.0)

project(nova)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NOVA_BUILD_TESTS "Build Nova tests" ON)
option(NOVA_USE_BUILD_CACHE "Use compiler cache" OFF)
option(NOVA_ENABLE_UNITY_BUILD "Enable CMake unity builds" ON)
option(NOVA_BUILD_BENCHMARKS "Build Nova benchmarks" OFF)

set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

enable_testing()
include(CTest)

find_package(Catch2 3 REQUIRED)

include_directories(${CMAKE_SOURCE_DIR})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ProjectOptions)
include(Cache)
include(Target)

nova_enable_build_cache()

add_subdirectory(core)
add_subdirectory(logging)
add_subdirectory(dicom)
add_subdirectory(concurrency)
add_subdirectory(di)
add_subdirectory(platform)
add_subdirectory(renderer)
add_subdirectory(math)
