# Showroom lighting: environment, light terms and post chain for the Autoshow

| | |
| --- | --- |
| Spec ID | `FORMAT-SHOWROOM-LIGHTING` |
| Status | partial (wrapped up early, playable first): the reflection cube, the light direction and colour, the tone-map, desaturation and grading constants, the drop shadow and the lamp glow's cause are established; the small glossy cube's construction, the exact composite order and the glow's post pass are open |
| Written | 2026-11-19, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | `site/findings/rendering.toml` groups "Frame structure and views" ("Autoshow showroom car passes"), "Shaders, textures and models" ("Autoshow car environment maps", "Autoshow car shader parameters"), "Post effects" ("Autoshow final composite"), "Shadows" ("Autoshow drop shadow") and "Light glows" ("Autoshow lamp and highlight glow") |
| Implements it | `shift-engine` `carview` showroom lighting, for plan item M1 (not yet) |

Our own reading of the player's files and of M1 frame dumps
([AUTOSHOW_REFERENCE.md](AUTOSHOW_REFERENCE.md)). No disassembly,
decompiled code, shader listing or game data is reproduced here
([LEGAL.md](../LEGAL.md)). Values are names, counts and our own
measurements of run-time state. The car files are specified in
[CARS.md](CARS.md) (FORMAT-CARS).

## Summary

- **What lights the car is one file**: the Autoshow track's static car
  cube map, `media/tracks/uiautoshow/staticcarcubemap.xpr`. It is a
  512 × 512 HDR photograph of a hangar with blue stage lights,
  LogLuv-encoded in 8-bit RGBA. Every car draw reads it, byte for byte as
  stored in the file. The blue and white reflections across bonnets and
  roofs are this cube. Its light sources reach about 97 times mid-grey
  once decoded.
- **The engine reads the rest of the light setup from one more file**:
  `media/tracks/uiautoshow/TrackSettings.xml`. Its key light direction,
  tone-map, desaturation and bloom values reach the shaders unchanged.
- **Nothing is rendered per frame for the car's environment.** The
  dynamic reflection cube is cleared every frame to a value that decodes
  to black, because the Autoshow's `TrackSettings.xml` turns the dynamic
  cube map off.
- **Two small cubes are made at run time.** One is a 64 × 64 glossy
  version of the environment that only metallic paint uses. The other is
  a 64 × 64 table of spherical-harmonic basis values, which the engine
  can compute itself.
- **The screen-sized texture is a "background map" input that is
  switched off.** It is the scene colour buffer, bound to every car
  shader, but the shaders' switch for it is off in the showroom. Car
  shaders also bind a 32 × 32 all-white shadow mask, so nothing is
  shadowed.
- **The darkening under the car is a drop-shadow quad**: a five-vertex
  fan on the floor, drawn with the car's matrix.
- **The glow round lamps and highlights is not a sprite.** It is the post
  chain spreading the car's brightest HDR pixels. The brightest of those
  come from the lamp covers' additive reflection pass. No lamps are lit.

## Where the inputs come from

| Input | Size and format | What it is | Source | Confidence |
| --- | --- | --- | --- | --- |
| Static environment cube | 512 × 512 × 6, 10 levels, 8-bit RGBA, LogLuv | specular reflection of the stage | file: `media/tracks/uiautoshow/staticcarcubemap.xpr`, one resource `StaticCarCubemap_HDR_1` | confirmed: the cube in guest memory equals the file's payload byte for byte, base level and mip region (2026-11-19, Abarth 500 dump) |
| Dynamic environment cube | 256 × 256 × 6, 9 levels, 10:10:10:2 written, read as 8-bit LogLuv | reflection of the live scene, off in the showroom | rendered at run time: cleared, six faces resolved, a mip chain built; no geometry is drawn into it | confirmed (draw log of four dumps); its value decodes to luminance below 10^-18 |
| Glossy cube ("specular cubemap") | 64 × 64 × 6, 1 level, 8-bit LogLuv | blurred environment, weighted by the paint's metallic flag | made at run time from the environment (not in any file); constant across the ten dumps read | probable (name from the shader's constant table; construction open) |
| SH lookup cube | 64 × 64 × 6, 16-bit float RGBA | the first two bands of spherical-harmonic basis values per direction; read by the vertex shader | made at run time; pure geometry | confirmed by decoding (below) |
| Screen texture ("background map") | 1280 × 720, 10:10:10:2 | the scene colour buffer from earlier in the frame (and the previous frame where not yet written) | run time | confirmed (address equals the scene resolve target); not sampled in the showroom: its switch is 0 (probable, by switch name and value) |
| Shadow mask | 32 × 32, 8-bit RGBA, every texel 255 | stand-in for the screen-space shadow mask | run time | confirmed (contents); unshadowed |
| Drop-shadow texture | 256 × 256, 8-bit RGBA | top-down soft silhouette of the car (grey about 209/255 inside, falling to 0 at the edge) | run time; not found in any of the player's files | probable |
| Light and post parameters | scalars | see the tables below | file: `media/tracks/uiautoshow/TrackSettings.xml` | confirmed where marked |

