#include "cars/shader_settings.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>

#include "cars/util.h"
#include "fh1data/archive.h"

namespace cars {
namespace {

std::string lower_copy(std::string_view s) {
  std::string out(s.size(), '\0');
  for (size_t i = 0; i < s.size(); ++i) out[i] = char(std::tolower(static_cast<unsigned char>(s[i])));
  return out;
}

bool is_tag_delim(char c) {
  return c == '>' || c == '/' || std::isspace(static_cast<unsigned char>(c));
}

// Extracts `<technique>...</technique>` (case-insensitive) from `xml`, returning
// empty string_view when absent.
std::string_view find_technique_block(std::string_view xml, std::string_view xml_low, std::string_view technique) {
  const std::string tech_l = lower_copy(technique);
  const std::string open_prefix = "<" + tech_l;
  const std::string close_tag = "</" + tech_l + ">";
  size_t pos = 0;
  while ((pos = xml_low.find(open_prefix, pos)) != std::string_view::npos) {
    const size_t after = pos + open_prefix.size();
    if (after < xml_low.size() && is_tag_delim(xml_low[after])) {
      const size_t open_end = xml_low.find('>', after);
      if (open_end == std::string_view::npos) return {};
      if (open_end > after && xml_low[open_end - 1] == '/') {
        pos = open_end + 1;
        continue;
      }
      const size_t close_pos = xml_low.find(close_tag, open_end + 1);
      if (close_pos == std::string_view::npos) return {};
      return xml.substr(open_end + 1, close_pos - (open_end + 1));
    }
    pos = after;
  }
  return {};
}

// Finds `<Tag ...>` (exact tag name match, case-insensitive) inside `block` and
// returns the attribute slice between `<Tag` and `>`.
std::string_view find_tag_attrs(std::string_view block, std::string_view block_low, std::string_view tag) {
  const std::string open_prefix = "<" + lower_copy(tag);
  size_t pos = 0;
  while ((pos = block_low.find(open_prefix, pos)) != std::string_view::npos) {
    const size_t after = pos + open_prefix.size();
    if (after < block_low.size() && is_tag_delim(block_low[after])) {
      const size_t end = block_low.find('>', after);
      if (end == std::string_view::npos) return {};
      return block.substr(after, end - after);
    }
    pos = after;
  }
  return {};
}

bool read_float_attr(std::string_view attrs, std::string_view attr_name, float& out) {
  const std::string attrs_low = lower_copy(attrs);
  const std::string key = lower_copy(attr_name) + "=\"";
  size_t pos = 0;
  while ((pos = attrs_low.find(key, pos)) != std::string::npos) {
    if (pos == 0 || std::isspace(static_cast<unsigned char>(attrs_low[pos - 1]))) {
      const size_t vs = pos + key.size();
      const size_t ve = attrs.find('"', vs);
      if (ve == std::string_view::npos) return false;
      std::string num(attrs.substr(vs, ve - vs));
      char* stop = nullptr;
      const float parsed = std::strtof(num.c_str(), &stop);
      if (stop != num.c_str()) {
        out = parsed;
        return true;
      }
      return false;
    }
    pos += 1;
  }
  return false;
}

bool find_tag_value(std::string_view block, std::string_view block_low, std::string_view tag, float& out) {
  const std::string_view attrs = find_tag_attrs(block, block_low, tag);
  if (attrs.empty()) return false;
  return read_float_attr(attrs, "value", out);
}

bool find_tag_rgb(std::string_view block, std::string_view block_low, std::string_view tag,
                  std::array<float, 3>& out) {
  const std::string_view attrs = find_tag_attrs(block, block_low, tag);
  if (attrs.empty()) return false;
  float r = out[0], g = out[1], b = out[2];
  bool ok = false;
  if (read_float_attr(attrs, "r", r) && read_float_attr(attrs, "g", g) && read_float_attr(attrs, "b", b)) {
    out = {r, g, b};
    ok = true;
  } else if (read_float_attr(attrs, "x", r) && read_float_attr(attrs, "y", g) && read_float_attr(attrs, "z", b)) {
    out = {r, g, b};
    ok = true;
  }
  return ok;
}

bool parse_finish_block(std::string_view scope, ShaderFinishParams& out) {
  const std::string scope_low = lower_copy(scope);
  bool any = false;
  any |= find_tag_value(scope, scope_low, "SpecularCubeScalar", out.specular_cube_scalar);
  any |= find_tag_value(scope, scope_low, "PaintScale", out.paint_scale);
  any |= find_tag_value(scope, scope_low, "SpecularAmount", out.spec_amount);
  any |= find_tag_value(scope, scope_low, "SpecularPower", out.spec_power);
  any |= find_tag_value(scope, scope_low, "SpecularAmount2", out.spec_amount2);
  any |= find_tag_value(scope, scope_low, "SpecularPower2", out.spec_power2);
  any |= find_tag_value(scope, scope_low, "SpecularAmount3", out.spec_amount3);
  any |= find_tag_value(scope, scope_low, "SpecularPower3", out.spec_power3);
  any |= find_tag_value(scope, scope_low, "FresnelIndex", out.fresnel_index);
  any |= find_tag_value(scope, scope_low, "FresnelScalar", out.fresnel_scalar);
  any |= find_tag_rgb(scope, scope_low, "MetalColor", out.metal_color);
  any |= find_tag_value(scope, scope_low, "MetalPower", out.metal_power);
  any |= find_tag_value(scope, scope_low, "MetalBias", out.metal_bias);
  any |= find_tag_value(scope, scope_low, "MetalScale", out.metal_scale);
  any |= find_tag_value(scope, scope_low, "GaugeEmissiveScale", out.gauge_emissive_scale);
  return any;
}

bool parse_gauge_emissive_scale(std::string_view xml, float& out) {
  const std::string xml_low = lower_copy(xml);
  for (std::string_view tech : {"gauge_emissive", "cockpit_gauge_emissive", "gauge_needle"}) {
    const std::string_view blk = find_technique_block(xml, xml_low, tech);
    if (!blk.empty()) {
      const std::string blk_low = lower_copy(blk);
      if (find_tag_value(blk, blk_low, "GaugeEmissiveScale", out)) return true;
    }
  }
  return find_tag_value(xml, xml_low, "GaugeEmissiveScale", out);
}

bool read_text_file(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const std::streamoff len = in.tellg();
  if (len <= 0) return false;
  in.seekg(0);
  out.resize(static_cast<size_t>(len));
  return bool(in.read(out.data(), len));
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

bool parse_shader_settings_xml_technique(std::string_view xml, std::string_view technique, ShaderFinishParams& out) {
  const std::string xml_low = lower_copy(xml);
  const std::string_view blk = find_technique_block(xml, xml_low, technique);
  if (blk.empty()) return false;
  return parse_finish_block(blk, out);
}

bool parse_shader_settings_xml(std::string_view xml, ShaderFinishParams& out) {
  const std::string xml_low = lower_copy(xml);
  const std::string_view body_blk = find_technique_block(xml, xml_low, "body");
  const std::string_view scope = body_blk.empty() ? xml : body_blk;
  bool any = parse_finish_block(scope, out);
  float gauge_scale = out.gauge_emissive_scale;
  if (parse_gauge_emissive_scale(xml, gauge_scale)) {
    out.gauge_emissive_scale = gauge_scale;
    any = true;
  }
  return any;
}

bool parse_rim_shader_settings_xml(std::string_view xml, RimShaderParams& out) {
  const std::string xml_low = lower_copy(xml);
  std::string_view scope;
  for (std::string_view tech : {"rim_v2", "rim", "outer_rim"}) {
    scope = find_technique_block(xml, xml_low, tech);
    if (!scope.empty()) break;
  }
  if (scope.empty()) scope = xml;
  const std::string scope_low = lower_copy(scope);
  bool any = false;
  any |= find_tag_value(scope, scope_low, "FresnelIndex", out.fresnel_index);
  any |= find_tag_value(scope, scope_low, "FresnelScalar", out.fresnel_scalar);
  any |= find_tag_value(scope, scope_low, "CubeScale", out.cube_scale);
  any |= find_tag_value(scope, scope_low, "SpecularCubeScalar", out.cube_scale);
  any |= find_tag_rgb(scope, scope_low, "TintColor", out.tint_color);
  if (any) out.has_override = true;
  return any;
}

bool load_shader_settings(const std::filesystem::path& shared_zip, ShaderSettingsSet& out, std::string* error) {
  out = ShaderSettingsSet{};
  // Sensible defaults matching FH1 Normal/Metallic/Matte profiles when XMLs are absent.
  out.normal.specular_cube_scalar = 1.0f;
  out.normal.paint_scale = 1.0f;
  out.normal.spec_amount2 = 3.0f;
  out.normal.spec_power2 = 12.0f;
  out.normal.spec_amount3 = 2.0f;
  out.normal.spec_power3 = 4.0f;
  out.normal.fresnel_index = 1.55f;
  out.normal.fresnel_scalar = 0.90f;
  out.normal.gauge_emissive_scale = 3.0f;

  out.metallic = out.normal;
  out.metallic.paint_scale = 0.5f;
  out.metallic.spec_amount2 = 7.0f;
  out.metallic.spec_power2 = 6.0f;
  out.metallic.spec_amount3 = 6.0f;
  out.metallic.spec_power3 = 4.0f;

  out.matte = out.normal;
  out.matte.specular_cube_scalar = 0.15f;
  out.matte.paint_scale = 0.85f;
  out.matte.spec_amount2 = 0.6f;
  out.matte.spec_power2 = 2.0f;
  out.matte.spec_amount3 = 0.3f;
  out.matte.spec_power3 = 1.5f;
  out.matte.fresnel_index = 1.0f;
  out.matte.fresnel_scalar = 0.0f;
  out.matte.metal_color = {1.0f, 1.0f, 1.0f};
  out.matte.metal_power = 0.7f;
  out.matte.metal_bias = 0.0f;
  out.matte.metal_scale = 0.06f;

  namespace fs = std::filesystem;
  std::error_code ec;
  const bool is_dir = fs::is_directory(shared_zip, ec);
  std::unique_ptr<fh1::Archive> arch;
  if (!is_dir) {
    arch = fh1::Archive::open(shared_zip, error);
    if (!arch) return false;
  }

  auto read_member = [&](const char* rel, std::string& text) -> bool {
    if (is_dir) {
      return read_text_file(shared_zip / rel, text);
    }
    auto r = arch->read(rel);
    if (!r.ok()) return false;
    text.assign(reinterpret_cast<const char*>(r.data.data()), r.data.size());
    return true;
  };

  bool any = false;
  std::string xml;
  if (read_member("ShaderSettings/Normal.xml", xml)) {
    if (parse_shader_settings_xml(xml, out.normal)) {
      any = true;
      // Per SPEC-AUTOSHOW-PAINT, Metallic.xml and Matte.xml are overlays over Normal.xml.
      out.metallic = out.normal;
      out.matte = out.normal;
      out.gauge_emissive_scale = out.normal.gauge_emissive_scale;
    }
  }
  if (read_member("ShaderSettings/Metallic.xml", xml)) {
    any |= parse_shader_settings_xml(xml, out.metallic);
  }
  if (read_member("ShaderSettings/Matte.xml", xml)) {
    any |= parse_shader_settings_xml(xml, out.matte);
  }
  out.loaded_from_xml = any;
  return any;
}

bool load_car_shader_overrides(const std::filesystem::path& car_archive_or_dir, CarShaderOverrides& out,
                               const ShaderFinishParams* base_finish, std::string* error) {
  out = CarShaderOverrides{};
  const ShaderFinishParams base = base_finish ? *base_finish : ShaderFinishParams{};
  out.gauge_emissive_scale = base.gauge_emissive_scale;

  namespace fs = std::filesystem;
  std::error_code ec;
  const bool is_dir = fs::is_directory(car_archive_or_dir, ec);
  std::unique_ptr<fh1::Archive> arch;
  if (!is_dir) {
    arch = fh1::Archive::open(car_archive_or_dir, error);
    if (!arch) return false;
  }

  auto process_xml = [&](const std::string& filename_lower, std::string_view xml) {
    if (filename_lower == "shadersettings.xml") {
      float ge = out.gauge_emissive_scale;
      if (parse_gauge_emissive_scale(xml, ge)) {
        out.gauge_emissive_scale = ge;
        out.has_gauge_emissive_override = true;
        out.loaded_any = true;
      }
      return;
    }
    if (filename_lower == "rimshadersettings.xml") {
      if (parse_rim_shader_settings_xml(xml, out.rim)) {
        out.loaded_any = true;
      }
      return;
    }
    if (filename_lower == "brakeshadersettings.xml") {
      ShaderFinishParams bp = base;
      bool parsed = false;
      for (std::string_view tech : {"caliper", "rotor", "brake", "body", "debugbody"}) {
        if (parse_shader_settings_xml_technique(xml, tech, bp)) {
          parsed = true;
          break;
        }
      }
      if (!parsed) parsed = parse_shader_settings_xml(xml, bp);
      if (parsed) {
        out.brake.params = bp;
        out.brake.has_override = true;
        out.loaded_any = true;
      }
      return;
    }
    // ColorShaderSettings1.xml .. ColorShaderSettings17.xml (and any positive integer index).
    constexpr std::string_view kPrefix = "colorshadersettings";
    constexpr std::string_view kSuffix = ".xml";
    if (filename_lower.size() > kPrefix.size() + kSuffix.size() &&
        filename_lower.rfind(kPrefix, 0) == 0 &&
        filename_lower.compare(filename_lower.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
      const std::string num_str =
          filename_lower.substr(kPrefix.size(), filename_lower.size() - kPrefix.size() - kSuffix.size());
      char* stop = nullptr;
      const long idx = std::strtol(num_str.c_str(), &stop, 10);
      if (stop == num_str.c_str() + num_str.size() && idx >= 1 && idx <= 64) {
        ShaderFinishParams fp = base;
        if (parse_shader_settings_xml(xml, fp)) {
          out.color_overrides[int(idx)] = fp;
          out.loaded_any = true;
        }
      }
    }
  };

  if (is_dir) {
    for (const auto& entry : fs::directory_iterator(car_archive_or_dir, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      const std::string fn = lower(entry.path().filename().string());
      if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".xml") != 0) continue;
      std::string xml;
      if (read_text_file(entry.path(), xml)) process_xml(fn, xml);
    }
  } else {
    for (const auto& mem : arch->members()) {
      const std::string ln = lower(mem.name);
      if (ln.find('/') != std::string::npos) continue;
      if (ln.size() < 4 || ln.compare(ln.size() - 4, 4, ".xml") != 0) continue;
      auto r = arch->read(mem.name);
      if (!r.ok()) continue;
      process_xml(ln, std::string_view(reinterpret_cast<const char*>(r.data.data()), r.data.size()));
    }
  }
  return out.loaded_any;
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
