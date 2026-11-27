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

## Optimization Pass 4: Ultra-Deep Assessment (Waves 1–3)

A full end-to-end audit of Android GPU passes, shaders, synchronization, CPU
threading, memory footprint, startup time, and ARM64 code generation was
implemented and verified on the AYN Odin 2 Portal (`bd89bfde`, Turnip MrPurple
T30, 1080p target):

### Wave 1 — High-Impact Sync, Render-Pass & Shader Fixes

1. **Preset Leak Fix (`drive_cli.cpp`)**: Prevented `settings.txt` default
   tokens from overriding `--preset` flags when running presets or benchmarks.
2. **Eliminated Hidden Full-Res SDR Brightness Pass (`upscale.frag`, `post.frag`, `renderer_frame.cpp`)**:
   Folded `sdr_brightness` directly into `upscale.frag` and `post.frag`,
   removing the extra 8 MiB `stand_in_` render target, 1080p full-screen blit
   pass, and Frame-1 `vkDeviceWaitIdle()`.
3. **Car & Guest Car Pipeline / Shader Cleanup (`showroom_frag.glsl`, `showroom_upload.cpp`, `showroom_draw.cpp`, `showroom_pipelines.cpp`)**:
   - Chained glass/mirror branches before full PBR lighting and skipped wasted
     `spec_map` sampling on `_NORM2` under `kMobileFast`.
   - Fixed guest/AI car duplicate pipeline creation, skipped redundant dual-pass
     glass covers and forced `2x2` VRS + `LOD2` far slices for distant AI cars,
     and merged rigid bodywork draws into contiguous index runs.
4. **4-Image Android FIFO Swapchain & Frame Pacing (`renderer_swapchain.cpp`, `drive_frame_render.cpp`, `drive_input_loop.cpp`, `DriveActivity.java`)**:
   - Requested 4 swapchain images on Android (`minImageCount + 2` clamped to
     `maxImageCount`), bypassed software `FramePacer` sleep under FIFO vsync,
     fixed ADPF work-duration reporting to exclude swapchain wait, switched DRS
     to GPU-timed feedback, and applied `preferredDisplayModeId` unconditionally.
5. **CPU & Allocator Hygiene (`drive_map_radio.cpp`, `platform_support.cpp`, `sfx_engine.cpp`, `drive_world_host.cpp`)**:
   - Throttled live minimap road/event rebuilding, configured Scudo
     `mallopt(M_DECAY_TIME, 1)` + `M_PURGE_ALL`, lowered background SFX decoder
     priority on Android/Linux, and made world soundscape loading asynchronous
     (`async_sc = !app.headless || app.bench`).

### Wave 2 — Zero-Barrier UBO Rings, FP16 Shaders & World Batching

1. **Zero Per-Frame `vkCmdUpdateBuffer` Calls or UBO Barriers (`renderer.h`, `renderer_*.cpp`, `showroom.h`, `showroom_*.cpp`)**:
   - Converted `post_optical_buf_`, `vol_ubo_`, `sky_ubo_`, and `env_sky_ubo_`
     to `kFramesInFlight`-sliced `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`
     rings; converted `frame_buffer_`, all 3 shadow cascade UBOs,
     `headlights_ubo_`, and `env_frame_buffer_` to persistently-mapped
     `Showroom::MappedRing`s; converted `glow_buf_` and `overlay_buf_` to
     `DynBuffer`s.
2. **Render-Pass & Barrier Reductions (`renderer_resources.cpp`, `renderer_frame.cpp`, `showroom_env.cpp`)**:
   - Started the Android bloom pyramid at quarter-res (`rw/4 x rh/4`).
   - Bypassed the 1:1 native bilinear `upscale` pass when no upscale or FXAA is
     needed.
   - Merged offscreen cubemap/mirror sky rendering into single-pass mode-2 sky
     and batched `env_prefilter` mip transitions into 2 barriers instead of
     serial per-mip barriers.
3. **Deep FP16 (`mediump`) & Shader ALU Cuts (all 54 SPIR-V shaders regenerated)**:
   - Replaced `transpose(inverse(mat3(model_mat)))` in `showroom.vert` with a
     division-free cofactor matrix; guarded `sin()` hash in `world_vert.glsl`.
   - Expanded `mediump` across `sky.frag`, `world_frag.glsl`, `showroom_frag.glsl`,
     `ssao.frag`, `ssr.frag`, and `post.frag`; replaced integer `pow()` calls
     with multiplies and `pow(denom, 1.5)` with `denom * sqrt(denom)` in
     `volumetric.frag`; gated multi-scale paint flakes, 12/16-tap car shadow
     loops, and PCSS under `kMobileFast`, capped dynamic headlights at 6 under
     `kMobileFast`, and respected `frame.shadow.w` for 1-tap bilinear PCF.
