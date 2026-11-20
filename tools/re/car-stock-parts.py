#!/usr/bin/env python3
"""Stock part selection and factory liveries of the M1 cars.

Counts what docs/formats/CARS.md quotes under "Stock parts" and "Factory
liveries": for every merged `Data_Car` row, the variant parts (bumpers,
skirts, hood, wing, exhausts, roll cage, seats) its stock upgrade rows
select, against the selection shown by the native engine's first M1
build (`shift-engine` apps/carview/showroom.cpp at d26abc9, emulated
here); and, given Autoshow frame dumps, which parts the game drew. It
reads the player's extracted files and database; nothing here contains
game data.

  car-stock-parts.py census <cars root> <merged.slt> [--report <m1 report.txt>]
  car-stock-parts.py dumps <cars root> <merged.slt> <dump.fh1frame>...
  car-stock-parts.py liveries <merged.slt>

The cars root is the one of tools/re/car-formats.py; `merged.slt` is the
database tools/re/car-list.py writes (base, v4 and the 21 packages).
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import math
import re
import sqlite3
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fh1_carbin  # noqa: E402

# Families the upgrade tables select from (List_PartsStrings names), the
# race-only parts, and the exhausts, which no table names.
VARIANT = re.compile(r"(bumperf|bumperr|skirtl|skirtr|hood|wing)([a-z]|race)|hood(ps|ci)race|cagerace"
                     r"|exhaust[lr]?[a-z]")
STOCK_TABLES = [("List_UpgradeCarBodyFrontBumper", "CarBodyID", "PartsStringId"),
                ("List_UpgradeCarBodyRearBumper", "CarBodyID", "PartsStringId"),
                ("List_UpgradeCarBodySideSkirt", "CarBodyID", "PartsStringId"),
                ("List_UpgradeCarBodyHood", "CarBodyID", "PartsStringId"),
                ("List_UpgradeRearWing", "Ordinal", "PartsStringId"),
                ("List_UpgradeCarBodyChassisStiffness", "CarbodyId", "PartsStringID"),
                ("List_UpgradeCarBodyWeight", "CarBodyId", "PartsStringID")]


def car_folders(root: Path) -> dict[str, Path]:
    folders: dict[str, Path] = {}
    for d in sorted((root / "base").iterdir()):
        folders.setdefault(d.name.lower(), d)
    for d in sorted(root.glob("dlc/*/*/[mM]edia/cars/*")):
        folders.setdefault(d.name.lower(), d)
    return folders


def stub(points) -> bool:
    """The engine's stand-in test: at most 6 vertices, a 17-vertex cube,
    or at most 24 vertices all on their box's corners."""
    if len(points) <= 6:
        return True
    if len(points) > 24:
        return False
    lo = [min(p[k] for p in points) for k in range(3)]
    hi = [max(p[k] for p in points) for k in range(3)]
    ext = [hi[k] - lo[k] for k in range(3)]
    if len(points) == 17 and max(ext) - min(ext) <= 0.02:
        return True
    return all(abs(p[k] - lo[k]) <= 0.001 or abs(p[k] - hi[k]) <= 0.001 for p in points for k in range(3))


def placeholder(s) -> bool:
    lods = sorted({sub.lod for sub in s.subsections if sub.indices})
    coarse = 0 in lods and len(lods) > 1 and s.lod0_pool.count * 4 < s.lod_pool.count
    for lod in lods:
        if lod == 0 and coarse:
            continue
        pool = s.lod0_pool if lod == 0 else s.lod_pool
        if pool.count and not stub(fh1_carbin.positions(s, pool)):
            return False
    return True


def read_models(folder: Path, media: str) -> dict[str, list]:
    models = {}
    for f in folder.iterdir():
        role = {media + ".carbin": "ext", media + "_lod0.carbin": "lod0",
                media + "_cockpit.carbin": "cockpit"}.get(f.name.lower())
        if role:
            models[role] = fh1_carbin.parse(f.read_bytes()).sections
    return models


