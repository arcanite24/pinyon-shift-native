#include "freeroam/outposts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "audio/xml.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"

namespace freeroam {
namespace {

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string resolve_str(const fh1::StringTable* tbl, std::string_view key) {
  if (!tbl || key.empty()) return {};
  const auto* u16 = tbl->find(fh1::string_hash(key));
  if (!u16) return {};
  return fh1::utf16_to_utf8(*u16);
}

std::array<double, 3> axis_from(const audio::XmlNode* n, std::array<double, 3> fallback) {
  if (!n) return fallback;
  return {n->number("x"), n->number("y"), n->number("z")};
}

// Recursive search for the first <Behaviour id="..."> inside an <Activity>.
const audio::XmlNode* find_behaviour(const audio::XmlNode& node, std::string_view id) {
  if (audio::iequals(node.name, "Behaviour") && audio::iequals(node.attr("id"), id)) return &node;
  for (const auto& c : node.children) {
    if (const auto* hit = find_behaviour(c, id)) return hit;
  }
  return nullptr;
}

int parse_trailing_number(std::string_view s) {
  size_t i = s.size();
  while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) --i;
  if (i == s.size()) return 0;
  return std::atoi(std::string(s.substr(i)).c_str());
}

// Minimal <NamedTransform name='...'><Transform pos.x='...' .../></NamedTransform>
// parser supporting both single and double quotes (same format as TrackRouteXXX.xml).
std::map<std::string, Placement> parse_route_transforms(std::string_view xml) {
  std::map<std::string, Placement> out;
  constexpr std::string_view kNamed = "<NamedTransform";
  constexpr std::string_view kTransform = "<Transform";
  constexpr std::string_view kClose = "</NamedTransform>";

  auto parse_attrs = [](std::string_view s, size_t at, size_t& end) {
    std::vector<std::pair<std::string_view, std::string_view>> attrs;
    size_t i = at;
    while (i < s.size() && s[i] != '>') {
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == '/')) ++i;
      if (i >= s.size() || s[i] == '>') break;
      const size_t n0 = i;
      while (i < s.size() && s[i] != '=' && s[i] != '>' && s[i] != ' ') ++i;
      const std::string_view name = s.substr(n0, i - n0);
      if (i >= s.size() || s[i] != '=') continue;
      ++i;
      if (i >= s.size() || (s[i] != '\'' && s[i] != '"')) continue;
      const char q = s[i++];
      const size_t v0 = i;
      while (i < s.size() && s[i] != q) ++i;
      attrs.emplace_back(name, s.substr(v0, i - v0));
      if (i < s.size()) ++i;
    }
    end = i;
    return attrs;
  };

  auto num = [](std::string_view v) { return std::strtod(std::string(v).c_str(), nullptr); };

  size_t at = 0;
  while ((at = xml.find(kNamed, at)) != std::string_view::npos) {
    const size_t after = at + kNamed.size();
    if (after < xml.size() && xml[after] != ' ' && xml[after] != '\t' && xml[after] != '\r' && xml[after] != '\n') {
      at = after;
      continue;
    }
    size_t end = 0;
    std::string name;
    for (const auto& [k, v] : parse_attrs(xml, after, end)) {
      if (k == "name") name = std::string(v);
    }
    const size_t close = xml.find(kClose, end);
    const size_t tr = xml.find(kTransform, end);
    if (!name.empty() && tr != std::string_view::npos && (close == std::string_view::npos || tr < close)) {
      size_t tend = 0;
      Placement p;
      double fx = 0.0, fz = 1.0;
      for (const auto& [k, v] : parse_attrs(xml, tr + kTransform.size(), tend)) {
        if (k == "pos.x") p.x = num(v);
        else if (k == "pos.y") p.y = num(v);
        else if (k == "pos.z") p.z = num(v);
        else if (k == "facing.x") fx = num(v);
        else if (k == "facing.z") fz = num(v);
      }
      const double fl = std::hypot(fx, fz);
      if (fl > 1e-6) {
        fx /= fl;
        fz /= fl;
        p.z_axis = {fx, 0.0, fz};
        p.x_axis = {fz, 0.0, -fx};
      }
      out.emplace(lower(name), p);
    }
    at = close == std::string_view::npos ? end : close + kClose.size();
  }
  return out;
}

