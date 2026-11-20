#include "cars/bake.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "fh1data/carbin.h"
#include "fh1data/xds.h"

namespace cars {
namespace {

using bake::MaterialClass;
using bake::Role;

bool ends_with(const std::string& s, const std::string& t) {
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}
bool starts_with(const std::string& s, const std::string& t) { return s.rfind(t, 0) == 0; }

bake::Texture texture_from_blocks(const std::string& name, const fh1::XdsBlocks& blocks) {
  bake::Texture t;
  t.name = name;
  t.format = bake::TextureFormat(uint32_t(blocks.format));
  t.swizzle = blocks.swizzle;
  for (const auto& l : blocks.levels) t.levels.push_back({l.width, l.height, l.blocks});
  return t;
}

// Every .xds of a folder (sorted by name), as block textures.
void bake_textures(Folder& folder, bake::Bake& out, std::vector<std::string>* notes) {
  for (const std::string& name : folder.names()) {
    if (!ends_with(name, ".xds")) continue;
    std::vector<uint8_t> data;
    std::string error;
    fh1::XdsBlocks blocks;
    if (!folder.read(name, data, &error) || !fh1::extract_xds_blocks(data, blocks, &error)) {
      if (notes) notes->push_back("texture " + name + ": " + error);
      continue;
    }
    out.textures.push_back(texture_from_blocks(name, blocks));
  }
}

bake::Vertex bake_vertex(const fh1::CarbinPart& part, const fh1::CarbinVertex& v) {
  bake::Vertex o;
  o.position = fh1::carbin_position(part, v);
  o.frame = v.frame;
  o.uv0 = v.uv0;
  o.uv1 = v.uv1;
  o.lighting = v.lighting;
  return o;
}

Role role_of(const std::string& name, bake::Corner& corner) {
  corner = bake::Corner::kNone;
  for (auto [tag, c] : {std::pair{"lf_", bake::Corner::kLF}, std::pair{"rf_", bake::Corner::kRF},
                        std::pair{"lr_", bake::Corner::kLR}, std::pair{"rr_", bake::Corner::kRR}}) {
    for (const char* kind : {"caliper", "rotor"}) {
      if (name.find(std::string(kind) + tag) != std::string::npos) corner = c;
    }
  }
  if (name.find("caliper") != std::string::npos) return Role::kCaliper;
  if (name.find("rotor") != std::string::npos) return Role::kRotor;
  if (ends_with(name, "_cockpit.carbin")) return Role::kCockpit;
  if (ends_with(name, "_lod0.carbin")) return Role::kLod0;
  return Role::kExterior;
}

// Texture slots of a sub-section (FORMAT-CARS "Texture slots"); names must
// exist in their pack or the slot is left empty and noted.
void assign_slots(bake::Subsection& s, Role role, const std::set<std::string>& own,
                  const std::set<std::string>& shared, std::vector<std::string>& notes) {
  const std::string t = lower(s.technique);
  const bool lod0 = s.lod == 0;
  auto own_ref = [&](const std::string& lod0_name, const std::string& lod_name) -> std::string {
    const std::string& n = lod0 ? lod0_name : lod_name;
    if (own.count(n)) return (role == Role::kWheel || role == Role::kTyre ? "wheel:" : "car:") + n;
    if (own.count(lod0_name)) return (role == Role::kWheel || role == Role::kTyre ? "wheel:" : "car:") + lod0_name;
    notes.push_back("no " + n + " for technique " + s.technique);
    return "";
  };
  auto shared_ref = [&](const std::string& n) -> std::string {
    if (shared.empty() || shared.count(n)) return "shared:" + n;
    notes.push_back("no shared " + n + " for technique " + s.technique);
    return "";
  };
  if (role == Role::kWheel) {
    s.slots[0] = own_ref("wheel_lod0.xds", "wheel.xds");
    return;
  }
  if (role == Role::kTyre) {
    // The tyre textures are taken from the car's tyre texture archive.
    if (t == "sidewall") s.slots[1] = "tyre:sidewall";
    if (t == "tread") s.slots[1] = "tyre:tread";
    return;
  }
  s.slots[0] = "@atlas";
  switch (s.material) {
    case MaterialClass::kInterior:
    case MaterialClass::kGauge:
      s.slots[1] = own_ref("interior_lod0.xds", "interior_lod0.xds");
      break;
    case MaterialClass::kLamp:
      s.slots[1] = own_ref("lights_lod0.xds", "lights.xds");
      break;
    case MaterialClass::kCarbon:
      s.slots[1] = shared_ref("carbonfiber.xds");
      break;
    default:
      if (t == "numplate") s.slots[1] = own_ref("lights_lod0.xds", "lights.xds");
      if (t == "bumper_frame") s.slots[1] = shared_ref("bumper_frame.xds");
      if (t == "grille1") s.slots[1] = shared_ref("grille1_alpha.xds");
      break;
  }
}

bool parse_model(Folder& folder, const std::string& name, Role role, bake::Corner corner,
                 const std::set<std::string>& own, const std::set<std::string>& shared, bake::Model& out,
                 std::vector<std::string>& notes, std::string* error) {
  std::vector<uint8_t> data;
  if (!folder.read(name, data, error)) return false;
  fh1::Carbin carbin;
  if (!fh1::parse_carbin(data, carbin, error)) {
    if (error) *error = name + ": " + *error;
    return false;
  }
  const auto transparent = fh1::carbin_transparent_flags(carbin);
  out.name = name;
  out.role = role;
  out.corner = corner;
  for (size_t p = 0; p < carbin.parts.size(); ++p) {
    const fh1::CarbinPart& part = carbin.parts[p];
    if (part.empty_box()) continue;  // no geometry
    bake::Part bp;
    bp.name = part.name;
    bp.bounds = part.bounds;
    for (const auto& v : part.lod_pool.vertices) bp.lod_vertices.push_back(bake_vertex(part, v));
    for (const auto& v : part.lod0_pool.vertices) bp.lod0_vertices.push_back(bake_vertex(part, v));
    for (size_t i = 0; i < part.subsections.size(); ++i) {
      const fh1::CarbinSubsection& s = part.subsections[i];
      bake::Subsection bs;
      bs.technique = s.material;
      bs.transparent = transparent[p][i];
      bs.lod = s.lod;
      bs.uv_transform = s.uv_transform;
      bs.material = classify(s.material, bs.transparent, role);
      bs.indices = fh1::carbin_triangle_list(s);
      assign_slots(bs, role, own, shared, notes);
      bp.subsections.push_back(std::move(bs));
    }
    out.parts.push_back(std::move(bp));
  }
  return true;
}

// <Name attr="value" ... /> elements of Masks.xml.
std::vector<bake::LiveryPlane> parse_masks(const std::string& xml) {
  std::vector<bake::LiveryPlane> planes;
  size_t at = 0;
  while ((at = xml.find('<', at)) != std::string::npos) {
    ++at;
    if (at >= xml.size() || xml[at] == '?' || xml[at] == '/' || xml[at] == '!') continue;
    size_t end = xml.find('>', at);
    if (end == std::string::npos) break;
    const std::string body = xml.substr(at, end - at);
    at = end;
    size_t name_end = body.find_first_of(" \t\r\n/");
    const std::string name = body.substr(0, name_end);
    if (name == "LiveryMasks" || name.empty()) continue;
    bake::LiveryPlane p;
    p.name = name;
    auto attr = [&](const std::string& key) -> std::string {
      const std::string k = key + "=\"";
      size_t a = 0;
      while ((a = body.find(k, a)) != std::string::npos) {
        if (a == 0 || std::isspace(static_cast<unsigned char>(body[a - 1]))) break;
        ++a;
      }
      if (a == std::string::npos) return "";
      const size_t v = a + k.size();
      return body.substr(v, body.find('"', v) - v);
    };
    auto num = [&](const std::string& key, float fallback) {
      const std::string v = attr(key);
      try {
        return v.empty() ? fallback : std::stof(v);
      } catch (...) {
        return fallback;
      }
    };
    p.valid = lower(attr("valid")) == "true";
    p.left = num("left", 0);
    p.right = num("right", 0);
    p.top = num("top", 0);
    p.bottom = num("bottom", 0);
    p.x_origin = num("xorigin", 0);
    p.y_origin = num("yorigin", 0);
    p.x_scale = num("xScale", 1);
    p.y_scale = num("yScale", 1);
    p.rotation = num("rotation", 0);
    p.x_axis = attr("xAxis");
    p.y_axis = attr("yAxis");
    planes.push_back(p);
  }
  return planes;
}

void bake_livery(Folder& folder, bake::Bake& out, std::vector<std::string>& notes) {
  std::vector<uint8_t> data;
  std::string error;
  if (!folder.contains("liverymasks/masks.xml")) return;
  if (!folder.read("liverymasks/masks.xml", data, &error)) {
    notes.push_back("livery: " + error);
    return;
  }
  out.livery = parse_masks(std::string(data.begin(), data.end()));
  // One R8 sheet: the maximum alpha of every valid plane's mask.
  bake::Texture sheet;
  sheet.name = "livery_mask";
  sheet.format = bake::TextureFormat::kR8;
  sheet.swizzle = {0, 0, 0, 5};
  bake::TextureLevel level;
  for (const auto& p : out.livery) {
    if (!p.valid) continue;
    const std::string file = "liverymasks/" + lower(p.name) + ".tga";
    if (!folder.contains(file)) {
      notes.push_back("livery plane " + p.name + " has no " + file);
      continue;
    }
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!folder.read(file, data, &error) || !decode_tga(data, w, h, rgba, &error)) {
      notes.push_back("livery " + file + ": " + error);
      continue;
    }
    if (level.data.empty()) {
      level.width = w;
      level.height = h;
      level.data.assign(size_t(w) * h, 0);
    }
    if (w != level.width || h != level.height) {
      notes.push_back("livery " + file + " is " + std::to_string(w) + "x" + std::to_string(h) +
                      ", not the sheet's size: left out");
      continue;
    }
    for (size_t i = 0; i < level.data.size(); ++i) level.data[i] = std::max(level.data[i], rgba[4 * i + 3]);
  }
  if (!level.data.empty()) {
    sheet.levels.push_back(std::move(level));
    out.textures.push_back(std::move(sheet));
  }
}

}  // namespace

