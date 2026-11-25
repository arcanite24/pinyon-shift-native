#include "game_hud_internal.h"
#include "hud_screen.h"
#include "race/campaign_flow.h"

#include <cstdlib>

namespace carview::hud_detail {

namespace {

std::string resolve_hud_event_title(const RaceHud& r) {
  std::string loc;
  if (!r.event.empty()) {
    const std::string gt = game_text(r.event);
    if (!gt.empty() && gt != r.event) loc = gt;
  }
  return race::resolve_event_display_title(r.event, loc);
}

struct ParsedGridDifficulty {
  bool found = false;
  int tier_index = 1;  // 0..4 (EASY, MED, HARD, PRO, HARD+)
  std::string tier_name = "MED";
  int bonus_pct = 15;
};

ParsedGridDifficulty parse_grid_difficulty(const RaceHud& r) {
  ParsedGridDifficulty out;
  out.bonus_pct = r.difficulty_pct > 0 ? std::max(0, r.difficulty_pct - r.assists_pct) : 15;
  for (const auto& item : r.grid_menu) {
    std::string up = item;
    for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (up.find("DIFFICULTY") == std::string::npos) continue;
    out.found = true;
    if (up.find("HARD+") != std::string::npos) {
      out.tier_index = 4;
      out.tier_name = "HARD+";
    } else if (up.find("PRO") != std::string::npos) {
      out.tier_index = 3;
      out.tier_name = "PRO";
    } else if (up.find("HARD") != std::string::npos) {
      out.tier_index = 2;
      out.tier_name = "HARD";
    } else if (up.find("EASY") != std::string::npos) {
      out.tier_index = 0;
      out.tier_name = "EASY";
    } else {
      out.tier_index = 1;
      out.tier_name = "MED";
    }
    const size_t plus = up.find('+');
    const size_t pct = plus != std::string::npos ? up.find('%', plus) : std::string::npos;
    if (plus != std::string::npos && pct != std::string::npos && pct > plus + 1) {
      out.bonus_pct = std::max(0, std::atoi(up.substr(plus + 1, pct - plus - 1).c_str()));
    }
    break;
  }
  return out;
}

bool menu_item_is_difficulty(std::string_view item) {
  std::string up(item);
  for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
  return up.find("DIFFICULTY") != std::string::npos;
}

bool menu_item_is_quit(std::string_view item) {
  std::string up(item);
  for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
  return up.find("QUIT") != std::string::npos || up.find("EXIT") != std::string::npos;
}

void draw_race_post_stage_pills(ui::List& l, const Layout& g, int active_stage_idx) {
  const float s = g.s;
  static constexpr const char* kLabels[3] = {"1  RACE RESULTS", "2  CREDITS EARNED", "3  WRISTBAND & REWARDS"};
  const float py = g.T(32);
  const float ph = 24.0f * s;
  const float gap = 8.0f * s;
  float widths[3]{};
  float total_w = 0.0f;
  for (int i = 0; i < 3; ++i) {
    widths[i] = std::max(122.0f * s, ui::List::measure(kLabels[i], font(ui::Font::kE, 11.0f * s, kText)) + 22.0f * s);
    total_w += widths[i];
  }
  total_w += gap * 2.0f;
  float x = g.w - 54.0f * s - total_w;
  Turn t(l, x + 0.5f * total_w, py + 0.5f * ph, ui4::kTiltRad * 0.45f);
  for (int i = 0; i < 3; ++i) {
    const bool act = (i == active_stage_idx);
    const bool done = (i < active_stage_idx);
    if (act) {
      ui4::brush(l, x - 3.0f * s, py - 2.0f * s, x + widths[i] + 3.0f * s, py + ph + 2.0f * s, ui4::kPink, false);
    } else {
      ui4::pill(l, x, py, x + widths[i], py + ph, done ? ui4::kRowAlt : ui4::kBrushBlack, ui4::PillKind::kHud01);
      if (done) l.rect(x, py + ph - 2.5f * s, x + widths[i], py + ph, ui4::kYellow);
    }
    l.text(x + 0.5f * widths[i], py + 6.0f * s, kLabels[i],
           font(ui::Font::kE, 11.0f * s, act ? kText : (done ? ui4::kYellow : ui4::kDim), Align::kCenter));
    x += widths[i] + gap;
  }
}

}  // namespace

void event_card(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  const SceneDraw sc = resolve_scene_draw("PRE_AND_POST_RACE", true);
  float tilt_rad = ui4::kTiltRad;
  if (sc.ok()) {
    if (const int rh = sc.find("RotationHelper"); rh >= 0) {
      const float a = -sc.angle(rh);
      if (std::fabs(a) > 1e-3f) tilt_rad = a;
    }
  }
  const std::string event_title = resolve_hud_event_title(r);
  const std::string event_type =
      !r.event_type.empty() ? r.event_type : str("CareerEventUI", "IDS_TitleFestivalRace", "FESTIVAL RACE");
  const ParsedGridDifficulty diff = parse_grid_difficulty(r);
  const int total_bonus_pct = diff.bonus_pct + std::max(0, r.assists_pct);

  // Lift the bottom-left event banner cleanly above the button bar.
  const float lift = r.grid_menu_open ? 44.0f : 0.0f;
  // Full-bleed-left pink brush with localized event title, overlapping black MiniTape with event type + metadata strip.
  {
    l.turn(g.L(484), g.B(585 - lift), tilt_rad);
    ui4::brush(l, -24 * s, g.B(538 - lift), g.L(820), g.B(632 - lift), ui4::kPink, true);
    TextStyle ns = font(ui::Font::kE, 44 * s, kText);
    fit(event_title, ns, g.L(792) - g.L(96), 20 * s);
    l.text(g.L(96), g.B(576 - lift) + (44 * s - ns.size) * 0.5f, event_title, ns);

    // Crisp dark event metadata strip directly beneath the pink title banner.
    std::string meta = r.showcase
                           ? ("SHOWCASE  •  VS " + (r.showcase_aircraft.empty() ? std::string("AIRCRAFT") : r.showcase_aircraft))
                           : (r.circuit ? ("CIRCUIT  •  " + std::to_string(std::max(1, r.laps)) +
                                           (r.laps == 1 ? " LAP" : " LAPS"))
                                        : std::string("POINT-TO-POINT SPRINT"));
    meta += "  •  " + std::to_string(std::max(1, r.cars)) + (r.cars == 1 ? " DRIVER" : " DRIVERS");
    if (!r.band_name.empty()) meta += "  •  " + r.band_name;
    if (r.base_credits > 0) meta += "  •  1ST: " + thousands(r.base_credits) + " CR";
    TextStyle ms = font(ui::Font::kB, 13.0f * s, ui4::kOffWhite);
    fit(meta, ms, g.L(740) - g.L(96), 9.5f * s);
    const float mw = std::clamp(ui::List::measure(meta, ms) + 34.0f * s, 320.0f * s, 720.0f * s);
    ui4::pill(l, g.L(84), g.B(634 - lift), g.L(84) + mw, g.B(660 - lift), ui4::kCharcoalDark, ui4::PillKind::kHud01);
    l.rect(g.L(84), g.B(634 - lift), g.L(88), g.B(660 - lift), ui4::kYellow);
    l.text(g.L(98), g.B(640 - lift), meta, ms);
    l.unturn();
  }
  {
    l.turn(g.L(257), g.B(550 - lift), tilt_rad);
    TextStyle ts = font(ui::Font::kE, 32 * s, kText);
    fit(event_type, ts, g.L(440) - g.L(96), 16 * s);
    const float tw = ui::List::measure(event_type, ts);
    ui4::brush(l, g.L(72), g.B(522 - lift), std::max(g.L(380), g.L(96) + tw + 34 * s), g.B(570 - lift),
               ui4::kBrushBlack, false);
    l.text(g.L(96), g.B(530 - lift) + (32 * s - ts.size) * 0.5f, event_type, ts);
    l.unturn();
  }
  if (!r.grid_menu_open || r.grid_menu.empty()) return;

  // Tilted left-side FH1 Pre-Race Action Menu (preserving exact row indices & hit zones for mouse/pad/keyboard).
  const bool on_diff_row = r.grid_selected >= 0 && size_t(r.grid_selected) < r.grid_menu.size() &&
                           menu_item_is_difficulty(r.grid_menu[size_t(r.grid_selected)]);
  const bool on_quit_row = r.grid_selected >= 0 && size_t(r.grid_selected) < r.grid_menu.size() &&
                           menu_item_is_quit(r.grid_menu[size_t(r.grid_selected)]);
  l.turn(g.L(226), g.T(195), tilt_rad);
  for (size_t i = 0; i < r.grid_menu.size(); ++i) {
    const bool sel = int(i) == r.grid_selected;
    const bool is_diff = menu_item_is_difficulty(r.grid_menu[i]);
    const float bottom = g.T(204 + 54.0f * float(i));
    std::string label = r.grid_menu[i];
    if (is_diff) {
      label = "DIFFICULTY: " + diff.tier_name + "  +" + std::to_string(diff.bonus_pct) + "% CR";
    }
    TextStyle it = font(ui::Font::kE, 32 * s, kText);
    fit(label, it, g.L(480), 14 * s);
    const float tw = ui::List::measure(label, it);
    const float arrow_pad = is_diff ? 46.0f * s : 0.0f;
    const float bx0 = g.L(122);
    const float by0 = bottom - it.size - 8 * s;
    const float bx1 = std::max(g.L(340), g.L(152) + tw + arrow_pad + 34 * s);
    const float by1 = bottom + 7 * s;
    l.add_hit(bx0, by0, bx1, by1, ui::kHitRow, int(i));
    if (sel) {
      std::array<float, 4> clip_uv{};
      if (ui_texture("DIRTMASKS/TAPECLIPON02", clip_uv)) {
        l.image(bx0 - 24 * s, by0 + 3 * s, bx0 + 18 * s, by1 - 2 * s, clip_uv, ui4::kPink);
      }
      ui4::brush(l, bx0, by0, bx1, by1, ui4::kPink, true);
    } else {
      ui4::brush(l, bx0, by0, bx1, by1, ui4::kBrushBlack, false);
    }
    if (is_diff) {
      const float mid_y = 0.5f * (by0 + by1);
      ui4::opt_arrow(l, g.L(144), mid_y, 14.0f * s, -1, sel ? ui4::kYellow : ui4::kDim);
      l.text(g.L(162), bottom - it.size, label, it);
      ui4::opt_arrow(l, bx1 - 18.0f * s, mid_y, 14.0f * s, +1, sel ? ui4::kYellow : ui4::kDim);
    } else {
      l.text(g.L(152), bottom - it.size, label, it);
    }
  }
  l.unturn();

  // Right-hand FH1 Pre-Race Info Card (Event Details + Current Difficulty Bonus OR Starting Grid / Opponent Lineup).
  // Anchored neatly on the right edge (x=834..1224) so the hero car stays cleanly framed in center view!
  {
    const float cx0 = g.R(834);
    const float cx1 = g.R(1224);
    const float cy0 = g.T(96);
    const float card_h = on_diff_row ? 316.0f * s : (on_quit_row ? 214.0f * s : 374.0f * s);
    Turn rt(l, 0.5f * (cx0 + cx1), cy0 + 0.5f * card_h, tilt_rad * 0.55f);
    ui4::panel01(l, cx0, cy0, cx1, cy0 + card_h, s, ui4::kCharcoalDark, ui4::kPink);

    const std::string card_hdr = on_diff_row ? "DIFFICULTY & REWARDS"
                                 : on_quit_row ? "EVENT BRIEFING"
                                               : "STARTING GRID  •  LINEUP";
    ui4::brush(l, cx0 - 6.0f * s, cy0 + 10.0f * s, cx1 + 6.0f * s, cy0 + 42.0f * s, ui4::kPink, false);
    l.text(cx0 + 16.0f * s, cy0 + 16.0f * s, card_hdr, font(ui::Font::kE, 16.5f * s, kText));

    float y = cy0 + 54.0f * s;
    const float ix0 = cx0 + 16.0f * s;
    const float ix1 = cx1 - 16.0f * s;

    // Player car & difficulty bonus summary strip (always visible at top of right info card).
    if (!r.grid_car_info.empty()) {
      ui4::offwhite_strip(l, ix0, y, ix1, y + 26.0f * s, s, false);
      TextStyle cs = font(ui::Font::kE, 12.5f * s, ui4::kInk);
      cs.keep_case = true;
      fit(r.grid_car_info, cs, (ix1 - ix0) - 20.0f * s, 9.5f * s);
      l.text(ix0 + 10.0f * s, y + 6.5f * s, r.grid_car_info, cs);
      y += 34.0f * s;
    }

    if (on_diff_row) {
      // 5-tier AI Difficulty selector bar (EASY / MED / HARD / PRO / HARD+).
      l.text(ix0, y, "AI OPPONENT DIFFICULTY  (< / > TO ADJUST)", font(ui::Font::kB, 11.0f * s, ui4::kDim));
      y += 18.0f * s;
      static constexpr const char* kTiers[5] = {"EASY", "MED", "HARD", "PRO", "HARD+"};
      const float gap = 5.0f * s;
      const float tw = ((ix1 - ix0) - gap * 4.0f) / 5.0f;
      for (int ti = 0; ti < 5; ++ti) {
        const float tx0 = ix0 + float(ti) * (tw + gap);
        const bool active_t = (ti == diff.tier_index);
        l.rect(tx0, y, tx0 + tw, y + 26.0f * s, active_t ? ui4::kPink : ui4::kRowAlt);
        if (active_t) l.rect(tx0, y + 23.0f * s, tx0 + tw, y + 26.0f * s, ui4::kYellow);
        l.text(tx0 + 0.5f * tw, y + 6.5f * s, kTiers[ti],
               font(ui::Font::kE, 11.5f * s, active_t ? kText : ui4::kDim, Align::kCenter));
      }
      y += 38.0f * s;

      // Live CR Bonus Breakdown.
      auto kv_row = [&](const std::string& k, const std::string& v, uint32_t vcol, bool alt) {
        if (alt) l.rect(ix0, y - 3.0f * s, ix1, y + 22.0f * s, ui4::kRowAlt);
        l.text(ix0 + 8.0f * s, y + 2.0f * s, k, font(ui::Font::kB, 12.0f * s, ui4::kOffWhite));
        l.text(ix1 - 8.0f * s, y + 1.0f * s, v, font(ui::Font::kE, 13.5f * s, vcol, Align::kRight));
        y += 26.0f * s;
      };
      kv_row("AI DIFFICULTY BONUS", "+" + std::to_string(diff.bonus_pct) + "% CR", ui4::kYellow, false);
      kv_row("DRIVING ASSISTS BONUS",
             r.assists_pct > 0 ? ("+" + std::to_string(r.assists_pct) + "% CR") : "STANDARD (0%)",
             r.assists_pct > 0 ? ui4::kGreen : ui4::kDim, true);
      if (r.base_credits > 0) {
        kv_row("1ST PLACE BASE PURSE", thousands(r.base_credits) + " CR", kText, false);
      }
      y += 6.0f * s;
      ui4::brush(l, ix0 - 2.0f * s, y, ix1 + 2.0f * s, y + 34.0f * s, ui4::kBrushBlack, false);
      l.rect(ix0 - 2.0f * s, y, ix0 + 3.0f * s, y + 34.0f * s, ui4::kYellow);
      l.text(ix0 + 12.0f * s, y + 9.0f * s, "TOTAL REWARD BONUS", font(ui::Font::kE, 13.0f * s, kText));
      l.text(ix1 - 12.0f * s, y + 7.0f * s, "+" + std::to_string(total_bonus_pct) + "% CR",
             font(ui::Font::kE, 18.0f * s, ui4::kYellow, Align::kRight));
    } else if (on_quit_row) {
      l.text(ix0, y, "EVENT: " + event_title, font(ui::Font::kE, 14.0f * s, kText));
      y += 24.0f * s;
      l.text(ix0, y, "DIFFICULTY BONUS: +" + std::to_string(total_bonus_pct) + "% CR (" + diff.tier_name + ")",
             font(ui::Font::kB, 12.0f * s, ui4::kYellow));
      y += 28.0f * s;
      l.rect(ix0, y, ix1, y + 1.0f * s, rgba(255, 255, 255, 30));
      y += 12.0f * s;
      l.text(ix0, y, "EXIT BACK TO COLORADO FREE ROAM", font(ui::Font::kB, 12.5f * s, ui4::kOffWhite));
      l.text(ix0, y + 19.0f * s, "NO WRISTBAND OR CREDIT PENALTY", font(ui::Font::kB, 11.0f * s, ui4::kDim));
    } else {
      // Starting Grid / Opponent Lineup + Difficulty Bonus summary chip!
      ui4::pill(l, ix0, y, ix1, y + 24.0f * s, ui4::kBrushBlack, ui4::PillKind::kHud01);
      l.text(ix0 + 10.0f * s, y + 5.0f * s,
             r.circuit ? ("CIRCUIT  •  " + std::to_string(std::max(1, r.laps)) + " LAPS")
                       : (r.showcase ? "HORIZON SHOWCASE" : "POINT-TO-POINT"),
             font(ui::Font::kB, 11.5f * s, ui4::kOffWhite));
      l.text(ix1 - 10.0f * s, y + 4.5f * s, diff.tier_name + "  +" + std::to_string(total_bonus_pct) + "% CR",
             font(ui::Font::kE, 12.0f * s, ui4::kYellow, Align::kRight));
      y += 30.0f * s;

      // Column headers for Starting Grid.
      l.text(ix0 + 4.0f * s, y, "POS", font(ui::Font::kB, 10.5f * s, ui4::kDim));
      l.text(ix0 + 36.0f * s, y, "DRIVER", font(ui::Font::kB, 10.5f * s, ui4::kDim));
      l.text(ix1 - 4.0f * s, y, "CLASS / PI", font(ui::Font::kB, 10.5f * s, ui4::kDim, Align::kRight));
      y += 16.0f * s;

      if (r.showcase) {
        const float rh = 34.0f * s;
        l.rect(ix0, y, ix1, y + rh, ui4::kRowHighlight);
        l.rect(ix0, y, ix0 + 4.0f * s, y + rh, ui4::kPink);
        l.text(ix0 + 10.0f * s, y + 8.0f * s, "1", font(ui::Font::kE, 14.0f * s, ui4::kYellow));
        l.text(ix0 + 36.0f * s, y + 9.0f * s, "YOU (LOANER SHOWCASE CAR)", font(ui::Font::kE, 12.5f * s, kText));
        y += rh + 6.0f * s;
        l.rect(ix0, y, ix1, y + rh, ui4::kRowAlt);
        l.rect(ix0, y, ix0 + 4.0f * s, y + rh, ui4::kAmber);
        l.text(ix0 + 10.0f * s, y + 8.0f * s, "VS", font(ui::Font::kE, 13.0f * s, ui4::kAmber));
        l.text(ix0 + 36.0f * s, y + 9.0f * s,
               r.showcase_aircraft.empty() ? std::string("SHOWCASE CHALLENGER") : r.showcase_aircraft,
               font(ui::Font::kE, 12.5f * s, ui4::kYellow));
      } else if (!r.rows.empty()) {
        const size_t n_show = std::min<size_t>(r.rows.size(), 8);
        const float rh = 27.0f * s;
        for (size_t k = 0; k < n_show; ++k) {
          const auto& row = r.rows[k];
          const uint32_t bg = row.player ? ui4::kRowHighlight : ((k % 2 == 1) ? ui4::kRowAlt : rgba(20, 22, 28, 170));
          l.rect(ix0, y, ix1, y + rh - 2.0f * s, bg);
          if (row.player) l.rect(ix0, y, ix0 + 4.0f * s, y + rh - 2.0f * s, ui4::kPink);
          l.text(ix0 + 10.0f * s, y + 5.5f * s, std::to_string(row.place > 0 ? row.place : int(k + 1)),
                 font(ui::Font::kE, 12.5f * s, row.player ? ui4::kYellow : ui4::kOffWhite));
          TextStyle dts = font(row.player ? ui::Font::kE : ui::Font::kB, 12.0f * s, row.player ? kText : ui4::kOffWhite);
          dts.keep_case = true;
          fit(row.name, dts, (ix1 - ix0) - 122.0f * s, 9.0f * s);
          l.text(ix0 + 36.0f * s, y + 6.0f * s, row.name, dts);
          if (!row.car_class.empty() && row.pi > 0) {
            ui4::class_badge(l, ix1 - 62.0f * s, y + 3.0f * s, 19.0f * s, row.car_class, row.pi);
          }
          y += rh;
        }
      } else {
        l.text(ix0 + 8.0f * s, y + 8.0f * s,
               std::to_string(std::max(1, r.cars)) + " COMPETITORS ON STARTING GRID",
               font(ui::Font::kB, 12.5f * s, ui4::kOffWhite));
      }
    }
  }

  std::vector<ui4::Prompt> prompts = {{"A", str("HelpButtons", "IDS_Select", "SELECT")}};
  if (on_diff_row) prompts.push_back({"DPAD_LR", "ADJUST DIFFICULTY"});
  prompts.push_back({"B", "QUIT RACE"});
  ui4::button_bar(l, prompts, g.w, g.h);
}

// (b2) The race HUD: laps (or the distance), the timers, place, standings.
void race_hud(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  const bool racing = r.phase == RaceHud::kRacing;
  // 947_HUD's EndConditionBlock and PlaceBlock (E 48 current, E 26 slash
  // and total, E 16 label; right-aligned boxes end at their node); the
  // drop shadow is ours. Without the scene: the drawn blocks below.
  // The blocks from 947_HUD (hud_screen.h); the drawn ones below without it.
  const bool scene_blocks = draw_hud_race(l, r, g.w, g.h, s);
  const SceneDraw sc = resolve_scene_draw("947_HUD");
  const int endcon = sc.find("EndConditionBlock"), placeblk = sc.find("PlaceBlock");
  const bool scened = endcon >= 0 && placeblk >= 0;
  auto stext = [&](const SceneFrame& f, int node, const std::string& t) {
    sc.text(l, f, node, t, rgba(0, 0, 0, 140), 1.5f, 1.5f, false);
    sc.text(l, f, node, t, kText, 0, 0, false);
  };
  if (scened && !scene_blocks) {
    const SceneFrame tl{0, 0, s}, tr{g.w - 1280 * s, 0, s};
    if (r.circuit) {
      stext(tl, sc.find("TEXT_ENDCON_CURRENT", endcon), std::to_string(racing ? std::max(1, r.lap) : 0));
      stext(tl, sc.find("TEXT_ENDCON_SLASH", endcon), "/");
      stext(tl, sc.find("TEXT_ENDCON_FINAL", endcon), std::to_string(r.laps));
      stext(tl, sc.find("TEXT_ENDCON_UNITS", endcon), str("InGame", "IDS_Laps", "LAPS"));
    } else {
      stext(tl, sc.find("TEXT_ENDCON_CURRENT", endcon), std::to_string(int(std::lround(r.progress * 100))));
      stext(tl, sc.find("TEXT_ENDCON_UNITS", endcon), "%");
    }
    stext(tr, sc.find("TEXT_PLACE_LABEL", placeblk), str("InGame", "IDS_Place", "PLACE"));
    stext(tr, sc.find("TEXT_PLACE", placeblk), std::to_string(std::max(1, r.place)));
    stext(tr, sc.find("TEXT_PLACE_SLASH", placeblk), "/");
    stext(tr, sc.find("TEXT_PLACE_MAX", placeblk), std::to_string(r.cars));
  }
  // Laps: "1/3 LAPS"; sprints: the distance done.
  if (!scened) {
    char big[16], of[16];
    if (r.circuit) {
      std::snprintf(big, sizeof big, "%d", racing ? std::max(1, r.lap) : 0);
      std::snprintf(of, sizeof of, "/%d", r.laps);
    } else {
      std::snprintf(big, sizeof big, "%d", int(std::lround(r.progress * 100)));
      std::snprintf(of, sizeof of, "%%");
    }
    const float bw = shadowed(l, g.L(70), g.T(34), big, font(ui::Font::kE, 48 * s, kText), s);
    const float ow = shadowed(l, std::max(g.L(118), g.L(70) + bw + 1 * s), g.T(33), of,
                              font(ui::Font::kE, 27 * s, kText), s);
    if (r.circuit)
      shadowed(l, std::max(g.L(182), g.L(70) + bw + ow + 18 * s), g.T(34), str("InGame", "IDS_Laps", "LAPS"),
               font(ui::Font::kE, 16 * s, kText), s);
  }
  // The timers: RACE from the start, LAP on circuits, LAST and BEST after a lap.
  if (!scene_blocks) {
    struct Row { std::string name; double t; };
    std::vector<Row> rows;
    rows.push_back({str("InGame", "IDS_Race", "RACE"), std::max(0.0, r.clock)});
    if (r.circuit) {
      rows.push_back({str("InGame", "IDS_CurrentLapTimeLabel", "LAP"), std::max(0.0, r.lap_time)});
      if (r.last_lap > 0) rows.push_back({str("InGame", "IDS_Last", "LAST"), r.last_lap});
      if (r.best_lap > 0) rows.push_back({str("InGame", "IDS_Best", "BEST"), r.best_lap});
    }
    const float ink[] = {115, 143, 171, 198};
    for (size_t i = 0; i < rows.size() && i < 4; ++i) {
      const float y = g.T(ink[i]);
      pill(l, g.L(64), y - 5 * s, g.L(224), y + 19 * s, kPillGrey, true);
      l.text(g.L(74), y, rows[i].name, font(ui::Font::kB, 14 * s, kText));
      l.text(g.L(215), y, clock_ms(rows[i].t), font(ui::Font::kB, 14 * s, kText, Align::kRight));
    }
    // Live lap split delta display (-0.42s green / +0.31s red).
    const bool derived_delta =
        !r.has_lap_split_delta && r.circuit && r.best_lap > 0.0 && r.last_lap > 0.0 &&
        std::fabs(r.last_lap - r.best_lap) > 0.004;
    if (racing && (r.has_lap_split_delta || derived_delta)) {
      const double delta = r.has_lap_split_delta ? r.lap_split_delta : (r.last_lap - r.best_lap);
      const uint32_t dcol = delta <= 0.0 ? ui4::kGreen : ui4::kRed;
      const float dy = g.T(ink[std::min<size_t>(rows.size(), 3)] + (rows.size() >= 4 ? 28.0f : 0.0f));
      pill(l, g.L(64), dy - 5 * s, g.L(224), dy + 19 * s, ui4::kBrushBlack, true);
      l.rect(g.L(64), dy - 5 * s, g.L(68), dy + 19 * s, dcol);
      l.text(g.L(74), dy, "SPLIT", font(ui::Font::kB, 13.0f * s, ui4::kOffWhite));
      l.text(g.L(215), dy, format_split_delta(delta), font(ui::Font::kE, 14.0f * s, dcol, Align::kRight));
    }
  }
  // Place: "PLACE 3/8".
  if (!scened) {
    char p[8], of[8];
    std::snprintf(p, sizeof p, "%d", std::max(1, r.place));
    std::snprintf(of, sizeof of, "/%d", r.cars);
    shadowed(l, g.R(1024), g.T(34), str("InGame", "IDS_Place", "PLACE"), font(ui::Font::kE, 16 * s, kText), s);
    const float ow = shadowed(l, g.R(1217), g.T(33), of, font(ui::Font::kE, 27 * s, kText, Align::kRight), s);
    shadowed(l, g.R(1217) - ow - 1 * s, g.T(32), p, font(ui::Font::kE, 50 * s, kText, Align::kRight), s);
  }
  // The standings: the leader and the rows round the player, four in all.
  if (!scene_blocks && !r.rows.empty()) {
    std::vector<size_t> pick;
    const size_t n = r.rows.size();
    size_t me = 0;
    for (size_t i = 0; i < n; ++i)
      if (r.rows[i].player) me = i;
    if (n <= 4 || me < 3) {
      for (size_t i = 0; i < std::min<size_t>(n, 4); ++i) pick.push_back(i);
    } else {
      const size_t last = std::min(n - 1, me + 1);
      pick = {0, last - 2, last - 1, last};
    }
    const float ty[] = {116, 143, 172, 200};
    bool any_gap = false;
    for (size_t idx : pick) {
      if (r.rows[idx].gap > 0.05) any_gap = true;
    }
    const float pill_left = any_gap ? g.R(984) : g.R(1043);
    for (size_t k = 0; k < pick.size(); ++k) {
      const RaceHudRow& row = r.rows[pick[k]];
      const float y = g.T(ty[k]);
      // 947_HUD/PositionIndicators: HUDPILL01 per row, driver name in B right-aligned
      // to 1181, place in E right-aligned to 1206, optional TEXT_TIME gap column on left.
      pill(l, pill_left, y - 5 * s, g.R(1213), y + 19 * s, row.player ? kPinkRow : kPillGrey);
      char pos[8];
      std::snprintf(pos, sizeof pos, "%d", row.place);
      l.text(g.R(1206), y - 0.5f * s, pos, font(ui::Font::kE, 15 * s, kText, Align::kRight));
      if (any_gap && row.gap > 0.05) {
        l.text(pill_left + 8 * s, y + 1.5f * s, gap_ms(row.gap),
               font(ui::Font::kB, 11.5f * s, row.player ? ui4::kWhite : ui4::kDim));
      }
      TextStyle ns = font(ui::Font::kB, 14 * s, kText, Align::kRight);
      ns.keep_case = true;
      fit(row.name, ns, g.R(1181) - (any_gap ? (pill_left + 64 * s) : g.R(1052)), 9 * s);
      l.text(g.R(1181), y + (14 * s - ns.size) * 0.5f, row.name, ns);
    }
  }
  // 947_HUD/I_321_Go: pre-race 3-2-1 countdown digits and GO! signal.
  {
    const SceneFrame cf{0.5f * g.w - 640 * s, 0.5f * g.h - 360 * s, s};
    const int go_root = sc.find("I_321_Go");
    const int go_node = sc.find("TEXT_GO", go_root);
    if (r.phase == RaceHud::kCountdown) {
      int digit = r.digit;
      if (digit <= 0 && r.clock < 0.0 && r.clock >= -3.0) {
        digit = int(std::ceil(-r.clock));
      }
      if (digit >= 1 && digit <= 3) {
        // Fractional progress [0, 1] within the current countdown second for pop-in / fade-out.
        const float u = (r.clock < 0.0 && r.clock >= -3.0) ? clamp01(double(digit) + r.clock) : 0.25f;
        const float in_a = ease_in_time(u, 0.0, 0.12);
        const float out_a = 1.0f - clamp01((u - 0.72) / 0.28);
        const float a = in_a * out_a;
        const float pulse = 1.0f + 0.30f * (1.0f - ease_in_time(u, 0.0, 0.22));
        if (a > 1e-3f) {
          char dtxt[4];
          std::snprintf(dtxt, sizeof dtxt, "%d", digit);
          const char* digit_node_name = digit == 3 ? "TEXT_3" : digit == 2 ? "TEXT_2" : "TEXT_1";
          int anchor_node = sc.find(digit_node_name, go_root);
          if (anchor_node < 0) anchor_node = go_node;

          float cx = g.C(637.5f);
          float cy = g.T(262.0f);
          TextStyle ts = font(ui::Font::kE, 84.0f * s * pulse, ui::fade(kText, a), Align::kCenter);
          if (anchor_node >= 0) {
            const auto [nx, ny] = sc.at(anchor_node);
            cx = cf.x(nx);
            cy = cf.y(ny);
            ts = sc.style(anchor_node, cf, ui::fade(kText, a));
            ts.align = Align::kCenter;
            ts.size = std::max(ts.size, 76.0f * s) * pulse;
          }
          const float top_y = cy - 0.5f * ts.size;
          // Horizon pink pop-in glow + crisp drop shadow + bold white countdown digit.
          TextStyle glow = ts;
          glow.color = ui::fade(kPink, a * (0.45f + 0.35f * (1.0f - u)));
          l.text(cx, top_y + 3.0f * s, dtxt, glow);
          TextStyle sh = ts;
          sh.color = ui::fade(kSoft, a);
          l.text(cx + 2.5f * s, top_y + 2.5f * s, dtxt, sh);
          l.text(cx, top_y, dtxt, ts);
        }
      }
    } else if (racing && r.clock >= 0.0 && r.clock < 0.80) {
      // R3-UI-11: Scale-pop animated "GO!" banner fading cleanly over 0.80 s after race start.
      const float in_u = ease_in_time(r.clock, 0.0, 0.18);
      const float out_a = 1.0f - clamp01((r.clock - 0.45) / 0.35);
      const float a = ease_in_time(r.clock, 0.0, 0.06) * out_a;
      const float pop_scale = (r.clock < 0.18) ? (1.45f - 0.41f * in_u) : (1.04f + 0.14f * clamp01((r.clock - 0.18) / 0.62));
      if (a > 1e-3f) {
        float cx = g.C(637.5f);
        float cy = g.T(252.0f);
        if (go_node >= 0) {
          const auto [nx, ny] = sc.at(go_node);
          cx = cf.x(nx);
          cy = cf.y(ny);
        }
        const std::string go_txt = str("InGame", "IDS_3_2_1_GO", "GO!");
        TextStyle gts = font(ui::Font::kE, 74.0f * s * pop_scale, ui::fade(kText, a), Align::kCenter);
        const float tw = ui::List::measure(go_txt, gts);
        {
          Turn gt(l, cx, cy, ui4::kTiltRad * 0.55f);
          const float bw = std::max(176.0f * s, tw + 66.0f * s);
          const float bh = 68.0f * s * pop_scale;
          ui4::brush(l, cx - 0.5f * bw, cy - 0.5f * bh, cx + 0.5f * bw, cy + 0.5f * bh, ui::fade(kPink, a * 0.92f),
                     true);
        }
        const float top_y = cy - 0.52f * gts.size;
        TextStyle glow = gts;
        glow.color = ui::fade(ui4::kYellow, a * 0.42f);
        l.text(cx, top_y + 3.0f * s, go_txt, glow);
        TextStyle sh = gts;
        sh.color = ui::fade(kSoft, a);
        l.text(cx + 2.5f * s, top_y + 2.5f * s, go_txt, sh);
        l.text(cx, top_y, go_txt, gts);
      }
    }
  }
  // R3-UI-04: Central FH1 pink/amber "MISSED CHECKPOINT" / "RETURNING TO TRACK IN X.X" warning + [Y] Rewind prompt.
  if (racing && r.missed_checkpoint) {
    const float pulse = 0.72f + 0.28f * float(std::sin(r.clock * 6.2831853 * 3.5));
    const float cx = 0.5f * g.w;
    const float wy0 = g.T(142);
    const float bw = 420.0f * s;
    const float x0 = cx - 0.5f * bw, x1 = cx + 0.5f * bw;
    {
      Turn mt(l, cx, wy0 + 38.0f * s, ui4::kTiltRad * 0.35f);
      // Top pink/amber brush warning header.
      ui4::brush(l, x0, wy0, x1, wy0 + 42.0f * s, ui::fade(ui4::kPink, 0.88f + 0.12f * pulse), true);
      l.rect(x0, wy0, x0 + 6.0f * s, wy0 + 42.0f * s, ui4::kAmber);
      l.rect(x1 - 6.0f * s, wy0, x1, wy0 + 42.0f * s, ui4::kAmber);
      shadowed(l, cx, wy0 + 8.0f * s, "MISSED CHECKPOINT",
               font(ui::Font::kE, 25.0f * s, ui4::kWhite, Align::kCenter), s);

      // Sub-plate: "RETURNING TO TRACK IN X.X" + live [Y] REWIND glyph prompt + draining countdown bar.
      const float sy0 = wy0 + 44.0f * s, sy1 = sy0 + 36.0f * s;
      l.rect(x0 + 12.0f * s, sy0, x1 - 12.0f * s, sy1, rgba(12, 14, 20, 232));
      l.rect(x0 + 12.0f * s, sy0, x0 + 16.0f * s, sy1, ui4::kAmber);
      char cbuf[64];
      std::snprintf(cbuf, sizeof cbuf, "RETURNING TO TRACK IN %.1f", std::max(0.0, r.missed_checkpoint_countdown_s));
      l.text(x0 + 28.0f * s, sy0 + 9.0f * s, cbuf, font(ui::Font::kE, 14.0f * s, ui4::kAmber));

      // [Y] Rewind prompt on right side of banner.
      const std::string rw_lbl = str("HelpButtons", "IDS_Mulligan", "REWIND");
      const TextStyle rts = font(ui::Font::kE, 13.5f * s, ui4::kWhite, Align::kRight);
      const float rw_w = ui::List::measure(rw_lbl, rts);
      l.text(x1 - 24.0f * s, sy0 + 9.5f * s, rw_lbl, rts);
      ui4::button_glyph(l, x1 - 24.0f * s - rw_w - 14.0f * s, sy0 + 16.5f * s, 9.5f * s, "Y");

      // Draining countdown progress bar along bottom edge of warning card.
      const float frac = float(std::clamp(r.missed_checkpoint_countdown_s / MissedCheckpointTracker::kCountdownSec, 0.0, 1.0));
      const float bx0 = x0 + 16.0f * s, bx1 = x1 - 12.0f * s;
      l.rect(bx0, sy1 - 4.0f * s, bx1, sy1, rgba(255, 255, 255, 36));
      if (frac > 0.005f) {
        l.rect(bx0, sy1 - 4.0f * s, bx0 + (bx1 - bx0) * frac, sy1, ui4::kAmber);
      }
    }
  }
  // R3-UI-11: Pulsing "WRONG WAY" warning icon + text (always shows both icon and label).
  if (racing && r.wrong_way) {
    const float pulse = 0.70f + 0.30f * float(std::cos(r.clock * 6.2832 * 2.2));
    const float y_off = r.missed_checkpoint ? 86.0f * s : 0.0f;
    const float cx = 0.5f * g.w;
    std::array<float, 4> ww_uv{};
    const float icon_y0 = g.T(162) + y_off;
    const float icon_sz = 84.0f * s;
    if (ui_texture("HUD/WRONGWAYICON", ww_uv)) {
      l.image(cx - 0.5f * icon_sz, icon_y0, cx + 0.5f * icon_sz, icon_y0 + icon_sz, ww_uv,
              ui::fade(rgba(255, 255, 255), pulse));
    } else {
      const float icx = cx, icy = icon_y0 + 0.5f * icon_sz;
      l.circle(icx, icy, 34.0f * s, ui::fade(kRed, pulse));
      l.arc(icx, icy, 31.0f * s, 4.0f * s, 0.0f, 6.28318f, ui::fade(ui4::kWhite, pulse));
      l.rect(icx - 19.0f * s, icy - 5.5f * s, icx + 19.0f * s, icy + 5.5f * s, ui::fade(ui4::kWhite, pulse));
    }
    // No string table holds a WRONG WAY label: the game's indicator is the
    // I_HUD_WrongWaySign texture (ui/Textures.zip), so this text is ours.
    const std::string ww_txt = std::string("WRONG WAY");
    const TextStyle ws = font(ui::Font::kE, 26.0f * s, ui::fade(kText, 0.85f + 0.15f * pulse), Align::kCenter);
    const float ww = std::max(220.0f * s, ui::List::measure(ww_txt, ws) + 54.0f * s);
    const float wy = icon_y0 + icon_sz + 4.0f * s;
    Turn wt(l, cx, wy + 20.0f * s, ui4::kTiltRad * 0.35f);
    ui4::brush(l, cx - 0.5f * ww, wy, cx + 0.5f * ww, wy + 40.0f * s, ui::fade(kRed, 0.85f * pulse));
    shadowed(l, cx, wy + 7.0f * s, ww_txt, ws, s);
  }
}

// (c) After the finish: the FINISHED banner with the place, then the cash
// reward page.

// The RACE RESULTS table (SPEC-UI-LAYOUTS c, the race results table; X
// from the FINISHED banner, B back): H_POST_RACE_RESULTS as laid out, each
// node turned by its own rotation; the rows' text from the session. The
// scene's opacities are its intro's start (0): drawn at full.
void results_table(ui::List& l, const RaceHud& r, const Layout& g) {
  const SceneDraw sc = resolve_scene_draw("H_POST_RACE_RESULTS", true);
  const float s = g.s;
  const std::string event_title = resolve_hud_event_title(r);
  ui4::button_bar(l, {{"B", str("HelpButtons", "IDS_Back", "BACK")}}, g.w, g.h);
  if (!sc.ok()) {
    // Crisp FH1 tilted fallback leaderboard when H_POST_RACE_RESULTS scene is unavailable.
    ui4::subtle_vignette(l, g.w, g.h, 0.36f);
    draw_race_post_stage_pills(l, g, 0);
    const float tx0 = g.L(88), ty0 = g.T(84), tw = g.R(1192) - tx0;
    Turn t(l, tx0 + 0.5f * tw, ty0 + 220.0f * s, ui4::kTiltRad * 0.45f);
    ui4::title_banner(l, s, tx0, ty0, str("ScreenTitles", "IDS_PostRace_RaceResults", "RACE RESULTS"),
                      (!r.event_type.empty() ? r.event_type : std::string("FESTIVAL RACE")) + "  •  " + event_title,
                      true, tw);
    const float py0 = ty0 + 78.0f * s;
    const size_t nrows = std::min<size_t>(r.rows.size(), 8);
    const float row_h = 38.0f * s;
    const float panel_h = 46.0f * s + float(std::max<size_t>(nrows, 1)) * row_h + 14.0f * s;
    ui4::panel(l, tx0, py0, tx0 + tw, py0 + panel_h, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    ui4::offwhite_strip(l, tx0 + 10.0f * s, py0 + 8.0f * s, tx0 + tw - 10.0f * s, py0 + 38.0f * s, s, false);
    const float c_pos = tx0 + 24.0f * s;
    const float c_drv = tx0 + 84.0f * s;
    const float c_cls = tx0 + 350.0f * s;
    const float c_car = tx0 + 440.0f * s;
    const float c_lap = tx0 + tw - 210.0f * s;
    const float c_tot = tx0 + tw - 22.0f * s;
    const TextStyle hs = font(ui::Font::kE, 13.0f * s, ui4::kInk);
    l.text(c_pos, py0 + 16.0f * s, "POS", hs);
    l.text(c_drv, py0 + 16.0f * s, "DRIVER", hs);
    l.text(c_cls, py0 + 16.0f * s, "CLASS", hs);
    l.text(c_car, py0 + 16.0f * s, "CAR", hs);
    l.text(c_lap, py0 + 16.0f * s, "BEST LAP", font(ui::Font::kE, 13.0f * s, ui4::kInk, Align::kRight));
    l.text(c_tot, py0 + 16.0f * s, "TIME / GAP", font(ui::Font::kE, 13.0f * s, ui4::kInk, Align::kRight));

    float ry = py0 + 46.0f * s;
    for (size_t i = 0; i < nrows; ++i) {
      const RaceHudRow& row = r.rows[i];
      const uint32_t bg = row.player ? ui4::kRowHighlight : ((i % 2 == 1) ? ui4::kRowAlt : rgba(20, 22, 28, 190));
      l.rect(tx0 + 10.0f * s, ry, tx0 + tw - 10.0f * s, ry + row_h - 4.0f * s, bg);
      if (row.player) l.rect(tx0 + 10.0f * s, ry, tx0 + 15.0f * s, ry + row_h - 4.0f * s, ui4::kPink);
      l.text(c_pos, ry + 8.0f * s, std::to_string(row.place),
             font(ui::Font::kE, 16.0f * s, row.place == 1 ? ui4::kYellow : kText));
      TextStyle ds = font(row.player ? ui::Font::kE : ui::Font::kB, 14.5f * s, kText);
      ds.keep_case = true;
      fit(row.name, ds, (c_cls - c_drv) - 16.0f * s, 10.0f * s);
      l.text(c_drv, ry + 9.0f * s, row.name, ds);
      if (!row.car_class.empty() && row.pi >= 0) {
        ui4::class_badge(l, c_cls, ry + 6.0f * s, 22.0f * s, row.car_class, row.pi);
      }
      TextStyle cs = font(ui::Font::kB, 13.5f * s, ui4::kOffWhite);
      cs.keep_case = true;
      fit(row.car, cs, (c_lap - 110.0f * s) - c_car, 10.0f * s);
      l.text(c_car, ry + 9.5f * s, row.car, cs);
      l.text(c_lap, ry + 9.0f * s, row.best_lap > 0 ? clock_ms(row.best_lap) : "--:--.---",
             font(ui::Font::kB, 13.0f * s, ui4::kDim, Align::kRight));
      const std::string tstr = row.finished ? clock_ms(row.time)
                                            : (row.gap > 0.05 ? gap_ms(row.gap) : std::string("--:--.---"));
      l.text(c_tot, ry + 8.5f * s, tstr,
             font(ui::Font::kE, 14.0f * s, row.place == 1 ? ui4::kYellow : kText, Align::kRight));
      ry += row_h;
    }
    return;
  }
  const SceneFrame f{0.5f * g.w - 640 * s, 0.5f * g.h - 360 * s, s};
  {
    SceneBindings bgb;
    bgb.ignore_zero_alpha = true;
    bgb.opacity = 0.5f;
    sc.draw_subtree(l, f, "BG_Noise", bgb);
  }
  // The title on its tape: the tapes cropped to the text (ours: a 40 px
  // margin past the text, as the frame shows).
  const int tape = sc.find("TitleTape");
  {
    const int tt = sc.find("TitleTapeText", tape);
    const std::string title = str("ScreenTitles", "IDS_PostRace_RaceResults", "RACE RESULTS");
    const float tw = ui::List::measure(title, sc.style(tt, f, kText)) / s;
    const float tx = sc.at(tt)[0];
    sc.model(l, f, sc.find("Clipon_2", tape));
    for (const char* name : {"Smudge_Tape", "Crisp_Tape"}) {
      const int n = sc.find(name, tape);
      sc.model(l, f, n, 1, 0, 0, tx + tw + 40 - sc.rect(n)[0]);
    }
    sc.text(l, f, tt, title, kText);
  }
  // The heading strip.
  const int head = sc.find("Heading");
  sc.model(l, f, sc.find("Pill_left", head));
  sc.model(l, f, sc.find("Pill_right", head));
  const std::pair<const char*, std::string> heads[] = {
      {"TEXT_POSITION", str("Scoreboard", "IDS_Pos", "POS")},
      {"TEXT_PLAYER", str("Scoreboard", "IDS_Driver", "DRIVER")},
      {"TEXT_CLASS", str("Scoreboard", "IDS_Class", "CLASS")},
      {"TEXT_CAR", str("Scoreboard", "IDS_Car", "CAR")},
      {"TEXT_BEST_LAP", str("InGame", "IDS_Header_Best_Lap", "BEST LAP")},
      {"TEXT_TOTAL_TIME", str("InGame", "IDS_Header_Total", "TOTAL")}};
  for (const auto& [node, text] : heads) {
    const int n = sc.find(node, head);
    // Upper case, as the frame shows.
    std::string up = text;
    for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    sc.text(l, f, n, up, kDark);
  }
  // The rows: I_LIST_ITEM, I_LIST_ITEM2 ... I_LIST_ITEM8, by place.
  for (size_t i = 0; i < r.rows.size() && i < 8; ++i) {
    const RaceHudRow& row = r.rows[i];
    const int item = sc.find(i == 0 ? std::string("I_LIST_ITEM") : "I_LIST_ITEM" + std::to_string(i + 1));
    if (item < 0) break;
    // The player's row on a lighter grey (measured, about 0.24).
    const uint32_t tint = row.player ? rgba(61, 61, 61) : 0;
    const int pill = sc.find("Pill", item);
    sc.model(l, f, sc.find("Pill_left", pill), 1, 0, 0, 0, tint);
    sc.model(l, f, sc.find("Pill_right", pill), 1, 0, 0, 0, tint);
    sc.text(l, f, sc.find("TEXT_POSITION", item), std::to_string(row.place), kText);
    sc.text(l, f, sc.find("TEXT_PLAYER", item), row.name, kText);
    sc.text(l, f, sc.find("TEXT_CAR", item), row.car, kText);
    sc.text(l, f, sc.find("TEXT_BEST_LAP", item), row.best_lap > 0 ? clock_ms(row.best_lap) : "--:--.---", kText);
    sc.text(l, f, sc.find("TEXT_TOTAL_TIME", item), row.finished ? clock_ms(row.time) : "--:--.---", kText);
    // The class badge: PICLASS/CLASS_<class> at CLASS_A's place, scaled by
    // its CLASS_SM group (0.75); the index on its white part.
    const int badge = sc.find("CLASS_A", item);
    std::array<float, 4> uv;
    if (!row.car_class.empty() && badge >= 0 && ui_texture("PICLASS/CLASS_" + row.car_class, uv)) {
      const auto b = sc.rect(badge);
      const float cx = 0.5f * (b[0] + b[2]), cy = 0.5f * (b[1] + b[3]);
      // (The frame's badge is CLASS_A's full rect, about 52 px of ink.)
      const float hw = 0.5f * (b[2] - b[0]), hh = 0.5f * (b[3] - b[1]);
      const float angle = -sc.angle(badge);
      l.image_rotated(f.x(cx), f.y(cy), hw * s, hh * s, angle, uv);
      if (row.pi >= 0) sc.text(l, f, sc.find("TEXT_PI", item), std::to_string(row.pi), kDark);
    }
  }
}

namespace {

struct PostRacePageTracker {
  bool active = false;
  int last_page = -1;
  double last_finish_time = -1e9;
  double entered_at = 0.0;
};

PostRacePageTracker& page_tracker() {
  static PostRacePageTracker t;
  return t;
}

// Crisp FH1 Career / Driver Level & Manufacturer Affinity XP breakdown panel
// (rendered on the right half of Page 2 Points Earned, or full-screen on Page 4).
void career_affinity_panel(ui::List& l, const RaceHud& r, const DriveHud& st, const Layout& g, float x0, float y0,
                           float x1, double page_elapsed, bool include_won_car_badge) {
  const float s = g.s;
  const int drv_lvl = std::max(1, r.driver_level > 0 ? r.driver_level : st.driver_level);
  const int drv_xp_tot = r.driver_xp_total > 0 ? r.driver_xp_total : st.driver_xp;
  const int drv_next = r.driver_next_xp > 0 ? r.driver_next_xp
                       : st.driver_next_xp > 0 ? st.driver_next_xp
                                               : std::max(1000, drv_lvl * 5000);
  const int aff_lvl = std::max(0, r.affinity_level > 0 ? r.affinity_level : st.affinity_level);
  const int aff_xp_tot = r.affinity_xp_total;
  const int aff_next = r.affinity_next_xp > 0 ? r.affinity_next_xp : std::max(500, (aff_lvl + 1) * 2500);
  const double aff_disc = r.affinity_discount_pct > 0.0 ? r.affinity_discount_pct : st.affinity_discount_pct;
  const std::string gift_car = !r.gift_car_name.empty() ? r.gift_car_name
                               : (include_won_car_badge ? r.won_car_name : std::string{});

  const float anim = ease_in_time(page_elapsed, 0.25, 1.0);
  const int shown_drv_gain = int(std::llround(double(std::max(0, r.driver_xp)) * anim));
  const int shown_aff_gain = int(std::llround(double(std::max(0, r.affinity_xp)) * anim));
  const int64_t shown_aff_cr = int64_t(std::llround(double(std::max<int64_t>(0, r.affinity_bonus_credits)) * anim));

  const float card_h = (gift_car.empty() ? 276.0f : 344.0f) * s;
  const float y1 = y0 + card_h;
  Turn t(l, 0.5f * (x0 + x1), 0.5f * (y0 + y1), ui4::kTiltRad * 0.65f);
  ui4::panel01(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kPink);

  // Header brush strip.
  ui4::brush(l, x0 - 6.0f * s, y0 + 10.0f * s, x1 + 6.0f * s, y0 + 44.0f * s, ui4::kPink);
  l.text(x0 + 16.0f * s, y0 + 17.0f * s, "DRIVER LEVEL & AFFINITY XP",
         ui4::style(ui::Font::kE, 17.5f * s, ui4::kWhite));

  std::array<float, 4> bar_uv{};
  const bool has_bar = ui_texture("KITPARTS/BARS/BARBASE26PX", bar_uv);
  auto draw_xp_bar = [&](float bx0, float by0, float bx1, float by1, float prev_f, float cur_f, uint32_t col) {
    if (has_bar) {
      l.image(bx0, by0, bx1, by1, bar_uv, rgba(255, 255, 255, 38));
    } else {
      l.rect(bx0, by0, bx1, by1, ui4::kBrushBlack);
    }
    const float f = std::clamp(cur_f, 0.0f, 1.0f);
    if (f > 0.005f) {
      l.rect(bx0, by0 + 1.0f * s, bx0 + (bx1 - bx0) * f, by1 - 1.0f * s, col);
      const float pf = std::clamp(prev_f, 0.0f, f);
      if (pf > 0.005f) {
        l.rect(bx0, by0 + 1.0f * s, bx0 + (bx1 - bx0) * pf, by1 - 1.0f * s, rgba(0, 0, 0, 72));
      }
    }
  };

  // Section 1: Driver Level + animated XP gain & progress bar.
  float cy = y0 + 58.0f * s;
  const float ix0 = x0 + 18.0f * s, ix1 = x1 - 18.0f * s;
  char dl_buf[48], dx_buf[64];
  std::snprintf(dl_buf, sizeof dl_buf, "DRIVER LEVEL %d", drv_lvl);
  l.text(ix0, cy, dl_buf, ui4::style(ui::Font::kE, 16.0f * s, ui4::kWhite));
  if (r.driver_xp > 0) {
    std::snprintf(dx_buf, sizeof dx_buf, "+%s XP", thousands(shown_drv_gain).c_str());
    l.text(ix1, cy, dx_buf, ui4::style(ui::Font::kE, 16.0f * s, ui4::kYellow, Align::kRight));
  }
  cy += 24.0f * s;
  const double drv_after = double(std::max(0, drv_xp_tot));
  const double drv_before = std::max(0.0, drv_after - double(std::max(0, r.driver_xp)));
  const double drv_now = drv_before + (drv_after - drv_before) * anim;
  const float drv_prev_f = drv_next > 0 ? float(std::clamp(drv_before / double(drv_next), 0.0, 1.0)) : 0.0f;
  const float drv_cur_f = drv_next > 0 ? float(std::clamp(drv_now / double(drv_next), 0.0, 1.0)) : 1.0f;
  draw_xp_bar(ix0, cy, ix1, cy + 13.0f * s, drv_prev_f, drv_cur_f, ui4::kYellow);
  cy += 18.0f * s;
  char drv_sub[64];
  std::snprintf(drv_sub, sizeof drv_sub, "%s / %s XP TO LEVEL %d",
                thousands(int64_t(std::llround(drv_now))).c_str(), thousands(drv_next).c_str(), drv_lvl + 1);
  l.text(ix1, cy, drv_sub, ui4::style(ui::Font::kB, 11.0f * s, ui4::kDim, Align::kRight));

  // Divider hairline.
  cy += 22.0f * s;
  l.rect(ix0, cy, ix1, cy + 1.0f * s, rgba(255, 255, 255, 32));
  cy += 12.0f * s;

  // Section 2: Manufacturer Affinity Make, Level, XP bar, Bonus CR & Upgrade Discount.
  const std::string make_lbl = !r.affinity_make.empty() ? r.affinity_make : std::string("MANUFACTURER");
  float aff_tx = ix0;
  if (!r.affinity_make.empty() && ui4::manufacturer_logo(l, ix0, cy - 2.0f * s, ix0 + 34.0f * s, cy + 32.0f * s, r.affinity_make)) {
    aff_tx = ix0 + 42.0f * s;
  }
  char al_buf[64];
  std::snprintf(al_buf, sizeof al_buf, "%s AFFINITY  LVL %d", make_lbl.c_str(), aff_lvl);
  ui::TextStyle ats = ui4::style(ui::Font::kE, 15.0f * s, ui4::kWhite);
  ui4::fit_text(al_buf, ats, (ix1 - aff_tx) - 96.0f * s, 10.0f * s);
  l.text(aff_tx, cy, al_buf, ats);
  if (r.affinity_xp > 0) {
    char ax_buf[48];
    std::snprintf(ax_buf, sizeof ax_buf, "+%s XP", thousands(shown_aff_gain).c_str());
    l.text(ix1, cy, ax_buf, ui4::style(ui::Font::kE, 15.0f * s, ui4::kBlue, Align::kRight));
  }
  cy += 24.0f * s;
  const double aff_after = double(std::max(r.affinity_xp, aff_xp_tot));
  const double aff_before = std::max(0.0, aff_after - double(std::max(0, r.affinity_xp)));
  const double aff_now = aff_before + (aff_after - aff_before) * anim;
  const float aff_prev_f = aff_next > 0 ? float(std::clamp(aff_before / double(aff_next), 0.0, 1.0)) : 0.0f;
  const float aff_cur_f = aff_next > 0 ? float(std::clamp(aff_now / double(aff_next), 0.0, 1.0)) : 1.0f;
  draw_xp_bar(ix0, cy, ix1, cy + 13.0f * s, aff_prev_f, aff_cur_f, ui4::kBlue);
  cy += 20.0f * s;

  // Affinity Bonus CR & Upgrade Discount strip.
  const std::string cr = format_unit_label(str("UnitStrings", "IDS_Units_Credits", "Cr"), "Cr");
  ui4::pill(l, ix0, cy, ix1, cy + 28.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
  l.text(ix0 + 12.0f * s, cy + 7.0f * s, "AFFINITY BONUS", ui4::style(ui::Font::kB, 11.5f * s, ui4::kOffWhite));
  char cr_buf[64];
  std::snprintf(cr_buf, sizeof cr_buf, "+%s %s", thousands(shown_aff_cr).c_str(), cr.c_str());
  l.text(ix1 - 12.0f * s, cy + 6.0f * s, cr_buf,
         ui4::style(ui::Font::kE, 14.0f * s, shown_aff_cr > 0 ? ui4::kGreen : ui4::kDim, Align::kRight, true));
  if (aff_disc > 0.5) {
    char db[32];
    std::snprintf(db, sizeof db, "%d%% OFF PARTS", int(std::lround(aff_disc)));
    l.text(ix0 + 136.0f * s, cy + 7.5f * s, db, ui4::style(ui::Font::kE, 11.0f * s, ui4::kAmber));
  }
  cy += 36.0f * s;

  // Section 3: Optional Gift Car Earned badge!
  if (!gift_car.empty()) {
    ui4::brush(l, ix0 - 4.0f * s, cy, ix1 + 4.0f * s, cy + 52.0f * s, ui4::kPink);
    float gx = ix0 + 12.0f * s;
    if (ui4::badge_icon(l, gx, cy + 8.0f * s, gx + 34.0f * s, cy + 42.0f * s, ui4::BadgeIconKind::kTrophy)) {
      gx += 42.0f * s;
    }
    l.text(gx, cy + 7.0f * s, "GIFT CAR EARNED!", ui4::style(ui::Font::kE, 12.5f * s, ui4::kYellow));
    ui::TextStyle gts = ui4::style(ui::Font::kE, 16.5f * s, ui4::kWhite, Align::kLeft, true);
    ui4::fit_text(gift_car, gts, (ix1 - gx) - 10.0f * s, 10.0f * s);
    l.text(gx, cy + 25.0f * s, gift_car, gts);
  }
}

}  // namespace

// (c) The wristband points page (SPEC-UI-LAYOUTS, the wristband points
// banner, measured): POINTS EARNED on a black brush, the band's large
// wristband, COLLECT IT FROM RACE CENTRAL on a light panel when a level is
// reached (else the band's progress, ours), Career/Affinity XP breakdown on
// the right, and PLEASE WAIT bottom left.
void points_page(ui::List& l, const RaceHud& r, const DriveHud& st, const Layout& g, double page_elapsed) {
  const float s = g.s;
  const std::string event_title = resolve_hud_event_title(r);
  const std::string event_type = !r.event_type.empty() ? r.event_type : std::string("FESTIVAL RACE");
  ui4::subtle_vignette(l, g.w, g.h, 0.28f);
  draw_race_post_stage_pills(l, g, 2);

  // Localized event eyebrow above Wristband & Popularity title.
  {
    Turn t(l, g.L(320), g.T(128));
    const std::string eye = event_type + "  •  " + event_title;
    TextStyle es = font(ui::Font::kB, 14.0f * s, ui4::kYellow);
    fit(eye, es, g.L(520), 10.0f * s);
    const float ew = std::clamp(ui::List::measure(eye, es) + 30.0f * s, 220.0f * s, 520.0f * s);
    ui4::brush(l, g.L(92), g.T(114), g.L(92) + ew, g.T(142), ui4::kBrushBlack, false);
    l.text(g.L(106), g.T(121), eye, es);
  }
  // The grainy dark panel behind the wristband (the frame: x 0-520, y 235-365).
  {
    Turn t(l, g.L(260), g.T(300), -0.03f);
    std::array<float, 4> noise;
    if (ui_texture("DIRTMASKS/BGNOISE01", noise)) l.image(g.L(-10), g.T(238), g.L(520), g.T(362), noise, rgba(0, 0, 0, 235));
    else l.rect(g.L(-10), g.T(238), g.L(520), g.T(362), rgba(0, 0, 0, 120));
  }
  {
    Turn t(l, g.L(339), g.T(192));
    brush(l, g.L(95), g.T(156), g.L(585), g.T(229), kBrush);
    const std::string title = str("PostRaceFlow", "IDS_PointsEarned", "WRISTBAND & POPULARITY");
    TextStyle ts = font(ui::Font::kE, 44 * s, kText);
    fit(title, ts, g.L(570) - g.L(108), 22 * s);
    l.text(g.L(108), g.T(168) + (44 * s - ts.size) * 0.5f, title, ts);
  }
  // The band's texture by the colour in its name (HORIZON\WRISTBANDS\<colour>XL).
  std::string lower, band;
  for (char c : r.band_name) lower += char(std::tolower(static_cast<unsigned char>(c)));
  for (const char* k : {"yellow", "green", "orange", "blue", "purple", "pink", "gold", "platinum"})
    if (lower.find(k) != std::string::npos) {
      for (const char* p = k; *p; ++p) band += char(std::toupper(static_cast<unsigned char>(*p)));
      if (band != "PLATINUM") band += "XL";
      break;
    }
  std::array<float, 4> uv;
  if (!band.empty() && ui_texture("WRISTBANDS/" + band, uv)) {
    // The texture's margins: its ink fills the frame's 95,222-454,361.
    l.image_rotated(g.L(274.5f), g.T(291.5f), 255 * s, 99 * s, -0.07f, uv);
  } else if (!r.band_name.empty()) {
    Turn t(l, g.L(274.5f), g.T(291.5f), -0.07f);
    l.rect(g.L(95), g.T(232), g.L(454), g.T(351), band_colour(r.band_name));
  }
  if (r.band_up) {
    Turn t(l, g.L(270), g.T(375));
    l.rect(g.L(100), g.T(359), g.L(440), g.T(391), kLight);
    const std::string c = str("PostRaceFlow", "IDS_WristbandAvailableLn2", "COLLECT IT FROM RACE CENTRAL");
    TextStyle cs = font(ui::Font::kB, 18 * s, kDark);
    fit(c, cs, g.L(432) - g.L(106), 10 * s);
    l.text(g.L(106), g.T(366) + (18 * s - cs.size) * 0.5f, c, cs);
  } else if (r.band_after >= 0) {
    // Sharp H_POST_RACE_CAREER_POINTS wristband progress strip (TAPEHALF50PX / BARBASE26PX — ZERO rounded bar!).
    Turn tp(l, g.L(270), g.T(388));
    // Measure elapsed time strictly from per-page entry time (never r.finish_time).
    const float anim = ease_in_time(page_elapsed, 0.25, 1.15);
    const double from = r.band_up ? r.band_lo : r.band_before;
    const double now = from + (r.band_after - from) * anim;
    const uint32_t bc = band_colour(r.band_name);
    const float px0 = g.L(100), px1 = g.L(460), py0 = g.T(364);
    l.text(px0, py0, (r.band_name.empty() ? std::string("WRISTBAND") : r.band_name) + " PROGRESS",
           ui4::style(ui::Font::kB, 11.5f * s, bc));
    char pts[64];
    if (r.band_hi > 0)
      std::snprintf(pts, sizeof pts, "%s / %s PTS", thousands(int64_t(std::llround(now))).c_str(),
                    thousands(r.band_hi).c_str());
    else
      std::snprintf(pts, sizeof pts, "%s PTS", thousands(int64_t(std::llround(now))).c_str());
    l.text(px1, py0, pts, ui4::style(ui::Font::kE, 12.0f * s, ui4::kWhite, Align::kRight));

    const float by0 = py0 + 18 * s, by1 = by0 + 14 * s;
    std::array<float, 4> bar_uv{};
    if (ui_texture("KITPARTS/BARS/BARBASE26PX", bar_uv)) {
      l.image(px0, by0, px1, by1, bar_uv, ui4::kPillGrey);
    } else {
      l.rect(px0, by0, px1, by1, ui4::kBrushBlack);
    }
    const double span = r.band_hi > r.band_lo ? double(r.band_hi - r.band_lo) : 1.0;
    const float frac = r.band_hi <= 0 ? 1.0f : float(std::clamp((now - r.band_lo) / span, 0.0, 1.0));
    if (frac > 0.005f) {
      const float fx1 = px0 + (px1 - px0) * frac;
      l.rect(px0, by0 + 1 * s, fx1, by1 - 1 * s, bc);
      if (!r.band_up && r.band_before > r.band_lo) {
        const float bfrac = float(std::clamp((r.band_before - r.band_lo) / span, 0.0, 1.0));
        l.rect(px0, by0 + 1 * s, px0 + (px1 - px0) * bfrac, by1 - 1 * s, rgba(0, 0, 0, 72));
      }
    }
    if (!r.band_next.empty() && r.band_hi > 0) {
      l.text(px1, by1 + 5 * s, "NEXT: " + r.band_next,
             ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim, Align::kRight));
    }
  }

  // Popularity Rank standing strip beneath Wristband progress.
  {
    const float pop_x0 = g.L(98), pop_x1 = g.L(464), pop_y0 = g.T(442), pop_y1 = g.T(518);
    Turn pt(l, 0.5f * (pop_x0 + pop_x1), 0.5f * (pop_y0 + pop_y1), ui4::kTiltRad * 0.5f);
    ui4::panel(l, pop_x0, pop_y0, pop_x1, pop_y1, s, ui4::kCharcoalDark, 0, ui4::kYellow, true);
    l.text(pop_x0 + 16.0f * s, pop_y0 + 10.0f * s, "POPULARITY RANK", font(ui::Font::kB, 11.5f * s, ui4::kDim));
    l.text(pop_x0 + 16.0f * s, pop_y0 + 29.0f * s, "#" + std::to_string(std::clamp(st.popularity_rank, 1, 250)),
           font(ui::Font::kE, 34.0f * s, ui4::kYellow));
    l.text(pop_x1 - 16.0f * s, pop_y0 + 38.0f * s, "FESTIVAL STANDING",
           font(ui::Font::kE, 14.0f * s, ui4::kGreen, Align::kRight));
  }

  // Right-hand Career / Driver Level & Manufacturer Affinity XP panel + Prize Car Awarded showcase card.
  const bool has_xp_panel = (r.driver_xp > 0 || r.affinity_xp > 0 || r.driver_level > 0 || r.affinity_level > 0 ||
                             r.affinity_bonus_credits > 0 || !r.gift_car_name.empty());
  if (has_xp_panel) {
    career_affinity_panel(l, r, st, g, g.R(646), g.T(146), g.R(1204), page_elapsed, !r.won_car_name.empty());
  } else if (!r.won_car_name.empty()) {
    const float px0 = g.R(646), px1 = g.R(1204), py0 = g.T(166), py1 = g.T(366);
    Turn wt(l, 0.5f * (px0 + px1), 0.5f * (py0 + py1), ui4::kTiltRad * 0.55f);
    ui4::panel(l, px0, py0, px1, py1, s, ui4::kCharcoalDark, ui4::kYellow, ui4::kPink, true);
    ui4::brush(l, px0 - 6.0f * s, py0 + 12.0f * s, px1 + 6.0f * s, py0 + 50.0f * s, ui4::kPink, false);
    float tx = px0 + 18.0f * s;
    if (ui4::badge_icon(l, tx, py0 + 16.0f * s, tx + 28.0f * s, py0 + 44.0f * s, ui4::BadgeIconKind::kTrophy)) {
      tx += 36.0f * s;
    }
    l.text(tx, py0 + 21.0f * s, "PRIZE CAR AWARDED!", font(ui::Font::kE, 18.0f * s, ui4::kYellow));
    TextStyle ns = font(ui::Font::kE, 26.0f * s, kText);
    ns.keep_case = true;
    fit(r.won_car_name, ns, (px1 - px0) - 36.0f * s, 14.0f * s);
    l.text(px0 + 20.0f * s, py0 + 70.0f * s, r.won_car_name, ns);
    ui4::offwhite_strip(l, px0 + 18.0f * s, py0 + 122.0f * s, px1 - 18.0f * s, py0 + 154.0f * s, s, false);
    l.text(px0 + 30.0f * s, py0 + 131.0f * s, "DELIVERED TO YOUR HORIZON GARAGE", font(ui::Font::kE, 13.0f * s, ui4::kInk));
  }

  // Final-stage action bar when no separate won-car page follows, else CONTINUE.
  if (r.won_car_name.empty()) {
    ui4::button_bar(l,
                    {{"A", str("InGame", "IDS_ContinueMenu", "CONTINUE")},
                     {"Y", str("InGame", "IDS_RestartMenu", "RESTART")},
                     {"X", str("InGame", "IDS_ViewReplay", "VIEW REPLAY")},
                     {"BACK", "MAP / GPS"}},
                    g.w, g.h);
  } else {
    ui4::button_bar(l, {{"A", str("InGame", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
  }
}

// (c4) Prize Car Won screen (H_POST_RACE_WON_CAR style).
void won_car_page(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  const std::string event_title = resolve_hud_event_title(r);
  ui4::subtle_vignette(l, g.w, g.h, 0.30f);
  draw_race_post_stage_pills(l, g, 2);
  {
    Turn t(l, g.L(340), g.T(148));
    const std::string eye = (!r.event_type.empty() ? r.event_type : std::string("SHOWCASE")) + "  •  " + event_title;
    TextStyle es = font(ui::Font::kB, 14.0f * s, ui4::kYellow);
    fit(eye, es, g.L(560), 10.0f * s);
    const float ew = std::clamp(ui::List::measure(eye, es) + 30.0f * s, 220.0f * s, 560.0f * s);
    ui4::brush(l, g.L(92), g.T(134), g.L(92) + ew, g.T(162), ui4::kBrushBlack, false);
    l.text(g.L(106), g.T(141), eye, es);
  }
  {
    Turn t(l, g.L(340), g.T(215));
    TextStyle ts = font(ui::Font::kE, 48 * s, kText);
    const float tw = ui::List::measure("PRIZE CAR AWARDED!", ts);
    brush(l, g.L(92), g.T(178), std::max(g.L(560), g.L(112) + tw + 38 * s), g.T(250), kBrush);
    l.text(g.L(112), g.T(188), "PRIZE CAR AWARDED!", ts);
  }
  {
    Turn t(l, g.L(440), g.T(310));
    TextStyle ns = font(ui::Font::kE, 52 * s, kText);
    ns.keep_case = true;
    fit(r.won_car_name, ns, g.L(880) - g.L(108), 22 * s);
    const float nw = ui::List::measure(r.won_car_name, ns);
    brush(l, g.L(75), g.T(258), std::max(g.L(640), g.L(108) + nw + 46 * s), g.T(362), kPink);
    l.text(g.L(108), g.T(280) + (52 * s - ns.size) * 0.5f, r.won_car_name, ns);
  }
  {
    Turn t(l, g.L(280), g.T(398));
    ui4::offwhite_strip(l, g.L(105), g.T(378), g.L(510), g.T(418), s, true);
    l.text(g.L(122), g.T(389), "DELIVERED TO YOUR HORIZON GARAGE", font(ui::Font::kB, 17 * s, kDark));
  }
  ui4::button_bar(l,
                  {{"A", str("InGame", "IDS_ContinueMenu", "CONTINUE")},
                   {"Y", str("InGame", "IDS_RestartMenu", "RESTART")},
                   {"X", str("InGame", "IDS_ViewReplay", "VIEW REPLAY")},
                   {"BACK", "MAP / GPS"}},
                  g.w, g.h);
}

void results(ui::List& l, const RaceHud& r, const DriveHud& st, const Layout& g) {
  const float s = g.s;
  const double page_elapsed = post_race_page_elapsed(r);
  const std::string event_title = resolve_hud_event_title(r);
  const std::string event_type = !r.event_type.empty() ? r.event_type : std::string("FESTIVAL RACE");

  if (r.results_page == 4) {
    ui4::subtle_vignette(l, g.w, g.h, 0.28f);
    draw_race_post_stage_pills(l, g, 2);
    career_affinity_panel(l, r, st, g, g.C(330), g.T(148), g.C(950), page_elapsed, true);
    ui4::button_bar(l, {{"A", str("InGame", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
    return;
  }
  if (r.results_page == 3 && !r.won_car_name.empty()) {
    won_car_page(l, r, g);
    return;
  }
  if (r.results_page == 2) {
    points_page(l, r, st, g, page_elapsed);
    return;
  }
  if (r.results_page == 0 && r.results_table) {
    results_table(l, r, g);
    return;
  }
  // Stage 1 — RACE RESULTS: FH1 magenta 1ST PLACE / FINISHED header + compact podium leaderboard table!
  if (r.results_page == 0) {
    ui4::subtle_vignette(l, g.w, g.h, 0.28f);
    draw_race_post_stage_pills(l, g, 0);
    const float in = ease_in_time(page_elapsed, 0, 0.35);
    const float dx = (1 - in) * -100 * s;
    const int place = r.stage ? r.stage_place : r.place;
    {
      Turn t(l, g.L(360) + dx, g.T(152));
      brush(l, dx, g.T(106), g.L(660) + dx, g.T(196), kPink);
      char p[16];
      std::snprintf(p, sizeof p, "%d%s ", std::max(1, place), place_suffix(std::max(1, place)));
      const std::string text = p + str("InGame", "IDS_Place", "PLACE");
      TextStyle ps = font(ui::Font::kE, 64 * s, kText);
      fit(text, ps, g.L(630) - g.L(88), 28 * s);
      l.text(g.L(88) + dx, g.T(122) + (64 * s - ps.size) * 0.5f, text, ps);
    }
    {
      Turn t(l, g.L(290) + dx, g.T(96));
      const std::string eye = str("InGame", "IDS_Finished", "FINISHED") + "  •  " + event_type + "  •  " + event_title;
      TextStyle fs = font(ui::Font::kE, 22 * s, ui4::kYellow);
      fit(eye, fs, g.L(660), 12 * s);
      const float fw = std::clamp(ui::List::measure(eye, fs) + 34 * s, g.L(320), g.L(680));
      brush(l, g.L(72) + dx, g.T(76), g.L(72) + dx + fw, g.T(114), kBrush);
      l.text(g.L(88) + dx, g.T(84) + (22 * s - fs.size) * 0.5f, eye, fs);
    }
    // Podium / Leaderboard table directly on Stage 1 (POS, DRIVER, CAR / PI, TIME / GAP).
    if (!r.rows.empty()) {
      const float tx0 = g.L(72) + dx * 0.5f;
      const float ty0 = g.T(214);
      const float tw = g.L(760);
      const size_t nrows = std::min<size_t>(r.rows.size(), 8);
      const float row_h = 33.0f * s;
      const float panel_h = 38.0f * s + float(nrows) * row_h + 14.0f * s;
      Turn tb(l, tx0 + 0.5f * tw, ty0 + 0.5f * panel_h, ui4::kTiltRad * 0.45f);
      ui4::panel(l, tx0, ty0, tx0 + tw, ty0 + panel_h, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
      ui4::offwhite_strip(l, tx0 + 10.0f * s, ty0 + 8.0f * s, tx0 + tw - 10.0f * s, ty0 + 34.0f * s, s, false);
      const float c_pos = tx0 + 22.0f * s;
      const float c_drv = tx0 + 74.0f * s;
      const float c_car = tx0 + 300.0f * s;
      const float c_time = tx0 + tw - 20.0f * s;
      const TextStyle hs = font(ui::Font::kE, 12.0f * s, ui4::kInk);
      l.text(c_pos, ty0 + 14.5f * s, "POS", hs);
      l.text(c_drv, ty0 + 14.5f * s, "DRIVER", hs);
      l.text(c_car, ty0 + 14.5f * s, "CAR / PI", hs);
      l.text(c_time, ty0 + 14.5f * s, "TIME / GAP", font(ui::Font::kE, 12.0f * s, ui4::kInk, Align::kRight));

      float ry = ty0 + 40.0f * s;
      for (size_t i = 0; i < nrows; ++i) {
        const RaceHudRow& row = r.rows[i];
        const uint32_t bg = row.player ? ui4::kRowHighlight : ((i % 2 == 1) ? ui4::kRowAlt : rgba(20, 22, 28, 185));
        l.rect(tx0 + 10.0f * s, ry, tx0 + tw - 10.0f * s, ry + row_h - 4.0f * s, bg);
        if (row.player) l.rect(tx0 + 10.0f * s, ry, tx0 + 15.0f * s, ry + row_h - 4.0f * s, ui4::kPink);
        l.text(c_pos, ry + 6.5f * s, std::to_string(row.place),
               font(ui::Font::kE, 14.5f * s, row.place == 1 ? ui4::kYellow : (row.player ? ui4::kPinkHi : kText)));
        TextStyle ds = font(row.player ? ui::Font::kE : ui::Font::kB, 13.5f * s, kText);
        ds.keep_case = true;
        fit(row.name, ds, (c_car - c_drv) - 14.0f * s, 9.5f * s);
        l.text(c_drv, ry + 7.5f * s, row.name, ds);

        float cx = c_car;
        if (!row.car_class.empty() && row.pi >= 0) {
          ui4::class_badge(l, cx, ry + 4.5f * s, 20.0f * s, row.car_class, row.pi);
          cx += 62.0f * s;
        }
        TextStyle cs = font(ui::Font::kB, 12.5f * s, row.player ? kText : ui4::kDim);
        cs.keep_case = true;
        fit(row.car, cs, (c_time - cx) - 102.0f * s, 9.0f * s);
        l.text(cx, ry + 8.0f * s, row.car, cs);

        const std::string tstr = row.finished ? clock_ms(row.time)
                                              : (row.gap > 0.05 ? gap_ms(row.gap) : std::string("FINISHED"));
        l.text(c_time, ry + 7.5f * s, tstr,
               font(ui::Font::kE, 13.0f * s, row.place == 1 ? ui4::kYellow : ui4::kOffWhite, Align::kRight));
        ry += row_h;
      }
    }
    ui4::button_bar(l,
                    {{"A", str("InGame", "IDS_ContinueMenu", "CONTINUE")},
                     {"Y", str("InGame", "IDS_RestartMenu", "RESTART")},
                     {"X", str("InGame", "IDS_RaceResultsMenu", "RACE RESULTS")},
                     {"B", str("InGame", "IDS_ViewReplay", "VIEW REPLAY")}},
                    g.w, g.h);
    return;
  }
  // Stage 2 — CREDITS EARNED (animated rolling count-up + updated balance).
  ui4::subtle_vignette(l, g.w, g.h, 0.25f);
  draw_race_post_stage_pills(l, g, 1);
  {
    Turn t(l, g.L(313), g.T(114));
    const std::string eye = event_type + "  •  " + event_title;
    TextStyle es = font(ui::Font::kB, 14.0f * s, ui4::kYellow);
    fit(eye, es, g.L(520), 10.0f * s);
    const float ew = std::clamp(ui::List::measure(eye, es) + 30.0f * s, 240.0f * s, 520.0f * s);
    ui4::brush(l, g.L(98), g.T(100), g.L(98) + ew, g.T(128), ui4::kBrushBlack, false);
    l.text(g.L(112), g.T(107), eye, es);
  }
  {
    Turn t(l, g.L(313), g.T(176));
    brush(l, g.L(98), g.T(138), g.L(540), g.T(214), kBrush);
    TextStyle ts = font(ui::Font::kE, 52 * s, kText);
    const std::string title = str("ScreenTitles", "IDS_PostRace_Winnings", "CREDITS EARNED");
    fit(title, ts, g.L(530) - g.L(108), 24 * s);
    l.text(g.L(108), g.T(150) + (52 * s - ts.size) * 0.5f, title, ts);
  }
  const std::string cr = format_unit_label(str("UnitStrings", "IDS_Units_Credits", "Cr"), "Cr");
  char line[64], diff_buf[32], assist_buf[32], xp_buf[64];
  const int place = r.stage ? r.stage_place : r.place;
  std::snprintf(line, sizeof line, "%s %d%s", str("InGame", "IDS_Finished", "FINISHED").c_str(), place, place_suffix(place));
  const int ai_pct = std::max(0, r.difficulty_pct - r.assists_pct);
  std::snprintf(diff_buf, sizeof diff_buf, "+%d%% (%s %s)", ai_pct, thousands(r.difficulty_bonus).c_str(), cr.c_str());
  const int64_t base_cr = r.base_credits > 0 ? r.base_credits : r.credits;
  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back(line, thousands(base_cr) + " " + cr);
  rows.emplace_back(str("OnlineRewards", "IDS_Mileage_Bonus_Heading", "MILEAGE BONUS"),
                    thousands(r.mileage_bonus) + " " + cr);
  rows.emplace_back(str("OnlineRewards", "IDS_Difficulty_Bonus_Heading", "AI DIFFICULTY BONUS"), diff_buf);
  if (r.assists_pct > 0 || r.assists_bonus > 0) {
    std::snprintf(assist_buf, sizeof assist_buf, "+%d%% (%s %s)", r.assists_pct, thousands(r.assists_bonus).c_str(),
                  cr.c_str());
    rows.emplace_back("ASSISTS BONUS", assist_buf);
  }
  if (r.skills_bonus > 0) rows.emplace_back("SKILLS BONUS", thousands(r.skills_bonus) + " " + cr);
  if (r.affinity_bonus_credits > 0)
    rows.emplace_back("AFFINITY BONUS", thousands(r.affinity_bonus_credits) + " " + cr);
  if (r.nemesis_bonus > 0) rows.emplace_back("NEMESIS BONUS", thousands(r.nemesis_bonus) + " " + cr);
  if (r.bounty_bonus > 0) rows.emplace_back("BOUNTY BONUS", thousands(r.bounty_bonus) + " " + cr);
  if (r.driver_xp > 0) {
    std::snprintf(xp_buf, sizeof xp_buf, "+%s XP  •  AFFINITY +%s XP", thousands(r.driver_xp).c_str(),
                  thousands(r.affinity_xp).c_str());
    rows.emplace_back("DRIVER & AFFINITY XP", xp_buf);
  }
  const float row_step = rows.size() > 4 ? 24.0f : 28.0f;
  for (size_t i = 0; i < rows.size(); ++i) {
    const float rev = ease_in_time(page_elapsed, 0.08 + double(i) * 0.12, 0.28);
    if (rev <= 0.01f) continue;
    const float rx_off = (1.0f - rev) * 24.0f * s;
    const float y = g.T(234 + row_step * float(i));
    const TextStyle ls = font(ui::Font::kB, (rows.size() > 5 ? 16 : 20) * s, ui::fade(kText, rev));
    TextStyle vs = font(ui::Font::kE, (rows.size() > 5 ? 15 : 18) * s, ui::fade(kText, rev));
    vs.keep_case = true;
    const float lw = ui::List::measure(rows[i].first, ls), vw = ui::List::measure(rows[i].second, vs);
    Turn t(l, g.L(148) + 0.5f * lw, y + 10 * s);
    brush(l, g.L(140) + rx_off, y - 4 * s, g.L(148) + lw + 6 * s + rx_off, y + 22 * s, ui::fade(kBrush, rev));
    l.text(g.L(148) + rx_off, y, rows[i].first, ls);
    const float vx = g.L(148) + lw + 10 * s + rx_off;
    brush(l, vx - 4 * s, y - 2 * s, vx + vw + 6 * s, y + 20 * s, ui::fade(kPink, rev));
    l.text(vx, y + 1 * s, rows[i].second, vs);
  }
  // TOTAL and rolling UPDATED BALANCE measured from per-page entry time!
  const float count = ease_in_time(page_elapsed, 0.2, 1.0);
  const int64_t shown_earned = int64_t(std::llround(double(r.credits) * count));
  {
    Turn tn(l, g.L(295), g.T(418));
    ui4::brush(l, -28 * s, g.T(370), g.L(490), g.T(472), ui4::kBrushBlack, true);
    const std::string t = str("OnlineRewards", "IDS_Total", "TOTAL");
    TextStyle ts = font(ui::Font::kE, 75 * s, kText);
    fit(t, ts, g.L(449) - g.L(140), 30 * s);
    l.text(g.L(140), g.T(382) + (75 * s - ts.size), t, ts);
  }
  Turn tv(l, g.L(678), g.T(412));
  ui4::brush(l, g.L(476), g.T(360), g.L(880), g.T(464), ui4::kPink, true);
  TextStyle vs = font(ui::Font::kE, 70 * s, kText, Align::kRight);
  vs.keep_case = true;
  const std::string total = thousands(shown_earned) + " " + cr;
  fit(total, vs, g.L(860) - g.L(490), 30 * s);
  l.text(g.L(860), g.T(377) + (70 * s - vs.size) * 0.5f, total, vs);
  l.unturn();
  if (r.balance >= 0) {
    const int64_t bal_start = std::max<int64_t>(0, r.balance - r.credits);
    const int64_t shown_bal = bal_start + shown_earned;
    Turn tb(l, g.L(280), g.T(497));
    ui4::offwhite_strip(l, g.L(150), g.T(475), g.L(440), g.T(520), s, true);
    TextStyle bs = font(ui::Font::kB, 16 * s, kDark);
    l.text(g.L(162), g.T(490), str("OnlineRewards", "IDS_Balance", "BALANCE"), bs);
    bs.align = Align::kRight;
    bs.keep_case = true;  // "117,881 Cr" as shown
    l.text(g.L(426), g.T(490), thousands(shown_bal) + " " + cr, bs);
  }
  ui4::button_bar(l, {{"A", str("InGame", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
}


void opponent_name_tags(ui::List& l, const std::vector<NameTagHud>& tags, const Layout& g) {
  if (tags.empty()) return;
  const float s = g.s;
  const float margin_x = 24.0f * s;
  const float top_limit = 20.0f * s;
  const float bottom_limit = g.h - 110.0f * s;
  const float pill_h = 22.0f * s;
  const float pad_x = 6.0f * s;
  const float pad_y = 4.0f * s;

  struct TagBox {
    size_t orig_idx = 0;
    float orig_x = 0.0f;
    float orig_y = 0.0f;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    float badge_w = 0.0f;
    float cls_w = 0.0f;
    float pi_w = 0.0f;
    bool has_badge = false;
    bool leader = false;
    std::string car_class;
    std::string pi_text;
    std::string driver;
    TextStyle name_ts{};
  };

  std::vector<TagBox> boxes;
  boxes.reserve(tags.size());
  for (size_t i = 0; i < tags.size(); ++i) {
    const auto& t = tags[i];
    if (t.name.empty()) continue;
    if (t.screen_x < 40.0f * s || t.screen_x > g.w - 40.0f * s || t.screen_y < 30.0f * s ||
        t.screen_y > bottom_limit)
      continue;

    TagBox b;
    b.orig_idx = i;
    b.orig_x = t.screen_x;
    b.orig_y = t.screen_y;
    b.leader = t.leader;
    b.driver = t.name;

    // Parse Festival AI "[<CLASS> <PI>] <DRIVER>" prefix into a dedicated FH1 class/PI badge.
    if (t.name.size() >= 6 && t.name.front() == '[') {
      const size_t close = t.name.find(']');
      if (close != std::string::npos && close > 2) {
        const std::string inside = t.name.substr(1, close - 1);
        const size_t sp = inside.find(' ');
        if (sp != std::string::npos && sp > 0 && sp + 1 < inside.size()) {
          std::string cls = inside.substr(0, sp);
          std::string pi = inside.substr(sp + 1);
          size_t rest_pos = close + 1;
          while (rest_pos < t.name.size() && t.name[rest_pos] == ' ') ++rest_pos;
          const std::string drv = t.name.substr(rest_pos);
          if (!drv.empty()) {
            b.has_badge = true;
            b.car_class = std::move(cls);
            b.pi_text = std::move(pi);
            b.driver = drv;
          }
        }
      }
    }

    b.name_ts = font(ui::Font::kB, 11.5f * s, b.leader ? ui4::kYellow : ui4::kWhite);
    fit(b.driver, b.name_ts, 185.0f * s, 9.0f * s);
    const float name_w = ui::List::measure(b.driver, b.name_ts);

    float pill_w = 0.0f;
    if (b.has_badge) {
      const float bh = pill_h - 6.0f * s;
      b.cls_w = (b.car_class.size() > 1 ? 20.0f : 16.0f) * s;
      b.pi_w = std::max(24.0f * s, bh * 1.55f);
      b.badge_w = b.cls_w + b.pi_w;
      pill_w = 4.0f * s + b.badge_w + 7.0f * s + name_w + 9.0f * s;
    } else {
      const float star_w = b.leader ? ui::List::measure("* ", b.name_ts) : 0.0f;
      pill_w = 4.0f * s + 7.0f * s + star_w + name_w + 9.0f * s;
    }

    const float cx = std::clamp(t.screen_x, margin_x + 0.5f * pill_w, std::max(margin_x + 0.5f * pill_w, g.w - margin_x - 0.5f * pill_w));
    b.x0 = cx - 0.5f * pill_w;
    b.x1 = cx + 0.5f * pill_w;
    b.y1 = t.screen_y - 4.0f * s;
    b.y0 = b.y1 - pill_h;
    boxes.push_back(std::move(b));
  }

  // Sort bottom-most (typically nearest) first so the closest car stays anchored
  // right above its roof and colliding farther tags stack cleanly upward into sky.
  std::sort(boxes.begin(), boxes.end(), [](const TagBox& a, const TagBox& b) {
    if (std::fabs(a.orig_y - b.orig_y) > 1.0f) return a.orig_y > b.orig_y;
    return a.orig_idx < b.orig_idx;
  });

  std::vector<TagBox> placed;
  placed.reserve(boxes.size());
  for (auto& cur : boxes) {
    bool moved = true;
    int guard = 0;
    while (moved && guard++ < 24) {
      moved = false;
      for (const auto& prev : placed) {
        const bool overlap_x = (cur.x0 - pad_x < prev.x1) && (cur.x1 + pad_x > prev.x0);
        const bool overlap_y = (cur.y0 - pad_y < prev.y1) && (cur.y1 + pad_y > prev.y0);
        if (overlap_x && overlap_y) {
          const float new_y1 = prev.y0 - pad_y;
          const float shift = cur.y1 - new_y1;
          cur.y0 -= shift;
          cur.y1 = new_y1;
          moved = true;
        }
      }
    }
    if (cur.y0 < top_limit) continue;
    placed.push_back(std::move(cur));
  }

  // Draw back-to-front (highest/stacked first, nearest/anchored last on top).
  for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
    const auto& b = *it;
    const float lift = (b.orig_y - 4.0f * s) - b.y1;
    if (lift > 6.0f * s) {
      const float stem_x = std::clamp(b.orig_x, b.x0 + 6.0f * s, b.x1 - 6.0f * s);
      l.line(stem_x, b.y1, stem_x, b.orig_y - 3.0f * s, 1.4f * s, rgba(255, 255, 255, 95));
    }

    // Soft drop shadow + compact FH1 dark translucent nameplate pill.
    l.rect(b.x0 + 1.5f * s, b.y0 + 2.0f * s, b.x1 + 1.5f * s, b.y1 + 2.0f * s, rgba(0, 0, 0, 115));
    ui4::pill(l, b.x0, b.y0, b.x1, b.y1, rgba(18, 20, 26, 208), ui4::PillKind::kMapSmall);
    // Crisp top hairline and left accent bar (pink for H2H/leader, subtle silver otherwise).
    l.rect(b.x0, b.y0, b.x1, b.y0 + 1.0f * s, rgba(255, 255, 255, 34));
    l.rect(b.x0, b.y0, b.x0 + 3.0f * s, b.y1, b.leader ? ui4::kPink : rgba(220, 224, 232, 165));

    float tx = b.x0 + 4.0f * s;
    if (b.has_badge) {
      const float by0 = b.y0 + 3.0f * s;
      const float by1 = b.y1 - 3.0f * s;
      const float bh = by1 - by0;
      const uint32_t cls_col = ui4::class_colour_by_name(b.car_class);
      l.rect(tx, by0, tx + b.cls_w, by1, cls_col);
      l.rect(tx + b.cls_w, by0, tx + b.badge_w, by1, ui4::kWhite);

      const bool light_cls = (b.car_class == "C" || b.car_class == "c" || b.car_class == "X" || b.car_class == "x");
      TextStyle cs = font(ui::Font::kE, bh * 0.68f, light_cls ? ui4::kInk : ui4::kWhite, Align::kCenter);
      fit(b.car_class, cs, b.cls_w - 2.0f * s, bh * 0.42f);
      l.text(tx + 0.5f * b.cls_w, by0 + 0.5f * (bh - cs.size), b.car_class, cs);

      TextStyle ps = font(ui::Font::kB, bh * 0.62f, ui4::kInk, Align::kCenter);
      fit(b.pi_text, ps, b.pi_w - 2.0f * s, bh * 0.40f);
      l.text(tx + b.cls_w + 0.5f * b.pi_w, by0 + 0.5f * (bh - ps.size), b.pi_text, ps);

      tx += b.badge_w + 7.0f * s;
      l.text(tx, b.y0 + 0.5f * (pill_h - b.name_ts.size), b.driver, b.name_ts);
    } else {
      tx += 7.0f * s;
      const std::string text = (b.leader ? "* " : "") + b.driver;
      l.text(tx, b.y0 + 0.5f * (pill_h - b.name_ts.size), text, b.name_ts);
    }
  }
}


void showcase_hud(ui::List& l, const RaceHud& r, const Layout& g, bool metric_units) {
  if (!r.showcase || r.phase != RaceHud::kRacing) return;
  const float s = g.s;
  const float cx = 0.5f * g.w;
  const float bw = 440 * s, bh = 54 * s;
  const float x0 = cx - 0.5f * bw, x1 = cx + 0.5f * bw;
  const float y0 = g.T(24), y1 = y0 + bh;
  ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  const std::string gap_str =
      (r.showcase_gap_m >= 0 ? std::string("AHEAD ") : std::string("BEHIND ")) +
      format_distance_unit(std::fabs(r.showcase_gap_m), metric_units);
  l.text(x0 + 12 * s, y0 + 10 * s, "SHOWCASE  //  VS " + r.showcase_aircraft, font(ui::Font::kB, 13 * s, kText));
  l.text(x1 - 12 * s, y0 + 10 * s, gap_str,
         font(ui::Font::kE, 14 * s, r.showcase_gap_m >= 0 ? ui4::kGreen : ui4::kAmber, Align::kRight));
  const float tx0 = x0 + 14 * s, tx1 = x1 - 14 * s, ty = y0 + 35 * s, th = 8 * s;
  l.rect(tx0, ty, tx1, ty + th, rgba(255, 255, 255, 35));
  const float ax = tx0 + float(std::clamp(r.showcase_progress, 0.0, 1.0)) * (tx1 - tx0);
  const float px = tx0 + float(std::clamp(r.progress, 0.0, 1.0)) * (tx1 - tx0);
  l.rect(tx0, ty, ax, ty + 4 * s, ui4::kAmber);
  l.rect(tx0, ty + 4 * s, px, ty + th, ui4::kPink);
  l.circle(ax, ty + 2 * s, 5 * s, ui4::kYellow);
  l.circle(px, ty + 6 * s, 5 * s, kText);
}

}  // namespace carview::hud_detail

namespace carview {

double post_race_page_elapsed(const RaceHud& r) {
  if (r.page_enter_time >= 0.0) {
    return std::max(0.0, r.clock - r.page_enter_time);
  }
  auto& t = hud_detail::page_tracker();
  if (!t.active || t.last_page != r.results_page || std::fabs(t.last_finish_time - r.finish_time) > 1e-3 ||
      r.clock < t.entered_at) {
    t.active = true;
    t.last_page = r.results_page;
    t.last_finish_time = r.finish_time;
    t.entered_at = r.clock;
  }
  return std::max(0.0, r.clock - t.entered_at);
}

void reset_post_race_page_timer() {
  hud_detail::page_tracker() = {};
}

}  // namespace carview