### The XPR cube file

`staticcarcubemap.xpr` is an `XPR2` container:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 bytes | `XPR2` |
| 4 | u32 | header size H (2048 here) |
| 8 | u32 | data size |
| 12 | u32 | resource count (1) |
| 16 | 4 bytes per resource: type (`TXCM`, a cube texture), then u32 header offset, u32 header size, u32 name offset, each from byte 12 | |
| 12 + header offset | 52 bytes | the same texture header as an `.xds` (FORMAT-CARS, `.xds`): `3`, six fixed words, then the six-word fetch constant |
| 12 + H | | texel data |

The fetch constant says: format 6 (8_8_8_8), endian mode 2, tiled,
512 × 512, cube (dimension 3), 10 levels, packed mip tail. The six faces
of the base level follow one another, each a tiled 512 × 512 surface of
4-byte texels (1 MiB) in the order +X, −X, +Y, −Y, +Z, −Z, laid out as
FORMAT-CARS's `.xds` rule says. The mip levels start at the data offset
that the fetch constant's mip address gives (6 MiB, right after the base
faces). The base level and the mip region were both checked byte for
byte against guest memory. The mip layout within that region was not
decoded. An engine may rebuild the mips itself, but must filter them
after decoding (below), never on the encoded bytes.

Each texel's four bytes, after the 8-in-32 swap, give the shader's
(R, G, B, A) as bytes 1, 2, 3 and 0 of the stored word: the standard
fetch swizzle of an ARGB texture. Faces decoded this way show a
continuous hangar.

The other track folders (`colorado`, `uipaintshop`, `uithumbig`,
`uithumbnail`, `uiworkshop`) and `media/tracks/` itself carry their own
`staticcarcubemap.xpr`. The executable names the path as the track
folder plus `staticCarCubemap.xpr` (and a numbered variant). The
Autoshow uses `uiautoshow`.

### LogLuv decoding

The cubes use the published LogLuv encoding for 8-bit render targets
(the form described by Christer Ericson and Marco Salvi). With the
texel's channels as fractions r, g, b, a in 0..1:

```text
Le = 255 b + a
Y  = 2 ^ ((Le - 127) / 2)
Z' = Y / g
X' = r Z'
(R, G, B) = max(0, M^-1 applied to (X', Y, Z'))

M (linear RGB to X', Y, Z', as rows for R, G, B):
  R: 0.2209 0.3390 0.4184
  G: 0.1138 0.6780 0.7319
  B: 0.0102 0.1130 0.2969
```

Here X' = 0.2209 R + 0.1138 G + 0.0102 B, and so on. M^-1 is its inverse.
The shader constants `psStatCubeLogLuvDecode` and `psDynCubeLogLuvDecode`
are set (1) in every car draw. Confidence: probable. The decode gives a
plausible image,
a continuous hangar with neutral greys, but the matrix was not read
from the game.

Decoded, the static cube has a mean radiance of (0.180, 0.166, 0.239)
(solid-angle weighted, linear RGB). Its lights peak at 80 to 97 per
channel, and the darkest texels are below 10^-5. The engine needs a
float format (16-bit at least) for it.

