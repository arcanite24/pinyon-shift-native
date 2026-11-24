#include "drive/replay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>

namespace drive {
namespace {

constexpr char kReplayMagic[8] = {'S', 'H', 'F', 'T', 'R', 'E', 'P', 'L'};
constexpr uint32_t kReplayVersion = 3;
constexpr uint32_t kMaxFramesFile = 360000;   // 100 minutes at 60 Hz
constexpr uint32_t kMaxCarsPerFrame = 128;
constexpr uint32_t kMaxRosterEntries = 256;
constexpr uint32_t kMaxMarkers = 1024;
constexpr uint32_t kMaxStringBytes = 1024;

struct ReplayCarSampleLegacyV2 {
  uint32_t slot;
  float pos[3];
  float quat[4];
  float vel[3];
  float speed_mps;
  float rpm;
  float redline_rpm;
  int8_t gear;
  uint8_t throttle_u8;
  uint8_t brake_u8;
  uint8_t handbrake_u8;
  int8_t steer_i8;
  uint8_t race_place;
  uint16_t lamp_bits;
  float wheel_steer[4];
  float wheel_spin[4];
  float wheel_omega[4];
  float wheel_y[4];
};
static_assert(sizeof(ReplayCarSampleLegacyV2) == 128);

uint32_t crc32_bytes(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void slerp_quat(const float a[4], const float b[4], float t, float out[4]) {
  float bx = b[1], by = b[2], bz = b[3], bw = b[0];
  float dot = a[0] * bw + a[1] * bx + a[2] * by + a[3] * bz;
  if (dot < 0.0f) {
    dot = -dot;
    bw = -bw;
    bx = -bx;
    by = -by;
    bz = -bz;
  }
  if (dot > 0.9995f) {
    out[0] = a[0] + (bw - a[0]) * t;
    out[1] = a[1] + (bx - a[1]) * t;
    out[2] = a[2] + (by - a[2]) * t;
    out[3] = a[3] + (bz - a[3]) * t;
  } else {
    const float theta0 = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sin0 = std::sin(theta0);
    const float s0 = std::sin((1.0f - t) * theta0) / sin0;
    const float s1 = std::sin(t * theta0) / sin0;
    out[0] = a[0] * s0 + bw * s1;
    out[1] = a[1] * s0 + bx * s1;
    out[2] = a[2] * s0 + by * s1;
    out[3] = a[3] * s0 + bz * s1;
  }
  const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
  if (len > 1e-6f) {
    for (int i = 0; i < 4; ++i) out[i] /= len;
  } else {
    out[0] = 1.0f;
    out[1] = out[2] = out[3] = 0.0f;
  }
}

float lerp_angle(float a, float b, float t) {
  constexpr float kTwoPi = 6.2831853071795864769f;
  constexpr float kPi = 3.14159265358979323846f;
  float diff = std::fmod(b - a, kTwoPi);
  if (diff > kPi) diff -= kTwoPi;
  if (diff < -kPi) diff += kTwoPi;
  return a + diff * t;
}

ReplayCarSample blend_car(const ReplayCarSample& a, const ReplayCarSample& b, float t) {
  ReplayCarSample out = a;
  auto lerpf = [&](float x, float y) { return x + (y - x) * t; };
  for (int i = 0; i < 3; ++i) {
    out.pos[i] = lerpf(a.pos[i], b.pos[i]);
    out.vel[i] = lerpf(a.vel[i], b.vel[i]);
  }
  slerp_quat(a.quat, b.quat, t, out.quat);
  out.speed_mps = lerpf(a.speed_mps, b.speed_mps);
  out.rpm = lerpf(a.rpm, b.rpm);
  out.redline_rpm = b.redline_rpm;
  out.gear = t < 0.5f ? a.gear : b.gear;
  out.throttle_u8 = uint8_t(std::clamp(std::lround(lerpf(float(a.throttle_u8), float(b.throttle_u8))), 0l, 255l));
  out.brake_u8 = uint8_t(std::clamp(std::lround(lerpf(float(a.brake_u8), float(b.brake_u8))), 0l, 255l));
  out.handbrake_u8 = uint8_t(std::clamp(std::lround(lerpf(float(a.handbrake_u8), float(b.handbrake_u8))), 0l, 255l));
  out.steer_i8 = int8_t(std::clamp(std::lround(lerpf(float(a.steer_i8), float(b.steer_i8))), -127l, 127l));
  out.race_place = t < 0.5f ? a.race_place : b.race_place;
  out.lamp_bits = t < 0.5f ? a.lamp_bits : b.lamp_bits;
  for (int i = 0; i < 4; ++i) {
    out.wheel_steer[i] = lerpf(a.wheel_steer[i], b.wheel_steer[i]);
    out.wheel_spin[i] = lerp_angle(a.wheel_spin[i], b.wheel_spin[i], t);
    out.wheel_omega[i] = lerpf(a.wheel_omega[i], b.wheel_omega[i]);
    out.wheel_y[i] = lerpf(a.wheel_y[i], b.wheel_y[i]);
    out.wheel_slip[i] = lerpf(a.wheel_slip[i], b.wheel_slip[i]);
    out.wheel_compression[i] = lerpf(a.wheel_compression[i], b.wheel_compression[i]);
    out.wheel_temp_c[i] = lerpf(a.wheel_temp_c[i], b.wheel_temp_c[i]);
    out.damage[i] = lerpf(a.damage[i], b.damage[i]);
  }
  out.engine_power_kw = lerpf(a.engine_power_kw, b.engine_power_kw);
  out.engine_torque_nm = lerpf(a.engine_torque_nm, b.engine_torque_nm);
  out.boost_psi = lerpf(a.boost_psi, b.boost_psi);
  out.boost_max_psi = b.boost_max_psi > 0.0f ? b.boost_max_psi : a.boost_max_psi;
  return out;
}

class BinWriter {
 public:
  std::vector<uint8_t> buf;
  template <typename T>
  void pod(const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(T));
  }
  void str(const std::string& s) {
    const uint32_t n = uint32_t(std::min<size_t>(s.size(), kMaxStringBytes));
    pod(n);
    if (n > 0) buf.insert(buf.end(), s.data(), s.data() + n);
  }
};

class BinReader {
 public:
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;