bool segments_intersect_2d(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz) {
  auto cross = [](double ux, double uz, double vx, double vz) { return ux * vz - uz * vx; };
  const double r_x = bx - ax, r_z = bz - az;
  const double s_x = dx - cx, s_z = dz - cz;
  const double denom = cross(r_x, r_z, s_x, s_z);
  if (std::fabs(denom) < 1e-9) return false;
  const double qp_x = cx - ax, qp_z = cz - az;
  const double t = cross(qp_x, qp_z, s_x, s_z) / denom;
  const double u = cross(qp_x, qp_z, r_x, r_z) / denom;
  return t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0;
}

std::string format_int_commas(int64_t n) {
  std::string raw = std::to_string(std::max<int64_t>(0, n));
  std::string out;
  out.reserve(raw.size() + raw.size() / 3);
  for (size_t i = 0; i < raw.size(); ++i) {
    if (i > 0 && (raw.size() - i) % 3 == 0) out.push_back(',');
    out.push_back(raw[i]);
  }
  return out;
}

bool load_str_from_zip(fh1::Archive& zip, std::string_view entry, fh1::StringTable& out) {
  auto bytes = zip.read(entry);
  if (!bytes.ok()) return false;
  return fh1::parse_string_table(bytes.data, out, nullptr);
}

void merge_str_from_zip(fh1::Archive& zip, std::string_view entry, fh1::StringTable& dest) {
  fh1::StringTable tmp;
  if (!load_str_from_zip(zip, entry, tmp)) return;
  if (dest.keys.empty()) {
    dest = std::move(tmp);
    return;
  }
  // Merge sorted key/text tables (earlier entries win on key collision).
  fh1::StringTable merged;
  merged.keys.reserve(dest.keys.size() + tmp.keys.size());
  merged.text.reserve(dest.text.size() + tmp.text.size());
  size_t i = 0, j = 0;
  while (i < dest.keys.size() || j < tmp.keys.size()) {
    if (j == tmp.keys.size() || (i < dest.keys.size() && dest.keys[i] < tmp.keys[j])) {
      merged.keys.push_back(dest.keys[i]);
      merged.text.push_back(std::move(dest.text[i]));
      ++i;
    } else if (i == dest.keys.size() || tmp.keys[j] < dest.keys[i]) {
      merged.keys.push_back(tmp.keys[j]);
      merged.text.push_back(std::move(tmp.text[j]));
      ++j;
    } else {
      merged.keys.push_back(dest.keys[i]);
      merged.text.push_back(std::move(dest.text[i]));
      ++i;
      ++j;
    }
  }
  dest = std::move(merged);
}

}  // namespace

const Outpost* OutpostCatalog::find_outpost(std::string_view id) const {
  const std::string key = lower(std::string(id));
  for (const auto& o : outposts) {
    if (lower(o.id) == key) return &o;
  }
  return nullptr;
}

const PrStunt* OutpostCatalog::find_stunt(std::string_view id) const {
  const std::string key = lower(std::string(id));
  for (const auto& s : stunts) {
    if (lower(s.id) == key) return &s;
  }
  return nullptr;
}

std::vector<const PrStunt*> OutpostCatalog::stunts_for_outpost(std::string_view outpost_id) const {
  std::vector<const PrStunt*> out;
  const std::string key = lower(std::string(outpost_id));
  for (const auto& s : stunts) {
    if (lower(s.outpost_id) == key) out.push_back(&s);
  }
  return out;
}

