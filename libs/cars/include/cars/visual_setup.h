// Visual appearance resolved from a car's fitted upgrade parts, aftermarket
// rims (List_Wheels, part_enum 43), CarTrackOffsets, upgraded brakes
// (media/brakes/), suspension ride height / camber, and per-car tyres.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cars/physics.h"

namespace cars {

struct WheelRow {
  int64_t id = 0;
  std::string media_name;
  std::string display_name;
  int64_t manufacturer_id = 0;
  std::string manufacturer_key;  // List_PartManufacturer.DisplayName or MediaName prefix
  int64_t price = 0;
  double mass = 0.0;
  double brake_draw_offset = 0.0;
  bool is_stock = false;
  std::string wheel_color;
  std::string actual_color;
  uint32_t rgb = 0;
  bool is_chrome = false;
};

struct CarTrackOffsetRow {
  bool found = false;
  double front_offset = 0.0;
  double rear_offset = 0.0;
  double upgrade_front_offset = 0.0;
  double upgrade_rear_offset = 0.0;
};

struct BrakeVisualSpec {
  int size_in = 0;   // FrontBrakeSize / RearBrakeSize (7..16 inches)
  int type_id = 2;   // 1=drum, 2=steel, 3=drilled, 4=slotted, 5=carbon
  int pistons = 2;   // 2, 4, 6, or 8
  int level = 0;     // 0=stock (use car's own baked brake models), >=1=swap from media/brakes/
};

// Complete visual configuration fed into Showroom::load() so fitted body kits,
// wings, hoods, skirts, roll cages, removed seats, exhausts, aftermarket rims,
// upgraded brakes, lowered suspension, CarTrackOffsets, camber, and per-car
// tyres (incl. TOY_FJCruiser_12) render accurately on the 3D car.
struct VisualSetup {
  bool active = false;
  // Lower-case List_PartsStrings tokens shown/removed by the fitted upgrades.
  std::vector<std::string> shown_parts;
  std::vector<std::string> removed_parts;
  bool has_exhaust_override = false;
  std::string exhaust_part;  // lower-case fitted exhaust part name (if any)

  // Wheel & tyre overrides (empty / 0 = keep baked stock values).
  std::string wheel_media;
  int64_t wheel_id = 0;
  float brake_draw_offset = 0.0f;
  bool wheel_is_chrome = false;
  bool wheel_is_stock = true;
  int tire_compound_level = 0;  // 0=stock/street (TireA), 1=sport (TireB), >=2=race/rally (TireC)
  std::array<int64_t, 2> tyre_width_mm{0, 0};
  std::array<int64_t, 2> rim_inches{0, 0};
  std::string tyre_media_override;  // e.g. "tireatoy_fjcruiser_12"

  // Suspension stance & alignment.
  std::array<float, 2> ride_height_m{-1.0f, -1.0f};  // >= 0 overrides baked ride height
  std::array<float, 2> track_offset_m{0.0f, 0.0f};   // per-side lateral offset (+ outward)
  std::array<float, 2> camber_rad{0.0f, 0.0f};       // negative = top tilts inward

  // Upgraded brakes (front=0, rear=1) + Data_Car caliper angles (degrees).
  std::array<BrakeVisualSpec, 2> brakes{};
  std::array<float, 2> caliper_angle_deg{0.0f, 0.0f};
  bool has_caliper_angle_override = false;
};

// Returns the lower-case per-car tyre archive stem ("tirea<media>") for the
// 6 models that ship their own tyre set (SVT Raptor + LCE/HALO, H1 Alpha,
// Wrangler Rubicon, FJ Cruiser), or empty string for cars using shared TireC.
std::string per_car_tyre_media(std::string_view car_media);

// Returns true if `car_media` is the FJ Cruiser (TOY_FJCruiser_12) or another
// per-car tyre model whose target A matches its modelled outer radius.
bool is_own_tyre_car(std::string_view car_media);

// Lists aftermarket wheels from List_Wheels (IsStock = 0, sorted by
// PartManufacturerID, DisplayName). If `include_stock` is true, returns all
// List_Wheels rows with a MediaName.
bool query_aftermarket_wheels(const PartsDb& db, std::vector<WheelRow>& out, std::string* error = nullptr,
                              bool include_stock = false);

// Looks up a single List_Wheels row by ID.
bool query_wheel_by_id(const PartsDb& db, int64_t wheel_id, WheelRow& out, std::string* error = nullptr);

// Reads CarTrackOffsets for `car_id` (if a row exists).
bool query_track_offsets(const PartsDb& db, int64_t car_id, CarTrackOffsetRow& out, std::string* error = nullptr);

// Resolves a car's complete VisualSetup from its fitted parts map and optional
// tuning overrides (normalized ride height [0..1] across Min..Max or < 0 for
// default; static camber in degrees or < -90 for default).
bool resolve_visual_setup(const PartsDb& db, int64_t car_id, const FittedParts& fit, VisualSetup& out,
                          std::string* error = nullptr, float tune_front_rh_norm = -1.0f, float tune_rear_rh_norm = -1.0f,
                          float tune_front_camber_deg = -100.0f, float tune_rear_camber_deg = -100.0f);

// Brake rotor filename stem for List_BrakeType ID (1..5):
// "drum", "rotor_steel", "rotor_drilled", "rotor_slotted", "rotor_carbon".
const char* brake_type_stem(int type_id);

}  // namespace cars
