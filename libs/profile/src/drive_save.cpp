#include "profile/drive_save.h"

namespace profile {

void to_drive_save(const Imported& p, const std::function<std::string(int64_t)>& media_of, drive::DriveSave& out,
                   std::vector<std::string>* notes) {
  auto note = [&](std::string s) {
    if (notes) notes->push_back(std::move(s));
  };
  out = drive::DriveSave{};
  out.owned_garage = true;
  // The selected car, or the next loadable one (FORMAT-PROFILE: a missing
  // model is kept but hidden; select another).
  const GarageCar* selected = p.selected_car();
  int hidden = 0;
  for (const auto& g : p.garage) {
    out.garage.push_back(int(g.car_id));
    if (media_of(g.car_id).empty()) ++hidden;
  }
  if (selected && !media_of(selected->car_id).empty()) out.car = media_of(selected->car_id);
  for (const auto& g : p.garage)
    if (out.car.empty() && !media_of(g.car_id).empty()) {
      out.car = media_of(g.car_id);
      note("CareerCar " + std::to_string(p.career_car) + " cannot be loaded: " + out.car + " selected");
    }
  if (hidden) note(std::to_string(hidden) + " garage cars hidden (models not loadable)");
  // The career (as is).
  out.credits = p.credits, out.total_winnings = p.total_winnings;
  out.wristband_points = int(p.xp);
  out.num_races = int(p.num_races), out.num_victories = int(p.num_victories), out.num_podiums = int(p.num_podiums);
  out.time_festival = p.time_festival, out.time_street = p.time_street, out.time_exhibition = p.time_exhibition;
  out.top_speed = p.top_speed;
  out.race_results = p.race_results, out.event_results = p.event_results;
  // Free roam.
  out.total_distance = p.distance_driven;
  for (const auto& s : p.signs)
    if (s.found) out.signs_found.push_back(s.name);
  for (const auto& b : p.barns) out.barns.push_back({b.name, int(b.status), b.rumour_at, b.found_at});
  out.road_discovery_nodes = p.road_discovery_nodes;
  out.road_discovery_words = p.road_discovery_words;
  out.discovered_outposts = p.outposts_discovered;
  out.completed_pr_stunts = p.pr_stunts_completed;
  out.satnav_enabled = p.satnav_on;
  // Carried as is: the level, unlocks, Rally counters and the 1000 Club.
  out.level = int(p.level);
  out.event_unlocks = p.event_unlocks, out.hub_unlocks = p.hub_unlocks;
  out.has_rally = p.rally_fields, out.num_rallies = int(p.num_rallies), out.num_rally_stages = int(p.num_rally_stages);
  out.num_rally_victories = int(p.num_rally_victories), out.rally_winnings = p.rally_winnings;
  out.rally_xp = int(p.rally_xp), out.rally_level = int(p.rally_level);
  for (const auto& c : p.club) out.club.push_back({c.car_id, c.keys});
  // UpgradeDiscount: recomputed from the signs and compared.
  if (int64_t(p.signs_found()) != p.upgrade_discount)
    note("UpgradeDiscount " + std::to_string(p.upgrade_discount) + " but " + std::to_string(p.signs_found()) +
         " signs found (the signs win)");
}

garage::Garage to_garage(const Imported& p) {
  garage::Garage g;
  g.last_id = p.last_used_garage_id;
  if (const GarageCar* s = p.selected_car()) g.selected = s->id;
  for (const auto& r : p.garage) {
    garage::Car c;
    c.id = r.id, c.car_id = r.car_id, c.class_id = int(r.class_id), c.colour = int(r.color_index);
    c.performance_index = r.performance_index;
    if (auto it = r.columns.find("PartsValue"); it != r.columns.end()) c.parts_value = it->second;
    for (const auto& s : cars::upgrade_slots())
      if (auto it = r.columns.find(s.column); it != r.columns.end() && it->second >= 0) c.slots[s.column] = it->second;
    g.cars.push_back(std::move(c));
  }
  for (const auto& b : p.parts) g.bought.push_back({b.garage_id, int(b.part_enum), b.part_id, b.manufacturer_id, b.price_paid});
  return g;
}

}  // namespace profile
