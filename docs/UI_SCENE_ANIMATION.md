# UI scene animation: the BGF timeline

The UI4 scenes (`Scenes/ui4/<name>.fbf/.bsg/.bgf`, FORMAT-UISCENES) are
drawn by Anark Gameface, of the UI Composer / Qt3DS line. The `.bgf` holds the
items and contracts (docs/UI_SCENE_SCREENS.md) and, after the relations, the
scene's **timeline**: components, their slides, the curves the slides play and
the actions events run. This page is the format as decoded from the title
(the loaders at 82F26AC0, 82F26D90 and 82F284A8, the evaluator at 82F2EF08)
and the 269 retail BGFs (230 on the disc, 39 in title update v4), and how the
engine plays it (`fh1::parse_ui_bgf`, `carview::SceneTimeline`).

Every number below is big-endian, as in the rest of the file.

## Names and attributes

Names are hashed as the runtime does: `h = (h + c) * 65599` over the bytes,
32-bit (`fh1::ui_name_hash`).

- **Slide names** compare on the full 32 bits, case-sensitive (`GotoSlide`'s
  argument is the hash of `"Blurred"`, not of `"BLURRED"`).
- **Attributes** use the low 27 bits (`fh1::ui_attr_hash`). In a tagged value
  the top 5 bits are the value's type; their low 3 bits are 1 for an int,
  3 for a float and 5 for a string (an identity index). The other two bits
  vary, for example `0x98000000` on a float SET. A string is
  `(tag & 0x38000000) == 0x28000000`.
- **Events** compare on 27 bits.

The attributes the scenes animate:

| Attribute | 27-bit hash | Unit |
|---|---|---|
| `opacity` | `0191C315` | percent, 0..100 |
| `endtime` | `003BF5F8` | ms, int: the element's end in its slide |
| `starttime` | `010A57B1` | ms, int (never set in retail) |
| `position.x/.y/.z` | `27C230D`, `27D234C`, `27E238B` | scene units, y up |
| `scale.x/.y/.z` | `465440C`, `466444B`, `467448A` | factor |
| `rotation.x/.y/.z` | | radians (spinners run 0 to -6.28) |
| `pivot.x/.y` | | scene units |
| `diffuse.r/.g/.b/.a` | `15B085A`, `15005A5`, `14B046A`, `14A042B` | a material's colour, 0..255 |
| `textcolor.r/.g/.b/.a` | `4E9F186`, `4DEEED1`, `4D9ED96`, `4D8ED57` | a text's colour, 0..255 |
| `positionu/v`, `emissivepower` | | a texture's offset, a material's glow |

An item's own properties (the item section) are its base values. They equal
the BSG node's values: `120_P_PRE_SPLASH`'s `START_BUTTON` is at
(-282, 2, -3) in both.

`UiContract::initial_state` reads property `0x083BF5F8`. That is `endtime`
as an int tag, not a state name (`083bf5f8 000007d0` is 2000 ms); do not use
it to pick a component's first slide (Q188).

## Sections

After the relations, four length-prefixed sections follow. The records in
each must consume the declared length exactly; the title checks the same
sums. The header gives the counts:

- 0x44: curves
- 0x48: curve keys
- 0x4C: action owners
- 0x50: events
- 0x54: actions
- 0x5C: slides
- 0x60: slide tracks
- 0x64: values
- 0x68: references
- 0x6C: the empty table

The engine reads the sections strictly. If any one does not end at its
declaration, `UiBgfDocument::has_timeline` is false and the timeline is
dropped. All 269 retail BGFs read.

### 1. Curves (82F26AC0)

Per curve:

| Field | Size |
|---|---|
| item index | u32 |
| attribute tag | u32 (keep 27 bits) |
| flag | u8, 1 in every curve |
| dynamic | u8: 1 starts from the current value (2070 retail curves) |
| key count | u16 |
| keys | 24 bytes each |

A key is: time (ms, int), value (f32), c1 time (int), c1 value (f32),
c2 time (int), c2 value (f32). Every retail key has control times of 0. The
last key of some curves has c1 = c2 = -1.0.

### 2. Actions (82F26D90)

