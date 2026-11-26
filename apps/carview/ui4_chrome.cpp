#include "ui4_chrome.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace carview::ui4 {
namespace {

std::function<bool(const std::string&, std::array<float, 4>&)>& tex_fn() {
  static std::function<bool(const std::string&, std::array<float, 4>&)> f;
  return f;
}

std::function<const fh1::UiScene*(const std::string&)>& scene_fn() {
  static std::function<const fh1::UiScene*(const std::string&)> f;
  return f;
}

GlyphSet& active_glyph_set() {
  static GlyphSet g = GlyphSet::kXbox;
  return g;
}

struct ControlsDiagramSync {
  int layout_id = 0;
  bool swap_hb_clutch = false;
};

ControlsDiagramSync& diagram_sync() {
  static ControlsDiagramSync s;
  return s;
}

std::string upper_copy(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string keyboard_keycap_label(const std::string& u) {
  if (u == "A" || u == "ACCEPT" || u == "CONFIRM" || u == "SELECT") return "ENTER";
  if (u == "B" || u == "CANCEL") return "ESC";
  if (u == "X") return "X";
  if (u == "ACTIVATE") return "E";
  if (u == "Y") return "Y";
  if (u == "REWIND") return "R";
  if (u == "LB") return "Q";
  if (u == "RB") return "E";
  if (u == "LT") return "-";
  if (u == "RT") return "+";
  if (u == "START" || u == "MENU" || u == "PAUSE") return "ESC";
  if (u == "BACK" || u == "MAP") return "M";
  if (u == "CAMERA") return "V";
  if (u == "HORN") return "H";
  if (u == "HANDBRAKE") return "SPACE";
  if (u == "CLUTCH") return "SHIFT";
  if (u == "DPAD" || u == "D-PAD") return "ARROWS";
  if (u == "DPAD_UD") return "UP/DN";
  if (u == "DPAD_LR") return "L/R";
  if (u == "LS") return "WASD";
  if (u == "RS") return "MOUSE";
  return u;
}

bool draw_keyboard_glyph(ui::List& l, float cx, float cy, float r, const std::string& b, float alpha) {
  const std::string u = upper_copy(b);
  const std::string lbl = keyboard_keycap_label(u);
  if (lbl.empty()) return false;
  const float char_w = r * (lbl.size() > 3 ? 0.50f : 0.62f);
  const float hw = std::max(r * 1.08f, float(lbl.size()) * char_w * 0.56f + r * 0.45f);
  const float hh = r * 0.88f;
  const float rad = l.turned() ? 0.0f : std::max(2.0f, r * 0.24f);
  // Subtle 3D keycap base bevel + crisp dark mechanical keycap top face.
  if (l.turned()) {
    // Inside a tilted scope, zero-radius flat rects rotate cleanly in lockstep with text.
    const float bw = std::max(1.1f, r * 0.12f);
    l.rect(cx - hw - bw, cy - hh - bw, cx + hw + bw, cy + hh + bw, ui::fade(ui::rgba(226, 230, 238, 225), alpha));
    l.rect(cx - hw, cy - hh, cx + hw, cy + hh, ui::fade(ui::rgba(36, 40, 48, 246), alpha));
  } else {
    l.rect(cx - hw, cy - hh + r * 0.14f, cx + hw, cy + hh + r * 0.18f, ui::fade(ui::rgba(10, 12, 16, 245), alpha), rad);
    l.rect(cx - hw, cy - hh, cx + hw, cy + hh, ui::fade(ui::rgba(36, 40, 48, 246), alpha), rad);
  }
  // Semantic bottom highlight bar (green Accept, red Cancel, pink Menu/Map, amber Rewind).
  uint32_t accent = ui::rgba(216, 222, 234, 210);
  if (u == "A" || u == "ACCEPT" || u == "CONFIRM" || u == "SELECT") accent = kGreen;
  else if (u == "B" || u == "CANCEL" || u == "START" || u == "MENU") accent = kRed;
  else if (u == "BACK" || u == "MAP" || u == "HORN") accent = kPink;
  else if (u == "Y" || u == "REWIND") accent = kAmber;
  else if (u == "X" || u == "ACTIVATE" || u == "CAMERA") accent = kBlue;
  l.rect(cx - hw + 2.0f, cy + hh - std::max(1.8f, r * 0.18f), cx + hw - 2.0f, cy + hh, ui::fade(accent, alpha));
  if (!l.turned()) {
    l.frame(cx - hw, cy - hh, cx + hw, cy + hh, ui::fade(ui::rgba(226, 230, 238, 225), alpha),
            std::max(1.1f, r * 0.12f), rad);
  }
  const float fsz = r * (lbl.size() >= 5 ? 0.72f : lbl.size() >= 3 ? 0.82f : 1.00f);
  ui::TextStyle ts = style(ui::Font::kB, fsz, ui::fade(kWhite, alpha), ui::Align::kCenter);
  fit_text(lbl, ts, (2.0f * hw) - 4.0f, r * 0.50f);
  l.text(cx, cy - 0.52f * ts.size, lbl, ts);
  return true;
}

bool draw_playstation_glyph(ui::List& l, float cx, float cy, float r, const std::string& b, float alpha) {
  const std::string u = upper_copy(b);
  const float stroke = std::max(1.5f, r * 0.20f);
  const uint32_t disc = ui::fade(ui::rgba(28, 30, 36, 245), alpha);
  const uint32_t ring = ui::fade(ui::rgba(210, 214, 224, 210), alpha);
  if (u == "A" || u == "CROSS" || u == "ACCEPT") {  // Cross (blue-tinted X)
    l.circle(cx, cy, r, disc);
    const uint32_t col = ui::fade(ui::rgba(116, 178, 255, 255), alpha);
    const float d = r * 0.44f;
    l.line(cx - d, cy - d, cx + d, cy + d, stroke, col);
    l.line(cx + d, cy - d, cx - d, cy + d, stroke, col);
    return true;
  }
  if (u == "B" || u == "CIRCLE" || u == "CANCEL") {  // Circle (coral-red O)
    l.circle(cx, cy, r, disc);
    const uint32_t col = ui::fade(ui::rgba(255, 108, 108, 255), alpha);
    l.arc(cx, cy, r * 0.48f, stroke, 0.0f, 6.2831853f, col, false);
    return true;
  }
  if (u == "X" || u == "SQUARE" || u == "ACTIVATE") {  // Square (pink-violet box)
    l.circle(cx, cy, r, disc);
    const uint32_t col = ui::fade(ui::rgba(232, 134, 214, 255), alpha);
    const float d = r * 0.44f;
    l.frame(cx - d, cy - d, cx + d, cy + d, col, stroke, 0.0f);
    return true;
  }
  if (u == "Y" || u == "TRIANGLE" || u == "REWIND") {  // Triangle (mint-green triangle)
    l.circle(cx, cy, r, disc);
    const uint32_t col = ui::fade(ui::rgba(82, 226, 174, 255), alpha);
    const float top_y = cy - r * 0.50f;
    const float bot_y = cy + r * 0.42f;
    const float half_w = r * 0.50f;
    l.line(cx, top_y, cx + half_w, bot_y, stroke, col);
    l.line(cx + half_w, bot_y, cx - half_w, bot_y, stroke, col);
    l.line(cx - half_w, bot_y, cx, top_y, stroke, col);
    return true;
  }
  if (u == "DPAD" || u == "D-PAD" || u == "DPAD_UD" || u == "DPAD_LR") {
    l.circle(cx, cy, r, disc);
    const float arm = r * 0.58f;
    const float thick = std::max(2.0f, r * 0.28f);
    const uint32_t hi = ui::fade(kWhite, alpha);
    const uint32_t lo = ui::fade(ui::rgba(130, 136, 148, 220), alpha);
    l.line(cx, cy - arm, cx, cy + arm, thick, u == "DPAD_LR" ? lo : hi);
    l.line(cx - arm, cy, cx + arm, cy, thick, u == "DPAD_UD" ? lo : hi);
    return true;
  }
  const char* ps_label = nullptr;
  if (u == "LB" || u == "L1") ps_label = "L1";
  else if (u == "RB" || u == "R1") ps_label = "R1";
  else if (u == "LT" || u == "L2") ps_label = "L2";
  else if (u == "RT" || u == "R2") ps_label = "R2";
  else if (u == "LS" || u == "L3") ps_label = "L3";
  else if (u == "RS" || u == "R3") ps_label = "R3";
  else if (u == "START" || u == "MENU" || u == "OPT") ps_label = "OPT";
  else if (u == "BACK" || u == "MAP" || u == "SHARE") ps_label = "SHARE";
  if (ps_label) {
    const std::string lbl(ps_label);
    const float hw = std::max(r * 1.35f, float(lbl.size()) * r * 0.42f + 4.0f);
    l.rect(cx - hw, cy - r * 0.85f, cx + hw, cy + r * 0.85f, disc, r * 0.35f);
    l.frame(cx - hw, cy - r * 0.85f, cx + hw, cy + r * 0.85f, ring, std::max(1.0f, r * 0.12f), r * 0.35f);
    ui::TextStyle ts = style(ui::Font::kB, r * 0.90f, ui::fade(kWhite, alpha), ui::Align::kCenter);
    l.text(cx, cy - 0.5f * ts.size, lbl, ts);
    return true;
  }
  return false;
}

std::string lower_copy(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string alnum_lower(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (char c : sv) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string thousands_str(int64_t v) {
  std::string d = std::to_string(v < 0 ? -v : v);
  std::string out;
  out.reserve(d.size() + d.size() / 3 + 1);
  for (size_t i = 0; i < d.size(); ++i) {
    if (i && (d.size() - i) % 3 == 0) out += ',';
    out += d[i];
  }
  return v < 0 ? "-" + out : out;
}

const char* ordinal_suffix(int p) {
  if (p % 100 >= 11 && p % 100 <= 13) return "TH";
  switch (p % 10) {
    case 1: return "ST";
    case 2: return "ND";
    case 3: return "RD";
    default: return "TH";
  }
}

bool ensure_tape_masks(std::array<float, 4>& smudge, std::array<float, 4>& crisp, bool big) {
  const bool have_sm = ui::atlas_find("tape.smudge", smudge) || texture("DIRTMASKS/SMUDGETAPE01", smudge);
  bool have_cr = false;
  if (big) {
    have_cr = texture("DIRTMASKS/CRISPTAPEBIG01", crisp) || ui::atlas_find("tape.crisp", crisp) ||
              texture("DIRTMASKS/CRISPTAPE01", crisp);
  } else {
    have_cr = ui::atlas_find("tape.crisp", crisp) || texture("DIRTMASKS/CRISPTAPE01", crisp);
  }
  return have_sm && have_cr;
}

std::string controller_texture_name(std::string_view btn) {
  const std::string u = upper_copy(btn);
  if (u == "A") return "CONTROLLER/A_BUTTON";
  if (u == "B") return "CONTROLLER/B_BUTTON";
  if (u == "X") return "CONTROLLER/X_BUTTON";
  if (u == "Y") return "CONTROLLER/Y_BUTTON";
  if (u == "LB" || u == "Q") return "CONTROLLER/LBUMPER";
  if (u == "RB" || u == "E") return "CONTROLLER/RBUMPER";
  if (u == "LT") return "CONTROLLER/LTRIGGER";
  if (u == "RT") return "CONTROLLER/RTRIGGER";
  if (u == "START") return "CONTROLLER/START_BUTTON";
  if (u == "BACK") return "CONTROLLER/BACK_BUTTON";
  if (u == "DPAD" || u == "D-PAD") return "CONTROLLER/D_PAD";
  if (u == "DPAD_UD") return "CONTROLLER/D_PAD_UPDOWN";
  if (u == "DPAD_LR") return "CONTROLLER/D_PAD_LEFT_RIGHT";
  if (u == "LS") return "CONTROLLER/L_THUMBSTICK";
  if (u == "RS") return "CONTROLLER/RTHUMBSTICK";
  return {};
}

uint32_t fallback_button_colour(std::string_view btn) {
  const std::string u = upper_copy(btn);
  if (u == "A") return kGreen;
  if (u == "B") return kRed;
  if (u == "X") return kBlue;
  if (u == "Y") return kAmber;
  if (u == "LB" || u == "RB" || u == "LT" || u == "RT") return ui::rgba(220, 224, 232);
  return kPink;
}

}  // namespace

void set_texture_loader(std::function<bool(const std::string&, std::array<float, 4>&)> fn) {
  tex_fn() = std::move(fn);
}

void set_scene_loader(std::function<const fh1::UiScene*(const std::string&)> fn) {
  scene_fn() = std::move(fn);
}

bool texture(const std::string& path, std::array<float, 4>& uv) {
  return tex_fn() && tex_fn()(path, uv);
}

const fh1::UiScene* scene(const std::string& name) {
  return scene_fn() ? scene_fn()(name) : nullptr;
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float ease_out3(float t) {
  const float k = clamp01(t);
  const float inv = 1.0f - k;
  return 1.0f - inv * inv * inv;
}

ui::TextStyle style(ui::Font f, float size, uint32_t color, ui::Align align, bool keep_case) {
  return ui::TextStyle{.size = size, .weight = 0.11f, .color = color, .align = align, .font = f, .keep_case = keep_case};
}

void fit_text(std::string_view text, ui::TextStyle& ts, float max_width, float min_size) {
  if (max_width <= 4.0f) return;
  while (ts.size > min_size && ui::List::measure(text, ts) > max_width) {
    ts.size *= 0.94f;
  }
}

float shadowed_text(ui::List& l, float x, float y, std::string_view text, const ui::TextStyle& ts, float s) {
  ui::TextStyle sh = ts;
  sh.color = ui::fade(kSoftShadow, float((ts.color >> 24) & 0xFF) / 255.0f);
  l.text(x + 1.5f * s, y + 1.5f * s, text, sh);
  return l.text(x, y, text, ts);
}

uint32_t class_colour_by_name(std::string_view cls) {
  const std::string u = upper_copy(cls);
  if (u == "F") return ui::rgba(0x78, 0x7E, 0x88);
  if (u == "E") return ui::rgba(0x46, 0xB8, 0x4E);
  if (u == "D") return ui::rgba(0x2A, 0xB0, 0xE8);
  if (u == "C") return ui::rgba(0xF5, 0xC8, 0x18);
  if (u == "B") return ui::rgba(0xFF, 0x92, 0x03);
  if (u == "A") return ui::rgba(0xE8, 0x24, 0x28);
  if (u == "S") return ui::rgba(0x9E, 0x36, 0xE0);
  if (u == "R3") return ui::rgba(0xE6, 0x1E, 0x78);
  if (u == "R2") return ui::rgba(0x28, 0x72, 0xE6);
  if (u == "R1") return ui::rgba(0xD8, 0x46, 0x20);
  if (u == "X") return ui::rgba(0x24, 0xD2, 0xA8);
  return ui::rgba(0x78, 0x7E, 0x88);
}

uint32_t class_colour_by_id(int class_id) {
  switch (class_id) {
    case 0: return class_colour_by_name("F");
    case 1: return class_colour_by_name("E");
    case 2: return class_colour_by_name("D");
    case 3: return class_colour_by_name("C");
    case 4: return class_colour_by_name("B");
    case 5: return class_colour_by_name("A");
    case 6: return class_colour_by_name("S");
    case 7: return class_colour_by_name("R3");
    case 8: return class_colour_by_name("R2");
    case 9: return class_colour_by_name("R1");
    default: return class_colour_by_name("X");
  }
}

uint32_t wristband_colour(std::string_view name) {
  const std::string n = lower_copy(name);
  if (n.find("yellow") != std::string::npos) return ui::rgba(0xFF, 0xD6, 0x28);
  if (n.find("green") != std::string::npos) return ui::rgba(0x4B, 0xDC, 0x6E);
  if (n.find("orange") != std::string::npos) return ui::rgba(0xFF, 0x92, 0x24);
  if (n.find("blue") != std::string::npos) return ui::rgba(0x0A, 0x9B, 0xFA);
  if (n.find("purple") != std::string::npos) return ui::rgba(0xC6, 0x78, 0xFF);
  if (n.find("pink") != std::string::npos) return kPink;
  if (n.find("red") != std::string::npos) return kRed;
  if (n.find("gold") != std::string::npos) return ui::rgba(0xF6, 0xCE, 0x4E);
  if (n.find("platinum") != std::string::npos) return ui::rgba(0xDC, 0xE6, 0xF4);
  return kPink;
}

std::string format_credits(int64_t cr) { return thousands_str(cr) + " Cr"; }

void brush(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour, bool big, float wipe_frac) {
  const float f = clamp01(wipe_frac);
  if (f <= 0.005f || x1 <= x0 || y1 <= y0) return;
  const float wx1 = x0 + (x1 - x0) * f;
  std::array<float, 4> smudge{}, crisp{};
  if (ensure_tape_masks(smudge, crisp, big)) {
    const float h = y1 - y0;
    const float px = 0.055f * h;
    const float py = 0.13f * h;
    const float bx0 = x0 - px, by0 = y0 - py, bx1 = x1 + px, by1 = y1 + py;
    const float crop_off = 1.0f - f;
    // Authentic FH1 two-texture masked brush crop:
    // Smudge_Tape (soft outer spray) + Crisp_Tape (feathered right edge via kImageMasked) + optional Dirt_Tile.
    const uint32_t sm_col = ui::fade(colour, 0.88f);
    l.image_masked(bx0, by0, bx1, by1, smudge, smudge, sm_col, {1.0f, 1.0f, crop_off, 0.0f},
                   ui::kMaskBorderTransparent);
    l.image_masked(bx0, by0, bx1, by1, crisp, crisp, colour, {1.0f, 1.0f, crop_off, 0.0f},
                   ui::kMaskBorderTransparent);
    std::array<float, 4> dirt{};
    if (texture("DIRTMASKS/DIRTTILE01", dirt)) {
      const float tile_u = std::max(1.0f, (bx1 - bx0) / std::max(1.0f, (by1 - by0) * 3.5f));
      l.image_masked(bx0, by0, wx1 + px, by1, dirt, crisp, ui::fade(ui::rgba(255, 255, 255, 36), float((colour >> 24) & 0xFF) / 255.0f),
                     {tile_u, 1.0f, 0.0f, 0.0f}, ui::kMaskRepeat);
    }
  } else {
    // Clean sharp-edged fallback (never rounded glass).
    l.rect(x0, y0, wx1, y1, colour);
  }
}

void pill(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour, PillKind kind, bool with_pin,
          bool with_clipon) {
  if (x1 <= x0 || y1 <= y0) return;
  const char* tex_name = "DIRTMASKS/PILL01";
  float texels = 512.0f;
  switch (kind) {
    case PillKind::kPill02: tex_name = "DIRTMASKS/PILL02"; texels = 512.0f; break;
    case PillKind::kPill03: tex_name = "DIRTMASKS/PILL03"; texels = 512.0f; break;
    case PillKind::kPillBig: tex_name = "DIRTMASKS/PILL_BIG"; texels = 1024.0f; break;
    case PillKind::kPill26: tex_name = "KITPARTS/PILL26PX"; texels = 512.0f; break;
    case PillKind::kHud01: tex_name = "DIRTMASKS/HUD/HUDPILL01"; texels = 1024.0f; break;
    case PillKind::kHud02: tex_name = "DIRTMASKS/HUD/HUDPILL02"; texels = 1024.0f; break;
    case PillKind::kMapSmall: tex_name = "DIRTMASKS/MAPPILLSMALL"; texels = 256.0f; break;
    case PillKind::kMenu:
    default: tex_name = "DIRTMASKS/PILL01"; texels = 512.0f; break;
  }
  std::array<float, 4> uv{};
  if (!texture(tex_name, uv) && !texture("DIRTMASKS/PILL01", uv)) {
    l.rect(x0, y0, x1, y1, colour);
    return;
  }
  const float h = y1 - y0;
  const float texel = h / 26.0f;
  const float ty0 = y0 - 2.5f * texel;
  const float ty1 = y1 + 2.5f * texel;
  const float full_w = texels * texel;
  const float vis_w = x1 - x0;
  if (vis_w < full_w * 0.99f) {
    // Authentic Anark 2-texture border-transparent crop: tex0 shifted by (1 - vis/full)
    // multiplied with unshifted tex1 preserves feathered edges on both left and right ends.
    const float off = 1.0f - std::clamp(vis_w / std::max(1.0f, full_w), 0.02f, 1.0f);
    l.image_masked(x0, ty0, x0 + full_w, ty1, uv, uv, colour, {1.0f, 1.0f, off, 0.0f}, ui::kMaskBorderTransparent);
  } else {
    l.image(x0, ty0, x1, ty1, uv, colour);
  }
  if (with_clipon) {
    std::array<float, 4> clip{};
    if (texture("DIRTMASKS/PILLCLIPON01", clip)) {
      const float cw = 26.0f * texel;
      l.image(x0 - cw * 0.45f, ty0, x0 + cw * 0.55f, ty1, clip, colour);
    }
  }
  if (with_pin) {
    std::array<float, 4> pin{};
    if (texture("DIRTMASKS/PILLPIN", pin)) {
      const float ps = 14.0f * texel;
      l.image(x0, y1 - 3.0f * texel, x0 + ps, y1 + ps - 3.0f * texel, pin, colour);
    }
  }
}

void panel01(ui::List& l, float x0, float y0, float x1, float y1, float s, uint32_t bg, uint32_t header_accent) {
  if (x1 <= x0 || y1 <= y0) return;
  l.rect(x0 + 3.0f * s, y0 + 4.0f * s, x1 + 3.0f * s, y1 + 4.0f * s, ui::rgba(0, 0, 0, 96));
  std::array<float, 4> p_uv{}, n_uv{};
  if (texture("DIRTMASKS/PANEL01", p_uv)) {
    if (texture("DIRTMASKS/BGNOISE01", n_uv)) {
      l.image(x0, y0, x1, y1, p_uv, bg);
      l.image_masked(x0, y0, x1, y1, n_uv, p_uv, ui::rgba(255, 255, 255, 26), {1.0f, 1.0f, 0.0f, 0.0f}, ui::kMaskRepeat);
    } else {
      l.image(x0, y0, x1, y1, p_uv, bg);
    }
  } else {
    panel(l, x0, y0, x1, y1, s, bg, 0, 0, true);
  }
  if (header_accent != 0) {
    l.rect(x0 + 2.0f * s, y0 + 2.0f * s, x1 - 2.0f * s, y0 + 5.0f * s, header_accent);
  }
}

void title_tape(ui::List& l, float s, float x, float y, std::string_view title, uint32_t colour, float font_size,
                float max_w, bool clipon_left) {
  ui::TextStyle ts = style(ui::Font::kE, font_size * s, kWhite);
  fit_text(title, ts, max_w * s, 16.0f * s);
  const float tw = ui::List::measure(title, ts);
  const float bh = ts.size + 18.0f * s;
  const float x0 = x - 12.0f * s;
  const float x1 = x + std::max(210.0f * s, tw + 44.0f * s);
  if (clipon_left) {
    std::array<float, 4> clip{};
    if (texture("DIRTMASKS/TAPECLIPON02", clip) || texture("DIRTMASKS/TAPECLIPON01", clip)) {
      l.image(x0 - 22.0f * s, y + 3.0f * s, x0 + 18.0f * s, y + bh - 2.0f * s, clip, colour);
    }
  }
  brush(l, x0, y, x1, y + bh, colour, true);
  l.text(x + 12.0f * s, y + 0.5f * (bh - ts.size), title, ts);
}

bool opt_arrow(ui::List& l, float cx, float cy, float size, int dir, uint32_t tint) {
  const float r = size * 0.5f;
  std::array<float, 4> uv{};
  if (texture("OPTIONS/OPT_BAR_ARROW", uv) || texture("COMMON/ARROW", uv) || texture("DIRTMASKS/ARROW", uv)) {
    // OPTIONS/OPT_BAR_ARROW.xds natively points left (<); flip U for right (>).
    const std::array<float, 4> oriented = (dir > 0) ? std::array<float, 4>{uv[2], uv[1], uv[0], uv[3]} : uv;
    l.image(cx - r, cy - r, cx + r, cy + r, oriented, tint);
    return true;
  }
  // Sharp vector chevron fallback (never ASCII "<" / ">").
  const float dx = (dir < 0 ? -1.0f : 1.0f) * r * 0.45f;
  const float dy = r * 0.58f;
  const float stroke = std::max(1.6f, size * 0.14f);
  l.line(cx - dx, cy - dy, cx + dx, cy, stroke, tint);
  l.line(cx + dx, cy, cx - dx, cy + dy, stroke, tint);
  return false;
}

void opt_slider(ui::List& l, float x0, float y0, float x1, float y1, float norm, float s, uint32_t fill_col,
                bool focused, float default_norm) {
  if (x1 <= x0 || y1 <= y0) return;
  const float frac = clamp01(norm);
  const float bw = x1 - x0;
  std::array<float, 4> frame_uv{}, empty_uv{}, fill_uv{};
  const bool has_empty = texture("DIRTTEXTURES/STATBAREMPTY", empty_uv);
  const bool has_fill = texture("DIRTTEXTURES/STATBARFILL", fill_uv) || texture("OPTIONS/OPT_SLIDER_FILL", fill_uv);
  const bool has_frame = texture("OPTIONS/OPT_SLIDER_FRAME", frame_uv);

  if (has_empty) {
    l.image(x0, y0, x1, y1, empty_uv, ui::rgba(255, 255, 255, focused ? 145 : 95));
  } else {
    l.rect(x0, y0, x1, y1, focused ? ui::rgba(0, 0, 0, 130) : ui::rgba(255, 255, 255, 36));
  }

  if (frac > 0.005f) {
    const float fx1 = x0 + bw * frac;
    if (has_fill) {
      const float u1 = fill_uv[0] + (fill_uv[2] - fill_uv[0]) * frac;
      l.image(x0, y0, fx1, y1, {fill_uv[0], fill_uv[1], u1, fill_uv[3]}, fill_col);
    } else {
      l.rect(x0, y0, fx1, y1, fill_col);
    }
  }
  if (has_frame) {
    l.image(x0 - 1.5f * s, y0 - 1.5f * s, x1 + 1.5f * s, y1 + 1.5f * s, frame_uv,
            focused ? kWhite : ui::rgba(210, 214, 224, 150));
  }
  if (default_norm >= 0.0f && default_norm <= 1.0f) {
    const float dx = x0 + bw * default_norm;
    l.rect(dx - 1.0f * s, y0 - 2.5f * s, dx + 1.0f * s, y1 + 2.5f * s, kYellow);
  }
  const float kx = x0 + bw * frac;
  l.rect(kx - 2.0f * s, y0 - 2.0f * s, kx + 2.0f * s, y1 + 2.0f * s, kWhite);
}

void scroll_indicator(ui::List& l, float x0, float y0, float x1, float y1, int first_idx, int visible_count,
                      int total_count, float s) {
  if (total_count <= visible_count || total_count <= 0 || y1 <= y0) return;
  const float track_h = y1 - y0;
  std::array<float, 4> ind_uv{}, mask_uv{}, sheen_uv{};
  const bool has_ind = texture("COMMON/SCROLLINDICATOR", ind_uv);
  const bool has_mask = texture("COMMON/SCROLLINDICATORMASK", mask_uv);
  const bool has_sheen = texture("SPECULAR/SPEC_SHEEN2", sheen_uv);

  // Subtle vertical track
  l.rect(x0, y0, x1, y1, ui::rgba(255, 255, 255, 26));
  const float frac_h = std::clamp(float(visible_count) / float(total_count), 0.12f, 1.0f);
  const float thumb_h = std::max(22.0f * s, track_h * frac_h);
  const float max_first = float(std::max(1, total_count - visible_count));
  const float t = std::clamp(float(first_idx) / max_first, 0.0f, 1.0f);
  const float ty0 = y0 + (track_h - thumb_h) * t;
  const float ty1 = ty0 + thumb_h;

  if (has_ind && has_mask) {
    l.image_masked(x0, ty0, x1, ty1, ind_uv, mask_uv, kPink, {1.0f, 1.0f, 0.0f, 0.0f}, 0u);
    if (has_sheen) {
      l.image_masked(x0, ty0, x1, ty1, sheen_uv, mask_uv, ui::rgba(255, 255, 255, 110), {1.0f, 1.0f, 0.0f, 0.0f}, 0u);
    }
  } else if (has_ind) {
    l.image(x0, ty0, x1, ty1, ind_uv, kPink);
  } else {
    l.rect(x0, ty0, x1, ty1, kPink);
  }
}

void panel(ui::List& l, float x0, float y0, float x1, float y1, float s, uint32_t bg, uint32_t top_accent,
           uint32_t left_accent, bool gritty) {
  if (x1 <= x0 || y1 <= y0) return;
  // Crisp drop shadow behind sharp-edged FH1 card.
  l.rect(x0 + 3.0f * s, y0 + 4.0f * s, x1 + 3.0f * s, y1 + 4.0f * s, ui::rgba(0, 0, 0, 105));
  // Sharp charcoal base (ZERO rounded glass corners!).
  l.rect(x0, y0, x1, y1, bg);
  if (gritty) {
    std::array<float, 4> noise{};
    if (texture("DIRTMASKS/BGNOISE01", noise)) {
      l.image(x0, y0, x1, y1, noise, ui::rgba(255, 255, 255, 24));
    }
  }
  // Subtle crisp 1px edge hairline.
  l.rect(x0, y0, x1, y0 + 1.0f * s, ui::rgba(255, 255, 255, 30));
  l.rect(x0, y1 - 1.0f * s, x1, y1, ui::rgba(0, 0, 0, 95));
  if (top_accent != 0) {
    l.rect(x0, y0, x1, y0 + 4.0f * s, top_accent);
  }
  if (left_accent != 0) {
    l.rect(x0, y0, x0 + 5.0f * s, y1, left_accent);
  }
}

void offwhite_strip(ui::List& l, float x0, float y0, float x1, float y1, float s, bool brushed) {
  if (brushed) {
    brush(l, x0, y0, x1, y1, kOffWhite);
  } else {
    l.rect(x0, y0, x1, y1, kOffWhite);
    std::array<float, 4> noise{};
    if (texture("DIRTMASKS/BGNOISE01", noise)) {
      l.image(x0, y0, x1, y1, noise, ui::rgba(0, 0, 0, 26));
    }
  }
  (void)s;
}

void title_banner(ui::List& l, float s, float x, float y, std::string_view title, std::string_view eyebrow,
                  bool pink_title, float max_w) {
  const float cx = x + 180.0f * s;
  const float cy = y + 26.0f * s;
  TiltScope tilt(l, cx, cy, kTiltRad);

  float title_y = y;
  if (!eyebrow.empty()) {
    ui::TextStyle es = style(ui::Font::kE, 17.0f * s, kWhite);
    fit_text(eyebrow, es, max_w * s, 10.0f * s);
    const float ew = ui::List::measure(eyebrow, es);
    brush(l, x - 6.0f * s, y, x + ew + 34.0f * s, y + 27.0f * s, kBrushBlack);
    l.text(x + 12.0f * s, y + 4.5f * s, eyebrow, es);
    title_y = y + 24.0f * s;
  }

  ui::TextStyle ts = style(ui::Font::kE, 36.0f * s, kWhite);
  fit_text(title, ts, max_w * s, 16.0f * s);
  const float tw = ui::List::measure(title, ts);
  const float bh = ts.size + 18.0f * s;
  brush(l, x - 14.0f * s, title_y, x + std::max(240.0f * s, tw + 46.0f * s), title_y + bh,
        pink_title ? kPink : kBrushBlack, true);
  l.text(x + 14.0f * s, title_y + 0.5f * (bh - ts.size), title, ts);
}

void set_glyph_set(GlyphSet set) { active_glyph_set() = set; }
GlyphSet glyph_set() { return active_glyph_set(); }

std::string glyph_label_for(std::string_view button, GlyphSet set) {
  const std::string u = upper_copy(button);
  if (set == GlyphSet::kKeyboard) {
    return keyboard_keycap_label(u);
  }
  if (set == GlyphSet::kPlayStation) {
    if (u == "A" || u == "ACCEPT" || u == "CROSS") return "CROSS";
    if (u == "B" || u == "CANCEL" || u == "CIRCLE") return "CIRCLE";
    if (u == "X" || u == "ACTIVATE" || u == "SQUARE") return "SQUARE";
    if (u == "Y" || u == "REWIND" || u == "TRIANGLE") return "TRIANGLE";
    if (u == "LB" || u == "L1") return "L1";
    if (u == "RB" || u == "R1") return "R1";
    if (u == "LT" || u == "L2") return "L2";
    if (u == "RT" || u == "R2") return "R2";
    if (u == "LS" || u == "L3") return "L3";
    if (u == "RS" || u == "R3") return "R3";
    if (u == "START" || u == "MENU") return "OPT";
    if (u == "BACK" || u == "MAP") return "SHARE";
    if (u == "DPAD" || u == "D-PAD" || u == "DPAD_UD" || u == "DPAD_LR") return "D-PAD";
    return u;
  }
  if (u == "ACCEPT") return "A";
  if (u == "CANCEL") return "B";
  if (u == "ACTIVATE") return "X";
  if (u == "REWIND") return "Y";
  if (u == "MENU") return "START";
  if (u == "MAP") return "BACK";
  return u;
}

std::string glyph_label_for(std::string_view button) {
  return glyph_label_for(button, active_glyph_set());
}

std::string controller_button_from_texture(std::string_view texture_key) {
  std::string u = upper_copy(texture_key);
  for (char& c : u)
    if (c == '\\') c = '/';
  const size_t pos = u.find("CONTROLLER/");
  if (pos == std::string::npos) return {};
  u = u.substr(pos + 11);
  if (u.size() > 4 && (u.compare(u.size() - 4, 4, ".XDS") == 0 || u.compare(u.size() - 4, 4, ".TGA") == 0)) {
    u.resize(u.size() - 4);
  }
  if (u == "A_BUTTON" || u == "BUTTON_A" || u == "A") return "A";
  if (u == "B_BUTTON" || u == "BUTTON_B" || u == "B") return "B";
  if (u == "X_BUTTON" || u == "BUTTON_X" || u == "X") return "X";
  if (u == "ACTIVATE") return "ACTIVATE";  // the binding, drawn per glyph set
  if (u == "Y_BUTTON" || u == "BUTTON_Y" || u == "Y") return "Y";
  if (u == "LBUMPER" || u == "LB" || u == "L_BUMPER") return "LB";
  if (u == "RBUMPER" || u == "RB" || u == "R_BUMPER") return "RB";
  if (u == "LTRIGGER" || u == "LT" || u == "L_TRIGGER") return "LT";
  if (u == "RTRIGGER" || u == "RT" || u == "R_TRIGGER") return "RT";
  if (u == "START_BUTTON" || u == "START") return "START";
  if (u == "BACK_BUTTON" || u == "BACK") return "BACK";
  if (u == "D_PAD_UPDOWN") return "DPAD_UD";
  if (u == "D_PAD_LEFT_RIGHT") return "DPAD_LR";
  if (u == "D_PAD" || u == "DPAD") return "DPAD";
  if (u == "L_THUMBSTICK" || u == "LTHUMBSTICK" || u == "LS") return "LS";
  if (u == "RTHUMBSTICK" || u == "R_THUMBSTICK" || u == "RS") return "RS";
  return {};
}

bool draw_controller_texture(ui::List& l, float x0, float y0, float x1, float y1, std::string_view texture_key,
                             float alpha) {
  const std::string btn = controller_button_from_texture(texture_key);
  if (btn.empty()) return false;
  const float cx = 0.5f * (x0 + x1);
  const float cy = 0.5f * (y0 + y1);
  const float r = 0.5f * std::min(x1 - x0, y1 - y0);
  button_glyph(l, cx, cy, r, btn.c_str(), alpha);
  return true;
}

void set_controller_diagram_state(int layout_id, bool swap_handbrake_clutch) {
  auto& ds = diagram_sync();
  ds.layout_id = std::clamp(layout_id, 0, 4);
  ds.swap_hb_clutch = swap_handbrake_clutch;
}

void draw_controller_layout_diagram(ui::List& l, float x0, float y0, float x1, float y1, float s, int layout_id,
                                    bool swap_handbrake_clutch, std::string_view highlight_action) {
  if (x1 <= x0 || y1 <= y0) return;
  const int id = std::clamp(layout_id, 0, 4);

  // Authentic FH1 distressed PANEL01 backdrop.
  panel01(l, x0, y0, x1, y1, s, kCharcoalDark, kPink);

  const float w = x1 - x0;
  const float h = y1 - y0;
  const float cx = x0 + 0.5f * w;

  // Compact header bar with layout name & active glyph family tag.
  const char* dev_tag = active_glyph_set() == GlyphSet::kPlayStation ? "DUALSENSE"
                        : active_glyph_set() == GlyphSet::kKeyboard  ? "KEYBOARD"
                                                                     : "GAMEPAD";
  char hdr[64];
  std::snprintf(hdr, sizeof hdr, "CONTROLLER LAYOUT %d%s", id + 1, swap_handbrake_clutch ? " (HB/CL SWAPPED)" : "");
  pill(l, x0 + 8.0f * s, y0 + 8.0f * s, x1 - 8.0f * s, y0 + 28.0f * s, ui::rgba(0xBF, 0xBF, 0xBF, 238), PillKind::kMenu);
  l.text(x0 + 16.0f * s, y0 + 13.0f * s, hdr, style(ui::Font::kD, 11.0f * s, kInk));
  l.text(x1 - 16.0f * s, y0 + 13.5f * s, dev_tag, style(ui::Font::kE, 10.0f * s, kPink, ui::Align::kRight));

  // Resolve layout 0..4 action mappings (matching drive::default_controller_layout).
  struct SlotMap {
    const char* lt;
    const char* lb;
    const char* rt;
    const char* rb;
    const char* a;
    const char* b;
    const char* x;
    const char* y;
  };
  static constexpr SlotMap kLayouts[5] = {
      {"BRAKE", "CLUTCH", "GAS", "CAMERA", "HANDBRAKE", "SHIFT UP", "SHIFT DN", "REWIND"},
      {"BRAKE", "CLUTCH", "GAS", "CAMERA", "SHIFT DN", "HANDBRAKE", "SHIFT UP", "REWIND"},
      {"BRAKE", "SHIFT DN", "GAS", "SHIFT UP", "CLUTCH", "HANDBRAKE", "REWIND", "CAMERA"},
      {"SHIFT DN", "CLUTCH", "SHIFT UP", "CAMERA", "GAS", "HANDBRAKE", "BRAKE", "REWIND"},
      {"GAS", "CAMERA", "BRAKE", "CLUTCH", "HANDBRAKE", "SHIFT UP", "SHIFT DN", "REWIND"},
  };
  auto maybe_swap = [&](const char* act) -> const char* {
    if (!swap_handbrake_clutch) return act;
    if (std::string_view(act) == "HANDBRAKE") return "CLUTCH";
    if (std::string_view(act) == "CLUTCH") return "HANDBRAKE";
    return act;
  };
  const SlotMap& lm = kLayouts[id];

  // Center stylized vector gamepad silhouette (body, grips, sticks, D-pad, triggers, bumpers).
  const float body_cy = y0 + 0.56f * h;
  const float bw = std::min(126.0f * s, w * 0.34f);
  const float bh = std::min(68.0f * s, h * 0.36f);
  const float bx0 = cx - 0.5f * bw, bx1 = cx + 0.5f * bw;
  const float by0 = body_cy - 0.5f * bh, by1 = body_cy + 0.5f * bh;

  const uint32_t shell_col = ui::rgba(46, 50, 58, 245);
  const uint32_t shell_rim = ui::rgba(190, 198, 212, 180);
  const uint32_t dark_well = ui::rgba(18, 20, 26, 245);

  // Triggers (top shoulders) + bumpers
  l.rect(bx0 + bw * 0.08f, by0 - 11.0f * s, bx0 + bw * 0.30f, by0 - 1.0f * s, ui::rgba(62, 66, 76, 235), 3.0f * s);
  l.rect(bx1 - bw * 0.30f, by0 - 11.0f * s, bx1 - bw * 0.08f, by0 - 1.0f * s, ui::rgba(62, 66, 76, 235), 3.0f * s);
  // Left & right angled grips
  l.circle(bx0 + bw * 0.16f, by1 - bh * 0.12f, bh * 0.38f, shell_col);
  l.circle(bx1 - bw * 0.16f, by1 - bh * 0.12f, bh * 0.38f, shell_col);
  // Main central chassis
  l.rect(bx0, by0, bx1, by1 - bh * 0.18f, shell_col, 14.0f * s);
  l.frame(bx0, by0, bx1, by1 - bh * 0.18f, shell_rim, 1.4f * s, 14.0f * s);
  // Left stick + D-pad + Right stick
  const float ls_x = bx0 + bw * 0.24f, ls_y = by0 + bh * 0.36f;
  const float dp_x = bx0 + bw * 0.38f, dp_y = by0 + bh * 0.64f;
  const float rs_x = bx1 - bw * 0.36f, rs_y = by0 + bh * 0.64f;
  l.circle(ls_x, ls_y, 8.5f * s, dark_well);
  l.circle(ls_x, ls_y, 5.5f * s, ui::rgba(92, 98, 110, 240));
  l.circle(rs_x, rs_y, 8.5f * s, dark_well);
  l.circle(rs_x, rs_y, 5.5f * s, ui::rgba(92, 98, 110, 240));
  l.line(dp_x - 6.0f * s, dp_y, dp_x + 6.0f * s, dp_y, 3.0f * s, ui::rgba(190, 196, 208, 220));
  l.line(dp_x, dp_y - 6.0f * s, dp_x, dp_y + 6.0f * s, 3.0f * s, ui::rgba(190, 196, 208, 220));
  // Face diamond cluster on right side of gamepad
  const float fc_x = bx1 - bw * 0.22f, fc_y = by0 + bh * 0.36f, fd = 7.2f * s;
  button_glyph(l, fc_x, fc_y + fd, 4.2f * s, "A");
  button_glyph(l, fc_x + fd, fc_y, 4.2f * s, "B");
  button_glyph(l, fc_x - fd, fc_y, 4.2f * s, "X");
  button_glyph(l, fc_x, fc_y - fd, 4.2f * s, "Y");

  // Callout rows flanking left & right of the gamepad silhouette with leader lines!
  const std::string hi_up = upper_copy(highlight_action);
  auto draw_callout = [&](bool left_col, int row_i, const char* btn, const char* action_txt, float ax, float ay) {
    const float row_y = y0 + 38.0f * s + float(row_i) * 26.5f * s;
    const bool hi = !hi_up.empty() && upper_copy(action_txt).find(hi_up) != std::string::npos;
    const uint32_t line_col = hi ? kYellow : ui::rgba(255, 255, 255, 78);
    const uint32_t txt_col = hi ? kYellow : kWhite;
    if (left_col) {
      const float gx = x0 + 22.0f * s;
      const float tx = x0 + 40.0f * s;
      const float max_tx = bx0 - 10.0f * s;
      l.line(max_tx + 2.0f * s, row_y + 7.0f * s, ax, ay, 1.1f * s, line_col);
      button_glyph(l, gx, row_y + 7.0f * s, 8.2f * s, btn);
      ui::TextStyle ts = style(ui::Font::kB, 10.5f * s, txt_col);
      fit_text(action_txt, ts, std::max(36.0f * s, max_tx - tx), 7.5f * s);
      l.text(tx, row_y + 1.5f * s, action_txt, ts);
    } else {
      const float gx = x1 - 22.0f * s;
      const float tx = x1 - 40.0f * s;
      const float min_tx = bx1 + 10.0f * s;
      l.line(ax, ay, min_tx - 2.0f * s, row_y + 7.0f * s, 1.1f * s, line_col);
      button_glyph(l, gx, row_y + 7.0f * s, 8.2f * s, btn);
      ui::TextStyle ts = style(ui::Font::kB, 10.5f * s, txt_col, ui::Align::kRight);
      fit_text(action_txt, ts, std::max(36.0f * s, tx - min_tx), 7.5f * s);
      l.text(tx, row_y + 1.5f * s, action_txt, ts);
    }
  };

  // Left callouts (LT, LB, LS, DPAD, BACK)
  draw_callout(true, 0, "LT", maybe_swap(lm.lt), bx0 + bw * 0.16f, by0 - 6.0f * s);
  draw_callout(true, 1, "LB", maybe_swap(lm.lb), bx0 + bw * 0.18f, by0 + 4.0f * s);
  draw_callout(true, 2, "LS", "STEER", ls_x, ls_y);
  draw_callout(true, 3, "DPAD_LR", "RADIO", dp_x, dp_y);
  draw_callout(true, 4, "BACK", "MAP", cx - 10.0f * s, by0 + bh * 0.36f);

  // Right callouts (RT, RB, Y, B, X, A)
  draw_callout(false, 0, "RT", maybe_swap(lm.rt), bx1 - bw * 0.16f, by0 - 6.0f * s);
  draw_callout(false, 1, "RB", maybe_swap(lm.rb), bx1 - bw * 0.18f, by0 + 4.0f * s);
  draw_callout(false, 2, "Y", maybe_swap(lm.y), fc_x, fc_y - fd);
  draw_callout(false, 3, "B", maybe_swap(lm.b), fc_x + fd, fc_y);
  draw_callout(false, 4, "X", maybe_swap(lm.x), fc_x - fd, fc_y);
  draw_callout(false, 5, "A", maybe_swap(lm.a), fc_x, fc_y + fd);
}

void draw_info_tip(ui::List& l, const std::string& text, float w, float h) {
  // The (i) tip, top left over a faint band (f00420_first30a_orig.png and
  // the locked map's f01290: the icon at 80,46, the text from 96 at 720p).
  if (text.empty()) return;
  (void)w;
  const float s = h / 720.0f;
  const auto st = ui4::style(ui::Font::kB, 21.0f * s, ui4::kWhite, ui::Align::kLeft);
  const float tw = ui::List::measure(text, st);
  l.rect(67.0f * s, 32.0f * s, 96.0f * s + tw + 6.0f * s, 59.0f * s, ui::rgba(0, 0, 0, 90));
  const float cx = 80.0f * s, cy = 46.0f * s, r = 11.0f * s;
  l.circle(cx, cy, r, ui4::kWhite);
  l.circle(cx, cy, r - 2.0f * s, ui::rgba(20, 20, 20, 255));
  l.text(cx, cy - 8.0f * s, "i", ui4::style(ui::Font::kE, 16.0f * s, ui4::kWhite, ui::Align::kCenter));
  l.text(96.0f * s, cy - 11.0f * s, text, st);
}

void button_glyph(ui::List& l, float cx, float cy, float r, const char* button, float alpha) {
  const std::string b = button ? std::string(button) : std::string("A");
  if (active_glyph_set() == GlyphSet::kKeyboard && draw_keyboard_glyph(l, cx, cy, r, b, alpha)) {
    return;
  }
  if (active_glyph_set() == GlyphSet::kPlayStation && draw_playstation_glyph(l, cx, cy, r, b, alpha)) {
    return;
  }
  const std::string tex = controller_texture_name(b);
  std::array<float, 4> uv{};
  if (!tex.empty() && texture(tex, uv)) {
    const bool wide = (tex.find("BUMPER") != std::string::npos || tex.find("TRIGGER") != std::string::npos);
    const float rx = wide ? r * 1.45f : r;
    const float ry = wide ? r * 0.95f : r;
    l.image(cx - rx, cy - ry, cx + rx, cy + ry, uv, ui::fade(kWhite, alpha));
    return;
  }
  const uint32_t col = ui::fade(fallback_button_colour(b), alpha);
  const bool is_face_btn = (b == "A" || b == "B" || b == "X" || b == "Y");
  if (!is_face_btn) {
    // Styled rectangular keyboard keycap (zero pink circles for single-letter keys like G/M/R!).
    const float hw = std::max(r * 1.15f, float(b.size()) * r * 0.45f + 5.0f);
    const float hh = r * 0.88f;
    l.rect(cx - hw, cy - hh, cx + hw, cy + hh, ui::fade(ui::rgba(28, 30, 36, 240), alpha), 3.0f);
    l.frame(cx - hw, cy - hh, cx + hw, cy + hh, ui::fade(ui::rgba(222, 226, 234, 225), alpha), 1.4f, 3.0f);
    ui::TextStyle ts = style(ui::Font::kB, r * (b.size() > 1 ? 0.90f : 1.05f), ui::fade(kWhite, alpha),
                             ui::Align::kCenter);
    l.text(cx, cy - 0.5f * ts.size, b, ts);
  } else {
    l.circle(cx, cy, r, col);
    ui::TextStyle ts = style(ui::Font::kB, r * 1.05f, ui::fade(kInk, alpha), ui::Align::kCenter);
    l.text(cx, cy - 0.5f * ts.size, b, ts);
  }
}

void tab_strip(ui::List& l, float s, float x0, float y0, float max_x, const std::vector<std::string>& tabs,
               int active_idx, bool show_bumpers, uint32_t active_colour) {
  if (tabs.empty()) return;
  const float h = 30.0f * s;
  float cx = x0;
  if (show_bumpers) {
    button_glyph(l, cx + 16.0f * s, y0 + 0.5f * h, 11.0f * s, "LB");
    cx += 38.0f * s;
  }
  const float right_limit = show_bumpers ? (max_x - 38.0f * s) : max_x;
  const float avail = std::max(80.0f * s, right_limit - cx);
  const int n = int(tabs.size());
  const float gap = 6.0f * s;

  // Measure natural widths; compress smoothly if tabs exceed available width.
  std::vector<float> widths(size_t(n), 0.0f);
  float total_w = 0.0f;
  for (int i = 0; i < n; ++i) {
    ui::TextStyle ts = style(ui::Font::kE, 13.0f * s, kWhite);
    widths[size_t(i)] = std::max(72.0f * s, ui::List::measure(tabs[size_t(i)], ts) + 26.0f * s);
    total_w += widths[size_t(i)] + (i > 0 ? gap : 0.0f);
  }
  const float compress = total_w > avail ? (avail / total_w) : 1.0f;

  for (int i = 0; i < n; ++i) {
    const float tw = widths[size_t(i)] * compress;
    const bool act = (i == active_idx);
    l.add_hit(cx, y0, cx + tw, y0 + h, ui::kHitTab, i);
    if (act) {
      brush(l, cx, y0, cx + tw, y0 + h, active_colour);
    } else {
      pill(l, cx, y0, cx + tw, y0 + h, kPillGrey, PillKind::kMenu);
    }
    ui::TextStyle ts = style(ui::Font::kE, 12.5f * s, act ? kWhite : kDim, ui::Align::kCenter);
    fit_text(tabs[size_t(i)], ts, tw - 10.0f * s, 8.0f * s);
    l.text(cx + 0.5f * tw, y0 + 0.5f * (h - ts.size), tabs[size_t(i)], ts);
    cx += tw + gap;
  }
  if (show_bumpers) {
    button_glyph(l, std::min(cx + 16.0f * s, max_x - 16.0f * s), y0 + 0.5f * h, 11.0f * s, "RB");
  }
}

namespace {
ButtonBarDrawer g_button_bar_drawer = nullptr;
}  // namespace

void set_button_bar_drawer(ButtonBarDrawer drawer) { g_button_bar_drawer = drawer; }

float button_glyph_half_width(const char* button, float r) {
  const std::string btn = button ? std::string(button) : std::string("A");
  const std::string eff_lbl = glyph_label_for(btn);
  if (active_glyph_set() == GlyphSet::kKeyboard) {
    const float char_w = r * (eff_lbl.size() > 3 ? 0.50f : 0.62f);
    return std::max(r * 1.08f, float(eff_lbl.size()) * char_w * 0.56f + r * 0.45f);
  }
  if (btn == "LB" || btn == "RB" || btn == "LT" || btn == "RT" || btn.size() > 2 ||
      (active_glyph_set() == GlyphSet::kPlayStation && (eff_lbl == "OPT" || eff_lbl == "SHARE")))
    return std::max(r * 1.45f, float(eff_lbl.size()) * r * 0.42f + 4.0f);
  return r;
}

void button_bar(ui::List& l, const std::vector<Prompt>& items, float screen_w, float screen_h, float left_x_canvas) {
  if (items.empty()) return;
  if (g_button_bar_drawer && g_button_bar_drawer(l, items, screen_w, screen_h, left_x_canvas)) return;
  const float s = screen_h / 720.0f;
  float base_cy = screen_h - 52.0f * s;  // BUTTON_BAR_SCENE canvas y = 668
  // Authentic BUTTON_BAR_SCENE alternating micro-tilts (BSG y-up degrees: +1, -1, +1, -0.5, +0.5, 0, +1).
  static constexpr float kDefaultTiltsDeg[7] = {1.0f, -1.0f, 1.0f, -0.5f, 0.5f, 0.0f, 1.0f};
  float slot_tilts[7];
  for (int i = 0; i < 7; ++i) slot_tilts[i] = -kDefaultTiltsDeg[i] * (3.14159265f / 180.0f);

  if (const fh1::UiScene* bar = scene("BUTTON_BAR_SCENE")) {
    int slot_idx = 0;
    for (const auto& node : bar->nodes) {
      if (node.object_index < 0) continue;
      const std::string& nm = bar->objects[size_t(node.object_index)].name;
      if (nm == "BUTTON_BAR") {
        const float cy = 360.0f - node.y;
        if (cy > 420.0f && cy < 700.0f) base_cy = screen_h - (720.0f - cy) * s;
      } else if (slot_idx < 7 && (nm.rfind("Button_", 0) == 0 || nm.rfind("BUTTON_", 0) == 0) &&
                 bar->objects[size_t(node.object_index)].kind == fh1::UiObjectKind::kGroup) {
        if (std::fabs(node.angle) > 1e-4f || slot_idx == 5) {
          slot_tilts[slot_idx] = -node.angle;
        }
        ++slot_idx;
      }
    }
  }

  const float h = 30.0f * s;
  const float base_y = base_cy - 0.5f * h;
  float cx = left_x_canvas * s;
  int idx = 0;
  for (const auto& item : items) {
    if (item.label.empty()) continue;
    ui::TextStyle ts = style(ui::Font::kB, 14.0f * s, kWhite, ui::Align::kLeft, false);
    const float tw = ui::List::measure(item.label, ts);
    const std::string btn = item.button ? std::string(item.button) : std::string("A");
    const float r = 10.5f * s;
    const float glyph_hw = button_glyph_half_width(btn.c_str(), r);
    const float gx = cx + 10.0f * s + glyph_hw;
    const float btn_pad = 10.0f * s + 2.0f * glyph_hw + 6.0f * s;
    const float pw = tw + btn_pad + 14.0f * s;
    if (cx + pw > screen_w - 40.0f * s) break;

    const float tilt = slot_tilts[idx % 7];
    const float gy = base_y + 0.5f * h;
    TiltScope slot_tilt(l, cx + 0.5f * pw, gy, tilt);
    l.add_hit(cx, base_y, cx + pw, base_y + h, kHitButton, idx);
    pill(l, cx, base_y, cx + pw, base_y + h, kPillGrey, PillKind::kMenu, true);
    button_glyph(l, gx, gy, r, btn.c_str());
    l.text(cx + btn_pad, gy - 0.5f * ts.size, item.label, ts);
    cx += pw + 10.0f * s;
    ++idx;
  }
}

void class_badge(ui::List& l, float x, float y, float h, std::string_view class_name, int pi, int class_id_fallback) {
  const std::string cname = class_name.empty() ? std::string("?") : upper_copy(class_name);
  char pibuf[16];
  std::snprintf(pibuf, sizeof pibuf, "%d", std::max(0, pi));

  std::array<float, 4> uv{};
  if (cname != "?" && texture("PICLASS/CLASS_" + cname, uv)) {
    // PICLASS/CLASS_* texture places the class square on the left and white PI box centered around ~0.58*w.
    const float w = h * 3.45f;
    l.image(x, y, x + w, y + h, uv);
    ui::TextStyle ps = style(ui::Font::kB, h * 0.50f, kInk, ui::Align::kCenter);
    fit_text(pibuf, ps, w * 0.27f, h * 0.30f);
    l.text(x + w * 0.575f, y + 0.5f * (h - ps.size), pibuf, ps);
    return;
  }

  // Pixel-matched sharp two-tone class square + white PI square.
  const uint32_t col = class_id_fallback >= 0 ? class_colour_by_id(class_id_fallback) : class_colour_by_name(cname);
  const float sq = h * 1.12f;
  const float piw = h * 1.92f;
  l.rect(x, y, x + sq, y + h, col);
  l.rect(x + sq, y, x + sq + piw, y + h, kWhite);
  ui::TextStyle cs = style(ui::Font::kE, h * 0.68f, kWhite, ui::Align::kCenter);
  fit_text(cname, cs, sq - 4.0f, h * 0.38f);
  l.text(x + 0.5f * sq, y + 0.5f * (h - cs.size), cname, cs);
  ui::TextStyle ps = style(ui::Font::kB, h * 0.60f, kInk, ui::Align::kCenter);
  l.text(x + sq + 0.5f * piw, y + 0.5f * (h - ps.size), pibuf, ps);
}

void career_strip(ui::List& l, const CareerStripData& d, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  auto R = [&](float cx) { return screen_w - (1280.0f - cx) * s; };
  auto T = [&](float cy) { return cy * s; };

  // Row 1: Wristband goal pill + real WRISTBANDS/<COLOR> graphic (oracle pause-freeroam.png).
  if (!d.band_next.empty()) {
    const uint32_t col = wristband_colour(d.band_next);
    pill(l, R(812), T(36), R(1042), T(62), kPillGrey, PillKind::kHud01);
    std::string goal;
    if (d.band_collect_ready) {
      goal = "COLLECT " + upper_copy(d.band_next) + " WRISTBAND";
    } else if (d.band_hi > 0) {
      goal = thousands_str(d.band_points) + " / " + thousands_str(d.band_hi) + " TO " + upper_copy(d.band_next);
    } else {
      goal = upper_copy(d.band_next) + " WRISTBAND";
    }
    ui::TextStyle gs = style(ui::Font::kB, 14.0f * s, d.band_collect_ready ? col : kWhite, ui::Align::kRight);
    fit_text(goal, gs, R(1034) - R(822), 8.5f * s);
    l.text(R(1034), T(42) + 0.5f * (14.0f * s - gs.size), goal, gs);
  }

  std::string color_word = d.band_name.substr(0, d.band_name.find(' '));
  color_word = upper_copy(color_word);
  std::array<float, 4> wb_uv{};
  if (!color_word.empty() && texture("WRISTBANDS/" + color_word, wb_uv)) {
    l.image(R(1000), T(15), R(1256), T(79), wb_uv);
  } else if (!d.band_name.empty()) {
    const uint32_t bc = wristband_colour(d.band_name);
    l.rect(R(1045), T(34), R(1210), T(62), bc);
    std::string lbl = "HORIZON 2012";
    if (d.band_level > 0) lbl += "  " + std::to_string(d.band_level);
    ui::TextStyle bs = style(ui::Font::kE, 14.0f * s, kWhite, ui::Align::kCenter);
    fit_text(lbl, bs, R(1204) - R(1050), 8.0f * s);
    l.text(0.5f * (R(1045) + R(1210)), T(41), lbl, bs);
  }

  // Row 2: Car display name on dark pill + PICLASS badge.
  if (!d.car_name.empty()) {
    pill(l, R(892), T(64), R(1210), T(90), kPillGrey, PillKind::kHud02);
    ui::TextStyle cs = style(ui::Font::kB, 17.5f * s, kWhite, ui::Align::kRight, true);
    const float text_right = d.car_class.empty() ? R(1200) : R(1124);
    fit_text(d.car_name, cs, text_right - R(900), 9.0f * s);
    l.text(text_right, T(69) + 0.5f * (17.5f * s - cs.size), d.car_name, cs);
    if (!d.car_class.empty()) {
      class_badge(l, R(1130), T(66), 22.0f * s, d.car_class, d.car_pi);
    }
  }

  // Row 3: Optional upgrade discount tag + Off-white distressed Credits strip (matching pause-freeroam.png:
  // retail PlayerBar has wristband goal, wristband graphic, car+PI, and credits — no invented RANK pill).
  if (d.credits >= 0) {
    if (d.discount_percent > 0) {
      char db[32];
      std::snprintf(db, sizeof db, "%d%% OFF", d.discount_percent);
      brush(l, R(974), T(92), R(1078), T(116), kPink);
      l.text(R(1026), T(97), db, style(ui::Font::kE, 13.0f * s, kWhite, ui::Align::kCenter));
    }
    offwhite_strip(l, R(1084), T(92), R(1210), T(116), s, true);
    const std::string ctext = format_credits(d.credits);
    ui::TextStyle crs = style(ui::Font::kB, 16.0f * s, kInk, ui::Align::kRight, true);
    fit_text(ctext, crs, R(1200) - R(1092), 8.5f * s);
    l.text(R(1200), T(96) + 0.5f * (16.0f * s - crs.size), ctext, crs);
  }
}

void stat_bar(ui::List& l, float x, float y, float w, float h, std::string_view label, double cur_val,
              double preview_val, bool show_diff, float s) {
  const float label_w = 104.0f * s;
  const float val_w = 46.0f * s;
  const float bx0 = x + label_w;
  const float bx1 = x + w - val_w;
  const float bw = std::max(24.0f * s, bx1 - bx0);
  const float bh = std::max(8.0f * s, h * 0.52f);
  const float by = y + 0.5f * (h - bh);

  ui::TextStyle ls = style(ui::Font::kB, 12.5f * s, kDim);
  l.text(x, y + 0.5f * (h - ls.size), label, ls);

  // Authentic FH1 DIRTTEXTURES/STATBAREMPTY + STATBARFILL (fallback: UPGRADES/UPG_STATBAR).
  std::array<float, 4> empty_uv{}, bar_uv{};
  const bool has_empty = texture("DIRTTEXTURES/STATBAREMPTY", empty_uv);
  const bool has_tex = texture("DIRTTEXTURES/STATBARFILL", bar_uv) || texture("UPGRADES/UPG_STATBAR", bar_uv);
  if (has_empty) {
    l.image(bx0, by, bx1, by + bh, empty_uv, ui::rgba(255, 255, 255, 110));
  } else {
    l.rect(bx0, by, bx1, by + bh, ui::rgba(255, 255, 255, 28));
  }

  const float c0 = float(std::clamp(cur_val / 10.0, 0.0, 1.0));
  const float c1 = float(std::clamp(preview_val / 10.0, 0.0, 1.0));
  const float lo = show_diff ? std::min(c0, c1) : c0;

  auto fill_seg = [&](float f0, float f1, uint32_t col) {
    if (f1 <= f0 + 1e-4f) return;
    const float sx0 = bx0 + bw * f0;
    const float sx1 = bx0 + bw * f1;
    if (has_tex) {
      const float u0 = bar_uv[0] + (bar_uv[2] - bar_uv[0]) * f0;
      const float u1 = bar_uv[0] + (bar_uv[2] - bar_uv[0]) * f1;
      l.image(sx0, by, sx1, by + bh, {u0, bar_uv[1], u1, bar_uv[3]}, col);
    } else {
      l.rect(sx0, by, sx1, by + bh, col);
    }
  };

  fill_seg(0.0f, lo, kWhite);
  if (show_diff && std::fabs(c1 - c0) > 0.004f) {
    if (c1 > c0) {
      fill_seg(c0, c1, kGreen);
    } else {
      fill_seg(c1, c0, kRed);
    }
  }

  // Subtle 10-unit tick hairlines matching FH1 stat bars.
  for (int t = 1; t < 10; ++t) {
    const float tx = bx0 + bw * (float(t) / 10.0f);
    l.rect(tx, by, tx + 1.0f * s, by + bh, ui::rgba(10, 12, 16, 135));
  }

  char vbuf[16];
  const double shown = show_diff ? preview_val : cur_val;
  std::snprintf(vbuf, sizeof vbuf, "%.1f", std::clamp(shown, 0.0, 10.0));
  uint32_t vcol = kWhite;
  if (show_diff && preview_val > cur_val + 0.04) vcol = kGreen;
  else if (show_diff && preview_val < cur_val - 0.04) vcol = kRed;
  ui::TextStyle vs = style(ui::Font::kE, 13.0f * s, vcol, ui::Align::kRight);
  l.text(x + w, y + 0.5f * (h - vs.size), vbuf, vs);
}

bool manufacturer_logo(ui::List& l, float x0, float y0, float x1, float y1, std::string_view make_name, uint32_t tint) {
  if (make_name.empty()) return false;
  const std::string key = alnum_lower(make_name);
  if (key.empty() || key == "allaffordable" || key == "firstcartutorial") return false;

  // Map common database display/icon names to Textures.zip UI4/Logos/Manufacturer_Logos/L_MAN_*_Logo.
  static const std::pair<const char*, const char*> kAliases[] = {
      {"abarth", "Abarth"},         {"acura", "Acura"},           {"alfaromeo", "AlfaRomeo"},
      {"amc", "AMC"},               {"amg", "AMG"},               {"ascari", "Ascari"},
      {"astonmartin", "AstonMartin"}, {"audi", "Audi"},           {"bentley", "Bentley"},
      {"bmw", "BMW"},               {"bugatti", "Bugatti"},       {"buick", "Buick"},
      {"cadillac", "Cadillac"},     {"chevrolet", "Chevrolet"},   {"chrysler", "Chrysler"},
      {"citroen", "Citroen"},       {"dodge", "Dodge"},           {"eagle", "Eagle"},
      {"ferrari", "Ferrari"},       {"fiat", "Fiat"},             {"ford", "Ford"},
      {"gmc", "GMC"},               {"gumpert", "Gumpert"},       {"hennessey", "Hennessey"},
      {"honda", "Honda"},           {"hummer", "Hummer"},         {"hyundai", "Hyundai"},
      {"infiniti", "Infiniti"},     {"jaguar", "Jaguar"},         {"jeep", "Jeep"},
      {"koenigsegg", "Koenigsegg"}, {"lamborghini", "Lamborghini"}, {"lancia", "Lancia"},
      {"landrover", "LandRover"},   {"lexus", "Lexus"},           {"lotus", "Lotus"},
      {"maserati", "Maserati"},     {"mazda", "Mazda"},           {"mclaren", "McLaren"},
      {"mercedes", "Mercedes"},     {"mercedesbenz", "Mercedes"}, {"mini", "MINI"},
      {"mitsubishi", "Mitsubishi"}, {"nissan", "Nissan"},         {"noble", "Noble"},
      {"oldsmobile", "Oldsmobile"}, {"pagani", "Pagani"},         {"peugeot", "Peugeot"},
      {"plymouth", "Plymouth"},     {"pontiac", "Pontiac"},       {"radical", "Radical"},
      {"ram", "Ram"},               {"renault", "Renault"},       {"ruf", "RUF"},
      {"saleen", "Saleen"},         {"scion", "Scion"},           {"shelby", "Shelby"},
      {"srt", "SRT"},               {"ssc", "SSC"},               {"subaru", "Subaru"},
      {"toyota", "Toyota"},         {"tvr", "TVR"},               {"ultima", "Ultima"},
      {"vauxhall", "Vauxhall"},     {"viper", "Viper"},           {"volkswagen", "Volkswagen"},
      {"vw", "Volkswagen"},         {"volvo", "Volvo"},
  };

  std::string token;
  for (const auto& [k, v] : kAliases) {
    if (key == k) {
      token = v;
      break;
    }
  }
  if (token.empty()) token = std::string(make_name);

  std::array<float, 4> uv{};
  if (texture("T:UI4/Logos/Manufacturer_Logos/L_MAN_" + token + "_Logo", uv) ||
      texture("T:UI4/Logos/Manufacturer_Logos/L_MAN_" + upper_copy(token) + "_Logo", uv)) {
    l.image(x0, y0, x1, y1, uv, tint);
    return true;
  }
  return false;
}

bool car_thumbnail(ui::List& l, float x0, float y0, float x1, float y1, int64_t car_id, uint32_t tint) {
  if (car_id <= 0) return false;
  std::array<float, 4> uv{};
  if (texture("T:Thumbnails/Thumbnail_" + std::to_string(car_id), uv)) {
    // FH1 Thumbnail_<id>.xds packs the 3/4 showroom render in u=[0.04..0.66], v=[0.18..0.88]
    // and a vertical top-down minimap/body silhouette in the right 30%. Crop cleanly to the 3/4 view.
    const float du = uv[2] - uv[0];
    const float dv = uv[3] - uv[1];
    const std::array<float, 4> crop = {uv[0] + du * 0.04f, uv[1] + dv * 0.18f, uv[0] + du * 0.66f, uv[1] + dv * 0.88f};
    l.image(x0, y0, x1, y1, crop, tint);
    return true;
  }
  return false;
}

void subtle_vignette(ui::List& l, float w, float h, float strength) {
  const float k = clamp01(strength);
  if (k <= 0.01f) return;
  const uint8_t top_a = uint8_t(std::lround(75.0f * k));
  const uint8_t bot_a = uint8_t(std::lround(125.0f * k));
  l.fullscreen_band(0, 0, w, h * 0.22f, ui::rgba(4, 6, 10, top_a), ui::rgba(4, 6, 10, 0));
  l.fullscreen_band(0, h * 0.74f, w, h, ui::rgba(4, 6, 10, 0), ui::rgba(4, 6, 10, bot_a));
}

bool upgrade_part_icon(ui::List& l, float x0, float y0, float x1, float y1, int part_enum, int level, bool is_stock,
                       uint32_t tint) {
  auto try_draw = [&](const std::string& key) {
    std::array<float, 4> uv{};
    if (!texture(key, uv)) return false;
    l.image(x0, y0, x1, y1, uv, tint);
    return true;
  };
  const int tier = std::clamp(level, 1, 3);
  auto try_tiered = [&](const char* dir, const char* base, int max_tier = 3) {
    const int t = std::clamp(level, 1, max_tier);
    if (!is_stock && level > 0) {
      if (try_draw(std::string("T:UI4/Upgrades/") + dir + "/" + base + "_LG_" + std::to_string(t))) return true;
      if (try_draw(std::string("T:UI4/Upgrades/") + dir + "/" + base + "_" + std::to_string(t))) return true;
    }
    if (try_draw(std::string("T:UI4/Upgrades/") + dir + "/" + base + "_LG_Icon")) return true;
    return try_draw(std::string("T:UI4/Upgrades/") + dir + "/" + base + "_Icon");
  };
  (void)tier;

  switch (part_enum) {
    case 0: return try_tiered("Conversion", "conversion_engine", 1);
    case 1: return try_tiered("Conversion", "conversion_drivetrain", 1);
    case 2: return try_tiered("Conversion", "conversion", 1);
    case 4: return try_tiered("Platform_Handling", "brakes");
    case 5: return try_tiered("Platform_Handling", "springs_dampers");
    case 6: return try_tiered("Platform_Handling", "anti_roll_bars_front");
    case 7: return try_tiered("Platform_Handling", "anti_roll_bars_rear");
    case 8: return try_tiered("Tires_Rims", "tire_compound", 4);
    case 9: return try_tiered("Aero_Appearance", "rear_wing", 1);
    case 10:
    case 11: return try_tiered("Tires_Rims", "rim_size", 7);
    case 12: return try_tiered("Engine_Power", "camshaft");
    case 13: return try_tiered("Engine_Power", "valves");
    case 14: return try_tiered("Engine_Power", "displacement");
    case 15: return try_tiered("Engine_Power", "pistons_compression_NA");
    case 16: return try_tiered("Engine_Power", "fuel_system_injection");
    case 17: return try_tiered("Engine_Power", "ignition_electronic");
    case 18: return try_tiered("Engine_Power", "exhaust");
    case 19: return try_tiered("Engine_Power", "intake");
    case 20: return try_tiered("Engine_Power", "flywheel");
    case 21: return try_tiered("Engine_Power", "manifold_throttle_body");
    case 22: return try_tiered("Engine_Power", "restrictor_plate");
    case 23: return try_tiered("Engine_Power", "oil_cooling_system");
    case 24: return try_tiered("Engine_Power", "turbo_single");
    case 25: return try_tiered("Engine_Power", "turbo_twin");
    case 27: return try_tiered("Engine_Power", "centrifugal_supercharger");
    case 28: return try_tiered("Engine_Power", "pos_dis_supercharger");
    case 29: return try_tiered("Engine_Power", "intercooler");
    case 30: return try_tiered("Drivetrain", "clutch");
    case 31: return try_tiered("Drivetrain", "transmission");
    case 32: return try_tiered("Drivetrain", "driveline");
    case 33: return try_tiered("Drivetrain", "diff");
    case 34: return try_tiered("Aero_Appearance", "front_bumper", 1);
    case 35: return try_tiered("Aero_Appearance", "rear_bumper", 1);
    case 36: return try_tiered("Aero_Appearance", "hood", 1);
    case 37: return try_tiered("Aero_Appearance", "side_skirts", 1);
    case 38:
    case 39: return try_tiered("Tires_Rims", "tire_width", 5);
    case 40: return try_tiered("Platform_Handling", "weight_reduction");
    case 41: return try_tiered("Platform_Handling", "chassis_reinforcement");
    case 43: return is_stock ? try_draw("T:UI4/Upgrades/stock_LG_Icon") || try_draw("T:UI4/Upgrades/stock_Icon")
                             : try_tiered("Tires_Rims", "rim_style", 1);
    default:
      return try_draw("T:UI4/Upgrades/Engine_Power/engine_power_Icon") || try_draw("T:UI4/Upgrades/stock_Icon");
  }
}

bool tuning_icon(ui::List& l, float x0, float y0, float x1, float y1, int tune_category, std::string_view slider_label,
                 uint32_t tint) {
  const std::string lbl = lower_copy(slider_label);
  const char* tex = "T:UI4/Tuning/I_TUN_Springs";
  if (lbl.find("camber") != std::string::npos) tex = "T:UI4/Tuning/I_TUN_Alignment_Camber";
  else if (lbl.find("toe") != std::string::npos) tex = "T:UI4/Tuning/I_TUN_Alignment_Toe";
  else if (lbl.find("caster") != std::string::npos) tex = "T:UI4/Tuning/I_TUN_Alignment_Caster";
  else if (lbl.find("ride height") != std::string::npos) tex = "T:UI4/Tuning/I_TUN_Springs_RideHeight";
  else if (lbl.find("front") != std::string::npos && lbl.find("downforce") != std::string::npos)
    tex = "T:UI4/Tuning/I_TUN_Aero_Front";
  else if (lbl.find("rear") != std::string::npos && lbl.find("downforce") != std::string::npos)
    tex = "T:UI4/Tuning/I_TUN_Aero_Rear";
  else {
    switch (tune_category) {
      case 0: tex = "T:UI4/Tuning/I_TUN_TireCompound"; break;
      case 1: tex = "T:UI4/Tuning/I_TUN_Differential_RWD"; break;
      case 2: tex = "T:UI4/Tuning/I_TUN_Alignment_Camber"; break;
      case 3: tex = "T:UI4/Tuning/I_TUN_AntiRoll"; break;
      case 4: tex = "T:UI4/Tuning/I_TUN_Springs"; break;
      case 5: tex = "T:UI4/Tuning/I_TUN_Damping"; break;
      case 6: tex = "T:UI4/Tuning/I_TUN_Aero_Front"; break;
      case 7: tex = "T:UI4/Tuning/I_TUN_Brakes"; break;
      case 8: tex = "T:UI4/Tuning/I_TUN_Differential_AWD"; break;
      default: tex = "T:UI4/Tuning/I_TUN_Springs"; break;
    }
  }
  std::array<float, 4> uv{};
  if (texture(tex, uv) || texture("T:UI4/Tuning/I_TUN_Springs", uv)) {
    l.image(x0, y0, x1, y1, uv, tint);
    return true;
  }
  return false;
}

bool event_ticket_art(ui::List& l, float x0, float y0, float x1, float y1, int event_id, int style, uint32_t tint) {
  auto try_tex = [&](const std::string& path) {
    std::array<float, 4> uv{};
    if (!texture(path, uv)) return false;
    l.image(x0, y0, x1, y1, uv, tint);
    return true;
  };
  // Outpost PR Stunt mission tickets (note exact disc spelling: MissonTicket_*).
  if (style == 10) {
    return try_tex("EVENTTICKETS/MISSIONS/MISSONTICKET_SPEEDSTUNT") || try_tex("EVENTTICKETS/MISSIONSPEEDTEMPLATE");
  }
  if (style == 11) {
    return try_tex("EVENTTICKETS/MISSIONS/MISSONTICKET_SKILLSTUNT") || try_tex("EVENTTICKETS/MISSIONPRTEMPLATE");
  }
  if (style == 12) {
    return try_tex("EVENTTICKETS/MISSIONS/MISSONTICKET_PHOTOSHOOT") || try_tex("EVENTTICKETS/MISSIONPHOTOTEMPLATE");
  }

  char idbuf[16];
  std::snprintf(idbuf, sizeof idbuf, "%03d", std::max(0, event_id));
  if (style == 2) {
    // Street Races (Horizon.zip/EventTickets/StreetRaces/038..099.xds)
    if (event_id > 0 && try_tex(std::string("EVENTTICKETS/STREETRACES/") + idbuf)) return true;
    static constexpr const char* kStreetFallback[] = {"050", "051", "052", "053", "054", "055", "056", "057", "080", "081"};
    if (try_tex(std::string("EVENTTICKETS/STREETRACES/") + kStreetFallback[std::abs(event_id) % 10])) return true;
  } else if (style == 3) {
    // Showcases (Horizon.zip/EventTickets/Showcases/003..246.xds)
    if (event_id > 0 && try_tex(std::string("EVENTTICKETS/SHOWCASES/") + idbuf)) return true;
    static constexpr const char* kShowFallback[] = {"003", "058", "059", "101", "168", "172", "210", "211"};
    if (try_tex(std::string("EVENTTICKETS/SHOWCASES/") + kShowFallback[std::abs(event_id) % 8])) return true;
  } else if (style == 4 || style == 5) {
    // Showdown / Nemesis (Horizon.zip/EventTickets/NemesisTickets/079..245.xds)
    if (event_id > 0 && try_tex(std::string("EVENTTICKETS/NEMESISTICKETS/") + idbuf)) return true;
    static constexpr const char* kNemFallback[] = {"079", "169", "241", "242", "243", "244", "245"};
    if (try_tex(std::string("EVENTTICKETS/NEMESISTICKETS/") + kNemFallback[std::abs(event_id) % 7])) return true;
  } else {
    // Festival Races (Horizon.zip/EventTickets/FestivalRaces/004..*.xds)
    if (event_id > 0 && try_tex(std::string("EVENTTICKETS/FESTIVALRACES/") + idbuf)) return true;
    static constexpr const char* kFestFallback[] = {"004", "024", "026", "029", "030", "031", "034", "035", "060", "061"};
    if (try_tex(std::string("EVENTTICKETS/FESTIVALRACES/") + kFestFallback[std::abs(event_id) % 10])) return true;
  }
  return try_tex("EVENTTICKETS/FESTIVALRACETEMPLATE") || try_tex("EVENTTICKETS/EXHIBITIONRACETEMPLATE");
}

bool sponsor_logo_tile(ui::List& l, float x0, float y0, float x1, float y1, std::string_view sponsor_or_id,
                       uint32_t tint) {
  const std::string key = alnum_lower(sponsor_or_id);
  static const std::pair<const char*, const char*> kMap[] = {
      {"adidas", "ADIDAS"},       {"bondurant", "BONDURANT"}, {"bose", "BOSE"},
      {"diesel", "DIESEL"},       {"edefice", "EDEFICE"},     {"edifice", "EDEFICE"},
      {"gopro", "GOPRO"},         {"gshock", "GSHOCK"},       {"hks", "HKS"},
      {"horizon", "HORIZONLOGO"}, {"krashmax", "KRASHMAX"},   {"nike", "NIKE"},
      {"nixon", "NIXON"},         {"oakley", "OAKLEY"},       {"pioneer", "PIONEER"},
      {"puma", "PUMA"},           {"redbull", "REDBULL"},     {"rockstar", "ROCKSTAR"},
      {"zooyork", "ZOOYORK"},
      // Skill-challenge category aliases -> authentic licensed shipping FH1 sponsor logos
      {"drift", "ROCKSTAR"},      {"nearmiss", "BONDURANT"},  {"speed", "OAKLEY"},
      {"air", "GOPRO"},           {"burnout", "BOSE"},        {"draft", "ADIDAS"},
      {"pass", "PUMA"},           {"destruction", "DIESEL"},  {"fame", "HORIZONLOGO"},
      {"skill", "GOPRO"},         {"tradingpaint", "KRASHMAX"}, {"donut", "ZOOYORK"},
      {"turn180", "GSHOCK"},      {"twowheels", "NIKE"},      {"jturn", "NIXON"},
      {"overtake", "PIONEER"},    {"audio", "BOSE"},          {"watch", "EDEFICE"},
  };
  const char* tex_name = "HORIZONLOGO";
  for (const auto& [needle, tile] : kMap) {
    if (key.find(needle) != std::string::npos) {
      tex_name = tile;
      break;
    }
  }
  std::array<float, 4> uv{};
  if (texture(std::string("SPONSORLOGOTILE/") + tex_name, uv) || texture("SPONSORLOGOTILE/HORIZONLOGO", uv)) {
    l.image(x0, y0, x1, y1, uv, tint);
    return true;
  }
  return false;
}

bool badge_icon(ui::List& l, float x0, float y0, float x1, float y1, BadgeIconKind kind, uint32_t tint) {
  auto try_tex = [&](const char* key) {
    std::array<float, 4> uv{};
    if (!texture(key, uv)) return false;
    l.image(x0, y0, x1, y1, uv, tint);
    return true;
  };
  switch (kind) {
    case BadgeIconKind::kGoldMedal:
      return try_tex("T:UI4/COMMON/ICON_GOLD") || try_tex("ICONS/TICKETSTICKERGOLD") ||
             try_tex("MAP/ICONS/STREETRACEICONS/GOLD") || try_tex("HUD/TROPHY");
    case BadgeIconKind::kSilverMedal:
      return try_tex("ICONS/TICKETSTICKERSILVER") || try_tex("MAP/ICONS/STREETRACEICONS/SILVER");
    case BadgeIconKind::kBronzeMedal:
      return try_tex("ICONS/TICKETSTICKERBRONZE") || try_tex("MAP/ICONS/STREETRACEICONS/BRONZE");
    case BadgeIconKind::kTrophy:
      return try_tex("HUD/TROPHY") || try_tex("T:UI4/COMMON/ICON_GOLD") || try_tex("ICONS/TICKETSTICKERGOLD");
    case BadgeIconKind::kLocked:
      return try_tex("T:A_MAP/A_COL_COLLECTIVE/A_COL_PADLOCK") || try_tex("T:UI4/BUYCAR/A_COL_PADLOCK") ||
             try_tex("ICONS/ACTIVATIONZONEICONS/LOCKED");
    case BadgeIconKind::kNew:
      return try_tex("ICONS/TICKETSTICKERNEW");
    case BadgeIconKind::kComplete:
      return try_tex("ICONS/MISSIONCOMPLETE") || try_tex("COMMON/TICK");
  }
  return false;
}

std::string pretty_car_media_name(std::string_view raw_media) {
  if (raw_media.empty()) return {};
  // Already looks like a display name (contains a space and no underscores).
  if (raw_media.find('_') == std::string_view::npos && raw_media.find(' ') != std::string_view::npos) {
    return std::string(raw_media);
  }
  // Parse Forza media naming convention: MAKE_ModelName_YY (e.g. FER_F40_87, NIS_SKYLINEGTRVSPECII_02).
  std::string s(raw_media);
  const size_t u1 = s.find('_');
  if (u1 == std::string::npos) return s;
  const size_t u2 = s.rfind('_');
  const std::string mk_raw = upper_copy(s.substr(0, u1));
  std::string model_raw = (u2 > u1) ? s.substr(u1 + 1, u2 - u1 - 1) : s.substr(u1 + 1);
  std::string yr_raw = (u2 > u1) ? s.substr(u2 + 1) : std::string{};

  static const std::pair<const char*, const char*> kMakes[] = {
      {"ABA", "Abarth"},       {"ACU", "Acura"},         {"ALF", "Alfa Romeo"}, {"AMC", "AMC"},
      {"AST", "Aston Martin"}, {"AUD", "Audi"},          {"BEN", "Bentley"},    {"BMW", "BMW"},
      {"BUG", "Bugatti"},      {"BUI", "Buick"},         {"CAD", "Cadillac"},   {"CHE", "Chevrolet"},
      {"CHR", "Chrysler"},     {"CIT", "Citroen"},       {"DOD", "Dodge"},      {"FER", "Ferrari"},
      {"FIA", "Fiat"},         {"FOR", "Ford"},          {"GMC", "GMC"},        {"GUM", "Gumpert"},
      {"HEN", "Hennessey"},    {"HON", "Honda"},         {"HUM", "Hummer"},     {"HYU", "Hyundai"},
      {"INF", "Infiniti"},     {"JAG", "Jaguar"},        {"JEE", "Jeep"},       {"KOE", "Koenigsegg"},
      {"LAM", "Lamborghini"},  {"LAN", "Lancia"},        {"LEX", "Lexus"},      {"LOT", "Lotus"},
      {"MAS", "Maserati"},     {"MAZ", "Mazda"},         {"MCL", "McLaren"},    {"MER", "Mercedes-Benz"},
      {"MIN", "MINI"},         {"MIT", "Mitsubishi"},    {"NIS", "Nissan"},     {"NOB", "Noble"},
      {"OLD", "Oldsmobile"},   {"PAG", "Pagani"},        {"PEU", "Peugeot"},    {"PLY", "Plymouth"},
      {"PON", "Pontiac"},      {"RAD", "Radical"},       {"REN", "Renault"},    {"RUF", "RUF"},
      {"SAL", "Saleen"},       {"SCI", "Scion"},         {"SHE", "Shelby"},     {"SRT", "SRT"},
      {"SSC", "SSC"},          {"SUB", "Subaru"},        {"TOY", "Toyota"},     {"TVR", "TVR"},
      {"ULT", "Ultima"},       {"VAU", "Vauxhall"},      {"VOL", "Volkswagen"}, {"VW", "Volkswagen"},
  };
  std::string make = mk_raw;
  for (const auto& [code, name] : kMakes) {
    if (mk_raw == code) {
      make = name;
      break;
    }
  }

  // Known iconic model cleanups before generic CamelCase/token formatting.
  const std::string model_up = upper_copy(model_raw);
  std::string model;
  if (model_up == "SKYLINEGTRVSPECII") model = "Skyline GT-R V-Spec II";
  else if (model_up == "IMPREZA22B") model = "Impreza 22B STi";
  else if (model_up == "CORRADOVR6") model = "Corrado VR6";
  else if (model_up == "VIPERGTS" || model_up == "SRTVIPERGTS") model = "SRT Viper GTS";
  else if (model_up == "ENZO") model = "Enzo Ferrari";
  else if (model_up == "F40") model = "F40";
  else if (model_up == "GT") model = "GT";
  else if (model_up == "GT40") model = "GT40";
  else if (model_up == "LANCEREVO10GSR" || model_up == "LANCEREVOX") model = "Lancer Evo X GSR";
  else {
    for (size_t i = 0; i < model_raw.size(); ++i) {
      const char c = model_raw[i];
      if (c == '_') {
        if (!model.empty() && model.back() != ' ') model.push_back(' ');
        continue;
      }
      if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
          std::islower(static_cast<unsigned char>(model_raw[i - 1]))) {
        model.push_back(' ');
      }
      model.push_back(c);
    }
  }

  std::string year;
  if (yr_raw.size() == 2 && std::isdigit(static_cast<unsigned char>(yr_raw[0])) &&
      std::isdigit(static_cast<unsigned char>(yr_raw[1]))) {
    const int y2 = std::stoi(yr_raw);
    year = std::to_string(y2 >= 40 ? (1900 + y2) : (2000 + y2));
  } else if (yr_raw.size() == 4 && std::isdigit(static_cast<unsigned char>(yr_raw[0]))) {
    year = yr_raw;
  }

  std::string out;
  if (!year.empty()) out += year + " ";
  out += make;
  if (!model.empty()) out += " " + model;
  return out;
}

void tilted_blade_screen(ui::List& l, float w, float h, const TiltedBladeSpec& spec) {
  const float raw_p = clamp01(spec.open_progress);
  if (raw_p <= 0.001f) return;
  const float ease = ease_out3(raw_p);
  const bool animating = raw_p < 0.999f;
  auto fa = [&](uint32_t col) -> uint32_t { return animating ? ui::fade(col, ease) : col; };

  const float s = h / 720.0f;
  const float slide_l = (1.0f - ease) * (-72.0f * s);
  const float slide_r = (1.0f - ease) * (56.0f * s);

  // Open Colorado shines cleanly around floating +3.0 deg pills (ZERO opaque full-screen dark wall!).
  subtle_vignette(l, w, h, 0.24f * ease);

  float tilt_rad = kTiltRad;
  if (!spec.scene_hint.empty()) {
    if (const fh1::UiScene* sc_ptr = scene(spec.scene_hint)) {
      for (const auto& node : sc_ptr->nodes) {
        if (node.object_index < 0 || size_t(node.object_index) >= sc_ptr->objects.size()) continue;
        const std::string& nm = sc_ptr->objects[size_t(node.object_index)].name;
        if (nm == "RotationHelper" || nm == "Group_BLADE" || nm == "Group_BODY") {
          const float a = -node.angle;
          if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) {
            tilt_rad = a;
            break;
          }
        }
      }
    }
  }

  // Compact top-right status strip with opaque dark backing so bright backgrounds never wash out text.
  if (!spec.status_value.empty()) {
    const float sx1 = w - 58.0f * s + slide_r;
    const float sx0 = sx1 - 356.0f * s;
    const float sy0 = 32.0f * s;
    l.rect(sx0, sy0, sx1, sy0 + 48.0f * s, fa(ui::rgba(14, 16, 22, 238)));
    pill(l, sx0, sy0, sx1, sy0 + 48.0f * s, fa(kCharcoalDark), PillKind::kPillBig, false, true);
    l.frame(sx0, sy0, sx1, sy0 + 48.0f * s, fa(ui::rgba(255, 255, 255, 36)), 1.0f * s);
    l.rect(sx0, sy0, sx0 + 5.0f * s, sy0 + 48.0f * s, fa(spec.status_color));
    if (!spec.status_kicker.empty()) {
      l.text(sx0 + 16.0f * s, sy0 + 7.0f * s, spec.status_kicker, style(ui::Font::kB, 10.0f * s, fa(kOffWhite)));
    }
    if (!spec.status_tag.empty()) {
      l.text(sx1 - 14.0f * s, sy0 + 7.0f * s, spec.status_tag,
             style(ui::Font::kE, 10.5f * s, fa(spec.status_color), ui::Align::kRight));
    }
    ui::TextStyle sv = style(ui::Font::kE, 16.0f * s, fa(kWhite));
    fit_text(spec.status_value, sv, 326.0f * s, 10.0f * s);
    l.text(sx0 + 16.0f * s, sy0 + 24.0f * s, spec.status_value, sv);
  }

  auto wrap_blade_text = [](const std::string& text, const ui::TextStyle& ts, float max_w, int max_lines) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i < text.size() && int(out.size()) < max_lines) {
      while (i < text.size() && text[i] == ' ') ++i;
      if (i >= text.size()) break;
      if (text[i] == '\n') {
        if (!cur.empty()) out.push_back(cur), cur.clear();
        ++i;
        continue;
      }
      size_t j = i;
      while (j < text.size() && text[j] != ' ' && text[j] != '\n') ++j;
      const std::string word = text.substr(i, j - i);
      i = j;
      const std::string cand = cur.empty() ? word : (cur + " " + word);
      if (!cur.empty() && ui::List::measure(cand, ts) > max_w) {
        out.push_back(cur);
        cur = word;
      } else {
        cur = cand;
      }
    }
    if (!cur.empty() && int(out.size()) < max_lines) out.push_back(cur);
    return out;
  };