bool parse_outposts(std::string_view gas_stations_xml, std::string_view speed_stunts_xml,
                    std::string_view pr_stunts_xml, std::string_view photo_shoots_xml,
                    std::string_view game_objs_xml, const std::map<int, std::string>& route_xmls,
                    const fh1::StringTable* gas_strings, const fh1::StringTable* mission_strings,
                    const fh1::StringTable* camera_strings, OutpostCatalog& out, std::string* error) {
  out = OutpostCatalog{};
  audio::XmlNode gas_node, speed_node, pr_node, photo_node, objs_node;
  std::string why;
  if (!audio::parse_xml(gas_stations_xml, gas_node, &why)) return fail(error, "gas_stations.xml: " + why);
  if (!audio::parse_xml(game_objs_xml, objs_node, &why)) return fail(error, "GameObjs.xml: " + why);
  if (!speed_stunts_xml.empty() && !audio::parse_xml(speed_stunts_xml, speed_node, &why))
    return fail(error, "mission_speedstunts.xml: " + why);
  if (!pr_stunts_xml.empty() && !audio::parse_xml(pr_stunts_xml, pr_node, &why))
    return fail(error, "mission_prstunts.xml: " + why);
  if (!photo_shoots_xml.empty() && !audio::parse_xml(photo_shoots_xml, photo_node, &why))
    return fail(error, "mission_photoshoots.xml: " + why);

  // Index GameObjs.xml by lowercase GameplayID.
  std::map<std::string, Placement> placed;
  for (const auto& o : objs_node.children) {
    if (!o.has("GameplayID")) continue;
    Placement p;
    if (const auto* pos = o.child("Pos")) {
      p.x = pos->number("x");
      p.y = pos->number("y");
      p.z = pos->number("z");
    }
    if (const auto* r = o.child("Orientation")) {
      p.x_axis = axis_from(r->child("XAxis"), p.x_axis);
      p.y_axis = axis_from(r->child("YAxis"), p.y_axis);
      p.z_axis = axis_from(r->child("ZAxis"), p.z_axis);
    }
    placed.emplace(lower(o.attr("GameplayID")), p);
  }

  // Parse any supplied TrackRouteXXX.xml transforms.
  std::map<int, std::map<std::string, Placement>> route_transforms;
  for (const auto& [rid, rxml] : route_xmls) {
    route_transforms.emplace(rid, parse_route_transforms(rxml));
  }

  // Map mission_id -> parent outpost_id.
  std::map<std::string, std::string> mission_to_outpost;

  // 1. Outposts (ActivityGasStation).
  for (const auto& a : gas_node.children) {
    if (!audio::iequals(a.name, "Activity") || !audio::iequals(a.attr("type"), "ActivityGasStation")) continue;
    Outpost o;
    o.id = a.attr("name");
    o.index = parse_trailing_number(o.id);
    const auto* tz = a.child("TriggerZone");
    if (!tz) continue;
    const std::string trigger_obj = lower(tz->attr("object"));
    auto it_trig = placed.find(trigger_obj);
    if (it_trig == placed.end()) return fail(error, "outpost " + o.id + " trigger " + trigger_obj + " not in GameObjs");
    o.trigger = it_trig->second;
    o.trigger_radius = tz->number("radius", 50.0);
    o.discovery_radius = tz->number("triggerZoneRadius", std::max(100.0, o.trigger_radius * 2.0));
    o.name_key = tz->attr("prompt");
    if (o.name_key.empty()) o.name_key = tz->attr("mapDescription");

    // Discovery object (GASSTATION_XXX_DISCOVERY) if present, else trigger.
    auto it_disc = placed.find(trigger_obj + "_discovery");
    o.discovery = (it_disc != placed.end()) ? it_disc->second : o.trigger;

    // Spawn/fast-travel landing node from CPlaceCarAtObject.
    o.spawn = o.trigger;
    if (const auto* place_b = find_behaviour(a, "CPlaceCarAtObject")) {
      const std::string spawn_obj = lower(place_b->attr("object_name"));
      if (auto it_sp = placed.find(spawn_obj); it_sp != placed.end()) o.spawn = it_sp->second;
    }

    // Localized Outpost display name from GasStation.str (or clean fallback).
    o.display_name = resolve_str(gas_strings, o.name_key);
    if (o.display_name.empty()) {
      o.display_name = "Horizon Outpost #" + std::to_string(o.index > 0 ? o.index : int(out.outposts.size() + 1));
    }

    if (const auto* missions = a.child("Missions")) {
      for (const auto& m : missions->children) {
        if (!audio::iequals(m.name, "Mission")) continue;
        const std::string mid = m.attr("name");
        if (!mid.empty()) {
          o.mission_ids.push_back(mid);
          mission_to_outpost[lower(mid)] = o.id;
        }
      }
    }
    out.outposts.push_back(std::move(o));
  }

  auto lookup_outpost_spawn = [&](const std::string& outpost_id) -> Placement {
    if (const Outpost* op = out.find_outpost(outpost_id)) return op->spawn;
    return Placement{};
  };

  auto extract_route_id = [](const audio::XmlNode& act) -> int {
    if (const auto* load_r = find_behaviour(act, "CLoadRoute")) {
      for (const auto& c : load_r->children) {
        if (audio::iequals(c.name, "Attribute") && audio::iequals(c.attr("id"), "route_id")) {
          return int(c.number("value", 0));
        }
      }
    }
    return 0;
  };

  auto extract_start_pos = [&](int route_id, const std::string& outpost_id) -> Placement {
    if (auto it = route_transforms.find(route_id); it != route_transforms.end()) {
      if (auto ms = it->second.find("mission_start_01"); ms != it->second.end()) return ms->second;
      if (auto sl = it->second.find("start_location_00"); sl != it->second.end()) return sl->second;
    }
    return lookup_outpost_spawn(outpost_id);
  };

  // 2. Speed Stunts (ActivitySpeedStunt).
  for (const auto& a : speed_node.children) {
    if (!audio::iequals(a.name, "Activity") || !audio::iequals(a.attr("type"), "ActivitySpeedStunt")) continue;
    PrStunt s;
    s.id = a.attr("name");
    s.kind = PrStuntKind::kSpeed;
    if (auto it = mission_to_outpost.find(lower(s.id)); it != mission_to_outpost.end()) s.outpost_id = it->second;
    s.route_id = extract_route_id(a);
    s.start_pos = extract_start_pos(s.route_id, s.outpost_id);
    if (const auto* sp = a.child("Speed")) s.target_mph = sp->number("easy", 120.0);
    if (const auto* tm = a.child("Time")) s.time_limit_s = tm->number("easy", 90.0);
    if (const auto* car = a.child("Car")) {
      s.loaner_car_id = int(car->number("carID", 0));
      s.loaner_car_colour = int(car->number("carColour", 0));
    }

    std::string cam_name;
    if (const auto* trap = a.child("SpeedTrap")) {
      const std::string cam_id = lower(trap->attr("name"));
      cam_name = resolve_str(camera_strings, trap->attr("nameDesc"));
      auto it_l = placed.find(cam_id + "_left");
      auto it_r = placed.find(cam_id + "_right");
      if (it_l != placed.end() && it_r != placed.end()) {
        s.gate_left = it_l->second;
        s.gate_right = it_r->second;
        s.has_gate = true;
        s.target_pos.x = 0.5 * (s.gate_left.x + s.gate_right.x);
        s.target_pos.y = 0.5 * (s.gate_left.y + s.gate_right.y);
        s.target_pos.z = 0.5 * (s.gate_left.z + s.gate_right.z);
      } else {
        s.target_pos = s.start_pos;
      }
    }
    if (const auto* desc = a.child("Descriptions")) {
      s.instruction = resolve_str(mission_strings, desc->attr("instruction_text"));
    }
    const int num = parse_trailing_number(s.id);
    s.title = cam_name.empty() ? ("Speed Stunt #" + std::to_string(num) + " (" + std::to_string(int(std::lround(s.target_mph))) + " MPH)")
                               : ("Speed Trap: " + cam_name);
    if (s.instruction.empty()) {
      s.instruction = "Hit " + std::to_string(int(std::lround(s.target_mph))) + " MPH through the speed trap";
    }
    s.reward_credits = 5000 + int64_t(std::max(0, num - 1)) * 500;
    s.reward_popularity = 2500;
    out.stunts.push_back(std::move(s));
  }

  // 3. Skill / Drift PR Stunts (ActivityPRStunt).
  for (const auto& a : pr_node.children) {
    if (!audio::iequals(a.name, "Activity") || !audio::iequals(a.attr("type"), "ActivityPRStunt")) continue;
    PrStunt s;
    s.id = a.attr("name");
    s.kind = PrStuntKind::kSkill;
    if (auto it = mission_to_outpost.find(lower(s.id)); it != mission_to_outpost.end()) s.outpost_id = it->second;
    s.route_id = extract_route_id(a);
    s.start_pos = extract_start_pos(s.route_id, s.outpost_id);
    s.target_pos = s.start_pos;
    if (const auto* dest = find_behaviour(a, "CSetSatNavDestinationPos")) {
      s.target_pos.x = dest->number("x", s.start_pos.x);
      s.target_pos.y = dest->number("y", s.start_pos.y);
      s.target_pos.z = dest->number("z", s.start_pos.z);
    }
    if (const auto* pop = a.child("Popularity")) s.target_points = int(pop->number("easy", 15000));
    if (const auto* tm = a.child("TimeLimit")) s.time_limit_s = tm->number("easy", 120.0);
    if (const auto* car = a.child("Car")) {
      s.loaner_car_id = int(car->number("carID", 0));
      s.loaner_car_colour = int(car->number("carColour", 0));
    }
    if (const auto* desc = a.child("Descriptions")) {
      s.instruction = resolve_str(mission_strings, desc->attr("instruction_in_skills"));
      if (s.instruction.empty()) s.instruction = resolve_str(mission_strings, desc->attr("instruction_to_skills"));
    }
    const int num = parse_trailing_number(s.id);
    s.title = "Skill Stunt #" + std::to_string(num) + " (" + format_int_commas(s.target_points) + " PTS)";
    if (s.instruction.empty()) {
      s.instruction = "Earn " + format_int_commas(s.target_points) + " skill points before time expires";
    }
    s.reward_credits = 6000 + int64_t(std::max(0, num - 1)) * 500;
    s.reward_popularity = 3000;
    out.stunts.push_back(std::move(s));
  }

  // 4. Photo Op PR Stunts (ActivityPhotoShoot).
  for (const auto& a : photo_node.children) {
    if (!audio::iequals(a.name, "Activity") || !audio::iequals(a.attr("type"), "ActivityPhotoShoot")) continue;
    PrStunt s;
    s.id = a.attr("name");
    s.kind = PrStuntKind::kPhoto;
    if (auto it = mission_to_outpost.find(lower(s.id)); it != mission_to_outpost.end()) s.outpost_id = it->second;
    s.route_id = extract_route_id(a);
    s.start_pos = extract_start_pos(s.route_id, s.outpost_id);
    s.target_pos = s.start_pos;
    s.photo_radius = 50.0;
    s.time_limit_s = 180.0;  // generous 3-minute photo op timer
    if (const auto* pz = a.child("PhotoZone")) {
      s.photo_radius = pz->number("radius", 50.0);
      const std::string node_name = lower(pz->attr("node_name"));
      if (auto it_r = route_transforms.find(s.route_id); it_r != route_transforms.end()) {
        if (auto it_n = it_r->second.find(node_name); it_n != it_r->second.end()) {
          s.target_pos = it_n->second;
        }
      }
    }
    if (const auto* car = a.child("Car")) {
      s.loaner_car_id = int(car->number("carID", 0));
      s.loaner_car_colour = int(car->number("carColour", 0));
    }
    std::string location;
    if (const auto* desc = a.child("Descriptions")) {
      location = resolve_str(mission_strings, desc->attr("instruction_location"));
      s.instruction = resolve_str(mission_strings, desc->attr("instruction_to_photo"));
    }
    const int num = parse_trailing_number(s.id);
    s.title = location.empty() ? ("Photo Op #" + std::to_string(num)) : ("Photo Op: " + location);
    if (s.instruction.empty()) {
      s.instruction = "Drive to the landmark and press [P] to take a photo";
    }
    s.reward_credits = 4500 + int64_t(std::max(0, num - 1)) * 500;
    s.reward_popularity = 2000;
    out.stunts.push_back(std::move(s));
  }

  if (out.outposts.empty()) return fail(error, "gas_stations.xml lists no Outposts");
  return true;
}

