# M3 drive: what the engine has and what it needs

Plan item M3 (REIMPLEMENTATION_PLAN): one car driven on Colorado's
collision with the engine's own physics, a HUD speedometer and engine
sound, gated by `tools/physics-behaviour.py compare` against the oracle
(0-100 and a lap within 2 %) and a play test. This note lists what
`shift-engine` already does and the specs it needs from the specification
side, in the order the work would use them. Written 2026-11-19, after M1's
wheel fit (FORMAT-CARS f193075f).

## What the engine has

- **Files:** the FH1 ZIP reader with XMem LZX, `.xds` textures, `.carbin`
  models, and the merged game database (disc, v4 and the 21 packages)
  with every `Data_Car` row the showroom uses.
- **Cars:** `carbake` bakes all 240 cars in under a minute, byte for byte
  the same each time: models, textures, paint and stripes, and per car the
  wheel sizes, the model-to-body pose (lift and pitch from the stock ride
  heights) and the four wheel centres in the body and model frames. The
  showroom draws any car with its wheels as separate transforms, so a
  driven car only needs per-wheel spin, steer and suspension travel.
- **World:** all 14,291 Colorado render models parse (WORLD-GEOMETRY), the
  props and their placements resolve (174,803 tile instances, 1,027,743
  zone placements), and `carview --world` draws the world-space models in
  a rectangle, headless, at a frame dump's camera. (When this note was
  written there was no streaming and no collision; the stand-ins below
  have added both.)
- **Renderer and platform:** Vulkan 1.3 with dynamic rendering, SDL3
  windows (SDL3 also gives gamepads and keyboard input), a headless PNG
  mode for checks.
- **Not yet (when written):** a physics module, an audio path, a HUD, a
  game loop with a fixed step. The physics module and the fixed-step loop
  now exist as stand-ins (below); audio and a HUD beyond the window
  title's speedometer do not.

## What the engine would build without new specs

- A fixed-step loop (1/120 s, the step RI-0.5 found deterministic in the
  oracle) with rendering interpolated between steps.
- Loading the render models and collision around one spawn point (a few
  hundred metres) instead of M2's full streaming.
- A rigid body with raycast or shape-cast wheels against a static BVH of
  the collision triangles.
- A chase camera, keyboard and gamepad input through SDL3, a text
  speedometer.
- A trace writer for the physics gate, once its format is known (below).

## Stand-ins built so far (2026-11-20)

Built at the spec side's request ("playable first") while COLLISION.md and
CAR_PHYSICS.md are written. Every one is ours, marked as a placeholder in
the code, and meant to be swapped out:

- **Physics** (`libs/drive`): since CAR_PHYSICS.md (a76cfa08) no longer a
  stand-in. A rigid body stepped at CarPhysicsUpdateHz (359 Hz substeps),
  raycast suspension with the stock springs, dampers, bump stops and
  anti-roll bars, the multi-curve tyre with the sidewall factor, the
  engine's torque curve, clutch and gearbox with shift phases, ABS, and
  the brake input curve and controller delays. A car's rows come from
  `carbake --physics-only` (`cars/<key>.physics.txt`); without them the
  synthetic car is used. `drive --physics-trace` writes the gate's JSONL.
  The clutch and gearbox follow the spec's state machine (launch
  controller, shift states, reverse selection under braking) since
  398228b8, with the measured engine and clutch fits of 67371030.
  Scenario results and the open questions: SPEC_QUESTIONS Q20 to Q29
  and Q33-Q34 (braking load transfer; the reverse launch).
- **Ground** (`drive::MeshGround`): a static BVH over triangles behind
  the `drive::Ground` interface. Since COLLISION.md it holds the game's
  collision (`world::Collision`): the `.fiz` tiles whose 100 m cell lies
  within 350 m of the streaming centre, core triangles only, with a
  surface index per triangle and grip = `FrictionScale` (COLLISION.md's
  engine choice until the tyre model). CameraCollision (49) is left out;
  Invisible (9) is a wall. The body's 26 contact points (rays from the
  box's centre) push it out of walls and barriers as well as the floor.
- **World** (`libs/world`): the world-space render models whose header
  box lies within 600 m of the car, at the lowest LOD of each object
  family among them, and the props within 250 m. The world is gathered
  again once the car is 200 m from the last centre. Left out: TERR_CUBE
  (reflection views), the UberLOD and middle-distance terrain, and
  occluders. Before COLLISION.md the ground was these render triangles
  minus thin upright objects; that stand-in is still there behind
  `--render-ground`. Render surfaces (road by the mesh object's name, then
  terrain, water and other) now only pick drawing colours and the test
  driver's road.
