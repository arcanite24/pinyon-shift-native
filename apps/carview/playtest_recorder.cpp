#include "playtest_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#include <stb_image_write.h>

namespace carview {
namespace {

void append_json_double(char* dst, size_t cap, size_t& pos, double v, const char* fmt) {
  if (pos >= cap) return;
  int n = 0;
  if (std::isfinite(v)) {
    n = std::snprintf(dst + pos, cap - pos, fmt, v);
  } else {
    n = std::snprintf(dst + pos, cap - pos, "null");
  }
  if (n > 0) pos += std::min(size_t(n), cap - pos - 1);
}

void write_json_num(std::ostringstream& os, double v, int precision) {
  if (std::isfinite(v)) {
    os << std::fixed << std::setprecision(precision) << v;
  } else {
    os << "null";
  }
}

double wrap_hour_24(double h) {
  if (!std::isfinite(h)) return 12.0;
  double r = std::fmod(h, 24.0);
  if (r < 0.0) r += 24.0;
  return r;
}

}  // namespace

PlaytestRecorder::~PlaytestRecorder() {
  if (active_) finish();
}

bool PlaytestRecorder::is_hitch(double frame_ms) {
  return std::isfinite(frame_ms) && frame_ms > kHitchThresholdMs;
}

bool PlaytestRecorder::has_non_finite(const PlaytestFrameSample& s) {
  if (!std::isfinite(s.t) || !std::isfinite(s.frame_ms) || !std::isfinite(s.cpu_ms) ||
      !std::isfinite(s.gpu_ms) || !std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z) ||
      !std::isfinite(s.heading) || !std::isfinite(s.speed_kmh) || !std::isfinite(s.rpm) ||
      !std::isfinite(s.tod_hour)) {
    return true;
  }
  if (s.has_road_y && !std::isfinite(s.road_y)) return true;
  return false;
}

bool PlaytestRecorder::is_ground_anomaly(const PlaytestFrameSample& s) {
  if (!std::isfinite(s.y)) return true;
  if (s.has_road_y && !std::isfinite(s.road_y)) return true;
  // Paused / menu / loading / pre-race-grid frames before physics snaps the car
  // onto the road are not live driving ground anomalies.
  if (s.paused) return false;
  if (!s.has_road_y) return true;
  const double dy = s.y - s.road_y;
  return dy < kGroundBelowThresholdM || dy > kGroundAboveThresholdM;
}

double PlaytestRecorder::compute_percentile(const std::vector<double>& sorted_vals, double q) {
  if (sorted_vals.empty()) return 0.0;
  if (sorted_vals.size() == 1) return sorted_vals.front();
  const double clamped = std::clamp(q, 0.0, 1.0);
  const double idx = clamped * double(sorted_vals.size() - 1);
  const size_t lo = size_t(std::floor(idx));
  const size_t hi = std::min(sorted_vals.size() - 1, lo + 1);
  const double frac = idx - double(lo);
  return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * frac;
}

std::string PlaytestRecorder::json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char esc[8];
          std::snprintf(esc, sizeof esc, "\\u%04x", unsigned(c));
          out += esc;
        } else {
          out.push_back(char(c));
        }
        break;
    }
  }
  return out;
}

std::string PlaytestRecorder::utc_timestamp_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
#ifdef _WIN32
  gmtime_s(&tm_buf, &tt);
#else
  gmtime_r(&tt, &tm_buf);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
  return buf;
}

std::string PlaytestRecorder::format_repro_cmd(const PlaytestFrameSample& s) {
  const std::string car_arg = s.car.empty() ? "VIP_Viper_13" : s.car;
  const bool need_quotes = car_arg.find(' ') != std::string::npos;
  const double sx = std::isfinite(s.x) ? s.x : 0.0;
  const double sy = std::isfinite(s.y) ? s.y : 0.0;
  const double sz = std::isfinite(s.z) ? s.z : 0.0;
  const double sh = std::isfinite(s.heading) ? s.heading : 0.0;
  const double hour = wrap_hour_24(s.tod_hour);
  char buf[512];
  if (need_quotes) {
    std::snprintf(buf, sizeof buf,
                  "carview.exe --drive --car \"%s\" --spawn %.2f %.2f %.2f %.4f --hour %.2f",
                  car_arg.c_str(), sx, sy, sz, sh, hour);
  } else {
    std::snprintf(buf, sizeof buf,
                  "carview.exe --drive --car %s --spawn %.2f %.2f %.2f %.4f --hour %.2f",
                  car_arg.c_str(), sx, sy, sz, sh, hour);
  }
  return buf;
}

