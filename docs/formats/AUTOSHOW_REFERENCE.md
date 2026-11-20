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
- **Some models exist twice.** 40 rows have files both on the disc and
  in a package (Rally ships folders for ten base cars; the LCE and
  October packs ship the same eleven). Which copy the game loads is not
  established; a bake should prefer the package copy and report a
  difference.
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
| Garage, when owned (unicorn) | 16 | Nine barn finds and seven gifts. The Autoshow leaves unicorn cars out. Use the garage of a save that owns them, or a test database mod that clears the unicorn flag (the #895 method) |
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
| about 240 to 600 | The standard view: three-quarter front, the price and the colour bar, a slow orbit |
| from 600 to 900 on | The UI hides and close beauty-camera cuts start (other shots at 900, 1200, 1800) |

Y (more tokens) brings the UI back; B opens a dialog. The stage has an
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

- take every reference at the same delay in the standard view; the pilot
  used 390 frames after A;
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

## Plan for every car

1. **Routes from the list.** Generate one route per make from
   `car-list.json` (`autoshow_make_index`, `autoshow_list_index`): open
   the Autoshow once, walk to the make, and for each car press A, capture
   at 300 and 390 frames, then return to the list and move one right.
   This depends on B taking the showroom back to the car list, which is
   not verified: in the pilot B during the beauty camera opened a
   dialog. If it cannot, re-enter the Autoshow per car. 57 make routes
   would cover the 200 Autoshow cars in roughly an hour of wall time.
2. **Dumps.** A run takes one dump, because the dump path is read at
   start and a second dump overwrites it. Either run one short route per
   car (200 runs, about 2.5 hours) or add a default-off SDK option that
   numbers dump files, so a make route can dump each car.
3. **Unicorns.** Use a test database mod in the seed that clears the
   unicorn flag (the method of the Autoshow price tests), so the 16
   unicorns appear in their makes; record that their references come
   from a modified list, and recompute the list places for that seed,
   since the unicorns shift the cars after them.
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

## Unknowns

- What sets the small per-car camera offset.
- Which colour query the showroom runs, and whether a metallic first
  colour is shown the same way.
- How the cube maps are produced and what the screen-sized texture
  holds.
- Which copy the game loads when a model exists both on the disc and in
  a package.
- Whether B from the showroom returns straight to the car list.
- The order of the six tied pairs in their car lists.
- Where the darkening under the car comes from.
