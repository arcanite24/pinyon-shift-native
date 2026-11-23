#include "perf_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace carview {

void FrameStats::frame(double now_sec) {
  if (last_ >= 0.0) {
    deltas_.push_back((now_sec - last_) * 1000.0);
    delta_stamps_.push_back(now_sec);
    if (deltas_.size() > keep_) {
      deltas_.pop_front();
      delta_stamps_.pop_front();
    }
  }
  last_ = now_sec;
  ++total_;
  stamps_.push_back(now_sec);
  while (!stamps_.empty() && now_sec - stamps_.front() > window_) stamps_.pop_front();
  while (!generated_recent_.empty() && now_sec - generated_recent_.front().first > window_)
    generated_recent_.pop_front();
}

double FrameStats::fps() const {
  if (stamps_.size() < 2) return 0.0;
  const double span = stamps_.back() - stamps_.front();
  return span > 1e-9 ? double(stamps_.size() - 1) / span : 0.0;
}

double FrameStats::generated_fps() const {
  if (stamps_.size() < 2) return 0.0;
  const double span = stamps_.back() - stamps_.front();
  if (span <= 1e-9) return 0.0;
  uint64_t n = 0;
  for (const auto& g : generated_recent_) n += g.second;
  return double(n) / span;
}

double FrameStats::ms_avg() const {
  if (deltas_.empty()) return 0.0;
  double s = 0.0;
  for (double d : deltas_) s += d;
  return s / double(deltas_.size());
}

double FrameStats::ms_percentile(double q) const {
  if (deltas_.empty()) return 0.0;
  std::vector<double> v(deltas_.begin(), deltas_.end());
  std::sort(v.begin(), v.end());
  const size_t k = std::min(v.size() - 1, size_t(std::clamp(q, 0.0, 1.0) * double(v.size() - 1) + 0.5));
  return v[k];
}

double FrameStats::ms_low_fps(double fraction, double window_sec, size_t max_frames) const {
  if (deltas_.empty()) return 0.0;
  std::vector<double> v;
  v.reserve(max_frames > 0 ? std::min(deltas_.size(), max_frames) : deltas_.size());
  for (size_t i = deltas_.size(); i > 0; --i) {
    const size_t idx = i - 1;
    if (max_frames > 0 && v.size() >= max_frames) break;
    if (window_sec > 0.0 && !v.empty() && idx < delta_stamps_.size() && (last_ - delta_stamps_[idx]) > window_sec) {
      break;
    }
    v.push_back(deltas_[idx]);
  }
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end(), std::greater<double>());
  const size_t n = std::max<size_t>(1, size_t(std::ceil(double(v.size()) * std::clamp(fraction, 1e-6, 1.0))));
  double s = 0.0;
  for (size_t i = 0; i < std::min(n, v.size()); ++i) s += v[i];
  const double avg_ms = s / double(std::min(n, v.size()));
  return avg_ms > 1e-6 ? 1000.0 / avg_ms : 0.0;
}

double FrameStats::ms_max() const {
  double m = 0.0;
  for (double d : deltas_) m = std::max(m, d);
  return m;
}

std::string FrameStats::summary() const {
  char buf[256];
  std::snprintf(buf, sizeof buf, "frames %zu, %.1f fps, frame ms avg %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f", total_,
                fps(), ms_avg(), ms_percentile(0.5), ms_percentile(0.95), ms_percentile(0.99), ms_max());
  return buf;
}

void FrameStats::reset() { *this = FrameStats(window_, keep_); }

std::string format_bytes(uint64_t bytes) {
  char buf[48];
  const double b = double(bytes);
  if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GiB", b / double(1ull << 30));
  else if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MiB", b / double(1ull << 20));
  else if (bytes >= (1ull << 10)) std::snprintf(buf, sizeof buf, "%.1f KiB", b / double(1ull << 10));
  else std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
  return buf;
}

namespace {

std::vector<std::pair<std::string, double>> top_gpu_passes(const PerfInfo& p, size_t max_count = 6) {
  std::vector<std::pair<std::string, double>> out;
  out.reserve(p.gpu_stages.size());
  for (const auto& [name, ms] : p.gpu_stages) {
    if (ms >= 0.005) out.emplace_back(name, ms);
  }
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  if (out.size() > max_count) out.resize(max_count);
  return out;
}

void fit_overlay_text(std::string_view text, ui::TextStyle& ts, float max_w, float min_size) {
  if (max_w <= 8.0f) return;
  while (ts.size > min_size && ui::List::measure(text, ts) > max_w) ts.size *= 0.92f;
}

}  // namespace

