// Shared private layout, palette, and text primitives for carview HUD modules.
#pragma once

#include "game_hud.h"
#include "hud_scene.h"
#include "ui4_chrome.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carview::hud_detail {

using ui::Align;
using ui::rgba;
using ui::TextStyle;

inline constexpr uint32_t kPanel = rgba(10, 12, 17, 200);
inline constexpr uint32_t kPanelLow = rgba(6, 7, 10, 225);
inline constexpr uint32_t kShadow = rgba(0, 0, 0, 110);
inline constexpr uint32_t kText = rgba(244, 245, 248);
inline constexpr uint32_t kDim = rgba(160, 166, 178);
inline constexpr uint32_t kAccent = rgba(255, 186, 20);
inline constexpr uint32_t kBlue = rgba(0x2C, 0x8C, 0xE8);
inline constexpr uint32_t kRed = rgba(236, 52, 44);
inline constexpr uint32_t kGreen = rgba(70, 220, 110);
inline constexpr uint32_t kInk = rgba(14, 14, 16);
inline constexpr float kDialA0 = -2.36f, kDialA1 = 2.36f;

inline constexpr uint32_t kPink = ui4::kPink;
inline constexpr uint32_t kPinkRow = rgba(0xFF, 0x03, 0x61);
inline constexpr uint32_t kBlack60 = rgba(0, 0, 0, 153);
inline constexpr uint32_t kBrush = ui4::kBrushBlack;
inline constexpr uint32_t kGear = rgba(0xBA, 0xB9, 0xB8);
inline constexpr uint32_t kDark = rgba(0x2D, 0x2D, 0x2D);
inline constexpr uint32_t kLight = rgba(0xD8, 0xD8, 0xD8, 235);
inline constexpr uint32_t kSoft = rgba(0, 0, 0, 120);
inline constexpr uint32_t kPillGrey = ui4::kPillGrey;

inline float clamp01(double v) { return float(std::clamp(v, 0.0, 1.0)); }

inline float ease_in_time(double t, double t0, double len) {
  const float k = clamp01((t - t0) / len);
  return 1 - (1 - k) * (1 - k) * (1 - k);
}

inline void panel(ui::List& l, float x0, float y0, float x1, float y1, float s, uint32_t color = kPanel) {
  l.shadow(x0, y0 + 3 * s, x1, y1 + 3 * s, kShadow, 10 * s, 12 * s);
  l.rect(x0, y0, x1, y1, color, 10 * s, ui::fade(color, 1.15));
}

inline void label(ui::List& l, float x, float y, const char* text, float s, uint32_t color = kAccent,
                  Align align = Align::kLeft) {
  l.text(x, y, text, TextStyle{.size = 8 * s, .weight = 0.12f, .tracking = 0.3f, .color = color, .align = align});
}

inline uint32_t band_colour(const std::string& name) {
  std::string n;
  for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
  const std::pair<const char*, uint32_t> named[] = {
      {"yellow", rgba(255, 214, 40)}, {"green", rgba(75, 220, 110)},  {"orange", rgba(255, 146, 36)},
      {"blue", rgba(80, 175, 255)},   {"purple", rgba(198, 120, 255)}, {"red", rgba(236, 52, 44)},
      {"gold", rgba(246, 206, 78)},   {"silver", rgba(205, 210, 220)}, {"black", rgba(120, 124, 134)},
      {"white", rgba(244, 244, 246)}, {"pink", rgba(255, 110, 185)}};
  for (const auto& [k, c] : named)
    if (n.find(k) != std::string::npos) return c;
  return kAccent;
}

inline uint32_t class_colour(const std::string& c) {
  if (c == "E") return rgba(124, 170, 82);
  if (c == "D") return rgba(56, 152, 226);
  if (c == "C") return rgba(232, 182, 42);
  if (c == "B") return rgba(236, 116, 38);
  if (c == "A") return rgba(218, 52, 52);
  if (c == "S") return rgba(146, 86, 214);
  return rgba(216, 44, 146);
}

struct Layout {
  float w, h, s;
  float L(float x) const { return x * s; }
  float R(float x) const { return w - (1280 - x) * s; }
  float T(float y) const { return y * s; }
  float B(float y) const { return h - (720 - y) * s; }
  float C(float x) const { return 0.5f * w + (x - 640) * s; }
};

inline constexpr float kTilt = -0.052f;
struct Turn {
  ui::List& l;
  Turn(ui::List& list, float cx, float cy, float a = kTilt) : l(list) { l.turn(cx, cy, a); }
  ~Turn() { l.unturn(); }
};

inline void brush(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour) {
  ui4::brush(l, x0, y0, x1, y1, colour, false, 1.0f);
}

inline void pill(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour, bool second = false,
                 const char* texture = nullptr, float /*texels*/ = 1024) {
  if (!texture) {
    ui4::pill(l, x0, y0, x1, y1, colour, second ? ui4::PillKind::kHud02 : ui4::PillKind::kHud01);
    return;
  }
  std::array<float, 4> uv;
  if (!ui_texture(texture, uv)) {
    l.rect(x0, y0, x1, y1, colour == kPillGrey ? kBlack60 : colour);
    return;
  }
  const float texel = (y1 - y0) / 26.0f;
  const float ty0 = y0 - 3 * texel, ty1 = y1 + 3 * texel;
  l.image_masked(x0, ty0, x1, ty1, uv, uv, colour, {1.0f, 1.0f, 0.0f, 0.0f}, ui::kMaskBorderTransparent);
}

