#include "cars/shader_settings.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "fh1data/archive.h"

namespace cars {
namespace {

bool find_tag_value(std::string_view block, std::string_view tag, float& out) {
  const std::string open = "<" + std::string(tag);
  const size_t pos = block.find(open);
  if (pos == std::string_view::npos) return false;
  const size_t end = block.find('>', pos);
  if (end == std::string_view::npos) return false;
  const std::string_view elem = block.substr(pos, end - pos);
  const std::string key = "value=\"";
  const size_t v = elem.find(key);
  if (v == std::string_view::npos) return false;
  const size_t vs = v + key.size();
  const size_t ve = elem.find('"', vs);
  if (ve == std::string_view::npos) return false;
  std::string num(elem.substr(vs, ve - vs));
  char* stop = nullptr;
  out = std::strtof(num.c_str(), &stop);
  return stop != num.c_str();
}

}  // namespace

const char* paint_finish_name(PaintFinish f) {
  switch (f) {
    case PaintFinish::kNormal: return "NORMAL GLOSS";
    case PaintFinish::kMetallic: return "METALLIC FLAKE";
    case PaintFinish::kMatte: return "MATTE";
    case PaintFinish::kTwoTone: return "TWO-TONE PEARL";
    case PaintFinish::kChrome: return "CHROME";
    case PaintFinish::kCarbon: return "CARBON FIBER";
    case PaintFinish::kWood: return "WOOD GRAIN";
    case PaintFinish::kCamo: return "CAMOUFLAGE";
    default: return "NORMAL";
  }
}

bool parse_shader_settings_xml(std::string_view xml, ShaderFinishParams& out) {
  const size_t b0 = xml.find("<body>");
  const size_t b1 = b0 == std::string_view::npos ? std::string_view::npos : xml.find("</body>", b0);
  const std::string_view scope =
      (b0 != std::string_view::npos && b1 != std::string_view::npos) ? xml.substr(b0, b1 - b0) : xml;
  bool any = false;
  any |= find_tag_value(scope, "SpecularCubeScalar", out.specular_cube_scalar);
  any |= find_tag_value(scope, "PaintScale", out.paint_scale);
  any |= find_tag_value(scope, "SpecularAmount2", out.spec_amount2);
  any |= find_tag_value(scope, "SpecularPower2", out.spec_power2);
  any |= find_tag_value(scope, "SpecularAmount3", out.spec_amount3);
  any |= find_tag_value(scope, "SpecularPower3", out.spec_power3);
  return any;
}

bool load_shader_settings(const std::filesystem::path& shared_zip, ShaderSettingsSet& out, std::string* error) {
  out = ShaderSettingsSet{};
  // Sensible defaults matching FH1 Normal/Metallic/Matte profiles.
  out.normal = {1.0f, 1.0f, 3.0f, 12.0f, 2.0f, 4.0f};
  out.metallic = {1.0f, 0.5f, 7.0f, 6.0f, 6.0f, 4.0f};
  out.matte = {0.15f, 0.85f, 0.6f, 2.0f, 0.3f, 1.5f};

  auto arch = fh1::Archive::open(shared_zip, error);
  if (!arch) return false;

  auto load_one = [&](const char* member, ShaderFinishParams& dst) -> bool {
    auto r = arch->read(member);
    if (!r.ok()) return false;
    return parse_shader_settings_xml(std::string_view(reinterpret_cast<const char*>(r.data.data()), r.data.size()),
                                     dst);
  };
  bool any = false;
  any |= load_one("ShaderSettings/Normal.xml", out.normal);
  any |= load_one("ShaderSettings/Metallic.xml", out.metallic);
  any |= load_one("ShaderSettings/Matte.xml", out.matte);
  out.loaded_from_xml = any;
  return any;
}

bool query_special_colors(const PartsDb& db, std::vector<SpecialColorRow>& out, std::string* error) {
  out.clear();
  return db.each(
      "SELECT ID, DisplayName, Finish, PriColorValid, PriColorRGB, SecColorValid, SecColorRGB, "
      "TwoToneScale, TwoToneBias, TwoTonePower FROM List_SpecialColors ORDER BY ID",
      {},
      [&](sqlite3_stmt* st) {
        SpecialColorRow r;
        r.id = sqlite3_column_int64(st, 0);
        const auto* txt = sqlite3_column_text(st, 1);
        r.display_key = txt ? reinterpret_cast<const char*>(txt) : "";
        r.finish = sqlite3_column_int(st, 2);
        r.pri_valid = sqlite3_column_int(st, 3) != 0;
        r.pri_rgb = uint32_t(sqlite3_column_int64(st, 4)) & 0xFFFFFFu;
        r.sec_valid = sqlite3_column_int(st, 5) != 0;
        r.sec_rgb = uint32_t(sqlite3_column_int64(st, 6)) & 0xFFFFFFu;
        r.two_tone_scale = float(sqlite3_column_double(st, 7));
        r.two_tone_bias = float(sqlite3_column_double(st, 8));
        r.two_tone_power = float(sqlite3_column_double(st, 9));
        out.push_back(std::move(r));
      },
      error);
}

uint32_t hsv_to_rgb(float h, float s, float v) {
  h = h - std::floor(h);
  s = std::clamp(s, 0.0f, 1.0f);
  v = std::clamp(v, 0.0f, 1.0f);
  const float c = v * s;
  const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
  const float m = v - c;
  float rf = 0, gf = 0, bf = 0;
  const int sector = int(h * 6.0f) % 6;
  switch (sector) {
    case 0: rf = c; gf = x; bf = 0; break;
    case 1: rf = x; gf = c; bf = 0; break;
    case 2: rf = 0; gf = c; bf = x; break;
    case 3: rf = 0; gf = x; bf = c; break;
    case 4: rf = x; gf = 0; bf = c; break;
    default: rf = c; gf = 0; bf = x; break;
  }
  const uint32_t r = uint32_t(std::clamp(std::lround((rf + m) * 255.0f), 0L, 255L));
  const uint32_t g = uint32_t(std::clamp(std::lround((gf + m) * 255.0f), 0L, 255L));
  const uint32_t b = uint32_t(std::clamp(std::lround((bf + m) * 255.0f), 0L, 255L));
  return (r << 16) | (g << 8) | b;
}

void rgb_to_hsv(uint32_t rgb, float& h, float& s, float& v) {
  const float r = float((rgb >> 16) & 0xFF) / 255.0f;
  const float g = float((rgb >> 8) & 0xFF) / 255.0f;
  const float b = float(rgb & 0xFF) / 255.0f;
  const float cmax = std::max({r, g, b});
  const float cmin = std::min({r, g, b});
  const float d = cmax - cmin;
  v = cmax;
  s = cmax > 1e-6f ? (d / cmax) : 0.0f;
  if (d < 1e-6f) {
    h = 0.0f;
  } else if (cmax == r) {
    h = std::fmod((g - b) / d + 6.0f, 6.0f) / 6.0f;
  } else if (cmax == g) {
    h = ((b - r) / d + 2.0f) / 6.0f;
  } else {
    h = ((r - g) / d + 4.0f) / 6.0f;
  }
}

}  // namespace cars
