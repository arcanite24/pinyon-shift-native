#!/usr/bin/env python3
"""The M1 car denominator: every base, v4 and DLC car, and where to see it.

Builds the list the car viewer (docs/REIMPLEMENTATION_PLAN.md, M1) must
cover, from the player's own files, read-only:

- the base disc database `media/db/gamedb.slt`;
- the v4 title update's copy in its `media.zip` (`db/gamedb.slt`; v4 reads
  only that one), and its `db/patch/1401000_merge.slt` (upgrade tables only);
- each installed Marketplace package's merge database
  (`Media/DLCZips/<offer>_pri_65.zip`, member `media/db/patch/*_merge.slt`),
  merged over v4's rows the way the title does at Start: whole rows by key,
  in ascending package priority.

For each car it records the row's origin (base, v4 or the package that adds
it), where the car's files live (a base `media/cars/<MediaName>.zip`, or the
`media/cars/<MediaName>/` folder of a package archive), and which screen can
show it. The Autoshow's car lists ask for drivable, selectable, non-unicorn,
visible cars that have a powertrain row; visibility of a DLC car follows the
package's offer and licence mask (findings "ContentOffersMapping and licence
masks"), which this tool evaluates from the package records in
`<state>/dlc/*.json`. A live check of the same cars is the render-test
directive `# dlc-car-trace` (tools/run-fh1-render-test.py).

The community UDLC package (`4D5309C900000729`) is not official content: it
is listed apart and left out of the M1 counts unless `--with-udlc`.

    python -I tools/re/car-list.py --state-root <preview> [--out .local/re/cars/car-list.json]

Output: JSON with names, IDs, flags and paths only (it stays under .local),
and a markdown summary on stdout. Databases are extracted once into
`.local/re/cars/db/` with the project's LZX extractor.
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
UDLC = "4D5309C900000729"
MARKETPLACE = Path("user/0000000000000000/4D5309C9/00000002")


def load_strings_module():
    spec = importlib.util.spec_from_file_location("fh1_strings", REPO / "tools" / "fh1-strings.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extract(extractor: Path, archive: Path, member: str, target: Path) -> Path:
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(extractor), "--archive-member", str(archive), member, str(target)],
                       check=True, capture_output=True, text=True)
    return target


def read_only(path: Path) -> sqlite3.Connection:
    return sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)


def priority(archive: Path) -> int:
    return int(archive.stem.rsplit("_pri_", 1)[1])


class Packages:
    """Installed Marketplace packages: records, offers, archives, merges."""

    def __init__(self, state: Path, extractor: Path, cache: Path, with_udlc: bool) -> None:
        self.root = state / MARKETPLACE
        self.items = []
        for folder in sorted(p for p in self.root.iterdir() if p.is_dir()) if self.root.is_dir() else []:
            package = folder.name
            if package == UDLC and not with_udlc:
                continue
            record_path = state / "dlc" / f"{package}.json"
            record = json.loads(record_path.read_text(encoding="utf-8")) if record_path.is_file() else {}
            offers = sorted(p.stem for p in (folder / "Media").glob("*.puboffer"))
            archives = sorted((folder / "Media" / "DLCZips").glob("*_pri_*.zip"), key=priority)
            content = [a for a in archives if not a.name.lower().startswith(("stringtables", "cameras"))]
            strings = [a for a in archives if a.name.lower().startswith("stringtables")]
            merges, cars = [], {}
            for archive in content:
                try:
                    infos = zipfile.ZipFile(archive).infolist()
                except zipfile.BadZipFile:
                    continue
                for info in infos:
                    name = info.filename
                    low = name.lower()
                    if low.startswith("media/db/patch/") and low.endswith(".slt"):
                        merges.append(extract(extractor, archive, name,
                                              cache / f"dlc-{package}-{Path(name).name}"))
                    parts = name.split("/")
                    if len(parts) >= 4 and parts[0].lower() == "media" and parts[1].lower() == "cars":
                        entry = cars.setdefault(parts[2].lower(), {"archive": archive, "folder": "/".join(parts[:3]),
                                                                   "members": 0, "lod0": None})
                        entry["members"] += 1
                        if Path(low).name == parts[2].lower() + "_lod0.carbin":
                            entry["lod0"] = info.file_size
            self.items.append({
                "package": package,
                "name": record.get("display_name", ""),
                "license_mask": int(record.get("license_mask", "0"), 16) if record else 0,
                "offers": offers,
                "priority": min((priority(a) for a in content), default=0),
                "merges": merges,
                "cars": cars,
                "strings": strings,
            })
        self.items.sort(key=lambda item: (item["priority"], item["package"]))


def merged_database(v4_db: Path, packages: Packages, scratch: Path) -> tuple[Path, dict]:
    """v4's database with every package's merge rows applied (whole rows by
    key, later packages winning). Returns the copy and car -> packages."""
    merged = scratch / "merged.slt"
    shutil.copy2(v4_db, merged)
    adds: dict[int, list[str]] = collections.defaultdict(list)
    db = sqlite3.connect(f"file:{merged.as_posix()}", uri=True)
    try:
        main_tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        for item in packages.items:
            for merge in item["merges"]:
                db.execute("ATTACH DATABASE ? AS m", (f"file:{merge.as_posix()}?mode=ro",))
                tables = [r[0] for r in db.execute("SELECT name FROM m.sqlite_master WHERE type='table'")]
                for table in tables:
                    if table not in main_tables:
                        continue
                    ours = [r[1] for r in db.execute(f'PRAGMA main.table_info("{table}")')]
                    theirs = [r[1] for r in db.execute(f'PRAGMA m.table_info("{table}")')]
                    columns = [c for c in theirs if c in ours]
                    quoted = ",".join(f'"{c}"' for c in columns)
                    db.execute(f'INSERT OR REPLACE INTO main."{table}" ({quoted}) SELECT {quoted} FROM m."{table}"')
                    if table == "Data_Car":
                        for (car,) in db.execute('SELECT Id FROM m."Data_Car"'):
                            adds[car].append(item["package"])
                db.commit()
                db.execute("DETACH DATABASE m")
    finally:
        db.close()
    return merged, adds


def string_tables(strings_module, game: Path, extractor: Path, packages: Packages) -> dict:
    """English text by `_&N` reference: base tables, then the packages'
    replacement tables in ascending priority (later tables win)."""
    text: dict[str, str] = {}
    sources = [game / "media/stringtables/EN.zip"]
    for item in packages.items:
        sources += item["strings"]
    for archive in sources:
        try:
            members = {i.filename.lower(): i.filename for i in zipfile.ZipFile(archive).infolist()}
        except (FileNotFoundError, zipfile.BadZipFile):
            continue
        for table in ("Data_Car.str", "List_CarMake.str"):
            name = members.get(table.lower()) or members.get(("en/" + table).lower())
            if not name:
                continue
            data = strings_module.read_archive_entry(archive, name, extractor)
            table_hash = strings_module.table_hash(table[:-4])
            for key, value in strings_module.parse(data):
                if value:
                    text[f"_&{(table_hash << 16) | key}"] = value
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--state-root", type=Path, required=True,
                        help="preview state root: title-update-v4/, dlc/, user/ (read-only)")
    parser.add_argument("--game", type=Path, default=REPO / ".local/game/base")
    parser.add_argument("--extractor", type=Path,
                        default=REPO / "out/build/win-amd64-release/pinyon_shift_fh1_archive_extract.exe")
    parser.add_argument("--cache", type=Path, default=REPO / ".local/re/cars/db")
    parser.add_argument("--out", type=Path, default=REPO / ".local/re/cars/car-list.json")
    parser.add_argument("--with-udlc", action="store_true", help="count the community UDLC package too")
    args = parser.parse_args(argv)

    state = args.state_root
    base_db = args.game / "media/db/gamedb.slt"
    v4_db = extract(args.extractor, state / "title-update-v4/media.zip", "db/gamedb.slt",
                    args.cache / "v4-db_gamedb.slt")
    packages = Packages(state, args.extractor, args.cache, args.with_udlc)
    strings = load_strings_module()
    names = string_tables(strings, args.game, args.extractor, packages)

    with read_only(base_db) as db:
        base_ids = {r[0] for r in db.execute("SELECT Id FROM Data_Car")}
    with read_only(v4_db) as db:
        v4_ids = {r[0] for r in db.execute("SELECT Id FROM Data_Car")}

    merged, adds = merged_database(v4_db, packages, args.cache)
    db = read_only(merged)
    try:
        db.row_factory = sqlite3.Row
        makes = {r["ID"]: names.get(r["DisplayName"], r["DisplayName"]) for r in db.execute("SELECT * FROM List_CarMake")}
        powertrains = {r[0] for r in db.execute("SELECT Id FROM PowerTrains")}
        offers = {r["OfferId"]: dict(r) for r in db.execute("SELECT * FROM ContentOffers")}
        mapping = collections.defaultdict(list)
        for r in db.execute("SELECT * FROM ContentOffersMapping WHERE ContentType=1"):
            mapping[r["ContentId"]].append(r["OfferId"])
        colours = collections.defaultdict(list)
        for r in db.execute("SELECT Ordinal, Sequence, RGB, Metallic FROM Combo_Colors ORDER BY Ordinal, Sequence"):
            colours[r[0]].append((r[1], r[2], r[3]))
        rows = [dict(r) for r in db.execute("SELECT * FROM Data_Car ORDER BY Id")]
    finally:
        db.close()

    by_offer_id = {}
    for item in packages.items:
        for offer in item["offers"]:
            by_offer_id[offer] = item
    base_archives = {p.stem.lower(): p for p in (args.game / "media/cars").glob("*.zip")}

    cars = []
    media_users = collections.Counter(r["MediaName"].lower() for r in rows)
    for r in rows:
        car = r["Id"]
        media = r["MediaName"] or ""
        origin = "base" if car in base_ids else "v4" if car in v4_ids else "dlc"
        packs = adds.get(car, [])
        # Ownership: an offer mapped to the car must come from an installed
        # package whose licence mask covers the offer's mask (an offer
        # with mask 0, such as Honda Challenge's, needs only the package).
        owned_by, offer_ids = [], mapping.get(car, [])
        for offer_id in offer_ids:
            offer = offers.get(offer_id)
            if not offer:
                continue
            item = by_offer_id.get(str(offer["PubOfferingID"]))
            if item and (not offer["LicenseMask"] or item["license_mask"] & offer["LicenseMask"]):
                owned_by.append(item["package"])
        if offer_ids:
            visible = bool(owned_by)
        else:
            visible = origin != "dlc" or bool(packs)
        files = []
        if media.lower() in base_archives:
            archive = base_archives[media.lower()]
            files.append({"source": "base", "path": archive.relative_to(args.game).as_posix(),
                          "lod0": next((i.file_size for i in zipfile.ZipFile(archive).infolist()
                                        if i.filename.lower() == media.lower() + "_lod0.carbin"), None)})
        for item in packages.items:
            entry = item["cars"].get(media.lower())
            if entry:
                files.append({"source": item["package"], "path": entry["archive"].relative_to(state).as_posix()
                              + "!" + entry["folder"] + "/", "lod0": entry["lod0"], "members": entry["members"]})
        drivable, selectable, unicorn = bool(r["IsDrivable"]), bool(r["IsSelectable"]), bool(r["IsUnicorn"])
        has_powertrain = r["PowertrainID"] in powertrains
        if not visible:
            screen = "none: not entitled (offer or licence missing)"
        elif drivable and selectable and not unicorn and has_powertrain:
            screen = "autoshow"
        elif drivable and selectable and unicorn and has_powertrain:
            screen = "garage when owned (unicorn); Autoshow only with a test mod clearing IsUnicorn"
        elif drivable and selectable and not has_powertrain:
            screen = "none: no powertrain row (Autoshow hides it)"
        elif not selectable and media_users[media.lower()] > 1:
            screen = "same model as a selectable row (event-only variant)"
        else:
            screen = "none in the stock game (not selectable: traffic, event or showcase row)"
        palette = colours.get(car, [])
        cars.append({
            "id": car,
            "media_name": media,
            "display_name": names.get(r["DisplayName"], r["DisplayName"]),
            "make": makes.get(r["MakeID"], r["MakeID"]),
            "year": r["Year"],
            "row_origin": origin,
            "row_packages": packs,
            "offer_packages": owned_by,
            "files": files,
            "flags": {k: r[k] for k in ("IsDrivable", "IsSelectable", "IsUnicorn", "IsArcade", "IsPurchased",
                                         "IsInstalled", "ContentId", "PowertrainID", "ClassID", "HasLivery")},
            "performance_index": r["PerformanceIndex"],
            "visible": visible,
            "screen": screen,
            "colours": len(palette),
            "first_colour": {"sequence": palette[0][0], "rgb": f"{palette[0][1]:06X}", "metallic": palette[0][2]}
                            if palette else None,
        })

    # The Autoshow's lists: makes by English name, then the make's cars.
    autoshow = [c for c in cars if c["screen"] == "autoshow"]
    make_order = sorted({c["make"] for c in autoshow}, key=lambda m: str(m).lower())
    by_make = collections.defaultdict(list)
    for c in autoshow:
        c["autoshow_make_index"] = make_order.index(c["make"])
        by_make[c["make"]].append(c)
    # Within a make the list is ordered by class, then performance index.
    for group in by_make.values():
        group.sort(key=lambda c: (c["flags"]["ClassID"], c["performance_index"]))
        for i, c in enumerate(group):
            c["autoshow_list_index"] = i
    out = {
        "schema": "pinyon-shift.m1-car-list.v1",
        "state_root": str(state),
        "packages": [{"package": i["package"], "name": i["name"], "license_mask": f"{i['license_mask']:08X}",
                      "offers": i["offers"], "merges": [m.name for m in i["merges"]],
                      "car_folders": len(i["cars"])} for i in packages.items],
        "autoshow_makes": make_order,
        "cars": cars,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(out, indent=1, ensure_ascii=False), encoding="utf-8")

    def source(c):
        if c["row_origin"] == "dlc":
            return "dlc"
        if c["row_origin"] == "v4":
            return "v4"
        return "base (DLC offer)" if c["offer_packages"] else "base"
    table = collections.Counter((source(c), c["screen"]) for c in cars)
    print(f"{len(cars)} car rows, {len({c['media_name'].lower() for c in cars})} distinct media names, "
          f"{len(autoshow)} in the Autoshow under {len(make_order)} makes\n")
    print("| Source | Screen | Cars |\n| --- | --- | ---: |")
    for (src, screen), n in sorted(table.items()):
        print(f"| {src} | {screen} | {n} |")
    missing = [c["id"] for c in cars if not c["files"]]
    print(f"\nrows without files: {len(missing)} {missing[:40]}")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
