#!/usr/bin/env python3
"""Cross-device hardware benchmark runner & comparison report generator for shift-engine.

Zero external dependencies (Python 3 standard library only).

Usage:
  # Run a benchmark suite locally (Windows, Linux/Steam Deck, macOS):
  python tools/benchmark.py run --bin build-bench/apps/carview/carview.exe \\
      --play out/play --suite quick --device-label "Desktop RTX" --out out/benchmarks

  # Run remotely over SSH (e.g. macOS host or Steam Deck) and pull back JSON/CSV:
  python tools/benchmark.py run --remote mac-mini --bin ~/shift-engine/build/apps/carview/carview \\
      --play ~/shift-play --suite standard --device-label "M4 Mac mini" --out out/benchmarks

  # Compare two or more benchmark runs or suites (Markdown + dark-mode HTML dashboard):
  python tools/benchmark.py compare run_a/suite.json run_b/suite.json \\
      --out comparison.md --html comparison.html

  # Run self-tests:
  python tools/benchmark.py --self-test
"""
from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

SUITE_DEFAULTS: dict[str, dict[str, Any]] = {
    "quick": {
        "scenarios": ["noon_cruise", "ui4_heavy"],
        "presets": ["high"],
        "resolutions": ["1280x720"],
        "seconds": 3.0,
        "warmup": 0.5,
    },
    "standard": {
        "scenarios": ["noon_cruise", "midnight_lights", "cockpit_mirrors", "race_pack", "ui4_heavy"],
        "presets": ["low", "high"],
        "resolutions": ["1280x720", "1920x1080"],
        "seconds": 6.0,
        "warmup": 1.0,
    },
    "full": {
        "scenarios": [
            "noon_cruise",
            "midnight_lights",
            "cockpit_mirrors",
            "race_pack",
            "autoshow_studio",
            "ui4_heavy",
            "cold_boot_switch",
        ],
        "presets": ["low-power", "low", "high"],
        "resolutions": ["1280x720", "1920x1080", "2560x1440"],
        "seconds": 8.0,
        "warmup": 1.5,
    },
}

ALL_SCENARIOS = list(SUITE_DEFAULTS["full"]["scenarios"])

CPU_PHASE_META = [
    ("physics_ms", "Physics", "#38bdf8"),
    ("ai_traffic_ms", "AI/Traffic", "#818cf8"),
    ("world_stream_ms", "Stream", "#c084fc"),
    ("audio_push_ms", "Audio", "#f472b6"),
    ("ui_build_ms", "UI4 Build", "#fb923c"),
    ("pose_cull_ms", "Pose/Cull", "#facc15"),
    ("cmd_record_ms", "Cmd Record", "#4ade80"),
    ("submit_wait_ms", "Submit/Wait", "#94a3b8"),
]

GPU_PASS_COLORS = [
    "#38bdf8",
    "#818cf8",
    "#c084fc",
    "#f472b6",
    "#fb7185",
    "#fb923c",
    "#facc15",
    "#a3e635",
    "#4ade80",
    "#2dd4bf",
    "#22d3ee",
    "#60a5fa",
    "#a78bfa",
    "#e879f9",
]


def split_csv_arg(value: str | None) -> list[str]:
    if not value:
        return []
    return [item.strip() for item in value.split(",") if item.strip()]


def parse_resolution(res: str) -> tuple[int, int]:
    parts = res.lower().split("x")
    if len(parts) != 2:
        raise ValueError(f"invalid resolution '{res}' (expected WxH)")
    w, h = int(parts[0]), int(parts[1])
    if w <= 0 or h <= 0:
        raise ValueError(f"resolution dimensions must be positive: '{res}'")
    return w, h


def delta_pct(value: float, baseline: float) -> float | None:
    if baseline is None or abs(baseline) < 1e-9:
        return None
    return ((value - baseline) / abs(baseline)) * 100.0


def fmt_delta(pct: float | None, higher_is_better: bool = True) -> str:
    if pct is None:
        return ""
    sign = "+" if pct >= 0 else ""
    return f" ({sign}{pct:.1f}%)"


def fmt_delta_badge(pct: float | None, higher_is_better: bool = True) -> str:
    if pct is None:
        return ""
    good = (pct >= 0.5) if higher_is_better else (pct <= -0.5)
    bad = (pct <= -0.5) if higher_is_better else (pct >= 0.5)
    cls = "delta-good" if good else ("delta-bad" if bad else "delta-flat")
    sign = "+" if pct >= 0 else ""
    return f'<span class="delta {cls}">{sign}{pct:.1f}%</span>'


def build_carview_args(
    *,
    bin_path: str,
    scenario: str,
    preset: str,
    resolution: str,
    seconds: float,
    warmup: float,
    json_out: str,
    csv_out: str | None,
    full_fx: bool,
    bench_audio: bool,
    device_label: str | None,
    play_dir: str | None = None,
    bake_dir: str | None = None,
    world_dir: str | None = None,
    game_dir: str | None = None,
    cache_path: str | None = None,
    sound_dir: str | None = None,
    car: str | None = None,
) -> list[str]:
    cmd = [
        bin_path,
        "--drive",
        "--no-settings",
        "--no-save",
        "--bench-scenario",
        scenario,
        "--preset",
        preset,
        "--size",
        resolution,
        "--seconds",
        f"{seconds:g}",
        "--bench-warmup",
        f"{warmup:g}",
        "--bench-json",
        json_out,
    ]
    if csv_out:
        cmd += ["--bench-csv", csv_out]
    if full_fx:
        cmd.append("--bench-full-fx")
    if bench_audio:
        cmd.append("--bench-audio")
    if device_label:
        cmd += ["--device-name", device_label]
    if play_dir:
        cmd += ["--play", play_dir]
    if bake_dir:
        cmd += ["--bake", bake_dir]
    if world_dir:
        cmd += ["--world", world_dir]
    if game_dir:
        cmd += ["--game", game_dir]
    if cache_path:
        cmd += ["--cache", cache_path]
    if sound_dir:
        cmd += ["--sound", sound_dir]
    if car:
        cmd += ["--car", car]
    return cmd


def build_remote_ssh_command(host: str, remote_cmd: list[str], ssh_opts: list[str] | None = None) -> list[str]:
    quoted = " ".join(shlex.quote(arg) for arg in remote_cmd)
    cmd = ["ssh"]
    if ssh_opts:
        cmd.extend(ssh_opts)
    cmd.extend([host, quoted])
    return cmd


def resolve_local_assets(args: argparse.Namespace) -> dict[str, str | None]:
    """Resolve --play or fallback out/ assets when running locally."""
    play_dir = args.play
    bake_dir = args.bake
    world_dir = args.world
    game_dir = args.game
    cache_path = args.cache
    sound_dir = args.sound
    car = args.car

    if play_dir and not args.remote:
        p = Path(play_dir)
        if not (p / "play.txt").is_file():
            # Convenience: allow pointing --play at an out/ folder containing bake/ & world/colorado.index.
            if (p / "bake").is_dir() and not bake_dir:
                bake_dir = str(p / "bake")
                if not cache_path and (p / "world" / "colorado.index").is_file():
                    cache_path = str(p / "world" / "colorado.index")
                if not sound_dir and (p / "sound").is_dir():
                    sound_dir = str(p / "sound")
                play_dir = None

    if not args.remote and not play_dir and not car:
        car = "NIS_SkylineGTRVSpecII_02"

    return {
        "play_dir": play_dir,
        "bake_dir": bake_dir,
        "world_dir": world_dir,
        "game_dir": game_dir,
        "cache_path": cache_path,
        "sound_dir": sound_dir,
        "car": car,
    }


