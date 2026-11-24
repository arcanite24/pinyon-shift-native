#include "launcher_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "launcher_font.h"
#include "ui4_chrome.h"

namespace launcher {
namespace {

namespace ui = carview::ui;

// ---- Calm dark charcoal-slate palette --------------------------------------
constexpr uint32_t kBgCanvas = ui::rgba(17, 19, 24);
constexpr uint32_t kBgSidebar = ui::rgba(22, 24, 30);
constexpr uint32_t kBgCard = ui::rgba(26, 29, 37);
constexpr uint32_t kBgInset = ui::rgba(19, 22, 28);
constexpr uint32_t kBgRowAlt = ui::rgba(31, 35, 44);
constexpr uint32_t kBgHover = ui::rgba(38, 43, 54);
constexpr uint32_t kBgPressed = ui::rgba(28, 32, 41);
constexpr uint32_t kBgSelected = ui::rgba(37, 41, 52);

constexpr uint32_t kBorderSubtle = ui::rgba(38, 42, 53);
constexpr uint32_t kBorderCard = ui::rgba(45, 50, 63);
constexpr uint32_t kBorderHover = ui::rgba(74, 82, 102);

constexpr uint32_t kTextPrimary = ui::rgba(236, 239, 244);
constexpr uint32_t kTextSecondary = ui::rgba(155, 163, 180);
constexpr uint32_t kTextMuted = ui::rgba(108, 116, 134);

// Restrained Horizon magenta (#E8195E) — primary action & active indicator only.
constexpr uint32_t kAccent = ui::rgba(0xE8, 0x19, 0x5E);
constexpr uint32_t kAccentHover = ui::rgba(0xF5, 0x2A, 0x6E);
constexpr uint32_t kAccentPress = ui::rgba(0xC9, 0x13, 0x50);
constexpr uint32_t kAccentSoft = ui::rgba(0xE8, 0x19, 0x5E, 30);

// Tasteful semantic colors.
constexpr uint32_t kGreen = ui::rgba(63, 185, 107);
constexpr uint32_t kGreenSoft = ui::rgba(63, 185, 107, 26);
constexpr uint32_t kAmber = ui::rgba(217, 155, 38);
constexpr uint32_t kAmberSoft = ui::rgba(217, 155, 38, 28);
constexpr uint32_t kRed = ui::rgba(224, 82, 82);
constexpr uint32_t kRedSoft = ui::rgba(224, 82, 82, 32);
constexpr uint32_t kBlue = ui::rgba(74, 144, 217);

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string fmt_size(uint64_t bytes) {
  char buf[32];
  if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
  else if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.0f MB", double(bytes) / double(1ull << 20));
  else if (bytes >= (1ull << 10)) std::snprintf(buf, sizeof buf, "%llu KB", (unsigned long long)(bytes >> 10));
  else std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
  return buf;
}

float text_y_center(float y0, float y1, float px_size) {
  return 0.5f * (y0 + y1) - 0.52f * px_size;
}

void draw_card(ui::List& l, float x0, float y0, float x1, float y1, float d) {
  l.rect(x0, y0, x1, y1, kBgCard, 6.0f * d);
  l.frame(x0, y0, x1, y1, kBorderCard, 1.0f * d, 6.0f * d);
}

void draw_inset_box(ui::List& l, float x0, float y0, float x1, float y1, float d, uint32_t border = kBorderSubtle) {
  l.rect(x0, y0, x1, y1, kBgInset, 5.0f * d);
  l.frame(x0, y0, x1, y1, border, 1.0f * d, 5.0f * d);
}

void draw_divider(ui::List& l, float x0, float y, float x1, float d) {
  l.rect(x0, std::floor(y), x1, std::floor(y) + std::max(1.0f, std::round(d)), kBorderSubtle);
}

struct ButtonStyle {
  bool primary = false;
  bool selected = false;
  bool quiet = false;
};

void draw_button(ui::List& l, std::vector<HitBox>& hits, const LauncherUiState& st, const std::string& id,
                 float x0, float y0, float x1, float y1, std::string_view label, float d, ButtonStyle bs = {}) {
  hits.push_back({id, x0, y0, x1, y1, true});
  const bool hov = st.hovered_id == id;
  const bool prs = st.pressed_id == id;
  const bool foc = st.focused_id == id;
  const float r = 5.0f * d;

  uint32_t fill = kBgRowAlt;
  uint32_t border = kBorderCard;
  uint32_t fg = kTextPrimary;

  if (bs.primary) {
    fill = prs ? kAccentPress : (hov ? kAccentHover : kAccent);
    border = fill;
    fg = ui::rgba(255, 255, 255);
  } else if (bs.selected) {
    fill = prs ? kBgPressed : (hov ? ui::rgba(46, 52, 66) : kBgSelected);
    border = kAccent;
    fg = ui::rgba(255, 255, 255);
  } else if (bs.quiet) {
    fill = prs ? kBgPressed : (hov ? kBgHover : ui::rgba(0, 0, 0, 0));
    border = hov ? kBorderCard : ui::rgba(0, 0, 0, 0);
    fg = hov ? kTextPrimary : kTextSecondary;
  } else {
    fill = prs ? kBgPressed : (hov ? kBgHover : ui::rgba(33, 37, 47));
    border = hov ? kBorderHover : kBorderCard;
  }

  if ((fill >> 24) > 0) l.rect(x0, y0, x1, y1, fill, r);
  if ((border >> 24) > 0) l.frame(x0, y0, x1, y1, border, (bs.selected ? 1.25f : 1.0f) * d, r);
  if (foc) l.frame(x0 - 2.0f * d, y0 - 2.0f * d, x1 + 2.0f * d, y1 + 2.0f * d, kAccent, 1.5f * d, r + 1.5f * d);

  const float font_px = (bs.primary && (y1 - y0) >= 42.0f * d) ? 15.0f * d : 13.0f * d;
  const float max_w = std::max(12.0f * d, (x1 - x0) - 16.0f * d);
  draw_text(l, 0.5f * (x0 + x1), text_y_center(y0, y1, font_px), label,
            bs.primary || bs.selected ? FontFace::kSemiBold : FontFace::kRegular,
            font_px, fg, ui::Align::kCenter, max_w);
}

void draw_checkbox(ui::List& l, std::vector<HitBox>& hits, const LauncherUiState& st, const std::string& id,
                   float x, float y, float w, float h, std::string_view label, bool checked, float d) {
  hits.push_back({id, x, y, x + w, y + h, true});
  const bool hov = st.hovered_id == id;
  const bool foc = st.focused_id == id;

  if (hov) l.rect(x, y, x + w, y + h, ui::rgba(255, 255, 255, 10), 4.0f * d);
  if (foc) l.frame(x, y, x + w, y + h, kAccent, 1.2f * d, 4.0f * d);

  const float box = 16.0f * d;
  const float bx0 = x + 4.0f * d;
  const float by0 = 0.5f * (y + y + h) - 0.5f * box;
  l.rect(bx0, by0, bx0 + box, by0 + box, checked ? kAccent : kBgInset, 3.0f * d);
  l.frame(bx0, by0, bx0 + box, by0 + box, checked ? kAccent : (hov ? kBorderHover : kBorderCard), 1.2f * d, 3.0f * d);
  if (checked) {
    // Crisp checkmark
    l.line(bx0 + 3.8f * d, by0 + 8.4f * d, bx0 + 6.8f * d, by0 + 11.6f * d, 1.8f * d, ui::rgba(255, 255, 255));
    l.line(bx0 + 6.8f * d, by0 + 11.6f * d, bx0 + 12.4f * d, by0 + 4.8f * d, 1.8f * d, ui::rgba(255, 255, 255));
  }
  const float font_px = 13.0f * d;
  draw_text(l, bx0 + box + 10.0f * d, text_y_center(y, y + h, font_px), label, FontFace::kRegular, font_px,
            checked ? kTextPrimary : kTextSecondary, ui::Align::kLeft, std::max(10.0f, w - box - 18.0f * d));
}

void draw_segmented(ui::List& l, std::vector<HitBox>& hits, const LauncherUiState& st, float x0, float y0,
                    float x1, float y1, const char* const* labels, const char* const* ids, int count,
                    int selected_idx, float d) {
  draw_inset_box(l, x0, y0, x1, y1, d);
  if (count <= 0) return;
  const float pad = 3.0f * d;
  const float inner_w = (x1 - x0) - 2.0f * pad;
  const float seg_w = inner_w / float(count);
  for (int i = 0; i < count; ++i) {
    const float sx0 = x0 + pad + float(i) * seg_w;
    const float sx1 = sx0 + seg_w;
    const float sy0 = y0 + pad;
    const float sy1 = y1 - pad;
    const std::string sid(ids[i]);
    hits.push_back({sid, sx0, sy0, sx1, sy1, true});
    const bool active = (i == selected_idx);
    const bool hov = st.hovered_id == sid;
    const bool foc = st.focused_id == sid;
    if (active) {
      l.rect(sx0, sy0, sx1, sy1, kBgSelected, 4.0f * d);
      l.frame(sx0, sy0, sx1, sy1, kBorderHover, 1.0f * d, 4.0f * d);
      l.rect(sx0 + 10.0f * d, sy1 - 2.0f * d, sx1 - 10.0f * d, sy1, kAccent, 1.0f * d);
    } else if (hov) {
      l.rect(sx0, sy0, sx1, sy1, ui::rgba(255, 255, 255, 10), 4.0f * d);
    }
    if (foc) l.frame(sx0, sy0, sx1, sy1, kAccent, 1.3f * d, 4.0f * d);
    const float font_px = 12.5f * d;
    draw_text(l, 0.5f * (sx0 + sx1), text_y_center(sy0, sy1, font_px), labels[i],
              active ? FontFace::kSemiBold : FontFace::kRegular, font_px,
              active ? kTextPrimary : (hov ? kTextPrimary : kTextSecondary),
              ui::Align::kCenter, std::max(8.0f, seg_w - 8.0f * d));
  }
}

void draw_scrollbar(ui::List& l, float x, float y0, float y1, int first, int visible, int total, float d) {
  if (total <= visible || visible <= 0) return;
  const float w = 4.0f * d;
  const float h = y1 - y0;
  l.rect(x, y0, x + w, y1, ui::rgba(255, 255, 255, 14), 2.0f * d);
  const float thumb_h = std::max(24.0f * d, h * (float(visible) / float(total)));
  const float max_scroll = float(std::max(1, total - visible));
  const float t = std::clamp(float(first) / max_scroll, 0.0f, 1.0f);
  const float ty0 = y0 + t * (h - thumb_h);
  l.rect(x, ty0, x + w, ty0 + thumb_h, ui::rgba(110, 120, 140, 190), 2.0f * d);
}

void draw_progress_bar(ui::List& l, float x, float y, float w, float h, float frac, uint32_t fill, float d) {
  frac = std::clamp(frac, 0.0f, 1.0f);
  const float r = h * 0.5f;
  l.rect(x, y, x + w, y + h, kBgInset, r);
  l.frame(x, y, x + w, y + h, kBorderSubtle, 1.0f * d, r);
  if (frac > 0.005f) {
    l.rect(x + 1.0f * d, y + 1.0f * d, x + std::max(h, w * frac) - 1.0f * d, y + h - 1.0f * d, fill, r);
  }
}

void draw_axis_bar(ui::List& l, float x, float y, float w, float bar_h, std::string_view label, float val,
                   bool centered, uint32_t fill, float dz_in, float d) {
  const float font_px = 12.5f * d;
  draw_text(l, x, y, label, FontFace::kRegular, font_px, kTextSecondary);
  char pct[24];
  if (centered) std::snprintf(pct, sizeof pct, "%+d%%", int(std::lround(val * 100.0f)));
  else std::snprintf(pct, sizeof pct, "%d%%", int(std::lround(val * 100.0f)));
  draw_text(l, x + w, y, pct, FontFace::kMono, font_px, kTextPrimary, ui::Align::kRight);

  const float by = y + 20.0f * d;
  const float r = 4.0f * d;
  l.rect(x, by, x + w, by + bar_h, kBgInset, r);
  l.frame(x, by, x + w, by + bar_h, kBorderSubtle, 1.0f * d, r);

  if (centered) {
    const float mid = x + w * 0.5f;
    const float dz_w = std::clamp(dz_in, 0.0f, 0.45f) * (w * 0.5f);
    if (dz_w > 1.0f) {
      l.rect(mid - dz_w, by + 2.0f * d, mid + dz_w, by + bar_h - 2.0f * d, ui::rgba(255, 255, 255, 18), 2.0f * d);
    }
    l.line(mid, by + 2.0f * d, mid, by + bar_h - 2.0f * d, 1.2f * d, ui::rgba(115, 125, 145, 180));
    const float clamped = std::clamp(val, -1.0f, 1.0f);
    const float end_x = mid + clamped * (w * 0.5f - 2.0f * d);
    if (std::fabs(end_x - mid) > 1.5f) {
      l.rect(std::min(mid, end_x), by + 2.5f * d, std::max(mid, end_x), by + bar_h - 2.5f * d, fill, 2.5f * d);
    }
  } else {
    const float clamped = std::clamp(val, 0.0f, 1.0f);
    const float dz_w = std::clamp(dz_in, 0.0f, 0.45f) * w;
    if (dz_w > 1.0f) {
      l.rect(x + 2.0f * d, by + 2.0f * d, x + dz_w, by + bar_h - 2.0f * d, ui::rgba(255, 255, 255, 16), 2.0f * d);
    }
    if (clamped > 0.005f) {
      l.rect(x + 2.0f * d, by + 2.5f * d, x + 2.0f * d + clamped * (w - 4.0f * d), by + bar_h - 2.5f * d, fill, 2.5f * d);
    }
  }
}

void draw_sidebar_icon(ui::List& l, LauncherTab tab, float cx, float cy, float d, uint32_t col) {
  const float r = 6.0f * d;
  const float t = 1.4f * d;
  switch (tab) {
    case LauncherTab::kPlay: {
      // Crisp play triangle outline
      l.line(cx - 3.5f * d, cy - 4.8f * d, cx + 4.8f * d, cy, t, col);
      l.line(cx + 4.8f * d, cy, cx - 3.5f * d, cy + 4.8f * d, t, col);
      l.line(cx - 3.5f * d, cy + 4.8f * d, cx - 3.5f * d, cy - 4.8f * d, t, col);
      break;
    }
    case LauncherTab::kSetup: {
      // Disc / setup circle with center hub
      l.arc(cx, cy, r, t, 0.0f, 6.283f, col);
      l.circle(cx, cy, 2.0f * d, col);
      break;
    }
    case LauncherTab::kDlc: {
      // Content package box
      l.frame(cx - r, cy - r * 0.85f, cx + r, cy + r * 0.85f, col, t, 1.5f * d);
      l.line(cx - r, cy - 1.5f * d, cx + r, cy - 1.5f * d, t, col);
      l.line(cx, cy - r * 0.85f, cx, cy - 1.5f * d, t, col);
      break;
    }
    case LauncherTab::kMods: {
      // Stacked layers / sliders
      l.line(cx - r, cy - 3.5f * d, cx + r, cy - 3.5f * d, t, col);
      l.line(cx - r, cy, cx + r, cy, t, col);
      l.line(cx - r, cy + 3.5f * d, cx + r, cy + 3.5f * d, t, col);
      l.circle(cx - 2.0f * d, cy - 3.5f * d, 2.0f * d, col);
      l.circle(cx + 2.5f * d, cy, 2.0f * d, col);
      l.circle(cx - 1.0f * d, cy + 3.5f * d, 2.0f * d, col);
      break;
    }
    case LauncherTab::kRadio: {
      // Radio antenna / waves
      l.circle(cx, cy + 1.5f * d, 2.0f * d, col);
      l.arc(cx, cy + 1.5f * d, 4.8f * d, t, -0.9f, 0.9f, col);
      l.arc(cx, cy + 1.5f * d, 4.8f * d, t, 2.24f, 4.04f, col);
      break;
    }
    case LauncherTab::kWheel: {
      // Steering wheel rim + spokes
      l.arc(cx, cy, r, t, 0.0f, 6.283f, col);
      l.line(cx - r + 1.5f * d, cy, cx + r - 1.5f * d, cy, t, col);
      l.line(cx, cy, cx, cy + r - 1.5f * d, t, col);
      break;
    }
    case LauncherTab::kUpdate: {
      // Circular update arrow
      l.arc(cx, cy, r * 0.9f, t, 0.4f, 5.5f, col);
      l.line(cx + 2.0f * d, cy - r * 0.9f, cx + 5.2f * d, cy - r * 0.6f, t, col);
      l.line(cx + 5.2f * d, cy - r * 0.6f, cx + 2.4f * d, cy - r * 0.2f, t, col);
      break;
    }
    case LauncherTab::kAndroid: {
      // Handheld / phone outline
      l.frame(cx - 4.2f * d, cy - 6.2f * d, cx + 4.2f * d, cy + 6.2f * d, col, t, 1.8f * d);
      l.line(cx - 1.5f * d, cy + 3.8f * d, cx + 1.5f * d, cy + 3.8f * d, t, col);
      break;
    }
    default: break;
  }
}

void draw_page_header(ui::List& l, float x0, float x1, float y, std::string_view title, float d) {
  draw_text(l, x0, y, title, FontFace::kSemiBold, 20.0f * d, kTextPrimary);
  draw_divider(l, x0, y + 36.0f * d, x1, d);
}

// ---- Tab 1: Play -----------------------------------------------------------

void draw_play_tab(ui::List& l, const LauncherUiState& st, float x0, float y0, float x1, float y1, float d,
                   std::vector<HitBox>& hits) {
  const float cw = x1 - x0;
  draw_page_header(l, x0, x1, y0, "Play", d);

  const float top_y = y0 + 50.0f * d;

  // 1. Hero Card: Game status, profile, car, and 1-click Play button
  const float hero_h = 124.0f * d;
  draw_card(l, x0, top_y, x1, top_y + hero_h, d);

  const bool ready = st.play_cfg.is_ready();
  const float play_w = 160.0f * d;
  const float play_h = 46.0f * d;
  const float play_x0 = x1 - 24.0f * d - play_w;
  const float play_y0 = top_y + 0.5f * (hero_h - play_h);
  const float text_max_w = std::max(200.0f * d, play_x0 - x0 - 44.0f * d);

  draw_text(l, x0 + 24.0f * d, top_y + 20.0f * d, "Forza Horizon", FontFace::kSemiBold, 23.0f * d, kTextPrimary,
            ui::Align::kLeft, text_max_w);

  // Readiness line with subtle status dot
  const float sy = top_y + 56.0f * d;
  l.circle(x0 + 29.0f * d, sy + 7.5f * d, 4.0f * d, ready ? kGreen : kAmber);
  const std::string car_info =
      st.play_cfg.disc_car_count > 0
          ? (std::to_string(st.play_cfg.disc_car_count) + " cars (direct-from-disc)")
          : "Direct-from-disc streaming";
  const std::string ready_str =
      ready ? ("Ready  \xC2\xB7  " + car_info + "  \xC2\xB7  " +
               (st.play_cfg.has_colorado_index ? "Colorado world index" : "World index builds on launch"))
            : "Game files not configured yet  \xC2\xB7  Open Setup to select your disc folder";
  draw_text(l, x0 + 40.0f * d, sy, ready_str, FontFace::kRegular, 13.5f * d, ready ? kTextPrimary : kAmber,
            ui::Align::kLeft, text_max_w - 16.0f * d);

  // Active profile & car line
  const std::string car_label = carview::ui4::pretty_car_media_name(st.launch_car);
  const std::string meta_str = "Profile: " + st.save_iso.profile + "   \xC2\xB7   Car: " + car_label;
  draw_text(l, x0 + 24.0f * d, top_y + 84.0f * d, meta_str, FontFace::kRegular, 13.0f * d, kTextSecondary,
            ui::Align::kLeft, text_max_w);

  // Play folder path (quiet monospace)
  const std::string play_path = st.paths.play_dir.empty() ? "(not set)" : st.paths.play_dir.string();
  draw_text(l, x0 + 24.0f * d, top_y + 106.0f * d, play_path, FontFace::kMono, 11.5f * d, kTextMuted,
            ui::Align::kLeft, text_max_w);

  // Primary Play button
  draw_button(l, hits, st, "act:play", play_x0, play_y0, play_x0 + play_w, play_y0 + play_h, "Play", d,
              {.primary = true});

  // 2. Display & Performance Card
  const float cfg_y = top_y + hero_h + 14.0f * d;
  const float cfg_h = 240.0f * d;
  draw_card(l, x0, cfg_y, x1, cfg_y + cfg_h, d);

  draw_text(l, x0 + 24.0f * d, cfg_y + 16.0f * d, "Graphics & Display", FontFace::kSemiBold, 14.0f * d, kTextPrimary);

  const float lbl_w = 104.0f * d;
  const float right_btn_w = 156.0f * d;
  const float seg_x0 = x0 + 24.0f * d + lbl_w;
  const float seg_x1 = x1 - 24.0f * d - right_btn_w - 12.0f * d;

  // Row 1: Graphics Preset
  const float r1_y = cfg_y + 44.0f * d;
  const float row_h = 32.0f * d;
  draw_text(l, x0 + 24.0f * d, text_y_center(r1_y, r1_y + row_h, 13.0f * d), "Preset", FontFace::kRegular,
            13.0f * d, kTextSecondary);
  static constexpr const char* kPresetLabels[5] = {"High", "Low-Spec", "Smooth 60", "Quality 30", "Low Power"};
  static constexpr const char* kPresetIds[5] = {"preset:0", "preset:1", "preset:2", "preset:3", "preset:4"};
  draw_segmented(l, hits, st, seg_x0, r1_y, seg_x1, r1_y + row_h, kPresetLabels, kPresetIds, 5,
                 std::clamp(st.selected_preset, 0, 4), d);
  draw_button(l, hits, st, "act:apply_rec", seg_x1 + 12.0f * d, r1_y, x1 - 24.0f * d, r1_y + row_h,
              "Use Recommended", d);

  // Row 2: Resolution & Window mode
  const float r2_y = r1_y + row_h + 10.0f * d;
  draw_text(l, x0 + 24.0f * d, text_y_center(r2_y, r2_y + row_h, 13.0f * d), "Resolution", FontFace::kRegular,
            13.0f * d, kTextSecondary);
  const char* res_labels[5] = {kResolutionPresets[0].label, kResolutionPresets[1].label, kResolutionPresets[2].label,
                               kResolutionPresets[3].label, kResolutionPresets[4].label};
  static constexpr const char* kResIds[5] = {"res:0", "res:1", "res:2", "res:3", "res:4"};
  draw_segmented(l, hits, st, seg_x0, r2_y, seg_x1, r2_y + row_h, res_labels, kResIds, 5,
                 std::clamp(st.selected_resolution, 0, 4), d);
  draw_button(l, hits, st, "act:fullscreen", seg_x1 + 12.0f * d, r2_y, x1 - 24.0f * d, r2_y + row_h,
              st.launch_fullscreen ? "Mode: Fullscreen" : "Mode: Windowed", d,
              {.selected = st.launch_fullscreen});

  // Row 3: Graphics Backend (Auto / Vulkan / DirectX 12 on Windows; Auto / Metal 3 / Vulkan on macOS)
  const float r3_y = r2_y + row_h + 10.0f * d;
  draw_text(l, x0 + 24.0f * d, text_y_center(r3_y, r3_y + row_h, 13.0f * d), "Backend", FontFace::kRegular,
            13.0f * d, kTextSecondary);
  const auto backends = available_backend_choices();
  const char* be_labels[4] = {};
  static constexpr const char* kBeIds[4] = {"backend:0", "backend:1", "backend:2", "backend:3"};
  const int be_count = std::min(int(backends.size()), 4);
  for (int bi = 0; bi < be_count; ++bi) be_labels[bi] = backends[size_t(bi)].label;
  draw_segmented(l, hits, st, seg_x0, r3_y, seg_x1, r3_y + row_h, be_labels, kBeIds, be_count,
                 std::clamp(st.selected_backend, 0, std::max(0, be_count - 1)), d);
  draw_text(l, seg_x1 + 12.0f * d, text_y_center(r3_y, r3_y + row_h, 12.0f * d),
            be_count > 0 ? (std::string("RHI: ") + backends[size_t(std::clamp(st.selected_backend, 0, be_count - 1))].label)
                         : "RHI: Auto",
            FontFace::kMono, 12.0f * d, kTextMuted, ui::Align::kLeft, right_btn_w);

  // Hardware 1-line summary bar
  const float hw_y = r3_y + row_h + 14.0f * d;
  const float hw_h = 38.0f * d;
  draw_inset_box(l, x0 + 24.0f * d, hw_y, x1 - 24.0f * d, hw_y + hw_h, d,
                 st.hw_rec.warning.empty() ? kBorderSubtle : kAmber);
  const std::string hw_line = describe_hardware(st.hw_info);
  const std::string rec_line =
      st.hw_rec.warning.empty() ? ("Recommended: " + st.hw_rec.label) : "Driver update recommended";
  const float rec_w = measure_text(FontFace::kSemiBold, 12.5f * d, rec_line) + 24.0f * d;
  draw_text(l, x0 + 38.0f * d, text_y_center(hw_y, hw_y + hw_h, 12.5f * d), hw_line, FontFace::kRegular,
            12.5f * d, kTextPrimary, ui::Align::kLeft, std::max(120.0f * d, cw - 76.0f * d - rec_w));
  draw_text(l, x1 - 38.0f * d, text_y_center(hw_y, hw_y + hw_h, 12.5f * d), rec_line, FontFace::kSemiBold,
            12.5f * d, st.hw_rec.warning.empty() ? kGreen : kAmber, ui::Align::kRight);

  // 3. Bottom Two-Column Row: Quick Actions + Installed Content Summary
  const float bot_y = cfg_y + cfg_h + 16.0f * d;
  const float bot_h = std::max(150.0f * d, y1 - bot_y);
  const float left_w = std::floor((cw - 16.0f * d) * 0.56f);
  const float left_x1 = x0 + left_w;
  const float right_x0 = left_x1 + 16.0f * d;

  // Left: Quick Actions
  draw_card(l, x0, bot_y, left_x1, bot_y + bot_h, d);
  draw_text(l, x0 + 24.0f * d, bot_y + 18.0f * d, "Quick Actions", FontFace::kSemiBold, 14.0f * d, kTextPrimary);

  const float qb_y = bot_y + 52.0f * d;
  const float qb_h = 36.0f * d;
  const float qb_gap = 10.0f * d;
  const float qb_w = (left_w - 48.0f * d - 2.0f * qb_gap) / 3.0f;
  draw_button(l, hits, st, "act:open_play", x0 + 24.0f * d, qb_y, x0 + 24.0f * d + qb_w, qb_y + qb_h,
              "Open Play Folder", d);
  draw_button(l, hits, st, "act:portable", x0 + 24.0f * d + qb_w + qb_gap, qb_y,
              x0 + 24.0f * d + 2.0f * qb_w + qb_gap, qb_y + qb_h,
              st.paths.portable ? "Portable: On" : "Portable: Off", d, {.selected = st.paths.portable});
  draw_button(l, hits, st, "act:steam", x0 + 24.0f * d + 2.0f * (qb_w + qb_gap), qb_y,
              left_x1 - 24.0f * d, qb_y + qb_h, "Add to Steam", d);

  if (!st.paths.path_length_warning.empty()) {
    draw_wrapped_text(l, x0 + 24.0f * d, qb_y + qb_h + 16.0f * d, left_w - 48.0f * d, 18.0f * d, 2,
                      st.paths.path_length_warning, FontFace::kRegular, 12.0f * d, kAmber);
  } else {
    draw_text(l, x0 + 24.0f * d, qb_y + qb_h + 18.0f * d,
              st.paths.portable ? "Data is stored in a portable folder beside the launcher."
                                : "Data is stored in your standard user application folder.",
              FontFace::kRegular, 12.5f * d, kTextSecondary, ui::Align::kLeft, left_w - 48.0f * d);
    draw_text(l, x0 + 24.0f * d, qb_y + qb_h + 40.0f * d, st.paths.data_root.string(), FontFace::kMono,
              11.5f * d, kTextMuted, ui::Align::kLeft, left_w - 48.0f * d);
  }

  // Right: Installed Content Overview
  draw_card(l, right_x0, bot_y, x1, bot_y + bot_h, d);
  draw_text(l, right_x0 + 24.0f * d, bot_y + 18.0f * d, "Library Overview", FontFace::kSemiBold, 14.0f * d,
            kTextPrimary);

  int enabled_mods = 0;
  for (const auto& m : st.mods) {
    if (m.enabled) ++enabled_mods;
  }

  const float kv_x = right_x0 + 24.0f * d;
  const float kv_rx = x1 - 24.0f * d;
  const float kv_y0 = bot_y + 50.0f * d;
  const float kv_step = 26.0f * d;
  const float val_max_w = (x1 - right_x0) * 0.55f;

  auto kv_row = [&](int idx, std::string_view k, std::string_view v, uint32_t vc) {
    const float ry = kv_y0 + float(idx) * kv_step;
    draw_text(l, kv_x, ry, k, FontFace::kRegular, 12.5f * d, kTextSecondary);
    draw_text(l, kv_rx, ry, v, FontFace::kSemiBold, 12.5f * d, vc, ui::Align::kRight, val_max_w);
  };

  kv_row(0, "Title Update v4", st.content_status.has_v4 ? "Installed" : "Not installed",
         st.content_status.has_v4 ? kGreen : kTextMuted);
  const std::string dlc_val =
      std::to_string(st.content_status.dlc_installed_count) + " packs" +
      (st.content_status.has_udlc ? "  \xC2\xB7  UDLC" : "") +
      (st.content_status.has_xe ? "  \xC2\xB7  XE" : "");
  kv_row(1, "DLC & Expansions", dlc_val, kTextPrimary);
  kv_row(2, "Mods", std::to_string(enabled_mods) + " of " + std::to_string(st.mods.size()) + " enabled",
         enabled_mods > 0 ? kTextPrimary : kTextMuted);
  kv_row(3, "Android Wi-Fi Sync",
         st.sync_active ? ("Active on :" + std::to_string(st.sync_port)) : "Stopped",
         st.sync_active ? kGreen : kTextMuted);
}

// ---- Tab 2: Setup ----------------------------------------------------------

void draw_setup_tab(ui::List& l, const LauncherUiState& st, float x0, float y0, float x1, float y1, float d,
                    std::vector<HitBox>& hits) {
  const float cw = x1 - x0;
  draw_page_header(l, x0, x1, y0, "Setup", d);
  const float top_y = y0 + 50.0f * d;

  // Step 1: Disc Source (.iso or Extracted Folder)
  const float c1_h = 138.0f * d;
  draw_card(l, x0, top_y, x1, top_y + c1_h, d);
  draw_text(l, x0 + 20.0f * d, top_y + 16.0f * d, "1. Game Disc (.iso or Folder)", FontFace::kSemiBold, 14.0f * d,
            kTextPrimary);

  const float br_fold_w = 118.0f * d;
  const float br_iso_w = 104.0f * d;
  draw_button(l, hits, st, "setup:browse_iso", x1 - 20.0f * d - br_fold_w - 8.0f * d - br_iso_w, top_y + 10.0f * d,
              x1 - 20.0f * d - br_fold_w - 8.0f * d, top_y + 38.0f * d, "Browse .iso", d);
  draw_button(l, hits, st, "setup:browse_folder", x1 - 20.0f * d - br_fold_w, top_y + 10.0f * d, x1 - 20.0f * d,
              top_y + 38.0f * d, "Browse Folder", d);

  draw_text(l, x0 + 20.0f * d, top_y + 42.0f * d,
            "Drop a Forza Horizon .iso image or extracted disc folder onto this window, or paste a path.",
            FontFace::kRegular, 12.5f * d, kTextSecondary, ui::Align::kLeft, cw - 40.0f * d);

  const float row_y = top_y + 66.0f * d;
  const float row_h = 34.0f * d;
  const float btn_run_w = 122.0f * d;
  const float btn_ver_w = 92.0f * d;
  const float box_x1 = x1 - 20.0f * d - btn_run_w - btn_ver_w - 18.0f * d;

  const bool path_focused = st.focused_id == "setup:path";
  hits.push_back({"setup:path", x0 + 20.0f * d, row_y, box_x1, row_y + row_h, true});
  const uint32_t box_border =
      path_focused ? kAccent : (st.disc_check.valid ? kGreen : (st.hovered_id == "setup:path" ? kBorderHover : kBorderSubtle));
  draw_inset_box(l, x0 + 20.0f * d, row_y, box_x1, row_y + row_h, d, box_border);

  std::string path_disp = st.disc_path_input;
  if (path_focused) path_disp.push_back('|');
  const bool empty_disp = st.disc_path_input.empty() && !path_focused;
  draw_text(l, x0 + 32.0f * d, text_y_center(row_y, row_y + row_h, 12.5f * d),
            empty_disp ? "Drop .iso image or disc folder, or click to type/paste path..." : path_disp,
            empty_disp ? FontFace::kRegular : FontFace::kMono, 12.5f * d,
            empty_disp ? kTextMuted : kTextPrimary,
            ui::Align::kLeft, std::max(40.0f, box_x1 - x0 - 44.0f * d));

  draw_button(l, hits, st, "setup:verify", box_x1 + 9.0f * d, row_y, box_x1 + 9.0f * d + btn_ver_w, row_y + row_h,
              "Verify", d);
  if (st.setup_progress.running) {
    draw_button(l, hits, st, "setup:cancel", box_x1 + 18.0f * d + btn_ver_w, row_y, x1 - 20.0f * d, row_y + row_h,
                "Cancel", d);
  } else {
    draw_button(l, hits, st, "setup:run", box_x1 + 18.0f * d + btn_ver_w, row_y, x1 - 20.0f * d, row_y + row_h,
                "Run Setup", d, {.primary = true});
  }

  const uint32_t sum_col = st.disc_check.valid ? (st.disc_check.sufficient_space ? kGreen : kAmber) : kAmber;
  l.circle(x0 + 26.0f * d, row_y + row_h + 18.5f * d, 3.8f * d, sum_col);
  draw_text(l, x0 + 38.0f * d, row_y + row_h + 11.0f * d, st.disc_check.summary, FontFace::kRegular, 12.0f * d,
            sum_col, ui::Align::kLeft, cw - 58.0f * d);

  // Step 2: Validation Checklist
  const float c2_y = top_y + c1_h + 14.0f * d;
  const float c2_h = 152.0f * d;
  draw_card(l, x0, c2_y, x1, c2_y + c2_h, d);
  draw_text(l, x0 + 20.0f * d, c2_y + 16.0f * d, "2. Verification Checklist", FontFace::kSemiBold, 14.0f * d,
            kTextPrimary);
  if (st.disc_check.looks_like_iso && !st.disc_check.iso_format.empty()) {
    const std::string iso_meta =
        "Format: " + st.disc_check.iso_format + "  \xC2\xB7  " + std::to_string(st.disc_check.iso_file_count) +
        " files  \xC2\xB7  " + fmt_size(st.disc_check.required_space_bytes) + " space req.";
    draw_text(l, x1 - 20.0f * d, c2_y + 18.0f * d, iso_meta, FontFace::kMono, 11.5f * d,
              st.disc_check.valid && st.disc_check.sufficient_space ? kGreen : kAmber, ui::Align::kRight,
              cw * 0.62f);
  }

  const float col_w = (cw - 60.0f * d) * 0.5f;
  for (size_t i = 0; i < st.disc_check.items.size() && i < 6; ++i) {
    const auto& it = st.disc_check.items[i];
    const float cx = (i % 2 == 0) ? (x0 + 20.0f * d) : (x0 + 40.0f * d + col_w);
    const float ry = c2_y + 46.0f * d + float(i / 2) * 32.0f * d;
    const uint32_t dot = it.present ? kGreen : (it.required ? kRed : kTextMuted);
    l.circle(cx + 6.0f * d, ry + 8.0f * d, 4.0f * d, dot);

    std::string right_meta;
    if (!it.sha256_prefix.empty()) right_meta = "SHA-256 " + it.sha256_prefix;
    else if (it.present && it.bytes > 0) right_meta = fmt_size(it.bytes);
    else if (it.present && it.relative_path == "media/cars" && st.disc_check.car_archive_count > 0)
      right_meta = std::to_string(st.disc_check.car_archive_count) + " archives";
    else if (it.present) right_meta = "Found";
    else right_meta = it.required ? "Missing" : "Optional";

    const float meta_w = measure_text(FontFace::kMono, 11.5f * d, right_meta) + 12.0f * d;
    draw_text(l, cx + 18.0f * d, ry, it.label, FontFace::kRegular, 12.5f * d,
              it.present ? kTextPrimary : kTextSecondary, ui::Align::kLeft, std::max(60.0f, col_w - 22.0f * d - meta_w));
    draw_text(l, cx + col_w, ry + 1.0f * d, right_meta, FontFace::kMono, 11.5f * d,
              it.present ? kTextMuted : (it.required ? kRed : kTextMuted), ui::Align::kRight);
  }
  if (st.disc_check.items.empty()) {
    draw_text(l, x0 + 20.0f * d, c2_y + 52.0f * d, "Select a .iso image or disc folder to inspect required files.",
              FontFace::kRegular, 12.5f * d, kTextMuted);
  }

  // Step 3: Instant Setup Options, Progress & Collapsible Log Viewer
  const float c3_y = c2_y + c2_h + 14.0f * d;
  const float c3_h = std::max(140.0f * d, y1 - c3_y);
  draw_card(l, x0, c3_y, x1, c3_y + c3_h, d);

  draw_text(l, x0 + 20.0f * d, c3_y + 18.0f * d, "3. Instant Setup & Log", FontFace::kSemiBold, 14.0f * d, kTextPrimary);

  // Right-aligned options & log toggle
  const float tog_w = 94.0f * d;
  draw_button(l, hits, st, "setup:toggle_log", x1 - 20.0f * d - tog_w, c3_y + 12.0f * d, x1 - 20.0f * d,
              c3_y + 40.0f * d, st.setup_log_expanded ? "Hide Log" : "Show Log", d);
  draw_checkbox(l, hits, st, "setup:toggle_index", x1 - 20.0f * d - tog_w - 176.0f * d, c3_y + 12.0f * d,
                168.0f * d, 28.0f * d, "Build world index", st.setup_build_index, d);

  const float mode_y = c3_y + 42.0f * d;
  l.circle(x0 + 26.0f * d, mode_y + 7.5f * d, 3.6f * d, kGreen);
  draw_text(l, x0 + 38.0f * d, mode_y,
            "Direct-from-disc streaming \xE2\x80\x94 no 5.6 GB car bake required",
            FontFace::kRegular, 12.0f * d, kTextSecondary, ui::Align::kLeft, cw - 58.0f * d);

  const float prog_y = c3_y + 66.0f * d;
  const std::string stg = st.setup_progress.stage_label.empty() ? "Idle" : st.setup_progress.stage_label;
  draw_text(l, x0 + 20.0f * d, prog_y, stg, FontFace::kRegular, 12.5f * d, kTextPrimary, ui::Align::kLeft,
            cw - 110.0f * d);
  char pct[16];
  std::snprintf(pct, sizeof pct, "%d%%", int(std::lround(std::clamp(st.setup_progress.fraction, 0.0f, 1.0f) * 100.0f)));
  draw_text(l, x1 - 20.0f * d, prog_y, pct, FontFace::kMono, 12.5f * d, kTextSecondary, ui::Align::kRight);
  draw_progress_bar(l, x0 + 20.0f * d, prog_y + 22.0f * d, cw - 40.0f * d, 8.0f * d,
                    st.setup_progress.fraction, st.setup_progress.success ? kGreen : kAccent, d);

  const float log_y0 = prog_y + 40.0f * d;
  const float log_y1 = c3_y + c3_h - 16.0f * d;
  if (st.setup_log_expanded && log_y1 - log_y0 > 36.0f * d) {
    draw_inset_box(l, x0 + 20.0f * d, log_y0, x1 - 20.0f * d, log_y1, d);
    const float line_h = 18.0f * d;
    const int vis = std::max(1, int((log_y1 - log_y0 - 16.0f * d) / line_h));
    const int total = int(st.setup_progress.log_lines.size());
    const int max_start = std::max(0, total - vis);
    const int start = std::clamp(max_start - st.setup_log_scroll, 0, max_start);
    for (int i = 0; i < vis && start + i < total; ++i) {
      draw_text(l, x0 + 32.0f * d, log_y0 + 8.0f * d + float(i) * line_h,
                st.setup_progress.log_lines[size_t(start + i)], FontFace::kMono, 11.5f * d, kTextSecondary,
                ui::Align::kLeft, cw - 74.0f * d);
    }
    if (total == 0) {
      draw_text(l, x0 + 32.0f * d, log_y0 + 10.0f * d, "No setup output yet.", FontFace::kMono, 11.5f * d, kTextMuted);
    }
    draw_scrollbar(l, x1 - 30.0f * d, log_y0 + 6.0f * d, log_y1 - 6.0f * d, start, vis, total, d);
  } else {
    draw_text(l, x0 + 20.0f * d, log_y0 + 4.0f * d,
              std::to_string(st.setup_progress.log_lines.size()) + " log lines (click Show Log to expand).",
              FontFace::kRegular, 12.0f * d, kTextMuted);
  }
}

// ---- Tab 3: Content (DLC & Title Update) -----------------------------------

void draw_dlc_tab(ui::List& l, const LauncherUiState& st, float x0, float y0, float x1, float y1, float d,
                  std::vector<HitBox>& hits) {
  const float cw = x1 - x0;
  draw_page_header(l, x0, x1, y0, "Content", d);
  const float btn_stfs_w = 116.0f * d;
  const float btn_fold_w = 116.0f * d;
  const float btn_scan_w = 84.0f * d;
  const float btn_gap = 8.0f * d;
  draw_button(l, hits, st, "dlc:rescan", x1 - btn_stfs_w - btn_fold_w - btn_scan_w - 2.0f * btn_gap, y0 - 2.0f * d,
              x1 - btn_stfs_w - btn_fold_w - 2.0f * btn_gap, y0 + 28.0f * d, "Rescan", d);
  draw_button(l, hits, st, "dlc:import_folder", x1 - btn_stfs_w - btn_fold_w - btn_gap, y0 - 2.0f * d,
              x1 - btn_stfs_w - btn_gap, y0 + 28.0f * d, "Import Folder", d);
  draw_button(l, hits, st, "dlc:import", x1 - btn_stfs_w, y0 - 2.0f * d, x1, y0 + 28.0f * d, "Import STFS", d,
              {.primary = true});

  const float top_y = y0 + 50.0f * d;

  // Summary / Live STFS Install Banner
  const float ban_h = st.content_install.running ? 66.0f * d : 44.0f * d;
  draw_card(l, x0, top_y, x1, top_y + ban_h, d);
  if (st.content_install.running) {
    const float cancel_w = 88.0f * d;
    draw_text(l, x0 + 18.0f * d, top_y + 12.0f * d, st.content_install.status_label, FontFace::kRegular, 12.5f * d,
              kTextPrimary, ui::Align::kLeft, cw - cancel_w - 92.0f * d);
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%",
                  int(std::lround(std::clamp(st.content_install.fraction, 0.0f, 1.0f) * 100.0f)));
    draw_text(l, x1 - cancel_w - 28.0f * d, top_y + 12.0f * d, pct, FontFace::kMono, 12.0f * d, kTextSecondary,
              ui::Align::kRight);
    draw_progress_bar(l, x0 + 18.0f * d, top_y + 38.0f * d, cw - cancel_w - 46.0f * d, 8.0f * d,
                      st.content_install.fraction, kAccent, d);
    draw_button(l, hits, st, "dlc:cancel", x1 - 18.0f * d - cancel_w, top_y + 17.0f * d, x1 - 18.0f * d,
                top_y + 49.0f * d, "Cancel", d);
  } else {
    draw_text(l, x0 + 18.0f * d, text_y_center(top_y, top_y + ban_h, 13.0f * d), st.content_status.notice,
              FontFace::kRegular, 13.0f * d, kTextPrimary, ui::Align::kLeft, cw - 36.0f * d);
  }

