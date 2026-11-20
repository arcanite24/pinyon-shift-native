# Cars: models, textures, paint and wheels for the showroom

| | |
| --- | --- |
| Spec ID | `FORMAT-CARS` |
| Status | draft for M1: the files are complete (every model and texture parses and lays out); materials, paint, brakes and lamps are probable; the radial wheel fit, two-tone and stripe paint, the lamp and node blocks of the model header, and the livery decal models are open |
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
| kind | u32 | 5 for exterior, LOD0 and cockpit models; 1 for calipers, rotors and the loose brake models | confirmed | all 2,587 models and 120 brake files |
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
| transparent lists | `n` lists | list `k` holds the sub-sections drawn with blending at LOD `k` (LOD `k + 1` in an exterior model) | confirmed | every entry's sub-section LOD matches its list |
| opaque lists | `n` lists | the same for opaque sub-sections | confirmed | same |
| list | u32 count + count × u32 | each entry is (part index × 65536 + sub-section index) | confirmed | every entry names an existing sub-section, and every sub-section appears exactly once in the model's lists |
| end | u32 | 1, then the part count | confirmed | all models |

**Locating it.** Scan the header at 4-byte offsets for the words 4, 4,
`n` (1 ≤ `n` ≤ 8) whose 2`n` lists parse and end at a word 1 followed by
a part count whose parts parse. In the corpus this finds exactly one
position in every model and every header is a multiple of 4 bytes, so an
implementation can either scan or walk the lamp and node blocks once
they are decoded. A model with no parts (the empty F-350 LOD0 and two
brake files) has `n` = 0, the word 1, a part count of 0 and the footer.

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
Parts that do not deform have empty blocks. A showroom ignores the
words (probable layout; the per-vertex meaning is a guess).

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
  `vinyls/`): a different layout. They start with the word 3 (360 with
  1), a box as two (x, y, z, 0) vectors, the words 1, 0, 4, 4, 0, 1, 1, 2,
  the box again, a part named `decal` and 36-byte vertices; some carry an
  embedded texture. Not decoded and not needed for M1, which shows
  factory paint (guess for the framing).

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
- The levels whose shorter side is 16 texels or less share one packed
  tile at the end, at block offsets the published packed-mip rule gives
  (levels 1 and 2 of the tail along one axis, the rest along the
  other). A texture whose base level is that small is all tail (209
  textures): its one region takes its pitch and height from the size
  rounded up to powers of two, not from the header pitch.
- Inside a region, block (x, y) is at the console's standard 2D tiled
  offset. Each block is byte-swapped by the endian mode.

Confidence: confirmed. The payload of each of the 7,440 tiled textures
equals the layout's size exactly; 698 base body atlases and 154 Autoshow
pictures decode and re-encode byte for byte (accar branch
`tools/fh1_xds.py`). DXN and DXT5A are size-checked only; their blocks
are the standard two-channel and one-channel BC formats.

## Materials

The technique name is the material. There are 284 techniques in the 240
cars (266 in the base cars). The most frequent: `black`, `body`,
`chrome`, `frame`, `plastic2`, `window`, `matte_colors`, `misc`, `badge`.
`CustomCarShaders.xml` maps 145 names to effect techniques, and a car's
`ShaderSettings.xml` overrides parameters for that car (probable).

**Transparent techniques** (from the draw lists; confirmed: each
technique is in only one kind of list in every model): `window`,
`window_2`, `frontwindow`, `glass`, `glass_2`, the `glass*_LOD<n>`
names, `detail_glass_clear`, `detail_glass_red`, `detail_glass_amber`,
`lights_glass`, `light_glass`, `red_glass`, `blur_rim` and
`chrome_blur_rim`. Every other technique is opaque.

**Texture slots** (pixel-shader fetch slots in the showroom frame;
confirmed for one car, the stock XKR-S, by matching each slot's texture
to the archive member with the same bytes):

