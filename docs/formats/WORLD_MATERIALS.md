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
| COLOR | 4 | 8_8_8_8, unsigned normalised | four bytes, in memory order A, R, G, B (probable: the first byte is 255 on most vertices) |

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

- Every world texture is a power of two on each side (8 to 1,024) and
  has the packed-tail layout; the header pitch is not stored: use the
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
   `Blend_C`); for a first pass `Blend_A` alone is enough. The weights
   are, by the data, the `Splat` texture's channels on `H_BLND*`
   (which UV it uses is not checked; most texels have R + G + B well
   under 1, so they read as "how much of B and C over A", not as
   normalised weights) and the
   vertex colour's R, G, B on roads (R + G + B is 255 on many road
   vertices): a guess, not checked against pixels. The pixel constants'
   per-layer tiling is not applied in this recipe.
3. **Ambient occlusion.** Multiply by `AO` or `AOMap` (linear) sampled
   at the lightmap UV (probable: per-model textures, unit UVs, white
   where open).
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
   texels), and draw the rest opaque (engine's choice). Which alpha the
   decals use (diffuse or the vertex colour's A, about 0.5 on decal
   strips) is not known.

The blend state per family is from `check-dump` (confirmed for the
frame); the formulas in 2 to 4 are not.

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
5. The pixel constants' meaning per effect, and the shading formulas
   (layer weights, lighting, fog): the recipe's are a stand-in.
6. What alpha the decal and road-blend draws use, and whether
   `Light_Map` is the night lighting (a night dump would settle it).
7. A HUD-free reference image.

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
