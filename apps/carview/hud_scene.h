// The game's UI scenes drawn as they are laid out (FORMAT-UISCENES): a
// screen finds its nodes by name, draws the models with their own material
// tint and textures, and puts its run-time strings in the scene's text
// boxes. The scene files are read at run time from the player's UI.zip.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fh1data/ui_scene.h"
#include "ui.h"

namespace carview {

// The scene by name (Scenes/ui4/<name>), or null; set by the app.
void set_ui_scenes(std::function<const fh1::UiScene*(const std::string&)> load);
const fh1::UiScene* ui_scene(const std::string& name);
// Monotonic generation bumped whenever scenes or UI textures/atlas loaders change.
uint64_t ui_scene_generation();
void notify_ui_atlas_changed();

class SceneDraw;
// Resolves a SceneDraw for `name`, never permanently caching nullptr before atlas/scenes load.
SceneDraw resolve_scene_draw(const std::string& name, bool turn = false);

// Canvas (1280x720, y down) to screen: ox + x * s, oy + y * s.
struct SceneFrame {
  float ox = 0, oy = 0, s = 1;
  float x(float cx) const { return ox + cx * s; }
  float y(float cy) const { return oy + cy * s; }
};

// Per-node runtime overrides for SceneDraw::draw_subtree().
struct NodeOverride {
  std::optional<std::string> text;
  std::optional<bool> visible;
  std::optional<float> alpha;          // overrides node's effective alpha (0..1)
  std::optional<uint32_t> tint;        // replaces material colour (models) or text colour
  std::optional<std::string> texture;  // overrides model's main texture key
  std::optional<float> visible_width;  // overrides 2-tex crop width (canvas px)
  float dx = 0.0f, dy = 0.0f;          // additional canvas px offset (propagates to descendants)
  // Extra rotation in radians (scene y up: counter-clockwise), added to the
  // node's and its descendants' own angles; each turns about its own centre
  // (a text about its anchor).
  float extra_angle = 0.0f;
  // Turns the node and its descendants about the node's own canvas position
  // (radians, scene y up), as a parent "rotation" node does in the game: a
  // child off the pivot orbits it (a dial's needle and hatches).
  float turn = 0.0f;
  // Scales the node and its descendants about the node's own canvas position
  // (a state's size step, e.g. a focused menu row); 1: none.
  float scale = 1.0f;
  // Stretches the node's and its descendants' models across, each about its
  // own middle (a picture swapped in at another aspect); 1: none.
  float stretch_x = 1.0f;
  // The same, down (a panel cut to its text's lines); 1: none.
  float stretch_y = 1.0f;
  // A two-texture panel whose first texture's offset only slides a dirt
  // pattern (the game wraps it): sample it repeating instead of cropping at
  // the texture's border (108's HUDPanel01 behind the car's stats).
  bool wrap = false;
};

// A node's state from the scene's timeline (scene_timeline.h, written by
// SceneTimeline::apply). The scene's BSG holds the items' base values; these
// replace them in the node's own frame, under any NodeOverride.
struct NodeAnim {
  bool inactive = false;         // not active in its component's slide: the subtree is hidden
  std::optional<float> opacity;  // the node's own opacity, 0..1 (its parents' still multiply)
  float dx = 0.0f, dy = 0.0f;    // canvas px move of the node and its subtree
  float sx = 1.0f, sy = 1.0f;    // scale about the node's own position; the subtree follows
  float turn = 0.0f;             // radians (scene y up) about the node's position; the subtree orbits
  std::optional<std::array<float, 4>> colour;  // a model's material or a text's colour, 0..1 rgba
};

// Binding table passed to SceneDraw::draw_subtree().
struct SceneBindings {
  std::unordered_map<std::string, NodeOverride> by_name;
  std::unordered_map<int, NodeOverride> by_node;
  std::unordered_map<int, NodeAnim> anim;  // by node index (SceneTimeline::apply)
  bool ignore_zero_alpha = false;  // treat BSG opacity==0 as 1.0 unless overridden
  float opacity = 1.0f;            // global opacity multiplier
  float dx = 0.0f, dy = 0.0f;      // global canvas offset
  uint32_t default_text_color = ui::rgba(255, 255, 255, 255);
  bool fit_text = true;

