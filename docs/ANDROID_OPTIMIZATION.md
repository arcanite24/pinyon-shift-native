# Android Performance, Memory & Graphics Optimization (Odin 2 / Adreno 7xx)

This document records the measured Android profiling data, driver comparisons,
implemented optimizations, presets, and remaining opportunities on the AYN Odin 2
Portal (Qualcomm Snapdragon 8 Gen 2, Adreno 740, 1080p 120 Hz HDR10 OLED,
Android 13 / API 33, 8 GB RAM).

## Target Device & Measurement Method

- **Device**: AYN Odin 2 Portal (`bd89bfde`), Snapdragon 8 Gen 2 (Adreno 740),
  1920x1080 120 Hz HDR10 OLED.
- **Scenarios**:
  - `noon_cruise` (300 frames, 1080p target, deterministic physics & camera,
    restoring `save.txt` before every run so camera/world position is identical).
  - `race_pack` (300 frames, 7 AI opponents + focus car).
  - `midnight_lights` (300 frames, night lighting & dynamic headlights).
  - Live windowed verification (`--window-frames 180`, 1080p 120 Hz swapchain).
- **Note on headless vs live FPS**: `--bench-scenario` uses headless offscreen
  readback (`vkCmdCopyImageToBuffer` + fence wait per frame), which adds ~2.5–3 ms
  per frame vs live swapchain presentation. Live windowed FPS is ~15–25% higher
  than headless FPS.

## Vulkan Driver Shootout (Adreno 740, `smooth-60`, `noon_cruise`)

We benchmarked the stock Qualcomm Vulkan driver against multiple open-source
Mesa Freedreno Turnip releases loaded via `libadrenotools` before
`vkCreateInstance()`:

| Driver | Config | `smooth-60` Headless FPS | Notes |
| --- | --- | ---: | --- |
| Stock Qualcomm (`744.x` / `800.x`) | default | 31.1 FPS | No `VK_KHR_fragment_shading_rate` benefit; heavier TBDR driver overhead |
| Mesa Turnip `Weab-chan` (25.x) | gmem (default) | 43.2 FPS | Clean rendering, stable |
| Mesa Turnip `K11MCH1` (25.2) | gmem (default) | 44.5 FPS | Good baseline |
| Mesa Turnip `MrPurple T30` | gmem (default) | 45.1 FPS | Slightly faster command submission |
| **Mesa Turnip `MrPurple T30`** | **`TU_DEBUG=sysmem`** | **71.2 FPS** | **+58% over gmem, +129% over stock Qualcomm** |

**Why `TU_DEBUG=sysmem` wins on Shift Engine**:
Shift Engine's render graph runs multiple render passes per frame (shadow atlas,
depth prepass, reflection cubemap face, opaque/cutout/transparent world, bloom
pyramid, tone-map/upscale, UI). Under Turnip's default tiled `gmem` binning,
binning pass overhead + tile load/store barriers across 8+ passes costs more than
Adreno 740's high-bandwidth LPDDR5X sysmem path. Setting `TU_DEBUG=sysmem`
automatically whenever bundled Turnip is active delivers ~+58% FPS across all
presets.

**Custom Turnip cross-build**:
`assets/drivers/turnip/libvulkan_freedreno.so` is bundled automatically by
`tools/android-apk.py`. To compile a custom or patched Mesa Turnip build from
source for `aarch64-linux-android29`, run `tools/build-turnip-android.sh` on
Linux / WSL2 with Android NDK r26+.

## Curated Android Presets

Five presets are exposed in `Settings -> Graphics -> Graphics Preset` (and CLI
`--preset <name>`), tuned specifically for handheld/mobile GPUs:

| Preset | Resolution Scale | FPS Cap / Refresh | Shadows | VRS | Reflections | Target Use |
| --- | ---: | ---: | --- | --- | --- | --- |
| `low-power` | 0.50 (540p) | 60 Hz | Off | 2x2 | Off | Lowest RAM/battery, entry-level Android |
| `performance-120` | 0.67 (720p) | 120 Hz | 512 (2-cascade) | 2x2 | Low (128, every 8th) | 120 FPS handheld play (Odin 2 Portal) |
| `smooth-60` (Default) | 0.67 (720p) | 60 Hz | 1024 (2-cascade) | 2x2 | Medium (128, every 4th) | Balanced 60 FPS locked |
| `quality-30` | 0.85 (918p) | 30 Hz | 2048 (3-cascade) | Off | High (256, every 2nd) | Sharp high-detail handheld |
| `ultra` (Best Graphics) | 1.00 (1080p) | 30 Hz | 2048 (3-cascade) | Off | Ultra (256, every frame) | Native 1080p, full post/reflections/HDR |

