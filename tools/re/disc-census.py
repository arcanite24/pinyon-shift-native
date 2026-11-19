#!/usr/bin/env python3
"""Census of the formats and bytes on the player's disc (REIMPLEMENTATION_PLAN RI-0.2).

Walks an extracted disc's `media/` folder and, optionally, the v4 title
update's `media.zip` and the installed DLC packages, and counts, per file
extension:

- loose files and members of every FH1 ZIP (central directory only: the
  sizes are read from the directory, nothing is decompressed);
- compressed and uncompressed bytes (a loose file counts its size as both);
- which archives hold them, and in how many distinct names (a name that
  occurs in several archives, or in several sources, counts once);
- how far the format is understood ("depth"), from the table below, which
  follows the Formats table of docs/REIMPLEMENTATION_PLAN.md.

It then sums the uncompressed bytes per depth level and per source, and
reconciles the scenery counts (`.bix`, `.bin`) by archive and name pattern.

FH1 ZIPs are ordinary ZIP central directories with XMem LZX payloads
(method 21) and an extra field 0x1123 that holds the absolute payload offset
(tools/patch-fh1-archive.py). Python's `zipfile` reads the directory; it
walks the directory by its size, so archives whose end record holds a
truncated entry count (Colorado's `bin.zip`) are still read in full.
Archives found as members of other archives are counted as members and
flagged; they are not opened.

Every input is opened read-only. The AppData save tree is never written.

    python -I tools/re/disc-census.py                       # base disc only
    python -I tools/re/disc-census.py --v4 --dlc            # with v4 and DLC
    python -I tools/re/disc-census.py --state-root <preview> --top 40

Output: JSON (default .local/re/disc-census.json) and a markdown table on
stdout. Only names, extensions and sizes are recorded, no file contents.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_STATE = Path(os.environ.get("LOCALAPPDATA", "")) / "PinyonShift/source/0.1.0/.local/preview"
DLC_CONTENT = PurePosixPath("user/0000000000000000/4D5309C9/00000002")
SCHEMA = "pinyon-shift.disc-census.v1"

# Depth known per extension, after the plan's Formats table:
#   read+write  a reader and a writer exist (round-trip or rebuild)
#   read        a reader decodes the content
#   header      the header or stream type is known, not the payload
#   container   the container is split, its contents are not decoded
#   named       the format is named and located, its layout is not known
#   none        nothing is decoded
# Extensions not in the table are reported as "unclassified".
DEPTH = {
    # Archives and manifests (tools/pinyon_setup.py, fh1_archive_extract.cpp).
    ".zip": "read+write",
    # Database, strings, fonts: vector fonts (.dt) and bitmap fonts (.sbm glyph
    # sheets, .abc code-point tables, .tex textures), site/findings/ui.toml.
    ".slt": "read+write", ".str": "read+write", ".dt": "read+write", ".sbm": "read+write",
    ".abc": "read+write", ".tex": "read+write",
    # Car meshes round-trip (tools/fh1_carbin.py); header and mass block opaque.
    ".carbin": "read+write",
    # Textures: base level on dev (src/ui/hostui/xds_texture.cpp).
    ".xds": "read", ".dds": "read", ".tga": "read",
    # Plain text formats.
    ".xml": "read", ".ini": "read", ".lst": "read", ".txt": "read", ".lua": "read",
    ".py": "read", ".csv": "read", ".sch": "read", ".xsd": "read", ".json": "read",
    ".html": "read", ".jpg": "read",
    # Title update patches: the SDK's XEX patch reader.
    ".xexp": "read",
    # FSB4 banks: read; radio read + write.
    ".fsb": "read",
    # Movies: decoded by FFmpeg (RI-0.6, tools/re/media-decode-check.py).
    ".wmv": "read",
    # Effects: microcode and vertex declarations split out.
    ".fxobj": "container",
    # UI4 scenes (.bgf with .fbf objects and .bsg scene graph): not in the plan's
    # table; site/findings/ui.toml records their headers and part of the graph.
    ".bgf": "header", ".fbf": "header", ".bsg": "header",
    # Visibility: named only (with the ribbons' PVSZLookup, FilenameMap and
    # PVSZoneSpeeds .dat files).
    ".pvs": "named", ".pvsz": "named", ".dat": "named",
    # World, models, collision, AI, audio events: nothing decoded.
    ".bix": "none", ".bin": "none", ".pgeo": "none", ".fiz": "none", ".sh": "none",
    ".rmb": "none", ".gr2": "none", ".skinbin": "none", ".xpr": "none",
    ".nav": "none", ".owr": "none", ".oww": "none", ".col": "none", ".crowd": "none",
    ".soundscape": "none", ".fev": "none",
    # Not in the plan's table, nothing decoded: AI routes (.owt), Colorado
    # bin.zip bundles, ribbon .hex files, speech grammars, movie definitions.
    ".owt": "none", ".bundle": "none", ".hex": "none", ".cfgp": "none", ".def": "none",
}
# Formats that describe the world (geometry, visibility, collision, AI, ambience).
# WORLD_PLAN is the plan's list (Formats table rows for world geometry, PVS and
# the world's AI, collision, crowd and ambience files); WORLD adds the formats
# found only in Colorado's bin.zip and ribbons (.bundle, .hex) and the AI routes.
WORLD_PLAN = (".bix", ".bin", ".pgeo", ".fiz", ".sh", ".rmb", ".pvs", ".pvsz", ".dat", ".nav",
              ".owr", ".oww", ".col", ".crowd", ".soundscape")
WORLD = WORLD_PLAN + (".bundle", ".hex", ".owt")
DEPTH_ORDER = ("read+write", "read", "header", "container", "named", "none", "unclassified")
SCENERY_NAME = re.compile(r"_0x[0-9a-f]{8}_[a-z]\.bix$", re.IGNORECASE)


def extension(name: str) -> str:
    suffix = PurePosixPath(name.replace("\\", "/")).suffix.lower()
    return suffix if suffix else "(none)"


def inner_extension(name: str) -> str | None:
    """The second suffix of a compound name, such as `.rmb` in `x.rmb.bin`."""
    suffixes = PurePosixPath(name.replace("\\", "/")).suffixes
    if len(suffixes) >= 2 and re.fullmatch(r"\.[A-Za-z][A-Za-z0-9]{0,9}", suffixes[-2]):
        return suffixes[-2].lower()
    return None


class Census:
    def __init__(self) -> None:
        self.ext = collections.defaultdict(lambda: {
            "files": 0, "loose": 0, "members": 0, "compressed": 0, "uncompressed": 0,
            "archives": collections.Counter(), "sources": collections.Counter(),
            "inner": collections.Counter(), "names": set(),
            "unique": 0, "unique_compressed": 0, "unique_uncompressed": 0})
        self.contents: set = set()
        self.archives: list[dict] = []
        self.nested: list[dict] = []
        self.errors: list[dict] = []
        self.sources = collections.defaultdict(lambda: {"files": 0, "compressed": 0,
                                                        "uncompressed": 0})
        self.methods = collections.Counter()
        self.by_group = collections.defaultdict(lambda: [0, 0, 0])
        self.scenery = collections.defaultdict(lambda: collections.Counter())

    def add(self, source: str, archive: str | None, name: str, compressed: int,
            uncompressed: int, content: tuple | None = None) -> None:
        """One file or member. `content` identifies its bytes (CRC-32 and size for
        members); repeated content is counted once in the `unique` columns."""
        ext = extension(name)
        row = self.ext[ext]
        key = (ext, content) if content is not None else (ext, source, archive, name)
        if key not in self.contents:
            self.contents.add(key)
            row["unique"] += 1
            row["unique_compressed"] += compressed
            row["unique_uncompressed"] += uncompressed
        row["files"] += 1
        row["loose" if archive is None else "members"] += 1
        row["compressed"] += compressed
        row["uncompressed"] += uncompressed
        family = archive_family(archive) if archive else "(loose)"
        row["archives"][family] += 1
        row["sources"][source] += 1
        inner = inner_extension(name)
        if inner:
            row["inner"][inner] += 1
        row["names"].add(name.replace("\\", "/").lower())
        group = self.by_group[(DEPTH.get(ext, "unclassified"), source.split("/")[0])]
        group[0] += 1
        group[1] += compressed
        group[2] += uncompressed
        totals = self.sources[source]
        totals["files"] += 1
        totals["compressed"] += compressed
        totals["uncompressed"] += uncompressed
        if ext in (".bix", ".bin"):
            key = f"{source}:{archive or '(loose)'}"
            self.scenery[key][ext] += 1
            if ext == ".bix" and SCENERY_NAME.search(name):
                self.scenery[key]["bix_scenery_pattern"] += 1


def archive_family(path: str) -> str:
    """An archive's family: its file name, with the track or car folder generalised."""
    parts = PurePosixPath(path.replace("\\", "/")).parts
    lower = [p.lower() for p in parts]
    name = lower[-1]
    if "tracks" in lower:
        i = lower.index("tracks")
        track = lower[i + 1] if i + 1 < len(lower) - 1 else ""
        return f"tracks/{track}/{name}" if track == "colorado" else f"tracks/*/{name}"
    if "cars" in lower:
        return f"cars/*/{name}" if len(lower) > lower.index("cars") + 2 else f"cars/{name}"
    if len(lower) > 2:
        return f"{lower[-2]}/{name}" if lower[0] == "media" and len(lower) == 3 else \
            f"{lower[1] if lower[0] == 'media' else lower[0]}/.../{name}"
    return "/".join(lower)


