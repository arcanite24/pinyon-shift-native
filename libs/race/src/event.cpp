#include "race/event.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <functional>

#include "race/route.h"

namespace race {
namespace {

struct Db {
  sqlite3* db = nullptr;
  ~Db() {
    if (db) sqlite3_close(db);
  }
  bool open(const std::filesystem::path& game_root, std::string* error) {
    const auto p = (game_root / "media" / "db" / "gamedb.slt").string();
    if (sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      if (error) *error = "cannot open " + p;
      return false;
    }
    return true;
  }
  // Runs `sql` with text parameters, calling `row` per result row.
  bool each(const std::string& sql, const std::vector<std::string>& params, const std::function<void(sqlite3_stmt*)>& row,
            std::string* error) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
      if (error) *error = std::string("gamedb: ") + sqlite3_errmsg(db);
      return false;
    }
    for (size_t i = 0; i < params.size(); ++i)
      sqlite3_bind_text(st, int(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) row(st);
    sqlite3_finalize(st);
    return true;
  }
};

double d(sqlite3_stmt* s, int c) { return sqlite3_column_double(s, c); }
int64_t i64(sqlite3_stmt* s, int c) { return sqlite3_column_int64(s, c); }
std::string txt(sqlite3_stmt* s, int c) {
  const auto* t = sqlite3_column_text(s, c);
  return t ? reinterpret_cast<const char*>(t) : "";
}

}  // namespace

bool parse_difficulty(const std::string& s0, Difficulty& out) {
  std::string s = s0;
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (s == "easy") out = Difficulty::kEasy;
  else if (s == "med" || s == "medium") out = Difficulty::kMed;
  else if (s == "hard") out = Difficulty::kHard;
  else if (s == "pro") out = Difficulty::kPro;
  else return false;
  return true;
}

const SkillRow* EventData::skill(int id) const {
  for (const auto& r : skills)
    if (r.id == id) return &r;
  return nullptr;
}
const RubberbandRow* EventData::rubberband(int id) const {
  for (const auto& r : rubberbands)
    if (r.id == id) return &r;
  return nullptr;
}
const TemperamentRow* EventData::temperament(int id) const {
  for (const auto& r : temperaments)
    if (r.id == id) return &r;
  return nullptr;
}

