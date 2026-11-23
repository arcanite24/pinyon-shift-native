#include "game_hud_internal.h"

namespace carview::hud_detail {

void draw_career_header(ui::List& l, const DriveHud& st, const Layout& g) {
  ui4::CareerStripData cs;
  cs.band_name = st.band_name;
  cs.band_next = st.band_next;
  cs.band_level = st.band_level;
  cs.band_points = st.band_points;
  cs.band_hi = st.band_hi;
  cs.band_collect_ready = st.band_collect_ready;
  cs.car_name = st.car_name;
  cs.car_class = st.car_class;
  cs.car_pi = st.car_pi;
  cs.credits = st.credits;
  cs.popularity_rank = st.popularity_rank;
  ui4::career_strip(l, cs, g.w, g.h);
}

void menu_panel(ui::List& l, const DriveHud& st, const Layout& g) {
  const float s = g.s;
  const bool pause = st.pause || st.menu_title == "paused";
  static const SceneDraw pause_sc(ui_scene("925_PAUSE_MENU"), true);
  const SceneFrame pf{0.5f * g.w - 640 * s, 0.5f * g.h - 360 * s, s};

  // Retail 925_PAUSE_MENU keeps Colorado bright & crystal clear behind the tilted brushes,
  // with subtle BG_Noise (BGNOISE01 + BGNOISEMASK01 via 2-tex kImageMasked).
  if (pause && pause_sc.ok()) {
    const int bg_noise = pause_sc.find("BG_Noise");
    if (bg_noise >= 0) pause_sc.model(l, pf, bg_noise, 0.22f);
  } else {
    ui4::subtle_vignette(l, g.w, g.h, pause ? 0.16f : 0.42f);
  }

  std::string title = st.menu_title, hint;
  if (auto c = title.find(':'); c != std::string::npos) {
    hint = title.substr(c + 1);
    title = title.substr(0, c);
    while (!hint.empty() && hint.front() == ' ') hint.erase(hint.begin());
  }
  if (!pause && !title.empty()) {
    ui4::title_banner(l, s, g.L(92), g.T(40), title, hint, false, 640.0f);
  }

  const int n = int(st.menu.size());
  const bool has_right = st.career != nullptr || !st.detail_lines.empty();
  // 925_PAUSE_MENU Option_List SUPER_STACKER reflow (50 px pitch, +3 deg Crisp_Tape,
  // +4 deg Smudge_Tape, Dirt_Tile DIRTTILE01 grit, and TAPECLIPON02 on selected row!).
  float row_step = pause ? 50.0f : 44.0f;
  float first_bottom = pause ? 186.0f : 204.0f;
  float list_tilt = ui4::kTiltRad;
  if (pause && pause_sc.ok()) {
    if (const int b0 = pause_sc.find("Button0"); b0 >= 0) {
      const float a = -pause_sc.angle(b0);
      if (std::fabs(a) > 1e-3f) list_tilt = a;
    }
  }
  const int fit_rows = std::clamp(int((g.B(612) - g.T(first_bottom)) / (row_step * s)) + 1, 1, 8);
  const int first = std::clamp(st.menu_selected - fit_rows / 2, 0, std::max(0, n - fit_rows));
  const float max_item_w = st.career ? g.L(265) : (has_right ? g.L(495) : (g.w - g.L(300)));
  const float left_shift = st.career ? -62.0f * s : 0.0f;
  const auto stack_ys = fh1::reflow_stacker(first_bottom, row_step, std::min(n - first, fit_rows));

  std::array<float, 4> dirt_uv{}, clip_uv{}, smudge_uv{}, crisp_uv{};
  const bool has_dirt = ui_texture("DIRTMASKS/DIRTTILE01", dirt_uv);
  const bool has_clip = ui_texture("DIRTMASKS/TAPECLIPON02", clip_uv);
  const bool has_smudge = ui_texture("DIRTMASKS/SMUDGETAPE01", smudge_uv);
  const bool has_crisp = ui_texture("DIRTMASKS/CRISPTAPE01", crisp_uv);

  Turn list_turn(l, g.L(226) + left_shift, g.T(185), list_tilt);
  if (first > 0) {
    l.text(g.L(188) + left_shift, g.T(first_bottom - 52.0f), "^ MORE", ui4::style(ui::Font::kB, 11 * s, ui4::kDim));
  }
  for (int i = first; i < n && i < first + fit_rows; ++i) {
    const bool sel = i == st.menu_selected;
    // Strictly fixed row pitch, font size, and strip bounds matching real FH1
    // pause-freeroam.png vs pause-freeroam-settings.png: moving focus swaps the
    // brush highlight & clip-on ONLY, never resizing or shifting neighbor rows.
    const float bottom = g.T(stack_ys[size_t(i - first)]);
    const std::string& item = st.menu[size_t(i)];
    const float base_sz = pause ? 36.0f : 25.0f;
    TextStyle it = font(ui::Font::kE, base_sz * s, kText);
    fit(item, it, max_item_w, 13 * s);
    const float tw = ui::List::measure(item, it);
    const float tx = g.L(186) + left_shift;
    const float bx0 = g.L(156) + left_shift;
    const float by0 = bottom - it.size - 7 * s;
    const float bx1 = std::max(g.L(300) + left_shift, tx + tw + 30 * s);
    const float by1 = bottom + 6 * s;
    l.add_hit(bx0, by0, bx1, by1, ui::kHitRow, i);

    if (sel) {
      // 925_PAUSE_MENU active item: 4 deg Smudge_Tape + 3 deg Crisp_Tape + Dirt_Tile + Clipon_2.
      if (has_clip) {
        l.image(bx0 - 24 * s, by0 + 3 * s, bx0 + 18 * s, by1 - 2 * s, clip_uv, ui4::kPink);
      }
      if (has_smudge && has_crisp) {
        const float px = 0.03f * (by1 - by0), py = 0.14f * (by1 - by0);
        // Subtle +1 deg extra counter-tilt on Smudge_Tape (4 deg total vs 3 deg Crisp_Tape).
        l.image_masked_rotated(0.5f * (bx0 + bx1), 0.5f * (by0 + by1), 0.5f * (bx1 - bx0) + px,
                               0.5f * (by1 - by0) + py, -0.01745f, smudge_uv, smudge_uv, ui::fade(ui4::kPink, 0.92),
                               {1.0f, 1.0f, 0.0f, 0.0f}, ui::kMaskBorderTransparent);
        l.image_masked(bx0 - px, by0 - py, bx1 + px, by1 + py, crisp_uv, crisp_uv, ui4::kPink,
                       {1.0f, 1.0f, 0.0f, 0.0f}, ui::kMaskBorderTransparent);
        if (has_dirt) {
          l.image_masked(bx0, by0, bx1, by1, dirt_uv, crisp_uv, ui::rgba(255, 255, 255, 58),
                         {2.5f, 1.0f, 0.0f, 0.0f}, ui::kMaskRepeat);
        }
      } else {
        ui4::brush(l, bx0, by0, bx1, by1, ui4::kPink, true);
      }
      l.text(tx, bottom - it.size, item, it);
    } else {
      ui4::brush(l, bx0, by0, bx1, by1, ui4::kBrushBlack, false);
      if (has_dirt && has_crisp) {
        l.image_masked(bx0, by0, bx1, by1, dirt_uv, crisp_uv, ui::rgba(255, 255, 255, 36),
                       {2.5f, 1.0f, 0.0f, 0.0f}, ui::kMaskRepeat);
      }
      l.text(tx, bottom - it.size, item, it);
    }
  }
  if (first + fit_rows < n) {
    const float end_y = g.T(first_bottom + row_step * float(fit_rows) + 4.0f);
    l.text(g.L(188), end_y, "v MORE", ui4::style(ui::Font::kB, 11 * s, ui4::kDim));
  }
  l.unturn();

  // Optional right-hand detail card (Regional Outpost Hub / Events / Race Central) —
  // authentic FH1 sharp-edged distressed panel + tilted brush title header.
  if (!st.detail_lines.empty() && !st.career) {
    const float x0 = g.R(740), x1 = g.R(1214), y0 = g.T(152);
    const float card_h = std::max(210 * s, (88.0f + 29.0f * float(st.detail_lines.size())) * s);
    ui4::panel(l, x0, y0, x1, y0 + card_h, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    if (!st.detail_title.empty()) {
      Turn dt_turn(l, 0.5f * (x0 + x1), y0 + 24 * s, ui4::kTiltRad * 0.6f);
      TextStyle dt = font(ui::Font::kE, 22 * s, ui4::kWhite);
      fit(st.detail_title, dt, x1 - x0 - 44 * s, 12 * s);
      const float dw = ui::List::measure(st.detail_title, dt);
      ui4::brush(l, x0 - 10 * s, y0 + 10 * s, std::min(x1 + 8 * s, x0 + dw + 40 * s), y0 + 48 * s, ui4::kPink);
      l.text(x0 + 18 * s, y0 + 18 * s, st.detail_title, dt);
    }
    float dy = y0 + 66 * s;
    for (size_t li = 0; li < st.detail_lines.size(); ++li) {
      const auto& line = st.detail_lines[li];
      uint32_t col = ui4::kWhite;
      bool highlight_row = false;
      if (line.rfind("INELIGIBLE", 0) == 0 || line.rfind("LOCKED", 0) == 0) col = rgba(255, 110, 95);
      else if (line.rfind("READY", 0) == 0 || line.rfind("1ST PLACE", 0) == 0 || line.rfind("BONUS CAR", 0) == 0 ||
               line.rfind("DISCOVERED", 0) == 0 || line.rfind("COMPLETED", 0) == 0) {
        col = ui4::kYellow;
        highlight_row = true;
      }
      if (highlight_row) {
        l.rect(x0 + 12 * s, dy - 3 * s, x1 - 12 * s, dy + 22 * s, ui::rgba(245, 4, 92, 46));
      } else if (li % 2 == 1) {
        l.rect(x0 + 12 * s, dy - 3 * s, x1 - 12 * s, dy + 22 * s, ui::rgba(255, 255, 255, 10));
      }
      TextStyle ls = font(ui::Font::kB, 14.5f * s, col);
      ls.keep_case = true;
      fit(line, ls, x1 - x0 - 40 * s, 9 * s);
      l.text(x0 + 20 * s, dy + 1 * s, line, ls);
      dy += 28 * s;
    }
  }
  // Authentic BUTTON_BAR_SCENE bottom-left help strip.
  ui4::button_bar(
      l,
      {{"A", str("HelpButtons", "IDS_Select", "SELECT")},
       {"B", pause ? str("HelpButtons", "IDS_Resume", "RESUME") : str("HelpButtons", "IDS_Back", "BACK")}},
      g.w, g.h);
  if (pause || st.credits >= 0) draw_career_header(l, st, g);
}

// (e) Authentic Forza Horizon Title Screen (120_P_PRE_SPLASH parity).
void title_screen_hud(ui::List& l, const TitleScreenHud& ts, const Layout& g, double time) {
  const float s = g.s;
  static const SceneDraw splash_sc(ui_scene("120_P_PRE_SPLASH"), true);
  // Keep live 3D Colorado & car crisp and open (subtle bottom edge vignette only).
  ui4::subtle_vignette(l, g.w, g.h, ts.menu_phase ? 0.28f : 0.18f);

  std::array<float, 4> logo_uv;
  const bool have_logo = ui_texture("E3/E3LARGELOGO", logo_uv) || ui_texture("E3/E3LargeLogo", logo_uv);

  if (!ts.menu_phase) {
    // 120_P_PRE_SPLASH splash phase: Forza Horizon logo cleanly in upper third
    // + Start@(910, 560) with PILL30PXPINLEFT + CONTROLLER/A_BUTTON so hero car stays unobstructed.
    if (have_logo) {
      const float lw = 540 * s, lh = 162 * s;
      l.image(0.5f * g.w - 0.5f * lw, g.T(64), 0.5f * g.w + 0.5f * lw, g.T(64) + lh, logo_uv);
    } else {
      Turn t(l, 0.5f * g.w, g.T(152));
      ui4::brush(l, g.C(420), g.T(90), g.C(860), g.T(146), ui4::kBrushBlack);
      l.text(0.5f * g.w, g.T(98), "FORZA", font(ui::Font::kE, 44 * s, kText, Align::kCenter));
      ui4::brush(l, g.C(382), g.T(142), g.C(898), g.T(216), ui4::kPink, true);
      l.text(0.5f * g.w, g.T(151), "HORIZON", font(ui::Font::kE, 58 * s, kText, Align::kCenter));
    }
    // 120_P_PRE_SPLASH Start@(910, 612) PILL30PXPINLEFT + CONTROLLER/A_BUTTON (cleanly below hero car).
    float start_cx = g.R(910), start_cy = g.B(612);
    if (splash_sc.ok()) {
      if (const int sn = splash_sc.find("Start"); sn >= 0) {
        const auto pt = splash_sc.at(sn);
        if (pt[0] > 100.0f && pt[0] < 1200.0f) start_cx = g.C(pt[0]);
        if (pt[1] > 560.0f && pt[1] < 660.0f) start_cy = g.T(pt[1]);
      }
    }
    const float pulse = 0.78f + 0.22f * float(std::cos(time * 3.4));
    const std::string prompt = "PRESS [A] OR ENTER TO START";
    TextStyle ps = font(ui::Font::kB, 16 * s, ui::fade(kText, pulse));
    const float pw = ui::List::measure(prompt, ps) + 64 * s;
    const float px0 = std::clamp(start_cx - 0.5f * pw, g.L(80), g.w - pw - g.L(60));
    const float px1 = px0 + pw;
    const float py0 = start_cy - 19 * s, py1 = start_cy + 19 * s;
    std::array<float, 4> pin_uv{};
    if (ui_texture("KITPARTS/PILL30PXPINLEFT", pin_uv)) {
      l.image(px0, py0, px1, py1, pin_uv, ui4::kPillGrey);
    } else {
      ui4::pill(l, px0, py0, px1, py1, ui4::kPillGrey, ui4::PillKind::kMenu);
    }
    std::array<float, 4> abtn;
    const float cy = 0.5f * (py0 + py1);
    if (ui_texture("CONTROLLER/A_BUTTON", abtn))
      l.image(px0 + 14 * s, cy - 11 * s, px0 + 36 * s, cy + 11 * s, abtn, ui::fade(rgba(255, 255, 255), pulse));
    else
      l.circle(px0 + 25 * s, cy, 9.5f * s, ui::fade(kGreen, pulse));
    l.text(px0 + 44 * s, cy - 8.0f * s, prompt, ps);
    l.add_hit(px0, py0, px1, py1, ui::kHitConfirm, 0);
    l.text(g.R(1210), g.B(672), "FORZA HORIZON  //  COLORADO FESTIVAL",
           font(ui::Font::kB, 12 * s, rgba(220, 224, 232, 190), Align::kRight));
    return;
  }

  // Menu phase: logo top-right, fixed-pitch tilted brush menu down the left,
  // compact career save card tucked in the lower-right (never clipping the hero car).
  if (have_logo) {
    l.image(g.R(842), g.T(28), g.R(1214), g.T(140), logo_uv);
  } else {
    Turn t(l, g.R(1028), g.T(78));
    brush(l, g.R(862), g.T(40), g.R(1204), g.T(78), kBrush);
    l.text(g.R(1033), g.T(46), "FORZA", font(ui::Font::kE, 25 * s, kText, Align::kCenter));
    brush(l, g.R(844), g.T(74), g.R(1214), g.T(122), kPink);
    l.text(g.R(1029), g.T(81), "HORIZON", font(ui::Font::kE, 35 * s, kText, Align::kCenter));
  }

  // Left tilted brush menu (120_P_PRE_SPLASH SPLASH_MENU): rock-solid fixed row pitch & dimensions.
  Turn mt(l, g.L(226), g.T(195));
  for (size_t i = 0; i < ts.menu.size(); ++i) {
    const bool sel = !ts.confirm_new_career && int(i) == ts.selected;
    const float bottom = g.T(224 + 54.0f * float(i));
    const std::string& item = ts.menu[i];
    TextStyle it = font(ui::Font::kE, 34 * s, kText);
    fit(item, it, g.L(480), 16 * s);
    const float tw = ui::List::measure(item, it);
    const float bx0 = g.L(156);
    const float by0 = bottom - 34 * s - 6 * s;
    const float bx1 = std::max(g.L(340), g.L(184) + tw + 30 * s);
    const float by1 = bottom + 6 * s;
    if (!ts.confirm_new_career) l.add_hit(bx0, by0, bx1, by1, ui::kHitRow, int(i));
    brush(l, bx0, by0, bx1, by1, sel ? kPink : kBrush);
    l.text(g.L(184), bottom - it.size, item, it);
  }
  l.unturn();

  // Right-side Career Save preview card (or New Career confirmation modal) —
  // authentic FH1 sharp distressed panel tucked neatly in lower-right so hero car stays clear.
  if (ts.confirm_new_career) {
    const float x0 = g.R(750), x1 = g.R(1216), y0 = g.B(376), y1 = g.B(626);
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    {
      Turn ct(l, 0.5f * (x0 + x1), y0 + 26 * s, ui4::kTiltRad * 0.6f);
      ui4::brush(l, x0 - 10 * s, y0 + 12 * s, x0 + 340 * s, y0 + 50 * s, ui4::kPink);
      l.text(x0 + 18 * s, y0 + 19 * s, "START NEW CAREER?", font(ui::Font::kE, 24 * s, kText));
    }
    l.text(x0 + 22 * s, y0 + 68 * s, "THIS WILL OVERWRITE YOUR EXISTING CAREER SAVE",
           font(ui::Font::kB, 13 * s, kDim));
    l.text(x0 + 22 * s, y0 + 88 * s, "AND START FRESH IN THE 1995 VW CORRADO VR6.",
           font(ui::Font::kB, 13 * s, kDim));
    const char* opts[2] = {"CONFIRM NEW CAREER", "CANCEL"};
    for (int i = 0; i < 2; ++i) {
      const bool sel = ts.confirm_selected == i;
      const float by = y0 + 126 * s + float(i) * 46 * s;
      l.add_hit(x0 + 22 * s, by, x1 - 22 * s, by + 36 * s, i == 0 ? ui::kHitConfirm : ui::kHitCancel, i);
      ui4::brush(l, x0 + 22 * s, by, x1 - 22 * s, by + 36 * s, sel ? ui4::kPink : ui4::kBrushBlack);
      l.text(x0 + 38 * s, by + 7 * s, opts[i], font(ui::Font::kE, 20 * s, kText));
    }
  } else if (ts.has_save) {
    const float x0 = g.R(816), x1 = g.R(1216), y0 = g.B(430), y1 = g.B(626);
    const uint32_t bc = ui4::wristband_colour(ts.save_band_name);
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, bc, 0, true);
    {
      Turn stt(l, x0 + 104 * s, y0 + 16 * s, ui4::kTiltRad * 0.5f);
      ui4::brush(l, x0 - 6 * s, y0 + 6 * s, x0 + 174 * s, y0 + 30 * s, ui4::kBrushBlack);
      l.text(x0 + 12 * s, y0 + 12 * s, "SAVED CAREER", font(ui::Font::kE, 13 * s, ui4::kWhite));
    }
    std::string cw = ts.save_band_name.substr(0, ts.save_band_name.find(' '));
    for (char& ch : cw) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    std::array<float, 4> wb_uv;
    const bool have_wb = !cw.empty() && ui_texture("WRISTBANDS/" + cw, wb_uv);
    if (have_wb) l.image(x1 - 144 * s, y0 + 12 * s, x1 - 12 * s, y0 + 46 * s, wb_uv);
    const std::string wb_label = ts.save_band_name.empty() ? std::string("YELLOW WRISTBAND") : ts.save_band_name;
    TextStyle wbs = font(ui::Font::kE, 20 * s, bc);
    fit(wb_label, wbs, (x1 - x0) - (have_wb ? 174 * s : 36 * s), 12 * s);
    l.text(x0 + 18 * s, y0 + 38 * s + (20 * s - wbs.size) * 0.5f, wb_label, wbs);

    char line1[96], line2[96];
    std::snprintf(line1, sizeof line1, "%s PTS  •  POPULARITY %d%s", thousands(ts.save_band_points).c_str(),
                  ts.save_pop_rank, place_suffix(ts.save_pop_rank));
    l.text(x0 + 18 * s, y0 + 66 * s, line1, font(ui::Font::kB, 12 * s, ui4::kYellow));

    l.text(x0 + 18 * s, y0 + 92 * s, "CURRENT CAR", font(ui::Font::kB, 10 * s, kDim));
    TextStyle cn = font(ui::Font::kB, 16 * s, kText);
    cn.keep_case = true;
    fit(ts.save_car_name, cn, x1 - x0 - 122 * s, 10 * s);
    l.text(x0 + 18 * s, y0 + 108 * s, ts.save_car_name, cn);
    if (!ts.save_car_class.empty()) {
      ui4::class_badge(l, x1 - 92 * s, y0 + 104 * s, 21 * s, ts.save_car_class, ts.save_car_pi);
    }
    l.rect(x0 + 18 * s, y0 + 136 * s, x1 - 18 * s, y0 + 137 * s, rgba(255, 255, 255, 35));
    ui4::offwhite_strip(l, x0 + 18 * s, y0 + 148 * s, x0 + 196 * s, y0 + 178 * s, s, true);
    TextStyle cr = font(ui::Font::kE, 18 * s, ui4::kInk);
    cr.keep_case = true;
    l.text(x0 + 28 * s, y0 + 153 * s, thousands(ts.save_credits) + " Cr", cr);
    std::snprintf(line2, sizeof line2, "RACES %d • WINS %d", ts.save_races, ts.save_wins);
    l.text(x1 - 18 * s, y0 + 157 * s, line2, font(ui::Font::kB, 12 * s, kDim, Align::kRight));
  }

  ui4::button_bar(
      l, {{"A", str("HelpButtons", "IDS_Select", "SELECT")}, {"B", str("HelpButtons", "IDS_Back", "BACK")}}, g.w, g.h);
}

// (f) 947_HUD/ActivationPrompt: clean white circular disc icon + large white italic

void stat_tile(ui::List& l, float x0, float y0, float x1, float y1, const std::string& value, const char* name,
               float s, uint32_t colour = kText) {
  panel(l, x0, y0, x1, y1, s, rgba(10, 12, 17, 190));
  TextStyle vs{.size = 26 * s, .weight = 0.12f, .slant = 0.1f, .color = colour};
  while (vs.size > 10 * s && ui::List::measure(value, vs) > x1 - x0 - 32 * s) vs.size *= 0.92f;
  l.text(x0 + 16 * s, y0 + 16 * s + (26 * s - vs.size), value, vs);
  label(l, x0 + 16 * s, y1 - 22 * s, name, s, kDim);
}

// A collection row: the name, "found / total" and a bar.
void collection_row(ui::List& l, float x0, float x1, float y, const char* name, int found, int total, float s,
                    uint32_t colour = kAccent) {
  l.text(x0, y, name, TextStyle{.size = 11 * s, .weight = 0.12f, .tracking = 0.1f, .color = kText});
  char v[48];
  if (total > 0) std::snprintf(v, sizeof v, "%d / %d", found, total);
  else std::snprintf(v, sizeof v, "%d", found);
  l.text(x1, y, v, TextStyle{.size = 11 * s, .weight = 0.11f, .color = kDim, .align = Align::kRight});
  const float by = y + 19 * s, bh = 6 * s;
  l.rect(x0, by, x1, by + bh, rgba(255, 255, 255, 30));
  if (total > 0 && found > 0) {
    const float k = float(std::clamp(double(found) / total, 0.0, 1.0));
    l.rect(x0, by, std::max(x0 + bh, x0 + (x1 - x0) * k), by + bh, found >= total ? kGreen : colour);
  }
}

void career_board(ui::List& l, const CareerBoard& c, float w, float h, float s) {
  if (c.show_sponsors) {
    draw_sponsor_challenges_ui4(l, w, h, c.sponsors, c.sponsor_selected, c.credits);
    return;
  }

  // Authentic H_STATISTICS (+3.0 deg Background@(640,295) floating PILL02 rows + STATBARFILL/EMPTY
  // over open Colorado — ZERO flat dark dashboard box!).
  float tilt_rad = ui4::kTiltRad;
  if (const fh1::UiScene* sc_ptr = ui_scene("H_STATISTICS")) {
    SceneDraw sc(sc_ptr, true);
    if (const int rn = sc.find("Background"); rn >= 0) {
      const float a = -sc.angle(rn);
      if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) tilt_rad = a;
    }
  }

