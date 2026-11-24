#include "drive/trainer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <system_error>

namespace drive {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool is_finite_point(const std::array<double, 3>& pt) {
  for (double v : pt) {
    if (!std::isfinite(v) || std::fabs(v) > 50000.0) return false;
  }
  return true;
}

}  // namespace

// ---- Hidden Cars ----

bool is_hidden_car_id(int64_t car_id) { return find_hidden_car(car_id) != nullptr; }

const HiddenCarInfo* find_hidden_car(int64_t car_id) {
  for (const auto& c : kHiddenCars) {
    if (c.id == car_id) return &c;
  }
  return nullptr;
}

// ---- Festival Pacing ----

double FestivalPacingConfig::effective_price_multiplier() const {
  return enabled ? std::clamp(price_multiplier, 0.25, 3.0) : 1.0;
}

double FestivalPacingConfig::effective_reward_multiplier() const {
  return enabled ? std::clamp(reward_multiplier, 0.5, 5.0) : 1.0;
}

bool FestivalPacingConfig::modifies_economy() const {
  if (!enabled) return false;
  return std::fabs(effective_price_multiplier() - 1.0) > 1e-4 ||
         std::fabs(effective_reward_multiplier() - 1.0) > 1e-4;
}

int64_t FestivalPacingConfig::scale_price(int64_t base_price, bool is_dlc) const {
  if (base_price <= 0 || is_dlc || !enabled) return base_price;
  const double m = effective_price_multiplier();
  if (std::fabs(m - 1.0) < 1e-4) return base_price;
  const int64_t scaled = static_cast<int64_t>(std::llround(double(base_price) * m));
  // Round to nearest 10 CR for clean Forza pricing, min 10 CR.
  const int64_t rounded = ((scaled + 5) / 10) * 10;
  return std::max<int64_t>(10, rounded);
}

int64_t FestivalPacingConfig::scale_credits(int64_t base_credits) const {
  if (base_credits <= 0 || !enabled) return base_credits;
  const double m = effective_reward_multiplier();
  if (std::fabs(m - 1.0) < 1e-4) return base_credits;
  return std::max<int64_t>(1, static_cast<int64_t>(std::llround(double(base_credits) * m)));
}

int FestivalPacingConfig::scale_points(int base_points) const {
  if (base_points <= 0 || !enabled) return base_points;
  const double m = effective_reward_multiplier();
  if (std::fabs(m - 1.0) < 1e-4) return base_points;
  return std::max(1, static_cast<int>(std::lround(double(base_points) * m)));
}

// ---- Free Camera ----

void FreeCameraState::reset_from_pose(const std::array<double, 3>& eye, const std::array<double, 3>& forward) {
  pos = eye;
  yaw_rad = std::atan2(forward[0], forward[2]);
  const double horiz = std::hypot(forward[0], forward[2]);
  pitch_rad = std::atan2(forward[1], std::max(horiz, 1e-6));
}

void FreeCameraState::step(double dt_s, double move_fwd, double move_right, double move_up, double turn_yaw,
                           double turn_pitch, bool fast) {
  const double dt = std::clamp(dt_s, 0.0, 0.1);
  yaw_rad = std::remainder(yaw_rad + turn_yaw * dt * 2.2, 2.0 * kPi);
  pitch_rad = std::clamp(pitch_rad + turn_pitch * dt * 1.8, -1.45, 1.45);
  const double cp = std::cos(pitch_rad), sp = std::sin(pitch_rad);
  const double sy = std::sin(yaw_rad), cy = std::cos(yaw_rad);
  const std::array<double, 3> fwd{sy * cp, sp, cy * cp};
  const std::array<double, 3> right{cy, 0.0, -sy};
  const double speed = move_speed_mps * (fast ? 3.5 : 1.0) * dt;
  for (size_t i = 0; i < 3; ++i) pos[i] += (fwd[i] * move_fwd + right[i] * move_right) * speed;
  pos[1] += move_up * speed;
}

// ---- Wristband tiers ----

const char* wristband_tier_name(int level) {
  static constexpr const char* kNames[] = {"YELLOW", "GREEN", "BLUE", "PINK", "ORANGE", "PURPLE", "GOLD"};
  return kNames[std::clamp(level, 0, 6)];
}

int wristband_tier_threshold(int level) {
  static constexpr int kThresholds[] = {0, 100, 250, 500, 900, 1400, 2000};
  return kThresholds[std::clamp(level, 0, 6)];
}

// ---- TrainerState ----

bool TrainerState::apply_set_credits(int64_t& credits, int64_t amount) const {
  if (!config.cheats_enabled) return false;
  credits = std::clamp<int64_t>(amount, 0, 999999999LL);
  return true;
}

