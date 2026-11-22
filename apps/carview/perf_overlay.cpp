#include "perf_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace carview {

void FrameStats::frame(double now_sec) {
  if (last_ >= 0.0) {
    deltas_.push_back((now_sec - last_) * 1000.0);
    if (deltas_.size() > keep_) deltas_.pop_front();
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

std::vector<std::string> perf_overlay_lines(const PerfInfo& p) {
  std::vector<std::string> out;
  char buf[200];
  if (p.generated_fps > 0.5)
    std::snprintf(buf, sizeof buf, "FPS %.0f   real %.0f + generated %.0f", p.fps + p.generated_fps, p.fps,
                  p.generated_fps);
  else
    std::snprintf(buf, sizeof buf, "FPS %.0f   real %.0f + generated 0", p.fps, p.fps);
  out.push_back(buf);
  std::snprintf(buf, sizeof buf, "RENDER %ux%u   OUTPUT %ux%u", p.render_w, p.render_h, p.output_w, p.output_h);
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
      line += "(no timestamps)";
    }
    out.push_back(line);
  }
  if (!p.gpu_stages.empty()) {
    std::string line = "GPU:";
    for (const auto& [name, ms] : p.gpu_stages) {
      std::snprintf(buf, sizeof buf, " %s %.2f", name.c_str(), ms);
      line += buf;
    }
    out.push_back(line);
  }
  if (p.vram_budget > 0 || p.vram_used > 0) {
    std::string line = "VRAM " + format_bytes(p.vram_used);
    if (p.vram_budget > 0) line += " / " + format_bytes(p.vram_budget);
    if (p.resident_cells || p.resident_draws) {
      std::snprintf(buf, sizeof buf, "   CELLS %u   DRAWS %u (drawn %u, culled %u)", p.resident_cells,
                    p.resident_draws, p.draws_drawn, p.draws_culled);
      line += buf;
    }
    out.push_back(line);
  }
  {
    std::string line = std::string("HDR ") + (p.hdr ? "ON" : "OFF");
    line += "   VSYNC " + (p.vsync.empty() ? std::string("-") : p.vsync);
    if (!p.upscaler.empty()) line += "   OUTPUT " + p.upscaler;
    if (!p.frame_generation.empty()) line += "   FRAMEGEN " + p.frame_generation;
    out.push_back(line);
  }
  if (!p.device.empty()) out.push_back(p.device);
  if (!p.build.empty()) out.push_back(p.build);
  return out;
}

void build_perf_overlay(ui::List& list, const PerfInfo& info, float width, float height, float scale) {
  const auto lines = perf_overlay_lines(info);
  if (lines.empty() || width <= 0 || height <= 0) return;
  // 1x at 1080 lines, growing with the window.
  const float k = std::clamp(height / 1080.0f, 0.6f, 4.0f) * std::max(scale, 0.25f);
  ui::TextStyle style;
  style.size = 13.0f * k;
  style.weight = 0.10f;
  style.color = ui::rgba(235, 240, 245);
  style.keep_case = true;
  const float pad = 8.0f * k, line_h = 20.0f * k;
  float w = 0.0f;
  for (const auto& l : lines) w = std::max(w, ui::List::measure(l, style));
  const float x0 = 10.0f * k, y0 = 10.0f * k;
  list.rect(x0, y0, x0 + w + 2 * pad, y0 + float(lines.size()) * line_h + 2 * pad - 4.0f * k, ui::rgba(8, 10, 14, 190),
            4.0f * k);
  for (size_t i = 0; i < lines.size(); ++i) {
    ui::TextStyle s = style;
    if (i == 0) s.color = ui::rgba(120, 230, 150);
    list.text(x0 + pad, y0 + pad + float(i) * line_h, lines[i], s);
  }
}

}  // namespace carview
