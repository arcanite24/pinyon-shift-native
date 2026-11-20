# Spec: car physics for the M3 drive

| | |
| --- | --- |
| Spec ID | `SPEC-CAR-PHYSICS` |
| Status | draft: parameters resolve for all 240 cars; longitudinal tyre, suspension units, inertia, the step, straight-line ABS, the braking friction scale and the gearbox and launch timing confirmed on one car; the launch-controller rule probable; lateral tyre, low-speed steering, drag, front brake torque and the torque chain measured fits; collision, surfaces and the lap gate open |
| Written | 2026-11-20, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | [objects findings](../../site/findings/objects.toml), group "Car dynamics"; [content findings](../../site/findings/content.toml), group "Game database" |
| Implements it | `shift-engine`, physics module (not yet) |

## What it is

This spec covers what `shift-engine` `docs/M3_DRIVE_NEEDS.md` lists as
items 4 to 7 for one car driven by the engine's own physics:

- **4. Car parameters:** which database tables and columns give a stock
  car's mass, inertia, suspension, engine, drivetrain, brakes, aero and
  steering, in which units.
- **5. Tyre model:** the force a tyre makes from its slip and load, the
  curves it reads, and how often it runs.
- **6. Assists and input shaping:** the default assists and how the pad
  reaches throttle, brake and steering.
- **7. Oracle comparison:** the trace format of
  `tools/physics-behaviour.py`, two scenarios with the oracle's numbers,
  and a drive on analytic ground that needs no world collision.

The aim is the plan's M3 gate: 0-100 km/h and a lap within 2 % of the
oracle, and a car that feels right in a play test. It is not bit-exact
physics. Where the exact form would take long to recover, the spec gives a
measured fit that is good enough for the gate, and says so.

All values come from the player's own files at run time: the game
database (`media/db/gamedb.slt`, merged with v4 and the packages as
[FORMAT-CARS](../formats/CARS.md) does), `media/physics.zip` and the
car's archive. This spec names tables, columns and keys; it quotes only
counts and scalars measured on the oracle.

Evidence comes from three places:

- the database, read-only, over the merged rows (250 `Data_Car` rows, 240
  distinct car models);
- oracle traces of a stock Nissan Skyline GT-R V-Spec II (2002, car 445,
  the "R34", seed `acc9-stock`), from `pinyon_shift_physics_oracle`
  (RI-0.5) and `pinyon_shift_physics_trace`;
- reading the translated step's data flow (what each stage reads and
  writes), recorded in the findings.

### Frames and units

- **World:** metres, y up. The car's body frame has x across the car, y
  up and z along it (the model frame of FORMAT-CARS: +z to the rear).
- **The game's body units:** the step keeps mass in units of 100 kg and
  force in units of 100 N (so a 1,560 kg car has mass 15.6), lengths in
  metres, time in seconds and angles in radians. Database columns are
  converted into these units when the car is built. An engine may work in
  SI throughout; the conversions below give SI.
- **Engine speed:** rad/s inside the step; rpm in the database and the
  trace (rpm = rad/s × 60 / 2π).

## Car parameters (item 4)

### How a stock car's rows are found

Every car row has exactly one row with `IsStock = 1` in each upgrade
table it uses. Select stock parts by `IsStock`, not by `Level`: stock
brakes, springs and anti-roll bars are level 1, other stock parts level 0.

| Key | Leads to |
| --- | --- |
| `Data_Car.Id` (the car) | `Ordinal` in `List_UpgradeCarBody`, `List_UpgradeEngine`, `List_UpgradeDrivetrain`, `List_UpgradeBrakes`, `List_UpgradeSpringDamper`, `List_UpgradeAntiSwayFront`, `List_UpgradeAntiSwayRear`, `List_UpgradeTireCompound`, `List_UpgradeRearWing`, `List_UpgradeRimSizeFront`, `List_UpgradeRimSizeRear` |
| `List_UpgradeCarBody.CarBodyID` (car × 1000) | `Data_CarBody.Id`; `CarBodyId` in `List_UpgradeCarBodyWeight`, `List_UpgradeCarBodyChassisStiffness`, `List_UpgradeCarBodyTireWidthFront`, `List_UpgradeCarBodyTireWidthRear`, `List_UpgradeCarBodyFrontBumper` (and hood, rear bumper, side skirt) |
| `List_UpgradeEngine.EngineID` | `Data_Engine`; `EngineID` in `List_UpgradeEngineCamshaft`, `List_UpgradeEngineFlywheel`, the turbo and supercharger tables and the other `List_UpgradeEngine*` tables |
| `List_UpgradeEngineCamshaft.TorqueCurveFullThrottleID` | `List_TorqueCurve.TorqueCurveID` |
| `List_UpgradeDrivetrain.DrivetrainID` | `Data_Drivetrain`; `DrivetrainID` in `List_UpgradeDrivetrainTransmission`, `…Differential`, `…Clutch`, `…Driveline` |
| `List_UpgradeSpringDamper.FrontSpringDamperPhysicsID`, `RearSpringDamperPhysicsID` | `List_SpringDamperPhysics` |
| `List_UpgradeAntiSway*.AntiSwayPhysicsID` | `List_AntiSwayPhysics` |
| `List_UpgradeTireCompound.TireCompoundID` | `List_TireCompound`, then its `FrictionMultiCurveLateralID` and `…LongitudinalID` into `List_TireFrictionMultiCurve`, then `TireFrictionCurveID0` and `…ID1` into `List_TireFrictionCurve` |
| `List_UpgradeRearWing.AeroPhysicsID`, `List_UpgradeCarBodyFrontBumper.AeroPhysicsID` | `List_AeroPhysics` |

**Count: all 240 distinct car models (and all 250 rows) resolve every
link above**, with exactly one stock row per table and every curve ID
present. 39 cars have a stock single turbo, 31 a twin turbo, 18 a
supercharger of one kind (`List_UpgradeEngineDSC`) and 1 of the other
(`List_UpgradeEngineCSC`); the rest are naturally aspirated. Drive types:
152 rear-wheel drive, 57 all-wheel drive, 31 front-wheel drive. Every
stock suspension is type 0 (independent). No car uses a CVT
(`CVTShiftTime` is −1 on every stock transmission). `Data_Motor` is
empty.

Upgrades change these values through the same tables (the non-stock
rows, `MassDiff`, `WeightDistDiff`, `DragScale` and the like). M3 needs
the stock car only.

### Mass, centre of mass and inertia

