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
  float extra_angle = 0.0f;            // extra rotation in radians (scene y-up)
};

// Binding table passed to SceneDraw::draw_subtree().
struct SceneBindings {
  std::unordered_map<std::string, NodeOverride> by_name;
  std::unordered_map<int, NodeOverride> by_node;
  bool ignore_zero_alpha = false;  // treat BSG opacity==0 as 1.0 unless overridden
  float opacity = 1.0f;            // global opacity multiplier
  float dx = 0.0f, dy = 0.0f;      // global canvas offset
  float anim_time = -1.0f;         // >= 0 evaluates matching BGF keyframe tracks
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
  bool model(ui::List& l, const SceneFrame& f, int node, float opacity = 1, float dx = 0, float dy = 0,
             float visible = 0, uint32_t tint = 0, std::string_view tex_override = {},
             float extra_angle = 0.0f) const;
  // A text node's style (font resolved via fontmap.xml, size; the colour given).
  ui::TextStyle style(int node, const SceneFrame& f, uint32_t colour) const;
  // Draws `text` in a text node's box; returns the width drawn, canvas pixels.
  float text(ui::List& l, const SceneFrame& f, int node, std::string_view text, uint32_t colour,
             float dx = 0, float dy = 0, bool fit = true) const;
  // A text node's box width in canvas pixels (0: none).
  float text_width(int node) const;
  // Walks a BSG subtree in Z/painter order, drawing all visible models and
  // bound text nodes with per-node overrides and optional BGF track animation.
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

}  // namespace carview
