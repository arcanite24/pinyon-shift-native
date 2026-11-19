# World geometry: Colorado render models

Status: RI-0.4 probe, specification role, 2026-11-19. Our own reading of
the player's files and of one frame dump; no disassembly, decompiled
code or game data is reproduced here ([LEGAL.md](../LEGAL.md)). The
reader is [`tools/re/world-geometry.py`](../../tools/re/world-geometry.py).

## Summary

- The scenery the game draws comes from `coloradoout.NNNNN.rmb.bin`, the
  14,291 render models in `media/tracks/colorado/bin.zip`. Every one of
  them parses to its last index list with the layout below.
- The vertex and index bytes in a model are the GPU's: the game uploads
  them unchanged. In a free-roam frame, 1,761 of the 2,855 indexed draws
  read a vertex buffer and an index buffer whose bytes, size, index count
  and primitive type are exactly a model's. Every draw that used the main
  view's world matrix (820) is one of them.
- Two kinds of model:
  - **world-space** models (terrain, roads, rivers, buildings; 11,898)
    hold world positions and are drawn with the view matrix alone. A
    sector can be built from the files by selecting models by their
    header box.
  - **local-space** models (props, signs, fences, trees, barriers; 2,393)
    sit around the origin and are drawn once per instance with a
    placement matrix. **Where those placements are stored is not yet
    found**: it is the blocker for complete sectors.
- `.bix`, the `_0x*.bin` members and `.bundle` hold textures; `.pgeo`,
  `.fiz`, `.sh` and `.pvsz` bytes appear in no vertex buffer of the frame.
  The census record calling `.bix` "scenery models" and `.bin` "track
  geometry" needs correcting: the geometry is in `.rmb.bin` only.

## Archive and member names

`bin.zip` has 230,057 directory entries for 43,546 names, compared
ignoring case (the copies of a name differ in letter case, 57,299
spellings in all). The same name recurs up to 33 times, always with the
same CRC-32 and size (checked over the whole directory), so any copy will
do. The end record's 16-bit entry count holds 33,449 (230,057 modulo
65,536), and readers that trust it stop there: the first 33,449 entries
are 33,449 distinct names, so `tools/fh1_archive_extract.cpp
--archive-all` misses the other 10,097, among them 4,337 render models.
Python's `zipfile` walks the directory by its size and sees every entry.

`Ribbon_00/FilenameMap_00.dat` lists the same 43,546 names: a big-endian
table of 43,546 u32 offsets, then the NUL-terminated names those offsets
point at (confirmed by reading it in full; the 173 shader objects appear
in the map without the `shaders/track/` folder the archive stores them
under). The list index is presumably the ID other files use to name a
member (not checked).

| Members | Count (names) | What they are | Confidence | How |
| --- | --- | --- | --- | --- |
| `coloradoout.NNNNN.rmb.bin` | 14,291 | render models (this page) | confirmed | layout parse of every member, frame-dump match |
| `_0xXXXXXXXX.bix`, `_0xXXXXXXXX_B.bix` | 5,234 each | textures: `BIX1` header with width, height, level count and a GPU format word, then block-compressed texels; the `_B` twin has no header and a power-of-two size | probable | header fields against payload sizes; texel blocks recognised |
| `_0xXXXXXXXX.bin` | 2,966 | texture containers: a `CAFF` header with a version string, a `.data` and a `.gpu` section, the type string `texture` and a source path under a `textures` folder | probable | headers and strings |
| `_0xXXXXXXXX.bundle` | 146 | block-compressed texel payloads behind a small header (texture arrays, perhaps lightmaps; guess) | guess | payload look |
| `__R00GNNNNN.pgeo` | 7,970 | a 128 m X/Z tile each (header box), a `LightMap_X_Y` name and a 16-bit quadtree; no vertex data | probable (tile, name); guess (use) | header floats, strings; not in any vertex buffer |
| `*__shdata__<x>_<z>.sh` | 1,421 | lighting data per X/Z cell, by its name (spherical harmonics is a guess from `shdata`) | guess | names |
| `N.fiz` | 3,201 | a `fiz ` header with an X/Z box and two spacings; not in any vertex buffer | guess | header floats |
| `__R00ZNNNNN.pvsz` | 1,434 | visibility zone lists of 32-bit entries with the top bit as a flag | guess | look |
| `colorado_00_NNNNN.soundscape` | 1,450 | sound | — | name |
| `shaders/track/*.fxobj` | 173 | compiled track shaders, by name (the shader paths the render models list) | probable | names |
| `.fev`, `.fsb` | 13 each | audio event and sample banks (FMOD extensions) | probable | names |

