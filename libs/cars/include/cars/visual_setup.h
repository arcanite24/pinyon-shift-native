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
  bool is_exception = false;  // listed in WheelExceptions (non-paintable)
  bool is_paintable = false;  // !is_stock && !is_chrome && !is_exception
  double norm_offset = 0.0;   // WheelNormOffsets.NormOffset lateral normalization
};

struct CarPaintExceptions {
  bool found = false;
  bool no_mirrors = false;
  bool no_windows = false;
  bool no_hood = false;
  bool no_paintable_wing = false;
};

struct CarTrackOffsetRow {
  bool found = false;
  double front_offset = 0.0;
  double rear_offset = 0.0;
  double upgrade_front_offset = 0.0;
  double upgrade_rear_offset = 0.0;
};

struct BrakeVisualSpec {
  int size_in = 0;   // FrontBrakeSize / RearBrakeSize (7..17 inches)
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
  // Aftermarket rim lateral offset normalization from WheelNormOffsets.
  float rim_offset_front = 0.0f;
  float rim_offset_rear = 0.0f;
  bool wheel_is_chrome = false;
  bool wheel_is_stock = true;
  bool wheel_is_exception = false;  // WheelExceptions (non-paintable)
  bool wheel_is_paintable = false;  // !wheel_is_stock && !wheel_is_chrome && !wheel_is_exception
  int tire_compound_level = 0;  // 0=stock, 1=street (TireA), 2=sport (TireB), 3=race/5=rally (TireC/Rally)
  int tire_compound_id = 0;     // List_UpgradeTireCompound.TireCompoundID (9 = Rally)
  std::array<int64_t, 2> tyre_width_mm{0, 0};
  std::array<int64_t, 2> rim_inches{0, 0};
  std::array<int64_t, 2> stock_tyre_width_mm{0, 0};
  std::array<int64_t, 2> stock_rim_inches{0, 0};
  std::array<int64_t, 2> stock_tyre_aspect{0, 0};
  std::string tyre_media_override;  // e.g. "tireatoy_fjcruiser_12"

  // CarExceptions paint/region restrictions (NoMirrors, NoWindows, NoHood, NoPaintableWing).
  CarPaintExceptions paint_exceptions{};

  // Fitted upgrade levels & Rally Expansion visual flags (Lv4 bodykits/suspension, Lv5 Rally tires).
  int front_bumper_level = 0;
  int rear_bumper_level = 0;
  int rear_wing_level = 0;
  int spring_damper_level = 0;
  bool has_rally_bodykit = false;     // FrontBumper / RearBumper / RearWing Lv4
  bool has_rally_suspension = false;  // SpringDamper Lv4
  bool has_rally_tires = false;       // TireCompound Lv5 (or TireCompoundID == 9)

  // Suspension stance & alignment.
  std::array<float, 2> ride_height_m{-1.0f, -1.0f};  // >= 0 overrides baked ride height
  std::array<float, 2> track_offset_m{0.0f, 0.0f};   // per-side lateral offset (+ outward)
  std::array<float, 2> camber_rad{0.0f, 0.0f};       // negative = top tilts inward

  // Upgraded brakes (front=0, rear=1) + Data_Car.CaliperAngleFront/Rear (already in radians).
  std::array<BrakeVisualSpec, 2> brakes{};
  std::array<float, 2> caliper_angle_deg{0.0f, 0.0f};  // raw Data_Car.CaliperAngleFront/Rear (radians)
  bool has_caliper_angle_override = false;
};

// Computes the rolling outer radius (metres) of a tyre/rim setup, preserving
// the stock rolling outer radius when rim diameter or tyre width is upgraded
// (thinning the sidewall as rim diameter grows, clamped to a minimum sidewall
// so the rim never exceeds the outer radius).
float compute_tire_outer_radius_m(int64_t stock_rim_in, int64_t stock_width_mm, int64_t stock_aspect,
                                  int64_t fitted_rim_in, int64_t fitted_width_mm);

// Maps List_UpgradeTireCompound.Level to the tyre texture archive letter:
// <2 -> 'a' (TireA: Stock/Street), ==2 -> 'b' (TireB: Sport), >2 -> 'c' (TireC: Race/Rally).
char tire_compound_texture_letter(int compound_level);

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

// Reads WheelNormOffsets.NormOffset for `wheel_id` (sets out_offset = 0.0f if absent).
bool query_wheel_norm_offset(const PartsDb& db, int64_t wheel_id, float& out_offset,
                             std::string* error = nullptr);

// Checks whether `wheel_id` is listed in WheelExceptions (30 non-paintable wheels).
bool query_wheel_is_exception(const PartsDb& db, int64_t wheel_id, bool& out_is_exception,
                              std::string* error = nullptr);

// Returns true iff `wheel_id` is paintable in the Paint Shop (aftermarket, non-chrome,
// and not listed in WheelExceptions).
bool is_wheel_paintable(const PartsDb& db, int64_t wheel_id, std::string* error = nullptr);

// Queries CarExceptions (NoMirrors, NoWindows, NoHood, NoPaintableWing) for `car_id`.
bool car_paint_exceptions(const PartsDb& db, int64_t car_id, CarPaintExceptions& out,
                          std::string* error = nullptr);

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