  const float x0 = std::min(w, 466.0f * s) + 18.0f * s;
  const float x1 = w - 62.0f * s;
  if (x1 - x0 < 300.0f * s) return;
  const float rw = x1 - x0;

  ui4::TiltScope rot(l, 0.5f * (x0 + x1), 0.46f * h, tilt_rad);
  float y = 52.0f * s;

  // 1. Floating Wristband Hero Ribbon (WRISTBANDS/<COLOR> + #DAFF00 BARBASE26PX + Off-white Credits)
  const bool band = !c.band_name.empty();
  const uint32_t bc = band ? band_colour(c.band_name) : ui4::kDim;
  ui4::pill(l, x0, y, x1, y + 66.0f * s, ui4::kCharcoalDark, ui4::PillKind::kPillBig, false, true);

  std::string wb_word = c.band_name.substr(0, c.band_name.find(' '));
  for (char& ch_w : wb_word) ch_w = char(std::toupper(static_cast<unsigned char>(ch_w)));
  std::array<float, 4> wb_uv{};
  const bool has_wb = !wb_word.empty() && ui4::texture("WRISTBANDS/" + wb_word, wb_uv);
  const float text_x = has_wb ? (x0 + 124.0f * s) : (x0 + 18.0f * s);
  if (has_wb) l.image(x0 + 12.0f * s, y + 8.0f * s, x0 + 116.0f * s, y + 58.0f * s, wb_uv);

