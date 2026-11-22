#include "launcher_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "ui4_chrome.h"

namespace launcher {
namespace {

namespace ui = carview::ui;
namespace ui4 = carview::ui4;

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string truncate_str(std::string_view s, size_t max_chars) {
  if (s.size() <= max_chars) return std::string(s);
  if (max_chars <= 3) return std::string(s.substr(0, max_chars));
  return std::string(s.substr(0, max_chars - 3)) + "...";
}

std::string fmt_gb(uint64_t bytes) {
  char buf[32];
  if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
  else if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.0f MB", double(bytes) / double(1ull << 20));
  else std::snprintf(buf, sizeof buf, "%llu KB", (unsigned long long)(bytes >> 10));
  return buf;
}

void draw_fit_text(ui::List& l, float x, float y, std::string_view text, ui::TextStyle ts, float max_w,
                   float min_size = 8.5f) {
  ui4::fit_text(text, ts, max_w, min_size);
  l.text(x, y, text, ts);
}

void draw_card(ui::List& l, float x0, float y0, float x1, float y1, float s, uint32_t top_accent = 0) {
  l.shadow(x0, y0 + 2 * s, x1, y1 + 4 * s, ui::rgba(0, 0, 0, 120), 6 * s, 10 * s);
  l.rect(x0, y0, x1, y1, ui::rgba(24, 27, 33, 245), 6 * s, ui::rgba(18, 20, 25, 248));
  l.frame(x0, y0, x1, y1, ui::rgba(70, 76, 88, 170), 1.2f * s, 6 * s);
  if (top_accent != 0) {
    l.rect(x0 + 1 * s, y0, x1 - 1 * s, y0 + 3.5f * s, top_accent, 2 * s);
  }
}

void draw_section_title(ui::List& l, float x, float y, std::string_view title, float s,
                        uint32_t accent = ui4::kPink) {
  l.rect(x, y + 2 * s, x + 4 * s, y + 18 * s, accent, 1.5f * s);
  ui::TextStyle ts = ui4::style(ui::Font::kE, 16 * s, ui4::kWhite);
  ts.weight = 0.12f;
  l.text(x + 12 * s, y + 1 * s, title, ts);
}

void draw_button(ui::List& l, std::vector<HitBox>& hits, const std::string& id, float x0, float y0, float x1,
                 float y1, std::string_view label, float s, bool primary, bool hovered, bool active = false) {
  hits.push_back({id, x0, y0, x1, y1});
  uint32_t bg = primary ? ui4::kPink : (active ? ui::rgba(56, 62, 74, 245) : ui::rgba(36, 40, 48, 240));
  uint32_t bg2 = primary ? ui::rgba(190, 8, 72, 255) : ui::rgba(26, 29, 36, 245);
  if (hovered) {
    bg = primary ? ui4::kPinkHi : ui::rgba(64, 70, 84, 250);
    bg2 = primary ? ui4::kPink : ui::rgba(46, 52, 64, 250);
  }
  l.rect(x0, y0, x1, y1, bg, 5 * s, bg2);
  l.frame(x0, y0, x1, y1, hovered ? ui4::kWhite : (primary ? ui4::kPinkHi : ui::rgba(92, 98, 112, 180)),
          1.3f * s, 5 * s);
  ui::TextStyle ts = ui4::style(ui::Font::kE, 14 * s, ui4::kWhite, ui::Align::kCenter);
  ts.weight = primary ? 0.13f : 0.11f;
  ui4::fit_text(label, ts, (x1 - x0) - 16 * s, 10 * s);
  l.text(0.5f * (x0 + x1), 0.5f * (y0 + y1) - 0.5f * ts.size, label, ts);
}

void draw_pill(ui::List& l, float x, float y, float w, float h, std::string_view label, uint32_t bg,
               uint32_t fg, float s) {
  l.rect(x, y, x + w, y + h, bg, h * 0.45f);
  ui::TextStyle ts = ui4::style(ui::Font::kB, 11 * s, fg, ui::Align::kCenter);
  ts.weight = 0.12f;
  ui4::fit_text(label, ts, w - 10 * s, 8 * s);
  l.text(x + w * 0.5f, y + (h - ts.size) * 0.5f, label, ts);
}

void draw_progress_bar(ui::List& l, float x, float y, float w, float h, float frac, uint32_t fill, float s) {
  frac = std::clamp(frac, 0.0f, 1.0f);
  l.rect(x, y, x + w, y + h, ui::rgba(14, 16, 20, 240), h * 0.4f);
  l.frame(x, y, x + w, y + h, ui::rgba(74, 80, 94, 180), 1.0f * s, h * 0.4f);
  if (frac > 0.005f) {
    l.rect(x + 1.5f * s, y + 1.5f * s, x + std::max(h, w * frac) - 1.5f * s, y + h - 1.5f * s, fill, h * 0.35f);
  }
}

void draw_axis_bar(ui::List& l, float x, float y, float w, float h, std::string_view label, float val,
                   bool centered, uint32_t fill, float dz_in, float s) {
  ui::TextStyle lbl = ui4::style(ui::Font::kB, 12 * s, ui4::kDim);
  l.text(x, y, label, lbl);
  char pct[24];
  std::snprintf(pct, sizeof pct, "%+d%%", int(std::lround(val * 100.0f)));
  ui::TextStyle val_ts = ui4::style(ui::Font::kB, 12 * s, ui4::kWhite, ui::Align::kRight);
  l.text(x + w, y, centered ? std::string(pct) : (std::to_string(int(std::lround(val * 100.0f))) + "%"), val_ts);

  const float by = y + 18 * s;
  const float bh = h;
  l.rect(x, by, x + w, by + bh, ui::rgba(12, 14, 18, 245), 4 * s);
  l.frame(x, by, x + w, by + bh, ui::rgba(68, 74, 88, 180), 1.0f * s, 4 * s);
  if (centered) {
    const float mid = x + w * 0.5f;
    // Deadzone band
    const float dz_w = std::clamp(dz_in, 0.0f, 0.45f) * (w * 0.5f);
    l.rect(mid - dz_w, by + 2 * s, mid + dz_w, by + bh - 2 * s, ui::rgba(255, 255, 255, 26), 2 * s);
    l.line(mid, by + 1 * s, mid, by + bh - 1 * s, 1.5f * s, ui::rgba(140, 148, 162, 180));
    const float clamped = std::clamp(val, -1.0f, 1.0f);
    const float end_x = mid + clamped * (w * 0.5f - 2 * s);
    if (std::fabs(end_x - mid) > 1.0f) {
      l.rect(std::min(mid, end_x), by + 3 * s, std::max(mid, end_x), by + bh - 3 * s, fill, 2.5f * s);
    }
  } else {
    const float clamped = std::clamp(val, 0.0f, 1.0f);
    const float dz_w = std::clamp(dz_in, 0.0f, 0.45f) * w;
    if (dz_w > 1.0f) l.rect(x + 2 * s, by + 2 * s, x + dz_w, by + bh - 2 * s, ui::rgba(255, 255, 255, 22), 2 * s);
    if (clamped > 0.005f) {
      l.rect(x + 2 * s, by + 3 * s, x + 2 * s + clamped * (w - 4 * s), by + bh - 3 * s, fill, 2.5f * s);
    }
  }
}

void draw_play_tab(ui::List& l, const LauncherUiState& st, float s, std::vector<HitBox>& hits) {
  // Left Hero Card
  draw_card(l, 32 * s, 142 * s, 742 * s, 658 * s, s, ui4::kPink);
  // Hero festival strip
  l.rect(34 * s, 146 * s, 740 * s, 268 * s, ui::rgba(46, 16, 36, 220), 4 * s, ui::rgba(20, 22, 28, 235));
  draw_pill(l, 52 * s, 166 * s, 196 * s, 24 * s, "COLORADO FESTIVAL", ui4::kPink, ui4::kWhite, s);
  draw_fit_text(l, 52 * s, 202 * s, "FORZA HORIZON NATIVE DRIVE", ui4::style(ui::Font::kE, 25 * s, ui4::kWhite),
                668 * s, 14 * s);
  draw_fit_text(l, 52 * s, 238 * s,
                st.play_cfg.baked_car_count > 0
                    ? ("READY: " + std::to_string(st.play_cfg.baked_car_count) + " BAKED CARS  -  COLORADO INDEX " +
                       (st.play_cfg.has_colorado_index ? "OK" : "MISSING"))
                    : "NO PLAY FOLDER YET - OPEN SETUP TAB FOR ONE-CLICK DISC BAKE",
                ui4::style(ui::Font::kB, 13 * s, st.play_cfg.baked_car_count > 0 ? ui4::kGreen : ui4::kAmber),
                668 * s, 9.5f * s);

  // Graphics Preset selector
  draw_section_title(l, 52 * s, 288 * s, "GRAPHICS & PLATFORM PRESET", s, ui4::kAmber);
  static constexpr const char* kPresets[5] = {"HIGH", "LOW-SPEC 60", "SMOOTH 60", "QUALITY 30", "LOW POWER"};
  for (int i = 0; i < 5; ++i) {
    const float px = 52 * s + float(i) * 134 * s;
    const std::string id = "preset:" + std::to_string(i);
    draw_button(l, hits, id, px, 318 * s, px + 126 * s, 354 * s, kPresets[i], s,
                st.selected_preset == i, st.hovered_id == id, st.selected_preset == i);
  }

  // Launch summary details
  draw_section_title(l, 52 * s, 376 * s, "LAUNCH CONFIGURATION", s, ui4::kBlue);
  ui::TextStyle kts = ui4::style(ui::Font::kB, 13 * s, ui4::kDim);
  ui::TextStyle vts = ui4::style(ui::Font::kB, 13 * s, ui4::kWhite);
  l.text(52 * s, 408 * s, "STARTING CAR:", kts);
  draw_fit_text(l, 224 * s, 408 * s, st.launch_car, vts, 496 * s, 9.5f * s);
  l.text(52 * s, 434 * s, "PLAY FOLDER:", kts);
  draw_fit_text(l, 224 * s, 434 * s, truncate_str(st.paths.play_dir.string(), 52), vts, 496 * s, 9.5f * s);
  l.text(52 * s, 460 * s, "INSTALL MODE:", kts);
  draw_fit_text(l, 224 * s, 460 * s, st.paths.portable ? "PORTABLE (SELF-CONTAINED DATA/)" : "STANDARD USER FOLDER",
                ui4::style(ui::Font::kB, 13 * s, st.paths.portable ? ui4::kAmber : ui4::kWhite), 496 * s, 9.5f * s);
  l.text(52 * s, 486 * s, "SAVE PROFILE:", kts);
  draw_fit_text(l, 224 * s, 486 * s, st.save_iso.profile + " (" + st.save_iso.kind + ")",
                ui4::style(ui::Font::kB, 13 * s, st.save_iso.kind == "own" ? ui4::kGreen : ui4::kAmber),
                496 * s, 9.5f * s);

  // Primary Play button + Steam + Portable buttons
  draw_button(l, hits, "act:play", 52 * s, 532 * s, 410 * s, 594 * s, "PLAY COLORADO NOW", s,
              true, st.hovered_id == "act:play");
  draw_button(l, hits, "act:steam", 426 * s, 532 * s, 578 * s, 594 * s, "ADD TO STEAM", s,
              false, st.hovered_id == "act:steam");
  draw_button(l, hits, "act:portable", 590 * s, 532 * s, 722 * s, 594 * s,
              st.paths.portable ? "PORTABLE: ON" : "PORTABLE: OFF", s,
              false, st.hovered_id == "act:portable", st.paths.portable);

  if (!st.paths.path_length_warning.empty()) {
    draw_fit_text(l, 52 * s, 616 * s, st.paths.path_length_warning,
                  ui4::style(ui::Font::kB, 12 * s, ui4::kAmber), 668 * s, 9.0f * s);
  } else {
    draw_fit_text(l, 52 * s, 616 * s, "ZERO PYTHON/QT RUNTIME REQUIRED - NATIVE SDL3 + VULKAN LAUNCHER",
                  ui4::style(ui::Font::kB, 11 * s, ui4::kDim), 668 * s, 9.0f * s);
  }

  // Right Column: Hardware Capability Advisor ("This PC")
  draw_card(l, 764 * s, 142 * s, 1248 * s, 412 * s, s, ui4::kGreen);
  draw_section_title(l, 784 * s, 162 * s, "THIS PC - HARDWARE ADVISOR", s, ui4::kGreen);

  const std::string gpu_name = st.hw_info.gpu ? st.hw_info.gpu->name : "NO VULKAN GPU DETECTED";
  const std::string vram_s = st.hw_info.gpu ? fmt_gb(st.hw_info.gpu->vram_bytes) : "N/A";
  const std::string vk_s = st.hw_info.gpu ? ("VULKAN " + st.hw_info.gpu->vulkan_version_string()) : "UNAVAILABLE";
  const std::string cpu_s = std::to_string(st.hw_info.physical_cores) + " CORES / " +
                            std::to_string(st.hw_info.logical_threads) + " THREADS - " +
                            std::to_string(st.hw_info.refresh_hz) + " HZ";

  l.text(784 * s, 198 * s, "GPU:", kts);
  draw_fit_text(l, 886 * s, 198 * s, gpu_name, vts, 340 * s, 9.5f * s);
  l.text(784 * s, 224 * s, "MEMORY:", kts);
  draw_fit_text(l, 886 * s, 224 * s, vram_s + "  -  " + vk_s, vts, 340 * s, 9.5f * s);
  l.text(784 * s, 250 * s, "CPU / HZ:", kts);
  draw_fit_text(l, 886 * s, 250 * s, cpu_s, vts, 340 * s, 9.5f * s);

  // Recommended preset callout box
  l.rect(784 * s, 284 * s, 1228 * s, 352 * s, ui::rgba(16, 36, 26, 220), 5 * s);
  l.frame(784 * s, 284 * s, 1228 * s, 352 * s, ui4::kGreen, 1.2f * s, 5 * s);
  draw_fit_text(l, 798 * s, 294 * s, "RECOMMENDED: " + st.hw_rec.label,
                ui4::style(ui::Font::kE, 15 * s, ui4::kGreen), 416 * s, 11 * s);
  draw_fit_text(l, 798 * s, 322 * s, st.hw_rec.reason,
                ui4::style(ui::Font::kB, 11 * s, ui4::kWhite), 416 * s, 8.5f * s);

  draw_button(l, hits, "act:apply_rec", 784 * s, 364 * s, 1020 * s, 398 * s, "APPLY RECOMMENDED PRESET", s,
              false, st.hovered_id == "act:apply_rec");
  if (!st.hw_rec.warning.empty()) {
    draw_pill(l, 1032 * s, 368 * s, 196 * s, 26 * s, "DRIVER WARNING", ui4::kRed, ui4::kWhite, s);
  } else {
    draw_pill(l, 1032 * s, 368 * s, 196 * s, 26 * s, "VULKAN 1.3 READY", ui::rgba(28, 84, 46, 240), ui4::kGreen, s);
  }

  // Right Bottom Card: Ecosystem Summary
  draw_card(l, 764 * s, 430 * s, 1248 * s, 658 * s, s, ui4::kAmber);
  draw_section_title(l, 784 * s, 450 * s, "CONTENT & ECOSYSTEM", s, ui4::kAmber);
  int enabled_mods = 0;
  for (const auto& m : st.mods) {
    if (m.enabled) ++enabled_mods;
  }
  l.text(784 * s, 486 * s, "TITLE UPDATE V4:", kts);
  draw_fit_text(l, 986 * s, 486 * s, st.content_status.has_v4 ? "INSTALLED (RALLY + CLUB)" : "NOT INSTALLED",
                ui4::style(ui::Font::kB, 13 * s, st.content_status.has_v4 ? ui4::kGreen : ui4::kDim),
                242 * s, 9.5f * s);
  l.text(784 * s, 514 * s, "DLC / UDLC / XE:", kts);
  draw_fit_text(l, 986 * s, 514 * s,
                std::to_string(st.content_status.dlc_installed_count) + " DLC" +
                    (st.content_status.has_udlc ? " + UDLC V1.2" : "") +
                    (st.content_status.has_xe ? " + HORIZON XE" : ""),
                vts, 242 * s, 9.5f * s);
  l.text(784 * s, 542 * s, "ACTIVE MODS:", kts);
  draw_fit_text(l, 986 * s, 542 * s, std::to_string(enabled_mods) + " OF " + std::to_string(st.mods.size()) + " ENABLED",
                vts, 242 * s, 9.5f * s);
  l.text(784 * s, 570 * s, "ANDROID SYNC:", kts);
  draw_fit_text(l, 986 * s, 570 * s,
                st.sync_active ? ("SHARING ON PORT " + std::to_string(st.sync_port)) : "READY OVER LAN WI-FI",
                ui4::style(ui::Font::kB, 13 * s, st.sync_active ? ui4::kGreen : ui4::kDim), 242 * s, 9.5f * s);

  draw_button(l, hits, "tab:mods", 784 * s, 606 * s, 926 * s, 642 * s, "MOD MANAGER", s, false,
              st.hovered_id == "tab:mods");
  draw_button(l, hits, "tab:radio", 936 * s, 606 * s, 1082 * s, 642 * s, "RADIO BUILDER", s, false,
              st.hovered_id == "tab:radio");
  draw_button(l, hits, "tab:android", 1092 * s, 606 * s, 1228 * s, 642 * s, "WI-FI SYNC", s, false,
              st.hovered_id == "tab:android");
}

void draw_setup_tab(ui::List& l, const LauncherUiState& st, float s, std::vector<HitBox>& hits) {
  draw_card(l, 32 * s, 142 * s, 1248 * s, 400 * s, s, ui4::kPink);
  draw_section_title(l, 52 * s, 162 * s, "ONE-CLICK DISC VERIFICATION & BAKE", s, ui4::kPink);

  l.text(52 * s, 196 * s, "EXTRACTED DISC FOLDER (DROP FOLDER ONTO WINDOW OR EDIT PATH):",
         ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  l.rect(52 * s, 218 * s, 960 * s, 254 * s, ui::rgba(12, 14, 18, 245), 4 * s);
  l.frame(52 * s, 218 * s, 960 * s, 254 * s, ui::rgba(80, 86, 100, 200), 1.2f * s, 4 * s);
  l.text(64 * s, 229 * s,
         st.disc_path_input.empty() ? "DRAG & DROP YOUR EXTRACTED FH1 DISC FOLDER HERE (HOLDING MEDIA/)"
                                    : truncate_str(st.disc_path_input, 74),
         ui4::style(ui::Font::kB, 13 * s, st.disc_path_input.empty() ? ui4::kDim : ui4::kWhite));
  draw_button(l, hits, "setup:verify", 974 * s, 218 * s, 1110 * s, 254 * s, "VERIFY DISC", s, false,
              st.hovered_id == "setup:verify");
  draw_button(l, hits, "setup:run", 1120 * s, 218 * s, 1228 * s, 254 * s,
              st.setup_progress.running ? "BAKING..." : "RUN SETUP", s, true, st.hovered_id == "setup:run");

  // Verification checklist grid (2 columns)
  for (size_t i = 0; i < st.disc_check.items.size() && i < 6; ++i) {
    const auto& it = st.disc_check.items[i];
    const float col_x = (i % 2 == 0) ? 52 * s : 650 * s;
    const float row_y = 272 * s + float(i / 2) * 34 * s;
    const uint32_t dot = it.present ? ui4::kGreen : (it.required ? ui4::kRed : ui4::kAmber);
    l.circle(col_x + 8 * s, row_y + 8 * s, 5 * s, dot);
    std::string desc = it.label;
    if (!it.sha256_prefix.empty()) desc += " [SHA256 " + it.sha256_prefix + "]";
    l.text(col_x + 22 * s, row_y + 1 * s, truncate_str(desc, 52), ui4::style(ui::Font::kB, 12 * s, ui4::kWhite));
  }
  l.text(52 * s, 376 * s, truncate_str(st.disc_check.summary, 96),
         ui4::style(ui::Font::kB, 12 * s, st.disc_check.valid ? ui4::kGreen : ui4::kAmber));

  // Progress + Log Drawer
  draw_card(l, 32 * s, 418 * s, 1248 * s, 658 * s, s, ui4::kAmber);
  draw_section_title(l, 52 * s, 438 * s, "LIVE BAKE PROGRESS & LOG DRAWER", s, ui4::kAmber);

  draw_button(l, hits, "setup:toggle_sounds", 820 * s, 434 * s, 1030 * s, 464 * s,
              st.setup_all_sounds ? "ALL SOUNDS: ON" : "ALL SOUNDS: DEFAULT CAR", s, false,
              st.hovered_id == "setup:toggle_sounds", st.setup_all_sounds);
  draw_button(l, hits, "setup:toggle_index", 1042 * s, 434 * s, 1228 * s, 464 * s,
              st.setup_build_index ? "COLORADO INDEX: ON" : "COLORADO INDEX: OFF", s, false,
              st.hovered_id == "setup:toggle_index", st.setup_build_index);

  const std::string stg = st.setup_progress.stage_label.empty() ? "IDLE - VERIFY DISC TO BEGIN" : st.setup_progress.stage_label;
  l.text(52 * s, 476 * s, stg, ui4::style(ui::Font::kB, 13 * s, ui4::kWhite));
  draw_progress_bar(l, 52 * s, 500 * s, 1176 * s, 16 * s, st.setup_progress.fraction,
                    st.setup_progress.success ? ui4::kGreen : ui4::kPink, s);

  // Log box
  l.rect(52 * s, 528 * s, 1228 * s, 644 * s, ui::rgba(10, 12, 16, 250), 4 * s);
  const size_t nlines = st.setup_progress.log_lines.size();
  const size_t start = nlines > 6 ? nlines - 6 : 0;
  for (size_t i = start; i < nlines; ++i) {
    const float ly = 536 * s + float(i - start) * 17 * s;
    l.text(64 * s, ly, truncate_str(st.setup_progress.log_lines[i], 96),
           ui4::style(ui::Font::kB, 11 * s, ui4::kDim));
  }
}

void draw_dlc_tab(ui::List& l, const LauncherUiState& st, float s, std::vector<HitBox>& hits) {
  draw_card(l, 32 * s, 142 * s, 1248 * s, 240 * s, s, ui4::kAmber);
  draw_section_title(l, 52 * s, 162 * s, "DLC, TITLE UPDATE V4, UDLC V1.2 & FORZA HORIZON XE", s, ui4::kAmber);
  l.text(52 * s, 196 * s, truncate_str(st.content_status.notice, 98), ui4::style(ui::Font::kB, 13 * s, ui4::kWhite));
  draw_button(l, hits, "dlc:rescan", 980 * s, 158 * s, 1110 * s, 192 * s, "RESCAN", s, false,
              st.hovered_id == "dlc:rescan");
  draw_button(l, hits, "dlc:import", 1120 * s, 158 * s, 1228 * s, 192 * s, "IMPORT PKG", s, true,
              st.hovered_id == "dlc:import");

  draw_card(l, 32 * s, 256 * s, 1248 * s, 658 * s, s, ui4::kPink);
  draw_section_title(l, 52 * s, 276 * s, "DETECTED PACKAGES & EXPANSIONS", s, ui4::kPink);

  for (size_t i = 0; i < st.content_status.packages.size() && i < 8; ++i) {
    const auto& pkg = st.content_status.packages[i];
    const float ry = 312 * s + float(i) * 41 * s;
    l.rect(52 * s, ry, 1228 * s, ry + 34 * s, (i % 2 == 0) ? ui::rgba(32, 35, 42, 235) : ui::rgba(26, 28, 34, 235),
           4 * s);
    l.circle(70 * s, ry + 17 * s, 5.5f * s, pkg.installed ? (pkg.enabled ? ui4::kGreen : ui4::kAmber) : ui::rgba(90, 96, 108, 220));
    l.text(88 * s, ry + 10 * s, truncate_str(pkg.title, 38), ui4::style(ui::Font::kE, 13 * s, ui4::kWhite));
    l.text(560 * s, ry + 11 * s, pkg.installed ? truncate_str(pkg.detail, 42) : "NOT INSTALLED (DROP STFS OR FOLDER)",
           ui4::style(ui::Font::kB, 12 * s, pkg.installed ? ui4::kDim : ui::rgba(120, 126, 138, 220)));
    draw_pill(l, 1110 * s, ry + 6 * s, 106 * s, 22 * s,
              pkg.installed ? (pkg.enabled ? "MOUNTED" : "DISABLED") : "MISSING",
              pkg.installed ? ui::rgba(28, 76, 44, 240) : ui::rgba(48, 52, 62, 230),
              pkg.installed ? ui4::kGreen : ui4::kDim, s);
  }
}

void draw_mods_tab(ui::List& l, const LauncherUiState& st, float s, std::vector<HitBox>& hits) {
  // Save-isolation banner
  const uint32_t iso_col = st.save_iso.kind == "own" ? ui4::kGreen : (st.save_iso.kind == "modded" ? ui4::kAmber : ui4::kPink);
  l.rect(32 * s, 140 * s, 1248 * s, 180 * s, ui::rgba(28, 30, 36, 245), 5 * s);
  l.frame(32 * s, 140 * s, 1248 * s, 180 * s, iso_col, 1.4f * s, 5 * s);
  draw_pill(l, 44 * s, 149 * s, 152 * s, 22 * s, "SAVE: " + st.save_iso.profile, iso_col, ui4::kInk, s);
  l.text(210 * s, 153 * s, truncate_str(st.save_iso.warning_banner, 86), ui4::style(ui::Font::kB, 12 * s, ui4::kWhite));

  // Left mod list
  draw_card(l, 32 * s, 194 * s, 680 * s, 658 * s, s, ui4::kPink);
  draw_section_title(l, 52 * s, 214 * s, "INSTALLED MODS (LOAD ORDER)", s, ui4::kPink);
  draw_button(l, hits, "mods:rescan", 442 * s, 210 * s, 548 * s, 242 * s, "RESCAN", s, false,
              st.hovered_id == "mods:rescan");
  draw_button(l, hits, "mods:install", 556 * s, 210 * s, 662 * s, 242 * s, "INSTALL ZIP", s, true,
              st.hovered_id == "mods:install");

  for (size_t i = 0; i < st.mods.size() && i < 8; ++i) {
    const auto& m = st.mods[i];
    const float ry = 256 * s + float(i) * 44 * s;
    const bool sel = int(i) == st.selected_mod;
    const std::string row_id = "mod:sel:" + std::to_string(i);
    hits.push_back({row_id, 50 * s, ry, 540 * s, ry + 38 * s});
    l.rect(50 * s, ry, 662 * s, ry + 38 * s,
           sel ? ui::rgba(58, 24, 42, 240) : ui::rgba(28, 31, 38, 235), 4 * s);
    if (sel) l.frame(50 * s, ry, 662 * s, ry + 38 * s, ui4::kPink, 1.3f * s, 4 * s);
    const uint32_t dot = !m.problems.empty() ? ui4::kRed : (m.enabled ? ui4::kGreen : ui::rgba(110, 116, 128, 220));
    l.circle(68 * s, ry + 19 * s, 5.5f * s, dot);
    l.text(84 * s, ry + 11 * s, "#" + std::to_string(i + 1) + "  " + truncate_str(m.title, 30),
           ui4::style(ui::Font::kE, 13 * s, ui4::kWhite));
    const std::string tog_id = "mod:tog:" + std::to_string(i);
    draw_button(l, hits, tog_id, 556 * s, ry + 5 * s, 654 * s, ry + 33 * s, m.enabled ? "ON" : "OFF", s,
                m.enabled, st.hovered_id == tog_id);
  }
  if (st.mods.empty()) {
    l.text(52 * s, 280 * s, "NO MODS IN MODS/ FOLDER. DROP A MOD FOLDER OR .ZIP ONTO THE LAUNCHER.",
           ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  }
  draw_button(l, hits, "mod:up", 50 * s, 614 * s, 190 * s, 646 * s, "MOVE UP", s, false, st.hovered_id == "mod:up");
  draw_button(l, hits, "mod:down", 200 * s, 614 * s, 340 * s, 646 * s, "MOVE DOWN", s, false,
              st.hovered_id == "mod:down");

  // Right inspector
  draw_card(l, 700 * s, 194 * s, 1248 * s, 658 * s, s, ui4::kAmber);
  draw_section_title(l, 720 * s, 214 * s, "MOD INSPECTOR & OPTIONS", s, ui4::kAmber);
  if (st.selected_mod >= 0 && size_t(st.selected_mod) < st.mods.size()) {
    const auto& m = st.mods[size_t(st.selected_mod)];
    l.text(720 * s, 250 * s, truncate_str(m.title, 36), ui4::style(ui::Font::kE, 20 * s, ui4::kWhite));
    const std::string byline = "V" + (m.version.empty() ? "1.0" : m.version) + "  |  " +
                               (m.author.empty() ? "COMMUNITY" : m.author) + "  |  PRIORITY " +
                               std::to_string(m.priority);
    l.text(720 * s, 278 * s, truncate_str(byline, 54), ui4::style(ui::Font::kB, 12 * s, ui4::kAmber));
    l.text(720 * s, 306 * s, truncate_str(m.description.empty() ? "No description provided in mod.toml / mod.txt." : m.description, 56),
           ui4::style(ui::Font::kB, 12 * s, ui4::kWhite));

    std::string tags;
    for (size_t ti = 0; ti < m.contents.size(); ++ti) {
      if (ti > 0) tags += ", ";
      tags += m.contents[ti];
    }
    l.text(720 * s, 338 * s, "CONTENTS: " + (tags.empty() ? "OVERRIDE FILES" : tags) + " (" + std::to_string(m.file_count) + " FILES)",
           ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
    l.text(720 * s, 364 * s,
           !m.profile.empty() ? ("SAVE ISOLATION: OWN PROFILE (" + m.profile + ")")
                              : (m.shares_save ? "SAVE ISOLATION: SHARES NORMAL SAVE" : "SAVE ISOLATION: USES USER-MODDED COPY"),
           ui4::style(ui::Font::kB, 12 * s, m.shares_save ? ui4::kGreen : ui4::kAmber));

    if (!m.problems.empty()) {
      l.rect(720 * s, 392 * s, 1228 * s, 430 * s, ui::rgba(80, 18, 24, 220), 4 * s);
      l.text(732 * s, 404 * s, "CONFLICT: " + truncate_str(m.problems[0], 50),
             ui4::style(ui::Font::kB, 12 * s, ui4::kRed));
    }

    // Mod options (mod.toml)
    float oy = 446 * s;
    l.text(720 * s, oy, "MOD OPTIONS (MOD.TOML):", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
    oy += 26 * s;
    for (size_t oi = 0; oi < m.options.size() && oi < 4; ++oi) {
      const auto& opt = m.options[oi];
      l.text(720 * s, oy + 6 * s, truncate_str(opt.title, 26), ui4::style(ui::Font::kB, 12 * s, ui4::kWhite));
      const std::string oid = "mod:opt:" + std::to_string(oi);
      draw_button(l, hits, oid, 960 * s, oy, 1228 * s, oy + 30 * s, opt.value.empty() ? "DEFAULT" : opt.value,
                  s, false, st.hovered_id == oid);
      oy += 38 * s;
    }
    if (m.options.empty()) {
      l.text(720 * s, oy + 4 * s, "NO CONFIGURABLE OPTIONS DECLARED.", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
    }
  }
}

void draw_radio_tab(ui::List& l, const LauncherUiState& st, float s, std::vector<HitBox>& hits) {
  // Left Builder Configuration Card
  draw_card(l, 32 * s, 142 * s, 636 * s, 658 * s, s, ui4::kPink);
  draw_section_title(l, 52 * s, 162 * s, "CUSTOM RADIO STATION BUILDER", s, ui4::kPink);

  l.text(52 * s, 196 * s, "TARGET FESTIVAL STATION:", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  struct StInfo {
    const char* slot;
    const char* label;
    uint32_t col;
  };
  constexpr StInfo kStations[3] = {
      {"Radio1", "BASS ARENA", ui::rgba(245, 4, 92, 255)},
      {"Radio2", "HORIZON PULSE", ui::rgba(255, 184, 28, 255)},
      {"Radio3", "HORIZON ROCKS", ui::rgba(44, 140, 232, 255)},
  };
  for (int i = 0; i < 3; ++i) {
    const float sx = 52 * s + float(i) * 192 * s;
    const bool active = st.radio_cfg.station_slot == kStations[i].slot;
    const std::string sid = std::string("radio:slot:") + kStations[i].slot;
    draw_button(l, hits, sid, sx, 220 * s, sx + 182 * s, 260 * s, kStations[i].label, s,
                active, st.hovered_id == sid, active);
  }

  l.text(52 * s, 278 * s, "PLAYLIST MODE:", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  draw_button(l, hits, "radio:mode:replace", 52 * s, 300 * s, 330 * s, 336 * s, "REPLACE STOCK TRACKS", s,
              st.radio_cfg.replace_playlist, st.hovered_id == "radio:mode:replace", st.radio_cfg.replace_playlist);
  draw_button(l, hits, "radio:mode:extend", 342 * s, 300 * s, 616 * s, 336 * s, "EXTEND STOCK PLAYLIST", s,
              !st.radio_cfg.replace_playlist, st.hovered_id == "radio:mode:extend", !st.radio_cfg.replace_playlist);

  // Music folder & station name & logo
  l.text(52 * s, 356 * s, "USER MUSIC FOLDER (MP3 / WAV / FLAC / OGG):", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  l.rect(52 * s, 376 * s, 616 * s, 410 * s, ui::rgba(12, 14, 18, 245), 4 * s);
  l.frame(52 * s, 376 * s, 616 * s, 410 * s, ui::rgba(76, 82, 96, 200), 1.0f * s, 4 * s);
  l.text(64 * s, 386 * s,
         st.radio_cfg.music_folder.empty() ? "DROP MUSIC FOLDER HERE"
                                           : truncate_str(st.radio_cfg.music_folder.string(), 48),
         ui4::style(ui::Font::kB, 12 * s, ui4::kWhite));

  l.text(52 * s, 426 * s, "CUSTOM STATION NAME STRING:", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  l.rect(52 * s, 446 * s, 616 * s, 480 * s, ui::rgba(12, 14, 18, 245), 4 * s);
  l.frame(52 * s, 446 * s, 616 * s, 480 * s, ui::rgba(76, 82, 96, 200), 1.0f * s, 4 * s);
  l.text(64 * s, 456 * s,
         st.radio_cfg.custom_station_name.empty() ? "(KEEP STOCK NAME)" : st.radio_cfg.custom_station_name,
         ui4::style(ui::Font::kB, 13 * s, ui4::kAmber));

  l.text(52 * s, 496 * s, "STATION LOGO PNG (210X100 REQUIRED):", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  l.rect(52 * s, 516 * s, 616 * s, 550 * s, ui::rgba(12, 14, 18, 245), 4 * s);
  l.frame(52 * s, 516 * s, 616 * s, 550 * s, st.radio_logo_check.valid ? ui4::kGreen : ui::rgba(76, 82, 96, 200),
          1.2f * s, 4 * s);
  l.text(64 * s, 526 * s, truncate_str(st.radio_logo_check.message, 48),
         ui4::style(ui::Font::kB, 12 * s, st.radio_logo_check.valid ? ui4::kGreen : ui4::kDim));

  const bool has_ff = !st.paths.ffmpeg_bin.empty();
  draw_pill(l, 52 * s, 566 * s, 210 * s, 26 * s, has_ff ? "FFMPEG: READY (48KHZ CBR)" : "FFMPEG: NOT ON PATH",
            has_ff ? ui::rgba(28, 76, 44, 240) : ui::rgba(80, 24, 24, 240), has_ff ? ui4::kGreen : ui4::kRed, s);
  draw_button(l, hits, "radio:build", 52 * s, 602 * s, 616 * s, 646 * s,
              "ENCODE & PACK CUSTOM RADIO MOD", s, true, st.hovered_id == "radio:build");

  // Right Playlist & Logo Preview Card
  draw_card(l, 656 * s, 142 * s, 1248 * s, 658 * s, s, ui4::kAmber);
  draw_section_title(l, 676 * s, 162 * s,
                     "PLAYLIST PREVIEW (" + std::to_string(st.radio_tracks.size()) + " TRACKS)", s, ui4::kAmber);

  // 210x100 logo preview box
  l.rect(676 * s, 198 * s, 886 * s, 298 * s, ui::rgba(18, 20, 26, 250), 4 * s);
  l.frame(676 * s, 198 * s, 886 * s, 298 * s, ui4::kPink, 1.4f * s, 4 * s);
  l.rect(682 * s, 204 * s, 880 * s, 232 * s, ui4::kPink, 2 * s);
  draw_fit_text(l, 781 * s, 211 * s,
                st.radio_cfg.custom_station_name.empty() ? st.radio_cfg.station_slot : st.radio_cfg.custom_station_name,
                ui4::style(ui::Font::kE, 13 * s, ui4::kWhite, ui::Align::kCenter), 186 * s, 8.5f * s);
  l.text(781 * s, 254 * s, "210 X 100 PNG LOGO", ui4::style(ui::Font::kB, 11 * s, ui4::kDim, ui::Align::kCenter));
  l.text(781 * s, 272 * s, st.radio_logo_check.valid ? "VERIFIED" : "CUSTOM ARTWORK",
         ui4::style(ui::Font::kB, 10 * s, st.radio_logo_check.valid ? ui4::kGreen : ui4::kAmber, ui::Align::kCenter));

  // Output metadata next to logo
  draw_fit_text(l, 902 * s, 204 * s, "MOD OUTPUT: MODS/" + st.radio_cfg.mod_name,
                ui4::style(ui::Font::kB, 12 * s, ui4::kWhite), 328 * s, 9.0f * s);
  draw_fit_text(l, 902 * s, 228 * s, "BANK: FSB4 / 48 KHZ / 160 KBPS CBR",
                ui4::style(ui::Font::kB, 12 * s, ui4::kDim), 328 * s, 9.0f * s);
  draw_fit_text(l, 902 * s, 252 * s, "SYNC POINTS: SONG / EVENT / IDENT",
                ui4::style(ui::Font::kB, 12 * s, ui4::kDim), 328 * s, 9.0f * s);
  draw_fit_text(l, 902 * s, 276 * s, "SAVE COMPAT: SHARES NORMAL SAVE",
                ui4::style(ui::Font::kB, 12 * s, ui4::kGreen), 328 * s, 9.0f * s);

  // Track table
  for (size_t i = 0; i < st.radio_tracks.size() && i < 8; ++i) {
    const auto& t = st.radio_tracks[i];
    const float ry = 316 * s + float(i) * 38 * s;
    l.rect(676 * s, ry, 1228 * s, ry + 32 * s, (i % 2 == 0) ? ui::rgba(32, 35, 42, 235) : ui::rgba(24, 27, 33, 235),
           4 * s);
    char idx_s[16];
    std::snprintf(idx_s, sizeof idx_s, "%02zu", i + 1);
    l.text(688 * s, ry + 9 * s, idx_s, ui4::style(ui::Font::kB, 12 * s, ui4::kAmber));
    draw_fit_text(l, 722 * s, ry + 9 * s, t.title, ui4::style(ui::Font::kE, 12 * s, ui4::kWhite), 275 * s, 9.0f * s);
    draw_fit_text(l, 1010 * s, ry + 9 * s, t.artist.empty() ? "UNKNOWN ARTIST" : t.artist,
                  ui4::style(ui::Font::kB, 11 * s, ui4::kDim), 205 * s, 8.5f * s);
  }
  if (!st.radio_status.empty()) {
    draw_fit_text(l, 676 * s, 628 * s, st.radio_status, ui4::style(ui::Font::kB, 12 * s, ui4::kGreen),
                  552 * s, 9.0f * s);
  }
}

void draw_wheel_tab(ui::List& l, const LauncherUiState& st, float s, std::vector<HitBox>& hits) {
  // Left: Devices & FFB Settings
  draw_card(l, 32 * s, 142 * s, 568 * s, 658 * s, s, ui4::kPink);
  draw_section_title(l, 52 * s, 162 * s, "CONNECTED SDL3 / USB DEVICES", s, ui4::kPink);

  for (size_t i = 0; i < st.devices.size() && i < 4; ++i) {
    const auto& d = st.devices[i];
    const float dy = 198 * s + float(i) * 44 * s;
    const bool sel = int(i) == st.selected_device;
    const std::string did = "wheel:dev:" + std::to_string(i);
    hits.push_back({did, 52 * s, dy, 548 * s, dy + 38 * s});
    l.rect(52 * s, dy, 548 * s, dy + 38 * s, sel ? ui::rgba(56, 24, 40, 240) : ui::rgba(28, 31, 38, 235), 4 * s);
    if (sel) l.frame(52 * s, dy, 548 * s, dy + 38 * s, ui4::kPink, 1.3f * s, 4 * s);
    l.circle(68 * s, dy + 19 * s, 5 * s, d.has_ffb ? ui4::kGreen : ui4::kBlue);
    l.text(84 * s, dy + 11 * s, truncate_str(d.name, 28), ui4::style(ui::Font::kE, 12 * s, ui4::kWhite));
    l.text(538 * s, dy + 12 * s,
           std::to_string(d.num_axes) + "AX / " + std::to_string(d.num_buttons) + "BTN",
           ui4::style(ui::Font::kB, 11 * s, ui4::kDim, ui::Align::kRight));
  }
  if (st.devices.empty()) {
    l.text(52 * s, 210 * s, "NO WHEEL OR GAMEPAD DETECTED - PLUG IN USB DEVICE.",
           ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  }

  draw_section_title(l, 52 * s, 390 * s, "WHEEL & FORCE FEEDBACK SETTINGS", s, ui4::kAmber);
  draw_button(l, hits, "wheel:separate", 52 * s, 424 * s, 310 * s, 458 * s,
              st.wheel_cfg.separate_devices ? "SEPARATE USB: ON" : "SEPARATE USB: OFF", s, false,
              st.hovered_id == "wheel:separate", st.wheel_cfg.separate_devices);
  draw_button(l, hits, "wheel:degrees", 322 * s, 424 * s, 548 * s, 458 * s,
              "LOCK: " + std::to_string(st.wheel_cfg.steering_degrees) + " DEG", s, false,
              st.hovered_id == "wheel:degrees");

  // FFB Gain bar + buttons
  draw_axis_bar(l, 52 * s, 474 * s, 496 * s, 16 * s, "FORCE FEEDBACK GAIN", st.wheel_cfg.ffb_gain,
                false, ui4::kPink, 0.0f, s);
  draw_button(l, hits, "wheel:ffb_minus", 52 * s, 518 * s, 150 * s, 550 * s, "FFB -", s, false,
              st.hovered_id == "wheel:ffb_minus");
  draw_button(l, hits, "wheel:ffb_plus", 160 * s, 518 * s, 258 * s, 550 * s, "FFB +", s, false,
              st.hovered_id == "wheel:ffb_plus");
  draw_button(l, hits, "wheel:ffb_invert", 268 * s, 518 * s, 416 * s, 550 * s,
              st.wheel_cfg.ffb_invert ? "INVERT: ON" : "INVERT: OFF", s, false,
              st.hovered_id == "wheel:ffb_invert", st.wheel_cfg.ffb_invert);
  draw_button(l, hits, "wheel:ffb_test", 426 * s, 518 * s, 548 * s, 550 * s, "TEST FFB", s, false,
              st.hovered_id == "wheel:ffb_test");

  draw_button(l, hits, "wheel:save", 52 * s, 574 * s, 548 * s, 616 * s, "SAVE TO SETTINGS.TXT", s, true,
              st.hovered_id == "wheel:save");
  if (!st.wheel_status.empty()) {
    draw_fit_text(l, 52 * s, 628 * s, st.wheel_status, ui4::style(ui::Font::kB, 11 * s, ui4::kGreen),
                  496 * s, 8.5f * s);
  }

  // Right: Live Axis Bar Preview + Deadzones + H-Shifter
  draw_card(l, 588 * s, 142 * s, 1248 * s, 658 * s, s, ui4::kGreen);
  draw_section_title(l, 608 * s, 162 * s, "LIVE AXIS TELEMETRY & BINDINGS", s, ui4::kGreen);

  draw_axis_bar(l, 608 * s, 200 * s, 620 * s, 18 * s,
                "STEERING (AXIS " + std::to_string(std::max(0, st.wheel_cfg.steer.axis_index)) + ")",
                st.live_steer, true, ui4::kPink, st.wheel_cfg.steer.deadzone_in, s);
  draw_axis_bar(l, 608 * s, 254 * s, 620 * s, 18 * s,
                "THROTTLE (AXIS " + std::to_string(std::max(0, st.wheel_cfg.throttle.axis_index)) + ")",
                st.live_throttle, false, ui4::kGreen, st.wheel_cfg.throttle.deadzone_in, s);
  draw_axis_bar(l, 608 * s, 308 * s, 620 * s, 18 * s,
                "BRAKE (AXIS " + std::to_string(std::max(0, st.wheel_cfg.brake.axis_index)) + ")",
                st.live_brake, false, ui4::kRed, st.wheel_cfg.brake.deadzone_in, s);
  draw_axis_bar(l, 608 * s, 362 * s, 620 * s, 18 * s,
                "CLUTCH (AXIS " + std::to_string(std::max(0, st.wheel_cfg.clutch.axis_index)) + ")",
                st.live_clutch, false, ui4::kBlue, st.wheel_cfg.clutch.deadzone_in, s);
  draw_axis_bar(l, 608 * s, 416 * s, 620 * s, 18 * s,
                "HANDBRAKE (AXIS " + std::to_string(std::max(0, st.wheel_cfg.handbrake.axis_index)) + ")",
                st.live_handbrake, false, ui4::kAmber, st.wheel_cfg.handbrake.deadzone_in, s);

  // Deadzone summary + H-Shifter live gear indicator
  draw_section_title(l, 608 * s, 476 * s, "H-SHIFTER & DEADZONE TUNING", s, ui4::kAmber);
  static constexpr const char* kGears[7] = {"R", "1", "2", "3", "4", "5", "6"};
  for (int g = 0; g < 7; ++g) {
    const float gx = 608 * s + float(g) * 56 * s;
    const bool active = (g == 0 && st.live_gear == -1) || (g > 0 && st.live_gear == g);
    l.rect(gx, 514 * s, gx + 48 * s, 560 * s, active ? ui4::kPink : ui::rgba(28, 32, 40, 240), 5 * s);
    l.frame(gx, 514 * s, gx + 48 * s, 560 * s, active ? ui4::kWhite : ui::rgba(74, 80, 96, 180), 1.2f * s, 5 * s);
    l.text(gx + 24 * s, 528 * s, kGears[g], ui4::style(ui::Font::kE, 18 * s, ui4::kWhite, ui::Align::kCenter));
  }

  char dz_buf[128];
  std::snprintf(dz_buf, sizeof dz_buf, "STEER DZ: %.0f%% - %.0f%%   |   PEDAL DZ: %.0f%% - %.0f%%",
                st.wheel_cfg.steer.deadzone_in * 100.0f, st.wheel_cfg.steer.deadzone_out * 100.0f,
                st.wheel_cfg.throttle.deadzone_in * 100.0f, st.wheel_cfg.throttle.deadzone_out * 100.0f);
  l.text(608 * s, 582 * s, dz_buf, ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  draw_button(l, hits, "wheel:dz_down", 608 * s, 608 * s, 790 * s, 642 * s, "STEER DZ -", s, false,
              st.hovered_id == "wheel:dz_down");
  draw_button(l, hits, "wheel:dz_up", 802 * s, 608 * s, 984 * s, 642 * s, "STEER DZ +", s, false,
              st.hovered_id == "wheel:dz_up");
}

void draw_update_tab(ui::List& l, const LauncherUiState& st, float s, std::vector<HitBox>& hits) {
  draw_card(l, 32 * s, 142 * s, 1248 * s, 658 * s, s, ui4::kBlue);
  draw_section_title(l, 52 * s, 162 * s, "SELF-UPDATER (GITHUB RELEASES + SHA-256 VERIFICATION)", s, ui4::kBlue);

  l.text(52 * s, 206 * s, "INSTALLED VERSION: v" + st.current_version + "  |  CHANNEL: " + st.update_channel,
         ui4::style(ui::Font::kE, 16 * s, ui4::kWhite));
  l.text(52 * s, 238 * s, truncate_str(st.update_status, 96), ui4::style(ui::Font::kB, 13 * s, ui4::kDim));

  draw_button(l, hits, "upd:check", 52 * s, 276 * s, 290 * s, 316 * s, "CHECK FOR UPDATES", s, true,
              st.hovered_id == "upd:check");
  draw_button(l, hits, "upd:channel", 304 * s, 276 * s, 510 * s, 316 * s,
              "CHANNEL: " + st.update_channel, s, false, st.hovered_id == "upd:channel");

  if (st.available_update) {
    const auto& u = *st.available_update;
    l.rect(52 * s, 344 * s, 1228 * s, 540 * s, ui::rgba(18, 30, 44, 220), 6 * s);
    l.frame(52 * s, 344 * s, 1228 * s, 540 * s, ui4::kBlue, 1.4f * s, 6 * s);
    l.text(72 * s, 364 * s, "NEW RELEASE AVAILABLE: " + u.tag + " (" + u.asset_name + ")",
           ui4::style(ui::Font::kE, 18 * s, ui4::kWhite));
    l.text(72 * s, 398 * s, "DOWNLOAD SIZE: " + fmt_gb(u.size_bytes), ui4::style(ui::Font::kB, 13 * s, ui4::kDim));
    l.text(72 * s, 424 * s, "EXPECTED SHA-256: " + u.sha256, ui4::style(ui::Font::kB, 12 * s, ui4::kGreen));
    l.text(72 * s, 452 * s, "STAGED SWAP: DOWNLOADS TO .SHIFT-UPDATE, VERIFIES SHA-256, ATOMIC ROLLBACK ON ERROR",
           ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
    draw_button(l, hits, "upd:stage", 72 * s, 484 * s, 360 * s, 524 * s, "STAGE & VERIFY UPDATE", s, true,
                st.hovered_id == "upd:stage");
  }
}

void draw_android_tab(ui::List& l, const LauncherUiState& st, float s, std::vector<HitBox>& hits) {
  // Left: Controls & Pairing
  draw_card(l, 32 * s, 142 * s, 748 * s, 658 * s, s, ui4::kGreen);
  draw_section_title(l, 52 * s, 162 * s, "ANDROID WI-FI SYNC SERVER (LOCAL LAN ONLY)", s, ui4::kGreen);

  // Pairing code highlight box
  l.rect(52 * s, 198 * s, 728 * s, 286 * s, ui::rgba(16, 22, 20, 245), 6 * s);
  l.frame(52 * s, 198 * s, 728 * s, 286 * s, st.sync_active ? ui4::kGreen : ui::rgba(76, 82, 96, 200), 1.5f * s, 6 * s);
  l.text(70 * s, 210 * s, "6-DIGIT PAIRING CODE:", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  const std::string code_disp = st.sync_manifest.pairing_code.size() == 6
                                    ? (st.sync_manifest.pairing_code.substr(0, 3) + " " + st.sync_manifest.pairing_code.substr(3))
                                    : "------";
  l.text(70 * s, 232 * s, code_disp, ui4::style(ui::Font::kE, 36 * s, ui4::kAmber));
  l.text(360 * s, 214 * s, "LAN ADDRESS:", ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  l.text(360 * s, 236 * s, st.sync_lan_ip + ":" + std::to_string(st.sync_port),
         ui4::style(ui::Font::kE, 18 * s, ui4::kWhite));
  draw_fit_text(l, 360 * s, 264 * s,
                st.sync_active ? "BROADCASTING UDP 47616 + SERVING HTTP" : "SERVER STOPPED",
                ui4::style(ui::Font::kB, 11 * s, st.sync_active ? ui4::kGreen : ui4::kDim), 356 * s, 8.5f * s);

  // Share group toggles
  l.text(52 * s, 304 * s, "CONTENT SHARED TO ANDROID/DATA/NET.SHIFTENGINE.DRIVE/FILES/:",
         ui4::style(ui::Font::kB, 12 * s, ui4::kDim));
  draw_button(l, hits, "sync:play", 52 * s, 326 * s, 382 * s, 362 * s,
              st.sync_include_play ? "[X] BAKED PLAY FOLDER" : "[ ] BAKED PLAY FOLDER", s, false,
              st.hovered_id == "sync:play", st.sync_include_play);
  draw_button(l, hits, "sync:dlc", 394 * s, 326 * s, 728 * s, 362 * s,
              st.sync_include_dlc ? "[X] DLC & TITLE UPDATE V4" : "[ ] DLC & TITLE UPDATE V4", s, false,
              st.hovered_id == "sync:dlc", st.sync_include_dlc);
  draw_button(l, hits, "sync:mods", 52 * s, 372 * s, 382 * s, 408 * s,
              st.sync_include_mods ? "[X] ENABLED MODS" : "[ ] ENABLED MODS", s, false,
              st.hovered_id == "sync:mods", st.sync_include_mods);
  draw_button(l, hits, "sync:radio", 394 * s, 372 * s, 728 * s, 408 * s,
              st.sync_include_radio ? "[X] CUSTOM RADIO STATIONS" : "[ ] CUSTOM RADIO STATIONS", s, false,
              st.hovered_id == "sync:radio", st.sync_include_radio);

  draw_button(l, hits, "sync:toggle", 52 * s, 426 * s, 728 * s, 474 * s,
              st.sync_active ? "STOP WI-FI SYNC SERVER" : "START WI-FI SYNC SERVER", s, true,
              st.hovered_id == "sync:toggle");

  // Activity log
  l.rect(52 * s, 492 * s, 728 * s, 642 * s, ui::rgba(10, 12, 16, 250), 4 * s);
  l.text(64 * s, 502 * s, "TRANSFER LOG:", ui4::style(ui::Font::kB, 11 * s, ui4::kDim));
  const size_t an = st.sync_activity.size();
  const size_t as = an > 6 ? an - 6 : 0;
  for (size_t i = as; i < an; ++i) {
    l.text(64 * s, 522 * s + float(i - as) * 19 * s, truncate_str(st.sync_activity[i], 64),
           ui4::style(ui::Font::kB, 11 * s, ui4::kWhite));
  }

  // Right: QR Code Matrix
  draw_card(l, 768 * s, 142 * s, 1248 * s, 658 * s, s, ui4::kPink);
  draw_section_title(l, 788 * s, 162 * s, "SCAN ON ANDROID DEVICE", s, ui4::kPink);

  const float qr_box = 280 * s;
  const float qx = 0.5f * (768 * s + 1248 * s) - qr_box * 0.5f;
  const float qy = 204 * s;
  l.rect(qx, qy, qx + qr_box, qy + qr_box, ui4::kWhite, 6 * s);
  if (st.sync_qr.size > 0) {
    const float quiet = 18 * s;
    const float cell = (qr_box - 2.0f * quiet) / float(st.sync_qr.size);
    for (int r = 0; r < st.sync_qr.size; ++r) {
      for (int c = 0; c < st.sync_qr.size; ++c) {
        if (st.sync_qr.at(c, r)) {
          const float mx = qx + quiet + float(c) * cell;
          const float my = qy + quiet + float(r) * cell;
          l.rect(mx, my, mx + cell + 0.35f, my + cell + 0.35f, ui::rgba(12, 14, 18, 255));
        }
      }
    }
  }
  const std::string url = "http://" + st.sync_lan_ip + ":" + std::to_string(st.sync_port) + "/" + st.sync_manifest.token;
  draw_fit_text(l, 0.5f * (768 * s + 1248 * s), 498 * s, url,
                ui4::style(ui::Font::kB, 12 * s, ui4::kAmber, ui::Align::kCenter), 444 * s, 8.5f * s);
  draw_fit_text(l, 790 * s, 534 * s, "1. SCAN QR CODE TO DOWNLOAD SHIFT-DRIVE-ARM64.APK",
                ui4::style(ui::Font::kB, 11 * s, ui4::kWhite), 440 * s, 8.5f * s);
  draw_fit_text(l, 790 * s, 560 * s, "2. OPEN SHIFT DRIVE & ENTER THE 6-DIGIT CODE",
                ui4::style(ui::Font::kB, 11 * s, ui4::kWhite), 440 * s, 8.5f * s);
  draw_fit_text(l, 790 * s, 586 * s, "3. RESUMABLE HTTP RANGE SYNC OVER PRIVATE WI-FI",
                ui4::style(ui::Font::kB, 11 * s, ui4::kWhite), 440 * s, 8.5f * s);
  draw_fit_text(l, 790 * s, 616 * s, "PRIVATE LAN ONLY - ZERO DATA UPLOADED OFF DEVICE",
                ui4::style(ui::Font::kB, 11 * s, ui4::kDim), 440 * s, 8.5f * s);
}

}  // namespace

const char* tab_name(LauncherTab tab) {
  switch (tab) {
    case LauncherTab::kPlay: return "PLAY";
    case LauncherTab::kSetup: return "SETUP";
    case LauncherTab::kDlc: return "DLC & V4";
    case LauncherTab::kMods: return "MODS";
    case LauncherTab::kRadio: return "RADIO";
    case LauncherTab::kWheel: return "WHEEL";
    case LauncherTab::kUpdate: return "UPDATE";
    case LauncherTab::kAndroid: return "ANDROID";
    default: return "PLAY";
  }
}

bool parse_tab_name(std::string_view name, LauncherTab& out) {
  const std::string l = lower_str(name);
  if (l == "play") { out = LauncherTab::kPlay; return true; }
  if (l == "setup") { out = LauncherTab::kSetup; return true; }
  if (l == "dlc" || l == "v4") { out = LauncherTab::kDlc; return true; }
  if (l == "mods" || l == "mod") { out = LauncherTab::kMods; return true; }
  if (l == "radio") { out = LauncherTab::kRadio; return true; }
  if (l == "wheel" || l == "controller") { out = LauncherTab::kWheel; return true; }
  if (l == "update" || l == "updater") { out = LauncherTab::kUpdate; return true; }
  if (l == "android" || l == "sync" || l == "wifi") { out = LauncherTab::kAndroid; return true; }
  return false;
}

void populate_demo_state(LauncherUiState& st) {
  st.play_cfg.baked_car_count = 240;
  st.play_cfg.has_colorado_index = true;
  st.play_cfg.has_sound_dir = true;
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

  // Demo disc check
  st.disc_path_input = "D:/ForzaHorizon/disc";
  st.disc_check.valid = true;
  st.disc_check.car_archive_count = 240;
  st.disc_check.free_space_bytes = 128ull << 30;
  st.disc_check.summary = "Verified disc folder (240 car archives, 128.0 GB free). Ready for One-Click Setup.";
  st.disc_check.items = {
      {"Colorado World Archive", "media/tracks/colorado/bin.zip", true, true, 1200000000ull, ""},
      {"Game Database (gamedb.slt)", "media/db/gamedb.slt", true, true, 18400000ull, "9f42c1a8d0b3e712"},
      {"Radio Music Bank", "media/audio/radio/Radio_Music.fsb", false, true, 310000000ull, ""},
      {"UI4 Scenes Archive", "media/ui/UI.zip", false, true, 48000000ull, ""},
      {"Retail Executable (default.xex)", "default.xex", false, true, 14000000ull, ""},
      {"Vehicle Archives (media/cars/)", "media/cars", true, true, 0, ""},
  };
  st.setup_progress.fraction = 1.0f;
  st.setup_progress.success = true;
  st.setup_progress.stage_label = "Ready to drive (240 cars + Colorado index baked)";
  st.setup_progress.log_lines = {
      "Disc verified: D:/ForzaHorizon/disc",
      "gamedb.slt SHA-256: 9f42c1a8d0b3e712...",
      "carbake: 240 cars baked to play/bake/cars",
      "soundbake: engine loops baked (48 kHz stereo)",
      "colorado.index written (3,201 collision tiles)",
      "One-Click Setup complete! Ready to drive.",
  };

  // Demo DLC/v4/UDLC/XE
  st.content_status.has_v4 = true;
  st.content_status.v4_enabled = true;
  st.content_status.has_rally = true;
  st.content_status.has_1000_club = true;
  st.content_status.has_udlc = true;
  st.content_status.dlc_installed_count = 21;
  st.content_status.notice = "Title Update v4 + 21/21 DLC packs + UDLC v1.2 verified and mounted.";
  st.content_status.packages = {
      {"title-update-v4", "Title Update v4 (Rally + 1000 Club)", ContentKind::kTitleUpdateV4, {}, true, true, true, 0, "Verified v4 package"},
      {"rally", "Horizon Rally Expansion Pack", ContentKind::kRallyExpansion, {}, true, true, true, 0, "ColoradoDirt + 7 championships"},
      {"1000club", "1000 Club Expansion Pack", ContentKind::kClub1000, {}, true, true, true, 0, "1,145 car challenges unlocked"},
      {"udlc", "UDLC v1.2 Community DLC Pack", ContentKind::kUdlcV12, {}, true, true, true, 0, "Mounted from mods/udlc-v1.2"},
      {"xe", "Forza Horizon XE", ContentKind::kHorizonXe, {}, false, false, false, 0, "Not installed"},
  };

  // Demo Mods
  st.mods = {
      {"synthwave_pulse", "Synthwave Pulse FM", "1.2.0", "Shift Radio Builder",
       "Replaces Horizon Pulse with 14 custom synthwave tracks + 210x100 neon logo.", "", "", {}, {}, true, true,
       true, false, 50, 4, {"custom radio"}, {}, {}, {}, {}},
      {"udlc_v12", "UDLC v1.2 Expanded Roster", "1.2.0", "Community",
       "Adds restored DLC and hidden vehicles with balanced PI tiers.", "", "", {}, {}, true, false, true, true, 40,
       18, {"database", "cars/physics"}, {}, {}, {},
       {{"traffic_density", "Traffic Density", "", "Normal", {{"Low", "Low"}, {"Normal", "Normal"}, {"Dense", "Dense"}}},
        {"hidden_cars", "Unlock Hidden Cars", "", "Enabled", {{"Enabled", "Enabled"}, {"Disabled", "Disabled"}}}}},
      {"colorado_4k_roads", "Colorado 4K Asphalt Pack", "2.0.1", "HorizonTex",
       "Hash-replaced road diffuse and normal maps at 4x resolution.", "", "", {}, {}, true, true, true, false, 30,
       64, {"textures"}, {}, {}, {}, {}},
  };
  st.save_iso = evaluate_save_isolation(st.mods);

  // Demo Radio Builder
  st.radio_cfg.station_slot = "Radio2";
  st.radio_cfg.mod_name = "synthwave_pulse";
  st.radio_cfg.custom_station_name = "SYNTHWAVE PULSE FM";
  st.radio_cfg.replace_playlist = true;
  st.radio_cfg.music_folder = "D:/Music/ColoradoSynthwave";
  st.radio_cfg.logo_png_path = "D:/Music/synthwave_logo_210x100.png";
  st.radio_logo_check = {true, 210, 100, "Valid 210x100 PNG station logo."};
  st.radio_tracks = {
      {"D:/Music/01.flac", "Nightcall Highway", "Kavinsky", 28400000},
      {"D:/Music/02.mp3", "Red Rock Sunset", "The Midnight", 10200000},
      {"D:/Music/03.flac", "Turbo Killer", "Carpenter Brut", 31800000},
      {"D:/Music/04.wav", "Colorado Midnight", "FM-84", 44100000},
      {"D:/Music/05.mp3", "Outrun the Festival", "Timecop1983", 9800000},
      {"D:/Music/06.flac", "Pikes Peak Drift", "Gunship", 26500000},
  };
  st.radio_status = "Ready: 6 tracks queued for 48 kHz 160 kbps FSB4 encode.";

  // Demo Wheel Configurator
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
  st.wheel_status = "Separate-USB multi-device rig active (Wheel + Pedals + TH8A + Handbrake).";

  // Demo Android Wi-Fi Sync
  st.sync_active = true;
  st.sync_lan_ip = "192.168.1.42";
  st.sync_port = WifiSyncServer::kPreferredPort;
  st.sync_manifest.host_name = "HORIZON-PC";
  st.sync_manifest.pairing_code = "482917";
  st.sync_manifest.token = "a7c9e204b18f3d56";
  st.sync_qr = generate_qr_matrix("http://192.168.1.42:47615/a7c9e204b18f3d56");
  st.sync_activity = {
      "Wi-Fi Sync started on 192.168.1.42:47615 (code 482917)",
      "UDP LAN discovery listening on port 47616",
      "192.168.1.88 (Odin 2 Portal) paired and fetched manifest",
      "Streaming play/bake/cars (240 files, resumable HTTP Range)",
  };
}

void build_launcher_ui(ui::List& l, const LauncherUiState& st, float width, float height,
                       std::vector<HitBox>& hitboxes) {
  hitboxes.clear();
  l.clear();
  const float s = std::min(width / 1280.0f, height / 720.0f);

  // Full background canvas
  l.rect(0, 0, width, height, ui::rgba(15, 17, 22, 255), 0, ui::rgba(10, 12, 16, 255));
  // Subtle top festival accent gradient strip
  l.rect(0, 0, width, 74 * s, ui::rgba(22, 24, 30, 255), 0, ui::rgba(16, 18, 24, 255));
  l.rect(0, 72 * s, width, 74.5f * s, ui4::kPink);

  // Tilted FH1-styled Brand Header
  {
    ui4::TiltScope tilt(l, 160 * s, 36 * s, ui4::kTiltRad * 0.7f);
    l.rect(28 * s, 12 * s, 294 * s, 60 * s, ui4::kPink, 3 * s);
    l.text(44 * s, 18 * s, "SHIFT ENGINE", ui4::style(ui::Font::kE, 21 * s, ui4::kWhite));
    l.text(44 * s, 45 * s, "UNIFIED 4-PLATFORM LAUNCHER", ui4::style(ui::Font::kB, 9 * s, ui4::kOffWhite));
  }

  // Header status pills (right side)
  draw_pill(l, 830 * s, 23 * s, 134 * s, 26 * s, st.paths.portable ? "PORTABLE MODE" : "STANDARD INSTALL",
            st.paths.portable ? ui::rgba(76, 56, 16, 240) : ui::rgba(36, 40, 48, 240),
            st.paths.portable ? ui4::kAmber : ui4::kDim, s);
  draw_pill(l, 974 * s, 23 * s, 148 * s, 26 * s,
            st.play_cfg.baked_car_count > 0 ? "PLAY FOLDER READY" : "SETUP REQUIRED",
            st.play_cfg.baked_car_count > 0 ? ui::rgba(24, 72, 42, 240) : ui::rgba(82, 54, 16, 240),
            st.play_cfg.baked_car_count > 0 ? ui4::kGreen : ui4::kAmber, s);
  draw_pill(l, 1132 * s, 23 * s, 116 * s, 26 * s, "V" + st.current_version, ui::rgba(36, 40, 48, 240), ui4::kWhite, s);

  // 8 Navigation Tabs
  const float tab_w = (1248.0f - 32.0f - 7.0f * 8.0f) / 8.0f;
  for (int i = 0; i < int(LauncherTab::kCount); ++i) {
    const LauncherTab t = LauncherTab(i);
    const float tx = 32 * s + float(i) * (tab_w + 8.0f) * s;
    const bool active = st.active_tab == t;
    const std::string tid = "tab:" + lower_str(std::string(tab_name(t)).substr(0, tab_name(t)[0] == 'D' ? 3 : 10));
    // Canonical tab hit IDs
    static constexpr const char* kTabIds[8] = {
        "tab:play", "tab:setup", "tab:dlc", "tab:mods", "tab:radio", "tab:wheel", "tab:update", "tab:android"};
    const std::string cid = kTabIds[i];
    hitboxes.push_back({cid, tx, 84 * s, tx + tab_w * s, 124 * s});
    const bool hov = st.hovered_id == cid;
    l.rect(tx, 84 * s, tx + tab_w * s, 124 * s,
           active ? ui4::kPink : (hov ? ui::rgba(48, 52, 64, 240) : ui::rgba(26, 29, 36, 240)), 4 * s);
    if (active) l.rect(tx + 4 * s, 121 * s, tx + tab_w * s - 4 * s, 124 * s, ui4::kWhite);
    ui::TextStyle tts = ui4::style(ui::Font::kE, 13 * s, active ? ui4::kWhite : ui4::kDim, ui::Align::kCenter);
    l.text(tx + tab_w * 0.5f * s, 97 * s, tab_name(t), tts);
  }

  // Active tab content
  switch (st.active_tab) {
    case LauncherTab::kPlay: draw_play_tab(l, st, s, hitboxes); break;
    case LauncherTab::kSetup: draw_setup_tab(l, st, s, hitboxes); break;
    case LauncherTab::kDlc: draw_dlc_tab(l, st, s, hitboxes); break;
    case LauncherTab::kMods: draw_mods_tab(l, st, s, hitboxes); break;
    case LauncherTab::kRadio: draw_radio_tab(l, st, s, hitboxes); break;
    case LauncherTab::kWheel: draw_wheel_tab(l, st, s, hitboxes); break;
    case LauncherTab::kUpdate: draw_update_tab(l, st, s, hitboxes); break;
    case LauncherTab::kAndroid: draw_android_tab(l, st, s, hitboxes); break;
    default: break;
  }

  // Footer status bar
  l.rect(0, 676 * s, width, height, ui::rgba(12, 14, 18, 255));
  l.line(0, 676 * s, width, 676 * s, 1.0f * s, ui::rgba(54, 60, 72, 180));
  const std::string footer_left = st.banner_notice.empty()
                                      ? "1-8 SWITCH TABS  -  DRAG & DROP DISC OR MOD FOLDER  -  ZERO PYTHON/QT RUNTIME"
                                      : st.banner_notice;
  draw_fit_text(l, 32 * s, 691 * s, footer_left,
                ui4::style(ui::Font::kB, 11 * s, st.banner_notice.empty() ? ui4::kDim : ui4::kAmber), 840 * s, 8.5f * s);
  l.text(width - 32 * s, 691 * s, "WINDOWS / LINUX / MACOS / ANDROID",
         ui4::style(ui::Font::kB, 11 * s, ui4::kDim, ui::Align::kRight));
}

}  // namespace launcher
