# World materials: Colorado vertices, materials and textures

Status: specification role, 2026-11-20. Our own reading of the player's
files and of the RI-0.4 free-roam frame dump; no disassembly, decompiled
code or game data is reproduced here ([LEGAL.md](../LEGAL.md)). It
continues [WORLD_GEOMETRY.md](WORLD_GEOMETRY.md), which covers the
render models' positions, index lists and placements. The reader is
[`tools/re/world-materials.py`](../../tools/re/world-materials.py).

## Summary

What an engine can draw from this page: every Colorado render model
with its own textures. Each step below is checked over all 14,291 render
models of `bin.zip` and against the draws of one free-roam frame.

- **Vertex.** A mesh object's vertex is its effect's vertex declaration
  packed in order: position three f32, then 4 bytes for each normal,
  texture coordinate, tangent or colour. That gives the stride in all
  48,052 submeshes that have a material. In the frame, the vertex
  fetches of all 667 colour draws whose shader the dump kept read
  exactly those offsets and formats.
- **UV.** Texture coordinates are 16-bit fractions scaled per submesh by
  one constant (`uvOffsetScale`, vertex constant c0) stored in the
  model's command section: UV = offset + scale × raw / 65535. The model
  holds one per submesh for 14,193 of 14,291 models. In the frame, the
  game set exactly that constant in 759 of 761 colour draws.
- **Material.** A model lists its materials after the meshes: an effect
  (shader) path, vertex and pixel constants, and one value per texture
  slot. The value indexes the model's texture list in the ribbon's
  `Colorado_00.pvs`, which names a texture record, whose ID is the
  texture's file name `_0x<ID>`. In the frame, 1,678 of the 2,102
  sampled slots that list a texture with a file are bound to that
  texture's bytes; nearly all the others are bound at a streaming
  size (below).
- **Texture.** `_0x<ID>.bix` (header and mip chain) with `_0x<ID>_B.bix`
  (the base level), or a CAFF `_0x<ID>.bin`, hold DXT1, DXT3, DXT5 or DXN
  blocks in the console's tiled layout: the `.xds` rules of
  [CARS.md](CARS.md) with one change to the size of the last region.
  Every one of the 5,234 `.bix` pairs and 2,962 of the 2,965 CAFF
  textures fits the layout to the byte, and decoded levels are
  recognisable pictures whose mips agree.
- **Slot roles.** The effect's sampler names give each slot's role.
  Slot 0 is the main diffuse (`Diffuse_Texture` or `Blend_A`) in every
  effect but the lake; `Light_Map` is always slot 7.

