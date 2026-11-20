#!/usr/bin/env python3
"""Read Forza Horizon 1's Colorado collision tiles (M3, FORMAT-COLLISION).

The ground, roads, walls and barriers the car touches are the `N.fiz`
members of `media/tracks/colorado/bin.zip`: 3,201 tiles of 100 m on a
133 x 100 lattice, each with an 8 m apron, holding a triangle soup with a
surface index per triangle and a 10 m grid of triangle lists. The layout
is our own reading of the player's files, checked against the physics
oracle's wheel blocks; it is written up in docs/formats/COLLISION.md.
Nothing here contains game data: it reads the player's extracted members
(`tools/re/world-geometry.py extract`).

  collision.py stats <members dir> [--col <Colorado_track_00.col>]
  collision.py ground <members dir> X Z
  collision.py oracle-check <members dir> <physics-oracle.bin>... [--radius F R]

`stats` parses every tile, checks the stated sizes, vertex references and
grid lists, cross-checks the `.col` tile index when given, and prints the
counts the spec's tests quote. `ground` lists every triangle under a point
(game frame) with its height, surface index and normal. `oracle-check`
reads RF-5 physics-oracle traces (src/guest_native/car_physics.cpp) and,
for every wheel in contact, finds the triangle the wheel block caches
among the tiles' triangles, compares surface indices and measures the
hub's height over that plane against the rolling radius.
"""

from __future__ import annotations

import argparse
import collections
import math
import os
import statistics
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

LATTICE_X0 = -6408.8564453125   # stated by the .col header; checked by stats
LATTICE_Z0 = -6132.85986328125
CELL = 100.0
APRON = 8.0
LATTICE_W = 133

# RF-5 oracle trace (src/guest_native/car_physics.cpp)
ORACLE_HEADER = struct.Struct("<IHHIIQddIIIIIIQ")
ORACLE_MAGIC = 0x544F5350
WHEELS = 0x1A30        # body offset of wheel block 0; blocks are 1024 bytes
W_HUB = 0xD0           # wheel hub, float3, world (game frame), m
W_TRI = (0x170, 0x180, 0x190)  # the contact triangle's three vertices
W_SURFACE = 337        # surface index byte
W_CONTACT = 796        # nonzero while the wheel touches the ground
BODY_POSITION = 0x4EA0


@dataclass
class Triangle:
    flags: int
    surface: int
    word: int
    v: tuple[int, int, int]
    normal: tuple[float, float, float]
    tail: int


@dataclass
class Tile:
    number: int
    box: tuple[float, float, float, float]   # x0, z0, x1, z1 (apron included)
    grid: tuple[int, int]
    cell: tuple[float, float, float, float]  # cell x, z and their inverses
    verts: list[tuple[float, float, float, int]]
    tris: list[Triangle]
    lists: list[list[int]]
    size_a: int
    size_b: int
    tail_magic: bytes
    file_size: int
    problems: list[str] = field(default_factory=list)

    def lattice(self) -> tuple[int, int]:
        return (round((self.box[0] + APRON - LATTICE_X0) / CELL),
                round((self.box[1] + APRON - LATTICE_Z0) / CELL))