4. **World Draw Batching & Cached Descriptor Sets (`world.h`, `showroom_world_stream.cpp`, `showroom_world_gpu.cpp`, `showroom_draw.cpp`)**:
   - Dropped raw `effect` string from `MeshMaterial::key()` so shader-identical
     materials merge into one batch; tuned Android near cell size to `384 m`
     (`128 m` for `fade == 1`).
   - Cached resolved `VkDescriptorSet` per `WorldGeometry::Draw` behind
     `world_sets_gen_`, reused bucket/touched scratch vectors, and sorted
     opaque/cutout draws by `(cached_set, decal)`.

### Wave 3 — Memory (-1.0 GiB PSS), Startup TTFF (-45%) & ARM64 Build

1. **Shared `bin.zip` (`Archive::open_shared`), Lock-Free `pread`, Hardware ARM64 CRC32 (`archive.h/.cpp`, `members.h/.cpp`, `world_acoustics.cpp`)**:
   - Deduplicated the 230k-member `bin.zip` central directory and sorted
     `BaseNameIndex` across `World`, `Collision`, `SmashableWorld`, and
     `WorldAcousticsData`.
   - Used lock-free POSIX `::pread()`, fast-pathed extra-field `0x1123` payload
     offsets (avoiding redundant local-header seeks), and accelerated
     `fh1::crc32()` with ARMv8 `__crc32d/w/h/b`.
2. **Audio Heap Cuts & RT Priority (`fsb4.h/.cpp`, `audio_engine.h/.cpp`, `satnav_vo.cpp`, `radio.cpp`)**:
   - Added `read_fsb4_file_headers()` so UI music, SatNav, and Radio scan only
     the FSB4 header table (~5–30 KiB instead of reading entire multi-MB banks).
   - Replaced eager 5-track float32 `init_ui_music()` decoding with lazy
     on-demand single-track `int16_t` loading and Android eviction when inactive.
   - Reduced Android `RadioPlayer` `StreamPool` to `(6, 49152)` and enabled
     ARM64 FPCR Flush-to-Zero + `PRIO_PROCESS -16` on the audio render thread.
3. **World/Prop Memory, Binary Disk Cache & RTTI Removal (`props.h/.cpp`, `world.h/.cpp`, `smashables.cpp`, `world_anim.h/.cpp`, `showroom_vfx.cpp`, `showroom_upload.cpp`, `stream.h`)**:
   - Interned `PropWorld::route_lists` (`uint16_t route_list` per placement/tree)
     and added versioned binary disk cache (`<cache>.props.bin`).
   - Shared `.bundle` small-texture payloads between `World` and `SmashableWorld`.
   - Fixed `World::gather()` prop preload radius (`prop_r2` instead of `tree_r2`),
     bounded Android `PropModels` via `trim(512)`, freed `SpectatorCatalog::atlas_rgba`
     (16 MiB) and player car CPU models/wheel/shared textures after GPU upload.
   - Replaced the repo's sole `std::any` (`world::Streamer::Prepare`) with
     `std::shared_ptr<void>`, enabling `-fno-rtti` cleanly across Android.
4. **ARM64 Compiler Flags, Symbol Visibility, Thread QoS & Mesa Shader Cache (`CMakeLists.txt`, `cmake/Dependencies.cmake`, `platform_support.h/.cpp`, `DriveActivity.java`)**:
   - Disabled `SHIFT_ENABLE_RAY_TRACING` and SDL OpenGL/GLES by default on
     Android (dropped `libGLESv1_CM.so` / `libGLESv2.so` linkage).
   - Added `-march=armv8.2-a+lse+crc+fp16+dotprod -mtune=cortex-x3 -fno-rtti -fvisibility=hidden -fvisibility-inlines-hidden -fno-math-errno -fno-trapping-math -gline-tables-only -Wl,--lto-O3` and `-ffp-contract=fast` on hot libraries.
   - Added ARM64 FPCR FZ + big/prime (`CPU 3..7`, nice `-4`) render-thread QoS
     and LITTLE (`CPU 0..2`, nice `+10`) stream-worker pinning on 8-core SoCs.
   - Configured Mesa single-file persistent shader cache (`MESA_SHADER_CACHE_DIR`,
     `MESA_DISK_CACHE_SINGLE_FILE=1`, `MESA_SHADER_CACHE_MAX_SIZE=256M`).

### Measured Wave 1–3 Progression (Odin 2 Portal, 1080p Target, Turnip MrPurple T30 + `TU_DEBUG=sysmem`)

