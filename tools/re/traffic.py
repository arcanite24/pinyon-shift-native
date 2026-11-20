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


def cmd_census(args) -> int:
    nav = Nav(Path(args.nav))
    index = SegmentIndex(nav)
    samples = load_trace(Path(args.run))
    if not samples:
        print("no fh1.physics_trace events with poses")
        return 1
    drivers = collections.Counter(c["driver"] for _, _, cars in samples for c in cars.values())
    print(f"{len(samples)} samples, frames {samples[0][0]}..{samples[-1][0]}; driver types {dict(drivers)}")
    report = {}
    for name, f0, f1 in PHASES:
        window = [s for s in samples if f0 <= s[0] <= f1]
        if not window:
            continue
        counts, speeds, dists, offs, side_sign = [], [], [], [], collections.Counter()
        with_way, roads, ids, masses, paths = 0, collections.Counter(), collections.Counter(), {}, collections.Counter()
        ppos = []
        tracks = collections.defaultdict(list)
        for frame, mode, cars in window:
            player = next((c for c in cars.values() if c["driver"] == 0 and "p" in c), None)
            others = [(i, c) for i, c in cars.items() if c["driver"] != 0 and "p" in c]
            counts.append(len(others))
            if player:
                ppos.append(player["p"])
            for i, c in others:
                speeds.append(c["speed"])
                if player:
                    dists.append(math.dist(c["p"], player["p"]))
                masses[c["car"]] = c["mass"]
                paths[c["path"]] += 1
                tracks[c["car"]].append((frame, c["p"], c["speed"]))
        # Direction and lane side from each car's own motion between samples.
        for car, tr in tracks.items():
            for (fa, pa, sa), (fb, pb, sb) in zip(tr, tr[1:]):
                if fb - fa > 12 or sa < 3:
                    continue
                mx, mz = pb[0] - pa[0], pb[2] - pa[2]
                m = math.hypot(mx, mz)
                if m < 0.5:
                    continue
                near = index.nearest(pa)
                if not near or near["d"] > 15:
                    continue
                with_way += 1
                t = near["tags"]
                dens = traffic_density_id(t)
                ids[dens] += 1
                roads[t.get("road_type")] += 1
                along = (mx * near["dir"][0] + mz * near["dir"][1]) / m
                forward = along > 0
                # Offset to the car's own left: positive = left of travel.
                left = near["side"] if forward else -near["side"]
                offs.append(left)
                side_sign["right" if left < 0 else "left"] += 1
                side_sign["with node order" if forward else "against node order"] += 1
                side_sign["oneway"] += t.get("oneway") == "true"
                side_sign["oneway against"] += t.get("oneway") == "true" and not forward
        q = lambda v, p: sorted(v)[int(p * (len(v) - 1))] if v else float("nan")
        r = dict(samples=len(window), cars_mean=statistics.mean(counts), cars_min=min(counts),
                 cars_max=max(counts), distinct_cars=len(tracks),
                 speed_median_mps=q(speeds, .5), speed_p90_mps=q(speeds, .9),
                 distance_median_m=q(dists, .5), distance_max_m=max(dists) if dists else None,
                 moving_samples_on_road=with_way, offset_left_median_m=q(offs, .5),
                 offset_abs_median_m=q([abs(x) for x in offs], .5),
                 sides=dict(side_sign), road_types=dict(roads),
                 density_ids={str(k): v for k, v in ids.items()},
                 step_path=dict(paths), masses_100kg=masses,
                 player_travel_m=math.dist(ppos[0], ppos[-1]) if len(ppos) > 1 else 0.0,
                 player_start=ppos[0] if ppos else None)
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
