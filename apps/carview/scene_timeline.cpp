#include "scene_timeline.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace carview {

namespace {
using fh1::UiBgfSlide;
namespace attr = fh1::ui_attr;

constexpr uint32_t kEventMask = 0x07FFFFFFu;

// The node's own value of an attribute the BSG holds (the items' base values).
bool node_value(const fh1::UiNode& n, uint32_t a, float& v) {
  if (a == attr::kPositionX) v = n.position[0];
  else if (a == attr::kPositionY) v = n.position[1];
  else if (a == attr::kPositionZ) v = n.position[2];
  else if (a == attr::kScaleX) v = n.scale[0];
  else if (a == attr::kScaleY) v = n.scale[1];
  else if (a == attr::kScaleZ) v = n.scale[2];
  else if (a == attr::kRotationX) v = n.rotation[0];
  else if (a == attr::kRotationY) v = n.rotation[1];
  else if (a == attr::kRotationZ) v = n.rotation[2];
  else if (a == attr::kOpacity) v = n.opacity * 100.0f;
  else return false;
  return true;
}
}  // namespace

namespace {
bool g_enabled = true;
}  // namespace
void set_scene_timelines_enabled(bool on) { g_enabled = on; }
bool scene_timelines_enabled() { return g_enabled; }

void SceneTimeline::bind(const fh1::UiScene* scene) {
  const bool same = scene == sc_ && scene && sig_slides_ == scene->bgf.slides.size() &&
                    sig_items_ == scene->bgf.items.size() && sig_nodes_ == scene->nodes.size();
  if (same) return;
  sc_ = scene;
  reset();
}

void SceneTimeline::reset() {
  comps_.clear();
  set_.clear();
  curves_.clear();
  curve_comp_.clear();
  member_of_.clear();
  item_node_.clear();
  node_item_.clear();
  node_material_.clear();
  slide_track_.clear();
  in_child_slide_.clear();
  action_slides_.clear();
  sig_slides_ = sig_items_ = sig_nodes_ = 0;
  if (!sc_) return;
  sig_slides_ = sc_->bgf.slides.size();
  sig_items_ = sc_->bgf.items.size();
  sig_nodes_ = sc_->nodes.size();
  if (!ok()) return;
  const auto& d = sc_->bgf;

  // Items and nodes: an element item's value is its FBF object.
  std::unordered_map<uint32_t, int> obj_item;
  for (size_t i = 0; i < d.items.size(); ++i)
    if (d.items[i].kind >= 1 && d.items[i].kind <= 7) obj_item.emplace(d.items[i].value, int(i));
  item_node_.assign(d.items.size(), -1);
  node_item_.assign(sc_->nodes.size(), -1);
  node_material_.assign(sc_->nodes.size(), -1);
  for (size_t n = 0; n < sc_->nodes.size(); ++n) {
    const fh1::UiNode& node = sc_->nodes[n];
    if (auto it = obj_item.find(node.object); it != obj_item.end()) {
      node_item_[n] = it->second;
      if (item_node_[size_t(it->second)] < 0) item_node_[size_t(it->second)] = int(n);
    }
    // A model's material follows its object in the FBF.
    const int oi = node.object_index;
    if (oi >= 0 && sc_->objects[size_t(oi)].kind == fh1::UiObjectKind::kModel &&
        size_t(oi) + 1 < sc_->objects.size() &&
        sc_->objects[size_t(oi) + 1].kind == fh1::UiObjectKind::kMaterial) {
      if (auto it = obj_item.find(sc_->objects[size_t(oi) + 1].id); it != obj_item.end())
        node_material_[n] = it->second;
    }
  }

  slide_track_.resize(d.slides.size());
  action_slides_.resize(d.actions.size());
  for (size_t s = 0; s < d.slides.size(); ++s) {
    const UiBgfSlide& sl = d.slides[s];
    for (size_t t = 0; t < sl.tracks.size(); ++t) {
      const auto& tr = sl.tracks[t];
      slide_track_[s].emplace(tr.item, int(t));
      if (tr.item != sl.component) {
        if (sl.master()) member_of_.emplace(tr.item, sl.component);
        else in_child_slide_[tr.item] = true;
      }
      for (const auto& ref : tr.refs)
        if (ref.action && ref.enabled && ref.index < action_slides_.size())
          action_slides_[ref.index].push_back(int(s));
    }
  }

  // Every component starts on its first slide after the Master Slide; the
  // scene's root first, so nested components start after their parents.
  std::vector<uint32_t> comps;
  for (const auto& sl : d.slides)
    if (std::find(comps.begin(), comps.end(), sl.component) == comps.end()) comps.push_back(sl.component);
  std::sort(comps.begin(), comps.end());
  for (uint32_t c : comps) {
    const auto list = d.slides_of(c);
    int first = -1;
    for (int s : list)
      if (!d.slides[size_t(s)].master()) {
        first = s;
        break;
      }
    if (first < 0 && !list.empty()) first = list.front();
    if (first >= 0) enter(c, first);
  }
}