std::vector<std::string> perf_overlay_lines(const PerfInfo& p) {
  std::vector<std::string> out;
  char buf[200];
  if (p.generated_fps > 0.5)
    std::snprintf(buf, sizeof buf, "FPS %.0f  real %.0f + generated %.0f", p.fps + p.generated_fps, p.fps,
                  p.generated_fps);
  else
    std::snprintf(buf, sizeof buf, "FPS %.0f  real %.0f + generated 0", p.fps, p.fps);
  if (p.fps_1pct_low > 0.5) {
    char low_buf[48];
    std::snprintf(low_buf, sizeof low_buf, "  1%% low %.0f", p.fps_1pct_low);
    out.push_back(std::string(buf) + low_buf);
  } else {
    out.push_back(buf);
  }
  std::snprintf(buf, sizeof buf, "RENDER %ux%u  OUTPUT %ux%u", p.render_w, p.render_h, p.output_w, p.output_h);
  out.push_back(buf);
  if (p.cpu_ms >= 0.0 || p.gpu_ms >= 0.0) {
    std::string line = "CPU ";
    if (p.cpu_ms >= 0.0) {
      std::snprintf(buf, sizeof buf, "%.2f ms", p.cpu_ms);
      line += buf;
    } else {
      line += "-";
    }
    line += "   GPU ";
    if (p.gpu_ms >= 0.0) {
      std::snprintf(buf, sizeof buf, "%.2f ms", p.gpu_ms);
      line += buf;
    } else {
      line += "(n/a)";
    }
    out.push_back(line);
  }
  // Compact top-6 non-zero GPU passes wrapped into 2 columns (max 3 rows, zero sprawling line).
  const auto passes = top_gpu_passes(p, 6);
  for (size_t i = 0; i < passes.size(); i += 2) {
    char left[64];
    std::snprintf(left, sizeof left, "%s %.2fms", passes[i].first.c_str(), passes[i].second);
    if (i + 1 < passes.size()) {
      char right[64];
      std::snprintf(right, sizeof right, "  |  %s %.2fms", passes[i + 1].first.c_str(), passes[i + 1].second);
      out.push_back(std::string(left) + right);
    } else {
      out.push_back(left);
    }
  }
  if (p.vram_budget > 0 || p.vram_used > 0 || p.rss_mib > 0.5) {
    std::string line = "VRAM " + format_bytes(p.vram_used);
    if (p.vram_budget > 0) line += " / " + format_bytes(p.vram_budget);
    if (p.rss_mib > 0.5) {
      std::snprintf(buf, sizeof buf, "  RAM %.0f MiB", p.rss_mib);
      line += buf;
    }
    out.push_back(line);
  }
  if (p.resident_cells || p.resident_draws) {
    std::snprintf(buf, sizeof buf, "DRAWS %u drawn / %u culled (%u res)", p.draws_drawn, p.draws_culled,
                  p.resident_draws ? p.resident_draws : p.resident_cells);
    out.push_back(buf);
  }
  {
    std::string line = std::string("HDR ") + (p.hdr ? "ON" : "OFF");
    line += "  VSYNC " + (p.vsync.empty() ? std::string("-") : p.vsync);
    if (!p.upscaler.empty()) line += "  " + p.upscaler;
    if (!p.frame_generation.empty() && p.frame_generation != "OFF") line += "  FG " + p.frame_generation;
    out.push_back(line);
  }
  if (!p.device.empty()) out.push_back(p.device);
  if (!p.build.empty()) out.push_back(p.build);
  return out;
}

std::array<float, 4> perf_overlay_rect(const PerfInfo& info, float width, float height, float scale) {
  if (width <= 0 || height <= 0) return {0, 0, 0, 0};
  const float k = std::clamp(height / 1080.0f, 0.65f, 3.0f) * std::max(scale, 0.25f);
  const float pad_x = 12.0f * k;
  const float pad_y = 9.0f * k;
  const float row_h = 18.0f * k;
  const float pass_h = 15.5f * k;
  const auto passes = top_gpu_passes(info, 6);
  const int pass_rows = int((passes.size() + 1) / 2);

  int main_rows = 3;  // FPS, CPU/GPU, RES
  if (info.vram_budget > 0 || info.vram_used > 0 || info.rss_mib > 0.5) ++main_rows;
  if (info.resident_cells || info.resident_draws) ++main_rows;
  ++main_rows;  // VSync/HDR/Upscaler
  if (!info.device.empty() || !info.build.empty()) ++main_rows;

  const float panel_w = std::min(392.0f * k, std::max(240.0f * k, width * 0.36f));
  const float panel_h =
      2.0f * pad_y + float(main_rows) * row_h + (pass_rows > 0 ? (6.0f * k + float(pass_rows) * pass_h) : 0.0f);
  const float x0 = 12.0f * k;
  const float y0 = 12.0f * k;
  return {x0, y0, x0 + panel_w, y0 + panel_h};
}

