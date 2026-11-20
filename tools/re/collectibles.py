#!/usr/bin/env python3
"""Free-roam collectibles and the map (SPEC-FREEROAM-COLLECTIBLES).

`census` counts the discount signs and barn finds on the player's disc and
checks that every activity has its placement: `Flyers.xml` and
`barnfinds.xml` from an extracted `gamemodes.zip` (the native extractor,
`pinyon_shift_fh1_archive_extract --archive-all`), and the ribbon's
`GameObjs.xml`. It prints counts only.

`profile` reads a decrypted profile body (PINYON_SHIFT_M5_SAVE_TRACE=1
writes them to <state>/logs/save-snapshots) and reports the collectible
records of the class-serialised tail: per discount sign the revealed and
found bytes, per barn find its bytes, the two odometer readings and the
status (docs/specs/FREEROAM_COLLECTIBLES.md, "Profile records").

`overlay` draws the road network of `colorado.nav` and the sign and barn
placements onto the low-detail map background with the spec's world-to-map
equation and writes a PNG, so a reader can check the corners by eye. With
--point X Z it also prints the map pixel of a world point.

  python -I tools/re/collectibles.py census --gamemodes DIR --gameobjs GameObjs.xml
  python -I tools/re/collectibles.py profile payload-loaded-*.bin
  python -I tools/re/collectibles.py overlay --textures DIR --nav colorado.nav \
      --gameobjs GameObjs.xml --out map.png --point 4247.4 676.2
"""
from __future__ import annotations

import argparse
import importlib.util
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# The map background (MapProfileFullscreen.xml, group "background"): the
# tile offset is the south-west corner, the size the north-south extent.
MAP_WEST, MAP_SOUTH, MAP_HEIGHT = -9583.3, -6900.0, 11500.0
MAP_WIDTH = MAP_HEIGHT * 5 / 3


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def map_uv(x: float, z: float) -> tuple[float, float]:
    """World X/Z (game frame) to map UV, u east, v down from the north edge."""
    return (x - MAP_WEST) / MAP_WIDTH, (MAP_SOUTH + MAP_HEIGHT - z) / MAP_HEIGHT


def game_objects(path: Path) -> dict[str, tuple[float, float, float]]:
    out = {}
    for obj in ET.parse(path).getroot():
        p = obj.find("Pos")
        out[obj.get("GameplayID")] = tuple(float(p.get(k)) for k in "xyz")
    return out


def cmd_census(args) -> int:
    root = Path(args.gamemodes)
    objects = game_objects(Path(args.gameobjs))
    flyers = [a.get("name") for a in ET.parse(root / "Colorado" / "Flyers.xml").getroot()
              if a.get("type") == "ActivityFlyers"]
    barns = [a for a in ET.parse(root / "Colorado" / "barnfinds.xml").getroot()
             if a.get("type") == "ActivityBarnFind"]
    placed = sum(name in objects for name in flyers)
    print(f"discount signs: {len(flyers)} activities, {placed} placed in GameObjs.xml")
    barn_placed = door_sets = 0
    cars = set()
    for barn in barns:
        trigger = barn.find("TriggerZone").get("object")
        door = barn.find("DoorInfo")
        barn_placed += trigger in objects
        door_sets += all(door.get(k) in objects for k in
                         ("open_id", "closed_id", "open_coll_id", "closed_coll_id"))
        cars.add(int(barn.find("UnlockCar").get("id")))
    print(f"barn finds: {len(barns)} activities, {barn_placed} trigger objects placed, "
          f"{door_sets} complete door sets, {len(cars)} distinct cars")
    return 0 if placed == len(flyers) and barn_placed == len(barns) else 1


def records(blob: bytes, at: int, count: int):
    """`count` tail records from `at`: name, class, version, payload."""
    for _ in range(count):
        (n,) = struct.unpack_from(">I", blob, at)
        name = blob[at + 4:at + 4 + n].decode("ascii")
        at += 4 + n
        (m,) = struct.unpack_from(">I", blob, at)
        cls = blob[at + 4:at + 4 + m].decode("ascii")
        at += 4 + m
        version, size = struct.unpack_from(">II", blob, at)
        at += 8
        yield name, cls, version, blob[at:at + size]
        at += size


