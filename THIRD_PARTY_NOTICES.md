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
| [SQLite](https://www.sqlite.org) | `3.50.4` amalgamation (2025-07-30), SHA-256 pinned | reads the game database read-only (`event-restrictions-corpus`) | public domain |
| [stb](https://github.com/nothings/stb) | `f58f558c` (2025-05-26) | `stb_image_write.h`, PNG output | MIT or public domain |

## libmspack and the licence decision

libmspack is LGPL-2.1, and `fh1data` links its LZX decoder statically.
Distributing binaries built that way carries LGPL obligations (the
library's source, and the means to relink against a modified version),
which a BSD-3-Clause project would not otherwise have. This matters for
the open licence decision (see README): before any binary release, either
keep libmspack and meet those terms (for example by building it as a
shared library), or replace it with an LZX decoder written for this
project from the public LZX format description.

The shaders under `apps/carview/shaders` are this project's own; the
SPIR-V in `shaders/prebuilt` is compiled from them.
