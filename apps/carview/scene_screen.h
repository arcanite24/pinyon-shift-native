// Scene-driven screens (docs/UI_SCENE_SCREENS.md): a screen is the game's
// own UI4 scene (Scenes/ui4/<name>) with its text boxes bound to string-table
// ids, its row stacker driven from a row list, its states (selected row,
// hidden rows) set as node overrides, and its navigation sounds named by
// ui4audio.xml play names. The pieces here hold no screen-specific data:
// pause_screen.h is the first screen built on them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hud_scene.h"
#include "ui.h"

namespace carview {

// A string in the game's string tables: `table` (e.g. "PauseMenu") and the
// id's IDS_ name, or its 16-bit hash in `raw` when the name is unknown.
// `fallback` is shown when the tables are not loaded or lack the key.
// `engine_label`: the slot is the table's key (so tests can check it exists)
// but the engine shows `fallback` instead (a relabelled retail slot).
struct StringRef {
  std::string_view table;
  std::string_view id;
  uint16_t raw = 0;
  std::string_view fallback;
  bool engine_label = false;
};

// The 32-bit key: fh1::string_hash(table) << 16 | its hash of the id (or
// `raw`).
uint32_t string_key(const StringRef& ref);
// The table text, or nullopt when unresolved (via game_text()).
std::optional<std::string> lookup(const StringRef& ref);
// What the screen shows: the engine label, else the table text, else the
// fallback.
std::string text_of(const StringRef& ref);
// Replaces "{0}" in a table format string.
std::string format1(std::string_view fmt, std::string_view arg0);

// A screen's navigation events and the ui4audio.xml play name each plays
// (resolved through UiAudioMap with the screen's scene name).
enum class UiEvent : uint8_t { kOpen, kClose, kMove, kAccept, kBack, kDeny };
std::string_view ui_play_name(UiEvent e);

// Row selection that wraps (the game's option lists wrap top to bottom).
int wrap_row(int selected, int by, int count);
// The first row of a `window`-row view that keeps `selected` in view.
int window_first(int selected, int count, int window);

// One frame of a scene screen: resolves the scene through ui_scene() each
// time it is made (never cached, so a UI.zip overlay applies), finds nodes by
// path and collects the frame's node overrides.
class SceneScreen {
 public:
  explicit SceneScreen(std::string_view scene_name, bool turn = true);
  bool ok() const { return draw_.ok(); }
  const std::string& name() const { return name_; }
  const SceneDraw& scene() const { return draw_; }
  SceneBindings& bindings() { return b_; }

  // Slash path ("PlayerBar/TopRow/SP/Points") or a bare name; -1: missing.
  int node(std::string_view path, int under = -1) const;
  // The override of a node (by index), made when first asked for.
  NodeOverride& at(int node) { return b_.by_node[node]; }
  // Sets a text node's string / hides a node; false when the node is missing.
  bool text(int node, std::string s);
  bool hide(int node);
  bool show(int node, float alpha = 1.0f);
  // `s` broken into lines (at spaces, keeping its own "\n") that fit the text
  // node's box at the node's size; the box when `width` is 0. Scene texts do
  // not wrap by themselves; draw the result with fit_text off.
  std::string wrap(int node, std::string_view s, float width = 0.0f) const;

  // The canvas -> screen frame for a w x h target (1280x720 scaled to fit,
  // centred).
  static SceneFrame frame(float w, float h);
  // Draws the subtree at `root` (-1: the whole scene) with the overrides.
  int draw(ui::List& l, const SceneFrame& f, int root = -1) const;

 private:
  std::string name_;
  SceneDraw draw_;
  SceneBindings b_;
};

}  // namespace carview
