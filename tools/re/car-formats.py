#!/usr/bin/env python3
"""Census of Forza Horizon 1's car files for the M1 car viewer.

Counts what docs/formats/CARS.md quotes in its Tests section over the
player's extracted car folders: every `.carbin` model is parsed with
tools/fh1_carbin.py and written back, its header's draw-list block is
located and checked against its sub-sections, and every `.xds` header is
laid out and compared with its payload size. Nothing here contains game
data; it reads the player's extracted files.

  car-formats.py census <cars root> [--brakes <media/brakes>]
  car-formats.py dlc-duplicates <cars root>

The cars root holds `base/<MediaName>/`, `wheels/<WheelMedia>/` and
`dlc/<package id>/<offer zip stem>/media/cars/<MediaName>/` (see the
Reproduce section of docs/formats/CARS.md). Base folders `Shared`,
`Driver` and `AppearancePresets` and the community package
`4D5309C900000729` are left out of the model counts. A DLC car whose
media name is also a base folder is counted once, as base.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fh1_carbin  # noqa: E402

NOT_CARS = {"shared", "driver", "appearancepresets"}
COMMUNITY = "4D5309C900000729"
# Bytes per 4x4 block, or per texel for the 4-byte formats (block side 1).
FORMATS = {18: (8, 4), 49: (16, 4), 51: (8, 4), 53: (16, 4), 59: (8, 4), 6: (4, 1), 50: (4, 1)}


def u32(data: bytes, at: int) -> int:
    return struct.unpack_from(">I", data, at)[0]


def car_folders(root: Path):
    """(source, media name, folder) for every base and official DLC car."""
    for folder in sorted((root / "base").iterdir()):
        if folder.is_dir() and folder.name.lower() not in NOT_CARS:
            yield "base", folder.name, folder
    for package in sorted((root / "dlc").iterdir()):
        if not package.is_dir() or package.name.upper() == COMMUNITY:
            continue
        for offer in sorted(package.iterdir()):
            for top in ("media", "Media"):
                cars = offer / top / "cars"
                if cars.is_dir():
                    for folder in sorted(cars.iterdir()):
                        if folder.is_dir():
                            yield "dlc", folder.name, folder


def model_kind(name: str) -> str:
    name = name.lower()
    for kind in ("caliper", "rotor"):
        if kind in name:
            return kind
    if name.endswith("_cockpit.carbin"):
        return "cockpit"
    if name.endswith("_lod0.carbin"):
        return "lod0"
    return "lod"


def draw_lists(head: bytes):
    """Every position of the draw-list block: 4, 4, n, 2n lists, then 1
    at the end of the header (the part count follows the header)."""
    found = []
    for at in range(0, len(head) - 12, 4):
        if u32(head, at) != 4 or u32(head, at + 4) != 4:
            continue
        lods = u32(head, at + 8)
        if not 1 <= lods <= 8:
            continue
        p, lists = at + 12, []
        for _ in range(2 * lods):
            if p + 4 > len(head):
                break
            count = u32(head, p)
            if count > 4096 or p + 4 + 4 * count > len(head):
                break
            lists.append(struct.unpack_from(f">{count}I", head, p + 4))
            p += 4 + 4 * count
        if len(lists) == 2 * lods and p + 4 == len(head) and u32(head, p) == 1:
            found.append((lods, lists))
    return found


def scan_file(data: bytes) -> list[int]:
    """Offsets of the part count found by the spec's rule over the whole
    file: 4, 4, n (0 <= n <= 8), 2n lists, the word 1, then a count c with
    either c = 0 and only the 24-byte footer after it, or a part that
    parses right after it."""
    marker = struct.pack(">II", 4, 4)
    found = []
    at = data.find(marker)
    while at != -1:
        if at % 4 == 0 and at + 12 <= len(data) and u32(data, at + 8) <= 8:
            p, ok = at + 12, True
            for _ in range(2 * u32(data, at + 8)):
                count = u32(data, p) if p + 4 <= len(data) else 1 << 30
                if count > 4096 or p + 4 + 4 * count > len(data):
                    ok = False
                    break
                p += 4 + 4 * count
            if ok and p + 8 <= len(data) and u32(data, p) == 1:
                count = u32(data, p + 4)
                if (count == 0 and len(data) - (p + 8) == 24) or (
                        0 < count <= 4096 and fh1_carbin._plausible_section(data, p + 8)):
                    found.append(p + 4)
        at = data.find(marker, at + 1)
    return found


def _align(value: int, to: int) -> int:
    return (value + to - 1) // to * to


def _log2_ceil(value: int) -> int:
    return max(value - 1, 0).bit_length()


def xds_layout(data: bytes):
    """(format, tiled, payload size the layout needs) from an .xds header."""
    f = struct.unpack_from(">6I", data, 0x1C)
    fmt, tiled = f[1] & 0x3F, bool(f[0] >> 31)
    width, height = (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1
    pitch, levels, packed = ((f[0] >> 22) & 0x1FF) * 32, ((f[4] >> 6) & 0xF) + 1, (f[5] >> 11) & 1
    if fmt not in FORMATS or not tiled:
        return fmt, tiled, None
    block_bytes, side = FORMATS[fmt]
    shorter = _log2_ceil(min(width, height))
    tail = (shorter - 4 if shorter > 4 else 0) if packed else levels
    if packed and tail == 0:
        # All levels are in the packed tile: one region sized from the
        # dimensions rounded up to powers of two.
        base_pitch = _align(max((1 << _log2_ceil(width)) // side, 1), 32)
        rows = _align(max((1 << _log2_ceil(height)) // side, 1), 32)
    else:
        base_pitch = _align(max(pitch, width) // side or 1, 32)
        rows = _align((height + side - 1) // side, 32)
    size = _align(base_pitch * rows * block_bytes, 4096)
    for level in range(1, min(levels - 1, tail) + 1):
        lp = _align(max(((1 << _log2_ceil(width)) >> level) // side, 1), 32)
        rows = _align(max(((1 << _log2_ceil(height)) >> level) // side, 1), 32)
        size += _align(lp * rows * block_bytes, 4096)
    return fmt, tiled, size


def census(root: Path, brakes: Path | None) -> int:
    seen: set[str] = set()
    stats = {src: collections.Counter() for src in ("base", "dlc")}
    lods = {src: collections.Counter() for src in ("base", "dlc")}
    techniques = {src: set() for src in ("base", "dlc")}
    problems = []
    for src, car, folder in car_folders(root):
        if car.lower() in seen:
            continue
        seen.add(car.lower())
        s = stats[src]
        s["cars"] += 1
        for path in sorted(folder.iterdir()):
            name = path.name
            if not name.lower().endswith(".carbin") or name.lower().startswith("stripped_"):
                continue
            data = path.read_bytes()
            try:
                model = fh1_carbin.parse(data)
            except fh1_carbin.CarbinError as error:
                problems.append(f"{car}/{name}: {error}")
                continue
            s["models"] += 1
            s["kind " + model_kind(name)] += 1
            s["round trip"] += model.encode() == data
            expected = len(model.head) if model.sections else len(data) - 28
            if scan_file(data) == [expected]:
                s["draw-list scan from the file start finds one position"] += 1
            else:
                problems.append(f"{car}/{name}: the whole-file draw-list scan does not find one position")
            if not model.sections:
                s["no parts"] += 1
                continue
            s["parts"] += len(model.sections)
            subs = {(i << 16 | j): sub for i, part in enumerate(model.sections)
                    for j, sub in enumerate(part.subsections)}
            s["sub-sections"] += len(subs)
            s["vertices"] += sum(max(p.lod_pool.count, 0) + max(p.lod0_pool.count, 0)
                                 for p in model.sections if p.lod_pool.count < 1 << 31)
            for sub in subs.values():
                lods[src][sub.lod] += 1
                s["triangle lists"] += sub.index_type == 4
                s["32-bit indices"] += sub.index_size == 4
                techniques[src].add(sub.name)
            found = draw_lists(model.head)
            if len(found) != 1:
                problems.append(f"{car}/{name}: draw-list block found {len(found)} times")
                continue
            count, lists = found[0]
            entries = [entry for one in lists for entry in one]
            ok = sorted(entries) == sorted(subs) and all(
                subs[entry].lod == (k % count) + (1 if count == 5 else 0)
                for k, one in enumerate(lists) for entry in one)
            s["draw lists cover every sub-section once"] += ok
            if not ok:
                problems.append(f"{car}/{name}: draw lists do not match the sub-sections")
    for src, folders in (("base", [root / "base", root / "wheels"]),
                         ("dlc", [p for p in (root / "dlc").iterdir()
                                  if p.is_dir() and p.name.upper() != COMMUNITY])):
        s = stats[src]
        for folder in folders:
            for path in folder.rglob("*"):
                if not path.name.lower().endswith(".xds"):
                    continue
                data = path.read_bytes()
                s["xds"] += 1
                fmt, tiled, size = xds_layout(data)
                s[f"xds format {fmt}"] += 1
                if not tiled:
                    s["xds linear"] += 1
                elif size is not None and size == len(data) - 0x34:
                    s["xds payload equals layout"] += 1
                else:
                    problems.append(f"{path}: payload {len(data) - 0x34}, layout {size}")
    for src in ("base", "dlc"):
        print(f"[{src}]")
        for key, value in sorted(stats[src].items()):
            print(f"  {key}: {value:,}")
        print("  sub-sections by LOD: " + " / ".join(f"{lods[src][k]:,}" for k in range(6)))
        print(f"  distinct techniques: {len(techniques[src])}")
    print(f"distinct techniques over both: {len(techniques['base'] | techniques['dlc'])}")
    if brakes:
        parsed = empty = 0
        for path in sorted(brakes.glob("*.carbin")):
            data = path.read_bytes()
            try:
                parsed += fh1_carbin.parse(data).encode() == data
            except fh1_carbin.CarbinError:
                empty += draw_lists(data) == [] and u32(data, len(data) - 28) == 0
        print(f"[brakes] parse and write back: {parsed}, empty models: {empty}")
    for problem in problems:
        print("problem:", problem, file=sys.stderr)
    return 1 if problems else 0


def dlc_duplicates(root: Path) -> int:
    digests: dict[str, dict[str, str]] = collections.defaultdict(dict)
    for src, car, folder in car_folders(root):
        h = hashlib.sha256()
        for path in sorted(folder.rglob("*"), key=lambda p: str(p).lower()):
            if path.is_file():
                h.update(path.relative_to(folder).as_posix().lower().encode())
                h.update(path.read_bytes())
        where = "base" if src == "base" else folder.parents[3].name + "/" + folder.parents[2].name
        digests[car.lower()][where] = h.hexdigest()
    dlc = {car: v for car, v in digests.items() if any(k != "base" for k in v)}
    on_disc = [car for car, v in dlc.items() if "base" in v]
    same_as_disc = [car for car in on_disc if len(set(dlc[car].values())) == 1]
    several = {car: v for car, v in dlc.items() if sum(k != "base" for k in v) > 1}
    print(f"DLC car folders: {len(dlc)}, also on the disc: {len(on_disc)} "
          f"({len(same_as_disc)} byte-identical), DLC-only: {len(dlc) - len(on_disc)}")
    print(f"in more than one package: {len(several)}, identical across them: "
          f"{sum(len({d for k, d in v.items() if k != 'base'}) == 1 for v in several.values())}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("census")
    c.add_argument("root", type=Path)
    c.add_argument("--brakes", type=Path)
    d = sub.add_parser("dlc-duplicates")
    d.add_argument("root", type=Path)
    args = parser.parse_args(argv)
    if args.command == "census":
        return census(args.root, args.brakes)
    return dlc_duplicates(args.root)


if __name__ == "__main__":
    sys.exit(main())
