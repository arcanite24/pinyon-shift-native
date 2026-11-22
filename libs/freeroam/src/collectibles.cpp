#include "freeroam/collectibles.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

#include "audio/xml.h"
#include "fh1data/archive.h"

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

std::array<double, 3> axis(const audio::XmlNode* n, std::array<double, 3> fallback) {
  if (!n) return fallback;
  return {n->number("x"), n->number("y"), n->number("z")};
}

}  // namespace

bool parse_collectibles(std::string_view flyers_xml, std::string_view barns_xml, std::string_view game_objs_xml,
                        Collectibles& out, std::string* error) {
  out = Collectibles{};
  audio::XmlNode flyers, barns, objs;
  std::string why;
  if (!audio::parse_xml(flyers_xml, flyers, &why)) return fail(error, "Flyers.xml: " + why);
  if (!audio::parse_xml(barns_xml, barns, &why)) return fail(error, "barnfinds.xml: " + why);
  if (!audio::parse_xml(game_objs_xml, objs, &why)) return fail(error, "GameObjs.xml: " + why);

  std::map<std::string, Placement> placed;  // lower-case GameplayID
  for (const auto& o : objs.children) {
    if (!o.has("GameplayID")) continue;
    Placement p;
    if (const auto* pos = o.child("Pos")) p.x = pos->number("x"), p.y = pos->number("y"), p.z = pos->number("z");
    if (const auto* r = o.child("Orientation")) {
      p.x_axis = axis(r->child("XAxis"), p.x_axis);
      p.y_axis = axis(r->child("YAxis"), p.y_axis);
      p.z_axis = axis(r->child("ZAxis"), p.z_axis);
    }
    placed.emplace(lower(o.attr("GameplayID")), p);
  }

  for (const auto& a : flyers.children) {
    if (!audio::iequals(a.name, "Activity") || !audio::iequals(a.attr("type"), "ActivityFlyers")) continue;
    Sign s;
    s.id = lower(a.attr("name"));
    auto it = placed.find(s.id);
    if (it == placed.end()) return fail(error, "sign " + s.id + " is not placed in GameObjs.xml");
    s.at = it->second;
    out.signs.push_back(std::move(s));
  }

  for (const auto& a : barns.children) {
    if (!audio::iequals(a.name, "Activity") || !audio::iequals(a.attr("type"), "ActivityBarnFind")) continue;
    Barn b;
    b.name = a.attr("name");
    const auto* trigger = a.child("TriggerZone");
    const auto* hint = a.child("HintRegion");
    const auto* doors = a.child("DoorInfo");
    const auto* car = a.child("UnlockCar");
    if (!trigger || !hint || !doors || !car) return fail(error, "barn find " + b.name + " lacks an element");
    b.trigger = lower(trigger->attr("object"));
    auto it = placed.find(b.trigger);
    if (it == placed.end()) return fail(error, "barn find object " + b.trigger + " is not placed in GameObjs.xml");
    b.at = it->second;
    b.radius = trigger->number("radius", 10);
    b.max_mph = trigger->number("maxMPH", 50);
    // x_offset east (+X), y_offset north (+Z): the spec's probable sign.
    b.hint_x = b.at.x + hint->number("x_offset");
    b.hint_z = b.at.z + hint->number("y_offset");
    b.hint_rx = hint->number("x_radius");
    b.hint_rz = hint->number("y_radius", b.hint_rx);
    b.open_id = lower(doors->attr("open_id"));
    b.closed_id = lower(doors->attr("closed_id"));
    b.open_coll_id = lower(doors->attr("open_coll_id"));
    b.closed_coll_id = lower(doors->attr("closed_coll_id"));
    b.car_id = int(car->number("id"));
    b.car_name_key = car->attr("stringTableNameId");
    out.barns.push_back(std::move(b));
  }
  if (out.signs.empty()) return fail(error, "Flyers.xml lists no signs");
  return true;
}

bool load_collectibles(const std::filesystem::path& game_root, Collectibles& out, std::string* error) {
  const auto media = game_root / "media";
  std::string why;
  auto modes = fh1::Archive::open(media / "gamemodes.zip", &why);
  if (!modes) return fail(error, why);
  auto flyers = modes->read("Colorado/Flyers.xml");
  if (!flyers.ok()) return fail(error, "gamemodes.zip Colorado/Flyers.xml: " + flyers.error);
  auto barns = modes->read("Colorado/barnfinds.xml");
  if (!barns.ok()) return fail(error, "gamemodes.zip Colorado/barnfinds.xml: " + barns.error);
  const auto objs_path = media / "tracks" / "colorado" / "Ribbon_00" / "GameObjs.xml";
  std::ifstream f(objs_path, std::ios::binary);
  if (!f) return fail(error, "cannot read " + objs_path.string());
  std::stringstream objs;
  objs << f.rdbuf();
  auto text = [](const std::vector<uint8_t>& d) { return std::string_view(reinterpret_cast<const char*>(d.data()), d.size()); };
  return parse_collectibles(text(flyers.data), text(barns.data), objs.str(), out, error);
}

