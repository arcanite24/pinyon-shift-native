# Cars: models, textures, paint and wheels for the showroom

| | |
| --- | --- |
| Spec ID | `FORMAT-CARS` |
| Status | draft for M1: the files are complete (every model and texture parses and lays out); the wheel fit is confirmed on 15 cars; the stock part selection agrees with 16 frame dumps; materials, paint, stripes, factory liveries, brakes and lamps are probable; the lamp and node blocks of the model header, the sizing of the six car-specific tyre models and the livery decal models are open |
| Written | 2026-11-19, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | `site/findings/files.toml` group "Textures and models"; `site/findings/content.toml` groups "Cars", "Upgrades, colours and liveries" and "Downloadable content" |
| Implements it | `shift-engine` `libs/fh1data` (`carbin`, `xds`) and `carview`, for plan item M1 |

Our own reading of the player's files, of one Autoshow frame dump and of
the game's database. No disassembly, decompiled code or game data is
reproduced here ([LEGAL.md](../LEGAL.md)); values are names and counts,
and the few measured numbers are our measurements. The census is
[`tools/re/car-formats.py`](../../tools/re/car-formats.py); the model
reader is [`tools/fh1_carbin.py`](../../tools/fh1_carbin.py).

## Summary

- M1 needs 240 distinct car models: the 176 base folders (175 cars and
  the placeholder `nul_car_00`) and 64 that only the 21 official DLC
  packages carry. v4 adds database rows but no car files. The community
  UDLC package (97 more car folders) is out of scope.
- A car is one folder of files named after `Data_Car.MediaName`: on the
  disc a ZIP archive `media/cars/<MediaName>.zip`; in a DLC package a
  folder inside the package's offer ZIP. Its wheels are a second archive
  named by `Data_Car.StockWheelID`. Brake models are loose files shared
  by all cars.
- `.carbin` models: all 2,587 models of the 240 cars parse with the
  layout below and write back byte for byte. One new result makes the
  header deterministic: it ends in a **draw-list block** that sorts
  every sub-section into a transparent and an opaque list per LOD, and
  the section count follows it.
- `.xds` textures: all 7,441 textures of the car and wheel folders are
  2D and tiled but one; the payload size of every tiled one equals the
  layout's exactly.
- Materials are technique names; 22 of the 284 techniques are drawn as
  transparent, and every sub-section of a technique is in the same list
  in every model. The showroom samples the body atlas in slot 0 and a
  technique-specific texture in slot 1.
- Paint: the body technique shows the paint colour where the atlas alpha
  is 0. The colour is a `Combo_Colors` row, converted from sRGB to linear
  before it reaches the shader (confirmed on one car). Calipers are
  tinted with `Data_Car.CaliperRGB*`, which reaches the shader unconverted.
