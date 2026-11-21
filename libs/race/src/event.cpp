#include "race/event.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <mutex>

#include "cars/sources.h"
#include "cars/strings.h"
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

double stock_performance_index(const std::filesystem::path& game_root, const std::string& media) {
  Db db;
  if (!db.open(game_root, nullptr)) return -1;
  double pi = -1;
  db.each("select PerformanceIndex from Data_Car where lower(MediaName) = lower(?) limit 1", {media},
          [&](sqlite3_stmt* s) { pi = d(s, 0); }, nullptr);
  return pi;
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
                   ", PlayerGridPosition, CashPrize, coalesce(Level, 0), coalesce(CareerEventStyle, 0), "
                   "coalesce(CareerTypeId, 0), coalesce(UpgradeAI, 0), coalesce(TargetClass, 0), coalesce(Name, '') from Events where HorizonEventID = ?",
               {horizon_id},
               [&](sqlite3_stmt* s) {
                 found = true;
                 out.event_id = i64(s, 0);
                 out.event_skill = int(i64(s, 1));
                 out.event_temperament = int(i64(s, 2));
                 out.event_rubberband = int(i64(s, 3));
                 out.player_grid_position = int(i64(s, 4));
                 out.cash_prize = d(s, 5);
                 out.level = int(i64(s, 6));
                 out.style = int(i64(s, 7));
                 out.career_type = int(i64(s, 8));
                 out.upgrade_ai = i64(s, 9) != 0;
                 out.target_class = int(i64(s, 10));
                 out.name = txt(s, 11);
               },
               error))
    return false;
  if (!found) {
    if (error) *error = "gamedb: no event " + horizon_id;
    return false;
  }
  found = false;
  const std::string ev = std::to_string(out.event_id);
  if (!db.each("select r.TrackId, r.NumLaps, t.RouteId, t.Length, r.Id from Races r join Tracks t on t.id = r.TrackId "
               "where r.EventId = ? order by r.RaceNumber limit 1",
               {ev},
               [&](sqlite3_stmt* s) {
                 found = true;
                 out.track_id = i64(s, 0);
                 out.laps = std::max(1, int(i64(s, 1)));
                 out.route_id = int(i64(s, 2));
                 out.track_length = d(s, 3);
                 out.race_id = i64(s, 4);
               },
               error))
    return false;
  if (!found) {
    if (error) *error = "gamedb: event " + horizon_id + " has no race and track";
    return false;
  }
  if (!db.each("select RewardCarId from Rewards_EventPrizes where EventId = ? order by Id limit 1", {ev},
               [&](sqlite3_stmt* s) { out.prize_car = i64(s, 0); }, error))
    return false;
  if (!db.each("select p.ID, p.Ordinal, coalesce(c.MediaName, ''), p.AIPlayerID, coalesce(a.Name, ''), "
               "coalesce(a.SkillModifier, 0), coalesce(a.AggroModifier, 0), coalesce(a.RubberBandModifier, 0), "
               "coalesce(c.PerformanceIndex, 0), coalesce(p.ColorSequence, 0) "
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
                 o.performance_index = d(s, 8);
                 o.color_sequence = i64(s, 9);
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

bool list_events(const std::filesystem::path& game_root, std::vector<EventSummary>& out, std::string* error) {
  Db db;
  if (!db.open(game_root, error)) return false;
  out.clear();
  return db.each(
      "select e.HorizonEventID, e.CareerTypeId, t.RibbonConfiguration, t.RouteId, r.NumLaps, "
      "(select count(*) from EventParticipants p where p.EventID = e.Id), coalesce(e.Name, '') "
      "from Events e join Races r on r.EventId = e.Id join Tracks t on t.id = r.TrackId order by e.HorizonEventID",
      {},
      [&](sqlite3_stmt* s) {
        EventSummary e;
        e.horizon_id = txt(s, 0);
        e.career_type = int(i64(s, 1));
        e.ribbon = int(i64(s, 2));
        e.route_id = int(i64(s, 3));
        e.laps = std::max(1, int(i64(s, 4)));
        e.opponents = int(i64(s, 5));
        e.name = txt(s, 6);
        out.push_back(std::move(e));
      },
      error);
}

std::string CarNames::split_words(const std::string& s) {
  std::string out;
  auto up = [](char c) { return c >= 'A' && c <= 'Z'; };
  auto low = [](char c) { return c >= 'a' && c <= 'z'; };
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i], p = i ? s[i - 1] : ' ', n = i + 1 < s.size() ? s[i + 1] : ' ';
    const bool cut = i > 0 && ((up(c) && low(p)) || (up(c) && (up(p) || digit(p)) && low(n)) || (digit(c) && low(p)));
    if (cut && !out.empty() && out.back() != ' ') out += ' ';
    out += c == '_' ? ' ' : c;
  }
  return out;
}

