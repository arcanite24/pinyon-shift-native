#!/usr/bin/env python3
"""Create the M3 oracle-lap seed: a solo one-lap circuit race for a pinned car.

The native engine's M3 gate (docs/REIMPLEMENTATION_PLAN.md) compares a lap
against the translated game (docs/specs/CAR_PHYSICS.md, "Oracle lap for the
M3 gate"). This derives a private seed from a pinned seed whose modded
profile holds the car to test (acc9-stock: a stock R34), and adds the mod
m3_oracle_lap:

  * db/10-oracle-lap.sql clones the festival race FR06 into a new solo
    event M3_ORACLE_LAP (no other drivers, no prize, no car restriction) on
    a circuit track (default 158, FESTIVAL_FEST_007) with one lap;
  * members/media/gamemodes.zip/... adds a portal for it at FR06's trigger
    (where the seed's car stands; FR06's own trigger is switched off), and,
    unless --no-autopilot, the race flow hands the car to the title's own AI
    at the grid (the create-rally-stage-seed.py --autopilot node).

Then it rebuilds the seed's generated mods (build-mod-patches.py,
build-mod-archives.py) from the player's files. Nothing from the disc is
written; the source seed is only read. Drive it with
config/render-tests/fh1-oracle-lap.fh1test.

  create-oracle-lap-seed.py --seed .local/render-seeds/acc9-stock \\
      --output .local/render-seeds/m3-lap-r34
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import sqlite3
import subprocess
import sys
import xml.etree.ElementTree as ET
from contextlib import closing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOD = "m3_oracle_lap"
EVENT = "M3_ORACLE_LAP"
TEMPLATE = "FR06"


def tool(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()


def lap_sql(db: sqlite3.Connection, track: int, laps: int) -> str:
    """INSERT ... SELECT statements that copy the template's rows; the script
    names columns only, so it carries no game data."""
    def columns(table):
        return [row[1] for row in db.execute(f"PRAGMA table_info({table})")]
    event_id = f"(SELECT Id FROM Events WHERE HorizonEventID='{EVENT}')"
    template_id = f"(SELECT Id FROM Events WHERE HorizonEventID='{TEMPLATE}')"
    def copy(table, where, changes):
        names = columns(table)
        values = [changes.get(name, name) for name in names]
        return (f"INSERT INTO {table} ({', '.join(names)})\n  SELECT {', '.join(values)}\n"
                f"  FROM {table} WHERE {where};\n")
    script = ["-- M3 oracle lap (tools/create-oracle-lap-seed.py): a solo copy of "
              f"{TEMPLATE} on track {track}, {laps} lap(s).\n"]
    script.append(copy("Events", f"HorizonEventID='{TEMPLATE}'", {
        "Id": "(SELECT MAX(Id) + 1 FROM Events)", "HorizonEventID": f"'{EVENT}'",
        "NumberOfDrivers": "0", "PlayerGridPosition": "0", "NemesisGridPosition": "0",
        "CashPrize": "0", "UnlockPointsReq": "0", "Level": "0"}))
    script.append(copy("Races", f"HorizonEventID='{TEMPLATE}'", {
        "Id": "(SELECT MAX(Id) + 1 FROM Races)", "EventId": event_id,
        "HorizonEventID": f"'{EVENT}'", "TrackId": str(track), "NumLaps": str(laps)}))
    for table in ("EventUIColors", "Event_Music", "EventRecommendedCars"):
        names = columns(table)
        key = next(name for name in names if name.lower() == "eventid")
        changes = {key: event_id}
        if "Id" in names:
            changes["Id"] = f"(SELECT MAX(Id) FROM {table}) + 1"
        script.append(copy(table, f"{key}={template_id}", changes))
    # No EventRestrictions row: with none, every limit stays at "none"
    # (SPEC-EVENT-RESTRICTIONS), so the seed's car is always eligible.
    return "".join(script)


def portal(activities: bytes, career: bytes) -> dict[str, bytes]:
    root = ET.fromstring(activities)
    activity = ET.SubElement(root, "Activity", type="ActivityGeneric", name="m3_oracle_lap")
    group = ET.SubElement(activity, "StateGroup", id="m3_oracle_lap")
    state = ET.SubElement(group, "State", id="Root")
    ET.SubElement(state, "Entry", id="enter")
    behaviour = ET.SubElement(state, "Behaviour", id="CLoadIntoCareerRace", event_id=EVENT)
    ET.SubElement(behaviour, "Attribute", id="resource_package", value="CareerLoading")
    ET.SubElement(activity, "TriggerZone", object=f"{TEMPLATE}_NODE", name="m3_oracle_lap",
                  radius="25", maxMPH="150", prompt="IDS_Description_MediaCenter",
                  mapTag="mediacentertodo", mapDescription="IDS_Description_MediaCenter")
    events = ET.fromstring(career)
    events.find(f"Activity[@name='{TEMPLATE}']/TriggerZone").set("radius", "0")
    return {"Colorado/activities.xml": ET.tostring(root, encoding="utf-8"),
            "Colorado/career_event_activations.xml": ET.tostring(events, encoding="utf-8")}


def create(args) -> Path:
    runner = tool("run-fh1-render-test")
    rally_seed = tool("create-rally-stage-seed")
    source, output = args.seed.resolve(), args.output.resolve()
    if os.environ.get("LOCALAPPDATA") and output.is_relative_to(
            (Path(os.environ["LOCALAPPDATA"]) / "PinyonShift").resolve()):
        raise ValueError("use a private output outside the AppData save")
    if output.exists() or output.is_relative_to(source):
        raise ValueError(f"refusing to overwrite or nest: {output}")
    manifest = json.loads((source / "seed.json").read_text(encoding="utf-8"))
    for relative, expected in manifest["profiles"].items():
        if sha256(source / relative) != expected:
            raise ValueError(f"pinned seed has changed: {relative}")
    modded = sorted((source / "user-modded").glob("**/ForzaProfile/ForzaProfile"))
    if not modded:
        raise ValueError("the seed has no modded profile (the car to test lives there)")
    game = args.game_root.resolve()
    with closing(sqlite3.connect(f"file:{game / 'media/db/gamedb.slt'}?mode=ro", uri=True)) as db:
        kind = db.execute("SELECT RibbonConfiguration, MediaName FROM Tracks WHERE id=?",
                          (args.track,)).fetchone()
        if kind is None or kind != (0, "Colorado"):
            raise ValueError(f"track {args.track} is not a Colorado circuit")
        script = lap_sql(db, args.track, args.laps)
    runner.prepare_isolated_state(source, output)
    mod = output / "mods" / MOD
    (mod / "db").mkdir(parents=True)
    (mod / "mod.toml").write_text(f'name = "{MOD}"\nversion = "0.1.0"\nabi = 1\n', encoding="utf-8")
    (mod / "db/10-oracle-lap.sql").write_text(script, encoding="utf-8")
    reference = output / "oracle-lap-reference"
    reference.mkdir()
    def extract(member, filename):
        target = reference / filename
        subprocess.run([str(args.extractor.resolve()), "--archive-member",
                        str(game / "media/gamemodes.zip"), member, str(target)],
                       check=True, capture_output=True, text=True)
        return target.read_bytes()
    replacements = portal(extract("Colorado/activities.xml", "activities.xml"),
                          extract("Colorado/career_event_activations.xml", "career.xml"))
    if not args.no_autopilot:
        replacements["game_festival_race_flow.xml"] = rally_seed.autopilot(
            extract("game_festival_race_flow.xml", "race-flow.xml"))
    for member, data in replacements.items():
        target = mod / "members/media/gamemodes.zip" / member
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    settings = output / "config/pinyon_shift.toml"
    text = settings.read_text(encoding="utf-8")
    current = re.search(r'(?m)^enabled_mods\s*=\s*"([^"]*)"', text)
    mods = [m for m in (current.group(1).split(",") if current else []) if m]
    mods.append(MOD)
    line = f'enabled_mods = "{",".join(mods)}"'
    text = re.sub(r'(?m)^enabled_mods\s*=.*$', line, text) if current else text + "\n" + line + "\n"
    settings.write_text(text, encoding="utf-8")
    python = sys.executable
    for name, extra in (("build-mod-patches", []),
                        ("build-mod-archives", ["--archive-extractor", str(args.extractor.resolve())])):
        subprocess.run([python, str(ROOT / "tools" / (name + ".py")), str(output),
                        "--game-root", str(game), *extra], check=True)
    (output / "seed.json").write_text(json.dumps(dict(
        manifest, note=f"M3 oracle lap: solo {EVENT} on track {args.track}, {args.laps} lap(s), "
                       f"autopilot {'off' if args.no_autopilot else 'on'}; from {source.name}",
        source=str(source),
        modded_profiles={str(p.relative_to(source)).replace("\\", "/"): sha256(p) for p in modded}),
        indent=2) + "\n", encoding="utf-8")
    (output / "oracle-lap.json").write_text(json.dumps(dict(
        event=EVENT, template=TEMPLATE, track_id=args.track, laps=args.laps,
        autopilot=not args.no_autopilot, mod=MOD, source_seed=str(source)), indent=2) + "\n",
        encoding="utf-8")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seed", type=Path, required=True, help="pinned seed with the car in its modded profile")
    parser.add_argument("--output", type=Path, required=True, help="new seed directory")
    parser.add_argument("--track", type=int, default=158, help="Colorado circuit Tracks.id")
    parser.add_argument("--laps", type=int, default=1)
    parser.add_argument("--no-autopilot", action="store_true",
                        help="leave the car to the route's inputs (the control run)")
    parser.add_argument("--game-root", type=Path, default=ROOT / ".local/game/base")
    parser.add_argument("--extractor", type=Path,
                        default=ROOT / "out/build/win-amd64-release/pinyon_shift_fh1_archive_extract.exe")
    args = parser.parse_args()
    try:
        print(create(args))
        return 0
    except (OSError, ValueError, sqlite3.Error, subprocess.SubprocessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
