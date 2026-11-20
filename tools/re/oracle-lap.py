#!/usr/bin/env python3
"""Turn an oracle-lap run into the M3 gate's lap trace (CAR_PHYSICS.md,
"Oracle lap for the M3 gate").

The run is config/render-tests/fh1-oracle-lap.fh1test on a seed from
tools/create-oracle-lap-seed.py, with the body oracle on
(`--pinyon_shift_physics_oracle=true`, a snapshot frame before the start, no
inject frames, enough trace ticks) and `--pinyon_shift_physics_trace=6`. The
oracle file holds the player's whole body at every physics step with the
step's dt; this resamples it on simulated time and writes:

  lap.jsonl   one fh1.physics_trace event per 1/60 s of simulated time from
              the start signal, readable by tools/physics-behaviour.py with
              --clock-hz 60, with the pose and accelerations as extra keys,
              and the run's final dlc.rally.race_trace event;
  lap.json    the race, gate, start state, settings and checks.

  python -I tools/re/oracle-lap.py .local/runs/m3lap-a --output .local/re/physics/oracle-lap
  python -I tools/re/oracle-lap.py RUN_A --repeat RUN_B --output DIR

Time: t = 0 is the start signal, found as the simulated time of the finish
crossing minus the race time the game reports, so the lap time and the race
time agree by construction; the start check (the first motion against t = 0)
and the gate check (the gate crossing against the finish) test that.
"""
from __future__ import annotations

import argparse
import bisect
import json
import math
import re
import struct
from pathlib import Path

HEADER = struct.Struct("<IHHIIQddIIIIIIQ")
MAGIC = 0x544F5350
DYN = 0xAD0
WHEELS = DYN + 3936
STRIDE = 1024
CONTROLS = DYN + 3904      # gas, brake, steer, clutch, handbrake (f32)
POSITION = 0x4EA0
VELOCITY = 0xC0
ROWS = (0x70, 0x80, 0x90)  # body x, y, z axes in the world (z forward)
MASS = 0x160               # units of 100 kg
HZ = 60
G = 9.80665


def f32(blob: bytes, offset: int) -> float:
    return struct.unpack_from(">f", blob, offset)[0]


def vec(blob: bytes, offset: int) -> tuple[float, float, float]:
    return struct.unpack_from(">fff", blob, offset)


def read_steps(path: Path) -> list[dict]:
    """Window 0 of a body oracle trace as per-step states (simulated time is
    the sum of the dt of the steps before the record)."""
    steps, t = [], 0.0
    with path.open("rb") as stream:
        while True:
            head = stream.read(HEADER.size)
            if len(head) < HEADER.size:
                break
            (magic, _, kind, window, tick, frame, dt, game_dt, body, size, *_rest) = HEADER.unpack(head)
            if magic != MAGIC:
                raise SystemExit(f"{path}: bad record")
            blob = stream.read(size)
            if len(blob) < size:
                break
            if kind != 0 or window != 0:
                continue
            wheels = [WHEELS + STRIDE * w for w in range(4)]
            steps.append(dict(
                tick=tick, frame=frame, t=t, dt=dt, body=body,
                pos=vec(blob, POSITION), vel=vec(blob, VELOCITY),
                axes=[vec(blob, row) for row in ROWS], mass=f32(blob, MASS) * 100,
                speed=f32(blob, DYN + 3128), rpm=f32(blob, DYN + 324) * 60 / (2 * math.pi),
                gear=blob[DYN + 2240],
                controls=[f32(blob, CONTROLS + 4 * i) for i in range(5)],
                surface=[blob[w + 337] for w in wheels], contact=[int(blob[w + 796] != 0) for w in wheels],
                abs_active=struct.unpack_from(">i", blob, DYN + 3020)[0],
                # The cockpit lights' sources (finding "ABS, traction and
                # stability flags"): TCS intervention amount, STM active.
                tcs=f32(blob, DYN + 3004), stm=struct.unpack_from(">i", blob, DYN + 3016)[0],
                abs_setting=struct.unpack_from(">I", blob, DYN + 3436)[0],
                # The grip factor on dirt seen under the AI, front and rear axles (finding
                # "grip factor on dirt under the AI"): 1 unless a wheel is on dirt.
                ai_grip=(f32(blob, wheels[0] + 0x4C), f32(blob, wheels[2] + 0x4C))))
            t += abs(dt)
    return steps