def read_tile(path: str | os.PathLike) -> Tile:
    data = Path(path).read_bytes()
    magic, version, size_a, size_b, _ = struct.unpack_from(">4sIIII", data, 0)
    if magic != b"fiz " or version != 1:
        raise ValueError(f"{path}: not a version-1 fiz tile")
    a = 0x20
    nv, nt, voff, toff, gw, gh = struct.unpack_from(">6I", data, a)
    x0, z0, x1, z1, cx, cz, icx, icz = struct.unpack_from(">8f", data, a + 24)
    goff, = struct.unpack_from(">I", data, a + 56)
    verts = [struct.unpack_from(">fffI", data, a + voff + 16 * i) for i in range(nv)]
    tile = Tile(int(Path(path).stem), (x0, z0, x1, z1), (gw, gh), (cx, cz, icx, icz), verts, [], [],
                size_a, size_b, data[a + size_a:a + size_a + 4] if size_b else b"", len(data))
    for i in range(nt):
        o = a + toff + 32 * i
        flags, surface, word = struct.unpack_from(">BBH", data, o)
        refs = struct.unpack_from(">3I", data, o + 4)
        if any((r - voff) % 16 or not 0 <= (r - voff) // 16 < nv for r in refs):
            tile.problems.append(f"triangle {i} vertex reference")
        v = tuple((r - voff) // 16 for r in refs)
        normal = struct.unpack_from(">3f", data, o + 16)
        tail, = struct.unpack_from(">I", data, o + 28)
        tile.tris.append(Triangle(flags, surface, word, v, normal, tail))
    for c in range(gw * gh):
        off, = struct.unpack_from(">I", data, a + goff + 4 * c)
        if off == 0:
            tile.lists.append([])
            continue
        n, = struct.unpack_from(">H", data, a + off)
        ids = list(struct.unpack_from(f">{n}H", data, a + off + 2))
        if any(t >= nt for t in ids):
            tile.problems.append(f"cell {c} list")
        tile.lists.append(ids)
    if 0x20 + size_a + size_b != len(data):
        tile.problems.append("block sizes")
    return tile


def tile_paths(root: str) -> list[Path]:
    return sorted(Path(root).glob("*.fiz"), key=lambda p: int(p.stem))


def corners(tile: Tile, t: Triangle):
    return [tile.verts[k][:3] for k in t.v]


def under(tile: Tile, t: Triangle, x: float, z: float) -> float | None:
    """Height of the triangle's plane at (x, z) if (x, z) is inside it in X/Z."""
    a, b, c = corners(tile, t)
    d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
    if abs(d) < 1e-12:
        return None
    l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
    l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
    l3 = 1 - l1 - l2
    if min(l1, l2, l3) < -1e-6:
        return None
    return l1 * a[1] + l2 * b[1] + l3 * c[1]


class World:
    def __init__(self, root: str):
        self.paths = tile_paths(root)
        self.cache: dict[Path, Tile] = {}
        self.boxes = []
        for p in self.paths:
            head = p.open("rb").read(0x50)
            self.boxes.append((struct.unpack_from(">4f", head, 0x38), p))

    def tile(self, p: Path) -> Tile:
        if p not in self.cache:
            self.cache[p] = read_tile(p)
        return self.cache[p]

    def tiles_at(self, x: float, z: float, margin: float = 0.0):
        for (x0, z0, x1, z1), p in self.boxes:
            if x0 - margin <= x <= x1 + margin and z0 - margin <= z <= z1 + margin:
                yield self.tile(p)

    def hits(self, x: float, z: float):
        out = []
        for tile in self.tiles_at(x, z):
            gx = min(int((x - tile.box[0]) * tile.cell[2]), tile.grid[0] - 1)
            gz = min(int((z - tile.box[1]) * tile.cell[3]), tile.grid[1] - 1)
            for i in tile.lists[gz * tile.grid[0] + gx]:
                t = tile.tris[i]
                y = under(tile, t, x, z)
                if y is not None:
                    out.append((y, t, tile.number, i))
        return sorted(out, key=lambda h: h[0])


def cmd_stats(args) -> int:
    paths = tile_paths(args.members)
    counts = collections.Counter()
    surfaces = collections.Counter()
    up_by_surface = collections.Counter()
    flags = collections.Counter()
    words = collections.Counter()
    tails = collections.Counter()
    seen_core = set()
    present = set()
    problems = 0
    numbering_ok = True
    tiles = []
    for p in paths:
        t = read_tile(p)
        tiles.append((t.number, t.lattice(), t.size_a))
        present.add(t.lattice())
        problems += len(t.problems)
        for msg in t.problems[:3]:
            print(f"{p.name}: {msg}", file=sys.stderr)
        counts["vertices"] += len(t.verts)
        counts["triangles"] += len(t.tris)
        counts["with second block"] += bool(t.size_b)
        listed = set(i for ids in t.lists for i in ids)
        counts["unlisted triangles"] += len(t.tris) - len(listed)
        x0, z0 = t.box[0] + APRON, t.box[1] + APRON
        for tri in t.tris:
            a, b, c = corners(t, tri)
            up = tri.normal[1] > 0.7
            surfaces[tri.surface] += 1
            up_by_surface[tri.surface] += up
            flags[tri.flags] += 1
            words["0x8000" if tri.word == 0x8000 else "other"] += 1
            tails[tri.tail == 0x00FFFFFF] += 1
            cx, cz = (a[0] + b[0] + c[0]) / 3, (a[2] + b[2] + c[2]) / 3
            if x0 <= cx < x0 + CELL and z0 <= cz < z0 + CELL:
                counts["core triangles"] += 1
                seen_core.add(tuple(sorted((a, b, c))))
            u = [b[i] - a[i] for i in range(3)]
            v = [c[i] - a[i] for i in range(3)]
            n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            length = math.sqrt(sum(k * k for k in n))
            if length > 1e-9:
                dot = sum(n[i] / length * tri.normal[i] for i in range(3))
                counts["normal = (b-a)x(c-a)" if dot > 0.999 else "normal other"] += 1
    order = sorted(tiles, key=lambda e: (e[1][1], e[1][0]))
    numbering_ok = all(e[0] == k for k, e in enumerate(order))
    print(f"tiles {len(paths)}, problems {problems}, numbered row-major by lattice z then x: {numbering_ok}")
    print(f"lattice cells used {len(present)}; x {min(c[0] for c in present)}..{max(c[0] for c in present)}, "
          f"z {min(c[1] for c in present)}..{max(c[1] for c in present)}")
    for key in ("vertices", "triangles", "core triangles", "unlisted triangles", "with second block",
                "normal = (b-a)x(c-a)", "normal other"):
        print(f"{key}: {counts[key]}")
    print(f"distinct core triangles: {len(seen_core)}")
    print("surface index: triangles (up-facing)")
    for s in sorted(surfaces):
        print(f"  {s:3d}: {surfaces[s]} ({up_by_surface[s]})")
    print("flag byte:", sorted(flags.items()))
    print("16-bit word:", dict(words), " tail all-ones-24:", dict(tails))
    if args.col:
        col = Path(args.col).read_bytes()
        w, h = struct.unpack_from(">II", col, 0)
        x0, z0, x1, z1 = struct.unpack_from(">4f", col, 8)
        n, = struct.unpack_from(">I", col, 0x4C)
        recs = [struct.unpack_from(">6I", col, 0x50 + 24 * k) for k in range(n)]
        by_number = {e[0]: e for e in tiles}
        ok = sum(1 for r in recs if r[2] in by_number and
                 r[3] == by_number[r[2]][1][1] * w + by_number[r[2]][1][0] and r[5] == by_number[r[2]][2])
        contiguous = all(recs[k + 1][1] == recs[k][1] + recs[k][5] for k in range(n - 1))
        print(f".col: lattice {w} x {h} from ({x0}, {z0}) to ({x1}, {z1}), {n} records; "
              f"cell, tile number and first-block size agree for {ok}; offsets contiguous {contiguous}")
    return 1 if problems else 0


def cmd_ground(args) -> int:
    world = World(args.members)
    for y, t, number, i in world.hits(args.x, args.z):
        print(f"y {y:.4f}  surface {t.surface}  flags {t.flags}  normal "
              f"({t.normal[0]:.4f}, {t.normal[1]:.4f}, {t.normal[2]:.4f})  tile {number} triangle {i}")
    return 0


def oracle_records(path: Path):
    data = path.read_bytes()
    at = 0
    while at + ORACLE_HEADER.size <= len(data):
        h = ORACLE_HEADER.unpack_from(data, at)
        if h[0] != ORACLE_MAGIC:
            raise ValueError(f"{path}: bad record at {at}")
        size = h[9]
        yield h[2], data[at + ORACLE_HEADER.size:at + ORACLE_HEADER.size + size]
        at += ORACLE_HEADER.size + size


def cmd_oracle_check(args) -> int:
    world = World(args.members)
    radius = [args.radius[0]] * 2 + [args.radius[1]] * 2
    stats = collections.Counter()
    gaps = collections.defaultdict(list)
    for path in args.traces:
        seen = set()
        for kind, body in oracle_records(Path(path)):
            if kind == 2:
                continue
            key = body[BODY_POSITION:BODY_POSITION + 12]
            if key in seen:      # injected windows repeat states
                continue
            seen.add(key)
            for w in range(4):
                wb = WHEELS + 1024 * w
                if not body[wb + W_CONTACT]:
                    continue
                stats["wheel samples"] += 1
                cached = {struct.unpack_from(">3f", body, wb + o) for o in W_TRI}
                hub = struct.unpack_from(">3f", body, wb + W_HUB)
                found = None
                for tile in world.tiles_at(hub[0], hub[2], margin=5.0):
                    for t in tile.tris:
                        if {tuple(struct.unpack(">3f", struct.pack(">3f", *c))) for c in corners(tile, t)} == cached:
                            found = (tile, t)
                            break
                    if found:
                        break
                if not found:
                    stats["cached triangle not in tiles"] += 1
                    continue
                tile, t = found
                stats["cached triangle found"] += 1
                stats["surface index equal" if t.surface == body[wb + W_SURFACE] else "surface index differs"] += 1
                y = under(tile, t, hub[0], hub[2])
                if y is None:
                    stats["hub outside its triangle in X/Z"] += 1
                    continue
                gaps[w].append((hub[1] - y) * t.normal[1] - radius[w])
    for k, v in sorted(stats.items()):
        print(f"{k}: {v}")
    for w in range(4):
        v = sorted(gaps[w])
        if v:
            print(f"wheel {w}: {len(v)} samples, hub-to-plane minus rolling radius median {statistics.median(v):+.4f} m, "
                  f"5th {v[len(v) // 20]:+.4f}, 95th {v[-1 - len(v) // 20]:+.4f}, range {v[0]:+.4f}..{v[-1]:+.4f}")
    return 0 if stats["cached triangle not in tiles"] == 0 and stats["surface index differs"] == 0 else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("stats")
    p.add_argument("members")
    p.add_argument("--col")
    p.set_defaults(func=cmd_stats)
    p = sub.add_parser("ground")
    p.add_argument("members")
    p.add_argument("x", type=float)
    p.add_argument("z", type=float)
    p.set_defaults(func=cmd_ground)
    p = sub.add_parser("oracle-check")
    p.add_argument("members")
    p.add_argument("traces", nargs="+")
    p.add_argument("--radius", type=float, nargs=2, default=(0.323, 0.319),
                   help="front and rear rolling radius in m (stock R34 by default)")
    p.set_defaults(func=cmd_oracle_check)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
