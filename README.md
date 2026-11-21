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
| `libs/drive` | Car physics (SPEC-CAR-PHYSICS), the cameras, the drive save, the oracle-lap follower, and the AI drivers (`drive/ai_driver.h`, M5 groundwork before SPEC-RACE-AI): one racing line every car shares, and a field that updates N cars in one pass over arrays |
| `apps/car-corpus` | Reproduces every count of FORMAT-CARS's Tests section over the player's files (`--check` fails on a mismatch) |
| `apps/carbake` | Bakes every distinct car (240), its stock wheel, the tyre archives and the shared textures to `.shiftbake` files, deterministically and in parallel; `--dump` lists a bake |
| `libs/audio` | Engine sound (SPEC-ENGINE-SOUND): the tuning XMLs (engine, harmonics, car model listener), FSB4 bank headers and XMA2 wave wrapping, the rpm map, crossfades, pitch and parameter curves, the loop index rule, a stereo mixer, and the bake to PCM with its reader; the radio (SPEC-RADIO): RadioSystem.xml and the sound-bank info files, the station schedules on one clock, the cycle and popup timing, and a player that streams the MPEG clips through dr_mp3 |
| `libs/freeroam` | Free-roam collectibles (SPEC-FREEROAM-COLLECTIBLES): the discount signs and barn finds from `gamemodes.zip` and `GameObjs.xml`, the player's progress, the smash, rumour and discovery rules, the upgrade discount and the map projection |
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

## Play the drive

