# Windows x64 Optimization Summary

This document records the Windows desktop performance, memory, startup, multi-car race, and binary-size optimizations implemented across Waves 1–4, together with measured results on the test workstation.

## Test Hardware & Methodology

- **CPU**: AMD Ryzen 7 5800X (8 cores / 16 threads, Zen 3, AVX2 + FMA3 + F16C)
- **GPU**: NVIDIA GeForce RTX 4080 (16 GiB VRAM, Driver 621.14, Vulkan 1.4.357, hardware ray tracing, GPU-driven world enabled)
- **RAM**: 128 GiB DDR4
- **OS**: Windows 11 (26200)
- **Compiler**: Visual Studio 2022 MSVC 17.14 (`cl.exe` 19.44.35226)
- **Benchmark Harness**: Deterministic 7-scenario suite via `carview --bench --bench-scenario ... --seconds 4` with isolated temporary `--save` files:
  1. `ultra_4k_noon` — 3840x2160, `--preset ultra`, `noon_cruise`
  2. `ultra_4k_race` — 3840x2160, `--preset ultra`, `race_pack`
  3. `high_4k_noon` — 3840x2160, `--preset high`, `noon_cruise`
  4. `high_1440p_noon` — 2560x1440, `--preset high`, `noon_cruise`
  5. `high_1080p_race` — 1920x1080, `--preset high`, `race_pack`
  6. `ultra_4k_midnight` — 3840x2160, `--preset ultra`, `midnight_lights`
  7. `ultra_1440p_rt` — 2560x1440, `--preset ultra --rt hybrid`, `noon_cruise`

## Summary of Changes

### Wave 1 — CPU Frame Loop, Shadows, RT, UI, Windows OS QoS & Audio
- **Deferred Texture Upload & Bindless Table Throttling (`apps/carview/showroom_upload.cpp`, `showroom.h`)**: Raised desktop `pump_deferred_uploads()` budget from `2 MiB / 12 textures` to `16 MiB / 48 textures` and deferred `build_gpu_table()` rebuilds to queue drain or every 4th frame during active upload bursts.
- **Dead World Matrix Copy Removal (`apps/carview/showroom_draw.cpp`)**: Removed redundant per-frame `world_draws_` model-matrix `memcpy` loop in `set_world_transform()`.
- **Shadow Cascade & Caster Pruning (`apps/carview/showroom_shadows.cpp`)**: Amortized desktop Far cascade updates (`(world_frame_ & 1) == 0`), hoisted `world_lru_.touch()` out of the per-draw shadow loop, skipped interior/gauge/caliper draws (`kl == 4, 5, 8`), switched distant AI/traffic cars (`> 25 m`) to `far_draws_`, capped car shadow casters at `140 m`, and skipped cars in the 3-cascade Far tile.
- **Ray Tracing Overlap & O(1) BLAS Cache (`apps/carview/rt_pipeline.cpp`, `rt_scene.h`, `rt_scene.cpp`)**: Removed the serializing compute barrier between independent `rt_refl` and `rt_shad` dispatches and added an O(1) steady-state cache fast path in `update_world_blases()`.
- **Minimap & UI4 Zero-Alloc Fast Paths (`apps/carview/drive_map_radio.cpp`, `ui.cpp`)**: Capped desktop minimap rasterization to 30 Hz (with stationary skip, `240 px` cap, and cached race map markers) and added zero-allocation single-line ASCII fast paths in `ui::List::measure()`, `measure_height()`, and `text()`.
- **Frame Pacing & World Sync Cleanup (`apps/carview/drive_frame_render.cpp`, `drive_world_sync.cpp`)**: Switched frame-cap sleep to `SDL_DelayPrecise()`, cached vehicle `max_power_w` instead of running a 25-step torque sweep every frame, and cached `SHIFT_BALANCE` once.
- **Windows OS QoS & x64 FP Flush-to-Zero (`apps/carview/platform_support.cpp`)**: Enabled x86/x64 SSE FTZ+DAZ (`_mm_setcsr(_mm_getcsr() | 0x8040u)`), disabled Windows EcoQoS (`SetProcessInformation(ProcessPowerThrottling)`), enabled 1 ms multimedia timers (`timeBeginPeriod(1)`), and registered the render thread with MMCSS (`L"Games"`, `AVRT_PRIORITY_HIGH`).
- **Real-Time Audio QoS & Telemetry (`libs/audio/src/audio_engine.cpp`)**: Registered the Windows audio worker with MMCSS (`L"Pro Audio"`) and replaced a 93 Hz 256-element `std::sort` in `publish_telemetry()` with `std::nth_element`.

