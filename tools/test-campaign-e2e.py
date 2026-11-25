#!/usr/bin/env python3
"""Automated Campaign E2E Visual + Telemetry Verification Harness for shift-engine.

Runs carview.exe headlessly through the first-time career's driven stages
(`--campaign-stage ftc:<stage>`): the opening (the AI drives the Viper under
Opening_Cutscene, then the hand-over) and the race to the festival in the
Corrado (and optionally the heats, the drive to Race Central), using an
isolated temporary save directory (NEVER touching
%LOCALAPPDATA%\\PinyonShift), captures PNG frames at key timestamps
(t=0.0s, 1.0s, 3.0s, 6.0s, 10.0s), parses per-frame JSON telemetry, and
evaluates hard automated gates:
  a) Ground clearance: car is not buried underground or over void
     (y - road_y in [-0.5m, +2.5m] across all frames).
  b) Forward driving: speed_kmh >= 60 km/h and distance >= 80 m within 8s.
  c) World streaming: chunks_loaded > 0 from frame 0 across all frames.
  d) Image sanity: captured PNGs are not flat brown/black, have sky & road
     variance/contrast, and show visual motion across the drive.
  e) Frame-time hitch gate: reports p50, p95, p99, max_ms, hitches > 33ms.

Zero external dependencies (Python 3 standard library only).

Usage:
  python tools/test-campaign-e2e.py
  python tools/test-campaign-e2e.py --bin build-campaign-e2e/apps/carview/carview.exe --out-dir out/campaign-e2e
  python tools/test-campaign-e2e.py --stages ftc:opening_cutscene,ftc:race_to_festival
  python tools/test-campaign-e2e.py --self-test
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# Pure-Python PNG reader (8-bit RGB/RGBA, all 5 PNG filter modes)
# ---------------------------------------------------------------------------

def _paeth_predictor(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png_rgb(path: Path) -> tuple[int, int, bytes]:
    """Decode an 8-bit RGB or RGBA PNG file into (width, height, rgb_bytes)."""
    raw = path.read_bytes()
    if len(raw) < 8 or raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a valid PNG file: {path}")

    pos = 8
    width = height = bit_depth = color_type = 0
    idat_chunks: list[bytes] = []
    while pos + 8 <= len(raw):
        length, ctype = struct.unpack(">I4s", raw[pos : pos + 8])
        pos += 8
        data = raw[pos : pos + length]
        pos += length + 4  # skip CRC
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, comp, flt, interlace = struct.unpack(">IIBBBBB", data)
            if bit_depth != 8 or color_type not in (2, 6) or comp != 0 or flt != 0 or interlace != 0:
                raise ValueError(
                    f"unsupported PNG format in {path}: depth={bit_depth} color={color_type} interlace={interlace}"
                )
        elif ctype == b"IDAT":
            idat_chunks.append(data)
        elif ctype == b"IEND":
            break

    if width <= 0 or height <= 0 or not idat_chunks:
        raise ValueError(f"incomplete PNG data in {path}")

    bpp = 3 if color_type == 2 else 4
    stride = width * bpp
    decompressed = zlib.decompress(b"".join(idat_chunks))
    expected = height * (1 + stride)
    if len(decompressed) < expected:
        raise ValueError(f"truncated PNG scanlines in {path}: {len(decompressed)} < {expected}")

    prev = bytearray(stride)
    rgb = bytearray(width * height * 3)
    in_off = 0
    out_off = 0
    for _ in range(height):
        ftype = decompressed[in_off]
        in_off += 1
        row = bytearray(decompressed[in_off : in_off + stride])
        in_off += stride

        if ftype == 1:  # Sub
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif ftype == 2:  # Up
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif ftype == 3:  # Average
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                up = prev[i]
                up_left = prev[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + _paeth_predictor(left, up, up_left)) & 0xFF
        elif ftype != 0:
            raise ValueError(f"unsupported PNG filter type {ftype} in {path}")

        prev = row
        if bpp == 3:
            rgb[out_off : out_off + width * 3] = row
            out_off += width * 3
        else:
            for x in range(0, stride, 4):
                rgb[out_off] = row[x]
                rgb[out_off + 1] = row[x + 1]
                rgb[out_off + 2] = row[x + 2]
                out_off += 3

    return width, height, bytes(rgb)


def write_test_png_rgb(path: Path, width: int, height: int, rgb: bytes) -> None:
    """Write a minimal uncompressed-filter RGB PNG (used by --self-test)."""
    def chunk(ctype: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(ctype + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", crc)

    raw_rows = bytearray()
    stride = width * 3
    for y in range(height):
        raw_rows.append(0)
        raw_rows.extend(rgb[y * stride : (y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw_rows)))
        + chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


# ---------------------------------------------------------------------------
# Image analysis & gate evaluation
# ---------------------------------------------------------------------------

def _region_stats(
    rgb: bytes,
    width: int,
    height: int,
    y0: int,
    y1: int,
    x0: int = 0,
    x1: int | None = None,
    step: int = 2,
) -> dict[str, float]:
    if x1 is None:
        x1 = width
    n = 0
    sum_r = sum_g = sum_b = 0.0
    sum_luma = sum_luma2 = 0.0
    for y in range(max(0, y0), min(height, y1), step):
        row_off = y * width * 3
        for x in range(max(0, x0), min(width, x1), step):
            idx = row_off + x * 3
            r = rgb[idx]
            g = rgb[idx + 1]
            b = rgb[idx + 2]
            luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
            sum_r += r
            sum_g += g
            sum_b += b
            sum_luma += luma
            sum_luma2 += luma * luma
            n += 1
    if n == 0:
        return {"mean_r": 0.0, "mean_g": 0.0, "mean_b": 0.0, "mean_luma": 0.0, "luma_std": 0.0}
    mr = sum_r / n
    mg = sum_g / n
    mb = sum_b / n
    ml = sum_luma / n
    var = max(0.0, (sum_luma2 / n) - ml * ml)
    return {
        "mean_r": mr,
        "mean_g": mg,
        "mean_b": mb,
        "mean_luma": ml,
        "luma_std": math.sqrt(var),
    }


def analyze_png(path: Path) -> dict[str, Any]:
    """Compute visual sanity metrics for a captured frame PNG."""
    width, height, rgb = decode_png_rgb(path)
    step = 2 if width >= 640 else 1
    whole = _region_stats(rgb, width, height, 0, height, step=step)
    sky = _region_stats(rgb, width, height, 0, int(height * 0.30), step=step)
    road = _region_stats(rgb, width, height, int(height * 0.55), height, step=step)
    # Left & right ground strips flanking the chase-camera car (below GPS pill, above minimap/tach)
    # to catch underworld void even when the car and HUD occupy the centre/edges.
    flank_l = _region_stats(
        rgb, width, height, int(height * 0.62), int(height * 0.76), int(width * 0.06), int(width * 0.22), step=step
    )
    flank_r = _region_stats(
        rgb, width, height, int(height * 0.62), int(height * 0.76), int(width * 0.78), int(width * 0.94), step=step
    )
    flank_std = min(flank_l["luma_std"], flank_r["luma_std"])

    # Measure flat-color dominance (fraction of pixels within L1 distance <= 24 of overall mean RGB)
    # and flat-brown/dark-dirt dominance (catches underground terrain-clipping frames).
    mr, mg, mb = whole["mean_r"], whole["mean_g"], whole["mean_b"]
    near_mean = 0
    near_dark = 0
    total = 0
    for y in range(0, height, step):
        row_off = y * width * 3
        for x in range(0, width, step):
            idx = row_off + x * 3
            r, g, b = rgb[idx], rgb[idx + 1], rgb[idx + 2]
            if abs(r - mr) + abs(g - mg) + abs(b - mb) <= 24.0:
                near_mean += 1
            if r < 18 and g < 18 and b < 18:
                near_dark += 1
            total += 1

    flat_fraction = near_mean / max(1, total)
    black_fraction = near_dark / max(1, total)
    sky_road_contrast = math.sqrt(
        (sky["mean_r"] - road["mean_r"]) ** 2
        + (sky["mean_g"] - road["mean_g"]) ** 2
        + (sky["mean_b"] - road["mean_b"]) ** 2
    )

    reasons: list[str] = []
    if whole["mean_luma"] < 15.0 or black_fraction > 0.85:
        reasons.append(f"nearly_black (mean_luma={whole['mean_luma']:.1f}, black_frac={black_fraction:.2f})")
    if whole["mean_luma"] > 245.0:
        reasons.append(f"washed_white (mean_luma={whole['mean_luma']:.1f})")
    if whole["luma_std"] < 14.0 or flat_fraction > 0.85:
        reasons.append(f"flat_image (luma_std={whole['luma_std']:.1f}, flat_frac={flat_fraction:.2f})")
    if sky["luma_std"] < 4.0 and sky_road_contrast < 15.0:
        reasons.append(
            f"no_sky_contrast (sky_std={sky['luma_std']:.1f}, sky_road_contrast={sky_road_contrast:.1f})"
        )
    if road["luma_std"] < 8.0:
        reasons.append(f"flat_road_region (road_std={road['luma_std']:.1f})")
    if flank_std < 2.0:
        reasons.append(f"void_ground_flanks (flank_std={flank_std:.2f})")

    return {
        "path": str(path),
        "width": width,
        "height": height,
        "mean_luma": round(whole["mean_luma"], 2),
        "luma_std": round(whole["luma_std"], 2),
        "flat_fraction": round(flat_fraction, 3),
        "black_fraction": round(black_fraction, 3),
        "sky_mean_rgb": [round(sky["mean_r"], 1), round(sky["mean_g"], 1), round(sky["mean_b"], 1)],
        "sky_luma_std": round(sky["luma_std"], 2),
        "road_mean_rgb": [round(road["mean_r"], 1), round(road["mean_g"], 1), round(road["mean_b"], 1)],
        "road_luma_std": round(road["luma_std"], 2),
        "flank_ground_std": round(flank_std, 2),
        "sky_road_contrast": round(sky_road_contrast, 2),
        "pass": len(reasons) == 0,
        "reasons": reasons,
    }


def frame_mean_abs_diff(path_a: Path, path_b: Path) -> float:
    """Return mean per-channel absolute pixel difference [0..255] between two PNGs."""
    wa, ha, rgba = decode_png_rgb(path_a)
    wb, hb, rgbb = decode_png_rgb(path_b)
    if wa != wb or ha != hb:
        return 255.0
    step = 4 if wa >= 640 else 1
    total_diff = 0
    samples = 0
    for y in range(0, ha, step):
        row_off = y * wa * 3
        for x in range(0, wa, step):
            idx = row_off + x * 3
            total_diff += abs(rgba[idx] - rgbb[idx])
            total_diff += abs(rgba[idx + 1] - rgbb[idx + 1])
            total_diff += abs(rgba[idx + 2] - rgbb[idx + 2])
            samples += 3
    return total_diff / max(1, samples)


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    idx = (len(s) - 1) * (pct / 100.0)
    lo = int(math.floor(idx))
    hi = min(len(s) - 1, lo + 1)
    frac = idx - lo
    return s[lo] * (1.0 - frac) + s[hi] * frac


def evaluate_telemetry(
    telemetry: dict[str, Any],
    *,
    hitch_threshold_ms: float = 33.0,
    max_hitch_count: int = 0,
    min_speed_kmh: float = 60.0,
    min_distance_m: float = 80.0,
    drive_window_s: float = 8.0,
) -> dict[str, Any]:
    """Evaluate all 5 automated campaign E2E gates from telemetry + captured PNGs."""
    frames: list[dict[str, Any]] = telemetry.get("frames", [])
    if not frames:
        return {
            "stage": telemetry.get("stage", "unknown"),
            "all_passed": False,
            "error": "no telemetry frames recorded",
            "gates": {},
        }

    # Gate A: Car NOT buried underground (y - road_y in [-0.5m, +2.5m] across all frames)
    missing_road_y = 0
    min_clearance = float("inf")
    max_clearance = float("-inf")
    first_clearance_fail: dict[str, Any] | None = None
    for fr in frames:
        ry = fr.get("road_y")
        if ry is None:
            missing_road_y += 1
            if first_clearance_fail is None:
                first_clearance_fail = {
                    "frame": fr["frame"],
                    "t": fr["t"],
                    "x": fr["x"],
                    "y": fr["y"],
                    "z": fr["z"],
                    "road_y": None,
                    "reason": "no_road_surface_detected",
                }
            continue
        dy = float(fr["y"]) - float(ry)
        min_clearance = min(min_clearance, dy)
        max_clearance = max(max_clearance, dy)
        if (dy < -0.5 or dy > 2.5) and first_clearance_fail is None:
            first_clearance_fail = {
                "frame": fr["frame"],
                "t": fr["t"],
                "x": fr["x"],
                "y": fr["y"],
                "z": fr["z"],
                "road_y": ry,
                "y_minus_road_y": round(dy, 3),
                "reason": "buried_underground" if dy < -0.5 else "floating_above_road",
            }
    gate_a_pass = (missing_road_y == 0) and (min_clearance >= -0.5) and (max_clearance <= 2.5)
    gate_a = {
        "pass": gate_a_pass,
        "min_y_minus_road_y_m": None if min_clearance == float("inf") else round(min_clearance, 3),
        "max_y_minus_road_y_m": None if max_clearance == float("-inf") else round(max_clearance, 3),
        "missing_road_y_frames": missing_road_y,
        "first_failure": first_clearance_fail,
    }

    # Gate B: Car actually drives forward (speed_kmh >= 60 and distance >= 80m within 8s)
    max_speed = 0.0
    distance_m = 0.0
    prev_x = float(frames[0]["x"])
    prev_z = float(frames[0]["z"])
    for fr in frames:
        t = float(fr["t"])
        if t > drive_window_s + 1e-6:
            break
        spd = float(fr.get("speed_kmh", 0.0))
        max_speed = max(max_speed, spd)
        cx, cz = float(fr["x"]), float(fr["z"])
        step_d = math.hypot(cx - prev_x, cz - prev_z)
        # Guard against single-frame teleport jumps inflating distance
        if step_d < 25.0:
            distance_m += step_d
        prev_x, prev_z = cx, cz
    displacement_m = math.hypot(
        float(frames[-1]["x"]) - float(frames[0]["x"]),
        float(frames[-1]["z"]) - float(frames[0]["z"]),
    )
    gate_b_pass = (max_speed >= min_speed_kmh) and (distance_m >= min_distance_m)
    gate_b = {
        "pass": gate_b_pass,
        "max_speed_kmh_within_window": round(max_speed, 2),
        "distance_m_within_window": round(distance_m, 2),
        "net_displacement_m": round(displacement_m, 2),
        "window_s": drive_window_s,
        "required_speed_kmh": min_speed_kmh,
        "required_distance_m": min_distance_m,
    }

    # Gate C: World chunks loaded around spawn (chunks_loaded > 0 from frame 0)
    frame0_chunks = int(frames[0].get("chunks_loaded", 0))
    min_chunks = min(int(fr.get("chunks_loaded", 0)) for fr in frames)
    max_chunks = max(int(fr.get("chunks_loaded", 0)) for fr in frames)
    frame0_dist = frames[0].get("streamer_dist_m")
    frame0_world_models = frames[0].get("streamer_world_models")
    gate_c_pass = frame0_chunks > 0 and min_chunks > 0
    gate_c = {
        "pass": gate_c_pass,
        "frame0_chunks_loaded": frame0_chunks,
        "min_chunks_loaded": min_chunks,
        "max_chunks_loaded": max_chunks,
        "frame0_streamer_dist_m": None if frame0_dist is None else round(float(frame0_dist), 2),
        "frame0_streamer_world_models": frame0_world_models,
    }

    # Gate D: Image sanity checks on all captured PNGs
    captured_paths: list[Path] = []
    for fr in frames:
        p = fr.get("captured_png")
        if p:
            captured_paths.append(Path(p))
    image_reports: list[dict[str, Any]] = []
    for p in captured_paths:
        if not p.exists():
            image_reports.append({"path": str(p), "pass": False, "reasons": ["missing_file"]})
        else:
            image_reports.append(analyze_png(p))
    motion_diff = None
    if len(captured_paths) >= 2 and captured_paths[0].exists() and captured_paths[-1].exists():
        motion_diff = round(frame_mean_abs_diff(captured_paths[0], captured_paths[-1]), 2)
    motion_ok = True if motion_diff is None else (motion_diff >= 3.0)
    gate_d_pass = len(image_reports) > 0 and all(r["pass"] for r in image_reports) and motion_ok
    gate_d = {
        "pass": gate_d_pass,
        "captured_count": len(image_reports),
        "first_to_last_mean_abs_diff": motion_diff,
        "motion_detected": motion_ok,
        "frames": image_reports,
    }

    # Gate E: Frame-time hitch gate (evaluated on steady-state frames 1..N; frame 0
    # includes synchronous offscreen PNG readback and first-frame font atlas init).
    steady_frames = frames[1:] if len(frames) > 1 else frames
    frame_times = [float(fr.get("frame_ms", 0.0)) for fr in steady_frames]
    frame0_ms = float(frames[0].get("frame_ms", 0.0)) if frames else 0.0
    p50 = percentile(frame_times, 50.0)
    p95 = percentile(frame_times, 95.0)
    p99 = percentile(frame_times, 99.0)
    max_ms = max(frame_times) if frame_times else 0.0
    hitches = [
        {"frame": fr["frame"], "t": round(float(fr["t"]), 3), "frame_ms": round(float(fr["frame_ms"]), 2)}
        for fr in steady_frames
        if float(fr.get("frame_ms", 0.0)) > hitch_threshold_ms
    ]
    gate_e_pass = (len(hitches) <= max_hitch_count) and (p95 <= hitch_threshold_ms)
    gate_e = {
        "pass": gate_e_pass,
        "frame_count": len(frame_times),
        "frame0_ms": round(frame0_ms, 2),
        "p50_ms": round(p50, 2),
        "p95_ms": round(p95, 2),
        "p99_ms": round(p99, 2),
        "max_ms": round(max_ms, 2),
        "hitch_threshold_ms": hitch_threshold_ms,
        "hitches_gt_threshold": len(hitches),
        "max_allowed_hitches": max_hitch_count,
        "top_hitches": sorted(hitches, key=lambda h: h["frame_ms"], reverse=True)[:10],
    }

    all_passed = gate_a_pass and gate_b_pass and gate_c_pass and gate_d_pass and gate_e_pass
    return {
        "stage": telemetry.get("stage", "unknown"),
        "car": telemetry.get("car", ""),
        "all_passed": all_passed,
        "gates": {
            "a_ground_clearance": gate_a,
            "b_forward_drive": gate_b,
            "c_world_chunks": gate_c,
            "d_image_sanity": gate_d,
            "e_frame_time_hitches": gate_e,
        },
    }


# ---------------------------------------------------------------------------
# Path discovery & runner
# ---------------------------------------------------------------------------

def _first_existing(candidates: list[Path]) -> Path | None:
    for c in candidates:
        if c and c.exists():
            return c
    return None


def resolve_paths(args: argparse.Namespace) -> dict[str, Path | None]:
    repo_root = Path(__file__).resolve().parent.parent
    sibling_h1 = repo_root.parent / "horizon1-recomp"

    bin_candidates: list[Path] = []
    if args.bin:
        bin_candidates.append(Path(args.bin))
    else:
        for bdir in ("build", "build-release", "build-dev", "build-play"):
            bin_candidates.append(repo_root / bdir / "apps" / "carview" / "carview.exe")
            bin_candidates.append(repo_root / bdir / "apps" / "carview" / "carview")

    game_candidates: list[Path] = []
    if args.game:
        game_candidates.append(Path(args.game))
    else:
        if os.environ.get("SHIFT_GAME_ROOT"):
            game_candidates.append(Path(os.environ["SHIFT_GAME_ROOT"]))
        game_candidates.append(sibling_h1 / ".local" / "game" / "base")

    game_path = _first_existing(game_candidates)

    world_candidates: list[Path] = []
    if args.world:
        world_candidates.append(Path(args.world))
    else:
        if game_path:
            world_candidates.append(game_path / "Media" / "Tracks" / "Colorado")
            world_candidates.append(game_path / "media" / "tracks" / "colorado")

    bake_candidates: list[Path] = []
    if args.bake:
        bake_candidates.append(Path(args.bake))
    else:
        bake_candidates.append(repo_root / "out" / "bake")

    cache_candidates: list[Path] = []
    if args.cache:
        cache_candidates.append(Path(args.cache))
    else:
        cache_candidates.append(repo_root / "out" / "world" / "colorado.index")

    sound_candidates: list[Path] = []
    if args.sound:
        sound_candidates.append(Path(args.sound))
    else:
        sound_candidates.append(repo_root / "out" / "sound")

    return {
        "bin": _first_existing(bin_candidates),
        "play": Path(args.play) if args.play else None,
        "bake": _first_existing(bake_candidates),
        "world": _first_existing(world_candidates),
        "game": game_path,
        "cache": _first_existing(cache_candidates),
        "sound": _first_existing(sound_candidates),
    }


# The first-time career's stages and the car each starts in (the career swaps
# to it anyway; starting in it saves a reload). The old onboarding names still
# map into the career (campaign_director.cpp, ftc_stage_for_legacy).
DEFAULT_STAGES = "ftc:opening_cutscene,ftc:race_to_festival"
VIPER = "vip_viper_13"
CORRADO = "vw_corrado_95"
STAGE_CARS = {
    "ftc:opening_cutscene": VIPER,
    "ftc:initial_drive": VIPER,
    "viper_drive": VIPER,
    "ftc:race_to_festival": CORRADO,
    "ftc:heats": CORRADO,
    "ftc:showcase": CORRADO,
    "ftc:travel_to_race_central": CORRADO,
    "qualify": CORRADO,
    "corrado_qualify": CORRADO,
}
# Every car the opening drives or shows; carview names a missing one and the
# career cannot swap to it (bake them: tools/setup.py bakes all by default).
OPENING_CARS = ("VIP_Viper_13", "VW_Corrado_95", "FER_599XX_10", "SUB_WRXSTi_11", "MIT_LancerEvoXGSR_08")


def missing_opening_cars(bake: Path | None) -> list[str]:
    if not bake:
        return []
    cars = bake / "cars" if (bake / "cars").is_dir() else bake
    have = {p.name.lower() for p in cars.glob("*.shiftbake")}
    return [c for c in OPENING_CARS if f"{c.lower()}.shiftbake" not in have]


def run_stage(
    *,
    stage: str,
    paths: dict[str, Path | None],
    out_dir: Path,
    seconds: float,
    capture_times: str,
    size: str,
    timeout_s: int,
    hitch_threshold_ms: float,
    max_hitch_count: int,
    car_override: str | None = None,
) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    telemetry_path = out_dir / f"{stage}_telemetry.json"
    out_png_prefix = out_dir / f"{stage}.png"

    with tempfile.TemporaryDirectory(prefix=f"shift-campaign-e2e-{stage}-") as tmp_dir:
        tmp_path = Path(tmp_dir)
        save_file = tmp_path / "save.txt"

        cmd: list[str] = [str(paths["bin"])]
        if paths["play"]:
            cmd += ["--play", str(paths["play"])]
        else:
            car_media = car_override or STAGE_CARS.get(stage, VIPER)
            cmd += [
                "--bake", str(paths["bake"]),
                "--car", car_media,
                "--world", str(paths["world"]),
                "--game", str(paths["game"]),
            ]
            if paths["cache"]:
                cmd += ["--cache", str(paths["cache"])]
            if paths["sound"]:
                cmd += ["--sound", str(paths["sound"])]

        cmd += [
            "--headless",
            "--no-settings",
            "--no-sound",
            "--save", str(save_file),
            "--campaign-stage", stage,
            "--run", "cruise",
            "--speed", "90",
            "--seconds", f"{seconds:.2f}",
            "--capture-times", capture_times,
            "--size", size,
            "--out", str(out_png_prefix),
            "--telemetry-json", str(telemetry_path),
        ]

        # Isolate state env vars so %LOCALAPPDATA%\PinyonShift is NEVER touched.
        env = os.environ.copy()
        env["SHIFT_STATE_ROOT"] = str(tmp_path / "state")
        env["SHIFT_USER_DIR"] = str(tmp_path / "user")

        print(f"\n=== Running campaign stage: {stage} ===")
        print("CMD:", " ".join(cmd))
        proc = subprocess.run(
            cmd,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
        log_path = out_dir / f"{stage}_carview.log"
        log_path.write_text(
            f"EXIT_CODE={proc.returncode}\n\n--- STDOUT ---\n{proc.stdout}\n\n--- STDERR ---\n{proc.stderr}\n",
            encoding="utf-8",
        )
        if proc.returncode != 0:
            print(f"[FAIL] carview exited with code {proc.returncode} (log: {log_path})")
            if proc.stderr.strip():
                print(proc.stderr.strip()[-1200:])
            return {
                "stage": stage,
                "all_passed": False,
                "exit_code": proc.returncode,
                "log_path": str(log_path),
                "error": f"carview exited with code {proc.returncode}",
                "gates": {},
            }

        if not telemetry_path.exists():
            return {
                "stage": stage,
                "all_passed": False,
                "exit_code": proc.returncode,
                "log_path": str(log_path),
                "error": f"telemetry JSON not created at {telemetry_path}",
                "gates": {},
            }

        telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
        result = evaluate_telemetry(
            telemetry,
            hitch_threshold_ms=hitch_threshold_ms,
            max_hitch_count=max_hitch_count,
        )
        result["telemetry_path"] = str(telemetry_path)
        result["log_path"] = str(log_path)
        return result


def print_stage_summary(res: dict[str, Any]) -> None:
    stage = res.get("stage", "unknown")
    car = res.get("car", "")
    status = "PASS" if res.get("all_passed") else "FAIL"
    print(f"\n--- Campaign E2E Summary [{stage}] (car={car}) => {status} ---")
    if res.get("error"):
        print(f"  ERROR: {res['error']}")
        return
    gates = res["gates"]
    ga = gates["a_ground_clearance"]
    print(
        f"  [{'PASS' if ga['pass'] else 'FAIL'}] Gate A (Ground Clearance [-0.5, +2.5]m): "
        f"min={ga['min_y_minus_road_y_m']}m max={ga['max_y_minus_road_y_m']}m "
        f"missing_road_y={ga['missing_road_y_frames']}"
    )
    if ga.get("first_failure"):
        print(f"         first_failure={ga['first_failure']}")

    gb = gates["b_forward_drive"]
    print(
        f"  [{'PASS' if gb['pass'] else 'FAIL'}] Gate B (Forward Drive >=60 km/h & >=80m in {gb['window_s']}s): "
        f"max_speed={gb['max_speed_kmh_within_window']} km/h, "
        f"dist={gb['distance_m_within_window']}m (net={gb['net_displacement_m']}m)"
    )

    gc = gates["c_world_chunks"]
    print(
        f"  [{'PASS' if gc['pass'] else 'FAIL'}] Gate C (World Chunks Loaded from frame 0): "
        f"frame0={gc['frame0_chunks_loaded']}, min={gc['min_chunks_loaded']}, max={gc['max_chunks_loaded']} "
        f"(streamer_dist={gc['frame0_streamer_dist_m']}m, world_models={gc['frame0_streamer_world_models']})"
    )

    gd = gates["d_image_sanity"]
    print(
        f"  [{'PASS' if gd['pass'] else 'FAIL'}] Gate D (Image Sanity & Motion): "
        f"captured={gd['captured_count']}, motion_diff={gd['first_to_last_mean_abs_diff']}"
    )
    for fr in gd["frames"]:
        flag = "OK" if fr["pass"] else f"BAD ({', '.join(fr['reasons'])})"
        print(
            f"         {Path(fr['path']).name}: {flag} "
            f"[luma={fr.get('mean_luma')} std={fr.get('luma_std')} "
            f"sky_rgb={fr.get('sky_mean_rgb')} road_rgb={fr.get('road_mean_rgb')}]"
        )

    ge = gates["e_frame_time_hitches"]
    print(
        f"  [{'PASS' if ge['pass'] else 'FAIL'}] Gate E (Frame-Time Hitches > {ge['hitch_threshold_ms']}ms): "
        f"p50={ge['p50_ms']}ms p95={ge['p95_ms']}ms p99={ge['p99_ms']}ms "
        f"max={ge['max_ms']}ms hitches={ge['hitches_gt_threshold']} (frames={ge['frame_count']})"
    )


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def run_self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="shift-campaign-e2e-selftest-") as td:
        tmp = Path(td)
        w, h = 64, 36
        # Good driving frame: blue sky on top, textured gray asphalt + car on bottom
        good0 = bytearray(w * h * 3)
        good1 = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                idx = (y * w + x) * 3
                if y < h // 3:
                    # Blue sky with subtle gradient
                    good0[idx : idx + 3] = bytes([70 + y * 2, 130 + y * 2, 210])
                    good1[idx : idx + 3] = bytes([75 + y * 2, 135 + y * 2, 215])
                else:
                    # Textured road
                    stripe = 190 if ((x + y) % 8 < 2) else 65
                    good0[idx : idx + 3] = bytes([stripe, stripe, stripe])
                    stripe1 = 190 if ((x + y + 3) % 8 < 2) else 75
                    good1[idx : idx + 3] = bytes([stripe1, stripe1, stripe1])
        # Bad underground frame: flat brown everywhere
        bad_brown = bytes([92, 68, 48] * (w * h))

        p_good0 = tmp / "good0.png"
        p_good1 = tmp / "good1.png"
        p_brown = tmp / "brown.png"
        write_test_png_rgb(p_good0, w, h, bytes(good0))
        write_test_png_rgb(p_good1, w, h, bytes(good1))
        write_test_png_rgb(p_brown, w, h, bad_brown)

        good_tel = {
            "stage": "viper_drive",
            "car": "DOD_ViperGTS_13",
            "frames": [
                {
                    "frame": i,
                    "t": i * 0.5,
                    "x": 100.0 + i * 10.0,
                    "y": 50.5,
                    "z": 200.0,
                    "road_y": 50.0,
                    "speed_kmh": min(140.0, i * 15.0),
                    "chunks_loaded": 24,
                    "streamer_world_models": 24,
                    "streamer_dist_m": 12.0,
                    "frame_ms": 8.5,
                    "captured_png": str(p_good0) if i == 0 else (str(p_good1) if i == 16 else None),
                }
                for i in range(17)
            ],
        }
        res_good = evaluate_telemetry(good_tel)
        assert res_good["all_passed"], f"expected good telemetry to pass: {res_good}"

        bad_tel = {
            "stage": "viper_drive",
            "car": "DOD_ViperGTS_13",
            "frames": [
                {
                    "frame": 0,
                    "t": 0.0,
                    "x": 1620.0,
                    "y": 128.0,
                    "z": 840.0,
                    "road_y": 135.0,  # buried 7m underground!
                    "speed_kmh": 0.0,
                    "chunks_loaded": 0,
                    "streamer_world_models": 20,
                    "streamer_dist_m": 1800.0,
                    "frame_ms": 145.0,
                    "captured_png": str(p_brown),
                },
                {
                    "frame": 1,
                    "t": 8.0,
                    "x": 1620.0,
                    "y": 128.0,
                    "z": 840.0,
                    "road_y": 135.0,
                    "speed_kmh": 0.0,
                    "chunks_loaded": 0,
                    "streamer_world_models": 20,
                    "streamer_dist_m": 1800.0,
                    "frame_ms": 145.0,
                    "captured_png": str(p_brown),
                },
            ],
        }
        res_bad = evaluate_telemetry(bad_tel)
        assert not res_bad["all_passed"], "expected bad telemetry to fail"
        for k, g in res_bad["gates"].items():
            assert not g["pass"], f"expected gate {k} to fail on bad telemetry"

    print("self-test: PASS (PNG decoder + all 5 E2E gates verified on positive & negative cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Automated Campaign E2E Visual + Telemetry Verification Harness")
    parser.add_argument("--bin", help="Path to carview.exe")
    parser.add_argument("--play", help="Path to play directory (alternative to --bake/--world/--game)")
    parser.add_argument("--bake", help="Path to baked cars directory")
    parser.add_argument("--car", help="Initial baked car media name (default: vip_viper_13 / vw_corrado_95)")
    parser.add_argument("--world", help="Path to Colorado track directory")
    parser.add_argument("--game", help="Path to extracted FH1 game root")
    parser.add_argument("--cache", help="Path to colorado.index cache file")
    parser.add_argument("--sound", help="Path to sound directory")
    parser.add_argument("--out-dir", default="out/campaign-e2e", help="Output directory for PNGs, telemetry & report")
    parser.add_argument(
        "--stages",
        default=DEFAULT_STAGES,
        help=f"Comma-separated campaign stages to test (default: {DEFAULT_STAGES}; any ftc:<stage>)",
    )
    parser.add_argument("--seconds", type=float, default=10.0, help="Drive duration in seconds (default: 10.0)")
    parser.add_argument("--capture-times", default="0,1,3,6,10", help="Comma-separated PNG capture timestamps in seconds")
    parser.add_argument("--size", default="1280x720", help="Render resolution WxH (default: 1280x720)")
    parser.add_argument("--timeout", type=int, default=180, help="Subprocess timeout per stage in seconds")
    parser.add_argument("--hitch-ms", type=float, default=33.0, help="Frame-time hitch threshold in ms (default: 33.0)")
    parser.add_argument("--max-hitches", type=int, default=0, help="Maximum allowed frames > --hitch-ms (default: 0)")
    parser.add_argument("--self-test", action="store_true", help="Run internal unit tests and exit")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    paths = resolve_paths(args)
    if not paths["bin"]:
        print("ERROR: carview executable not found. Pass --bin <path>.", file=sys.stderr)
        return 2
    if not paths["play"] and (not paths["bake"] or not paths["world"] or not paths["game"]):
        print(
            "ERROR: missing local asset paths. Pass --play <dir> or --bake/--world/--game.\n"
            f"  resolved: bake={paths['bake']} world={paths['world']} game={paths['game']}",
            file=sys.stderr,
        )
        return 2

    missing = missing_opening_cars(paths["play"] / "bake" if paths["play"] else paths["bake"])
    if missing:
        print(f"ERROR: the opening's cars are not baked: {', '.join(missing)} "
              f"(bake them: tools/setup.py <game> --cars {','.join(missing)})", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    stages = [s.strip() for s in args.stages.split(",") if s.strip()]

    results: list[dict[str, Any]] = []
    for stage in stages:
        res = run_stage(
            stage=stage,
            paths=paths,
            out_dir=out_dir,
            seconds=args.seconds,
            capture_times=args.capture_times,
            size=args.size,
            timeout_s=args.timeout,
            hitch_threshold_ms=args.hitch_ms,
            max_hitch_count=args.max_hitches,
            car_override=args.car,
        )
        print_stage_summary(res)
        results.append(res)

    report = {
        "all_passed": all(r.get("all_passed", False) for r in results),
        "out_dir": str(out_dir),
        "carview_bin": str(paths["bin"]),
        "stages": results,
    }
    report_path = out_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"\nCampaign E2E report written -> {report_path}")
    print(f"OVERALL RESULT: {'PASS' if report['all_passed'] else 'FAIL'}")
    return 0 if report["all_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
