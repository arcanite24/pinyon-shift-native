# Spec questions

Gaps found while implementing a spec. Each names the spec, what is
missing, whether it blocks, and what the engine does meanwhile. Blocking
questions are also sent to the specification session.

## Q1 (answered): the per-car reference JSON

- **Spec:** `docs/formats/AUTOSHOW_REFERENCE.md`, "Reference files"
  (horizon1-recomp `ebecd64d`, schema `shift-engine.m1-reference.v1`).
- **Asked:** field names, matrix order and clip conventions, the car's
  transform and the mask definition. v1 answers all of them:
  `clip_from_car` is column-major, car space to upright Direct3D clip with
  the half pixel removed; the mask is every pixel the resolved image
  changes by the car's draws.
- **Engine:** reads v1 (`apps/carview/reference.h`), draws the mask with
  the colour pass's MSAA and counts any resolved coverage (the spec's
  mask is now the pixels a car-only replay changes by 6/255 or more, so
  ours may be a fraction of a pixel wider), compares colour with
  `image_car_only` when present, and prints IoU,
  masked PSNR and the edge-overlap score (reference edge pixels with an
  engine edge pixel within 1 px; the plan reports it without a threshold).
  Not used yet: `checks.transparent_clip_from_car`, the glass and lamp
  covers' own matrix (about 1 px off), so glass may sit 1 px off.

## Q2 (answered): which copy of a car present on the disc and in a package

- **Specs:** FORMAT-CARS "Rule" says take the disc copy first;
  AUTOSHOW_REFERENCE "Some models exist twice" says a bake should prefer
  the package copy and report a difference.
- **Engine:** the bake takes the disc copy, hashes every package copy of
  the same media name, and reports any difference. For the official set
  FORMAT-CARS says they are identical, so the choice changes nothing.
- **Answer (spec session):** bake the disc copy; all 31 multi-copy media
  names are byte-identical, member for member.

## Q3 (not blocking): tyre model

- **Spec:** FORMAT-CARS "Wheels" and the Tests table ("Which tyre model a
  car uses is not known; the stock XKR-S drew `TireC` with the `A`
  sidewall and tread textures").
- **Engine:** the sizes come from `Data_Car` `FrontTireWidthMM`,
  `FrontTireAspect`, `FrontWheelDiameterIN` and the `Rear*` columns, and
  every car draws `TireC` with the `TireA` textures. The rim's axle scale
  is the axle's tyre width over the front's, and the tyre's axle scale
  width × 1.033.
- **Changed from the spec's M1 rule:** the rim's radial scale is the rim
  radius over the rim model's own LOD0 outer radius, not over 0.194.
  FORMAT-CARS notes that only 95 of 176 stock rims are normalised to
  about 0.194 m. `ALF_8C_08`'s rim is authored at 0.273 m; scaled by
  rim radius / 0.194 it overfilled the tyre, and the reference showed it
  at the rim radius. With the model's own radius its IoU went from 0.985
  to 0.991 (and `AST_DBS_09` from 0.989 to 0.992). The showroom notes
  every car whose rim is not at 0.194 m.

## Q4 (not blocking): which part variants the showroom draws

- **Spec:** FORMAT-CARS lists the models but not which of a part's
  variants a stock car shows. The LOD0 models hold sets such as
  `bumperFa`..`bumperFd`, `bumperRa`..`bumperRd`, `skirtLa`..`skirtLc`,
  `hooda`/`hoodb`, `winga`/`wingb`, and upgrade parts ending in `race`
  (`bumperFrace`, `wingrace`, `cagerace`). LOD0 also holds a rim named
  `wheel` at the origin.
- **Engine:** draws the `a` variant of each set and parts without
  variants, leaves out the `race` parts and LOD0's `wheel` (the wheels
  come from the wheel model at each centre). `--all-variants` draws
  everything. Silhouette IoU will show where this is wrong (a stock car
  with a `b` bumper, say).

## Q5 (not blocking): what the cockpit model adds to the showroom