  // ---- Entire blade tilted +3.0 deg (H_FIXED_SETTING_LIST / 300_I_OPT_ASSISTS_DIFFICULTY) ----
  {
    TiltScope rot(l, 0.5f * w, 0.5f * h, tilt_rad);

    // 1. TitleTape (TAPECLIPON02 + SMUDGETAPE01 + CRISPTAPE01 + DIRTTILE01) + optional eyebrow
    float header_y = 32.0f * s;
    if (!spec.eyebrow.empty()) {
      ui::TextStyle es = style(ui::Font::kE, 14.0f * s, fa(kWhite));
      fit_text(spec.eyebrow, es, 560.0f * s, 9.5f * s);
      const float ew = ui::List::measure(spec.eyebrow, es);
      brush(l, 74.0f * s + slide_l, header_y, 74.0f * s + slide_l + ew + 34.0f * s, header_y + 24.0f * s,
            fa(kBrushBlack), false, ease);
      l.text(90.0f * s + slide_l, header_y + 4.5f * s, spec.eyebrow, es);
      header_y += 22.0f * s;
    }
    {
      ui::TextStyle tts = style(ui::Font::kE, 34.0f * s, fa(kWhite));
      fit_text(spec.title, tts, 580.0f * s, 16.0f * s);
      const float tw = ui::List::measure(spec.title, tts);
      const float bh = tts.size + 18.0f * s;
      const float tx = 80.0f * s + slide_l;
      const float bx0 = tx - 12.0f * s;
      const float bx1 = tx + std::max(210.0f * s, tw + 44.0f * s);
      std::array<float, 4> clip{};
      if (texture("DIRTMASKS/TAPECLIPON02", clip) || texture("DIRTMASKS/TAPECLIPON01", clip)) {
        l.image(bx0 - 22.0f * s, header_y + 3.0f * s, bx0 + 18.0f * s, header_y + bh - 2.0f * s, clip, fa(kPink));
      }
      brush(l, bx0, header_y, bx1, header_y + bh, fa(kPink), true, ease);
      l.text(tx + 12.0f * s, header_y + 0.5f * (bh - tts.size), spec.title, tts);
    }

    float content_y = header_y + 58.0f * s;
    if (!spec.tabs.empty()) {
      tab_strip(l, s, 78.0f * s + slide_l, content_y, w - 78.0f * s + slide_l, spec.tabs, spec.active_tab,
                spec.show_tab_bumpers, fa(kPink));
      content_y += 42.0f * s;
    }

    // 2. Floating Left List Column (PILL01 #BFBFBF header + floating PILL02/03 rows over open Colorado)
    const float lx0 = 78.0f * s + slide_l;
    const float lw = 696.0f * s;
    const float list_bot = h - 98.0f * s - (spec.notice.empty() ? 0.0f : 32.0f * s);

    pill(l, lx0, content_y, lx0 + lw, content_y + 28.0f * s, fa(ui::rgba(0xBF, 0xBF, 0xBF, 242)), PillKind::kMenu,
         false, true);
    l.text(lx0 + 16.0f * s, content_y + 7.0f * s, spec.list_header, style(ui::Font::kD, 13.0f * s, fa(kInk)));
    if (!spec.list_header_right.empty()) {
      l.text(lx0 + lw - 16.0f * s, content_y + 7.5f * s, spec.list_header_right,
             style(ui::Font::kB, 11.5f * s, fa(kInk), ui::Align::kRight));
    }

    const float rows_top = content_y + 34.0f * s;
    const int n = int(spec.rows.size());
    bool any_sub = false;
    for (const auto& r : spec.rows) {
      if (!r.subtitle.empty() || r.has_slider) {
        any_sub = true;
        break;
      }
    }
    const float row_h = (any_sub ? 44.0f : 34.0f) * s;
    const float row_pitch = row_h + 5.0f * s;
    const int max_vis = std::min(std::max(1, spec.max_visible), std::max(1, int((list_bot - rows_top) / row_pitch)));
    const int sel = n > 0 ? std::clamp(spec.selected_index, 0, n - 1) : 0;
    const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, n - max_vis));

    float ry = rows_top;
    if (n == 0) {
      pill(l, lx0, ry, lx0 + lw, ry + 42.0f * s, fa(kPillGrey), PillKind::kPill02);
      ui::TextStyle es = style(ui::Font::kB, 12.5f * s, fa(kDim), ui::Align::kLeft, true);
      fit_text(spec.empty_message, es, lw - 32.0f * s, 9.0f * s);
      l.text(lx0 + 16.0f * s, ry + 14.0f * s, spec.empty_message, es);
    } else {
      for (int i = first; i < std::min(n, first + max_vis); ++i) {
        const auto& r = spec.rows[size_t(i)];
        const bool act = r.selected || (i == sel);
        l.add_hit(lx0, ry, lx0 + lw, ry + row_h, ui::kHitRow, i);
        const uint32_t bg_col = act ? (r.row_accent != 0 ? r.row_accent : kPink)
                                    : ((i % 2 == 0) ? kPillGrey : ui::rgba(0x18, 0x18, 0x1C, 236));
        pill(l, lx0, ry, lx0 + lw, ry + row_h, fa(bg_col), (i % 2 == 0) ? PillKind::kPill02 : PillKind::kPill03, false,
             act);

        const bool dark_ink_row = act && (r.row_accent == kAmber || r.row_accent == kYellow || r.row_accent == kGreen);
        const uint32_t primary_fg = fa(dark_ink_row ? kInk : kWhite);
        const uint32_t sub_fg = fa(dark_ink_row ? kInk : (act ? kOffWhite : kDim));

        float tx = lx0 + 16.0f * s;
        if (!r.index_label.empty()) {
          l.text(tx, ry + 0.5f * (row_h - 13.0f * s), r.index_label,
                 style(ui::Font::kE, 13.0f * s, fa(dark_ink_row ? kInk : kDim)));
          tx += 26.0f * s;
        }
        if (r.has_checkbox) {
          const float bs = 18.0f * s;
          const float by = ry + 0.5f * (row_h - bs);
          l.rect(tx, by, tx + bs, by + bs, fa(r.checked ? r.checkbox_color : ui::rgba(255, 255, 255, 34)));
          if (r.checked) {
            l.line(tx + 3.5f * s, by + 9.0f * s, tx + 7.5f * s, by + 13.5f * s, 2.2f * s, fa(kInk));
            l.line(tx + 7.5f * s, by + 13.5f * s, tx + 14.5f * s, by + 4.5f * s, 2.2f * s, fa(kInk));
          }
          tx += 26.0f * s;
        }
        if (r.has_custom_icon) {
          const float isz = std::min(row_h - 6.0f * s, 32.0f * s);
          const float iy = ry + 0.5f * (row_h - isz);
          l.rect(tx - 1.0f * s, iy - 1.0f * s, tx + isz + 1.0f * s, iy + isz + 1.0f * s, fa(ui::rgba(10, 12, 16, 210)));
          l.image(tx, iy, tx + isz, iy + isz, r.custom_icon_uv, fa(kWhite));
          l.frame(tx - 1.0f * s, iy - 1.0f * s, tx + isz + 1.0f * s, iy + isz + 1.0f * s,
                  fa(ui::rgba(255, 255, 255, 48)), 1.0f * s);
          tx += isz + 10.0f * s;
        } else if (r.badge_icon_kind >= 0) {
          const float isz = std::min(row_h - 8.0f * s, 30.0f * s);
          const float iy = ry + 0.5f * (row_h - isz);
          if (!badge_icon(l, tx, iy, tx + isz, iy + isz, static_cast<BadgeIconKind>(r.badge_icon_kind), fa(kWhite))) {
            l.rect(tx, iy, tx + isz, iy + isz, fa(ui::rgba(255, 255, 255, 24)));
          }
          tx += isz + 10.0f * s;
        }

        const float right_zone_w = r.arrows ? 228.0f * s : 210.0f * s;
        const float label_max_w = std::max(100.0f * s, (lx0 + lw - right_zone_w) - tx);
        // Strictly fixed font family & size so moving selection never shifts or resizes rows.
        ui::TextStyle lts = style(ui::Font::kD, 14.0f * s, primary_fg);
        fit_text(r.label, lts, label_max_w, 9.5f * s);
        if (!r.subtitle.empty()) {
          l.text(tx, ry + 6.0f * s, r.label, lts);
          ui::TextStyle sts = style(ui::Font::kB, 10.5f * s, sub_fg, ui::Align::kLeft, true);
          fit_text(r.subtitle, sts, label_max_w + 24.0f * s, 8.0f * s);
          l.text(tx, ry + 25.0f * s, r.subtitle, sts);
        } else if (r.has_slider) {
          l.text(tx, ry + 6.0f * s, r.label, lts);
          opt_slider(l, tx, ry + 27.0f * s, lx0 + lw - 196.0f * s, ry + 36.0f * s, r.slider_norm, s,
                     fa(act ? kYellow : r.slider_color), act);
        } else {
          l.text(tx, ry + 0.5f * (row_h - lts.size), r.label, lts);
        }

        // Right value / authentic OPT_BAR_ARROW selector
        const float vx0 = lx0 + lw - 196.0f * s;
        const float vx1 = lx0 + lw - 16.0f * s;
        const float vcy = ry + 0.5f * row_h;
        float val_left_x = vx1 - 74.0f * s;
        if (r.arrows) {
          const uint32_t arr_col = fa(dark_ink_row ? kInk : (act ? kWhite : ui::rgba(190, 196, 208, 175)));
          opt_arrow(l, vx0 + 10.0f * s, vcy, 14.0f * s, -1, arr_col);
          opt_arrow(l, vx1 - 10.0f * s, vcy, 14.0f * s, +1, arr_col);
          ui::TextStyle vs = style(ui::Font::kB, 13.5f * s,
                                   fa(dark_ink_row ? kInk : (act ? kYellow : r.value_color)), ui::Align::kCenter);
          fit_text(r.value, vs, (vx1 - vx0) - 42.0f * s, 9.0f * s);
          l.text(0.5f * (vx0 + vx1), ry + 0.5f * (row_h - vs.size), r.value, vs);
          val_left_x = vx0 - 8.0f * s;
        } else if (!r.value.empty()) {
          ui::TextStyle vs = style(ui::Font::kE, 13.0f * s,
                                   fa(dark_ink_row ? kInk : (act ? kYellow : r.value_color)), ui::Align::kRight);
          fit_text(r.value, vs, 128.0f * s, 8.5f * s);
          const float vw = ui::List::measure(r.value, vs);
          l.text(vx1, ry + 0.5f * (row_h - vs.size), r.value, vs);
          val_left_x = vx1 - vw - 10.0f * s;
        }

        // Optional compact tag chip (e.g. "SAVE", "NATIVE", "100G") placed cleanly left of value!
        if (!r.badge_tag.empty()) {
          const float tag_w = std::max(44.0f * s, float(r.badge_tag.size()) * 7.5f * s + 12.0f * s);
          const float tag_x1 = std::min(lx0 + lw - 76.0f * s, val_left_x);
          const float tag_x0 = tag_x1 - tag_w;
          const float tag_y = ry + 0.5f * (row_h - 18.0f * s);
          l.rect(tag_x0, tag_y, tag_x1, tag_y + 18.0f * s, fa(r.badge_color));
          const bool dk = (r.badge_color == kAmber || r.badge_color == kYellow || r.badge_color == kGreen);
          l.text(0.5f * (tag_x0 + tag_x1), tag_y + 4.0f * s, r.badge_tag,
                 style(ui::Font::kE, 9.5f * s, fa(dk ? kInk : kWhite), ui::Align::kCenter));
        }
        ry += row_pitch;
      }

      // Authentic COMMON/SCROLLINDICATOR + SPECULAR/SPEC_SHEEN2
      scroll_indicator(l, lx0 + lw + 8.0f * s, rows_top, lx0 + lw + 16.0f * s, ry - 4.0f * s, first, max_vis, n, s);
    }

    if (!spec.notice.empty()) {
      const float ny = std::min(list_bot + 4.0f * s, h - 110.0f * s);
      brush(l, lx0, ny, lx0 + lw, ny + 26.0f * s, fa(kBrushBlack), false, ease);
      ui::TextStyle ns = style(ui::Font::kE, 12.0f * s, fa(kYellow), ui::Align::kCenter);
      fit_text(spec.notice, ns, lw - 32.0f * s, 8.5f * s);
      l.text(lx0 + 0.5f * lw, ny + 6.5f * s, spec.notice, ns);
    }

    // 3. Compact Right DIRTMASKS/PANEL01 Detail Card (tightly sized to content — ZERO empty charcoal void!)
    const float rx0 = 78.0f * s + lw + 28.0f * s + slide_r;
    const float rx1 = w - 68.0f * s + slide_r;
    const float dw = (rx1 - rx0) - 32.0f * s;
    float right_bottom_y = content_y;
    const bool has_detail = !spec.detail_title.empty() || !spec.detail_kvs.empty() || !spec.detail_paragraphs.empty();
    if (has_detail) {
      // Pre-wrap text to measure exact compact height so there is ZERO wasted empty void!
      ui::TextStyle p_ts = style(ui::Font::kB, 11.5f * s, fa(kWhite), ui::Align::kLeft, true);
      std::vector<std::string> wrapped_p;
      for (const auto& para : spec.detail_paragraphs) {
        auto lines = wrap_blade_text(para, p_ts, dw, 4);
        wrapped_p.insert(wrapped_p.end(), lines.begin(), lines.end());
      }
      ui::TextStyle f_ts = style(ui::Font::kB, 10.5f * s, fa(kOffWhite), ui::Align::kLeft, true);
      std::vector<std::string> wrapped_f;
      if (!spec.detail_footer.empty()) wrapped_f = wrap_blade_text(spec.detail_footer, f_ts, dw - 12.0f * s, 3);

      float card_h = 16.0f * s;
      if (!spec.detail_header.empty()) card_h += 30.0f * s;
      if (!spec.detail_title.empty()) card_h += 44.0f * s;
      card_h += float(wrapped_p.size()) * 17.0f * s + (wrapped_p.empty() ? 0.0f : 8.0f * s);
      card_h += float(spec.detail_kvs.size()) * 23.0f * s + (spec.detail_kvs.empty() ? 0.0f : 6.0f * s);
      if (spec.detail_progress >= 0.0f) card_h += 36.0f * s;
      if (!wrapped_f.empty()) card_h += float(wrapped_f.size()) * 15.0f * s + 18.0f * s;
      card_h = std::min(card_h, (h - 102.0f * s) - content_y);

      panel01(l, rx0, content_y, rx1, content_y + card_h, s, fa(kCharcoalDark), fa(kPink));
      right_bottom_y = content_y + card_h;
      float dy = content_y + 8.0f * s;

      if (!spec.detail_header.empty()) {
        pill(l, rx0 + 8.0f * s, dy, rx1 - 8.0f * s, dy + 24.0f * s, fa(ui::rgba(0xBF, 0xBF, 0xBF, 240)),
             PillKind::kMenu);
        l.text(rx0 + 18.0f * s, dy + 5.5f * s, spec.detail_header, style(ui::Font::kD, 12.0f * s, fa(kInk)));
        dy += 30.0f * s;
      }

      if (!spec.detail_title.empty()) {
        const uint32_t tbg = spec.detail_title_color != 0 ? spec.detail_title_color : kPink;
        const bool dk = (tbg == kAmber || tbg == kYellow || tbg == kGreen);
        brush(l, rx0 - 4.0f * s, dy, rx1 + 4.0f * s, dy + 34.0f * s, fa(tbg), false, ease);
        float dtx = rx0 + 16.0f * s;
        if (spec.has_detail_icon_uv) {
          l.rect(rx0 + 11.0f * s, dy + 2.0f * s, rx0 + 41.0f * s, dy + 32.0f * s, fa(ui::rgba(10, 12, 16, 210)));
          l.image(rx0 + 12.0f * s, dy + 3.0f * s, rx0 + 40.0f * s, dy + 31.0f * s, spec.detail_icon_uv, fa(kWhite));
          dtx = rx0 + 48.0f * s;
        } else if (spec.detail_icon_kind >= 0) {
          badge_icon(l, rx0 + 12.0f * s, dy + 3.0f * s, rx0 + 40.0f * s, dy + 31.0f * s,
                     static_cast<BadgeIconKind>(spec.detail_icon_kind), fa(kWhite));
          dtx = rx0 + 46.0f * s;
        }
        ui::TextStyle dt = style(ui::Font::kE, 16.0f * s, fa(dk ? kInk : kWhite));
        fit_text(spec.detail_title, dt, (rx1 - dtx) - 12.0f * s, 10.0f * s);
        l.text(dtx, dy + 8.0f * s, spec.detail_title, dt);
        dy += 42.0f * s;
      }

      for (const auto& ln : wrapped_p) {
        if (dy + 16.0f * s > content_y + card_h - 6.0f * s) break;
        l.text(rx0 + 16.0f * s, dy, ln, p_ts);
        dy += 17.0f * s;
      }
      if (!wrapped_p.empty()) dy += 6.0f * s;

      for (const auto& kv : spec.detail_kvs) {
        if (dy + 20.0f * s > content_y + card_h - 6.0f * s) break;
        l.text(rx0 + 16.0f * s, dy, kv.key, style(ui::Font::kB, 10.5f * s, fa(kDim)));
        ui::TextStyle vs = style(ui::Font::kE, 12.0f * s, fa(kv.value_color), ui::Align::kRight, true);
        fit_text(kv.value, vs, dw - 104.0f * s, 8.5f * s);
        l.text(rx1 - 16.0f * s, dy - 0.5f * s, kv.value, vs);
        dy += 22.0f * s;
      }

      if (spec.detail_progress >= 0.0f && dy + 30.0f * s <= content_y + card_h - 4.0f * s) {
        dy += 4.0f * s;
        if (!spec.detail_progress_label.empty()) {
          l.text(rx0 + 16.0f * s, dy, spec.detail_progress_label,
                 style(ui::Font::kE, 11.0f * s, fa(spec.detail_progress_color)));
        }
        opt_slider(l, rx0 + 16.0f * s, dy + 16.0f * s, rx1 - 16.0f * s, dy + 25.0f * s, spec.detail_progress, s,
                   fa(spec.detail_progress_color), true);
        dy += 32.0f * s;
      }

      if (!wrapped_f.empty() && dy + 16.0f * s <= content_y + card_h - 4.0f * s) {
        dy += 4.0f * s;
        const float fh = float(wrapped_f.size()) * 15.0f * s + 10.0f * s;
        l.rect(rx0 + 12.0f * s, dy, rx1 - 12.0f * s, std::min(content_y + card_h - 6.0f * s, dy + fh),
               fa(ui::rgba(255, 255, 255, 14)));
        float fy = dy + 5.0f * s;
        for (const auto& fl : wrapped_f) {
          if (fy + 13.0f * s > content_y + card_h - 6.0f * s) break;
          l.text(rx0 + 18.0f * s, fy, fl, f_ts);
          fy += 15.0f * s;
        }
      }
    }

    // 4. Language-independent visual Controller Layout Diagram via explicit ControllerDiagramParams
    //    (with gentle fallback ONLY when scene_hint == "H_CONTROLS" and controller_diagram.enabled is false).
    const bool show_ctrl_diagram =
        spec.controller_diagram.enabled || (!spec.controller_diagram.enabled && spec.scene_hint == "H_CONTROLS");
    if (show_ctrl_diagram && (rx1 - rx0) >= 220.0f * s) {
      const float diag_y0 = has_detail ? (right_bottom_y + 10.0f * s) : content_y;
      const float diag_y1 = std::min(h - 92.0f * s, diag_y0 + 206.0f * s);
      if (diag_y1 - diag_y0 >= 120.0f * s) {
        const auto& ds = diagram_sync();
        const int layout_id = spec.controller_diagram.enabled ? spec.controller_diagram.layout_id : ds.layout_id;
        const bool swap_hb =
            spec.controller_diagram.enabled ? spec.controller_diagram.swap_handbrake_clutch : ds.swap_hb_clutch;
        const std::string_view hi =
            spec.controller_diagram.enabled ? std::string_view(spec.controller_diagram.highlight_action) : std::string_view{};
        draw_controller_layout_diagram(l, rx0, diag_y0, rx1, diag_y1, s, layout_id, swap_hb, hi);
      }
    }
  }

  button_bar(l, spec.prompts, w, h, 56.0f);
}

}  // namespace carview::ui4
