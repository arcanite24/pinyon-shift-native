#include "ui_widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "events_page.h"
#include "game_hud.h"
#include "settings_page.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kPink = ui4::kPink;
constexpr uint32_t kBlack = ui4::kBrushBlack;
constexpr uint32_t kWhite = ui4::kWhite;
constexpr uint32_t kDim = ui4::kDim;

void draw_brush_strip(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour) {
  ui4::brush(l, x0, y0, x1, y1, colour);
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
  // Retail FH1 SPEC-RADIO / hud-freeroam.png: right-aligned above the tachometer,
  // NO opaque dark rounded box — crisp station logo, drop-shadowed white track
  // title, and a vibrant Horizon pink distressed brush strip behind the artist!
  const float right_x = screen_w - (1280.0f - 1212.0f) * s;
  const float top_y = screen_h - (720.0f - 372.0f) * s;

  std::array<float, 4> logo_uv{};
  const std::string tex = radio_logo_texture_path(st.station_name, st.off);
  const bool have_logo = ui_texture(tex, logo_uv);
  const float logo_h = 44.0f * s;
  const float logo_w = 136.0f * s;

  const std::string title = st.off ? (st.station_label.empty() ? std::string("RADIO OFF") : st.station_label)
                                   : st.title;
  const std::string artist = st.off ? std::string{} : st.artist;

  ui::TextStyle title_ts = ui4::style(ui::Font::kE, 19.0f * s, ui::fade(kWhite, a), ui::Align::kRight, true);
  ui::TextStyle artist_ts = ui4::style(ui::Font::kB, 14.0f * s, ui::fade(kWhite, a), ui::Align::kRight, true);

  const float aw = artist.empty() ? 0.0f : ui::List::measure(artist, artist_ts);

  if (have_logo) {
    l.image(right_x - logo_w, top_y, right_x, top_y + logo_h, logo_uv, ui::fade(kWhite, a));
  } else {
    ui::TextStyle st_ts = ui4::style(ui::Font::kE, 16.0f * s, ui::fade(kPink, a), ui::Align::kRight);
    const std::string sl = !st.station_label.empty() ? st.station_label : st.station_name;
    ui4::shadowed_text(l, right_x, top_y + 12.0f * s, sl, st_ts, s);
  }

  // Title line (crisp drop-shadowed white, right-aligned).
  const float title_y = top_y + logo_h + 6.0f * s;
  ui4::shadowed_text(l, right_x, title_y, title, title_ts, s);

  // Artist line on an iconic tilted FH1 pink brush strip.
  if (!artist.empty()) {
    const float ay = title_y + 26.0f * s;
    const float strip_w = aw + 28.0f * s;
    ui4::TiltScope tilt(l, right_x - 0.5f * strip_w, ay + 9.0f * s, ui4::kTiltRad * 0.5f);
    ui4::brush(l, right_x - strip_w, ay - 3.0f * s, right_x + 4.0f * s, ay + 21.0f * s, ui::fade(kPink, a));
    l.text(right_x - 10.0f * s, ay + 2.0f * s, artist, artist_ts);
  }
}

// ------------------------------------------------ UI4 Scene Parity Renderers

void draw_button_bar_ui4(ui::List& l, const std::vector<ButtonBarItem>& items, float screen_w, float screen_h) {
  std::vector<ui4::Prompt> prompts;
  prompts.reserve(items.size());
  for (const auto& it : items) prompts.push_back({it.button, it.label});
  ui4::button_bar(l, prompts, screen_w, screen_h);
}

