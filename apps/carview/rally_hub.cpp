#include "rally_hub.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "game_hud.h"
#include "hud_scene.h"
#include "scene_screen.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kWhite = ui::rgba(248, 249, 252, 255);
constexpr uint32_t kMuted = ui::rgba(174, 180, 192, 255);
constexpr uint32_t kRallyGold = ui::rgba(255, 184, 28, 255);
constexpr uint32_t kRallyOrange = ui::rgba(242, 108, 30, 255);
constexpr uint32_t kRallyRed = ui::rgba(228, 48, 52, 255);
constexpr uint32_t kRallyGreen = ui::rgba(46, 204, 113, 255);
constexpr uint32_t kRallyYellow = ui::rgba(0xDA, 0xFF, 0x00, 255);
constexpr uint32_t kRallyCyan = ui::rgba(62, 198, 242, 255);

uint32_t icon_accent_color(race::PaceIconKind kind) {
  switch (kind) {
    case race::PaceIconKind::kEasyLeft:
    case race::PaceIconKind::kEasyRight: return kRallyGreen;
    case race::PaceIconKind::kMediumLeft:
    case race::PaceIconKind::kMediumRight: return kRallyYellow;
    case race::PaceIconKind::kHardLeft:
    case race::PaceIconKind::kHardRight: return kRallyOrange;
    case race::PaceIconKind::kSquareLeft:
    case race::PaceIconKind::kSquareRight:
    case race::PaceIconKind::kHairpinLeft:
    case race::PaceIconKind::kHairpinRight: return kRallyRed;
    case race::PaceIconKind::kCrest:
    case race::PaceIconKind::kJump: return kRallyGold;
    case race::PaceIconKind::kCaution: return kRallyRed;
    default: return kWhite;
  }
}

// Formats a car name cleanly through car_name(), falling back to readable title case
// if a raw internal car media ID (e.g. "SUB_IMPREZA22B_98") is passed without DB bound.
std::string resolve_rally_car_name(const std::string& raw) {
  if (raw.empty()) return "Rally Spec Car";
  const std::string mapped = car_name(raw);
  if (!mapped.empty() && mapped != raw) return mapped;
  // Detect raw MEDIA_ID_YY pattern (underscores + uppercase).
  if (raw.find('_') == std::string::npos) return raw;
  static const std::pair<const char*, const char*> kKnown[] = {
      {"SUB_IMPREZA22B_98", "1998 Subaru Impreza 22B STi"},
      {"SUB_IMPREZAWRX_05", "2005 Subaru Impreza WRX STi"},
      {"SUB_WRXSTI_11", "2011 Subaru WRX STI"},
      {"MIT_LANCEREVO6_99", "1999 Mitsubishi Lancer Evo VI GSR"},
      {"MIT_LANCEREVO10_08", "2008 Mitsubishi Lancer Evo X GSR"},
      {"FOR_ESCORTCOSWORTH_92", "1992 Ford Escort RS Cosworth"},
      {"FOR_SVTRAPTOR_11", "2011 Ford F-150 SVT Raptor"},
      {"LAN_037STRADALE_82", "1982 Lancia 037 Stradale"},
      {"LAN_DELTAS4_86", "1986 Lancia Delta S4"},
      {"AUD_SPORTQUATTRO_83", "1983 Audi Sport Quattro"},
      {"PEU_205TURBO16_84", "1984 Peugeot 205 Turbo 16"},
      {"TOY_CELICAGT4_94", "1994 Toyota Celica GT-Four ST205"},
  };
  for (const auto& [k, v] : kKnown) {
    if (raw == k) return v;
  }
  std::string out;
  for (char c : raw) out.push_back(c == '_' ? ' ' : c);
  return out;
}

// CoDriverIconSet.xds is a 512x64 (8x1 horizontal strip of 64x64) sprite atlas of
// turn & hazard pace-note glyphs. Left turns mirror the right-turn cell horizontally.
bool codriver_atlas_cell(race::PaceIconKind kind, int& col, bool& mirror_x) {
  mirror_x = false;
  switch (kind) {
    case race::PaceIconKind::kEasyLeft: mirror_x = true; [[fallthrough]];
    case race::PaceIconKind::kEasyRight: col = 0; return true;
    case race::PaceIconKind::kMediumLeft: mirror_x = true; [[fallthrough]];
    case race::PaceIconKind::kMediumRight: col = 1; return true;
    case race::PaceIconKind::kHardLeft: mirror_x = true; [[fallthrough]];
    case race::PaceIconKind::kHardRight: col = 2; return true;
    case race::PaceIconKind::kSquareLeft: mirror_x = true; [[fallthrough]];
    case race::PaceIconKind::kSquareRight: col = 3; return true;
    case race::PaceIconKind::kHairpinLeft: mirror_x = true; [[fallthrough]];
    case race::PaceIconKind::kHairpinRight: col = 4; return true;
    default: return false;
  }
}

void draw_codriver_icon(ui::List& out, race::PaceIconKind kind, float x0, float y0, float x1, float y1, uint32_t col) {
  std::array<float, 4> sheet{};
  int cx = 0;
  bool mirror_x = false;
  if (codriver_atlas_cell(kind, cx, mirror_x) &&
      (ui_texture("HUD/RALLY/CODRIVERICONSET", sheet) || ui_texture("Hud/Rally/CoDriverIconSet", sheet))) {
    const float du = (sheet[2] - sheet[0]) * 0.125f;  // 8 cells across 512x64 strip
    float u0 = sheet[0] + float(cx) * du;
    float u1 = u0 + du;
    const float v0 = sheet[1];
    const float v1 = sheet[3];
    if (mirror_x) std::swap(u0, u1);
    out.image(x0, y0, x1, y1, {u0, v0, u1, v1}, col);
    return;
  }
  // Crisp sharp-edged fallback glyph when Rally DLC CoDriverIconSet.xds is absent (never stick-figure boxes).
  const char* sym = "!";
  switch (kind) {
    case race::PaceIconKind::kEasyLeft: sym = "6L"; break;
    case race::PaceIconKind::kEasyRight: sym = "6R"; break;
    case race::PaceIconKind::kMediumLeft: sym = "4L"; break;
    case race::PaceIconKind::kMediumRight: sym = "4R"; break;
    case race::PaceIconKind::kHardLeft: sym = "2L"; break;
    case race::PaceIconKind::kHardRight: sym = "2R"; break;
    case race::PaceIconKind::kSquareLeft: sym = "90L"; break;
    case race::PaceIconKind::kSquareRight: sym = "90R"; break;
    case race::PaceIconKind::kHairpinLeft: sym = "HP L"; break;
    case race::PaceIconKind::kHairpinRight: sym = "HP R"; break;
    case race::PaceIconKind::kCrest: sym = "^"; break;
    case race::PaceIconKind::kJump: sym = "^^"; break;
    case race::PaceIconKind::kCaution: sym = "!"; break;
    default: break;
  }
  const float h = y1 - y0;
  ui::TextStyle ts = ui4::style(ui::Font::kE, h * 0.56f, col, ui::Align::kCenter);
  ui4::fit_text(sym, ts, x1 - x0 - 4.0f, h * 0.28f);
  out.text(0.5f * (x0 + x1), 0.5f * (y0 + y1) - 0.5f * ts.size, sym, ts);
}

}  // namespace

