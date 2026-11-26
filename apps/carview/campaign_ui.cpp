#include "campaign_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

#include "fh1data/stringtables.h"
#include "ui4_chrome.h"

namespace carview::campaign_ui {
namespace {

std::string loc_str(std::string_view table, std::string_view id, const char* fallback) {
  const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
  const std::string ref = "_&" + std::to_string(packed);
  const std::string resolved = ui::lookup_text(ref);
  if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) return resolved;
  return fallback;
}

bool is_bright_accent(uint32_t argb) {
  const uint32_t r = (argb >> 16) & 0xFFu;
  const uint32_t g = (argb >> 8) & 0xFFu;
  const uint32_t b = argb & 0xFFu;
  // Relative luminance heuristic for choosing dark ink vs crisp white on brush backgrounds.
  return (r * 299u + g * 587u + b * 114u) > 168000u;
}

uint32_t with_alpha(uint32_t col, float alpha) {
  const uint32_t base_a = (col >> 24) & 0xFFu;
  const uint32_t a = uint32_t(std::clamp(int(std::lround(float(base_a) * std::clamp(alpha, 0.0f, 1.0f))), 0, 255));
  return (col & 0x00FFFFFFu) | (a << 24);
}

std::string wrap_lines(std::string_view text, const ui::TextStyle& ts, float max_w) {
  std::string out;
  std::string cur;
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && text[pos] == ' ') ++pos;
    size_t end = text.find(' ', pos);
    if (end == std::string_view::npos) end = text.size();
    const std::string word(text.substr(pos, end - pos));
    pos = end;
    if (word.empty()) continue;
    const std::string cand = cur.empty() ? word : (cur + " " + word);
    if (!cur.empty() && ui::List::measure(cand, ts) > max_w) {
      if (!out.empty()) out.push_back('\n');
      out += cur;
      cur = word;
    } else {
      cur = cand;
    }
  }
  if (!cur.empty()) {
    if (!out.empty()) out.push_back('\n');
    out += cur;
  }
  return out;
}

std::string upper_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string format_dist(double m, bool metric) {
  char buf[48];
  if (metric) {
    if (m >= 1000.0) std::snprintf(buf, sizeof buf, "%.1f KM", m / 1000.0);
    else std::snprintf(buf, sizeof buf, "%d M", std::max(0, int(std::lround(m))));
  } else {
    const double mi = m / 1609.344;
    if (mi >= 0.1) std::snprintf(buf, sizeof buf, "%.1f MI", mi);
    else std::snprintf(buf, sizeof buf, "%d FT", std::max(0, int(std::lround(m * 3.28084))));
  }
  return buf;
}

const char* ordinal_suffix(int place) {
  const int m100 = place % 100;
  if (m100 >= 11 && m100 <= 13) return "TH";
  switch (place % 10) {
    case 1: return "ST";
    case 2: return "ND";
    case 3: return "RD";
    default: return "TH";
  }
}

std::string format_gap_s(double gap) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "+%.2fs", std::max(0.0, gap));
  return buf;
}

std::string format_clock_s(double sec) {
  if (sec < 0.0) return "--:--.--";
  const int64_t ms = int64_t(std::llround(sec * 1000.0));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02lld:%02lld.%02lld", (long long)(ms / 60000), (long long)((ms / 1000) % 60),
                (long long)((ms % 1000) / 10));
  return buf;
}

void draw_post_race_stage_pills(ui::List& l, float screen_w, float s, int active_stage_idx) {
  static constexpr const char* kStages[3] = {"1  RACE RESULTS", "2  CREDITS EARNED", "3  WRISTBAND & REWARDS"};
  const float py = 36.0f * s;
  const float ph = 26.0f * s;
  const float gap = 8.0f * s;
  float total_w = 0.0f;
  float widths[3]{};
  for (int i = 0; i < 3; ++i) {
    widths[i] = std::max(126.0f * s, ui::List::measure(kStages[i], ui4::style(ui::Font::kE, 11.5f * s, ui4::kWhite)) + 24.0f * s);
    total_w += widths[i];
  }
  total_w += gap * 2.0f;
  float x = screen_w - 58.0f * s - total_w;
  ui4::TiltScope tilt(l, x + 0.5f * total_w, py + 0.5f * ph, ui4::kTiltRad * 0.45f);
  for (int i = 0; i < 3; ++i) {
    const bool act = (i == active_stage_idx);
    const bool done = (i < active_stage_idx);
    if (act) {
      ui4::brush(l, x - 3.0f * s, py - 2.0f * s, x + widths[i] + 3.0f * s, py + ph + 2.0f * s, ui4::kPink, false);
    } else {
      ui4::pill(l, x, py, x + widths[i], py + ph, done ? ui4::kRowAlt : ui4::kBrushBlack, ui4::PillKind::kHud01);
      if (done) l.rect(x, py + ph - 2.5f * s, x + widths[i], py + ph, ui4::kYellow);
    }
    l.text(x + 0.5f * widths[i], py + 6.5f * s, kStages[i],
           ui4::style(ui::Font::kE, 11.5f * s, act ? ui4::kWhite : (done ? ui4::kYellow : ui4::kDim),
                      ui::Align::kCenter));
    x += widths[i] + gap;
  }
}

