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

## Q6 (answered): stand-in LOD0 and cockpit models

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
- **Answer (spec session, 2026-11-19):** the 14 cars are traffic or
  non-selectable cars (mostly among `car-list.json`'s 23 non-selectable
  rows), so the Autoshow never shows them and no reference covers them.
  Keep the engine's default.
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

## Q11 (answered in AUTOSHOW_REFERENCE v2, 86590f50): glows in the mask

- **Asked:** the v1 masks held lamp glows and highlight glare the engine
  does not draw, which put six cars under 0.97.
- **Answer:** the lamp glow is the car's additive draws (ONE + ONE, no
  depth write); v2 masks are built without them (`checks.glow_px`,
  `checks.mask_draws_left_out`), and glare more than 32 px outside the
  car's projected box is cut (`checks.mask_outside_box`). The car-only
  image keeps the glow. The glow and the post chain's glare are polish
  (bloom over HDR highlights; SHOWROOM_LIGHTING), not part of M1.
- **Engine:** draws no additive glow, so its mask already matches v2's.
  The report scores v2 references only; IoU-g stays as a report column.

## Q12 (answered in AUTOSHOW_REFERENCE v2): holes in the captured masks

- **Answer:** v2 fills enclosed holes where the full stage changes with
  the car present (`checks.holes_filled_px`; 4,342 px on the One-77). A
  near-grey part on the silhouette's edge can still be missed.
- **Engine:** unchanged: every fully covered pixel counts, the 6/255 rule
  only at partly covered silhouette pixels.

## Q13 (closed as polish): rims whose header x extent is empty

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
- **Decision (spec session):** keep the default; not worth more spec time.

## Q14 (closed as polish): the windscreen band and other opaque glass techniques

- **Spec:** FORMAT-CARS "Paint": glass draws take no paint and do not
  sample the body atlas; the windscreen's tinted band is the glass
  techniques' own, shade not given.