std::string rally_time_str(double seconds) {
  if (seconds < 0.0) return "--:--.---";
  const int total_ms = int(std::llround(seconds * 1000.0));
  const int mins = total_ms / 60000;
  const int secs = (total_ms / 1000) % 60;
  const int ms = total_ms % 1000;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d.%03d", mins, secs, ms);
  return buf;
}

std::string rally_gap_str(double seconds) {
  if (std::fabs(seconds) < 0.0005) return "LEADER";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%+.2fs", seconds);
  return buf;
}

void build_rally_pace_notes_hud(ui::List& out, const RallyStageHud& st, float w, float h) {
  if (!st.active) return;
  const float u = h / 720.0f;

  // 1. Top-center Co-Driver Pace-Note Strip: real CoDriverIconSet.xds on slim sharp FH1
  //    tilted HUDPILL01 / CRISPTAPE01 strips (ZERO 10px rounded orange boxes or stick figures!).
  if (st.pace.active && (!st.pace.icons.empty() || !st.pace.modifiers.empty())) {
    const size_t n_icons = std::min<size_t>(st.pace.icons.size(), 4);
    const float card_w = 78.0f * u;
    const float card_h = 78.0f * u;
    const float gap = 8.0f * u;
    const float strip_w = n_icons > 0 ? (float(n_icons) * card_w + float(n_icons - 1) * gap) : 210.0f * u;
    const float x0 = 0.5f * (w - strip_w);
    const float y0 = 26.0f * u;

    ui4::TiltScope tilt(out, 0.5f * w, y0 + 0.5f * card_h, ui4::kTiltRad * 0.5f);
    for (size_t i = 0; i < n_icons; ++i) {
      const auto& ic = st.pace.icons[i];
      const float ix = x0 + float(i) * (card_w + gap);
      const uint32_t accent = icon_accent_color(ic.kind);
      const uint32_t bg = ic.active ? ui4::kBrushBlack : ui4::kPillGrey;
      out.rect(ix, y0, ix + card_w, y0 + card_h, bg);
      out.rect(ix, y0, ix + card_w, y0 + 4.0f * u, accent);
      if (ic.active) {
        out.rect(ix, y0 + card_h - 3.0f * u, ix + card_w, y0 + card_h, ui4::kPink);
      }
      draw_codriver_icon(out, ic.kind, ix + 11.0f * u, y0 + 7.0f * u, ix + card_w - 11.0f * u, y0 + card_h - 22.0f * u,
                         accent);

      ui::TextStyle ts = ui4::style(ui::Font::kE, 14.0f * u, kWhite, ui::Align::kCenter);
      out.text(ix + card_w * 0.5f, y0 + card_h - 19.0f * u, ic.label, ts);
    }

    // Modifier pills ("50M", "DON'T CUT", "CREST", "CAUTION") on slim sharp HUDPILL01.
    if (!st.pace.modifiers.empty()) {
      const float my = y0 + card_h + 6.0f * u;
      std::string mod_line;
      for (const auto& m : st.pace.modifiers) {
        if (!mod_line.empty()) mod_line += "   •   ";
        mod_line += m;
      }
      ui::TextStyle ms = ui4::style(ui::Font::kE, 13.0f * u, ui4::kYellow, ui::Align::kCenter);
      const float mw = std::max(140.0f * u, ui::List::measure(mod_line, ms) + 34.0f * u);
      ui4::pill(out, 0.5f * w - 0.5f * mw, my, 0.5f * w + 0.5f * mw, my + 24.0f * u, ui4::kBrushBlack,
                ui4::PillKind::kHud01);
      out.text(w * 0.5f, my + 5.0f * u, mod_line, ms);
    }
  }

  // 2. Top-right Rally Stage Split Timer & Stage Progress (slim sharp HUDPILL01 rows, ZERO rounded rects!).
  const float rw = 292.0f * u;
  const float rx1 = w - 64.0f * u;
  const float rx0 = rx1 - rw;
  const float ry0 = 32.0f * u;

  // Stage header brush strip
  {
    ui4::TiltScope stilt(out, 0.5f * (rx0 + rx1), ry0 + 13.0f * u, ui4::kTiltRad * 0.5f);
    ui4::brush(out, rx0, ry0, rx1, ry0 + 26.0f * u, ui4::kBrushBlack, false);
    char hdr[96];
    std::snprintf(hdr, sizeof hdr, "STAGE %d/%d  %s", st.stage_number, st.total_stages,
                  st.championship_name.empty() ? "HORIZON RALLY" : st.championship_name.c_str());
    ui::TextStyle hs = ui4::style(ui::Font::kE, 12.0f * u, kRallyGold);
    ui4::fit_text(hdr, hs, rw - 20.0f * u, 8.5f * u);
    out.text(rx0 + 12.0f * u, ry0 + 7.0f * u, hdr, hs);
  }

  // Live clock + percent pill
  const float cy0 = ry0 + 30.0f * u;
  ui4::pill(out, rx0, cy0, rx1, cy0 + 32.0f * u, ui4::kPillGrey, ui4::PillKind::kHud01);
  out.text(rx0 + 12.0f * u, cy0 + 5.0f * u, rally_time_str(std::max(0.0, st.stage_clock)),
           ui4::style(ui::Font::kE, 21.0f * u, kWhite));
  char pct[24];
  std::snprintf(pct, sizeof pct, "%d%%", int(std::lround(std::clamp(st.progress, 0.0, 1.0) * 100.0)));
  out.text(rx1 - 12.0f * u, cy0 + 9.0f * u, pct, ui4::style(ui::Font::kB, 14.0f * u, ui4::kYellow, ui::Align::kRight));

  // Sharp flat 4-split sector bar (ZERO corner radius).
  const int sectors = std::max(1, st.total_splits + 1);
  const float bar_y = cy0 + 36.0f * u;
  const float seg_gap = 3.0f * u;
  const float seg_w = (rw - seg_gap * float(sectors - 1)) / float(sectors);
  for (int i = 0; i < sectors; ++i) {
    const float sx0 = rx0 + float(i) * (seg_w + seg_gap);
    const float sec_lo = float(i) / float(sectors);
    const float sec_hi = float(i + 1) / float(sectors);
    const float fill = std::clamp((float(st.progress) - sec_lo) / std::max(1e-4f, sec_hi - sec_lo), 0.0f, 1.0f);
    out.rect(sx0, bar_y, sx0 + seg_w, bar_y + 7.0f * u, ui::rgba(33, 33, 33, 210));
    if (fill > 0.001f) {
      const uint32_t col = (i < st.splits_passed) ? kRallyGreen : ui4::kPink;
      out.rect(sx0, bar_y, sx0 + seg_w * fill, bar_y + 7.0f * u, col);
    }
  }

  // Live split delta callout strip (sharp HUDPILL02, ZERO rounded frame).
  if (st.last_split_age < 4.5 && st.splits_passed > 0) {
    const float dy = bar_y + 12.0f * u;
    const bool faster = st.last_split_delta <= 0.0;
    const uint32_t dcol = faster ? kRallyGreen : ui4::kPink;
    ui4::pill(out, rx0, dy, rx1, dy + 26.0f * u, dcol, ui4::PillKind::kHud02);
    char dbuf[64];
    std::snprintf(dbuf, sizeof dbuf, "SPLIT %d   %+.2fs", st.splits_passed, st.last_split_delta);
    out.text(rx0 + rw * 0.5f, dy + 6.0f * u, dbuf,
             ui4::style(ui::Font::kE, 14.0f * u, faster ? ui4::kInk : kWhite, ui::Align::kCenter));
  }
}

