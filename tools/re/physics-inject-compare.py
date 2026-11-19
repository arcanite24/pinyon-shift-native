#!/usr/bin/env python3
"""Compare the windows of a physics oracle trace (RF-5, RI-0.5).

`--pinyon_shift_physics_oracle=true` (src/guest_native/car_physics.cpp)
writes `physics-oracle.bin`: at every step entry of the player's car body,
for a number of steps after the snapshot (window 0) and after each
injection of that snapshot (windows 1..n), a 72-byte header and the body's
0x5040 raw guest bytes. An injection window that repeats window 0 tick for
tick within the per-field tolerances is an A/A pass: the captured state is
enough to step the body.

    python -I tools/re/physics-inject-compare.py <run>/physics-oracle.bin
    python -I tools/re/physics-inject-compare.py trace.bin --seconds 2 --json out.json
    python -I tools/re/physics-inject-compare.py trace.bin --discover

`summary` (the default) prints, per injection window, each named field's
largest error over the horizon and the first tick it passes its tolerance,
and the same for the negative control (the state each injection replaced,
held against window 0's tick 0). `--discover` lists the body words that
change while driving, the substep counts and the dt, to find unnamed state.
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path

HEADER = struct.Struct("<IHHIIQddIIIIIIQ")
MAGIC = 0x544F5350
DYN = 0xAD0          # ICarDynamics inside the body (CCarDynamics)
WHEELS = DYN + 3936  # four 1024-byte wheel blocks
STRIDE = 1024


def fields() -> list[tuple[str, int, str, float]]:
    """(name, body offset, type, tolerance). With a fixed dt the oracle's own
    A/A error is zero over 2 s (RI-0.5, 2026-11-19), so these are the bounds
    a native step has to hold, not the oracle's noise: 2 cm, 5 cm/s, 0.05
    rad/s, 2 rad/s engine, 2 mm suspension, 0.5 rad/s wheel speed, 0.05 slip,
    2 (200 N) of wheel load, and exact gear, surface and contact. The body
    works in 100 kg and 100 N (mass 15.6 at body+0x160 for 1,560 kg)."""
    out = [("position_" + a, 0x4EA0 + 4 * i, "f", 0.02) for i, a in enumerate("xyz")]
    out += [("velocity_" + a, 0xC0 + 4 * i, "f", 0.05) for i, a in enumerate("xyz")]
    out += [("angular_velocity_" + a, 0xA0 + 4 * i, "f", 0.05) for i, a in enumerate("xyz")]
    out += [
        ("engine_speed_rad_s", DYN + 324, "f", 2.0),
        ("gear", DYN + 2240, "B", 0.0),
        ("speed_m_s", DYN + 3128, "f", 0.05),
    ]
    for w in range(4):
        b = WHEELS + STRIDE * w
        out += [
            (f"w{w}.suspension", b + 0, "f", 0.002),
            (f"w{w}.wheel_speed_rad_s", b + 12, "f", 0.5),
            (f"w{w}.slip_raw", b + 492, "f", 0.05),
            (f"w{w}.slip_norm", b + 496, "f", 0.05),
            (f"w{w}.slip_angle_norm", b + 520, "f", 0.05),
            (f"w{w}.normal_load", b + 676, "f", 2.0),
            (f"w{w}.surface", b + 337, "B", 0.0),
            (f"w{w}.contact", b + 796, "B", 0.0),
        ]
    return out


# Words that differ between windows without being state: the unused w
# lanes of the velocity vector and its copy, a word at body+2760, and the
# controls at step entry, which the input side rewrites and clears between
# steps (gas reads 0 on about one step in five under a held trigger in both
# windows, on different steps, with no effect on the step).
NOT_STATE = {0xCC, 0x15EC, 0xAC8} | {DYN + 3904 + 4 * i for i in range(5)}


def read_trace(path: Path) -> list[dict]:
    data = path.read_bytes()
    records, at = [], 0
    while at + HEADER.size <= len(data):
        (magic, version, kind, window, tick, frame, dt, game_dt, body, size, step_path,
         substeps, rate0, rate1, host_ns) = HEADER.unpack_from(data, at)
        if magic != MAGIC:
            raise SystemExit(f"bad record at byte {at}")
        at += HEADER.size
        blob = data[at:at + size]
        at += size
        if len(blob) < size:
            break
        records.append(dict(version=version, kind=kind, window=window, tick=tick, frame=frame,
                            dt=dt, game_dt=game_dt, body=body, path=step_path,
                            substeps=substeps, rate=(rate0, rate1), host_ns=host_ns, bytes=blob))
    return records


def value(blob: bytes, offset: int, kind: str) -> float:
    if kind == "f":
        return struct.unpack_from(">f", blob, offset)[0]
    return float(blob[offset])


def windows(records: list[dict]) -> tuple[dict[int, list[dict]], dict[int, dict]]:
    traced: dict[int, list[dict]] = {}
    replaced: dict[int, dict] = {}
    for r in records:
        if r["kind"] == 2:
            replaced[r["window"]] = r
        else:
            traced.setdefault(r["window"], []).append(r)
    for rows in traced.values():
        rows.sort(key=lambda r: r["tick"])
    return traced, replaced


def horizon_ticks(rows: list[dict], seconds: float) -> int:
    t = 0.0
    for i, r in enumerate(rows):
        if t >= seconds - 1e-9:
            return i
        t += abs(r["dt"])
    return len(rows)


def elapsed(rows: list[dict]) -> list[float]:
    """Simulated time at each record (the steps before it)."""
    out, t = [], 0.0
    for r in rows:
        out.append(t)
        t += abs(r["dt"])
    return out


def ref_value(ref: list[dict], ref_t: list[float], t: float, offset: int, kind: str) -> float:
    """window 0's value at simulated time t: linear between the two records
    around it for floats, the earlier record's for bytes."""
    import bisect
    i = max(0, min(bisect.bisect_right(ref_t, t) - 1, len(ref) - 1))
    a = value(ref[i]["bytes"], offset, kind)
    if kind != "f" or i + 1 >= len(ref) or ref_t[i + 1] == ref_t[i]:
        return a
    b = value(ref[i + 1]["bytes"], offset, kind)
    w = (t - ref_t[i]) / (ref_t[i + 1] - ref_t[i])
    return a + (b - a) * w


