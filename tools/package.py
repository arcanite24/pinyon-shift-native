#!/usr/bin/env python3
"""Make a 4-platform release package from a build folder (M7 / Slice 13B).

Bundles shift-launcher alongside carview, carbake, soundbake and
profile-import, plus setup scripts, licences and player docs. On macOS,
builds a proper Shift Engine.app bundle (Contents/Info.plist, Contents/MacOS,
Contents/Frameworks for optional libMoltenVK.dylib, Contents/Resources).
It holds no game data: setup bakes the player's own files into the play folder.

    python tools/package.py --build build-s13a --platform windows [--out out/release]
        [--moltenvk /path/to/libMoltenVK.dylib]
        [--check --game-root <disc root> [--state-root <state copy>]]

--check runs tools/check-binary-content.py on the finished stage.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAMS = [
    ("shift-launcher", "launcher"),
    ("carview", "carview"),
    ("carbake", "carbake"),
    ("soundbake", "soundbake"),
    ("profile-import", "profile-import"),
]
PLATFORMS = {"windows": ".exe", "linux": "", "macos": "", "android": ""}


def version() -> str:
    r = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, capture_output=True, text=True)
    return r.stdout.strip() or "0.1.0"


def macos_info_plist(ver: str) -> str:
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleDisplayName</key><string>Shift Engine</string>
  <key>CFBundleExecutable</key><string>shift-launcher</string>
  <key>CFBundleIconFile</key><string>ShiftEngine</string>
  <key>CFBundleIdentifier</key><string>net.shiftengine.launcher</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>Shift Engine</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>{ver}</string>
  <key>CFBundleVersion</key><string>{ver}</string>
  <key>LSMinimumSystemVersion</key><string>13.3</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.racing-games</string>
</dict>
</plist>
"""


def moltenvk_icd_json(library_path: str) -> str:
    manifest = {
        "file_format_version": "1.0.0",
        "ICD": {
            "library_path": library_path,
            "api_version": "1.2.0",
            "is_portability_driver": True,
        },
    }
    return json.dumps(manifest, indent=2) + "\n"


def macos_launcher_wrapper() -> str:
    return """#!/bin/sh
set -eu
MACOS_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
CONTENTS_DIR="$(dirname -- "$MACOS_DIR")"
FW_DIR="$CONTENTS_DIR/Frameworks"
ICD_FW="$FW_DIR/MoltenVK_icd.json"
ICD_RES="$CONTENTS_DIR/Resources/vulkan/icd.d/MoltenVK_icd.json"
if [ -f "$ICD_FW" ]; then
  ICD_PATH="$ICD_FW"
elif [ -f "$ICD_RES" ]; then
  ICD_PATH="$ICD_RES"
else
  ICD_PATH=""
fi
if [ -n "$ICD_PATH" ]; then
  export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$ICD_PATH}"
  export VK_DRIVER_FILES="${VK_DRIVER_FILES:-$ICD_PATH}"
fi
if [ -d "$FW_DIR" ]; then
  export DYLD_FALLBACK_LIBRARY_PATH="$FW_DIR${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
fi
exec "$MACOS_DIR/shift-launcher-bin" "$@"
"""


