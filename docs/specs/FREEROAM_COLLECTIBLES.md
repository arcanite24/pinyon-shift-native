# Spec: free-roam collectibles and the minimal map

| | |
| --- | --- |
| Spec ID | `SPEC-FREEROAM-COLLECTIBLES` |
| Status | first usable draft for M4: placements, profile records, the discount rule and the map projection confirmed; the sign's break threshold, the rumour spawn rule and the minimap's zoom open with interim rules |
| Written | 2026-11-20, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | [objects findings](../../site/findings/objects.toml), group "Activities and collectibles" (CActivityFlyers, CActivityBarnFind, map markers, discount sign smash event); [content findings](../../site/findings/content.toml), "free-roam activities" and "barn find activity data"; [saves findings](../../site/findings/saves.toml), "collectible records in the profile" and "upgrade discount"; [UI findings](../../site/findings/ui.toml), "map background projection" and "map render profiles" |
| Implements it | `shift-engine` free-roam module (not yet) |

## What it is

Free roam hides two kinds of collectible in Colorado:

- **Discount signs** (the code calls them flyers): 100 breakable signs.
  Driving through one smashes it, and each smashed sign takes one
  percent off every upgrade price, so all 100 make upgrades free.
- **Barn finds**: 9 old cars in barns. A rumour (a circle on the map)
  appears from time to time; driving slowly up to the barn inside it
  plays a short cut scene and gives the car to the garage.

Both are shown on the pause map and the minimap, found or not, through
the map's render profiles. The map draws a painted background texture,
the roads from `colorado.nav` as vector lines, and icons from one atlas.

Inputs, all on the player's disc:

| Input | File | Used for |
| --- | --- | --- |
| Sign list | `Colorado/Flyers.xml` in `media/gamemodes.zip` | the 100 activities, `flyer_001` to `flyer_100` |
| Barn list | `Colorado/barnfinds.xml` in the same archive | the 9 activities: trigger, rumour circle, spawn data, door objects, the car |
| Shared flow | `barnfinds.xml` (archive root) | the discovery and rumour-reveal sequences |
| Placements | `media/tracks/colorado/Ribbon_00/GameObjs.xml` | position and orientation of every sign, barn trigger and barn door object, by `GameplayID` |
| Models | the `.pvsz` zone files and the `.pvs` object table ([WORLD-GEOMETRY](../formats/WORLD_GEOMETRY.md)) | the render model of each placement |
| Map profiles | `media/ui/MapProfileFullscreen.xml`, `media/ui/MapProfileMinimap.xml` | what the map and minimap draw, in which colour, size and order |
| Map textures | `Map/background/*.xds`, `Map/icons/MapIcons/<language>/*.xds` in `media/ui/textures/Horizon.zip` | background tiles and the icon atlases ([FORMAT-CARS](../formats/CARS.md) has the `.xds` layout) |
| Road map | `media/tracks/colorado/colorado.nav` ([SPEC-TRAFFIC](TRAFFIC.md)) | roads, the north marker, area outlines and map labels |
| Car rows | `gamedb.slt`, `Data_Car` | the barn cars (`IsUnicorn` rows) |
| Profile | the career profile's class-serialised tail and `Main` fields | found flags, rumour state, the discount |

Outputs: sign and barn objects in the world (drawn, collidable, and
smashed or opened), map and minimap layers, the upgrade discount, a car
added to the garage, and the profile records.

## Discount signs

### Placement and model

`Flyers.xml` lists 100 activities of type `ActivityFlyers` with no
attributes beyond the name. Each name is also a `GameplayID` in
`GameObjs.xml`: an object with a position (`Pos`, game frame, metres)
and an orientation (three axis vectors, `XAxis`, `YAxis`, `ZAxis`). All
100 are placed exactly once there. The same 100 names are placed in the
`.pvsz` zone files at the same positions (within 10⁻⁶ m), so either file
gives the position; `GameObjs.xml` is the simpler one.

Every sign uses one render model, the LOD set whose first mesh is
`O_FEST_EquipSign`, found through the zone record's object ID and the
`.pvs` object table as for any other prop. Each zone record carries the
same small index (the flag-1 u32, one value for all 100 signs, probably
the smashable type) and the same route list: free roam (route 0) and
nine race routes, so the signs show in free roam and in those races.