  // Tabular package list
  const float tbl_y = top_y + ban_h + 14.0f * d;
  const float tbl_h = std::max(200.0f * d, y1 - tbl_y);
  draw_card(l, x0, tbl_y, x1, tbl_y + tbl_h, d);

  const float hdr_h = 34.0f * d;
  l.rect(x0 + 1.0f * d, tbl_y + 1.0f * d, x1 - 1.0f * d, tbl_y + hdr_h, kBgInset, 5.0f * d);
  draw_divider(l, x0 + 1.0f * d, tbl_y + hdr_h, x1 - 1.0f * d, d);
  const float col2_x = x0 + cw * 0.44f;
  const float col3_x = x1 - 24.0f * d;
  draw_text(l, x0 + 24.0f * d, text_y_center(tbl_y, tbl_y + hdr_h, 12.0f * d), "Package", FontFace::kSemiBold,
            12.0f * d, kTextMuted);
  draw_text(l, col2_x, text_y_center(tbl_y, tbl_y + hdr_h, 12.0f * d), "Details", FontFace::kSemiBold, 12.0f * d,
            kTextMuted);
  draw_text(l, col3_x, text_y_center(tbl_y, tbl_y + hdr_h, 12.0f * d), "Status", FontFace::kSemiBold, 12.0f * d,
            kTextMuted, ui::Align::kRight);