def repair_controls(steps: list[dict]) -> int:
    """The input side clears the control words between steps, so a step's
    entry reads exactly 0 now and then under a held pedal (finding "controls
    at step entry are not the step's input"). A lone exact zero between two
    non-zero neighbours is taken as that clear and replaced by their mean."""
    repaired = 0
    for i in range(1, len(steps) - 1):
        for c in range(5):
            a, b, n = steps[i - 1]["controls"][c], steps[i]["controls"][c], steps[i + 1]["controls"][c]
            if b == 0.0 and a != 0.0 and n != 0.0 and (a > 0) == (n > 0):
                steps[i]["controls"][c] = 0.5 * (a + n)
                repaired += 1
    return repaired


def double_zeros(steps: list[dict], go: float, end: float) -> int:
    """Steps left with gas and brake both 0 for one or two steps between
    neighbours with the gas fully open: clears the lone-zero repair misses."""
    lap = [s for s in steps if go <= s["t"] <= end]
    count = 0
    for i in range(1, len(lap) - 3):
        for width in (1, 2):
            run = lap[i:i + width]
            if (lap[i - 1]["controls"][0] > 0.99 and lap[i + width]["controls"][0] > 0.99 and
                    all(r["controls"][0] == 0 and r["controls"][1] == 0 for r in run)):
                count += width
    return count


def events(path: Path) -> list[dict]:
    out = []
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("{"):
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return out


def race_result(log: list[dict]) -> dict:
    finish = [e for e in log if e.get("event") == "dlc.rally.race_trace" and
              (e.get("started"), e.get("ended"), e.get("end_reason")) == ("1", "1", "1")]
    if not finish:
        raise SystemExit("the run has no finished race (dlc.rally.race_trace ended=1 end_reason=1)")
    return finish[-1]


def route_gate(route_xml: Path) -> dict:
    """The start gantry of the track's route file: the start/finish line is
    taken through it, across its facing (checked against the finish)."""
    text = route_xml.read_text(encoding="utf-8", errors="replace")
    named = {}
    for name, x, y, z, fx, fz in re.findall(
            r"name='([^']+)'[^>]*>\s*<Transform pos.x='([^']+)' pos.y='([^']+)' pos.z='([^']+)' "
            r"facing.x='([^']+)' facing.y='[^']+' facing.z='([^']+)'", text):
        named[name] = tuple(map(float, (x, y, z, fx, fz)))
    return named


def crossings(steps: list[dict], point, normal) -> list[float]:
    """Simulated times at which the body point crosses the plane through
    point (x, z) with horizontal normal, in the normal's direction."""
    out = []
    side = lambda p: (p[0] - point[0]) * normal[0] + (p[2] - point[1]) * normal[1]
    for a, b in zip(steps, steps[1:]):
        sa, sb = side(a["pos"]), side(b["pos"])
        if sa < 0 <= sb:
            out.append(a["t"] + (b["t"] - a["t"]) * (-sa) / (sb - sa))
    return out


def interpolate(steps: list[dict], times: list[float], t: float) -> dict:
    i = max(0, min(bisect.bisect_right(times, t) - 1, len(steps) - 2))
    a, b = steps[i], steps[i + 1]
    u = 0.0 if b["t"] == a["t"] else min(1.0, max(0.0, (t - a["t"]) / (b["t"] - a["t"])))
    lerp = lambda p, q: tuple(x + (y - x) * u for x, y in zip(p, q))
    near = a if u < 0.5 else b
    return dict(pos=lerp(a["pos"], b["pos"]), vel=lerp(a["vel"], b["vel"]),
                axes=[lerp(p, q) for p, q in zip(a["axes"], b["axes"])],
                speed=a["speed"] + (b["speed"] - a["speed"]) * u, rpm=a["rpm"] + (b["rpm"] - a["rpm"]) * u,
                controls=list(lerp(a["controls"], b["controls"])), gear=near["gear"],
                ai_grip=list(lerp(a["ai_grip"], b["ai_grip"])),
                surface=near["surface"], contact=near["contact"])


def gear_label(raw: int) -> int:
    # The trace format: 0 reverse, 1-8 forward, 9 neutral in the body; the
    # fh1.physics_trace field writes -1 reverse and 0 neutral.
    return -1 if raw == 0 else 0 if raw >= 9 else raw


