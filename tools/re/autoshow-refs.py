#!/usr/bin/env python3
"""Autoshow reference captures for the M1 car viewer.

  autoshow-refs.py routes <car-list.json> <out dir> [--makes 0,2] [--group N]
      write render-test routes that open the Autoshow once and, make by
      make, show every car in its showroom and take one numbered frame
      dump of each (the dump path is a runtime cvar, so one run takes
      many dumps)
  autoshow-refs.py analyse <dump> <car-list.json> <car id> --cars-root <dir>
      the car's draws, its matrices and the skip list for the mask replay
  autoshow-refs.py build ...  (see --help)

The reference JSON schema is docs/formats/AUTOSHOW_REFERENCE.md
("Reference files"). Our own tooling over the player's files and frame
dumps; nothing here contains game data.
"""
from __future__ import annotations

import argparse
import subprocess
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

# Boot to free roam, then the activity button opens the Autoshow (the
# seed's autoshow_anywhere mod). Frames are the render-test runner's.
BOOT = """input 0 0000 0 0 0 0 0 0
wait 10 30000 file pressstart.wmv
input 360 1000 0 0 0 0 0 0
input 366 0000 0 0 0 0 0 0
input 480 1000 0 0 0 0 0 0
input 486 0000 0 0 0 0 0 0
input 600 1000 0 0 0 0 0 0
input 606 0000 0 0 0 0 0 0
input 720 1000 0 0 0 0 0 0
input 726 0000 0 0 0 0 0 0
input 840 1000 0 0 0 0 0 0
input 864 0000 0 0 0 0 0 0
input 960 1000 0 0 0 0 0 0
input 984 0000 0 0 0 0 0 0
input 1080 1000 0 0 0 0 0 0
input 1104 0000 0 0 0 0 0 0
input 1200 1000 0 0 0 0 0 0
input 1224 0000 0 0 0 0 0 0
wait 1400 3000 vehicle
input 1680 4000 0 0 0 0 0 0
input 1686 0000 0 0 0 0 0 0
"""
A, B, RIGHT = "1000", "2000", "0008"
CAROUSEL_AT = 2160      # first make press (the carousel is up by 2100)
STEP = 30               # frames between carousel presses
LIST_WAIT = 300         # A on a make to the car list
REF_DELAY = 390         # A on a car to the reference frame (pilot)
BACK_AT = 450           # A on a car to B (still in the standard view)
BACK_WAIT = 240         # B to the car list again


class Route:
    def __init__(self) -> None:
        self.inputs: list[tuple[int, str]] = []
        self.events: list[tuple[int, str]] = []   # capture and cvar lines

    def press(self, frame: int, button: str, hold: int = 6) -> None:
        self.inputs += [(frame, button), (frame + hold, "0000")]

    def text(self, header: list[str]) -> str:
        lines = ["pinyon-shift-fh1-render-test-v1"] + header + BOOT.splitlines()
        rows = [(f, f"input {f} {b} 0 0 0 0 0 0") for f, b in self.inputs]
        rows += self.events
        rows.sort(key=lambda r: r[0])
        return "\n".join(lines + [r for _, r in rows]) + "\n"