void draw_pause_menu_ui4(ui::List& l, const PauseMenuUi4State& st, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  // Retail 925_PAUSE_MENU keeps Colorado bright & clear behind the tilted brushes.
  ui4::subtle_vignette(l, screen_w, screen_h, 0.22f);

  float list_x = 147.0f * s;
  float list_y0 = 128.0f * s;
  float pitch = 50.0f * s;
  float slant_rad = ui4::kTiltRad;
  if (const fh1::UiScene* sc_raw = ui_scene("925_PAUSE_MENU")) {
    SceneDraw sc(sc_raw, true);
    if (const int b0 = sc.find("Button0"); b0 >= 0) {
      const auto p = sc.at(b0);
      if (p[0] > 40.0f && p[0] < 400.0f) list_x = (p[0] - 40.0f) * s;
      if (p[1] > 60.0f && p[1] < 240.0f) list_y0 = (p[1] - 24.0f) * s;
      const float a = -sc.angle(b0);
      if (std::fabs(a) > 1e-3f) slant_rad = a;
    }
  }
  const int visible_count = int(st.items.size());
  const float max_h = screen_h - list_y0 - 95.0f * s;
  if (visible_count > 1 && pitch * float(visible_count) > max_h) {
    pitch = std::max(36.0f * s, max_h / float(visible_count));
  }
  const auto ys = fh1::reflow_stacker(list_y0, pitch, visible_count);

  {
    ui4::TiltScope tilt(l, list_x + 150.0f * s, list_y0 + 180.0f * s, slant_rad);
    for (int i = 0; i < visible_count; ++i) {
      const bool sel = (i == st.selected);
      ui::TextStyle ts = ui4::style(ui::Font::kE, (sel ? 38.0f : 33.0f) * s * (pitch / (50.0f * s)), kWhite,
                                    ui::Align::kLeft, true);
      const float tw = ui::List::measure(st.items[size_t(i)], ts);
      const float bw = std::max(sel ? 260.0f * s : 210.0f * s, tw + 74.0f * s);
      const float bh = pitch * (sel ? 1.02f : 0.86f);
      const float ry = ys[size_t(i)];
      const uint32_t bg = sel ? ui::fade(kPink, 0.88f + 0.12f * st.selection_anim) : kBlack;
      ui4::brush(l, list_x, ry, list_x + bw, ry + bh, bg, sel, sel ? (0.4f + 0.6f * st.selection_anim) : 1.0f);
      l.text(list_x + 36.0f * s, ry + 0.5f * (bh - ts.size), st.items[size_t(i)], ts);
    }
  }

  // Pixel-matched top-right Career Strip (out/oracle/ui/pause-freeroam.png).
  ui4::CareerStripData cs;
  cs.band_name = st.wristband_label;
  cs.band_next = st.wristband_goal;
  cs.band_collect_ready = !st.wristband_goal.empty();
  cs.car_name = st.car_name;
  cs.car_class = st.car_class;
  cs.car_pi = st.car_pi;
  // Parse credits string if numeric, else draw right-aligned off-white strip below.
  cs.credits = -1;
  ui4::career_strip(l, cs, screen_w, screen_h);
  if (!st.credits_text.empty()) {
    const float rx = screen_w - 70.0f * s;
    const float cry = 92.0f * s;
    ui4::offwhite_strip(l, rx - 130.0f * s, cry, rx, cry + 24.0f * s, s, true);
    ui::TextStyle crs = ui4::style(ui::Font::kB, 15.5f * s, ui4::kInk, ui::Align::kRight, true);
    l.text(rx - 10.0f * s, cry + 4.5f * s, st.credits_text, crs);
  }

  draw_button_bar_ui4(l, st.help_bar, screen_w, screen_h);
}

