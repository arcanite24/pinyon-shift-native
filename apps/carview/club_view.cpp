#include "club_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace carview {
namespace {

constexpr uint32_t kBgDim = ui::rgba(10, 12, 18, 206);
constexpr uint32_t kPanelBg = ui::rgba(16, 20, 28, 236);
constexpr uint32_t kPanelAlt = ui::rgba(24, 30, 42, 234);
constexpr uint32_t kRowSel = ui::rgba(44, 34, 60, 242);
constexpr uint32_t kBorder = ui::rgba(255, 255, 255, 36);
constexpr uint32_t kWhite = ui::rgba(248, 249, 252, 255);
constexpr uint32_t kMuted = ui::rgba(176, 186, 202, 255);
constexpr uint32_t kGold = ui::rgba(246, 198, 56, 255);
constexpr uint32_t kGoldDark = ui::rgba(148, 108, 20, 255);
constexpr uint32_t kMagenta = ui::rgba(232, 32, 118, 255);
constexpr uint32_t kGreen = ui::rgba(46, 204, 113, 255);
constexpr uint32_t kCyan = ui::rgba(56, 194, 240, 255);

std::string clip_text(const std::string& s, size_t max_chars) {
  if (s.size() <= max_chars) return s;
  if (max_chars <= 3) return s.substr(0, max_chars);
  return s.substr(0, max_chars - 3) + "...";
}

void draw_text(ui::List& out, float x, float y, float size, std::string_view text, uint32_t col,
               ui::Font font = ui::Font::kB, bool keep_case = false, ui::Align align = ui::Align::kLeft) {
  ui::TextStyle ts;
  ts.size = size;
  ts.color = col;
  ts.font = font;
  ts.keep_case = keep_case;
  ts.align = align;
  ts.slant = (font == ui::Font::kE) ? 0.12f : 0.0f;
  out.text(x, y, text, ts);
}

// Draws a crisp anti-aliased SDF FH1 1000 Club gold medal emblem.
void draw_medal_badge(ui::List& out, float cx, float cy, float r, bool earned, bool active_ring) {
  constexpr float kTwoPi = 6.2831853f;
  if (earned) {
    out.circle(cx, cy, r, kGold);
    out.arc(cx, cy, r * 0.74f, std::max(1.5f, r * 0.12f), 0.0f, kTwoPi, kGoldDark);
    const float lw = std::max(2.0f, r * 0.18f);
    out.line(cx - r * 0.34f, cy + r * 0.04f, cx - r * 0.08f, cy + r * 0.32f, lw, ui::rgba(28, 20, 8, 255));
    out.line(cx - r * 0.08f, cy + r * 0.32f, cx + r * 0.38f, cy - r * 0.28f, lw, ui::rgba(28, 20, 8, 255));
  } else {
    const uint32_t ring = active_ring ? ui::rgba(246, 198, 56, 195) : ui::rgba(140, 150, 168, 110);
    out.circle(cx, cy, r, ui::rgba(22, 26, 36, 224));
    out.arc(cx, cy, r * 0.92f, std::max(1.6f, r * 0.14f), 0.0f, kTwoPi, ring);
  }
}

void draw_box(ui::List& out, float x, float y, float w, float h, uint32_t fill, uint32_t border, float radius = 6.0f) {
  out.rect(x, y, x + w, y + h, fill, radius);
  out.frame(x, y, x + w, y + h, border, 1.5f, radius);
}

}  // namespace

