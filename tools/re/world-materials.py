#!/usr/bin/env python3
"""Read the materials and textures of Forza Horizon 1's Colorado scenery.

Companion of tools/re/world-geometry.py, which reads the render models'
meshes. This one reads what follows the meshes in a render model (the
material records and the shader list), the per-model texture lists and
the texture records of the ribbon's `.pvs` file, and the two texture
containers of the track archive (`_0x<id>.bix` with its `_B.bix` twin,
and the CAFF `_0x<id>.bin`). The layout is our own reading of the
player's files and of the RI-0.4 frame dump; it is written up in
docs/formats/WORLD_MATERIALS.md. Nothing here contains game data: it
reads the player's extracted bin.zip members (world-geometry.py extract).

  world-materials.py census <members dir> [--pvs FILE]
  world-materials.py model <members dir> <member number> [--pvs FILE]
  world-materials.py texture <members dir> <texture id> --out FILE.png [--level N]
  world-materials.py check-dump <members dir> <file.fh1frame> --members @names.txt [--pvs FILE]

`census` parses every render model's materials, resolves every texture
slot through the `.pvs` lists to a file, checks every material's vertex
stride against the vertex shaders of its effect, checks every texture
file's size against the console's tiled layout and counts formats.
`model` prints one model's draw recipe: per submesh the effect, the
texture file in each slot and the UV dequantisation constant. `texture`
untiles and decodes one texture to a PNG. `check-dump` compares all of
this with the draws of a frame dump that world-geometry.py ties to the
selected models: the vertex fetches each draw's shader makes, the UV
constant it is given and the textures it samples.
"""
from __future__ import annotations

import argparse
import collections
import glob
import hashlib
import importlib.util
import os
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("world_geometry", HERE / "world-geometry.py")
wg = importlib.util.module_from_spec(_spec)
sys.modules["world_geometry"] = wg
_spec.loader.exec_module(wg)

FormatError = wg.FormatError
DEFAULT_PVS = wg.DEFAULT_PVS

# ---------------------------------------------------------------- materials


@dataclass
class Material:
    shader: int                       # index into RenderModel shader list
    vs_constants: tuple[tuple[float, ...], ...]
    ps_constants: tuple[tuple[float, ...], ...]
    textures: tuple[int, ...]         # per fetch slot: index into the model's .pvs list, -1/-2 none


@dataclass
class Materials:
    materials: list[Material]
    shaders: list[str]
    end: int                          # file offset after the shader list
    uv_constants: list[tuple[float, ...]] = field(default_factory=list)


def read_materials(model) -> Materials:
    """The section after the last mesh object: four words 1, then the
    material count and each material (3, effect index, 0; a group of VS
    vec4 constants; a group of PS vec4 constants; a group of per-slot
    texture list indices), then the effect path list."""
    r = wg.Reader(model.data, model.end_of_geometry)
    if [r.u32() for _ in range(4)] != [1, 1, 1, 1]:
        raise FormatError(f"{model.path}: unexpected material section start")
    materials = []
    for _ in range(r.u32()):
        tag, shader, zero = r.u32(), r.u32(), r.u32()
        if tag != 3 or zero != 0:
            raise FormatError(f"{model.path}: unexpected material tag {tag}, {zero}")
        groups = []
        for _ in range(2):
            if r.u32() != 1:
                raise FormatError(f"{model.path}: unexpected constant group tag")
            n = r.u32()
            groups.append(tuple(r.f32(4) for _ in range(n)))
        if r.u32() != 1:
            raise FormatError(f"{model.path}: unexpected texture group tag")
        n = r.u32()
        r.need(4 * n)
        textures = struct.unpack_from(f">{n}i", r.data, r.at)
        r.at += 4 * n
        materials.append(Material(shader, groups[0], groups[1], textures))
    if r.u32() != 1:
        raise FormatError(f"{model.path}: unexpected shader list tag")
    shaders = [r.raw(r.u32()).rstrip(b"\0").decode("latin-1") for _ in range(r.u32())]
    if any(m.shader >= len(shaders) for m in materials):
        raise FormatError(f"{model.path}: effect index past the shader list")
    out = Materials(materials, shaders, r.at)
    out.uv_constants = uv_constants(model, out.end)
    return out


def uv_constants(model, start: int) -> list[tuple[float, ...]]:
    """The UV dequantisation constant (vertex shader c0, offset u, offset
    v, scale u, scale v) of every submesh, from the command section after
    the shader list: register writes of 16 or 32 dwords (4 or 8 constants) at c0 whose
    second constant is (1, 1, 1, 1). They come one per submesh, in file
    order, repeated once per pass; returns the first pass."""
    data = model.data
    found = []
    for at in range(start, len(data) - 36):
        if data[at] == 0 and data[at + 2] == 0x40 and data[at + 3] == 0 and data[at + 1] in (0x0F, 0x1F):
            if struct.unpack_from(">4f", data, at + 20) == (1.0, 1.0, 1.0, 1.0):
                found.append(struct.unpack_from(">4f", data, at + 4))
    submeshes = sum(len(o.submeshes) for o in model.objects)
    return found[:submeshes] if len(found) >= submeshes else []


def effect_name(path: str) -> str:
    """`shaders\\track\\h_road_diff3_noise_ao_lm.fx` -> H_ROAD_DIFF3_NOISE_AO_LM."""
    return path.replace("/", "\\").split("\\")[-1].rsplit(".", 1)[0].upper()


def effect_family(name: str) -> str:
    parts = name.split("_")
    return "_".join(parts[:2]) if parts[0] in ("H", "ANIM", "TREE") and len(parts) > 1 else parts[0]

# ---------------------------------------------------------------- .pvs lists


@dataclass
class TextureRecord:
    texture_id: int                   # names _0x%08X.bix / _B.bix / .bin
    index: int
    flag: int
    type: int                         # 4 .bix pair, 0 CAFF .bin, 1 no file


