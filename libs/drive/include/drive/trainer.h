// F10 Trainer, isolated cheat save tree (`user-modded`), Festival Pacing
// economy multipliers (0.25x-3x prices, 0.5x-5x rewards), and Hidden Cars
// unlock (4 qualified player-model variants; traffic vehicles excluded).
// Pure C++ (no SDL dependency) so libs/drive and unit tests stay headless.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "drive/save.h"

namespace drive {

// ---- Hidden Cars (docs/HIDDEN_CARS.md) ----

struct HiddenCarInfo {
  int64_t id = 0;
  const char* media_name = "";
  const char* display_name = "";
  const char* make_name = "";
  int64_t fallback_cost = 0;
  int class_id = 2;
  double stock_pi = 300.0;
};

// The 4 qualified player-model hidden variants (Intro Corrado + 3 Showcase cars).
// Traffic cars (e.g. 1529 Tour Bus, 282 Beetle, 1532 Camry) are explicitly excluded.
inline constexpr std::array<HiddenCarInfo, 4> kHiddenCars = {{
    {1565, "VW_Corrado_95", "1995 Volkswagen Corrado VR6 (Intro)", "Volkswagen", 5000, 1, 286.0},
    {1566, "MIN_CooperS_09", "2009 MINI Cooper S (Showcase)", "MINI", 22000, 2, 358.0},
    {1567, "ABA_500esseesse_10", "2010 Abarth 500 esseesse (Showcase)", "Abarth", 24000, 2, 372.0},
    {1568, "PEU_205T16_84", "1984 Peugeot 205 Turbo 16 (Showcase)", "Peugeot", 130000, 3, 488.0},
}};

bool is_hidden_car_id(int64_t car_id);
const HiddenCarInfo* find_hidden_car(int64_t car_id);

// ---- Festival Pacing (site/pages/playing.md) ----

struct FestivalPacingConfig {
  bool enabled = false;
  // Car, upgrade & rim price multiplier: 0.25x .. 3.0x (default 0.5x when enabled).
  double price_multiplier = 0.5;
  // Race/event/loyalty/wristband/XP reward multiplier: 0.5x .. 5.0x (default 2.0x when enabled).
  double reward_multiplier = 2.0;

  double effective_price_multiplier() const;
  double effective_reward_multiplier() const;
  bool modifies_economy() const;

  // Scales a car/upgrade/rim price (DLC items keep their stock price).
  int64_t scale_price(int64_t base_price, bool is_dlc = false) const;
  int64_t scale_credits(int64_t base_credits) const;
  int scale_points(int base_points) const;
};

// ---- F10 Trainer ----

inline constexpr double kDayLengthGameDefault = 0.0;
inline constexpr double kDayLengthPcClock = -1.0;

struct TrainerConfig {
  bool cheats_enabled = false;
  // World controls.
  double game_speed = 1.0;                // 0.25 .. 2.0
  double time_of_day_hour = -1.0;         // <0 = running, 0..23 = held hour
  double day_length_minutes = 0.0;        // 0 = game default, 12..1440 = minutes/24h, -1 = real PC clock
  bool free_camera = false;
  bool reveal_collectibles = false;
  bool any_car_in_any_event = false;
  // Vehicle controls.
  bool fast_travel_anywhere = false;
  std::optional<std::array<double, 3>> saved_map_road;
  // Graphics & Debug toggles.
  bool debug_bloom = true;
  bool debug_motion_blur = true;
  bool debug_dof = true;
  bool debug_log_file_opens = false;
  bool debug_ffb_trace = false;
  bool debug_telemetry_probe = false;
};

// Free camera fly state controlled while trainer free_camera is active.
struct FreeCameraState {
  std::array<double, 3> pos{0.0, 5.0, 0.0};
  double yaw_rad = 0.0;
  double pitch_rad = 0.0;
  double move_speed_mps = 35.0;