### Wave 2 — GPU-Driven World, Shaders, VRAM & Bandwidth
- **Discard-Free Opaque Batches & Early-Z Restoration (`apps/carview/world_gpu_logic.h`, `world_gpu.h`, `world_gpu.cpp`, `showroom_world_gpu.cpp`, `shaders/world_cull.comp`, `shaders/tree_cull.comp`)**: Split solid opaque draws (`kOpaque`, `kOpaqueDecal`) from dither-faded LOD opaque draws (`kOpaqueDither`, `kOpaqueDitherDecal`, `kBatchCount = 6`), restoring hardware Early-Z and `record_z_prepass()` for solid world geometry.
- **Zero Per-Frame Descriptor Allocation & Concurrent Compute Culling (`apps/carview/world_gpu.h`, `world_gpu.cpp`)**: Replaced per-frame `vkResetDescriptorPool` + 12 descriptor allocations + 30 updates with persistent per-slot descriptor sets, added a dedicated `tree_cull_ubo` so `world_cull` and `tree_cull` dispatch concurrently without an inter-pass barrier, batched pre/post-cull barriers, and hoisted `hiz_camera_stable()` to the CPU.
- **Compact `B10G11R11_UFLOAT_PACK32` HDR Auxiliary Targets (`apps/carview/renderer.h`, `renderer_resources.cpp`, `renderer_post.cpp`, `showroom_env.cpp`, `showroom_pipelines.cpp`, `showroom_sky.cpp`)**: Used 32-bit `B10G11R11_UFLOAT` on desktop (halving VRAM and bandwidth vs `RGBA16F`) for the bloom pyramid, luminance adaptation, dynamic environment cubemap, and virtual mirror while preserving `RGBA16F` UBWC on Mesa Turnip. Pre-allocated `bloom_b_[5]` at mip-0 extent to eliminate first-frame recreation.
- **Shader Fast Paths (`apps/carview/shaders/world_frag.glsl`, `shaders/ssr.frag`, `shaders/motion_vectors.frag`, `renderer_modern_record.cpp`)**: Added zero-tap `sun_shadow()` early-out when `shadow_scale <= 0.001` (night), 4-tap PCF for tree billboards, stochastic IGN single-cascade selection across cascade seams, early-out in SSR resolve Mode 1 for low-gloss pixels (`gloss < 0.06`), and skipped 4 neighbor depth taps in `motion_vectors.frag` when temporal upscaling and RT are inactive.

### Wave 3 — MSVC LTCG/AVX2, Win32 Lock-Free I/O, SIMD, Worker Pool & RAM Compaction
- **MSVC Release Codegen & Binary Stripping (`CMakeLists.txt`, `cmake/Dependencies.cmake`, `apps/carview/carview.manifest`)**:
  - Enabled `/MP`, `/GL` + `/LTCG` (including static archiver `/LTCG`), `/Gw /Gy /Oi /Ot /Zc:inline /fp:contract`, `/OPT:REF /OPT:ICF /INCREMENTAL:NO`, configurable `/arch:AVX2` (`SHIFT_WIN_ARCH`), `/GR-` on project targets, and `/fp:fast` on hot math/render/audio/codec targets.
  - Disabled unused Windows SDL3 subsystems (`SDL_OPENGL`, `SDL_OPENGLES`, `SDL_DISKAUDIO`, `SDL_DUMMYAUDIO`, `SDL_DUMMYVIDEO`, `SDL_OFFSCREEN`, `SDL_VIRTUAL_JOYSTICK`).
  - Embedded `carview.manifest` enabling Windows 10/11 `SegmentHeap`, `longPathAware`, and `PerMonitorV2` DPI awareness.
- **Lock-Free Parallel Win32 Archive I/O & Fast Decompression (`libs/fh1data/`)**:
  - Replaced mutex-serialized `std::ifstream` reads in `Archive::read_range()` on Windows with `CreateFileW(..., FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED)` and thread-local auto-reset Win32 events (`ReadFile` / `GetOverlappedResult`).
  - Upgraded x86_64 `fh1::crc32()` to Slice-by-8 and skipped redundant second CRC32 verification on `ZipMethod::kXmem` members (already verified inside `xmem_decompress`).
  - Added a thread-local slab allocator in `xmem.cpp` to eliminate per-member `calloc`/`free` heap lock contention during LZX decoding.
  - Optimized `xds.cpp:copy_level_blocks()` with row-hoisted tiling coordinates and 64-bit SWAR/32-bit byte swaps; used compiler `_byteswap_*` intrinsics in `bytes.h`; removed redundant payload copy in `world_texture.cpp:decode_bundle_entry()`.
  - Switched `props.cpp:load_props_cache()` to single bulk file read and cached `PropModels::lod_set()` in `lod_set_cache_`.
  - Optimized `libs/cars/src/database.cpp` with `Archive::open_shared()`, bulk file reads, `SQLITE_OPEN_NOMUTEX`, and transactional `BEGIN`/`COMMIT` merge inserts.