int SceneTimeline::item_of_node(int node) const {
  return node >= 0 && size_t(node) < node_item_.size() ? node_item_[size_t(node)] : -1;
}

int SceneTimeline::node_of_item(uint32_t item) const {
  return item < item_node_.size() ? item_node_[item] : -1;
}

float SceneTimeline::base(uint32_t item, uint32_t a, float fallback) const {
  if (!ok() || item >= sc_->bgf.items.size()) return fallback;
  for (const auto& [tag, raw] : sc_->bgf.items[item].properties)
    if ((tag & 0x07FFFFFFu) == a) return fh1::ui_bgf_number(tag, raw);
  float v = 0;
  const int n = node_of_item(item);
  if (n >= 0 && node_value(sc_->nodes[size_t(n)], a, v)) return v;
  return fallback;
}

bool SceneTimeline::touched(uint32_t item, uint32_t a) const {
  const uint64_t k = key(item, a);
  return curves_.count(k) != 0 || set_.count(k) != 0;
}

float SceneTimeline::value(uint32_t item, uint32_t a, float fallback) const {
  const uint64_t k = key(item, a);
  if (auto it = curves_.find(k); it != curves_.end()) {
    const auto ct = curve_comp_.find(k);
    const float t = ct != curve_comp_.end() ? time_ms(ct->second) : 0.0f;
    return fh1::evaluate_bgf_curve(it->second, t);
  }
  if (auto it = set_.find(k); it != set_.end()) return it->second;
  return base(item, a, fallback);
}

void SceneTimeline::bake(uint32_t component) {
  for (auto it = curves_.begin(); it != curves_.end();) {
    const auto ct = curve_comp_.find(it->first);
    if (ct != curve_comp_.end() && ct->second == component) {
      set_[it->first] = fh1::evaluate_bgf_curve(it->second, time_ms(component));
      curve_comp_.erase(ct);
      it = curves_.erase(it);
    } else {
      ++it;
    }
  }
}

bool SceneTimeline::enter(uint32_t component, int slide) {
  if (!ok() || slide < 0 || size_t(slide) >= sc_->bgf.slides.size()) return false;
  const auto& d = sc_->bgf;
  const UiBgfSlide& sl = d.slides[size_t(slide)];
  if (sl.component != component) return false;
  // The values the last slide's curves reached stay until something sets them.
  bake(component);

  // The slide's curves; a dynamic one starts from the value now.
  std::vector<std::pair<uint64_t, fh1::UiBgfCurve>> start;
  for (const auto& tr : sl.tracks)
    for (const auto& ref : tr.refs) {
      if (ref.action || !ref.enabled || ref.index >= d.curves.size()) continue;
      fh1::UiBgfCurve c = d.curves[ref.index];
      if (c.dynamic) fh1::restart_bgf_curve(c, value(c.item, c.attr, 0.0f));
      start.emplace_back(key(c.item, c.attr), std::move(c));
    }

  // The Master Slide's values, then the slide's own.
  auto apply_values = [&](const UiBgfSlide& s) {
    for (const auto& tr : s.tracks)
      for (const auto& v : tr.values) set_[key(tr.item, v.attr)] = v.number();
  };
  const auto list = d.slides_of(component);
  if (!list.empty() && d.slides[size_t(list.front())].master() && list.front() != slide)
    apply_values(d.slides[size_t(list.front())]);
  apply_values(sl);

  for (auto& [k, c] : start) {
    curve_comp_[k] = component;
    curves_[k] = std::move(c);
  }

  Comp& st = comps_[component];
  st.slide = slide;
  st.t = float(sl.start_ms);
  st.playing = !sl.paused();
  st.done = false;
  return true;
}

bool SceneTimeline::go_to(uint32_t component, std::string_view name) {
  if (!ok()) return false;
  return enter(component, sc_->bgf.find_slide(component, name));
}

void SceneTimeline::run(const fh1::UiBgfAction& a) {
  const auto& d = sc_->bgf;
  if (a.handler == fh1::ui_handler::kGotoSlide) {
    for (int s : d.slides_of(a.target))
      if (fh1::ui_name_hash(d.slides[size_t(s)].name) == a.arg_tag) {
        enter(a.target, s);
        return;
      }
  } else if (a.handler == fh1::ui_handler::kSetProperty) {
    const uint64_t k = key(a.target, a.arg_tag & 0x07FFFFFFu);
    if (((a.arg_tag >> 27) & 7u) == 5u) return;  // a string (a text's content)
    // A set ends a curve on the property.
    curves_.erase(k);
    curve_comp_.erase(k);
    set_[k] = fh1::ui_bgf_number(a.arg_tag, a.arg);
  } else if (a.handler == fh1::ui_handler::kGotoTime) {
    set_time(a.target, float(a.arg_tag));
  } else if (a.handler == fh1::ui_handler::kPause) {
    set_playing(a.target, false);
  } else if (a.handler == fh1::ui_handler::kPlay) {
    set_playing(a.target, true);
  }
}

