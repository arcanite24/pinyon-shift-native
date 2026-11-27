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

# SQLite, the official amalgamation, for reading the game database
# (media/db/gamedb.slt) read-only. Public domain.
# Checked 2026-11-19: the extracted sqlite3.c matches the SHA3-256 that
# sqlite.org's 3.50.4 release log publishes (9145255e...df36782).
FetchContent_Declare(sqlite3
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
  URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(sqlite3)
add_library(shift_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
target_include_directories(shift_sqlite3 PUBLIC "${sqlite3_SOURCE_DIR}")
target_compile_definitions(shift_sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DQS=0)
if(MSVC)
  target_compile_options(shift_sqlite3 PRIVATE /w)
else()
  target_compile_options(shift_sqlite3 PRIVATE -w)
endif()

# dr_mp3 (public domain / MIT-0), pinned to the mp3-0.7.4 release: the radio
# streams the disc's MPEG samples (SPEC-RADIO).
FetchContent_Declare(dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG aec2dd20667f54acfdb66031217df6d240f56c52
  SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(dr_libs)
add_library(shift_dr_libs INTERFACE)
target_include_directories(shift_dr_libs INTERFACE "${dr_libs_SOURCE_DIR}")

include(FFmpegXma2)

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
  if(ANDROID)
    target_compile_definitions(shift_volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
  endif()
  if(NOT WIN32)
    target_link_libraries(shift_volk PUBLIC ${CMAKE_DL_LIBS})
  endif()

  if(ANDROID AND CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    # libadrenotools (BSD-2-Clause): rootless Adreno custom Vulkan driver loader
    # (Mesa Turnip) and KGSL GPU turbo clocks on Android 9+ arm64.
    FetchContent_Declare(libadrenotools
      GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
      GIT_TAG 8fae8ce254dfc1344527e05301e43f37dea2df80
      GIT_SUBMODULES "lib/linkernsbypass"
      GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(libadrenotools)
  endif()

  # VulkanMemoryAllocator (MIT): sub-allocates device and staging memory.
  FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.3.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake_)
  FetchContent_MakeAvailable(vma)
  add_library(shift_vma INTERFACE)
  target_include_directories(shift_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")
  target_link_libraries(shift_vma INTERFACE shift_volk)
  target_compile_definitions(shift_vma INTERFACE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

  # SDL3, static, without the subsystems carview does not use.
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_AUDIO ON CACHE BOOL "" FORCE)
  set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
  set(SDL_RENDER OFF CACHE BOOL "" FORCE)
  set(SDL_GPU OFF CACHE BOOL "" FORCE)
  if(NOT ANDROID)
    set(SDL_DIALOG ON CACHE BOOL "" FORCE)
  else()
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
  endif()
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
