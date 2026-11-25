#include "hud_scene.h"

#include <algorithm>
#include <cmath>

#include "game_hud.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

std::function<const fh1::UiScene*(const std::string&)>& scenes() {
  static std::function<const fh1::UiScene*(const std::string&)> f;
  return f;
}

uint64_t& scene_gen() {
  static uint64_t g = 1;
  return g;
}

std::unordered_map<std::string, std::pair<uint64_t, const fh1::UiScene*>>& resolved_cache() {
  static std::unordered_map<std::string, std::pair<uint64_t, const fh1::UiScene*>> c;
  return c;
}

// The texture's key for ui_texture: the Horizon.zip member without .xds,
// or "T:" and the Textures.zip member.
std::string texture_key(const std::string& path) {
  bool horizon = false;
  std::string m = fh1::ui_texture_member(path, horizon);
  m.resize(m.size() - 4);
  for (char& c : m)
    if (c == '\\') c = '/';
  return horizon ? m : "T:" + m;
}

uint32_t colour_of(const std::array<float, 4>& c, float alpha) {
  auto b = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
  return ui::rgba(b(c[0]), b(c[1]), b(c[2]), b(c[3] * alpha));
}

}  // namespace

void set_ui_scenes(std::function<const fh1::UiScene*(const std::string&)> load) {
  scenes() = load;
  ++scene_gen();
  resolved_cache().clear();
  ui4::set_scene_loader(std::move(load));
}

uint64_t ui_scene_generation() { return scene_gen(); }

void notify_ui_atlas_changed() {
  ++scene_gen();
  resolved_cache().clear();
}

const fh1::UiScene* ui_scene(const std::string& name) { return scenes() ? scenes()(name) : nullptr; }

SceneDraw resolve_scene_draw(const std::string& name, bool turn) {
  const uint64_t gen = scene_gen();
  auto& cache = resolved_cache();
  if (auto it = cache.find(name); it != cache.end() && it->second.first == gen && it->second.second != nullptr) {
    return SceneDraw(it->second.second, turn);
  }
  const fh1::UiScene* sc = ui_scene(name);
  if (sc != nullptr) {
    cache[name] = {gen, sc};
  }
  return SceneDraw(sc, turn);
}

int SceneDraw::find(std::string_view name, int under) const {
  if (!sc_) return -1;
  for (size_t i = 0; i < sc_->nodes.size(); ++i) {
    const fh1::UiNode& n = sc_->nodes[i];
    if (n.object_index < 0 || sc_->objects[size_t(n.object_index)].name != name) continue;
    if (under < 0) return int(i);
    for (int p = n.parent; p >= 0; p = sc_->nodes[size_t(p)].parent)
      if (p == under) return int(i);
  }
  return -1;
}

std::vector<int> SceneDraw::find_all(std::string_view name, int under) const {
  std::vector<int> out;
  if (!sc_) return out;
  for (size_t i = 0; i < sc_->nodes.size(); ++i) {
    const fh1::UiNode& n = sc_->nodes[i];
    if (n.object_index < 0 || sc_->objects[size_t(n.object_index)].name != name) continue;
    if (under < 0) {
      out.push_back(int(i));
      continue;
    }
    for (int p = n.parent; p >= 0; p = sc_->nodes[size_t(p)].parent) {
      if (p == under) {
        out.push_back(int(i));
        break;
      }
    }
  }
  return out;
}

int SceneDraw::find_path(std::string_view slash_path, int under) const {
  if (!sc_ || slash_path.empty()) return -1;
  int cur = under;
  size_t pos = 0;
  while (pos < slash_path.size()) {
    const size_t next = slash_path.find('/', pos);
    const std::string_view seg =
        next == std::string_view::npos ? slash_path.substr(pos) : slash_path.substr(pos, next - pos);
    if (!seg.empty()) {
      cur = find(seg, cur);
      if (cur < 0) return -1;
    }
    if (next == std::string_view::npos) break;
    pos = next + 1;
  }
  return cur;
}

std::vector<int> SceneDraw::children_of(int parent) const {
  std::vector<int> out;
  if (!sc_) return out;
  for (size_t i = 0; i < sc_->nodes.size(); ++i)
    if (sc_->nodes[i].parent == parent) out.push_back(int(i));
  return out;
}

std::string_view SceneDraw::object_name(int node) const {
  if (!sc_ || node < 0 || size_t(node) >= sc_->nodes.size()) return {};
  const int oi = sc_->nodes[size_t(node)].object_index;
  if (oi < 0 || size_t(oi) >= sc_->objects.size()) return {};
  return sc_->objects[size_t(oi)].name;
}

