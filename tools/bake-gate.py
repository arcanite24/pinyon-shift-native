#!/usr/bin/env python3
"""The bake gate: two carbake runs from empty folders, timed, compared.

Runs `carbake --game-root <game> --state-root <state> --out <work>/bake<n>`
twice (each folder emptied first), times each from process start to exit,
and hashes every file of each bake except `bake-time.txt` (it holds the
wall time). The combined hash is the SHA-256 of the lines
"<sha256>  <relative path>\\n" sorted by path. The gate passes when both
bakes finish without problems inside the budget (10 minutes each) and the
combined hashes match. With `--compare <bake>` the same hash is taken of
an existing bake (the one the reports render from) and must match too.
Writes `<work>/gate.txt`; with `--keep` the two bakes stay, otherwise
they are deleted after hashing (7.85 GB each).

  python tools/bake-gate.py --carbake build/apps/carbake/carbake.exe \\
      --game-root <game> --state-root <state> --work out/gate --compare out/bake
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

BUDGET_S = 600.0


def combined_hash(folder: Path) -> tuple[str, int]:
    lines = []
    for f in sorted(p for p in folder.rglob("*") if p.is_file()):
        rel = f.relative_to(folder).as_posix()
        if rel == "bake-time.txt":
            continue
        with f.open("rb") as fh:
            lines.append(f"{hashlib.file_digest(fh, 'sha256').hexdigest()}  {rel}\n")
    lines.sort(key=lambda line: line.split("  ", 1)[1])
    return hashlib.sha256("".join(lines).encode()).hexdigest(), len(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--carbake", required=True)
    parser.add_argument("--game-root", required=True)
    parser.add_argument("--state-root", required=True)
    parser.add_argument("--dlc-extracted")
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--compare", type=Path, help="an existing bake whose hash must match")
    parser.add_argument("--keep", action="store_true", help="keep the two bakes")
    args = parser.parse_args()
    carbake = str(Path(args.carbake).resolve())
    args.work.mkdir(parents=True, exist_ok=True)

    lines, ok = [], True
    hashes = []
    for n in (1, 2):
        out = args.work / f"bake{n}"
        shutil.rmtree(out, ignore_errors=True)
        cmd = [carbake, "--game-root", args.game_root, "--state-root", args.state_root, "--out", str(out)]
        if args.dlc_extracted:
            cmd += ["--dlc-extracted", args.dlc_extracted]
        start = time.monotonic()
        run = subprocess.run(cmd, capture_output=True, text=True,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        wall = time.monotonic() - start
        summary = (run.stdout.strip().splitlines() or ["(no output)"])[-1]
        inner = (out / "bake-time.txt").read_text().strip() if (out / "bake-time.txt").exists() else "missing"
        ok &= run.returncode == 0 and wall <= BUDGET_S
        digest, files = combined_hash(out)
        hashes.append(digest)
        lines.append(f"bake{n}: {summary}; exit {run.returncode}; wall {wall:.1f} s (process start to exit)")
        lines.append(f"bake{n}: bake-time.txt: {inner}")
        lines.append(f"bake{n}: files {files} combined sha256 {digest}")
        if not args.keep:
            shutil.rmtree(out, ignore_errors=True)
    same = hashes[0] == hashes[1]
    ok &= same
    lines.append("bakes byte-identical" if same else "BAKES DIFFER")
    if args.compare:
        digest, files = combined_hash(args.compare)
        match = digest == hashes[0]
        ok &= match
        lines.append(f"compare {args.compare}: files {files} combined sha256 {digest} "
                     f"({'matches' if match else 'DIFFERS FROM'} the gate bakes)")
    lines.append(f"gate: {'pass' if ok else 'FAIL'} (budget {BUDGET_S:.0f} s per bake)")
    text = "\n".join(lines) + "\n"
    (args.work / "gate.txt").write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
