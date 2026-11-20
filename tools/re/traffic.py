#!/usr/bin/env python3
"""Colorado's road network and a free-roam traffic census (SPEC-TRAFFIC).

`nav` decodes `media/tracks/colorado/colorado.nav` (docs/specs/TRAFFIC.md,
"The road network") and prints its counts, the byte accounting and the
road length per traffic density id, so a reader can check its own decoder
against them.

`census` reads a run of config/render-tests/fh1-traffic-census.fh1test (the
runner's result JSON or the session JSONL) with the fh1.physics_trace
`poses` field and reports, per phase of the route: the number of traffic
cars around the player, their speeds, distance to the player, the road
each one is on (nearest road segment), whether it drives with or against
the way's node order, its lateral offset from the centre line, and the
body mass and step path.

  python -I tools/re/traffic.py nav .local/game/base/media/tracks/colorado/colorado.nav
  python -I tools/re/traffic.py census .local/runs/m4traffic-a/result.json \
      --nav .local/game/base/media/tracks/colorado/colorado.nav
"""
from __future__ import annotations

import argparse
import collections
import json
import math
import statistics
import struct
import sys
from pathlib import Path

MAGIC = 0x0E177551


class Nav:
    """The decoded file: nodes, ways, the two reference lists, tags and
    the string tables (TRAFFIC.md, "File layout")."""

    def __init__(self, path: Path):
        d = path.read_bytes()
        h = struct.unpack_from(">10I", d, 0)
        if h[0] != MAGIC:
            raise SystemExit(f"{path}: magic {h[0]:08X}, expected {MAGIC:08X}")
        (_, nn, nw, nt, nr, nb, nk, nv, kbytes, vbytes) = h
        o = 40
        self.nodes = [struct.unpack_from(">I3f4I", d, o + 32 * i) for i in range(nn)]
        o += 32 * nn
        self.ways = [struct.unpack_from(">4I", d, o + 16 * i) for i in range(nw)]
        o += 16 * nw
        self.refs = struct.unpack_from(f">{nr}I", d, o)
        o += 4 * nr
        self.back = [struct.unpack_from(">2I", d, o + 8 * i) for i in range(nb)]
        o += 8 * nb
        self.tags = [struct.unpack_from(">2I", d, o + 8 * i) for i in range(nt)]
        o += 8 * nt
        koff = struct.unpack_from(f">{nk}I", d, o)
        o += 4 * nk
        voff = struct.unpack_from(f">{nv}I", d, o)
        o += 4 * nv
        kpool, vpool = d[o:o + kbytes], d[o + kbytes:o + kbytes + vbytes]
        o += kbytes + vbytes
        self.trailer = struct.unpack_from(">I", d, o)[0] if o + 4 <= len(d) else None
        self.size, self.end = len(d), o + 4
        cs = lambda pool, x: pool[x:pool.index(b"\0", x)].decode("ascii")
        self.keys = [cs(kpool, x) for x in koff]
        self.values = [cs(vpool, x) for x in voff]
        self.header = h

    def tags_of(self, count: int, start: int) -> dict[str, str]:
        return {self.keys[k]: self.values[v] for k, v in self.tags[start:start + count]}

    def way_tags(self, w) -> dict[str, str]:
        return self.tags_of(w[2], w[3])

    def node_tags(self, n) -> dict[str, str]:
        return self.tags_of(n[6], n[7])

    def way_points(self, w):
        return [self.nodes[i][1:4] for i in self.refs[w[1]:w[1] + w[0]]]

    def roads(self):
        """Ways with a road_type tag: the drivable network. The others are
        map areas and labels."""
        for wi, w in enumerate(self.ways):
            t = self.way_tags(w)
            if "road_type" in t:
                yield wi, t, self.way_points(w)


def traffic_density_id(t: dict[str, str]) -> int | None:
    """The way's traffic density id, or None when traffic may not use it."""
    if "traffic_disabled" in t or "ai_disabled" in t or "traffic_density" not in t:
        return None
    return int(t["traffic_density"])


