#include "drive/telemetry_udp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace drive::telemetry {
namespace {

// Exact FM7 Data Out Dash field byte offsets (232-byte Sled + 79-byte Dash = 311 bytes).
constexpr size_t kIsRaceOn = 0;                     // s32
constexpr size_t kTimestampMs = 4;                  // u32
constexpr size_t kEngineMaxRpm = 8;                 // f32
constexpr size_t kEngineIdleRpm = 12;               // f32
constexpr size_t kCurrentEngineRpm = 16;            // f32
constexpr size_t kAccelX = 20;                      // f32
constexpr size_t kAccelY = 24;                      // f32
constexpr size_t kAccelZ = 28;                      // f32
constexpr size_t kVelX = 32;                        // f32
constexpr size_t kVelY = 36;                        // f32
constexpr size_t kVelZ = 40;                        // f32
constexpr size_t kAngVelX = 44;                     // f32
constexpr size_t kAngVelY = 48;                     // f32
constexpr size_t kAngVelZ = 52;                     // f32
constexpr size_t kYaw = 56;                         // f32
constexpr size_t kPitch = 60;                       // f32
constexpr size_t kRoll = 64;                        // f32
constexpr size_t kNormalizedSuspensionTravel = 68;  // 4 x f32
constexpr size_t kTireSlipRatio = 84;               // 4 x f32
constexpr size_t kWheelRotationSpeed = 100;         // 4 x f32
constexpr size_t kWheelOnRumbleStrip = 116;         // 4 x s32
constexpr size_t kWheelInPuddleDepth = 132;         // 4 x f32
constexpr size_t kSurfaceRumble = 148;              // 4 x f32
constexpr size_t kTireSlipAngle = 164;              // 4 x f32
constexpr size_t kTireCombinedSlip = 180;           // 4 x f32
constexpr size_t kSuspensionTravelMeters = 196;     // 4 x f32
constexpr size_t kCarOrdinal = 212;                 // s32
constexpr size_t kCarClass = 216;                   // s32
constexpr size_t kCarPi = 220;                      // s32
constexpr size_t kDrivetrainType = 224;             // s32
constexpr size_t kNumCylinders = 228;               // s32
// Dash extension starts at byte 232 (shifted by +12 in FH4/5 Horizon).
constexpr size_t kPosX = 232;                       // f32
constexpr size_t kPosY = 236;                       // f32
constexpr size_t kPosZ = 240;                       // f32
constexpr size_t kSpeed = 244;                      // f32
constexpr size_t kPower = 248;                      // f32
constexpr size_t kTorque = 252;                     // f32
constexpr size_t kTireTemp = 256;                   // 4 x f32
constexpr size_t kBoost = 272;                      // f32
constexpr size_t kFuel = 276;                       // f32
constexpr size_t kDistanceTraveled = 280;           // f32
constexpr size_t kBestLap = 284;                    // f32
constexpr size_t kLastLap = 288;                    // f32
constexpr size_t kCurrentLap = 292;                 // f32
constexpr size_t kCurrentRaceTime = 296;            // f32
constexpr size_t kLapNumber = 300;                  // u16
constexpr size_t kRacePosition = 302;               // u8
constexpr size_t kAccel = 303;                      // u8
constexpr size_t kBrake = 304;                      // u8
constexpr size_t kClutch = 305;                     // u8
constexpr size_t kHandBrake = 306;                  // u8
constexpr size_t kGear = 307;                       // u8
constexpr size_t kSteer = 308;                      // s8
constexpr size_t kDrivingLine = 309;                // s8
constexpr size_t kAiBrakeDiff = 310;                // s8

class PacketWriter {
 public:
  explicit PacketWriter(std::array<uint8_t, kFm7DashSize>& out) : out_(out) {}
  void u32(size_t off, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) out_[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
  }
  void s32(size_t off, int32_t v) { u32(off, static_cast<uint32_t>(v)); }
  void u16(size_t off, uint16_t v) {
    out_[off] = static_cast<uint8_t>(v & 0xFFu);
    out_[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  }
  void f32(size_t off, float v) {
    if (!std::isfinite(v)) v = 0.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    u32(off, bits);
  }
  void u8(size_t off, uint8_t v) { out_[off] = v; }
  void s8(size_t off, int8_t v) { out_[off] = static_cast<uint8_t>(v); }

 private:
  std::array<uint8_t, kFm7DashSize>& out_;
};

uint8_t unit_u8(float v) {
  if (!std::isfinite(v)) return 0;
  return static_cast<uint8_t>(std::clamp(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f), 0L, 255L));
}

int8_t signed_s8(float v) {
  if (!std::isfinite(v)) return 0;
  return static_cast<int8_t>(std::clamp(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f), -127L, 127L));
}

}  // namespace