FreeRoam::FreeRoam(const Collectibles& data, Progress& progress) : data_(data), progress_(progress) {
  progress_.barns.resize(data_.barns.size());
  sign_found_.assign(data_.signs.size(), 0);
  for (size_t i = 0; i < data_.signs.size(); ++i) sign_found_[i] = progress_.signs_found.count(data_.signs[i].id) > 0;
  rebuild_hidden();
}

int FreeRoam::barns_found() const {
  int n = 0;
  for (const auto& b : progress_.barns) n += b.status == kBarnFound;
  return n;
}

void FreeRoam::rebuild_hidden() {
  auto set = std::make_shared<std::set<std::string>>(progress_.signs_found);
  for (size_t i = 0; i < data_.barns.size(); ++i) {
    const Barn& b = data_.barns[i];
    if (progress_.barns[i].status == kBarnFound) {
      set->insert({b.closed_id, b.closed_coll_id, b.trigger});
    } else {
      set->insert({b.open_id, b.open_coll_id});
    }
  }
  set->erase(std::string());
  hidden_ = std::move(set);
}

std::vector<Event> FreeRoam::step(const CarState& car) {
  std::vector<Event> events;
  bool changed = false;
  progress_.total_distance += std::max(0.0, car.moved);

  // Signs: the car's footprint, grown by the sign's reach, holds the sign
  // (interim break rule: contact above 1 m/s).
  if (std::fabs(car.speed) > kSignBreakSpeed) {
    const double fx = car.forward_x, fz = car.forward_z;
    for (size_t i = 0; i < data_.signs.size(); ++i) {
      if (sign_found_[i]) continue;
      const Placement& p = data_.signs[i].at;
      const double dx = p.x - car.x, dz = p.z - car.z;
      if (dx * dx + dz * dz > 100 || std::fabs(p.y - car.y) > 3) continue;
      const double along = dx * fx + dz * fz, across = dx * fz - dz * fx;
      if (std::fabs(along) > car.half_length + kSignReach || std::fabs(across) > car.half_width + kSignReach) continue;
      sign_found_[i] = 1;
      progress_.signs_found.insert(data_.signs[i].id);
      events.push_back({Event::kSignSmashed, int(i)});
      changed = true;
    }
  }

  // Barns: discovery at status 5, within the trigger radius, below maxMPH.
  for (size_t i = 0; i < data_.barns.size() && i < progress_.barns.size(); ++i) {
    BarnProgress& bp = progress_.barns[i];
    if (bp.status != kBarnRumour) continue;
    const Barn& b = data_.barns[i];
    if (std::hypot(b.at.x - car.x, b.at.z - car.z) > b.radius) continue;
    if (std::fabs(car.speed) >= b.max_mph * 0.44704) continue;
    bp.status = kBarnFound;
    bp.found_at = progress_.total_distance;
    if (std::find(progress_.garage.begin(), progress_.garage.end(), b.car_id) == progress_.garage.end())
      progress_.garage.push_back(b.car_id);
    events.push_back({Event::kBarnFound, int(i)});
    changed = true;
  }

  // Rumours (interim): the next not-spawned barn by number once the total
  // distance passes the last rumour's start by 50 km.
  double last = 0;
  for (const auto& bp : progress_.barns)
    if (bp.status != kBarnNotSpawned) last = std::max(last, bp.rumour_at);
  if (progress_.total_distance >= last + kRumourSpacing) {
    for (size_t i = 0; i < progress_.barns.size(); ++i) {
      if (progress_.barns[i].status != kBarnNotSpawned) continue;
      progress_.barns[i].status = kBarnRumour;
      progress_.barns[i].rumour_at = progress_.total_distance;
      events.push_back({Event::kRumourStarted, int(i)});
      break;
    }
  }

  if (changed) rebuild_hidden();
  return events;
}

int64_t discounted_price(int64_t price, int discount_percent) {
  const int d = upgrade_discount(discount_percent);
  if (d <= 0) return price;
  const float f = (1.0f - float(d) / 100.0f) * float(price) + 0.5f;
  return int64_t(std::floor(f));
}

std::array<double, 2> map_uv(double x, double z) {
  return {(x - kMapWest) / kMapWidth, (kMapNorth - z) / kMapHeight};
}

}  // namespace freeroam
