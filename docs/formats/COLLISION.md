# Collision: Colorado's ground, walls and surfaces for the M3 drive

| | |
| --- | --- |
| Spec ID | `FORMAT-COLLISION` |
| Status | draft for M3: the static collision mesh, its tiling and the per-triangle surface index are confirmed against the physics oracle; the surface table's columns are named but how the tyre model uses them is open (the tyre-model spec, item 5 of the engine's needs); prop collision shapes and a few per-triangle fields are not decoded |
| Written | 2026-11-19, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | `site/findings/world.toml` groups "Collision and cameras" and "Placements and routes"; `site/findings/objects.toml` group "Car dynamics" (the wheel blocks the check reads) |
| Implements it | `shift-engine` (not yet), for plan item M3 |

Our own reading of the player's files, checked against the RF-5 physics
oracle's traces of the player's car. No disassembly, decompiled code or
game data is reproduced here ([LEGAL.md](../LEGAL.md)): values are names,
counts and our own measurements. The reader is
[`tools/re/collision.py`](../../tools/re/collision.py).

Priority (user, 2026-11-20): playable first. This spec gives what the
engine needs to drive one car around Colorado on the game's own collision;
fields not needed for that are listed as unknown, with a default.

## Summary

- The car's ground and walls are the **`N.fiz` members of Colorado's
  `bin.zip`**: 3,201 tiles, each a triangle soup with a surface index on
  every triangle and a 10 m grid of triangle lists. They are not the
  render models (WORLD-GEOMETRY): the `.fiz` triangles are a separate,
  coarser mesh. All 3,201 parse to their stated sizes: 1,729,941
  vertices and 2,007,595 triangles, of which 1,401,862 belong to a tile's
  own 100 m square (the rest are the 8 m aprons shared with neighbours).
- **Confirmed by the oracle:** in four oracle traces of the player's car
  (6,711 wheel-in-contact samples), the triangle each wheel's block holds
  as its contact triangle is, vertex for vertex, a `.fiz` triangle, and
  that triangle's surface byte equals the wheel's surface index every
  time (6,711 of 6,711). The wheel hub sits a rolling radius above the
  triangle's plane to a median within 1 cm on every wheel.
- **Frame:** the game frame, the one `CollObjs.xml`, `TrackRoute*.xml` and
  the car body use. That is WORLD-GEOMETRY's render file frame with z
  negated. Units are metres, y is up.
- **Tiling:** a 133 × 100 lattice of 100 m cells; a tile exists for 3,201
  of the 13,300 cells, numbered in row-major order (z row, then x).
  `Ribbon_00/Colorado_track_00.col` is the index of that lattice.
- **Surfaces:** the triangle's surface byte is a 0-based index into the
  `SurfaceTypesList` of `physics.zip` `surfaceTypes.xml` (68 names on the
  disc; v4 appends four). The same file defines each surface's friction,
  drag, bumpiness, springiness and effects, by name.
- **Props:** fixed walls, guard rails, concrete barriers and invisible
  edge walls are triangles in the tiles. The 21,877 movable or breakable
  props of `CollObjs.xml` (signs, fences, cones, tables, bins) carry their
  own physics shapes, which are not decoded; M3 can leave them out.
- **Spawn:** the reference seed's free-roam start, measured from a run,
  on asphalt, with the ground under each wheel checked; and a route start
  on asphalt 93 m from the festival, read from a route file.

## Files