bool load_outposts(const std::filesystem::path& game_root, OutpostCatalog& out, std::string* error) {
  const auto media = game_root / "media";
  std::string why;
  auto modes = fh1::Archive::open(media / "gamemodes.zip", &why);
  if (!modes) return fail(error, "gamemodes.zip: " + why);

  auto gas = modes->read("Colorado/gas_stations.xml");
  if (!gas.ok()) return fail(error, "gamemodes.zip Colorado/gas_stations.xml: " + gas.error);
  auto speed = modes->read("Colorado/mission_speedstunts.xml");
  auto pr = modes->read("Colorado/mission_prstunts.xml");
  auto photo = modes->read("Colorado/mission_photoshoots.xml");

  const auto objs_path = media / "tracks" / "colorado" / "Ribbon_00" / "GameObjs.xml";
  std::ifstream f(objs_path, std::ios::binary);
  if (!f) return fail(error, "cannot read " + objs_path.string());
  std::stringstream objs;
  objs << f.rdbuf();

  auto sv = [](const std::vector<uint8_t>& d) {
    return std::string_view(reinterpret_cast<const char*>(d.data()), d.size());
  };

  // Pre-scan referenced route_ids from the three stunt XMLs so we load only the needed TrackRouteXXX.xml files.
  std::map<int, std::string> route_xmls;
  auto collect_routes = [&](std::string_view xml) {
    if (xml.empty()) return;
    audio::XmlNode root;
    if (!audio::parse_xml(xml, root, nullptr)) return;
    for (const auto& a : root.children) {
      if (const auto* load_r = find_behaviour(a, "CLoadRoute")) {
        for (const auto& c : load_r->children) {
          if (audio::iequals(c.name, "Attribute") && audio::iequals(c.attr("id"), "route_id")) {
            const int rid = int(c.number("value", 0));
            if (rid > 0 && !route_xmls.count(rid)) {
              char fn[40];
              std::snprintf(fn, sizeof fn, "TrackRoute%03d.xml", rid);
              const auto rpath = media / "tracks" / "colorado" / "Ribbon_00" / fn;
              std::ifstream rf(rpath, std::ios::binary);
              if (rf) {
                std::stringstream rss;
                rss << rf.rdbuf();
                route_xmls.emplace(rid, rss.str());
              }
            }
          }
        }
      }
    }
  };
  if (speed.ok()) collect_routes(sv(speed.data));
  if (pr.ok()) collect_routes(sv(pr.data));
  if (photo.ok()) collect_routes(sv(photo.data));

  // Optional localized strings from EN.zip.
  fh1::StringTable gas_str, mission_str, cam_str;
  if (auto en = fh1::Archive::open(media / "stringtables" / "EN.zip", nullptr)) {
    for (const char* tbl : {"GasStation.str", "Map.str", "Activities.str", "InGame.str", "GameStrings.str",
                            "Missions.str", "EventHubs.str"}) {
      merge_str_from_zip(*en, tbl, gas_str);
    }
    for (const char* tbl : {"Missions.str", "Activities.str", "InGame.str", "GasStation.str"}) {
      merge_str_from_zip(*en, tbl, mission_str);
    }
    load_str_from_zip(*en, "List_SpeedCameras.str", cam_str);
  }

  return parse_outposts(sv(gas.data), speed.ok() ? sv(speed.data) : std::string_view{},
                        pr.ok() ? sv(pr.data) : std::string_view{}, photo.ok() ? sv(photo.data) : std::string_view{},
                        objs.str(), route_xmls, gas_str.keys.empty() ? nullptr : &gas_str,
                        mission_str.keys.empty() ? nullptr : &mission_str, cam_str.keys.empty() ? nullptr : &cam_str,
                        out, error);
}