bool load_event(const std::filesystem::path& game_root, const std::string& horizon_id, Difficulty difficulty,
                EventData& out, std::string* error) {
  Db db;
  if (!db.open(game_root, error)) return false;
  out = EventData{};
  out.horizon_id = horizon_id;
  static const char* kSuffix[] = {"Easy", "Med", "Hard", "Pro"};
  const std::string sfx = kSuffix[int(difficulty)];
  bool found = false;
  if (!db.each("select Id, AISkill" + sfx + ", AITemperament" + sfx + ", AIRubberband" + sfx +
                   ", PlayerGridPosition, CashPrize from Events where HorizonEventID = ?",
               {horizon_id},
               [&](sqlite3_stmt* s) {
                 found = true;
                 out.event_id = i64(s, 0);
                 out.event_skill = int(i64(s, 1));
                 out.event_temperament = int(i64(s, 2));
                 out.event_rubberband = int(i64(s, 3));
                 out.player_grid_position = int(i64(s, 4));
                 out.cash_prize = d(s, 5);
               },
               error))
    return false;
  if (!found) {
    if (error) *error = "gamedb: no event " + horizon_id;
    return false;
  }
  found = false;
  const std::string ev = std::to_string(out.event_id);
  if (!db.each("select r.TrackId, r.NumLaps, t.RouteId, t.Length from Races r join Tracks t on t.id = r.TrackId "
               "where r.EventId = ? order by r.RaceNumber limit 1",
               {ev},
               [&](sqlite3_stmt* s) {
                 found = true;
                 out.track_id = i64(s, 0);
                 out.laps = std::max(1, int(i64(s, 1)));
                 out.route_id = int(i64(s, 2));
                 out.track_length = d(s, 3);
               },
               error))
    return false;
  if (!found) {
    if (error) *error = "gamedb: event " + horizon_id + " has no race and track";
    return false;
  }
  if (!db.each("select p.ID, p.Ordinal, coalesce(c.MediaName, ''), p.AIPlayerID, coalesce(a.Name, ''), "
               "coalesce(a.SkillModifier, 0), coalesce(a.AggroModifier, 0), coalesce(a.RubberBandModifier, 0) "
               "from EventParticipants p left join AIPlayers a on a.Id = p.AIPlayerID "
               "left join Data_Car c on c.Id = p.Ordinal where p.EventID = ? order by p.ID",
               {ev},
               [&](sqlite3_stmt* s) {
                 Opponent o;
                 o.participant_id = i64(s, 0);
                 o.car_id = i64(s, 1);
                 o.car_media = txt(s, 2);
                 o.ai_player_id = i64(s, 3);
                 o.driver = txt(s, 4);
                 o.skill_modifier = int(i64(s, 5));
                 o.aggro_modifier = int(i64(s, 6));
                 o.rubberband_modifier = int(i64(s, 7));
                 out.opponents.push_back(std::move(o));
               },
               error))
    return false;
  if (!db.each("select id, SoloLapping_BrakingMinimum, SoloLapping_BrakingMaximum, SoloLapping_CorneringMinimum, "
               "SoloLapping_CorneringMaximum, SoloLapping_DistBehindForMaxPerformance, SoloLapping_NominalTorqueScale, "
               "SoloLapping_FarBehindTorqueBoost, Rubberbands_id from AISkills order by id",
               {},
               [&](sqlite3_stmt* s) {
                 SkillRow r;
                 r.id = int(i64(s, 0));
                 r.braking_min = d(s, 1), r.braking_max = d(s, 2);
                 r.cornering_min = d(s, 3), r.cornering_max = d(s, 4);
                 r.dist_behind_for_max = d(s, 5);
                 r.nominal_torque_scale = d(s, 6);
                 r.far_behind_torque_boost = d(s, 7);
                 r.rubberband_id = int(i64(s, 8));
                 out.skills.push_back(r);
               },
               error))
    return false;
  if (!db.each("select id, StartRubberbandingDistance, MaxRubberbandingEffectDistance, StartTorqueCutDistance, "
               "MaxTorqueCutDistance, TorqueCutFactor, StartCatchUpDistance, StopCatchUpDistance, CatchUpFactor, "
               "CatchUpGapPerPosition from AIRubberbands order by id",
               {},
               [&](sqlite3_stmt* s) {
                 RubberbandRow r;
                 r.id = int(i64(s, 0));
                 r.start_rubberbanding = d(s, 1), r.max_effect = d(s, 2);
                 r.start_torque_cut = d(s, 3), r.max_torque_cut = d(s, 4), r.torque_cut_factor = d(s, 5);
                 r.start_catch_up = d(s, 6), r.stop_catch_up = d(s, 7), r.catch_up_factor = d(s, 8);
                 r.catch_up_gap_per_position = d(s, 9);
                 out.rubberbands.push_back(r);
               },
               error))
    return false;
  if (!db.each("select AIRubberbands_id, AIRubberbandModes_id, RelativeFrequency, Amount from AIRubberbandModeChoices "
               "order by Id",
               {},
               [&](sqlite3_stmt* s) {
                 for (auto& r : out.rubberbands)
                   if (r.id == int(i64(s, 0))) r.modes.push_back({int(i64(s, 1)), d(s, 2), d(s, 3)});
               },
               error))
    return false;
  if (!db.each("select id, Passing_AggressionLevel, Passing_TrailingDistance, Passing_CarClearance, "
               "Passing_TrackEdgeClearance from AITemperaments order by id",
               {},
               [&](sqlite3_stmt* s) {
                 out.temperaments.push_back({int(i64(s, 0)), d(s, 1), d(s, 2), d(s, 3), d(s, 4)});
               },
               error))
    return false;
  return db.each("select Credits from EventScoring where ScoringID = 1 order by place", {},
                 [&](sqlite3_stmt* s) { out.scoring.push_back(d(s, 0)); }, error);
}

bool event_census(const std::filesystem::path& game_root, EventCensus& out, std::string* error) {
  Db db;
  if (!db.open(game_root, error)) return false;
  out = EventCensus{};
  auto count = [&](const char* t, int& n) {
    return db.each(std::string("select count(*) from ") + t, {}, [&](sqlite3_stmt* s) { n = int(i64(s, 0)); }, error);
  };
  if (!count("Events", out.events) || !count("Races", out.races) || !count("Tracks", out.tracks)) return false;
  std::vector<int> routes;
  if (!db.each("select t.RouteId from Events e join Races r on r.EventId = e.Id join Tracks t on t.id = r.TrackId", {},
               [&](sqlite3_stmt* s) { routes.push_back(int(i64(s, 0))); }, error))
    return false;
  for (int r : routes)
    if (std::filesystem::exists(route_path(game_root, r))) ++out.resolved;
  return true;
}

}  // namespace race
