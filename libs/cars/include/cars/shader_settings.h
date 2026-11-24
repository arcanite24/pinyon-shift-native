// Paint Shop finishes, Shared/ShaderSettings/*.xml parser, List_SpecialColors
// (102 two-tone paints), HSV colour helpers, and per-region CarPaintSpec
// (body, hood, wing, mirrors, aftermarket non-chrome rims, calipers, window tint).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cars/physics.h"

namespace cars {

enum class PaintFinish : int {
  kNormal = 0,
  kMetallic = 1,
  kMatte = 2,
  kTwoTone = 3,
  kChrome = 4,
  kCarbon = 5,
  kWood = 6,
  kCamo = 7,
  kCount = 8,
};

const char* paint_finish_name(PaintFinish f);

struct ShaderFinishParams {
  float specular_cube_scalar = 1.0f;
  float paint_scale = 1.0f;
  float spec_amount2 = 3.0f;
  float spec_power2 = 12.0f;
  float spec_amount3 = 2.0f;
  float spec_power3 = 4.0f;
  // Extended retail paint & cockpit shader parameters (Normal/Metallic/Matte & per-car XMLs).
  float spec_amount = 0.0f;
  float spec_power = 75.0f;
  float fresnel_index = 1.55f;
  float fresnel_scalar = 0.90f;
  std::array<float, 3> metal_color{0.0f, 0.0f, 1.0f};
  float metal_power = 5.0f;
  float metal_bias = 0.0f;
  float metal_scale = 0.0f;
  float gauge_emissive_scale = 3.0f;
};

struct ShaderSettingsSet {
  ShaderFinishParams normal;
  ShaderFinishParams metallic;
  ShaderFinishParams matte;
  bool loaded_from_xml = false;
  float gauge_emissive_scale = 3.0f;
};

struct RimShaderParams {
  float fresnel_index = 1.55f;
  float fresnel_scalar = 0.90f;
  float cube_scale = 1.0f;
  std::array<float, 3> tint_color{1.0f, 1.0f, 1.0f};
  bool has_override = false;
};

struct BrakeShaderParams {
  ShaderFinishParams params;
  bool has_override = false;
};

// Per-car shader overrides loaded from ColorShaderSettings1..17.xml,
// RimShaderSettings.xml, BrakeShaderSettings.xml, and ShaderSettings.xml.
struct CarShaderOverrides {
  std::map<int, ShaderFinishParams> color_overrides;  // keyed by 1-based color sequence (1..17)
  RimShaderParams rim;
  BrakeShaderParams brake;
  bool has_gauge_emissive_override = false;
  float gauge_emissive_scale = 3.0f;
  bool loaded_any = false;
};

struct SpecialColorRow {
  int64_t id = 0;
  std::string display_key;
  int finish = 1;  // 0=Normal, 1=Metallic/TwoTone, 2=Matte, etc.
  bool pri_valid = true;
  uint32_t pri_rgb = 0;
  bool sec_valid = false;
  uint32_t sec_rgb = 0;
  float two_tone_scale = 0.0f;
  float two_tone_bias = 0.0f;
  float two_tone_power = 0.0f;
};

struct PaintRegionSpec {
  uint32_t rgb = 0xCC2222;
  uint32_t sec_rgb = 0x2244CC;
  PaintFinish finish = PaintFinish::kNormal;
  float two_tone_scale = 0.25f;
  float two_tone_bias = 0.0f;
  float two_tone_power = 4.0f;
  bool active = false;
};

struct CarPaintSpec {
  PaintRegionSpec body;
  PaintRegionSpec hood;
  PaintRegionSpec wing;
  PaintRegionSpec mirrors;
  PaintRegionSpec rims;      // ignored on chrome rims (SPEC-AUTOSHOW-PAINT)
  PaintRegionSpec calipers;
  float window_tint = 0.0f;  // 0.0 = stock glass .. 1.0 = limo tint
};

// Parses a ShaderSettings/*.xml body block (and optional <gauge_emissive> block),
// overlaying any present tags onto `out` while preserving defaults for absent tags.
bool parse_shader_settings_xml(std::string_view xml, ShaderFinishParams& out);

// Parses a specific <technique>...</technique> block from a ShaderSettings XML.
bool parse_shader_settings_xml_technique(std::string_view xml, std::string_view technique, ShaderFinishParams& out);

// Parses RimShaderSettings.xml (<rim_V2>, <rim>, <outer_rim>, or root).
bool parse_rim_shader_settings_xml(std::string_view xml, RimShaderParams& out);

// Loads Normal.xml, Metallic.xml, and Matte.xml from media/cars/Shared.zip (or unpacked dir).
bool load_shader_settings(const std::filesystem::path& shared_zip, ShaderSettingsSet& out,
                          std::string* error = nullptr);

// Loads per-car ColorShaderSettings1..17.xml, RimShaderSettings.xml, BrakeShaderSettings.xml,
// and ShaderSettings.xml (GaugeEmissiveScale) from a car archive (.zip) or unpacked directory.
bool load_car_shader_overrides(const std::filesystem::path& car_archive_or_dir, CarShaderOverrides& out,
                               const ShaderFinishParams* base_finish = nullptr, std::string* error = nullptr);

// Queries all 102 List_SpecialColors rows from PartsDb.
bool query_special_colors(const PartsDb& db, std::vector<SpecialColorRow>& out, std::string* error = nullptr);

// Converts HSV (h in [0,1), s in [0,1], v in [0,1]) to packed sRGB 0xRRGGBB.
uint32_t hsv_to_rgb(float h, float s, float v);
void rgb_to_hsv(uint32_t rgb, float& h, float& s, float& v);

}  // namespace cars