| Techniques | Slot 0 | Slot 1 | Slots 2 and up |
| --- | --- | --- | --- |
| exterior paint and trim: `body`, `body_2`, `rubber_trim` | `nodamage_LOD0` | — | `dirt_pattern`, `dirt_mask` |
| `black`, `frame`, `plastic2`, `matte_colors`, `emblem`, `bottom`, `weatherstrip`, `misc`, `chrome` (cockpit) | `nodamage_LOD0` | `dirt_mask` | |
| `undercarriage` | `nodamage_LOD0` | `dirt_mask` | `undercarriage` (Shared) |
| `carbon_fiber` | `nodamage_LOD0` | `carbonFiber` | `carbonFiberNormal` (Shared) |
| `bumper_frame`, `grille1` | `nodamage_LOD0` | `bumper_frame`, `grille1_alpha` (Shared) | |
| lamps: `taillight2S`, `taillightst`, `tail_light_lod0`, `reverse_light_lod0`, `indicator_left`, `indicator_right`, `xenonhead`, `slorange`, `textured_reflector`, `numplate`, `drlwhite` | `nodamage_LOD0` | `lights_LOD0` | |
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

## Paint

| Fact | Confidence | How |
| --- | --- | --- |
| A car's factory colours are its `Combo_Colors` rows (1 to 31 a car, 1,281 rows): `ID` = car id × 1000 + n, `Sequence`, `Metallic` (0 or 1), `RGB` = r × 65536 + g × 256 + b, `BaseTexture`, `DamageTexture` | confirmed | database; a clone given seven rows showed seven swatches |
| The showroom shows the row with the lowest `Sequence` | probable | three cars (other agent's M1 pilot; record "Autoshow showroom colour") |
| On exterior techniques a pixel-shader constant equals that row's RGB / 255 converted from sRGB to linear; it is 0 on other techniques | confirmed for one car | frame dump of the stock XKR-S |
| The paint shows where the atlas alpha is 0; alpha 255 shows the texel (badges, engine bay) | probable | atlas content and the body technique's alpha census |
| `BaseTexture` names the clean atlas to use with that colour (`nodamage*.tga` in the table, shipped as `.xds`); `DamageTexture` is always `damage.tga` | confirmed (names), probable (use) | every name resolves to a member of the car's archive |
| `Metallic` picks the metallic finish; `List_ColorFinish` names 0 Normal, 1 Metallic, 2 CarbonFiber | probable | names |
| Two-tone and stripes: `Combo_Colors_Exceptions` (`Color2`, `Stripe`), `StripeColors`, `List_SpecialColors` (102 rows of two-tone scale, bias and power) and effect parameters `psPaintTwoTone*` and `StripeColor` | guess | names only |

**Rule for M1.** Paint = the lowest-`Sequence` row's RGB, each channel
/ 255, converted from sRGB to linear, applied where the slot-0 texel's
alpha is 0 on exterior techniques; texel colour elsewhere. Metallic
cars use a metallic finish of the engine's choosing until the effect's
paint terms are specified (log the car). Two-tone and stripes are not
drawn (log).

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
masks only to show where a livery could go; factory cars have none.

## Wheels

| Fact | Confidence | How |
| --- | --- | --- |
| `Data_Car.StockWheelID` is a `List_Wheels` row whose `MediaName` names `media/wheels/<WheelMedia>.zip` (or a package's `media/wheels/<WheelMedia>/`); 172 of the 176 base cars use their own media name | confirmed | database against the listings, all 176 cars |
| A wheel archive holds `<WheelMedia>.carbin` (one part, `wheel`, with sub-sections for LOD0 to LOD5: `rim`, `inner_rim`, `outer_rim`, `wheel_black`, `blur_lip`, `blur_rim`), `wheel_LOD0.xds`, `wheel.xds` and `wheel0`-`wheel3.xds` | confirmed | listing; the showroom draws the LOD0 sub-sections |
| `List_Wheels` has 736 rows; 285 of their media are on the disc | confirmed | database and listing |
| Tyres are shared models `TireA`, `TireB`, `TireC` (sub-sections `sidewall`, `tread`), x from −0.5 to 0.5 and radius 0.194 to 0.287; the Raptor and Wrangler have their own variants | confirmed (files) | parse |
| Which tyre model a car uses is not known; the stock XKR-S drew `TireC` with the `A` sidewall and tread textures | open | frame dump |
| `CarPartPositions` holds 10 rows a car; rows 5 to 8 are the wheel centres in car space (5 left rear, 6 right rear, 7 right front, 8 left front) | confirmed | their spacing equals the wheelbase, their height the tyre radius less the stock ride height within a few cm, on 176 cars |
| Rows 0 to 3 are lamp positions, rows 4 and 9 driver or steering points | guess | positions |
| The rim and tyre models are drawn once per wheel in each of the showroom's three passes, as every car model is; left wheels are the same model mirrored | probable (count), guess (mirroring) | frame dump draw counts |
| Each wheel draw carries a constant with the rim radius = `WheelDiameterIN` × 0.0254 / 2 and the tyre's outer radius = rim radius + (tyre width in mm / 1000) × (aspect / 100), per axle | confirmed for one car | frame dump against `Data_Car` front and rear tyre sizes |
| Lateral scale: the tyre's is its width in metres × 1.033; the rim's is the axle's tyre width ÷ the front tyre width | confirmed for one car | frame dump constants |
| Rim models are normalised: 95 of the 176 stock rims have an outer radius of 0.190 to 0.196, the rest from 0.02 to 0.42 | confirmed (census) | parse |
| The radial fit happens in the vertex shader from the constants above | probable | the model bytes are drawn unchanged |
| `WheelNormOffsets` (111 rows, a small offset a wheel) and `WheelExceptions` (30 wheel ids), and `List_Wheels.BrakeDrawOffset` | guess (meaning) | names |

**Rule for M1 (guess, to be checked against reference captures).**
Place each wheel at its `CarPartPositions` centre. Scale the tyre's
x by its width × 1.033 and map its radius linearly from the model's
inner (0.194) and outer (0.287) radius to the rim radius and the outer
radius. Scale the rim's x by the axle's tyre width ÷ the front width and
its radius by the rim radius ÷ 0.194. Use `TireC` and log the car until
the tyre choice is known.

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
  front by part. `window` is tinted and see-through; in the showroom the
  glass texels sampled are white with alpha 0, so the tint is the
  effect's (probable).
- **Shadow.** v4's `shaders/cars` has `dropshadow` and the car effect a
  `dropShadow` technique; no shadow model was found in the car files
  (probable: the shadow is drawn from the car's own models or a generic
  quad). The engine may draw its own contact shadow.

## Unknowns

| What | Impact on M1 | Rule meanwhile |
| --- | --- | --- |
| lamp-point bytes, the kind-5 records and the node block | none | skip by the draw-list scan; keep bytes for a write |
| the 28-, 4-, 12-, 5-, 36- and 56-byte fields of parts and sub-sections | none known | keep as bytes |
| the radial wheel fit and which tyre model a car uses | wheels may look the wrong size | the rule under [Wheels](#wheels); log cars whose rim radius ratio is outside 0.9-1.1 |
| metallic, two-tone and stripe paint terms | colours right, finish approximate | flat paint; log two-tone and stripe cars |
| back-face culling for glass and the cockpit | visual | cull opaque techniques, draw transparent ones two-sided |
| livery decal and vinyl models | none (no liveries in M1) | reject with a message |
| whether the v4 or the disc copy of the car effect wins, and which package copy of a duplicated car loads | none (copies are identical for the official set) | disc first, then packages |
| a model whose draw-list scan finds no or several positions | none in the corpus | reject the model and name it |

## Tolerances

Parsing is exact: an implementation must reproduce every count below.
For drawing, compare against the Autoshow reference captures by image
metric (the other agent's `docs/formats/AUTOSHOW_REFERENCE.md` sets the
metric and the views).

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
| models whose draw-list scan finds exactly one position, covering every sub-section once | all 1,882 with parts | all 704 |
| `.xds` in car and wheel folders | 4,794 | 2,647 |
| tiled textures whose payload equals the layout size | 4,793 (one linear) | 2,647 |
| loose brake models that parse / empty | 118 / 2 | — |

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
```

The frame-dump facts (texture slots, paint, caliper and wheel constants)
come from `.local/ac0-dumps/stock.fh1frame`, an Autoshow frame of the
stock XKR-S, read with `tools/re/world-geometry.py`'s frame-dump reader.
