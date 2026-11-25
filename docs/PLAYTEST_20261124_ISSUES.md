# Playtest Issue Tracker — 2026-11-24 (`20261124-143848`)

Source playtest session: `out/playtests/20261124-143848/`
- **Duration**: `1,919.2 s` (`32 min`), `212,860` frames, **0 crashes**, `0` non-finite states, `0` audio glitches
- **Frame timing**: `p50 = 8.44 ms` (~118 FPS), `p95 = 13.47 ms`, `p99 = 21.00 ms`, `max = 364.51 ms`
- **Telemetry flags**: `930` hitches (`> 25 ms`), `235` ground anomalies, `29` user-marked bug bookmarks (`bug_001`..`bug_029`) + `2` manual screenshots
- **Test config**: `3840x2071`, `preset low` (2048px UI atlas), NVIDIA GeForce RTX 4080, Vulkan 1.3 (`DirectX 12 (RHI) / Vulkan 1.3`), `VSYNC FIFO`

---

## Summary Index

| Category | Count | Open |
|---|---:|---:|
| [1. Critical Rendering, Flicker & UI Atlas Corruption](#1-critical-rendering-flicker--ui-atlas-corruption) | 5 | 0 |
| [2. Performance, Stutters & Frame Drops](#2-performance-stutters--frame-drops) | 4 | 0 |
| [3. Audio & Lighting / Time-of-Day Bugs](#3-audio--lighting--time-of-day-bugs) | 3 | 0 |
| [4. World Geometry, VFX, Crowds, Lights & Sky](#4-world-geometry-vfx-crowds-lights--sky) | 10 | 0 |
| [5. Cars, Lamps, Driver Model & Showcase](#5-cars-lamps-driver-model--showcase) | 4 | 0 |
| [6. Physics, Collision, Smashables & Camera](#6-physics-collision-smashables--camera) | 5 | 0 |
| [7. World Map & Minimap Parity](#7-world-map--minimap-parity) | 5 | 0 |
| [8. HUD, Menus, Campaign & 1:1 UI/UX Parity](#8-hud-menus-campaign--11-uiux-parity) | 9 | 0 |
| **Total** | **45** | **0** |

---

## 1. Critical Rendering, Flicker & UI Atlas Corruption

- [x] **PT-01 — Title screen (`PressStart`) insane flicker, solid black box, and duplicated `FORZA HORIZON` logo**
  - **Priority**: P0
  - **Evidence**: User screenshot 1 (`PressStart` title screen). Large black rectangle covers top-left behind a doubled/offset `FORZA HORIZON` logo over `PressStart.wmv`.
  - **Fix (PT-A)**: Always allocate a `4096x4096` UI atlas on desktop regardless of quality preset, guard current-frame transient shelves from mid-frame eviction, and merge contiguous idle transient shelves cleanly.

- [x] **PT-02 — Settings UI text disappears and flickers massively**
  - **Priority**: P0
  - **Evidence**: `bug_015.png` / `bug_015.json` (`t=702.7s`, `-2208.31 13.43 -1535.64`, `VW_Corrado_95`). Entire left column of setting names is completely blank; only `< ON >` / slider values and right info card render.
  - **Fix (PT-A)**: `4096x4096` desktop UI atlas + 6 quantized glyph cache buckets (`12..64 px`), contiguous idle glyph-shelf merging, 8 MiB UI staging retention, and stroke-glyph fallback so text never disappears.

- [x] **PT-03 — Missing characters across HUD, menus, subtitles, and toasts**
  - **Priority**: P0
  - **Evidence**:
    - `bug_006.png`: `"QUIT RACE"` renders as `"UIT RACE"` (missing `Q`).
    - `bug_007.png`–`bug_016.png`: `"PLACE 8/8"` renders as `"PLACE 8  8"` (missing `/`).
    - `bug_017.png`: FMV subtitle `"One more thing."` renders as `"ne more thing."` (missing `O`).
    - `bug_026.png`: Holly Cruz showcase toast renders as `"New Showcase  vent unlocke :   STANG VS   STANG   0 or  ustang Boss  2 vs  -5    STANG !"` (dozens of missing glyphs).
  - **Fix (PT-A)**: Eliminated glyph-band exhaustion at 4K (`4096` atlas + 6 quantized sizes + shelf merge + stroke fallback).

- [x] **PT-04 — FMV video still shows exposed bars on left and right sides (3D world visible behind edges)**
  - **Priority**: P1
  - **Evidence**: `bug_017.png` / `bug_017.json` (`t=772.7s`, `3840x2071`, aspect `1.854` vs video `1.778`). Left and right strips show the live 3D Colorado road behind the cutscene instead of filling edge-to-edge or clean black.
  - **Fix (PT-A)**: Added `kUiFlagFullscreen` & `kUiFlagFullscreenCover` in `ui.{h,cpp}` and `renderer_ui.cpp` bypassing the 16:9 HUD viewport lock to fill physical framebuffer bounds edge-to-edge with clean cover-fill up to `1.25x` aspect.

- [x] **PT-05 — Showcase unlocked toast (`HOLLY CRUZ` / `New Showcase event unlocked`) looks broken**
  - **Priority**: P1
  - **Evidence**: `bug_026.png` / `bug_026.json` (`t=1492.1s`, `-2274.92 25.27 -4839.60`). Broken glyph rendering in bottom box, top-left objective text overlaps perf overlay / clipped, and layout does not match retail FH1 showcase unlock presentation.
  - **Fix (PT-A)**: Fixed glyph rendering in `ui.cpp` and moved perf overlay top-right (`PT-44`) so showcase unlock toast & top-left banner are crisp and unobstructed.

---

## 2. Performance, Stutters & Frame Drops

- [x] **PT-06 — World traversal frame stutters (`120 FPS -> ~100 FPS` + `930` hitches `> 25 ms`, max `364.5 ms`)**
  - **Priority**: P0
  - **Evidence**: `session.log` & `summary.json`:
    - `scenery hand-over` on main thread spikes repeatedly to `64–274.8 ms` (`t=136.8s: 111.2 ms`, `t=143.6s: 197.8 ms`, `t=183.9s: 227.1 ms`, `t=208.1s: 274.8 ms`, `t=115.2s: 177.9 ms`).
    - `930` total hitches `> 25 ms`; `p99 = 21.00 ms`.
  - **Fix (PT-A)**: Replaced doomed O(N²) eviction loops and redundant double `vkDeviceWaitIdle` in `place_world_arena()` with O(N) capacity pre-flight, O(1) `res.reset()`, O(N) `evict_world_textures()`, and pre-reserved hash tables.

- [x] **PT-07 — Multi-second freeze when loading `FESTIVAL_SECOND` (race transitions / hot-reloads)**
  - **Priority**: P0
  - **Evidence**: `session.log:152-153`: `race: FESTIVAL_SECOND via session hot-reload` -> `carview: hot swap to VW_Corrado_95 in 1959.3 ms` (nearly 2 full seconds frozen on main thread); also `946.8 ms` on post-race exit and `855.4 ms` / `814.2 ms` on car swaps.
  - **Fix (PT-A)**: Cached loaded car media to skip redundant `reload_car()`, pumped 60 FPS `LoadingScreen` during async hot-reload regathers, parallelized `RaceView::load_cars()` PI bisection, and eliminated O(N²) `clip_room()`.

- [x] **PT-08 — Severe FPS drop (`120 FPS -> ~57–63 FPS`) after race countdown and in Pause Menu**
  - **Priority**: P0
  - **Evidence**: `bug_008.png` (`63 FPS`, `CPU 13.69 ms`, `GPU 4.82 ms`), `bug_009.png` (`57 FPS`, `CPU 11.37 ms`, `GPU 5.22 ms`), plus `summary.json` top hitches (`210–364 ms` while `paused: true, in_race: true`, `CPU 9.8–12.0 ms`).
  - **Fix (PT-A + PT-B)**: Fixed 120 Hz race CPU overhead (PT-A) plus cached spectator ground heights, prebuilt pools, 5-plane frustum culling, unique-vertex skinning, and `SpectatorFrameCache` pause skip so paused/crowd frames no longer rebuild 120k vertices every frame (PT-B).

- [x] **PT-09 — World Map panning performance is very poor**
  - **Priority**: P1
  - **Evidence**: `bug_019.png` (`CPU 9.20 ms`, `GPU 9.88 ms`, `ui 5.36 ms`, `1% low 10 FPS`), `bug_022.png` (`ui 5.56 ms`, `GPU 7.12 ms`), plus `211–223 ms` hitches in `summary.json` while paused.
  - **Fix (PT-D)**: Capped internal pause-map raster to `<= 1600x900` with 8-thread parallel stripes, SWAR bilinear upscale, cached `base_cache_.rgba`, persistent `Horizon.zip` handle, parallel tile decode, and `<= 320 px` minimap raster.

---

## 3. Audio & Lighting / Time-of-Day Bugs

- [x] **PT-10 — Sound not working**
  - **Priority**: P0
  - **Evidence**: User report during playtest (`session.log` shows `radio`, `audio mixer`, `sfx engine`, `engine sound` initialized, yet user heard no sound).
  - **Fix (PT-A)**: Restored `AudioHost::ensure_device_started()` in `update_frame()` / `set_fmv_audio()`, prevented `UIExterior` from overwriting `saved_gameplay_idx_` in `SnapshotMatrix::is_transient_gameplay()`, unmuted `Bus::kMenuMusic` during active UI music, and unified overlay snapshot priority.

- [x] **PT-11 — Toggling Radio ON/OFF in Settings changes lighting / Time of Day**
  - **Priority**: P1
  - **Evidence**: User report; confirmed in `settings.txt` (`tod-preset 19`) + `apps/carview/drive_input_settings_media.cpp:172` (`if (ls.tod_preset_hour >= 0.0) app.tod_clock.set_hour(ls.tod_preset_hour);`).
  - **Fix (PT-A)**: Gated `tod_clock.set_hour` in `drive_input_settings_media.cpp` so it only applies when `ls.tod_preset_hour` actually changed.

- [x] **PT-12 — Time of Day changes too abruptly (sudden lighting jumps while driving)**
  - **Priority**: P1
  - **Evidence**: User report while driving across Colorado; also visible across timestamps (`hour 23.84 -> 3.07 -> 7.14 -> 12.27 -> 7.51 -> 19.17 -> 21.70 -> 2.07 -> 17.25 -> 5.98`).
  - **Fix (PT-A)**: Added cyclic midnight wrap in `TodCurve`, 30 s cross-fade across the 20:00 A/B preset roll, frame-rate-independent `world::TodSmoother` for dawn/dusk steps, and float-precision `world::ColorLutBlender`.

---

## 4. World Geometry, VFX, Crowds, Lights & Sky

- [x] **PT-13 — Floating white light orbs in sky near Horizon Festival and outposts**
  - **Priority**: P1
  - **Evidence**:
    - `bug_003.png` (`t=308.8s`, `-226.55 8.43 -299.84`): two bright white orbs floating high above the festival road.
    - `bug_004.png` (`-334.23 3.25 -215.94`): floating orb above streetlamp.
    - `bug_024.png` (`t=1406.2s`, `-1726.21 17.57 -3271.12`): glowing white orbs floating detached above Montano Plains outpost poles.
  - **Fix (PT-B)**: Anchored world glows to visible fixture prop AABBs with terrain clearance validation, synced draw/fade distance to `world_fade_[2..3]`, gated out bright daytime glows, and replaced harsh overbright spheres with soft atmospheric coronas.

- [x] **PT-14 — Horizon / distant terrain cutoff band near Festival**
  - **Priority**: P1
  - **Evidence**: `bug_003.png`, `bug_010.png`, `bug_020.png`, `bug_021.png`, `bug_023.png`: stark bright/flat strip right along the horizon where distant mountains/far-ring cut off sharply against the sky dome.
  - **Fix (PT-B)**: Replaced the dead-shelf atmosphere `pow` in `sky.frag` with a smooth C¹ horizon-to-zenith gradient anchored at `V.y = 0`, synchronized sky/world fog scatter (`8000 m`), and blended distant far-ring haze smoothly to `1.0`.

- [x] **PT-15 — Giant untextured square particle emitters at Festival / Crowd / Event spots**
  - **Priority**: P1
  - **Evidence**:
    - `bug_004.png` (`-334.23 3.25 -215.94`), `bug_005.png` (`-370.87 3.88 -199.35`)
    - `bug_025.png` (`-1423.89 43.12 -3610.82`, Recaro Rush)
    - `bug_029.png` (`-5424.29 73.58 -3174.18`, Mustang vs Mustang spot)
  - **Fix (PT-B)**: Fixed `_Composite_Flare*` expansion to ground flares, clamped negative buoyancy rocket acceleration, sanitized DXT1/RGB-only particle alpha masks, and added primary-luminance + circular `quad_radial` edge feathering in `particle.frag`.

- [x] **PT-16 — Static crowds, non-colliding festival barriers/fences, and missing event-site geometry**
  - **Priority**: P1
  - **Evidence**: `bug_005.png` (`-370.87 3.88 -199.35`), `bug_008.png`, `bug_029.png` (`-5424.29 73.58 -3174.18`).
  - **Fix (PT-B)**: Upgraded all standing/sitting crowd FK animations with lively cheering/clapping/waving/gaze tracking, registered unbreakable/static `.pvsz` festival crowd barriers with smooth horizontal SAT OBB collision, and restored 533 emissive buildings/booths + 146 flags/bunting.

- [x] **PT-17 — Broken/shredded geometry & alpha on tall curved Horizon feather flags and race lamps**
  - **Priority**: P1
  - **Evidence**: `bug_008.png` (`-949.98 -3.15 -586.79`), `bug_009.png` (`-950.74 -2.90 -595.72`), `bug_012.png`, `bug_013.png`. Right-side curved flags show only thin black poles with shredded red triangles floating in mid-air.
  - **Fix (PT-B)**: Routed `ANIM_FLAG_*` and `BUNTING_*` to two-sided `Blend::kCoverage`, excluded rigid `ANIM_FLAGPOLE_*` from cloth displacement, refined horizontal wind ripples, and added geometric face normals + back-face normal flipping.

- [x] **PT-18 — Static `"START"` and `"0:05"` gantry displays not animated on race start**
  - **Priority**: P2
  - **Evidence**: `bug_007.png` (`t=450.9s`, `-950.05 -3.51 -546.42`). Overhead race gantry shows frozen `"0:05"` LED panels and static `"START"` sign instead of counting down / switching to `GO` / race clock.
  - **Fix (PT-B)**: Wired `h_diff_countdown_nolight_masked_1` and `h_diff_flipbook_nolight_masked_1` to live self-lit LED dot-matrix animation stepping cleanly through `0:05`..`0:01` -> pulsing green `GO` -> `HORIZON FESTIVAL TV` plus 16-frame festival TV flipbooks.

- [x] **PT-19 — Floating building roofs / canopies mid-air**
  - **Priority**: P1
  - **Evidence**: `bug_016.png` (`t=745.6s`, `-3036.80 5.08 -1439.40`). Three dark roof canopies float high above the road with no walls/support pillars underneath.
  - **Fix (PT-B)**: Replaced float32 `+ 0.5f` flag rounding (which rounded odd `kFlagEmissiveMap | kFlagTextured = 2^23 + 1` to `kFlagSky`, projecting 533 buildings into the sky dome!) with exact `round()` / `std::lround()` across all world shaders and CPU/GPU pipelines.

- [x] **PT-20 — Cloud texture transparency / horizon band artifacts**
  - **Priority**: P2
  - **Evidence**: `bug_020.png` (`t=870.6s`, `-3147.20 13.80 -1211.58`). Dark flat clouds with harsh transparency edges over a bright cyan-white strip along the horizon.
  - **Fix (PT-B)**: Switched Mode 1 cloud compositing in `sky.frag` to true premultiplied linear Porter-Duff OVER (eliminating `rgb * a^2` dark halos), applied `SkyGain` + half-Lambert wrap, and feathered clouds smoothly into horizon mist.

- [x] **PT-21 — Upgrade discount signs do not break when hit**
  - **Priority**: P1
  - **Evidence**: `bug_021.png` (`t=1052.8s`, `-3398.42 7.99 -1348.46`). Car sits clipped inside the Upgrade Discount billboard without breaking/collecting it.
  - **Fix (PT-B)**: Lazily recreated `host.free_roam` after `start_new_career()` hot-reloads, upgraded sign detection to swept 2D XZ SAT OBB-vs-OBB with planar speed, immediately hid GPU billboard draws on hit, and added oriented shatter debris.

- [x] **PT-22 — `235` ground anomalies recorded during playtest**
  - **Priority**: P1
  - **Evidence**: `summary.json`: `"ground_anomaly_count": 235`.
  - **Fix (PT-B)**: Telemetry breakdown confirmed 0 collision holes (162 real PT-27 vertical-launch frames + 1 paused pre-race-grid frame + 72 overhead canopy/guardrail probe frames). Fixed `world::ground_height` steep-wall pass-through + hairline seam fallback and filtered paused/overhead false positives in `playtest_recorder.cpp`.

---

## 5. Cars, Lamps, Driver Model & Showcase

- [x] **PT-23 — Tail lights have an always-on red spot / flare blob**
  - **Priority**: P1
  - **Evidence**: User screenshot 2, `bug_001.png`–`bug_005.png`, `bug_020.png`, `bug_023.png`–`bug_025.png`, `bug_029.png`. Bright circular red glow blobs float over both tail lights continuously (even when cruising/accelerating with zero braking).
  - **Fix (PT-C)**: Added `cars::evaluate_car_lamp_glow_sprite()` suppressing close/mid-range running tail-light halo sprites (`0x02` and rear `0x01`) while cruising so lenses glow cleanly, reserving proportional tight housing-seated glows for braking (`0x04`/fallback).

- [x] **PT-24 — Player driver model missing head / clipped inside cabin**
  - **Priority**: P1
  - **Evidence**: `bug_010.png` (`t=532.5s`, side view of `VW_Corrado_95`), `bug_018.png` (side view of Beetle in Autoshow). Driver torso/red collar is visible but the head is missing/clipped or positioned incorrectly.
  - **Fix (PT-C)**: Generalized `world::parse_skinbin()` for `num_lods == 1` (`Driver01.skinbin`), loaded exact `Driver01.skinbin` + `Driver01_D.xds` facing forward with roof-clearance seating and hands/body triangle partitioning (cockpit vs external/Autoshow), and fixed swapped polar-cap guards in procedural fallback.

- [x] **PT-25 — Car rear spoiler floating detached behind the car**
  - **Priority**: P1
  - **Evidence**: `bug_023.png` (`t=1352.9s`, `-3025.10 1.24 -1727.87`, `VW_Corrado_95` at `135 km/h`). Corrado active rear wing deploys too high/detached with a visible gap behind the hatch.
  - **Fix (PT-C)**: Added `cars::compute_active_wing_deploy()` pivoting multi-subsection wings rigidly around a unified leading-mount hinge with aerodynamic negative downforce pitch (`-0.11 rad`) and decklid-bounded vertical travel (`2.2–4.2 cm` flush / `4.2–7.2 cm` pedestal-linked).

- [x] **PT-26 — Mustang vs Mustang (P-51 plane) Showcase not 1:1**
  - **Priority**: P1
  - **Evidence**: User report during `PLANE_RACE_001` (`bug_027` session). Plane flies straight through mountains/terrain and has no exhaust/smoke/propeller particles.
  - **Fix (PT-C)**: Added multi-station lookahead terrain-clearance flight envelope (`min_clearance_m()` + smooth anticipatory climb/descent pitch & coordinated bank), twin Merlin exhaust puffs, wingtip vortex trails, low-altitude prop-wash dust, and P-51D visual accents.

---

## 6. Physics, Collision, Smashables & Camera

- [x] **PT-27 — Physics launch car into the air on small fence/barrier crashes**
  - **Priority**: P0
  - **Evidence**: User report ("a small crash with a fence, launches the car on the air").
  - **Fix (PT-C)**: Rejected steep side-wall/bank surfaces (`normal.y < 0.65`) from wheel suspension probes, clamped bumpstop over-compression/damping and single-wheel peak load, projected steep wall body-contact normals strictly horizontally (`n_eff.y = 0`), and clamped wall-contact upward `vel_.y` spikes.

- [x] **PT-28 — Camera moves too aggressively when crashing a fence**
  - **Priority**: P1
  - **Evidence**: User report ("camera movements when crashing a fence, moves too aggressively").
  - **Fix (PT-C)**: Added collision deadzone + light-vs-heavy shaping curve, rising-edge `0.22 s` refractory cooldown on impulse layers, damped spring recovery (`>= 0.75`), bounded total collision kick offsets, and chase yaw-rate/slope slew clamps in `DriveCamera`.

- [x] **PT-29 — Crashing a road sign emits a single giant floating sign/particle instead of natural debris**
  - **Priority**: P1
  - **Evidence**: `bug_012.png` (`t=620.8s`), `bug_013.png` (`t=622.1s`), `bug_014.png` (`t=624.7s`, `-1390..-1450`). Sign appears hugely oversized / hovers in front of the camera instead of breaking cleanly with realistic scale and splinter/spark particles.
  - **Fix (PT-C)**: Added outward lateral deflection, impact-aligned tumble spin, air/spin drag, rear chase-camera corridor clearance, `2.15 m` near-lens debris cull in `showroom_vfx.cpp`, and metallic spark bursts on metal sign impacts.

- [x] **PT-30 — Default driving assists on New Campaign do not match retail 1:1**
  - **Priority**: P1
  - **Evidence**: User report ("looks like we have no ABS/traction control/etc by default on new campaign"; also note `ABS` / `TCS` icons dimmed in speedometer on `bug_001`–`bug_029`).
  - **Fix (PT-C)**: Added `drive::AssistSettings::retail_defaults()` (`ABS=ON`, `TCS=ON`, `STM=ON`, Automatic, Normal steering, Full driving line, Rewind ON, Cosmetic damage) applied on fresh profiles and `start_new_career()` while preserving customized saved settings.

- [x] **PT-31 — Reset-to-road (`R`) UX not 1:1**
  - **Priority**: P2
  - **Evidence**: User report ("reset to road UX not 1:1"). Retail FH1 uses a smooth fade/reposition with brief ghosting/invulnerability and clean camera re-center rather than an abrupt teleport snap.
  - **Fix (PT-C)**: Added `DriveSession::ResetToRoadState` with `0.25 s` fade-out -> clean road-centerline zero-velocity reposition + chase camera re-center -> `0.30 s` fade-in and `2.5 s` traffic/AI collision ghosting.

---

## 7. World Map & Minimap Parity

- [x] **PT-32 — World Map black borders when scrolling near edges**
  - **Priority**: P1
  - **Evidence**: `bug_022.png` (`t=1122.1s`). Panning near Colorado's southwest edge exposes massive solid black void covering half the screen instead of clamping camera bounds to the map texture.
  - **Fix (PT-D)**: Added `MapView::min_zoom()` and viewport-inset `MapView::clamp_camera()` wired into `MapCameraController` and `pause_map()` so zero black border void pixels can ever appear at any zoom or aspect ratio.

- [x] **PT-33 — Broken `"H"` / Horizon sign texture on World Map**
  - **Priority**: P1
  - **Evidence**: `bug_019.png` (`t=825.8s`). Horizon icon in center-left of map has corrupted noisy blue/black pixels inside the hexagonal badge.
  - **Fix (PT-D)**: Fixed signed 32-bit integer overflow in `FastCanvas::icon()` / `FastCanvas::blit()` by promoting bilinear alpha-weighted sums to `uint64_t` with clamped rounded blending and skipping `_new` dynamic symbolizers.

- [x] **PT-34 — Selecting a GPS route on the map closes the map; keybinds/controller & UX not 1:1**
  - **Priority**: P1
  - **Evidence**: User report + `bug_019.png` / `bug_022.png` (notice bottom bar has duplicate `A SET GPS WAYPOINT` and `X SET / CLEAR GPS (G)` prompts!).
  - **Fix (PT-D)**: `A`/`Enter`/Left-Click sets or toggles GPS while keeping the map open; `X`/`F` handles Fast Travel / Enter / Clear GPS with zero prompt duplication; `Y`/`Tab` centers on car; `LB`/`RB` cycles all 7 filter tabs; `LT`/`RT` zooms smoothly; `B`/`M`/`Back` closes map.

- [x] **PT-35 — Minimap missing grey road outline / background in several segments**
  - **Priority**: P2
  - **Evidence**: `bug_001.png`–`bug_005.png`, `bug_023.png`: minimap shows only the green GPS line and arrow with no grey road geometry underneath (unlike `bug_016.png` / `bug_024.png` where grey roads appear).
  - **Fix (PT-D)**: Added segment subdivision + near-plane clipping (`draw_projected_segment`) in `MapView::minimap()`, widened road ribbons with crisp dark casing for discovered & undiscovered roads, and nested GPS/race routes cleanly inside road shoulders.

- [x] **PT-36 — Driving/Racing Line UX not 1:1 (transitions too harsh, oversized chevrons)**
  - **Priority**: P2
  - **Evidence**: User report + `bug_001.png`–`bug_005.png`, `bug_010.png`, `bug_020.png`, `bug_023.png`. Chevrons are overly wide/bright and snap abruptly between green/yellow/red instead of smooth retail FH1 braking-zone gradient ribbons.
  - **Fix (PT-D)**: Added Gaussian curvature + display-speed smoothing (`sigma = 9.0 m`), corner-exit acceleration unwind, temporal `DrivingLineSmoother`, sleek `0.38 m` half-width ribbon with bumper/horizon smoothstep fades, and continuous feathered sRGB gradient chevrons in `world_frag.glsl`.

---

## 8. HUD, Menus, Campaign & 1:1 UI/UX Parity

- [x] **PT-37 — Car nameplates and world hub/event labels not 1:1 parity**
  - **Priority**: P1
  - **Evidence**: `bug_001.png`, `bug_002.png` (`S 700 CHARLIE HERB`, `S 692 PETER HOUSTON`), `bug_007.png`–`bug_009.png` (vertical stack of 7 huge boxes blocking the road ahead), `bug_024.png` (`MONTANO PLAINS` overlapping `C 395 JAMIE CAMPBELL`), `bug_025.png`, `bug_029.png`.
  - **Fix (PT-E)**: Replaced fixed-size sky-stacking boxes with compact FH1 translucent pills + class/PI micro-badges, hexagonal world hub/event markers, smooth distance scale/alpha LOD (`45..160 m`), race cap of 5 closest tags, and unified 2D screen-space overlap resolution.

- [x] **PT-38 — Popularity Rank (`250TH`) HUD badge is not animated**
  - **Priority**: P2
  - **Evidence**: User report + `bug_002.png`, `bug_003.png`, `bug_012.png`, `bug_020.png`, `bug_024.png`, `bug_026.png`. Badge pops in/out statically instead of FH1's smooth radial ring fill, rank-up flip/spin, and skill-chain pop animations.
  - **Fix (PT-E)**: Added `PopularityBadgeAnimator` & `evaluate_popularity_badge_anim()` driving smooth exponential radial ring fill, elastic `1.24x` rank-up pop + vertical rank-flip (`250TH -> 249TH`), magenta/gold ring flash sweep, and active skill-chain pulse.

- [x] **PT-39 — Start Race pre-race screen not 1:1 parity**
  - **Priority**: P2
  - **Evidence**: `bug_006.png` (`t=425.6s`). Missing authentic FH1 pre-race grid layout, event details/opponents/difficulty/assists submenus, and camera orbit polish (plus `"UIT RACE"` missing glyph from PT-03).
  - **Fix (PT-E)**: Upgraded `event_card()` with localized event titles, pink/dark metadata strip, `ui4::opt_arrow` Difficulty row, and context-sensitive right-hand `DIRTMASKS/PANEL01` card displaying the live Starting Grid / Opponent Lineup (`POS`, `DRIVER`, `CLASS/PI`), 5-tier AI Difficulty + CR bonus breakdown, and Event Briefing.

- [x] **PT-40 — Road Discovered banner texture is dark and unreadable (+ yellow artifact under car)**
  - **Priority**: P1
  - **Evidence**: `bug_011.png` (`t=565.8s`, `-706.06 45.86 -1525.05`). Bottom `ROAD DISCOVERED 2 / 216` bar renders dark charcoal text on a dark grunge plate (almost invisible), and a stray bright yellow rectangle appears beneath the car's rear bumper.
  - **Fix (PT-E)**: Replaced the `H_BARN_FIND_HUD` `ignore_zero_alpha = true` FBF path (which forced hidden zero-alpha yellow quads visible under the bumper) with a high-contrast angled FH1 discovery ribbon featuring crisp white bold text, magenta `#F90064` accent/counter chip, road icon, and animated progress bar.

- [x] **PT-41 — Starter car selection after intro qualification race is not 1:1**
  - **Priority**: P1
  - **Evidence**: `bug_018.png` (`t=789.1s`). After finishing the Corrado intro race, game drops directly into the generic Autoshow `MANUFACTURERS` screen (showing VW Beetle) instead of FH1's curated starter car / Dak & Alice onboarding flow.
  - **Fix (PT-E)**: Made `kFestivalRegistration` blocking with a dedicated Alice & Dak **WELCOME TO HORIZON • FIRST FESTIVAL CAR** onboarding card, armed `starter_autoshow_onboarding()` on handoff, and opened Autoshow directly into price-sorted `"All Affordable"` starter picks (with Alice/Dak banner & Corrado Free Roam exit).

- [x] **PT-42 — Post-Race Finish / Reward Tally screen not 1:1 parity**
  - **Priority**: P1
  - **Evidence**: `bug_027.png` (`t=1801.9s`, `FOR_MustangBOSS429_70`). Displays raw internal ID `PLANE_RACE_001` at top-left, combines credits/wristband/popularity/prize-car into a single custom dashboard instead of FH1's sequential animated Post-Race screens (`FINISHED` -> `RACE RESULTS` -> `CR EARNED` -> `POINTS EARNED` / `POPULARITY` / `PRIZE CAR`), and overlaps top-left title with perf overlay.
  - **Fix (PT-E)**: Added `race::resolve_event_display_title()` (bound to localized stringtables; `PLANE_RACE_001` -> `MUSTANG VS MUSTANG`) and implemented retail FH1's sequential animated 3-stage Post-Race flow (`RACE RESULTS` leaderboard -> `CREDITS EARNED` rolling count-up -> `WRISTBAND & POPULARITY` + `PRIZE CAR AWARDED` celebration).

- [x] **PT-43 — Autoshow UX/UI not 1:1 parity**
  - **Priority**: P1
  - **Evidence**: `bug_028.png` (`t=1855.3s`, `31,330 Cr`). Dark flat backdrop (`67 FPS`), manufacturer cards clip against car, missing authentic FH1 Autoshow UI4 carousel/grid, spec comparison, lighting/reflections, and showroom camera transitions.
  - **Fix (PT-E)**: Brightened the Autoshow studio to a cool-white cyclorama with reflective turntable, overhead softboxes, smooth shift-lens & `kPaint` camera transitions, cut stage tessellation/per-frame scans for high FPS, enforced strict non-clipping `AutoshowLayout` (bottom horizontal carousel rail, right-docked 5-bar live spec comparison with deltas, factory/custom finish & caliper strip, and purchase confirmation guard).

- [x] **PT-44 — Top-left objective banner overlaps the Performance Overlay (`F3`) and lacks proper left-margin / styling**
  - **Priority**: P2
  - **Evidence**: `bug_001.png`–`bug_005.png`, `bug_018.png`, `bug_026.png`–`bug_028.png`. Perf overlay sits directly on top of `"DRIVE TO THE HORIZON FESTIVAL"`, `"HORIZON AUTOSHOW"`, `"EARN GREEN WRISTBAND"`, and `"REWARD TALLY"`. Moving the perf overlay to top-right or bottom-left (or offsetting when active) avoids obscuring top-left FH1 headers.
  - **Fix (PT-A)**: Docked `perf_overlay` to the top-right corner so top-left FH1 objective banners and headers are never obscured.

- [x] **PT-45 — Verify build version / `SHIFT_GIT_COMMIT` freshness**
  - **Priority**: P2
  - **Evidence**: Perf overlay in `bug_001`–`bug_029` still prints `shift-engine 7aa88733 (Release)` even though `HEAD` is `537a90e5` because CMake only refreshes `SHIFT_GIT_COMMIT` on configure — ensure incremental builds or playtest scripts report the current commit cleanly.
  - **Fix (PT-A)**: Added `CMAKE_CONFIGURE_DEPENDS` on `.git/HEAD` and the active ref in `CMakeLists.txt` so `SHIFT_GIT_COMMIT` stays fresh automatically on incremental builds.

---

## Bookmark Quick Reference (`out/playtests/20261124-143848/`)

| Bookmark | Time | Car | Repro Command | Tracked IDs |
|---|---:|---|---|---|
| `bug_001` | `57.1s` | `VIP_Viper_13` | `carview.exe --drive --car VIP_Viper_13 --spawn 2303.92 113.63 548.02 -3.0964 --hour 23.84` | PT-23, PT-35, PT-37 |
| `bug_002` | `111.4s` | `VIP_Viper_13` | `carview.exe --drive --car VIP_Viper_13 --spawn 2116.20 116.28 -509.58 -3.1177 --hour 3.07` | PT-23, PT-37, PT-38 |
| `bug_003` | `308.8s` | `VIP_Viper_13` | `carview.exe --drive --car VIP_Viper_13 --spawn -226.55 8.43 -299.84 -1.1128 --hour 7.14` | PT-13, PT-14 |
| `bug_004` | `330.7s` | `VIP_Viper_13` | `carview.exe --drive --car VIP_Viper_13 --spawn -334.23 3.25 -215.94 -0.9052 --hour 7.20` | PT-13, PT-15 |
| `bug_005` | `358.5s` | `VIP_Viper_13` | `carview.exe --drive --car VIP_Viper_13 --spawn -370.87 3.88 -199.35 1.2815 --hour 7.28` | PT-15, PT-16 |
| `bug_006` | `425.6s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -950.95 -4.09 -519.02 3.0473 --hour 12.27` | PT-03, PT-39 |
| `bug_007` | `450.9s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -950.05 -3.51 -546.42 3.0988 --hour 12.27` | PT-03, PT-18, PT-37 |
| `bug_008` | `479.4s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -949.98 -3.15 -586.79 -3.0577 --hour 12.27` | PT-08, PT-16, PT-17 |
| `bug_009` | `485.1s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -950.74 -2.90 -595.72 -3.0573 --hour 12.27` | PT-08, PT-17 |
| `bug_010` | `532.5s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -930.23 7.99 -803.20 3.0281 --hour 12.27` | PT-14, PT-24 |
| `bug_011` | `565.8s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -706.06 45.86 -1525.05 -2.9544 --hour 12.27` | PT-40 |
| `bug_012` | `620.8s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -1390.84 19.18 -1598.46 -0.9023 --hour 12.27` | PT-29 |
| `bug_013` | `622.1s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -1401.35 18.74 -1590.16 -0.9023 --hour 12.27` | PT-29 |
| `bug_014` | `624.7s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -1450.99 16.72 -1558.90 -1.2434 --hour 12.27` | PT-29 |
| `bug_015` | `702.7s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -2208.31 13.43 -1535.64 2.9572 --hour 12.27` | PT-02 |
| `bug_016` | `745.6s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -3036.80 5.08 -1439.40 -1.3514 --hour 12.27` | PT-19 |
| `bug_017` | `772.7s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -3419.90 3.25 -1508.89 -1.6774 --hour 12.27` | PT-03, PT-04 |
| `bug_018` | `789.1s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -3409.59 3.34 -1508.26 2.1588 --hour 12.27` | PT-24, PT-41 |
| `bug_019` | `825.8s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -3406.83 3.37 -1508.24 1.6499 --hour 7.51` | PT-09, PT-33, PT-34 |
| `bug_020` | `870.6s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -3147.20 13.80 -1211.58 -0.2226 --hour 7.77` | PT-14, PT-20 |
| `bug_021` | `1052.8s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -3398.42 7.99 -1348.46 2.3896 --hour 8.82` | PT-21 |
| `bug_022` | `1122.1s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -3428.74 7.25 -1376.22 0.0551 --hour 19.17` | PT-09, PT-32, PT-34 |
| `bug_023` | `1352.9s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -3025.10 1.24 -1727.87 2.7083 --hour 19.45` | PT-23, PT-25 |
| `bug_024` | `1406.2s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -1726.21 17.57 -3271.12 2.2159 --hour 21.70` | PT-13, PT-37 |
| `bug_025` | `1430.5s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -1423.89 43.12 -3610.82 2.3325 --hour 22.80` | PT-15, PT-37 |
| `bug_026` | `1492.1s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -2274.92 25.27 -4839.60 -2.9304 --hour 2.07` | PT-03, PT-05 |
| `bug_027` | `1801.9s` | `FOR_MustangBOSS429_70` | `carview.exe --drive --car FOR_MustangBOSS429_70 --spawn -5429.79 71.66 -3114.25 -0.1765 --hour 17.25` | PT-26, PT-42 |
| `bug_028` | `1855.3s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -5455.79 75.44 -3215.42 0.6558 --hour 5.98` | PT-43 |
| `bug_029` | `1877.0s` | `VW_Corrado_95` | `carview.exe --drive --car VW_Corrado_95 --spawn -5424.29 73.58 -3174.18 3.1348 --hour 6.46` | PT-15, PT-16 |