std::string PlaytestRecorder::format_telemetry_line(const PlaytestFrameSample& s, const char* reason,
                                                    uint64_t audio_stall_delta, bool non_finite) {
  const std::string esc_car = json_escape(s.car);
  char buf[1024];
  size_t pos = 0;
  auto emit = [&](const char* str) {
    if (pos >= sizeof(buf)) return;
    const int n = std::snprintf(buf + pos, sizeof(buf) - pos, "%s", str);
    if (n > 0) pos += std::min(size_t(n), sizeof(buf) - pos - 1);
  };

  emit("{\"t\":");
  append_json_double(buf, sizeof(buf), pos, s.t, "%.3f");
  emit(",\"frame_ms\":");
  append_json_double(buf, sizeof(buf), pos, s.frame_ms, "%.3f");
  emit(",\"cpu_ms\":");
  append_json_double(buf, sizeof(buf), pos, s.cpu_ms, "%.3f");
  emit(",\"gpu_ms\":");
  append_json_double(buf, sizeof(buf), pos, s.gpu_ms, "%.3f");
  emit(",\"x\":");
  append_json_double(buf, sizeof(buf), pos, s.x, "%.3f");
  emit(",\"y\":");
  append_json_double(buf, sizeof(buf), pos, s.y, "%.3f");
  emit(",\"z\":");
  append_json_double(buf, sizeof(buf), pos, s.z, "%.3f");
  emit(",\"road_y\":");
  if (s.has_road_y) {
    append_json_double(buf, sizeof(buf), pos, s.road_y, "%.3f");
  } else {
    emit("null");
  }
  emit(",\"heading\":");
  append_json_double(buf, sizeof(buf), pos, s.heading, "%.4f");
  emit(",\"speed_kmh\":");
  append_json_double(buf, sizeof(buf), pos, s.speed_kmh, "%.2f");

  if (pos < sizeof(buf)) {
    const int n = std::snprintf(buf + pos, sizeof(buf) - pos, ",\"gear\":%d,\"rpm\":", s.gear);
    if (n > 0) pos += std::min(size_t(n), sizeof(buf) - pos - 1);
  }
  append_json_double(buf, sizeof(buf), pos, s.rpm, "%.1f");

  if (pos < sizeof(buf)) {
    const int n = std::snprintf(buf + pos, sizeof(buf) - pos, ",\"car\":\"%s\",\"tod_hour\":", esc_car.c_str());
    if (n > 0) pos += std::min(size_t(n), sizeof(buf) - pos - 1);
  }
  append_json_double(buf, sizeof(buf), pos, s.tod_hour, "%.3f");

  if (pos < sizeof(buf)) {
    const int n = std::snprintf(buf + pos, sizeof(buf) - pos,
                                ",\"chunks_loaded\":%u,\"paused\":%s,\"in_race\":%s,\"audio_stalls\":%llu",
                                s.chunks_loaded, s.paused ? "true" : "false", s.in_race ? "true" : "false",
                                static_cast<unsigned long long>(audio_stall_delta));
    if (n > 0) pos += std::min(size_t(n), sizeof(buf) - pos - 1);
  }
  if (non_finite) emit(",\"non_finite\":true");
  if (pos < sizeof(buf)) {
    const int n = std::snprintf(buf + pos, sizeof(buf) - pos, ",\"reason\":\"%s\"}\n", reason ? reason : "periodic");
    if (n > 0) pos += std::min(size_t(n), sizeof(buf) - pos - 1);
  }
  return std::string(buf, pos);
}

