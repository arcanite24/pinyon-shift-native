# Spec: driving cameras

| | |
| --- | --- |
| Spec ID | `SPEC-CAMERAS` |
| Status | first usable draft for M4: the camera set, cycle, button, default, the per-car placement of all five cameras and their lenses confirmed on two cars at rest, except the R34's low chase height (5.8 cm over, open); chase springs and the look rule probable; the chase look-at drop, behaviour at speed and the shake layers open |
| Written | 2026-11-20, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | [world findings](../../site/findings/world.toml), group "Collision and cameras" ("driving camera cycle and button", "per-car camera overrides", "chase camera loaded tuning and per-car offsets", "cockpit-family camera placement", "car collision box for cameras", "camera physics layer"); [objects findings](../../site/findings/objects.toml), "camera controllers and free camera"; [rendering findings](../../site/findings/rendering.toml), "camera vertical field of view" and "camera position constants" |
| Implements it | `shift-engine` camera module (not yet; the M3 chase camera in `apps/carview/showroom_main.cpp` is a placeholder) |

## What it is

While driving, the player looks through one of five cameras and steps
through them with one button. Two are chase cameras behind the car, low
and high; three ride on the car: the driver's eye in the cockpit, the
hood and the front bumper. The right stick looks back, left and right
from any of them.

Inputs, all on the player's disc:

| Input | File | Used for |
| --- | --- | --- |
| Camera tuning | `CameraSettings.ini` in `media/camera.zip` | per camera: FOV, near plane, chase height, distance and target offsets, the chase height band, chase springs, the bumper height, the default driver offset |
| Camera layers | `CameraPhysics.xml` in the same archive (v4 reads its own copy from its `media.zip`) | per camera, a list of layers that turn the car's motion into offsets of the eye, the view angles and the FOV ([layers](#camera-physics-layers)) |
| Per-car overrides | `gamedb.slt` table `CameraOverrides` | per car: chase, hood, bumper and cockpit offsets |
| Car geometry | `gamedb.slt`: `CarPartPositions`, `Data_CarBody`, `List_UpgradeCarBodyWeight`; the car archive's `Physics/MAXData.xml` | the steering wheel position, the frame shift, the centre of mass, the collision box |
| Controls | `ControllerLayouts` and the profile (`ForzaProfile`) | the camera button and the look stick; the starting camera |

The engine reads every value from those files at load time; this spec
names the keys and says what they do. Values quoted are the oracle's
measurements, or values the tests need.

Output: per frame, an eye position, a view orientation and a vertical
field of view for the main view.

### Frames

- **Car frame**: [FORMAT-CARS](../formats/CARS.md)'s body frame, the one
  `CarPartPositions` uses: x to the right, y up, z forward, in metres.
  Its origin is the model origin; at rest the ground is at y = minus the
  stock ride height. The drawn model frame is this frame with z negated
  (FORMAT-CARS); the frame dumps report eyes in the model frame, so a
  model z of +5 is 5 m behind.