  template <typename T>
  bool pod(T& v) {
    if (pos + sizeof(T) > size) return false;
    std::memcpy(&v, data + pos, sizeof(T));
    pos += sizeof(T);
    return true;
  }
  bool str(std::string& s) {
    uint32_t n = 0;
    if (!pod(n) || n > kMaxStringBytes || pos + n > size) return false;
    s.assign(reinterpret_cast<const char*>(data + pos), n);
    pos += n;
    return true;
  }
};

std::string timestamp_tag() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
#ifdef _WIN32
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
  return buf;
}

}  // namespace

uint16_t pack_replay_lamps(const CarLampState& lamps) {
  uint16_t bits = 0;
  if (lamps.headlamps_on) bits |= kReplayLampHeadlights;
  if (lamps.fullbeam_on) bits |= kReplayLampHighBeam;
  if (lamps.brake_on) bits |= kReplayLampBrake;
  if (lamps.reverse_on) bits |= kReplayLampReverse;
  if (lamps.left_ind_active) bits |= kReplayLampLeftTurn;
  if (lamps.right_ind_active) bits |= kReplayLampRightTurn;
  if (lamps.left_ind_active && lamps.right_ind_active) bits |= kReplayLampHazards;
  if (lamps.fog_on) bits |= kReplayLampFog;
  return bits;
}

CarLampState unpack_replay_lamps(uint16_t bits) {
  CarLampState s;
  s.headlamps_on = (bits & kReplayLampHeadlights) != 0;
  s.fullbeam_on = (bits & kReplayLampHighBeam) != 0;
  s.brake_on = (bits & kReplayLampBrake) != 0;
  s.reverse_on = (bits & kReplayLampReverse) != 0;
  s.left_ind_active = (bits & (kReplayLampLeftTurn | kReplayLampHazards)) != 0;
  s.right_ind_active = (bits & (kReplayLampRightTurn | kReplayLampHazards)) != 0;
  s.fog_on = (bits & kReplayLampFog) != 0;
  return s;
}