bool OutpostProgress::is_outpost_discovered(std::string_view id) const {
  return discovered_outposts.count(std::string(id)) > 0;
}

bool OutpostProgress::is_stunt_completed(std::string_view id) const {
  return completed_pr_stunts.count(std::string(id)) > 0;
}

bool OutpostProgress::discover_outpost(const std::string& id) {
  if (id.empty()) return false;
  return discovered_outposts.insert(id).second;
}

bool OutpostProgress::complete_stunt(const std::string& id) {
  if (id.empty()) return false;
  return completed_pr_stunts.insert(id).second;
}

int OutpostProgress::check_discovery(const OutpostCatalog& catalog, double car_x, double car_z) {
  for (size_t i = 0; i < catalog.outposts.size(); ++i) {
    const Outpost& o = catalog.outposts[i];
    if (is_outpost_discovered(o.id)) continue;
    const double d_disc = std::hypot(o.discovery.x - car_x, o.discovery.z - car_z);
    const double d_trig = std::hypot(o.trigger.x - car_x, o.trigger.z - car_z);
    if (d_disc <= o.discovery_radius || d_trig <= o.trigger_radius) {
      discovered_outposts.insert(o.id);
      return int(i);
    }
  }
  return -1;
}

int OutpostProgress::discovered_count(const OutpostCatalog& catalog) const {
  int n = 0;
  for (const auto& o : catalog.outposts) {
    if (is_outpost_discovered(o.id)) ++n;
  }
  return n;
}