std::string PlaytestRecorder::format_bug_json(const PlaytestBugEntry& entry, const std::string& utc_iso) {
  const auto& s = entry.snapshot;
  std::ostringstream os;
  os << "{\n";
  os << "  \"bug_index\": " << entry.index << ",\n";
  os << "  \"timestamp_utc\": \"" << json_escape(utc_iso) << "\",\n";
  os << "  \"t\": ";
  write_json_num(os, entry.t, 3);
  os << ",\n";
  os << "  \"screenshot\": \"" << json_escape(entry.png_file) << "\",\n";
  os << "  \"screenshot_saved\": " << (entry.screenshot_saved ? "true" : "false") << ",\n";
  os << "  \"repro_cmd\": \"" << json_escape(entry.repro_cmd) << "\",\n";
  os << "  \"state\": {\n";
  os << "    \"t\": "; write_json_num(os, s.t, 3); os << ",\n";
  os << "    \"frame_ms\": "; write_json_num(os, s.frame_ms, 3); os << ",\n";
  os << "    \"cpu_ms\": "; write_json_num(os, s.cpu_ms, 3); os << ",\n";
  os << "    \"gpu_ms\": "; write_json_num(os, s.gpu_ms, 3); os << ",\n";
  os << "    \"x\": "; write_json_num(os, s.x, 3); os << ",\n";
  os << "    \"y\": "; write_json_num(os, s.y, 3); os << ",\n";
  os << "    \"z\": "; write_json_num(os, s.z, 3); os << ",\n";
  os << "    \"road_y\": ";
  if (s.has_road_y) write_json_num(os, s.road_y, 3);
  else os << "null";
  os << ",\n";
  os << "    \"y_minus_road_y\": ";
  if (s.has_road_y && std::isfinite(s.y) && std::isfinite(s.road_y)) write_json_num(os, s.y - s.road_y, 3);
  else os << "null";
  os << ",\n";
  os << "    \"heading\": "; write_json_num(os, s.heading, 4); os << ",\n";
  os << "    \"speed_kmh\": "; write_json_num(os, s.speed_kmh, 2); os << ",\n";
  os << "    \"gear\": " << s.gear << ",\n";
  os << "    \"rpm\": "; write_json_num(os, s.rpm, 1); os << ",\n";
  os << "    \"car\": \"" << json_escape(s.car) << "\",\n";
  os << "    \"tod_hour\": "; write_json_num(os, s.tod_hour, 3); os << ",\n";
  os << "    \"chunks_loaded\": " << s.chunks_loaded << ",\n";
  os << "    \"paused\": " << (s.paused ? "true" : "false") << ",\n";
  os << "    \"in_race\": " << (s.in_race ? "true" : "false") << ",\n";
  os << "    \"audio_stalls\": " << s.audio_stalls << ",\n";
  os << "    \"camera_mode\": \"" << json_escape(s.camera_mode) << "\"\n";
  os << "  }\n";
  os << "}\n";
  return os.str();
}

