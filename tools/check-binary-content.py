#!/usr/bin/env python3
"""The M7 binary-content check: a release package holds no guest code and
no game data.

Every file of the package (a folder or a .zip) is checked four ways:

0. Text: none of tools/check-provenance.py's patterns (guest function
   names and addresses, generated-code idioms) anywhere in a file's bytes.
1. Names: no file named like a file of the game (disc media, executables,
   title update, content packages), and no game-data extension.
2. Magics: no file that starts as a game container (XEX, XPR, the
   database, the game's ZIP archives are ZIPs and are refused by name and
   fingerprint instead).
3. Guest code: the game's executable is PowerPC, which an x86-64 or ARM64
   binary does not contain. A copy shows the PowerPC return and link-save
   words (blr 4E800020, mflr r0 7C0802A6) by the thousand; native code has
   them only by chance. More than GUEST_LIMIT of either fails.
4. Game data: content-defined fingerprints (a hash of every 32-byte window,
   kept when the hash falls in 1/512 of its range) of the game's files,
   cached under out/; a package file sharing MATCH_LIMIT or more of them
   holds a copy of game bytes wherever it sits. Fingerprints that also occur
   in the system's own libraries (generic runs and tables) are dropped
   first.

    python tools/check-binary-content.py <package dir or .zip>
        --game-root <disc root> [--state-root <state copy>] [--cache out/content-fp.npy]

Exit 1 with one line per problem. Reads the game's files only to hash
them; writes only the cache.
"""
from __future__ import annotations

import argparse
import io
import os
import re
import sys
import zipfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
K = 32                      # window, bytes
SAMPLE_SHIFT = 55           # keep a window when its mixed hash's top 9 bits are 0 (1/512)
MATCH_LIMIT = 8             # shared fingerprints that fail a file
GUEST_LIMIT = 64            # PowerPC idiom words that fail a binary
CHUNK = 1 << 24             # bytes hashed at a time
P = np.uint64(0x100000001B3)                 # odd: invertible mod 2^64
P_INV = np.uint64(pow(0x100000001B3, -1, 1 << 64))
MIX = np.uint64(0x9E3779B97F4A7C15)
GAME_EXT = {".xex", ".xexp", ".xpr", ".xds", ".bix", ".slt", ".pgeo", ".pvsz", ".col", ".fiz", ".owt", ".owr", ".oww", ".nav",
            ".rmb", ".xma", ".xwb", ".xsb", ".fsb", ".fev", ".puboffer", ".header", ".bundle", ".crowd", ".hex", ".pvs"}
# tools/check-provenance.py's list, on the binaries' text: guest function
# names and addresses, and the generated code's idioms.
PROVENANCE = [
    (re.compile(rb"\bsub_8[23][0-9A-Fa-f]{6}\b"), "a guest function name"),
    (re.compile(rb"\b0x8[23][0-9A-Fa-f]{6}\b"), "a guest code or data address"),
    (re.compile(rb"\bctx\.(?:r|f|v|cr|lr|ctr)\d*\b"), "generated-code register access"),
    (re.compile(rb"\bREX_(?:LOAD|STORE)_|\b__imp__sub_|\bPPC_FUNC|\bDEFINE_REX_FUNC"), "generated-code macro"),
]
MAGICS = [(b"XEX2", "an XEX executable"), (b"XPR2", "an XPR resource"), (b"SQLite format 3\0", "a database")]


def fingerprints(data: bytes) -> np.ndarray:
    """Sampled 32-byte window hashes of `data` (unique, sorted)."""
    out = []
    for start in range(0, max(1, len(data) - K + 1), CHUNK):
        part = data[start:start + CHUNK + K - 1]
        if len(part) < K:
            break
        b = np.frombuffer(part, dtype=np.uint8).astype(np.uint64)
        n = len(b)
        with np.errstate(over="ignore"):
            pw = np.empty(n + 1, dtype=np.uint64)
            pw[0] = 1
            pw[1:] = P
            pw = np.cumprod(pw, dtype=np.uint64)            # P^k
            ipw = np.empty(n + 1, dtype=np.uint64)
            ipw[0] = 1
            ipw[1:] = P_INV
            ipw = np.cumprod(ipw, dtype=np.uint64)          # P^-k
            c = np.zeros(n + 1, dtype=np.uint64)
            c[1:] = np.cumsum(b * pw[:n], dtype=np.uint64)  # sum b[k] P^k
            m = n - K + 1
            h = (c[K:K + m] - c[:m]) * ipw[:m]              # window hash, position-free
            mixed = h * MIX
        keep = h[(mixed >> np.uint64(SAMPLE_SHIFT)) == 0]
        if keep.size:
            out.append(keep)
    return np.unique(np.concatenate(out)) if out else np.empty(0, dtype=np.uint64)


