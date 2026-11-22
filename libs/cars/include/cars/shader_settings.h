// Paint Shop finishes, Shared/ShaderSettings/*.xml parser, List_SpecialColors
// (102 two-tone paints), HSV colour helpers, and per-region CarPaintSpec
// (body, hood, wing, mirrors, aftermarket non-chrome rims, calipers, window tint).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
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
};

struct ShaderSettingsSet {
  ShaderFinishParams normal;
  ShaderFinishParams metallic;
  ShaderFinishParams matte;
  bool loaded_from_xml = false;
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

// Parses a ShaderSettings/*.xml body block (<SpecularCubeScalar>, <PaintScale>, etc.).
bool parse_shader_settings_xml(std::string_view xml, ShaderFinishParams& out);

// Loads Normal.xml, Metallic.xml, and Matte.xml from media/cars/Shared.zip.
bool load_shader_settings(const std::filesystem::path& shared_zip, ShaderSettingsSet& out,
                          std::string* error = nullptr);

// Queries all 102 List_SpecialColors rows from PartsDb.
bool query_special_colors(const PartsDb& db, std::vector<SpecialColorRow>& out, std::string* error = nullptr);

// Converts HSV (h in [0,1), s in [0,1], v in [0,1]) to packed sRGB 0xRRGGBB.
uint32_t hsv_to_rgb(float h, float s, float v);
void rgb_to_hsv(uint32_t rgb, float& h, float& s, float& v);

}  // namespace cars