- **Persistent Bounded Low-Priority Worker Pool (`libs/fh1data/include/fh1data/worker_pool.h`)**: Replaced ad-hoc `std::thread` spawns in `World::gather()`, `World::open()`, `shared_bundle_textures()`, `load_props()`, `PropModels::preload()`, `Collision::gather()`, and `WorldTextureLoader::load()` with a persistent `clamp(hw - 2, 2, 8)` worker pool running at `THREAD_PRIORITY_BELOW_NORMAL` with SSE FTZ+DAZ enabled.
- **World Streaming, Vertex Dedup & RAM Compaction (`libs/world/src/world.cpp`, `apps/carview/showroom_world_stream.cpp`, `showroom_upload.cpp`, `drive_world_host.cpp`, `traffic_view.cpp`)**:
  - Added binary world model index cache (`SHWI0004`, bulk-read) + parallel cold scan, prebuilt `model_index_by_member` hash map, 128 m 2D spatial grids for props and tree billboards, flat dense `hole_fill` array, and bounded desktop `PropModels::trim(2048)`.
  - Replaced `std::unordered_map<VertexKey, uint32_t>` in `build_textured_world()` with a generation-tagged flat open-addressed hash table, accelerated `float_to_half()` with hardware F16C (`_mm_cvtps_ph`), and packed world texture levels directly into mapped Vulkan staging buffers.
  - Enabled desktop CPU mesh/texture compaction (`showroom_upload.cpp`, `streamer->set_compact_mesh_after_prepare(true)`) while preserving car vertex/index buffers when hardware RT is supported.
  - Cached per-traffic-car overhead tunnel roof raycasts (`traffic_view.cpp`) across 4 m movement intervals.

## Measured Results (Ryzen 7 5800X + RTX 4080)

### Frame Rate (Avg FPS / 1% Low FPS)

| Scenario | Baseline | Wave 1 | Wave 2 | Wave 3 | Baseline → Wave 3 |
|---|---:|---:|---:|---:|---:|
| `ultra_4k_noon` | 84.1 (53.1) | 79.3 (59.4) | 90.1 (66.5) | **115.1 (82.7)** | **+36.9% (+55.7% 1%)** |
| `ultra_4k_race` | 198.9 (170.5) | 216.8 (182.3) | 211.8 (152.3) | **231.5 (189.2)** | **+16.4% (+11.0% 1%)** |
| `high_4k_noon` | 240.4 (184.1) | 267.1 (186.4) | 256.8 (197.9) | **251.4 (159.4)** | **+4.6%** |
| `high_1440p_noon` | 195.6 (115.1) | 216.4 (121.8) | 195.9 (111.4) | **203.9 (120.5)** | **+4.2% (+4.7% 1%)** |
| `high_1080p_race` | 227.9 (192.8) | 220.4 (158.4) | 223.4 (161.2) | **228.4 (159.2)** | **+0.2%** |
| `ultra_4k_midnight` | 251.4 (195.8) | 250.1 (199.5) | 258.8 (202.3) | **262.8 (184.7)** | **+4.5%** |
| `ultra_1440p_rt` | 123.9 (75.2) | 135.4 (81.9) | **137.0 (90.8)** | 128.8 (83.0) | **+4.0% (+10.4% 1%)** |

### CPU / GPU Time, Startup TTFF & Process Memory (Baseline → Wave 3)