int OutpostProgress::completed_stunt_count(const OutpostCatalog& catalog) const {
  int n = 0;
  for (const auto& s : catalog.stunts) {
    if (is_stunt_completed(s.id)) ++n;
  }
  return n;
}

void PrStuntSession::start(const PrStunt& stunt, int64_t baseline_skill_points) {
  stunt_ = stunt;
  state_ = PrStuntState::kActive;
  baseline_skill_points_ = baseline_skill_points;
  earned_skill_points_ = 0;
  time_remaining_s_ = stunt.time_limit_s;
  elapsed_s_ = 0.0;
  best_mph_ = 0.0;
  distance_to_target_ = std::hypot(stunt.target_pos.x - stunt.start_pos.x, stunt.target_pos.z - stunt.start_pos.z);
  photo_hold_s_ = 0.0;
  in_target_zone_ = false;
}

void PrStuntSession::cancel() {
  state_ = PrStuntState::kInactive;
  in_target_zone_ = false;
}

PrStuntState PrStuntSession::update(const PrStuntUpdate& u) {
  if (state_ != PrStuntState::kActive) return state_;
  const double dt = std::max(0.0, u.dt);
  elapsed_s_ += dt;
  if (stunt_.time_limit_s > 0.0) {
    time_remaining_s_ = std::max(0.0, time_remaining_s_ - dt);
  }
  distance_to_target_ = std::hypot(stunt_.target_pos.x - u.car_x, stunt_.target_pos.z - u.car_z);

  switch (stunt_.kind) {
    case PrStuntKind::kSpeed: {
      best_mph_ = std::max(best_mph_, u.speed_mph);
      bool hit_trap = distance_to_target_ <= 28.0;
      if (!hit_trap && stunt_.has_gate) {
        // Widen gate endpoints slightly (+6m) so shoulder approaches count fairly.
        const double gx = stunt_.gate_right.x - stunt_.gate_left.x;
        const double gz = stunt_.gate_right.z - stunt_.gate_left.z;
        const double gl = std::hypot(gx, gz);
        double ax = stunt_.gate_left.x, az = stunt_.gate_left.z;
        double bx = stunt_.gate_right.x, bz = stunt_.gate_right.z;
        if (gl > 1e-3) {
          ax -= (gx / gl) * 6.0;
          az -= (gz / gl) * 6.0;
          bx += (gx / gl) * 6.0;
          bz += (gz / gl) * 6.0;
        }
        hit_trap = segments_intersect_2d(u.prev_x, u.prev_z, u.car_x, u.car_z, ax, az, bx, bz);
      }
      in_target_zone_ = distance_to_target_ <= 120.0;
      if (hit_trap && u.speed_mph >= stunt_.target_mph) {
        state_ = PrStuntState::kSucceeded;
        return state_;
      }
      break;
    }

    case PrStuntKind::kSkill: {
      in_target_zone_ = distance_to_target_ <= 350.0;
      const int64_t delta = std::max<int64_t>(0, u.total_skill_points - baseline_skill_points_);
      earned_skill_points_ = std::max(earned_skill_points_, delta);
      if (stunt_.target_points > 0 && earned_skill_points_ >= stunt_.target_points) {
        state_ = PrStuntState::kSucceeded;
        return state_;
      }
      break;
    }

    case PrStuntKind::kPhoto: {
      const double effective_r = std::max(55.0, stunt_.photo_radius);
      in_target_zone_ = distance_to_target_ <= effective_r;
      if (in_target_zone_) {
        if (u.photo_pressed) {
          state_ = PrStuntState::kSucceeded;
          return state_;
        }
        if (u.speed_mph < 8.0) {
          photo_hold_s_ += dt;
          if (photo_hold_s_ >= 1.5) {
            state_ = PrStuntState::kSucceeded;
            return state_;
          }
        } else {
          photo_hold_s_ = 0.0;
        }
      } else {
        photo_hold_s_ = 0.0;
      }
      break;
    }
  }

  if (stunt_.time_limit_s > 0.0 && time_remaining_s_ <= 0.0) {
    state_ = PrStuntState::kFailed;
  }
  return state_;
}

