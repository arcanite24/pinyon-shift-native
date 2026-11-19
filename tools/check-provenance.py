#!/usr/bin/env python3
"""Refuse text in engine sources that only the specification side may hold.

The engine is written from specs (horizon1-recomp docs/CLEAN_ROOM.md). Its
sources must not name guest functions or code addresses, or contain text
copied from the recompiled translation. This scans tracked source files and
exits 1 with one line per problem.

    python tools/check-provenance.py
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".glsl", ".comp", ".vert", ".frag", ".py", ".cmake", ".txt", ".md"}
PATTERNS = [
    (re.compile(r"\bsub_8[23][0-9A-Fa-f]{6}\b"), "a guest function name"),
    (re.compile(r"\b0x8[23][0-9A-Fa-f]{6}\b"), "a guest code or data address"),
    (re.compile(r"\bctx\.(?:r|f|v|cr|lr|ctr)\d*\b"), "generated-code register access"),
    (re.compile(r"\bREX_(?:LOAD|STORE)_|\b__imp__sub_|\bPPC_FUNC|\bDEFINE_REX_FUNC"), "generated-code macro"),
]
SELF = Path(__file__).resolve()


def tracked() -> list[Path]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    return [ROOT / p for p in out.decode().split("\0") if p]


def main() -> int:
    problems = []
    for path in tracked():
        if path.suffix.lower() not in SOURCES or path.resolve() == SELF or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_no, line in enumerate(text.splitlines(), 1):
            for pattern, what in PATTERNS:
                match = pattern.search(line)
                if match:
                    problems.append(f"{path.relative_to(ROOT).as_posix()}:{line_no}: {what} `{match.group(0)}`")
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print("provenance: no guest addresses or generated-code text in tracked sources")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