std::string PlaytestRecorder::format_summary_json(const PlaytestSummary& summary, const std::filesystem::path& dir,
                                                  const std::string& started_utc, const std::string& finished_utc) {
  std::ostringstream os;
  os << "{\n";
  os << "  \"session_dir\": \"" << json_escape(dir.string()) << "\",\n";
  os << "  \"started_utc\": \"" << json_escape(started_utc) << "\",\n";
  os << "  \"finished_utc\": \"" << json_escape(finished_utc) << "\",\n";
  os << "  \"total_frames\": " << summary.total_frames << ",\n";
  os << "  \"duration_s\": "; write_json_num(os, summary.duration_s, 3); os << ",\n";
  os << "  \"p50_ms\": "; write_json_num(os, summary.p50_ms, 3); os << ",\n";
  os << "  \"p95_ms\": "; write_json_num(os, summary.p95_ms, 3); os << ",\n";
  os << "  \"p99_ms\": "; write_json_num(os, summary.p99_ms, 3); os << ",\n";
  os << "  \"max_ms\": "; write_json_num(os, summary.max_ms, 3); os << ",\n";
  os << "  \"hitch_threshold_ms\": "; write_json_num(os, kHitchThresholdMs, 1); os << ",\n";
  os << "  \"hitch_count\": " << summary.hitch_count << ",\n";
  os << "  \"top_hitches\": [";
  if (!summary.top_hitches.empty()) os << "\n";
  for (size_t i = 0; i < summary.top_hitches.size(); ++i) {
    const auto& h = summary.top_hitches[i];
    os << "    {\"t\": "; write_json_num(os, h.t, 3);
    os << ", \"frame_ms\": "; write_json_num(os, h.frame_ms, 3);
    os << ", \"cpu_ms\": "; write_json_num(os, h.cpu_ms, 3);
    os << ", \"gpu_ms\": "; write_json_num(os, h.gpu_ms, 3);
    os << ", \"x\": "; write_json_num(os, h.x, 2);
    os << ", \"y\": "; write_json_num(os, h.y, 2);
    os << ", \"z\": "; write_json_num(os, h.z, 2);
    os << ", \"speed_kmh\": "; write_json_num(os, h.speed_kmh, 1);
    os << ", \"car\": \"" << json_escape(h.car) << "\"";
    os << ", \"chunks_loaded\": " << h.chunks_loaded;
    os << ", \"paused\": " << (h.paused ? "true" : "false");
    os << ", \"in_race\": " << (h.in_race ? "true" : "false");
    os << "}" << (i + 1 < summary.top_hitches.size() ? ",\n" : "\n");
  }
  os << (summary.top_hitches.empty() ? "],\n" : "  ],\n");
  os << "  \"ground_anomaly_count\": " << summary.ground_anomaly_count << ",\n";
  os << "  \"non_finite_count\": " << summary.non_finite_count << ",\n";
  os << "  \"audio_glitch_count\": " << summary.audio_glitch_count << ",\n";
  os << "  \"telemetry_file\": \"telemetry.jsonl\",\n";
  os << "  \"telemetry_lines_written\": " << summary.telemetry_lines_written << ",\n";
  os << "  \"bugs\": [";
  if (!summary.bugs.empty()) os << "\n";
  for (size_t i = 0; i < summary.bugs.size(); ++i) {
    const auto& b = summary.bugs[i];
    os << "    {\"index\": " << b.index;
    os << ", \"t\": "; write_json_num(os, b.t, 3);
    os << ", \"png\": \"" << json_escape(b.png_file) << "\"";
    os << ", \"json\": \"" << json_escape(b.json_file) << "\"";
    os << ", \"screenshot_saved\": " << (b.screenshot_saved ? "true" : "false");
    os << ", \"repro_cmd\": \"" << json_escape(b.repro_cmd) << "\"";
    os << "}" << (i + 1 < summary.bugs.size() ? ",\n" : "\n");
  }
  os << (summary.bugs.empty() ? "]\n" : "  ]\n");
  os << "}\n";
  return os.str();
}

bool PlaytestRecorder::start(const std::filesystem::path& dir, std::string* error) {
  if (active_) finish();
  if (dir.empty()) {
    if (error) *error = "empty playtest session directory";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    if (error) *error = "cannot create playtest directory " + dir.string() + ": " + ec.message();
    return false;
  }
  const auto telemetry_path = dir / "telemetry.jsonl";
  std::FILE* fp = std::fopen(telemetry_path.string().c_str(), "wb");
  if (!fp) {
    if (error) *error = "cannot open " + telemetry_path.string() + " for writing";
    return false;
  }
  std::setvbuf(fp, nullptr, _IOFBF, 65536);

  dir_ = dir;
  started_utc_ = utc_timestamp_iso8601();
  start_tp_ = std::chrono::steady_clock::now();
  telemetry_file_ = fp;

  active_buf_.clear();
  active_buf_.reserve(kFlushBytesThreshold * 2);
  write_queue_.clear();
  png_queue_.clear();
  stop_writer_ = false;
  writer_busy_ = false;

  total_frames_ = 0;
  first_sample_t_ = 0.0;
  last_sample_t_ = 0.0;
  have_sample_t_ = false;
  last_periodic_t_ = -1e9;
  have_audio_stalls_ = false;
  last_audio_stalls_ = 0;
  have_valid_road_y_ = false;
  last_valid_road_y_ = 0.0;
  last_valid_x_ = 0.0;
  last_valid_z_ = 0.0;
  last_valid_t_ = 0.0;

  frame_ms_samples_.clear();
  frame_ms_samples_.reserve(16384);
  hitch_count_ = 0;
  top_hitches_.clear();
  top_hitches_.reserve(kTopHitchesMax + 1);
  ground_anomaly_count_ = 0;
  non_finite_count_ = 0;
  audio_glitch_count_ = 0;
  telemetry_lines_written_ = 0;

  bug_counter_ = 0;
  last_bug_mark_t_ = -1e9;
  bugs_.clear();
  last_summary_ = PlaytestSummary{};

  active_ = true;
  writer_thread_ = std::thread(&PlaytestRecorder::writer_main, this);
  return true;
}