`Ribbon_00/Colorado_00.pvs` starts with `FPVS`; one 16-bit header field is
7,970, the number of `.pgeo` tiles (a lead for the PVS work, not
followed).

## Render model layout

All values are big-endian. Records are packed with no alignment: after
the first variable-length name nothing is 4-byte aligned.

### Header

| Offset | Size | Field | Confidence | How |
| --- | --- | --- | --- | --- |
| 0 | u32 | kind, 6 in every member | confirmed (constant) | all 14,291 members |
| 4 | 3 × f32 + u32 pad | box minimum (x, y, z) in the model's space | confirmed | every vertex lies inside it |
| 20 | 3 × f32 + u32 pad | box maximum | confirmed | same |
| 36 | 16 × f32 | a matrix, identity in every member | confirmed (constant) | all members |
| 100 | 4 × u32 | 4, 4, 0, 1 in every member | constant, meaning unknown | all members |
| 116 | u32 | number of mesh objects (1 to 55) | confirmed | parsing that many objects ends exactly at the material section in every member |
| 120 | | the mesh objects | | |

After the objects come the material and shader records (see below).

### Mesh object

One object per vertex format: a tree is one object for its leaf cards and
one for its trunk, a terrain patch one per layer set. All objects of a
model share its space.

| Field | Size | Meaning | Confidence | How |
| --- | --- | --- | --- | --- |
| tag | u32 | 1 | constant | all members |
| centre | 3 × f32 + pad | object box centre | probable | consistent with its vertices |
| minus half extent | 3 × f32 + pad | | probable | equals −(half extent) |
| half extent | 3 × f32 + pad | | probable | |
| count | u32 | 1 | constant | |
| name | u32 length + bytes | node name (`Redstone_Area04_Road_LOD01_23`, `Tree_Elm01_LOD01`) | confirmed | strings |
| vertex format | u32 | 3 in every member | constant, meaning unknown | |
| vertex count | u32 | | confirmed | indices never exceed it; the draw's fetch size is count × stride |
| stride | u32 | 16, 20, 24, 28, 32 or 36 bytes | confirmed | fetch size and dump layout |
| zero | u32 | 0 | constant | |
| vertices | count × stride | the GPU vertex buffer, byte for byte | confirmed | every byte the dump recorded at the address the draw's vertex fetch constant names equals the file's (the fetch size equals the block's) |
| one | u32 | 1 | constant | |
| submesh count | u32 | | confirmed | parse |
| submeshes | | see below | | |

**Vertex contents.** Bytes 0-11 are the position, three f32 (confirmed:
the positions, projected with each draw's matrix, land on the drawn
surfaces in the capture; see Evidence). The other 4 to 24 bytes (normal,
tangent frame, texture coordinates, colour; by stride) are **not
decoded**. They are not the car format: carbin positions are four 16-bit
integers (direction and length), scenery positions are plain floats. The
next step is to read the vertex fetch instructions of the vertex shader
each tied draw binds (the dump holds the shaders), which gives every
element's offset and format per stride.

### Submesh (one index list, one material)

| Field | Size | Meaning | Confidence | How |
| --- | --- | --- | --- | --- |
| tag | 2 × u32 | 1, 2 | constant | |
| material name | u32 length + bytes | `Material__25`, `sand_to_grassy_sand`, `cards` | confirmed | strings |
| header | 4 × u32 | 0; **primitive type**; **material index**; 1 | confirmed (primitive), probable (material index: counts 0..n-1 over a model's submeshes) | the draw's primitive type equals the second word in all tied draws: 6 triangle strip, 4 triangle list |
| reserved | 16 bytes | zero | constant | |
| colour | 4 × f32 | 1, 1, 1, 1 in every member seen | constant | |
| sphere | 4 × f32 | centre and radius | probable | radius covers the list's vertices |
| fourth word | u32 | 4 in every member | constant, meaning unknown (not the primitive type) | |
| zero | u32 | 0 | constant | |
| index count | u32 | | confirmed | equals the draw's index count |
| index size | u32 | 2, or 4 for one object with 90,388 vertices | confirmed (2), probable (4) | parse; the 32-bit list is a triangle list |
| indices | count × size | the GPU index buffer, byte for byte | confirmed | found at the draw's index address |
| trailer | u32 | 5 (45,591), 13 (1,431), 6 (998), 2 (32), 0 (1) | meaning unknown | census |

Strips restart at the 16-bit index 0xFFFF (confirmed: the draw reads
the same bytes and the triangles cover the drawn surfaces). A 32-bit
strip would presumably restart at all ones; the only 32-bit list is a
triangle list, so that case is a guess and does not arise.

### After the objects

Material records (a count, then per material a block of floats that
looks like shader parameters), a list of shader paths
(`shaders\track\h_road_diff3_norm1_noise_ao_lm.fx` and similar), then
what looks like compiled shader microcode. 71.1 % of the render-model
bytes are geometry (up to the last index list); the remaining 28.9 % are
these records, not decoded. They matter for materials, not for
positions.

## Coordinate frames and draws

- **File frame.** World-space positions are drawn with one matrix (vertex
  shader constants c128-c131, the "scene matrix") that equals the main
  view's projection times view times a negation of z. So the game's world
  is the file frame with z negated: the camera of the dumped frame is at
  (1999.0, 145.4, −962.9) in the file frame, and the game's own camera
  constants give +962.9. Confirmed by the overlay below and by the scene
  matrix differing from the camera-relative view matrix only in the sign
  of its z column.
- **Instances.** A local-space model's draw carries its own c128-c131. The
  scene matrix's inverse times that matrix is a rigid transform (unit
  rotation rows, last row 0 0 0 1) for draws in the main view, which gives
  the instance placement in the file frame. For example a chain-link fence
  model is drawn 14 times about 2.66 m apart along a road (its name says
  `2_66m`). 130 placements were recovered this way from one frame.
- **Screen.** The scene projection has negative x and y scales and the
  viewport is the ordinary one (scale 640/−360 at 1280×720), so the main
  view is drawn rotated by a half turn and turned upright later in the
  frame. In the presented image, x falls as clip x grows and y grows with
  clip y (measured: only this mapping puts the flag, crowd barriers and
  ridge lines over the capture).
- **Other views.** The same models are also drawn into other views:
  reflection or cube-map faces (`TERR_CUBE_*` models, matrices with 0/±1
  rows) and shadow or other passes. 98 world-space and 328 local-space
  tied draws were in such views.

## Evidence (frame-dump correlation)

Run: route `.local/re/world/world-dump.fh1test` (a copy of
`config/render-tests/fh1-macos-fixed-frame.fh1test`), base build
`out/build/win-amd64-release`, Vulkan, `gpu_record_thread=false`, 1x, seed
`appdata-2026-09-27`, base game root. One frame after the parked capture
was dumped (93 MB). Free roam on the road near the Gauntlet sign.

`world-geometry.py match-dump` replays the dump's register writes
(type-0, type-1 and constant packets, including constants loaded from
memory) to know each draw's vertex fetch constants, index address, count,
primitive type and c128-c131, then ties a draw to a mesh object and
submesh when the fetch buffer has exactly the object's size and every
recorded byte equals the file's, and the index count, primitive type and
every recorded index byte equal the list's (a dump keeps only the pages
the frame read, so long buffers can be partial).