def compare(ref: list[dict], other: list[dict], ticks: int, table, align: str = "time") -> dict:
    out = {}
    n = min(ticks, len(ref), len(other))
    ref_t, other_t = elapsed(ref), elapsed(other)
    if align == "time":
        # Only the records whose time window 0 covers within the horizon.
        horizon = ref_t[min(ticks, len(ref)) - 1]
        n = sum(1 for t in other_t if t <= horizon)
    for name, offset, kind, tol in table:
        worst, worst_tick, first_fail = 0.0, None, None
        for t in range(n):
            if align == "time":
                a = ref_value(ref, ref_t, other_t[t], offset, kind)
            else:
                a = value(ref[t]["bytes"], offset, kind)
            b = value(other[t]["bytes"], offset, kind)
            err = abs(a - b) if not (math.isnan(a) or math.isnan(b)) else math.inf
            if err > worst:
                worst, worst_tick = err, t
            if first_fail is None and err > tol:
                first_fail = t
        out[name] = dict(max_error=worst, at_tick=worst_tick, first_fail=first_fail, tolerance=tol)
    return out


def words_diff(a: bytes, b: bytes, skip=frozenset()) -> list[int]:
    return [o for o in range(0, min(len(a), len(b)), 4)
            if o not in skip and a[o:o + 4] != b[o:o + 4]]