- **Collision box**: `MAXData.xml` `Collision/BoundingBox` (`MinX` ..
  `MaxZ`). Its x and y are car-frame values; its z is shifted: car-frame
  z = box z − `Data_CarBody.BottomCenterWheelbasePosZ` (car id × 1000).
  `MAXData.xml` `Misc/BottomCenterWheelbasePos z` holds the same shift
  with the sign the other way for 153 of the 184 models checked (both
  test cars among them); see Unknowns for the other 31. This is not the
  `Data_CarBody` pristine box, which is larger (the R34's front is 2.42 m
  against the collision box's 2.11 m).
- **Centre of mass** (CoM): the car's physics body position,
  [SPEC-CAR-PHYSICS](CAR_PHYSICS.md). In the car frame it lies at
  z = rear axle z + `ModelWheelbase` × `CMBackFront` (rear axle:
  `CarPartPositions` rows 7 and 8), within 1 mm on both test cars, and
  `CMHeight` above the ground.

## State

| Field | Unit | Kept by |
| --- | --- | --- |
| Current mode | one of the five driving cameras (index 0-4 of the game's 24-mode table) | the player's camera; saved in the profile as the starting camera |
| Chase heading θ and its rate ω | rad, rad/s, world yaw | chase cameras |
| Chase height state h and its rate | m above the CoM | chase cameras |
| Chase start correction c | m, set once when the camera starts | chase cameras |
| Look angle and its spring | rad, added to the heading (chase) or turning the view (on-car) | every camera |
| Layer states | per layer: attack/decay follower, spring position and rate | every camera ([layers](#camera-physics-layers)) |

The renderer reads the eye, view and FOV; the audio and the HUD read the
mode (the cockpit draws the interior, the other cameras the exterior).

## Rule

### The camera set, the button and the default

The game's camera controller has a table of 24 modes. Five of them form
the driving group, in this order:

| Mode | Name in the table | Kind |
| --- | --- | --- |
| 0 | FollowCam | low chase (`FollowLowCam` tuning) |
| 1 | FollowCam2 | high chase (`FollowHighCam`) |
| 2 | DriverCam | cockpit (`Driver`, `CockpitCam`) |
| 3 | Hood | hood (`HoodCam`) |
| 4 | BumperHigh | front bumper (`BumperHighCam`) |

The other 19 are replay, photo, free, track-side, race-start and blend
cameras; the cycle never reaches them.

- **Cycle**: each press steps to the next mode of the driving group,
  wrapping from the bumper back to FollowCam. Cockpit is skipped when the
  car does not allow it (a per-car flag, probably "has a cockpit"; see
  Unknowns); hood and bumper are skipped in a special controller state
  that free roam does not use. Measured on two cars, parked and cruising:
  FollowCam, FollowCam2, cockpit, hood, bumper, FollowCam.
- **Button**: the `SwitchCamera` action. In the default controller layout
  (`ControllerLayouts` layout 0) it is the right shoulder button; other
  layouts put it on Y or the left shoulder. The right stick gives
  `LookForward` (centre), `LookBack` (down), `LookLeft` and `LookRight`;
  the right stick click is `LookStraightBack`.
- **Default**: the profile's `Main/CameraAngle` is the starting camera;
  a new profile has 0, FollowCam. The profile's `CameraEffects` (1 by
  default) selects `CameraPhysics.xml`; with 0 the game presumably reads
  `CameraPhysicsSansEffects.xml`, the same layers without the motion
  effects (not tested).

### Per-car overrides

`CameraOverrides` has one row per car (241 of the 250 `Data_Car` rows,
231 of 240 models; the 9 cars without one are traffic models). Its
columns, in this order: `CamFollowLowHeight`, `CamFollowLowRadius`,
`CamFollowLowTargetY`, `CamFollowHighHeight`, `CamFollowHighRadius`,
`CamFollowHighTargetY`, `CamBumperHighHeight`, `CamBumperHighZ`,
`CamBumperLowHeight`, `CamBumperLowZ`, `CamHoodHeight`, `CamHoodZ`,
`CamHoodPitch`, `CamCockpitOffsetX/Y/Z`,
`SplitscreenCamCockpitOffsetX/Y/Z`, `CockpitWheelPositionOffsetX/Y/Z`,
`CockpitWheelRotationOffset`. A car without a row has all zeros.

What the rows hold, over the 241:

| Fields | Rows with a non-zero value |
| --- | --- |
| low chase (height, radius, target) | 72 |
| high chase | 17 |
| bumper (both) | 0 |
| hood | 241 (none all zero); `CamHoodPitch` non-zero on 2 |
| cockpit camera and wheel | 172; 69 have every cockpit field zero, and no row has a zero camera offset with a non-zero wheel offset |

How each camera uses them is under that camera below. A chase override
replaces the matching `CameraSettings.ini` global (`HeightOffsetGlobal`,
`RadiusOffsetGlobal`, `TargetYOffsetGlobal`) only when it is non-zero.

### Chase cameras (FollowCam, FollowCam2)

The two chase cameras share one rule with their own tuning blocks
(`FollowLowCam\…`, `FollowHighCam\…`).

**Pivot.** The camera orbits the car's centre of mass. Its heading θ is
a world yaw, not the car's; the eye sits at horizontal distance r behind
the CoM along θ plus the look angle.

**Distance.**

```text
r0 = rear extent + R + d_base
rear extent = CoM z − (collision box MinZ − BottomCenterWheelbasePosZ)
```

R is the radius offset (global or override) and d_base a fixed base
distance, 2.95 m for FollowCam and 3.10 m for FollowCam2, measured (two
cars agree within 2 mm; where it comes from is open). With the disc's
globals, r0 = rear extent + 2.80 m (low) or + 2.90 m (high). The rule
uses the car's own collision box, not its overall size: the R34's low
chase sits 5.19 m from its CoM and the CR-X's 4.83 m.

**Height.** When the camera starts, the game sets one correction c so
that the eye is between `HeightMinAboveGround` and
`HeightMaxAboveGround` above the ground:

```text
start height = box top above ground + H + split-screen term
c = (Min − start height) if below Min, (Max − start height) if above Max, else 0
```

Box top above ground is the collision box `MaxY` above the ground under
the car (one wheel's contact height at that moment, the physics wheel 2);
H is the height offset (global or override). c is added to both the height offset
and the target offset and kept until the camera restarts. Both chase
cameras on the disc have Min = Max, so at rest every car's chase eye is
at the same height above its ground: 1.80 m (FollowCam) and 2.08 m
(FollowCam2), and the per-car height and target overrides would only
change where the camera looks. This holds for the CR-X and the Miata
(no override) and the R34's FollowCam2, but the R34's FollowCam, which
has a height override, sits 1.858 m up in two runs (see Unknowns).

Every frame the eye height above the CoM is H + c + h. The state h
springs toward the box top above the CoM plus a slope term:

```text
h'' = K_h · (target − h) − D_h · h'
target = (MaxY − CoM y) + slope term
```

with `Spring\HeightSpringK` and `Spring\HeightSpringD`. The slope term is
the height of the car's own ground plane (through its four contacts,
tilted with the car) at the eye's horizontal position, relative to under
the car; it is limited to a fixed fraction of r. So on a hill the eye
keeps its height above the road behind the car, not above the car. It is
applied while the car is upright and its wheels are down (a car state,
probably three or more wheels in contact) and for a short time after;
otherwise the target is the box top alone. (Measured parked
on a 2.7° slope, rear uphill: the low chase eye is 1.56 m above the CoM
looking forward and 1.10 m looking back, both 1.80 m above the plane.)

**Look-at.** The camera looks at a point above the CoM:

```text
look-at height above CoM = (MaxY − CoM y) + T + c − δ
```

T is the target offset (global or override) and δ an extra drop,
0.335 m for FollowCam and 0.58 m for FollowCam2, measured (source open).
At rest on flat ground the view therefore looks down by
atan((H − T + δ) / r0): about 5° for the low chase and 7° for the high
one with the disc's globals.

**Heading spring.** θ follows the car's world heading ψ with a spring:

```text
e  = wrap(ψ − θ)
ω' = g · (K_y · e − D_y · (ω + s · ψ'))
θ' = ω, |ω| ≤ YRotMaxSpeed
```

`Spring\YRotSpringK` and `Spring\YRotSpringD` give K_y and D_y; s is
`Spring\YRotCarDampScale`; ψ' is the car's yaw rate. g is the mean
ground contact of the four wheels (1 with all wheels down, 0 in the air),
so the heading stops following in a jump. θ is wrapped to ±π. The form is
read from the update; the sign of the yaw-rate term is probable, and the
heading's response has not been measured in a turn.

**Side.** At rest the eye is directly behind the CoM in the world (0 to
2 mm sideways on both cars); the car's small roll shows as a 3 cm model-x
offset in the dumps.

**Clearance.** After placement the eye is lifted to 0.3 m above the world
collision hit under it when it would be lower (probable; the car's
`IsCameraCollidable` and `UseBoxCameraCollision` columns are 0 on all
three cars checked and were not exercised).

### On-car cameras (cockpit, hood, bumper)

These are fixed to the car frame: eye = anchor + offset + layer offsets,
taken to the world with the car's pose (pitch and roll included). They
share the look rule below.

**Cockpit (DriverCam).**

- Anchor: the steering wheel, `CarPartPositions` row `PartId` 9.
- Offset: (`CamCockpitOffsetX`, `Y`, `Z`) when any of the car's cockpit
  fields is non-zero; otherwise `Driver\DefaultOffsetFromSteeringWheel`
  X, Y, Z. The 69 cars whose cockpit fields are all zero use the default.
  Split screen adds the split-screen offsets, triple screen
  `Driver\TripleScreenAdditionalZOffset`. `CockpitWheelPositionOffset` and
  `CockpitWheelRotationOffset` move the drawn steering wheel, not the eye.
- FOV: `Driver\FOV`; the layers add a constant pitch (the disc's: 1.11°
  down) and, above 140 mph, more FOV.

**Hood.**

- Anchor: (0, 0.45, 0.60) m in the car frame, the same for every car (a
  car-specific vector is added to it; it was zero on both test cars).
- Offset: (0, `CamHoodHeight`, `CamHoodZ`), pitch `CamHoodPitch`; when
  all three are zero the `HoodCam\CamYOffset`, `CamZOffset` and
  `CamPitchOffset` defaults (no drivable car needs them; the 9 traffic
  models without a row would).
- The disc's layers add a constant 0.03 m down, 0.164 m forward (scaled
  by a speed curve that is 1 up to 20 mph and grows to 1.44 at 190 mph),
  and a constant 7.2° look down.
- A near-plane correction moves the eye along the pitched view axis by an
  amount from the FOV and pitch; it is zero with a zero `CamHoodPitch`
  (probable; both test cars have zero pitch).

**Bumper (BumperHigh).**

- Anchor: x = 0; y = `BumperHighCam\RestHeightAboveGround` above the
  ground under one wheel (the physics wheel 0: wheel centre height minus
  tyre radius, taken when the camera starts); z = the collision box front
  (`MaxZ` − `BottomCenterWheelbasePosZ`) − 0.6096 m.
- Offset: (0, 0, `CamBumperHighZ`) when that override is non-zero (no
  car on the disc), otherwise (0, 0, `BumperHighCam\CamZOffset`).
- The disc's layers add a speed-scaled look down (5.06° at rest).
- `CamBumperHighHeight` and both `CamBumperLow` fields are zero on every
  row and unused here.

### Looking around

The look stick sets a look target: back (π), left or right (±π/2), and a
look spring moves the look angle to it.

- **Chase**: the look angle adds to θ. The eye swings round the CoM at
  the same r, and the slope term follows it. Measured: look back puts the
  eye 4.83 m in front of the CoM looking back; look left puts it 4.83 m
  to the car's right looking left, look right the mirror.
- **On-car cameras**: the view turns by the look angle. When the angle
  is near π the eye moves to the collision box rear (`MinZ` −
  `BottomCenterWheelbasePosZ` + 0.6096 m), keeping its height; near
  ±π/2 it moves sideways to the box side 0.6096 m inside (x = ±(`MaxX` −
  0.6096 m)), keeping its z. Measured on the bumper camera; the cockpit
  and hood are the same rule (probable).

### Field of view

Each camera's vertical FOV is its `CameraSettings.ini` `FOV` plus the
output of its FOV layers. On the disc every camera has one speed layer
(input `SpeedMPH`, a five-point or three-point curve, scaled by its
`Param.x` in degrees, sprung): the chase cameras widen from standstill
to the full 22° at 190 mph, the hood and the bumper from 20 mph (10° and
23° at 190 mph), the cockpit only above 140 mph (8°). The horizontal FOV follows from the
aspect ratio.

**Chase distance at speed.** The chase cameras also move back with speed
(a `SpeedMPH` layer on the car-space z offset, sprung), and the distance
r0 is multiplied by tan(F) / tan(F + ΔF), F the tuning FOV and ΔF the
FOV layers' output, so the camera moves in as the lens widens and the
car keeps its size on screen (read from the update, probable; whether F
is the full or the half angle is not settled). The cruises do not
confirm it: at 14 to 27 mph, accelerating gently, the chase was farther
back than at rest, while the g layers were live as well:

| Camera, car | Speed | ΔF | Distance from the CoM, rest / cruising |
| --- | --- | --- | --- |
| FollowCam, CR-X | 13.6 mph | 1.17° | 4.829 / 5.150 m |
| FollowCam, CR-X | 15.4 mph | 1.34° | 4.829 / 5.134 m |
| FollowCam, R34 | 24.0 mph | 2.35° | 5.192 / 5.635 m |
| FollowCam2, CR-X | 17.0 mph | 1.48° | 4.929 / 5.131 m |
| FollowCam2, R34 | 27.2 mph | 3.05° | 5.290 / 5.459 m |

How the speed layer, the g layers and the FOV factor combine at speed is
open.

### Camera physics layers

`CameraPhysics.xml` has one element per driving camera (`FollowCam`,
`FollowCam2`, `DriverCam`, `Hood`, `BumperHigh`) holding `Layer`
elements. Each enabled layer:

1. reads an input (`InputType`): longitudinal or lateral g, speed (mph
   or a 0-1 fraction), longitudinal or lateral slip, the car's pitch in
   degrees, a constant one, one when stationary, world or car collision
   pulses, per-wheel surface bumps, the suspension's longitudinal bias,
   head tracking, an external source;
2. clamps it (`InputClampMin`/`Max`), optionally takes its derivative and
   follows its magnitude with attack and decay rates;
3. maps it through a curve (`InputToParamMapping`: constant one, linear,
   three-point or five-point, optionally mirrored for negative inputs);
4. optionally multiplies by a noise (`NoiseType` Perlin, Simplex, height
   grid or sine, with frequency from its own curve and an output range);
5. scales `Param.x`, `y`, `z`, `w` by the result and adds them to its
   output (`OutputType`): car-space eye offset (x right, y up, z forward,
   m), car-space or camera-space yaw, pitch, roll (degrees; yaw positive
   right, pitch positive down, roll positive tilts left), camera-space
   eye offset, FOV (degrees), a car-space or camera-space angular impulse,
   or the drawn steering wheel's angle;
6. when `Sprung`, passes the output through a spring with `SpringK` and
   `SpringDPercent`.

For a first playable engine only the layers without noise and without
collision or impulse inputs matter: the constant layers, the speed
layers and the pitch-angle layers above. The g, slip and bump layers are
sprung motion effects; the noise layers are shake.

### What to change in the engine's chase camera

The M3 chase camera puts the eye 1.9 + 0.6 × length behind the car's
origin and 0.6 + 0.77 × height up, and trails the heading with a 0.2 s
time constant. For the game's cameras:

1. Pivot on the CoM, not the model origin.
2. Distance: the collision box rear extent behind the CoM plus the
   radius offset plus d_base, not a multiple of the length.
3. Height: set the start correction so the eye is at the band (1.80 m or
   2.08 m) above the ground, then spring h toward the box top plus the
   slope term with the height spring; not a multiple of the height.
4. Look at the CoM plus the box top, T, c and −δ, instead of a point
   0.35 × length ahead.
5. Heading: the yaw spring above, scaled by wheel contact and limited to
   `YRotMaxSpeed`, instead of the exponential lag.
6. FOV 48.5° or 58° plus the speed layer, and the speed layer's pull-back.
7. Add the cockpit, hood and bumper cameras and the right-shoulder cycle;
   the right stick's look back and sides as above.

The engine's pull-in against the ground's collision can stay; the game's
own clearance rule is above.

## Rates and timing

- Every rendered frame: the springs step with the frame time; the
  heading and height springs use its absolute value.
- The chase start correction c and the bumper's ground height are taken
  once when the camera starts (each cycle into it), so a camera that
  starts while the car is still settling keeps that error.
- Layer springs and attack/decay followers run every frame.

## Unknowns

| Unknown | Confidence of the current reading | What to do meanwhile |
| --- | --- | --- |
| Why the R34's FollowCam is 1.858 m above the ground, not 1.80 m | measured twice, both times the camera the run started in (a second run with the presses after the car settled found the same; its cycled-back FollowCam was taken while the car rolled and is not usable); the CR-X's start camera is 1.795 to 1.798 m and the R34's FollowCam2 on the band. The R34 is the only car checked with a low-chase height override (72 cars have one), so the override may enter the start correction | put the eye on the band; recheck on a car with an override, parked with the brake held |
| d_base (2.95 m / 3.10 m) and δ (0.335 m / 0.58 m) | measured on two cars, agree within 2 mm (d_base) and 6 mm (δ) | use the measured values |
| Which shift the box z takes when `MAXData.xml` and `Data_CarBody` disagree (31 models) | both test cars agree | use `Data_CarBody.BottomCenterWheelbasePosZ` |
| Collision box for models whose `MAXData.xml` was not checked (65 of 240 in this extraction, DLC packs) | the file is in every checked car archive | read it from the car archive; failing that, the pristine box |
| The cockpit availability flag | a car field the cycle tests; 69 cars have no cockpit offsets | allow the cockpit for every car |
| Chase distance at speed: the FOV factor and the layers | factor read from the update (probable); the low-speed cruises sit 0.17 to 0.44 m farther back than at rest | the speed layer plus the factor as read |
| Heading spring sign of the yaw-rate term, and its response in a turn | read from the update, not measured in motion | as written |
| `SpringDPercent` meaning | name | damping as that fraction of critical damping (2√K) |
| Noise, impulse, collision, bump and head-tracking layers | layer semantics known, values not checked | ignore them |
| Look spring rate | the dumps were taken 1 s after the stick, fully turned | ease the look angle with a 0.1 s time constant |
| Hood near-plane correction with a non-zero pitch (2 cars) | read from the start code | ignore |
| `CameraEffects` off | file name only | read `CameraPhysicsSansEffects.xml` |

## Tolerances

- At rest, each camera's eye within 5 cm of the oracle in the car frame
  (on-car cameras) or in distance from the CoM and height above the
  ground plane (chase), and its vertical FOV within 1°.
- View pitch at rest within 0.5°.
- At speed, FOV within 1° of the oracle at the same trace speed.

## Tests

Oracle: `tools/re/cameras.py route` on two pinned seeds, the R34 V-Spec
II (`acc9-stock`, car 445) and the Honda CR-X Si (`i895-crx-owned`, car
320), parked on the same road at (2002.9, 143.8, 960.0), a 2.7° slope
with the rear uphill; the immersive camera mod's layers set to 0. Each
camera dumped twice parked (40 frames apart, identical), then the look
directions, then a part-throttle cruise. Read with `tools/re/cameras.py
analyse` (eye from the car draws' view constants) and the camera position
constants for the bumper, whose view draws no car.

Car data used:

| | R34 | CR-X |
| --- | --- | --- |
| Collision box `MinZ` / `MaxZ` / `MaxY` / `MaxX` | −2.2040 / 2.1097 / 1.1652 / 0.8455 | −1.7524 / 1.8117 / 1.0150 / 0.8200 |
| `BottomCenterWheelbasePosZ` | 0.0004 | 0.0426 |
| CoM z (car frame), measured / from the axle rule | 0.1856 / 0.1862 | 0.2339 / 0.2334 |
| Ride height | 0.2073 | 0.2547 |
| Overrides: low chase H, R, T | 0.1375, −0.15, −0.0025 | none |
| Overrides: hood H, Z | 0.5249, 0.1762 | 0.3769, 0.0834 |
| Overrides: cockpit X, Y, Z | 0, 0.21, −0.44 | 0, 0.21, −0.49 |
| Steering wheel (`PartId` 9) | (0.3599, 0.7275, 0.2600) | (0.3502, 0.6104, 0.2849) |

Rule against oracle, parked (on-car eyes in the car frame, z forward;
chase as distance from the CoM and height above the ground plane):

| Camera | Car | Rule | Oracle | FOV rule / oracle |
| --- | --- | --- | --- | --- |
| FollowCam | R34 | 5.191 m, 1.800 m | 5.192 m, 1.858 m (fails by 5.8 cm, open) | 48.5 / 48.5 |
| FollowCam | CR-X | 4.828 m, 1.800 m | 4.829 m, 1.798 m | 48.5 / 48.5 |
| FollowCam2 | R34 | 5.291 m, 2.080 m | 5.290 m, 2.078 m | 58.0 / 58.0 |
| FollowCam2 | CR-X | 4.928 m, 2.080 m | 4.929 m, 2.078 m | 58.0 / 58.0 |
| Cockpit | R34 | (0.360, 0.938, −0.180) | (0.360, 0.938, −0.180) | 62.0 / 62.0 |
| Cockpit | CR-X | (0.350, 0.820, −0.205) | (0.350, 0.820, −0.205) | 62.0 / 62.0 |
| Hood | R34 | (0, 0.945, 0.940) | (0.000, 0.945, 0.940) | 66.0 / 66.0 |
| Hood | CR-X | (0, 0.797, 0.847) | (0.000, 0.797, 0.847) | 66.0 / 66.0 |
| Bumper | R34 | (0, 0.943, 1.450) | (0.000, 0.943, 1.450) | 53.0 / 53.0 |
| Bumper | CR-X | (0, 0.895, 1.110) | (0.000, 0.895, 1.109) | 53.0 / 53.0 |

Every eye is within 2 mm and every FOV within 0.01°, except the R34's
low chase height, 5.8 cm over the band: a failed check, open (a second
R34 run, presses after the car settled, read 5.190 m and 1.858 m
again for the starting FollowCam).

A third car, the 1994 Miata (car 1110, no chase override, a box shift of
the other sign: `BottomCenterWheelbasePosZ` −0.0429), from an earlier
FollowCam dump at the same spot (`.local/mr981/parked.fh1frame`, the
camera's start time unknown): the rule gives 4.819 m from the CoM and
1.800 m above the ground plane, the dump 4.819 m and 1.818 m.

View pitch, parked: cockpit 1.11°, hood 7.20° down in the car frame
(oracle 1.11° and 7.20°). Chase, the eye-to-look-at drop at the CoM, in
the world: R34 low 0.721 m by the rule against 0.723 m measured (δ was
fitted on the CR-X's 0.662 m); high chase 0.58 m extra drop on both cars
within 4 mm.

Looking, parked:

| Camera, car | Look | Rule | Oracle |
| --- | --- | --- | --- |
| Bumper, R34 | back | eye z −1.595 (box rear + 0.6096) | −1.595 |
| Bumper, R34 | right | eye x 0.236 (box side − 0.6096), z 1.450 | 0.235, 1.450 |
| FollowCam, CR-X | back | 4.83 m ahead of the CoM | 4.828 m |
| FollowCam, CR-X | left, right | 4.83 m to the right, left | 4.828 m |

FOV at speed (speed from the physics trace, which is sampled every few
frames; the FOV layers are sprung):

| Camera, car, speed | Steady layer | Oracle |
| --- | --- | --- |
| FollowCam, CR-X, 13.6 mph | +1.20° | +1.17° |
| FollowCam, CR-X, 15.4 mph | +1.36° | +1.34° |
| FollowCam, R34, 24.0 mph | +2.52° | +2.35° |
| FollowCam2, CR-X, 17.0 mph | +1.50° | +1.48° |
| FollowCam2, R34, 27.2 mph | +3.12° | +3.05° |
| Hood, CR-X, 18.0 mph | 0 | 0 |
| Hood, R34, 30.3 mph | +0.52° | +0.50° |
| Bumper, R34, 21.8 mph | +0.30° | +0.53° |
| Cockpit, both, under 30 mph | 0 | 0 |

From the player's disc:

- `CameraOverrides` gives the counts under "Per-car overrides".
- The cycle from FollowCam visits the five cameras in the table's order
  and wraps.
