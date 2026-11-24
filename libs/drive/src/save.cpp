#include "drive/save.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace drive {

namespace {

constexpr const char* kHeader = "shift-drive-save";

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.6f", v);
  return buf;
}

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

}  // namespace

std::string save_to_text(const DriveSave& s) {
  std::string out = std::string(kHeader) + " " + std::to_string(DriveSave::kVersion) + "\n";
  out += "car " + s.car + "\n";
  out += "position " + num(s.position[0]) + " " + num(s.position[1]) + " " + num(s.position[2]) + "\n";
  out += "heading " + num(s.heading) + "\n";
  if (s.has_spawn)
    out += "spawn " + num(s.spawn[0]) + " " + num(s.spawn[1]) + " " + num(s.spawn[2]) + " " + num(s.spawn_heading) +
           "\n";
  if (s.total_distance > 0) out += "distance " + num(s.total_distance) + "\n";
  if (!s.signs_found.empty()) {
    out += "signs";
    for (const auto& id : s.signs_found) out += " " + id;
    out += "\n";
  }
  for (const auto& b : s.barns)
    out += "barn " + b.name + " " + std::to_string(b.status) + " " + num(b.rumour_at) + " " + num(b.found_at) + "\n";
  for (const auto& br : s.pending_barn_restorations)
    out += "pending_barn " + std::to_string(br.barn_id) + " " + std::to_string(br.car_id) + " " +
           num(double(br.remaining_sec)) + "\n";
  if (!s.garage.empty()) {
    out += "garage";
    for (int id : s.garage) out += " " + std::to_string(id);
    out += "\n";
  }
  if (s.owned_garage) out += "owned_garage 1\n";
  if (s.career_started) out += "career_started 1\n";
  if (s.credits || s.total_winnings)
    out += "credits " + std::to_string(s.credits) + " " + std::to_string(s.total_winnings) + "\n";
  if (s.wristband_points) out += "wristband " + std::to_string(s.wristband_points) + "\n";
  if (s.collected_wristband >= 0) out += "collected_wristband " + std::to_string(s.collected_wristband) + "\n";
  if (s.popularity_points > 0) out += "popularity " + num(s.popularity_points) + "\n";
  if (s.max_skills_score > 0)
    out += "max_skills " + std::to_string(s.max_skills_score) + " " + std::to_string(s.max_skills_car) + "\n";
  if (s.fame_rewards_claimed > 0) out += "fame_claimed " + std::to_string(s.fame_rewards_claimed) + "\n";
  if (s.difficulty != 1) out += "difficulty " + std::to_string(s.difficulty) + "\n";
  if (s.ai_skill != 10) out += "ai_skill " + std::to_string(s.ai_skill) + "\n";
  if (s.ai_mode != 0) out += "ai_mode " + std::to_string(s.ai_mode) + "\n";
  if (!s.radio_station.empty() || !s.radio_dj || std::fabs(s.radio_volume - 1.0) > 1e-4)
    out += "radio " + (s.radio_station.empty() ? std::string("-") : s.radio_station) + " " +
           (s.radio_dj ? "1" : "0") + " " + num(s.radio_volume) + "\n";
  if (!s.satnav_enabled) out += "satnav 0\n";
  if (s.treasure_map) out += "treasure_map 1\n";
  if (s.fast_travel_anywhere) out += "fast_travel_anywhere 1\n";
  if (!s.codriver_enabled) out += "codriver 0\n";
  if (s.tod_sec >= 0.0 && std::isfinite(s.tod_sec)) out += "tod_sec " + num(std::fmod(s.tod_sec, 86400.0)) + "\n";
  if (!s.intro_completed) out += "intro_completed 0\n";
  if (!s.campaign_stage.empty()) out += "campaign_stage " + s.campaign_stage + "\n";
  if (!s.completed_cutscenes.empty()) {
    out += "completed_cutscenes";
    for (const auto& id : s.completed_cutscenes) out += " " + id;
    out += "\n";
  }
  if (!s.heard_dialogs.empty()) {
    out += "heard_dialogs";
    for (const auto& id : s.heard_dialogs) out += " " + id;
    out += "\n";
  }
  if (!s.unlocked_bosses.empty()) {
    out += "unlocked_bosses";
    for (const auto& id : s.unlocked_bosses) out += " " + id;
    out += "\n";
  }
  if (!s.unlocked_showcases.empty()) {
    out += "unlocked_showcases";
    for (const auto& id : s.unlocked_showcases) out += " " + id;
    out += "\n";
  }
  if (s.finale_complete) out += "finale_complete 1\n";
  if (s.road_discovery_nodes > 0 && !s.road_discovery_words.empty()) {
    size_t last = s.road_discovery_words.size();
    while (last > 0 && s.road_discovery_words[last - 1] == 0u) --last;
    out += "roads_discovered " + std::to_string(s.road_discovery_nodes) + " ";
    if (last == 0) {
      out += "0\n";
    } else {
      char hb[9];
      for (size_t i = 0; i < last; ++i) {
        std::snprintf(hb, sizeof hb, "%08x", s.road_discovery_words[i]);
        out.append(hb, 8);
      }
      out += "\n";
    }
  }
  if (!s.discovered_outposts.empty()) {
    out += "outposts";
    for (const auto& id : s.discovered_outposts) out += " " + id;
    out += "\n";
  }
  if (!s.completed_pr_stunts.empty()) {
    out += "pr_stunts";
    for (const auto& id : s.completed_pr_stunts) out += " " + id;
    out += "\n";
  }
  for (const auto& [id, mph] : s.speed_traps)
    out += "speed_trap " + id + " " + num(mph) + "\n";
  for (const auto& [id, mph] : s.speed_zones)
    out += "speed_zone " + id + " " + num(mph) + "\n";
  for (const auto& sp : s.sponsors)
    out += "sponsor " + sp.id + " " + std::to_string(sp.count) + " " + std::to_string(sp.claimed) + "\n";
  if (s.h2h_races > 0 || s.h2h_wins > 0)
    out += "h2h " + std::to_string(s.h2h_races) + " " + std::to_string(s.h2h_wins) + "\n";
  if (s.driver_xp > 0) out += "driver_xp " + std::to_string(s.driver_xp) + "\n";
  for (const auto& [make_id, xp] : s.make_affinity)
    if (xp > 0) out += "affinity " + std::to_string(make_id) + " " + std::to_string(xp) + "\n";
  if (!s.promo_cars.empty()) {
    out += "promo_cars";
    for (int id : s.promo_cars) out += " " + std::to_string(id);
    out += "\n";
  }
  if (s.num_races)
    out += "stats " + std::to_string(s.num_races) + " " + std::to_string(s.num_victories) + " " +
           std::to_string(s.num_podiums) + " " + num(s.time_festival) + " " + num(s.time_street) + " " +
           num(s.time_exhibition) + " " + num(s.top_speed) + "\n";
  for (const auto& [id, place] : s.race_results)
    out += "race_result " + std::to_string(id) + " " + std::to_string(place) + "\n";
  for (const auto& [id, place] : s.event_results)
    out += "event_result " + std::to_string(id) + " " + std::to_string(place) + "\n";
  if (!s.prize_cars.empty()) {
    out += "prize_cars";
    for (auto id : s.prize_cars) out += " " + std::to_string(id);
    out += "\n";
  }
  if (s.level) out += "level " + std::to_string(s.level) + "\n";
  for (const auto* list : {&s.event_unlocks, &s.hub_unlocks}) {
    if (list->empty()) continue;
    out += list == &s.event_unlocks ? "unlocks event" : "unlocks hub";
    for (auto id : *list) out += " " + std::to_string(id);
    out += "\n";
  }
  if (s.has_rally)
    out += "rally " + std::to_string(s.num_rallies) + " " + std::to_string(s.num_rally_stages) + " " +
           std::to_string(s.num_rally_victories) + " " + std::to_string(s.rally_winnings) + " " +
           std::to_string(s.rally_xp) + " " + std::to_string(s.rally_level) + "\n";
  for (const auto& car : s.club) {
    out += "club " + std::to_string(car.car_id);
    for (size_t i = 0; i < car.challenges.size(); ++i) {
      const auto& [key, done] = car.challenges[i];
      out += " " + std::to_string(key) + (done ? ":1" : ":0");
      if (i < car.progress.size() && !car.progress[i].empty()) {
        bool any_nonzero = false;
        for (int v : car.progress[i]) any_nonzero = any_nonzero || (v > 0);
        if (any_nonzero) {
          out += ":";
          for (size_t j = 0; j < car.progress[i].size(); ++j) {
            if (j) out += ",";
            out += std::to_string(car.progress[i][j]);
          }
        }
      }
    }
    out += "\n";
  }
  for (const auto& h : s.car_history) {
    out += "car_history " + std::to_string(h.garage_id) + " " + std::to_string(h.car_id) + " " +
           num(h.distance_driven) + " " + num(h.time_driven) + " " + std::to_string(h.total_winnings) + " " +
           std::to_string(h.total_repairs) + " " + std::to_string(h.num_races) + " " +
           std::to_string(h.num_victories) + " " + std::to_string(h.num_podiums) + "\n";
  }
  if (!s.car_club_name.empty()) out += "car_club_name " + s.car_club_name + "\n";
  if (!s.car_club_tag.empty()) out += "car_club_tag " + s.car_club_tag + "\n";
  if (!s.car_club_shared.empty()) {
    out += "car_club_shared";
    for (int id : s.car_club_shared) out += " " + std::to_string(id);
    out += "\n";
  }
  if (s.has_controls) {
    const auto& c = s.controls;
    out += "controls_layout " + std::to_string(c.layout_id) + "\n";
    const auto& d = c.deadzones;
    out += "controls_deadzones " + num(d.steering_inside) + " " + num(d.steering_outside) + " " +
           num(d.throttle_inside) + " " + num(d.throttle_outside) + " " + num(d.brake_inside) + " " +
           num(d.brake_outside) + " " + num(d.clutch_inside) + " " + num(d.clutch_outside) + " " +
           num(d.handbrake_inside) + " " + num(d.handbrake_outside) + "\n";
    out += "controls_rumble " + std::string(c.rumble_enabled ? "1 " : "0 ") + num(c.rumble_scale) + "\n";
    out += "controls_camera_effects " + std::string(c.camera_effects ? "1\n" : "0\n");
    const std::string kb = c.keys.serialize();
    if (!kb.empty()) out += "controls_keys " + kb + "\n";
    const auto& a = c.assists;
    out += "controls_assists " + std::to_string(a.braking) + " " + std::to_string(a.steering) + " " +
           (a.traction_control ? "1 " : "0 ") + (a.stability_control ? "1 " : "0 ") +
           std::to_string(a.shifting) + " " + std::to_string(a.driving_line) + " " +
           (a.rewind ? "1\n" : "0\n");
  }
  return out;
}