| Measure | Whole frame | Sector (below) |
| --- | --- | --- |
| draws / indexed draws | 3,974 / 2,855 | |
| draws tied to render models | 1,761 (135 models, 189 mesh objects) | 532 (43 models) |
| world-space draws with the scene matrix | 820 (every draw using that matrix) | 434 |
| world-space draws in other views | 98 | 98 |
| local-space draws in the main view (rigid placement) | 515 | — |
| local-space draws in other views | 328 | — |

The 1,094 indexed draws not tied: 287 in the main view with a rigid
placement whose vertex bytes are in no track member (sampled: cars and
people, presumably from their own archives; not checked), 698 with other
matrices, 109 with no matrix in c128-c131 (screen passes). Of 25 sampled
untied scene-matrix draws before the partial-buffer comparison, all read
render-model bytes.

**The sector.** A 1 km square around the dumped camera, file frame
x 1500..2500, z −1460..−460: `dump-obj --box 1500 -1460 2500 -460` exports
277 world-space models (366,251 vertices) to an OBJ. The frame drew 39 of
them in the main view (the others are other LODs, other views or culled),
with 434 draws whose recorded vertex and index bytes are identical to
the exported members' (98 more draws of these models are in other views;
35 MB of vertex and 0.9 MB of index bytes compared over all 532).
`overlay` draws the edges of every tied main-view draw, from the file's
triangles and the draw's own matrix, over the capture: the road edges,
terrain ridges, flag, barriers and guard rail line up with the image
(`.local/re/world/png/overlay-frame.png`, local only).

