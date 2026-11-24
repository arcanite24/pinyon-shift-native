#include "race/rivals.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace race {
namespace {

std::string sanitize_line(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\r' || c == '\n') out.push_back(' ');
    else out.push_back(c);
  }
  return out;
}

void slerp_quat(double ax, double ay, double az, double aw, double bx, double by, double bz, double bw, double u,
                double& ox, double& oy, double& oz, double& ow) {
  double dot = ax * bx + ay * by + az * bz + aw * bw;
  if (dot < 0.0) {
    bx = -bx, by = -by, bz = -bz, bw = -bw;
    dot = -dot;
  }
  if (dot > 0.9995) {
    ox = ax + u * (bx - ax);
    oy = ay + u * (by - ay);
    oz = az + u * (bz - az);
    ow = aw + u * (bw - aw);
  } else {
    dot = std::clamp(dot, -1.0, 1.0);
    const double theta0 = std::acos(dot);
    const double sin0 = std::sin(theta0);
    const double theta = theta0 * u;
    const double s0 = std::sin(theta0 - theta) / sin0;
    const double s1 = std::sin(theta) / sin0;
    ox = s0 * ax + s1 * bx;
    oy = s0 * ay + s1 * by;
    oz = s0 * az + s1 * bz;
    ow = s0 * aw + s1 * bw;
  }
  const double len = std::sqrt(ox * ox + oy * oy + oz * oz + ow * ow);
  if (len > 1e-12) ox /= len, oy /= len, oz /= len, ow /= len;
  else ox = 0.0, oy = 0.0, oz = 0.0, ow = 1.0;
}

}  // namespace

void GhostRun::clear() {
  samples.clear();
  total_time = 0.0;
  clean = true;
}

double GhostRun::duration() const {
  if (total_time > 0.0) return total_time;
  return samples.empty() ? 0.0 : samples.back().time;
}

void GhostRun::record(const GhostSample& s, bool force) {
  if (!samples.empty() && !force) {
    if (s.time < samples.back().time) return;
    if (sample_interval > 0.0 && (s.time - samples.back().time) + 1e-9 < sample_interval) return;
  }
  if (!samples.empty() && s.time <= samples.back().time) {
    samples.back() = s;
    return;
  }
  samples.push_back(s);
}

bool GhostRun::interpolate(double t, GhostSample& out) const {
  if (samples.empty()) return false;
  if (samples.size() == 1 || t <= samples.front().time) {
    out = samples.front();
    return true;
  }
  if (t >= samples.back().time) {
    out = samples.back();
    return true;
  }
  const auto it = std::lower_bound(samples.begin(), samples.end(), t,
                                   [](const GhostSample& s, double v) { return s.time < v; });
  if (it == samples.begin()) {
    out = *it;
    return true;
  }
  const GhostSample& a = *(it - 1);
  const GhostSample& b = *it;
  const double dt = b.time - a.time;
  const double u = dt > 1e-12 ? std::clamp((t - a.time) / dt, 0.0, 1.0) : 0.0;
  out.time = t;
  out.x = a.x + u * (b.x - a.x);
  out.y = a.y + u * (b.y - a.y);
  out.z = a.z + u * (b.z - a.z);
  slerp_quat(a.qx, a.qy, a.qz, a.qw, b.qx, b.qy, b.qz, b.qw, u, out.qx, out.qy, out.qz, out.qw);
  out.speed = a.speed + u * (b.speed - a.speed);
  out.steer = float(a.steer + u * (b.steer - a.steer));
  out.brake = float(a.brake + u * (b.brake - a.brake));
  out.gear = u < 0.5 ? a.gear : b.gear;
  return true;
}

bool GhostRun::is_better(const GhostRun& candidate, const GhostRun& current_best) {
  if (candidate.total_time <= 0.0 || candidate.samples.empty()) return false;
  if (current_best.total_time <= 0.0 || current_best.samples.empty()) return true;
  if (candidate.clean != current_best.clean) return candidate.clean;
  return candidate.total_time < current_best.total_time;
}

std::string GhostRun::to_text() const {
  std::ostringstream os;
  os << "shift-ghost 1\n";
  os << "event " << sanitize_line(event_id) << "\n";
  os << "driver " << sanitize_line(driver_name) << "\n";
  os << "car " << sanitize_line(car_media) << "\n";
  os << "car_id " << car_id << "\n";
  os << "pi " << shown_pi << "\n";
  char buf[256];
  std::snprintf(buf, sizeof buf, "time %.6f\nclean %d\ninterval %.6f\nsamples %zu\n", total_time, clean ? 1 : 0,
                sample_interval, samples.size());
  os << buf;
  for (const auto& s : samples) {
    std::snprintf(buf, sizeof buf, "s %.6f %.4f %.4f %.4f %.6f %.6f %.6f %.6f %.4f %.4f %.3f %d\n", s.time, s.x, s.y,
                  s.z, s.qx, s.qy, s.qz, s.qw, s.speed, double(s.steer), double(s.brake), s.gear);
    os << buf;
  }
  return os.str();
}