def make_route(makes: list[tuple[int, list[dict]]], dump_dir: Path) -> tuple[str, list[dict]]:
    """One route over consecutive makes: walk the carousel to the first
    make, and for each make open its list, show each car in list order,
    dump it, go back; B twice returns to the carousel for the next make."""
    r = Route()
    t = CAROUSEL_AT
    r.events.append((2100, "capture 2100 manufacturers"))
    position = 0
    plan = []
    for make_index, cars in makes:
        for _ in range(make_index - position):
            r.press(t, RIGHT)
            t += STEP
        position = make_index
        t += 60
        r.events.append((t, f"capture {t} make-{make_index:02d}"))
        r.press(t + 30, A, 36)
        t += 30 + LIST_WAIT
        cursor = 0
        for car in sorted(cars, key=lambda c: c["autoshow_list_index"]):
            for _ in range(car["autoshow_list_index"] - cursor):
                r.press(t, RIGHT)
                t += 60
            cursor = car["autoshow_list_index"]
            cid = car["id"]
            r.events.append((t, f"capture {t} {cid}-list"))
            a = t + 30
            r.press(a, A, 36)
            dump = dump_dir / f"{cid}.fh1frame"
            r.events.append((a + REF_DELAY - 10, f"cvar {a + REF_DELAY - 10} fh1_frame_dump_path {dump}"))
            r.events.append((a + REF_DELAY, f"cvar {a + REF_DELAY} fh1_frame_dump_frame -1"))
            r.events.append((a + REF_DELAY, f"capture {a + REF_DELAY} {cid}-ref"))
            r.press(a + BACK_AT, B, 12)
            t = a + BACK_AT + BACK_WAIT
            plan.append({"car_id": cid, "media_name": car["media_name"], "make_index": make_index,
                         "list_index": car["autoshow_list_index"], "a_frame": a,
                         "dump": str(dump), "capture": f"{cid}-ref"})
        r.press(t, B, 12)           # car list back to the carousel
        t += BACK_WAIT
    r.events.append((t, f"capture {t} end"))
    ids = ",".join(str(p["car_id"]) for p in plan)
    header = [f"# M1 Autoshow references (seed m1-autoshow-v4, v4 build): makes "
              f"{','.join(str(m) for m, _ in makes)}; one dump per car, {REF_DELAY} frames after A.",
              "# dlc-car-trace", f"# dlc-car-ids {ids}"]
    text = r.text(header).rstrip("\n") + f"\nstop {t + 30}\n"
    return text, plan


def with_unicorns(rows: list[dict]) -> list[dict]:
    """The unicorn rows placed in the Autoshow's lists as a seed whose
    database clears IsUnicorn shows them: makes sorted by name without
    case, each make's cars by class, performance index and ID, as
    car-list.py orders the stock lists. Returns only the unicorn rows,
    with their make and list indices recomputed."""
    shown = [dict(c) for c in rows if c["screen"] == "autoshow" or c["screen"].startswith("garage")]
    makes = sorted({c["make"] for c in shown}, key=str.casefold)
    out = []
    for m, make in enumerate(makes):
        group = sorted((c for c in shown if c["make"] == make),
                       key=lambda c: (c["flags"]["ClassID"], c["performance_index"], c["id"]))
        for k, car in enumerate(group):
            car["autoshow_make_index"], car["autoshow_list_index"] = m, k
            if car["screen"].startswith("garage"):
                out.append(car)
    return out


def cmd_routes(args) -> int:
    data = json.loads(Path(args.car_list).read_text(encoding="utf-8"))
    cars = [c for c in data["cars"] if c["screen"] == "autoshow"]
    if args.unicorns:
        cars = with_unicorns(data["cars"])
    by_make: dict[int, list[dict]] = {}
    for car in cars:
        by_make.setdefault(car["autoshow_make_index"], []).append(car)
    wanted = sorted(by_make) if not args.makes else [int(m) for m in args.makes.split(",")]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    groups, group = [], []
    for m in wanted:
        group.append(m)
        if sum(len(by_make[g]) for g in group) >= args.group:
            groups.append(group)
            group = []
    if group:
        groups.append(group)
    for group in groups:
        name = f"{args.prefix}-{group[0]:02d}-{group[-1]:02d}"
        dump_dir = Path(args.dump_root).resolve() / name
        text, plan = make_route([(m, by_make[m]) for m in group], dump_dir)
        (out / f"{name}.fh1test").write_text(text, encoding="utf-8", newline="\n")
        (out / f"{name}.plan.json").write_text(json.dumps(plan, indent=1) + "\n",
                                                encoding="utf-8", newline="\n")
        print(name, len(plan), "cars")
    return 0


# --- dumps to references ------------------------------------------------------

