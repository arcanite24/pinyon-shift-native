#include "frame_inspector.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "perf_overlay.h"

namespace carview {

const char* inspector_view_name(InspectorView v) {
  switch (v) {
    case InspectorView::kColor: return "COLOR";
    case InspectorView::kDepth: return "DEPTH";
    case InspectorView::kAo: return "SSAO";
    case InspectorView::kShadow: return "SHADOW";
    case InspectorView::kMotion: return "MOTION";
    case InspectorView::kHiZ: return "HI-Z";
    default: return "COLOR";
  }
}

InspectorView parse_inspector_view(const std::string& name) {
  std::string lower;
  for (char c : name) lower.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  if (lower == "depth") return InspectorView::kDepth;
  if (lower == "ao" || lower == "ssao") return InspectorView::kAo;
  if (lower == "shadow" || lower == "shadows") return InspectorView::kShadow;
  if (lower == "motion" || lower == "velocity") return InspectorView::kMotion;
  if (lower == "hiz" || lower == "hi-z") return InspectorView::kHiZ;
  return InspectorView::kColor;
}

std::vector<std::string> frame_inspector_lines(const FrameInspectorSnapshot& s) {
  std::vector<std::string> out;
  char buf[192];
  std::snprintf(buf, sizeof buf, "FRAME INSPECTOR [CTRL+F8]  VIEW: %s", inspector_view_name(s.view));
  out.push_back(buf);
  std::snprintf(buf, sizeof buf, "RENDER %ux%u -> OUTPUT %ux%u  %s  %s", s.render_w, s.render_h, s.output_w,
                s.output_h, s.hdr_mode.c_str(), s.upscaler.c_str());
  out.push_back(buf);
  std::snprintf(buf, sizeof buf, "CPU %.2f ms  GPU %.2f ms  VRS %s  Z-PRE %s", s.cpu_ms >= 0 ? s.cpu_ms : 0.0,
                s.gpu_ms >= 0 ? s.gpu_ms : 0.0, s.vrs_active ? "2x2" : "1x1", s.z_prepass ? "ON" : "OFF");
  out.push_back(buf);
  std::snprintf(buf, sizeof buf, "WORLD %u DRAWN / %u CULLED (%u TRIS)  SHADOW %u+%uC", s.world_draws, s.world_culled,
                s.world_triangles, s.shadow_draws, s.shadow_cars);
  out.push_back(buf);
  std::snprintf(buf, sizeof buf, "RESIDENCY %u CELLS (%u DRAWS)  VRAM %s / %s", s.resident_cells, s.resident_draws,
                format_bytes(s.vram_used).c_str(), format_bytes(s.vram_budget).c_str());
  out.push_back(buf);
  for (const auto& [name, ms] : s.pass_timings) {
    std::snprintf(buf, sizeof buf, "  %-13s %6.3f ms", name.c_str(), ms);
    out.push_back(buf);
  }
  return out;
}

void build_frame_inspector(ui::List& list, const FrameInspectorSnapshot& s, float width, float height) {
  if (!s.open || width <= 0 || height <= 0) return;
  const float k = std::clamp(height / 1080.0f, 0.7f, 1.5f);
  const float panel_w = std::min(430.0f * k, width * 0.42f);
  const float pad = 12.0f * k;
  const float x0 = width - panel_w - 14.0f * k;
  const float y0 = 14.0f * k;
  const float font_h = 15.0f * k;
  const float line_h = 20.0f * k;
  const float bar_h = 10.0f * k;
  const float preview_w = panel_w - 2.0f * pad;
  const float preview_h = preview_w * (9.0f / 16.0f);

  const size_t n_passes = std::min<size_t>(s.pass_timings.size(), 12);
  const float panel_h =
      pad * 3.0f + line_h * 5.2f + float(n_passes) * (line_h + 2.0f * k) + preview_h + 28.0f * k;

  // Dark sharp panel background + magenta Horizon accent bar.
  list.rect(x0, y0, x0 + panel_w, y0 + panel_h, ui::rgba(14, 16, 22, 228), 0.0f);
  list.frame(x0, y0, x0 + panel_w, y0 + panel_h, ui::rgba(62, 68, 86, 240), 1.5f, 0.0f);
  list.rect(x0, y0, x0 + panel_w, y0 + 3.0f * k, ui::rgba(249, 0, 100, 255), 0.0f);

  auto draw_txt = [&](float x, float ty, float sz, uint32_t col, std::string_view txt,
                      ui::Align al = ui::Align::kLeft) {
    ui::TextStyle st;
    st.size = sz;
    st.weight = 0.10f;
    st.color = col;
    st.align = al;
    st.keep_case = true;
    list.text(x, ty, txt, st);
  };

  float y = y0 + pad;
  const float tx = x0 + pad;
  char buf[160];
  std::snprintf(buf, sizeof buf, "FRAME INSPECTOR [CTRL+F8]");
  draw_txt(tx, y, font_h * 1.05f, ui::rgba(249, 0, 100, 255), buf);
  draw_txt(x0 + panel_w - pad, y, font_h * 0.9f, ui::rgba(218, 255, 0, 255), inspector_view_name(s.view),
           ui::Align::kRight);
  y += line_h * 1.15f;

  // Buffer tabs strip [1..6].
  const float tab_w = (panel_w - 2.0f * pad) / float(int(InspectorView::kCount));
  for (int i = 0; i < int(InspectorView::kCount); ++i) {
    const bool active = (int(s.view) == i);
    const float bx = tx + float(i) * tab_w;
    list.rect(bx + 1.0f, y, bx + tab_w - 1.0f, y + 18.0f * k,
              active ? ui::rgba(249, 0, 100, 230) : ui::rgba(32, 36, 46, 220), 0.0f);
    draw_txt(bx + tab_w * 0.5f, y + 2.5f * k, font_h * 0.78f,
             active ? ui::rgba(255, 255, 255, 255) : ui::rgba(185, 192, 208, 230),
             inspector_view_name(InspectorView(i)), ui::Align::kCenter);
  }
  y += 24.0f * k;

  std::snprintf(buf, sizeof buf, "%ux%u -> %ux%u | %s | %s", s.render_w, s.render_h, s.output_w, s.output_h,
                s.hdr_mode.c_str(), s.upscaler.c_str());
  draw_txt(tx, y, font_h * 0.85f, ui::rgba(235, 238, 245, 240), buf);
  y += line_h * 0.9f;

  auto draw_fit = [&](float x, float fy, float sz, uint32_t col, std::string_view text) {
    ui::TextStyle st;
    st.font = ui::Font::kB;
    st.size = sz;
    st.color = col;
    st.align = ui::Align::kLeft;
    st.keep_case = true;
    const float max_w = panel_w - 2.0f * pad;
    const float mw = ui::List::measure(text, st);
    if (mw > max_w && mw > 1.0f) st.size = std::max(7.5f * k, sz * (max_w / mw));
    list.text(x, fy, text, st);
  };
  std::snprintf(buf, sizeof buf, "DRAWS %u / CULLED %u (%u TRIS)  SHADOW %u+%uC", s.world_draws, s.world_culled,
                s.world_triangles, s.shadow_draws, s.shadow_cars);
  draw_fit(tx, y, font_h * 0.82f, ui::rgba(195, 210, 235, 235), buf);
  y += line_h * 0.9f;

  std::snprintf(buf, sizeof buf, "CELLS %u (%u)  VRAM %s / %s  VRS %s", s.resident_cells, s.resident_draws,
                format_bytes(s.vram_used).c_str(), format_bytes(s.vram_budget).c_str(),
                s.vrs_active ? "2x2" : "1x1");
  draw_fit(tx, y, font_h * 0.82f, ui::rgba(180, 192, 212, 230), buf);
  y += line_h * 1.1f;

  // GPU Pass Timings with proportional bars.
  double max_stage = 0.05;
  for (size_t i = 0; i < n_passes; ++i) max_stage = std::max(max_stage, s.pass_timings[i].second);
  const float bar_x = tx + 118.0f * k;
  const float bar_max_w = panel_w - 2.0f * pad - 195.0f * k;
  for (size_t i = 0; i < n_passes; ++i) {
    const auto& [name, ms] = s.pass_timings[i];
    draw_txt(tx, y, font_h * 0.84f, ui::rgba(220, 224, 232, 240), name);
    const float frac = float(std::clamp(ms / max_stage, 0.02, 1.0));
    list.rect(bar_x, y + 3.0f * k, bar_x + bar_max_w, y + 3.0f * k + bar_h, ui::rgba(32, 36, 46, 200), 0.0f);
    list.rect(bar_x, y + 3.0f * k, bar_x + bar_max_w * frac, y + 3.0f * k + bar_h, ui::rgba(0, 205, 225, 230), 0.0f);
    std::snprintf(buf, sizeof buf, "%.2f ms", ms);
    draw_txt(x0 + panel_w - pad, y, font_h * 0.84f, ui::rgba(235, 242, 250, 245), buf, ui::Align::kRight);
    y += line_h;
  }

  y += 6.0f * k;
  // Live buffer preview thumbnail box.
  list.rect(tx, y, tx + preview_w, y + preview_h, ui::rgba(10, 12, 16, 255), 0.0f);
  list.frame(tx, y, tx + preview_w, y + preview_h, ui::rgba(90, 96, 114, 255), 1.0f, 0.0f);
  if (s.has_preview) {
    list.image(tx + 1.0f, y + 1.0f, tx + preview_w - 1.0f, y + preview_h - 1.0f, s.preview_uv);
  } else {
    draw_txt(tx + preview_w * 0.5f, y + preview_h * 0.45f, font_h * 0.9f, ui::rgba(160, 168, 184, 220),
             "CAPTURING BUFFER...", ui::Align::kCenter);
  }
}

}  // namespace carview