double PlaytestRecorder::elapsed_seconds() const {
  if (!active_) return 0.0;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_tp_).count();
}

void PlaytestRecorder::record_hitch(const PlaytestFrameSample& s) {
  ++hitch_count_;
  PlaytestHitchEntry e;
  e.t = std::isfinite(s.t) ? s.t : 0.0;
  e.frame_ms = s.frame_ms;
  e.cpu_ms = std::isfinite(s.cpu_ms) ? s.cpu_ms : 0.0;
  e.gpu_ms = std::isfinite(s.gpu_ms) ? s.gpu_ms : -1.0;
  e.x = std::isfinite(s.x) ? s.x : 0.0;
  e.y = std::isfinite(s.y) ? s.y : 0.0;
  e.z = std::isfinite(s.z) ? s.z : 0.0;
  e.speed_kmh = std::isfinite(s.speed_kmh) ? s.speed_kmh : 0.0;
  e.car = s.car;
  e.chunks_loaded = s.chunks_loaded;
  e.paused = s.paused;
  e.in_race = s.in_race;

  if (top_hitches_.size() < kTopHitchesMax) {
    top_hitches_.push_back(std::move(e));
    std::sort(top_hitches_.begin(), top_hitches_.end(),
              [](const PlaytestHitchEntry& a, const PlaytestHitchEntry& b) { return a.frame_ms > b.frame_ms; });
  } else if (e.frame_ms > top_hitches_.back().frame_ms) {
    top_hitches_.back() = std::move(e);
    std::sort(top_hitches_.begin(), top_hitches_.end(),
              [](const PlaytestHitchEntry& a, const PlaytestHitchEntry& b) { return a.frame_ms > b.frame_ms; });
  }
}

void PlaytestRecorder::record_frame(const PlaytestFrameSample& sample) {
  if (!active_) return;
  ++total_frames_;

  if (std::isfinite(sample.t)) {
    if (!have_sample_t_) {
      first_sample_t_ = sample.t;
      have_sample_t_ = true;
    }
    if (sample.t > last_sample_t_) last_sample_t_ = sample.t;
  }

  if (std::isfinite(sample.frame_ms) && sample.frame_ms >= 0.0) {
    frame_ms_samples_.push_back(sample.frame_ms);
  }

  uint64_t stall_delta = 0;
  if (!have_audio_stalls_) {
    have_audio_stalls_ = true;
    last_audio_stalls_ = sample.audio_stalls;
  } else if (sample.audio_stalls > last_audio_stalls_) {
    stall_delta = sample.audio_stalls - last_audio_stalls_;
    last_audio_stalls_ = sample.audio_stalls;
  }
  const bool audio_glitch = sample.audio_underrun || stall_delta > 0;
  if (audio_glitch) ++audio_glitch_count_;

  const bool non_finite = has_non_finite(sample);
  if (non_finite) ++non_finite_count_;

  const bool hitch = is_hitch(sample.frame_ms);
  if (hitch) record_hitch(sample);

  const double sample_t = std::isfinite(sample.t) ? sample.t : last_sample_t_;

  bool ground_anom = is_ground_anomaly(sample);
  const bool finite_pos = std::isfinite(sample.x) && std::isfinite(sample.y) && std::isfinite(sample.z);
  if (have_valid_road_y_ && finite_pos) {
    const double dx = sample.x - last_valid_x_;
    const double dz = sample.z - last_valid_z_;
    if (dx * dx + dz * dz > 25.0 * 25.0 || (sample_t - last_valid_t_) > 2.0 || sample_t < last_valid_t_) {
      have_valid_road_y_ = false;
    }
  }

  bool overhead_probe_hit = false;
  if (ground_anom && !sample.paused && finite_pos && sample.has_road_y && std::isfinite(sample.road_y)) {
    const double dy = sample.y - sample.road_y;
    // Detect overhead roof / bridge beam / leaning wall hit by the +2.2 m downward probe:
    // road_y jumps sharply upward (> 1.0 m above the continuous road surface) into the
    // probe headroom while the car itself stays at/above the continuous road.
    if (dy < kGroundBelowThresholdM && have_valid_road_y_ &&
        sample.road_y > last_valid_road_y_ + 1.0 &&
        sample.y >= last_valid_road_y_ + kGroundBelowThresholdM &&
        sample.road_y <= sample.y + 2.35) {
      ground_anom = false;
      overhead_probe_hit = true;
      last_valid_x_ = sample.x;
      last_valid_z_ = sample.z;
      last_valid_t_ = sample_t;
    }
  }

  if (!ground_anom && !overhead_probe_hit && !sample.paused && finite_pos && sample.has_road_y &&
      std::isfinite(sample.road_y)) {
    have_valid_road_y_ = true;
    last_valid_road_y_ = sample.road_y;
    last_valid_x_ = sample.x;
    last_valid_z_ = sample.z;
    last_valid_t_ = sample_t;
  }

  if (ground_anom) ++ground_anomaly_count_;

  const bool periodic = (last_periodic_t_ < -1e8) || ((sample_t - last_periodic_t_) >= kPeriodicIntervalSec - 1e-6);

  if (!hitch && !ground_anom && !non_finite && !audio_glitch && !periodic) return;

  if (periodic) last_periodic_t_ = sample_t;

  const char* reason = "periodic";
  if (ground_anom || non_finite) reason = "ground_anomaly";
  else if (hitch) reason = "hitch";
  else if (audio_glitch) reason = "audio_glitch";

  append_line(format_telemetry_line(sample, reason, stall_delta, non_finite));
}