def load_benchmark_inputs(paths: list[str | Path]) -> list[dict[str, Any]]:
    """Load one or more single-run JSONs, suite bundles, or directories into flat run dicts."""
    runs: list[dict[str, Any]] = []
    for raw in paths:
        p = Path(raw)
        if p.is_dir():
            suite_file = p / "suite.json"
            if suite_file.is_file():
                runs.extend(load_benchmark_inputs([suite_file]))
            else:
                for child in sorted(p.glob("*.json")):
                    runs.extend(load_benchmark_inputs([child]))
            continue
        if not p.is_file():
            raise FileNotFoundError(f"benchmark input not found: {p}")
        data = json.loads(p.read_text(encoding="utf-8"))
        if data.get("bundle_type") == "suite" and isinstance(data.get("runs"), list):
            for entry in data["runs"]:
                if isinstance(entry, dict) and entry.get("schema_version") == 1:
                    entry = dict(entry)
                    entry["_source_file"] = str(p)
                    runs.append(entry)
        elif data.get("schema_version") == 1:
            data = dict(data)
            data["_source_file"] = str(p)
            runs.append(data)
        else:
            raise ValueError(f"unsupported benchmark JSON schema in {p}")
    return runs


def run_device_label(run: dict[str, Any]) -> str:
    b = run.get("build", {})
    g = run.get("gpu", {})
    label = b.get("device_label") or g.get("device_name") or "Unknown Device"
    return str(label)


def run_case_key(run: dict[str, Any]) -> tuple[str, str, str]:
    b = run.get("build", {})
    scenario = str(b.get("scenario", "custom"))
    preset = str(b.get("preset", "high"))
    res = f"{int(b.get('output_width', 0))}x{int(b.get('output_height', 0))}"
    return (scenario, preset, res)