`media/tracks/colorado/SmashableObjectTypes.xml` lists `Flyer` among the
four object types the car may smash (with `Fence`, `Mailbox` and
`RaceActivation`).

### The smash

A sign is smashed when the car breaks its smashable object. The game
then posts the event "Activity Discount Sign Smashed", whose handler
runs the activity's smash with notification on. The smash:

1. sets the activity's smashed flag and the profile record's found byte;
2. takes the "flyer" map marker off the map and, while free-roam
   collectibles are live, puts the "flyer_collected" marker on it;
3. with notification on, updates the career profile (the discount,
   below) and the UI.

Collectibles are live in free roam only, and not during the first-time
career (the opening drive): there a sign may break as a prop but counts
nothing.

**Interim rule for the break (open, see Unknowns):** a sign breaks when
the player car's body touches the sign's collision at any speed above
1 m/s. The engine then hides the sign's model, spawns no debris (the
track's `SmashableEffects.xml` lists no effect for it) and runs the
three steps above.

### The reward

The profile field `Main/UpgradeDiscount` (an unsigned integer, percent)
holds the discount. It equals the number of signs found on both saves
read: 85 on a save with 85 signs found, 100 on the reference save with
all 100.

With d = `UpgradeDiscount` / 100, an upgrade's price p (from the
`List_Upgrade*` tables) becomes

```text
p' = floor((1 − d) · p + 0.5)     when d > 0
p' = p                            when d = 0
```

in single precision. Checks against observed prices on the 85-sign save:
a 10,000-credit tyre costs 1,500; a part listed at 6,200 costs 930. With
all 100 signs every upgrade costs 0 credits.

Car prices in the Autoshow are **not** discounted (probable: car
purchases on saves with most signs found were charged `Data_Car.BaseCost`
in full; the discount function's six callers were not each traced). The
database's `AffinityLevelDef.Discount` column is a different discount,
not used by signs.

### Profile record

The career profile's class-serialised tail (after the self-describing
fields that `tools/fh1-profile.py` decodes) holds a record named
`Colorado` of class `CHorizonLocationState`, version 2. Its payload is a
count n (191 on every save read) and n child records. Every tail record
has the form

| Field | Size |
| --- | --- |
| name length, name (ASCII, no terminator) | 4 + length |
| class name length, class name | 4 + length |
| version | 4 |
| payload size s | 4 |
| payload | s |

all big-endian. A sign's child record is named after its activity
(`flyer_001` …), class `CFlyerState`, version 1, payload 2 bytes:

| Byte | Meaning |
| ---: | --- |
| 0 | revealed: 1 once the Treasure Map was bought (shows unfound signs on the map) |
| 1 | found: 1 once smashed |

So the engine marks a sign found when its record's byte 1 is 1. A save
without a record for a sign treats it as not found and not revealed.

### Map icons

| State | Map tag | Small atlas cell (minimap) | Large atlas cell (pause map) | Shown when |
| --- | --- | --- | --- | --- |
| not found | `flyer` | column 6, row 3 | column 6, row 3 | revealed (Treasure Map), or a cheat |
| found | `flyer_collected` | column 7, row 3 | column 7, row 3 | always |

## Barn finds

### Data

`Colorado/barnfinds.xml` holds one `ActivityBarnFindController`
(`barnfind_controller`) and 9 `ActivityBarnFind` activities,
`barnfind_01` to `barnfind_09`. Each barn find has these elements (the
engine reads the values from the file):

| Element | Attributes | Meaning |
| --- | --- | --- |
| `TriggerZone` | `object`, `radius` (m), `maxMPH`, `mapTag` (`barnfind`), `requires_input` (false), `mapDescription` | discovery trigger around the named `GameObjs.xml` object: 10 m and below 50 mph for all nine |
| `HintRegion` | `x_offset`, `y_offset`, `x_radius`, `y_radius` (m), `angle` | the rumour circle: centre = trigger position + (x_offset east, y_offset north); radii 286 to 600 m, all circles, angle 0 |
| `SpawnInfo` | `weighting`, `min_probability`, `min_distance`, `max_probability`, `max_distance` | spawn weighting (meaning open) |
| `DistanceDriven` | `distanceMin`, `distanceMax`, `threshold` | 5, 10 and 10 for all nine; the file's comment says miles (meaning open) |
| `DoorInfo` | `open_id`, `closed_id`, `open_coll_id`, `closed_coll_id` | four `GameObjs.xml` objects: the barn with closed or open doors and the matching collision |
| `UnlockCar` | `id`, `stringTableNameId` | the `Data_Car` row given, and its name in `Barnfind.str` |
| state group | cut scene names, map animation `barn_find_region<k>_unlock`, a VO trigger | presentation only |