std::array<float, 2> SceneDraw::at(int node) const {
  if (!sc_ || node < 0) return {640, 360};
  const fh1::UiNode& n = sc_->nodes[size_t(node)];
  return {640 + n.x, 360 - n.y};
}

std::array<float, 4> SceneDraw::rect(int node) const {
  if (!sc_ || node < 0) return {0, 0, 0, 0};
  const fh1::UiNode& n = sc_->nodes[size_t(node)];
  const auto [cx, cy] = at(node);
  // Bounds are in the node's frame (y up); the canvas rect is y down.
  return {cx + n.bounds_min[0] * n.sx, cy - n.bounds_max[1] * n.sy, cx + n.bounds_max[0] * n.sx,
          cy - n.bounds_min[1] * n.sy};
}

float SceneDraw::angle(int node) const { return sc_ && node >= 0 ? sc_->nodes[size_t(node)].angle : 0; }

float SceneDraw::alpha(int node) const { return sc_ && node >= 0 ? sc_->nodes[size_t(node)].alpha : 0; }

float SceneDraw::z_depth(int node) const { return sc_ && node >= 0 ? sc_->nodes[size_t(node)].z : 0; }

namespace {
// k R(t) (x, y) + (bx, by) on the canvas (y down; t counter-clockwise).
std::array<float, 2> turn_point(float x, float y, float k, float t, float bx, float by) {
  const float c = std::cos(t), s = std::sin(t);
  return {k * (c * x + s * y) + bx, k * (-s * x + c * y) + by};
}
}  // namespace