const char* format_name(Format f) { return f == Format::kFh4Horizon ? "horizon" : "fm7"; }

Format parse_format(std::string_view text) {
  std::string l(text);
  for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (l == "horizon" || l == "fh4" || l == "fh5") return Format::kFh4Horizon;
  return Format::kFm7Dash;
}

std::array<uint8_t, kFm7DashSize> encode_fm7_dash(const Sample& s, uint32_t timestamp_ms) {
  std::array<uint8_t, kFm7DashSize> pkt{};
  PacketWriter w(pkt);
  w.u32(kTimestampMs, timestamp_ms);
  if (!s.driving) return pkt;  // IsRaceOn == 0, all telemetry fields zeroed to silence shakers

  w.s32(kIsRaceOn, 1);
  w.f32(kEngineMaxRpm, s.redline_rpm);
  w.f32(kEngineIdleRpm, s.idle_rpm);
  w.f32(kCurrentEngineRpm, s.rpm);
  w.f32(kAccelX, s.accel_x);
  w.f32(kAccelY, s.accel_y);
  w.f32(kAccelZ, s.accel_z);
  w.f32(kVelX, s.vel_x);
  w.f32(kVelY, s.vel_y);
  w.f32(kVelZ, s.vel_z);
  w.f32(kAngVelX, s.ang_vel_x);
  w.f32(kAngVelY, s.ang_vel_y);
  w.f32(kAngVelZ, s.ang_vel_z);
  w.f32(kYaw, s.yaw);
  w.f32(kPitch, s.pitch);
  w.f32(kRoll, s.roll);

  for (size_t i = 0; i < 4; ++i) {
    const float sr = s.wheel_slip_ratio[i];
    const float sa = s.wheel_slip_angle[i];
    w.f32(kNormalizedSuspensionTravel + 4 * i, s.suspension_normalized[i]);
    w.f32(kTireSlipRatio + 4 * i, sr);
    w.f32(kWheelRotationSpeed + 4 * i, s.wheel_speed_rad_s[i]);
    w.s32(kWheelOnRumbleStrip + 4 * i, s.wheel_on_rumble[i]);
    w.f32(kWheelInPuddleDepth + 4 * i, s.wheel_puddle[i]);
    w.f32(kSurfaceRumble + 4 * i, s.surface_rumble[i]);
    w.f32(kTireSlipAngle + 4 * i, sa);
    w.f32(kTireCombinedSlip + 4 * i, std::sqrt(sr * sr + sa * sa));
    w.f32(kSuspensionTravelMeters + 4 * i, s.suspension_m[i]);
  }

  w.s32(kCarOrdinal, s.car_ordinal);
  w.s32(kCarClass, s.car_class);
  w.s32(kCarPi, s.car_pi);
  w.s32(kDrivetrainType, s.drivetrain_type);
  w.s32(kNumCylinders, s.num_cylinders);

  w.f32(kPosX, s.pos_x);
  w.f32(kPosY, s.pos_y);
  w.f32(kPosZ, s.pos_z);
  w.f32(kSpeed, s.speed_mps);
  w.f32(kPower, s.power_w);
  w.f32(kTorque, s.torque_nm);
  for (size_t i = 0; i < 4; ++i) w.f32(kTireTemp + 4 * i, s.tire_temp_f[i]);
  w.f32(kBoost, s.boost_psi);
  w.f32(kFuel, s.fuel);
  w.f32(kDistanceTraveled, s.distance_traveled);
  w.f32(kBestLap, s.best_lap_s);
  w.f32(kLastLap, s.last_lap_s);
  w.f32(kCurrentLap, s.current_lap_s);
  w.f32(kCurrentRaceTime, s.current_race_time_s);
  w.u16(kLapNumber, s.lap_number);
  w.u8(kRacePosition, s.race_position);
  w.u8(kAccel, unit_u8(s.throttle));
  w.u8(kBrake, unit_u8(s.brake));
  w.u8(kClutch, unit_u8(s.clutch));
  w.u8(kHandBrake, unit_u8(s.handbrake));
  w.u8(kGear, s.gear < 0 ? uint8_t(0) : (s.gear == 0 ? kDashNeutralGear : static_cast<uint8_t>(std::min(s.gear, 10))));
  w.s8(kSteer, signed_s8(s.steer));
  w.s8(kDrivingLine, s.driving_line);
  w.s8(kAiBrakeDiff, s.ai_brake_diff);
  return pkt;
}