bool PlaytestRecorder::can_trigger_bug_mark(double now_s) const {
  if (!active_) return false;
  if (!std::isfinite(now_s)) return true;
  return (now_s - last_bug_mark_t_) >= kBugDebounceSec;
}

std::string PlaytestRecorder::save_bug_bookmark(const PlaytestFrameSample& sample, std::vector<uint8_t> rgba,
                                                uint32_t width, uint32_t height) {
  if (!active_) return {};
  const double mark_t = std::isfinite(sample.t) ? sample.t : elapsed_seconds();
  if (!can_trigger_bug_mark(mark_t)) return {};

  last_bug_mark_t_ = mark_t;
  const uint32_t idx = ++bug_counter_;

  char png_name[32], json_name[32];
  std::snprintf(png_name, sizeof png_name, "bug_%03u.png", idx);
  std::snprintf(json_name, sizeof json_name, "bug_%03u.json", idx);

  const bool has_pixels = !rgba.empty() && width > 0 && height > 0 &&
                          rgba.size() >= size_t(width) * size_t(height) * 4;

  PlaytestBugEntry entry;
  entry.index = idx;
  entry.t = mark_t;
  entry.png_file = png_name;
  entry.json_file = json_name;
  entry.screenshot_saved = has_pixels;
  entry.repro_cmd = format_repro_cmd(sample);
  entry.snapshot = sample;

  // Write bug_00N.json immediately so state + repro_cmd survive even an abrupt crash.
  {
    const auto json_path = dir_ / json_name;
    std::ofstream out(json_path, std::ios::binary);
    if (out) {
      out << format_bug_json(entry, utc_timestamp_iso8601());
    }
  }

  // Queue PNG encoding on the background writer thread to avoid a main-thread zlib hitch.
  if (has_pixels) {
    PendingPngJob job;
    job.path = dir_ / png_name;
    job.rgba = std::move(rgba);
    job.width = width;
    job.height = height;
    {
      std::lock_guard<std::mutex> lk(io_mu_);
      png_queue_.push_back(std::move(job));
    }
    io_cv_.notify_one();
  }

  // Record a `user_mark` row in telemetry.jsonl and push buffered lines out.
  append_line(format_telemetry_line(sample, "user_mark", 0, has_non_finite(sample)));
  enqueue_active_buf(false);

  bugs_.push_back(std::move(entry));

  char banner[96];
  std::snprintf(banner, sizeof banner, "BUG #%u SAVED -> %s", idx, has_pixels ? png_name : json_name);
  return banner;
}

void PlaytestRecorder::append_line(const std::string& line) {
  active_buf_.append(line);
  ++telemetry_lines_written_;
  if (active_buf_.size() >= kFlushBytesThreshold) {
    enqueue_active_buf(false);
  }
}

