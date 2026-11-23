#include "game_hud_internal.h"

namespace carview::hud_detail {

void world_marker_hud(ui::List& l, const WorldMarkerHud& wm, const Layout& g) {
  if (!wm.visible) return;
  const float s = g.s;
  const float mx = wm.in_range ? std::clamp(wm.screen_x, g.L(160), g.w - g.L(320)) : wm.screen_x;
  const float my = wm.in_range ? std::clamp(wm.screen_y, g.T(120), g.h - g.T(185)) : wm.screen_y;
  if (mx < -100 || mx > g.w + 100 || my < -80 || my > g.h + 80) return;

  // Clean white circular event/festival badge (matching hud-freeroam-event-marker.png).
  const float r = (wm.in_range ? 20.0f : 15.0f) * s;
  l.circle(mx + 1.5f * s, my + 2.0f * s, r + 1.0f * s, rgba(0, 0, 0, 145));
  l.circle(mx, my, r, ui4::kWhite);
  l.circle(mx, my, r - 3.2f * s, rgba(24, 26, 30, 235));
  l.text(mx, my - (wm.in_range ? 9.5f : 7.5f) * s, wm.is_festival ? "H" : "!",
         font(ui::Font::kE, (wm.in_range ? 18 : 14) * s, ui4::kWhite, Align::kCenter));

  // Slightly tilted large white italic title with crisp drop shadow (NO dark subtitle box!).
  const float tx = mx + r + 10 * s;
  Turn t(l, tx + 110 * s, my, ui4::kTiltRad * 0.45f);
  TextStyle ts = font(ui::Font::kE, (wm.in_range ? 32 : 22) * s, ui4::kWhite);
  fit(wm.title, ts, g.L(420), 15 * s);
  const float tw = ui::List::measure(wm.title, ts);
  // Crisp multi-sample drop shadow for bright sky readability (just like retail FH1).
  TextStyle sh = ts;
  sh.color = rgba(0, 0, 0, 185);
  l.text(tx + 2.0f * s, my - 0.55f * ts.size + 2.0f * s, wm.title, sh);
  l.text(tx, my - 0.55f * ts.size, wm.title, ts);

  if (wm.in_range) {
    // Compact #F90064 ENTER NOW [X] strip tucked flush beneath the right half of the title.
    const std::string ptxt = wm.prompt.empty() ? str("Activities", "IDS_EnterNow", "ENTER NOW") : wm.prompt;
    TextStyle ps = font(ui::Font::kE, 16.5f * s, ui4::kWhite);
    const float pw = ui::List::measure(ptxt, ps);
    const float strip_w = pw + 42 * s;
    const float sx0 = std::max(tx + 10 * s, tx + tw - strip_w + 4 * s);
    const float sy0 = my + 0.45f * ts.size + 3 * s;
    const float sy1 = sy0 + 27 * s;
    ui4::brush(l, sx0 - 4 * s, sy0, sx0 + strip_w, sy1, wm.eligible ? ui4::kPink : rgba(110, 110, 118, 230));
    l.text(sx0 + 8 * s, sy0 + 5 * s, ptxt, ps);
    const float bx = sx0 + pw + 24 * s, by = 0.5f * (sy0 + sy1);
    std::array<float, 4> xuv;
    if (ui_texture("CONTROLLER/X_BUTTON", xuv)) {
      l.image(bx - 10 * s, by - 10 * s, bx + 10 * s, by + 10 * s, xuv);
    } else {
      l.circle(bx, by, 9 * s, kBlue);
      l.text(bx, by - 5 * s, "X", font(ui::Font::kB, 10 * s, kText, Align::kCenter));
    }
  } else if (wm.distance_m > 0) {
    char db[24];
    std::snprintf(db, sizeof db, "%.0f M", wm.distance_m);
    shadowed(l, tx, my + 0.5f * ts.size + 4 * s, db, font(ui::Font::kB, 12.5f * s, ui4::kOffWhite), s);
  }
}

// (g) 947_HUD/PopularityBar & 947_HUD/DrivingSkill (exact match to out/oracle/ui/race-hud.png):
// Bright silver-white POPULARITYBARBACKGROUND disc (NO black cover circle!), #DAFF00 radial
// progress ring, BLACK italic rank number with inline superscript ("165TH"), and clean
// drop-shadowed #DAFF00 score + white X1.0 + skill callout beneath.
void skill_chain_hud(ui::List& l, const SkillChainHud& sk, const Layout& g, double time) {
  const double since_bank = time - sk.banked_at;
  const double since_lost = time - sk.lost_at;
  const double since_rank = time - sk.rank_up_at;
  const bool show_bank = since_bank >= 0 && since_bank < 3.5;
  const bool show_lost = since_lost >= 0 && since_lost < 2.2;
  const bool show_rank = since_rank >= 0 && since_rank < 4.0;
  if (!sk.active && !show_bank && !show_lost && !show_rank) return;

  const float s = g.s;
  const float cx = g.C(640), cy = g.T(62);
  const float r = 34 * s;
  constexpr uint32_t kSkillYellow = ui4::kYellow;

  // 1. 947_HUD/PopularityBar textures: shadow -> opaque silver-white centre disc ->
  //    POPULARITYBARBACKGROUND rim -> #DAFF00 radial fill -> highlight (matches race-hud.png!).
  std::array<float, 4> psh{}, pbg{}, phi{};
  if (ui_texture("HUD/POPULARITYBAR/POPULARITYBARSHADOW", psh)) {
    l.image(cx - 42 * s, cy - 42 * s, cx + 42 * s, cy + 42 * s, psh, rgba(0, 0, 0, 165));
  }
  l.circle(cx, cy, r - 1.5f * s, rgba(226, 228, 232, 236));
  l.circle(cx, cy, r - 6.5f * s, rgba(242, 244, 248, 246));
  if (ui_texture("HUD/POPULARITYBAR/POPULARITYBARBACKGROUND", pbg)) {
    l.image(cx - 38 * s, cy - 38 * s, cx + 38 * s, cy + 38 * s, pbg, ui4::kWhite);
  }
  // Radial progress ring along the outer groove of POPULARITYBARBACKGROUND.
  const float sweep = float(std::clamp(sk.popularity_progress, 0.05, 1.0)) * 6.28318f;
  l.arc(cx, cy, r - 4.0f * s, 4.8f * s, -1.5708f, -1.5708f + sweep, show_rank ? ui4::kPink : kSkillYellow, true);
  if (ui_texture("HUD/POPULARITYBAR/POPULARITYBARHIGHLIGHT", phi)) {
    l.image(cx - 38 * s, cy - 38 * s, cx + 38 * s, cy + 38 * s, phi, rgba(255, 255, 255, 110));
  }

  // BLACK italic rank number with inline superscript suffix ("165TH") cleanly inside inner disc!
  char rbuf[16];
  std::snprintf(rbuf, sizeof rbuf, "%d", sk.popularity_rank);
  const char* suf = place_suffix(sk.popularity_rank);
  const float rsz = sk.popularity_rank >= 100 ? 15.5f : 19.0f;
  const float ssz = sk.popularity_rank >= 100 ? 8.8f : 10.5f;
  TextStyle num_ts = font(ui::Font::kE, rsz * s, ui4::kBrushBlack);
  TextStyle suf_ts = font(ui::Font::kE, ssz * s, ui4::kBrushBlack);
  const float nw = ui::List::measure(rbuf, num_ts);
  const float sw = ui::List::measure(suf, suf_ts);
  const float total_w = nw + sw + 1.0f * s;
  const float tx0 = cx - 0.5f * total_w;
  const float ty = cy - 0.52f * num_ts.size;
  l.text(tx0, ty, rbuf, num_ts);
  l.text(tx0 + nw + 1.0f * s, ty + 1.5f * s, suf, suf_ts);

  // 2. 947_HUD/DrivingSkill: #DAFF00 score + white X1.0 + clean drop-shadowed skill callout
  //    (optional CRISPTAPESKILLS + SKILLICONSET, ZERO dark rectangle behind the skill name!).
  if (sk.active) {
    char pts_buf[32], mult_buf[16];
    std::snprintf(pts_buf, sizeof pts_buf, "%s", thousands(sk.chain_points).c_str());
    std::snprintf(mult_buf, sizeof mult_buf, " X%d.0", sk.multiplier);
    TextStyle pts_ts = font(ui::Font::kE, 26 * s, kSkillYellow);
    TextStyle mul_ts = font(ui::Font::kE, 22 * s, ui4::kWhite);
    const float pw = ui::List::measure(pts_buf, pts_ts);
    const float mw = ui::List::measure(mult_buf, mul_ts);
    const float sx0 = cx - 0.5f * (pw + mw);

    // Subtle CRISPTAPESKILLS/CIRCLEFLASH glow when chaining high scores.
    std::array<float, 4> flash_uv{}, icon_uv{};
    if (ui_texture("HUD/DRIVINGSKILLS/CIRCLEFLASH", flash_uv) && sk.multiplier >= 2) {
      l.image(cx - 28 * s, cy - 28 * s, cx + 28 * s, cy + 28 * s, flash_uv, ui::fade(kSkillYellow, 0.28f));
    }

    shadowed(l, sx0, g.T(102), pts_buf, pts_ts, s);
    shadowed(l, sx0 + pw, g.T(105), mult_buf, mul_ts, s);

    if (!sk.last_skill.empty()) {
      char pval[24];
      std::snprintf(pval, sizeof pval, " %d", sk.last_points);
      TextStyle sn_ts = font(ui::Font::kE, 14.5f * s, ui4::kWhite);
      TextStyle sp_ts = font(ui::Font::kE, 14.5f * s, kSkillYellow);
      const float snw = ui::List::measure(sk.last_skill, sn_ts);
      const float spw = ui::List::measure(pval, sp_ts);
      const float icon_sz = ui_texture("HUD/DRIVINGSKILLS/SKILLICONSET", icon_uv) ? 24.0f * s : 0.0f;
      const float row_w = (icon_sz > 0 ? (icon_sz + 6 * s) : 0.0f) + snw + spw;
      float rx = cx - 0.5f * row_w;
      if (icon_sz > 0) {
        // Hud/DrivingSkills/SkillIconSet.xds is a 512x256 (8x4 grid of 64x64) white-outlined icon sheet.
        const float u_span = icon_uv[2] - icon_uv[0];
        const float v_span = icon_uv[3] - icon_uv[1];
        const float aspect = (v_span > 1e-6f) ? (u_span / v_span) : 2.0f;
        const int cols = (aspect >= 1.5f) ? 8 : 4;
        const int rows = 4;
        std::string sk_up = sk.last_skill;
        for (char& ch : sk_up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        int col = 0, row = 1;  // default: skill badge (0,1)
        if (sk_up.find("AIR") != std::string::npos || sk_up.find("JUMP") != std::string::npos) {
          col = 4;
          row = 2;  // winged flying car
        } else if (sk_up.find("SPEED") != std::string::npos) {
          col = 2;
          row = 2;  // speeding car
        } else if (sk_up.find("NEAR MISS") != std::string::npos || sk_up.find("CLEAN") != std::string::npos ||
                   sk_up.find("PASS") != std::string::npos) {
          col = 4;
          row = 0;  // cars passing close
        } else if (sk_up.find("DRIFT") != std::string::npos || sk_up.find("180") != std::string::npos) {
          col = 2;
          row = 0;  // spinning drift arrow
        } else if (sk_up.find("BURN") != std::string::npos) {
          col = 5;
          row = 0;  // burning wheel
        } else if (sk_up.find("DRAFT") != std::string::npos || sk_up.find("SLIP") != std::string::npos) {
          col = 2;
          row = 1;  // slingshot
        } else if (sk_up.find("WRECK") != std::string::npos || sk_up.find("SMASH") != std::string::npos) {
          col = 3;
          row = 1;  // explosion
        }
        col = std::clamp(col, 0, cols - 1);
        row = std::clamp(row, 0, rows - 1);
        const float du = u_span / float(cols);
        const float dv = v_span / float(rows);
        const float pad_u = du * 0.04f;
        const float pad_v = dv * 0.04f;
        l.image(rx, g.T(124), rx + icon_sz, g.T(124) + icon_sz,
                {icon_uv[0] + col * du + pad_u, icon_uv[1] + row * dv + pad_v,
                 icon_uv[0] + (col + 1) * du - pad_u, icon_uv[1] + (row + 1) * dv - pad_v},
                ui4::kWhite);
        rx += icon_sz + 6 * s;
      }
      shadowed(l, rx, g.T(130), sk.last_skill, sn_ts, s);
      shadowed(l, rx + snw, g.T(130), pval, sp_ts, s);
    }
  } else if (show_rank) {
    const float a = 1.0f - clamp01((since_rank - 3.0) / 1.0);
    Turn t(l, cx, g.T(114));
    char rl[64];
    std::snprintf(rl, sizeof rl, "POPULARITY RANK %d%s!", sk.popularity_rank, place_suffix(sk.popularity_rank));
    const TextStyle ts = font(ui::Font::kE, 22 * s, ui::fade(kText, a), Align::kCenter);
    const float w = ui::List::measure(rl, ts) + 34 * s;
    ui4::brush(l, cx - 0.5f * w, g.T(100), cx + 0.5f * w, g.T(132), ui::fade(ui4::kPink, a));
    l.text(cx, g.T(106), rl, ts);
  } else if (show_bank) {
    const float a = 1.0f - clamp01((since_bank - 2.6) / 0.9);
    const std::string bl = "+" + thousands(sk.banked_total) + " BANKED";
    shadowed(l, cx, g.T(104), bl, font(ui::Font::kE, 24 * s, ui::fade(kSkillYellow, a), Align::kCenter), s);
  } else if (show_lost) {
    const float a = 1.0f - clamp01((since_lost - 1.5) / 0.7);
    shadowed(l, cx, g.T(104), "SKILL CHAIN LOST", font(ui::Font::kE, 22 * s, ui::fade(kRed, a), Align::kCenter), s);
  }
}


void ceremony_overlay(ui::List& l, const CeremonyHud& c, const Layout& g) {
  const float s = g.s;
  l.rect(0, 0, g.w, g.h, rgba(0, 0, 0, 155));
  const uint32_t bc = band_colour(c.band_colour);
  {
    Turn t(l, g.L(340), g.T(185));
    brush(l, g.L(95), g.T(150), g.L(610), g.T(218), kBrush);
    TextStyle hs = font(ui::Font::kE, 44 * s, kText);
    fit(c.headline, hs, g.L(490), 20 * s);
    l.text(g.L(115), g.T(160), c.headline, hs);
  }
  std::array<float, 4> uv;
  if (!c.band_colour.empty() && ui_texture("WRISTBANDS/" + c.band_colour + "XL", uv)) {
    l.image_rotated(g.L(300), g.T(292), 255 * s, 99 * s, -0.06f, uv);
  }
  {
    Turn t(l, g.L(410), g.T(392));
    brush(l, g.L(88), g.T(354), g.L(740), g.T(432), bc);
    TextStyle bs = font(ui::Font::kE, 48 * s, kInk);
    fit(c.band_name, bs, g.L(620), 20 * s);
    l.text(g.L(112), g.T(368), c.band_name, bs);
  }
  if (!c.subtitle.empty()) {
    Turn t(l, g.L(340), g.T(465));
    l.rect(g.L(105), g.T(445), g.L(640), g.T(486), kLight);
    l.text(g.L(122), g.T(456), c.subtitle, font(ui::Font::kB, 17 * s, kDark));
  }
  if (!c.bonus_car.empty()) {
    Turn t(l, g.L(340), g.T(518));
    brush(l, g.L(105), g.T(495), g.L(660), g.T(542), kPink);
    const std::string bc_txt = "BONUS CAR ADDED TO GARAGE: " + c.bonus_car;
    TextStyle cs = font(ui::Font::kB, 16 * s, kText);
    cs.keep_case = true;
    fit(bc_txt, cs, g.L(530), 10 * s);
    l.text(g.L(122), g.T(509), bc_txt, cs);
  }
  ui4::button_bar(l, {{"A", str("HelpButtons", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
}

// (j) Interactive 1:1 FH1 H_MAP HUD overlay:
// Center MAP_CROSSHAIR + #CF2169 MAP_LOCKON, clean hover MAPPILLSMALL pill,
// compact non-intrusive top-left filter bar & roads counter (NEVER a giant
// permanent list blocking the right third of Colorado!), PlayerBar top-right,
// and BUTTON_BAR_SCENE + road legend at bottom.
void map_overlay_hud(ui::List& l, const MapOverlayHud& mo, const Layout& g, bool has_player_bar, bool metric_units) {
  const float s = g.s;
  constexpr uint32_t kMapPink = rgba(0xCF, 0x21, 0x69);
  const bool hovering_marker = !mo.title.empty() && mo.title != "COLORADO MAP";

  // 1. Center H_MAP crosshair reticle: MAP/MASKS/MAP_CROSSHAIR + #CF2169 MAP_LOCKON.
  {
    const float cx = 0.5f * g.w, cy = 0.5f * g.h;
    std::array<float, 4> ch_uv{}, lo_uv{};
    if (ui_texture("MAP/MASKS/MAP_CROSSHAIR", ch_uv)) {
      l.image(cx - 56 * s, cy - 56 * s, cx + 56 * s, cy + 56 * s, ch_uv, rgba(255, 255, 255, 235));
    } else {
      l.arc(cx, cy, 30 * s, 2.0f * s, 0.0f, 6.28318f, rgba(255, 255, 255, 220));
      l.line(cx - 44 * s, cy, cx - 18 * s, cy, 2.0f * s, ui4::kWhite);
      l.line(cx + 18 * s, cy, cx + 44 * s, cy, 2.0f * s, ui4::kWhite);
      l.line(cx, cy - 44 * s, cx, cy - 18 * s, 2.0f * s, ui4::kWhite);
      l.line(cx, cy + 18 * s, cx, cy + 44 * s, 2.0f * s, ui4::kWhite);
    }
    if (hovering_marker) {
      if (ui_texture("MAP/MASKS/MAP_LOCKON", lo_uv)) {
        l.image(cx - 42 * s, cy - 42 * s, cx + 42 * s, cy + 42 * s, lo_uv, kMapPink);
      } else {
        l.arc(cx, cy, 22 * s, 2.5f * s, 0.0f, 6.28318f, kMapPink);
      }
      // Compact hover MAPPILLSMALL tooltip placed cleanly to the left of the reticle.
      TextStyle hts = font(ui::Font::kE, 14 * s, ui4::kWhite);
      fit(mo.title, hts, 260 * s, 10 * s);
      const float hw = std::clamp(ui::List::measure(mo.title, hts) + 32 * s, 128 * s, 292 * s);
      const float hx0 = cx - 48 * s - hw, hy0 = cy - 15 * s;
      ui4::pill(l, hx0, hy0, hx0 + hw, hy0 + 30 * s, ui4::kBrushBlack, ui4::PillKind::kMapSmall);
      l.rect(hx0 + hw - 4 * s, hy0, hx0 + hw, hy0 + 30 * s, kMapPink);
      l.text(hx0 + 12 * s, hy0 + 0.5f * (30 * s - hts.size), mo.title, hts);
    } else if (mo.distance_m >= 0.0) {
      // Subtle compact distance pill beside crosshair when panning over open road.
      const std::string db = format_distance_unit(mo.distance_m, metric_units);
      TextStyle dts = font(ui::Font::kB, 11.5f * s, ui4::kWhite);
      const float dw = ui::List::measure(db, dts) + 22 * s;
      const float dx0 = cx + 36 * s, dy0 = cy - 12 * s;
      ui4::pill(l, dx0, dy0, dx0 + dw, dy0 + 24 * s, ui::rgba(14, 16, 20, 205), ui4::PillKind::kMapSmall);
      l.text(dx0 + 11 * s, dy0 + 6 * s, db, dts);
    }
  }

  // 2. Compact non-intrusive top-left H_MAP filter bar & roads discovery pill
  //    (cycles cleanly via LB / RB without covering the right third of Colorado!).
  {
    const float fx0 = g.L(48);
    const float fy0 = g.T(32);
    const std::string active_name =
        (!mo.filter_tabs.empty() && mo.active_filter >= 0 && mo.active_filter < int(mo.filter_tabs.size()))
            ? mo.filter_tabs[size_t(mo.active_filter)]
            : std::string("ALL");
    const std::string f_txt = "FILTER: " + active_name;
    TextStyle fts = font(ui::Font::kE, 13.0f * s, ui4::kWhite);
    const float f_pill_w = std::max(180 * s, ui::List::measure(f_txt, fts) + 92 * s);
    ui4::pill(l, fx0, fy0, fx0 + f_pill_w, fy0 + 28 * s, ui4::kBrushBlack, ui4::PillKind::kHud01);
    l.rect(fx0, fy0, fx0 + 4 * s, fy0 + 28 * s, kMapPink);
    ui4::button_glyph(l, fx0 + 24 * s, fy0 + 14 * s, 9.5f * s, "LB");
    l.text(fx0 + 46 * s, fy0 + 7.5f * s, f_txt, fts);
    ui4::button_glyph(l, fx0 + f_pill_w - 22 * s, fy0 + 14 * s, 9.5f * s, "RB");

    // Compact Roads Discovered pill right beside filter bar.
    char rb[64];
    std::snprintf(rb, sizeof rb, "ROADS %d / %d", mo.roads_discovered, mo.roads_total);
    TextStyle rts = font(ui::Font::kB, 12.0f * s, ui4::kWhite);
    const float rx0 = fx0 + f_pill_w + 10 * s;
    const float rw = std::max(134 * s, ui::List::measure(rb, rts) + 26 * s);
    ui4::pill(l, rx0, fy0, rx0 + rw, fy0 + 28 * s, ui4::kPillGrey, ui4::PillKind::kHud01);
    l.text(rx0 + 12 * s, fy0 + 6.5f * s, rb, rts);
    const float rfrac = mo.roads_total > 0 ? float(std::clamp(double(mo.roads_discovered) / mo.roads_total, 0.0, 1.0)) : 0.0f;
    l.rect(rx0 + 4 * s, fy0 + 23 * s, rx0 + rw - 4 * s, fy0 + 26.5f * s, rgba(255, 255, 255, 32));
    if (rfrac > 0.0f) l.rect(rx0 + 4 * s, fy0 + 23 * s, rx0 + 4 * s + (rw - 8 * s) * rfrac, fy0 + 26.5f * s, ui4::kGreen);

    if (mo.treasure_map) {
      const float tx0 = rx0 + rw + 10 * s;
      ui4::pill(l, tx0, fy0, tx0 + 150 * s, fy0 + 28 * s, ui4::kAmber, ui4::PillKind::kHud01);
      l.text(tx0 + 75 * s, fy0 + 7.5f * s, "TREASURE MAP", font(ui::Font::kE, 11.5f * s, ui4::kInk, Align::kCenter));
    }
  }

  // 3. Compact contextual right-hand PILL26PX detail card ONLY when hovering an icon/location
  //    (zero permanent wall blocking the map when browsing roads!).
  if (hovering_marker) {
    const float px0 = g.R(908), px1 = g.R(1214);
    float ry = has_player_bar ? g.T(130) : g.T(36);
    std::array<float, 4> pill26_uv{}, fest_uv{};
    const bool has_pill26 = ui_texture("KITPARTS/PILL26PX", pill26_uv);
    auto draw_map_row = [&](float y, float h_px, uint32_t col) {
      if (has_pill26) l.image(px0, y, px1, y + h_px, pill26_uv, col);
      else ui4::pill(l, px0, y, px1, y + h_px, col, ui4::PillKind::kHud01);
    };
    draw_map_row(ry, 32 * s, kMapPink);
    float hdr_tx = px0 + 12 * s;
    if (ui_texture("KITPARTS/FESTIVALMAPICON", fest_uv)) {
      l.image(px0 + 8 * s, ry + 3 * s, px0 + 34 * s, ry + 29 * s, fest_uv, ui4::kWhite);
      hdr_tx = px0 + 40 * s;
    }
    TextStyle hts = font(ui::Font::kE, 15.5f * s, ui4::kWhite);
    fit(mo.title, hts, px1 - hdr_tx - 12 * s, 10 * s);
    l.text(hdr_tx, ry + 0.5f * (32 * s - hts.size), mo.title, hts);
    ry += 34 * s;

    if (!mo.subtitle.empty()) {
      draw_map_row(ry, 26 * s, ui4::kPillGrey);
      TextStyle ss = font(ui::Font::kB, 12.0f * s, ui4::kWhite);
      fit(mo.subtitle, ss, (px1 - px0) - 24 * s, 9 * s);
      l.text(px0 + 12 * s, ry + 6.5f * s, mo.subtitle, ss);
      ry += 28 * s;
    }
    if (mo.distance_m >= 0.0 || mo.gps_active) {
      draw_map_row(ry, 26 * s, ui4::kPillGrey);
      if (mo.distance_m >= 0.0) {
        const std::string db = "DISTANCE  " + format_distance_unit(mo.distance_m, metric_units);
        l.text(px0 + 12 * s, ry + 6.5f * s, db, font(ui::Font::kB, 12.0f * s, ui4::kYellow));
      }
      if (mo.gps_active) {
        l.rect(px1 - 86 * s, ry + 3 * s, px1 - 6 * s, ry + 23 * s, ui4::kGreen);
        l.text(px1 - 46 * s, ry + 7 * s, "GPS SET", font(ui::Font::kE, 10.5f * s, ui4::kInk, Align::kCenter));
      }
    }
  }

  // 4. Authentic BUTTON_BAR_SCENE bottom help strip + driven/undriven legend pill.
  ui4::button_bar(
      l,
      {{"A", mo.action_hint.empty() ? "FAST TRAVEL" : mo.action_hint},
       {"X", mo.gps_hint.empty() ? "SET GPS ROUTE" : mo.gps_hint},
       {"LB", "FILTER"},
       {"LT", "ZOOM"},
       {"B", str("HelpButtons", "IDS_Back", "BACK")}},
      g.w, g.h, 48.0f);

  const float ly0 = g.B(654), ly1 = g.B(682);
  ui4::pill(l, g.R(992), ly0, g.R(1232), ly1, ui4::kPillGrey, ui4::PillKind::kMenu);
  l.rect(g.R(1008), ly0 + 11 * s, g.R(1030), ly0 + 17 * s, rgba(252, 244, 214));
  l.text(g.R(1036), ly0 + 8 * s, "DRIVEN", font(ui::Font::kB, 11.5f * s, ui4::kWhite));
  l.rect(g.R(1110), ly0 + 11 * s, g.R(1132), ly0 + 17 * s, rgba(108, 112, 122));
  l.text(g.R(1138), ly0 + 8 * s, "UNDRIVEN", font(ui::Font::kB, 11.5f * s, ui4::kDim));
}

// (j2) 947_HUD/MiniMap/SatNavCommands: slim HUDPILL01 turn-by-turn strip above minimap.
void gps_turn_hud(ui::List& l, const GpsHud& gps, const Layout& g, bool metric_units) {
  if (!should_show_gps_turn_banner(gps)) return;
  const float s = g.s;
  const float x0 = g.L(64), y0 = g.B(376), x1 = g.L(296), y1 = g.B(408);
  ui4::pill(l, x0, y0, x1, y1, ui4::kPillGrey, ui4::PillKind::kHud01);
  l.rect(x0, y0, x0 + 28 * s, y1, ui4::kGreen);
  const char* glyph = "^";
  switch (gps.turn_kind) {
    case 2: glyph = "\\"; break;
    case 3: glyph = "<"; break;
    case 4: glyph = "<<"; break;
    case 5: glyph = "/"; break;
    case 6: glyph = ">"; break;
    case 7: glyph = ">>"; break;
    case 8: glyph = "U"; break;
    case 9: glyph = "*"; break;
    default: glyph = "^"; break;
  }
  l.text(x0 + 14 * s, y0 + 6 * s, glyph, font(ui::Font::kE, 18 * s, ui4::kInk, Align::kCenter));

  const bool show_dist = should_show_gps_turn_distance(gps);
  const std::string line1 = gps.turn_text.empty() ? std::string("FOLLOW ROUTE") : gps.turn_text;
  TextStyle ts = font(ui::Font::kE, 13.5f * s, ui4::kWhite);
  fit(line1, ts, (x1 - x0) - (show_dist ? 96.0f : 48.0f) * s, 9.5f * s);
  l.text(x0 + 36 * s, y0 + 9 * s, line1, ts);
  if (show_dist) {
    const double rounded_m =
        gps.dist_to_turn_m >= 1000.0 ? gps.dist_to_turn_m : std::max(10.0, std::round(gps.dist_to_turn_m / 10.0) * 10.0);
    const std::string dt_buf = format_distance_unit(rounded_m, metric_units);
    l.text(x1 - 10 * s, y0 + 9 * s, dt_buf, font(ui::Font::kB, 12.5f * s, ui4::kGreen, Align::kRight));
  }
}

// (j3) 947_HUD/Missions (top-right, [956,30..1214,126]): slim HUDPILL01 rows for active
// Outpost PR Stunt (replaces the giant 430x114 top-left dark box!).
void pr_stunt_hud(ui::List& l, const PrStuntHud& ps, const Layout& g) {
  if (!ps.active) return;
  const float s = g.s;
  const float x0 = g.R(948), x1 = g.R(1214);
  float ry = g.T(32);
  const uint32_t accent = ps.in_zone ? ui4::kGreen : ui4::kPink;

  // Row 1: #F90064 / #46DC6E slim HUDPILL01 header + countdown timer.
  ui4::pill(l, x0, ry, x1, ry + 26 * s, accent, ui4::PillKind::kHud01);
  const std::string badge = ps.kind_badge.empty() ? std::string("PR STUNT") : ps.kind_badge;
  l.text(x0 + 12 * s, ry + 6 * s, badge, font(ui::Font::kE, 13.5f * s, ui4::kWhite));
  if (ps.time_remaining_s > 0.0) {
    l.text(x1 - 12 * s, ry + 5 * s, race_clock(ps.time_remaining_s),
           font(ui::Font::kE, 15.0f * s, ps.time_remaining_s < 15.0 ? ui4::kYellow : ui4::kWhite, Align::kRight));
  }
  ry += 28 * s;

  // Row 2: #212121 slim HUDPILL01 stunt title.
  ui4::pill(l, x0, ry, x1, ry + 24 * s, ui4::kPillGrey, ui4::PillKind::kHud01);
  TextStyle ts = font(ui::Font::kB, 13.0f * s, ui4::kWhite);
  fit(ps.title, ts, (x1 - x0) - 22 * s, 9.5f * s);
  l.text(x0 + 12 * s, ry + 5 * s, ps.title, ts);
  ry += 26 * s;

  // Row 3: Status & sharp progress fill strip.
  if (!ps.status_line.empty() || ps.progress > 0.0) {
    ui4::pill(l, x0, ry, x1, ry + 26 * s, ui4::kPillGrey, ui4::PillKind::kHud02);
    if (!ps.status_line.empty()) {
      TextStyle ss = font(ui::Font::kB, 11.5f * s, ps.in_zone ? ui4::kGreen : ui4::kYellow);
      fit(ps.status_line, ss, (x1 - x0) - 22 * s, 8.5f * s);
      l.text(x0 + 12 * s, ry + 4 * s, ps.status_line, ss);
    }
    const float bx0 = x0 + 10 * s, bx1 = x1 - 10 * s, by = ry + 19 * s;
    l.rect(bx0, by, bx1, by + 4 * s, rgba(255, 255, 255, 34));
    const float p = float(std::clamp(ps.progress, 0.0, 1.0));
    if (p > 0.0f) l.rect(bx0, by, bx0 + (bx1 - bx0) * p, by + 4 * s, accent);
  }
}


float speed_activity_hud(ui::List& l, const freeroam::SpeedActivityHud& sh, const Layout& g, bool metric_units,
                         float lift_canvas) {
  const float s = g.s;
  if (sh.flash_alpha > 0.01f) {
    l.rect(0, 0, g.w, g.h, rgba(255, 255, 255, uint8_t(std::clamp(sh.flash_alpha * 255.0f, 0.0f, 220.0f))));
  }
  const float cx = 0.5f * g.w;
  float used = 0.0f;
  if (sh.zone_active) {
    const float x0 = cx - 190 * s, x1 = cx + 190 * s, y0 = g.B(584 - lift_canvas);
    ui4::pill(l, x0, y0, x1, y0 + 24 * s, ui4::kBlue, ui4::PillKind::kHud01);
    l.text(cx, y0 + 5 * s, sh.active_zone_id + "  •  SPEED ZONE", font(ui::Font::kE, 12.5f * s, ui4::kWhite, Align::kCenter));
    ui4::pill(l, x0, y0 + 26 * s, x1, y0 + 56 * s, ui4::kPillGrey, ui4::PillKind::kHud02);
    const std::string avg =
        "AVG " + format_speed_unit(sh.live_avg_mph, metric_units, true) + "   •   " +
        format_distance_unit(sh.zone_distance_m, metric_units);
    l.text(cx, y0 + 33 * s, avg, font(ui::Font::kE, 17 * s, ui4::kWhite, Align::kCenter));
    used = std::max(used, 66.0f);
  }
  if (sh.banner_active) {
    const float extra_lift = sh.zone_active ? 68.0f : 0.0f;
    const float x0 = cx - 220 * s, x1 = cx + 220 * s, y0 = g.B(564 - lift_canvas - extra_lift),
                y1 = g.B(656 - lift_canvas - extra_lift);
    Turn t(l, cx, 0.5f * (y0 + y1), ui4::kTiltRad * 0.5f);
    ui4::brush(l, x0, y0, x1, y0 + 28 * s, ui4::kBrushBlack);
    l.text(cx, y0 + 6 * s, sh.banner_title + (sh.banner_new_pb ? "  •  NEW PERSONAL BEST!" : ""),
           font(ui::Font::kB, 13.5f * s, sh.banner_new_pb ? ui4::kYellow : ui4::kWhite, Align::kCenter));
    ui4::brush(l, x0 + 14 * s, y0 + 28 * s, x1 - 14 * s, y1, sh.banner_is_zone ? ui4::kPink : ui4::kBlue, true);
    const std::string spd =
        format_speed_unit(sh.banner_speed_mph, metric_units, true) + (sh.banner_is_zone ? " AVG" : "");
    const std::string best = "PERSONAL BEST  " + format_speed_unit(sh.banner_best_mph, metric_units, true);
    l.text(cx, y0 + 34 * s, spd, font(ui::Font::kE, 28 * s, ui4::kWhite, Align::kCenter));
    l.text(cx, y0 + 68 * s, best, font(ui::Font::kB, 11 * s, rgba(255, 255, 255, 230), Align::kCenter));
    used = extra_lift + 100.0f;
  }
  return used;
}

// 947_HUD/RaceEncounter: bottom-center Head-to-Head challenge prompt, live bar, and finish banner.
float h2h_hud(ui::List& l, const freeroam::HeadToHeadHud& hh, const Layout& g, bool metric_units, float lift_canvas) {
  const float s = g.s;
  const float cx = 0.5f * g.w;
  float used = 0.0f;
  if (hh.prompt_visible && !hh.active) {
    const float x0 = cx - 230 * s, x1 = cx + 230 * s, y0 = g.B(576 - lift_canvas);
    ui4::brush(l, x0 + 16 * s, y0, x1 - 16 * s, y0 + 26 * s, ui4::kPink);
    l.text(cx, y0 + 5 * s, "HEAD-TO-HEAD CHALLENGE  [X]", font(ui::Font::kE, 14.5f * s, ui4::kWhite, Align::kCenter));
    ui4::pill(l, x0, y0 + 28 * s, x1, y0 + 54 * s, ui4::kPillGrey, ui4::PillKind::kHud01);
    char sub[128];
    std::snprintf(sub, sizeof sub, "VS %s [%s %d]  •  WAGER %s CR", hh.prompt_driver.c_str(),
                  hh.prompt_car_class.c_str(), hh.prompt_pi, thousands(hh.prompt_wager).c_str());
    l.text(cx, y0 + 34 * s, sub, font(ui::Font::kB, 12.5f * s, ui4::kYellow, Align::kCenter));
    used = std::max(used, 64.0f);
  }
  if (hh.active) {
    const float x0 = cx - 230 * s, x1 = cx + 230 * s, y0 = g.B(584 - lift_canvas);
    ui4::pill(l, x0, y0, x1, y0 + 26 * s, ui4::kPink, ui4::PillKind::kHud01);
    char hdr[96], stats[160];
    std::snprintf(hdr, sizeof hdr, "HEAD-TO-HEAD  VS  %s", hh.opponent_name.c_str());
    l.text(cx, y0 + 6 * s, hdr, font(ui::Font::kE, 14.5f * s, ui4::kWhite, Align::kCenter));
    ui4::pill(l, x0, y0 + 28 * s, x1, y0 + 60 * s, ui4::kPillGrey, ui4::PillKind::kHud02);
    const std::string rem_s = format_distance_unit(hh.player_remaining_m, metric_units);
    const std::string gap_s = format_gap_unit(hh.gap_m, metric_units);
    std::snprintf(stats, sizeof stats, "%d%s / 2  •  %s LEFT  •  GAP %s  •  %s CR", hh.place,
                  place_suffix(hh.place), rem_s.c_str(), gap_s.c_str(), thousands(hh.wager_credits).c_str());
    l.text(cx, y0 + 34 * s, stats,
           font(ui::Font::kB, 12.5f * s, hh.place == 1 ? ui4::kGreen : ui4::kYellow, Align::kCenter));
    const float bx0 = x0 + 12 * s, bx1 = x1 - 12 * s, by = y0 + 52 * s;
    l.rect(bx0, by, bx1, by + 4 * s, rgba(255, 255, 255, 35));
    l.rect(bx0, by, bx0 + float(hh.progress) * (bx1 - bx0), by + 4 * s, ui4::kPink);
    used = std::max(used, 68.0f);
  }
  if (hh.finish_banner) {
    const float x0 = cx - 230 * s, x1 = cx + 230 * s, y0 = g.B(564 - lift_canvas), y1 = g.B(638 - lift_canvas);
    Turn t(l, cx, 0.5f * (y0 + y1), ui4::kTiltRad * 0.5f);
    ui4::brush(l, x0, y0, x1, y1, hh.finish_won ? ui4::kPink : ui4::kBrushBlack, true);
    l.text(cx, y0 + 10 * s, hh.finish_won ? "HEAD-TO-HEAD VICTORY!" : "HEAD-TO-HEAD DEFEAT",
           font(ui::Font::kE, 26 * s, kText, Align::kCenter));
    char detail[96];
    if (hh.finish_won)
      std::snprintf(detail, sizeof detail, "BEAT %s   •   +%s CR  +1,000 FAME", hh.finish_opponent.c_str(),
                    thousands(hh.finish_credits).c_str());
    else std::snprintf(detail, sizeof detail, "%s REACHED THE FINISH FIRST", hh.finish_opponent.c_str());
    l.text(cx, y0 + 44 * s, detail, font(ui::Font::kB, 13 * s, ui4::kYellow, Align::kCenter));
    used = std::max(used, 84.0f);
  }
  return used;
}

// 947_HUD/CombinedNotification: bottom-center Sponsor Notification (lifted cleanly above active subtitles/banners).
bool progression_hud(ui::List& l, const DriveHud& st, const Layout& g, float lift_canvas) {
  const float s = g.s;
  const double age = st.time - st.sponsor_at;
  if (st.sponsor_title.empty() || age < 0.0 || age >= 5.5) return false;

  const float fade_a = ease_in_time(age, 0.0, 0.25) * (1.0f - clamp01((age - 4.9) / 0.6));
  const float cx = 0.5f * g.w;
  const float x0 = cx - 196 * s, x1 = cx + 196 * s, y0 = g.B(570 - lift_canvas), y1 = g.B(656 - lift_canvas);

  // Pick matching SPONSORLOGOTILE/*.xds by brand substring, else HorizonLogo/RedBull.
  std::string up = st.sponsor_title;
  for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
  std::string logo_key = "SPONSORLOGOTILE/HORIZONLOGO";
  static constexpr std::pair<const char*, const char*> kBrands[] = {
      {"REDBULL", "SPONSORLOGOTILE/REDBULL"},   {"RED BULL", "SPONSORLOGOTILE/REDBULL"},
      {"OAKLEY", "SPONSORLOGOTILE/OAKLEY"},     {"GOPRO", "SPONSORLOGOTILE/GOPRO"},
      {"HKS", "SPONSORLOGOTILE/HKS"},           {"NIKE", "SPONSORLOGOTILE/NIKE"},
      {"ADIDAS", "SPONSORLOGOTILE/ADIDAS"},     {"BOSE", "SPONSORLOGOTILE/BOSE"},
      {"PUMA", "SPONSORLOGOTILE/PUMA"},         {"ROCKSTAR", "SPONSORLOGOTILE/ROCKSTAR"},
      {"PIONEER", "SPONSORLOGOTILE/PIONEER"},   {"GSHOCK", "SPONSORLOGOTILE/GSHOCK"},
      {"DIESEL", "SPONSORLOGOTILE/DIESEL"},     {"NIXON", "SPONSORLOGOTILE/NIXON"},
      {"ZOO", "SPONSORLOGOTILE/ZOOYORK"},
  };
  for (const auto& [needle, path] : kBrands) {
    if (up.find(needle) != std::string::npos) {
      logo_key = path;
      break;
    }
  }

  // Top #DAFF00 tilted brush banner dynamically sized to wrap header text cleanly.
  {
    const char* hdr_txt = "SPONSOR CHALLENGE UNLOCKED!";
    TextStyle hts = font(ui::Font::kE, 12.5f * s, ui::fade(ui4::kInk, fade_a), Align::kCenter);
    const float hbw = std::max(240 * s, ui::List::measure(hdr_txt, hts) + 46 * s);
    Turn bt(l, cx, y0 + 12 * s, ui4::kTiltRad * 0.4f);
    ui4::brush(l, cx - 0.5f * hbw, y0, cx + 0.5f * hbw, y0 + 24 * s, ui::fade(ui4::kYellow, fade_a));
    l.text(cx, y0 + 5 * s, hdr_txt, hts);
  }

  // Dark HUDPANEL02 backing strip + white square SPONSORLOGOTILE badge on the left.
  std::array<float, 4> panel_uv{}, logo_uv{};
  if (ui_texture("DIRTMASKS/HUD/HUDPANEL02", panel_uv)) {
    l.image_masked(x0, y0 + 24 * s, x1, y1, panel_uv, panel_uv, ui::fade(ui4::kPillGrey, fade_a),
                   {1.0f, 1.0f, 0.0f, 0.0f}, ui::kMaskBorderTransparent);
  } else {
    ui4::panel(l, x0, y0 + 24 * s, x1, y1, s, ui::fade(ui4::kCharcoalDark, fade_a), 0, ui::fade(ui4::kYellow, fade_a),
               true);
  }
  const float tile_sz = 50 * s;
  const float lx0 = x0 + 12 * s, ly0 = y0 + 30 * s;
  l.rect(lx0, ly0, lx0 + tile_sz, ly0 + tile_sz, ui::fade(ui4::kWhite, fade_a));
  if (ui_texture(logo_key, logo_uv) || ui_texture("SPONSORLOGOTILE/OAKLEY", logo_uv) ||
      ui_texture("SPONSORLOGOTILE/BOSE", logo_uv)) {
    l.image(lx0 + 2 * s, ly0 + 2 * s, lx0 + tile_sz - 2 * s, ly0 + tile_sz - 2 * s, logo_uv,
            ui::fade(ui4::kWhite, fade_a));
  }
  const float tx = lx0 + tile_sz + 14 * s;
  TextStyle stt = font(ui::Font::kE, 16 * s, ui::fade(ui4::kWhite, fade_a));
  fit(st.sponsor_title, stt, x1 - tx - 14 * s, 10 * s);
  TextStyle stdt = font(ui::Font::kB, 12.5f * s, ui::fade(ui4::kYellow, fade_a));
  fit(st.sponsor_detail, stdt, x1 - tx - 14 * s, 9 * s);
  l.text(tx, y0 + 34 * s, st.sponsor_title, stt);
  l.text(tx, y0 + 56 * s, st.sponsor_detail, stdt);
  return true;
}


}  // namespace carview::hud_detail
