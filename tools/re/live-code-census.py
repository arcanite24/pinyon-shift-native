#!/usr/bin/env python3
"""Live-code census: which guest functions run, grouped by subsystem (RF-1).

docs/REIMPLEMENTATION_PLAN.md RI-0.1. A native reimplementation has to
replace the code the game runs in play, not every function in the image.
This joins four things the project already has:

  * the generated tree (`.local/generated*/default`): every translated
    function, its size in guest instructions (one comment line each) and
    its direct calls;
  * the generator's `classes.json`: vtables and their slot functions, named
    by RTTI class;
  * the findings in `site/findings/*.toml`: functions a record names, filed
    under the record's topic;
  * one or more "entered" lists: the symbol ordering files written from a
    probe build's call counts (`config/performance/symbol-order-*.txt`), or a
    probe build's `probe.json`.

Each function gets a subsystem from, in order: a finding, a vtable slot of a
class whose name matches a rule, a code range the findings record, then the
subsystems of its callers (repeatedly, majority of labelled callers), then
the neighbours on both sides in the image. The result is a size estimate,
not a classification to trust per function.

    python -I tools/re/live-code-census.py --tree .local/generated/default \
        --entered config/performance/symbol-order-base.txt --json .local/re/census-base.json
    python -I tools/re/live-code-census.py --v4 --tree .local/generated-v4/default \
        --entered config/performance/symbol-order-v4.txt
    python -I tools/re/live-code-census.py --v4 --tree .local/generated-v4/default \
        --entered .local/re/probe-v4/*/probe.json --json .local/re/census-v4-routes.json

With several lists it prints, in the order given, what each one entered and
added to the lists before it; the census counts their union. "Entered
instructions" are the sizes of functions entered at least once, an upper
bound on the code that ran.

It reads generated code only to count instructions and find call targets;
it prints addresses and counts, never code.
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)")
CALL_RE = re.compile(r"\b(?:__imp__)?(sub_[0-9A-F]{8})\(ctx")
ADDR_RE = re.compile(r"(?:sub_|0x)?\b(8[23][0-9A-Fa-f]{6})\b")

# Class-name rules, first match wins. Names are RTTI names as the generator
# writes them (`Outer::Inner`, or `Class@Namespace` for some templates).
CLASS_RULES: list[tuple[str, str]] = [
    (r"ForzaUI|UI4|\bCUI|UIPostProcess|Overlay|Hud|HUD|SatNav|Gameface|Anark", "ui"),
    (r"Audio|Radio|Sound|FMOD|Speech|Voice|XHV|Music|Cue", "audio"),
    (r"RacingAI|AIOpenWorld|\bAI|Driver(Model|AI)|Rubber|Navigation|TrackPositioning|Traffic|MLP", "ai"),
    (r"CarDynamics|Physics|Tyre|Tire|Suspension|Collision|Assists|Drivetrain|Engine(Sim|Model)|Rigid|Havok|hk[A-Z]", "physics"),
    (r"Render|Presentation|Shadow|Shader|Texture|Graphics|Light|Particle|Skid|Model|Livery|Camera|View|PostProcess|Bloom|Sky|proceduralGeometry|Mesh|Material|Draw|Display", "render"),
    (r"Stream|IOSys|File|Archive|Zip|Loader|Cache|Track|World|Zone|PVS|Placement|Scenery|Smash", "world"),
    (r"Network|Lobby|Live|Online|Session|Matchmak|Leaderboard|Xls|Club|Storefront|Market", "online"),
    (r"Profile|Save|Registry|Database|Sql|Achiev|Unlock|Entitlement|Content|DLC|Licen", "data"),
    (r"States|StateSystem|StateGroup|GameModes|HorizonGameState|RaceFeats|Event|Career|Festival|Activity|Flow|Script|Cutscene|Replay|Rally|Wristband", "flow"),
    (r"Input|Controller|Wheel|ForceFeedback|Gamepad|Rumble", "input"),
    (r"^std::|RTTI|Core|\bM::|Allocator|Heap|Memory|Thread|Job|Async|Task|Container|Vector|String|Hash", "core"),
]

# Findings topic file -> subsystem, with per-group overrides.
TOPIC = {
    "audio": "audio", "content": "data", "files": "world", "flow": "flow",
    "objects": "flow", "rendering": "render", "saves": "data", "systems": "flow",
    "ui": "ui", "world": "world",
    # Thread maps, hot-function lists and image notes name functions from
    # every subsystem, so they seed nothing (group overrides still apply).
    "executable": None, "threads": None, "title-update": None,
}
GROUP = {
    "Car dynamics": "physics", "Vehicles, camera and crowd": "render",
    "AI opponents": "ai", "Online services": "online", "Wheel force feedback": "input",
    "Runtime library": "crt", "Hot functions": None, "Thread map": None,
    "Collision and cameras": "physics", "Input and system UI": "input",
    "Time of day": "render", "Textures and models": "render",
}

# Code ranges the findings record (base image), as [start, end) -> subsystem.
# FMOD Ex: audio.toml "FMOD Ex code range" (structural reading). XDK online,
# Kinect and voice: systems.toml "online and Kinect code range is not dead".
BASE_RANGES = [
    (0x82F34000, 0x83031000, "fmod"),
    (0x82A90000, 0x82B40000, "xdk-online"),
]


def parse_tree(tree: Path):
    sizes: dict[int, int] = {}
    calls: dict[int, set[int]] = collections.defaultdict(set)
    files = sorted(tree.glob("*_recomp.*.cpp"))
    if not files:
        sys.exit(f"no *_recomp.*.cpp files under {tree}")
    for path in files:
        current = None
        with path.open(encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if line.startswith("DEFINE_REX_FUNC("):
                    m = FUNC_RE.match(line)
                    current = int(m.group(1)[4:], 16) if m else None
                    if current is not None:
                        sizes.setdefault(current, 0)
                    continue
                if current is None:
                    continue
                if line.startswith("\t// "):
                    sizes[current] += 1
                elif "sub_" in line:
                    for target in CALL_RE.findall(line):
                        callee = int(target[4:], 16)
                        if callee != current:
                            calls[current].add(callee)
    return sizes, calls


def read_list(path: Path) -> set[int]:
    """One entered list: a symbol-order .txt, a probe build's probe.json
    (`calls` is a list of [address, count]) or probe-summary.py --json
    (`calls` is a dict of address -> count)."""
    entered: set[int] = set()
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".json":
        calls = json.loads(text).get("calls", {})
        pairs = calls.items() if isinstance(calls, dict) else calls
        for address, count in pairs:
            if count > 0:
                entered.add(int(str(address).removeprefix("0x"), 16))
    else:
        for m in re.finditer(r"sub_([0-9A-F]{8})", text):
            entered.add(int(m.group(1), 16))
    if not entered:
        sys.exit(f"{path}: no entered functions read")
    return entered


def read_entered(paths: list[Path], sizes: dict[int, int]) -> set[int]:
    """The union of the lists, with what each list adds in the order given."""
    entered: set[int] = set()
    lists = [(path, read_list(path)) for path in paths]
    for index, (path, functions) in enumerate(lists):
        others = set().union(*(f for i, (_, f) in enumerate(lists) if i != index))
        new = functions - entered
        # A run's probe.json sits in <run>.state/logs/: name it by the run.
        label = path.parents[1].name if path.name == "probe.json" and len(path.parents) > 1 else path.name
        print(f"  {label}: entered {len(functions):,} ({sum(sizes.get(a, 0) for a in functions):,} instr), "
              f"new vs. earlier lists {len(new):,} ({sum(sizes.get(a, 0) for a in new):,} instr), "
              f"only in this list {len(functions - others):,}")
        entered |= functions
    return entered


def class_labels(classes_path: Path, sizes: dict[int, int]) -> dict[int, str]:
    rules = [(re.compile(p), s) for p, s in CLASS_RULES]
    owners: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    for entry in json.loads(classes_path.read_text(encoding="utf-8")):
        name = entry.get("class", "")
        label = next((s for rx, s in rules if rx.search(name)), None)
        for slot in entry.get("slots", []):
            try:
                addr = int(slot, 16)
            except ValueError:
                continue
            owners[addr][label] += 1
    labels = {}
    for addr, counter in owners.items():
        if addr not in sizes:
            continue
        # A slot shared by many classes (pure virtual, empty destructor, a
        # default "return 0") says nothing about a subsystem.
        if sum(counter.values()) > 8:
            continue
        label, count = counter.most_common(1)[0]
        if label and count * 3 >= 2 * sum(counter.values()):
            labels[addr] = label
    return labels


def finding_labels(v4: bool, sizes: dict[int, int]) -> dict[int, str]:
    labels = {}
    field = "v4" if v4 else "base"
    for path in sorted((ROOT / "site" / "findings").glob("*.toml")):
        topic = TOPIC.get(path.stem)
        data = tomllib.loads(path.read_text(encoding="utf-8"))
        for record in data.get("finding", []):
            label = GROUP.get(record.get("group", ""), topic) or topic
            if label is None:
                continue
            for m in ADDR_RE.finditer(str(record.get(field, ""))):
                addr = int(m.group(1), 16)
                if addr in sizes:
                    labels.setdefault(addr, label)
    return labels


def propagate(sizes, calls, labels: dict[int, str], rounds: int = 12) -> dict[int, str]:
    callers: dict[int, set[int]] = collections.defaultdict(set)
    for caller, callees in calls.items():
        for callee in callees:
            callers[callee].add(caller)
    labels = dict(labels)
    for _ in range(rounds):
        added = {}
        for addr in sizes:
            if addr in labels:
                continue
            votes = collections.Counter(labels[c] for c in callers.get(addr, ()) if c in labels)
            if not votes:
                continue
            label, count = votes.most_common(1)[0]
            total = sum(votes.values())
            added[addr] = label if count * 3 >= 2 * total else "shared"
        if not added:
            break
        labels.update(added)
    # Callers of labelled code that nothing labelled calls (thread entries,
    # vtable slots the rules missed): take the majority of their callees.
    for _ in range(4):
        added = {}
        for addr in sizes:
            if addr in labels:
                continue
            votes = collections.Counter(labels[c] for c in calls.get(addr, ()) if labels.get(c) not in (None, "shared"))
            if votes:
                label, count = votes.most_common(1)[0]
                if count * 3 >= 2 * sum(votes.values()):
                    added[addr] = label
        if not added:
            break
        labels.update(added)
    # Image neighbours: an unlabelled run between two functions of the same
    # subsystem takes it (object files are laid out contiguously).
    ordered = sorted(sizes)
    for index, addr in enumerate(ordered):
        if addr in labels:
            continue
        left = next((labels[a] for a in reversed(ordered[max(0, index - 6):index]) if a in labels and labels[a] != "shared"), None)
        right = next((labels[a] for a in ordered[index + 1:index + 7] if a in labels and labels[a] != "shared"), None)
        if left and left == right:
            labels[addr] = left
    return labels


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", type=Path, required=True, help="generated tree, e.g. .local/generated/default")
    parser.add_argument("--classes", type=Path, help="classes.json (default: <tree>/classes.json)")
    parser.add_argument("--entered", type=Path, nargs="*", default=[], help="symbol-order .txt or probe.json files")
    parser.add_argument("--v4", action="store_true", help="the tree is v4: use the findings' v4 addresses, no base ranges")
    parser.add_argument("--json", type=Path, help="write per-function labels and the summary here")
    args = parser.parse_args()

    sizes, calls = parse_tree(args.tree)
    entered = read_entered(args.entered, sizes)
    seeds = finding_labels(args.v4, sizes)
    sources = collections.Counter({"finding": len(seeds)})
    for addr, label in class_labels(args.classes or args.tree / "classes.json", sizes).items():
        if addr not in seeds:
            seeds[addr] = label
            sources["vtable"] += 1
    if not args.v4:
        for addr in sizes:
            if addr in seeds:
                continue
            for start, end, label in BASE_RANGES:
                if start <= addr < end:
                    seeds[addr] = label
                    sources["range"] += 1
    labels = propagate(sizes, calls, seeds)
    sources["propagated"] = len(labels) - len(seeds)

    rows = collections.defaultdict(lambda: [0, 0, 0, 0])
    for addr, size in sizes.items():
        row = rows[labels.get(addr, "unassigned")]
        row[0] += 1
        row[1] += size
        if addr in entered:
            row[2] += 1
            row[3] += size
    total = [sum(r[i] for r in rows.values()) for i in range(4)]
    known_entered = len(entered & sizes.keys())
    print(f"functions {total[0]:,}, guest instructions {total[1]:,}; entered lists name {len(entered):,}, {known_entered:,} in this tree")
    print(f"seeds: {dict(sources)}")
    print(f"{'subsystem':<12}{'functions':>11}{'instr':>12}{'entered':>10}{'entered instr':>15}{'share of live':>15}")
    for name, row in sorted(rows.items(), key=lambda kv: -kv[1][3]):
        share = row[3] / total[3] * 100 if total[3] else 0.0
        print(f"{name:<12}{row[0]:>11,}{row[1]:>12,}{row[2]:>10,}{row[3]:>15,}{share:>14.1f}%")
    print(f"{'total':<12}{total[0]:>11,}{total[1]:>12,}{total[2]:>10,}{total[3]:>15,}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        summary = {name: dict(zip(("functions", "instructions", "entered", "entered_instructions"), row)) for name, row in rows.items()}
        out = {
            "schema": "pinyon-shift.live-code-census.v1",
            "tree": str(args.tree), "entered_lists": [str(p) for p in args.entered],
            "seeds": dict(sources), "summary": summary,
            "functions": {f"{a:08X}": {"subsystem": labels.get(a, "unassigned"), "instructions": s, "entered": a in entered}
                          for a, s in sorted(sizes.items())},
        }
        args.json.write_text(json.dumps(out, indent=1), encoding="utf-8", newline="\n")
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
