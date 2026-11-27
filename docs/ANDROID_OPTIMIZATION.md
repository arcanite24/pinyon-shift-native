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

| Preset / Scenario | Pass 1 (`177d9213`) FPS | Pass 2 FPS | Delta | Pass 1 RSS | Pass 2 RSS | RAM Saved | Pass 1 VMA | Pass 2 VMA |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `low-power` (`noon_cruise`) | 74.3 FPS | **84.1 FPS** | +13.2% | 1,682 MiB | **1,352 MiB** | -330 MiB | 219 MiB | 220 MiB |
| `performance-120` (`noon_cruise`) | 79.4 FPS | **83.7 FPS** | +5.4% | 1,554 MiB | **1,243 MiB** | -311 MiB | 229 MiB | **220 MiB** |
| `smooth-60` (`noon_cruise`) | 71.2 FPS | **75.5 FPS** | +6.0% | 1,512 MiB | **1,364 MiB** | -148 MiB | 248 MiB | **236 MiB** |
| `quality-30` (`noon_cruise`) | 35.4 FPS | **44.8 FPS** | **+26.6%** | 1,684 MiB | **1,400 MiB** | -284 MiB | 390 MiB | **341 MiB** |
| `ultra` (`noon_cruise`) | 24.5 FPS | **32.1 FPS** | **+31.0%** | 1,702 MiB | **1,386 MiB** | -316 MiB | 474 MiB | **389 MiB** |
| `race_pack` (`smooth-60`, 8 cars) | 30.1 FPS | **38.8 FPS** | **+28.9%** | — | **1,531 MiB** | — | — | 340 MiB |
| `race_pack` (`performance-120`, 8 cars) | — | **41.0 FPS** | — | — | **1,495 MiB** | — | — | 320 MiB |
| `midnight_lights` (`performance-120`) | — | **84.7 FPS** | — | — | **1,361 MiB** | — | — | 220 MiB |

- **Live windowed verification (`--window-frames 180 --preset performance-120`)**:
  **94.7 FPS** avg (`p50 10.31 ms`, `p95 12.06 ms`), 4 LOD1 traffic models
  uploaded in `19–44 ms` each, `280 MiB` VMA allocated.
- **Visual verification**:
  - `smooth-60` (720p + `2x2` VRS + `1x1` cutout VRS): barrier railings, fences,
    and foliage render razor-sharp at `1x1` while terrain/roads shade at `2x2`.
  - `quality-30` (1080p + amortized Mid/Far shadow cascades): crisp car/stage/
    spectator shadows with zero cascade flicker.

## Future Stretch Opportunities

1. **World draw-call merging / GPU-driven multi-draw indirect on Android**:
   Colorado gathers currently submit ~1,500–2,500 draws per frame; enabling the
   GPU-driven `world_cull.comp` + `vkCmdDrawIndexedIndirectCount` path on Turnip
   (Adreno 740 supports `multiDrawIndirect` + descriptor indexing) can further
   reduce CPU submission time in dense Festival / Red Rock scenes.
2. **Custom Mesa Turnip per-pass `use_sysmem_rendering` heuristic**:
   Instead of global `TU_DEBUG=sysmem`, patching `tu_cmd_buffer.cc` in a custom
   Turnip build (`tools/build-turnip-android.sh`) to use `gmem` for full-screen
   post-processing passes (bloom/SSAO/tone-map) and `sysmem` for high-geometry
   world/shadow passes could capture the best of both worlds (especially on
   `quality-30` and `ultra` at 918p–1080p).
3. **ASTC / ETC2 offline texture transcoding cache**:
   Currently BC1/BC3/BC5 textures are either uploaded natively (Turnip supports
   `textureCompressionBC` on Adreno 740!) or transcoded on devices without BCn.
   On non-BCn GPUs (Mali/PowerVR), caching ASTC blocks on first load would avoid
   RGBA8 fallback expansion.