  NodeOverride& node(std::string_view name) { return by_name[std::string(name)]; }
  NodeOverride& index(int idx) { return by_node[idx]; }
};

class SceneDraw {
 public:
  // `turn`: each node drawn turned by its own rotation, about its centre
  // (a text about its anchor), unless the list is already turned; a text
  // drawn at its node's x over y scale.
  explicit SceneDraw(const fh1::UiScene* scene, bool turn = false) : sc_(scene), turn_(turn) {}
  bool ok() const { return sc_ != nullptr; }
  const fh1::UiScene* scene() const { return sc_; }
  // The first node whose object is called `name`, under `under` (-1: any).
  int find(std::string_view name, int under = -1) const;
  // All nodes whose object is called `name`, under `under` (-1: any).
  std::vector<int> find_all(std::string_view name, int under = -1) const;
  // Slash-separated path lookup (e.g. "PopularityBar/Text_Rank") under `under`.
  int find_path(std::string_view slash_path, int under = -1) const;
  // Direct child node indices of `parent`.
  std::vector<int> children_of(int parent) const;
  // A model node's first texture as a ui_texture key; empty: none.
  std::string texture_key_of(int node) const;
  // Object name for a node index (empty if none).
  std::string_view object_name(int node) const;
  // A node's centre and bounds on the canvas (y down).
  std::array<float, 2> at(int node) const;
  std::array<float, 4> rect(int node) const;
  // The scene's own opacity for the node (its parents' times its own).
  float alpha(int node) const;
  // The node's rotation about z (radians, scene y up).
  float angle(int node) const;
  // The node's resolved BSG Z depth.
  float z_depth(int node) const;
  // Draws a model node: the material's colour times its textures. A
  // material with two textures uses real kImageMasked two-texture compositing
  // (border-transparent crop or tiled pattern * mask). `visible` > 0 overrides
  // the visible crop width in canvas pixels. `tint` != 0 replaces the material
  // colour. `tex_override` swaps the main texture. False when nothing could be drawn.
  // `am`: an animated ancestor's (or the node's own) map of the scene's
  // canvas, x' = (ax x + bx, ay y + by), applied before dx/dy.
  struct AnimMap {
    float ax = 1.0f, ay = 1.0f, bx = 0.0f, by = 0.0f;
    bool identity() const { return ax == 1.0f && ay == 1.0f && bx == 0.0f && by == 0.0f; }
    std::array<float, 2> apply(float x, float y) const { return {ax * x + bx, ay * y + by}; }
  };
  bool model(ui::List& l, const SceneFrame& f, int node, float opacity = 1, float dx = 0, float dy = 0,
             float visible = 0, uint32_t tint = 0, std::string_view tex_override = {},
             float extra_angle = 0.0f, float scale_x = 1.0f, float scale_y = 1.0f, float k = 1.0f, float kbx = 0.0f,
             float kby = 0.0f, float kt = 0.0f, bool wrap = false, const AnimMap& am = {}) const;
  // A text node's style (font resolved via fontmap.xml, size; the colour given).
  ui::TextStyle style(int node, const SceneFrame& f, uint32_t colour) const;
  // Draws `text` in a text node's box; returns the width drawn, canvas pixels.
  // `extra_angle` turns it about its anchor on top of its own angle (which
  // applies only when the draw turns).
  float text(ui::List& l, const SceneFrame& f, int node, std::string_view text, uint32_t colour,
             float dx = 0, float dy = 0, bool fit = true, float scale = 1.0f, float extra_angle = 0.0f) const;
  // A text node's box width in canvas pixels (0: none).
  float text_width(int node) const;
  // The advance of `text` in a text node's font and size, canvas pixels.
  float measure(int node, std::string_view text) const;
  // Walks a BSG subtree in Z/painter order, drawing all visible models and
  // bound text nodes with per-node overrides and the timeline's state (anim).
  // Returns the number of drawables emitted.
  int draw_subtree(ui::List& l, const SceneFrame& f, int root_node, const SceneBindings& bindings = {}) const;
  int draw_subtree(ui::List& l, const SceneFrame& f, std::string_view root_name,
                   const SceneBindings& bindings = {}) const;
  // Computes packed SUPER_STACKER positions for `visible_count` items.
  std::vector<float> stacker_positions(float origin, float step, int visible_count) const;

 private:
  const fh1::UiScene* sc_;
  bool turn_ = false;
};

// Tracks smooth ~0.18 s UI4 screen open/close cubic ease-out transitions keyed by active screen ID.
class UiTransitionTracker {
 public:
  static constexpr float kDefaultDurationSec = 0.18f;

  void set_duration(float seconds);
  float duration() const { return duration_; }

  // Updates active screen ID and advances transition time by `dt` seconds.
  // Switching to a new non-empty screen ID starts a fresh open transition from 0.
  // Passing an empty screen ID reverses progress smoothly toward 0 (close transition).
  float step(std::string_view screen_id, float dt);

  // Current cubic ease-out progress (0..1) and raw linear progress (0..1).
  float progress() const;
  float raw_progress() const;
  const std::string& active_screen() const { return active_screen_; }
  bool is_closing() const { return closing_; }
  void reset();

 private:
  std::string active_screen_;
  float elapsed_ = kDefaultDurationSec;
  float duration_ = kDefaultDurationSec;
  bool closing_ = false;
};

UiTransitionTracker& ui_transition_tracker();
float ui_screen_open_progress(std::string_view screen_id, float dt = 0.0f);

}  // namespace carview
