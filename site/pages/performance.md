---
title: Performance
section: Technical
order: 1
description: Shift Engine's Vulkan 1.3 architecture, split bindless support for MoltenVK and Android, measured desktop and handheld numbers, and how to run tools/benchmark.py.
---

# Performance

Shift Engine is designed to scale from integrated GPUs, handhelds and Apple Silicon up to 4K high-refresh desktops using one Vulkan codebase. All numbers on this page are measured directly from Release builds using the built-in `--bench` harness and `tools/benchmark.py`.

## Architecture

- **GPU-driven world rendering (`WorldGpu`):** Colorado geometry and billboard trees are resident in GPU arenas and culled on the GPU via compute shaders (`world_cull.comp`, `tree_cull.comp`) against the frustum and a hierarchical depth pyramid (`hiz_gen.comp`), emitting indirect draws with minimal CPU recording overhead.
- **Discard-free opaque passes and Early-Z sky:** Opaque world surfaces render with `early_fragment_tests` (`world_opaque` / `world_gpu_opaque`), cutout foliage alpha-tests early, and the sky dome and stars render after opaque geometry at far depth so covered sky pixels are rejected by Early-Z (cutting sky GPU time at 4K from `0.37 ms` to `0.03 ms`).
- **Lazy render targets and preset-sized arenas:** Bloom, SSAO, FXAA, motion-vector, temporal-history, upscaler and cockpit-mirror render targets are allocated only when active. World arenas, texture LRU caches, UI4 atlases and dynamic cubemaps scale with the selected preset, and cubemap/mirror updates are amortized across frames.
- **Zero-hitch streaming and hot car swap:** Traffic spawns and despawns avoid `vkDeviceWaitIdle()` via spare frame descriptor sets and multi-frame retirement rings. Switching cars, changing paint, installing upgrades or entering/leaving races hot-swaps the car in place (`Showroom::reload_car()`) without tearing down the renderer or world.
- **Parallel CPU pipeline:** Archive decompression, world/collision/prop gathering, texture decoding and multi-car AI physics run on worker pools alongside the dedicated lock-free audio thread.

## Split bindless and TBDR

Earlier Vulkan bindless renderers commonly use `COMBINED_IMAGE_SAMPLER[4096]`, which exceeds the per-stage sampler limits on Apple Silicon (MoltenVK / Metal 3 Argument Buffers) and many Android Adreno/Mali drivers, forcing a slow CPU draw fallback.

Shift Engine avoids that fallback with three portability optimizations:

- **Split bindless descriptors:** `WorldGpu` binds `1×` immutable `VK_DESCRIPTOR_TYPE_SAMPLER` alongside `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE[4096]`, qualifying Apple Silicon MoltenVK and Android Turnip/Vulkan 1.1+ devices for the full GPU-driven world culling and Hi-Z path.
- **Persistent `VkPipelineCache`:** All 30+ graphics and compute pipelines share a vendor-, device- and UUID-validated on-disk pipeline cache (`vk_pipeline_cache.bin`), eliminating repeated SPIR-V-to-Metal (MSL/PSO) and mobile driver shader compilation stalls across launches.
- **TBDR-friendly MSAA and render passes:** MSAA color and offscreen depth attachments use `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` / lazily allocated memory with `DONT_CARE` store ops, and empty particle splits are skipped so tile-based GPUs (Apple Silicon, Adreno, Mali) keep MSAA resolves on-chip instead of flushing full-resolution buffers to RAM.

## Measured numbers

### Desktop (Ryzen 7 5800X + NVIDIA GeForce RTX 4080)

Windows 11, AMD Ryzen 7 5800X (8 cores / 16 threads), 128 GB RAM, NVIDIA GeForce RTX 4080, MSVC Release (`e1400b72` vs. pre-optimization baseline `aa0fb579`), native resolution (`--render-scale 0`, traffic + HUD enabled):