| File | Where | What | Confidence | How |
| --- | --- | --- | --- | --- |
| `N.fiz`, N = 0 to 3200 | `media/tracks/colorado/bin.zip` (117 MB extracted) | collision tiles (this page) | confirmed | every tile parsed; oracle contact triangles found in them |
| `Colorado_track_00.col` | `media/tracks/colorado/Ribbon_00/`, loose | index of the collision tiles: the lattice and one record a tile | confirmed | its records agree with all 3,201 tiles |
| `surfaceTypes.xml` | `media/physics.zip` (v4: `media.zip` `physics/surfacetypes.xml`, read instead) | the surface list, categories, per-surface parameters and per-category-pair contact parameters | confirmed (list order); probable (columns' roles, by name) | the oracle's surface index against the list; reading the file |
| `NatalSurfaceTypes.xml` | `media/physics.zip` | a second surface file of the same kind; by its name, for the Kinect ("Natal") mode | guess | name |
| `CollObjects.xml` | `media/physics.zip` | 31 generic collision-object types (cones, poles, balls, boxes): sphere and block shapes, masses, a `SurfaceId` | probable | reading the file |
| `PhysicsSettings.ini`, `PI.xml`, `SecondaryTypes.xml`, `SmashableObjectTypes.xml` | `media/physics.zip` | global physics settings and type lists, not read for M3 | — | names |
| `PhysicsDefinitions.bin` | `media/tracks/colorado/`, loose (also in the UI tracks) | binary physics definitions for the track's collision objects (the executable has the class names `CPhysicsDefinitionList` and `CCollObjPhysicsDefinition`); layout not decoded | guess | class names, a count-like first word |
| `CollObjs.xml` | `media/tracks/colorado/Ribbon_00/` | placements of 21,877 collidable props, each a `PhysicsType` (`CO_<name>.<part>.rmb`, 180 names, 758 with the part number) with a position and three axis vectors | confirmed | WORLD-GEOMETRY; reading the file |

The disc has no `.col` per tile and no collision inside the render
models. The one `.col` file is the index above.

## Coordinate frame and units

- Positions are the **game frame** in metres, y up. A tile's ground under
  the car's position from the oracle (body position, same axes, same
  signs) lies within 0.5 m below it; with z negated the point lands on
  another tile 43 m lower. Route start positions and `CollObjs.xml`
  placements are in the same frame: in a sample of 314 route starts, the
  242 over a tile triangle sit a median 0.54 m above it (5th to 95th
  percentile 0.29 to 1.15 m).
- WORLD-GEOMETRY's render models are in the **file frame**, the game frame
  with z negated. An engine that keeps one world frame negates z when it
  reads either the tiles or the render models, and negating z flips the
  triangle winding (see the normal rule below).
- The car body's physics frame, measured on a stock R34 from the oracle:
  the body's three rotation rows are its x, y and z axes in the game
  frame, +z forward (the rear axle to the front), +y up, x = y × z. The
  four wheel hubs sit at body x ±0.77 m, y −0.15 m, z +1.146 m (front) and
  −1.519 m (rear), so the body's reference point is not FORMAT-CARS' body
  origin (that one lies on the ground under the car). Wheels 0 and 2 are
  at −x, which is the left side if this frame is FORMAT-CARS' body frame
  (left at −x, +z forward; probable).

## Tiling and loading

**Lattice.** Cell (i, j), i = 0 to 132 and j = 0 to 99, covers x from
X0 + 100 i to X0 + 100 (i + 1) and z from Z0 + 100 j to Z0 + 100 (j + 1)
in the game frame. X0 and Z0 are the two floats the `.col` header gives
(below; about −6,409 and −6,133). A tile's box is its cell widened by an
8 m apron on all four sides, 116 m square. The tile number is the rank of
its cell in (j, i) order among the cells that have a tile: tile 0 is the
first occupied cell of row j = 0. Confirmed for all 3,201 tiles.

**Aprons.** A triangle that reaches into a neighbour's apron is stored in
both tiles, with the same vertices. To get each triangle once, keep a
tile's triangle when the centroid of its three vertices (x and z) lies in
the tile's own cell (lower bounds inclusive, upper exclusive). That keeps
1,401,862 triangles, 1,399,504 distinct by their three vertices (the
other 2,358 are repeats inside a tile); no triangle is lost, since every
centroid falls in a cell that has a tile. Vertices may lie outside the
box (161,489 do): triangles are whole, not clipped.