bool SceneDraw::model(ui::List& l, const SceneFrame& f, int node, float opacity, float dx, float dy, float visible,
                      uint32_t tint, std::string_view tex_override, float extra_angle, float scale_x,
                      float scale_y, float k, float kbx, float kby, float kt) const {
  if (!sc_ || node < 0 || size_t(node) >= sc_->nodes.size()) return false;
  if (opacity <= 1e-4f) return false;
  const fh1::UiNode& n = sc_->nodes[size_t(node)];
  if (n.object_index < 0 || sc_->objects[size_t(n.object_index)].kind != fh1::UiObjectKind::kModel) return false;
  // The material and its textures follow the model in the FBF.
  const fh1::UiObject* mat = nullptr;
  const fh1::UiObject* tex[2] = {};
  int ntex = 0;
  for (size_t k = size_t(n.object_index) + 1; k < sc_->objects.size(); ++k) {
    const auto& q = sc_->objects[k];
    if (q.kind == fh1::UiObjectKind::kMaterial && !mat) mat = &q;
    else if (q.kind == fh1::UiObjectKind::kTexture && mat && q.owner == mat->id) {
      if (ntex < 2) tex[ntex] = &q;
      ++ntex;
    } else break;
  }
  const fh1::UiGeometry* geo = sc_->geometry_of(node);
  auto r = rect(node);
  if (geo && (r[2] <= r[0] + 1e-4f || r[3] <= r[1] + 1e-4f)) {
    const auto [cx_n, cy_n] = at(node);
    r = {cx_n + geo->bounds_min[0] * n.sx, cy_n - geo->bounds_max[1] * n.sy, cx_n + geo->bounds_max[0] * n.sx,
         cy_n - geo->bounds_min[1] * n.sy};
  }
  if (std::fabs(scale_x - 1.0f) > 1e-4f || std::fabs(scale_y - 1.0f) > 1e-4f) {
    const float rcx = 0.5f * (r[0] + r[2]);
    const float rcy = 0.5f * (r[1] + r[3]);
    const float hwx = 0.5f * (r[2] - r[0]) * scale_x;
    const float hwy = 0.5f * (r[3] - r[1]) * scale_y;
    r = {rcx - hwx, rcy - hwy, rcx + hwx, rcy + hwy};
  }
  r[0] += dx, r[2] += dx, r[1] += dy, r[3] += dy;
  // A scaled or turned ancestor (NodeOverride::scale / turn):
  // x' = k R(kt) x + (kbx, kby).
  if (k != 1.0f || kbx != 0.0f || kby != 0.0f || kt != 0.0f) {
    const float cx = 0.5f * (r[0] + r[2]), cy = 0.5f * (r[1] + r[3]);
    const float hx = 0.5f * (r[2] - r[0]) * k, hy = 0.5f * (r[3] - r[1]) * k;
    const auto p = turn_point(cx, cy, k, kt, kbx, kby);
    r = {p[0] - hx, p[1] - hy, p[0] + hx, p[1] + hy};
    if (visible > 0.0f) visible *= k;
  }
  // The node's own rotation (scene y up: counter-clockwise on screen).
  const float eff_angle = n.angle + extra_angle + kt;
  const bool turning = (turn_ || extra_angle != 0.0f || kt != 0.0f) && eff_angle != 0.0f && !l.turned();
  if (turning) l.turn(f.x(0.5f * (r[0] + r[2])), f.y(0.5f * (r[1] + r[3])), -eff_angle);
  struct Unturn {
    ui::List& l;
    bool on;
    ~Unturn() {
      if (on) l.unturn();
    }
  } unturn{l, turning};
  const uint32_t c = tint ? ui::fade(tint, opacity) : colour_of(mat ? mat->colour : std::array<float, 4>{1, 1, 1, 1}, opacity);
  if (!tex_override.empty()) {
    if (ui4::glyph_set() != ui4::GlyphSet::kXbox &&
        ui4::draw_controller_texture(l, f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), tex_override, opacity)) {
      return true;
    }
    std::array<float, 4> uv{};
    if (!ui_texture(std::string(tex_override), uv)) return false;
    l.image(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), uv, c);
    return true;
  }
  if (ntex == 0) {
    if (geo && !geo->is_unit_quad && geo->vertices.size() >= 3) {
      const auto [cx_n, cy_n] = at(node);
      const float stroke = std::max(1.5f, 2.0f * f.s);
      for (size_t vi = 0; vi < geo->vertices.size(); ++vi) {
        const auto& a = geo->vertices[vi];
        const auto& b = geo->vertices[(vi + 1) % geo->vertices.size()];
        const float ax = f.x(cx_n + a[0] * n.sx * scale_x + dx);
        const float ay = f.y(cy_n - a[1] * n.sy * scale_y + dy);
        const float bx = f.x(cx_n + b[0] * n.sx * scale_x + dx);
        const float by = f.y(cy_n - b[1] * n.sy * scale_y + dy);
        l.line(ax, ay, bx, by, stroke, c);
      }
      return true;
    }
    l.rect(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), c);
    return true;
  }
  if (ntex < 2) {
    const std::string key = texture_key(tex[0]->path);
    if (ui4::glyph_set() != ui4::GlyphSet::kXbox &&
        ui4::draw_controller_texture(l, f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), key, opacity)) {
      return true;
    }
    std::array<float, 4> uv{};
    if (!ui_texture(key, uv)) return false;
    const auto& m = tex[0]->matrix;
    auto U = [&](float u) { return uv[0] + (uv[2] - uv[0]) * u; };
    auto V = [&](float v) { return uv[1] + (uv[3] - uv[1]) * v; };
    const float u0 = m[12], u1 = m[12] + m[0];
    float v0 = std::min(m[13], m[13] + m[5]), v1 = std::max(m[13], m[13] + m[5]);
    // A whole-texture offset (947_HUD's REDLINE maps v 1..2): the same texels
    // under wrap addressing.
    if (const float whole = std::floor(v0 + 1e-4f); whole != 0.0f && v1 - whole <= 1.0f + 1e-4f) v0 -= whole, v1 -= whole;
    float x0 = r[0], x1 = r[2], a = u0, b = u1;
    if (b > a && (a < 0 || b > 1)) {
      const float w = (x1 - x0) / (b - a);
      if (a < 0) x0 -= a * w, a = 0;
      if (b > 1) x1 -= (b - 1) * w, b = 1;
      if (x1 <= x0) return false;
    }
    l.image(f.x(x0), f.y(r[1]), f.x(x1), f.y(r[3]), {U(a), V(v0), U(b), V(v1)}, c);
    return true;
  }

  // Real two-texture masked / crop multiply mode (SPEC-UI-SCREENS):
  // color = material_rgba * tex0(uv0_transformed) * tex1(uv1).
  std::array<float, 4> uv0{}, uv1{};
  const std::string key0 = texture_key(tex[0]->path);
  const std::string key1 = texture_key(tex[1]->path);
  const bool have0 = ui_texture(key0, uv0);
  const bool have1 = ui_texture(key1, uv1);
  if (!have0 && !have1) return false;
  if (!have0 || !have1) {
    const auto& uv = have1 ? uv1 : uv0;
    const auto& m = (have1 ? tex[1] : tex[0])->matrix;
    auto U = [&](float u) { return uv[0] + (uv[2] - uv[0]) * u; };
    auto V = [&](float v) { return uv[1] + (uv[3] - uv[1]) * v; };
    const float u0 = std::clamp(m[12], 0.0f, 1.0f), u1 = std::clamp(m[12] + m[0], 0.0f, 1.0f);
    const float v0 = std::clamp(std::min(m[13], m[13] + m[5]), 0.0f, 1.0f);
    const float v1 = std::clamp(std::max(m[13], m[13] + m[5]), 0.0f, 1.0f);
    l.image(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), {U(u0), V(v0), U(u1), V(v1)}, c);
    return true;
  }

  const float full = r[2] - r[0];
  if (full <= 0.0f) return false;

  const auto& m0 = tex[0]->matrix;
  const auto& m1 = tex[1]->matrix;
  // Sub-rect for tex1 (mask / base strip).
  auto U1 = [&](float u) { return uv1[0] + (uv1[2] - uv1[0]) * u; };
  auto V1 = [&](float v) { return uv1[1] + (uv1[3] - uv1[1]) * v; };
  const float b_u0 = std::clamp(std::min(m1[12], m1[12] + m1[0]), 0.0f, 1.0f);
  const float b_u1 = std::clamp(std::max(m1[12], m1[12] + m1[0]), 0.0f, 1.0f);
  const float b_v0 = std::clamp(std::min(m1[13], m1[13] + m1[5]), 0.0f, 1.0f);
  const float b_v1 = std::clamp(std::max(m1[13], m1[13] + m1[5]), 0.0f, 1.0f);
  const std::array<float, 4> mask_uv = {U1(b_u0), V1(b_v0), U1(b_u1), V1(b_v1)};

  const float su0 = (std::fabs(m0[0]) > 1e-4f) ? m0[0] : 1.0f;
  const float sv0 = (std::fabs(m0[5]) > 1e-4f) ? std::fabs(m0[5]) : 1.0f;
  const bool tiled = (std::fabs(su0) > 1.05f || std::fabs(sv0) > 1.05f);

  float ou0 = m0[12];
  float ov0 = 0.0f;
  if (!tiled && m0[5] < 0.0f) {
    ov0 = 1.0f - m0[13];
  } else if (!tiled && m0[5] > 0.0f) {
    ov0 = m0[13];
  }
  if (visible > 0.0f && !tiled) {
    // Dynamic crop width: shifts tex0 so its right feathered edge lands at `visible`.
    const float vis = std::clamp(visible, 0.0f, full);
    ou0 = 1.0f - vis / full;
  }
  uint32_t mask_flags = 0u;
  if (tiled) {
    mask_flags = ui::kMaskRepeat;
  } else if (tex[0]->tex_address_mode == 4u || visible > 0.0f || std::fabs(ou0) > 1e-4f || std::fabs(ov0) > 1e-4f) {
    mask_flags = ui::kMaskBorderTransparent;
  }
  l.image_masked(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), uv0, mask_uv, c, {su0, sv0, ou0, ov0}, mask_flags);
  return true;
}

