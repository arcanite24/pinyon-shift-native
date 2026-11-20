# Spec: free-roam traffic

| | |
| --- | --- |
| Spec ID | `SPEC-TRAFFIC` |
| Status | first usable draft for M4: road network complete; population, spawning and lane following measured on one oracle cruise; the local density sum, junction choice, avoidance and collision response probable or open |
| Written | 2026-11-20, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | [world findings](../../site/findings/world.toml), group "Placements and routes" (road network, traffic population, traffic driving, traffic bodies); [content findings](../../site/findings/content.toml), "tunable settings and traffic density" and "routes and AI lines" |
| Implements it | `shift-engine` traffic module (not yet) |

## What it is

Free roam puts ordinary road cars on Colorado's roads around the player:
a small population, about six cars at noon on a town road, that appears
about 330 m away, drives its lane at the road's speed and disappears
behind the player at about 390 m. Traffic follows the sat-nav road graph,
not the race routes' racing lines. Festival drivers (other Horizon cars)
share the roads and use the same spawning rules with their own densities
and caps; this spec covers traffic and notes where festival cars differ.

Inputs, all on the player's disc:

| Input | File | Used for |
| --- | --- | --- |
| Road graph | `media/tracks/colorado/colorado.nav` (loose, big-endian) | lanes' centre lines, direction, junctions, road type, traffic density id |
| Densities and car list | `AIOpenWorld.xml` in `media/gametunablesettings.zip`, settings block `freeroam` | per-density-id range, speed, spawn distances, banned and allowed car groups; the traffic models, how many load at once, the initial count |
| Tunables | `GlobalRegistry.xml` in the same archive (v4 replaces this member from its `media.zip`; the `AI/Traffic` and `AI/Spawning` values are the same) | population caps, time-of-day ramp, spawn intervals, speeds, avoidance, lane changes, level-of-detail distances |
| Collision tunables | `PhysicsSettings.ini` in `media/physics.zip` | the `Collision` keys that name traffic |
| Car data | `gamedb.slt`: `Data_Car`, `List_UpgradeCarBody`, `List_UpgradeCarBodyWeight` | each traffic model's physics, mass included ([SPEC-CAR-PHYSICS](CAR_PHYSICS.md)) |

The engine reads every value from those files at load time; this spec
names the keys and says what they do. Values quoted below are ones the
oracle measured, or are needed by the tests.

Outputs: traffic cars, each a car with a model, a pose and a speed,
driven along a lane, for the renderer, the physics world and the audio.

## The road network

`colorado.nav` is a road map in the style of OpenStreetMap: nodes with
positions, ways that list nodes in order, and key-value tags on both. It
is the only lane data free-roam traffic can use: Colorado's `bin.zip` has
no other road or lane member, and the `OWTM` files in `aiopenworld.zip`
(`route_NNN.owt`, 46 files, a 48-byte header and 48-byte points) are
racing lines for 46 race routes, with no route for free roam. The map
screen, the sat-nav and fast travel's "nearest road point" use the same
file.

### File layout

All numbers are big-endian 32-bit: unsigned integers, or IEEE floats for
positions. The file is ten header words, nine tables in order, and a
trailer word equal to the first header word.

| Header word | Meaning | Colorado |
| ---: | --- | ---: |
| 0 | magic, repeated as the file's last word | `0E177551` |
| 1 | node count N | 12,036 |
| 2 | way count W | 459 |
| 3 | tag count T | 3,680 |
| 4 | way-node reference count R | 13,238 |
| 5 | node-way reference count (equals R) | 13,238 |
| 6 | key count K | 17 |
| 7 | value count V | 312 |
| 8 | key string bytes | 223 |
| 9 | value string bytes | 3,299 |

| Table | Entries | Entry (bytes) | Fields |
| --- | ---: | ---: | --- |
| nodes | N | 32 | id; x, y, z (floats, m); count and first index of this node's node-way references; count and first index of its tags |
| ways | W | 16 | node count and first index into the way-node references; tag count and first index of its tags |
| way-node references | R | 4 | a node index (into the node table, not the node id) |
| node-way references | R | 8 | a way index and the node's position in that way |
| tags | T | 8 | key index, value index |
| key offsets | K | 4 | offset of a zero-terminated ASCII string in the key strings |
| value offsets | V | 4 | the same for the value strings |
| key strings | | header word 8 | |
| value strings | | header word 9 | |

