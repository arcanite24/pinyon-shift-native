cmake_minimum_required(VERSION 3.25)

project(shift_engine VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SHIFT_BUILD_CARVIEW "Build the carview app (SDL3 + Vulkan)" ON)
option(SHIFT_STATIC_RUNTIME "Link libstdc++/libgcc statically on Linux" ON)

if(MSVC)
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SHIFT_STATIC_RUNTIME)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

add_subdirectory(libs/fh1data)
add_subdirectory(libs/data)
add_subdirectory(libs/cars)
add_subdirectory(apps/fh1data-corpus)
add_subdirectory(apps/event-restrictions-corpus)
add_subdirectory(apps/car-corpus)
if(SHIFT_BUILD_CARVIEW)
  add_subdirectory(apps/carview)
endif()

enable_testing()
add_subdirectory(tests)