The trigger object (`BARNFIND_<car>`) is the derelict car itself: in the
zone files it is a car-specific model (for example `Cuda_LOD0_001`), and
the barn building is 4.0 to 6.1 m from it. Both door variants use one
barn model set (`Barnfind_Barn__LOD00`, two members: closed and open).
All 9 triggers and all 36 door objects are placed in `GameObjs.xml`
(`tools/re/collectibles.py census`).

### The nine cars

All nine are unicorn rows (garage only), with references in
[AUTOSHOW_REFERENCE.md](../formats/AUTOSHOW_REFERENCE.md#cars-outside-the-stock-lists):

| Activity | `Data_Car` id | Car |
| --- | ---: | --- |
| barnfind_01 | 1219 | Bugatti EB110 SS |
| barnfind_02 | 1040 | BMW M1 |
| barnfind_03 | 1277 | Plymouth Cuda 426 |
| barnfind_04 | 1105 | Aston Martin DB5 Vantage |
| barnfind_05 | 1386 | Aston Martin DBR1 |
| barnfind_06 | 1154 | Ferrari 250 Testa Rossa |
| barnfind_07 | 1301 | Jaguar D-Type |
| barnfind_08 | 251 | Mercedes-Benz 300SL |
| barnfind_09 | 1155 | Shelby Cobra Daytona |

The other seven unicorns (Dino 246 GT, Fiesta Zetec S, CR-X SiR, Elan
Sprint, MX-5 Superlight, MINI JCW Coupe, S5S Raptor) are gifts, not barn
finds.

### States

Each barn find has a status, kept in its profile record:

| Status | Meaning | Map |
| ---: | --- | --- |
| 3 | not spawned: no rumour yet | nothing (the exact location if the Treasure Map revealed it) |
| 4 | spawn pending (transient) | — |
| 5 | rumour running: discoverable | the rumour circle (`barn_find_region`) |
| 1, 0 | discovered, then collected (transients set by the discovery flow, from the code) | — |
| 2 | found and the car delivered: the value on every found barn in the reference save, live and saved | the `barnfind_collected` icon |

Only a barn at status 5 can be discovered.

**Discovery.** At status 5, when the player car comes within the
trigger radius of the trigger object below `maxMPH` (no button), the
game runs the discovery flow: car control off, audio muted, handbrake
on, fade to black, traffic and remote cars removed, the discover cut
scene, a second fade, the outro cut scene, then everything restored and
the HUD back; then the "Barn This Way" achievement, a micro-save and a
one-second wait. The car of `UnlockCar` goes to the garage. The barn
switches from its closed to its open door objects (render and
collision).

**Rumour reveal.** When a barn spawns (status 3 to 5) the game pauses,
opens the map with the barn-finds filter forced on, plays the region's
unlock animation and VO, then returns to free roam.

**Spawn rule (open, see Unknowns).** Each record stores the total
distance driven when its rumour started. On the reference save the
rumours started in the order 02, 01, 03, 04, 08, 05, 07, at total
distances whose consecutive gaps were 144, 49, 50, 52, 207 and 250 km
(the last two gaps include races, which the counter may not count).
**Interim rule:** start the next rumour, lowest activity number first
among those at status 3, each time the profile's total distance driven
passes the last rumour's start by 50 km, and never while another rumour
is running.

### Profile record

In the same `Colorado` record, each barn find has a child record named
after its activity, class `CBarnFindState`, version 3, payload 15 bytes:

| Bytes | Meaning |
| ---: | --- |
| 0 | revealed by the Treasure Map |
| 1 | discovered |
| 2 | collected (the car is in the garage) |
| 3-6 | f32: total distance driven when discovered (0 before) |
| 7-10 | f32: total distance driven when the rumour started (0 before) |
| 11-14 | u32: status (table above) |

The distances use the unit of `Main/TotalDistanceDriven` (probable:
metres; 873,207 on the reference save after 19.3 h of driving). On every
record read the discovery distance is above the rumour distance.

### Map icons

| State | Map tag | Small atlas cell | Large atlas cell |
| --- | --- | --- | --- |
| rumour circle | `type` = `barn_find_region` | an ellipse, white with a dark 20 % rim on the minimap, no icon | an ellipse |
| exact location (Treasure Map or cheat) | `barnfind` | column 0, row 4 | column 0, row 4 |
| found | `barnfind_collected` | column 1, row 4 | column 1, row 4 |

## The minimal map

### Background texture and projection

The pause map's background (`MapProfileFullscreen.xml`, group
`background`, filtered on `grid_control` = `north`) is one painted
satellite-style image of Colorado with place names, in two levels:

| Level | Members | Size |
| --- | --- | --- |
| low | `Map/background/low-0-0.xds` | 2048 × 1229, one tile |
| high | `Map/background/high-<c>-<r>.xds`, c = 0..4 west to east, r = 0..2 north to south | 15 tiles of 1024 × 1024 (5120 × 3072), drawn within 3,000 m of the view, faded from 1,000 m |

Both levels cover the same rectangle. The profile gives its south-west
corner (`Offset` x −9583.3, z −6900) and its north-south extent (`Size`
11500); the east-west extent follows from the 5 : 3 tile grid. With X
and Z the game frame's horizontal axes (+Z north, as the `colorado.nav`
north marker points):

```text
west  X = −9583.3     east  X = +9583.3     (width  W = 19166.7 m)
south Z = −6900.0     north Z = +4600.0     (height H = 11500.0 m)

u = (X + 9583.3) / 19166.7          u: 0 at the west edge, 1 at the east
v = (4600.0 − Z) / 11500.0          v: 0 at the north edge, 1 at the south
```

Scales: 9.36 m per pixel on the low level, 3.74 m per pixel on the high
level. The image's outer columns (about 7 % each side) are black; every
road lies inside u 0.17 to 0.86, v 0.07 to 0.93.

`media/UI.zip` also holds the artists' source images (`New Map/…`,
including a 47 MB `MapGameReady.tga`); the game does not read them.

### Roads and other vector layers

The game draws roads as vector lines from `colorado.nav`, not from the
texture: the profiles' road groups filter on the way tag `road_type`
(`dirt`, `b`, `a`, `freeway`; `none` and `shortcut` ways are not drawn),
each as a dark under-line (`line_fade_lr`) and a coloured line on top,
with sizes and colours in the profile. The painted roads in the texture
lie under them (checked by drawing every way over both levels with the
equation above: they coincide everywhere, `tools/re/collectibles.py
overlay`). Other `colorado.nav` layers: 19 text ways (`map:ids_map_*`
place-name labels, string keys), 13 `area_type` outlines (festival,
lakes, regions, and a `cursor_area` rectangle that bounds the map
cursor) and the `grid_control` north marker. A minimal map needs only the
roads; the texture already carries the place names.

### Icons

Icons come from one atlas per language, 8 × 8 cells:
`Map/icons/MapIcons/<lang>/MapIconSheet.xds` (1024 × 1024, 128-pixel
cells, the pause map) and `MapIconSheetSmall.xds` (512 × 512, 64-pixel
cells, the minimap). Each profile group names a cell by column and row
(`Atlas x`, `y`), a size (`DynamicSize`) and a colour. Those M4 needs:

| Item | Map tag | Cell (column, row) | Notes |
| --- | --- | --- | --- |
| player | `vehicle_type` = `player_local` | (2, 3) and (3, 3), stacked | the pause map adds (4, 4) and (1, 5); the arrow turns with the car's heading |
| race event | `activity_type` = `race` | (1, 0) with colour layer (0, 0), finish (5, 1), new (3, 4) | other event types have their own groups |
| discount sign | `flyer` / `flyer_collected` | (6, 3) / (7, 3) | size 20, the smallest icons |
| barn find | `barnfind` / `barnfind_collected` | (0, 4) / (1, 4) | |
| traffic and festival cars | `vehicle_type` = `traffic`, `ai` | (5, 3) | |

Minimap icon groups fade out between 2,400 and 2,800 m from the player
(`VisibleRange`). Icons on the minimap stand up facing the camera
(`icon_facing`); on the pause map they lie flat (`icon_up`).

### Pause map and minimap views

- **Pause map:** north up, the background, vector roads and icons, a
  cursor that starts at the player. Zoom levels are not measured; an
  implementation may use its own.
