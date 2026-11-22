#include "drive/head_tracking.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "drive/cameras.h"

namespace drive {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void close_socket(Socket s) { close(s); }
#endif

using V3 = std::array<double, 3>;

V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 norm(V3 a) {
  const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  return l > 1e-12 ? mul(a, 1.0 / l) : V3{0, 0, 1};
}

// Rodrigues rotation of vector v around unit axis k by angle rad.
V3 rotate_axis(V3 v, V3 k, double angle) {
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  const double kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
  const V3 kxv = cross(k, v);
  return {v[0] * c + kxv[0] * s + k[0] * kv * (1.0 - c),
          v[1] * c + kxv[1] * s + k[1] * kv * (1.0 - c),
          v[2] * c + kxv[2] * s + k[2] * kv * (1.0 - c)};
}

Socket open_loopback_udp(int port) {
  const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) return kNoSocket;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(std::clamp(port, 1, 65535)));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_socket(s);
    return kNoSocket;
  }
#if defined(_WIN32)
  const DWORD timeout_ms = 100;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
  const timeval tv{0, 100000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
  return s;
}

HeadPose apply_invert_and_scale(HeadPose p, const std::array<bool, 6>& invert, float keep) {
  for (size_t i = 0; i < 6; ++i) {
    p[i] *= keep;
    if (invert[i]) p[i] = -p[i];
  }
  return p;
}

}  // namespace

std::optional<HeadPose> parse_opentrack_packet(std::span<const uint8_t> packet) {
  if (packet.size() < 48) return std::nullopt;
  double v[6]{};
  std::memcpy(v, packet.data(), sizeof(v));
  auto finite = [](double x) -> float { return std::isfinite(x) ? static_cast<float>(x) : 0.0f; };
  // OpenTrack UDP format: x, y, z (cm), yaw, pitch, roll (degrees).
  HeadPose p;
  p.yaw_deg = finite(v[3]);
  p.pitch_deg = finite(v[4]);
  p.roll_deg = finite(v[5]);
  p.x_m = finite(v[0]) / 100.0f;
  p.y_m = finite(v[1]) / 100.0f;
  p.z_m = finite(v[2]) / 100.0f;
  return p;
}

std::array<bool, 6> parse_head_invert(std::string_view text) {
  static constexpr std::string_view kNames[] = {"yaw", "pitch", "roll", "x", "y", "z"};
  std::array<bool, 6> inv{};
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = std::min(text.find(',', start), text.size());
    std::string_view tok = text.substr(start, end - start);
    while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.remove_prefix(1);
    while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.remove_suffix(1);
    std::string low(tok);
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (size_t i = 0; i < 6; ++i) {
      if (low == kNames[i]) inv[i] = true;
    }
    if (end == text.size()) break;
    start = end + 1;
  }
  return inv;
}

std::string format_head_invert(const std::array<bool, 6>& invert) {
  static constexpr const char* kNames[] = {"yaw", "pitch", "roll", "x", "y", "z"};
  std::string out;
  for (size_t i = 0; i < 6; ++i) {
    if (!invert[i]) continue;
    if (!out.empty()) out += ',';
    out += kNames[i];
  }
  return out;
}

std::optional<HeadPose> parse_head_test_pose(std::string_view text) {
  if (text.empty()) return std::nullopt;
  HeadPose pose{};
  size_t start = 0;
  for (size_t i = 0; i < 6 && start <= text.size(); ++i) {
    const size_t end = std::min(text.find(',', start), text.size());
    std::string part(text.substr(start, end - start));
    try {
      const float val = std::stof(part);
      pose[i] = std::isfinite(val) ? val : 0.0f;
    } catch (...) {
      pose[i] = 0.0f;
    }
    if (end == text.size()) break;
    start = end + 1;
  }
  return pose;
}

void apply_head_tracking(CameraView& view, const HeadPose& pose, CameraMode mode) {
  if (mode != CameraMode::kCockpit && mode != CameraMode::kHood) return;
  if (pose.is_zero()) return;

  // Orthonormal camera basis: forward (+Z), up (+Y), right = up x forward (+X in left-handed / car frame).
  const V3 f0 = norm(view.forward);
  const V3 u0 = norm(view.up);
  const V3 r0 = norm(cross(u0, f0));
  const V3 true_u0 = norm(cross(f0, r0));

  // Clamp head translation conservatively so extreme tracker spikes stay near the cabin.
  const double tx = std::clamp(double(pose.x_m), -0.35, 0.35);
  const double ty = std::clamp(double(pose.y_m), -0.25, 0.25);
  const double tz = std::clamp(double(pose.z_m), -0.35, 0.35);
  view.eye = add(view.eye, add(mul(r0, tx), add(mul(true_u0, ty), mul(f0, tz))));

  // Apply yaw (about up, positive right), pitch (about right, positive up), roll (about forward, positive right).
  const double yaw = std::clamp(double(pose.yaw_deg), -135.0, 135.0) * kDeg;
  const double pitch = std::clamp(double(pose.pitch_deg), -75.0, 75.0) * kDeg;
  const double roll = std::clamp(double(pose.roll_deg), -60.0, 60.0) * kDeg;

  V3 f = f0;
  V3 u = true_u0;
  V3 r = r0;

  if (std::fabs(yaw) > 1e-9) {
    // Rotating forward toward +right: f' = f cos(y) + r sin(y).
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    f = norm(add(mul(f, cy), mul(r, sy)));
    r = norm(cross(u, f));
  }
  if (std::fabs(pitch) > 1e-9) {
    // Positive pitch looks up: f' = f cos(p) + u sin(p), u' = u cos(p) - f sin(p).
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const V3 fn = norm(add(mul(f, cp), mul(u, sp)));
    const V3 un = norm(add(mul(u, cp), mul(f, -sp)));
    f = fn;
    u = un;
  }
  if (std::fabs(roll) > 1e-9) {
    // Positive roll tilts head right: u' = u cos(roll) + r sin(roll).
    r = norm(cross(u, f));
    u = norm(rotate_axis(u, f, -roll));
  }
  view.forward = f;
  view.up = u;
}

