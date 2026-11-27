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

std::string SceneDraw::texture_key_of(int node) const {
  if (!sc_ || node < 0 || size_t(node) >= sc_->nodes.size()) return {};
  const int oi = sc_->nodes[size_t(node)].object_index;
  if (oi < 0 || sc_->objects[size_t(oi)].kind != fh1::UiObjectKind::kModel) return {};
  // The material and its textures follow the model.
  const fh1::UiObject* mat = nullptr;
  for (size_t k = size_t(oi) + 1; k < sc_->objects.size(); ++k) {
    const auto& q = sc_->objects[k];
    if (q.kind == fh1::UiObjectKind::kMaterial && !mat) mat = &q;
    else if (q.kind == fh1::UiObjectKind::kTexture && mat && q.owner == mat->id) return texture_key(q.path);
    else break;
  }
  return {};
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
                      float scale_y, float k, float kbx, float kby, float kt, bool wrap,
                      const AnimMap& am, const NodeAnim* uv_anim) const {
  if (!sc_ || node < 0 || size_t(node) >= sc_->nodes.size()) return false;
  if (opacity <= 1e-4f) return false;
  const fh1::UiNode& n = sc_->nodes[size_t(node)];
  if (n.object_index < 0 || sc_->objects[size_t(n.object_index)].kind != fh1::UiObjectKind::kModel) return false;
  // The material and its textures follow the model in the FBF.
  const fh1::UiObject* mat = nullptr;
  const fh1::UiObject* tex[3] = {};
  int ntex = 0;
  for (size_t k = size_t(n.object_index) + 1; k < sc_->objects.size(); ++k) {
    const auto& q = sc_->objects[k];
    if (q.kind == fh1::UiObjectKind::kMaterial && !mat) mat = &q;
    else if (q.kind == fh1::UiObjectKind::kTexture && mat && q.owner == mat->id) {
      if (ntex < 3) tex[ntex] = &q;
      ++ntex;
    } else break;
  }
  auto effective_mat = [&](const fh1::UiObject* t, bool apply_anim) {
    std::array<float, 16> m = t->matrix;
    if (apply_anim && uv_anim) {
      if (uv_anim->tex0_u.has_value()) m[12] += (*uv_anim->tex0_u - t->tex_u_offset);
      if (uv_anim->tex0_v.has_value()) m[13] += (*uv_anim->tex0_v - t->tex_v_offset);
      if (uv_anim->tex0_rot.has_value()) {
        const float da = *uv_anim->tex0_rot - t->tex_rotation;
        if (std::fabs(da) > 1e-4f) {
          const float ca = std::cos(da), sa = std::sin(da);
          const float pu = t->tex_pivot_u, pv = t->tex_pivot_v;
          const float a0 = m[0], b0 = m[4], c0 = m[1], d0 = m[5], tx0 = m[12], ty0 = m[13];
          m[0] = ca * a0 - sa * c0;
          m[1] = sa * a0 + ca * c0;
          m[4] = ca * b0 - sa * d0;
          m[5] = sa * b0 + ca * d0;
          const float rx = tx0 - pu, ry = ty0 - pv;
          m[12] = ca * rx - sa * ry + pu;
          m[13] = sa * rx + ca * ry + pv;
        }
      }
    }
    return m;
  };
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
  if (!am.identity()) {
    const auto p0 = am.apply(r[0], r[1]);
    const auto p1 = am.apply(r[2], r[3]);
    r = {std::min(p0[0], p1[0]), std::min(p0[1], p1[1]), std::max(p0[0], p1[0]), std::max(p0[1], p1[1])};
    if (visible > 0.0f) visible *= std::fabs(am.ax);
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
  const uint32_t add_flag = (mat && mat->additive) ? ui::kUiFlagAdditive : 0u;
  if (!tex_override.empty()) {
    if (ui4::glyph_set() != ui4::GlyphSet::kXbox &&
        ui4::draw_controller_texture(l, f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), tex_override, opacity)) {
      return true;
    }
    std::array<float, 4> uv{};
    if (!ui_texture(std::string(tex_override), uv)) return false;
    l.image(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), uv, c, 0.0f, add_flag);
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
    const auto m = effective_mat(tex[0], true);
    auto U = [&](float u) { return uv[0] + (uv[2] - uv[0]) * u; };
    auto V = [&](float v) { return uv[1] + (uv[3] - uv[1]) * v; };
    const bool wrap_u = (tex[0]->tex_sampler_enums[2] == 0u);
    const bool wrap_v = (tex[0]->tex_sampler_enums[3] == 0u);
    const bool border_u = (tex[0]->tex_sampler_enums[2] == 2u);
    const bool border_v = (tex[0]->tex_sampler_enums[3] == 2u);
    // 2D UV rotation / skew (e.g. 90-degree rotated car images, 357-degree dirt tiles,
    // or BGF rotationuv curves): use the exact 2D affine UV shader path.
    if (std::fabs(m[1]) > 1e-4f || std::fabs(m[4]) > 1e-4f) {
      const std::array<float, 4> uv_mat = {m[0], -m[4], m[1], -m[5]};
      const std::array<float, 2> uv_off = {m[4] + m[12], m[5] + m[13]};
      uint32_t uv_flags = add_flag;
      const float scale_u = std::hypot(m[0], m[1]);
      const float scale_v = std::hypot(m[4], m[5]);
      if (wrap_u || wrap_v || scale_u > 1.05f || scale_v > 1.05f) {
        uv_flags |= ui::kMaskRepeat;
      } else if (border_u || border_v) {
        uv_flags |= ui::kMaskBorderTransparent;
      }
      l.image_uv(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), uv, c, uv_mat, uv_off, uv_flags);
      return true;
    }
    float u0 = m[12], u1 = m[12] + m[0];
    const float u_min = std::min(u0, u1), u_max = std::max(u0, u1);
    const float u_span = u_max - u_min;
    // Whole-integer wrap shift (e.g. mirrored right pill caps m0=-0.5, m12=-1.5
    // -> u=0.5..0.0, or wrapped strips u=1..2 -> 0..1).
    if (const float whole_u = std::floor(u_min + 1e-4f); whole_u != 0.0f && u_max - whole_u <= 1.0f + 1e-4f) {
      u0 -= whole_u;
      u1 -= whole_u;
    }
    // Preserve vertical flip direction (v_anark = 1 - t.y => top t.y=0 -> m[5]+m[13], bottom t.y=1 -> m[13]).
    float v0 = m[5] + m[13], v1 = m[13];
    const float v_min = std::min(v0, v1), v_max = std::max(v0, v1);
    const float v_span = v_max - v_min;
    // A whole-texture offset (947_HUD's REDLINE maps v 1..2): the same texels
    // under wrap addressing.
    if (const float whole_v = std::floor(v_min + 1e-4f); whole_v != 0.0f && v_max - whole_v <= 1.0f + 1e-4f) {
      v0 -= whole_v;
      v1 -= whole_v;
    }
    const bool u_oob = (std::min(u0, u1) < -1e-4f || std::max(u0, u1) > 1.0f + 1e-4f);
    const bool v_oob = (std::min(v0, v1) < -1e-4f || std::max(v0, v1) > 1.0f + 1e-4f);
    // Single-texture repeating patterns (e.g. DIRTTILE01 at m0=6.19 or sub-1.0 wrap
    // patterns with AddressU/V == 0 like 947_HUD Dirt): tile cleanly via kMaskRepeat.
    if (u_span > 1.05f || v_span > 1.05f || (wrap_u && u_oob) || (wrap_v && v_oob)) {
      static const std::array<uint8_t, 64> kWhite = [] {
        std::array<uint8_t, 64> w;
        w.fill(255);
        return w;
      }();
      std::array<float, 4> white{};
      if (ui::atlas_find("ui.white", white) || ui::atlas_image("ui.white", 4, 4, kWhite, white)) {
        const float du = 0.25f * (white[2] - white[0]), dv = 0.25f * (white[3] - white[1]);
        const std::array<float, 4> white_uv = {white[0] + du, white[1] + dv, white[2] - du, white[3] - dv};
        const float su = (std::fabs(m[0]) > 1e-4f) ? m[0] : 1.0f;
        const float sv = (std::fabs(m[5]) > 1e-4f) ? -m[5] : 1.0f;
        const float ou = m[12];
        const float ov = m[5] + m[13];
        l.image_masked(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), uv, white_uv, c, {su, sv, ou, ov},
                       ui::kMaskRepeat | add_flag);
        return true;
      }
    }
    float x0 = r[0], x1 = r[2], a = u0, b = u1;
    if (std::fabs(b - a) > 1e-4f && u_oob) {
      const float t0 = (0.0f - a) / (b - a);
      const float t1 = (1.0f - a) / (b - a);
      const float t_lo = std::clamp(std::min(t0, t1), 0.0f, 1.0f);
      const float t_hi = std::clamp(std::max(t0, t1), 0.0f, 1.0f);
      if (t_hi <= t_lo + 1e-4f) return false;
      const float span_x = x1 - x0;
      const float na = a + (b - a) * t_lo;
      const float nb = a + (b - a) * t_hi;
      x0 = r[0] + span_x * t_lo;
      x1 = r[0] + span_x * t_hi;
      a = std::clamp(na, 0.0f, 1.0f);
      b = std::clamp(nb, 0.0f, 1.0f);
    }
    // Symmetric border-transparent V clip when AddressV == 2 (Border/Clamp).
    float y0 = r[1], y1 = r[3], va = v0, vb = v1;
    if ((border_v || (!wrap_v && tex[0]->tex_address_mode == 4u)) && std::fabs(vb - va) > 1e-4f && v_oob) {
      const float t0 = (0.0f - va) / (vb - va);
      const float t1 = (1.0f - va) / (vb - va);
      const float t_lo = std::clamp(std::min(t0, t1), 0.0f, 1.0f);
      const float t_hi = std::clamp(std::max(t0, t1), 0.0f, 1.0f);
      if (t_hi <= t_lo + 1e-4f) return false;
      const float span_y = y1 - y0;
      const float nva = va + (vb - va) * t_lo;
      const float nvb = va + (vb - va) * t_hi;
      y0 = r[1] + span_y * t_lo;
      y1 = r[1] + span_y * t_hi;
      va = std::clamp(nva, 0.0f, 1.0f);
      vb = std::clamp(nvb, 0.0f, 1.0f);
    }
    (void)border_u;
    l.image(f.x(x0), f.y(y0), f.x(x1), f.y(y1), {U(a), V(va), U(b), V(vb)}, c, 0.0f, add_flag);
    return true;
  }

  // Real two-texture masked / crop multiply mode (SPEC-UI-SCREENS):
  // color = material_rgba * tex0(uv0_transformed) * tex1(uv1).
  // For 3-texture models (e.g. rally points FillBar2, club CarImage), fallback to
  // tex[2] if either primary texture fails to resolve.
  std::array<float, 4> uv0{}, uv1{}, uv2{};
  const fh1::UiObject* t0_obj = tex[0];
  const fh1::UiObject* t1_obj = tex[1];
  bool have0 = ui_texture(texture_key(t0_obj->path), uv0);
  bool have1 = ui_texture(texture_key(t1_obj->path), uv1);
  if (ntex >= 3 && tex[2] && (!have0 || !have1)) {
    if (ui_texture(texture_key(tex[2]->path), uv2)) {
      if (!have0) {
        t0_obj = tex[2];
        uv0 = uv2;
        have0 = true;
      } else if (!have1) {
        t1_obj = tex[2];
        uv1 = uv2;
        have1 = true;
      }
    }
  }
  if (!have0 && !have1) return false;
  if (!have0 || !have1) {
    const auto& uv = have1 ? uv1 : uv0;
    const auto m = effective_mat(have1 ? t1_obj : t0_obj, !have1);
    auto U = [&](float u) { return uv[0] + (uv[2] - uv[0]) * u; };
    auto V = [&](float v) { return uv[1] + (uv[3] - uv[1]) * v; };
    const float u0 = std::clamp(m[12], 0.0f, 1.0f), u1 = std::clamp(m[12] + m[0], 0.0f, 1.0f);
    const float v0 = std::clamp(m[5] + m[13], 0.0f, 1.0f);
    const float v1 = std::clamp(m[13], 0.0f, 1.0f);
    l.image(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), {U(u0), V(v0), U(u1), V(v1)}, c, 0.0f, add_flag);
    return true;
  }

  const float full = r[2] - r[0];
  if (full <= 0.0f) return false;

  const auto m0 = effective_mat(t0_obj, true);
  const auto& m1 = t1_obj->matrix;
  // Sub-rect for tex1 (mask / base strip), preserving horizontal mirror direction
  // (e.g. SMUDGETAPE01 m1=-1,1 and TAPECLIPON02 m1=-4,4) and border-transparent clipping.
  auto U1 = [&](float u) { return uv1[0] + (uv1[2] - uv1[0]) * u; };
  auto V1 = [&](float v) { return uv1[1] + (uv1[3] - uv1[1]) * v; };
  float u1_a = m1[12], u1_b = m1[12] + m1[0];
  const float u1_min = std::min(u1_a, u1_b), u1_max = std::max(u1_a, u1_b);
  if (const float whole = std::floor(u1_min + 1e-4f); whole != 0.0f && u1_max - whole <= 1.0f + 1e-4f) {
    u1_a -= whole;
    u1_b -= whole;
  }
  float draw_x0 = r[0], draw_x1 = r[2];
  float t1_lo = 0.0f, t1_hi = 1.0f;
  // Stretched-cap progress bars (e.g. FillBar TAPEHALF50PX with m1[0]=-21) clamp
  // tex1 instead of border-discarding, so only border-clip tex1 when |m1[0]| <= 5.
  const bool clamped_cap_t1 = (std::fabs(m1[0]) > 5.0f);
  if (!clamped_cap_t1 && std::fabs(u1_b - u1_a) > 1e-4f &&
      (std::min(u1_a, u1_b) < -1e-4f || std::max(u1_a, u1_b) > 1.0f + 1e-4f)) {
    const float s0 = (0.0f - u1_a) / (u1_b - u1_a);
    const float s1 = (1.0f - u1_a) / (u1_b - u1_a);
    t1_lo = std::clamp(std::min(s0, s1), 0.0f, 1.0f);
    t1_hi = std::clamp(std::max(s0, s1), 0.0f, 1.0f);
    if (t1_hi <= t1_lo + 1e-4f) return false;
    draw_x0 = r[0] + full * t1_lo;
    draw_x1 = r[0] + full * t1_hi;
  }
  float b_u0 = clamped_cap_t1 ? 0.99f : std::clamp(u1_a + (u1_b - u1_a) * t1_lo, 0.0f, 1.0f);
  float b_u1 = clamped_cap_t1 ? 0.99f : std::clamp(u1_a + (u1_b - u1_a) * t1_hi, 0.0f, 1.0f);
  const float b_v0 = std::clamp(m1[5] + m1[13], 0.0f, 1.0f);
  const float b_v1 = std::clamp(m1[13], 0.0f, 1.0f);

  float su0 = (std::fabs(m0[0]) > 1e-4f) ? m0[0] : 1.0f;
  const float sv0 = (std::fabs(m0[5]) > 1e-4f) ? -m0[5] : 1.0f;
  const bool wrap_u0 = (t0_obj->tex_sampler_enums[2] == 0u);
  const bool wrap_v0 = (t0_obj->tex_sampler_enums[3] == 0u);
  const bool border_u0 = (t0_obj->tex_sampler_enums[2] == 2u);
  const bool border_v0 = (t0_obj->tex_sampler_enums[3] == 2u);

  // Exact Anark vertical transform: v_tex = m0[5]*(1 - t.y) + m0[13] = (-m0[5])*t.y + (m0[5] + m0[13]).
  float ou0 = m0[12];
  const float ov0 = m0[5] + m0[13];
  // Stretched-cap progress bars (e.g. TAPEHALF50PX m0[0]=21 with AddressU=2) clamp
  // horizontally rather than repeating or border-discarding.
  const bool clamped_cap_t0 = (std::fabs(su0) > 5.0f && border_u0 && !wrap_u0);
  const bool tiled = !clamped_cap_t0 &&
                     ((std::fabs(su0) > 1.05f || std::fabs(sv0) > 1.05f) ||
                      (wrap_u0 && wrap_v0 && !border_u0 && !border_v0 && visible <= 0.0f &&
                       (std::fabs(ou0) > 1e-4f || std::fabs(ov0) > 1e-4f)));

  if (visible > 0.0f && !tiled && !clamped_cap_t0) {
    // Dynamic crop width: shifts tex0 so its right feathered edge lands at `visible`.
    const float vis = std::clamp(visible, 0.0f, full);
    ou0 = 1.0f - vis / full;
  } else if (visible > 0.0f && (tiled || clamped_cap_t0)) {
    // Clip tiled or stretched-cap fill bars (e.g. H_POST_RACE_CAREER_POINTS FillBar/FillBar2)
    // to `visible` px from left without distorting the texture.
    const float vis_hi = std::clamp(visible / full, 0.0f, 1.0f);
    if (vis_hi <= t1_lo + 1e-4f) return false;
    if (vis_hi < t1_hi) {
      t1_hi = vis_hi;
      draw_x1 = r[0] + full * t1_hi;
      if (!clamped_cap_t1) b_u1 = std::clamp(u1_a + (u1_b - u1_a) * t1_hi, 0.0f, 1.0f);
    }
  }
  const std::array<float, 4> mask_uv = {U1(b_u0), V1(b_v0), U1(b_u1), V1(b_v1)};
  // Remap tex0's linear horizontal UV transform when tex1 or tiled/clamped visible_width clipped the quad.
  if (t1_lo > 0.0f || t1_hi < 1.0f) {
    ou0 = ou0 + su0 * t1_lo;
    su0 = su0 * (t1_hi - t1_lo);
  }
  uint32_t mask_flags = add_flag;
  if (tiled || wrap) {
    mask_flags |= ui::kMaskRepeat;
  } else if (!clamped_cap_t0 &&
             (border_u0 || border_v0 || visible > 0.0f || std::fabs(ou0) > 1e-4f || std::fabs(ov0) > 1e-4f)) {
    mask_flags |= ui::kMaskBorderTransparent;
  }
  l.image_masked(f.x(draw_x0), f.y(r[1]), f.x(draw_x1), f.y(r[3]), uv0, mask_uv, c, {su0, sv0, ou0, ov0}, mask_flags);
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
  const int oi = sc_->nodes[size_t(node)].object_index;
  float y_off = -0.5f * ts.size;
  if (oi >= 0 && size_t(oi) < sc_->objects.size()) {
    const fh1::UiObject& o = sc_->objects[size_t(oi)];
    if (o.kind == fh1::UiObjectKind::kText) {
      if (o.text_valign == 0u) y_off = 0.0f;
      else if (o.text_valign == 2u) y_off = -ts.size;
      if (o.text_has_shadow && o.text_shadow_colour[3] > 1e-3f) {
        const float main_a = float((colour >> 24) & 0xFFu) / 255.0f;
        const uint32_t sh_col = colour_of(o.text_shadow_colour, main_a);
        if ((sh_col >> 24) > 0u) {
          ui::TextStyle sh_ts = ts;
          sh_ts.color = sh_col;
          const float sh_dx = o.text_shadow_offset[0] * ts.size * 0.25f;
          const float sh_dy = o.text_shadow_offset[1] * ts.size * 0.25f;
          l.text(f.x(cx + dx) + sh_dx, f.y(cy + dy) + y_off + sh_dy, s, sh_ts);
        }
      }
    }
  }
  const float w = l.text(f.x(cx + dx), f.y(cy + dy) + y_off, s, ts);
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
  // The timeline's map of the rest canvas (SceneBindings::anim): x' = a x + b.
  std::vector<AnimMap> acc_am(ncount);
  // Scale about pivots (NodeOverride::scale): a node's offset canvas point p
  // draws at acc_k * p + acc_b.
  std::vector<float> acc_k(ncount, 1.0f), acc_bx(ncount, 0.0f), acc_by(ncount, 0.0f), acc_t(ncount, 0.0f);
  std::vector<const NodeOverride*> node_ov(ncount, nullptr);
  std::vector<const NodeAnim*> node_an(ncount, nullptr);

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

    const auto ait = bindings.anim.find(int(i));
    const NodeAnim* an = ait != bindings.anim.end() ? &ait->second : nullptr;
    node_an[i] = an;
    if (an && an->inactive && !(ov && ov->visible.has_value() && *ov->visible)) continue;

    const float parent_a = parent_in ? eff_alpha[size_t(n.parent)] : bindings.opacity;
    float local_a = n.opacity;
    if (bindings.ignore_zero_alpha && local_a <= 1e-4f) local_a = 1.0f;
    if (an && an->opacity.has_value()) local_a = *an->opacity;
    float a = parent_a * local_a;
    if (ov && ov->alpha.has_value()) a = *ov->alpha * bindings.opacity;
    if (ov && ov->visible.has_value() && *ov->visible && a <= 1e-4f) a = bindings.opacity;

    in_sub[i] = 1;
    eff_alpha[i] = a;
    acc_dx[i] = (parent_in ? acc_dx[size_t(n.parent)] : bindings.dx) + (ov ? ov->dx : 0.0f);
    acc_dy[i] = (parent_in ? acc_dy[size_t(n.parent)] : bindings.dy) + (ov ? ov->dy : 0.0f);
    acc_sx[i] = (parent_in ? acc_sx[size_t(n.parent)] : 1.0f) * (ov ? ov->stretch_x : 1.0f);
    acc_sy[i] = (parent_in ? acc_sy[size_t(n.parent)] : 1.0f) * (ov ? ov->stretch_y : 1.0f);
    acc_ang[i] = (parent_in ? acc_ang[size_t(n.parent)] : 0.0f) + (ov ? ov->extra_angle : 0.0f);
    {
      // The timeline: the node moves by (dx, dy) under its parent's map and
      // scales about its own (moved) position, its subtree with it.
      AnimMap m = parent_in ? acc_am[size_t(n.parent)] : AnimMap{};
      if (an && (an->dx != 0.0f || an->dy != 0.0f || an->sx != 1.0f || an->sy != 1.0f)) {
        const auto c = at(int(i));
        const auto p = m.apply(c[0], c[1]);
        const float px = p[0] + m.ax * an->dx, py = p[1] + m.ay * an->dy;
        AnimMap q;
        q.ax = m.ax * an->sx;
        q.ay = m.ay * an->sy;
        q.bx = px - q.ax * c[0];
        q.by = py - q.ay * c[1];
        m = q;
      }
      acc_am[i] = m;
    }
    {
      float k = parent_in ? acc_k[size_t(n.parent)] : 1.0f;
      float bx = parent_in ? acc_bx[size_t(n.parent)] : 0.0f, by = parent_in ? acc_by[size_t(n.parent)] : 0.0f;
      float t = parent_in ? acc_t[size_t(n.parent)] : 0.0f;
      const float o_scale = ov ? ov->scale : 1.0f;
      const float o_turn = (ov ? ov->turn : 0.0f) + (an ? an->turn : 0.0f);
      if (o_scale != 1.0f || o_turn != 0.0f) {
        // About the node's drawn position P: x' = P + s R(turn) (T(x) - P).
        const auto c = acc_am[i].apply(at(int(i))[0], at(int(i))[1]);
        const auto p = turn_point(c[0] + acc_dx[i], c[1] + acc_dy[i], k, t, bx, by);
        const auto b = turn_point(bx - p[0], by - p[1], o_scale, o_turn, p[0], p[1]);
        k *= o_scale;
        t += o_turn;
        bx = b[0], by = b[1];
      }
      acc_k[i] = k, acc_bx[i] = bx, acc_by[i] = by, acc_t[i] = t;
    }
    if (an && an->colour.has_value()) {
      const auto& c = *an->colour;
      auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
      bgf_tint[i] = ui::rgba(byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3]));
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
      const bool wrap = ov && ov->wrap;
      if (model(l, f, d.idx, a, dx, dy, vis_w, tint, tex_ov, ea, sx, sy, k, kbx, kby, kt, wrap,
                acc_am[size_t(d.idx)], node_an[size_t(d.idx)]))
        ++emitted;
    } else if (o.kind == fh1::UiObjectKind::kText) {
      if (!ov || !ov->text.has_value() || ov->text->empty()) continue;
      const uint32_t base_col = ov->tint.has_value()    ? *ov->tint
                                : bgf_tint[size_t(d.idx)] ? bgf_tint[size_t(d.idx)]
                                : o.has_text_colour     ? colour_of(o.text_colour, 1.0f)
                                                        : bindings.default_text_color;
      const uint32_t col = ui::fade(base_col, a);
      const auto c = at(d.idx);
      const AnimMap& am = acc_am[size_t(d.idx)];
      const auto ca = am.apply(c[0], c[1]);
      const auto p = turn_point(ca[0] + dx, ca[1] + dy, k, kt, kbx, kby);
      if (text(l, f, d.idx, *ov->text, col, p[0] - c[0], p[1] - c[1], bindings.fit_text, k * std::fabs(am.ay),
               ea + kt) > 0.0f)
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