void draw_map_chrome_ui4(ui::List& l, const MapChromeUi4State& st, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  (void)ui_scene("H_MAP");
  ui4::title_banner(l, s, 60.0f * s, 26.0f * s, st.filter_label.empty() ? "COLORADO MAP" : st.filter_label,
                    "HORIZON MAP", false, 400.0f);

  if (!st.roads_label.empty()) {
    const float rx = screen_w - 64.0f * s;
    ui4::panel(l, rx - 300.0f * s, 34.0f * s, rx, 68.0f * s, s, ui4::kCharcoalDark, 0, ui4::kGreen, true);
    ui::TextStyle rts = ui4::style(ui::Font::kB, 14.0f * s, kWhite, ui::Align::kRight, true);
    l.text(rx - 14.0f * s, 44.0f * s, st.roads_label, rts);
  }
  if (!st.hovered_title.empty()) {
    const float rx = screen_w - 64.0f * s;
    const float cy = screen_h - 156.0f * s;
    ui4::panel(l, rx - 350.0f * s, cy, rx, cy + 76.0f * s, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    ui::TextStyle ht = ui4::style(ui::Font::kE, 19.0f * s, kWhite, ui::Align::kLeft, true);
    ui::TextStyle hs = ui4::style(ui::Font::kB, 13.0f * s, kDim, ui::Align::kLeft, true);
    l.text(rx - 332.0f * s, cy + 14.0f * s, st.hovered_title, ht);
    l.text(rx - 332.0f * s, cy + 44.0f * s, st.hovered_subtitle, hs);
  }
  draw_button_bar_ui4(l, st.help_bar, screen_w, screen_h);
}

void draw_pre_post_race_ui4(ui::List& l, const PrePostRaceUi4State& st, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  (void)ui_scene("PRE_AND_POST_RACE");
  ui4::subtle_vignette(l, screen_w, screen_h, 0.25f);
  if (st.is_post_race) {
    {
      ui4::TiltScope tilt(l, 240.0f * s, 198.0f * s, ui4::kTiltRad);
      ui4::brush(l, 75.0f * s, 165.0f * s, 395.0f * s, 232.0f * s, ui4::kBrushBlack);
      ui::TextStyle ft = ui4::style(ui::Font::kE, 42.0f * s, ui4::kWhite);
      l.text(92.0f * s, 178.0f * s, st.event_type.empty() ? "FINISHED" : st.event_type, ft);
    }
    {
      ui4::TiltScope tilt(l, 370.0f * s, 275.0f * s, ui4::kTiltRad);
      ui4::brush(l, -18.0f * s, 214.0f * s, 740.0f * s, 336.0f * s, ui4::kPink, true);
      ui::TextStyle pt = ui4::style(ui::Font::kE, 72.0f * s, ui4::kWhite);
      ui4::fit_text(st.place_text, pt, 620.0f * s, 30.0f * s);
      l.text(92.0f * s, 238.0f * s + 0.5f * (72.0f * s - pt.size), st.place_text, pt);
    }
  } else {
    {
      ui4::TiltScope tilt(l, 450.0f * s, screen_h - 110.0f * s, ui4::kTiltRad);
      ui4::brush(l, -18.0f * s, screen_h - 140.0f * s, 920.0f * s, screen_h - 74.0f * s, ui4::kPink, true);
      ui::TextStyle en = ui4::style(ui::Font::kE, 40.0f * s, ui4::kWhite);
      ui4::fit_text(st.event_name, en, 800.0f * s, 20.0f * s);
      l.text(96.0f * s, screen_h - 127.0f * s + 0.5f * (40.0f * s - en.size), st.event_name, en);
    }
    {
      ui4::TiltScope tilt(l, 260.0f * s, screen_h - 158.0f * s, ui4::kTiltRad);
      ui4::brush(l, 75.0f * s, screen_h - 185.0f * s, 450.0f * s, screen_h - 130.0f * s, ui4::kBrushBlack);
      ui::TextStyle et = ui4::style(ui::Font::kE, 30.0f * s, ui4::kWhite);
      ui4::fit_text(st.event_type, et, 340.0f * s, 16.0f * s);
      l.text(96.0f * s, screen_h - 173.0f * s + 0.5f * (30.0f * s - et.size), st.event_type, et);
    }

    const auto ys = fh1::reflow_stacker(175.0f * s, 52.0f * s, int(st.menu_items.size()));
    ui4::TiltScope mt(l, 250.0f * s, 220.0f * s, ui4::kTiltRad);
    for (size_t i = 0; i < st.menu_items.size(); ++i) {
      const bool sel = int(i) == st.selected;
      ui::TextStyle ts = ui4::style(ui::Font::kE, (sel ? 36.0f : 30.0f) * s, ui4::kWhite);
      const float bw = std::max(280.0f * s, ui::List::measure(st.menu_items[i], ts) + 74.0f * s);
      ui4::brush(l, 147.0f * s, ys[i], 147.0f * s + bw, ys[i] + 46.0f * s, sel ? ui4::kPink : ui4::kBrushBlack, sel);
      l.text(184.0f * s, ys[i] + 6.0f * s, st.menu_items[i], ts);
    }
  }
  draw_button_bar_ui4(l, st.help_bar, screen_w, screen_h);
}

void draw_street_race_hub_ui4(ui::List& l, const StreetRaceHubUi4State& st, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  (void)ui_scene("H_STREET_RACE_HUB");
  ui4::subtle_vignette(l, screen_w, screen_h, 0.34f);

  ui4::title_banner(l, s, 56.0f * s, 30.0f * s,
                    st.hub_name.empty() ? "STREET RACE HUB" : st.hub_name,
                    st.hub_subtitle.empty() ? "HORIZON UNDERGROUND" : st.hub_subtitle, true, 560.0f);

  const float px0 = 56.0f * s, py0 = 132.0f * s, px1 = screen_w - 56.0f * s;
  const float ph = std::min(screen_h - 216.0f * s, std::max(180.0f * s, float(st.races.size()) * 58.0f * s + 54.0f * s));
  ui4::panel(l, px0, py0, px1, py0 + ph, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, px0 + 14.0f * s, py0 + 12.0f * s, px1 - 14.0f * s, py0 + 36.0f * s, s, false);
  l.text(px0 + 26.0f * s, py0 + 17.0f * s, "UNDERGROUND EVENTS", ui4::style(ui::Font::kE, 13.5f * s, ui4::kInk));
  l.text(px1 - 26.0f * s, py0 + 17.0f * s, "CLASS   •   STAKE / PRIZE",
         ui4::style(ui::Font::kB, 12.5f * s, ui4::kInk, ui::Align::kRight));

  const auto ys = fh1::reflow_stacker(py0 + 48.0f * s, 54.0f * s, int(st.races.size()));
  for (size_t i = 0; i < st.races.size(); ++i) {
    if (ys[i] + 46.0f * s > py0 + ph - 8.0f * s) break;
    const auto& r = st.races[i];
    const bool sel = int(i) == st.selected;
    if (sel) {
      ui4::brush(l, px0 + 8.0f * s, ys[i] - 2.0f * s, px1 - 8.0f * s, ys[i] + 48.0f * s, ui4::kPink, true);
    } else {
      l.rect(px0 + 14.0f * s, ys[i], px1 - 14.0f * s, ys[i] + 46.0f * s,
             (i % 2 == 0) ? ui::rgba(32, 34, 42, 220) : ui::rgba(24, 26, 32, 220));
    }
    ui::TextStyle rt = ui4::style(sel ? ui::Font::kE : ui::Font::kB, (sel ? 21.0f : 18.0f) * s, ui4::kWhite);
    ui4::fit_text(r.name, rt, (px1 - px0) - 340.0f * s, 12.0f * s);
    l.text(px0 + 28.0f * s, ys[i] + 0.5f * (46.0f * s - rt.size), r.name, rt);

    float rx = px1 - 26.0f * s;
    if (r.completed) {
      ui4::brush(l, rx - 78.0f * s, ys[i] + 10.0f * s, rx, ys[i] + 36.0f * s, ui4::kGreen);
      l.text(rx - 39.0f * s, ys[i] + 16.0f * s, "WON", ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk, ui::Align::kCenter));
      rx -= 90.0f * s;
    }
    if (!r.stake.empty()) {
      ui::TextStyle sk = ui4::style(ui::Font::kE, 16.0f * s, sel ? ui4::kYellow : ui4::kWhite, ui::Align::kRight, true);
      l.text(rx, ys[i] + 14.0f * s, r.stake, sk);
      rx -= ui::List::measure(r.stake, sk) + 22.0f * s;
    }
    if (!r.car_class.empty()) {
      l.rect(rx - 86.0f * s, ys[i] + 11.0f * s, rx, ys[i] + 35.0f * s, ui4::class_colour_by_name(r.car_class));
      l.text(rx - 43.0f * s, ys[i] + 16.0f * s, r.car_class,
             ui4::style(ui::Font::kE, 13.5f * s, ui4::kWhite, ui::Align::kCenter));
    }
  }
  draw_button_bar_ui4(l, st.help_bar, screen_w, screen_h);
}

