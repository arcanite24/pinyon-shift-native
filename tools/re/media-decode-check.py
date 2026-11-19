#!/usr/bin/env python3
"""Does FFmpeg decode the disc's movies and audio (REIMPLEMENTATION_PLAN RI-0.6)?

Two checks, both read-only on the game files:

1. Movies. Every `.wmv` under the disc's `media/` (and the DLC packages with
   `--dlc`): `ffprobe` gives the container, codecs, resolution, frame rate
   and duration; `ffmpeg -f null` decodes every stream in full and the
   messages it prints at warning level or above are kept as decode errors.

2. Audio. Every loose FSB4 bank (`.fsb`) is listed from its header (the
   layout tools/fh1_audio_decode.cpp reads: 48-byte bank header, 80-byte
   sample headers, data 32-byte aligned), which gives the codec mix (XMA,
   MPEG, PCM16) per bank category (the folder under `media/audio`). A sample
   of banks per category is then decoded two ways:

   - with the project's decoder, `pinyon_shift_fh1_audio_decode` (FFmpeg's
     MP3 decoder for MPEG, the SDK's XMA frame decoder for XMA), when a
     build has it (`cmake --build <dir> --target pinyon_shift_fh1_audio_decode`);
   - with stock FFmpeg: an XMA sample is wrapped as a RIFF WAVE with an
     XMA2WAVEFORMATEX header (format 0x166, one stream, 2048-byte packets)
     and decoded by FFmpeg's `xma2` decoder; an MPEG sample's frames, with
     the padding between them removed, are fed to the `mp3` demuxer.

   Banks are sampled per category and main codec (`--per-category` of each).

   For each decoded sample the decoded length is compared with the length
   the sample header gives, and, where both decoders ran, the two outputs
   are compared over the first seconds (the signal-to-difference ratio after
   the best alignment within a few thousand samples).

Decoded audio goes to a temporary folder under `.local/re` and is deleted at
the end. Only names, codecs, counts and timings are recorded.

    python -I tools/re/media-decode-check.py
    python -I tools/re/media-decode-check.py --dlc --per-category 6 --decoder <exe>

Output: JSON (default .local/re/media-decode.json) and a summary on stdout.
"""
from __future__ import annotations

import argparse
import array
import collections
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_STATE = Path(os.environ.get("LOCALAPPDATA", "")) / "PinyonShift/source/0.1.0/.local/preview"
DLC_CONTENT = Path("user/0000000000000000/4D5309C9/00000002")
SCHEMA = "pinyon-shift.media-decode.v1"

MODE_PCM16 = 0x00000010
MODE_MPEG = 0x00000200
MODE_XMA = 0x01000000
MODE_BIG_ENDIAN = 0x08000000
BANK_HEADER = 48
SAMPLE_HEADER = 80
DATA_ALIGN = 32
XMA_PACKET = 2048
COMPARE_SECONDS = 2
# Informational FFmpeg messages that say nothing about the decode.
BENIGN = ("Estimating duration from bitrate",)


# ---- FSB4 ----------------------------------------------------------------------


def read_bank(path: Path) -> list[dict]:
    """The sample headers of an FSB4 bank (fh1_audio_decode.cpp ReadBank)."""
    with path.open("rb") as handle:
        head = handle.read(BANK_HEADER)
        if len(head) < BANK_HEADER or head[:4] != b"FSB4":
            raise ValueError("not an FSB4 bank")
        count, header_size = struct.unpack_from("<II", head, 4)
        headers = handle.read(header_size)
    size = path.stat().st_size
    samples = []
    at = 0
    offset = (BANK_HEADER + header_size + DATA_ALIGN - 1) & ~(DATA_ALIGN - 1)
    for index in range(count):
        if at + SAMPLE_HEADER > len(headers):
            raise ValueError("sample header past the header block")
        (record,) = struct.unpack_from("<H", headers, at)
        if record < SAMPLE_HEADER:
            raise ValueError("short sample header")
        name = headers[at + 2:at + 32].split(b"\0", 1)[0].decode("latin-1")
        length_samples, length_bytes = struct.unpack_from("<II", headers, at + 32)
        mode, frequency = struct.unpack_from("<Ii", headers, at + 48)
        (channels,) = struct.unpack_from("<H", headers, at + 62)
        if offset + length_bytes > size:
            raise ValueError("sample data past the end of the bank")
        samples.append({"index": index, "name": name, "length_samples": length_samples,
                        "length_bytes": length_bytes, "mode": mode, "rate": frequency,
                        "channels": channels, "offset": offset, "codec": codec_of(mode)})
        offset = (offset + length_bytes + DATA_ALIGN - 1) & ~(DATA_ALIGN - 1)
        at += record
    return samples


