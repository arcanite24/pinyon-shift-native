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
  the colour pass's MSAA and counts any resolved coverage, and prints IoU,
  masked PSNR and the edge-overlap score (reference edge pixels with an
  engine edge pixel within 1 px; the plan reports it without a threshold).

## Q2 (not blocking): which copy of a car present on the disc and in a package

- **Specs:** FORMAT-CARS "Rule" says take the disc copy first;
  AUTOSHOW_REFERENCE "Some models exist twice" says a bake should prefer
  the package copy and report a difference.
- **Engine:** the bake takes the disc copy, hashes every package copy of
  the same media name, and reports any difference. For the official set
  FORMAT-CARS says they are identical, so the choice changes nothing.

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

## Q10 (not blocking): ALF_ZagatoTZ3Stradale_12 (car 1656) is under 0.97

- **Observed:** IoU 0.968 after Q3 and Q9. The rest of the body matches.
  Two places differ:
  - its rear wheel-centre rows are at x = ±0.848 m (front ±0.809); with
    the 345 mm rear tyre the tread reaches 1.026 m, about 6 cm outside
    the 0.97 m body, where the capture's tyre sits inside the arch.
  - the capture shows the roof higher over the rear side windows than
    any part the engine draws.
- **Needed:** whether the wheel draw offsets the wheel sideways
  (FORMAT-CARS lists `WheelNormOffsets`, `WheelExceptions` and
  `List_Wheels.BrakeDrawOffset` with unknown meaning), and what the
  roof is (a part variant, or a part from another model).
- **Engine:** places the wheel at the row's centre with no offset.
