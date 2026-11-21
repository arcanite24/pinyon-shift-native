// The game's UI scenes drawn as they are laid out (FORMAT-UISCENES): a
// screen finds its nodes by name, draws the models with their own material
// tint and textures, and puts its run-time strings in the scene's text
// boxes. The scene files are read at run time from the player's UI.zip.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <string_view>

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

class SceneDraw {
 public:
  // `turn`: each node drawn turned by its own rotation, about its centre
  // (a text about its anchor), unless the list is already turned; a text
  // drawn at its node's x over y scale.
  explicit SceneDraw(const fh1::UiScene* scene, bool turn = false) : sc_(scene), turn_(turn) {}
  bool ok() const { return sc_ != nullptr; }
  // The first node whose object is called `name`, under `under` (-1: any).
  int find(std::string_view name, int under = -1) const;
  // A node's centre and bounds on the canvas (y down).
  std::array<float, 2> at(int node) const;
  std::array<float, 4> rect(int node) const;
  // The scene's own opacity for the node (its parents' times its own).
  float alpha(int node) const;
  // The node's rotation about z (radians, scene y up).
  float angle(int node) const;
  // Draws a model node: the material's colour times its textures. A
  // material with two textures is a crop (the first copy's u offset hides
  // the strip from 1 - offset): `visible` > 0 overrides the visible width
  // in canvas pixels. `tint` != 0 replaces the material colour. Offsets
  // move the node in canvas pixels. False when nothing could be drawn.
  bool model(ui::List& l, const SceneFrame& f, int node, float opacity = 1, float dx = 0, float dy = 0,
             float visible = 0, uint32_t tint = 0) const;
  // A text node's style (font, size; the colour given).
  ui::TextStyle style(int node, const SceneFrame& f, uint32_t colour) const;
  // Draws `text` in a text node's box (the node's position taken as the
  // box's left (or its align point) and vertical centre: ours until the
  // spec says); returns the width drawn, canvas pixels.
  float text(ui::List& l, const SceneFrame& f, int node, std::string_view text, uint32_t colour,
             float dx = 0, float dy = 0, bool fit = true) const;
  // A text node's box width in canvas pixels (0: none).
  float text_width(int node) const;

 private:
  const fh1::UiScene* sc_;
  bool turn_ = false;
};

}  // namespace carview