MaterialClass classify(const std::string& technique, bool transparent, Role role) {
  const std::string t = lower(technique);
  if (t == "blur_rim" || t == "chrome_blur_rim") return MaterialClass::kHidden;
  if (transparent) return MaterialClass::kGlass;
  if (role == Role::kWheel) {
    if (t == "rim" || t == "inner_rim" || t == "outer_rim" || t == "wheel_black" || t == "blur_lip" ||
        t == "hub")
      return MaterialClass::kRim;
  }
  if (t == "tire" || t == "sidewall" || t == "tread") return MaterialClass::kTyre;
  if (t == "body" || t == "body_2") return MaterialClass::kPaint;
  if (t == "caliper") return MaterialClass::kCaliper;
  static const std::set<std::string> kLampExact = {"head_light", "hidhead", "oldhead", "xenonhead", "fullbeam",
                                                   "taillight2s", "taillightst", "drlwhite", "slorange",
                                                   "textured_reflector", "reflector"};
  if (kLampExact.count(t) || starts_with(t, "tail_light") || starts_with(t, "indicator_") ||
      starts_with(t, "reverse_light") || starts_with(t, "lights_gls_"))
    return MaterialClass::kLamp;
  static const std::set<std::string> kInterior = {"leather", "cloth", "plastic", "solid", "mottled",
                                                  "headliner", "bump_leather", "bump_plastic", "bump_stitching"};
  if (kInterior.count(t)) return MaterialClass::kInterior;
  if (t == "gauge_emissive" || t == "z_gauge_emissive_alpha" || t == "gauge_needle" || t == "digital_gauge")
    return MaterialClass::kGauge;
  if (starts_with(t, "chrome")) return MaterialClass::kChrome;
  if (t == "carbon_fiber") return MaterialClass::kCarbon;
  return MaterialClass::kOpaque;
}