ui::TextStyle SceneDraw::style(int node, const SceneFrame& f, uint32_t colour) const {
  ui::TextStyle ts{.size = 12 * f.s, .color = colour, .font = ui::Font::kB, .keep_case = true};
  if (!sc_ || node < 0 || size_t(node) >= sc_->nodes.size()) return ts;
  const fh1::UiNode& n = sc_->nodes[size_t(node)];
  if (n.object_index < 0) return ts;
  const fh1::UiObject& o = sc_->objects[size_t(n.object_index)];
  if (o.kind != fh1::UiObjectKind::kText) return ts;
  ts.font = ui::resolve_font_name(o.font);
  ts.size = float(o.text_size) * n.sy * f.s;
  ts.align = o.text_align == 1 ? ui::Align::kCenter : o.text_align == 2 ? ui::Align::kRight : ui::Align::kLeft;
  if (turn_ && n.sy > 0) ts.stretch = n.sx / n.sy;
  return ts;
}

float SceneDraw::text_width(int node) const {
  if (!sc_ || node < 0 || size_t(node) >= sc_->nodes.size()) return 0;
  const fh1::UiNode& n = sc_->nodes[size_t(node)];
  if (n.object_index < 0) return 0;
  const fh1::UiObject& o = sc_->objects[size_t(n.object_index)];
  return o.kind == fh1::UiObjectKind::kText ? o.text_width * n.sx : 0;
}