- **Spec:** FORMAT-CARS names `<car>_cockpit.carbin` but not which of its
  parts the outside view draws. On the Abarth the LOD0 model has no hood,
  windscreen, headlights or cabin; the cockpit has them, plus copies of
  the side glass and mirrors, the A-pillars and three positions of each
  gauge needle (`_min`, `_mid`, `_max`).
- **Engine:** draws every cockpit part whose name the LOD0 model does not
  have, the needles at `_min` only.

## Q6 (not blocking): stand-in LOD0 and cockpit models

- **Observed:** some models hold stand-ins instead of geometry: cubes of
  exactly 17 vertices about 0.2 m across (506 such pools over the 240
  cars and their wheels), quads of at most 6 vertices, boxes of at most
  24 vertices on their bounding-box corners, and LOD0 pools with under a
  quarter of the LOD1-5 pool's vertices (some rims). FORMAT-CARS does not
  mention them.
- **Count (all 240 cars):** 14 cars have no real LOD0 body
  (`AUD_A4Avant_04`, `BUS_TOUR_12`, `CHE_Express2500Cargo_05`,
  `DOD_Durango_99`, `FOR_Explorer_10`, `FOR_F350SuperDuty_08`,
  `FOR_FocusZX4_05`, `GMC_YukonXL_01`, `KEN_T440_12`, `NIS_Leaf_11`,
  `TOY_Camry_07`, `TOY_CorollaDX_95`, `TOY_RAV4Sport_04`,
  `VOL_XC70_12`); 6 have stand-in parts next to real ones
  (`CHR_PTCruiser_04` 23, `VW_Beetle_04` 18, `TOY_Prius_11` 13,
  `FOR_TaurusSHO_10` 12, `NIS_240SX_94` 3, `FER_599XX_10` 2).
- **Needed:** does the game's showroom draw the LOD1-5 model for these
  cars or parts? None is among the reference cars yet.
- **Engine:** does not draw stand-in pools; draws each such part from its
  lowest real LOD, and the whole LOD1-5 exterior when LOD0 has no real
  part. Where a part has variants `a`-`h`, the first with real geometry is
  drawn (the Prius's are in `e`).

## Q7 (not blocking): calliper angle and wheel mirroring

- **Spec:** FORMAT-CARS gives `CaliperAngle` per axle and the calliper
  models per corner, but not the axis or sign of the angle, and not which
  side of the rim model faces out.
- **Engine:** callipers and rotors sit on the hub centre unrotated (a
  note on every car); the rim model's face is taken to point at -X (the
  left side), so the right-hand wheels are mirrored, which matches the
  Abarth by eye.

## Q8 (not blocking): which UV set slot 1 uses, and which techniques are paint

- **Spec:** FORMAT-CARS's slot table names the slot-1 sheets (interior,
  lights, carbon fibre) but not their UV set, and the paint rule names
  `body` and `body_2`.
- **Engine:** samples slot 1 with UV0 except carbon fibre (UV1), and
  paints `body` and `body_2` only.

## Q9 (not blocking; correction to a confirmed fact): wheel-centre z sign

- **Spec:** FORMAT-CARS "Wheels": `CarPartPositions` `PartId` 5 to 8
  are the wheel centres, "5 left rear, 6 right rear, 7 right front,
  8 left front", and "unlike the lamp points, z is not negated" (rear
  at +z), marked confirmed.
- **Observed:** on the cars whose axles are not centred, the references
  put the axles the other way round. `FIA_500AbarthSS_10` (car 1115)
  has rows at z = -1.256 (7, 8) and +1.043 (5, 6) with a body from
  -1.70 to +1.74: read as written, that gives a 0.44 m front overhang
  and 0.70 m rear, the reverse of the real front-engined car, and the
  render's wheels sat about 0.2 m forward of the capture's. The
  rear-engined `FIA_595_68` (1545) was off the other way. The spacing
  check in the spec cannot see this: the wheelbase is the same either
  way.
- **Likely cause:** the spec session reports (after `ebecd64d`) that the
  references' `car_to_world` holds a z mirror between the decoded
  `.carbin` positions and the body, so `clip_from_car` applies to
  `.carbin` positions directly. If the `CarPartPositions` rows are in the
  mirrored (body) frame, they need z negated to sit with the `.carbin`
  positions, which is what the captures show.