| Benchmark (1920x1080) | Pre-Audit Baseline | Wave 1 | Wave 2 | **Wave 3 (Final)** | Total Gain vs Baseline |
| --- | ---: | ---: | ---: | ---: | ---: |
| **`live_perf120`** (real live 120 Hz) | 62.0 FPS (`p50 21.1 ms`) | 65.3 FPS (`p50 15.4 ms`) | 119.0 FPS (`p50 8.31 ms`) | **118.8 FPS** (`avg 8.33 ms`, `p50 8.30 ms`, `p95 9.23 ms`, `p99 10.45 ms`) | **+91.6% (120 Hz ceiling locked)** |
| **`perf120_noon`** (headless) | 47.4 FPS (`GPU 11.42 ms`, 356 draws) | 61.8 FPS (`GPU 8.38 ms`) | 102.2 FPS (`GPU 4.73 ms`, 148 draws) | **100.9 FPS** (`GPU 4.78 ms`, `CPU 1.88 ms`, 148 draws, 100% hitch-free) | **+112.9% FPS / -58.1% GPU** |
| **`smooth60_noon`** (2× MSAA + GTAO/SSCS) | — | 50.6 FPS (`GPU 10.53 ms`) | 79.7 FPS (`min 61.4 FPS`) | **84.8 FPS** (`min 63.9 FPS`, `GPU 5.79 ms`, `0 >16.7ms`) | **+67.6% vs Wave 1** |
| **`quality30_noon`** (4× MSAA + shadows/vol/bloom) | — | 25.8 FPS (`GPU 22.03 ms`) | 37.2 FPS (`GPU 14.81 ms`) | **40.4 FPS** (`min 34.5 FPS`, `GPU 13.44 ms`, `0 >33.3ms`) | **+56.6% vs Wave 1** |
| **`ultra_noon`** (4× MSAA + PCSS/SSR/Full GTAO) | — | 20.1 FPS (`GPU 28.87 ms`) | 31.0 FPS (`GPU 18.08 ms`) | **31.1 FPS** (`GPU 17.95 ms`, 228 draws) | **+54.7% vs Wave 1** |
| **`perf120_race`** (8-car pack) | 33.7 FPS (`GPU 16.13 ms`) | 42.9 FPS (`GPU 12.20 ms`) | 48.5 FPS (`GPU 10.63 ms`) | **49.7 FPS** (`min 44.0 FPS`, `GPU 10.59 ms`, 100% hitch-free) | **+47.5% FPS** |
| **`live_perf120` TOTAL PSS** (`dumpsys meminfo`) | 1,491 MiB (`Native Heap 1,175 MiB`) | — | 1,491 MiB | **489 MiB** (`Native Heap 373 MiB`, `TOTAL RSS 502 MiB`) | **-67.2% (-1.00 GiB PSS)** |
| **Warm Startup TTFF** (`perf120_noon`) | ~5,243 ms (`perf120_race`) | — | 5,243 ms | **2,905 ms** (`pipe 6.6 ms`, `world 1,892 ms`; `perf120_race` 4,087 ms) | **-44.6% TTFF** |
| **Android Binary / APK** | ~220+ MiB unstripped / >22k exports | — | — | **73.2 MiB unstripped, 11.1 MiB APK, 4,321 exports, 0 GLES deps** | **~3× smaller debug `.so`, ~5× fewer exports** |

### Post-Wave 3 `TU_DEBUG=sysmem` vs `gmem` Verification (Odin 2 Portal, 1080p `noon_cruise`)

Even after Wave 1–3 render-pass, UBO-barrier, and draw-count cuts, `TU_DEBUG=sysmem`
still beats `TU_DEBUG=gmem` across every preset (including 4× MSAA):

| Preset | `TU_DEBUG=sysmem` | `TU_DEBUG=gmem` | `sysmem` Advantage |
| --- | ---: | ---: | ---: |
| `performance-120` | **100.9 FPS** (`GPU 4.78 ms`) | 78.5 FPS (`GPU 6.56 ms`) | **+28.5%** |
| `smooth-60` | **84.8 FPS** (`GPU 5.79 ms`) | 76.6 FPS (`GPU 6.61 ms`) | **+10.7%** |
| `quality-30` (4× MSAA) | **40.4 FPS** (`GPU 13.44 ms`) | 33.9 FPS (`GPU 16.44 ms`) | **+19.2%** |
| `ultra` (4× MSAA) | **31.1 FPS** (`GPU 17.95 ms`) | 27.1 FPS (`GPU 20.96 ms`) | **+14.8%** |

## Future Stretch Opportunities

1. **ASTC / ETC2 offline texture transcoding cache**:
   Currently BC1/BC3/BC5 textures are uploaded natively on Adreno 740 / Turnip
   (`bc=1`). On non-BCn GPUs (Mali/PowerVR), caching ASTC blocks on first load
   would avoid RGBA8 fallback expansion.
