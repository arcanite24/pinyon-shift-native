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
| `libs/fh1data` | Format readers (RF-6): FH1 ZIP archives (stored and XMem LZX, method 21, with the `0x1123` payload-offset field and archives without local headers), `.xds` textures (Xenos fetch constant, untiling, endian swap, DXT1/3/5, DXN, DXT3A/5A and 8888, every mip level), `.carbin` car models (parts, vertex pools, 28-byte vertices, strips, sub-sections, UV transforms), Colorado render models `coloradoout.NNNNN.rmb.bin` (WORLD-GEOMETRY: header box, mesh objects, raw vertices with the position decoded, strip or list submeshes, 16- or 32-bit indices), and the placements of its local-space models (WORLD-GEOMETRY: the object table in `Ribbon_00/Colorado_00.pvs`, kind-7 `.pgeo` tiles with model records, groups and 96-byte instances, `.pvsz` zone records, LOD sets), and Colorado's materials and textures (WORLD-MATERIALS: material records, `uvOffsetScale`, vertex layouts per effect, the `.pvs` texture records and per-model lists, `.bix` pairs and CAFF textures with the trimmed last region) |
| `libs/data` | Game database rules (SPEC-EVENT-RESTRICTIONS): decodes and re-encodes `EventRestrictions.Value` strings into per-bucket restriction sets, and tests a car against a set |
| `apps/fh1data-corpus` | Runs the readers over every archive under `media/` and reports what parses; `--names` and `--render-models` census an archive's directory and its render models; `--props` the object table, `.pgeo` tiles and `.pvsz` zones; `--world-materials` the materials, UV scales, vertex layouts and every world texture against WORLD_MATERIALS.md's counts |
| `apps/event-restrictions-corpus` | Decodes every `EventRestrictions` row of the player's `media/db/gamedb.slt` (opened read-only) and checks each round-trips; prints counts only |
| `libs/cars` | Cars (FORMAT-CARS): the install (disc, v4 title update, installed DLC packages in priority order), the merged car database (v4 `gamedb.slt` plus each package's patch databases), and the baked format `.shiftbake` with its writer and reader |
| `apps/car-corpus` | Reproduces every count of FORMAT-CARS's Tests section over the player's files (`--check` fails on a mismatch) |
| `apps/carbake` | Bakes every distinct car (240), its stock wheel, the tyre archives and the shared textures to `.shiftbake` files, deterministically and in parallel; `--dump` lists a bake |
| `libs/audio` | Engine sound (SPEC-ENGINE-SOUND): the tuning XMLs (engine, harmonics, car model listener), FSB4 bank headers and XMA2 wave wrapping, the rpm map, crossfades, pitch and parameter curves, the loop index rule, a stereo mixer, and the bake to PCM with its reader |
| `apps/soundbake` | Decodes each car's engine loops to 16-bit PCM under `out/sound/<car>/` with copies of its tuning files (FFmpeg, bake time only) |
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

## Drive it yourself (M3 play test)

On multivac, from the repository root, after a build and a bake (`out/bake1`
holds all 240 cars and their physics rows):

```powershell
.\build\apps\carview\carview.exe
```

With no arguments it drives the R34 on Colorado in a 1600 x 900 window,
from the bake in `out\bake` (or `out\bake1`) and the game under
`SHIFT_GAME_ROOT` (or the sibling `..\horizon1-recomp\.local\game\base`);
the console prints what it picked. The same with every argument given:

```powershell
.\build\apps\carview\carview.exe --drive --bake out\bake1 --car NIS_SkylineGTRVSpecII_02 --world ..\horizon1-recomp\.local\game\base\media\tracks\colorado --size 1600x900
```

A window opens on Colorado at the measured free-roam spawn by the
festival, in the R34 on CAR_PHYSICS.md's model: automatic gears, ABS on,
traction control off. The scenery and the collision stream in the
background as you drive. The first run builds `out\world\colorado.index`
(about 10 s before the window opens). Any car of the bake works with
`--car` (its media name, e.g. `FIA_500AbarthSS_10`).

The scenery is drawn with its own textures (WORLD_MATERIALS.md's first
pass: each material's main texture lit by the sun and sky of the
reference frame; terrain blends its Blend_A and Blend_B layers by
vertex colour or Splat, and the forest-covered hills are brightened to
the reference). Beyond 400 m the far ring
draws the terrain's MIDDIST and UberLOD objects within 3 km (tree-covered
slopes, the snowy peaks) under a sky gradient, and the tree sprites of
the kind-7 tiles (WORLD_MATERIALS.md's tree billboards) stand within
2 km, each turned to the camera. Textures decode and are
staged on the streaming worker and copied with the scenery, so a
regather costs the window about 10 ms.
`--flat-world` draws the earlier flat colours, `--no-props` leaves the
placed props out, `--no-far` the far ring, `--no-trees` the tree sprites.

| | Keyboard | Gamepad |
| --- | --- | --- |
| Throttle | W or Up | right trigger |
| Brake (held at a stop: reverse, then it is the throttle) | S or Down | left trigger |
| Steer | A/D or Left/Right | left stick |
| Handbrake | Space | A |
| Back on the nearest road | R | Start |
| Chase or front view | V | Back |
| Look around | | right stick |
| Look back (held) | C | Y |
| Choose another car (shown in the title) | [ and ] | LB and RB |
| Drive the chosen car (from where this one stands) | Enter | X |
| Hide or show the HUD | H | |
| Quit | Esc | |

The pad's sticks and triggers are rescaled from an inside to an outside
deadzone (steering 0.10 to 0.95, triggers 0.03 to 0.97; ours, the rule is
CAR_PHYSICS's). The chase camera's heading trails the car's by 0.2 s and
its distance follows the car's size. Picking a car restarts the drive
with it (a few seconds: the car, its sound and the world load again).

For the first six seconds a line over the HUD names the car and the
picker and reset keys; while choosing a car it shows the choice. The HUD
in the bottom-right corner shows the speed, the rpm with a
tachometer bar (red from 90 % of the redline), the gear (R in reverse)
and the ABS flag; the window title repeats them. Headless drive renders
draw it too with `--hud`. The chase camera pulls in when a wall is between it
and the car.

The engine sound (ENGINE_SOUND.md) plays when the car's loops are baked.
Bake them once (FFmpeg decodes the XMA samples at bake time; it is found
on PATH or in WinGet's packages, or pass `--ffmpeg`):

```powershell
.\build\apps\soundbake\soundbake.exe --game-root ..\horizon1-recomp\.local\game\base --state-root <state> --out out\sound --cars NIS_SkylineGTRVSpecII_02
```

`--all` bakes every car (about 3 s each). The drive reads `out\sound`
(`--sound <dir>` for another folder, `--no-sound` to turn it off).

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
# rule, and reference | engine | mask-difference PNGs (keep out/ local).
# --cars, --where key=value and --skip-scored <report.txt> rerun a subset;
# --gate adds the bake gate (two bakes from empty folders, timed and hashed)
python tools/bake-gate.py --carbake build/apps/carbake/carbake.exe --game-root <game> --state-root <state> --work out/gate --compare out/bake
python tools/m1-report.py --carview build/apps/carview/carview.exe --bake out/bake --refs <refs> --out out/m1 --gate out/gate/gate.txt

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
# The engine sound of a headless drive, to a WAV (after soundbake)
carview --drive --headless --bake out/bake --car NIS_SkylineGTRVSpecII_02 --run launch --out out/drive/r34.png --audio-out out/drive/r34.wav
carview --drive --headless --bake out/bake --car FIA_500AbarthSS_10 --world <game>/media/tracks/colorado \
        --run cruise --seconds 120 --every 10 --out out/drive/colorado.png
# The M3 gate's lap (CAR_PHYSICS.md, "Oracle lap for the M3 gate"): the
# oracle's grid, its pedals replayed by distance along its line, pure-pursuit
# steering on it, timed to the gate of lap.json; prints the race time, the
# line error, the surfaces and the time against the oracle every 100 m, and
# writes a trace physics-behaviour.py compares (--clock-hz 60)
drive --bake out/bake --car NIS_SkylineGTRVSpecII_02 --world <game>/media/tracks/colorado \
      --run lap --line <oracle-lap>/lap.jsonl --physics-trace out/lap/lap.jsonl
python <horizon1-recomp>/tools/physics-behaviour.py compare <oracle-lap>/lap.jsonl out/lap/lap.jsonl --clock-hz 60

# Performance (M2): the cruise from the spawn, every frame at 60 Hz of game
# time rendered and timed, no images: frame CPU, GPU by stage (timestamps)
# and process CPU. --preset low: no MSAA, cut-outs alpha-tested.
carview --drive --bench --preset low --size 1280x720 --bake out/bake --car NIS_SkylineGTRVSpecII_02 \
        --world <game>/media/tracks/colorado --run cruise --seconds 60 --no-sound

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
fh1data-corpus --render-models <game>/media/tracks/colorado/bin.zip [--objects]

# World materials and textures (WORLD_MATERIALS.md): material records,
# uvOffsetScale writes, vertex layouts, texture records and every .bix
# pair and CAFF texture decoded (about 15 s)
fh1data-corpus --world-materials <game>/media/tracks/colorado/bin.zip <game>/media/tracks/colorado/Ribbon_00/Colorado_00.pvs

# The textured world at the world reference's camera, without the car,
# beside the capture (out.png and out_vs.png; keep them local)
carview --drive --headless --bake out/bake --car NIS_SkylineGTRVSpecII_02 --world <game>/media/tracks/colorado \
    --world-reference <refs>/freeroam.json --out out/world-ref/freeroam.png [--no-props]

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
  Rerun with `tools/bake-gate.py` for the M1 report (2026-11-20; the bake
  now also writes each car's `physics.txt`): 45.5 s and 74.2 s, 722
  files, byte-identical to each other and to `out/bake`, combined
  `ec300aee1ca8b4b9331895c9dec3f850ab5e7683a12985fbf015846dacd5b2f6`.
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
  masks leave out the lamps' additive glow) and passes a car at IoU-g
  0.97 (the IoU without the largest reference-only region, the glow);
  plain IoU under 0.97 asks for a by-eye check, and masked PSNR is
  reported only. The M1 run (2026-11-20, 216 references: the 200
  stock-Autoshow views and the 16 unicorns), with FORMAT-CARS's stock
  parts and factory liveries: IoU mean 0.986, min 0.922; 213 of 216 pass
  by IoU-g, 30 are under 0.97 by IoU (on the 200: mean 0.9848 -> 0.9863,
  misses 7 -> 3, under 0.97 31 -> 27; the 16 unicorns all pass, mean
  0.984). With the six hidden and showcase cars (222 references, 18
  traffic models with no usable Autoshow view): IoU mean 0.986, 219 of
  222 pass by IoU-g. The three misses: the RX-8 Mazdaspeed (its kit is
  gone; what is left is headlamp glow, IoU-g 0.964), the Huayra (two glow
  regions, 0.9700) and the FJ Cruiser (its own tyre draws larger than the
  reference; its sizing is open, Q39: 0.963).
- Grilles draw their Shared tile on the tiled UV0, cut out on the
  `_alpha` techniques (ours, Q39). The six models with their own tyre set
  draw `TireA<MediaName>` from their package, sized as the shared tyre.
- Stock parts (FORMAT-CARS): each Data_Car row's IsStock upgrade rows
  name the parts drawn; other members of those families, race parts and
  the roll cage are hidden, exhausts take letter a, a missing or
  stand-in stock part leaves its slot empty (or takes its LOD1-5 twin),
  and rows sharing a model are told apart by car id (`--car-id`, or the
  reference's). A `HasRaceLivery` row shows its body atlas at every
  alpha, unpainted.
- The lighting's free settings (exposure, environment, key and fill
  strengths, the diffuse and reflection tints, metallic paint's share)
  were fitted to the references for median masked PSNR, keeping the
  diffuse light neutral; `--light name=value,...` overrides them. What
  is left is mostly the stage's sharp coloured reflections, which our
  studio environment does not have, and the white cars.
- Open in the spec: cockpit parts (Q5), calliper
  angle (Q7), slot-1 UVs (Q8). Polish for later: the showroom lighting
  (SHOWROOM_LIGHTING), the lamp glow and glare, Q13 and Q14.

## Status (M2 performance, 2026-11-20)

`carview --drive --bench` along the free-roam cruise from the spawn, 60 s
of game time (3,601 frames), 1280x720, about 570-750k triangles of
scenery with the far ring and 39k tree sprites, frames rendered back to
back without readback. GPU time is between timestamps at the frame's
start and end; frame CPU is the steps (with the streaming's hand-over),
posing and recording on the main thread; process CPU counts every
thread (the gather worker included).

| Machine | Preset | GPU p50 / p95 / max ms | Frame CPU p50 / p99 / max ms | Process CPU ms/frame |
| --- | --- | --- | --- | --- |
| blindvac, UHD 620, i5-8250U | low (no MSAA) | 10.1 / 12.7 / 16.2 | 1.16 / 2.1 / 2.8-3.5 | 1.7 |
| blindvac, UHD 620, i5-8250U | high (MSAA 4) | 12.7 / 15.5 / 26.4 | 1.18 / 2.1 / 3.1 | 1.75 |
| multivac, RTX 4080, Ryzen 7 5800X | low | 0.4 / 0.5 / 0.5 | 0.67 / 1.2-1.4 / 2.3-2.5 | 1.15 |
| Steam Deck LCD (Van Gogh, RADV), SteamOS 3.7 | low | 2.9 / 3.6 / 13.8 (GPU clock low, see below) | 2.85 / 3.9-4.05 / 6.4-7.7 | 3.97 |

On the UHD 620 (low) the GPU goes to the world's cut-outs (props'
foliage and the tree sprites, 4.5 ms), its opaque draws (3.4 ms), the car
(1.4 ms) and the sky dome (0.6 ms). It does not follow the resolution
(10.9 ms at 640x360, 11.3 ms at 1920x1080) or the triangle count (no
props, far ring or trees: 10.3 to 10.7 ms; flat colours: 10.2 ms), so
the cost is per vertex or per draw rather than per pixel.

The frame CPU's maximum was the scenery hand-over on a regather (blindvac
low: max 26-31 ms, p99 2.25 ms). The streaming worker now also makes the
new textures' images and views and the road follower's points; the old
gather's meshes and the old scenery's buffers are freed on other threads
(the buffers once the frame in flight is done, with no device wait). The
hand-over is about 1.2 ms on the i5-8250U (0.9 ms on the 5800X), and the
slowest frames are the physics steps (2.5-3.2 ms).

The Steam Deck runs a Linux build (WSL Ubuntu 24.04, gcc 13, static
libstdc++; glibc 2.38 symbols) started from a temporary Steam shortcut,
three runs. Its CPU sat at 1.5-2.0 GHz (powersave) and, the bench
presenting no frames, its GPU at 200-534 MHz, so the GPU times are an
upper bound. The slowest frame of each run is the bench driver's
stuck-car recovery (48.5 s); over SSH a first background swap sometimes
stalls 5-6 ms on the driver (a fence destroy while another thread frees
the old gather), taking the maximum to 7.7-8.8 ms.

## Licence

GPL-3.0-or-later (`LICENSE`), copyright Brandon G. Neri. The LGPL-2.1
libmspack dependency is compatible with it; see `THIRD_PARTY_NOTICES.md`.
