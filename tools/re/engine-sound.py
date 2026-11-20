#!/usr/bin/env python3
"""Where each car's engine sound lives, and which loops play (SPEC-ENGINE-SOUND).

Reads the player's own files, read-only, and writes only names, counts and
the loop choice the spec's tests quote:

- the merged game database `.local/re/cars/db/merged.slt` (tools/re/car-list.py
  builds it: base, v4 and the installed packages);
- the tuning archives `media/audio/cars/CarModelTuning.zip`,
  `media/audio/cars/Engines/EngineTuning.zip`, `HarmonicTuning.zip` and
  `DSPTuning.zip` on the disc, the same folders in each package's
  `Media/DLCZips/*_pri_*.zip`, and v4's `media.zip` `audio/cars/...`
  members (v4 > packages by priority > disc, as for other members);
- the FSB4 engine banks in `media/audio/cars/Engines/Soundbanks/LOD1/` on the
  disc and in each package's loose `Media/DLCZips/<n>_pri_65/Media/audio/...`.

The chain is `Data_Car.MediaName` -> `<MediaName>_ET.xml` (engine tuning) and
`<MediaName>_CMT.xml` (emitters and listener levels) -> one harmonic tuning
per emission group and upgrade level (`Upgrade L0..L3`) -> its
`Soundbank name` -> the `.fsb`. Names match case-insensitively.

    python -I tools/re/engine-sound.py census --state-root <preview>
    python -I tools/re/engine-sound.py loops --state-root <preview> --car 445 --rpm 4000

`census` prints and saves the per-hop counts (default
`.local/re/engine-sound.json`). `loops` maps a physics rpm to the audio rpm
(the spec's rpm map) and lists the loops that sound in each emission group,
their crossfade gain and pitch ratio; its output stays on the terminal.
Members are decompressed once into `.local/re/audio/` with the project's LZX
extractor (`pinyon_shift_fh1_archive_extract`).
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import os
import sqlite3
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MARKETPLACE = Path("user/0000000000000000/4D5309C9/00000002")
UDLC = "4D5309C900000729"
FOLDERS = {"carmodeltuning": "cmt", "engines/enginetuning": "et",
           "engines/harmonictuning": "ht", "engines/dsptuning": "dsp"}
ARCHIVES = {"CarModelTuning.zip": "cmt", "Engines/EngineTuning.zip": "et",
            "Engines/HarmonicTuning.zip": "ht", "Engines/DSPTuning.zip": "dsp"}
GROUPS = ("EngineIntake", "EngineAmbient", "Exhaust")
# Emission groups in the order the loop sets are kept (spec, "Emission groups").
LOOP_SETS = {"EngineIntake": ["EngineIntake"], "EngineAmbient": ["EngineAmbient"],
             "Exhaust": ["ExhaustL", "ExhaustR"]}
LAST_LOOP_EDGE = 32000.0   # the rpm cap the loader applies; the last loop never fades out


# ---- archives -------------------------------------------------------------------


def read_member(archive: Path, info: zipfile.ZipInfo, data: bytes, extractor: Path, out: Path) -> None:
    head = struct.unpack_from("<IHHHHHIIIHH", data, info.header_offset)
    body = info.header_offset + 30 + head[9] + head[10]
    out.parent.mkdir(parents=True, exist_ok=True)
    if info.compress_type == 0:
        out.write_bytes(data[body:body + info.compress_size])
    elif info.compress_type == 8:
        out.write_bytes(zlib.decompress(data[body:body + info.compress_size], -15))
    elif info.compress_type == 21:
        subprocess.run([str(extractor), str(archive), str(body), str(info.compress_size),
                        str(info.file_size), str(out)], check=True, capture_output=True)
    else:
        raise ValueError(f"{archive}:{info.filename}: compression {info.compress_type}")


def unpack(archive: Path, cache: Path, extractor: Path, prefix: str = "") -> dict[tuple[str, str], Path]:
    """Members of one archive under the audio/cars folders, by (kind, lower name)."""
    found = {}
    data = archive.read_bytes()
    for info in zipfile.ZipFile(archive).infolist():
        if info.is_dir():
            continue
        name = info.filename.replace("\\", "/")
        low = name.lower()
        if prefix:
            if not low.startswith(prefix):
                continue
            folder, _, leaf = low[len(prefix):].rpartition("/")
            kind = FOLDERS.get(folder)
        else:
            kind, leaf = "", low
        if kind is None:
            continue
        out = cache / name
        if not out.exists():
            read_member(archive, info, data, extractor, out)
        found[(kind, leaf)] = out
    return found


class Files:
    """Tuning members and banks, later sources winning (v4 > packages > disc)."""

    def __init__(self, game: Path, state: Path, extractor: Path, cache: Path, with_udlc: bool) -> None:
        self.xml: dict[tuple[str, str], Path] = {}
        self.origin: dict[tuple[str, str], str] = {}
        self.banks: dict[str, Path] = {}
        cars = game / "media/audio/cars"
        for rel, kind in ARCHIVES.items():
            for (_, leaf), path in unpack(cars / rel, cache / "base" / kind, extractor).items():
                self.xml[(kind, leaf)] = path
                self.origin[(kind, leaf)] = "base"
        for bank in (cars / "Engines/Soundbanks/LOD1").glob("*.fsb"):
            self.banks[bank.name.lower()] = bank
        self.packages = []
        root = state / MARKETPLACE
        archives = []
        for folder in sorted(p for p in root.iterdir() if p.is_dir()) if root.is_dir() else []:
            if folder.name == UDLC and not with_udlc:
                continue
            for archive in (folder / "Media" / "DLCZips").glob("*_pri_*.zip"):
                archives.append((int(archive.stem.rsplit("_pri_", 1)[1]), folder.name, archive))
            loose = folder / "Media" / "DLCZips"
            for bank in loose.glob("*/Media/audio/cars/Engines/Soundbanks/LOD1/*.fsb"):
                self.banks[bank.name.lower()] = bank
        for _, package, archive in sorted(archives):
            try:
                members = unpack(archive, cache / "dlc" / package / archive.stem, extractor, "media/audio/cars/")
            except zipfile.BadZipFile:
                continue
            for key, path in members.items():
                self.xml[key] = path
                self.origin[key] = package
            if members:
                self.packages.append(package)
        update = state / "title-update-v4/media.zip"
        if update.is_file():
            for key, path in unpack(update, cache / "v4", extractor, "audio/cars/").items():
                self.xml[key] = path
                self.origin[key] = "v4"

    def get(self, kind: str, name: str) -> Path | None:
        return self.xml.get((kind, name.lower()))


# ---- FSB4 -----------------------------------------------------------------------


def bank_header(path: Path) -> dict:
    """Sample count, channels and rates of an FSB4 bank (media-decode-check.py layout)."""
    with path.open("rb") as handle:
        head = handle.read(48)
        if head[:4] != b"FSB4":
            raise ValueError(f"{path}: not FSB4")
        count, size = struct.unpack_from("<II", head, 4)
        headers = handle.read(size)
    samples, at = [], 0
    for _ in range(count):
        (record,) = struct.unpack_from("<H", headers, at)
        name = headers[at + 2:at + 32].split(b"\0", 1)[0].decode("latin-1")
        length, _, loop_start, loop_end, mode, rate = struct.unpack_from("<IIIIIi", headers, at + 32)
        (channels,) = struct.unpack_from("<H", headers, at + 62)
        samples.append({"name": name, "length": length, "loop": (loop_start, loop_end), "mode": mode,
                        "rate": rate, "channels": channels, "record": record})
        at += record
    return {"count": count, "samples": samples}


# ---- tuning files ---------------------------------------------------------------


def parse(path: Path) -> ET.Element:
    text = path.read_bytes().decode("utf-8", errors="replace").lstrip("﻿\r\n\t ")
    return ET.fromstring(text)


def child(node: ET.Element | None, tag: str) -> ET.Element | None:
    if node is None:
        return None
    return next((c for c in node if c.tag.lower() == tag.lower()), None)


def et_levels(root: ET.Element) -> dict[str, list[str]]:
    """Harmonic tuning names per emission group, index = upgrade level."""
    out = {}
    for group in GROUPS:
        upgrade = child(child(root, group), "Upgrade")
        names = []
        if upgrade is not None:
            for level in range(4):
                names.append(upgrade.get(f"L{level}", ""))
        out[group] = names
    return out


def ht_loops(root: ET.Element) -> dict[str, list[dict]]:
    sets = {}
    for group, names in LOOP_SETS.items():
        for name in names:
            node = child(root, name)
            if node is None:
                continue
            loops = []
            for loop in node:
                if loop.tag != "Loop":
                    continue
                loops.append({key: float(loop.get(key, "0")) for key in
                              ("wavebankindex", "rpm_min", "rpm_sample", "rpm_max", "volume",
                               "pitch", "pitchMin", "pitchMax")})
            sets[name] = loops
    return sets


# ---- the rule (SPEC-ENGINE-SOUND "Loop crossfade and pitch") --------------------


def audio_rpm(rpm: float, idle: float, redline: float, audio_idle: float, audio_redline: float) -> tuple[float, float]:
    """The physics rpm mapped into the recording's rpm range, and the normalised rpm."""
    n = (rpm - idle) / (redline - idle)
    return audio_idle + (audio_redline - audio_idle) * n, n


