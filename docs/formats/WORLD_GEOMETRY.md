# World geometry: Colorado render models

Status: RI-0.4 probe, RF-7 placement search, RI-0.4 depth check and
object table, specification role, 2026-11-19. Our own reading of
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
    Both name an **object ID**, and the object table in
    `Ribbon_00/Colorado_00.pvs` gives each object's render model (see
    [Object table](#object-table)): for every one of the 130 placements
    it names the drawn model, a model of its LOD set, or a model with a
    byte-identical mesh. `dump-obj --props` places both stores from it.
    Collidable props are also listed, with the same position and
    orientation and a physics type name, in `Ribbon_00/CollObjs.xml` (59
    of the 107).
- Drawn at the dumped camera from these files, the 73 world-space models
  of one frame match the game's own main-view depth to a median 0.024 %
  of the view depth (95th percentile 0.10 %), sampled at the game's pixel
  centres; see [Depth check](#depth-check-ri-04-gate). With the frame's
  130 props added, the world pixels keep that tolerance but the prop
  pixels do not yet (median 1.4 m, mostly the engine's barrier
  orientation and props whose materials matter). The game's depth is
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
member; the placement files do not use it: they name objects, whose
model is a member number in the [object table](#object-table).

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
| `.pvsz` zone file | the other 107 (benches, information points, flags, signs, barriers), position within 1 cm, rotation within half-float precision | f32 position, 9 half floats | confirmed |

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
| +0x38 | a distance: 2,500 in 1,432 of the 2,031 kind-7 tiles, 500 in 418, 3,500 in 131, 750 in 20, 350 in 15 | guess (draw distance) |
| +0x3C | name length; the name starts at +0x60, or at +0x98 when the word at +0x58 is non-zero (1,562 and 469 kind-7 tiles; the 0x38 bytes between are not decoded) | confirmed (every tile parses to its size) |
| +0x40 | number of 8-byte references (u32 value, u32 flag) | probable |
| +0x44 | number of 8-byte string records | probable |
| +0x48 | total size | confirmed (equals the file size) |
| +0x50 | size of the trailing block of 40-byte packed records | confirmed (the parse ends on it) |
| +0x54 | number of model records | confirmed |

Then, 16-byte aligned, 0x40-byte **model records**: a box (minimum, pad,
maximum, pad), a string length, three (count, slot) pairs giving up to
three lists of u32 object IDs (one per LOD: list 0 names the LOD00
model, lists 1 and 2 the next ones, probable from the pine and a few
barriers), and a word; the ID lists follow, 4-byte aligned. The IDs
are in the object ID space the `.pvsz` files use, at its top, and the
[object table](#object-table) gives their models. Of the 12,044 model
records, 9,268 have one ID in lists 0 and 1, 1,902 one ID in list 0
only, 661 one ID in each of the three, and 213 no ID at all (no model to
place). In 11,913 records the IDs run consecutively from list 0 on; in
the other 131 they do not, so a reader must take each list's own IDs
rather than count on from list 0.

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
the sector below link to exactly one `.rmb.bin`, because many variants
share a box); the ID lists through the object table are the link.

### `.pvsz` zone files

A zone file has three parts:

1. a u32 count n and n u32 object IDs (the top bit is a flag, not
   decoded);
2. a block not decoded (visibility data, presumably), whose size is not
   stored anywhere found;
3. exactly n placement records running to the end of the file, record k
   for the k-th ID.

| Record field | Size | Meaning | Confidence |
| --- | --- | --- | --- |
| three half floats | 6 | 100, 250 and 500 in most samples; NaN, −1 and 0 also occur | guess (LOD or fade distances) |
| position | 12 | three f32, game frame | confirmed |
| rotation | 18 | nine half floats h0..h8; see the reading below | confirmed |
| unknown | 16 | zero in the samples | — |
| flag | 1 | 0: the record ends here (53 bytes) | confirmed |
| when the flag is 1 | 4 + 1 + c + 32 | a u32 (−1, or a small index shared by every placement of one model: perhaps a physics or smashable type), a byte count c, c bytes, a 32-byte name such as `FR06_13` | probable |

**Finding the records.** The block's size is not known, and in most
zones several start offsets give n records that end exactly at the end
of the file (the record fields are loose enough to chain from a wrong
start). The rule that works: among those starts, take the one whose
records hold the most rigid rotations (the three rows of halves
orthonormal within 0.01). Over the 1,434 zones: 1,209 have more than
one start; in 56 the first start is not the most rigid one, and taking
the first leaves 502 objects whose record differs from the same
object's record in another zone (the implementation, with its own
field checks, counted 1,214 zones and 1,570 such records); with the
most-rigid rule none differ, and no zone has two starts that are both
fully rigid. An object listed by several
zones then has the same record in each (59,599 distinct objects between
0 and 62,172), so the zones are views of one object list, and the model
is not in the record: it comes from the [object table](#object-table).
Every one of the 107 matched placements is the position field of a
record.

**Reading the rotation.** With h0..h8 the nine halves in file order and
F = diag(1, 1, −1), the file-frame rotation is R[r][c] = h[3c + r]·F[r]:
the halves are the rotation's columns, one after another, with the
third component of each negated. A model vertex v (file frame) is
placed at R·v + (x, y, −z), where (x, y, z) is the record's position.
Checked against all 107 placements the dump drew: largest element error
0.0005 for the 91 upright props and 0.0005 for the 16 tilted ones
(within half-float precision). The other reading, R[r][c] = h[3r + c]·F[r]
(rows instead of columns), is off by up to 0.049 for the upright props
and 0.16 for the tilted ones; reading the halves as F·R·F gives errors
up to 2 (mirrored placements).

**One object per LOD at a spot.** A zone often lists several objects at
one position with one rotation, one per LOD level of a prop. In the
sector, the 847 objects sit at 572 spots; at each of the 236 spots with
more than one object the rotations are equal and the table names a
different member for each, of one LOD set at 231 of them. An engine that
resolves each to the same level draws the model several times in
place; keep one placement per model and spot. Which of them the game
draws is the selection question (open question 3).

### Object table

`Ribbon_00/Colorado_00.pvs` is big-endian. Its header:

| Offset | Meaning | Confidence |
| --- | --- | --- |
| +0x00 | magic `FPVS` | confirmed |
| +0x04 | version, 50 | probable |
| +0x08 | 47,014,282 | unknown |
| +0x0C, +0x10 | zero | — |
| +0x14 | bytes 1F 22 01 01 | unknown |
| +0x18, +0x1C | 50,120 and 3,852 | unknown |
| +0x20 | 33, the length of the next list | probable |
| +0x24 | 33 u32, rising from 43,546 to 229,835: the first `bin.zip` directory entry of each of the 33 repeated runs of members (see [Archive and member names](#archive-and-member-names)) | probable (the entries at those indices start a repeat, checked by name) |
| +0xA8 | 12,800 | unknown |
| +0xAC | three zero bytes | — |
| +0xAF (175) | u32 count of the 28-byte records, 19,613; the records follow at +0xB3, unaligned | confirmed (the records end exactly on the shader-name count) |

After the header come the 28-byte record table, a list of the 173 track
shader names (a u32 count, then
u32-length-prefixed strings), and right after the names the **object
table**: a u32 count, 62,173 (the size of the object ID space), then
one 18-byte record per object ID. The first big-endian u16 of a record
is the member number of the object's render model, `coloradoout.NNNNN`.
The other fields (a signed u16, two flag-like u16s, a u32 that is 0 or
an index, a u32 that is −1 or a small count, a u16 that is usually all
ones) are not decoded. The file goes on with the clustered boxes
(see [Ruled out](#ruled-out)).

For an object with LODs, the table names one member of the LOD set, not
always LOD00. The set is the run of consecutive members whose first mesh
names differ only in a `_LODnn` part that counts up by one (for example
`OBJ_FEST_BarrierMetal_LOD00_`, `_LOD01_`, `_LOD02_` as three
consecutive members; probable, it holds for every set the dump draws).
A model whose name has no `_LODnn` is a set of one. The frame dump drew
LOD00 for some near objects and LOD01 or LOD02 for others, so the game
chooses the level at run time, presumably from the distances in the
`.pvsz` record or the `.pgeo` group (not decoded).

Checked against the 130 main-view placements of the dumped frame
(`match-dump --pgeo`):

| Store | Same model | Same LOD set | Byte-identical mesh | Other |
| --- | --- | --- | --- | --- |
| `.pgeo` (any of the record's ID lists) | 23 | 0 | 0 | 0 |
| `.pvsz` | 88 | 8 | 11 | 0 |

"Byte-identical mesh" means the drawn mesh object is, byte for byte, a
mesh of the model the table names (signs that share a post or a board,
copies of one flag): the draw tie cannot tell such twins apart and takes
the lowest member number, so these are agreements the dump cannot
distinguish, not misses.

`dump-obj --box 1500 -1460 2500 -460 --props` now places, besides the
277 world-space models of the sector, 2,460 `.pgeo` instances and
845 of the 847 `.pvsz` objects whose origin lies in the box (2 name no
render model), each with the LOD00 model of its set.

Across the whole table, 1,087 objects name 268 member numbers that have
no render model in `bin.zip` (found by the implementation's reader and
reproduced here; why is not known).

The 28-byte records before the shader names follow a u32 count of
19,613. A record is a u32 that increases with gaps, the record number,
two f32 (1.0 in every record), ten zero bytes, a flag byte and a type
byte. The type counts match the texture members exactly (5,234 of type
4, the `.bix` pairs; 2,965 of type 0, the `_0x*.bin` containers; 11,409
of type 1; 5 of type 3), so this is
presumably the texture and material resource list; how an entry names a
file is not found (neither index is a filename-map position or a
texture's hex name beyond chance).

### Ruled out

- The earlier negative (no float triple within 0.05 m anywhere,
  `CollObjs.xml` included) was wrong: it searched for the biased
  placements of the full inverse.
- The placement IDs are not `FilenameMap_00.dat` indices, not `bin.zip`
  directory indices and not PVS record numbers offset by the name count
  (tested on the fence and pine IDs). No render model holds its own
  object IDs, and a scan of the loose track files for other ID-indexed
  tables at common strides found only false hits. They are object IDs
  (above).
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
models only.

**With props** (measured 2026-11-19, later the same day). A third replay
keeps the 820 world-space draws and the 515 main-view draws of the 130
props the frame drew (`depth-skips --props`: 1,444 other draws
skipped). The engine image is shift-engine `8a2932c` drawing the same 73
world-space models plus those 130 placements (`--drawn-props`).
`depth-compare --split` sorts each compared pixel by whether the props
change the engine's depth there (against the world-only image above).

| Pixels | Count | Median | 95th | Median, relative | 95th, relative |
| --- | --- | --- | --- | --- | --- |
| world | 519,023 | 0.007 m | 1.15 m | 0.025 % | 0.11 % |
| world, under 40 m | 266,157 | 0.001 m | 0.009 m | 0.023 % | 0.067 % |
| props | 51,087 | 1.41 m | 1,165 m | 13.7 % | 98.7 % |
| props, under 40 m | 39,840 | 1.12 m | 15.0 m | 9.5 % | 64 % |
| all | 570,110 | 0.016 m | 4.49 m | 0.028 % | 11.5 % |

The world pixels keep their tolerance. The prop pixels do not. Sorting
them (unshifted, 2 ‰ tolerance): 2,760 agree, 5,956 show the world
behind the engine's prop (the game has nothing there), 2,651 are sky in
the game, 6,570 are nearer in the game, and 43,217 are farther in the
game but are not the world either, which means the game's prop surface is
somewhere other than the engine's. Most of them are on the metal
barriers beside the road and on the flag. The causes, from the images:

- **Barrier orientation.** The engine draws the near metal barriers with
  a yaw clearly different from the capture's and the game's depth, while
  the `.pvsz` records of those placements match the dump's rotation to
  0.0005 (see [Reading the rotation](#pvsz-zone-files)). So the difference
  is in how the engine applies the record, not in the data. This
  accounts for most of the far-from-the-world residual.
- **Animation and cut-outs.** The flag is animated in the game (its
  depth is a different shape), and the chain-link fence and the pine are
  alpha-tested: the game writes depth only on opaque texels, the engine
  over whole triangles. These pixels cannot agree without the materials.
- **The instanced clip z row** (implementation question). Instanced draws
  use a clip z row slightly different from the scene matrix times the
  placement. Measured over the 515 prop draws at each model's origin,
  converting the game's z/w with the scene's z mapping errs by a median
  0.012 m (0.018 % of view depth), at most 0.16 m (0.029 %), about the
  size of the world residual. It is not what keeps the props out of
  tolerance.

## Open questions and next steps

1. **Placed models (resolved 2026-11-19).** The `.pgeo` and `.pvsz` IDs
   are object IDs; the [object table](#object-table) names their models,
   and the `.pvsz` framing is decoded. Still open: the record fields
   around the position and the LOD each object draws (question 3).
   `CollObjs.xml`'s `GraphicsName="#N"` is the object's own index, not a
   render-model number.
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
| How is a model known to be world-space or local-space? | No stored flag is known. The rule used here: local when the header box lies within 200 m of the origin in X and Z, which gives 11,898 world and 2,393 local; it is probable. A model that the object table names for a `.pgeo` or `.pvsz` placement is local |
| How does the engine place props? | Read the object table from `Ribbon_00/Colorado_00.pvs`, every `.pvsz` record (one per object; dedupe by object ID, then keep one placement per model and spot) and every kind-7 `.pgeo` group; the model is the table's member for the object ID (for `.pgeo`, the first ID of list 0), widened to its LOD set; place it as in [Reading the rotation](#pvsz-zone-files) (`.pvsz`) and F·Rᵀ·F·v + F·t (`.pgeo`). Which LOD to draw is the engine's choice until question 3 is answered: LOD00 near the camera matches the frame |
| Are the nine `.pvsz` halves row-major or column-major? | Columns: R[r][c] = h[3c + r]·F[r] in the file frame. Checked on all 107 drawn `.pvsz` placements, the 16 tilted ones included (largest error 0.0005); the row reading errs by up to 0.16 on the tilted ones. The earlier "H = F·R_gameᵀ" wording was ambiguous and is replaced |
| Which record start is right when several end at the end of the file? | The one with the most rigid rotations; the first such start is wrong in 56 zones (see [`.pvsz` zone files](#pvsz-zone-files)) |
| Instanced draws use another clip z row: how big is the depth effect? | A median 0.018 % of view depth at the model origin, at most 0.029 % (515 prop draws); see [Depth check](#depth-check-ri-04-gate) |
| Why are some `.pgeo` barrier sets (for example `models_proc_clrd_barrier_reds_*`) in the sector but not in the frame? | Not known: which placements the game shows (by event, time or PVS zone) is part of the selection question (open question 3). The files place them; the free-roam frame does not draw them |
| The kind-7 name offset | +0x60, or +0x98 when the word at +0x58 is non-zero (see the header table) |
| Model records whose IDs are not consecutive | 131 of 12,044; read each list's IDs as stored |
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
python -I tools/re/world-geometry.py depth-skips <drawn members> <dump.fh1frame> <runtime.log> [--props]
    (replay again with --fh1_debug_skip_draws=<the printed ranges>)
python -I tools/re/world-geometry.py depth-compare <world-depth.pfm> <frame file> \
    <band 1> <band 2> <band 3> --shift 0.5 0.5 [--split <world-only depth.pfm>]
```

With props, the members file is the 73 world-space models plus the 47 local-space models
the placements draw.

`--members @file` reads the comma-separated names from a file (the
selection used for the 130-placement check, the sector's 277
world-space models plus the 47 local-space models the placements draw,
is too long for a Windows command line). `dump-obj --props` and
`match-dump --pgeo` read the object table from
`.local/game/base/media/tracks/colorado/Ribbon_00/Colorado_00.pvs`
unless `--pvs` names another file. The memory
snapshot route is `.local/re/world/world-snapshot.fh1test` (local): the
world-dump inputs, then `snapshot 2340 freeroam`, run with
`--pinyon_shift_snapshot_virtual=true` to add the virtual-memory file.
