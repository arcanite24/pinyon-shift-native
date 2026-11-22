// Car tuning sliders (all 35 Career_Garage.Tuning_* parameters):
// tyres, gearing, alignment, ARBs, springs, ride height, damping, aero, brakes, differential.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "drive/from_physics.h"

namespace garage {

enum class TuneCategory : int {
  kTires = 0,
  kGearing = 1,
  kAlignment = 2,
  kAntiRollBars = 3,
  kSprings = 4,
  kDamping = 5,
  kAero = 6,
  kBrake = 7,
  kDifferential = 8,
  kCount = 9,
};

const char* tune_category_name(TuneCategory cat);

struct TuneSlider {
  std::string key;       // Tuning_* column name
  std::string label;
  std::string unit;
  TuneCategory category = TuneCategory::kTires;
  double min_val = 0.0;
  double max_val = 1.0;
  double def_val = 0.5;
  double step = 0.05;
  bool unlocked = true;
  // Current value in physical/display units; `tuned` false means stock default.
  double value = 0.5;
  bool tuned = false;
};

// Builds the list of tuning sliders for `car` with ranges and unlock status
// derived from its fitted CarPhysics rows and current `tune`.
std::vector<TuneSlider> build_tune_sliders(const cars::CarPhysics& car, const drive::CarTune& tune);

// Writes a slider's value back into `tune` (by Tuning_* key).
bool set_tune_slider(drive::CarTune& tune, const cars::CarPhysics& car, const std::string& key, double value);

// Resets all sliders in `tune` (or only one category) to stock defaults.
void reset_tune(drive::CarTune& tune);
void reset_tune_category(drive::CarTune& tune, TuneCategory category);

// Serializes non-default sliders as "key=val ..." for garage.txt.
std::string tune_to_tokens(const drive::CarTune& tune);
// Parses a single "Tuning_xxx=val" token into `tune`; returns true if recognized.
bool parse_tune_token(const std::string& kv, drive::CarTune& tune);

}  // namespace garage