Per owner item: owner (u32), event count (u32); per event: name hash (u32),
action count (u32); per action, 20 bytes: source item, target item, handler
hash, argument tag, argument.

| Handler | Meaning |
|---|---|
| `1195C23B` | GotoSlide: argument tag = the slide name's hash; target = the component |
| `7E4906FD` | SetProperty: argument tag = the attribute tag, argument = the value |
| `5B2FA69C` | GotoTime: argument tag = ms (the pause menu's `SIZE0..7` scrub its bar 0..6000) |
| `4F15F91E` | Pause (probable; follows each GotoTime) |
| `573BA2B8` | Play (probable) |
| `6444A519`, `06875030` | rare, not known (H_MAP's HIDE) |

The owner is usually a contract item. The events are the contract's states:
`120_P_PRE_SPLASH`'s owner item 1 sends START, MENU, START_OUTRO, MENU_INTRO
and INSTALL to the root's slides of those names; the buttons send FOCUS, BLUR,
FOCUSED and BLURRED; SHOWN, HIDDEN, SHOW and HIDE set opacity to 100 or 0.
The C++ contracts fire them.

### 3. An empty table

Length 0 in every scene.

### 4. Slides (82F284A8)

Per slide:

| Field | Size |
|---|---|
| name | 16 bytes, NUL-padded |
| component item | u32 (0 is the scene) |
| flags | u8 |
| start | u32 ms |
| end | u32 ms |
| track count | u32 |

Per track:

- item (u32), active (u8)
- the values set when the slide starts: a count, then (tag, value) pairs
- references: a count, then 4 bytes each: kind (u8; 0 a curve, else an
  action), enabled (u8), index (u16, `& 0x3FFF`) into the curves or actions

Each component's slides start with its **Master Slide**. It lists every
element of the component, with the values common to all its slides, and
every curve and action with `enabled` 0. Each other slide lists its elements
again with its own values, and its own curves and actions with `enabled` 1
(0: present but not run here). The 9423 curves that no slide enables are
each listed, disabled, in exactly one slide.

Slide flags. 82F2B230 packs them and 82F2B408 hands them to the time manager
(+0x38: duration, `flags & 3`, bit 3, bit 4; +0x3C: bit 2). The receiver was
not found, so the meaning below is read from the corpus (Q183):

| Bits | Meaning |
|---|---|
| `flags & 3` | repetitions: 0 loops forever, 1 plays once |
| bit 2 (4) | starts paused; the game sets the time (`PROGRESS`, the pause menu's size bar) |
| bit 3 (8) | set on nearly every play-once slide; effect unknown |
| bit 4 (16) | at its end, plays through to the next slide (`Focus` to `Focused`) |

Seen: 9 (19,707 slides), 0, 17, 13, 8, 4, 12.

## The evaluator (82F2EF08)

A curve's value at a slide time t:

1. Find the first key whose time is greater than t.
2. Before the first key, the first key's value. After the last key, the last
   key's value. Two keys at the same time: the later one's value.
3. Between key a and key b, with control times 0:
   - s = (t - a.time) / (b.time - a.time), u = 1 - s
   - value = u³·a.value + 3·a.c1_value·s·u² + 3·a.c2_value·s²·u + s³·b.value

   That is the cubic Bezier on the values, with s linear in time.

With non-zero control times the title solves the time cubic for s (82F2E3B8,
Cardano at 82F2DAC0). No retail key needs that, and the corpus test checks
so. A linear key has its controls a third and two thirds of the way;
`0, 0, 100, 100` is a smoothstep.

A **dynamic** curve restarts from the current value when its slide starts
(82F2D4B0):

- third = (v1 - v0) / 3
- k1 = (c1 - v0) / third, k2 = (v1 - c2) / third (both 0 when third is 0)
- the first key's value becomes `start`
- nt = (v1 - start) / 3
- c1 = start + nt·k1, c2 = v1 - nt·k2

## Playing it (`carview::SceneTimeline`)

`apps/carview/scene_timeline.h`. A screen keeps one in its model and each
frame binds it to the scene, sends it the contract's events or slides,
advances it by the frame's time, and `SceneScreen::play` writes it into the
frame's bindings.

**Loading.** Every component enters its first slide after the Master Slide at
time 0. A screen whose components start elsewhere puts them there before its
first frame (the title rows go to `Blurred` / `Focused`).

**Entering a slide.**

1. The curves of the slide being left are baked to their current values.
2. The Master Slide's values are set, then the slide's own.
3. The slide's enabled curves start. Dynamic ones restart from the value as
   it stood.
4. The time goes to the slide's start, playing unless the slide starts
   paused.

**A property's value.** The current slide's curve if one runs, else the last
value set (by a slide or SetProperty, which also ends a curve on it), else
the item's base.

**Events.** `fire(event)` runs every action for the event (27 bits) whose
slide, among those that enable it, is its component's current slide. Which
actions run is decided before any of them runs.

**Time.**

- A looping slide wraps.
- A play-once slide holds at its end (`done`), and with bit 4 goes on to the
  next slide.
- A paused slide holds; `set_time` scrubs it.

**Activity.** An element is off when its component's current slide lists it
with `active` 0, or omits it while another slide of the component lists it
The Master Slide's own `active` bytes are ignored. In the Master Slides
checked (the pause rows' and the notification's), every element reads 0,
yet those elements show on the other slides. An element is also off outside
`starttime` / `endtime` when those are set. An off element hides its subtree.
This is inferred (Q184).

**What the draw gets** (`NodeAnim`, per BSG node, by its FBF object):

| Property | Becomes |
|---|---|
| opacity / 100 | the node's own opacity (its parents still multiply) |
| position | a canvas move (the change from the base, through the parent's world scale and angle; y flips) |
| scale | a factor over the base, about the node's position; the subtree follows |
| rotation.z | a turn about the node; the subtree orbits |
| diffuse | a model's colour (through the material item after the model's object) |
| textcolor | a text's colour |

Only the properties the timeline has touched are written. A screen's
`NodeOverride` still applies on top (and a tint there wins), so screens drop
their measured looks when the timeline runs (`scene_timelines_enabled()`;
off, the screens draw as before).

## Where it plays

- **The front end** (`frontend_screens.cpp`, FE-02). `120_P_PRE_SPLASH`'s
  root slides:
  - `Start`: the logo up; `START_BUTTON` grows in over 0.86..1.14 s.
  - `Menu_Intro`: the logo out over 0..0.299 s, the menu in over
    0.493..1.013 s.
  - `Menu`: the menu in over 0..0.507 s.
  - `Start_Intro`: the logo back over 0..0.326 s.
  - `OUTRO`: the menu out over 0..0.549 s.

  Each row plays its own slides:
  - `Blurred`: text 200, tape grey 33.
  - `Focus`: 0.1 s to text 233 on a pink (250, 0, 100) tape; v4's special
    row's medal mode goes to a yellow (255, 221, 62) tape with dark text.
  - `Focused`: loops 2.5 s; the row's Group pulses 0.7 to 0.722 at
    1.04..1.27 s.
  - `Blur`: 0.5 s back.
- **HUD notifications** (`hud_screen.cpp`; assigned as HUD-11, though the
  audit's HUD-11 is the map's reveal flow, Q187). `947_HUD`'s
  `CombinedNotification` component:
  - `SHOW`: its `State` fades in over 0.166 s, growing 0.9 to 1.03 and back
    to 1 at 0.218 s.
  - `HIDE`: it fades out, shrinking to 0.8 over 0.002..0.137 s.
- **The opening titles** (`first_time_career_ui.cpp`). The cards' times come
  from `H_OPENING_TITLES`' `PLAY` slide: each card layer's opacity curve
  has four keys. The curves' values are all 0 and the slide sets opacity 0,
  so the fade itself is not in the scene; the engine ramps over the keys
  (Q186).
- **The pause menu's rows** (`pause_screen.cpp`, MENU-17). `925_PAUSE_MENU`'s
  `Button0..8` components:
  - `FOCUSED`: scale 1.2, the tapes pink (250, 0, 100), the clip-on in.
  - `BLURREDABOVE` / `BLURREDBELOW`: the rows above the focus rest 7 px up,
    the rows below 6 px down. Both captures agree to a pixel or so: MAP
    focused (`07_pause_menu_orig.png`) and QUIT focused
    (`07b_pause_menu_scrolled_orig.png`) put the unfocused rows 12 to 13 px
    apart.
  - `FOCUSABOVE` / `FOCUSBELOW` and `BLURABOVE` / `BLURBELOW` (150 ms) play
    while the focus moves.

  Some parts are not from the scene:
  - The moves are scrubbed by the measured cross-fade (0.1 s), not the
    slides' own 150 ms.
  - The list's own slide (`Option_List`'s `Slide1`, which the game scrubs
    with its `SIZE` events) is not sent. The list keeps its measured
    placement, 7 px higher than before the spread (Q187).
  - The root's `INTRO` and `OUTRO` are 300 ms and change nothing visible
    (the ButtonBar stays at 100); the captures show the menu opening and
    closing at once.

## Screen fades

The flows' fades are not scene slides: they are the `CFadeIn` behaviour of
`gamemodes.zip`'s `state_definitions.xml`, with a `FadeOptions` block
(parser 8285F3A8; `CFadeIn`'s loader 829080C0 keeps it at offset +64). The
block reads `Duration`, `HoldDuration`, `CurveType` (the names `Linear`
= 0 and `S` = 1) and `Color`. The disc's global fades:

| State | Duration | Hold | Curve | Colour |
|---|---|---|---|---|
| `global.fade_in` | 1.0 s | 0 | S | black |
| `global.fade_out` | 1.0 s | 0 | S | black |
| `global.fade_out_half_sec` | 0.5 s | 0 | S | black |

The free-roam flow after a load blacks out and ends on `global.fade_in`.
The engine keeps the three in `race/screen_fade.h` (the S curve taken as
smoothstep, Q244), and `cutscene_corpus_tests` checks them against the
disc's file. One fade now serves every screen:

- **The load hand-off.** The load screen's close is `global.fade_in`: black,
  then 1 s of S curve back to the game (`LoadingScreen::kFadeOutSec`). The
  boot load and the world loads (fast travel, into and out of events) draw
  their own load screens, so they hand the fade to the session's
  (`DriveApp::loading_hand_off`); before this, only the title's load faded,
  over 0.25 s. Capture: `load_handoff_fade_sheet_anim2.png` (0.25 s apart).
- **The campaign's stage changes.** Every `TransitionKind` draws the black
  fade: `fade_out_half_sec` to black, then `fade_in` (it was 0.55 s in
  all, a cubic ease each way). The engine's pink brush wipe, its two slides and
  the "HORIZON COLORADO" card on the wipe are gone.
- **The reset.** Reset-to-road keeps its `GameTunableSettings.ini` timings
  on the same black S-curve fade.

## Decoded, not yet played

- **`H_GENERIC` / `H_GENERIC_TITLE`.** `INTRO` is 300 / 600 ms with no
  visible curve on the root.
- **The map (`H_MAP`).** Its only slides besides the stackers' are the
  event hubs' (`AVAILABLE`, `UNAVAILABLE`, `BRONZE`, `SILVER`, `GOLD`) and
  each activity's (`Upright` / `UprightWarning`). The engine's map does not
  draw those icons from the scene. The nodes it does draw (the cursor,
  popups, title tape and player bar) have no slides. The icons' own
  animations and the rumour reveal are profile animations ("Map profile
  animations" below), now played.
- **The mission HUD (ACT-02).** `Missions/MissionsDamage`'s `BARFILL`
  (1 s) is a gauge scrubbed by its time: `DAMAGEBAR.TGA`'s offset runs from
  -0.049 to -0.79 and `FillBarGradient` widens from 0 to 1.71 over it, so
  the damage's fraction is the time. The engine has no damage for the
  photo shoots and draws its own mission HUD, not `MissionCountdown`,
  `MissionsScoring` or `MissionsDamage` (Q187). The speed trap panel
  (ACT-01) and the chain's score (`ScoreGroup`) are played ("The speed trap
  panel" and "The skill chain's score" below); the `Icon`'s `SPEEDCAM` /
  `AVERAGESPEEDCAM` slides belong to the activation prompt's icon, which
  the engine draws with its own texture.

## The speed trap panel

`947_HUD`'s `SpeedCamera` is its own component, with the slides `OFF`,
`SHOW` (1 s), `ON` (1 s), `OLDPB` and `NEWPB` (4 s), six `COMPARE*` (7 s)
and `ABORT` (2 s). Its rows: `Title` (a pill), `Speed` (the speed and its
unit on a tape), `SpeedPB` (a pill), `PBIcon` (`NEW_PB` on a tape),
`CompLines` (two rival lines and a VS icon), `Summary` (three lines) and
`LeaderboardText` (a button prompt).

What the slides do:
- `SHOW`: `SpeedGroup` fades in over 0.11 s while it grows to 1.02 and back
  by 0.2 s; `Title` and `Speed` are up, `SpeedPB` and `PBIcon` down.
- `ON`: the same, still.
- `OLDPB`: `SpeedPB` slides in from 27 px right (alpha 0.07 to 1 over
  0.2 s), 4 px lower than in `SHOW`.
- `NEWPB`: `Speed` goes down and `PBIcon` pops in (alpha 0.5 to 1, scale
  1.3 to 0.95 to 1 over 0.3 s).
- Both fade `SpeedGroup` out and shrink it to 0.9 from 1.8 to 2 s.
- `ABORT`: `SpeedGroup` fades out over 0.2 s.
- `COMPARE*` / `NOCOMPARE*`: the rival lines and the summary
  (`IDS_Summary_Win` FASTER THAN:, `_Lose`, `_Move`, `_NC` YOU'RE
  RANKED:) and, without `NOLB`, the leaderboard prompt, from 0.2 s to
  about 4.2 s.

The engine (`speed_camera_phase`, `draw_hud_speed_camera` in
`hud_screen.cpp`) plays, from the trap and zone tracker:
- a trap: `SHOW` for its 1 s with the pass's speed, then `NEWPB` or
  `OLDPB`, down at 2 s;
- a zone: `SHOW` as it starts and `ON` while it runs, with the live
  average; `NEWPB` or `OLDPB` as it ends; `ABORT` when it is cancelled.

The title is the camera's name (`List_SpeedCameras`), the speed has two
decimals as the map's popup shows it, the unit is `Activities/IDS_Mph` or
`IDS_KmPerHour`, the personal best line is `IDS_PB`: speed unit and
NEW PB! is `IDS_NewPB`. The comparison slides are not played: the engine
has no leaderboard or rival times offline. The stackers are not run, so
each row is placed on its `Stacker`'s line; the personal best goes one row
below the title (Q242). Without the scene's timeline the engine's own
drawn panel stays.

## The skill chain's score

`DrivingSkill/CompleteComponent/ScoreGroup` (the chain's total and
multiplier) is its own component. Its slides move `anim_helper`:
- `SHOW`: alpha 0 to 1 over 0.2 s while it grows from 0.9 to 1.05 and
  settles at 1 by 0.3 s;
- `HIDE`: alpha 1 to 0 over 0.2 s while it shrinks to 0.95;
- `ABORT`: holds it (alpha 0.97 to 1 over 0.6 s) and starts a 0.6 s timer;
- `DEFAULT`: down.

Its children have their own components:
- `Total`: `DEFAULT` (up, yellow) and `BANK` (2.62 s): `anim_helper` comes
  in from scale 3 at alpha 0 (0.1 s) to full by 0.5 s, overshoots to 1.05
  and settles by 0.8 s, holds, and goes out by 2.3 s; `TEXT_TOTAL` flashes
  white at 0.5 s and is yellow again by 1 s; an event at 2.3 s. `ABORT`
  tilts it off and fades it over 0.4 s.
- `Multiplier`: `SHOWN`, `ANIMATE` (from scale 1.3 to 1 over 0.2 s with a
  flash), `HIDE` (alpha out over 0.3 s), `ABORT`, `HIDDEN`.
- `SkillGroup/Skill1`: `SHOWSKILL` (alpha in over 0.2 s, scale 1.5 to 0.95
  to 1 by 0.3 s, a flash), `SHOWNSKILL`, `WARNINGSKILL` (1.2 s: alpha
  pulses 30/50 % every 0.2 s, then 0), `ABORTSKILL` (tilts off, out by
  0.4 s), and the `*COMBO` versions.
- `PopularityBar`: `SHOW` (1.4 s; the 1st-place background shrinks from
  2.5, the rest is in by 0.75 s), `SHOWN`, `HIDE` (3 s; holds to 2.27 s, out
  by 2.67 s), `LEVEL_UP`, `LEVEL_UP_FINAL`.

`draw_hud_skills` plays `SHOW` from the chain's first skill, `ABORT` for
0.6 s when the chain is lost and then `HIDE`. A bank keeps the group up and
plays `Total`'s `BANK` (the original's banked total is gone about 2.1 s
after it shows, first30b). `Skill1` plays `SHOWSKILL` for 0.6 s as a skill
lands, `WARNINGSKILL` over the chain's last 1.2 s (holding its last dim
step, 0.2 s before its end, while a drift or burnout keeps the chain past
the timer) and `SHOWNSKILL` between; the multiplier plays `ANIMATE` as it
changes. The disc plays `PopularityBar`'s `SHOW` from the bank, then
`HIDE` from 1.4 s (out by 4.07 s), or `SHOWN` for a rank change on its
own; the engine's badge animation still gives the rank and the ring
flash, and the fill arc takes the opacity of the disc's `Backgrounds`.
Each component runs on its own timeline and writes only its own subtree
(`play_slide`'s `subtree_only`): one timeline's resting look would
otherwise overwrite the nodes another moves. Without a timeline, a lost
chain stays 1.5 s and fades over 0.7 s, a banked total fades from 2 to
2.3 s and the disc from 3.67 to 4.07 s (Q243).

## Map profile animations

The map's icon and camera animations are not BGF timelines: they are the
`<animations>` of `MapProfileFullscreen.xml` (the minimap's profile has
none), bound to icon events by each symbolizer's
`<animation event=".." animation=".."/>`. `carview::mapanim`
(`apps/carview/map_anim.h`) reads and samples them; `map_view.cpp` draws the
icons with them and `drive_map_screen.cpp` runs the reveal camera.

**The format** (the title's parsers: animation 82CD6428, frame 82CD5C50,
ease 82CD17F0):
- `<animation name loop speed>`: `loop` missing or `stop` holds the last
  frame, `loop` wraps, `pingpong` runs back and forth; `speed` (default 1)
  multiplies every key time. The length is the last frame's time times the
  speed. Where a name is defined twice (the two `pulse`), the first stays.
- `<frame advance>`: the frame's time is the previous frame's plus
  `advance` (default 0). Its children are keys and triggers at that time.
- `<key type value ease>`: seven tracks. `scaleX` and `posX2` share one,
  `scaleY` and `zoom` one, `angle` and `posY2` one; the others are `posX`,
  `posY`, `alpha` and `colour`.
- `ease`: missing is `both`; `none`, `in`, `out`, `both`, `smooth`; any
  other name is `smooth`.
- `<trigger type value>`: `audio` (a ui4audio play name), `reveal` (`next`),
  `shake` (`camera`) and `set_cursor` (`set`), fired as the time passes
  them (82CD5FC8, 8255AEF8).

**Sampling** (8251BF60): before the first key the first value, from the
last key on the last value; between keys a and b a Catmull-Rom spline
(82CB7D60) whose outer points come from the eases. Before a (8251B980):
`none` / `in` continue the line (2a - b), `out` / `both` repeat b (flat
leaving a), `smooth` takes the key before a (flat if a is the first).
After b (8251BA30): `none` / `out` continue the line (2b - a), `in` /
`both` repeat a (flat arriving at b), `smooth` takes the key after b. A
loop folds the time by the length; ping-pong by twice the length, running
back in the second half (8251C3E0).

**The icons** (`MapScene`'s animation fields): each symbolizer plays the
animation its icon's event binds, or its `default` one from the map's
opening when it binds none:
- `intro` from the opening: the facilities' and career icons' `grow`
  (0.7 at 0.1 s, 1.05 at 0.26 s, 1 at 0.43 s); the hotspot's `hidden`.
- `hover` under the cursor: `bulge` (1 to 1.7 over 0.15 s, 1.5 at 0.25 s,
  straight lines). The captures agree: the outpost goes from 29 px in
  `map_open_orig.png` to 46 px under the cursor in
  `map_popup_outpost_orig.png`. The average speed cameras' line ends use
  `fade_in`, `hidden` otherwise.
- `ghost` on the rest while an icon is hovered (alpha 1 to 0.3 over
  0.3 s), `unghost` when the cursor leaves it (0.3 to 1). The icon hovered
  before ghosts from the moment the cursor moves to another; when the
  cursor leaves for an empty spot it goes back to its `default`.
- The career icons' NEW badge: `fade_out` on hover and ghost, `fade_in` on
  unghost.
- The player's ping (cell 4,4): `radar`, a 3 s loop: at 1.2 s alpha 0.5
  and scale 0 with the `Map_Radar` sound, at 1.8 s alpha 0 and scale 8
  (flat arriving).

Not played: the career unlocks' `reveal` (`reveal_drop`, the smoke layers,
`reveal_new_slide` and their `shake` / `reveal next` triggers), which
belong to the career's map reveal flow; `pulse`, which no fullscreen
symbolizer binds; the camera shake. The pause map redraws at 30 Hz while
it is open so the animations run (the title steps them by at most 1/30 s).

**The camera** (8251CEF8): the title's map camera sits at a distance of
`Map_Distance_Min + (Map_Distance_Max - Map_Distance_Min) * zoom`
(tunables `UI/Map/Map_Distance_Min` 200 and `_Max` 7652, registered at
8315CD48 and the next function); it opens at `UI/Map/Zoom_Default` 0.35
(2808 m), and `UI/Map/Cursor_Zoom_Min` / `_Max` are 0.1 and 0.4. A camera
animation blends the player's camera towards the animation's: the position
towards (`posX`, `posY`) and the distance towards `scaleY`'s distance, both
by `alpha`. The engine has a flat map, so its zoom is taken as the opening
zoom times 2808 m over the distance (Q241).

**The barn rumour reveal** (gamemodes.zip `barnfinds.xml`,
`barnfindspawn_root`): when a rumour starts the title forces the Barn
Finds filter on (`CForceMapFilterStatus`), shows an empty screen, pauses,
triggers the barn's `MapVO` line (`BarnFind<n>AppearsOnMap`, not waited
on), loads the map, plays the barn's `MapAnimation`
(`barn_find_region<k>_unlock`) and runs the normal map flow; leaving the
map unpauses the game. Each unlock animation holds the player's view for
0.6 s, then blends over 1.5 s to its target at a zoom of 0.1 to 0.2, and
fires `set_cursor` at 2.1 s. The engine holds the player's input until
then and puts the cursor at the middle of the view. Barn 1's animation also
has `posX2` / `posY2` keys, which are not used (Q241).

## Tools

- `carview --ui-scene <game root> <scene> bgf` prints the items, contracts,
  slides (values, enabled curves with their keys) and actions.
- `ui_overlay_tests --corpus` reads every disc and v4 BGF's timeline. It
  checks the zero control times and the Master Slides, and the known slides:
  `120_P_PRE_SPLASH`'s, the pause menu's 300 ms `INTRO` / `OUTRO`, and
  `H_OPENING_TITLES`' card keys.
- `scene_screen_tests` checks these on every run, with no shot folder
  needed:
  - the front end's fades;
  - the opening titles' keys, read by the layers' names;
  - the notification's size and alpha;
  - the pause rows' 13 px spread.

  With `SHIFT_SCENE_SCREEN_SHOT_DIR` set, it also draws each with and without
  the timeline:
  - the front end's transition at nine times (`title_anim_*.png`);
  - the notification at six ages (`hud_notify_anim_*.png`);
  - the pause rows (`pause_anim_*.png`).

  With `SHIFT_SCENE_SCREEN_HUD_DIR` set as well, the notification and pause
  frames are drawn over the original's captures.
- `carview --ui-scene ... bgf` also prints each item's properties (its base
  values).