bool save_from_text(const std::string& text, DriveSave& out, std::string* error) {
  out = DriveSave{};
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  std::string line;
  if (!std::getline(in, line)) return fail(error, "empty save");
  {
    std::istringstream h(line);
    h.imbue(std::locale::classic());
    std::string tag;
    int version = 0;
    if (!(h >> tag >> version) || tag != kHeader) return fail(error, "not a shift-drive-save file");
    if (version < 1 || version > DriveSave::kVersion)
      return fail(error, "save version " + std::to_string(version) + " (this build reads up to " +
                             std::to_string(DriveSave::kVersion) + ")");
  }
  bool have_pos = false, have_heading = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream l(line);
    l.imbue(std::locale::classic());
    std::string key;
    if (!(l >> key)) continue;
    if (key == "car") {
      l >> out.car;
    } else if (key == "position") {
      have_pos = bool(l >> out.position[0] >> out.position[1] >> out.position[2]);
    } else if (key == "heading") {
      have_heading = bool(l >> out.heading);
    } else if (key == "spawn") {
      out.has_spawn = bool(l >> out.spawn[0] >> out.spawn[1] >> out.spawn[2] >> out.spawn_heading);
    } else if (key == "distance") {
      if (!(l >> out.total_distance) || !std::isfinite(out.total_distance) || out.total_distance < 0)
        out.total_distance = 0;
    } else if (key == "signs") {
      for (std::string id; l >> id;) out.signs_found.push_back(id);
    } else if (key == "barn") {
      SavedBarn b;
      if (l >> b.name >> b.status >> b.rumour_at >> b.found_at) out.barns.push_back(b);
    } else if (key == "pending_barn") {
      PendingBarnRestoration br;
      double rem = 0.0;
      if (l >> br.barn_id >> br.car_id >> rem && std::isfinite(rem) && rem >= 0.0) {
        br.remaining_sec = float(rem);
        out.pending_barn_restorations.push_back(br);
      }
    } else if (key == "garage") {
      for (int id; l >> id;) out.garage.push_back(id);
    } else if (key == "owned_garage") {
      int v = 0;
      out.owned_garage = (l >> v) && v != 0;
    } else if (key == "career_started") {
      int v = 0;
      out.career_started = (l >> v) && v != 0;
    } else if (key == "credits") {
      l >> out.credits >> out.total_winnings;
    } else if (key == "wristband") {
      l >> out.wristband_points;
    } else if (key == "collected_wristband") {
      l >> out.collected_wristband;
    } else if (key == "popularity") {
      if (!(l >> out.popularity_points) || !std::isfinite(out.popularity_points) || out.popularity_points < 0)
        out.popularity_points = 0;
    } else if (key == "max_skills") {
      l >> out.max_skills_score >> out.max_skills_car;
    } else if (key == "fame_claimed") {
      l >> out.fame_rewards_claimed;
    } else if (key == "difficulty") {
      int d = 1;
      if (l >> d && d >= 0 && d <= 4) out.difficulty = d;
    } else if (key == "ai_skill") {
      int sk = 10;
      if (l >> sk && sk >= 1 && sk <= 20) out.ai_skill = sk;
    } else if (key == "ai_mode") {
      int m = 0;
      if (l >> m && m >= 0 && m <= 2) out.ai_mode = m;
    } else if (key == "radio") {
      std::string st;
      int dj = 1;
      double vol = 1.0;
      if (l >> st) {
        out.radio_station = (st == "-") ? std::string{} : st;
        if (l >> dj) out.radio_dj = (dj != 0);
        if (l >> vol && std::isfinite(vol) && vol >= 0.0 && vol <= 2.0) out.radio_volume = vol;
      }
    } else if (key == "satnav") {
      int v = 1;
      if (l >> v) out.satnav_enabled = (v != 0);
    } else if (key == "treasure_map") {
      int v = 0;
      if (l >> v) out.treasure_map = (v != 0);
    } else if (key == "fast_travel_anywhere") {
      int v = 0;
      if (l >> v) out.fast_travel_anywhere = (v != 0);
    } else if (key == "codriver") {
      int v = 1;
      if (l >> v) out.codriver_enabled = (v != 0);
    } else if (key == "tod_sec") {
      double v = -1.0;
      if (l >> v && std::isfinite(v) && v >= 0.0 && v < 86400.0) out.tod_sec = v;
    } else if (key == "intro_completed") {
      int v = 1;
      if (l >> v) out.intro_completed = (v != 0);
    } else if (key == "campaign_stage") {
      l >> out.campaign_stage;
    } else if (key == "completed_cutscenes") {
      for (std::string id; l >> id;) out.completed_cutscenes.push_back(id);
    } else if (key == "heard_dialogs") {
      for (std::string id; l >> id;) out.heard_dialogs.push_back(id);
    } else if (key == "unlocked_bosses") {
      for (std::string id; l >> id;) out.unlocked_bosses.push_back(id);
    } else if (key == "unlocked_showcases") {
      for (std::string id; l >> id;) out.unlocked_showcases.push_back(id);
    } else if (key == "finale_complete") {
      int v = 0;
      if (l >> v) out.finale_complete = (v != 0);
    } else if (key == "roads_discovered") {
      uint32_t n_nodes = 0;
      std::string hex;
      if (l >> n_nodes >> hex) {
        out.road_discovery_nodes = n_nodes;
        out.road_discovery_words.clear();
        if (hex != "0" && hex.size() % 8 == 0) {
          const size_t nw = hex.size() / 8;
          out.road_discovery_words.resize(nw, 0u);
          bool ok_hex = true;
          for (size_t i = 0; i < nw && ok_hex; ++i) {
            uint32_t v = 0;
            for (size_t k = 0; k < 8; ++k) {
              const char ch = hex[i * 8 + k];
              int nib = -1;
              if (ch >= '0' && ch <= '9') nib = ch - '0';
              else if (ch >= 'a' && ch <= 'f') nib = 10 + (ch - 'a');
              else if (ch >= 'A' && ch <= 'F') nib = 10 + (ch - 'A');
              if (nib < 0) {
                ok_hex = false;
                break;
              }
              v = (v << 4) | uint32_t(nib);
            }
            out.road_discovery_words[i] = v;
          }
          if (!ok_hex) out.road_discovery_words.clear();
        }
      }
    } else if (key == "outposts") {
      for (std::string id; l >> id;) out.discovered_outposts.push_back(id);
    } else if (key == "pr_stunts") {
      for (std::string id; l >> id;) out.completed_pr_stunts.push_back(id);
    } else if (key == "speed_trap") {
      std::string id;
      double mph = 0;
      if (l >> id >> mph && std::isfinite(mph) && mph > 0) out.speed_traps.emplace_back(id, mph);
    } else if (key == "speed_zone") {
      std::string id;
      double mph = 0;
      if (l >> id >> mph && std::isfinite(mph) && mph > 0) out.speed_zones.emplace_back(id, mph);
    } else if (key == "sponsor") {
      DriveSave::SavedSponsor sp;
      if (l >> sp.id >> sp.count >> sp.claimed && sp.count >= 0 && sp.claimed >= 0) out.sponsors.push_back(sp);
    } else if (key == "h2h") {
      l >> out.h2h_races >> out.h2h_wins;
    } else if (key == "driver_xp") {
      if (!(l >> out.driver_xp) || out.driver_xp < 0) out.driver_xp = 0;
    } else if (key == "affinity") {
      int64_t make_id = 0;
      int xp = 0;
      if (l >> make_id >> xp && make_id > 0 && xp > 0) out.make_affinity.emplace_back(make_id, xp);
    } else if (key == "promo_cars") {
      for (int id; l >> id;) out.promo_cars.push_back(id);
    } else if (key == "stats") {
      l >> out.num_races >> out.num_victories >> out.num_podiums >> out.time_festival >> out.time_street >>
          out.time_exhibition >> out.top_speed;
    } else if (key == "race_result" || key == "event_result") {
      int64_t id = 0;
      int place = 0;
      if (l >> id >> place && place >= 1)
        (key == "race_result" ? out.race_results : out.event_results).emplace_back(id, place);
    } else if (key == "prize_cars") {
      for (int64_t id; l >> id;) out.prize_cars.push_back(id);
    } else if (key == "level") {
      l >> out.level;
    } else if (key == "unlocks") {
      std::string kind;
      l >> kind;
      auto& list = kind == "event" ? out.event_unlocks : out.hub_unlocks;
      if (kind == "event" || kind == "hub")
        for (int64_t id; l >> id;) list.push_back(id);
    } else if (key == "rally") {
      out.has_rally = bool(l >> out.num_rallies >> out.num_rally_stages);
      // Older saves stop after the stage count.
      if (out.has_rally) l >> out.num_rally_victories >> out.rally_winnings >> out.rally_xp >> out.rally_level;
    } else if (key == "club") {
      SavedClubCar car;
      if (!(l >> car.car_id)) continue;
      for (std::string kv; l >> kv;) {
        const auto colon = kv.find(':');
        if (colon == std::string::npos) continue;
        const auto colon2 = kv.find(':', colon + 1);
        const std::string done_str =
            colon2 == std::string::npos ? kv.substr(colon + 1) : kv.substr(colon + 1, colon2 - colon - 1);
        car.challenges.emplace_back(uint32_t(std::strtoul(kv.c_str(), nullptr, 10)), done_str == "1");
        std::vector<int> prog;
        if (colon2 != std::string::npos) {
          size_t pos = colon2 + 1;
          while (pos < kv.size()) {
            const size_t comma = kv.find(',', pos);
            const std::string part =
                comma == std::string::npos ? kv.substr(pos) : kv.substr(pos, comma - pos);
            if (!part.empty()) prog.push_back(std::max(0, std::atoi(part.c_str())));
            if (comma == std::string::npos) break;
            pos = comma + 1;
          }
        }
        car.progress.push_back(std::move(prog));
      }
      out.club.push_back(std::move(car));
    } else if (key == "car_history") {
      SavedCarHistory h;
      if (l >> h.garage_id >> h.car_id >> h.distance_driven >> h.time_driven >> h.total_winnings >> h.total_repairs >>
          h.num_races >> h.num_victories >> h.num_podiums) {
        if (!std::isfinite(h.distance_driven) || h.distance_driven < 0.0) h.distance_driven = 0.0;
        if (!std::isfinite(h.time_driven) || h.time_driven < 0.0) h.time_driven = 0.0;
        out.car_history.push_back(h);
      }
    } else if (key == "car_club_name") {
      std::string rest;
      std::getline(l, rest);
      while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
      out.car_club_name = rest;
    } else if (key == "car_club_tag") {
      std::string tag;
      if (l >> tag) {
        if (tag.size() > 4) tag.resize(4);
        out.car_club_tag = tag;
      }
    } else if (key == "car_club_shared") {
      for (int id = 0; l >> id;) out.car_club_shared.push_back(id);
    } else if (key == "controls_layout") {
      if (l >> out.controls.layout_id) out.has_controls = true;
    } else if (key == "controls_deadzones") {
      auto& d = out.controls.deadzones;
      if (l >> d.steering_inside >> d.steering_outside >> d.throttle_inside >> d.throttle_outside >>
          d.brake_inside >> d.brake_outside >> d.clutch_inside >> d.clutch_outside >>
          d.handbrake_inside >> d.handbrake_outside) {
        d.clamp_valid();
        out.has_controls = true;
      }
    } else if (key == "controls_rumble") {
      int en = 1;
      if (l >> en >> out.controls.rumble_scale) {
        out.controls.rumble_enabled = en != 0;
        out.has_controls = true;
      }
    } else if (key == "controls_camera_effects") {
      int en = 1;
      if (l >> en) {
        out.controls.camera_effects = en != 0;
        out.has_controls = true;
      }
    } else if (key == "controls_keys") {
      std::string kb;
      if (l >> kb) {
        out.controls.keys.deserialize(kb);
        out.has_controls = true;
      }
    } else if (key == "controls_assists") {
      auto& a = out.controls.assists;
      int tcs = 0, stm = 1, rew = 1;
      if (l >> a.braking >> a.steering >> tcs >> stm >> a.shifting >> a.driving_line) {
        if (l >> rew) a.rewind = rew != 0;
        a.traction_control = tcs != 0;
        a.stability_control = stm != 0;
        out.has_controls = true;
      }
    }
  }
  if (out.car.empty()) return fail(error, "save without a car");
  if (!have_pos || !have_heading) return fail(error, "save without a position and heading");
  for (double v : {out.position[0], out.position[1], out.position[2], out.heading})
    if (!std::isfinite(v)) return fail(error, "save with a non-finite position");
  return true;
}

