# Cross-Device Hardware Benchmark Suite

`shift-engine` ships a deterministic, cross-platform hardware benchmark and
telemetry suite built directly into `carview` and orchestrated by the
zero-dependency runner in `tools/benchmark.py`.

It captures structured **JSON (schema v1)** reports and optional **per-frame
CSV** traces across Windows, Linux / Steam Deck, macOS (Apple Silicon via
MoltenVK), and Android (`adb`), then produces side-by-side **Markdown tables**
and **self-contained dark-mode HTML dashboards**.

## 1. Benchmark Scenarios (`--bench-scenario`)

| Scenario | Description |
| --- | --- |
| `noon_cruise` | Daytime (12:00) Colorado free-roam cruise with traffic, streaming, full FX, audio, and HUD. |
| `midnight_lights` | 02:00 night Colorado with headlights, bloomed taillights, stars, traffic, and full FX. |
| `cockpit_mirrors` | Cockpit camera with 3-band 1024x256 rear/side mirrors, live gauges, and interior normals. |
| `race_pack` | Multi-car festival race pack (`FR22` by default) with AI physics, skids/particles, and race HUD. |
| `autoshow_studio` | Autoshow hub with `staticcarcubemap.xpr`, live 3D car sync, and `+3.0 deg` UI4 carousel. |
| `ui4_heavy` | Rapid cycling across `H_MAP`, `925_PAUSE_MENU`, `H_STREET_RACE_HUB` EventTickets, and `H_FIXED_SETTING_LIST` to stress 2-tex masks, vector fonts, and the UI4 atlas LRU. |
| `cold_boot_switch` | Full cold-boot startup breakdown + Time-to-First-Frame (TTFF) followed by 5 deterministic car hot-switches. |

## 2. Direct `carview` Benchmark Flags

Legacy `--bench` remains backward-compatible. New flags:

- `--bench-scenario <name>`: run one of the 7 deterministic scenarios above.
- `--bench-json <path>`: write structured JSON schema v1 report.
- `--bench-csv <path>`: write per-frame CSV trace (includes warmup rows marked `warmup=1`, all CPU phases, GPU passes, draw counts, and RSS).
- `--bench-full-fx`: keep real gameplay post-FX (motion blur, DoF, god rays, lens flare, smashables, particles, skidmarks), HUD, and audio active.
- `--bench-audio`: run the dedicated lock-free audio thread against a paced null sink and collect block timing / xrun telemetry.
- `--bench-warmup <sec>`: warmup duration excluded from pacing/hitch/geometry aggregates.
- `--device-name <label>`: friendly hardware label recorded in `build.device_label`.

### Direct Example

```bash
carview --drive --play out/play \
  --bench-scenario noon_cruise --bench-full-fx --bench-audio \
  --preset high --size 1920x1080 --seconds 6 --bench-warmup 1.0 \
  --device-name "Desktop RTX" \
  --bench-json out/benchmarks/noon.json \
  --bench-csv out/benchmarks/noon.csv
```

## 3. Running Suites with `tools/benchmark.py`

### Local Run (Windows / Linux / Steam Deck / macOS)

```bash
# Quick smoke suite (noon_cruise + ui4_heavy, high preset, 1280x720)
python tools/benchmark.py run \
  --bin build-bench/apps/carview/carview.exe \
  --play out/play \
  --suite quick \
  --device-label "Desktop RTX" \
  --out out/benchmarks

# Full cross-preset / cross-resolution matrix
python tools/benchmark.py run \
  --bin build/apps/carview/carview \
  --play out/play \
  --suite full \
  --presets low-power,low,high \
  --resolutions 1280x720,1920x1080,2560x1440 \
  --device-label "Steam Deck OLED" \
  --out out/benchmarks
```

Each run writes individual `<scenario>_<preset>_<res>.{json,csv}` files, a
combined `suite.json` bundle, `report.md`, and `report.html`.

### Remote SSH Run (macOS `minivac`, Steam Deck, Linux hosts)

```bash
python tools/benchmark.py run \
  --remote minivac \
  --bin ~/shift-engine/build/apps/carview/carview \
  --play ~/shift-play \
  --suite standard \
  --device-label "M4 Mac mini (MoltenVK)" \
  --out out/benchmarks
```