ReplayCarSample ReplayCarSample::from_vehicle(uint32_t slot, const Vehicle& v, const CarLampState& lamps,
                                              double throttle, double brake, double handbrake, double steer,
                                              uint8_t place, const CarDamageState* dmg) {
  ReplayCarSample s;
  s.slot = slot;
  const Vec3 p = v.centre_of_mass();  // matches Vehicle::Pose::pos
  const Quat q = v.orientation();
  const Vec3 vel = v.velocity();
  s.pos[0] = float(p.x);
  s.pos[1] = float(p.y);
  s.pos[2] = float(p.z);
  s.quat[0] = float(q.w);
  s.quat[1] = float(q.x);
  s.quat[2] = float(q.y);
  s.quat[3] = float(q.z);
  s.vel[0] = float(vel.x);
  s.vel[1] = float(vel.y);
  s.vel[2] = float(vel.z);
  s.speed_mps = float(v.speed());
  s.rpm = float(v.rpm());
  s.redline_rpm = float(v.params().redline_rpm);
  s.gear = int8_t(std::clamp(v.gear(), -1, 10));
  s.throttle_u8 = uint8_t(std::clamp(std::lround(throttle * 255.0), 0l, 255l));
  s.brake_u8 = uint8_t(std::clamp(std::lround(brake * 255.0), 0l, 255l));
  s.handbrake_u8 = uint8_t(std::clamp(std::lround(handbrake * 255.0), 0l, 255l));
  s.steer_i8 = int8_t(std::clamp(std::lround(steer * 127.0), -127l, 127l));
  s.race_place = place;
  s.lamp_bits = pack_replay_lamps(lamps);
  for (int i = 0; i < 4; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    const int axle = i < 2 ? 0 : 1;
    s.wheel_steer[i] = float(w.steer);
    s.wheel_spin[i] = float(w.spin);
    s.wheel_omega[i] = float(w.omega);
    s.wheel_y[i] = float(w.centre.y);
    s.wheel_slip[i] = w.contact ? float(std::hypot(w.slip_ratio, w.slip_angle)) : 0.0f;
    const double half_travel = std::max(0.04, v.params().bump_travel[size_t(axle)]);
    s.wheel_compression[i] =
        w.contact ? float(std::clamp(0.5 + 0.5 * (w.compression / half_travel), 0.0, 1.0)) : 0.0f;
    s.wheel_temp_c[i] = float(v.tyre_temp_c(i));
  }
  if (dmg != nullptr) {
    s.damage[0] = std::clamp(dmg->engine_damage, 0.0f, 1.0f);
    s.damage[1] = std::clamp(std::max(dmg->trans_damage, float(v.transmission_damage())), 0.0f, 1.0f);
    s.damage[2] = std::clamp(
        std::max({dmg->suspension_damage[0], dmg->suspension_damage[1], dmg->suspension_damage[2],
                  dmg->suspension_damage[3]}),
        0.0f, 1.0f);
    s.damage[3] = std::clamp(
        std::max({dmg->aero_damage, dmg->scratch_amount, dmg->zone_front, dmg->zone_rear, dmg->zone_left,
                  dmg->zone_right}),
        0.0f, 1.0f);
  } else {
    const auto& dm = v.damage_mods();
    s.damage[0] = float(std::clamp((1.0 - dm.engine_torque) / 0.45, 0.0, 1.0));
    s.damage[1] = float(std::clamp(std::max(v.transmission_damage(), dm.shift_time_add / 0.60), 0.0, 1.0));
    const double min_susp =
        std::min({dm.suspension_scale[0], dm.suspension_scale[1], dm.suspension_scale[2], dm.suspension_scale[3]});
    s.damage[2] = float(std::clamp((1.0 - min_susp) / 0.42, 0.0, 1.0));
    s.damage[3] = float(std::clamp((1.0 - dm.aero_downforce) / 0.50, 0.0, 1.0));
  }
  const double tq = v.engine_torque(v.rpm(), v.engine_throttle());
  constexpr double kRpmToRadS = 3.14159265358979323846 / 30.0;
  s.engine_torque_nm = float(tq);
  s.engine_power_kw = float((tq * v.rpm() * kRpmToRadS) / 1000.0);
  const bool has_fi = v.params().has_turbo || v.params().has_supercharger;
  s.boost_psi = has_fi ? float(std::max(0.0, v.boost_psi())) : 0.0f;
  s.boost_max_psi = has_fi ? float(std::max(1.0, v.params().boost_max_psi)) : 0.0f;
  return s;
}