| Quantity | Table and column | Unit and rule | Evidence |
| --- | --- | --- | --- |
| Mass | `List_UpgradeCarBodyWeight.Mass` | kg, without a driver. `Data_Car.CurbWeight` holds the same in 100 kg, but the game does not take the weight from it (finding "not the car's weight in game") | body mass 15.6 for 1,560 kg; doubling `Mass` doubled the 0-100 time (#1019) |
| Front weight share | `List_UpgradeCarBodyWeight.CMBackFront` (also `Data_Car.WeightDistribution`) | fraction on the front axle | the front pair carries 57.5 % of the traced static load, the column within 1 % |
| Left-right share | `List_UpgradeCarBodyWeight.CMLeftRight` | fraction | data |
| Centre of mass height | `List_UpgradeCarBodyWeight.CMHeight` | metres | probable: not checked against the trace |
| Inertia | `List_UpgradeCarBodyWeight.BlockDimX`, `BlockDimY`, `BlockDimZ` | the car's mass as a solid box of width X, height Y and length Z, in metres: I_xx = m (Y² + Z²) / 12, I_yy = m (X² + Z²) / 12, I_zz = m (X² + Y²) / 12 about the body axes | confirmed: the traced world inertia tensor, rotated into the body frame, is diagonal and equals the three box values to the printed precision |

### Suspension

Each axle has one `List_SpringDamperPhysics` row. **Its rates are per unit
of the car's mass:** the rate the step uses is the column × the car's mass.
In SI, a spring rate in N/m is `DefSpringRate` × mass in kg.

| Quantity | Column | Unit and rule | Evidence |
| --- | --- | --- | --- |
| Spring rate | `DefSpringRate` (Min and Max are the tuning range) | × mass (kg) → N/m | confirmed: front and rear rates in the trace equal the column × 15.6 within 0.01 % |
| Bump damping | `DefDampenBumpRate` | × mass → N·s/m | confirmed, same check |
| Rebound damping | `DefDampenReboundRate` | × mass → N·s/m | confirmed |
| Damper force limits | `DampenBumpClamp`, `DampenReboundClamp` | × mass → N; the damper force is clamped to them | confirmed values; the clamp is from the data flow |
| Ride height | `DefRideHeight` (also `Data_Car.FrontStockRideHeight`, `RearStockRideHeight`) | metres | the wheel's traced rest point is within 2 mm of the column |
| Travel | `MaxCompressHeight`, `MaxStretchDeltaFromRideHeight` | metres | probable from the names |
| Bump stop | `BumpstopStiffness`, `BumpstopDamping` | probably per mass like the springs | guess |
| Geometry | `StaticCamber`, `StaticToe`, `Caster` (degrees), `CamberChangeRatioCompression`, `CamberChangeRatioExpansion`, `ChassisPivotX`, `TirePivotX`, `SteerRotAxisTireOffsetX` | | names only; not needed on flat ground |
| Anti-roll bar | `List_AntiSwayPhysics.DefSwaybarStiffness`, `SwaybarDamping` | probably per mass like the springs | the traced anti-roll force is equal and opposite on an axle's two wheels; its rate was not checked |

The spring force is the rate × the compression from the rest point, and
the damper force the bump or rebound rate (by the sign of the suspension
velocity) × that velocity, clamped. Both act along the suspension at each
wheel; the step sums them with the anti-roll force into the wheel's
normal load. At rest the four normal loads sum to the car's weight (152.8
for 15.6 × 9.8, in 100 N).

### Engine

| Quantity | Table and column | Unit and rule |
| --- | --- | --- |
| Full-throttle torque curve | `List_TorqueCurve` row `TorqueCurveFullThrottleID`: `TorqueScale` × `v0`…`v(NumTorqueValues−1)` | N·m. Sample i is at i × `TorqueCurveMaxRPM` / (`NumTorqueValues` − 1) rpm; interpolate linearly. The samples are normalised to 1.0 at their peak, and the last sample is a negative sentinel, not torque |
| Closed-throttle torque | `List_TorqueCurve.ZeroThrottleTorqueScale` | N·m of engine braking, probable |
| Rev limit | `List_UpgradeEngineCamshaft.RedlineRPM` | rpm. The traced limiter (the clamp of engine speed) sits about 1.0 × the redline column; the dashboard's shift light at 0.9 × |
| Idle and stall | `List_UpgradeEngineCamshaft.StallRPM` | rpm; the traced idle sits 0 to 100 rpm below the column |
| Engine inertia | `Data_Engine.MomentInertia` + `List_UpgradeEngineFlywheel.MomentInertia` | kg·m² |
| Boost | stock `List_UpgradeEngineTurboSingle`/`Twin` (`MinScale`, `MaxScale`, `PowerMinScale`, `PowerMaxScale`, `RobScale`, `TorqueDropOff*`) or `List_UpgradeEngineDSC`/`CSC` (`ZeroRPMScale`, `RedlineRPMScale`) | multipliers on the curve; see the torque chain below |
| Wheel share | `Data_Car.GameTorqueScale` | multiplier |

**The torque chain is a measured fit, not decoded.** On the R34 (stock twin turbo) the
engine-side torque rebuilt from the trace (the four tyres' F_x × the
rolling radius, through gear × final drive, plus the wheel, gearbox and
engine inertias times their angular accelerations) equals
`TorqueScale` × curve(rpm) × k with **k = 1.00 ± 0.02** over first and
second gear between 4,900 and 7,600 rpm at full throttle, in two runs. So
on this car the boost multiplier and `GameTorqueScale` together come to
about 1. Build the engine torque as `TorqueScale` × curve(rpm) × the stock
boost row's multiplier × `GameTorqueScale`. If that product is not near 1
on the R34, use k = 1 for M3 (that is what the gate was measured with) and
leave the split open. Confidence: the net torque is confirmed on one car;
the split between boost and `GameTorqueScale` is open. The torque reaches
the axles through gear × final drive, and an all-wheel-drive car splits it
by `RearToqueSplit`.

### Drivetrain

| Quantity | Table and column | Unit and rule |
| --- | --- | --- |
| Layout | `Data_Drivetrain.DrivetypeID` (also `Data_Car.DriveTypeID`) | 1 FWD, 2 RWD, 3 AWD (`List_DriveType`) |
| Gear ratios | `List_UpgradeDrivetrainTransmission.GearRatio0`…`GearRatio8`, `NumGears` | `GearRatio0` is reverse (negative); `NumGears` counts reverse, so a 6-speed has 7; unused gears hold −1 |
| Final drive | `…Transmission.FinalDriveRatio` | |
| Shift time | `…Transmission.GearShiftTime` | seconds |
| Gearbox and driveline inertia | `…Transmission.MomentInertia`, `List_UpgradeDrivetrainDriveline.MomentInertia` | kg·m² |
| Clutch | `List_UpgradeDrivetrainClutch.ClutchMaxTorque` (N·m), `ClutchInTime`, `ClutchOutTime` (s) | |
| Differentials | `List_UpgradeDrivetrainDifferential`: `Front`/`Rear`/`CenterLimitedSlipTorqueAccel` and `…Decel` (N·m of locking torque), `…RelVelClamp`, `…AccelDefInputTorque`, `RearToqueSplit` (sic; AWD share to the rear) | limited-slip torques probable from the names. The AWD split is confirmed: the R34's rear tyres made 67 % of the traced drive force, within 3 points of the column |
| Wheel inertia | `List_TireCompound.MomentInertia` | kg·m² per wheel (one value on every compound) |