@dataclass
class TextureTables:
    records: dict[int, TextureRecord]
    lists: list[tuple[int, ...]]      # lists[member - 1]: record indices


RECORD_COUNT_AT = 0xAF


def read_texture_tables(path) -> TextureTables:
    """The 28-byte texture records near the start of the `.pvs` file, and
    the per-render-model lists that follow the object table: a u32 list
    count and three words, then per list a u32 count of effect indices
    and those indices, 15 floats (an oriented box) and a u32 count of
    texture record indices and those indices. List j belongs to render
    model member j + 1."""
    data = Path(path).read_bytes()
    (count,) = struct.unpack_from(">I", data, RECORD_COUNT_AT)
    records = {}
    for i in range(count):
        at = RECORD_COUNT_AT + 4 + 28 * i
        texture_id, index, a, b = struct.unpack_from(">IIff", data, at)
        if (a, b) != (1.0, 1.0) or any(data[at + 16:at + 26]):
            raise FormatError(f"{path}: unexpected texture record {i}")
        records[index] = TextureRecord(texture_id, index, data[at + 26], data[at + 27])
    first = data.find(b"shaders\\")
    at = first - 8
    (n,) = struct.unpack_from(">I", data, at); at += 4
    for _ in range(n):
        (size,) = struct.unpack_from(">I", data, at); at += 4 + size
    (objects,) = struct.unpack_from(">I", data, at); at += 4 + 18 * objects
    (n,) = struct.unpack_from(">I", data, at); at += 16
    lists = []
    for _ in range(n):
        (k,) = struct.unpack_from(">I", data, at); at += 4 + 4 * k + 60
        (t,) = struct.unpack_from(">I", data, at); at += 4
        lists.append(struct.unpack_from(f">{t}I", data, at)); at += 4 * t
    return TextureTables(records, lists)


def slot_texture(tables: TextureTables, member: int, value: int) -> TextureRecord | None:
    if value < 0 or member - 1 >= len(tables.lists):
        return None
    lst = tables.lists[member - 1]
    return tables.records.get(lst[value]) if value < len(lst) else None


def member_number(path: str) -> int:
    return int(os.path.basename(path).split(".")[1])

# ---------------------------------------------------------------- textures

BLOCK = {18: (8, "DXT1"), 19: (16, "DXT3"), 20: (16, "DXT5"), 49: (16, "DXN")}
ENDIAN_MASKS = (0, 1, 3, 2)


@dataclass
class Texture:
    texture_id: int
    container: str                    # "bix" or "caff"
    width: int
    height: int
    levels: int
    format_word: int
    base: bytes                       # base level storage
    chain: bytes                      # levels 1 and up, then the tail

    @property
    def format(self) -> int:
        return self.format_word & 0x3F

    @property
    def endian(self) -> int:
        return (self.format_word >> 6) & 3

    @property
    def tiled(self) -> bool:
        return bool((self.format_word >> 8) & 1)

    @property
    def gamma(self) -> bool:
        """Sign fields X, Y and Z all 3 (gamma): sample as sRGB."""
        return all((self.format_word >> s) & 3 == 3 for s in (9, 11, 13))


def read_texture(root: str, texture_id: int) -> Texture:
    stem = os.path.join(root, "_0x%08X" % texture_id)
    if os.path.exists(stem + ".bix"):
        chain = Path(stem + ".bix").read_bytes()
        magic, w, h, levels, fmt, total, base = struct.unpack_from(">4s6I", chain)
        if magic != b"BIX1":
            raise FormatError(f"{stem}.bix: not BIX1")
        base_bytes = Path(stem + "_B.bix").read_bytes()
        if len(base_bytes) != base or len(chain) - 28 != total - base:
            raise FormatError(f"{stem}.bix: sizes do not match the header")
        return Texture(texture_id, "bix", w, h, levels, fmt, base_bytes, chain[28:])
    if os.path.exists(stem + ".bin"):
        data = Path(stem + ".bin").read_bytes()
        if not data.startswith(b"CAFF") or b"texture" not in data[0x190:0x2B0]:
            raise FormatError(f"{stem}.bin: not a CAFF texture")
        (fmt,) = struct.unpack_from(">I", data, 0x1A8)
        w, h = struct.unpack_from(">HH", data, 0x1B4)
        levels = data[0x1C0]
        tex = Texture(texture_id, "caff", w, h, levels, fmt, b"", b"")
        levels_, base_size, total = layout(tex)
        payload = data[0x2B0:]
        if len(payload) != total:
            raise FormatError(f"{stem}.bin: payload {len(payload)} != layout {total}")
        tex.base, tex.chain = payload[:base_size], payload[base_size:]
        return tex
    raise FileNotFoundError(stem)


def _log2_ceil(v: int) -> int:
    return max(v - 1, 0).bit_length()


def _align(v: int, a: int) -> int:
    return (v + a - 1) // a * a


@dataclass
class Level:
    width: int                        # blocks
    height: int
    start: int                        # byte offset in base + chain
    pitch: int                        # storage pitch in blocks
    x: int = 0                        # block offset in a packed tail
    y: int = 0