bool atomic_replace_file(const std::filesystem::path& tmp_path, const std::filesystem::path& dst_path,
                         std::string* error) {
  std::error_code ec;
  if (dst_path.has_parent_path()) std::filesystem::create_directories(dst_path.parent_path(), ec);
#if defined(_WIN32)
  // Best-effort flush of tmp_path before atomic replace.
  {
    HANDLE h = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      FlushFileBuffers(h);
      CloseHandle(h);
    }
  }
  BOOL moved = MoveFileExW(tmp_path.c_str(), dst_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!moved) {
    const DWORD last_err = GetLastError();
    if (last_err == ERROR_INVALID_PARAMETER || last_err == ERROR_NOT_SUPPORTED) {
      moved = MoveFileExW(tmp_path.c_str(), dst_path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
  }
  if (!moved) {
    const DWORD last_err = GetLastError();
    DeleteFileW(tmp_path.c_str());
    return fail(error, "cannot replace " + dst_path.string() + " (win32 error " + std::to_string(last_err) + ")");
  }
  return true;
#else
  {
    const int fd = ::open(tmp_path.c_str(), O_RDONLY);
    if (fd >= 0) {
      (void)::fsync(fd);
      ::close(fd);
    }
  }
  std::filesystem::rename(tmp_path, dst_path, ec);
  if (ec) {
    std::error_code rm_ec;
    std::filesystem::remove(tmp_path, rm_ec);
    return fail(error, "cannot replace " + dst_path.string() + ": " + ec.message());
  }
  if (dst_path.has_parent_path()) {
    const int dfd = ::open(dst_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
      (void)::fsync(dfd);
      ::close(dfd);
    }
  }
  return true;
#endif
}

bool atomic_write_file(const std::filesystem::path& path, const void* data, size_t size, std::string* error) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "cannot create directory for " + path.string() + ": " + ec.message());
  }
  auto tmp = path;
  tmp += ".tmp";
  const auto* ptr = static_cast<const uint8_t*>(data);