- **Engine:** an opaque sub-section whose technique names glass
  (`black_glass`, the windscreen's top band) and whose slot 0 is the body
  atlas is drawn flat dark grey (linear 0.02) without the atlas. Needed:
  the band's shade, and whether `black_glass` is the band.
- **Decision (spec session):** keep the default; not worth more spec time.

## Q15 (answered): reference schema v2

- **Answer:** AUTOSHOW_REFERENCE 86590f50 defines
  `shift-engine.m1-reference.v2`: the same fields, a new mask (Q11, Q12).
- **Engine:** reads v1 and v2; `tools/m1-report.py` scores v2 only and
  lists the v1 leftovers (`--include-v1` scores them too). Masked PSNR is
  reported, not part of the pass (playable first, polish later).

## Q16 (not blocking): which Colorado geometry the main view draws near the car

- **Spec:** WORLD-GEOMETRY selects world-space models by header box. It
  names TERR_CUBE models as drawn in reflection views, and leaves open
  which LOD the game draws.
- **Engine (M3 stand-in, `libs/world`):**
  - draws every world-space model within 600 m, at the lowest LOD of
    each object family among them;
  - leaves out TERR_CUBE, `UberLOD`, `MIDDIST`/`middistance` objects and
    the `Occluder` material;
  - places props within 250 m, with LOD00 nearer than 80 m and LOD02
    beyond.
- **Needed:** the game's draw and LOD rule around the player, whenever
  M2's streaming comes up.

## Q17 (answered by COLLISION.md, 43346e1d): road and surface identity

- **Answer:** the ground is the `N.fiz` collision tiles, with a surface
  index per triangle into `surfaceTypes.xml`. The engine now drives on
  them (`world::Collision`), grip = FrictionScale (COLLISION.md's engine
  choice). The render-name classes below remain only for drawing colours
  and the test driver's road search.

- **Engine (M3 stand-in):** a triangle is road when its mesh object's
  name holds `_road`/`road_` (Colorado names roads
  `..._Road_LODnn_...`). Otherwise it is classed as terrain, water or
  other by object or material name, with a placeholder grip each. Thin
  upright objects (armco, barriers, railings, cables, fences, signs) and
  props are left out of the ground.
- **Needed:** COLLISION.md's surfaces and collision geometry. The stand-in
  sits behind `drive::Ground`, so it swaps out without touching the
  vehicle.

## Q18 (answered by COLLISION.md, 43346e1d): a spawn point

- **Answer:** the measured free-roam spawn (2002.96, 143.84, 960.00),
  heading 126.1 degrees; the engine drops the car onto the tiles there.

- **Engine (M3 stand-in):** the flat road triangle nearest the free-roam
  camera of WORLD_GEOMETRY's frame dump (game frame 1999.0, 962.9). The
  car faces the way that camera looked, and lands 0.4 m from it, 1.8 m
  below the camera.
- **Needed:** the game's free-roam spawn (festival) position and heading,
  if a spec covers it.

## Q19 (answered from existing specs): the drive's world frame

- **Answer:** the game frame (WORLD-GEOMETRY: the file frame with z
  negated). AUTOSHOW_REFERENCE's `car_to_world` negates the model frame's
  z, as FORMAT-CARS's model-to-body pose does, so the body frame is
  placed in the game frame by a rotation.
- **Engine:** `libs/world` negates z once, as it gathers. 0d4bb44 first
  drove in the file frame, which drew the scenery mirrored; the next
  commit fixed it.
- **Check:** a frame at the spawn, set beside the frame dump's capture,
  shows the gantry ahead and the barriers and flag on the right in both.

## Q20 to Q26 (answered in CAR_PHYSICS 398228b8): the first run against the oracle

The first run (a76cfa08, the engine's own clutch and gearbox) missed
0-20/40/60, the 1-2 shift and scenario B. The questions asked for the
launch clutch (Q20), the shift's timing (Q21), the clutch under braking
(Q22), the rear brake (Q23), the tyre's torque on the wheel under braking
(Q24), the ABS slip filter (Q25) and the turn circle's inputs (Q26).
398228b8 answers all seven ("Clutch and gearbox states", Brakes, ABS, the
turn circle's inputs). The engine now follows them:

- **Gearbox:** the spec's states (open, launch, engaged, clutch in, gear
  change, rev-match, clutch out). The launch controller has the spec's
  entry rule, its falling H/L target, the servo
  `dc/dt = 53 (target - (rpm + 0.04 rpm')) / redline` and its exit
  (output past the target, driven-wheel slip under 0.7). The power is cut
  in clutch in and gear change. In clutch out the throttle is
  `lerp(driver, request, c)`. Clutch capacity is
  `(1 - c) x ClutchMaxTorque x GameTorqueScale`.
- **Braking:** the clutch stays locked; 2-1 on the way. Reverse is
  selected below MaxForwardVelShiftReverseMPH with the brake held, and
  then the pedals swap.
- **Brakes:** 3,900 N m front, 0.41 x that at the rear.
- **Tyre and ABS:** a slowed wheel takes TorqueFreeLongFrictionScaleBrake
  x GameFrictionScaleBraking (1.805) and puts F_x x r back on the wheel.
  The ABS tests the mean of five substep slips.
- **Turn circle:** full lock, full throttle to 3 m/s, then the coast.

Our choices inside the answers:

- the boost-like throttle build-up, a 0.28 s first-order lag of the
  trigger capped by the pedal, which fits the spec's 0.3 / 0.6 / 0.83 /
  1.0;
- the rev-match request, proportional over 200 rpm towards the output
  speed;
- the 2-1 under braking when the lower gear sits under 0.73 x the
  redline;
- the clutch opening when the output would drag the engine under 0.9 x
  idle;
- the profile road's inner points rounded over +-4 m. The linear profile's
  slope step at 60 m, where scenario B's brake lands, unloaded the
  massless front wheels.

### Results (car 445, spec defaults; ctest 7/7, the gates not fatal)

| Check | Engine | Oracle | |
|---|---|---|---|
| Launch state | 0.617-1.592 s, left at 28.5 km/h, locked 1.767 s | 0.51-1.42 s, 29.6 km/h, 1.58 s | late |
| 0-20 / 40 / 60 km/h | 1.315 / 2.113 / 3.063 s | 1.122 / 1.891 / 2.780 s | miss |
| 0-80 / 100 km/h | 4.793 / 6.137 s | 4.557 / 5.84 s | miss |
| 1-2: request, change, locked | 3.533, 3.850 s (67.7 km/h), 4.200 s | 3.20, 3.55 s (68), 3.91 s | late, durations match |
| B mean deceleration | 7.25 m/s^2 | 8.57 | miss |
| B ABS phase | 14.65 m/s^2, to reverse at +1.09 s, 24.1 km/h | 11.9-12.1, +1.31-1.36 s, 23 km/h | too strong |
| B braked steps | front 95 %, rear 45 % | 50-54 %, 74-81 % | |
| B reverse phase | 3.2 m/s^2 over 2.0 s | ~5 over 1.27 s | |
| B 2-1 gear change | +0.49 s, 58.4 km/h | +0.50 s, 59.8 km/h | ok |
| Turn circle | 3.344 m, coast 3.00 -> 2.32 m/s | 3.37 m, 2.84 -> 2.08 | pass |

## Q27 (answered, CAR_PHYSICS 67371030): the free-revving engine's inertia

Answer: J = 0.145 (0.71 x the rows' engine + flywheel), engine braking
0.71 x ZeroThrottleTorqueScale, clutch capacity factor 0.72 (measured
fits). Implemented in `from_physics.cpp`. Results (R34):

| Check | Engine | Oracle |
|---|---|---|
| Launch state | 0.492-1.500 s, left at 28.0 km/h; locked 1.675 s at 31.9 km/h | 0.51-1.42 s at 29.6 km/h; locked 1.58 s at 32.7 km/h |
| 0-20 / 40 / 60 km/h | 1.226 / 2.023 / 2.925 s (miss) | 1.122 / 1.891 / 2.780 s |
| 0-80 / 0-100 km/h | 4.620 / 5.943 s (ok) | 4.557 / 5.84 s |
| 1-2 request / change / locked | 3.367 / 3.683 s at 67.9 km/h (miss) / 4.033 s | 3.20 / 3.55 / 3.91 s |

The launch is still about 0.1 s late below 60 km/h; 80 and 100 pass.

## Q28 (answered, 67371030): the front tyre's braking grip

Answer: the spec's onset table (front left, run B1). With the per-wheel
F_x now in the trace (`fx0..fx3`), ours over the same window:

| t (s) | brake (N m) | slip | F_x | load | F_x / load | oracle slip, F_x, load |
|---|---|---|---|---|---|---|
| 0.000 | 0 | +0.05 | +937 | 2,962 | | +0.06, +1,050, 2,500 |
| 0.017 | 1,610 | -0.17 | -3,723 | 3,118 | 1.19 | -0.23, -3,400, 2,600 |
| 0.033 | 2,646 | -0.47 | -5,981 | 3,497 | 1.71 | -0.63, -5,150, 2,900 |
| 0.050 | 3,900 | -1.09 | -6,915 | 3,865 | 1.79 | -1.50, -6,070, 3,400 |
| 0.067 | 3,900 | -1.92 | -7,455 | 4,252 | 1.75 | -2.37, -7,200, 4,200 |
| 0.083 | 3,900 | -2.66 | -7,949 | 4,635 | 1.71 | -3.20, -7,200, 4,300 |
| 0.108 | 0 (released) | -2.77 | -8,970 | 5,169 | 1.74 | -1.51, -8,080, 4,600 |
| 0.167 | 3,900 | -0.42 | -9,160 | 5,444 | 1.68 | -1.17, -8,480, 4,800 |

Our 1.805 is applied once and F_x / load peaks at 1.79. One fix came
from the 0.106 row: the oracle's released front keeps F_x / load 1.76,
so a wheel counts as slowed while the brake pedal is held, through an
ABS release (before: only while its brake torque was applied, which
gave 1.1 on released steps). The first release is at 0.108 s.

## Q29 (answered, 67371030): the rears under braking

Answer: open centre differential, 70 % rear, the same under braking;
the locked clutch feeds the rears. Ours already split it so. Checks:
the engine is pulled from 5,660 to 4,271 rpm by 0.083 s (oracle 4,240
by 0.085 s); the rears stay within 3 % of the ground speed until
0.075 s (oracle 0.085 s) and first reach their release point at
0.208 s (oracle 0.25 s).

## Q33 (answered, CAR_PHYSICS 241331e7): the load transfer under braking

Answer: CMHeight is 0.47-0.48 m above the ground (confirmed); the tyres'
longitudinal forces pitch the body through a 0.27 m arm (measured fit);
the rear brake is 1,750 N m = 0.45 x the front; in the reverse launch the
centre differential's drive-side lock (`CenterLimitedSlipTorqueAccel`,
255) moves torque to the fronts (probable). Implemented:
`longitudinal_pitch_arm` 0.27 (the force's moment taken at a point
raised along the body's up axis), rear brake 0.45 x front, and a
clutch-type limited-slip centre: up to the lock x the final drive at the
wheels, never more than would bring the axles' mean speeds together in
the substep (ours; `RelVelClamp` and `AccelDefInputTorque` are not
decoded).

Our B1 loads now sit in the oracle's band: fronts 5,100-5,800 N
(oracle 4,600-5,955), rears 1,500-2,300 N (1,358-2,709).

| Check | Engine | Oracle |
|---|---|---|
| B mean deceleration | 8.016 m/s^2 (-6.5 %, miss) | 8.57 |
| ABS phase | 13.19 m/s^2 (+10 %) | 11.9-12.1 |
| Braked steps front / rear | 71 / 70 % | 50-54 / 74-81 % |
| Reverse selected | +1.225 s, 23.6 km/h | +1.31-1.36 s |
| Reverse phase | 3.99 m/s^2 over 1.575 s | ~4.9 over 1.26-1.29 s |
| Turn circle | 3.372 m (pass) | 3.37 m |

## Q34 (polish): the reverse launch's drive torque, the front ABS duty, the early launch

Moved to polish under the playable-first rule (OpenFH1, after 241331e7).
The M3 physics gate (`physics_tests`, fatal unless PHYSICS_GATE=0) is
0-80 and 0-100 at 2 %, the turn circle at 2 %, B's mean at 8 % and its
ABS phase at 12 % (of 12.0 m/s^2). Report-only: 0-20/40/60, the 1-2
change, and B's mean at 2 %. Open:

0. The launch is about 0.1 s late to 20, 40 and 60 km/h (1.236 / 2.034 /
   2.934 s against 1.122 / 1.891 / 2.780 s), and the 1-2 change 0.16 s
   late (3.708 s against 3.55 s).

1. Our reverse launch is clutch-limited: at engagement 0.41-0.50 (c
   0.50-0.59, the oracle's 0.43-0.58) the capacity is
   0.45 x 977 x 0.72 = 316 N m, x 3.28 x 3.545 = 3,680 N m at the
   wheels, about 11 kN of tyre force. Ours makes 10.5-11.8 kN (5.9 m/s^2
   over 1.1 s from 6.7 m/s), the rears at a slip of only -0.1 (the
   centre lock holds the axles together). The oracle's front pair alone
   makes 7,000-12,500 N and the spinning rears 5,000-9,000 N more, which
   the same clutch capacity cannot pass. Needed: the oracle's clutch
   torque (or the engine's net torque) in the reverse launch, or whether
   the reverse path has its own ratio or capacity.
2. The coast before it is 0.47 s (oracle 0.38 s): our engine starts it
   at 1,544 rpm (oracle 1,940).
3. Our fronts are braked on 71 % of steps in the ABS phase (oracle
   50-54 %). After each release (6 steps, as the oracle's) they spin up
   to a slip of about 0 and then take 9-21 steps to reach the release
   point again, the longest while the clutch is locked in 1st (the
   engine's inertia on the wheels). The oracle's 0.106 s row has a
   released front at only -1.51. Needed: a few of the oracle's front
   ABS cycles (slip per step over one release and reapply) in 1st with
   the clutch locked.

## Q30 (polish, M1): the F40 Competizione's rear window

FORMAT-CARS has `lexanR` (`window`) lying flat over the engine deck,
between the roof and the wing. The engine draws it (LOD0, the transparent
pass, two-sided, counted in the mask), but in our bake it does not lie
flat. It slopes from the roof down to the tail (x -0.59..0.59, y 0.69..0.95,
z 0.82..1.94 m in car space), inside the trunk's rubber trim (the same
bounds). From the reference camera it lies wholly inside coverage the
other parts already give: drawing it or leaving it out changes no mask
pixel. The reference's extra region is the triangle above it, from the
roof's rear edge to the wing's top. Needed: the game's world-space bounds
of `lexanR` in capture 1023 (or the draw that covers that triangle), to
tell a wrong vertex read from a missing transform.

## Q31 (M1): `liveryBadge`

The Challenger SRT8 392's lighter patch on the door and rear quarter
comes from `body`'s `liveryBadge` sections (also on `bumperRa`). They
sample opaque white atlas texels. The technique is in neither of
FORMAT-CARS's technique tables. The engine now leaves it out unless
`--livery` is given, which removes the patch (the mask is unchanged:
it sits on the body). Is `liveryBadge` drawn without a livery, and with
what?

## Q32 (not blocking): part-throttle upshifts

The spec traced the automatic box at full throttle only (up at 0.96 x
the redline). Cruising at part throttle, that kept the R34 in first at
55 km/h and 6,000 rpm. Ours: up when the engine passes
lerp(0.5, 0.96, throttle) x the redline, if the next gear keeps it over
0.4 x the redline (the lugging downshift sits at 0.35). Full throttle is
unchanged. Needed if the play test calls for it: the game's part-throttle
shift points.

## Q35 (polish): the engine sound's open items

Built from ENGINE_SOUND.md (14243a1c); the R34's table checks out at
4,000 and 7,000 rpm (audio_tests), and its 37 samples bake to their
declared lengths. A headless sweep's dominant tone tracks the rpm to
within 1 % of a fixed ratio from 4,000 to 7,350 rpm. Left open:

- The NegTorque sign: the spec leaves whether braking torque raises or
  lowers a parameter; ours lowers it (`EnginePlayer::neg_raises` false),
  with no switch on the command line yet.
- Not applied: the harmonic tunings' parametric EQs, the load EQs and the
  expander. The groups play with their gain and one-pole low-pass only.
- The listener's pan: our fold of the views' channel levels to stereo,
  L = Left + 0.7 Centre + 0.7 rear Left (R likewise), scaled so the
  larger is 1. The game's own downmix would replace it.
- The master gain 0.6 is ours, set by the mix's peaks (0.22 on the R34's
  full-throttle sweep). The game's overall level against other sounds is
  not known.
- The windowed playback (SDL audio stream, about 60 ms queued) has not
  been heard yet: the play test checks it.

## Q36 (polish): the world materials' first pass

Built from WORLD_MATERIALS.md (d410dd92, 52d48a6f, 6521547d). Over
bin.zip (`fh1data-corpus --world-materials`): all 14,291 models parse with
47,749 materials; 14,193 hold one uvOffsetScale per submesh, 98 none; every
submesh's (effect, stride) is in the stride table or its exceptions; the
texture records are 5,234 / 2,965 / 11,409 / 5 by type and the slot
outcomes match the spec's table; every `.bix` pair fits the layout, and
2,962 CAFF textures do (the spec's three 196,604-byte ones do not).

Found while doing it, for the spec:

- **Not every world texture is a power of two.** 12 CAFF textures are
  not (`_0x00000D63`, `_0x000010F2`, `_0x00001EE4`, `_0x00001EF2` to
  `_0x00001EF9`, `_0x00004578`): nine 768 x 256 and one 768 x 128 DXT1
  with 10 levels, one 255 x 256 and one 7 x 256 with 9 levels. All 12 fit the `.xds` layout exactly with the mip regions
  sized from the power-of-two rounding (a 768-wide level 1 is stored as
  if 512 wide), the last region trimmed. The 255 and 7 widths may be a
  misread field; they decode to the layout's size either way.
- The all-tail CAFF case: 17 CAFF textures have a side of 16 or less;
  the full-region rule (as the spec says) changes the size of 8 of them,
  `_0x0000203A` (32 x 16) and seven of 64 x 8 to 256 x 16.

Engine choices, open for the spec:

- **AO.** Not applied: which slot holds `AO` / `AOMap` depends on the
  effect, and this side does not read `.fxobj`. Needed: the effect to
  sampler-slot table (`world-materials.py samplers`), or the AO slot per
  effect family.
- **Terrain layers.** `Blend_A` alone, at TEXCOORD0 with no per-layer
  tiling: hills read as uniform grey rock (terrain UVs repeat about every
  50 m). That the missing layers are the cause is probable, not shown:
  the base levels of the main `H_BLND*` albedos (records 11180, 4314,
  11408) were not checked for colour spread. Needed for a second pass: which pixel constants are the layer
  tilings, which UV `Splat` uses, and the blend rule.
- **Cut-outs.** Alpha to coverage on `H_DIFF*` and `TREE_*` (the frame's
  families) and also on other effects named `_OPAC`
  (`H_VBLND_NORM_MASK_OPAC_BIAS_LM` has cut-out alpha; no draw of it was
  in the frame). The `_OPAC` rule also catches `ADD_DIFF_OPAC_RGBA`,
  additive by its name; it draws as a cut-out until the blend is known.
  Blended decals use the texture's alpha times the vertex colour's A.
- **Window streaming.** With `--async-stream` the textured frames match the
  synchronous ones until a regather; the background gather's textures
  draw once it swaps in (checked headless over a 40 s cruise).
- **Normals.** The authored normal is used on both sides. Turning it to
  the eye (the car shader's two-sided rule) darkened every surface above
  the eye.
- **Lighting.** The recipe's sun and two-colour sky, exposure 1, no fog;
  the sun colour's w (20) is not used.
- **Props in the reference frame.** Answered (WORLD-GEOMETRY, "Which
  placements free roam draws", recomp-research a818c127): a `.pvsz`
  record's or kind-7 tile's route list; free roam (route 0) draws a
  placement with no list or with 0 in it, and never the
  `Temp_Arrow_Pointer` marker (a guess from one marker). In the frame
  this leaves out 232 placements and the marker: the gantry, the
  grandstand and its banners, the concrete barriers and the striped box
  are gone, the crowd barriers and the flag stay. The zone choice is
  ours (the union of every zone); the group's first distance as a
  per-instance cut-off for kind-7 models is not applied yet.
- **Upload.** The streaming worker stages the textures and makes their
  images; the old scenery is freed off the render thread. A regather's
  hand-over is about 1 ms on the render thread.

## Q37 (polish): the far ring and the sky

The hills of the reference frame were flat grey shapes on a flat blue sky.
The near gather skips every object named TERR_CUBE, UberLOD, MIDDIST or
OCCLUDER (the WORLD_GEOMETRY selection). Ours, until the spec says which
models draw the hills, the distant tree cover and the sky:

- **The far ring.** The MIDDIST and UberLOD objects (the model index marks
  the 406 models that hold them) of the models whose header box is within
  3 km, finest LOD present per family, drawn with their own slot-0 texture.
  Their triangles wholly within 400 m of the gather point are left out
  (UberLOD: 600 m), and the rest are pushed away from the eye in depth (3 %
  for MIDDIST, 6 % for UberLOD), so the near terrain wins where both are
  drawn. At the reference camera: 117 models, about 99,000 triangles.
- **Left out of it.** TERR_CUBE objects (road and diffuse effects over the
  terrain's footprint; by the name, the cube maps' stand-ins) and UberLOD
  `_CAGE` objects (tall walls round a patch, untextured: they hid the
  sky). Some far objects use H_DIFF_1 with no slot-0 file
  (`Zone2_Area3_6_middistance_*`, several UberLOD patches); they draw flat.
- **The sky.** A gradient by height from a pale horizon to blue, ours,
  behind everything. Needed: the sky's model or effect, if the game draws
  one.
- **Far plane.** The drive camera's far plane is 16 km (was 4 km).
- **Census.** `fh1data-corpus --render-models bin.zip --objects` lists
  every mesh object with its model's box and effects.

## Q38 (polish): terrain layers after WORLD-MATERIALS 1c08e550

Built from the spec's Terrain shading section:

- **Layers.** `H_VBLND*` (not `DECAL`): lerp(`Blend_A` at TEXCOORD0 x
  c0.xy, `Blend_B` at TEXCOORD0 x c0.zw, w); `H_BLND2*` and `H_BLND3*`:
  the same layers, w = `Splat`'s R at TEXCOORD1 x c1.zw; `H_BLND1*`:
  `Blend_A` at c0.xy. `Blend_B` is slot 1 and `Splat` slot 2: slot 1
  resolves to `_0x000036A1` on the `MIDDIST` hills, as the spec names.
  `Blend_C` is not drawn (its weight is open). At the reference camera,
  28 vertex-blended materials and 90 splat materials.
- **COLOR.x is the first stored byte.** With the second stored byte (R
  in A, R, G, B) the road-paint decals (`H_VBLND_DECAL_MASK_BIAS_LM`,
  c4.x 3.5 to 10) fade to nothing: their R is about 0 and their first
  byte about 234. With the first byte they draw where the frame shows
  them. So w = saturate((first byte / 255 - 0.5) x c4.x + 0.5) for the
  hills' weight and the decal fade (ours, from that evidence; the spec
  calls the byte order probable). On the hills c4.x is 1 and the first
  byte is mostly 255: they draw mostly `Blend_B`.
- **AO** stays off (type-1 slots: AO = 1, as the spec says; the per-model
  AO maps are not applied yet).
- **Brightness.** Vertex-blended materials take a light gain of 2.5
  (the spec: scale the hills to the reference). Hill luma medians at
  the reference camera, game / ours: 81 / 57, 61 / 42, 59 / 56 (34, 28,
  34 before).
- **Non-power-of-two CAFF.** The decoder already takes ceil(width / 4)
  blocks per row; the upload keeps the stated size, so the crop is the
  image's own.
- **Trees.** Not drawn: the billboard record's fields are open
  (WORLD-MATERIALS open question 8). On `__R00G07021.pgeo`'s trailing
  block (361 records, tile box 304 x 148 x 309 m), the half floats at
  +24 read as (2.5 to 3.5, 5.7 to 7.8, -1000, 2500), which looks like a
  quad size and two distances (2,500 is the tile's +0x38), and +32 as an
  ARGB tint near (255, 124, 128, 132). No 32-bit word read as 10_11_11
  or 11_11_10 unsigned within the tile's box gives terrain-smooth
  positions (median height change to the nearest record 1.9 to 3 m per
  m); +4, +16 and +20 are near-constant. Needed: the position field and
  its frame (the draws' instance matrix?), the atlas cell.

**Answered** (WORLD-MATERIALS 47c7801e, "Tree billboards (engine
question Q38)" and "Colour byte order"). Built:

- **Trees.** The 40-byte records decode as the spec gives them: the
  position word is X, Z, height in 10_11_11 over the tile box (the box at
  +0x10 is in the game frame, like the instances; the earlier search
  failed because it read the third field as Z), then the trunk axis,
  normal, atlas cell, half width, height, near, far and tint. Each
  sprite is a quad turned to the camera about its trunk axis in
  world.vert, cut out by the atlas alpha with alpha to coverage, tinted
  2 x RGB. The atlas is the group key's entry of the tile's reference
  list. At the reference camera the 3,403 sprites within 600 m stand on
  the drawn terrain (3,401 within 1 m, median 0.06 m below) and use
  `_0x00005010` to `_0x00005013` and `_0x00005015`, as the spec's frame.
  Ours: only the file records (the generated records of placed
  instances are not made), sprites gathered within 2 km and their far
  distance of the gather point rather than per frame from the camera
  (the records looked at all have near -1000, far 2500), the sprite's light is
  the sun and sky on its trunk axis, and no grass.
- **Colour byte order.** The `H_VBLND` layer weight now comes from
  COLOR.x, the second stored byte; the decal fade stays on the first
  (COLOR.w). The decal's noise term is not drawn yet.

## Q39 (polish, M1): grille textures and per-car tyre sizing

- **Grilles.** FORMAT-CARS gives `grille1` the Shared `grille1_alpha` in
  slot 1 (one frame dump) and calls `grille1_alpha` alpha-tested, but not
  which UV set samples which slot, nor the slots of `grille1_alpha`,
  `grille2` and `grille2_alpha`. Every grille section's UV0 transform
  tiles 4 to 77 times (380 sections over the 240 models), which fits the
  64 x 64 Shared tile and not the atlas: sampling the atlas there drew
  coarse squares (the RX-8's front, for one). Ours: every grille
  technique takes its family's Shared `grilleN_alpha` in slot 1, the draw
  shows that tile on UV0, and the `_alpha` techniques discard texels under
  alpha 0.5. Open: the slots and UV sets of the four techniques, the
  alpha reference, and what slot 0 (the atlas) contributes.
- **Per-car tyres.** A model's own tyre set (`TireA<MediaName>`) is
  drawn. Its sizing is open (FORMAT-CARS, Wheels): it is modelled at the
  car's tyre radius (the FJ Cruiser's 0.401 m is its 265/75 R16) and,
  like the shared tyre, one unit wide. Ours: no outer-radius target, the
  shared tyre's rim target and lateral fit. Since then: the six sets are
  sized exactly as the shared tyre (both targets and the lateral fit), as
  the spec now gives it (d07a2840).
- **The FJ Cruiser's tyre (open, deferred as polish; M1 closed with it as
  an exception).** 1651 scores IoU-g 0.9629 (0.9685 with the shared tyre):
  the engine-only pixels ring the bottom and outer side of every tyre, so
  the reference's tyre is smaller in radius and narrower than ours. Checked
  and matching the dump: target A equals the base (no vertex moves more
  than 0.4 mm, `carbake --dump`); the body pose (lift 0.4466, pitch 0, the
  0.447 m ride heights); the wheel centres at body y 0.402 m; and the
  rim lateral 1.1791, tyre lateral 0.2697, centre offset 0.0072, wB 0.2,
  wA 0.897. Not yet compared: the drawn extremes of one tyre
  (`fj-tyre-check.json`).

## Q40 (M3): the oracle lap after "What the AI's controls mean" (d748dc77)

- **Done.** The lap runs the AI's controls from the start signal: the gas
  and brake unshaped, the road wheels at steer x SteerMaxAngle at once, the
  brakes linear at 1.59 x the full press, and the AI's ABS (ReleasePointABS
  on both axles, the latest slip, one step's release). For all cars: one
  brake unit is 1,200 N m (4,390 / 1,790 N m full press on the R34), the
  up-shift is at 0.945 x the redline, the braking down-shift at 0.8 x, and
  the part-throttle up-shift is the pad's only.
- **Placeholder: the pad's lock and rate at speed.** The lock is
  atan(1.61 x SteerSpeedSensitiveMaxGees x g x wheelbase / v^2), the
  measured 2,410 / v^2 degrees per unit on the R34; the turning rate
  scales with the lock's share of the full lock, down to a quarter (20 to
  38 deg/s measured at speed); centring keeps its rate. Open: the rule
  behind both.
- **The lap: DNF** (595 of 1,199 m in 77.6 s; 0-100 km/h 6.09 s against
  5.76). Split: +0.26 s at 100 m, +0.28 at 200, +0.27 at 300, +2.04 at
  400, +2.44 at 500. The first braking zone pulls -14.6 m/s^2 at brake
  0.39 to 0.45 (the oracle -15.1 to -16.7; the spec's check -15.0). The
  loss is the first corner (300 to 400 m): the front axle saturates. The
  oracle turns at 12 to 14 m/s^2 on 3 to 4 deg of road-wheel angle; ours
  reaches 5 to 8 m/s^2 on 8 to 15 deg, runs wide onto the grass at 340 m
  and never rejoins the line. That is the tyres' lateral force (or the
  front's load) and not the controls; deferred.
- **Closed (Slice 1, 2026-11-21):** With `CAR_PHYSICS.md` `5792a248` and
  `0077d463` implemented (direct `J = engine + flywheel`, `S_upg` + `RobScale`,
  turbo power-lag ODE + 15 Hz boost filter, `GameTorqueScale` wheel split,
  peak-power `T_closed`, immediate `kClutchIn` driveline unlock, `target_rpm`
  launch PID, Front/Rear/Center LSDs with rpm `RelVelClamp`, 6-iter per-car
  `init_brakes()` + direct `BrakeInputScale` + 10-pt decel spline, 3D
  `BodyAero*` + `List_AeroPhysics`, and surface `VelDepFriction` + `Bumpiness`),
  **all M3 and Slice 1 gates pass within 2 %**:
  - `physics_tests`: 0 failures, 0 gate misses, 0 report-only misses.
    Scenario A 0–100 km/h **5.827 s** (oracle 5.840 s, −0.22 %);
    Scenario B mean decel **8.421 m/s²** (oracle 8.57 m/s², −1.74 %, reverse
    phase 4.97 m/s² over 1.292 s vs oracle ~5 over 1.26–1.29 s);
    turn circle **3.340 m** (oracle 3.37 m, −0.89 %).
  - M3 oracle lap (`drive --run lap`): **39.205 s** (oracle 38.784 s, **+1.08 %**,
    `physics-behaviour.py compare` PASS: 0–60 2.73 vs 2.72 s, 0–100 5.74 vs
    5.76 s, top speed 147.6 vs 147.5 km/h).

- **Scenario B's mean (historical, closed above).** With the new brake unit the pad's stop has its
  ABS phase at 11.95 m/s^2 (oracle 11.9 to 12.1; was 13.19) and the braked
  steps at 52 / 76 % (oracle 50 to 54 / 74 to 81). The mean falls to
  7.69 m/s^2 (oracle 8.57) because the reverse phase is still 4.1 m/s^2
  over 1.58 s against the oracle's ~5 over 1.27 s (Q34). The mean's 8 %
  gate stays and is a known miss until Q34's reverse phase is answered
  (gates move only with the spec); the ABS phase passes its 12 % gate.
- **Since b6920963 (the tyre law).** Each curve times its row's
  `FrictionScale`, the width scale (0.9266 on the R34), the compound's
  `TorqueFreeLatFrictionScale` (1.8) and the 1.01 constant on the lateral
  force, in place of the 0.915 fit; the off-road peak (`OffRoadness`,
  `OffRoadDryPeakSA`) and `RearGripMultiplier` from surfaceTypes.xml; the
  rear-grip fix keyed to the road wheels' angle in degrees; the combined
  slip's 1e-4 floor; the lateral forces through a 0.28 m roll arm; the
  AI's grip factor replayed from `ai_grip` on the lap only. **The lap
  finishes: 40.915 s against 38.784 (+5.49 %, outside the 2 %).** 0-100
  km/h 6.08 s (5.76). Split: +0.26 s at 100 m, +0.28 at 200, +0.27 at
  300, +0.30 at 400, +0.32 at 500, +0.37 at 600, +0.72 at 700, +1.04 at
  800, +1.40 at 900, +1.93 at 1,000, +2.10 at 1,100. The first corner
  holds (cross-track RMS 0.78 m over the lap, max 3.7 m). The loss is
  from 550 to 650 m, where our line puts three wheels on dirt and the
  oracle's stays on asphalt (38 -> 34 m/s, never made up before the
  850 m braking), then the 900 to 1,000 m hairpin (+0.5 s). The first
  braking zone peaks at -14.8 m/s^2 (the oracle -16.8). Scenario B's ABS
  phase 12.06 (oracle 11.9 to 12.1); the turn circle 3.35 m (3.37). Not
  chased.
- **The skidpad (2026-11-21, physics_tests, report only).** Under the
  AI's controls on flat ground, held at speed, with the road wheels at a
  fixed angle, the R34 reaches the oracle's cornering. At 28 m/s it holds
  5.0, 9.5, 12.8 and 14.3 m/s^2 at 1, 2, 3 and 4 deg, peaking at 14.8.
  At 20 m/s it peaks at 14.7. Steady grip isn't the gap.
- **What the lap loses instead: speed while cornering.** From 600 to
  650 m both cars are at full throttle in third on asphalt. The oracle
  gains 131 -> 137 km/h at 13 to 15 m/s^2 lateral; ours loses
  126 -> 120 at about 10. On the 1,025 to 1,100 m straight our third
  gear pulls as hard as the oracle's (2.56 against 2.32 m/s^2). So the
  cornering drag (the tyre force's rearward share at our slip angles:
  front 0.5 to 0.8 of the peak at 13 to 14 m/s^2) or the rolling
  resistance under lateral load costs more than the game's. Needed: the
  oracle's longitudinal acceleration at a steady 12 to 14 m/s^2 corner at
  full throttle, or the rule that sets the slip angle (and so the drag)
  for a given lateral force. The pure-pursuit lookahead moves the lap
  between +4.0 % (0.45 s) and +5.8 % (0.3 s) without a trend; left at
  0.5 s.
- **Answered in CAR_PHYSICS.md 3ab1fd15** ("Force direction, surface
  drag and cornering drag"; "Check: speed in a full-throttle corner").
  F_y acts across the wheel's heading, so drag = sum |F_y| sin(alpha).
  The oracle loses only 0.4 to 0.6 m/s^2 to a 12 to 15 m/s^2 corner at
  full throttle (3rd, 130 to 140 km/h: 1.97 along the path against 2.42
  straight, gravity removed; not `a_long`), with the fronts at about 3 deg
  (s_y about 0.3) and the rears at 2.2. From 622 to 647 m its left wheels
  are on dirt. Our s_y of 0.5 to 0.8 explains at most about 0.8 m/s^2 of
  the gap; the rest is open.
- **Engine check (2026-11-21), by the body motion as the oracle's.** To
  560 m the two cars match (130 to 135 km/h, 13 to 17 m/s^2 lateral, body
  slip -0.4 to -3.9 deg, rear slip 0.9 to 5.1 deg against 1.2 to 4.4). Our
  car drifts wide of the oracle's path from 540 m (+0.4 m, +1.5 m at 570,
  +3.7 m at 615); from 575 m its outside wheels are on dirt where the
  oracle's are on asphalt, and the rear steps out: at 600 to 622 m body
  slip -8 to -16 deg and rear slip 9 to 17 deg (oracle -1.4 and 2.2),
  along-path -0.2 to -2.0 m/s^2 (oracle +1.76). A cross-track term on the
  lap's follower (`cross_gain`, SHIFT_LAP_CROSS_GAIN; 0.2 holds 2.6 m max)
  only moves the lap from +5.5 % to +5.0 to +5.3 % and the rear still
  slides (9 deg) once wheel 3 touches dirt. So the loss is the rear's grip
  with one or two wheels on the dirt edge at 130 km/h, not cornering drag.
  Asked: the oracle's rear on dirt (its 622 to 647 m, rear slip 1.5 deg
  with three wheels on dirt), so the dirt's lateral peak, OffRoadDryPeakSA
  or RearGripMultiplier read for a rear wheel there.
- **Answered (CAR_PHYSICS dc112515) and applied (2026-11-21).** The lap
  now computes the AI grip factor from its own wheels each step (races
  already did); the oracle's `ai_grip` replay is behind
  SHIFT_LAP_DIRT_REPLAY=1. **Lap 40.478 s, +4.37 %** (was +5.49 %). At 600
  to 622 m rear slip is 4.6 deg (was 9 to 17; oracle 2.2). Splits +0.37 s
  at 600 m, +0.82 at 900, +1.32 at 1,000, +1.60 at 1,100. What is left is
  mostly the 900 to 1,000 m hairpin: on the oracle's line within 1 m, ours
  turns at 11.6 m/s^2 lateral and 78.6 km/h against the oracle's 13.4 and
  86.5, under the same replayed pedals. Open, with the dirt lateral drag
  (0.35).
- **Answered in CAR_PHYSICS.md dc112515** ("A rear tyre on dirt"). No
  off-road curve: the asphalt curve with s_y = alpha / 18 deg
  (OffRoadDryPeakSA, OffRoadness 1), read at alpha x alpha_peak,road / 18,
  x FrictionScale 0.95 x RearGripMultiplier 0.93. R34 rear, 5 kN: asphalt
  peak F/N 1.63 at 8.2 deg, dirt 1.44 at 16.7 deg; at 2 to 3 deg dirt
  gives about 0.55 of asphalt. The oracle's 622 to 647 m has only its
  inner (light, about 1.7 kN) wheels on dirt, and the AI's grip factor
  (rear 1.20 there) multiplies all four wheels. Ours has the outer rear
  on dirt with ai_grip 1 replayed from the oracle, which off dirt is 1:
  at 13.5 m/s^2 that needs 8.1 deg (factor 1) or 5.1 (factor 1.2), and
  at 15 it is past the peak. The factor is the car's own state; compute
  it from our wheels, not by the oracle's distance. The dirt
  VelDepFriction lateral term (0.35) is still open.

## Q41 (M4): free-roam traffic (SPEC-TRAFFIC 33788840)

- **Done.** `libs/traffic`: colorado.nav and the drivable network (every
  count of the spec's tables matches), the freeroam settings and registry
  values, the time-of-day factor, the population (local density per 100 m
  within the spawn-out distance, smoothed every 0.1 s; target min(D_s,
  cap); four initial cars; spawns on the spawn-in circle, despawns at the
  spawn-out distance + 20 m; four loaded models of the 17, rotated every
  30 s) and lane driving (2.2 m right of the centre line, the road's mph
  speed within SpeedMin/Max, slowing for bends between RadiusForMin/Max
  and for the car or player ahead). `traffic-census` runs the oracle's
  protocol headless.
- **Census (10 seeds; 40 seeds in brackets).** Noon parked 4.79 cars
  (4.84) against the oracle's 6.1, inside 30 %. 02:00 parked, from 30 s
  after the change, 1.25 (1.40) against 0.9: **outside the 30 % gate**, a
  known miss. Noon cruising 2.99 against 4.4 (not gated). Every moving
  sample within 15 m of a traffic road (0 of 10,714 off); two-way roads
  100 % right of the centre line, median 2.20 m; spawns at 330 m, 31 %
  driving away (oracle 5 of 21); despawns at 390 m; 13.39 m/s on straight
  road (30 mph is 13.41).
- **Reading the misses.** The night count is the noon cars still inside
  390 m plus about one car (target 0.62: one spawn whenever none is
  active). The noon count is held by the spawn rate (one per 5 to 9 s while
  below target) and the cars' time in range (a car spawned towards a
  parked player stays about 54 s, one driving away about 5 s). Open: how
  the spawn interval follows TrafficMin/Med/Max and Variance, and whether
  the density walk's extent (the spec's second reading predicts 0.5 at
  night) is the one the game uses. Not tuned.
- **Ours, until the spec says.** The spawn interval (TrafficMed +-
  TrafficVariance); the away chance rising from SpawnAwayChanceMin at rest
  to the road's value at 20 m/s (SpawnAwayChanceLerpSpeed read as a
  speed); one loaded model swapped per rotation, a rotated-out model's
  cars driving on until they despawn; the junction choice fixed per car
  and node (uniform over the allowed ways); a car in the lane ahead
  within 2 m of its line is followed (matched less 2 m/s inside the
  look-ahead gap + 6 m, stopped inside 60 % of it); bends at 3 m/s^2.
- **Drawing and bodies (since d6e6b63).** The window draws each traffic
  model as a second Showroom from its M1 bake, with the player's camera
  and light; models load on a worker as the loaded set changes (12 to
  23 ms to upload one on the desktop reference GPU; headless loads at once so runs repeat)
  and a rotated-out model stays until its last car is gone. Cars within
  30 m are bodies with their own rows (17 of 17 baked), handed over on
  the move; in the census they stay within 0.29 m of their lane line, and
  the push test (the player's car at 31 m/s into one at 13.4) leaves the
  traffic car at 23.1 m/s. Traffic costs about 0.07 ms CPU and 0.03 ms
  GPU a frame on the desktop reference GPU (cruise bench). `--hour` and T change only the
  density: no day/night lighting. Not done: festival cars, fades,
  indicators and hazard lights, lane changes (one lane each way), the
  give-way slowing at junctions, the gear caps (GearMaxSpeed1st to 4th)
  and SteeringMultiplier, and the dead / reanimate behaviour (a car left
  stopped off its line stays a body until it despawns).
- **Checks after review.** The census with bodies (10 seeds): noon 4.82,
  02:00 1.43 (the night miss is larger than kinematic's 1.25), cruising
  2.95. The lane check now takes the nearest traffic road and the car's
  heading, independent of the population's own road: 100 % right at a
  median 2.20 m (10,985 samples). Beside the longest one-way freeway way
  (`--freeway`): 7,186 of 7,186 samples in node order, 26.78 m/s (60 mph is
  26.82), none off road. All 17 models as bodies (`--models`): stock mass
  (1,045 to 5,000 kg), their own boxes (the bus 2.39 x 9.90 m), upright;
  13 hold 30 mph within 0.5 m/s at AccelMaxInput 0.2, but the Prius, Leaf,
  Focus and Corolla lose about 0.3 m/s each second at that throttle (the
  Corolla at 2,500 to 3,000 rpm in 2nd). Open: whether AccelMaxInput is a
  cap on the throttle (as read here) or on its rate.
- **AccelMaxInput as a rate (interim, since this entry).** Per OpenFH1's
  call, AccelMaxInput and BrakeMaxInput now limit how fast the throttle
  and brake change (0.2 per second), not how far they go; the wanted
  inputs come from a proportional controller up to full. All 17 models
  then hold 30 mph within 0.7 m/s (12.74 to 13.50 m/s from 2 to 8 s; the
  Corolla 12.74 to 13.09). The census with bodies is unchanged (noon
  4.81, 02:00 1.41, 100 % right at 2.20 m); the push test unchanged.
  Pending the spec's answer on cap or rate. Superseded below.
- **Cap and lane hold (SPEC-TRAFFIC a7b959a2).** AccelMaxInput is a cap:
  on its line a body drives at throttle 0.2 (the oracle's 492 samples were
  all exactly 0.20), braking proportionally up to BrakeMaxInput when 0.6
  m/s over its target, and the lane hold keeps it at its lane speed,
  offset and heading. Knocked off its line it brakes at 1.0 with no hold.
  Ours until the spec says: (1) the reading of the VehiclePosition/
  Interpolation gains: each K is the share of the error removed per 1/60 s
  tick, interpolated by the body's length between Min (4.5 m: PositionK
  0.999, HeadingK and LaneOffsetK 0.95) and Max (10 m: 0.9, 0.93, 0.93);
  applied to the body's velocity, the speed along the lane towards a lane
  speed that moves to the target at AccelerationMax and DeccelerationMax
  (as a kinematic car's), the speed across it and the yaw rate to those
  that remove that share of the offset and heading error, capped at
  1.5 m/s across and 1.0 rad/s in yaw (uncapped, a body 1.5 m and 0.5 rad
  off its line was commanded 140 m/s and 47 rad/s; capped it is back on
  its line within 3 s at no more than its lane speed, traffic_tests);
  LaneOffsetDecay and the pitch and roll limits are unused; (2) no hold
  for 1 s after a contact, so a push or a knock is the body's own; (3) no
  first gear forced off the line. At these gains the hold is close to
  glue: all 17 models hold 13.41 m/s from 2 to 8 s (`--models`), bodies
  stay within 0.01 m of their lane line, the push test is unchanged (31.5
  to 20.8 m/s, the traffic car to 23.5).
- **Spawning while parked (a7b959a2).** The away chance was already
  SpawnAwayChanceMin (0.25) with the player still; the census's 31 %
  pooled the cruise, where the chance rises with speed. Parked spawns
  alone: 30 % of 105 in the census, 23 % of 741 over 600 s at noon. The
  terms behind the count, parked at noon: lifetime towards the player
  54.1 s, away 4.6 s, interval 6.9 to 7.2 s, lifetime over interval 5.8
  to 5.9 cars (the spec's 5.7); the steady count over 60 to 600 s
  (`--steady`) is 5.87 (6.1). The census's 4.79 is the first minute from
  the four initial cars, which drive either way at even odds (ours), so it
  is probably a transient. Open: whether the oracle's noon minute began at
  the initial placement, and which way its four initial cars drove.
  Settled 02:00 (the minute after the last noon car left,
  48 s after the change at the median; the oracle's 44 s): 0.97 with
  bodies, 0.95 kinematic (0.8; within 30 %). The 0.9 row (30 to 60 s
  after the change) still misses at 1.33 / 1.25: our noon cars leave
  later than the oracle's. Open: the interval's shortening with the
  player's speed (3.1 to 6.0 s cruising; Interval/SpeedFactor and
  SpeedMax not decoded), so the cruise count stays low (2.6, 4.4).

## Q42 (polish): the drive's post chain and sun shadows

The drive renders the scene to a float target (RGBA16F) and resolves it
through SHOWROOM_LIGHTING's post chain; the showroom (M1) path is
unchanged, byte for byte (references 1006 and 1115, their masks, the
headless car, and the drive with `--no-post`).

- **From the spec (confirmed):** the scene clipped at 4.0; the Hable
  filmic curve over its white point; desaturation towards the luminance
  (`s·c + (1−s)·L(c)`). The constants are the track's TrackSettings.xml
  values, not the spec's printed showroom ones (Colorado: filmicTone day
  A 0.700001, B 0.038, C 0.634, D 0.220001, E 0.003, F 0.107, white 1.0;
  DesaturateAmount 0.9; DesatLuminosity 0.2126 / 0.7152 / 0.0722; bloom
  cutoff 1.0, scale 1.0).
- **Ours:** the exposure, 0.8 (`--exposure`), set once so the free-roam
  reference frame's masked mean luminance matches the game's (118.3
  against 117.5), not swept for PSNR; the track's filmicTone exposure
  (0.31) applies to a scene the game lights more brightly than ours. The
  bloom: a bright pass over the cutoff into a quarter-size target and a
  separable Gaussian (sigma about 2 texels), added before the curve. The
  order exposure, bloom, curve, desaturation, sRGB. `--preset low` has no
  bloom; `--bloom` / `--no-bloom` / `--no-post` override.
- **Measured:** masked PSNR against the reference frame 14.96 dB before,
  15.92 dB after (a number, not a goal); mean rgb 122 / 119 / 97 against
  the game's 128 / 117 / 91, std 69 against 56 (ours has more contrast).
  The bloom adds nothing to that frame: our shading (sun colour below 1)
  leaves no pixel above the cutoff after the exposure.
- **Open:** DesatPostMod (1.04, 1.02, 1.0) and where it applies; the
  adaptive exposure (ToneMapAdaptiveExposure, CameraExposureRange); the
  night filmicTone values (the drive uses the day curve at every hour);
  the spec's probable bright-pass cutoff and blur sizes against ours; the
  composite order. The windowed path was not run (headless only).
- **Sun shadow (ours):** one orthographic map, its box centred half a
  radius ahead of the eye along the view, snapped to whole texels from
  the scenery's origin, 800 m deep along the light. It casts from the
  track's shadow light when TrackSettings overrides it
  (OverrideShadowLightDirection 1: towards the light is
  ShadowLightDirection negated, (-0.75, 0.65, -0.59) on Colorado, about
  34 degrees up), else from WORLD-MATERIALS' sunDir. Against the
  free-roam reference the override's barrier shadows fall the game's
  way (down and to the left); with z negated, and from sunDir, they do
  not. Casters: the scenery's opaque and cut-out draws culled against
  the box (blended draws and the sky cast none), tree sprites turned to
  the light, the car and the traffic within the box (glass casts none).
  Receivers: the scenery's sun term only, times `mix(0.35, 1, lit)`
  (TrackSettings ShadowColor 0.35, read as the sun's share left in
  shadow; ours). Tree sprites and the cars do not receive: sprites face
  the eye in the colour pass and the light in the shadow pass, and the
  car's key light is fixed to the car. `--shadows off | low | high`: low
  1024 texels over 80 m, one filtered compare (`--preset low`'s
  default); high 2048 over 120 m, four. Open: the game's own cascades
  (ShadowEnds Near 25, InGameShadowEnd 202, ShadowMipBias) and how
  ShadowColor applies; the shading still lights with sunDir (61 degrees
  up), so the shadows' light and the shading's differ.
- **Shadow costs (UHD 620, 1280x720, two runs each):** the pass's GPU p50
  1.69 ms on low, 2.41 to 2.54 ms on high; frame p95 11.93 to 11.95 ms on
  low (before the post chain and shadows 11.97), 16.54 to 17.31 ms on
  high (before 11.87; high is over 16 ms on this GPU). Steam Deck
  (headless over ssh, not at Steam's clocks, 1280x800 MSAA 4): the pass
  0.88 ms, frame p95 4.90 ms off and 5.84 ms high. Masked PSNR against
  the free-roam reference: 15.92 dB with the post chain alone, 15.41 dB
  with the shadows from the track's shadow light (a number, not a goal).

## Q43 (play-test): invisible walls, glow cards and the reset

- **Route groups (answered, COLLISION.md 9005ebcb / c5b81225):** the
  `.fiz` word at offset 2 is a per-tile route mask, the tile's route list
  at block A +60 (count at +122). The drive applies the rule everywhere:
  a triangle collides when (word AND (0x8000 OR the tile's bit for the
  active route)) is not zero, the active route 0 in free roam (
  `world::Collision::set_active_route`). collision_tests matches the
  spec's counts: 7 tiles list route 0; 1,851,955 triangles collide and
  155,640 do not (cores 1,291,222 and 110,640); 96 only through route 0.
  The category byte is not applied yet; the edge word at +28 is not
  decoded. On the 200 s cruise, the car-height wall samples with nothing
  drawn within 1 m stay at 33, 337 and 526 over the first three gathers
  (9,736 and 13,601 before); what is left is mostly always-on GuardRail
  (x 2640 to 2780, z 1260 to 1550, no drawn rail within 1 m: open) and
  Invisible walls. `--contacts-log file` logs hard contacts and these
  walls.
- **Glow cards (answered, WORLD-MATERIALS item 7):** coloradoout.06509 is
  the only `DIFF_GLOW_1` model, a gameplay beacon column at 105 places
  the game did not draw in any frame checked: its placements are left
  out (interim rule). The family stays drawn unlit and additive (ONE,
  ONE, no depth write, no shadow) should another appear.
- **Reset (ours):** R, Start and the headless driver's "stuck" put the car
  on the nearest nav road's centre line (SPEC-TRAFFIC roads), stepping
  4 m along it out to 60 m for a spot whose ground is within 2 m of the
  road's height, faces up, and has no wall within 3 m at bonnet height;
  the old nearest-road-triangle placement is the fallback. The body
  contacts probe from the box's centre, so a car placed straddling a thin
  wall was pushed out through it (a fall through the world, then a
  non-finite state); headless runs now stop when the state is not
  finite. A swept contact is still to do.

## Q44 (play-test): untextured draws and the bare GuardRail

- **Fallback census (ours, `--texture-census`, headless):** per frame, the
  scenery triangles drawn with a fallback (a slot whose record has no
  file, a texture that failed to decode, a missing terrain layer) by
  model, effect and slot, with their unoccluded screen area. On the 200 s
  cruise they cover 0.1 to 0.8% of a 1600 x 900 frame. Nearly all of that
  is `H_DIFF_1` models coloradoout.05367 to 05468 (slot 0 value 0, a
  type-1 record: "no file", WORLD_MATERIALS says a 4 x 4 texture is
  bound). They run along the road 7 to 12 m above it: the power-line
  cables. The rest is `H_VBLND_NORM_MASK_OPAC_BIAS(_LM)` with slot 1 -2
  (no Blend_B, drawn with Blend_A alone; under 0.1%). So the grey slopes
  are not fallbacks: they are drawn with their listed textures. Open: the
  4 x 4 texture's texels for type-1 and type-3 records, and which spot
  the grey-slope screenshot was taken at.
- **Bare GuardRail at x 2640 to 2780 (open):** tiles 2463, 2508 and 2562
  hold always-on (word 0x8000) GuardRail triangles at car height about
  17 m left of the road (x 2640 to 2771, z 1263 to 1534), with nothing
  drawn within 1 m, on route 0 or with every route's placements
  (`--route -1`, which adds concrete event barriers along the road but
  none there). Whether the game draws anything at about (2665, 115, 1315),
  or keeps an invisible rail there, needs a frame.

Q44 answered in horizon1-recomp (branch reimplementation-plan):
ecf6b0b3, eb31ad8f, 1ca50963 (2026-11-20), 9bccba4e (2026-11-21).
(a) Type-1 and type-3 records are textures in the 146 `.bundle`
members (WORLD_MATERIALS "Small textures"); the cables' record 4203 is
a 4 x 4 DXT1 gamma texture of (33, 34, 33), opaque. Records in no
bundle: white AO/AOMap, black Light_Map. Confirmed. (b) The tiles hold
two 50 m vertical GuardRail walls; the +x wall has a drawn armco along
its length, the -x wall (your point) none from z 1258 to 1555: an
invisible rail by design. Confirmed for four frames (COLLISION "Bare
GuardRail walls"). (c) The game's far hills are dark warm browns, never
grey; grey is the far UberLOD peak. Interim: lerp(Blend_A, Blend_B,
R/255), AO 1, warm sunColor (0.97, 0.84, 0.58); check Blend_B pixels
by hue (red about twice blue). Probable. (d) coloradoout.06765 is
placeholder geometry, not foliage: slot 0 `_0x00002CA9` is the
missing-material marker (yellow with a caption), shared by 53 `H_DIFF_1`
models (terrain blockouts incl. the 06760 far ring, shadow stand-ins,
tunnels, two `_CAGE` walls). It lies about 80 m below the drawn terrain
where they overlap, has no alpha source (the game's `H_DIFF_1` is
opaque, no coverage), and none of the 53 is drawn in the free-roam or
hill frames. Rule: skip every material whose slot 0 resolves to
`_0x00002CA9` (WORLD_MATERIALS "Placeholder models").
(e) The sky hole at pixel (530, 207) of hill-2244-707 is terrain in the
game: `TERR_CLRD_ZONE02_AREA3_MIDDIST_06_LOD00` (coloradoout.12436,
`H_VBLND_NORM_AO_LM`, 353 to 377 m), drawn in that frame; `_LOD01`
(12437) and `_LOD02` (12438) cover the same pixels, so any level fills
it. Not a CAGE, TERR_CUBE, occluder or placeholder model; the aspens are
billboards in front of it. Check why the engine's gather drops
MIDDIST_06 there (its box is about 314 m from the camera). Confirmed;
horizon1-recomp 7e842336.

## Q45 (M4): the driving cameras (SPEC-CAMERAS 163ffec3)

`drive::DriveCamera` (libs/drive cameras.h) places the five cameras from
the player's CameraSettings.ini, CameraPhysics.xml (v4's own copy when
installed), the car's CameraOverrides row and MAXData box. camera_tests
matches the spec's rule column on the R34 and the CR-X parked: every eye
within 1 mm, every FOV exact, cockpit and hood pitch 1.11 and 7.20 deg,
the looks back and to the side, and the steady speed FOV (CR-X low chase
at 13.6 mph +1.20 deg; R34 hood at 30.3 mph +0.51 against +0.52). The R34
low chase sits on the 1.80 m band (the oracle's 1.858 m stays open). On
the 200 s cruise the chase eye stays 5.31 to 5.53 m from the CoM (no
pull-in: the spec's 0.3 m clearance replaces the old ray pull). Open, with
ours meanwhile:

- the slope term's limit as a fraction of r (ours 0.25) and how long it
  stays on after the wheels leave (ours 0.5 s);
- the pitch layers' input sign (ours: nose up positive);
- the chase FOV factor's angle (ours: half angles, tan(F/2) /
  tan((F + dF)/2)); the five cruise rows are not matched yet;
- what the cockpit draws: no interior is baked, so the car is left out
  there for now;
- the look spring (ours: 0.1 s), and the heading spring's yaw-rate sign
  (as written).

answered in CAMERAS.md 4d4e2ca4 (with 24a2044b) for (a) to (c) and the
yaw-rate sign, from Q51's lap: (a) the slope term is the car's tilted
plane at the eye (this frame's eye and up axis), limited to ±2 r
(probable, never reached), applied while up.y > 0.7 and for 0.2 s after
fewer than three wheels touch; (b) PitchAngleDegrees is positive nose up,
ours was right, but both bands (past +3.5° and −3.5°) raise the eye and
look down; (c) the factor is sin(F) / sin(F + ΔF) of the full angle, not
a tangent; the heading spring damps ω − s · ψ' (as written it was +, 21°
off in replay). Open: (d) the cockpit, the look spring.

(d) the cockpit's interior colours and mirrors: answered in CARS.md
62e6916c (horizon1-recomp, branch reimplementation-plan, "Cockpit
interior and mirrors"), 2026-11-21. (1) No colour constant, database
column or vertex colour: albedo = interior_LOD0 texel at UV0 through the
sub-section's UV0 transform x a per-technique scale (plastic 0.5,
mottled 0.35, bump_plastic 0.3, cloth and solid 0.5, gauge_emissive 1.0;
effect defaults that ShaderSettings.xml can override), then the usual
lighting. The R34's dash texels are already near black (sRGB 24, 22, 24;
wheel 11, 9, 11). The interior shaders have no slot-0 sampler: the body
atlas there is white paint texels, so don't read it. Many interior
sub-sections sample a tiny swatch (UV0 scale about 0.005), so a white
result means that the transform or the texel read is wrong. (2) UV0,
which the effect calls "uv1" (confirmed on plastic, mottled and gauges;
probable on the two-UV techniques). (3) The mirrors sample a mirror
texture in per-mirror x bands (right 0.7 to 1.0, middle 0 to 1). The
showroom binds a zeroed 32x32 texture, and a rear-view target while
driving is probable. Interim: flat near-black. Open: the look spring.

## Q46 (M5, blocking the gate): oracle races over ten events

The M5 gate compares AI mean distance (3 %) and finishing order over ten
events; the oracle has FR22 and FR16 only. Needed: eight more oracle races
covering the race types of SPEC-EVENTS (ribbon-0 circuit, ribbon-2
circuit, ribbon-2 sprint, multi-lap sprint, street point-to-point,
single make, headline, nemesis), each with the RACE_AI Tests table (AI
times, mean distance, places, slots) and the player's path by time as a
readable oracle output (a CSV of t, x, y, z, distance along the route), so
the engine can replay the player and the rubber-banding sees the same
gaps. Meanwhile: FR22 and FR16 only, the player under our AI.

answered in SPEC-RACE-AI (Tests: oracle races over ten events) and
SPEC-EVENTS (grid: PlayerGridPosition 0 is the last slot; sprint finish
offset varies) 6346f8ee. Exports in out/oracle: <event>-player.csv and
<event>-results.json for FR22, FR33, EXHIBITION_FEST_003, HEADLINE_EVENT,
FR16, FR06, FR24, FR01, STREET_PLNS_002, NR02 (regenerate with
horizon1-recomp tools/re/race.py export). Sprint and checkpoint finish
planes are fitted to the player's race record per race.

## Q47 (M5): street and nemesis races (checkpoints, no AI line)

Ribbon-6 routes have no OWTM line. Needed: the checkpoint data in the
route file and the pass rule (radius or gate plane, order, missed
checkpoint), the finish, and what the AI follows there (the nav road
graph? a line built from the checkpoints?). Meanwhile: checkpoints as the
route file's gates in order, crossed as planes; the AI follows a line
through the checkpoints on the road graph (ours).

answered in SPEC-EVENTS (checkpoint races) and SPEC-RACE-AI (routes with
no AI line) 933fc0ff: checkpoints in number order, a plane crossing within
half the width in either direction, the finish at the last one
(probable); the AI drives the nav road graph, about ±4.5 m around the
centre line. Open with interims: missed checkpoints, the route choice
between checkpoints, the line within the road.

## Q48 (M5): ribbon-2 circuits and multi-lap sprints

The AI line is open and runs past both ends. Needed: how a lap is
counted and where cars rejoin (do they U-turn, or is the lap a there and
back?). Meanwhile: not run.

answered in SPEC-EVENTS (laps on an open route) ffe6ef0f: NumLaps is
ignored on a ribbon-2 route; the race is one run from the grid to the
sprint finish, the HUD shows progress in percent, opponents stop after
finishing (FR24 and FR01 measured; probable for all seven). Open: the
field that decides it (interim: RibbonConfiguration 2), FR37 on ribbon 0.

## Q49 (M5): rubber-band mode ids and Amount, AI grip on dirt

SPEC-RACE-AI draws modes 1 to 7 from AIRubberbandModeChoices but gives
neither the id-to-mode map nor what Amount scales; the AI's dirt grip
factor is 1.0 to 1.5 with no rule. Meanwhile: modes drawn with no effect;
dirt grip 1.25.

answered in SPEC-RACE-AI (difficulty and rubber-banding: mode table, draw,
hold time) and SPEC-CAR-PHYSICS (grip on dirt) 8f391c58

## Q50 (M5/M6): rewards, XP and the results screen

Needed: the credits rule (EventScoring, CashPrize, RaceWinnings,
CreditsMultiplier), XP or popularity for a race, what the results screen
lists, and how the profile records an event win (wristband progress).
Meanwhile: the interim of SPEC-EVENTS (EventScoring row × CashPrize /
1,000, no XP).

answered in SPEC-EVENT-REWARDS 28bd6ba6 (credit split and rounding,
wristband points, saved race and event results, profile counters, prize
cars, results screen; bonus lines, difficulty bonus, driver XP and
popularity open with interims)

## Q51 (polish, queued last): a chase capture at speed

For the chase camera's feel: an oracle capture (camera pose per frame
against the car's pose) through corners, a crest and hard braking, so the
engine's chase can be compared. Meanwhile: Q45's interims.

answered in CAMERAS.md 4d4e2ca4 (Tests: chase at speed; rules in Chase
cameras and Camera physics layers): the R34 oracle lap in the low chase,
traced every frame twice. Export out/oracle/chase-r34-lap.csv (t, dt, car
CoM, axes, velocity, contacts; view eye, forward, up, FOV; chase eye,
look-at, heading, h, slope hold; layer sums), regenerate with
horizon1-recomp tools/re/chase.py export. Replayed from the car's pose:
heading 0.3 to 0.44° rms, h 0.3 mm, distance 0.2 mm, layer sums 7 to 23
mm and 0.16°, final direction 0.15°. Layers compose as eye += car-axes
offset, chase direction pitched by the pitch sum (no re-aim), roll from
the layers only. Open: the high chase at speed (same rules assumed).

Base height (R34 low chase 5.8 cm): answered in CAMERAS.md 50040686
(Chase cameras, Height and Look-at). The start applies the car's
CameraOverrides for that camera first; if any of its height, radius or
target override is non-zero, the band correction is skipped (c = 0).
A fixed eye lift L (0.35 m FollowCam, 0.60 m FollowCam2, code
constants) is in both the start height and the spring target: start =
box top above ground + L + H; target = (MaxY - CoM y) + L + slope; eye
above CoM = H + c + h; look-at above CoM = (MaxY - CoM y) + T + c (no
delta). R34: 1.3725 + 0.35 + 0.1375 = 1.860 m vs 1.858 m parked; on the
lap its height and target offsets read exactly the overrides (0.1375,
-0.0025) and the target base is box top + 0.350 within 0.4 mm. For a
car without an override the result is unchanged (delta was L).

## Q52 (M5, not blocking): where CareerRaceModes.xml lives

SPEC-EVENT-REWARDS reads each mode's WinningsDistribution from
`CareerRaceModes.xml`; no loose file of that name is on the disc (inside
an archive?). Needed: its archive and member, and how an event names its
mode (CareerEventTypes.RaceModeId?). Meanwhile: the distribution from the
career type (headline: winner takes all; street and nemesis: top three;
else the default split).

## Q53 (M5, blocking the gate): the AI's speed planning rule

SPEC-RACE-AI gives the AI's target speed as a measured fit (corner
acceleration cornering x 1.7 g, braking x 2.0 g, fitted on FR22's R34).
With it FR22 passes (AI mean +0.6 %), but the other oracle races are slow:
FR16 +5 to 8 % (rear-drive cars), STREET_PLNS_002 +10 %, NR02 (a
Challenger at skill 31) +23 %. Needed: how the title's AI plans its speed
along the line (what it reads per car: grip, mass, power, aero; how
cornering and braking scale it; any look-ahead or per-segment speed table
in the line file or the car), with an oracle trace of target against
actual speed on one more car. Meanwhile: the fit.

answered in RACE_AI.md d3708678

Follow-ups answered in RACE_AI.md 0479ff4e:

1. Calibration: the title's own tables, dumped per AI driver on the FR22
   and FR16 seeds, are in RACE_AI.md ("Measured tables"). The R34 at
   20 m/s gives A_lat 15.44 and A_brk 17.60 m/s^2 on the stock car
   (1,560 kg). On the AI car (1,531 kg, UpgradeAI) it gives 16.01 and
   18.26. The other cars give 15.1-16.7 and 17.5-19.0. The 16.7 and 20.9
   were not a real equivalence, so drop the x1.11 and x1.23 scaling. The
   engine's 15.0 and 17.0 are 3 % under the stock car; use the AI car's
   mass.
   - The terms the title has: per wheel, the blended peak friction at
     load (FrictionScale once, peak not sliding) x the wheel's lateral
     scale x cos(peak slip angle); the rear axle capped at (1-w)/w x the
     front (yaw balance); lateral transfer by axle arm over track, six
     passes; x TorqueFreeLatFrictionScale / mass.
   - Braking has forward load transfer (six passes) and the car's
     braking friction scale, plus aero drag / mass.
   - Downforce is in both, at f(v).
2. Braking sign: confirmed. a = -B A_brk sqrt(...) + a_g, with
   a_g = -9.81 m_s s_g; carry v - 2a/v and sqrt(v^2 - 4a). The text is
   fixed.
   - Correction: k_B is 0.96 for four-wheel drive (DrivetypeID 3) and
     0.94 otherwise, not for rear drive. The car's field is
     DrivetypeID - 1.
3. NR02 skill: the race rival ran skill 31 (rubber-band row 43, cornering
   0.82-0.89, braking 0.82-0.88), as the engine rolls it. The 26 and 27
   were free-roam drivers' controllers before the race. Difficulty was Med
   (1) on every oracle run.
4. Nemesis boost (probable): the skill x 1.5 boost needs at least one
   other race AI ahead of the rival, so NR02 (one-on-one) never gets it.
   With no other race AI there is a torque-only boost, ramped by the
   rival's gap behind the player (50-100 m) and ended near the finish;
   its size is not read (interim none). Keep the interim of no skill
   boost.

Mini 1566 follow-up answered in RACE_AI.md 9d11eccd ("Measured tables",
"The Mini 1566"):

- The title's seven Minis on EXHIBITION_FEST_003 share one table (606 kg,
  drivetrain field 0 = front drive, top speed 56.3 m/s). At 20 m/s:
  A_lat 16.24 and A_brk 19.12 m/s^2. Lateral at 0/20/40/60 m/s: 16.22,
  16.24, 16.27, 16.32 (nearly flat, almost no downforce). Braking: 18.73,
  19.12, 20.29, 22.23.
- The tyre limits it, not the yaw cap. Per wheel at static load (194 kgf
  front, 109 kgf rear): the 1.18/0.93 FrictionScale pair blended by load
  gives 1.135 front, 1.155 rear; width scale 0.8625 (145 mm clamps at the
  150 mm end); TorqueFreeLatFrictionScale 1.73. Front 1.083 g, rear
  0.621 g against a cap of 0.609 g: the cap trims 0.7 % of the total.
  The 1.69 g (16.6) less lateral transfer and cos(alpha_peak) is the
  measured 16.22.
- The engine's 20.7 is 1.27 x the title's; neither the cap nor the
  FrictionScale pair explains it. Check the terms one by one: leaving
  out the width scale alone gives 19.2. Braking is 1.06 x high.
- At C 0.9 on a 50 m radius the title plans 27.0 m/s; a 20.7 table
  plans 30.5.

AI auto-upgrade follow-up answered in RACE_AI.md f16fbf95 and 0e634b9a
("AI car upgrades"):

- Bottom line for FR01: the title's Corvette (-0.03) and Focus (-0.03)
  are slightly detuned and the FR-S gets only t 0.08 (torque 1.03). The
  upgrade does not explain their 5-7 s; look elsewhere for those. It
  does explain the Charger (t 0.45), Raptor (0.61) and Firebird (0.75).
- 1. Target (confirmed rule; probable that the input is the player's
  PI): each AI aims at the player's shown points P, pulled to hi - 5 if
  P + 5 >= hi - 5 and to lo + 5 if near the bottom, plus a fixed
  per-car offset 0, +1, -2, +3 or -4, clamped to the event class. Not
  the class cap; +300 only applies when P > the car's points + 300.
  With the stock R34 (461): FR01 targets 395, FR22 506. Compute it from
  the player's car at run time; never hardcode.
- 2. Steps (confirmed): six scales (mass, torque, 2 front + 2 rear
  friction) between stock and mass 0.9 / torque 1.4 / friction 1.2 (the
  inverse for a car above its target). Far end rated first; then 10
  bisection steps on the midpoint of all six together. No order: one
  fraction t. Up: mass 1 - 0.1t, torque 1 + 0.4t, friction 1 + 0.2t.
  Down: mass 1 + 0.111t, torque 1 - 0.286t, friction 1 - 0.167t.
- 3. Per car (confirmed), each from its own PI.
- 4. Oracle (masses confirmed; trace mass / CurbWeight): FR01 Corvette
  1.004, Focus 1.003, Jeep 0.997, FR-S 0.992, Charger 0.955, Raptor
  0.939, Firebird 0.925. FR22 RS4 1.018, R34 0.982, Quattro/Evo
  0.970/0.965, Range Rover 0.960, Jeep 0.961. Friction from the
  ai.tables ratios matches t (R34 1.037, Jeep 1.08); torque from the
  forward tables 1.050 and 1.147 against 1.074 and 1.159 (probable:
  take torque from t).
- Interim (the engine has no PI lap): t = min(1, gap / 240) up,
  min(1, -gap / 170) down, gap in shown points to the target above.
- Your one-s-per-event interim: the 0.1/0.4/0.2 shape is right for cars
  going up, but one s per event is not. FR01's t runs from -0.03 to
  0.75, and s = 0.3 would wrongly speed up the Corvette. FR22's mean
  (about 0.27) is close to 0.3, but the RS4 is detuned.

Field-compression follow-up answered in RACE_AI.md ce706361 ("Catch-up",
"What else compresses the field"): no such thing beyond the
player-relative rules.

1. No torque or speed boost while behind (confirmed): DistBehindFor-
   TorqueBoost is 0 on skill rows 1-80 and FarBehindTorqueBoost is 1.0
   on every row. On the FR24, FR01 and NR02 traces AI torque never
   exceeds 1.000; the values below 1 are torque cuts over 140 m ahead.
2. Catch-up is measured to the player (confirmed in code and traces): on
   FR24/FR01/FR22 it was on only while the AI was behind the player in
   place (119/709, 159/593, 1,420/3,488 samples), never while ahead of
   the player however far behind the leading AI.
3. Row 6: rubber-banding from 10 m ahead; torque cut 140-500 m, factor
   0.4; catch-up on at 60 + 20n m, off at 40 + 20n m (n = places behind
   the player), CatchUpFactor 0.8. While on: b' = b + 0.8 (1.0 - b),
   c' = c + 0.8 (1.25 - c), a pull and not a multiplier (skill 50 at its
   maxima: 0.953 and 1.14). Before that, any AI more than 20 m behind
   the player already sits at its row's maxima (0.763 / 0.700), every
   sample in the traces. The trace's scalars are the rolled ones;
   catch-up writes the pulled values to the planner.
- So the oracle's 3.8 s spread comes from compression around the
  player from both sides. A 10-15 s engine tail points at a missing or
  weaker piece: the 20 m maxima step, the pull form, the 20 m per place,
  or the torque cut and mistakes on the cars ahead.

Field-spread follow-up (FR06, HEADLINE) answered in RACE_AI.md 1e1c0617
("Oracle grids and the tail on FR06"):

1. Slot to car (slot 0 = pole; car from trace mass / CurbWeight and the
   upgrade rule; "prob" = a pair with near-equal curb split by the
   bigger detune going to the car further above the target).
   - FR06 (target 506; the player in slot 6, 5th, 144.02): 0 California
     1062 (down 0.19, 2nd 142.65), 1 Exige 454 (down 0.15, 3rd 143.19),
     2 Camaro ZL1 1419 (down 0.47, 1st 141.60), 3 370Z 1103 (down 0.03,
     4th 143.58), 4 Diablo SV 325 prob (down 0.37, 6th 145.31), 5 TVR 433
     (down 0.53, 7th 145.59), 7 Ford GT 348 prob (down 0.65, 8th 145.97).
   - HEADLINE (target 906; the player in slot 7, 8th, 513.33): 0 599XX
     1171 (up 0.12, 1st 496.82), 1 FXX 1006 (down 0.03, 2nd 503.24), 2
     Zonda R 1175 prob (down 0.41, 7th 506.65), 3 SR8 LM 1247 (down 0.33,
     5th 506.35), 4 MC12 Corsa 1539 prob (down 0.56, 3rd 503.33), 5 F40
     Competizione 1023 (down 0.39, 6th 506.45), 6 F50 GT 1020 (down 0.58,
     4th 505.30).
   The engine's FR06 tail (370Z, Camaro, Exige, TVR) holds the oracle's
   1st, 3rd and 4th, so check the engine's slots first.
2. On FR06 the field mostly ran ahead of the player: five of seven AI
   were never more than 20 m behind it, and the leaders were torque-cut
   (159 and 66 of ~270 samples). Only two AI went more than 60 m behind,
   for 90 and 39 samples. Per-car mean speed per 500 m of each car's own
   path from its grid is in the spec table for the engine to compare.
   No other lift (decoded):
   - catch-up writes only b' and c', not the AI grip factor;
   - C = c' x 1.0 can exceed 1 (1.14), so the plan goes about 7 % over
     the capacity-table corner speed, and the physics decides; the boost
     clamp acts only while a code-only boost is on;
   - no torque cut behind the player;
   - the detune at negative t is intended (the Camaro at t -0.47 won).

Grid-order follow-up answered in EVENTS.md 20a9063a (Grid, "Opponents"
and "Measured grids"); partial:
- Not a per-run shuffle: two FR22 oracle runs gave the same grid and
  the same AI masses (fixed per event, measured).
- The rule is PIWithPlayerLast (CareerRaceModes.xml, every career race
  mode but one Random one). The PI is probably each AI's faked PI after
  the auto-upgrade: the shared target plus the per-participant offset 0,
  +1, -2, +3, -4. So the order is set by that offset plus a tie order.
  Open: the offset's seed (a value in the participant's runtime entry;
  none of ID, Ordinal, AIPlayerID, ColorSequence or the row index fits),
  the sort direction, and the tie order. The sort code was not found.
- Interim: use the measured grids (slot 0 first, the player as listed):
  FR01 Raptor 1460, Jeep 1554, FR-S 1548, Firebird 1276, Charger 513,
  Corvette 312, player, Focus ST 1399. FR06 California 1062, Exige 454,
  Camaro 1419, 370Z 1103, Diablo 325 (prob), TVR 433, player, Ford GT 348
  (prob). FR22 Quattro 633 or Evo 375, Jeep 1554, Range Rover 1525, R34,
  R34, player, RS4 419, the other of 633/375. HEADLINE 599XX 1171, FXX
  1006, Zonda R 1175 (prob), SR8 LM 1247, MC12 1539 (prob), F40C 1023,
  F50 GT 1020, player. Other events: ID order until resolved.

- Answer (spec, 2026-11-21, grid hidden run; horizon1-recomp e6b5ec95,
  SPEC-EVENTS Grid, SPEC-RACE-AI AI car upgrades): a new read-only log of
  each participant's faked PI, on hidden FR06 and HEADLINE_EVENT runs.
  - Seed: the car's `Ordinal`. One LCG step on it gives k in 0..4, r = +k
    odd, -k even; 14 of 14 cars match. A per-participant flag forces +5
    (one car per event: FR06 Camaro 1419, HEADLINE 599XX 1171; what sets
    it is open).
  - Direction: AI sorted by faked PI (shared target + r), highest on
    pole. HEADLINE 7 of 7 (+5, +3, 0, 0, -4, -4, -4); FR06 6 of 7. The
    exception is the flagged Camaro (+5): it started on slot 2 behind the
    two +1 cars (open; interim: sort it like the rest).
  - Ties (exactly equal PI): neither row order nor its reverse, and no
    entry field explains them (open). Interim: row order.
  - PlayerGridPosition: the player takes slot p - 1 (p = 0: last). The AI
    fill the other slots in sorted order, so on FR06 (p = 7) the slowest
    AI, the Diablo, starts on slot 7 behind the player.
  - Grids matched the earlier oracle runs (fixed per event). Correction:
    FR06 slot 4 is the Ford GT 348 and slot 7 the Diablo 325 (the tables
    are built in participant order); HEADLINE splits confirmed (Zonda slot
    2, MC12 slot 4).

- Offset step (spec, 2026-11-21, horizon1-recomp 85473942, SPEC-RACE-AI
  AI car upgrades step 1): with v the car's `Ordinal`,
  x = (214013 v + 2531011) mod 2^32 (0x343FD, 0x269EC3, unsigned 32-bit);
  u = floor(x / 2^16) mod 2^15; k = u mod 5; r = +k for odd k, -k for
  even k (0, +1, -2, +3, -4); the flag sets r = +5. Example: California
  1062 gives x = 229812817, u = 3506, k = 1, r = +1 (base 507, shown 508).
  FR06 grid, consistent in both specs: California 1062, Exige 454, Camaro
  1419, 370Z 1103, Ford GT 348, TVR 433, player, Diablo 325.

## Q54 (M6, queued after the M5 items): the specs M6 needs, in order

M6's gate is "the reference save imports and shows the same progress
(signs, cars, DLC)". The engine has its own save (drive/save.h) with
position, collectibles, barn cars and the career records. Needed, each a
narrow spec with its own tests:

1. FORMAT-PROFILE: the Pinyon Shift / game profile as the reference save
   stores it (container, encryption or checksum if any, the tables and
   counters): credits, wristband points and level, owned cars with their
   colour, upgrades and tuning, found signs and barns, event results, DLC
   ownership flags. Counts and field layouts only.
2. Garage and ownership: the starting car, the car list a garage shows,
   selling, the car a race starts in.
3. Upgrades: the parts tables (CarPartsList.xml and the database), what a
   part changes in the physics parameters, prices, PI and class, and the
   discount signs' effect (SPEC-FREEROAM-COLLECTIBLES gives the discount).
4. Autoshow: which cars are buyable when, prices, unlocks.
5. Paint: the colour picker rows and how a colour lands in the car's
   paint (FORMAT-CARS has the paint inputs).
6. The 1000 Club, Horizon Rally and the DLC packs (what each pack adds:
   cars, events, the Rally expansion).
Meanwhile: none of this in the engine.

FORMAT-PROFILE answered in docs/formats/PROFILE.md 9e6822f3
(horizon1-recomp, branch reimplementation-plan, 2026-11-20). Item 1 is
covered: folder, `cmss` container, secure envelope and key derivation,
profile body, class-serialised tail and the 19 `PlayerDatabase` tables,
with reference counts for both trees (`user`, base-written: 100/100
signs, 6/9 barns, 19 cars, 36 event results; `user-modded`, v4-written,
the profile the game plays: 38 cars, 119 event results, 1000 Club and
Rally records) and 22 installed content packages. Interim: import the
plaintext export of `tools/re/profile-export.py`; livery key open
(paint from `DefaultManufacturerColorIndex`); 1000 Club layout a guess.

AUTOSHOW-PAINT answered in docs/specs/AUTOSHOW_PAINT.md c58c6685
(horizon1-recomp, branch reimplementation-plan, 2026-11-20). Items 4
and 5: the listing rule and counts (127 listed on the disc, 127 with
v4, 200 with the 21 packs; 16 unicorns, 23 not selectable, 10 Rally
twins, 1 unlicensed), order, the All affordable entry, price =
`BaseCost` (signs do not discount cars), the hand-off to
SPEC-GARAGE-UPGRADES; `DefaultManufacturerColorIndex` is a
`Combo_Colors.Sequence`; finishes are overlays of
`Shared/ShaderSettings` (load at run time); special colours. Liveries
for the M6 gate: paint every owned car from its stored Sequence, draw
the 15 `HasRaceLivery` cars from their atlas, ignore `LiveryFileName`
(the one file in the reference save is the default design). Open with
interims: colour grids, two-tone blend, the metallic and matte look,
the livery format, tokens (none).

GARAGE-UPGRADES answered in docs/specs/GARAGE_UPGRADES.md f25f7f7d
(horizon1-recomp, branch reimplementation-plan, 2026-11-20; first
commit d845a1e1). Items 2 and 3: a purchase (credits, the new garage
row with stock parts, `CareerCar`), the owned-car record, the 45 part
slots and their 43 `List_Upgrade*` tables (enum to column to table),
part prices with the sign discount and `PricePaid`/`PartsValue`, the
mass rule, the class bands and shown index (checked on 45 stored garage
rows), stock owned index = `Data_Car.PerformanceIndex` (15 cars). The
upgraded index is a virtual lap from `PI.xml` with the time map
confirmed and the lap model open. Interims: keep stored indexes, move
the stock index by the engine's own lap-time difference after a part
change; selling at 0.5 x (BaseCost + PartsValue); no garage limit;
starter Corrado 309 with stock parts; Auto Upgrade by table order.

GARAGE-UPGRADES follow-ups answered in docs/specs/GARAGE_UPGRADES.md
e6296d17 (2026-11-21). (1) `CMHeightDiff` is unused: the executable
never names the column and it is 1.0 on all 623 chassis-stiffness rows;
leave `CMHeight` as is (the engine's multiply is equivalent). (2) The
torque combine is additive, confirmed from the part loader:
S = 1 + sum(TorqueScale - 1) over the nine engine part tables, times the
fitted camshaft curve. Car 1566: S = 1.893 x curve 112.50/107.20 =
1.987 against the game's 2.1297/1.07 = 1.99; the product gives 2.45.

V4-CONTENT answered in docs/specs/V4_CONTENT.md b07ea92a
(horizon1-recomp, branch reimplementation-plan, 2026-11-20). Item 6:
DLC packages (folder layout, `<offer>.puboffer`, content archive and
loose folder over `media`, `zipmount.xml`, string tables, any
`media/db/patch/*_merge.slt` merged into v4's database by priority,
ownership by offer and licence mask; 21 official + UDLC, 15 with a
merge, 96 car rows / 84 ids, no events or routes in car packs). 1000
Club: 218 challenge files, 1,145 challenges, the node and condition
grammar, medals and the ten achievements; the profile record is now
probable (file order, blob = saved counts, flag A = completed; key
algorithm open, match by index). Rally: 36 v4 events (28 stages, 7
championships, heats), routes, 4 split fractions per stage, a 49-driver
timed field, unlock chain, XP and level tables, 10 twins; block 2
preserved. Interims: stage finish at AI-line progress 1, AI times =
best / f(skill, difficulty), XP per championship place, credits and
the Rally-upgraded test guessed.

## Q55 (M5, blocking the gate): AI contact and resets in the oracle races

Six of ten events pass the 3 % AI mean time; FR24, FR01, NR02 and
EXHIBITION_FEST_003 fail (115.7/110.8, 107.7/102.7, 135.2/130.7,
212.5/179.0 s). Our front runners sit 1 to 3 s off the oracle's; the
loss is the tail (the oracle's field spans 3.8 s first to last, ours 10
to 15 s), from 3 to 6 resets a run after AI-to-AI contact (800 to 3,500
contact steps a race). Needed, from the oracle captures of those four
races: (a) how many AI resets ("hand of God") happen, per car; (b)
whether AI-to-AI contacts are softened or skipped (a collision scale,
mass ratio or ghosting between AI cars in the tunables or the records);
(c) EXHIBITION's Minis: anything that lifts or stiffens the AI ride on
that event (they bottom out braking into the dip at 2,700 m). Interim
meanwhile: the spec's reset rule as written, full AI-to-AI contact.

Answered in RACE_AI.md f9a514cc ("Contacts and resets (Q55)") and
CAR_PHYSICS.md ("The Mini 1566's ride"):

- (a) Zero resets (measured). No position jump (more than speed x step
  + 3 m, or a stop from above 8 m/s in one ~18 Hz sample) on any race car
  inside the race window of FR24, FR01, NR02 or EXHIBITION_FEST_003: 0 of
  7, 7, 1 and 7 AI. The only jumps are the cars' return to free roam
  after the results.
- (b) Not ghosted (measured); softened in spin (probable, from the
  settings' names; the code that reads them was not found). Race cars'
  centres came within 2.5 m in 14 AI-to-AI episodes (2 FR24, 3 FR01, 9
  Minis) but never closer than 1.5 m, and none led to a reset.
  PhysicsSettings.ini [Collision]: RaceCarTorqueScaleInRace 0.2 (race car
  vs race car in a race), VsTrafficInRace 0.05, PlayerTorqueScale 0,
  HumanCarAICarCollYawTorqueScale0 0.75. CarCarResitution -0.1 is
  misspelt in the file (the key is CarCarRestitution), so restitution
  stays at the default, 0. Interim: full linear contact, angular part of
  an AI-to-AI impulse x 0.2, restitution 0. After a reset: no car
  collisions for 3 s.
- (c) Nothing lifts or stiffens them. EXHIBITION_FEST_003 has UpgradeAI
  0, so t = 0, and AI upgrades never touch suspension anyway. The AI
  Minis use the stock spring row (level 0; rows 1566000/1566100): ride
  height 0.1703 m, MaxCompressHeight 0.0852 m, springs 39.808/22.392,
  bump 3.1408/1.7667, rebound 4.7112/2.6501, bump stop 200 with damping
  2.0, all x the car's mass (606 kg stock: 24.1/13.6 kN/m per wheel,
  bump stop 121 kN/m). Do not use the "upgraded" rows (level 3 is 0.1277
  m, springs 94.9/86.2). Use the body row's CMBackFront 0.5875, not
  WeightDistribution 0.64; this also corrects the Q53 Mini estimate,
  which does not change: 16.7 m/s^2 before transfer, cap trims 0.5 %.
  Whether the oracle Minis touch the bump stops at 2,700 m is not
  measurable (no suspension travel in the traces).

## Q56 (M6): where a Rally stage finishes

The ColoradoDirt AI lines run past the stage (route 7: line 5,297 m,
gantry at 623 m; route 24: 8,456 m, gantry 572 m), so SPEC-V4-CONTENT's
interim "progress 1" can't be the finish. The engine finishes at
gantry + `Tracks.FinishLine` x `Tracks.Length` (0.9821 x 4,636 m on
route 7), which lands within 150 m of `post_race_location_00` on the
seven routes checked; the last `route_waypoint` is 100 to 146 m off the
line on routes 1, 19 and 24. Needed: (a) the finish rule (a plane, a
fraction of what, measured from where); (b) the heats (track 1137,
route 24): its five best split times add up to 86.1 s (first split
0.968 s) and it has no `SplitPoints`, against about 130 s driven to our
finish, so does the heats end earlier, or are its timings another
measure? (c) 8 to 11 CAFF textures in ColoradoDirt's bin.zip fail our
layout check ("sizes do not fit the layout", e.g. `_0x00000b64.bin`);
is there a CAFF variant the world spec doesn't cover yet?

Q56 answered in horizon1-recomp docs/specs/V4_CONTENT.md ("Where a
stage ends", "The heats") and docs/formats/WORLD_MATERIALS.md ("CAFF
`.bin`"), 92071f5c (branch reimplementation-plan, 2026-11-21).

- (a) Probable: progress runs over the track's range [`StartLine`,
  `FinishLine`] (0 and 0.9821 on every Rally track); the stage finishes
  when progress reaches `FinishLine`, and split k sits at StartLine +
  (FinishLine - StartLine) x Split_k, so the last split ends with the
  range. From v4 code: the track loader keeps both values, the range
  setter takes them and the split inserter interpolates inside the
  range. No finish gantry, transform or trigger in the route file.
  Progress = distance along the AI line from the start line / `Length`
  is probable (festival fits), not traced on a stage. Your interim
  (gantry + 0.9821 x Length along the line) is the rule; keep it.
- (b) The heats do not end earlier. Track 1137 is track 1132's road:
  same waypoints, post-race location, identical AI line file, same
  Length 4,548 and range, so ~4,467 m; 130 s is 124 km/h, a normal
  stage pace. The 1137 timing row is placeholder data (86.1 s total,
  first split 1.1 %, 187 km/h implied; NumSplitPoints 4 but no
  SplitPoints rows, the stages are the reverse). Interim: no split
  display; AI times from the row's total by the spec's formula.
- (c) Not a texture variant: the texel offset is not fixed. The CAFF
  `.data` block size (u32 at 0x1C4) leads to the source-path length;
  the path follows 8 bytes later, then 0x40 bytes (texel size u32 at
  path end + 0x1A), then the texels at the next 4-byte boundary.
  Colorado's paths are 100 bytes (texels at 0x2B0); ColoradoDirt's are
  118 (texels at 0x2C4), so a fixed 0x2B0 reads every ColoradoDirt
  CAFF 20 bytes off with 20 extra bytes at the end, not only the
  failing ones. With the computed offset 1,821 of 1,824 fit the 2D
  layout exactly. The 13 non-power-of-two ones fit too: eleven 768 wide
  (`_0x00000b64` is 768 x 128, 10 levels, DXT1), 255 x 256, 7 x 256; if
  your check fails 8 to 11 of these, the width handling differs from
  tools/re/world-materials.py, not the file. The other 3 (`_0x000001a3`,
  `_0x00000793`, `_0x0000192a`; Colorado has 3 alike) are resource kind
  2 at 0x1AC, block size 0x3C, 256 x 256 DXT1, 196,608 bytes = six
  faces: probably cube maps, face order unverified.

## Q57 (polish): car lamps and their glow

No spec text covers car lamps. Engine interim: red covers and tail-lamp
techniques glow while a front wheel brakes, and cover tints (red, amber,
clear) are picked by technique name; the showroom reference masks leave
the lamps' additive glow out (IoU-g). Needed, narrow: (a) the brake-lamp
rule (brake input, threshold) and which techniques it lights; (b)
headlamps: when they are on (time of day, always) and what they draw
(the emissive cover only, a glow card/sprite with its texture and size,
light on the road); (c) the glow's intensity and blend against the post
chain's exposure. A shader/technique list with parameter names is
enough; no capture needed.

## Q59 (M6/UI, not blocking): skills, the chain and popularity

The HUD's skill chain and the popularity rank disc (947_HUD DrivingSkill
and PopularityBar; SPEC-UI-LAYOUTS b rows 115-116, states measured) are
laid out, but nothing says what drives them, so the engine draws neither
(interim: no skill scoring, no rank). Needed, narrow: (a) the skill list
(RaceFeats ids) and each skill's trigger and points (drift angle/speed,
draft distance, near miss, air, clean pass), from tunables if they are
data; (b) the chain: how points add up, the multiplier's steps, what
breaks or banks a chain (a contact, a time without a skill); (c)
popularity: points to rank (the 250-to-1 ladder and its thresholds), and
what the rank unlocks; (d) where the banked points go (the save field).
Tunable file names and the rules as tables are enough; no capture needed.

Q59 answered in docs/specs/SKILLS_POPULARITY.md 06dbea33 (horizon1-recomp;
spec a8cd57ba, status fix 06dbea33): 54 skill types from HorizonFeats.xml
with grades and points; multiplier x1/x2/x3 at 0/3/6 skills (data;
m x P at bank probable); cancel on a collision over 12, under 10 speed,
then 3 s lockout; bank timing not in the data (interim 3 s); ladder
Fame.xml 249 targets, 15,000 to 2,042,782; ranks gate 10 showcases
(Events.PopularityPointsReq) and pay the Fame sponsor; popularity points
guessed at the last float of the Player0 CPlayerState record.

## Q58 (M1/M5/M6): string tables and fonts

FORMAT-STRINGTABLES answered in docs/formats/STRINGTABLES.md ebae56f2
(horizon1-recomp): `.str` = big-endian `LSB2`, 36-byte header, strings
chunk of sorted (u16 key, u32 char offset) entries with a 0xFFFF
sentinel and a UTF-16BE pool; `_&N` = H(table) * 65536 + H(IDS_ name),
H a 16-bit rotate-XOR hash over the lower-cased name; load disc
`<LANG>.zip`, then v4 `stringtables/<lang>/`, then each pack's
`StringTables_pri_<q>` by ascending q, a later table replacing an
earlier one whole. 5,498 of 5,501 merged references resolve, all 250
`Data_Car` DisplayName and ModelShort. Missing-id behaviour, markup
rendering and the role of `stringtables.xml` are open with interims.

Font estimate (not decoded further): `media/ui/Fonts.zip`
`*_vector_aa.dt` are CAFF containers with a `vfont` asset: a code-point
hash table, metrics, 40-byte glyph records (237 glyphs, 303 in the
Cyrillic `*ru` variants) and half-float triangle meshes with curve
coordinates, coverage from u^2 - v in the shader. Neither outlines nor
an SDF. docs/UI_ASSETS.md already describes it, so a FORMAT-FONTS doc is
worth it and cheap: about 1 to 2 h, confidence high (glyph encoding
probable). Alternative: ship an open (OFL) font; quickest, but no
installed font matched the advances within 5 %, so layout would shift.
CJK text uses separate bitmap fonts in `media/ui/fonts` either way.

FORMAT-FONTS answered in docs/formats/FONTS.md b399812a
(horizon1-recomp): 16 fonts, 2,421 glyphs; code-point table slot =
code point (size = largest code point + 1); 40-byte records; meshes of
(x, y, u, v) half floats with u16 indices; coverage
clamp(0.5 - s(u^2 - v)/|grad|, 0, 1); no kerning; `*ru` fonts are
strict supersets.

Q45 FOV check answered in CAMERAS.md 4b03a066: 427/1280, vFOV 48.5 (full vertical angle, not half), confirmed

FORMAT-UISCENES answered in docs/formats/UI_SCENES.md 8f4a5c61: partial go
(probable). Tree, BSG transforms (centre origin, y up, 1280x720 px),
unit quads, textures (Horizon.zip / Textures.zip .xds), materials and
text boxes (font, px size, width) read for 204 of 205 scenes; text
content, rows, stacker reflow, states and animations come from the C++
contracts at run time (no string ids in any scene). Projection, BGF
properties and non-quad geometry open.

SPEC-UI-LAYOUTS answered in docs/specs/UI_LAYOUTS.md 793f9ff9

AI steering answered in RACE_AI.md 89fa4414: no speed-sensitive lock (wheel = 41.85 deg x steer, only the full lock bounds it; Minis use up to 13 deg at speed, mostly counter-steer); fit delta = 0.27 alpha + 0.29 beta, alpha the bearing of the line point 1.0 s x v ahead and beta the slip, both from the body heading (probable, R2 0.69); interim: that rule, no cap below the full lock.

UI states answered in UI_LAYOUTS.md af0e37b6; captures in out/oracle/ui

UI follow-up Q1-Q3 answered in docs/specs/UI_SCREENS.md 943ecc1f: HUD 947_HUD, pause 925_PAUSE_MENU, map H_MAP, help bar BUTTON_BAR_SCENE, finish H_POST_RACE_FINISHED then H_POST_RACE_RESULTS (probable); 7xx and split-screen scenes never open in retail; 63 scenes unreferenced; text node = alignment point (left/centre probable, right guess) at the line's vertical middle; material floats 0-3 = diffuse RGBA (confirmed), tint and crop rule 0 <= s*u + o <= 1 probable; pre-race and post-race pages interim, four frame checks listed.

UI follow-up correction in docs/specs/UI_SCREENS.md 3ec78780: only 708_PAUSE and 710_POST_RACE are proven never opened (948_HUD_Split_Screen and other 7xx scenes have unread openers); ResPack.xml packs confirm the HUD group (947_HUD, 925_PAUSE_MENU, H_MAP, H_BARN_FIND_HUD) and list PRE_CAREER_EVENT_TITLE with PRE_AND_POST_RACE in PreEvent, so 62 scenes are unreferenced; finish/results order not established; pause 53-unit vs 50 px pitch read as stacker reflow (lay out stacker rows in the engine); baseline = y - (ascent - descent)/2, which middle box interim.

## Q60 (M6, not blocking): where the profile keeps popularity

SPEC-SKILLS-POPULARITY ("The save") and Q59 (d) guess the last f32 of
`Player0` (`CPlayerState` v13). That reads 33,649 on both reference
exports, rank 245, yet `user` has won the showcases gated at ranks 225,
200 and 175, so the guess cannot hold. `Main` has no popularity field.

**Engine reading (probable): the first u32 of `SponsorshipState`**
(`CHorizonSponsorshipState` v2). The payload is a 20-byte header (u32,
a zero word, f32, u32 2, u32 child count 15) and 15
`CSponsorshipChallengeState` v1 children of 8 bytes. Evidence:

- the header word reads 811,260 on `user` and 1,689,100 on
  `user-modded`: ranks 115 and 28 on Fame.xml;
- the `Fame` child's first word is 5 and 8: exactly the Fame-sponsor
  tiers at or above those ranks (225 to 125, and 225 to 50);
- on the other 28 children (14 per save) the first word equals the number
  of SponsorshipChallenges.xml ranks the second word reaches, so a child is
  (tiers claimed, count), and the header and children belong to the
  popularity and sponsor system;
- ranks 115 and 28 reproduce the saves' showcase rows in
  `CareerEventUnlocks` exactly (6 and 9 of the 10; any rank in 101 to 125
  and 26 to 50 would, and rank 245 would not);
- the class's v2 serializer writes the first field and the literal 2
  through the same integer writer, and the two following fields through
  another (the f32s), so the first field is an integer, as the Fame
  targets are whole numbers.

Not yet compared with a rank the game displays. The f32 at header +8
(246.66 and 26.79) and the zero word are kept as read; their meaning is
open. The engine reads and writes the value in one place
(`profile::popularity_points_from_sponsorship_state`,
`profile::encode_sponsorship_state`) and the corpus test
`save_import_corpus_tests` checks both saves. Needed, narrow: one
displayed popularity rank (pause menu or HUD disc) next to the header
word of the same save, or the reader of this member in the generated
code. Also open: `CHorizonDialoguesState` (hashed line keys, kept as
bytes; the engine seeds its own heard-dialogue keys from the career
instead). `CFirstTimeCareerState`'s stage is answered by Q66: the value
is the activity's stage, and a finished save reads 8 because the drive
to Race Central is the last stage that saves (Race Central, 9, never
does).

## Q61 (M6/INTRO, not blocking): cutscene camera details

libs/cutscene reads the CutsceneManager documents and plays them with the
original's key rule (Hermite time-to-index map, Catmull-Rom pose spline;
read from the camera player). Interims, narrow: (a) roll sign (applied
positive = top leans right; no disc cam framing evidence); (b) FOV taken
as the vertical angle like the driving cameras; (c) post-effect keys
(DOF, exposure, saturation) interpolated linearly; (d) the FadeOptions S
curve taken as smoothstep; (e) CWhiteFlash shown as white fading over
0.1 s (about 6 frames, UI_LAYOUTS) and CBlackOut as an instant cut held
until the next fade or cutscene; (f) PlacePlayerCar `initialSpeed` unit
and `timeTillRelease` (**answered 2026-11-25**: mph; the place-car
trigger scales it by 0.44704 into the car's velocity along its heading,
and `timeTillRelease` is when the parking brake lets go; the placement
keeps the authored x/z and heading and drops the car onto the surface
nearest the authored height, ray +-1000 m, normal.y >= 0.2; PartSpace
cams are relative to the named car part, and to a target node with the
part ignored); (g) whether in-engine cutscenes can be skipped (the engine does not skip them; FMV_01/FMV_02 skip with
confirm, as `confirmskip="true"`). A trace of the post-effect blend and
the white-flash state, or a capture at a cut, answers (a)-(e).

## Q62 (M6/INTRO): scoring the race to the festival

**Answered 2026-11-25** (read from the first-time-career activity's update
and its zone setup in the generated code; implemented in libs/race
first_time_career.*). The "Heats Places Remaining" counter is not a
distance: it starts at 10 and each TicketTrigger the player enters takes
one place, never under 1; after the player passes the last stretch's
zone (around -701, 46, -1501, 20 m), non-traffic AI cars reaching the
EndRacePosition zone take places too. First50TicketsDistances and
RacePointsRequired are loaded and never read, so (b) the race cannot be
lost. (c) RadioDJTriggers 1001-1005 and the FreeTicketsTrigger (1003)
play nothing (no RadioDJ request has those ids; 1005 is commented out in
the file); the FreeTicketsTrigger re-routes the sat-nav from TollBooth to
BeyondTollBooth. The DJ's FestivalTicketsDepleting<count> lines play once
each, only before the last stretch and only when the DJ is idle; at one
place left the giveaway reminders follow; the last stretch's zone stops
the DJ. The counter's HUD (CHudStatusTicketCountdown) is created as the
race starts (stage 2 to 3) and removed at the festival. Engine interim,
narrow: the AI cars' places (the engine's traffic has no festival
racers on that road yet) and the reminder delays (30 s and 60 s).

## Q63 (M6/INTRO): tutor prompts in the first drive

**Answered 2026-11-25** (the driving tutor's update in the generated
code; implemented in libs/race driving_tutor.*). The tutor is a state
machine over the CTutorialMessages hints, table HelpButtons
(IDS_InputHint_<X>_L "PRESS", the button glyph, IDS_InputHint_<X>_R):
accelerate from the hand-over until the throttle passes 0.01; then the
brake hint after BrakeDelay (10 s) for BrakeMessageTimeout (5 s); then
idle, where a crash with rewind available shows the rewind hint for
RewindMessageTimeout (5 s; cool-down 30 s, ten times at most), and over
50 mph within 110 m of a sat-nav left or right turn shows the e-brake
hint for EBrakeTimeout (6.5 s; cool-down 30 s, ten times), followed once
by the camera hint (ChangeCameraMessageTimeout 5 s, a crash after 2 s
switches it to the rewind hint). Using rewind or the e-brake ends that
hint for good. The tutor is activated at the hand-over and nothing turns
it off. The FM4-era strings (IDS_Accelerate_Message and the like) are not
used. The map hint (IDS_InputHint_Map_*) replaces the sat-nav popup when
Kinect is off, after the first Autoshow. Engine interim, narrow: the
turn test reads the guidance's next manoeuvre as "left or right, normal
or sharp", and the map hint stays up 5 s.

## Q64 (M6/INTRO): the first Autoshow restriction

**Answered 2026-11-25** (the restriction's state in the generated code).
CSetRestrictionsForFirstAutoshow true limits the car list to Id IN
(1323, 1009) (SUB_WRXSTi_11, MIT_LancerEvoXGSR_08), ordered by class
then PI; the preview car is 1323; Back is disabled and hidden, so the
player must buy; the "keep shopping?" prompt after buying is skipped.
The engine's Autoshow tutorial mode lists those two; interim, narrow:
the order and the preview car follow the tutorial mode's own sort. The
AutoshowPreFirstYellowEvent VO state is not referenced.

## Q65 (M6/INTRO): the opening credits

**Partly answered 2026-11-25.** ShowOpeningCredits opens the scene
H_OPENING_TITLES (a CEmptyScene) while New_Diner_Cutscene_2 plays; the
scene holds only textures (OPENINGTITLES: TITLEMICROSOFT, TITLETURN10,
TITLEPLAYGROUND, HORIZONLOGO and their shadows), no text. It stays open
after its state exits and closes with the HUD as the race to the festival
starts (probable). Still open: the timing and placement, which are the
scene's BGF animation (not decoded). Engine interim: the four cards in
turn, 4 s each with half-second fades, centred.

## Q66 (M6/INTRO): stage order and triggers around the Heats

**Answered 2026-11-25** (the first-time-career activity's update and
save in the generated code; implemented in libs/race first_time_career.*).
The activity's stage value runs: 0 the opening and the initial drive;
1 the diner; 2 the drive set-up; 3 the race to the festival; 4 and 6 the
festival arrival flow; 7 the Horizon Heats; 10 then 12 the Red Rock
showcase; 8 the drive to Race Central; 9 Race Central. It saves at
stages 1, 7, 12 and 8 (9 is never saved, so a finished career reads 8).
(a) The controller loading screen leaves when the opening has loaded, on
no timer or button (probable). (b) The initial drive ends in either of
the diner's arrival spheres ((1709, 117.8, -1718) and (1486, 115, -2024),
10 m); the approach spheres before them (20 m) remove the traffic and
load NEW_Diner_Cutscene_1. (c) The race ends in a 20 m sphere at
EndRacePosition or at (-945.9, -1, -647.2). (d) The heats are driven to:
a 27 m sphere at the FESTIVAL_SECOND_NODE race activation signs up the
final (AllComerFinal 77; Race1ID 76 has no Events row); a result place
under 4 moves on, else the final is retried. Then the activity places
the player by the heats at 15:48 and the Red Rock showcase (MustangRace
101) follows the same way; after it the player is placed by the plane
race and drives to Race Central. (e) Race Central's arrival is the Race
Central activity's own callback. (f) Travel_To_Workshop is stage 5's
instruction (unused by the flow). While the career holds the controls
the player's car is driven by a race behaviour on the opening's route.
Engine interims, narrow: whether the result place is 0- or 1-based (the
engine reads 1st to 3rd as passing); Race Central's arrival radius (the
catalogue's site radius); the AI drives the sat-nav route rather than
route 170's own path; the two time-of-day zones ((-116, 107, -3377) and
(2284, 109.9, 398.1), ramping to 18:00 and 14:30) are not implemented.
The initial drive's sat-nav has three points (CSetDestForInitialDrive);
the engine's sat-nav takes one destination, so the career routes leg by
leg, (1552, 1436) then (1881, 1077) then the diner point, each leg ending
60 m short of its point (the order is read from the road: CO-20 passes
the first about 490 m before the second; the code's order of use is not
decoded). The opening's AI plans its speed on the rounded-off sat-nav
line at 6.5 m/s^2 of cornering, capped at the 100 mph PlacePlayerCar
gives it (the original's race-behaviour speed is not found), and only
the opening's cutscene keeps the car's physics running under it (the
diner cutscenes and Festival_Race_Outro keep the paused sim);
`timeTillRelease`'s parking brake is not applied (Q61 f).

## Q67 (polish, not blocking): the camera's collision pulses

SPEC-CAMERAS has the `WorldCollision` and `CarCollision` inputs as pulses
"injected directly into the layer pipeline on impact events" and leaves
their unit and the impulse outputs' firing rule unread (Unknowns,
`InputType 1..34` sub-branches). The stock `CameraPhysics.xml` collision
layers clamp their inputs at about 3 to 12 and bend their curves at 0.25,
0.75 and 1.0 to 5.0, sizes that fit an impact speed in m/s.

