# Autoshow reference captures for the car viewer

Status: M1 ground truth, pilot of three cars, specification role,
2026-11-19. Our own reading of the player's files, of route captures and
of frame dumps; no disassembly, decompiled code or game data is
reproduced here ([LEGAL.md](../LEGAL.md)). The car list comes from
[`tools/re/car-list.py`](../../tools/re/car-list.py). The car model
format is specified separately (`docs/formats/CARS.md`).

M1 in [REIMPLEMENTATION_PLAN.md](../REIMPLEMENTATION_PLAN.md) asks that
every base, v4 and DLC car is drawn by the engine's car viewer and
matches an Autoshow capture side by side. This page says which cars that
is, which screen of the game shows each one, how a reference capture is
produced, and what the engine must reproduce of the showroom: camera,
paint colour, parts and lighting.

## The cars

`car-list.py` reads the base disc's database, v4's replacement copy and
the merge database of each installed Marketplace package. It merges them
the way the title does at Start, whole rows by key, packages in priority
order. Each row gets three things:

- where the row comes from: base disc, v4, or the package that adds it;
- where the car's files are. A base car is one `media/cars/<MediaName>.zip`
  archive. A package car is the folder `media/cars/<MediaName>/` inside
  the package's `<offer>_pri_65.zip`, with its wheels in
  `media/wheels/<MediaName>/`;
- which screen can show it (below).

With the 21 official packages installed there are **250 car rows** and
240 distinct models:

| Source | Autoshow | Unicorn | Event-only twin | Not selectable | Not entitled | Rows |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Base disc | 127 | 16 | | 23 | | 166 |
| Base row, sold through a DLC offer (Pre-Order, VIP, single-car packs) | 10 | | | | | 10 |
| v4 title update (files in the Rally Expansion Pack) | 5 | | 10 | | | 15 |
| DLC packages (11 packs add rows) | 58 | | | | 1 | 59 |
| **Total** | **200** | **16** | **10** | **23** | **1** | **250** |

The DLC rows by package: LCE Day1 11 (the October Car Pack carries the
same 11 rows), December IGN 7, February Jalopnik 7, January Recaro 7,
March Meguiar's 7, April TopGear 6, November Bondurant 6, Honda
Challenge 3, 1000 Club 2, Rockstar Energy Shelby 1, Miura Concept 1, and
Rally's Season Pass Focus SVT 1. The five single-car packs and the
smaller VIP package add no rows: they sell base rows and carry wheels.

- **v4 rows have no files of their own.** The title update adds 15 Rally
  car rows to its database, but its `media.zip` holds no car archives.
  The models are in the Rally Expansion Pack, so without that package
  the five selectable v4 cars cannot be drawn.
- **Some models exist twice.** 40 rows (31 media names, 9 of the rows
  event-only twins) have files in more than one place: 19 media names
  both on the disc and in a package (Rally ships nine base cars, the
  Pre-Order and VIP packs five each) and 12 in two packages (the LCE and
  October packs carry the same eleven). Every copy is byte-identical to
  the other, member for member (SHA-256 of all members, 2026-11-19),
  which agrees with FORMAT-CARS. Which copy the game loads is not
  established, and for the official set it does not matter: a bake takes
  the disc copy first, then the packages in package order, as
  FORMAT-CARS says, and reports a difference if a later install ever has
  one.
- **Rows without a model of their own.** The ten Rally event-only rows
  reuse the media name of a selectable car, and of the 23 base rows that
  are drivable but not selectable, 18 traffic rows have stub models of a
  few kilobytes ([HIDDEN_CARS.md](../HIDDEN_CARS.md)).

The community UDLC package is not official content. The tool leaves it
out unless `--with-udlc`; it would add 113 rows. The M1 seed does not
install it.

### Which screen shows a car

