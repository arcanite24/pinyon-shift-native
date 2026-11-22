#include "ui_widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "game_hud.h"

namespace carview {
namespace {

constexpr uint32_t kPink = ui::rgba(0xF5, 0x04, 0x5C);
constexpr uint32_t kBlack = ui::rgba(14, 16, 22, 220);
constexpr uint32_t kPillGrey = ui::rgba(33, 33, 33);
constexpr uint32_t kWhite = ui::rgba(246, 247, 250);
constexpr uint32_t kDim = ui::rgba(176, 182, 194);
constexpr uint32_t kInk = ui::rgba(18, 20, 24);

void draw_brush_strip(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour) {
  std::array<float, 4> smudge{}, crisp{};
  if (ui::atlas_find("tape.smudge", smudge) && ui::atlas_find("tape.crisp", crisp)) {
    const float px = 0.03f * (y1 - y0), py = 0.1f * (y1 - y0);
    l.image(x0 - px, y0 - py, x1 + px, y1 + py, smudge, colour);
    l.image(x0 - px, y0 - py, x1 + px, y1 + py, crisp, colour);
  } else {
    l.rect(x0, y0, x1, y1, colour, 3.0f);
  }
}

uint32_t button_colour(const char* face) {
  if (!face) return ui::rgba(70, 220, 110);
  switch (face[0]) {
    case 'A': return ui::rgba(70, 220, 110);
    case 'B': return ui::rgba(236, 52, 44);
    case 'X': return ui::rgba(44, 140, 232);
    case 'Y': return ui::rgba(255, 196, 36);
    default: return ui::rgba(220, 224, 232);
  }
}

std::string radio_logo_texture_path(const std::string& station_name, bool off) {
  if (off || station_name == "Radio4_Silent") return "HUD/RADIO/RS_OFF";
  if (station_name == "Radio1") return "HUD/RADIO/RS_BASSARENA";
  if (station_name == "Radio2") return "HUD/RADIO/RS_PULSE";
  if (station_name == "Radio3") return "HUD/RADIO/RS_2ELEVEN";
  return "HUD/RADIO/RS_BASSARENA";
}

}  // namespace

// ------------------------------------------------ GPU Vector-Font Radio Popup

void draw_radio_popup_gpu(ui::List& l, const RadioPopupGpuState& st, float screen_w, float screen_h) {
  const float a = std::clamp(st.alpha, 0.0f, 1.0f);
  if (a <= 0.005f) return;
  const float s = screen_h / 720.0f;
  // Positioned right-aligned above the tachometer (SPEC-RADIO HUD section: x 1075..1210, y 368..470).
  const float right_x = screen_w - (1280.0f - 1210.0f) * s;
  const float top_y = screen_h - (720.0f - 368.0f) * s;

  std::array<float, 4> logo_uv{};
  const std::string tex = radio_logo_texture_path(st.station_name, st.off);
  const bool have_logo = ui_texture(tex, logo_uv);
  const float logo_h = 44.0f * s;
  const float logo_w = 135.0f * s;

  const std::string title = st.off ? (st.station_label.empty() ? std::string("RADIO OFF") : st.station_label)
                                   : st.title;
  const std::string artist = st.off ? std::string{} : st.artist;

  ui::TextStyle title_ts;
  title_ts.font = ui::Font::kE;
  title_ts.size = 18.0f * s;
  title_ts.color = ui::fade(kWhite, a);
  title_ts.align = ui::Align::kRight;
  title_ts.keep_case = true;

  ui::TextStyle artist_ts;
  artist_ts.font = ui::Font::kB;
  artist_ts.size = 14.0f * s;
  artist_ts.color = ui::fade(kWhite, a);
  artist_ts.align = ui::Align::kRight;
  artist_ts.keep_case = true;

  const float tw = ui::List::measure(title, title_ts);
  const float aw = artist.empty() ? 0.0f : ui::List::measure(artist, artist_ts);
  const float panel_w = std::max({logo_w + 24.0f * s, tw + 28.0f * s, aw + 32.0f * s, 210.0f * s});
  const float panel_h = (artist.empty() ? 86.0f : 114.0f) * s;
  const float x0 = right_x - panel_w;

  // Translucent Horizon backing card with pink right edge accent.
  l.rect(x0, top_y - 6.0f * s, right_x + 6.0f * s, top_y + panel_h, ui::fade(ui::rgba(10, 12, 18, 195), a), 4.0f * s);
  l.rect(right_x + 2.0f * s, top_y - 6.0f * s, right_x + 6.0f * s, top_y + panel_h, ui::fade(kPink, a));

  if (have_logo) {
    l.image(right_x - logo_w, top_y, right_x, top_y + logo_h, logo_uv, ui::fade(ui::rgba(255, 255, 255), a));
  } else {
    ui::TextStyle st_ts;
    st_ts.font = ui::Font::kE;
    st_ts.size = 15.0f * s;
    st_ts.color = ui::fade(kPink, a);
    st_ts.align = ui::Align::kRight;
    const std::string sl = !st.station_label.empty() ? st.station_label : st.station_name;
    l.text(right_x - 8.0f * s, top_y + 12.0f * s, sl, st_ts);
  }

  // Title line (white, right-aligned).
  const float title_y = top_y + logo_h + 6.0f * s;
  l.text(right_x - 8.0f * s, title_y, title, title_ts);

  // Artist line on a Horizon pink brush strip.
  if (!artist.empty()) {
    const float ay = title_y + 26.0f * s;
    const float strip_w = aw + 20.0f * s;
    draw_brush_strip(l, right_x - strip_w - 2.0f * s, ay - 4.0f * s, right_x, ay + 20.0f * s, ui::fade(kPink, a));
    l.text(right_x - 8.0f * s, ay + 1.0f * s, artist, artist_ts);
  }
}

// ------------------------------------------------ UI4 Scene Parity Renderers

void draw_button_bar_ui4(ui::List& l, const std::vector<ButtonBarItem>& items, float screen_w, float screen_h) {
  if (items.empty()) return;
  const float s = screen_h / 720.0f;
  // Query BUTTON_BAR_SCENE if present on disc for authentic vertical anchor.
  float base_y = screen_h - 66.0f * s;
  if (const fh1::UiScene* bar = ui_scene("BUTTON_BAR_SCENE")) {
    SceneDraw sc(bar);
    if (const int root = sc.find("BUTTON_BAR"); root >= 0) {
      const auto pos = sc.at(root);
      if (pos[1] > 400.0f && pos[1] < 705.0f) base_y = screen_h - (720.0f - pos[1]) * s;
    }
  }
  float cursor_x = 70.0f * s;
  const float h = 32.0f * s;
  (void)screen_w;
  for (const auto& item : items) {
    ui::TextStyle ts;
    ts.font = ui::Font::kB;
    ts.size = 14.0f * s;
    ts.color = kWhite;
    ts.keep_case = true;
    const float tw = ui::List::measure(item.label, ts);
    const float pw = tw + 48.0f * s;
    std::array<float, 4> uv{};
    if (ui_texture("DIRTMASKS/PILL01", uv)) {
      l.image(cursor_x, base_y, cursor_x + pw, base_y + h, uv, kPillGrey);
    } else {
      l.rect(cursor_x, base_y, cursor_x + pw, base_y + h, ui::rgba(16, 18, 24, 210), 4.0f * s);
    }
    const float cx = cursor_x + 18.0f * s;
    const float cy = base_y + 0.5f * h;
    if (ui_texture(std::string("CONTROLLER/") + item.button + "_BUTTON", uv)) {
      l.image(cx - 11.0f * s, cy - 11.0f * s, cx + 11.0f * s, cy + 11.0f * s, uv);
    } else {
      l.circle(cx, cy, 10.0f * s, button_colour(item.button));
      ui::TextStyle bt;
      bt.font = ui::Font::kB;
      bt.size = 11.0f * s;
      bt.color = kInk;
      bt.align = ui::Align::kCenter;
      l.text(cx, cy - 5.5f * s, item.button, bt);
    }
    l.text(cx + 18.0f * s, cy - 7.0f * s, item.label, ts);
    cursor_x += pw + 10.0f * s;
  }
}

void draw_pause_menu_ui4(ui::List& l, const PauseMenuUi4State& st, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  // Subtle full-screen dark scrim so menu brushes pop against Colorado.
  l.rect(0, 0, screen_w, screen_h, ui::rgba(6, 8, 14, 110));

  // Query 925_PAUSE_MENU scene + SUPER_STACKER reflow (50 px pitch per SPEC-UI-SCREENS).
  float list_x = 147.0f * s;
  float list_y0 = 128.0f * s;
  float pitch = 50.0f * s;
  float slant_rad = 0.0f;
  if (const fh1::UiScene* sc_raw = ui_scene("925_PAUSE_MENU")) {
    SceneDraw sc(sc_raw, true);
    if (const int b0 = sc.find("Button0"); b0 >= 0) {
      const auto p = sc.at(b0);
      if (p[0] > 40.0f && p[0] < 400.0f) list_x = (p[0] - 40.0f) * s;
      if (p[1] > 60.0f && p[1] < 240.0f) list_y0 = (p[1] - 24.0f) * s;
      slant_rad = -sc.angle(b0);
    }
  }
  const int visible_count = int(st.items.size());
  // Clamp pitch if menu has many rows so it never overlaps the bottom help bar.
  const float max_h = screen_h - list_y0 - 95.0f * s;
  if (visible_count > 1 && pitch * float(visible_count) > max_h) {
    pitch = std::max(32.0f * s, max_h / float(visible_count));
  }
  const auto ys = fh1::reflow_stacker(list_y0, pitch, visible_count);

  if (std::fabs(slant_rad) > 1e-4f) l.turn(list_x + 150.0f * s, list_y0 + 180.0f * s, slant_rad);
  for (int i = 0; i < visible_count; ++i) {
    const bool sel = (i == st.selected);
    ui::TextStyle ts;
    ts.font = ui::Font::kE;
    ts.size = (sel ? 34.0f : 28.0f) * s * (pitch / (50.0f * s));
    ts.color = kWhite;
    ts.keep_case = true;
    const float tw = ui::List::measure(st.items[size_t(i)], ts);
    const float bw = std::max(240.0f * s, tw + 74.0f * s);
    const float bh = pitch * (sel ? 0.96f : 0.84f);
    const float ry = ys[size_t(i)];
    const uint32_t bg = sel ? ui::fade(kPink, 0.85f + 0.15f * st.selection_anim) : ui::rgba(12, 14, 18, 225);
    draw_brush_strip(l, list_x, ry, list_x + bw, ry + bh, bg);
    l.text(list_x + 36.0f * s, ry + 0.5f * (bh - ts.size), st.items[size_t(i)], ts);
  }
  if (std::fabs(slant_rad) > 1e-4f) l.unturn();

  // Top-right wristband / car / credits strip (SPEC-UI-LAYOUTS d).
  const float rx = screen_w - 64.0f * s;
  const float ry0 = 36.0f * s;
  if (!st.wristband_label.empty()) {
    l.rect(rx - 385.0f * s, ry0, rx - 165.0f * s, ry0 + 26.0f * s, ui::rgba(14, 16, 22, 200));
    ui::TextStyle gts{.size = 13.0f * s, .color = st.wristband_colour, .align = ui::Align::kRight, .font = ui::Font::kB, .keep_case = true};
    l.text(rx - 174.0f * s, ry0 + 6.0f * s, st.wristband_goal, gts);

    l.rect(rx - 160.0f * s, ry0, rx, ry0 + 26.0f * s, st.wristband_colour);
    ui::TextStyle wts{.size = 14.0f * s, .color = kWhite, .align = ui::Align::kCenter, .font = ui::Font::kE, .keep_case = true};
    l.text(rx - 80.0f * s, ry0 + 6.0f * s, st.wristband_label, wts);
  }
  if (!st.car_name.empty()) {
    const float cy = ry0 + 30.0f * s;
    l.rect(rx - 300.0f * s, cy, rx, cy + 26.0f * s, ui::rgba(14, 16, 22, 205));
    ui::TextStyle cts{.size = 15.0f * s, .color = kWhite, .align = ui::Align::kRight, .font = ui::Font::kB, .keep_case = true};
    l.text(rx - 68.0f * s, cy + 5.0f * s, st.car_name, cts);
    if (!st.car_class.empty()) {
      l.rect(rx - 60.0f * s, cy + 3.0f * s, rx - 40.0f * s, cy + 23.0f * s, ui::rgba(255, 146, 3));
      ui::TextStyle cl{.size = 13.0f * s, .color = kWhite, .align = ui::Align::kCenter, .font = ui::Font::kE};
      l.text(rx - 50.0f * s, cy + 6.0f * s, st.car_class, cl);
      l.rect(rx - 40.0f * s, cy + 3.0f * s, rx - 4.0f * s, cy + 23.0f * s, ui::rgba(240, 240, 244));
      char pib[16];
      std::snprintf(pib, sizeof pib, "%d", st.car_pi);
      ui::TextStyle pi{.size = 13.0f * s, .color = kInk, .align = ui::Align::kCenter, .font = ui::Font::kB};
      l.text(rx - 22.0f * s, cy + 6.0f * s, pib, pi);
    }
  }
  if (!st.credits_text.empty()) {
    const float cry = ry0 + 60.0f * s;
    l.rect(rx - 140.0f * s, cry, rx, cry + 24.0f * s, ui::rgba(220, 222, 226, 235));
    ui::TextStyle crs{.size = 14.0f * s, .color = kInk, .align = ui::Align::kRight, .font = ui::Font::kB, .keep_case = true};
    l.text(rx - 10.0f * s, cry + 5.0f * s, st.credits_text, crs);
  }

  draw_button_bar_ui4(l, st.help_bar, screen_w, screen_h);
}

void draw_map_chrome_ui4(ui::List& l, const MapChromeUi4State& st, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  // Query H_MAP scene presence for authentic Horizon header brush & roads pill.
  (void)ui_scene("H_MAP");
  draw_brush_strip(l, 54.0f * s, 28.0f * s, 390.0f * s, 72.0f * s, kBlack);
  ui::TextStyle fts{.size = 24.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
  l.text(74.0f * s, 38.0f * s, st.filter_label.empty() ? "COLORADO MAP" : st.filter_label, fts);

  if (!st.roads_label.empty()) {
    const float rx = screen_w - 64.0f * s;
    l.rect(rx - 290.0f * s, 32.0f * s, rx, 66.0f * s, ui::rgba(14, 16, 22, 215), 4.0f * s);
    l.rect(rx - 290.0f * s, 32.0f * s, rx - 284.0f * s, 66.0f * s, kPink);
    ui::TextStyle rts{.size = 14.0f * s, .color = kWhite, .align = ui::Align::kRight, .font = ui::Font::kB, .keep_case = true};
    l.text(rx - 14.0f * s, 42.0f * s, st.roads_label, rts);
  }
  if (!st.hovered_title.empty()) {
    const float rx = screen_w - 64.0f * s;
    const float cy = screen_h - 150.0f * s;
    l.rect(rx - 340.0f * s, cy, rx, cy + 72.0f * s, ui::rgba(12, 14, 20, 220), 4.0f * s);
    l.rect(rx - 340.0f * s, cy, rx - 334.0f * s, cy + 72.0f * s, kPink);
    ui::TextStyle ht{.size = 18.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
    ui::TextStyle hs{.size = 13.0f * s, .color = kDim, .font = ui::Font::kB, .keep_case = true};
    l.text(rx - 324.0f * s, cy + 12.0f * s, st.hovered_title, ht);
    l.text(rx - 324.0f * s, cy + 40.0f * s, st.hovered_subtitle, hs);
  }
  draw_button_bar_ui4(l, st.help_bar, screen_w, screen_h);
}

void draw_pre_post_race_ui4(ui::List& l, const PrePostRaceUi4State& st, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  (void)ui_scene("PRE_AND_POST_RACE");
  if (st.is_post_race) {
    draw_brush_strip(l, 75.0f * s, 165.0f * s, 395.0f * s, 232.0f * s, kBlack);
    ui::TextStyle ft{.size = 42.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
    l.text(92.0f * s, 178.0f * s, st.event_type.empty() ? "FINISHED" : st.event_type, ft);

    draw_brush_strip(l, 0.0f, 214.0f * s, 740.0f * s, 336.0f * s, kPink);
    ui::TextStyle pt{.size = 72.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
    l.text(92.0f * s, 238.0f * s, st.place_text, pt);
  } else {
    // Event title card bottom-left (SPEC-UI-LAYOUTS b1).
    draw_brush_strip(l, 75.0f * s, screen_h - 185.0f * s, 450.0f * s, screen_h - 130.0f * s, kBlack);
    ui::TextStyle et{.size = 30.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
    l.text(96.0f * s, screen_h - 173.0f * s, st.event_type, et);

    draw_brush_strip(l, 0.0f, screen_h - 140.0f * s, 920.0f * s, screen_h - 74.0f * s, kPink);
    ui::TextStyle en{.size = 40.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
    l.text(96.0f * s, screen_h - 127.0f * s, st.event_name, en);

    const auto ys = fh1::reflow_stacker(175.0f * s, 50.0f * s, int(st.menu_items.size()));
    for (size_t i = 0; i < st.menu_items.size(); ++i) {
      const bool sel = int(i) == st.selected;
      ui::TextStyle ts{.size = (sel ? 36.0f : 30.0f) * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
      const float bw = std::max(280.0f * s, ui::List::measure(st.menu_items[i], ts) + 74.0f * s);
      draw_brush_strip(l, 147.0f * s, ys[i], 147.0f * s + bw, ys[i] + 46.0f * s, sel ? kPink : kBlack);
      l.text(184.0f * s, ys[i] + 6.0f * s, st.menu_items[i], ts);
    }
  }
  draw_button_bar_ui4(l, st.help_bar, screen_w, screen_h);
}

void draw_street_race_hub_ui4(ui::List& l, const StreetRaceHubUi4State& st, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  (void)ui_scene("H_STREET_RACE_HUB");
  l.rect(0, 0, screen_w, screen_h, ui::rgba(8, 10, 16, 155));

  draw_brush_strip(l, 70.0f * s, 56.0f * s, 620.0f * s, 118.0f * s, kPink);
  ui::TextStyle ht{.size = 34.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
  l.text(92.0f * s, 70.0f * s, st.hub_name.empty() ? "STREET RACE HUB" : st.hub_name, ht);
  if (!st.hub_subtitle.empty()) {
    ui::TextStyle hs{.size = 15.0f * s, .color = kDim, .font = ui::Font::kB, .keep_case = true};
    l.text(92.0f * s, 128.0f * s, st.hub_subtitle, hs);
  }

  const auto ys = fh1::reflow_stacker(168.0f * s, 56.0f * s, int(st.races.size()));
  for (size_t i = 0; i < st.races.size(); ++i) {
    const auto& r = st.races[i];
    const bool sel = int(i) == st.selected;
    draw_brush_strip(l, 90.0f * s, ys[i], screen_w - 140.0f * s, ys[i] + 48.0f * s, sel ? kPink : kBlack);
    ui::TextStyle rt{.size = 20.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
    l.text(114.0f * s, ys[i] + 13.0f * s, r.name, rt);
    ui::TextStyle st_ts{.size = 15.0f * s, .color = sel ? kWhite : kDim, .align = ui::Align::kRight, .font = ui::Font::kB, .keep_case = true};
    l.text(screen_w - 165.0f * s, ys[i] + 15.0f * s,
           r.car_class + "  •  " + r.stake + (r.completed ? "  [WON]" : ""), st_ts);
  }
  draw_button_bar_ui4(l, st.help_bar, screen_w, screen_h);
}

// ------------------------------------------------ Animated Loading Screen

void LoadingScreen::start(std::string destination_title, std::string subtitle) {
  active_ = true;
  title_ = std::move(destination_title);
  subtitle_ = std::move(subtitle);
  elapsed_ = 0.0;
  tip_timer_ = 0.0;
  progress_ = 0.0f;
  if (tips_.empty()) {
    tips_ = {
        "Chain together drifts, near misses, and drafting to build your Skill Multiplier.",
        "Smash all 100 Horizon Discount Signs across Colorado for free upgrades at the Horizon Garage.",
        "Listen to Horizon Bass Arena, Pulse, and Rocks for Barn Find rumours from the festival DJs.",
        "Adjust ABS, Traction Control, and Manual + Clutch in Settings to boost your race credit bonus.",
    };
  }
}

void LoadingScreen::set_tips(std::vector<std::string> tips) {
  if (!tips.empty()) {
    tips_ = std::move(tips);
    tip_idx_ = 0;
  }
}

void LoadingScreen::set_progress(float p) { progress_ = std::clamp(p, 0.0f, 1.0f); }

void LoadingScreen::update(double dt) {
  if (!active_) return;
  dt = std::max(0.0, dt);
  elapsed_ += dt;
  tip_timer_ += dt;
  if (tip_timer_ >= 5.0 && !tips_.empty()) {
    tip_timer_ -= 5.0;
    tip_idx_ = (tip_idx_ + 1) % int(tips_.size());
  }
}

const std::string& LoadingScreen::current_tip() const {
  static const std::string kEmpty;
  if (tips_.empty()) return kEmpty;
  return tips_[size_t(tip_idx_ % int(tips_.size()))];
}

void LoadingScreen::draw(ui::List& l, float screen_w, float screen_h) const {
  if (!active_) return;
  const float s = screen_h / 720.0f;
  (void)ui_scene("2_C_INI_LOADING");

  // Deep Horizon night-to-sunset loading backdrop.
  l.rect(0, 0, screen_w, screen_h, ui::rgba(14, 16, 24, 255), 0.0f, ui::rgba(36, 14, 34, 255));
  // Subtle diagonal pink accent bar.
  l.rect(0, screen_h - 170.0f * s, screen_w, screen_h, ui::rgba(8, 10, 14, 235));
  l.rect(0, screen_h - 172.0f * s, screen_w, screen_h - 168.0f * s, kPink);

  // Destination title & subtitle.
  draw_brush_strip(l, 70.0f * s, 90.0f * s, 720.0f * s, 164.0f * s, kPink);
  ui::TextStyle tt{.size = 42.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
  l.text(98.0f * s, 106.0f * s, title_.empty() ? "HORIZON COLORADO" : title_, tt);
  if (!subtitle_.empty()) {
    draw_brush_strip(l, 70.0f * s, 174.0f * s, 540.0f * s, 214.0f * s, kBlack);
    ui::TextStyle st{.size = 18.0f * s, .color = kDim, .font = ui::Font::kB, .keep_case = true};
    l.text(98.0f * s, 185.0f * s, subtitle_, st);
  }

  // Cycling tip in bottom panel (supports multi-line \n & {colour=...} tags!).
  ui::TextStyle tip_hdr{.size = 13.0f * s, .color = kPink, .font = ui::Font::kE};
  l.text(72.0f * s, screen_h - 150.0f * s, "FESTIVAL TIP", tip_hdr);
  ui::TextStyle tip_ts{.size = 17.0f * s, .color = kWhite, .font = ui::Font::kA, .keep_case = true};
  l.text(72.0f * s, screen_h - 124.0f * s, current_tip(), tip_ts);

  // Progress bar.
  const float bx0 = 72.0f * s, bx1 = screen_w - 220.0f * s, by = screen_h - 52.0f * s, bh = 8.0f * s;
  l.rect(bx0, by, bx1, by + bh, ui::rgba(255, 255, 255, 42), 4.0f * s);
  const float fill_w = std::max(bh, (bx1 - bx0) * std::clamp(progress_, 0.0f, 1.0f));
  l.rect(bx0, by, bx0 + fill_w, by + bh, kPink, 4.0f * s);

  char pct[16];
  std::snprintf(pct, sizeof pct, "%d%%", int(std::lround(progress_ * 100.0f)));
  ui::TextStyle pct_ts{.size = 14.0f * s, .color = kDim, .align = ui::Align::kRight, .font = ui::Font::kB};
  l.text(bx1, by - 22.0f * s, pct, pct_ts);

  // Animated rotating Horizon loading spinner bottom-right.
  const float sx = screen_w - 110.0f * s;
  const float sy = screen_h - 84.0f * s;
  const float r = 24.0f * s;
  const float ang = float(std::fmod(elapsed_ * 4.5, 6.2831853));
  l.arc(sx, sy, r, 4.0f * s, 0.0f, 6.2831853f, ui::rgba(255, 255, 255, 40));
  l.arc(sx, sy, r, 4.5f * s, ang, ang + 1.85f, kPink);
  ui::TextStyle lw{.size = 11.0f * s, .color = kDim, .align = ui::Align::kCenter, .font = ui::Font::kB};
  l.text(sx, sy + r + 10.0f * s, "LOADING", lw);
}

// ------------------------------------------------ Virtual On-Screen Keyboard

void OnScreenKeyboard::open(std::string title, std::string initial_text, int max_length, Purpose purpose) {
  open_ = true;
  confirmed_ = false;
  cancelled_ = false;
  title_ = std::move(title);
  text_ = std::move(initial_text);
  max_len_ = std::max(1, max_length);
  if (int(text_.size()) > max_len_) text_.resize(size_t(max_len_));
  purpose_ = purpose;
  shifted_ = (purpose == Purpose::kLicensePlate);
  symbols_ = false;
  row_ = 0;
  col_ = 0;
  blink_t_ = 0.0;
}

std::vector<std::vector<std::string>> OnScreenKeyboard::active_layout() const {
  if (symbols_) {
    return {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
        {"-", "_", "/", ":", ";", "(", ")", "&", "@", "#"},
        {"!", "?", ".", ",", "'", "\"", "+", "=", "*", "%"},
        {"ABC", "SPACE", "BKSP", "CLEAR", "DONE"},
    };
  }
  if (shifted_) {
    return {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
        {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
        {"A", "S", "D", "F", "G", "H", "J", "K", "L", "-"},
        {"SHIFT", "Z", "X", "C", "V", "B", "N", "M", "_", "BKSP"},
        {"SYM", "SPACE", "CLEAR", "CANCEL", "DONE"},
    };
  }
  return {
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
      {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l", "-"},
      {"SHIFT", "z", "x", "c", "v", "b", "n", "m", "_", "BKSP"},
      {"SYM", "SPACE", "CLEAR", "CANCEL", "DONE"},
  };
}

void OnScreenKeyboard::move(int dcol, int drow) {
  if (!open_) return;
  const auto lay = active_layout();
  const int rows = int(lay.size());
  row_ = ((row_ + drow) % rows + rows) % rows;
  const int cols = int(lay[size_t(row_)].size());
  col_ = std::clamp(col_, 0, cols - 1);
  if (dcol != 0) col_ = ((col_ + dcol) % cols + cols) % cols;
}

void OnScreenKeyboard::press_selected() {
  if (!open_) return;
  const auto lay = active_layout();
  if (row_ < 0 || row_ >= int(lay.size())) return;
  const auto& r = lay[size_t(row_)];
  if (col_ < 0 || col_ >= int(r.size())) return;
  const std::string& key = r[size_t(col_)];
  if (key == "SHIFT") toggle_shift();
  else if (key == "SYM" || key == "ABC") {
    toggle_symbols();
    row_ = std::min(row_, int(active_layout().size()) - 1);
    col_ = 0;
  } else if (key == "SPACE") type_char(' ');
  else if (key == "BKSP") backspace();
  else if (key == "CLEAR") text_.clear();
  else if (key == "CANCEL") cancel();
  else if (key == "DONE") confirm();
  else if (key.size() == 1) type_char(key[0]);
}

void OnScreenKeyboard::type_char(char c) {
  if (!open_ || int(text_.size()) >= max_len_) return;
  if (c < 32 || c > 126) return;
  if (purpose_ == Purpose::kLicensePlate) c = char(std::toupper(static_cast<unsigned char>(c)));
  text_.push_back(c);
}

void OnScreenKeyboard::backspace() {
  if (!open_ || text_.empty()) return;
  text_.pop_back();
}

void OnScreenKeyboard::confirm() {
  if (!open_) return;
  confirmed_ = true;
  cancelled_ = false;
  open_ = false;
}

void OnScreenKeyboard::cancel() {
  if (!open_) return;
  cancelled_ = true;
  confirmed_ = false;
  open_ = false;
}

void OnScreenKeyboard::draw(ui::List& l, float screen_w, float screen_h) const {
  if (!open_) return;
  const float s = screen_h / 720.0f;
  l.rect(0, 0, screen_w, screen_h, ui::rgba(6, 8, 14, 175));

  const float pw = 760.0f * s, ph = 430.0f * s;
  const float px0 = 0.5f * (screen_w - pw), py0 = 0.5f * (screen_h - ph);
  l.rect(px0, py0, px0 + pw, py0 + ph, ui::rgba(14, 16, 22, 242), 8.0f * s);
  l.frame(px0, py0, px0 + pw, py0 + ph, kPink, 2.0f * s, 8.0f * s);

  // Title header.
  ui::TextStyle tt{.size = 22.0f * s, .color = kWhite, .font = ui::Font::kE, .keep_case = true};
  l.text(px0 + 28.0f * s, py0 + 20.0f * s, title_.empty() ? "ENTER TEXT" : title_, tt);

  char lenbuf[32];
  std::snprintf(lenbuf, sizeof lenbuf, "%zu / %d", text_.size(), max_len_);
  ui::TextStyle lt{.size = 13.0f * s, .color = kDim, .align = ui::Align::kRight, .font = ui::Font::kB};
  l.text(px0 + pw - 28.0f * s, py0 + 26.0f * s, lenbuf, lt);

  // Text input box with blinking caret.
  const float ix0 = px0 + 28.0f * s, ix1 = px0 + pw - 28.0f * s, iy0 = py0 + 58.0f * s, iy1 = iy0 + 48.0f * s;
  l.rect(ix0, iy0, ix1, iy1, ui::rgba(28, 32, 42, 255), 4.0f * s);
  l.frame(ix0, iy0, ix1, iy1, ui::rgba(255, 255, 255, 60), 1.5f * s, 4.0f * s);
  const bool caret_on = std::fmod(blink_t_, 1.0) < 0.55;
  const std::string shown = text_ + (caret_on ? "_" : "");
  ui::TextStyle it{.size = 22.0f * s, .color = kWhite, .font = ui::Font::kB, .keep_case = true};
  l.text(ix0 + 16.0f * s, iy0 + 13.0f * s, shown, it);

  // Key grid.
  const auto lay = active_layout();
  const float gx0 = px0 + 28.0f * s, gw = pw - 56.0f * s;
  const float gy0 = iy1 + 20.0f * s;
  const float gap = 8.0f * s;
  const float key_h = 46.0f * s;
  for (size_t r = 0; r < lay.size(); ++r) {
    const auto& row_keys = lay[r];
    const float kw = (gw - gap * float(row_keys.size() - 1)) / float(row_keys.size());
    const float ky0 = gy0 + float(r) * (key_h + gap);
    for (size_t c = 0; c < row_keys.size(); ++c) {
      const bool sel = (int(r) == row_ && int(c) == col_);
      const bool is_done = (row_keys[c] == "DONE");
      const float kx0 = gx0 + float(c) * (kw + gap);
      const uint32_t bg = sel ? kPink : is_done ? ui::rgba(40, 120, 72, 235) : ui::rgba(34, 38, 48, 240);
      l.rect(kx0, ky0, kx0 + kw, ky0 + key_h, bg, 4.0f * s);
      if (sel) l.frame(kx0, ky0, kx0 + kw, ky0 + key_h, kWhite, 2.0f * s, 4.0f * s);
      ui::TextStyle kt;
      kt.font = row_keys[c].size() > 1 ? ui::Font::kB : ui::Font::kE;
      kt.size = (row_keys[c].size() > 1 ? 13.0f : 18.0f) * s;
      kt.color = kWhite;
      kt.align = ui::Align::kCenter;
      kt.keep_case = true;
      l.text(kx0 + 0.5f * kw, ky0 + 0.5f * (key_h - kt.size), row_keys[c], kt);
    }
  }
}

}  // namespace carview
