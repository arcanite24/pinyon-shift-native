#!/usr/bin/env python3
"""Make the Android APK from an Android build folder (M7).

The APK holds the drive as lib/arm64-v8a/libmain.so under SDL's activity
(apps/carview/android). It holds no game data: the player copies the play
folder that tools/setup.py makes to Android/data/net.shiftengine.drive/
files/play on the device. No Gradle: javac, d8, aapt2, zipalign and
apksigner from the SDK, a debug key made once under out/android.

    python tools/android-apk.py --build build-android [--out out/release]

ANDROID_HOME (else %LOCALAPPDATA%/Android/Sdk or ~/Android/Sdk) names the
SDK; JAVA_HOME or the PATH gives javac and keytool.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANDROID_DIR = ROOT / "apps" / "carview" / "android"
MIN_API, TARGET_API = 29, 35
EXE = ".exe" if os.name == "nt" else ""
BAT = ".bat" if os.name == "nt" else ""


def sdk_root() -> Path:
    if os.environ.get("ANDROID_HOME"):
        return Path(os.environ["ANDROID_HOME"])
    if os.name == "nt":
        return Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk"
    return Path.home() / "Android" / "Sdk"


def newest(folder: Path) -> Path:
    dirs = sorted((d for d in folder.iterdir() if d.is_dir()), key=lambda d: [int(x) if x.isdigit() else 0 for x in d.name.split(".")])
    if not dirs:
        raise SystemExit(f"android-apk: nothing in {folder}")
    return dirs[-1]


def java_tool(name: str) -> str:
    if os.environ.get("JAVA_HOME"):
        p = Path(os.environ["JAVA_HOME"]) / "bin" / (name + EXE)
        if p.is_file():
            return str(p)
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"android-apk: {name} not found (set JAVA_HOME)")
    return found


def run(cmd: list[str]) -> None:
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"android-apk: failed: {Path(str(cmd[0])).name}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "out" / "release")
    a = ap.parse_args()

    so = a.build / "apps" / "carview" / "libmain.so"
    if not so.is_file():
        print(f"android-apk: missing {so} (configure the Android build without SHIFT_ANDROID_EXECUTABLE)")
        return 1
    sdl_java = a.build / "_deps" / "sdl3-src" / "android-project" / "app" / "src" / "main" / "java"
    if not sdl_java.is_dir():
        print(f"android-apk: missing SDL's Java sources at {sdl_java}")
        return 1
    sdk = sdk_root()
    tools = newest(sdk / "build-tools")
    android_jar = sdk / "platforms" / f"android-{TARGET_API}" / "android.jar"
    if not android_jar.is_file():
        print(f"android-apk: missing {android_jar}")
        return 1
    strip = None
    ndk = sdk / "ndk"
    if ndk.is_dir():
        for host in ("windows-x86_64", "linux-x86_64", "darwin-x86_64"):
            p = newest(ndk) / "toolchains" / "llvm" / "prebuilt" / host / "bin" / ("llvm-strip" + EXE)
            if p.is_file():
                strip = p
                break

    a.out.mkdir(parents=True, exist_ok=True)
    keystore = ROOT / "out" / "android" / "debug.keystore"
    if not keystore.is_file():
        keystore.parent.mkdir(parents=True, exist_ok=True)
        run([java_tool("keytool"), "-genkeypair", "-keystore", keystore, "-storepass", "android", "-keypass",
             "android", "-alias", "shiftdebug", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
             "-dname", "CN=Shift Engine Debug"])

    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        classes, dex = t / "classes", t / "dex"
        classes.mkdir()
        dex.mkdir()
        sources = sorted(sdl_java.rglob("*.java")) + sorted((ANDROID_DIR / "java").rglob("*.java"))
        run([java_tool("javac"), "-nowarn", "-encoding", "UTF-8", "-source", "11", "-target", "11",
             "-classpath", android_jar, "-d", classes] + sources)
        run([tools / ("d8" + BAT), "--release", "--min-api", str(MIN_API), "--lib", android_jar, "--output", dex]
            + sorted(classes.rglob("*.class")))
        base = t / "base.apk"
        run([tools / ("aapt2" + EXE), "link", "-o", base, "--manifest", ANDROID_DIR / "AndroidManifest.xml", "-I",
             android_jar, "--min-sdk-version", str(MIN_API), "--target-sdk-version", str(TARGET_API)])
        lib = t / "libmain.so"
        if strip:
            run([strip, "--strip-unneeded", "-o", lib, so])
        else:
            shutil.copy2(so, lib)
        with zipfile.ZipFile(base, "a") as z:
            z.write(dex / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
            # Stored, so the loader maps it from the APK (extractNativeLibs false).
            z.write(lib, "lib/arm64-v8a/libmain.so", compress_type=zipfile.ZIP_STORED)
        aligned = t / "aligned.apk"
        run([tools / ("zipalign" + EXE), "-P", "16", "-f", "4", base, aligned])
        apk = a.out / "shift-drive-arm64.apk"
        run([tools / ("apksigner" + BAT), "sign", "--ks", keystore, "--ks-pass", "pass:android", "--ks-key-alias",
             "shiftdebug", "--out", apk, aligned])
        run([tools / ("apksigner" + BAT), "verify", apk])
    print(f"android-apk: {apk} ({apk.stat().st_size / 2**20:.1f} MiB), lib {lib_size(so, strip)}")
    return 0


def lib_size(so: Path, strip) -> str:
    return f"{so.stat().st_size / 2**20:.1f} MiB unstripped" + (", stripped in the APK" if strip else "")


if __name__ == "__main__":
    sys.exit(main())