CAR_LIST = ROOT / ".local/re/cars/car-list.json"
SEED = ROOT / ".local/render-seeds/m1-autoshow-v4"
BUILD = ROOT / "out/build/win-amd64-v4"
PYTHON = sys.executable


def load_cars(path=CAR_LIST) -> dict[int, dict]:
    return {c["id"]: c for c in json.loads(Path(path).read_text(encoding="utf-8"))["cars"]}


def plan_entries(paths: list[str]) -> list[dict]:
    out = []
    for path in paths:
        out += json.loads(Path(path).read_text(encoding="utf-8"))
    return out


def cmd_prepare(args) -> int:
    """Per dump, beside it: the draws each replay leaves out. `ref` (the
    reference image) leaves out the UI; `caronly` also the stage (every
    other triangle draw of the main colour pass), so the car stands on the
    pass's clear colour; `empty` also the car. The mask is where `caronly`
    and `empty` differ."""
    import autoshow_dump as ad
    cars = load_cars(args.car_list)
    for entry in plan_entries(args.plan):
        dump = Path(entry["dump"])
        out = dump.with_suffix(".skips.json")
        if not dump.is_file() or (out.is_file() and not args.force):
            continue
        try:
            a = ad.analyse(dump, cars[entry["car_id"]])
            stage = ad.stage_numbers(a)
            skips = {"ui": ad.wg.ranges(a["ui"]),
                     "caronly": ad.wg.ranges(a["ui"] + stage),
                     "empty": ad.wg.ranges(a["ui"] + stage + a["car_numbers"]),
                     "kinds": a["kinds"], "files": a["files"], "car_draws": len(a["car_numbers"]),
                     "stage_draws": len(stage), "ui_draws": len(a["ui"]), "executed": a["executed"]}
            if args.check:
                # the car without wheels, rotors and calipers, for the
                # projection check of the exterior and cockpit
                wheels = [a["draws"][n]["executor"] for n in a["car_draws"]
                          if ad.part_label(*a["tagged"][n][:2]) not in ("lod0", "cockpit", "exterior")]
                skips["wheelless"] = ad.wg.ranges(a["ui"] + stage + wheels)
                skips["drawn_parts"] = sorted({f"{a['tagged'][n][1]}:{a['tagged'][n][2]}"
                                               for n in a["car_draws"]})
        except Exception as error:  # noqa: BLE001 - reported per car
            skips = {"error": f"{type(error).__name__}: {error}"}
        out.write_text(json.dumps(skips, indent=1) + "\n", encoding="utf-8", newline="\n")
        print(entry["car_id"], skips.get("error") or skips["kinds"], flush=True)
    return 0