def repeat_layout(files: list[zipfile.ZipInfo]) -> dict:
    """How repeated names are laid out: the index of the first repeat, the
    runs of entries in which every name occurs once (a new run starts at a
    name already seen in the current run), and whether every repeat carries
    the same CRC-32 and size as the first entry of that name."""
    first: dict[str, tuple[int, int]] = {}
    first_repeat = None
    runs, current, sizes, start = 1, set(), [], 0
    same = differ = 0
    for index, info in enumerate(files):
        name = info.filename.replace("\\", "/").lower()
        if name in current:
            runs += 1
            sizes.append(index - start)
            start = index
            current = set()
        current.add(name)
        if name in first:
            if first_repeat is None:
                first_repeat = index
            if first[name] == (info.CRC, info.file_size):
                same += 1
            else:
                differ += 1
        else:
            first[name] = (info.CRC, info.file_size)
    sizes.append(len(files) - start)
    return {"first_repeat_index": first_repeat, "unique_name_runs": runs,
            "unique_name_run_sizes": sizes if 1 < runs <= 256 else None,
            "repeats_same_content": same, "repeats_other_content": differ}


def read_archive(census: Census, source: str, path: Path, label: str) -> None:
    try:
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            end_count = None
            try:
                # The end record's own entry count (16 bits), to compare with the directory.
                with path.open("rb") as handle:
                    handle.seek(0, 2)
                    size = handle.tell()
                    handle.seek(max(0, size - 65557))
                    tail = handle.read()
                    at = tail.rfind(b"PK\x05\x06")
                    if at >= 0:
                        end_count = int.from_bytes(tail[at + 10:at + 12], "little")
            except OSError:
                pass
    except (OSError, zipfile.BadZipFile) as error:
        census.errors.append({"source": source, "path": label, "error": str(error)})
        return
    files = [i for i in infos if not i.is_dir()]
    names = collections.Counter(i.filename.replace("\\", "/").lower() for i in files)
    record = {"source": source, "path": label, "family": archive_family(label),
              "bytes": path.stat().st_size, "entries": len(infos), "members": len(files),
              "directories": len(infos) - len(files),
              "end_record_count": end_count,
              "duplicate_names": sum(c - 1 for c in names.values() if c > 1),
              "distinct_names": len(names),
              "distinct_contents": len({(i.CRC, i.file_size) for i in files}),
              "compressed": 0, "uncompressed": 0, "methods": collections.Counter(),
              **repeat_layout(files)}
    for info in files:
        census.add(source, label, info.filename, info.compress_size, info.file_size,
                   (info.CRC, info.file_size))
        record["compressed"] += info.compress_size
        record["uncompressed"] += info.file_size
        record["methods"][str(info.compress_type)] += 1
        census.methods[str(info.compress_type)] += 1
        if extension(info.filename) == ".zip":
            census.nested.append({"source": source, "archive": label, "member": info.filename,
                                  "method": info.compress_type, "bytes": info.file_size})
    record["methods"] = dict(record["methods"])
    census.archives.append(record)