What this does not show yet: a depth comparison. The dump records the
front buffer, not the depth buffer, so "depth within tolerance" needs a
dump of the depth target or a render of the exported sector compared
against a depth capture.

## Open questions and next steps

1. **Instance placements (blocker for complete sectors).** The fence
   placements (for example x 2006.24, y 141.51, z −937.20 in the file
   frame, and the same with z negated) do not occur as float32 triples
   within 0.05 m in any member of `bin.zip`, in `Ribbon_00` (including
   `CollObjs.xml`, `GameObjs.xml`, `Colorado_00.pvs`) or in the loose
   Colorado files. `CollObjs.xml`'s `GraphicsName="#N"` is the object's
   own index, not a render-model number. Candidates: `.pvsz`/`.pvs` with
   quantised or zone-relative positions, `.fiz` (foliage?), or another
   archive. Next: a read trace of the members loaded around a prop
   (RF-7 a), then a data-flow look in Ghidra at whatever writes the
   instance matrix that ends up in c128-c131 (specification role).
2. **Vertex attributes** after the position: read the bound vertex
   shaders' fetch instructions in the dump, per stride.
3. **Which LOD is drawn.** Models come as `_LOD00`/`_LOD01`/`_LOD02`,
   `_NoLOD`, `_MIDDIST`, `UberLOD`, `_CUBE`; the choice is the PVS and
   streaming work.
4. **Depth check** for the RI-0.4 gate: dump with the depth target, or
   draw the sector with our own renderer at the dumped camera.
5. Header constants (4, 4, 0, 1; vertex format 3; the fourth submesh word
   4) and the per-list trailer: mutation runs (RF-7 b) would show whether
   they matter.

## Reproduce

```text
python -I tools/re/world-geometry.py extract \
    .local/game/base/media/tracks/colorado/bin.zip .local/re/world/all
python -I tools/re/world-geometry.py stats .local/re/world/all
python -I tools/re/world-geometry.py dump-obj .local/re/world/all --box 1500 -1460 2500 -460
python -I tools/re/world-geometry.py match-dump .local/re/world/all <dump.fh1frame> [--verbose]
python -I tools/re/world-geometry.py overlay .local/re/world/all <dump.fh1frame> <capture.ppm>
```