bool decode_tga(const std::vector<uint8_t>& d, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba,
                std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (d.size() < 18) return fail("TGA too small");
  const uint8_t id_length = d[0], colour_map = d[1], type = d[2];
  width = uint32_t(d[12] | d[13] << 8);
  height = uint32_t(d[14] | d[15] << 8);
  const uint32_t bpp = d[16], descriptor = d[17];
  if (colour_map || (type != 2 && type != 10) || (bpp != 24 && bpp != 32))
    return fail("unsupported TGA (only true colour, 24 or 32 bits)");
  const size_t px = bpp / 8, count = size_t(width) * height;
  size_t at = 18 + id_length;
  std::vector<uint8_t> raw(count * 4);
  auto put = [&](size_t i, const uint8_t* p) {
    raw[4 * i] = p[2];
    raw[4 * i + 1] = p[1];
    raw[4 * i + 2] = p[0];
    raw[4 * i + 3] = px == 4 ? p[3] : 255;
  };
  if (type == 2) {
    if (at + count * px > d.size()) return fail("TGA truncated");
    for (size_t i = 0; i < count; ++i) put(i, &d[at + i * px]);
  } else {
    size_t i = 0;
    while (i < count) {
      if (at >= d.size()) return fail("TGA truncated");
      const uint8_t head = d[at++];
      const size_t n = (head & 0x7F) + 1u;
      if (i + n > count) return fail("TGA run overflows");
      if (head & 0x80) {
        if (at + px > d.size()) return fail("TGA truncated");
        for (size_t k = 0; k < n; ++k) put(i + k, &d[at]);
        at += px;
      } else {
        if (at + n * px > d.size()) return fail("TGA truncated");
        for (size_t k = 0; k < n; ++k) put(i + k, &d[at + k * px]);
        at += n * px;
      }
      i += n;
    }
  }
  // Rows are stored bottom first unless descriptor bit 5 is set.
  const bool top_first = (descriptor & 0x20) != 0;
  rgba.resize(raw.size());
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t src = top_first ? y : height - 1 - y;
    std::copy_n(&raw[size_t(src) * width * 4], size_t(width) * 4, &rgba[size_t(y) * width * 4]);
  }
  return true;
}