| Scenario | Before (`aa0fb579`) | After (`e1400b72`) | Improvement |
| --- | --- | --- | --- |
| 1080p `low-power` | CPU `1.14 / 6.71 ms`, GPU `0.55 ms`, VRAM `639 MiB` | CPU `0.41 / 0.84 ms`, GPU `0.31 ms`, VRAM `323 MiB` | 2.8× CPU p50, 8.0× CPU p99, −49% VRAM |
| 1080p `high` (4× MSAA) | CPU `1.58 / 6.61 ms`, GPU `0.90 ms`, VRAM `749 MiB` | CPU `0.70 / 1.37 ms`, GPU `0.68 ms`, VRAM `553 MiB` | 2.3× CPU p50, 4.8× CPU p99, −26% VRAM |
| 4K `high` (3840×2160) | CPU `1.54 / 4.40 ms`, GPU `2.49 ms`, VRAM `1,331 MiB` | CPU `0.73 / 2.69 ms`, GPU `2.04 ms`, VRAM `943 MiB` | 2.1× CPU p50, −0.45 ms GPU, −29% VRAM |
| 4K `ultra` (SSAO + FXAA) | CPU `2.05 / 10.04 ms`, GPU `3.28 ms`, VRAM `1,331 MiB` | CPU `0.75 / 1.54 ms`, GPU `2.69 ms`, VRAM `1,045 MiB` | 2.7× CPU p50, 6.5× CPU p99, −0.59 ms GPU |
| 1080p cockpit (3-band mirrors) | CPU `2.72 / 9.92 ms`, GPU `1.22 / 21.30 ms`, VRAM `749 MiB` | CPU `0.81 / 3.25 ms`, GPU `1.00 / 1.80 ms`, VRAM `556 MiB` | 3.4× CPU p50, 11.8× GPU p99 |
| 1080p 8-car race (`FR22`) | CPU `4.48 / 17.38 ms`, GPU `1.49 / 26.07 ms`, VRAM `864 MiB` | CPU `1.16 / 1.72 ms`, GPU `0.97 / 2.47 ms`, VRAM `667 MiB` | 3.9× CPU p50, 10.1× CPU p99, 1.54× GPU |
| 1080p 240 km/h streaming (20 s) | CPU `1.73 / 8.49 ms` (23.9 ms hitch), GPU `0.78 ms` | CPU `0.74 / 1.29 ms` (0 stream hitch), GPU `0.59 ms` | 2.3× CPU p50, 6.6× CPU p99, 0 hitch |
| Cold / warm startup (Colorado) | `11.18 – 13.51 s` | `3.91 s` cold / `3.74 s` warm (`vk_pipeline_cache.bin`) | 3.0–3.4× faster |
| Car switch (`R34 → DBS`) | `~11,500 – 12,600 ms` | `212.8 ms` (535 textures & 4,556 GPU draws kept) | 54–59× faster |

CPU timings in the table are `p50 / p99` frame times; GPU timings are `p50` (or `p50 / p99` where shown). With Vulkan validation enabled (`SHIFT_VALIDATION=1`), the 1080p cockpit smoke benchmark on `e1400b72` averaged `354.0 fps` (CPU p50 `0.83 ms`, GPU p50 `1.11 ms`, TTFF `3,332.7 ms`, pipeline compile `29.3 ms`, `0` validation messages).

### Integrated GPU and Steam Deck (earlier baseline)

Earlier 60-second 1280×720 free-roam cruise measurements (`carview --drive --bench`, 3,601 frames, before the `e1400b72` GPU/VRAM optimization pass):

| Hardware | Preset | GPU p50 / p95 / max | Frame CPU p50 / p99 |
| --- | --- | --- | --- |
| Intel UHD 620, Core i5-8250U (Windows) | `low` (no MSAA) | `10.1 / 12.7 / 16.2 ms` | `1.16 / 2.1 ms` |
| Intel UHD 620, Core i5-8250U (Windows) | `high` (4× MSAA) | `12.7 / 15.5 / 26.4 ms` | `1.18 / 2.1 ms` |
| Steam Deck LCD (AMD Van Gogh, RADV, SteamOS 3.7) | `low` | `2.9 / 3.6 / 13.8 ms` (powersave clocks) | `2.85 / 3.9–4.05 ms` |

