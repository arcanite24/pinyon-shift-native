#include "race/event.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>

#include "cars/sources.h"
#include "cars/strings.h"
#include "db_util.h"
#include "race/route.h"

namespace race {
namespace {

using Db = detail::RaceDb;

double d(sqlite3_stmt* s, int c) { return sqlite3_column_double(s, c); }
int64_t i64(sqlite3_stmt* s, int c) { return sqlite3_column_int64(s, c); }
std::string txt(sqlite3_stmt* s, int c) {
  const auto* t = sqlite3_column_text(s, c);
  return t ? reinterpret_cast<const char*>(t) : "";
}

std::string lower_ascii(std::string s) {
  for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

uint32_t hsv_to_rgb24(double h, double s, double v) {
  if (h > 1.0) h = std::fmod(h, 360.0) / 360.0;
  h = std::fmod(std::fmod(h, 1.0) + 1.0, 1.0);
  if (s > 1.0) s *= 0.01;
  if (v > 1.0) v *= 0.01;
  s = std::clamp(s, 0.0, 1.0);
  v = std::clamp(v, 0.0, 1.0);
  const double c = v * s;
  const double hp = h * 6.0;
  const double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
  double r = 0, g = 0, b = 0;
  if (hp < 1.0) r = c, g = x;
  else if (hp < 2.0) r = x, g = c;
  else if (hp < 3.0) g = c, b = x;
  else if (hp < 4.0) g = x, b = c;
  else if (hp < 5.0) r = x, b = c;
  else r = c, b = x;
  const double m = v - c;
  const uint32_t ri = uint32_t(std::clamp(std::lround((r + m) * 255.0), 0l, 255l));
  const uint32_t gi = uint32_t(std::clamp(std::lround((g + m) * 255.0), 0l, 255l));
  const uint32_t bi = uint32_t(std::clamp(std::lround((b + m) * 255.0), 0l, 255l));
  return (ri << 16) | (gi << 8) | bi;
}

bool read_ai_player_colors(Db& db, std::vector<AiPlayerColor>& out) {
  out.clear();
  std::vector<std::string> cols;
  std::string err;
  if (!db.each("PRAGMA table_info(AIPlayerColors)", {}, [&](sqlite3_stmt* s) { cols.push_back(txt(s, 1)); }, &err) ||
      cols.empty())
    return false;
  auto find_col = [&](std::initializer_list<const char*> names) -> std::string {
    for (const char* want : names) {
      for (const auto& c : cols) {
        if (lower_ascii(c) == want) return c;
      }
    }
    return {};
  };
  const std::string c_id = find_col({"id"});
  const std::string c_ai = find_col({"aiplayerid", "aiplayer_id", "driverid", "playerid"});
  const std::string c_rgb = find_col({"rgb", "colorrgb", "color", "primarycolor", "paintcolor", "color1"});
  const std::string c_r = find_col({"r", "red", "colorr", "color_r"});
  const std::string c_g = find_col({"g", "green", "colorg", "color_g"});
  const std::string c_b = find_col({"b", "blue", "colorb", "color_b"});
  const std::string c_h = find_col({"hue", "h", "colorhue", "paint_h"});
  const std::string c_s = find_col({"saturation", "sat", "s", "colorsat", "paint_s"});
  const std::string c_v = find_col({"brightness", "value", "val", "lightness", "l", "v", "paint_v"});
  const std::string c_met = find_col({"metallic", "ismetallic"});
  const std::string c_seq = find_col({"colorsequence", "sequence"});

  std::string sql = "select ";
  sql += c_id.empty() ? "rowid" : ("coalesce(" + c_id + ", 0)");
  sql += ", " + (c_ai.empty() ? std::string("0") : ("coalesce(" + c_ai + ", 0)"));
  sql += ", " + (c_rgb.empty() ? std::string("''") : ("coalesce(" + c_rgb + ", '')"));
  sql += ", " + (c_r.empty() ? std::string("-1") : ("coalesce(" + c_r + ", -1)"));
  sql += ", " + (c_g.empty() ? std::string("-1") : ("coalesce(" + c_g + ", -1)"));
  sql += ", " + (c_b.empty() ? std::string("-1") : ("coalesce(" + c_b + ", -1)"));
  sql += ", " + (c_h.empty() ? std::string("-1") : ("coalesce(" + c_h + ", -1)"));
  sql += ", " + (c_s.empty() ? std::string("-1") : ("coalesce(" + c_s + ", -1)"));
  sql += ", " + (c_v.empty() ? std::string("-1") : ("coalesce(" + c_v + ", -1)"));
  sql += ", " + (c_met.empty() ? std::string("0") : ("coalesce(" + c_met + ", 0)"));
  sql += ", " + (c_seq.empty() ? std::string("0") : ("coalesce(" + c_seq + ", 0)"));
  sql += " from AIPlayerColors order by " + (c_id.empty() ? std::string("rowid") : c_id);

  const bool ok = db.each(
      sql, {},
      [&](sqlite3_stmt* s) {
        AiPlayerColor row;
        row.id = i64(s, 0);
        row.ai_player_id = i64(s, 1);
        row.metallic = i64(s, 9) != 0;
        row.color_sequence = i64(s, 10);
        if (!c_rgb.empty()) {
          const int ctype = sqlite3_column_type(s, 2);
          if (ctype == SQLITE_INTEGER) {
            row.rgb = uint32_t(i64(s, 2)) & 0xFFFFFFu;
            row.has_rgb = true;
          } else {
            std::string raw = txt(s, 2);
            if (!raw.empty()) {
              const char* p = raw.c_str();
              if (*p == '#') ++p;
              else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
              char* end = nullptr;
              const bool hex_hint = (!raw.empty() && raw[0] == '#') || raw.find_first_of("abcdefABCDEFxX") != std::string::npos;
              const unsigned long parsed = std::strtoul(p, &end, hex_hint ? 16 : 10);
              if (end != p) {
                row.rgb = uint32_t(parsed) & 0xFFFFFFu;
                row.has_rgb = true;
              }
            }
          }
        } else if (!c_r.empty() && !c_g.empty() && !c_b.empty()) {
          double rv = d(s, 3), gv = d(s, 4), bv = d(s, 5);
          if (rv >= 0.0 && gv >= 0.0 && bv >= 0.0) {
            if (rv <= 1.0 && gv <= 1.0 && bv <= 1.0) rv *= 255.0, gv *= 255.0, bv *= 255.0;
            const uint32_t ri = uint32_t(std::clamp(std::lround(rv), 0l, 255l));
            const uint32_t gi = uint32_t(std::clamp(std::lround(gv), 0l, 255l));
            const uint32_t bi = uint32_t(std::clamp(std::lround(bv), 0l, 255l));
            row.rgb = (ri << 16) | (gi << 8) | bi;
            row.has_rgb = true;
          }
        } else if (!c_h.empty() && !c_s.empty() && !c_v.empty()) {
          const double hv = d(s, 6), sv = d(s, 7), vv = d(s, 8);
          if (hv >= 0.0 && sv >= 0.0 && vv >= 0.0) {
            row.rgb = hsv_to_rgb24(hv, sv, vv);
            row.has_rgb = true;
          }
        }
        out.push_back(row);
      },
      &err);
  return ok && !out.empty();
}

}  // namespace

bool parse_difficulty(const std::string& s0, Difficulty& out) {
  std::string s = s0;
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (s == "easy") out = Difficulty::kEasy;
  else if (s == "med" || s == "medium") out = Difficulty::kMed;
  else if (s == "hard") out = Difficulty::kHard;
  else if (s == "pro" || s == "insane") out = Difficulty::kPro;
  else if (s == "hard+" || s == "hard-plus" || s == "hardplus" || s == "unbeatable") out = Difficulty::kHardPlus;
  else return false;
  return true;
}

bool parse_ai_mode(const std::string& s0, AiMode& out) {
  std::string s = s0;
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (s == "stock" || s == "default") out = AiMode::kStock;
  else if (s == "fair" || s == "no-rubberband" || s == "clean") out = AiMode::kFair;
  else if (s == "hard" || s == "hard+" || s == "pinyon") out = AiMode::kHard;
  else return false;
  return true;
}

bool is_pinyon_hard(Difficulty d, AiMode mode) {
  return d == Difficulty::kHardPlus || mode == AiMode::kHard;
}

int effective_db_difficulty(Difficulty d, AiMode mode) {
  int base = std::clamp(int(d), 0, 3);
  if (d == Difficulty::kHardPlus) return 3;
  if (mode == AiMode::kHard && base < 3) ++base;
  return base;
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
const AiPlayerColor* EventData::ai_player_color(const Opponent& opp) const {
  if (ai_player_colors.empty()) return nullptr;
  if (opp.ai_player_id > 0) {
    for (const auto& c : ai_player_colors)
      if (c.ai_player_id == opp.ai_player_id) return &c;
  }
  const uint64_t key = opp.ai_player_id > 0 ? uint64_t(opp.ai_player_id) : uint64_t(std::max<int64_t>(0, opp.participant_id));
  return &ai_player_colors[size_t(key % ai_player_colors.size())];
}

double stock_performance_index(const std::filesystem::path& game_root, const std::string& media,
                               const std::filesystem::path& state_root, const cars::PartsDb* shared_db) {
  Db db;
  if (!db.open(game_root, state_root, shared_db, nullptr)) return -1;
  double pi = -1;
  db.each("select PerformanceIndex from Data_Car where lower(MediaName) = lower(?) limit 1", {media},
          [&](sqlite3_stmt* s) { pi = d(s, 0); }, nullptr);
  return pi;
}

bool load_event(const std::filesystem::path& game_root, const std::string& horizon_id, Difficulty difficulty,
                EventData& out, std::string* error, const std::filesystem::path& state_root,
                const cars::PartsDb* shared_db, AiMode ai_mode) {
  Db db;
  if (!db.open(game_root, state_root, shared_db, error)) return false;
  out = EventData{};
  out.horizon_id = horizon_id;
  static const char* kSuffix[] = {"Easy", "Med", "Hard", "Pro"};
  const std::string sfx = kSuffix[effective_db_difficulty(difficulty, ai_mode)];
  bool has_tod_start_col = false, has_tod_end_col = false;
  {
    std::string pragma_err;
    db.each("PRAGMA table_info(Events)", {},
            [&](sqlite3_stmt* s) {
              const std::string col = lower_ascii(txt(s, 1));
              if (col == "timeofdaystart") has_tod_start_col = true;
              else if (col == "timeofdayend") has_tod_end_col = true;
            },
            &pragma_err);
  }
  const std::string tod_start_expr = has_tod_start_col ? "TimeOfDayStart" : "NULL";
  const std::string tod_end_expr = has_tod_end_col ? "coalesce(TimeOfDayEnd, 0)" : "0";
  bool found = false;
  if (!db.each("select Id, AISkill" + sfx + ", AITemperament" + sfx + ", AIRubberband" + sfx +
                   ", PlayerGridPosition, CashPrize, coalesce(Level, 0), coalesce(CareerEventStyle, 0), "
                   "coalesce(CareerTypeId, 0), coalesce(UpgradeAI, 0), coalesce(TargetClass, 0), coalesce(Name, ''), " +
                   tod_start_expr + ", " + tod_end_expr + " from Events where HorizonEventID = ?",
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
                 if (has_tod_start_col && sqlite3_column_type(s, 12) != SQLITE_NULL) {
                   const double start_sec = d(s, 12);
                   const double end_sec = has_tod_end_col ? d(s, 13) : 0.0;
                   if (start_sec > 0.0 || end_sec > 0.0) {
                     out.has_tod_start = true;
                     out.tod_start_sec = std::fmod(std::fmod(start_sec, 86400.0) + 86400.0, 86400.0);
                     out.tod_end_sec = end_sec > 0.0 ? std::fmod(std::fmod(end_sec, 86400.0) + 86400.0, 86400.0) : 0.0;
                   }
                 }
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
  if (!db.each("select RewardCarId, coalesce(Position, 1) from Rewards_EventPrizes where EventId = ? order by Id limit 1",
               {ev},
               [&](sqlite3_stmt* s) {
                 out.prize_car = i64(s, 0);
                 out.prize_car_max_place = std::max(1, int(i64(s, 1)));
               },
               nullptr)) {
    if (!db.each("select RewardCarId from Rewards_EventPrizes where EventId = ? order by Id limit 1", {ev},
                 [&](sqlite3_stmt* s) { out.prize_car = i64(s, 0); }, error))
      return false;
  }
  auto load_participant = [&](sqlite3_stmt* s, bool has_tuning_livery) {
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
    if (has_tuning_livery) {
      o.tuning_level = int(i64(s, 10));
      o.livery_level = int(i64(s, 11));
    }
    out.opponents.push_back(std::move(o));
  };
  if (!db.each("select p.ID, p.Ordinal, coalesce(c.MediaName, ''), p.AIPlayerID, coalesce(a.Name, ''), "
               "coalesce(a.SkillModifier, 0), coalesce(a.AggroModifier, 0), coalesce(a.RubberBandModifier, 0), "
               "coalesce(c.PerformanceIndex, 0), coalesce(p.ColorSequence, 0), "
               "coalesce(p.TuningLevel, 0), coalesce(p.LiveryLevel, 0) "
               "from EventParticipants p left join AIPlayers a on a.Id = p.AIPlayerID "
               "left join Data_Car c on c.Id = p.Ordinal where p.EventID = ? order by p.ID",
               {ev}, [&](sqlite3_stmt* s) { load_participant(s, true); }, nullptr)) {
    out.opponents.clear();
    if (!db.each("select p.ID, p.Ordinal, coalesce(c.MediaName, ''), p.AIPlayerID, coalesce(a.Name, ''), "
                 "coalesce(a.SkillModifier, 0), coalesce(a.AggroModifier, 0), coalesce(a.RubberBandModifier, 0), "
                 "coalesce(c.PerformanceIndex, 0), coalesce(p.ColorSequence, 0) "
                 "from EventParticipants p left join AIPlayers a on a.Id = p.AIPlayerID "
                 "left join Data_Car c on c.Id = p.Ordinal where p.EventID = ? order by p.ID",
                 {ev}, [&](sqlite3_stmt* s) { load_participant(s, false); }, error))
      return false;
  }
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
  if (!db.each("select Credits from EventScoring where ScoringID = 1 order by place", {},
               [&](sqlite3_stmt* s) { out.scoring.push_back(d(s, 0)); }, error))
    return false;
  // Optional in minimal synthetic test databases.
  std::string adj_err;
  db.each("select TimeRatio, SkillAdjustment from AISkillAdjustments order by TimeRatio", {},
          [&](sqlite3_stmt* s) { out.skill_adjustments.emplace_back(int(i64(s, 0)), int(i64(s, 1))); }, &adj_err);
  read_ai_player_colors(db, out.ai_player_colors);
  return true;
}

bool event_census(const std::filesystem::path& game_root, EventCensus& out, std::string* error,
                  const std::filesystem::path& state_root, const cars::PartsDb* shared_db) {
  Db db;
  if (!db.open(game_root, state_root, shared_db, error)) return false;
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

bool list_events(const std::filesystem::path& game_root, std::vector<EventSummary>& out, std::string* error,
                 const std::filesystem::path& state_root, const cars::PartsDb* shared_db) {
  Db db;
  if (!db.open(game_root, state_root, shared_db, error)) return false;
  out.clear();
  return db.each(
      "select e.HorizonEventID, e.CareerTypeId, t.RibbonConfiguration, t.RouteId, r.NumLaps, "
      "(select count(*) from EventParticipants p where p.EventID = e.Id), coalesce(e.Name, ''), "
      "coalesce(t.EnvironmentId, 0) "
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
        e.environment_id = int(i64(s, 7));
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
                    std::string* error, const std::string& language, const cars::PartsDb* shared_db) {
  static std::mutex lock;
  struct Cached {
    std::map<std::string, Row> rows;
    size_t resolved = 0;
    std::shared_ptr<const fh1::StringTables> strings;
  };
  static std::map<std::string, Cached> cache;
  std::string lang;
  lang.reserve(language.size());
  for (char c : language) lang.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  if (lang.empty()) lang = "EN";
  const bool merged = (shared_db && shared_db->is_open()) || !state_root.empty();
  const std::string key = game_root.string() + "|" + state_root.string() + "|" + lang + (merged ? "|1" : "|0");
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
    if (cars::discover_install(game_root, state_root, "", install, &why)) {
      if (!cars::load_string_tables(install, strings, &why, lang)) {
        if (lang == "EN" || !cars::load_string_tables(install, strings, &why, "EN"))
          strings = {};  // the media names stand in
      }
    } else {
      strings = {};  // the media names stand in
    }
  }
  Db db;
  if (!db.open(game_root, state_root, shared_db, error)) return false;
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

bool load_ai_player_names(const std::filesystem::path& game_root, std::vector<std::string>& out,
                          const CarNames* names, const std::filesystem::path& state_root,
                          const cars::PartsDb* db_in) {
  out.clear();
  Db db;
  if (!db.open(game_root, state_root, db_in, nullptr)) return false;
  db.each("select coalesce(Name, '') from AIPlayers order by Id", {},
          [&](sqlite3_stmt* s) {
            std::string raw = txt(s, 0);
            if (raw.empty()) return;
            if (names) raw = names->text(raw);
            if (!raw.empty() && !fh1::StringTables::is_reference(raw)) out.push_back(std::move(raw));
          },
          nullptr);
  return !out.empty();
}

bool load_ai_player_colors(const std::filesystem::path& game_root, std::vector<AiPlayerColor>& out,
                           const std::filesystem::path& state_root, const cars::PartsDb* db_in) {
  out.clear();
  Db db;
  if (!db.open(game_root, state_root, db_in, nullptr)) return false;
  return read_ai_player_colors(db, out);
}

}  // namespace race