#if defined(_WIN32)
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return fail(error, "cannot write " + tmp.string() + " (win32 error " + std::to_string(GetLastError()) + ")");
  }
  size_t written = 0;
  while (written < size) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - written, 16u * 1024u * 1024u));
    DWORD n = 0;
    if (!WriteFile(h, ptr + written, chunk, &n, nullptr) || n == 0) {
      const DWORD last_err = GetLastError();
      CloseHandle(h);
      DeleteFileW(tmp.c_str());
      return fail(error, "cannot write " + tmp.string() + " (win32 error " + std::to_string(last_err) + ")");
    }
    written += n;
  }
  if (!FlushFileBuffers(h)) {
    const DWORD last_err = GetLastError();
    CloseHandle(h);
    DeleteFileW(tmp.c_str());
    return fail(error, "cannot flush " + tmp.string() + " (win32 error " + std::to_string(last_err) + ")");
  }
  CloseHandle(h);
  BOOL moved = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!moved) {
    const DWORD last_err = GetLastError();
    if (last_err == ERROR_INVALID_PARAMETER || last_err == ERROR_NOT_SUPPORTED) {
      moved = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
  }
  if (!moved) {
    const DWORD last_err = GetLastError();
    DeleteFileW(tmp.c_str());
    return fail(error, "cannot replace " + path.string() + " (win32 error " + std::to_string(last_err) + ")");
  }
  return true;
#else
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return fail(error, "cannot write " + tmp.string() + ": " + std::strerror(errno));
  size_t written = 0;
  while (written < size) {
    const ssize_t n = ::write(fd, ptr + written, size - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      const int err = errno;
      ::close(fd);
      ::unlink(tmp.c_str());
      return fail(error, "cannot write " + tmp.string() + ": " + std::strerror(err));
    }
    if (n == 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      return fail(error, "short write to " + tmp.string());
    }
    written += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0 && errno != EINVAL && errno != EROFS) {
    const int err = errno;
    ::close(fd);
    ::unlink(tmp.c_str());
    return fail(error, "cannot flush " + tmp.string() + ": " + std::strerror(err));
  }
  if (::close(fd) != 0) {
    const int err = errno;
    ::unlink(tmp.c_str());
    return fail(error, "cannot close " + tmp.string() + ": " + std::strerror(err));
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::error_code rm_ec;
    std::filesystem::remove(tmp, rm_ec);
    return fail(error, "cannot replace " + path.string() + ": " + ec.message());
  }
  if (path.has_parent_path()) {
    const int dfd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
      (void)::fsync(dfd);
      ::close(dfd);
    }
  }
  return true;
