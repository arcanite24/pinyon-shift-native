#include "rally_hub.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game_hud.h"

namespace carview {
namespace {

constexpr uint32_t kDarkBg = ui::rgba(14, 17, 22, 228);
constexpr uint32_t kPanelBg = ui::rgba(20, 24, 31, 236);
constexpr uint32_t kPanelAlt = ui::rgba(28, 34, 44, 230);
constexpr uint32_t kBorder = ui::rgba(255, 255, 255, 36);
constexpr uint32_t kWhite = ui::rgba(246, 248, 252, 255);
constexpr uint32_t kMuted = ui::rgba(168, 178, 194, 255);
constexpr uint32_t kRallyGold = ui::rgba(255, 184, 28, 255);
constexpr uint32_t kRallyOrange = ui::rgba(242, 108, 30, 255);
constexpr uint32_t kRallyRed = ui::rgba(228, 48, 52, 255);
constexpr uint32_t kRallyGreen = ui::rgba(46, 204, 113, 255);
constexpr uint32_t kRallyYellow = ui::rgba(250, 212, 42, 255);
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

// Draws crisp directional pace-note geometry inside box [cx - r, cy - r, cx + r, cy + r].
void draw_pace_symbol(ui::List& out, race::PaceIconKind kind, float cx, float cy, float r, uint32_t col) {
  const float lw = std::max(2.5f, r * 0.19f);
  auto arrow_head = [&](float tx, float ty, float dx, float dy) {
    const float len = std::hypot(dx, dy);
    if (len < 1e-4f) return;
    const float ux = dx / len, uy = dy / len;
    const float px = -uy, py = ux;
    const float ah = r * 0.36f, aw = r * 0.26f;
    out.line(tx, ty, tx - ux * ah + px * aw, ty - uy * ah + py * aw, lw, col);
    out.line(tx, ty, tx - ux * ah - px * aw, ty - uy * ah - py * aw, lw, col);
  };

  switch (kind) {
    case race::PaceIconKind::kEasyLeft:
    case race::PaceIconKind::kEasyRight: {
      const float s = (kind == race::PaceIconKind::kEasyRight) ? 1.0f : -1.0f;
      // Gentle bend up-and-outward.
      out.line(cx - s * r * 0.15f, cy + r * 0.65f, cx - s * r * 0.15f, cy + r * 0.05f, lw, col);
      out.line(cx - s * r * 0.15f, cy + r * 0.05f, cx + s * r * 0.42f, cy - r * 0.58f, lw, col);
      arrow_head(cx + s * r * 0.42f, cy - r * 0.58f, s * 0.57f, -0.63f);
      break;
    }
    case race::PaceIconKind::kMediumLeft:
    case race::PaceIconKind::kMediumRight: {
      const float s = (kind == race::PaceIconKind::kMediumRight) ? 1.0f : -1.0f;
      // Medium 45-degree corner.
      out.line(cx - s * r * 0.25f, cy + r * 0.65f, cx - s * r * 0.25f, cy + r * 0.05f, lw, col);
      out.line(cx - s * r * 0.25f, cy + r * 0.05f, cx + s * r * 0.55f, cy - r * 0.40f, lw, col);
      arrow_head(cx + s * r * 0.55f, cy - r * 0.40f, s * 0.80f, -0.45f);
      break;
    }
    case race::PaceIconKind::kHardLeft:
    case race::PaceIconKind::kHardRight: {
      const float s = (kind == race::PaceIconKind::kHardRight) ? 1.0f : -1.0f;
      // Tight bend (approx 75 deg).
      out.line(cx - s * r * 0.35f, cy + r * 0.65f, cx - s * r * 0.35f, cy - r * 0.10f, lw, col);
      out.line(cx - s * r * 0.35f, cy - r * 0.10f, cx + s * r * 0.58f, cy - r * 0.28f, lw, col);
      arrow_head(cx + s * r * 0.58f, cy - r * 0.28f, s * 0.93f, -0.18f);
      break;
    }
    case race::PaceIconKind::kSquareLeft:
    case race::PaceIconKind::kSquareRight: {
      const float s = (kind == race::PaceIconKind::kSquareRight) ? 1.0f : -1.0f;
      // Crisp 90-degree square corner.
      out.line(cx - s * r * 0.38f, cy + r * 0.65f, cx - s * r * 0.38f, cy - r * 0.22f, lw, col);
      out.line(cx - s * r * 0.38f, cy - r * 0.22f, cx + s * r * 0.58f, cy - r * 0.22f, lw, col);
      arrow_head(cx + s * r * 0.58f, cy - r * 0.22f, s, 0.0f);
      break;
    }
    case race::PaceIconKind::kHairpinLeft:
    case race::PaceIconKind::kHairpinRight: {
      const float s = (kind == race::PaceIconKind::kHairpinRight) ? 1.0f : -1.0f;
      // 180-degree U-turn hairpin.
      const float x0 = cx - s * r * 0.34f;
      const float x1 = cx + s * r * 0.34f;
      out.line(x0, cy + r * 0.65f, x0, cy - r * 0.22f, lw, col);
      out.arc(cx, cy - r * 0.22f, r * 0.34f, lw, -1.5708f, 1.5708f, col);
      out.line(x1, cy - r * 0.22f, x1, cy + r * 0.45f, lw, col);
      arrow_head(x1, cy + r * 0.45f, 0.0f, 1.0f);
      break;
    }
    case race::PaceIconKind::kCrest: {
      // Hill crest arch + dashed road line.
      out.line(cx - r * 0.68f, cy + r * 0.35f, cx - r * 0.22f, cy - r * 0.35f, lw, col);
      out.line(cx - r * 0.22f, cy - r * 0.35f, cx + r * 0.22f, cy - r * 0.35f, lw, col);
      out.line(cx + r * 0.22f, cy - r * 0.35f, cx + r * 0.68f, cy + r * 0.35f, lw, col);
      break;
    }
    case race::PaceIconKind::kJump: {
      // Ramp + airborne chevron.
      out.line(cx - r * 0.65f, cy + r * 0.45f, cx + r * 0.15f, cy - r * 0.15f, lw, col);
      out.line(cx + r * 0.25f, cy - r * 0.32f, cx + r * 0.65f, cy - r * 0.45f, lw * 0.9f, col);
      arrow_head(cx + r * 0.65f, cy - r * 0.45f, 0.4f, -0.13f);
      break;
    }
    case race::PaceIconKind::kCaution: {
      // Warning triangle + exclamation mark.
      out.line(cx, cy - r * 0.62f, cx - r * 0.62f, cy + r * 0.55f, lw * 0.85f, col);
      out.line(cx, cy - r * 0.62f, cx + r * 0.62f, cy + r * 0.55f, lw * 0.85f, col);
      out.line(cx - r * 0.62f, cy + r * 0.55f, cx + r * 0.62f, cy + r * 0.55f, lw * 0.85f, col);
      out.line(cx, cy - r * 0.24f, cx, cy + r * 0.18f, lw * 0.9f, col);
      out.circle(cx, cy + r * 0.36f, lw * 0.55f, col);
      break;
    }
    default: break;
  }
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

  // 1. Top-center Co-Driver Pace-Note Strip.
  if (st.pace.active && (!st.pace.icons.empty() || !st.pace.modifiers.empty())) {
    const size_t n_icons = std::min<size_t>(st.pace.icons.size(), 4);
    const float card_w = 78.0f * u;
    const float card_h = 84.0f * u;
    const float gap = 10.0f * u;
    const float strip_w = n_icons > 0 ? (float(n_icons) * card_w + float(n_icons - 1) * gap) : 220.0f * u;
    const float x0 = 0.5f * (w - strip_w);
    const float y0 = 24.0f * u;

    // Backing bar behind pace-note icons.
    const float pad = 12.0f * u;
    const float mods_h = st.pace.modifiers.empty() ? 0.0f : 28.0f * u;
    out.shadow(x0 - pad, y0 - 8.0f * u, x0 + strip_w + pad, y0 + card_h + mods_h + 12.0f * u,
               ui::rgba(0, 0, 0, 140), 10.0f * u, 10.0f * u);
    out.rect(x0 - pad, y0 - 8.0f * u, x0 + strip_w + pad, y0 + card_h + mods_h + 12.0f * u,
             ui::rgba(14, 17, 23, 210), 10.0f * u);
    out.frame(x0 - pad, y0 - 8.0f * u, x0 + strip_w + pad, y0 + card_h + mods_h + 12.0f * u,
              ui::rgba(255, 184, 28, 120), 1.5f * u, 10.0f * u);

    for (size_t i = 0; i < n_icons; ++i) {
      const auto& ic = st.pace.icons[i];
      const float ix = x0 + float(i) * (card_w + gap);
      const uint32_t accent = icon_accent_color(ic.kind);
      const uint32_t bg = ic.active ? ui::rgba(32, 38, 48, 245) : ui::rgba(22, 26, 34, 220);
      out.rect(ix, y0, ix + card_w, y0 + card_h, bg, 8.0f * u);
      out.rect(ix, y0, ix + card_w, y0 + 5.0f * u, accent, 2.0f * u);
      out.frame(ix, y0, ix + card_w, y0 + card_h, ic.active ? accent : kBorder, ic.active ? 2.2f * u : 1.0f * u,
                8.0f * u);
      draw_pace_symbol(out, ic.kind, ix + card_w * 0.5f, y0 + card_h * 0.44f, card_w * 0.34f, accent);

      ui::TextStyle ts;
      ts.size = 15.0f * u;
      ts.color = kWhite;
      ts.align = ui::Align::kCenter;
      ts.font = ui::Font::kE;
      out.text(ix + card_w * 0.5f, y0 + card_h - 21.0f * u, ic.label, ts);
    }

    // Modifier pills ("50M", "DON'T CUT", "CREST", "CAUTION", etc.).
    if (!st.pace.modifiers.empty()) {
      const float my = y0 + card_h + 8.0f * u;
      std::string mod_line;
      for (const auto& m : st.pace.modifiers) {
        if (!mod_line.empty()) mod_line += "   •   ";
        mod_line += m;
      }
      ui::TextStyle ms;
      ms.size = 14.0f * u;
      ms.color = kRallyGold;
      ms.align = ui::Align::kCenter;
      ms.font = ui::Font::kB;
      out.text(w * 0.5f, my + 2.0f * u, mod_line, ms);
    }
  }

  // 2. Top-right Rally Stage Split Timer & Stage Progress.
  const float rw = 336.0f * u;
  const float rx1 = w - 24.0f * u;
  const float rx0 = rx1 - rw;
  const float ry0 = 24.0f * u;
  const float rh = (st.last_split_age < 4.5 && st.splits_passed > 0) ? 124.0f * u : 92.0f * u;

  out.shadow(rx0, ry0, rx1, ry0 + rh, ui::rgba(0, 0, 0, 140), 8.0f * u, 8.0f * u);
  out.rect(rx0, ry0, rx1, ry0 + rh, kDarkBg, 8.0f * u);
  out.rect(rx0, ry0, rx1, ry0 + 4.0f * u, kRallyGold, 2.0f * u);
  out.frame(rx0, ry0, rx1, ry0 + rh, kBorder, 1.0f * u, 8.0f * u);

  {
    ui::TextStyle hs;
    hs.size = 11.5f * u;
    hs.color = kRallyGold;
    hs.font = ui::Font::kE;
    char hdr[96];
    std::snprintf(hdr, sizeof hdr, "STAGE %d / %d  •  %s", st.stage_number, st.total_stages,
                  st.championship_name.empty() ? "HORIZON RALLY" : st.championship_name.c_str());
    out.text(rx0 + 12.0f * u, ry0 + 10.0f * u, hdr, hs);
  }
  {
    ui::TextStyle cs;
    cs.size = 24.0f * u;
    cs.color = kWhite;
    cs.font = ui::Font::kE;
    out.text(rx0 + 12.0f * u, ry0 + 30.0f * u, rally_time_str(std::max(0.0, st.stage_clock)), cs);

    ui::TextStyle ps;
    ps.size = 14.0f * u;
    ps.color = kMuted;
    ps.align = ui::Align::kRight;
    ps.font = ui::Font::kB;
    char pct[24];
    std::snprintf(pct, sizeof pct, "%d%%", int(std::lround(std::clamp(st.progress, 0.0, 1.0) * 100.0)));
    out.text(rx1 - 12.0f * u, ry0 + 36.0f * u, pct, ps);
  }

  // 4-split progress bar segments (5 sectors).
  const int sectors = std::max(1, st.total_splits + 1);
  const float bar_x0 = rx0 + 12.0f * u;
  const float bar_w = rw - 24.0f * u;
  const float bar_y = ry0 + 66.0f * u;
  const float seg_gap = 4.0f * u;
  const float seg_w = (bar_w - seg_gap * float(sectors - 1)) / float(sectors);
  for (int i = 0; i < sectors; ++i) {
    const float sx0 = bar_x0 + float(i) * (seg_w + seg_gap);
    const float sec_lo = float(i) / float(sectors);
    const float sec_hi = float(i + 1) / float(sectors);
    const float fill = std::clamp((float(st.progress) - sec_lo) / std::max(1e-4f, sec_hi - sec_lo), 0.0f, 1.0f);
    out.rect(sx0, bar_y, sx0 + seg_w, bar_y + 10.0f * u, ui::rgba(52, 60, 74, 220), 2.0f * u);
    if (fill > 0.001f) {
      const uint32_t col = (i < st.splits_passed) ? kRallyGreen : kRallyGold;
      out.rect(sx0, bar_y, sx0 + seg_w * fill, bar_y + 10.0f * u, col, 2.0f * u);
    }
  }

  // Live split delta callout banner.
  if (st.last_split_age < 4.5 && st.splits_passed > 0) {
    const float dy = ry0 + 86.0f * u;
    const bool faster = st.last_split_delta <= 0.0;
    const uint32_t dcol = faster ? kRallyGreen : kRallyRed;
    out.rect(rx0 + 12.0f * u, dy, rx1 - 12.0f * u, dy + 26.0f * u, ui::fade(dcol, 0.22), 4.0f * u);
    out.frame(rx0 + 12.0f * u, dy, rx1 - 12.0f * u, dy + 26.0f * u, dcol, 1.2f * u, 4.0f * u);
    ui::TextStyle ds;
    ds.size = 15.0f * u;
    ds.color = dcol;
    ds.font = ui::Font::kE;
    char dbuf[64];
    std::snprintf(dbuf, sizeof dbuf, "SPLIT %d   %+.2fs", st.splits_passed, st.last_split_delta);
    ds.align = ui::Align::kCenter;
    out.text(rx0 + rw * 0.5f, dy + 5.0f * u, dbuf, ds);
  }
}

void build_rally_hub_screen(ui::List& out, const RallyHubState& st, float w, float h) {
  if (!st.active) return;
  const float u = h / 720.0f;
  out.rect(0, 0, w, h, ui::rgba(8, 10, 14, 195));

  const float mx = 46.0f * u;
  const float my = 32.0f * u;
  const float pw = w - 2.0f * mx;
  const float ph = h - 2.0f * my;

  out.shadow(mx, my, mx + pw, my + ph, ui::rgba(0, 0, 0, 170), 12.0f * u, 14.0f * u);
  out.rect(mx, my, mx + pw, my + ph, kPanelBg, 12.0f * u);
  out.rect(mx, my, mx + pw, my + 6.0f * u, kRallyGold, 3.0f * u);
  out.frame(mx, my, mx + pw, my + ph, kBorder, 1.2f * u, 12.0f * u);

  // Header strip.
  {
    ui::TextStyle ts;
    ts.size = 30.0f * u;
    ts.color = kRallyGold;
    ts.font = ui::Font::kE;
    ts.slant = 0.12f;
    out.text(mx + 26.0f * u, my + 20.0f * u, "HORIZON RALLY HUB", ts);

    ui::TextStyle sub;
    sub.size = 13.0f * u;
    sub.color = kMuted;
    sub.font = ui::Font::kB;
    char sbuf[160];
    std::snprintf(sbuf, sizeof sbuf, "%s  •  %d XP / %d XP  •  %d WINS  •  %d STAGES  •  %s CR",
                  st.wristband_name.c_str(), st.rally_xp, st.next_level_xp > 0 ? st.next_level_xp : st.rally_xp,
                  st.num_victories, st.num_stages, thousands(st.credits).c_str());
    out.text(mx + 28.0f * u, my + 56.0f * u, sbuf, sub);

    // Rally XP progress bar (top right).
    const float bx1 = mx + pw - 26.0f * u;
    const float bx0 = bx1 - 240.0f * u;
    const float by = my + 34.0f * u;
    out.rect(bx0, by, bx1, by + 12.0f * u, ui::rgba(46, 54, 66, 220), 4.0f * u);
    const float frac = st.next_level_xp > 0 ? std::clamp(float(st.rally_xp) / float(st.next_level_xp), 0.0f, 1.0f) : 1.0f;
    if (frac > 0.0f) out.rect(bx0, by, bx0 + (bx1 - bx0) * frac, by + 12.0f * u, kRallyGold, 4.0f * u);
    ui::TextStyle xs;
    xs.size = 11.0f * u;
    xs.color = kWhite;
    xs.align = ui::Align::kRight;
    xs.font = ui::Font::kB;
    out.text(bx1, by - 16.0f * u, st.rally_level >= 1 ? "RALLY CHAMPION" : "NEXT: RALLY CHAMPION (310 XP)", xs);
  }

  // Left column: Event & Hub Actions list.
  const float lx0 = mx + 26.0f * u;
  const float lw = pw * 0.52f;
  const float ly0 = my + 88.0f * u;
  const float row_h = 44.0f * u;
  const float row_gap = 6.0f * u;

  for (size_t i = 0; i < st.items.size(); ++i) {
    const auto& it = st.items[i];
    const float y = ly0 + float(i) * (row_h + row_gap);
    if (y + row_h > my + ph - 52.0f * u) break;
    const bool sel = int(i) == st.selected;
    const uint32_t bg = sel ? ui::rgba(255, 184, 28, 46) : kPanelAlt;
    const uint32_t brd = sel ? kRallyGold : kBorder;
    out.rect(lx0, y, lx0 + lw, y + row_h, bg, 6.0f * u);
    if (sel) out.rect(lx0, y, lx0 + 5.0f * u, y + row_h, kRallyGold, 2.0f * u);
    out.frame(lx0, y, lx0 + lw, y + row_h, brd, sel ? 1.8f * u : 1.0f * u, 6.0f * u);

    // Class badge.
    if (!it.class_badge.empty()) {
      const uint32_t bcol = it.unlocked ? kRallyOrange : ui::rgba(90, 96, 108, 220);
      out.rect(lx0 + 12.0f * u, y + 9.0f * u, lx0 + 62.0f * u, y + row_h - 9.0f * u, bcol, 4.0f * u);
      ui::TextStyle bs;
      bs.size = 13.0f * u;
      bs.color = kWhite;
      bs.align = ui::Align::kCenter;
      bs.font = ui::Font::kE;
      out.text(lx0 + 37.0f * u, y + 14.0f * u, it.class_badge, bs);
    }

    ui::TextStyle ts;
    ts.size = 16.0f * u;
    ts.color = it.unlocked ? (sel ? kWhite : ui::rgba(224, 230, 238, 255)) : kMuted;
    ts.font = ui::Font::kE;
    out.text(lx0 + 74.0f * u, y + 13.0f * u, it.title, ts);

    if (!it.status_badge.empty()) {
      ui::TextStyle st_s;
      st_s.size = 12.0f * u;
      st_s.align = ui::Align::kRight;
      st_s.font = ui::Font::kB;
      st_s.color = !it.unlocked ? kRallyRed : (!it.eligible ? kRallyOrange : kRallyGreen);
      out.text(lx0 + lw - 14.0f * u, y + 15.0f * u, it.status_badge, st_s);
    }
  }

  // Right column: Selected item detail + current car Rally Readiness card.
  const float rx0 = lx0 + lw + 22.0f * u;
  const float rx1 = mx + pw - 26.0f * u;
  const float rw = rx1 - rx0;

  // Top right card: Selected championship / action details.
  const float dy0 = ly0;
  const float dh = 276.0f * u;
  out.rect(rx0, dy0, rx1, dy0 + dh, kPanelAlt, 8.0f * u);
  out.frame(rx0, dy0, rx1, dy0 + dh, kBorder, 1.0f * u, 8.0f * u);

  if (st.selected >= 0 && size_t(st.selected) < st.items.size()) {
    const auto& sel = st.items[size_t(st.selected)];
    ui::TextStyle dt;
    dt.size = 20.0f * u;
    dt.color = kRallyGold;
    dt.font = ui::Font::kE;
    out.text(rx0 + 16.0f * u, dy0 + 16.0f * u, sel.title, dt);

    ui::TextStyle body;
    body.size = 13.0f * u;
    body.color = kWhite;
    body.font = ui::Font::kB;
    float y = dy0 + 48.0f * u;

    if (sel.kind == RallyHubItem::kChampionship || sel.kind == RallyHubItem::kHeats) {
      char meta[128];
      std::snprintf(meta, sizeof meta, "CLASS: %s   •   PRIZE: %s CR   •   STAGES: %d", sel.class_badge.c_str(),
                    thousands(sel.cash_prize).c_str(), sel.stages);
      out.text(rx0 + 16.0f * u, y, meta, body);
      y += 26.0f * u;

      if (!sel.lock_reason.empty()) {
        ui::TextStyle ws = body;
        ws.color = kRallyRed;
        out.text(rx0 + 16.0f * u, y, "GATE: " + sel.lock_reason, ws);
        y += 24.0f * u;
      }

      ui::TextStyle sh = body;
      sh.color = kMuted;
      out.text(rx0 + 16.0f * u, y, "STAGE SCHEDULE:", sh);
      y += 22.0f * u;
      for (size_t k = 0; k < sel.stage_names.size() && k < 4; ++k) {
        char sl[128];
        std::snprintf(sl, sizeof sl, "  %zu.  %s", k + 1, sel.stage_names[k].c_str());
        out.text(rx0 + 16.0f * u, y, sl, body);
        y += 22.0f * u;
      }
    } else if (sel.kind == RallyHubItem::kFitRallyKit) {
      out.text(rx0 + 16.0f * u, y, "INSTALL COMPLETE HORIZON RALLY CONVERSION KIT:", body);
      y += 26.0f * u;
      out.text(rx0 + 16.0f * u, y, "  • RALLY TIRES (COMPOUND 9) — REQUIRED FOR 0x80 EVENTS", body);
      y += 22.0f * u;
      out.text(rx0 + 16.0f * u, y, "  • RALLY SUSPENSION (RAISED GRAVEL RIDE HEIGHT)", body);
      y += 22.0f * u;
      out.text(rx0 + 16.0f * u, y, "  • RALLY TRANSMISSION (CLOSE RATIO, -0.15S SHIFT)", body);
      y += 22.0f * u;
      out.text(rx0 + 16.0f * u, y, "  • OPTIONAL RALLY AERO & LIGHT PODS (WHERE SUPPORTED)", body);
      y += 28.0f * u;
      char cbuf[96];
      std::snprintf(cbuf, sizeof cbuf, "TOTAL CONVERSION COST: %s CR", thousands(st.rally_kit_cost).c_str());
      ui::TextStyle cs = body;
      cs.color = kRallyGold;
      out.text(rx0 + 16.0f * u, y, cbuf, cs);
    } else if (sel.kind == RallyHubItem::kFreeRoam) {
      out.text(rx0 + 16.0f * u, y, "DRIVE FREELY ACROSS COLORADO DIRT GRAVEL ROADS", body);
    } else {
      out.text(rx0 + 16.0f * u, y, "SEAMLESSLY RETURN TO COLORADO FESTIVAL FREE ROAM", body);
    }
  }

  // Bottom right card: Active Car Rally Readiness (0x80).
  const float cy0 = dy0 + dh + 16.0f * u;
  const float cy1 = my + ph - 52.0f * u;
  out.rect(rx0, cy0, rx1, cy1, kPanelAlt, 8.0f * u);
  out.frame(rx0, cy0, rx1, cy1, st.kit_status.is_rally_upgraded ? kRallyGreen : kRallyOrange, 1.4f * u, 8.0f * u);

  {
    ui::TextStyle ct;
    ct.size = 15.0f * u;
    ct.color = kWhite;
    ct.font = ui::Font::kE;
    char car_hdr[128];
    std::snprintf(car_hdr, sizeof car_hdr, "ACTIVE CAR: %s  [%s %d]", st.car_name.c_str(), st.car_class.c_str(),
                  st.car_pi);
    out.text(rx0 + 16.0f * u, cy0 + 14.0f * u, car_hdr, ct);

    ui::TextStyle st_s;
    st_s.size = 13.0f * u;
    st_s.font = ui::Font::kB;
    st_s.color = st.kit_status.is_rally_upgraded ? kRallyGreen : kRallyOrange;
    out.text(rx0 + 16.0f * u, cy0 + 40.0f * u,
             st.kit_status.is_rally_upgraded ? "RALLY SPEC ELIGIBLE (0x80 PASS)" : "NOT RALLY-UPGRADED (NEEDS RALLY TIRES)",
             st_s);

    auto pill = [&](float px, float py, const char* lbl, bool ok) {
      const float pw_p = (rw - 44.0f * u) * 0.5f;
      out.rect(px, py, px + pw_p, py + 24.0f * u, ok ? ui::fade(kRallyGreen, 0.2) : ui::rgba(50, 56, 66, 200), 4.0f * u);
      ui::TextStyle ps;
      ps.size = 12.0f * u;
      ps.color = ok ? kRallyGreen : kMuted;
      ps.font = ui::Font::kB;
      out.text(px + 8.0f * u, py + 5.0f * u, std::string(ok ? "[OK] " : "[--] ") + lbl, ps);
    };
    pill(rx0 + 16.0f * u, cy0 + 68.0f * u, "RALLY TIRES", st.kit_status.has_rally_tyres);
    pill(rx0 + 28.0f * u + (rw - 44.0f * u) * 0.5f, cy0 + 68.0f * u, "RALLY SUSPENSION",
         st.kit_status.has_rally_suspension);
    pill(rx0 + 16.0f * u, cy0 + 98.0f * u, "RALLY TRANSMISSION", st.kit_status.has_rally_transmission);
    pill(rx0 + 28.0f * u + (rw - 44.0f * u) * 0.5f, cy0 + 98.0f * u, "RALLY AERO / PODS",
         st.kit_status.optional_aero_fitted > 0);
  }

  // Footer.
  ui::TextStyle ft;
  ft.size = 13.0f * u;
  ft.color = !st.notice.empty() ? kRallyGold : kMuted;
  ft.font = ui::Font::kB;
  out.text(mx + 26.0f * u, my + ph - 32.0f * u,
           !st.notice.empty() ? st.notice : "UP/DOWN SELECT   •   ENTER / [A] CONFIRM   •   ESC / [B] CLOSE", ft);
}

void build_rally_service_area_screen(ui::List& out, const RallyServiceAreaState& st, float w, float h) {
  if (!st.active) return;
  const float u = h / 720.0f;
  out.rect(0, 0, w, h, ui::rgba(8, 10, 14, 200));

  const float mx = 46.0f * u;
  const float my = 32.0f * u;
  const float pw = w - 2.0f * mx;
  const float ph = h - 2.0f * my;

  out.shadow(mx, my, mx + pw, my + ph, ui::rgba(0, 0, 0, 170), 12.0f * u, 14.0f * u);
  out.rect(mx, my, mx + pw, my + ph, kPanelBg, 12.0f * u);
  out.rect(mx, my, mx + pw, my + 6.0f * u, kRallyOrange, 3.0f * u);
  out.frame(mx, my, mx + pw, my + ph, kBorder, 1.2f * u, 12.0f * u);

  // Header.
  {
    ui::TextStyle ts;
    ts.size = 28.0f * u;
    ts.color = kRallyGold;
    ts.font = ui::Font::kE;
    ts.slant = 0.12f;
    char hbuf[128];
    std::snprintf(hbuf, sizeof hbuf, "SERVICE AREA  —  STAGE %d OF %d COMPLETE", st.completed_stage, st.total_stages);
    out.text(mx + 26.0f * u, my + 20.0f * u, hbuf, ts);

    ui::TextStyle sub;
    sub.size = 14.0f * u;
    sub.color = kMuted;
    sub.font = ui::Font::kB;
    out.text(mx + 28.0f * u, my + 56.0f * u,
             st.championship_name + "  •  NEXT: " + st.next_stage_name, sub);
  }

  // Left side: Stage summary, damage bar, and 4 Service Area menu items.
  const float lx0 = mx + 26.0f * u;
  const float lw = pw * 0.42f;
  const float ly0 = my + 90.0f * u;

  out.rect(lx0, ly0, lx0 + lw, ly0 + 150.0f * u, kPanelAlt, 8.0f * u);
  out.frame(lx0, ly0, lx0 + lw, ly0 + 150.0f * u, kBorder, 1.0f * u, 8.0f * u);
  {
    ui::TextStyle lb;
    lb.size = 13.0f * u;
    lb.color = kMuted;
    lb.font = ui::Font::kB;
    ui::TextStyle vl;
    vl.size = 20.0f * u;
    vl.color = kWhite;
    vl.font = ui::Font::kE;

    out.text(lx0 + 16.0f * u, ly0 + 14.0f * u, "STAGE TIME", lb);
    out.text(lx0 + 16.0f * u, ly0 + 32.0f * u, rally_time_str(st.last_stage_time), vl);

    out.text(lx0 + lw * 0.54f, ly0 + 14.0f * u, "STAGE PLACE", lb);
    char sp[32];
    std::snprintf(sp, sizeof sp, "%d%s / 50", st.stage_place, place_suffix(st.stage_place));
    vl.color = kRallyGold;
    out.text(lx0 + lw * 0.54f, ly0 + 32.0f * u, sp, vl);

    out.text(lx0 + 16.0f * u, ly0 + 74.0f * u, "CUMULATIVE TOTAL", lb);
    vl.color = kWhite;
    out.text(lx0 + 16.0f * u, ly0 + 92.0f * u, rally_time_str(st.total_time), vl);

    out.text(lx0 + lw * 0.54f, ly0 + 74.0f * u, "OVERALL", lb);
    char op[48];
    std::snprintf(op, sizeof op, "%d%s (%s)", st.overall_place, place_suffix(st.overall_place),
                  rally_gap_str(st.gap_to_leader).c_str());
    vl.color = st.overall_place <= 3 ? kRallyGreen : kRallyCyan;
    out.text(lx0 + lw * 0.54f, ly0 + 92.0f * u, op, vl);
  }

  // Car Damage & Repair meter.
  const float dmy = ly0 + 164.0f * u;
  out.rect(lx0, dmy, lx0 + lw, dmy + 72.0f * u, kPanelAlt, 8.0f * u);
  out.frame(lx0, dmy, lx0 + lw, dmy + 72.0f * u, kBorder, 1.0f * u, 8.0f * u);
  {
    ui::TextStyle cs;
    cs.size = 13.0f * u;
    cs.color = kWhite;
    cs.font = ui::Font::kB;
    char cbuf[96];
    std::snprintf(cbuf, sizeof cbuf, "%s [%s %d]  •  DAMAGE: %.0f%%", st.car_name.c_str(), st.car_class.c_str(),
                  st.car_pi, st.damage_pct);
    out.text(lx0 + 16.0f * u, dmy + 14.0f * u, cbuf, cs);
    const float bx0 = lx0 + 16.0f * u, bx1 = lx0 + lw - 16.0f * u;
    out.rect(bx0, dmy + 42.0f * u, bx1, dmy + 54.0f * u, ui::rgba(46, 54, 66, 220), 4.0f * u);
    const float dmg = float(std::clamp(st.damage_pct / 100.0, 0.0, 1.0));
    const uint32_t dcol = dmg < 0.05f ? kRallyGreen : (dmg < 0.4f ? kRallyGold : kRallyRed);
    out.rect(bx0, dmy + 42.0f * u, bx0 + (bx1 - bx0) * std::max(0.03f, 1.0f - dmg), dmy + 54.0f * u, dcol, 4.0f * u);
  }

  // Service Area menu actions.
  const char* actions[4] = {
      "CONTINUE TO NEXT STAGE",
      st.repaired ? "CAR REPAIRED (100%)" : "REPAIR CAR DAMAGE (FREE)",
      "UPGRADES & TUNING",
      "RETIRE FROM CHAMPIONSHIP",
  };
  const float my0 = dmy + 88.0f * u;
  for (int i = 0; i < 4; ++i) {
    const float y = my0 + float(i) * 48.0f * u;
    const bool sel = st.selected == i;
    out.rect(lx0, y, lx0 + lw, y + 40.0f * u, sel ? ui::rgba(255, 184, 28, 48) : kPanelAlt, 6.0f * u);
    if (sel) out.rect(lx0, y, lx0 + 5.0f * u, y + 40.0f * u, kRallyGold, 2.0f * u);
    out.frame(lx0, y, lx0 + lw, y + 40.0f * u, sel ? kRallyGold : kBorder, sel ? 1.8f * u : 1.0f * u, 6.0f * u);
    ui::TextStyle as;
    as.size = 15.0f * u;
    as.color = sel ? kWhite : ui::rgba(216, 222, 232, 255);
    as.font = ui::Font::kE;
    out.text(lx0 + 18.0f * u, y + 11.0f * u, actions[i], as);
  }

  // Right side: 4-Stage Cumulative Standings Leaderboard.
  const float rx0 = lx0 + lw + 22.0f * u;
  const float rx1 = mx + pw - 26.0f * u;
  const float ry1 = my + ph - 52.0f * u;
  out.rect(rx0, ly0, rx1, ry1, kPanelAlt, 8.0f * u);
  out.frame(rx0, ly0, rx1, ry1, kBorder, 1.0f * u, 8.0f * u);

  {
    ui::TextStyle th;
    th.size = 14.0f * u;
    th.color = kRallyGold;
    th.font = ui::Font::kE;
    out.text(rx0 + 16.0f * u, ly0 + 14.0f * u, "CUMULATIVE CHAMPIONSHIP STANDINGS", th);

    ui::TextStyle ch;
    ch.size = 11.0f * u;
    ch.color = kMuted;
    ch.font = ui::Font::kB;
    out.text(rx0 + 16.0f * u, ly0 + 40.0f * u, "POS", ch);
    out.text(rx0 + 64.0f * u, ly0 + 40.0f * u, "DRIVER", ch);
    ch.align = ui::Align::kRight;
    out.text(rx1 - 180.0f * u, ly0 + 40.0f * u, "STAGE", ch);
    out.text(rx1 - 88.0f * u, ly0 + 40.0f * u, "TOTAL", ch);
    out.text(rx1 - 14.0f * u, ly0 + 40.0f * u, "GAP", ch);
  }

  // Show top 10 plus player's row if outside top 10.
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
    const float y = ly0 + 62.0f * u + float(i) * 34.0f * u;
    if (y + 30.0f * u > ry1 - 8.0f * u) break;
    if (r.is_player) {
      out.rect(rx0 + 8.0f * u, y, rx1 - 8.0f * u, y + 30.0f * u, ui::rgba(255, 184, 28, 46), 4.0f * u);
      out.frame(rx0 + 8.0f * u, y, rx1 - 8.0f * u, y + 30.0f * u, kRallyGold, 1.2f * u, 4.0f * u);
    } else if (i % 2 == 1) {
      out.rect(rx0 + 8.0f * u, y, rx1 - 8.0f * u, y + 30.0f * u, ui::rgba(255, 255, 255, 10), 4.0f * u);
    }
    ui::TextStyle rs;
    rs.size = 13.0f * u;
    rs.color = r.is_player ? kRallyGold : kWhite;
    rs.font = r.is_player ? ui::Font::kE : ui::Font::kB;
    out.text(rx0 + 18.0f * u, y + 7.0f * u, std::to_string(r.overall_place), rs);
    out.text(rx0 + 64.0f * u, y + 7.0f * u, r.name, rs);
    rs.align = ui::Align::kRight;
    out.text(rx1 - 180.0f * u, y + 7.0f * u, rally_time_str(r.last_stage_time), rs);
    out.text(rx1 - 88.0f * u, y + 7.0f * u, rally_time_str(r.total_time), rs);
    rs.color = r.overall_place == 1 ? kRallyGreen : kMuted;
    out.text(rx1 - 14.0f * u, y + 7.0f * u, rally_gap_str(r.gap_to_leader), rs);
  }

