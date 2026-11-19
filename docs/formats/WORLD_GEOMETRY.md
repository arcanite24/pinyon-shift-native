# World geometry: Colorado render models

Status: RI-0.4 probe and RF-7 placement search, specification role,
2026-11-19. Our own reading of
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
    placement matrix. The placements are in two member types (see
    [Placements of local-space models](#placements-of-local-space-models)):
    kind-7 `.pgeo` tiles hold f32 instance matrices (23 of the 130
    placements of one frame, exact), and `.pvsz` zone files hold an f32
    position plus a half-float rotation (the other 107, within 1 cm).
    Which model a `.pvsz` placement names is not decoded yet. Collidable
    props are also listed, with the same position and orientation and a
    physics type name, in `Ribbon_00/CollObjs.xml` (59 of the 107).
- Drawn at the dumped camera from these files, the 73 world-space models
  of one frame match the game's own main-view depth to a median 0.024 %
  of the view depth (95th percentile 0.10 %), sampled at the game's pixel
  centres; see [Depth check](#depth-check-ri-04-gate). The game's depth is
  reversed 24-bit float, and its pixel centres sit half a pixel off an
  ordinary rasteriser's ([Coordinate frames](#coordinate-frames-and-draws)).
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
member; for the model IDs the placement files use it is not (see
[Ruled out](#ruled-out)).

| Members | Count (names) | What they are | Confidence | How |
| --- | --- | --- | --- | --- |
| `coloradoout.NNNNN.rmb.bin` | 14,291 | render models (this page) | confirmed | layout parse of every member, frame-dump match |
| `_0xXXXXXXXX.bix`, `_0xXXXXXXXX_B.bix` | 5,234 each | textures: `BIX1` header with width, height, level count and a GPU format word, then block-compressed texels; the `_B` twin has no header and a power-of-two size | probable | header fields against payload sizes; texel blocks recognised |
| `_0xXXXXXXXX.bin` | 2,966 | texture containers: a `CAFF` header with a version string, a `.data` and a `.gpu` section, the type string `texture` and a source path under a `textures` folder | probable | headers and strings |
| `_0xXXXXXXXX.bundle` | 146 | block-compressed texel payloads behind a small header (texture arrays, perhaps lightmaps; guess) | guess | payload look |
| `__R00GNNNNN.pgeo` | 7,970 | "procedural geometry" tiles (`OEGP` magic) of seven kinds by a header word (2-8); kind 7 (2,031 tiles) places models: 12,044 model records and 174,803 instance matrices. No vertex data | confirmed (kind 7 layout); probable (kind word); guess (other kinds) | full parse of every kind-7 tile to its stated size; exact match to drawn placements; runtime snapshot; class names in the executable |
| `*__shdata__<x>_<z>.sh` | 1,421 | lighting data per X/Z cell, by its name (spherical harmonics is a guess from `shdata`) | guess | names |
| `N.fiz` | 3,201 | a `fiz ` header with an X/Z box and two spacings; not in any vertex buffer | guess | header floats |
| `__R00ZNNNNN.pvsz` | 1,434 | zone files: a list of 32-bit IDs with the top bit as a flag, then prop placements (f32 position, half-float rotation) among other data | probable (placements); guess (framing) | 107 drawn placements found, position and rotation |
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
- **Instances.** A local-space model's draw carries its own c128-c131,
  the scene matrix times the instance placement. Solve for the placement
  from the clip x, y and w rows only: the z row of instanced draws differs
  from the scene matrix's (a different depth mapping; why is not
  established), and the full 4×4 inverse puts every placement 0.2-0.5 m
  off (median 0.25 m over 130). From the three rows the placement is
  rigid (unit rotation rows) and equals the stored one to file precision.
  For example one chain-link fence model has 22 distinct placements
  about 2.66 m apart along a road (its name says `2_66m`). 130 distinct placements
  were recovered from one frame (`placement_xyw` in the reader).
- **Screen.** The scene projection has negative x and y scales and the
  viewport is the ordinary one (scale 640/−360 at 1280×720), so the main
  view is drawn rotated by a half turn and turned upright later in the
  frame. In the presented image, x falls as clip x grows and y grows with
  clip y (measured: only this mapping puts the flag, crowd barriers and
  ridge lines over the capture).
- **Depth.** The main view's depth is reversed: the stored value is
  1 − z/w (viewport z scale −1, offset 1), the depth test is
  greater-or-equal, and the target is cleared to 0. It is the 24-bit
  float depth format (4-bit exponent, 20-bit mantissa, values 0 to 1).
  With the scene matrix's z row equal to 1.000015 times its w row minus
  0.29995 m, z/w is 0 at 0.3 m and 1 at about 19,957 m; the w row is the
  view depth in metres. A world-space depth prepass at 1× fills a
  1280×720 target first; the colour pass then draws at 4× MSAA in three
  bands of 256, 256 and 208 rows (predicated tiling: the same command
  stream runs once per band, and a packet walk that ignores the bin mask
  and select sees 3,974 draws where the GPU runs 3,730). Confirmed by
  reading both depth targets back from a replay of the dump (Evidence).
- **Pixel centres.** The game rasterises with Direct3D 9 pixel centres
  (on integer window coordinates, not half-integers). After the half
  turn, the game's sample for presented pixel (x, y) lies half a pixel
  right of and below the centre an ordinary (Vulkan or Direct3D 10+)
  rasteriser uses. An engine that wants the game's pixels shifts its
  projection by half a pixel; without it, near edges move by half a
  pixel and depth at grazing angles differs by about 0.2 % (Evidence).
  Confirmed: the pixel-centre setting on every scene draw, and the depth
  comparison is best at exactly that shift.
- **Other views.** The same models are also drawn into other views:
  reflection or cube-map faces (`TERR_CUBE_*` models, matrices with 0/±1
  rows) and shadow or other passes. 98 world-space and 328 local-space
  tied draws were in such views.

## Placements of local-space models

Established 2026-11-19 (RF-7, specification role) by three methods that
agree: the 130 placements of the dumped frame searched for in the
members, a physical-memory snapshot of the running game in free roam at
the same spot, and the executable's class names with the factory that
picks a class per `.pgeo` kind. Both files are big-endian and use the
game frame (the file frame with z negated).

| Store | Placements of the frame | Encoding | Confidence |
| --- | --- | --- | --- |
| kind-7 `.pgeo` tile | 23 of 130 (22 chain-link fence in one tile, 1 lodgepole pine), translation within 1 cm and rotation within 0.001 | f32 axis rows and translation | confirmed |
| `.pvsz` zone file | the other 107 (benches, information points, flags, signs, barriers), position within 1 cm, rotation within half-float precision | f32 position, 9 half floats | probable |

No placement was found in both, and every one was found in one of them.
`Ribbon_00/CollObjs.xml`, the plain-XML list of 21,877 collidable
objects, also holds 59 of the 107 `.pvsz` placements (game frame,
position within 1 cm, orientation within 0.002): fences, picnic tables,
signs, outhouses, information points. None of the 23 `.pgeo` placements
(chain-link fence, pine) is in it. Its `PhysicsType` names a collision
model whose name resembles the render model's, not always the same
(`CO_Table_Picnic` for `tablebench_LOD_00`), so it is a lead for the
`.pvsz` model link, not the link itself.

### `.pgeo` tiles

A tile starts with the magic `OEGP`, a box (minimum at +0x10, maximum at
+0x20, game frame) and a kind word at +0x30. Counts by kind over the 7,970
tiles: 2: 3,199; 3: 1,783; 4: 33; 5: 202; 6: 278; 7: 2,031; 8: 444. The
executable's resource class for these files is the track "procedural
geometry" resource, and its factory maps the kind word to one class each;
kind 7 is the procedural-models class (the other classes include
vegetation, billboards, points and light maps; which kind is which past
kind 7 is not checked). Only kind 7 is decoded here.

Kind-7 header fields (offsets from the start):

| Offset | Meaning | Confidence |
| --- | --- | --- |
| +0x38 | a distance (2,500 in the tiles read) | guess (draw distance) |
| +0x3C | name length; the name follows the header | probable |
| +0x40 | number of 8-byte references (u32 value, u32 flag) | probable |
| +0x44 | number of 8-byte string records | probable |
| +0x48 | total size | confirmed (equals the file size) |
| +0x50 | size of the trailing block of 40-byte packed records | confirmed (the parse ends on it) |
| +0x54 | number of model records | confirmed |

Then, 16-byte aligned, 0x40-byte **model records**: a box (minimum, pad,
maximum, pad), a string length, three (count, slot) pairs giving up to
three lists of u32 model IDs (presumably LODs), and a word; the ID lists
follow, 4-byte aligned. The IDs are in one space shared with the `.pvsz`
ID lists; see open question 1.

Then the **class part**: a small header whose group count is at +0xC,
and 0x5C-byte **group records** from +0x14. A group names a reference (the
key at +0x00), a model record (+0x2C), its instance count (+0x30), counts
of 40-byte and 32-byte records (+0x34, +0x38) and four distances (+0x3C,
presumably LOD switch distances). Each group's instances follow,
16-byte aligned, then its 32-byte records.

A 96-byte **instance** is three rows of three f32 (each padded to 16
bytes), each row the image of one local axis in the game frame with the
scale included; the translation (three f32 and 1.0); then colour-like
floats and a normal-like vector with a u32 (not decoded). In the file
frame, a model vertex v is placed at F·Rᵀ·F·v + F·t, where R has the
three rows, t is the translation and F negates z. The runtime copy seen
in the snapshot keeps each instance's rotation, its translation relative
to the tile's box minimum, and a world box.

Every kind-7 tile parses to its stated size. Linking a group's model
record to a render model by header box is weak (122 of 2,460 instances in
the sector below link to exactly one `.rmb.bin`); the ID lists are the
real link and are not resolved.

### `.pvsz` zone files

A zone file begins with a count and that many u32 IDs (top bit a flag),
then further sections. The 107 placements sit at unaligned offsets: three
f32 (position, game frame), then nine half floats that hold the
game-frame rotation transposed (H = F·R_gameᵀ), then padding. Three half
floats (100, 250 and 500 in the samples, perhaps distances) and flag
bytes come just before the position. How the records are framed, how
many there are and which model each names are not decoded, so `dump-obj`
cannot place these yet; `match-dump --pgeo` only checks them.

### Ruled out

- The earlier negative (no float triple within 0.05 m anywhere,
  `CollObjs.xml` included) was wrong: it searched for the biased
  placements of the full inverse.
- Model IDs are not `FilenameMap_00.dat` indices, not `bin.zip`
  directory indices and not PVS record numbers offset by the name count
  (tested on the fence and pine IDs).
- The roughly 14,560 oriented boxes with ID lists in `Colorado_00.pvs`
  (from about 0x199000) are clusters, not instances.
- The 40-byte `.pgeo` records are packed bitfields, not positions.
- The file-open log of the free-roam route showed only `colorado_00.pvs`
  and `bin.zip` opened for the track during the logged span (the
  `Ribbon_00` XML is presumably read earlier or through another path; not
  checked).

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
| draws / indexed draws (packet walk, all bands) | 3,974 / 2,855 (3,730 run on the GPU; the rest are predicated off for the band) | |
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

### Depth check (RI-0.4 gate)

Measured 2026-11-19. The dump holds no depth image, but replaying it
offline (`tools/replay-fh1-frame.py`, Vulkan, same build) re-runs the
frame's commands, camera included, and the replay can write every
resolve's output (`--fh1_resolve_dump_dir`). The game resolves both depth
targets in the frame: the 1× prepass once, and the 4× main pass once per
band (the first sample of each pixel). The draw log
(`--fh1_debug_log_draws`) tells which resolve is which.

To compare only what the engine draws, a second replay skips every draw
that writes those depth targets other than the 820 world-space draws with
the scene matrix (`depth-skips` lists them: 1,959 draws, the car, props,
people and trees among them; the bands' full-screen depth clears stay).
`depth-compare` then decodes the main pass's three bands (untiled, turned
back by the half turn, float depth, 1 − stored = z/w), converts both
images to view depth with the scene matrix's z and w rows, and compares
pixel by pixel. The engine image is shift-engine `d55ed64` drawing the 73
world-space models the dump tied to the main view (carview `--world`, one
sample, z/w). The car, minimap, speedometer and event-banner boxes of the
engine's frame file are left out.

| Pixels compared | Median | 95th | Median, relative | 95th, relative |
| --- | --- | --- | --- | --- |
| all, game sample point (half-pixel shift) | 0.006 m | 1.05 m | 0.024 % | 0.10 % |
| view depth under 40 m | 0.001 m | 0.008 m | 0.022 % | 0.064 % |
| 40 to 200 m | 0.010 m | 0.065 m | 0.012 % | 0.069 % |
| 200 to 1,000 m | 0.20 m | 0.75 m | 0.025 % | 0.092 % |
| over 1,000 m | 0.42 m | 7.8 m | 0.033 % | 0.15 % |
| all, our own pixel centres (no shift) | 0.15 m | 3.5 m | 0.16 % | 0.96 % |

569,343 pixels compared at the shifted sample point; 39,012 more (6 %) are
left out because the four engine pixels around the sample straddle a
depth step of more than 2 %. Coverage agrees: 82,829 pixels are empty
(sky) in the game's image and 83,294 in ours, and only 467 are covered in
one image but not the other. The far residual grows with distance as a
fixed fraction (the 4× pass stores its first sample, which is not at the
pixel centre). Against the 1× prepass, which holds only the nearer world
(348,148 pixels), the median is 0.001 m (0.011 %) and the 95th under
40 m 0.0075 m.

What this rests on: the replayed depth is the recompilation's (Vulkan,
float depth converted to the 24-bit format at the resolve), not a console
readback; the model set came from the dump's draw list, not from a
selection rule (open question 3); and the comparison covers world-space
models only (props need the model IDs, open question 1).

## Open questions and next steps

1. **Model IDs and `.pvsz` framing (blocker for complete sectors).**
   Where the placements live is answered above. Still open: the mapping
   from the u32 model IDs in `.pgeo` model records and `.pvsz` lists to
   `.rmb.bin` members, and the record framing of the `.pvsz` placements
   (the `CollObjs.xml` physics names are a partial lead).
   Next: follow an ID in the snapshot to the loaded render model
   (`pinyon_shift_snapshot_virtual` writes the guest's virtual memory),
   or move one `.pvsz` position in a private media copy and see which
   draw moves (RF-7 b). `CollObjs.xml`'s `GraphicsName="#N"` is the
   object's own index, not a render-model number.
2. **Vertex attributes** after the position: read the bound vertex
   shaders' fetch instructions in the dump, per stride.
3. **Which LOD is drawn.** Models come as `_LOD00`/`_LOD01`/`_LOD02`,
   `_NoLOD`, `_MIDDIST`, `UberLOD`, `_CUBE`; the choice is the PVS and
   streaming work.
4. **Depth check** for the RI-0.4 gate. The engine now draws the sector
   at the dumped camera (shift-engine `d55ed64`): near features land
   within a median 1 px of the capture, but far terrain and the skyline
   were off: the skyline by a mean 8 px, ridges by 40 to 50 px. Drawing exactly the 73 world-space models the dump tied to the main view, with the matrix at full precision, puts the skyline a median 3-4 px (mean 6 px) from the capture and the near silhouettes 1 px. Only 39 of those 73 lie in the 1 km box, and limiting the draw to them puts the skyline back to a mean 8 px. The ridges come from models outside the sector, and the sector's other 238 models are not drawn by the game. Given the game's draw list, the format and matrix reproduce the frame; the selection rule is question 3. Depth
   is now measured against the game's own depth targets (see
   [Depth check](#depth-check-ri-04-gate)): at the game's sample point the
   view depth agrees to a median 0.024 % and a 95th percentile of 0.10 %.
   `match-dump` printed the scene
   matrix to 4 decimals, which moves near geometry by several pixels; it
   now prints full precision.
5. Header constants (4, 4, 0, 1; vertex format 3; the fourth submesh word
   4) and the per-list trailer: mutation runs (RF-7 b) would show whether
   they matter.

## Questions from the implementation side

Asked on 2026-11-19 by the first engine implementation. Where the answer
says "engine's choice", the game's rule is not known.

| Question | Answer |
| --- | --- |
| How is a model known to be world-space or local-space? | No stored flag is known. The rule used here: local when the header box lies within 200 m of the origin in X and Z, which gives 11,898 world and 2,393 local; it is probable. A model that a `.pgeo` or `.pvsz` placement names is local, once the model IDs are resolved |
| Which LOD does the game draw? | Unknown (open question 3). Drawing every overlapping model puts far LODs over near ones; for comparisons, draw the nearest LOD and leave out `UberLOD` and `_CUBE` models, as an engine choice |
| Do name lengths include a terminating NUL? | Strip trailing NULs |
| The restart value for a 4-byte strip | All ones, by analogy; no 4-byte strip is on the disc (the one 4-byte list is a triangle list) |
| Does "overlaps" include boxes that only touch the rectangle? | Yes, inclusive. That gives the 277 models of the sector |
| A constant field off its usual value | Not on the disc. Engine's choice: reject the model and log it |

## Reproduce

```text
python -I tools/re/world-geometry.py extract \
    .local/game/base/media/tracks/colorado/bin.zip .local/re/world/all
python -I tools/re/world-geometry.py stats .local/re/world/all
python -I tools/re/world-geometry.py dump-obj .local/re/world/all --box 1500 -1460 2500 -460
python -I tools/re/world-geometry.py dump-obj .local/re/world/all --box 1500 -1460 2500 -460 --props
python -I tools/re/world-geometry.py pgeo .local/re/world/all
python -I tools/re/world-geometry.py match-dump .local/re/world/all <dump.fh1frame> [--verbose]
python -I tools/re/world-geometry.py match-dump .local/re/world/all <dump.fh1frame> \
    --members @<names file> --pgeo .local/re/world/all
python -I tools/re/world-geometry.py overlay .local/re/world/all <dump.fh1frame> <capture.ppm>
python tools/replay-fh1-frame.py <dump.fh1frame> --state-root <seed> --hidden \
    --game-argument=--gpu_backend=vulkan --game-argument=--fh1_debug_log_draws=true \
    --game-argument=--fh1_resolve_dump_dir=<dir>
python -I tools/re/world-geometry.py depth-skips <drawn members> <dump.fh1frame> <runtime.log>
    (replay again with --fh1_debug_skip_draws=<the printed ranges>)
python -I tools/re/world-geometry.py depth-compare <world-depth.pfm> <frame file> \
    <band 1> <band 2> <band 3> --shift 0.5 0.5
```

`--members @file` reads the comma-separated names from a file (the
selection of 58 local-space and 277 world-space models used for the
130-placement check is too long for a Windows command line). The memory
snapshot route is `.local/re/world/world-snapshot.fh1test` (local): the
world-dump inputs, then `snapshot 2340 freeroam`, run with
`--pinyon_shift_snapshot_virtual=true` to add the virtual-memory file.
