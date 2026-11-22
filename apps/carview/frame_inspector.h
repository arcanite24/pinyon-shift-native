#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"

namespace carview {

// Ctrl+F8 live Frame Inspector: shows GPU pass timings, draw-call / cull /
// residency counts, and a selectable live buffer preview overlay
// (Color, Depth, SSAO, Shadow Map, Motion Vectors, Hi-Z).
enum class InspectorView : int {
  kColor = 0,
  kDepth = 1,
  kAo = 2,
  kShadow = 3,
  kMotion = 4,
  kHiZ = 5,
  kCount = 6,
};

const char* inspector_view_name(InspectorView v);
InspectorView parse_inspector_view(const std::string& name);

struct FrameInspectorSnapshot {
  bool open = false;
  InspectorView view = InspectorView::kColor;
  uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
  double cpu_ms = -1.0, gpu_ms = -1.0;
  std::vector<std::pair<std::string, double>> pass_timings;
  uint32_t world_draws = 0, world_culled = 0, world_triangles = 0;
  uint32_t shadow_draws = 0, shadow_cars = 0;
  uint32_t resident_cells = 0, resident_draws = 0;
  uint64_t vram_used = 0, vram_budget = 0;
  bool vrs_active = false, reverse_z = true, z_prepass = false;
  std::string hdr_mode = "SDR";
  std::string upscaler = "BILINEAR";
  // Live buffer preview UV quad inside the renderer's UI atlas (0 = hidden).
  std::array<float, 4> preview_uv{};
  bool has_preview = false;
};

std::vector<std::string> frame_inspector_lines(const FrameInspectorSnapshot& s);

// Builds the Ctrl+F8 Frame Inspector panel on the right side of the viewport.
void build_frame_inspector(ui::List& list, const FrameInspectorSnapshot& s, float width, float height);

}  // namespace carview
