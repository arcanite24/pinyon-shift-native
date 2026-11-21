"""Each car's cornering on a race trace (SPEC-RACE-AI, Corner speeds per car).

usage: corners.py TRACE.jsonl

Reads a SHIFT_RACE_TRACE file (one JSON line per car every 0.2 s) and
prints, per car, the median of v^2 kappa over part-throttle samples in
curves: kappa from the circle through the car's own samples 6 before and
6 after, kappa > 0.012 per m, v > 8 m/s, throttle under 0.95 (the spec's
measure). The spec's gate: each AI car within 10 % of the oracle row for
its car.
"""
import json
import math
import statistics
import sys
from collections import defaultdict


def circle_kappa(a, b, c):
    ax, az = a[0] - b[0], a[1] - b[1]
    cx, cz = c[0] - b[0], c[1] - b[1]
    cross = ax * cz - az * cx
    la, lc, lac = math.hypot(ax, az), math.hypot(cx, cz), math.hypot(cx - ax, cz - az)
    if la * lc * lac == 0:
        return 0.0
    return 2 * abs(cross) / (la * lc * lac)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cars = defaultdict(list)
    with open(sys.argv[1]) as f:
        for line in f:
            r = json.loads(line)
            if r["t"] < 0:
                continue
            cars[r["car"]].append(r)
    for car in sorted(cars):
        rows = cars[car]
        vals = []
        for i in range(6, len(rows) - 6):
            r = rows[i]
            if r["v"] <= 8 or r["thr"] >= 0.95:
                continue
            k = circle_kappa((rows[i - 6]["x"], rows[i - 6]["z"]), (r["x"], r["z"]), (rows[i + 6]["x"], rows[i + 6]["z"]))
            if k > 0.012:
                vals.append(r["v"] ** 2 * k)
        med = statistics.median(vals) if vals else float("nan")
        print(f"car {car}: v^2 kappa median {med:.1f} m/s^2 ({len(vals)} samples)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