def replay_one(dump: Path, skip: str, out: Path, work: Path, log: bool) -> tuple[bool, str]:
    arguments = [f"--game-argument=--fh1_frame_replay_output_path={out}",
                 f"--game-argument=--fh1_debug_skip_draws={skip}"]
    if log:
        arguments.append("--game-argument=--fh1_debug_log_draws=true")
    process = subprocess.run(
        [PYTHON, str(ROOT / "tools/replay-fh1-frame.py"), str(dump), "--state-root", str(SEED),
         "--build-directory", str(BUILD), "--work", str(work), "--timeout", "240", *arguments],
        capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    if log:
        lines = [line for line in (work / "logs/runtime.log").read_text(
            encoding="utf-8", errors="replace").splitlines() if "FH1 debug draw" in line]
        out.with_suffix(".draws.log").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return out.is_file(), process.stdout[-400:] + process.stderr[-400:]


def cmd_replay(args) -> int:
    """Both replays of every prepared dump (the caller holds the machine
    lock): <id>.ref.ppm, <id>.caronly.ppm, <id>.empty.ppm for the first and
    last dump, and <id>.wheelless.ppm with --check (see prepare)."""
    from concurrent.futures import ThreadPoolExecutor
    jobs = []
    for entry in plan_entries(args.plan):
        dump = Path(entry["dump"])
        skips_path = dump.with_suffix(".skips.json")
        if not dump.is_file() or not skips_path.is_file():
            continue
        skips = json.loads(skips_path.read_text(encoding="utf-8"))
        if "error" in skips:
            continue
        jobs.append([dump, skips, False])
    # The empty frame (UI, stage and car left out) was identical across
    # dumps; replay it for the first and last dump of each plan only and
    # check that they agree.
    if jobs:
        jobs[0][2] = jobs[-1][2] = True

    import queue
    slots = queue.Queue()
    for k in range(args.jobs):
        slots.put(k)

    def run(job):
        slot = slots.get()
        try:
            return run_in(job, ROOT / f".local/re/cars/replay/w{slot}")
        finally:
            slots.put(slot)

    def run_in(job, work):
        dump, skips, empty = job
        results = []
        wanted = [("ref", skips["ui"], True), ("caronly", skips["caronly"], False)]
        if empty:
            wanted.append(("empty", skips["empty"], False))
        if "wheelless" in skips:
            wanted.append(("wheelless", skips["wheelless"], False))
        for name, skip, log in wanted:
            out = dump.with_suffix(f".{name}.ppm")
            if out.is_file() and not args.force:
                continue
            ok, tail = replay_one(dump, skip, out, work, log)
            results.append((name, ok, tail))
        for stale in (Path(f"{dump}.replay.json"), Path(f"{dump}.replay.bin")):
            stale.unlink(missing_ok=True)
        return dump.stem, results

    with ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(run, job) for job in jobs]
        for future in futures:
            name, results = future.result()
            for kind, ok, tail in results:
                print(name, kind, "ok" if ok else "FAILED " + tail, flush=True)
    return 0


def dump_swap(dump: Path) -> int:
    import struct
    with open(dump, "rb") as handle:
        return struct.unpack("<8sQ", handle.read(16))[1]


MASK_THRESHOLD = 6