- **Engine:** negates z, as for the lamp points, so rows 5 and 6 are the
  front pair (LF 5, RF 6, LR 8, RR 7). The 500's IoU went from 0.953 to
  0.989, the 595's from 0.949 to 0.989, and every reference car is at
  or above its earlier score. Front and rear sizes follow the swapped
  rows.

## Q10 (blocks the M1 numeric pass for one car): ALF_ZagatoTZ3Stradale_12 (car 1656) at IoU 0.968

- **Observed:** IoU 0.968 after Q3 and Q9; the body matches, the wheels
  do not. The capture shows a large rim in a thin tyre, the engine a
  smaller rim in a taller tyre that reaches lower and further out.
- **Measured:** the references' `checks.model_to_body` lifts the model
  by a height that, on seven of the nine cars, equals the Data_Car tyre's
  outer radius less the wheel centre's height to within 1 mm, i.e. the
  ground under the stock tyres. On the TZ3 the lift (0.0787 m) puts the
  ground 0.016 m inside the front tyre (295/30 R18, outer 0.317 m) and
  0.030 m inside the rear (345/30 R19, 0.345 m): the game draws smaller
  tyres than those columns give, about 0.301 and 0.315 m. (`AST_DBS_09`
  and `AST_Virage_12` are 0.02 m inside as well, and still score 0.99.)
  The wheel model's own LOD1 tyre has a 0.261 m rim and 0.331 m outer,
  a ratio near the capture's.
- **Needed:** where the game takes this car's wheel sizes from, if not
  the `Data_Car` front and rear columns (`List_Wheels`, an upgrade or
  wheel table, `WheelNormOffsets`), and whether a wheel is offset
  sideways (the rear centres at x = +-0.848 m put a 345 mm tread 6 cm
  outside the body).
- **Engine:** Data_Car sizes and the row's centre, no offset.

## Q11 (blocks the M1 numeric pass): glows are in the mask

- **Spec:** AUTOSHOW_REFERENCE's mask counts every pixel a car-only
  replay changes by 6/255 or more; the faint glow halo round the body
  drops out, but the captures show bright glows that stay in: round each
  lit headlamp, and on `AUD_RS4_06` a flare above the roof where the
  stage's light catches the window frame.
- **Measured (30 references, 2026-11-19):** six cars are under 0.97
  besides the broken capture below, and on each the largest connected
  reference-only region of the mask difference is such a glow. Without it:
  `AST_DBS_09` 0.957 -> 0.990, `AST_Vanquish_13` 0.960 -> 0.982,
  `BMW_M3E92_08` 0.966 -> 0.984, `BEN_CONTINENTALGT_11` 0.966 -> 0.977,
  `AUD_RS4_06` 0.965 -> 0.992, `ALF_ZagatoTZ3Stradale_12` 0.968 -> 0.973.
- **Needed:** what draws the glows (a sprite or flare per lamp point, or a
  glare pass on bright highlights; with the car's matrix?), their size
  and colour, and whether lamp points 0 to 3 of `CarPartPositions` place
  the lamp ones. Or, if they are not part of the car, whether the mask
  should leave them out.
- **Engine:** draws no glow.
- **Also:** the `AUD_RS4Avant_13` (1681) capture's car-only image holds a
  black band and the stage's purple floor lights, so its mask covers
  121,633 px that are not the car (IoU 0.785; 0.998 without that region).
  Reported to the capture side.

## Q12 (not blocking): holes in the captured masks

- **Observed:** the 6/255 rule leaves out car pixels whose colour is
  within 6/255 of the empty frame's grey (95, 95, 95): mid-grey grilles,
  vents and trim on `AST_One77_10` show as holes inside the reference
  mask (its edge overlap is 0.26 against 0.47-0.82 for the rest).
- **Engine:** counts every fully covered pixel and applies the 6/255
  rule only to partly covered silhouette pixels, so shading near the
  background grey (a material difference) does not cut holes in our mask.
  The holes cost the One-77 some IoU and most of its edge score.