float SceneDraw::measure(int node, std::string_view s) const {
  if (!sc_ || node < 0 || size_t(node) >= sc_->nodes.size() || s.empty()) return 0;
  return ui::List::measure(s, style(node, SceneFrame{}, 0xffffffffu));
}

float SceneDraw::text(ui::List& l, const SceneFrame& f, int node, std::string_view s, uint32_t colour, float dx,
                      float dy, bool fit, float scale, float extra_angle) const {
  if (!sc_ || node < 0 || size_t(node) >= sc_->nodes.size() || s.empty()) return 0;
  ui::TextStyle ts = style(node, f, colour);
  ts.size *= scale;
  const float box = text_width(node) * f.s * scale;
  if (fit && box > 0)
    for (float w = ui::List::measure(s, ts); w > box && ts.size > 6 * f.s; w = ui::List::measure(s, ts))
      ts.size *= std::max(0.8f, box / w);
  const auto [cx, cy] = at(node);
  const float angle = (turn_ ? sc_->nodes[size_t(node)].angle : 0.0f) + extra_angle;
  const bool turning = angle != 0 && !l.turned();
  if (turning) l.turn(f.x(cx + dx), f.y(cy + dy), -angle);
  const float w = l.text(f.x(cx + dx), f.y(cy + dy) - 0.5f * ts.size, s, ts);
  if (turning) l.unturn();
  return w / f.s;
}

int SceneDraw::draw_subtree(ui::List& l, const SceneFrame& f, std::string_view root_name,
                            const SceneBindings& bindings) const {
  const int root = root_name.empty() ? -1 : find(root_name);
  if (!root_name.empty() && root < 0) return 0;
  return draw_subtree(l, f, root, bindings);
}

