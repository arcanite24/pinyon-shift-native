#include "hud_scene.h"

#include <algorithm>
#include <cmath>

#include "game_hud.h"

namespace carview {
namespace {

std::function<const fh1::UiScene*(const std::string&)>& scenes() {
  static std::function<const fh1::UiScene*(const std::string&)> f;
  return f;
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

void set_ui_scenes(std::function<const fh1::UiScene*(const std::string&)> load) { scenes() = std::move(load); }

const fh1::UiScene* ui_scene(const std::string& name) { return scenes() ? scenes()(name) : nullptr; }

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

float SceneDraw::alpha(int node) const { return sc_ && node >= 0 ? sc_->nodes[size_t(node)].alpha : 0; }

bool SceneDraw::model(ui::List& l, const SceneFrame& f, int node, float opacity, float dx, float dy, float visible,
                      uint32_t tint) const {
  if (!sc_ || node < 0) return false;
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
  auto r = rect(node);
  r[0] += dx, r[2] += dx, r[1] += dy, r[3] += dy;
  const uint32_t c = tint ? ui::fade(tint, opacity) : colour_of(mat ? mat->colour : std::array<float, 4>{1, 1, 1, 1}, opacity);
  if (ntex == 0) {
    l.rect(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), c);
    return true;
  }
  std::array<float, 4> uv;
  const fh1::UiObject* main = ntex >= 2 ? tex[1] : tex[0];
  if (!ui_texture(texture_key(main->path), uv)) return false;
  const auto& m = main->matrix;
  // The texture rect: u from m12 over m0, v (flipped) from m13 + m5 to m13.
  auto U = [&](float u) { return uv[0] + (uv[2] - uv[0]) * u; };
  auto V = [&](float v) { return uv[1] + (uv[3] - uv[1]) * v; };
  const float u0 = m[12], u1 = m[12] + m[0], v0 = std::min(m[13], m[13] + m[5]), v1 = std::max(m[13], m[13] + m[5]);
  if (ntex < 2) {
    l.image(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), {U(u0), V(v0), U(u1), V(v1)}, c);
    return true;
  }
  // A crop: the strip's left part to the visible width, then the strip's
  // own right end (as the offset copy masks it).
  const float full = r[2] - r[0];
  if (full <= 0) return false;
  const float off = std::clamp(tex[0]->matrix[12], 0.0f, 1.0f);
  const float vis = std::clamp(visible > 0 ? visible : full * (1 - off), 0.0f, full);
  const float cap = std::min(vis, 0.012f * full * (r[3] - r[1]) / (r[3] - r[1]));  // 12 texels of 1024 (ours)
  const float x1 = r[0] + vis;
  l.image(f.x(r[0]), f.y(r[1]), f.x(x1 - cap), f.y(r[3]), {U(0), V(v0), U((vis - cap) / full), V(v1)}, c);
  l.image(f.x(x1 - cap), f.y(r[1]), f.x(x1), f.y(r[3]), {U(1 - cap / full), V(v0), U(1), V(v1)}, c);
  return true;
}

ui::TextStyle SceneDraw::style(int node, const SceneFrame& f, uint32_t colour) const {
  ui::TextStyle ts{.size = 12 * f.s, .color = colour, .font = ui::Font::kB, .keep_case = true};
  if (!sc_ || node < 0) return ts;
  const fh1::UiNode& n = sc_->nodes[size_t(node)];
  if (n.object_index < 0) return ts;
  const fh1::UiObject& o = sc_->objects[size_t(n.object_index)];
  if (o.kind != fh1::UiObjectKind::kText) return ts;
  const std::string& fn = o.font;
  const char last = fn.empty() ? 'b' : fn.back();
  ts.font = last == 'a' ? ui::Font::kA : last == 'd' ? ui::Font::kD : last == 'e' ? ui::Font::kE : ui::Font::kB;
  ts.size = float(o.text_size) * n.sy * f.s;
  ts.align = o.text_align == 1 ? ui::Align::kCenter : o.text_align == 2 ? ui::Align::kRight : ui::Align::kLeft;
  return ts;
}

float SceneDraw::text_width(int node) const {
  if (!sc_ || node < 0) return 0;
  const fh1::UiNode& n = sc_->nodes[size_t(node)];
  if (n.object_index < 0) return 0;
  const fh1::UiObject& o = sc_->objects[size_t(n.object_index)];
  return o.kind == fh1::UiObjectKind::kText ? o.text_width * n.sx : 0;
}

float SceneDraw::text(ui::List& l, const SceneFrame& f, int node, std::string_view s, uint32_t colour, float dx,
                      float dy, bool fit) const {
  if (!sc_ || node < 0 || s.empty()) return 0;
  ui::TextStyle ts = style(node, f, colour);
  const float box = text_width(node) * f.s;
  if (fit && box > 0)
    for (float w = ui::List::measure(s, ts); w > box && ts.size > 6 * f.s; w = ui::List::measure(s, ts))
      ts.size *= std::max(0.8f, box / w);
  const auto [cx, cy] = at(node);
  const float w = l.text(f.x(cx + dx), f.y(cy + dy) - 0.5f * ts.size, s, ts);
  return w / f.s;
}

}  // namespace carview
