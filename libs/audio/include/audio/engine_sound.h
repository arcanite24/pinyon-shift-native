// A car's engine sound: its loop sets and parameters, read from the engine,
// harmonic and model tunings, and the rules that pick, fade and pitch the
// loops for an engine rpm.
// Spec: SPEC-ENGINE-SOUND (horizon1-recomp docs/specs/ENGINE_SOUND.md).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "audio/xml.h"

namespace audio {

// The inputs a parameter weighs (each clamped to 0..1 by the curve).
struct SoundInputs {
  double n = 0;           // normalised rpm (unclamped; spec "Rpm map")
  double throttle = 0;    // the pedal after input shaping
  double pos_torque = 0;  // the engine's load while it drives
  double neg_torque = 0;  // its load while it brakes (a magnitude)
};

// A PhysicsCoeff and its ThreePointCurve (spec "Parameter curves").
struct Param {
  bool present = false;
  double rpm = 0, throttle = 0, pos_torque = 0, neg_torque = 0;
  std::array<double, 3> x{0, 0.5, 1}, y{1, 1, 1};
  // `neg_raises`: confirmed true (braking raises x whether `in.neg_torque`
  // is passed as signed [-1, 0] or positive magnitude [0, 1]).
  double eval(const SoundInputs& in, bool neg_raises = true) const;
};

struct PeqConfig {
  bool active = false;
  Param gain;
  Param center_freq;
  Param bandwidth;
};

struct LoadPeqConfig {
  bool active = false;
  PeqConfig pos_load;
  PeqConfig neg_load;
};

struct ExpanderConfig {
  bool present = false;
  Param max_gain;
  double attack_ms = 0;
  double hold_ms = 0;
  double release_ms = 0;
};

struct BurbleConfig {
  std::string wavebank;
  int burbles = 0, backfires = 0;
  Param volume;
  double min_time_ms = 0, max_time_ms = 0;
  double min_rpm = 0, max_rpm = 0;
  double silence_weighting = 100;
};

struct DrivetrainSpringConfig {
  bool enabled = false;
  double spring_f1 = 8.0, damper_r1 = 0.5;
  double spring_f2 = 16.0, damper_r2 = 0.5;
  double scalar = 1.0;
  double clamp = 400.0;
};

struct DistortionConfig {
  bool active = false;
  Param level;
  double level_scalar = 0.0;
  double volume_compensate = 1.0;
};

struct TrashBandConfig {
  double input_gain = 1.0;
  double overdrive = 0.0;
  double mix = 0.0;
  double output_gain = 1.0;
};

struct TrashDspConfig {
  bool active = false;
  double cutoff1_hz = 300.0;
  double cutoff2_hz = 2500.0;
  std::array<TrashBandConfig, 3> bands{};
};

struct ShiftVolumeScalarConfig {
  bool present = false;
  double boost_up_pct = 0.0;      // ShiftVolBoostUpPct (normalized fractional gain delta)
  double boost_up_time_s = 0.0;   // ShiftVolBoostUpTime (seconds)
  double boost_down_pct = 0.0;    // ShiftVolBoostDownPct (normalized fractional gain delta)
  double boost_down_time_s = 0.0; // ShiftVolBoostDownTime (seconds)
};

struct ViewOtherVolumes {
  double wind = 1.0;
  double tires = 1.0;
  double transmission = 1.0;
  double turbo = 1.0;
  double supercharger = 1.0;
  double backfire = 1.0;
  double burble = 1.0;
  double suspension = 1.0;
  double engine_damage = 1.0;
  double exhaust_noise = 1.0;
  double electric_motor = 1.0;
  double engine_lfe = 1.0;
  double reflections = 1.0;
};

struct Loop {
  int bank_index = 0;  // wavebankindex
  int rpm_min = 0, rpm_sample = 1, rpm_max = 0;
  double volume = 1, pitch = 1, pitch_min = 1, pitch_max = 1;
  // Resolved against the bank (spec "Loop index"): the sample and channel.
  int sample = 0, channel = 0;
};

enum class View { kFollow, kCockpit, kFront, kDefault };
inline constexpr int kViews = 4;

// One emission group's loop set: 0 intake, 1 engine ambient, 2 and 3 the
// exhaust's left and right.
struct Group {
  std::string name;  // EngineIntake, EngineAmbient, ExhaustL, ExhaustR
  std::string bank;  // the .fsb file name
  std::vector<Loop> loops;  // at most 16, in rising rpm
  Param gain;               // the group gain (engine tuning over harmonic tuning)
  bool lowpass = false;
  Param lowpass_cutoff;     // Hz
  Param lowpass_resonance;  // Q
  PeqConfig peq;
  LoadPeqConfig load_peq;
  ExpanderConfig expander;
  double idle_gain_reset = 0;  // audio_rpm_idle_gain_reset (ExhaustL PEQ blend)
  std::array<double, kViews> view_level{1, 1, 1, 1};
  std::array<double, kViews> pan_left{1, 1, 1, 1}, pan_right{1, 1, 1, 1};  // folded 5.1 (FMOD 0.707 matrix)
};

struct EngineSound {
  std::string media;
  double audio_rpm_idle = 800, audio_rpm_redline = 7000;
  Param global_volume;     // GlobalCarVolume
  double engine_volume = 1;  // VolumeAdjustments EngineVolume
  double transmission_volume = 1, shift_volume = 1, supercharger_volume = 1;
  double electric_motor_volume = 1, turbo_volume = 1, suspension_volume = 1;
  double backfire_volume = 1, burble_volume = 0;
  int num_cylinders = 0;
  std::array<int, 3> blowoff_level{1, 2, 3};
  std::array<int, 3> blowoff_choice{1, 1, 1};
  Param intake_chuff_volume;
  std::string intake_chuff_bank;
  int intake_chuff_sample = -1;
  BurbleConfig burble;
  double exhaust_noise_volume = 0;
  DrivetrainSpringConfig drivetrain_spring;
  ShiftVolumeScalarConfig shift_volume_scalar;
  PeqConfig focus_peq;
  DistortionConfig distortion;
  TrashDspConfig trash_dsp;
  // Emission group scope for FocusPEQ / Distortion / TrashDSP (default 2 = ExhaustL/ExhaustR).
  int focus_peq_group = 2;
  int distortion_group = 2;
  int trash_dsp_group = 2;
  std::array<ViewOtherVolumes, kViews> other_volumes{};
  bool has_other_volumes = false;
  std::vector<Group> groups;
};

// The audio rpm for a physics rpm (spec "Rpm map"); `n` receives the
// normalised rpm, unclamped.
double audio_rpm(double rpm, double idle, double redline, double audio_idle, double audio_redline, double* n);

// Each loop's crossfade gain at audio rpm `r`: 0 outside its range, else 1
// or log2(1 + t) on an overlap edge (spec "Loop crossfade and pitch").
std::vector<double> crossfade(const std::vector<Loop>& loops, double r);

// The playback-rate ratio of a loop at audio rpm `r`.
double pitch_ratio(const Loop& loop, double r);

// The harmonic tuning's loops in its group element; bank sample count and
// channels resolve the loop index (spec "Loop index").
void resolve_loop_indices(std::vector<Loop>& loops, int sample_count, const std::vector<int>& channels);

// Parsing. The engine tuning names the harmonic tunings; each harmonic
// tuning's file is read separately (read_harmonic). `model` may be null
// (no model tuning: every view level 1).
struct UpgradeLevels {
  int intake = 0;   // 0..3 -> L0..L3 (SPEC-ENGINE-SOUND "Emission groups and upgrade levels")
  int exhaust = 0;  // 0..3 -> L0..L3
};

struct HarmonicRef {
  std::string element;  // EngineIntake, EngineAmbient or Exhaust
  std::string file;     // the selected harmonic tuning, e.g. "6T2_RB26_o1043_Exh_HT.xml"
};
bool read_engine_tuning(const XmlNode& engine, EngineSound& out, std::vector<HarmonicRef>& harmonics,
                        std::string* error, UpgradeLevels levels = {});
// Adds the harmonic tuning's groups (ExhaustL and ExhaustR for an Exhaust
// tuning) to `out`, with the engine tuning's EmissionGroup<g> gain when it
// has one. Silent (NA) tunings and banks add nothing.
bool read_harmonic(const XmlNode& engine, const XmlNode& harmonic, const std::string& element, EngineSound& out,
                   std::string* error);
void read_model_tuning(const XmlNode& model, EngineSound& out);

bool silent_name(const std::string& name);  // NA.xml, NA_<group>_HT.xml, NA.fsb

}  // namespace audio
