#include "game_hud_internal.h"

namespace carview::hud_detail {

void event_card(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  static const SceneDraw sc(ui_scene("PRE_AND_POST_RACE"), true);
  float tilt_rad = ui4::kTiltRad;
  if (sc.ok()) {
    if (const int rh = sc.find("RotationHelper"); rh >= 0) {
      const float a = -sc.angle(rh);
      if (std::fabs(a) > 1e-3f) tilt_rad = a;
    }
  }
  // Lift the card slightly when the grid menu's help bar sits beneath it.
  const float lift = r.grid_menu_open ? 34.0f : 0.0f;
  // Full-bleed-left pink brush with event name, overlapping black MiniTape with event type.
  {
    l.turn(g.L(484), g.B(597 - lift), tilt_rad);
    ui4::brush(l, -24 * s, g.B(547 - lift), g.L(968), g.B(647 - lift), ui4::kPink, true);
    TextStyle ns = font(ui::Font::kE, 47 * s, kText);
    fit(r.event, ns, g.L(940) - g.L(100), 20 * s);
    l.text(g.L(100), g.B(588 - lift) + (47 * s - ns.size) * 0.5f, r.event, ns);
    l.unturn();
  }
  {
    l.turn(g.L(257), g.B(562 - lift), tilt_rad);
    const std::string type =
        !r.event_type.empty() ? r.event_type : str("CareerEventUI", "IDS_TitleFestivalRace", "FESTIVAL RACE");
    TextStyle ts = font(ui::Font::kE, 36 * s, kText);
    fit(type, ts, g.L(440) - g.L(100), 18 * s);
    const float tw = ui::List::measure(type, ts);
    ui4::brush(l, g.L(75), g.B(535 - lift), std::max(g.L(420), g.L(100) + tw + 34 * s), g.B(588 - lift),
               ui4::kBrushBlack, false);
    l.text(g.L(100), g.B(544 - lift) + (36 * s - ts.size) * 0.5f, type, ts);
    l.unturn();
  }
  if (!r.grid_menu_open || r.grid_menu.empty()) return;
  // Tilted left-side Grid Menu (PRE_AND_POST_RACE MENU / race-grid.png: strictly fixed row pitch & dimensions).
  l.turn(g.L(226), g.T(195), tilt_rad);
  for (size_t i = 0; i < r.grid_menu.size(); ++i) {
    const bool sel = int(i) == r.grid_selected;
    const float bottom = g.T(210 + 52.0f * float(i));
    const std::string& item = r.grid_menu[i];
    TextStyle it = font(ui::Font::kE, 35 * s, kText);
    fit(item, it, g.L(680), 14 * s);
    const float tw = ui::List::measure(item, it);
    const float bx0 = g.L(152);
    const float by0 = bottom - it.size - 7 * s;
    const float bx1 = std::max(g.L(340), g.L(184) + tw + 32 * s);
    const float by1 = bottom + 6 * s;
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
    l.text(g.L(184), bottom - it.size, item, it);
  }
  l.unturn();
  // Match retail race-grid.png: only A SELECT on Grid Menu (prevents accidental race quits).
  ui4::button_bar(l, {{"A", str("HelpButtons", "IDS_Select", "SELECT")}}, g.w, g.h);
}

// (b2) The race HUD: laps (or the distance), the timers, place, standings.
void race_hud(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  const bool racing = r.phase == RaceHud::kRacing;
  // 947_HUD's EndConditionBlock and PlaceBlock (E 48 current, E 26 slash
  // and total, E 16 label; right-aligned boxes end at their node); the
  // drop shadow is ours. Without the scene: the drawn blocks below.
  static const SceneDraw sc(ui_scene("947_HUD"));
  const int endcon = sc.find("EndConditionBlock"), placeblk = sc.find("PlaceBlock");
  const bool scened = endcon >= 0 && placeblk >= 0;
  auto stext = [&](const SceneFrame& f, int node, const std::string& t) {
    sc.text(l, f, node, t, rgba(0, 0, 0, 140), 1.5f, 1.5f, false);
    sc.text(l, f, node, t, kText, 0, 0, false);
  };
  if (scened) {
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
  {
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
  if (!r.rows.empty()) {
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
    } else if (racing && r.clock < 40.0 / 60.0) {
      const float a = 1 - clamp01((r.clock - 0.5) / 0.17);
      if (go_node >= 0) {
        sc.text(l, cf, go_node, "GO!", ui::fade(kSoft, a), 2.0f, 2.0f, false);
        sc.text(l, cf, go_node, "GO!", ui::fade(kText, a), 0, 0, false);
      } else {
        shadowed(l, g.C(637.5f), g.T(230), "GO!", font(ui::Font::kE, 64 * s, ui::fade(kText, a), Align::kCenter), s);
      }
    }
  }
  // Wrong way: 947_HUD's WrongWay sign (WRONGWAYICON, 582,174-698,290),
  // pulsing (ours: the scene's animation is not read); without the
  // texture, a red plate.
  std::array<float, 4> ww_uv;
  if (racing && r.wrong_way && ui_texture("HUD/WRONGWAYICON", ww_uv)) {
    const float pulse = 0.75f + 0.25f * float(std::cos(r.clock * 6.2832 * 1.5));
    l.image(g.C(582), g.T(174), g.C(698), g.T(290), ww_uv, ui::fade(rgba(255, 255, 255), pulse));
  } else if (racing && r.wrong_way) {
    const float pulse = 0.75f + 0.25f * float(std::cos(r.clock * 6.2832 * 1.5));
    const TextStyle ws = font(ui::Font::kE, 30 * s, kText, Align::kCenter);
    const float ww = ui::List::measure("WRONG WAY", ws) + 60 * s, wy = g.T(140);
    l.rect(0.5f * g.w - 0.5f * ww, wy, 0.5f * g.w + 0.5f * ww, wy + 48 * s, ui::fade(kRed, pulse));
    l.text(0.5f * g.w, wy + 9 * s, "WRONG WAY", ws);
  }
}

// (c) After the finish: the FINISHED banner with the place, then the cash
// reward page.

// The RACE RESULTS table (SPEC-UI-LAYOUTS c, the race results table; X
// from the FINISHED banner, B back): H_POST_RACE_RESULTS as laid out, each
// node turned by its own rotation; the rows' text from the session. The
// scene's opacities are its intro's start (0): drawn at full.
void results_table(ui::List& l, const RaceHud& r, const Layout& g) {
  static const SceneDraw sc(ui_scene("H_POST_RACE_RESULTS"), true);
  const float s = g.s;
  ui4::button_bar(l, {{"B", str("HelpButtons", "IDS_Back", "BACK")}}, g.w, g.h);
  if (!sc.ok()) return;
  const SceneFrame f{0.5f * g.w - 640 * s, 0.5f * g.h - 360 * s, s};
  sc.model(l, f, sc.find("BG_Noise"), 0.5f);
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

// (c) The wristband points page (SPEC-UI-LAYOUTS, the wristband points
// banner, measured): POINTS EARNED on a black brush, the band's large
// wristband, COLLECT IT FROM RACE CENTRAL on a light panel when a level is
// reached (else the band's progress, ours), PLEASE WAIT bottom left. The
// scene (H_POST_RACE_CAREER_POINTS) starts off screen and its animation is
// not read: the frame's rects stand.
void points_page(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
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
    const std::string title = str("PostRaceFlow", "IDS_PointsEarned", "POINTS EARNED");
    TextStyle ts = font(ui::Font::kE, 56 * s, kText);
    fit(title, ts, g.L(570) - g.L(108), 24 * s);
    l.text(g.L(108), g.T(166) + (56 * s - ts.size) * 0.5f, title, ts);
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
    const double since = r.clock - r.finish_time;
    const float anim = ease_in_time(since, 0.8, 1.2);
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
  // PLEASE WAIT: rotating festival spinner + sharp HUDPILL01.
  std::array<float, 4> sp;
  if (ui_texture("COMMON/LOADSPINNER", sp) || ui_texture("LOADING/LOADINGSPINNER50PX", sp)) {
    l.image_rotated(g.L(86), g.B(664), 24 * s, 24 * s, float(r.clock * 2.8), sp, ui4::kWhite);
  }
  ui4::pill(l, g.L(112), g.B(652), g.L(242), g.B(680), ui4::kPillGrey, ui4::PillKind::kHud01);
  l.text(g.L(122), g.B(659), str("Main", "IDS_Please_Wait_Title", "PLEASE WAIT"), font(ui::Font::kB, 14.5f * s, kText));
}

// (c4) Prize Car Won screen (H_POST_RACE_WON_CAR style).
void won_car_page(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  ui4::subtle_vignette(l, g.w, g.h, 0.30f);
  {
    Turn t(l, g.L(340), g.T(215));
    TextStyle ts = font(ui::Font::kE, 48 * s, kText);
    const float tw = ui::List::measure("NEW CAR WON!", ts);
    brush(l, g.L(92), g.T(178), std::max(g.L(560), g.L(112) + tw + 38 * s), g.T(250), kBrush);
    l.text(g.L(112), g.T(188), "NEW CAR WON!", ts);
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
    ui4::offwhite_strip(l, g.L(105), g.T(378), g.L(470), g.T(418), s, true);
    l.text(g.L(122), g.T(389), "ADDED TO YOUR GARAGE", font(ui::Font::kB, 18 * s, kDark));
  }
  ui4::button_bar(l, {{"A", str("HelpButtons", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
}

void results(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  const double since = r.clock - r.finish_time;
  if (r.results_page == 3 && !r.won_car_name.empty()) {
    won_car_page(l, r, g);
    return;
  }
  if (r.results_page == 2) {
    points_page(l, r, g);
    return;
  }
  if (r.results_page == 0 && r.results_table) {
    results_table(l, r, g);
    return;
  }
  if (r.results_page == 0) {
    const float in = ease_in_time(since, 0, 0.35);
    const float dx = (1 - in) * -120 * s;
    {
      Turn t(l, g.L(376) + dx, g.T(270));
      brush(l, dx, g.T(204), g.L(753) + dx, g.T(337), kPink);
      const int place = r.stage ? r.stage_place : r.place;
      char p[16];
      std::snprintf(p, sizeof p, "%d%s ", place, place_suffix(place));
      const std::string text = p + str("InGame", "IDS_Place", "PLACE");
      TextStyle ps = font(ui::Font::kE, 80 * s, kText);
      fit(text, ps, g.L(735) - g.L(95), 30 * s);
      l.text(g.L(95) + dx, g.T(230) + (80 * s - ps.size) * 0.5f, text, ps);
    }
    {
      Turn t(l, g.L(229) + dx, g.T(200));
      brush(l, g.L(75) + dx, g.T(167), g.L(383) + dx, g.T(233), kBrush);
      TextStyle fs = font(ui::Font::kE, 45 * s, kText);
      fit(str("InGame", "IDS_Finished", "FINISHED"), fs, g.L(372) - g.L(85), 20 * s);
      l.text(g.L(85) + dx, g.T(171), str("InGame", "IDS_Finished", "FINISHED"), fs);
    }
    ui4::button_bar(l,
                    {{"A", str("HelpButtons", "IDS_ContinueMenu", "CONTINUE")},
                     {"Y", str("HelpButtons", "IDS_RestartMenu", "RESTART")},
                     {"X", str("InGame", "IDS_RaceResultsMenu", "RACE RESULTS")},
                     {"V", "VIEW REPLAY"}},
                    g.w, g.h);
    return;
  }
  // The cash reward (oracle results-credits.png).
  ui4::subtle_vignette(l, g.w, g.h, 0.25f);
  {
    Turn t(l, g.L(313), g.T(180));
    brush(l, g.L(98), g.T(141), g.L(529), g.T(218), kBrush);
    TextStyle ts = font(ui::Font::kE, 57 * s, kText);
    const std::string title = str("ScreenTitles", "IDS_PostRace_Winnings", "CASH REWARD");
    fit(title, ts, g.L(519) - g.L(108), 24 * s);
    l.text(g.L(108), g.T(151), title, ts);
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
  if (r.nemesis_bonus > 0) rows.emplace_back("NEMESIS BONUS", thousands(r.nemesis_bonus) + " " + cr);
  if (r.bounty_bonus > 0) rows.emplace_back("BOUNTY BONUS", thousands(r.bounty_bonus) + " " + cr);
  if (r.driver_xp > 0) {
    std::snprintf(xp_buf, sizeof xp_buf, "+%s XP  •  AFFINITY +%s XP", thousands(r.driver_xp).c_str(),
                  thousands(r.affinity_xp).c_str());
    rows.emplace_back("DRIVER & AFFINITY XP", xp_buf);
  }
  const float row_step = rows.size() > 4 ? 24.0f : 28.0f;
  for (size_t i = 0; i < rows.size(); ++i) {
    const float y = g.T(234 + row_step * float(i));
    const TextStyle ls = font(ui::Font::kB, (rows.size() > 5 ? 16 : 20) * s, kText);
    TextStyle vs = font(ui::Font::kE, (rows.size() > 5 ? 15 : 18) * s, kText);
    vs.keep_case = true;
    const float lw = ui::List::measure(rows[i].first, ls), vw = ui::List::measure(rows[i].second, vs);
    Turn t(l, g.L(148) + 0.5f * lw, y + 10 * s);
    brush(l, g.L(140), y - 4 * s, g.L(148) + lw + 6 * s, y + 22 * s, kBrush);
    l.text(g.L(148), y, rows[i].first, ls);
    const float vx = g.L(148) + lw + 10 * s;
    brush(l, vx - 4 * s, y - 2 * s, vx + vw + 6 * s, y + 20 * s, kPink);
    l.text(vx, y + 1 * s, rows[i].second, vs);
  }
  // TOTAL and its value (out/oracle/ui/results-credits.png: wide left-bleeding dark
  // feathered brush behind TOTAL seamlessly meeting the hot pink total value brush!).
  const float count = ease_in_time(since, 0.3, 1.0);
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
  const std::string total = thousands(int64_t(std::llround(double(r.credits) * count))) + " " + cr;
  fit(total, vs, g.L(860) - g.L(490), 30 * s);
  l.text(g.L(860), g.T(377) + (70 * s - vs.size) * 0.5f, total, vs);
  l.unturn();
  if (r.balance >= 0) {
    Turn tb(l, g.L(280), g.T(497));
    ui4::offwhite_strip(l, g.L(150), g.T(475), g.L(410), g.T(520), s, true);
    TextStyle bs = font(ui::Font::kB, 16 * s, kDark);
    l.text(g.L(162), g.T(490), str("OnlineRewards", "IDS_Balance", "BALANCE"), bs);
    bs.align = Align::kRight;
    bs.keep_case = true;  // "117,881 Cr" as shown
    l.text(g.L(398), g.T(490), thousands(r.balance) + " " + cr, bs);
  }
  ui4::button_bar(l, {{"A", str("HelpButtons", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
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


void showcase_hud(ui::List& l, const RaceHud& r, const Layout& g) {
  if (!r.showcase || r.phase != RaceHud::kRacing) return;
  const float s = g.s;
  const float cx = 0.5f * g.w;
  const float bw = 440 * s, bh = 54 * s;
  const float x0 = cx - 0.5f * bw, x1 = cx + 0.5f * bw;
  const float y0 = g.T(24), y1 = y0 + bh;
  ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  char gap_buf[48];
  const int gm = int(std::llround(std::fabs(r.showcase_gap_m)));
  std::snprintf(gap_buf, sizeof gap_buf, "%s %d M", r.showcase_gap_m >= 0 ? "AHEAD" : "BEHIND", gm);
  l.text(x0 + 12 * s, y0 + 10 * s, "SHOWCASE  //  VS " + r.showcase_aircraft, font(ui::Font::kB, 13 * s, kText));
  l.text(x1 - 12 * s, y0 + 10 * s, gap_buf,
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