int SceneDraw::draw_subtree(ui::List& l, const SceneFrame& f, int root_node, const SceneBindings& bindings) const {
  if (!sc_ || bindings.opacity <= 1e-4f) return 0;
  const size_t ncount = sc_->nodes.size();
  if (root_node >= int(ncount)) return 0;

  // Evaluate BGF state records, keyframe tracks, and animation channels onto their real property targets.
  struct BgfEval {
    std::optional<float> alpha;
    std::optional<float> dx;
    std::optional<float> dy;
    std::optional<float> scale_x;
    std::optional<float> scale_y;
    std::optional<float> rotation;
    std::optional<float> color_r;
    std::optional<float> color_g;
    std::optional<float> color_b;
    std::optional<uint32_t> color_rgba;
  };
  std::unordered_map<uint32_t, BgfEval> bgf_eval;
  if (sc_->has_bgf && (bindings.anim_time >= 0.0f || bindings.state_hash != 0)) {
    std::unordered_map<uint32_t, uint32_t> hash_to_fbf;
    for (const auto& it : sc_->bgf.items) {
      if (it.value != 0) hash_to_fbf[it.name_hash] = it.value;
    }
    auto apply_prop = [&](uint32_t item_hash, fh1::UiBgfPropTarget tgt, float val, uint32_t raw_val) {
      if (tgt == fh1::UiBgfPropTarget::kUnknown) return;
      const auto it = hash_to_fbf.find(item_hash);
      if (it == hash_to_fbf.end()) return;
      BgfEval& ev = bgf_eval[it->second];
      switch (tgt) {
        case fh1::UiBgfPropTarget::kAlpha: ev.alpha = std::clamp(val, 0.0f, 1.0f); break;
        case fh1::UiBgfPropTarget::kPosX: ev.dx = val; break;
        case fh1::UiBgfPropTarget::kPosY: ev.dy = val; break;
        case fh1::UiBgfPropTarget::kScaleX: ev.scale_x = val; break;
        case fh1::UiBgfPropTarget::kScaleY: ev.scale_y = val; break;
        case fh1::UiBgfPropTarget::kRotation: ev.rotation = val; break;
        case fh1::UiBgfPropTarget::kColorR: ev.color_r = std::clamp(val, 0.0f, 1.0f); break;
        case fh1::UiBgfPropTarget::kColorG: ev.color_g = std::clamp(val, 0.0f, 1.0f); break;
        case fh1::UiBgfPropTarget::kColorB: ev.color_b = std::clamp(val, 0.0f, 1.0f); break;
        case fh1::UiBgfPropTarget::kColorRgba: ev.color_rgba = raw_val; break;
        case fh1::UiBgfPropTarget::kUnknown: break;
      }
    };
    if (bindings.state_hash != 0) {
      for (const auto& st : sc_->bgf.styles) {
        for (const auto& rec : st.states) {
          if (rec.state_hash != bindings.state_hash) continue;
          const auto tgt = rec.target != fh1::UiBgfPropTarget::kUnknown
                               ? rec.target
                               : fh1::classify_bgf_property(rec.attr_hash, rec.value_type);
          const uint32_t target_h = rec.target_item != 0 ? rec.target_item : st.owner_item;
          apply_prop(target_h, tgt, rec.value, rec.raw_value);
        }
      }
    }
    if (bindings.anim_time >= 0.0f) {
      for (const auto& tr : sc_->bgf.tracks) {
        if (tr.keys.empty()) continue;
        // Group keyframes within a track by (target_item, property target).
        std::unordered_map<uint64_t, std::vector<fh1::UiBgfKeyframe>> grouped;
        for (const auto& kf : tr.keys) {
          auto tgt = kf.target != fh1::UiBgfPropTarget::kUnknown
                         ? kf.target
                         : fh1::classify_bgf_property(kf.attr_hash, kf.raw_tag);
          if (tgt == fh1::UiBgfPropTarget::kUnknown) tgt = tr.target;
          if (tgt == fh1::UiBgfPropTarget::kUnknown) continue;
          const uint32_t ih = kf.target_item != 0 ? kf.target_item : tr.owner_item;
          const uint64_t gkey = (uint64_t(ih) << 8) | uint64_t(tgt);
          grouped[gkey].push_back(kf);
        }
        for (const auto& [gkey, keys] : grouped) {
          fh1::UiBgfTrack sub;
          sub.keys = keys;
          const float val = fh1::evaluate_bgf_track(sub, bindings.anim_time, 0.0f);
          const uint32_t ih = uint32_t(gkey >> 8);
          const auto tgt = static_cast<fh1::UiBgfPropTarget>(gkey & 0xFFu);
          apply_prop(ih, tgt, val, keys.back().raw_value);
        }
      }
      for (const auto& an : sc_->bgf.animations) {
        for (const auto& ch : an.channels) {
          if (ch.keys.empty()) continue;
          const auto tgt = ch.target != fh1::UiBgfPropTarget::kUnknown
                               ? ch.target
                               : fh1::classify_bgf_property(ch.attr_hash, ch.channel_tag);
          if (tgt == fh1::UiBgfPropTarget::kUnknown) continue;
          const float val = fh1::evaluate_bgf_anim_channel(ch, bindings.anim_time, 0.0f);
          apply_prop(an.owner_item, tgt, val, ch.keys.back().raw_value);
        }
      }
    }
  }

  // Parents precede children in BSG; propagate subtree membership, visibility,
  // accumulated alpha, offsets, scales, rotations, and colour overrides in a single forward pass.
  std::vector<uint8_t> in_sub(ncount, 0);
  std::vector<float> eff_alpha(ncount, 0.0f);
  std::vector<float> acc_dx(ncount, bindings.dx);
  std::vector<float> acc_dy(ncount, bindings.dy);
  std::vector<float> acc_sx(ncount, 1.0f);
  std::vector<float> acc_sy(ncount, 1.0f);
  std::vector<float> acc_ang(ncount, 0.0f);
  std::vector<uint32_t> bgf_tint(ncount, 0u);
  // Scale about pivots (NodeOverride::scale): a node's offset canvas point p
  // draws at acc_k * p + acc_b.
  std::vector<float> acc_k(ncount, 1.0f), acc_bx(ncount, 0.0f), acc_by(ncount, 0.0f), acc_t(ncount, 0.0f);
  std::vector<const NodeOverride*> node_ov(ncount, nullptr);

  auto find_override = [&](int idx) -> const NodeOverride* {
    if (auto it = bindings.by_node.find(idx); it != bindings.by_node.end()) return &it->second;
    const std::string_view nm = object_name(idx);
    if (!nm.empty()) {
      if (auto it = bindings.by_name.find(std::string(nm)); it != bindings.by_name.end()) return &it->second;
    }
    return nullptr;
  };

  struct DrawEntry {
    int idx;
    float z;
    int order;
  };
  std::vector<DrawEntry> drawables;
  drawables.reserve(64);

  for (size_t i = 0; i < ncount; ++i) {
    const fh1::UiNode& n = sc_->nodes[i];
    const bool is_root = (root_node < 0) ? (n.parent < 0) : (int(i) == root_node);
    const bool parent_in = (n.parent >= 0 && size_t(n.parent) < i && in_sub[size_t(n.parent)] != 0);
    if (!is_root && !parent_in) continue;

    const NodeOverride* ov = find_override(int(i));
    node_ov[i] = ov;
    if (ov && ov->visible.has_value() && !*ov->visible) continue;

    const auto bit = bgf_eval.find(n.object);
    const BgfEval* bev = (bit != bgf_eval.end()) ? &bit->second : nullptr;

    const float parent_a = parent_in ? eff_alpha[size_t(n.parent)] : bindings.opacity;
    float local_a = n.opacity;
    if (bindings.ignore_zero_alpha && local_a <= 1e-4f) local_a = 1.0f;
    if (bev && bev->alpha.has_value()) local_a = *bev->alpha;
    float a = parent_a * local_a;
    if (ov && ov->alpha.has_value()) a = *ov->alpha * bindings.opacity;
    if (ov && ov->visible.has_value() && *ov->visible && a <= 1e-4f) a = bindings.opacity;

    in_sub[i] = 1;
    eff_alpha[i] = a;
    acc_dx[i] = (parent_in ? acc_dx[size_t(n.parent)] : bindings.dx) + (ov ? ov->dx : 0.0f) +
                (bev && bev->dx.has_value() ? *bev->dx : 0.0f);
    acc_dy[i] = (parent_in ? acc_dy[size_t(n.parent)] : bindings.dy) + (ov ? ov->dy : 0.0f) +
                (bev && bev->dy.has_value() ? *bev->dy : 0.0f);
    acc_sx[i] = (parent_in ? acc_sx[size_t(n.parent)] : 1.0f) *
                (bev && bev->scale_x.has_value() ? *bev->scale_x : 1.0f);
    acc_sy[i] = (parent_in ? acc_sy[size_t(n.parent)] : 1.0f) *
                (bev && bev->scale_y.has_value() ? *bev->scale_y : 1.0f);
    acc_ang[i] = (parent_in ? acc_ang[size_t(n.parent)] : 0.0f) + (ov ? ov->extra_angle : 0.0f) +
                 (bev && bev->rotation.has_value() ? *bev->rotation : 0.0f);
    {
      float k = parent_in ? acc_k[size_t(n.parent)] : 1.0f;
      float bx = parent_in ? acc_bx[size_t(n.parent)] : 0.0f, by = parent_in ? acc_by[size_t(n.parent)] : 0.0f;
      float t = parent_in ? acc_t[size_t(n.parent)] : 0.0f;
      if (ov && (ov->scale != 1.0f || ov->turn != 0.0f)) {
        // About the node's drawn position P: x' = P + s R(turn) (T(x) - P).
        const auto c = at(int(i));
        const auto p = turn_point(c[0] + acc_dx[i], c[1] + acc_dy[i], k, t, bx, by);
        const auto b = turn_point(bx - p[0], by - p[1], ov->scale, ov->turn, p[0], p[1]);
        k *= ov->scale;
        t += ov->turn;
        bx = b[0], by = b[1];
      }
      acc_k[i] = k, acc_bx[i] = bx, acc_by[i] = by, acc_t[i] = t;
    }
    if (bev) {
      if (bev->color_rgba.has_value()) {
        bgf_tint[i] = *bev->color_rgba;
      } else if (bev->color_r.has_value() || bev->color_g.has_value() || bev->color_b.has_value()) {
        const uint8_t cr = uint8_t(std::lround(std::clamp(bev->color_r.value_or(1.0f), 0.0f, 1.0f) * 255.0f));
        const uint8_t cg = uint8_t(std::lround(std::clamp(bev->color_g.value_or(1.0f), 0.0f, 1.0f) * 255.0f));
        const uint8_t cb = uint8_t(std::lround(std::clamp(bev->color_b.value_or(1.0f), 0.0f, 1.0f) * 255.0f));
        bgf_tint[i] = ui::rgba(cr, cg, cb, 255);
      }
    }

    if (a <= 1e-4f || n.object_index < 0) continue;
    const auto kind = sc_->objects[size_t(n.object_index)].kind;
    if (kind == fh1::UiObjectKind::kModel || kind == fh1::UiObjectKind::kText) {
      drawables.push_back({int(i), n.z, int(i)});
    }
  }

  // Sort by BSG Z depth, back to front, preserving tree order for equal Z.
  // The scenes' cameras sit at z -600 looking down +z, so the nearer node has
  // the smaller z (a row's text at -14 over its tapes at -10 and -11) and draws
  // last.
  std::stable_sort(drawables.begin(), drawables.end(), [](const DrawEntry& a, const DrawEntry& b) {
    if (std::fabs(a.z - b.z) > 1e-4f) return a.z > b.z;
    return a.order < b.order;
  });

  int emitted = 0;
  for (const auto& d : drawables) {
    const fh1::UiNode& n = sc_->nodes[size_t(d.idx)];
    const fh1::UiObject& o = sc_->objects[size_t(n.object_index)];
    const NodeOverride* ov = node_ov[size_t(d.idx)];
    const float a = std::clamp(eff_alpha[size_t(d.idx)], 0.0f, 1.0f);
    const float dx = acc_dx[size_t(d.idx)];
    const float dy = acc_dy[size_t(d.idx)];
    const float sx = acc_sx[size_t(d.idx)];
    const float sy = acc_sy[size_t(d.idx)];
    const float ea = acc_ang[size_t(d.idx)];
    const float k = acc_k[size_t(d.idx)], kbx = acc_bx[size_t(d.idx)], kby = acc_by[size_t(d.idx)];
    const float kt = acc_t[size_t(d.idx)];
    if (o.kind == fh1::UiObjectKind::kModel) {
      const float vis_w = (ov && ov->visible_width.has_value()) ? *ov->visible_width : 0.0f;
      const uint32_t tint = (ov && ov->tint.has_value()) ? *ov->tint : bgf_tint[size_t(d.idx)];
      const std::string_view tex_ov = (ov && ov->texture.has_value()) ? std::string_view(*ov->texture) : std::string_view{};
      if (model(l, f, d.idx, a, dx, dy, vis_w, tint, tex_ov, ea, sx, sy, k, kbx, kby, kt)) ++emitted;
    } else if (o.kind == fh1::UiObjectKind::kText) {
      if (!ov || !ov->text.has_value() || ov->text->empty()) continue;
      const uint32_t base_col = ov->tint.has_value() ? *ov->tint
                                : bgf_tint[size_t(d.idx)] ? bgf_tint[size_t(d.idx)]
                                                          : bindings.default_text_color;
      const uint32_t col = ui::fade(base_col, a);
      const auto c = at(d.idx);
      const auto p = turn_point(c[0] + dx, c[1] + dy, k, kt, kbx, kby);
      if (text(l, f, d.idx, *ov->text, col, p[0] - c[0], p[1] - c[1], bindings.fit_text, k, ea + kt) > 0.0f)
        ++emitted;
    }
  }
  return emitted;
}

