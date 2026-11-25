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
| `scale` | Scale a node and its subtree about the node's own position (a focused row's size step) |

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