bool GhostRun::from_text(std::string_view text, GhostRun& out, std::string* error) {
  out = GhostRun{};
  std::istringstream in{std::string(text)};
  std::string line;
  if (!std::getline(in, line)) {
    if (error) *error = "ghost: empty file";
    return false;
  }
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  int ver = 0;
  if (std::sscanf(line.c_str(), "shift-ghost %d", &ver) != 1 || ver != 1) {
    if (error) *error = "ghost: unsupported header: " + line;
    return false;
  }
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) continue;
    if (line.rfind("event ", 0) == 0) {
      out.event_id = line.substr(6);
    } else if (line.rfind("driver ", 0) == 0) {
      out.driver_name = line.substr(7);
    } else if (line.rfind("car ", 0) == 0) {
      out.car_media = line.substr(4);
    } else if (line.rfind("car_id ", 0) == 0) {
      long long id = 0;
      if (std::sscanf(line.c_str() + 7, "%lld", &id) == 1) out.car_id = id;
    } else if (line.rfind("pi ", 0) == 0) {
      int pi = 0;
      if (std::sscanf(line.c_str() + 3, "%d", &pi) == 1) out.shown_pi = pi;
    } else if (line.rfind("time ", 0) == 0) {
      double t = 0.0;
      if (std::sscanf(line.c_str() + 5, "%lf", &t) == 1) out.total_time = t;
    } else if (line.rfind("clean ", 0) == 0) {
      int c = 1;
      if (std::sscanf(line.c_str() + 6, "%d", &c) == 1) out.clean = (c != 0);
    } else if (line.rfind("interval ", 0) == 0) {
      double iv = 0.0;
      if (std::sscanf(line.c_str() + 9, "%lf", &iv) == 1 && iv > 0.0) out.sample_interval = iv;
    } else if (line.rfind("samples ", 0) == 0) {
      unsigned long long cnt = 0;
      if (std::sscanf(line.c_str() + 8, "%llu", &cnt) == 1 && cnt < 2000000ull) out.samples.reserve(size_t(cnt));
    } else if (line.rfind("s ", 0) == 0) {
      GhostSample s;
      double steer = 0.0, brake = 0.0;
      if (std::sscanf(line.c_str() + 2, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %d", &s.time, &s.x, &s.y, &s.z,
                      &s.qx, &s.qy, &s.qz, &s.qw, &s.speed, &steer, &brake, &s.gear) == 12) {
        s.steer = float(steer);
        s.brake = float(brake);
        out.samples.push_back(s);
      }
    }
  }
  return true;
}

bool GhostRun::save(const std::filesystem::path& path, std::string* error) const {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "ghost: cannot write " + path.string();
    return false;
  }
  const std::string data = to_text();
  out.write(data.data(), std::streamsize(data.size()));
  if (!out) {
    if (error) *error = "ghost: write failed for " + path.string();
    return false;
  }
  return true;
}

bool GhostRun::load(const std::filesystem::path& path, GhostRun& out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "ghost: cannot open " + path.string();
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return from_text(ss.str(), out, error);
}

std::filesystem::path ghost_file_path(const std::filesystem::path& ghost_dir, std::string_view event_id) {
  std::string safe;
  safe.reserve(event_id.size());
  for (char c : event_id) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '_' || c == '-') safe.push_back(c);
    else safe.push_back('_');
  }
  if (safe.empty()) safe = "run";
  return ghost_dir / (safe + ".ghost");
}

bool update_personal_best_ghost(const std::filesystem::path& ghost_dir, const GhostRun& candidate,
                                bool* saved_new_best, std::string* error) {
  if (saved_new_best) *saved_new_best = false;
  if (candidate.total_time <= 0.0 || candidate.samples.empty()) return true;
  const auto path = ghost_file_path(ghost_dir, candidate.event_id);
  GhostRun existing;
  std::string load_err;
  const bool have_existing = std::filesystem::exists(path) && GhostRun::load(path, existing, &load_err);
  if (have_existing && !GhostRun::is_better(candidate, existing)) return true;
  if (!candidate.save(path, error)) return false;
  if (saved_new_best) *saved_new_best = true;
  return true;
}

}  // namespace race