bool TrainerState::apply_add_credits(int64_t& credits, int64_t delta) const {
  if (!config.cheats_enabled) return false;
  credits = std::clamp<int64_t>(credits + delta, 0, 999999999LL);
  return true;
}

bool TrainerState::apply_set_wristband_level(int& wristband_points, int& collected_wristband, int level) const {
  if (!config.cheats_enabled) return false;
  const int clamped = std::clamp(level, 0, 6);
  collected_wristband = clamped;
  const int req = wristband_tier_threshold(clamped);
  if (wristband_points < req) wristband_points = req;
  return true;
}

double local_pc_clock_hour() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  return double(tm_buf.tm_hour) + double(tm_buf.tm_min) / 60.0 + double(tm_buf.tm_sec) / 3600.0;
}

double TrainerState::effective_fixed_hour() const {
  if (!config.cheats_enabled) return -1.0;
  // Held time of day takes precedence over PC clock.
  if (config.time_of_day_hour >= 0.0) return std::fmod(config.time_of_day_hour, 24.0);
  if (config.day_length_minutes < 0.0) return local_pc_clock_hour();
  return -1.0;
}

double TrainerState::effective_day_length_scale() const {
  if (!config.cheats_enabled) return 1.0;
  if (config.time_of_day_hour >= 0.0) return 0.0;
  if (config.day_length_minutes <= 0.0) return 1.0;
  // FH1 default full day is ~30.5 real minutes.
  const double mins = std::clamp(config.day_length_minutes, 12.0, 1440.0);
  return 30.5 / mins;
}

void TrainerState::note_map_road(const std::array<double, 3>& world_pos) {
  if (is_finite_point(world_pos)) last_map_road = world_pos;
}

bool TrainerState::save_last_map_road() {
  if (!last_map_road) return false;
  config.saved_map_road = *last_map_road;
  return true;
}

bool TrainerState::request_gps(const std::array<double, 3>& pt, bool in_race) {
  if (!config.cheats_enabled || in_race || !is_finite_point(pt)) return false;
  gps.pending = true;
  gps.target = pt;
  return true;
}

bool TrainerState::request_teleport(const std::array<double, 3>& pt, double now_s, bool in_race) {
  if (!config.cheats_enabled || in_race || !is_finite_point(pt)) return false;
  teleport.pending = true;
  teleport.target = pt;
  teleport.requested_at_s = now_s;
  return true;
}

bool TrainerState::request_gps_to_last_road(bool in_race) {
  return last_map_road && request_gps(*last_map_road, in_race);
}

bool TrainerState::request_gps_to_saved_road(bool in_race) {
  return config.saved_map_road && request_gps(*config.saved_map_road, in_race);
}

bool TrainerState::request_teleport_to_last_road(double now_s, bool in_race) {
  return last_map_road && request_teleport(*last_map_road, now_s, in_race);
}

bool TrainerState::request_teleport_to_saved_road(double now_s, bool in_race) {
  return config.saved_map_road && request_teleport(*config.saved_map_road, now_s, in_race);
}

std::optional<std::array<double, 3>> TrainerState::consume_teleport(double now_s, bool map_open, bool in_race) {
  if (!teleport.pending) return std::nullopt;
  if (!config.cheats_enabled || in_race) {
    teleport.pending = false;
    return std::nullopt;
  }
  if ((now_s - teleport.requested_at_s) > TeleportRequest::kExpirySeconds) {
    teleport.pending = false;
    return std::nullopt;
  }
  if (!map_open) return std::nullopt;
  const auto pt = teleport.target;
  teleport.pending = false;
  return pt;
}

std::optional<std::array<double, 3>> TrainerState::consume_gps(bool in_race) {
  if (!gps.pending) return std::nullopt;
  if (!config.cheats_enabled || in_race) {
    gps.pending = false;
    return std::nullopt;
  }
  const auto pt = gps.target;
  gps.pending = false;
  return pt;
}

void TrainerState::configure_save_root(const std::filesystem::path& clean_user_dir, bool already_isolated) {
  clean_user_dir_ = clean_user_dir;
  active_user_dir_ = clean_user_dir;
  modded_isolation_active_ = already_isolated;
  modded_isolation_triggered_ = false;
  if (!modded_isolation_active_ && !clean_user_dir_.empty() &&
      requires_modded_save(config, pacing, hidden_cars)) {
    active_user_dir_ = resolve_isolated_user_dir(clean_user_dir_, true);
    modded_isolation_active_ = true;
    modded_isolation_triggered_ = true;
  }
}