Node ids rise with the index but skip 1,382 numbers; references use the
index. Every node-way reference points back at its node (all 13,238
checked). 1,022 nodes are on two or more ways, but many of those are
only where one way ends and the next begins. Counted by incident road
segments, 446 nodes are junctions (367 with three segments, 74 with four,
5 with five), 10,840 nodes are plain points on a road and 16 are road
ends.

Positions are in the physics world frame, y up, metres: the M3 oracle
lap's body positions lie a median 3.0 m and at most 8.9 m from the
nearest road segment (2,508 samples).

### Tags

| Key | On | Values | Meaning for traffic |
| --- | --- | --- | --- |
| `road_type` | ways | `a`, `b`, `freeway`, `dirt`, `shortcut`, `none` | a way with this tag is a road. Ways without it (33, including the long map outlines) are map areas and labels (`text`, `area_type`) and carry no traffic |
| `traffic_density` | ways | a density id, 2 to 17 | which `Density` entry of `AIOpenWorld.xml` applies |
| `traffic_disabled`, `ai_disabled` | ways | `true` | no traffic on the way (both make the way unusable here) |
| `oneway` | ways | `true` | one direction only, in node order (probable: on all 154 sampled freeway segments the opposite carriageway lies to the left of node-order travel, as right-hand driving needs; no one-way way was driven in the oracle) |
| `display_name` | ways | a name | map label |
| `allowedfordemo`, `first_time_flow_disabled`, `iefestivaldrive`, `grid_control` | ways | | demo, first-run and festival-drive flags; no traffic effect known |
| `deadend` | nodes | `true` | the road ends here (219 nodes) |
| `uturn` | nodes | a count | U-turns allowed at this node (probable) |
| `sat_nav_junction` | nodes | `roundabout` | roundabout node (11) |
| `ignore_turn_flags` | nodes | `true` | sat-nav turn hints off |
| `discovery_zone` | nodes | a zone id | road discovery for the map |

Drivable network: 426 roads, 238.1 km, 11,559 segments (median 20.3 m):

| `road_type` | km |
| --- | ---: |
| `a` | 90.8 |
| `b` | 90.0 |
| `dirt` | 27.2 |
| `freeway` | 24.9 |
| `none` | 2.7 |
| `shortcut` | 2.5 |

Traffic density ids on roads (ways that traffic may use):

| Density id | Ways | km | One-way ways |
| ---: | ---: | ---: | ---: |
| 2 | 106 | 48.8 | 4 |
| 3 | 54 | 44.5 | 4 |
| 4 | 47 | 24.9 | 37 |
| 5 | 17 | 24.8 | 0 |
| 6 | 17 | 8.6 | 0 |
| 7 | 13 | 15.8 | 5 |
| 10 | 1 | 0.6 | 0 |
| 12 | 15 | 5.3 | 2 |
| 13 | 3 | 7.0 | 0 |
| 14 | 8 | 4.8 | 0 |
| 15 | 2 | 1.4 | 0 |
| 16 | 2 | 0.5 | 0 |
| 17 | 20 | 5.7 | 20 |
| none or disabled | 121 | 45.4 | 2 |

The freeway is mostly one-way ways, one per carriageway; other roads are
two-way centre lines.

## State

Per traffic car: its model, its way and segment, the direction it drives
(with or against node order), the distance along the segment, its lateral
offset, its speed and target speed, and whether it is in full physics.

For the population, kept per player: a smoothed local density for traffic
and one for festival cars, the next density test time, the next spawn
time, and the set of loaded traffic models with the time of the next
model rotation.

## Rule

### Time-of-day factor

A factor v in [0, 1] scales every density between its minimum (v = 0)
and maximum (v = 1). With the hour h and four registry times (in hours,
`AI/Spawning/Density/…`): EndTimeAtMaxDensity h₁, StartTimeAtMinDensity
h₂, EndTimeAtMinDensity h₃ and StartTimeAtMaxDensity h₄, each taken
past the one before it by adding 24 h when it is smaller, and h itself
advanced by 24 h when it is below h₁:

```text
f = 1 − (h − h₁)/(h₂ − h₁)     for h₁ ≤ h < h₂   (evening fall)
f = 0                           for h₂ ≤ h < h₃   (night)
f = (h − h₃)/(h₄ − h₃)          for h₃ ≤ h ≤ h₄   (morning rise)
f = 1                           for h > h₄        (day)
v = (1 − cos(π f)) / 2
```