**Engine reading (ours, until decoded):**

- `drive::Vehicle::last_body_impact()`: a body contact that begins after
  a step (1/120 s) or more without one is an impact; its size is the
  largest closing speed along a pushing contact's normal at that substep
  (m/s). carview feeds each impact once, for one frame, split along the
  car's forward and right axes; car-to-car impacts (traffic, race AI) are
  fed for one frame and then spent.
- An impulse output (`*Impulse`) adds its mapped value to its spring's
  velocity once per rising edge of the input past `ImpulseInputSizeCutoff`
  (with a non-zero mapped value) and re-arms when the input falls back, so
  an attack/decay follower's tail does not fire it again every frame.
- No other limit: the PT-28 dead zone, refractory period, value and
  offset clamps and forced damping are gone, and the chase heading spring
  takes the raw yaw rate again (Q51's replay: direction 0.40 deg rms
  with the low-pass, 0.20 deg rms without it).

With the stock `CameraPhysics.xml`, no chase-camera collision layer is an
impulse output. They are offset layers behind attack/decay followers
(attack 289.5 to 12000 per s), and both signs of each axis reach one. A
one-frame pulse rises at most attack/120 per frame, so on the chase
cameras every impact from 5 to 25 m/s gives the same kick against a
no-impact run: about 0.7 deg for lateral and 0.2 to 0.33 deg for
longitudinal, with the eye moving 7 mm. A glancing scrape along a wall
(0.02 to 0.15 rad) and a head-on hit at 8 or 28 m/s each make exactly
one impact.