void draw_post_race_results_stage(ui::List& l, const race::RewardTallyState& tally, float screen_w, float screen_h,
                                  double anim_t) {
  const float s = screen_h / 720.0f;
  const float enter = float(race::ease_out_cubic(std::clamp(anim_t / 0.42, 0.0, 1.0)));
  ui4::subtle_vignette(l, screen_w, screen_h, 0.34f * enter);
  draw_post_race_stage_pills(l, screen_w, s, 0);

  const std::string clean_title = race::resolve_event_display_title(tally.event_title);
  const std::string category = tally.event_category.empty() ? std::string("FESTIVAL RACE") : tally.event_category;
  const int pl = std::max(1, tally.place);
  const std::string place_str = std::to_string(pl) + ordinal_suffix(pl) + " PLACE";

  const float lx0 = 58.0f * s + (1.0f - enter) * (-72.0f * s);
  const float ly0 = 62.0f * s;
  const float table_w = std::min(740.0f * s, screen_w - 136.0f * s);

  {
    ui4::TiltScope tilt(l, lx0 + 0.5f * table_w, ly0 + 220.0f * s, ui4::kTiltRad * 0.50f);

    // Iconic FH1 FINISHED black eyebrow + hot-pink 1ST PLACE banner + localized event subtitle.
    const std::string eye = "FINISHED  •  " + category + "  •  " + clean_title;
    ui::TextStyle eye_ts = ui4::style(ui::Font::kB, 14.0f * s, ui4::kYellow);
    ui4::fit_text(eye, eye_ts, table_w - 28.0f * s, 10.0f * s);
    const float ew = std::clamp(ui::List::measure(eye, eye_ts) + 34.0f * s, 280.0f * s, table_w);
    ui4::brush(l, lx0 - 6.0f * s, ly0, lx0 + ew, ly0 + 28.0f * s, ui4::kBrushBlack, false);
    l.text(lx0 + 14.0f * s, ly0 + 7.0f * s, eye, eye_ts);

    ui::TextStyle pl_ts = ui4::style(ui::Font::kE, 46.0f * s, ui4::kWhite);
    const float pw = std::clamp(ui::List::measure(place_str, pl_ts) + 54.0f * s, 320.0f * s, 540.0f * s);
    ui4::brush(l, lx0 - 12.0f * s, ly0 + 30.0f * s, lx0 + pw, ly0 + 92.0f * s, ui4::kPink, true, enter);
    l.text(lx0 + 16.0f * s, ly0 + 39.0f * s, place_str, pl_ts);

    // RACE RESULTS podium & standings leaderboard table.
    const float ty0 = ly0 + 108.0f * s;
    const size_t row_count = std::min<size_t>(tally.results_rows.size(), 8);
    const float row_h = 34.0f * s;
    const float header_h = 30.0f * s;
    const float panel_h = header_h + float(std::max<size_t>(row_count, 1)) * row_h + 22.0f * s;
    ui4::panel(l, lx0, ty0, lx0 + table_w, ty0 + panel_h, s, ui4::kCharcoalDark, ui4::kPink, 0, true);

    // Off-white FH1 column header strip.
    ui4::offwhite_strip(l, lx0 + 10.0f * s, ty0 + 8.0f * s, lx0 + table_w - 10.0f * s, ty0 + 8.0f * s + header_h, s,
                        false);
    const float col_pos = lx0 + 24.0f * s;
    const float col_drv = lx0 + 84.0f * s;
    const float col_car = lx0 + 310.0f * s;
    const float col_time = lx0 + table_w - 22.0f * s;
    const auto hdr_ts = ui4::style(ui::Font::kE, 12.5f * s, ui4::kInk);
    l.text(col_pos, ty0 + 15.5f * s, "POS", hdr_ts);
    l.text(col_drv, ty0 + 15.5f * s, "DRIVER", hdr_ts);
    l.text(col_car, ty0 + 15.5f * s, "CAR / CLASS", hdr_ts);
    l.text(col_time, ty0 + 15.5f * s, "TIME / GAP", ui4::style(ui::Font::kE, 12.5f * s, ui4::kInk, ui::Align::kRight));

    float ry = ty0 + header_h + 14.0f * s;
    if (tally.results_rows.empty()) {
      l.rect(lx0 + 10.0f * s, ry, lx0 + table_w - 10.0f * s, ry + row_h - 4.0f * s, ui4::kRowHighlight);
      l.rect(lx0 + 10.0f * s, ry, lx0 + 15.0f * s, ry + row_h - 4.0f * s, ui4::kPink);
      l.text(col_pos, ry + 7.0f * s, std::to_string(pl), ui4::style(ui::Font::kE, 15.0f * s, ui4::kYellow));
      l.text(col_drv, ry + 7.0f * s, "YOU", ui4::style(ui::Font::kE, 14.5f * s, ui4::kWhite));
      l.text(col_car, ry + 7.5f * s, clean_title, ui4::style(ui::Font::kB, 13.0f * s, ui4::kOffWhite));
      l.text(col_time, ry + 7.0f * s, "FINISHED", ui4::style(ui::Font::kE, 13.5f * s, ui4::kYellow, ui::Align::kRight));
    } else {
      for (size_t i = 0; i < row_count; ++i) {
        const auto& row = tally.results_rows[i];
        const float r_rev = float(race::ease_out_cubic(std::clamp((anim_t - 0.10 - double(i) * 0.06) / 0.25, 0.0, 1.0)));
        const float slide = (1.0f - r_rev) * 20.0f * s;
        const uint32_t row_bg = row.player ? ui4::kRowHighlight : ((i % 2 == 1) ? ui4::kRowAlt : ui::rgba(22, 24, 30, 185));
        l.rect(lx0 + 10.0f * s + slide, ry, lx0 + table_w - 10.0f * s + slide, ry + row_h - 4.0f * s,
               with_alpha(row_bg, r_rev));
        if (row.player) {
          l.rect(lx0 + 10.0f * s + slide, ry, lx0 + 15.0f * s + slide, ry + row_h - 4.0f * s,
                 with_alpha(ui4::kPink, r_rev));
        }

        const uint32_t pos_col = row.place == 1 ? ui4::kYellow : (row.player ? ui4::kPinkHi : ui4::kWhite);
        l.text(col_pos + slide, ry + 6.5f * s, std::to_string(row.place),
               ui4::style(ui::Font::kE, 15.5f * s, with_alpha(pos_col, r_rev)));

        ui::TextStyle dts = ui4::style(row.player ? ui::Font::kE : ui::Font::kB, 14.0f * s,
                                       with_alpha(row.player ? ui4::kWhite : ui4::kOffWhite, r_rev));
        ui4::fit_text(row.driver, dts, (col_car - col_drv) - 14.0f * s, 10.0f * s);
        l.text(col_drv + slide, ry + 7.5f * s, row.driver, dts);

        float cx = col_car + slide;
        if (!row.car_class.empty() && row.pi > 0) {
          ui4::class_badge(l, cx, ry + 5.0f * s, 20.0f * s, row.car_class, row.pi);
          cx += 62.0f * s;
        }
        ui::TextStyle cts = ui4::style(ui::Font::kB, 12.5f * s, with_alpha(row.player ? ui4::kWhite : ui4::kDim, r_rev),
                                       ui::Align::kLeft, true);
        ui4::fit_text(row.car, cts, (col_time - cx) - 108.0f * s, 9.5f * s);
        l.text(cx, ry + 8.0f * s, row.car, cts);

        const std::string t_str = row.time_s > 0.0
                                      ? format_clock_s(row.time_s)
                                      : (row.place == 1 ? std::string("WINNER") : format_gap_s(row.gap_s));
        l.text(col_time + slide, ry + 7.5f * s, t_str,
               ui4::style(ui::Font::kE, 13.5f * s,
                          with_alpha(row.place == 1 ? ui4::kYellow : ui4::kOffWhite, r_rev), ui::Align::kRight));
        ry += row_h;
      }
    }
  }

  ui4::button_bar(l, {{"A", loc_str("HelpButtons", "IDS_Continue", "CONTINUE")}}, screen_w, screen_h, 70.0f);
}

}  // namespace

void draw_objective_tracker(ui::List& l, const race::CampaignFlow& flow, const DrawContext& ctx) {
  if (flow.stage() != race::CampaignStage::kViperIntroDrive && flow.stage() != race::CampaignStage::kFreeRoam) return;
  const auto obj = flow.next_objective(ctx.wristband_points, ctx.collected_wristband, ctx.popularity_rank, ctx.catalog);
  if (!obj.valid) return;
  // In free roam, show the compact tracker when an urgent objective or active dialog is on screen.
  if (flow.stage() == race::CampaignStage::kFreeRoam && !obj.is_urgent && !flow.dialogs().active()) return;

  const float s = ctx.screen_h / 720.0f;
  const float alpha = flow.hud_reveal_alpha();
  if (alpha < 0.02f) return;

  std::string sub = obj.detail;
  if (flow.stage() == race::CampaignStage::kViperIntroDrive) {
    sub = "HORIZON FESTIVAL  •  " + format_dist(flow.distance_to_festival(), ctx.metric_units) + " REMAINING";
  }

  ui::TextStyle title_ts = ui4::style(ui::Font::kE, 18.0f * s, with_alpha(ui4::kWhite, alpha));
  ui::TextStyle sub_ts = ui4::style(ui::Font::kB, 13.0f * s, with_alpha(ui4::kWhite, alpha));
  const float slide_x = (1.0f - alpha) * (-80.0f * s);
  const float x0 = 64.0f * s + slide_x;
  const float y0 = 36.0f * s;
  const float tw = std::clamp(ui::List::measure(obj.title, title_ts) + 42.0f * s, 280.0f * s, 540.0f * s);
  const float sw = std::clamp(ui::List::measure(sub, sub_ts) + 36.0f * s, 260.0f * s, 560.0f * s);

  {
    ui4::TiltScope tilt(l, x0 + 0.5f * tw, y0 + 26.0f * s, ui4::kTiltRad * 0.55f);
    ui4::brush(l, x0 - 6.0f * s, y0, x0 + tw, y0 + 32.0f * s, with_alpha(ui4::kPink, alpha), false);
    l.text(x0 + 14.0f * s, y0 + 7.0f * s, obj.title, title_ts);
    if (!sub.empty()) {
      ui4::pill(l, x0, y0 + 34.0f * s, x0 + sw, y0 + 60.0f * s, with_alpha(ui4::kCharcoalDark, alpha),
                ui4::PillKind::kHud01);
      l.rect(x0, y0 + 34.0f * s, x0 + 4.0f * s, y0 + 60.0f * s, with_alpha(ui4::kYellow, alpha));
      l.text(x0 + 14.0f * s, y0 + 40.0f * s, sub, sub_ts);
    }
  }

  if (flow.stage() == race::CampaignStage::kViperIntroDrive) {
    const float sx1 = ctx.screen_w - 64.0f * s;
    const std::string skip_lbl =
        "[" + ui4::glyph_label_for("BACK") + "] " + std::string("SKIP INTRO STAGE");
    ui4::pill(l, sx1 - 220.0f * s, y0, sx1, y0 + 28.0f * s, with_alpha(ui4::kCharcoalDark, alpha),
              ui4::PillKind::kHud01);
    l.text(sx1 - 12.0f * s, y0 + 7.0f * s, skip_lbl,
           ui4::style(ui::Font::kB, 12.0f * s, with_alpha(ui4::kDim, alpha), ui::Align::kRight));
  }
}

