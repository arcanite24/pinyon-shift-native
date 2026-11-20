#!/usr/bin/env python3
"""The radio's stations, tracks and clips on the player's disc (SPEC-RADIO).

Reads the player's own files, read-only, and prints only names, counts and
durations:

- `media/audio/radio/RadioSystem.xml`: the stations, their playlists, idents
  and DJ dialogue groups, and the `System` levels and fade times;
- `Radio_Music.fsb` (music) and `Radio_VO_<lang>.fsb` (idents and DJ lines),
  FSB4 banks of MPEG samples;
- `RadioSoundbankInfo_Music.xml` and `RadioSoundbankInfo_VO_<lang>.xml`.

    python -I tools/re/radio.py census [--game-root .local/game/base] [--lang EN]
    python -I tools/re/radio.py decode --out .local/re/radio [--rate 8000]
    python tools/re/radio.py match <capture.wav> [...] --pcm .local/re/radio

`census` resolves every name a station uses to a bank sample, counts tracks,
idents and DJ lines per station, and decodes a sample per station with
FFmpeg to check it gives the declared length (`--decode-all` decodes every
sample). `decode` writes every track, ident and DJ line of Radio1-3 as mono
16-bit PCM at a low rate for `match`. `match` needs numpy (run it without `-I` if numpy
is only in the user site): it cuts each capture of the native mix
(`audiocapture`, 6 channels) into windows and finds the track and offset
each window plays, by cross-correlation against the decoded tracks.

RadioSystem.xml is not strict XML: it has bare `&` in artist names and an
attribute named `3dPanLevel`. The reader escapes the first and renames the
second before parsing. Track entries spell the attribute `fmodname`, idents
and dialogue `fmodName`.
"""

from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import re
import shutil
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RADIO = Path("media/audio/radio")
_spec = importlib.util.spec_from_file_location("media_decode_check", REPO / "tools/re/media-decode-check.py")
mdc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mdc)

SELECTABLE = ("Radio1", "Radio2", "Radio3")
GROUPS = ("Playlist", "Idents", "DJBookendDialogueIn", "DJBookendDialogueOut", "DJRegularDialogue",
          "DJSpecialDialogue", "DJImmediateDialogue", "DJFestivalUpdateLeadIn", "DJFestivalUpdateLeadOut")


def read_system(path: Path) -> ET.Element:
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"&(?!amp;|lt;|gt;|quot;|apos;)", "&amp;", text)
    text = text.replace(" 3dPanLevel=", " pan3dLevel=")
    return ET.fromstring(text)


def clip_name(element: ET.Element) -> str:
    return element.get("fmodname") or element.get("fmodName") or ""


def read_info(path: Path) -> dict[str, dict]:
    info = {}
    for bank in ET.parse(path).getroot():
        entry = bank.find("SoundBankInfo")
        info[bank.get("Name")] = {k: int(v) for k, v in entry.attrib.items()}
    return info


def load(root_dir: Path, bank: str, info: str) -> tuple[dict, dict, list]:
    """Name -> sample, resolved through the info file's SoundBankIndex: the
    bank's own 30-byte names are truncated and collide (the VO banks' DJ
    lines share their first 29 characters)."""
    samples = mdc.read_bank(root_dir / bank)
    table = read_info(root_dir / info)
    by_name = {}
    for name, entry in table.items():
        index = entry["SoundBankIndex"]
        if 0 <= index < len(samples) and name.startswith(samples[index]["name"]):
            by_name[name] = samples[index]
    return by_name, table, samples


def stations(root: ET.Element) -> list[dict]:
    out = []
    for station in root.findall("RadioStation"):
        record = {"name": station.get("name"), "attributes": dict(station.attrib), "groups": {}}
        for group in station:
            record["groups"][group.tag] = {
                "attributes": dict(group.attrib),
                "clips": [clip_name(child) for child in group],
                "tags": sorted({child.tag for child in group}),
            }
        out.append(record)
    return out