double PrStuntSession::progress_fraction() const {
  switch (stunt_.kind) {
    case PrStuntKind::kSpeed:
      return stunt_.target_mph > 0.0 ? std::clamp(best_mph_ / stunt_.target_mph, 0.0, 1.0) : 0.0;
    case PrStuntKind::kSkill:
      return stunt_.target_points > 0 ? std::clamp(double(earned_skill_points_) / double(stunt_.target_points), 0.0, 1.0) : 0.0;
    case PrStuntKind::kPhoto:
      if (state_ == PrStuntState::kSucceeded) return 1.0;
      return in_target_zone_ ? std::clamp(0.5 + 0.5 * (photo_hold_s_ / 1.5), 0.5, 1.0)
                             : std::clamp(1.0 - distance_to_target_ / 1500.0, 0.0, 0.5);
  }
  return 0.0;
}

std::string PrStuntSession::status_line() const {
  char buf[160];
  switch (stunt_.kind) {
    case PrStuntKind::kSpeed:
      std::snprintf(buf, sizeof buf, "BEST %.0f / %.0f MPH   •   %.0f M TO TRAP", best_mph_, stunt_.target_mph,
                    distance_to_target_);
      return buf;
    case PrStuntKind::kSkill:
      std::snprintf(buf, sizeof buf, "%s / %s SKILL PTS%s", format_int_commas(earned_skill_points_).c_str(),
                    format_int_commas(stunt_.target_points).c_str(),
                    in_target_zone_ ? "   •   IN STUNT ZONE" : "");
      return buf;
    case PrStuntKind::kPhoto:
      if (in_target_zone_) return "IN PHOTO ZONE   •   PRESS [P] OR STOP TO SNAP PHOTO";
      std::snprintf(buf, sizeof buf, "DRIVE TO LANDMARK   •   %.0f M AWAY", distance_to_target_);
      return buf;
  }
  return {};
}

const char* pr_stunt_kind_name(PrStuntKind kind) {
  switch (kind) {
    case PrStuntKind::kSpeed: return "SPEED STUNT";
    case PrStuntKind::kSkill: return "SKILL STUNT";
    case PrStuntKind::kPhoto: return "PHOTO OP";
  }
  return "PR STUNT";
}

}  // namespace freeroam
