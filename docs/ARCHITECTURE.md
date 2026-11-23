# Shift Engine Architecture

This document maps Shift Engine's library layers, runtime subsystems, threading model, and module boundaries for contributors.

## Design Principles

1. **Strict clean-room separation.** All code is independently authored from format and behavioral specifications. No retail game files, extracted assets, disassembly, decompiled output, or generated translation code may ever enter the repository (verified on every commit by `python tools/check-provenance.py`).
2. **Headless-first domain libraries.** Every library under `libs/` is plain C++20 with **no dependency on Vulkan, SDL3, or any windowing system**. Physics, audio synthesis, world streaming, traffic, career progression, garage upgrades, and save files are all testable headlessly in `ctest -LE corpus` using synthetic data.
3. **Acyclic layered dependencies.** Libraries form a strict DAG from low-level file format readers up to gameplay systems and presentation. Lower layers never depend on higher layers.
4. **Explicit ownership and state.** Prefer value types, `std::unique_ptr` / RAII wrappers, and explicit context structs over file-scope mutable globals or function-local `static` caches.

---

## Library Dependency Layers

```text
Tier 5 — Applications & Presentation
  apps/carview (carview_render + carview)   apps/launcher (shift-launcher)
  apps/carbake, apps/soundbake, apps/profile-import, apps/drive, *-corpus, *-census
        |
        v
Tier 4 — Game Modes & Persistence
  shift_freeroam          shift_race                shift_profile
  (collectibles, skills,  (events, career flow,     (saves, backups,
   outposts, 1000 Club)    rally, showcases)         Xenia import, sync)
        |                       |                         |
        v                       v                         v
Tier 3 — World, Traffic & Garage
  shift_world             shift_traffic             shift_garage
  (Colorado streaming,    (road graph, AI traffic,  (garage roster, PI,
   collision, sky, VFX)    GPS routing)              upgrades, tuning)
        |                       |                         |
        +-----------+-----------+-----------+-------------+
                    v                       v
Tier 2 — Core Simulation & Audio
  shift_drive                               shift_audio
  (vehicle physics, tyres, cameras,         (lock-free mixer, engine synth,
   assists, rewind, AI, FFB/rumble)          SFX, radio, VO, acoustics)
                    |                       |
                    +-----------+-----------+
                                v
Tier 1 — Asset Resolution & Baking
  shift_cars
  (disc/TU4/DLC resolver, car catalog, .shiftbake baker/reader, mods)
        |
        v
Tier 0 — Format Readers & Rules (zero internal dependencies)
  fh1data                                   shift_data
  (ZIP/LZX, .xds, .carbin, world/PVS,       (gamedb rules & event
   .bgf/.bsg/.fbf UI scenes, fonts)          restriction decoding)
```

### Library Responsibilities

| Target | Path | Public Deps | Responsibility |
| --- | --- | --- | --- |
| `fh1data` | `libs/fh1data` | — | Pure format parsers: FH1 ZIP/LZX archives, `.xds` textures, `.carbin` geometry, Colorado scenery/PVS/materials, `.bgf`/`.bsg`/`.fbf` UI4 scenes, vector and CJK fonts, string tables. |
| `shift_data` | `libs/data` | — | Read-only game database rules and event restriction evaluation. |
| `shift_cars` | `libs/cars` | `fh1data` | Retail disc + Title Update v4 + DLC source resolver, merged car catalog, `.shiftbake` offline format, mod overlay and native mod loader. |
| `shift_drive` | `libs/drive` | `shift_cars` | Fixed-timestep car physics (`Vehicle`), tyre/suspension/aero model, driving assists, rewind ring buffer, driving line, AI drivers, 5 driving cameras, lamp/cockpit animation state, rumble/wheel/DualSense input. |
| `shift_audio` | `libs/audio` | `shift_cars`, `fh1data` | Real-time audio engine (`AudioEngine`), engine harmonic/loop synthesis, FSB4/XMA2 parsing, 4-wheel & collision SFX, radio scheduler (`dr_mp3`), co-driver/sat-nav/narrative VO, world acoustics and mix snapshots. |
| `shift_world` | `libs/world` | `fh1data`, `shift_drive` | Colorado & Rally world streaming, terrain/prop collision queries, 24-hour Time-of-Day & real-time sky assets, smashables, particles/skidmarks/debris VFX, world animations. |
| `shift_traffic` | `libs/traffic` | `shift_drive` | Road network graph, free-roam traffic simulation, GPS pathfinding and turn-by-turn navigation prompts. |
| `shift_garage` | `libs/garage` | `shift_cars`, `shift_drive` | Player garage inventory, upgrade catalog, tuning setup, Performance Index (PI) calculator, Rally kit handling. |
| `shift_freeroam` | `libs/freeroam` | `shift_traffic` | Collectibles, barn finds, festival outposts, PR stunts, speed traps/zones, road discovery, skill chains, festival roamers, 1000 Club challenges. |
| `shift_race` | `libs/race` | `shift_drive`, `shift_traffic`, `shift_data`, `fh1data`, `shift_cars` | Race session orchestration, career progression catalog, `CampaignFlow` state machine, showcases, head-to-head, Rally championship and co-driver pacenotes. |
| `shift_profile` | `libs/profile` | `shift_drive`, `shift_garage`, `fh1data` | Career save serialization, rolling backups, multi-profile management, legacy save import, LAN Wi-Fi save sync. |