def stock_names(db, car_id: int) -> tuple[set[str], set[str]]:
    """Part names the car's IsStock rows name, and the seats they remove."""
    strings = dict(db.execute("SELECT Id, PartsString FROM List_PartsStrings"))
    body = db.execute("SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal=? AND IsStock=1",
                      (car_id,)).fetchone()[0]
    shown, removed = set(), set()
    for table, key, column in STOCK_TABLES:
        value = car_id if key == "Ordinal" else body
        for (ref,) in db.execute(f"SELECT {column} FROM {table} WHERE {key}=? AND IsStock=1", (value,)):
            if ref in (None, "", 0, "0"):
                continue
            for name in strings[int(ref)].split(","):
                name = name.strip().lower()
                (removed if name.startswith("-") else shown).add(name.lstrip("-"))
    return shown, removed


def first_letter_hidden(n: str, real: set[str]) -> bool:
    """The first M1 build: race parts hidden; a name ending in b-h hidden
    when the same stem with an earlier letter is a real part."""
    if n.endswith("race"):
        return True
    if len(n) > 1 and "b" <= n[-1] <= "h":
        return any(n[:-1] + ch in real for ch in map(chr, range(ord("a"), ord(n[-1]))))
    return False


def select(models, rule: str, shown=frozenset(), removed=frozenset()) -> set[str]:
    """Lower-case names of the parts drawn, after showroom.cpp's model
    choice and stand-in fill, with variant visibility by `rule`."""
    flag = {role: [(s.name.lower(), placeholder(s)) for s in sections] for role, sections in models.items()}
    lod0, cockpit, ext = flag.get("lod0", []), flag.get("cockpit", []), flag.get("ext", [])
    body = lod0 if any(not ph for _, ph in lod0) else ext
    names = {n for n, _ in body}
    body_real = {n for n, ph in body if not ph}
    cockpit_real = {n for n, ph in cockpit if not ph}

    def hidden(n, real):
        if rule == "stock":
            if VARIANT.fullmatch(n):
                m = re.fullmatch(r"exhaust[lr]?([a-z])", n)
                return (m.group(1) != "a") if m else n not in shown
            if n in removed:
                return True
        return first_letter_hidden(n, real)

    drawn = {n for n, ph in body if n != "wheel" and not ph and not hidden(n, body_real)}
    drawn |= {n for n, ph in cockpit if n not in names and not n.endswith(("_mid", "_max")) and not ph
              and not hidden(n, cockpit_real)}
    if body is not ext:
        stand_ins = {n for n, ph in body if ph} | {n for n, ph in cockpit if ph and n not in names}
        real = body_real | cockpit_real
        stem = lambda n: n[:-1] if len(n) > 1 and "a" <= n[-1] <= "h" else ""
        stems = {stem(n) for n in real if stem(n)}
        ext_real = {n for n, ph in ext if not ph}
        for n in sorted(stand_ins):
            if n == "wheel" or n.endswith(("race", "_mid", "_max")) or n not in ext_real:
                continue
            if rule == "stock":
                if hidden(n, real):
                    continue
            elif stem(n) and stem(n) in stems:
                continue
            drawn.add(n)
            if stem(n):
                stems.add(stem(n))
    return drawn


def cmd_census(args) -> int:
    db = sqlite3.connect(args.db)
    folders = car_folders(Path(args.root))
    scored, failing = set(), set()
    if args.report:
        for line in open(args.report, encoding="utf-8"):
            f = line.split()
            if len(f) > 8 and f[0].isdigit():
                scored.add(int(f[0]))
                if "IoU-g<0.97" in line:
                    failing.add(int(f[0]))
    cache, counts, changed = {}, collections.Counter(), []
    for car_id, media in db.execute("SELECT Id, MediaName FROM Data_Car ORDER BY Id").fetchall():
        key = media.lower()
        if key not in cache:
            cache[key] = read_models(folders[key], key)
        models = cache[key]
        shown, removed = stock_names(db, car_id)
        counts["rows"] += 1
        counts["rows whose stock rows name a part other than letter a"] += any(
            not (n.endswith("a") or n in ("cagerace",)) for n in shown)
        counts["rows with a stock roll cage"] += "cagerace" in shown
        counts["rows with a stock seat removed"] += bool(removed)
        old, new = select(models, "first-letter"), select(models, "stock", shown, removed)
        if old != new:
            added, gone = sorted(new - old), sorted(old - new)
            changed.append(car_id)
            counts["rows whose drawn parts change"] += 1
            counts["  of them only losing parts"] += not added
            if car_id in scored:
                counts["  of them scored, failing" if car_id in failing else "  of them scored, passing"] += 1
            print(f"{car_id} {media}: drop {' '.join(gone) or '-'}; add {' '.join(added) or '-'}")
    for k, v in counts.items():
        print(f"{k}: {v}")
    print(f"distinct models: {len(cache)}")
    return 0