  const float rows_y0 = tbl_y + hdr_h + 4.0f * d;
  const float rows_y1 = tbl_y + tbl_h - 38.0f * d;
  const float row_h = 44.0f * d;
  const int vis = std::max(1, int((rows_y1 - rows_y0) / row_h));
  const int total = int(st.content_status.packages.size());
  const int start = std::clamp(st.content_scroll, 0, std::max(0, total - vis));

  for (int i = 0; i < vis && start + i < total; ++i) {
    const auto& pkg = st.content_status.packages[size_t(start + i)];
    const float ry = rows_y0 + float(i) * row_h;
    if (i % 2 == 1) l.rect(x0 + 2.0f * d, ry, x1 - 14.0f * d, ry + row_h, ui::rgba(255, 255, 255, 5));

    const uint32_t dot = pkg.installed ? (pkg.enabled ? kGreen : kAmber) : kTextMuted;
    l.circle(x0 + 28.0f * d, ry + 0.5f * row_h, 4.0f * d, dot);
    draw_text(l, x0 + 42.0f * d, text_y_center(ry, ry + row_h, 13.5f * d), pkg.title, FontFace::kSemiBold,
              13.5f * d, pkg.installed ? kTextPrimary : kTextSecondary, ui::Align::kLeft, col2_x - x0 - 54.0f * d);
    draw_text(l, col2_x, text_y_center(ry, ry + row_h, 12.5f * d),
              pkg.installed ? pkg.detail : "Drop STFS container or extracted folder", FontFace::kRegular,
              12.5f * d, pkg.installed ? kTextSecondary : kTextMuted, ui::Align::kLeft, col3_x - col2_x - 104.0f * d);
    const char* status_lbl = pkg.installed ? (pkg.enabled ? "Mounted" : "Disabled") : "Not installed";
    draw_text(l, col3_x, text_y_center(ry, ry + row_h, 12.5f * d), status_lbl, FontFace::kSemiBold, 12.5f * d,
              pkg.installed ? (pkg.enabled ? kGreen : kAmber) : kTextMuted, ui::Align::kRight);
  }
  draw_scrollbar(l, x1 - 10.0f * d, rows_y0 + 4.0f * d, rows_y1 - 4.0f * d, start, vis, total, d);

