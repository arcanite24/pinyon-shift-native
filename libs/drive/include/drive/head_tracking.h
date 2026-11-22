// OpenTrack 6-DoF UDP head tracking receiver (default 127.0.0.1:4242) for
// cockpit and hood cameras, with axis invert and 1 s timeout smooth recenter.
// Pure C++ (no SDL dependency) so libs/drive and unit tests stay headless.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace drive {

struct CameraView;
enum class CameraMode;

// Yaw, pitch, roll in degrees (positive: look right, look up, tilt right)
// and x, y, z translation in metres (right, up, forward).
struct HeadPose {
  float yaw_deg = 0.0f;
  float pitch_deg = 0.0f;
  float roll_deg = 0.0f;
  float x_m = 0.0f;
  float y_m = 0.0f;
  float z_m = 0.0f;

  float& operator[](size_t i) {
    switch (i) {
      case 0: return yaw_deg;
      case 1: return pitch_deg;
      case 2: return roll_deg;
      case 3: return x_m;
      case 4: return y_m;
      default: return z_m;
    }
  }
  float operator[](size_t i) const {
    switch (i) {
      case 0: return yaw_deg;
      case 1: return pitch_deg;
      case 2: return roll_deg;
      case 3: return x_m;
      case 4: return y_m;
      default: return z_m;
    }
  }
  bool is_zero() const {
    return yaw_deg == 0.0f && pitch_deg == 0.0f && roll_deg == 0.0f && x_m == 0.0f && y_m == 0.0f && z_m == 0.0f;
  }
};

struct HeadTrackingConfig {
  bool enabled = false;
  int port = 4242;
  // Invert flags for {yaw, pitch, roll, x, y, z}.
  std::array<bool, 6> invert{};
  std::string test_pose;  // optional fixed pose "yaw,pitch,roll,x,y,z"
};

// Parses one 48-byte OpenTrack UDP packet: 6 little-endian doubles:
// x, y, z (cm) -> converted to metres; yaw, pitch, roll (degrees).
// Non-finite values are clamped to 0. Returns nullopt if packet < 48 bytes.
std::optional<HeadPose> parse_opentrack_packet(std::span<const uint8_t> packet);

// Parses/formats comma-separated axis names ("yaw,pitch,roll,x,y,z").
std::array<bool, 6> parse_head_invert(std::string_view text);
std::string format_head_invert(const std::array<bool, 6>& invert);
std::optional<HeadPose> parse_head_test_pose(std::string_view text);

// Applies a 6-DoF HeadPose to a CameraView when mode is kCockpit or kHood.
// Other camera modes (chase, bumper) are left untouched.
void apply_head_tracking(CameraView& view, const HeadPose& pose, CameraMode mode);

// Background UDP receiver listening on 127.0.0.1:<port> with 1.0 s hold and
// 0.5 s smooth linear ease-back to centre when packets stop arriving.
class HeadTrackingReceiver {
 public:
  HeadTrackingReceiver() = default;
  ~HeadTrackingReceiver();
  HeadTrackingReceiver(const HeadTrackingReceiver&) = delete;
  HeadTrackingReceiver& operator=(const HeadTrackingReceiver&) = delete;

  // Starts/stops/rebinds the background UDP listener according to `cfg`.
  void configure(const HeadTrackingConfig& cfg);
  void stop();

  // Current pose at wall-clock time (applies timeout ease-back & axis invert).
  HeadPose current_pose() const;
  bool receiving() const;

  // Deterministic injection & evaluation for headless unit tests (time in seconds).
  void inject_packet_at(std::span<const uint8_t> packet, double time_s);
  HeadPose pose_at(double time_s, const HeadTrackingConfig& cfg) const;

 private:
  void listener_loop();

  mutable std::mutex mutex_;
  HeadTrackingConfig cfg_{};
  HeadPose latest_{};
  std::chrono::steady_clock::time_point last_rx_{};
  double last_rx_test_s_ = -1e9;
  bool have_pose_ = false;
  bool have_test_pose_ = false;

  std::atomic<int> port_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace drive