def find_built_program(build: Path, prog: str, subdir: str, exe: str) -> Path | None:
    for cand in (build / "apps" / subdir / (prog + exe), build / "apps" / prog / (prog + exe), build / (prog + exe)):
        if cand.is_file():
            return cand
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "out" / "release")
    ap.add_argument("--moltenvk", type=Path, help="macOS: path to libMoltenVK.dylib / libvulkan.1.dylib to bundle")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--game-root", type=Path)
    ap.add_argument("--state-root", type=Path)
    a = ap.parse_args()

    exe = PLATFORMS[a.platform]
    ver = version()
    name = f"shift-engine-{a.platform}-{ver}"
    stage = a.out / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    prog_names = {p for p, _ in PROGRAMS}

    if a.platform == "macos":
        app = stage / "Shift Engine.app" / "Contents"
        bin_dir = app / "MacOS"
        fw_dir = app / "Frameworks"
        res_dir = app / "Resources"
        icd_dir = res_dir / "vulkan" / "icd.d"
        bin_dir.mkdir(parents=True)
        fw_dir.mkdir(parents=True)
        icd_dir.mkdir(parents=True)
        (app / "Info.plist").write_text(macos_info_plist(ver), encoding="utf-8", newline="\n")
        for prog, subdir in PROGRAMS:
            src = find_built_program(a.build, prog, subdir, exe)
            if not src:
                print(f"package: missing {prog} in {a.build}")
                return 1
            dst_name = "shift-launcher-bin" if prog == "shift-launcher" else src.name
            shutil.copy2(src, bin_dir / dst_name)
        wrapper = bin_dir / "shift-launcher"
        wrapper.write_text(macos_launcher_wrapper(), encoding="utf-8", newline="\n")
        os.chmod(wrapper, 0o755)
        if a.moltenvk:
            if not a.moltenvk.is_file():
                print(f"package: missing --moltenvk file {a.moltenvk}")
                return 1
            shutil.copy2(a.moltenvk, fw_dir / a.moltenvk.name)
            # Bundle sibling Vulkan loader dylib if present next to libMoltenVK.dylib.
            sibling_vk = a.moltenvk.parent / "libvulkan.1.dylib"
            if sibling_vk.is_file() and sibling_vk.name != a.moltenvk.name:
                shutil.copy2(sibling_vk, fw_dir / sibling_vk.name)
            # Generate MoltenVK ICD manifests both next to the dylib and in standard bundle Resources/vulkan/icd.d/.
            (fw_dir / "MoltenVK_icd.json").write_text(
                moltenvk_icd_json(f"./{a.moltenvk.name}"), encoding="utf-8", newline="\n"
            )
            (icd_dir / "MoltenVK_icd.json").write_text(
                moltenvk_icd_json(f"../../../Frameworks/{a.moltenvk.name}"), encoding="utf-8", newline="\n"
            )
        gcdb = ROOT / "config" / "gamecontrollerdb.txt"
        if gcdb.is_file():
            shutil.copy2(gcdb, res_dir / gcdb.name)
        icns = ROOT / "assets" / "branding" / "ShiftEngine.icns"
        if icns.is_file():
            shutil.copy2(icns, res_dir / icns.name)
    elif a.platform == "android":
        apk = a.out / "shift-drive-arm64.apk"
        if apk.is_file():
            shutil.copy2(apk, stage / apk.name)
        else:
            so = a.build / "apps" / "carview" / "libmain.so"
            if so.is_file():
                shutil.copy2(so, stage / so.name)
            else:
                print(f"package: missing {apk} and {so} (run tools/android-apk.py first)")
                return 1
    else:
        for prog, subdir in PROGRAMS:
            src = find_built_program(a.build, prog, subdir, exe)
            if not src:
                print(f"package: missing {prog}{exe} in {a.build}")
                return 1
            shutil.copy2(src, stage / src.name)

    for f in ["tools/setup.py", "tools/setup.sh", "tools/setup.ps1", "LICENSE", "THIRD_PARTY_NOTICES.md",
              "config/gamecontrollerdb.txt"]:
        p = ROOT / f
        if p.is_file():
            shutil.copy2(p, stage / p.name)
    player = ROOT / "docs" / "player"
    if player.is_dir():
        for f in sorted(player.glob("*.md")):
            shutil.copy2(f, stage / f.name)

    zpath = a.out / (name + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(stage.rglob("*")):
            if f.is_file():
                info = zipfile.ZipInfo.from_file(f, f"{name}/{f.relative_to(stage).as_posix()}")
                run = f.stem in prog_names or f.stem == "shift-launcher-bin" or f.suffix in (".sh", ".py")
                info.external_attr = (0o100755 if run else 0o100644) << 16
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, f.read_bytes())
    print(f"package: {zpath} ({zpath.stat().st_size / 2**20:.1f} MiB)")

    if a.check:
        if not a.game_root:
            print("package: --check needs --game-root")
            return 1
        cmd = [sys.executable, str(ROOT / "tools" / "check-binary-content.py"), str(stage),
               "--game-root", str(a.game_root)]
        if a.state_root:
            cmd += ["--state-root", str(a.state_root)]
        return subprocess.run(cmd).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
