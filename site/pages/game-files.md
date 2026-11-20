---
title: Files and formats
section: Reverse engineering
order: 9
description: The layout of Forza Horizon's media folder and the formats inside it: XMem LZX ZIP archives and their payload-offset field, LSB2 string tables, XDS textures, the SQLite game database and its merge files, UI scene files and FMOD banks, with code to read them.
---

# Files and formats

The disc's `media/` folder is 7.18 GB in 2,385 files. Most of it is 524 ZIP archives holding 258,936 members, nearly all compressed with the Xbox's XMem LZX codec, which ordinary ZIP tools list but cannot extract. The rest is loose FMOD sound banks, XML, compiled shaders and one SQLite database.

| Folder | Size | What is in it |
| --- | ---: | --- |
| `tracks/` | 2.9 GB | Colorado's scenery archive (230,057 members) and the UI studio tracks ([World and tracks](game-world.md)) |
| `cars/` | 1.5 GB | one archive per car: `.carbin` models, `.xds` textures, livery masks, physics XML |
| `audio/` | 1.4 GB | 984 FMOD `.fsb` banks and 34 `.fev` event files, the radio ([Audio and radio](game-audio.md)) |
| `ui/` | 0.7 GB | textures, fonts and UI scene archives ([UI, HUD and input](game-ui.md)) |
| `wheels/` | 135 MB | rim models |
| `stringtables/` | 7 MB | one archive of `.str` tables per language |
| `db/` | 3 MB | `gamedb.slt`, the game database |
| top level | | `gamemodes.zip` (flow scripts), `renderscenarios.zip`, `camera.zip`, `physics.zip`, `Livery.zip`, `UI.zip` and loose XML |

The v4 title update adds one `media.zip` whose members the game prefers over the disc's ([Title update v4](game-title-update.md)).

## Media layout and file access

{{findings:files/Media layout and file access}}

## Archives