std::array<uint8_t, kFh4HorizonSize> encode_fh4_horizon(const Sample& s, uint32_t timestamp_ms) {
  const auto dash = encode_fm7_dash(s, timestamp_ms);
  std::array<uint8_t, kFh4HorizonSize> pkt{};
  // Sled block (bytes 0..231), 12-byte Horizon gap (bytes 232..243), Dash block (bytes 244..322), trailing pad 323.
  std::copy(dash.begin(), dash.begin() + kHorizonGapOffset, pkt.begin());
  std::copy(dash.begin() + kHorizonGapOffset, dash.end(), pkt.begin() + kHorizonGapOffset + 12);
  return pkt;
}

// ---- Sampler ----

void Sampler::reset() {
  started_ = false;
  prev_vel_ = {};
  susp_min_ = {0.0f, 0.0f, 0.0f, 0.0f};
  susp_max_ = {0.12f, 0.12f, 0.12f, 0.12f};
  prev_sim_time_ = -1.0;
  frozen_s_ = 0.0;
}

Sample Sampler::build(const Vehicle& v, bool driving, double dt_s, bool moving_only,
                      int32_t car_ordinal, int32_t car_class, int32_t car_pi,
                      uint16_t lap, uint8_t race_pos, float cur_lap_s,
                      float best_lap_s, float last_lap_s, float distance_m) {
  Sample s;
  const double sim_t = v.time();
  if (started_ && std::fabs(sim_t - prev_sim_time_) < 1e-9) {
    frozen_s_ += std::max(0.0, dt_s);
  } else {
    frozen_s_ = 0.0;
  }
  prev_sim_time_ = sim_t;

  const Vec3 vel_world = v.velocity();
  const double spd = std::sqrt(vel_world.x * vel_world.x + vel_world.y * vel_world.y + vel_world.z * vel_world.z);
  const bool frozen = frozen_s_ >= 0.5;
  const bool stopped_filtered = moving_only && spd < 0.15;
  s.driving = driving && !frozen && !stopped_filtered;

  s.rpm = static_cast<float>(v.rpm());
  s.idle_rpm = static_cast<float>(v.params().idle_rpm);
  s.redline_rpm = static_cast<float>(v.params().redline_rpm);

  const Quat q = v.orientation();
  const Vec3 vel_local = rotate_inverse(q, vel_world);
  const Vec3 ang_local = rotate_inverse(q, v.angular_velocity());
  s.vel_x = static_cast<float>(vel_local.x);
  s.vel_y = static_cast<float>(vel_local.y);
  s.vel_z = static_cast<float>(vel_local.z);
  s.ang_vel_x = static_cast<float>(ang_local.x);
  s.ang_vel_y = static_cast<float>(ang_local.y);
  s.ang_vel_z = static_cast<float>(ang_local.z);

  if (started_ && dt_s > 1e-5) {
    const Vec3 a_world = (vel_world - prev_vel_) * (1.0 / dt_s);
    const Vec3 a_local = rotate_inverse(q, a_world);
    s.accel_x = static_cast<float>(a_local.x);
    s.accel_y = static_cast<float>(a_local.y);
    s.accel_z = static_cast<float>(a_local.z);
  }
  prev_vel_ = vel_world;
  started_ = true;

  // Yaw, pitch, roll from car basis.
  const Vec3 fwd = rotate(q, {0, 0, 1});
  const Vec3 right = rotate(q, {1, 0, 0});
  const Vec3 up = rotate(q, {0, 1, 0});
  s.yaw = static_cast<float>(std::atan2(fwd.x, fwd.z));
  s.pitch = static_cast<float>(std::asin(std::clamp(fwd.y, -1.0, 1.0)));
  s.roll = static_cast<float>(std::atan2(-right.y, up.y));

  const auto& wheels = v.wheels();
  const float surf_rumble = static_cast<float>(std::clamp(v.surface_bump_rumble(), 0.0, 1.0));
  for (size_t i = 0; i < 4; ++i) {
    const float comp = static_cast<float>(wheels[i].compression);
    susp_min_[i] = std::min(susp_min_[i], comp);
    susp_max_[i] = std::max(susp_max_[i], comp);
    const float span = std::max(0.01f, susp_max_[i] - susp_min_[i]);
    s.suspension_m[i] = comp;
    s.suspension_normalized[i] = std::clamp((comp - susp_min_[i]) / span, 0.0f, 1.0f);
    s.wheel_slip_ratio[i] = static_cast<float>(wheels[i].slip_ratio);
    s.wheel_slip_angle[i] = static_cast<float>(wheels[i].slip_angle);
    s.wheel_speed_rad_s[i] = static_cast<float>(wheels[i].omega);
    s.surface_rumble[i] = surf_rumble;
    s.wheel_on_rumble[i] = surf_rumble > 0.25f ? 1 : 0;
  }

  s.car_ordinal = car_ordinal;
  s.car_class = car_class;
  s.car_pi = car_pi;
  s.drivetrain_type = v.params().layout == CarParams::Layout::kFront ? 0
                      : v.params().layout == CarParams::Layout::kAll ? 2
                                                                     : 1;
  const Vec3 pos = v.position();
  s.pos_x = static_cast<float>(pos.x);
  s.pos_y = static_cast<float>(pos.y);
  s.pos_z = static_cast<float>(pos.z);
  s.speed_mps = static_cast<float>(spd);
  const double trq = v.engine_torque(v.rpm(), v.engine_throttle());
  s.torque_nm = static_cast<float>(trq);
  s.power_w = static_cast<float>(trq * (v.rpm() * 0.10471975511965977));
  s.boost_psi = static_cast<float>(v.boost_psi());
  s.fuel = 1.0f;
  s.distance_traveled = distance_m;
  s.best_lap_s = best_lap_s;
  s.last_lap_s = last_lap_s;
  s.current_lap_s = cur_lap_s;
  s.current_race_time_s = static_cast<float>(sim_t);
  s.lap_number = lap;
  s.race_position = race_pos;

  const Input& in = v.shaped_input();
  s.throttle = static_cast<float>(in.throttle);
  s.brake = static_cast<float>(in.brake);
  s.clutch = static_cast<float>(1.0 - v.clutch());
  s.handbrake = in.handbrake ? 1.0f : 0.0f;
  s.gear = v.gear();
  s.steer = static_cast<float>(in.steer);
  return s;
}