void build_rally_hub_screen(ui::List& out, const RallyHubState& st, float w, float h) {
  if (!st.active) return;
  const float u = h / 720.0f;
  const SceneFrame f{0.5f * w - 640.0f * u, 0.5f * h - 360.0f * u, u};
  ui4::subtle_vignette(out, w, h, 0.28f);

  // Drive from H_STREET_RACE_HUB (+3.0 deg horizontal tilted Event Ticket cards,
  // clean open Colorado behind, ZERO rounded kPanelAlt boxes!).
  const fh1::UiScene* hub_sc = ui_scene("H_STREET_RACE_HUB");
  (void)hub_sc;

  // Top-right Career Strip (Wristband + Car + Credits).
  ui4::CareerStripData csd;
  csd.band_name = st.wristband_name.empty() ? "YELLOW WRISTBAND" : st.wristband_name;
  csd.band_next = st.rally_level >= 1 ? "" : "RALLY CHAMPION";
  csd.band_level = std::max(1, st.rally_level);
  csd.band_points = st.rally_xp;
  csd.band_hi = st.next_level_xp;
  csd.car_name = resolve_rally_car_name(st.car_name);
  csd.car_class = st.car_class;
  csd.car_pi = st.car_pi;
  csd.credits = st.credits;
  ui4::career_strip(out, csd, w, h);

  // Tilted +3.0 deg H_STREET_RACE_HUB title + horizontal ticket carousel.
  {
    ui4::TiltScope tilt(out, f.x(640.0f), f.y(380.0f), ui4::kTiltRad);

    // TitleTapeText ("HORIZON RALLY HUB")
    ui4::brush(out, f.x(84.0f), f.y(104.0f), f.x(560.0f), f.y(162.0f), ui4::kPink, true);
    out.text(f.x(106.0f), f.y(115.0f), "HORIZON RALLY", ui4::style(ui::Font::kE, 38.0f * u, kWhite));

    // Horizontal tilted Event Ticket cards (up to 4 visible, centered around selected).
    const int n = int(st.items.size());
    const int vis = std::min(4, n);
    const int start = std::clamp(st.selected - 1, 0, std::max(0, n - vis));
    const float card_w = 264.0f * u;
    const float card_h = 286.0f * u;
    const float gap = 20.0f * u;
    const float row_x0 = f.x(88.0f);
    const float row_y0 = f.y(194.0f);

    std::array<float, 4> dirt_top{}, shadow_uv{}, largetape{};
    const bool has_dirt_top = ui_texture("KITPARTS/TICKETDIRTTOPSR", dirt_top);
    const bool has_shadow = ui_texture("KITPARTS/DROPSHADOW", shadow_uv);
    const bool has_ltape = ui_texture("KITPARTS/LARGETAPE", largetape);

    for (int k = 0; k < vis; ++k) {
      const int idx = start + k;
      const auto& it = st.items[size_t(idx)];
      const bool sel = (idx == st.selected);
      const float cx0 = row_x0 + float(k) * (card_w + gap);
      const float cy0 = row_y0;
      const float cx1 = cx0 + card_w;
      const float cy1 = cy0 + card_h;
      out.add_hit(cx0, cy0, cx1, cy1, ui::kHitRow, idx);

      if (has_shadow) {
        out.image(cx0 - 8.0f * u, cy0 + 8.0f * u, cx1 + 10.0f * u, cy1 + 14.0f * u, shadow_uv,
                  ui::rgba(0, 0, 0, sel ? 185 : 120));
      } else {
        out.rect(cx0 + 5.0f * u, cy0 + 7.0f * u, cx1 + 5.0f * u, cy1 + 7.0f * u, ui::rgba(0, 0, 0, 125));
      }

      // Ticket body (#0E0E0E sharp distressed card).
      out.rect(cx0, cy0, cx1, cy1, ui4::kCharcoalDark);

      // Top artwork zone: real RallyTickets/00N or EventTickets/FestivalRaces artwork!
      const float art_h = 128.0f * u;
      std::array<float, 4> art_uv{};
      char rtk[64];
      std::snprintf(rtk, sizeof rtk, "EVENTTICKETS/RALLYTICKETS/%03d", (idx % 8) + 1);
      const bool has_art = ui_texture(rtk, art_uv) ||
                           ui_texture("EVENTTICKETS/STREETRACES/SR_0" + std::to_string((idx % 9) + 1), art_uv) ||
                           ui_texture("LOADING/BGLOADINGIMAGES/IMAGE" + std::to_string((idx % 10) + 1), art_uv);
      if (has_art) {
        out.image(cx0, cy0, cx1, cy0 + art_h, art_uv, it.unlocked ? kWhite : ui::rgba(110, 114, 122, 220));
      } else {
        out.rect(cx0, cy0, cx1, cy0 + art_h, sel ? ui::rgba(58, 22, 38, 255) : ui::rgba(32, 34, 40, 255));
      }
      if (has_dirt_top) {
        out.image(cx0, cy0, cx1, cy0 + art_h, dirt_top, ui::rgba(255, 255, 255, 90));
      }

      // LARGETAPE header banner across ticket midpoint (#F90064 selected / #212121 unselected).
      const float ty0 = cy0 + art_h - 14.0f * u;
      const float ty1 = ty0 + 38.0f * u;
      const uint32_t tape_col = sel ? ui4::kPink : ui4::kPillGrey;
      if (has_ltape) {
        out.image(cx0 - 4.0f * u, ty0, cx1 + 4.0f * u, ty1, largetape, tape_col);
      } else {
        ui4::brush(out, cx0 - 4.0f * u, ty0, cx1 + 4.0f * u, ty1, tape_col, false);
      }
      ui::TextStyle tt = ui4::style(ui::Font::kE, 16.0f * u, kWhite);
      ui4::fit_text(it.title, tt, card_w - 20.0f * u, 10.0f * u);
      out.text(cx0 + 10.0f * u, ty0 + 0.5f * (ty1 - ty0 - tt.size), it.title, tt);

      // Class badge + Prize + Stages + Status strip (all sharp, zero rounded boxes).
      if (!it.class_badge.empty()) {
        out.rect(cx0 + 10.0f * u, ty1 + 10.0f * u, cx0 + 66.0f * u, ty1 + 32.0f * u,
                 it.unlocked ? kRallyOrange : ui::rgba(80, 84, 92, 230));
        out.text(cx0 + 38.0f * u, ty1 + 15.0f * u, it.class_badge,
                 ui4::style(ui::Font::kE, 12.5f * u, kWhite, ui::Align::kCenter));
      }
      if (!it.status_badge.empty()) {
        const uint32_t scol = !it.unlocked ? kRallyRed : (!it.eligible ? kRallyOrange : ui4::kYellow);
        out.text(cx1 - 10.0f * u, ty1 + 15.0f * u, it.status_badge,
                 ui4::style(ui::Font::kE, 12.0f * u, scol, ui::Align::kRight));
      }
      if (it.cash_prize > 0) {
        out.text(cx0 + 10.0f * u, ty1 + 44.0f * u, thousands(it.cash_prize) + " Cr",
                 ui4::style(ui::Font::kE, 19.0f * u, ui4::kYellow, ui::Align::kLeft, true));
      }
      if (it.stages > 0) {
        char sbuf[48];
        std::snprintf(sbuf, sizeof sbuf, "%d STAGE%s", it.stages, it.stages == 1 ? "" : "S");
        out.text(cx1 - 10.0f * u, ty1 + 48.0f * u, sbuf,
                 ui4::style(ui::Font::kB, 12.0f * u, kMuted, ui::Align::kRight));
      }
      if (!it.stage_names.empty()) {
        ui::TextStyle ss = ui4::style(ui::Font::kB, 11.5f * u, kWhite);
        ui4::fit_text(it.stage_names.front(), ss, card_w - 20.0f * u, 8.5f * u);
        out.text(cx0 + 10.0f * u, ty1 + 76.0f * u, it.stage_names.front(), ss);
      }
      if (it.kind == RallyHubItem::kRivals) {
        const std::string pb_txt =
            it.pb_time_s > 0.0 ? ("PB: " + race::format_rivals_time(it.pb_time_s)) : "PB: --:--.---";
        const std::string tgt_txt = it.rival_name + "  " + race::format_rivals_time(it.target_time_s);
        ui::TextStyle pbs = ui4::style(ui::Font::kB, 10.5f * u, kRallyCyan);
        ui4::fit_text(pb_txt, pbs, card_w - 20.0f * u, 8.0f * u);
        out.text(cx0 + 10.0f * u, ty1 + 94.0f * u, pb_txt, pbs);
        ui::TextStyle tgs = ui4::style(ui::Font::kB, 10.5f * u, kRallyGold);
        ui4::fit_text(tgt_txt, tgs, card_w - 20.0f * u, 8.0f * u);
        out.text(cx0 + 10.0f * u, cy1 - 24.0f * u, tgt_txt, tgs);
      } else if (!it.lock_reason.empty()) {
        ui::TextStyle ws = ui4::style(ui::Font::kB, 10.5f * u, kRallyRed);
        ui4::fit_text(it.lock_reason, ws, card_w - 20.0f * u, 8.0f * u);
        out.text(cx0 + 10.0f * u, cy1 - 24.0f * u, it.lock_reason, ws);
      }
      if (sel) {
        out.rect(cx0, cy0, cx1, cy0 + 4.0f * u, ui4::kPink);
        out.rect(cx0, cy1 - 4.0f * u, cx1, cy1, ui4::kPink);
      }
    }

    // Compact bottom-tilted Rally Readiness PILL02 strip (no rounded boxes!).
    const float ry0 = f.y(510.0f);
    ui4::pill(out, f.x(88.0f), ry0, f.x(1188.0f), ry0 + 36.0f * u, ui4::kPillGrey, ui4::PillKind::kHud01);
    const std::string ready_txt =
        st.kit_status.is_rally_upgraded
            ? ("RALLY SPEC READY  •  " + resolve_rally_car_name(st.car_name) + "  •  TIRES / SUSPENSION / TRANS")
            : ("RALLY KIT REQUIRED  •  FIT RALLY KIT (TIRES/SUSP/GEARBOX) (" + thousands(st.rally_kit_cost) + " Cr)");
    out.text(f.x(104.0f), ry0 + 11.0f * u, ready_txt,
             ui4::style(ui::Font::kB, 13.5f * u, st.kit_status.is_rally_upgraded ? kRallyGreen : kRallyOrange));
  }

  ui4::button_bar(out, {{"A", "SELECT"}, {"DPAD_LR", "BROWSE"}, {"B", "BACK"}}, w, h);
}