def layout(tex: Texture, base_size: int | None = None) -> tuple[list[Level], int, int]:
    """Every level's storage, the base level's size and the total size.

    The `.xds` rules of docs/formats/CARS.md with one change at the end:
    each level up to the packed tail gets a region of 32 x 32-block tiles
    rounded to 4 KiB, and the levels whose shorter side is 16 texels or
    less share the tail region; but the last region (the tail, or the
    last level when the chain stops before the tail) is only as large as
    the tiled bytes its levels touch, rounded to 4 KiB. A `.bix` pair gives the base size itself
    (the `_B.bix` file): a base level under 32 blocks tall can be smaller
    than a full tile row."""
    if tex.format not in BLOCK:
        raise FormatError(f"texture format {tex.format}")
    if not 1 <= tex.levels <= 14:
        raise FormatError(f"{tex.levels} levels")
    nbytes = BLOCK[tex.format][0]
    log2 = nbytes.bit_length() - 1
    w, h = tex.width, tex.height
    lw, lh = _log2_ceil(w), _log2_ceil(h)
    tail = max(min(lw, lh) - 4, 0)
    pw, ph = 1 << lw, 1 << lh

    def full(level):
        if level == 0 and tail > 0:
            pitch = _align(max((w + 3) // 4, 1), 32)
            rows = _align((h + 3) // 4, 32)
        else:
            pitch = _align(max((pw >> level) // 4, 1), 32)
            rows = _align(max((ph >> level) // 4, 1), 32)
        return pitch, _align(pitch * rows << log2, 4096)

    out = []
    for level in range(tex.levels):
        x = y = 0
        if level >= tail:
            t = level - tail
            if t < 3:
                x, y = (0, 16 >> t) if lw > lh else (16 >> t, 0)
            else:
                off = (1 << (max(lw, lh) - tail)) >> (t - 2)
                x, y = (off, 0) if lw > lh else (0, off)
            x, y = x // 4, y // 4
        out.append(Level((max(w >> level, 1) + 3) // 4, (max(h >> level, 1) + 3) // 4,
                         0, full(min(level, tail))[0], x, y))
    at = 0
    sizes = {}
    last = min(tex.levels - 1, tail)
    for stored in range(0, last + 1):
        _, size = full(stored)
        if stored == 0 and base_size is not None:
            size = base_size
        elif stored == last > 0:
            end = max(tiled_offset(lv.x + bx, lv.y + by, lv.pitch, log2)
                      for lv in out[stored:] for by in range(lv.height) for bx in range(lv.width))
            size = _align(end + nbytes, 4096)
        sizes[stored] = (at, size)
        at += size
    for level, lv in enumerate(out):
        lv.start = sizes[min(level, tail)][0]
    return out, sizes[0][1], at


def tiled_offset(x: int, y: int, pitch: int, log2: int) -> int:
    """Byte offset of block (x, y) in a tiled surface `pitch` blocks wide:
    the console's standard 2D tiling, as open emulators document it."""
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def level_blocks(tex: Texture, level: int) -> tuple[Level, list[bytes]]:
    """One level's blocks, row by row, untiled and in host (D3D) byte order."""
    levels, _, _ = layout(tex, len(tex.base) if tex.container == "bix" else None)
    lv = levels[level]
    data = tex.base + tex.chain
    nbytes = BLOCK[tex.format][0]
    log2 = nbytes.bit_length() - 1
    mask = ENDIAN_MASKS[tex.endian]
    blocks = []
    for y in range(lv.height):
        for x in range(lv.width):
            at = lv.start + (tiled_offset(x + lv.x, y + lv.y, lv.pitch, log2) if tex.tiled
                             else ((y + lv.y) * lv.pitch + x + lv.x) * nbytes)
            b = data[at:at + nbytes]
            blocks.append(bytes(b[i ^ mask] for i in range(nbytes)) if mask else b)
    return lv, blocks


def _rgb565(c):
    r, g, b = c >> 11, (c >> 5) & 0x3F, c & 0x1F
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _colour(block, four):
    c0, c1, bits = struct.unpack_from("<HHI", block)
    a, b = _rgb565(c0), _rgb565(c1)
    if c0 > c1 or four:
        pal = [a + (255,), b + (255,), tuple((2 * p + q + 1) // 3 for p, q in zip(a, b)) + (255,),
               tuple((p + 2 * q + 1) // 3 for p, q in zip(a, b)) + (255,)]
    else:
        pal = [a + (255,), b + (255,), tuple((p + q + 1) // 2 for p, q in zip(a, b)) + (255,),
               (0, 0, 0, 0)]
    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]


def _alpha(block):
    a0, a1 = block[0], block[1]
    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + i * a1 + 3) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + i * a1 + 2) // 5 for i in range(1, 5)] + [0, 255]
    bits = int.from_bytes(block[2:8], "little")
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def decode_block(fmt: int, block: bytes):
    if fmt == 18:
        return _colour(block, False)
    if fmt == 20:
        a = _alpha(block[:8])
        return [(r, g, b, a[i]) for i, (r, g, b, _) in enumerate(_colour(block[8:], True))]
    if fmt == 19:
        a = [((int.from_bytes(block[:8], "little") >> (4 * i)) & 15) * 17 for i in range(16)]
        return [(r, g, b, a[i]) for i, (r, g, b, _) in enumerate(_colour(block[8:], True))]
    if fmt == 49:
        x, y = _alpha(block[:8]), _alpha(block[8:])
        out = []
        for i in range(16):
            nx, ny = x[i] / 127.5 - 1, y[i] / 127.5 - 1
            nz = max(0.0, 1 - nx * nx - ny * ny) ** 0.5
            out.append((x[i], y[i], int(round((nz + 1) * 127.5)), 255))
        return out
    raise FormatError(f"texture format {fmt}")


def decode_level(tex: Texture, level: int = 0) -> tuple[int, int, bytearray]:
    lv, blocks = level_blocks(tex, level)
    w, h = max(tex.width >> level, 1), max(tex.height >> level, 1)
    pixels = bytearray(4 * w * h)
    for n, block in enumerate(blocks):
        bx, by = n % lv.width, n // lv.width
        for i, p in enumerate(decode_block(tex.format, block)):
            x, y = 4 * bx + i % 4, 4 * by + i // 4
            if x < w and y < h:
                pixels[4 * (y * w + x):4 * (y * w + x) + 4] = bytes(p)
    return w, h, pixels


def write_png(path: str, w: int, h: int, rgba: bytes) -> None:
    raw = b"".join(b"\0" + bytes(rgba[4 * w * y:4 * w * (y + 1)]) for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

# ---------------------------------------------------------------- effects

# Vertex element sizes by declaration usage (D3D numbering): position
# three f32; normal and tangent 2_10_10_10 signed; texcoord 16_16
# unsigned normalized; colour 8_8_8_8 unsigned normalized.
USAGE_SIZE = {0: 12, 3: 4, 5: 4, 6: 4, 10: 4}
USAGE_NAME = {0: "POSITION", 3: "NORMAL", 5: "TEXCOORD", 6: "TANGENT", 10: "COLOR"}


def packed(declaration) -> list[tuple[int, int, int]]:
    """(offset, usage, usage index) of a declaration packed in its order."""
    out, at = [], 0
    for usage, index in declaration:
        out.append((at, usage, index))
        at += USAGE_SIZE.get(usage, 4)
    return out


FETCH_FORMAT = {0: "32_32_32_FLOAT", 3: "2_10_10_10", 5: "16_16", 6: "2_10_10_10", 10: "8_8_8_8"}


def packed_size(declaration) -> int:
    return sum(USAGE_SIZE.get(u, 4) for u, _ in declaration)


VFETCH_FORMATS = {6: "8_8_8_8", 7: "2_10_10_10", 25: "16_16", 26: "16_16_16_16",
                  31: "16_16_FLOAT", 37: "32_32_FLOAT", 57: "32_32_32_FLOAT"}
USAGES = {0: "POSITION", 3: "NORMAL", 5: "TEXCOORD", 6: "TANGENT", 10: "COLOR"}


def fetches(code: bytes):
    """Vertex and texture fetches of a shader's microcode, from its control
    flow exec clauses: ('v', stride, offset, format, signed) and ('t', slot)."""
    w = struct.unpack(f">{len(code) // 4}I", code[:len(code) // 4 * 4])
    cf = []
    for i in range(len(w) // 3):
        a, b, c = w[3 * i:3 * i + 3]
        cf += [a | (b & 0xFFFF) << 32, b >> 16 | c << 16]
    out, seen = [], set()
    for ins in cf:
        if (ins >> 44) & 0xF not in (1, 2, 3, 4, 5, 6):
            continue
        addr, count, seq = ins & 0xFFF, (ins >> 12) & 7, (ins >> 16) & 0xFFF
        for k in range(count):
            ia = addr + k
            if not (seq >> (2 * k)) & 1 or ia in seen or 3 * ia + 2 >= len(w):
                continue
            seen.add(ia)
            a, b, c = w[3 * ia:3 * ia + 3]
            if a & 0x1F == 0:
                out.append(("v", (c & 0xFF) * 4, ((c >> 8) & 0x7FFFFF) * 4,
                            VFETCH_FORMATS.get((b >> 16) & 0x3F, (b >> 16) & 0x3F), (b >> 12) & 1))
            elif a & 0x1F == 1:
                out.append(("t", (a >> 20) & 0x1F))
    return out


def effect_shaders(path: str):
    """(stage, microcode, vertex elements, sampler slots) of every shader
    container in an `.fxobj`."""
    data = Path(path).read_bytes()
    out, at = [], 0
    while at + 36 <= len(data):
        flags, vsz, psz, _, cto, _, so, f1c, f20 = struct.unpack_from(">9I", data, at)
        size = vsz + psz
        if not (flags & 0xFFFFFF00 == 0x102A1100 and 36 <= size <= len(data) - at and cto
                and f1c == 0 and f20 == 0 and so + 24 <= vsz):
            at += 4
            continue
        po, ss = struct.unpack_from(">2I", data, at + so)
        if not (ss and ss % 12 == 0 and po <= psz and ss <= psz - po):
            at += 4
            continue
        stage = "vs" if flags & 1 else "ps"
        code = data[at + vsz + po:at + vsz + po + ss]
        elements = ()
        if stage == "vs":
            v = struct.unpack_from(">9I", data, at + so)
            words = struct.unpack_from(f">{v[6] + v[7]}I", data, at + so + 36)
            elements = tuple((x & 0xFFF, (x >> 12) & 0xF, (x >> 16) & 0xF) for x in words[v[6]:])
        samplers = {}
        try:
            base = cto + 4
            _, _, _, count, info = struct.unpack_from(">5I", data, at + base)
            for i in range(count):
                no, rs, ri, rc = struct.unpack_from(">IHHH", data, at + base + info + 20 * i)
                if rs == 3:
                    name = data[at + base + no:data.index(b"\0", at + base + no)].decode("latin-1")
                    samplers[ri] = name
        except (struct.error, ValueError):
            pass
        out.append((stage, code, elements, samplers))
        at += size
    return out


class Effects:
    def __init__(self, root: str):
        self.dir = os.path.join(root, "shaders", "track")
        self.cache = {}

    def get(self, name: str):
        if name not in self.cache:
            path = os.path.join(self.dir, name + ".fxobj")
            match = [p for p in glob.glob(os.path.join(self.dir, "*.fxobj"))
                     if os.path.basename(p).upper() == name + ".FXOBJ"]
            self.cache[name] = effect_shaders(match[0]) if match else None
        return self.cache[name]

    def declarations(self, name: str) -> set[tuple[tuple[int, int], ...]]:
        """The (usage, usage index) lists of the effect's vertex shaders,
        position-only (depth and shadow) shaders left out. The `.fxobj`
        microcode carries no strides: the vertex fetches are patched with
        the buffer's stride when the shader is bound."""
        return {tuple((u, i) for _, u, i in elements) for stage, _, elements, _ in self.get(name) or []
                if stage == "vs" and len(elements) > 1}

    def samplers(self, name: str) -> dict[int, str]:
        out = {}
        for stage, _, _, s in self.get(name) or []:
            if stage == "ps":
                out.update(s)
        return out

# ---------------------------------------------------------------- commands


def unit_uvs(o, s, at: int, c0) -> bool:
    """True when the texcoord at byte `at`, dequantised with c0, lies in
    [0, 1] for (up to 500 of) the submesh's vertices."""
    for i in sorted({i for i in s.indices if i != wg.INDEX_RESTART})[:500]:
        a, b = struct.unpack_from(">HH", o.vertices, i * o.stride + at)
        if not (-0.01 <= c0[0] + c0[2] * a / 65535 <= 1.01 and -0.01 <= c0[1] + c0[3] * b / 65535 <= 1.01):
            return False
    return True


def cmd_census(args) -> int:
    tables = read_texture_tables(args.pvs)
    effects = Effects(args.root)
    c = collections.Counter()
    strides = collections.Counter()
    families = collections.Counter()
    slot_types = collections.Counter()
    missing = collections.Counter()
    lm_uv = collections.Counter()
    roles = collections.Counter()
    zero = {}
    for path in wg.rmb_files(args.root):
        c["render models"] += 1
        try:
            model = wg.read_rmb(path)
            mats = read_materials(model)
        except (FormatError, struct.error) as e:
            c["models whose materials do not parse"] += 1
            if c["models whose materials do not parse"] <= 5:
                print("not parsed:", os.path.basename(path), e)
            continue
        c["models whose materials parse"] += 1
        c["materials"] += len(mats.materials)
        nsub = sum(len(o.submeshes) for o in model.objects)
        c["submeshes"] += nsub
        c["models with one UV constant per submesh" if mats.uv_constants else
          "models without a UV constant per submesh"] += 1
        member = member_number(path)
        g = -1
        for o in model.objects:
            for s in o.submeshes:
                g += 1
                if s.material_index >= len(mats.materials):
                    c[f"submeshes with no material (index {s.material_index:#x}, {s.material_name!r})"] += 1
                    continue
                mat = mats.materials[s.material_index]
                name = effect_name(mats.shaders[mat.shader])
                strides[(o.stride, effect_family(name))] += 1
                decls = effects.declarations(name)
                sizes = sorted(packed_size(d) for d in decls)
                if effects.get(name) is None:
                    c["submeshes whose effect has no .fxobj"] += 1
                elif o.stride in sizes:
                    c["submeshes: an effect vertex declaration packs to exactly the stride"] += 1
                elif sizes and sizes[-1] < o.stride:
                    c["submeshes: the effect's declarations pack to less than the stride"] += 1
                    missing[(o.stride, name, tuple(sorted(decls)))] += 1
                else:
                    c["submeshes: no declaration fits"] += 1
                    missing[(o.stride, name, tuple(sorted(decls)))] += 1
                if name.endswith("_LM") and mats.uv_constants and o.stride in sizes:
                    decl = next(d for d in decls if packed_size(d) == o.stride)
                    last = [at for at, u, _ in packed(decl) if u == 5][-1]
                    lm_uv[unit_uvs(o, s, last, mats.uv_constants[g])] += 1
        for mat in mats.materials:
            name = effect_name(mats.shaders[mat.shader])
            families[effect_family(name)] += 1
            for slot, value in enumerate(mat.textures):
                if value < 0:
                    slot_types[f"list value {value}"] += 1
                    continue
                rec = slot_texture(tables, member, value)
                if rec is None:
                    slot_types["index past the model's list"] += 1
                    continue
                stem = os.path.join(args.root, "_0x%08X" % rec.texture_id)
                has = {4: os.path.exists(stem + ".bix") and os.path.exists(stem + "_B.bix"),
                       0: os.path.exists(stem + ".bin")}.get(rec.type, False)
                slot_types[(f"record type {rec.type}", "file found" if has else "no file")] += 1
                if has:
                    if rec.texture_id not in zero:
                        try:
                            zero[rec.texture_id] = not any(read_texture(args.root, rec.texture_id).base)
                        except FormatError:
                            zero[rec.texture_id] = None
                    role = effects.samplers(name).get(slot, "unnamed").replace("Sampler", "").strip("_")
                    if zero[rec.texture_id] is not None:
                        roles[(role, "base level all zero" if zero[rec.texture_id] else "data")] += 1
    for k, v in c.items():
        print(f"{k}: {v}")
    print("texture slots:")
    for k, v in sorted(slot_types.items(), key=str):
        print(f"  {k}: {v}")
    print("texture slot uses with a file, by sampler:")
    for k, v in sorted(roles.items()):
        print(f"  {k}: {v}")
    print("effect families (materials):")
    for k, v in families.most_common():
        print(f"  {k}: {v}")
    print("stride x family (submeshes):")
    for k, v in sorted(strides.items(), key=str):
        print(f"  {k}: {v}")
    print("lightmap effects (*_LM): last TEXCOORD after dequantisation (500 vertices a submesh):")
    for k, v in lm_uv.items():
        print(f"  {'inside' if k else 'not inside'} [0, 1] (0.01 margin): {v}")
    for k, v in missing.most_common(40):
        print(f"  declaration does not pack to the stride: {k}: {v}")
    # texture files
    t = collections.Counter()
    for path in sorted(glob.glob(os.path.join(args.root, "_0x*.bix")) +
                       glob.glob(os.path.join(args.root, "_0x*.bin"))):
        name = os.path.basename(path)
        if name.endswith("_B.bix"):
            continue
        try:
            tex = read_texture(args.root, int(name[3:11], 16))
            _, base, total = layout(tex, len(tex.base) if tex.container == "bix" else None)
        except (FormatError, FileNotFoundError) as e:
            t[("not read", type(e).__name__)] += 1
            continue
        t[(tex.container, "files")] += 1
        t[(tex.container, BLOCK[tex.format][1], "gamma" if tex.gamma else "linear",
           "tiled" if tex.tiled else "linear storage")] += 1
        if tex.container == "bix":
            t[("bix", "base equals layout" if len(tex.base) == base else "base differs")] += 1
            t[("bix", "total equals layout" if len(tex.base) + len(tex.chain) == total
               else "total differs")] += 1
    print("texture files:")
    for k, v in sorted(t.items(), key=str):
        print(f"  {k}: {v}")
    rt = collections.Counter((r.type, r.flag) for r in tables.records.values())
    print("texture records (type, flag):", dict(sorted(rt.items())))
    print("model texture lists:", len(tables.lists))
    return 0


def cmd_model(args) -> int:
    tables = read_texture_tables(args.pvs)
    effects = Effects(args.root)
    path = os.path.join(args.root, "coloradoout.%05d.rmb.bin" % args.member)
    model = wg.read_rmb(path)
    mats = read_materials(model)
    g = 0
    for k, o in enumerate(model.objects):
        for si, s in enumerate(o.submeshes):
            if s.material_index >= len(mats.materials):
                print(f"object {k} submesh {si} {s.material_name!r}: no material, not drawn")
                g += 1
                continue
            mat = mats.materials[s.material_index]
            name = effect_name(mats.shaders[mat.shader])
            uv = mats.uv_constants[g] if mats.uv_constants else None
            print(f"object {k} {o.name!r} stride {o.stride} submesh {si} {s.material_name!r}: {name}")
            print("  uvOffsetScale", uv and tuple(round(x, 5) for x in uv))
            print("  PS constants", [tuple(round(x, 4) for x in v) for v in mat.ps_constants])
            samplers = effects.samplers(name)
            for slot, value in enumerate(mat.textures):
                rec = slot_texture(tables, args.member, value)
                what = (f"_0x{rec.texture_id:08X} type {rec.type} flag {rec.flag}" if rec else f"{value}")
                print(f"  slot {slot} {samplers.get(slot, '')}: {what}")
            g += 1
    return 0


def cmd_texture(args) -> int:
    tex = read_texture(args.root, int(args.id, 0))
    w, h, rgba = decode_level(tex, args.level)
    write_png(args.out, w, h, rgba)
    print(f"{tex.container} {tex.width}x{tex.height} levels {tex.levels} {BLOCK[tex.format][1]}"
          f" {'gamma' if tex.gamma else 'linear'} -> {args.out} ({w}x{h})")
    return 0


def _texture_index(root: str):
    """64-byte heads of every texture's storage -> texture id, to name the
    textures a dump binds (memory holds the files' bytes unchanged)."""
    index = {}
    for p in glob.glob(os.path.join(root, "_0x*.bix")):
        n = os.path.basename(p)
        d = Path(p).read_bytes()
        index[d[:64] if n.endswith("_B.bix") else d[28:92]] = int(n[3:11], 16)
    for p in glob.glob(os.path.join(root, "_0x*.bin")):
        d = Path(p).read_bytes()
        if d.startswith(b"CAFF"):
            index[d[0x2B0:0x2F0]] = int(os.path.basename(p)[3:11], 16)
    return index


def _content(root: str, texture_id: int):
    try:
        t = read_texture(root, texture_id)
    except (FormatError, FileNotFoundError):
        return None
    return hashlib.md5(t.base + t.chain).hexdigest()


# Render-backend registers (the console's published register map).
RB_BLENDCONTROL0, RB_COLORCONTROL, RB_ALPHA_REF = 0x2201, 0x2202, 0x210E
ALPHA_FUNCS = ("never", "less", "equal", "less-equal", "greater", "not-equal", "greater-equal", "always")


def cmd_check_dump(args) -> int:
    tables = read_texture_tables(args.pvs)
    effects = Effects(args.root)
    original = wg.walk_draws

    def walk(registers, packets, memory, predicate=False, constants=False):
        """world-geometry's walker, plus each draw's shaders, fetch
        constants and c0."""
        regs = list(registers)
        extra = []
        vs = ps = None
        i = 0
        while i < len(packets):
            h = packets[i]
            kind = h >> 30
            if kind == 0:
                base, count, single = h & 0x7FFF, ((h >> 16) & 0x3FFF) + 1, (h >> 15) & 1
                for k in range(count):
                    reg = base if single else base + k
                    if reg < len(regs):
                        regs[reg] = packets[i + 1 + k]
                i += 1 + count
                continue
            if kind == 1:
                regs[h & 0x7FF] = packets[i + 1]
                regs[(h >> 11) & 0x7FF] = packets[i + 2]
                i += 3
                continue
            if kind == 2:
                i += 1
                continue
            op, count = (h >> 8) & 0x7F, ((h >> 16) & 0x3FFF) + 1
            body = packets[i + 1:i + 1 + count]
            if op == 0x27:
                code = memory.read((body[0] & ~3) & 0x1FFFFFFF, (body[1] & 0xFFFF) * 4)
                vs, ps = (code, ps) if body[0] & 3 == 0 else (vs, code)
            elif op == 0x2B:
                code = struct.pack(f">{body[1] & 0xFFFF}I", *body[2:2 + (body[1] & 0xFFFF)])
                vs, ps = (code, ps) if body[0] & 3 == 0 else (vs, code)
            elif op == 0x2D:
                base = wg.CONSTANT_BASES.get((body[0] >> 16) & 0xFF)
                if base is not None:
                    for k, v in enumerate(body[1:]):
                        regs[base + (body[0] & 0x7FF) + k] = v
            elif op in (0x55, 0x56):
                for k, v in enumerate(body[1:]):
                    regs[(body[0] & 0xFFFF) + k] = v
            elif op == 0x2F:
                base = wg.CONSTANT_BASES.get((body[1] >> 16) & 0xFF)
                size = body[2] & 0xFFF
                loaded = memory.read(body[0] & 0x1FFFFFFF, 4 * size)
                if base is not None and loaded is not None:
                    for k, v in enumerate(struct.unpack(f">{size}I", loaded)):
                        regs[base + (body[1] & 0x7FF) + k] = v
            elif op in (0x22, 0x36):
                extra.append((vs, ps, tuple(tuple(regs[wg.FETCH + 6 * t:wg.FETCH + 6 * t + 6])
                                             for t in range(32)),
                              struct.unpack("<4f", struct.pack("<4I", *regs[wg.ALU:wg.ALU + 4])),
                              (regs[RB_COLORCONTROL], regs[RB_ALPHA_REF], regs[RB_BLENDCONTROL0])))
            i += 1 + count
        draws = original(registers, packets, memory, predicate, constants)
        if len(draws) != len(extra):
            raise FormatError("walkers disagree on the draw count")
        for d, (v, p, t, c0, state) in zip(draws, extra):
            d.update(vs_code=v, ps_code=p, textures=t, c0=c0, state=state)
        return draws

    wg.walk_draws = walk
    draws, tied, _ = wg.tie_draws(args)
    _, _, blocks = wg.read_frame_dump(args.dump)
    memory = wg.GuestMemory(blocks)
    heads = _texture_index(args.root)
    ps_of_effect = {}
    c = collections.Counter()
    layouts = collections.Counter()
    binding = collections.Counter()
    states = collections.Counter()
    cache = {}
    for n, (model, k, sub) in sorted(tied.items()):
        d = draws[n]
        if model.path not in cache:
            cache[model.path] = read_materials(model)
        mats = cache[model.path]
        o = model.objects[k]
        mat = mats.materials[sub.material_index]
        name = effect_name(mats.shaders[mat.shader])
        if name not in ps_of_effect:
            ps_of_effect[name] = {code for stage, code, _, _ in effects.get(name) or [] if stage == "ps"}
        colour = d["ps_code"] in ps_of_effect[name]
        c["tied draws"] += 1
        if not colour:
            c["tied draws of another pass (pixel shader not the material's effect)"] += 1
            continue
        c["colour-pass draws (pixel shader from the material's effect)"] += 1
        colour_control, alpha_ref, blend = d["state"]
        test = (f"alpha test {ALPHA_FUNCS[colour_control & 7]} "
                f"{struct.unpack('<f', struct.pack('<I', alpha_ref))[0]:.3f}"
                if colour_control & 8 else "no alpha test") + (", alpha to coverage" if colour_control & 0x10 else "")
        tokens = " ".join(t for t in ("_BLEND", "DECAL", "OPAC") if t in name)
        states[(effect_family(name), tokens and f"name has {tokens}", test,
                f"blend {blend & 0x1FFF1FFF:#010x}")] += 1
        vf = tuple((f[2], f[3]) for f in fetches(d["vs_code"] or b"") if f[0] == "v" and f[1] == o.stride)
        layouts[(o.stride, vf)] += 1
        decl = [x for x in effects.declarations(name) if packed_size(x) == o.stride]
        if not vf:
            c["vertex fetches: shader microcode not recorded"] += 1
        elif decl and all(FETCH_FORMAT[u] == fmt for off, fmt in vf
                          for at, u, _ in packed(decl[0]) if at == off) and                 {off for off, _ in vf} <= {at for at, _, _ in packed(decl[0])}:
            c["vertex fetches: every offset and format as the packed declaration"] += 1
        else:
            c["vertex fetches: differ from the packed declaration"] += 1
        g = sum(len(x.submeshes) for x in model.objects[:k]) + o.submeshes.index(sub)
        if mats.uv_constants:
            want = mats.uv_constants[g]
            same = all(abs(a - b) <= 1e-4 * max(1, abs(b)) for a, b in zip(d["c0"], want))
            c["c0 equals the submesh's UV constant" if same else "c0 differs from the submesh's UV constant"] += 1
        else:
            c["model without a UV constant per submesh"] += 1
        used = sorted({f[1] for f in fetches(d["ps_code"] or b"") if f[0] == "t"})
        member = member_number(model.path)
        for slot in used:
            if slot >= len(mat.textures):
                binding["slot past the material's list (runtime texture)"] += 1
                continue
            value = mat.textures[slot]
            fc = d["textures"][slot]
            address = ((fc[1] >> 12) << 12) & 0x1FFFFFFF
            size = ((fc[2] & 0x1FFF) + 1, ((fc[2] >> 13) & 0x1FFF) + 1)
            head = memory.read(address, 64) if fc[0] & 3 == 2 else None
            got = heads.get(head) if head and any(head) else None
            rec = slot_texture(tables, member, value)
            listed = None
            if rec is not None and rec.type in (0, 4):
                try:
                    listed = read_texture(args.root, rec.texture_id)
                except (FormatError, FileNotFoundError):
                    pass
            if listed is not None and size == (listed.width, listed.height):
                seen, equal = memory.compare(address, listed.base)
                if seen and equal:
                    binding[("listed texture bound", f"all zero, slot {slot}" if not any(listed.base)
                             else "same bytes")] += 1
                    continue
            if (listed is not None and listed.container == "bix"
                    and size == (max(listed.width // 2, 1), max(listed.height // 2, 1))):
                seen, equal = memory.compare(address, listed.chain[:4096])
                if seen and equal:
                    binding[("listed texture bound from its mip chain alone (base not loaded)",
                             f"all zero, slot {slot}" if not any(listed.chain) else "same bytes")] += 1
                    continue
            if rec is None:
                binding[(f"list value {value}", f"bound {size[0]}x{size[1]}")] += 1
            elif rec.type == 1:
                binding[("record type 1 (no file)", f"bound {size[0]}x{size[1]}",
                         "named" if got else "not named")] += 1
            elif got is None:
                binding[("bound texture not named", f"record type {rec.type}",
                         f"bound {size[0]}x{size[1]}" + (f", listed {listed.width}x{listed.height}"
                                                         if listed else ""), f"slot {slot}")] += 1
            elif _content(args.root, got) == _content(args.root, rec.texture_id):
                binding["same content, other id"] += 1
            elif rec.flag == 0:
                binding["flag-0 record, other texture"] += 1
            else:
                binding[("different texture", effect_family(name), f"slot {slot}")] += 1
    for k, v in c.items():
        print(f"{k}: {v}")
    print("colour-pass vertex fetches of the model's buffer (stride: offset, format):")
    for (stride, vf), v in sorted(layouts.items(), key=str):
        print(f"  {stride}: {v} draws: {vf}")
    print("colour-pass render state by effect family:")
    for k, v in sorted(states.items(), key=str):
        print(f"  {k}: {v}")
    print("texture slots the colour-pass pixel shaders sample:")
    for k, v in sorted(binding.items(), key=str):
        print(f"  {k}: {v}")
    return 0


def cmd_reference(args) -> int:
    """A reference frame for the engine: the capture as PNG and the main
    view's matrix in the M1 reference conventions
    (docs/formats/AUTOSHOW_REFERENCE.md): column-major for column vectors,
    Direct3D clip space for an upright image (the game's half turn taken
    out), standard z/w (0 near), half a pixel removed. World space is the
    render models' file frame, positions as stored."""
    import json
    sys.path.insert(0, str(HERE))
    import _ppm
    draws, tied, _ = wg.tie_draws(args)
    scene = wg.scene_matrix(draws, tied)
    width, height, pixels = _ppm.read_ppm(args.capture)
    rows = [list(scene[4 * r:4 * r + 4]) for r in range(4)]
    rows[0] = [-v for v in rows[0]]                       # half turn: clip x
    rows[1] = [-v for v in rows[1]]                       # and clip y
    rows[0] = [a - b / width for a, b in zip(rows[0], rows[3])]    # half pixel
    rows[1] = [a + b / height for a, b in zip(rows[1], rows[3])]
    column_major = [rows[r][c] for c in range(4) for r in range(4)]
    # The z row is a multiple of the w row less a constant: z/w = 0 at w = near.
    zs = [rows[2][c] / rows[3][c] for c in range(3) if abs(rows[3][c]) > 1e-6]
    scale = sum(zs) / len(zs)
    offset = rows[2][3] - scale * rows[3][3]
    near = -offset / scale
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    image = out.with_suffix(".png")
    _ppm.write_png(image, width, height, pixels)
    members = sorted({os.path.basename(m.path) for n, (m, _, _) in tied.items()
                      if draws[n]["mvp"] == scene})
    doc = {
        "schema": "shift-engine.world-reference.v1",
        "image": image.name,
        "width": width,
        "height": height,
        "clip_from_world": column_major,
        "near": near,
        "half_pixel_removed": True,
        "camera_position": list(wg.camera_position(scene)),
        "world_space_members": members,
        "game_clip_from_world": list(scene),
    }
    out.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"{out}: {len(members)} world-space members, near {near:.4f} m, camera "
          + ", ".join(f"{v:.3f}" for v in doc["camera_position"]))
    return 0


def cmd_samplers(args) -> int:
    """Each track effect's sampler per texture slot (from its constant
    tables), and the distinct slot layouts with their effects."""
    effects = Effects(args.root)
    layouts = collections.defaultdict(list)
    for path in sorted(glob.glob(os.path.join(effects.dir, "*.fxobj"))):
        name = os.path.basename(path)[:-6].upper()
        slots = effects.samplers(name)
        row = tuple((k, slots[k].replace("Sampler", "").replace("Samp", "").strip("_"))
                    for k in sorted(slots) if k < 8)
        layouts[row].append(name)
    for row, names in sorted(layouts.items(), key=lambda x: -len(x[1])):
        print(f"{len(names)} effects: " + ", ".join(f"{k} {n}" for k, n in row))
        if args.verbose:
            for n in names:
                print(f"    {n}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    census = commands.add_parser("census")
    census.add_argument("root")
    census.add_argument("--pvs", default=str(DEFAULT_PVS))
    census.set_defaults(run=cmd_census)
    model = commands.add_parser("model")
    model.add_argument("root")
    model.add_argument("member", type=int)
    model.add_argument("--pvs", default=str(DEFAULT_PVS))
    model.set_defaults(run=cmd_model)
    texture = commands.add_parser("texture")
    texture.add_argument("root")
    texture.add_argument("id", help="texture id, for example 0x1197")
    texture.add_argument("--out", required=True)
    texture.add_argument("--level", type=int, default=0)
    texture.set_defaults(run=cmd_texture)
    check = commands.add_parser("check-dump")
    check.add_argument("root")
    check.add_argument("dump")
    check.add_argument("--members", required=True, help="comma-separated names, or @file")
    check.add_argument("--box", type=float, nargs=4, default=None)
    check.add_argument("--pvs", default=str(DEFAULT_PVS))
    check.set_defaults(run=cmd_check_dump)
    samplers = commands.add_parser("samplers")
    samplers.add_argument("root")
    samplers.add_argument("--verbose", action="store_true", help="list the effects of each layout")
    samplers.set_defaults(run=cmd_samplers)
    reference = commands.add_parser("reference")
    reference.add_argument("root")
    reference.add_argument("dump")
    reference.add_argument("capture", help="a binary PPM capture of the same frame")
    reference.add_argument("--members", required=True, help="comma-separated names, or @file")
    reference.add_argument("--box", type=float, nargs=4, default=None)
    reference.add_argument("--out", required=True, help="the JSON to write; the PNG goes beside it")
    reference.set_defaults(run=cmd_reference)
    args = parser.parse_args(argv)
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