// ---- ProbeRecorder ----

ProbeRecorder::~ProbeRecorder() { stop(); }

bool ProbeRecorder::start(const std::filesystem::path& path) {
  stop();
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  file_.open(path, std::ios::out | std::ios::trunc);
  if (!file_) return false;
  rows_ = 0;
  file_ << "timestamp_ms,race_on,rpm,speed_mps,gear,throttle,brake,clutch,handbrake,steer,"
           "slip_fl,slip_fr,slip_rl,slip_rr,susp_fl,susp_fr,susp_rl,susp_rr,pos_x,pos_y,pos_z\n";
  return bool(file_);
}

void ProbeRecorder::stop() {
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
}

void ProbeRecorder::record(uint32_t timestamp_ms, const Sample& s) {
  if (!file_.is_open()) return;
  file_ << timestamp_ms << ',' << (s.driving ? 1 : 0) << ',' << std::fixed << std::setprecision(4) << s.rpm << ','
        << s.speed_mps << ',' << s.gear << ',' << s.throttle << ',' << s.brake << ',' << s.clutch << ','
        << s.handbrake << ',' << s.steer << ',' << s.wheel_slip_ratio[0] << ',' << s.wheel_slip_ratio[1] << ','
        << s.wheel_slip_ratio[2] << ',' << s.wheel_slip_ratio[3] << ',' << s.suspension_m[0] << ','
        << s.suspension_m[1] << ',' << s.suspension_m[2] << ',' << s.suspension_m[3] << ',' << s.pos_x << ','
        << s.pos_y << ',' << s.pos_z << '\n';
  ++rows_;
}

