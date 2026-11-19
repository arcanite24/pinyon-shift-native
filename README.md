# shift-engine

*shift-engine is a provisional working name.*

A native engine for a clean-room reimplementation of Forza Horizon (2012,
Xbox 360). This is probe RI-0.3 of the Pinyon Shift reimplementation plan
(`docs/REIMPLEMENTATION_PLAN.md` in horizon1-recomp): the first engine
skeleton, with an SDL3 window, a Vulkan 1.3 device, the RF-6 format
readers, and a car drawn from the game's own files.

## What it reads, and what it ships

- It reads only the player's own copy of the game: the extracted disc
  (`media/` and the archives under it). Nothing is downloaded and nothing
  is written back to the game files.
- The repository and its binaries contain **no game data**: no models,
  textures, shaders, tables or bytes taken from the game. Renders and
  corpus reports are made from the player's files, so they stay local
  (`.gitignore` excludes `*.png`). Tests use synthetic bytes only.
- **Clean-room rule.** This repository is the implementation side of the
  RF-8 split. Code here is written from specifications, format notes and
  the oracle's output (the findings records and tools of horizon1-recomp,
  written from our own analysis). It is never written from disassembly,
  decompiler output or the recompilation's generated code, and none of
  those is consulted while working on it. Each commit names the
  specifications it implements.

## Layout

| Path | What |
| --- | --- |
| `libs/fh1data` | Format readers (RF-6): FH1 ZIP archives (stored and XMem LZX, method 21, with the `0x1123` payload-offset field and archives without local headers), `.xds` textures (Xenos fetch constant, untiling, endian swap, DXT1/3/5, DXN, DXT3A/5A, 8888 and some 16-bit formats, every mip level), `.carbin` car models (parts, vertex pools, 28-byte vertices, strips, sub-sections, UV transforms) |
| `apps/fh1data-corpus` | Runs the readers over every archive under `media/` and reports what parses |
| `apps/carview` | SDL3 + Vulkan 1.3 car viewer: dynamic rendering, one pipeline, depth, 4x MSAA, one texture, orbit camera (mouse or gamepad), and a headless mode that renders offscreen to a PNG |
| `tests` | Unit tests on synthetic bytes |
| `tools` | Windows build script, shader regeneration |

## Building

Requirements: CMake 3.25+, Ninja, a C++20 compiler (MSVC 2022 on Windows;
GCC 13 or Clang 17+ on Linux), Git (FetchContent pulls the dependencies
listed in `THIRD_PARTY_NOTICES.md`). No Vulkan SDK is needed: the headers
are fetched and the loader is opened at run time.

Windows (from any PowerShell; the script enters the Visual Studio
environment itself):

```powershell
.\tools\build-windows.ps1            # -> build\apps\carview\carview.exe
```

Linux (SDL3 picks up X11/Wayland development headers when present; without
them the window path is limited, the headless path still works):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Shaders: the build compiles `apps/carview/shaders/*.vert|frag` when it
finds `glslangValidator` or `glslc` (on `PATH`, in `$VULKAN_SDK`, or given
as `-DSHIFT_GLSL_COMPILER=...`). Otherwise it embeds the committed SPIR-V in
`apps/carview/shaders/prebuilt`. After changing a shader, refresh that copy
with `python tools/regenerate-shaders.py [--compiler path]`.

## Running

`<game>` is the folder that holds the extracted disc's `media/` (or the
`media` folder itself). Car names are the archive names under
`media/cars`, e.g. `AST_DB5Vantage_64`.

```sh
# Corpus test: every archive, every .carbin and .xds member
fh1data-corpus --game-root <game> [--verbose] [--filter cars/]

# Window
carview --game-root <game> --car AST_DB5Vantage_64

# Headless, offscreen, no window system: writes a PNG
carview --game-root <game> --car AST_DB5Vantage_64 --headless --out car.png --size 1280x720

# Options: --lod 0-5, --yaw/--pitch degrees, --distance m, --paint r,g,b,
# --msaa N, --no-cockpit, --all-parts, --validation (or SHIFT_VALIDATION=1)
```

LOD 0 draws `<car>_lod0.carbin` plus `<car>_cockpit.carbin` (which holds
the hood, headlights, windscreen and cabin) with `nodamage_LOD0.xds`.
Upgrade variants (parts named `...race`) and the wheel (placed by the
car's physics data, which is not read yet) are hidden unless `--all-parts`.
Interior parts sample the exterior atlas for now, and glass is drawn
opaque.

## Licence

BSD-3-Clause (`LICENSE`), copyright Brandon G. Neri. **The licence is still
an open decision** of the reimplementation plan. The LGPL-2.1 libmspack
dependency bears on it; see `THIRD_PARTY_NOTICES.md`.