- **Minimap:** a perspective view of the vector layers only (roads,
  rumour ellipses, icons), with **no background texture**, drawn in the
  lower-left corner. It turns with the car (heading up) and tilts away
  from the viewer; the player arrow sits low in its centre. The rumour
  ellipse fills white there. Zoom: open (interim: about 300 m ahead of
  the player to the far edge).

## Rates and timing

The smash and the discovery trigger are tested each simulation step
while collectibles are live. The rumour spawn rule runs on the distance
counter, not on time. The map draws at the frame rate; nothing on the
map depends on the simulation step.

## Unknowns

| Unknown | Confidence of what is known | Interim rule |
| --- | --- | --- |
| The sign's break threshold (speed, impulse or mass) | the break is a smashable-object event, confirmed; no threshold found in the track or physics files read | break on contact above 1 m/s |
| The rumour spawn rule: what `SpawnInfo` and `DistanceDriven` do, the order, whether race distance counts | the record's distance fields, confirmed; order and gaps from one save | next barn by number every 50 km of total distance, one rumour at a time |
| The distance unit | probable metres | metres |
| `HintRegion` y sign | probable +Z (north), from the reference save's one running rumour drawn on the pause map | north |
| Status values 1 and 0 | from code reading; saves hold 2 | write 2 when found |
| Whether a found sign's model is removed or drawn broken after a reload | not checked | do not draw found signs |
| Whether the derelict car model stays in the barn after collection | not checked | hide it once collected |
| Minimap zoom and tilt | one frame | about 300 m to the far edge, 45° tilt |
| Car prices and the discount | probable not applied | apply to upgrades only |
| Where `UpgradeDiscount` is written (count or increment) | it equals the found count on two saves | set it to the found count |

## Tolerances

- Placements: positions as read from `GameObjs.xml`; the live game's
  positions match within 0.1 m (the logging precision).
- Map projection: a world point must land within 2 high-level pixels
  (7.5 m) of the same feature on the texture; the projection has no
  free parameter.
- Discount: exact integer credits.

## Tests

Counts over the player's disc (`tools/re/collectibles.py census`):

- 100 sign activities, all 100 placed in `GameObjs.xml`, all with model
  `O_FEST_EquipSign` in the zone files.
- 9 barn find activities, 9 trigger objects and 36 door objects placed, 9
  distinct cars, all 9 rows `IsUnicorn` in `Data_Car`.
- The `Colorado` record of any profile has 191 children: 100
  `CFlyerState`, 9 `CBarnFindState`.

Reference save (the pinned copy `m4-collectibles-v4` of the AppData save,
route `config/render-tests/fh1-m4-collectibles.fh1test` on the v4 build
with the save trace, 2026-11-20; `tools/re/collectibles.py profile`):

- 100 sign records, 100 revealed, 100 found; `Main/UpgradeDiscount` 100.
- Barn finds: 01, 02, 03, 04, 05 and 08 found (status 2), 07 rumour
  running (status 5), 06 and 09 not spawned (status 3), all revealed.
  The live census agrees (6 of 9 found, 100 of 100 signs).

Position checks (same run, `cheat_show_collectibles` logging each
activity's live position):

- **Sign:** all 100 live sign positions are within 0.08 m of their
  `GameObjs.xml` positions (the log rounds to 0.1 m).
- **Barn find:** all 9 live positions are within 0.08 m of their trigger
  objects, for example `barnfind_07` (Jaguar D-Type).
- **Player on the map:** the player stood at X 4247.4, Z 676.2 (the
  capture's vehicle pose). The equation gives u 0.72160, v 0.34120: on
  the high level, the road junction south-west of the Bunker compound,
  where the car stood at an event start. On the pause-map capture the
  player arrow lies within about 5 screen pixels (about 25 m) of that
  point when the screen is registered to the texture by two painted
  labels (Beaumont and Finley Dam).

Synthetic cases the implementation must reproduce:

- u, v of the corners: (−9583.3, 4600) gives (0, 0); (9583.3, −6900)
  gives (1, 1); (0, −1150) gives (0.5, 0.5).
- Upgrade price 10,000 with 85 signs found: 1,500. With 0 signs: 10,000.
  With 100: 0. Price 999 with 1 sign: 989 (0.99 × 999 = 989.01). Price
  50 with 1 sign: 50 (49.5 rounds up).