int SceneTimeline::fire(std::string_view event, int owner) {
  if (!ok()) return 0;
  const auto& d = sc_->bgf;
  const uint32_t h = fh1::ui_name_hash(event) & kEventMask;
  // Which actions run is decided before any runs (a GOTO changes the slide).
  std::vector<size_t> run_list;
  for (size_t i = 0; i < d.actions.size(); ++i) {
    const auto& a = d.actions[i];
    if ((a.event & kEventMask) != h) continue;
    if (owner >= 0 && a.owner != uint32_t(owner)) continue;
    bool enabled = false;
    for (int s : action_slides_[i]) {
      const auto it = comps_.find(d.slides[size_t(s)].component);
      if (it != comps_.end() && it->second.slide == s) {
        enabled = true;
        break;
      }
    }
    if (enabled) run_list.push_back(i);
  }
  for (size_t i : run_list) run(d.actions[i]);
  return int(run_list.size());
}

void SceneTimeline::advance(float dt) {
  if (!ok() || dt <= 0.0f) return;
  const auto& d = sc_->bgf;
  std::vector<std::pair<uint32_t, int>> through;
  for (auto& [comp, st] : comps_) {
    if (!st.playing || st.done || st.slide < 0) continue;
    const UiBgfSlide& sl = d.slides[size_t(st.slide)];
    const float t0 = float(sl.start_ms), t1 = float(sl.end_ms);
    st.t += dt * 1000.0f;
    if (st.t < t1) continue;
    if (sl.loops() && t1 > t0) {
      st.t = t0 + std::fmod(st.t - t0, t1 - t0);
    } else {
      st.t = t1;
      st.done = true;
      if (sl.play_through()) {
        const auto list = d.slides_of(comp);
        const auto it = std::find(list.begin(), list.end(), st.slide);
        if (it != list.end() && it + 1 != list.end()) through.emplace_back(comp, *(it + 1));
      }
    }
  }
  for (const auto& [comp, s] : through) enter(comp, s);
}

void SceneTimeline::set_time(uint32_t component, float ms) {
  auto it = comps_.find(component);
  if (it == comps_.end() || !ok()) return;
  const UiBgfSlide& sl = sc_->bgf.slides[size_t(it->second.slide)];
  it->second.t = std::clamp(ms, float(sl.start_ms), float(std::max(sl.start_ms, sl.end_ms)));
  it->second.done = false;
}

void SceneTimeline::set_playing(uint32_t component, bool playing) {
  if (auto it = comps_.find(component); it != comps_.end()) it->second.playing = playing;
}

std::string_view SceneTimeline::slide(uint32_t component) const {
  const auto it = comps_.find(component);
  if (it == comps_.end() || !ok() || it->second.slide < 0) return {};
  return sc_->bgf.slides[size_t(it->second.slide)].name;
}

float SceneTimeline::time_ms(uint32_t component) const {
  const auto it = comps_.find(component);
  return it != comps_.end() ? it->second.t : 0.0f;
}

bool SceneTimeline::done(uint32_t component) const {
  const auto it = comps_.find(component);
  return it != comps_.end() && it->second.done;
}

bool SceneTimeline::active(uint32_t item, int depth) const {
  if (depth > 16) return true;
  const auto m = member_of_.find(item);
  if (m == member_of_.end()) return true;  // no component's element: always on
  const uint32_t comp = m->second;
  const auto st = comps_.find(comp);
  if (st == comps_.end() || st->second.slide < 0) return true;
  const int s = st->second.slide;
  const auto& tracks = slide_track_[size_t(s)];
  const auto t = tracks.find(item);
  if (t == tracks.end()) {
    // A Master Slide element is on every slide; one that belongs to other
    // slides is off here.
    if (in_child_slide_.count(item)) return false;
  } else if (!sc_->bgf.slides[size_t(s)].tracks[size_t(t->second)].active) {
    return false;
  }
  const float now = st->second.t;
  if (touched(item, attr::kStartTime) && now < value(item, attr::kStartTime)) return false;
  if (touched(item, attr::kEndTime) && now > value(item, attr::kEndTime)) return false;
  return comp == item || active(comp, depth + 1);
}