### Clutch and gearbox states

The clutch value c runs from 1 (open) to 0 (locked). The transmitted
torque was measured at five points of the R34's launch as
(1 − c) × `ClutchMaxTorque` × about 0.78, within 1 to 4 %; 0.78 matches
the car's `GameTorqueScale` (0.775), but one car cannot tell whether
that is the factor (measured fit). The gearbox is a small state machine,
and each state sets how c moves:

| State | What it does | Clutch rate |
| --- | --- | --- |
| Open | standing start, or reverse just selected | c = 1 |
| Launch | slips the clutch to hold the engine at a falling target (below) | servo |
| Engaged | normal driving | c falls to 0 at 1/`ClutchOutTime` if not already 0 |
| Clutch in | a shift was requested; the throttle is closed | c rises at 1/`ClutchInTime` |
| Gear change | the gear number changes at the end | c = 1 for `GearShiftTime` |
| Rev-match | the engine is driven towards the new gear's speed | c = 1, about 0.05 s |
| Clutch out | power comes back | c falls at 1/`ClutchOutTime` |

In the rev-match and clutch-out states the throttle the engine sees is
the blend of the driver's throttle and the gearbox's own request by c,
so power returns as the clutch closes.

**Launch.** From open, the box enters the launch state when the throttle
is above 0.1, the brake below 0.1, the clutch output speed (driven-wheel
speed × gear × final drive) at most 700 rpm and the engine speed above
lerp(S₀, `StartRPM`, throttle), where S₀ = idle + 0.2 × (`StartRPM` −
idle) and idle is `StallRPM`. Rolling faster than 700 rpm at the clutch
it goes to rev-match instead. In the launch state, with τ = the time in
the state over 1.5 s (clamped to 1), the engine target is
lerp(L, H, throttle), with

- H = lerp(`StartRPM` + 0.2 × (`RedlineRPM` − `StartRPM`),
  idle + 0.3 × (`StartRPM` − idle), τ): 5,485 falling to 2,017 rpm on
  the R34;
- L = lerp(idle + 0.2 × (`StartRPM` − idle), idle + 0.01 × (`StartRPM` −
  idle), τ).

The clutch rate is 53 × (target − (ω + 0.04 s × dω/dt)) / `RedlineRPM`
per second (ω the engine speed in rpm, dω/dt its rate), so c opens when
the engine is below the target and closes when it is above. The launch
ends, into clutch out, when the clutch output speed passes the target
and the mean |normalised slip| of the driven wheels is under 0.7; a
brake above 0.1 returns it to open. Confidence: the rule and constants
from the data flow, probable; the timing below confirmed on one car in
three runs.

Measured on the R34 at full throttle (scenario A, sim time from the
throttle onset):

| t (s) | km/h | State | c | Engine rpm | Clutch output rpm | Rear slip | Front slip |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0.0 | 0.0 | open | 1.00 | 791 | 0 | 0.00 | 0.00 |
| 0.3 | 0.4 | open | 1.00 | 2,395 | 44 | 0 | 0 |
| 0.5 | 0.6 | open | 1.00 | 4,857 | 71 | 0 | 0 |
| 0.6 | 2.1 | launch | 0.65 | 5,874 | 353 | 0.21 | 0.13 |
| 0.7 | 5.7 | launch | 0.43 | 5,334 | 929 | 0.73 | 0.22 |
| 0.9 | 12.7 | launch | 0.58 | 4,623 | 1,525 | 0.32 | 0.11 |
| 1.1 | 19.2 | launch | 0.53 | 4,203 | 2,291 | 0.45 | 0.13 |
| 1.3 | 25.8 | launch | 0.56 | 3,726 | 3,018 | 0.39 | 0.12 |
| 1.4 | 28.9 | launch | 0.54 | 3,562 | 3,320 | 0.29 | 0.09 |
| 1.5 | 31.1 | clutch out | 0.25 | 3,535 | 3,530 | 0.23 | 0.12 |
| 1.6 | 33.1 | engaged | 0 | 3,776 | 3,776 | 0.22 | 0.06 |
| 2.0 | 42.6 | engaged | 0 | 4,867 | 4,867 | 0.23 | 0.07 |
| 2.5 | 54.0 | engaged | 0 | 6,146 | 6,146 | 0.20 | 0.06 |
| 3.0 | 64.4 | engaged | 0 | 7,276 | 7,276 | 0.16 | 0.06 |

(Slip is the mean normalised slip ratio of the axle.) The clutch stays
fully open for 0.5 s while the engine revs to `StartRPM`; the car only
creeps on the grade. The launch then holds the engine 80 to 100 rpm above
the falling target with c between 0.43 and 0.58, the rears slipping at
0.2 to 0.47 (peaks of 0.72 and 0.86 on single wheels near 0.7 s). At
about 1.42 s and 29.6 km/h the clutch output speed passes the target
(3,394 against 3,364 rpm) and the clutch closes over `ClutchOutTime`, locked
by 1.58 s at 32.7 km/h. The engine's effective throttle (its load over
the curve) is 0.3 at 0.1 s, 0.6 at 0.2 to 0.3 s, 0.83 at 0.5 s and 1 by
0.9 s, which looks like boost build-up (probable).

**Up-shift.** The automatic box requests a shift at about 0.96 ×
`RedlineRPM`. On the R34 (three runs, within 0.02 s): request at
3.20 s (68.1 km/h, 7,680 rpm); clutch in for 0.17 s with the throttle
closed, so the engine brakes (this is the power cut); gear change for
0.18 s, the gear number changing at its end, 3.55 s, which is the time
scenario A quotes; rev-match 0.05 s; clutch out 0.30 s, locked at
3.91 s, full power again by about 4.0 s.

**Braking.** The clutch stays locked under the brake; the box shifts
down and, near the stop, into reverse:

- On the R34 the 2→1 downshift starts 0.18 s after the press (75 km/h,
  3,570 rpm) and runs the same states as an up-shift: the gear becomes
  1 at +0.50 s (59.8 km/h), the rev-match blips the engine to about
  5,500 rpm, and the clutch is locked again at +1.02 s (36.7 km/h).