void ReplayCarSample::apply_to_pose(Vehicle::Pose& out_pose) const {
  out_pose.pos = {double(pos[0]), double(pos[1]), double(pos[2])};
  out_pose.vel = {double(vel[0]), double(vel[1]), double(vel[2])};
  // drive::Quat is (x, y, z, w); ReplayCarSample::quat is (w, x, y, z).
  out_pose.rot = {double(quat[1]), double(quat[2]), double(quat[3]), double(quat[0])};
  for (int i = 0; i < 4; ++i) {
    out_pose.wheels[size_t(i)].steer = double(wheel_steer[i]);
    out_pose.wheels[size_t(i)].spin = double(wheel_spin[i]);
    out_pose.wheels[size_t(i)].omega = double(wheel_omega[i]);
    out_pose.wheels[size_t(i)].centre.y = double(wheel_y[i]);
  }
}

bool ReplayRecording::sample(double t_rel, std::vector<ReplayCarSample>& out_cars) const {
  out_cars.clear();
  if (frames.empty()) return false;
  if (frames.size() == 1 || t_rel <= 0.0) {
    out_cars = frames.front().cars;
    return true;
  }
  const double t_abs = frames.front().time_s + std::clamp(t_rel, 0.0, duration());
  if (t_abs >= frames.back().time_s) {
    out_cars = frames.back().cars;
    return true;
  }
  auto it = std::lower_bound(frames.begin(), frames.end(), t_abs,
                             [](const ReplayFrame& f, double t) { return f.time_s < t; });
  if (it == frames.begin()) {
    out_cars = frames.front().cars;
    return true;
  }
  const ReplayFrame& f1 = *it;
  const ReplayFrame& f0 = *(it - 1);
  const double dt = f1.time_s - f0.time_s;
  const float alpha = dt > 1e-6 ? float(std::clamp((t_abs - f0.time_s) / dt, 0.0, 1.0)) : 0.0f;

  out_cars.reserve(f0.cars.size());
  for (const auto& c0 : f0.cars) {
    const ReplayCarSample* c1 = nullptr;
    for (const auto& cand : f1.cars) {
      if (cand.slot == c0.slot) {
        c1 = &cand;
        break;
      }
    }
    out_cars.push_back(c1 ? blend_car(c0, *c1, alpha) : c0);
  }
  return true;
}

bool ReplayRecording::sample_car(double t_rel, uint32_t slot, ReplayCarSample& out_car) const {
  std::vector<ReplayCarSample> all;
  if (!sample(t_rel, all)) return false;
  for (const auto& c : all) {
    if (c.slot == slot) {
      out_car = c;
      return true;
    }
  }
  if (!all.empty()) {
    out_car = all.front();
    return true;
  }
  return false;
}

const ReplayCarInfo* ReplayRecording::find_car_info(uint32_t slot) const {
  for (const auto& r : meta.roster)
    if (r.slot == slot) return &r;
  return nullptr;
}

void ReplayRecorder::begin(const ReplayMetadata& meta, size_t max_frames, bool ring_wrap) {
  meta_ = meta;
  if (meta_.sample_hz <= 1.0) meta_.sample_hz = 60.0;
  if (meta_.recorded_utc == 0) {
    meta_.recorded_utc = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  }
  cap_ = std::max<size_t>(60, max_frames);
  ring_wrap_ = ring_wrap;
  ring_.clear();
  ring_.resize(cap_);
  head_ = 0;
  count_ = 0;
  last_record_t_ = -1e9;
  raw_markers_.clear();
  active_ = true;
}

void ReplayRecorder::clear() {
  active_ = false;
  ring_.clear();
  head_ = 0;
  count_ = 0;
  last_record_t_ = -1e9;
  raw_markers_.clear();
}

void ReplayRecorder::update_summary(int place, int field_size, double best_lap_s, double tod_sec) {
  meta_.player_place = place;
  meta_.field_size = field_size;
  meta_.best_lap_s = best_lap_s;
  meta_.tod_sec = tod_sec;
}

void ReplayRecorder::add_marker(double sim_time, const std::string& label) {
  if (!active_ || label.empty()) return;
  raw_markers_.push_back({sim_time, label});
}

bool ReplayRecorder::maybe_record(double sim_time, std::vector<ReplayCarSample> cars) {
  if (!active_) return false;
  const double interval = 1.0 / meta_.sample_hz;
  if (count_ > 0 && sim_time - last_record_t_ < interval - 1e-5) return false;
  record_frame(sim_time, std::move(cars));
  return true;
}

