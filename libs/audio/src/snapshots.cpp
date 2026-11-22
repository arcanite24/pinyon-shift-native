#include "audio/snapshots.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "audio/xml.h"
#include "cars/mods.h"

namespace audio {
namespace {

constexpr const char* kCategoryNames[kCategories] = {
    "FocusCar",         "OppCar",         "Transmission",  "SuperCharger",
    "Turbo",            "EngineDamage",   "Backfire",      "Suspension",
    "CarWind",          "Tire",           "Collision",     "AmbientStatic",
    "AmbientDynamic",   "GameMusic",      "HomespaceAmbientDynamic", "HomespaceMusic",
    "MenuMusic",        "GameUI",         "VO",            "SatNav",
    "DJModifier",       "RallyCoDriver",
};

constexpr const char* kGroupNames[kSnapshotGroups] = {
    "Gameplay", "Location", "Speed", "Impact", "SpeedZones", "VO", "Reverb", "Radio", "SatNav",
};

std::string read_text_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

float slew(float cur, float target, float max_step) {
  if (target > cur) return std::min(target, cur + max_step);
  return std::max(target, cur - max_step);
}

bool parse_bool_attr(const XmlNode& n, std::string_view attr, bool fallback) {
  if (!n.has(attr)) return fallback;
  const std::string v = n.attr(attr);
  if (iequals(v, "true") || v == "1") return true;
  if (iequals(v, "false") || v == "0") return false;
  return n.number(attr, fallback ? 1.0 : 0.0) != 0.0;
}

}  // namespace

const char* category_name(Category c) {
  return kCategoryNames[std::clamp(int(c), 0, kCategories - 1)];
}

bool category_from_name(std::string_view name, Category& out) {
  for (int i = 0; i < kCategories; ++i) {
    if (iequals(name, kCategoryNames[i])) {
      out = Category(i);
      return true;
    }
  }
  return false;
}

Bus category_to_bus(Category c) {
  switch (c) {
    case Category::kFocusCar:
    case Category::kTransmission:
    case Category::kSuperCharger:
    case Category::kTurbo:
    case Category::kEngineDamage:
    case Category::kBackfire:
    case Category::kSuspension:
      return Bus::kEngine;
    case Category::kOppCar:
      return Bus::kOtherCars;
    case Category::kTire:
      return Bus::kTyres;
    case Category::kCarWind:
      return Bus::kWind;
    case Category::kCollision:
    case Category::kAmbientStatic:
    case Category::kAmbientDynamic:
    case Category::kHomespaceAmbientDynamic:
      return Bus::kAmbience;
    case Category::kGameMusic:
    case Category::kHomespaceMusic:
    case Category::kMenuMusic:
      return Bus::kMusic;
    case Category::kGameUI:
      return Bus::kUi;
    case Category::kVO:
    case Category::kSatNav:
    case Category::kDJModifier:
    case Category::kRallyCoDriver:
      return Bus::kVo;
  }
  return Bus::kAmbience;
}

const char* snapshot_group_name(SnapshotGroup g) {
  return kGroupNames[std::clamp(int(g), 0, kSnapshotGroups - 1)];
}

bool snapshot_group_from_name(std::string_view name, SnapshotGroup& out) {
  for (int i = 0; i < kSnapshotGroups; ++i) {
    if (iequals(name, kGroupNames[i])) {
      out = SnapshotGroup(i);
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------ SnapshotMatrix

SnapshotMatrix::SnapshotMatrix() { active_idx_.fill(-1); }

bool SnapshotMatrix::is_transient_gameplay(std::string_view name) {
  return iequals(name, "Paused") || iequals(name, "PausedMultiplayer") || iequals(name, "Loading") ||
         iequals(name, "Rewind");
}

bool SnapshotMatrix::parse(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  defs_.clear();
  dynamics_ = DynamicsData{};
  category_count_ = 21;
  for (auto& row : cells_)
    for (auto& c : row) c = Cell{};
  active_idx_.fill(-1);
  saved_gameplay_idx_ = -1;
  reset_dynamics();

  bool saw_rally = false;
  for (const auto& child : root.children) {
    if (iequals(child.name, "DynamicsData")) {
      if (const XmlNode* cd = child.child("CrashData")) {
        dynamics_.crash_threshold_mph = float(cd->number("threshold", dynamics_.crash_threshold_mph));
        dynamics_.crash_drop_mph = float(cd->number("drop", dynamics_.crash_drop_mph));
      }
      if (const XmlNode* ts = child.child("TopSpeedData")) {
        dynamics_.top_speed_threshold = float(ts->number("threshold", dynamics_.top_speed_threshold));
      }
      if (const XmlNode* mh = child.child("MusicHighpass")) {
        dynamics_.music_hp_fadein = float(mh->number("fadeinrate", dynamics_.music_hp_fadein));
        dynamics_.music_hp_fadeout = float(mh->number("fadeoutrate", dynamics_.music_hp_fadeout));
        dynamics_.music_hp_cutoff = float(mh->number("zonefilterlevel", dynamics_.music_hp_cutoff));
      }
      continue;
    }
    SnapshotGroup g{};
    if (!child.has("group") || !snapshot_group_from_name(child.attr("group"), g)) continue;
    MixSnapshotDef def;
    def.name = child.name;
    def.group = g;
    for (const auto& cat_node : child.children) {
      Category c{};
      if (!category_from_name(cat_node.name, c)) continue;
      if (c == Category::kRallyCoDriver) saw_rally = true;
      SnapshotEntry& e = def.categories[size_t(c)];
      e.active = cat_node.number("active", 0.0) != 0.0;
      e.dest_volume = float(cat_node.number("destvolume", 1.0));
      e.fade_time = float(std::max(0.0, cat_node.number("fadetime", 0.0)));
      e.return_time = float(std::max(0.0, cat_node.number("returntime", 0.0)));
      e.return_fade_time = float(std::max(0.0, cat_node.number("returnfadetime", 0.0)));
    }
    defs_.push_back(std::move(def));
  }
  category_count_ = saw_rally ? 22 : 21;
  if (defs_.empty()) {
    if (error) *error = "AudioMixerSnapshots.xml has no snapshots";
    return false;
  }
  // Initialize default group states on startup (SPEC-AUDIO-EVENTS Part 3).
  auto init_snap = [&](std::string_view name) {
    const int idx = find(name);
    if (idx < 0) return;
    const MixSnapshotDef& d = defs_[size_t(idx)];
    const size_t gi = size_t(d.group);
    active_idx_[gi] = idx;
    if (d.group == SnapshotGroup::kGameplay && !is_transient_gameplay(d.name)) saved_gameplay_idx_ = idx;
    for (size_t c = 0; c < kCategories; ++c) {
      if (!d.categories[c].active) continue;
      cells_[gi][c] = Cell{};
      cells_[gi][c].current = d.categories[c].dest_volume;
      cells_[gi][c].target = d.categories[c].dest_volume;
      cells_[gi][c].start = d.categories[c].dest_volume;
    }
  };
  init_snap("FreeRoam");
  init_snap("LowSpeed");
  init_snap("AverageSpeedExit");
  init_snap("VONotPlaying");
  init_snap("NotInTunnel");
  init_snap("RadioNormal");
  init_snap("SatNavNotPlaying");
  return true;
}

bool SnapshotMatrix::load(const std::filesystem::path& game_root, std::string* error) {
  const auto path = cars::mods::resolve(game_root / "media" / "audio" / "AudioMixerSnapshots.xml");
  const std::string text = read_text_file(path);
  if (text.empty()) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  return parse(text, error);
}

int SnapshotMatrix::find(std::string_view name) const {
  for (size_t i = 0; i < defs_.size(); ++i)
    if (iequals(defs_[i].name, name)) return int(i);
  return -1;
}

std::string_view SnapshotMatrix::active_name(SnapshotGroup g) const {
  const int idx = active_idx_[size_t(g)];
  if (idx < 0 || size_t(idx) >= defs_.size()) return {};
  return defs_[size_t(idx)].name;
}

bool SnapshotMatrix::activate(std::string_view name) {
  const std::string_view gameplay = active_name(SnapshotGroup::kGameplay);
  const std::string_view vo = active_name(SnapshotGroup::kVO);
  const std::string_view radio = active_name(SnapshotGroup::kRadio);

  // Override rule 1: VOPlaying while Gameplay is UIExterior or Paused -> VOPlayingOnUIExterior.
  if (iequals(name, "VOPlaying") && (iequals(gameplay, "UIExterior") || iequals(gameplay, "Paused"))) {
    name = "VOPlayingOnUIExterior";
  }
  // Override rule 2: SatNavPlaying ignored while Radio is RadioDJSpeaking or RadioFestivalUpdate.
  if (iequals(name, "SatNavPlaying") &&
      (iequals(radio, "RadioDJSpeaking") || iequals(radio, "RadioFestivalUpdate"))) {
    return false;
  }
  // Override rule 3: while Barnfind or VOPlayingInstant is active, ordinary VOPlaying/VONotPlaying are blocked.
  if ((iequals(name, "VOPlaying") || iequals(name, "VONotPlaying")) &&
      (iequals(gameplay, "Barnfind") || iequals(vo, "VOPlayingInstant"))) {
    return false;
  }

  const int idx = find(name);
  if (idx < 0) return false;
  apply_def(idx);
  return true;
}

void SnapshotMatrix::apply_def(int idx) {
  const MixSnapshotDef& d = defs_[size_t(idx)];
  const size_t gi = size_t(d.group);
  const int prev_idx = active_idx_[gi];
  const MixSnapshotDef* prev = (prev_idx >= 0 && size_t(prev_idx) < defs_.size()) ? &defs_[size_t(prev_idx)] : nullptr;

  if (d.group == SnapshotGroup::kGameplay && !is_transient_gameplay(d.name)) saved_gameplay_idx_ = idx;
  active_idx_[gi] = idx;

  for (size_t c = 0; c < kCategories; ++c) {
    const SnapshotEntry& e = d.categories[c];
    if (!e.active) continue;  // active = 0 leaves cell (g, c) untouched
    Cell& cell = cells_[gi][c];
    float fade = e.fade_time;
    if (fade <= 0.0f && prev && prev->categories[c].return_fade_time > 0.0f) {
      fade = prev->categories[c].return_fade_time;
    }
    cell.hold_time = e.return_time;
    cell.return_fade = e.return_fade_time;
    if (fade <= 0.0f) {
      cell.current = e.dest_volume;
      cell.start = e.dest_volume;
      cell.target = e.dest_volume;
      cell.elapsed = 0.0f;
      cell.duration = 0.0f;
      cell.phase = e.return_time > 0.0f ? Phase::kHold : Phase::kIdle;
    } else {
      cell.start = cell.current;
      cell.target = e.dest_volume;
      cell.elapsed = 0.0f;
      cell.duration = fade;
      cell.phase = Phase::kFadeIn;
    }
  }
}

void SnapshotMatrix::restore_gameplay() {
  if (saved_gameplay_idx_ >= 0 && size_t(saved_gameplay_idx_) < defs_.size()) apply_def(saved_gameplay_idx_);
}

void SnapshotMatrix::reset_dynamics() {
  have_prev_speed_ = false;
  prev_speed_mph_ = 0.0f;
  crash_latched_ = false;
  crash_saw_slow_ = false;
  in_top_speed_ = false;
}

void SnapshotMatrix::update_dynamics(double /*dt*/, float focus_speed_mph, float car_top_speed_mph) {
  if (car_top_speed_mph <= 1.0f) car_top_speed_mph = 140.0f;
  // CrashData trigger & latch.
  if (crash_latched_) {
    if (focus_speed_mph <= 5.0f) crash_saw_slow_ = true;
    else if (crash_saw_slow_ && focus_speed_mph > 5.0f) {
      crash_latched_ = false;
      crash_saw_slow_ = false;
    }
  }
  if (have_prev_speed_ && !crash_latched_ && prev_speed_mph_ > dynamics_.crash_threshold_mph &&
      (prev_speed_mph_ - focus_speed_mph) > dynamics_.crash_drop_mph) {
    activate("Crash");
    crash_latched_ = true;
    crash_saw_slow_ = (focus_speed_mph <= 5.0f);
  }
  prev_speed_mph_ = focus_speed_mph;
  have_prev_speed_ = true;

  // TopSpeedData trigger.
  const bool want_top = focus_speed_mph > dynamics_.top_speed_threshold * car_top_speed_mph;
  if (want_top != in_top_speed_) {
    in_top_speed_ = want_top;
    activate(want_top ? "TopSpeed" : "LowSpeed");
  }
}

void SnapshotMatrix::advance(double dt) {
  if (dt <= 0.0) return;
  const float step = float(dt);
  for (auto& row : cells_) {
    for (Cell& cell : row) {
      float rem = step;
      while (rem > 0.0f && cell.phase != Phase::kIdle) {
        if (cell.phase == Phase::kFadeIn) {
          const float left = std::max(0.0f, cell.duration - cell.elapsed);
          const float take = std::min(rem, left);
          cell.elapsed += take;
          rem -= take;
          const float t = cell.duration > 0.0f ? std::clamp(cell.elapsed / cell.duration, 0.0f, 1.0f) : 1.0f;
          cell.current = cell.start + (cell.target - cell.start) * t;
          if (cell.elapsed >= cell.duration - 1e-6f) {
            cell.current = cell.target;
            cell.elapsed = 0.0f;
            cell.phase = cell.hold_time > 0.0f ? Phase::kHold : Phase::kIdle;
          }
        } else if (cell.phase == Phase::kHold) {
          const float left = std::max(0.0f, cell.hold_time - cell.elapsed);
          const float take = std::min(rem, left);
          cell.elapsed += take;
          rem -= take;
          if (cell.elapsed >= cell.hold_time - 1e-6f) {
            cell.elapsed = 0.0f;
            if (cell.return_fade > 0.0f) {
              cell.start = cell.current;
              cell.target = 1.0f;
              cell.duration = cell.return_fade;
              cell.phase = Phase::kReturnFade;
            } else {
              cell.current = 1.0f;
              cell.phase = Phase::kIdle;
            }
          }
        } else if (cell.phase == Phase::kReturnFade) {
          const float left = std::max(0.0f, cell.duration - cell.elapsed);
          const float take = std::min(rem, left);
          cell.elapsed += take;
          rem -= take;
          const float t = cell.duration > 0.0f ? std::clamp(cell.elapsed / cell.duration, 0.0f, 1.0f) : 1.0f;
          cell.current = cell.start + (cell.target - cell.start) * t;
          if (cell.elapsed >= cell.duration - 1e-6f) {
            cell.current = 1.0f;
            cell.phase = Phase::kIdle;
          }
        }
      }
    }
  }
}

float SnapshotMatrix::gain(Category c) const {
  const size_t ci = size_t(std::clamp(int(c), 0, kCategories - 1));
  float prod = 1.0f;
  for (size_t g = 0; g < kSnapshotGroups; ++g) prod *= cells_[g][ci].current;
  return std::clamp(prod, 0.0f, 1.0f);
}

float SnapshotMatrix::bus_gain(Bus b) const {
  switch (b) {
    case Bus::kEngine: return gain(Category::kFocusCar);
    case Bus::kOtherCars: return gain(Category::kOppCar);
    case Bus::kTyres: return gain(Category::kTire);
    case Bus::kWind: return gain(Category::kCarWind);
    case Bus::kAmbience: return gain(Category::kAmbientStatic);
    case Bus::kMusic: return gain(Category::kGameMusic);
    case Bus::kUi: return gain(Category::kGameUI);
    case Bus::kVo: return gain(Category::kVO);
  }
  return 1.0f;
}

// ------------------------------------------------------------ GlobalDspConfig

bool GlobalDspConfig::parse(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  if (const XmlNode* r = root.child("RewindDSPConfig")) {
    rewind.bypass = parse_bool_attr(*r, "Bypass", true);
    rewind.gain = float(r->number("Gain", 1.0));
    rewind.bandwidth = float(r->number("Bandwidth", 1.0));
    rewind.center = float(r->number("Center", 1000.0));
  }
  if (const XmlNode* c = root.child("ThreeTwoOneDSPConfig")) {
    countdown.bypass = parse_bool_attr(*c, "Bypass", true);
    countdown.gain = float(c->number("Gain", 1.0));
    countdown.bandwidth = float(c->number("Bandwidth", 1.0));
    countdown.center = float(c->number("Center", 1000.0));
    countdown_start_s = float(c->number("FilterStartTime", 6.5));
    countdown_end_s = float(c->number("FilterEndTime", 9.5));
  }
  if (const XmlNode* k = root.child("CockpitDSPConfig")) {
    for (int i = 0; i < 4; ++i) {
      const std::string sfx = i == 0 ? std::string{} : std::to_string(i);
      cockpit[size_t(i)].bypass = parse_bool_attr(*k, "Bypass" + sfx, true);
      cockpit[size_t(i)].gain = float(k->number("Gain" + sfx, 1.0));
      cockpit[size_t(i)].bandwidth = float(k->number("Bandwidth" + sfx, 1.0));
      cockpit[size_t(i)].center = float(k->number("Center" + sfx, 1000.0));
    }
  }
  return true;
}

bool GlobalDspConfig::load(const std::filesystem::path& game_root, std::string* error) {
  const auto path = cars::mods::resolve(game_root / "media" / "audio" / "GlobalDSPConfig.xml");
  const std::string text = read_text_file(path);
  if (text.empty()) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  return parse(text, error);
}

// ------------------------------------------------------------------ CarBusEq

void CarBusEq::prepare(int rate) {
  rate_ = std::max(8000, rate);
  z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0f;
  update_coeffs();
}

void CarBusEq::set_target(const GlobalDspBand& band) {
  bypass_ = band.bypass;
  target_center_ = std::clamp(band.center, 20.0f, 0.45f * float(rate_));
  target_bw_ = std::clamp(band.bandwidth, 0.1f, 6.0f);
  target_gain_ = band.bypass ? 1.0f : std::clamp(band.gain, 0.05f, 3.0f);
}

void CarBusEq::update_coeffs() {
  const double fc = std::clamp(double(center_), 20.0, 0.45 * double(rate_));
  const double bw = std::clamp(double(bandwidth_), 0.1, 6.0);
  const double A = std::sqrt(std::clamp(double(gain_), 0.05, 3.0));
  const double w0 = 2.0 * 3.14159265358979323846 * fc / double(rate_);
  const double sin_w0 = std::sin(w0);
  const double cos_w0 = std::cos(w0);
  const double alpha =
      sin_w0 > 1e-6 ? sin_w0 * std::sinh((0.6931471805599453 * 0.5) * bw * (w0 / sin_w0)) : 1e-6;
  const double a0 = 1.0 + alpha / A;
  b0_ = float((1.0 + alpha * A) / a0);
  b1_ = float((-2.0 * cos_w0) / a0);
  b2_ = float((1.0 - alpha * A) / a0);
  a1_ = float((-2.0 * cos_w0) / a0);
  a2_ = float((1.0 - alpha / A) / a0);
}

void CarBusEq::process(float* stereo, int frames) {
  if (frames <= 0) return;
  const float dt = float(frames) / float(rate_);
  const bool moving = std::fabs(center_ - target_center_) > 1e-2f || std::fabs(bandwidth_ - target_bw_) > 1e-4f ||
                      std::fabs(gain_ - target_gain_) > 1e-4f;
  if (moving) {
    center_ = slew(center_, target_center_, 44000.0f * dt);
    bandwidth_ = slew(bandwidth_, target_bw_, 10.0f * dt);
    gain_ = slew(gain_, target_gain_, 6.0f * dt);
    update_coeffs();
  }
  if (bypass_ && std::fabs(gain_ - 1.0f) < 1e-4f) return;
  for (int i = 0; i < frames; ++i) {
    for (int c = 0; c < 2; ++c) {
      const float x = stereo[2 * i + c];
      const float y = b0_ * x + z1_[c];
      z1_[c] = b1_ * x - a1_ * y + z2_[c];
      z2_[c] = b2_ * x - a2_ * y;
      stereo[2 * i + c] = y;
    }
  }
}

// ----------------------------------------------------------- MasterDspConfig

bool MasterDspConfig::parse(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  compressor_bypass = parse_bool_attr(root, "compressorBypass", compressor_bypass);
  compressor_level = float(root.number("compressorLevel", compressor_level));
  compressor_threshold = float(root.number("compressorThreshold", compressor_threshold));
  compressor_attack = float(root.number("compressorAttack", compressor_attack));
  compressor_release = float(root.number("compressorRelease", compressor_release));
  compressor_ratio = float(root.number("compressorRatio", compressor_ratio));
  compressor_gain_makeup = float(root.number("compressorGainMakeup", compressor_gain_makeup));

  compressor_bypass_tv = parse_bool_attr(root, "compressorBypassTV", compressor_bypass_tv);
  compressor_level_tv = float(root.number("compressorLevelTV", compressor_level_tv));
  compressor_threshold_tv = float(root.number("compressorThresholdTV", compressor_threshold_tv));
  compressor_attack_tv = float(root.number("compressorAttackTV", compressor_attack_tv));
  compressor_release_tv = float(root.number("compressorReleaseTV", compressor_release_tv));
  compressor_ratio_tv = float(root.number("compressorRatioTV", compressor_ratio_tv));
  compressor_gain_makeup_tv = float(root.number("compressorGainMakeupTV", compressor_gain_makeup_tv));

  saturator_bypass = parse_bool_attr(root, "saturatorBypass", saturator_bypass);
  saturator_level = float(root.number("saturatorLevel", saturator_level));
  saturator_saturation = float(root.number("saturatorSaturation", saturator_saturation));
  saturator_gain = float(root.number("saturatorGain", saturator_gain));
  return true;
}

bool MasterDspConfig::load(const std::filesystem::path& game_root, std::string* error) {
  const auto path = cars::mods::resolve(game_root / "media" / "audio" / "AudioEngineConfig.xml");
  const std::string text = read_text_file(path);
  if (text.empty()) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  return parse(text, error);
}

// ------------------------------------------------------------ MasterRmsStage

float master_saturate(float x, float level, float saturation, float gain) {
  const float t = 1.0f - saturation;
  const float a = std::fabs(x) * level;
  const float shaped = (a <= t) ? a : a / (1.0f + a - t);
  const float sgn = x < 0.0f ? -1.0f : 1.0f;
  return sgn * gain * shaped;
}

void MasterRmsStage::prepare(int rate) {
  rate_ = std::max(8000, rate);
  rms_ring_.fill(0.0f);
  rms_pos_ = 0;
  prev_gain_ = 1.0f;
  target_gain_ = 1.0f;
  peak_ = 0.0f;
}

void MasterRmsStage::set_config(const MasterDspConfig& cfg) { cfg_ = cfg; }

float MasterRmsStage::gain_reduction_db() const {
  const float g = std::clamp(target_gain_, 1e-5f, 1.0f);
  return -20.0f * std::log10(g);
}

void MasterRmsStage::process(float* io, int frames, int channels) {
  if (frames <= 0 || channels <= 0) return;
  peak_ = 0.0f;
  int offset = 0;
  while (offset < frames) {
    const int n = std::min(frames - offset, kBlockSize);
    process_block(io + size_t(offset) * size_t(channels), n, channels);
    offset += n;
  }
}

void MasterRmsStage::process_block(float* io, int n, int channels) {
  const bool comp_bypass = full_range_ || (tv_profile_ ? cfg_.compressor_bypass_tv : cfg_.compressor_bypass);
  const float level = tv_profile_ ? cfg_.compressor_level_tv : cfg_.compressor_level;
  const float threshold = std::max(1e-5f, tv_profile_ ? cfg_.compressor_threshold_tv : cfg_.compressor_threshold);
  const float attack = std::max(1e-4f, tv_profile_ ? cfg_.compressor_attack_tv : cfg_.compressor_attack);
  const float release = std::max(1e-4f, tv_profile_ ? cfg_.compressor_release_tv : cfg_.compressor_release);
  const float ratio = std::clamp(tv_profile_ ? cfg_.compressor_ratio_tv : cfg_.compressor_ratio, 0.0f, 1.0f);
  const float makeup = tv_profile_ ? cfg_.compressor_gain_makeup_tv : cfg_.compressor_gain_makeup;

  double sum_sq = 0.0;
  int count = 0;
  const float g0 = comp_bypass ? 1.0f : prev_gain_;
  const float g1 = comp_bypass ? 1.0f : target_gain_;
  const float dg = n > 0 ? (g1 - g0) / float(n) : 0.0f;

  for (int i = 0; i < n; ++i) {
    const float g = g0 + dg * float(i);
    for (int c = 0; c < channels; ++c) {
      float& x = io[size_t(i) * size_t(channels) + size_t(c)];
      const bool is_lfe = (channels == 6 && c == 3);
      if (!is_lfe) {
        const float scaled = x * level;
        sum_sq += double(scaled) * double(scaled);
        ++count;
        if (!comp_bypass) x = x * g * level * makeup;
      }
      if (!cfg_.saturator_bypass) {
        x = master_saturate(x, cfg_.saturator_level, cfg_.saturator_saturation, cfg_.saturator_gain);
      } else {
        x = soft_limit(x);
      }
      peak_ = std::max(peak_, std::fabs(x));
    }
  }

  // Update 12-block sliding RMS window and smoothed gain target.
  const float block_msq = count > 0 ? float(sum_sq / double(count)) : 0.0f;
  rms_ring_[size_t(rms_pos_)] = block_msq;
  rms_pos_ = (rms_pos_ + 1) % kRmsBlocks;
  double mean_sq = 0.0;
  for (float v : rms_ring_) mean_sq += double(v);
  const float rms = float(std::sqrt(mean_sq / double(kRmsBlocks)));
  const float desired =
      (!comp_bypass && rms > threshold) ? (threshold + (rms - threshold) * ratio) / rms : 1.0f;
  const float tc = desired < target_gain_ ? attack : release;
  const float alpha = float(std::exp(-double(n) / (double(rate_) * double(tc))));
  prev_gain_ = target_gain_;
  target_gain_ = alpha * target_gain_ + (1.0f - alpha) * desired;
}

}  // namespace audio
