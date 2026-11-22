// UDP Telemetry Out for SimHub, dashboards and motion rigs:
// - Forza Motorsport 7 "Data Out" Dash layout (311 bytes, little-endian)
// - Forza Horizon 4/5 layout (324 bytes, 232B Sled + 12B Horizon gap + Dash)
// Configurable host:port (default 127.0.0.1:5300), moving_only toggle,
// race-off in menus/paused/frozen sim, and telemetry-probe.csv logger.
// Pure C++ (no SDL dependency) so libs/drive and unit tests stay headless.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include "drive/vehicle.h"

namespace drive::telemetry {

inline constexpr size_t kFm7DashSize = 311;
inline constexpr size_t kFh4HorizonSize = 324;
inline constexpr size_t kHorizonGapOffset = 232;
inline constexpr uint8_t kDashNeutralGear = 11;

enum class Format : uint8_t {
  kFm7Dash = 0,
  kFh4Horizon = 1,
};

const char* format_name(Format f);
Format parse_format(std::string_view text);

struct Config {
  bool enabled = false;
  Format format = Format::kFm7Dash;
  std::string target = "127.0.0.1:5300";
  bool moving_only = false;
  bool probe_csv = false;
};

struct Sample {
  bool driving = false;  // IsRaceOn (0 in menus/paused or when stopped with moving_only)
  float rpm = 0.0f;
  float idle_rpm = 0.0f;
  float redline_rpm = 0.0f;

  // Car-local acceleration (m/s^2), velocity (m/s), angular velocity (rad/s), orientation (rad).
  float accel_x = 0.0f, accel_y = 0.0f, accel_z = 0.0f;
  float vel_x = 0.0f, vel_y = 0.0f, vel_z = 0.0f;
  float ang_vel_x = 0.0f, ang_vel_y = 0.0f, ang_vel_z = 0.0f;
  float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;

  // Per-wheel (FL, FR, RL, RR).
  std::array<float, 4> suspension_normalized{};  // 0 extended .. 1 compressed
  std::array<float, 4> wheel_slip_ratio{};       // 1 = peak grip
  std::array<float, 4> wheel_speed_rad_s{};
  std::array<int32_t, 4> wheel_on_rumble{};
  std::array<float, 4> wheel_puddle{};
  std::array<float, 4> surface_rumble{};
  std::array<float, 4> wheel_slip_angle{};       // 1 = peak grip
  std::array<float, 4> suspension_m{};

  // Car metadata.
  int32_t car_ordinal = 0;
  int32_t car_class = 0;
  int32_t car_pi = 0;
  int32_t drivetrain_type = 1;  // 0 FWD, 1 RWD, 2 AWD
  int32_t num_cylinders = 6;

  // World position & dash telemetry.
  float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
  float speed_mps = 0.0f;
  float power_w = 0.0f;
  float torque_nm = 0.0f;
  std::array<float, 4> tire_temp_f{180.0f, 180.0f, 180.0f, 180.0f};
  float boost_psi = 0.0f;
  float fuel = 1.0f;
  float distance_traveled = 0.0f;
  float best_lap_s = 0.0f;
  float last_lap_s = 0.0f;
  float current_lap_s = 0.0f;
  float current_race_time_s = 0.0f;
  uint16_t lap_number = 0;
  uint8_t race_position = 1;

  // Controls after assists.
  float throttle = 0.0f;   // 0..1
  float brake = 0.0f;      // 0..1
  float clutch = 0.0f;     // 0..1
  float handbrake = 0.0f;  // 0..1
  int gear = 0;            // -1 reverse, 0 neutral, 1..10
  float steer = 0.0f;      // -1 left .. +1 right
  int8_t driving_line = 0;
  int8_t ai_brake_diff = 0;
};

// Encodes FM7 Data Out Dash (311 bytes) and FH4/5 Horizon (324 bytes) packets.
std::array<uint8_t, kFm7DashSize> encode_fm7_dash(const Sample& sample, uint32_t timestamp_ms);
std::array<uint8_t, kFh4HorizonSize> encode_fh4_horizon(const Sample& sample, uint32_t timestamp_ms);

// Stateful sampler extracting a Sample from drive::Vehicle, tracking per-wheel
// suspension min/max normalization, body acceleration derivative, and 0.5 s
// stopped-simulation detection (silencing shakers automatically when paused).
class Sampler {
 public:
  void reset();
  Sample build(const Vehicle& v, bool driving, double dt_s, bool moving_only,
               int32_t car_ordinal = 0, int32_t car_class = 0, int32_t car_pi = 0,
               uint16_t lap = 0, uint8_t race_pos = 1, float cur_lap_s = 0.0f,
               float best_lap_s = 0.0f, float last_lap_s = 0.0f, float distance_m = 0.0f);

 private:
  bool started_ = false;
  Vec3 prev_vel_{};
  std::array<float, 4> susp_min_{0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 4> susp_max_{0.12f, 0.12f, 0.12f, 0.12f};
  double prev_sim_time_ = -1.0;
  double frozen_s_ = 0.0;
};

// Writes `telemetry-probe.csv` rows for diagnostics and verification.
class ProbeRecorder {
 public:
  ~ProbeRecorder();
  bool start(const std::filesystem::path& path);
  void stop();
  bool is_recording() const { return file_.is_open(); }
  void record(uint32_t timestamp_ms, const Sample& sample);
  uint64_t rows_written() const { return rows_; }

 private:
  std::ofstream file_;
  uint64_t rows_ = 0;
};

// Fire-and-forget UDP packet sender to `target` (default 127.0.0.1:5300).
class UdpSender {
 public:
  UdpSender();
  ~UdpSender();
  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  bool send(const Config& cfg, const Sample& sample, uint32_t timestamp_ms);
  void close();
  uint64_t packets_sent() const { return sent_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint64_t sent_ = 0;
};

}  // namespace drive::telemetry