| Screen | Cars | How a reference is taken |
| --- | ---: | --- |
| Autoshow | 200 | Make carousel, then the car list, then the showroom (below) |
| Garage, when owned (unicorn) | 16 | Nine barn finds and seven gifts. The Autoshow leaves unicorn cars out. A test database mod that clears the unicorn flag (the #895 method) lists them; all 16 references are taken that way ([below](#cars-outside-the-stock-lists)) |
| Same model as a selectable car | 10 | Rally's event-only twins. No reference of their own: the selectable car's covers the model |
| Not selectable | 23 | Traffic, the placeholder and four showcase variants. Not shown in the stock game. The showcase four can be sold with the `hidden_cars` mod |
| Not entitled | 1 | The Season Pass Focus SVT: Rally's licence does not cover its offer |

The Autoshow's lists ask for cars that are drivable, selectable, not
unicorn, visible to the player and have a powertrain row. Visibility of
a package car follows its offer: the car's offer must belong to an
installed package, and the package's licence mask must cover the offer's
mask, except that an offer with mask 0 needs only its package (Honda
Challenge). Evaluated this way from the files, it agrees with the live
entitlement cache read in game for all 84 rows the packages touch.

## Producing a reference

### Seed and settings

Reference runs use a pinned seed: the v4 reference save with the 21
official packages, at 1x resolution, with split GPU recording off so a
frame dump can be taken. A local test mod widens the Autoshow's trigger
zone so that the activity button opens the Autoshow anywhere in free
roam. The render-test runner sets bloom, depth of field and motion blur
off and resolves MSAA to one sample, so references are the scene before
those effects. The engine should match that, not the shipped look.

The built-in immersive-camera mod is always loaded. It changes only the
driving camera's physics file; a pilot run with its four layers set to
0 put the showroom camera within the spread of two unchanged runs, so it
does not affect the reference.

### Selecting a car

The Autoshow opens on a make carousel: one row, the all-affordable entry
first, then the makes sorted by English name (57 makes with the 21
packages). D-pad right moves one make. A opens the make's car list, also
one row, sorted by class and then by performance index. A opens the
showroom. So a car is reached by two counts, both computed from the
database by `car-list.py` (`autoshow_make_index` and
`autoshow_list_index`). No code change is needed. The pilot routes
landed on the predicted car each time, and the SQL trace shows both list
queries as run.

### What the showroom shows

Three pilot cars were taken this way on the v4 build: one base car
(Abarth 595 esseesse, first make, first car), one v4 car (Lancia 037
Stradale, make 30, third car; its files are in the Rally pack) and one
package car (American Motors Javelin-AMX, make 2, first car, from the
December IGN Car Pack). Each route opened the Autoshow, walked the two
carousels by count, pressed A, captured the screen every 60 frames and
took one frame dump. All three landed on the expected car. The Abarth
was run three times (twice unchanged, once with the immersive camera
off) to check repeatability.

After A the showroom goes through three phases. The frames are the
render-test runner's, counted from the A press with the frame rate
uncapped; some UI camera shots advance with wall time, so the
boundaries may move at another frame rate.

| Frames after A | What is on screen |
| --- | --- |
| 0 to about 180 | Load, then a camera sweep in from a low view near the floor |
| about 200 to 640 | The standard view: three-quarter front, the price and the colour bar, a slow orbit |
| from about 680 | The UI hides and close beauty-camera cuts start (other shots at 900, 1200, 1800) |

The bulk runs measured the later boundaries with captures every 40
frames from A, for a slow-loading package car (Aston Martin DBS) and a
disc car (Virage), with the window visible and hidden: the four
timelines agreed. B behaves differently in each phase: during the
sweep it is ignored; in the standard view one B returns to the car
list (within 60 frames); in the beauty camera B only brings the UI
back, and a second B is needed. Y (more tokens) also brings the UI
back. The standard-view camera sits 6.58 m from the car's origin in
every reference taken; the close-ups are nearer (3.6 m for one DBS dump
at 720 frames). The stage has an
animated light wall and crowd behind the car, so two captures of the
same view are never identical; compare the car, not the background.

### Camera

The view is not static, and it is only nearly reproducible. At 390
frames after A, three runs of the Abarth gave clip matrices that differ
in the second or third decimal of a few terms: a fraction of a degree
of orbit between runs. The Lancia and the Javelin at the same delay sit
inside that spread in rotation, but their vertical translation term is
offset from the Abarth's by two to five times the run-to-run spread, in
opposite directions, which looks like the camera aiming at each car's
own height. So:

- take every reference at the same delay in the standard view: the
  references use 420 frames after A (390 for the first eight cars;
  each file's `frame.after_a` says which), and `autoshow-refs.py build`
  refuses a dump whose camera is not 6.58 ± 0.5 m from the car;
- do not assume one camera for all cars: read each reference's camera
  from its own frame dump (the stage's clip matrix), or model the aim
  point once the offset's source is known.

The idle orbit is a constant step per update, so a fixed delay in
runner frames lands close to the same view, but not exactly: a
reference is compared with the engine using the camera from its own
dump, never a shared one.

### Paint colour

The showroom shows the car in the factory colour with the lowest
sequence number in the car's colour rows; the first swatch of the
colour bar is highlighted. All three pilots match their first row
(orange, white and red). The viewer should paint a car with its first
colour row, using that row's metallic flag. The query the showroom runs
was not caught in the trace, so this rests on three cars, all with a
non-metallic first colour.

### Parts and lighting

From the frame dumps, matched to each car's model files by vertex
bytes:

- **Drawn:** body, interior and steering wheel, mirrors, lamps, glass,
  the four wheels with rotors and calipers. Interior and glass matter:
  the reference sees them through the windows.
- **Passes:** every car geometry draw is a triangle strip in the
  multisampled main pass with the stage: opaque parts with depth
  written, then glass and lamp covers without depth writes. (A few
  rectangle and point draws later in the frame still point a vertex
  fetch at a car buffer; they are post-processing and query draws, not
  car geometry.)
- **Shadows:** no car model geometry is in any depth-only or shadow
  pass, so there is no car shadow map. The captures do show darkening
  under the car; where it comes from (a blob or decal mesh outside the
  car's model files, the stage, or the screen-sized texture below) is
  not established.
- **Floor reflection:** the stage draws a mirrored copy of itself for the
  floor, but the car is not in it.
- **Lighting:** car draws sample several cube maps (two larger colour
  cubes, a small float-format cube that looks like irradiance, and a tiny
  one), a screen-sized texture, the car's own atlases and small lookup
  tables. How the cubes are made, and what the screen-sized texture holds,
  are open; the engine can start from a fixed cube map taken from a dump.

### Time per car

A one-car route is about 4,400 frames: boot and free roam about 1,400,
the carousels about 2,300 at a press every 30 frames, the showroom 720.
It took 41 to 50 s of wall time on multivac with the runner uncapped,
including start-up (six runs). In the showroom alone a car needs about
400 frames from A to the reference frame. The per-run and per-make
figures below are extrapolated from these runs, not measured.

## Reference files

Schema `shift-engine.m1-reference.v2`, one JSON file per referenced car
at `.local/re/cars/refs/<car_id>.json`, written by
[`tools/re/autoshow-refs.py`](../../tools/re/autoshow-refs.py) from the
car's frame dump. The images sit beside it. Everything stays under
`.local/`: the images are captures of the player's game.

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | string | `shift-engine.m1-reference.v2` |
| `car_id` | integer | `Data_Car.Id` |
| `media_name` | string | `Data_Car.MediaName`, the car's folder |
| `display_name`, `make`, `year` | | for reports only |
| `source` | string | where the row comes from: `base`, `v4`, or `dlc:<package id>` |
| `files` | string | the car folder the game read: disc archive or package folder, as `car-list.json` gives it |
| `seed` | string | the render seed the route ran on: `m1-autoshow-v4` (stock lists) or a seed with a test database mod that adds cars to the lists, which shifts the list places ([Cars outside the stock lists](#cars-outside-the-stock-lists)) |
| `image` | string | PNG of the dumped frame without the UI, path relative to the JSON |
| `image_car_only` | string | PNG of the same frame with only the car's draws, relative path (below) |
| `mask` | string | PNG car-coverage mask, same size, relative path (below) |
| `width`, `height` | integers | image size in pixels: 1280 and 720, the game's own render size at 1x |
| `frame` | object | `after_a`: runner frames from the A press that opened the showroom to the dumped frame; `dump_swap`: the executor's swap number of the dump |
| `clip_from_car` | 16 numbers | car space to clip space, **column-major**, for column vectors: clip = M × (x, y, z, 1) |
| `view` | 16 numbers | world to camera, column-major, rigid |
| `proj` | 16 numbers | camera to clip, column-major; `clip_from_car` = `proj` × `view` × `car_to_world` |
| `car_to_world` | 16 numbers | car space to the showroom's world frame, column-major. Not rigid: its determinant is −1, because it includes the game's model-to-body step (below) |
| `near` | number | the projection's near distance in metres (camera z where clip z is 0) |
| `half_pixel_removed` | boolean | always true (below) |
| `paint_rgb` | 3 integers | the colour shown: the car's first `Combo_Colors` row, sRGB, 0-255, as stored in the database (the game converts it to linear before shading; FORMAT-CARS) |
| `paint_metallic` | integer | that row's metallic flag |
| `game_clip_from_car` | 16 numbers | provenance: the car draws' matrix as the game sets it (rows are the clip components, row-major), before the conversions below |
| `checks` | object | the tool's own checks: car draw counts, which models the car draws read, the model-to-body matrix, the transparent pass's matrix, `draw_numbers_match_replay_log`, `empty_frames_agree`, `mask_threshold`, `mask_pixels`, `mask_box`, `glow_px`, `stage_leak_px`, `holes_filled_px`, `mask_outside_box`, `projected_box`, `mask_draws_left_out`, `camera_distance` |

**Spaces.**

- *Car space* is FORMAT-CARS': +X to the car's right, +Y up, +Z to the
  rear, the body's lowest point at y = 0, metres. Positions are the
  decoded `.carbin` positions; the wheels are placed in the vertex
  shader, so wheel, tyre, rotor and caliper models are drawn with the
  car's own matrix and their wheel-centre placement (FORMAT-CARS,
  Wheels).
- *Model to body.* The game does not draw car space directly: each
  car draw first maps the model into a body frame with z mirrored, the
  ride height added to y (0.163 m for the Abarth 595) and a small rake,
  then maps the body frame to clip. `clip_from_car` and `car_to_world`
  include both steps, so they take decoded `.carbin` positions as they
  are; the model-to-body step alone is in `checks.model_to_body`.
- *Transparent pass.* Glass and lamp covers are drawn with a second
  matrix whose projection differs by about one pixel (about 1.1 px at
  the car's edges); `clip_from_car` is the opaque pass's matrix, and
  the transparent one is in `checks.transparent_clip_from_car`.
- *Clip space* is Direct3D's: +x right and +y up on the screen as
  presented, w > 0 in front of the camera, z/w from 0 at the near plane
  towards 1 far away. The game's own matrix has negative x and y scales
  (it draws the view rotated by a half turn and turns it upright later
  in the frame, as in WORLD_GEOMETRY.md); `clip_from_car` has that half
  turn taken out, so it is the matrix for an upright image.
- *Depth.* The game's reversed depth is left out: the game stores
  1 − z/w and tests greater-or-equal; `clip_from_car` gives the standard
  z/w, 0 near and 1 far. The showroom projection has no far plane: its z
  row equals the w row less `near`, so z/w = 1 − `near`/w.
- *Camera space* (`view`, `proj`): +X right, +Y up, +Z forward into the
  screen (left-handed, as Direct3D), w = camera z.
- *World frame*: the frame of the stage's world-space draws, +Y up. Only
  `car_to_world` refers to it; an engine can use `clip_from_car` alone.
- *Pixels.* Pixel (i, j), j counted down from the top, has its centre at
  clip x/w = 2 (i + 0.5) / width − 1 and y/w = 1 − 2 (j + 0.5) / height,
  as a Vulkan or Direct3D 10+ viewport of the image's size (with y
  flipped for Vulkan) puts it. The game samples Direct3D 9 pixel centres,
  half a pixel right of and below those; the matrices have that half
  pixel removed (clip x moved by −w / width and clip y by +w / height),
  so a standard viewport reproduces the game's pixels.

**Image.** The frame the dump recorded, replayed offline with
`tools/replay-fh1-frame.py` with the UI draws left out (the post-processing
output before host upscaling, 8 bits per channel). The UI draws are the
triangle lists between the frame's last two EDRAM copies. Bloom, depth
of field and motion blur are off and MSAA is resolved, as in every
reference run.

**Car-only image.** The same dump replayed with the UI and every other
triangle draw into the car's colour surface left out, so only the car's
draws and the frame's clears and full-screen passes remain: the car,
lit as in the image, with its lamp glow. A full-screen pass before the
stage leaves stage content in some dumps (the floor lights' animated
reflection, a band across the lower frame); it is the same in the
dump's own empty replay, so the image keeps the car-only replay where it
differs from that empty replay by 2 or more, or the mask is set, and
shows the plan's first empty frame (the plain grey) elsewhere.
`checks.stage_leak_px` counts the pixels where the dump's empty frame
differs from the plan's first one.

**Mask.** 8-bit greyscale, 255 where the car covers the pixel, 0
elsewhere. Schema v2 changed it (v1 compared the car-only image with
the empty replay directly). It is built in three steps:

1. *Cover against empty.* Pixels where any channel of a *cover* replay
   differs from the same dump's *empty* replay by 6 or more of 255. The cover replay
   is the car-only replay without the car's additive draws (colour
   blend ONE + ONE: the lit lamps' glow, `checks.mask_draws_left_out`
   names their model sub-sections); the empty replay also leaves out
   every car draw. The car's draws are every draw of the main colour
   pass with the car's matrix (within 0.05, which takes in the
   transparent pass): the exterior LOD0 model, the cockpit model
   (interior, steering wheel, mirrors, lamp units), glass and lamp
   covers, rotors, calipers, rims and tyres. Glass counts, and so does
   the interior seen through it. The stage is drawn in neither replay,
   so the car's shadow and reflections on the stage stay out. Each dump
   has its own empty replay, which cancels any stage content the dump
   left in the car-only replays.
2. *Holes.* A part coloured like the empty background (grey 95, 95, 95)
   leaves holes in step 1. Pixels that step 1 leaves out but that it
   encloses (the image border cannot reach them without crossing the
   mask) are added where the reference image differs from a replay of
   the full stage without the car by 6 or more
   (`checks.holes_filled_px`). A part on the silhouette's edge that
   matches the background is not enclosed and can still be missed.
3. *Glare.* The post chain spreads a soft glare around very bright
   highlights (up to 16 of 255, as far as 60 px from the body). It is
   not a car draw: leaving out each draw class after the car's
   showed that it is made by the final post passes. Mask pixels more than 32 px
   outside the projected box of the car's LOD0 vertices (96 px below,
   for the tyres) are cut off and counted in `checks.mask_outside_box`;
   more than 5,000 such pixels reject the dump.

`checks.glow_px` counts the pixels the additive glow changes outside
the mask; `image_car_only` keeps the glow.

**Projection check.** The parts the game drew (exterior LOD0 and
cockpit sections, tyres and wheels left out of both sides), projected
with `clip_from_car` and rasterised at pixel centres, against the game's
coverage of the same parts (a car-only replay without the wheel draws,
against the empty frame, at the mask threshold): IoU 0.988 to 0.996 on
seven cars (Abarth 595 of 1968 0.990, Abarth 500 esseesse 0.988,
Alfa Romeo 8C Spider 0.995, 8C Competizione 0.996, TZ3 Stradale 0.990,
AMC Javelin AMX 0.994, Lancia 037 Stradale 0.990). The remaining difference is
edge pixels and the transparent pass's one pixel shift. Thin parts the
car draws, such as the Ford F-150 SVT Raptor's whip antenna, stay in the
mask as one-pixel lines; they are car geometry, not stage leak.

## Plan for every car

1. **Routes from the list.** Generate one route per make from
   `car-list.json` (`autoshow_make_index`, `autoshow_list_index`): open
   the Autoshow once, walk to the make, and for each car press A, capture
   at 300 and 390 frames, then return to the list and move one right.
   B at 480 frames after A, in the standard view, returns to the car
   list with the same car selected; the next press waits 300 frames.
   Routes run hidden (the menus advance in wall time and a visible
   window changes the pacing). `autoshow-refs.py routes` writes routes
   of several makes each, and `--only` routes a subset.
2. **Dumps.** The route sets `fh1_frame_dump_path` at run time before
   each request, so one run dumps every car it shows (FH1 render test
   automation, Frame dumps). Each dump is replayed three times (image,
   car only, and the empty frame for the first and last dump), and the
   dump is deleted once its reference is built.
3. **Unicorns.** Done (2026-11-20): all 16 have references from the seed
   `m1-autoshow-v4-unicorn-db` ([below](#cars-outside-the-stock-lists)).
4. **Not in the Autoshow.** The ten event-only twins share a model with
   a selectable car and need no reference. The 23 non-selectable base
   rows get no stock reference; the four showcase variants can be shown
   with the `hidden_cars` mod if wanted. The Focus SVT needs Rally's
   Season Pass licence, which the seed does not have.
5. **Check.** For each car, assert the opened make from the SQL trace
   (`PINYON_SHIFT_DLC_SQL_TRACE` prints the car list query with its
   `MakeId`) and check the list capture names the expected car before
   trusting the showroom frames. The make order is predicted from a plain
   lowercase sort of English names, not the game's collation, and six
   pairs of cars share class and performance index (in Audi, Mitsubishi,
   Dodge, Volkswagen and Ford twice); `car-list.py` orders such pairs by
   ID, which the game does not promise. Keep captures and dumps under
   `.local/re/cars/`.

## Cars outside the stock lists

A car the stock Autoshow leaves out can be shown by a test database mod
in a copy of the reference seed: a `db/*.sql` script, listed in the
seed's `enabled_mods`, that the launcher applies to v4's own database in
the `zz-archive-patches` update overlay (v4 reads no other copy;
[MODDING.md](../MODDING.md#database-patches)). The mod changes the
lists, so `autoshow-refs.py routes` recomputes every place for the cars
it routes (`--unicorns`, `--shown <ids>`), and each reference records
its seed in `seed`. The seeds are local test copies; the reference save
itself is never modded.

**Unicorns** (seed `m1-autoshow-v4-unicorn-db`, mod `m1_unicorn_db`:
`update Data_Car set IsUnicorn = 0 where IsUnicorn = 1`). All 16 rows
then appear in their makes at the places predicted by the stock order
(class, performance index, ID), with a price and the usual showroom, and
Plymouth becomes a new make (58 makes). One route of 16 cars: every list
capture named the expected car, every dump read the expected model, and
all 16 references passed the camera (6.577 to 6.578 m), alignment, leak
(0 px) and stray-mask checks (at most 2,115 px outside the box, the
S5S Raptor).

| Car | Make, place | Car | Make, place |
| --- | --- | --- | --- |
| 1105 Aston Martin DB5 Vantage | 4, 0 | 1386 Aston Martin DBR1 | 4, 1 |
| 1040 BMW M1 | 7, 3 | 1219 Bugatti EB110 SS | 9, 0 |
| 326 Ferrari Dino 246 GT | 16, 0 | 1154 Ferrari 250 Testa Rossa | 16, 2 |
| 1082 Ford Fiesta Zetec S | 17, 3 | 320 Honda CR-X SiR | 21, 2 |
| 1301 Jaguar D-Type | 25, 1 | 330 Lotus Elan Sprint | 33, 0 |
| 1233 Mazda MX-5 Superlight | 35, 2 | 251 Mercedes-Benz 300SL | 37, 0 |
| 1488 MINI John Cooper Works Coupe | 38, 2 | 1277 Plymouth Cuda 426 | 44, 0 |
| 1197 Saleen S5S Raptor | 48, 0 | 1155 Shelby Cobra Daytona | 50, 1 |

## Unknowns

- What sets the small per-car camera offset.
- Which colour query the showroom runs, and whether a metallic first
  colour is shown the same way.
- How the cube maps are produced and what the screen-sized texture
  holds.
- Which copy the game loads when a model exists both on the disc and in
  a package (no effect: the copies are identical).
- The order of the six tied pairs in their car lists.
- Where the darkening under the car comes from.