def ordered_unique(items: list[Any]) -> list[Any]:
    seen = set()
    out = []
    for x in items:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def generate_markdown_report(runs: list[dict[str, Any]], title: str = "shift-engine Hardware Benchmark Report") -> str:
    if not runs:
        return f"# {title}\n\nNo benchmark runs found.\n"

    devices = ordered_unique([run_device_label(r) for r in runs])
    baseline_dev = devices[0]
    cases = ordered_unique([run_case_key(r) for r in runs])

    # Index: (case_key, device) -> run
    by_case_dev: dict[tuple[tuple[str, str, str], str], dict[str, Any]] = {}
    first_by_dev: dict[str, dict[str, Any]] = {}
    for r in runs:
        dev = run_device_label(r)
        first_by_dev.setdefault(dev, r)
        by_case_dev[(run_case_key(r), dev)] = r

    lines: list[str] = []
    lines.append(f"# {title}")
    lines.append("")
    lines.append(f"Generated: `{dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}` | Runs: `{len(runs)}` | Devices: `{len(devices)}`")
    if len(devices) > 1:
        lines.append(f"Baseline: **{baseline_dev}**")
    lines.append("")

    # 1. Hardware & Build Fingerprint
    lines.append("## Hardware & Build Fingerprint")
    lines.append("")
    lines.append("| Device | OS / Arch | CPU | RAM | GPU | Driver / API | Features | Commit |")
    lines.append("| --- | --- | --- | ---: | --- | --- | --- | --- |")
    for dev in devices:
        r = first_by_dev[dev]
        hw = r.get("hardware", {})
        g = r.get("gpu", {})
        f = r.get("features", {})
        b = r.get("build", {})
        feat_tags = []
        if f.get("gpu_driven"):
            feat_tags.append("GPU-world")
        if f.get("hiz"):
            feat_tags.append("Hi-Z")
        if f.get("vrs_active"):
            feat_tags.append("VRS")
        if g.get("is_moltenvk"):
            feat_tags.append("MoltenVK")
        if g.get("is_turnip"):
            feat_tags.append("Turnip")
        feat_tags.append(f"MSAA {f.get('msaa', 1)}x")
        if f.get("hdr_mode", "off") != "off":
            feat_tags.append(str(f.get("hdr_mode")).upper())
        if f.get("full_fx"):
            feat_tags.append("Full-FX")
        drv = g.get("driver_name") or g.get("driver_info") or str(g.get("driver_version_raw", ""))
        lines.append(
            f"| {dev} | {hw.get('os_name', '?')} {hw.get('os_version', '')} ({hw.get('cpu_arch', '?')}) "
            f"| {hw.get('cpu_brand', '?')} ({hw.get('cpu_logical_cores', 0)}T) "
            f"| {hw.get('system_ram_mib', 0.0) / 1024.0:.1f} GiB "
            f"| {g.get('device_name', '?')} ({g.get('vram_total_mib', 0.0) / 1024.0:.1f} GiB) "
            f"| {drv} / Vk {g.get('vulkan_api_version', '?')} "
            f"| {', '.join(feat_tags)} | `{b.get('git_commit', '?')}` |"
        )
    lines.append("")

    # 2. Performance & Pacing Summary
    lines.append("## Frame Pacing & Throughput Summary")
    lines.append("")
    lines.append(
        "| Scenario | Preset | Res | Device | Avg FPS | 1% Low | 0.1% Low | Frame p50 / p99 (ms) "
        "| CPU avg (ms) | GPU avg (ms) | Hitch-Free | >16.7ms | Peak RSS | VRAM Used | TTFF |"
    )
    lines.append(
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    )
    for ck in cases:
        scen, preset, res = ck
        base_run = by_case_dev.get((ck, baseline_dev))
        for dev in devices:
            r = by_case_dev.get((ck, dev))
            if not r:
                continue
            p = r.get("pacing", {})
            st = r.get("stutter", {})
            mem = r.get("memory", {})
            su = r.get("startup_ms", {})
            is_cmp = len(devices) > 1 and dev != baseline_dev and base_run is not None
            bp = base_run.get("pacing", {}) if base_run else {}
            bsu = base_run.get("startup_ms", {}) if base_run else {}

            fps_d = fmt_delta(delta_pct(p.get("fps_avg", 0.0), bp.get("fps_avg", 0.0)), True) if is_cmp else ""
            l1_d = fmt_delta(delta_pct(p.get("fps_1pct_low", 0.0), bp.get("fps_1pct_low", 0.0)), True) if is_cmp else ""
            cpu_d = fmt_delta(delta_pct(p.get("cpu_ms", {}).get("avg", 0.0), bp.get("cpu_ms", {}).get("avg", 0.0)), False) if is_cmp else ""
            gpu_avg = p.get("gpu_ms", {}).get("avg", 0.0) if p.get("has_gpu_timestamps") else -1.0
            bgpu_avg = bp.get("gpu_ms", {}).get("avg", 0.0) if bp.get("has_gpu_timestamps") else -1.0
            gpu_str = f"{gpu_avg:.2f}" if gpu_avg >= 0 else "n/a"
            if is_cmp and gpu_avg >= 0 and bgpu_avg > 0:
                gpu_str += fmt_delta(delta_pct(gpu_avg, bgpu_avg), False)
            ttff_d = fmt_delta(delta_pct(su.get("ttff_ms", 0.0), bsu.get("ttff_ms", 0.0)), False) if is_cmp else ""

            lines.append(
                f"| `{scen}` | {preset} | {res} | {dev} "
                f"| **{p.get('fps_avg', 0.0):.1f}**{fps_d} "
                f"| {p.get('fps_1pct_low', 0.0):.1f}{l1_d} "
                f"| {p.get('fps_0_1pct_low', 0.0):.1f} "
                f"| {p.get('frame_ms', {}).get('p50', 0.0):.2f} / {p.get('frame_ms', {}).get('p99', 0.0):.2f} "
                f"| {p.get('cpu_ms', {}).get('avg', 0.0):.2f}{cpu_d} "
                f"| {gpu_str} "
                f"| {st.get('hitch_free_pct', 0.0):.1f}% "
                f"| {st.get('frames_over_16_67ms', 0)} "
                f"| {mem.get('rss_peak_mib', 0.0):.0f} MiB "
                f"| {mem.get('vma_used_mib', 0.0):.0f} MiB "
                f"| {su.get('ttff_ms', 0.0):.0f} ms{ttff_d} |"
            )
    lines.append("")

    # 3. CPU Phase Breakdown
    lines.append("## CPU Phase Breakdown (avg ms)")
    lines.append("")
    lines.append("| Scenario / Device | Physics | AI/Traffic | Stream | Audio Push | UI4 Build | Pose/Cull | Cmd Record | Submit/Wait | CPU % (1 Core / All) |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for ck in cases:
        for dev in devices:
            r = by_case_dev.get((ck, dev))
            if not r:
                continue
            cp = r.get("cpu_phases_ms", {})
            cu = r.get("cpu_util", {})
            label = f"`{ck[0]}` ({ck[1]} {ck[2]}) — {dev}"
            lines.append(
                f"| {label} "
                f"| {cp.get('physics_ms', {}).get('avg', 0.0):.2f} "
                f"| {cp.get('ai_traffic_ms', {}).get('avg', 0.0):.2f} "
                f"| {cp.get('world_stream_ms', {}).get('avg', 0.0):.2f} "
                f"| {cp.get('audio_push_ms', {}).get('avg', 0.0):.2f} "
                f"| {cp.get('ui_build_ms', {}).get('avg', 0.0):.2f} "
                f"| {cp.get('pose_cull_ms', {}).get('avg', 0.0):.2f} "
                f"| {cp.get('cmd_record_ms', {}).get('avg', 0.0):.2f} "
                f"| {cp.get('submit_wait_ms', {}).get('avg', 0.0):.2f} "
                f"| {cu.get('pct_of_one_core', 0.0):.0f}% / {cu.get('pct_of_all_cores', 0.0):.1f}% |"
            )
    lines.append("")

    # 4. GPU Pass Breakdown
    all_passes: list[str] = []
    for r in runs:
        for pn in r.get("gpu_passes_ms", {}).keys():
            if pn not in all_passes:
                all_passes.append(pn)
    if all_passes:
        lines.append("## GPU Pass Vulkan Timestamp Breakdown (p50 ms)")
        lines.append("")
        lines.append("| Scenario / Device | " + " | ".join(all_passes) + " |")
        lines.append("| --- | " + " | ".join(["---:"] * len(all_passes)) + " |")
        for ck in cases:
            for dev in devices:
                r = by_case_dev.get((ck, dev))
                if not r:
                    continue
                gp = r.get("gpu_passes_ms", {})
                vals = [f"{gp[pn].get('p50', 0.0):.2f}" if pn in gp else "-" for pn in all_passes]
                lines.append(f"| `{ck[0]}` ({ck[1]} {ck[2]}) — {dev} | " + " | ".join(vals) + " |")
        lines.append("")

    # 5. Geometry, Audio, Streaming & UI4 Atlas
    lines.append("## Geometry, Streaming, Audio & UI4 Atlas")
    lines.append("")
    lines.append(
        "| Scenario / Device | World Draws (Culled) | Tris Drawn | UI4 Shapes | Atlas Util (Evict) "
        "| Tex Resident (MiB) | Audio Blocks (p95 / Stalls) | Car Switch Avg |"
    )
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for ck in cases:
        for dev in devices:
            r = by_case_dev.get((ck, dev))
            if not r:
                continue
            geo = r.get("geometry", {})
            ua = r.get("ui4_atlas", {})
            st = r.get("streaming", {})
            au = r.get("audio", {})
            su = r.get("startup_ms", {})
            aud_str = (
                f"{au.get('blocks_rendered', 0)} ({au.get('p95_block_ms', 0.0):.2f}ms / {au.get('stalls', 0)})"
                if au.get("enabled")
                else "off"
            )
            sw_str = (
                f"{su.get('car_switch_avg_ms', 0.0):.1f} ms ({len(su.get('car_switches', []))})"
                if su.get("car_switches")
                else "-"
            )
            lines.append(
                f"| `{ck[0]}` ({ck[1]} {ck[2]}) — {dev} "
                f"| {geo.get('world_draws', {}).get('avg', 0.0):.0f} ({geo.get('world_culled', {}).get('avg', 0.0):.0f}) "
                f"| {geo.get('triangles_drawn', {}).get('avg', 0.0) / 1000.0:.0f}k "
                f"| {geo.get('ui4_shapes', {}).get('avg', 0.0):.0f} (pk {geo.get('ui4_shapes', {}).get('peak', 0)}) "
                f"| {ua.get('utilization_pct', 0.0):.1f}% ({ua.get('transient_evictions', 0)}) "
                f"| {st.get('resident_textures', 0)} ({st.get('resident_textures_mib', 0.0):.1f}) "
                f"| {aud_str} | {sw_str} |"
            )
    lines.append("")
    return "\n".join(lines)


def _svg_fps_chart(runs: list[dict[str, Any]], baseline_by_case: dict[tuple[str, str, str], dict[str, Any]]) -> str:
    if not runs:
        return ""
    max_fps = max((r.get("pacing", {}).get("fps_avg", 0.0) for r in runs), default=60.0)
    max_fps = max(max_fps * 1.15, 30.0)
    row_h = 54
    left_pad = 240
    right_pad = 150
    chart_w = 560
    total_w = left_pad + chart_w + right_pad
    total_h = 36 + len(runs) * row_h + 16

    parts = [
        f'<svg viewBox="0 0 {total_w} {total_h}" width="100%" class="chart-svg" role="img" aria-label="FPS Comparison">',
        '<g font-family="ui-monospace,SFMono-Regular,Menlo,Consolas,monospace" font-size="11">',
    ]
    # Vertical grid lines
    for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
        gx = left_pad + frac * chart_w
        val = frac * max_fps
        parts.append(f'<line x1="{gx:.1f}" y1="20" x2="{gx:.1f}" y2="{total_h - 10}" stroke="#27272a" stroke-width="1"/>')
        parts.append(f'<text x="{gx:.1f}" y="14" fill="#94a3b8" text-anchor="middle">{val:.0f} FPS</text>')

    for idx, r in enumerate(runs):
        y0 = 28 + idx * row_h
        p = r.get("pacing", {})
        ck = run_case_key(r)
        dev = run_device_label(r)
        label = f"{ck[0]} · {ck[1]} · {ck[2]}"
        avg_fps = p.get("fps_avg", 0.0)
        low1 = p.get("fps_1pct_low", 0.0)
        low01 = p.get("fps_0_1pct_low", 0.0)
        base = baseline_by_case.get(ck)
        d_str = ""
        if base is not None and base is not r:
            dp = delta_pct(avg_fps, base.get("pacing", {}).get("fps_avg", 0.0))
            if dp is not None:
                d_str = f" ({'+' if dp >= 0 else ''}{dp:.1f}%)"

        w_avg = max(2.0, (avg_fps / max_fps) * chart_w)
        w_l1 = max(2.0, (low1 / max_fps) * chart_w)
        w_l01 = max(2.0, (low01 / max_fps) * chart_w)

        parts.append(f'<text x="{left_pad - 10}" y="{y0 + 14}" fill="#f1f5f9" text-anchor="end" font-weight="600">{html.escape(dev[:28])}</text>')
        parts.append(f'<text x="{left_pad - 10}" y="{y0 + 29}" fill="#94a3b8" text-anchor="end" font-size="10">{html.escape(label[:34])}</text>')

        parts.append(f'<rect x="{left_pad}" y="{y0 + 2}" width="{w_avg:.1f}" height="12" rx="3" fill="#38bdf8"/>')
        parts.append(f'<rect x="{left_pad}" y="{y0 + 17}" width="{w_l1:.1f}" height="10" rx="3" fill="#a78bfa"/>')
        parts.append(f'<rect x="{left_pad}" y="{y0 + 30}" width="{w_l01:.1f}" height="8" rx="3" fill="#fb7185"/>')

        parts.append(f'<text x="{left_pad + w_avg + 6:.1f}" y="{y0 + 12}" fill="#e2e8f0">{avg_fps:.1f}{html.escape(d_str)}</text>')
        parts.append(f'<text x="{left_pad + w_l1 + 6:.1f}" y="{y0 + 26}" fill="#cbd5e1" font-size="10">1% {low1:.1f}</text>')
        parts.append(f'<text x="{left_pad + w_l01 + 6:.1f}" y="{y0 + 38}" fill="#fda4af" font-size="9">0.1% {low01:.1f}</text>')

    parts.append("</g></svg>")
    return "\n".join(parts)


def _svg_stacked_bars(
    rows: list[tuple[str, list[tuple[str, float, str]]]],
    unit: str = "ms",
    aria_label: str = "Breakdown",
) -> str:
    if not rows:
        return ""
    totals = [sum(v for _, v, _ in segs) for _, segs in rows]
    max_total = max(max(totals, default=1.0) * 1.15, 0.5)
    row_h = 34
    left_pad = 240
    right_pad = 90
    chart_w = 560
    total_w = left_pad + chart_w + right_pad
    total_h = 30 + len(rows) * row_h + 10

    parts = [
        f'<svg viewBox="0 0 {total_w} {total_h}" width="100%" class="chart-svg" role="img" aria-label="{html.escape(aria_label)}">',
        '<g font-family="ui-monospace,SFMono-Regular,Menlo,Consolas,monospace" font-size="11">',
    ]
    for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
        gx = left_pad + frac * chart_w
        parts.append(f'<line x1="{gx:.1f}" y1="18" x2="{gx:.1f}" y2="{total_h - 6}" stroke="#27272a" stroke-width="1"/>')
        parts.append(f'<text x="{gx:.1f}" y="12" fill="#94a3b8" text-anchor="middle">{frac * max_total:.2f} {unit}</text>')

    for idx, (label, segs) in enumerate(rows):
        y = 24 + idx * row_h
        parts.append(f'<text x="{left_pad - 10}" y="{y + 15}" fill="#e2e8f0" text-anchor="end">{html.escape(label[:34])}</text>')
        x = float(left_pad)
        row_sum = 0.0
        for name, val, color in segs:
            if val <= 0.0:
                continue
            row_sum += val
            w = (val / max_total) * chart_w
            parts.append(
                f'<rect x="{x:.2f}" y="{y + 2}" width="{max(w, 0.8):.2f}" height="18" fill="{color}">'
                f'<title>{html.escape(name)}: {val:.2f} {unit}</title></rect>'
            )
            x += w
        parts.append(f'<text x="{x + 8:.1f}" y="{y + 15}" fill="#cbd5e1">{row_sum:.2f} {unit}</text>')

    parts.append("</g></svg>")
    return "\n".join(parts)


def generate_html_dashboard(runs: list[dict[str, Any]], title: str = "shift-engine Hardware Benchmark Dashboard") -> str:
    devices = ordered_unique([run_device_label(r) for r in runs])
    baseline_dev = devices[0] if devices else ""
    cases = ordered_unique([run_case_key(r) for r in runs])

    first_by_dev: dict[str, dict[str, Any]] = {}
    by_case_dev: dict[tuple[tuple[str, str, str], str], dict[str, Any]] = {}
    baseline_by_case: dict[tuple[str, str, str], dict[str, Any]] = {}
    for r in runs:
        dev = run_device_label(r)
        ck = run_case_key(r)
        first_by_dev.setdefault(dev, r)
        by_case_dev[(ck, dev)] = r
        if dev == baseline_dev:
            baseline_by_case.setdefault(ck, r)

    # Top KPI cards (summary across runs)
    avg_fps_all = sum(r.get("pacing", {}).get("fps_avg", 0.0) for r in runs) / max(1, len(runs))
    low1_all = sum(r.get("pacing", {}).get("fps_1pct_low", 0.0) for r in runs) / max(1, len(runs))
    hitch_free_all = sum(r.get("stutter", {}).get("hitch_free_pct", 0.0) for r in runs) / max(1, len(runs))
    peak_rss_all = max((r.get("memory", {}).get("rss_peak_mib", 0.0) for r in runs), default=0.0)
    peak_vram_all = max((r.get("memory", {}).get("vma_used_mib", 0.0) for r in runs), default=0.0)
    avg_ttff = sum(r.get("startup_ms", {}).get("ttff_ms", 0.0) for r in runs) / max(1, len(runs))

    # Build CPU stacked rows
    cpu_rows = []
    for r in runs:
        cp = r.get("cpu_phases_ms", {})
        ck = run_case_key(r)
        lbl = f"{run_device_label(r)[:14]} · {ck[0]}"
        segs = [(name, float(cp.get(key, {}).get("avg", 0.0)), color) for key, name, color in CPU_PHASE_META]
        cpu_rows.append((lbl, segs))

    # Build GPU stacked rows
    all_passes: list[str] = []
    for r in runs:
        for pn in r.get("gpu_passes_ms", {}).keys():
            if pn not in all_passes:
                all_passes.append(pn)
    pass_color = {pn: GPU_PASS_COLORS[i % len(GPU_PASS_COLORS)] for i, pn in enumerate(all_passes)}
    gpu_rows = []
    for r in runs:
        gp = r.get("gpu_passes_ms", {})
        if not gp:
            continue
        ck = run_case_key(r)
        lbl = f"{run_device_label(r)[:14]} · {ck[0]}"
        segs = [(pn, float(gp.get(pn, {}).get("p50", 0.0)), pass_color[pn]) for pn in all_passes if pn in gp]
        gpu_rows.append((lbl, segs))

    # Hardware specs table rows
    hw_rows_html = []
    for dev in devices:
        r = first_by_dev[dev]
        hw = r.get("hardware", {})
        g = r.get("gpu", {})
        f = r.get("features", {})
        b = r.get("build", {})
        flags = []
        if f.get("gpu_driven"):
            flags.append("GPU-Driven")
        if f.get("hiz"):
            flags.append("Hi-Z")
        if f.get("bindless"):
            flags.append("Bindless")
        if f.get("vrs_active"):
            flags.append("VRS")
        if g.get("is_moltenvk"):
            flags.append("MoltenVK")
        if g.get("is_turnip"):
            flags.append("Turnip")
        flags.append(f"MSAA {f.get('msaa', 1)}x")
        if f.get("hdr_mode", "off") != "off":
            flags.append(str(f.get("hdr_mode")).upper())
        if f.get("full_fx"):
            flags.append("Full-FX")
        hw_rows_html.append(
            "<tr>"
            f"<td><strong>{html.escape(dev)}</strong></td>"
            f"<td>{html.escape(str(hw.get('os_name', '?')))} {html.escape(str(hw.get('os_version', '')))} "
            f"<span class='muted'>({html.escape(str(hw.get('cpu_arch', '?')))})</span></td>"
            f"<td>{html.escape(str(hw.get('cpu_brand', '?')))} <span class='muted'>({int(hw.get('cpu_logical_cores', 0))}T)</span></td>"
            f"<td class='num'>{hw.get('system_ram_mib', 0.0) / 1024.0:.1f} GiB</td>"
            f"<td>{html.escape(str(g.get('device_name', '?')))}<br><span class='muted'>"
            f"{html.escape(str(g.get('driver_name', '')))} · Vk {html.escape(str(g.get('vulkan_api_version', '?')))} · "
            f"{g.get('vram_total_mib', 0.0) / 1024.0:.1f} GiB VRAM</span></td>"
            f"<td>{''.join(f'<span class=\"pill\">{html.escape(x)}</span>' for x in flags)}</td>"
            f"<td><code>{html.escape(str(b.get('git_commit', '?')))}</code></td>"
            "</tr>"
        )

    # Scenario comparison table rows
    cmp_rows_html = []
    for ck in cases:
        scen, preset, res = ck
        base_run = by_case_dev.get((ck, baseline_dev))
        for dev in devices:
            r = by_case_dev.get((ck, dev))
            if not r:
                continue
            p = r.get("pacing", {})
            st = r.get("stutter", {})
            mem = r.get("memory", {})
            su = r.get("startup_ms", {})
            is_cmp = len(devices) > 1 and dev != baseline_dev and base_run is not None
            bp = base_run.get("pacing", {}) if base_run else {}
            bsu = base_run.get("startup_ms", {}) if base_run else {}
            bmem = base_run.get("memory", {}) if base_run else {}

            fps_b = fmt_delta_badge(delta_pct(p.get("fps_avg", 0.0), bp.get("fps_avg", 0.0)), True) if is_cmp else ""
            l1_b = fmt_delta_badge(delta_pct(p.get("fps_1pct_low", 0.0), bp.get("fps_1pct_low", 0.0)), True) if is_cmp else ""
            cpu_b = fmt_delta_badge(delta_pct(p.get("cpu_ms", {}).get("avg", 0.0), bp.get("cpu_ms", {}).get("avg", 0.0)), False) if is_cmp else ""
            gpu_val = p.get("gpu_ms", {}).get("avg", 0.0) if p.get("has_gpu_timestamps") else -1.0
            bgpu_val = bp.get("gpu_ms", {}).get("avg", 0.0) if bp.get("has_gpu_timestamps") else -1.0
            gpu_cell = f"{gpu_val:.2f} ms" if gpu_val >= 0 else "<span class='muted'>n/a</span>"
            if is_cmp and gpu_val >= 0 and bgpu_val > 0:
                gpu_cell += fmt_delta_badge(delta_pct(gpu_val, bgpu_val), False)
            ttff_b = fmt_delta_badge(delta_pct(su.get("ttff_ms", 0.0), bsu.get("ttff_ms", 0.0)), False) if is_cmp else ""
            vram_b = fmt_delta_badge(delta_pct(mem.get("vma_used_mib", 0.0), bmem.get("vma_used_mib", 0.0)), False) if is_cmp else ""

            cmp_rows_html.append(
                "<tr>"
                f"<td><code>{html.escape(scen)}</code></td>"
                f"<td>{html.escape(preset)}</td>"
                f"<td>{html.escape(res)}</td>"
                f"<td><strong>{html.escape(dev)}</strong></td>"
                f"<td class='num'><strong>{p.get('fps_avg', 0.0):.1f}</strong>{fps_b}</td>"
                f"<td class='num'>{p.get('fps_1pct_low', 0.0):.1f}{l1_b}</td>"
                f"<td class='num'>{p.get('fps_0_1pct_low', 0.0):.1f}</td>"
                f"<td class='num'>{p.get('frame_ms', {}).get('p50', 0.0):.2f} / {p.get('frame_ms', {}).get('p99', 0.0):.2f}</td>"
                f"<td class='num'>{p.get('cpu_ms', {}).get('avg', 0.0):.2f} ms{cpu_b}</td>"
                f"<td class='num'>{gpu_cell}</td>"
                f"<td class='num'>{st.get('hitch_free_pct', 0.0):.1f}%</td>"
                f"<td class='num'>{st.get('frames_over_16_67ms', 0)} / {st.get('frames_over_33_33ms', 0)} / {st.get('frames_over_50ms', 0)}</td>"
                f"<td class='num'>{mem.get('rss_peak_mib', 0.0):.0f} MiB</td>"
                f"<td class='num'>{mem.get('vma_used_mib', 0.0):.0f} MiB{vram_b}</td>"
                f"<td class='num'>{su.get('ttff_ms', 0.0):.0f} ms{ttff_b}</td>"
                "</tr>"
            )

    # Top slow frames & startup table
    slow_rows_html = []
    for r in runs:
        ck = run_case_key(r)
        dev = run_device_label(r)
        su = r.get("startup_ms", {})
        st = r.get("stutter", {})
        au = r.get("audio", {})
        top = st.get("top5_slowest_frames", [])
        top_desc = "; ".join(
            f"f#{x.get('frame_index')} {x.get('frame_ms', 0.0):.1f}ms ({x.get('dominant_phase', '?')})"
            for x in top[:3]
        ) or "-"
        aud_desc = (
            f"{au.get('blocks_rendered', 0)} blk · avg {au.get('avg_block_ms', 0.0):.2f}ms · p95 {au.get('p95_block_ms', 0.0):.2f}ms · stalls {au.get('stalls', 0)}"
            if au.get("enabled")
            else "disabled"
        )
        slow_rows_html.append(
            "<tr>"
            f"<td><code>{html.escape(ck[0])}</code> · {html.escape(dev)}</td>"
            f"<td class='num'>{su.get('vulkan_init_ms', 0.0):.0f} / {su.get('pipeline_compile_ms', 0.0):.0f} / "
            f"{su.get('car_bake_load_ms', 0.0):.0f} / {su.get('world_load_ms', 0.0):.0f} ms</td>"
            f"<td class='num'>{st.get('worst_hitch_ms', 0.0):.2f} ms @ {st.get('worst_hitch_sim_sec', 0.0):.2f}s</td>"
            f"<td>{html.escape(top_desc)}</td>"
            f"<td>{html.escape(aud_desc)}</td>"
            "</tr>"
        )

    cpu_legend = "".join(
        f'<span class="legend-item"><i style="background:{c}"></i>{html.escape(n)}</span>'
        for _, n, c in CPU_PHASE_META
    )
    gpu_legend = "".join(
        f'<span class="legend-item"><i style="background:{pass_color[pn]}"></i>{html.escape(pn)}</span>'
        for pn in all_passes
    )
    fps_legend = (
        '<span class="legend-item"><i style="background:#38bdf8"></i>Avg FPS</span>'
        '<span class="legend-item"><i style="background:#a78bfa"></i>1% Low FPS</span>'
        '<span class="legend-item"><i style="background:#fb7185"></i>0.1% Low FPS</span>'
    )

    generated_at = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(title)}</title>