void PlaytestRecorder::enqueue_active_buf(bool allow_blocking) {
  if (active_buf_.empty()) return;
  if (allow_blocking) {
    std::lock_guard<std::mutex> lk(io_mu_);
    write_queue_.push_back(std::move(active_buf_));
    active_buf_.clear();
    active_buf_.reserve(kFlushBytesThreshold * 2);
    io_cv_.notify_one();
  } else {
    std::unique_lock<std::mutex> lk(io_mu_, std::try_to_lock);
    if (lk.owns_lock()) {
      write_queue_.push_back(std::move(active_buf_));
      active_buf_.clear();
      active_buf_.reserve(kFlushBytesThreshold * 2);
      lk.unlock();
      io_cv_.notify_one();
    }
  }
}

void PlaytestRecorder::writer_main() {
  for (;;) {
    std::string chunk;
    PendingPngJob png_job;
    bool has_chunk = false;
    bool has_png = false;
    {
      std::unique_lock<std::mutex> lk(io_mu_);
      io_cv_.wait(lk, [&]() { return stop_writer_ || !write_queue_.empty() || !png_queue_.empty(); });
      if (!write_queue_.empty()) {
        chunk = std::move(write_queue_.front());
        write_queue_.pop_front();
        has_chunk = true;
        writer_busy_ = true;
      } else if (!png_queue_.empty()) {
        png_job = std::move(png_queue_.front());
        png_queue_.pop_front();
        has_png = true;
        writer_busy_ = true;
      } else if (stop_writer_) {
        break;
      }
    }

    if (has_chunk && telemetry_file_ && !chunk.empty()) {
      std::fwrite(chunk.data(), 1, chunk.size(), telemetry_file_);
    }
    if (has_png && !png_job.rgba.empty() && png_job.width > 0 && png_job.height > 0) {
      stbi_write_png(png_job.path.string().c_str(), int(png_job.width), int(png_job.height), 4, png_job.rgba.data(),
                     int(png_job.width) * 4);
    }

    {
      std::lock_guard<std::mutex> lk(io_mu_);
      writer_busy_ = false;
      if (write_queue_.empty() && png_queue_.empty()) {
        if (telemetry_file_) std::fflush(telemetry_file_);
        flush_cv_.notify_all();
      }
    }
  }
}

void PlaytestRecorder::flush() {
  if (!active_) return;
  enqueue_active_buf(true);
  std::unique_lock<std::mutex> lk(io_mu_);
  flush_cv_.wait(lk, [&]() { return write_queue_.empty() && png_queue_.empty() && !writer_busy_; });
}

PlaytestSummary PlaytestRecorder::compute_summary() const {
  PlaytestSummary s;
  s.total_frames = total_frames_;
  if (have_sample_t_ && last_sample_t_ >= first_sample_t_) {
    s.duration_s = last_sample_t_ - first_sample_t_;
  } else {
    s.duration_s = elapsed_seconds();
  }
  if (!frame_ms_samples_.empty()) {
    std::vector<double> sorted = frame_ms_samples_;
    std::sort(sorted.begin(), sorted.end());
    s.p50_ms = compute_percentile(sorted, 0.50);
    s.p95_ms = compute_percentile(sorted, 0.95);
    s.p99_ms = compute_percentile(sorted, 0.99);
    s.max_ms = sorted.back();
  }
  s.hitch_count = hitch_count_;
  s.top_hitches = top_hitches_;
  s.ground_anomaly_count = ground_anomaly_count_;
  s.non_finite_count = non_finite_count_;
  s.audio_glitch_count = audio_glitch_count_;
  s.telemetry_lines_written = telemetry_lines_written_;
  s.bugs = bugs_;
  return s;
}

bool PlaytestRecorder::finish() {
  if (!active_) return false;
  enqueue_active_buf(true);
  {
    std::lock_guard<std::mutex> lk(io_mu_);
    stop_writer_ = true;
  }
  io_cv_.notify_all();
  if (writer_thread_.joinable()) writer_thread_.join();

  if (telemetry_file_) {
    std::fflush(telemetry_file_);
    std::fclose(telemetry_file_);
    telemetry_file_ = nullptr;
  }

  last_summary_ = compute_summary();
  active_ = false;

  const auto summary_path = dir_ / "summary.json";
  std::ofstream out(summary_path, std::ios::binary);
  if (!out) return false;
  out << format_summary_json(last_summary_, dir_, started_utc_, utc_timestamp_iso8601());
  out.flush();
  return bool(out);
}

}  // namespace carview