def cmd_nav(args) -> int:
    nav = Nav(Path(args.nav))
    h = nav.header
    print(f"header: {h[1]} nodes, {h[2]} ways, {h[3]} tags, {h[4]} way-node refs, "
          f"{h[5]} node-way refs, {h[6]} keys, {h[7]} values, {h[8]} + {h[9]} string bytes")
    print(f"size {nav.size}, decoded to {nav.end}, trailer {nav.trailer:08X}")
    bad = 0
    for i, n in enumerate(nav.nodes):
        for k in range(n[4]):
            wi, pos = nav.back[n[5] + k]
            w = nav.ways[wi]
            bad += not (pos < w[0] and nav.refs[w[1] + pos] == i)
    print(f"node-way back references that do not point back: {bad}")
    print("keys:", ", ".join(nav.keys))
    by_id, by_type = collections.defaultdict(float), collections.defaultdict(float)
    ways_by_id = collections.Counter()
    oneway = collections.Counter()
    segments = []
    for wi, t, pts in nav.roads():
        s = [math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
        segments += s
        d = traffic_density_id(t)
        key = d if d is not None else "none"
        by_id[key] += sum(s)
        ways_by_id[key] += 1
        oneway[key] += t.get("oneway") == "true"
        by_type[t["road_type"]] += sum(s)
    roads = sum(1 for _ in nav.roads())
    print(f"roads (ways with road_type): {roads}, {sum(by_type.values()) / 1000:.1f} km, "
          f"{len(segments)} segments, median {statistics.median(segments):.1f} m")
    for k, v in sorted(by_type.items(), key=lambda x: -x[1]):
        print(f"  road_type {k}: {v / 1000:.1f} km")
    print("traffic density id: ways, km, one-way ways")
    for k in sorted(by_id, key=lambda x: (isinstance(x, str), x)):
        print(f"  {k}: {ways_by_id[k]}, {by_id[k] / 1000:.1f}, {oneway[k]}")
    print("node ways:", dict(sorted(collections.Counter(n[4] for n in nav.nodes).items())))
    return 0


class SegmentIndex:
    """Road segments on a 100 m grid of the ground plane (x, z)."""

    def __init__(self, nav: Nav, cell: float = 100.0):
        self.cell, self.grid = cell, collections.defaultdict(list)
        for wi, t, pts in nav.roads():
            for k in range(len(pts) - 1):
                a, b = pts[k], pts[k + 1]
                x0, x1 = sorted((a[0], b[0]))
                z0, z1 = sorted((a[2], b[2]))
                for gx in range(int(x0 // cell), int(x1 // cell) + 1):
                    for gz in range(int(z0 // cell), int(z1 // cell) + 1):
                        self.grid[(gx, gz)].append((wi, k, a, b, t))

    def nearest(self, p):
        best = None
        gx, gz = int(p[0] // self.cell), int(p[2] // self.cell)
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for wi, k, a, b, t in self.grid.get((gx + dx, gz + dz), ()):
                    ex, ez = b[0] - a[0], b[2] - a[2]
                    l2 = ex * ex + ez * ez
                    if l2 == 0:
                        continue
                    u = max(0.0, min(1.0, ((p[0] - a[0]) * ex + (p[2] - a[2]) * ez) / l2))
                    d = math.hypot(p[0] - a[0] - u * ex, p[2] - a[2] - u * ez)
                    if best is None or d < best["d"]:
                        l = math.sqrt(l2)
                        # Signed offset: positive to the left of the way's
                        # node order seen from above with y up (x right, z
                        # forward gives left = -x).
                        cross = (ex * (p[2] - a[2]) - ez * (p[0] - a[0])) / l
                        best = dict(d=d, way=wi, seg=k, dir=(ex / l, ez / l), side=cross, tags=t)
        return best


def load_trace(path: Path):
    if path.suffix != ".jsonl":
        path = Path(json.loads(path.read_text(encoding="utf-8"))["event_log"])
    samples = []
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if '"fh1.physics_trace"' not in line:
                continue
            e = json.loads(line)
            fields = e.get("fields", e)
            if "poses" not in fields:
                continue
            cars = {}
            for packed in fields["cars"].split("|") if fields["cars"] else []:
                i, car, driver, speed = packed.split(":")[:4]
                cars[int(i)] = dict(car=car, driver=int(driver), speed=float(speed))
            for packed in fields["poses"].split("|") if fields["poses"] else []:
                i, x, y, z, mass, step, light = packed.split(":")
                if int(i) in cars:
                    cars[int(i)].update(p=(float(x), float(y), float(z)), mass=float(mass),
                                        path=int(step), light=int(light))
            samples.append((int(fields["frame"]), int(fields.get("mode", 0)), cars))
    samples.sort(key=lambda s: s[0])
    return samples


PHASES = (("noon parked", 1700, 4690), ("02:00 parked", 6500, 8290), ("noon cruise", 8400, 10300))
TRAFFIC, FESTIVAL = 4, 1
ACTIVE_M = 1000.0   # inactive cars wait in a pool about 2.5 km away
ON_ROAD_M = 15.0


def road_length_near(nav: Nav, centre, radius: float) -> dict:
    """Road length (m) per traffic density id within a ground-plane radius."""
    out = collections.defaultdict(float)
    for wi, t, pts in nav.roads():
        for a, b in zip(pts, pts[1:]):
            n = max(1, int(math.dist(a, b) // 2) + 1)
            for j in range(n):
                q = [a[i] + (b[i] - a[i]) * (j + 0.5) / n for i in range(3)]
                if math.hypot(q[0] - centre[0], q[2] - centre[2]) <= radius:
                    out[traffic_density_id(t)] += math.dist(a, b) / n
    return dict(out)


def cmd_census(args) -> int:
    nav = Nav(Path(args.nav))
    index = SegmentIndex(nav)
    samples = load_trace(Path(args.run))
    if not samples:
        print("no fh1.physics_trace events with poses")
        return 1
    drivers = collections.Counter(c["driver"] for _, _, cars in samples for c in cars.values())
    print(f"{len(samples)} samples, frames {samples[0][0]}..{samples[-1][0]}; driver types {dict(drivers)}")
    player_at = {}
    for frame, _, cars in samples:
        p = next((c["p"] for c in cars.values() if c["driver"] == 0 and "p" in c), None)
        if p:
            player_at[frame] = p
    first_player = next(p for f, p in player_at.items() if f >= PHASES[0][1])

    # Spawns and despawns: a traffic car crossing the active radius after
    # the first free-roam frame (the pool reset at entry is not counted).
    previous, spawns, despawns, spawn_away = {}, [], [], [0, 0]
    tracks = collections.defaultdict(list)
    for frame, _, cars in samples:
        p = player_at.get(frame)
        if not p:
            continue
        for c in cars.values():
            if c["driver"] != TRAFFIC or "p" not in c:
                continue
            d = math.dist(c["p"], p)
            tracks[c["car"]].append((frame, d))
            last = previous.get(c["car"])
            if last is not None and last[1] > 0:
                if last[0] >= ACTIVE_M and d < ACTIVE_M:
                    spawns.append((frame, d, c["car"]))
                elif last[0] < ACTIVE_M <= d and last[0] > 1.0:
                    despawns.append((frame, last[0]))
            previous[c["car"]] = (d, frame)
    for frame, d, car in spawns:
        later = [x for x in tracks[car] if frame < x[0] <= frame + 30]
        if later:
            spawn_away[later[-1][1] > d] += 1
    print(f"\nspawns {len(spawns)}: {min(x[1] for x in spawns):.0f} to {max(x[1] for x in spawns):.0f} m"
          f" (frames {[x[0] for x in spawns]})" if spawns else "\nno spawns")
    print(f"  driving away {spawn_away[1]}, towards the player {spawn_away[0]}")
    print(f"despawns {len(despawns)}: {min(x[1] for x in despawns):.0f} to {max(x[1] for x in despawns):.0f} m"
          if despawns else "no despawns")
    for r in (330.0, 370.0, 500.0):
        near = road_length_near(nav, first_player, r)
        print(f"road within {r:.0f} m of the start, m per density id:",
              {str(k): round(v) for k, v in sorted(near.items(), key=lambda x: str(x[0]))})

    report = {}
    for name, f0, f1 in PHASES:
        window = [s for s in samples if f0 <= s[0] <= f1 and s[0] in player_at]
        if not window:
            continue
        counts, festival, speeds, offs = [], [], [], []
        sides = collections.Counter()
        steps = collections.Counter()
        full_d, light_d = [], []
        moving = rejected = 0
        ids, types, models = collections.Counter(), collections.Counter(), collections.Counter()
        pos = collections.defaultdict(list)
        for frame, _, cars in window:
            p = player_at[frame]
            active = [c for c in cars.values()
                      if c["driver"] == TRAFFIC and "p" in c and math.dist(c["p"], p) < ACTIVE_M]
            counts.append(len(active))
            festival.append(sum(1 for c in cars.values() if c["driver"] == FESTIVAL and "p" in c
                                and math.dist(c["p"], p) < ACTIVE_M))
            for c in active:
                d = math.dist(c["p"], p)
                speeds.append(c["speed"])
                steps[(c["path"], c["light"])] += 1
                (full_d if c["path"] == 0 else light_d).append(d)
                models[round(c["mass"], 3)] += 1
                pos[c["car"]].append((frame, c["p"], c["speed"]))
        for car, tr in pos.items():
            for (fa, pa, sa), (fb, pb, sb) in zip(tr, tr[1:]):
                mx, mz = pb[0] - pa[0], pb[2] - pa[2]
                m = math.hypot(mx, mz)
                if fb - fa > 12 or sa < 3 or m < 0.5:
                    continue
                moving += 1
                near = index.nearest(pa)
                if not near or near["d"] > ON_ROAD_M:
                    rejected += 1
                    continue
                t = near["tags"]
                ids[traffic_density_id(t)] += 1
                types[t.get("road_type")] += 1
                forward = (mx * near["dir"][0] + mz * near["dir"][1]) > 0
                left = near["side"] if forward else -near["side"]
                offs.append(left)
                sides["right" if left < 0 else "left"] += 1
                sides["with node order" if forward else "against node order"] += 1
                if t.get("oneway") == "true":
                    sides["one-way, with node order" if forward else "one-way, against node order"] += 1
        q = lambda v, f: sorted(v)[int(f * (len(v) - 1))] if v else None
        r = dict(samples=len(window),
                 traffic_mean=round(statistics.mean(counts), 2), traffic_min=min(counts), traffic_max=max(counts),
                 festival_mean=round(statistics.mean(festival), 2),
                 speed_median_mps=q(speeds, .5),
                 moving_samples=moving, moving_off_road=rejected,
                 offset_left_median_m=q(offs, .5), sides=dict(sides),
                 density_ids={str(k): v for k, v in ids.items()}, road_types=dict(types),
                 step_path_light={f"{k[0]}/{k[1]}": v for k, v in sorted(steps.items())},
                 full_step_distance_max_m=q(full_d, 1.0), full_step_distance_p95_m=q(full_d, .95),
                 light_step_distance_min_m=q(light_d, 0.0),
                 body_masses_100kg=dict(models),
                 player_travel_m=round(math.dist(player_at[window[0][0]], player_at[window[-1][0]]), 1))
        report[name] = r
        print(f"\n{name} (frames {f0}..{f1}):")
        for k, v in r.items():
            print(f"  {k}: {v}")
    if args.output:
        Path(args.output).write_text(json.dumps(report, indent=1), encoding="utf-8")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    n = commands.add_parser("nav")
    n.add_argument("nav")
    c = commands.add_parser("census")
    c.add_argument("run", help="the runner's result.json or the session JSONL")
    c.add_argument("--nav", required=True)
    c.add_argument("--output")
    args = parser.parse_args()
    return {"nav": cmd_nav, "census": cmd_census}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
