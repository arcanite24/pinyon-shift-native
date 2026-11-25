#include "profile/drive_save.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "garage/tuning.h"

namespace profile {
namespace {

void push_be32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void push_be64(std::vector<uint8_t>& out, uint64_t v) {
  push_be32(out, uint32_t(v >> 32));
  push_be32(out, uint32_t(v & 0xFFFFFFFFu));
}

void push_bef32(std::vector<uint8_t>& out, float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  push_be32(out, u);
}

std::vector<uint8_t> make_speed_payload(double mph) {
  std::vector<uint8_t> p;
  p.reserve(6);
  p.push_back(1u);  // revealed
  p.push_back(1u);  // triggered
  push_bef32(p, float(mph));
  return p;
}

std::vector<uint8_t> make_barn_payload(const drive::SavedBarn& b) {
  std::vector<uint8_t> p;
  p.reserve(15);
  const bool collected = (b.status == 2);
  const bool discovered = (b.status == 1 || b.status == 2);
  const bool revealed = (b.status != 3);
  p.push_back(revealed ? 1u : 0u);
  p.push_back(discovered ? 1u : 0u);
  p.push_back(collected ? 1u : 0u);
  push_bef32(p, float(b.found_at));
  push_bef32(p, float(b.rumour_at));
  push_be32(p, uint32_t(b.status));
  return p;
}

Record* find_child(Record& parent, std::string_view name, std::string_view class_name) {
  for (auto& c : parent.children)
    if (c.name == name && c.class_name == class_name) return &c;
  return nullptr;
}

Record* find_child_by_class(Record& parent, std::string_view class_name) {
  for (auto& c : parent.children)
    if (c.class_name == class_name) return &c;
  return nullptr;
}

}  // namespace

int engine_driver_level_from_profile(int64_t main_level) { return int(std::max<int64_t>(0, main_level)) + 1; }

int64_t profile_level_from_engine(int engine_level) { return std::max(0, engine_level - 1); }

ImportedCareerStage imported_career_stage(const Imported& p) {
  ImportedCareerStage s;
  // Interim (PROG-05 is open): the first-time career's stages 1 (the opening
  // Viper race) and 2 (the Corrado drive to the festival) map to the engine's
  // Viper drive and Corrado qualifier while no band, point or result exists.
  const uint32_t ftc = p.first_time_career_stage;
  if (p.has_first_time_career && (ftc == 1 || ftc == 2) && p.wristband_level <= 0 && p.xp <= 0 &&
      p.event_results.empty()) {
    s.intro_completed = false;
    s.campaign_stage = ftc == 1 ? "viper_drive" : "qualify";
    s.collected_wristband = -1;
    return s;
  }
  s.intro_completed = true;
  s.campaign_stage = "freeroam";
  s.collected_wristband = int(std::clamp<int64_t>(p.wristband_level, 0, 6));
  return s;
}

void to_drive_save(const Imported& p, const std::function<std::string(int64_t)>& media_of, drive::DriveSave& out,
                   std::vector<std::string>* notes) {
  auto note = [&](std::string s) {
    if (notes) notes->push_back(std::move(s));
  };
  // Preserve existing local spawn/pose, camera/control bindings, audio/radio,
  // and local difficulty/ToD preferences when importing into an existing DriveSave.
  const auto prev_position = out.position;
  const double prev_heading = out.heading;
  const bool prev_has_spawn = out.has_spawn;
  const auto prev_spawn = out.spawn;
  const double prev_spawn_heading = out.spawn_heading;
  const bool prev_has_controls = out.has_controls;
  const auto prev_controls = out.controls;
  const std::string prev_radio_station = out.radio_station;
  const bool prev_radio_dj = out.radio_dj;
  const double prev_radio_volume = out.radio_volume;
  const double prev_tod_sec = out.tod_sec;
  const int prev_difficulty = out.difficulty;
  const int prev_ai_skill = out.ai_skill;
  const int prev_ai_mode = out.ai_mode;
  const bool prev_codriver_enabled = out.codriver_enabled;

  out = drive::DriveSave{};
  out.position = prev_position;
  out.heading = prev_heading;
  out.has_spawn = prev_has_spawn;
  out.spawn = prev_spawn;
  out.spawn_heading = prev_spawn_heading;
  out.has_controls = prev_has_controls;
  out.controls = prev_controls;
  out.radio_station = prev_radio_station;
  out.radio_dj = prev_radio_dj;
  out.radio_volume = prev_radio_volume;
  out.tod_sec = prev_tod_sec;
  out.difficulty = prev_difficulty;
  out.ai_skill = prev_ai_skill;
  out.ai_mode = prev_ai_mode;
  out.codriver_enabled = p.has_codriver ? p.codriver_on : prev_codriver_enabled;
  out.fast_travel_anywhere = p.fast_travel_anywhere;
  out.owned_garage = true;
  // The selected car, or the next loadable one (FORMAT-PROFILE: a missing
  // model is kept but hidden; select another).
  const GarageCar* selected = p.selected_car();
  int hidden = 0;
  for (const auto& g : p.garage) {
    out.garage.push_back(int(g.car_id));
    if (media_of(g.car_id).empty()) ++hidden;
    const auto& h = g.history;
    if (h.distance_driven > 0.0 || h.time_driven > 0.0 || h.total_winnings != 0 || h.total_repairs != 0 ||
        h.num_races != 0 || h.num_victories != 0 || h.num_podiums != 0) {
      drive::SavedCarHistory sh;
      sh.garage_id = g.id;
      sh.car_id = g.car_id;
      sh.distance_driven = h.distance_driven;
      sh.time_driven = h.time_driven;
      sh.total_winnings = h.total_winnings;
      sh.total_repairs = h.total_repairs;
      sh.num_races = h.num_races;
      sh.num_victories = h.num_victories;
      sh.num_podiums = h.num_podiums;
      out.car_history.push_back(sh);
    }
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
  // Popularity and the sponsors (SponsorshipState, SPEC_QUESTIONS Q60).
  out.popularity_points = p.popularity;
  out.fame_rewards_claimed = int(p.fame_tiers_claimed);
  for (const auto& sc : p.sponsors)
    if (sc.id != "Fame") out.sponsors.push_back({sc.id, int(sc.count), int(sc.tiers_claimed)});
  out.max_skills_score = p.max_skills_score;
  out.max_skills_car = p.max_skills_car;
  // No driver XP in the profile: the level is kept as saved (see below).
  out.driver_xp = 0;
  // The collected wristband (Main/WristbandLevel, zero-based); the opening.
  const ImportedCareerStage stage = imported_career_stage(p);
  out.intro_completed = stage.intro_completed;
  out.campaign_stage = stage.campaign_stage;
  out.collected_wristband = stage.collected_wristband;
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
  out.speed_traps = p.speed_traps;
  out.speed_zones = p.speed_zones;
  out.satnav_enabled = p.satnav_on;
  // Carried as is: the level, unlocks, Rally counters and the 1000 Club.
  // Main/Level is the zero-based PlayerLevelDef.Id; the engine's level is one-based.
  out.level = engine_driver_level_from_profile(p.level);
  out.event_unlocks = p.event_unlocks, out.hub_unlocks = p.hub_unlocks;
  out.has_rally = p.rally_fields, out.num_rallies = int(p.num_rallies), out.num_rally_stages = int(p.num_rally_stages);
  out.num_rally_victories = int(p.num_rally_victories), out.rally_winnings = p.rally_winnings;
  out.rally_xp = int(p.rally_xp), out.rally_level = int(p.rally_level);
  for (const auto& c : p.club) out.club.push_back({c.car_id, c.keys, c.progress});
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
    // Preserve explicit -1 sentinel slots (e.g. removed stock induction on aspiration conversions).
    for (const auto& s : cars::upgrade_slots())
      if (auto it = r.columns.find(s.column); it != r.columns.end()) c.slots[s.column] = it->second;
    if (auto it = r.columns.find("WheelStyle"); it != r.columns.end()) c.slots["WheelStyle"] = it->second;
    if (auto it = r.texts.find("LiveryFileName"); it != r.texts.end() && !it->second.empty())
      c.livery_file = it->second;
    for (const auto& [col, val] : r.reals) {
      if (col.rfind("Tuning_", 0) == 0) {
        char kv[96];
        std::snprintf(kv, sizeof kv, "%s=%.6f", col.c_str(), val);
        garage::parse_tune_token(kv, c.tune);
      }
    }
    for (const auto& [col, val] : r.columns) {
      if (col.rfind("Tuning_", 0) == 0) {
        garage::parse_tune_token(col + "=" + std::to_string(val), c.tune);
      }
    }
    g.cars.push_back(std::move(c));
  }
  for (const auto& b : p.parts) g.bought.push_back({b.garage_id, int(b.part_enum), b.part_id, b.manufacturer_id, b.price_paid});
  return g;
}

void from_drive_save(const drive::DriveSave& save, const garage::Garage& garage, Imported& out,
                     const Imported* base_template) {
  if (base_template) {
    out = *base_template;
  } else {
    out = Imported{};
  }

  const bool need_v4 = (base_template && base_template->version_flags == 2) || save.has_rally || !save.club.empty() ||
                       save.fast_travel_anywhere || !save.codriver_enabled;
  out.version_flags = need_v4 ? 2u : (base_template ? base_template->version_flags : 0u);
  if (find(out.body.fields, "VersionFlags") != nullptr) {
    set_field_int(out.body.fields, "VersionFlags", out.version_flags, Value::kU32);
  }

  // 1. Update Main/* fields in out.body.fields (preserving all unknown fields and structs).
  const int64_t clamped_credits = std::clamp<int64_t>(save.credits, 0, 999999999);
  const int64_t selected_car_id =
      garage.selected_car() ? garage.selected_car()->id : (base_template ? base_template->career_car : 0);
  const int64_t discount = std::min<int64_t>(100, int64_t(save.signs_found.size()));
  const int64_t wristband_lvl =
      save.collected_wristband >= 0 ? int64_t(save.collected_wristband) : (base_template ? base_template->wristband_level : 0);

  set_field_int(out.body.fields, "Main/Credits", clamped_credits, Value::kU32);
  set_field_int(out.body.fields, "Main/XP", save.wristband_points, Value::kI32);
  set_field_int(out.body.fields, "Main/WristbandLevel", wristband_lvl, Value::kI32);
  const int64_t main_level = profile_level_from_engine(save.level);
  set_field_int(out.body.fields, "Main/Level", main_level, Value::kU32);
  set_field_int(out.body.fields, "Main/CareerCar", selected_car_id, Value::kI32);
  set_field_int(out.body.fields, "Main/UpgradeDiscount", discount, Value::kU32);
  set_field_int(out.body.fields, "Main/TotalWinnings", save.total_winnings, Value::kI32);
  set_field_int(out.body.fields, "Main/NumRaces", save.num_races, Value::kI32);
  set_field_int(out.body.fields, "Main/NumVictories", save.num_victories, Value::kI32);
  set_field_int(out.body.fields, "Main/NumPodiums", save.num_podiums, Value::kI32);
  // Main has no Popularity or DriverXP field (popularity goes to the
  // SponsorshipState record below); the best skills car is MaxSkillsCarOrdinal.
  set_field_int(out.body.fields, "Main/MaxSkillsScore", save.max_skills_score, Value::kU32);
  set_field_int(out.body.fields, "Main/MaxSkillsCarOrdinal", save.max_skills_car, Value::kU32);
  set_field_double(out.body.fields, "Main/TopSpeed", save.top_speed, Value::kFloat);
  set_field_double(out.body.fields, "Main/TotalDistanceDriven", save.total_distance, Value::kFloat);
  set_field_double(out.body.fields, "Main/TimeInFestivalRaces", save.time_festival, Value::kFloat);
  set_field_double(out.body.fields, "Main/TimeInStreetRaces", save.time_street, Value::kFloat);
  set_field_double(out.body.fields, "Main/TimeInExhibitionRaces", save.time_exhibition, Value::kFloat);
  set_field_bool(out.body.fields, "Main/SatNav", save.satnav_enabled);

  if (need_v4 || find(out.body.fields, "Main/FastTravelAnywhere") != nullptr) {
    set_field_bool(out.body.fields, "Main/FastTravelAnywhere", save.fast_travel_anywhere);
  }
  if (need_v4 || find(out.body.fields, "Main/CoDriver") != nullptr) {
    set_field_bool(out.body.fields, "Main/CoDriver", save.codriver_enabled);
  }
  if (save.has_rally || find(out.body.fields, "Main/NumRallies") != nullptr) {
    set_field_int(out.body.fields, "Main/NumRallies", save.num_rallies, Value::kI32);
    set_field_int(out.body.fields, "Main/NumRallyStages", save.num_rally_stages, Value::kI32);
    set_field_int(out.body.fields, "Main/NumRallyVictories", save.num_rally_victories, Value::kI32);
    set_field_int(out.body.fields, "Main/RallyWinnings", save.rally_winnings, Value::kI32);
    set_field_int(out.body.fields, "Main/RallyXP", save.rally_xp, Value::kI32);
    set_field_int(out.body.fields, "Main/RallyWristbandLevel", save.rally_level, Value::kI32);
  }

  // 2. Ensure tail Block 1 (version 2) and Colorado CHorizonLocationState exist.
  if (out.body.blocks.empty()) {
    Block blk;
    blk.version = 2;
    out.body.blocks.push_back(std::move(blk));
  }
  Block& blk0 = out.body.blocks[0];
  Record* colorado = nullptr;
  for (auto& rec : blk0.records) {
    if (rec.name == "Colorado" && rec.class_name == "CHorizonLocationState") {
      colorado = &rec;
      break;
    }
  }
  if (!colorado) {
    Record rec;
    rec.name = "Colorado";
    rec.class_name = "CHorizonLocationState";
    rec.version = 2;
    blk0.records.push_back(std::move(rec));
    colorado = &blk0.records.back();
  }

  // Popularity and the sponsors (CHorizonSponsorshipState v2, SPEC_QUESTIONS
  // Q60): patched in place, or a fresh record without a template.
  SponsorshipState sponsorship;
  {
    sponsorship.popularity_points =
        uint32_t(std::llround(std::clamp(save.popularity_points, 0.0, double(UINT32_MAX))));
    uint32_t fame_count = 0;
    if (base_template)
      for (const auto& sc : base_template->sponsors)
        if (sc.id == "Fame") fame_count = sc.count;
    sponsorship.challenges.push_back({"Fame", uint32_t(std::max(0, save.fame_rewards_claimed)), fame_count});
    for (const auto& sp : save.sponsors)
      if (sp.id != "Fame")
        sponsorship.challenges.push_back({sp.id, uint32_t(std::max(0, sp.claimed)), uint32_t(std::max(0, sp.count))});
    Record* rec = nullptr;
    for (auto& r : blk0.records)
      if (r.name == "SponsorshipState" && r.class_name == "CHorizonSponsorshipState") rec = &r;
    if (rec) {
      rec->payload = encode_sponsorship_state(rec->payload, sponsorship);
    } else {
      Record r;
      r.name = "SponsorshipState";
      r.class_name = "CHorizonSponsorshipState";
      r.version = 2;
      r.payload = encode_sponsorship_state({}, sponsorship);
      blk0.records.push_back(std::move(r));
    }
    // Re-read so the mirror holds every child (template ones included).
    for (const auto& r : blk0.records)
      if (r.name == "SponsorshipState" && r.class_name == "CHorizonSponsorshipState")
        decode_sponsorship_state(r.payload, sponsorship);
    // `colorado` may have moved when the block grew.
    for (auto& r : blk0.records)
      if (r.name == "Colorado" && r.class_name == "CHorizonLocationState") colorado = &r;
  }

  // Discount signs (CFlyerState v1, 2 bytes: [revealed, found]).
  {
    std::set<std::string> found_set(save.signs_found.begin(), save.signs_found.end());
    std::set<std::string> seen;
    for (auto& c : colorado->children) {
      if (c.class_name != "CFlyerState") continue;
      if (c.payload.size() < 2) c.payload.resize(2, 0u);
      const bool is_found = found_set.count(c.name) != 0;
      c.payload[1] = is_found ? 1u : 0u;
      if (is_found || save.treasure_map) c.payload[0] = 1u;
      seen.insert(c.name);
    }
    for (const auto& name : save.signs_found) {
      if (seen.count(name)) continue;
      Record c;
      c.name = name;
      c.class_name = "CFlyerState";
      c.version = 1;
      c.payload = {1u, 1u};
      colorado->children.push_back(std::move(c));
      seen.insert(name);
    }
  }

  // Barn finds (CBarnFindState v3, 15 bytes).
  for (const auto& b : save.barns) {
    if (Record* existing = find_child(*colorado, b.name, "CBarnFindState")) {
      existing->payload = make_barn_payload(b);
    } else {
      Record c;
      c.name = b.name;
      c.class_name = "CBarnFindState";
      c.version = 3;
      c.payload = make_barn_payload(b);
      colorado->children.push_back(std::move(c));
    }
  }

  // Road discovery (CRoadDiscovery v2: u32 node_count + ceil(node_count/32) u32 words).
  if (save.road_discovery_nodes > 0) {
    const size_t min_words = size_t((save.road_discovery_nodes + 31u) / 32u);
    const size_t nwords = std::max(min_words, save.road_discovery_words.size());
    std::vector<uint8_t> payload;
    payload.reserve(4 + nwords * 4);
    push_be32(payload, save.road_discovery_nodes);
    for (size_t i = 0; i < nwords; ++i) {
      push_be32(payload, i < save.road_discovery_words.size() ? save.road_discovery_words[i] : 0u);
    }
    if (Record* existing = find_child_by_class(*colorado, "CRoadDiscovery")) {
      existing->payload = std::move(payload);
    } else {
      Record c;
      c.name = "RoadDiscovery";
      c.class_name = "CRoadDiscovery";
      c.version = 2;
      c.payload = std::move(payload);
      colorado->children.push_back(std::move(c));
    }
  }

  // Outposts (CGasStationState v1, 1 byte).
  for (const auto& name : save.discovered_outposts) {
    if (Record* existing = find_child(*colorado, name, "CGasStationState")) {
      if (existing->payload.empty()) existing->payload.resize(1, 1u);
      else existing->payload[0] = 1u;
    } else {
      Record c;
      c.name = name;
      c.class_name = "CGasStationState";
      c.version = 1;
      c.payload = {1u};
      colorado->children.push_back(std::move(c));
    }
  }

  // PR Stunts (CMissionState v1, 2 bytes).
  for (const auto& name : save.completed_pr_stunts) {
    if (Record* existing = find_child(*colorado, name, "CMissionState")) {
      if (existing->payload.size() < 2) existing->payload.resize(2, 1u);
      else existing->payload[0] = 1u, existing->payload[1] = 1u;
    } else {
      Record c;
      c.name = name;
      c.class_name = "CMissionState";
      c.version = 1;
      c.payload = {1u, 1u};
      colorado->children.push_back(std::move(c));
    }
  }

  // Speed Traps (CSpeedCameraState v1, 6 bytes: [1, 1, be_f32(mph)]).
  for (const auto& [name, mph] : save.speed_traps) {
    if (Record* existing = find_child(*colorado, name, "CSpeedCameraState")) {
      existing->payload = make_speed_payload(mph);
    } else {
      Record c;
      c.name = name;
      c.class_name = "CSpeedCameraState";
      c.version = 1;
      c.payload = make_speed_payload(mph);
      colorado->children.push_back(std::move(c));
    }
  }

  // Speed Zones (retail CAverageSpeedState v1, 6 bytes: [1, 1, be_f32(mph)]).
  for (const auto& [name, mph] : save.speed_zones) {
    Record* existing = find_child(*colorado, name, "CAverageSpeedState");
    if (!existing) existing = find_child(*colorado, name, "CSpeedZoneState");
    if (existing) {
      existing->class_name = "CAverageSpeedState";
      existing->payload = make_speed_payload(mph);
    } else {
      Record c;
      c.name = name;
      c.class_name = "CAverageSpeedState";
      c.version = 1;
      c.payload = make_speed_payload(mph);
      colorado->children.push_back(std::move(c));
    }
  }

  // 3. 1000 Club (CCarChallengeState v4, 8-byte stride save_progress entries).
  {
    Record* club_rec = nullptr;
    for (auto& rec : blk0.records) {
      if (rec.class_name == "CCarChallengeState") {
        club_rec = &rec;
        break;
      }
    }
    if (!save.club.empty() || club_rec != nullptr) {
      // Index template flag_b and raw_blobs by (car_id, challenge_key) for lossless round-trip.
      std::map<std::pair<int64_t, uint32_t>, bool> prev_flag_b;
      std::map<std::pair<int64_t, uint32_t>, std::vector<uint8_t>> prev_blobs;
      if (base_template) {
        for (const auto& tc : base_template->club) {
          for (size_t j = 0; j < tc.keys.size(); ++j) {
            const auto ck = std::make_pair(tc.car_id, tc.keys[j].first);
            if (j < tc.flags_b.size()) prev_flag_b[ck] = tc.flags_b[j];
            if (j < tc.raw_blobs.size()) prev_blobs[ck] = tc.raw_blobs[j];
          }
        }
      }
      std::vector<uint8_t> payload;
      push_be32(payload, uint32_t(save.club.size()));
      for (const auto& car : save.club) {
        push_be32(payload, uint32_t(car.car_id));
        push_be32(payload, uint32_t(car.challenges.size()));
        for (size_t j = 0; j < car.challenges.size(); ++j) {
          const uint32_t key = car.challenges[j].first;
          const bool flag_a = car.challenges[j].second;
          const auto ck = std::make_pair(car.car_id, key);
          const bool flag_b = prev_flag_b.count(ck) ? prev_flag_b[ck] : false;
          push_be32(payload, key);
          payload.push_back(flag_a ? 1u : 0u);
          payload.push_back(flag_b ? 1u : 0u);
          if (j < car.progress.size() && !car.progress[j].empty()) {
            const auto& prog = car.progress[j];
            push_be32(payload, uint32_t(prog.size() * 8u));
            for (int v : prog) push_be64(payload, uint64_t(std::max(0, v)));
          } else if (auto bit = prev_blobs.find(ck); bit != prev_blobs.end() && !bit->second.empty()) {
            push_be32(payload, uint32_t(bit->second.size()));
            payload.insert(payload.end(), bit->second.begin(), bit->second.end());
          } else {
            push_be32(payload, 0u);
          }
        }
      }
      if (base_template && !base_template->club_trailing.empty()) {
        payload.insert(payload.end(), base_template->club_trailing.begin(), base_template->club_trailing.end());
      } else {
        payload.insert(payload.end(), {0xFFu, 0xFFu, 0xFFu, 0xFFu});
      }
      if (!club_rec) {
        Record rec;
        rec.name = "CCarChallengeState";
        rec.class_name = "CCarChallengeState";
        rec.version = 4;
        rec.payload = std::move(payload);
        blk0.records.push_back(std::move(rec));
      } else {
        club_rec->payload = std::move(payload);
      }
    }
  }

  // 4. Rebuild PlayerDatabase rows (preserving untouched template columns per garage Id).
  std::map<int64_t, const GarageCar*> prev_cars;
  if (base_template) {
    for (const auto& g : base_template->garage) prev_cars[g.id] = &g;
  }
  std::map<int64_t, const drive::SavedCarHistory*> hist_by_gid;
  for (const auto& h : save.car_history) hist_by_gid[h.garage_id] = &h;

  out.garage.clear();
  int64_t max_gid = garage.last_id;
  for (const auto& c : garage.cars) {
    GarageCar g;
    if (auto it = prev_cars.find(c.id); it != prev_cars.end()) g = *it->second;
    g.id = c.id;
    g.car_id = c.car_id;
    g.performance_index = c.performance_index;
    g.class_id = c.class_id;
    g.color_index = c.colour;
    g.columns["Id"] = c.id;
    g.columns["CarId"] = c.car_id;
    g.reals["PerformanceIndex"] = c.performance_index;
    g.columns["ClassID"] = c.class_id;
    g.columns["PartsValue"] = c.parts_value;
    g.columns["DefaultManufacturerColorIndex"] = c.colour;
    // Copy every fitted slot, including explicit -1 sentinels.
    for (const auto& [col, val] : c.slots) g.columns[col] = val;
    if (!c.livery_file.empty()) g.texts["LiveryFileName"] = c.livery_file;
    // Serialize non-default Tuning_* sliders into g.reals.
    const std::string tune_tokens = garage::tune_to_tokens(c.tune);
    if (!tune_tokens.empty()) {
      std::istringstream ts(tune_tokens);
      for (std::string tok; ts >> tok;) {
        const auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = tok.substr(0, eq);
        const double val = std::strtod(tok.c_str() + eq + 1, nullptr);
        if (std::isfinite(val)) g.reals[key] = val;
      }
    }
    // Apply per-car history if present on DriveSave.
    if (auto hit = hist_by_gid.find(c.id); hit != hist_by_gid.end()) {
      const auto& h = *hit->second;
      g.history.distance_driven = h.distance_driven;
      g.history.time_driven = h.time_driven;
      g.history.total_winnings = h.total_winnings;
      g.history.total_repairs = h.total_repairs;
      g.history.num_races = h.num_races;
      g.history.num_victories = h.num_victories;
      g.history.num_podiums = h.num_podiums;
      g.reals["DistanceDriven"] = h.distance_driven;
      g.reals["TimeDriven"] = h.time_driven;
      g.columns["TotalWinnings"] = h.total_winnings;
      g.columns["TotalRepairs"] = h.total_repairs;
      g.columns["NumRaces"] = h.num_races;
      g.columns["NumVictories"] = h.num_victories;
      g.columns["NumPodiums"] = h.num_podiums;
    }
    max_gid = std::max(max_gid, c.id);
    out.garage.push_back(std::move(g));
  }
  out.last_used_garage_id = max_gid;

  out.parts.clear();
  for (const auto& b : garage.bought) {
    out.parts.push_back({b.garage_id, b.part_enum, b.part_id, b.manufacturer, b.price_paid});
  }
  out.event_results = save.event_results;
  out.race_results = save.race_results;
  out.event_unlocks = save.event_unlocks;
  out.hub_unlocks = save.hub_unlocks;

  // 5. Refresh Imported mirror fields so `out` is self-consistent in memory.
  out.main_fields = field_count(out.body.fields, "Main");
  out.credits = clamped_credits;
  out.xp = save.wristband_points;
  out.wristband_level = wristband_lvl;
  out.level = main_level;
  out.career_car = selected_car_id;
  out.upgrade_discount = discount;
  out.total_winnings = save.total_winnings;
  out.num_races = save.num_races;
  out.num_victories = save.num_victories;
  out.num_podiums = save.num_podiums;
  out.popularity = double(sponsorship.popularity_points);
  out.has_sponsorship = true;
  out.sponsors = sponsorship.challenges;
  out.fame_tiers_claimed = 0;
  if (const SponsorshipChallenge* fame = sponsorship.find("Fame")) out.fame_tiers_claimed = fame->tiers_claimed;
  out.max_skills_score = save.max_skills_score;
  out.max_skills_car = save.max_skills_car;
  out.top_speed = save.top_speed;
  out.distance_driven = save.total_distance;
  out.time_festival = save.time_festival;
  out.time_street = save.time_street;
  out.time_exhibition = save.time_exhibition;
  out.rally_fields = find(out.body.fields, "Main/NumRallies") != nullptr;
  out.num_rallies = save.num_rallies;
  out.num_rally_stages = save.num_rally_stages;
  out.num_rally_victories = save.num_rally_victories;
  out.rally_winnings = save.rally_winnings;
  out.rally_xp = save.rally_xp;
  out.rally_level = save.rally_level;
  out.satnav_on = save.satnav_enabled;
  out.fast_travel_anywhere = save.fast_travel_anywhere;
  out.has_codriver = find(out.body.fields, "Main/CoDriver") != nullptr;
  out.codriver_on = save.codriver_enabled;
  out.road_discovery_nodes = save.road_discovery_nodes;
  out.road_discovery_words = save.road_discovery_words;
  out.outposts_discovered = save.discovered_outposts;
  out.pr_stunts_completed = save.completed_pr_stunts;
  out.speed_traps = save.speed_traps;
  out.speed_zones = save.speed_zones;
  out.signs.clear();
  for (const auto& s : save.signs_found) out.signs.push_back({s, true, true});
  out.barns.clear();
  for (const auto& b : save.barns) {
    out.barns.push_back(
        {b.name, b.status != 3, b.status == 1 || b.status == 2, b.status == 2, float(b.rumour_at), float(b.found_at),
         uint32_t(b.status)});
  }
  out.club.clear();
  for (const auto& c : save.club) {
    ClubCar cc;
    cc.car_id = c.car_id;
    cc.challenges = int(c.challenges.size());
    for (const auto& ch : c.challenges) cc.done += ch.second ? 1 : 0;
    cc.keys = c.challenges;
    cc.progress = c.progress;
    out.club.push_back(std::move(cc));
  }
}

bool export_forza_profile(const drive::DriveSave& save, const garage::Garage& garage,
                          const std::filesystem::path& out_folder, std::string* error, const ExportOptions& opts) {
  Imported loaded_template;
  const Imported* base = opts.base_profile;
  std::filesystem::path template_db;
  if (!opts.template_folder.empty()) {
    if (!base) {
      if (!import_profile(opts.template_folder, loaded_template, error)) return false;
      base = &loaded_template;
    }
    template_db = opts.template_folder / "PlayerDatabase.sqlite";
  }
  Imported exported;
  from_drive_save(save, garage, exported, base);
  if (opts.version_flags >= 0) {
    exported.version_flags = uint32_t(opts.version_flags);
    if (find(exported.body.fields, "VersionFlags") != nullptr) {
      set_field_int(exported.body.fields, "VersionFlags", exported.version_flags, Value::kU32);
    }
  }
  return export_forza_profile(exported, out_folder, error, template_db);
}

}  // namespace profile
