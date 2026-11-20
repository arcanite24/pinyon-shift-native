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
| `libs/fh1data` | Format readers (RF-6): FH1 ZIP archives (stored and XMem LZX, method 21, with the `0x1123` payload-offset field and archives without local headers), `.xds` textures (Xenos fetch constant, untiling, endian swap, DXT1/3/5, DXN, DXT3A/5A and 8888, every mip level), `.carbin` car models (parts, vertex pools, 28-byte vertices, strips, sub-sections, UV transforms), Colorado render models `coloradoout.NNNNN.rmb.bin` (WORLD-GEOMETRY: header box, mesh objects, raw vertices with the position decoded, strip or list submeshes, 16- or 32-bit indices), and the placements of its local-space models (WORLD-GEOMETRY: the object table in `Ribbon_00/Colorado_00.pvs`, kind-7 `.pgeo` tiles with model records, groups and 96-byte instances, `.pvsz` zone records, LOD sets) |
| `libs/data` | Game database rules (SPEC-EVENT-RESTRICTIONS): decodes and re-encodes `EventRestrictions.Value` strings into per-bucket restriction sets, and tests a car against a set |
| `apps/fh1data-corpus` | Runs the readers over every archive under `media/` and reports what parses; `--names` and `--render-models` census an archive's directory and its render models; `--props` the object table, `.pgeo` tiles and `.pvsz` zones |
| `apps/event-restrictions-corpus` | Decodes every `EventRestrictions` row of the player's `media/db/gamedb.slt` (opened read-only) and checks each round-trips; prints counts only |
| `libs/cars` | Cars (FORMAT-CARS): the install (disc, v4 title update, installed DLC packages in priority order), the merged car database (v4 `gamedb.slt` plus each package's patch databases), and the baked format `.shiftbake` with its writer and reader |
| `apps/car-corpus` | Reproduces every count of FORMAT-CARS's Tests section over the player's files (`--check` fails on a mismatch) |
| `apps/carbake` | Bakes every distinct car (240), its stock wheel, the tyre archives and the shared textures to `.shiftbake` files, deterministically and in parallel; `--dump` lists a bake |
| `apps/carview` | SDL3 + Vulkan 1.3 car viewer: dynamic rendering, depth, 4x MSAA, orbit camera (mouse or gamepad), and a headless mode that renders offscreen to a PNG. `--bake` is the showroom over baked cars (our car shader and lighting, paint, livery masks, wheels, brakes, glass last) with a reference mode that renders at a reference camera and writes the car-coverage mask; `--world` draws one Colorado sector headless at a game camera and compares it with a capture |
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

`<state>` is the folder that holds the installed packages' `user/` tree
(`user/0000000000000000/4D5309C9/00000002/<package>/Media/DLCZips`), and
`<dlc>` an optional folder of the packages' offer ZIPs already extracted
(`<package>/<offer stem>/media/...`); without it they are read in place.

```sh
# Cars: every count of FORMAT-CARS's Tests section (also run by ctest when
# SHIFT_GAME_ROOT and SHIFT_STATE_ROOT are set; SHIFT_DLC_EXTRACTED optional)
car-corpus --game-root <game> --state-root <state> [--dlc-extracted <dlc>] --check

# Bake every car to out/bake (keep it out of the repository); the time is
# written to out/bake/bake-time.txt
carbake --game-root <game> --state-root <state> [--dlc-extracted <dlc>] --out out/bake
carbake --dump out/bake/cars/fia_500abarthss_10.shiftbake

# Showroom over the bake: window, or headless with the coverage mask
carview --bake out/bake --car FIA_500AbarthSS_10 [--colour N] [--livery]
carview --bake out/bake --car FIA_500AbarthSS_10 --headless --out car.png --mask car_mask.png

# Reference mode: camera, size and paint from an AUTOSHOW_REFERENCE v1
# JSON; compared with the capture and mask named in it (or --against
# ref.png --against-mask ref_mask.png), printing IoU, masked PSNR and
# edge overlap
carview --bake out/bake --reference <refs>/1115.json --out 1115.png --mask 1115_mask.png

# The M1 report over every reference: report.txt with the plan's pass
# rule, and reference | engine | mask-difference PNGs (keep out/ local)
python tools/m1-report.py --carview build/apps/carview/carview.exe --bake out/bake --refs <refs> --out out/m1

# M3 drive (CAR_PHYSICS.md; carbake --physics-only writes the cars' rows,
# docs/M3_DRIVE_NEEDS.md lists the rest): headless trace, then chase-camera frames (--view front shows
# the wheels' steer and spin); without --headless it opens a window driven
# by keyboard or gamepad. --world <game>/media/tracks/colorado streams
# Colorado around the car: the collision tiles as the ground (COLLISION.md)
# and the render models as scenery, from the measured free-roam spawn by
# the festival; --run cruise drives itself on the road. drive --probe x y z
# lists the collision triangles near a point. In the window, streaming runs
# on a worker thread (headless: --async-stream).
# The model index is cached in out/world/colorado.index (first run ~10 s)
drive --bake out/bake --car FIA_500AbarthSS_10 --run launch --trace launch.csv
drive --bake out/bake --car FIA_500AbarthSS_10 --run launch --grade 0.07
drive --bake out/bake --car FIA_500AbarthSS_10 --run launch --physics-trace launch.jsonl --every 6
carview --drive --headless --bake out/bake --car FIA_500AbarthSS_10 --run slalom --every 0.5 --out out/drive/slalom.png
carview --drive --headless --bake out/bake --car FIA_500AbarthSS_10 --world <game>/media/tracks/colorado \
        --run cruise --seconds 120 --every 10 --out out/drive/colorado.png

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

# Prop placements: object table, kind-7 .pgeo tiles, .pvsz zones, and the
# placements whose translation lies in a box (x0 z0 x1 z1, file frame)
fh1data-corpus --props <game>/media/tracks/colorado/bin.zip \
        <game>/media/tracks/colorado/Ribbon_00/Colorado_00.pvs 1500 -1460 2500 -460

# One sector at a game camera (the frame file holds numbers measured from
# a game frame: keep it, and the outputs, out of the repository)
carview --world <bin.zip or extracted dir> --frame frame.txt --out-dir out \
        --capture frame.ppm [--nearest-lod --skip uberlod --skip terr_cube]

# The same with the sector's props placed through the object table
# (--members-root: where the .pgeo/.pvsz members and the props' models are
# read; --drawn-props keeps the placements a game frame drew)
carview --world <bin.zip or extracted dir> --frame frame.txt --out-dir out \
        --pvs <game>/media/tracks/colorado/Ribbon_00/Colorado_00.pvs \
        [--members-root <bin.zip or extracted dir>] [--drawn-props drawn.txt] \
        [--depth-skip chainlinkfence --depth-skip pine --depth-skip flag]
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


## Status (RI-0.4 props, 2026-11-19)

- The object table in `Ribbon_00/Colorado_00.pvs` reads (19,613 28-byte
  records, 173 shader names, 62,173 objects; 1,087 objects name 268
  member numbers that have no render model). All 2,031 kind-7 `.pgeo`
  tiles parse to their stated size (12,044 model records, 12,044 groups,
  174,803 instances) and all 1,434 `.pvsz` zones parse (1,027,743
  records, 59,599 distinct objects), read from `bin.zip` or from an
  extracted folder, in about 3 s.
- Zone records: 1,214 zones have more than one start from which the
  records end exactly at the end of the file. The first such start (the
  spec's rule) gives 1,570 records that disagree with the same object's
  record in another zone; taking the start with the most rigid rotations
  gives none. The reader uses the second rule.
- `carview --world --pvs` places every `.pgeo` instance and `.pvsz`
  object in the box. The spec's sector holds 2,460 `.pgeo` instances and
  847 `.pvsz` objects (2 name no render model). The 847 sit at 572
  spots: a zone often lists one object per LOD level of a prop, so a
  model resolved to one level is drawn once per spot (270 repeats left
  out). Of the 130 placements one game frame drew, 130 are found within
  5 cm (largest 0.7 mm). For each, the table names the drawn model, a
  model of its LOD set, or a model with a byte-identical mesh object.
- Zone rotations follow the spec's column reading: in the file frame
  R[r][c] = h[3c + r]·F[r] (the nine halves are R's columns with the
  third component negated). The earlier row reading turned the near
  metal barriers wrongly.
- With the 73 world-space models and the 130 drawn props at the frame's
  camera, the zone props' edges lie a median 1 px from the capture's
  edges (the `.pgeo` fence and pine 1.4 px): the
  metal barriers a mean 1.9 px (2.5 and 3.7 px when shifted by 20 px in
  x and y), the signs 1.7 px (4.8 and 3.9 px), the benches 1.4 px. The
  pine (leaf cards alpha-tested in the game, solid here) does not line
  up. The validation layer is clean.
- `--depth-skip <text>` writes a second depth image without the mesh
  objects whose names hold the text (`world-depth-skip.pfm`), for depth
  comparisons that leave out alpha-tested props (the chain-link fence,
  the pine) and animated ones (the flag) until materials exist.
- Which placements the game shows is open. The sector's `.pgeo` tiles
  include barrier sets, for example a line of concrete barriers along
  the road through the dumped camera, that the free-roam frame does not
  draw.

## Status (M1 car viewer, 2026-11-19)

- `car-corpus --check` reproduces every count of FORMAT-CARS's Tests
  section over the disc, v4 and the 21 installed packages (as of
  `f193075f`), including 250 `Data_Car` rows, 240 distinct media names,
  the stock rims' morph targets and header bounds, the wheel rows against
  the track rule and the stripe exceptions, in 10 s.
- `carbake` writes 480 files (7.85 GB) for all 240 cars. Gate run on
  multivac (16 threads, 2026-11-19, bake format 2): two bakes from empty
  folders took 19.1 s and 28.7 s (budget 10 min) and gave the same 482
  files (the bake files plus manifest and report; `bake-time.txt` left
  out), combined SHA-256 of the sorted per-file hashes
  `2c3b19d08d8409f4653c2fdb72cd21fd85dd17f5519273f8397ca5429a08afaa`.
- All 240 cars render headless at the default orbit camera with no load
  failures, no missing textures and none empty (168 s), checked by eye
  on one contact sheet.
- The showroom draws the LOD0 body (the `a` variant of each part set, no
  upgrade parts), the cockpit's hood, windscreen, headlights and cabin,
  rotors and callipers per corner, and the wheels by FORMAT-CARS's rule:
  centres from the track, tyre size and the rows' z, mapped to the model
  frame by the inverse model-to-body pose; rims sized by their morph
  target and scaled across by the header's design width; `TireC` sized
  by its two targets; right wheels turned half a turn. Paint is the
  lowest-Sequence colour where the atlas alpha is 0; on a striped colour
  row the alpha-255 texels take the stripe colour (the Abarth's side
  stripe and mirror caps). Glass never samples the body atlas. Lighting
  is our own: a procedural studio cube for diffuse and reflections, a key
  and a fill light, and the vertices' baked occlusion. Glass is blended
  last, far to near.
- 14 cars have no real LOD0 body and 6 have stand-in parts; they draw
  from the LOD1-5 model (Q6).
- Reference mode reads AUTOSHOW_REFERENCE v1 and v2, draws the mask by
  the 6/255 rule at the silhouette and the glass with the transparent
  pass's matrix. `tools/m1-report.py` scores v2 references only (their
  masks leave out the lamps' additive glow) and passes a car at IoU
  0.97; masked PSNR is reported only. The five v2 references so far all
  pass (IoU 0.976-0.997, mean 0.986); the other 25 are being recaptured
  as v2. On the 25 v1 captures the wheel fit gives IoU mean 0.990, the
  one miss there (`BEN_CONTINENTALGT_11`, 0.967) being lamp glow.
- The lighting's free settings (exposure, environment, key and fill
  strengths, the diffuse and reflection tints, metallic paint's share)
  were fitted to the references for median masked PSNR, keeping the
  diffuse light neutral; `--light name=value,...` overrides them. What
  is left is mostly the stage's sharp coloured reflections, which our
  studio environment does not have, and the white cars.
- Open in the spec: part variants (Q4), cockpit parts (Q5), calliper
  angle (Q7), slot-1 UVs (Q8). Polish for later: the showroom lighting
  (SHOWROOM_LIGHTING), the lamp glow and glare, Q13 and Q14.

## Licence

GPL-3.0-or-later (`LICENSE`), copyright Brandon G. Neri. The LGPL-2.1
libmspack dependency is compatible with it; see `THIRD_PARTY_NOTICES.md`.