  ui::TextStyle bts = ui4::style(ui::Font::kE, 23.0f * s, bc);
  const std::string btitle = band ? c.band_name : std::string("NO WRISTBAND YET");
  ui4::fit_text(btitle, bts, (x1 - 196.0f * s) - text_x, 12.0f * s);
  l.text(text_x, y + 10.0f * s, btitle, bts);

  // Off-white Credits strip right-aligned inside hero pill
  ui4::offwhite_strip(l, x1 - 186.0f * s, y + 10.0f * s, x1 - 12.0f * s, y + 38.0f * s, s, true);
  ui::TextStyle crs = ui4::style(ui::Font::kB, 15.5f * s, ui4::kInk, Align::kRight, true);
  const std::string crstr = ui4::format_credits(c.credits);
  ui4::fit_text(crstr, crs, 158.0f * s, 9.5f * s);
  l.text(x1 - 20.0f * s, y + 16.0f * s, crstr, crs);

  // #DAFF00 BARBASE26PX wristband progress bar
  const float bx0 = text_x, bx1 = x1 - 16.0f * s, by = y + 44.0f * s, bh = 10.0f * s;
  const float k = c.band_hi > c.band_lo
                      ? float(std::clamp(double(c.points - c.band_lo) / double(c.band_hi - c.band_lo), 0.0, 1.0))
                      : 1.0f;
  std::array<float, 4> bar_uv{};
  const bool has_bar26 = ui_texture("KITPARTS/BARS/BARBASE26PX", bar_uv);
  if (has_bar26) {
    l.image(bx0, by, bx1, by + bh, bar_uv, rgba(255, 255, 255, 42));
    if (band || c.points > 0) l.image(bx0, by, std::max(bx0 + 8.0f * s, bx0 + (bx1 - bx0) * k), by + bh, bar_uv, bc);
  } else {
    ui4::opt_slider(l, bx0, by, bx1, by + bh, k, s, bc, false);
  }
  y += 74.0f * s;