{{diagram:game-archive-format|An FH1 archive. The game finds each member's data through extra field 0x1123 in the central directory, not through the local header, so a repacked archive must rewrite that field.}}

The extra field is easy to read with Python's `zipfile`, which lists method-21 members even though it cannot decompress them:

```python
import struct, zipfile

def payload_offsets(path):
    """(name, method, size, payload offset) for each member of an FH1 archive."""
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            extra, pos = info.extra, 0
            while pos + 4 <= len(extra):
                tag, size = struct.unpack_from("<HH", extra, pos)
                if tag == 0x1123:  # little-endian u32: absolute payload offset
                    yield (info.filename, info.compress_type, info.file_size,
                           struct.unpack_from("<I", extra, pos + 4)[0])
                pos += 4 + size

for name, method, size, offset in payload_offsets("media/gamemodes.zip"):
    print(f"{offset:#010x} method {method:2} {size:8} {name}")
```

Where a local header exists, the offset equals the header's position plus its 30 bytes, name and extra field; a method-21 payload then starts with a frame header (`0xFF` and two big-endian sizes for a short frame). To decompress, use `pinyon_shift_fh1_archive_extract` ([Methods and tools](game-methods.md#data-files)) or an LZX decoder with a 64 KiB window over the joined frame bodies.

{{findings:files/Archives}}

## String tables

All UI text is in `.str` tables, one archive per language under `media/stringtables/`. A string is found by a 16-bit key; the game database refers to strings as `_&N`, where N combines a table hash and the key ([Cars, events and DLC](game-content.md#game-database)). This reads a table's directory:

```python
import struct

def read_str_table(data: bytes) -> dict[int, str]:
    """Key -> text for one extracted .str (LSB2) table."""
    assert data[:4] == b"LSB2"
    chunk = struct.unpack_from(">I", data, 12)[0]          # offset of the string chunk
    count = struct.unpack_from(">I", data, chunk + 4)[0]
    entries = [struct.unpack_from(">HI", data, chunk + 8 + 6 * i) for i in range(count + 1)]
    pool = chunk + 8 + 6 * (count + 1)                      # NUL-terminated UTF-16BE strings
    strings = {}
    for key, offset in entries[:-1]:                        # the last entry is the 0xFFFF sentinel
        start = pool + 2 * offset
        end = start
        while data[end:end + 2] != b"\0\0":
            end += 2
        strings[key] = data[start:end].decode("utf-16-be")
    return strings
```

`tools/fh1-strings.py` does the same with extraction included, and mods replace an entry with `set_ui_string`.

{{findings:files/String tables}}

## Textures and models

A car's models are `.carbin` files in its archive: one for LOD1-5, one for LOD0, the cockpit, and a caliper and a rotor per wheel. Each is a list of named parts (body, hood, bumpers, glass, lights). A part has a box, a pool of 28-byte vertices and one index buffer per material and LOD. A vertex stores its position relative to the box's offset as a direction and a length: a unit vector in three 16-bit values, and a fourth value giving the length as a fraction of the length of the box's half-extent vector. The game passes the offset and that length to its vertex shader as one constant per part, so the boxes are tight:

```text
position = (x, y, z) / 32767 * w / 32767 * |half_extent| + offset
```

`tools/fh1_carbin.py` reads every base car's models and writes them back byte for byte (`roundtrip`), and `dump` lists the parts. It is the first step of the car importer ([#496](https://github.com/arcanite24/pinyon-shift/issues/496)). `set_positions` refits a part's box to new positions and re-encodes them; an XKR-S rewritten 1.2 times wider this way is clean in game. Decoding each axis with its own half-extent looks right for an unchanged model but breaks as soon as a box changes, which is what an Autoshow frame dump sorted out.

A model's header ends in a block of draw lists: for each LOD, the sub-sections drawn with blending (glass, lamp covers, blurred rims) and the opaque ones. Every sub-section is listed once, so the lists also say where the parts start. With it, all 2,587 models of the 176 disc cars and the 64 cars only the official DLC carries parse and write back, and all of their 7,441 textures lay out exactly. The wheels are separate archives named by the car's stock rim, with three shared tyre models that the vertex shader stretches to the car's tyre sizes. The format, the materials, the paint and the wheel placement are written up for the native engine's car viewer in [CARS.md](https://github.com/arcanite24/pinyon-shift/blob/dev/docs/formats/CARS.md).

Colorado's scenery is drawn from the 14,291 render models `coloradoout.NNNNN.rmb.bin` in its `bin.zip`, not from the `.bix` members, which are textures. A model is a list of mesh objects, each a vertex buffer and one index list per material, and the game uploads both to the GPU unchanged: in a free-roam frame dump, every draw made with the main view's world matrix read bytes equal to a model's. Most models (11,898: terrain, roads, rivers, buildings) hold world positions. The rest (2,393: props, fences, signs, trees) sit around the origin and are drawn once per placement. Those placements are stored as instance matrices in the kind-7 `.pgeo` tiles and as positions with half-float rotations in the `.pvsz` zone files. Both name object IDs, and the object table in the ribbon's `Colorado_00.pvs` gives each object's render model: for all 130 placements of the dumped frame it names the drawn model, another level of its LOD set, or a model with a byte-identical mesh. Not every placement is shown: a `.pvsz` record or a switchable `.pgeo` tile can carry a list of track route numbers, and free roam (route 0) shows only placements without a list or with 0 in it. The rest is race set dressing (gantries, grandstands, marquees, concrete barriers); on the reference frame this rule separates the 70 drawn from the 136 undrawn spots in the camera's zone within 470 m with no exception, once the placeholder arrow model that marks each activity's node is left out too. `tools/re/world-geometry.py` extracts the archive past its wrapped entry count, exports a sector to OBJ with its props placed and ties models to a frame dump; the layout is in [WORLD_GEOMETRY.md](https://github.com/arcanite24/pinyon-shift/blob/dev/docs/formats/WORLD_GEOMETRY.md).

The rest of each vertex (normal, texture coordinates, tangent, colour) is the vertex declaration of the model's effect packed in order, which matches the stride of every submesh on the disc and the vertex fetches of every checked draw; texture coordinates are 16-bit fractions scaled by one constant per submesh, stored in the model's pre-built register writes. A material names its effect and, per texture slot, an entry of the model's texture list in `Colorado_00.pvs`, whose record carries the texture's ID, which is its file name: `_0x<ID>.bix` (a header and the mip chain) with `_0x<ID>_B.bix` (the base level, streamed separately), or a CAFF `_0x<ID>.bin`. The texels are DXT1, DXT3, DXT5 or DXN in the console's tiled layout, the car textures' rules with a trimmed last region; every `.bix` pair and all but 3 of the 2,965 CAFF textures fit it to the byte, and the textures a frame binds are byte for byte the files. `tools/re/world-materials.py` checks all of it and decodes textures to PNG. In a daytime free-roam frame no scenery draw uses the alpha test: trees and props use alpha to coverage, road decals and the lake blend by alpha, and the lightmaps read as light added at night (mostly black, all zero where the frame bound them), not a multiply. The layout and a first-pass shading recipe are in [WORLD_MATERIALS.md](https://github.com/arcanite24/pinyon-shift/blob/dev/docs/formats/WORLD_MATERIALS.md).

{{findings:files/Textures and models}}

## Game database

`gamedb.slt` is an ordinary SQLite 3 file, so any SQLite tool reads it (open it read-only). What its tables mean for cars and events is on [Cars, events and DLC](game-content.md).

```sh
sqlite3 "file:media/db/gamedb.slt?mode=ro" "select name from sqlite_master where type='table' order by name"
```

{{findings:files/Game database}}

## UI scene files

UI scenes are described with the rest of the interface on [UI, HUD and input](game-ui.md#scene-files).

## Other formats

{{findings:files/Other formats}}