void ReplayRecorder::record_frame(double sim_time, std::vector<ReplayCarSample> cars) {
  if (!active_ || cap_ == 0) return;
  if (count_ > 0) {
    const size_t newest = (head_ + count_ - 1) % cap_;
    if (sim_time < ring_[newest].time_s) {
      // Handle rewind/reset cleanly by truncating frames strictly newer than sim_time.
      while (count_ > 0 && ring_[(head_ + count_ - 1) % cap_].time_s > sim_time) --count_;
    }
  }
  if (count_ < cap_) {
    const size_t idx = (head_ + count_) % cap_;
    ring_[idx] = {sim_time, std::move(cars)};
    ++count_;
  } else if (ring_wrap_) {
    ring_[head_] = {sim_time, std::move(cars)};
    head_ = (head_ + 1) % cap_;
  } else {
    return;
  }
  last_record_t_ = sim_time;
}

double ReplayRecorder::duration() const {
  if (count_ < 2) return 0.0;
  const double t0 = ring_[head_].time_s;
  const double t1 = ring_[(head_ + count_ - 1) % cap_].time_s;
  return std::max(0.0, t1 - t0);
}

ReplayRecording ReplayRecorder::snapshot() const {
  ReplayRecording out;
  out.meta = meta_;
  out.meta.markers.clear();
  if (count_ == 0) return out;
  const double t0 = ring_[head_].time_s;
  const double t1 = ring_[(head_ + count_ - 1) % cap_].time_s;
  out.frames.reserve(count_);
  for (size_t i = 0; i < count_; ++i) {
    const ReplayFrame& src = ring_[(head_ + i) % cap_];
    ReplayFrame f;
    f.time_s = std::max(0.0, src.time_s - t0);
    f.cars = src.cars;
    out.frames.push_back(std::move(f));
  }
  for (const auto& m : raw_markers_) {
    if (m.time_s >= t0 - 0.05 && m.time_s <= t1 + 0.05) {
      out.meta.markers.push_back({std::clamp(m.time_s - t0, 0.0, std::max(0.0, t1 - t0)), m.label});
    }
  }
  return out;
}

bool save_replay_file(const std::filesystem::path& path, const ReplayRecording& rec, std::string* error) {
  if (rec.frames.empty()) {
    if (error) *error = "cannot save empty replay";
    return false;
  }
  BinWriter payload;
  payload.str(rec.meta.event_name);
  payload.str(rec.meta.world_name);
  payload.pod(rec.meta.tod_sec);
  payload.pod(rec.meta.recorded_utc);
  payload.pod(rec.meta.sample_hz);
  const uint8_t is_race = rec.meta.is_race ? 1 : 0;
  payload.pod(is_race);
  const int32_t place = rec.meta.player_place;
  const int32_t field = rec.meta.field_size;
  payload.pod(place);
  payload.pod(field);
  payload.pod(rec.meta.best_lap_s);

  const uint32_t roster_n = uint32_t(std::min<size_t>(rec.meta.roster.size(), kMaxRosterEntries));
  payload.pod(roster_n);
  for (uint32_t i = 0; i < roster_n; ++i) {
    const auto& r = rec.meta.roster[i];
    payload.pod(r.slot);
    payload.pod(r.car_id);
    payload.str(r.car_media);
    payload.str(r.driver_name);
    const uint8_t flags = (r.is_player ? 1u : 0u) | (r.is_traffic ? 2u : 0u) | (r.has_body_color ? 4u : 0u);
    payload.pod(flags);
    payload.pod(r.paint_index);
    payload.pod(r.body_color);
    payload.pod(r.rim_id);
    payload.str(r.livery_id);
  }

  const uint32_t marker_n = uint32_t(std::min<size_t>(rec.meta.markers.size(), kMaxMarkers));
  payload.pod(marker_n);
  for (uint32_t i = 0; i < marker_n; ++i) {
    payload.pod(rec.meta.markers[i].time_s);
    payload.str(rec.meta.markers[i].label);
  }

  const uint32_t frame_n = uint32_t(std::min<size_t>(rec.frames.size(), kMaxFramesFile));
  payload.pod(frame_n);
  for (uint32_t fi = 0; fi < frame_n; ++fi) {
    const auto& f = rec.frames[fi];
    payload.pod(f.time_s);
    const uint32_t car_n = uint32_t(std::min<size_t>(f.cars.size(), kMaxCarsPerFrame));
    payload.pod(car_n);
    for (uint32_t ci = 0; ci < car_n; ++ci) payload.pod(f.cars[ci]);
  }

  const uint32_t crc = crc32_bytes(payload.buf.data(), payload.buf.size());
  const uint32_t payload_bytes = uint32_t(payload.buf.size());

  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "cannot open " + path.string();
      return false;
    }
    out.write(kReplayMagic, sizeof kReplayMagic);
    out.write(reinterpret_cast<const char*>(&kReplayVersion), sizeof kReplayVersion);
    out.write(reinterpret_cast<const char*>(&payload_bytes), sizeof payload_bytes);
    out.write(reinterpret_cast<const char*>(&crc), sizeof crc);
    out.write(reinterpret_cast<const char*>(payload.buf.data()), std::streamsize(payload.buf.size()));
    if (!out) {
      if (error) *error = "write failed for " + path.string();
      return false;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
      if (error) *error = "rename failed for " + path.string();
      return false;
    }
  }
  return true;
}

