#include "audio/mixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace audio {

namespace {
const char* const kBusNames[kBuses] = {"Engine", "OtherCars", "Tyres", "Wind", "Ambience", "Music", "UI", "VO"};

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}
}  // namespace

const char* bus_name(Bus b) { return kBusNames[std::clamp(int(b), 0, kBuses - 1)]; }

bool bus_from_name(std::string_view name, Bus& out) {
  for (int i = 0; i < kBuses; ++i)
    if (ieq(name, kBusNames[i])) {
      out = Bus(i);
      return true;
    }
  return false;
}

// ---------------------------------------------------------------- snapshots

int SnapshotMixer::add(Snapshot s) {
  snaps_.push_back({std::move(s), false, 0.0f});
  return int(snaps_.size()) - 1;
}

int SnapshotMixer::find(std::string_view name) const {
  for (size_t i = 0; i < snaps_.size(); ++i)
    if (ieq(snaps_[i].snap.name, name)) return int(i);
  return -1;
}

void SnapshotMixer::set_active(int id, bool on) {
  if (id >= 0 && size_t(id) < snaps_.size()) snaps_[size_t(id)].on = on;
}

bool SnapshotMixer::active(int id) const { return id >= 0 && size_t(id) < snaps_.size() && snaps_[size_t(id)].on; }

void SnapshotMixer::advance(double dt) {
  for (State& s : snaps_) {
    const float target = s.on ? 1.0f : 0.0f;
    if (s.weight == target) continue;
    const float time = std::max(0.001f, s.on ? s.snap.fade : s.snap.return_fade);
    const float step = float(dt) / time;
    s.weight = target > s.weight ? std::min(target, s.weight + step) : std::max(target, s.weight - step);
  }
}

float SnapshotMixer::level(Bus b) const {
  float v = 1;
  for (const State& s : snaps_) {
    if (s.weight <= 0) continue;
    v *= 1.0f + (s.snap.dest[size_t(b)] - 1.0f) * s.weight;
  }
  return v;
}

void SnapshotMixer::add_defaults() {
  auto make = [&](const char* name, float world, float music, float vo, float fade, float back) {
    Snapshot s;
    s.name = name;
    for (Bus b : {Bus::kEngine, Bus::kOtherCars, Bus::kTyres, Bus::kWind, Bus::kAmbience}) s.dest[size_t(b)] = world;
    s.dest[size_t(Bus::kMusic)] = music;
    s.dest[size_t(Bus::kVo)] = vo;
    s.fade = fade;
    s.return_fade = back;
    add(std::move(s));
  };
  //            world music vo   fade  back
  make("Paused", 0.0f, 0.25f, 0.5f, 0.15f, 0.3f);   // the pause fix: the world drops out, the music dips
  make("Menu", 0.0f, 0.0f, 0.5f, 0.1f, 0.3f);       // a menu or page over the drive
  make("Title", 0.0f, 0.0f, 1.0f, 0.1f, 0.5f);      // the title screen
  make("Loading", 0.0f, 0.25f, 1.0f, 0.1f, 0.5f);   // spec: loading lowers the music
}

// ------------------------------------------------------------------- master

float soft_limit(float x) {
  const float a = std::fabs(x);
  if (a <= 0.9f) return x;
  const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f);
  return x < 0 ? -y : y;
}

void MasterStage::prepare(int rate) {
  rate_ = std::max(8000, rate);
  set_settings(s_);
  gr_db_ = 0;
}

void MasterStage::set_settings(const CompressorSettings& s) {
  s_ = s;
  attack_ = 1.0f - std::exp(-1.0f / (std::max(0.1f, s.attack_ms) * 0.001f * float(rate_)));
  release_ = 1.0f - std::exp(-1.0f / (std::max(1.0f, s.release_ms) * 0.001f * float(rate_)));
}

void MasterStage::process(float* io, int frames) {
  const float makeup = std::pow(10.0f, s_.makeup_db / 20.0f);
  const float slope = 1.0f - 1.0f / std::max(1.0f, s_.ratio);
  float peak = 0;
  for (int i = 0; i < frames; ++i) {
    float& l = io[2 * i];
    float& r = io[2 * i + 1];
    float target = 0;
    if (!full_range_) {
      const float level = std::max(std::fabs(l), std::fabs(r));
      if (level > 1e-5f) {
        const float over = 20.0f * std::log10(level) - s_.threshold_db;
        if (2 * over > s_.knee_db) target = slope * over;
        else if (2 * over > -s_.knee_db) {
          const float t = over + s_.knee_db * 0.5f;
          target = slope * t * t / (2 * std::max(0.01f, s_.knee_db));
        }
      }
    }
    gr_db_ += (target - gr_db_) * (target > gr_db_ ? attack_ : release_);
    const float g = std::pow(10.0f, -gr_db_ / 20.0f) * (full_range_ ? 1.0f : makeup);
    l = soft_limit(l * g);
    r = soft_limit(r * g);
    peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
  }
  peak_ = peak;
}

// -------------------------------------------------------------------- mixer

void Mixer::prepare(int rate) {
  rate_ = std::max(8000, rate);
  stage_.prepare(rate_);
}

void Mixer::mix(const float* const* bus, int frames, float* out) {
  const size_t n = 2 * size_t(std::max(0, frames));
  std::fill(out, out + n, 0.0f);
  const float a = 1.0f - std::exp(-1.0f / (0.01f * float(rate_)));
  for (int b = 0; b < kBuses; ++b) {
    const float target = user_[size_t(b)] * snaps_.level(Bus(b));
    float& s = smooth_[size_t(b)];
    const float* in = bus ? bus[b] : nullptr;
    if (!in) {
      s = target;  // nothing to ramp
      continue;
    }
    for (int i = 0; i < frames; ++i) {
      s += (target - s) * a;
      out[2 * i] += in[2 * i] * s;
      out[2 * i + 1] += in[2 * i + 1] * s;
    }
  }
  const float m_target = master_;
  const float mute_target = mute_ ? 0.0f : 1.0f;
  for (int i = 0; i < frames; ++i) {
    master_smooth_ += (m_target - master_smooth_) * a;
    mute_smooth_ += (mute_target - mute_smooth_) * a;
    const float g = master_smooth_ * mute_smooth_;
    out[2 * i] *= g;
    out[2 * i + 1] *= g;
  }
  stage_.process(out, frames);
  snaps_.advance(double(frames) / rate_);
}

}  // namespace audio