def codec_of(mode: int) -> str:
    if mode & MODE_XMA:
        return "xma"
    if mode & MODE_MPEG:
        return "mpeg"
    if mode & MODE_PCM16:
        return "pcm16"
    return "unknown"


def category_of(path: Path, media: Path) -> str:
    try:
        parts = path.relative_to(media).parts
    except ValueError:
        return "dlc"
    if parts[0].lower() == "audio" and len(parts) > 2:
        return parts[1].lower()
    return parts[0].lower()


def xma2_wave(sample: dict, data: bytes) -> bytes:
    """A RIFF WAVE with an XMA2WAVEFORMATEX header around one sample's packets."""
    channels = sample["channels"]
    mask = 0x4 if channels == 1 else 0x3
    packets = len(data) // XMA_PACKET
    extra = struct.pack("<HIIIIIIIBBH", 1, mask, sample["length_samples"], len(data), 0,
                        sample["length_samples"], 0, 0, 0, 4, 1)
    fmt = struct.pack("<HHIIHHH", 0x166, channels, sample["rate"], sample["rate"] * channels * 2,
                      XMA_PACKET, 16, len(extra)) + extra
    body = data[:packets * XMA_PACKET]
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(body)) + body
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks


MPEG_BITRATES = ((0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0),
                 (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0))
MPEG_RATES = ((11025, 12000, 8000), (0, 0, 0), (22050, 24000, 16000), (44100, 48000, 32000))


def mpeg_frames(data: bytes) -> bytes:
    """The Layer III frames of an FSB MPEG sample without the padding FSB puts
    between them (the walk in fh1_audio_decode.cpp DecodeMpeg). FFmpeg's mp3
    demuxer needs two adjacent frames to lock on, which padded short samples
    do not give it."""
    out = bytearray()
    at = 0
    while at + 4 <= len(data):
        h = data[at:at + 4]
        version, layer = (h[1] >> 3) & 3, (h[1] >> 1) & 3
        bitrate, rate_index = h[2] >> 4, (h[2] >> 2) & 3
        if (h[0] != 0xFF or (h[1] & 0xE0) != 0xE0 or version == 1 or layer != 1
                or rate_index == 3 or not MPEG_BITRATES[version != 3][bitrate]):
            at += 1
            continue
        kbps = MPEG_BITRATES[version != 3][bitrate] * 1000
        length = (144 if version == 3 else 72) * kbps // MPEG_RATES[version][rate_index] + ((h[2] >> 1) & 1)
        if at + length > len(data):
            break
        out += data[at:at + length]
        at += length
    return bytes(out)


def ffmpeg_decode(ffmpeg: str, args: list[str]) -> tuple[bytes, list[str], int]:
    completed = subprocess.run([ffmpeg, "-hide_banner", "-nostdin", "-v", "warning", *args,
                                "-f", "s16le", "-acodec", "pcm_s16le", "-"],
                               capture_output=True)
    messages = [line for line in completed.stderr.decode("utf-8", "replace").splitlines()
                if line and not any(text in line for text in BENIGN)]
    return completed.stdout, messages, completed.returncode


def read_wav_pcm(path: Path) -> tuple[bytes, int]:
    data = path.read_bytes()
    at = 12
    channels = 1
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
        if tag == b"fmt ":
            channels = struct.unpack_from("<H", data, at + 10)[0]
        if tag == b"data":
            return data[at + 8:at + 8 + size], channels
        at += 8 + size + (size & 1)
    return b"", channels