bool CarNames::load(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                    std::string* error) {
  static std::mutex lock;
  struct Cached {
    std::map<std::string, Row> rows;
    size_t resolved = 0;
    std::shared_ptr<const fh1::StringTables> strings;
  };
  static std::map<std::string, Cached> cache;
  const std::string key = game_root.string() + "|" + state_root.string();
  {
    std::lock_guard<std::mutex> g(lock);
    if (auto it = cache.find(key); it != cache.end()) {
      rows_ = it->second.rows, resolved_ = it->second.resolved, strings_ = it->second.strings;
      return true;
    }
  }
  auto loaded = std::make_shared<fh1::StringTables>();
  fh1::StringTables& strings = *loaded;
  {
    cars::Install install;
    std::string why;
    if (!cars::discover_install(game_root, state_root, "", install, &why) ||
        !cars::load_string_tables(install, strings, &why))
      strings = {};  // the media names stand in
  }
  Db db;
  if (!db.open(game_root, error)) return false;
  rows_.clear();
  resolved_ = 0;
  const bool ok = db.each(
      "select c.MediaName, coalesce(c.Year, 0), coalesce(m.IconPathBase, ''), coalesce(c.ModelShort, ''), "
      "coalesce(m.DisplayName, ''), coalesce(c.DisplayName, '') from Data_Car c left join List_CarMake m on m.ID = c.MakeID",
      {},
      [&](sqlite3_stmt* s) {
        const std::string media = txt(s, 0);
        Row r;
        r.year = int(i64(s, 1));
        const auto make = strings.resolve(txt(s, 4));
        // DisplayName is the model's full name ("Skyline GT-R V-spec II");
        // ModelShort an abbreviation ("GT-R R34"), the fallback.
        auto model = strings.resolve(txt(s, 5));
        if (!model || model->empty()) model = strings.resolve(txt(s, 3));
        r.make = make && !make->empty() ? *make : split_words(txt(s, 2));
        if (model && !model->empty()) {
          r.model = *model;
          ++resolved_;
        } else {
          // MAKE_Model_Words_YY: the parts between the make code and the year.
          const size_t a = media.find('_'), b = media.rfind('_');
          if (a != std::string::npos && b != std::string::npos && b > a)
            r.model = split_words(media.substr(a + 1, b - a - 1));
        }
        rows_[media] = std::move(r);
      },
      error);
  if (ok) {
    std::lock_guard<std::mutex> g(lock);
    strings_ = loaded;
    cache[key] = {rows_, resolved_, strings_};
  }
  return ok;
}

std::string CarNames::short_name(const std::string& media) const {
  auto it = rows_.find(media);
  if (it == rows_.end() || it->second.model.empty()) return media;
  const std::string& make = it->second.make;
  const std::string& model = it->second.model;
  // "Jeep Grand Cherokee", not "Jeep Jeep Grand Cherokee".
  const bool has_make = !make.empty() && model.size() >= make.size() &&
                        std::equal(make.begin(), make.end(), model.begin(), [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                        }) &&
                        (model.size() == make.size() || model[make.size()] == ' ');
  return make.empty() || has_make ? model : make + " " + model;
}

std::string CarNames::text(const std::string& value) const {
  if (!strings_ || !fh1::StringTables::is_reference(value)) return value;
  auto t = strings_->resolve(value);
  return t ? *t : value;
}

std::string CarNames::full_name(const std::string& media) const {
  auto it = rows_.find(media);
  const std::string s = short_name(media);
  return it != rows_.end() && it->second.year > 0 ? std::to_string(it->second.year) + " " + s : s;
}

}  // namespace race