def census(args: argparse.Namespace) -> int:
    root_dir = args.game_root / RADIO
    system = read_system(root_dir / "RadioSystem.xml")
    vo_path = root_dir / f"Radio_VO_{args.lang}.fsb"
    music, music_info, music_bank = load(root_dir, "Radio_Music.fsb", "RadioSoundbankInfo_Music.xml")
    vo, vo_info, vo_bank = load(root_dir, vo_path.name, f"RadioSoundbankInfo_VO_{args.lang}.xml")
    result = {"system": dict(system.find("System").attrib), "stations": [], "banks": {}}
    used_music, used_vo = set(), set()
    print("station                 tracks  min:s total  idents  DJ lines (regular/special/immediate/festival in+out)  unresolved")
    for station in stations(system):
        g = station["groups"]
        tracks = g.get("Playlist", {}).get("clips", [])
        idents = g.get("Idents", {}).get("clips", [])
        missing = []
        seconds = 0.0
        for name in tracks:
            sample = music.get(name)
            if sample is None:
                missing.append(name)
                continue
            used_music.add(name)
            seconds += sample["length_samples"] / sample["rate"]
        for tag, group in g.items():
            if tag == "Playlist":
                continue
            for name in group["clips"]:
                if name in vo:
                    used_vo.add(name)
                elif name in music:
                    used_music.add(name)
                else:
                    missing.append(name)
        dj = {tag: len(group["clips"]) for tag, group in g.items() if tag.startswith("DJ")}
        blank = sum(1 for tag, group in g.items() if tag != "Playlist"
                    for name in group["clips"] if name in ("DJ_Blank", "Ident_Blank"))
        channels = collections.Counter(music[n]["channels"] for n in tracks if n in music)
        record = {"name": station["name"], "attributes": station["attributes"],
                  "tracks": len(tracks), "distinct_tracks": len(set(tracks)), "seconds": round(seconds, 1),
                  "idents": len(idents), "blank_clips": blank, "dj": dj,
                  "channels": dict(channels), "no_repeat": {t: grp["attributes"].get("noRepeat") for t, grp in g.items()},
                  "unresolved": missing}
        result["stations"].append(record)
        print(f"{station['name']:22} {len(tracks):7} {int(seconds // 60):4}:{int(seconds % 60):02} {len(idents):7}   "
              f"{dj.get('DJRegularDialogue', 0)}/{dj.get('DJSpecialDialogue', 0)}/{dj.get('DJImmediateDialogue', 0)}/"
              f"{dj.get('DJFestivalUpdateLeadIn', 0)}+{dj.get('DJFestivalUpdateLeadOut', 0)}"
              f"{'':30} {len(missing)}")
    festival = [u.get("fmodName") for u in system.iter("FestivalUpdate")]
    used_vo.update(n for n in festival if n in vo)
    result["festival_updates"] = len(festival)
    result["festival_unresolved"] = [n for n in festival if n not in vo]

    # Sync points of the selectable stations' tracks and the idents.
    idents_ms, intro_ms, outro_ms = [], [], []
    for name in {n for s in result["stations"] if s["name"] in SELECTABLE
                 for n in [*next(x for x in stations(system) if x["name"] == s["name"])["groups"]["Playlist"]["clips"]]}:
        inf = music_info.get(name)
        if inf:
            intro_ms.append(inf["OffsetEventStart"])
            outro_ms.append(inf["FileLength"] - inf["OffsetIdentStart"])
    for station in stations(system):
        for name in station["groups"].get("Idents", {}).get("clips", []):
            inf = vo_info.get(name)
            if inf and station["name"] in SELECTABLE:
                idents_ms.append((inf["OffsetStartNextTrack"], inf["FileLength"]))
    def spread(values):
        values = sorted(values)
        return {"n": len(values), "min": values[0], "median": values[len(values) // 2], "max": values[-1]} if values else {}
    result["sync"] = {"event_start_ms": spread(intro_ms), "ident_start_to_end_ms": spread(outro_ms),
                      "ident_next_track_ms": spread([a for a, _ in idents_ms]),
                      "ident_length_ms": spread([b for _, b in idents_ms])}
    result["banks"] = {
        "music_samples": len(music_bank), "music_named": len(music),
        "music_unreferenced": sorted(set(music) - used_music),
        "music_codecs": dict(collections.Counter(s["codec"] for s in music_bank)),
        "music_rates": dict(collections.Counter(s["rate"] for s in music_bank)),
        "music_channels": dict(collections.Counter(s["channels"] for s in music_bank)),
        "vo_lang": args.lang, "vo_samples": len(vo_bank), "vo_named": len(vo), "vo_used": len(used_vo),
        "vo_unreferenced": len(set(vo) - used_vo),
        "vo_codecs": dict(collections.Counter(s["codec"] for s in vo_bank)),
        "vo_banks_on_disc": sorted(p.name for p in root_dir.glob("Radio_VO_*.fsb")),
        "vo_info_files": len(list(root_dir.glob("RadioSoundbankInfo_VO_*.xml"))),
    }
    print(f"festival updates {len(festival)} ({len(result['festival_unresolved'])} unresolved)")
    print("sync (ms):", json.dumps(result["sync"]))
    print("banks:", json.dumps(result["banks"]))

    # Decode check: the first track of each station (or every sample).
    ffmpeg = shutil.which("ffmpeg")
    checks = []
    if ffmpeg:
        targets = []
        for s in result["stations"]:
            clips = next(x for x in stations(system) if x["name"] == s["name"])["groups"]
            for name in (clips["Playlist"]["clips"] if args.decode_all else clips["Playlist"]["clips"][:1]):
                targets.append(("music", music[name]))
            for name in clips.get("Idents", {}).get("clips", [])[:1]:
                targets.append(("vo", vo[name]))
        with tempfile.TemporaryDirectory() as tmp:
            for kind, sample in targets:
                bank = root_dir / ("Radio_Music.fsb" if kind == "music" else vo_path.name)
                pcm = decode_sample(ffmpeg, bank, sample, Path(tmp))
                frames = len(pcm) // (2 * sample["channels"])
                checks.append({"name": sample["name"], "declared": sample["length_samples"], "decoded": frames})
        exact = sum(1 for c in checks if c["decoded"] == c["declared"])
        within = sum(1 for c in checks if abs(c["decoded"] - c["declared"]) <= 1152)
        print(f"decode: {len(checks)} samples, {exact} exact, {within} within one MPEG frame")
        result["decode"] = checks
    out = args.output or REPO / ".local/re/radio.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=1), encoding="utf-8")
    print(f"wrote {out}")
    return 0


def decode_sample(ffmpeg: str, bank: Path, sample: dict, tmp: Path, rate: int | None = None,
                  mono: bool = False) -> bytes:
    with bank.open("rb") as handle:
        handle.seek(sample["offset"])
        data = handle.read(sample["length_bytes"])
    source = tmp / "sample.mp3"
    source.write_bytes(mdc.mpeg_frames(data))
    extra = []
    if mono:
        extra += ["-ac", "1"]
    if rate:
        extra += ["-ar", str(rate)]
    pcm, _, _ = mdc.ffmpeg_decode(ffmpeg, ["-f", "mp3", "-i", str(source), *extra])
    return pcm


def decode(args: argparse.Namespace) -> int:
    root_dir = args.game_root / RADIO
    system = read_system(root_dir / "RadioSystem.xml")
    music, _, _ = load(root_dir, "Radio_Music.fsb", "RadioSoundbankInfo_Music.xml")
    vo, _, _ = load(root_dir, f"Radio_VO_{args.lang}.fsb", f"RadioSoundbankInfo_VO_{args.lang}.xml")
    ffmpeg = shutil.which("ffmpeg")
    args.out.mkdir(parents=True, exist_ok=True)
    index = {}
    with tempfile.TemporaryDirectory() as tmp:
        for station in stations(system):
            if station["name"] not in SELECTABLE:
                continue
            for tag in GROUPS:
                for name in sorted(set(station["groups"].get(tag, {}).get("clips", []))):
                    bank, sample = (("Radio_Music.fsb", music[name]) if name in music
                                    else (f"Radio_VO_{args.lang}.fsb", vo[name]))
                    pcm = decode_sample(ffmpeg, root_dir / bank, sample, Path(tmp), args.rate, mono=True)
                    (args.out / f"{name}.s16").write_bytes(pcm)
                    index[name] = {"station": station["name"], "kind": tag, "seconds": len(pcm) / 2 / args.rate}
    (args.out / "index.json").write_text(json.dumps({"rate": args.rate, "clips": index}, indent=1), encoding="utf-8")
    print(f"decoded {len(index)} clips to {args.out}")
    return 0


def match(args: argparse.Namespace) -> int:
    import numpy as np  # only this command needs it
    meta = json.loads((args.pcm / "index.json").read_text(encoding="utf-8"))
    factor = max(1, meta["rate"] // args.rate)
    rate = meta["rate"] // factor

    def decimate(signal):
        signal = signal[: len(signal) // factor * factor]
        return signal.reshape(-1, factor).mean(axis=1)

    refs = {name: decimate(np.frombuffer((args.pcm / f"{name}.s16").read_bytes(), dtype="<i2").astype(np.float64))
            for name in meta["clips"]}
    window = int(args.window * rate)
    spectra, energies = {}, {}
    for name, ref in refs.items():
        if len(ref) < window:
            continue
        n = 1 << int(np.ceil(np.log2(len(ref) + window)))
        spectra[name] = (n, np.fft.rfft(ref, n))
        squares = np.concatenate(([0.0], np.cumsum(ref ** 2)))
        energies[name] = np.sqrt(squares[window:] - squares[:-window]) + 1e-9
    ffts = {}

    def score_all(w):
        ffts.clear()
        best = []
        for name, (n, spectrum) in spectra.items():
            if n not in ffts:
                ffts[n] = np.conj(np.fft.rfft(w, n))
            corr = np.fft.irfft(spectrum * ffts[n], n)[: len(refs[name]) - window + 1]
            score = corr / energies[name]
            at = int(np.argmax(score))
            best.append((float(score[at]) / np.sqrt(window), name, at / rate))
        best.sort(reverse=True)
        return best

    def score_near(w, name, expected):
        ref = refs[name]
        lo = max(0, int((expected - 1.0) * rate))
        hi = min(len(ref) - window, int((expected + 1.0) * rate))
        if hi <= lo:
            return None
        corr = np.correlate(ref[lo:hi + window], w, "valid")
        score = corr / energies[name][lo:hi + 1]
        at = int(np.argmax(score))
        return float(score[at]) / np.sqrt(window), name, (lo + at) / rate

    for capture in args.captures:
        raw = capture.read_bytes()
        source_rate = struct.unpack_from("<I", raw, 24)[0]
        data, channels = mdc.read_wav_pcm(capture)
        x = np.frombuffer(data, dtype="<i2").astype(np.float64).reshape(-1, channels)
        x = x[:, :2].mean(axis=1) if channels >= 2 else x[:, 0]
        step = source_rate // rate
        x = x[: len(x) // step * step].reshape(-1, step).mean(axis=1)
        print(f"{capture.name}: {len(x) / rate:.1f} s, rms {float(np.sqrt((x ** 2).mean())):.1f}", flush=True)
        previous = None
        for start in range(0, len(x) - window + 1, int(args.hop * rate)):
            w = x[start:start + window]
            level = float(np.sqrt((w ** 2).mean()))
            if level < args.silence:
                print(f"  {start / rate:6.1f} s  silent (rms {level:.1f})", flush=True)
                previous = None
                continue
            w = (w - w.mean()) / (w.std() + 1e-9)
            top = None
            if previous:
                top = score_near(w, previous[1], previous[2] + args.hop)
                if top and top[0] < args.keep:
                    top = None
            note = "follows"
            if top is None:
                best = score_all(w)
                top, note = best[0], f"next {best[1][1]} r={best[1][0]:.2f}"
            previous = top
            print(f"  {start / rate:6.1f} s  {top[1]:36} at {top[2]:7.1f} s  r={top[0]:.2f}  rms {level:.0f}  {note}",
                  flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("census")
    c.add_argument("--game-root", type=Path, default=REPO / ".local/game/base")
    c.add_argument("--lang", default="EN")
    c.add_argument("--decode-all", action="store_true")
    c.add_argument("--output", type=Path)
    d = sub.add_parser("decode")
    d.add_argument("--game-root", type=Path, default=REPO / ".local/game/base")
    d.add_argument("--lang", default="EN")
    d.add_argument("--out", type=Path, default=REPO / ".local/re/radio")
    d.add_argument("--rate", type=int, default=8000)
    m = sub.add_parser("match")
    m.add_argument("captures", type=Path, nargs="+")
    m.add_argument("--pcm", type=Path, default=REPO / ".local/re/radio")
    m.add_argument("--window", type=float, default=4.0)
    m.add_argument("--hop", type=float, default=4.0)
    m.add_argument("--rate", type=int, default=2000, help="matching rate (a divisor of the decoded rate)")
    m.add_argument("--silence", type=float, default=30.0, help="window RMS below which it is silent")
    m.add_argument("--keep", type=float, default=0.5, help="score that keeps following the last track")
    args = parser.parse_args()
    return {"census": census, "decode": decode, "match": match}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
