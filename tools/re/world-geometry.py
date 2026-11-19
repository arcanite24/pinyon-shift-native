#!/usr/bin/env python3
"""Read Forza Horizon 1's Colorado scenery render models (RI-0.4).

The track archive media/tracks/colorado/bin.zip holds the world as many
small members. The meshes the game draws for scenery come from the render
models, `coloradoout.NNNNN.rmb.bin`: one model per member, made of one or
more mesh objects (a vertex block and one index list per material), then
material and shader records. The layout is our own reading of the
player's files and of frame dumps; it is written up in
docs/formats/WORLD_GEOMETRY.md. Nothing here contains game data: it reads
the player's extracted members (`extract`, which walks the whole
directory, past the wrapped 16-bit entry count).

  world-geometry.py extract <bin.zip> <members dir>
  world-geometry.py stats <members dir> [--limit N]
  world-geometry.py dump-obj <members dir> --box X0 Z0 X1 Z1 [--out FILE]
  world-geometry.py dump-obj <members dir> --members a.rmb.bin,b.rmb.bin
  world-geometry.py dump-obj <members dir> --box X0 Z0 X1 Z1 --props
  world-geometry.py pgeo <members dir> [--verbose]
  world-geometry.py match-dump <members dir> <file.fh1frame> [--box ...] [--verbose]
                               [--members @names.txt] [--pgeo <members dir>]
  world-geometry.py overlay <members dir> <file.fh1frame> <capture.ppm> [--box ...]

`dump-obj` writes every world-space model whose header box overlaps the
X/Z rectangle (one "sector") to an OBJ, by default .local/re/world/sector.obj.
Instanced (local-space) models are left out unless `--props` is given:
their placements are in the kind-7 procedural-geometry tiles
(`__R00GNNNNN.pgeo`, read by `pgeo`), which `--props` links to render
models by header box (provisional: the tiles name models by ID lists not
yet resolved), and in the `.pvsz` zone files, which are only searched
(by `match-dump --pgeo`), not placed. `--members @file` reads the
comma-separated names from a file. `match-dump` ties the selected models to the draws of a frame dump:
a mesh object is tied to a draw when the draw fetches its vertex block
(same bytes, address and size) and reads one of its index lists (same
bytes, address, count and primitive type).
"""
from __future__ import annotations

import argparse
import bisect
import collections
import glob
import os
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

RMB_GLOB = "coloradoout.*.rmb.bin"
INDEX_RESTART = 0xFFFF
LOCAL_MODEL_SIZE = 200.0   # metres; see RenderModel.placement
REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / ".local" / "re" / "world"


class FormatError(ValueError):
    pass


class Reader:
    """Big-endian cursor. Records in an rmb are packed with no alignment."""

    def __init__(self, data: bytes, at: int = 0) -> None:
        self.data = data
        self.at = at

    def need(self, size: int) -> None:
        if size < 0 or self.at + size > len(self.data):
            raise FormatError(f"read of {size} bytes at {self.at:#x} runs past the end")

    def u32(self) -> int:
        self.need(4)
        value = struct.unpack_from(">I", self.data, self.at)[0]
        self.at += 4
        return value

    def f32(self, count: int) -> tuple[float, ...]:
        self.need(4 * count)
        value = struct.unpack_from(f">{count}f", self.data, self.at)
        self.at += 4 * count
        return value

    def string(self) -> str:
        size = self.u32()
        if size > 4096:
            raise FormatError(f"string of {size} bytes at {self.at - 4:#x}")
        self.need(size)
        value = self.data[self.at:self.at + size]
        self.at += size
        return value.decode("latin-1")

    def raw(self, size: int) -> bytes:
        self.need(size)
        value = self.data[self.at:self.at + size]
        self.at += size
        return value


@dataclass
class Submesh:
    material_name: str
    material_index: int
    header: tuple[int, ...]       # 0, primitive type, material index, 1
    colour: tuple[float, ...]     # four floats, 1.0 in every member seen
    sphere: tuple[float, ...]     # centre xyz and radius, in the model's space
    unknown4: int                 # 4 in every member seen; not the primitive type
    index_size: int               # 2, or 4 when the object has over 65,535 vertices
    index_offset: int             # file offset of the first index
    indices: tuple[int, ...]
    trailer: int                  # word after the list: 5, 6, 13 or 2; meaning unknown

    @property
    def primitive(self) -> int:
        """The GPU primitive type the draw uses: 6 triangle strip (cut by
        0xFFFF), 4 triangle list."""
        return self.header[1]

    def triangles(self) -> list[tuple[int, int, int]]:
        """Triangles of a list, or of strips with the usual alternating
        winding; degenerate triangles (a repeated index) are dropped."""
        if self.primitive == 4:
            i = self.indices
            return [(i[n], i[n + 1], i[n + 2]) for n in range(0, len(i) - 2, 3)
                    if len({i[n], i[n + 1], i[n + 2]}) == 3]
        if self.primitive != 6:
            raise FormatError(f"primitive type {self.primitive}")
        out = []
        strip: list[int] = []
        for index in list(self.indices) + [INDEX_RESTART]:
            if index == INDEX_RESTART:
                for n in range(len(strip) - 2):
                    a, b, c = strip[n], strip[n + 1], strip[n + 2]
                    if a == b or b == c or a == c:
                        continue
                    out.append((a, b, c) if n % 2 == 0 else (b, a, c))
                strip = []
            else:
                strip.append(index)
        return out


@dataclass
class MeshObject:
    """One mesh of a render model: a vertex block and its index lists."""
    centre: tuple[float, ...]
    half_extent: tuple[float, ...]
    name: str
    vertex_format: int            # 3 in every member seen
    vertex_count: int
    stride: int
    vertex_offset: int            # file offset of the first vertex
    vertices: bytes
    submeshes: list[Submesh] = field(default_factory=list)

    def positions(self) -> list[tuple[float, float, float]]:
        return [struct.unpack_from(">3f", self.vertices, n * self.stride)
                for n in range(self.vertex_count)]