<style>
  :root {{
    --bg: #09090b; --card: #121216; --card2: #18181d; --border: #27272a;
    --text: #f4f4f5; --muted: #a1a1aa; --accent: #38bdf8;
    --good: #4ade80; --bad: #fb7185; --flat: #94a3b8;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0; padding: 28px 24px 64px; background: var(--bg); color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Inter, sans-serif;
    line-height: 1.45;
  }}
  .wrap {{ max-width: 1440px; margin: 0 auto; }}
  header {{ display: flex; justify-content: space-between; align-items: baseline; flex-wrap: wrap; gap: 12px; margin-bottom: 24px; border-bottom: 1px solid var(--border); padding-bottom: 16px; }}
  h1 {{ margin: 0; font-size: 24px; font-weight: 700; letter-spacing: -0.02em; }}
  h2 {{ margin: 28px 0 12px; font-size: 16px; font-weight: 600; color: #e4e4e7; text-transform: uppercase; letter-spacing: 0.06em; }}
  .meta {{ color: var(--muted); font-size: 13px; }}
  .kpi-grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 14px; margin-bottom: 24px; }}
  .kpi {{ background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 14px 16px; }}
  .kpi-label {{ color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; }}
  .kpi-val {{ font-size: 26px; font-weight: 700; margin-top: 4px; color: var(--text); font-variant-numeric: tabular-nums; }}
  .card {{ background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 16px; margin-bottom: 20px; overflow-x: auto; }}
  table {{ width: 100%; border-collapse: collapse; font-size: 13px; }}
  th, td {{ padding: 8px 10px; border-bottom: 1px solid var(--border); text-align: left; vertical-align: middle; }}
  th {{ color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.06em; background: var(--card2); }}
  td.num, th.num {{ text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }}
  tr:hover td {{ background: rgba(56,189,248,0.04); }}
  code {{ background: #1e293b; color: #7dd3fc; padding: 2px 6px; border-radius: 4px; font-size: 12px; }}
  .muted {{ color: var(--muted); font-size: 12px; }}
  .pill {{ display: inline-block; background: #1f2937; border: 1px solid #334155; color: #cbd5e1; font-size: 11px; padding: 1px 6px; border-radius: 999px; margin: 1px 3px 1px 0; }}
  .delta {{ display: inline-block; margin-left: 6px; padding: 1px 5px; border-radius: 4px; font-size: 11px; font-weight: 600; }}
  .delta-good {{ background: rgba(74,222,128,0.14); color: var(--good); }}
  .delta-bad {{ background: rgba(251,113,133,0.15); color: var(--bad); }}
  .delta-flat {{ background: rgba(148,163,184,0.14); color: var(--flat); }}
  .legend {{ display: flex; flex-wrap: wrap; gap: 12px; margin-bottom: 10px; font-size: 12px; color: var(--muted); }}
  .legend-item {{ display: inline-flex; align-items: center; gap: 6px; }}
  .legend-item i {{ display: inline-block; width: 10px; height: 10px; border-radius: 2px; }}
  .chart-svg {{ display: block; max-width: 100%; height: auto; }}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>{html.escape(title)}</h1>
      <div class="meta">Generated {html.escape(generated_at)} · {len(runs)} run(s) · {len(devices)} device(s){f' · Baseline: {html.escape(baseline_dev)}' if len(devices) > 1 else ''}</div>
    </div>
  </header>

  <section class="kpi-grid">
    <div class="kpi"><div class="kpi-label">Mean Avg FPS</div><div class="kpi-val">{avg_fps_all:.1f}</div></div>
    <div class="kpi"><div class="kpi-label">Mean 1% Low FPS</div><div class="kpi-val">{low1_all:.1f}</div></div>
    <div class="kpi"><div class="kpi-label">Mean Hitch-Free</div><div class="kpi-val">{hitch_free_all:.1f}%</div></div>
    <div class="kpi"><div class="kpi-label">Peak Process RSS</div><div class="kpi-val">{peak_rss_all:.0f} MiB</div></div>
    <div class="kpi"><div class="kpi-label">Peak VRAM Used</div><div class="kpi-val">{peak_vram_all:.0f} MiB</div></div>
    <div class="kpi"><div class="kpi-label">Mean TTFF</div><div class="kpi-val">{avg_ttff:.0f} ms</div></div>
  </section>

  <h2>Hardware &amp; Build Fingerprint</h2>
  <div class="card">
    <table>
      <thead><tr><th>Device</th><th>OS / Arch</th><th>CPU</th><th class="num">RAM</th><th>GPU &amp; Driver</th><th>Features</th><th>Commit</th></tr></thead>
      <tbody>{''.join(hw_rows_html)}</tbody>
    </table>
  </div>

  <h2>FPS &amp; Frame Pacing Comparison</h2>
  <div class="card">
    <div class="legend">{fps_legend}</div>
    {_svg_fps_chart(runs, baseline_by_case)}
  </div>

  <div class="card">
    <table>
      <thead>
        <tr>
          <th>Scenario</th><th>Preset</th><th>Res</th><th>Device</th>
          <th class="num">Avg FPS</th><th class="num">1% Low</th><th class="num">0.1% Low</th>
          <th class="num">p50 / p99 ms</th><th class="num">CPU avg</th><th class="num">GPU avg</th>
          <th class="num">Hitch-Free</th><th class="num">&gt;16.7/33/50ms</th>
          <th class="num">Peak RSS</th><th class="num">VRAM</th><th class="num">TTFF</th>
        </tr>
      </thead>
      <tbody>{''.join(cmp_rows_html)}</tbody>
    </table>
  </div>

  <h2>CPU Phase Breakdown (avg ms)</h2>
  <div class="card">
    <div class="legend">{cpu_legend}</div>
    {_svg_stacked_bars(cpu_rows, "ms", "CPU Phase Breakdown")}
  </div>

  <h2>GPU Pass Vulkan Timestamp Breakdown (p50 ms)</h2>
  <div class="card">
    <div class="legend">{gpu_legend if gpu_legend else '<span class="muted">No GPU timestamps reported</span>'}</div>
    {_svg_stacked_bars(gpu_rows, "ms", "GPU Pass Breakdown") if gpu_rows else '<p class="muted">No Vulkan GPU timestamp data in this run.</p>'}
  </div>

  <h2>Startup, Stutter Attribution &amp; Audio Thread</h2>
  <div class="card">
    <table>
      <thead>
        <tr>
          <th>Run</th>
          <th class="num">Startup (Vk / Pipe / Car / World)</th>
          <th class="num">Worst Hitch</th>
          <th>Top Slowest Frames (Phase)</th>
          <th>Audio Thread</th>
        </tr>
      </thead>
      <tbody>{''.join(slow_rows_html)}</tbody>
    </table>
  </div>
</div>
</body>
</html>
"""


def cmd_run(args: argparse.Namespace) -> int:
    suite = SUITE_DEFAULTS[args.suite]
    scenarios = split_csv_arg(args.scenarios) or list(suite["scenarios"])
    presets = split_csv_arg(args.presets) or list(suite["presets"])
    resolutions = split_csv_arg(args.resolutions) or list(suite["resolutions"])
    seconds = args.seconds if args.seconds is not None else float(suite["seconds"])
    warmup = args.warmup if args.warmup is not None else float(suite["warmup"])

    for sc in scenarios:
        if sc not in ALL_SCENARIOS and sc != "custom":
            print(f"error: unknown scenario '{sc}' (valid: {', '.join(ALL_SCENARIOS)})", file=sys.stderr)
            return 2
    for res in resolutions:
        parse_resolution(res)

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    label_slug = "".join(c if c.isalnum() or c in "-_" else "_" for c in (args.device_label or "local")).strip("_") or "local"
    out_dir = Path(args.out) / f"{stamp}_{label_slug}_{args.suite}"
    out_dir.mkdir(parents=True, exist_ok=True)

    assets = resolve_local_assets(args)
    ssh_opts = shlex.split(args.ssh_opts) if args.ssh_opts else []
    remote_root = args.remote_out or f"/tmp/shift-bench-{stamp}"

    collected_runs: list[dict[str, Any]] = []
    total_jobs = len(scenarios) * len(presets) * len(resolutions)
    job_idx = 0

    for scen in scenarios:
        for preset in presets:
            for res in resolutions:
                job_idx += 1
                stem = f"{scen}_{preset}_{res}"
                local_json = out_dir / f"{stem}.json"
                local_csv = None if args.no_csv else (out_dir / f"{stem}.csv")
                print(f"[{job_idx}/{total_jobs}] {scen} | preset={preset} | {res} ({seconds:g}s, warmup {warmup:g}s)")

                if args.remote:
                    remote_json = f"{remote_root}/{stem}.json"
                    remote_csv = None if args.no_csv else f"{remote_root}/{stem}.csv"
                    mkdir_cmd = build_remote_ssh_command(args.remote, ["mkdir", "-p", remote_root], ssh_opts)
                    subprocess.run(mkdir_cmd, check=True)
                    carview_cmd = build_carview_args(
                        bin_path=args.bin,
                        scenario=scen,
                        preset=preset,
                        resolution=res,
                        seconds=seconds,
                        warmup=warmup,
                        json_out=remote_json,
                        csv_out=remote_csv,
                        full_fx=not args.no_full_fx,
                        bench_audio=not args.no_audio,
                        device_label=args.device_label,
                        **assets,
                    )
                    ssh_cmd = build_remote_ssh_command(args.remote, carview_cmd, ssh_opts)
                    proc = subprocess.run(ssh_cmd)
                    if proc.returncode != 0:
                        print(f"error: remote benchmark failed with code {proc.returncode}", file=sys.stderr)
                        return proc.returncode
                    subprocess.run(["scp", *ssh_opts, f"{args.remote}:{remote_json}", str(local_json)], check=True)
                    if remote_csv and local_csv:
                        subprocess.run(["scp", *ssh_opts, f"{args.remote}:{remote_csv}", str(local_csv)], check=True)
                else:
                    carview_cmd = build_carview_args(
                        bin_path=args.bin,
                        scenario=scen,
                        preset=preset,
                        resolution=res,
                        seconds=seconds,
                        warmup=warmup,
                        json_out=str(local_json),
                        csv_out=str(local_csv) if local_csv else None,
                        full_fx=not args.no_full_fx,
                        bench_audio=not args.no_audio,
                        device_label=args.device_label,
                        **assets,
                    )
                    proc = subprocess.run(carview_cmd)
                    if proc.returncode != 0:
                        print(f"error: benchmark '{stem}' failed with exit code {proc.returncode}", file=sys.stderr)
                        return proc.returncode

                if local_json.is_file():
                    collected_runs.append(json.loads(local_json.read_text(encoding="utf-8")))

    bundle = {
        "schema_version": 1,
        "bundle_type": "suite",
        "suite": args.suite,
        "device_label": args.device_label or (run_device_label(collected_runs[0]) if collected_runs else "local"),
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "runs": collected_runs,
    }
    suite_json = out_dir / "suite.json"
    suite_json.write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")

    title = f"shift-engine Benchmark — {bundle['device_label']} ({args.suite})"
    md_text = generate_markdown_report(collected_runs, title=title)
    html_text = generate_html_dashboard(collected_runs, title=title)
    md_path = out_dir / "report.md"
    html_path = out_dir / "report.html"
    md_path.write_text(md_text, encoding="utf-8")
    html_path.write_text(html_text, encoding="utf-8")

    print("")
    print(f"Suite bundle : {suite_json}")
    print(f"Markdown     : {md_path}")
    print(f"HTML report  : {html_path}")
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    runs = load_benchmark_inputs(args.inputs)
    if not runs:
        print("error: no valid schema v1 benchmark runs found in inputs", file=sys.stderr)
        return 2
    title = args.title or "shift-engine Cross-Device Benchmark Comparison"
    md = generate_markdown_report(runs, title=title)
    if args.out:
        out_p = Path(args.out)
        out_p.parent.mkdir(parents=True, exist_ok=True)
        out_p.write_text(md, encoding="utf-8")
        print(f"Wrote Markdown report -> {out_p}")
    else:
        print(md)
    if args.html:
        html_p = Path(args.html)
        html_p.parent.mkdir(parents=True, exist_ok=True)
        html_p.write_text(generate_html_dashboard(runs, title=title), encoding="utf-8")
        print(f"Wrote HTML dashboard  -> {html_p}")
    return 0


def _make_synthetic_run(
    *,
    device_label: str,
    gpu_name: str,
    scenario: str,
    preset: str,
    res: tuple[int, int],
    fps_avg: float,
    fps_1pct: float,
    fps_01pct: float,
    cpu_avg: float,
    gpu_avg: float,
    vram_mib: float,
    rss_mib: float,
    ttff_ms: float,
    moltenvk: bool = False,
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "timestamp_utc": "2026-11-22T20:00:00Z",
        "hardware": {
            "os_name": "macOS" if moltenvk else "Windows",
            "os_version": "15.2" if moltenvk else "10.0.26100",
            "cpu_arch": "arm64" if moltenvk else "x86_64",
            "cpu_logical_cores": 10 if moltenvk else 16,
            "cpu_brand": "Apple M4" if moltenvk else "AMD Ryzen 9 7950X",
            "system_ram_mib": 16384.0 if moltenvk else 32768.0,
        },
        "gpu": {
            "device_name": gpu_name,
            "vendor_id": "0x106b" if moltenvk else "0x10de",
            "device_id": "0x0001",
            "device_type": "integrated" if moltenvk else "discrete",
            "vulkan_api_version": "1.3.296",
            "vulkan_api_version_raw": 4206888,
            "driver_version_raw": 100,
            "driver_name": "MoltenVK" if moltenvk else "NVIDIA",
            "driver_info": "1.2.11" if moltenvk else "566.14",
            "is_moltenvk": moltenvk,
            "is_turnip": False,
            "portability_subset": moltenvk,
            "has_timestamps": True,
            "vram_total_mib": 12288.0,
            "vram_budget_mib": 11000.0,
        },
        "features": {
            "gpu_driven": True,
            "hiz": True,
            "bindless": True,
            "vrs_supported": not moltenvk,
            "vrs_active": False,
            "hdr10_supported": True,
            "scrgb_supported": not moltenvk,
            "hdr_mode": "off",
            "upscaler": "bilinear",
            "frame_gen": "off",
            "aa_mode": "msaa",
            "ssao": "medium",
            "shadows": "high",
            "vsync": "fifo",
            "msaa": 4,
            "anisotropy": 16.0,
            "render_scale": 0,
            "base_540p": False,
            "z_prepass": False,
            "reverse_z": True,
            "bloom": True,
            "full_fx": True,
            "bench_audio": True,
        },
        "build": {
            "git_commit": "abc1234",
            "build_config": "Release",
            "preset": preset,
            "device_label": device_label,
            "scenario": scenario,
            "output_width": res[0],
            "output_height": res[1],
            "render_width": res[0],
            "render_height": res[1],
            "target_seconds": 6.0,
            "warmup_seconds": 1.0,
        },
        "startup_ms": {
            "vulkan_init_ms": 40.0,
            "pipeline_compile_ms": 110.0,
            "db_catalog_load_ms": 80.0,
            "audio_load_ms": 45.0,
            "car_bake_load_ms": 60.0,
            "world_load_ms": 220.0,
            "ttff_ms": ttff_ms,
            "car_switch_avg_ms": 38.5,
            "car_switch_max_ms": 51.0,
            "car_switches": [{"car": "NIS_SkylineGTRVSpecII_02", "load_ms": 18.0, "upload_ms": 20.5, "total_ms": 38.5}],
        },
        "pacing": {
            "total_frames": 300,
            "warmup_frames": 30,
            "wall_sec": 300.0 / fps_avg,
            "sim_sec": 5.0,
            "fps_avg": fps_avg,
            "fps_1pct_low": fps_1pct,
            "fps_0_1pct_low": fps_01pct,
            "fps_min": fps_01pct * 0.9,
            "fps_max": fps_avg * 1.2,
            "frame_ms": {"avg": 1000.0 / fps_avg, "min": 4.0, "p50": 950.0 / fps_avg, "p90": 1100.0 / fps_avg, "p95": 1150.0 / fps_avg, "p99": 1000.0 / fps_1pct, "p99_9": 1000.0 / fps_01pct, "max": 1000.0 / (fps_01pct * 0.9)},
            "cpu_ms": {"avg": cpu_avg, "min": cpu_avg * 0.7, "p50": cpu_avg, "p90": cpu_avg * 1.1, "p95": cpu_avg * 1.2, "p99": cpu_avg * 1.4, "p99_9": cpu_avg * 1.6, "max": cpu_avg * 1.8},
            "has_gpu_timestamps": True,
            "gpu_ms": {"avg": gpu_avg, "min": gpu_avg * 0.8, "p50": gpu_avg, "p90": gpu_avg * 1.1, "p95": gpu_avg * 1.15, "p99": gpu_avg * 1.3, "p99_9": gpu_avg * 1.4, "max": gpu_avg * 1.5},
        },
        "cpu_phases_ms": {
            "physics_ms": {"avg": cpu_avg * 0.12},
            "ai_traffic_ms": {"avg": cpu_avg * 0.14},
            "world_stream_ms": {"avg": cpu_avg * 0.08},
            "audio_push_ms": {"avg": cpu_avg * 0.04},
            "ui_build_ms": {"avg": cpu_avg * 0.10},
            "pose_cull_ms": {"avg": cpu_avg * 0.16},
            "cmd_record_ms": {"avg": cpu_avg * 0.24},
            "submit_wait_ms": {"avg": cpu_avg * 0.12},
        },
        "gpu_passes_ms": {
            "shadows": {"p50": gpu_avg * 0.18},
            "world gpu": {"p50": gpu_avg * 0.36},
            "car_opaque": {"p50": gpu_avg * 0.14},
            "car_glass": {"p50": gpu_avg * 0.06},
            "post": {"p50": gpu_avg * 0.18},
            "ui": {"p50": gpu_avg * 0.08},
        },
        "audio": {
            "enabled": True,
            "blocks_rendered": 250,
            "avg_block_ms": 0.62,
            "p95_block_ms": 1.15,
            "worst_block_ms": 1.85,
            "stalls": 0,
            "main_push_avg_ms": 0.12,
            "main_push_worst_ms": 0.35,
        },
        "stutter": {
            "frames_over_16_67ms": 2,
            "frames_over_33_33ms": 0,
            "frames_over_50ms": 0,
            "hitch_threshold_ms": 15.0,
            "hitch_free_pct": 98.7,
            "worst_hitch_ms": 18.2,
            "worst_hitch_sim_sec": 2.4,
            "worst_hitch_frame": 144,
            "top5_slowest_frames": [
                {"frame_index": 144, "sim_sec": 2.4, "frame_ms": 18.2, "dominant_phase": "world_stream"},
            ],
        },
        "geometry": {
            "world_draws": {"avg": 420.0, "peak": 512},
            "world_culled": {"avg": 260.0, "peak": 340},
            "world_hiz_culled": {"avg": 85.0, "peak": 120},
            "trees_drawn": {"avg": 1800.0, "peak": 2400},
            "shadow_world_draws": {"avg": 140.0, "peak": 180},
            "shadow_cars": {"avg": 4.0, "peak": 6},
            "env_draws": {"avg": 90.0, "peak": 110},
            "mirror_draws": {"avg": 0.0, "peak": 0},
            "car_draws": {"avg": 96.0, "peak": 96},
            "car_instances": {"avg": 4.0, "peak": 6},
            "particle_sprites": {"avg": 48.0, "peak": 110},
            "skid_tris": {"avg": 320.0, "peak": 640},
            "ui4_shapes": {"avg": 210.0, "peak": 315},
            "triangles_drawn": {"avg": 640000.0, "peak": 780000},
            "triangles_total": {"avg": 1800000.0, "peak": 2100000},
        },
        "ui4_atlas": {
            "atlas_size": 4096,
            "glyph_rows_used": 180,
            "persistent_rows_used": 620,
            "transient_rows_used": 240,
            "utilization_pct": 25.4,
            "transient_evictions": 0,
            "glyph_count": 195,
            "image_count": 44,
        },
        "memory": {
            "rss_current_mib": rss_mib * 0.92,
            "rss_peak_mib": rss_mib,
            "vma_blocks": 14,
            "vma_allocs": 310,
            "vma_used_mib": vram_mib,
            "vma_reserved_mib": vram_mib * 1.15,
            "vram_usage_mib": vram_mib * 1.2,
            "vram_budget_mib": 11000.0,
            "vram_total_mib": 12288.0,
        },
        "streaming": {
            "resident_textures": 512,
            "resident_textures_mib": 284.0,
            "texture_uploads": 520,
            "texture_evictions": 8,
            "texture_reuploads": 0,
            "retired_pending": 0,
            "descriptor_pools": 3,
            "staging_upload_bytes": 96 * 1024 * 1024,
            "staging_upload_mib_per_sec": 18.4,
        },
        "cpu_util": {
            "process_cpu_sec": 3.5,
            "wall_sec": 2.5,
            "ms_per_frame": 11.6,
            "pct_of_one_core": 140.0,
            "pct_of_all_cores": 8.75,
            "logical_cores": 16,
        },
    }


def run_self_test() -> int:
    # 1. Delta math
    assert abs((delta_pct(120.0, 100.0) or 0.0) - 20.0) < 1e-6
    assert abs((delta_pct(80.0, 100.0) or 0.0) + 20.0) < 1e-6
    assert delta_pct(50.0, 0.0) is None
    assert fmt_delta(delta_pct(150.0, 100.0)) == " (+50.0%)"
    assert "delta-good" in fmt_delta_badge(25.0, higher_is_better=True)
    assert "delta-bad" in fmt_delta_badge(-25.0, higher_is_better=True)
    assert "delta-good" in fmt_delta_badge(-15.0, higher_is_better=False)
    assert "delta-bad" in fmt_delta_badge(15.0, higher_is_better=False)

    # 2. CLI & SSH command builders
    cmd = build_carview_args(
        bin_path="/usr/bin/carview",
        scenario="noon_cruise",
        preset="high",
        resolution="1920x1080",
        seconds=6.0,
        warmup=1.0,
        json_out="/tmp/out.json",
        csv_out="/tmp/out.csv",
        full_fx=True,
        bench_audio=True,
        device_label="M4 Mac mini",
        play_dir="/data/play",
    )
    assert "--bench-scenario" in cmd and "noon_cruise" in cmd
    assert "--bench-full-fx" in cmd and "--bench-audio" in cmd
    ssh_cmd = build_remote_ssh_command("mac-mini", cmd, ["-p", "2222"])
    assert ssh_cmd[:4] == ["ssh", "-p", "2222", "mac-mini"]
    assert "'M4 Mac mini'" in ssh_cmd[4]

    # 3. Single-run & suite-bundle loading + Markdown & HTML generation
    r1 = _make_synthetic_run(
        device_label="Desktop RTX <4080>",
        gpu_name="NVIDIA GeForce RTX 4080",
        scenario="noon_cruise",
        preset="high",
        res=(1920, 1080),
        fps_avg=160.0,
        fps_1pct=128.0,
        fps_01pct=104.0,
        cpu_avg=3.2,
        gpu_avg=5.1,
        vram_mib=740.0,
        rss_mib=680.0,
        ttff_ms=540.0,
    )
    r2 = _make_synthetic_run(
        device_label="M4 Mac mini",
        gpu_name="Apple M4",
        scenario="noon_cruise",
        preset="high",
        res=(1920, 1080),
        fps_avg=96.0,
        fps_1pct=78.0,
        fps_01pct=62.0,
        cpu_avg=4.1,
        gpu_avg=9.2,
        vram_mib=620.0,
        rss_mib=590.0,
        ttff_ms=610.0,
        moltenvk=True,
    )

    with tempfile.TemporaryDirectory() as td:
        p1 = Path(td) / "r1.json"
        p2 = Path(td) / "suite2.json"
        p1.write_text(json.dumps(r1), encoding="utf-8")
        p2.write_text(json.dumps({"schema_version": 1, "bundle_type": "suite", "runs": [r2]}), encoding="utf-8")
        loaded = load_benchmark_inputs([p1, p2])
        assert len(loaded) == 2

        md = generate_markdown_report(loaded)
        assert "Desktop RTX <4080>" in md
        assert "M4 Mac mini" in md
        assert "(-40.0%)" in md  # 96 vs 160 FPS
        assert "MoltenVK" in md
        assert "GPU Pass Vulkan Timestamp Breakdown" in md

        html_doc = generate_html_dashboard(loaded)
        assert "<!DOCTYPE html>" in html_doc
        assert "Desktop RTX &lt;4080&gt;" in html_doc  # HTML escaped
        assert "<svg " in html_doc
        assert "delta-bad" in html_doc
        assert "delta-good" in html_doc  # M4 uses less VRAM (-16.2%)
        assert "http://" not in html_doc and "https://" not in html_doc  # 100% self-contained

    print("benchmark.py self-test: ok (schema, deltas, remote ssh, markdown, self-contained html)")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="shift-engine cross-device benchmark suite & comparison tool")
    parser.add_argument("--self-test", action="store_true", help="Run built-in unit tests and exit")
    sub = parser.add_subparsers(dest="command")

    p_run = sub.add_parser("run", help="Run a deterministic benchmark suite locally or over SSH")
    p_run.add_argument("--bin", required=True, help="Path to carview executable (local or remote)")
    p_run.add_argument("--play", help="Play folder (containing play.txt, or out/ directory)")
    p_run.add_argument("--bake", help="Explicit car bake directory")
    p_run.add_argument("--world", help="Explicit Colorado track directory")
    p_run.add_argument("--game", help="Explicit game disc root")
    p_run.add_argument("--cache", help="Explicit world .index cache file")
    p_run.add_argument("--sound", help="Explicit baked sound directory")
    p_run.add_argument("--car", help="Car media name (optional)")
    p_run.add_argument("--suite", choices=list(SUITE_DEFAULTS.keys()), default="quick", help="Benchmark suite preset")
    p_run.add_argument("--scenarios", help="Comma-separated scenario override")
    p_run.add_argument("--presets", help="Comma-separated graphics presets (e.g. low-power,low,high)")
    p_run.add_argument("--resolutions", help="Comma-separated WxH list (e.g. 1280x720,1920x1080)")
    p_run.add_argument("--seconds", type=float, help="Override sim seconds per scenario")
    p_run.add_argument("--warmup", type=float, help="Override warmup seconds excluded from aggregates")
    p_run.add_argument("--no-full-fx", action="store_true", help="Disable --bench-full-fx")
    p_run.add_argument("--no-audio", action="store_true", help="Disable --bench-audio")
    p_run.add_argument("--no-csv", action="store_true", help="Skip writing per-frame CSV files")
    p_run.add_argument("--device-label", default="", help='Friendly device label (e.g. "Desktop RTX")')
    p_run.add_argument("--out", default="out/benchmarks", help="Output root directory")
    p_run.add_argument("--remote", help="Run remotely over SSH on <user@host> or SSH alias")
    p_run.add_argument("--remote-out", help="Remote temporary output directory for JSON/CSV")
    p_run.add_argument("--ssh-opts", default="", help="Extra flags passed to ssh/scp")

    p_cmp = sub.add_parser("compare", help="Compare benchmark JSONs/suites into Markdown and HTML")
    p_cmp.add_argument("inputs", nargs="+", help="Benchmark run JSONs, suite.json bundles, or directories")
    p_cmp.add_argument("--out", help="Output Markdown path (prints to stdout if omitted)")
    p_cmp.add_argument("--html", help="Output self-contained dark-mode HTML dashboard path")
    p_cmp.add_argument("--title", help="Custom report title")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.self_test:
        return run_self_test()
    if args.command == "run":
        return cmd_run(args)
    if args.command == "compare":
        return cmd_compare(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