## Optimization Pass 1 (Commit `177d9213`)

1. **Bundled Mesa Turnip T30 + `libadrenotools` + `TU_DEBUG=sysmem`** (overrideable
   with `SHIFT_ANDROID_TURNIP=0` or `SHIFT_ANDROID_TU_DEBUG=...`).
2. **Adreno TBDR render-pass discipline**: `LOAD_OP_DONT_CARE` / `STORE_OP_DONT_CARE`
   on transient depth/MSAA targets, `VK_KHR_fragment_shading_rate` (2x2 VRS) on
   performance/balanced presets, `R11G11B10` HDR scene target (halving bandwidth vs
   `RGBA16F`), and reduced post/bloom/reflection cost.
3. **Android build & platform tuning**: ThinLTO (`-flto=thin`), `-O3` + `-ffp-contract=fast`,
   ADPF `APerformanceHintManager` frame-time reporting, persistent Vulkan pipeline
   cache, `mallopt(M_DECAY_TIME, 1)` + `trim_android_memory()`.
4. **Texture & memory caps**: 256–1024 px texture caps per preset, ASTC/BCn
   streaming caps, aggressive staging buffer release.

## Optimization Pass 2 (Closing All Profiled Gaps)

After Pass 1, profiling exposed six concrete remaining gaps — all six have been
implemented:

1. **Car LOD1 for Traffic & AI Opponents (`ShowroomOptions::prefer_lod1`)**:
   Previously, every AI opponent and traffic car loaded LOD0 + full cockpit
   geometry (~35–60k tris each), making `race_pack` drop to 30.1 FPS on
   `smooth-60`. On Android, traffic and AI opponents now select exterior LOD1
   (`exterior_real`) with no interior cockpit mesh, and tightened traffic culling
   distances (`14 m` / `25 m` mid, `80 m` / `120 m` far).
2. **Shadow Cascade Amortization on Android (`showroom_shadows.cpp`)**:
   Near cascade (cascade 0, high-frequency car/road shadows) updates every frame;
   Mid (cascade 1) and Far (cascade 2) alternate on even/odd frames using
   `LOAD_OP_LOAD` + `vkCmdClearAttachments` scoped to the updated tile rect,
   cutting shadow-pass draw calls by 33–50% with zero shadow-edge flicker.
3. **Cutout `1x1` VRS Preservation (`world_coverage_`)**:
   When 2x2 VRS is active (`low-power`, `performance-120`, `smooth-60`), opaque
   terrain/roads/sky shade at 2x2 while alpha-tested cutout geometry (fences,
   foliage, signs — `world_coverage_`) resets VRS to `1x1` via
   `Renderer::reset_vrs(cmd)` so thin chain-link fences and leaves stay razor-sharp.
4. **FP16 (`mediump` / `RelaxedPrecision`) + `kMobileFast` Shader Specialization**:
   Added `layout(constant_id = 2) const bool kMobileFast` to `world_frag.glsl` and
   `showroom_frag.glsl` (bound on Android low-memory / fast presets) plus `mediump`
   qualifiers on hot lighting accumulators (`Adreno 740` executes FP16 at 2x ALU
   rate). `kMobileFast` skips 6-tap wheel spin blur, procedural `fbm` damage
   noise, hex-tiling / road modulate layers, and caps dynamic night lights to 2.
5. **Android HDR10 OLED Color Mode & Live 60↔120 Hz Refresh Switching**:
   `DriveActivity.java` enables `ActivityInfo.COLOR_MODE_HDR` on API 26+ HDR
   displays and exposes `setPreferredRefreshRateHz(int)` over JNI
   (`carview::set_android_refresh_rate_hz`), invoked automatically when the user
   switches presets or FPS caps at runtime (no restart required).
