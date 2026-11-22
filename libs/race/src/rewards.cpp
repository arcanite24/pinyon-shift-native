#include "race/rewards.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>

namespace race {

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

bool load_wristband_tables(const std::filesystem::path& root, WristbandTables& out, std::string* error) {
  sqlite3* db = nullptr;
  const auto p = (root / "media" / "db" / "gamedb.slt").string();
  if (sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (error) *error = "cannot open " + p;
    if (db) sqlite3_close(db);
    return false;
  }
  out = WristbandTables{};
  bool ok = true;
  auto each = [&](const char* sql, auto row) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
      if (error) *error = std::string("gamedb: ") + sqlite3_errmsg(db);
      ok = false;
      return;
    }
    while (sqlite3_step(st) == SQLITE_ROW) row(st);
    sqlite3_finalize(st);
  };
  each("select WristbandLevel, Place, Points from WristbandScoring", [&](sqlite3_stmt* s) {
    out.points[sqlite3_column_int(s, 0)][sqlite3_column_int(s, 1)] = sqlite3_column_int(s, 2);
  });
  if (ok)
    each("select XP, coalesce(Name, '') from CareerWristbandLevels order by XP", [&](sqlite3_stmt* s) {
      const auto* t = sqlite3_column_text(s, 1);
      out.levels.emplace_back(sqlite3_column_int(s, 0), t ? reinterpret_cast<const char*>(t) : "");
    });
  sqlite3_close(db);
  return ok;
}

int difficulty_bonus_percent(int difficulty_index) {
  switch (difficulty_index) {
    case 0: return 0;
    case 1: return 15;
    case 2: return 30;
    case 3: return 45;
    default: return 0;
  }
}

int64_t mileage_bonus_credits(double distance_m) {
  if (!(distance_m > 0)) return 0;
  return round_prize(distance_m * 0.05);
}

RaceAward award_race(CareerRecord& r, int64_t event_id, int64_t race_id, int career_type, int style, int level,
                     double prize, int racers, int place, bool finished, double race_time, double top_speed,
                     int64_t prize_car, const WristbandTables& tables, int difficulty_pct, double distance_m) {
  RaceAward a;
  a.base_credits = race_credits(prize, winnings_for_career_type(career_type), racers, place, finished);
  a.mileage_bonus = finished ? mileage_bonus_credits(distance_m) : 0;
  a.difficulty_pct = std::max(0, difficulty_pct);
  a.difficulty_bonus =
      finished && a.difficulty_pct > 0 ? int64_t(std::llround(double(a.base_credits + a.mileage_bonus) * a.difficulty_pct / 100.0)) : 0;
  a.credits = a.base_credits + a.mileage_bonus + a.difficulty_bonus;
  if (!finished) return a;
  // Wristband points: an improvement only (the previous best from the event record).
  const auto prev = r.event_places.find(event_id);
  if (style == 1) {
    const int before = prev == r.event_places.end() ? 0 : tables.points_for(level, prev->second);
    a.wristband_points = std::max(0, tables.points_for(level, place) - before);
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
