#!/usr/bin/env python3
"""Set up the drive to play (README, Play the drive).

    python tools/setup.py <game> [--state <dir>] [--dlc-extracted <dir>] [--data <dir>]
                          [--cars a,b] [--all-sounds] [--bin <dir>] [--no-build]

<game> is the extracted disc: the folder that holds media/ (or media itself).
--state is the installed packages' tree (user/0000000000000000/...) for the
title update and DLC cars; without it only the disc's cars are baked.

It builds the tools when no build is found (tools/build-windows.ps1 on
Windows, CMake and Ninja elsewhere), bakes the cars (carbake), the engine
sound (soundbake, when FFmpeg is found) and the Colorado model index into
the play folder, writes play.txt there naming the game, and prints how to
launch. The play folder is the user-data folder by default
(%APPDATA%\\shift-engine\\play on Windows, ~/.local/share/shift-engine/play
on Linux), where carview with no arguments finds it; --data puts it
elsewhere (then run carview --play <dir>, or put carview beside it).
Nothing is written under <game> or into the repository.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""
DEFAULT_CAR = "NIS_SkylineGTRVSpecII_02"


def user_data_dir() -> Path:
    # SDL_GetPrefPath("shift-engine", "play"), as carview reads it.
    system = platform.system()
    if system == "Windows":
        return Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming")) / "shift-engine" / "play"
    if system == "Darwin":
        return Path.home() / "Library" / "Application Support" / "shift-engine" / "play"
    xdg = os.environ.get("XDG_DATA_HOME")
    return (Path(xdg) if xdg else Path.home() / ".local" / "share") / "shift-engine" / "play"


def find_bin(given):
    names = ["carbake", "carview", "soundbake"]
    candidates = [Path(given)] if given else [REPO / "build" / "apps", REPO / "build", Path(__file__).resolve().parent]
    for base in candidates:
        found = {}
        for n in names:
            for p in (base / n / (n + EXE), base / (n + EXE)):
                if p.is_file():
                    found[n] = p
                    break
        if len(found) == len(names):
            return found
    return None


def build():
    print("setup: building the tools (first time only) ...", flush=True)
    if os.name == "nt":
        cmd = ["pwsh", "-NoProfile", "-File", str(REPO / "tools" / "build-windows.ps1")]
        if not shutil.which("pwsh"):
            cmd[0] = "powershell"
        run(cmd)
    else:
        run(["cmake", "-S", str(REPO), "-B", str(REPO / "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
        run(["cmake", "--build", str(REPO / "build"), "--target", "carbake", "carview", "soundbake"])


def run(cmd, check=True, quiet=False):
    print("  $ " + " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE if quiet else None,
                       stderr=subprocess.STDOUT if quiet else None, text=True)
    if check and r.returncode != 0:
        if quiet and r.stdout:
            print(r.stdout[-3000:])
        sys.exit(f"setup: failed ({r.returncode}): {cmd[0]}")
    return r


def main():
    ap = argparse.ArgumentParser(description="Set up the drive to play.")
    ap.add_argument("game", help="the extracted disc (the folder holding media/)")
    ap.add_argument("--state", help="the installed packages' tree (title update and DLC)")
    ap.add_argument("--dlc-extracted", help="the packages' offer ZIPs already extracted")
    ap.add_argument("--data", help="the play folder (default: the user-data folder)")
    ap.add_argument("--cars", help="bake only these cars (comma-separated media names; default all)")
    ap.add_argument("--all-sounds", action="store_true", help="bake every car's engine sound (about 3 s each)")
    ap.add_argument("--bin", help="the folder with carbake, carview and soundbake")
    ap.add_argument("--no-build", action="store_true", help="do not build the tools when none are found")
    a = ap.parse_args()

    game = Path(os.path.abspath(Path(a.game).expanduser()))
    if game.name.lower() == "media":
        game = game.parent
    colorado = game / "media" / "tracks" / "colorado"
    if not (colorado / "bin.zip").is_file() or not (game / "media" / "cars").is_dir():
        sys.exit(f"setup: {game} is not an extracted disc (no media/tracks/colorado/bin.zip or media/cars)")
    data = Path(os.path.abspath(Path(a.data).expanduser())) if a.data else user_data_dir()
    data.mkdir(parents=True, exist_ok=True)

    tools = find_bin(a.bin)
    if not tools and not a.no_build and not a.bin:
        build()
        tools = find_bin(None)
    if not tools:
        sys.exit("setup: carbake, carview and soundbake not found (build them, or pass --bin)")

    t0 = time.time()
    print(f"setup: game {game}\nsetup: play folder {data}", flush=True)
    bake = data / "bake"
    cmd = [tools["carbake"], "--game-root", game, "--out", bake]
    if a.state:
        cmd += ["--state-root", Path(os.path.abspath(Path(a.state).expanduser()))]
    if a.dlc_extracted:
        cmd += ["--dlc-extracted", Path(os.path.abspath(Path(a.dlc_extracted).expanduser()))]
    if a.cars:
        cmd += ["--cars", a.cars]
    run(cmd, quiet=True)
    baked = sorted(p.name[: -len(".shiftbake")] for p in (bake / "cars").glob("*.shiftbake"))
    if not baked:
        sys.exit("setup: carbake wrote no cars")
    print(f"setup: {len(baked)} cars baked", flush=True)
    car = DEFAULT_CAR if DEFAULT_CAR.lower() in baked else baked[0]

    # The engine sound: FFmpeg decodes it at bake time; without it the drive is silent.
    cmd = [tools["soundbake"], "--game-root", game, "--out", data / "sound"]
    if a.state:
        cmd += ["--state-root", Path(os.path.abspath(Path(a.state).expanduser()))]
    cmd += ["--all"] if a.all_sounds else ["--cars", car]
    if run(cmd, check=False, quiet=True).returncode != 0:
        print("setup: note: no engine sound (soundbake failed; is FFmpeg installed?)")

    # play.txt before the index run, which reads it.
    (data / "play.txt").write_text(f"game {game}\n", encoding="utf-8")

    # The Colorado model index (about 10 s), with one headless frame.
    (data / "world").mkdir(exist_ok=True)
    frame = data / "world" / "setup-check"
    run([tools["carview"], "--drive", "--headless", "--play", data, "--car", car, "--run", "cruise",
         "--seconds", "0.5", "--every", "1", "--no-sound", "--out", str(frame) + ".png"], quiet=True)
    for p in (data / "world").glob("setup-check*.png"):
        p.unlink()
    if not (data / "world" / "colorado.index").is_file():
        sys.exit("setup: the Colorado index was not written")

    print(f"\nsetup: done in {time.time() - t0:.0f} s. Play the drive:")
    launch = tools["carview"]
    if a.data:
        print(f"  {launch} --play {data}")
    else:
        print(f"  {launch}")
    print("  (any baked car with --car <media name>; --hour 2 for night traffic)")


if __name__ == "__main__":
    main()