// rally.str's STAGE RESULTS and the common CONTINUE.
constexpr StringRef kStageResultsTitle{"rally", "IDS_ResultsStageTitle", 0, "STAGE RESULTS"};
constexpr StringRef kContinue{"", "", 0, "CONTINUE", true};
// rally.str's results titles and headings (h_post_race_rally_finished).
constexpr StringRef kRallyResultsTitle{"rally", "IDS_ResultsRallyTitle", 0, "RALLY RESULTS"};
constexpr StringRef kRallyStandingsTitle{"rally", "IDS_ResultsRallyStandingsTitle", 0, "RALLY STANDINGS"};
constexpr StringRef kTotalTimeHeading{"rally", "IDS_ResultsHeading_TotalTime", 0, "TOTAL TIME"};

std::vector<StringRef> rally_results_text_refs() {
  return {kStageResultsTitle, kRallyResultsTitle, kRallyStandingsTitle, kTotalTimeHeading};
}

void build_rally_service_area_screen(ui::List& out, const RallyServiceAreaState& st, float w, float h) {
  if (!st.active) return;
  const float u = h / 720.0f;
  const SceneFrame f{0.5f * w - 640.0f * u, 0.5f * h - 360.0f * u, u};
  ui4::subtle_vignette(out, w, h, 0.34f);

  // Between stages the race flow shows the stage's result and goes on to
  // the next (game_rally_race_flow: no service menu; every car is repaired
  // before each stage). interim (SPEC_QUESTIONS Q147): the layout is the
  // engine's; the game's single-player flow shows its finish-position screen.
  {
    char eyebrow[128];
    std::snprintf(eyebrow, sizeof eyebrow, "%s  •  STAGE %d OF %d COMPLETE", st.championship_name.c_str(),
                  st.completed_stage, st.total_stages);
    ui4::title_banner(out, u, f.x(76.0f), f.y(42.0f), text_of(kStageResultsTitle), eyebrow, true, 540.0f);

    // Top-right next-stage off-white distressed strip.
    ui4::TiltScope nt(out, f.x(1020.0f), f.y(66.0f), ui4::kTiltRad * 0.6f);
    ui4::offwhite_strip(out, f.x(820.0f), f.y(48.0f), f.x(1206.0f), f.y(82.0f), u, true);
    ui::TextStyle ns = ui4::style(ui::Font::kE, 14.0f * u, ui4::kInk, ui::Align::kRight);
    ui4::fit_text("NEXT: " + st.next_stage_name, ns, 366.0f * u, 9.5f * u);
    out.text(f.x(1194.0f), f.y(58.0f), "NEXT: " + st.next_stage_name, ns);
  }

  ui4::TiltScope screen_tilt(out, f.x(640.0f), f.y(380.0f), ui4::kTiltRad);

  // Left side: Stage summary pills + Damage strip + 4 tilted brush actions.
  const float lx0 = f.x(76.0f), lw = 452.0f * u, ly0 = f.y(152.0f);
  ui4::panel(out, lx0, ly0, lx0 + lw, ly0 + 124.0f * u, u, ui4::kCharcoalDark, kRallyGold, 0, true);
  {
    ui::TextStyle lb = ui4::style(ui::Font::kB, 11.5f * u, ui4::kDim);
    ui::TextStyle vl = ui4::style(ui::Font::kE, 19.0f * u, ui4::kWhite);

    out.text(lx0 + 16.0f * u, ly0 + 12.0f * u, "STAGE TIME", lb);
    out.text(lx0 + 16.0f * u, ly0 + 29.0f * u, rally_time_str(st.last_stage_time), vl);

    out.text(lx0 + lw * 0.54f, ly0 + 12.0f * u, "STAGE PLACE", lb);
    char sp[32];
    std::snprintf(sp, sizeof sp, "%d%s / 50", st.stage_place, place_suffix(st.stage_place));
    vl.color = kRallyGold;
    out.text(lx0 + lw * 0.54f, ly0 + 29.0f * u, sp, vl);

    out.text(lx0 + 16.0f * u, ly0 + 66.0f * u, text_of(kTotalTimeHeading), lb);
    vl.color = ui4::kWhite;
    out.text(lx0 + 16.0f * u, ly0 + 83.0f * u, rally_time_str(st.total_time), vl);

    out.text(lx0 + lw * 0.54f, ly0 + 66.0f * u, "OVERALL", lb);
    char op[48];
    std::snprintf(op, sizeof op, "%d%s (%s)", st.overall_place, place_suffix(st.overall_place),
                  rally_gap_str(st.gap_to_leader).c_str());
    vl.color = st.overall_place <= 3 ? kRallyGreen : kRallyCyan;
    vl.size = 17.0f * u;
    out.text(lx0 + lw * 0.54f, ly0 + 84.0f * u, op, vl);
  }

  // Right side: H_POST_RACE_RESULTS style floating tilted PILL03 standings table!
  const float rx0 = lx0 + lw + 26.0f * u;
  const float rx1 = f.x(1206.0f);
  ui4::offwhite_strip(out, rx0, ly0, rx1, ly0 + 28.0f * u, u, false);
  ui::TextStyle ch = ui4::style(ui::Font::kD, 12.0f * u, ui4::kInk);
  out.text(rx0 + 14.0f * u, ly0 + 8.0f * u, "POS", ch);
  out.text(rx0 + 58.0f * u, ly0 + 8.0f * u, "DRIVER", ch);
  ch.align = ui::Align::kRight;
  out.text(rx1 - 210.0f * u, ly0 + 8.0f * u, "STAGE", ch);
  out.text(rx1 - 104.0f * u, ly0 + 8.0f * u, "TOTAL", ch);
  out.text(rx1 - 14.0f * u, ly0 + 8.0f * u, "GAP", ch);

  std::vector<const race::RallyStandingRow*> rows;
  bool player_in_top = false;
  for (size_t i = 0; i < st.standings.size() && rows.size() < 11; ++i) {
    rows.push_back(&st.standings[i]);
    if (st.standings[i].is_player) player_in_top = true;
  }
  if (!player_in_top) {
    for (const auto& r : st.standings) {
      if (r.is_player) {
        if (rows.size() >= 11) rows.back() = &r;
        else rows.push_back(&r);
        break;
      }
    }
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& r = *rows[i];
    const float y = ly0 + 34.0f * u + float(i) * 34.0f * u;
    if (r.is_player) {
      ui4::brush(out, rx0 - 6.0f * u, y, rx1 + 4.0f * u, y + 30.0f * u, ui4::kPink, false);
    } else {
      ui4::pill(out, rx0, y, rx1, y + 30.0f * u, (i % 2 == 1) ? ui4::kRowAlt : ui4::kPillGrey, ui4::PillKind::kHud01);
    }
    ui::TextStyle rs = ui4::style(r.is_player ? ui::Font::kE : ui::Font::kB, 13.0f * u, ui4::kWhite);
    out.text(rx0 + 16.0f * u, y + 8.0f * u, std::to_string(r.overall_place), rs);
    out.text(rx0 + 58.0f * u, y + 8.0f * u, r.name, rs);
    ui::TextStyle ts = ui4::style(ui::Font::kB, 13.0f * u, r.is_player ? ui4::kYellow : ui4::kWhite, ui::Align::kRight);
    out.text(rx1 - 210.0f * u, y + 8.0f * u, rally_time_str(r.last_stage_time), ts);
    out.text(rx1 - 104.0f * u, y + 8.0f * u, rally_time_str(r.total_time), ts);
    ts.color = r.overall_place == 1 ? ui4::kGreen : (r.is_player ? ui4::kWhite : ui4::kDim);
    out.text(rx1 - 14.0f * u, y + 8.0f * u, rally_gap_str(r.gap_to_leader), ts);
  }

  out.unturn();
  ui4::button_bar(out, {{"A", text_of(kContinue)}}, w, h);
}