---

## Runtime Architecture (`apps/carview`)

`apps/carview` is split into three CMake targets:

1. **`carview_render` (static library):** Vulkan 1.3 + SDL3 rendering engine, GPU-driven world pipeline, optional hardware ray tracing / path tracing (`SHIFT_ENABLE_RAY_TRACING`), UI4 shape/atlas renderer, platform/gamepad/touch support, frame inspector, and benchmark collector. Linked by `carview`, `shift-launcher`, and headless rendering/UI tests.
2. **`carview_hud` (static library):** Split HUD rendering (`game_hud_core`, `game_hud_race`, `game_hud_freeroam`, `game_hud_menus`), `hud_scene`, and `rally_hub`. Linked by both `carview` and the HUD/UI test suite without duplicating translation units.
3. **`carview` (executable / Android `libmain.so`):** Interactive gameplay host, CLI entry points (`--bake` showroom mode, `--drive` full game loop, `--world` scenery viewer), campaign director, and menu screens.

### Core Subsystems & Module Boundaries

To keep every translation unit concise, single-responsibility, and readable (<960 LOC), the major runtime subsystems are organized along these boundaries:

#### 1. Vulkan Backend (`Renderer`)
- **`renderer_device`:** Vulkan instance/surface/physical-device selection, feature & RT capability probing, logical device & queues, VMA allocator (`VMA_IMPLEMENTATION`), pipeline cache persistence, VRAM telemetry.
- **`renderer_resources`:** VMA buffer/image allocation helpers, one-shot transfer submission, render targets, depth/MSAA targets, multi-cascade shadow map atlas.
- **`renderer_swapchain`:** Swapchain creation/resize, frame-in-flight slots, GPU timestamp collection, swapchain capture readback.
- **`renderer_frame`:** `draw_frame()`, `render_offscreen()`, `render_timed()`, and HDR pattern / present orchestration.
- **`renderer_modern_setup` & `renderer_modern_record`:** Screen-space & modern passes — XeGTAO/SSAO, SSCS, Hi-Z SSR, volumetric fog/sun shafts, motion vectors, tile motion blur, FXAA, spatial/temporal upscaling, HDR10/scRGB output, frame inspector preview.
- **`renderer_post`:** Scene pass orchestration (`record_scene()`), HDR resolve, 6-level bloom pyramid, adaptive auto-exposure, 3D LUT color grading, optical post-FX.
- **`renderer_ui`:** UI4 instanced quad/glyph/image pipeline, dynamic 4K/2K UI texture atlas staging, CPU overlay panel blits.

