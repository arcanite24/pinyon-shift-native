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
  return ghost_dir / (safe + ".shgh");
}

bool load_personal_best_ghost(const std::filesystem::path& ghost_dir, std::string_view event_id, GhostRun& out,
                              std::string* error) {
  const auto shgh_path = ghost_file_path(ghost_dir, event_id);
  std::error_code ec;
  if (std::filesystem::exists(shgh_path, ec)) {
    return GhostRun::load(shgh_path, out, error);
  }
  auto legacy_path = shgh_path;
  legacy_path.replace_extension(".ghost");
  if (std::filesystem::exists(legacy_path, ec)) {
    return GhostRun::load(legacy_path, out, error);
  }
  if (error) *error = "ghost: not found";
  return false;
}

bool update_personal_best_ghost(const std::filesystem::path& ghost_dir, const GhostRun& candidate,
                                bool* saved_new_best, std::string* error) {
  if (saved_new_best) *saved_new_best = false;
  if (candidate.total_time <= 0.0 || candidate.samples.empty()) return true;
  const auto path = ghost_file_path(ghost_dir, candidate.event_id);
  GhostRun existing;
  std::string load_err;
  const bool have_existing = load_personal_best_ghost(ghost_dir, candidate.event_id, existing, &load_err);
  if (have_existing && !GhostRun::is_better(candidate, existing)) return true;
  if (!candidate.save(path, error)) return false;
  if (saved_new_best) *saved_new_best = true;
  return true;
}

std::array<RivalTarget, 3> generate_rivals_targets(std::string_view event_id, double base_time_s,
                                                   int64_t base_prize_cr, bool is_rally_stage) {
  // Deterministic hash from event_id so rival pairings stay consistent across sessions.
  uint32_t h = 2166136261u;
  for (unsigned char c : event_id) {
    h ^= c;
    h *= 16777619u;
  }
  static constexpr const char* kRoadBronze[] = {"DAK STEWART", "HOLLY CRUZ", "SCOTT TYLER", "ALI HOWARD"};
  static constexpr const char* kRoadSilver[] = {"RAMONA CRAVACHE", "DUKE MAGUIRE", "ZAKI MALIK", "MARKO BARAN"};
  static constexpr const char* kRoadGold[] = {"DARIUS FLYNT", "HALEY HARPER", "ALICE HART", "DARIUS FLYNT"};
  static constexpr const char* kRallyBronze[] = {"COLIN MCRAE CLUB", "BOULDER DIRT CLUB", "RED ROCK CLUB", "PINYON CLUB"};
  static constexpr const char* kRallySilver[] = {"CLEAR SPRINGS PRO", "MONTANO STAGE ACE", "BEAUMONT RALLY PRO", "COPPER RIDGE ACE"};
  static constexpr const char* kRallyGold[] = {"HORIZON RALLY CHAMP", "COLORADO DIRT KING", "STAGE RECORD HOLDER", "GROUP B LEGEND"};

  const double safe_base = std::max(15.0, base_time_s > 1.0 ? base_time_s : (is_rally_stage ? 95.0 : 120.0));
  const int64_t safe_prize = std::max<int64_t>(10000, base_prize_cr);
  const size_t pick = size_t(h % 4u);

  std::array<RivalTarget, 3> out{};
  out[0].tier_index = 1;
  out[0].tier_label = "BRONZE";
  out[0].rival_name = is_rally_stage ? kRallyBronze[pick] : kRoadBronze[pick];
  out[0].target_time_s = safe_base * 1.12;
  out[0].bounty_cr = std::max<int64_t>(2500, (safe_prize * 25) / 100);

  out[1].tier_index = 2;
  out[1].tier_label = "SILVER";
  out[1].rival_name = is_rally_stage ? kRallySilver[pick] : kRoadSilver[pick];
  out[1].target_time_s = safe_base * 1.04;
  out[1].bounty_cr = std::max<int64_t>(5000, (safe_prize * 50) / 100);

  out[2].tier_index = 3;
  out[2].tier_label = "GOLD";
  out[2].rival_name = is_rally_stage ? kRallyGold[pick] : kRoadGold[pick];
  out[2].target_time_s = safe_base * 0.96;
  out[2].bounty_cr = std::max<int64_t>(10000, safe_prize);
  return out;
}

