# Third-party notices

shift-engine contains no third-party source in this repository. The build
fetches these projects with CMake FetchContent, pinned in
`cmake/Dependencies.cmake`, and links them statically:

| Project | Pin | Used for | Licence |
| --- | --- | --- | --- |
| [libmspack](https://github.com/kyz/libmspack) | `v1.11` (2023-02-24) | `lzxd.c` only: the LZX decoder behind ZIP method 21 (XMem) | **LGPL-2.1** |
| [SDL](https://github.com/libsdl-org/SDL) | `release-3.2.28` | window, input, Vulkan surface | zlib |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.4.350.1` | Vulkan API headers | Apache-2.0 / MIT |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.4.350.1` | loads the Vulkan loader at run time | MIT |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | `v3.3.0` | Vulkan memory sub-allocation (`vk_mem_alloc.h`) | MIT |
| [SQLite](https://www.sqlite.org) | `3.50.4` amalgamation (2025-07-30), SHA-256 pinned | reads the game database read-only (`event-restrictions-corpus`) | public domain |
| [stb](https://github.com/nothings/stb) | `f58f558c` (2025-05-26) | `stb_image_write.h`, PNG output | MIT or public domain |
| [dr_libs](https://github.com/mackron/dr_libs) | `mp3-0.7.4` (`aec2dd20`) | `dr_mp3.h`, the radio's MPEG decoding | public domain or MIT-0 |
| [FFmpeg](https://github.com/wmarti/FFmpeg) | `0604b464c7` (FFmpeg 4.4) | `libavutil` + `wmaprodec.c` (`xma2`/`wmapro`) only: in-process XMA2 audio decoding | **LGPL-2.1** |
| [libadrenotools](https://github.com/bylaws/libadrenotools) (with [liblinkernsbypass](https://github.com/bylaws/liblinkernsbypass)) | `8fae8ce254` (2024-09-10) | Android arm64 rootless custom Vulkan driver loading (Mesa Turnip) and Adreno GPU turbo clocks | **BSD-2-Clause** |
| [Mesa 3D / Freedreno Turnip](https://gitlab.freedesktop.org/mesa/mesa) (`assets/drivers/turnip/libvulkan_freedreno.so`, T30 `git-62ac221a33`) | `26.3.0-devel` (VK 1.4.359) | Bundled Android arm64 open-source Vulkan driver for Qualcomm Adreno 6xx/7xx GPUs | **MIT** |

## libmspack

libmspack is LGPL-2.1, and `fh1data` links its LZX decoder statically.
The project is GPL-3.0-or-later, which the LGPL permits, so a binary
built this way is distributed under the GPL with libmspack's own notice
kept.

The shaders under `apps/carview/shaders` are this project's own; the
SPIR-V in `shaders/prebuilt` is compiled from them.