def loop_state(loops: list[dict], rpm: float) -> list[tuple[int, float, float]]:
    """(loop position, crossfade gain, pitch ratio) for each loop that sounds."""
    out = []
    last = len(loops) - 1
    for i, loop in enumerate(loops):
        lo, hi, rec = loop["rpm_min"], loop["rpm_max"], loop["rpm_sample"]
        if rpm < lo or rpm > hi:
            continue
        fade_in_end = 0.0 if i == 0 else loops[i - 1]["rpm_max"]
        fade_out_start = LAST_LOOP_EDGE if i == last else loops[i + 1]["rpm_min"]
        if rpm < fade_in_end:
            t = (rpm - lo) / (fade_in_end - lo)
            gain = math.log2(1.0 + t)
        elif rpm > fade_out_start:
            t = (hi - rpm) / (hi - fade_out_start)
            gain = math.log2(1.0 + t)
        else:
            gain = 1.0
        if rpm < rec:
            pitch = loop["pitch"] + loop["pitchMin"] * (rpm - rec) / rec
        else:
            pitch = loop["pitch"] + loop["pitchMax"] * (rpm - rec) / rec
        out.append((i, gain, pitch))
    return out


# ---- commands -------------------------------------------------------------------


def stock_rpm_range(db: sqlite3.Connection, car: int) -> tuple[float, float] | None:
    row = db.execute(
        "SELECT c.StallRPM, c.RedlineRPM FROM List_UpgradeEngine e JOIN List_UpgradeEngineCamshaft c "
        "ON c.EngineID = e.EngineID AND c.IsStock = 1 WHERE e.Ordinal = ? AND e.IsStock = 1", (car,)).fetchone()
    return (row[0], row[1]) if row else None


