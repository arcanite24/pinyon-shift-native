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
  // `neg_raises`: the open sign of NegTorque. false (the default) subtracts
  // it, as the spec's formula reads; true adds it.
  double eval(const SoundInputs& in, bool neg_raises = false) const;
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
  std::array<double, kViews> view_level{1, 1, 1, 1};
  std::array<double, kViews> pan_left{1, 1, 1, 1}, pan_right{1, 1, 1, 1};  // folded 5.1 (ours)
};

struct EngineSound {
  std::string media;
  double audio_rpm_idle = 800, audio_rpm_redline = 7000;
  Param global_volume;     // GlobalCarVolume
  double engine_volume = 1;  // VolumeAdjustments EngineVolume
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
struct HarmonicRef {
  std::string element;  // EngineIntake, EngineAmbient or Exhaust
  std::string file;     // the L0 harmonic tuning, e.g. "6T2_RB26_o1043_Exh_HT.xml"
};
bool read_engine_tuning(const XmlNode& engine, EngineSound& out, std::vector<HarmonicRef>& harmonics,
                        std::string* error);
// Adds the harmonic tuning's groups (ExhaustL and ExhaustR for an Exhaust
// tuning) to `out`, with the engine tuning's EmissionGroup<g> gain when it
// has one. Silent (NA) tunings and banks add nothing.
bool read_harmonic(const XmlNode& engine, const XmlNode& harmonic, const std::string& element, EngineSound& out,
                   std::string* error);
void read_model_tuning(const XmlNode& model, EngineSound& out);

bool silent_name(const std::string& name);  // NA.xml, NA_<group>_HT.xml, NA.fsb

}  // namespace audio
