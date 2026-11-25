# Scene-driven screens

A scene-driven screen draws one of the game's own UI4 scenes
(`Scenes/ui4/<name>.fbf/.bsg/.bgf` in the player's `media/UI.zip`) as it is
laid out, and binds the engine's state into the scene's nodes: texts from the
string tables, hidden or shown nodes, a row's selected state, textures, crops.
The pause menu (`925_PAUSE_MENU`) is the pilot. The HUD, map, post-race and
hub screens follow the same steps.

The code is in `apps/carview`:

| File | What it holds |
|---|---|
| `scene_screen.h/.cpp` | The reusable pieces: `StringRef`, `text_of`, `UiEvent` play names, row navigation, `SceneScreen` |
| `hud_scene.h/.cpp` | The renderer: `SceneDraw::draw_subtree` with `SceneBindings` / `NodeOverride` |
| `pause_screen.h/.cpp` | The pilot: the pause rows, its Sponsor Challenges and Message Centre screens |
| `hud.h` (`PauseMenu`) | The pause menu's model: rows, EXTRAS, sub-screens, the selection |
| `hud_screen.h/.cpp` | The driving HUD on `947_HUD`: dial, race blocks, skills, prompt, notifications, start, wrong way |
| `hud_notify.h` | The notification kinds and icons, and the HUD toggles (`HudToggles`) |
| `help_bar_screen.cpp` | The help bar on `BUTTON_BAR_SCENE` (installed into `ui4::button_bar`) |
| `title_menu.h` | The front end's model: Press Start, the profile step, SINGLE PLAYER / MULTIPLAYER, EXTRAS, popups |
| `frontend_screens.h/.cpp` | The front end on `120_P_PRE_SPLASH` and the load screen on `2_C_INI_LOADING` |
| `tests/scene_screen_tests.cpp` | The corpus test: scenes, nodes, strings, cues; optional renders |

## The pieces

### Scenes: `SceneScreen`

```cpp
SceneScreen sc("925_PAUSE_MENU");          // resolved through ui_scene() now
const int text = sc.node("Button0/Button/Text");   // slash path or a bare name
sc.text(text, "MAP");
sc.hide(sc.node("Layer2"));
sc.at(sc.node("Button0")).scale = 1.21f;   // any NodeOverride field
sc.draw(list, SceneScreen::frame(w, h));   // the whole scene, or a subtree
```

- Make a `SceneScreen` for each frame you draw. It resolves the scene through
  `resolve_scene_draw()` / `ui_scene()` every time, so a UI.zip overlay (the
  v4 `media.zip` scenes) or a reload takes effect. Never keep a `SceneDraw` in
  a function-local `static`: it pins the first scene, or a null one if the
  scene was not loaded yet.
- Node positions in a loaded scene are already canvas positions (1280x720,
  y down); `SceneDraw::at` / `rect` give them.
- A name can occur in several places (every row has a `Dirt_Tile`, and so does
  its `MessageIndicator`). Take the direct child you mean with
  `children_of` (`child()` in `pause_screen.cpp`), not `find(name, under)`,
  which returns the first descendant.
- Nodes whose scene alpha is 0 (timers, popups, the MP bars) stay hidden unless
  you `show()` them.
- Draw order is the nodes' scene z, back to front. The scenes' cameras sit at
  z -600 looking down +z, so the smaller z is nearer and draws later (a row's
  text at -14 over its tapes at -10 and -11); equal z keeps tree order.
- Texts shrink to their box unless `bindings().fit_text` is false. The
  original draws most labels at full size past their boxes (SPONSOR
  CHALLENGES, COLLECT BLUE WRISTBAND), so the pilot turns it off.
- Scene texts do not wrap. `sc.wrap(node, s, width)` breaks a string at spaces
  to the node's box (or `width`) at the node's size; draw it with
  `fit_text` off.
- Many text nodes are right-aligned at their position (TEXT_CAR, TEXT_CR,
  TEXT_POSITION, TEXT_PROGRESS): to put one's right edge at x, set
  `dx = x - at(node).x`.
- Draw anything the scene cannot (an interim pane) into the list before
  `sc.draw()`, so the scene draws over it. To put such a pane over one scene
  node, draw that node's subtree first (`sc.draw(l, f, node)`), then the pane,
  then `hide(node)` and draw the scene (the sponsor screen's details panel).

### `NodeOverride`

| Field | Use |
|---|---|
| `text` | A text node's string (text nodes draw only with one) |
| `visible`, `alpha` | Hide a node and its subtree, or show one the scene keeps at 0 |
| `tint` | A model's material colour or a text's colour (the selected row's pink) |
| `texture` | Swap a model's texture (the wristband, the class badge, sponsor logos) |
| `visible_width` | A two-texture tape's crop width in canvas pixels (a row's tape follows its text) |
| `dx`, `dy` | Move a node and its subtree |
| `extra_angle` | Turn a node |
| `turn` | Turn a node and its subtree about the node's own position, children orbiting it (the dial's needle and hatches) |
| `scale` | Scale a node and its subtree about the node's own position (a focused row's size step) |
| `stretch_x` | Stretch the models of a node and its subtree across, each about its middle (a sponsor logo of another aspect) |
| `stretch_y` | The same, down (the STAY IN CAR panel cut to its message's lines) |

### Strings: `StringRef`

```cpp
StringRef r{"PauseMenu", "IDS_Map", 0, "MAP"};        // table, IDS name, -, fallback
StringRef raw{"SponsorChallengeScreen", {}, 0x17D6, "POPULARITY CHART POSITION"};  // unknown name: the id
text_of(r);   // the table text, else the fallback
lookup(r);    // the table text or nullopt (what the corpus test checks)
format1(text_of(pause_text::kCredits), thousands(cr));   // "{0} Cr"
```

- The key is `fh1::string_hash(table) << 16 | fh1::string_hash(id)`, looked up
  as `_&<key>` through `game_text_raw()`. `lookup` strips the markup as
  `game_text()` does but keeps the `{0}`..`{9}` format slots, which
  `game_text()` would remove, so `format1(text_of(ref), arg)` works.
- When the id's `IDS_` name is not known, find the text in the tables and use
  the 16-bit hash in `raw`.
- `engine_label = true` keeps a retail key for the slot but shows the
  fallback. The pause menu's SETTINGS row uses it: the retail second row is
  MULTIPLAYER (`PauseMenu` 0xDD6B), and the engine puts its settings there, as
  the recomp's settings option does.
- Strings that no table has (the engine's own, e.g. EXTRAS) are a `StringRef`
  with no table and `engine_label = true`, so they are visible in one place.
- List every string a screen binds where the corpus test can reach it
  (`pause_text::*` in `pause_screen.h`).

### Sounds: `UiEvent`

`ui_play_name(UiEvent)` gives the `ui4audio.xml` play name of a navigation
event: open `Pause`, close `UnPause`, move `Vscroll`, accept `Accept`, back
`Cancel`, deny `DenyAccess`. The app plays them as `audio::UiCue` (`kPause`,
`kUnPause`, `kVscroll`, `kAccept`, `kCancel`), which `SfxEngine` preloads by
those play names through `UiAudioMap`. `UiAudioMap::resolve(play, scene)`
applies a scene's override (e.g. `924_POST_RACE` uses `PostRace`); add the
new screen's scene name when you check its cues.

### Rows

- `wrap_row(selected, by, count)`: the game's option lists wrap.
- `window_first(selected, count, window)`: the first row shown when a list is
  longer than the scene's row slots.
- The selected row: its tapes tinted pink and the `Clipon` shown, the others
  black, the focused row one size step larger
  (`kPauseFocusScale`). The selection cross-fades over about three frames
  (`kPauseFadeSeconds`); the model records when the drawn selection changed
  (`PauseMenu::note_shown`).
- Rows past the list's end are hidden; the remaining slots keep their places
  (the scene's stacker packs from the top).
- Add a `ui::kHitRow` hit zone per row for the mouse.

## The driving HUD

`hud_screen.cpp` draws `947_HUD`'s blocks from `build_drive_hud`
(`game_hud_core.cpp`, `game_hud_race.cpp`, `game_hud_freeroam.cpp`). Every
`draw_hud_*` returns false when the scene or its node is missing, and the
caller then draws the old hand-drawn block.

- **Frames.** The HUD scene is laid out for 1280x720. `hud_frame(anchor, w,
  h, s)` scales it by the HUD scale and pins it to the screen edge its block
  sits on (the race blocks at the top corners, the prompt at the top, the toasts at
  the bottom, the start at the centre), so a wider screen keeps each block at its
  edge.
- **Blocks.** The dial (`GAUGE_ANALOG`; its scale from `dial_scale`, Q73), the
  race blocks (`EndConditionBlock`, `PlaceBlock`, `HUDTimes`,
  `PositionIndicators`), the skill chain and popularity disc
  (`DrivingSkill`, `PopularityBar`; the multiplier's value stays the chain's,
  only its text is in `skill_multiplier_text`), the event prompt
  (`ActivationPrompt`), the notifications (`CombinedNotification`,
  `PopularityUnlock`), the start (`I_321_Go`) and the wrong way sign
  (`WrongWay/Sign_Top`).
- **Notifications.** `notify_strings()` gives each `NotifyKind` its tag,
  title and message keys; `DriveSession::Popup::notify` makes a toast from a
  kind and its `{0}` arguments, and `draw_hud_notification` stacks them. The
  sponsor rank toast is calibrated on the oracle `race-hud-notification.png`;
  the other kinds' pairings of title and message are inferred from the
  string tables.
- **Help bar.** `ui4_chrome` is in `carview_render` and the scene code in
  `carview_hud`, so `ui4::button_bar` calls a drawer installed by
  `install_scene_help_bar()` (at start-up in `drive_helpers_runtime.cpp`, and
  in the corpus test). Without it, `button_bar` draws its old pills; the
  `ui4_parity_tests` check that path.
- **The start.** The original counts 0:05..0:01 on the start gantry's LEDs and
  shows only GO! on screen. The world clock that `world.frag`'s gantry
  branch reads (`set_tod`'s time) runs from the race clock in a race (6 s
  before the signal at 0:05) and stays past the count elsewhere
  (`drive_world_sync.cpp`), so it jumps at race entry and exit (water,
  flags and crowds jump with it; not checked whether a transition hides it).
  The gantry panels stay dark either way (see the interims), so this has no
  visible effect on the count yet.
- **Toggles.** The HUD options (`HudToggles`: lap time and position, wrong
  way warning, names over cars, popularity and skills) are settings rows
  named from `InGame/IDS_HudOption_*` and reach the HUD through
  `DriveSession::live_hud` and `DriveHud::hud`.
- **Calibration.** `scene_screen_tests` renders `build_drive_hud` over the
  original captures (`SHIFT_SCENE_SCREEN_HUD_DIR`, the captures folder) or
  the oracles (`SHIFT_SCENE_SCREEN_ORACLE`) into
  `SHIFT_SCENE_SCREEN_SHOT_DIR` (`hud_*.png`), with each offset in a named
  constant next to the capture it came from.

## The map

`map_screen.cpp` draws `H_MAP` over the engine's map raster
(`MapView::pause_map`, flat and top-down; Q87), with the session glue in
`drive_map_screen.cpp`, the cursor and zoom in `map_cursor.h` and the
filter in `map_filter.h`.

- **Cursor.** The map opens centred on the player with the cursor on the
  player's arrow (`map_open_orig.png`); near the map's edge the view stays
  inside the map and the cursor opens off the centre, still on the arrow.
  The left stick, the D-pad and WASD/arrows move a free cursor
  (`MAP_OVERLAY/CURSOR`) at 490 px/s, slowed to 0.45 times over an icon.
  Past 190 px across or 120 px up from the centre the map scrolls under it
  at 470 px/s; it stops at x 96..1184, y 88..635. Within 30 px of an icon
  (or the player's arrow, a found sign or a revealed barn) it locks on
  (`CURSOR_SNAP`, tinted faint white and scaled to about 54 px as in
  `map_popup_outpost_orig.png`); the icon grows 1.25 times and the others
  dim to 0.6.
- **Zoom.** The right stick's y (or the wheel) zooms about the cursor at 1.9
  per second, from the opening scale (the most zoomed out) to 2.6 times it.
- **Popup.** `MAP_OVERLAY/DETAIL` shows the icon's group: `CareerRace`
  (title, class chip `PICLASS/CLASS_<c>_SM`, points, credits), `StreetRace`
  (the hub's medals from its events' best places), `Outpost` (one pip per
  mission and the fee's discount), `Generic` (two lines: a festival place's
  `IDS_Festival_*_Line1/2`, a speed trap's PB) or `Simple` (one line: the
  player's name, a sign's `IDS_Flyer` and number, a barn). The box is cropped
  to its widest row and centred on the icon with its last row about 60 px
  above it, measured from `map_popup_*_orig.png`.
- **Help bar.** A SET ROUTE (CLEAR ROUTE when a route is set; A then clears
  it wherever the cursor is), B EXIT MAP, X for the icon (FAST TRAVEL (n Cr)
  on a destination, DETAILS on an event or hub) and LB FILTER; the player
  bar is `925_PAUSE_MENU`'s, bound by `bind_pause_player_bar`.
- **Filter.** LB opens `H_FIXED_SETTING_LIST` titled `IDS_FilterMap_Title`
  over the blurred map: the twelve InGame rows (All, New, Not Won, Rivals,
  Festival Events, Showcases, Star Showdowns, Outposts, Street Race Hubs,
  Speed Cameras, Discount Signs, Barn Finds), all ticked at first. A ticks a
  row; A on All ticks or clears every row, and All is ticked exactly when
  every row is. An icon shows when any ticked row covers it (Q88);
  facilities (Race Central, the Autoshow and so on) always show. Eight rows
  show, drawn with the scene's five bars on one leaning line, and the list
  scrolls only when the cursor passes its first or last row.
- **Rumours.** A barn rumour is the profiles' `barn_find_region` ellipse,
  flat white (a=80 on the pause map, solid with a faint black edge on the
  minimap); the barn icon sits inside it.

## Fast travel

`freeroam/fast_travel.h` quotes a destination from `GlobalRegistry.xml`
(`Activities/Outposts/TotalCost`, 10,000 Cr, less `Discount_<n>_Missions`
for the missions done at that outpost: 10,000 / 7,500 / 5,000 / 0). The
festival places are free; events, hubs, traps, signs and barns are not
destinations, and an outpost must be discovered (Q90).

- **Confirm.** X shows `H_MESSAGE_POPUP` titled PauseMenu `IDS_FastTravel`
  with `IDS_FastTravelDesc` (or `_Free`) and OK / CANCEL; short of credits it
  says `IDS_FastTravelDesc_Failed` with OK alone
  (`map_fasttravel_confirm_orig.png`). OK takes the fee, saves and reloads
  the world at the outpost's spawn behind the loading screen.
- **Stunt discount.** A first completion of an outpost's mission shows the
  stunt's completed headline with GasStation `IDS_MissionCompletedDiscount`
  (FAST TRAVEL DISCOUNT EARNED) as its message; there is no travel offer
  (Q91).
- **Trainer.** With the trainer's fast travel anywhere every destination is
  free, undiscovered outposts too, and X on the bare cursor travels there.

## Porting the next screen

1. **Find the scene.** Look up the screen's scene in the scene registry and
   dump it: `carview --ui-scene <UI.zip> <SCENE>` prints the objects (fonts,
   sizes, box widths, materials, textures) and the nodes (canvas positions,
   rotations, scales, alphas). Compare it with a capture of the original.
2. **Find the strings.** For every text the original shows, find its table and
   id (the tables dumped to JSON; hash the `IDS_` names you know). Write them
   as `StringRef`s next to the screen.
3. **Model.** Keep the screen's state (rows, selection, sub-screen) in a plain
   struct with no renderer dependency, so `ui_tests` can check it.
4. **Bind.** Write a `draw_<screen>(ui::List&, ..., w, h)` that makes a
   `SceneScreen`, binds texts, hides what the engine has no data for, sets
   the selection state, and draws. Draw the help prompts with
   `ui4::button_bar` (it follows the active glyph set and adds hit zones).
5. **Calibrate.** Render the screen headlessly (step 7's renders) over the
   original capture and line it up. Put any measured offset in one named
   constant with the capture it came from (`kListDx` / `kListDy`).
6. **Route.** Draw it in place of the old hand-drawn path, keeping that path
   as the fallback when the scene is missing (`draw_pause_screen` returns
   false and `build_drive_hud` falls back to `menu_panel`).
7. **Test.** Unit-test the model, string keys and cues in `ui_tests`. Add the
   scene, its node paths, its strings and its cues to
   `tests/scene_screen_tests.cpp` (corpus; run it with `SHIFT_GAME_ROOT` and
   `SHIFT_STATE_ROOT`; it exits 77 without them, which is not a pass). Set
   `SHIFT_SCENE_SCREEN_SHOT_DIR` (and `SHIFT_SCENE_SCREEN_BACKDROP` to a 1280x720
   capture) to write its renders.
8. **Capture.** Take before and after captures with the engine harness at the
   same moment as the original capture and list what still differs.

## What is not decoded

- The BGF state blocks (`SELECTED`, `UNSELECTED`, `FOCUSED`, ...) and their
  timelines are not mapped to node properties yet, so the selection look and
  its timing are measured from captures (open and close in one frame, the
  selection cross-fade about three frames, the new row already at its larger
  size). Replace `kPauseFocusScale`, `kPauseFadeSeconds` and the tints when the
  states are decoded.
- Stackers are not run: a scene's packed rows keep their laid-out places. The
  pause list's offset (`kListDx`, `kListDy`) and the player bar's packing
  (`kObjectiveRight`, `kPillPad`, `fit_pill`) are measured on
  `07_pause_menu_orig.png` instead.
- The `BigPane01` panes (the sponsor rows' backs, the popularity and reward
  boxes, the message centre's description box) draw as 1024x512 quads far
  from where the original shows them; their crop is not decoded. The sponsor
  screen hides them and draws INTERIM panes measured on
  `27_sponsor_challenges_orig.png` (`kSponsorBackPane` and the rest in
  `pause_screen.cpp`).
- The popularity bar fills from the 250th place (`kPopularityChartSize`),
  inferred from one capture (166th toward 150th fills 84%).

## Interims in the pilot

- **Message Centre.** Offline, the original answers MESSAGE CENTER with a "NO
  NETWORK ACCESS" popup over the pause menu
  (`29_message_center_offline_orig.png`). The engine opens
  `802_MESSAGE_CENTER` with its title and an empty inbox (NO MESSAGES / YOU
  HAVE NO MESSAGES / Please check back later.) on a plain pane; the scene's
  list, tabs and images are hidden.
- **Sponsor Challenges.** The scene's texts, logos, bars and layout with the
  measured panes above. The original's BUY POPULARITY SPIKE prompt is not
  shown (the engine has no tokens).
- **EXTRAS.** The engine's own screens (events, garage, upgrades, paint, ...)
  sit behind X EXTRAS in the help bar, in the same list with the scene's
  screen-title text reading EXTRAS. The original has neither.

## Interims in the HUD

- **Minimap.** The engine's map raster is kept: it covers the original's
  visible map (90..270 x 420..600) and the scene's `MATERIAL_VIEWPORT` frame
  is not drawn. Its camera is fixed, with the player's arrow 100 px down the
  180 px panel (y 520 as in the original; Q89); the roads are the profile's
  white line over its soft under-line, with no driven tint, and traffic and
  the Festival's drivers are the profile's vehicle icon. The original's wide
  fading ribbon on the player's own road is not drawn.
- **Map details.** X DETAILS on an event or hub opens the engine's events
  page at that hub; the original shows the event's ticket over the map
  (`map_popup_event_details_orig.png`, Q92). A speed trap has no X
  LEADERBOARD (the engine has no leaderboards). The selected event's course
  outline (pink, `map_popup_event_details_back_orig.png`) is not drawn. The
  original zooms out about a sixth past its opening scale
  (`map_zoom_out_orig.png`); the engine stops at the opening scale. The
  engine's view stops where the background texture's paint ends (its
  black side margins never show); at the west edge the original stops
  about 100 px (opening scale) further in
  (`map_streethub_search_w_orig.png`: about 140 px of desert west of
  Carson's highway curve at its scale, the engine about 285 px at its
  own). In the live render the FILTER title tape is greyer and
  taller than the original's white bar, and the list's scroll indicator is
  drawn by hand (a track and a thumb at the cursor's row) in place of the
  scene's. The icons' open and pulse animations and the barn rumour's
  reveal animation are not ported, and travel keeps the loading screen
  without the original's fast travel camera.
- **Not ported.** `DirectionArrows`; the notifications' tape slide (they
  fade); the Show me! action (hidden, no action behind it); the ghosts and
  rewind message toggles (the engine has neither element).
- **Engine wording kept.** The outpost and hub prompts, the locked prompt's
  reason, the lap split line under `HUDTimes`, and X EXTRAS in the pause help
  bar.
- **Extras outside the layout.** The performance overlay, trainer, touch
  controls and the tracked achievement card stay the engine's; the card goes
  under whatever the HUD draws top left, following the race's timer rows.
- **Help bar.** More than seven prompts (the scene has seven buttons) keep the
  old drawn bar.

## The screens around a race

`race_flow.h` holds the original's order of screens around an event (its
flow files) as plain steps, with the timings measured on the captures;
`race_screens.cpp` draws each step on its own scene, and
`drive_race_screens.cpp` runs them in the drive session.

- **Entry.** The marker opens `H_EVENT_SUMMARY` (the event's ticket
  texture, track, surface, points and credits; Y turns the ticket to its
  restrictions). A car that does not qualify leads to car select instead of
  a toast. A 0.5 s fade, then the load names the event.
- **Pre-race.** `PRE_AND_POST_RACE`'s title card for 1.8 s, then
  `H_GENERIC_TITLE` (START RACE, DIFFICULTY, OPTIONS, QUIT RACE), and the
  rival callout where the event has a star rival. A restart shows only the
  menu.
- **Post-race.** `H_POST_RACE_FINISHED` (X results, B replay, Y restart);
  then the AI cars go and the car waits at the post-race location;
  `H_POST_RACE_CREDITS_BREAKDOWN` (it moves on by itself),
  `H_POST_RACE_CAREER_POINTS`, `H_POST_RACE_WON_CAR`, STAY IN CAR on
  `H_MESSAGE_POPUP` when the event lent a car, then black and the load back
  to free roam. Head to heads, Rivals, showcases and Rally skip the steps
  the original skips (`race_flow::post_steps`).
  The flow's only post-race speech (`radiodj.post_race`) comes before the
  results, so the campaign's call-ins and objective tracker wait from the
  finish until free roam (`DialogQueue::set_held`).
- **Measured placements.** Every scene here rests at its intro start (the
  stackers and timelines are not run), so the drawn positions are offsets
  measured on `10_`..`18_*_orig.png`: the ticket's value tapes, the rival
  callout's lines, the cash lines and total, and the popup's panel (cut
  to the message's lines) and line pitch.

## Interims around a race

- **Ticket.** The surface line for surfaces other than asphalt (Q93); the
  value tapes' colours for tickets other than festival ones (Q94).
- **Rival callout.** Its tapes' offsets and its 3 s hold; no event carries a
  star rival yet, so it is not reached in play (Q95, Q98).
- **Points and won car.** Shown at measured rects without their intros'
  end states; no wristband art or car render (Q96).
- **Car select and difficulty.** Car select lists the eligible cars on
  `H_GENERIC_TITLE`'s rows; DIFFICULTY steps the difficulty in place (Q97).
- **Not drawn.** The tombola and the rival offer (Q98); the title and
  rival tapes' specular sheens (Q99).

## The front end and the load screen

The boot plays the original's two splashes, `T10_MS_Combined` then
`Dolby_Corona_Intro` (`kBootSplashes`), then the title over the looping
`PressStart.wmv`. After 53 s idle on Press Start the original replays
`forza_intro.wmv` (`kAttractMovie`) and comes back to Press Start.

- **Press Start.** `120_P_PRE_SPLASH`'s `Content` group (the logo and
  `TitleTapeText` reading PRESS START) moved by one measured offset
  (`title_layout::kContentDx/Dy`, matched to the pixel on
  `01_title_pressstart_orig.png`). The scene's own Kinect and controller
  prompts (`Start`), install pane, help bar, video placeholder and fader are
  hidden. No help bar, as the original.
- **Profile step.** Press Start's outro and the menu's intro
  (`TitleMenu::kProfileSeconds`): the logo fades, then the menu shows. A save
  that exists but does not read shows the table's INVALID PROFILE popup once
  the menu is up.
- **Menu.** `SPLASH_MENU`'s rows: SINGLE PLAYER (`Main/IDS_Horizon`) and
  MULTIPLAYER. The game mounts title update v4's scene, whose first row is
  `SPLASH_MENU_BUTTON_SPECIAL` (its `NormalMode` drawn, its medal-badged
  `SpecialMode` hidden) and which has no `BREADCRUMB`; the disc's
  `SPLASH_MENU_BUTTON1` serves without v4 (`title_row_nodes`). The selected row on its pink tapes (`Crisp_Tape`,
  `smudge_tape`) cut to its text, a size step up; the other plain, light grey,
  moved in (`02_title_menu_orig.png`). Help bar A SELECT, B BACK. SINGLE
  PLAYER continues a played career through the load screen, or starts the
  first-time career; MULTIPLAYER shows the original's NO NETWORK ACCESS
  popup (`LiveStrings`, the title it shows for features that need Xbox
  LIVE). B goes back to Press Start.
- **EXTRAS.** The engine's NEW CAREER (with a confirm over a save), FREE
  ROAM, SETTINGS and QUIT sit behind X EXTRAS in the help bar, on the same
  rows, as the pause menu has its extras. The original has none of them.
- **Load screen.** `2_C_INI_LOADING`: the tip's title on `Title_Text`'s black
  tape, its message wrapped on `Body_Text`'s `Line_n` pills (dark text on
  white), `Info`'s PLEASE WAIT and turning `LoadSpinner`, over `BG` with
  `LOADING/BGLOADINGIMAGES/IMAGE1` (the stage every capture shows). No
  progress bar. `Stat_Text` (the big pink title) carries the event's name on
  event loads (an engine request) and is hidden otherwise. The texts' offsets
  against their tapes and pills are measured on `03_loading_tip_orig.png`
  (`loading_layout`).
- **Tips.** Each load takes its LoadingDefs.xml group's `tipcount` tips, at
  random or, with `order="sequential"`, the next in file order (a cursor per
  group for the session), each held its `displaytime`. Free roam uses
  `freeroam`; an event `festival_N` by its level (N = level + 1: its
  wristband's star rival's tips, `11b_event_loading_orig.png`),
  `festival_heats` for a qualifier, `streetrace`, `showcase`, else
  `festivalevent` (`loading_group_for_event`).
- **Timing.** The load from the title into free roam holds 1 s at least,
  loads into and out of an event 3 s, fast travel and a restart none
  (`LoadingScreen::kTitleMinSeconds`, `kEventMinSeconds`).
- **Sound.** Loads use the Loading mix: the radio ducked to 0.25, no menu
  music bed, no VO.

## Interims in the front end

- **Profile step.** A fixed 0.35 s with a fade: the `START_OUTRO` and
  `MENU_INTRO` slides are not decoded; no profile picker or storage device
  (Q137).
- **Menu states.** The selected row's size step and offset, the unselected
  row's indent and grey are measured, not read from the scene's states;
  `menu_shadow` is hidden (Q138).
- **Popups.** NO NETWORK ACCESS and INVALID PROFILE draw on
  `H_MESSAGE_POPUP` as STAY IN CAR does; the original's exact popup for
  MULTIPLAYER offline on PC is not recorded (Q138).
- **Tips.** Which value picks `festival_N` (the event's level or the
  player's wristband) is ambiguous in the one capture; a long load comes
  round to its first tip again; the other load contexts (paint shop,
  workshop, missions, online) are not wired (Q139).
- **Load screen.** The spinner's rate and the event name's place on
  `Stat_Text` are set by eye (Q140).
- **Not started.** An autosave or storage notice: no scene or string for
  one was found (Q142).

## The 1000 Club

`club_menu.h` holds the 1000 Club's screens as a model (`ClubMenu`, no
renderer; `club_rally_tests` drives it) over what the drive fills from the
save and the package (`ClubMenuData`). `club_screens.cpp` draws each
screen on the title update's own scene (the v4 overlay), and
`drive_club.cpp` runs them from the pause row.

- **Pause row.** 1000 CLUB (`CarChallenges/IDS_PauseMenu_CarChallenges`)
  sits under MAP, then HORIZON RALLY (`Main/IDS_HorizonRally`), only when
  the package is owned (offers 2700 and 1600; `PauseMenu::has_club`,
  `has_rally`). Without it, the engine's own rows stay in the extras.
- **First visit.** The package's intro video
  (`Splash_Intros/car_challenges_intro`), then `H_CAR_CHALLENGE_HUB` with
  the tutorial's welcome on `H_MESSAGE_POPUP`. Until the first medal the
  hub has one ticket (the first achievement); A opens its cars
  (`H_CAR_CHALLENGE_BUY_CARSELECT`, the car-list tutorial), A on a car
  opens SELECT ACTION (`H_POPUP_MENU`), and BEGIN CHALLENGES IN THIS CAR
  starts the car's challenges (a car swap when it is not the driven one).
  The steps are saved (`club_tutorial` 0..4) so each popup shows once.
- **Hub.** The tickets are a carousel (Ticket4 is the focused slot): the
  current car's challenges once begun, the next achievement to earn, and
  VIEW ALL CHALLENGES after the first medal. The achievement ticket opens
  its cars; VIEW ALL opens `H_CAR_CHALLENGE_ACHIEVEMENTS` (the ten
  achievements with their progress and gamerscore); the car ticket opens
  `H_CAR_CHALLENGE_DETAILS` (the car's challenges, their descriptions and
  hints).
- **Driving HUD.** `947_HUD`'s `HUDCarChallengePrompt`: up to six items
  (the medal number, the running description, then MEDAL EARNED! or
  FAILED for `kClubResultHold`), PRESS (D-pad) TO VIEW CAR CHALLENGE
  DETAILS at the bottom, and the tutorial's next step in `HUDInstruction`
  (top left). It replaces the hand-drawn strip, banner and medal ceremony.
- **Strings.** Every string comes from `carchallenges` (`club_text`); the
  tables leave the count out of IDS_MedalsRequired and IDS_CarsRequired
  (" MEDALS REQUIRED"), so `format_count` puts it in front.
- **Measured placements.** As around a race, the scenes rest at their intro
  start, so the drawn offsets are measured on the captures
  (`club_*_orig.png`): the popup panels' cut, the car list's row pitch and
  its dark backing, and the HUD prompt's line.

## Interims in the 1000 Club

- **Screens.** The home ticket's achievement name and description, the car
  list's flag, drivetrain, stats, purchase and pack nodes and its PlayerBar
  are hidden; the car list's help bar is SELECT CAR / BACK where the
  capture shows SELECT CAR / TOGGLE / SEARCH; the details rows' focus is a
  pink header; the popup panels and rows are cut to measured boxes, not by
  the scenes' timelines (Q143).
- **Medals elsewhere.** The Autoshow and garage do not show per-car medals
  yet; `freeroam::club::car_medals()` is the hook (Q143).
- **HUD.** The medal cue is the achievement reward, not the sign smash; the
  COMPLETE and FAIL animations are not run (a 1 s large medal and a 3 s
  hold instead); the item layout is the scene's rest, with no capture of a
  running challenge to measure against (Q144).

## Horizon Rally's entry

The HORIZON RALLY pause row opens `draw_rally_entry_popup` on
`H_MESSAGE_POPUP`: the yellow-wristband gate (`rally/IDS_FirstTimeCareerError`)
or the quit-to-Rally confirmation (`rally/IDS_QuitHorizonToRally`, A OK,
B CANCEL). The first OK plays `Forza_Tone_Rally` and saves `rally_flow`,
then switches worlds. The intro race, first Autoshow and hub tutorial are
not run yet (Q145).

## The hub's menus and car lists

- **Option lists on `H_GENERIC`** (`hub_menus.h`). MY PROFILE, OPTIONS,
  the workshop, the paint booth, the upgrade choice and photo mode are the
  original's string-table rows on one scene (`20_`, `21_`, `22_`,
  `31_*_orig.png`), its list placed per row count. The pause menu's
  SETTINGS, MY PROFILE and PHOTO MODE open them; B on the first list goes
  back to the pause menu. An engine page reached from one is on X (OPTIONS'
  ENGINE SETTINGS); MY PROFILE replaces the career page as the entry, and
  the career page stays as an extra on the pause menu's extras.
- **Car lists on `108_C_BUY_CARSELECT`** (`car_list_screen.h`,
  `drive_car_lists.cpp`). One draw, parameterised by title, tiles, card and
  help bar:
  - the Autoshow's car select, titled HORIZON RECOMMENDS for the first car
    and CAR SELECT for a make, with the price under the cursor
    (`19_autoshow_orig.png`);
  - MY GARAGE, the garage's cars in class then PI order with no line under
    the cursor (`11_car_select_orig.png` is the same scene for an event);
  - BARN FINDS, every barn with its state icon and Barnfind.str's state
    under the cursor.
  The cursor sits on the strip's fourth slot. The card shows the model
  (`Data_Car.ModelShort`), year and make, the country flag
  (`List_Country.IconPath`), class and PI, the drivetrain icon
  (`UI4/Common/Drivetrain_<drive>_<placement>`) and the five rating bars
  over its dirt panel. Y TOGGLE hides the card.
- **The manufacturer select on `115_C_BUY_MFRSELECT`** (`draw_make_list`,
  `fill_make_list`). The Autoshow's first step: the makes' logos
  (`List_CarMake.IconPath` on `MANU_IMAGE`, held clear by the scene until
  the list fills it) on the same seven-slot strip as 108, the make's name
  under the cursor, its count of cars on `Available` and A SELECT, B BACK,
  Y ALL AFFORDABLE CARS. `Autoshow::strip_makes` is the strip; B from the
  car select comes back to the make it left.
- **Option pages on `300_I_OPT_ASSISTS_DIFFICULTY`** (`options_screen.h`).
  OPTIONS's DIFFICULTY and HUD (`24_`, `25_*_orig.png`): the settings
  page's rows by their own names, in the original's order and words
  (Assists.str, InGame.str), the pink bar on the selected row, the row's
  description, DIFFICULTY's TOTAL RACE BONUS, and A SAVE, B CANCEL (the
  rows as they were when the page opened), Y RESET TO DEFAULT (the page's
  rows only; GAME DIFFICULTY then follows the assists it reset). The
  engine's other rows (STABILITY CONTROL, its HUD extras) stay on its own
  pages, behind OPTIONS's X ENGINE SETTINGS.
- **Popups.** `draw_message_popup` draws `H_MESSAGE_POPUP` with a title, a
  wrapped message and its own help bar (STAY IN CAR, and the garage's
  COLLECT CAR, NOT READY, NOT DISCOVERED and CAN'T SELL BARN FIND).
- **Two-texture panels.** A pane whose first texture's offset only slides
  its dirt pattern (108's `HUDPanel01` behind the card) takes
  `NodeOverride::wrap`, which samples the pattern repeating instead of
  cropping it at the texture's border.

## Interims in the hub

- **Car lists.** The card's photo and the tiles are the cars' thumbnails
  on the studio sweep (Q128). The panes behind the strip are drawn as a
  measured band (their crop is not decoded). SEARCH steps the Autoshow's
  all-cars toggle and the garage's class filter. MY GARAGE has no buy-car
  tile, current-car icon or list view (Q130).
- **Manufacturer select.** No capture: the help bar, the strip's contents
  and the info panel are guesses (Q130). The info panel is hidden because
  every `List_CarMake.Profile` is "N/A"; the engine's Hidden & Showcase tab
  is a text tile after the makes.
- **Rooms.** Each room is the engine's studio stage, the Autoshow's dark
  (Q129).
- **Rows.** The workshop's order (Q131); the paint booth's vinyl rows open
  the livery editor (Q132); TEAM CREDITS shows a notice (Q134). OPTIONS's
  CONTROLS, AUDIO and VIDEO open the engine's pages.
- **Option pages.** GAME DIFFICULTY's values and RESET TO DEFAULT's
  defaults are the engine's (Q135). The description panel's dirt is a
  measured dark panel (its crop is not decoded).
- **Barn Finds.** Which popups offer VIEW ON MAP, and the undiscovered and
  collected tiles (Q133).
- **Engine extras kept.** The garage's sorts (O, left stick), favourites
  (F, V), and selling (Q, Del, LB) work but are not on the help bar; the
  manufacturer strip's paging (Q, E, LB, RB); the livery editor; the
  engine's settings pages.