Needed, narrow: the writer of the camera collision input channel (its
unit, and how many frames a pulse lasts, which the followers' attack
rates make matter), and the impulse branch of the layer update (edge,
level or force).

(2026-11-25) The executable's InputType name table gives the ids:

| Id | InputType |
|---|---|
| 0 | ConstantOne |
| 1 | Gs_Lateral |
| 2 | Gs_Longitudinal |
| 3 | WorldCollision |
| 4 | WorldCollision_Lateral |
| 5 | WorldCollision_Longitudinal |
| 6 | CarCollision |
| 7 | CarCollision_Lateral |
| 8 | CarCollision_Longitudinal |
| 9 to 11 | Slip, Slip_Lateral, Slip_Longitudinal |
| 12 | Speed |
| 13 | SpeedMPH |
| 14 | RPM |
| 15 | SuspensionBias_Lateral |

The writer is the code that fills ids 3 to 8 for the layer update. It
is not traced yet.

## Q73 (UI, not blocking): the dial's rpm scale

The HUD dial (`947_HUD` `GAUGE_ANALOG`) has 12 numeral slots, 12 large,
12 mid and 24 small hatch slots, each under a `rotation` group the game
turns at run time; the scene does not say how many it shows or where.
Read off two retail captures: the Mazda MX-5 (redline about 7,000 rpm)
shows 0..8 in ones on a sweep that runs one step past the 8 (0..9, about
0.52 rad a step from 2.34 rad left of the top); the Nissan Skyline GT-R
R34 (redline about 8,000 rpm) shows 0..10 in twos over the whole sweep.
The engine's rule (`dial_scale()` in `apps/carview/hud_screen.cpp`): the
sweep spans the redline in thousands rounded up plus two; up to 9 it
counts in ones with numerals to one below the end, above it in twos to an
even end. Needed, narrow: the code that fills the dial's slots (the rpm
range it reads per car and how it picks the step and the last numeral),
or captures of a car with a redline of 6,000 or 9,000 rpm and up.

## Q74 (UI, not blocking): the race start count without a gantry

At a festival start the retail game counts down on the start gantry's
LEDs (0:05..0:01, then GO) and shows nothing on screen until the signal,
when `947_HUD` `I_321_Go`'s `GO` text shows GO! (`15_countdown_orig.png`,
`15b_countdown_go_orig.png`). The node `I_321_Go/Group/TEXT_COUNT` is laid
out at full opacity but nothing in those captures fills it. The engine
runs the world clock that the gantry LEDs read from the race clock, and
its gantry panels now light at the FR16 start. They had been turned to
face their normals, which point into the gantry, and were back-face
culled; the file's own order is culled too, and with the corners
reversed they light. Only the FR16 gantry is checked. So the engine, like
those captures, leaves `TEXT_COUNT` empty and shows only GO!.

Unknown: whether a start with no gantry (a street race, a point-to-point
started on an open road) fills `TEXT_COUNT` with 3, 2, 1. The engine's
interim there now counts 3, 2, 1 on `TEXT_COUNT`, then GO! (see Q173).
Needed, narrow: a capture of such a start, or the
code that writes `TEXT_COUNT`.

## Q68 (FEEL-11, not blocking): FrictionTorqueMod selection, and our AI

The engine now runs the original's FrictionTorqueMod as decoded. Each
substep it sums the four tyres' friction torque (not the suspension
force's) and takes its yaw part T about the body's up axis. It picks the
table: SimulationSteering in Simulation steering, else WithAssists or
NoAssists. It multiplies in the drivetrain row and, when all four wheels
are off-road, the off-road row, then blends the Mass0KG and Mass1KG rows
by the car's mass. The yaw factor runs from 0 at CosYawAngle0 to 1 at
CosYawAngle1 (the cosine of the angle between the car's forward axis and
its flat velocity). The speed factor runs from 0 at 8 m/s to 1 at
15 m/s. The Turn pair applies when the yaw rate and T have the same
sign; otherwise the Straighten pair applies. The result is
T * lerp(1, Scale, speed), clamped to
Clamp deg/s^2 * yaw inertia * lerp(10, 1, speed).

Open points:

- (a) **Drivetrain rows.** The drivetrain-row selection falls through for
  every layout, so the AWD rows (all 1) are always taken. The ini's FWD
  rows (TurnClamp0 0.7778, ...) are never read. The engine follows the
  code. Needed: confirm whether this is a shipped bug or whether we have
  misread the layout value.
- (b) **WithAssists or NoAssists.** Two car flags choose between them.
  One is the TCS yaw-cosine hysteresis state; the other is not
  identified, and ours reads it as "TCS or STM on". The shipped rows are
  equal, so no behaviour depends on this.
- (c) **Off-road rows.** They apply when all four wheels have
  OffRoadness > 0. The engine's old reading was >= 0.5.
- (d) **Yaw inertia (probable).** It is read from the rigid body's
  inertia vector. The oracle lap's peak yaw acceleration is 2.25 rad/s^2,
  and none reaches 3. That fits the clamp band for the R34 (about 116 to
  151 deg/s^2). The engine without the mod reaches 5.75 rad/s^2.
- (e) **Interim: AI-driven cars skip the mod in the engine.** The
  original runs it for every car. On the oracle lap the AI's results
  were:

  | Tables | Lap time | Note |
  |---|---|---|
  | Identity | 39.312 s | Same as before the port |
  | Scale only | 39.314 s | |
  | Clamp only | 39.558 s | |
  | Full mod | 47.146 s | The AI spins at about 850 m |

  The cause is in our suspension. At about 850 m the wheel loads chatter
  at about 8 Hz, with one wheel going from 5 to 21 kN in a few substeps.
  That swings the raw tyre yaw torque by +-20 kN m. The Turn and
  Straighten clamps differ (151 against 116 deg/s^2 at the R34's mass),
  so the swing is rectified into a net turning torque. The original's
  yaw rate on the same stretch is smooth. Needed: nothing from the spec.
  The exemption comes off once the load chatter is fixed.

  Re-measured with the sphere body solver (Q100, Q101). Without the mod
  the AI laps in 39.246 s. With the full mod it no longer finishes: it
  spins braking into the turn at about 855 m and stops at 904 of 1199 m
  (DNF after 77.6 s). Logged per substep, the chatter is still there
  (single wheels swing by 10 to 40 kN within 0.1 s from 24 s on, and the
  raw yaw torque by +-50 kN m). What turns it into the spin is the
  interim wheel load cap of Q101, max(4 x preload, 1.1 m g): at 855 m the
  right-hand wheels hit it, the rear goes light and the car spins. With
  that cap removed (the 0.14 m bump-stop cap kept) the AI finishes with
  the mod in 41.532 s (+7.09 %, cross-track max 5.34 m); removing only
  the 0.14 m cap still DNFs. Without the mod, removing both caps leaves
  the lap at 39.258 s.

  The load cap now applies only to a wheel more than 5 cm into its bump
  stop (Q101). The lap's bump-stop hits at 855 m reach 1.4 cm, at
  under 1 m/s, so they are no longer capped. The kerb test is unchanged
  (2.696 m/s up). With the mod the AI now finishes, but in 41.532 s
  (+7.09 %; target 38.78 s +-1 %); without it, 39.258 s. The lost
  2.7 s is almost all between 840 and 1040 m, where the AI weaves
  through the left-right transition (yaw 9 deg off at 800 m, then 55 and
  -19 deg where the run without the mod reads 37 and -3 deg) and runs up
  to 7 m/s slower. That is our AI controller against the mod's unequal
  clamps on the chattering loads, not the caps. The exemption stays.

  (2026-11-25) Both caps are now gone (Q101's decoded wheel query and
  bump stop). Superseded: "The load cap now applies only to a wheel
  more than 5 cm into its bump stop". With the mod on every car, the
  oracle lap reads 38.980 s. The AI gate showed no regression from the
  caps' removal, and leans slower on NR02 and FR22 once the spring stops
  at the travel (Q101).

  **Resolved (2026-11-25): the exemption is gone.** The controller that
  weaved was the oracle lap's pure-pursuit follower. That follower is
  the M3 interim, not the title's AI. The lap now steers by the AI's own
  rule (SPEC-RACE-AI, Steering at speed: delta = 0.27 alpha + 0.29 beta,
  with the line point 1.0 s x v ahead, at least 2 m, and only the lock
  as a bound). Every car runs the mod. The R34 lap times, with the AI's
  TCS and STM on (Q122):

  | Steering | Mod | Lap time |
  |---|---|---|
  | Pure pursuit | off | 39.258 s (+1.22 %, before Q122) |
  | Pure pursuit | on | 40.549 s (+4.55 %) |
  | AI rule | off | 38.568 s (-0.56 %) |
  | AI rule | on | 39.003 s (+0.57 %) |

  The last row is within the +-1 % target, with a cross-track maximum of
  3.38 m. From 840 to 1040 m the yaw stays within 6 deg of the oracle's;
  pure pursuit with the mod was up to 22 deg off. `SHIFT_LAP_STEER=pursuit`
  brings back the old follower.

  The race gate (seed 1, AI mean time) moved with the mod and the AI
  rule together, so they land together with Q122. The mod alone, under
  the old capped and rate-limited race steering, made NR02 170.6 s with 3
  resets (from 138.6 s, 0 resets) and FR22 185.7 s (from 172.6 s).

## Q69 (FEEL-16/17, not blocking): the pad steering filter's law

The engine's pad filter is now the parts SPEC-CAR-PHYSICS names, plus
its own keyboard filter. Those parts are the speed-sensitive lock, the
rate scaled toward FastRateScale, SteerAngVelCountersteer while the
stick holds against a rear slide, and the SteerAccelTimeToMaxRate ramp.
Three engine-only helpers are now an opt-in extra (ENGINE tab,
"steering helpers", off by default): the Assisted lane-centring bias,
the injected countersteer angle and the front peak-slip pull. The
per-mode lock factors (Simulation 1.25, Assisted 0.90) are gone.

Still ours (interim):

- **The lock law.** It is atan(1.61 * MaxGees * g * wheelbase / v^2)
  between MinMaxAngle and the mode's cap. The 1.61 is a fit.
- **The countersteer trigger.** It fires at a mean normalised rear slip
  above 0.5. Settings carry NormRearSlipAngleToCountersteer 50; its unit
  is not read.
- **SteerAngVelDynFindPeak (10 deg/s).** It is unused unless the helpers
  are on.
- **SlowSpeed and FastSpeed units.** They are read as m/s (FEEL-17).
  Every car ships SlowSpeed 1.0, FastSpeed 120.0 and FastRateScale 1.0.
  In the engine the two speeds only blend the rate toward FastRateScale,
  so with the shipped rows the unit changes nothing today. It matters
  once the lock law is the spec's u_spd narrowing toward MinMaxAngle:
  120 m/s, 120 mph (53.6 m/s) and 120 km/h (33.3 m/s) give very
  different curves. The oracle lap cannot settle it, since the AI
  bypasses the pad filter. The name has no unit suffix, while the
  tables that use mph say so (FrictionLowSpeedMph,
  MaxForwardVelShiftReverseMPH).

~~Needed: the pad filter's narrowing law (which G it queries)~~ (decoded
below), the unit of NormRearSlipAngleToCountersteer, and what
DynFindPeak drives.

**The lock law (decoded, 2026-11-25).** The steer row sits in the car
in its file order: MaxAngle, a field not identified, MaxAngVelTurning,
Straighten, Countersteer, DynFindPeak, AccelTimeToMaxRate, MaxGees,
MinMaxAngle, SlowSpeed, FastSpeed, FastRateScale.

- **The curve, built once per car.** There are 32 samples up to 67.056
  m/s (150 mph), at 0.4623 samples per m/s. Sample i is taken at
  v = max(0.01, i / 0.4623). It is atan(MaxGees x L x a(v) / v^2),
  clamped between MinMaxAngle and MaxAngle. L is a car length, probably
  the wheelbase, whose writer is not found.
- **a(v).** This is the G the law queries: the car's own lateral grip
  acceleration at that speed, not g. The steering model works it out
  from the tyres. It iterates the lateral load transfer six times. Each
  wheel's peak friction comes from the tyre's light-to-heavy load
  blend, and the wheel forces are summed and divided by the mass. The
  speed and downforce inputs are not fully traced.
- **Each frame.**
  - The lock is MaxAngle, capped by the mean steering-lock cap of the
    surfaces under the wheels in contact (Q70).
  - The narrowed lock is the smaller of that lock and the curve at
    |velocity|. The curve is read by a four-point cubic through the
    samples around the speed, then multiplied by the surfaces' mean
    third steering factor.
  - The result is clamped between min(MinMaxAngle, lock) and the lock.
    The target is the stick times it.
- **The rate.** MaxAngVelTurning is scaled from 1 toward FastRateScale
  as |velocity| goes from SlowSpeed to FastSpeed. The speed is the same
  m/s field the curve reads, so SlowSpeed and FastSpeed are m/s unless
  the row loader converts them (FEEL-17, probable). The rate is also
  scaled by the surfaces' mean rate factor. A test of |stick| against
  0.976 is on this path; its use is not decoded.

The engine keeps its interim lock law. Its 1.61 x g stands in for a(v),
and the engine has no per-speed table. Porting a(v) needs the steering
model's load-transfer iteration, which is decoded only in outline.
Supersedes, in Q69's Still ours, "The 1.61 is a fit": it is a fit to
a(v) / g.

## Q70 (FEEL-15, not blocking): the wheel path's mapping

The SteeringWheelFilter block of PhysicsSettings.ini (Speed 10/50,
ControlScale 1.5/3.0, CorrectSpeed 5/25, CorrectControlScale 1.0/2.5)
is dead data:

- the settings loader never binds its keys;
- the strings "CorrectSpeed" and "CorrectControlScale" are not in the
  executable.

The engine no longer reads it, and the pad no longer squares the stick
by it.

The wheel's steering goes through a WheelMapping step instead. It has
four tables, picked by steering mode and 900-degree option:
WheelMapping, 900Degree, SimSteer and 900DegreeSimSteer. Their fields
are WheelMapSteerSpeed0/1, SteerScale0/1, NormSteerInput1/Output1 and
CounterSteerScale. None of these keys are in the shipped ini, so their
values are the executable's built-in defaults, which are not game files.

~~Engine interim: the wheel maps linearly to full lock (steer x
SteerMaxAngle) with no rate limit, as before. Needed: the WheelMapping
law and its default values from a decode of the settings constructor,
or a wheel trace.~~ Superseded by the decode below (2026-11-25).

**Decoded (2026-11-25).** The settings constructor's defaults:

| Table | SteerSpeed0/1 (m/s) | SteerScale0/1 | NormSteerInput1 / Output1 | CounterSteerScale |
|---|---|---|---|---|
| WheelMapping | 0 / 30 | 1.0 / 0.35 | 0.14 / 0.07 | 1.0 |
| WheelMapping900Degree | 0 / 30 | 1.0 / 1.0 | 0.14 / 0.14 | 1.5 |
| WheelMappingSimSteer | 0 / 30 | 1.0 / 0.35 | 0.14 / 0.07 | 1.0 |
| WheelMapping900DegreeSimSteer | 0 / 30 | 1.0 / 1.0 | 0.14 / 0.14 | 1.0 |

The table is the SimSteer pair in Simulation steering, otherwise the
plain pair, and the 900Degree one of the pair when the 900-degree
option is on. The law, each frame:

- **The input.** Its magnitude maps piecewise-linearly through (0, 0),
  (NormSteerInput1, NormSteerOutput1) and (1, 1); the sign is kept.
- **The scale.** It is 1 below 2 m/s forward speed, and 1 while the
  input is at or across centre from the road wheels' current angle. It
  is CounterSteerScale in a countersteer state above 2 m/s. Otherwise
  it runs from SteerScale0 to SteerScale1 as |velocity| goes from
  SteerSpeed0 to SteerSpeed1 (m/s; the same speed field the pad filter
  reads).
- **The target.** It is the steering model's lock x the scale x the
  mapped input, clamped to +-lock.
- **The slew.** The road wheels move to the target at up to 4 pi rad/s.

So a 900-degree wheel is linear to lock with a 1.5 countersteer scale,
and a wheel with the option off has a soft centre (0.14 in gives 0.07
out) and narrows to 0.35 of lock by 30 m/s.

The engine now runs this law (`Input::wheel_900`, on by default; the
carview wheel path does not yet pass its 900-degree setting). Interim:

- ~~**The lock.** It is SteerMaxAngle. The original asks the steering
  model for it through a virtual call that is not decoded.~~ Decoded:
  the steering model's lock is SteerMaxAngle capped by the mean of the
  steering-lock caps (degrees, converted) of the surfaces under the
  wheels in contact, 9999 when none is. The wheel path takes it without
  speed narrowing. The engine now does the same.
- **The countersteer state.** It is off, so CounterSteerScale never
  applies. A guess would switch on the 900-degree table's 1.5 scale,
  which is the only table carview's wheels reach. The original tests
  the sign of the road wheels' angle against the same field of the two
  front wheel blocks. It counts as countersteer when the angle is
  positive and the first front wheel's field is negative, or the angle
  is not positive and the second front wheel's field is positive. The
  field is set at car setup from a car vector. No per-frame writer turns
  up in a search of plain stores, so it may be written by a vector
  store.

Needed: what that front-wheel field is.

## Q71 (FEEL-12, not blocking): new-profile assists and the presets

ForzaProfile.sch's career assists have these defaults: MetaDiff 1,
SuggestedLine 0, Braking 1, Steering 1, StabilityControl 1,
TractionControl 0, Shifting 0 and Rewind 0. The game's assist label
functions give the polarity:

| Field | 0 | 1 | 2 |
|---|---|---|---|
| TractionControl | On | Off | |
| StabilityControl | On | Off | |
| SuggestedLine | Full | Braking | Off |

A new profile is therefore Medium: ABS On, Normal steering, TCS On,
STM Off, Automatic, the full line and Rewind On. The engine now has the
same defaults in each of these places:

- AssistSettings and `retail_defaults`;
- the settings rows ("traction control" defaults on and is turned off
  with `--assist-tcs 0`; "stability control" defaults off and is turned
  on with `--assist-stm 1`);
- the MEDIUM preset and its detection;
- race::DifficultyAssists.

The engine's own driving-line enum (0 Off, 2 Full) runs the opposite
way to the profile's. That only matters to a profile importer, and the
engine has none.

Still interim:

- **Preset list.** MetaDifficulty has six presets: Easy, Medium, Hard,
  Extreme, Insane and Custom. The engine has Easy, Medium, Hard, Insane
  and Custom.
- **Preset assists.** Only Medium is backed by data. The assists of the
  other presets are the engine's.
- **Values in code.** The defaults are written in code rather than read
  from the schema at run time.

Needed: the table that maps MetaDifficulty to assists.

(2026-11-25) Not found as data. A scan of the executable's image looked
for 5 or more rows of small values, as bytes or 32-bit words, at row
strides of 7 to 10, with the second row equal to Medium's values (three
1s and four 0s). It finds only monotonic ramps and unrelated bit
patterns. The presets are likely set in code by the assists popup's
handler (the spinner list that carries the labels), which is not
traced.

## Q72 (FEEL-26, FEEL-27, FEEL-31, not blocking): look-around, sputter rumble and camera blur

Three parts of the driving feel now follow the original's data, but
each still has an interim rule.

**Right-stick look (FEEL-26).** The right stick picks one of the
original's look targets in every camera: LookBack (pi), LookLeft or
LookRight (pi/2). The stick click is LookStraightBack. The chase
cameras no longer orbit continuously. Still interim:

- **Threshold.** A target is picked past half deflection on the
  dominant axis, and a diagonal counts as left or right. The original's
  stick thresholds are not decoded.
- **Spring rate.** The look spring is the engine's 0.1 s
  (SPEC-CAMERAS).
- **Invert setting.** The profile's `InvertCameraLookBack`
  (`forzaprofile.sch`) is not read yet.

**Engine sputter (FEEL-27).** OnEngineSputter rumble now comes from
engine damage, not the rev limiter. PhysicsSettings.ini gives
`EngineDamageLevelStartSputter 0.3` and
`EngineDamageMaxSputtersPerSec 1`, and both are read at load. Still
interim:

- **Rate rule.** Sputters are evenly spaced. Their rate rises linearly
  from 0 at the start level to the maximum at full damage. ~~The
  original's rule between those two values is not decoded, and it may
  be random.~~ Decoded (2026-11-25), and only the spacing differs:
  - **The rate.** Each frame, the rate is EngineDamageMaxSputtersPerSec
    x clamp((damage - start) / (1 - start), 0, 1), with damage clamped
    to 0..1. It is halved unless a car setting next to the steering mode
    reads 2; that setting is not identified (possibly the damage mode).
  - **The trigger.** A sputter starts when a uniform random draw is
    below rate x dt. The draw comes from the car's own linear
    congruential generator (x' = x x 1103515245 + 12345, 15 bits).
  - **The sputter.** It lasts 0.08 s, and for that time the throttle is
    forced to 0.

  So the original sputter is a 0.08 s throttle cut at random (Poisson)
  times, not only a rumble event. The engine has the even spacing and no
  throttle cut.
- **Damage level.** The damage level is the engine's own damage model
  (FEEL-19 to FEEL-23, owned by another branch).

**Camera motion blur (FEEL-31).**

- **Strength.** Strength is TrackSettings.xml `motionBlurAmount` × the
  car speed over `DetailBlurMPHForMaxBlur`, capped at 0.95. The engine's
  0.55 floor and 18 mph offset are gone, so the free-roam baseline of 0
  means no blur.
- **Direction.** On Tier 0, the blur is six taps along the camera's
  screen motion from its rotation against the world over the last
  frame (the view-projection composed with the world-to-car transform),
  which smears sideways in turns. This replaced the radial blur, which photo mode
  still uses for its shutter effect. The velocity-blur tiers already
  follow per-pixel motion.

Still interim:

- **Dynamic amount.** The original changes the amount at run time
  (CDynamicRenderMotionBlurControl), and that rule is not decoded. So
  races or modes that should blur from a 0 baseline do not.
- **Frame-rate scaling.** The motion is per rendered frame, so the
  smear shortens at high frame rates. The original runs at 30 fps.
- **Car mask.** The player car is kept sharp with the engine's depth
  mask.
- **Camera travel.** Camera travel is not part of the Tier-0 vector.

Needed: the stick thresholds and InvertCameraLookBack handling; the
sputter scheduler; the dynamic motion-blur amount rule and its time
base.

## Q80 (audio, not blocking): what drives the ambience state

SPEC-AUDIO-EVENTS "World ambience" lists the parameters of
`AMB_Default.fev` and `Colorado_Festival.fev`. The engine now evaluates the
events' own curves (AUDIO-11), and feeds `CurrentTime` from the time of
day, `CarSpeed`, `Speed` and `Engine_RPM` from the focus car,
`RadioIsPlaying` from the radio, and `InRace` from the flag that selects
the `Race` snapshot. Missing:

- **`PreRaceAmbience` (0..1).** It is on `Default_Crickets_Bed` (silent
  from 0.5), `Default_Crowd_Grandstand` (ducked to 0.063 from 0.5) and
  `Default_Street_Race_Music`. Which race phase sets it, and whether 1
  means "in the pre-race" or "out of it", is not decoded. The engine keeps
  it at 0 (`AudioHost::set_pre_race_ambience` has no caller).
- **`CrowdExcitement`** (`Colorado_Festival.fev`): its source is not
  decoded either. The engine keeps it at 0 (`set_crowd_excitement`).
- **`CameraType` (0..1)**: which camera is which value. The engine keeps
  it at 0.
- **`Triggerable_Events.fev`** (28 scripted flybys driven by `Speed`,
  `PhaseRate`, `ClimbAngle`, `Bank`, `Pitch`): their dispatch and paths
  are not decoded, so no flyby plays.

Needed, narrow: the writers of these parameters in the ambience code the
findings name (`COpenWorldAmbience`, `CAmbientAudio`).

## Q81 (audio, not blocking): ambience playback the disc does not settle

- **Spawned sounds.** A sound definition's property template starts with
  u32 play mode, u32 spawn time min and max (ms), u32 maximum spawned and
  f32 volume. That layout is read from the values (Tree_Rustle
  1,000..10,000 ms, Air_Horns 500..30,000, Shouts_Lots 0..2,000 with 3
  spawned; the continuous beds River, Crickets and Race_Crowd_Amb read
  0..0), not from a spec. The engine loops the 0..0 beds and does not play
  the spawned one-shots (horns, whistles, door slams, dogs, shouts).
- **Placement.** Every placed Track3D event but the 110,000
  `Default_Reflections_*` placements is now an emitter (10,080). The engine
  pools the 8 nearest whose event has a loop it plays (4,795 placements),
  each within its event's own `(distance)` range. FMOD's voice limit per
  event (`max_playbacks`) and its real voice stealing are not applied, and
  race-only events (grandstands, race crowds) take pool places in free
  roam, where their `InRace` window silences them.
- **Quad beds.** The region beds are 4-channel (`_F` and `_R` stems, the
  rear on `Speaker Level` 2). The engine plays the front stem only.
- **Regional banks.** Placements name `AMB_Main_Town`, `AMB_Mountains`
  and `AMB_Foothills` events that are not on the base disc; they stay
  silent.
- **Trees.** The ambience templates name a `TreeEventName`
  (`Default_Tree`, `Default_Tree_Desert`: birds by day, crickets by night)
  and the tiles hold 76,636 `<Tree>` positions, but no tree is an emitter
  yet.

## Q82 (audio, not blocking): TrashDSP units and the overdrive scale

The focus car's `_ET.xml` `TrashDSP` bands read `inputgain`/`outputgain`
0, 1, 2, -2, 3, -3 and `mix` 80 (1,739 of 2,055 bands), so the engine now
reads the gains as dB and mix as a percentage, and scales the band sum by
the TrashDSP's own `VolumeCompensate` (0.18 to 0.25 on most tunings). That
is an inference from the value distributions. Before, a 0 dB band
multiplied by zero: VW_Beetle_04's focus exhaust rendered silent; it now
renders at 48.1 (the same loops as another car, without the focus DSP:
56.9). `overdrive` reads 2.8 to 7.5 on the disc but the shaper takes 0..1,
so it saturates at 1. `UseCurves="1"` (208 tunings) per-band curves are not
read. Needed: the shaping curve and the overdrive scale.

## Q83 (audio, not blocking): UI cues and other-car audio left open

- `kAchievementReward` and `kSkillBreak` map to `Hud_GeneralNotificationAppear`
  and `Lose_Points`, which are empty events on the disc, so both are
  silent. Is another event meant?
- The 3-2-1 countdown plays `321MomentStart`, `321Count` per digit and
  `321Go`; digit 3 gets no `321Count` (the MomentStart covers it). Not
  confirmed.
- Stereo UI samples are folded to mono; LFE stems fold into stereo at full
  level.
- Other cars at the Default view are point sources (the view's speaker
  pans are 0 on the disc); traffic has no throttle signal and plays at
  0.5. The pitch-envelope scale of FEV events is not decoded.

## Q84 (audio, not blocking): placed ambience events the disc does not have

Of the 10,080 placed Track3D events (reflections excluded), 1,295 name an
event that no base-disc ambience FEV has. Most are regional
(`MainTown_Car_Door_Slam` 251, `MainTown_Car_Horn_Distant` 246,
`MainTown_Dogs` 211, `Mountains_*`, `Plains_Rusty_*`; Q81). Of the 173
`Festival_*` placements, 57 resolve. The tiles name `Festival_Air_Horn`
(32), `Festival_Whistle` (44), `Festival_Swell` and `Festival_Workshop`,
but `Colorado_Festival.fev` names its events `Air_Horn`, `Whistle`, `Swell`
and `Workshop`. `Festival_Music_Speakers_C`/`_D`,
`Festival_Paintshop_Spraypaint` and `Festival_Quad_MemRes` match nothing.
The engine plays none of them (no stand-in). Before AUDIO-11 a global crowd
bed played near any festival placement; with the disc's events loaded it
does not. The festival is heard from its stage music, stage crowd,
grandstand, tanoy and speakers.

Needed: how the game resolves a placement's `eventName` to an event (a
project or group prefix it strips, or a remap table), and where the
regional banks are.

## Q100 (FEEL-19/20, not blocking): the car's collision spheres

The original collides the car body with the world through collision
spheres, not a box. MAXData.xml `Collision/CollSpheres` lists them per car
(the R34 has 32: twenty Flags 2 spheres of radius 0.16 to 0.34 m around the
body, twelve Flags 1 spheres of radius 0.05 m at the corners and roof).

**Engine reading:**

- `cars::load_car_collision_spheres` reads the list from the car archive.
  `apply_physics` moves it into our car frame: y from the box's `MinY` (the
  ground under the stock ride height), z from `BottomCenterWheelbasePos`.
  Bakes carry no MAXData, so the disc supplies the spheres; a car without
  any uses a stand-in set on the body box (ours: eight corners and six face
  middles, radius 0.15 m).
- Each step the spheres are raised so their bottoms stay
  `CarCollSphereMinDistAboveGround` (0.11 m) above the lowest wheel's
  ground, or `CarGroundCollSphereMinDistAboveGround` (0.4 m) for spheres
  with Flags bit 0. This follows the original's per-step lift, which reads
  each wheel's position. On flat road no sphere touches the ground.

Still interim:

- **Scales.** `PosScale` (0.995) and `RadiusScale` (1.35) are not applied.
  The car's `physicsdefinition.bin` holds the runtime sphere list
  unscaled, with hand moves for some cars (the R34's side spheres sit at
  y 0.41, x 0.66, and its nose and tail spheres about 0.29 m further out).
  That file's header is not decoded, so MAXData's list is used.
- **The lift's second term.** The original takes the lower of the wheels'
  ground and a body field; only the wheels' term is read.

Needed: the physicsdefinition.bin sphere block's header, and the body
field in the lift.

## Q101 (FEEL-19/20/21, PT-27, not blocking): the body-to-world contact

PT-27's fence launches came from the engine's 26-point penalty box: stiff
springs that turned any slanted surface into an upward force, then clamped.
The original solves contacts as impulses.

**Engine reading (decoded):**

- **Contacts.** Up to 16 sphere contacts, the deepest. MeshGround tests the
  triangles near the car once per step for all spheres. A face is pushed
  from the side the body's centre is on, so a sphere that passes through a
  one-sided fence is still pushed back. Other grounds use two rays per
  sphere.
- **Push-out.** The body is moved out along the contacts' normals, each
  contact adding only the depth the push so far leaves. Only the position
  changes; the velocity is untouched. Wheel compression moves with the
  body, so the dampers see no rate.
- **Impulses.** These are computed from the velocities after the step's
  forces. For each contact closing on its surface:
  - The normal impulse is `j = -(1 + e) vn / (1/m + n . ((I^-1 (r x n)) x r))`,
    where `e` is the pair's `Elasticity` as written. Car/HardWorld's −0.9
    removes a tenth of the closing speed per substep, and the push-out
    keeps the car outside.
  - The friction impulse is `mu * min(j, m * 7 mph)`. It acts against the
    slide direction, and below 5 m/s it is scaled by 0.2 per m/s of sliding
    speed.
  - `mu` blends `LowSpeedFrictionCoefficient` into `FrictionCoefficient`
    over `Collision\FrictionLowSpeedMph..FrictionHighSpeedMph` (10..40 mph)
    of the contact point's speed.
- **Combining.** The impulses and their moments are averaged over the
  closing contacts. The moment is scaled per body axis by
  `CarObjWorldCollTorqueScalePitch/Yaw/Roll` (0.55, 0.65, 0.55), then both
  are applied.
- **Pair rows.** Surfaces take surfaceTypes.xml's CategoryToCategoryParams
  row for Car and their Category, or Default/Default when there is none.
- **GimbalLock.** Past `GimbalCosAngle0` (0.87) of tilt, a spring acts on
  the spin about the horizontal tilt axis: rate `GimbalSpringK` (50),
  damped by `DCompress` (5) when the tilt grows and `DRebound` (0) when it
  shrinks. Past `GimbalCosAngle1` (0.82), spin that tilts further is first
  reflected with `GimbalAngImpulseElasticity` (0.2). This replaces the
  10 rad/s spin cap.
- **Q67.** `last_body_impact` keeps its meaning. A contact that begins
  after a step or more with none is an impact, and its speed is the
  largest closing speed then. `strongest_body_contact().force` is the
  substep's impulse over its length.

**Removed (PT-27):**

- the flattened wall normals;
- the wall damping floor and rebound factor;
- the force caps per point (180 and 120 × mass share) and on the summed
  upward force (3 m g);