GhostRun synthesize_target_ghost(const std::vector<GhostWaypoint>& waypoints, double target_time_s,
                                 std::string_view event_id, std::string_view rival_name,
                                 std::string_view car_media, int64_t car_id, int shown_pi, double sample_interval) {
  GhostRun out;
  out.event_id = std::string(event_id);
  out.driver_name = std::string(rival_name);
  out.car_media = std::string(car_media);
  out.car_id = car_id;
  out.shown_pi = shown_pi;
  out.total_time = std::max(1.0, target_time_s);
  out.clean = true;
  out.sample_interval = std::clamp(sample_interval, 1.0 / 60.0, 0.5);
  if (waypoints.size() < 2) return out;

  const double s0 = waypoints.front().progress;
  const double s1 = waypoints.back().progress;
  const double total_len = std::max(1.0, s1 - s0);
  const double avg_speed = total_len / out.total_time;
  const int steps = std::max(2, int(std::ceil(out.total_time / out.sample_interval)));
  out.samples.reserve(size_t(steps) + 1);

  auto eval_at_progress = [&](double s_want, double& x, double& y, double& z, double& dx, double& dy, double& dz) {
    auto it = std::lower_bound(waypoints.begin(), waypoints.end(), s_want,
                               [](const GhostWaypoint& w, double v) { return w.progress < v; });
    size_t i1 = size_t(it - waypoints.begin());
    if (i1 == 0) i1 = 1;
    if (i1 >= waypoints.size()) i1 = waypoints.size() - 1;
    const auto& a = waypoints[i1 - 1];
    const auto& b = waypoints[i1];
    const double seg = b.progress - a.progress;
    const double u = seg > 1e-6 ? std::clamp((s_want - a.progress) / seg, 0.0, 1.0) : 0.0;
    x = a.x + u * (b.x - a.x);
    y = a.y + u * (b.y - a.y);
    z = a.z + u * (b.z - a.z);
    dx = b.x - a.x;
    dy = b.y - a.y;
    dz = b.z - a.z;
  };

  for (int i = 0; i <= steps; ++i) {
    const double frac = double(i) / double(steps);
    const double t = frac * out.total_time;
    // Slight smooth standing-launch ramp over the first 2 seconds, then steady target pace.
    const double ramp_f = std::min(1.0, t / 2.0);
    const double eff_frac = std::clamp(frac, 0.0, 1.0);
    const double s_cur = s0 + eff_frac * total_len;
    double x = 0, y = 0, z = 0, dx = 0, dy = 0, dz = 1;
    eval_at_progress(s_cur, x, y, z, dx, dy, dz);
    const double horiz = std::hypot(dx, dz);
    const double heading = horiz > 1e-6 ? std::atan2(dx, dz) : 0.0;
    const double pitch = horiz > 1e-6 ? std::atan2(dy, horiz) : 0.0;
    const double cy = std::cos(heading * 0.5), sy = std::sin(heading * 0.5);
    const double cp = std::cos(-pitch * 0.5), sp = std::sin(-pitch * 0.5);
    GhostSample gs{};
    gs.time = t;
    gs.x = x;
    gs.y = y;
    gs.z = z;
    gs.qx = cy * sp;
    gs.qy = sy * cp;
    gs.qz = -sy * sp;
    gs.qw = cy * cp;
    gs.speed = avg_speed * (0.25 + 0.75 * ramp_f);
    gs.gear = std::clamp(1 + int(gs.speed / 14.0), 1, 6);
    out.record(gs, i == 0 || i == steps);
  }
  return out;
}

int64_t evaluate_rivals_bounty(double player_time_s, bool clean, double target_time_s, int64_t base_bounty_cr,
                               bool& beat_rival) {
  beat_rival = (player_time_s > 0.0 && target_time_s > 0.0 && player_time_s < target_time_s);
  if (!beat_rival || base_bounty_cr <= 0) return 0;
  return clean ? base_bounty_cr : std::max<int64_t>(500, (base_bounty_cr * 75) / 100);
}

std::string format_rivals_time(double seconds) {
  if (!(seconds > 0.0) || !std::isfinite(seconds)) return "--:--.---";
  const int total_ms = int(std::llround(seconds * 1000.0));
  const int mins = total_ms / 60000;
  const int secs = (total_ms / 1000) % 60;
  const int ms = total_ms % 1000;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d.%03d", mins, secs, ms);
  return buf;
}

}  // namespace race
