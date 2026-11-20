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
- **Props in the reference frame.** The gather places every `.pgeo` and
  `.pvsz` prop near the camera, including event props the frame did not
  draw (a gantry, grandstands, banners, a crate on the road). The world-only
  render (`--no-props`) matches the frame's road, markings, shoulder and
  guardrail.
- **Upload.** Textures upload on the render thread when a gather swaps in,
  synchronously, and stay cached: a regather can stall the window.

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