- Stock parts: a model carries every body-kit variant (`bumperFa`,
  `bumperFp`, `wingl`, ...). The showroom draws only the parts the
  car's `IsStock` upgrade rows name, which is letter `a` on all but two
  rows, and draws nothing in a slot whose stock part is missing or a
  stand-in; see [Stock parts](#stock-parts).
- Factory liveries: the 15 cars with `Data_Car.HasRaceLivery` = 1 carry
  their livery in the body atlas and show the atlas colour instead of
  the paint; see [Factory liveries](#factory-liveries).
- Wheels: the rim model is the wheel archive's; tyres are three shared
  models. The wheel centres are rows 5 to 8 of `CarPartPositions`. Tyre
  and rim are scaled in the vertex shader from the car's tyre sizes:
  the lateral scale is measured, the radial rule is a guess.

## Where the files are

### Base cars

| Path (in the disc's `media` folder) | Count | What | Confidence | How |
| --- | --- | --- | --- | --- |
| `cars/<MediaName>.zip` | 176 | one car each: models, textures, livery masks, physics files | confirmed | listing; `Data_Car.MediaName` names each of the 176 base rows |
| `cars/Shared.zip` | 1 | textures and attributes every car may use: carbon fibre, dirt, bumper frame, undercarriage, grilles, dashboards, `CarPartAttributes.xml` | confirmed (contents), probable (use) | listing; the frame dump shows cars sampling them |
| `cars/AppearancePresets.zip`, `cars/Driver.zip` | 2 | parts presets and the driver model | probable | listing |
| `wheels/<WheelMedia>.zip` | 297 | one rim model each, plus the shared tyre archives `TireA`, `TireB`, `TireC` and truck variants of them | confirmed | listing; see [Wheels](#wheels) |
| `brakes/*.rmb.carbin` | 120 | loose caliper, rotor and drum models by size | confirmed | listing; see [Brakes](#brakes-calipers-and-rotors) |
| `carlights/*.xds` | 8 | headlamp glow and beam textures | probable | names |
| `shaders/Cars/` | about 150 effect files | the car effect (`shaders_v16.fxobj`), one effect file per technique and `CustomCarShaders.xml` (145 technique entries) | probable | names; the frame dump's techniques |
| `Livery.zip` | 1 | the paint shop's decal and vinyl shapes (1,372 `.carbin` files of another layout) | confirmed (count) | listing; see [Other carbin layouts](#other-carbin-layouts) |

Archive members use the archive method of the rest of the disc (LZX,
method 21), with a few stored or deflated members; see the archive
records in `site/findings/files.toml`. The game names a car's files by
folder, as `media\cars\<MediaName>\<member>`, so a folder of extracted
members and the archive are interchangeable (probable: the names are
the game's strings; a car archive with stored members loads in game,
see "a car with its own model archive").

### Car members

| Member | Per car | What | Confidence | How |
| --- | --- | --- | --- | --- |
| `<MediaName>.carbin` | 1 | exterior LOD1 to LOD5 | confirmed | sub-section LODs 1-5 only |
| `<MediaName>_lod0.carbin` | 1 (2 cars have none; one is empty) | close exterior, 32-bit index buffers | confirmed | parse; the frame dump draws it in the showroom |
| `<MediaName>_cockpit.carbin` | 1 (3 cars have none) | interior and the parts seen from it: seats, steering wheel, gauges, mirrors, headlamp units | confirmed | part names; drawn in the showroom |
| `<MediaName>_caliper<W>_LOD0.carbin`, `_rotor<W>_LOD0.carbin` | 4 each (`W` = LF, RF, LR, RR); 6 cars have none | the car's own calipers and rotors, about the hub | confirmed | parse; drawn in the showroom |
| `stripped_*.carbin` | one per model | an index-only twin, fully determined by its model | confirmed | see [Stripped twins](#stripped-twins) |
| `nodamage_LOD0.xds`, `nodamage.xds`, `damage_LOD0.xds`, `damage.xds` | 1 each | the body atlases for LOD0 and LOD1-5, clean and damaged | confirmed | see [Body atlas](#body-atlas) |
| other `nodamage*.xds` | 0 or more | alternative clean atlases that `Combo_Colors.BaseTexture` names | confirmed (names) | every `BaseTexture` names a member of the car's own archive |
| `interior_LOD0.xds`, `interior_emissive_LOD0.xds`, `lights_LOD0.xds`, `dirt_mask.xds` and similar | varies | technique textures (slot 1 and up) | confirmed (drawn) | frame dump |
| `LiveryMasks/Masks.xml`, `LiveryMasks/<view>.tga` | 1 and up to 6 | livery planes | confirmed | see [Livery masks](#livery-masks) |
| `ShaderSettings.xml` | 0 or 1 | per-car overrides of technique parameters | probable | contents name techniques |
| `physicsdefinition.bin`, `Physics/MAXData.xml` | 1 each | physics; not needed for the showroom | confirmed (names) | listing |

Six cars have no calipers or rotors and three no cockpit (the three
are among the six); the M1 car list says which rows reach the
Autoshow. `FOR_F350SuperDuty_08_lod0.carbin` is a header with no
parts.

### v4

The v4 title update's `media.zip` holds no `media/cars` folder. It adds
15 `Data_Car` rows through its merge database: 9 reuse base folders and 6
use the Rally package's folders. It does carry `shaders/cars` (six effect
files, among them `shaders_v16.fxobj` and `dropshadow`), which replace
the disc's (probable: same names). An engine that reads v4 takes car
files from the disc and the packages, and the car effect from v4.

### DLC packages

The engine reads the installed packages' extracted files, not the STFS
containers. The install (the launcher's state root) keeps each package
as a folder:

```text
<state root>/user/0000000000000000/4D5309C9/00000002/<package id>/
    Media/<offer>.puboffer
    Media/DLCZips/<offer>_pri_65.zip
        media/cars/<MediaName>/<members>      (or Media/cars/...)
        media/wheels/<WheelMedia>/<members>
        db/patch/<n>_merge.slt and other folders
    Media/DLCZips/<offer>_pri_65/Media/audio/...   (loose audio folders)
    Media/DLCZips/zipmount.xml
    Media/DLCZips/cameras_pri_<n>.zip, StringTables_pri_<n>.zip, ...
```

`<package id>` is 42 hex digits (`4D5309C900000729` for UDLC). The offer
ZIP is not listed in `zipmount.xml`, which mounts only the extra ZIPs
(cameras, string tables, tuning folders) at the paths it names; the offer
ZIP's own folders sit at the media root (probable: the member paths
mirror the disc's). Its members use methods 21, 8 and 0, so the
reader needs LZX and deflate.

| Fact | Count | Confidence | How |
| --- | --- | --- | --- |
| official car folders in the 21 packages | 83 | confirmed | listing of every offer ZIP |
| byte-identical copies of a disc car | 19 | confirmed | SHA-256 of every member against the disc |
| DLC-only cars | 64 | confirmed | the rest |
| cars in more than one package | 12, all byte-identical across packages | confirmed | SHA-256 of every member |
| car rows in the merge databases | 84, for 83 media names | confirmed | merge databases |

**Database rows.** A package's car rows come from its merge database,
`media/db/patch/<n>_merge.slt` inside the offer ZIP (15 of the 21
packages carry one; the other six add no car rows), merged over v4's
database as whole rows by key in package priority order
(`tools/re/car-list.py`; record "M1 car denominator"). Over the merged
database's 250 `Data_Car` rows every join this page uses resolves: each
`MediaName` has a car folder, each `StockWheelID` a `List_Wheels` row
whose media has a wheel folder, each car at least one `Combo_Colors`
row and the four wheel rows of `CarPartPositions`. `CarPartPositions`
has no unique key, so a whole-row merge leaves identical duplicate rows
for 37 cars; read any one of them.

**Rule.** Key cars by media name compared without case. Take a folder
from the disc first and from the packages in package order; copies are
identical, so the order changes nothing for the official set. A media
name that no source has is an error the car list reports, not a crash.

## `.carbin`: car models

All values are big-endian. After the first variable-length name nothing
is aligned. A model is:

```text
header (variable) | u32 part count | parts | footer (24 bytes)
```

### Header

The header has three kinds by its first word, and ends in the
draw-list block, which locates the part count.

| Field | Size | Meaning | Confidence | How |
| --- | --- | --- | --- | --- |
| kind | u32 | 5 for exterior, LOD0 and cockpit models; 1 for calipers, rotors and the loose brake models; 3 for wheel (rim) and tyre models | confirmed | all 2,587 models and 120 brake files; all 240 stock wheel models |
| (kind 3) bounds | floats from byte 16 | the rim's extents: x maximum at byte 32, the design +x face at 36, x minimum at 40, the design −x face at 44; further radii follow (the morphed outer radius, the LOD1 radius) | confirmed (32-44), probable (rest) | the lateral scales of 15 cars' wheel draws ([Wheels](#wheels)); 239 of 240 hold finite values |
| (kind 5) five records | 5 × 64 bytes | not decoded; in the exterior model the first four begin with four floats, the fifth holds large integers (perhaps buffer sizes) | guess | census |
| (kind 5) | 16 bytes | not decoded | | |
| (kind 5) lamp count `L` | u32, at byte 340 | number of lamp points, 4 to 24 on the exterior models | confirmed | the block after them starts at 344 + 32 `L` in every kind-5 model |
| (kind 5) lamp points | `L` × 32 bytes | see [Lamp points](#lamp-points) | probable | |
| node block | variable | starts with the words 17, 16, 1, `n`: exhaust-tip records with 4 × 4 matrices, a count of three points (seat or driver), identity 3 × 4 matrices and two floats. Not decoded; a showroom does not need it | guess | census of the word patterns |
| draw lists | variable | see below | confirmed | every model |

**Draw-list block.**

| Field | Size | Meaning | Confidence | How |
| --- | --- | --- | --- | --- |
| tag | 2 × u32 | 4, 4 | confirmed (constant) | all models with parts |
| LOD count `n` | u32 | 5 in exterior models, 1 in every other model | confirmed | 240 and 2,346 models |
| transparent lists | `n` lists | list `k` holds the sub-sections drawn after the opaque ones, mostly without depth writes, at LOD `k` (LOD `k + 1` in an exterior model) | confirmed (lists), confirmed for one car (draw order) | every entry's sub-section LOD matches its list |
| opaque lists | `n` lists | the same for opaque sub-sections | confirmed | same |
| list | u32 count + count × u32 | each entry is (part index × 65536 + sub-section index) | confirmed | every entry names an existing sub-section, and every sub-section appears exactly once in the model's lists |
| end | u32 | 1, then the part count | confirmed | all models |

**Locating it.** Scan the file from its start at 4-byte offsets for the
words 4, 4, `n` with 0 ≤ `n` ≤ 8, whose 2`n` lists parse and are
followed by the word 1 and a part count `c`, where either `c` = 0 and
exactly the 24-byte footer follows, or `c` > 0 and a part parses right
after the count. Over the whole file, not only the header, this finds
exactly one position in every one of the 2,587 models, and every header
is a multiple of 4 bytes. A model with no parts (the empty F-350 LOD0 and
the two empty brake files) is the `n` = 0 form: the words 4, 4, 0, 1, 0
and the footer. An implementation can scan, or walk the lamp and node
blocks once they are decoded.

The render models of [WORLD_GEOMETRY.md](WORLD_GEOMETRY.md) carry the
constant words 4, 4, 0, 1 before their object count: by this layout that
is the same block with no lists (probable).

**Footer.** Six words, 1, 0, 1, 0, 1, 0, in every model (meaning
unknown; write them back as read).

### Lamp points

Each 32-byte point starts with a position (3 × f32). In the exterior
models checked the points sit on the lamp lenses with z negated against
the model's own frame (a lamp point and its lamp part agree in x and y
and have opposite z), so the lamp points use the physics files' +Z
forward. One byte is the side (1 left, 2 right, matching the sign of x),
one byte takes values 0 to 4 (a lamp kind; which value is which is not
known), and three bytes look like an RGB colour: red or amber on
1,836 of the 2,035 points at the rear (z below 0 in that frame) and
white or blue on 792 of the 1,264 at the front. The rest is not
decoded. The z sign was checked against the lamp parts of one car. Probable as a whole; the engine needs none of it for a lit
showroom with lamps off.

### Parts

One part per named piece (`body`, `hood`, `glassF`, `taillightL`, a
gauge needle). Parts are not aligned.

| Field | Size | Meaning | Confidence | How |
| --- | --- | --- | --- | --- |
| marker | u32 | 5 | confirmed | every part |
| box | 9 × f32 | offset (the centre), then the minimum and maximum relative to it; minimum = −maximum | confirmed | every vertex decodes inside it; the game sends the offset and the length of the half-extent vector to the vertex shader (frame dump) |
| | 28 bytes | mass, centre of mass and damage fields; not decoded | guess | |
| records | u32 count + count × 16 bytes | glass parts only; not decoded | | |
| | 4 bytes | | | |
| shorts | u32 count + count × u16 | not decoded | | |
| | 12 bytes | | | |
| name | u8 length + bytes | the part name | confirmed | strings |
| version | u32 | 3 | confirmed (constant) | all 20,991 parts |
| LOD pool | pool | vertices of LOD1-5 (exterior model only) | confirmed | indices of LOD 1-5 sub-sections stay inside it |
| | 4 bytes | | | |
| sub-sections | u32 count + sub-sections | one per material and LOD | confirmed | |
| | 4 bytes | | | |
| LOD0 pool | pool | vertices of LOD0 (all other models) | confirmed | indices of LOD 0 sub-sections stay inside it |
| damage tail | see below | | probable | |

A part can be empty: no vertices and a box of ±FLT_MAX. Skip it.

**Pool.** u32 vertex count, u32 stride (28 for every pool with
vertices, 0 for an empty one), u32 buffer flag (0 no buffer, 1 a buffer with its data, 3 the
same buffer in a stripped twin), u32 buffer id when the flag is not 0,
then count × stride bytes (confirmed: byte-exact round trip; the stripped
twins confirm the flag).

### Vertex (28 bytes)

| Offset | Size | Field | Confidence | How |
| --- | --- | --- | --- | --- |
| 0 | 3 × i16 | unit direction (x, y, z) / 32767 | confirmed | norm 1.0000 on every vertex |
| 6 | i16 | length w / 32767, a fraction of the length of the part's half-extent vector | confirmed | positions fill the part box within 0.03 % |
| 8 | 2 × u16 | UV0 / 65535, before the sub-section's transform | confirmed | |
| 12 | 2 × u16 | UV1 / 65535, before the transform | confirmed (layout), guess (use: a tiled detail map) | |
| 16 | 4 × i16 | unit quaternion (x, y, z, w) / 32767: it turns +X to the normal, +Y to the tangent along U and −Z to the tangent along V | confirmed | the rotated +X agrees with the area-weighted face normal within dot > 0.9 on 97.8 % and > 0.5 on 99.9 % of 3.96 million LOD0 vertices of 40 cars; norm 1 on all but 34 vertices |
| 24 | 4 × u8 | baked lighting: the first byte behaves as ambient occlusion (about 100 open to the sky, about 60 under the car), the other three as a signed vector that follows (−ny, nz, −nx) of the normal at a length of about 90 | probable | fits against the normal over one car body |

Position = direction × (w / 32767) × |half-extent| + offset. The sign of
the quaternion's w is not handedness: both signs occur.

**Car space.** +X to the car's right, +Y up, +Z to the rear; the body's
lowest point is at y = 0 (confirmed: steering wheel, bumpers and wheel
arches).

### Sub-sections and index buffers

| Field | Size | Meaning | Confidence | How |
| --- | --- | --- | --- | --- |
| | 5 bytes | not decoded | | |
| UV transform | 8 × f32 | (U offset, U scale, V offset, V scale) for UV0, then for UV1: u' = offset + scale × u, counted from the top of the texture | confirmed | the transformed UVs land on the part's region of the body atlas |
| | 36 bytes | not decoded | | |
| technique | u32 length + bytes | the material: a car shader technique name | confirmed | strings; frame dump |
| LOD | i32 | 0 indexes the LOD0 pool, 1 to 5 the LOD pool | confirmed | |
| index type | u32 | 6 triangle strip, 4 triangle list | confirmed | 18 lists in 171,840 sub-sections |
| | 56 bytes | not decoded | | |
| version | 2 × u32 | 4, 0 (4, 2 in a stripped twin) | confirmed | |
| index count, index size | 2 × u32 | size 2 or 4 | confirmed | |
| indices | count × size | strips restart at 0xFFFF (16-bit) or 0x00FFFFFF (32-bit), with no restart at the end | confirmed | every index is inside its pool |
| | 4 bytes | not decoded | | |

**Winding.** With A, B, C a triangle's vertices in index order,
(B − A) × (C − A) points out of the surface; in a strip every second
triangle has its first two vertices swapped (confirmed against the
quaternion normals). Whether the game culls back faces on cars is not
established; see Unknowns.

### Damage tail

After a part's pools: u32 1, a u8 flag (1 when the part deforms), then one
block per pool (LOD, then LOD0) with a pool's header (u32 3, vertex
count, stride 4, the buffer flag and id) and one 4-byte word per vertex.
Parts that do not deform have empty blocks. In car models a showroom
ignores the words (probable layout; the per-vertex meaning is a guess).

In wheel and tyre models the block has stride 16 instead: after the
buffer id, two morph targets per vertex, each four half floats read as a
direction and a length (position = xyz × w, in the pool's units), each
32-bit word holding its two halves low half first (confirmed: the
decoded targets reproduce the drawn rims and tyres of 15 cars within
1.2 mm). They are not damage: the game uses them to size the wheel (see
[Wheels](#wheels)). The rim's LOD0 block holds one target, the LOD1-5
block two.

### Stripped twins

`stripped_<lower-case name>.carbin` keeps every header, count and
buffer id of its model but no vertex, index or damage bytes; pool and
damage flags 1 become 3. A twin is fully determined by its model
(confirmed for all 1,883 base twins). The engine does not read them.

### Other carbin layouts

- **Loose brake models** (`media/brakes`, kind 1): 118 of the 120 parse
  with the car layout. `Circle01` and `drum_null_caliper` are empty
  models (no parts). Confirmed.
- **Livery shapes** (`Livery.zip`, 492 under `decals/`, 880 under
  `vinyls/`): close to the render-model layout of
  [WORLD_GEOMETRY.md](WORLD_GEOMETRY.md#mesh-object), not to the car
  layout. The 664 with first word 3 continue with a box as two
  (x, y, z, 0) vectors, the words 1, 0, then 4, 4, 0, 1 (the empty
  draw-list block) and an object count of 1, then one mesh object whose
  tag is 2, not 1: centre, half-extents, a name (`decal`, or the
  vinyl's own name), a vertex format (2 or 3), a vertex count, stride 36
  and the vertices; some decals then carry what looks like an embedded
  texture. The world-geometry reader does not walk them as they are (no
  matrix in the header, tag 2). The 708 with first word 1 were not
  examined. Probable for the kind-3 framing, open for the rest; not
  needed for M1, which shows factory paint.

## `.xds`: textures

| Offset | Size | Field | Confidence | How |
| --- | --- | --- | --- | --- |
| 0 | u32 | 3 | confirmed (constant) | all 7,441 car-folder textures |
| 4 | 6 × u32 | 1, 0, 0, 0, 0xFFFF0000, 0xFFFF0000 | constant, meaning unknown | all |
| 28 | 6 × u32 | the console's texture fetch constant | confirmed | below |
| 52 | | the texel data | confirmed | |

From the fetch constant (dword `d`, bits counted from 0): type
d0 bits 0-1 (2, a texture), pitch d0 bits 22-30 × 32 texels, tiled
d0 bit 31, format d1 bits 0-5, endian mode d1 bits 6-7, width − 1 d2
bits 0-12, height − 1 d2 bits 13-25, level count − 1 d4 bits 6-9,
dimension d5 bits 9-10 (1 = 2D), packed mip tail d5 bit 11. This is the
console's published fetch-constant layout as open emulators document it.

| Format | Name | Bytes per 4 × 4 block | Base and wheels | DLC | Use |
| --- | --- | --- | --- | --- | --- |
| 53 | DXT4/5 as 16-bit | 16 | 4,148 | 2,081 | atlases, most technique textures |
| 51 | DXT1 as 16-bit | 8 | 421 | 262 | |
| 49 | DXN (two channels) | 16 | 206 | 138 | `_NRM` normal maps |
| 59 | DXT5A (one channel) | 8 | 14 | 36 | masks |
| 18 | DXT1 | 8 | 4 | 0 | |
| 50 | 8888 as 16-bit (4 bytes a texel) | — | 1 (linear) | 44 | |
| 6 | 8888 (4 bytes a texel) | — | 0 | 86 | DLC thumbnails |

Every texture is 2D, every texture has the packed-tail flag, and every
one but `Shared/grille1_NRM_s_LOD0.xds` (linear) is tiled. Block
textures use endian mode 1 (8-in-16 swap) and the 4-byte formats mode 2
(8-in-32). Sizes need not be powers of two (from 2 to 2048; 6,440 have
the full level chain, 1,001 one level).

**Layout.** The data is the base level, then each further level in its
own region, until the packed tail:

- A level's region is a tiled surface of 32 × 32-block tiles. Its pitch
  is the level's width in blocks rounded up to 32, taken from the size
  rounded up to a power of two and halved per level (the base level uses
  the header pitch when that is larger); its height in blocks is rounded
  up to 32 the same way. Its byte size is rounded up to 4,096.
- The levels whose shorter side, rounded up to a power of two, is 16
  texels or less share one packed tile at the end. With `L` and `S` the
  base-2 logarithms of the longer and shorter side rounded up, the tail
  starts at level `max(S − 4, 0)`, and tail level `t` (0 for the first
  level in the tail) sits at this texel offset in the tile (divide by 4
  for block formats):

  | Tail level `t` | Offset along the shorter axis | Offset along the longer axis |
  | --- | --- | --- |
  | 0, 1, 2 | 16 >> `t` (16, 8, 4) | 0 |
  | 3 and up | 0 | 2^(`L` − `max(S − 4, 0)`) >> (`t` − 2) |

  The shorter axis is y for a texture wider than tall, x otherwise
  (square textures count as not wider). A texture whose base level is
  that small is all tail (209 textures): its one region takes its pitch
  and height from the size rounded up to powers of two, not from the
  header pitch.
- Inside a region, block (x, y) is at the console's standard 2D tiled
  offset. Each block is byte-swapped by the endian mode.

Confidence: confirmed for region sizes and for full-size levels. The
payload of each of the 7,440 tiled textures equals the layout's size
exactly; 698 base body atlases and 154 Autoshow pictures decode and
re-encode byte for byte (accar branch `tools/fh1_xds.py`). The tail
offsets and the all-tail textures are probable: the 209 all-tail
textures are size-checked only, and no tail level was compared texel
by texel. DXN and DXT5A are size-checked only; their blocks are the
standard two-channel and one-channel BC formats.

## Materials

The technique name is the material. There are 284 techniques in the 240
cars (266 in the base cars). The most frequent: `black`, `body`,
`chrome`, `frame`, `plastic2`, `window`, `matte_colors`, `misc`, `badge`.
`CustomCarShaders.xml` maps 145 names to effect techniques, and a car's
`ShaderSettings.xml` overrides parameters for that car (probable).

**Transparent techniques** (from the draw lists; confirmed: each
technique is in only one kind of list in every model). In the showroom
frame of the stock XKR-S, each of the three times the car is drawn, the
transparent-list draws come after the opaque ones, and 138 of the 162
transparent draws have depth writes off while 1,032 of the 1,038 opaque
draws write depth; the blend register is the same for both, so blending
itself was not read from the dump. The techniques are: `window`,
`window_2`, `frontwindow`, `glass`, `glass_2`, the `glass*_LOD<n>`
names, `detail_glass_clear`, `detail_glass_red`, `detail_glass_amber`,
`lights_glass`, `light_glass`, `red_glass`, `blur_rim` and
`chrome_blur_rim`. Every other technique is opaque.

**Texture slots** (pixel-shader fetch slots in the showroom frame;
confirmed for one car, the stock XKR-S, by matching each slot's texture
to the archive member with the same bytes):

| Techniques | Slot 0 | Slot 1 | Slots 2 and up |
| --- | --- | --- | --- |
| exterior paint: `body`, `body_2` | `nodamage_LOD0` | — | `dirt_pattern`, `dirt_mask` |
| `rubber_trim` | `nodamage_LOD0` | `dirt_pattern` | `dirt_mask` |
| `black`, `frame`, `plastic2`, `matte_colors`, `emblem`, `bottom`, `weatherstrip`, `misc`, `chrome` (cockpit) | `nodamage_LOD0` | `dirt_mask` | |
| `undercarriage` | `nodamage_LOD0` | `dirt_mask` | `undercarriage` (Shared) |
| `carbon_fiber` | `nodamage_LOD0` | `carbonFiber` | `carbonFiberNormal` (Shared) |
| `bumper_frame`, `grille1` | `nodamage_LOD0` | `bumper_frame`, `grille1_alpha` (Shared) | |
| lamps: `taillight2S`, `taillightst`, `tail_light_lod0`, `reverse_light_lod0`, `indicator_left`, `indicator_right`, `xenonhead`, `slorange`, `textured_reflector`, `numplate`; `drlwhite` (no slot 0) | `nodamage_LOD0` | `lights_LOD0` | |
| interior: `leather`, `cloth`, `plastic`, `solid`, `mottled`, `headliner` | (`nodamage_LOD0` on some) | `interior_LOD0` | `interior_emissive_LOD0` on some |
| interior with normal maps: `bump_leather`, `bump_plastic`, `bump_stitching` | | `interior_LOD0` | `leather_NRM`, `plastic_NRM`, `stitches_BUMP` |
| gauges: `gauge_emissive`, `z_gauge_emissive_alpha` | | `interior_LOD0` | `interior_emissive_LOD0` |
| glass: `window`, `glass` | | | slot 7: a small texture shared by several files (environment or a flat swatch; not settled) |
| rim: `rim`, `inner_rim`, `outer_rim`, `wheel_black`, `blur_lip` | `wheel_LOD0` (wheel archive) | | |
| `blur_rim` | | | slots 3-5 and 7: small textures (the effect's samplers are named blur rim 1 to 4, which suggests `wheel0`-`wheel3.xds`; probable) |
| tyre: `sidewall`, `tread` | | `sidewallA_CLR`, `treadA_CLR` | `sidewallA_NRM`, `treadA_NRM` |
| calipers and rotors: `caliper`, `badge`, `brake`, `hub` | `nodamage_LOD0` | | |

The other slots hold runtime textures (shadow, reflection, lighting).
The LOD1-5 exterior uses `nodamage.xds` in slot 0 (probable by name and
size; LOD1-5 were not in the showroom frame).

**Groups an implementation needs** (probable: names, slots and lists):

- glass: the transparent techniques above, except `blur_rim` and
  `chrome_blur_rim` (spinning-rim cards, hidden when the wheel is still);
- lamps: `head_light`, `hidhead`, `oldhead`, `xenonhead`, `fullbeam`,
  `taillight2S`, `taillightst`, `tail_light*`, `indicator_*`,
  `reverse_light*`, `drlwhite`, `slorange`, `lights_gls_*`,
  `textured_reflector`, `reflector`;
- interior: every technique that samples `interior_LOD0`, plus
  `gauge_needle` and `digital_gauge`;
- tyres: `tire` (in car models), `sidewall`, `tread` (tyre models);
- rims: `rim`, `inner_rim`, `outer_rim`, `wheel_black`, `blur_lip`, `hub`.

## Body atlas

`nodamage_LOD0.xds` (2048 on 172 of 176 base cars) belongs to the LOD0,
cockpit, caliper and rotor models; `nodamage.xds` (1024 on 158, 512 on
18) to the LOD1-5 model. Each has a `damage` twin of the same layout.
Nearly all are format 53. A sub-section's UV transform places its part
in the atlas. Over 20 cars, the `body` technique samples texels with
alpha 0 at 97 % of its vertices; `black`, `frame` and `plastic2` sample
alpha 255; `chrome` 23 % alpha 0 (probable).

## Stock parts

A car's LOD0, cockpit and LOD1-5 models hold every body part the
upgrade shop can fit, overlapping in place: the stock part and each
aftermarket kit of a slot are separate parts named by a family and a
letter (`bumperFa`, `bumperFp`, `bumperFrace`). Only the stock set is
drawn in the showroom.

**Families.** The upgrade tables name parts through
`List_PartsStrings` (136 strings): `BumperF<x>`, `BumperR<x>`, `Wing<x>`,
`Hood<x>` and the pair `SkirtL<x>,SkirtR<x>` for `x` from `a` to `z`
(hoods `a` to `x`),
the race parts `BumperFRace`, `BumperRRace`, `WingRace`, `hoodPSrace`,
`hoodCIrace` and `cagerace`, and `-seatL` and `-seatR`, which remove a
seat. Part names compare without case.

| Table | Keyed by | Selects | Confidence | How |
| --- | --- | --- | --- | --- |
| `List_UpgradeCarBodyFrontBumper`, `...RearBumper`, `...SideSkirt`, `...Hood` | `CarBodyID` (the car's `List_UpgradeCarBody` stock row, car id × 1000) | `PartsStringId` | confirmed | database; 16 frame dumps |
| `List_UpgradeRearWing` | `Ordinal` = car id | `PartsStringId` | confirmed | same |
| `List_UpgradeCarBodyChassisStiffness`, `List_UpgradeCarBodyWeight` | `CarbodyId` | `PartsStringID` (0 = none): the roll cage, a removed seat | probable | database; the cage was not in a dump |

**Rule.** For the `Data_Car` row being shown (by car id, not by media
name: two rows can share a model and differ in stock parts):

1. The stock part of each slot is the string of that table's row with
   `IsStock` = 1. Draw it, and hide every other member of the same
   family, every race part, and `cagerace`, unless a stock row names it.
2. Exhausts (`exhaust<x>`, `exhaustL<x>`, `exhaustR<x>`) are named by no
   table (`List_UpgradeEngineExhaust.PartsStringID` is 0 on every row).
   Draw letter `a` and hide the other letters. On 249 of the 250 rows the
   stock rear bumper is `a` too, so this cannot be told apart from
   "the exhaust follows the rear bumper's letter"; the exception is
   `VW_IECorrado_95` (stock rear bumper `d`, only `exhausta`), not an M1
   reference car.
3. A seat a stock weight row removes (`-seatL`, `-seatR`) is not drawn.
4. A stock part that the model lacks, or that is a stand-in (a pool of
   at most 6 vertices, a 17-vertex cube, or at most 24 vertices all on
   their box's corners), leaves its slot empty. Never promote another letter. The
   LOD0 stand-in may be filled from the LOD1-5 model's part of the same
   name, as for any other part.
5. Sub-parts that extend a family member's name (`winga_arm1`,
   `winga_piston`, `winga_lower`) follow that member.
6. Other lettered names (`undercarriagea`, the Mini's `wheela` and
   `wheelb`, `mirrorRa`) belong to no upgrade family; keep the first real
   letter, as the first M1 build does.

`Shared/CarPartAttributes.xml` agrees: its `DefaultParts` list names the
families with a `#` placeholder (`bumperF#`, `bumperR#`, `hood#`,
`wing#`, `exhaust#`, `exhaustL#`, `exhaustR#`, `trunk#`, `mirrorL#`) and
a `NonUpgradeableParts` list (`door*`, `engine*`, `badge*`,
`undercarriage*`, `a_pillar*`); `CarInvalidDefaultParts` is empty. The
executable holds the query that reads a `List_PartsStrings` row by id.

**What the first M1 build did instead.** It hid race parts and, of a
family, a letter from `b` to `h` when an earlier letter was a real
part. Two faults follow: letters after `h` are never hidden (the RX-8
Mazdaspeed drew its stock `a` kit and the `o`, `p`, `r`, `t` and `u`
kits on top of each other), and when the stock `a` part is missing or a
stand-in the next letter is drawn (the FJ Cruiser's roof rack `wingc`,
the Beetle's roof rack `wingb`). Set `a` itself was right on all six
failing cars.

**Evidence.**

- Frame dumps: every part draw carries the part's box offset and
  half-extent length as vertex-shader constants (see [Parts](#parts)),
  so a dump tells which parts the game drew. Over 16 Autoshow dumps,
  165 variant and seat parts (parts with a box identical to another
  part's excluded) agree with the rule with no exception; the first
  build's rule disagrees on 3 (the Mini Coupé JCW's `exhaustLc` and
  `exhaustRc`, the CR-X's `wingc`). The CR-X's `exhaustd` has the same
  box and vertex count as `exhausta`, so a dump cannot separate them.
- Over the 250 merged rows the rule changes the drawn parts of 38 rows
  (32 only lose parts). All six cars of the M1 report that failed on
  kits change: the RX-8 Mazdaspeed loses 17 kit parts, the Supra 6, the
  Celica ST185 5 (`bumperFi`, `hoodi`, `skirtLj`/`skirtRj` and
  `wingj`; by the sheet these are the rally light pods, roof vent, mud
  flaps and wing, matched by eye), the BRZ 28, the FJ Cruiser and the
  Beetle their roof racks. On the sheets of the Supra, Celica, Beetle
  and BRZ the kit is exactly the engine-only region of the mask diff.
- 22 cars that already pass the M1 numeric gate also change, all by
  losing parts: side skirts `b` to `e` (206 RC, Golf GTi, S2000, Silvia,
  Rabbit, 240SX, 037 Stradale, Scirocco R and its LCE twin, Bel Air,
  Escort Cosworth, M135i, Civic Si 2013, and the RX-7 with two hoods),
  wing `b` (370Z, ML63,
  RX-3, Vandura, Sprinter Trueno) and kit sets after `h` (Civic Type R,
  Lancer Evo VI, FR-S). On the sheets checked (370Z, Vandura, RX-7,
  Civic Si, Lancer Evo VI, FR-S) the removed part is exactly the
  engine-only region of the mask diff, so these cars move towards the
  reference.
- Stock rows name a letter other than `a` on two rows only:
  `VW_IECorrado_95` (rear bumper `d`, skirts `b`) and the v4 rally
  `SUB_WRXSTi_05` row 1626 (front bumper `d`, skirts `e`, hood `f`, wing
  `d`, a stock roll cage). That these two rows draw their non-`a`
  parts rests on the database alone: neither has a reference or a dump.
  Three rows have a stock roll cage and three a
  removed seat (the three Showcase cars and that WRX). None is an M1
  reference car.
- The Beetle (1968)'s stock `winga` is a 3-vertex LOD0 stand-in whose
  LOD1-5 twin has 15 vertices; filling it draws a small piece where the
  game shows none (visual, one car; whether the game skips that twin is
  not known).

`tools/re/car-stock-parts.py census` prints the per-row changes and
counts, `dumps` the frame-dump check.

## Paint

| Fact | Confidence | How |
| --- | --- | --- |
| A car's factory colours are its `Combo_Colors` rows (1 to 31 a car, 1,281 rows): `ID` = car id × 1000 + n, `Sequence`, `Metallic` (0 or 1), `RGB` = r × 65536 + g × 256 + b, `BaseTexture`, `DamageTexture` | confirmed | database; a clone given seven rows showed seven swatches |
| The showroom shows the row with the lowest `Sequence` | probable | three pilot cars ([AUTOSHOW_REFERENCE.md](AUTOSHOW_REFERENCE.md#paint-colour); record "Autoshow showroom colour") |
| On exterior techniques a pixel-shader constant equals that row's RGB / 255 converted from sRGB to linear; it is 0 on other techniques | confirmed for one car | frame dump of the stock XKR-S |
| The paint shows where the atlas alpha is 0; alpha 255 shows the texel (badges, engine bay) | probable | atlas content and the body technique's alpha census |
| `BaseTexture` names the clean atlas to use with that colour (`nodamage*.tga` in the table, shipped as `.xds`); `DamageTexture` is always `damage.tga` | confirmed (names), probable (use) | every name resolves to a member of the car's archive |
| `Metallic` picks the metallic finish; `List_ColorFinish` names 0 Normal, 1 Metallic, 2 CarbonFiber | probable | names |
| A factory stripe, decal or contrasting part (the Abarth 500's ABARTH side stripe and mirror caps) is texels of the body atlas with alpha 255 under an exterior paint technique, mostly white; the mirror caps are the cockpit model's `mirrorL` and `mirrorR` parts, technique `body` | probable | atlas texels at the vertices: all 1,048 of the caps' are white at alpha 255 |
| Where a colour row has a `Combo_Colors_Exceptions` row (`Ordinal` = car, the same `Sequence`) with `HasStripe` 1, the exterior draws also receive a constant equal to `ColorStripe` (r × 65536 + g × 256 + b) / 255 converted from sRGB to linear; it tints the alpha-255 texels, so white stripe texels show the stripe colour | confirmed (constant, one car), probable (use) | the Abarth 500's frame dump against its first row's red stripe colour, on its body, skirt and mirror-cap draws |
| A `BaseTexture` other than `nodamage.tga` (`nodamage_noStripe`, `nodamageBlack` and others) is an atlas where the stripe texels differ: in the Abarth's `noStripe` atlas the stripe texels have alpha 0, so they take the paint | confirmed (one car's two atlases), probable (rest) | texel diff: 127,275 texels differ |
| `HasColor2` and `Color2` are never set: factory cars have no second paint colour; their second colour is the stripe colour or the texel | confirmed | 0 of 674 exception rows |
| `StripeColors` (`Ordinal`, `Sequence`, `ColorStripe`, `MetallicAmount`), `List_SpecialColors` (two-tone scale, bias and power) and the effect parameters `psPaintTwoTone*` serve the paint shop | guess | names |
| Glass draws receive no paint (the paint constant is 0) and do not sample the body atlas; the windscreen's tinted top band is the glass techniques' own | probable | constants on four cars' glass draws (the same grey constant on all four); the slot table above |

**Rule for M1.** Paint = the lowest-`Sequence` row's RGB, each channel
/ 255, converted from sRGB to linear, applied where the slot-0 texel's
alpha is 0 on exterior techniques; texel colour elsewhere. Metallic
cars use a metallic finish of the engine's choosing until the effect's
paint terms are specified (log the car). Stripes: where the showroom
row has an exception with `HasStripe` 1, multiply the alpha-255 texels of
the exterior paint techniques (the LOD0 and cockpit models' `body` and
`body_2`) by `ColorStripe` converted to linear; blend by alpha between
paint and the tinted texel (the blend at intermediate alpha is probable).
Without an exception, show the texel colour. Use the row's `BaseTexture`
atlas. Never apply paint or the body atlas to glass techniques. A car
with `HasRaceLivery` = 1 takes no paint at all: see
[Factory liveries](#factory-liveries).

The stripe colour can look different from `ColorStripe` in the
references: the Challenger SRT8 392's showroom row is white with a
`ColorStripe` of (23, 25, 25), near black, and its atlas's stripe texels
are black, yet the reference shows blue stripes on the hood and roof. No
texel in either of its atlases is blue, the vertical bumper stripes stay
dark, and other dark cars in the same showroom show the same blue on
upward-facing panels: it is the showroom's blue light reflected in a
dark gloss, not a stripe colour. Black stripes are right for M1.

## Livery masks

`LiveryMasks/Masks.xml` lists six planes (Front, Back, Top, Left, Right,
Wing). Each valid plane is a box in one 1024 × 1024 sheet: `left`,
`right`, `top` and `bottom` are pixels from the sheet's centre, rows
counted from the bottom as the TGA stores them, and boxes never overlap.
A plane also has `xorigin`, `yorigin`, `xScale`, `yScale` and `rotation`
(Right has 180), probably the projection and its mirroring.
`LiveryMasks/<plane>.tga` (32-bit, RLE) is opaque white on the paint,
white with alpha 0 in the rest of the box and transparent black outside.
The paint's UV0 after its transform lands in the box of the view it faces
(+x Right, −x Left, +y Top, −z Front, +z Back), so the paint samples the
livery sheet at its own UVs (probable; one car checked, and a vinyl test
in game). `Data_Car.HasLivery`, `WingMask` and `CarExceptions`
(`NoPaintableWing` and others) gate the planes (guess). M1 needs the
masks only to show where a player livery could go; the factory
liveries below do not use them.

## Factory liveries

Fifteen cars reach the Autoshow in a factory livery (the HPD Fit's
Honda Racing scheme, the Forza Horizon art cars of the Limited
Collector's Edition, the Transit SuperSportVan's Ford scheme, the
Radical's green). The livery is not a file of its own: no car folder,
package or disc archive holds a livery or `Livery.bin` for them, and
the livery and vinyl tables have no per-car rows. It is painted into the
car's body atlases.

| Fact | Confidence | How |
| --- | --- | --- |
| A factory livery car is a `Data_Car` row with `HasRaceLivery` = 1: 15 rows, 15 distinct models (6 base, 9 DLC-only). `HasLivery` is 1 on 13 of them and on no other row; the Shelby GT500 LCE and the Raptor HALO have only `HasRaceLivery`, and the Shelby shows its livery, so `HasRaceLivery` is the gate | confirmed (rows), probable (gate) | merged database; M1 reference sheets of 11 of the 15 (not viewed: Zonda R, LCE Sport Quattro, Apollo Enraged, Viper ACR-X) |
| The livery is the RGB of the body atlas texels whose alpha is 0, the texels that take the paint on every other car; the alpha-255 texels (sponsor decals, number boxes) are as on any car. The LOD0 atlas and the LOD1-5 atlas carry the same scheme | confirmed (atlas content) | decoded atlases of the HPD Fit, the LCE Lancer Evo X and the LCE Scirocco R: the alpha-0 region holds the scheme seen in the game; on unflagged cars it is flat |
| The showroom shows the texel colour at every alpha and does not apply the paint, neither replacing nor tinting. Mean alpha-0 colour of the shown row's LOD0 atlas against its colour row, over the 15: the HPD Fit's row is red and its atlas mean is a pale pink-grey, and the hood shows the atlas's white; the LCE Scirocco's row is mid blue against a teal mean, and it shows the atlas's light blue and graffiti; both Raptors' rows are near black against a mid grey (LCE) and an olive (HALO) mean, and both references show the white skull and the camouflage, not a black truck; the DS3 Racing's row is grey against a black mean, and it shows black. On the Zonda R, the Agera (each of its atlases) and the Apollo the mean is within a few levels of the row, so these cars cannot separate the two readings | probable | atlas means against the colour rows; references; how the effect skips the paint term is not traced |
| Unflagged cars keep the paint even where their alpha-0 texels are coloured or dark (the Veyron Super Sport, the 250 GT): the first M1 build's paint matches their references | probable | references |
| 14 of the 15 have one colour row; the Agera has 7, each naming its own atlas (`nodamage_darkGrey` and others), so its colour choice is the atlas choice | confirmed | database |
| The executable reads both flags: a query for `HasLivery` by car id (its builder is in the content findings), and a `HasRaceLivery` query string with no reference the analysis resolved | probable (query), open (use) | strings; one cross-reference |

**Rule for M1.** For a row with `HasRaceLivery` = 1, draw the exterior
paint techniques with the slot-0 texel colour wherever its alpha is,
with no paint and no stripe tint; keep the finish (gloss) the engine
uses for paint. Every other row follows [Paint](#paint). Effect: the 15
cars' colours, not their silhouettes (the M1 IoU of these cars does not
depend on it; their PSNR does).

## Wheels

| Fact | Confidence | How |
| --- | --- | --- |
| `Data_Car.StockWheelID` is a `List_Wheels` row whose `MediaName` names `media/wheels/<WheelMedia>.zip` (or a package's `media/wheels/<WheelMedia>/`); 172 of the 176 base cars use their own media name | confirmed | database against the listings: all 176 base rows and all 250 merged rows resolve |
| A wheel archive holds `<WheelMedia>.carbin` (one part, `wheel`, with sub-sections for LOD0 to LOD5: `rim`, `inner_rim`, `outer_rim`, `wheel_black`, `blur_lip`, `blur_rim`), `wheel_LOD0.xds`, `wheel.xds` and `wheel0`-`wheel3.xds` | confirmed | listing; the showroom draws the LOD0 sub-sections |
| `List_Wheels` has 736 rows; 285 of their media are on the disc | confirmed | database and listing |
| Tyres are shared models `TireA`, `TireB`, `TireC` (sub-sections `sidewall`, `tread`), x from −0.5 to 0.5 and radius 0.194 to 0.287; the three share one geometry | confirmed (files) | parse |
| The game's files hold two per-car tyre sets beside the shared ones: `TireA`, `TireB`, `TireC` for the SVT Raptor and `TireA` only for the Wrangler. Of the 201 M1 references, 195 draw the shared `TireC`; the SVT Raptor, its LCE and HALO versions and the H1 Alpha Open Top draw the Raptor's `TireA`, and the Wrangler its own `TireA`; the FJ Cruiser's reference matched no tyre draw. The executable builds a tyre path from the wheels folder, `tire`, a letter (`a` for a level below 2, `b` for 2, `c` above) and, when a flag is set, a media name; what sets the flag and why the H1 takes the Raptor's name is not traced. Archives for the FJ Cruiser and the H1 Alpha exist only in the XE mod's files, not in the game's | confirmed (files, references), probable (path), open (flag) | listing; reference model lists; the tyre-path builder in the content findings |
| The stock XKR-S drew `TireC` with the `A` sidewall and tread textures; which of the three a car draws does not change its shape | confirmed (one car) | frame dump |
| `CarPartPositions` (`ID` = car id × 1000 + n, `Ordinal` = car id, `PartId`, `PartPosX`, `PartPosY`, `PartPosZ`) holds 10 rows a car; `PartId` 5 to 8 are the axle positions in the body frame, +Z forward like the lamp points: 5 left front, 6 right front, 7 right rear, 8 left rear, left at −x. In the model's frame z is negated | confirmed | 11 frame dumps, 44 wheels: the drawn centre's model z = −row z within 2.6 mm |
| Only the rows' z places the wheel: their x and y are not the drawn centre (up to 7.4 cm off in x on one car) | confirmed | the same dumps; see the counts under Tests |
| Rows 0 to 3 are lamp positions, rows 4 and 9 driver or steering points | guess | positions |
| The rim and tyre models are drawn once per wheel each of the three times the frame draws the car, as every car model is | probable | frame dump draw counts |
| The car model is posed in the body frame: z mirrored, lifted by (`Data_Car.FrontStockRideHeight` + `RearStockRideHeight`) / 2 − `Data_CarBody.BottomCenterWheelbasePosY` (row car id × 1000), and pitched by atan((rear − front ride height) / `ModelWheelbase`) | confirmed | model-to-body constants of 11 dumps |
| Each wheel draw carries its axle's rim radius = `WheelDiameterIN` × 0.0254 / 2 and tyre outer radius = rim radius + (tyre width in mm / 1000) × (aspect / 100) | confirmed | 15 cars, 30 axles |
| Wheel centre in the body frame: x = ±(`ModelFrontTrackOuter` or `ModelRearTrackOuter` − the axle's tyre width) / 2, y = the tyre's outer radius (the tyre stands on body y = 0), z = the row's z; mapped to the model frame by the inverse pose | confirmed | 15 cars: worst error 0.8 mm in x, 0.5 mm in y, 2.6 mm in z |
| Right wheels are the wheel and tyre models turned half a turn about y, not mirrored; left wheels are drawn as authored, so the rim's outboard face is −x. Front wheels have a small camber (about 0.5°) | confirmed (turn), probable (camber) | wheel transforms of the dumps |
| Rims are never scaled radially. A rim with a LOD0 morph target (see [Damage tail](#damage-tail)) is drawn as base + w × (target − base) with w = (D − 14) / 10 for a D-inch wheel; most are a 14-inch design whose target is the same rim at about 24 inches (radial scale 1.66-1.77, x unchanged) | confirmed | the weight constant on 15 cars (−0.2 at 12 inches to 0.6 at 20) |
| A rim without a morph target is drawn as authored; its outer radius is about D × 0.0127 + 0.019 m | confirmed (drawn), probable (radius) | census and dumps |
| Rim lateral scale = the axle's tyre width ÷ the rim's design width (header float at byte 36 − float at byte 44) | confirmed | exact on 15 cars |
| The tyre is drawn as p + wB × (B − p) + wA × (A − p): target B raises the inner radius (to 0.324 m) with the outer fixed, at wB = (D − 14) / 10, the rim's weight, so the inner edge meets the rim; target A raises the outer radius (to 0.4136 m) with the inner fixed, at wA = (outer radius − 0.2850) / 0.1301 | confirmed | 10 weight values fitted exactly; drawn outer radius within 1.2 mm of `Data_Car` |
| Tyre lateral scale = the rim's lateral scale × the rim's x extent (header float at byte 32 − float at byte 40); the tyre's centre is the rim's centre moved across by the rim's lateral scale × (byte 32 + byte 40) / 2 in the rim's own x | confirmed | exact on 10 cars |
| A tyre flattening term exists and is zero in the showroom | confirmed (zero) | every dump |
| `WheelNormOffsets` (111 rows), `WheelExceptions` (30 wheel ids), `CarTrackOffsets` (54 cars) and `List_Wheels.BrakeDrawOffset` | guess (meaning) | none applies to any measured car |

**Rule for M1.** Pose the car model as above. For each wheel take the
axle's `Data_Car` sizes (rim diameter D, tyre width, aspect) and place
it at the centre above (row z, track-rule x, y on the ground). Draw the
LOD0 rim with its morph target at (D − 14) / 10, or as authored when it
has none, scaled across by tyre width ÷ design width. Draw `TireC` (any
of the three) with its two targets at the weights above, scaled across
by rim scale × rim x extent and centred on the rim's x extent. Turn the
right-hand wheels half a turn about y. Log a car with `CarTrackOffsets`
rows, a rim whose header bounds are not finite, or no LOD0 rim pool.

The sizes are `Data_Car` columns: `FrontTireWidthMM`,
`FrontTireAspect` and `FrontWheelDiameterIN` for the front axle, and
`RearTireWidthMM`, `RearTireAspect` and `RearWheelDiameterIN` for the
rear (confirmed on 15 cars; the Autoshow shows stock wheels, so upgrade
tables do not apply).

## Brakes, calipers and rotors

| Fact | Confidence | How |
| --- | --- | --- |
| A car with calipers has its own caliper and rotor LOD0 models per wheel, about the hub (the rotor's box is centred on the origin); they sample the car's `nodamage_LOD0` (the rotor sits in the atlas's top left) | confirmed | parse; frame dump |
| On caliper draws a pixel-shader constant equals `Data_Car.CaliperRGBFront` / 255, not converted to linear | confirmed for one car (front and rear colours were equal) | frame dump |
| Rear calipers take `CaliperRGBRear`; `CaliperAngleFront`/`Rear` turn the caliper about the axle | guess | names |
| `media/brakes` holds sized replacements (sizes 10 to 17: calipers with 2, 4, 6 or 8 pistons with LOD0 twins, steel, drilled, slotted and carbon rotors, drums), chosen through `List_UpgradeBrakes` (`FrontBrakeSize`, `TypeID`, `CaliperPistons`) and `List_BrakeType.PartName` | probable | names and tables; upgrades are outside M1 |

## Lights, glass and shadow

- **Lamps.** Lamp parts are ordinary parts (`headlightL`, `taillightR`)
  drawn with lamp techniques; the lit state belongs to the effect. For
  M1 draw them unlit (lamps off, as in the showroom). The lamp points
  in the header place glows and are not needed.
- **Glass.** Draw the transparent lists after the opaque ones, back to
  front by part. `window` is tinted and see-through; the tint is the
  effect's (probable). Glass draws take no paint and no body-atlas
  texels: the Abarth 500's windscreen UVs land on red atlas texels at its
  top edge, which the game does not show (see [Paint](#paint)).
  Every LOD0 part with a transparent technique is drawn, including the
  rear windows of mid-engined cars, which sit flat over the engine deck
  between the roof and the wing: the F40 Competizione's `lexanR` (1,310
  LOD0 vertices) and the F50's `glassR` (2,239), both `window`. They are
  not engine-bay, variant or cockpit parts, and each has LOD1 to LOD5
  twins. In the Autoshow references the dark deck behind them reads 6 to
  15 levels (of 255) brighter where the window covers it, so the glass
  adds a faint reflection; an engine whose glass leaves the pixel
  unchanged loses the part. The F40's `trunk` part also carries louvres
  on an alpha-tested technique (`grille1_alpha`).
- **Shadow.** v4's `shaders/cars` has `dropshadow` and the car effect a
  `dropShadow` technique, and no shadow model is in the car files. The
  Autoshow frame dumps put no car geometry in a depth-only or shadow
  pass, and the darkening under the car has no established source
  ([AUTOSHOW_REFERENCE.md](AUTOSHOW_REFERENCE.md#parts-and-lighting)).
  The engine may draw its own contact shadow.

## Unknowns

| What | Impact on M1 | Rule meanwhile |
| --- | --- | --- |
| lamp-point bytes, the kind-5 records and the node block | none | skip by the draw-list scan; keep bytes for a write |
| the 28-, 4-, 12-, 5-, 36- and 56-byte fields of parts and sub-sections | none known | keep as bytes |
| `CarTrackOffsets`, `WheelNormOffsets` and `WheelExceptions` (no measured car has rows) and one rim whose header bounds are ±FLT_MAX | a few cars' wheels may sit off | the rule under [Wheels](#wheels); log the car |
| metallic finish; the stripe blend at intermediate alpha and the stripe constant of a car without an exception | colours right, finish approximate | flat paint; stripes as under [Paint](#paint) |
| back-face culling for glass and the cockpit | visual | cull opaque techniques, draw transparent ones two-sided |
| a lighter rectangle the engine draws on the Challenger SRT8 392's rear door and quarter, absent in the game | visual, one car | not a paint-rule case: the LOD0 and cockpit `body` and `body_2` triangles sample only transparent (paint) texels or the dark stripe texels of the atlas, none opaque white, so the patch comes from the engine's draw (another part or technique overlapping, or a UV or alpha read); check which part the engine draws there |
| livery decal and vinyl models | none (M1 shows no player liveries; factory liveries are atlas texels) | reject with a message |
| which cars draw a per-car tyre (the Raptor's for four rows, the Wrangler's), what sets the flag, and how the FJ Cruiser's tyres are sized | the FJ Cruiser's tyres too large (its M1 IoU), and the off-road cars' tyre shape | draw the Raptor's `TireA` for the four Raptor-tyre rows and the Wrangler's for the Wrangler, the shared tyre otherwise; a frame dump of the FJ Cruiser settles it |
| how the effect drops the paint on a `HasRaceLivery` car (a flag, a constant, another technique) | none for colours | the rule under [Factory liveries](#factory-liveries) |
| whether exhausts follow the stock rear bumper's letter or always take `a` | one non-reference row (`VW_IECorrado_95`) | letter `a` |
| whether the v4 or the disc copy of the car effect wins, and which package copy of a duplicated car loads | none (copies are identical for the official set) | disc first, then packages |
| a model whose draw-list scan (including the `n` = 0 form) finds no or several positions | none in the corpus | reject the model and name it |

## Tolerances

Parsing is exact: an implementation must reproduce every count below.
For drawing, compare against the Autoshow reference captures of
[AUTOSHOW_REFERENCE.md](AUTOSHOW_REFERENCE.md), each with the camera
from its own frame dump; the image metric and its threshold are not set
yet (open).

## Tests

Counts over the player's files; `tools/re/car-formats.py census`
prints each.

| Test | Base (176 cars) | DLC-only (64 cars) |
| --- | --- | --- |
| models (stripped twins excluded) | 1,883 | 704 |
| exterior / LOD0 / cockpit / caliper / rotor | 176 / 174 / 173 / 680 / 680 | 64 / 64 / 64 / 256 / 256 |
| models with no parts | 1 | 0 |
| models that write back byte for byte | 1,883 | 704 |
| parts | 14,917 | 6,074 |
| sub-sections | 118,629 | 53,211 |
| sub-sections by LOD 0 / 1 / 2 / 3 / 4 / 5 | 30,390 / 20,924 / 19,954 / 19,278 / 17,312 / 10,771 | 13,844 / 9,416 / 9,033 / 8,645 / 7,772 / 4,501 |
| triangle lists (type 4) | 11 | 7 |
| 32-bit index buffers | 28,359 | 13,008 |
| vertices in all pools | 35,193,432 | 16,461,551 |
| distinct techniques | 266 | 161 (284 over both) |
| models whose draw-list scan from the file's start finds exactly one position | all 1,883 | all 704 |
| models whose draw lists cover every sub-section once, each in its LOD's list | all 1,882 with parts | all 704 |
| `.xds` in car and wheel folders | 4,794 | 2,647 |
| tiled textures whose payload equals the layout size | 4,793 (one linear) | 2,647 |
| loose brake models that parse / empty | 118 / 2 | — |
| merged `Data_Car` rows (base, v4 and the 21 packages) with a car folder, a wheel folder, a colour row and wheel rows 5-8 | 250 of 250 (one table over both columns) | |
| stock wheel models with header kind 3 | 240 of 240 (over both columns) | |
| stock rims with a LOD0 morph target / without / with no LOD0 rim pool | 160 / 76 / 4 (over both) | |
| morph rims with a 14-inch design (base outer radius 0.190-0.196 m) | 140 (over both) | |
| rims with finite header bounds (design width 0.162-0.368 m) / of them at 0.225 m | 239 / 82 (over both) | |
| `CarPartPositions` wheel rows more than 1 cm off the track rule in x / in y / in z | 321 / 249 / 0 of 960, on 133 cars (over both) | |
| cars with `CarTrackOffsets` rows | 54 (over both) | |
| `Combo_Colors_Exceptions` rows / cars / with `HasStripe` 1 / with `HasColor2` 1 | 674 / 61 / 661 / 0 (merged table) | |
| merged first colour rows with a stripe exception (of them with stripe colour 0, black) | 21 of 250 (2) | |
| colour rows naming an atlas other than `nodamage.tga` / cars | 229 / 30 (merged table) | |
| merged rows whose stock rows name a letter other than `a` / with a stock roll cage / with a removed seat | 2 / 3 / 3 (of 250) | |
| merged rows whose drawn parts the stock rule changes against the first M1 build / of them only losing parts | 38 / 32 | |
| of those, M1 reference cars that passed / failed the numeric gate | 22 / 6 (all six kit failures) | |
| variant and seat parts in 16 Autoshow frame dumps / disagreeing with the stock rule / with the first build's rule | 165 / 0 / 3 | |
| rows with `HasRaceLivery` / `HasLivery` / both / distinct models with `HasRaceLivery` | 15 / 13 / 13 / 15 | |

Synthetic cases an implementation should also pass: a model with an
empty part (±FLT_MAX box, no vertices); a model with no parts; a 16-bit
strip with restarts and an odd number of triangles; a 32-bit strip with
the 0x00FFFFFF restart; a texture with a non-power-of-two size; a
texture 16 texels or smaller (all tail); a DLC folder whose members are
stored, deflated and LZX.

## Reproduce

```text
extract the disc's car, wheel and brake archives and the packages' offer ZIPs:
  out/build/win-amd64-release/pinyon_shift_fh1_archive_extract.exe --archive-all <zip> <dir>
  (then fill members of method 0 and 8 with Python's zipfile)
  .local/re/cars/base/<MediaName>/, .local/re/cars/wheels/<WheelMedia>/,
  .local/re/cars/dlc/<package>/<offer zip stem>/

python -I tools/re/car-formats.py census .local/re/cars [--brakes .local/game/base/media/brakes]
python -I tools/re/car-formats.py dlc-duplicates .local/re/cars
python -I tools/re/car-stock-parts.py census .local/re/cars .local/re/cars/db/merged.slt [--report <m1 report.txt>]
python -I tools/re/car-stock-parts.py dumps .local/re/cars .local/re/cars/db/merged.slt <dumps>/*.fh1frame
python -I tools/re/car-stock-parts.py liveries .local/re/cars/db/merged.slt
```

The stock-part census emulates the first M1 build's part choice
(`shift-engine` `apps/carview/showroom.cpp` at d26abc9) to count what
the rule changes; the frame dumps are the M1 reference dumps of
2026-11-20 (`.local/re/cars/dumps`).

The wheel, pose and stripe facts come from Autoshow frame dumps of 15
cars taken for the M1 references (2026-11-19). The other frame-dump
facts (texture slots, paint, caliper constants) come from `.local/ac0-dumps/stock.fh1frame`, an Autoshow frame of the
stock XKR-S, read with `tools/re/world-geometry.py`'s frame-dump reader.