void build_rally_championship_results_screen(ui::List& out, const RallyChampResultsState& st, float w, float h) {
  if (!st.active) return;
  const float u = h / 720.0f;
  const SceneFrame f{0.5f * w - 640.0f * u, 0.5f * h - 360.0f * u, u};
  ui4::subtle_vignette(out, w, h, 0.34f);

  // Authentic +3.0 deg tilted H_POST_RACE_RESULTS + H_POST_RACE_CREDITS_BREAKDOWN layout
  // (ZERO 8px/4px rounded kPanelAlt boxes!).
  // rally.str's RALLY RESULTS over the event's name (heats and
  // championships alike). interim (SPEC_QUESTIONS Q147): the layout is the
  // engine's, not h_post_race_rally_finished / h_post_race_rally_points.
  ui4::title_banner(out, u, f.x(76.0f), f.y(42.0f), text_of(kRallyResultsTitle), st.title, true, 600.0f);

  ui4::TiltScope tilt(out, f.x(640.0f), f.y(380.0f), ui4::kTiltRad);

  const float lx0 = f.x(76.0f), lw = 472.0f * u, ly0 = f.y(158.0f);

  // Final Place + Time brush strip.
  char pbuf[48];
  std::snprintf(pbuf, sizeof pbuf, "%d%s PLACE  OF %d", st.final_place, place_suffix(st.final_place), st.field_size);
  ui4::brush(out, lx0 - 12.0f * u, ly0, lx0 + lw, ly0 + 62.0f * u, st.final_place == 1 ? ui4::kPink : ui4::kBrushBlack,
             true);
  out.text(lx0 + 16.0f * u, ly0 + 12.0f * u, pbuf, ui4::style(ui::Font::kE, 32.0f * u, kWhite));

  ui4::pill(out, lx0, ly0 + 70.0f * u, lx0 + lw, ly0 + 102.0f * u, ui4::kPillGrey, ui4::PillKind::kHud01);
  out.text(lx0 + 14.0f * u, ly0 + 78.0f * u, text_of(kTotalTimeHeading) + "  " + rally_time_str(st.total_time),
           ui4::style(ui::Font::kB, 15.0f * u, kWhite));
  out.text(lx0 + lw - 14.0f * u, ly0 + 78.0f * u, rally_gap_str(st.gap_to_leader),
           ui4::style(ui::Font::kE, 14.0f * u, kRallyCyan, ui::Align::kRight));

  // Rally XP & Wristband strip (sharp flat progress bar).
  const float xpy = ly0 + 118.0f * u;
  ui4::panel(out, lx0, xpy, lx0 + lw, xpy + 88.0f * u, u, ui4::kCharcoalDark, kRallyGold, 0, true);
  {
    char xbuf[96];
    std::snprintf(xbuf, sizeof xbuf, "RALLY XP  +%d XP  (%d TOTAL)", st.rally_xp_earned, st.new_rally_xp);
    out.text(lx0 + 14.0f * u, xpy + 12.0f * u, xbuf, ui4::style(ui::Font::kE, 14.0f * u, ui4::kYellow));
    out.text(lx0 + 14.0f * u, xpy + 36.0f * u,
             st.wristband_up ? ("PROMOTION: " + st.wristband_name) : st.wristband_name,
             ui4::style(ui::Font::kB, 13.0f * u, st.wristband_up ? kRallyGreen : kMuted));
    const float bx0 = lx0 + 14.0f * u, bx1 = lx0 + lw - 14.0f * u;
    out.rect(bx0, xpy + 62.0f * u, bx1, xpy + 72.0f * u, ui::rgba(46, 54, 66, 220));
    const float frac = st.next_level_xp > 0 ? std::clamp(float(st.new_rally_xp) / float(st.next_level_xp), 0.0f, 1.0f) : 1.0f;
    out.rect(bx0, xpy + 62.0f * u, bx0 + (bx1 - bx0) * frac, xpy + 72.0f * u, kRallyGold);
  }

  // Credits Breakdown (H_POST_RACE_CREDITS_BREAKDOWN brush strips).
  const float cry = xpy + 104.0f * u;
  auto cr_row = [&](float y, const std::string& lbl, const std::string& val, bool total) {
    ui4::brush(out, lx0, y, lx0 + lw * 0.64f, y + (total ? 36.0f : 28.0f) * u, ui4::kBrushBlack, total);
    out.text(lx0 + 14.0f * u, y + (total ? 8.0f : 6.0f) * u, lbl,
             ui4::style(total ? ui::Font::kE : ui::Font::kB, (total ? 17.0f : 13.0f) * u, kWhite));
    ui4::brush(out, lx0 + lw * 0.66f, y, lx0 + lw, y + (total ? 36.0f : 28.0f) * u, ui4::kPink, total);
    out.text(lx0 + lw - 14.0f * u, y + (total ? 8.0f : 6.0f) * u, val,
             ui4::style(ui::Font::kE, (total ? 18.0f : 14.0f) * u, kWhite, ui::Align::kRight, true));
  };
  cr_row(cry, "CHAMPIONSHIP PRIZE", thousands(st.base_credits) + " Cr", false);
  cr_row(cry + 34.0f * u, "DIFFICULTY & ASSISTS BONUS", "+" + thousands(st.bonus_credits) + " Cr", false);
  cr_row(cry + 72.0f * u, "TOTAL EARNED", "+" + thousands(st.total_credits) + " Cr", true);
  if (!st.unlocked_text.empty()) {
    ui4::offwhite_strip(out, lx0, cry + 118.0f * u, lx0 + lw, cry + 146.0f * u, u, true);
    out.text(lx0 + 14.0f * u, cry + 125.0f * u, "UNLOCKED: " + st.unlocked_text,
             ui4::style(ui::Font::kD, 12.5f * u, ui4::kInk));
  }

  // Right: H_POST_RACE_RESULTS tilted standings table (PILL03 rows, off-white header, pink highlight).
  const float rx0 = lx0 + lw + 26.0f * u;
  const float rx1 = f.x(1206.0f);
  out.text(rx0, ly0 - 24.0f * u, text_of(kRallyStandingsTitle), ui4::style(ui::Font::kE, 16.0f * u, kWhite));
  ui4::offwhite_strip(out, rx0, ly0, rx1, ly0 + 28.0f * u, u, false);
  {
    ui::TextStyle ch = ui4::style(ui::Font::kD, 12.5f * u, ui4::kInk);
    out.text(rx0 + 14.0f * u, ly0 + 7.0f * u, "POS", ch);
    out.text(rx0 + 58.0f * u, ly0 + 7.0f * u, "DRIVER", ch);
    ch.align = ui::Align::kRight;
    out.text(rx1 - 96.0f * u, ly0 + 7.0f * u, "TOTAL", ch);
    out.text(rx1 - 14.0f * u, ly0 + 7.0f * u, "GAP", ch);
  }
  std::vector<const race::RallyStandingRow*> rows;
  bool in_top = false;
  for (size_t i = 0; i < st.standings.size() && rows.size() < 11; ++i) {
    rows.push_back(&st.standings[i]);
    if (st.standings[i].is_player) in_top = true;
  }
  if (!in_top) {
    for (const auto& r : st.standings)
      if (r.is_player) {
        if (rows.size() >= 11) rows.back() = &r;
        else rows.push_back(&r);
        break;
      }
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& r = *rows[i];
    const float y = ly0 + 34.0f * u + float(i) * 34.0f * u;
    if (r.is_player) {
      ui4::brush(out, rx0 - 6.0f * u, y, rx1 + 4.0f * u, y + 30.0f * u, ui4::kPink, false);
    } else {
      ui4::pill(out, rx0, y, rx1, y + 30.0f * u, (i % 2 == 1) ? ui4::kRowAlt : ui4::kPillGrey, ui4::PillKind::kHud01);
    }
    ui::TextStyle rs = ui4::style(r.is_player ? ui::Font::kE : ui::Font::kB, 13.0f * u, kWhite);
    out.text(rx0 + 16.0f * u, y + 8.0f * u, std::to_string(r.overall_place), rs);
    out.text(rx0 + 58.0f * u, y + 8.0f * u, r.name, rs);
    ui::TextStyle ts = ui4::style(ui::Font::kB, 13.0f * u, r.is_player ? ui4::kYellow : kWhite, ui::Align::kRight);
    out.text(rx1 - 96.0f * u, y + 8.0f * u, rally_time_str(r.total_time), ts);
    ts.color = r.overall_place == 1 ? kRallyGreen : (r.is_player ? kWhite : kMuted);
    out.text(rx1 - 14.0f * u, y + 8.0f * u, rally_gap_str(r.gap_to_leader), ts);
  }

  // Dedicated celebratory Rally Champion ceremony banner when the player claims the Rally Crown.
  if (st.champion_unlocked) {
    const float cy0 = f.y(572.0f);
    const float cy1 = f.y(628.0f);
    ui4::brush(out, lx0 - 12.0f * u, cy0, rx1 + 8.0f * u, cy1, ui4::kPink, true);
    out.rect(lx0 - 12.0f * u, cy0, rx1 + 8.0f * u, cy0 + 4.0f * u, kRallyGold);
    out.rect(lx0 - 12.0f * u, cy1 - 4.0f * u, rx1 + 8.0f * u, cy1, kRallyGold);
    out.text(f.x(640.0f), cy0 + 9.0f * u, "HORIZON RALLY CHAMPION!",
             ui4::style(ui::Font::kE, 22.0f * u, ui4::kYellow, ui::Align::kCenter, true));
    out.text(f.x(640.0f), cy0 + 34.0f * u, st.wristband_name,
             ui4::style(ui::Font::kB, 12.5f * u, kWhite, ui::Align::kCenter));
  }

  out.unturn();
  ui4::button_bar(out, {{"A", text_of(kContinue)}}, w, h);
}