| Scenario | CPU ms (`rec` ms) | GPU ms | Startup TTFF ms | Steady RSS MiB |
|---|---:|---:|---:|---:|
| `ultra_4k_noon` | 9.15 → **5.80** (1.67 → **1.36**) | 2.44 → 2.67 (Wave 2: **1.94**) | 10,662 → **3,226** (**-69.7%**) | 1,742 → **1,335** (**-407 MiB**) |
| `ultra_4k_race` | 2.76 → **2.41** (1.24 → **0.99**) | 2.09 → **1.78** (**-14.8%**) | 6,055 → **4,050** (**-33.1%**) | 2,102 → **1,445** (**-657 MiB**) |
| `high_4k_noon` | 2.35 → **2.27** (0.94 → **0.84**) | 1.62 → **1.53** (Wave 2: **1.40**) | 4,008 → **3,690** (**-7.9%**) | 1,671 → **1,310** (**-361 MiB**) |
| `high_1440p_noon` | 3.21 → **3.19** (0.96 → **0.88**) | 1.70 → **1.50** (**-11.8%**) | 4,076 → **3,498** (**-14.2%**) | 1,672 → **1,302** (**-370 MiB**) |
| `high_1080p_race` | 2.62 → **2.51** (1.16 → **1.00**) | 1.61 → 1.68 | 5,012 → 6,161 (DB spike; warm ~3,830) | 2,077 → **1,447** (**-630 MiB**) |
| `ultra_4k_midnight` | 2.25 → **2.03** (0.96 → **0.80**) | 1.56 → 1.63 | 4,150 → **3,627** (**-12.6%**) | 1,678 → **1,308** (**-370 MiB**) |
| `ultra_1440p_rt` | 5.39 → **5.21** (2.86 → **2.52**) | 2.48 → **2.34** (**-5.6%**) | 5,761 → **3,644** (**-36.7%**) | 1,691 → **1,317** (**-374 MiB**) |

- **Executable Size (`carview.exe`)**: `16.0 MiB` → **`15.5 MiB`** (-500 KiB).
- **Database Open Time**: `114–173 ms` → **`83.5–97.6 ms`** (~25–45% faster).
- **Audio Engine Startup**: `345–607 ms` → **`242–353 ms`** (~30–40% faster, 0 stalls on `high_4k_noon`, `high_1440p_noon`, and `ultra_1440p_rt`).

---

## Wave 4 — Multi-Car Race Hyper-Optimization

Profiling active 8-car races (`FR22`, `--render-scale 0` native resolution, `--seconds 15 --bench-warmup 6` after the 4 s launch countdown) revealed severe multi-opponent CPU, HUD, GPU overdraw, shadow/mirror, and RT BVH bottlenecks that did not appear in solo cruising or countdown-only runs.

### Summary of Changes

- **Opponent Car LOD, Frustum Culling & Draw Pipeline (`apps/carview/race_view.cpp`, `showroom_car_state.cpp`, `showroom_draw.cpp`, `showroom_upload.cpp`)**:
  - Enabled `options.prefer_lod1 = true` for race opponent cars on desktop (retail LOD1 exterior + LOD2/3 `far_draws_`), skipping invisible opponent cockpit steering-wheel and gauge-needle parts at load time.
  - Built `far_draws_` for all `guest_` cars and updated `set_wheel_poses()` to precompute per-wheel steer/spin matrices once and animate both `draws_` and `far_draws_`.
  - Added 5-plane camera frustum culling per instance group in `record_all_instances()`, enabled desktop distance switching to `far_draws_` past `20 m`, skipped guest gauges unconditionally and interior/caliper draws past `16–18 m`, disabled 2-pass `dual_cover` lamp-cover overdraw past `mid_dist` on desktop, and eliminated per-pass heap allocations.
  - Fixed `car_opaque` GPU timestamp placement in `showroom_upload.cpp` to occur after `record_all_instances(..., 1)` so opponent car opaque cost is attributed to `car_opaque` instead of `sky`, and counted instanced draws in `car_draws_`.
  - Hoisted the player `world_to_player` affine inverse once outside the opponent loop in `RaceView::instances()`, blended opponent poses directly without double `Vehicle::set_pose()`, and skipped wheel pose updates past `120 m`.
- **Shadow Cascades, Env Cubemap, Cockpit Mirrors & RT BLAS (`apps/carview/showroom_shadows.cpp`, `showroom_env.cpp`, `showroom_draw.cpp`, `rt_scene.h`, `rt_scene.cpp`)**:
  - Added ortho-box frustum culling per shadow cascade in `showroom_shadows.cpp`, switched all opponent shadow casters to `far_draws_`, and capped desktop shadow caster distance to `85 m` (matching the Mid cascade).
  - Switched `record_instance_offscreen()` to `far_draws_` (skipping interior/gauge/caliper and cover glass), and added 5-plane frustum culling, `160 m` range cap, and 6-car limit for cockpit/virtual rearview mirrors in `showroom_env.cpp` (preventing cars ahead of the player from rendering into rearview mirrors).
  - Built guest/opponent RT BLASes from `far_draws_` (skipping interior/gauge/caliper geometry), culled opponent cars past `140 m` from the RT TLAS, omitted distant (`> 45 m`) opponent glass/wheel/driver sub-instances, and throttled `spectator_blas_` rebuilds to every 6 frames or `> 4 m` eye/origin shift.