- With the brake held and the speed below
  `PhysicsSettings.ini` `MaxForwardVelShiftReverseMPH` (15 mph,
  24.1 km/h), the box selects reverse (clutch open), and the pedals swap:
  the brake pedal becomes the reverse throttle and the wheel brakes
  release. On the R34 this came at 22.8 and 23.0 km/h in the two stops,
  1.31 to 1.36 s after the press. The car then coasts for about 0.39 s
  while the engine revs, a reverse launch spins the rears backwards, and
  the clutch locks near 4 km/h, which stops the car 2.57 to 2.65 s after
  the press.

### Brakes

| Quantity | Column of `List_UpgradeBrakes` | Unit and rule |
| --- | --- | --- |
| Brake torque | `BrakeTorqueSlider` with `FrontBrakeSize`, `RearBrakeSize` (inches), `…CaliperPistons`, `…BrakeTypeID` and `GameFrictionScaleBraking` | the formula is not decoded. Measured fit: at full press each front wheel takes about 3,500 to 4,000 N·m (3,100 to 4,200 over eleven steps in two stops; the wheel's own inertia × its deceleration plus the tyre's F_x × the radius). Each wheel carries a brake torque value, constant through both stops: the rears' is 0.41 × the fronts' on the R34 (both sliders at 0.5, both brakes 11 in, four pistons front and two rear), so take the rear as 0.41 × the front for M3 (measured on one car; the bias rule is open). The torque is far above the grip limit: the fronts pass the ABS release point within 0.09 s of the press, so the stop is set by the ABS cycle and the tyre curve past its peak (scenario B), and a few percent of brake torque hardly changes it |
| Braking friction | `List_TireCompound.TorqueFreeLongFrictionScaleBrake` × `GameFrictionScaleBraking` | the tyre's longitudinal friction scale on a wheel whose applied torque slows it (brake, or engine braking through the driveline): 1.8 × 1.0025 = 1.805 on the R34, against the speed-blended `…Accel0`/`…Accel1` scale (1.25 to 1.15) when the wheel is driven or free. The tyre's F_x already includes the scale, and its torque on the wheel is F_x × the rolling radius with no further factor. Confirmed on one car |
| Bias | `BrakeBiasSlider` and `PhysicsSettings.ini` `BrakeBiasOffsetRear` | fraction |
| Handbrake | `BiasHandbrake` | probable: handbrake strength |
| ABS | `ReleasePointABS`, `DurationABS`, `ReleasePointABSFrontNoSteer`, `…FrontFullSteer`, `…RearNoSteer`, `…RearFullSteer` | see Assists |

### Aerodynamics

| Quantity | Column | Unit and rule |
| --- | --- | --- |
| Drag | `Data_Car.BodyAeroLongitudinalDrag`, × `Data_Car.GameDragScale` and each fitted part's `DragScale` | probable: F = `BodyAeroLongitudinalDrag` × `GameDragScale` × v², unit factor open. The traced resistance at full throttle (tyre force minus mass × acceleration, corrected for the grade) fits c₀ + c₂·v² with c₂ ≈ 0.21 N/(m/s)² and c₀ ≈ 240 N between 19 and 29 m/s, ±60 N of noise. Two readings of the column fit within that noise: the drag force in N at 100 km/h, or the coefficient × 1,000. Use the fit for M3: below 100 km/h resistance is under 4 % of the drive force |
| Lift and downforce | `Data_Car.BodyAeroForwardDownforceFront`, `…Rear`, `BodyAeroAngleZeroDownforce`; wing and bumper `List_AeroPhysics` (`Drag0`/`1`, `Downforce0`/`1`, `DefaultTuneSlider`) | probable: force per speed squared at each axle; the R34's stock wing and bumper rows have zero drag and downforce |
| Side and vertical drag | `BodyAeroLateralDragFront`, `…Rear`, `BodyAeroVerticalDrag` | probable from the names; damping of sideways and vertical motion |
| High-speed drag | `PhysicsSettings.ini` `HighSpeedDragSpeed0MetersSec`, `…Speed1MetersSec`, `HighSpeedDragScale0`, `…Scale1` | a drag multiplier ramped between two speeds (not reached by a stock car) |

### Steering

| Quantity | Column of `Data_Car` | Unit and rule |
| --- | --- | --- |
| Lock | `SteerMaxAngle`; `SteerMaxAngleFiltered` | degrees at the road wheels; the filtered lock is the one the default (assisted) steering uses, probable |
| Steering rates | `SteerMaxAngVelTurning`, `SteerMaxAngVelStraighten`, `SteerAngVelCountersteer`, `SteerAngVelDynFindPeak`, `SteerAccelTimeToMaxRate` | degrees per second, seconds |
| Speed sensitivity | `SteerSpeedSensitiveMaxGees`, `…MinMaxAngle`, `…SlowSpeed`, `…FastSpeed`, `…FastRateScale` | see Assists |
| Rear grip fix | `FixListingRearFricScale`, `FixListingNormSlip0`/`1`, `FixListingSteerAngle0`/`1` | a rear lateral-grip multiplier, see the tyre model |
| Ratio | none | the game steers the road wheels directly; there is no steering-wheel ratio column |

### Files in the car's archive

`media/cars/<MediaName>.zip` holds `physicsdefinition.bin` (2,400 bytes on
the R34) and `Physics/MAXData.xml`. `MAXData.xml` holds the wheelbase,
track and ride heights (the same as `Data_CarBody`) and the collision
bounding box and spheres; `physicsdefinition.bin` holds the same kind of
sphere list in binary. They describe the car's collision shape, not its
dynamics, and the M3 drive on flat ground does not need them. Their
layout is open (the collision spec).

### Global settings

`media/physics.zip` member `PhysicsSettings.ini` holds the game-wide
tunables as `Section\Key value` lines. The ones this spec uses are named
where they apply. `CarPhysicsUpdateHz` is the substep rate (below).
`CarGravityScale`, `AirbornGravityScale` and `PerTireOffGroundGravityScale`
scale gravity on the car (on the ground, in the air, and per wheel off the
ground).

## Tyre model (item 5)

### Inputs and outputs

Per wheel in contact, once per substep:

| Input | Meaning |
| --- | --- |
| Load N | the wheel's normal load from the suspension (force) |
| Contact velocities | the ground's velocity at the contact patch along the wheel's heading (v_x) and across it (v_y), and the slip velocity: the wheel's surface speed ω·r − v_x |
| Surface | the surface index under the wheel (the surface spec's table) |
| Wheel | which axle (front or rear) and the car's scales below |

The outputs are the longitudinal and lateral forces at the contact patch,
an aligning torque, and the wheel's slip state (which the ABS, traction
control and the HUD read).

### Slips

- **Slip ratio** σ = |ω·r − v_x| / max(|v_x|, v_min), with a small v_min
  floor at standstill.