bool TrainerState::sync_modded_isolation() {
  if (modded_isolation_active_) return false;
  if (!requires_modded_save(config, pacing, hidden_cars)) return false;
  if (!clean_user_dir_.empty()) {
    active_user_dir_ = resolve_isolated_user_dir(clean_user_dir_, true);
  }
  modded_isolation_active_ = true;
  modded_isolation_triggered_ = true;
  return true;
}

bool TrainerState::consume_modded_isolation_triggered() {
  if (!modded_isolation_triggered_) return false;
  modded_isolation_triggered_ = false;
  return true;
}

// ---- Save-tree isolation ----

bool requires_modded_save(const TrainerConfig& trainer, const FestivalPacingConfig& pacing, bool hidden_cars) {
  return trainer.cheats_enabled || pacing.modifies_economy() || hidden_cars;
}

namespace {

std::filesystem::path compute_modded_dir(const std::filesystem::path& parent) {
  if (parent.filename() == "user") {
    return parent.parent_path() / "user-modded";
  }
  if (parent.filename() == "user-modded") {
    return parent;
  }
  return parent / "user-modded";
}

bool seed_modded_save_tree(const std::filesystem::path& src_dir, const std::filesystem::path& dst_dir,
                           const std::filesystem::path& save_filename) {
  if (src_dir.empty() || dst_dir.empty() || src_dir == dst_dir) return false;
  bool any_seeded = false;

  auto copy_file_if_missing = [&](const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    if (!std::filesystem::exists(dst, ec) && std::filesystem::is_regular_file(src, ec)) {
      std::filesystem::create_directories(dst.parent_path(), ec);
      if (std::filesystem::copy_file(src, dst, std::filesystem::copy_options::skip_existing, ec)) {
        any_seeded = true;
      }
    }
  };

  if (!save_filename.empty()) {
    copy_file_if_missing(src_dir / save_filename, dst_dir / save_filename);
  }
  if (save_filename != "save.txt") {
    copy_file_if_missing(src_dir / "save.txt", dst_dir / "save.txt");
  }
  copy_file_if_missing(src_dir / "garage.txt", dst_dir / "garage.txt");

  std::error_code ec;
  const std::filesystem::path src_liveries = src_dir / "liveries";
  const std::filesystem::path dst_liveries = dst_dir / "liveries";
  if (!std::filesystem::exists(dst_liveries, ec) && std::filesystem::is_directory(src_liveries, ec)) {
    std::filesystem::create_directories(dst_liveries, ec);
    std::filesystem::copy(src_liveries, dst_liveries,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing, ec);
    if (!ec) any_seeded = true;
  }

  return any_seeded;
}

}  // namespace

std::filesystem::path resolve_isolated_user_dir(const std::filesystem::path& main_user_dir, bool use_modded,
                                                bool* seeded_copy) {
  if (seeded_copy) *seeded_copy = false;
  if (!use_modded || main_user_dir.empty()) return main_user_dir;

  const std::filesystem::path modded_dir = compute_modded_dir(main_user_dir);
  std::error_code ec;
  std::filesystem::create_directories(modded_dir, ec);
  const bool seeded = seed_modded_save_tree(main_user_dir, modded_dir, "save.txt");
  if (seeded_copy) *seeded_copy = seeded;
  return modded_dir;
}

std::filesystem::path resolve_isolated_save_path(const std::filesystem::path& main_save_path, bool use_modded,
                                                 bool* seeded_copy) {
  if (seeded_copy) *seeded_copy = false;
  if (!use_modded || main_save_path.empty()) return main_save_path;

  const std::filesystem::path filename = main_save_path.filename();
  const std::filesystem::path parent = main_save_path.parent_path();
  const std::filesystem::path modded_dir = compute_modded_dir(parent);
  const std::filesystem::path modded_path = modded_dir / filename;

  const bool seeded = seed_modded_save_tree(parent, modded_dir, filename);
  if (seeded_copy) *seeded_copy = seeded;
  return modded_path;
}

std::optional<std::array<double, 3>> parse_road_point(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::array<double, 3> pt{};
  size_t start = 0;
  for (size_t i = 0; i < 3; ++i) {
    const size_t comma = text.find(',', start);
    if (i < 2 && comma == std::string_view::npos) return std::nullopt;
    const size_t end = i < 2 ? comma : text.size();
    std::string part(text.substr(start, end - start));
    try {
      pt[i] = std::stod(part);
    } catch (...) {
      return std::nullopt;
    }
    start = end + 1;
  }
  if (!is_finite_point(pt)) return std::nullopt;
  return pt;
}

std::string format_road_point(const std::array<double, 3>& pt) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%.2f,%.2f,%.2f", pt[0], pt[1], pt[2]);
  return buf;
}

}  // namespace drive