  // 2. H_STATISTICS Header PILL01 (#BFBFBF) + PILLCLIPON01
  ui4::pill(l, x0, y, x1, y + 26.0f * s, rgba(0xBF, 0xBF, 0xBF, 240), ui4::PillKind::kMenu, false, true);
  l.text(x0 + 14.0f * s, y + 6.0f * s, "DRIVER STATISTICS  //  COLORADO", ui4::style(ui::Font::kD, 13.0f * s, ui4::kInk));
  char fame_buf[64];
  std::snprintf(fame_buf, sizeof fame_buf, "LVL %d  •  RANK #%d", std::max(1, c.driver_level), c.popularity_rank);
  l.text(x1 - 14.0f * s, y + 6.0f * s, fame_buf, ui4::style(ui::Font::kE, 12.5f * s, ui4::kInk, Align::kRight));
  y += 31.0f * s;

  // 3. Floating +3.0 deg PILL02 Key Statistic Rows (28px pitch — open Colorado shows cleanly between rows!)
  auto stat_pill_row = [&](std::string_view label_txt, std::string_view val_txt, uint32_t vcol = ui4::kWhite,
                           bool pink = false) {
    ui4::pill(l, x0, y, x1, y + 25.0f * s, pink ? ui4::kPink : ui4::kPillGrey, ui4::PillKind::kPill02);
    l.text(x0 + 14.0f * s, y + 5.5f * s, label_txt,
           ui4::style(ui::Font::kD, 13.5f * s, pink ? ui4::kWhite : ui4::kOffWhite));
    l.text(x1 - 14.0f * s, y + 5.5f * s, val_txt,
           ui4::style(ui::Font::kB, 14.0f * s, vcol, Align::kRight, true));
    y += 28.0f * s;
  };