RallyHubState make_rally_hub_state(const race::RallyCatalog& catalog, const drive::DriveSave& save,
                                   const garage::Garage* g, const cars::PartsDb* db,
                                   const std::vector<garage::ClassBand>& bands, const std::string& car_media,
                                   int car_class_id, int car_pi, int discount_pct, int selected,
                                   const std::string& notice, const std::filesystem::path& ghosts_dir) {
  RallyHubState st;
  st.active = true;
  st.rally_level = save.rally_level;
  st.rally_xp = save.rally_xp;
  st.next_level_xp = catalog.next_wristband_xp(save.rally_level);
  st.wristband_name = catalog.wristband_name(save.rally_level);
  st.num_rallies = save.num_rallies;
  st.num_stages = save.num_rally_stages;
  st.num_victories = save.num_rally_victories;
  st.rally_winnings = save.rally_winnings;
  st.credits = save.credits;
  st.car_name = car_name(car_media);
  st.car_class = (!bands.empty() && car_class_id >= 0 && size_t(car_class_id) < bands.size())
                     ? bands[size_t(car_class_id)].name
                     : race::rally_class_name(car_class_id);
  st.car_pi = car_pi;
  st.notice = notice;

  if (g && db && db->is_open()) {
    if (const garage::Car* sc = g->selected_car()) {
      st.kit_status = garage::inspect_rally_kit(*sc, *db);
      const auto plan = garage::plan_rally_kit(*g, *db, sc->id, discount_pct, true);
      st.rally_kit_cost = plan.ok ? plan.total_cost : 0;
    }
  }

  const bool heats_done = race::is_rally_heats_completed(save);
  auto best_place_of = [&](int64_t eid) -> int {
    for (const auto& [id, place] : save.event_results)
      if (id == eid) return place;
    return 0;
  };

  // 1. RALLY_HEATS intro (always accessible with loaned WRX Rally twin).
  {
    RallyHubItem it;
    it.kind = RallyHubItem::kHeats;
    it.event_id = catalog.heats.event_id;
    it.title = catalog.heats.name;
    it.class_badge = "LOAN";
    it.unlocked = true;
    it.eligible = true;
    it.cash_prize = catalog.heats.cash_prize;
    it.stages = 1;
    it.stage_names = {"Horizon Rally Heats Qualifier (Subaru WRX Rally)"};
    const int bp = best_place_of(catalog.heats.event_id);
    it.status_badge = bp > 0 ? (std::to_string(bp) + place_suffix(bp)) : "START HERE";
    st.items.push_back(std::move(it));
  }

  // 2. All 7 x 4-stage championships in hub order.
  for (const auto& ch : catalog.championships) {
    RallyHubItem it;
    it.kind = RallyHubItem::kChampionship;
    it.event_id = ch.event_id;
    it.title = ch.name;
    it.class_badge = race::rally_class_name(ch.target_class);
    it.cash_prize = ch.cash_prize;
    it.stages = int(ch.stages.size());
    for (const auto& s : ch.stages) it.stage_names.push_back(s.name);
    it.unlocked = catalog.is_championship_unlocked(ch.event_id, save.rally_xp, heats_done, save.event_unlocks,
                                                   save.event_results);
    const bool class_ok = (car_class_id <= ch.target_class);
    const bool rally_ok = st.kit_status.is_rally_upgraded;
    it.eligible = it.unlocked && class_ok && rally_ok;
    const int bp = best_place_of(ch.event_id);
    if (!it.unlocked) {
      it.status_badge = "LOCKED";
      if (!heats_done) it.lock_reason = "COMPLETE HORIZON RALLY HEATS FIRST";
      else if (save.rally_xp < ch.unlock_points_req)
        it.lock_reason = "NEEDS " + std::to_string(ch.unlock_points_req) + " RALLY XP";
      else
        it.lock_reason = "COMPLETE PREVIOUS RALLY CHAMPIONSHIP";
    } else if (!rally_ok) {
      it.status_badge = "NEEDS RALLY KIT";
      it.lock_reason = "FIT RALLY KIT (TIRES/SUSP/GEARBOX)";
    } else if (!class_ok) {
      it.status_badge = "CLASS > " + it.class_badge;
      it.lock_reason = "CAR EXCEEDS CLASS " + it.class_badge + " LIMIT";
    } else {
      it.status_badge = bp > 0 ? (std::to_string(bp) + place_suffix(bp)) : "READY";
    }
    st.items.push_back(std::move(it));
  }

  // 2b. Rally Rivals / Time Trials (offline Bronze/Silver/Gold .shgh target ghosts & PB tracking).
  {
    int rival_route = catalog.heats.route_id > 0 ? catalog.heats.route_id : 24;
    std::string stage_title = catalog.heats.name.empty() ? "Horizon Rally Stage" : catalog.heats.name;
    double base_stage_time = 0.0;
    for (double seg : catalog.heats.track.best) base_stage_time += seg;
    if (!catalog.championships.empty() && !catalog.championships.front().stages.empty()) {
      const auto& stg = catalog.championships.front().stages.front();
      if (stg.route_id > 0) {
        rival_route = stg.route_id;
        stage_title = stg.name;
        double sum = 0.0;
        for (double seg : stg.track.best) sum += seg;
        if (sum > 10.0) base_stage_time = sum;
      }
    }
    if (base_stage_time < 15.0) base_stage_time = 95.0;
    const std::string stage_eid = "stage:" + std::to_string(rival_route);
    RallyHubItem it;
    it.kind = RallyHubItem::kRivals;
    it.event_id = catalog.heats.event_id;
    it.route_id = rival_route;
    it.title = "RALLY RIVALS / TIME TRIAL";
    it.class_badge = "RIVAL";
    it.unlocked = true;
    it.eligible = true;
    it.stages = 1;
    it.stage_names = {stage_title};
    it.rival_tiers = race::generate_rivals_targets(stage_eid, base_stage_time, 15000, true);
    if (!ghosts_dir.empty()) {
      race::GhostRun pb;
      if (race::load_personal_best_ghost(ghosts_dir, stage_eid, pb, nullptr) && pb.total_time > 0.0) {
        it.pb_time_s = pb.total_time;
        it.pb_clean = pb.clean;
      }
    }
    int tier_idx = 1;
    if (it.pb_time_s > 0.0) {
      if (it.pb_time_s < it.rival_tiers[2].target_time_s) tier_idx = 0;
      else if (it.pb_time_s < it.rival_tiers[1].target_time_s) tier_idx = 3;
      else if (it.pb_time_s < it.rival_tiers[0].target_time_s) tier_idx = 2;
    }
    it.rival_tier_index = tier_idx;
    if (tier_idx == 0 && it.pb_time_s > 0.0) {
      it.rival_name = "PERSONAL BEST";
      it.target_time_s = it.pb_time_s;
      it.bounty_cr = it.rival_tiers[2].bounty_cr;
      it.status_badge = "GOLD+";
    } else {
      const auto& rt = it.rival_tiers[size_t(std::clamp(tier_idx - 1, 0, 2))];
      it.rival_name = rt.rival_name;
      it.target_time_s = rt.target_time_s;
      it.bounty_cr = rt.bounty_cr;
      it.status_badge = rt.tier_label;
    }
    it.cash_prize = it.bounty_cr;
    st.items.push_back(std::move(it));
  }

  // 3. One-click Fit Rally Kit conversion.
  {
    RallyHubItem it;
    it.kind = RallyHubItem::kFitRallyKit;
    it.title = st.kit_status.is_full_kit ? "RALLY KIT INSTALLED" : "FIT HORIZON RALLY KIT";
    it.class_badge = "KIT";
    it.unlocked = true;
    it.eligible = !st.kit_status.is_full_kit;
    it.status_badge = st.kit_status.is_full_kit ? "FITTED" : (thousands(st.rally_kit_cost) + " CR");
    st.items.push_back(std::move(it));
  }

  // 4. Free Roam ColoradoDirt.
  {
    RallyHubItem it;
    it.kind = RallyHubItem::kFreeRoam;
    it.title = "FREE ROAM COLORADO DIRT";
    it.class_badge = "DIRT";
    it.unlocked = true;
    it.eligible = true;
    it.status_badge = "DRIVE";
    st.items.push_back(std::move(it));
  }

  // 5. Return to Colorado Festival.
  {
    RallyHubItem it;
    it.kind = RallyHubItem::kReturnColorado;
    it.title = "RETURN TO COLORADO FESTIVAL";
    it.class_badge = "EXIT";
    it.unlocked = true;
    it.eligible = true;
    it.status_badge = "COLORADO";
    st.items.push_back(std::move(it));
  }

  st.selected = std::clamp(selected, 0, std::max(0, int(st.items.size()) - 1));
  return st;
}