void draw_dialog_and_subtitles(ui::List& l, const race::DialogQueue& dialogs, const DrawContext& ctx) {
  if (!dialogs.active()) return;
  const float slide = dialogs.banner_slide();
  if (slide <= 0.01f) return;

  const auto& cue = dialogs.current();
  const auto& prof = race::character_profile(cue.speaker);
  const std::string speaker = cue.speaker_override.empty() ? std::string(prof.display_name) : cue.speaker_override;
  const std::string role = cue.role_override.empty() ? std::string(prof.role_tag) : cue.role_override;
  const uint32_t accent = prof.accent_rgba;
  const float s = ctx.screen_h / 720.0f;

  // 1. Top-Right Non-Blocking FH1 Character Call-In Banner (speaker + role + live animated audio waveform).
  if (!ctx.suppress_callin) {
    const float bw = 348.0f * s;
    const float bh = 74.0f * s;
    const float slide_offset = (1.0f - slide) * (bw + 48.0f * s);
    const float bx1 = ctx.screen_w - 48.0f * s + slide_offset;
    const float bx0 = bx1 - bw;
    const float by0 = 76.0f * s;

    ui4::TiltScope tilt(l, bx0 + 0.5f * bw, by0 + 0.5f * bh, ui4::kTiltRad * 0.45f);
    // Distressed dark charcoal body + speaker accent edge.
    ui4::pill(l, bx0, by0, bx1, by0 + bh, with_alpha(ui4::kCharcoalDark, slide), ui4::PillKind::kHud01);
    l.rect(bx0, by0, bx0 + 5.0f * s, by0 + bh, with_alpha(accent, slide));
    // Speaker name on FH1 brush header strip.
    ui::TextStyle name_ts = ui4::style(ui::Font::kE, 15.5f * s, with_alpha(ui4::kWhite, slide));
    const float nw = std::clamp(ui::List::measure(speaker, name_ts) + 30.0f * s, 160.0f * s, bw - 92.0f * s);
    ui4::brush(l, bx0 + 10.0f * s, by0 - 12.0f * s, bx0 + 10.0f * s + nw, by0 + 16.0f * s,
               with_alpha(accent, slide), false);
    // High-contrast ink on bright yellow/gold accents, white on pink/blue/purple accents.
    name_ts.color = is_bright_accent(accent) ? with_alpha(ui4::kInk, slide) : with_alpha(ui4::kWhite, slide);
    l.text(bx0 + 22.0f * s, by0 - 6.0f * s, speaker, name_ts);

    // Role tag + live signal indicator.
    ui::TextStyle role_ts = ui4::style(ui::Font::kB, 11.5f * s, with_alpha(ui4::kDim, slide));
    l.text(bx0 + 16.0f * s, by0 + 24.0f * s, role, role_ts);
    const std::string radio_line =
        ctx.skip_vo_with_a
            ? ("HORIZON RADIO LINK  •  [" + ui4::glyph_label_for("A") + "] " + loc_str("HelpButtons", "IDS_Skip", "SKIP"))
            : "HORIZON RADIO LINK  •  LIVE";
    l.text(bx0 + 16.0f * s, by0 + 44.0f * s, radio_line,
           ui4::style(ui::Font::kB, 10.5f * s, with_alpha(ui4::kYellow, slide * 0.9f)));

    // Animated 10-bar audio waveform spectrum on right side of call-in banner.
    const auto bars = dialogs.waveform_bars(10);
    const float wx0 = bx1 - 86.0f * s;
    const float wy_base = by0 + bh - 14.0f * s;
    const float max_bh = 36.0f * s;
    const float bar_w = 4.5f * s;
    const float bar_gap = 2.5f * s;
    for (size_t i = 0; i < bars.size(); ++i) {
      const float h_i = std::max(4.0f * s, bars[i] * max_bh);
      const float x_i = wx0 + float(i) * (bar_w + bar_gap);
      const uint32_t bcol = (i % 3 == 0) ? with_alpha(ui4::kYellow, slide) : with_alpha(accent, slide);
      l.rect(x_i, wy_base - h_i, x_i + bar_w, wy_base, bcol);
    }
  }

  // 2. Crisp Bottom-Center Localized Subtitle Strip (cleanly clear of minimap & tach;
  //    suppressed during full-card ceremonies so it never duplicates ceremony card text).
  if (!ctx.suppress_subtitle && !cue.subtitle.empty()) {
    const float max_box_w = std::min(ctx.screen_w - 460.0f * s, 720.0f * s);
    ui::TextStyle sub_ts = ui4::style(ui::Font::kA, 15.5f * s, with_alpha(ui4::kWhite, slide), ui::Align::kLeft, true);
    const std::string wrapped = wrap_lines(cue.subtitle, sub_ts, std::max(240.0f * s, max_box_w - 36.0f * s));
    const float th = ui::List::measure_height(wrapped, sub_ts);
    const float box_h = std::max(46.0f * s, th + 20.0f * s);
    const float cx0 = 0.5f * (ctx.screen_w - max_box_w);
    const float cy0 = ctx.screen_h - 62.0f * s - box_h + (1.0f - slide) * 20.0f * s;

    // Speaker tag pill perched on top-left of subtitle bar.
    ui::TextStyle sp_ts = ui4::style(ui::Font::kE, 12.5f * s,
                                     is_bright_accent(accent) ? with_alpha(ui4::kInk, slide)
                                                              : with_alpha(ui4::kWhite, slide));
    const float spw = ui::List::measure(speaker, sp_ts) + 26.0f * s;
    ui4::brush(l, cx0 - 4.0f * s, cy0 - 22.0f * s, cx0 + spw, cy0 + 2.0f * s, with_alpha(accent, slide), false);
    l.text(cx0 + 10.0f * s, cy0 - 17.0f * s, speaker, sp_ts);

    ui4::pill(l, cx0, cy0, cx0 + max_box_w, cy0 + box_h, with_alpha(ui4::kCharcoalDark, slide * 0.96f),
              ui4::PillKind::kHud01);
    l.rect(cx0, cy0, cx0 + 4.5f * s, cy0 + box_h, with_alpha(accent, slide));
    l.text(cx0 + 18.0f * s, cy0 + 10.0f * s, wrapped, sub_ts);
  }
}

