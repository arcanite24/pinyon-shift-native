#!/usr/bin/env python3
"""Make a release package from a build folder (M7).

The package is flat: the four programs, the setup scripts that find them
beside themselves, the licences and the player pages. It holds no game
data: setup bakes the player's own files into the play folder.

    python tools/package.py --build build-play --platform windows [--out out/release]
        [--check --game-root <disc root> [--state-root <state copy>]]

--check runs tools/check-binary-content.py on the finished zip.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAMS = ["carview", "carbake", "soundbake", "profile-import"]
PLATFORMS = {"windows": ".exe", "linux": "", "macos": "", "android": ""}


def version() -> str:
    r = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, capture_output=True, text=True)
    return r.stdout.strip() or "unknown"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "out" / "release")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--game-root", type=Path)
    ap.add_argument("--state-root", type=Path)
    a = ap.parse_args()
    exe = PLATFORMS[a.platform]
    name = f"shift-engine-{a.platform}-{version()}"
    stage = a.out / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for prog in PROGRAMS:
        src = a.build / "apps" / prog / (prog + exe)
        if not src.is_file():
            print(f"package: missing {src}")
            return 1
        shutil.copy2(src, stage / src.name)
    for f in ["tools/setup.py", "tools/setup.sh", "tools/setup.ps1", "LICENSE", "THIRD_PARTY_NOTICES.md"]:
        shutil.copy2(ROOT / f, stage / Path(f).name)
    player = ROOT / "docs" / "player"
    if player.is_dir():
        for f in sorted(player.glob("*.md")):
            shutil.copy2(f, stage / f.name)
    zpath = a.out / (name + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(stage.rglob("*")):
            if f.is_file():
                info = zipfile.ZipInfo.from_file(f, f"{name}/{f.relative_to(stage).as_posix()}")
                # Unix modes (a zip made on Windows has none): the programs and
                # scripts executable.
                run = f.stem in PROGRAMS or f.suffix in (".sh", ".py")
                info.external_attr = (0o100755 if run else 0o100644) << 16
                info.create_system = 3  # Unix, so extractors apply the modes
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, f.read_bytes())
    print(f"package: {zpath} ({zpath.stat().st_size / 2**20:.1f} MiB)")
    if a.check:
        if not a.game_root:
            print("package: --check needs --game-root")
            return 1
        cmd = [sys.executable, str(ROOT / "tools" / "check-binary-content.py"), str(stage), "--game-root", str(a.game_root)]
        if a.state_root:
            cmd += ["--state-root", str(a.state_root)]
        return subprocess.run(cmd).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