RallyServiceAreaState make_rally_service_state(const race::RallyChampionshipSession& session,
                                               const std::string& cname, const std::string& cclass, int cpi,
                                               double damage_pct, bool repaired, int selected,
                                               const std::string& notice) {
  RallyServiceAreaState st;
  st.active = true;
  st.selected = std::clamp(selected, 0, 3);
  st.championship_name = session.title();
  const auto& fin = session.last_stage_finish();
  st.completed_stage = fin.stage_index + 1;
  st.total_stages = fin.total_stages;
  if (fin.stage_index >= 0 && size_t(fin.stage_index) < session.stages().size())
    st.completed_stage_name = session.stages()[size_t(fin.stage_index)].name;
  if (fin.stage_index + 1 < int(session.stages().size()))
    st.next_stage_name = session.stages()[size_t(fin.stage_index + 1)].name;
  st.last_stage_time = fin.player_stage_time;
  st.stage_place = fin.stage_place;
  st.total_time = fin.player_total_time;
  st.overall_place = fin.overall_place;
  st.gap_to_leader = fin.overall_gap_to_leader;
  st.damage_pct = damage_pct;
  st.repaired = repaired;
  st.car_name = cname;
  st.car_class = cclass;
  st.car_pi = cpi;
  st.standings = session.overall_standings();
  st.notice = notice;
  return st;
}