bool load_replay_file(const std::filesystem::path& path, ReplayRecording& out, std::string* error) {
  out = ReplayRecording{};
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  const std::streamoff total = in.tellg();
  if (total < 20) {
    if (error) *error = "replay file too short";
    return false;
  }
  in.seekg(0, std::ios::beg);
  char magic[8] = {};
  uint32_t version = 0, payload_bytes = 0, expected_crc = 0;
  in.read(magic, 8);
  in.read(reinterpret_cast<char*>(&version), 4);
  in.read(reinterpret_cast<char*>(&payload_bytes), 4);
  in.read(reinterpret_cast<char*>(&expected_crc), 4);
  if (!in || std::memcmp(magic, kReplayMagic, 8) != 0) {
    if (error) *error = "invalid replay file magic";
    return false;
  }
  if (version < 1 || version > kReplayVersion) {
    if (error) *error = "unsupported replay version " + std::to_string(version);
    return false;
  }
  if (payload_bytes == 0 || std::streamoff(20) + std::streamoff(payload_bytes) != total) {
    if (error) *error = "truncated or oversized replay payload";
    return false;
  }
  std::vector<uint8_t> buf(payload_bytes);
  in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(payload_bytes));
  if (!in) {
    if (error) *error = "failed reading replay payload";
    return false;
  }
  if (crc32_bytes(buf.data(), buf.size()) != expected_crc) {
    if (error) *error = "replay CRC32 mismatch";
    return false;
  }

  BinReader r{buf.data(), buf.size(), 0};
  uint8_t is_race = 0;
  int32_t place = 0, field = 1;
  if (!r.str(out.meta.event_name) || !r.str(out.meta.world_name) || !r.pod(out.meta.tod_sec) ||
      !r.pod(out.meta.recorded_utc) || !r.pod(out.meta.sample_hz) || !r.pod(is_race) || !r.pod(place) ||
      !r.pod(field) || !r.pod(out.meta.best_lap_s)) {
    if (error) *error = "corrupt replay metadata";
    return false;
  }
  out.meta.is_race = is_race != 0;
  out.meta.player_place = place;
  out.meta.field_size = field;

  uint32_t roster_n = 0;
  if (!r.pod(roster_n) || roster_n > kMaxRosterEntries) {
    if (error) *error = "invalid replay roster count";
    return false;
  }
  out.meta.roster.resize(roster_n);
  for (uint32_t i = 0; i < roster_n; ++i) {
    auto& ci = out.meta.roster[i];
    uint8_t flags = 0;
    if (!r.pod(ci.slot) || !r.pod(ci.car_id) || !r.str(ci.car_media) || !r.str(ci.driver_name) || !r.pod(flags)) {
      if (error) *error = "corrupt replay roster entry";
      return false;
    }
    ci.is_player = (flags & 1u) != 0;
    ci.is_traffic = (flags & 2u) != 0;
    ci.has_body_color = (flags & 4u) != 0;
    if (version >= 2) {
      if (!r.pod(ci.paint_index) || !r.pod(ci.body_color) || !r.pod(ci.rim_id) || !r.str(ci.livery_id)) {
        if (error) *error = "corrupt replay roster appearance metadata";
        return false;
      }
    }
  }

  uint32_t marker_n = 0;
  if (!r.pod(marker_n) || marker_n > kMaxMarkers) {
    if (error) *error = "invalid replay marker count";
    return false;
  }
  out.meta.markers.resize(marker_n);
  for (uint32_t i = 0; i < marker_n; ++i) {
    if (!r.pod(out.meta.markers[i].time_s) || !r.str(out.meta.markers[i].label)) {
      if (error) *error = "corrupt replay marker";
      return false;
    }
  }

  uint32_t frame_n = 0;
  if (!r.pod(frame_n) || frame_n == 0 || frame_n > kMaxFramesFile) {
    if (error) *error = "invalid replay frame count";
    return false;
  }
  out.frames.resize(frame_n);
  for (uint32_t fi = 0; fi < frame_n; ++fi) {
    uint32_t car_n = 0;
    if (!r.pod(out.frames[fi].time_s) || !r.pod(car_n) || car_n > kMaxCarsPerFrame) {
      if (error) *error = "corrupt replay frame";
      return false;
    }
    out.frames[fi].cars.resize(car_n);
    for (uint32_t ci = 0; ci < car_n; ++ci) {
      if (version <= 2) {
        ReplayCarSampleLegacyV2 legacy{};
        if (!r.pod(legacy)) {
          if (error) *error = "corrupt replay car sample";
          return false;
        }
        auto& dst = out.frames[fi].cars[ci];
        dst = ReplayCarSample{};
        dst.slot = legacy.slot;
        std::memcpy(dst.pos, legacy.pos, sizeof(dst.pos));
        std::memcpy(dst.quat, legacy.quat, sizeof(dst.quat));
        std::memcpy(dst.vel, legacy.vel, sizeof(dst.vel));
        dst.speed_mps = legacy.speed_mps;
        dst.rpm = legacy.rpm;
        dst.redline_rpm = legacy.redline_rpm;
        dst.gear = legacy.gear;
        dst.throttle_u8 = legacy.throttle_u8;
        dst.brake_u8 = legacy.brake_u8;
        dst.handbrake_u8 = legacy.handbrake_u8;
        dst.steer_i8 = legacy.steer_i8;
        dst.race_place = legacy.race_place;
        dst.lamp_bits = legacy.lamp_bits;
        std::memcpy(dst.wheel_steer, legacy.wheel_steer, sizeof(dst.wheel_steer));
        std::memcpy(dst.wheel_spin, legacy.wheel_spin, sizeof(dst.wheel_spin));
        std::memcpy(dst.wheel_omega, legacy.wheel_omega, sizeof(dst.wheel_omega));
        std::memcpy(dst.wheel_y, legacy.wheel_y, sizeof(dst.wheel_y));
      } else if (!r.pod(out.frames[fi].cars[ci])) {
        if (error) *error = "corrupt replay car sample";
        return false;
      }
    }
  }
  return true;
}