- **First pass.** [Minimal recipe](#minimal-recipe-first-pass) gives
  a drawable shading rule (albedo × AO × sun and sky, the frame's blend
  state per family) and [Reference frame](#reference-frame) a capture
  with its matrix to compare against.

Open: the material constants' meaning beyond names, what the 98 models
without a UV constant use, the game's exact shading formulas, and what
4x4 placeholders the record types 1 and 3 stand for.

## Vertex layout

### The rule

Each material names an effect (`shaders\track\<name>.fx`; the compiled
effect is `shaders/track/<NAME>.fxobj` in `bin.zip`). Each vertex
shader in the effect carries a vertex declaration: a list of
(usage, usage index) pairs. The vertex is that declaration packed in
its stored order, with these sizes and formats:

| Usage | Size | Format | Decode |
| --- | --- | --- | --- |
| POSITION | 12 | three big-endian f32 | x, y, z in the model's space (WORLD_GEOMETRY) |
| NORMAL | 4 | 2_10_10_10, signed normalised | one big-endian u32: x bits 0-9, y bits 10-19, z bits 20-29, each a two's-complement 10-bit value / 511; bits 30-31 unused |
| TANGENT | 4 | 2_10_10_10, signed normalised | as NORMAL |
| TEXCOORD n | 4 | 16_16, unsigned normalised | two big-endian u16, u first, then v; dequantise with the submesh's `uvOffsetScale` (below) |
| COLOR | 4 | 8_8_8_8, unsigned normalised | four bytes, in memory order A, R, G, B; the shader's COLOR is (R, G, B, A) = (stored byte 1, byte 2, byte 3, byte 0) / 255 (see [Colour byte order](#colour-byte-order)) |

Pick the declaration whose packed size equals the object's stride
(effects also hold position-only shaders for depth and shadow passes,
which are not used for the layout). Confidence: confirmed.

- Over all 48,052 submeshes with a material, exactly one packed size of
  the effect's declarations equals the stride every time.
- In the frame, 667 colour-pass draws (draws whose pixel shader is one
  of the material's effect) kept their vertex shader in the dump, and
  every vertex fetch they make from the model's buffer reads the
  offset and format the packed declaration gives. A shader may skip an
  element (84 draws of stride 20 read only position and the texture
  coordinate at 16); the offsets do not move.
- Normal x in the low bits: the decoded normal agrees with the face
  normal of the submesh's triangles to a mean |cos| of 0.98 to 1.0 on
  six sampled road, terrain and building submeshes (a one-off probe, not
  in the reader), against 0.76 to 0.99 with the components reversed. u first: the last texture coordinate of
  lightmapped effects lands in [0, 1] (below) only read that way.

The common layouts per stride (offsets in bytes):

| Stride | Layout | Effects (submeshes) |
| --- | --- | --- |
| 16 | position 0, TEXCOORD0 12 | trees' leaf cards (`TREE_DIFF_OPAC_DIRLIGHT*`; 74), 7 others |
| 20 | position 0, NORMAL 12, TEXCOORD0 16 | buildings and props (`H_DIFF_*`; 8,193), 80 others |
| 24 | position 0, NORMAL 12, TEXCOORD0 16, TEXCOORD1 20 | `H_DIFF_*` with AO or lightmap (6,958), flagpoles (72) |
| 28 | position 0, NORMAL 12, TEXCOORD0 16, TEXCOORD1 20, COLOR 24 | terrain blends `H_BLND1/2/2B/3` (8,449), `H_VBLND` (1,634), 125 others |
| 32 | position 0, NORMAL 12, TEXCOORD0 16, TEXCOORD1 20, TEXCOORD2 24, COLOR 28 | roads `H_ROAD` (5,143), `H_VBLND` (6,088), `H_BLND3` (2,262) |
| 36 | position 0, NORMAL 12, TEXCOORD0 16, TEXCOORD1 20, TEXCOORD2 24, TANGENT 28, COLOR 32 | roads with a normal map (`H_ROAD_*NORM*`; 8,374), `H_VBLND` (378) |

The packed rule, not this table, decides: some effects order their
elements differently at the same stride.

| Effect | Stride | Packed layout |
| --- | --- | --- |
| `ADD_DIFF_OPAC_RGBA` (30 submeshes) | 24 | position, COLOR 12, TEXCOORD0 16, TEXCOORD1 20 |
| `BUNTING_*` (30 at stride 24) | 24 | position, NORMAL 12, TEXCOORD0 16, COLOR 20 |
| `TREE_DIFF_OPAC_DIRLIGHT_2_2SD` (7 at stride 24) | 24 | position, TEXCOORD0 12, NORMAL 16, COLOR 20 |
| `ANIM_FLAG_*` (75 at stride 32) | 32 | position, TEXCOORD0 12, COLOR 16, TANGENT 20, TEXCOORD1 24, TEXCOORD2 28 |
| `LAKE_*` (34) | 32 | position, NORMAL 12, TEXCOORD2 16, COLOR 20, TEXCOORD0 24, TANGENT 28 |
| `DIFF_GLOW_1` (1) | 28 | position, NORMAL 12, TEXCOORD0 16, TEXCOORD2 20, COLOR 24 |

Reading the declaration needs the effect container's vertex shader
metadata; [the shader corpus tool](../../tools/extract-fh1-shader-corpus.py)
documents that layout. An engine that does not want to read `.fxobj`
files can use the stride table plus the exceptions above, keyed by
effect name.

### Colour byte order

Engine question Q38, settled 2026-11-20 (confirmed for the frame). A
colour element's four stored bytes are A, R, G, B, and every world
shader sees them as COLOR = (R, G, B, A), that is COLOR.x is the
**second** stored byte and COLOR.w the **first**:

- The console reads an 8_8_8_8 element as one 32-bit word with the
  byte swap of its fetch constant; all 1,773 8_8_8_8 vertex fetches of the frame
  use the 8-in-32 swap, so the element's x is the last stored byte, y
  the third, z the second and w the first.
- All 493 colour fetches among the 1,367 tied world draws (every `H_VBLND`,
  `H_BLND*` and `H_ROAD` family the frame drew) put the fetched
  components into the shader register in the order z, y, x, w, which
  makes the register (byte 1, byte 2, byte 3, byte 0).
- The data agree: on the frame's `H_VBLND` submeshes byte 0 stays
  between 152 and 255 (5 distinct values), while byte 1, the terrain
  layer weight, spans 0 to 255.

Which component a family uses:

| Family | Uses | Stored byte |
| --- | --- | --- |
| `H_VBLND` (not `DECAL`) | layer weight from COLOR.x | 1 (R) |
| `H_VBLND_DECAL_*` | opacity from COLOR.w; its RGB is multiplied by a constant that is 0 in the frame | 0 (A) |

So an engine that found the decals right only with the first stored
byte was right about the byte; the recipe below now names it as
COLOR.w.

### UV dequantisation: `uvOffsetScale`

All texture coordinates of a submesh are dequantised with one
four-float constant, (offset u, offset v, scale u, scale v):

```text
u = offset_u + scale_u × raw_u / 65535
v = offset_v + scale_v × raw_v / 65535
```

The vertex shaders name vertex constant c0 `uvOffsetScale`; c1 is
`positionScale` (1, 1, 1, 1 in every draw seen) and c2
`positionOffset` (zero), so positions need no dequantising.

**Where it is stored.** After the shader path list (below) a model holds
a command section: pre-built GPU register writes for its draws. Among
them is a write of 16 or 32 dwords to the vertex constants from c0,
whose first four floats are the `uvOffsetScale` and whose next four are
(1, 1, 1, 1). These writes come one per submesh, in file order (object
by object, submesh by submesh), and the whole run repeats once per
render pass with the same values; take the first n for n submeshes.
The PM4 packet framing is the console's public one; the exact header
words are in `site/findings` and the reader (`uv_constants`).

| Measure | Count |
| --- | --- |
| Models with at least one write per submesh | 14,193 of 14,291 |
| Models with no such write at all | 98: 79 `H_ROAD` decal models (`MT_Area02_Decals*` and similar), 18 tree models, 1 other |
| Colour-pass draws of the frame whose c0 equals the submesh's value | 759 of 761 |

Confidence: confirmed for the dequantisation and for the per-submesh
value (the frame); probable for the "first n in file order" reading
(the two misses are one road model, `coloradoout.09271`, where two of
its five submeshes take each other's value, so the order is not always
file order). For the 98 models without a write the value is open;
offset (0, 0) and scale (1, 1) is the engine's choice.

**Lightmap coordinates.** In effects whose name ends in `_LM`, the last
texture coordinate of the declaration (TEXCOORD1, or TEXCOORD2 on
roads) lands in [0, 1] after dequantisation for 20,352 of 22,432
submeshes (all vertices, within 0.01, up to 500 vertices a submesh).
That is the lightmap's (and the per-model AO map's) coordinate:
probable. TEXCOORD0 is the tiling surface coordinate (road and terrain
values run to the hundreds).

## Materials

### Material records

After the last mesh object (WORLD_GEOMETRY.md, "After the objects"), all
values big-endian:

| Field | Size | Meaning | Confidence |
| --- | --- | --- | --- |
| tags | 4 × u32 | 1, 1, 1, 1 | constant (all models) |
| material count | u32 | | confirmed |
| materials | | see below, one per material | confirmed |
| tag | u32 | 1 | constant |
| shader count | u32 | | confirmed |
| shader paths | count × (u32 length, bytes) | `shaders\track\h_road_diff3_norm1_noise_ao_lm.fx` and similar; strip trailing NULs | confirmed |
| command section | to the end | pre-built register writes for the draws (holds `uvOffsetScale`, above) | partly decoded |

One material:

| Field | Size | Meaning | Confidence |
| --- | --- | --- | --- |
| tag | u32 | 3 | constant |
| effect | u32 | index into the shader path list | confirmed: in the frame every colour draw's pixel shader is one of that effect's |
| zero | u32 | 0 | constant |
| tag, n | 2 × u32 | 1, number of vertex constants | confirmed |
| vertex constants | n × 4 × f32 | effect parameters for the vertex shader (n is 0 in most materials) | probable |
| tag, n | 2 × u32 | 1, number of pixel constants | confirmed |
| pixel constants | n × 4 × f32 | effect parameters: in the frame they are the pixel shader's float constants from its first material parameter on, in order | confirmed (values), meaning per effect not decoded |
| tag, n | 2 × u32 | 1, number of texture slots (8) | confirmed |
| texture slots | n × i32 | per fetch slot 0..n−1: an index into the model's texture list, or −2 (no texture; 110,038 slots) or −1 (bound at run time; 2,178) | confirmed |

A submesh's material is its header's material index (WORLD_GEOMETRY.md).
Counts: all 14,291 models parse; 47,749 materials for 48,053 submeshes
(230 models share a material between submeshes). One submesh, in
`coloradoout.10531`, has material index all ones and the material name
`ErrorShader`: skip it.

Pixel constants seen on terrain and roads look like per-layer UV tiling
and tints: (1, 1, 1, 1), (4, 4, 2, 2), (1.1, 1.1, 1, 1), (5, 3, 1.5,
0.5). Their names come from the effect's constant table (for example
`Blend_A` tiling); this page does not decode them, and the minimal
recipe below does not need them.

### From a texture slot to a file

`Ribbon_00/Colorado_00.pvs` holds, besides the object table
(WORLD_GEOMETRY.md), two tables that link materials to files:

**Texture records.** At offset 0xAF (175) a u32 count, 19,613, then that
many 28-byte records, unaligned:

| Offset | Size | Field | Confidence |
| --- | --- | --- | --- |
| 0 | u32 | texture ID: the file is `_0x%08X.bix` (with `_0x%08X_B.bix`) or `_0x%08X.bin`, upper-case hex | confirmed |
| 4 | u32 | record index (the value the per-model lists hold) | confirmed |
| 8 | 2 × f32 | 1.0, 1.0 | constant |
| 16 | 10 bytes | zero | constant |
| 26 | u8 | flag: 1, or 0 for 75 records | probable (see below) |
| 27 | u8 | type: 4 a `.bix` pair (5,234), 0 a CAFF `.bin` (2,965), 1 no file (11,409), 3 no file (5) | confirmed (the counts match the files one to one) |

**Per-model texture lists.** After the object table: a u32 count, 14,560,
then three u32 (2, 0, 1), then 14,560 lists. List j belongs to render
model member j + 1 (`coloradoout.%05d` with that number):

| Field | Size | Meaning | Confidence |
| --- | --- | --- | --- |
| k | u32 | | confirmed |
| effects | k × u32 | indices into the `.pvs` shader name list (0-172): the model's effects | probable |
| box | 15 × f32 | an oriented box: a 3×3 rotation, a centre, half extents | probable |
| t | u32 | | confirmed |
| textures | t × u32 | texture record indices; a material's slot value i means entry i | confirmed |

The lists end at a fixed point followed by data not decoded. Earlier
these lists were read as "clusters" (WORLD_GEOMETRY.md, Ruled out);
they are per-model resource lists.

So: slot s of a material holds value i ≥ 0 → entry i of the model's
list → texture record → texture ID → file.

| Slot values over all materials | Count |
| --- | --- |
| a record of type 4 whose `.bix` pair exists | 161,392 |
| a record of type 0 whose `.bin` exists | 27,537 |
| a record of type 1 (no file) | 13,232 |
| a record of type 3 (no file) | 214 |
| −2, no texture | 110,038 |
| −1, set at run time | 2,178 |
| past the end of the model's list | 2 |

**Checked against the frame.** For every texture slot a colour-pass draw
samples (from the pixel shader's texture fetches):

| Outcome | Slots |
| --- | --- |
| bound at the listed texture's size, bytes equal to its file | 1,479 |
| bound at the listed texture's size, both all zero (lightmaps, slot 7) | 90 |
| bound at half size from the listed texture's mip chain alone, bytes equal (the base level not loaded yet) | 95, plus 14 all-zero lightmaps |
| bound at a small size (16 × 16 for most, down to 8 × 1), not identified: the listed texture's low levels while it streams in (probable, by size) | 414 |
| one of the 3 CAFF textures not decoded (below), bound at 256 × 256 | 6 |
| record type 1 or 3: a 4 × 4 texture is bound | 22 |
| animated flags: another texture of the flag set is bound | 4 |
| list value −1: a runtime texture (4 × 4 to 256 × 256) | 55 |
| slots 8 and up (shadow map, shadow mask; not in the material) | 633 |

Apart from the animated flags, no slot shows another texture in full
than the listed one. Confidence: confirmed. Flag-0 records: in an earlier pass with
a different matching rule some flag-0 records were bound to another
texture with the same content; the engine can ignore the flag.

### Slot roles

The effect's pixel shader constant table names its samplers. Over the
173 track effects:

- slot 0 is `Diffuse_Texture` (buildings, props, trees, flags) or
  `Blend_A` (roads and terrain), in every effect except `LAKE_*` (a
  cube map, normal maps A and B) and one effect with no sampler;
- slot 7 is `Light_Map` whenever the effect has one;
- `AO` (roads, terrain) sits in slot 4, 5 or 6 and `AOMap` (props) in
  slot 5, depending on the effect; `Splat` (terrain blend weights) is
  slot 2 on `H_BLND*`, where roads have `Noise`;
- `NormalMap` is slot 1 on `H_DIFF_*NORM*` and slot 3 on roads and
  terrain; DXN normal maps hold x and y only.

`world-materials.py samplers` lists every layout (82 distinct slot sets,
and one effect without samplers).
Slot uses with a file, by sampler (all materials):

| Sampler | Slot uses | Note |
| --- | --- | --- |
| `Blend_A`, `Blend_B`, `Blend_C` | 32,156, 28,957, 17,967 | road and terrain layers, DXT1 sRGB |
| `Diffuse_Texture` | 14,557 | DXT1 or DXT5 sRGB |
| `AO` / `AOMap` | 22,718 / 4,188 | per-model, DXT1 linear, mostly CAFF `.bin` |
| `Light_Map` | 17,386 | per-model, DXT1 sRGB; **9,661 of the uses are all-zero textures** |
| `Noise`, `Modulate`, `Splat` | 12,713, 9,451, 10,606 | |
| `NormalMap` (A, B) | 9,409 | DXN |
| `SpecularMap` | 7,419 | |

## Texture payloads

### The format word

Both containers carry the texture's Direct3D format word (the console's
`D3DFORMAT`). Its fields, by the console's published fetch layout:

| Bits | Field | Values seen |
| --- | --- | --- |
| 0-5 | texel format | 18 DXT1 (BC1), 19 DXT2/3 (BC2), 20 DXT4/5 (BC3), 49 DXN (BC5, two channels) |
| 6-7 | endian | 1: swap the bytes of each 16-bit word |
| 8 | tiled | 1 in every texture |
| 9-14 | sign of x, y, z (two bits each) | 0 linear, or 3 on all three: gamma (sample as sRGB) |

| Container | Format | Gamma | Linear |
| --- | --- | --- | --- |
| `.bix` | DXT1 | 3,717 | 1,369 |
| `.bix` | DXT5 | 92 | 13 |
| `.bix` | DXT3 | 2 | 0 |
| `.bix` | DXN | 0 | 41 |
| CAFF `.bin` | DXT1 | 778 | 2,175 |
| CAFF `.bin` | DXT5 | 9 | 0 |

The gamma reading is probable: colour textures (diffuse, layers,
lightmaps) carry it, data textures (normal maps, AO, noise, splat
weights, specular) do not. The console's gamma curve is a piecewise
approximation of 2.2; sRGB is close enough for a first pass.

### `.bix` and its `_B` twin

`_0x<ID>.bix`, big-endian header of 28 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 bytes | `BIX1` |
| 4 | u32 | width |
| 8 | u32 | height |
| 12 | u32 | level count (base included) |
| 16 | u32 | format word (above) |
| 20 | u32 | total payload size (base + chain) |
| 24 | u32 | base level size |

After the header: the mip chain (levels 1 and up, then the packed tail),
total − base bytes. `_0x<ID>_B.bix` is the base level alone, no header,
exactly the base size. Confidence: confirmed (all 5,234: file sizes
equal the header's, and the layout below gives both sizes).

The game streams the two halves separately: in the frame, 109 slots
were bound at half size from the `.bix` chain alone (its level 1 is the
first thing in the file), while the `_B` base was not yet resident.

### CAFF `.bin`

`_0x<ID>.bin` starts with `CAFF` and a version string; a `.data`
section at 0x190 (0x120 bytes) holds the type string `texture`, its own
version string, then the format word (u32 at 0x1A8), width and height
(u16 each at 0x1B4 and 0x1B6) and the level count (u8 at 0x1C0); the
texels (`.gpu`) run from 0x2B0 (688) to the end of the file, base level
first. Confidence: confirmed for the 2,962 textures whose payload equals
the layout; 3 (`_0x00000D9D`, `_0x00001A09`, `_0x00002A9D`, 256 × 256,
one level) have a 196,604-byte payload that does not fit and are not
decoded.

### Layout and untiling

The `.xds` rules of [CARS.md](CARS.md) ("`.xds`: textures") apply, with
these differences:

- Every `.bix` texture is a power of two on each side (8 to 1,024).
  Twelve CAFF textures are not: ten are 768 wide (768 × 256 or
  768 × 128), `_0x000010F2` is 255 × 256 and `_0x00004578` is 7 × 256.
  These are the stored sizes, not misreads: the level counts (10 for
  the 768-wide ones, 9 for the other two) are those of the stated
  largest side, the payloads fit the layout with a pitch of whole
  32-block tiles, and `_0x000010F2` decodes to a coherent picture with
  its last column. Decode ceil(width / 4) blocks per row and crop. All
  have the packed-tail layout; the header pitch is not stored: use the
  width.
- **The last region is trimmed.** Each level up to the tail gets a
  region of whole 32 × 32-block tiles rounded to 4,096 bytes, as for
  `.xds`; but the last region (the packed tail, or the last level when
  the chain stops before the tail) is only as large as the tiled bytes
  its levels touch, rounded up to 4,096. A square DXT1 or DXT5 tail is
  4,096 bytes, a 2:1 DXT5 tail 8,192.
- A `.bix` base level is the `_B` file, whatever its size: a base under
  32 blocks tall can be smaller than a full tile row (a 128 × 32 DXT1
  base is 4,096 bytes).
- A CAFF texture whose levels are all in the tail is one full region.

Each 8- or 16-byte block sits at the console's standard 2D tiled offset
inside its region (`tiled_offset` in the reader, as in the `.xds`
decoder) and has the 16-bit byte swap of its endian field. After
untiling and swapping, the blocks are ordinary BC1, BC2, BC3 and BC5
blocks: an engine can upload them as such (or decode them with the
`.xds` decoder's block code, which is the same).

Confidence: confirmed for the sizes (5,234 of 5,234 `.bix`, 2,962 CAFF)
and for decoding: road, normal (DXN), AO, lightmap and building
textures decode to plausible images, and levels 1, 5 (in the tail), 6
and 8 of a 512 × 512 DXT1 average to the same colour as the base
(within 4 of 255 per channel). The texels the frame bound are byte for
byte the files' (above), so the files need no conversion beyond this.

## Minimal recipe (first pass)

A recipe to draw Colorado recognisably, not the game's shading. Each
step says how sure it is; "engine's choice" marks where the game's rule
is not known and any reasonable choice will do for now.

1. **Geometry and UVs.** Decode the vertex with the packed declaration;
   dequantise every TEXCOORD with the submesh's `uvOffsetScale`
   (confirmed). Call TEXCOORD0 the surface UV and the last TEXCOORD of
   an `_LM` or `_AO` effect the lightmap UV (probable).
2. **Albedo.** Sample slot 0 (`Diffuse_Texture` or `Blend_A`) at the
   surface UV, as sRGB when its format word says gamma (confirmed slot
   and texture; probable that slot 0 alone uses the plain surface UV).
   Roads and terrain blend two or three layers (`Blend_A`, `Blend_B`,
   `Blend_C`), each tiled by the material's pixel constants; the
   terrain rules (UVs, tilings, `Splat` at TEXCOORD1, vertex-colour
   weights on `H_VBLND`) are in
   [Terrain shading](#terrain-shading). The road weights (vertex
   colour R, G, B, which sum to 255 on many road vertices) are still a
   guess.
3. **Ambient occlusion.** `AO` or `AOMap` (linear) at the lightmap UV
   (per-model textures, unit UVs, white where open). On the terrain
   effects it scales the light by 0.05 + 0.95 × AO (see
   [Terrain shading](#terrain-shading)); multiplying the albedo by it is
   the simpler stand-in. Its slot follows from the sampler names, which
   the engine can read from the `.fxobj` constant table: `AOMap` is
   always slot 5; `AO` is slot 5, or 6 when slot 5 is `Blend_C` (17
   effects), or 4 when slots 5 and 6 are `Blend_C` and `Modulate`
   (4 road effects, `H_ROAD_DIFF3_*NOISE_MODULATE_AO*`); 89 of the 172
   effects with samplers have neither. `world-materials.py samplers
   --verbose` lists every effect's slots.
4. **Lighting.** The effects are lit by a sun and a two-colour sky. The
   frame's values (daytime, read from the constants of the colour draws
   checked; the census did not compare them across all 761):

   | Constant | Value |
   | --- | --- |
   | `sunDir` | (0.3848, 0.8754, 0.2926) in the game frame; (0.3848, 0.8754, −0.2926) in the file frame |
   | `sunColor` | (0.9695, 0.8358, 0.584), w 20 |
   | `skyColor` | (0.1687, 0.2229, 0.2746) |
   | `groundColor` | (0.1788, 0.1729, 0.1024) |
   | `ambColor` (props) | (0.1717, 0.2079, 0.2229) |
   | fog | `FogColor` (0.091, 0.1374, 0.1446), `FogColor2` (0.2233, 0.2232, 0.2232); `FogConsts2` (200, 4,000, 0.9, 4.9949) |

   `sunDir` is in the game frame: at a world-space draw the track
   vertex shader's world matrix (and its inverse transpose) is the z
   negation, so normals reach the pixel shader in the game frame. For
   the file frame, negate `sunDir`'s z; a local-space model's normals
   also take its placement's rotation (WORLD_GEOMETRY.md). A plausible first pass (engine's choice):
   colour = albedo × AO × (sunColor × max(0, N·sunDir) + mix(groundColor,
   skyColor, 0.5 + 0.5 N·up)), then the engine's own exposure. "Unlit",
   albedo × AO, is also fine for a first frame.
5. **Lightmap.** Do not multiply by `Light_Map`. 9,661 of its 17,386
   uses name an all-zero texture, and every lightmap the frame bound
   from its file (104 slots) was all zero, while the surfaces they sit
   on are lit; 105 more were bound at a 16 × 16 streaming size. The
   non-zero ones decoded (8, on `_EMISSIVE_` and `_AO_` effects) are
   gamma DXT1 atlases, mostly black (mean 5 to 35 of 255, 6 to 37 % of
   texels above near-black) with bright patches such as rows of
   windows: light to add, not an occlusion term to multiply. The pixel
   shaders that sample it also read `EmissiveSwitchOnThreshold` and
   `TimeGain`, so it is light added at night (lamps, windows):
   probable. Leave it out by day.
6. **Transparency.** In the frame no scenery draw used the alpha test;
   these did not write opaquely (blend source alpha, one minus source
   alpha): road decals (`H_ROAD_*_BLEND_*`, 15 draws), `H_VBLND_DECAL_*`
   (18) and the lake (5). Trees (18 draws) and most props (297) drew
   opaque with alpha to coverage (the multisampled scene pass; see
   Rendering, "alpha to coverage and late depth", on the docs site).
   Their diffuse textures carry the cut-out in alpha: decoded
   `TREE_DIFF_OPAC` diffuses are DXT1 with 76 to 81 % transparent
   texels, `OPAC` prop diffuses DXT5 or DXT1 with shaped alpha. That
   the shader's output alpha is the diffuse alpha is probable. An
   engine without multisampling can alpha-test at 0.5 the draws whose
   slot-0 texture has alpha (DXT3, DXT5, or DXT1 with transparent
   texels), and draw the rest opaque (engine's choice). The
   `H_VBLND_DECAL*` alpha is the diffuse (`Blend_A`) alpha times a
   vertex-colour fade broken up by noise:
   saturate(0.5 + c4.x × (COLOR.w − 0.5) − c4.y × (N − 0.5)), where
   COLOR.w is the first stored colour byte
   ([Colour byte order](#colour-byte-order)), N is the alpha of
   `Noise_Sampler` (slot 2) at TEXCOORD0 × c1.zw, and c4 is the
   material's pixel vec4 4 (3.5 and 8 on the frame's decal), by our
   reading of the shader (probable). On the frame's decal vertices the
   first byte runs from 178 to 255, so the fade is soft at the edges. `H_ROAD_*_BLEND_*` samples `Blend_A` without its
   alpha and has no c4: its alpha comes from interpolated vertex data
   (the vertex colour's alpha is the likely source; form not settled).
   `ADD_DIFF_OPAC_RGBA` (not drawn in this frame, so its blend state was
   not seen) writes colour only, from a diffuse texture scrolled with
   time, an `opacity` texture and a sun tint; additive (one, one) is a
   guess from its name and the missing alpha output.

The blend state per family is from `check-dump` (confirmed for the
frame); the formulas in 2 to 4 are not.

## Terrain, far scenery, trees and sky

Measured 2026-11-20 on the same free-roam frame: which draws cover which
part of the reference image, and how the terrain effects shade. The
shading below is our reading of the effects' pixel shaders (their
constant tables name the inputs; the order of operations is read from
the shader code and not yet tested against pixels), so it is probable
unless a line says otherwise.

### What draws the hills

A depth-tested raster of the 73 world-space models the frame drew, with
`clip_from_world` at 320 × 180, owns the upper 42 % of the image
(the hills and the sky line) as follows: about 9 % is no geometry
(sky), and nearly all the rest is terrain, mostly
`TERR_CLRD_ZONE02_AREA3_MIDDIST_*` and `TERR_CLRD_ZONE02_AREA6_*`
models with effect `H_VBLND_NORM_AO_LM` (600 m to 1.4 km away), then
near `Redstone_Area01_Terrain_*` and `TERR_*_PATCH_*` models
(`H_BLND2B`/`H_BLND3`, under 700 m), and two `UberLOD23_LOD00` and
`UberLOD24_LOD00` models (`H_BLND1_SPEC`, 5.8 and 7.4 km) for the far
snowy peaks.

| Kind of model | Drawn in the main view | Not drawn (all views) |
| --- | --- | --- |
| `*_MIDDIST_*` terrain | 17 (all within 3 km, a mix of `_NoLOD`, `_LOD01` and `_LOD02`) | 316 |
| `UberLOD*` (not `_CAGE`) | 2 | 65 |
| `*_CAGE_*` | 0 | 24 |
| `TERR_CUBE_*` | 0; 4 models drew 96 draws with other matrices (cube-map or reflection views) | 1,524 |

So the `_CAGE` walls do not draw at all in this frame, and `TERR_CUBE`
models draw only into the other views; an engine leaves both out of the
main view (confirmed for this frame). Which `MIDDIST` level and which
`UberLOD` patch the game picks for a camera is not known (WORLD_GEOMETRY
open question 3): within 3 km the frame drew 17 of the 73 `MIDDIST`
objects, and it drew different LOD suffixes side by side, so a nearest
level by distance is an engine choice, not the game's rule.

The far `H_DIFF_1` objects (`Zone2_Area3_6_middistance_*`, the `UberLOD`
patches and the `_CAGE` walls) all list a slot-0 texture with a file: all
58 such materials name a type-4 record whose `.bix` pair exists
(checked for all 58). One chain to compare against:
`coloradoout.06760.rmb.bin` (`Zone2_Area3_6_middistance_Area3`),
slot-0 value 0 → list 6,759 (member 6,760) entry 0 → record 11,177
(type 4, flag 1) → `_0x00002CA9.bix` and `_0x00002CA9_B.bix`.

### Terrain shading

All the terrain effects take the material's pixel constants as tilings
(`c360_0` to `c360_5` in their constant tables are the material's pixel
vec4s 0 to 5) and sample `AO` and `Light_Map` at TEXCOORD2, the unit
lightmap coordinate:

| Effect family | Layers and their UVs | Layer weights |
| --- | --- | --- |
| `H_VBLND*` (middle-distance hills, verges) | `Blend_A` at TEXCOORD0 × c0.xy, `Blend_B` at TEXCOORD0 × c0.zw | lerp(A, B, w), w = saturate((COLOR.x − 0.5) × c4.x + 0.5); c4.x is 1 on the hills, so w is the colour channel itself |
| `H_BLND2*`, `H_BLND3*` (near terrain) | `Blend_A` at TEXCOORD0 × c0.xy, `Blend_B` at TEXCOORD0 × c0.zw, `Blend_C` at TEXCOORD0 × c1.xy, `Splat` at TEXCOORD1 × c1.zw | lerp(A, B, Splat.R), then `Blend_C` over that with a weight that involves `Blend_C`'s alpha (form not settled) |
| `H_BLND1_SPEC` (`UberLOD`) | `Blend_A` at TEXCOORD0 × c0.xy (c0.xy is 1: an atlas laid out for the patch) | one layer |

(COLOR.x is the second stored colour byte, R: see
[Colour byte order](#colour-byte-order).) The middle-distance textures are aerial forest
photographs: `Blend_A` `_0x000036A0` (256², luma median 18 of 255) and
`Blend_B` `_0x000036A1` (512², median 28) on most `MIDDIST` materials,
with tilings 0.3 to 0.5. **The distant tree cover is in these textures**:
the hills carry no tree geometry past the billboards below.

Lighting in these shaders, with the frame's literal constants
(c254 = (1/256, 0.05, 0, 1/3), c255 = (0.5, 0.95, 0.1, 1)):

```text
ambient = mix(groundColor, skyColor, 0.5 + 0.5 * N.y) * (0.05 + 0.95 * AO)
sun     = sunColor * saturate(dot(N, sunDir)) * shadow * (0.05 + 0.95 * AO)
colour  = albedo * (ambient + sun) + specular
        + Light_Map * max(EmissiveSwitchOnThreshold.y, c3.w)
colour  = colour * fog.w + fog.rgb        (fog from the vertex shader)
output  = sqrt(colour)
```

- N and sunDir are in the game frame; `shadow` is the screen-space
  shadow mask (sampler 13), 1 for a first pass.
- AO scales the light through 0.05 + 0.95 × AO. On the `MIDDIST`
  hills the `AO` and `Light_Map` slots name type-1 records, and the
  frame binds type-1 slots as all-zero 4 × 4 DXT1 textures (8 `AO` and
  3 `Light_Map` bindings over the 131 world-space and local models of
  the frame; which draws they belong to was not tied). Taken literally
  that would light the hills at 5 %, and the image says otherwise: the
  game's pixels average 78 (of 255) on `MIDDIST` hills and 90 on near
  `H_BLND3` terrain, although the hills' layer textures are several
  times darker. **Use AO = 1 for type-1 slots.** Something brightens
  the far terrain that this reading does not show (the vertex colour,
  which the shader also reads, is a candidate); until it is found, an
  engine scales the hills to match the reference.
- `EmissiveSwitchOnThreshold.y` is 0 by day, so `Light_Map` drops out
  (this settles the recipe's lightmap reading for these effects).
- Specular: sunColor × saturate(N·H)^sunColor.w (w = 20), scaled by
  `TimeGain.w` (8 in the frame), the shadow and a mask from the blended
  layers' luminance × c5; it can be left out of a first pass.
- The shader writes √colour: the colour target holds gamma-2 values,
  decoded by later passes (exposure and grading are not covered here).
  The layer textures are dark in sRGB (luma medians 18 to 28 on the
  forest textures, 54 to 91 on the near layers): the brightness of the
  game's image comes from the light and those later passes, not from the
  albedo.

Colour spread of the near-terrain layers the engine asked about
(record index → texture, decoded base level, sRGB 0-255):

| Record | Texture | Size | Mean RGB | Luma 5th / 50th / 95th |
| --- | --- | --- | --- | --- |
| 11,180 | `_0x00002CAC` | 512² DXT1 gamma | (66, 63, 34) olive grass and litter | 25 / 54 / 84 |
| 4,314 | `_0x000010FE` | 512² DXT1 gamma | (90, 84, 76) grey-brown rock | 28 / 87 / 125 |
| 11,408 | `_0x00002D95` | 512² DXT1 gamma | (109, 94, 72) sand and dirt | 68 / 91 / 116 |
| (UberLOD) | `_0x00004767` | 512² DXT1 gamma | (68, 70, 63) atlas of rock and snow patches | 10 / 29 / 232 |

### Trees

Two systems, both found in the frame:

- **Near trees** are render models with `TREE_DIFF_OPAC_*` and
  `DIFF_TREEBEND_*` effects, placed through the kind-7 `.pgeo` tiles
  (WORLD_GEOMETRY), drawn with alpha to coverage (above).
- **Mid and far trees are billboards.** The frame has 3 × 167
  non-indexed quad-list draws (one set per main-pass band, 10,610 quads
  per set), plus grass and other vegetation sets drawn with related
  shaders. Each tree quad is one 40-byte record of a kind-7 `.pgeo`
  tile, read with a 40-byte stride, and its texture is a sprite atlas
  named by the tile. The atlases are texture records at the end of the
  record table, in no model's texture list. See
  [Tree billboards](#tree-billboards-engine-question-q38) for the
  record, the quad and the atlas choice.

#### Tree billboards (engine question Q38)

Answered 2026-11-20 from the frame dump (the draws' vertex fetches and
constants, the tree vertex shader run on sampled quads) and the
player's files. Confidence: confirmed for the position, frame,
batching and atlas choice (checks below); probable for the corner and
fade rules (read from the shader and checked on sampled quads).

**Frame and matrix.** There is no per-instance matrix. A tree draw's
matrix is the scene's clip-from-world matrix with the tile's origin
folded in: a point given in the tile's own frame, which is the game
frame (z = − file z) shifted so that the tile's box minimum (header
+0x10) is the origin, maps to clip space exactly as the world point
(minimum + point, then z negated) does under the scene matrix (checked
to 0.0002 on four points). The draw also gets the camera in that tile
frame and the tile box's size (header +0x20 minus +0x10).

**The record** (big-endian words; field offsets in bytes):

| Offset | Format | Meaning |
| --- | --- | --- |
| +0 | 10_11_11, unsigned normalised | base point: bits 0-10 / 2047 × box size x is X, bits 11-21 / 2047 × box size z is Z, bits 22-31 / 1023 × box size y is the height Y, in the tile frame. The two 11-bit fields are the horizontal axes and the 10-bit field the height, so the stored order is X, Z, Y, not X, Y, Z |
| +4 | 10_11_11, signed normalised | the trunk axis U: two's-complement fields of 11, 11 and 10 bits over 1023, 1023 and 511, in the order x, y, z (y up; unlike the position, not swapped); about (0, 1, 0) with a few degrees of lean |
| +8 | — | not read by the tree shader (the other vegetation shaders read it as a second signed vector) |
| +12 | 2_10_10_10, signed normalised | normal for the sprite's lighting, laid out as NORMAL |
| +16 | 10_11_11, unsigned normalised | bits 11-21 / 2047 is u1, bits 22-31 / 1023 is v1 (bottom edge); bits 0-10 are not read by the tree shader |
| +20 | 10_11_11, unsigned normalised | bits 0-10 / 2047 is u0, bits 11-21 / 2047 is v0 (top edge); bits 22-31 are 0 |
| +24 | four half floats, in stored order | half width w, height h (metres), near and far distance (metres) |
| +32 | 8_8_8_8, stored A, R, G, B | tint: 2 × (R, G, B) / 255 when A is above 127 (A is 255 in 370,664 of 401,498 records); otherwise the same times a per-frame colour (white in the frame) |
| +36 | 8_8_8_8, stored A, R, G, B | an extra light colour, squared and scaled by a per-frame factor (0 in the frame); zero in 400,771 records |

Decoded this way, all 361 records of `__R00G07021.pgeo` sit on the
frame's terrain mesh (the render models the depth check used): the
base point is 0.08 m below the ground at the median, and every record
is within 1 m (5th to 95th percentile −0.18 to +0.04 m). Over the five
kind-7 tiles inside the camera's 1 km sector, 1,298 of 1,298 records
are within 1 m. Quantisation is about 0.15 m across and 0.14 m in
height for a tile of this size. The engine's earlier test failed
because it read the third field as Z instead of the height.

**The quad.** Record k feeds vertices 4k to 4k + 3. With B the base
point, U the trunk axis, d the distance from the camera to B, and R the
unit vector at right angles to U and to the direction to the camera,
pointing to the viewer's right:

| Corner | Position | UV |
| --- | --- | --- |
| 0 | B − w R | (u0, v1) |
| 1 | B + w R | (u1, v1) |
| 2 | B + w R + h U | (u1, v0) |
| 3 | B − w R + h U | (u0, v0) |

That is bottom left, bottom right, top right, top left on screen
(checked by projecting six sampled quads in front of the camera with
the reference matrix). The quad is drawn when near − 100 < d ≤ far
and collapses to a point otherwise (100 m is a per-frame constant of
the draw; the switch is sharp, with no scaling in between). The pixel
shader multiplies the atlas colour by the tint and a per-vertex light
(a sky-to-ground gradient up the quad plus sun on the record's
normal; not decoded in detail), applies the fog and writes the square
root like the terrain; its alpha is the atlas alpha. Cut the sprites
out by that alpha (the draws' render state was not read).

**Batches and generated records.** A tile's groups (WORLD_GEOMETRY,
`.pgeo` tiles) hold its records in group order. Consecutive groups
that share the reference key and three other group words are one
draw. The draw's buffer holds those groups' file records, then one
record per placed model instance in the same groups, built at run time.
So the trees drawn as models up close also become sprites further
away. A generated record holds the instance's translation, quantised
the same way. Its near distance is the group's first distance (the
point where the model stops), and its far distance and sprite are the
group's. Checked: the buffer size is (records + instances) × 40 for
519 of 519 main-pass tree draws whose first record is in a tile, and
3,906 of 3,912 generated records lie within 0.15 m of one of their
batch's instances. Thirty draws start with a generated record and were
not tied.

**The atlas.** The group's reference key indexes the tile's reference
list (WORLD_GEOMETRY header +0x40), whose values are texture record
indices. The record's ID names the file `_0x<ID>`. For all 519 tied
draws the bound texture is the one this chain names; the frame binds
`_0x00005010` to `_0x00005013` and `_0x00005015`. The cell is not a
fixed grid: the group record holds its rectangle (u0, u1, v1, v0) and
the sprite's width and height. Every one of the 401,498 records over
the 1,582 kind-7 tiles that have any carries that rectangle, with u0
and u1 swapped in 204,899 of them (mirrored sprites). In most records,
w and h are the group's width / 2 and height times one per-tree scale.
No kind-7 reference names record 19,580 (`_0x00005007`, the grass
atlas). The grass and the other vegetation quads (four further shaders
with the same 40-byte stride) take their records from somewhere other
than any `.pgeo` file; their source is open. The second stream (2,736
bytes, fetch constant 90) is not read by the tree shader.

### Sky

The track has no sky model: no render model covers the sky pixels
(about 9 % of the image, which the depth targets leave empty).
`media/realtimesky.zip` holds a procedural sky's data: `CloudDefs.xml`,
near and far cloud density and ambient maps (front and back, full size
and small), a far-cloud mask, sun, moon phases, `Stars.bin` and a dither
texture, all `.xds`. Its shaders are not in the track's effects. Which
draw of the frame paints the sky was not isolated. An engine's own
gradient dome is the right stand-in until that sky is specified.

## Reference frame

`world-materials.py reference` writes the RI-0.4 free-roam frame as a
reference for the engine, in the M1 reference conventions of
[AUTOSHOW_REFERENCE.md](AUTOSHOW_REFERENCE.md#reference-files):
`.local/re/world/reference/freeroam.json` and `freeroam.png` beside it
(local only: the image is a capture of the player's game).

| Field | Meaning |
| --- | --- |
| `schema` | `shift-engine.world-reference.v1` |
| `image`, `width`, `height` | the capture, 1280 × 720, as PNG |
| `clip_from_world` | 16 numbers, column-major, for column vectors: clip = M × (x, y, z, 1) with (x, y, z) a render model position in the file frame as stored |
| `near` | 0.29993 m: clip z/w is 0 at that view depth |
| `half_pixel_removed` | true |
| `camera_position` | (1999.04, 145.383, −962.878), file frame |
| `world_space_members` | the 73 world-space models drawn with this matrix |
| `game_clip_from_world` | provenance: the scene matrix as the game sets it (rows are the clip components) |

The conventions are the car references' (a loader for those reads
this file with two changes): `clip_from_world` takes the place of
`clip_from_car`, and there is no `view`, `proj`, `car_to_world`,
`image_car_only`, `mask`, paint or `checks` field. As for the Autoshow
references: clip space is Direct3D's for an upright
image (the game's half turn taken out by negating the clip x and y
rows), z/w is standard (0 at `near`, towards 1 far away; the game stores
1 − z/w), and the matrix has the game's half-pixel offset removed (clip
x moved by −w / width, clip y by +w / height), so a standard viewport of
the image's size reproduces the game's pixels. Checked: a vertex
projected with `clip_from_world` lands 0.5 px left of and above where
the game's matrix and its pixel convention put it, as intended.

The image is the run's parked capture (`before.ppm`, route frame 1,200);
the dump was taken a moment later with the car still parked at the same
pose, so the scenery is the same (WORLD_GEOMETRY's overlay and depth
check used the same pair). It includes the HUD (minimap, speedometer,
the "THE GAUNTLET" banner). For a HUD-free image, replay the dump with
the UI draws left out as `autoshow-refs.py` does for cars; not done here.

## Evidence

Run: the RI-0.4 free-roam dump (WORLD_GEOMETRY.md, Evidence), base
build, Vulkan, 1x. `check-dump` ties draws to the 120 models the
depth check used (73 world-space, 47 local-space), with the frame's
register state replayed per draw (shaders, texture fetch constants,
c0):

| Measure | Count |
| --- | --- |
| tied draws | 1,367 |
| colour-pass draws (pixel shader one of the material's effect) | 761 |
| other passes (depth prepass, shadow, reflection) | 606 |
| colour draws whose vertex shader microcode the dump kept | 667 |

Colour-pass render state (`check-dump`; RB_COLORCONTROL and
RB_BLENDCONTROL0 at each draw):

| Effect family | Draws | State |
| --- | --- | --- |
| `H_BLND*`, `H_ROAD` (not `_BLEND`), `H_VBLND` (not `DECAL`), `ANIM_FLAG*`, `UNDERGROUND`, part of `H_DIFF` | 408 | opaque, no alpha test |
| `H_DIFF` (props, including `OPAC` fences) | 297 | opaque, alpha to coverage |
| `TREE_DIFF_*OPAC*` | 18 | opaque, alpha to coverage |
| `H_ROAD_*_BLEND_*` | 15 | source alpha, one minus source alpha |
| `H_VBLND_DECAL_*` | 18 | source alpha, one minus source alpha |
| `LAKE` | 5 | source alpha, one minus source alpha |

No colour draw enabled the alpha test. The lighting constants in the
recipe are the pixel and vertex constants read at colour draws of
this frame; the world matrix of the track vertex shader is the z
negation at the world-space draw read.

Corpus counts come from `census` over the extracted members.

## Open questions

1. The 98 models without a `uvOffsetScale` write (road decals, trees).
2. Which `uvOffsetScale` a submesh takes when the order is not file
   order (one model of the frame).
3. Record types 1 (11,409) and 3 (5): no file; the frame binds 4 × 4
   textures for them (runtime placeholders or generated textures).
4. The 3 CAFF textures with a 196,604-byte payload; the 146 `.bundle`
   members (texel payloads, not referenced by any texture record seen).
5. The pixel constants' meaning beyond the terrain tilings; the
   `Blend_C` weight on near terrain and the road weights; whether the
   sun term takes the AO factor. Testing the terrain reading against
   the reference frame's pixels would settle it.
6. Whether `Light_Map` is night lighting (a night dump would settle
   it; by day the terrain shaders switch it off).
7. A HUD-free reference image.
8. The tree billboards (answered for kind-7 tiles, see
   [Tree billboards](#tree-billboards-engine-question-q38)): where the
   grass and other vegetation quads' records come from, the sprites'
   lighting in detail, and their render state.
9. Which `MIDDIST` level and `UberLOD` patch the game draws for a
   camera (WORLD_GEOMETRY open question 3).
10. The sky: the draw that paints it and its parameters
    (`realtimesky.zip`).
11. Which placements the game shows (event props): answered 2026-11-20
    for free roam (route lists on `.pvsz` records and switchable kind-7
    tiles; route 0 is free roam), see WORLD_GEOMETRY.md,
    [Which placements free roam draws](WORLD_GEOMETRY.md#which-placements-free-roam-draws-engine-question-q4).
    Still open: the camera-to-zone lookup, and which instances of the
    shown vegetation tiles are drawn.

## Reproduce

```text
python -I tools/re/world-materials.py census .local/re/world/all
python -I tools/re/world-materials.py samplers .local/re/world/all [--verbose]
python -I tools/re/world-materials.py model .local/re/world/all 9271
python -I tools/re/world-materials.py texture .local/re/world/all 0x2EB6 --out road.png [--level N]
python -I tools/re/world-materials.py check-dump .local/re/world/all <dump.fh1frame> \
    --members @<names file>
python -I tools/re/world-materials.py reference .local/re/world/all <dump.fh1frame> \
    .local/re/world/run1/before.ppm --members @<names file> \
    --out .local/re/world/reference/freeroam.json
```

`--pvs` names another `Colorado_00.pvs` than
`.local/game/base/media/tracks/colorado/Ribbon_00/Colorado_00.pvs`.
The members file for `check-dump` is the depth check's (WORLD_GEOMETRY.md,
Reproduce). `census` takes about a minute.
