// The scene's own timeline (docs/UI_SCENE_ANIMATION.md): the slides, curves
// and actions of a UI4 scene's BGF, played as the title's Anark runtime
// plays them. Every component (the scene root, item 0, and each wrapper the
// scene's slides belong to) is on one slide at a time; a slide sets its
// elements' properties when it starts, plays its curves over its time and
// enables the actions its events run (go to a slide, set a property, go to a
// time). A screen keeps one SceneTimeline in its model, binds it to the
// scene it draws each frame, fires the events or goes to the slides the
// game's contract would (FOCUS, START_OUTRO, ...), advances it by the frame's
// time and writes it into the frame's bindings before drawing.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fh1data/ui_scene.h"
#include "hud_scene.h"

namespace carview {

// Off: SceneScreen::play draws the scenes static, as before the timelines
// were read (before/after comparisons; on by default).
void set_scene_timelines_enabled(bool on);
bool scene_timelines_enabled();

class SceneTimeline {
 public:
  // Binds the scene. Another scene (or the same one reloaded) resets the
  // state: every component goes to its first slide after the Master Slide
  // at time 0, as the runtime does when a scene loads.
  void bind(const fh1::UiScene* scene);
  const fh1::UiScene* scene() const { return sc_; }
  bool ok() const { return sc_ && sc_->has_bgf && sc_->bgf.has_timeline; }
  // Back to the scene's load state.
  void reset();

  // The BGF item of a node (by its FBF object), or -1.
  int item_of_node(int node) const;
  int node_of_item(uint32_t item) const;

  // Goes to a component's slide by name (case-sensitive); false: none. The
  // slide's values apply, its dynamic curves start from the current values
  // and its time starts at 0 (playing unless the slide starts paused).
  bool go_to(uint32_t component, std::string_view slide);
  // Fires an event (a contract's state, e.g. "FOCUS", "START_OUTRO"): runs
  // every action for it that its component's current slide enables, held by
  // `owner` (its contract item) or anywhere when -1. Returns how many ran.
  int fire(std::string_view event, int owner = -1);
  // Advances every playing component by `dt` seconds: a looping slide wraps,
  // a play-once slide stops at its end (and plays through to the next slide
  // when its flag says so).
  void advance(float dt);
  // A component's time in ms on its current slide (a paused slide the game
  // scrubs: the pause menu's SIZEn, a progress bar), and whether it plays.
  void set_time(uint32_t component, float ms);
  void set_playing(uint32_t component, bool playing);

  // The component's current slide name (empty: none), its time in ms, and
  // whether a play-once slide has reached its end.
  std::string_view slide(uint32_t component) const;
  float time_ms(uint32_t component) const;
  bool done(uint32_t component) const;

  // A property's current value (the curve's, else the last set value, else
  // the item's own); `fallback` when the item never has it.
  float value(uint32_t item, uint32_t attr, float fallback = 0.0f) const;

  // Writes the state of every node the timeline controls into `b.anim`:
  // visibility in its slide, opacity, position and scale (relative to the
  // scene's base values), rotation, and material and text colours.
  void apply(SceneBindings& b) const;

 private:
  struct Comp {
    int slide = -1;
    float t = 0.0f;
    bool playing = false;
    bool done = false;
  };
  static uint64_t key(uint32_t item, uint32_t attr) { return uint64_t(item) << 32 | attr; }
  bool enter(uint32_t component, int slide);
  void bake(uint32_t component);
  void run(const fh1::UiBgfAction& a);
  float base(uint32_t item, uint32_t attr, float fallback) const;
  bool touched(uint32_t item, uint32_t attr) const;
  bool active(uint32_t item, int depth = 0) const;

  const fh1::UiScene* sc_ = nullptr;
  size_t sig_slides_ = 0, sig_items_ = 0, sig_nodes_ = 0;
  std::unordered_map<uint32_t, Comp> comps_;
  // Values set by slides and actions, by (item, attribute).
  std::unordered_map<uint64_t, float> set_;
  // The curves of the current slides (copies: dynamic ones are restarted).
  std::unordered_map<uint64_t, fh1::UiBgfCurve> curves_;
  std::unordered_map<uint64_t, uint32_t> curve_comp_;  // whose slide time drives it
  // Per item: its component (from the Master Slides) and node.
  std::unordered_map<uint32_t, uint32_t> member_of_;
  std::vector<int> item_node_, node_item_;
  // Per node: its material's item (a model's colour), or -1.
  std::vector<int> node_material_;
  // Per slide: its tracks by item.
  std::vector<std::unordered_map<uint32_t, int>> slide_track_;
  // Items some non-master slide of their component lists (slide elements).
  std::unordered_map<uint32_t, bool> in_child_slide_;
  // Per action: the slides that enable it.
  std::vector<std::vector<int>> action_slides_;
};

}  // namespace carview