def discover(ref: list[dict]) -> None:
    dts = [r["dt"] for r in ref]
    print(f"window 0: {len(ref)} steps, dt min {min(dts):.6f} max {max(dts):.6f} "
          f"mean {sum(dts) / len(dts):.6f}; game dt min {min(r['game_dt'] for r in ref):.6f} "
          f"max {max(r['game_dt'] for r in ref):.6f}")
    subs: dict[int, int] = {}
    for r in ref:
        subs[r["substeps"]] = subs.get(r["substeps"], 0) + 1
    print(f"substeps (body+0x502C) histogram {dict(sorted(subs.items()))}; rate words "
          f"{sorted({r['rate'] for r in ref})}; step path words {sorted({r['path'] for r in ref})}")
    # A record's substep word was written by the previous step: check it
    # against ceil(|dt| * rate) of that step's dt (dt clamped to 0.1 s).
    pairs = [(p, c) for p, c in zip(ref, ref[1:]) if c["substeps"]]
    hits = sum(1 for p, c in pairs
               if c["substeps"] == max(1, math.ceil(min(abs(p["dt"]), 0.1) * p["rate"][0] - 1e-6)))
    print(f"substeps == ceil(dt * rate[0]) on {hits} of {len(pairs)} steps")
    changed: dict[int, int] = {}
    for prev, cur in zip(ref, ref[1:]):
        for o in words_diff(prev["bytes"], cur["bytes"]):
            changed[o] = changed.get(o, 0) + 1
    steps = len(ref) - 1
    print(f"{len(changed)} of {len(ref[0]['bytes']) // 4} words change in {steps} steps; "
          f"{sum(1 for c in changed.values() if c == steps)} change every step")
    # Runs of changed words, with the first and last value as floats.
    runs, start, last = [], None, None
    for o in sorted(changed):
        if start is None:
            start = last = o
        elif o == last + 4:
            last = o
        else:
            runs.append((start, last))
            start = last = o
    if start is not None:
        runs.append((start, last))
    for a, b in runs:
        rel = f"dyn+{a - DYN}" if a >= DYN else f"body+{a}"
        if WHEELS <= a < WHEELS + 4 * STRIDE:
            rel = f"wheel{(a - WHEELS) // STRIDE}+{(a - WHEELS) % STRIDE}"
        first = [round(struct.unpack_from('>f', ref[0]['bytes'], o)[0], 4) for o in range(a, min(b, a + 12) + 4, 4)]
        lastv = [round(struct.unpack_from('>f', ref[-1]['bytes'], o)[0], 4) for o in range(a, min(b, a + 12) + 4, 4)]
        print(f"  {a:#06x}-{b + 4:#06x} ({rel}, {(b - a) // 4 + 1} words, min changes "
              f"{min(changed[o] for o in range(a, b + 4, 4))}): {first} -> {lastv}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--seconds", type=float, default=2.0, help="horizon in simulated seconds")
    parser.add_argument("--discover", action="store_true")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--align", choices=("time", "tick"), default="time",
                        help="match records by simulated time (default) or by step index")
    args = parser.parse_args()

    records = read_trace(args.trace)
    traced, replaced = windows(records)
    if 0 not in traced:
        print("no snapshot window")
        return 2
    ref = traced[0]
    if args.discover:
        discover(ref)
    table = fields()
    ticks = horizon_ticks(ref, args.seconds)
    if sum(abs(r["dt"]) for r in ref) < args.seconds - 1e-6:
        print(f"window 0 covers less than {args.seconds} s")
        return 2
    if not any(k for k in traced):
        print("no injection windows")
        return 2
    report = {"horizon_ticks": ticks, "horizon_s": args.seconds, "windows": {}}
    ok = True
    print(f"window 0: frame {ref[0]['frame']}, {len(ref)} steps; horizon {ticks} steps "
          f"({args.seconds} s)")
    for w in sorted(k for k in traced if k):
        rows = traced[w]
        result = compare(ref, rows, ticks, table, args.align)
        exact = [len(words_diff(ref[t]["bytes"], rows[t]["bytes"], NOT_STATE))
                 for t in range(min(ticks, len(rows)))]
        failed = [k for k, v in result.items() if v["first_fail"] is not None]
        entry = {"frame": rows[0]["frame"], "steps": len(rows), "fields": result,
                 "differing_words_by_tick": exact, "failed": failed,
                 "pass": not failed and sum(abs(r["dt"]) for r in rows) >= args.seconds - 1e-6}
        if w in replaced:
            entry["control"] = compare(ref, [replaced[w]], 1, table, "tick")
            entry["control_failed"] = [k for k, v in entry["control"].items() if v["first_fail"] is not None]
        report["windows"][w] = entry
        ok &= entry["pass"]
        print(f"\nwindow {w}: frame {rows[0]['frame']}, {len(rows)} steps, "
              f"{'PASS' if entry['pass'] else 'FAIL'}; state words differing at tick 0/1/10/last: "
              f"{exact[0] if exact else '-'}/{exact[1] if len(exact) > 1 else '-'}/"
              f"{exact[10] if len(exact) > 10 else '-'}/{exact[-1] if exact else '-'}")
        if "control" in entry:
            print(f"  negative control (state replaced): {len(entry['control_failed'])} fields "
                  f"out of tolerance ({', '.join(entry['control_failed'][:6])})")
        for name, v in result.items():
            flag = "" if v["first_fail"] is None else f"  FAIL from tick {v['first_fail']}"
            print(f"  {name:24s} max {v['max_error']:.6g} (tick {v['at_tick']}, tol {v['tolerance']}){flag}")
    if args.json:
        args.json.write_text(json.dumps(report, indent=1), encoding="utf-8", newline="\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