void draw_club_screen(ui::List& out, int w, int h, const ClubScreenState& st) {
  if (!st.active) return;
  const float W = float(w), H = float(h);
  const float sx = W / 1280.0f, sy = H / 720.0f;
  const float s = std::min(sx, sy);

  out.rect(0, 0, W, H, kBgDim);

  const float px = 52.0f * sx;
  const float py = 40.0f * sy;
  const float pw = W - 2.0f * px;
  const float ph = H - 2.0f * py;
  out.shadow(px, py, px + pw, py + ph, ui::rgba(0, 0, 0, 150), 10.0f * s, 18.0f * s);
  draw_box(out, px, py, pw, ph, kPanelBg, kBorder, 8.0f * s);

  // Top FH1 magenta-to-gold bar.
  out.rect(px, py, px + pw * 0.40f, py + 5.0f * sy, kMagenta);
  out.rect(px + pw * 0.40f, py, px + pw, py + 5.0f * sy, kGold);

  // Header band.
  const float hdr_y = py + 18.0f * sy;
  out.rect(px + 22.0f * sx, hdr_y, px + 168.0f * sx, hdr_y + 34.0f * sy, kGold, 4.0f * s);
  draw_text(out, px + 95.0f * sx, hdr_y + 8.0f * sy, 18.0f * s, "1000 CLUB", ui::rgba(18, 16, 12, 255), ui::Font::kE,
            false, ui::Align::kCenter);

  const std::string car_hdr = st.car_name.empty() ? "COLORADO GARAGE" : st.car_name;
  draw_text(out, px + 184.0f * sx, hdr_y + 6.0f * sy, 22.0f * s, clip_text(car_hdr, 38), kWhite, ui::Font::kE);

  // Medal slot strip (up to 6 slots) on the top-right.
  const int slots = std::clamp(st.car_total > 0 ? st.car_total : 5, 1, 6);
  const float slot_r = 13.0f * s;
  const float slot_step = 34.0f * sx;
  const float slot_x0 = px + pw - 28.0f * sx - float(slots - 1) * slot_step;
  const float slot_cy = hdr_y + 18.0f * sy;
  for (int i = 0; i < slots; ++i) {
    const bool earned = (i < int(st.rows.size())) ? st.rows[size_t(i)].completed : (i < st.car_medals);
    draw_medal_badge(out, slot_x0 + float(i) * slot_step, slot_cy, slot_r, earned, st.challenges_begun);
  }

  char sub[128];
  std::snprintf(sub, sizeof sub, "%d / %d MEDALS IN THIS CAR    |    CAREER TOTAL: %d MEDALS IN %d CARS",
                st.car_medals, st.car_total, st.stats.total_medals, st.stats.cars_with_medal);
  draw_text(out, px + 22.0f * sx, hdr_y + 44.0f * sy, 13.0f * s, sub, kMuted);

  const float body_y = hdr_y + 70.0f * sy;
  const float body_h = ph - 146.0f * sy;

  if (!st.gate_reason.empty()) {
    draw_box(out, px + 24.0f * sx, body_y + 24.0f * sy, pw - 48.0f * sx, 164.0f * sy, kPanelAlt, kGold, 6.0f * s);
    draw_text(out, px + 48.0f * sx, body_y + 52.0f * sy, 20.0f * s, "1000 CLUB LOCKED", kGold, ui::Font::kE);
    draw_text(out, px + 48.0f * sx, body_y + 92.0f * sy, 16.0f * s, st.gate_reason, kWhite, ui::Font::kB, true);
    draw_text(out, px + 48.0f * sx, body_y + 128.0f * sy, 13.0f * s,
              "Earn the Horizon Green Wristband and drive one of your own Colorado garage cars to enter.", kMuted,
              ui::Font::kB, true);
    draw_text(out, px + 24.0f * sx, py + ph - 36.0f * sy, 14.0f * s, "[ESC] BACK TO PAUSE MENU", kMuted);
    return;
  }

  // Left column (67%): optional intro banner + challenge rows.
  const float left_x = px + 22.0f * sx;
  const float left_w = pw * 0.67f;
  float cur_y = body_y;

  if (st.show_intro) {
    const float intro_h = 52.0f * sy;
    draw_box(out, left_x, cur_y, left_w, intro_h, ui::rgba(38, 28, 52, 232), kMagenta, 5.0f * s);
    out.rect(left_x, cur_y, left_x + 5.0f * sx, cur_y + intro_h, kMagenta);
    draw_text(out, left_x + 16.0f * sx, cur_y + 8.0f * sy, 13.5f * s, "WELCOME TO THE HORIZON 1000 CLUB", kGold,
              ui::Font::kE);
    draw_text(out, left_x + 16.0f * sx, cur_y + 28.0f * sy, 12.0f * s,
              "Complete challenges in each garage car to earn Gold Medals and unlock 1000 Club Achievements.", kWhite,
              ui::Font::kB, true);
    cur_y += intro_h + 10.0f * sy;
  }

  const int row_count = int(st.rows.size());
  const float avail_h = (body_y + body_h) - cur_y;
  const float gap = 8.0f * sy;
  const float row_h = std::min(76.0f * sy, std::max(54.0f * sy, (avail_h - gap * float(std::max(1, row_count - 1))) /
                                                                    float(std::max(1, row_count))));

  for (int i = 0; i < row_count; ++i) {
    const auto& r = st.rows[size_t(i)];
    const bool sel = (i == st.selected);
    const float ry = cur_y + float(i) * (row_h + gap);
    draw_box(out, left_x, ry, left_w, row_h, sel ? kRowSel : kPanelAlt,
             sel ? kGold : (r.completed ? ui::rgba(246, 198, 56, 120) : kBorder), 5.0f * s);
    if (sel) out.rect(left_x, ry, left_x + 4.0f * sx, ry + row_h, kGold);

    draw_medal_badge(out, left_x + 28.0f * sx, ry + row_h * 0.5f, 15.0f * s, r.completed, st.challenges_begun);

    const float tx = left_x + 56.0f * sx;
    draw_text(out, tx, ry + 8.0f * sy, 15.5f * s, clip_text(r.title, 44), r.completed ? kGold : kWhite, ui::Font::kE);
    draw_text(out, tx, ry + 30.0f * sy, 12.5f * s, clip_text(r.description, 68), kMuted, ui::Font::kB, true);

    const float bar_w = 152.0f * sx;
    const float bar_x = left_x + left_w - bar_w - 14.0f * sx;
    if (r.completed) {
      out.rect(bar_x + 26.0f * sx, ry + 14.0f * sy, bar_x + bar_w, ry + 38.0f * sy, ui::rgba(246, 198, 56, 46),
               4.0f * s);
      draw_text(out, bar_x + 89.0f * sx, ry + 20.0f * sy, 12.0f * s, "MEDAL EARNED", kGold, ui::Font::kB, false,
                ui::Align::kCenter);
    } else {
      const std::string status_txt = r.status.empty() ? (st.challenges_begun ? "ACTIVE" : "READY") : r.status;
      draw_text(out, bar_x, ry + 10.0f * sy, 12.0f * s, clip_text(status_txt, 22),
                st.challenges_begun ? kCyan : kMuted);
      const float p = float(std::clamp(r.progress, 0.0, 1.0));
      const float by = ry + 32.0f * sy;
      out.rect(bar_x, by, bar_x + bar_w, by + 8.0f * sy, ui::rgba(255, 255, 255, 24), 3.0f * s);
      if (p > 0.001f) out.rect(bar_x, by, bar_x + bar_w * p, by + 8.0f * sy, kGold, 3.0f * s);
    }
    if (sel && !r.hint.empty() && row_h >= 62.0f * sy) {
      draw_text(out, tx, ry + 48.0f * sy, 11.0f * s, "Hint: " + clip_text(r.hint, 64), ui::rgba(198, 212, 232, 220),
                ui::Font::kB, true);
    }
  }

  // Right sidebar: 1000 Club Medal Milestones & Achievements (SPA 216..225).
  const float side_x = left_x + left_w + 16.0f * sx;
  const float side_w = (px + pw - 22.0f * sx) - side_x;
  draw_box(out, side_x, body_y, side_w, body_h, kPanelAlt, kBorder, 6.0f * s);
  out.rect(side_x, body_y, side_x + side_w, body_y + 3.0f * sy, kGold);

  draw_text(out, side_x + 14.0f * sx, body_y + 14.0f * sy, 14.5f * s, "1000 CLUB MILESTONES", kGold, ui::Font::kE);
  char ach_hdr[64];
  std::snprintf(ach_hdr, sizeof ach_hdr, "ACHIEVEMENTS: %d / 10", st.stats.achievements_unlocked);
  draw_text(out, side_x + 14.0f * sx, body_y + 36.0f * sy, 12.5f * s, ach_hdr, kWhite);

  struct MilestoneLine {
    const char* label;
    int cur;
    int target;
    bool done;
  };
  const MilestoneLine lines[] = {
      {"FIRST MEDAL", std::min(1, st.stats.total_medals), 1, st.stats.ach_first_medal},
      {"DIFFERENT CARS", st.stats.cars_with_medal, 5, st.stats.ach_multi_cars},
      {"FORD CARS", st.stats.ford_cars, 5, st.stats.ach_ford},
      {"FERRARI CARS", st.stats.ferrari_cars, 3, st.stats.ach_ferrari},
      {"SHELBY CARS", st.stats.shelby_cars, 2, st.stats.ach_shelby},
      {"LAMBORGHINI", st.stats.lamborghini_cars, 3, st.stats.ach_lamborghini},
      {"NISSAN CARS", st.stats.nissan_cars, 3, st.stats.ach_nissan},
      {"BMW CARS", st.stats.bmw_cars, 3, st.stats.ach_bmw},
      {"BRITISH CARS", st.stats.british_cars, 5, st.stats.ach_british},
      {"1,000,000+ CR CAR", st.stats.million_cr_cars, 1, st.stats.ach_million_cr},
  };
  float my = body_y + 64.0f * sy;
  const float mstep = std::min(36.0f * sy, (body_h - 86.0f * sy) / 10.0f);
  for (const auto& m : lines) {
    char val[32];
    std::snprintf(val, sizeof val, "%d/%d", std::min(m.cur, m.target), m.target);
    draw_text(out, side_x + 14.0f * sx, my, 12.0f * s, m.label, m.done ? kGold : kMuted);
    draw_text(out, side_x + side_w - 14.0f * sx, my, 12.0f * s, val, m.done ? kGreen : kWhite, ui::Font::kB, false,
              ui::Align::kRight);
    const float frac = std::clamp(float(m.cur) / float(std::max(1, m.target)), 0.0f, 1.0f);
    out.rect(side_x + 14.0f * sx, my + 16.0f * sy, side_x + side_w - 14.0f * sx, my + 20.0f * sy,
             ui::rgba(255, 255, 255, 20), 2.0f * s);
    if (frac > 0.0f) {
      out.rect(side_x + 14.0f * sx, my + 16.0f * sy, side_x + 14.0f * sx + (side_w - 28.0f * sx) * frac,
               my + 20.0f * sy, m.done ? kGreen : kGold, 2.0f * s);
    }
    my += mstep;
  }

  // Footer action strip.
  const float foot_y = py + ph - 46.0f * sy;
  const uint32_t btn_bg = st.challenges_begun ? ui::rgba(32, 98, 60, 236) : kMagenta;
  out.rect(px + 22.0f * sx, foot_y, px + 362.0f * sx, foot_y + 32.0f * sy, btn_bg, 4.0f * s);
  draw_text(out, px + 192.0f * sx, foot_y + 9.0f * sy, 13.5f * s,
            st.challenges_begun ? "CHALLENGES ACTIVE IN THIS CAR" : "[ENTER] BEGIN CHALLENGES IN THIS CAR", kWhite,
            ui::Font::kE, false, ui::Align::kCenter);
  draw_text(out, px + 382.0f * sx, foot_y + 10.0f * sy, 13.0f * s, "[UP/DOWN] INSPECT    [ESC] RETURN TO DRIVE",
            kMuted);
}

