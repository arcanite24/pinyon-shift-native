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

std::string upper_copy(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
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
    const float px = 0.045f * h;
    const float py = 0.11f * h;
    std::array<float, 4> sm_uv = smudge;
    std::array<float, 4> cr_uv = crisp;
    if (f < 0.999f) {
      sm_uv[2] = smudge[0] + (smudge[2] - smudge[0]) * f;
      cr_uv[2] = crisp[0] + (crisp[2] - crisp[0]) * f;
    }
    l.image(x0 - px, y0 - py, wx1 + px, y1 + py, sm_uv, colour);
    l.image(x0 - px, y0 - py, wx1 + px, y1 + py, cr_uv, colour);
  } else {
    // Clean sharp-edged fallback (never rounded glass).
    l.rect(x0, y0, wx1, y1, colour);
  }
}

void pill(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour, PillKind kind, bool with_pin) {
  if (x1 <= x0 || y1 <= y0) return;
  const char* tex_name = "DIRTMASKS/PILL01";
  float texels = 512.0f;
  switch (kind) {
    case PillKind::kHud01: tex_name = "DIRTMASKS/HUD/HUDPILL01"; texels = 1024.0f; break;
    case PillKind::kHud02: tex_name = "DIRTMASKS/HUD/HUDPILL02"; texels = 1024.0f; break;
    case PillKind::kMapSmall: tex_name = "DIRTMASKS/MAPPILLSMALL"; texels = 256.0f; break;
    case PillKind::kMenu:
    default: tex_name = "DIRTMASKS/PILL01"; texels = 512.0f; break;
  }
  std::array<float, 4> uv{};
  if (!texture(tex_name, uv)) {
    l.rect(x0, y0, x1, y1, colour);
    return;
  }
  const float h = y1 - y0;
  const float texel = h / 26.0f;
  const float ty0 = y0 - 2.5f * texel;
  const float ty1 = y1 + 2.5f * texel;
  const float cap = std::min(12.0f * texel, 0.45f * (x1 - x0));
  const float du = (uv[2] - uv[0]) / texels;
  const float body = (x1 - cap - x0) / std::max(0.1f, texel);
  l.image(x0, ty0, x1 - cap, ty1, {uv[0], uv[1], uv[0] + du * std::min(body, texels - 12.0f), uv[3]}, colour);
  l.image(x1 - cap, ty0, x1, ty1, {uv[2] - du * 12.0f, uv[1], uv[2], uv[3]}, colour);
  if (with_pin) {
    std::array<float, 4> pin{};
    if (texture("DIRTMASKS/PILLPIN", pin)) {
      const float ps = 14.0f * texel;
      l.image(x0, y1 - 3.0f * texel, x0 + ps, y1 + ps - 3.0f * texel, pin, colour);
    }
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

void button_glyph(ui::List& l, float cx, float cy, float r, const char* button, float alpha) {
  const std::string b = button ? std::string(button) : std::string("A");
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
  const bool multi = b.size() > 1;
  if (multi) {
    const float hw = std::max(r * 1.3f, float(b.size()) * r * 0.45f + 4.0f);
    l.rect(cx - hw, cy - r * 0.85f, cx + hw, cy + r * 0.85f, ui::fade(ui::rgba(225, 228, 236), alpha));
    ui::TextStyle ts = style(ui::Font::kB, r * 0.95f, ui::fade(kInk, alpha), ui::Align::kCenter);
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
    if (act) {
      brush(l, cx, y0 - 2.0f * s, cx + tw, y0 + h + 2.0f * s, active_colour);
    } else {
      pill(l, cx, y0, cx + tw, y0 + h, kPillGrey, PillKind::kMenu);
    }
    ui::TextStyle ts = style(act ? ui::Font::kE : ui::Font::kB, (act ? 13.5f : 12.0f) * s, act ? kWhite : kDim,
                             ui::Align::kCenter);
    fit_text(tabs[size_t(i)], ts, tw - 10.0f * s, 8.0f * s);
    l.text(cx + 0.5f * tw, y0 + 0.5f * (h - ts.size), tabs[size_t(i)], ts);
    cx += tw + gap;
  }
  if (show_bumpers) {
    button_glyph(l, std::min(cx + 16.0f * s, max_x - 16.0f * s), y0 + 0.5f * h, 11.0f * s, "RB");
  }
}

void button_bar(ui::List& l, const std::vector<Prompt>& items, float screen_w, float screen_h, float left_x_canvas) {
  if (items.empty()) return;
  const float s = screen_h / 720.0f;
  float base_y = screen_h - 66.0f * s;
  if (const fh1::UiScene* bar = scene("BUTTON_BAR_SCENE")) {
    for (const auto& node : bar->nodes) {
      if (node.object_index >= 0 && bar->objects[size_t(node.object_index)].name == "BUTTON_BAR") {
        const float cy = 360.0f - node.y;
        if (cy > 420.0f && cy < 700.0f) base_y = screen_h - (720.0f - cy) * s;
        break;
      }
    }
  }
  const float h = 28.0f * s;
  float cx = left_x_canvas * s;
  for (const auto& item : items) {
    if (item.label.empty()) continue;
    ui::TextStyle ts = style(ui::Font::kB, 14.0f * s, kWhite, ui::Align::kLeft, false);
    const float tw = ui::List::measure(item.label, ts);
    const std::string btn = item.button ? std::string(item.button) : std::string("A");
    const bool wide_btn = (btn == "LB" || btn == "RB" || btn == "LT" || btn == "RT" || btn.size() > 2);
    const float btn_pad = wide_btn ? 42.0f * s : 32.0f * s;
    const float pw = tw + btn_pad + 18.0f * s;
    if (cx + pw > screen_w - 40.0f * s) break;

    pill(l, cx, base_y, cx + pw, base_y + h, kPillGrey, PillKind::kMenu, true);
    const float gy = base_y + 0.5f * h;
    const float gx = cx + (wide_btn ? 20.0f : 16.0f) * s;
    button_glyph(l, gx, gy, 10.5f * s, btn.c_str());
    l.text(cx + btn_pad + 4.0f * s, gy - 0.5f * ts.size, item.label, ts);
    cx += pw + 8.0f * s;
  }
}

void class_badge(ui::List& l, float x, float y, float h, std::string_view class_name, int pi, int class_id_fallback) {
  const std::string cname = class_name.empty() ? std::string("?") : upper_copy(class_name);
  char pibuf[16];
  std::snprintf(pibuf, sizeof pibuf, "%d", std::max(0, pi));

  std::array<float, 4> uv{};
  if (cname != "?" && texture("PICLASS/CLASS_" + cname, uv)) {
    // PICLASS/CLASS_* artwork has ~3.8:1 aspect ratio (letter square + white PI box).
    const float w = h * 3.45f;
    l.image(x, y, x + w, y + h, uv);
    ui::TextStyle ps = style(ui::Font::kB, h * 0.62f, kInk, ui::Align::kCenter);
    fit_text(pibuf, ps, w * 0.42f, h * 0.36f);
    l.text(x + w * 0.66f, y + 0.5f * (h - ps.size), pibuf, ps);
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

  // Row 3: Optional popularity rank / discount tag + Off-white distressed Credits strip!
  if (d.credits >= 0) {
    if (d.discount_percent > 0) {
      char db[32];
      std::snprintf(db, sizeof db, "%d%% OFF", d.discount_percent);
      brush(l, R(974), T(92), R(1078), T(116), kPink);
      l.text(R(1026), T(97), db, style(ui::Font::kE, 13.0f * s, kWhite, ui::Align::kCenter));
    } else if (d.popularity_rank >= 1 && d.popularity_rank <= 250) {
      pill(l, R(972), T(92), R(1080), T(116), kPillGrey, PillKind::kMenu);
      char rk[32];
      std::snprintf(rk, sizeof rk, "RANK %d%s", d.popularity_rank, ordinal_suffix(d.popularity_rank));
      l.text(R(1026), T(98), rk, style(ui::Font::kB, 12.5f * s, kYellow, ui::Align::kCenter));
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

  // Track background (UPGRADES/UPG_STATBAR or crisp charcoal track with 10-segment dividers).
  l.rect(bx0, by, bx1, by + bh, ui::rgba(255, 255, 255, 28));

  const float c0 = float(std::clamp(cur_val / 10.0, 0.0, 1.0));
  const float c1 = float(std::clamp(preview_val / 10.0, 0.0, 1.0));
  const float lo = show_diff ? std::min(c0, c1) : c0;

  std::array<float, 4> bar_uv{};
  const bool has_tex = texture("UPGRADES/UPG_STATBAR", bar_uv);
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
      {"mercedes", "MercedesBenz"}, {"mercedesbenz", "MercedesBenz"}, {"mini", "MINI"},
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
    l.image(x0, y0, x1, y1, uv, tint);
    return true;
  }
  return false;
}

void subtle_vignette(ui::List& l, float w, float h, float strength) {
  const float k = clamp01(strength);
  if (k <= 0.01f) return;
  const uint8_t top_a = uint8_t(std::lround(75.0f * k));
  const uint8_t bot_a = uint8_t(std::lround(125.0f * k));
  l.rect(0, 0, w, h * 0.22f, ui::rgba(4, 6, 10, top_a), 0.0f, ui::rgba(4, 6, 10, 0));
  l.rect(0, h * 0.74f, w, h, ui::rgba(4, 6, 10, 0), 0.0f, ui::rgba(4, 6, 10, bot_a));
}

}  // namespace carview::ui4