### Apple Silicon (M4 Pro / MoltenVK)

On Apple Silicon via MoltenVK, the legacy combined-sampler path silently dropped out of `WorldGpu` into the CPU draw fallback (`SHIFT_NO_ARENA=1`, measured on desktop proxy at `2.95 ms` GPU p50 / `48.5 ms` CPU p95). The split-bindless layout (`50483a9f`), persistent `VkPipelineCache`, and TBDR transient MSAA changes in current `main` specifically restore GPU-driven culling and Hi-Z on Metal 3 Argument Buffers.

A full seven-scenario `tools/benchmark.py` sweep on Apple M4 Pro (macOS 15 + MoltenVK) on current `main` has not yet been published into this table; capture one directly with `--remote` (below) and compare it against your desktop baseline.

## Presets and scaling

| Preset | Intended hardware | Key defaults |
| --- | --- | --- |
| `low-power` | Mobile / handheld low-wattage, integrated GPUs | 1× MSAA, shadows and bloom off, compact world arena and LRU |
| `smooth-60` | Android handhelds, Steam Deck 60 fps | 2× MSAA, shadows off, quarter-rate reflections |
| `low` | Integrated GPUs, older laptops | 1× MSAA, reduced shadow and reflection budgets |
| `quality-30` | Handheld 30 fps quality mode | 4× MSAA, low shadows, bloom on, quarter-rate reflections |
| `high` | Mainstream desktop GPUs (1080p–4K) | 4× MSAA, cascaded shadows, full world draw distance, bloom |
| `ultra` | High-end desktop GPUs | Full resolution, SSAO, FXAA, highest shadow and cubemap budgets |

Render scale (`--render-scale`), CAS / FSR / temporal upscaling, and Variable Rate Shading (`--vrs`, measured `-18%` GPU time at 1080p on RTX 4080) can be toggled live from the Settings overlay (<kbd>F6</kbd>) or `settings.txt`.

## Running benchmarks

`carview` embeds seven deterministic scenarios (`noon_cruise`, `midnight_lights`, `cockpit_mirrors`, `race_pack`, `autoshow_studio`, `ui4_heavy`, `cold_boot_switch`) and writes JSON schema v1 reports and per-frame CSV traces. `tools/benchmark.py` orchestrates suites locally or over SSH with zero external Python dependencies:

```sh
# Validate the benchmark script itself (no game files needed)
python tools/benchmark.py --self-test

# Run a quick local suite (noon_cruise + ui4_heavy)
python tools/benchmark.py run \
  --bin build/apps/carview/carview \
  --play /path/to/play \
  --suite quick \
  --device-label "Desktop RTX 4080" \
  --out out/benchmarks/desktop

# Run remotely on a macOS Apple Silicon host or Steam Deck over SSH
python tools/benchmark.py run \
  --remote mac-host \
  --bin ~/shift-engine/build/apps/carview/carview \
  --play ~/shift-play \
  --suite standard \
  --device-label "Apple M4 Pro (MoltenVK)" \
  --out out/benchmarks/m4_pro

# Generate a side-by-side Markdown report and self-contained HTML dashboard
python tools/benchmark.py compare \
  out/benchmarks/desktop/suite.json \
  out/benchmarks/m4_pro/suite.json \
  --out out/benchmarks/comparison.md \
  --html out/benchmarks/comparison.html
```

See [docs/BENCHMARK.md](https://github.com/arcanite24/pinyon-shift-native/blob/main/docs/BENCHMARK.md) for the full JSON schema v1 field list, Android `adb` commands and direct `carview` flags.
