#!/usr/bin/env python3
"""The M1 car-viewer report: render every reference car and score it.

For each `<car_id>.json` of AUTOSHOW_REFERENCE schema v2 in the
references folder, runs `carview --bake <bake> --reference <json>` and
collects the metric it prints (silhouette IoU, PSNR inside the reference's
car mask, edge overlap). v1 references are listed, not scored (their masks
hold the lamp glow); `--include-v1` scores them too. Writes `report.txt`
with one row per car and the pass rule: IoU at least 0.97 for every car.
Masked PSNR is reported only (polish, not M1's gate). With Pillow it also
reports IoU-g, the IoU without the largest connected reference-only region
(glare a mask still holds, SPEC_QUESTIONS Q11), beside the official IoU and
never in the pass rule, and writes `<car_id>.side.png`:
reference (car-only replay when given) | engine | mask difference (green:
both, red: reference only, blue: engine only) | absolute colour
difference inside the reference mask, for judging by eye.

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
    # Absolute colour difference inside the reference mask (black outside).
    from PIL import ImageChops
    absdiff = Image.composite(ImageChops.difference(a, b), Image.new("RGB", (w, h)),
                              ma.point(lambda v: 255 if v else 0))
    sheet = Image.new("RGB", (w * 4, h))
    for i, im in enumerate((a, b, diff, absdiff)):
        sheet.paste(im, (i * w, 0))
    sheet.save(out)


def iou_without_largest(ref_mask: Path, ours_mask: Path) -> tuple[float, int] | None:
    """IoU with the largest connected (4-neighbour) reference-only region
    left out, and that region's size: the glows the captured masks hold
    and the engine does not draw (SPEC_QUESTIONS Q11). Reported beside the
    official IoU, never instead of it."""
    try:
        from PIL import Image
    except ImportError:
        return None
    a = Image.open(ref_mask).convert("L")
    w, h = a.size
    ra = a.tobytes()
    eb = Image.open(ours_mask).convert("L").tobytes()
    inter = union = 0
    ref_only = bytearray(w * h)
    for i in range(w * h):
        r, e = ra[i] > 0, eb[i] > 0
        inter += r and e
        union += r or e
        if r and not e:
            ref_only[i] = 1
    largest = 0
    stack = []
    for start in range(w * h):
        if not ref_only[start]:
            continue
        ref_only[start] = 0
        stack.append(start)
        n = 0
        while stack:
            i = stack.pop()
            n += 1
            x = i % w
            for j in (i - w, i + w, i - 1 if x else -1, i + 1 if x + 1 < w else -1):
                if 0 <= j < w * h and ref_only[j]:
                    ref_only[j] = 0
                    stack.append(j)
        largest = max(largest, n)
    return (inter / (union - largest) if union > largest else 1.0), largest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--carview", required=True)
    parser.add_argument("--bake", required=True)
    parser.add_argument("--refs", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--no-images", action="store_true", help="skip the side-by-side PNGs")
    parser.add_argument("--light", help="passed to carview --light (lighting settings)")
    parser.add_argument("--include-v1", action="store_true", help="score v1 references as well")
    args = parser.parse_args()
    args.carview = str(Path(args.carview).resolve())  # CreateProcess does not search relative paths
    args.out.mkdir(parents=True, exist_ok=True)

    rows = []
    leftovers = []  # v1 references, not scored
    schemas = {"shift-engine.m1-reference.v2"}
    if args.include_v1:
        schemas.add("shift-engine.m1-reference.v1")
    for ref in sorted(args.refs.glob("*.json"), key=lambda p: (len(p.stem), p.stem)):
        data = json.loads(ref.read_text(encoding="utf-8"))
        if data.get("schema") not in schemas:
            if data.get("schema") == "shift-engine.m1-reference.v1":
                leftovers.append(f"{ref.stem} {data.get('media_name', '?')}")
            continue
        ours = args.out / f"{ref.stem}.engine.png"
        ours_mask = args.out / f"{ref.stem}.engine.mask.png"
        run = subprocess.run([args.carview, "--bake", args.bake, "--reference", str(ref), "--out", str(ours),
                              "--mask", str(ours_mask)] + (["--light", args.light] if args.light else []), capture_output=True, text=True,
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
        cleaned = iou_without_largest(ref.parent / data["mask"], ours_mask)
        if cleaned:
            rows[-1]["iou_clean"], rows[-1]["glow_px"] = cleaned
        if not args.no_images:
            folder = ref.parent
            side_by_side(folder / data.get("image_car_only", data["image"]), folder / data["mask"], ours, ours_mask,
                         args.out / f"{ref.stem}.side.png")

    scored = [r for r in rows if "iou" in r]
    median = statistics.median(r["psnr"] for r in scored) if scored else 0.0
    lines = ["M1 car viewer report (REIMPLEMENTATION_PLAN M1; AUTOSHOW_REFERENCE v2)", ""]
    lines.append("IoU is the official number. IoU-g leaves out the largest connected reference-only")
    lines.append("region (glow-px, the lamp and highlight glows: SPEC_QUESTIONS Q11); it separates")
    lines.append("geometry from glow and is not used for the pass.")
    lines.append("")
    lines.append(f"{'car_id':>7}  {'media name':32} {'IoU':>7} {'IoU-g':>7} {'glow px':>8} {'PSNR dB':>8} {'edge':>6}"
                 f" {'ref px':>8} {'eng px':>8}  flags")
    fails = 0
    for r in rows:
        if "error" in r:
            lines.append(f"{r['car_id']:>7}  {r['media']:32} error: {r['error'][0]}")
            fails += 1
            continue
        flags = []
        if r["iou"] < 0.97:
            flags.append("IoU<0.97")
        fails += bool(flags)
        if r["psnr"] < median - 6:
            flags.append("PSNR>6dB under median (report only)")
        if r.get("iou_clean", 1.0) < 0.97:
            flags.append("IoU-g<0.97")
        clean = f"{r['iou_clean']:7.4f} {r['glow_px']:8d}" if "iou_clean" in r else f"{'-':>7} {'-':>8}"
        lines.append(f"{r['car_id']:>7}  {r['media']:32} {r['iou']:7.4f} {clean} {r['psnr']:8.2f} {r['edge']:6.3f}"
                     f" {r['ref_px']:8d} {r['eng_px']:8d}  {' '.join(flags)}")
    lines += ["", f"cars: {len(rows)}, scored: {len(scored)}, median masked PSNR: {median:.2f} dB",
              f"IoU: min {min((r['iou'] for r in scored), default=0):.4f},"
              f" mean {statistics.mean(r['iou'] for r in scored) if scored else 0:.4f};"
              f" IoU-g: min {min((r.get('iou_clean', r['iou']) for r in scored), default=0):.4f},"
              f" cars under 0.97 by IoU-g: {sum(r.get('iou_clean', 1.0) < 0.97 for r in scored)}",
              f"cars failing the numeric rule (IoU >= 0.97): {fails} (the by-eye check is separate)"]
    if leftovers:
        lines += ["", f"v1 references not scored ({len(leftovers)}): " + ", ".join(leftovers)]
    text = "\n".join(lines) + "\n"
    (args.out / "report.txt").write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