#endif
}

bool atomic_write_file(const std::filesystem::path& path, std::string_view text, std::string* error) {
  return atomic_write_file(path, text.data(), text.size(), error);
}

bool read_save(const std::filesystem::path& path, DriveSave& out, std::string* error) {
  if (error) error->clear();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail(error, "cannot read " + path.string());
  std::stringstream ss;
  ss << f.rdbuf();
  if (!save_from_text(ss.str(), out, error)) {
    if (error) *error = path.string() + ": " + *error;
    return false;
  }
  return true;
}

bool write_save(const std::filesystem::path& path, const DriveSave& save, std::string* error) {
  return atomic_write_file(path, save_to_text(save), error);
}

bool load_save_with_recovery(const std::filesystem::path& save_path, const std::filesystem::path& backup_dir,
                             DriveSave& out_save, std::filesystem::path* recovered_from, std::string* error) {
  namespace fs = std::filesystem;
  if (recovered_from) recovered_from->clear();
  if (error) error->clear();

  std::string primary_err;
  if (read_save(save_path, out_save, &primary_err)) return true;

  std::error_code ec;
  const bool primary_existed = fs::exists(save_path, ec);
  const std::string target_name = save_path.filename().empty() ? std::string("save.txt") : save_path.filename().string();

  std::vector<fs::path> candidates;
  if (!backup_dir.empty() && fs::is_directory(backup_dir, ec)) {
    struct SnapCand {
      bool has_time = false;
      int64_t time = 0;
      std::string id;
      fs::path file;
    };
    struct FileCand {
      fs::file_time_type mtime{};
      std::string name;
      fs::path file;
    };
    std::vector<SnapCand> snaps;
    std::vector<FileCand> direct_files;
    for (const auto& entry : fs::directory_iterator(backup_dir, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.empty() || name[0] == '.') continue;
      if (entry.is_directory(ec)) {
        fs::path sf = entry.path() / target_name;
        std::string sf_name = target_name;
        if (!fs::is_regular_file(sf, ec) && target_name != "save.txt") {
          sf = entry.path() / "save.txt";
          sf_name = "save.txt";
        }
        if (!fs::is_regular_file(sf, ec)) continue;
        SnapCand sc;
        sc.id = name;
        sc.file = sf;
        bool size_ok = true;
        if (fs::is_regular_file(entry.path() / "backup.txt", ec)) {
          std::ifstream mf(entry.path() / "backup.txt");
          for (std::string line; std::getline(mf, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream ls(line);
            std::string k;
            ls >> k;
            if (k == "time") {
              if (ls >> sc.time) sc.has_time = true;
            } else if (k == "file") {
              std::string fname;
              uint64_t fsize = 0;
              if (ls >> fname >> fsize && fname == sf_name) {
                const uint64_t actual = fs::file_size(sf, ec);
                if (ec || actual != fsize) size_ok = false;
              }
            }
          }
        }
        if (size_ok) snaps.push_back(std::move(sc));
      } else if (entry.is_regular_file(ec)) {
        if (name == "backup.txt" || name.ends_with(".tmp") || name.ends_with(".corrupt")) continue;
        if (name.ends_with(".bak") || name.ends_with(".txt")) {
          FileCand fc;
          fc.mtime = entry.last_write_time(ec);
          fc.name = name;
          fc.file = entry.path();
          direct_files.push_back(std::move(fc));
        }
      }
    }
    std::sort(snaps.begin(), snaps.end(), [](const SnapCand& a, const SnapCand& b) {
      if (a.has_time != b.has_time) return a.has_time > b.has_time;
      if (a.time != b.time) return a.time > b.time;
      return a.id > b.id;
    });
    for (auto& s : snaps) candidates.push_back(std::move(s.file));
    std::sort(direct_files.begin(), direct_files.end(), [](const FileCand& a, const FileCand& b) {
      if (a.mtime != b.mtime) return a.mtime > b.mtime;
      return a.name > b.name;
    });
    for (auto& f : direct_files) candidates.push_back(std::move(f.file));
  }

  fs::path sibling_bak = save_path;
  sibling_bak += ".bak";
  if (fs::is_regular_file(sibling_bak, ec)) candidates.push_back(sibling_bak);

  for (const auto& cand : candidates) {
    DriveSave recovered;
    std::string cand_err;
    if (!read_save(cand, recovered, &cand_err)) continue;

    if (primary_existed) {
      std::string raw_corrupt;
      {
        std::ifstream cf(save_path, std::ios::binary);
        if (cf) raw_corrupt.assign(std::istreambuf_iterator<char>(cf), {});
      }
      fs::path corrupt_path = save_path;
      corrupt_path += ".corrupt";
      std::string corrupt_err;
      (void)atomic_write_file(corrupt_path, raw_corrupt, &corrupt_err);
    }

    std::string write_err;
    if (!write_save(save_path, recovered, &write_err)) {
      return fail(error, "recovered from " + cand.string() + " but failed to restore " + save_path.string() + ": " +
                             write_err);
    }
    out_save = std::move(recovered);
    if (recovered_from) *recovered_from = cand;
    if (error) error->clear();
    return true;
  }

  if (primary_existed) {
    return fail(error, primary_err.empty() ? ("corrupt save: " + save_path.string()) : primary_err);
  }
  return false;
}

bool load_save_file(const std::filesystem::path& save_path, DriveSave& out_save, std::string* error,
                    const std::filesystem::path& backup_dir, std::filesystem::path* recovered_from) {
  std::filesystem::path effective_backups = backup_dir;
  if (effective_backups.empty() && save_path.has_parent_path() && save_path.parent_path().has_parent_path()) {
    std::error_code ec;
    const auto candidate = save_path.parent_path().parent_path() / "backups" / "saves";
    if (std::filesystem::is_directory(candidate, ec)) effective_backups = candidate;
  }
  return load_save_with_recovery(save_path, effective_backups, out_save, recovered_from, error);
}

}  // namespace drive