  draw_divider(l, x0 + 1.0f * d, rows_y1 + 2.0f * d, x1 - 1.0f * d, d);
  draw_text(l, x0 + 20.0f * d, text_y_center(rows_y1 + 2.0f * d, tbl_y + tbl_h, 12.0f * d),
            "Drag and drop Xbox 360 STFS packages (CON / LIVE / PIRS) for Title Update v4, Rally, 1000 Club, or DLC — or click Import STFS.",
            FontFace::kRegular, 12.0f * d, kTextMuted, ui::Align::kLeft, cw - 40.0f * d);
}

// ---- Tab 4: Mods -----------------------------------------------------------

void draw_mods_tab(ui::List& l, const LauncherUiState& st, float x0, float y0, float x1, float y1, float d,
                   std::vector<HitBox>& hits) {
  const float cw = x1 - x0;
  draw_page_header(l, x0, x1, y0, "Mods", d);
  draw_button(l, hits, st, "mods:rescan", x1 - 224.0f * d, y0 - 2.0f * d, x1 - 126.0f * d, y0 + 28.0f * d, "Rescan", d);
  draw_button(l, hits, st, "mods:install", x1 - 116.0f * d, y0 - 2.0f * d, x1, y0 + 28.0f * d, "Install", d,
              {.primary = true});

  const float top_y = y0 + 50.0f * d;

  // Calm, clear Save Isolation banner
  const bool iso_normal = st.save_iso.kind == "own";
  const bool iso_modded = st.save_iso.kind == "modded";
  const uint32_t iso_col = iso_normal ? kGreen : (iso_modded ? kAmber : kAccent);
  const uint32_t iso_bg = iso_normal ? kGreenSoft : (iso_modded ? kAmberSoft : kAccentSoft);
  const float ban_h = 44.0f * d;
  l.rect(x0, top_y, x1, top_y + ban_h, iso_bg, 6.0f * d);
  l.rect(x0, top_y + 6.0f * d, x0 + 3.5f * d, top_y + ban_h - 6.0f * d, iso_col, 1.5f * d);
  l.frame(x0, top_y, x1, top_y + ban_h, ui::fade(iso_col, 0.45), 1.0f * d, 6.0f * d);
  const std::string iso_prefix = "Save profile (" + st.save_iso.profile + "):  ";
  const float pref_w = draw_text(l, x0 + 18.0f * d, text_y_center(top_y, top_y + ban_h, 12.5f * d), iso_prefix,
                                 FontFace::kSemiBold, 12.5f * d, iso_col);
  draw_text(l, x0 + 18.0f * d + pref_w, text_y_center(top_y, top_y + ban_h, 12.5f * d), st.save_iso.warning_banner,
            FontFace::kRegular, 12.5f * d, kTextPrimary, ui::Align::kLeft, std::max(80.0f, cw - 36.0f * d - pref_w));

  // Two-pane layout
  const float pane_y = top_y + ban_h + 14.0f * d;
  const float pane_h = std::max(240.0f * d, y1 - pane_y);
  const float left_w = std::floor((cw - 16.0f * d) * 0.57f);
  const float left_x1 = x0 + left_w;
  const float right_x0 = left_x1 + 16.0f * d;

  // Left: Mod Load Order Table
  draw_card(l, x0, pane_y, left_x1, pane_y + pane_h, d);
  draw_text(l, x0 + 20.0f * d, pane_y + 16.0f * d, "Load Order", FontFace::kSemiBold, 14.0f * d, kTextPrimary);
  draw_button(l, hits, st, "mod:up", left_x1 - 170.0f * d, pane_y + 10.0f * d, left_x1 - 96.0f * d, pane_y + 38.0f * d,
              "Move Up", d);
  draw_button(l, hits, st, "mod:down", left_x1 - 88.0f * d, pane_y + 10.0f * d, left_x1 - 14.0f * d, pane_y + 38.0f * d,
              "Down", d);
  draw_divider(l, x0 + 1.0f * d, pane_y + 48.0f * d, left_x1 - 1.0f * d, d);

  const float list_y0 = pane_y + 54.0f * d;
  const float list_y1 = pane_y + pane_h - 36.0f * d;
  const float row_h = 44.0f * d;
  const int vis = std::max(1, int((list_y1 - list_y0) / row_h));
  const int total = int(st.mods.size());
  const int start = std::clamp(st.mods_scroll, 0, std::max(0, total - vis));

  for (int i = 0; i < vis && start + i < total; ++i) {
    const size_t idx = size_t(start + i);
    const auto& m = st.mods[idx];
    const float ry = list_y0 + float(i) * row_h;
    const bool sel = int(idx) == st.selected_mod;
    const std::string row_id = "mod:sel:" + std::to_string(idx);
    const std::string tog_id = "mod:tog:" + std::to_string(idx);
    const bool row_hov = st.hovered_id == row_id;

    if (sel) {
      l.rect(x0 + 6.0f * d, ry + 2.0f * d, left_x1 - 12.0f * d, ry + row_h - 2.0f * d, kBgSelected, 5.0f * d);
      l.rect(x0 + 6.0f * d, ry + 8.0f * d, x0 + 9.0f * d, ry + row_h - 8.0f * d, kAccent, 1.5f * d);
    } else if (row_hov) {
      l.rect(x0 + 6.0f * d, ry + 2.0f * d, left_x1 - 12.0f * d, ry + row_h - 2.0f * d, kBgHover, 5.0f * d);
    }

    // Checkbox hit zone (left)
    const float chk_x = x0 + 16.0f * d;
    const float chk_sz = 16.0f * d;
    const float chk_y = ry + 0.5f * (row_h - chk_sz);
    hits.push_back({tog_id, chk_x - 4.0f * d, ry + 4.0f * d, chk_x + chk_sz + 6.0f * d, ry + row_h - 4.0f * d, true});
    l.rect(chk_x, chk_y, chk_x + chk_sz, chk_y + chk_sz, m.enabled ? kAccent : kBgInset, 3.0f * d);
    l.frame(chk_x, chk_y, chk_x + chk_sz, chk_y + chk_sz,
            m.enabled ? kAccent : (st.hovered_id == tog_id ? kBorderHover : kBorderCard), 1.2f * d, 3.0f * d);
    if (m.enabled) {
      l.line(chk_x + 3.8f * d, chk_y + 8.4f * d, chk_x + 6.8f * d, chk_y + 11.6f * d, 1.8f * d, ui::rgba(255, 255, 255));
      l.line(chk_x + 6.8f * d, chk_y + 11.6f * d, chk_x + 12.4f * d, chk_y + 4.8f * d, 1.8f * d, ui::rgba(255, 255, 255));
    }

    // Row select hit zone (rest of row)
    hits.push_back({row_id, chk_x + chk_sz + 8.0f * d, ry + 2.0f * d, left_x1 - 12.0f * d, ry + row_h - 2.0f * d, true});

    // Order index + Title + Content tag
    draw_text(l, chk_x + chk_sz + 14.0f * d, text_y_center(ry, ry + row_h, 12.0f * d),
              std::to_string(idx + 1), FontFace::kMono, 12.0f * d, kTextMuted);

    std::string tag = !m.problems.empty() ? "Conflict" : (m.contents.empty() ? "Override" : m.contents[0]);
    const float tag_w = measure_text(FontFace::kRegular, 11.5f * d, tag) + 18.0f * d;
    draw_text(l, chk_x + chk_sz + 36.0f * d, text_y_center(ry, ry + row_h, 13.5f * d), m.title,
              sel ? FontFace::kSemiBold : FontFace::kRegular, 13.5f * d,
              m.enabled ? kTextPrimary : kTextMuted, ui::Align::kLeft,
              std::max(80.0f, left_w - (chk_sz + 74.0f * d) - tag_w));
    draw_text(l, left_x1 - 22.0f * d, text_y_center(ry, ry + row_h, 11.5f * d), tag, FontFace::kRegular, 11.5f * d,
              !m.problems.empty() ? kRed : kTextMuted, ui::Align::kRight);
  }

  if (st.mods.empty()) {
    draw_text(l, x0 + 20.0f * d, list_y0 + 14.0f * d, "No mods installed.", FontFace::kRegular, 13.0f * d, kTextMuted);
  }
  draw_scrollbar(l, left_x1 - 9.0f * d, list_y0 + 4.0f * d, list_y1 - 4.0f * d, start, vis, total, d);

  draw_divider(l, x0 + 1.0f * d, list_y1 + 2.0f * d, left_x1 - 1.0f * d, d);
  draw_text(l, x0 + 18.0f * d, text_y_center(list_y1 + 2.0f * d, pane_y + pane_h, 11.5f * d),
            "Drag and drop a mod folder or .zip archive onto this window to install.",
            FontFace::kRegular, 11.5f * d, kTextMuted, ui::Align::kLeft, left_w - 36.0f * d);

  // Right: Mod Inspector
  const float rw = x1 - right_x0;
  draw_card(l, right_x0, pane_y, x1, pane_y + pane_h, d);
  draw_text(l, right_x0 + 20.0f * d, pane_y + 16.0f * d, "Details", FontFace::kSemiBold, 14.0f * d, kTextPrimary);
  draw_divider(l, right_x0 + 1.0f * d, pane_y + 48.0f * d, x1 - 1.0f * d, d);

  if (st.selected_mod >= 0 && size_t(st.selected_mod) < st.mods.size()) {
    const auto& m = st.mods[size_t(st.selected_mod)];
    float iy = pane_y + 62.0f * d;
    draw_text(l, right_x0 + 20.0f * d, iy, m.title, FontFace::kSemiBold, 17.0f * d, kTextPrimary, ui::Align::kLeft,
              rw - 40.0f * d);
    iy += 26.0f * d;

    const std::string meta = "v" + (m.version.empty() ? "1.0" : m.version) + "  \xC2\xB7  " +
                             (m.author.empty() ? "Community" : m.author) + "  \xC2\xB7  Priority " +
                             std::to_string(m.priority);
    draw_text(l, right_x0 + 20.0f * d, iy, meta, FontFace::kRegular, 12.0f * d, kTextSecondary, ui::Align::kLeft,
              rw - 40.0f * d);
    iy += 26.0f * d;

    const std::string desc = m.description.empty() ? "No description provided." : m.description;
    iy += draw_wrapped_text(l, right_x0 + 20.0f * d, iy, rw - 40.0f * d, 18.0f * d, 3, desc, FontFace::kRegular,
                            12.5f * d, kTextPrimary);
    iy += 12.0f * d;
    draw_divider(l, right_x0 + 20.0f * d, iy, x1 - 20.0f * d, d);
    iy += 14.0f * d;

    std::string tags;
    for (size_t ti = 0; ti < m.contents.size(); ++ti) {
      if (ti > 0) tags += ", ";
      tags += m.contents[ti];
    }
    if (tags.empty()) tags = "Overrides";
    draw_text(l, right_x0 + 20.0f * d, iy, "Contents", FontFace::kRegular, 12.0f * d, kTextMuted);
    draw_text(l, x1 - 20.0f * d, iy, tags + " (" + std::to_string(m.file_count) + " files)", FontFace::kRegular,
              12.0f * d, kTextPrimary, ui::Align::kRight, rw * 0.62f);
    iy += 24.0f * d;

    const std::string save_s = !m.profile.empty() ? ("Own profile (" + m.profile + ")")
                                                  : (m.shares_save ? "Shares main save" : "Isolated (user-modded)");
    draw_text(l, right_x0 + 20.0f * d, iy, "Save behavior", FontFace::kRegular, 12.0f * d, kTextMuted);
    draw_text(l, x1 - 20.0f * d, iy, save_s, FontFace::kSemiBold, 12.0f * d,
              m.shares_save && m.profile.empty() ? kGreen : kAmber, ui::Align::kRight, rw * 0.62f);
    iy += 28.0f * d;

    if (!m.problems.empty()) {
      l.rect(right_x0 + 20.0f * d, iy, x1 - 20.0f * d, iy + 34.0f * d, kRedSoft, 5.0f * d);
      l.frame(right_x0 + 20.0f * d, iy, x1 - 20.0f * d, iy + 34.0f * d, kRed, 1.0f * d, 5.0f * d);
      draw_text(l, right_x0 + 30.0f * d, text_y_center(iy, iy + 34.0f * d, 12.0f * d), m.problems[0],
                FontFace::kRegular, 12.0f * d, kRed, ui::Align::kLeft, rw - 60.0f * d);
      iy += 44.0f * d;
    }

    draw_divider(l, right_x0 + 20.0f * d, iy, x1 - 20.0f * d, d);
    iy += 14.0f * d;
    draw_text(l, right_x0 + 20.0f * d, iy, "Options", FontFace::kSemiBold, 13.0f * d, kTextPrimary);
    iy += 26.0f * d;

    for (size_t oi = 0; oi < m.options.size() && oi < 4; ++oi) {
      if (iy + 32.0f * d > pane_y + pane_h - 12.0f * d) break;
      const auto& opt = m.options[oi];
      const std::string oid = "mod:opt:" + std::to_string(oi);
      draw_text(l, right_x0 + 20.0f * d, text_y_center(iy, iy + 30.0f * d, 12.5f * d), opt.title,
                FontFace::kRegular, 12.5f * d, kTextSecondary, ui::Align::kLeft, rw * 0.50f);
      draw_button(l, hits, st, oid, x1 - 156.0f * d, iy, x1 - 20.0f * d, iy + 30.0f * d,
                  opt.value.empty() ? "Default" : opt.value, d);
      iy += 38.0f * d;
    }
    if (m.options.empty()) {
      draw_text(l, right_x0 + 20.0f * d, iy, "No configurable options.", FontFace::kRegular, 12.0f * d, kTextMuted);
    }
  } else {
    draw_text(l, right_x0 + 20.0f * d, pane_y + 66.0f * d, "Select a mod to view details.", FontFace::kRegular,
              13.0f * d, kTextMuted);
  }
}

// ---- Tab 5: Radio ----------------------------------------------------------

void draw_radio_tab(ui::List& l, const LauncherUiState& st, float x0, float y0, float x1, float y1, float d,
                    std::vector<HitBox>& hits) {
  const float cw = x1 - x0;
  draw_page_header(l, x0, x1, y0, "Radio", d);
  const float top_y = y0 + 50.0f * d;
  const float h = std::max(260.0f * d, y1 - top_y);
  const float left_w = std::floor((cw - 16.0f * d) * 0.49f);
  const float left_x1 = x0 + left_w;
  const float right_x0 = left_x1 + 16.0f * d;

  // Left: Builder Form
  draw_card(l, x0, top_y, left_x1, top_y + h, d);
  float fy = top_y + 18.0f * d;
  draw_text(l, x0 + 20.0f * d, fy, "Station Configuration", FontFace::kSemiBold, 14.0f * d, kTextPrimary);
  fy += 32.0f * d;

  draw_text(l, x0 + 20.0f * d, fy, "Target station", FontFace::kRegular, 12.0f * d, kTextSecondary);
  fy += 20.0f * d;
  static constexpr const char* kStLabels[3] = {"Bass Arena", "Horizon Pulse", "Horizon Rocks"};
  static constexpr const char* kStIds[3] = {"radio:slot:Radio1", "radio:slot:Radio2", "radio:slot:Radio3"};
  int st_idx = st.radio_cfg.station_slot == "Radio2" ? 1 : (st.radio_cfg.station_slot == "Radio3" ? 2 : 0);
  draw_segmented(l, hits, st, x0 + 20.0f * d, fy, left_x1 - 20.0f * d, fy + 34.0f * d, kStLabels, kStIds, 3, st_idx, d);
  fy += 48.0f * d;

  draw_text(l, x0 + 20.0f * d, fy, "Playlist mode", FontFace::kRegular, 12.0f * d, kTextSecondary);
  fy += 20.0f * d;
  static constexpr const char* kModeLabels[2] = {"Replace stock tracks", "Extend stock playlist"};
  static constexpr const char* kModeIds[2] = {"radio:mode:replace", "radio:mode:extend"};
  draw_segmented(l, hits, st, x0 + 20.0f * d, fy, left_x1 - 20.0f * d, fy + 34.0f * d, kModeLabels, kModeIds, 2,
                 st.radio_cfg.replace_playlist ? 0 : 1, d);
  fy += 48.0f * d;

  auto field_box = [&](std::string_view lbl, std::string_view val, bool mono, uint32_t col, uint32_t border = kBorderSubtle) {
    draw_text(l, x0 + 20.0f * d, fy, lbl, FontFace::kRegular, 12.0f * d, kTextSecondary);
    fy += 20.0f * d;
    draw_inset_box(l, x0 + 20.0f * d, fy, left_x1 - 20.0f * d, fy + 34.0f * d, d, border);
    draw_text(l, x0 + 32.0f * d, text_y_center(fy, fy + 34.0f * d, 12.5f * d), val,
              mono ? FontFace::kMono : FontFace::kRegular, 12.5f * d, col, ui::Align::kLeft, left_w - 64.0f * d);
    fy += 46.0f * d;
  };

  field_box("Music folder (MP3, WAV, FLAC, OGG)",
            st.radio_cfg.music_folder.empty() ? "Drop a music folder onto this window" : st.radio_cfg.music_folder.string(),
            !st.radio_cfg.music_folder.empty(),
            st.radio_cfg.music_folder.empty() ? kTextMuted : kTextPrimary);
  field_box("Station display name",
            st.radio_cfg.custom_station_name.empty() ? "Keep stock station name" : st.radio_cfg.custom_station_name,
            false, st.radio_cfg.custom_station_name.empty() ? kTextMuted : kTextPrimary);
  field_box("Station logo PNG (210\xC3\x97" "100)",
            st.radio_logo_check.message.empty() ? "Optional: drop a 210x100 PNG" : st.radio_logo_check.message,
            false, st.radio_logo_check.valid ? kGreen : kTextSecondary,
            st.radio_logo_check.valid ? kGreen : kBorderSubtle);

  const bool has_ff = !st.paths.ffmpeg_bin.empty();
  l.circle(x0 + 26.0f * d, fy + 8.0f * d, 4.0f * d, has_ff ? kGreen : kAmber);
  draw_text(l, x0 + 38.0f * d, fy,
            has_ff ? "FFmpeg detected (48 kHz, 160 kbps MP3)" : "FFmpeg not found on PATH",
            FontFace::kRegular, 12.5f * d, has_ff ? kTextSecondary : kAmber);

  draw_button(l, hits, st, "radio:build", x0 + 20.0f * d, top_y + h - 56.0f * d, left_x1 - 20.0f * d,
              top_y + h - 18.0f * d, "Build Radio Mod", d, {.primary = true});

  // Right: Preview & Playlist
  const float rw = x1 - right_x0;
  draw_card(l, right_x0, top_y, x1, top_y + h, d);
  draw_text(l, right_x0 + 20.0f * d, top_y + 18.0f * d,
            "Playlist (" + std::to_string(st.radio_tracks.size()) + " tracks)", FontFace::kSemiBold, 14.0f * d,
            kTextPrimary);

  // Logo preview box (210x100 scaled cleanly)
  const float logo_w = 180.0f * d;
  const float logo_h = 86.0f * d;
  const float lx = right_x0 + 20.0f * d;
  const float ly = top_y + 50.0f * d;
  draw_inset_box(l, lx, ly, lx + logo_w, ly + logo_h, d, st.radio_logo_check.valid ? kGreen : kBorderCard);
  draw_text(l, lx + 0.5f * logo_w, ly + 22.0f * d,
            st.radio_cfg.custom_station_name.empty() ? kStLabels[st_idx] : st.radio_cfg.custom_station_name,
            FontFace::kSemiBold, 13.0f * d, kTextPrimary, ui::Align::kCenter, logo_w - 16.0f * d);
  draw_text(l, lx + 0.5f * logo_w, ly + 46.0f * d,
            st.radio_logo_check.valid ? "210\xC3\x97" "100 PNG verified" : "210\xC3\x97" "100 PNG slot",
            FontFace::kRegular, 11.5f * d, st.radio_logo_check.valid ? kGreen : kTextMuted, ui::Align::kCenter);

  const float mx = lx + logo_w + 16.0f * d;
  const float mw = std::max(80.0f, x1 - 20.0f * d - mx);
  draw_text(l, mx, ly + 4.0f * d, "Output: mods/" + st.radio_cfg.mod_name, FontFace::kMono, 12.0f * d, kTextPrimary,
            ui::Align::kLeft, mw);
  draw_text(l, mx, ly + 28.0f * d, "Format: FSB4  \xC2\xB7  48 kHz  \xC2\xB7  160 kbps", FontFace::kRegular, 12.0f * d,
            kTextSecondary, ui::Align::kLeft, mw);
  draw_text(l, mx, ly + 50.0f * d, "Save: Compatible with main save", FontFace::kRegular, 12.0f * d, kGreen,
            ui::Align::kLeft, mw);

  // Track table
  const float ty0 = ly + logo_h + 18.0f * d;
  const float ty1 = top_y + h - 44.0f * d;
  draw_divider(l, right_x0 + 20.0f * d, ty0, x1 - 20.0f * d, d);
  const float tr_h = 34.0f * d;
  const int vis = std::max(1, int((ty1 - ty0 - 6.0f * d) / tr_h));
  const int total = int(st.radio_tracks.size());
  const int start = std::clamp(st.radio_scroll, 0, std::max(0, total - vis));

  for (int i = 0; i < vis && start + i < total; ++i) {
    const auto& t = st.radio_tracks[size_t(start + i)];
    const float ry = ty0 + 6.0f * d + float(i) * tr_h;
    if (i % 2 == 1) l.rect(right_x0 + 16.0f * d, ry, x1 - 16.0f * d, ry + tr_h, ui::rgba(255, 255, 255, 5), 4.0f * d);
    char nbuf[8];
    std::snprintf(nbuf, sizeof nbuf, "%02d", start + i + 1);
    draw_text(l, right_x0 + 24.0f * d, text_y_center(ry, ry + tr_h, 11.5f * d), nbuf, FontFace::kMono, 11.5f * d,
              kTextMuted);
    draw_text(l, right_x0 + 52.0f * d, text_y_center(ry, ry + tr_h, 12.5f * d), t.title, FontFace::kRegular,
              12.5f * d, kTextPrimary, ui::Align::kLeft, rw * 0.48f);
    draw_text(l, x1 - 24.0f * d, text_y_center(ry, ry + tr_h, 12.0f * d),
              t.artist.empty() ? "Unknown artist" : t.artist, FontFace::kRegular, 12.0f * d, kTextSecondary,
              ui::Align::kRight, rw * 0.34f);
  }
  draw_scrollbar(l, x1 - 10.0f * d, ty0 + 6.0f * d, ty1 - 4.0f * d, start, vis, total, d);

  if (!st.radio_status.empty()) {
    draw_text(l, right_x0 + 20.0f * d, top_y + h - 30.0f * d, st.radio_status, FontFace::kRegular, 12.0f * d,
              kTextSecondary, ui::Align::kLeft, rw - 40.0f * d);
  }
}

// ---- Tab 6: Controls (Wheel & Controller) ----------------------------------

void draw_wheel_tab(ui::List& l, const LauncherUiState& st, float x0, float y0, float x1, float y1, float d,
                    std::vector<HitBox>& hits) {
  const float cw = x1 - x0;
  draw_page_header(l, x0, x1, y0, "Controls", d);
  const float top_y = y0 + 50.0f * d;
  const float h = std::max(300.0f * d, y1 - top_y);
  const float left_w = std::floor((cw - 16.0f * d) * 0.46f);
  const float left_x1 = x0 + left_w;
  const float right_x0 = left_x1 + 16.0f * d;

  // Left: Devices & Force Feedback
  draw_card(l, x0, top_y, left_x1, top_y + h, d);
  draw_text(l, x0 + 20.0f * d, top_y + 16.0f * d, "Input Devices", FontFace::kSemiBold, 14.0f * d, kTextPrimary);

  const float dev_y0 = top_y + 46.0f * d;
  const float dev_h = 38.0f * d;
  const int max_devs = 4;
  const int dev_total = int(st.devices.size());
  const int dev_start = std::clamp(st.devices_scroll, 0, std::max(0, dev_total - max_devs));
  for (int i = 0; i < max_devs && dev_start + i < dev_total; ++i) {
    const size_t idx = size_t(dev_start + i);
    const auto& dev = st.devices[idx];
    const float ry = dev_y0 + float(i) * (dev_h + 4.0f * d);
    const bool sel = int(idx) == st.selected_device;
    const std::string did = "wheel:dev:" + std::to_string(idx);
    hits.push_back({did, x0 + 16.0f * d, ry, left_x1 - 16.0f * d, ry + dev_h, true});
    l.rect(x0 + 16.0f * d, ry, left_x1 - 16.0f * d, ry + dev_h,
           sel ? kBgSelected : (st.hovered_id == did ? kBgHover : kBgInset), 5.0f * d);
    if (sel) {
      l.rect(x0 + 16.0f * d, ry + 6.0f * d, x0 + 19.0f * d, ry + dev_h - 6.0f * d, kAccent, 1.5f * d);
      l.frame(x0 + 16.0f * d, ry, left_x1 - 16.0f * d, ry + dev_h, kBorderHover, 1.0f * d, 5.0f * d);
    }
    l.circle(x0 + 30.0f * d, ry + 0.5f * dev_h, 3.8f * d, dev.has_ffb ? kGreen : kBlue);
    const std::string spec = std::to_string(dev.num_axes) + " ax \xC2\xB7 " + std::to_string(dev.num_buttons) + " btn";
    const float spec_w = measure_text(FontFace::kMono, 11.0f * d, spec) + 14.0f * d;
    draw_text(l, x0 + 42.0f * d, text_y_center(ry, ry + dev_h, 12.5f * d), dev.name,
              sel ? FontFace::kSemiBold : FontFace::kRegular, 12.5f * d, kTextPrimary, ui::Align::kLeft,
              std::max(80.0f, left_w - 72.0f * d - spec_w));
    draw_text(l, left_x1 - 24.0f * d, text_y_center(ry, ry + dev_h, 11.0f * d), spec, FontFace::kMono, 11.0f * d,
              kTextMuted, ui::Align::kRight);
  }
  if (st.devices.empty()) {
    draw_text(l, x0 + 20.0f * d, dev_y0 + 10.0f * d, "No controller or wheel connected.", FontFace::kRegular,
              12.5f * d, kTextMuted);
  }

  float sy = dev_y0 + float(max_devs) * (dev_h + 4.0f * d) + 12.0f * d;
  draw_divider(l, x0 + 20.0f * d, sy, left_x1 - 20.0f * d, d);
  sy += 14.0f * d;
  draw_text(l, x0 + 20.0f * d, sy, "Wheel & Force Feedback", FontFace::kSemiBold, 14.0f * d, kTextPrimary);
  sy += 30.0f * d;

  // Separate USB + Steering lock
  const float lock_w = 138.0f * d;
  draw_checkbox(l, hits, st, "wheel:separate", x0 + 16.0f * d, sy, left_w - 44.0f * d - lock_w, 32.0f * d,
                "Separate USB devices", st.wheel_cfg.separate_devices, d);
  draw_button(l, hits, st, "wheel:degrees", left_x1 - 20.0f * d - lock_w, sy, left_x1 - 20.0f * d, sy + 32.0f * d,
              "Lock: " + std::to_string(st.wheel_cfg.steering_degrees) + "\xC2\xB0", d);
  sy += 46.0f * d;

  // FFB gain bar + controls
  draw_axis_bar(l, x0 + 20.0f * d, sy, left_w - 40.0f * d, 12.0f * d, "Force feedback gain", st.wheel_cfg.ffb_gain,
                false, kAccent, 0.0f, d);
  sy += 44.0f * d;

  const float bw = (left_w - 40.0f * d - 24.0f * d) / 4.0f;
  draw_button(l, hits, st, "wheel:ffb_minus", x0 + 20.0f * d, sy, x0 + 20.0f * d + bw, sy + 30.0f * d, "-5%", d);
  draw_button(l, hits, st, "wheel:ffb_plus", x0 + 20.0f * d + bw + 8.0f * d, sy, x0 + 20.0f * d + 2.0f * bw + 8.0f * d,
              sy + 30.0f * d, "+5%", d);
  draw_button(l, hits, st, "wheel:ffb_invert", x0 + 20.0f * d + 2.0f * (bw + 8.0f * d), sy,
              x0 + 20.0f * d + 3.0f * bw + 16.0f * d, sy + 30.0f * d,
              st.wheel_cfg.ffb_invert ? "Invert: On" : "Invert: Off", d, {.selected = st.wheel_cfg.ffb_invert});
  draw_button(l, hits, st, "wheel:ffb_test", x0 + 20.0f * d + 3.0f * (bw + 8.0f * d), sy, left_x1 - 20.0f * d,
              sy + 30.0f * d, "Test Pulse", d);

  draw_button(l, hits, st, "wheel:save", x0 + 20.0f * d, top_y + h - 76.0f * d, left_x1 - 20.0f * d,
              top_y + h - 40.0f * d, "Save Settings", d, {.primary = true});
  if (!st.wheel_status.empty()) {
    draw_text(l, x0 + 20.0f * d, top_y + h - 30.0f * d, st.wheel_status, FontFace::kRegular, 11.5f * d, kTextSecondary,
              ui::Align::kLeft, left_w - 40.0f * d);
  }

  // Right: Live Axis Bars + Deadzone + H-Shifter
  const float rw = x1 - right_x0;
  draw_card(l, right_x0, top_y, x1, top_y + h, d);
  draw_text(l, right_x0 + 20.0f * d, top_y + 16.0f * d, "Live Input", FontFace::kSemiBold, 14.0f * d, kTextPrimary);

  const float ax_x = right_x0 + 20.0f * d;
  const float ax_w = rw - 40.0f * d;
  float ay = top_y + 50.0f * d;
  const float ax_step = 50.0f * d;
  draw_axis_bar(l, ax_x, ay, ax_w, 14.0f * d,
                "Steering  (Axis " + std::to_string(std::max(0, st.wheel_cfg.steer.axis_index)) + ")",
                st.live_steer, true, kAccent, st.wheel_cfg.steer.deadzone_in, d);
  ay += ax_step;
  draw_axis_bar(l, ax_x, ay, ax_w, 14.0f * d,
                "Throttle  (Axis " + std::to_string(std::max(0, st.wheel_cfg.throttle.axis_index)) + ")",
                st.live_throttle, false, kGreen, st.wheel_cfg.throttle.deadzone_in, d);
  ay += ax_step;
  draw_axis_bar(l, ax_x, ay, ax_w, 14.0f * d,
                "Brake  (Axis " + std::to_string(std::max(0, st.wheel_cfg.brake.axis_index)) + ")",
                st.live_brake, false, kRed, st.wheel_cfg.brake.deadzone_in, d);
  ay += ax_step;
  draw_axis_bar(l, ax_x, ay, ax_w, 14.0f * d,
                "Clutch  (Axis " + std::to_string(std::max(0, st.wheel_cfg.clutch.axis_index)) + ")",
                st.live_clutch, false, kBlue, st.wheel_cfg.clutch.deadzone_in, d);
  ay += ax_step;
  draw_axis_bar(l, ax_x, ay, ax_w, 14.0f * d,
                "Handbrake  (Axis " + std::to_string(std::max(0, st.wheel_cfg.handbrake.axis_index)) + ")",
                st.live_handbrake, false, kAmber, st.wheel_cfg.handbrake.deadzone_in, d);
  ay += ax_step + 8.0f * d;

  draw_divider(l, ax_x, ay, x1 - 20.0f * d, d);
  ay += 14.0f * d;

  // Deadzone row
  char dz_buf[128];
  std::snprintf(dz_buf, sizeof dz_buf, "Steering deadzone: %.0f%% \xE2\x80\x93 %.0f%%   \xC2\xB7   Pedals: %.0f%% \xE2\x80\x93 %.0f%%",
                st.wheel_cfg.steer.deadzone_in * 100.0f, st.wheel_cfg.steer.deadzone_out * 100.0f,
                st.wheel_cfg.throttle.deadzone_in * 100.0f, st.wheel_cfg.throttle.deadzone_out * 100.0f);
  draw_text(l, ax_x, ay + 6.0f * d, dz_buf, FontFace::kRegular, 12.5f * d, kTextSecondary, ui::Align::kLeft,
            ax_w - 140.0f * d);
  draw_button(l, hits, st, "wheel:dz_down", x1 - 148.0f * d, ay, x1 - 88.0f * d, ay + 28.0f * d, "DZ -", d);
  draw_button(l, hits, st, "wheel:dz_up", x1 - 80.0f * d, ay, x1 - 20.0f * d, ay + 28.0f * d, "DZ +", d);
  ay += 44.0f * d;

  // H-Shifter gear cells
  draw_text(l, ax_x, ay, "H-Shifter", FontFace::kSemiBold, 13.0f * d, kTextPrimary);
  ay += 24.0f * d;
  static constexpr const char* kGears[7] = {"R", "1", "2", "3", "4", "5", "6"};
  const float cell_w = 46.0f * d;
  const float cell_h = 40.0f * d;
  for (int g = 0; g < 7; ++g) {
    const float gx = ax_x + float(g) * (cell_w + 8.0f * d);
    const bool active = (g == 0 && st.live_gear == -1) || (g > 0 && st.live_gear == g);
    l.rect(gx, ay, gx + cell_w, ay + cell_h, active ? kAccent : kBgInset, 5.0f * d);
    l.frame(gx, ay, gx + cell_w, ay + cell_h, active ? kAccent : kBorderCard, 1.0f * d, 5.0f * d);
    draw_text(l, gx + 0.5f * cell_w, text_y_center(ay, ay + cell_h, 15.0f * d), kGears[g], FontFace::kSemiBold,
              15.0f * d, active ? ui::rgba(255, 255, 255) : kTextSecondary, ui::Align::kCenter);
  }
}

// ---- Tab 7: Updates --------------------------------------------------------

void draw_update_tab(ui::List& l, const LauncherUiState& st, float x0, float y0, float x1, float y1, float d,
                     std::vector<HitBox>& hits) {
  const float cw = x1 - x0;
  draw_page_header(l, x0, x1, y0, "Updates", d);
  const float top_y = y0 + 50.0f * d;
  const float card_h = std::min(420.0f * d, std::max(260.0f * d, y1 - top_y));
  draw_card(l, x0, top_y, x1, top_y + card_h, d);

  float cy = top_y + 22.0f * d;
  draw_text(l, x0 + 24.0f * d, cy, "Installed version", FontFace::kRegular, 12.5f * d, kTextSecondary);
  draw_text(l, x0 + 180.0f * d, cy, "v" + st.current_version, FontFace::kSemiBold, 14.0f * d, kTextPrimary);
  cy += 34.0f * d;

  draw_text(l, x0 + 24.0f * d, cy + 7.0f * d, "Release channel", FontFace::kRegular, 12.5f * d, kTextSecondary);
  static constexpr const char* kChLabels[2] = {"Stable", "Preview"};
  static constexpr const char* kChIds[2] = {"upd:ch:stable", "upd:ch:preview"};
  draw_segmented(l, hits, st, x0 + 180.0f * d, cy, x0 + 380.0f * d, cy + 34.0f * d, kChLabels, kChIds, 2,
                 st.update_channel == "preview" ? 1 : 0, d);
  cy += 50.0f * d;

  draw_text(l, x0 + 24.0f * d, cy, "Status", FontFace::kRegular, 12.5f * d, kTextSecondary);
  draw_text(l, x0 + 180.0f * d, cy, st.update_status, FontFace::kRegular, 13.0f * d, kTextPrimary, ui::Align::kLeft,
            cw - 204.0f * d);
  cy += 36.0f * d;

  draw_button(l, hits, st, "upd:check", x0 + 24.0f * d, cy, x0 + 196.0f * d, cy + 36.0f * d, "Check for Updates", d,
              {.primary = true});
  cy += 56.0f * d;

  if (st.available_update) {
    const auto& u = *st.available_update;
    draw_inset_box(l, x0 + 24.0f * d, cy, x1 - 24.0f * d, cy + 156.0f * d, d, kBlue);
    draw_text(l, x0 + 40.0f * d, cy + 16.0f * d, "Available: " + u.tag + "  \xC2\xB7  " + u.asset_name,
              FontFace::kSemiBold, 15.0f * d, kTextPrimary, ui::Align::kLeft, cw - 80.0f * d);
    draw_text(l, x0 + 40.0f * d, cy + 44.0f * d, "Size: " + fmt_size(u.size_bytes), FontFace::kRegular, 12.5f * d,
              kTextSecondary);
    draw_text(l, x0 + 40.0f * d, cy + 68.0f * d, "SHA-256: " + u.sha256, FontFace::kMono, 11.5f * d, kGreen,
              ui::Align::kLeft, cw - 80.0f * d);
    draw_button(l, hits, st, "upd:stage", x0 + 40.0f * d, cy + 102.0f * d, x0 + 236.0f * d, cy + 136.0f * d,
                "Stage & Verify Update", d, {.primary = true});
  }
}

// ---- Tab 8: Android Wi-Fi Sync ---------------------------------------------

void draw_android_tab(ui::List& l, const LauncherUiState& st, float x0, float y0, float x1, float y1, float d,
                      std::vector<HitBox>& hits) {
  const float cw = x1 - x0;
  draw_page_header(l, x0, x1, y0, "Android Wi-Fi Sync", d);
  const float top_y = y0 + 50.0f * d;
  const float h = std::max(320.0f * d, y1 - top_y);
  const float left_w = std::floor((cw - 16.0f * d) * 0.59f);
  const float left_x1 = x0 + left_w;
  const float right_x0 = left_x1 + 16.0f * d;

  // Left: Pairing, Sync Groups, Activity Log
  draw_card(l, x0, top_y, left_x1, top_y + h, d);

  // Pairing & LAN box
  const float py = top_y + 18.0f * d;
  const float ph = 86.0f * d;
  draw_inset_box(l, x0 + 20.0f * d, py, left_x1 - 20.0f * d, py + ph, d, st.sync_active ? kGreen : kBorderSubtle);
  draw_text(l, x0 + 36.0f * d, py + 14.0f * d, "Pairing code", FontFace::kRegular, 12.0f * d, kTextSecondary);
  const std::string code_disp =
      st.sync_manifest.pairing_code.size() == 6
          ? (st.sync_manifest.pairing_code.substr(0, 3) + " " + st.sync_manifest.pairing_code.substr(3))
          : "--- ---";
  draw_text(l, x0 + 36.0f * d, py + 34.0f * d, code_disp, FontFace::kMono, 30.0f * d, kTextPrimary);

  const float rx = x0 + left_w * 0.48f;
  draw_text(l, rx, py + 14.0f * d, "Local address", FontFace::kRegular, 12.0f * d, kTextSecondary);
  draw_text(l, rx, py + 34.0f * d, st.sync_lan_ip + ":" + std::to_string(st.sync_port), FontFace::kMono, 15.5f * d,
            kTextPrimary, ui::Align::kLeft, left_x1 - rx - 32.0f * d);
  l.circle(rx + 4.5f * d, py + 65.5f * d, 3.8f * d, st.sync_active ? kGreen : kTextMuted);
  draw_text(l, rx + 14.0f * d, py + 58.0f * d,
            st.sync_active ? "Listening on local Wi-Fi" : "Server stopped",
            FontFace::kRegular, 12.0f * d, st.sync_active ? kGreen : kTextMuted);

  // Sync group checkboxes (2x2)
  float gy = py + ph + 18.0f * d;
  draw_text(l, x0 + 20.0f * d, gy, "Sync to Android device", FontFace::kSemiBold, 13.0f * d, kTextPrimary);
  gy += 24.0f * d;
  const float chk_w = (left_w - 48.0f * d) * 0.5f;
  draw_checkbox(l, hits, st, "sync:play", x0 + 16.0f * d, gy, chk_w, 30.0f * d, "Play folder", st.sync_include_play, d);
  draw_checkbox(l, hits, st, "sync:dlc", x0 + 24.0f * d + chk_w, gy, chk_w, 30.0f * d, "DLC & Title Update v4",
                st.sync_include_dlc, d);
  gy += 32.0f * d;
  draw_checkbox(l, hits, st, "sync:mods", x0 + 16.0f * d, gy, chk_w, 30.0f * d, "Enabled mods", st.sync_include_mods, d);
  draw_checkbox(l, hits, st, "sync:radio", x0 + 24.0f * d + chk_w, gy, chk_w, 30.0f * d, "Custom radio stations",
                st.sync_include_radio, d);
  gy += 44.0f * d;

  draw_button(l, hits, st, "sync:toggle", x0 + 20.0f * d, gy, left_x1 - 20.0f * d, gy + 38.0f * d,
              st.sync_active ? "Stop Sync Server" : "Start Sync Server", d, {.primary = !st.sync_active});
  gy += 52.0f * d;

  // Transfer activity log
  draw_text(l, x0 + 20.0f * d, gy, "Activity Log", FontFace::kSemiBold, 12.5f * d, kTextSecondary);
  gy += 22.0f * d;
  const float log_y1 = top_y + h - 18.0f * d;
  if (log_y1 - gy > 36.0f * d) {
    draw_inset_box(l, x0 + 20.0f * d, gy, left_x1 - 20.0f * d, log_y1, d);
    const float line_h = 19.0f * d;
    const int vis = std::max(1, int((log_y1 - gy - 14.0f * d) / line_h));
    const int total = int(st.sync_activity.size());
    const int max_start = std::max(0, total - vis);
    const int start = std::clamp(max_start - st.sync_log_scroll, 0, max_start);
    for (int i = 0; i < vis && start + i < total; ++i) {
      draw_text(l, x0 + 32.0f * d, gy + 8.0f * d + float(i) * line_h, st.sync_activity[size_t(start + i)],
                FontFace::kMono, 11.5f * d, kTextSecondary, ui::Align::kLeft, left_w - 72.0f * d);
    }
    if (total == 0) {
      draw_text(l, x0 + 32.0f * d, gy + 10.0f * d, "No connections yet.", FontFace::kMono, 11.5f * d, kTextMuted);
    }
    draw_scrollbar(l, left_x1 - 30.0f * d, gy + 6.0f * d, log_y1 - 6.0f * d, start, vis, total, d);
  }

  // Right: QR Code & Steps
  const float rw = x1 - right_x0;
  draw_card(l, right_x0, top_y, x1, top_y + h, d);
  draw_text(l, right_x0 + 20.0f * d, top_y + 18.0f * d, "Scan on Android", FontFace::kSemiBold, 14.0f * d,
            kTextPrimary);

  const float qr_box = std::min(232.0f * d, rw - 64.0f * d);
  const float qx = right_x0 + 0.5f * (rw - qr_box);
  const float qy = top_y + 52.0f * d;
  l.rect(qx, qy, qx + qr_box, qy + qr_box, ui::rgba(255, 255, 255), 8.0f * d);
  if (st.sync_qr.size > 0) {
    const float quiet = 16.0f * d;
    const float cell = (qr_box - 2.0f * quiet) / float(st.sync_qr.size);
    for (int r = 0; r < st.sync_qr.size; ++r) {
      for (int c = 0; c < st.sync_qr.size; ++c) {
        if (st.sync_qr.at(c, r)) {
          const float mx = qx + quiet + float(c) * cell;
          const float my = qy + quiet + float(r) * cell;
          l.rect(mx, my, mx + cell + 0.35f, my + cell + 0.35f, ui::rgba(16, 18, 22));
        }
      }
    }
  }

  const std::string url =
      "http://" + st.sync_lan_ip + ":" + std::to_string(st.sync_port) + "/" + st.sync_manifest.token;
  draw_text(l, right_x0 + 0.5f * rw, qy + qr_box + 16.0f * d, url, FontFace::kMono, 11.5f * d, kTextSecondary,
            ui::Align::kCenter, rw - 32.0f * d);

  float step_y = qy + qr_box + 48.0f * d;
  draw_divider(l, right_x0 + 20.0f * d, step_y - 10.0f * d, x1 - 20.0f * d, d);
  const char* kSteps[3] = {
      "1. Scan the QR code to download the APK.",
      "2. Open Shift Drive and enter the 6-digit code.",
      "3. Files transfer directly over local Wi-Fi.",
  };
  for (int i = 0; i < 3; ++i) {
    draw_text(l, right_x0 + 22.0f * d, step_y + float(i) * 26.0f * d, kSteps[i], FontFace::kRegular, 12.0f * d,
              kTextSecondary, ui::Align::kLeft, rw - 44.0f * d);
  }
}

}  // namespace