6. **CPU RAM Compaction (~250–350 MB Saved)**:
   - **Audio `int16_t` PCM compaction (`PcmSample::compact_to_s16()`)**: On
     Android, all decoded SFX (278 FEV + 344 UI4) and Ambience (45+ regional &
     rally `AMB_*.fev`) loops are stored as `int16_t` instead of `float32`,
     halving audio heap RAM with transparent conversion in the mixer.
   - **Post-upload CPU World Mesh Release (`world::Streamer::release_render_mesh()`)**:
     After `prepare_()` builds GPU staging buffers and submits them to Vulkan,
     Android frees `world::Mesh` render vertex arrays (`normals`, `uv0..2`,
     `colours`, `materials`, non-road `positions`) while keeping road-only
     positions for `find_spawn()` / `recover()`. Leaving a hub room
     (Autoshow/Garage/Paintshop) transparently regathers Colorado on demand.

## Measured Results (Odin 2 Portal, 1080p Target, Bundled Turnip T30 + `TU_DEBUG=sysmem`)

All benchmark cases below use `--bench-scenario` (headless offscreen readback,
which adds ~2.5–3 ms/frame over live swapchain presentation) with `save.txt`
restored to the exact same position before every run:

| Preset / Scenario | Pass 1 (`177d9213`) | Pass 2 (`ffe3ebd4`) | **Pass 3** | Total FPS Gain | Pass 1 → **Pass 3 RSS** (Peak) | Pass 1 → **Pass 3 VMA** |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `low-power` (`noon_cruise`) | 74.3 FPS | 84.1 FPS | **85.3 FPS** | **+14.8%** | 1,682 → **1,372 MiB** (1,381) | 219 → 220 MiB |
| `performance-120` (`noon_cruise`) | 79.4 FPS | 83.7 FPS | **85.0 FPS** | **+7.1%** | 1,554 → **1,328 MiB** (1,523) | 229 → **220 MiB** |
| `smooth-60` (`noon_cruise`) | 71.2 FPS | 75.5 FPS | **76.6 FPS** | **+7.6%** | 1,512 → **1,257 MiB** (1,715) | 248 → **236 MiB** |
| `quality-30` (`noon_cruise`) | 35.4 FPS | 44.8 FPS | **43.9 FPS** | **+24.0%** | 1,684 → **1,366 MiB** (1,907) | 390 → **341 MiB** |
| `ultra` (`noon_cruise`) | 24.5 FPS | 32.1 FPS | **33.1 FPS** | **+35.1%** | 1,702 → **1,397 MiB** (1,922) | 474 → **389 MiB** |
| `race_pack` (`smooth-60`, 8 cars) | 30.1 FPS | 38.8 FPS | **39.3 FPS** | **+30.6%** | — → **1,551 MiB** (1,781) | — → 342 MiB |
| `race_pack` (`performance-120`, 8 cars) | — | 41.0 FPS | **42.0 FPS** | — | — → **1,481 MiB** (1,801) | — → 321 MiB |
| `midnight_lights` (`performance-120`) | — | 84.7 FPS (5.8 ms GPU) | **5.3 ms GPU** | — | — → **1,350 MiB** (1,498) | — → 220 MiB |
| `smooth-60` + `SHIFT_GPU_WORLD=1` | — | — | **70.0 FPS** (`-8.6%`) | — | 1,395 MiB (`+138 MiB`) | 284 MiB (`+48 MiB`) |

- **Live windowed `performance-120` (`--window-frames 180`, triple-buffered)**:
  **96.6 FPS** avg (`p50 10.21 ms`, `p95 11.83 ms`, `p99 24.70 ms`), `280.3 MiB`
  VMA allocated — up from `94.7 FPS` (`p99 27.52 ms`) in Pass 2.
- **Live windowed `ultra --hdr` (`--window-frames 90`, Odin 2 Portal OLED)**:
  Confirmed **true scRGB 16-bit HDR active** (`fmt=97`
  `VK_FORMAT_R16G16B16A16_SFLOAT`, `cs=1000104002`
  `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`, `scrgb_sup=1`, `hdr_mode=2`) at a
  rock-solid **30.0 FPS** (`avg 33.33 ms`, `p50 33.33 ms`, `p95 33.57 ms`,
  `p99 33.89 ms`).
- **GPU-driven world culling (`SHIFT_GPU_WORLD=1`) verdict**:
  Confirmed `-8.6%` slower (`70.0` vs `76.6 FPS`) and `+138 MiB` RSS / `+48 MiB`
  VMA heavier on Turnip Adreno 740 due to `nonuniformEXT` scalarization; kept
  off by default on Android (`gpu_world=0`).
