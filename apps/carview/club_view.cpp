#include "club_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kGold = ui::rgba(246, 198, 56, 255);
constexpr uint32_t kGoldDark = ui::rgba(148, 108, 20, 255);
constexpr uint32_t kCyan = ui::rgba(56, 194, 240, 255);

std::string clip_text(const std::string& s, size_t max_chars) {
  if (s.size() <= max_chars) return s;
  if (max_chars <= 3) return s.substr(0, max_chars);
  return s.substr(0, max_chars - 3) + "...";
}

void draw_text(ui::List& out, float x, float y, float size, std::string_view text, uint32_t col,
               ui::Font font = ui::Font::kB, bool keep_case = false, ui::Align align = ui::Align::kLeft) {
  ui::TextStyle ts = ui4::style(font, size, col, align, keep_case);
  if (font == ui::Font::kE) ts.slant = 0.12f;
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
    const uint32_t ring = active_ring ? ui::rgba(246, 198, 56, 200) : ui::rgba(150, 158, 174, 120);
    out.circle(cx, cy, r, ui::rgba(20, 22, 28, 232));
    out.arc(cx, cy, r * 0.92f, std::max(1.6f, r * 0.14f), 0.0f, kTwoPi, ring);
  }
}

}  // namespace

void draw_club_screen(ui::List& out, int w, int h, const ClubScreenState& st) {
  if (!st.active) return;
  const float W = float(w), H = float(h);
  const float sx = W / 1280.0f, sy = H / 720.0f;
  const float s = std::min(sx, sy);

  // Authentic UI4 subtle edge vignette so Colorado shines cleanly behind the screen.
  ui4::subtle_vignette(out, W, H, 0.36f);

  const float px = 48.0f * sx;
  const float py = 30.0f * sy;
  const float pw = W - 2.0f * px;
  const float ph = H - 76.0f * sy;

  ui4::panel(out, px, py, px + pw, py + ph, s, ui4::kCharcoalDark, ui4::kPink, 0, true);

  // Iconic tilted FH1 UI4 banner header ("1000 CLUB" over current car eyebrow).
  const std::string car_hdr = st.car_name.empty() ? "COLORADO GARAGE" : st.car_name;
  ui4::title_banner(out, s, px + 22.0f * sx, py + 28.0f * sy, "1000 CLUB", clip_text(car_hdr, 40), true, 520.0f);

  // Top-right off-white career medal summary & car medal slot strip.
  {
    const int slots = std::clamp(st.car_total > 0 ? st.car_total : 5, 1, 6);
    const float slot_r = 13.0f * s;
    const float slot_step = 34.0f * sx;
    const float slot_x0 = px + pw - 26.0f * sx - float(slots - 1) * slot_step;
    const float slot_cy = py + 34.0f * sy;
    for (int i = 0; i < slots; ++i) {
      const bool earned = (i < int(st.rows.size())) ? st.rows[size_t(i)].completed : (i < st.car_medals);
      draw_medal_badge(out, slot_x0 + float(i) * slot_step, slot_cy, slot_r, earned, st.challenges_begun);
    }

    const float st_x1 = px + pw - 22.0f * sx;
    const float st_x0 = st_x1 - 450.0f * sx;
    ui4::offwhite_strip(out, st_x0, py + 58.0f * sy, st_x1, py + 84.0f * sy, s, true);
    char sub[128];
    std::snprintf(sub, sizeof sub, "%d / %d IN THIS CAR   |   CAREER: %d MEDALS (%d CARS)",
                  st.car_medals, st.car_total, st.stats.total_medals, st.stats.cars_with_medal);
    draw_text(out, st_x1 - 12.0f * sx, py + 65.0f * sy, 12.0f * s, sub, ui4::kInk, ui::Font::kB, false,
              ui::Align::kRight);
  }

  const float body_y = py + 98.0f * sy;
  const float body_h = ph - 114.0f * sy;

  if (!st.gate_reason.empty()) {
    ui4::panel(out, px + 24.0f * sx, body_y + 20.0f * sy, px + pw - 24.0f * sx, body_y + 184.0f * sy, s,
               ui4::kCharcoal, kGold, ui4::kPink, true);
    draw_text(out, px + 48.0f * sx, body_y + 46.0f * sy, 20.0f * s, "1000 CLUB LOCKED", kGold, ui::Font::kE);
    draw_text(out, px + 48.0f * sx, body_y + 84.0f * sy, 16.0f * s, st.gate_reason, ui4::kWhite, ui::Font::kB, true);
    draw_text(out, px + 48.0f * sx, body_y + 120.0f * sy, 13.0f * s,
              "Earn the Horizon Green Wristband and drive one of your own Colorado garage cars to enter.", ui4::kDim,
              ui::Font::kB, true);
    ui4::button_bar(out, {{"B", "BACK"}}, W, H, 48.0f);
    return;
  }

  // Left column (67%): optional intro brush strip + sharp distressed challenge rows.
  const float left_x = px + 22.0f * sx;
  const float left_w = pw * 0.67f;
  float cur_y = body_y;

  if (st.show_intro) {
    const float intro_h = 52.0f * sy;
    ui4::panel(out, left_x, cur_y, left_x + left_w, cur_y + intro_h, s, ui4::kCharcoal, 0, ui4::kPink, true);
    draw_text(out, left_x + 16.0f * sx, cur_y + 8.0f * sy, 13.5f * s, "WELCOME TO THE HORIZON 1000 CLUB", kGold,
              ui::Font::kE);
    draw_text(out, left_x + 16.0f * sx, cur_y + 28.0f * sy, 12.0f * s,
              "Complete challenges in each garage car to earn Gold Medals and unlock 1000 Club Achievements.",
              ui4::kWhite, ui::Font::kB, true);
    cur_y += intro_h + 10.0f * sy;
  }

  const int row_count = int(st.rows.size());
  const float avail_h = (body_y + body_h) - cur_y;
  const float gap = 8.0f * sy;
  const float row_h = std::min(78.0f * sy, std::max(54.0f * sy, (avail_h - gap * float(std::max(1, row_count - 1))) /
                                                                    float(std::max(1, row_count))));

  for (int i = 0; i < row_count; ++i) {
    const auto& r = st.rows[size_t(i)];
    const bool sel = (i == st.selected);
    const float ry = cur_y + float(i) * (row_h + gap);
    if (sel) {
      ui4::panel(out, left_x, ry, left_x + left_w, ry + row_h, s, ui::rgba(52, 26, 44, 240), 0, ui4::kPink, true);
      out.rect(left_x, ry, left_x + 4.0f * sx, ry + row_h, ui4::kYellow);
    } else {
      ui4::panel(out, left_x, ry, left_x + left_w, ry + row_h, s, ui4::kCharcoal, 0,
                 r.completed ? kGold : ui::rgba(80, 86, 98, 160), true);
    }

    draw_medal_badge(out, left_x + 30.0f * sx, ry + row_h * 0.5f, 15.0f * s, r.completed, st.challenges_begun);

    const float tx = left_x + 58.0f * sx;
    draw_text(out, tx, ry + 8.0f * sy, 15.5f * s, clip_text(r.title, 44), r.completed ? kGold : ui4::kWhite,
              ui::Font::kE);
    draw_text(out, tx, ry + 30.0f * sy, 12.5f * s, clip_text(r.description, 68), sel ? ui4::kWhite : ui4::kDim,
              ui::Font::kB, true);

    const float bar_w = 152.0f * sx;
    const float bar_x = left_x + left_w - bar_w - 14.0f * sx;
    if (r.completed) {
      ui4::brush(out, bar_x + 18.0f * sx, ry + 14.0f * sy, bar_x + bar_w, ry + 38.0f * sy, ui::rgba(246, 198, 56, 228),
                 false);
      draw_text(out, bar_x + 85.0f * sx, ry + 20.0f * sy, 12.0f * s, "MEDAL EARNED", ui4::kInk, ui::Font::kE, false,
                ui::Align::kCenter);
    } else {
      const std::string status_txt = r.status.empty() ? (st.challenges_begun ? "ACTIVE" : "READY") : r.status;
      draw_text(out, bar_x, ry + 10.0f * sy, 12.0f * s, clip_text(status_txt, 22),
                st.challenges_begun ? kCyan : ui4::kDim);
      const float p = float(std::clamp(r.progress, 0.0, 1.0));
      const float by = ry + 32.0f * sy;
      out.rect(bar_x, by, bar_x + bar_w, by + 7.0f * sy, ui::rgba(255, 255, 255, 24));
      if (p > 0.001f) out.rect(bar_x, by, bar_x + bar_w * p, by + 7.0f * sy, ui4::kYellow);
    }
    if (sel && !r.hint.empty() && row_h >= 62.0f * sy) {
      draw_text(out, tx, ry + 48.0f * sy, 11.0f * s, "Hint: " + clip_text(r.hint, 64), ui::rgba(220, 228, 242, 230),
                ui::Font::kB, true);
    }
  }

  // Right sidebar: 1000 Club Medal Milestones & Achievements (SPA 216..225) in UI4 gritty panel.
  const float side_x = left_x + left_w + 16.0f * sx;
  const float side_w = (px + pw - 22.0f * sx) - side_x;
  ui4::panel(out, side_x, body_y, side_x + side_w, body_y + body_h, s, ui4::kCharcoal, kGold, 0, true);

  draw_text(out, side_x + 14.0f * sx, body_y + 12.0f * sy, 15.0f * s, "1000 CLUB MILESTONES", kGold, ui::Font::kE);
  ui4::offwhite_strip(out, side_x + 10.0f * sx, body_y + 36.0f * sy, side_x + side_w - 10.0f * sx, body_y + 58.0f * sy,
                      s, false);
  char ach_hdr[64];
  std::snprintf(ach_hdr, sizeof ach_hdr, "ACHIEVEMENTS: %d / 10", st.stats.achievements_unlocked);
  draw_text(out, side_x + 16.0f * sx, body_y + 41.0f * sy, 12.0f * s, ach_hdr, ui4::kInk);

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
  float my = body_y + 68.0f * sy;
  const float mstep = std::min(38.0f * sy, (body_h - 84.0f * sy) / 10.0f);
  for (const auto& m : lines) {
    char val[32];
    std::snprintf(val, sizeof val, "%d/%d", std::min(m.cur, m.target), m.target);
    draw_text(out, side_x + 14.0f * sx, my, 12.0f * s, m.label, m.done ? kGold : ui4::kDim);
    draw_text(out, side_x + side_w - 14.0f * sx, my, 12.0f * s, val, m.done ? ui4::kGreen : ui4::kWhite, ui::Font::kB,
              false, ui::Align::kRight);
    const float frac = std::clamp(float(m.cur) / float(std::max(1, m.target)), 0.0f, 1.0f);
    out.rect(side_x + 14.0f * sx, my + 16.0f * sy, side_x + side_w - 14.0f * sx, my + 20.0f * sy,
             ui::rgba(255, 255, 255, 22));
    if (frac > 0.0f) {
      out.rect(side_x + 14.0f * sx, my + 16.0f * sy, side_x + 14.0f * sx + (side_w - 28.0f * sx) * frac,
               my + 20.0f * sy, m.done ? ui4::kGreen : kGold);
    }
    my += mstep;
  }

  // Authentic BUTTON_BAR_SCENE bottom prompt bar with real controller glyphs.
  std::vector<ui4::Prompt> prompts;
  if (!st.challenges_begun) prompts.push_back({"A", "BEGIN CHALLENGES IN THIS CAR"});
  else prompts.push_back({"A", "CHALLENGES ACTIVE"});
  prompts.push_back({"DPAD", "INSPECT"});
  prompts.push_back({"B", "RETURN TO DRIVE"});
  ui4::button_bar(out, prompts, W, H, 48.0f);
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

  const float bw = 520.0f * sx;
  const float bh = 102.0f * sy;
  const float bx = c.dock_bottom_right ? (W - bw - 68.0f * sx) : (W - bw) * 0.5f;
  const float by = c.dock_bottom_right ? (H - bh - 56.0f * sy) : 18.0f * sy;

  ui4::panel(out, bx, by, bx + bw, by + bh, s, ui::fade(ui4::kCharcoalDark, alpha), ui::fade(kGold, alpha),
             ui::fade(ui4::kPink, alpha), true);

  draw_medal_badge(out, bx + 48.0f * sx, by + bh * 0.5f, 27.0f * s, true, true);

  draw_text(out, bx + 92.0f * sx, by + 11.0f * sy, 13.0f * s, "1000 CLUB MEDAL EARNED!", ui::fade(kGold, alpha),
            ui::Font::kE);
  draw_text(out, bx + 92.0f * sx, by + 31.0f * sy, 19.5f * s, clip_text(c.challenge_title, 34),
            ui::fade(ui4::kWhite, alpha), ui::Font::kE);
  if (!c.challenge_desc.empty())
    draw_text(out, bx + 92.0f * sx, by + 56.0f * sy, 12.5f * s, clip_text(c.challenge_desc, 54),
              ui::fade(ui4::kDim, alpha), ui::Font::kB, true);

  char foot[128];
  std::snprintf(foot, sizeof foot, "%d / %d IN THIS CAR   |   TOTAL: %d MEDALS (%d CARS)", c.car_medals,
                std::max(1, c.car_total), c.total_medals, c.cars_with_medal);
  draw_text(out, bx + 92.0f * sx, by + 78.0f * sy, 11.5f * s, foot, ui::fade(kCyan, alpha));
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

  ui4::panel(out, bx, by, bx + bw, by + bh, s, ui4::kCharcoalDark, 0, kGold, true);
  draw_text(out, bx + 14.0f * sx, by + 7.0f * sy, 12.5f * s, "1000 CLUB  " + clip_text(strip.title, 28), kGold,
            ui::Font::kE);
  draw_text(out, bx + 14.0f * sx, by + 25.0f * sy, 11.5f * s, clip_text(strip.status, 40), ui4::kWhite, ui::Font::kB,
            true);
  const float p = float(std::clamp(strip.progress, 0.0, 1.0));
  if (p > 0.001f) {
    out.rect(bx + 4.0f * sx, by + bh - 4.0f * sy, bx + 4.0f * sx + (bw - 4.0f * sx) * p, by + bh, ui4::kYellow);
  }
}

}  // namespace carview