On the disc that is full density by day, a ramp down in the evening,
minimum density at night and a ramp up in the morning. Confidence:
confirmed by data and the guest code's arithmetic; matched in the oracle
by the noon and 02:00 counts (Tests).

### Density

Each `Density` entry of a `DensityList` (`cartype="traffic"` or
`"festival"`) gives `min` and `max`. A road's density at the hour is

```text
d = min + (max − min) · v
```

A way without a density id, or with an id the list lacks, takes a
default entry; that it is the list's first entry (id 1, which is zero)
is a guess, and an engine should give such ways no traffic. `AI/Traffic/Density/
UseOverrideValues` replaces every road's range by the registry's
`AI/Traffic/Density/MinTraffic` and `MaxTraffic` (festival: `MinFestival`,
`MaxFestival`); it is off on the disc.

The **local density** D is a sum of d over the roads around the
player: the game walks the road graph outwards from the player's way, in
both directions, and adds each way it reaches (confirmed). How far the
walk goes and whether a way counts once or by its length was not
decoded. Two readings fit the one oracle road (Tests): d as cars per
100 m of road within the spawn-out distance (6.2 and 0.6 cars predicted
for 6.1 and 0.9 observed at noon and 02:00), or d once per way within
about 500 m (5.0 and 0.5). The engine should use the per-100 m reading
until a second site separates them.

Every `AI/Spawning/Density/DensityTestInterval` seconds the local density
is measured again and smoothed:

```text
D_s ← D_measured + (D_s − D_measured) · DensityDecay
```

### Population target

```text
cap(h) = MinTraffic + (MaxTraffic − MinTraffic) · v
target = min(D_s, cap(h))
```

with `AI/Spawning/Density/MinTraffic` and `MaxTraffic` (festival:
`MinFestival`, `MaxFestival`). The cap is a whole-world ceiling; on the
oracle road the local density is always below it. Confidence: confirmed
for the formula, probable that target is compared with the number of
active traffic cars.

### Spawning and despawning

- **Initial spawn.** On entering free roam the game places
  `numInitialTrafficCars` cars at once (4; festival
  `numInitialFestivalCars`, 2), spread along the player's road. In the
  oracle they appeared at 52, 112, 195 and 289 m (registry
  `AI/Spawning/InitialSpawnMinDistance`, `InitialSpawnMaxCars`). A
  change of the hour does not run the initial placement again: after the
  switch from noon to 02:00 the six cars on the road drove out one by one
  (the last despawn 44 s later) with no spawn until the road was empty,
  and after the switch back to noon cars came one at a time.
- **Spawn.** While the active count is below the target, one car spawns
  on a road at the spawn-in distance from the player: the density
  entry's `spawnInDistance` when present (town ids 6 and 7), otherwise
  `AI/Spawning/SpawnInDistance` (330 m). Measured: 17 spawns at 317 to
  347 m. The time between spawns follows `AI/Spawning/Interval/Traffic*`
  (minimum, medium, maximum, variance; seconds) and `Interval/SpeedFactor`
  and `SpeedMax`. Measured with the player parked at noon: 4.8 to 9.4 s
  (mean 7.4 s, 8 intervals) whether 4 or 7 cars were active, so the
  interval does not visibly shorten towards `TrafficMin` when the count
  is low; cruising at noon it fell to 3.1 to 6.0 s as the player gained
  speed, so the player's speed shortens it (probable; the formula is not
  decoded). At 02:00 a car spawned only when the road was empty (0.2 and
  2.7 s after the last car left), consistent with spawning only while the
  active count is below a target under 1.
- **Count follows lifetime.** With a fixed interval the parked count is
  about the mean car lifetime over the interval. A car spawned towards the
  player crosses 330 m to him and 393 m past him, about 54 s at 13.4 m/s;
  one spawned driving away leaves in about 5 s. With 0.24 of spawns away,
  that is 0.76 × 54 + 0.24 × 5 = 42 s, over 7.4 s gives 5.7 cars,
  against 6.1 observed. The spawn direction matters as much as the rate.
- **Direction.** A spawned car drives towards the player or away from
  him; `spawnAwayChance` (per density id) and `AI/Spawning/
  SpawnAwayChanceMin`, `Max` and `LerpSpeed` set the chance it drives
  away. Measured: 5 of 21 spawns drove away from the player, most with
  him parked (close to `SpawnAwayChanceMin`), so the chance probably rises
  with the player's speed towards the road's value (probable). An engine
  should use `SpawnAwayChanceMin` while the player is still; using the
  road's value (0.6) shortens the mean lifetime to about 25 s.