const char* tab_name(LauncherTab tab) {
  switch (tab) {
    case LauncherTab::kPlay: return "Play";
    case LauncherTab::kSetup: return "Setup";
    case LauncherTab::kDlc: return "Content";
    case LauncherTab::kMods: return "Mods";
    case LauncherTab::kRadio: return "Radio";
    case LauncherTab::kWheel: return "Controls";
    case LauncherTab::kUpdate: return "Updates";
    case LauncherTab::kAndroid: return "Android";
    default: return "Play";
  }
}

bool parse_tab_name(std::string_view name, LauncherTab& out) {
  const std::string l = lower_str(name);
  if (l == "play") { out = LauncherTab::kPlay; return true; }
  if (l == "setup") { out = LauncherTab::kSetup; return true; }
  if (l == "content" || l == "dlc" || l == "v4") { out = LauncherTab::kDlc; return true; }
  if (l == "mods" || l == "mod") { out = LauncherTab::kMods; return true; }
  if (l == "radio") { out = LauncherTab::kRadio; return true; }
  if (l == "controls" || l == "wheel" || l == "controller") { out = LauncherTab::kWheel; return true; }
  if (l == "updates" || l == "update" || l == "updater") { out = LauncherTab::kUpdate; return true; }
  if (l == "android" || l == "sync" || l == "wifi") { out = LauncherTab::kAndroid; return true; }
  return false;
}