def compare(a_bytes: bytes, b_bytes: bytes, channels: int, rate: int) -> dict:
    """Signal-to-difference ratio of b against a over COMPARE_SECONDS, after the
    best lag within +-4096 frames (searched on a decimated mono signal)."""
    a = array.array("h")
    b = array.array("h")
    span = 2 * channels * (COMPARE_SECONDS * rate + 8192)
    a.frombytes(a_bytes[:span - span % 2])
    b.frombytes(b_bytes[:span - span % 2])
    if sys.byteorder != "little":
        a.byteswap()
        b.byteswap()
    frames = min(len(a), len(b)) // channels
    if frames < 8192:
        return {"compared": False, "reason": "short"}
    step, max_lag = 8, 4096
    am = a[0::channels * step]
    bm = b[0::channels * step]
    window = min(len(am), len(bm), rate // step) - max_lag // step - 1
    best_lag, best = 0, None
    for lag in range(-max_lag // step, max_lag // step + 1):
        total = 0
        for i in range(max(0, -lag), window, 4):
            total += am[i] * bm[i + lag]
        if best is None or total > best:
            best, best_lag = total, lag

    def sdr(lag: int):
        signal = noise = 0
        first = max(0, -lag)
        count = min(COMPARE_SECONDS * rate, frames - abs(lag) - first)
        for f in range(first, first + count):
            for c in range(channels):
                x = a[f * channels + c]
                d = x - b[(f + lag) * channels + c]
                signal += x * x
                noise += d * d
        return signal, noise, count

    results = []
    for lag in range(best_lag * step - step, best_lag * step + step + 1):
        signal, noise, count = sdr(lag)
        results.append((signal / noise if noise else float("inf"), lag, signal, count))
    ratio, lag, signal, count = max(results)
    if signal == 0:
        return {"compared": True, "lag_frames": lag, "frames": count, "sdr_db": None,
                "note": "silent"}
    return {"compared": True, "lag_frames": lag, "frames": count,
            "sdr_db": "identical" if ratio == float("inf") else round(10 * math.log10(ratio), 1)}


def check_banks(args, media: Path, banks: list[Path], decoder: Path | None, ffmpeg: str,
                work: Path) -> dict:
    mix = collections.defaultdict(lambda: collections.defaultdict(lambda: {"samples": 0, "bytes": 0}))
    bank_rows = []
    errors = []
    by_category = collections.defaultdict(list)
    for bank in banks:
        category = category_of(bank, media)
        try:
            samples = read_bank(bank)
        except (OSError, ValueError, struct.error) as error:
            errors.append({"bank": bank.name, "error": str(error)})
            continue
        for s in samples:
            m = mix[category][s["codec"]]
            m["samples"] += 1
            m["bytes"] += s["length_bytes"]
            if s["codec"] == "xma" and s["channels"] > 2:
                mix[category]["xma_over_2_channels"]["samples"] += 1
        # Stratified by the bank's main codec, so every (category, codec) cell is sampled.
        main = collections.Counter(s["codec"] for s in samples).most_common(1)
        by_category[(category, main[0][0] if main else "empty")].append((bank, samples))

    chosen = []
    for (category, _), items in sorted(by_category.items()):
        items.sort(key=lambda item: item[0].as_posix().lower())
        if len(items) <= args.per_category:
            chosen += [(category, *item) for item in items]
        else:
            stride = len(items) / args.per_category
            chosen += [(category, *items[int(i * stride)]) for i in range(args.per_category)]

    totals = collections.defaultdict(lambda: collections.Counter())
    for category, bank, samples in chosen:
        row = {"bank": bank.relative_to(media).as_posix() if bank.is_relative_to(media) else bank.name,
               "category": category, "samples": len(samples),
               "codecs": dict(collections.Counter(s["codec"] for s in samples)), "results": []}
        sdk = {}
        folder = work / f"sdk_{len(bank_rows)}"
        if decoder:
            folder.mkdir(parents=True, exist_ok=True)
            started = time.perf_counter()
            for s in samples[:args.samples_per_bank]:
                completed = subprocess.run([str(decoder), str(bank), str(folder), "--sample",
                                            str(s["index"])], capture_output=True,
                                           text=True, encoding="utf-8", errors="replace")
                for line in completed.stdout.splitlines():
                    try:
                        record = json.loads(line)
                    except ValueError:
                        continue
                    record["exit"] = completed.returncode
                    sdk[record.get("index")] = record
            row["sdk_seconds"] = round(time.perf_counter() - started, 2)
        data = bank.read_bytes()
        for s in samples[:args.samples_per_bank]:
            result = {"index": s["index"], "codec": s["codec"], "channels": s["channels"],
                      "rate": s["rate"], "length_samples": s["length_samples"]}
            pcm_sdk = None
            if decoder:
                record = sdk.get(s["index"], {})
                ok = "file" in record
                result["sdk_ok"] = ok
                if ok:
                    pcm_sdk, ch = read_wav_pcm(folder / record["file"])
                    result["sdk_frames"] = len(pcm_sdk) // (2 * max(1, ch))
                else:
                    result["sdk_error"] = record.get("error", "no output")
                totals[(category, s["codec"])]["sdk_ok" if ok else "sdk_fail"] += 1
            payload = data[s["offset"]:s["offset"] + s["length_bytes"]]
            pcm_ff = None
            if s["codec"] in ("xma", "mpeg"):
                source = work / f"s_{len(bank_rows)}_{s['index']}"
                if s["codec"] == "xma":
                    source = source.with_suffix(".wav")
                    source.write_bytes(xma2_wave(s, payload))
                    pcm_ff, messages, code = ffmpeg_decode(ffmpeg, ["-i", str(source)])
                else:
                    source = source.with_suffix(".mp3")
                    source.write_bytes(mpeg_frames(payload))
                    pcm_ff, messages, code = ffmpeg_decode(ffmpeg, ["-f", "mp3", "-i", str(source)])
                source.unlink(missing_ok=True)
                ok = code == 0 and len(pcm_ff) > 0
                result["ffmpeg_ok"] = ok
                result["ffmpeg_frames"] = len(pcm_ff) // (2 * max(1, s["channels"]))
                result["ffmpeg_messages"] = len(messages)
                if messages:
                    result["ffmpeg_first_message"] = messages[0][:160]
                totals[(category, s["codec"])]["ffmpeg_ok" if ok else "ffmpeg_fail"] += 1
                if messages:
                    totals[(category, s["codec"])]["ffmpeg_with_messages"] += 1
            elif s["codec"] == "pcm16":
                totals[(category, s["codec"])]["stored_pcm"] += 1
            if pcm_sdk and pcm_ff and args.compare:
                # Long samples (music) are compared in their middle, past any lead-in silence.
                frame = 2 * max(1, s["channels"])
                middle = 0
                if s["length_samples"] > 4 * COMPARE_SECONDS * s["rate"]:
                    middle = (s["length_samples"] // 2) * frame
                result["sdk_vs_ffmpeg"] = compare(pcm_sdk[middle:], pcm_ff[middle:],
                                                  max(1, s["channels"]), s["rate"])
                result["sdk_vs_ffmpeg"]["from_frame"] = middle // frame
            row["results"].append(result)
        if decoder:
            shutil.rmtree(folder, ignore_errors=True)
        bank_rows.append(row)
        print(f"  {category:12s} {row['bank']}: {len(row['results'])} samples checked", file=sys.stderr)
    return {
        "banks_listed": len(banks), "bank_errors": errors,
        "codec_mix": {c: {k: dict(v) for k, v in m.items()} for c, m in sorted(mix.items())},
        "banks_checked": bank_rows,
        "totals": {f"{c}/{k}": dict(v) for (c, k), v in sorted(totals.items())},
    }


# ---- movies --------------------------------------------------------------------


def check_movies(movies: list[Path], media: Path, ffmpeg: str, ffprobe: str) -> list[dict]:
    rows = []
    for movie in movies:
        probe = subprocess.run([ffprobe, "-v", "error", "-show_format", "-show_streams", "-of", "json",
                                str(movie)], capture_output=True, text=True, encoding="utf-8",
                               errors="replace")
        info = json.loads(probe.stdout or "{}")
        streams = [{"type": s.get("codec_type"), "codec": s.get("codec_name"),
                    "profile": s.get("profile"), "width": s.get("width"), "height": s.get("height"),
                    "frame_rate": s.get("r_frame_rate"), "sample_rate": s.get("sample_rate"),
                    "channels": s.get("channels"), "bit_rate": s.get("bit_rate")}
                   for s in info.get("streams", [])]
        started = time.perf_counter()
        decode = subprocess.run([ffmpeg, "-hide_banner", "-nostdin", "-v", "warning", "-i", str(movie),
                                 "-map", "0", "-f", "null", "-progress", "pipe:1", "-"],
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        seconds = time.perf_counter() - started
        progress = dict(line.split("=", 1) for line in decode.stdout.splitlines() if "=" in line)
        messages = [line for line in decode.stderr.splitlines()
                    if line.strip() and not any(text in line for text in BENIGN)]
        rows.append({
            "file": movie.relative_to(media).as_posix() if movie.is_relative_to(media) else movie.name,
            "bytes": movie.stat().st_size, "container": info.get("format", {}).get("format_name"),
            "duration_s": float(info.get("format", {}).get("duration", 0) or 0),
            "streams": streams, "decode_exit": decode.returncode,
            "decoded_frames": int(progress.get("frame", 0) or 0),
            "decode_seconds": round(seconds, 2),
            "decode_messages": len(messages), "first_messages": [m[:160] for m in messages[:3]],
        })
        print(f"  {rows[-1]['file']}: exit {decode.returncode}, {len(messages)} messages",
              file=sys.stderr)
    return rows


def find_decoder(explicit: Path | None) -> Path | None:
    if explicit:
        return explicit if explicit.is_file() else None
    name = "pinyon_shift_fh1_audio_decode" + (".exe" if os.name == "nt" else "")
    for folder in sorted((ROOT / "out/build").glob("*")):
        if (folder / name).is_file():
            return folder / name
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--media", type=Path, default=ROOT / ".local/game/base/media")
    parser.add_argument("--state-root", type=Path, default=DEFAULT_STATE,
                        help="installed preview state (read-only) for --dlc")
    parser.add_argument("--dlc", action="store_true", help="add the DLC packages' movies and banks")
    parser.add_argument("--decoder", type=Path, help="pinyon_shift_fh1_audio_decode (default: out/build/*)")
    parser.add_argument("--per-category", type=int, default=4, help="banks decoded per category")
    parser.add_argument("--samples-per-bank", type=int, default=12)
    parser.add_argument("--no-compare", dest="compare", action="store_false")
    parser.add_argument("--skip-movies", action="store_true")
    parser.add_argument("--out", type=Path, default=ROOT / ".local/re/media-decode.json")
    args = parser.parse_args()

    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        print("ffmpeg and ffprobe must be on PATH", file=sys.stderr)
        return 1
    version = subprocess.run([ffmpeg, "-version"], capture_output=True, text=True).stdout.splitlines()[0]
    roots = [args.media]
    if args.dlc:
        roots += sorted(p for p in (args.state_root / DLC_CONTENT).iterdir() if p.is_dir())
    movies, banks = [], []
    for root in roots:
        for path in sorted(root.rglob("*")):
            suffix = path.suffix.lower()
            if suffix == ".wmv":
                movies.append(path)
            elif suffix == ".fsb":
                banks.append(path)
    decoder = find_decoder(args.decoder)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    result = {"schema": SCHEMA, "ffmpeg": version, "decoder": str(decoder) if decoder else None,
              "inputs": {"media": str(args.media), "dlc": args.dlc,
                         "per_category": args.per_category, "samples_per_bank": args.samples_per_bank}}
    with tempfile.TemporaryDirectory(prefix="media-decode-", dir=args.out.parent) as temp:
        work = Path(temp)
        if not args.skip_movies:
            print("movies", file=sys.stderr)
            result["movies"] = check_movies(movies, args.media, ffmpeg, ffprobe)
        print("audio", file=sys.stderr)
        result["audio"] = check_banks(args, args.media, banks, decoder, ffmpeg, work)
    with args.out.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(result, handle, indent=1)
        handle.write("\n")

    for movie in result.get("movies", []):
        video = next((s for s in movie["streams"] if s["type"] == "video"), {})
        audio = next((s for s in movie["streams"] if s["type"] == "audio"), {})
        print(f"{movie['file']}: {video.get('codec')} {video.get('width')}x{video.get('height')} "
              f"{movie['duration_s']:.1f} s, audio {audio.get('codec')}, exit {movie['decode_exit']}, "
              f"{movie['decoded_frames']} frames, {movie['decode_messages']} messages")
    print("codec mix (samples):")
    for category, mix in result["audio"]["codec_mix"].items():
        print(f"  {category}: " + ", ".join(f"{k} {v['samples']}" for k, v in mix.items()))
    print("decode results:")
    for key, counts in result["audio"]["totals"].items():
        print(f"  {key}: {counts}")
    print(f"wrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