bool bake_car(const FolderRef& ref, const CarRow& row, bake::Bake& out, std::string* error) {
  out = bake::Bake{};
  out.kind = bake::Kind::kCar;
  Folder folder;
  if (!folder.open(ref, error)) return false;
  bake::CarInfo& c = out.car;
  std::vector<std::string>& notes = c.notes;
  c.media_name = row.media_name;
  c.display_name = row.display_name;
  c.origin = ref.origin;
  c.car_id = row.id;
  c.wheel_media = lower(row.wheel_media);
  c.tyre_media = kTyreModel;
  c.tyre_texture_media = kTyreTextures;
  c.tyre_width_mm = row.tyre_width_mm;
  c.tyre_aspect = row.tyre_aspect;
  c.rim_inches = row.rim_inches;
  c.ride_height = {float(row.ride_height[0]), float(row.ride_height[1])};
  c.caliper_rgb = {row.caliper_rgb_front, row.caliper_rgb_rear};
  c.caliper_angle = {float(row.caliper_angle_front), float(row.caliper_angle_rear)};
  c.has_livery = row.has_livery;
  c.wing_mask = row.wing_mask;
  for (size_t i = 0; i < 10; ++i)
    for (size_t a = 0; a < 3; ++a) c.part_positions[i][a] = float(row.part_positions[i][a]);
  // LF, RF, LR, RR. FORMAT-CARS reads PartId 5-8 with +z to the rear
  // (5 LR, 6 RR, 7 RF, 8 LF); the reference captures put the axles the
  // other way round (docs/SPEC_QUESTIONS.md Q9): z is negated as for the
  // lamp points, so 5 and 6 are the front pair.
  const size_t wheel_rows[4] = {5, 6, 8, 7};
  for (size_t w = 0; w < 4; ++w) {
    c.wheel_centres[w] = c.part_positions[wheel_rows[w]];
    c.wheel_centres[w][2] = -c.wheel_centres[w][2];
    if (!row.has_part_position[wheel_rows[w]]) notes.push_back("no CarPartPositions row " + std::to_string(wheel_rows[w]));
  }
  notes.push_back("tyre model " + std::string(kTyreModel) + " with " + kTyreTextures +
                  " textures (FORMAT-CARS: the tyre choice is not known)");

  std::set<std::string> own;
  for (const auto& n : folder.names())
    if (ends_with(n, ".xds")) own.insert(n);
  for (const CarColour& col : row.colours) {
    bake::Colour bc;
    bc.id = col.id;
    bc.sequence = col.sequence;
    bc.metallic = col.metallic;
    bc.rgb = col.rgb;
    std::string stem = lower(col.base_texture);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos) stem = stem.substr(0, dot);
    if (stem.empty()) stem = "nodamage";
    bc.atlas_lod0 = own.count(stem + "_lod0.xds") ? stem + "_lod0.xds" : own.count(stem + ".xds") ? stem + ".xds" : "";
    bc.atlas_lod = own.count(stem + ".xds") ? stem + ".xds" : "";
    if (bc.atlas_lod0.empty()) {
      notes.push_back("colour " + std::to_string(col.id) + ": no atlas for " + col.base_texture + "; nodamage_lod0.xds");
      bc.atlas_lod0 = own.count("nodamage_lod0.xds") ? "nodamage_lod0.xds" : "";
    }
    if (bc.atlas_lod.empty()) bc.atlas_lod = own.count("nodamage.xds") ? "nodamage.xds" : bc.atlas_lod0;
    if (bc.atlas_lod0 == stem + ".xds" && stem != "nodamage")
      notes.push_back("colour " + std::to_string(col.id) + ": " + col.base_texture + " has no _lod0 atlas; " +
                      stem + ".xds for LOD0");
    c.colours.push_back(bc);
  }
  if (c.colours.empty()) notes.push_back("no Combo_Colors row");
  else if (c.colours[0].metallic) notes.push_back("first colour is metallic: engine finish");

  for (const std::string& name : folder.names()) {
    if (name.find('/') != std::string::npos || !ends_with(name, ".carbin") || starts_with(name, "stripped_")) continue;
    bake::Corner corner;
    const Role role = role_of(name, corner);
    bake::Model model;
    std::string why;
    if (!parse_model(folder, name, role, corner, own, {}, model, notes, &why)) {
      if (error) *error = ref.name + "/" + why;
      return false;
    }
    if (model.parts.empty()) notes.push_back(name + " has no parts");
    out.models.push_back(std::move(model));
  }
  bake_textures(folder, out, &notes);
  bake_livery(folder, out, notes);
  std::sort(notes.begin(), notes.end());
  notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
  return true;
}

bool bake_wheel(const FolderRef& ref, bool tyre, bake::Bake& out, std::string* error) {
  out = bake::Bake{};
  out.kind = bake::Kind::kWheel;
  Folder folder;
  if (!folder.open(ref, error)) return false;
  std::set<std::string> own;
  for (const auto& n : folder.names())
    if (ends_with(n, ".xds")) own.insert(n);
  std::vector<std::string> notes;
  for (const std::string& name : folder.names()) {
    if (name.find('/') != std::string::npos || !ends_with(name, ".carbin") || starts_with(name, "stripped_")) continue;
    bake::Model model;
    if (!parse_model(folder, name, tyre ? Role::kTyre : Role::kWheel, bake::Corner::kNone, own, {}, model, notes,
                     error))
      return false;
    out.models.push_back(std::move(model));
  }
  bake_textures(folder, out, &notes);
  return true;
}

bool bake_shared(const FolderRef& ref, bake::Bake& out, std::string* error) {
  out = bake::Bake{};
  out.kind = bake::Kind::kShared;
  Folder folder;
  if (!folder.open(ref, error)) return false;
  bake_textures(folder, out, nullptr);
  return true;
}

}  // namespace cars