def walk(census: Census, source: str, root: Path, prefix: str = "") -> None:
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for filename in sorted(filenames):
            path = Path(dirpath) / filename
            label = prefix + path.relative_to(root).as_posix()
            size = path.stat().st_size
            if filename.lower().endswith(".zip"):
                # The archive itself is a container on disc; its members are the data.
                read_archive(census, source, path, label)
                census.sources[source]["archive_files"] = \
                    census.sources[source].get("archive_files", 0) + 1
                census.sources[source]["archive_bytes"] = \
                    census.sources[source].get("archive_bytes", 0) + size
            else:
                census.add(source, None, label, size, size)


def gb(value: int) -> str:
    return f"{value / 1e9:.2f}"


def summarise(census: Census, top: int) -> dict:
    total_u = sum(r["uncompressed"] for r in census.ext.values())
    total_c = sum(r["compressed"] for r in census.ext.values())
    unique_u = sum(r["unique_uncompressed"] for r in census.ext.values())
    unique_c = sum(r["unique_compressed"] for r in census.ext.values())
    rows = []
    for ext, r in census.ext.items():
        rows.append({
            "extension": ext, "depth": DEPTH.get(ext, "unclassified"),
            "files": r["files"], "loose": r["loose"], "members": r["members"],
            "distinct_names": len(r["names"]),
            "compressed_bytes": r["compressed"], "uncompressed_bytes": r["uncompressed"],
            "share_uncompressed": r["uncompressed"] / total_u if total_u else 0,
            "unique_contents": r["unique"],
            "unique_compressed_bytes": r["unique_compressed"],
            "unique_uncompressed_bytes": r["unique_uncompressed"],
            "share_unique_uncompressed": r["unique_uncompressed"] / unique_u if unique_u else 0,
            "families": dict(r["archives"].most_common(8)),
            "family_count": len(r["archives"]),
            "sources": dict(r["sources"]),
            "inner_extensions": dict(r["inner"].most_common(8)),
        })
    rows.sort(key=lambda r: -r["uncompressed_bytes"])
    depth = {d: {"extensions": 0, "files": 0, "compressed_bytes": 0, "uncompressed_bytes": 0,
                 "unique_contents": 0, "unique_compressed_bytes": 0,
                 "unique_uncompressed_bytes": 0} for d in DEPTH_ORDER}
    for r in rows:
        d = depth[r["depth"]]
        d["extensions"] += 1
        d["files"] += r["files"]
        d["compressed_bytes"] += r["compressed_bytes"]
        d["uncompressed_bytes"] += r["uncompressed_bytes"]
        d["unique_contents"] += r["unique_contents"]
        d["unique_compressed_bytes"] += r["unique_compressed_bytes"]
        d["unique_uncompressed_bytes"] += r["unique_uncompressed_bytes"]
    for d in depth.values():
        d["share_unique_uncompressed"] = d["unique_uncompressed_bytes"] / unique_u if unique_u else 0
        d["share_unique_compressed"] = d["unique_compressed_bytes"] / unique_c if unique_c else 0
        d["share_uncompressed"] = d["uncompressed_bytes"] / total_u if total_u else 0
        d["share_compressed"] = d["compressed_bytes"] / total_c if total_c else 0
    world = WORLD
    plan = {k: sum(r[k] for r in rows if r["extension"] in WORLD_PLAN)
            for k in ("uncompressed_bytes", "compressed_bytes", "unique_uncompressed_bytes",
                      "unique_compressed_bytes")}
    plan.update({"extensions": list(WORLD_PLAN),
                 "share_uncompressed": plan["uncompressed_bytes"] / total_u if total_u else 0,
                 "share_compressed": plan["compressed_bytes"] / total_c if total_c else 0,
                 "share_unique_uncompressed": plan["unique_uncompressed_bytes"] / unique_u if unique_u else 0,
                 "share_unique_compressed": plan["unique_compressed_bytes"] / unique_c if unique_c else 0})
    world_u = sum(r["uncompressed_bytes"] for r in rows if r["extension"] in world)
    world_c = sum(r["compressed_bytes"] for r in rows if r["extension"] in world)
    world_uu = sum(r["unique_uncompressed_bytes"] for r in rows if r["extension"] in world)
    world_uc = sum(r["unique_compressed_bytes"] for r in rows if r["extension"] in world)
    colorado = [a for a in census.archives if a["family"] == "tracks/colorado/bin.zip"]
    return {
        "schema": SCHEMA,
        "totals": {"files": sum(r["files"] for r in rows), "compressed_bytes": total_c,
                   "uncompressed_bytes": total_u, "extensions": len(rows),
                   "unique_contents": sum(r["unique_contents"] for r in rows),
                   "unique_compressed_bytes": unique_c, "unique_uncompressed_bytes": unique_u,
                   "archives": len(census.archives)},
        "sources": {k: dict(v) for k, v in census.sources.items()},
        "methods": dict(census.methods),
        "depth": depth,
        "depth_by_source": {f"{d}/{g}": {"files": v[0], "compressed_bytes": v[1],
                                         "uncompressed_bytes": v[2]}
                            for (d, g), v in sorted(census.by_group.items())},
        "world_formats": {"extensions": list(world), "uncompressed_bytes": world_u,
                          "compressed_bytes": world_c,
                          "share_uncompressed": world_u / total_u if total_u else 0,
                          "share_compressed": world_c / total_c if total_c else 0,
                          "unique_uncompressed_bytes": world_uu, "unique_compressed_bytes": world_uc,
                          "share_unique_uncompressed": world_uu / unique_u if unique_u else 0,
                          "share_unique_compressed": world_uc / unique_c if unique_c else 0,
                          "colorado_bin_zip": colorado},
        "world_formats_plan_list": plan,
        "scenery_by_archive": {k: dict(v) for k, v in sorted(
            census.scenery.items(), key=lambda kv: -sum(kv[1].values()))},
        "nested_archives": census.nested,
        "archives": sorted(census.archives, key=lambda a: -a["bytes"]),
        "extensions": rows,
        "errors": census.errors,
        "top": top,
    }