- **Slip angle** α = atan2(|v_y|, |v_x|), in degrees. Below about 1 m/s
  the angle is blended towards a linear term in v_y, and at a near
  standstill it is that linear term only, so the force fades smoothly
  instead of jumping.
- **Normalised slips:** s_x = σ / σ_peak(N) and s_y = α / α_peak(N), so
  1 is the peak of each curve.
- **Combined slip:** s = max(√(s_x² + s_y²), s_min), with a small floor.
  This is a friction circle in normalised slip.

Signs follow the velocities; the forces oppose the slip.

### Curves and where they live

The longitudinal and the lateral curve each come from one
`List_TireFrictionMultiCurve` row (the compound's
`FrictionMultiCurveLongitudinalID` and `FrictionMultiCurveLateralID`):

- `TireFrictionCurveID0` is the curve at the light load `MinLoadCurve`,
  `TireFrictionCurveID1` the curve at the heavy load `MaxLoadCurve`. The
  loads are in kilograms-force (the same pair on every row). Between them
  the curves are blended linearly by load in kgf; outside, the load is
  clamped (`LoadClamp`, which a road car does not reach).
- A `List_TireFrictionCurve` row is `NumCurveValues` (100) friction values
  `v0`…`v99` at evenly spaced slips from 0 to `MaxSlip`: slip ratio for the
  longitudinal curve, slip angle in degrees for the lateral one. The values rise from 0 to a peak of 1.0 and fall off past it.
- **Peak slip:** the slip of a curve's largest sample, blended by load
  like the values, and scaled by the sidewall factor below. This gives
  σ_peak(N) and α_peak(N).
- **Sidewall factor:** the sidewall height h = tyre width × aspect / 100
  (from `Data_Car.FrontTireWidthMM`, `FrontTireAspect` and the rear pair)
  maps through `PhysicsSettings.ini` `SidewallScalePeakSA`
  (`TireSidewall0`, `TireSidewall1` in metres to
  `TireSidewallScalePeakSASR0`, `…SASR1`), clamped at the ends. The factor
  scales the peak slips and the curve's slip axis together, so a taller
  sidewall stretches the whole curve. The lateral peak is then clamped to
  `TireClampHeavyPeakSALow`, `…High` (degrees), probable.

### Force

```text
mu_x = curve_x( s · σ_peak(N), N )      the longitudinal curve at the combined slip
mu_y = curve_y( s · α_peak(N), N )      the lateral curve at the combined slip

F_x = mu_x · (s_x / s) · N · k_x        each direction takes its share of the circle
F_y = mu_y · (s_y / s) · N · k_y
```

curve(x, N) reads both load curves at slip x (on the stretched slip axis)
and blends them by load. The scales are products:

| Scale | Longitudinal k_x | Lateral k_y |
| --- | --- | --- |
| Surface friction | the surface's `FrictionScale` (1 on asphalt) | same |
| Body | `List_UpgradeCarBodyChassisStiffness.FrontLongFrictionScale` / `RearLongFrictionScale` | `…FrontLatFrictionScale` / `RearLatFrictionScale` |
| Compound | under drive, `List_TireCompound.TorqueFreeLongFrictionScaleAccel0` at `…AccelSpeed0` m/s to `…Accel1` at `…AccelSpeed1`, clamped (the trace reads 1.15 above 15 m/s) | none found; see "What was checked" for the measured lateral factor |
| Off-road | 1 + the surface's `OffRoadness` × a constant | — |
| Rear | — | the surface's `RearGripMultiplier`, and on the rear wheels the `Data_Car.FixListing*` multiplier: `FixListingRearFricScale` faded in between the normalised slips `FixListingNormSlip0`, `…1` and out by the steering input between `FixListingSteerAngle0`, `…1`, probable |
| Handbrake | the surface's `HandbrakeGripMultiplier` on the rear wheels while the handbrake is held | same |
| Misc | a constant near 1 (1.01 on the R34; the source is not identified) | |

`List_TireFrictionCurve.FrictionScale` (a different value on each load
curve) does not appear in the traced values: the curve value in the trace matches the blended samples without
it. An implementation leaves it out.

Surface off-road behaviour (`OffRoadness`, `OffRoadDryPeakSA`) pulls the
lateral peak slip towards the surface's own peak; on asphalt it does
nothing. The surface table itself is the surface spec's (M3 needs item 2).

Temperature, pressure, wear and camber act through `List_TireAffectCurve`
rows named by the compound (`AffectCurve…ID`) and the `PhysicsSettings.ini`
`TempAffectFrictionScale`, `PressureAffectFrictionScale`,
`WearAffectFrictionScale`, `CamberAffectFrictionScale` switches. On the
R34's drive they stay within 1 % of 1 (the lateral scale read 0.982 to
0.9925). For M3 an implementation may leave them out.

### What was checked

Against the R34's oracle trace (stock compound, asphalt):

| Check | Result | Confidence |
| --- | --- | --- |
| Longitudinal curve value | the load-blended, sidewall-stretched curve at the traced slip gives the traced μ within 1 % at four points between μ 0.02 and 0.60 | confirmed |
| Longitudinal force | F_x = μ_x × N × the compound scale × the misc scale × the affect scale, exact to the printed precision | confirmed |
| Sidewall factor | 1.27 on the R34's tyres, applied to both the peak slips and the curve's slip axis | confirmed |
| Lateral curve value | the same construction **over-predicts** the traced lateral value by 7 to 11 % (three points) | open |
| Lateral measured fit | multiply the lateral curve value by **0.915** (the three ratios are 0.907, 0.907 and 0.930) | measured, three points |
| Low-speed turn | at full lock and 2 to 3 m/s, coasting (the throttle released with the steer input, measured from 0.8 s after it), the car's path radius is 3.37 m, steady for 3 s | measured |

The lateral gap may come from the lateral peak clamp, a different stretch
on the angle axis or a camber term. The fit is enough for M3's play test;
the lateral gate is the turn circle and the lap.

### Step and rates

- **Substeps:** the per-body step clamps dt to 0.1 s and splits it into
  n = ⌈dt × 359⌉ equal substeps. 359 is `PhysicsSettings.ini`
  `CarPhysicsUpdateHz`. So the body runs at about 360 Hz: 3 substeps at
  8.33 ms, 6 at 16.67 ms (confirmed, RI-0.5).
- **Tyre calls:** each substep evaluates every wheel in contact once: four
  calls per substep with all wheels down, 1,440 a second per car. One
  contact point per wheel (confirmed, RI-0.5).
- **Order within a substep:** suspension and contact, then the tyres, then
  the drivetrain and body integration (from the data flow; the order of
  the last two is probable).

An engine can use a fixed 1/120 s outer step with three substeps, which is
the oracle's own pattern at 120 fps; RI-0.5 found the oracle deterministic
at that step.