def game_files(game_root: Path, state_root: Path | None):
    roots = [game_root] + ([state_root] if state_root else [])
    for r in roots:
        for dirpath, _, names in os.walk(r):
            for n in names:
                yield Path(dirpath) / n


def build_reference(game_root: Path, state_root: Path | None, cache: Path) -> tuple[np.ndarray, set[str]]:
    names = {p.name.lower() for p in game_files(game_root, state_root)}
    if cache.exists():
        return np.load(cache), names
    fps, total = [], 0
    for p in game_files(game_root, state_root):
        if p.suffix.lower() == ".json":  # the state copy's own records, not game data
            continue
        with open(p, "rb") as f:
            while True:
                blob = f.read(1 << 28)
                if not blob:
                    break
                fps.append(fingerprints(blob))
                total += len(blob)
        if len(fps) > 64:
            fps = [np.unique(np.concatenate(fps))]
    ref = np.unique(np.concatenate(fps)) if fps else np.empty(0, dtype=np.uint64)
    # Generic windows: whatever the system's own libraries also hold.
    sysdir = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"
    generic = []
    if sysdir.is_dir():
        libs = sorted(sysdir.glob("*.dll"), key=lambda q: q.stat().st_size, reverse=True)[:120]
        for q in libs:
            try:
                generic.append(fingerprints(q.read_bytes()))
            except OSError:
                pass
    if generic:
        ref = np.setdiff1d(ref, np.unique(np.concatenate(generic)), assume_unique=True)
    cache.parent.mkdir(parents=True, exist_ok=True)
    np.save(cache, ref)
    print(f"reference: {total / 2**30:.1f} GiB of game files, {ref.size} fingerprints -> {cache}")
    return ref, names


def package_files(pkg: Path):
    if pkg.is_file() and pkg.suffix.lower() == ".zip":
        with zipfile.ZipFile(pkg) as z:
            for i in z.infolist():
                if not i.is_dir():
                    yield i.filename, z.read(i)
    else:
        for p in sorted(pkg.rglob("*")):
            if p.is_file():
                yield p.relative_to(pkg).as_posix(), p.read_bytes()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("package", type=Path)
    ap.add_argument("--game-root", type=Path, required=True)
    ap.add_argument("--state-root", type=Path)
    ap.add_argument("--cache", type=Path, default=ROOT / "out" / "content-fp.npy")
    a = ap.parse_args()
    ref, names = build_reference(a.game_root, a.state_root, a.cache)
    allow = {"license", "readme.md", "license.txt", "readme.txt"}
    problems, checked, size = [], 0, 0
    blr, mflr = bytes.fromhex("4E800020"), bytes.fromhex("7C0802A6")
    for rel, data in package_files(a.package):
        checked += 1
        size += len(data)
        base = rel.rsplit("/", 1)[-1].lower()
        ext = os.path.splitext(base)[1]
        if base in names and base not in allow and len(base) > 4:
            problems.append(f"{rel}: named like a game file")
        if ext in GAME_EXT:
            problems.append(f"{rel}: a game-data extension ({ext})")
        for magic, what in MAGICS:
            if data.startswith(magic):
                problems.append(f"{rel}: starts as {what}")
        for rx, what in PROVENANCE:
            hit = rx.search(data)
            if hit:
                problems.append(f"{rel}: {what} ({hit.group(0)[:32].decode('latin-1')})")
        nb, nm = data.count(blr), data.count(mflr)
        if nb > GUEST_LIMIT or nm > GUEST_LIMIT:
            problems.append(f"{rel}: PowerPC code words (blr {nb}, mflr r0 {nm})")
        if len(data) >= K and ref.size:
            shared = np.intersect1d(fingerprints(data), ref, assume_unique=True).size
            if shared >= MATCH_LIMIT:
                problems.append(f"{rel}: {shared} fingerprints of game files")
    for p in problems:
        print(p)
    print(f"binary-content check: {checked} files, {size / 2**20:.1f} MiB, "
          f"{'FAIL' if problems else 'pass'} ({len(problems)} problems)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