std::vector<float> SceneDraw::stacker_positions(float origin, float step, int visible_count) const {
  return fh1::reflow_stacker(origin, step, visible_count);
}

void UiTransitionTracker::set_duration(float seconds) {
  duration_ = std::max(0.01f, seconds);
  elapsed_ = std::clamp(elapsed_, 0.0f, duration_);
}

float UiTransitionTracker::step(std::string_view screen_id, float dt) {
  const float safe_dt = std::clamp(dt, 0.0f, 0.1f);
  if (!screen_id.empty()) {
    if (screen_id != active_screen_ || closing_) {
      active_screen_ = std::string(screen_id);
      closing_ = false;
      elapsed_ = 0.0f;
    }
    elapsed_ = std::min(duration_, elapsed_ + safe_dt);
  } else if (!active_screen_.empty()) {
    closing_ = true;
    elapsed_ = std::max(0.0f, elapsed_ - safe_dt);
    if (elapsed_ <= 1e-5f) {
      active_screen_.clear();
      closing_ = false;
    }
  }
  return progress();
}

float UiTransitionTracker::raw_progress() const {
  if (active_screen_.empty()) return 0.0f;
  return duration_ > 1e-5f ? std::clamp(elapsed_ / duration_, 0.0f, 1.0f) : 1.0f;
}

float UiTransitionTracker::progress() const { return ui4::ease_out3(raw_progress()); }

void UiTransitionTracker::reset() {
  active_screen_.clear();
  elapsed_ = duration_;
  closing_ = false;
}

UiTransitionTracker& ui_transition_tracker() {
  static UiTransitionTracker tracker;
  return tracker;
}

float ui_screen_open_progress(std::string_view screen_id, float dt) {
  return ui_transition_tracker().step(screen_id, dt);
}

}  // namespace carview