void populate_demo_state(LauncherUiState& st) {
  st.paths.portable = false;
  st.paths.data_root = "D:/ShiftEngine/data";
  st.paths.play_dir = "D:/ShiftEngine/data/play";
  st.paths.mods_dir = "D:/ShiftEngine/data/mods";
  st.paths.state_dir = "D:/ShiftEngine/data/state";
  st.paths.path_length_warning.clear();

  st.play_cfg.baked_car_count = 0;
  st.play_cfg.disc_car_count = 240;
  st.play_cfg.game_root_valid = true;
  st.play_cfg.has_colorado_index = true;
  st.play_cfg.has_sound_dir = false;
  st.play_cfg.game_root = "D:/ForzaHorizon/disc";

  if (!st.hw_info.gpu) {
    GpuInfo g;
    g.name = "NVIDIA GeForce RTX 4070 Laptop GPU";
    g.discrete = true;
    g.vram_bytes = 8ull << 30;
    g.api_version = (1u << 22) | (3u << 12);
    g.vendor_id = 0x10DE;
    st.hw_info.gpu = g;
  }
  if (st.hw_info.physical_cores <= 0) st.hw_info.physical_cores = 8;
  if (st.hw_info.logical_threads <= 0) st.hw_info.logical_threads = 16;
  st.hw_info.refresh_hz = 120;
  st.hw_rec = recommend_preset(st.hw_info);

  // Demo verified Xbox 360 XGD3 .iso disc check
  st.disc_path_input = "D:/ForzaHorizon/Forza_Horizon.iso";
  st.disc_check.valid = true;
  st.disc_check.looks_like_iso = true;
  st.disc_check.iso_format = "XGD3";
  st.disc_check.iso_file_count = 312;
  st.disc_check.iso_extract_bytes = 7420000000ull;
  st.disc_check.required_space_bytes = st.disc_check.iso_extract_bytes + (512ull << 20);
  st.disc_check.car_archive_count = 240;
  st.disc_check.free_space_bytes = 128ull << 30;
  st.disc_check.sufficient_space = true;
  st.disc_check.summary = "XGD3 ISO verified (312 files, 240 cars, 6.9 GB extract, 7.4 GB req., 128.0 GB free).";
  st.disc_check.items = {
      {"Colorado World Archive", "media/tracks/colorado/bin.zip", true, true, 1200000000ull, ""},
      {"Game Database (gamedb.slt)", "media/db/gamedb.slt", true, true, 18400000ull, ""},
      {"Radio Music Bank", "media/audio/radio/Radio_Music.fsb", false, true, 310000000ull, ""},
      {"UI4 Scenes Archive", "media/ui/UI.zip", false, true, 48000000ull, ""},
      {"Retail Executable (default.xex)", "default.xex", false, true, 14000000ull, ""},
      {"Vehicle Archives (media/cars/)", "media/cars", true, true, 0, ""},
  };
  st.setup_progress.fraction = 1.0f;
  st.setup_progress.success = true;
  st.setup_progress.stage_label = "Game Ready (XGD3 ISO extracted, Colorado index & AOT shaders cached)";
  st.setup_progress.log_lines = {
      "ISO verified (XGD3): D:/ForzaHorizon/Forza_Horizon.iso",
      "[ISO] 7076.3 / 7076.3 MB (312/312) media/tracks/colorado/bin.zip",
      "Disc verified: D:/ShiftEngine/data/play/game",
      "Wrote D:/ShiftEngine/data/play/play.txt",
      "Colorado world index ready (3,201 models).",
      "AOT shader pipelines cached.",
      "Game ready in 1.6s (direct-from-disc streaming \xE2\x80\x94 no car bake required).",
  };

  // Demo Content (DLC/v4/UDLC/XE)
  st.content_status.has_v4 = true;
  st.content_status.v4_enabled = true;
  st.content_status.has_rally = true;
  st.content_status.has_1000_club = true;
  st.content_status.has_udlc = true;
  st.content_status.dlc_installed_count = 21;
  st.content_status.notice = "Title Update v4, 21 DLC packs, and UDLC v1.2 detected.";
  st.content_status.packages = {
      {"title-update-v4", "Title Update v4", ContentKind::kTitleUpdateV4, {}, true, true, true, 0, "Rally and 1000 Club support"},
      {"rally", "Horizon Rally Expansion", ContentKind::kRallyExpansion, {}, true, true, true, 0, "ColoradoDirt + 7 championships"},
      {"1000club", "1000 Club Expansion", ContentKind::kClub1000, {}, true, true, true, 0, "1,145 car challenges"},
      {"udlc", "UDLC v1.2 Pack", ContentKind::kUdlcV12, {}, true, true, true, 0, "Mounted from mods/udlc-v1.2"},
      {"xe", "Forza Horizon XE", ContentKind::kHorizonXe, {}, false, false, false, 0, "Not installed"},
  };

  // Demo Mods
  st.mods = {
      {"synthwave_pulse", "Synthwave Pulse FM", "1.2.0", "Custom Radio Builder",
       "Replaces Horizon Pulse with 14 custom synthwave tracks and a 210x100 station logo.", "", "", {}, {}, true,
       true, true, false, 50, 4, {"custom radio"}, {}, {}, {}, {}},
      {"udlc_v12", "UDLC v1.2 Expanded Roster", "1.2.0", "Community",
       "Restores DLC and hidden vehicles with balanced PI tiers.", "", "", {}, {}, true, false, true, true, 40, 18,
       {"database", "cars/physics"}, {}, {}, {},
       {{"traffic_density", "Traffic Density", "", "Normal", {{"Low", "Low"}, {"Normal", "Normal"}, {"Dense", "Dense"}}},
        {"hidden_cars", "Unlock Hidden Cars", "", "Enabled", {{"Enabled", "Enabled"}, {"Disabled", "Disabled"}}}}},
      {"colorado_4k_roads", "Colorado 4K Asphalt Pack", "2.0.1", "HorizonTex",
       "Replaces road diffuse and normal maps at 4x resolution.", "", "", {}, {}, true, true, true, false, 30, 64,
       {"textures"}, {}, {}, {}, {}},
  };
  st.selected_mod = 1;
  st.save_iso = evaluate_save_isolation(st.mods);

  // Demo Radio Builder
  st.radio_cfg.station_slot = "Radio2";
  st.radio_cfg.mod_name = "synthwave_pulse";
  st.radio_cfg.custom_station_name = "Synthwave Pulse FM";
  st.radio_cfg.replace_playlist = true;
  st.radio_cfg.music_folder = "D:/Music/ColoradoSynthwave";
  st.radio_cfg.logo_png_path = "D:/Music/synthwave_logo_210x100.png";
  st.radio_logo_check = {true, 210, 100, "Valid 210x100 PNG"};
  st.radio_tracks = {
      {"D:/Music/01.flac", "Nightcall Highway", "Kavinsky", 28400000},
      {"D:/Music/02.mp3", "Red Rock Sunset", "The Midnight", 10200000},
      {"D:/Music/03.flac", "Turbo Killer", "Carpenter Brut", 31800000},
      {"D:/Music/04.wav", "Colorado Midnight", "FM-84", 44100000},
      {"D:/Music/05.mp3", "Outrun the Festival", "Timecop1983", 9800000},
      {"D:/Music/06.flac", "Pikes Peak Drift", "Gunship", 26500000},
  };
  st.radio_status = "6 tracks ready for FSB4 encoding.";

  // Demo Controls
  st.devices = {
      {1, "Logitech G29 Driving Force Wheel", 6, 25, false, true, {}},
      {2, "Fanatec ClubSport V3 Pedals (USB)", 3, 0, false, false, {}},
      {3, "Thrustmaster TH8A H-Shifter", 0, 8, false, false, {}},
      {4, "USB Analog Handbrake", 1, 0, false, false, {}},
  };
  st.wheel_cfg.separate_devices = true;
  st.wheel_cfg.steering_degrees = 900;
  st.wheel_cfg.steer.axis_index = 0;
  st.wheel_cfg.throttle.axis_index = 1;
  st.wheel_cfg.brake.axis_index = 2;
  st.wheel_cfg.clutch.axis_index = 3;
  st.wheel_cfg.handbrake.axis_index = 4;
  st.wheel_cfg.ffb_gain = 0.85f;
  st.live_steer = -0.34f;
  st.live_throttle = 0.78f;
  st.live_brake = 0.14f;
  st.live_clutch = 0.0f;
  st.live_handbrake = 0.0f;
  st.live_gear = 3;
  st.wheel_status = "Multi-device USB rig active.";

  // Demo Android Wi-Fi Sync
  st.sync_active = true;
  st.sync_lan_ip = "192.168.1.42";
  st.sync_port = WifiSyncServer::kPreferredPort;
  st.sync_manifest.host_name = "HORIZON-PC";
  st.sync_manifest.pairing_code = "482917";
  st.sync_manifest.token = "a7c9e204b18f3d56";
  st.sync_qr = generate_qr_matrix("http://192.168.1.42:47615/a7c9e204b18f3d56");
  st.sync_activity = {
      "Server started on 192.168.1.42:47615 (code 482917)",
      "UDP discovery listening on port 47616",
      "192.168.1.88 paired and fetched manifest",
      "Serving play/world & disc media",
  };
}