- **Spawn:** COLLISION.md's measured free-roam spawn (2002.96, 960.00,
  heading 126.1 degrees), dropped onto the tiles (0.48 m under the
  measured body point, on Asphalt). `--near x z` still takes the render
  road triangle nearest a point.
- **Frames:** the drive runs in the game frame (the file frame with z
  negated). The car's body frame has the game frame's handedness:
  AUTOSHOW_REFERENCE's `car_to_world` negates the model frame's z, as the
  model-to-body pose does. libs/world negates z once, as it gathers.
- **Drawing:** `carview --drive --world` draws the streamed triangles
  with their own textures (WORLD_MATERIALS.md's first-pass recipe; flat
  colours per surface class with `--flat-world`; the far ring of MIDDIST
  and UberLOD terrain and a sky gradient, ours) around the posed car,
  with a chase
  camera that pulls in when a ray from its look-at point to the eye meets
  the collision (and eases back out). Headless, it writes frames.
  Windowed (keyboard or gamepad, speed in the title), it is written but
  has not been run here.
- **Streaming off the step:** in the window (and headless with
  `--async-stream`), gathers after the first run on a worker thread:
  render models, collision tiles, the ground's BVH and the scenery's draw
  data, staged in host-visible buffers. The step keeps the old ground
  until the worker is done; the GPU copy is fenced and the old scenery
  stays drawn until it lands. On the test cruise the longest step went
  from 373 ms (synchronous) to about 3 ms. Headless runs stay synchronous
  by default so they repeat exactly.
- **Test driver** (`world::RoadFollower`, `--run cruise`): steers to the
  middle of the road ahead at a set speed. Headless runs put a stuck or
  rolled car back on the nearest road. It is for checks, not the game's
  AI.

## Specs needed, in order

Items 1 to 3 are answered by COLLISION.md (43346e1d), items 4 to 7 by
CAR_PHYSICS.md (a76cfa08); kept for the record.

1. **Collision geometry (blocking).** Where Colorado's collision lives
   (separate files or the render models, which members of `bin.zip` or
   another archive), the triangle or shape layout, its frame against the
   render models (the game frame negates z), and the tiling (bounds per
   file) so the area around a point can be loaded. Static props:
   which placed objects collide (barriers, walls, poles), with what
   shape, and which break away.
2. **Surface types (blocking for grip).** The per-triangle or per-region
   surface ID and the table that maps it to grip, rolling resistance and
   effects (tarmac, dirt, grass, gravel, rumble strips). Water and the
   world's edge, if they act on the car.
3. **A spawn point.** One free-roam start position and heading on a road
   (from the save, a festival site or an event's start grid), and how a
   car is placed on the ground there (the ride height rule FORMAT-CARS
   now gives).
4. **Car physics parameters (RF-4, blocking).** For a stock car, which
   tables and columns give: mass and its distribution, inertia, centre of
   mass; suspension per axle (spring rate, damping bump and rebound,
   travel, anti-roll bars, geometry such as camber and toe); engine
   (torque curve, rev limit, idle, inertia, engine braking); drivetrain
   (layout, gear ratios, final drive, shift time, clutch, differential
   type and locking); brakes (torque, bias, handbrake); aerodynamics
   (drag, frontal area, downforce front and rear); steering (lock, ratio,
   speed sensitivity). Units and frames for each, and whether upgrades
   change them (the stock car only for M3).
5. **The tyre model.** The force model (slip ratio and slip angle to
   force, load sensitivity, combined slip, relaxation length), its
   parameters per tyre size or per car, and the call rate and substeps
   RI-0.5 measured.
6. **Assists and input shaping.** The default assist set in free roam
   (ABS, traction and stability control, steering assist, automatic
   gears) and what each does, the steering filter (speed-sensitive
   steering, return to centre) and the deadzones and ramp rates for pad
   and keyboard.
7. **The physics gate's trace.** The format `tools/physics-behaviour.py
   compare` reads (fields, units, frame, time base), and the two
   scenarios: the car, start state and input script for 0-100 and for the
   lap (which route and start), so the engine can run the same inputs and
   write the same trace.
8. **Camera (not blocking).** The default chase camera's distance,
   height, look-at point, lag and field of view; the hood and bumper
   positions if `CarPartPositions` rows 4 and 9 are those.
9. **Engine sound (answered by ENGINE_SOUND.md, 14243a1c; built:
   `libs/audio`, `soundbake`, carview's drive plays it).** Which FSB bank
   and samples belong to a car, and how the game mixes them by RPM and
   load (layers, crossfades, pitch). RI-0.6 already decodes the samples.

## Not needed for M3

Traffic, AI, the radio, damage, the full HUD, upgrades and tuning, the
showroom lighting and post chain (polish), and M2's full streaming beyond
the area around the spawn.