RallyChampResultsState make_rally_results_state(const race::RallyCatalog& catalog,
                                                const race::RallyChampionshipSession& session,
                                                const race::RallyChampionshipReward& reward, int64_t new_balance) {
  RallyChampResultsState st;
  st.active = true;
  st.is_heats = reward.is_heats;
  st.title = session.title();
  st.final_place = reward.final_place;
  st.field_size = int(session.overall_standings().size());
  st.total_time = reward.total_time;
  st.gap_to_leader = session.last_stage_finish().overall_gap_to_leader;
  st.rally_xp_earned = reward.rally_xp_earned;
  st.old_rally_xp = reward.old_rally_xp;
  st.new_rally_xp = reward.new_rally_xp;
  st.next_level_xp = catalog.next_wristband_xp(reward.new_rally_level);
  st.wristband_name = catalog.wristband_name(reward.new_rally_level);
  st.wristband_up = reward.wristband_up;
  st.champion_unlocked = reward.champion_unlocked;
  st.base_credits = reward.base_credits;
  st.bonus_credits = reward.bonus_credits;
  st.total_credits = reward.total_credits;
  st.new_balance = new_balance;
  st.standings = session.overall_standings();
  if (reward.is_heats) {
    st.unlocked_text = "BEAUMONT RALLY CHAMPIONSHIP & ALL 28 STAGES";
  } else if (reward.champion_unlocked) {
    st.unlocked_text = "HORIZON RALLY CHAMPION!";
  } else if (!reward.unlocked_events.empty()) {
    if (const auto* next = catalog.find_championship(reward.unlocked_events.front()))
      st.unlocked_text = next->name + " (" + race::rally_class_name(next->target_class) + " CLASS)";
  }
  return st;
}

}  // namespace carview