@dataclass
class RenderModel:
    path: str
    kind: int                     # 6 in every member seen
    box_min: tuple[float, ...]
    box_max: tuple[float, ...]
    matrix: tuple[float, ...]     # identity in every member seen
    counts: tuple[int, ...]       # 4, 4, 0, 1, then the object count
    objects: list[MeshObject] = field(default_factory=list)
    end_of_geometry: int = 0
    data: bytes = b""

    def raw_indices(self, sub: Submesh) -> bytes:
        return self.data[sub.index_offset:sub.index_offset + sub.index_size * len(sub.indices)]

    @property
    def name(self) -> str:
        return self.objects[0].name

    @property
    def placement(self) -> str:
        """'world' when the vertices are already in world space, 'local'
        when the model is drawn through a per-instance matrix. A heuristic
        (see the spec): local models sit in a small box around the origin."""
        near = all(abs(self.box_min[k]) < LOCAL_MODEL_SIZE and abs(self.box_max[k]) < LOCAL_MODEL_SIZE
                   for k in (0, 2))
        return "local" if near else "world"


def read_object(r: Reader) -> MeshObject:
    if r.u32() != 1:
        raise FormatError(f"unexpected object tag at {r.at - 4:#x}")
    centre = r.f32(3); r.u32()
    r.f32(3); r.u32()               # minus the half extent
    half_extent = r.f32(3); r.u32()
    if r.u32() != 1:
        raise FormatError(f"unexpected word before the object name at {r.at - 4:#x}")
    name = r.string()
    vertex_format = r.u32()
    vertex_count = r.u32()
    stride = r.u32()
    if r.u32() != 0 or not 12 <= stride <= 64:
        raise FormatError(f"unexpected vertex block header (stride {stride})")
    vertex_offset = r.at
    vertices = r.raw(vertex_count * stride)
    mesh = MeshObject(centre, half_extent, name, vertex_format, vertex_count, stride,
                      vertex_offset, vertices)
    if r.u32() != 1:
        raise FormatError("unexpected word before the submesh count")
    data = r.data
    for _ in range(r.u32()):
        a, b = r.u32(), r.u32()
        if (a, b) != (1, 2):
            raise FormatError(f"unexpected submesh tag {a}, {b} at {r.at - 8:#x}")
        material_name = r.string()
        header = tuple(r.u32() for _ in range(4))
        r.raw(16)
        colour = r.f32(4)
        sphere = r.f32(4)
        unknown4 = r.u32()
        if r.u32() != 0:
            raise FormatError("unexpected word after the fourth header word")
        count = r.u32()
        index_size = r.u32()
        if index_size not in (2, 4):
            raise FormatError(f"index size {index_size}")
        index_offset = r.at
        r.need(index_size * count)
        indices = struct.unpack_from(f">{count}{'H' if index_size == 2 else 'I'}", data, r.at)
        if index_size == 4:
            # Guess: a 32-bit strip would cut at all ones. The only 32-bit
            # list seen is a triangle list, so this is not exercised.
            indices = tuple(INDEX_RESTART if i == 0xFFFFFFFF else i for i in indices)
        r.at += index_size * count
        trailer = r.u32()
        if any(i >= vertex_count and i != INDEX_RESTART for i in indices):
            raise FormatError("index past the vertex block")
        mesh.submeshes.append(Submesh(material_name, header[2], header, colour, sphere,
                                      unknown4, index_size, index_offset, indices, trailer))
    return mesh


def read_rmb(path: str | os.PathLike, data: bytes | None = None) -> RenderModel:
    if data is None:
        data = Path(path).read_bytes()
    r = Reader(data)
    kind = r.u32()
    box_min = r.f32(3); r.u32()
    box_max = r.f32(3); r.u32()
    matrix = r.f32(16)
    counts = tuple(r.u32() for _ in range(5))
    if kind != 6 or counts[:4] != (4, 4, 0, 1) or not 1 <= counts[4] <= 256:
        raise FormatError(f"unexpected header kind {kind} / counts {counts}")
    model = RenderModel(str(path), kind, box_min, box_max, matrix, counts)
    for _ in range(counts[4]):
        model.objects.append(read_object(r))
    model.end_of_geometry = r.at
    model.data = data
    return model


def rmb_files(root: str) -> list[str]:
    return sorted(glob.glob(os.path.join(root, RMB_GLOB)))


def read_header_box(path: str) -> tuple[tuple[float, ...], tuple[float, ...]]:
    with open(path, "rb") as handle:
        head = handle.read(0x20)
    return struct.unpack_from(">3f", head, 4), struct.unpack_from(">3f", head, 0x14)


def overlaps(box_min, box_max, rect) -> bool:
    x0, z0, x1, z1 = rect
    return box_min[0] <= x1 and box_max[0] >= x0 and box_min[2] <= z1 and box_max[2] >= z0


def select(root: str, args) -> list[str]:
    if getattr(args, "members", None):
        names = args.members
        if names.startswith("@"):   # long selections overflow the Windows command line
            with open(names[1:], encoding="utf-8") as f:
                names = f.read()
        return [os.path.join(root, name.strip()) for name in names.split(",") if name.strip()]
    files = rmb_files(root)
    if getattr(args, "box", None):
        files = [f for f in files if overlaps(*read_header_box(f), args.box)]
    return files


# --- commands ---------------------------------------------------------------