  void reset_from_pose(const std::array<double, 3>& eye, const std::array<double, 3>& forward);
  void step(double dt_s, double move_fwd, double move_right, double move_up, double turn_yaw, double turn_pitch,
            bool fast);
};

// Queued explicit teleport request (expires after 10 s if map cannot accept it,
// cancelled in races or when cheats turn off; matches docs/research/TRAVEL_DESTINATIONS.md).
struct TeleportRequest {
  bool pending = false;
  std::array<double, 3> target{};
  double requested_at_s = 0.0;
  static constexpr double kExpirySeconds = 10.0;
};

// Queued GPS target request.
struct GpsRequest {
  bool pending = false;
  std::array<double, 3> target{};
};

// Runtime trainer state bridging F10 overlay actions to the simulation.
class TrainerState {
 public:
  TrainerConfig config;
  FestivalPacingConfig pacing;
  bool hidden_cars = false;
  FreeCameraState free_cam;

  std::optional<std::array<double, 3>> last_map_road;
  TeleportRequest teleport;
  GpsRequest gps;

  // Player actions (require config.cheats_enabled).
  bool apply_set_credits(int64_t& credits, int64_t amount) const;
  bool apply_add_credits(int64_t& credits, int64_t delta) const;
  // Sets wristband level 0..6 (Yellow..Gold), raising wristband_points to threshold if below.
  bool apply_set_wristband_level(int& wristband_points, int& collected_wristband, int level) const;

  // Effective simulation delta scale (1.0 when cheats off).
  double effective_game_speed() const { return config.cheats_enabled ? config.game_speed : 1.0; }
  bool effective_any_car() const { return config.cheats_enabled && config.any_car_in_any_event; }
  bool effective_fast_travel_anywhere() const { return config.cheats_enabled && config.fast_travel_anywhere; }
  bool effective_free_camera() const { return config.cheats_enabled && config.free_camera; }
  bool effective_reveal_collectibles() const { return config.cheats_enabled && config.reveal_collectibles; }

  // Computes effective time-of-day hour & day-length scale (1.0 = 30.5 min FH1 default).
  // Returns fixed_hour >= 0 when held or synced to local PC clock; <0 when advancing normally.
  double effective_fixed_hour() const;
  double effective_day_length_scale() const;

  // Vehicle travel / GPS actions (finite check, rejected during races or with cheats off).
  void note_map_road(const std::array<double, 3>& world_pos);
  bool save_last_map_road();
  bool request_gps_to_last_road(bool in_race);
  bool request_gps_to_saved_road(bool in_race);
  bool request_teleport_to_last_road(double now_s, bool in_race);
  bool request_teleport_to_saved_road(double now_s, bool in_race);
  bool request_gps(const std::array<double, 3>& pt, bool in_race);
  bool request_teleport(const std::array<double, 3>& pt, double now_s, bool in_race);

  // Polls queued teleport; expires after 10 s or cancels in races / cheats off.
  // Returns the destination point when ready to execute on an open map (or direct handoff).
  std::optional<std::array<double, 3>> consume_teleport(double now_s, bool map_open, bool in_race);
  std::optional<std::array<double, 3>> consume_gps(bool in_race);
};

// ---- Save-tree isolation (`user` vs `user-modded`) ----

// True whenever Trainer cheats, Festival Pacing (non-1x), or Hidden Cars are enabled.
bool requires_modded_save(const TrainerConfig& trainer, const FestivalPacingConfig& pacing, bool hidden_cars);

// Resolves the save file path: returns `main_save_path` unchanged when `!use_modded`.
// When `use_modded` is true, maps `<root>/user/<name>` (or `<dir>/<name>`) to
// `<root>/user-modded/<name>` and seeds it once from `main_save_path` if missing,
// guaranteeing the player's main save is never written by modded/cheat sessions.
std::filesystem::path resolve_isolated_save_path(const std::filesystem::path& main_save_path, bool use_modded,
                                                 bool* seeded_copy = nullptr);

// Parses/formats "x,y,z" saved road coordinates (finite bounds check).
std::optional<std::array<double, 3>> parse_road_point(std::string_view text);
std::string format_road_point(const std::array<double, 3>& pt);
// Current local PC wall-clock time as fractional hours [0, 24).
double local_pc_clock_hour();

// Wristband tier names & thresholds (Yellow=0 .. Gold=6).
const char* wristband_tier_name(int level);
int wristband_tier_threshold(int level);

}  // namespace drive