- the horizontal-only wall friction;
- the wall torque factors (0.20, 0.35, 0.20);
- the vertical-velocity clamps after any contact;
- the 10 rad/s spin cap;
- the wheels' rejection of normals under 0.65.

With Car/HardWorld:

| Test | Result |
|---|---|
| Fence at 25 km/h | 0.03 m/s up, nose 2 cm in, one impact |
| Wall head-on at 150 km/h | stopped in 0.1 m, 1.1 m/s up, 0.4 m/s rebound |
| Wall glance at 20 deg, 108 km/h | keeps 24 m/s along the wall |
| PT-27 bank | 1.0 m, 1.3 m/s up |
| PT-27 0.35 m kerb at 90 km/h | 0.7 m, 2.7 m/s up |

**The wheel query and the bump stop (decoded, 2026-11-25).** This
supersedes the "Suspension clamps" item below and the "wheel query's
treatment of steep faces and steps" in Needed.

- **The frame's query.** Once per frame, before the substeps, each
  wheel sweeps a sphere of the tyre's radius along its strut. The sweep
  runs from the top of its travel down to where the wheel is. The top is
  the bump stop plus the tyre's own maximum compression. The wheel takes
  the highest triangle near it that passes three tests:
  - its normal . the car's up axis is at least 0.17;
  - the sphere at the top of the sweep is at most 0.3 m into its plane;
  - the sphere at the bottom is within about 0.1 m of it.
- **A failed test.** A face that fails the tests does not carry the
  wheel, so a fence or a step more than 0.3 m above the wheel's reach is
  the body spheres' to meet. Each substep re-tests the wheel against the
  same triangle with the same limits.
- **The tyre first.** A face above the wheel first compresses the tyre,
  which has its own deflection and spring (below). Past the tyre's
  maximum compression, the rest moves the suspension. The suspension
  stops at its travel, and what is left over is the bump stop's
  overflow. The overflow is recomputed from zero by every query, and it
  adds what the sphere at the top of the sweep is into the face.
- **The suspension force.** The spring (on the position, which stops at
  the travel), the anti-roll bar and the bump stop's spring,
  BumpstopStiffness x overflow, are summed. They are
  added to the damper, which is clamped to DampenBumpClamp and
  DampenReboundClamp. BumpstopDamping x the contact point's closing speed
  along the ground's normal is added to the damper before that clamp.
  The result is floored at 0. Nothing else caps the force.
- **TireSpringiness.** PhysicsSettings.ini has no TireSpringiness keys,
  so the executable's built-in values apply:
  - SideWallHeight: 0.05 to 0.15 m in, 0.005 to 0.015 out.
  - RestLoadKg: in up to 500, out 0.57 to 1.43.
  - TirePressure: 15 to 41 psi in.
  - CompoundStiffness: 0 to 1 in, out from 1.2.
  - TireWidth: 0.15 in, out 1.1 to 0.9.
  - RestCompression: 0.005 to 0.025 m.
  - DBump and DRebound: 0.
  - MaxGees: 4.
  - MaxCompression: 0.06 m.
  - MinMaxCompression: 0.015 m.
  - MaxRestCompressRatio: 0.333.

  The rule from these values:
  - **Rest compression.** It is the product of the tables, clamped to
    RestCompressionMin..Max and to MaxRestCompressRatio x the sidewall.
  - **Tyre rate.** The rest load over the rest compression.
  - **Maximum compression.** MaxGees x the rest compression, at most
    MaxCompression, and at most MaxCompressRatio x the sidewall (at
    least MinMaxCompression).

The engine now runs this query on its ray, with these interim parts:

- **No tyre spring.** The engine has no tyre deflection and no unsprung
  mass, so the sweep's top takes the tyre's largest allowed compression
  (0.06 m), and the overflow is the compression past the bump stop.
- **One ray, not a sphere sweep.** The ray looks past a face that fails a
  test, and the penetration is that of the sphere at the top of the
  sweep along the hit's normal.
- **Every substep.** The query runs every substep, not once per frame.
- **The damper's rate.** The engine differentiates the unclamped
  compression, so a step drives the ordinary damper to its clamp. Then
  BumpstopDamping, which joins it before the clamp, changes nothing: the
  step tests read the same with BumpstopDamping at 0. The original's
  damper reads the suspension's own velocity, whose handling where the
  position clamps is not decoded.

The 0.14 m cap and the load cap are gone. With them gone:
- The 0.35 m vertical step at 90 km/h is within the wheel's reach. It
  lifts the test car 1.88 m at 5.5 m/s. (2.24 m at 6.1 m/s while the
  main spring still acted past the travel.)
- A 0.7 m step is out of reach. The wheels stay on the road and the body
  meets it (0.07 m/s up).
- The 150 km/h wall lifts the nose at 1.58 m/s (1.1 with the caps).
- The oracle lap reads 38.980 s (38.976 s before the spring stopped at
  the travel; oracle 38.784 s).
- The AI gate (AI-driven player) leans slower on NR02 and FR22 once the
  spring stops at the travel: 6-seed means 158.7 -> 164.6 s and
  164.8 -> 166.7 s, within the seed spread.

Still interim:

- ~~**Suspension clamps.**~~ Superseded by the decode above. The bump
  stop's 0.14 m over-compression cap and the wheel load cap `max(4 x
  preload, 1.1 m g)` are gone. The old text: "A raycast wheel meeting a
  step higher than its axle jumps onto it in one substep. Without the
  caps, the 0.35 m kerb at 90 km/h throws the car 2.3 m up at 6.2 m/s.
  Applied to every wheel, the load cap also clipped ordinary cornering
  loads (the oracle lap's bump-stop hits reach 1.4 cm) and spun the AI
  under FrictionTorqueMod (Q68). The original's wheel query on such
  steps is not decoded."
- **Rate.** The contact runs every substep (359 Hz). That the original's
  contact chain runs at the same rate is assumed.
- **Friction noise.** The pair's friction noise (`FrictionNoiseFreqScale`,
  input to output range) is loaded but not applied, because its noise
  function is not decoded.
- **Low-speed blend.** The blend's car-side gate (a body flag) is taken
  as always on.
- **Smashables.** Props below their break speed now add body-sphere
  contacts to this solver with the Smashable/Car row (and solid barriers
  with Car/GuardRail; `SmashableWorld::add_sphere_contacts`); the old OBB
  push is no longer called. Their shapes are the props' own spheres,
  boxes and hulls, or the bounding box when a def has none.

Needed: ~~the wheel query's treatment of steep faces and steps~~
(decoded above), the friction noise function, and the contact chain's
tick. Also needed: the tyre spring and unsprung-mass integration, the
remaining TireSpring table outputs (TirePressureOutput, the upper
CompoundStiffness and TireWidth points, MaxCompressRatio), and the
reader of the query's ground-normal velocity term.

## Q102 (FEEL-05/07/23, not blocking): damage difficulties and List_DamageModDef

The original's damage difficulties are Cosmetic, Limited and Simulation
(EDamageDifficulty; GameOptionValues 19 Cosmetic and 20 Limited, in that
order). gamedb.slt `List_DamageModDef` has one row per part, for GearBox,
Driveline, Steering, BodyDrag, Front/RearDownforce, Brakes, Tires,
Suspension, Engine and Motor. The Lobby's damage option also sets fuel
depletion and tyre wear. The rows' consumer is not decoded.

**Engine reading (ours, on the rows' columns):**

- **Modes.** `DamageMode` gains Limited (stored as 3). The stored 0
  (None, ours), 1 (Cosmetic) and 2 (Simulation) keep their meaning for
  saves, and only Simulation earns the +10% reward. The settings row
  offers limited.
- **Parts.** Under Limited and Simulation, each body impact damages each
  part as follows:
  - The impact in mph is first varied by ±`RandomImpact`.
  - At or past `DamageMinImpact`, the damage added runs from `DamageMin`
    to `DamageMax` across `DamageMinImpact..DamageMaxImpact`.
  - The damage applies with a chance that runs from `ProbabilityMin` to
    `ProbabilityMax` across `ProbabilityMinImpact..MaxImpact`.
  - Limited adds half.
  - The part's effect runs from `ModZeroDamage` to `ModFullDamage` across
    its damage:
    - Engine and Motor: torque;
    - GearBox: shift time;
    - BodyDrag: drag;
    - Front and Rear Downforce: the downforce at each end;
    - Brakes: brake torque;
    - Suspension: rates;
    - Steering: degrees of pull, towards the side of the hit.
- **Rows.** The rows come from the disc (`load_disc_damage_mod_defs`),
  with the shipped values as the fallback.
- **Impacts.** carview records one impact per body impact at its closing
  speed (`last_body_impact`), no longer once per frame of contact force.
- **Sputter and audio.** `engine_damage` is the Engine/Motor part's
  damage, so the sputter rumble (`EngineDamageLevelStartSputter` 0.3) and
  the audio's damage sounds follow these rows.
- **Wear and fuel.** Tyre wear and fuel burn only in Simulation.

Still interim:

- the impact unit (mph of closing speed);
- the random roll;
- Limited's half share;
- the application to every part regardless of where the car is hit;
- the Driveline part, which has no effect;
- Steering's pull side;
- the wear and fuel rates;
- the cosmetic zone and lamp model;
- the settings page, which still lists the engine's none and presets
  that use it (Q71).

Needed: the List_DamageModDef consumer (impact unit, location and roll),
Limited's rule, and the original's settings rows.

**Partly decoded (2026-11-25).** The rows' loader reads each part's row
when the database has one. For a missing row it fills a built-in row,
which in its read order is 1.0, 0.25, 40, 75, 0.1, 0.35, 0.2, 0, 75,
0.2 and 1.0. Read as the table's columns, that is a part with no effect
at zero damage and a quarter at full damage, a 40 to 75 impact range
and a 0.1 to 0.35 damage range; the mapping of the last five values to
columns is not confirmed. The engine's fallback is the shipped rows, so
this only matters for a database without them. The consumer, which
turns an impact into part damage, is still not found.

## Q103 (FEEL-14, not blocking): Assisted braking's gain

SPEC-CAR-PHYSICS confirms the pipeline: with `AutoBrakeOn` (Braking 0)
the controller reads the racing line's lookahead target speed and adds
brake (and cuts throttle) while the car is faster than it. The engine
now takes that target from the driving line everywhere
(`drive::auto_brake_target`): in races on the racing line, and in free
roam on the GPS route or the road ahead, built from the car's own
10-point braking table. The fixed 0.85 g corner rule and 7.5 m/s^2
braking that free roam used are gone.

Still interim (the spec marks the gain "probable"):

- the margin: nothing below 0.5 m/s over the target;
- the gain: brake (excess - 0.5) / 8, throttle cut over (excess - 0.5)
  / 3, both clamped to 0..1;
- the free-roam path: the GPS route or the nearest road's centre line
  ahead, which the original may not use.

Needed: the auto-brake's threshold and gain constants, from a trace
through a few corners with Braking set to Assisted.

## Q104 (FEEL-18, not blocking): racing-wheel dead zones

ForzaProfile.sch keeps a wheel's dead zones apart from a pad's:
WheelAdvancedOptions has steering 0.01/0.95, throttle 0.15/1.00, brake
0.15/0.95, clutch 0.15/0.90 and handbrake 0.10/1.00, against
ControllerAdvancedOptions' 0.24/0.95 steering. The engine gave a wheel
only its own WheelShaping (saturation, curve, skip), with the pad's
pedal window. A wheel, whether SDL sees it as a gamepad or as a raw
joystick, now takes WheelAdvancedOptions' window
(`ControlDeadzones::wheel_defaults`) before the shaping, whose defaults
change nothing.

Still interim:

- the rescale itself, linear from inside to outside per axis, as for the
  pad (the pre-curve is open, SPEC-CAR-PHYSICS);
- whether the original applies the pad's steering dead zone radially;
- the wheel window is the profile default; the settings page has no
  rows for it;
- WheelShaping, which the original does not have, stays as a user
  option, and the Separate Devices path keeps its own per-axis dead
  zones.

Needed: the input manager's dead-zone rescale and its wheel rows.

## Q105 (FEEL-08, not blocking): thresholds beside the tyre law

The tyre law follows SPEC-CAR-PHYSICS, but four thresholds next to it
are the engine's. None of them is in the spec or the car data:

- **VelDepFriction's fade-in.** The surface drag is scaled by
  min(1, |v| / 0.2 m/s) per wheel axis, and runs only when the car has
  aero drag (`aero_drag_long > 0`). The spec confirms the slew-clamped
  `-sign(v) * coeff * N`, not the fade or the gate.
- **Bearing drag in the air.** A wheel off the ground loses spin as
  `omega *= 1 - 0.5 dt`.
- **RWD launch assist.** Below 11 m/s on a RWD car's rear wheels, slip
  past 1.7 is cut to 0.35 of its excess, fading out from 5 to 11 m/s.
  The spec gives only "damps slip > 1.7 below ~5..11 m/s". The player
  only; the AI skips it.
- **Gravity per off-ground tyre.** Gravity is scaled by
  PerTyreOffGravityScale to the power of the number of wheels off the
  ground (all four: AirborneGravityScale). The settings name the
  constant, not whether it multiplies per tyre or adds.

Needed: the launch assist's gain, and how the per-tyre gravity scale
combines. Both are visible in a standing-start trace of a RWD car and a
trace of a car with one or two wheels off a kerb.

## Q106 (FEEL-21, not blocking): props the car does not break

A prop breaks when the car's speed is above its BreakSpeedMph
(FORMAT-PHYSICS-DEFINITIONS); below that, and for an unbreakable prop,
the engine used to push the car out horizontally with its own impulse
(1.08 restitution, 0.22 friction, a 0.65 m push cap), so a fence or
post hit had no pitch and no rebound of the original's kind. The props
now join the car's body solve (Q101) as fixed bodies:
`world::SmashableGround` adds their PhysicsDefinitions shapes (spheres
and OBBs) to the ground's sphere contacts, with the
CategoryToCategoryParams Smashable/Car row (mu 0.5, Elasticity 0.3).
The solid barriers go the same way with the Car/GuardRail row.

Still interim:

- **Solid barriers by name.** Which static models are solid is still
  chosen from their names and proportions; the original's rule for
  static objects' collision is not decoded.
- **Convex hulls** meet the car as their box in the prop's frame.
- **Fixed bodies.** An unbroken prop does not move; its Mass is unused
  until it breaks.
- **The break test** reads the car's speed at the start of its step,
  not the contact's closing speed.
- **Only the player's car** meets the props; AI and traffic cars still
  pass through them, as before.
- **Barn doors** keep the old horizontal push.
- **The smash-through slow-down** on a break is the engine's.
- **Low hard props.** The wheels' rays do not see props, so a box lower
  than the body spheres' centres is met by the spheres' edges and the
  Elasticity 0.3 row. A 0.3 m unbreakable box hit at 25 m/s lifts the
  car 0.30 m more than its own settling, at 1.09 m/s up (0.89 m and
  3.8 m/s on another approach; the HardWorld row gives 0.16 m and
  0.40 m/s and lets the car slide over). The shipped
  PhysicsDefinitions have no such prop: the five that are unbreakable or
  break above 27 mph all reach 1.06 m or more.

Needed: the static-object collision rule, and whether an unbroken prop
is fixed or a heavy dynamic body.

## Q93 (UI, not blocking): the event ticket's surface line

`H_EVENT_SUMMARY`'s `TEXT_TRACK_INFO` reads "<surface> <layout>" (the
`{0} {1}` string with ASPHALT and POINT-TO-POINT or CIRCUIT). The engine
picks the surface from `Tracks.SurfaceTypeRibbonConfiguration`: 0 is
asphalt (FR06, The Gauntlet, whose track has 0, shows ASPHALT
POINT-TO-POINT in `10_event_card_prerace_orig.png`). As an interim, 1 is
taken as the mixed surface and 2 as off road (`event_track_info()` in
`apps/carview/race_screens.cpp`). Needed, narrow: a capture of a dirt or
mixed event's summary, or the code that picks the surface string.

## Q94 (UI, not blocking): the event ticket's colour scheme

The ticket's value tapes (`Points/Tape`, `Credits/Tape`) take a colour
per event style at run time through the scene's ColorScheme states, which
are not decoded. As an interim the engine tints festival tickets with the
green and dark ink sampled on `10_event_card_prerace_orig.png` (rgb
4,221,125 and 46,46,46) and leaves the scene's own colours on street,
showcase, star showdown and headline tickets. Needed, narrow: captures of
the summary for one event of each other style, or the ColorScheme state
table.

## Q95 (UI, not blocking): the pre-race rival callout

Before a festival race that has a star rival in it, the original shows
the rival on `PRE_AND_POST_RACE`'s `NemesisGroup` after the title card,
alongside the `nemesis_show` cutscene: Beat <name> on the big black tape
and REWARD with the reward on a pink tape under it
(`14_rival_intro_orig.png`). The scene rests at its intro start (the big
tape off-screen left), so the engine places the lines where the capture
has them; the tapes land lower than their static rects say, and the
engine corrects them by offsets fitted to that one capture. The star
rival is now the field's first opponent flagged `AIPlayers.Nemesis`, and
the reward line is its BEAT THE STAR bonus (no line when the bonus is
zero). The timing follows `nemesis_show`: its `CCutsceneNemesisUITrigger`
shows the callout from 0.8 s for 10 s, and the flow then waits half a
second (`kRivalIntro`, 11.3 s), with the cutscene's cameras under it. As
interims the sequence is taken to end with the UI trigger's duration (the
cutscene itself loops from 4.8 s), A skips it, and
`CIsNemesisInEvent check_wristband="true"` (the star is skipped below the
player's wristband) is not decoded, so every event with a star shows it.
Needed: the callout's intro end state (so the offsets can go), a capture
timing the callout from START to the count, and `CIsNemesisInEvent`'s
wristband check.

## Q96 (UI, not blocking): the points and won-car screens

`H_POST_RACE_CAREER_POINTS` rests fully transparent (its intro is a state
timeline) and `H_POST_RACE_WON_CAR` rests at its intro start like the
finish banner. As interims the engine shows the points title, the points
earned and the wristband bar at the rects measured on the UI layouts
(POINTS EARNED at 103,160), and YOU WON A CAR! with the car's name on the
banner's pink tape at the banner's offsets. The wristband art, the
COLLECT panel and the won car's render are not reproduced. Needed:
captures of both screens at rest (a race that levels the wristband, and a
race that wins a car), or the intro timelines' end states.

## Q97 (UI, not blocking): car select and difficulty from the race menus

When the current car does not qualify, the original's summary leads to
car select (`108_C_BUY_CARSELECT`: a carousel of the garage's eligible
cars with a buy-car tile, and the focused car's card and stats;
`11_car_select_orig.png`). The engine now draws it on that scene's car
list, as My Garage does (`car_list_screen.h`): the eligible cars' tiles,
the focused car's panel (Y TOGGLE), A SELECT and B BACK. As interims the
buy-car tile and X CAR LIST are not shown (the engine has no buy flow from
here), and neither is BACK SEARCH. The grid menu's DIFFICULTY row opens
the difficulty screen (`300_I_OPT_ASSISTS_DIFFICULTY`, the options page);
its rows reach the race when it closes. Needed: a capture of car select's
buy-car tile and X CAR LIST in use, and one of the difficulty screen
opened from the grid menu (its help bar there).

## Q98 (UI, not blocking): the tombola, the rival offer and star rivals

The post-race order has two screens the engine does not draw yet: the car
tombola (`H_CAR_TOMBOLA`, after an event whose prize is a tombola) and
the rival offer (online only, so never reached offline). The flow model
has steps for both (`race_flow::post_steps`), but the session sets
neither yet. The rival offer needs a live session (`CanShowRivalOffer`),
so offline it is never shown, as the engine does. For the tombola no
career event's data marks a tombola prize in the tables read so far
(`Events`, `Rewards_*`), so the engine never reaches it. The event's star
rival now comes from the field (`AIPlayers.Nemesis`, Q95). Needed: a
capture of a tombola after a race, and the data that marks an event's
prize as a tombola.

## Q99 (UI, not blocking): the tapes' specular sheens

Some tapes carry a `specular` model under their crisp tape (the event
title's `MainTitle/Crisp/specular` at alpha 0.5, the rival callout's
`NemesisName/Crisp/specular` at 0.4). The engine draws them as a dark
gloss across the pink, and, being one texture, they are not cropped with
the tape, so they run on past its end as a dark band. The captures
(`12_race_intro_card_orig.png`, `14_rival_intro_orig.png`) show flat
tapes, so as an interim the engine hides both. Needed, narrow: the
sheens' blend mode (additive or alpha) and whether the original crops
them with their tape.

## Q114 (PROG-07, not blocking): rewards for speed traps and speed zones

PR stunts and outpost discovery pay nothing in the title: no credits, no
popularity. Completing a stunt only raises the outpost's fast-travel
discount. Both are decoded.

For speed traps and speed zones the evidence is weaker:
- the trap handler's own body makes no popularity call;
- `HorizonFeats.xml` has no speed-trap skill;
- the credits setter has no caller in trap code.

But the handler's listener loop and two of its callees were not read, and
the speed-zone (average speed) handlers were not opened. The engine now
pays nothing for either (it used to pay +500 popularity on a new best and
+150 otherwise).

Needed: whether a trap or zone result adds popularity or credits anywhere
(a listener on the trap event, or the zone's completion path).

## Q115 (PROG-08, not blocking): the star driver and the payout's early exits

BEAT THE STAR pays `CareerWristbandLevels.NemesisBonus` of the event's
`Level` when the event is a festival race (CareerEventStyle 1) and the
player places strictly ahead of the star. Both conditions are decoded.

What remains open:
- **Who the star is.** The payout takes the first participant whose
  driver object answers true to one virtual query, and that query's
  function was not found. The grid builder reads `AIPlayers.Nemesis` for
  each `EventParticipants` row and seats flagged drivers at
  `Events.NemesisGridPosition`, so the engine takes the first
  `AIPlayers.Nemesis` driver in the field as the star. This is inferred.
- **The star's place.** The engine reads the star's running place when the
  player crosses the line. The title probably reads the same field, but
  this is not confirmed.
- **Early exits.** The payout returns with nothing in two cases, and what
  each one means is not known:
  - a setup flag is set and a ratio of two player-car values reaches a
    threshold;
  - another setup flag is clear and a byte on the player object is set.

  Neither one is the DNF check. The engine has neither exit.

**2026-11-25, from the generated code.** The early exits belong to the
whole race payout, not only to BEAT THE STAR: they return before the
credits, the skill bonus and the star bonus are worked out. The star
bonus itself is a separate step. It takes the event id, the player's
place and the star's place, and pays only when the player is ahead and
the event's `CareerEventStyle` is 1.

Both flags are copied into the race setup from a session description,
next to each other. Which class that description is was not confirmed.
The search for their writers matched on the byte offset only, so it can
hit unrelated classes. Among the hits, the online lobby writes the whole
run of flags and sets the second flag to 1, and a constructor-like
function writes 0 to both.
- So the first exit, which needs the second flag set, is probably online
  only.
- The second exit needs the first flag clear, which looks like the
  default. It may therefore apply to career races too, and that is
  undetermined.

The two exits in detail:
- **First exit.** The second flag is set, and a per-race counter on the
  player, divided by a value from the player's race timer (when that value
  is above 1.0), reaches an integer tuning threshold. The tuning record
  is the same one the second exit reads.
- **Second exit.** The first flag is clear, and the player was flagged
  during the race. The flag is set while racing when the spread of one
  tracked value stays below a float tuning threshold: the race keeps that
  value's minimum and maximum, and the flag means max − min < threshold.
  The value comes from one of the car's per-frame queries. It reads like
  an "idle or barely moved" guard, but this is inferred.

The star query was not settled. The step walks the participants, takes the
first whose driver answers true to one virtual query, and reads that
driver's place; that function is still unidentified.

The engine keeps its reading (Q115's first two bullets) and still has
neither exit. That stays open: the second exit may apply offline. C2 is
not settled.

Needed:
- the session description's class and the second exit's flag;
- the virtual query;
- the counter's writer and the timer value (exit 1);
- the tracked value (exit 2).

## Q116 (PROG-12, not blocking): barn-find gates the engine does not model

The barn controller is decoded: spawn ticks, the weighted pick and its 1-in-N
roll, pending to rumour, discovery, restoration over the `DistanceDriven`
distance, ready, and collection at the garage. The tuning comes from
GlobalRegistry `Activities/BarnFinds`. Five points are still open:

- **Two predicates that skip the update.** One skips the controller's whole
  update, including its tick timer. The other is a global flag checked
  beside it. Either could be a story gate, such as the opening. The engine
  has neither; it only pauses during onboarding.
- **The idle timer.** A tick is skipped when a player timer reaches
  `MaxIdleTime` (600 s). The timer is probably idle time. The engine counts
  the time spent below 0.5 m/s.
- **The ready check's player value.** Ready needs a player value of at least
  11.176, probably speed (25 mph). The engine reads it as speed.
- **Dialogue.** The pending timer pauses while dialogue plays. The engine does
  not track dialogue, so its timer never pauses for it.
- **Collection.** The title collects a ready car on its garage Barn Finds
  screen, with a COLLECT CAR confirmation. So does the engine now (Q133:
  the Barn Finds list on `108_C_BUY_CARSELECT` and its popups); it no
  longer collects when the garage opens. UI.zip has no Barn Finds scene: its only barn scene is
  `H_BARN_FIND_HUD`, the driving HUD's. The screen is therefore probably a
  garage car list with Barnfind's popups:
  - `IDS_Collect_Popup_Title` / `_Message`: "COLLECT CAR", "This Barn Find
    will now be added to your garage."
  - `IDS_Wrecked_Popup_*`: "NOT READY", "Please come back later. This Barn
    Find is still being restored".
  - `IDS_Locked_Popup_*`: "NOT DISCOVERED", "This Barn Find has not been
    discovered yet!".
  - `IDS_Trade_BarnFind_*`: "CAN'T SELL BARN FIND".
  - `IDS_ViewMapAction`: "VIEW ON MAP".

  The engine's port of that list is Q133's.

**2026-11-25, from the generated code:**
- **The idle timer is decoded.** It is a player timer. Each frame it adds
  the frame time while the magnitude of one player value is below a
  threshold, and resets to 0 otherwise. It does not run while the player
  is in a skip state (a player flag, or a player query that is true).
  - The threshold is a field of a tuning record. The race payout's second
    early exit (Q115) compares a value's spread with the same field.
  - The value is probably speed, as the engine reads it, so the engine's
    rule (time below a speed threshold) has the right shape. The
    threshold's value was not read; the engine's 0.5 m/s is still a guess.
  - The tick is skipped once the timer reaches `MaxIdleTime`, as stated
    above.
- **The predicate that skips the whole update** is a game query. It follows
  the game's session object to a state object. It skips when that object
  is missing or a byte flag on it is clear. The flag's meaning (free roam
  active, or not in a race or cutscene) is not identified, so the engine's
  "pause during onboarding" stays an interim.
- **The pending step checks two more predicates.** One is a global
  manager's query, probably the dialogue-playing check above. Not
  identified further.

Needed:
- the session state's byte flag;
- the tuning record's threshold values;
- the value behind the ready check.

## Q117 (PROG-03, not blocking): campaign captions with no shipped text

The wristband, Star Showdown and Showcase tables now come from the disc:
gamedb.slt for the numbers, airborne_challenges.xml for the opponents and
the string tables for the names. The title voices these moments, and the
string tables hold no text for the lines the engine shows around them, so
those lines are blank or interim:

- **Blank:** the wristband announcement, the Star Showdown challenge and
  victory lines, and the Showcase intro and victory lines. Their dialogue
  cues still play the VO.
- **From the string tables** (the English text is the fallback without a
  resolver):

  | Where | String |
  |---|---|
  | Wristband ceremony eyebrow | PostRaceFlow `IDS_WristbandUnlocked` "WRISTBAND UNLOCKED!" |
  | Showcase intro eyebrow and detail | InGame `IDS_HudMsg_ShowcaseEvent` "SHOWCASE EVENT", CareerEventUI `IDS_ShowcaseInstructions` "WIN THE RACE TO KEEP THE CAR" |
  | Showcase unlock subtitle | InGame `IDS_ShowcaseEventUnlocked_Message` "SHOWCASE EVENT UNLOCKED!" |
  | Star Showdown intro eyebrow and detail | CareerEventUI `IDS_TitleNemesisRace` "STAR SHOWDOWN", `IDS_NemesisCalloutTitle` "Beat {0}" |
  | Showcase and Star Showdown victory eyebrows | CareerEventUI `IDS_Headline_Event_Won` "WINNER" |
  | Collect-wristband objective | InGame `IDS_CollectWristband` "COLLECT {0} WRISTBAND", `IDS_WristbandNewAvailable` "NEW WRISTBAND AT RACE CENTRAL" |
  | Finale objective | InGame `IDS_HudMsg_HorizonFinal` "HORIZON FINAL", PostRaceFlow `IDS_HeadlineEventAvailableLn2` "WIN TO BE CHAMPION!" |
  | Star Showdown objective detail | "Beat {0}" |
  | Barn rumour subtitle | Barnfind `IDS_Spawned_Body` "NEW BARN FIND RUMOR" |
  | Barn found detail and subtitle | Activities `IDS_BarnfindBeingRestored_Message` "{0} IS BEING RESTORED", `IDS_BarnfindBeingRestored_Title` "DAK WILL CALL YOU WHEN IT'S READY" |
  | Barn ready headline, detail and subtitle | Barnfind `IDS_Ready_Message` "BARN FIND READY - {0}", `IDS_Title` "Head to the garage to collect it!" |

- **Interim, with no string to use:** the wristband ceremony's subheadline
  and badge ("7 NEW FESTIVAL EVENTS • STAR SHOWDOWN UNLOCKED", "WRISTBAND
  TIER 2 / 7"), the Showcase and Star Showdown badges and victory
  subheadlines ("POPULARITY RANK #250 SHOWCASE", "PINK SLIP CLAIMED"), the
  Star Showdown unlock subtitle ("Star Showdown unlocked! ..."), the
  "COMPLETE" and "DEFEATED" headline suffixes, and the free-roam objective
  texts. Each is engine text.
- **Casing:** names go in as the string tables hold them ("Hailey Harper",
  "Manic MINI"). Only captions built from an all-capitals format are in
  capitals ("{0} WRISTBAND" gives "PURPLE WRISTBAND").

Needed: which of these screens the title draws, which strings each one
uses (the ones above are matched by their wording, not traced to their
callers), and whether the UI changes the case.

## Q107 (WORLD-11, not blocking): how an ordinary event sets and holds the time of day

The original changes the clock by a request: a target (plus 86,400 to pass
midnight) and a rate, which the clock runs at until it lands and pauses
there; a held-clock flag keeps the title's own setters off it. The engine
has it (`TodClock::request`). The only callers traced are two special
flows: the first-time-career intro (a 45 s ramp to 18:00, 14:30 or
midnight, picked by state we have not matched) and the E3 demo. What an
ordinary event start writes is not traced.

Seen in the original: from a 22:00 free roam The Gauntlet loads in
daylight, so the start time is set at once. Quitting that event (pause,
QUIT RACE, confirm) returns to free roam still in daylight: the pre-race
time is not put back. The engine now does the same: it sets the event's
`TimeOfDayStart` at once, holds the clock through the race (SKY_AND_TOD
§7: paused on fixed-time events) and lets free roam carry on from there.
The restore it used to do was the backwards jump the playtest saw.

Still open: whether finishing an event (rather than quitting) leaves the
clock in the same place, and whether free roam resumes at the event's
start time or at the time it had reached. Needed: a capture either side
of a finished event, or the writer of the clock's time and hold flag on
event unload.

## Q108 (WORLD-12, not blocking): where world lamp glows come from

The original's lamp glows are camera-facing sprites drawn after the opaque
world (a sprite texture times a 1D colour ramp, additive, depth-tested,
a per-lamp `SwitchOnLights` threshold and a `LookFadeValues` cone). Not
decoded: where each glow's position, direction, colour, radius, threshold
and textures come from (a record in the track or prop data, or the prop's
model). The engine makes its own: a glow on props whose names read as
lamps or floodlights, colour by kind, a hashed threshold, re-seated into
the prop's box and dropped when no box matches, drawn as a procedural
corona. Without the source, changing those constants trades one guess for
another, so they stay as they are.

Needed: the glow record format and its sprite and ramp textures, or a
night frame's draw log of the lamp-glow draws with their vertex data.

## Q109 (WORLD-14, not blocking): spectator clips and placements

The original steps each spectator one frame per rendered frame through a
`Spectators.zip` clip and, at its end, picks another clip or loops. The
engine parses each `.anim.bin` header and skeleton but not its keys, and
poses spectators with its own procedural moves. Hard-coded clusters and
the head turn toward the car are gone (WORLD-14); the remaining clusters
are placed from `GameObjs.xml` gameplay nodes by the engine's own offsets,
not from a crowd record (`Colorado_track_00.crowd` is 12 bytes).

Needed: the `.anim.bin` key layout (rotation and translation tracks, rate)
and the source of the start-line spectator instances.

## Q110 (WORLD-POST, not blocking): overall exposure and grade against the original

Matched free-roam captures (`world_{day,dusk,night}_orig.png` against
`world_*_engine_after.png`) put the engine darker overall: mean luma 96.6
against 111.6 by day, 53.0 against 58.8 at dusk, 29.4 against 34.5 at
night, with the sky brighter and bluer and the ground darker. The
originals were taken with the recomp's bloom, depth of field and MSAA
off, and the car and pose differ, so part of the gap is the frame, not
the grade. No tone-map, LUT or exposure change was made to close it by
eye.

Needed: the original's post chain constants for one time of day
(exposure, tone curve, colour cube in use), or matched captures with the
same post settings on both sides.

## Q111 (WORLD-15, not blocking): where the animated showcase props stand in free roam

The `animatedobjects.zip` `.pgeo` meshes (balloon race, showcase-intro
balloons, air walkers, main stage lights, plane banner, blimp, Ferris
wheel) are placed by `AnimatedWorldSystem::populate_pgeo_placements` at
anchors the engine picked (box-derived spreads, the festival hub, a
reservoir vista) with its own colour palettes. The fixed-position props
are gone (WORLD-15), but these placements are not from a placement
record either. Whether the original shows any of them outside the
showcase events that own them is not known.

Needed: the placement source for these `.pgeo` objects (a track object
list or the showcase event scripts), and a free-roam capture of the
festival and a showcase start in the original.

## Q87 (UI, not blocking): the pause map's view

The original draws the pause map from `MapProfileFullscreen.xml` and the
captures (`map_open_orig.png`, `map_popup_*_orig.png`) look top-down, but
whether its camera tilts or pans in perspective while zooming is not
known. The engine draws a flat top-down raster (`MapView::pause_map`)
under `H_MAP`. Needed, narrow: a capture sequence of a full zoom in and
out, or the map camera's setup in the code.

## Q88 (UI, not blocking): the map filter's combination and Rivals

`H_FIXED_SETTING_LIST` titled `IDS_FilterMap_Title` lists twelve rows,
all ticked in `map_filter_orig.png`. The engine shows an icon when any
ticked row covers it (New, Not Won and the category rows OR'd), ticks or
clears every row with All (cleared in `map_filter_alloff_orig.png`), and
keeps All ticked exactly when every row is (`map_filter.h`). Unknown:
whether New and Not Won narrow the category rows instead (AND), and what
Rivals shows (the engine has no rivals, so the row covers nothing).
Needed, narrow:
captures of the map with only New, only Not Won and only Rivals ticked.

## Q89 (UI, not blocking): the minimap camera

In `04_roam_hud_day_idle_orig.png` the minimap's arrow sits about 100 px
down the 180 px panel (y 520 at 720 lines) with roads well ahead of it.
The engine uses a fixed camera 108 m up and 54 m behind the car, which
puts the arrow there and shows about 160 m ahead and 45 m behind. In
`05_roam_hud_day_drive_orig.png` (moving slowly, same place) the icons sit
about 15% further apart and the arrow a few pixels lower than at rest, so
the original's view changes a little with the car, the other way from
pulling out. Unknown: the original's real camera height and pitch, and how
it follows speed. Needed, narrow: captures of the minimap at rest and at
200 km/h on the same road, or the minimap camera's parameters.

## Q90 (UI, not blocking): fast travel to undiscovered outposts, and A on the map

The engine offers FAST TRAVEL only on discovered outposts and the
festival places, and A with a route set clears it wherever the cursor is
(the help bar then reads CLEAR ROUTE). Unknown: whether the original
lists an undiscovered outpost as a destination, and whether A over a
different icon replaces the route instead of clearing it. Needed, narrow:
a capture with the cursor on an undiscovered outpost, and one of A over
another icon while a route is set.

## Q91 (UI, not blocking): the post-stunt fast travel card

When a mission at an outpost is done for the first time, the original
adds GasStation `IDS_MissionCompletedDiscount` (FAST TRAVEL DISCOUNT
EARNED) to the stunt's completed headline; the strings `IDS_Discount`
({0}% OFF) and `IDS_AvailableAtHub` (AVAILABLE AT {0}) suggest a card with
the discount and the outpost. No travel offer follows. The engine shows
the headline with FAST TRAVEL DISCOUNT EARNED as the message, without the
percentage, the outpost or the credits. Needed, narrow: a capture of a
first PR stunt completion.

## Q92 (UI, not blocking): X DETAILS on the map

X on an event or a street-race hub shows the event's ticket over the map
(`map_popup_event_details_orig.png`). That screen is the race pre-screen,
so the engine opens its events page at the hub (or the whole list) in the
meantime. B on the ticket returns to the map with the event still
selected (`map_popup_event_details_back_orig.png`). Unknown: whether A on
the ticket starts the event or sets a route. Needed, narrow: a capture of
A on the ticket.

The ticket is now drawn over the map; Q151 holds what is still open.

## Q136 (BOOT-01, BOOT-06, not blocking): skipping the boot splashes and the attract loop

The engine boots with the original's two splashes (`T10_MS_Combined`,
`Dolby_Corona_Intro`) and, after 53 s idle on Press Start, plays
`forza_intro.wmv` and returns to Press Start. Not recorded: which inputs
skip a splash (the engine keeps its hold-to-skip), whether any input other
than a button resets the idle timer, and whether the attract movie can be
skipped. `Forza_Tone.wmv` is no longer played anywhere; where the original
uses it is not known.

Needed: a recorded boot with inputs during each splash, and one attract
cycle with an input partway through.

The hold-to-skip is gone: a press skips (Q155).

## Q137 (BOOT-04, not blocking): the profile step after Press Start

The original reads its profile after Press Start, then shows the menu.
The engine reads its save at startup and shows a fixed 0.35 s step (the
logo fades, then the menu shows; `TitleMenu::kProfileSeconds`), because the
scene's `START_OUTRO` and `MENU_INTRO` slides are not decoded. There is no
profile picker or storage-device step. A save that does not read shows
`Main/IDS_ProfileCorrupt_*` (INVALID PROFILE) once the menu is up; what
the original does next (new profile, retry) is not known.

Needed: the slides' timelines, and the original's flow for a damaged
profile.

Update (2026-11-25): the slides are decoded (docs/UI_SCENE_ANIMATION.md).
The front end now plays `Menu_Intro` on Press Start: the logo fades out
over 0..0.299 s and the menu fades in over 0.493..1.013 s. The profile
step is the part before the menu shows, so `kProfileSeconds` is now 0.493
s. The original's profile read could hold it longer; that is inferred
(Q185). The damaged-profile flow is still open.

## Q138 (BOOT-02, BOOT-03, not blocking): the front-end menu's states and its offline popup

The selected row's size step (1.03) and offset, and the unselected row's
28 px indent and grey (195) are measured on `02_title_menu_orig.png`, not
read from the scene's states; `menu_shadow` is hidden. MULTIPLAYER offline
shows `LiveStrings/IDS_Not_Connected_To_Live_PopUp_*` (NO NETWORK ACCESS)
on `H_MESSAGE_POPUP`, the title the original uses for MESSAGE CENTER
offline; the original's popup for MULTIPLAYER on PC is not recorded.

v4's first row, `SPLASH_MENU_BUTTON_SPECIAL`, has a `NormalMode` and a
`SpecialMode` (a `MedalHUDUnlock` badge beside the text, its texture under
`DLC/CARCHALLENGE`, v4's 1000 Club). Its BGF contract
`BUTTON_TEXT_SPECIAL` binds both modes' `TEXT_BUTTON`, so the row carries
the first item in either mode; the engine draws `NormalMode`, which gives
`02_title_menu_orig.png` to the pixel. That row's tapes rest grey (0.13)
where the other rows' are pink; selected, the engine tints them the rows'
pink (0.98 0 0.39). The BGF's state records for these nodes do not decode
yet, so which state turns the tape pink, and when the original shows the
badged row, are not known (a short search of the recomp's v4 notes found
the 1000 Club but nothing on the title row).

Needed: the decoded `SPLASH_MENU_BUTTON` states, what switches
`SPLASH_MENU_BUTTON_SPECIAL` to `SpecialMode` (a 1000 Club medal?), and a capture of
MULTIPLAYER chosen offline.

Update (2026-11-25): the row states are decoded (docs/UI_SCENE_ANIMATION.md)
and the engine plays them (FE-02). `Blurred`: text 200, tape grey 33 on
every row, so the grey tape is the scene's own unfocused look. `Focus`
(0.1 s): text 233 on a pink (250, 0, 100) tape. `Focused`: a 2.5 s loop
whose Group pulses from 0.7 to 0.722. The capture's 1.03 is that pulse's
peak (Q185). `SpecialMode` turns the tape yellow (255, 221, 62) with dark
text. The offsets (8 and 28 px) are still measured; what selects
`SpecialMode` is still open.

## Q139 (LOAD-01, LOAD-02, not blocking): which tips a load shows

Each load shows its LoadingDefs.xml group's `tipcount` tips (random, or
the next in file order for `order="sequential"`, a cursor per group for
the session), each for its `displaytime`; a load longer than all of them
comes round to the first again. An event's group is `festival_N` with
N = the event's level + 1; `11b_event_loading_orig.png` (a level-1 event
while the player held the Green wristband) fits both the event's level
and the player's wristband, so which one the original uses is open.
Qualifiers use `festival_heats`, street races `streetrace`, showcases
`showcase`, the rest `festivalevent`. The other contexts (paint shop,
workshop, photo, skill and speed missions, online) are not wired, the
`layout` attribute's slides are not decoded, and every capture shows the
backdrop `IMAGE1`, which is the only one used.

Needed: the group index's source for festival events, whether sequential
cursors persist across sessions, and what shows after the last tip.

## Q140 (LOAD-03, LOAD-04, not blocking): the load screen's spinner and the event name

PLEASE WAIT and the spinner sit where `03_loading_tip_orig.png` has them;
the spinner turns once a second (set by eye). On event loads the engine
puts the event's name on the scene's `Stat_Text` (an engine request; no
original frame shows it), placed by eye. The minimum holds (1 s from the
title into free roam, 3 s into and out of events, none for fast travel)
follow the recomp; the engine's 0.25 s fades are its own.

Needed: the spinner's rate and the fades' timings from the scene's
timelines.

## Q141 (LOAD-05, not blocking): the load's mix fade

Loads use the Loading mix (game music 0.25, menu and homespace music,
cars and VO 0) and no UI music. With the game's `AudioMixerSnapshots.xml`
loaded the file's 2 s fade applies; the engine's built-in fallback keeps a
0.1 s fade. Whether the original plays the `LoadingScreen` UI event when a
load starts is not recorded; the engine plays nothing.

Needed: a recorded load with its audio.

## Q142 (BOOT-05, not blocking): an autosave or storage notice

No scene or string for an autosave or storage notice was found among the
front end's scenes or the string tables; the engine shows none.

Needed: a capture of any save notice the original shows at boot or after
Press Start.

## Q143 (V4-02..V4-07, not blocking): the 1000 Club screens' unmeasured parts

The 1000 Club now runs on the title update's scenes (`H_CAR_CHALLENGE_HUB`,
`_BUY_CARSELECT`, `_ACHIEVEMENTS`, `_DETAILS`, `H_POPUP_MENU`,
`H_MESSAGE_POPUP`), checked against `club_{home,carlist,carlist_tut,
select_action,tutorial,begin}_orig.png`. What the captures do not show is
interim:

- The home ticket's AchievementName and Description are hidden (the
  capture's first ticket shows neither); the later tickets (current car,
  VIEW ALL) and the hub after the first medal have no capture.
- The car list's FLAG, DRIVETRAIN, STATS, PURCHASED, PACK_NAME and
  PlayerBar are hidden; its help bar is A SELECT CAR / B BACK where the
  capture has A SELECT CAR / Y TOGGLE / LT SEARCH (no toggle or search
  yet).
- The SELECT ACTION panels and the car list's rows and backing are placed
  at offsets measured on the captures, not by the scenes' window sizing.
- The details screen's rows are all pink in the scene; the focused one
  keeps it and the others are dark (`kTapeDark`). Its small CURRENT CAR
  CHALLENGES tape rests across the car's name tape and is lifted 28 px
  clear of it.
- The achievements screen's rows show the achievement's name (the box
  shows its description, wrapped to the panel's 360 px); each row's
  Sponsor_Back pane, which rests far up and right of the row over the
  player bar, is hidden; NAV_POSITION_INDICATOR is hidden.
- A ticket's picture is the car's showroom thumbnail over the card's
  middle, under its gamerscore, G and title; club_home.png's picture is a
  scenic render whose source is not found yet. The first ticket's icon is
  the achievement's spanner where club_home.png shows a wheel, and the
  capture's dark progress track with its gold count is not drawn.
- The tutorial's last prompt (Visit the 1000 CLUB screen) shows after the
  first medal until the hub is opened.
- The Autoshow and garage show no per-car medals (`car_medals()` is the
  hook; those screens are another branch's).
- Fixed on the second pass: the two dark bars under the details and
  achievements panels were HUDPANEL01 mapped at v 0.3..1.3, sampling the
  atlas past the texture; a single-texture model now shows nothing past
  its texture's edge, as it already did across.

Needed: captures of the hub after a medal (three tickets), the
achievements and details screens, and a car list with several cars; the
car list's Y and LT actions.

## Q144 (V4-06, not blocking): the 1000 Club HUD's timings and cue

A passed challenge plays `UiCue::kAchievementReward` (the old sign-smash
cue is gone), shows the large medal for 1 s and holds MEDAL EARNED! or
FAILED for 3 s (`kClubResultHold`); the items' COMPLETE and FAIL
animations (their flashes and scale) are not run, and the item layout is
the scene's rest. No capture shows a running or finished challenge.

Needed: a capture (or video) of a challenge running, passing and failing
in the original, and the cue the medal plays.

## Q145 (V4-10, not blocking): the rest of Horizon Rally's first entry

The HORIZON RALLY pause row now follows `game_start_rally.xml`'s front:
without the yellow wristband it shows IDS_FirstTimeCareerError (A OK);
otherwise IDS_QuitHorizonToRally (A OK, B CANCEL; `rally_confirm_orig.png`),
both on `H_MESSAGE_POPUP`. The first OK plays Forza_Tone_Rally and saves
`rally_flow` 1, then the world switches to ColoradoDirt. The popups draw
over the world, where the original keeps the blurred pause menu under
them.

The rest of the first visit now runs, with `rally_flow` as its step
(0 never entered, 1 video seen, 2 heats driven, 3 first Autoshow left,
4 hub tutorial heard):

- The switch after the video starts `RALLY_HEATS` in its loaner
  (`DriveApp::start_rally_heats`, the hub's own heats start) before any
  hub. Its results' A plays `PostraceHeatsQualification` and
  `ToTheAutoshow` (`game_rally_intro_flow`'s `post_race_VO`).
- Then the Autoshow opens listing only the Horizon Rally cars (the five
  with `OptionalRallyUpgrades` rows, Q150), as
  `CSetRestrictionsForFirstRallyAutoshow`'s Autoshow; leaving it opens the
  hub. A profile that left at step 2 gets the Autoshow on its next entry.
- The hub's first opening plays `ChampionshipStructure` then
  `CoDriverIntro` (`first_time_hub_flow`) and saves step 4
  (`CSetHasSeenInitialRallyTicket` plus its micro-save). A profile that
  drove the heats before this step existed gets the tutorial on its next
  hub opening.

All four triggers are in `Rally_DialogueScript_EN.xml`. Interims: the
heats start from the engine's pre-race menu, where the original runs a
rolling start under AI control with the pep talk and the heats' intro
cutscene; and the Autoshow's restriction is the Rally
cars only, though the behaviour's actual contents are unknown. In the
harness's play bake none of the five Rally cars (nor the heats' loaner)
is baked, so the restricted Autoshow is empty and the hub opens straight
away ("no cars to buy"). The five are on sale at a price
(`club_rally_corpus_tests` checks the dealer list; 7,000 to 240,000 Cr),
so a bake with them lists them (checked with a baked stand-in accepted by
the filter: the Autoshow listed that one car and its exit opened the
hub).

Changed (WP5): the hub tutorial now follows `first_time_hub_flow`: the
help bar is hidden and the hub takes no input while `ChampionshipStructure`
plays (waited on), a 0.5 s pause, and `CoDriverIntro` (waited on); then
the help bar returns and step 4 is saved. A line that never starts
(no Rally VO bank) gives up after 2 s. The two post-race lines still
queue behind each other (`VoPlayer::trigger` without interrupt).

Needed: `CSetRestrictionsForFirstRallyAutoshow`'s restrictions (cars,
classes, credits) and the heats' rolling start and cutscene.

## Q146 (V4-18, not blocking): ColoradoDirt's speed traps and their records

The Rally world now reads the package's own `gamemodes/ColoradoDirt`
`speed_camera.xml` and `average_speed.xml`. They name the same 22 traps
and 9 zones as the disc's Colorado set, with the same scoreboard ids and
minimum speeds (`club_rally_corpus_tests` checks this), and ColoradoDirt
ships no GameObjs, so the gates are Colorado's. The engine keeps one set
of best speeds for both worlds, where the profile has a separate
ColoradoDirt block (22 cameras, 9 zones).

Needed: whether the original keeps Rally-world speed records apart from
Colorado's (and shows them on separate scoreboards), and where
ColoradoDirt's gates come from if not Colorado's GameObjs.

## Q128 (UI, not blocking): the car list's photo and tile renders

`108_C_BUY_CARSELECT` is bound for the Autoshow's car select, the garage
and Barn Finds (`car_list_screen.h`). Two pictures on it are not the
original's:

- **The card's photo.** `19_autoshow_orig.png` shows the selected car
  photographed outdoors (`CAR/Car_Thumb_`). The engine draws the car's
  thumbnail on the studio sweep (`KITPARTS/INFINITYCURVE`) in the
  measured photo rect.
- **The tiles.** `CAR_IMAGE_NEW` is filled at run time. The engine draws
  `Thumbnails/Thumbnail_<id>`, a 768x288 render that exists for only 154
  cars, on each tile's `InfinityCurve`; a car without one shows the bare
  sweep.

Needed: where the card's photo and the tiles' renders come from (a
render target of the car, or a file per car), and the camera they use.

## Q129 (UI, not blocking): the hub rooms' tracks and cameras

Mostly answered (WP3). The Autoshow, the workshop and the paint booth are
tracks on the disc (`uiautoshow`, `uiworkshop`, `uipaintshop`), and the
engine now draws each one in Colorado's place while its screen is open
(`hub_room.h`, `DriveWorldHost::enter_room_world`). MY GARAGE opened from
the workshop stays in the workshop (WP3b); from elsewhere it and Barn
Finds keep the studio stage (Q262). What reading them took:

- The rooms' models are local (within 200 m of the origin), so the room
  gathers them with `GatherOptions::local_models`.
- Their `.pvs` model lists differ from Colorado's: list 0 names no model,
  and the parser had taken its textures for a fixed header (`52b4b95a`).
- The rooms' game frame is the car's body frame (+Z forward), mirrored in
  z into the car model's frame; the car stands at the origin, its wheels
  on the floor there. Unturned, the paint booth matches
  `20_paint_shop_orig.png` (EXIT sign, doors and ceiling lights).
- A room's texture records move past Colorado's
  (`hub_room_record_base`). One record in the Autoshow (21) and one in the
  paint booth are in no bundle of the room's archive and draw untextured.
- Leaving a room re-prepares Colorado's last gather on the main thread
  (about 0.3 s); entering and leaving twice is clean
  (`19c_room_left_free_roam_wp3.png`).

The cameras are three files, not only the presets:

- The room lists (the Autoshow's browse, the paint booth's rows, the
  workshop's rows) play `camera.zip`'s `HomeSpaceCams.xml`: 12 cams over
  79.58 s, looping. Its first shot is the front-left three-quarter view of
  captures 19 and 20.
- The workshop's body parts and wheels play `WorkshopCams.xml`'s looping
  `Upgrade_Part_*` sequences, placed on the car's parts (`PartSpace`);
  its engine and drivetrain parts use the `Upgrades_*` presets. Which part
  uses which camera is Q221.
- The paint editor's regions use `LiveryPaintMenu_*` and the Autoshow's
  colour select `CarColorSel` from `UICarTargetCameraPresets.xml`. A
  change of preset blends over 0.75 s (smoothstep, the yaw the short way
  round).

Still open:

- the presets' X and Y angle limits and `allowZoom` (the engine's orbit
  limits stay), and the `LiveryLayerSelect_*` presets (the vinyl rows open
  the engine's editor, Q132);
- the blend's length and curve between presets (measured nowhere; 0.75 s
  is the engine's);
- the `HorizonAutoshow` preset, which no screen uses now (the Autoshow
  plays HomeSpaceCams);
- the presets' origin: the engine takes `posy` from the ground, but
  `CarColorSel`'s posy of -0.15 puts the eye below the floor that way, so
  the origin may be the car's centre or its body's base;
- which workshop part uses a sequence and which a preset (Q221);
- the rooms' light (Q222, partly answered; the car's is Q261) and the
  Autoshow's crowd and DJ (Q223).

## Q130 (UI, not blocking): MY GARAGE on 108, the manufacturer select on 115, and their help bars

No capture shows the garage's own list; the parity run that should have
(`orig-garage`) stayed on the map. `11_car_select_orig.png` shows the
garage's cars on `108_C_BUY_CARSELECT` for an event (no line under the
cursor, a buy-car tile, a wheel icon on the current car, and A SELECT,
B BACK, X CAR LIST, Y TOGGLE, BACK SEARCH), so MY GARAGE
(`carflow IDS_Garage_View_Title`) is drawn on the same scene, in the
original order (class, then PI; GARAGE_UPGRADES "The active car and
switching"). Interims: no buy-car tile and no current-car icon; X CAR LIST
(a list view, probably `106_C_CAR_LIST`) is not built; SEARCH steps the
class filter. The engine's sorts, favourites and selling stay as unlisted
extras. Needed: a capture of MY GARAGE from the workshop's CHANGE CAR, the
SEARCH screen's options, and which icon marks the current car.

No capture shows the Autoshow's first step either. The engine draws it on
`115_C_BUY_MFRSELECT` (`ScreenTitles IDS_Manufacturer_Select`): a strip of
the makes' logos (`List_CarMake.IconPath` on each tile's `MANU_IMAGE`) in
alphabetical order, the make's name under the cursor, `carflow
IDS_Num_Available` ("{0} AVAILABLE") for its count of cars, and A SELECT,
B BACK, Y ALL AFFORDABLE CARS (`HelpButtons IDS_AffordableCars`). Guesses:

- The strip holds the makes alone; the engine's All Affordable is Y, its
  All Cars the car select's SEARCH, and its Hidden & Showcase tab (an
  engine extra) a text tile after the makes.
- The info panel (`InfoPanelGroup`) would show the make's
  `List_CarMake.Profile`, but every profile on the disc is "N/A", so the
  panel is not shown.
- The tiles' DLC, NEW and lock icons are not shown.
- Y with nothing affordable opens carflow's NONE AFFORDABLE popup
  (`IDS_No_Affordable_Title`, `IDS_No_Affordable_Msg`) with A OK, and the
  strip stays.

Needed: a capture of the manufacturer select, its help bar, what the info
panel shows, and where the cursor starts.

Per-car 1000 Club medals (V4-05) stay off these lists. The scenes say so:
each tile of the club's own list (`H_CAR_CHALLENGE_BUY_CARSELECT`) has
`Icons/SmallMedal` (`MedalSmall`, `MedalComplete`) for its medal count,
while `108_C_BUY_CARSELECT`, on the disc and as the title update
replaces it, has no medal node at all (its `CLUB_IMAGE` and
`Icons/Icon_1..3` are untextured). The captures agree, though they are
probably, not certainly, from one session (the same profile, car and
money): `club_carlist_orig.png` shows the MX-5 with a medal and 0/5,
`11_car_select_orig.png` the same MX-5 with its wheel icon only, and
`19_autoshow_orig.png` (HORIZON RECOMMENDS) no medals. In
car_challenge_menus.xml the medal screens are the club's own states
(ShowCarChallengeCarSelectScreen, ShowCarChallengeGarageCarSelectScreen,
ShowBuyCarChallengeDetailsScreen, where "{0} MEDALS AVAILABLE", OWNED and
NOT ELIGIBLE come from), reached from the 1000 Club's ALL CHALLENGES
(manufacturer, then model, then the Autoshow's purchase), not from the
Autoshow, MY GARAGE or the workshop's CHANGE CAR. So those draw no
medals, and `car_medals()` belongs to the club's lists; that buy path is
the club's (Q143's ALL CHALLENGES interim). Needed: a capture of MY GARAGE
and the Autoshow's CAR SELECT after the club is opened.

## Q131 (UI, not blocking): the workshop's rows

Dak's garage opens `H_GENERIC` with Workshop.str's rows. Their order is
not captured; the engine lists CHANGE CAR, UPGRADE CAR, PAINT CAR, BARN
FINDS (the table's order), with AUTO UPGRADE and CUSTOM UPGRADE under
UPGRADE CAR. MY UPGRADE SETUPS and BUY UPGRADE SETUPS are not listed.
Needed: a capture of the workshop's list.

## Q132 (UI, not blocking): the paint booth's vinyl rows

The paint booth's rows (PAINT CAR, ADD DECALS, CREATE VINYL GROUP, MY
VINYL GROUPS, FM4 VINYL GROUPS, CLEAR DESIGN; `20_paint_shop_orig.png`)
are on `H_GENERIC`. The vinyl rows open the livery editor's layer stack
(an engine extra) because the original's group browsers and decal picker
are not ported; CLEAR DESIGN opens it too. Needed: the screens behind
those rows (scenes and flows).

## Q133 (UI, not blocking): the Barn Finds screen's details

Q116 left the garage's Barn Finds for the garage screen. The engine now
draws it on `108_C_BUY_CARSELECT` under `ScreenTitles
IDS_SelectBarnFindCar`: every barn in barnfinds.xml order, the tile's icon
from the scene (`Ready_Icon`, `Not_Ready_Icon`, `Undiscovered_Icon`) over
the car, Barnfind.str's state under the cursor, and the popups on
`H_MESSAGE_POPUP` (COLLECT CAR, NOT READY, NOT DISCOVERED; CAN'T SELL BARN
FIND when selling a barn's car from MY GARAGE). Collecting replaces the
old collection when the garage opened. Guesses:

- An undiscovered barn's tile shows no car and its card is hidden.
- VIEW ON MAP is offered on NOT DISCOVERED for a rumoured barn (the map
  centres on the rumour's circle) and on NOT READY (on the barn).
- A on a collected barn does nothing.
- The icons draw at full opacity (the scene holds them at 0.4).

Needed: a capture of the screen with barns in each state, and which
popups offer VIEW ON MAP.

## Q134 (UI, not blocking): MY PROFILE's TEAM CREDITS and STATS

MY PROFILE's TEAM CREDITS opens `834_CREDITS`, a scrolling credits roll.
The scene's scroller is not run, so the row shows "TEAM CREDITS — NOT
AVAILABLE YET". Needed: the credits text's source (a string table or a
file) and the scroll's speed.

STATS is on `H_STATISTICS`; `26_my_profile_stats_orig.png` shows only
GENERAL. The title's code lists `IDS_GeneralStats`, `IDS_RecordsStats` and
`IDS_TimeStats`, so the engine's tabs are GENERAL, RECORDS and TIME. The
other two tabs' rows are the engine's reading of the MyProfile.str names:
- RECORDS: HIGHEST SKILL CHAIN SCORE and the thirteen ULTIMATE ... SKILLS
  (`IDS_Num_Perfect_*` with English text; `IDS_Num_Perfect_Turns` has none);
- TIME: the three TIME IN ... RACES, then TIME SPENT IN THE MENUS,
  UPGRADING, TUNING, EDITING LIVERIES, TEST DRIVING and IN AUTOVISTA.
GAME PROGRESS is the engine's measure: the share of the free roam's finds
and activities done (signs, barns, roads, outposts, PR stunts, speed traps
and zones). The engine does not count the ultimate skills or the menu and
shop times, which show zero. Needed: each tab's rows and order, GAME
PROGRESS's formula, and whether the tabs wrap.

## Q135 (UI, not blocking): OPTIONS's pages (DIFFICULTY answered by capture 24)

`24_options_difficulty_orig.png` answers the rows: the original's
DIFFICULTY page (`300_I_OPT_ASSISTS_DIFFICULTY`) lists GAME DIFFICULTY
(`Assists IDS_MetaDifficulty`, "Automatically adjusts the driving assists
below ..."), BRAKING, STEERING, TRACTION CONTROL, SHIFTING, DRIVING LINE,
COSMETIC DAMAGE, REWIND and OPPONENT DIFFICULTY, with TOTAL RACE BONUS
under them and A SAVE, B CANCEL, Y RESET TO DEFAULT. So the original has a
difficulty preset in its own layout, and the engine's (the settings row
"difficulty preset") is drawn as GAME DIFFICULTY there; the engine draws
the page from its settings rows (`options_screen.h`). The engine's
STABILITY CONTROL row is not on the capture and stays on its own page.
Still open:

- The preset's values: Assists.str has EASY, MEDIUM, HARD, EXPERT
  (`IDS_MetaDifficulty_Insane`), PRO (`IDS_MetaDifficulty_Extreme`) and
  CUSTOM; the engine's preset steps EASY, MEDIUM, HARD, EXPERT and shows
  CUSTOM, with no PRO.
- The defaults RESET TO DEFAULT restores (the engine uses its rows'
  defaults, and GAME DIFFICULTY follows the reset assists).
- OPPONENT DIFFICULTY's values (the capture shows MEDIUM; AIDiff.str has
  EASY, NORMAL, HARD). RESET TO DEFAULT sets it to MEDIUM, a new profile's
  level (Q71), not the engine's "default".

AUDIO and CONTROLS are on their scenes too (`837_I_AUDIO_OPTIONS`,
`831_C_OPT_CONTROLLER`). Open there:

- AUDIO has no capture. The rows are the engine's reading of MyProfile.str
  (`IDS_AudioOption_*_Desc`, `IDS_SatNav_Desc`): SFX VOLUME, RADIO VOLUME
  (837's two sliders), RADIO STATION, RADIO DJ, GPS VOICE, PLAYBACK
  SYSTEM. The engine has no effects-only volume, so SFX VOLUME sets its
  master volume (which the radio also follows). RADIO STATION shows the
  station playing or OFF, and steps the radio on and off, not through
  MyProfile.str's three stations. PLAYBACK SYSTEM's HOME THEATER is the
  engine's full dynamic range. Needed: a capture of the page, its rows and
  the slider's length and step.
- CONTROLS: SWITCH GEAR UP/DOWN (`IDS_Option_InvertGearUpDown`) has no
  engine setting; it shows OFF and does not step. The capture pins layout 1
  with automatic shifting only; the labels for manual shifting (SHIFT UP,
  SHIFT DOWN, CLUTCH with the clutch) and for the other layouts are the
  engine's bindings by MyProfile.str's names. The engine's d-pad up and
  down (photo mode, telemetry) are its extras and read NOT USED, as on the
  capture. The diagram's places are measured on the capture (the disc's
  are the entry animation's start). Needed: captures of the other layouts
  and of manual shifting, and what X ADVANCED lists (831's
  `Group_ADVANCED`, the deadzones; the engine opens its CONTROLS page).
- VIDEO: no scene carries its name. MyProfile.str's VIDEO options are
  BRIGHTNESS (`IDS_VideoOption_Brightness_Desc`, the gamma screen
  `830_GAMMA_ADJUST`) and SUBTITLES; the engine has neither row, so VIDEO
  opens its DISPLAY page. Needed: the VIDEO page's scene and rows.

## Q150 (V4-19, not blocking): what the Rally achievements count

The writers for 208, 211, 214 and 215 now run:

- A Horizon Rally car is one with `OptionalRallyUpgrades` rows: the five
  the package sells (`club_rally_corpus_tests` finds exactly five, none of
  the event-only twins 1626..1635). 208 counts one given or bought
  (`DriveApp::award_garage_car` and the Autoshow purchase); 215 counts
  Autoshow purchases only. Cars owned before this build are not counted
  until they are obtained again.
- 211 sums the horizontal distance covered with no wheel down during a
  Rally stage (start to finish), in metres.
- 214 counts a stage won with no rewind and no crash, where a crash is an
  impact the skills treat as one (`WorldCollisionThreshold` /
  `CarCollisionThreshold`, 12 m/s, which cancels a skill chain).

Needed: the original's air measure (distance or the Air skill's own
length), its crash test for 214, and whether a car won as a prize or
given counts for 208.

## Q147 (V4-12..V4-14, not blocking): the Rally stage sequence's screens

`game_rally_race_flow` runs each stage as prepare (every car repaired,
`reset_car_damage_all_cars`), intro and pre-race menu (`onQuit` back to
free roam), countdown (`CRallyStageIntro` on a first start,
`CCoDriverPepTalk` every time), the stage, then the finish-position
screen and, while stages remain, `CGoToNextHeat` with
`CShowNextMultiStageLoadingScreen`. The single-player flow has no
service menu and no stage or rollup scoreboard screen (those are the
Car Club's and online's). After the last stage come the earnings, the
Rally points screen (`CIsRallyXPEvent`), a pending prize car and, once,
the champion sequence (`Rally_Champion_Cutscene`,
`CPlayRallyChampionDialogue`, `H_HORIZON_RALLY_CHAMPION`,
`CSetHasSeenRallyEndSequence`).

The engine now drops the Service Area: between stages it shows the
stage's result and standings under rally.str's STAGE RESULTS with
A CONTINUE, repairs the car and loads the next stage. Quitting a stage
from the pause menu ends the event and returns to the Rally world.
Interim: that between-stage page is the engine's layout, not the game's
finish-position screen, and it shows the standings the game's flow does
not; the stage intro and pep talk are not played.

Needed: the finish-position screen as shown after a Rally stage, and
the next-stage loading screen's content.

V4-12, the multi-stage event select (ALL STAGES or one stage): the
flows place `CShowMultiStageEventSelectionScreen`
(`ui_show_screens.xml`'s `show_multi_stage_event_selection_screen`) on
Race Central's Rivals path only (`race_central.xml`'s
`multi_stage_event_menu`, from the rival category and event screens). The
single-player Rally rivals ticket (`career_event_activations.xml`'s
`rally_rival_event_select_screen`) sends that screen's exit back, and
`game_rally_free_roam_flow` has no stage select. So a championship ticket
has no stage step, and a stand-alone stage result (the reference save's
`RALLY_4B`, `NumRallyStages` 1) comes from a Rivals run. Not built: the
engine's Rally rivals entry is still one route (V4-11's event tickets
would carry the rest), with no ALL STAGES choice and no per-stage result
written.

V4-14, the results: the engine's results page now titles itself with
rally.str's RALLY RESULTS (`IDS_ResultsRallyTitle`) over the event's
name, heads its table RALLY STANDINGS and labels TOTAL TIME from rally.str,
in place of its own "RALLY HEATS COMPLETE" / "CHAMPIONSHIP RESULTS" and
"GREEN RALLY WRISTBAND ..." lines (`club_rally_corpus_tests` checks each
string). The champion part plays once: `DriveSave::rally_end_seen`
(CSetHasSeenRallyEndSequence) is set the first time the final is won on
the podium, with VO `HorizonRallyChampion`. Inferred: that trigger as
`CPlayRallyChampionDialogue`'s line (the behaviour takes no parameter;
the dialogue script also has `ChampionAgain` and `PlayerWonRally`).
Changed (WP5, Q212): a championship's end now shows
`H_POST_RACE_RALLY_FINISHED`, `H_POST_RACE_RALLY_POINTS` and, once,
`H_HORIZON_RALLY_CHAMPION`; the engine's page is X on the banner and the
heats' end. Still interim: `Rally_Champion_Cutscene` is not played. rally.str's `IDS_RallyFinalTop_1..4` / `IDS_RallyFinalBottom_1`
read "NEW EVENT UNLOCKED", "NOW GET OVER TO THE", "NOW GET TO THE",
"YOU'VE UNLOCKED THE" / "HORIZON RALLY FINAL": the final's unlock
notice, not the champion screen.

Needed: which line `CPlayRallyChampionDialogue` plays, the champion
screen's texts, and the points screen's rows.

## Q148 (V4-16, not blocking): Horizon Rally's progression rules

Changed on the evidence of the flows and SPEC-V4-CONTENT:

- A championship unlocks its `Rewards_EventUnlock` successor when it is
  finished, at any place. `game_rally_free_roam_flow` runs
  `CTriggerRallyEventUnlocks` on hub entry when `CCompletedLastEvent` says
  yes; the engine required a podium.
- `RALLY_HEATS` runs with 15 drivers (`race::kHeatsDrivers`) where the
  engine ran the full 49-driver field. Interim: the 14 AI are the field's
  first 14.

Unchanged and still open: whether a podium check sits inside
`CTriggerRallyEventUnlocks`; the champion rule (the engine: the final
championship, `MS_RALLY_02`, on the podium, with its `UnlockPointsReq`
of 310 Rally XP); Rally XP on a replay (the engine adds only the gain
over the best earlier place, where the spec reads as the full
`RallyScoring` points each time); the credits' difficulty and assists
bonus; and which drivers make the heats' field. `CInitialiseEventUnlocksRally`
takes no parameters in `game_start_rally.xml`, so the first-visit unlock
set stays the engine's (the heats, `MS_RALLY_05` and the 28 stages,
which the reference save confirms).

## Q149 (V4-11, V4-21, V4-23, not blocking): the Rally world, the co-driver's lines and level-up gifts

V4-11, the hub as free roam: settled (Q211). The reading above, that
the Rally is ColoradoDirt free roam, was wrong: `game_rally_free_roam_flow`
removes the car and shows `CShowUIScreen_RallyHub` on `H_RALLY_HUB` over
BgLoop.wmv, a paused menu. The engine now draws that scene. Driving
ColoradoDirt (the FREE ROAM COLORADO DIRT extra, its world triggers and
speed traps) is the engine's own, kept off the hub's main layout.

V4-21, the co-driver and Alice: the first visit now plays
`PostraceHeatsQualification`, `ToTheAutoshow`, `ChampionshipStructure`
and `CoDriverIntro` (Q145), and becoming champion (the final on the podium,
once) plays `HorizonRallyChampion` (Q147); `club_rally_corpus_tests`
checks that each resolves to a sample in `Rally_VO_EN`. Not played: `CCoDriverPepTalk` before
every countdown and `CRallyStageIntro` on a first start. Both behaviours
take no parameters; the dialogue script's candidates are `PreRace`,
`PreRaceFirstStage`, `PreRaceLastStage`, `FinalPreRaceFirstStage`,
`FinalPreRaceSecondStage`, `FinalPreRaceFinalStage` and
`PreraceHeatsIntro`, and for the finish `FinishedStage`,
`FinishedStageFirst`, `FinishedStageLast`, `FinishedIntroRace` and
`PlayerWonStage`. The finish call's Good/Bad split at 10th place is
still the engine's.

V4-23, the driver-level gift cars: unchanged; `driver_level_gift_car`
returns the level's first `Rewards_DriverLevelv3` row, with no
ownership filter for DLC cars. In v4's `pause.xml` the Message Centre
(`CShowMessageCentreScreen`) has `onGiftCarPrepare` /
`onGiftCarUnprepare` transitions, so a gift is claimed from a message,
which suggests a choice among the level's rows (`MaxGiftCars` 5 to 7).
The UI archive also has `UI4/BuyCar/GiftCarChoice.xds` (the recomp's UI
catalogue), more support for a choice. WP5 searched the recomp's derived
source for `CCoDriverPepTalk`, `CRallyStageIntro` and the gift message
and found no decompiled body, so V4-21's and V4-23's rules stay open.

Needed: which line each of `CCoDriverPepTalk`, `CRallyStageIntro` and
the finish picks; the gift message's rule (one of N or all N) and its
DLC filter; and the Rally ticket's trigger radius and speed.

Needed: `CTriggerRallyEventUnlocks`, `CIsRallyXPEvent` and the Rally
reward path in the generated v4 code.

## Q121 (WORLD-05, not blocking): the one-on-one nemesis catch-up flag

The race-start and nemesis boosts are decoded (C4 settled). The boost
update reads the `AI/Racing/RaceBehavior/RaceStartBoost/*` and
`.../NemesisBoost/*` registry values. Each is registered with a
compiled-in default, and those defaults are the old "code-only" figures:
0.4 and 0.1, 300 and 700 m, 1.5 and +0.3, and 50, 100 and 500 m. The
title loads GlobalRegistry.xml over them at boot, matching each
`<Value id>` to its registered value. So the disc's values are the ones
that run: 0.6 and 0.2, 500 and 250 m held, a 1,000 m decay, a skill
factor of 2.0, 20 m a car, 25 m/s, nemesis 1.1 and +0.4, and catch-up
+0.6 over 0 to 150 m, off 300 m from the finish. The engine now runs the
decoded rule with those values (`race::race_boost`).

Open: the one-on-one branch, `PlayerCatchupTorqueBoost`, runs only while
a game-state byte is set. That byte is copied from a byte of another
game object, and what that object is was not traced. Interim: on. On
`NR02` this branch moves the rival from 160.7 s to 138.6 s (oracle
130.7 s).

Needed: what the flag means (for example single-player career only, or a
difficulty or assist option), from a trace of that byte on `NR02`.

Also interim: the distance the boost reads is the distance raced past the
start line, and the speed test is in m/s. The title reads both from the
car's route state and does not name their units.

## Q122 (WORLD-02/03, not blocking): the AI-driven car's assists and the planner's calibration

WORLD-02 and WORLD-03 are done: the race AI's own modifiers are gone.
That covers the speed-narrowed turn-in cap, the 3.8 lock/s steer rate
limit, the 0.29 alpha gain (now 0.27), the stretched and clamped
look-ahead (now 1.0 s x v, at least 2 m), `off_line_slow`, the rollover
cap on C, the cornering grip boost (the tyre's AI grip factor is now 1
outside the car's dirt term, as RACE_AI's AI-only grip terms say) and
`ai_traction`'s throttle cap and slip cut. The planner's vertical
curvature now runs unclamped with the set's
`SpeedComputerSlopeCurvatureModifier` (`default` 0.75, `freeroam` 1.0,
`streetrace` 0.5) instead of a fixed 0.75.

Without `ai_traction`, the full on/off gas spun the heavy rear-drive
cars. NR02's Challenger went from 138.6 s with 0 resets to 277.1 s with
10 resets. The engine now runs the title's own assists on AI-driven cars:

- TCS with PhysicsSettings.ini's `...AI` keys: `TractionControlSpeedAI`
  22, `TCSFullEffectFricDiffAI` 0.25, `TCSFullSteerSlipScale{RWD,FWD,AWD}AI`
  1.3/1.7/0.71, and a takeoff slip of 1.0. The evidence is fair: on the
  oracle lap the title's TCS amount was non-zero under the AI.
- STM with the AI's window [1.0, 2.0]. The evidence is weak: the decoded
  code has an AI window, but whether the AI's car has STM on is open
  (CAR_PHYSICS, Unknowns). On the engine's oracle lap STM acts for 87
  steps (0.7 s, amount up to 0.26) at 958 to 965 m, where the title's
  never acted. The engine's TCS reaches 1.0 for three steps at the launch,
  against the title's 0.064 peak.

Update: under the decoded line controller (Q125), STM acts on 0 steps of
the oracle lap. TCS still reaches 1.0, but only for three steps (25 ms)
at the launch. In a race the AI's TCS target is a slip ratio of 0.77
(`AssistsTCSSlipDefMoving` 0.5, `TCSFullEffectFricDiffAI` 0.25 and the steer
scale), so on NR02 it never cut the Challenger's power slide (rear slip
0.15 to 0.6). Running the race without TCS or without STM made NR02
slower, by 31 s and 27 s.

Gate (seed 1, AI mean time / resets), oracle in brackets:

| Event | Before (f55db691) | Neither assist | TCS + STM | TCS + STM + `ai_traction` |
|---|---|---|---|---|
| FR22 (161.5) | 172.6 / 0 | 194.4 / 0 | 192.7 / 2 | 188.9 / 2 |
| FR16 (124.0) | 123.4 / 1 | 137.4 / 1 | 121.8 / 0 | 123.6 / 0 |
| FR24 (110.8) | 108.6 / 0 | 109.0 / 0 | 108.4 / 0 | 108.9 / 0 |
| FR01 (102.7) | 108.6 / 1 | 121.3 / 8 | 105.8 / 1 | 102.2 / 1 |
| NR02 (130.7) | 138.6 / 0 | 277.1 / 10 | 151.5 / 0 | 132.6 / 0 |
| EXHIBITION_FEST_003 (179.0) | 193.8 / 0 | 197.9 / 2 | 185.0 / 0 | 209.9 / 4 |

TCS + STM ships; `ai_traction` stays only on the pure-pursuit path
(`SHIFT_AI_STEER=pursuit`). NR02's rival lost 20 s of the 25 s it had
gained on the invented grip boost; that boost lifted a catch-up car's
grip up to 1.25.

Still interim:

- The capacity calibration (`AiCapacityCalibration`, the R34's
  16.01/14.6 lateral and 18.26/17.1 braking, blended out below 1,100 kg)
  stays until the physics' raw tables match the title's dumps (the audit
  says to fix the physics first). The engine's car model has no
  downforce for `ai_aero_factor` to scale.
- The tyre's AI grip factor's cornering, heavy-braking and state terms
  are 0 (RACE_AI, interim).

Needed: whether the AI-driven car has TCS and STM on, and with which
slip targets (`AssistsTCSSlipDefMoving` for the AI), from the assist
flags of an AI car in a race trace; and an AI car's speed against its
plan on `FR22` and `NR02`, to tell the planner's tables from the car's
handling.

## Q123 (WORLD-01, not blocking): the passing search's interim parts

The overtaking heuristic (`plan_lanes`) is replaced by the decoded
dynamic-line search (`race::dls_update`, libs/race/src/dls.cpp). It runs
on each set's `<DLS>` and `<PassingDriver>` values from AIRacing.xml:
`default` for festival races, `streetrace` for street and traffic races,
`freeroam` in free roam. Decoded and implemented:

- 201 slots 0.5 m apart around the car's predicted place, with slots
  1 to 199 scored;
- the OUT and EDGE slots from the road's half width at the meeting point;
- the cars alongside (within 3 m) covering every slot from their near
  edge, and the predicted meetings covering the other car's lateral span
  widened by ExtraCarWidth;
- the cost sum (OutOfBounds, OffTrack, Car, OffLine weighted by
  max(1, v x 3.3 / d), TimeToCollision on the slot and half its
  neighbours, ChangePassSides, HeadOnPassWrongSide by LegalSideOfRoad);
- the strict argmin, accepted below MaxAcceptableCost;
- a search every 0.2 s, and on failure a hold, +5 frustration and a
  retry next frame, with following past 1000;
- the target as the first car met (a hit, or a miss under 0.2 m) among
  those not yet passed;
- the lane's rate as the offset over the distance to the meeting,
  capped at 0.2 of the half width a waypoint (2 m).

Interim (ours):

- The predictions run along the shared line at constant speed. Each car
  keeps its lateral place, and the own car moves to its lane over 1 s.
  The boxes are aligned with the line, not the four corners.
- A covered car is skipped when its near end is more than 1.5 own half
  lengths from the car at the meeting. The decode reads "the along-track
  positions differ by more than 1.5 x own half-length", which would skip
  a car about to be rear-ended.
- The target stays chosen until the car is past it or stops closing on
  it, as if in the same lane.
- Frustration grows by 5 a frame of a 30 Hz AI tick (150 a second). A
  following car searches again once a second and stops following when a
  search succeeds, when the target is 20 m ahead, or when it is passed.
  A traffic car is chosen afresh every step, since its index is not
  stable.
- After a pass, or with no target, the lane returns to the line at 1 m a
  25 m (at least 0.3 m/s), never into a car alongside.
- Following trails at 0.75 x TrailingDistance. With any car in both the
  car's path and its lane, a speed cap brakes at 12 m/s^2 to 1 m behind
  it.
- Not modelled: the use-rate flag, the low-relative-velocity passes, the
  starting-line passing state (`StartingLineMinSpeedForPassingState`,
  `StartingLinePassingDynLineUpdateInterval`), the temperament's TTC
  threshold and front margin, and ChiVariance.

Gate (seed 1, AI mean time / resets / car-to-car contacts), plan_lanes
then the search: FR22 192.7/2/11178 to 188.3/1/1857, FR16 121.8/0/450
to 133.3/2/1467, FR24 108.4/0/819 to 109.1/0/1532, FR01 105.8/1/1576
to 105.0/0/1367, NR02 151.5/0/56 to 135.0/0/121 (one AI: the change is
the streetrace set's 0.5 slope weight), EXHIBITION_FEST_003
185.0/0/750 to 193.9/2/1551. Contacts rise on four routes: the interim
trailing lets a car close to a metre.

Needed: the transition shape from the current lane to the chosen one,
the return to the line after a pass, the trailing controller, and what
the "along-track" test in the coverage compares.

## Q124 (WORLD-08, not blocking): festival group spawns

The roaming festival population now follows SPEC-TRAFFIC's population
target with the festival entries: target = min(D_s, cap(h)). D_s is the
festival `DensityList`'s d per 100 m of road within the spawn-out
distance, smoothed by `DensityDecay` every `DensityTestInterval`, and
cap(h) runs from `MinFestival` to `MaxFestival`. A way whose density id
the festival list lacks gets no festival cars (spawn weight 0). Before,
the target was the cap alone, 2 to 7 cars wherever the player was.
Without a festival list (no AIOpenWorld.xml) the cap alone still
applies.

Interim (ours): the same per-100 m reading as traffic (SPEC-TRAFFIC,
Density), the 5 s spawn interval, and the spawn ring (180 to 320 m, or 45
to 240 m for the first cars).

Not modelled: `AI/Spawning/FestivalGroupSpawns` (on, on the disc). The
registry flag is read, but what a group spawn places (several cars
together, a shared destination) was not decoded (audit WORLD-08:
"festival group destinations").

At the oracle's census road (TRAFFIC, Tests: 746 m of density-5 and 68 m
of density-2 road within 370 m), the per-100 m reading of the festival
list (id 5 0.1 to 0.3, id 2 0.1 to 0.2) gives 2.4 at noon and 0.8 at
02:00. The oracle counted 0.8 and 1.7 festival cars within 1 km. The old
cap-only target was 7 and 2. The reading's night fall is the wrong way
round for the festival cars, so their hour factor may not be traffic's.

Needed: the group spawn's rule (how many cars, where, and whether they
drive together), the festival cars' hour factor, and a free-roam count
of festival cars against D_s at one more site than the oracle's road.

## Q125 (WORLD-02/03, not blocking): the line controller's interim parts

The race AI and the oracle lap's follower now steer by the decoded line
controller (PredictiveControllerRLH) instead of the slip rule. The slip
rule was ours: `0.27 alpha + 0.29 beta` on a look-ahead of 1.0 s x v.
The decode found no slip-angle or countersteer term anywhere between the
controller and the wheels. The engine now runs these decoded parts:

- The look-ahead is T x v, with T = clamp(1.2 / G, 0, 1) s. It carries
  the slide stretch, the speed floor, the limits on growth and fall, and
  the widening by 40 x (|chi| - 1), clamped to 6 to 100 m.
- The aim is the Hermite curve at s = 0.8, which gives omega = 2 v sin(a)
  / l (or a, past |chi| 1.1). Inside |chi| 0.9, omega is capped by the
  lateral limit.
- steer = omega / G(v), less a yaw-rate damping of 0.75 (2 / G)^2 that
  never crosses straight ahead. The under-steer boost (1 to 2) applies
  while steering back.
- More than a metre off the line, the aim point moves 0.2 e towards the
  car and the tangent turns towards the point.
- Track-edge avoidance acts on the throttle only (prev - 0.016 d|e|/dt),
  and the set's `DisableTrackEdgeAvoidance` turns it off.
- With the heading 84 degrees or more off the line, the gas is at most
  0.6.
- The steering-saturation grip term goes on both axles' AI grip factor.
  It applies past 0.8 of the lateral limit's curvature and is recomputed
  every update. Its cap is ramp(level, threshold, 1.25): it opens only
  while the controller's level is above the threshold (0.94, or 0.97 on
  a four-wheel-drive car). The oracle lap's follower does not apply it.
  On the title's oracle lap, all 285 of the AI grip factor's non-unit
  samples have the 1.1 rear ratio, with no equal-axle term.

The oracle lap is now 38.976 s, +0.50 % (it was 39.003 s under the slip
rule). Its cross-track error is at most 1.04 m (rms 0.27 m), against
3.38 m before. STM acts on 0 steps, against 87.

Interim (ours):

- G(v) is the yaw rate per unit steer, in 20 bins of 5 m/s. Ours comes
  from our own vehicle sim, a small steady steer on flat ground. The
  title builds its table per car with its own sim, which was not decoded.
- The track's look-ahead floor is 8 m; the title's is 6 to 10 m, by
  track. chi is the cross-track over the line's room on that side (4.5 m
  on the oracle lap).
- The pedals keep our band rule by default. The title's pedals are now
  decoded (the hold throttle and the off-track cap included) and run
  with `SHIFT_AI_PEDALS=title`; Q175 has why they are not the default.
- The saturation term's threshold of 0.94 is 0.97 on a four-wheel-drive
  car: the integer on the car body is the drivetrain, the same test as
  k_B's 0.96.
- The level is no longer the planner's C, but the level of 1 is an
  interim. The term compares a controller field with the threshold. The
  writers found for that field are the constructor (1) and the cheat
  level's setter (1 + 0.025 n for the driver's AISkills cheat level n,
  0 on every row of the disc: Q176). The planner's C and the start and
  nemesis boosts do not reach it. Under the old level, a boosted car
  reached 1.25 and the nemesis boost's flapping threw the Challenger's
  grip between x1 and x2. At level 1 the term is at most 0.19 on a
  two-wheel-drive car and 0.1 on a four-wheel-drive one. The capture
  argues against level 1, though. On our oracle lap at level 1, the
  term's condition holds on 352 steps (peak 0.107 on the
  four-wheel-drive Skyline), but the title's oracle lap shows no
  equal-axle sample at all. So either the title's level on that lap is
  at most the threshold (as with an earlier reading, 0.95 for a car with
  no drivatar profile), or its lateral limit is higher than ours and the
  condition never holds. A writer of the field that this decode missed
  would explain the first.
- The rule that caps a driver's skills at 0.93 for a heavy car is
  applied, on operands that are ours (Q177).
- The AI grip terms add up. The dirt term and this one are summed and
  then clamped to [0, 2], as in the title.

Gate: the 3-seed mean of each seed's AI mean time, in seconds, with the
oracle in brackets. f55db691 is before WORLD-02/03, and the slip rule is
6faaef8c.

| Event | f55db691 | Slip rule | Controller | Level 1 |
|---|---|---|---|---|
| FR22 (161.5) | 173.1 | 192.5 | 163.0 | 169.5 |
| FR16 (124.0) | 121.9 | 127.3 | 122.6 | 123.9 |
| FR24 (110.8) | 108.4 | 109.3 | 109.0 | 109.7 |
| FR01 (102.7) | 103.6 | 102.3 | 101.9 | 105.0 |
| NR02 (130.7) | 153.5 | 139.6 | 179.3 | 147.9 |
| EXHIBITION_FEST_003 (179.0) | 191.0 | 194.2 | 186.6 | 180.5 |

"Level 1" is the level of 1, the four-wheel-drive threshold and the
0.93 cap, on the band pedals. Its resets are 8 in the 18 races
(the controller's: 11). FR22's 169.5 s is one seed: over 9 seeds it is
166.4 s.

Car-to-car contacts per race were as high as 33,151 under the slip rule
(FR22, seed 2). Most of that count came from cars piling up behind the
end of the line after finishing. Under the controller, contacts are 150
to 831 per race.

NR02 is the event the controller misses. Its one rival, the Challenger,
power-slides at about 40 m/s on full gas in 3rd and spins. The race
turns on whether it crashes, so 3 seeds are not enough. Over seeds 1 to
9:

| Build | NR02 mean |
|---|---|
| f55db691 | 158.2 s |
| Controller, grip terms multiplied (e0e89cc0) | 165.5 s |
| Controller, grip terms summed | 175.0 s |
| Level 1, band pedals | 165.5 s (10 resets) |
| Level 1, the title's pedals (Q175) | 157.3 s (0 resets, the same on every seed) |

The slip rule's 0.29 beta (139.6 s over 3 seeds) held the car straight.
The title has no such term. The old level made it worse: the nemesis
boost turns on and off, and with it the level threw the Challenger's
grip between x1 and x2. At level 1 the car still loses its time in
traction, not in the plan. On seeds 1 and 3 the plan's own time (the
line over the target speed) is 98 and 92 s, against finishes of 135.2
and 145.1 s. The car is below its target for 81 and 88 % of its time
and on full gas for 83 and 87 %, and TCS acts for 17 and 42 s. The time
goes in power slides at 30 to 35 m/s in 2nd and 3rd (slip angle 0.2 to
0.4 rad), some of which end off the road. A clean run (seed 1) is
135.2 s, +3.4 %; the 9-seed mean is lost to spins and resets. What keeps
the title's Challenger straight is still to find: the G table, the AI's
assists, or the car's own physics.

Needed:

- the title's G table for one car, from a memory read of the controller;
- whether the title's AI drives with TCS and STM;
- every writer of the controller's level field, or a memory read of it
  on the oracle lap;
- a trace of the title's Challenger on NR02 (speed, gas and slip angle).

## Q151 (MAP-01, not blocking): A on the map's event ticket, and a hub's details

X DETAILS on an event marker now draws the event's `H_EVENT_SUMMARY`
ticket over the map with A SET ROUTE, B BACK and Y RESTRICTIONS
(`map_popup_event_details_orig.png`; Q92). The help bar's A reads SET
ROUTE, so the engine sets a route to the event and closes the ticket,
staying on the map. Not recorded: whether the original then closes the map
or stays, and whether A on a ticket can start the event from the map. X on
a street-race hub has no capture; the engine opens its events page at the
hub. The ticket's `PlayerBar` is hidden over the map (the map has its
own).

Needed: a capture after A on an event's ticket, and one of X on a
street-race hub.

## Q152 (HUD-08, HUD-10, HUD-13, not blocking): the notifications' pairings, slide and Show me!

The notifications draw on `947_HUD`'s `CombinedNotification` (and
`PopularityUnlock` for a showcase) with lines from the string tables, but
which title goes with which message is inferred from the tables' names
(`notify_strings()`), the tape's slide in and out is not ported (they
fade over `kNotifyIn` / `kNotifyOut`), and a showcase's Show me! stays
hidden: what its button does (a route to the showcase, the map on it, or
the event's ticket) is not recorded. The co-op tag is the engine's
English.

Needed: captures of each notification kind with its message line, the
tape's slide timeline (the scene's BGF), and a press of Show me!.

## Q153 (HUD-04, HUD-00, not blocking): the route arrow's text, timing and race use

A route's next turn now draws `947_HUD`'s `DirectionArrows` (the
`DIRECTIONARROWBG` box and the turn's `DIRECTION_ARROWS` cell) where
`16_race_hud_orig.png` shows a hairpin in a race, in place of the engine's
turn banner. Not known: what `TEXT_JUNCTION` (horizon_d 13, 56 px under
the box) says (a distance, a road name, or nothing on PC), so it stays
empty; how far ahead of a turn the arrow shows and how it fades in (the
box rests at alpha 0); whether a recalculating route shows anything (the
engine shows nothing); and which corners get an arrow in a race, so the
race's corner arrows are not drawn. The minimap's `Details/TEXT_DISTANCE`
with `SATNAVDEST` (the distance to the destination) is not bound either.

Needed: a free-roam capture with a route set, approaching a junction, and
the rule the race uses to pick its corners.

## Q154 (HUD-15, VIS-15, C11, not blocking): the activation prompt's button

`ActivationPrompt` draws the event's name and SIGN ME UP! (or ENTER NOW)
with an X glyph, as `09_event_marker_prompt_orig.png` and
`30_skill_toast_orig.png` show, while the prompt's own texture is the A
button (`ACTIVATIONZONEICONS/ABUTTON`) and `gamedb.slt`'s
`ControllerLayouts` holds no activation binding. The engine keeps X
because both frames show it. Not known: whether the
captures' controller layout was the default one, so whether the default
is A or X.

Needed: one capture of an activation prompt with the default controller
layout.

## Q155 (INTRO-13, VIS-03, not blocking): what skips a movie

The boot splashes skip on a B tap in the recorded title steps, and the
story movies (`FMV_01`, `FMV_02`, `FMV_04`, `car_challenges_intro`,
`Forza_Tone_Rally`) carry `confirmskip="true"`, with no help bar over the
picture (`00_opening_movie_orig.png`). The engine now skips any movie on a
press of A, B, Enter, Space or Esc (and Start or Tab in the campaign), and
never on a button held from before the movie. What `confirmskip` confirms
is not decoded: a second press, a popup, or nothing on PC. The engine's
in-engine cinematics (the campaign's own scenes, not movies) keep their
0.75 s hold, because Q61 (g) records that the original's in-engine
cutscenes cannot be skipped at all.

Needed: the `confirmskip` handling in the generated movie player, or a
capture of one press on `FMV_01`.

## Q156 (VIS-09, not blocking): the car name tag's look

Measured on one night frame (`30_skill_toast_orig.png`, BISHOP): white
`horizon_b` capitals 18 px high, a dark pill at about 38% black (33, 33,
33, a=96 over the scene) 30 px high with 10 px and 12 px side pads, and a
16 x 6 px notch under its centre. No PI badge or leader star shows. Not
recorded: the tag by day, its fade with distance, whether a leader or a
rival gets a different tag, and the scene or texture it comes from.

Needed: a daytime race capture with several tags at different distances.

## Q159 (INTRO-03, WORLD-11): route 170 and the opening's time-of-day zones

Supersedes, in Q66's engine interims, "the AI drives the sat-nav route
rather than route 170's own path" in part and "the two time-of-day zones
... are not implemented" in full.

**Time-of-day zones (answered 2026-11-25**, from the first-time-career
activity's zone setup and trigger handler in the generated code). Two
20 m spheres: (-116, 107, -3377) asks for 18:00 (64,800 s) and
(2284.42, 109.87, 398.12) for 14:30 (52,200 s). On entering one, and only
while the activity's stage value is 4 or less (up to the festival
arrival), the target gets 86,400 s added if the clock is past it, and the
clock is asked to run there at (target - now) / 45 game seconds per real
second: a 45 s ramp, after which it holds. The engine does the same
(`FirstTimeCareer::kTodZones`, `ftc_tod_request`, `TodClock::request`),
on the zone's entry edge only.

**Route 170** (`media/tracks/colorado/Ribbon_00/TrackRoute170.xml`, the
opening's CLoadRoute) is the race to the festival's route, not the
Viper's: its grid (`start_location_00..07`) is at the diner, its two
checkpoints are near (398, -3269) and the toll booth (-798, -1065), and
its waypoints 00 to 12 run from (1582, -1840) to (-964, 118) inside the
festival. The engine now loads it at the opening and sends the festival
arrival's sat-nav to its last waypoint (`Route::waypoints`,
`FirstTimeCareer::set_route_waypoints`).

Still interim, narrow: while the career holds the controls the engine's
autopilot drives the sat-nav route; the title's race behaviour on the
route (which path the Viper's AI takes from the opening's placement to the
diner) is not read.

## Q160 (INTRO-08, INTRO-09): the cutscenes' cars and people

Opening_Cutscene places `FestivalCar_0` (CarID 1171, a Ferrari 599XX) at
(816.3, 172.43, 1806.2), heading 97 degrees, at 31 s and releases it at
once at 110 mph (`despawnTime` 0); NEW_Diner_Cutscene_1 places two cars
and releases them at 120 mph with a 25 s despawn. The people and animals
are CCutsceneAnimationTrigger clips (`AnimatedObjects\*.pgeo`: the diner's
characters, Radio_Unlock, Wristband_Intro, deer and bird flocks), each
with an `animname`, a duration and a parent node.

The engine reads both triggers (`CampaignDirector::take_cutscene_cars`)
and logs them, but has no scripted car spawn: its traffic and festival AI
have no call to place a given car at a pose and speed and drive it on,
and RaceView is the only other AI-car path. The animation clips need the
`.anim.bin` keys (Q109). Interim: the cutscenes play without those cars
and people.

Needed: the scripted car's behaviour after release (does it follow a
road or drive straight, and what despawns it with `despawnTime` 0), and
Q109's keys for the people.

## Q161 (INTRO-04): the opening's radio states

**Engine reading** (from first_time_career.xml and game_free_roam_flow.xml):

- Under the controller loading screen the free-roam flow runs
  `Audio.disable_radio_dj`, `Audio.disable_radio_station_switching`,
  `Audio.enable_radio_silent_idents` and CSetRadioPlaylist
  (`startWithFirstTrack`, `startAtBeginningOfTrack`,
  `normalBehaviourAfterListPlayed`, R1_Everyday then R1_ThePower).
- Radio_Unlock's flow (FestivalArriveCutscene) turns station switching on
  after Dak's line, then, once the camera sequence ends and the HUD is
  back, the DJ on and the silent idents off. RaceCentralArriveCutscene
  turns all three on again.
- The race to the festival's last stretch stops the DJ (Q62).

The engine applies these (`FtcCommands::set_radio_dj`, `radio_playlist`,
`set_radio_switching`, `set_silent_idents`, `stop_dj`; `Radio::set_playlist`
and friends). A resumed save before the heats gets the opening's states;
from the heats on, the station's own. Out of the first-time career the
host resets them, and the DJ follows the player's DJ option whenever the
flow allows it.

Inferred, not read from code:

- A silent ident keeps the ident's place between tracks (the next track
  starts at the ident's `OffsetStartNextTrack`) and plays nothing.
- CPrepareDinerCutscene's `musicTrack="R1_ThePower"` is played like a
  one-track playlist: from its start, then the station's own picks.
- Immediate DJ lines (CRadioDJImmediate, such as IntroCutscene and E3Intro)
  play while the DJ is disabled.

Needed: the radio's handling of `enable_radio_silent_idents`, the diner's
`musicTrack`, and whether CRadioDJImmediate checks the DJ switch.
## Q162 (PROG-13, answered): the popularity header word against a displayed rank

Supersedes Q60's "Not yet compared with a rank the game displays." Q60's
reading (the first u32 of `SponsorshipState` is the popularity points) now
has one displayed rank behind it.

The original's stats screen (`26_my_profile_stats_orig.png` in the audit's
captures, General tab) shows POPULARITY LEVEL 166th. It was taken on the
recomp seed `appdata-2026-09-27`. That seed's `ForzaProfile` matches the
hash in its `seed.json`. A copy was decrypted with the recomp's
`profile-export.py` and read with the engine's `profile-import`
(`profile::popularity_points_from_sponsorship_state`). The save is the one
on screen: credits 167,700 and 6 garage cars, as the capture shows.

- Header word: 456,480. On Fame.xml that is rank 166 (target 450,682).
  Rank 165 needs 456,840, so the window is 6,158 points wide and the word
  sits 360 points short of the next rank.
- The `Fame` child's first word is 3: the Fame-sponsor tiers at or above
  rank 166 (225, 200, 175), as Q60's rule predicts.

The reader is unchanged. The f32 at header +8 and the zero word stay open
(Q60).

## Q163 (PROG-15, answered in part): `DialoguesState`, the heard-dialogue record

Supersedes Q60's "Also open: `CHorizonDialoguesState` (hashed line keys,
kept as bytes …)" as far as the layout and the key go. Read from the
class's serializer and the dialogue script loader in the recomp's
generated code.

**Layout** (`CHorizonDialoguesState` v1): a u32 entry count, then that many
12-byte entries (key u32, variant u32, count u32), in ascending key order.
In memory the state is an ordered map from key to (count, variant), and
the serializer writes key, variant, count. All three reference saves parse
exactly: 4 + 12n bytes, with n = 126 (`user`), 176 (`user-modded`) and 79
(the `appdata-2026-09-27` seed of Q162).

**Key:** a hash of the `DialogueScript_<LANG>.xml` trigger id. The id is
read from the `Trigger` element's `id` attribute and passed to the game's
string hash with 32 bits and folding on:

    h = 0xFFFFFFFF
    for each byte c of the id, lowercased (as a signed char):
        h = rotl32(h ^ c, 7)

Every key on `user` (126 of 126) and on the seed (79 of 79) is a trigger in
`DialogueScript_EN.xml`. On `user-modded` 170 of 176 are. That save was
written by v4, and the other 6 are probably Horizon Rally triggers
(`Rally_DialogueScript_*.xml` is not in the local game copy), so that part
is unconfirmed.

**Values:** count is the times the trigger fired or played (which one is
not settled; `WristbandReminder` reads 1,107), and the variant is the
`Filename` index to play next. Across the three saves:
- on 304 entries the trigger has one `Filename`, and the variant is 0;
- on the 71 entries whose trigger has several, the variant is count mod
  the `Filename` count 65 times;
- the other 6 are all `HEATSFinalTicketNeedsWin` and `RouteSetOnMap`, and
  there the variant is (count + 1) mod the `Filename` count, one step ahead.

Some entries read count 0: `FirstRaceCentralUnlockedFastTravel` and
`FirstSpeedZoneSight` on all three saves, and `NewWristbandUnlocked3` on
the seed. So an entry can exist without the line having played, and
"heard" is probably count > 0, not the key being present. The variant reading is
inferred from the numbers.

**Still open:**
- where in the dialogue manager an entry is added or bumped, and whether a
  one-shot trigger is skipped on count > 0 or on its own flag;
- why two triggers run one variant ahead.

The engine still seeds its own heard-dialogue keys from the career (Q60).
It does not read this record yet.

## Q164 (INTRO-07, answered in part): the opening titles' timing

Supersedes, for the timing, two of Q65's sentences:
- "Still open: the timing and placement, which are the scene's BGF
  animation (not decoded)";
- "Engine interim: the four cards in turn, 4 s each with half-second
  fades, centred". Only the centring stays.

The timing was read from the bytes of `H_OPENING_TITLES.bgf`
(UI.zip). The engine's BGF reader mis-sorts the scene's tracks as styles,
but the bytes are plain. The document has two animations:
- `Master Slide`, 10,000 ms;
- `PLAY`, 33,000 ms. It has one channel per item (29). Eight of them,
  the card and shadow layers, carry a track, all on the same attribute.

Each track has four keys, in milliseconds. They are read here as time
from the scene opening (ShowOpeningCredits); that PLAY starts as the scene
opens is inferred.

| Layer | Keys (s) |
|---|---|
| `_Microsoft` | 4.500, 5.000, 8.000, 8.500 |
| `_MicrosoftShadow` | 4.740, 5.240, 7.776, 8.276 |
| `_Turn10` | 12.000, 12.500, 15.500, 16.000 |
| `_Turn10Shadow` | 12.246, 12.746, 15.240, 15.740 |
| `_Playground` | 18.621, 19.121, 22.121, 22.633 |
| `_PlaygroundShadow` | 18.853, 19.353, 21.852, 22.352 |
| `_Horizon` | 28.165, 28.657, 31.665, 32.157 |
| `_HorizonShadow` | 28.400, 28.900, 31.400, 31.900 |

So the cards come one at a time. Each has half-second ramps, and the
Horizon logo also leaves, at 32.157 s; it is not held.

The engine uses the card keys (`race::opening_title_alpha`) as an alpha
that rises over the first two keys, holds, and falls over the last two.

Still open:
- The key values. They read 0, except a -1.0 pair on each last key, so the
  animated attribute and its curve are inferred. The ramp shape fits the
  half-second spacing.
- The attribute's name (hash 0x1991C315).
- The cards' placement in the scene. The engine still centres them.
- The shadow layers, which the engine does not draw.
- Whether the cards get their full 32.2 s. The engine keeps the scene open
  from New_Diner_Cutscene_2 until the race to the festival starts, after
  New_Diner_Cutscene_3. If those two cutscenes run shorter than 32.2 s,
  the later cards never show. The cutscenes' length was not checked, and
  the change has not been seen in a run.

Update (2026-11-25): the BGF timeline is decoded (docs/UI_SCENE_ANIMATION.md),
and three of the times above were misread. The curves give Playground's
last key as 22.621 s (not 22.633), and the logo's second and last keys as
28.665 and 32.165 s (not 28.657 and 32.157). The attribute 0x1991C315 is
`opacity` (27-bit hash 0x191C315). The engine now reads the keys from the
scene's `PLAY` slide, with the corrected table as the fallback. Why the
values are 0 is Q186.

## Q167 (UI, not blocking): the pause menu's popups

MESSAGE CENTER offline opens the NO NETWORK ACCESS popup (LiveStrings'
`IDS_Not_Connected_To_Live_PopUp_Title` and `_Body`), and QUIT asks first
on `H_MESSAGE_POPUP` with A EXIT SINGLE PLAYER and B CANCEL over a blurred
frame (`28_quit_prompt_orig.png`). QUIT RACE asks with PauseMenu's
`IDS_QuitRace` and `IDS_QuitRace_Desc_Career`. As interims: the offline
popup's help bar is A OK and QUIT RACE's is A QUIT, B CANCEL (neither
popup is captured with its bar); the blur covers the 3D frame only, so
the pause list behind the popup stays sharp, while the capture blurs it;
and the blur is the post shader's depth-of-field run uniformly at its
widest radius, not the original's backdrop filter. Needed: captures of
the offline popup and the QUIT RACE popup, and the backdrop blur's
radius and whether it takes the UI under the popup.

## Q168 (RACE-09, RACE-13, RACE-25, not blocking): the race cutscenes

The race now plays the flows' cutscenes from gamemodes.zip
(`cutscenes_default_race.xml`, and the track's own
`Tracks/cutscenes_<Tracks.DevName>.xml` where `race_specific="true"`):
`prerace_intro` under the title card, `prerace_looping_01` under the grid
menu, `nemesis_show` under the rival callout, `prerace_countdown` after
START (its `Follow` cut hands the view to the driving camera), then
`postrace_finishline` under the banner, `postrace_transition_01` under the
cash, `_02` under the points and `postrace_rewardcar_transition_01` under
the won car. Cams on a named node take the route's transform of that name
(`start_location_00`; FESTIVAL_FEST_001's intro node lies 13 m from it).
As interims:

- The menu keeps the measured 1.8 s after the card, while most festival
  intros raise their `CCutsceneCountdownTrigger` (the flow's
  `wait_for_cutscene_countdown` before the menu) at 5 s, the first race's
  at 18 s. Whether the card shows partway into the intro, or the capture
  pressed A, is not known.
- The engine's count starts at START, not at `prerace_countdown`'s
  `CCutsceneStartCountdownTrigger` (1.3 s), and its HUD returns on its own
  swoop timing, not at the `CCutsceneCountdownTrigger` (2.75 s).
- The cutscenes' events are not acted on: the audio cues
  (`UIInGame/UI/Cutscenes`), `HideAllCars`, the cutscene cars and people,
  and `PlacePlayerCar` (the car stays where the race left it). Without
  `HideAllCars` the AI cars show under the grid cameras.
- A cam on a node no file or route places (the default
  `postrace_finishline`'s `CameraTargetNode_0`) uses the player car; the
  world streams around the car, not the camera.
- Rivals' `Cockpit_Cam_Activate` and the street and star flows' own
  orders are not wired; they use the festival order.

Needed: a capture timing the card, the menu and the intro's cuts from the
load; the count's start against `CCutsceneStartCountdownTrigger`; and the
default `CameraTargetNode_0`'s source.

## Q169 (MENU-04, VIS-12, not blocking): the pause menu's SETTINGS row and X EXTRAS

Offline the original's pause menu has seven rows. The engine shows MAP,
SETTINGS, PHOTO MODE, MESSAGE CENTER, SPONSOR CHALLENGES, CAREER and
QUIT, which matches the recomp's captures (`07_pause_menu_orig.png`,
`28_quit_prompt_orig.png`: SETTINGS in the second row). The disc's
`pause.xml` also has a MULTIPLAYER exit (`onMultiplayer`), which the
recomp's row may stand in for. The engine's own screens (events, garage, upgrades,
paint, difficulty, restart, ...) sit behind X EXTRAS in the help bar,
which the original does not have. Kept as interims: the captured row
order, and X EXTRAS as an engine addition outside the original's rows.
Needed, an owner's call: whether that slot shows SETTINGS (as the recomp
does) or the disc's row, and whether X EXTRAS stays.

## Q170 (VIS-25, not blocking): the Sponsor Challenges screen

SPONSOR CHALLENGES opens `H_SPONSOR_CHALLENGE_STATUS` with the scene's
texts, logos, bars and layout. As interims its panes are placed at rects
measured on the capture (the scene rests at its intro start), and the
original's BUY POPULARITY SPIKE prompt is not shown: the engine has no
tokens or store. Needed: the scene's intro end state, and whether BUY
POPULARITY SPIKE shows offline.

## Q171 (RACE-22, not blocking): the head-to-head's screens

A free-roam head-to-head now shows the original's screens
(`race_encounters.xml`): `H_RACE_ENCOUNTER` as the challenge starts, for
`RandomEncounter_Intro`'s 3.5 s (the rival's name, the car's make, its
name and class, REWARD and the reward on pink), and on a win
`H_POST_RACE_FINISHED` (1ST) then `H_POST_RACE_CREDITS_BREAKDOWN` with the
payout; a loss shows nothing, as the flow does. As interims: no capture
of either screen exists, so the intro draws at the scene's own rects with
its tapes cut to their texts and its reward lines (alpha 0 at rest)
shown; the win's banner has A CONTINUE alone; the flow's cutscenes
(`RandomEncounter_Intro`, the win's `Cockpit_Cam_Activate`) do not play
and the player keeps control; and the engine's own head-to-head meter,
the GPS turn and the stunt meters hide while the screens show (the speed
dial stays through the intro and hides on the banner). The rival's driving and pace stay the AI branch's
(WORLD-07). Needed: captures of a head-to-head's intro and of a win's
banner and earnings.

## Q172 (RACE, not blocking): the fade on QUIT RACE

QUIT RACE, from the grid menu or from the pause menu (A EXIT on its
popup), now fades to black and holds it before the load
(`race_flow::post_steps` with `quit`: `kBlackOut`, then `kDone`), as the
race's other exits do. Before, the session ended on the press and the
load cut in. In the flow files neither quit path names a black-out. The
grid's QUIT goes from `prerace_menu` to `load_back_to_free_roam`. A quit
from the pause menu runs `postrace_prepare` (`show_empty_screen`), then
`should_skip_postrace` (`CIsRaceQuit`), then `postrace_cleanup`, then
`load_back_to_free_roam`. `global.black_out` sits only on the finished
race's `postrace_rivals` quit. So the fade is an interim until we know
what `CLoadIntoFreeRoam` shows when it starts. Needed: a capture of a
quit from the grid and from the pause menu, frame by frame, up to the
loading screen.

## Q173 (RACE, not blocking): the count at a start without a gantry

A start without a start line (no gantry: the street races, `gate 0` in
`race-census`) now counts 3, 2, 1 on `947_HUD`'s `I_321_Go/TEXT_COUNT`
for the race clock's last three seconds, then shows GO! as usual. Before,
the chase camera waited about three seconds with nothing on screen.
Festival starts still leave `TEXT_COUNT` empty, as their captures do
(Q74). The interim rests on two things. The scene lays out `TEXT_COUNT`
at full opacity, but no gantry start fills it. ui4audio has a street
start cue (`321MomentStartStreet`) and a street go cue (`321GoStreet`),
but no street count cue. Needed: a capture of a street race's start, or
the code that writes `TEXT_COUNT` (Q74).

## Q174 (RACE, not blocking): POINTS AVAILABLE on a ticket that pays none

The event list's payout line now shows the wristband points a festival
win pays (`WristbandTables::points_for(level, 1)`). That is the ticket's
POINTS AVAILABLE (`10_event_card_prerace_orig.png`, 110 PTS). Only
festival events pay wristband points (`race::award`, style 1), so the
list shows none for the other kinds. The ticket (`event_ticket`) still
fills POINTS AVAILABLE from the same table for every kind, so a street
race's or a showcase's ticket can name points the race does not pay. No
capture shows such a ticket. Needed: a capture of a street race's,
showcase's or rivals ticket, to see whether its POINTS AVAILABLE tape
shows, and with what.

## Q175 (WORLD-02/03, not blocking): the title's pedals, and why they are opt-in

The title's pedal rules under the line controller are decoded and in
the engine (`drive::ai_pedals`, `AiField::Options::title_pedals`), but
the race AI keeps our band rule unless `SHIFT_AI_PEDALS=title`. The
decoded rules, per update (the title's 60 Hz factors are raised to
60 dt at our step):

- The target is the plan 0.67 of a line point past the car's, between
  the two points around it, capped by the passing search's cap and by
  0.2 (0.1) of the plan's top speed with the heading 84 (120) degrees
  off the line.
- While steering back, the target is cut by k. Off the road, k runs
  from 0.02 at |e| 0.25 m to (-1 - edge if edge < -1, else 0) + 0.12 at
  3 m. On the road, past 2.5 m with |chi| over 0.25 and the front slip
  metric over 1.5, k runs from 0 to 0.1 at 10 m.
- Five states on err = target - speed: stop (target 0), hold (|err| at
  most 1: the hold throttle at 1 + err), full gas (held while err is
  over 0.25), edge avoidance (the last gas less 0.016 x the rate of
  |e|), and brake (held while err is below 0: 0.6 x the plan's friction
  share at the next point).
- Full gas off the road with the front slip metric at 1 or more is the
  hold throttle, 0 from 1.05. The gas is at most 0.6 turned 84 degrees
  off the line, and clamp(1.05 - the mean slip metric, 0.2, 0.9) past
  |chi| 1 with that mean over 0.25.
- The brake is filtered: 0.75 old + 0.25 new rising, 0.5 and 0.5
  falling.
- The hold throttle is a car method: where the drag power at the
  forward speed falls between the engine's power at no gas and at full
  gas, at the current rpm.
- The slip metrics: the mean of the four wheels' metrics (0.25 for a
  wheel with none) and the larger front one (0.25 if both are 0), each
  smoothed by 0.2 old.

Interim (ours):

- The per-wheel slip metric. The title's is a value from its tyre model
  that was not identified. Ours is hypot(slip ratio, slip angle) over
  the curves' peaks, so 1 at the peak.
- Off the road is past the line's room to the road edge on the car's
  side, and edge is that room less |cross-track| in metres. The title's
  test and the unit of its edge value were not found. With edge in
  metres, k reaches 1 (a target of 0) 2 m past the edge, so ours clamps
  k to 1.
- The hold throttle's drag is our aero (drag, high-speed scale, draft,
  damage), without the surface's drag scale.

Why opt-in: on our handling, the title's pedals lose the festival race
and the band rule loses NR02. 9 seeds each, the mean of the AI's times
and the resets over the 9 races, with the oracle in brackets:

| Event (oracle) | Band | Title |
|---|---|---|
| FR22 (161.5) | 166.4 s, 6 | 162.0 s, 4 |
| EXHIBITION_FEST_003 (179.0) | 182.6 s, 3 | 204.5 s, 12 |
| NR02 (130.7) | 165.5 s, 10 | 157.3 s, 0 |

Under the title's pedals, 7 of the festival race's 12 resets are Minis
stuck in the pack between 2.8 and 3.3 km, and contacts run to 5,721 a
race (band: 654 at most). The road-edge cut is not the cause: without
it, over 3 seeds, the festival race is 196.3 s with 4 resets and NR02
168.8 s with 5. The band rule's NR02 resets are the Challenger
power-sliding out at 1.8 and 2.25 km. Needed: the title's per-wheel
slip metric, its off-road test and edge unit, and a trace of a title
AI's pedals through a pack.

## Q176 (WORLD-02/03, not blocking): the AI cheat levels and the heavy-braking term

AISkills' `SoloLapping_NominalCheatLevel` and
`SoloLapping_TorqueBoostCheatLevel` are now loaded (`race::SkillRow`).
The decode shows the line controller storing n and 0.1 n beside a grip
level of 1 + 0.025 n and a braking level of 1 + 0.05 n, so the nominal
level n sets the steering-saturation term's level
(`AiField::set_grip_level`, Q125). Both columns are 0 on every row of
the disc, so every driver's level is 1 in the engine. That is an
interim: the title's oracle lap argues for a level at or below the
threshold there (Q125). For n above 0 the title also
turns on a heavy-braking term, which was not decoded. The engine leaves
that term at 0 and prints a note if a row ever has a level. What the
torque-boost cheat level does was not found. Needed: the heavy-braking
term, and the reader of `SoloLapping_TorqueBoostCheatLevel`.

## Q177 (WORLD-02/03, not blocking): the operands of the 0.93 skill cap

The decode found a rule in the driver's set-up: four of its skill values
(with no drivatar profile, 1, 1, 0.95 and 0.95) are capped at 0.93 when
100 x a x b reaches 1183, where a and b are two quantities read from the
car. They were not identified. Read as the mass in kg and the
centre-of-mass height in m, every car passes the test 12 to 67 times
over, which cannot be the intent. The engine's interim is mass x
centre-of-mass height (kg x m, above the body frame's ground) >= 1183,
applied to the planner's C and B before the boosts
(`race::ai_skill_capped`). That caps the Range Rover (1739) and the
Grand Cherokees (1491 and 1553); every other car in the gate events is
at 944 or below. The drivers' C in these events is 0.6 to 0.9, so the
cap rarely binds. Needed: the two car quantities the test reads, and
their units.

## Q178 (WORLD-07/08, not blocking): the free-roam racers on the race AI

The festival racers and the head-to-head rival still drive by the
kinematic model in `freeroam::FestivalManager`, not by the race AI with
the `freeroam` AIRacing set. A head-to-head now starts (its route is
built from the world's nav). Moving a racer onto the race AI needs
pieces free roam does not have:

- a physics car per racer: a baked body, wheels on the world's ground,
  and its draw through the race cars' path;
- a `drive::RacingLine` and planner built from the head-to-head route;
- the hand-over between the kinematic roamer and the physics car as a
  challenge starts and ends.

That is not a reuse of `RaceView`, so it is left for its own task. The
festival group spawns (Q124) were not decoded either. Needed: the
free-roam racers' spawn and hand-over code (how the title swaps a
roaming racer for a driven one), and the group spawn table.

## Q179 (WORLD, not blocking): the AI's contact with the smashables

The intact props and solid barriers meet the player's car only
(`world::SmashableGround` in the player's step). The race AI's cars and
the traffic pass through them, and only the player breaks props. Giving
the AI the same ground is a few lines. But `SmashableWorld::step`, which
breaks props and steps the debris, takes one car at a time and steps the
debris on each call, so the AI's breaks need that split first. Needed:
whether the title's AI and traffic break props at all, and if so, how
far from the player.

## Q183 (UI, not blocking): the slide flags and the time policy

Each BGF slide has a flags byte (docs/UI_SCENE_ANIMATION.md). 82F2B230
packs it and 82F2B408 hands it to the time manager: +0x38 gets the
duration, `flags & 3`, bit 3 and bit 4; +0x3C gets bit 2. The routine that
reads +0x38 was not found, so the engine reads the bits from the corpus:
- `flags & 3`: 0 loops, 1 plays once. 9 (play once + bit 3) is on 19,707
  slides.
- bit 2: starts paused. These are the slides the game scrubs, such as the
  progress bars and the pause menu's `SIZE` steps.
- bit 4: plays through to the next slide (`Focus` 17 into `Focused`).
- bit 3 is unknown. It is on nearly every play-once slide, and also on
  `CombinedNotification`'s `ON` / `OFF` (13).

The engine also assumes a slide's time starts at its `start` and that a
play-once slide holds its last frame.

Needed: the time manager's reader of +0x38 and +0x3C, or what bit 3
changes.

## Q184 (UI, not blocking): which elements a slide shows

The engine's rule for an element being on is inferred:
- A slide that lists it with `active` 0 hides it.
- A component's slide that omits it hides it, if another of the
  component's slides lists it.
- `starttime` / `endtime` bound it within the slide.
- The Master Slide's `active` bytes are ignored. In the Master Slides
  checked (`925_PAUSE_MENU`'s rows, `947_HUD`'s `State`), every element
  reads 0; taken literally, that would hide everything.

Values set by one slide persist into the next unless that slide sets them
again (the Master Slide's values are applied on every entry). A curve's
last value is baked when its slide is left. Both rules fit the front end
and the HUD notification, but neither has been checked against the title's
slide enter path, which was not located.

Needed: the enter path's handling of the `active` byte and of elements a
slide does not list.

## Q185 (FE-02, BOOT-02, BOOT-04, not blocking): the front end against its capture

The front end now plays `120_P_PRE_SPLASH`'s slides. Three things are
still fitted rather than read:
- The selected row in `02_title_menu_orig.png` is 1.031 times its rest
  size (text 865..1139 against 870..1136). The `Focused` slide's Group
  rests at 0.7 and pulses to 0.722 (x1.031) at 1.04..1.27 s of its 2.5 s
  loop, so the capture was taken during a pulse. Without a timeline the
  engine keeps the 1.03 step; with one it plays the pulse.
- The rows' x offsets (selected 8 px, unselected 28 px) are not in any
  slide. They come from the capture.
- The input timing. Press Start plays `Menu_Intro`, where the menu
  appears at 0.493 s. The engine takes that as the profile step's length
  (`kProfileSeconds`), but the original may wait on its profile read
  instead. The scene's `START_OUTRO` (the logo out over 0.299 s) is not
  used, because `Menu_Intro` already contains it.

Needed: a capture series of the menu (to see the pulse), the code that
places the rows, and which of `START_OUTRO` / `MENU_INTRO` the original
sends on Press Start.

## Q186 (INTRO-07, not blocking): the opening titles' zero-valued curves

`H_OPENING_TITLES`' `PLAY` slide (33 s) gives each card and shadow layer
one `opacity` curve with four keys (Q164). All the values are 0, and the
slide sets each layer's opacity to 0. Read literally, the cards never
show. The keys' controls are 0 too, except c1 = c2 = -1.0 on each last
key. The engine reads the key times from the scene and ramps the alpha
0 → 1 → 1 → 0 over them, as before.

Possible readings: the game's code sets the card values (no action in the
scene does), or the curves were baked from an authoring tool with values
the export lost.

WP6's table (parity/wp6) misread three times, now corrected:
- Playground's last key: 22.621 s, not 22.633.
- The logo's second key: 28.665 s, not 28.657.
- The logo's last key: 32.165 s, not 32.157.

Needed: a capture of the opening titles, or the code that drives
`H_OPENING_TITLES`.

## Q187 (MENU-17, MAP-05, ACT-01, ACT-02, not blocking): the scenes decoded but not fully played

**`925_PAUSE_MENU` (MENU-17)** now plays its rows' slides, but not all of
the scene is used:
- `FOCUSED` scales the focused row to 1.2 (measured: 1.21).
- `BLURREDABOVE` / `BLURREDBELOW` rest the rows above the focus 7 px up
  and those below 6 px down. Both captures confirm the spread:
  `07_pause_menu_orig.png` (MAP focused) and
  `07b_pause_menu_scrolled_orig.png` (QUIT focused) put SETTINGS .. MY
  PROFILE 12 to 13 px apart. The engine's rows now land within 1 px of
  both. They also land within 1 px of `29_message_center_offline_orig.png`
  (SETTINGS focused, rows above and below), a capture the list's rest was
  not fitted to.
- Not from the scene:
  - The list's rest is measured: `kListDy` plus 7 px up, since the old
    measurement had the spread folded in.
  - The list's own slide (`Option_List`'s `Slide1`, 0..6000 ms paused,
    scrubbed by the game's `SIZE` GotoTime actions) is not sent. Which
    `SIZE` the game sends for a list's length is not known.
  - The moves (`FOCUSABOVE` .. `BLURBELOW`, 150 ms) are scrubbed by the
    measured 0.1 s cross-fade.
- The root's `INTRO` / `OUTRO` (300 ms) change nothing visible.

Still not played:
- **`H_MAP` (MAP-05).** Besides the stackers, the only slides are the
  event hubs' (`AVAILABLE` / `UNAVAILABLE` / `BRONZE` / `SILVER` / `GOLD`)
  and each activity's (`Upright` / `UprightWarning`). The engine's map does
  not draw those icons from the scene, and the nodes it does draw have no
  slides. The audit's MAP-05 (a perspective map, its zoom, and the icons'
  `reveal_drop` / `bulge` / `grow` animations) and HUD-11 (the rumour's
  map reveal) are `MapProfileFullscreen.xml` profile animations. Those are
  not BGF timelines, and this work does not cover them. What this work
  did under HUD-11 is `947_HUD`'s notification panel (`SHOW` / `HIDE`).
- **The activity HUDs (ACT-01, ACT-02).** `947_HUD` has the slides:
  - `SpeedCamera`: `OFF` / `SHOW` / `ON` / `OLDPB`, and more;
  - its `Icon`: `SPEEDCAM` / `AVERAGESPEEDCAM`;
  - `MissionsDamage`: `BARFILL`;
  - `ScoreGroup`: `SHOW` / `HIDE` / `ABORT`.

  The engine draws its own speed trap and mission HUDs, not these
  components (ACT-01 and ACT-02 themselves), so there is nothing to play
  them on yet.

Also open: the time a contract event is sent relative to the frame it
takes effect on (the engine fires and advances in the same frame).

Needed:
- pause menu captures taken mid-move;
- the `SIZE` event the game sends for each list length;
- the map's icon layer and the activity popups drawn from their scenes.

## Q188 (UI, not blocking): the contracts' `initial_state`

`UiContract::initial_state` reads item property 0x083BF5F8. Decoded, that
is `endtime` (27-bit 0x3BF5F8) with an int tag, so `083bf5f8 000007d0` is
2000 ms, not a state name. The engine puts each component on its first
slide after the Master Slide, and screens choose their own starting
slides. What the title starts each component on is not confirmed.

Needed: the scene loader's first-slide choice, and a rename of
`initial_state` once that is known.

## Q199 (FEEL-16/19/20, not blocking): PhysicsSettings' built-in values

The executable's PhysicsSettings constructor holds a built-in value for
every key. The shipped PhysicsSettings.ini overrides some of them. Keys
the ini leaves out keep the built-in value, which is not in any game
file. The ones this work package reads:

| Key | Built in | Shipped ini |
|---|---|---|
| NormRearSlipAngleToCountersteer | 3.0 | 50 |
| SteerDefCountersteerRate | 160 | 105 |
| SteerDefDynSteerFindPeakRate | 10 | 105 |
| SteerFrictionClamp | 1.2 | (not set) |
| CarCollSphereMinDistAboveGround | 0.08 | 0.11 |
| CarGroundCollSphereMinDistAboveGround | 0.45 | 0.4 |
| CarBoundingBoxMin | 0 | (not set) |
| AmountIntoBumpstopsToCauseDamage | 1e-4 | (not set) |
| TimeInBumpstops | 0.25 | (not set) |
| BumpstopDamagePerSec | 0.02 | (not set) |
| BumpstopFrictionScale | 0.9 | (not set) |
| NumUpdatesToSkipSurfaceBumps | 1 | (not set) |

The TireSpringiness block and the four WheelMapping tables are built-in
only (Q101, Q70).

By their names, the bump-stop damage keys mean that a wheel held more
than 1e-4 m into its bump stop for 0.25 s takes 0.02 damage per second,
and BumpstopFrictionScale scales grip in the bump stop by 0.9. Neither
is in the engine, and the consumers are not decoded.

Needed: the consumers of the bump-stop damage keys and
BumpstopFrictionScale.

## Q191 (WORLD-GEOMETRY Q3 follow-up, not blocking): the far terrain over the Finley Dam gorge

`Mountains_Area04A_Terrain_05_LOD00..02` (coloradoout.12897–12899,
`H_BLND1_SPEC`, in the UberLOD block next to `TERR_UberLOD_Patch10_CAGE003`)
duplicates the Finley Dam terrain to within about ±2 m. Below the dam road
(x 6527..6793, z -470..-113) it is the only geometry over the gorge.

Open:
- How does the game draw it near the camera without covering the road? A
  PVS or zone exclusion, a separate far-terrain depth pass, or something
  else?
- What draws the gorge floor in a frame from the Cliff Run route near
  (6577, 231, -456)?

The engine meanwhile:
- Objects drawn only with `H_BLND1_SPEC` are UberLOD-level far terrain.
- Inside the inner cut, far triangles fill only the holes in the near
  ground.
- Six thin slivers at hole edges remain.

Needed: a capture from the Cliff Run route above the gorge, or the
renderer's far-terrain pass.

## Q192 (MAP, not blocking): X on the map at the festival site you are at

On the map, a festival site offers X FAST TRAVEL (0 Cr)
(`map_popup_garage_orig.png`, `map_popup_paintshop_orig.png`). No capture
shows the map opened while the player stands in that site's trigger.

The engine meanwhile: on the site the player is at, X reads ENTER NOW
(`Activities/IDS_EnterNow`) and enters the site, as the world prompt does.
Elsewhere X still offers the fast travel.

Needed: a capture of the map opened inside a festival site's trigger,
with the cursor on that site.

## Q193 (MAP, HUD-11, not blocking): which barn states show which map icon

`CActivityBarnFind` sets up two map markers at the barn: `barnfind`
(MapIconSheet cell 0,4, tan) and `barnfind_collected` (cell 1,4, dark).
Its marker update shows the tan one when asked to, and shows the dark one
in its place when the activity's byte +44 or byte +513 is non-zero. The
original's map shows tan barns inside the rumour circles
(`map_open_orig.png`, `08_world_map_orig.png`).

The engine meanwhile:
- A barn with a running rumour gets the tan icon.
- A found barn (restoring, ready or collected) gets the dark icon, the
  sheet's colour with no tint, as both profiles give it.
- A selection fades the other icons to the profile's `ghost` alpha (0.3).

Open:
- Which fields bytes +44 and +513 are.
- Whether a restoring or ready barn keeps the tan icon until it is
  collected.

Needed: the writers of those two bytes, or a map capture with a found but
uncollected barn.

## Q211 (V4-11, not blocking): the Rally hub's guesses and interims

V4-11 is settled against Q149's reading: the Rally's "free roam" is not
a drive. `game_rally_free_roam_flow` removes the car, shows BgLoop.wmv
and `CShowUIScreen_RallyHub` on `H_RALLY_HUB`, and its exits are
`okCareer` (a ticket), `okRivals`, `selectCar`, `purchaseCar`, `outpost`
and `return` (the QUIT HORIZON RALLY popup, which unloads to the title
through `CUnloadToPreGame`). The engine now draws that hub (UI_SCENE_SCREENS
"The Rally hub"). Guessed or kept as the engine's:

- The buttons. The scene's button bar is filled at run time; the engine
  shows A ENTER EVENT, B BACK, Y TRACK LAYOUT (the bgf's Restrictions
  reverse) and BACK EXTRAS. No X RIVALS: the Rally rivals entry
  (`okRivals`) is not built (Q147's V4-12).
- The tickets: only the unlocked championships, in the
  `MultiStageSeriesEvents` order (tickets 005, 004, 003, 001, 006, 007,
  002). Whether the original also lists locked
  tickets, and its order, is unmeasured.
- The ticket's texts: WINNER TAKES and the prize, the class badge, NEW
  while the event has never been finished, then its best place. BEST
  TIME (`TEXT_RACE_TIME`) and the points tape are hidden; the original
  fills them from data the engine does not keep per championship.
- ENTRY RESTRICTIONS: the `RestrictionDescription` ids (`_&2027516xx`,
  table 3093) resolve in no loaded string table, so the reverse shows
  "Car Class must be <class> or below." (CareerEventUI 0x7013).
- B's popup reads "return to the main menu" as in the original, but OK
  goes back to Colorado, not to the title.
- The engine's EXTRAS (BACK): replay the heats, fit the rally kit in one
  press (the original converts a car through the car select's RALLY
  CONVERSION popup), drive ColoradoDirt, and return to Colorado without
  the popup. None of them is the original's.
- The car select (`selectCar`), the purchase path (`purchaseCar`), the
  hub's radio and the ticket ColorScheme tints are not built.
- The backdrop is black in the live window. BgLoop.wmv (in the Rally
  package, 1280x720 wmv3) opens, loops and decodes real frames, and
  `atlas_update("fmv.frame")` succeeds, but the frame's atlas texels
  never show on screen: a plain image of that atlas entry, drawn last,
  is black too, at 1280x720 and at 640x360. So the atlas entry reads
  black on the GPU; the hub's scene does not cover it. Not diagnosed
  further.

Needed: the hub's button bar contents, whether locked tickets show, what
fills BEST TIME and the points tape, and the restriction strings' table.

## Q212 (V4-14, RACE-23, not blocking): the Rally final's screens

After a championship's last stage the engine now runs
`game_rally_race_flow`'s end on the Rally's own scenes
(`rally_post_steps`, drawn by `game_hud_core.cpp`):

1. `H_POST_RACE_RALLY_FINISHED`: FINISHED and the place, as the festival
   banner. A CONTINUE; X RACE RESULTS shows the engine's standings page
   (B or A closes it).
2. `H_POST_RACE_RALLY_POINTS`: POINTS EARNED, +N Rally XP and the Rally
   level bar with the Rally wristbands' art. `TEXT_POINTS_TO_NEXT` is set
   to PostRaceFlow's "{0} POINTS TO RALLY FINAL" / "{0} POINT TO RALLY
   FINAL" / "WIN TO BE CHAMPION!" (0xC2FD, 0x481D, 0xC923) against the
   final's `UnlockPointsReq` (none once champion), but it does not show:
   at the scene's rest it sits under the bar, as the career line does
   (`17c_results_points_engine_wp2.png` shows only its last digits).
3. Once, `H_HORIZON_RALLY_CHAMPION`, with `HorizonRallyChampion` (moved
   from the finish to this screen). A returns to the hub.

The intro heats keep the engine's results page (`game_rally_intro_flow`
ends differently). `club_rally_corpus_tests` renders the three (the
captures folder's `rally_{finished,points,champion}_offline_wp5.png`) and
checks the lines and the steps. `carview --demo-rally-final` opens the
sequence after a won first championship; the live captures are
`rally_{finished,finished_results,points,champion,final_after}_live_wp5.png`.

Interims: no earnings screen (the credits are on X's page); the prize-car
step is not in the sequence; `Rally_Champion_Cutscene` is not played, so
black is behind the champion screen; its texts are guesses, the title
CareerEventUI's "HORIZON RALLY CHAMPION 2012" (0x2DD1) and the place on
the position tape; the banner's help bar is guessed (no RESTART or
REPLAY); the points line is hidden under the bar (above; Q96's scene
rest); the bar's span starts at 0 XP; `rally_end_seen` is saved at the finish,
before the champion screen shows. Between stages the engine's STAGE
RESULTS page stays (Q147). `race_flow::post_steps` is unchanged: the
sequence runs on the results state.

Needed: the final's help bars, the champion screen's texts and timing
against the cutscene, and whether the earnings come before the points.

## Q221 (MENU-03, not blocking): which workshop part plays which camera

The disc has two sources for the upgrade shop's camera, and neither says
which part uses it:

- `WorkshopCams.xml` has seven looping 10 s sequences:
  `Upgrade_Part_FRONT`, `HOOD`, `REAR`, `WING`, `SIDE`, `FRONTWHEEL` and
  `REARWHEEL`. Each is placed on one of the car's parts (`headlightR`,
  `headlightL`, `taillightL`, `taillightR`, `wheelLF`, `wheelRF`,
  `wheelRR`).
- `UICarTargetCameraPresets.xml` has fixed presets: `Upgrades_Main`,
  `FrontBumper`, `RearBumper`, `RearWing`, `Hood`, `WheelStyle`,
  `Flywheel`, `Engine_Front`, `Engine_Rear` and `Drivetrain`.

The engine plays the sequences for the parts they name, and the presets
for the parts that have no sequence:

- **Sequences (`workshop_part_sequence`):**
  - front bumper → FRONT;
  - rear bumper and exhaust → REAR;
  - wing → WING;
  - hood → HOOD;
  - side skirts → SIDE;
  - tyres, rims, brakes and suspension → FRONTWHEEL.
- **Presets (`workshop_part_preset`):**
  - the engine's parts → `Upgrades_Engine_Front` (every engine taken as
    front-mounted);
  - the flywheel → `Upgrades_Flywheel`;
  - the drivetrain → `Upgrades_Drivetrain`.

REARWHEEL is not used. The recomp's source does not name either file's
entries; the names are in the game's data.

The engine's part positions are estimates (`car_parts`: headlights at the
front overhang, 0.7 m up). With them, HOOD's shot looks down at the floor
ahead of the car, with the car at the top left of the frame. That may be
the original's framing, or a wrong part position or `PartSpace` reading.
No capture shows the upgrade shop.

Needed:
- the title's part-to-camera table, and whether the sequences play only
  after a part is fitted;
- the engine's placement for `Upgrades_Engine_Rear`;
- the parts' real positions (the car model's light and wheel bones);
- a capture of the upgrade shop.

## Q222 (MENU-02, not blocking): the hub rooms' interior shaders

Partly answered (WP3b). The rooms' surfaces use
`H_INT_DIFF_SPEC_REFL_NORM_AO_LM`, `H_INT_DIFF_SPEC_REFL_AO_LM`,
`H_INT_ROAD_*`, `H_INT_DIFF_EMISSIVE_2` and a `HS_SKY_DIFF_HDRI_SHAD_1`
dome. Their TrackSettings have no sun and keep fog off. The decoded pixel
shaders give the light:

- `H_INT_*_AO_LM`: albedo x c0.z x (LM.rgb + (AO.g + 0.05) x ambColor),
  plus the reflection and specular, output as sqrt. The lightmap is read
  at the second UV (TEXCOORD1.zw); no scale constant multiplies it.
- `H_INT_ROAD`: blend x (hemisphere x (AO + 0.05) + LM), the 0.05 in
  c253.y.
- `H_INT_DIFF_EMISSIVE_2`: albedo x c0.z x ambColor + Emissive x c0.z.

ambColor is not `LightAmbient` (0.04, which leaves the room 17 times too
dark). It is image-based: `iblAmbientScale` x the mean of the room's
`staticcarcubemap.xpr` (LogLuv), at `iblAmbientSaturation`. One factor of
2 more is fitted on capture 20, so the engine takes
`ambient = iblAmbientScale x 2 x mean(cube)`. Cube means: paint booth
0.174, workshop (0.18, 0.198, 0.205), Autoshow (0.189, 0.175, 0.253);
`iblAmbientScale` 4.8, 2.3 and 2.0.

The rooms' post chain is their own TrackSettings: `filmicTone` (white
1.5, Hable 0.151, 0.038, 0.566, 0.21, 0.003, 0.141), a fixed exposure of
2^(0.75 + ExposureBias), `DesaturateAmount` 0.92 at `DesatLuminosity`, and
no LUT, vignette, contrast, motion blur, god rays, lens flare or depth of
field.

The engine (`world_frag.glsl`'s interior branch, `hub_room.cpp`,
`apply_hub_room_post`), measured on `20_paint_shop_wp3b.png` against
`20_paint_shop_orig.png` (same regions, the two cameras close but not
equal):

| Region (x0,y0,x1,y1) | Engine | Original |
|---|---|---|
| back wall (600,30,700,90) | 234 | 232 |
| left wall (20,180,90,240) | 178 | 218 |
| floor, front left (20,620,120,700) | 167 | 150 |
| floor, right (1000,560,1200,640) | 159 | 140 |

The back wall sits on the tone curve's shoulder and barely constrains the
factor. With 1.5 instead of 2 the floors match (152 and 149) but the back
wall drops to 219 and the left wall to 157. No single factor fits every
surface, so the light is still an interim. A material whose lightmap record is a stub
(the paint booth's door glass) takes its AO map as the lightmap and stays
interior; before, it took Colorado's light and drew blue.

Still open:

- The factor of 2 is fitted, not decoded. It may be the cube's mip or
  spherical-harmonic weighting, or the shader's `ambColor` constant.
- The car's light is an interim (Q261).
- The paint booth's door glass draws mid-grey, the original's near-black.

## Q223 (MENU-02, MENU-08, not blocking): the Autoshow's crowd and DJ

`19_autoshow_orig.png` shows a crowd, a DJ at a desk and moving lights
on the Autoshow's stage. The `uiautoshow` track has the stage, speaker
stacks, banners and light strips but no characters: its `GameObjs.xml`
is empty. The engine draws the room without them.

Needed: where the title spawns the Autoshow's crowd and DJ (a homespace
script, `075_SHOWROOM_HOMESPACE`'s scene, or the spectator system) and
their animations.

## Q224 (MENU-06, MENU-18, not blocking): the world behind the option and stats pages

Partly answered (WP3b). Captures 23 to 26 show the world defocused behind
the OPTIONS pages (CONTROLS, DIFFICULTY, HUD) and MY PROFILE's STATS.
Captures 22 and 31 show it sharp behind the lists (OPTIONS and PHOTO
MODE). The engine now blurs the frame behind those four pages (and AUDIO,
which has no capture) with the pause popups' backdrop blur
(`DriveSession::page_backdrop`;
`24_options_difficulty_wp3b.png`, `26_my_profile_stats_wp3b.png`).

The prebuilt `post.frag.spv` had not been rebuilt since the backdrop flag
went in, so before WP3b neither these pages nor the pause popups blurred
in a built engine.

Still open: the blur is the engine's eight-tap gather (about 5 px at
720p), softer and grainier than the original's; which screens turn it on
in the title (a scene flag or the screen's code); its strength; and its
fade in and out.

## Q261 (MENU-02, VIS-20, not blocking): the car's light in the hub rooms

The rooms' surfaces are decoded (Q222); the car in them is not. The
engine lights it with:

- a key light along the room's `ShadowLightDirection`, coloured
  iblSunColor x iblDirectScale x iblDirectScaleCar;
- the mean of the room's `staticcarcubemap.xpr` (about 0.17 to 0.2) as
  its ambient, above and below;
- the engine's reflections, not the room's `staticcarcubemap.xpr`;
- no shadow on the room's floor, which the original casts under the car
  (`20_paint_shop_orig.png`).

Two other ambients were tried. The image-based one that lights the room
(about 1.7 in the paint booth) turned the tyres light grey, against the
original's near-black (`20_paint_shop_orig.png`). The rooms' hemisphere
pair is zero in the workshop and left the car's sides black there. A white
car in the Autoshow still reads close to clipping (`19_autoshow_wp3b.png`);
no capture shows the same car in a room to compare.

Needed: the car shader's room terms (the static cube's reflection and its
ambient, and whether `iblAmbientScale` reaches the car), and a capture of
one car in the engine and the original in the same room.

## Q262 (VIS-20, VIS-22, not blocking): the scene behind MY GARAGE

`11_car_select_orig.png` shows 108's garage list (an event's CAR SELECT)
over the world it was opened from, with no stage of its own. The engine
now keeps the workshop room behind MY GARAGE when the workshop's CHANGE
CAR opens it (`11_my_garage_workshop_wp3b.png`). From the pause menu and
the outposts it keeps the engine's studio stage (interim).

No capture shows MY GARAGE itself.

Needed: captures of MY GARAGE from the pause menu, from an outpost and
from the workshop, and the camera the original uses on the current car
there.