void draw_cinematic_overlay(ui::List& l, const race::CampaignFlow& flow, const DrawContext& ctx) {
  const auto st = flow.stage();
  if (st == race::CampaignStage::kCredits) {
    draw_credits_roll(l, flow, ctx.screen_w, ctx.screen_h);
    return;
  }
  const auto& cin = flow.cinematic();
  if (cin.kind == race::CutsceneKind::kNone || st == race::CampaignStage::kOpeningCutscene) return;

  const float w = ctx.screen_w;
  const float h = ctx.screen_h;
  const float s = h / 720.0f;
  const float enter = float(race::ease_out_cubic(std::clamp(flow.stage_time() / 0.55, 0.0, 1.0)));

  // Smooth cinema letterbox bars.
  const float bar_h = 54.0f * s * cin.letterbox_amount * enter;
  if (bar_h > 1.0f) {
    l.rect(0, 0, w, bar_h, ui::rgba(8, 8, 10, 242));
    l.rect(0, h - bar_h, w, h, ui::rgba(8, 8, 10, 242));
  }

  // Wristband Ceremony / Festival Registration Hero Presentation.
  if (st == race::CampaignStage::kWristbandCeremony || st == race::CampaignStage::kFestivalRegistration) {
    const bool is_registration = (st == race::CampaignStage::kFestivalRegistration);
    ui4::subtle_vignette(l, w, h, 0.35f * enter);
    const float cx = 0.5f * w;
    const float card_w = 740.0f * s;
    const float card_x0 = cx - 0.5f * card_w;
    const float card_y0 = (is_registration ? 86.0f : 112.0f) * s + (1.0f - enter) * 24.0f * s;

    {
      ui4::TiltScope tilt(l, cx, card_y0 + 160.0f * s, ui4::kTiltRad * 0.55f);
      // Eyebrow strip.
      ui::TextStyle eye_ts = ui4::style(ui::Font::kB, 14.0f * s, ui4::kWhite, ui::Align::kCenter);
      const float ew = std::clamp(ui::List::measure(cin.eyebrow, eye_ts) + 40.0f * s, 260.0f * s, card_w);
      ui4::brush(l, cx - 0.5f * ew, card_y0, cx + 0.5f * ew, card_y0 + 28.0f * s, ui4::kBrushBlack, false);
      l.text(cx, card_y0 + 7.0f * s, cin.eyebrow, eye_ts);

      // Headline on wristband colour brush with animated wipe.
      ui::TextStyle head_ts = ui4::style(ui::Font::kE, 36.0f * s, ui4::kWhite, ui::Align::kCenter);
      const bool bright_band = upper_ascii(cin.colour_key) == "YELLOW" || upper_ascii(cin.colour_key) == "GOLD";
      if (bright_band) head_ts.color = ui4::kInk;
      const float hw = std::clamp(ui::List::measure(cin.headline, head_ts) + 74.0f * s, 380.0f * s, card_w);
      ui4::brush(l, cx - 0.5f * hw, card_y0 + 32.0f * s, cx + 0.5f * hw, card_y0 + 88.0f * s, cin.accent_rgba, true,
                 enter);
      l.text(cx, card_y0 + 44.0f * s, cin.headline, head_ts);

      // Center distressed wristband showcase card with real WRISTBANDS/<COLOR>XL artwork.
      const float py0 = card_y0 + 102.0f * s;
      const float py1 = py0 + 190.0f * s;
      ui4::panel(l, card_x0, py0, card_x0 + card_w, py1, s, ui4::kCharcoalDark, cin.accent_rgba, 0, true);

      std::array<float, 4> wb_uv{};
      const std::string ckey = upper_ascii(cin.colour_key);
      const bool has_wb = !ckey.empty() &&
                          (ui4::texture("WRISTBANDS/" + ckey + "XL", wb_uv) || ui4::texture("WRISTBANDS/" + ckey, wb_uv));
      const float badge_x0 = card_x0 + 26.0f * s;
      const float badge_y0 = py0 + 24.0f * s;
      if (has_wb) {
        l.image(badge_x0, badge_y0, badge_x0 + 190.0f * s, badge_y0 + 132.0f * s, wb_uv);
      } else {
        // Stylized wristband emblem fallback when UI4 texture pack is absent in minimal fixtures.
        l.rect(badge_x0, badge_y0 + 28.0f * s, badge_x0 + 180.0f * s, badge_y0 + 92.0f * s, cin.accent_rgba);
        l.rect(badge_x0 + 12.0f * s, badge_y0 + 42.0f * s, badge_x0 + 168.0f * s, badge_y0 + 78.0f * s,
               ui4::kBrushBlack);
        l.text(badge_x0 + 90.0f * s, badge_y0 + 50.0f * s, ckey.empty() ? "HORIZON" : ckey,
               ui4::style(ui::Font::kE, 20.0f * s, cin.accent_rgba, ui::Align::kCenter));
      }

      const float tx = badge_x0 + 216.0f * s;
      const float max_tw = (card_x0 + card_w) - tx - 24.0f * s;
      // Tier pill sized cleanly to label text.
      ui::TextStyle badge_ts = ui4::style(ui::Font::kE, 14.0f * s, ui4::kInk);
      ui4::fit_text(cin.badge_text, badge_ts, max_tw - 24.0f * s, 10.5f * s);
      const float badge_w = std::min(max_tw, std::max(180.0f * s, ui::List::measure(cin.badge_text, badge_ts) + 30.0f * s));
      ui4::offwhite_strip(l, tx, py0 + 22.0f * s, tx + badge_w, py0 + 48.0f * s, s, false);
      l.text(tx + 12.0f * s, py0 + 28.0f * s, cin.badge_text, badge_ts);

      // Subheadline & unlock description.
      ui::TextStyle sh_ts = ui4::style(ui::Font::kE, 16.0f * s, ui4::kYellow);
      ui4::fit_text(cin.subheadline, sh_ts, max_tw, 11.5f * s);
      l.text(tx, py0 + 58.0f * s, cin.subheadline, sh_ts);
      if (!cin.detail_line.empty()) {
        ui::TextStyle det_ts = ui4::style(ui::Font::kA, 13.5f * s, ui4::kWhite, ui::Align::kLeft, true);
        l.text(tx, py0 + 84.0f * s, wrap_lines(cin.detail_line, det_ts, max_tw), det_ts);
      }
      if (!cin.reward_car.empty()) {
        const std::string rtext = "STARTER GARAGE RIDE: " + upper_ascii(cin.reward_car);
        ui::TextStyle r_ts = ui4::style(ui::Font::kE, 13.0f * s, ui4::kWhite);
        ui4::fit_text(rtext, r_ts, max_tw - 24.0f * s, 10.0f * s);
        ui4::pill(l, tx, py1 - 44.0f * s, card_x0 + card_w - 24.0f * s, py1 - 14.0f * s, ui4::kPink,
                  ui4::PillKind::kHud01);
        l.text(tx + 12.0f * s, py1 - 36.0f * s, rtext, r_ts);
      }

      // Dedicated First Festival Car / Welcome to the Autoshow onboarding guidance card (PT-41).
      if (is_registration) {
        const float gy0 = py1 + 14.0f * s;
        const float gy1 = gy0 + 106.0f * s;
        ui4::panel(l, card_x0, gy0, card_x0 + card_w, gy1, s, ui4::kBrushBlack, 0, ui4::kPink, true);
        l.text(card_x0 + 22.0f * s, gy0 + 12.0f * s, "WELCOME TO THE AUTOSHOW  •  ALICE HART & DAK STEWART",
               ui4::style(ui::Font::kE, 14.5f * s, ui4::kPinkHi));
        const std::string guide =
            "Your entry Yellow Wristband is official and your 1995 VW Corrado VR6 is parked in your garage alongside a 15,000 Cr starter bonus. Next up: browse curated affordable starter cars in the Horizon Autoshow, or press B there anytime to hit Colorado Free Roam and chase your Green Wristband!";
        ui::TextStyle gts = ui4::style(ui::Font::kA, 13.2f * s, ui4::kOffWhite, ui::Align::kLeft, true);
        l.text(card_x0 + 22.0f * s, gy0 + 36.0f * s, wrap_lines(guide, gts, card_w - 44.0f * s), gts);
      }
    }
    if (is_registration) {
      ui4::button_bar(l,
                      {{"A", std::string("OPEN STARTER AUTOSHOW")},
                       {"X", std::string("SET GPS TO FIRST EVENT")}},
                      w, h, 70.0f);
    } else {
      ui4::button_bar(l,
                      {{"A", loc_str("HelpButtons", "IDS_Continue", "CONTINUE")},
                       {"X", std::string("SET GPS TO NEXT EVENT")}},
                      w, h, 70.0f);
    }
    return;
  }

  // Stage 1 Post-Race Finish: Full FH1 RACE RESULTS podium/leaderboard screen (PT-42).
  if (st == race::CampaignStage::kPostRaceFinish) {
    draw_post_race_results_stage(l, flow.reward_tally(), w, h, flow.stage_time());
    return;
  }

  // Boss Rival / Star Showdown & Darius Flynt Finale Hero Dossier Card.
  // Anchored tightly to the far-left margin (460px wide) so the rival hero car is framed
  // cleanly unobstructed on the right half of the screen.
  if (st == race::CampaignStage::kBossIntro || st == race::CampaignStage::kBossVictory ||
      st == race::CampaignStage::kFinaleIntro || st == race::CampaignStage::kFinaleVictory) {
    ui4::subtle_vignette(l, w, h, 0.24f * enter);
    const float x0 = 40.0f * s + (1.0f - enter) * (-90.0f * s);
    const float y0 = 96.0f * s;
    const float card_w = 460.0f * s;

    {
      ui4::TiltScope tilt(l, x0 + 0.5f * card_w, y0 + 120.0f * s, ui4::kTiltRad * 0.55f);
      // Eyebrow on black brush.
      ui::TextStyle eye_ts = ui4::style(ui::Font::kB, 13.5f * s, ui4::kYellow);
      const float ew = std::clamp(ui::List::measure(cin.eyebrow, eye_ts) + 34.0f * s, 260.0f * s, card_w);
      ui4::brush(l, x0 - 6.0f * s, y0, x0 + ew, y0 + 28.0f * s, ui4::kBrushBlack, false);
      l.text(x0 + 12.0f * s, y0 + 7.0f * s, cin.eyebrow, eye_ts);

      // Giant Boss / Champion Name on accent brush.
      ui::TextStyle name_ts = ui4::style(ui::Font::kE, 36.0f * s,
                                         is_bright_accent(cin.accent_rgba) ? ui4::kInk : ui4::kWhite);
      ui4::fit_text(cin.headline, name_ts, card_w - 28.0f * s, 20.0f * s);
      const float nw = std::clamp(ui::List::measure(cin.headline, name_ts) + 52.0f * s, 300.0f * s, card_w + 24.0f * s);
      ui4::brush(l, x0 - 10.0f * s, y0 + 30.0f * s, x0 + nw, y0 + 88.0f * s, cin.accent_rgba, true, enter);
      l.text(x0 + 14.0f * s, y0 + 42.0f * s, cin.headline, name_ts);

      // Distressed rival dossier card (vehicle, stakes, wristband tier) with strict fit_text bounds.
      const float py0 = y0 + 98.0f * s;
      const float py1 = py0 + 152.0f * s;
      ui4::panel(l, x0, py0, x0 + card_w, py1, s, ui4::kCharcoalDark, 0, cin.accent_rgba, true);
      ui::TextStyle sub_ts = ui4::style(ui::Font::kE, 15.0f * s, ui4::kWhite);
      ui4::fit_text(cin.subheadline, sub_ts, card_w - 36.0f * s, 11.5f * s);
      l.text(x0 + 18.0f * s, py0 + 16.0f * s, cin.subheadline, sub_ts);

      // Stakes strip (off-white FH1 paper tape).
      ui4::offwhite_strip(l, x0 + 18.0f * s, py0 + 48.0f * s, x0 + card_w - 18.0f * s, py0 + 80.0f * s, s, false);
      const std::string stakes =
          (st == race::CampaignStage::kFinaleVictory || st == race::CampaignStage::kBossVictory)
              ? ("PRIZE CLAIMED: " + upper_ascii(cin.reward_car))
              : (st == race::CampaignStage::kFinaleIntro
                     ? "STAKES: HORIZON CROWN  •  50,000 CR + FERRARI 599XX"
                     : "STAKES: 1-ON-1 PINK SLIP  •  RIVAL CAR KEYS");
      ui::TextStyle st_ts = ui4::style(ui::Font::kE, 12.5f * s, ui4::kInk);
      ui4::fit_text(stakes, st_ts, card_w - 64.0f * s, 8.5f * s);
      l.text(x0 + 28.0f * s, py0 + 57.0f * s, stakes, st_ts);

      if (!cin.detail_line.empty()) {
        ui::TextStyle dt = ui4::style(ui::Font::kA, 13.0f * s, ui4::kDim, ui::Align::kLeft, true);
        l.text(x0 + 18.0f * s, py0 + 90.0f * s, wrap_lines(cin.detail_line, dt, card_w - 36.0f * s), dt);
      }
    }

    // Badge chip positioned cleanly below the top-right call-in banner (zero overlap with call-in or subtitles).
    const float rx1 = w - 48.0f * s;
    const float ry0 = 164.0f * s;
    ui::TextStyle chip_ts = ui4::style(ui::Font::kE, 13.5f * s, ui4::kYellow, ui::Align::kRight);
    const float chip_w = std::max(210.0f * s, ui::List::measure(cin.badge_text, chip_ts) + 34.0f * s);
    ui4::pill(l, rx1 - chip_w, ry0, rx1, ry0 + 32.0f * s, ui4::kBrushBlack, ui4::PillKind::kHud01);
    l.rect(rx1 - chip_w, ry0, rx1 - chip_w + 5.0f * s, ry0 + 32.0f * s, cin.accent_rgba);
    l.text(rx1 - 14.0f * s, ry0 + 8.0f * s, cin.badge_text, chip_ts);

    const char* action_lbl = (st == race::CampaignStage::kFinaleVictory)
                                 ? "PROCEED TO CREDITS"
                                 : (st == race::CampaignStage::kBossVictory ? "CLAIM PINK SLIP" : "START SHOWDOWN");
    ui4::button_bar(l, {{"A", action_lbl}}, w, h, 70.0f);
    return;
  }

  // Lower-third cinematic card for Showcases, Barn Finds, Pre-Race Grid Flyby, Post-Race Finish.
  const float lx0 = 68.0f * s + (1.0f - enter) * (-70.0f * s);
  const float ly0 = h - bar_h - 128.0f * s;
  const float lw = 640.0f * s;
  {
    ui4::TiltScope tilt(l, lx0 + 0.5f * lw, ly0 + 50.0f * s, ui4::kTiltRad * 0.5f);
    ui::TextStyle eye_ts = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite);
    const float ew = std::max(240.0f * s, ui::List::measure(cin.eyebrow, eye_ts) + 34.0f * s);
    ui4::brush(l, lx0 - 6.0f * s, ly0, lx0 + ew, ly0 + 26.0f * s, cin.accent_rgba, false);
    l.text(lx0 + 12.0f * s, ly0 + 6.0f * s, cin.eyebrow, eye_ts);

    ui4::panel(l, lx0, ly0 + 28.0f * s, lx0 + lw, ly0 + 104.0f * s, s, ui4::kCharcoalDark, 0, cin.accent_rgba, true);
    l.text(lx0 + 18.0f * s, ly0 + 38.0f * s, cin.headline, ui4::style(ui::Font::kE, 26.0f * s, ui4::kWhite));
    l.text(lx0 + 18.0f * s, ly0 + 72.0f * s, cin.subheadline, ui4::style(ui::Font::kB, 14.0f * s, ui4::kYellow));
  }
  ui4::button_bar(l, {{"A", "CONTINUE"}}, w, h, 70.0f);
}

