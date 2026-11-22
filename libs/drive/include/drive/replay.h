// Gameplay replay recorder, ring buffer, interpolation, and persistent binary
// file save/load (Slice 10). Captures compact 60 Hz car poses, wheel spin/steer,
// lamp states, audio/RPM, and event metadata across player, race opponents, and
// traffic cars.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "drive/lamps.h"
#include "drive/vehicle.h"

namespace drive {

enum ReplayLampBit : uint16_t {
  kReplayLampHeadlights = 1u << 0,
  kReplayLampHighBeam = 1u << 1,
  kReplayLampBrake = 1u << 2,
  kReplayLampReverse = 1u << 3,
  kReplayLampLeftTurn = 1u << 4,
  kReplayLampRightTurn = 1u << 5,
  kReplayLampHazards = 1u << 6,
  kReplayLampFog = 1u << 7,
};

uint16_t pack_replay_lamps(const CarLampState& lamps);
CarLampState unpack_replay_lamps(uint16_t bits);

struct ReplayCarInfo {
  uint32_t slot = 0;       // 0 = player, 1..N = opponents / traffic
  int32_t car_id = 0;
  std::string car_media;
  std::string driver_name;
  bool is_player = false;
  bool is_traffic = false;
};

struct ReplayCarSample {
  uint32_t slot = 0;
  float pos[3] = {0, 0, 0};
  float quat[4] = {1, 0, 0, 0};  // w, x, y, z
  float vel[3] = {0, 0, 0};
  float speed_mps = 0.0f;
  float rpm = 900.0f;
  float redline_rpm = 7000.0f;
  int8_t gear = 1;
  uint8_t throttle_u8 = 0;   // 0..255
  uint8_t brake_u8 = 0;      // 0..255
  uint8_t handbrake_u8 = 0;  // 0..255
  int8_t steer_i8 = 0;       // -127..+127
  uint8_t race_place = 0;
  uint16_t lamp_bits = 0;
  float wheel_steer[4] = {0, 0, 0, 0};
  float wheel_spin[4] = {0, 0, 0, 0};
  float wheel_omega[4] = {0, 0, 0, 0};
  float wheel_y[4] = {0, 0, 0, 0};

  static ReplayCarSample from_vehicle(uint32_t slot, const Vehicle& v, const CarLampState& lamps,
                                      double throttle = 0.0, double brake = 0.0, double handbrake = 0.0,
                                      double steer = 0.0, uint8_t place = 0);
  void apply_to_pose(Vehicle::Pose& out_pose) const;
};

struct ReplayFrame {
  double time_s = 0.0;
  std::vector<ReplayCarSample> cars;
};

struct ReplayEventMarker {
  double time_s = 0.0;
  std::string label;
};

struct ReplayMetadata {
  std::string event_name = "FREE ROAM";
  std::string world_name = "Colorado";
  double tod_sec = 12.0 * 3600.0;
  int64_t recorded_utc = 0;
  double sample_hz = 60.0;
  bool is_race = false;
  int player_place = 0;
  int field_size = 1;
  double best_lap_s = -1.0;
  std::vector<ReplayCarInfo> roster;
  std::vector<ReplayEventMarker> markers;
};

// Self-contained playback recording with normalized time [0, duration].
class ReplayRecording {
 public:
  ReplayMetadata meta;
  std::vector<ReplayFrame> frames;

  bool empty() const { return frames.empty(); }
  size_t frame_count() const { return frames.size(); }
  double duration() const { return frames.size() >= 2 ? frames.back().time_s - frames.front().time_s : 0.0; }
  double start_time() const { return frames.empty() ? 0.0 : frames.front().time_s; }
  double end_time() const { return frames.empty() ? 0.0 : frames.back().time_s; }

  // Interpolates all cars present at normalized playback time `t_rel` in [0, duration()].
  bool sample(double t_rel, std::vector<ReplayCarSample>& out_cars) const;
  bool sample_car(double t_rel, uint32_t slot, ReplayCarSample& out_car) const;

  const ReplayCarInfo* find_car_info(uint32_t slot) const;
};

// Ring-buffered 60 Hz gameplay replay recorder.
class ReplayRecorder {
 public:
  static constexpr size_t kDefaultMaxFrames = 18000;  // 5 minutes at 60 Hz

  void begin(const ReplayMetadata& meta, size_t max_frames = kDefaultMaxFrames, bool ring_wrap = true);
  void clear();
  bool active() const { return active_; }
  void set_roster(std::vector<ReplayCarInfo> roster) { meta_.roster = std::move(roster); }
  void update_summary(int place, int field_size, double best_lap_s, double tod_sec);
  void add_marker(double sim_time, const std::string& label);

  // Records a frame when `sim_time` has advanced by at least 1/sample_hz.
  bool maybe_record(double sim_time, std::vector<ReplayCarSample> cars);
  // Forces recording a frame immediately (used for deterministic tests & events).
  void record_frame(double sim_time, std::vector<ReplayCarSample> cars);

  size_t frame_count() const { return count_; }
  size_t capacity() const { return cap_; }
  double duration() const;

  // Builds a time-normalized ReplayRecording (starting at t = 0) from the ring buffer.
  ReplayRecording snapshot() const;

 private:
  bool active_ = false;
  bool ring_wrap_ = true;
  ReplayMetadata meta_{};
  std::vector<ReplayFrame> ring_;
  size_t cap_ = kDefaultMaxFrames;
  size_t head_ = 0;   // oldest frame index
  size_t count_ = 0;
  double last_record_t_ = -1e9;
  std::vector<ReplayEventMarker> raw_markers_;
};

struct ReplayFileEntry {
  std::filesystem::path path;
  std::string filename;
  std::string event_name;
  std::string player_car;
  double duration_s = 0.0;
  int64_t recorded_utc = 0;
  bool is_race = false;
};

// Persistent binary replay file I/O (magic "SHFTREPL", version 1, CRC32 checked).
bool save_replay_file(const std::filesystem::path& path, const ReplayRecording& rec, std::string* error = nullptr);
bool load_replay_file(const std::filesystem::path& path, ReplayRecording& out, std::string* error = nullptr);
bool save_replay_auto(const std::filesystem::path& dir, const ReplayRecording& rec, std::filesystem::path& out_path,
                      std::string* error = nullptr);
std::vector<ReplayFileEntry> list_replay_files(const std::filesystem::path& dir);

}  // namespace drive