  ui::TextStyle ft;
  ft.size = 13.0f * u;
  ft.color = !st.notice.empty() ? kRallyGold : kMuted;
  ft.font = ui::Font::kB;
  out.text(mx + 26.0f * u, my + ph - 32.0f * u,
           !st.notice.empty() ? st.notice : "UP/DOWN SELECT   •   ENTER / [A] CONFIRM", ft);
}

void build_rally_championship_results_screen(ui::List& out, const RallyChampResultsState& st, float w, float h) {
  if (!st.active) return;
  const float u = h / 720.0f;
  out.rect(0, 0, w, h, ui::rgba(8, 10, 14, 210));

  const float mx = 48.0f * u;
  const float my = 32.0f * u;
  const float pw = w - 2.0f * mx;
  const float ph = h - 2.0f * my;

  out.shadow(mx, my, mx + pw, my + ph, ui::rgba(0, 0, 0, 180), 12.0f * u, 14.0f * u);
  out.rect(mx, my, mx + pw, my + ph, kPanelBg, 12.0f * u);
  out.rect(mx, my, mx + pw, my + 6.0f * u, kRallyGold, 3.0f * u);
  out.frame(mx, my, mx + pw, my + ph, kBorder, 1.2f * u, 12.0f * u);

  {
    ui::TextStyle ts;
    ts.size = 30.0f * u;
    ts.color = kRallyGold;
    ts.font = ui::Font::kE;
    ts.slant = 0.12f;
    out.text(mx + 26.0f * u, my + 20.0f * u,
             st.is_heats ? "HORIZON RALLY HEATS COMPLETE" : "CHAMPIONSHIP FINAL RESULTS", ts);

    ui::TextStyle sub;
    sub.size = 15.0f * u;
    sub.color = kWhite;
    sub.font = ui::Font::kB;
    out.text(mx + 28.0f * u, my + 58.0f * u, st.title, sub);
  }

  // Left: Final Place, Rally XP, Credits Breakdown, Unlock Banner.
  const float lx0 = mx + 26.0f * u;
  const float lw = pw * 0.44f;
  const float ly0 = my + 96.0f * u;

  out.rect(lx0, ly0, lx0 + lw, ly0 + 130.0f * u, kPanelAlt, 8.0f * u);
  out.frame(lx0, ly0, lx0 + lw, ly0 + 130.0f * u, kRallyGold, 1.4f * u, 8.0f * u);
  {
    ui::TextStyle ps;
    ps.size = 46.0f * u;
    ps.color = st.final_place == 1 ? kRallyGold : kWhite;
    ps.font = ui::Font::kE;
    char pbuf[32];
    std::snprintf(pbuf, sizeof pbuf, "%d%s", st.final_place, place_suffix(st.final_place));
    out.text(lx0 + 20.0f * u, ly0 + 20.0f * u, pbuf, ps);

    ui::TextStyle ms;
    ms.size = 14.0f * u;
    ms.color = kMuted;
    ms.font = ui::Font::kB;
    char mbuf[64];
    std::snprintf(mbuf, sizeof mbuf, "OF %d DRIVERS", st.field_size);
    out.text(lx0 + 22.0f * u, ly0 + 78.0f * u, mbuf, ms);

    ms.align = ui::Align::kRight;
    out.text(lx0 + lw - 18.0f * u, ly0 + 26.0f * u, "TOTAL TIME", ms);
    ui::TextStyle ts = ps;
    ts.size = 24.0f * u;
    ts.color = kWhite;
    ts.align = ui::Align::kRight;
    out.text(lx0 + lw - 18.0f * u, ly0 + 48.0f * u, rally_time_str(st.total_time), ts);
    ms.color = kRallyCyan;
    out.text(lx0 + lw - 18.0f * u, ly0 + 82.0f * u, rally_gap_str(st.gap_to_leader), ms);
  }

  // Rally XP & Wristband Card.
  const float xpy = ly0 + 146.0f * u;
  out.rect(lx0, xpy, lx0 + lw, xpy + 106.0f * u, kPanelAlt, 8.0f * u);
  out.frame(lx0, xpy, lx0 + lw, xpy + 106.0f * u, st.wristband_up ? kRallyGreen : kBorder, 1.2f * u, 8.0f * u);
  {
    ui::TextStyle lb;
    lb.size = 14.0f * u;
    lb.color = kRallyGold;
    lb.font = ui::Font::kE;
    char xbuf[96];
    std::snprintf(xbuf, sizeof xbuf, "RALLY XP: +%d XP  (%d TOTAL)", st.rally_xp_earned, st.new_rally_xp);
    out.text(lx0 + 16.0f * u, xpy + 14.0f * u, xbuf, lb);

    ui::TextStyle wb;
    wb.size = 13.0f * u;
    wb.color = st.wristband_up ? kRallyGreen : kMuted;
    wb.font = ui::Font::kB;
    out.text(lx0 + 16.0f * u, xpy + 40.0f * u,
             st.wristband_up ? ("WRISTBAND PROMOTION: " + st.wristband_name) : st.wristband_name, wb);

    const float bx0 = lx0 + 16.0f * u, bx1 = lx0 + lw - 16.0f * u;
    out.rect(bx0, xpy + 70.0f * u, bx1, xpy + 84.0f * u, ui::rgba(46, 54, 66, 220), 4.0f * u);
    const float frac = st.next_level_xp > 0 ? std::clamp(float(st.new_rally_xp) / float(st.next_level_xp), 0.0f, 1.0f) : 1.0f;
    out.rect(bx0, xpy + 70.0f * u, bx0 + (bx1 - bx0) * frac, xpy + 84.0f * u, kRallyGold, 4.0f * u);
  }

  // Credits Breakdown Card.
  const float cry = xpy + 122.0f * u;
  out.rect(lx0, cry, lx0 + lw, cry + 120.0f * u, kPanelAlt, 8.0f * u);
  out.frame(lx0, cry, lx0 + lw, cry + 120.0f * u, kBorder, 1.0f * u, 8.0f * u);
  {
    ui::TextStyle lb;
    lb.size = 13.0f * u;
    lb.color = kMuted;
    lb.font = ui::Font::kB;
    ui::TextStyle vl = lb;
    vl.color = kWhite;
    vl.align = ui::Align::kRight;

    out.text(lx0 + 16.0f * u, cry + 14.0f * u, "CHAMPIONSHIP PRIZE", lb);
    out.text(lx0 + lw - 16.0f * u, cry + 14.0f * u, thousands(st.base_credits) + " CR", vl);
    out.text(lx0 + 16.0f * u, cry + 38.0f * u, "DIFFICULTY & ASSISTS BONUS", lb);
    out.text(lx0 + lw - 16.0f * u, cry + 38.0f * u, "+" + thousands(st.bonus_credits) + " CR", vl);

    lb.size = 16.0f * u;
    lb.color = kRallyGold;
    lb.font = ui::Font::kE;
    vl.size = 18.0f * u;
    vl.color = kRallyGreen;
    vl.font = ui::Font::kE;
    out.text(lx0 + 16.0f * u, cry + 68.0f * u, "TOTAL CREDITS EARNED", lb);
    out.text(lx0 + lw - 16.0f * u, cry + 66.0f * u, "+" + thousands(st.total_credits) + " CR", vl);

    if (!st.unlocked_text.empty()) {
      ui::TextStyle ul;
      ul.size = 12.0f * u;
      ul.color = kRallyCyan;
      ul.font = ui::Font::kB;
      out.text(lx0 + 16.0f * u, cry + 96.0f * u, "UNLOCKED: " + st.unlocked_text, ul);
    }
  }

  // Right: Final Standings Table.
  const float rx0 = lx0 + lw + 22.0f * u;
  const float rx1 = mx + pw - 26.0f * u;
  const float ry1 = my + ph - 52.0f * u;
  out.rect(rx0, ly0, rx1, ry1, kPanelAlt, 8.0f * u);
  out.frame(rx0, ly0, rx1, ry1, kBorder, 1.0f * u, 8.0f * u);
  {
    ui::TextStyle th;
    th.size = 14.0f * u;
    th.color = kRallyGold;
    th.font = ui::Font::kE;
    out.text(rx0 + 16.0f * u, ly0 + 14.0f * u, "FINAL STANDINGS", th);

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
      const float y = ly0 + 48.0f * u + float(i) * 34.0f * u;
      if (y + 30.0f * u > ry1 - 8.0f * u) break;
      if (r.is_player) {
        out.rect(rx0 + 8.0f * u, y, rx1 - 8.0f * u, y + 30.0f * u, ui::rgba(255, 184, 28, 46), 4.0f * u);
        out.frame(rx0 + 8.0f * u, y, rx1 - 8.0f * u, y + 30.0f * u, kRallyGold, 1.2f * u, 4.0f * u);
      }
      ui::TextStyle rs;
      rs.size = 13.0f * u;
      rs.color = r.is_player ? kRallyGold : kWhite;
      rs.font = r.is_player ? ui::Font::kE : ui::Font::kB;
      out.text(rx0 + 18.0f * u, y + 7.0f * u, std::to_string(r.overall_place), rs);
      out.text(rx0 + 62.0f * u, y + 7.0f * u, r.name, rs);
      rs.align = ui::Align::kRight;
      out.text(rx1 - 92.0f * u, y + 7.0f * u, rally_time_str(r.total_time), rs);
      rs.color = r.overall_place == 1 ? kRallyGreen : kMuted;
      out.text(rx1 - 14.0f * u, y + 7.0f * u, rally_gap_str(r.gap_to_leader), rs);
    }
  }

  ui::TextStyle ft;
  ft.size = 13.0f * u;
  ft.color = kMuted;
  ft.font = ui::Font::kB;
  out.text(mx + 26.0f * u, my + ph - 32.0f * u, "ENTER / [A] RETURN TO RALLY HUB", ft);
}

RallyHubState make_rally_hub_state(const race::RallyCatalog& catalog, const drive::DriveSave& save,
                                   const garage::Garage* g, const cars::PartsDb* db,
                                   const std::vector<garage::ClassBand>& bands, const std::string& car_media,
                                   int car_class_id, int car_pi, int discount_pct, int selected,
                                   const std::string& notice) {
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
      it.lock_reason = "RESTRICTION 0x80: FIT RALLY TIRES / RALLY KIT";
    } else if (!class_ok) {
      it.status_badge = "CLASS > " + it.class_badge;
      it.lock_reason = "CAR EXCEEDS CLASS " + it.class_badge + " LIMIT";
    } else {
      it.status_badge = bp > 0 ? (std::to_string(bp) + place_suffix(bp)) : "READY";
    }
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
