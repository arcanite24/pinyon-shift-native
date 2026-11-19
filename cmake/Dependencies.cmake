# Third-party code, fetched at configure time and pinned to exact tags or
# commits. Nothing here is vendored; THIRD_PARTY_NOTICES.md lists licences.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# libmspack: the LZX decoder (lzxd.c) only. LGPL-2.1.
FetchContent_Declare(libmspack
  GIT_REPOSITORY https://github.com/kyz/libmspack.git
  GIT_TAG v1.11
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(libmspack)
set(SHIFT_MSPACK_DIR "${libmspack_SOURCE_DIR}/libmspack/mspack")
add_library(shift_mspack_lzxd STATIC "${SHIFT_MSPACK_DIR}/lzxd.c")
target_include_directories(shift_mspack_lzxd PUBLIC "${SHIFT_MSPACK_DIR}")
if(MSVC)
  target_compile_options(shift_mspack_lzxd PRIVATE /w)
else()
  target_compile_options(shift_mspack_lzxd PRIVATE -w)
endif()
set_target_properties(shift_mspack_lzxd PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(SHIFT_BUILD_CARVIEW)
  # Vulkan headers and the volk loader: no Vulkan SDK needed to build.
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.350.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake_)
  FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.350.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake_)
  FetchContent_MakeAvailable(vulkan_headers volk)
  add_library(shift_volk STATIC "${volk_SOURCE_DIR}/volk.c")
  target_include_directories(shift_volk PUBLIC
    "${volk_SOURCE_DIR}" "${vulkan_headers_SOURCE_DIR}/include")
  target_compile_definitions(shift_volk PUBLIC VK_NO_PROTOTYPES)
  if(NOT WIN32)
    target_link_libraries(shift_volk PUBLIC ${CMAKE_DL_LIBS})
  endif()

  # SDL3, static, without the subsystems carview does not use.
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
  set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
  set(SDL_RENDER OFF CACHE BOOL "" FORCE)
  set(SDL_GPU OFF CACHE BOOL "" FORCE)
  set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.28
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL3)

  # stb_image_write (public domain / MIT), pinned to a commit.
  FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG f58f558c120e9b32c217290b80bad1a0729fbb2c
    SOURCE_SUBDIR _no_cmake_)
  FetchContent_MakeAvailable(stb)
  add_library(shift_stb INTERFACE)
  target_include_directories(shift_stb INTERFACE "${stb_SOURCE_DIR}")
endif()
