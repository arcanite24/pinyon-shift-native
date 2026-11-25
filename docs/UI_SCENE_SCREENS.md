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

- **Start count.** The engine's gantry panels stay dark at the FR16 start
  (the panels' `H_DIFF_COUNTDOWN_NOLIGHT_MASKED` materials load flagged
  and textured, and the shipped shaders carry the LED branch, so the
  cause lies past that (the texture or the draw's intensity) and is not
  found), so `TEXT_COUNT` shows 3, 2, 1 for the last three seconds until
  they light (Q74).
- **Minimap.** The engine's map raster is kept: it covers the original's
  visible map (90..270 x 420..600) and the scene's `MATERIAL_VIEWPORT` frame
  is not drawn. The player arrow sits lower than the original's (about
  y 570 against 520).
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
- **Measured placements.** Every scene here rests at its intro start (the
  stackers and timelines are not run), so the drawn positions are offsets
  measured on `10_`..`18_*_orig.png`: the ticket's value tapes, the rival
  callout's lines, the cash lines and total, and the popup's panel and line
  pitch.

## Interims around a race

- **Ticket.** The surface line for surfaces other than asphalt (Q93); the
  value tapes' colours for tickets other than festival ones (Q94).
- **Rival callout.** Its tapes' offsets and its 3 s hold; no event carries a
  star rival yet, so it is not reached in play (Q95, Q98).
- **Points and won car.** Shown at measured rects without their intros'
  end states; no wristband art or car render (Q96).
- **Car select and difficulty.** Car select lists the eligible cars on
  `H_GENERIC_TITLE`'s rows; DIFFICULTY steps the difficulty in place (Q97).
- **Not drawn.** The tombola and the rival offer (Q98).