def cmd_profile(args) -> int:
    profile = load("fh1_profile", REPO / "tools" / "fh1-profile.py")
    body = Path(args.body).read_bytes()
    decoded = profile.decode(body)
    tail = decoded.tail
    marker = struct.pack(">I", 8) + b"Colorado"
    at = tail.find(marker)
    if at < 0:
        print("no Colorado location record in the tail")
        return 1
    _, cls, version, payload = next(records(tail, at, 1))
    (count,) = struct.unpack_from(">I", payload, 0)
    signs = found = revealed = 0
    print(f"{cls} version {version}: {count} records")
    for name, kind, ver, data in records(payload, 4, count):
        if kind == "CFlyerState":
            signs += 1
            revealed += data[0]
            found += data[1]
        elif kind == "CBarnFindState":
            found_at, spawn = struct.unpack_from(">ff", data, 3)
            (status,) = struct.unpack_from(">I", data, 11)
            print(f"  {name}: revealed {data[0]} discovered {data[1]} collected {data[2]} "
                  f"rumour at {spawn:.0f} found at {found_at:.0f} status {status}")
    print(f"  discount signs: {signs} records, {revealed} revealed, {found} found")
    for field in decoded.fields:
        if field.name == "Main":
            for child in field.children or field.value or []:
                if child.name in ("UpgradeDiscount", "TotalDistanceDriven"):
                    print(f"  Main/{child.name} = {child.value}")
    return 0


def cmd_overlay(args) -> int:
    ui = load("inspect_fh1_ui", REPO / "tools" / "inspect-fh1-ui.py")
    traffic = load("fh1_traffic", REPO / "tools" / "re" / "traffic.py")
    header, rgba = ui.decode_xds((Path(args.textures) / "Map" / "background" / "low-0-0.xds").read_bytes())
    w, h = header["width"], header["height"]
    img = bytearray(rgba)

    def put(px, py, colour, r=0):
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                x, y = int(px) + dx, int(py) + dy
                if 0 <= x < w and 0 <= y < h:
                    img[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(colour)

    def pixel(x, z):
        u, v = map_uv(x, z)
        return u * w, v * h

    nav = traffic.Nav(Path(args.nav))
    for _, _, points in nav.roads():
        q = [pixel(p[0], p[2]) for p in points]
        for (x0, y0), (x1, y1) in zip(q, q[1:]):
            n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
            for k in range(n + 1):
                put(x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n, (255, 0, 0, 255))
    for name, (x, _, z) in game_objects(Path(args.gameobjs)).items():
        if name.startswith("flyer_"):
            put(*pixel(x, z), (255, 255, 0, 255), 3)
        elif name.startswith("BARNFIND_"):
            put(*pixel(x, z), (0, 255, 255, 255), 6)
    if args.point:
        px, py = pixel(*args.point)
        put(px, py, (255, 0, 255, 255), 8)
        u, v = map_uv(*args.point)
        print(f"point ({args.point[0]}, {args.point[1]}): u {u:.5f} v {v:.5f}, "
              f"low-map pixel ({px:.1f}, {py:.1f})")
    Path(args.out).write_bytes(ui.encode_png(w, h, bytes(img)))
    print(f"wrote {args.out} ({w}x{h})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    census = commands.add_parser("census")
    census.add_argument("--gamemodes", required=True, help="extracted gamemodes.zip")
    census.add_argument("--gameobjs", required=True)
    census.set_defaults(run=cmd_census)
    prof = commands.add_parser("profile")
    prof.add_argument("body", help="a decrypted profile body")
    prof.set_defaults(run=cmd_profile)
    overlay = commands.add_parser("overlay")
    overlay.add_argument("--textures", required=True, help="extracted media/ui/textures/Horizon.zip")
    overlay.add_argument("--nav", required=True)
    overlay.add_argument("--gameobjs", required=True)
    overlay.add_argument("--out", required=True)
    overlay.add_argument("--point", type=float, nargs=2, metavar=("X", "Z"))
    overlay.set_defaults(run=cmd_overlay)
    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