HeadTrackingReceiver::~HeadTrackingReceiver() { stop(); }

void HeadTrackingReceiver::configure(const HeadTrackingConfig& cfg) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
  }
  const bool want_socket = cfg.enabled && cfg.test_pose.empty();
  port_.store(want_socket ? std::clamp(cfg.port, 1, 65535) : 0, std::memory_order_relaxed);
  if (want_socket && !thread_.joinable()) {
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&HeadTrackingReceiver::listener_loop, this);
  }
}

void HeadTrackingReceiver::stop() {
  stop_.store(true, std::memory_order_relaxed);
  port_.store(0, std::memory_order_relaxed);
  if (thread_.joinable()) thread_.join();
}

void HeadTrackingReceiver::listener_loop() {
#if defined(_WIN32)
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif
  using namespace std::chrono_literals;
  Socket s = kNoSocket;
  int bound_port = 0;
  auto next_retry = std::chrono::steady_clock::time_point{};
  uint8_t buf[256]{};

  while (!stop_.load(std::memory_order_relaxed)) {
    const int wanted = port_.load(std::memory_order_relaxed);
    if (s != kNoSocket && wanted != bound_port) {
      close_socket(s);
      s = kNoSocket;
      bound_port = 0;
    }
    if (s == kNoSocket) {
      if (wanted == 0 || std::chrono::steady_clock::now() < next_retry) {
        std::this_thread::sleep_for(50ms);
        continue;
      }
      s = open_loopback_udp(wanted);
      if (s == kNoSocket) {
        next_retry = std::chrono::steady_clock::now() + 1s;
        continue;
      }
      bound_port = wanted;
    }
    const auto n = recv(s, reinterpret_cast<char*>(buf), int(sizeof(buf)), 0);
    if (n <= 0) continue;
    const auto parsed = parse_opentrack_packet({buf, size_t(n)});
    if (!parsed) continue;
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = *parsed;
    last_rx_ = std::chrono::steady_clock::now();
    have_pose_ = true;
  }

  if (s != kNoSocket) close_socket(s);
#if defined(_WIN32)
  WSACleanup();
#endif
}

HeadPose HeadTrackingReceiver::current_pose() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!cfg_.enabled) return {};
  if (const auto test = parse_head_test_pose(cfg_.test_pose)) {
    return apply_invert_and_scale(*test, cfg_.invert, 1.0f);
  }
  if (!have_pose_) return {};
  const double age =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - last_rx_).count();
  // Hold for 1.0 s after packets stop, then smoothly ease back to centre over 0.5 s.
  const float keep = static_cast<float>(std::clamp(1.0 - (age - 1.0) / 0.5, 0.0, 1.0));
  return apply_invert_and_scale(latest_, cfg_.invert, keep);
}

bool HeadTrackingReceiver::receiving() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!cfg_.enabled || !have_pose_) return false;
  const double age =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - last_rx_).count();
  return age < 1.5;
}

void HeadTrackingReceiver::inject_packet_at(std::span<const uint8_t> packet, double time_s) {
  const auto parsed = parse_opentrack_packet(packet);
  if (!parsed) return;
  std::lock_guard<std::mutex> lock(mutex_);
  latest_ = *parsed;
  last_rx_test_s_ = time_s;
  have_test_pose_ = true;
}

HeadPose HeadTrackingReceiver::pose_at(double time_s, const HeadTrackingConfig& cfg) const {
  if (!cfg.enabled) return {};
  if (const auto test = parse_head_test_pose(cfg.test_pose)) {
    return apply_invert_and_scale(*test, cfg.invert, 1.0f);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!have_test_pose_) return {};
  const double age = std::max(0.0, time_s - last_rx_test_s_);
  const float keep = static_cast<float>(std::clamp(1.0 - (age - 1.0) / 0.5, 0.0, 1.0));
  return apply_invert_and_scale(latest_, cfg.invert, keep);
}

}  // namespace drive