`--remote` runs `carview` on the target host via `ssh`, pulls back every JSON
and CSV artifact via `scp`, and builds the local bundle and HTML dashboard.

### Android (`adb`)

Build with `-DSHIFT_ANDROID_EXECUTABLE=ON`, push `carview` and the play assets,
run over `adb shell`, and pull back the JSON/CSV:

```bash
adb shell "/data/local/tmp/carview --drive --play /sdcard/shift-play \
  --bench-scenario noon_cruise --preset low-power --size 1280x720 \
  --seconds 6 --bench-warmup 1.0 --device-name 'Snapdragon 8 Gen 3 (Turnip)' \
  --bench-json /data/local/tmp/android_noon.json"
adb pull /data/local/tmp/android_noon.json out/benchmarks/
```

## 4. Side-by-Side Comparison Reports

Pass any mix of single-run JSONs, `suite.json` bundles, or benchmark output
directories. The first device is used as the baseline for speedup/regression
delta percentages:

```bash
python tools/benchmark.py compare \
  out/benchmarks/desktop_rtx/suite.json \
  out/benchmarks/m4_minivac/suite.json \
  out/benchmarks/steam_deck/suite.json \
  --out out/benchmarks/comparison.md \
  --html out/benchmarks/comparison.html
```

## 5. JSON Schema v1 Overview

Every benchmark JSON contains `"schema_version": 1` and the following sections:

- `hardware`: OS name/version, CPU arch, logical cores, CPU brand, system RAM (MiB).
- `gpu`: Vulkan device name, vendor/device IDs, device type, API/driver version & info, MoltenVK/Turnip/portability flags, timestamp support, VRAM total/budget.
- `features`: GPU-driven world, Hi-Z, bindless, VRS, HDR10/scRGB, upscaler, frame gen, AA/SSAO/shadows/VSync, MSAA, anisotropy, render scale, full FX, audio.
- `build`: git commit, build config, preset, device label, scenario, output/render extents, durations.
- `startup_ms`: `vulkan_init_ms`, `pipeline_compile_ms`, `db_catalog_load_ms`, `audio_load_ms`, `car_bake_load_ms`, `world_load_ms`, `ttff_ms`, and `car_switches` array.
- `pacing`: frame counts, wall/sim seconds, Avg / 1% low / 0.1% low / min / max FPS, and full percentile distributions (`avg`, `min`, `p50`, `p90`, `p95`, `p99`, `p99_9`, `max`) for `frame_ms`, `cpu_ms`, and `gpu_ms`.
- `cpu_phases_ms`: distributions for `physics_ms`, `ai_traffic_ms`, `world_stream_ms`, `audio_push_ms`, `ui_build_ms`, `pose_cull_ms`, `cmd_record_ms`, and `submit_wait_ms`.
- `gpu_passes_ms`: per-pass Vulkan timestamp distributions (`env_cubemap`, `env_prefilter`, `cockpit_mirrors`, `hiz_cull`, `shadows`, `zpre`, `setup`, `sky`, `world gpu`, `contact_shadow`, `car_opaque`, `car_glass`, `hiz_build`, `particles`, `ssao`, `motion`, `post`, `fxaa`, `upscale`, `hdr`, `ui`, `rest`).
- `audio`: blocks rendered, avg/p95/worst block ms, stall/xrun count, main-thread push avg/worst ms.
- `stutter`: frames > 16.67 ms / 33.33 ms / 50 ms, hitch-free % (within `1.5x p50`), worst hitch, and `top5_slowest_frames` with dominant CPU phase attribution.
- `geometry` & `ui4_atlas`: per-frame avg/peak draw calls, culled draws, triangles drawn vs total, UI4 shapes, and UI4 atlas utilization/evictions.
- `memory`, `streaming` & `cpu_util`: process RSS current/peak MiB, VMA blocks/allocs/used/reserved MiB, resident world textures & upload/eviction/reupload/retired counts, staging bandwidth MiB/s, and process CPU utilization (% of 1 core and % of all logical cores).