void draw_club_medal_ceremony(ui::List& out, int w, int h, const ClubMedalCeremony& c) {
  if (!c.active || c.age < 0.0 || c.age > c.duration) return;
  const float W = float(w), H = float(h);
  const float sx = W / 1280.0f, sy = H / 720.0f;
  const float s = std::min(sx, sy);

  float alpha = 1.0f;
  if (c.age < 0.25) alpha = float(c.age / 0.25);
  else if (c.age > c.duration - 0.5) alpha = float((c.duration - c.age) / 0.5);
  alpha = std::clamp(alpha, 0.0f, 1.0f);

  const float bw = 540.0f * sx;
  const float bh = 104.0f * sy;
  const float bx = c.dock_bottom_right ? (W - bw - 68.0f * sx) : (W - bw) * 0.5f;
  const float by = c.dock_bottom_right ? (H - bh - 56.0f * sy) : 18.0f * sy;

  out.shadow(bx, by, bx + bw, by + bh, ui::fade(ui::rgba(0, 0, 0, 160), alpha), 8.0f * s, 14.0f * s);
  draw_box(out, bx, by, bw, bh, ui::fade(ui::rgba(16, 20, 28, 242), alpha), ui::fade(kGold, alpha), 7.0f * s);
  out.rect(bx, by, bx + bw, by + 4.0f * sy, ui::fade(kGold, alpha));
  out.rect(bx, by + bh - 3.0f * sy, bx + bw, by + bh, ui::fade(kMagenta, alpha));

  draw_medal_badge(out, bx + 48.0f * sx, by + bh * 0.5f, 28.0f * s, true, true);

  draw_text(out, bx + 92.0f * sx, by + 12.0f * sy, 13.0f * s, "1000 CLUB MEDAL EARNED!", ui::fade(kGold, alpha),
            ui::Font::kE);
  draw_text(out, bx + 92.0f * sx, by + 32.0f * sy, 20.0f * s, clip_text(c.challenge_title, 34), ui::fade(kWhite, alpha),
            ui::Font::kE);
  if (!c.challenge_desc.empty())
    draw_text(out, bx + 92.0f * sx, by + 57.0f * sy, 12.5f * s, clip_text(c.challenge_desc, 56), ui::fade(kMuted, alpha),
              ui::Font::kB, true);

  char foot[128];
  std::snprintf(foot, sizeof foot, "%d / %d IN THIS CAR   |   TOTAL: %d MEDALS (%d CARS)", c.car_medals,
                std::max(1, c.car_total), c.total_medals, c.cars_with_medal);
  draw_text(out, bx + 92.0f * sx, by + 79.0f * sy, 11.5f * s, foot, ui::fade(kCyan, alpha));
}