**Loading around a point.** For the M3 drive, load every tile whose box
overlaps a square of a few hundred metres around the car and drop the
apron repeats as above (or keep them: repeats are harmless to ray and
shape casts). How the game itself streams tiles is not traced; the
`.col` record order and offsets look like a streaming layout.

**The `.col` index.** All words are big-endian unless noted.

| Offset | Size | Field | Confidence | How |
| --- | --- | --- | --- | --- |
| 0 | u32 × 2 | lattice width and height: 133, 100 | confirmed | tile cells |
| 8 | f32 × 2 | lattice origin X0, Z0 | confirmed | every tile box is origin + 100 × cell − 8 |
| 16 | f32 × 2 | a far corner, short of the lattice's own (a world bound, guess) | guess | values |
| 24 | f32 × 2 | 100, 100 stored little-endian | probable | bytes |
| 32 | f32 × 2 | 0.01, 0.01: the inverse cell size | confirmed | value |
| 40 | u32 | end of the record stream (the last record's offset plus its size) | confirmed | sum |
| 44 | u32 × 2 | 4, then a constant marker word | constant | |
| 52 | f32 × 4 | 100, 100 (cell size), 8, 8 (apron) | confirmed | tile boxes |
| 68 | f32 × 2 | 10, 10 (the tiles' grid cell) stored little-endian | probable | bytes |
| 76 | u32 | number of records: 3,201 | confirmed | |
| 80 | 24 bytes × count | records: a word (non-zero only in record 0), a stream offset, the tile number, the cell index j × 133 + i, the stream offset again, and the size of the tile's first block | confirmed (number, cell, size, offsets contiguous from the `.col`'s own length) | all 3,201 against the tiles |

## Tile layout (`N.fiz`)

All values are big-endian. A tile is a 32-byte file header, block A (the
collision mesh) and, in 2,212 tiles, block B.

### File header

| Offset | Size | Field | Confidence |
| --- | --- | --- | --- |
| 0 | 4 bytes | `fiz ` | confirmed |
| 4 | u32 | version, 1 in every tile | constant |
| 8 | u32 | size of block A | confirmed: header + A + B is the file size in all tiles |
| 12 | u32 | size of block B (0 in 989 tiles) | confirmed |
| 16 | u32 | block B's record count (equals the count in B's own header) | confirmed |
| 20 | 12 bytes | zero | constant |

### Block A: the mesh

Block A starts at file offset 32. **Offsets inside block A are from its
start.**