void draw_events_ui4(ui::List& l, float screen_w, float screen_h, const EventsPage& page,
                     const std::vector<garage::ClassBand>& bands) {
  if (!page.is_open()) return;
  const float s = screen_h / 720.0f;
  ui4::subtle_vignette(l, screen_w, screen_h, 0.36f);

  const bool is_hub = page.hub_filter() > 0;
  ui4::title_banner(l, s, 56.0f * s, 26.0f * s,
                    is_hub ? ("STREET RACE HUB " + std::to_string(page.hub_filter())) : "RACE CENTRAL",
                    is_hub ? "COLORADO UNDERGROUND" : "HORIZON FESTIVAL EVENTS", true, 470.0f);

  if (!is_hub) {
    static const std::vector<std::string> kEventTabs = {"ALL", "FESTIVAL", "STREET", "SHOWCASES", "SHOWDOWNS"};
    ui4::tab_strip(l, s, 56.0f * s, 102.0f * s, screen_w - 56.0f * s, kEventTabs, int(page.active_tab()), true,
                   ui4::kPink);
  }

  const float top_y = is_hub ? 116.0f * s : 146.0f * s;
  const float bot_y = screen_h - 82.0f * s;
  const float lx0 = 56.0f * s, lw = 640.0f * s;
  const float rx0 = lx0 + lw + 18.0f * s, rx1 = screen_w - 56.0f * s;

  ui4::panel(l, lx0, top_y, lx0 + lw, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, lx0 + 12.0f * s, top_y + 10.0f * s, lx0 + lw - 12.0f * s, top_y + 34.0f * s, s, false);
  l.text(lx0 + 22.0f * s, top_y + 15.0f * s,
         "AVAILABLE EVENTS (" + std::to_string(page.entries().size()) + ")",
         ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

  const auto& entries = page.entries();
  if (entries.empty()) {
    l.text(lx0 + 0.5f * lw, top_y + 160.0f * s, "NO EVENTS IN THIS CATEGORY YET",
           ui4::style(ui::Font::kB, 15.0f * s, ui4::kDim, ui::Align::kCenter));
  } else {
    const float row_h = 54.0f * s;
    const int max_vis = std::max(1, int((bot_y - top_y - 50.0f * s) / row_h));
    const int n = int(entries.size());
    const int sel = int(std::min(page.selected_index(), entries.size() - 1));
    const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, n - max_vis));
    float ry = top_y + 44.0f * s;
    for (int i = first; i < std::min(n, first + max_vis); ++i) {
      const auto& e = entries[size_t(i)];
      const bool act = (i == sel);
      if (act) {
        ui4::brush(l, lx0 + 6.0f * s, ry, lx0 + lw - 6.0f * s, ry + row_h - 4.0f * s,
                   e.is_collect_wristband ? ui4::kYellow : ui4::kPink);
      } else {
        l.rect(lx0 + 12.0f * s, ry, lx0 + lw - 12.0f * s, ry + row_h - 6.0f * s,
               (i % 2 == 0) ? ui::rgba(32, 34, 42, 220) : ui::rgba(24, 26, 32, 220));
      }

      const uint32_t title_col =
          (act && e.is_collect_wristband) ? ui4::kInk : (!e.unlocked ? ui4::kDim : ui4::kWhite);
      ui::TextStyle ts = ui4::style(act ? ui::Font::kE : ui::Font::kB, (act ? 16.5f : 14.5f) * s, title_col);
      ui4::fit_text(e.display_name, ts, lw - 170.0f * s, 10.0f * s);
      l.text(lx0 + 22.0f * s, ry + 7.0f * s, e.display_name, ts);

      std::string sub;
      if (e.is_collect_wristband) {
        sub = "WRISTBAND CEREMONY READY — PRESS [A] TO COLLECT";
      } else if (e.event) {
        sub = std::string(race::CareerCatalog::event_style_label(*e.event)) + "  •  " +
              ui4::format_credits(int64_t(e.event->cash_prize));
      }
      l.text(lx0 + 22.0f * s, ry + 28.0f * s, sub,
             ui4::style(ui::Font::kB, 11.0f * s,
                        (act && e.is_collect_wristband) ? ui4::kInk : (act ? ui4::kYellow : ui4::kDim)));

      std::string tag = "NEW";
      uint32_t tag_bg = ui4::kBlue;
      uint32_t tag_fg = ui4::kWhite;
      if (e.is_collect_wristband) {
        tag = "COLLECT", tag_bg = ui4::kBrushBlack, tag_fg = ui4::kYellow;
      } else if (!e.unlocked) {
        tag = "LOCKED", tag_bg = ui::rgba(60, 64, 74, 220), tag_fg = ui4::kDim;
      } else if (e.best_place == 1) {
        tag = "1ST", tag_bg = ui4::kGreen, tag_fg = ui4::kInk;
      } else if (e.best_place > 0) {
        tag = std::to_string(e.best_place) + (e.best_place == 2 ? "ND" : e.best_place == 3 ? "RD" : "TH");
        tag_bg = ui4::kAmber, tag_fg = ui4::kInk;
      } else if (!e.eligible) {
        tag = "CLASS", tag_bg = ui4::kRed, tag_fg = ui4::kWhite;
      }
      l.rect(lx0 + lw - 96.0f * s, ry + 12.0f * s, lx0 + lw - 22.0f * s, ry + 34.0f * s, tag_bg);
      l.text(lx0 + lw - 59.0f * s, ry + 17.0f * s, tag,
             ui4::style(ui::Font::kE, 11.5f * s, tag_fg, ui::Align::kCenter));
      ry += row_h;
    }
  }

  ui4::panel(l, rx0, top_y, rx1, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  std::string d_title, dummy_t;
  std::vector<std::string> dummy_lines, d_lines;
  int dummy_sel = 0;
  page.fill(dummy_t, dummy_lines, dummy_sel, d_title, d_lines);

  if (const auto* sel = page.selected_entry()) {
    ui4::brush(l, rx0 + 8.0f * s, top_y + 12.0f * s, rx1 - 8.0f * s, top_y + 48.0f * s, ui4::kPink);
    ui::TextStyle hst = ui4::style(ui::Font::kE, 18.0f * s, ui4::kWhite);
    const std::string hdr = d_title.empty() ? sel->display_name : d_title;
    ui4::fit_text(hdr, hst, (rx1 - rx0) - 34.0f * s, 11.0f * s);
    l.text(rx0 + 20.0f * s, top_y + 20.0f * s + 0.5f * (18.0f * s - hst.size), hdr, hst);

    float dy = top_y + 62.0f * s;
    if (sel->event && sel->event->target_class > 0 && size_t(sel->event->target_class) < bands.size()) {
      const auto& cb = bands[size_t(sel->event->target_class)];
      ui4::class_badge(l, rx0 + 20.0f * s, dy, 26.0f * s, cb.name, int(std::lround(cb.shown_max)), sel->event->target_class);
      ui4::offwhite_strip(l, rx0 + 130.0f * s, dy, rx1 - 20.0f * s, dy + 26.0f * s, s, false);
      l.text(rx1 - 28.0f * s, dy + 6.0f * s, ui4::format_credits(int64_t(sel->event->cash_prize)),
             ui4::style(ui::Font::kE, 15.0f * s, ui4::kInk, ui::Align::kRight));
      dy += 40.0f * s;
    }

    for (const auto& line : d_lines) {
      if (dy + 26.0f * s > bot_y - 16.0f * s) break;
      const bool ready = line.rfind("READY TO RACE", 0) == 0;
      const bool locked = line.rfind("LOCKED:", 0) == 0 || line.rfind("INELIGIBLE:", 0) == 0;
      const bool prize = line.rfind("1ST PLACE PRIZE", 0) == 0 || line.rfind("BONUS CAR:", 0) == 0;
      if (ready) {
        ui4::brush(l, rx0 + 14.0f * s, dy, rx1 - 14.0f * s, dy + 32.0f * s, ui4::kGreen);
        l.text(0.5f * (rx0 + rx1), dy + 9.0f * s, line,
               ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk, ui::Align::kCenter));
        dy += 38.0f * s;
      } else {
        ui::TextStyle ls = ui4::style(prize ? ui::Font::kE : ui::Font::kB, 13.0f * s,
                                      locked ? ui4::kRed : (prize ? ui4::kYellow : ui4::kWhite), ui::Align::kLeft, true);
        ui4::fit_text(line, ls, (rx1 - rx0) - 40.0f * s, 9.5f * s);
        l.text(rx0 + 20.0f * s, dy, line, ls);
        dy += 26.0f * s;
      }
    }
  }

  std::vector<ui4::Prompt> bar = {{"A", "START EVENT"}};
  if (!is_hub) bar.push_back({"LB", "FILTER TAB"});
  bar.push_back({"B", "BACK"});
  ui4::button_bar(l, bar, screen_w, screen_h, 56.0f);
}