#### 2. Scene & World GPU Host (`Showroom` + `WorldGpu` + `RtScene`/`RtPipeline`)
- **`showroom_car_load`, `showroom_car_state`, `showroom_pipelines`, `showroom_upload`:** `.shiftbake` model loading, part assembly, wheel/cockpit/lamp/paint/livery/damage state, graphics pipeline creation, and batched GPU texture/geometry uploads.
- **`showroom_world_stream` & `showroom_world_gpu`:** Background world staging, arena placement (`WorldGpu`), GPU culling tables, deferred GPU resource retirement/reaping, LRU world texture cache & descriptor sets.
- **`showroom_shadows`, `showroom_env`, `showroom_sky`, `showroom_lights`:** 2/3-cascade CSM placement & shadow recording, dynamic reflection cubemap & rearview mirror targets, 24-hour ToD sky & starfield, projected car headlights & glows.
- **`showroom_vfx` & `showroom_actors`:** Particles, skidmarks, smashable debris, seated driver mesh, and race/traffic actor updates/draws.
- **`showroom_draw`:** Frame uniform updates, Z-prepass, contact shadows, instanced car draws, opaque & blended world draw recording.
- **`world_gpu` / `world_gpu_logic`:** Persistent vertex/index arena allocator and compute frustum/Hi-Z occlusion culling.
- **`rt_scene` / `rt_pipeline` / `rt_logic`:** BLAS/TLAS acceleration structures, Tier-1 hybrid ray-query reflections/shadows/AO, real-time & Photo Mode path tracing with ReSTIR DI + SVGF denoiser.

#### 3. HUD & UI4 Screens (`carview_hud`, `ui`, `ui4_chrome`, `*_page`, `*_view`)
- **`ui` & `ui4_chrome`:** Low-level 2D batched shape/text/hit-zone list (`ui::List`), vector/CJK glyph rasterization, FH1 tilted-blade chrome primitives, controller/keyboard glyphs.
- **`game_hud_core` / `game_hud_race` / `game_hud_freeroam` / `game_hud_menus`:** Split HUD builders for tachometer/speedometer & unit formatting, race HUD & results/ceremonies, free-roam skill chains/GPS/minimap/PR stunts/H2H, and pause/title/career/sponsor panels.
- **Screen controllers:** Self-contained screen modules (`autoshow`, `upgrade_shop`, `hub_scene`, `livery_editor`, `events_page`, `career_page`, `club_view`, `rally_hub`, `settings_page`, `mods_page`, `save_sync_ui`, `photo_mode`, `replay_view`, `map_view`, `campaign_ui`, `fmv_player`).

#### 4. Drive Runtime Shell (`showroom_main` -> `DriveApp` -> `DriveWorldHost` -> `DriveSession`)
Rather than capturing hundreds of stack locals inside one monolithic function, the drive runtime separates configuration, persistent application state, per-world host state, per-session gameplay state, and loop phases:
- **`showroom_main`:** Thin 32-line `run_drive()` entry point wiring `DriveApp` and `DriveWorldHost`.
- **`showroom_mode`:** Standalone `--bake` / `--reference` car showroom mode with local `LightSettings`.
- **`drive_cli`:** `DriveConfig`, CLI + `Settings` parsing, preset & benchmark scenario defaults, headless UI audit dumps.
- **`drive_helpers_runtime` & `drive_camera_input`:** Camera rig & projection state (`CameraProjectionState`), input bindings & gamepad axes, world texture loader, post/shadow XML readers, RAII UI font cache (`UiFontCache`).
- **`drive_app`:** `DriveApp` (persists across car/world reloads: SDL window, saves, garage, career progression, audio host, ToD, collectibles, outposts, Horizon Rally state).
- **`drive_world_host`:** `DriveWorldHost` (persists across hot car swaps: `Renderer`, `Showroom`, world streamer, collision, VFX systems, frame pacer, `CameraProjectionState`).
- **`drive_session`:** `DriveSession` (active vehicle, race/traffic/festival views, rewind buffer, menus, overlays, session init/finalize/achievements/save).
- **`drive_simulation`, `drive_world_sync`, & `drive_club`:** Fixed-timestep physics, free-roam & 1000 Club stepping, world placement, replay/promo/rewind sync.
- **`drive_hud_sync` & `drive_map_radio`:** Per-frame HUD, outpost hub, world markers, minimap rendering, and radio popup sync.
- **`drive_menus_dispatch`:** Menu open/close transitions, garage/autoshow/upgrade/paint/events/map/career actions.
- **`drive_input_loop`, `drive_input_world_screens`, `drive_input_hub_screens`, `drive_input_overlay_screens`, & `drive_input_settings_media`:** SDL3 event pump, driving input sampling, and modular modal screen dispatch (campaign/title/grid/map/events/outpost, garage/autoshow/shop/rally/career/club/paint, trainer/mods/save-sync, settings/photo/replay/rewind).
- **`drive_benchmark_runner` & `drive_headless_runner`:** Deterministic `--bench-scenario` sweeps, world reference captures, headless demo captures, and scripted offline drives.
- **`drive_render_loop` & `drive_frame_render`:** Interactive windowed loop (`run_window_loop`) and audio/camera/HUD/present frame execution (`render_window_frame`).