// ---- UdpSender ----

struct UdpSender::Impl {
#if defined(_WIN32)
  using Socket = SOCKET;
  static constexpr Socket kNoSocket = INVALID_SOCKET;
  static void close_sock(Socket s) { closesocket(s); }
#else
  using Socket = int;
  static constexpr Socket kNoSocket = -1;
  static void close_sock(Socket s) { ::close(s); }
#endif
  Socket sock = kNoSocket;
  sockaddr_storage addr{};
  socklen_t addr_len = 0;
  std::string bound_target;
  bool failed = false;

  bool ensure_open(const std::string& target) {
    if (target == bound_target) return !failed;
    bound_target = target;
    failed = true;
    if (sock != kNoSocket) {
      close_sock(sock);
      sock = kNoSocket;
    }
#if defined(_WIN32)
    static const bool wsa_ok = [] {
      WSADATA d{};
      return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!wsa_ok) return false;
#endif
    const size_t colon = target.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == target.size()) return false;
    std::string host = target.substr(0, colon);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    const std::string port = target.substr(colon + 1);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock != kNoSocket) {
      std::memcpy(&addr, res->ai_addr, res->ai_addrlen);
      addr_len = static_cast<socklen_t>(res->ai_addrlen);
    }
    freeaddrinfo(res);
    if (sock == kNoSocket) return false;
    failed = false;
    return true;
  }

  void close() {
    if (sock != kNoSocket) {
      close_sock(sock);
      sock = kNoSocket;
    }
    bound_target.clear();
    failed = false;
  }
};

UdpSender::UdpSender() : impl_(std::make_unique<Impl>()) {}
UdpSender::~UdpSender() { close(); }

bool UdpSender::send(const Config& cfg, const Sample& sample, uint32_t timestamp_ms) {
  if (!cfg.enabled) return false;
  if (!impl_->ensure_open(cfg.target)) return false;
  if (cfg.format == Format::kFh4Horizon) {
    const auto pkt = encode_fh4_horizon(sample, timestamp_ms);
    const auto r = sendto(impl_->sock, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0,
                          reinterpret_cast<const sockaddr*>(&impl_->addr), impl_->addr_len);
    if (r > 0) ++sent_;
    return r > 0;
  }
  const auto pkt = encode_fm7_dash(sample, timestamp_ms);
  const auto r = sendto(impl_->sock, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0,
                        reinterpret_cast<const sockaddr*>(&impl_->addr), impl_->addr_len);
  if (r > 0) ++sent_;
  return r > 0;
}

void UdpSender::close() {
  if (impl_) impl_->close();
}

}  // namespace drive::telemetry