void build_launcher_ui(ui::List& l, const LauncherUiState& st, float width, float height,
                       std::vector<HitBox>& hitboxes, float dpi_scale) {
  hitboxes.clear();
  l.clear();
  init_launcher_fonts();

  const float d = std::clamp(dpi_scale > 0.0f ? dpi_scale : 1.0f, 0.75f, 3.0f);
  const float sidebar_w = std::min(200.0f * d, width * 0.28f);
  const float footer_h = 28.0f * d;

  // Full background canvas
  l.rect(0.0f, 0.0f, width, height, kBgCanvas);

  // Left Sidebar
  l.rect(0.0f, 0.0f, sidebar_w, height, kBgSidebar);
  l.rect(sidebar_w - std::max(1.0f, std::round(d)), 0.0f, sidebar_w, height, kBorderSubtle);

  // Brand header (clean, flat, zero marketing)
  l.rect(18.0f * d, 21.0f * d, 21.5f * d, 47.0f * d, kAccent, 1.5f * d);
  draw_text(l, 30.0f * d, 18.0f * d, "Shift Engine", FontFace::kSemiBold, 16.0f * d, kTextPrimary);
  draw_text(l, 30.0f * d, 38.0f * d, "v" + st.current_version, FontFace::kRegular, 11.5f * d, kTextMuted);
  draw_divider(l, 16.0f * d, 64.0f * d, sidebar_w - 16.0f * d, d);

  // Sidebar navigation items
  static constexpr const char* kTabIds[8] = {
      "tab:play", "tab:setup", "tab:dlc", "tab:mods", "tab:radio", "tab:wheel", "tab:update", "tab:android"};
  const float nav_y0 = 76.0f * d;
  const float nav_h = 34.0f * d;
  const float nav_gap = 4.0f * d;

  for (int i = 0; i < int(LauncherTab::kCount); ++i) {
    const LauncherTab t = LauncherTab(i);
    const float ny = nav_y0 + float(i) * (nav_h + nav_gap);
    const std::string cid = kTabIds[i];
    const bool active = st.active_tab == t;
    const bool hov = st.hovered_id == cid;
    const bool foc = st.focused_id == cid;
    hitboxes.push_back({cid, 10.0f * d, ny, sidebar_w - 10.0f * d, ny + nav_h, true});

    if (active) {
      l.rect(10.0f * d, ny, sidebar_w - 10.0f * d, ny + nav_h, kBgSelected, 5.0f * d);
      l.rect(10.0f * d, ny + 6.0f * d, 13.0f * d, ny + nav_h - 6.0f * d, kAccent, 1.5f * d);
    } else if (hov) {
      l.rect(10.0f * d, ny, sidebar_w - 10.0f * d, ny + nav_h, ui::rgba(255, 255, 255, 10), 5.0f * d);
    }
    if (foc) l.frame(10.0f * d, ny, sidebar_w - 10.0f * d, ny + nav_h, kAccent, 1.3f * d, 5.0f * d);

    const uint32_t icon_col = active ? kTextPrimary : (hov ? kTextPrimary : kTextSecondary);
    draw_sidebar_icon(l, t, 28.0f * d, ny + 0.5f * nav_h, d, icon_col);
    draw_text(l, 46.0f * d, text_y_center(ny, ny + nav_h, 13.5f * d), tab_name(t),
              active ? FontFace::kSemiBold : FontFace::kRegular, 13.5f * d,
              active ? kTextPrimary : (hov ? kTextPrimary : kTextSecondary));
  }

  // Quiet Sidebar Bottom Status
  const float sb_foot_y = height - 62.0f * d;
  draw_divider(l, 16.0f * d, sb_foot_y - 10.0f * d, sidebar_w - 16.0f * d, d);
  const bool ready = st.play_cfg.is_ready();
  l.circle(24.0f * d, sb_foot_y + 9.0f * d, 3.8f * d, ready ? kGreen : kAmber);
  draw_text(l, 34.0f * d, sb_foot_y + 1.0f * d, ready ? "Game ready" : "Setup required", FontFace::kSemiBold,
            12.0f * d, kTextPrimary);
  draw_text(l, 34.0f * d, sb_foot_y + 20.0f * d, st.paths.portable ? "Portable mode" : "Standard install",
            FontFace::kRegular, 11.5f * d, kTextMuted);

  // Main Canvas Area
  const float pad_x = 24.0f * d;
  const float content_x0 = sidebar_w + pad_x;
  const float content_x1 = width - pad_x;
  const float content_y0 = 20.0f * d;
  const float content_y1 = height - footer_h - 14.0f * d;

  switch (st.active_tab) {
    case LauncherTab::kPlay: draw_play_tab(l, st, content_x0, content_y0, content_x1, content_y1, d, hitboxes); break;
    case LauncherTab::kSetup: draw_setup_tab(l, st, content_x0, content_y0, content_x1, content_y1, d, hitboxes); break;
    case LauncherTab::kDlc: draw_dlc_tab(l, st, content_x0, content_y0, content_x1, content_y1, d, hitboxes); break;
    case LauncherTab::kMods: draw_mods_tab(l, st, content_x0, content_y0, content_x1, content_y1, d, hitboxes); break;
    case LauncherTab::kRadio: draw_radio_tab(l, st, content_x0, content_y0, content_x1, content_y1, d, hitboxes); break;
    case LauncherTab::kWheel: draw_wheel_tab(l, st, content_x0, content_y0, content_x1, content_y1, d, hitboxes); break;
    case LauncherTab::kUpdate: draw_update_tab(l, st, content_x0, content_y0, content_x1, content_y1, d, hitboxes); break;
    case LauncherTab::kAndroid: draw_android_tab(l, st, content_x0, content_y0, content_x1, content_y1, d, hitboxes); break;
    default: break;
  }

  // Quiet Bottom Status Strip (main canvas only)
  const float fy = height - footer_h;
  l.rect(sidebar_w, fy, width, height, ui::rgba(15, 17, 22));
  draw_divider(l, sidebar_w, fy, width, d);
  if (!st.banner_notice.empty()) {
    draw_text(l, content_x0, text_y_center(fy, height, 11.5f * d), st.banner_notice, FontFace::kRegular,
              11.5f * d, kTextPrimary, ui::Align::kLeft, (content_x1 - content_x0) * 0.72f);
  }
  draw_text(l, content_x1, text_y_center(fy, height, 11.0f * d),
            "1-8 Tabs  \xC2\xB7  Tab Focus  \xC2\xB7  Enter Activate", FontFace::kRegular, 11.0f * d, kTextMuted,
            ui::Align::kRight);
}

}  // namespace launcher