void draw_settings_ui4(ui::List& l, float screen_w, float screen_h, const SettingsPage& page, bool live_overlay) {
  if (!page.is_open()) return;
  const float s = screen_h / 720.0f;
  ui4::subtle_vignette(l, screen_w, screen_h, 0.42f);

  ui4::title_banner(l, s, 56.0f * s, 26.0f * s, live_overlay ? "LIVE SETTINGS" : "OPTIONS",
                    live_overlay ? "PINYON SHIFT // F6 OVERLAY" : "HORIZON FESTIVAL", true, 430.0f);

  static const std::vector<std::string> kTabs = {"DISPLAY", "AUDIO", "GAMEPLAY", "CONTROLS", "PINYON SHIFT"};
  const int cur_tab = int(page.active_tab());
  ui4::tab_strip(l, s, 56.0f * s, 102.0f * s, screen_w - 56.0f * s, kTabs, cur_tab, true, ui4::kPink);

  const auto all_rows = page.display_rows();
  std::vector<SettingsPage::DisplayRow> tab_rows;
  int sel_in_tab = 0;
  for (const auto& r : all_rows) {
    if (int(r.tab) == cur_tab || r.is_save) {
      if (r.selected) sel_in_tab = int(tab_rows.size());
      tab_rows.push_back(r);
    }
  }

  const float top_y = 146.0f * s;
  const float bot_y = screen_h - 82.0f * s;
  const float lx0 = 56.0f * s, lw = 760.0f * s;
  const float rx0 = lx0 + lw + 18.0f * s, rx1 = screen_w - 56.0f * s;

  ui4::panel(l, lx0, top_y, lx0 + lw, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, lx0 + 12.0f * s, top_y + 10.0f * s, lx0 + lw - 12.0f * s, top_y + 34.0f * s, s, false);
  l.text(lx0 + 22.0f * s, top_y + 15.0f * s,
         kTabs[size_t(std::clamp(cur_tab, 0, 4))] + " SETTINGS",
         ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
  l.text(lx0 + lw - 22.0f * s, top_y + 15.0f * s,
         page.capturing_key() ? "PRESS ANY KEY TO BIND (ESC CANCELS)" : "CHANGES APPLY LIVE",
         ui4::style(ui::Font::kB, 11.5f * s, page.capturing_key() ? ui4::kPink : ui4::kInk, ui::Align::kRight));

  const float row_h = 44.0f * s;
  const int max_vis = std::max(1, int((bot_y - top_y - 48.0f * s) / row_h));
  const int n = int(tab_rows.size());
  const int first = std::clamp(sel_in_tab - max_vis / 2, 0, std::max(0, n - max_vis));
  float ry = top_y + 42.0f * s;
  for (int i = first; i < std::min(n, first + max_vis); ++i) {
    const auto& r = tab_rows[size_t(i)];
    const bool act = r.selected;
    if (act) {
      ui4::brush(l, lx0 + 6.0f * s, ry, lx0 + lw - 6.0f * s, ry + row_h - 4.0f * s,
                 r.is_save ? ui4::kGreen : ui4::kPink);
    } else {
      l.rect(lx0 + 12.0f * s, ry, lx0 + lw - 12.0f * s, ry + row_h - 6.0f * s,
             (i % 2 == 0) ? ui::rgba(32, 34, 42, 220) : ui::rgba(24, 26, 32, 220));
    }
    const uint32_t fg = (act && r.is_save) ? ui4::kInk : ui4::kWhite;
    l.text(lx0 + 24.0f * s, ry + 11.0f * s, r.name,
           ui4::style(act ? ui::Font::kE : ui::Font::kB, (act ? 15.5f : 14.0f) * s, fg));

    const std::string vstr = r.is_save ? r.value : (r.is_rebind ? ("[ " + r.value + " ]") : ("<  " + r.value + "  >"));
    const float vx0 = lx0 + lw - 270.0f * s, vx1 = lx0 + lw - 22.0f * s;
    if (!r.is_save) {
      l.rect(vx0, ry + 7.0f * s, vx1, ry + row_h - 13.0f * s,
             r.capturing ? ui4::kYellow : (act ? ui::rgba(14, 15, 20, 175) : ui::rgba(255, 255, 255, 16)));
    }
    ui::TextStyle vs = ui4::style(ui::Font::kE, 14.0f * s,
                                  r.capturing ? ui4::kInk : (act && r.is_save ? ui4::kInk : (act ? ui4::kYellow : ui4::kWhite)),
                                  ui::Align::kCenter);
    ui4::fit_text(vstr, vs, (vx1 - vx0) - 12.0f * s, 9.5f * s);
    l.text(0.5f * (vx0 + vx1), ry + 12.0f * s + 0.5f * (14.0f * s - vs.size), vstr, vs);
    ry += row_h;
  }

  ui4::panel(l, rx0, top_y, rx1, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, rx0 + 12.0f * s, top_y + 10.0f * s, rx1 - 12.0f * s, top_y + 34.0f * s, s, false);
  l.text(rx0 + 22.0f * s, top_y + 15.0f * s, "OPTION DETAILS", ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

  if (sel_in_tab >= 0 && sel_in_tab < int(tab_rows.size())) {
    const auto& sr = tab_rows[size_t(sel_in_tab)];
    ui4::brush(l, rx0 + 8.0f * s, top_y + 46.0f * s, rx1 - 8.0f * s, top_y + 82.0f * s, ui4::kPink);
    ui::TextStyle stt = ui4::style(ui::Font::kE, 17.0f * s, ui4::kWhite);
    ui4::fit_text(sr.name, stt, (rx1 - rx0) - 34.0f * s, 11.0f * s);
    l.text(rx0 + 20.0f * s, top_y + 54.0f * s, sr.name, stt);

    l.text(rx0 + 20.0f * s, top_y + 98.0f * s, "CURRENT VALUE", ui4::style(ui::Font::kB, 11.0f * s, ui4::kDim));
    l.text(rx0 + 20.0f * s, top_y + 116.0f * s, sr.value, ui4::style(ui::Font::kE, 22.0f * s, ui4::kYellow));

    const char* hint = sr.is_save      ? "Writes changes to settings.txt and closes Settings."
                       : sr.is_rebind  ? "Press [A] / Enter then any key to rebind control."
                       : cur_tab == 0  ? "Display, camera rig, day length & post-FX."
                       : cur_tab == 1  ? "FMOD master volume, radio DJ & 8-bus mixer."
                       : cur_tab == 2  ? "Assists, AI difficulty, rubber-band & damage."
                       : cur_tab == 3  ? "Controller layout, rumble, deadzones & keys."
                                       : "Pinyon Shift QoL: language, intros & photo.";
    ui::TextStyle hs = ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite, ui::Align::kLeft, true);
    ui4::fit_text(hint, hs, (rx1 - rx0) - 40.0f * s, 9.5f * s);
    l.text(rx0 + 20.0f * s, top_y + 162.0f * s, hint, hs);

    const LiveSettings ls = page.live_settings();
    const float sy = bot_y - 124.0f * s;
    l.rect(rx0 + 16.0f * s, sy, rx1 - 16.0f * s, sy + 108.0f * s, ui::rgba(255, 255, 255, 14));
    l.text(rx0 + 26.0f * s, sy + 10.0f * s, "LIVE STATUS", ui4::style(ui::Font::kE, 12.0f * s, ui4::kPink));
    l.text(rx0 + 26.0f * s, sy + 32.0f * s, "LANGUAGE: " + ls.language, ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite));
    l.text(rx0 + 26.0f * s, sy + 54.0f * s,
           std::string("AUDIO: ") + (ls.sound ? "ON" : "OFF") + "  •  RADIO: " + (ls.radio ? "ON" : "OFF"),
           ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite));
    l.text(rx0 + 26.0f * s, sy + 76.0f * s,
           std::string("REWIND: ") + (ls.controls.assists.rewind ? "ON" : "OFF") + "  •  TCS: " +
               (ls.controls.assists.traction_control ? "ON" : "OFF"),
           ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim));
  }

  ui4::button_bar(l,
                  {{"DPAD_LR", "CHANGE"},
                   {"LB", "CATEGORY"},
                   {"A", "REBIND / SAVE"},
                   {"B", "BACK"}},
                  screen_w, screen_h, 56.0f);
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