  char rate_buf[64];
  std::snprintf(rate_buf, sizeof rate_buf, "%d WINS / %d RACES  (%d%%)", c.wins, c.races,
                c.races > 0 ? int(std::lround(100.0 * c.wins / c.races)) : 0);
  stat_pill_row("RACE VICTORIES", rate_buf, ui4::kYellow);
  stat_pill_row("PODIUM FINISHES", std::to_string(c.podiums), ui4::kWhite);
  stat_pill_row("TOTAL CAREER WINNINGS", ui4::format_credits(c.winnings), ui4::kGreen);
  stat_pill_row("CARS IN GARAGE", std::to_string(c.garage) + (c.garage == 1 ? " CAR" : " CARS"), ui4::kWhite);

  y += 6.0f * s;

  // 4. Floating +3.0 deg PILL02 Collection Progress Rows with DIRTTEXTURES/STATBARFILL & STATBAREMPTY!
  ui4::pill(l, x0, y, x1, y + 25.0f * s, rgba(0xBF, 0xBF, 0xBF, 240), ui4::PillKind::kMenu, false, true);
  l.text(x0 + 14.0f * s, y + 5.5f * s, "FESTIVAL & EXPLORATION PROGRESS",
         ui4::style(ui::Font::kD, 13.0f * s, ui4::kInk));
  y += 29.0f * s;