void build_perf_overlay(ui::List& list, const PerfInfo& info, float width, float height, float scale) {
  if (width <= 0 || height <= 0) return;
  const float k = std::clamp(height / 1080.0f, 0.65f, 3.0f) * std::max(scale, 0.25f);
  const auto r = perf_overlay_rect(info, width, height, scale);
  const float x0 = r[0], y0 = r[1], x1 = r[2], y1 = r[3];
  const float panel_w = x1 - x0;
  const float pad_x = 12.0f * k;
  const float pad_y = 9.0f * k;
  const float row_h = 18.0f * k;
  const float pass_h = 15.5f * k;
  const float inner_w = panel_w - 2.0f * pad_x;

  // Compact translucent dark telemetry card with Horizon pink left accent bar.
  list.rect(x0, y0, x1, y1, ui::rgba(10, 12, 16, 236), 3.0f * k);
  list.frame(x0, y0, x1, y1, ui::rgba(255, 255, 255, 36), 1.0f * k, 3.0f * k);
  list.rect(x0, y0, x0 + 3.0f * k, y1, ui::rgba(0xF9, 0x00, 0x64, 240));

  auto make_ts = [&](float sz, uint32_t col, ui::Align align = ui::Align::kLeft) {
    ui::TextStyle ts;
    ts.size = sz * k;
    ts.weight = 0.11f;
    ts.color = col;
    ts.align = align;
    ts.font = ui::Font::kB;
    ts.keep_case = true;
    return ts;
  };

  float y = y0 + pad_y;
  char buf[160];

  // Row 0: FPS / 1% Low / Frame ms
  {
    const double shown_fps = info.generated_fps > 0.5 ? (info.fps + info.generated_fps) : info.fps;
    const double frame_ms = shown_fps > 0.5 ? (1000.0 / shown_fps) : 0.0;
    const uint32_t fps_col = shown_fps >= 55.0   ? ui::rgba(108, 236, 146)
                             : shown_fps >= 30.0 ? ui::rgba(255, 206, 72)
                                                 : ui::rgba(255, 102, 92);
    if (info.generated_fps > 0.5)
      std::snprintf(buf, sizeof buf, "FPS %.0f (%.0f+%.0f)", shown_fps, info.fps, info.generated_fps);
    else
      std::snprintf(buf, sizeof buf, "FPS %.0f", shown_fps);
    list.text(x0 + pad_x, y, buf, make_ts(12.5f, fps_col));

    if (info.fps_1pct_low > 0.5) {
      std::snprintf(buf, sizeof buf, "1%% LOW %.0f", info.fps_1pct_low);
      list.text(x0 + pad_x + inner_w * 0.44f, y + 0.8f * k, buf, make_ts(11.0f, ui::rgba(218, 226, 238)));
    }
    if (frame_ms > 0.0) {
      std::snprintf(buf, sizeof buf, "%.1f ms", frame_ms);
      list.text(x1 - pad_x, y + 0.8f * k, buf, make_ts(11.0f, ui::rgba(176, 184, 198), ui::Align::kRight));
    }
    y += row_h;
  }

  // Row 1: CPU / GPU timing
  {
    std::string left = "CPU ";
    if (info.cpu_ms >= 0.0) {
      std::snprintf(buf, sizeof buf, "%.2f ms", info.cpu_ms);
      left += buf;
    } else {
      left += "-";
    }
    list.text(x0 + pad_x, y, left, make_ts(11.2f, ui::rgba(232, 236, 244)));

    std::string right = "GPU ";
    if (info.gpu_ms >= 0.0) {
      std::snprintf(buf, sizeof buf, "%.2f ms", info.gpu_ms);
      right += buf;
    } else {
      right += "n/a";
    }
    list.text(x0 + pad_x + inner_w * 0.50f, y, right, make_ts(11.2f, ui::rgba(232, 236, 244)));
    y += row_h;
  }

  // Compact 2-column top-6 non-zero GPU pass breakdown
  const auto passes = top_gpu_passes(info, 6);
  if (!passes.empty()) {
    list.rect(x0 + pad_x, y + 1.0f * k, x1 - pad_x, y + 2.0f * k, ui::rgba(255, 255, 255, 24));
    y += 5.0f * k;
    const float col_w = (inner_w - 10.0f * k) * 0.5f;
    for (size_t i = 0; i < passes.size(); i += 2) {
      for (int col = 0; col < 2 && (i + size_t(col)) < passes.size(); ++col) {
        const auto& [name, ms] = passes[i + size_t(col)];
        const float cx = x0 + pad_x + float(col) * (col_w + 10.0f * k);
        ui::TextStyle ns = make_ts(10.0f, ui::rgba(174, 182, 196));
        fit_overlay_text(name, ns, col_w - 46.0f * k, 7.5f * k);
        list.text(cx, y, name, ns);
        std::snprintf(buf, sizeof buf, "%.2f ms", ms);
        list.text(cx + col_w, y, buf, make_ts(10.0f, ui::rgba(222, 228, 238), ui::Align::kRight));
      }
      y += pass_h;
    }
    y += 1.0f * k;
  }

  // Row: Render -> Output resolution
  {
    std::snprintf(buf, sizeof buf, "RES %ux%u -> %ux%u", info.render_w, info.render_h, info.output_w, info.output_h);
    ui::TextStyle ts = make_ts(10.8f, ui::rgba(216, 222, 232));
    fit_overlay_text(buf, ts, inner_w, 8.0f * k);
    list.text(x0 + pad_x, y, buf, ts);
    y += row_h;
  }

  // Row: VRAM / RAM
  if (info.vram_budget > 0 || info.vram_used > 0 || info.rss_mib > 0.5) {
    std::string vram = "VRAM " + format_bytes(info.vram_used);
    if (info.vram_budget > 0) vram += " / " + format_bytes(info.vram_budget);
    ui::TextStyle vs = make_ts(10.6f, ui::rgba(216, 222, 232));
    fit_overlay_text(vram, vs, info.rss_mib > 0.5 ? inner_w * 0.66f : inner_w, 8.0f * k);
    list.text(x0 + pad_x, y, vram, vs);
    if (info.rss_mib > 0.5) {
      std::snprintf(buf, sizeof buf, "RAM %.0f MiB", info.rss_mib);
      list.text(x1 - pad_x, y, buf, make_ts(10.6f, ui::rgba(178, 186, 200), ui::Align::kRight));
    }
    y += row_h;
  }

  // Row: Resident / Drawn / Culled draws (when active)
  if (info.resident_cells || info.resident_draws) {
    std::snprintf(buf, sizeof buf, "DRAWS %u drawn / %u culled (%u res)", info.draws_drawn, info.draws_culled,
                  info.resident_draws ? info.resident_draws : info.resident_cells);
    ui::TextStyle ts = make_ts(10.2f, ui::rgba(182, 190, 204));
    fit_overlay_text(buf, ts, inner_w, 7.5f * k);
    list.text(x0 + pad_x, y, buf, ts);
    y += row_h;
  }

  // Row: VSync / HDR / Upscaler / FrameGen
  {
    std::string flags = "VSYNC " + (info.vsync.empty() ? std::string("-") : info.vsync);
    flags += std::string("  HDR ") + (info.hdr ? "ON" : "OFF");
    if (!info.upscaler.empty()) flags += "  " + info.upscaler;
    if (!info.frame_generation.empty() && info.frame_generation != "OFF") flags += "  FG " + info.frame_generation;
    ui::TextStyle ts = make_ts(10.2f, ui::rgba(196, 204, 218));
    fit_overlay_text(flags, ts, inner_w, 7.5f * k);
    list.text(x0 + pad_x, y, flags, ts);
    y += row_h;
  }

  // Footer: GPU Device + Real Build Version
  if (!info.device.empty() || !info.build.empty()) {
    std::string foot = info.device;
    if (!info.build.empty()) {
      if (!foot.empty()) foot += "  |  ";
      foot += info.build;
    }
    ui::TextStyle ts = make_ts(9.8f, ui::rgba(148, 156, 170));
    fit_overlay_text(foot, ts, inner_w, 7.5f * k);
    list.text(x0 + pad_x, y, foot, ts);
  }
}

}  // namespace carview