def markdown(result: dict, top: int) -> str:
    t = result["totals"]
    lines = [f"Files and members: {t['files']:,}; uncompressed {gb(t['uncompressed_bytes'])} GB, "
             f"stored {gb(t['compressed_bytes'])} GB; {t['archives']} archives. Unique contents "
             f"(CRC-32 and size): {t['unique_contents']:,}, {gb(t['unique_uncompressed_bytes'])} GB "
             f"uncompressed, {gb(t['unique_compressed_bytes'])} GB stored.", "",
             "| Extension | Depth | Files | Loose | Members | Unique | Stored GB | Uncompressed GB | Share | Unique GB | Unique share | Families |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for r in result["extensions"][:top]:
        lines.append(f"| `{r['extension']}` | {r['depth']} | {r['files']:,} | {r['loose']:,} | "
                     f"{r['members']:,} | {r['unique_contents']:,} | {gb(r['compressed_bytes'])} | "
                     f"{gb(r['uncompressed_bytes'])} | {100 * r['share_uncompressed']:.1f} % | "
                     f"{gb(r['unique_uncompressed_bytes'])} | {100 * r['share_unique_uncompressed']:.1f} % | "
                     f"{r['family_count']} |")
    lines += ["", "| Depth | Extensions | Files | Uncompressed GB | Share | Stored share | Unique GB | Unique share | Unique stored share |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for name, d in result["depth"].items():
        lines.append(f"| {name} | {d['extensions']} | {d['files']:,} | {gb(d['uncompressed_bytes'])} | "
                     f"{100 * d['share_uncompressed']:.1f} % | {100 * d['share_compressed']:.1f} % | "
                     f"{gb(d['unique_uncompressed_bytes'])} | {100 * d['share_unique_uncompressed']:.1f} % | "
                     f"{100 * d['share_unique_compressed']:.1f} % |")
    w = result["world_formats"]
    lines += ["", f"World formats: {gb(w['uncompressed_bytes'])} GB uncompressed "
              f"({100 * w['share_uncompressed']:.1f} %), {gb(w['compressed_bytes'])} GB stored "
              f"({100 * w['share_compressed']:.1f} %); unique contents {gb(w['unique_uncompressed_bytes'])} GB "
              f"({100 * w['share_unique_uncompressed']:.1f} %), stored {gb(w['unique_compressed_bytes'])} GB "
              f"({100 * w['share_unique_compressed']:.1f} %)."]
    w = result["world_formats_plan_list"]
    lines += [f"World formats, the plan's list only (no .bundle, .hex, .owt): "
              f"{100 * w['share_uncompressed']:.1f} % uncompressed, {100 * w['share_compressed']:.1f} % stored; "
              f"unique contents {100 * w['share_unique_uncompressed']:.1f} %, stored "
              f"{100 * w['share_unique_compressed']:.1f} %."]
    lines += ["", "| Archive | Entries | End-record count | Distinct names | Distinct contents | First repeat | Unique-name runs |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for a in result["archives"]:
        if a["duplicate_names"] or (a["end_record_count"] not in (None, a["entries"])):
            lines.append(f"| {a['source']}:{a['path']} | {a['entries']:,} | {a['end_record_count']} | "
                         f"{a['distinct_names']:,} | {a['distinct_contents']:,} | "
                         f"{a['first_repeat_index']} | {a['unique_name_runs']} |")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--media", type=Path, default=ROOT / ".local/game/base/media",
                        help="the extracted disc's media folder")
    parser.add_argument("--state-root", type=Path, default=DEFAULT_STATE,
                        help="installed preview state (read-only) for --v4 and --dlc")
    parser.add_argument("--v4", action="store_true", help="add title-update-v4 (media.zip)")
    parser.add_argument("--dlc", action="store_true", help="add the installed DLC packages")
    parser.add_argument("--out", type=Path, default=ROOT / ".local/re/disc-census.json")
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args()

    census = Census()
    if not args.media.is_dir():
        print(f"no media folder at {args.media}", file=sys.stderr)
        return 1
    walk(census, "base", args.media, "media/")
    if args.v4:
        folder = args.state_root / "title-update-v4"
        if not folder.is_dir():
            print(f"no v4 title update at {folder}", file=sys.stderr)
            return 1
        walk(census, "v4", folder)
    if args.dlc:
        content = args.state_root / DLC_CONTENT
        for package in sorted(p for p in content.iterdir() if p.is_dir()):
            walk(census, f"dlc/{package.name}", package)

    result = summarise(census, args.top)
    result["inputs"] = {"media": str(args.media), "v4": args.v4, "dlc": args.dlc}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(result, handle, indent=1)
        handle.write("\n")
    print(markdown(result, args.top))
    print(f"\nwrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