bool save_replay_auto(const std::filesystem::path& dir, const ReplayRecording& rec, std::filesystem::path& out_path,
                      std::string* error) {
  std::error_code ec;
  if (!dir.empty()) std::filesystem::create_directories(dir, ec);
  const std::string tag = timestamp_tag();
  std::filesystem::path candidate = dir / ("replay-" + tag + ".shrep");
  for (int seq = 1; std::filesystem::exists(candidate, ec) && seq < 999; ++seq) {
    char suffix[20];
    std::snprintf(suffix, sizeof suffix, "-%02d.shrep", seq);
    candidate = dir / ("replay-" + tag + suffix);
  }
  if (!save_replay_file(candidate, rec, error)) return false;
  out_path = candidate;
  return true;
}

std::vector<ReplayFileEntry> list_replay_files(const std::filesystem::path& dir) {
  std::vector<ReplayFileEntry> out;
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return out;
  for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
    if (!ent.is_regular_file(ec) || ent.path().extension() != ".shrep") continue;
    ReplayRecording rec;
    if (!load_replay_file(ent.path(), rec, nullptr)) continue;
    ReplayFileEntry fe;
    fe.path = ent.path();
    fe.filename = ent.path().filename().string();
    fe.event_name = rec.meta.event_name;
    fe.duration_s = rec.duration();
    fe.recorded_utc = rec.meta.recorded_utc;
    fe.is_race = rec.meta.is_race;
    if (const auto* pc = rec.find_car_info(0)) fe.player_car = pc->car_media;
    out.push_back(std::move(fe));
  }
  std::sort(out.begin(), out.end(),
            [](const ReplayFileEntry& a, const ReplayFileEntry& b) { return a.recorded_utc > b.recorded_utc; });
  return out;
}

}  // namespace drive