- **Spacing.** No spawn within `AI/Spawning/MinDistanceFromOtherCars` of
  another car, scaled up to `MaxDistanceFromOtherCars` at
  `SpeedAtMaxDistanceFromOtherCars` (probable reading of the keys).
- **Despawn.** A car farther than the spawn-out distance
  (`spawnOutDistance` or `AI/Spawning/SpawnOutDistance`, 370 m) is
  removed. Measured: 13 despawns at 392 to 395 m, so the test runs on a
  slightly larger radius or at an interval (`AI/Traffic/VehicleManager/
  OverspawnCheckTime`, 4 s); an engine may despawn at 390 m.
- **Fade.** A new car fades in over `AI/Traffic/Vehicle/FadeInTime`
  between `MinFullAlphaDistance` and `MaxFullAlphaDistance` (probable).
- **Allowed roads.** A way with `traffic_disabled` or `ai_disabled`, or
  without a density id, gets no spawns. A density entry's
  `BannedVehicles` group (the `bus` group: the tour bus and the T440) may
  not spawn there; `AllowedVehicles` (dirt roads: the `4x4` group)
  restricts spawns to the group.

### Models

The `freeroam` `CarList` lists 17 traffic models by `Data_Car.Id`. At
most `maxLoadedTrafficModels` (4) are loaded at once, and every
`AI/Traffic/VehicleManager/ModelRotationTime` (30 s) the loaded set
changes; each spawn picks one of the loaded models. `maxactive="1"` with
`streamonly` and `largevehicle` (the tour bus and the T440 truck) allows
one of that model at a time. In the oracle every 30 s window held at most
four models (the Leaf, the Express van, the Taurus, the Prius and the PT
Cruiser seen over the run). How a model is picked within the loaded set
is not decoded: pick uniformly.

The 18th traffic row of the database, 1530 (RAV4 Sport), is in no
`AIOpenWorld.xml` car list on the base disc; it appears only in a
livery stress-test list and is not used by free roam.

### Driving a lane

- **Lane.** Traffic drives on the right. A car on a two-way road keeps
  about 2.2 m right of the way's centre line (median of 2,883 moving
  samples, 99 % on the right); on a one-way way it drives in node order
  (probable, see `oneway`).
  The file has no lane count or width: take one lane each way on two-way
  roads, centred 2.2 m from the centre line, and on one-way freeway
  carriageways the lanes offset the same way (lane count open).
- **Following the line.** The car tracks its lane line with smoothing
  (`AI/Traffic/VehiclePosition/Interpolation/…` gains for position,
  heading and lane offset, interpolated between a short car at
  `VehicleLengthAtMin` and a long one at `VehicleLengthAtMax`; limits on
  pitch and roll). Off the player's view this is a kinematic placement on
  the line.
