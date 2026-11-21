#!/usr/bin/env python3
"""The M5 gate over a set of events (SPEC-RACE-AI Tolerances, the plan's M5).

Runs `carview --drive --headless --event <id>` for each event and seed
(synchronous streaming, so the player never outruns the collision), reads
the results lines, and compares each event against its oracle table
(tools/race/oracle.json, overridden by out/oracle/<event>-results.json):

  - AI mean race time within 3 % of the oracle's,
  - AI mean distance from the grid to the finish within 3 %,
  - the player's place within one of the oracle's,
  - the winner from the front three grid slots.

With out/oracle/<event>-player.csv present the player's car replays the
oracle's path (--race-replay), as the spec's gate asks; --no-replay drives
it with our AI.

usage: gate.py [--events FR22,FR16] [--seeds 1,2] [--jobs 3] [--seconds 400]
               [--exe build-dev/apps/carview/carview.exe] [--extra "..."]
Exit 0 when every event with an oracle passes every check it has data for.
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = os.environ.get("SHIFT_GAME_ROOT", "C:/Users/neri/code/horizon1-recomp/.local/game/base")
RESULT = re.compile(r"^\s+P(\d+)\s+(\S+)\s+best\s+\S+\s+slot (\d+)\s+(\d+) m\s+(player|AI)")


def oracle(event):
    p = ROOT / "out" / "oracle" / f"{event}-results.json"
    if p.exists():
        # The specification side's export (SPEC-EVENTS / SPEC-RACE-AI runs).
        d = json.loads(p.read_text())
        o = {"spec": "out/oracle export", "winner_slot": d.get("winner_slot"), "last_slot": d.get("last_slot")}
        if d.get("ai_times_s"):
            o["ai_times"] = d["ai_times_s"]
        if d.get("ai_mean_distance_m"):
            o["ai_mean_distance"] = d["ai_mean_distance_m"]
        pl = d.get("player") or {}
        if pl.get("place"):
            o["player_place"] = pl["place"]
            o["player_time"] = pl.get("time_s")
        return o
    table = json.loads((ROOT / "tools" / "race" / "oracle.json").read_text())
    return table.get(event)


def clock(s):
    if s == "--":
        return None
    m, sec = s.split(":")
    return int(m) * 60 + float(sec)


def run(exe, event, seed, seconds, extra, outdir, replay):
    log = outdir / f"{event}-s{seed}.log"
    csv = ROOT / "out" / "oracle" / f"{event}-player.csv"
    if replay and csv.exists():
        extra = extra + ["--race-replay", str(csv)]
    cmd = [str(exe), "--drive", "--headless", "--bake", str(ROOT / "out" / "bake"), "--car",
           "NIS_SkylineGTRVSpecII_02", "--world", f"{GAME}/media/tracks/colorado", "--event", event,
           "--seconds", str(seconds), "--every", "100000", "--no-save", "--no-sound", "--size", "64x36",
           "--out", str(outdir / "g.png"), "--race-seed", str(seed)] + extra
    with open(log, "w") as f:
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT).returncode
    cars = []
    for line in log.read_text(errors="replace").splitlines():
        m = RESULT.match(line)
        if m:
            cars.append({"place": int(m[1]), "time": clock(m[2]), "slot": int(m[3]), "distance": int(m[4]),
                         "player": m[5] == "player"})
    return {"event": event, "seed": seed, "rc": rc, "cars": cars, "log": str(log)}


def judge(event, runs, o):
    ai_times, ai_dist, places, winners, dnf = [], [], [], [], 0
    for r in runs:
        for c in r["cars"]:
            if c["player"]:
                places.append(c["place"])
                continue
            if c["time"] is None:
                dnf += 1
                continue
            ai_times.append(c["time"])
            ai_dist.append(c["distance"])
        fin = [c for c in r["cars"] if c["time"] is not None]
        if fin:
            winners.append(min(fin, key=lambda c: c["place"])["slot"])
    row = {"event": event, "runs": len(runs), "dnf": dnf}
    checks = {}
    if ai_times:
        row["ai_mean_time"] = statistics.mean(ai_times)
        row["ai_mean_distance"] = statistics.mean(ai_dist)
    row["player_places"] = places
    row["winner_slots"] = winners
    if o:
        if "ai_times" in o and ai_times:
            ref = statistics.mean(o["ai_times"])
            row["oracle_ai_mean_time"] = ref
            checks["time 3%"] = abs(row["ai_mean_time"] / ref - 1) <= 0.03
        if "ai_mean_distance" in o and ai_dist:
            checks["distance 3%"] = abs(row["ai_mean_distance"] / o["ai_mean_distance"] - 1) <= 0.03
        if "player_place" in o and places:
            checks["place ±1"] = all(abs(p - o["player_place"]) <= 1 for p in places)
        if "winner_slot" in o and winners:
            checks["winner front 3"] = all(w <= 2 for w in winners)
    row["checks"] = checks
    row["pass"] = bool(checks) and all(checks.values()) and dnf == 0
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--events", default="FR22,FR16")
    ap.add_argument("--seeds", default="1,2")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=400)
    ap.add_argument("--exe", default=str(ROOT / "build-dev" / "apps" / "carview" / "carview.exe"))
    ap.add_argument("--out", default=str(ROOT / "out" / "gate-race"))
    ap.add_argument("--extra", default="")
    ap.add_argument("--no-replay", action="store_true",
                    help="the player under our AI instead of the oracle's path (out/oracle/<event>-player.csv)")
    a = ap.parse_args()
    outdir = Path(a.out)
    outdir.mkdir(parents=True, exist_ok=True)
    events = a.events.split(",")
    seeds = [int(s) for s in a.seeds.split(",")]
    extra = a.extra.split()
    with cf.ThreadPoolExecutor(a.jobs) as pool:
        futs = [pool.submit(run, a.exe, e, s, a.seconds, extra, outdir, not a.no_replay) for e in events for s in seeds]
        results = [f.result() for f in futs]
    rows = []
    for e in events:
        rows.append(judge(e, [r for r in results if r["event"] == e], oracle(e)))
    ok = True
    for r in rows:
        t = f"{r.get('ai_mean_time', 0):.1f}" + (f"/{r['oracle_ai_mean_time']:.1f}" if "oracle_ai_mean_time" in r else "")
        checks = " ".join(f"{k}:{'ok' if v else 'FAIL'}" for k, v in r["checks"].items()) or "no oracle"
        print(f"{r['event']:8} runs {r['runs']} ai time {t} s dist {r.get('ai_mean_distance', 0):.0f} m "
              f"player {r['player_places']} winners {r['winner_slots']} dnf {r['dnf']}  {checks}")
        if r["checks"]:
            ok &= r["pass"]
    (outdir / "gate.json").write_text(json.dumps({"rows": rows, "runs": results}, indent=1))
    print("gate:", "PASS" if ok else "FAIL", f"({sum(1 for r in rows if r['checks'])} events with an oracle)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
