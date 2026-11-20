"""Small PPM/PGM and PNG helpers (no third-party modules)."""
from __future__ import annotations
import struct
import zlib
from pathlib import Path


def read_ppm(path) -> tuple[int, int, bytes]:
    data = Path(path).read_bytes()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        if data[at:at + 1] == b"#":
            at = data.index(b"\n", at) + 1
            continue
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    at += 1
    width, height = int(fields[1]), int(fields[2])
    channels = 3 if fields[0] == b"P6" else 1
    return width, height, data[at:at + width * height * channels]


def write_png(path, width: int, height: int, pixels: bytes, channels: int = 3) -> None:
    kind = {1: 0, 3: 2}[channels]
    stride = width * channels
    raw = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(height))
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, kind, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    Path(path).write_bytes(png)


def read_png(path) -> tuple[int, int, int, bytes]:
    data = Path(path).read_bytes()
    at, idat, width = 8, b"", 0
    while at < len(data):
        (length,) = struct.unpack_from(">I", data, at)
        tag = data[at + 4:at + 8]
        body = data[at + 8:at + 8 + length]
        if tag == b"IHDR":
            width, height, depth, kind = struct.unpack_from(">IIBB", body)
            channels = {0: 1, 2: 3}[kind]
        elif tag == b"IDAT":
            idat += body
        at += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray()
    prev = bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return width, height, channels, bytes(out)


def downscale(width, height, pixels, factor, channels=3):
    w, h = width // factor, height // factor
    out = bytearray()
    for y in range(h):
        row = pixels[(y * factor) * width * channels:(y * factor + 1) * width * channels]
        for x in range(w):
            out += row[x * factor * channels:x * factor * channels + channels]
    return w, h, bytes(out)