### The SH lookup cube

For a unit direction (x, y, z) in the cube's frame, each texel holds
(c, −k y, k z, −k x) with c = 1.1807 and k = 2.0449 (16-bit float, all
six faces checked at centre, corner and edge texels). These are the
constant and linear spherical-harmonic basis functions scaled by about
4π/3. An engine computes them; it needs no texture.

## Light parameters

All values come from the frame dumps of ten cars (base, v4 and DLC; Abarth
500 and 595, 8C Competizione, 8C Spider, TZ3, DBS, One-77, Vanquish,
RS4 Avant, RS4). They were read from the car shaders' constant registers
and named through the constant tables in the player's
`media/shaders/Cars/shaders_ui_v16.fxobj`, the effect the showroom loads
(its shaders' microcode is found in that file). "Global" values were the
same in all ten.

### Global terms (from `TrackSettings.xml`)

| Shader constant | Measured | From the file | Meaning | Confidence |
| --- | --- | --- | --- | --- |
| `psLightDirWS` | (−0.5145, 0.8575, 0) | `ShadowLightDirection` (0.6, −1, 0), normalised and negated, applied because `OverrideShadowLightDirection` is 1 | unit vector towards the key light, world frame (+Y up) | confirmed (value) |
| `shDirectionalColour` | 0.6525 in all channels | `iblSunColor` 0.87 × 0.75; the 0.75 equals both `filmicTone exposure` and `iblDirectScale` × `iblDirectScaleCar` (0.5 × 1.5), so which one applies is open | key light colour | probable (value), guess (source) |
| `shDirectionalLightI` | (1.1816, −1.755, ≈0, 1.053) | the light direction in the SH basis above | the key light in the SH basis | confirmed (it is the basis at `psLightDirWS`) |
| `shHemisphericalLightR`, `…G`, `…B` | R (0.2205, 0.0811, −0.0124, −0.0036), G (0.2062, 0.0559, −0.0245, −0.0064), B (0.3330, 0.0169, −0.0271, −0.0427) | computed at run time; not the static cube's plain projection (its directional terms are about twice as strong relative to the constant term) | ambient light as SH coefficients, dotted with the basis of the normal (vertex shader) | probable (meaning), open (how they are made) |
| `MinShadowValue` | 0.75 | per-effect | floor of the shadow term; irrelevant because the shadow mask is white | probable |
| `DynamicEnvLightmapDampner`, `StaticEnvLightmapDampner` | 0.3, 0.85 | per-effect | weights of the dynamic and static cubes; the dynamic one is black | probable (by name) |
| `ReflEnvDampnerMin`, `…Max` (vertex) | 0.1, 0.45 | per-effect | reflection dampening range | guess |
| `psFogColor` | (0.5, 0.5, 0.5) | | fog colour; the fog bands are disabled in the file; the height fog's effect on cars is not established | open |

The paint does not use `SHLighting` from `TrackSettings.xml` directly:
its top (0.2) and bottom (0.1) do not appear among the car constants.

### Paint (techniques `body`, `body_2` and the other exterior paint)

| Constant | Measured | Meaning | Confidence |
| --- | --- | --- | --- |
| `PaintColor` | the car's first colour row, sRGB to linear (FORMAT-CARS, Paint); e.g. 0.956 for 250/255 | base colour | confirmed (ten cars) |
| `FresnelIndex` | 1.55 on every car | index of refraction of the clear coat; Schlick's F0 = ((n − 1)/(n + 1))² = 0.0465 | confirmed (value), probable (meaning) |
| `FresnelScalar` | 0.9 | scale of the Fresnel reflection | probable |
| `reflectionScaler` | 1.0 | scale of the cube reflection | probable |
| `SpecularCubeScalar` | 1.0 where the colour row's `Metallic` is 1, 0.0 where it is 0 | weight of the glossy 64 × 64 cube, the metallic look | confirmed (ten cars: six metallic, three not, plus one more of each kind seen in the transparent pass) |
| `MetalColor`, `MetalScale`, `MetalPower`, `MetalBias` | (0, 0, 1), 0, 5, 0 on every car, metallic or not | a flake term that is off in the showroom | confirmed (values) |
| `TwoToneScale`, `TwoToneBias` | 0 | two-tone paint off for the factory colour | confirmed (values) |
| `StripeColor` | the stripe colour, sRGB to linear (0.4452, 0.0006, 0.0006 on the Abarth 500's red stripe) | stripes (FORMAT-CARS leaves them open) | probable |
| `StaticEnvSHUpper`, `StaticEnvSHLower` | 0.05, 0 | ambient from the environment, upper and lower hemisphere | guess |
| `psDeferredHeadlightEnable` | 0 | headlamps lighting the scene; off | confirmed (value) |
| `useStaticCubeMap` | 1 | the static cube is used | confirmed (value) |

### Glass and lamp covers

The transparent list (FORMAT-CARS, Materials) is drawn after the opaque
parts, with depth test on and depth write off:

| Parts | Blend | Shader inputs | Measured constants |
| --- | --- | --- | --- |
| windows (`window`, `glass`) | source alpha, one minus source alpha | both cubes; the white shadow mask | `FresnelIndex` 4.35, `FresnelScalar` 0.7, `GlassColor` (0, 0.01, 0.0072) alpha 0.29, `GlassMaxOpacity` 1.0, `SpecularColor` (0.604, 0.604, 0.787), `SpecularPower` 133, `SpecularAmount` 0 |
| lamp covers and lamp glass (`headlight*`, `taillight*` and their `glass*HL`/`*TL` sections) | two passes per part: first multiply (destination colour, zero), then add (one, one) | the multiply pass reads the car's `lights` atlas; the add pass reads both cubes | add pass: `Glancing` 0.4, `Direct` 0.1, `FresnelPower` 5, `FresnelScale` 1, `StaticEnvLightmapDampner` 0.85 |

So a lamp cover first tints what is behind it by its texture, then adds
an environment reflection on top. Fresnel with power 5 runs from
`Direct` (0.1) facing the camera to `Glancing` (0.4) at grazing angles
(probable, by name). The add pass has no alpha, so where it reflects a
cube light (80 to 97) the HDR value is very large. This is what reads as
a "lit" headlamp.

The per-technique values differ from the constant tables' defaults (for
example `FresnelIndex` 1.65 by default, 1.55 measured), so the effect sets
them per technique. Per-car overrides are in the car's `ShaderSettings.xml`
(FORMAT-CARS, Materials). Until the effect's parameter blocks are
specified, use the measured values above.

### What the engine should do (M1)

1. Load `staticcarcubemap.xpr`, untile, decode LogLuv to float RGB and
   build mips in linear space. Use it for every reflection. Do not add a
   studio light of your own: the cube is the light.
2. Treat the dynamic cube as black. Ambient light is the SH coefficients
   above, dotted with the basis of the surface normal. Add the key light
   with direction `psLightDirWS` and colour 0.6525.
3. Paint: diffuse `PaintColor` × (ambient + key × max(0, n·l)), plus a
   mirror reflection of the static cube (level 0) weighted by Fresnel
   with F0 from `FresnelIndex` 1.55 and scale 0.9. For metallic paint add
   a blurred reflection (a cosine-power lobe of exponent 8 to 16 over the
   decoded cube is the nearest simple filter found; see Unknowns). This is
   a guess at the shape of the BRDF, not the game's equation.
4. Lamp covers: multiply by the lights atlas, then add the cube
   reflection × Fresnel(0.1 to 0.4, power 5).
5. Shade into a float target and clamp to 4.0 before the post chain
   (below).

## Post chain

The car is shaded into the 4x MSAA main pass in a 7e3 float format
(2_10_10_10_FLOAT), which saturates at 31.875. Each band is resolved,
with the four samples averaged, into a 1280 × 720 10:10:10:2 UNORM
texture with exponent bias −2: the stored value is HDR / 4, so **scene
values above 4.0 are clipped** before tone mapping (confirmed: resolve
format and bias registers). After the bands, the frame runs:

| Step | Size | Role | Parameters from `TrackSettings.xml` | Confidence |
| --- | --- | --- | --- | --- |
| reduce | 320 × 192, then 64 × 32 and 32 × 32 | luminance and bloom sources | | confirmed (passes and sizes) |
| exposure | 32 × 32 float | adapted value, read back by the final composite; it held 0.0584 in every texel of the Abarth 500 dump | `ToneMapAdaptiveExposure` (all exposures 0), `CameraExposureRange` (−2, 0), `ExposureBias` 0 | probable (role), open (use) |
| bright pass | 320 × 192 | constants (1.0, 1.0, 0.06, 3.0) | `bloom cutoff` 1.0, `scale` 1.0, `curve lowerGradient` 0.06 and `upperGradient` 3.0 | confirmed (values), probable (role) |
| blur down and up | 160 × 96, 96 × 96, 96 × 64, 64 × 64, 64 × 32, 32 × 32 and back to 320 × 192 | bloom | `bloom weights` (layer1 0.3, layer4 2.4) not matched to constants | probable |
| scene rewrite | 1280 × 720 | reads the scene three times and writes it back; a required pass (record "ordered reductions and final-composite preparation") | | open |
| final composite | 1280 × 720 into 10:10:10:2 | tone map, desaturate, grade; reads the scene, the 320 × 192 bloom, the 16 × 16 × 16 grading volume and the exposure | below | confirmed (inputs), probable (order) |

The final composite's constants match the file exactly:

- **Tone map**: the Hable ("Uncharted 2") filmic curve with
  `filmicTone` white 1.5, shoulder strength A = 0.151, linear strength
  B = 0.038, linear angle C = 0.566, toe strength D = 0.21, toe
  numerator E = 0.003 and toe denominator F = 0.141. The constants
  hold C·B, D·F, D·E and E/F as precomputed (confirmed by value).
  F(x) = (x (A x + C B) + D E) / (x (A x + B) + D F) − E/F, output
  F(e x) / F(white). `filmicTone exposure` 0.75 is also present.
- **Desaturation**: a colour matrix out = s · c + (1 − s) · L(c), with
  s = `DesaturateAmount` 0.92 and L the luminance weights
  `DesatLuminosity` (0.2125, 0.7154, 0.0721) (confirmed: every term of the
  matrix matches). `DesatPostMod` is 1.
- **Grading**: a 16 × 16 × 16 8-bit volume, addressed with scale
  15/16 and offset 1/32 (texel centres). Its first rows are an identity
  ramp. Whether the whole volume is identity was not checked (open).
- `ShadowColor`, `DarkColor`, `LightColor` and `TrackLightMapParams` are
  also loaded into the composite. Their effect is not established.

The order of exposure, tone map, desaturation, grading and the output
gamma was not recovered. Since the M1 references are measured after all
of it, the engine can fit the whole chain from these measurements (our
own, Abarth 500, near-grey pixels, the stored 10-bit pre-composite value
from the dump against the reference image's 8-bit value):

| Stored scene value (of 1023) | Scene HDR (×4/1023) | Reference sRGB byte |
| ---: | ---: | ---: |
| 4 | 0.016 | 9 |
| 20 | 0.078 | 26 |
| 36 | 0.141 | 45 |
| 52 | 0.203 | 70 |
| 68 | 0.266 | 100 |
| 84 | 0.328 | 131 |
| 100 | 0.391 | 157 |
| 116 | 0.454 | 180 |
| 132 | 0.516 | 201 |
| 148 | 0.579 | 216 |
| 164 | 0.641 | 228 |
| 180 | 0.704 | 236 |
| 260 and up | 1.0 and up | 255 |

The curve is S-shaped (a toe below about 0.2 and a shoulder above 0.45)
and saturates at about HDR 1.0. The pre-composite values were read after
the scene rewrite and from the previous frame (the dump's memory at the
frame start), against the reference frame one orbit step later. Use only
the smooth regions. Hable with the file's constants alone, followed by a
plain power-law gamma, fits it poorly (residuals of 10 to 20 levels), so
another term (the scene rewrite, the exposure or the presenter's gamma
ramp) is in the chain. Confidence: confirmed as a measurement, open as a
formula.

For the references, `disable_bloom` (on by default in Pinyon Shift and
in the render-test runner) zeroes the per-frame bloom scale, depth of
field and motion blur are off, and MSAA is resolved
([AUTOSHOW_REFERENCE.md](AUTOSHOW_REFERENCE.md#seed-and-settings)). The
glow below survives all of that.

## Drop shadow

One draw before the car's transparent list puts a five-vertex fan (a
centre and four corners, four triangles) on the floor plane, y = 0 in the
car's body frame, with the car's own matrix. Its corners are about
±1.02 m across and −1.85 to +1.86 m along the car for the Abarth 500. It
blends source alpha over the stage, uses a stencil test (reference 128)
and no depth test, and samples the 256 × 256 top-down silhouette texture.
`TrackSettings.xml` sets `DropShadowColor` black and
`DropShadowOpacityAdjustment` 0.4 (guess: these drive it). The reference
masks leave this draw out (it is not a car-model draw), so it only
changes the stage around the car in the full image. An engine may draw
its own soft top-down blob instead. Confidence: probable (draw and
vertices confirmed; its texture's origin and the opacity open).

## Lamp and highlight glow (engine question Q11)

The reference coverage masks show a soft glow beyond the body around the
lamp nearest the camera (DBS, Vanquish, One-77, Abarth 500) and round
other hot spots, such as a roof flare where a stage light catches the
RS4's window frame.

| Question | Answer | Confidence and evidence |
| --- | --- | --- |
| What draws it? | No sprite or flare geometry. It is the post chain spreading the car's brightest HDR pixels across the screen. Those pixels come mostly from the lamp covers' additive reflection pass, and from mirror-like reflections of the cube's lights on chrome and paint. | Probable. The car-only replay of the DBS minus the same replay without the car's additive draws loses 13,522 mask pixels, all round the left headlamp (box 22..192 × 358..520); the additive draws write only inside their own parts, so the change outside them must be spread by a screen pass. The glow on the Abarth 500 falls smoothly from about 8/255 at the body to 1/255 at about 100 px, while the frame's far background is unchanged (0 difference), so it is local, not an exposure shift. No car-matrix draw in the main pass other than the drop-shadow fan is non-model geometry, and no car draw binds a `media/carlights` texture. |
| Which pass and blend? | The glow is added in the post chain after the main pass. The likeliest pass is the bloom chain: bright pass at 320 × 192 with cutoff 1.0, blur to 32 × 32 and back, added by the final composite. It reads the scene after the 4.0 clip. | Open: which post pass carries it was not isolated. The per-frame bloom scale is zero in these runs and the glow remains. Test: replay a car-only dump with the composite's bloom input nulled, then with the scene rewrite skipped (replays were blocked by the bulk capture's run lock on 2026-11-19). |
| Size rule, shape, colour | Screen space: a wide, soft, radially falling halo about 100 px across at 1280 × 720, the colour of the source highlight (bluish white on the DBS). It does not depend on lamp position or car size. | Probable (measured on two cars). |
| Lamp positions and a lights-on flag | Not used. No lamp is lit in the showroom: `psDeferredHeadlightEnable` is 0 on every car draw, and the bright lamps are the cube reflection on the lamp covers. `CarPartPositions` rows 0 to 3 are not needed for the glow; their meaning stays a guess (FORMAT-CARS, Wheels). | Confirmed (switch value); probable (that this is the whole cause). |

For the engine: render the lamp covers' additive pass and keep HDR to
4.0. Then threshold the frame at the bright-pass cutoff (1.0 HDR), blur
it at quarter resolution with a radius of about 25 quarter-resolution
pixels and add a small fraction to the image. Calibrate the fraction
against the DBS reference (13,522 mask pixels from the glow) and the
Abarth 500 (2,488). The reference mask now leaves the additive draws out
(the `cover` replay of `autoshow-refs.py`), so an engine mask should
also be made without its own glow.

## Unknowns

| What | Impact on M1 | Rule meanwhile |
| --- | --- | --- |
| How the 64 × 64 glossy cube is made. A cosine-power lobe over the decoded static cube correlates 0.76 to 0.79 in log luminance (best exponents 8 to 16), but the real cube has much more contrast, so its source or filter differs. The executable names a "blit lighting cube face from cube map" step in LogLuv | metallic paint's soft reflection | filter the decoded static cube with a lobe of exponent 8 to 16, scale to match the measured mean (0.087, 0.090, 0.138) |
| How the ambient SH coefficients are made | ambient level and direction | use the measured coefficients (they were the same in all ten dumps) |
| The paint and glass shading equations (shape of the specular term, how Fresnel and the cubes combine) | finish | the M1 rule above; tune to the tests |
| The order of the final composite and the source of the measured curve's toe | tone | use the measured table |
| Which post pass draws the glow | silhouette | the M1 glow rule above |
| The drop-shadow texture's origin and opacity | stage only, outside the mask | engine's own blob |
| Whether the grading volume is identity | colour | identity |
| Fog colour 0.5 and the height fog | none seen on the car | ignore |

## Tolerances

Compare against the M1 references with the image metric of
[REIMPLEMENTATION_PLAN.md](../REIMPLEMENTATION_PLAN.md) (masked PSNR,
silhouette IoU). Inputs read from files (the cube's decoded texels, the
`TrackSettings.xml` values) must match exactly. Measured run-time values
are exact to the four decimals given.

## Tests

| Test | Expected |
| --- | --- |
| `uiautoshow/staticcarcubemap.xpr`: resources, type, size, levels, format | 1, `TXCM`, 512, 10, 8_8_8_8 |
| Decoded static cube, solid-angle mean (linear RGB) | (0.180, 0.166, 0.239) ± 0.005 |
| Decoded static cube, largest channel value | 80 to 98 |
| Decoded face means (+X, −X, +Y, −Y, +Z, −Z), blue channel | 0.42, 0.25, 0.15, 0.29, 0.26, 0.15 (± 0.01) |
| `psLightDirWS` from `TrackSettings.xml` | (−0.5145, 0.8575, 0) |
| SH basis at +X, +Y, +Z | (1.1807, 0, 0, −2.0449), (1.1807, −2.0449, 0, 0), (1.1807, 0, 2.0449, 0) |
| Hable constants derived from `filmicTone`: C·B, D·F, D·E, E/F | 0.02151, 0.02961, 0.00063, 0.02128 |
| Desaturation matrix first row | (0.937, 0.0572, 0.0058) |
| Abarth 500 (car 1115, paint 250/250/250, not metallic), reference image, bonnet box x 450..599, y 300..329, mean sRGB | (214.4, 215.6, 226.5) |
| Abarth 500, door box x 940..1039, y 330..399 | (191.5, 189.7, 196.4) |
| Abarth 595 (car 1545, paint 232/233/228, not metallic), bonnet box x 560..699, y 360..394 | (210.6, 212.3, 217.9) |
| Abarth 595, door box x 880..979, y 400..499 | (164.9, 163.7, 168.1) |
| Abarth 500 bonnet, stored scene value before the composite (previous frame) | about (147, 149, 163) of 1023: HDR about 0.58, 0.58, 0.64 |
| Glow: DBS (car 1081) car-only mask pixels added by the additive lamp draws | 13,522 |

Boxes are in the reference image's pixels, top-left origin. The same
boxes in `<car>.car.png` (car only) give the same means within 0.1. The
bonnets are bluish because they reflect the cube's blue stage lights,
not because of a blue key light (the key light is grey).

## Reproduce

All of this was read from dumps under `.local/re/cars/dumps/` and from
the player's files. The dump reader is `tools/re/world-geometry.py`
(`read_frame_dump`, `walk_draws`), extended to keep the pixel-shader
constants, the bool constants, the texture fetch constants and the
shader loaded at each draw. Constant names come from the constant tables
in `media/shaders/Cars/shaders_ui_v16.fxobj`, matched to the dumped
shader by its microcode bytes. The draw logs (`*.ref.draws.log`) give
each draw's bound textures. No replay was run for this spec.