## Assists and input shaping (item 6)

### Defaults

A new profile (`media/profileschema/ForzaProfile.sch`, "Career Assists")
starts with `Braking` 1, `Steering` 1, `StabilityControl` 1,
`TractionControl` 0 and `Shifting` 0 (values of the profile's enums). In
the seed's traces the ABS setting reads 1, the traction and stability
flags stay 0 and the gears change on their own: ABS on, traction control
off, automatic gears. M3 implements that set.

### ABS

Each step, for each wheel: when the smoothed normalised slip ratio (the
mean of the wheel's last five substep values) passes the release point (locking, so the slip is negative), the brake on that
wheel goes to 0, and it is applied again after `DurationABS` seconds. The
release point is blended by the steering input between the `NoSteer` and
`FullSteer` columns of the axle (`ReleasePointABSFrontNoSteer`,
`…FrontFullSteer`, `…RearNoSteer`, `…RearFullSteer`, in units of the
normalised slip, so 1 is the tyre's peak). The ABS flag that the HUD
shows is set on the steps where a wheel is released, so it flickers
during a stop.

Measured on the R34's straight stops (no steering): the fronts released at
a normalised slip of −2.9 to −3.3 and the rears at −2.4 to −2.7, which are
the two `NoSteer` columns within the step size; each release lasted 0.045
to 0.06 s against `DurationABS`, after which the brake went straight back
to 1 with no ramp. Three releases checked against the trace came one step
after the five-substep mean passed −3.0 (means −3.00, −2.86 and −2.96
just before). Over the ABS part of the stop (the press to the reverse
selection) the fronts were braked on 50 to 54 % of steps and the rears on
74 to 81 %; an engine that releases far more often stops short of the
oracle's deceleration. So a straight ABS stop lets the wheels run to two or three times the
peak slip before it releases, which is why the mean deceleration (0.87 g)
sits well below the tyre's peak grip: the gate number depends on these
release points and on the curve past its peak. Confidence: the straight-
line rule confirmed on one car; the steering blend is from the data flow,
probable. With ABS off the game uses `PhysicsSettings.ini`
`NoABSSlipDef…` and `NoABSBrakeTravel` instead (probable).

### Traction control

When the driven wheels' normalised slip ratio passes 1 under drive, the
traction control pulls the wheel's speed back so that the slip sits at
the peak, rather than cutting engine torque (from the data flow, probable).
`Data_Car.AssistsTCSSlipDefTakeoff` and `…Moving` and the
`PhysicsSettings.ini` `TractionControlSpeed`, `TCSFullEffectFricDiff`,
`TCSFullSteerSlipScale*` and `YawMin`/`MaxAngleForTCS` keys tune it. With
traction control off (the M3 default) the flag stays 0 in every oracle
run. The R34's full-throttle launch then takes the rear normalised slip
ratio to about 0.79 and the front to 0.25: below the peak, so the launch
is limited by the clutch phase (below), not by wheelspin, and M3 does not
need traction control.

### Stability control

`PhysicsSettings.ini` `StabilityManageSpeed` and `AutoSteerOverrides`
(`STMSlipMin`, `STMSlipMax`) name it; the cockpit's STM light reads its
active flag. What it does was not traced. An implementation leaves it
out for M3.

### Throttle, brake and steering from the pad

| Stage | Source | Rule |
| --- | --- | --- |
| Deadzones | `ForzaProfile.sch` `ControllerAdvancedOptions` (`SteeringAxisDeadzoneInside`, `…Outside`, `ThrottleDeadzoneInside`, `…Outside`, `BrakeDeadzoneInside`, `…Outside`) | the axis is rescaled from the inside to the outside deadzone to 0..1 and clamped, probable |
| Throttle ramp | `PhysicsSettings.ini` `ControllerDelays\AccelDelaySpeed` | the throttle moves towards the trigger at that many full travels per second. Measured: the traced gas control's lower envelope rises at 4.9 per second, so about 0.2 s from 0 to 1. On alternate steps of the ramp the control reads 1.0 (finding "controls at step entry are not the step's input"), so take the envelope |
| Brake ramp | `ControllerDelays\BrakeDelaySpeed`, `HandBrakeDelaySpeed` | the same rule. Measured: the traced brake control reads 1.0 on the first step of a full press, but the wheels' brake value ramps 0, 0.38, 0.68, 1.0 over 0.05 s, which is `BrakeDelaySpeed` full travels per second; the handbrake rate is probable |
| Brake pedal curve | `PhysicsSettings.ini` `BrakeInputScale` (`…Input0`…`3`, `…Output0`…`3`) | a piecewise-linear map from the brake input to the brake torque fraction, probable |
| Steering | `Data_Car.Steer*`, `PhysicsSettings.ini` `SteeringWheelFilter\*`, `SteerDefCountersteerRate`, `SteerDefDynSteerFindPeakRate` | measured at walking pace: the traced steer control steps straight to the stick (−1 at full left), so the shaping acts after it. The car's yaw then builds linearly over 0.40 s, the lock divided by `SteerMaxAngVelTurning`. At full lock the path radius (3.37 m) gives a kinematic road-wheel angle of 41.5°, which is `SteerMaxAngle` within 1°. So at low speed the road-wheel angle moves towards stick × `SteerMaxAngle` at `SteerMaxAngVelTurning` degrees per second, probable. With speed the lock narrows through the `SteerSpeedSensitive*` columns towards `SteerMaxAngleFiltered` (probable, not traced at speed). The counter-steer and find-peak rates belong to the assisted steering (`Steering` 1) and were not traced |
| Gears | `Shifting` 0 | automatic: it shifts up at about 0.96 × `RedlineRPM` (1→2 requested at 7,680 rpm and 68.1 km/h, the same in three runs). A shift takes about 0.70 s from the request to the clutch locking, which is `ClutchInTime` + `GearShiftTime` + `ClutchOutTime` plus the short rev-match, with the throttle cut while the clutch opens. Under braking it shifts down (2→1, gear changed near 60 km/h). With the brake held below `MaxForwardVelShiftReverseMPH` it selects reverse and the brake pedal becomes the throttle (23 km/h in both stops). See "Clutch and gearbox states" |

## Oracle comparison (item 7)

### The trace format

`--pinyon_shift_physics_trace=N` writes one JSONL event every N frames to
the session log (`<state>/logs/<session>.jsonl`), which the route runner's
`result.json` names as `event_log`:

| Field | Meaning |
| --- | --- |
| `event` | `fh1.physics_trace` |
| `frame` | the route's frame number (wall frames at the route's `# clock-hz`) |
| `t_ms` | host time in milliseconds, for routes without a fixed clock |
| `mode` | the game mode: 17 free roam, 3 a race, 20 a v4 Rally stage |
| `cars` | one entry per car in the world, joined by `|`; each entry is `index:car:driver:speed:rpm:gear:gas:brake:steer` |

In an entry: `index` is the car's place in the world's car vector, `car`
its identity (stable for the run), `driver` 0 for the player, `speed` m/s
(always positive), `rpm` the engine speed, `gear` 0 reverse, 1 to 8
forward and 9 neutral, and `gas`, `brake` (0 to 1) and `steer` (−1 to 1)
the controls after input shaping.

`tools/physics-behaviour.py summary` integrates the player's speed over
the route's fixed clock (frame / clock-hz) and reports 0-60 and 0-100
km/h from the **launch**, the first sample with gas above 0.05 at a speed
below 1 m/s, interpolated at the crossing, plus distance and top speed per
car, and the race time when a Rally stage finished. `compare` fails when
the player's 0-60, 0-100 or race time differs by more than 2 % (or the AI
field's mean distance by more than 3 %). The engine writes the same
events, and `compare` can compare the two files once the engine's frame
follows its sim time.

Notes for an implementation:

- The launch is the first sample with gas above 0.05, not the trigger
  press: the throttle ramp is inside the measured time.
- With N = 6 at 60 Hz a sample is 0.1 s apart.
- **Use sim time, not route frames.** A route frame is wall time at the
  route's clock, while the game's sim time per frame drifts by up to 2 %
  within a run and between runs (the runner's `wall_time_ratio` read 1.05
  to 1.11 over whole runs). The summary's 0-100 therefore varies from 5.68
  to 5.82 s between oracle runs of the same route, while the body trace's
  sim-time crossings repeat within 0.3 %. The engine writes `frame` as
  round(sim time × clock-hz), and the gate uses the sim-time numbers
  below.
- `compare` has no gate for a free-roam segment: with no Rally stage the
  race time is missing on both sides, which passes. The segment's gate
  below is therefore a separate check on the same events.

The oracle's body trace (`physics-oracle.bin`, RI-0.5) is finer: the whole
body state at every step, with the step's dt. The reference curves below
come from it.

### Scenario A: 0-100 km/h standing start

| | |
| --- | --- |
| Car | car 445 (Nissan Skyline GT-R V-Spec II, 2002), stock, as the seed `acc9-stock` has it |
| Assists | ABS on, traction control off, automatic gears (the defaults above) |
| Place | the Colorado free-roam spawn, on asphalt |
| Start | at rest, all wheels down and settled (normal loads summing to the weight), first gear, engine at idle (about 770 rpm), at world (2002.9, 143.84, 960.0) |
| Direction | along the world vector (x, z) = (0.805, −0.593), straight, no steering |
| Input | full throttle from t = 0, no brake, no steering. The oracle route is `config/render-tests/fh1-physics-accel.fh1test` (throttle at route frame 1100). The time is measured from the launch, the first step with the gas control above 0.05 |
| Road | falls 5.1 m over the first 100 m (the profile under "Drive on analytic ground"); the car leaves the asphalt at about 110 m |

Oracle numbers (sim time from the launch, from the body trace):

| Crossing | Run A | Run B1 | Run B2 | Run C (accel route) | Distance |
| --- | --- | --- | --- | --- | --- |
| 0-20 km/h | 1.122 s | 1.126 s | 1.126 s | 1.119 s | 1.7 m |
| 0-40 km/h | 1.891 s | 1.899 s | 1.892 s | 1.893 s | 8.4 m |
| 0-60 km/h | 2.780 s | 2.788 s | 2.781 s | 2.782 s | 20.7 m |
| 0-80 km/h | 4.557 s | 4.556 s | (lifted at 79 km/h) | 4.572 s | 55.1 m |
| **0-100 km/h** | **5.837 s** | | | **5.851 s** | 87.2 m, 3.80 m lower |

What happens on the way:

- **Clutch phase:** for the first 0.5 s the clutch is open while the
  engine revs from idle to `StartRPM`; the launch controller then slips
  it, holding the engine above a falling target, until about 1.42 s and
  29.6 km/h, and it is locked from 1.58 s (32.7 km/h). The table and the
  rule are under "Clutch and gearbox states". This phase sets 0-20 and
  most of 0-40 and 0-60: an engine that locks the clutch later is slow
  there, and one that lets it bite at once is quick.
- **First gear:** rear normalised slip ratio up to 0.79, front 0.25, no
  traction-control action.
- **1→2 shift:** requested at 3.20 s (68.1 km/h); the gear number changes
  at 3.55 s, and the clutch locks at 3.91 s (see "Clutch and gearbox
  states"). Gate the gear change, not the request.
- The physics-behaviour summary of the same routes (frame clock) gives
  0-60 2.62 to 2.69 s and 0-100 5.68 to 5.82 s; do not gate on those (see
  the trace notes).

### Scenario B: a straight segment (launch, lift, brake)

Route `config/render-tests/fh1-physics-launch-brake.fh1test`: the start of
scenario A, full throttle from route frame 1100 to 1440, then full brake
until frame 1700, all on the spawn road's asphalt. The throttle takes
effect about 70 route frames after the input (a game-side delay before the
car responds, outside the gate), so the brake comes at a slightly
different speed in each run. Gate the stop from the brake onset:

| Quantity | Run B1 | Run B2 |
| --- | --- | --- |
| 0-80 km/h (sim time) | 4.556 s | (lifted at 79.4 km/h) |
| Brake onset after the launch | 4.664 s | 4.517 s |
| Speed and gear at the onset | 81.69 km/h, 2nd | 79.37 km/h, 2nd |
| Distance at the onset | 57.4 m | 54.2 m |
| Time to stop (speed below 0.3 m/s) | 2.645 s | 2.573 s |
| Stopping distance | 24.95 m | 23.63 m |
| **Mean deceleration** | **8.58 m/s²** | **8.57 m/s²** |
| ABS-active steps during the stop | 64 | 64 |

The mean deceleration is 0.87 g on a road falling about 4 % there; the
tyres themselves make about 0.91 g. The fronts reach the ABS release
point within 0.09 s of the press and the rears within 0.25 s, and the ABS
then cycles each wheel between about −3 (front) or −2.5 (rear) and near 0
until the stop (see ABS). The gearbox drops to first and then reverse on the way
(see "Clutch and gearbox states"), and the stop has two phases:

| Phase | Run B1 | Run B2 |
| --- | --- | --- |
| ABS braking, press to reverse selection | 1.355 s, 81.7 → 22.8 km/h, 12.06 m/s² | 1.313 s, 79.4 → 23.0 km/h, 11.92 m/s² |
| Reverse: coast, then reverse launch to the stop | 1.29 s, about 4.9 m/s² | 1.26 s, about 5.1 m/s² |

The 8.57 m/s² mean depends on both: an engine that keeps braking to the
stop with the ABS alone will be quicker than the oracle, so it needs the
reverse selection with the pedal swap. The engine runs the same inputs and compares the stop from
its own onset speed v₀: stopping distance v₀² / (2 × 8.57 m/s²).

### The lap

**Not specified yet; the lap gate is open.** Three things block it:

- `compare` gates only a Rally stage's race time, and a stage needs the
  world's collision and surfaces (M3 needs items 1 to 3), which this spec
  does not cover.
- The oracle can drive a stage only with its test AI driver at the
  player's wheel (Rally route 1: 171.26 and 171.22 s in two runs,
  [FH1_RENDER_TEST_AUTOMATION](../native-renderer/FH1_RENDER_TEST_AUTOMATION.md)).
  The engine would need the same driver, or a replay of the traced
  controls, which drifts off the line without closed-loop steering.
- A free-roam segment has no gated time in `compare`.

Until then M3's handling gate is scenarios A and B plus the turn circle
(below), and the play test. When collision lands, the next step is a
recorded-control replay of a short stage, compared as distance along the
line against time.

### Drive on analytic ground (no world collision)

The engine can reproduce scenarios A and B without the world: an
analytic ground that follows the spawn road's height along the start
direction and is level across it, all asphalt (surface friction 1, no
off-road terms), no other cars.

Height along the road, relative to the launch point, from the oracle's
body trace (the car's centre of mass; the road surface follows it within
a few centimetres):

| Distance | 0 m | 10 m | 20 m | 30 m | 40 m | 50 m | 60 m | 70 m | 80 m | 90 m | 100 m |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Height | 0 | −0.44 | −0.85 | −1.24 | −1.57 | −1.80 | −2.06 | −2.58 | −3.24 | −4.06 | −5.06 m |

Interpolate linearly; the road is straight (the heading stays constant to
0.1°). A level plane is also worth running, as a sanity check, but its
times are not the oracle's: the grade gives the car about 0.4 m/s² of
free acceleration, so on the level the 0-100 comes later. Gate on the
profile.

Checks on this ground:

| Check | Oracle | Tolerance |
| --- | --- | --- |
| Static normal loads | sum to the weight, front pair 57.5 % | 1 % |
| 0-20, 0-40, 0-60, 0-80 km/h | scenario A's table | 2 % or 0.05 s, the larger |
| 0-100 km/h | 5.84 s (5.837 and 5.851 in two runs) | 2 % (5.72 to 5.96 s) |
| 1→2 gear change | 68 km/h, 3.55 s | 2 % |
| Stop from scenario B's onset speed | 8.57 m/s² mean | 2 % |
| Full-lock circle at 2 to 3 m/s (route `fh1-physics-steer`) | 3.37 m path radius | 2 % |

The turn circle's inputs: full throttle for about 0.6 s of motion, then
the gas released with full lock at about 3 m/s. The car coasts in first
with the clutch locked and no throttle, the engine idling down from
1,100 to 816 rpm, and the speed decays from 2.84 to 2.08 m/s over 3.5 s
(about 0.22 m/s²). There is no creep throttle.

## Unknowns

| Part | State | What closes it |
| --- | --- | --- |
| Lateral tyre value | over-predicted 7 to 11 %; a measured factor stands in | trace the lateral path at higher slip angles; the steer route at speed |
| Brake torque | front measured at full press (3,500 to 4,000 N·m a wheel), rear 0.41 × front on one car; the formula and bias rule not decoded | a light brake press in a route; a car with other brake columns |
| Clutch capacity factor | (1 − c) × `ClutchMaxTorque` × 0.78 measured on one car | the same fit on a car with another `GameTorqueScale` |
| Drag and rolling resistance | noisy fit, unit of the drag column open | a coasting route in neutral |
| Boost and `GameTorqueScale` | net 1.00 on one car | the same fit on a naturally aspirated car and a supercharged one |
| Differentials | limited-slip columns named only | a cornering trace under power |
| Steering at speed, assisted steering, stability control | not traced | the steer route above 60 km/h |
| Traction control on | not traced | a launch with the assist on |
| ABS with steering | straight-line release points and duration confirmed; the steering blend probable | a stop with the wheel turned |
| Temperature, pressure, wear, camber | stay within 1 % of 1 on these drives | not needed for M3 |
| Aligning torque | outputs seen, not used here | steering feel, after M3 |
| Other surfaces | grass (surface 2) seen past 110 m | the surface spec |
| Collision shape, the lap | out of scope here | the collision spec, then the lap |

## Tolerances

- The oracle repeats its sim-time crossings within 0.4 % (four runs; 0-100 within 0.24 % in two) and
  its braking deceleration within 0.2 % (two runs). The physics-behaviour
  summary on the frame clock repeats only within about 2.5 %.
- The M3 gate is 2 % on 0-100 km/h (sim time) and on scenario B's mean
  deceleration; the intermediate crossings take 2 % or 0.05 s, the larger,
  because the clutch phase dominates the first second.
- Quantities marked measured fit (drag, the lateral factor) carry their own
  spread; a miss within those spreads is not an implementation bug.
- Bit-exact agreement is not a goal; the step only has to keep the
  oracle's rate of about 360 Hz (an engine may use 1/120 s with three
  substeps).

## Tests

Oracle side (multivac, run lock, hidden, a copy of the pinned seed):

- `tools/run-fh1-render-test.py config/render-tests/fh1-physics-launch-brake.fh1test --state-root .local/render-seeds/acc9-stock --hidden`
  with `--game-argument=` for each of `--pinyon_shift_physics_trace=6`,
  `--pinyon_shift_physics_oracle=true`,
  `--pinyon_shift_physics_oracle_snapshot_frame=1095` and
  `--pinyon_shift_physics_oracle_trace_ticks=800`. It writes the JSONL
  events and `physics-oracle.bin` (scenarios A and B).
- The same with `config/render-tests/fh1-physics-accel.fh1test` (scenario A's
  0-100; Run C), and with `config/render-tests/fh1-physics-steer.fh1test`, trace
  N = 1 and 600 trace ticks (the turn circle).
- `tools/physics-behaviour.py summary <result.json>` for the frame-clock
  summary; `tools/re/physics-inject-compare.py` reads the body trace.

Engine side, as unit tests on the R34's rows:

- The inertia from the box rule and the spring, damper and clamp rates
  from the per-mass rule against the values above.
- The substep count n = ⌈dt × 359⌉ for dt 1/120, 1/60 and the clamp.
- The tyre curve read at a load between the two curves, and the sidewall
  factor, against the longitudinal check.
- Scenarios A and B and the turn circle on the analytic ground, against
  the tolerances.