void SceneTimeline::apply(SceneBindings& b) const {
  if (!ok()) return;
  const auto& nodes = sc_->nodes;
  for (size_t n = 0; n < nodes.size(); ++n) {
    const int item_i = node_item_[n];
    if (item_i < 0) continue;
    const uint32_t item = uint32_t(item_i);
    NodeAnim an;
    bool any = false;
    if (!active(item)) {
      an.inactive = true;
      any = true;
    }
    if (touched(item, attr::kOpacity)) {
      an.opacity = std::clamp(value(item, attr::kOpacity, 100.0f) / 100.0f, 0.0f, 1.0f);
      any = true;
    }
    if (touched(item, attr::kPositionX) || touched(item, attr::kPositionY)) {
      const float ux0 = value(item, attr::kPositionX) - base(item, attr::kPositionX, 0.0f);
      const float uy0 = value(item, attr::kPositionY) - base(item, attr::kPositionY, 0.0f);
      // In the parent's frame: its world scale and angle (the BSG resolve).
      float psx = 1.0f, psy = 1.0f, pang = 0.0f;
      if (const int p = nodes[n].parent; p >= 0 && size_t(p) < n) {
        psx = nodes[size_t(p)].sx, psy = nodes[size_t(p)].sy, pang = nodes[size_t(p)].angle;
      }
      const float ux = ux0 * psx, uy = uy0 * psy;
      const float c = std::cos(pang), s = std::sin(pang);
      an.dx = c * ux - s * uy;
      an.dy = -(s * ux + c * uy);  // canvas y is down
      any = any || an.dx != 0.0f || an.dy != 0.0f;
    }
    if (touched(item, attr::kScaleX) || touched(item, attr::kScaleY)) {
      const float bx = base(item, attr::kScaleX, 1.0f), by = base(item, attr::kScaleY, 1.0f);
      an.sx = bx != 0.0f ? value(item, attr::kScaleX, 1.0f) / bx : 1.0f;
      an.sy = by != 0.0f ? value(item, attr::kScaleY, 1.0f) / by : 1.0f;
      any = any || an.sx != 1.0f || an.sy != 1.0f;
    }
    if (touched(item, attr::kRotationZ)) {
      an.turn = value(item, attr::kRotationZ) - base(item, attr::kRotationZ, 0.0f);
      any = any || an.turn != 0.0f;
    }
    // Colour: a text's own, a model's material's (0..255).
    const int oi = nodes[n].object_index;
    const bool is_text = oi >= 0 && sc_->objects[size_t(oi)].kind == fh1::UiObjectKind::kText;
    if (is_text && (touched(item, attr::kTextColorR) || touched(item, attr::kTextColorG) ||
                    touched(item, attr::kTextColorB) || touched(item, attr::kTextColorA))) {
      an.colour = std::array<float, 4>{value(item, attr::kTextColorR, 255.0f) / 255.0f,
                                       value(item, attr::kTextColorG, 255.0f) / 255.0f,
                                       value(item, attr::kTextColorB, 255.0f) / 255.0f,
                                       value(item, attr::kTextColorA, 255.0f) / 255.0f};
      any = true;
    } else if (const int mi = node_material_[n]; mi >= 0) {
      const uint32_t mat = uint32_t(mi);
      if (touched(mat, attr::kDiffuseR) || touched(mat, attr::kDiffuseG) || touched(mat, attr::kDiffuseB) ||
          touched(mat, attr::kDiffuseA)) {
        const auto& mc = sc_->objects[size_t(oi) + 1].colour;
        an.colour = std::array<float, 4>{value(mat, attr::kDiffuseR, mc[0] * 255.0f) / 255.0f,
                                         value(mat, attr::kDiffuseG, mc[1] * 255.0f) / 255.0f,
                                         value(mat, attr::kDiffuseB, mc[2] * 255.0f) / 255.0f,
                                         value(mat, attr::kDiffuseA, mc[3] * 255.0f) / 255.0f};
        any = true;
      }
    }
    if (any) b.anim[int(n)] = an;
  }
}

std::vector<float> slide_curve_keys(const fh1::UiScene* scene, uint32_t component, std::string_view slide,
                                    std::string_view object, uint32_t attr) {
  std::vector<float> keys;
  if (!scene || !scene->has_bgf || !scene->bgf.has_timeline) return keys;
  const auto& d = scene->bgf;
  const int s = d.find_slide(component, slide);
  if (s < 0) return keys;
  for (const auto& tr : d.slides[size_t(s)].tracks) {
    if (tr.item >= d.items.size()) continue;
    const int oi = scene->find_object(d.items[tr.item].value);
    if (oi < 0 || scene->objects[size_t(oi)].name != object) continue;
    for (const auto& r : tr.refs) {
      if (r.action || !r.enabled || r.index >= d.curves.size()) continue;
      const auto& cv = d.curves[r.index];
      if (cv.attr != attr) continue;
      for (const auto& k : cv.keys) keys.push_back(k.time_ms);
      return keys;
    }
  }
  return keys;
}

}  // namespace carview