  std::array<float, 4> sb_empty{}, sb_fill{};
  const bool has_sbe = ui_texture("DIRTTEXTURES/STATBAREMPTY", sb_empty);
  const bool has_sbf = ui_texture("DIRTTEXTURES/STATBARFILL", sb_fill);

  auto progress_pill_row = [&](std::string_view name, int done, int total, uint32_t bar_col) {
    if (y + 27.0f * s > h - 76.0f * s) return;
    ui4::pill(l, x0, y, x1, y + 25.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
    l.text(x0 + 14.0f * s, y + 5.5f * s, name, ui4::style(ui::Font::kD, 12.5f * s, ui4::kWhite));

    const float sbx0 = x0 + rw * 0.44f;
    const float sbx1 = x1 - 92.0f * s;
    const float sby0 = y + 7.0f * s, sby1 = y + 18.0f * s;
    const float frac = total > 0 ? std::clamp(float(done) / float(total), 0.0f, 1.0f) : 0.0f;
    if (has_sbe) l.image(sbx0, sby0, sbx1, sby1, sb_empty, rgba(255, 255, 255, 110));
    else l.rect(sbx0, sby0, sbx1, sby1, rgba(255, 255, 255, 30));
    if (frac > 0.005f) {
      const float fx1 = sbx0 + (sbx1 - sbx0) * frac;
      if (has_sbf) {
        const float u1 = sb_fill[0] + (sb_fill[2] - sb_fill[0]) * frac;
        l.image(sbx0, sby0, fx1, sby1, {sb_fill[0], sb_fill[1], u1, sb_fill[3]}, bar_col);
      } else {
        l.rect(sbx0, sby0, fx1, sby1, bar_col);
      }
    }

    char cnt[32];
    std::snprintf(cnt, sizeof cnt, "%d / %d", done, total);
    l.text(x1 - 14.0f * s, y + 5.5f * s, cnt,
           ui4::style(ui::Font::kB, 12.5f * s, (done >= total && total > 0) ? ui4::kGreen : ui4::kOffWhite,
                      Align::kRight));
    y += 28.0f * s;
  };

  progress_pill_row("ROADS DISCOVERED", c.roads, c.roads_total, ui4::kGreen);
  if (c.outposts_total > 0) {
    progress_pill_row("HORIZON OUTPOSTS", c.outposts, c.outposts_total, rgba(255, 146, 3));
    progress_pill_row("OUTPOST PR STUNTS", c.pr_stunts, c.pr_stunts_total, ui4::kYellow);
  }
  if (c.speed_traps_total > 0 || c.speed_zones_total > 0) {
    progress_pill_row("SPEED TRAPS", c.speed_traps, c.speed_traps_total, ui4::kBlue);
    progress_pill_row("SPEED ZONES", c.speed_zones, c.speed_zones_total, ui4::kPink);
  }
  progress_pill_row("DISCOUNT SIGNS", c.signs, c.signs_total, ui4::kPink);
  progress_pill_row("BARN FINDS", c.barns, c.barns_total, ui4::kYellow);
  progress_pill_row("1000 CLUB MEDALS", c.club_done, c.club_total, ui4::kAmber);
  if (c.rally) {
    progress_pill_row("HORIZON RALLY WINS", c.rally_wins, c.rallies, ui4::kBlue);
  }
  progress_pill_row("CONTENT PACKS", c.packs_owned, c.packs_total, rgba(200, 205, 215));
}


}  // namespace carview::hud_detail