- **Target speed.** The density entry's `speed`, in **mph**: measured
  13.1 to 13.7 m/s (median 13.3) on a road whose entry says 30 (30 mph
  is 13.4 m/s). Within the registry bounds `AI/Traffic/Speed/SpeedMin`
  and `SpeedMax` (m/s; 27 m/s is 60 mph, the freeway entries' value),
  reduced on bends between `RadiusForMinSpeed` and `RadiusForMaxSpeed`
  (m), with a small per-car variance.
- **Acceleration.** Limited by `AI/Traffic/Speed/AccelerationMax` and
  `DeccelerationMax` (m/s²) and their rates of change (m/s³).
- **Gap keeping.** A car looks ahead `CollisionAvoidance/LookAheadTime`
  seconds of travel (at least `MinLookAheadDistance`) and searches
  `TrafficCollisionSearchDistance` for traffic and
  `PlayerCollisionSearchDistance` for the player, scaled between
  `MinCollisionSearchDistance` and the search distance by speed
  (`SpeedAtMinCollisionSearchDistance`, `SpeedAtMaxCollisionSearchDistance`).
  It matches the speed of the car ahead (`DifferenceToCarInFrontSpeed`)
  and brakes for one in its path.
- **Junctions.** At a junction node a car picks a continuing way, may
  indicate (`VehicleControls/TimeBeforeIndicating` and related keys),
  slows to `Junctions/SpeedAtGivewayJunctions` where it gives way, and
  checks for crossing cars within `Junctions/MaxCollisionCheckDistance`
  and the `JunctionManoeuvreSafeTimeGap`. Which way it picks is not
  decoded: pick any way at the node that traffic may use and that a
  one-way does not forbid, never the one it came from.
- **Lane changes.** On multi-lane ways a car blocked for
  `LaneChangeBehavior/TimeBlockedBeforeLaneChange` changes lane with
  `LaneChangeChance`, no more often than `MinTimeBetweenLaneChanges`.
- **The player.** A car that sees the player in its path within the
  player search distance brakes, and steers aside by up to
  `Avoidance/MaxAvoidOffset` (m) when slower than `Avoidance/MaxAvoidSpeed`
  (probable reading of the keys; not measured). After a hit it may flash
  its headlights (`CollisionResponse/…`). A car knocked off its line
  becomes "dead": hazard lights after `DeadBehavior/HazardLightDelay`, and
  after `ReanimateDelay`, if nearly stopped and upright, it rejoins the
  nearest suitable segment.
- **Behaviour priority.** `AI/Traffic/BehaviorPriorities` orders the
  behaviours: dead above stop above junction above lane change above
  normal driving.

### Physics and level of detail

Every traffic car is a full car body in the physics world, with its
model's own physics and mass, but only some take physics steps. Each
simulation tick a body either takes the full dynamics step (the same
step as the player's car, [SPEC-CAR-PHYSICS](CAR_PHYSICS.md)) or is left
alone by the physics world: in every sample of active traffic not on the
full step, the body's step-path word was set and its light-step flag
clear, the state in which the physics world does nothing for the body.
Those cars are placed on their lane line kinematically by the traffic
system (the interpolation above), at the simulation tick.

Which cars get full physics (probable): on the cruise every full-step
body was within 28 m of the player and every other one 32 m or more
away, which matches `AI/Traffic/VehiclePosition/MaxHighLodDistance`
(30 m). While parked at noon, three cars also took the full step at up
to 238 m, each moving at 18 to 20 m/s instead of the road's 13.4 and one
slowing to 1.8 m/s: cars pushed off their line (festival cars passed at
30 to 50 m/s) stay in full physics until they rejoin it. Engine rule:
full physics within 30 m of the player and for a car knocked off its
line until it is back on it; kinematic otherwise.
`MaxMediumLodDistance` is the next level, probably for drawing.

A full-physics traffic car drives with the controls a driver would use:
throttle and brake inputs limited by `VehicleControls/AccelMaxInput` and
`BrakeMaxInput`, steering scaled by `SteeringMultiplier`, gears capped by
`GearMaxSpeed1st` to `4th` (m/s).

`AccelMaxInput` (0.2) is a cap, not a rate: in all 492 full-step samples
of traffic on its line the throttle input was exactly 0.20 (Leaf, Express
van, Taurus, Prius), never between 0 and 0.2. At that throttle the cars
held 13.3 to 13.6 m/s, and the Express van went from 10.4 to 13.4 m/s in
about 2 s. Meanwhile the engine speed rose slowly from idle (about 700 to
1,600 rpm over 5 s in third gear) at a constant road speed, so the
engine's torque is not what holds the car's speed when it enters full
physics: the traffic position controller (the interpolation gains above)
probably keeps pulling the body to its predicted place on the line
(probable). Engine rule: cap the throttle at 0.2, and hold a full-physics
traffic car at its lane speed with the same position and heading blend
the kinematic cars use, so a weak model (Prius, Leaf, Focus, Corolla)
still keeps 30 mph. Cars knocked off their line showed full brake (1.0)
and first gear until stopped.

### Collision with the player

Traffic cars are rigid bodies the player can push. Their body mass is the
model's stock `List_UpgradeCarBodyWeight.Mass`, as for the player's car:
the oracle read 15.24, 26.66, 19.813, 13.798 and 14.08 (100 kg units) for
the Leaf, the Express van, the Taurus, the Prius and the PT Cruiser,
which are those rows' masses. The disc's traffic masses run from 1,045 kg
(Corolla DX) to 5,000 kg (the tour bus and the T440). `PhysicsSettings.ini`
section `Collision` holds `CollisionBiasTraffic` and `TrafficTorqueScale`
(and `RaceCarTorqueScaleVsTrafficInRace` for races): a bias in how a
car-to-traffic impulse is shared and a scale on the torque a contact puts
on the traffic car (probable reading of the names; not measured). Until
measured: resolve contacts as between two ordinary cars, scale the
angular part of the impulse on the traffic car by `TrafficTorqueScale`,
and leave `CollisionBiasTraffic` at an even share.

## Rates and timing

- Population: density test every `DensityTestInterval`, spawn attempts
  on the interval rule, despawn checks within a few seconds.
- Driving: every simulation tick (8.33 ms at 120 fps on the desktop),
  full step or light step per body. Path prediction runs every
  `AI/Traffic/PathPrediction/UpdateIntervalFrames` frames.
- Model rotation every 30 s; time of day from the world clock.

## Unknowns

| Unknown | Confidence of the current reading | What to do meanwhile |
| --- | --- | --- |
| Exact extent and weighting of the local density walk | the per-100 m reading fits the noon and night counts on one road | sum d per 100 m of traffic road within the spawn-out distance |
| Lane count and width per road type | 2.2 m right of the centre line measured on one road type | one lane each way at 2.2 m; one-way ways the same |
| Junction way choice and give-way rules | names only | uniform choice among allowed ways, slow to give way at junction nodes with three or more road segments |
| Local density walk: extent, per way or per length | two readings fit one road | per 100 m within the spawn-out distance |
| One-way direction | consistent with the data, not driven | node order |
| Model pick weights | at most four loaded, rotation 30 s | uniform among the loaded set, respecting `maxactive` and the density's groups |
| Reaction to the player (brake, swerve) | key names | brake to keep the look-ahead gap, no swerve |
| Despawn radius above 370 m | measured 392 to 395 m | despawn at 390 m |
| Collision bias and torque scale | key names | as under "Collision with the player" |
| Freeway and night density, festival cars | not measured on this route | apply the same rules with their entries |
| What `uturn` counts | name | ignore |

## Tolerances

- Road choice: every traffic car within 15 m of a road segment whose way
  allows traffic (the oracle: 2,883 moving samples of driver type 4, none
  farther than 15 m from a road).
- Direction: on one-way ways, node order; on two-way roads, right
  of the centre line in at least 95 % of samples, offset 2.2 ± 0.5 m.
- Density: the mean active count over a minute within 30 % of the
  oracle's at the same place and hour.
- Spawn and despawn distances within 30 m of the measured ones; speed
  within 1 m/s of the road's `speed` on straight road.

## Tests

From the player's disc:

- `colorado.nav` decodes to the header counts above and its last word,
  every node-way reference points back at its node, and the drivable
  network is 426 roads and 238.1 km.
- Traffic density ids on roads add up to the table above (for example
  density id 5: 17 ways, 24.8 km; freeway id 4: 47 ways, 24.9 km, 37 of
  them one-way).
- v = 1 at noon and v = 0 at 02:00 with the disc's times; v = ½ halfway
  through the morning ramp.

Oracle cruise (`config/render-tests/fh1-traffic-census.fh1test` on the
`acc9-stock` seed, base game, read with `tools/re/traffic.py census`):
the player parked at (2002.9, 143.8, 960.0) on a density-5 two-way road,
60 s at noon, 60 s at 02:00, then 30 s at part throttle (313 m driven).
Traffic counts are cars of driver type 4 within 1 km of the player
(pooled inactive cars wait about 2.5 km away and are not counted):

| Phase | Active traffic (mean, min, max) | Festival cars (mean) |
| --- | --- | --- |
| noon, parked | 6.1, 4, 8 | 0.8 |
| 02:00, parked (from 30 s after the change; the noon cars were still leaving until 44 s) | 0.9, 0, 2 | 1.7 |
| 02:00, parked, after the last noon car left (16 s) | 0.8, 0, 1 | |
| noon, cruising | 4.4, 2, 8 | 1.0 |

Density check at noon: within 370 m of the player the network holds
746 m of density-5 road and 68 m of density-2 road. At v = 1 the
per-100 m reading (density 5's range is 0.08 to 0.8, density 2's 0.04
to 0.4) predicts 0.8 × 7.46 + 0.4 × 0.68 = 6.2 cars against
6.1 observed; at 02:00 (v = 0) it predicts 0.6 against 0.9 observed.
Within 500 m the network has five density-5 ways, one of id 3 and one
of id 2, so the once-per-way reading predicts 5.0 and 0.5. Spawns at 317
to 347 m (17, after an initial spawn of 4 cars at 52 to 289 m), 5 of the
21 driving away from the player, despawns at 392 to 395 m (13), speeds
13.1 to 13.7 m/s (median 13.4), all 2,883 moving samples within 15 m of
a road, on density-5 ways (21 on a shortcut beside them), 99 % right of
the centre line at a median 2.2 m. Steps: no active traffic body took the
light step; off the full step, every one was left unstepped.