def analyse(run: Path, route_xml: Path) -> dict:
    result = json.loads((run / "result.json").read_text(encoding="utf-8"))
    log = events(Path(result["event_log"]))
    steps = read_steps(run / "physics-oracle.bin")
    if len(steps) < 100:
        raise SystemExit(f"{run}: only {len(steps)} oracle steps")
    repaired = repair_controls(steps)
    finish = race_result(log)
    race_s = float(finish["time_72"])
    named = route_gate(route_xml)
    gx, gy, gz, fx, fz = named["start_gantry_cannon_00"]
    norm = math.hypot(fx, fz)
    # The cars cross the line along the gantry's facing.
    normal = (fx / norm, fz / norm)
    cross = crossings(steps, (gx, gz), normal)
    times = [s["t"] for s in steps]
    # The launch: the first step with the gas pressed at a standstill (the
    # car is dropped onto the grid first, which moves it without gas).
    launch = next((s["t"] for s in steps if s["controls"][0] > 0.05 and s["speed"] < 1.0), None)
    if launch is None or not cross:
        raise SystemExit(f"{run}: no launch or the car never crossed the gantry line")
    # The finish is the crossing that lies about the race time after the
    # launch; t = 0 is that crossing minus the race time.
    finish_t = min(cross, key=lambda c: abs(c - launch - race_s))
    go = finish_t - race_s
    moving = next(s["t"] for s in steps if s["t"] >= launch and s["speed"] > 0.5)
    # Wall and car contact: a horizontal acceleration the tyres cannot make.
    peak, peak_t = 0.0, None
    for a, b in zip(steps, steps[1:]):
        if b["t"] < go or a["t"] > finish_t or b["dt"] <= 0:
            continue
        dv = [q - p for p, q in zip(a["vel"], b["vel"])]
        acc = math.hypot(dv[0], dv[2]) / (b["t"] - a["t"])
        if acc > peak:
            peak, peak_t = acc, a["t"] - go
    samples = []
    n = int(math.floor((finish_t - go) * HZ)) + 1
    previous = None
    distance = 0.0
    for k in range(-int(2 * HZ), n + int(1 * HZ)):
        t = go + k / HZ
        if t < steps[0]["t"] or t > steps[-1]["t"]:
            continue
        s = interpolate(steps, times, t)
        fwd, up = s["axes"][2], s["axes"][1]
        right = s["axes"][0]
        if previous is not None:
            distance += math.dist(previous["pos"], s["pos"])
        # Accelerations from the velocity one sample either side.
        lo, hi = interpolate(steps, times, t - 0.5 / HZ), interpolate(steps, times, t + 0.5 / HZ)
        acc = [(q - p) * HZ for p, q in zip(lo["vel"], hi["vel"])]
        a_long = sum(x * y for x, y in zip(acc, fwd))
        a_lat = sum(x * y for x, y in zip(acc, right))
        gas, brake, steer, clutch, handbrake = s["controls"]
        samples.append(dict(
            event="fh1.physics_trace", frame=k, mode="3", t_s=round(k / HZ, 6),
            cars=f"0:R34:0:{s['speed']:.3f}:{s['rpm']:.0f}:{gear_label(s['gear'])}:{gas:.3f}:{brake:.3f}:{steer:.3f}",
            x=round(s["pos"][0], 4), y=round(s["pos"][1], 4), z=round(s["pos"][2], 4),
            yaw_deg=round(math.degrees(math.atan2(fwd[0], fwd[2])), 3),
            forward=[round(v, 5) for v in fwd], up=[round(v, 5) for v in up],
            vx=round(s["vel"][0], 4), vy=round(s["vel"][1], 4), vz=round(s["vel"][2], 4),
            a_long=round(a_long, 3), a_lat=round(a_lat, 3), clutch=round(clutch, 3),
            handbrake=round(handbrake, 3), distance_m=round(distance, 3) if k >= 0 else 0.0,
            surfaces=s["surface"], contact=s["contact"],
            ai_grip=[round(v, 4) for v in s["ai_grip"]]))
        previous = s if k >= 0 else None
    lap = [x for x in samples if 0 <= x["t_s"] <= race_s]
    # At 60 Hz (the velocity differenced over 1/60 s) the step-to-step
    # noise of kerbs and the ABS is smoothed out; a wall strike still shows
    # as a speed loss well beyond the brakes' 1 g.
    peak60 = max(lap, key=lambda x: math.hypot(x["a_long"], x["a_lat"]))
    wheel_steps = [w for x in lap for w in x["surfaces"]]
    surface_share = {str(k): round(wheel_steps.count(k) / len(wheel_steps), 4) for k in sorted(set(wheel_steps))}
    start = interpolate(steps, times, go)
    fwd = start["axes"][2]
    return dict(
        run=str(run), event_log=result["event_log"], steps=len(steps), controls_repaired=repaired,
        mass_kg=round(steps[0]["mass"], 1), abs_setting=steps[0]["abs_setting"],
        abs_active_steps=sum(1 for s in steps if go <= s["t"] <= finish_t and s["abs_active"]),
        lap_steps=sum(1 for s in steps if go <= s["t"] <= finish_t),
        tcs_max=max(s["tcs"] for s in steps if go <= s["t"] <= finish_t),
        tcs_light_steps=sum(1 for s in steps if go <= s["t"] <= finish_t and s["tcs"] > 0.1),
        stm_active_steps=sum(1 for s in steps if go <= s["t"] <= finish_t and s["stm"]),
        gas_brake_double_zero_steps=double_zeros(steps, go, finish_t),
        dt_s=dict(min=min(s["dt"] for s in steps), max=max(s["dt"] for s in steps),
                  mean=sum(s["dt"] for s in steps) / len(steps)),
        race_seconds=race_s, race_trace=finish, go_sim_s=go, finish_sim_s=finish_t,
        lap_seconds=finish_t - go, first_motion_after_go_s=moving - go,
        launch_after_go_s=launch - go,
        gantry_crossings_after_go_s=[c - go for c in cross],
        gate=dict(point_xz=[gx, gz], height=gy, crossing_direction_xz=list(normal)),
        start_state=dict(position=list(start["pos"]), forward=list(fwd), speed=start["speed"],
                         gear=gear_label(start["gear"]), rpm=start["rpm"],
                         yaw_deg=math.degrees(math.atan2(fwd[0], fwd[2]))),
        lap_distance_m=lap[-1]["distance_m"],
        peak_step_accel_g=peak / G, peak_step_accel_at_s=peak_t,
        peak_60hz_accel_g=math.hypot(peak60["a_long"], peak60["a_lat"]) / G, peak_60hz_at_s=peak60["t_s"],
        min_a_long_g=min(x["a_long"] for x in lap) / G, max_a_lat_g=max(abs(x["a_lat"]) for x in lap) / G,
        top_speed_kmh=max(float(x["cars"].split(":")[3]) for x in lap) * 3.6,
        wheel_surface_share=surface_share,
        surfaces_seen=sorted({w for s in steps if go <= s["t"] <= finish_t for w in s["surface"]}),
        samples=samples)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", type=Path, help="render-test output directory of the oracle run")
    parser.add_argument("--repeat", type=Path, action="append", default=[],
                        help="further runs of the same route, for the spread")
    parser.add_argument("--route-xml", type=Path,
                        default=Path(".local/game/base/media/tracks/colorado/Ribbon_00/TrackRoute158.xml"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--meta", type=Path, help="JSON merged into lap.json (race, settings)")
    args = parser.parse_args()
    report = analyse(args.run, args.route_xml)
    repeats = [analyse(run, args.route_xml) for run in args.repeat]
    args.output.mkdir(parents=True, exist_ok=True)
    samples = report.pop("samples")
    with (args.output / "lap.jsonl").open("w", encoding="utf-8", newline="\n") as out:
        for sample in samples:
            out.write(json.dumps(sample, separators=(",", ":")) + "\n")
        out.write(json.dumps(dict(report["race_trace"], time_72=f"{report['lap_seconds']:.6f}"),
                             separators=(",", ":")) + "\n")
    runs = [report] + repeats
    for r in repeats:
        r.pop("samples")
    lap_times = [r["lap_seconds"] for r in runs]
    race_times = [r["race_seconds"] for r in runs]
    meta = json.loads(args.meta.read_text(encoding="utf-8")) if args.meta else {}
    meta.update(trace=report, repeats=repeats, lap_seconds=lap_times, race_seconds=race_times,
                spread_s=max(lap_times) - min(lap_times),
                spread_percent=100 * (max(lap_times) - min(lap_times)) / min(lap_times))
    (args.output / "lap.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in meta.items() if k not in ("trace", "repeats")}, indent=2))
    print(f"lap {report['lap_seconds']:.3f} s, first motion {report['first_motion_after_go_s']:+.3f} s, "
          f"launch {report['launch_after_go_s']:+.3f} s, crossings {[round(c, 2) for c in report['gantry_crossings_after_go_s']]}, "
          f"peak (60 Hz) {report['peak_60hz_accel_g']:.2f} g at {report['peak_60hz_at_s']} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