void draw_club_active_strip(ui::List& out, int w, int h, const ClubActiveStrip& strip) {
  if (!strip.active) return;
  const float W = float(w), H = float(h);
  const float sx = W / 1280.0f, sy = H / 720.0f;
  const float s = std::min(sx, sy);

  const float bw = 400.0f * sx;
  const float bh = 48.0f * sy;
  const float bx = (W - bw) * 0.5f;
  const float by = 16.0f * sy;

  draw_box(out, bx, by, bw, bh, ui::rgba(16, 20, 28, 220), ui::rgba(246, 198, 56, 180), 5.0f * s);
  out.rect(bx, by, bx + 4.0f * sx, by + bh, kGold);
  draw_text(out, bx + 14.0f * sx, by + 7.0f * sy, 12.5f * s, "1000 CLUB  " + clip_text(strip.title, 28), kGold,
            ui::Font::kE);
  draw_text(out, bx + 14.0f * sx, by + 26.0f * sy, 11.5f * s, clip_text(strip.status, 40), kWhite, ui::Font::kB, true);
  const float p = float(std::clamp(strip.progress, 0.0, 1.0));
  if (p > 0.001f) {
    out.rect(bx + 4.0f * sx, by + bh - 4.0f * sy, bx + 4.0f * sx + (bw - 4.0f * sx) * p, by + bh, kGold);
  }
}

}  // namespace carview