void draw_reward_tally(ui::List& l, const race::RewardTallyState& tally, float screen_w, float screen_h) {
  if (!tally.active) return;

  // Stage 1: RACE RESULTS podium & standings screen.
  if (tally.stage == race::PostRaceStage::kRaceResults) {
    draw_post_race_results_stage(l, tally, screen_w, screen_h, tally.stage_elapsed_s);
    return;
  }

  const float s = screen_h / 720.0f;
  const float enter = float(race::ease_out_cubic(std::clamp(tally.stage_elapsed_s / 0.42, 0.0, 1.0)));
  const std::string clean_title = race::resolve_event_display_title(tally.event_title);
  const std::string category = tally.event_category.empty() ? std::string("FESTIVAL RACE") : tally.event_category;

  ui4::subtle_vignette(l, screen_w, screen_h, 0.34f * enter);

  // Stage 2: CREDITS EARNED (focused animated rolling credits & updated balance).
  if (tally.stage == race::PostRaceStage::kCreditsEarned) {
    draw_post_race_stage_pills(l, screen_w, s, 1);

    const float lx0 = 60.0f * s + (1.0f - enter) * (-64.0f * s);
    const float ly0 = 64.0f * s;
    const float lw = 580.0f * s;
    {
      ui4::TiltScope tilt(l, lx0 + 0.5f * lw, ly0 + 230.0f * s, ui4::kTiltRad * 0.52f);
      ui4::title_banner(l, s, lx0, ly0, "CREDITS EARNED", category + "  •  " + clean_title, true, lw);

      const float py0 = ly0 + 80.0f * s;
      const float py1 = py0 + 372.0f * s;
      ui4::panel(l, lx0, py0, lx0 + lw, py1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);

      struct LineItem {
        std::string label;
        int64_t amount;
        uint32_t col;
      };
      const int pl = std::max(1, tally.place);
      std::vector<LineItem> rows;
      rows.push_back({"BASE PURSE  (" + std::to_string(pl) + ordinal_suffix(pl) + " / " +
                          std::to_string(std::max(1, tally.total_cars)) + ")",
                      tally.base_credits, ui4::kWhite});
      if (tally.mileage_bonus > 0) rows.push_back({"MILEAGE BONUS", tally.mileage_bonus, ui4::kOffWhite});
      if (tally.difficulty_bonus > 0)
        rows.push_back({"DIFFICULTY BONUS (+" + std::to_string(tally.difficulty_pct) + "%)", tally.difficulty_bonus,
                        ui4::kYellow});
      if (tally.assists_bonus > 0)
        rows.push_back({"ASSISTS BONUS (+" + std::to_string(tally.assists_pct) + "%)", tally.assists_bonus, ui4::kYellow});
      if (tally.skills_bonus > 0) rows.push_back({"SKILLS & STYLE BONUS", tally.skills_bonus, ui4::kGreen});
      if (tally.nemesis_bonus > 0) rows.push_back({"NEMESIS / SHOWDOWN BONUS", tally.nemesis_bonus, ui4::kAmber});

      float ry = py0 + 20.0f * s;
      const float row_pitch = rows.size() > 5 ? 32.0f * s : 36.0f * s;
      for (size_t i = 0; i < rows.size(); ++i) {
        const float rev = tally.row_reveal(int(i));
        if (rev <= 0.01f) continue;
        const float rx = lx0 + 22.0f * s + (1.0f - rev) * 28.0f * s;
        if (i % 2 == 1) {
          l.rect(lx0 + 12.0f * s, ry - 4.0f * s, lx0 + lw - 12.0f * s, ry + row_pitch - 8.0f * s,
                 with_alpha(ui4::kRowAlt, rev * 0.72f));
        }
        l.text(rx, ry, rows[i].label, ui4::style(ui::Font::kB, 14.5f * s, with_alpha(ui4::kOffWhite, rev)));
        const int64_t shown_row = int64_t(std::llround(double(rows[i].amount) * double(rev)));
        l.text(lx0 + lw - 24.0f * s, ry, "+" + ui4::format_credits(shown_row),
               ui4::style(ui::Font::kE, 16.0f * s, with_alpha(rows[i].col, rev), ui::Align::kRight));
        ry += row_pitch;
      }

      // Rolling Total Credits Earned + Updated Balance strip.
      const float tot_y = py1 - 108.0f * s;
      ui4::brush(l, lx0 + 12.0f * s, tot_y, lx0 + lw - 12.0f * s, tot_y + 46.0f * s, ui4::kPink, false);
      l.text(lx0 + 28.0f * s, tot_y + 13.0f * s, "TOTAL CREDITS EARNED",
             ui4::style(ui::Font::kE, 16.0f * s, ui4::kWhite));
      l.text(lx0 + lw - 28.0f * s, tot_y + 9.0f * s, "+" + ui4::format_credits(tally.displayed_credits()),
             ui4::style(ui::Font::kE, 24.0f * s, ui4::kYellow, ui::Align::kRight));

      ui4::offwhite_strip(l, lx0 + 12.0f * s, tot_y + 52.0f * s, lx0 + lw - 12.0f * s, tot_y + 90.0f * s, s, false);
      l.text(lx0 + 28.0f * s, tot_y + 62.0f * s, "UPDATED BALANCE", ui4::style(ui::Font::kB, 14.5f * s, ui4::kInk));
      l.text(lx0 + lw - 28.0f * s, tot_y + 59.0f * s, ui4::format_credits(tally.displayed_balance()),
             ui4::style(ui::Font::kE, 20.0f * s, ui4::kInk, ui::Align::kRight));
    }

    // Subtle compact finish position emblem on right so hero car stays unobstructed in center-right.
    {
      const int pl = std::max(1, tally.place);
      const float rx1 = screen_w - 58.0f * s;
      const float rx0 = rx1 - 244.0f * s;
      const float ry0 = 96.0f * s;
      ui4::TiltScope rt(l, 0.5f * (rx0 + rx1), ry0 + 44.0f * s, ui4::kTiltRad * 0.45f);
      ui4::panel01(l, rx0, ry0, rx1, ry0 + 88.0f * s, s, ui4::kCharcoalDark, ui4::kPink);
      l.text(rx0 + 16.0f * s, ry0 + 14.0f * s, "FINISH POSITION", ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
      l.text(rx0 + 16.0f * s, ry0 + 36.0f * s, std::to_string(pl) + ordinal_suffix(pl) + " PLACE",
             ui4::style(ui::Font::kE, 28.0f * s, pl == 1 ? ui4::kYellow : ui4::kWhite));
    }

    ui4::button_bar(l, {{"A", tally.stage_animation_done() ? "CONTINUE" : "SKIP TALLY"}}, screen_w, screen_h, 70.0f);
    return;
  }

  // Stage 3: WRISTBAND & POPULARITY (+ PRIZE CAR AWARDED showcase card when a prize car was won!).
  draw_post_race_stage_pills(l, screen_w, s, 2);

  const float lx0 = 60.0f * s + (1.0f - enter) * (-56.0f * s);
  const float ly0 = 64.0f * s;
  const float lw = 560.0f * s;
  {
    ui4::TiltScope tilt(l, lx0 + 0.5f * lw, ly0 + 225.0f * s, ui4::kTiltRad * 0.50f);
    ui4::title_banner(l, s, lx0, ly0, "WRISTBAND & POPULARITY", category + "  •  " + clean_title, false, lw);

    // 1. Wristband Progression Card with real WRISTBANDS/<COLOR>XL artwork & animated bar.
    const float wb_y0 = ly0 + 80.0f * s;
    const float wb_h = 204.0f * s;
    const float flash = tally.tier_up_flash();
    const uint32_t wb_col = ui4::wristband_colour(tally.wb_current_name);
    const uint32_t top_acc = flash > 0.05f ? ui4::kYellow : wb_col;
    ui4::panel(l, lx0, wb_y0, lx0 + lw, wb_y0 + wb_h, s, ui4::kCharcoalDark, top_acc, 0, true);
    if (flash > 0.05f) {
      l.rect(lx0, wb_y0, lx0 + lw, wb_y0 + wb_h, with_alpha(ui4::kYellow, flash * 0.16f));
    }

    // Extract wristband colour key for WRISTBANDS/<COLOR>XL texture.
    std::string wb_key;
    const std::string low_wb = [&]() {
      std::string out = tally.wb_current_name;
      for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
      return out;
    }();
    for (const char* k : {"yellow", "green", "blue", "pink", "orange", "purple", "gold"}) {
      if (low_wb.find(k) != std::string::npos) {
        wb_key = upper_ascii(k);
        break;
      }
    }
    std::array<float, 4> wb_uv{};
    float info_x0 = lx0 + 22.0f * s;
    if (!wb_key.empty() &&
        (ui4::texture("WRISTBANDS/" + wb_key + "XL", wb_uv) || ui4::texture("WRISTBANDS/" + wb_key, wb_uv))) {
      l.image(lx0 + 16.0f * s, wb_y0 + 18.0f * s, lx0 + 148.0f * s, wb_y0 + 112.0f * s, wb_uv);
      info_x0 = lx0 + 162.0f * s;
    }

    l.text(info_x0, wb_y0 + 18.0f * s, "WRISTBAND PROGRESSION", ui4::style(ui::Font::kB, 12.5f * s, ui4::kDim));
    l.text(lx0 + lw - 20.0f * s, wb_y0 + 16.0f * s, "+" + std::to_string(tally.wb_points_earned) + " PTS",
           ui4::style(ui::Font::kE, 19.0f * s, tally.wb_points_earned > 0 ? ui4::kYellow : ui4::kDim, ui::Align::kRight));
    ui::TextStyle wb_ts = ui4::style(ui::Font::kE, 23.0f * s, wb_col);
    ui4::fit_text(tally.wb_current_name, wb_ts, (lx0 + lw - 20.0f * s) - info_x0, 14.0f * s);
    l.text(info_x0, wb_y0 + 42.0f * s, tally.wb_current_name, wb_ts);
    l.text(info_x0, wb_y0 + 74.0f * s,
           tally.wb_tier_up ? "COLLECT NEXT WRISTBAND AT RACE CENTRAL" : ("NEXT: " + tally.wb_next_name),
           ui4::style(ui::Font::kB, 12.5f * s, tally.wb_tier_up ? ui4::kYellow : ui4::kOffWhite));

    // Animated Wristband Fill Bar.
    const float bx0 = lx0 + 22.0f * s;
    const float bx1 = lx0 + lw - 22.0f * s;
    const float by0 = wb_y0 + 124.0f * s;
    const float by1 = by0 + 24.0f * s;
    const float frac = tally.wb_bar_fraction();
    l.rect(bx0, by0, bx1, by1, ui::rgba(255, 255, 255, 34));
    l.rect(bx0, by0, bx0 + (bx1 - bx0) * frac, by1, tally.wb_tier_up ? ui4::kYellow : ui4::kPink);
    l.text(bx0, by1 + 10.0f * s, std::to_string(tally.displayed_wb_points()) + " PTS",
           ui4::style(ui::Font::kE, 15.0f * s, ui4::kWhite));
    l.text(bx1, by1 + 10.0f * s,
           tally.wb_tier_up ? ("TIER UP!  ->  " + tally.wb_next_name)
                            : ("TARGET: " + std::to_string(tally.wb_tier_hi) + " PTS"),
           ui4::style(ui::Font::kB, 13.5f * s, tally.wb_tier_up ? ui4::kYellow : ui4::kDim, ui::Align::kRight));

    // 2. Popularity Rank Progression Card.
    const float pop_y = wb_y0 + wb_h + 18.0f * s;
    const float pop_h = 112.0f * s;
    ui4::panel(l, lx0, pop_y, lx0 + lw, pop_y + pop_h, s, ui4::kCharcoalDark, 0, ui4::kYellow, true);
    l.text(lx0 + 22.0f * s, pop_y + 14.0f * s, "POPULARITY RANK PROGRESSION",
           ui4::style(ui::Font::kB, 13.0f * s, ui4::kDim));
    const int cur_rank = tally.displayed_pop_rank();
    const int climbed = std::max(0, tally.pop_rank_before - tally.pop_rank_after);
    l.text(lx0 + 22.0f * s, pop_y + 38.0f * s, "#" + std::to_string(cur_rank),
           ui4::style(ui::Font::kE, 42.0f * s, ui4::kYellow));
    l.text(lx0 + lw - 22.0f * s, pop_y + 42.0f * s,
           climbed > 0 ? ("+" + std::to_string(climbed) + " RANKS CLIMBED") : "FESTIVAL STANDING",
           ui4::style(ui::Font::kE, 17.0f * s, climbed > 0 ? ui4::kGreen : ui4::kOffWhite, ui::Align::kRight));
    if (climbed > 0) {
      l.text(lx0 + lw - 22.0f * s, pop_y + 68.0f * s,
             "PREVIOUS RANK #" + std::to_string(tally.pop_rank_before) + "  ->  #" + std::to_string(tally.pop_rank_after),
             ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim, ui::Align::kRight));
    }
  }

  // Right Column: PRIZE CAR AWARDED showcase celebration card + Next Campaign Objective card.
  const float rx0 = lx0 + lw + 40.0f * s;
  const float rw = screen_w - rx0 - 58.0f * s;
  float cur_ry = 144.0f * s;
  if (rw >= 240.0f * s) {
    ui4::TiltScope rtilt(l, rx0 + 0.5f * rw, cur_ry + 160.0f * s, ui4::kTiltRad * 0.45f);

    if (!tally.prize_car_name.empty()) {
      const float ph = 186.0f * s;
      ui4::panel(l, rx0, cur_ry, rx0 + rw, cur_ry + ph, s, ui4::kCharcoalDark, ui4::kYellow, ui4::kPink, true);
      ui4::brush(l, rx0 - 6.0f * s, cur_ry + 12.0f * s, rx0 + rw + 6.0f * s, cur_ry + 50.0f * s, ui4::kPink, false);
      float hx = rx0 + 18.0f * s;
      if (ui4::badge_icon(l, hx, cur_ry + 16.0f * s, hx + 28.0f * s, cur_ry + 44.0f * s, ui4::BadgeIconKind::kTrophy)) {
        hx += 36.0f * s;
      }
      l.text(hx, cur_ry + 21.0f * s, "PRIZE CAR AWARDED!", ui4::style(ui::Font::kE, 18.0f * s, ui4::kYellow));

      ui::TextStyle car_ts = ui4::style(ui::Font::kE, 25.0f * s, ui4::kWhite, ui::Align::kLeft, true);
      ui4::fit_text(tally.prize_car_name, car_ts, rw - 38.0f * s, 14.0f * s);
      l.text(rx0 + 20.0f * s, cur_ry + 68.0f * s, tally.prize_car_name, car_ts);

      ui4::offwhite_strip(l, rx0 + 18.0f * s, cur_ry + 118.0f * s, rx0 + rw - 18.0f * s, cur_ry + 150.0f * s, s, false);
      l.text(rx0 + 30.0f * s, cur_ry + 127.0f * s, "DELIVERED TO YOUR HORIZON GARAGE",
             ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
      l.text(rx0 + 20.0f * s, cur_ry + 158.0f * s, "SWITCH CARS ANYTIME IN MY GARAGE OR AT OUTPOSTS",
             ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
      cur_ry += ph + 18.0f * s;
    }

    if (!tally.next_unlock_headline.empty() || tally.has_next_gps) {
      const float nh = 114.0f * s;
      ui4::panel(l, rx0, cur_ry, rx0 + rw, cur_ry + nh, s, ui4::kBrushBlack, ui4::kPink, 0, true);
      const std::string hdr =
          !tally.next_unlock_headline.empty() ? tally.next_unlock_headline : "NEXT CAMPAIGN OBJECTIVE";
      const std::string det =
          !tally.next_unlock_detail.empty() ? tally.next_unlock_detail : ("GPS READY: " + tally.next_gps_label);
      ui::TextStyle hts = ui4::style(ui::Font::kE, 17.5f * s, ui4::kPinkHi);
      ui4::fit_text(hdr, hts, rw - 36.0f * s, 12.0f * s);
      l.text(rx0 + 20.0f * s, cur_ry + 16.0f * s, hdr, hts);
      ui::TextStyle dts = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite);
      ui4::fit_text(det, dts, rw - 36.0f * s, 10.0f * s);
      l.text(rx0 + 20.0f * s, cur_ry + 44.0f * s, det, dts);
      if (tally.has_next_gps) {
        l.text(rx0 + 20.0f * s, cur_ry + 76.0f * s,
               "PRESS [" + ui4::glyph_label_for("X") + "] TO AUTO-SET GPS TO " + tally.next_gps_label,
               ui4::style(ui::Font::kB, 12.5f * s, ui4::kYellow));
      }
    }
  }

  std::vector<ui4::Prompt> prompts = {
      {"A", tally.stage_animation_done() ? loc_str("InGame", "IDS_ContinueMenu", "CONTINUE")
                                         : loc_str("HelpButtons", "IDS_Skip", "SKIP ANIMATION")}};
  if (tally.has_next_gps) {
    prompts.push_back({"X", std::string("SET GPS TO NEXT OBJECTIVE")});
  }
  ui4::button_bar(l, prompts, screen_w, screen_h, 70.0f);
}

void draw_credits_roll(ui::List& l, const race::CampaignFlow& flow, float screen_w, float screen_h) {
  const float s = screen_h / 720.0f;
  // Subtle dark theater wash so sunset Colorado still shines behind credits.
  l.rect(0, 0, screen_w, screen_h, ui::rgba(8, 8, 12, 168));
  const float bar_h = 46.0f * s;
  l.rect(0, 0, screen_w, bar_h, ui::rgba(0, 0, 0, 240));
  l.rect(0, screen_h - bar_h, screen_w, screen_h, ui::rgba(0, 0, 0, 240));

  static const std::pair<const char*, const char*> kCredits[] = {
      {"FORZA HORIZON  •  COLORADO", "HORIZON FESTIVAL CHAMPION"},
      {"FESTIVAL DIRECTOR", "ALICE HART"},
      {"CHIEF MECHANIC", "DAK STEWART"},
      {"EVENT COORDINATOR", "HOLLY CRUZ"},
      {"HORIZON BASS ARENA", "SCOTT TYLER"},
      {"STAR SHOWDOWN RIVALS", "RAMONA CRAVACHE  •  ALI HOWARD  •  DUKE MAGUIRE"},
      {"STAR SHOWDOWN RIVALS", "MARKO BARAN  •  ZAKI MALIK  •  HAILEY HARPER"},
      {"HORIZON FINALE CHAMPION", "DARIUS FLYNT  •  2010 FERRARI 599XX"},
      {"SHIFT ENGINE", "NATIVE C++20 / VULKAN CAMPAIGN & SIMULATION"},
      {"THANK YOU FOR DRIVING", "ENDLESS COLORADO FREE ROAM CONTINUES AFTER CREDITS"},
  };

  const float cx = 0.5f * screen_w;
  const float scroll_y = screen_h - float(flow.stage_time()) * 58.0f * s;
  for (size_t i = 0; i < std::size(kCredits); ++i) {
    const float y = scroll_y + float(i) * 92.0f * s;
    if (y < bar_h - 40.0f * s || y > screen_h - bar_h + 20.0f * s) continue;
    l.text(cx, y, kCredits[i].first, ui4::style(ui::Font::kB, 14.0f * s, ui4::kPinkHi, ui::Align::kCenter));
    l.text(cx, y + 22.0f * s, kCredits[i].second, ui4::style(ui::Font::kE, 22.0f * s, ui4::kWhite, ui::Align::kCenter));
  }

  ui4::button_bar(l, {{"A", "RETURN TO COLORADO FREE ROAM"}}, screen_w, screen_h, 70.0f);
}

void draw_transition(ui::List& l, const race::TransitionState& trans, float screen_w, float screen_h) {
  if (!trans.active) return;
  const float alpha = trans.cover_alpha();
  if (alpha <= 0.005f) return;

  // Every kind is the flows' black screen fade (race/screen_fade.h): the
  // original has no brush wipes, slides or title strips between screens.
  l.rect(0, 0, screen_w, screen_h, with_alpha(ui::rgba(0, 0, 0, 255), alpha));
}

void draw(ui::List& l, const race::CampaignFlow& flow, const DrawContext& ctx) {
  const auto st = flow.stage();
  const bool full_ceremony =
      (st == race::CampaignStage::kWristbandCeremony || st == race::CampaignStage::kFestivalRegistration);
  if (st == race::CampaignStage::kRewardTally && flow.reward_tally().active) {
    draw_reward_tally(l, flow.reward_tally(), ctx.screen_w, ctx.screen_h);
  } else if (flow.is_cinematic_active()) {
    draw_cinematic_overlay(l, flow, ctx);
  } else if (!ctx.race_screens) {
    draw_objective_tracker(l, flow, ctx);
  }
  DrawContext dctx = ctx;
  if (full_ceremony) {
    // Prevent top-right call-in from colliding with centered ceremony banner & suppress duplicate subtitle text.
    dctx.suppress_callin = true;
    dctx.suppress_subtitle = true;
  }
  if (!ctx.race_screens) draw_dialog_and_subtitles(l, flow.dialogs(), dctx);
  draw_transition(l, flow.transition(), ctx.screen_w, ctx.screen_h);
}

}  // namespace carview::campaign_ui