def diff_mask(a: bytes, b: bytes, threshold: int = MASK_THRESHOLD) -> bytes:
    """255 where any channel differs by at least `threshold`. The car-only
    and empty frames also differ by one level in a soft halo around the
    car (a post-processing term), so one level is not counted."""
    out = bytearray(len(a) // 3)
    for i in range(len(out)):
        j = 3 * i
        if (abs(a[j] - b[j]) >= threshold or abs(a[j + 1] - b[j + 1]) >= threshold
                or abs(a[j + 2] - b[j + 2]) >= threshold):
            out[i] = 255
    return bytes(out)


def cmd_build(args) -> int:
    """Reference JSON, image, car-only image and mask for each replayed dump."""
    import autoshow_dump as ad
    import _ppm
    cars = load_cars(args.car_list)
    refs = Path(args.refs)
    refs.mkdir(parents=True, exist_ok=True)
    entries = plan_entries(args.plan)
    empties = [Path(e["dump"]).with_suffix(".empty.ppm") for e in entries]
    empties = [path for path in empties if path.is_file()]
    if not empties:
        print("no empty frame replayed for this plan", file=sys.stderr)
        return 1
    empty = _ppm.read_ppm(empties[0])
    empty_agree = all(_ppm.read_ppm(path) == empty for path in empties[1:])
    for entry in entries:
        dump = Path(entry["dump"])
        ref_ppm, car_ppm = dump.with_suffix(".ref.ppm"), dump.with_suffix(".caronly.ppm")
        if not (dump.is_file() and ref_ppm.is_file() and car_ppm.is_file()):
            continue
        car = cars[entry["car_id"]]
        a = ad.analyse(dump, car)
        width, height, image = _ppm.read_ppm(ref_ppm)
        w2, h2, car_only = _ppm.read_ppm(car_ppm)
        assert (w2, h2) == (width, height) == empty[:2]
        logged = ad.wg.logged_draws(str(ref_ppm.with_suffix(".draws.log")))
        numbers = ad.wg.executor_numbers(a["draws"], logged) if logged else []
        aligned = bool(logged) and all(n == d["executor"] for n, d in zip(numbers, a["draws"]))
        mask = diff_mask(car_only, empty[2])
        cid = car["id"]
        _ppm.write_png(refs / f"{cid}.png", width, height, image)
        _ppm.write_png(refs / f"{cid}.car.png", width, height, car_only)
        _ppm.write_png(refs / f"{cid}.mask.png", width, height, mask, channels=1)
        rgb, metallic = ad.paint(cid)
        source = car["row_origin"]
        if source == "dlc":
            source = "dlc:" + car["row_packages"][0]
        m = ad.matrices(a, width, height)
        covered = sum(1 for v in mask if v)
        xs = [i % width for i, v in enumerate(mask) if v]
        ys = [i // width for i, v in enumerate(mask) if v]
        reference = {
            "schema": "shift-engine.m1-reference.v1",
            "car_id": cid, "media_name": car["media_name"], "display_name": car["display_name"],
            "make": car["make"], "year": car["year"], "source": source,
            "files": car["files"][0]["path"],
            "image": f"{cid}.png", "image_car_only": f"{cid}.car.png", "mask": f"{cid}.mask.png",
            "width": width, "height": height,
            "frame": {"after_a": REF_DELAY, "dump_swap": dump_swap(dump)},
            "clip_from_car": m["clip_from_car"], "view": m["view"], "proj": m["proj"],
            "car_to_world": m["car_to_world"], "near": m["near"], "half_pixel_removed": True,
            "paint_rgb": rgb, "paint_metallic": metallic,
            "game_clip_from_car": m["game_clip_from_car"],
            "checks": {**m["checks"], "car_draws": a["kinds"], "model_files": a["files"],
                       "ui_draws_left_out": len(a["ui"]), "draw_numbers_match_replay_log": aligned,
                       "empty_frames_agree": empty_agree, "mask_threshold": MASK_THRESHOLD,
                       "viewport": a["viewport"], "z_viewport": a["zviewport"],
                       "mask_pixels": covered,
                       "mask_box": [min(xs), min(ys), max(xs), max(ys)] if xs else None},
        }
        (refs / f"{cid}.json").write_text(json.dumps(reference, indent=1) + "\n",
                                          encoding="utf-8", newline="\n")
        print(cid, car["media_name"], "mask", covered, "aligned", aligned, flush=True)
        if args.clean:
            # A --check plan keeps what the projection check reads.
            checked = dump.with_suffix(".wheelless.ppm").is_file()
            for suffix in (".ref.ppm", ".caronly.ppm", ".ref.draws.log") + (
                    () if checked else (".wheelless.ppm", ".empty.ppm", ".skips.json")):
                dump.with_suffix(suffix).unlink(missing_ok=True)
            dump.unlink(missing_ok=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("routes")
    p.add_argument("car_list")
    p.add_argument("out")
    p.add_argument("--makes")
    p.add_argument("--group", type=int, default=1, help="cars per route, at least")
    p.add_argument("--dump-root", default=str(ROOT / ".local/re/cars/dumps"))
    p.add_argument("--unicorns", action="store_true",
                   help="route the unicorn rows only, for a seed whose database clears IsUnicorn")
    p.add_argument("--prefix", default="m1refs")
    p.set_defaults(func=cmd_routes)
    p = sub.add_parser("prepare")
    p.add_argument("plan", nargs="+")
    p.add_argument("--car-list", default=str(CAR_LIST))
    p.add_argument("--check", action="store_true",
                   help="also a replay without the wheels, for the projection check")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_prepare)
    p = sub.add_parser("replay")
    p.add_argument("plan", nargs="+")
    p.add_argument("--jobs", type=int, default=3)
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_replay)
    p = sub.add_parser("build")
    p.add_argument("plan", nargs="+")
    p.add_argument("--car-list", default=str(CAR_LIST))
    p.add_argument("--refs", default=str(ROOT / ".local/re/cars/refs"))
    p.add_argument("--clean", action="store_true", help="delete the dump and replays afterwards")
    p.set_defaults(func=cmd_build)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
