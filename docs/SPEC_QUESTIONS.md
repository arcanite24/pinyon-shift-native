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
instead) and the meaning of `CFirstTimeCareerState` stages other than 1
and 2 (both finished saves read stage 8).

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
(read as km/h) and `timeTillRelease`; (g) whether in-engine cutscenes can
be skipped (the engine does not skip them; FMV_01/FMV_02 skip with
confirm, as `confirmskip="true"`). A trace of the post-effect blend and
the white-flash state, or a capture at a cut, answers (a)-(e).

## Q62 (M6/INTRO): scoring the race to the festival

FirstTimeCareer.xml has `RacePointsRequired Points="6"`, six
TicketTriggers, RadioDJTriggers and First50TicketsDistances (remaining
distance fraction to tickets 10..2). Engine interim: the "Heats Places
Remaining" counter follows the straight-line distance left to
EndRacePosition through that table, the DJ's FestivalTicketsDepleting<n>
lines fire as it drops, and the race cannot be lost (the table never
reaches 0). Needed: (a) what the distance is measured along (route,
straight line, trigger order); (b) what RacePointsRequired counts and
whether the player can fail; (c) what the TicketTriggers and
RadioDJTriggers ids 1001-1005 fire.

## Q63 (M6/INTRO): tutor prompts in the first drive

Strings (IDS_Accelerate_Message, IDS_Steering_Message, IDS_View_Message,
IDS_Button_to_Rewind) and the GlobalRegistry FirstTimeCareer timers
(AccelerateTextTimerLimit 3, BrakeTextTimerLimit 3, MapText* 3/15,
RewindTextTimerLimit 3, EBrake thresholds) are known; the sequence is
not. Engine interim: only the accelerate prompt, shown from the hand-over
until the throttle has been held 3 s. Needed: the prompt order, each
prompt's show and clear condition, and where the brake, e-brake, rewind,
camera and map prompts start.

## Q64 (M6/INTRO): the first Autoshow restriction

RaceCentralArrive wraps `autoshows.root` in
CSetRestrictionsForFirstAutoshow true/false. Engine interim: the
Autoshow opens with the restricted flag set and no rule applied. Needed:
what the restriction is (car list, price cap, class, a forced pick) and
whether leaving without buying is allowed.

## Q65 (M6/INTRO): the opening credits

SetupFestivalDrive runs ShowOpeningCredits while New_Diner_Cutscene_2
plays (the H_OPENING_TITLES scene, probable). Engine interim: a flag for
the length of that cutscene and no credit text. Needed: the scene, its
text source and timing (per card or a fixed list), and when it hides.

## Q66 (M6/INTRO): stage order and triggers around the Heats

The flow files give the steps inside each first-time-career state but
not every hand-off between states, which is code. Engine interims, each
marked `interim` in libs/race first_time_career.*: (a) the controller
loading screen leaves on confirm or after 6 s; (b) the Viper drive ends
within 40 m of FirstTimeCareer StartPosition (the save finding says stage
1 has a route and an arrival trigger; route 170's end is also accepted);
(c) the race to the festival ends within 40 m of EndRacePosition; (d)
the All Comers heats (AllComerRaces 76 then 77, AllComerFinal 77, top 3
to pass, the final retried otherwise) are launched directly after
Radio_Unlock, before the drive to Race Central (FMV_01 says the player
made the heats; FMV_02 that they bagged the last slot; the
Do_All_Comers_Heat and Travel_To_RaceCentral instructions); the task list
puts the Heats after the SatNav tutorial, which this contradicts; (e)
Race Central's arrival point is Wristband_Intro's player placement,
within 40 m; (f) MustangRace 101 and Travel_To_Workshop are unused.
Needed: the state transitions of the first-time career activity (stages
1, 2, 7 and 10 are its save checkpoints) with their triggers, and
whether the heats are driven to or launched.

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
its gantry panels now light at the FR16 start (they had been turned to
face their normals, which point into the gantry, and were back-face
culled; their front is the file's clockwise side). So the engine, like
those captures, leaves `TEXT_COUNT` empty and shows only GO!.

Unknown: whether a start with no gantry (a street race, a point-to-point
started on an open road) fills `TEXT_COUNT` with 3, 2, 1; the engine
shows no count there. Needed, narrow: a capture of such a start, or the
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

Needed: the pad filter's narrowing law (which G it queries), the unit of
NormRearSlipAngleToCountersteer, and what DynFindPeak drives. A trace of
pad input against road-wheel angle at several speeds would settle the
lock law.

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

Engine interim: the wheel maps linearly to full lock (steer x
SteerMaxAngle) with no rate limit, as before. Needed: the WheelMapping
law and its default values from a decode of the settings constructor,
or a wheel trace. The values would then come from the executable rather
than from data.

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
  from 0 at the start level to the maximum at full damage. The
  original's rule between those two values is not decoded, and it may
  be random.
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