Build the tools ([Building](#building)), or let setup build them, and point
setup at your extracted disc. Add `--state` with the installed packages'
tree to bake the title update's and the DLC cars as well:

```powershell
python tools\setup.py D:\fh1\disc --state D:\fh1\packages
```

```sh
tools/setup.sh ~/fh1/disc --state ~/fh1/packages
```

Setup takes about half a minute on a desktop. It bakes every car (7.4 GB),
the R34's engine sound (when FFmpeg is installed; `--all-sounds` bakes
every car's) and the Colorado model index. They go into the play folder,
which is `%APPDATA%\shift-engine\play` on Windows and
`~/.local/share/shift-engine/play` on Linux. Setup writes nothing under the
disc or the repository. `--cars a,b` bakes fewer cars; keep the 17 traffic
models (`traffic-census --list-models` lists them) or the roads stay empty.
Then:

```powershell
.\build\apps\carview\carview.exe
```

With no arguments carview finds the play folder and drives the R34 on
Colorado with traffic. The drive is the default mode, so arguments alone
still drive (`--car <media name>`, `--hour 2`, `--no-sound`,
`--size 1280x800`); `carview --help` lists them. The showroom is `--bake`
without `--world` or `--play`, the disc viewer `--game-root`. Without an
audio device it drives on silent.

- **Another folder:** `--data <dir>` puts the play folder elsewhere. Run
  `carview --play <dir>`, set `SHIFT_PLAY_DIR`, or put carview in that
  folder. The order is `--play`, `SHIFT_PLAY_DIR`, the executable's
  folder, then the user-data folder.
- **Controls:** keyboard or pad; Esc or Start opens the pause menu, which
  has the car picker (controls table below).

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
| Back on the nearest road | R | |
| Pause menu: resume, map, change car, reset to road, quit | Esc | Start |
| In the menu: choose, take, close | Up/Down (W/S), Enter or Space, Esc | D-pad, A, B or Start |
| Map (the drive waits): close, zoom, pan | M; M or Esc, + and -, arrows | Back; B, LB and RB, D-pad |
| Radio station: previous, next (Bass Arena, Pulse, Rocks, off) | , and . | D-pad left, right |
| Chase or front view | V | |
| Look around | | right stick |
| Look back (held) | C | Y |
| Choose another car (shown in the title) | [ and ] | LB and RB |
| Drive the chosen car (from where this one stands) | Enter | X |
| Hide or show the HUD | H | |
| Traffic's time of day, an hour on or back | T, Shift+T | |

The pause menu stops the car and is drawn over the HUD; "change car"
opens the picker (Esc closes it without a change), "quit" closes the
window.

**Save.** The drive keeps its own small save (ours: `drive/save.h`, a
versioned text file with the car, its position and heading, and the last
spawn) in our user-data folder, `%APPDATA%\shift-engine\drive\save.txt`
on Windows; it reads nothing of the game's profile. It is written when the
menu opens, every 30 s of driving and on quit, and read at start: with no
`--car` the saved car drives, and with no `--spawn` it starts where it
stood (when the car is the same). `--save <file>` names another file
(headless runs too), `--no-save` neither reads nor writes one.
`--menu-png <file>` writes the HUD with the menu open and exits.

**Collectibles and the map.** Free roam has the 100 discount signs and the
9 barn finds (SPEC-FREEROAM-COLLECTIBLES; `libs/freeroam`), read from
`gamemodes.zip` and placed by `GameObjs.xml`. Driving through a sign above
1 m/s smashes it: the sign is no longer drawn and each one takes 1 % off
upgrades (`freeroam::discounted_price`; no upgrade shop yet). A barn's
rumour starts every 50 km of total distance, lowest number first; driving
within 10 m of the barn's car below 50 mph finds it: its car goes to the
garage list, and the barn's closed doors give way to the open ones. These
rules are the spec's interims. The save keeps the distance, the found signs,
the barns and the garage. The minimap (lower left) shows the roads, rumour
circles and icons, with the heading up. The map (M) is north up over the
painted map. Unfound signs and barns show only with `--reveal`.
`--distance <m>` adds to the total driven (a rumour sooner),
`--no-collectibles` turns them off (`--bench` too), and `--map-png
<prefix>` writes the map, a close-up and the minimap at the end of a run.

**The radio.** Bass Arena, Pulse and Rocks play from the disc's
`media/audio/radio` banks (SPEC-RADIO; `audio/radio.h`), on from the start
on Bass Arena (the spec's interim). Clips resolve through the info files'
`SoundBankIndex`; the MPEG frames are decoded as they play (dr_mp3), not
baked. Each station has its own shuffled schedule: a track to its
`IdentStart`, an ident, the next track at the ident's
`OffsetStartNextTrack`, and a regular DJ line over the intro when one ends
before `EventStart` (music ducked to `duckValue`). Every station runs on one
clock while the radio is on and stops while it is off, so switching joins a
station where it has got to. A change fades over
`stationChangeFadeTimeUser` and shows the popup (logo, title, artist) above
the HUD panel: a station's after 1 s for 4 s, RADIO OFF at once, a new track
2 s after it starts. Our level: the files' levels as they are (about 0.12
RMS, the game's 0.08 to 0.10), about 15 dB over our idling engine. `--no-radio` turns it off; headless, it plays with
`--audio-out`, and `--radio-press <s>` presses D-pad right every s seconds.

**Races (M5; SPEC-EVENTS, SPEC-RACE-AI).** `--event <id>` (the
`HorizonEventID`, e.g. `FR22`, a 4-lap circuit on route 143, or `FR16`, a
sprint on route 154) runs the event's first race: the route, the grid and
the opponents from the disc (`libs/race`: the game database's Events,
Races, Tracks and EventParticipants; the route file's slots, start line and
sprint finish; the route's OWTM AI line from aiopenworld.zip). Each
opponent drives its own car (the bake's model and physics) on the racing
line with its skill, rubber band (torque cut ahead of the player,
catch-up and the far-behind maxima behind) and the race-start boost. The
countdown holds every car on its slot; 3, 2, 1, then the start signal at
6 s; laps count at the start line (a circuit finishes on its NumLaps + 1th
crossing), a sprint at its finish plane; the HUD shows the lap, place and
time, then the finishing place and credits; the results (place, time,
best lap, distance, driver and car) print at the end of a run. Cars
reset onto the line when wrong-way, stopped or off the route (the
interim rules). Cars outside the gathered collision drive on a ribbon
ground built from the AI line, so the field keeps racing wherever the
player is. `--difficulty easy|med|hard|pro`, `--no-rubberband`,
`--race-auto` (the AI drives the player's car, the default headless) and
`--race-seed N`. `SHIFT_RACE_TRACE=<file>` writes every car 4 times a
second. Traffic is off in a race unless `--traffic`. For example:
`carview --drive --event FR22`. Car-to-car contact is `drive/contact.h`
(a box contact with the normal chosen from the step before, so closing
at 300 km/h does not pass through).

**Traffic.** Free roam has the game's traffic (SPEC-TRAFFIC; `libs/traffic`):
the road map in `colorado.nav`, the freeroam car list and density table of
`AIOpenWorld.xml`, and the registry's spawning and driving values. Around
the player the population follows the local density for the hour (full by
day, a few cars at night): four cars at the start, then one every 5 to 9 s
on the 330 m circle while below the target, each removed past 390 m. Four
of the 17 traffic models are loaded at a time, changing every 30 s; each is
drawn from its M1 bake. Cars keep 2.2 m right of the centre line at the
road's speed (30 or 60 mph), slow for bends and for the car ahead, and
pick a way at junctions. Within 30 m of the player, or once knocked off
their line, they are full-physics cars with their own rows that the
player's car can push. Traffic is on in the window and off headless;
`--traffic` and `--no-traffic` choose, `--hour <h>` sets the hour it
follows (12 by default; the lighting does not change with it) and
`--traffic-seed <n>` its random seed. `traffic-census --game-root <disc>
[--bake out/bake] [--freeway] [--models] [--steady]` runs the spec's
census headless: noon 4.8 cars in the first minute (5.9 steady with
`--steady`; the oracle's 6.1), 02:00 settled 1.0 (0.8), 02:00 at 30 to
60 s 1.3 (0.9, a known miss: SPEC_QUESTIONS Q41); `--freeway` parks beside
a one-way carriageway instead, `--models` checks each traffic model as a
body, `--registry <text>` prints the registry lines whose id holds it.

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
# AI drivers (M5 groundwork): N cars on the oracle's line in three lanes,
# 15 m apart, with the cost of the AI pass and of the physics per step.
# 8 cars, 60 s: physics 0.74 ms a 60 Hz frame on multivac (46 us a car a
# 120 Hz step), 1.19 ms on blindvac's i7-8550U (74 us); the AI pass 0.2 to
# 0.3 us a car on both
drive --bake out/bake --car NIS_SkylineGTRVSpecII_02 --world <game>/media/tracks/colorado \
      --line <oracle-lap>/lap.jsonl --ai 8 --seconds 60

# Performance (M2): the cruise from the spawn, every frame at 60 Hz of game
# time rendered and timed, no images: frame CPU, GPU by stage (timestamps)
# and process CPU. --preset low: no MSAA, cut-outs alpha-tested, no bloom.
# The drive renders HDR through the post chain (the track's filmic curve and
# desaturation, our bloom; SPEC_QUESTIONS Q42): --no-post, --no-bloom,
# --bloom, --exposure <x> (ours 0.8). The sun shadow: --shadows off|low|high
# (low with --preset low, otherwise high).
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

Frustum culling and vertex reuse (the world's draws split by material
and 256 m ground cell, 1,536 m for the far ring; each draw's box tested
against the view's side planes; corners with equal attributes share a
vertex). Per frame about 680 of 2,420 draws and 235k of 675k triangles
are drawn, and the frames match the unculled ones pixel for pixel.
Interleaved runs, GPU p50 ms:

| Machine | Before | After | No sharing | 128 / 768 m cells | No culling (5,564 draws) |
| --- | --- | --- | --- | --- | --- |
| multivac, RTX 4080 | 0.38-0.39 | 0.22 | 0.24 | 0.25 | 0.26 |
| blindvac, UHD 620 | 9.9-10.6 | 10.3-10.4 | 10.2 | 10.3 | 13.2 |

The UHD 620 does not improve: with 2.6x fewer triangles its world stages
lose about 1 ms, and the sky and the car (unchanged draws) gain about as
much. The frame there has a floor near 10 ms that follows neither the
pixels nor the vertices; only the draw count shows (5,564 draws: +3 ms).
Left as is (10 ms against the 33 ms target). Unchecked guesses: the
iGPU's clock management holding its busy share level, or a fixed cost
per frame (the render pass's loads and stores, the timestamp queries).
Recording costs 0.10 ms on the 5800X (0.07 before) and 0.21 ms on the
i5-8250U (0.16). The vertex hashing runs on the gather worker.

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

## Refactor candidates (performance)

Standing rule (2026-11-20): match the specs' behaviour, numbers and gates,
not the game's structure; pick the fastest design for modern hardware.
Where the code is shaped per object for no reason, it is listed here.

- **World draws are CPU-recorded one by one.** Each frame the CPU culls
  about 2,400 draw boxes and records about 650 draws, each with its own push
  constants and descriptor bind (`Showroom::record_world`). The shadow pass
  walks the same list again (`record_shadow`). Target: one buffer set per
  scenery, bindless textures, a compute pass that culls into indirect
  buffers for the view and the shadow map, then `vkCmdDrawIndexedIndirect`
  per pipeline.
- **Traffic cars are drawn per instance and per part.** Each guest car
  replays its whole draw list for every instance (`record_instance`, and
  `record_shadow_car` in the shadow pass). Target: one instanced draw per
  model and material, with an instance buffer of transforms, so traffic
  at scale costs draws per model, not per car.
- **The traffic population is an array of `Car` structs stepped one at a
  time** (`traffic::Population::step`), with a `std::function` ground query
  per kinematic car. Target: structure-of-arrays lanes (road, s, speed,
  lateral) stepped in a batch, with ground heights batched per tile.
- **Traffic bodies are full `drive::Vehicle`s stepped serially on the
  main thread** (`traffic::Bodies::step`). Bodies only interact through
  contacts, so they can step in parallel on a worker pool, with contacts
  resolved after the step.
- **The car's draws bind a descriptor set per material**
  (`record_draws`). Target: bindless textures and draws sorted by
  pipeline, the same as the world.
- **Streaming starts a `std::async` per gather.** Target: a persistent
  job system shared by gathers, texture decode, the traffic model loads
  and (next) the radio's decode.
- **Regathers rebuild the whole scenery every 200 m.** The drive now
  hides it (props cross-fade by eye distance over both LODs, and the old
  scenery dithers out under the new one for 0.5 s, drawing both), but the
  fade costs a second scenery's draws during the swap and LOD00 is held for
  every prop within 350 m. Target: per-cell residency with per-instance
  LOD picked on the GPU (the culling pass above), so only cells that change
  stream and no frame draws two sceneries.
- **The bloom runs as three full-screen fragment passes.** A compute
  down-sample and blur would do the same in fewer passes. It is small
  today (under 1 ms on the UHD 620), so it comes last.

## Polish backlog

These are known gaps that don't block driving or features. Each runs on
the interim noted here until a later polish sweep.

- **Chase camera feel (first: it hurts the driving).** Brandon on
  build-play: "the chase camera is super bad, the angles make the car
  feeling like you're driving wrong, feels horrible". Suspects, all Q45
  interims: the PitchAngleDegrees sign (inverted, the camera tilts the
  wrong way on slopes and under braking or acceleration), the slope
  term's limit and timing, the yaw spring's lag, and the half-angle
  against full-angle FOV factor. To do: compare our chase with a game
  chase capture at speed through corners and slopes and tune until it
  feels right; feel wins over the interims.
- **World bundle textures** (Q44 a). Type-1 and type-3 slots resolve
  through the 146 `.bundle` members of Colorado's bin.zip. Interim: a 4×4
  fallback, so the power-line cables (record 4203, about (33, 34, 33))
  draw flat. The bundles also hold low mips for far LOD.
- **Warm hill shading** (Q44 c). The game shades lerp(Blend_A, Blend_B,
  R/255) under the warm sun (0.97, 0.84, 0.58). Interim: the current
  terrain shading, which leaves grey bands. Reference pose: camera
  (2243.8, 119.8, 706.9).
- **Camera details** (Q45). The slope term, pitch sign, speed-FOV factor
  and cockpit interior. Interim: as in `libs/drive/src/cameras.cpp`.
- **Lamps.** CARS.md gives no brake-lamp rule. Interim: red covers and
  tail-lamp techniques glow while a front wheel brakes. Cover tints
  (red, amber, clear) are picked by technique name.
- **Handbrake torque.** CAR_PHYSICS only names `BiasHandbrake` as
  probable. Interim: 2500 N m per rear wheel; it locks the rears and holds
  a 10 % grade (`handbrake_tests`).
- **Traffic junctions.** No crossing check (`Junctions/
  MaxCollisionCheckDistance`) and no steer-aside for the player. Two cars
  can meet at a junction.
- **The cruise test driver's junction choice.** At T junctions it can
  drive straight into a rail; headless runs put it back.
- **The F12 screenshot.** F12 saves the pose but not an image yet (the
  window has no swapchain read-back).
- **Collectibles presentation.** No discovery cut scene, fade, achievement
  or rumour-reveal map animation (a notice line instead); no smash debris or
  sound. The barn's collision doesn't follow the doors (only the drawn
  props swap; the 10 m trigger is reachable from outside). A smashed sign
  stays drawn until the next gather lands (one frame headless, a moment
  in the window). Barn car names come from the string-table key
  (`Barnfind.str` is not read).
- **Map details.** The minimap's tilt, zoom and icon sizes are ours (the
  spec's interim: 45 degrees, about 300 m); its icons don't fade by
  `VisibleRange`. No traffic dots, place-name layers or the high
  background tiles (the low 2048×1229 level only), and no Treasure Map
  (`--reveal` stands in). Profile line sizes are scaled by our factors.
- **Holes in the collision.** The `.fiz` tiles cover the drivable strips
  only: east of the Bugatti barn (1448, -5363, tile 193) there is no
  triangle under the drawn ground, in either frame and before any filter
  (the barn itself is covered). A car 20 m below where its wheels last
  touched is put back on the road (ours; sent to the spec side to say what
  the game does there). The barn is reached from its road only through the
  rail.
- **Radio against the engine.** The spec has the radio about 20 dB over
  the idling engine; ours is about 15 dB, because our idle (0.022 RMS) is
  louder than the game's (0.008). Raising the radio clips under throttle
  (peak 1.12 at 1.8 times), so the engine's own level wants the look.
- **Races: against the oracle.** `python tools/race/gate.py` runs the M5
  gate (the player replays the oracle's path from `out/oracle/`). FR22 at
  Med, three seeds: AI mean 162.4 s (oracle 161.5), 4,673 m (4,638), the
  player 2nd to 3rd (3rd), winner slot 00: passes. FR16: AI mean 129 to
  134 s against 124.0 (fails 3 %), the AI R34 itself 125.5 s inside the
  oracle's 122 to 126 s pack, the rear-drive cars 128 to 132 s; 4 to 7
  resets a run, from pack contact (a car hit from behind spins and pins on
  a wall for the 3 s no-progress rule). To do: fewer rear-end hits at the
  start and in the pack, then a per-car look at the rear-drive cars.
  Headless runs with `--async-stream` or `--bench` outrun the background
  gather and fall off the collision (the gate runs synchronously).
- **Race AI interims (ours where the spec has none).** The steer is capped
  at the pad's speed-narrowed lock (past it the fronts slide wide); a car
  more than 1 m off its lane slows 8 % a metre; the over-braking and over-cornering mistakes hold
  1 + Amount for 1 s (the spec's interim) and the lags have no effect;
  a slide guard (throttle cap on wheelspin, cut and counter-steer past 5 to
  13 degrees of body slip) and a speed cap behind a slower car in the way; cars alongside keep a car's
  width apart (beyond the spec's passing rule); one AI line; stock cars;
  opponents take the free slots in row order. Pack contact is frequent
  (800 to 1,250 steps with a contact in an 8-car FR22, 1,400 to 3,500 in FR16).
- **Garage and upgrades (SPEC-GARAGE-UPGRADES interims).** `--import-profile`
  writes `garage.txt` beside the save. The selected garage car drives
  with its fitted parts, from the merged database (`SHIFT_STATE_ROOT`'s
  packages when set, the disc's otherwise; opened in the background; point
  it at the sanctioned copy `out/state-copy`, never the AppData tree:
  both import trees then find every garage car, class and parts value all
  match, mass 18 of 19 and 37 of 38, the Corrado 6.7 kg off). The
  pause menu's upgrades list is ours: up and down pick a slot, left and
  right a part, Enter buys it at the sign-discounted price or refits an
  owned one, Esc rebuilds the car where it stands. Interims: the index and
  class stay the stored ones after a part change (the virtual lap is
  open); engine, drivetrain and body conversions are not offered; the
  wheel geometry stays the stock bake's (a rim or width part changes the
  grip, not the drawn wheel); no Auto Upgrade; no selling.
- **Autoshow and paint (SPEC-AUTOSHOW-PAINT interims).** The pause menu's
  autoshow is one list (class, then index; left and right jump a class)
  instead of the make carousel, of the cars the bake has; the offer
  visibility rule is not applied (on the disc alone the 10 offer-only cars
  are listed); the bought car takes its first colour (no colour bar); no
  tutorial list. A garage car is drawn in its stored colour Sequence; the
  paint shop and player liveries are not done.
- **Race AI speed planner (SPEC-RACE-AI Speed planning, Q53).** Each car
  plans from its own lateral, braking and drive capacity tables (40 speeds,
  from the engine's tyre model at 0.98 of the car's mass), the line's
  curvature, dip and bank, and a 420 m braking pass. Interim: the tables are
  scaled by the R34's shortfall against its measured AI row (lateral
  16.01/14.6, braking 18.26/17.1), and the heavy SUVs plan higher than the
  title's yaw-capped 15.3 to 15.5 (sent to the spec side).
  `SHIFT_RACE_PLANNER=fit` restores the old flat fit. Gate, one seed: AI
  mean time within 3 % on FR06, FR22, FR33 and HEADLINE; slow on
  EXHIBITION_FEST_003 (209.7 against 179.0 s), NR02 (134.9/130.7, from
  145.5 before the minimum-curvature street line), STREET_PLNS_002
  (131.4/124.1), FR24 (119.9/110.8), FR01 (109.5/102.7) and FR16
  (127.9/124.0). Where the time goes: on FR01 the R34's acceleration
  matches the oracle's from 20 to 45 m/s and its line is the OWTM's
  (the oracle sits at 0.6 to 0.7 of the file's offset on seven of eight
  events, 0.97 on HEADLINE; scaling the offset did not help), the loss is
  pack contact and one spin over a dirt edge; on NR02 the AI loses two
  contacts with the replayed player and runs 37 % of the race within 20 m
  behind it. EXHIBITION's showcase Minis (stock rows that are upgrades,
  now with their deltas: acceleration 1.0 to 1.3 of the oracle's) plan
  from 20.7 m/s^2 lateral and 20.2 braking, slide off the first corners
  and need 33 resets (asked of the spec side). The AI's steer is capped
  at the pad's speed-narrowed lock (interim); twice the cap changed
  nothing. Tools: `SHIFT_PLAN_DUMP=<file>` (the corner pass and the AI
  line file), the trace's `hit`, `hny`, `surf` (the strongest body
  contact).
- **Street and nemesis races (SPEC-EVENTS, SPEC-RACE-AI interims).**
  Ribbon-6 routes have no AI line: the line is the shortest road-map path
  through slot 00 and the checkpoints (`race/street_line.h`), smoothed to
  round the junctions, half width 4.5 m, no racing offset yet (the spec
  asks for one to the inside of bends). Checkpoints are planes crossed
  either way within half their width, in order; the last is the finish,
  held until the others are passed; a missed one falls to the off-route
  reset. `STREET_PLNS_002`: AI mean 137 s (oracle 124.1), 5 to 11 resets a
  run; `NR02` (one opponent): 161 s (130.7). No traffic in races (the spec
  has it on for these). Route 136 (`FR44`, `FR62`) has neither gantry nor
  cannons: the start line through slot 00 and the finish across the AI
  line nearest its last waypoint (ours). Races on ribbon-2 routes run once
  to the sprint finish whatever `NumLaps` says (the spec's rule); `FR37`
  (ribbon-0 sprint, 3 laps) is untested.
- **Race flow left out.** No event card, pre-race menu, cutscene camera,
  finish screen or results screen (the banner carries the result, then
  A/Enter continues in free roam at the route's post-race spot and Y
  restarts); the pause menu's race item picks any runnable event. No
  player wrong-way warning or rewind. Opponents show as red dots on the
  minimap and the pause map (ours; the profiles' opponent icon is unmapped).
- **Ribbon ground.** The far cars' road is the AI line's centre +- its half
  width plus 8 m, flat across (within 6 cm of the collision on the line
  near the grid): a car on it laps a little differently from one on the
  collision. A near car with no collision under it steps on it too. Refactor candidate: step the far cars on a worker
  (they are independent of the player's ground).
- **AI drivers.** The physics is the cost (46 to 74 us a car a step); the
  cars step one after another on one thread (a worker pool is the next
  lever if a field grows). The same exe ends a run differently on two
  machines (float results differ by CPU), so runs are not bit-identical
  across machines.
- **Radio details.** No snapshot levels (pause, loading, speed, place) and
  no engine duck under DJ lines; no RADIO VOLUME, RADIO DJ or saved station
  (the profile is not read yet); a clip's bytes are read on the main thread
  when it starts (a 10 MB read for a long track: prefetch on a worker). The
  popup is an opaque panel whose content fades (the overlay has no alpha),
  in the HUD's 5x7 font, upper case; characters it lacks are left out.
  Special, festival and story DJ lines and the 3D location stations are out
  of M4's scope.

## Licence

GPL-3.0-or-later (`LICENSE`), copyright Brandon G. Neri. The LGPL-2.1
libmspack dependency is compatible with it; see `THIRD_PARTY_NOTICES.md`.