namespace carview {

using namespace hud_detail;

void draw_sponsor_challenges_ui4(ui::List& l, float w, float h, const std::vector<SponsorChallengeRow>& rows,
                                 int selected, int64_t credits) {
  const float s = h / 720.0f;
  ui4::subtle_vignette(l, w, h, 0.22f);

  float tilt_rad = ui4::kTiltRad;
  if (const fh1::UiScene* sc_ptr = ui_scene("H_SPONSOR_CHALLENGE_STATUS")) {
    SceneDraw sc(sc_ptr, true);
    if (const int rn = sc.find("SponsorBars"); rn >= 0) {
      const float a = -sc.angle(rn);
      if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) tilt_rad = a;
    }
  }

  // Top-right Credits strip (PlayerBar@(1207,38))
  if (credits >= 0) {
    const float rx = w - 60.0f * s;
    ui4::offwhite_strip(l, rx - 186.0f * s, 30.0f * s, rx, 58.0f * s, s, true);
    l.text(rx - 14.0f * s, 36.0f * s, ui4::format_credits(credits),
           ui4::style(ui::Font::kB, 16.5f * s, ui4::kInk, Align::kRight, true));
  }

  // ---- H_SPONSOR_CHALLENGE_STATUS: SponsorBars@(630,322) tilted +3.0 deg over open Colorado ----
  {
    ui4::TiltScope rot(l, 0.5f * w, 0.48f * h, tilt_rad);

    ui4::title_tape(l, s, 96.0f * s, 68.0f * s, "SPONSOR CHALLENGES", ui4::kPink, 36.0f, 560.0f);

    const int n = int(rows.size());
    const int sel = n > 0 ? std::clamp(selected, 0, n - 1) : 0;
    const int max_vis = 6;
    const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, n - max_vis));

    std::array<float, 4> bar26_uv{}, obj_uv{};
    const bool has_bar26 = ui_texture("KITPARTS/BARS/BARBASE26PX", bar26_uv);
    const bool has_obj = ui_texture("HUD/OBJECTIVEICONS/OBJECTIVEICONSMALL", obj_uv);

    const float row_x0 = 98.0f * s;
    const float row_x1 = w - 98.0f * s;
    const float pitch = 69.0f * s;
    const float row_h = 62.0f * s;
    float ry = 152.0f * s;

    for (int i = first; i < std::min(n, first + max_vis); ++i) {
      const auto& sp = rows[size_t(i)];
      const bool act = (i == sel);

      // Floating row pill (NO full-screen charcoal wall!)
      l.add_hit(row_x0, ry, row_x1, ry + row_h, ui::kHitRow, i);
      ui4::pill(l, row_x0, ry, row_x1, ry + row_h, act ? ui4::kPink : ui4::kPillGrey, ui4::PillKind::kPillBig, false,
                act);

      // Authentic 128x64 SponsorLogoTile/*.xds logo ([112,203..240,267] in scene coordinates)
      const float lx0 = row_x0 + 10.0f * s, ly0 = ry + 4.0f * s;
      const float lw = 112.0f * s, lh = 54.0f * s;
      l.rect(lx0, ly0, lx0 + lw, ly0 + lh, ui4::kWhite);
      const std::string key = sp.logo_key.empty() ? sp.sponsor_name : sp.logo_key;
      if (!ui4::sponsor_logo_tile(l, lx0 + 2.0f * s, ly0 + 2.0f * s, lx0 + lw - 2.0f * s, ly0 + lh - 2.0f * s, key)) {
        l.text(lx0 + 0.5f * lw, ly0 + 18.0f * s, sp.sponsor_name,
               ui4::style(ui::Font::kE, 14.0f * s, ui4::kInk, Align::kCenter));
      }

      // TEXT_SPONSOR (D@18px) + TEXT_TITLE (D@16px)
      const float tx = lx0 + lw + 18.0f * s;
      ui::TextStyle sn = ui4::style(ui::Font::kD, 17.5f * s, ui4::kWhite);
      ui4::fit_text(sp.sponsor_name, sn, 260.0f * s, 11.0f * s);
      l.text(tx, ry + 10.0f * s, sp.sponsor_name, sn);

      ui::TextStyle ct = ui4::style(ui::Font::kB, 13.5f * s, act ? ui4::kOffWhite : ui4::kDim);
      ui4::fit_text(sp.challenge_title, ct, 260.0f * s, 9.5f * s);
      l.text(tx, ry + 34.0f * s, sp.challenge_title, ct);

      // #DAFF00 BARBASE26PX Progress Bar ([521..837] in scene coordinates)
      const float bx0 = row_x0 + 416.0f * s;
      const float bx1 = row_x1 - 220.0f * s;
      const float by0 = ry + 24.0f * s, by1 = ry + 44.0f * s;
      const float frac = sp.target > 0 ? std::clamp(float(sp.current) / float(sp.target), 0.0f, 1.0f) : 1.0f;

      char tbuf[48];
      std::snprintf(tbuf, sizeof tbuf, "TIER %d/%d   •   %d / %d", sp.tier, std::max(1, sp.max_tiers), sp.current,
                    sp.target);
      l.text(bx0, ry + 7.0f * s, tbuf, ui4::style(ui::Font::kB, 11.5f * s, act ? ui4::kWhite : ui4::kOffWhite));

      if (has_bar26) {
        l.image(bx0, by0, bx1, by1, bar26_uv, rgba(0, 0, 0, 145));
        if (frac > 0.005f) {
          l.image(bx0, by0, std::max(bx0 + 10.0f * s, bx0 + (bx1 - bx0) * frac), by1, bar26_uv, ui4::kYellow);
        }
      } else {
        l.rect(bx0, by0, bx1, by1, rgba(0, 0, 0, 145));
        if (frac > 0.005f) l.rect(bx0, by0, bx0 + (bx1 - bx0) * frac, by1, ui4::kYellow);
      }

      // Reward strip on right + optional OBJECTIVEICONSMALL
      const float rx = row_x1 - 16.0f * s;
      if (has_obj) {
        l.image(bx1 + 16.0f * s, ry + 20.0f * s, bx1 + 40.0f * s, ry + 44.0f * s, obj_uv, ui4::kYellow);
      }
      l.text(rx, ry + 12.0f * s, "NEXT REWARD",
             ui4::style(ui::Font::kB, 10.5f * s, act ? ui4::kOffWhite : ui4::kDim, Align::kRight));
      l.text(rx, ry + 29.0f * s, "+" + ui4::format_credits(sp.reward_cr),
             ui4::style(ui::Font::kE, 17.0f * s, ui4::kYellow, Align::kRight, true));
      ry += pitch;
    }

    // Authentic SCROLLINDICATOR + SPEC_SHEEN2
    ui4::scroll_indicator(l, row_x1 + 10.0f * s, 152.0f * s, row_x1 + 18.0f * s, ry - 8.0f * s, first, max_vis, n, s);
  }

  ui4::button_bar(l, {{"DPAD_UD", "BROWSE SPONSORS"}, {"B", "BACK"}}, w, h, 56.0f);
}

}  // namespace carview