- **Visual verification**:
  - `smooth-60` (720p + `2x2` VRS + `1x1` cutout VRS): barrier railings, fences,
    and foliage render razor-sharp at `1x1` while terrain/roads shade at `2x2`.
  - `quality-30` (1080p + amortized Mid/Far shadow cascades): crisp car/stage/
    spectator shadows with zero cascade flicker.

## Optimization Pass 3 (Closing Remaining Telemetry Gaps)

Inspecting the Pass 2 Odin 2 Portal logs exposed 7 remaining opportunities:

1. **Multi-LOD AI/Traffic Cars (`LOD1` → `LOD2/LOD3`), Distant VRS & Shadow Skip**:
   `race_pack` (`performance-120`) still spent `+6.85 ms` in `scene` for 7 AI
   cars because `prefer_lod1` stayed at LOD1 at all distances and cast full
   shadows. Adding a secondary `far_draws_` slice (LOD2/LOD3 from the same
   shared `lod_vertices` pool, zero extra vertex RAM) switched at `> 22 m`,
   skipping dual-pass `glass_mul_` + `glass_add_` headlight covers at `> 14 m`,
   applying `2x2` VRS on distant AI car glass, and casting shadows from
   `far_draws_` (skipping AI cars `> 45 m` from shadows) targets `race_pack`
   headroom directly.
2. **Triple Buffering (`frames_in_flight = 3`) & Dynamic Resolution Scaling (DRS)**:
   Live `performance-120` ran at `94.7 FPS` (`10.83 ms`) despite `GPU 5.8 ms` +
   `CPU 2.1 ms` due to a ~3 ms swapchain/fence stall with `frames_in_flight = 2`
   on Android `SurfaceFlinger`. Enabling `frames_in_flight = 3` on Android plus
   hysteresis-guarded DRS (adapting `scale` in `0.05` steps when rolling GPU
   frame time exceeds target budget) smooths 120 Hz pacing and heavy race packs.
3. **Env Cubemap Interval Tuning & FP16 Post-Chain Shaders**:
   `low-power` / `performance-120` spent `0.81 ms` on `env_cubemap` every 4th
   frame, and `quality-30` / `ultra` spent `3.6–4.0 ms` across FP32 post passes
   (`ssao`, `ssr`, `post`, `bloom`, `volumetric`, `sscs`). Extending
   `schedule_reflections` to support `1/8` and `1/16` face cadences, skipping
   AI/traffic car draws inside the 128x128 cubemap on Android, converting the
   post-chain shaders to FP16 (`mediump`) math, and trimming Android GTAO/SSR
   tap counts cuts GPU cost across every preset.
4. **Custom Mesa Turnip Hybrid `gmem`/`sysmem` + `B10G11R11` UBWC Patch**:
   Global `TU_DEBUG=sysmem` wins on world/shadow passes, but forces `4.0 ms` of
   bandwidth-bound full-screen post passes onto `sysmem` (and Turnip drops UBWC
   on `B10G11R11`). Added `tools/patches/turnip-adreno7xx-hybrid-sysmem.patch`
   and `--hybrid-patch` support in `tools/build-turnip-android.sh`.
5. **GPU-Driven World Culling (`SHIFT_GPU_WORLD=1`) Verification**:
   `renderer_device.cpp` defaults `gpu_world=0` on Android because Adreno 740
   incurs `nonuniformEXT` waterfall overhead and +85 MiB RAM; recorded as an
   opt-in env flag (`SHIFT_GPU_WORLD=1`) and re-verified against Pass 3.
6. **Peak RAM (`1.90 GB`) & Startup TTFF (`5.0 s`) Reduction**:
   Compact `world::Mesh` inside `Streamer::gather_now()` immediately after
   `prepare_()` finishes (before GPU staging coexists with the 200 MiB CPU
   render mesh) and skip `world_anim` crowds on `performance-120` as well as
   `low-power`.
7. **Android HDR10 Swapchain Probe & Diagnostics**:
   Log available Vulkan surface formats/colorspaces and active `hdr_mode_` on
   Android swapchain creation so HDR10 (`VK_COLOR_SPACE_HDR10_ST2084_EXT`)
   capability on Turnip + Odin 2 Portal OLED is explicitly verified.

## Future Stretch Opportunities

1. **ASTC / ETC2 offline texture transcoding cache**:
   Currently BC1/BC3/BC5 textures are uploaded natively on Adreno 740 / Turnip
   (`bc=1`). On non-BCn GPUs (Mali/PowerVR), caching ASTC blocks on first load
   would avoid RGBA8 fallback expansion.
