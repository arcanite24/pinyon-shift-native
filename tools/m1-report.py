#!/usr/bin/env python3
"""The M1 car-viewer report: render every reference car and score it.

For each `<car_id>.json` (AUTOSHOW_REFERENCE schema v1) in the references
folder, runs `carview --bake <bake> --reference <json>` and collects the
metric it prints (silhouette IoU, PSNR inside the reference's car mask, edge
overlap). Writes `report.txt` with one row per car and the plan's pass rule:
IoU at least 0.97 for every car, and no car's masked PSNR more than 6 dB
under the median. With Pillow installed it also writes `<car_id>.side.png`:
reference | engine | mask difference (green: both, red: reference only,
blue: engine only), for judging by eye.

  python tools/m1-report.py --carview build/apps/carview/carview.exe \\
      --bake out/bake --refs <horizon1-recomp>/.local/re/cars/refs --out out/m1

Keep the output out of the repository: it holds the player's captures.
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

METRIC = re.compile(
    r"metric (\S+): iou ([0-9.]+) masked_psnr ([0-9.]+|inf) dB edge_overlap ([0-9.]+)"
    r" \(reference (\d+) px, engine (\d+) px\)")


def side_by_side(ref_png: Path, ref_mask: Path, ours_png: Path, ours_mask: Path, out: Path) -> None:
    try:
        from PIL import Image
    except ImportError:
        return
    a = Image.open(ref_png).convert("RGB")
    b = Image.open(ours_png).convert("RGB")
    ma = Image.open(ref_mask).convert("L")
    mb = Image.open(ours_mask).convert("L")
    w, h = a.size
    diff = Image.new("RGB", (w, h))
    pa, pb, pd = ma.load(), mb.load(), diff.load()
    for y in range(h):
        for x in range(w):
            r, e = pa[x, y] > 0, pb[x, y] > 0
            pd[x, y] = (0, 160, 0) if r and e else (220, 40, 40) if r else (40, 90, 230) if e else (0, 0, 0)
    sheet = Image.new("RGB", (w * 3, h))
    for i, im in enumerate((a, b, diff)):
        sheet.paste(im, (i * w, 0))
    sheet.save(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--carview", required=True)
    parser.add_argument("--bake", required=True)
    parser.add_argument("--refs", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--no-images", action="store_true", help="skip the side-by-side PNGs")
    args = parser.parse_args()
    args.carview = str(Path(args.carview).resolve())  # CreateProcess does not search relative paths
    args.out.mkdir(parents=True, exist_ok=True)

    rows = []
    for ref in sorted(args.refs.glob("*.json"), key=lambda p: (len(p.stem), p.stem)):
        data = json.loads(ref.read_text(encoding="utf-8"))
        if data.get("schema") != "shift-engine.m1-reference.v1":
            continue
        ours = args.out / f"{ref.stem}.engine.png"
        ours_mask = args.out / f"{ref.stem}.engine.mask.png"
        run = subprocess.run([args.carview, "--bake", args.bake, "--reference", str(ref), "--out", str(ours),
                              "--mask", str(ours_mask)], capture_output=True, text=True,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))  # no console on the desktop
        match = METRIC.search(run.stdout)
        if run.returncode != 0 or not match:
            rows.append({"car_id": ref.stem, "media": data.get("media_name", "?"), "error":
                         (run.stderr.strip() or run.stdout.strip()).splitlines()[-1:] or ["no metric"]})
            continue
        notes = [line[6:] for line in run.stdout.splitlines() if line.startswith("note: ")]
        rows.append({"car_id": ref.stem, "media": match.group(1), "iou": float(match.group(2)),
                     "psnr": float("inf") if match.group(3) == "inf" else float(match.group(3)),
                     "edge": float(match.group(4)), "ref_px": int(match.group(5)), "eng_px": int(match.group(6)),
                     "notes": notes})
        if not args.no_images:
            folder = ref.parent
            side_by_side(folder / data.get("image_car_only", data["image"]), folder / data["mask"], ours, ours_mask,
                         args.out / f"{ref.stem}.side.png")

    scored = [r for r in rows if "iou" in r]
    median = statistics.median(r["psnr"] for r in scored) if scored else 0.0
    lines = ["M1 car viewer report (REIMPLEMENTATION_PLAN M1; AUTOSHOW_REFERENCE v1)", ""]
    lines.append(f"{'car_id':>7}  {'media name':32} {'IoU':>7} {'PSNR dB':>8} {'edge':>6} {'ref px':>8}"
                 f" {'eng px':>8}  flags")
    fails = 0
    for r in rows:
        if "error" in r:
            lines.append(f"{r['car_id']:>7}  {r['media']:32} error: {r['error'][0]}")
            fails += 1
            continue
        flags = []
        if r["iou"] < 0.97:
            flags.append("IoU<0.97")
        if r["psnr"] < median - 6:
            flags.append("PSNR>6dB under median")
        fails += bool(flags)
        lines.append(f"{r['car_id']:>7}  {r['media']:32} {r['iou']:7.4f} {r['psnr']:8.2f} {r['edge']:6.3f}"
                     f" {r['ref_px']:8d} {r['eng_px']:8d}  {' '.join(flags)}")
    lines += ["", f"cars: {len(rows)}, scored: {len(scored)}, median masked PSNR: {median:.2f} dB",
              f"IoU: min {min((r['iou'] for r in scored), default=0):.4f},"
              f" mean {statistics.mean(r['iou'] for r in scored) if scored else 0:.4f}",
              f"cars failing the numeric rule: {fails} (the by-eye check is separate)"]
    text = "\n".join(lines) + "\n"
    (args.out / "report.txt").write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
