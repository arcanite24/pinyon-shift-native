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

## Q3 (answered in FORMAT-CARS f193075f): rim and tyre fit

- **Asked:** which tyre model a car draws, and how rims are sized (the
  engine had scaled each rim radially by the rim radius over its own
  authored radius).
- **Answer:** rims are never scaled radially. A rim with a LOD0 morph
  target (160 of 240) is drawn as base + w x (target - base) with
  w = (D - 14) / 10; the rest as authored. Across, the rim is scaled by
  the axle's tyre width over the header's design width (bytes 36 and 44).
  The tyre models share one geometry with two targets (outer radius at
  (outer - 0.2850) / 0.1301, inner at the rim's weight), scaled across by
  the rim scale x the rim's x extent (bytes 32 and 40) and centred on it.
- **Engine:** as specified; the targets are decoded at bake time (xyz x w
  in metres, about the part's offset) and applied per axle when the car
  is loaded. See Q13 for the rims whose x extent is empty.

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

## Q7 (half answered): calliper angle and wheel mirroring

- **Answered (FORMAT-CARS f193075f):** right wheels are the rim and tyre
  models turned half a turn about y, not mirrored; left wheels are drawn
  as authored. The engine does this.
- **Open:** the axis and sign of `CaliperAngle`. Callipers and rotors sit
  on the hub centre unrotated (a note on every car).

## Q8 (not blocking): which UV set slot 1 uses, and which techniques are paint

- **Spec:** FORMAT-CARS's slot table names the slot-1 sheets (interior,
  lights, carbon fibre) but not their UV set, and the paint rule names
  `body` and `body_2`.
- **Engine:** samples slot 1 with UV0 except carbon fibre (UV1), and
  paints `body` and `body_2` only.

## Q9 (answered in FORMAT-CARS f193075f): wheel rows and centres

- **Asked:** the `CarPartPositions` wheel rows read as written put the
  axles the wrong way round.
- **Answer:** rows 5-8 are axle positions in the body frame, +Z forward
  (5 LF, 6 RF, 7 RR, 8 LR); only their z places a wheel. The centre in
  the body frame is x = +-(track outer - tyre width) / 2, y = the tyre's
  outer radius, z = the row's z, mapped to the model frame by the inverse
  of the model-to-body pose (z mirrored, lift = mean stock ride height -
  `Data_CarBody.BottomCenterWheelbasePosY`, pitch = atan(ride height
  difference / `ModelWheelbase`)).
- **Engine:** as specified. The pose matches the references'
  `checks.model_to_body` on the reference cars.

## Q10 (answered in FORMAT-CARS f193075f): ALF_ZagatoTZ3Stradale_12 wheels

- **Asked:** the TZ3's tyres looked smaller in the capture than the
  `Data_Car` sizes give.
- **Answer:** the sizes are right; the row's y had put the wheel too low.
  With the centre rule of Q9 the TZ3 scores IoU 0.990 (from 0.968).

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
- **Now (25 v1 references, wheels by FORMAT-CARS f193075f):** only
  `BEN_CONTINENTALGT_11` is under 0.97 (0.967; 0.979 without its glow).
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

## Q13 (not blocking): rims whose header x extent is empty

- **Spec:** FORMAT-CARS "Wheels": the tyre's lateral scale is the rim's
  lateral scale x the rim's x extent (header floats at bytes 32 and 40),
  centred on it.
- **Observed:** 18 stock rims hold -FLT_MAX at byte 32 and +FLT_MAX at
  byte 40 (an empty extent) while their design faces (36 and 44) are
  finite: `aud_a4avant_04`, `bus_tour_12`, `che_express2500cargo_05`,
  `chr_ptcruiser_04`, `dod_durango_99`, `for_explorer_10`,
  `for_f350superduty_08`, `for_focuszx4_05`, `for_taurussho_10`,
  `gmc_yukonxl_01`, `ken_t440_12`, `nis_leaf_11`, `toy_camry_07`,
  `toy_corolladx_95`, `toy_prius_11`, `toy_rav4sport_04`, `vol_xc70_12`,
  `vw_beetle_04`: the 14 cars of Q6 with no real LOD0 body and four of
  the six with stand-in parts.
- **Needed:** how the game sizes these cars' tyres across.
- **Engine:** uses the design faces in place of the extent and notes the
  car. `ult_gtr_12` (no finite design width) is drawn unscaled across.

## Q14 (not blocking): the windscreen band and other opaque glass techniques

- **Spec:** FORMAT-CARS "Paint": glass draws take no paint and do not
  sample the body atlas; the windscreen's tinted band is the glass
  techniques' own, shade not given.
- **Engine:** an opaque sub-section whose technique names glass
  (`black_glass`, the windscreen's top band) and whose slot 0 is the body
  atlas is drawn flat dark grey (linear 0.02) without the atlas. Needed:
  the band's shade, and whether `black_glass` is the band.

## Q15 (not blocking): reference schema v2

- **Observed:** four references (1081, 1181, 1681, 1683) are now
  `shift-engine.m1-reference.v2`, with `checks.glow_px` and
  `checks.holes_filled_px`; AUTOSHOW_REFERENCE on `recomp-research` still
  describes v1 only.
- **Engine:** reads v1 only, so the report scores the 25 v1 references.
  It will read v2 once the spec defines it.