def cmd_dumps(args) -> int:
    spec = importlib.util.spec_from_file_location("world_geometry", Path(__file__).with_name("world-geometry.py"))
    wg = importlib.util.module_from_spec(spec)
    sys.modules["world_geometry"] = wg
    spec.loader.exec_module(wg)
    db = sqlite3.connect(args.db)
    folders = car_folders(Path(args.root))
    total = collections.Counter()
    for path in args.dumps:
        car_id = int(Path(path).name.split(".")[0])
        media = db.execute("SELECT MediaName FROM Data_Car WHERE Id=?", (car_id,)).fetchone()[0].lower()
        regs, packets, blocks = wg.read_frame_dump(path)
        constants = collections.defaultdict(list)
        for d in wg.walk_draws(regs, packets, wg.GuestMemory(blocks), constants=True):
            words = d.get("vs_constants") or []
            for k in range(0, len(words) - 3, 4):
                v = struct.unpack(">4f", struct.pack(">4I", *words[k:k + 4]))
                if all(math.isfinite(x) for x in v):
                    constants[tuple(round(x, 3) for x in v[:3])].append(v[3])
        models = read_models(folders[media], media)
        # A part is drawn when a draw's vertex constants hold its box offset
        # and half-extent length. Parts with an identical box are ambiguous.
        boxes, drawn = collections.Counter(), set()
        for sections in models.values():
            for s in sections:
                off = tuple(round(x, 3) for x in s.bounds[0:3])
                length = math.sqrt(sum(e * e for e in s.bounds[6:9]))
                boxes[(off, round(length, 3))] += 1
                if any(abs(x - length) < 0.002 for x in constants.get(off, [])):
                    drawn.add(s.name.lower())
        ambiguous = {s.name.lower() for sections in models.values() for s in sections
                     if boxes[(tuple(round(x, 3) for x in s.bounds[0:3]),
                               round(math.sqrt(sum(e * e for e in s.bounds[6:9])), 3))] > 1}
        shown, removed = stock_names(db, car_id)
        variants = {s.name.lower() for sections in models.values() for s in sections
                    if VARIANT.fullmatch(s.name.lower()) or s.name.lower().startswith("seat")} - ambiguous
        for rule in ("stock", "first-letter"):
            wrong = (select(models, rule, shown, removed) ^ drawn) & variants
            total[f"{rule} rule disagreements"] += len(wrong)
            print(f"{car_id} {media} {rule}: {len(variants)} variant parts, wrong {sorted(wrong)}")
        total["cars"] += 1
        total["variant parts compared"] += len(variants)
    for k, v in total.items():
        print(f"{k}: {v}")
    return 0


def cmd_liveries(args) -> int:
    db = sqlite3.connect(args.db)
    rows = db.execute("SELECT d.Id, d.MediaName, d.HasLivery, COUNT(k.ID) FROM Data_Car d "
                      "JOIN Combo_Colors k ON k.Ordinal = d.Id WHERE d.HasRaceLivery = 1 "
                      "GROUP BY d.Id ORDER BY d.Id").fetchall()
    for r in rows:
        print(*r)
    flags = db.execute("SELECT SUM(HasLivery), SUM(HasRaceLivery), SUM(HasLivery AND HasRaceLivery), "
                       "COUNT(DISTINCT CASE WHEN HasRaceLivery = 1 THEN LOWER(MediaName) END) "
                       "FROM Data_Car").fetchone()
    print("HasLivery rows, HasRaceLivery rows, both, distinct models with HasRaceLivery:", *flags)
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("census")
    c.add_argument("root")
    c.add_argument("db")
    c.add_argument("--report")
    c.set_defaults(func=cmd_census)
    d = sub.add_parser("dumps")
    d.add_argument("root")
    d.add_argument("db")
    d.add_argument("dumps", nargs="+")
    d.set_defaults(func=cmd_dumps)
    v = sub.add_parser("liveries")
    v.add_argument("db")
    v.set_defaults(func=cmd_liveries)
    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