def resolve(files: Files, media: str) -> dict:
    result = {"media": media, "et": None, "cmt": None, "groups": {}}
    et_path = files.get("et", f"{media}_et.xml")
    cmt_path = files.get("cmt", f"{media}_cmt.xml")
    result["cmt"] = files.origin.get(("cmt", f"{media}_cmt.xml".lower())) if cmt_path else None
    if not et_path:
        return result
    result["et"] = files.origin.get(("et", f"{media}_et.xml".lower()))
    root = parse(et_path)
    settings = child(root, "EngineSettings")
    result["audio_rpm"] = (float(settings.get("audio_rpm_idle", "0")),
                           float(settings.get("audio_rpm_redline", "0"))) if settings is not None else None
    for group, names in et_levels(root).items():
        levels = []
        for name in names:
            entry = {"ht": name, "ht_found": False, "bank": None, "bank_found": False, "loops": {}}
            ht = files.get("ht", name) if name else None
            if ht:
                entry["ht_found"] = True
                ht_root = parse(ht)
                entry["bank"] = ht_root.get("name", "")
                entry["loops"] = ht_loops(ht_root)
                bank = files.banks.get(entry["bank"].lower())
                if bank:
                    entry["bank_found"] = True
                    entry["bank_path"] = bank
            levels.append(entry)
        result["groups"][group] = levels
    return result


def open_files(args) -> Files:
    return Files(args.game, args.state_root, args.extractor, args.cache, args.with_udlc)