inline uint16_t table_hash(std::string_view v) {
  uint16_t h = 0xFFFF;
  for (char c : v) {
    h ^= uint16_t(std::tolower(static_cast<unsigned char>(c)));
    h = uint16_t((h << 7) | (h >> 9));
  }
  return h;
}

inline std::string str(std::string_view table, std::string_view id, const char* fallback) {
  const uint32_t key = uint32_t(table_hash(table)) << 16 | table_hash(id);
  const std::string ref = "_&" + std::to_string(key);
  std::string t = game_text(ref);
  if (t.empty() || t == ref) return fallback;
  return t;
}

inline TextStyle font(ui::Font f, float size, uint32_t color, Align align = Align::kLeft) {
  return TextStyle{.size = size, .weight = 0.11f, .color = color, .align = align, .font = f};
}

inline float shadowed(ui::List& l, float x, float y, std::string_view t, TextStyle ts, float s) {
  TextStyle sh = ts;
  sh.color = ui::fade(kSoft, float((ts.color >> 24) & 0xFF) / 255.0f);
  l.text(x + 1.5f * s, y + 1.5f * s, t, sh);
  return l.text(x, y, t, ts);
}

inline void fit(std::string_view t, TextStyle& ts, float room, float floor) {
  while (ts.size > floor && ui::List::measure(t, ts) > room) ts.size *= 0.96f;
}

inline std::string clock_ms(double t) {
  if (t < 0) t = 0;
  const int64_t ms = int64_t(std::llround(t * 1000));
  char b[32];
  std::snprintf(b, sizeof b, "%02lld:%02lld.%03lld", (long long)(ms / 60000), (long long)(ms / 1000 % 60),
                (long long)(ms % 1000));
  return b;
}

inline std::string gap_ms(double t) {
  char b[32];
  std::snprintf(b, sizeof b, "+%.3f", std::max(0.0, t));
  return b;
}

inline void help_item(ui::List& l, const Layout& g, float x0, float x1, float y0, float y1, const char* face,
                      uint32_t colour, const std::string& text, bool bottom = true) {
  auto Y = [&](float y) { return bottom ? g.B(y) : g.T(y); };
  const float s = g.s;
  std::array<float, 4> pin;
  if (ui_texture("DIRTMASKS/PILL01", pin)) {
    pill(l, g.L(x0), Y(y0), g.L(x1), Y(y1) - 10 * s, kPillGrey, false, "DIRTMASKS/PILL01", 512);
    if (ui_texture("DIRTMASKS/PILLPIN", pin))
      l.image(g.L(x0), Y(y1) - 14 * s, g.L(x0) + 16 * s, Y(y1) + 2 * s, pin, kPillGrey);
  } else {
    l.rect(g.L(x0), Y(y0), g.L(x1), Y(y1), kBlack60);
  }
  const float cy = 0.5f * (Y(y0) + Y(y1) - 10 * s), cx = g.L(x0) + 16 * s;
  (void)colour;
  ui4::button_glyph(l, cx, cy, 10.5f * s, face);
  l.text(cx + 16 * s, cy - 7 * s, text, font(ui::Font::kB, 14 * s, kText));
}

// Race HUD builders (game_hud_race.cpp)
void event_card(ui::List& l, const RaceHud& r, const Layout& g);
// `hud`: the original's HUD options (lap time and position, wrong way).
void race_hud(ui::List& l, const RaceHud& r, const Layout& g, const HudToggles& hud = {});
void results(ui::List& l, const RaceHud& r, const DriveHud& st, const Layout& g);
void opponent_name_tags(ui::List& l, const std::vector<NameTagHud>& tags, const Layout& g);
void showcase_hud(ui::List& l, const RaceHud& r, const Layout& g, bool metric_units);

// Free-roam HUD builders (game_hud_freeroam.cpp)
void world_marker_hud(ui::List& l, const WorldMarkerHud& wm, const Layout& g, bool metric_units);
void skill_chain_hud(ui::List& l, const SkillChainHud& sk, const Layout& g, double time);
void ceremony_overlay(ui::List& l, const CeremonyHud& c, const Layout& g);
void map_overlay_hud(ui::List& l, const MapOverlayHud& mo, const Layout& g, bool has_player_bar, bool metric_units);
void gps_turn_hud(ui::List& l, const GpsHud& gps, const Layout& g, bool metric_units);
void pr_stunt_hud(ui::List& l, const PrStuntHud& ps, const Layout& g);
float speed_activity_hud(ui::List& l, const freeroam::SpeedActivityHud& sh, const Layout& g, bool metric_units,
                         float lift_canvas);
float h2h_hud(ui::List& l, const freeroam::HeadToHeadHud& hh, const Layout& g, bool metric_units, float lift_canvas);
bool progression_hud(ui::List& l, const DriveHud& st, const Layout& g, float lift_canvas);

// Menu, title & career HUD builders (game_hud_menus.cpp)
void draw_career_header(ui::List& l, const DriveHud& st, const Layout& g);
void menu_panel(ui::List& l, const DriveHud& st, const Layout& g);
void title_screen_hud(ui::List& l, const TitleScreenHud& ts, const Layout& g, double time);
void career_board(ui::List& l, const CareerBoard& c, float w, float h, float s);

}  // namespace carview::hud_detail