---

## Frame Lifecycle & Threading Model

Shift Engine uses explicit, bounded concurrency with clear thread ownership:

1. **Main / Render Thread:** Owns the SDL3 window and event pump, fixed-timestep vehicle/world/AI/race simulation, UI4 layout, Vulkan command buffer recording (`kFramesInFlight` slots, default 2), and swapchain presentation.
2. **World Streaming & Resource Reaper Workers:** `WorldView` gathers upcoming Colorado cells off-thread, `Showroom::stage_world()` prepares host-visible staging buffers and texture images, and a dedicated reaper thread destroys retired GPU objects only after `frames_done >= retire_tag`.
3. **Dedicated Audio Thread (`AudioHost` / `AudioEngine`):** Runs real-time mixing and synthesis driven by lock-free SPSC command, telemetry, and decoded-sample queues. The audio callback never blocks on disk I/O or mutexes; background decoder threads decode FSB4/XMA2/MP3 assets and push PCM buffers across SPSC queues.
4. **Mod & Sync Background Workers:** Save sync and mod texture staging run off the render thread and synchronize state back at frame boundaries.

### Typical Windowed Frame Order

```text
SDL_PollEvent + Gamepad/Touch Input
  -> Menu / Overlay / Campaign Dispatch
  -> Fixed-Timestep Simulation (Vehicle, AI, Traffic, Race, Free-Roam, VFX, Smashables)
  -> Audio State Snapshot Push (lock-free SPSC)
  -> World Streaming Poll & Arena Upload
  -> Camera Update + Frustum/Shadow/Reflection Placement
  -> UI4 HUD & Minimap Build
  -> Renderer::draw_frame():
       1. SceneHooks::before (UBOs, sky, lights, GPU world cull, dynamic env/mirror)
       2. Shadow Map Pass (2- or 3-cascade D32 CSM)
       3. Optional Z-Prepass + Main HDR Scene Pass (MSAA color + depth resolve)
       4. Hi-Z Pyramid + Soft Particles / Glows Pass
       5. Modern Passes (SSAO, SSCS, Hi-Z SSR, Volumetrics, Motion Vectors)
       6. Optional Hardware RT / Path Tracing Pass
       7. Post Chain (Bloom, Auto-Exposure, 3D LUT, Tonemap, Optical FX)
       8. Anti-Aliasing / Upscaling / HDR10 or scRGB Output
       9. UI4 Layer + CPU Overlays + Optional Frame Capture
      10. Queue Submit + Swapchain Present
```

---

## Offline Tools & Launcher

| Target | Path | Purpose |
| --- | --- | --- |
| `shift-launcher` | `apps/launcher` | Cross-platform native SDL3 + Vulkan GUI launcher (disc setup, mod management, settings, Android LAN Wi-Fi Sync, play). |
| `carbake` | `apps/carbake` | Offline `.carbin` + `.xds` -> `.shiftbake` car baker. |
| `soundbake` | `apps/soundbake` | Offline engine audio loop baker (optional FFmpeg decode). |
| `profile-import` | `apps/profile-import` | Imports legacy save profiles into Shift Engine's native format. |
| `drive` | `apps/drive` | Headless driving physics & telemetry CLI harness. |
| `*-corpus`, `*-census` | `apps/*-corpus`, `apps/*-census` | Verification tools that validate format readers against a local retail disc. |

---

## Testing & Quality Gates

- **CI synthetic unit tests (`ctest --test-dir build -LE corpus --output-on-failure`):** Fast, deterministic tests using strictly synthetic in-memory data. Every library and UI/render logic module has unit test coverage here.
- **Local-disc corpus tests (`-L corpus`):** Automatically skip with exit code `77` when `SHIFT_GAME_ROOT` is unset.
- **Formatting & editor rules:** Defined in `.clang-format` (C++20, 2-space indent, 120-column limit, left-aligned pointers) and `.editorconfig` (UTF-8, LF except `.ps1`).
- **Clean-room & static checks:**
  - `python tools/check-provenance.py` — blocks guest addresses/names or generated translation code.
  - `python tools/check-binary-content.py` — checks packages against local disc trees.
  - `python site/build.py` — builds and link-checks the documentation site.