def cmd_census(args) -> int:
    files = open_files(args)
    db = sqlite3.connect(f"file:{args.db.as_posix()}?mode=ro", uri=True)
    rows = db.execute("SELECT Id, MediaName FROM Data_Car ORDER BY Id").fetchall()
    models = {}
    for car, media in rows:
        models.setdefault(media.lower(), (car, media))
    hop = collections.Counter()
    sources = collections.Counter()
    loops_per_set = collections.Counter()
    index_beyond = collections.Counter()
    missing = collections.defaultdict(list)
    banks_used = set()
    for key, (car, media) in sorted(models.items()):
        r = resolve(files, media)
        hop["models"] += 1
        hop["cmt"] += r["cmt"] is not None
        if r["et"] is None:
            missing["et"].append(media)
            continue
        hop["et"] += 1
        sources[r["et"]] += 1
        complete = True
        for group in GROUPS:
            levels = r["groups"].get(group) or []
            l0 = levels[0] if levels else None
            if not l0 or not l0["ht"]:
                hop[f"{group}.l0_absent"] += 1
                missing[f"{group}.l0"].append(media)
                complete = False
                continue
            hop[f"{group}.l0_named"] += 1
            if not l0["ht_found"]:
                missing[f"{group}.ht"].append(l0["ht"])
                complete = False
                continue
            hop[f"{group}.ht"] += 1
            if l0["bank"].lower() == "na.fsb":
                hop[f"{group}.na"] += 1
                continue
            if not l0["bank_found"]:
                missing[f"{group}.bank"].append(l0["bank"])
                complete = False
                continue
            hop[f"{group}.bank"] += 1
            banks_used.add(l0["bank"].lower())
            header = bank_header(l0["bank_path"])
            for name, loops in l0["loops"].items():
                loops_per_set[len(loops)] += 1
                for loop in loops:
                    index = int(loop["wavebankindex"])
                    if index >= header["count"]:
                        channel, sample = divmod(index, header["count"])
                        ok = sample < header["count"] and channel < header["samples"][sample]["channels"]
                        index_beyond["second channel" if ok else "out of range"] += 1
                    else:
                        index_beyond["direct"] += 1
            levels_named = sum(1 for e in levels if e["ht"])
            hop[f"{group}.levels_{levels_named}"] += 1
        hop["complete_l0"] += complete
    # Bank census over every engine bank (disc and packages).
    codecs, rates, channels, loops_full = collections.Counter(), collections.Counter(), collections.Counter(), 0
    samples = 0
    for bank in files.banks.values():
        for s in bank_header(bank)["samples"]:
            samples += 1
            codecs["xma" if s["mode"] & 0x01000000 else hex(s["mode"])] += 1
            rates[s["rate"]] += 1
            channels[s["channels"]] += 1
            loops_full += s["loop"] == (0, s["length"] - 1)
    dsp_used = 0
    result = {
        "schema": "pinyon-shift.engine-sound-census.v1",
        "packages": files.packages,
        "hops": dict(sorted(hop.items())),
        "et_source": dict(sources),
        "loops_per_set": dict(sorted(loops_per_set.items())),
        "loop_index": dict(index_beyond),
        "banks": {"files": len(files.banks), "used_by_l0": len(banks_used), "samples": samples,
                  "codecs": dict(codecs), "channels": dict(channels),
                  "rates_top": dict(rates.most_common(4)), "rates_distinct": len(rates),
                  "whole_sample_loops": loops_full},
        "tuning_members": dict(collections.Counter(kind for kind, _ in files.xml)),
        "missing": {k: sorted(set(v))[:20] for k, v in missing.items()},
        "missing_counts": {k: len(set(v)) for k, v in missing.items()},
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=1), encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "missing"}, indent=1))
    return 0


def cmd_loops(args) -> int:
    files = open_files(args)
    db = sqlite3.connect(f"file:{args.db.as_posix()}?mode=ro", uri=True)
    (media,) = db.execute("SELECT MediaName FROM Data_Car WHERE Id = ?", (args.car,)).fetchone()
    span = stock_rpm_range(db, args.car)
    r = resolve(files, media)
    print(f"car {args.car} {media}: physics idle/redline {span}, audio idle/redline {r.get('audio_rpm')}")
    rpm, n = audio_rpm(args.rpm, *span, *r["audio_rpm"])
    print(f"physics {args.rpm:.0f} rpm -> normalised {n:.3f}, audio {rpm:.0f} rpm")
    for group, levels in r["groups"].items():
        entry = levels[args.level] if args.level < len(levels) else None
        if not entry or not entry["ht_found"]:
            print(f"  {group}: no tuning")
            continue
        for name, loops in entry["loops"].items():
            state = loop_state(loops, rpm)
            text = ", ".join(f"loop {i} (recorded {loops[i]['rpm_sample']:.0f}) gain {g:.3f} pitch {p:.3f}"
                             for i, g, p in state)
            print(f"  {group}/{name} [{len(loops)} loops]: {text or 'silent'}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name, run in (("census", cmd_census), ("loops", cmd_loops)):
        p = sub.add_parser(name)
        p.add_argument("--state-root", type=Path, required=True,
                       help="preview state root with title-update-v4/ and user/ (read-only)")
        p.add_argument("--game", type=Path, default=REPO / ".local/game/base")
        p.add_argument("--db", type=Path, default=REPO / ".local/re/cars/db/merged.slt")
        p.add_argument("--extractor", type=Path,
                       default=REPO / "out/build/win-amd64-release/pinyon_shift_fh1_archive_extract.exe")
        p.add_argument("--cache", type=Path, default=REPO / ".local/re/audio")
        p.add_argument("--with-udlc", action="store_true")
        p.set_defaults(run=run)
    census = sub.choices["census"]
    census.add_argument("--out", type=Path, default=REPO / ".local/re/engine-sound.json")
    loops = sub.choices["loops"]
    loops.add_argument("--car", type=int, required=True)
    loops.add_argument("--rpm", type=float, required=True, help="physics engine rpm")
    loops.add_argument("--level", type=int, default=0, help="upgrade level (0 = stock)")
    args = parser.parse_args(argv)
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