def cmd_stats(args) -> int:
    files = rmb_files(args.root)
    if args.limit:
        files = files[::max(1, len(files) // args.limit)]
    counts = collections.Counter()
    strides = collections.Counter()
    trailers = collections.Counter()
    primitives = collections.Counter()
    objects = collections.Counter()
    placements = collections.Counter()
    lo = [1e30] * 3
    hi = [-1e30] * 3
    failures = []
    for path in files:
        try:
            model = read_rmb(path)
        except (FormatError, struct.error) as error:
            failures.append((os.path.basename(path), str(error)))
            continue
        counts["models"] += 1
        objects[len(model.objects)] += 1
        placements[model.placement] += 1
        for mesh in model.objects:
            strides[mesh.stride] += 1
            counts["vertices"] += mesh.vertex_count
            counts["submeshes"] += len(mesh.submeshes)
            for sub in mesh.submeshes:
                trailers[sub.trailer] += 1
                primitives[(sub.primitive, sub.unknown4, sub.header[0], sub.header[3],
                            sub.index_size)] += 1
                counts["triangles"] += len(sub.triangles())
        if model.placement == "world":
            for k in range(3):
                lo[k] = min(lo[k], model.box_min[k])
                hi[k] = max(hi[k], model.box_max[k])
        counts["bytes"] += len(model.data)
        counts["geometry bytes"] += model.end_of_geometry
    print(f"render models read: {counts['models']} of {len(files)}")
    print(f"objects per model: {dict(sorted(objects.items()))}")
    print(f"placement (heuristic): {dict(placements)}")
    print(f"submeshes: {counts['submeshes']}, vertices: {counts['vertices']}, "
          f"triangles: {counts['triangles']}")
    if counts["bytes"]:
        print(f"bytes up to the end of the last index list: {counts['geometry bytes']} of "
              f"{counts['bytes']} ({100 * counts['geometry bytes'] / counts['bytes']:.1f} %)")
    print("vertex strides (per object):", dict(sorted(strides.items())))
    print("word after each index list:", dict(sorted(trailers.items())))
    print("(primitive, fourth word, header words 0 and 3, index size):",
          dict(sorted(primitives.items())))
    print("world-space models span: x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f"
          % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    for name, error in failures[:20]:
        print("FAIL", name, error)
    return 1 if failures else 0


def cmd_dump_obj(args) -> int:
    files = select(args.root, args)
    out = Path(args.out) if args.out else DEFAULT_OUT / "sector.obj"
    out.parent.mkdir(parents=True, exist_ok=True)
    base = 1
    written = skipped = 0
    with out.open("w", encoding="utf-8", newline="\n") as obj:
        obj.write("# Colorado scenery render models read by tools/re/world-geometry.py\n")
        if args.box:
            obj.write("# rectangle x %.1f..%.1f z %.1f..%.1f\n"
                      % (args.box[0], args.box[2], args.box[1], args.box[3]))
        def emit(label, model, transform=None):
            nonlocal base
            for k, mesh in enumerate(model.objects):
                obj.write(f"o {label}#{k}:{mesh.name}\n")
                for v in mesh.positions():
                    x, y, z = transform(v) if transform else v
                    obj.write(f"v {x:.4f} {y:.4f} {z:.4f}\n")
                for sub in mesh.submeshes:
                    obj.write(f"g {mesh.name}:{sub.material_name}\n")
                    for a, b, c in sub.triangles():
                        obj.write(f"f {a + base} {b + base} {c + base}\n")
                base += mesh.vertex_count

        for path in files:
            model = read_rmb(path)
            if model.placement != "world" and not args.members:
                skipped += 1   # instanced: placed from the .pgeo files below
                continue
            emit(os.path.basename(path), model)
            written += 1
        placed = ambiguous = unlinked = 0
        if args.props:
            if not args.box:
                raise SystemExit("--props needs --box")
            by_box = rmb_box_index(args.root)
            cache = {}
            for pgeo_path in sorted(glob.glob(os.path.join(args.root, PGEO_GLOB))):
                tile = read_pgeo(pgeo_path)
                if tile is None or tile.kind != PGEO_MODELS:
                    continue
                for group in tile.groups:
                    inside = [inst for inst in group.instances
                              if args.box[0] <= inst.file_translation[0] <= args.box[2]
                              and args.box[1] <= inst.file_translation[2] <= args.box[3]]
                    if not inside:
                        continue
                    candidates = by_box.get(box_key(*tile.models[group.model].box), [])
                    if not candidates:
                        unlinked += len(inside)
                        continue
                    if len(candidates) > 1:
                        ambiguous += len(inside)
                    member = candidates[0]
                    if member not in cache:
                        cache[member] = read_rmb(os.path.join(args.root, member))
                    for inst in inside:
                        emit(f"{os.path.basename(pgeo_path)}:{member}", cache[member],
                             inst.to_file_frame)
                        placed += 1
    print(f"wrote {written} models, {base - 1} vertices to {out}"
          + (f"; left out {skipped} instanced (local-space) models" if skipped and not args.props
             else ""))
    if args.props:
        print(f"placed {placed} instances from .pgeo model groups ({ambiguous} with a model "
              f"chosen among several of the same box); {unlinked} instances have no render "
              f"model of their group's box")
    return 0


# --- procedural geometry (.pgeo) --------------------------------------------

PGEO_GLOB = "__R00G*.pgeo"
PGEO_MODELS = 7     # the header word that marks a placed-models tile


def align(value: int, step: int) -> int:
    return (value + step - 1) & ~(step - 1)


@dataclass
class PgeoModel:
    box: tuple[tuple[float, ...], tuple[float, ...]]
    ids: list[list[int]]        # up to three ID lists (LODs, probable)
    word: int


@dataclass
class PgeoInstance:
    rows: list[tuple[float, float, float]]   # images of the local axes, game frame
    translation: tuple[float, float, float]  # game frame (z = -file z)

    @property
    def file_translation(self) -> tuple[float, float, float]:
        x, y, z = self.translation
        return (x, y, -z)

    def to_file_frame(self, v):
        """A model-space vertex (file frame) to the world file frame: the
        stored rows act on the vertex with z negated, the result's z is
        negated back."""
        x, y, z = v[0], v[1], -v[2]
        r = self.rows
        g = [x * r[0][k] + y * r[1][k] + z * r[2][k] + self.translation[k] for k in range(3)]
        return (g[0], g[1], -g[2])


@dataclass
class PgeoGroup:
    model: int
    key: int
    counts: tuple[int, int, int]   # 96-byte instances, 40-byte and 32-byte records
    distances: tuple[float, ...]
    instances: list[PgeoInstance] = field(default_factory=list)


@dataclass
class PgeoTile:
    path: str
    kind: int
    name: str
    box: tuple[tuple[float, ...], tuple[float, ...]]
    models: list[PgeoModel] = field(default_factory=list)
    refs: list[int] = field(default_factory=list)
    groups: list[PgeoGroup] = field(default_factory=list)
    end: int = 0


def read_pgeo(path: str) -> PgeoTile | None:
    """Reads a `.pgeo` tile. The common part (header, name, model records
    and their ID lists, a reference list) is read for every kind; the
    groups and their placements only for kind 7. Layout:
    docs/formats/WORLD_GEOMETRY.md, "Placements of local-space models"."""
    data = Path(path).read_bytes()
    if data[:4] != b"OEGP":
        return None
    u32 = lambda at: struct.unpack_from(">I", data, at)[0]
    f32 = lambda at, n: struct.unpack_from(f">{n}f", data, at)
    tile = PgeoTile(path, u32(0x30), "", (f32(0x10, 3), f32(0x20, 3)))
    pos = 0x60 + (0x38 if u32(0x58) else 0)
    name_size = u32(0x3C)
    tile.name = data[pos:pos + name_size].split(b"\0")[0].decode("latin-1")
    pos += name_size
    if u32(0x44):
        pos = align(pos, 4)
        table = pos
        pos += u32(0x44) * 8
        for k in range(u32(0x44)):
            pos += u32(table + 8 * k + 4)
    if u32(0x54):
        pos = align(pos, 16)
        table = pos
        pos += u32(0x54) * 0x40
        strings = 0
        for k in range(u32(0x54)):
            record = table + 0x40 * k
            ids = []
            for slot in range(3):
                count = u32(record + 0x24 + 8 * slot)
                if count:
                    pos = align(pos, 4)
                ids.append(list(struct.unpack_from(f">{count}I", data, pos)))
                pos += 4 * count
            strings += u32(record + 0x20)
            tile.models.append(PgeoModel((f32(record, 3), f32(record + 16, 3)), ids,
                                         u32(record + 0x3C)))
        pos += strings
    if u32(0x40):
        pos = align(pos, 4)
        table = pos
        pos += u32(0x40) * 8
        for k in range(u32(0x40)):
            tile.refs.append(u32(table + 8 * k))
            if u32(table + 8 * k + 4):
                raise FormatError(f"{path}: reference with a sub-block (not seen in the disc)")
    if tile.kind != PGEO_MODELS:
        return tile
    pos = align(pos, 4)
    group_count = u32(pos + 0xC)
    pos = align(pos + 0x14, 4)
    for g in range(group_count):
        record = pos + 0x5C * g
        tile.groups.append(PgeoGroup(model=u32(record + 0x2C), key=u32(record),
                                     counts=(u32(record + 0x30), u32(record + 0x34),
                                             u32(record + 0x38)),
                                     distances=f32(record + 0x3C, 4)))
    pos += 0x5C * group_count
    for group in tile.groups:
        if group.counts[0]:
            pos = align(pos, 16)
            for k in range(group.counts[0]):
                record = pos + 0x60 * k
                group.instances.append(PgeoInstance([f32(record + 16 * r, 3) for r in range(3)],
                                                    f32(record + 0x30, 3)))
            pos += 0x60 * group.counts[0]
        if group.counts[2]:
            pos = align(pos, 4) + 0x20 * group.counts[2]
    tile.end = pos
    if pos + u32(0x50) != len(data):
        raise FormatError(f"{path}: groups end at {pos:#x}, 40-byte block {u32(0x50):#x}, "
                          f"size {len(data):#x}")
    return tile


def box_key(box_min, box_max) -> tuple[float, ...]:
    return tuple(round(v, 3) for v in (*box_min, *box_max))


def rmb_box_index(root: str) -> dict[tuple[float, ...], list[str]]:
    """Render models by header box: the provisional link from a .pgeo
    model record to a member (the ID lists are not resolved yet)."""
    index = collections.defaultdict(list)
    for path in rmb_files(root):
        index[box_key(*read_header_box(path))].append(os.path.basename(path))
    return index


def cmd_pgeo(args) -> int:
    kinds = collections.Counter()
    instances = models = 0
    for path in sorted(glob.glob(os.path.join(args.root, PGEO_GLOB))):
        tile = read_pgeo(path)
        if tile is None:
            continue
        kinds[tile.kind] += 1
        if tile.kind == PGEO_MODELS:
            models += len(tile.models)
            instances += sum(len(g.instances) for g in tile.groups)
            if args.verbose:
                print(f"{os.path.basename(path)} {tile.name}: {len(tile.groups)} groups, "
                      f"{sum(len(g.instances) for g in tile.groups)} instances")
    print("tiles by kind:", dict(sorted(kinds.items())))
    print(f"kind {PGEO_MODELS}: {models} model records, {instances} placed instances")
    return 0


def check_placements_against_pgeo(root: str, instances: dict) -> None:
    """Compares the main-view placements of a frame dump with the
    instances stored in kind-7 .pgeo tiles (translation within 1 cm,
    rotation within 0.001)."""
    stored = []
    for path in sorted(glob.glob(os.path.join(root, PGEO_GLOB))):
        tile = read_pgeo(path)
        if tile is None or tile.kind != PGEO_MODELS:
            continue
        for g, group in enumerate(tile.groups):
            for inst in group.instances:
                stored.append((os.path.basename(path), g, inst))
    found = collections.Counter()
    for (member, _), (n, rotation, t) in sorted(instances.items(), key=lambda kv: kv[1][0]):
        hit = None
        for name, g, inst in stored:
            ft = inst.file_translation
            if max(abs(ft[k] - t[k]) for k in range(3)) > 0.01:
                continue
            # file rotation = F * transpose(rows) * F, F = diag(1, 1, -1)
            flip = (1, 1, -1)
            ok = all(abs(flip[r] * flip[c] * inst.rows[c][r] - rotation[r][c]) < 1e-3
                     for r in range(3) for c in range(3))
            if ok:
                hit = (name, g)
                break
        found[member, hit is not None] += 1
        print(f"  draw {n}: {member} at {t[0]:.3f} {t[1]:.3f} {t[2]:.3f}: "
              + (f"{hit[0]} group {hit[1]}" if hit else "no stored instance"))
    matched = sum(v for (m, ok), v in found.items() if ok)
    print(f"placements found in .pgeo model tiles: {matched} of {sum(found.values())}")


PVSZ_GLOB = "__R00Z*.pvsz"


def check_placements_against_pvsz(root: str, instances: dict) -> None:
    """Looks for the main-view placements of a frame dump in the .pvsz
    zone files. A placement there is a big-endian f32 position in the game
    frame at any byte offset, followed by nine big-endian half floats that
    hold the game-frame rotation transposed with the third column negated.
    Only the position and rotation are checked: how a record is framed and
    which model it names are not decoded (provisional)."""
    flip = (1, 1, -1)
    targets = []
    for (member, _), (n, rotation, t) in sorted(instances.items(), key=lambda kv: kv[1][0]):
        game = (t[0], t[1], -t[2])
        lo, hi = (struct.pack(">f", game[0] + d)[:3] for d in (-0.01, 0.01))
        targets.append((n, member, rotation, game, {lo, hi}))
    prefixes = {}
    for k, target in enumerate(targets):
        for prefix in target[4]:
            prefixes.setdefault(prefix, []).append(k)
    hits = {}
    for path in sorted(glob.glob(os.path.join(root, PVSZ_GLOB))):
        data = Path(path).read_bytes()
        for prefix, ks in prefixes.items():
            start = data.find(prefix)
            while start >= 0 and len(ks) > len([k for k in ks if k in hits]):
                if start + 30 <= len(data):
                    pos = struct.unpack_from(">3f", data, start)
                    for k in ks:
                        n, member, rotation, game, _ = targets[k]
                        if k in hits or max(abs(pos[i] - game[i]) for i in range(3)) > 0.01:
                            continue
                        h = struct.unpack_from(">9e", data, start + 12)
                        # half matrix H[i][j] = file rotation R[j][i] * flip[j]
                        if all(abs(h[3 * i + j] - rotation[j][i] * flip[j]) < 4e-3
                               for i in range(3) for j in range(3)):
                            hits[k] = (os.path.basename(path), start)
                start = data.find(prefix, start + 1)
    for k, (n, member, rotation, game, _) in enumerate(targets):
        where = f"{hits[k][0]} +0x{hits[k][1]:x}" if k in hits else "not found"
        print(f"  draw {n}: {member}: {where}")
    print(f"placements found in .pvsz zone files: {len(hits)} of {len(targets)}")


# --- frame dumps ------------------------------------------------------------

def read_frame_dump(path: str):
    """Registers, packets and guest ranges of an FH1FRM01 frame dump. The
    layout is the SDK's (src/graphics/fh1_frame_dump.cpp): host-order
    registers, guest-order packet dwords, then (address, length, bytes)."""
    data = Path(path).read_bytes()
    if data[:8] != b"FH1FRM01":
        raise FormatError("not an FH1 frame dump")
    at = 16
    (count,) = struct.unpack_from("<I", data, at); at += 4
    registers = list(struct.unpack_from(f"<{count}I", data, at)); at += 4 * count
    (count,) = struct.unpack_from("<I", data, at); at += 4
    for _ in range(count):
        _, size = struct.unpack_from("<II", data, at); at += 8 + 4 * size
    (count,) = struct.unpack_from("<I", data, at); at += 4
    packets = struct.unpack_from(f">{count}I", data, at); at += 4 * count
    (count,) = struct.unpack_from("<I", data, at); at += 4
    blocks = []
    for _ in range(count):
        address, size = struct.unpack_from("<II", data, at); at += 8
        blocks.append((address, data[at:at + size])); at += size
    blocks.sort()
    return registers, packets, blocks


class GuestMemory:
    def __init__(self, blocks) -> None:
        self.blocks = blocks
        self.starts = [address for address, _ in blocks]

    def read(self, address: int, size: int) -> bytes | None:
        n = bisect.bisect_right(self.starts, address) - 1
        if n < 0:
            return None
        start, data = self.blocks[n]
        if address + size > start + len(data):
            return None
        return data[address - start:address - start + size]

    def compare(self, address: int, expected: bytes) -> tuple[int, bool]:
        """Compares the recorded bytes of [address, address + len) with
        `expected`: (bytes compared, all equal)."""
        end = address + len(expected)
        n = max(0, bisect.bisect_right(self.starts, address) - 1)
        compared = 0
        while n < len(self.blocks) and self.blocks[n][0] < end:
            start, data = self.blocks[n]
            lo, hi = max(start, address), min(start + len(data), end)
            if lo < hi:
                if data[lo - start:hi - start] != expected[lo - address:hi - address]:
                    return compared, False
                compared += hi - lo
            n += 1
        return compared, compared > 0

    def find_all(self, needle: bytes) -> list[int]:
        found = []
        for address, data in self.blocks:
            at = data.find(needle)
            while at >= 0:
                found.append(address + at)
                at = data.find(needle, at + 1)
        return found


ALU, FETCH = 0x4000, 0x4800
CONSTANT_BASES = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}


def walk_draws(registers, packets, memory: GuestMemory):
    """Replays the register writes of the packet stream (PM4 types 0, 1, 3;
    constants set inline or loaded from memory) and returns one record per
    draw: primitive type, index source, count and address, the vertex
    fetch constants and vertex shader constants 0-63 at that draw. Packet
    and register numbering follow the public Xenos documentation and the
    SDK's command processor."""
    regs = list(registers)
    draws = []
    i = 0
    while i < len(packets):
        header = packets[i]
        kind = header >> 30
        if kind == 0:
            base, count = header & 0x7FFF, ((header >> 16) & 0x3FFF) + 1
            single = (header >> 15) & 1
            for k in range(count):
                reg = base if single else base + k
                if reg < len(regs):
                    regs[reg] = packets[i + 1 + k]
            i += 1 + count
        elif kind == 1:
            regs[header & 0x7FF] = packets[i + 1]
            regs[(header >> 11) & 0x7FF] = packets[i + 2]
            i += 3
        elif kind == 2:
            i += 1
        else:
            opcode, count = (header >> 8) & 0x7F, ((header >> 16) & 0x3FFF) + 1
            body = packets[i + 1:i + 1 + count]
            if opcode == 0x2D:                       # SET_CONSTANT
                base = CONSTANT_BASES.get((body[0] >> 16) & 0xFF)
                if base is not None:
                    for k, value in enumerate(body[1:]):
                        regs[base + (body[0] & 0x7FF) + k] = value
            elif opcode in (0x55, 0x56):             # SET_CONSTANT2, SET_SHADER_CONSTANTS
                for k, value in enumerate(body[1:]):
                    regs[(body[0] & 0xFFFF) + k] = value
            elif opcode == 0x2F:                     # LOAD_ALU_CONSTANT
                base = CONSTANT_BASES.get((body[1] >> 16) & 0xFF)
                size = body[2] & 0xFFF
                loaded = memory.read(body[0] & 0x1FFFFFFF, 4 * size)
                if base is not None and loaded is not None:
                    for k, value in enumerate(struct.unpack(f">{size}I", loaded)):
                        regs[base + (body[1] & 0x7FF) + k] = value
            elif opcode in (0x22, 0x36):             # DRAW_INDX, DRAW_INDX_2
                words = body[1:] if opcode == 0x22 else body
                initiator = words[0]
                source = (initiator >> 6) & 3
                fetch = []
                for slot in range(96):
                    lo, hi = regs[FETCH + 2 * slot], regs[FETCH + 2 * slot + 1]
                    if lo & 3 == 3:
                        fetch.append((slot, lo & 0x1FFFFFFC, ((hi >> 2) & 0xFFFFFF) * 4))
                draws.append({
                    "primitive": initiator & 0x3F,
                    "count": initiator >> 16,
                    "indices": (words[1] & 0x1FFFFFFF) if source == 0 and len(words) > 1 else None,
                    "fetch": fetch,
                    # Vertex shader constants c128-c131 (registers 0x4200-0x420F).
                    "mvp": struct.unpack("<16f", struct.pack("<16I", *regs[ALU + 512:ALU + 528])),
                    # PA_CL_VPORT_XSCALE, XOFFSET, YSCALE, YOFFSET
                    "viewport": struct.unpack("<4f", struct.pack("<4I", *regs[0x210F:0x2113])),
                })
            i += 1 + count
    return draws


def invert4(m: tuple[float, ...]) -> list[float]:
    """Inverse of a row-major 4x4 matrix (Gauss-Jordan)."""
    a = [[float(m[4 * r + c]) for c in range(4)] + [1.0 if r == k else 0.0 for k in range(4)]
         for r in range(4)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        pivot = a[c][c]
        a[c] = [x / pivot for x in a[c]]
        for r in range(4):
            if r != c:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [a[r][4 + c] for r in range(4) for c in range(4)]


def multiply4(a, b) -> list[float]:
    return [sum(a[4 * r + k] * b[4 * k + c] for k in range(4)) for r in range(4) for c in range(4)]


def project(mvp, point, width: int, height: int):
    """Position in the presented frame and depth of a point under a draw's
    matrix (rows are the clip components). The main view's projection has
    negative x and y scales while the viewport is the usual one, so the
    scene is drawn rotated by a half turn and turned upright later in the
    frame; screen x therefore falls as clip x grows, and screen y grows
    with clip y (measured by overlaying draws on the capture: see the spec)."""
    x, y, z = point
    clip = [mvp[4 * r] * x + mvp[4 * r + 1] * y + mvp[4 * r + 2] * z + mvp[4 * r + 3]
            for r in range(4)]
    if clip[3] <= 1e-3:
        return None
    return ((1 - clip[0] / clip[3]) * 0.5 * width, (1 + clip[1] / clip[3]) * 0.5 * height,
            clip[2] / clip[3])


def solve3(a, b) -> list[float]:
    """x with a x = b for a 3x3 matrix (Cramer's rule)."""
    def det(m):
        return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
    d = det(a)
    out = []
    for c in range(3):
        m = [row[:] for row in a]
        for r in range(3):
            m[r][c] = b[r]
        out.append(det(m) / d)
    return out


def placement_xyw(scene, mvp) -> tuple[list[list[float]], list[float]]:
    """A local-space draw's placement in the file frame, from the clip x, y
    and w rows of the scene matrix and the draw's matrix only. The clip z
    row of instanced draws differs slightly from the scene matrix's (a
    different depth mapping), so the full 4x4 inverse puts instances about
    0.2-0.5 m off; x, y and w alone give the stored placement exactly.
    Returns (rotation as rows, translation)."""
    rows = (0, 1, 3)
    a = [[scene[4 * r + c] for c in range(3)] for r in rows]
    t = solve3(a, [mvp[4 * r + 3] - scene[4 * r + 3] for r in rows])
    columns = [solve3(a, [mvp[4 * r + c] for r in rows]) for c in range(3)]
    rotation = [[columns[c][r] for c in range(3)] for r in range(3)]
    return rotation, t


def placed_in_scene(scene, mvp) -> bool:
    """True when a draw's matrix is the scene matrix times a rigid
    placement: the instance is drawn in the main view."""
    placed = multiply4(invert4(scene), mvp)
    rows = [placed[0:3], placed[4:8][:3], placed[8:11]]
    return (all(abs(v) < 1e-3 for v in placed[12:15]) and abs(placed[15] - 1) < 1e-3
            and all(abs(sum(x * x for x in r) - 1) < 0.02 for r in rows))


def camera_position(mvp) -> tuple[float, float, float]:
    """The eye of a perspective matrix: the point whose clip x, y and w
    are all zero."""
    rows = [mvp[0:4], mvp[4:8], mvp[12:16]]
    a = [list(r[:3]) for r in rows]
    b = [-r[3] for r in rows]

    def det(m):
        return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
    d = det(a)
    out = []
    for c in range(3):
        m = [row[:] for row in a]
        for r in range(3):
            m[r][c] = b[r]
        out.append(det(m) / d)
    return tuple(out)


def tie_draws(args):
    """Ties draws of a frame dump to the selected render models, draw by
    draw. A draw is tied to a mesh object and one of its index lists when
    - a vertex fetch constant of the draw points at a buffer exactly the
      size of the object's vertex block, whose first vertex is the
      object's first vertex and whose every recorded byte equals the
      file's (a dump records only the pages a frame read, so a block can
      be partial), and
    - the draw's index count and primitive type are the list's, and every
      recorded byte of its index buffer equals the file's list.
    Returns the draws, {draw: (model, object index, submesh)} and counters."""
    registers, packets, blocks = read_frame_dump(args.dump)
    memory = GuestMemory(blocks)
    draws = walk_draws(registers, packets, memory)
    models = [read_rmb(path) for path in select(args.root, args)]
    by_head = collections.defaultdict(list)
    for model in models:
        for k, mesh in enumerate(model.objects):
            if mesh.vertex_count:
                by_head[(mesh.vertices[:12], len(mesh.vertices))].append((model, k))
    tied = {}
    totals = collections.Counter()
    objects_drawn = set()
    for n, draw in enumerate(draws):
        if draw["indices"] is None:
            continue
        for _, address, size in draw["fetch"]:
            head = memory.read(address, 12)
            if head is None or (head, size) not in by_head:
                continue
            for model, k in by_head[(head, size)]:
                mesh = model.objects[k]
                compared, equal = memory.compare(address, mesh.vertices)
                if not equal:
                    continue
                for sub in mesh.submeshes:
                    if sub.primitive != draw["primitive"] or len(sub.indices) != draw["count"]:
                        continue
                    seen, same = memory.compare(draw["indices"], model.raw_indices(sub))
                    if same and seen:
                        tied[n] = (model, k, sub)
                        objects_drawn.add((model.path, k))
                        totals["vertex bytes compared, summed over draws"] += compared
                        totals["index bytes compared, summed over draws"] += seen
                        break
                if n in tied:
                    break
            if n in tied:
                break
    totals["mesh objects drawn"] = len(objects_drawn)
    for model in models:
        totals[f"{model.placement}-space models selected"] += 1
    return draws, tied, totals


def scene_matrix(draws, tied):
    """The matrix most world-space draws use: the main view's projection
    times view, with the z flip between the file frame and the game's."""
    counts = collections.Counter(draws[n]["mvp"] for n, (model, _, _) in tied.items()
                                 if model.placement == "world")
    return counts.most_common(1)[0][0] if counts else None


def cmd_match_dump(args) -> int:
    draws, tied, totals = tie_draws(args)
    scene = scene_matrix(draws, tied)
    kinds = collections.Counter()
    instances = {}
    for n, (model, k, sub) in sorted(tied.items()):
        if model.placement == "world":
            view = "scene matrix" if draws[n]["mvp"] == scene else "another view"
        else:
            view = ("main view, rigid placement" if scene and placed_in_scene(scene, draws[n]["mvp"])
                    else "another view")
        kinds[(model.placement, view)] += 1
        if model.placement == "local" and scene and placed_in_scene(scene, draws[n]["mvp"]):
            rotation, t = placement_xyw(scene, draws[n]["mvp"])
            instances.setdefault((os.path.basename(model.path), tuple(round(v, 2) for v in t)),
                                 (n, rotation, t))
    if args.verbose:
        for n in sorted(tied):
            model, k, sub = tied[n]
            line = (f"draw {n}: {os.path.basename(model.path)} #{k} {model.objects[k].name} "
                    f"[{model.placement}] {sub.material_name} {len(sub.indices)} indices, ")
            if draws[n]["mvp"] == scene:
                line += "scene matrix"
            elif model.placement == "local" and scene and placed_in_scene(scene, draws[n]["mvp"]):
                _, t = placement_xyw(scene, draws[n]["mvp"])
                line += f"instance at {t[0]:.2f} {t[1]:.2f} {t[2]:.2f} (file frame)"
            else:
                line += "other matrix"
            print(line)
    indexed = sum(1 for d in draws if d["indices"] is not None)
    print(f"draws in the frame: {len(draws)}, from an index buffer: {indexed}")
    print(f"draws tied to a render model: {len(tied)}, "
          f"models: {len({os.path.basename(v[0].path) for v in tied.values()})}")
    for (placement, matrix), value in sorted(kinds.items()):
        print(f"  {placement}-space models, {matrix}: {value}")
    print(f"distinct main-view instance placements of local-space models: {len(instances)}")
    if getattr(args, "pgeo", None):
        check_placements_against_pgeo(args.pgeo, instances)
        check_placements_against_pvsz(args.pgeo, instances)
    for key, value in totals.items():
        print(f"{key}: {value}")
    if scene:
        print("scene matrix:", " ".join(f"{v!r}" for v in scene))
        print("camera position (file frame): %.2f %.2f %.2f" % camera_position(scene))
    return 0


def cmd_overlay(args) -> int:
    """Draws the edges of every tied main-view draw, from the file's
    triangles and the draw's own matrix, over a capture of the same frame."""
    draws, tied, _ = tie_draws(args)
    scene = scene_matrix(draws, tied)
    data = Path(args.image).read_bytes()
    magic, size, depth, pixels = data.split(b"\n", 3)
    width, height = map(int, size.split())
    image = bytearray(pixels)
    colours = {"world": b"\x00\xff\x00", "local": b"\xff\x00\xff"}

    def line(a, b, colour):
        steps = int(max(abs(a[0] - b[0]), abs(a[1] - b[1]))) + 1
        if steps > 4 * (width + height):
            return
        for s in range(steps + 1):
            t = s / steps
            x, y = int(a[0] + (b[0] - a[0]) * t), int(a[1] + (b[1] - a[1]) * t)
            if 0 <= x < width and 0 <= y < height:
                image[3 * (y * width + x):3 * (y * width + x) + 3] = colour

    drawn = 0
    for n, (model, k, sub) in sorted(tied.items()):
        mvp = draws[n]["mvp"]
        if model.placement == "world" and mvp != scene:
            continue   # reflection, cube map or shadow views
        if model.placement == "local" and not (scene and placed_in_scene(scene, mvp)):
            continue   # drawn into another view
        points = [project(mvp, p, width, height) for p in model.objects[k].positions()]
        for a, b, c in sub.triangles():
            pa, pb, pc = points[a], points[b], points[c]
            if pa and pb and pc and all(0 <= p[2] <= 1 for p in (pa, pb, pc)):
                line(pa, pb, colours[model.placement])
                line(pb, pc, colours[model.placement])
                line(pc, pa, colours[model.placement])
        drawn += 1
    out = Path(args.out) if args.out else DEFAULT_OUT / "overlay.ppm"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(magic + b"\n" + size + b"\n" + depth + b"\n" + bytes(image))
    print(f"drew the edges of {drawn} tied draws over {args.image} to {out}")
    return 0


def cmd_extract(args) -> int:
    """Extract every distinct member of bin.zip, past the 16-bit count.

    bin.zip's end record counts its directory entries in 16 bits, and the
    count wraps (230,057 entries hold 43,546 names that differ only in
    case), so readers that trust the count stop early. Python's zipfile
    walks the directory by its byte size instead. This copies one payload
    per name (all copies share CRC and size) into a temporary archive
    whose directory points straight at the payloads, then extracts that
    with the project's own LZX extractor. The temporary archive is a copy
    of the player's data and is deleted afterwards.
    """
    import subprocess
    import tempfile
    import zipfile

    source = Path(args.archive)
    out = Path(args.out)
    tool = Path(args.extractor)
    if not tool.is_file():
        print(f"no extractor at {tool}; build pinyon_shift_fh1_archive_extract first", file=sys.stderr)
        return 1
    out.mkdir(parents=True, exist_ok=True)
    entries, seen = [], set()
    with zipfile.ZipFile(source) as archive, source.open("rb") as raw, \
            tempfile.TemporaryDirectory(dir=out.parent) as temp:
        slim = Path(temp) / "distinct.zip"
        with slim.open("wb") as dst:
            for info in archive.infolist():
                key = info.filename.lower()
                if key in seen:
                    continue
                seen.add(key)
                raw.seek(info.header_offset)
                local = raw.read(30)
                payload_at = info.header_offset
                if local[:4] == b"PK\x03\x04":
                    name_len, extra_len = struct.unpack_from("<HH", local, 26)
                    payload_at += 30 + name_len + extra_len
                raw.seek(payload_at)
                at = dst.tell()
                dst.write(raw.read(info.compress_size))
                entries.append((info, at))
            if len(entries) > 0xFFFF:
                print(f"{len(entries)} distinct names do not fit one archive", file=sys.stderr)
                return 1
            directory = dst.tell()
            for info, at in entries:
                name = info.filename.encode()
                dst.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, info.compress_type,
                                      0, 0, info.CRC, info.compress_size, info.file_size, len(name),
                                      0, 0, 0, 0, 0, at) + name)
            size = dst.tell() - directory
            dst.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries),
                                  size, directory, 0))
        print(f"{len(archive.infolist())} directory entries, {len(entries)} distinct names")
        result = subprocess.run([str(tool), "--archive-all", str(slim), str(out)])
    return result.returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    extract = commands.add_parser("extract")
    extract.add_argument("archive", help="media/tracks/colorado/bin.zip")
    extract.add_argument("out", help="directory for the members")
    extract.add_argument("--extractor", default=str(
        REPO / "out" / "build" / "win-amd64-release" / "pinyon_shift_fh1_archive_extract.exe"))
    extract.set_defaults(run=cmd_extract)

    def add_selection(sub):
        sub.add_argument("root", help="directory of extracted bin.zip members")
        sub.add_argument("--box", type=float, nargs=4, metavar=("X0", "Z0", "X1", "Z1"))
        sub.add_argument("--members", help="comma-separated member names, or @file holding them")

    stats = commands.add_parser("stats")
    stats.add_argument("root")
    stats.add_argument("--limit", type=int, default=0, help="sample about N members")
    stats.set_defaults(run=cmd_stats)
    dump = commands.add_parser("dump-obj")
    add_selection(dump)
    dump.add_argument("--out")
    dump.add_argument("--props", action="store_true",
                      help="also place local-space models from the kind-7 .pgeo tiles "
                           "(instances whose origin lies in --box)")
    dump.set_defaults(run=cmd_dump_obj)
    pgeo = commands.add_parser("pgeo")
    pgeo.add_argument("root")
    pgeo.add_argument("--verbose", action="store_true")
    pgeo.set_defaults(run=cmd_pgeo)
    match = commands.add_parser("match-dump")
    add_selection(match)
    match.add_argument("dump")
    match.add_argument("--verbose", action="store_true", help="list every tied draw")
    match.add_argument("--pgeo", metavar="DIR",
                       help="check every main-view placement against the .pgeo and .pvsz files in DIR")
    match.set_defaults(run=cmd_match_dump)
    overlay = commands.add_parser("overlay")
    add_selection(overlay)
    overlay.add_argument("dump")
    overlay.add_argument("image", help="a binary PPM capture of the same frame")
    overlay.add_argument("--out")
    overlay.set_defaults(run=cmd_overlay)
    args = parser.parse_args(argv)
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
