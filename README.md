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
| `libs/fh1data` | Format readers (RF-6): FH1 ZIP archives (stored and XMem LZX, method 21, with the `0x1123` payload-offset field and archives without local headers), `.xds` textures (Xenos fetch constant, untiling, endian swap, DXT1/3/5, DXN, DXT3A/5A and 8888, every mip level), `.carbin` car models (parts, vertex pools, 28-byte vertices, strips, sub-sections, UV transforms), Colorado render models `coloradoout.NNNNN.rmb.bin` (WORLD-GEOMETRY: header box, mesh objects, raw vertices with the position decoded, strip or list submeshes, 16- or 32-bit indices) |
| `libs/data` | Game database rules (SPEC-EVENT-RESTRICTIONS): decodes and re-encodes `EventRestrictions.Value` strings into per-bucket restriction sets, and tests a car against a set |
| `apps/fh1data-corpus` | Runs the readers over every archive under `media/` and reports what parses; `--names` and `--render-models` census an archive's directory and its render models |
| `apps/event-restrictions-corpus` | Decodes every `EventRestrictions` row of the player's `media/db/gamedb.slt` (opened read-only) and checks each round-trips; prints counts only |
| `apps/carview` | SDL3 + Vulkan 1.3 car viewer: dynamic rendering, one pipeline, depth, 4x MSAA, one texture, orbit camera (mouse or gamepad), and a headless mode that renders offscreen to a PNG; `--world` draws one Colorado sector headless at a game camera and compares it with a capture |
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

# Event restrictions: every row decodes and re-encodes to the same Value
# (--game-root, or the SHIFT_GAME_ROOT environment variable)
event-restrictions-corpus --game-root <game>

# Window
carview --game-root <game> --car AST_DB5Vantage_64

# Headless, offscreen, no window system: writes a PNG
carview --game-root <game> --car AST_DB5Vantage_64 --headless --out car.png --size 1280x720

# Options: --lod 0-5, --yaw/--pitch degrees, --distance m, --paint r,g,b,
# --msaa N, --no-cockpit, --all-parts, --validation (or SHIFT_VALIDATION=1)

# Render models: the archive's directory, then every model parsed
fh1data-corpus --names <game>/media/tracks/colorado/bin.zip [extracted-dir]
fh1data-corpus --render-models <game>/media/tracks/colorado/bin.zip

# One sector at a game camera (the frame file holds numbers measured from
# a game frame: keep it, and the outputs, out of the repository)
carview --world <bin.zip or extracted dir> --frame frame.txt --out-dir out \
        --capture frame.ppm [--nearest-lod --skip uberlod --skip terr_cube]
```

The frame file format and every option are described at the top of
`apps/carview/world_view.cpp`.

LOD 0 draws `<car>_lod0.carbin` plus `<car>_cockpit.carbin` (which holds
the hood, headlights, windscreen and cabin) with `nodamage_LOD0.xds`.
Upgrade variants (parts named `...race`) and the wheel (placed by the
car's physics data, which is not read yet) are hidden unless `--all-parts`.
Interior parts sample the exterior atlas for now, and glass is drawn
opaque.

## Status (RI-0.3, 2026-11-19)

- Corpus on the base disc (Windows and the Steam Deck give the same
  counts): 524 archives, 258,936 members; all 15,081 `.carbin`/`.xds`
  members decompress with matching CRCs. With `--all-members`, 258,521
  of the 258,537 stored and XMem members decompress with matching CRCs
  (231,270 of them have no local header); every XMem member needed a
  128 KiB LZX window. 16 fail in libmspack's lzxd (development JPGs in
  `UI.zip`, two `.pgeo`, four `.gr2`), cause unknown; the 399 deflated
  members are not read yet. 329 members lack extra field `0x1123`. All 1,883 car models of the 179
  `cars/` archives parse (118,629 sub-sections, no index outside its
  pool). All 9,656 `.xds` (all 2D) decode; for 9,653 the level layout
  accounts for the payload exactly, the other three are a linear texture
  (mips not laid out) and two narrow textures of 16 texels or less whose
  layout is unverified. DXN decodes to clean normal maps, but which block
  is X and which Y is not checked. Not decoded: the `stripped_*` twins, and the 1,372 livery
  decal and 2 brake `.carbin` files, which use another layout.
- carview draws a car headless on Windows (RTX 4080, validation layer
  clean, windowed path too) and on the Steam Deck LCD (RADV, Linux build
  from WSL Ubuntu 24.04, GCC 13, static libstdc++, X11 window support).

## Status (RI-0.4 world geometry, 2026-11-19)

- All 14,291 render models of Colorado's `bin.zip` parse to their last
  index list, read from the archive or from an extracted folder: 18,340
  mesh objects, 48,053 submeshes, 18.6 M vertices, no index outside its
  object, every vertex inside its model's box, 71.1 % of the bytes
  geometry. The archive reader sees all 230,057 directory entries (43,546
  names) although the end record's count wraps.
- `carview --world` draws the 277 world-space models of the spec's 1 km
  sector at the camera of a frame dump. The edges of the road-line
  strips, guard rails and rocks lie a median 1 px from the capture's
  edges (mean about 1-2 px); far silhouettes (mid-distance terrain) do
  not line up and are unverified. A matrix printed with four decimals
  is not enough for a pixel-exact check. Which LOD the game draws is not
  known yet, so drawing every model overlapping the sector puts far LODs
  over near ones.

## Licence

BSD-3-Clause (`LICENSE`), copyright Brandon G. Neri. **The licence is still
an open decision** of the reimplementation plan. The LGPL-2.1 libmspack
dependency bears on it; see `THIRD_PARTY_NOTICES.md`.
