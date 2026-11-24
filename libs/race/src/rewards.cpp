#include "race/rewards.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>
#include <utility>

#include "cars/sources.h"
#include "cars/strings.h"
#include "db_util.h"
#include "fh1data/stringtables.h"

namespace race {
namespace {

std::string resolve_wristband_name(const std::string& raw, const std::string& thumbnail,
                                   const fh1::StringTables* strings, size_t tier_index) {
  if (!raw.empty() && !fh1::StringTables::is_reference(raw)) return raw;
  if (strings && fh1::StringTables::is_reference(raw)) {
    if (auto resolved = strings->resolve(raw);
        resolved && !resolved->empty() && !fh1::StringTables::is_reference(*resolved)) {
      return *resolved;
    }
  }
  // Exact CareerWristbandLevels localization hashes from FH1 gamedb.slt.
  static constexpr std::pair<std::string_view, const char*> kKnownHashes[] = {
      {"_&2698127755", "Yellow"},
      {"_&2698127627", "Green"},
      {"_&2698127499", "Blue"},
      {"_&2698127371", "Pink"},
      {"_&2698128267", "Orange"},
      {"_&2698128139", "Purple"},
      {"_&2698128011", "Gold"},
  };
  for (const auto& [hash_ref, label] : kKnownHashes) {
    if (raw == hash_ref) return label;
  }
  if (!thumbnail.empty() && !fh1::StringTables::is_reference(thumbnail)) return thumbnail;
  static constexpr const char* kTierNames[] = {"Yellow", "Green", "Blue", "Pink", "Orange", "Purple", "Gold"};
  if (tier_index < std::size(kTierNames)) return kTierNames[tier_index];
  return raw;
}

}  // namespace

Winnings winnings_for_career_type(int t) {
  if (t == 12) return Winnings::kWinnerTakesAll;
  if (t == 4 || t == 11) return Winnings::kTopThree;
  return Winnings::kDefault;
}

int64_t round_prize(double x) {
  if (!(x >= 10)) return 0;
  const double s = std::pow(10.0, std::floor(std::log10(x)) - 1);
  return int64_t(std::floor(x / s + 0.5) * s + 0.5);
}

int64_t race_credits(double prize, Winnings w, int n, int p, bool finished) {
  if (!finished || n <= 1 || p < 1) return 0;
  double share = 0;
  switch (w) {
    case Winnings::kDefault: share = std::trunc(prize * double(n - p + 1) / double(n)); break;
    case Winnings::kWinnerTakesAll: share = p == 1 ? prize : 0; break;
    case Winnings::kTopThree: share = p == 1 ? prize : p == 2 ? prize / 2 : p == 3 ? prize / 4 : 0; break;
  }
  return round_prize(share);  // times 1.0 offline, truncated
}

int WristbandTables::points_for(int level, int place) const {
  if (place < 1) return 0;
  auto it = points.find(std::max(0, level));
  if (it == points.end()) return 0;
  auto jt = it->second.find(place);
  return jt == it->second.end() ? 0 : jt->second;
}

int WristbandTables::level_index(int total) const {
  int best = -1;
  for (size_t i = 0; i < levels.size(); ++i)
    if (total >= levels[i].first) best = int(i);
  return best;
}

int64_t WristbandTables::nemesis_bonus_for(int level) const {
  if (nemesis_bonuses.empty()) return 0;
  const int idx = std::clamp(level, 0, int(nemesis_bonuses.size()) - 1);
  return nemesis_bonuses[size_t(idx)];
}

bool load_wristband_tables(const std::filesystem::path& root, WristbandTables& out, std::string* error) {
  return load_wristband_tables(root, {}, nullptr, out, error);
}

bool load_wristband_tables(const std::filesystem::path& root, const std::filesystem::path& state_root,
                           const cars::PartsDb* shared_db, WristbandTables& out, std::string* error) {
  detail::RaceDb db;
  if (!db.open(root, state_root, shared_db, error)) return false;
  out = WristbandTables{};
  if (!db.each("select WristbandLevel, Place, Points from WristbandScoring", {},
               [&](sqlite3_stmt* s) {
                 out.points[sqlite3_column_int(s, 0)][sqlite3_column_int(s, 1)] = sqlite3_column_int(s, 2);
               },
               error))
    return false;
  fh1::StringTables strings;
  bool have_strings = false;
  if (!root.empty()) {
    cars::Install install;
    std::string why;
    if (cars::discover_install(root, state_root, "", install, &why) &&
        cars::load_string_tables(install, strings, &why, "EN")) {
      have_strings = true;
    }
  }
  size_t tier_idx = 0;
  std::string thumb_err;
  if (db.each("select XP, coalesce(Name, ''), coalesce(NemesisBonus, 0), coalesce(Thumbnail, '') "
              "from CareerWristbandLevels order by XP",
              {},
              [&](sqlite3_stmt* s) {
                const auto* t = sqlite3_column_text(s, 1);
                const auto* th = sqlite3_column_text(s, 3);
                const std::string raw = t ? reinterpret_cast<const char*>(t) : "";
                const std::string thumb = th ? reinterpret_cast<const char*>(th) : "";
                out.levels.emplace_back(
                    sqlite3_column_int(s, 0),
                    resolve_wristband_name(raw, thumb, have_strings ? &strings : nullptr, tier_idx++));
                out.nemesis_bonuses.push_back(sqlite3_column_int64(s, 2));
              },
              &thumb_err)) {
    return true;
  }
  out.levels.clear();
  out.nemesis_bonuses.clear();
  tier_idx = 0;
  return db.each("select XP, coalesce(Name, ''), coalesce(NemesisBonus, 0) from CareerWristbandLevels order by XP", {},
                 [&](sqlite3_stmt* s) {
                   const auto* t = sqlite3_column_text(s, 1);
                   const std::string raw = t ? reinterpret_cast<const char*>(t) : "";
                   out.levels.emplace_back(
                       sqlite3_column_int(s, 0),
                       resolve_wristband_name(raw, "", have_strings ? &strings : nullptr, tier_idx++));
                   out.nemesis_bonuses.push_back(sqlite3_column_int64(s, 2));
                 },
                 error);
}

int ProgressionTables::driver_level_for_xp(int xp) const {
  if (driver_levels.empty()) return 1;
  int lvl = driver_levels.front().level + 1;
  for (const auto& row : driver_levels)
    if (xp >= row.xp) lvl = row.level + 1;
  return lvl;
}

int ProgressionTables::next_driver_level_xp(int xp) const {
  for (const auto& row : driver_levels)
    if (row.xp > xp) return row.xp;
  return driver_levels.empty() ? 0 : driver_levels.back().xp;
}

int64_t ProgressionTables::driver_level_gift_car(int level) const {
  const auto it = driver_level_cars.find(level);
  if (it == driver_level_cars.end() || it->second.empty()) return 0;
  return it->second.front();
}

int ProgressionTables::affinity_level_for_xp(int xp) const {
  int lvl = 0;
  for (const auto& row : affinity_levels)
    if (xp >= row.xp) lvl = row.level;
  return lvl;
}

double ProgressionTables::affinity_discount_for_xp(int xp) const {
  double disc = 0.0;
  for (const auto& row : affinity_levels)
    if (xp >= row.xp) disc = row.discount;
  return std::clamp(disc, 0.0, 1.0);
}

int64_t ProgressionTables::affinity_bonus_for_level(int level) const {
  for (const auto& row : affinity_levels)
    if (row.level == level) return row.bonus_credits;
  return 0;
}

bool load_progression_tables(const std::filesystem::path& root, ProgressionTables& out, std::string* error) {
  return load_progression_tables(root, {}, nullptr, out, error);
}

bool load_progression_tables(const std::filesystem::path& root, const std::filesystem::path& state_root,
                             const cars::PartsDb* shared_db, ProgressionTables& out, std::string* error) {
  detail::RaceDb db;
  if (!db.open(root, state_root, shared_db, error)) return false;
  out = ProgressionTables{};
  if (!db.each("select Id, XP, coalesce(MaxGiftCars, 0) from PlayerLevelDef order by Id", {},
               [&](sqlite3_stmt* s) {
                 DriverLevelRow r;
                 r.level = sqlite3_column_int(s, 0);
                 r.xp = sqlite3_column_int(s, 1);
                 r.max_gift_cars = sqlite3_column_int(s, 2);
                 out.driver_levels.push_back(r);
               },
               error))
    return false;
  if (!db.each("select DriverLevel, RewardCarId from Rewards_DriverLevelv3 order by DriverLevel, SortOrder", {},
               [&](sqlite3_stmt* s) {
                 const int lvl = sqlite3_column_int(s, 0);
                 const int64_t cid = sqlite3_column_int64(s, 1);
                 if (cid > 0) out.driver_level_cars[lvl].push_back(cid);
               },
               error))
    return false;
  return db.each("select Id, XP, coalesce(Discount, 0), coalesce(BonusCredits, 0) from AffinityLevelDef order by Id", {},
                 [&](sqlite3_stmt* s) {
                   AffinityLevelRow r;
                   r.level = sqlite3_column_int(s, 0);
                   r.xp = sqlite3_column_int(s, 1);
                   r.discount = sqlite3_column_double(s, 2);
                   r.bonus_credits = sqlite3_column_int64(s, 3);
                   out.affinity_levels.push_back(r);
                 },
                 error);
}

ProgressionUpdate apply_progression_xp(CareerRecord& record, int driver_xp_delta, int64_t make_id,
                                       int affinity_xp_delta, const ProgressionTables& tables) {
  ProgressionUpdate u;
  u.old_driver_level = std::max(1, tables.driver_levels.empty() ? record.driver_level
                                                                : tables.driver_level_for_xp(record.driver_xp));
  if (driver_xp_delta > 0) record.driver_xp += driver_xp_delta;
  u.new_driver_level = std::max(u.old_driver_level,
                                tables.driver_levels.empty() ? record.driver_level
                                                             : tables.driver_level_for_xp(record.driver_xp));
  record.driver_level = u.new_driver_level;
  for (int lv = u.old_driver_level + 1; lv <= u.new_driver_level; ++lv) {
    const int64_t car = tables.driver_level_gift_car(lv);
    if (car > 0) u.gift_cars.push_back(car);
  }
  if (make_id > 0) {
    int& cur_xp = record.make_affinity_xp[make_id];
    u.old_affinity_level = tables.affinity_level_for_xp(cur_xp);
    if (affinity_xp_delta > 0) cur_xp += affinity_xp_delta;
    u.new_affinity_level = tables.affinity_level_for_xp(cur_xp);
    u.affinity_discount = tables.affinity_discount_for_xp(cur_xp);
    for (int lv = u.old_affinity_level + 1; lv <= u.new_affinity_level; ++lv) {
      const int64_t bonus = tables.affinity_bonus_for_level(lv);
      if (bonus > 0) {
        u.affinity_bonus_credits += bonus;
        record.credits = std::min<int64_t>(999999999, record.credits + bonus);
        record.total_winnings += bonus;
      }
    }
  }
  return u;
}

double rewind_difficulty_bonus(bool rewind_on, int session_mode) {
  if (session_mode == DifficultyAssists::kExcludedSessionMode) return 0.0;
  return rewind_on ? 0.0 : 0.20;
}

double assist_difficulty_bonus(const DifficultyAssists& assists) {
  // SPEC-REWIND section 7: Rewind OFF = +20%, ON = +0% (excluded in session mode 3).
  // SPEC-EVENT-REWARDS Unknowns: remaining assists (ABS, TCS, STM, steering,
  // shifting, driving line) are interim 0% until confirmed.
  return rewind_difficulty_bonus(assists.rewind_on, assists.session_mode);
}

int assist_difficulty_bonus_percent(const DifficultyAssists& assists) {
  return int(std::lround(assist_difficulty_bonus(assists) * 100.0));
}

int difficulty_bonus_percent(int difficulty_index) {
  switch (difficulty_index) {
    case 0: return 0;
    case 1: return 15;
    case 2: return 30;
    case 3: return 45;
    case 4: return 60;  // Pinyon Shift HARD+ (+1 tier, no rubber-banding)
    default: return 0;
  }
}

int total_difficulty_bonus_percent(int difficulty_index, const DifficultyAssists& assists) {
  return difficulty_bonus_percent(difficulty_index) + assist_difficulty_bonus_percent(assists);
}

int64_t mileage_bonus_credits(double distance_m) {
  if (!(distance_m > 0)) return 0;
  return round_prize(distance_m * 0.05);
}

RaceAward award_race(CareerRecord& r, int64_t event_id, int64_t race_id, int career_type, int style, int level,
                     double prize, int racers, int place, bool finished, double race_time, double top_speed,
                     int64_t prize_car, const WristbandTables& tables, int difficulty_pct, double distance_m,
                     const RaceBonusInputs& extras) {
  RaceAward a;
  const auto prev = r.event_places.find(event_id);
  const bool first_win = (prev == r.event_places.end() || prev->second > 1);
  a.base_credits = race_credits(prize, winnings_for_career_type(career_type), racers, place, finished);
  // SPEC-EVENT-REWARDS: Style-6 (Horizon Finale) awards 50,000 CR on first win.
  if (finished && style == 6 && place == 1 && first_win && a.base_credits < 50000) a.base_credits = 50000;
  a.mileage_bonus = finished ? mileage_bonus_credits(distance_m) : 0;
  const int total_pct = std::max(0, difficulty_pct);
  a.assists_pct = std::clamp(extras.assists_pct, 0, total_pct);
  a.difficulty_pct = total_pct;
  const int ai_pct = std::max(0, total_pct - a.assists_pct);
  const double bonus_base = double(a.base_credits + a.mileage_bonus);
  a.difficulty_bonus = finished && ai_pct > 0 ? int64_t(std::llround(bonus_base * ai_pct / 100.0)) : 0;
  a.assists_bonus = finished && a.assists_pct > 0 ? int64_t(std::llround(bonus_base * a.assists_pct / 100.0)) : 0;
  a.skills_bonus = finished && extras.skills_points > 0 ? round_prize(double(extras.skills_points) * 0.25) : 0;
  a.bounty_bonus = finished && extras.bounty_bonus > 0 ? extras.bounty_bonus : 0;
  if (finished && (career_type == 11 || style == 5) && place == 1) {
    const int wb_lvl = extras.wristband_level >= 0 ? extras.wristband_level : std::max(0, tables.level_index(r.wristband_points));
    a.nemesis_bonus = tables.nemesis_bonus_for(wb_lvl);
  }
  a.credits = a.base_credits + a.mileage_bonus + a.difficulty_bonus + a.assists_bonus + a.skills_bonus +
              a.bounty_bonus + a.nemesis_bonus;
  if (!finished) return a;
  // Driver XP & Manufacturer Affinity XP earned from race finish + distance.
  a.driver_xp = int(std::max<int64_t>(500, a.base_credits / 4 + int64_t(distance_m * 0.25) + (place == 1 ? 1000 : 0)));
  a.affinity_xp = int(std::max<int64_t>(250, a.base_credits / 6 + int64_t(distance_m * 0.15) + (place == 1 ? 500 : 0)));
  // Wristband points: an improvement only (the previous best from the event record).
  if (style == 1) {
    const int before = prev == r.event_places.end() ? 0 : tables.points_for(level, prev->second);
    a.wristband_points = std::max(0, tables.points_for(level, place) - before);
  }
  // Festival Pacing reward multiplier (0.5x..5.0x; 1.0x leaves rewards unchanged).
  const double rm = std::clamp(extras.reward_multiplier, 0.5, 5.0);
  if (std::fabs(rm - 1.0) > 1e-4) {
    a.credits = a.credits > 0 ? std::max<int64_t>(1, int64_t(std::llround(double(a.credits) * rm))) : 0;
    a.wristband_points = a.wristband_points > 0 ? std::max(1, int(std::lround(double(a.wristband_points) * rm))) : 0;
    a.driver_xp = a.driver_xp > 0 ? std::max(1, int(std::lround(double(a.driver_xp) * rm))) : 0;
    a.affinity_xp = a.affinity_xp > 0 ? std::max(1, int(std::lround(double(a.affinity_xp) * rm))) : 0;
  }
  // 1. The race record; 2. the event record (one race: its place).
  auto rp = r.race_places.find(race_id);
  if (rp == r.race_places.end()) r.race_places[race_id] = place;
  else rp->second = std::min(rp->second, place);
  if (prev == r.event_places.end()) {
    r.event_places[event_id] = place;
    a.first_completion = true;
  } else {
    prev->second = std::min(prev->second, place);
  }
  a.won = place == 1;
  // 3. The counters (podiums in every mode: which modes count is open).
  r.credits = std::min<int64_t>(999999999, r.credits + a.credits);
  r.total_winnings += a.credits;
  ++r.num_races;
  if (place == 1) ++r.num_victories;
  if (place <= 3) ++r.num_podiums;
  if (career_type == 4 || career_type == 11) r.time_in_street += race_time;
  else if (career_type == 7 || career_type == 8) r.time_in_exhibition += race_time;
  else r.time_in_festival += race_time;
  r.top_speed = std::max(r.top_speed, top_speed);
  r.wristband_points += a.wristband_points;
  // 4. The prize car, once, for place 1.
  if (prize_car && place == 1 && std::find(r.prize_cars.begin(), r.prize_cars.end(), prize_car) == r.prize_cars.end()) {
    r.prize_cars.push_back(prize_car);
    a.prize_car = prize_car;
  }
  return a;
}

}  // namespace race
