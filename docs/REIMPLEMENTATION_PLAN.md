# Native reimplementation: reverse-engineering foundations and plan

Written 2026-11-19 at `dev` `4631583d`. The question: how do we get from
Pinyon Shift, a static recompilation, to a native reimplementation of the
game's engine, in the way OpenRCT2 replaced RollerCoaster Tycoon 2? In that
model the player points the program at their own disc image, and the
project ships its own source code and binaries. It needs no recompiled
executable and no SDK. The goal is efficiency, not byte parity: the new
engine may compute differently from the original as long as it plays the
same.

Nothing was built or run for this page. The project's numbers come from the
pages and records linked below. The numbers for other projects come from a
web survey done the same day; each one links its source, and anything that
survey could not confirm is marked *unverified*. Savings marked *estimate*
are arithmetic on recorded numbers. The IDs are `RF-*` for
reverse-engineering foundations, `RI-0.*` for Stage 0 probes and `M*` for
engine milestones.

This page extends B6 in
[BEYOND_RECOMP_RESEARCH.md](BEYOND_RECOMP_RESEARCH.md#b6-decompilation-and-an-open-reimplementation-effort-100).
It turns the one-paragraph sketch there into a programme of work.

## Summary

- **What can ship as a binary.** A release binary can be shipped only if
  it contains no translated guest code and no game data: no PowerPC
  translation, no shader microcode and no constant tables taken from the
  executable. Everything else follows from that rule:
  - Pinyon Shift's `guest_native` replacements stay where they are. They
    are the test harness, not the product.
  - The new engine is a separate program with its own data model. It
    reads the player's files through its own format readers.
  - Some engine modules have an interface narrow enough to plug into
    Pinyon Shift through an adapter. Those modules are also played and
    measured inside the real game. That is how the OpenRCT2 approach is
    used here.
- **Reverse-engineering foundations come first, and they are most of the
  work.** The project has 572 findings, but only about 561 functions are
  named, roughly 1 % of the image. The data formats a renderer and a
  physics step need are mostly undecoded:
  - world geometry, the PVS, collision, `physicsdefinition.bin`, the FMOD
    `.fev` event files and the `.fxobj` effects;
  - the world formats alone are about 40 % of `media/`.

  No public reverse engineering of these formats exists. RF-1 to RF-9
  build the tools and records that close these gaps.
- **There is no session-replay oracle, so verification has to be built
  around that.** CS-0.1 and CS-1 showed that whole-session replay does not
  hold ([CLEAN_SLATE_RESEARCH.md](CLEAN_SLATE_RESEARCH.md#progress)).
  Every gate here therefore uses one of four checks:
  - short-horizon state injection: set a state, step N ticks, compare;
  - traces compared within a tolerance, using the existing
    `tools/physics-behaviour.py`;
  - image comparison against the recompilation's frame dumps;
  - comparison of save and profile fields.

  Our own engine can be deterministic by design, so later work can test
  the engine against itself.
- **The efficiency case is structural, so it is measured early.** B6 said
  a reimplementation "pays in control, not speed". That holds for rewriting
  the same code function by function: live game code ran only 1.2-1.5 times
  faster when native. What a separate engine removes that the recompilation
  cannot:
  - the GPU emulation front end, about 24 ms of CPU a frame on the Deck;
  - about 4,500 draws a frame on the CPU, with no occlusion culling;
  - physics tied to the frame rate;
  - the 512 MB mirror and the endian conversion.

  The first native world draw (M2) is the gate that decides whether the
  programme continues.
- **One behaviour target: v4.** The engine's code reproduces the v4 title
  update, which includes Rally and the offline 1000 Club. Its data comes
  from the base disc, with v4's `media.zip` and DLC as overlays.
- **Open decisions for the maintainer** are listed in
  [Decisions](#decisions-for-the-maintainer). The largest are the
  repository and its licence, the name, and the legal points:
  - shipped binaries;
  - save keys;
  - how the specification work is kept separate from the implementation
    (clean room).

## What we start from

### Knowledge

| Area | Depth | Notes |
| --- | --- | --- |
| Main loop, threads, pacing | struct layouts; parts native | [threads](../site/findings/threads.toml): the simulation step runs once per rendered frame (8.33 ms at 120 fps, 16.67 ms at 60), so physics is tied to the frame rate |
| Renderer | struct layouts; parts native | 110 records. The frame structure, shadows, post chain, resolves and D3D wrappers are known. Native owned-renderer studies FR-1 to FR-6 all closed below their gates *inside the recompilation* |
| UI (Anark Gameface UI4) | struct layouts | Scene registry and pause-menu model are known. BGF/FBF/BSG round-trips only for the pause scene, which is 1 of about 300 scene sets |
| Events, career, restrictions, DLC | struct layouts | [content](../site/findings/content.toml). The restriction encoding is decoded |
| Saves | struct layouts; partly native | `tools/fh1-profile.py` round-trips the profile field tree. Encryption is handled only by a runtime hook, with no offline crypto |
| Car dynamics | data flow; some offsets | The `ICarDynamics` vtable, wheel blocks, ABS/TCS and the step's two paths are recorded. 117 of the 131 physics functions are unnamed. The tyre model is located but not understood |
| AI opponents | data flow | Assignment, rubber-banding and difficulty are recorded. The driving logic (47 RacingAI/MLP classes) is unexplored |
| World streaming, collision | collision decoded; streaming named only | The static collision tiles, their lattice and the per-triangle surface index are confirmed against the physics oracle ([COLLISION.md](formats/COLLISION.md)); streaming is still one "guess" |
| Audio | data flow; parts native | FMOD Ex internals, FSB4 and radio are known. The engine loops (car to bank, crossfade, pitch, curves) are specified in [SPEC-ENGINE-SOUND](specs/ENGINE_SOUND.md); its DSP stages and the event-project car sounds are not |
| Flow scripts | data flow | StateGroup XML is data. Its 439 behaviour names are C++ classes, so their meaning is code |

### Formats

| Format | Depth | Reader |
| --- | --- | --- |
| Disc, FH1 ZIP (LZX method 21), manifests | read + write, byte-exact rebuild | `tools/pinyon_setup.py`, `tools/fh1_archive_extract.cpp`, `tools/patch-fh1-archive.py` |
| STFS DLC, v4 `.xexp` | read (the SDK's reader) | `tools/manage-fh1-dlc.py`, `tools/pinyon_title_update.py` |
| `.xds` textures | base level on `dev`; every mip and a BC1/BC3 encoder in the unmerged car-importer work | `src/ui/hostui/xds_texture.cpp`, `tools/inspect-fh1-ui.py`. Every `.xds` on the disc is 2D (RI-0.3) |
| `.carbin` car meshes | round-trip. The header and the mass block are opaque | `tools/fh1_carbin.py` |
| `gamedb.slt` (SQLite, 221 tables) | read + write. Many physics columns are unknown | `tools/build-mod-patches.py` |
| `.str`, vector and bitmap fonts | read + write | `tools/fh1-strings.py`, `src/ui/hostui/vector_font.cpp` |
| World XML (placements, routes, settings, time of day) | read | plain XML |
| FSB4 banks | read; radio read + write | `tools/build-fh1-radio.py`, `tools/fh1_audio_decode.cpp` |
| Colorado render models (`.rmb.bin`) | positions and index lists of all 14,291 (RI-0.4), and instance placements with each object's model (`.pgeo`, `.pvsz`, the `.pvs` object table); the rest of each vertex, materials and the LOD choice are not decoded | `tools/re/world-geometry.py`, [WORLD_GEOMETRY.md](formats/WORLD_GEOMETRY.md) |
| `.bix`, `_0x*.bin` textures | identified (RI-0.4), not decoded | |
| `.pgeo` kind 7 (prop instance matrices), `.pvsz` placements | instances read and matched (RF-7); model IDs, other kinds and the `.pvsz` framing not decoded | `tools/re/world-geometry.py pgeo` |
| Collision (`.fiz` tiles, the `.col` tile index, `surfaceTypes.xml`) | the static mesh, its tiling and the per-triangle surface index (M3 items 1-3, 2026-11-20); flag fields, prop shapes (`PhysicsDefinitions.bin`) and how the tyre uses the surface columns not decoded | `tools/re/collision.py`, [COLLISION.md](formats/COLLISION.md) |
| `.sh`, `.gr2`, `.skinbin`, `.xpr` | **none** | |
| PVS (`.pvs`, `.pvsz`, `PVSZLookup`, `FilenameMap`) | **named only** | |
| `.nav`, `.owr`/`.oww`, `OWTM` racing lines, `PhysicsDefinitions.bin`, `.col`, `.crowd`, `.soundscape` | **none** | |
| `.fxobj` effects | container: microcode and 34 vertex declarations | techniques, render states and parameter bindings are not decoded |
| `.fev` FMOD events | **none** | |
| `.wmv` movies | stored | today they are decoded by the XMedia facade module |

Three counts on these pages disagree:

- **Functions.** 57,127 is the base image's `.pdata` count. 76,262 is the
  number the generator translates, which includes split entry points.
  76,499 is the figure cited in BEYOND_RECOMP_RESEARCH.md and internals.
  None of the three is the size of the work: RI-0.1 measures the live
  code.
- **Scenery members.** `files.toml` counts 43,589 `.bix` and 115,538
  `.bin` members. `world.toml` says about 230,000 `.bix`. RI-0.2
  reconciles them.

### Tools

These already exist and RF-* builds on them:

- **Knowledge base:** `tools/re/kb.py`, with `check` and `lint`.
- **Ghidra:** the workspace and its MCP server (`tools/re/ghidra-*`).
- **Probe build and profilers:** `tools/re/probe-summary.py`,
  `tools/re/batch-census.py`, `tools/re/draw-chains.py`.
- **Three lockstep verifiers:** `src/guest_native/guest_native.cpp`,
  `src/guest_native/reference_verify.cpp`, `src/guest_native/region_oracle.cpp`.
- **Frame dump and offline replay:** `tools/replay-fh1-frame.py`.
- **Scripted routes:** 131 routes from pinned seeds
  (`tools/run-fh1-render-test.py`). They support `poke`, `snapshot`,
  `capture` and `audiocapture`.
- **Physics trace and behaviour gates:** `tools/physics-behaviour.py`.
- **Telemetry:** Forza-style Dash and Sled UDP output (`src/telemetry/`).
- **Mod overlay:** archive and member overrides through
  `tools/build-mod-archives.py`.

## What the precedents teach

| Project | Route | What it teaches |
| --- | --- | --- |
| OpenRCT2 | A DLL hooked into `rct2.exe`, replacing functions until none were left. Work started in April 2014, the first release came in December 2014, and 0.0.5 (December 2016) is listed as the first version that no longer needs the original exe ([Wikipedia](https://en.wikipedia.org/wiki/OpenRCT2), [forum](https://forums.openrct2.org/topic/2364-fully-implemented)) | The game was playable at every step, and the original binary was the oracle. Our reading (an inference, not something the project states) is that this worked because the native x86 code could share the original's memory layout directly. Here the guest layout is 32-bit big-endian, so code written against it would carry that layout forever. That is why the adapters below exist |
| re3 / reVC | The same route: hook-and-replace inside `gta3.exe` | Sued by Take-Two in 2021 for copyright infringement; settled in 2023 with no ruling ([TorrentFreak](https://torrentfreak.com/take-two-dismisses-claims-against-lead-defendants-in-gta-mods-lawsuit-230405/)). It was reverse engineered from the original executable and replaced it function by function. The clean-room split here keeps the engine's code from being derived that way |
| OpenMW | A new engine reading Morrowind's data, with behaviour recorded on wiki research pages | It specified behaviour in prose and calibrated it against the original. After more than 17 years it is still before 1.0 ([Wikipedia](https://en.wikipedia.org/wiki/OpenMW)). The scope has to stay narrow |
| OpenNFS | A new engine using Bullet's generic vehicle, loading the original NFS data | After many years it is still early and has no faithful handling ([repo](https://github.com/OpenNFS/OpenNFS)). A racing engine without an oracle for handling stalls |
| OpenGOAL, SM64 / Ship of Harkinian, DevilutionX | Decompilation, then a port | They use the player's own ISO or ROM. Nintendo went after distributed SM64 builds ([VGC](https://www.videogameschronicle.com//news/nintendo-takes-action-against-mario-64-pc-port)). Publishing decompiler output is ruled out here by [LEGAL.md](LEGAL.md) |
| UnleashedRecomp, N64Recomp | Static recompilation | This is where Pinyon Shift is today |

What the survivors had in common: they shipped no original code or assets,
required a copy the player owns, and gave no help with piracy. FH1 and its
DLC were delisted on 20 October 2016, and the Xbox 360 store closed to
purchases on 29 July 2024. Owners can still download what they bought
([Windows Central](https://www.windowscentral.com/first-forza-horizon-game-will-be-removed-sale-xbox-store),
[Xbox Wire](https://news.xbox.com/en-us/?p=195254)). No public
documentation of 360-era ForzaTech formats or physics was found. Turn 10
has said its earlier games used one tyre contact point at 60 Hz
([TechRadar](https://techradar.com/news/forza-motorsport-is-a-huge-generational-leap-from-previous-games-heres-why));
whether that applies to FH1 is *unverified*, and RF-5 measures it.

## Architecture: three programs, one knowledge base

```text
 player's disc + v4 + DLC
          |
          v
 +-------------------+   specs, findings,    +--------------------------+
 | Pinyon Shift      |   traces, images      | New engine (new repo)    |
 | (recompilation)   | --------------------> |                          |
 |  - the oracle     |                       |  baker: disc -> packs    |
 |  - adapters run   | <-------------------- |  runtime: SDL3 + Vulkan  |
 |    engine modules |  engine modules as    |  modules: physics, AI,   |
 |    in the game    |  static libraries     |  world, render, audio,   |
 +-------------------+                       |  UI, flow, saves         |
          |                                  +--------------------------+
          v
 site/findings + format specs + behaviour specs (written from our own analysis)
```

- **Pinyon Shift stays the shipping game and the oracle** until the engine
  matches it, milestone by milestone. It gains RF-5's state-injection and
  trace API.
- **Engine modules are libraries with their own data types.** They are
  little-endian, use native pointers and contain no guest structs. A
  module with a narrow interface also gets a Pinyon adapter: a
  `guest_native` replacement that copies the guest state into the module's
  types, steps it and writes the result back. It is verified with the
  existing lockstep or trace gates, and it can be played in the real game.
  Candidates, by how narrow the interface is:

  | Module | Interface | Adapter? |
  | --- | --- | --- |
  | Car physics (per-body step) | `sub_82D46EF8(world, body, dt, …)`, wheel blocks and CCarDynamics offsets partly known | **Yes**, after RF-2 maps the body state. This is the main candidate |
  | Tyre model | `sub_82D2F348`, scalar, no events or allocation | Yes, a pure function |
  | Event rules (restrictions, rewards, eligibility) | Pure functions over database rows | Yes |
  | Engine sound model | CAudioThread command lists (58 % of that thread) | Probably |
  | AI driving | Unknown | Decided after RF-5 |
  | Renderer, world streaming, UI | Wide: they own memory, the GPU and files | **No.** They are built standalone against baked assets, with frame dumps as the reference |

- **The baker** runs once at install, as the shader and AOT pipeline steps
  already do. It reads the disc, v4 and DLC, and writes engine packs:
  - untiled block-compressed textures;
  - little-endian meshes;
  - world sectors merged for GPU-driven drawing;
  - audio decoded from XMA or MPEG into a host codec;
  - movies transcoded with FFmpeg, if FFmpeg decodes the disc's `.wmv`
    files (*to verify*, RI-0.6).

  Packs are local to the player and are never distributed. `gamedb.slt`
  stays SQLite, read at run time.

### Runtime choices

Each choice is a recommendation, with what would change it.

| Layer | Choice | Why / to verify |
| --- | --- | --- |
| Platform | SDL3: windows, input (gamepads, DualSense), the audio device, files | Pinyon Shift already uses SDL3 for input. Covers Windows, Linux and the Deck, macOS and Android |
| GPU | A thin in-house layer over Vulkan 1.3 with descriptor indexing; MoltenVK on macOS first | Every yardstick device runs the project's Vulkan backend today: RADV on the Deck, Turnip on the Odin, Mali bindless on the Tab S10, MoltenVK on the Mac. Native Metal only if M2 shows MoltenVK costs. SDL_GPU was considered; whether it offers bindless and indirect-count draws is *to verify* |
| Renderer | GPU-driven: compute frustum and depth-pyramid occlusion culling, multi-draw indirect, bindless materials, our own shaders reproducing FH1's look (the post chain is recorded in [rendering](../site/findings/rendering.toml)) | The game has no occlusion culling, and 48-70 % of the main scene's depth-writing draws on the Odin pass no samples (#784). The Xenos microcode is game data and is not shipped. It may be translated on the player's machine as a reference, as today |
| Simulation | A fixed-step, deterministic simulation thread. Physics runs at its own rate with interpolation for rendering, and car bodies in parallel on a job system | Removes the frame-coupled step. The rate is chosen by RF-5 measurements, not by the 360 Hz marketing figure |
| Audio | Our own mixer and DSP. A `.fev` event reader (RF-6) and FSB4 samples Whether FMOD Ex can still be licensed for a host build is *unverified*, and an open engine could not ship it anyway. The DSP twins in `src/guest_native/fmod_*.cpp` show the mixer is small |
| UI | Our own retained-mode UI drawing the game's textures, fonts and strings. Menus are rebuilt as data plus code, screen by screen | A full Gameface UI4 player is larger than the menus it would run. It can be reconsidered after M6 |
| Flow and events | StateGroup XML loaded as data; the 439 behaviours implemented in C++ as the milestones need them | The XML is data; the behaviours are specified by RF-4 |
| Saves | Our own open format, plus an importer that takes a profile Pinyon Shift has decrypted and exported in game | No offline crypto exists, and no keys are shipped (see Decisions) |
| Mods | Pack overrides from the first milestone; database patches as in Pinyon Shift | Mod support costs little if it is designed in from the start |

### Why the rejected renderer numbers don't carry over

| Result | Why it failed inside the recompilation | Whether it applies to a separate engine |
| --- | --- | --- |
| Engine-level scene renderer, 91-95 ms frames (NN) | The guest still built its D3D command buffers and PM4, so the owned path ran on top of the emulated one | Not applicable: there is no guest |
| B4, FR-3 to FR-6 (5 ms ceiling against a 6-15 ms gate) | Skipping decode still left 1.57 µs per decoded draw of front-end excess. The guest still walked its scene | Not applicable. The ceiling measured what could be removed *while the guest runs* |
| Live game code 1.2-1.5x faster when native (#1071) | The same algorithm, rewritten | **Applies** to logic rewritten one for one. The engine's gain has to come from structure (rates, culling, threading, data layout), not from rewriting the same functions |
| Flat guest profile, no function above 3 % | — | **Applies.** There is no single hot spot; every subsystem has to be rewritten to gain |

The CPU budget on the Deck, as an *estimate* from
[threads](../site/findings/threads.toml) and
[platforms](../site/pages/platforms.md#linux-and-the-steam-deck):

- **Today:** process CPU is 44-52 ms a frame. Of that, the GPU emulation
  threads take about 24 ms and the guest threads about 23 ms.
- **Removed outright:** the 24 ms of GPU emulation.
- **Guest threads (23 ms):**
  - Rewritten one for one, at 1.2-1.5x, they become 15-19 ms.
  - If scene submission, the largest share at 8 ms, became a native
    engine's per-draw cost, 0.6-1.0 µs a draw
    ([CLEAN_SLATE_RESEARCH.md](CLEAN_SLATE_RESEARCH.md#summary)) over
    about 4,500 draws, it would be 2.7-4.5 ms. GPU-driven culling and
    indirect draws would take it lower; how much lower is unmeasured.
  - If FMOD's 3 ms becomes a small native mixer, it may halve, as the
    stereo change did (a guess, not measured).
- **End point:** about 14-22 ms of process CPU a frame.

M2 and M3 measure this end point. No milestone claims it.

## Reverse-engineering foundations (RF)

This is the bulk of the work. Each item names the tool or record it
produces.

| ID | Foundation | What it produces | Builds on | Size |
| --- | --- | --- | --- | --- |
| RF-1 | **Live-code census and subsystem map** | Per-function entry counts over the full route set from the probe build, joined to RTTI namespaces, vtables and call-graph clusters. Output: each function assigned to a subsystem, with a "live in play" flag and a coverage page on the site (named, specified, implemented). This replaces the function counts above with the real size of the work | `tools/re/probe-summary.py`, `batch-census.py`, RTTI records | M |
| RF-2 | **Type database** | Struct layouts as data (`config/re/types/*.toml`): field, offset, type, confidence and source. Applied to Ghidra, used to generate the big-endian accessors the Pinyon adapters need, rendered on the Reverse engineering pages, and checked by `kb.py`. Starts with the car body, wheel blocks, the world root chain, the event and session objects and the profile | `config/mod/fh1-symbols.toml` offsets, objects findings | M, then ongoing |
| RF-3 | **Class hierarchy recovery** | 6,789 RTTI types (7,558 in v4) with base classes, vtable slots and slot owners, as data. Ghidra gets real class types | RTTI records, `ApplyPinyonKnowledge.java` | M |
| RF-4 | **Behaviour specifications** | A written specification per subsystem: inputs, state, update rule, rates, edge cases and tolerances, in prose, equations, tables and diagrams in our own words. No pseudocode: code-shaped text derived from a decompiler is the transcription [LEGAL.md](LEGAL.md#native-replacements-of-game-and-library-code) rules out, and RF-8's lint refuses it. Linked from findings and stored with the subsystem's docs. These are the inputs the implementation is written from | findings, RF-2, RF-5 traces | L, per subsystem |
| RF-5 | **Oracle harness in Pinyon Shift** | A stable API for each subsystem: snapshot, inject, step and trace. Car body state, inputs, AI cars, event state and the time of day go to versioned trace files (JSONL with a binary option). There is a scenario library (seeds and routes) and a comparator library with tolerances per field. This replaces whole-session replay with short-horizon checks: inject a state, step N ticks, compare | route `poke`/`snapshot`, physics trace, telemetry, `tools/physics-behaviour.py` | L |
| RF-6 | **Format workbench** | One C++ reader library for the engine, with a Python twin for quick work. For every format: a specification page, a reader, a round-trip test and a corpus test run on the player's disc ("every member parses"), plus a report of the share of bytes understood per format. It absorbs `fh1_carbin.py`, `fh1_xds.py`, the archive tools and the string, font and profile readers, and owns the decoders now borrowed from the SDK (LZX, STFS, XEX patches) | the readers listed under Formats | L |
| RF-7 | **Read tracing and mutation** | (a) A file-read trace in Pinyon Shift: for every read, the member, offset, length, thread and caller, together with what appears on screen afterwards. (b) A mutation runner: change bytes in an archive member through the mod overlay, run a route, diff images and snapshots. (c) Frame-dump correlation: find the vertex and index buffers of a PM4 dump inside decompressed member bytes. Method (c) found the carbin vertex encoding | mod overlay, frame dumps, the render-test runner | M |
| RF-8 | **Clean-room process** | Two roles, enforced by tool access. **Specification** (Ghidra and disassembly allowed) writes findings, types and specs; `kb.py lint` is extended to refuse code in specs. **Implementation** works without Ghidra access, from specs, formats and oracle output only. Each engine commit names the specs it implements. With agents the two roles are separate sessions with separate tool permissions | `kb.py lint`, [LEGAL.md](LEGAL.md#native-replacements-of-game-and-library-code) | S, then policy |
| RF-9 | **Cross-title mapping** (later) | FH2 v2 shares about 81 % of functions with FH1 v4 (Lavender Shift's masked hash). Map findings and types across with `tools/map-fh1-v4-addresses.py`, so the format readers can later take FH2 data. Not on the FH1 critical path | Lavender Shift LV-0.2 | S, later |

### How each format gap gets closed

| Gap | Method, in order | Done when | Stop rule |
| --- | --- | --- | --- |
| World geometry (`.rmb.bin` render models, prop placements, `.pgeo`, `.sh`) | Read trace to tie members to sectors, then frame-dump correlation of positions and indices, then mutation of suspected fields, then loader analysis in Ghidra (specification role) | One Colorado sector baked and drawn by the engine, matching a frame dump at the same camera (depth within tolerance) | **RI-0.4 timebox: 4 weeks.** If no sector draws by then, fallback B is a world harvest on the player's machine: the baker runs the player's own Pinyon Shift build along a scripted fly-through and stores the vertex and index buffers it submits. That keeps the data local, but every player would need a working recompilation to install the engine, and the harvest yields only what the route drew. If that cost is unacceptable, the programme stops at M1 |
| PVS and streaming | Read trace during scripted drives; correlate zone files with the members loaded | The engine streams the route the oracle streams, within a radius | If the PVS can't be decoded, compute our own visibility at bake time. That is allowed, since we only need a playable result |
| Collision (the `.fiz` tiles indexed by `Colorado_track_00.col`, done for M3 in [COLLISION.md](formats/COLLISION.md); left: `PhysicsDefinitions.bin` prop shapes; `.rmb.bin` is render geometry, RI-0.4) | Mutation (raise or remove a wall, then watch a car pass), ground-query traces from RF-5 | The ground height under four wheels matches the oracle along a recorded drive, within 1 cm | Fallback: build collision from render geometry at bake time |
| Car physics data (`physicsdefinition.bin`, `MAXData.xml`, unknown database columns) | Sweep columns and fields, measure with the physics trace (the #1019 approach) | Each field used by the M3 step has a meaning and a unit | — |
| `.fxobj` effects | Effect structure from the shader corpus and the D3D wrapper records | A material table per shader family, enough to write our own shaders | Our own shaders are a look match, not a translation, so the decoding only has to reach that level |
| `.fev` events | Structure from FSB references and FMOD's public Ex-era behaviour (*to verify* what is public) | Engine, tyre and UI events play from the engine's mixer | Fallback: drive the FSB samples from our own event tables |
| Save crypto | Not reverse engineered for shipping (see Decisions) | — | The importer runs inside Pinyon Shift |
| `.wmv` | RI-0.6 | — | Transcode at bake time |

## Stage 0: probes that size and gate the programme

About six weeks in total. Each probe decides something.

| ID | Probe | Premise | Gate | Size |
| --- | --- | --- | --- | --- |
| RI-0.1 | RF-1 first pass: probe build over the route set (free roam, race, Rally, Autoshow, paint, career menus) on v4 | The live code is far smaller than the image | A function count per subsystem with a "live" flag. That gives the size of every milestone below | S |
| RI-0.2 | Census of formats and bytes on the player's disc: members per type, bytes per type, depth known (reconciles the `.bix` and `.bin` counts) | — | One table, recorded in [files](../site/findings/files.toml) | S |
| RI-0.3 | Engine skeleton in the new repository: SDL3 window, Vulkan device, the RF-6 archive reader, a car `.carbin` plus `.xds` drawn in a viewer | The readers work outside Pinyon | A car renders on Windows and the Deck | S |
| RI-0.4 | World geometry decode, timeboxed (see the table above) | The world formats can be decoded with the RF-7 methods | One sector drawn and matching a frame dump | M, at most 4 weeks |
| RI-0.5 | Physics observability: RF-5 snapshot, inject and step for one car body. Measure the tyre-model call rate, the substeps and the units of the wheel-block fields | The body state can be captured and restored well enough to step it | A 2 s trace after injection repeats within tolerance on the oracle itself (A/A) | M |
| RI-0.6 | Media: does FFmpeg decode the disc's `.wmv` and the XMA in its FSBs (the latter through the SDK decoder already in `tools/fh1_audio_decode.cpp`) | — | Both decode, or the gap is recorded | S |
| RI-0.7 | Clean-room setup: RF-8 roles, tool permissions, the spec template and the lint | — | The first spec (the tyre model or the event restrictions) is written and implemented across the split | S |
| RF-1 | **First pass done.** `tools/re/live-code-census.py` assigns every function to a subsystem from findings, vtables, address ranges and the call graph, and flags it live from the v4 probe build's six routes (RI-0.1); it reads `probe.json` directly and reports what each route adds. Not yet: the uncovered modes in the RI-0.1 row, a v4 FMOD and XDK code range (v4 counts FMOD Ex under audio), fewer unassigned functions (10 % of live code), and the coverage page | RI-0.1 row |
| RF-6 | **Started.** The engine's `fh1data` library reads FH1 ZIP with LZX, `.xds`, `.carbin` and `.rmb.bin`, each with a corpus test on the player's disc; `docs/formats/` holds the first format spec, which the lint checks. Not yet: the round-trip tests, the share-of-bytes report per format, and moving the Python readers over | RI-0.3, RI-0.4 rows |
| RF-7 | **Started.** (c) Frame-dump correlation ties render models to draws (`world-geometry.py match-dump`). (a) A file-open log and full physical and virtual memory snapshots (`pinyon_shift_snapshot_virtual`, default off) located the prop placements. (b) The mutation runner is not built yet | RI-0.4 row |

The programme continues past Stage 0 only if RI-0.4 passes or fallback B
gives a plan for the world. Without a world there is no game.

## Engine milestones

Each milestone is a vertical slice that can be played, with a gate.
Performance is measured on the yardstick devices (Steam Deck LCD, Odin 2
Portal, Intel UHD 620, the RTX 4080 desktop) with the methods of
[NEAR_NATIVE_RESEARCH.md](NEAR_NATIVE_RESEARCH.md#verification).

| ID | Slice | Needs | Gate |
| --- | --- | --- | --- |
| M1 | **Car viewer:** every base, v4 and DLC car baked and drawn in a showroom with our own car shader, paint colours and livery masks | RF-6 cars and textures, RI-0.3 | Each car matches an Autoshow capture side by side, judged by eye with a PSNR report; the bake runs in a fixed time. Metric (set 2026-11-19): references at 1280x720 or more, one per showable car at the camera read from that frame's own dump ([AUTOSHOW_REFERENCE](formats/AUTOSHOW_REFERENCE.md)); the engine draws at that camera and size; the report gives, per car, PSNR inside the car mask (the pixels the game's car draws cover), silhouette IoU of the two masks, and an edge-overlap score. Pass: silhouette IoU at least 0.97 for every car (geometry, wheels and placement), masked PSNR reported for all and no car more than 6 dB under the median (no outlier material or paint bug), and every contact sheet judged by eye. Priority (user, 2026-11-20): playable first, polish later. The gate is geometry and a recognisable car; matching the stage's lighting, lamp glow and post chain is polish and does not hold M1 or later milestones back. So a car passes on IoU with the largest reference-only region removed (IoU-g, its size reported as glow px), and every car under 0.97 on plain IoU gets a by-eye check of its sheet that the removed region is glow or glare, not a missing part. Bake: every model and texture of the 240 baked in at most 10 minutes on multivac, and two bakes give byte-identical output |
| M2 | **World viewer:** Colorado streamed, with a fly camera, time of day, sky and the post chain; GPU-driven culling | RI-0.4, PVS or our own visibility | **The programme gate.** The same camera path as an oracle route: frame time and CPU on the Deck, Odin and UHD 620. Proposed targets, to be confirmed at Stage 0: Deck process CPU under 8 ms for the world (today the front end alone is about 24 ms); UHD 620 under 33 ms of GPU at 1x (1280x720) with a low preset, against about 78 ms today at LOW-SPEC 60, 1x ([limits](../site/pages/limits.md)). Fail: rethink the renderer before M3 |
| M3 | **Drive:** one car on the world's collision with the engine's physics, the HUD speedometer, engine sound | RF-5, RF-4 car-physics spec, collision decode | `tools/physics-behaviour.py compare` against the oracle: 0-100 and lap time within 2 %, along with a play test. The physics module also runs in Pinyon Shift through its adapter and passes the same gates there |
| M4 | **Free roam:** traffic, the radio, a minimal map, discount signs, barn finds, the camera set | AI and traffic specs, audio events | Traffic density and a drive with the radio play-tested; save and load of position through the engine's own save |
| M5 | **Events:** races against AI on the 243 routes, restrictions, rewards, rubber-banding | Flow, AI and event-rules specs; the AI line format | AI mean distance within 3 % and finishing order statistics against the oracle over 10 events |
| M6 | **Festival and career:** wristbands, XP and credits, garage, upgrades, Autoshow, paint, the 1000 Club, Rally, DLC packs; save import from Pinyon Shift | UI rebuild, profile spec, liveries | The reference save imports and shows the same progress (signs, cars, DLC) |
| M7 | **Release:** Windows, Linux and Deck, macOS, Android, settings, mods | Everything | The player pages and install steps on the site; the binary-content check passes (no guest code, no game data) |

Pinyon Shift stays the release until M6 passes.

## Verification

- **Formats:** corpus tests on the player's disc in CI-like local runs,
  never with game data in the repository, and a byte-coverage report per
  format.
- **Behaviour:** short-horizon injection and trace comparison (RF-5), with
  tolerances per field set by an A/A run of the oracle, because the oracle
  is not deterministic either.
- **Images:** frame dumps of the same route and camera. A look match is
  judged by eye with a PSNR report, not byte-exact.
- **Audio:** the `audiocapture` method of isolating categories and
  comparing envelopes, not whole-mix RMS.
- **Saves:** the field tree of a profile written by the engine against one
  written by the oracle for the same actions.
- **Engine self-consistency:** the engine is deterministic for a given
  input log, so a replay is its own regression test. That is the oracle
  CS-1 could not provide for the recompilation.

## Risks

- **The world formats.** They are undecoded, have no public work, and are
  40 % of the data. This is why RI-0.4 is timeboxed and the programme gate
  depends on it.
- **Scale.** OpenRCT2 took about 2.7 years to stand alone, with a
  community and an exe whose layout matched. OpenMW is still before 1.0
  after 17 years. Here there are 439 flow behaviours and about 300 UI
  scenes. RI-0.1 has to show how much of it is live, and the milestones
  only ask for what play needs.
- **Handling feel.** Without the oracle, OpenNFS never reached faithful
  handling. M3's gate and the Pinyon adapter address this. "Efficiency over
  parity" stops at handling the player can feel.
- **Legal.** Microsoft owns the game and the brands of the licensed cars.
  The research found
  that console licence terms that forbid reverse engineering were enforced
  in Davidson v. Jung
  ([opinion](https://law.resource.org/pub/us/case/reporter/F3/422/422.F3d.630.04-3654.html)).
  re3, which replaced the original function by function, was sued and
  settled. The clean-room split (RF-8) and the decisions below are the
  mitigation.
  This page is not legal advice.
- **The cost of the oracle.** RF-5 adds hooks to Pinyon Shift. They have to
  stay off by default, as the CS probes are, so the shipping build does
  not change.

## Decisions for the maintainer

1. **Repository and licence.** Recommendation: a new public repository,
   separate from `pinyon-shift`, so its history never contains guest code.
   The licence is BSD-3 like the other ports, or GPL-3 like OpenRCT2 if
   forks must stay open.
2. **Name.** Following the project's naming, a new name rather than
   "Pinyon Shift", and nothing using "Forza" or "Horizon" (trademarks).
3. **LEGAL.md for shipped binaries.** Proposed additions:
   - binaries contain only code written from specifications;
   - no microcode, constant tables or other data taken from the
     executable;
   - packs are built on the player's machine and never distributed;
   - no keys.
4. **Save keys.** The game encrypts its saves with `XeKeysAesCbcUsingKey`.
   The obscured key comes from the game, which makes it game data. The SDK
   de-obscures it with an obfuscation key it carries
   (`thirdparty/shiftglue-sdk/src/kernel/xboxkrnl/xboxkrnl_crypt.cpp`).
   Proposed: never ship either key in the engine. Import saves through
   Pinyon Shift instead: it decrypts them inside the running game, and the
   engine then writes its own unencrypted format. The cost: a player who
   wants their old save carried over has to run Pinyon Shift once.
5. **Clean room.** Whether the specification and implementation roles must
   be different people or agents with different tools (recommended), or
   only kept apart by process.
6. **Behaviour target.** v4 (recommended) or base. v4 means the
   specifications follow the v4 build, while findings stay keyed by base
   address with v4 addresses beside them. Today 332 of the 572 findings
   carry a v4 address. The remaining 240 have to be mapped
   (`tools/map-fh1-v4-addresses.py`) before they can be used in the
   specifications.
7. **Scope.** Whether online play, the cut modes and new content are in
   scope after M7. The recommendation is to keep them out until M6
   passes.

## Progress

| Item | Outcome | Evidence |
| --- | --- | --- |
| RI-0.1 | **Pass (2026-11-19).** A v4 probe build ran six routes, each checked by its captures: free roam (a drive), a race (event entry, car select, racing), the Autoshow (buy a car through to the save), the paint shop (Paint Car and the colour picker), a Horizon Rally stage entered from the pause menu and driven, and the festival menus (pause list, settings, My Profile, the map). Together they enter **42,603 of 81,084 functions (52.5 %), 1.87 M of 3.80 M guest instructions (49 %)**. Free roam alone enters 37,217. The others add 5,386 functions and 246 K instructions (+15 %): the race 3,052 (UI, flow, rendering, AI), the Autoshow 954 (UI 40 %, rendering 36 %), paint 549 (rendering 47 %, UI 40 %), Rally 662 (flow 28 %, rendering, UI), the menus 169 (UI 63 %). The earlier v4 drive list adds only 35 more, so the set is close to saturated for these modes. By share of entered instructions: rendering 27 %, flow and states 17 %, UI 17 %, unassigned 10 %, audio with FMOD Ex 9 %, AI 5 %, data 4 %, world 3 %, physics 3 %, online 3 %, shared 3 %, core 1 %. Taking out the libraries a native engine replaces (FMOD Ex, the XDK and D3D code, 12-18 % of the base race), the game code to specify is about 1.55-1.65 M live guest instructions. Not covered: the decal and vinyl editor, the contents of the menu tabs, Message Center (offline), photo mode, the 1000 Club, DLC events and online. The paint shop is reached through a local test mod that widens its trigger zone; the routes and seeds are local | `tools/re/live-code-census.py` (per-route additions); [executable](../site/findings/executable.toml) "live code per subsystem"; `.local/re/census-v4-routes.json`, `.local/re/probe-v4/` (routes, runs, `probe.json` per run); probe build `out/build/win-amd64-v4-probe` |
| RI-0.2 | **Done (2026-11-19).** The base disc has 260,797 files and archive members: 18.46 GB uncompressed and 7.16 GB stored, but only 70,019 distinct contents (11.0 GB). Colorado's `bin.zip` repeats most of its files up to 34 times: its 230,057 entries hold 10,468 distinct `.bix` names. That resolves the count conflict (the "about 230,000 `.bix`" was every entry of every type, and is now corrected). The plan's "world formats ≈ 40 %" holds for stored bytes only (40.8 %). Counted once per content, the undecoded world formats are about 18 % of the data, against 43 % for textures and 16 % for car models. Formats with no decoding at all are 47.6 % of uncompressed bytes, but 17.6 % of distinct bytes | `tools/re/disc-census.py`; [files](../site/findings/files.toml) "media folder census"; `.local/re/disc-census{,-base}.json` |
| RI-0.3 | **Pass (2026-11-19).** A new local repository, `shift-engine` (provisional name, no remote), holds two parts. The first is the `fh1data` reader library: FH1 ZIP with XMem LZX through libmspack, `.xds` with every mip, and `.carbin`. The second is `carview`, an SDL3 and Vulkan 1.3 viewer with a headless PNG mode. Its corpus test reads 258,521 of the 258,537 non-deflated archive members with matching CRCs; 16 fail in the LZX decoder. All 9,656 textures decode, and all 179 car archives parse (1,883 models). It ran the same on Windows (RTX 4080, no validation messages) and the Steam Deck (RADV), and headless car renders were checked by eye on both. Wheels, glass and interiors are simplified | `shift-engine` commits `6bf0f88`..`2e2f133`; [files](../site/findings/files.toml) |
| RI-0.4 | **Passed with conditions (2026-11-19, day 1 of the 4-week timebox; the maintainer accepted the conditional pass the same day).** The scenery is drawn from the 14,291 render models `coloradoout.NNNNN.rmb.bin` in Colorado's `bin.zip`; every one parses, and their vertex and index buffers are the GPU's, unchanged. In one free-roam frame dump, 1,761 of 2,855 indexed draws tie byte for byte to a model, including all 820 made with the main view's world matrix. The engine (shift-engine `d55ed64`, written from [WORLD_GEOMETRY.md](formats/WORLD_GEOMETRY.md) alone) reads all 14,291 and draws a 1 km sector (277 models) at the dumped camera: near features such as road lines, the guard rail and rocks land within a median 1 px of the capture. Prop placements are found: kind-7 `.pgeo` tiles hold f32 instance matrices (174,803 instances) and `.pvsz` zone files hold positions with half-float rotations; together they account for all 130 props of the frame. Far terrain first missed: the skyline was a mean 8 px off, with ridges 40-50 px off. The model set differed both ways. Only 39 of the sector's 277 models were drawn by the game, and the visible ridges come from 34 models outside the 1 km box. Drawing exactly the 73 world-space models the game drew, with the dump's matrix at full precision, gives a skyline a median 3-4 px (mean 6 px) from the capture and near silhouettes 1 px. Restricting that set to the box puts the skyline back to a mean 8 px. So the format and matrix reproduce the frame given the game's draw list. Depth is measured (2026-11-19): a replay of the dump resolves the game's own main-view depth (reversed 24-bit float, 4x MSAA in three bands); with every other depth-writing draw skipped, the engine's depth of those 73 models agrees over 569,343 pixels to a median 0.024 % of view depth (6 mm) and a 95th percentile of 0.10 % (1.05 m; under 40 m: 1 mm and 8 mm), at the game's half-pixel-offset pixel centres, and coverage differs in 467 pixels. Props are resolved in the files (2026-11-19): the `.pgeo` and `.pvsz` IDs are object IDs, the object table in `Ribbon_00/Colorado_00.pvs` names each object's render model, and the `.pvsz` records are framed; for all 130 placements of the frame the table names the drawn model, its LOD set, or a byte-identical mesh, and `dump-obj --props` places 2,460 `.pgeo` instances and 845 `.pvsz` objects in the sector. The engine now draws props too (shift-engine `6ed6cdc`..`dc76af7`, from the spec alone). The first prop run (`8a2932c`) missed by a median 1.4 m on the props, mostly near metal barriers turned the wrong way because the spec's rotation wording was ambiguous; `dc76af7` reads the `.pvsz` halves by columns and puts the barrier edges a median 1 px from the capture. The gate set leaves out on both sides the alpha-tested chain-link fence and pine and the animated flag (the replay keeps the 820 world-space draws and the other props' 235 main-view draws). Over 565,092 pixels the depth then agrees to a median 0.024 % of view depth and a 95th percentile of 0.10 %; over the 18,678 prop pixels to a median 0.015 % (1.6 mm) and a 95th of 0.12 % (under 40 m: 0.014 % and 0.105 %). The prop 95th is 0.02 points over the world's 0.10 %. The excess is where the engine's own depth changes inside one pixel: of the 1,205 prop pixels over 0.1 %, 1,126 have the game's depth inside the range of the engine's four texels around the sample point, leaving 79 (0.42 % of the prop pixels, against 0.74 % for the world); where those texels lie within 0.2 % of each other the props are at 0.014 % and 0.047 %, at or under the world in every band. It is not only silhouettes (away from the prop edges the 95th is 0.107 %, and 0.23 % at 200 to 1,000 m), and which sample the game's 4x pass keeps is not measured. With every prop the world pixels hold (0.025 % and 0.10 %) and the prop pixels do not (95th 42 %: the fence and pine cut-outs and the flag's animation). The instanced clip z row costs only 0.018 % (median). Verdict (2026-11-19): **passed, with these conditions.** (1) The draw list is the game's own for that one frame, since the LOD and visibility rule is open, so "one sector baked" holds for the dumped frame's model set only. (2) Alpha-tested and animated props are left out until there are materials and animation. (3) The comparison is against the replayed (recompilation) depth, not a console readback. (4) The pass is a judgement on the prop 95th percentile, 0.12 % against the world-derived 0.10 %: the excess is in pixels where the engine's depth itself steps or slopes by 0.1-2 % and the game's value lies inside that range, so it is counted as sampling, not as a placement or shape error. Only positions are decoded in each vertex. `bin.zip` overflows its 16-bit entry count, so the archive reader on `dev` misses 10,097 names (recorded, not fixed there; the engine reader walks the directory by size) | `tools/re/world-geometry.py`, [WORLD_GEOMETRY.md](formats/WORLD_GEOMETRY.md); [files](../site/findings/files.toml), [world](../site/findings/world.toml), [rendering](../site/findings/rendering.toml); `.local/re/world/` |
| RI-0.5 | **Pass at a fixed dt (2026-11-19).** The default-off oracle `pinyon_shift_physics_oracle` snapshots the player's body (the 0x5040 bytes the step prefetches), injects it later and traces it. With a fixed 1/120 s dt, all three injections repeated the following 2 s bit for bit on all 241 steps; the negative control failed 27-32 fields. With the game's own dt the runs drift: surface edges after 1.0 s, and an impact from untimed traffic after 0.2 s. The full step runs **substeps at about 360 Hz**: n = ceil(dt × 359), clamped to 0.1 s. That corrects the earlier "no substeps" record. The tyre model runs once per wheel in contact per substep, about 1,440 calls a second per car, which fits one contact point per wheel. The wheel-block units are mapped: the body's mass is in units of 100 kg and forces in 100 N. Not covered: steering, contact with other cars, a snapshot taken at spawn (it does not restore), and where the step reads its input | `src/guest_native/car_physics.cpp`, `config/render-tests/fh1-physics-inject-aa.fh1test`, `tools/re/physics-inject-compare.py`; [objects](../site/findings/objects.toml) "Car dynamics"; `.local/runs/ri05-aa*` |
| RI-0.6 | **Pass (2026-11-19).** All 12 movies (WMV9 video, WMA Pro audio) decode fully with stock FFmpeg 8.1.1. The FSB banks hold XMA and MPEG samples only. FFmpeg's own XMA2 decoder reads the XMA samples once they are wrapped as RIFF, at their exact declared lengths, and 299 of 299 sampled across categories decoded. So the baker can transcode all media with FFmpeg. Side finding: the SDK's XMA path decodes one long ambience sample short | `tools/re/media-decode-check.py`; [files](../site/findings/files.toml), [audio](../site/findings/audio.toml); `.local/re/media-decode.json` |
| RI-0.7 | **Done (2026-11-19): two specs implemented across the split.** SPEC-EVENT-RESTRICTIONS is implemented in shift-engine `34a4804` (all 120 disc rows decode and re-encode exactly; its questions are answered in the spec), and WORLD_GEOMETRY.md in `d55ed64` (all 14,291 render models parse; the sector draw of RI-0.4). Both commits carry `Spec:` lines and pass `check-provenance.py`. Both implementers ran as sub-agents of the specification session, so the engine's `.claude/settings.json` did not apply to them: the split held by their briefs (the spec and named outputs only) and the provenance check. A real split needs the implementation side started in its own session in the engine repository. [CLEAN_ROOM.md](CLEAN_ROOM.md) defines the two roles and their tools. `kb.py lint` checks specs. The first spec, [SPEC-EVENT-RESTRICTIONS](specs/EVENT_RESTRICTIONS.md), decodes every `EventRestrictions` bit the disc uses. The widths are the only assignment of 0/1/2/4 bytes that fits all 120 values, and every decoded field matches its event's own restriction text | `docs/specs/`, `tools/re/kb.py` |
| M1 | **Pass (2026-11-20), with three cars recorded as exceptions.** shift-engine bakes all 240 models (250 `Data_Car` rows) from the disc, v4 and the 21 DLC packages: two bakes from empty folders are byte-identical (722 files) and took 45 and 74 s while captures ran beside them, against the 10-minute budget. The showroom draws each with our car shader, the colour row's paint, stripes, livery masks, the stock part set, factory liveries (`HasRaceLivery`), per-car tyres and the grille tiles. References: 222 Autoshow captures at 1280x720 (200 stock, 16 unicorns and 6 showcase or hidden rows through test-database seeds); the other 18 rows are traffic models the showroom draws only as stubs or stand-ins, matched against free-roam traffic in M4 instead. `m1-report` over the 222: silhouette IoU mean 0.986, median masked PSNR 17.9 dB (report only), 219 pass on IoU-g at 0.97. The three under it: RX-8 Mazdaspeed (0.964) and Huayra (0.970, two glow regions) are lamp glow by eye, and the FJ Cruiser (0.963) draws its own tyre about 20 px large although every sizing input matches the game's frame dump; it is deferred as polish (user, 2026-11-20: defer small blockers). All 31 cars under 0.97 on plain IoU were viewed by eye (sheets or a tile of their masks): the reference-only regions are lamp glow, apart from small polish items left open, namely the ML63 and DBR1 wheel placement a few pixels off, the F50's see-through engine cover and the F40 Competizione's rear glass (Q30) | shift-engine `8bcfb7c`..`254e86e` (report tool `d26abc9`); [AUTOSHOW_REFERENCE](formats/AUTOSHOW_REFERENCE.md), [CARS](formats/CARS.md) |
| M2 | **Performance targets met (2026-11-20); the viewer is partial.** `carview --drive --bench` drives a 60 s free-roam cruise from the festival spawn (570-750k triangles, the far ring and 39k tree sprites, streamed). Low preset, 1280x720: UHD 620 (blindvac) GPU 10.1 ms median against the 33 ms target (the recompilation needs about 78 ms), main-thread CPU 1.2 ms; Steam Deck LCD (Linux build, Steam launch) process CPU 3.97 ms per frame against the 8 ms target, frame CPU 2.9 ms median, 7.7 ms worst; RTX 4080 GPU 0.42 ms. The scenery hand-over moved off the main thread (worst frame 26-31 ms to 3-3.5 ms on blindvac). The Deck's GPU figure is an upper bound: a headless bench gets no game clocks. Not done: time of day, the post chain, GPU-driven culling, the Odin, and the oracle route's own camera path instead of our cruise. The world draws terrain layers, far terrain, tree billboards and the free-roam prop set (route lists) | shift-engine `cf831bd`, `901fb32`, `090119e`; [WORLD_MATERIALS](formats/WORLD_MATERIALS.md), [WORLD_GEOMETRY](formats/WORLD_GEOMETRY.md) |
| M3 | **In progress (2026-11-20).** The stock R34 drives on Colorado's `.fiz` collision with the CAR_PHYSICS model, a HUD speedometer and engine sound, windowed or headless; pad deadzones, a chase camera and a car picker. Launch, turn circle, braking and ABS gates pass. The lap gate's oracle is recorded (FR06 solo, 38.784 s, two runs 0.014 s apart); the engine's first replay of it brakes too little and leaves the line at the first slow corner, pending the spec of the AI's control path, brake mapping and gearbox. The play test is still to do | shift-engine `a26ceb1`..`d769452`; [CAR_PHYSICS](specs/CAR_PHYSICS.md) "Oracle lap for the M3 gate" |

## Order

1. **Stage 0** (RI-0.1 to RI-0.7), with RF-1, RF-6 and RF-7 started as
   their first users need them.
2. **RF-2, RF-3 and RF-5**, while M1 is built on the RF-6 readers.
3. **M2**, the programme gate.
4. **M3 to M6**, each preceded by its RF-4 specifications and RF-5
   scenarios.
5. **M7.**