| Offset | Size | Field | Confidence | How |
| --- | --- | --- | --- | --- |
| 0 | u32 | vertex count | confirmed | references never exceed it |
| 4 | u32 | triangle count | confirmed | grid lists never exceed it |
| 8 | u32 | offset of the vertices (144 in every tile) | confirmed | |
| 12 | u32 | offset of the triangles (the vertices' end) | confirmed | all tiles |
| 16 | u32 × 2 | grid width and height in cells: 12 × 12 in every tile | confirmed | |
| 24 | f32 × 4 | the tile box: x min, z min, x max, z max | confirmed | the lattice |
| 40 | f32 × 2 | grid cell size in x and z: 10 m | confirmed | |
| 48 | f32 × 2 | their inverses | confirmed | |
| 56 | u32 | offset of the grid table (the triangles' end) | confirmed | all tiles |
| 60 | 64 bytes | mostly zero; small 16-bit pairs in some tiles and a small count at 120 in about a third | unknown | |
| 124 | u32 | top bit set, in every tile | constant | |

**Vertex**, 16 bytes: x, y, z as f32 (game frame, metres; confirmed),
then a u32 that is not decoded (it looks like four small signed bytes).

**Triangle**, 32 bytes:

| Offset | Size | Field | Confidence | How |
| --- | --- | --- | --- | --- |
| 0 | u8 | flags; bit 0 is never set, bit 3 is set on all but 46 triangles, and bits 1 and 2 are set mostly on up-facing ones | unknown (bit pattern counted) | counts |
| 1 | u8 | **surface index** into `SurfaceTypesList` | confirmed | oracle wheels, 6,711 of 6,711 |
| 2 | u16 | 0x8000 on 92 % of triangles, otherwise a small number, mostly on walls | unknown | counts |
| 4 | u32 × 3 | the three corners, as byte offsets into block A: the vertex index is (offset − vertex offset) / 16 | confirmed | every reference lands on a vertex start |
| 16 | f32 × 3 | unit normal | confirmed | |
| 28 | u32 | 0x00FFFFFF on 89 % of triangles; three bytes per edge, perhaps neighbour or edge flags | guess | |

**Normal rule:** with corners a, b, c in the order stored, the normal is
(b − a) × (c − a) normalised, in the game frame (all 2,007,593
non-degenerate triangles; one is degenerate). After negating z the same
corners give the opposite cross product, so swap two corners or negate
the normal.

**Grid table:** 144 u32 offsets, one per cell, row-major (z row, then x);
cell (gx, gz) covers x from box x min + 10 gx and z from box z min + 10 gz.
An offset of 0 is an empty cell; otherwise it points (within block A) at a
u16 count followed by that many u16 triangle indices of the triangles that
touch the cell. Every triangle is in at least one list. A point query
takes the tiles whose box holds the point, the cell under it, and tests
that cell's triangles; it gave the same answers as testing every triangle
of the tile at the points tried.

### Block B

`OWBB`, a header of its own and records that look like a linked graph
(16-bit index pairs). Not collision geometry: no vertex of it was used.
Skip it by its size (unknown; guess: road or AI data).

## Surface types

**Per triangle:** byte 1 of the triangle record. **Per wheel** the game
keeps the index of the triangle it stands on (the oracle's wheel blocks),
so a wheel's surface is the surface of its contact triangle.

**The list.** `surfaceTypes.xml` (in `physics.zip`) has a
`SurfaceTypesList` whose `SurfaceType value="<name>"` children, in order,
are indices 0, 1, 2, and so on. 68 on the disc. The v4 file is the same
with four appended (`RallyGrass`, `Dirt2`, `Dirt3`, `RallyGravel`, 68 to
71); indices 0 to 67 do not change. The index-to-name mapping is
confirmed for 0 (`Asphalt`) and 2 (`Grass`), the indices the oracle drove
on; for the rest it is probable: walls carry wall surfaces, the
`CollObjects.xml` cone's `SurfaceId` names `RubberCone`, and the up-facing
share of each index fits its name (roads and ground up, rails and walls
sideways).

Indices the tiles use (triangles, and how many face up: normal y over 0.7):

| Index | Name | Category | Triangles (up-facing) |
| --- | --- | --- | --- |
| 0 | `Asphalt` | HardWorld | 507,494 (333,715) |
| 1 | `Dirt` | SoftWorld | 675,593 (647,432) |
| 2 | `Grass` | SoftWorld | 351,735 (343,695) |
| 3 | `GuardRail` | GuardRail | 145,547 (293) |
| 4 | `ConcreteBarrier` | HardWorld | 30,560 (2,620) |
| 5 | `Asphault2` | HardWorld | 89,648 (78,563) |
| 6 | `RumbleStrip` | RumbleStrip | 2,085 (2,085) |
| 9 | `Invisible` | HardWorld | 39,572 (165) |
| 10 | `Brick` | HardWorld | 13,405 (6,559) |
| 15 | `Wood` | HardWorld | 18,931 (303) |
| 23 | `Gravel` | SoftWorld | 8,107 (8,107) |
| 25 | `SlowGrass` | SoftWorld | 15,666 (12,205) |
| 28 | `Concrete` | HardWorld | 38,396 (33,823) |
| 29 | `Concrete2` | HardWorld | 14,975 (14,893) |
| 41 | `BumpyAsphaltExtreme` | HardWorld | 6,915 (6,915) |
| 44 | `BumpyConcreteExtreme` | HardWorld | 27,992 (293) |
| 57 | `Trackway` | HardWorld | 3,153 (3,153) |

The other 33 indices in use have under 5,000 triangles each (50 indices
in all, the largest 62). `tools/re/collision.py stats` prints them all.
Indices 58 (`Kerb`) and 60 and up (the `Smashable*` types) are not on any
tile triangle except 61 (one) and 62 (126).

**Name quirk.** Definitions are matched to list names by element name. One
list name, `Kerb` (58), has no definition, and one definition,
`BowlingPin`, has no list entry; no triangle uses 58. Engine: give an
index without a definition the defaults of `Asphalt` and log it.

**Per-surface parameters.** `<SurfaceTypes>` holds one element per
surface, named after it, with a `Category` attribute and these children
(each a `value` attribute):

| Group | Columns | Units and role |
| --- | --- | --- |
| `Friction` | `FrictionScale`, `HeatTireScale`, `OffRoadness`, `OffRoadWetFrictionScale`, `OffRoadDryPeakSA`, `OffRoadWetPeakSA`, `RearGripMultiplier`, `MinimumArcadeGripValue`, `HandbrakeGripMultiplier`, the `TCS*` multipliers, `TCandStabilitySpeedMultiplier` | dimensionless scales; the two `PeakSA` look like peak slip angles in degrees. `FrictionScale` multiplying the tyre's grip is a guess from its name |
| `Friction/Offroad` | `ChaseCamSpringMultiplier`, `MaxSteeringLockCap`, `SteeringRateMultiplier`, `SteeringMaxGeesMultiplier` | off-road handling and camera scales |
| `Friction/VelDepFriction` | `VelPeak0`, `VelPeak`, `LateralCoeff0`, `LateralCoeff`, `LongCoeff0`, `LongCoeff`, `TorqueVel0`, `TorqueVel1`, `TorqueLateralCoeff0/1`, `TorqueLongitudinalCoeff0/1`, `LatTorqueScaleSlipAngle0/1`, `LatTorqueScale0/1` | speed-dependent drag coefficients, the speeds probably m/s; a candidate for rolling resistance (guess) |
| `Friction/CarWorldCollFricMods` | `CarUpAxisY0`, `CarUpAxisY1`, `FrictionScale0`, `FrictionScale1` | friction scale for the car body against the world, by how upright the car is |
| `Aerodynamics` | `LinearDrag`, `AngularDrag` | drag on the car while on the surface (guess) |
| `Bumpiness` | `NoiseType`, `Frequency`, `ClampMin`, `ClampMax`, `Scale`, `Offset`, `Rumble` | a height-noise model for rough surfaces |
| `Springiness` | `IsSpringy`, `SpringK`, `SpringCompD`, `SpringRebD`, `NormDepSpringNormYCutoff`, `NormDepSpringSpeedCutoff` | soft-ground spring |
| `SteerTorque` | `FrictionTorqueScale` and the `SurfaceNoise*` columns | steering-wheel force feedback |
| `SkidData`, `DebugColor` | colours, `TextureIndex`, `MinIntensity`, `SmokeType` | skid marks and smoke |
| (top level) | `ShouldSpark`, `CarDamageScalar`, `IsPenaltySurface`, `IsHandOfGodTriggerSurface` | flags and a damage scale |

Some surfaces leave groups out (`CameraCollision`, `BowlingPin` and the
`Smashable*` types lack parts of `Aerodynamics` and `Bumpiness`; the
smashable ones add `SmokeType`). Read missing columns as `Asphalt`'s.

**Per category pair.** `<SurfaceCategories>` lists 13 categories (`Car`,
`Cone`, `Tire`, `HardWorld`, `SoftWorld`, `RumbleStrip`, `TireWall`,
`GuardRail`, `GenCollObj`, `Pylon`, `SoccerBall`, `NoCollision`,
`Smashable`). `<CategoryToCategoryParams>` has one `Node` per pair
(`Category0`, `Category1`) with `FrictionCoefficient`,
`LowSpeedFrictionCoefficient` (some rows), `Elasticity` and `FrictionNoise*`
columns, and a `Default`/`Default` row. The `Car` rows are the car body
sliding on or hitting the world; the `Tire` rows the tyre against each
category. Elasticity is negative in many rows; its sign convention is not
known (guess: negative means the default restitution is replaced).

**For M3 (engine's choice until the tyre-model spec):** grip = the car's
tyre grip × `FrictionScale`; body against walls: the `Car`-row
`FrictionCoefficient` and a small restitution. Treat `Invisible` (9) as a
solid wall (its triangles are nearly all vertical: they bound the drivable
world) and `CameraCollision` (49, 46 triangles) as not touching the car.
No surface is water; what water does to the car is not traced.

## Props

- **Fixed barriers are in the tiles.** Guard rails, concrete barriers,
  wood and wire fences are triangles with those surfaces, nearly all
  facing sideways (see the table). The car collides with them through the
  mesh.
- **Movable and breakable props** are the 21,877 placements of
  `CollObjs.xml` (the most common types: fence sections, festival signs,
  marker poles, speed signs, picnic tables, benches, bins, cones, wooden
  barriers, armco arrows). Their physics shapes are not in the tiles or
  the render models. For 31 generic types `CollObjects.xml` gives spheres
  and blocks (`Collision` with `SphereN` radius and position, and
  `MassDistribution`); the track's own `CO_*` types are presumably the
  records of `PhysicsDefinitions.bin`, which is not decoded.
  `SmashableObjectTypes.xml` names the smashable kinds (`Fence`, `Flyer`,
  `Mailbox`, `RaceActivation`). Trees and the chain-link fence from
  `.pgeo` are not in `CollObjs.xml`.
- **For M3:** leave the `CollObjs.xml` props out (the car passes through
  signs and fences), or as an engine choice give each a static box from its
  render model's bounds. Neither blocks the drive.

## Spawn point

**Primary, measured** (reference seed's free-roam start, the place of
WORLD-GEOMETRY's free-roam frame dump and of the RI-0.5 oracle runs): the
player's stock R34 spawned and settled at rest on `Asphalt`:

| | Game frame |
| --- | --- |
| Body reference point at rest | (2002.96, 143.84, 960.00) m |
| Forward (rear axle to front axle) | (0.8072, −0.0464, −0.5884) |
| Heading | 126.1° from +z towards +x, about the y axis |
| Ground under the hubs | 143.30 to 143.45 m, surface 0, all four wheels in contact |

The car is dropped onto the ground there: it appeared 0.91 m above the
tile's ground (the body reference point) with its wheels off the ground
and settled to 0.48 m above it. With the R34's hubs 0.15 m below the body
reference point, the hubs then sit a rolling radius (0.32 m) over the
ground.

**Secondary, from data** (not run): `Ribbon_00/TrackRoute021.xml`
`start_location_06`, 93 m from the festival objects (`festival_01` to
`_06` in `GameObjs.xml`), on `Asphalt` with the tile ground 0.58 m below
its position. A route `Transform` has `pos.x/y/z` and a horizontal unit
`facing.x/z`, in the game frame. `TrackRoute110.xml` `start_location_00`
(364 m away, 0.52 m above asphalt) is a second choice.

**Placing a car (engine rule):** cast down from the spawn point to the
tiles, put the body at its rest height over that ground (each hub a
rolling radius above it, or the body reference point about 0.5 m above
the ground for a car like the R34), face along the stated forward with y
up, and let the physics settle it. A route position is already about
0.5 m over the ground.

## Evidence

- **Contact triangles (confirmed).** Each wheel block of the player's
  body holds three float3 vertices of the triangle the wheel stands on,
  its normal, the hub position and the surface index (offsets in the
  "Car dynamics" findings). Over the four RI-0.5 oracle traces (one
  spawn and three drives on asphalt and grass, 6,711 samples of a wheel
  in contact), every held triangle equals a `.fiz` triangle bit for bit,
  and its surface byte equals the wheel's surface index every time.
- **Ground height (measured).** The hub's distance to that triangle's
  plane minus the rolling radius (0.323 m front, 0.319 m rear) is, per
  wheel: median −8.7, −8.6, −2.2 and −1.1 mm; 5th to 95th percentiles
  −32 to +24 mm; range −59 to +40 mm. The spread includes tyre deflection,
  camber and the hub's vertical offset on slopes, so it measures the
  wheel model, not the collision; the collision itself matches exactly
  (the triangle identity above). In 418 samples the hub lies just past its
  triangle's edge in x and z: the contact point is not straight under the
  hub.
- **Frame.** The ground under the body is 0.4 to 0.9 m below it with the
  frame as read; with z negated it is 43 m off.

## Unknowns and what to do

| Unknown | Engine default |
| --- | --- |
| Triangle flag byte, the 16-bit word, the tail word, the vertex's fourth word, block A bytes 60 to 123 | ignore; collide with every triangle |
| Block B (`OWBB`) | skip it by its size |
| How the game streams tiles | load by box around the car (above) |
| How the tyre model uses the surface columns | `FrictionScale` as a grip multiplier until the tyre-model spec |
| Elasticity sign convention | a small positive restitution against walls |
| Prop collision shapes (`PhysicsDefinitions.bin`, `CO_*`) | leave props out, or boxes from render bounds |
| Water, the world's edge beyond the `Invisible` walls | none; respawn the car if it falls below the lowest tile vertex (about −46 m) |
| A list index with no definition | `Asphalt`'s parameters, logged |
| Which side wheel 0 is | left (probable, FORMAT-CARS' body frame) |

## Tolerances

The collision is exact data: an engine reading must reproduce the tile
triangles bit for bit. The plan's M3-era gate "ground height under four
wheels within 1 cm of the oracle" is met in the sense that the contact
triangle is identical; a hub-height comparison carries the wheel model's
own ±3 cm and belongs to the physics gate, not to this format.

## Tests

Counts over the player's base disc (`tools/re/collision.py stats`):

- 3,201 tiles, all version 1, each with a 12 × 12 grid of 10 m cells and a
  116 m box; header + A + B equals the file size in all; 2,212 have
  block B.
- 1,729,941 vertices, 2,007,595 triangles; 1,401,862 core triangles
  (1,399,504 distinct by vertices); every triangle in a grid list; no
  vertex reference out of range.
- Tile numbers are the (j, i) rank of their cells, i from 0 to 132, j from
  0 to 99; the `.col` index has 3,201 records whose tile number, cell
  index and first-block size agree with the tiles, and whose offsets are
  contiguous.
- The stored normal equals (b − a) × (c − a) normalised on all 2,007,593
  non-degenerate triangles.
- The surface counts in the table above; 50 distinct indices in use, the
  largest 62.
- `ground` at the measured spawn's x and z (game frame) finds one
  up-facing `Asphalt` triangle 0.48 m under the settled body reference
  point (it lies in two tiles' boxes, so it is listed twice).
- `oracle-check` over the four RI-0.5 traces: 6,711 of 6,711 contact
  triangles found, 6,711 surface indices equal.

## Reproduce

```text
python -I tools/re/world-geometry.py extract \
    .local/game/base/media/tracks/colorado/bin.zip .local/re/world/all
python -I tools/re/collision.py stats .local/re/world/all \
    --col .local/game/base/media/tracks/colorado/Ribbon_00/Colorado_track_00.col
python -I tools/re/collision.py ground .local/re/world/all 2002.96 960.00
python -I tools/re/collision.py oracle-check .local/re/world/all \
    .local/runs/ri05-aa1/physics-oracle.bin .local/runs/ri05-aa2/physics-oracle.bin \
    .local/runs/ri05-aa3/physics-oracle.bin .local/runs/ri05-aa4/physics-oracle.bin
```

`physics.zip` members are LZX-compressed; extract them with
`pinyon_shift_fh1_archive_extract --archive-all`. The traces come from the
`fh1-physics-inject-aa` route with `pinyon_shift_physics_oracle` on
(REIMPLEMENTATION_PLAN RI-0.5).