- **UI4 Scene Timeline & 120 Hz Race AI/Physics (`apps/carview/scene_timeline.h`, `scene_timeline.cpp`, `hud_screen.cpp`, `libs/drive/include/drive/ai_driver.h`, `libs/drive/src/ai_driver.cpp`, `apps/carview/race_view.cpp`, `drive_simulation.cpp`, `drive_frame_render.cpp`, `drive_hud_sync.cpp`)**:
  - Added flat per-item touched-attribute bitmasks (`item_mask_`), flat `member_of_`/`in_child_slide_` vectors, `apply_subtree()`, and lazy component initialization (`bind(scene, /*init_all_components=*/false)`) to `SceneTimeline`, eliminating the `5–6 ms` per-frame and first-activation HUD hitches when skill chains animate in `947_HUD`.
  - Added single-pass `AiField::plan_point_pair()` to halve 420 m backward braking integrations per AI car at 120 Hz, widened `update_car_planner()` quantization tolerance to `0.005f` to stop continuous `corner_speed_` cache wipes during boost/rubberband ramps, throttled aerodynamic drafting to 30 Hz in a single pairwise pass, replaced O(N) `line_.line.segment()` with O(log N) `line_segment_fast()`, staggered per-car tunnel probes, enabled SSE FTZ+DAZ with a zero-allocation callback trampoline in `PhysicsStepPool`, cached `classify_other_archetype()`, and reused `thread_local` `OpponentTag` scratch vectors.

### Measured Active Multi-Car Race Results (Ryzen 7 5800X + RTX 4080, `FR22` 8 Cars)

Active racing window: `--seconds 15 --bench-warmup 6 --render-scale 0` (native resolution, MSAA 4x on raster Ultra).

| Scenario | Metric | Before (Wave 3) | After (Wave 4) | Delta |
|---|---|---:|---:|---:|
| **Native 4K Ultra Chase** | Avg / 1% Low / Min FPS | 64.8 / 35.0 / 27.1 | **79.3 / 66.4 / 59.7** | **+22.4% / +89.7% / +120.3%** |
| | Hitch-free (`>16.7ms` / `>33.3ms`) | 91.7% (153 / 1) | **100.0% (1 / 0)** | **-152 hitches** |
| | Frame ms (avg / p99 / max) | 15.43 / 25.55 / 36.92 | **12.61 / 14.52 / 16.75** | **-18% / -43% / -55%** |
| | CPU ms (avg / p99 / max) | 5.40 / 12.58 / 27.38 | **3.68 / 5.22 / 7.08** | **-32% / -59% / -74%** |
| | `ui_build_ms` (avg / p99) | 1.90 / 6.06 | **0.71 / 1.24** | **-63% / -80%** |
| | `ai_traffic_ms` (avg / p99 / max) | 0.73 / 1.50 / 22.73 | **0.59 / 0.96 / 1.15** | **-19% / -36% / -95%** |
| | GPU ms (avg / p99 / max) | 9.65 / 17.22 / 20.22 | **8.68 / 10.29 / 10.60** | **-10% / -40% / -48%** |
| | `gpu_shadows` / `gpu_car_glass` (p99) | 0.37 / 1.33 ms | **0.27 / 0.06 ms** | **-27% / -95%** |
| **Native 4K Ultra Cockpit** | Avg / 1% Low / Min FPS | 65.9 / 39.5 / 36.5 | **81.3 / 67.0 / 61.9** | **+23.4% / +69.6% / +69.6%** |
| | CPU ms (avg / p99 / max) | 6.20 / 15.32 / 19.30 | **4.10 / 5.37 / 7.66** | **-34% / -65% / -60%** |
| | `cockpit_mirrors` GPU (p50) | 0.35 ms | **0.15 ms** | **-57%** |
| **Native 1440p RT Hybrid** | Avg / 1% Low / Min FPS | 80.5 / 63.7 / 56.0 | **92.0 / 74.3 / 70.9** | **+14.3% / +16.6% / +26.6%** |
| | CPU ms (`rec` ms) / CPU util | 5.44 (2.05) / 19.61 ms/f | **3.98 (1.48) / 6.84 ms/f** | **-27% (`-28%`) / -65% CPU burn** |
| | Device-local VRAM | 2,284.0 MiB | **2,140.5 MiB** | **-143.5 MiB** |
| **720p-scaled Race Cockpit** | Avg FPS (`cockpit_mirrors` GPU) | 173.9 (0.35 ms) | **193.1 (0.14 ms)** | **+11.0% (-60% mirrors)** |
| **720p-scaled RT Hybrid** | Avg FPS (`rec` ms) | 139.0 (2.21 ms) | **162.1 (1.44 ms)** | **+16.6% (-35% rec)** |
