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

## Decoded, not yet played

- **`H_GENERIC` / `H_GENERIC_TITLE`.** `INTRO` is 300 / 600 ms with no
  visible curve on the root.
- **The map (`H_MAP`).** Its only slides besides the stackers' are the
  event hubs' (`AVAILABLE`, `UNAVAILABLE`, `BRONZE`, `SILVER`, `GOLD`) and
  each activity's (`Upright` / `UprightWarning`). The engine's map does not
  draw those icons from the scene. The nodes it does draw (the cursor,
  popups, title tape and player bar) have no slides. MAP-05's animations
  (the icons' `reveal_drop`, `bulge`, `grow` ...) and HUD-11's reveal flow
  are `MapProfileFullscreen.xml` profile animations, not BGF timelines
  (Q187).
- **The activity HUDs (ACT-01, ACT-02).** `947_HUD` has the slides:
  - `SpeedCamera`: `OFF`, `SHOW`, `ON`, `OLDPB` and others;
  - the `Icon`: `SPEEDCAM` / `AVERAGESPEEDCAM`;
  - `MissionsDamage`: `BARFILL`;
  - `ScoreGroup`: `SHOW` / `HIDE` / `ABORT`.

  The engine draws its own speed trap and mission HUDs, not these
  components. Playing the slides waits on drawing those components first
  (Q187).

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
