#include "race/rally_championship.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

#include "audio/xml.h"
#include "cars/physics.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"
#include "race/rewards.h"
#include "sqlite3.h"

namespace race {
namespace {

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string col_text(sqlite3_stmt* st, int col) {
  const unsigned char* s = sqlite3_column_text(st, col);
  return s ? reinterpret_cast<const char*>(s) : "";
}

std::string resolve_text(const fh1::StringTables* strings, const std::string& raw, const std::string& fallback) {
  if (strings && !raw.empty()) {
    if (auto r = strings->resolve(raw); r && !r->empty() && *r != raw && r->rfind("_&", 0) != 0) return *r;
  }
  if (!raw.empty() && raw.rfind("_&", 0) != 0) return raw;
  return fallback;
}

std::string default_championship_name(const std::string& horizon_id) {
  if (horizon_id == "MS_RALLY_01") return "Rockies Rally";
  if (horizon_id == "MS_RALLY_02") return "Clear Springs Rally";
  if (horizon_id == "MS_RALLY_03") return "North Plains Rally";
  if (horizon_id == "MS_RALLY_04") return "Montano Plains Rally";
  if (horizon_id == "MS_RALLY_05") return "Beaumont Rally";
  if (horizon_id == "MS_RALLY_06") return "Kettle Hills Rally";
  if (horizon_id == "MS_RALLY_07") return "Red Rock Rally";
  return horizon_id;
}

bool read_v4_member(const std::filesystem::path& state_root, const char* member, std::vector<uint8_t>& out,
                    std::string* error) {
  std::string why;
  auto zip = fh1::Archive::open(state_root / "title-update-v4" / "media.zip", &why);
  if (!zip) return fail(error, "title-update-v4/media.zip: " + why);
  auto r = zip->read(member);
  if (!r.ok()) return fail(error, std::string(member) + ": " + r.error);
  out = std::move(r.data);
  return true;
}

struct TimingMap {
  std::map<int64_t, std::vector<double>> best_splits;
};

bool load_all_timings(const std::filesystem::path& state_root, TimingMap& out, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!read_v4_member(state_root, "gametunablesettings/rallytracktimings.xml", bytes, error)) return false;
  audio::XmlNode root;
  std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!audio::parse_xml(sv, root, error)) return false;
  out.best_splits.clear();
  for (const auto& t : root.children) {
    if (!audio::iequals(t.name, "Track")) continue;
    const int64_t tid = int64_t(t.number("Id"));
    auto& v = out.best_splits[tid];
    for (const auto& s : t.children) v.push_back(s.number("Time"));
  }
  return !out.best_splits.empty();
}

void upsert_best_result(std::vector<std::pair<int64_t, int>>& list, int64_t id, int place) {
  for (auto& [eid, best] : list) {
    if (eid == id) {
      if (place < best) best = place;
      return;
    }
  }
  list.emplace_back(id, place);
}

bool add_unique_unlock(std::vector<int64_t>& unlocks, int64_t id) {
  if (id <= 0) return false;
  if (std::find(unlocks.begin(), unlocks.end(), id) != unlocks.end()) return false;
  unlocks.push_back(id);
  return true;
}

}  // namespace

const char* rally_class_name(int target_class) {
  switch (target_class) {
    case 1: return "E";
    case 2: return "D";
    case 3: return "C";
    case 4: return "B";
    case 5: return "A";
    case 6: return "S";
    case 7: return "R3";
    case 8: return "R2";
    case 9: return "R1";
    default: return "B";
  }
}

std::filesystem::path find_rally_package(const std::filesystem::path& state_root) {
  namespace fs = std::filesystem;
  if (state_root.empty()) return {};
  const fs::path content = state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002";
  std::error_code ec;
  if (!fs::is_directory(content, ec)) return {};
  for (const auto& entry : fs::directory_iterator(content, ec)) {
    if (!entry.is_directory(ec)) continue;
    const fs::path p = entry.path();
    if (fs::exists(p / "Media" / "1600.puboffer", ec) ||
        fs::exists(p / "Media" / "DLCZips" / "ColoradoDirt_pri_65.zip", ec)) {
      return p;
    }
  }
  return {};
}

const RallyChampionshipDef* RallyCatalog::find_championship(int64_t event_id) const {
  for (const auto& c : championships)
    if (c.event_id == event_id) return &c;
  return nullptr;
}

const RallyChampionshipDef* RallyCatalog::find_by_horizon_id(const std::string& horizon_id) const {
  for (const auto& c : championships)
    if (c.horizon_id == horizon_id) return &c;
  return nullptr;
}

int RallyCatalog::points_for_place(int place) const {
  if (place < 1 || size_t(place) >= scoring.size()) return 0;
  return scoring[size_t(place)];
}

int RallyCatalog::wristband_for_xp(int xp) const {
  int lvl = 0;
  for (const auto& w : wristbands)
    if (xp >= w.xp_required) lvl = w.level;
  return lvl;
}

std::string RallyCatalog::wristband_name(int level) const {
  for (const auto& w : wristbands)
    if (w.level == level) return w.name;
  return level >= 1 ? "Horizon Rally Champion" : "Horizon Rally Yellow";
}

int RallyCatalog::next_wristband_xp(int level) const {
  for (const auto& w : wristbands)
    if (w.level > level) return w.xp_required;
  return -1;
}

bool is_rally_heats_completed(const drive::DriveSave& save) {
  for (const auto& [eid, place] : save.event_results)
    if (eid == 279 && place > 0) return true;
  if (save.has_rally && (save.num_rallies > 0 || save.num_rally_stages > 0)) return true;
  for (int64_t id : save.event_unlocks)
    if (id == 275) return true;
  return false;
}

bool RallyCatalog::is_championship_unlocked(int64_t event_id, int rally_xp, bool heats_done,
                                            const std::vector<int64_t>& event_unlocks,
                                            const std::vector<std::pair<int64_t, int>>& event_results) const {
  const auto* ch = find_championship(event_id);
  if (!ch) return false;
  if (rally_xp < ch->unlock_points_req) return false;

  // First championship in hub order (MS_RALLY_05, series_id 1) unlocks once RALLY_HEATS is completed.
  if (ch->series_id == 1) {
    if (heats_done) return true;
  }

  // Explicit unlock row (from Rewards_EventUnlock or imported profile).
  if (std::find(event_unlocks.begin(), event_unlocks.end(), event_id) != event_unlocks.end()) return true;

  // Or unlocked because its predecessor championship in Rewards_EventUnlock has a podium finish (1st-3rd).
  for (const auto& prev : championships) {
    if (prev.unlocks_event_id == event_id) {
      for (const auto& [eid, place] : event_results)
        if (eid == prev.event_id && place >= 1 && place <= 3) return true;
    }
  }
  return false;
}

int RallyCatalog::championships_won(const drive::DriveSave& save) const {
  int won = 0;
  for (const auto& ch : championships) {
    for (const auto& [eid, place] : save.event_results) {
      if (eid == ch.event_id && place == 1) {
        ++won;
        break;
      }
    }
  }
  return won;
}

int RallyCatalog::podiums_count(const drive::DriveSave& save) const {
  int podiums = 0;
  for (const auto& ch : championships) {
    for (const auto& [eid, place] : save.event_results) {
      if (eid == ch.event_id && place >= 1 && place <= 3) {
        ++podiums;
        break;
      }
    }
  }
  return podiums;
}

int RallyCatalog::stages_completed_total(const drive::DriveSave& save) const {
  std::set<int64_t> done;
  for (const auto& [eid, place] : save.event_results) {
    if (place > 0) done.insert(eid);
  }
  int stages = 0;
  for (const auto& ch : championships) {
    if (done.count(ch.event_id) > 0) {
      stages += int(ch.stages.size());
    } else {
      for (const auto& st : ch.stages) {
        if (done.count(st.event_id) > 0) ++stages;
      }
    }
  }
  return stages;
}

bool RallyCatalog::champion_unlocked(const drive::DriveSave& save) const {
  const bool has_champion_band = (save.rally_level >= 1) || (wristband_for_xp(save.rally_xp) >= 1);
  if (!has_champion_band) return false;
  for (const auto& ch : championships) {
    if (ch.unlocks_event_id != 0) continue;
    for (const auto& [eid, place] : save.event_results) {
      if (eid == ch.event_id && place >= 1 && place <= 3) return true;
    }
  }
  return false;
}

RallyProgress RallyCatalog::summarize_progress(const drive::DriveSave& save) const {
  RallyProgress p;
  p.championships_won_count = championships_won(save);
  p.podiums = podiums_count(save);
  p.stages_completed = stages_completed_total(save);
  p.champion = champion_unlocked(save);
  return p;
}

int64_t RallyCatalog::championship_credits(int64_t cash_prize, int place, int diff_pct, int assists_pct) {
  if (cash_prize <= 0 || place < 1) return 0;
  const int64_t base = race_credits(double(cash_prize), Winnings::kDefault, 50, place, true);
  const int pct = std::max(0, diff_pct) + std::max(0, assists_pct);
  const int64_t bonus = (base * pct) / 100;
  return base + bonus;
}

bool load_rally_catalog(const std::filesystem::path& state_root, const cars::PartsDb* shared_db,
                        const fh1::StringTables* strings, RallyCatalog& out, std::string* error) {
  out = {};
  out.package_dir = find_rally_package(state_root);

  TimingMap timings;
  if (!load_all_timings(state_root, timings, error)) return false;
  if (!load_rally_field(state_root, out.field, error)) return false;

  // Open either the shared PartsDb or v4's gamedb.slt in-memory.
  sqlite3* owned_db = nullptr;
  auto close_owned = [&] {
    if (owned_db) {
      sqlite3_close(owned_db);
      owned_db = nullptr;
    }
  };

  auto query = [&](const char* sql, const std::vector<std::string>& args,
                   const std::function<void(sqlite3_stmt*)>& fn) -> bool {
    if (shared_db && shared_db->is_open()) {
      return shared_db->each(sql, args, fn, error);
    }
    if (!owned_db) {
      std::vector<uint8_t> db_bytes;
      if (!read_v4_member(state_root, "db/gamedb.slt", db_bytes, error)) return false;
      if (sqlite3_open_v2(":memory:", &owned_db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        return fail(error, "sqlite open");
      auto* copy = static_cast<unsigned char*>(sqlite3_malloc64(db_bytes.size()));
      std::memcpy(copy, db_bytes.data(), db_bytes.size());
      if (sqlite3_deserialize(owned_db, "main", copy, sqlite3_int64(db_bytes.size()), sqlite3_int64(db_bytes.size()),
                              SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE) != SQLITE_OK) {
        close_owned();
        return fail(error, "sqlite deserialize");
      }
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(owned_db, sql, -1, &st, nullptr) != SQLITE_OK) {
      return fail(error, std::string("sqlite prepare: ") + sqlite3_errmsg(owned_db));
    }
    for (size_t i = 0; i < args.size(); ++i)
      sqlite3_bind_text(st, int(i + 1), args[i].c_str(), int(args[i].size()), SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) fn(st);
    sqlite3_finalize(st);
    return true;
  };

  // 1. RallyScoring (places 1..50).
  out.scoring.assign(51, 0);
  if (!query("select Place, Points from RallyScoring order by Place", {}, [&](sqlite3_stmt* st) {
        const int place = sqlite3_column_int(st, 0);
        const int pts = sqlite3_column_int(st, 1);
        if (place >= 1 && place <= 50) out.scoring[size_t(place)] = pts;
      })) {
    close_owned();
    return false;
  }

  // 2. RallyWristbandLevels (2 rows: Yellow 0 XP, Green Champion 310 XP).
  if (!query("select Id, XP, Name, Thumbnail from RallyWristbandLevels order by Id", {}, [&](sqlite3_stmt* st) {
        RallyWristbandDef w;
        w.level = sqlite3_column_int(st, 0);
        w.xp_required = sqlite3_column_int(st, 1);
        w.thumbnail = col_text(st, 3);
        const std::string fallback = w.level == 0 ? "Horizon Rally Yellow" : "Horizon Rally Champion";
        w.name = resolve_text(strings, col_text(st, 2), fallback);
        out.wristbands.push_back(std::move(w));
      })) {
    close_owned();
    return false;
  }

  // 3. AIPlayers display names.
  query("select Id, FirstName, LastName, Name from AIPlayers", {}, [&](sqlite3_stmt* st) {
    const int id = sqlite3_column_int(st, 0);
    std::string first = col_text(st, 1);
    std::string last = resolve_text(strings, col_text(st, 2), "");
    std::string full = resolve_text(strings, col_text(st, 3), "");
    if (!full.empty()) {
      out.ai_names[id] = full;
    } else if (!first.empty() && !last.empty()) {
      out.ai_names[id] = first + " " + last;
    } else if (!first.empty()) {
      out.ai_names[id] = first;
    }
  });

  // 4. Rewards_EventUnlock for Rally events.
  std::map<int64_t, int64_t> unlock_map;
  query("select r.EventId, r.UnlockEventId from Rewards_EventUnlock r join Events e on e.Id = r.EventId where "
        "e.CareerTypeId = 9",
        {}, [&](sqlite3_stmt* st) { unlock_map[sqlite3_column_int64(st, 0)] = sqlite3_column_int64(st, 1); });

  // 5. SplitPoints for ColoradoDirt tracks.
  std::map<int64_t, std::vector<double>> split_map;
  query("select sp.TrackId, sp.Split from SplitPoints sp join Tracks t on t.id = sp.TrackId where t.EnvironmentId = 111 "
        "order by sp.TrackId, sp.Split",
        {}, [&](sqlite3_stmt* st) { split_map[sqlite3_column_int64(st, 0)].push_back(sqlite3_column_double(st, 1)); });

  // 6. MultiStageSeriesEvents (7 championships in hub order).
  if (!query("select m.Id, m.EventId, e.HorizonEventID, e.Name, e.TargetClass, e.CashPrize, e.UnlockPointsReq "
             "from MultiStageSeriesEvents m join Events e on e.Id = m.EventId order by m.Id",
             {}, [&](sqlite3_stmt* st) {
               RallyChampionshipDef ch;
               ch.series_id = sqlite3_column_int(st, 0);
               ch.event_id = sqlite3_column_int64(st, 1);
               ch.horizon_id = col_text(st, 2);
               ch.name = resolve_text(strings, col_text(st, 3), default_championship_name(ch.horizon_id));
               ch.target_class = sqlite3_column_int(st, 4);
               ch.cash_prize = sqlite3_column_int64(st, 5);
               ch.unlock_points_req = sqlite3_column_int(st, 6);
               if (auto it = unlock_map.find(ch.event_id); it != unlock_map.end()) ch.unlocks_event_id = it->second;
               out.championships.push_back(std::move(ch));
             })) {
    close_owned();
    return false;
  }

  // 7. MultiStageSeriesRaces (28 stages across the 7 championships).
  for (auto& ch : out.championships) {
    if (!query("select msr.StageIndex, msr.EventId, e.HorizonEventID, e.Name, r.TrackId, t.RouteId, t.DisplayName, "
               "t.Length, t.StartLine, t.FinishLine "
               "from MultiStageSeriesRaces msr "
               "join Events e on e.Id = msr.EventId "
               "join Races r on r.EventId = msr.EventId "
               "join Tracks t on t.id = r.TrackId "
               "where msr.MultiStageSeriesEventId = ? order by msr.StageIndex",
               {std::to_string(ch.series_id)}, [&](sqlite3_stmt* st) {
                 RallyStageInfo s;
                 s.stage_index = sqlite3_column_int(st, 0);
                 s.event_id = sqlite3_column_int64(st, 1);
                 s.horizon_id = col_text(st, 2);
                 s.track_id = sqlite3_column_int64(st, 4);
                 s.route_id = sqlite3_column_int(st, 5);
                 const std::string fallback = ch.name + " - Stage " + std::to_string(s.stage_index);
                 s.name = resolve_text(strings, col_text(st, 6), resolve_text(strings, col_text(st, 3), fallback));
                 s.track.track_id = s.track_id;
                 s.track.route_id = s.route_id;
                 s.track.length = sqlite3_column_double(st, 7);
                 s.track.start_line = sqlite3_column_double(st, 8);
                 s.track.finish_line = sqlite3_column_double(st, 9);
                 if (auto it = split_map.find(s.track_id); it != split_map.end()) s.track.splits = it->second;
                 if (auto it = timings.best_splits.find(s.track_id); it != timings.best_splits.end())
                   s.track.best = it->second;
                 ch.stages.push_back(std::move(s));
               })) {
      close_owned();
      return false;
    }
  }

  // 8. RALLY_HEATS (EventId 279, route 24, WRX twin 1626).
  query("select e.Id, e.Name, e.CashPrize, e.PlayerCarId, r.TrackId, t.RouteId, t.Length, t.StartLine, t.FinishLine "
        "from Events e join Races r on r.EventId = e.Id join Tracks t on t.id = r.TrackId "
        "where e.HorizonEventID = 'RALLY_HEATS' limit 1",
        {}, [&](sqlite3_stmt* st) {
          out.heats.event_id = sqlite3_column_int64(st, 0);
          out.heats.name = resolve_text(strings, col_text(st, 1), "Horizon Rally Heats");
          out.heats.cash_prize = sqlite3_column_int64(st, 2);
          out.heats.player_car_id = sqlite3_column_int64(st, 3);
          out.heats.track.track_id = sqlite3_column_int64(st, 4);
          out.heats.route_id = out.heats.track.route_id = sqlite3_column_int(st, 5);
          out.heats.track.length = sqlite3_column_double(st, 6);
          out.heats.track.start_line = sqlite3_column_double(st, 7);
          out.heats.track.finish_line = sqlite3_column_double(st, 8);
          if (auto it = timings.best_splits.find(out.heats.track.track_id); it != timings.best_splits.end())
            out.heats.track.best = it->second;
        });

  close_owned();

  // 9. Optional world trigger positions from rally_event_activations.xml.
  if (!out.package_dir.empty()) {
    const auto act_path = out.package_dir / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "gamemodes" /
                          "ColoradoDirt" / "rally_event_activations.xml";
    std::ifstream in(act_path, std::ios::binary);
    if (in) {
      std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      audio::XmlNode root;
      if (audio::parse_xml(xml, root, nullptr)) {
        for (const auto& act : root.children) {
          if (!audio::iequals(act.name, "Activity")) continue;
          const std::string hid = act.attr("name");
          for (auto& ch : out.championships) {
            if (ch.horizon_id == hid) {
              if (const auto* tz = act.child("TriggerZone")) {
                ch.trigger_x = tz->number("position.x");
                ch.trigger_z = tz->number("position.z");
              }
            }
          }
        }
      }
    }
  }

  out.loaded = !out.championships.empty();
  return out.loaded;
}

void RallyChampionshipSession::init_drivers(const RallyCatalog& catalog, const std::string& player_name) {
  drivers_.clear();
  overall_.clear();

  RallyStandingRow player;
  player.driver_id = 0;
  player.name = player_name.empty() ? "YOU" : player_name;
  player.is_player = true;
  drivers_.push_back(std::move(player));

  for (size_t i = 0; i < catalog.field.drivers.size(); ++i) {
    const int ai_id = catalog.field.drivers[i].first;
    RallyStandingRow ai;
    ai.driver_id = ai_id;
    auto it = catalog.ai_names.find(ai_id);
    ai.name = (it != catalog.ai_names.end() && !it->second.empty()) ? it->second : ("RALLY DRIVER " + std::to_string(i + 1));
    ai.is_player = false;
    drivers_.push_back(std::move(ai));
  }
}

bool RallyChampionshipSession::start_championship(const RallyCatalog& catalog, int64_t championship_event_id,
                                                  int difficulty, uint64_t seed, const std::string& player_name) {
  const auto* ch = catalog.find_championship(championship_event_id);
  if (!ch || ch->stages.empty()) return false;
  active_ = true;
  is_heats_ = false;
  finished_ = false;
  finalized_ = false;
  in_service_area_ = false;
  event_id_ = ch->event_id;
  unlocks_event_id_ = ch->unlocks_event_id;
  title_ = ch->name;
  target_class_ = ch->target_class;
  cash_prize_ = ch->cash_prize;
  difficulty_ = std::clamp(difficulty, 0, 3);
  seed_ = seed;
  stages_ = ch->stages;
  current_stage_ = 0;
  last_finish_ = {};
  init_drivers(catalog, player_name);
  return true;
}

bool RallyChampionshipSession::start_heats(const RallyCatalog& catalog, int difficulty, uint64_t seed,
                                           const std::string& player_name) {
  active_ = true;
  is_heats_ = true;
  finished_ = false;
  finalized_ = false;
  in_service_area_ = false;
  event_id_ = catalog.heats.event_id;
  unlocks_event_id_ = 275;  // MS_RALLY_05 (Beaumont)
  title_ = catalog.heats.name;
  target_class_ = 4;
  cash_prize_ = catalog.heats.cash_prize;
  difficulty_ = std::clamp(difficulty, 0, 3);
  seed_ = seed;
  stages_.clear();
  RallyStageInfo s;
  s.event_id = catalog.heats.event_id;
  s.stage_index = 1;
  s.track_id = catalog.heats.track.track_id;
  s.route_id = catalog.heats.route_id;
  s.horizon_id = "RALLY_HEATS";
  s.name = catalog.heats.name;
  s.track = catalog.heats.track;
  stages_.push_back(std::move(s));
  current_stage_ = 0;
  last_finish_ = {};
  init_drivers(catalog, player_name);
  return true;
}

const RallyStageInfo* RallyChampionshipSession::current_stage() const {
  if (!active_ || current_stage_ < 0 || current_stage_ >= int(stages_.size())) return nullptr;
  return &stages_[size_t(current_stage_)];
}

RallyStageFinish RallyChampionshipSession::record_stage_finish(const RallyCatalog& catalog, double player_stage_time,
                                                               const std::vector<double>& ai_stage_times) {
  RallyStageFinish fin;
  if (!active_ || current_stage_ >= int(stages_.size())) return fin;
  if (finished_ || in_service_area_ || stages_completed() > current_stage_) return last_finish_;

  const auto& stg = stages_[size_t(current_stage_)];
  std::vector<double> ai_times = ai_stage_times;
  if (ai_times.empty() && !catalog.field.drivers.empty()) {
    const uint64_t stage_seed = seed_ ^ (uint64_t(current_stage_ + 1) * 0xBF58476D1CE4E5B9ull);
    ai_times = field_times(catalog.field, stg.track, difficulty_, stage_seed);
  }

  // Record this stage's time for player and each AI driver.
  drivers_[0].stage_times.push_back(player_stage_time);
  drivers_[0].last_stage_time = player_stage_time;
  drivers_[0].total_time += player_stage_time;

  for (size_t i = 1; i < drivers_.size(); ++i) {
    const double t = (i - 1 < ai_times.size()) ? ai_times[i - 1] : (player_stage_time + 1.0);
    drivers_[i].stage_times.push_back(t);
    drivers_[i].last_stage_time = t;
    drivers_[i].total_time += t;
  }

  // Sort stage standings (ties go to the player).
  std::vector<RallyStandingRow> stage_sorted = drivers_;
  std::sort(stage_sorted.begin(), stage_sorted.end(), [](const RallyStandingRow& a, const RallyStandingRow& b) {
    if (a.last_stage_time != b.last_stage_time) return a.last_stage_time < b.last_stage_time;
    return a.is_player && !b.is_player;
  });
  const double stage_lead = stage_sorted.empty() ? 0.0 : stage_sorted.front().last_stage_time;
  int player_stage_place = 1;
  for (size_t i = 0; i < stage_sorted.size(); ++i) {
    stage_sorted[i].stage_place = int(i) + 1;
    stage_sorted[i].gap_to_leader = stage_sorted[i].last_stage_time - stage_lead;
    if (stage_sorted[i].is_player) player_stage_place = int(i) + 1;
  }

  // Sort cumulative overall standings (ties go to the player).
  overall_ = drivers_;
  std::sort(overall_.begin(), overall_.end(), [](const RallyStandingRow& a, const RallyStandingRow& b) {
    if (a.total_time != b.total_time) return a.total_time < b.total_time;
    return a.is_player && !b.is_player;
  });
  const double overall_lead = overall_.empty() ? 0.0 : overall_.front().total_time;
  int player_overall = 1;
  for (size_t i = 0; i < overall_.size(); ++i) {
    overall_[i].overall_place = int(i) + 1;
    overall_[i].gap_to_leader = overall_[i].total_time - overall_lead;
    if (overall_[i].is_player) player_overall = int(i) + 1;
  }

  fin.stage_index = current_stage_;
  fin.total_stages = int(stages_.size());
  fin.player_stage_time = player_stage_time;
  fin.player_total_time = drivers_[0].total_time;
  fin.stage_place = player_stage_place;
  fin.overall_place = player_overall;
  fin.stage_gap_to_leader = player_stage_time - stage_lead;
  fin.overall_gap_to_leader = drivers_[0].total_time - overall_lead;
  fin.stage_standings = std::move(stage_sorted);
  fin.overall_standings = overall_;

  if (current_stage_ + 1 >= int(stages_.size())) {
    fin.championship_complete = true;
    finished_ = true;
    in_service_area_ = false;
  } else {
    fin.championship_complete = false;
    in_service_area_ = true;
  }

  last_finish_ = fin;
  return fin;
}

RallyCarValidation RallyChampionshipSession::validate_car(int car_class_id, bool has_rally_kit) const {
  RallyCarValidation v;
  if (!active_) {
    v.valid = false;
    v.reason = "NO ACTIVE RALLY SESSION";
    return v;
  }
  if (is_heats_) return v;  // RALLY_HEATS supplies a loaner rally car.
  if (!has_rally_kit) {
    v.valid = false;
    v.reason = "RALLY UPGRADE KIT REQUIRED";
    return v;
  }
  if (target_class_ > 0 && car_class_id > target_class_) {
    v.valid = false;
    v.reason = std::string("CAR CLASS EXCEEDS CLASS ") + rally_class_name(target_class_) + " LIMIT";
    return v;
  }
  return v;
}

bool RallyChampionshipSession::advance_from_service_area() {
  if (!active_ || !in_service_area_ || finished_) return false;
  ++current_stage_;
  in_service_area_ = false;
  return current_stage_ < int(stages_.size());
}

bool RallyChampionshipSession::advance_from_service_area(int car_class_id, bool has_rally_kit, std::string* reason) {
  if (!active_ || !in_service_area_ || finished_) {
    if (reason) *reason = "NOT IN SERVICE AREA";
    return false;
  }
  const RallyCarValidation v = validate_car(car_class_id, has_rally_kit);
  if (!v.valid) {
    if (reason) *reason = v.reason;
    return false;
  }
  if (reason) reason->clear();
  return advance_from_service_area();
}

void RallyChampionshipSession::retire() {
  active_ = false;
  in_service_area_ = false;
  finished_ = false;
}

int RallyChampionshipSession::player_overall_place() const {
  for (const auto& r : overall_)
    if (r.is_player) return r.overall_place;
  return 1;
}

double RallyChampionshipSession::player_total_time() const {
  return drivers_.empty() ? 0.0 : drivers_[0].total_time;
}

RallyChampionshipReward RallyChampionshipSession::finalize(const RallyCatalog& catalog, drive::DriveSave& save,
                                                           int diff_pct, int assists_pct) {
  RallyChampionshipReward rew;
  if (!active_ || !finished_ || finalized_) return rew;
  finalized_ = true;

  rew.is_heats = is_heats_;
  rew.event_id = event_id_;
  rew.final_place = player_overall_place();
  rew.total_time = player_total_time();

  save.has_rally = true;
  save.num_rallies += 1;
  save.num_rally_stages += int(stages_.size());

  // Look up previous best championship finish BEFORE upserting, so replay XP is capped to the delta.
  int prev_best_place = 0;
  if (!is_heats_) {
    for (const auto& [eid, best] : save.event_results) {
      if (eid == event_id_) {
        prev_best_place = best;
        break;
      }
    }
  }

  // Record each stage result in Career_EventResults (and championship result).
  if (!is_heats_) {
    for (size_t s = 0; s < stages_.size() && s < drivers_[0].stage_times.size(); ++s) {
      int st_place = 1;
      const double pt = drivers_[0].stage_times[s];
      for (size_t d = 1; d < drivers_.size(); ++d)
        if (s < drivers_[d].stage_times.size() && drivers_[d].stage_times[s] < pt) ++st_place;
      upsert_best_result(save.event_results, stages_[s].event_id, st_place);
    }
  }
  upsert_best_result(save.event_results, event_id_, rew.final_place);

  // Credits payout (SPEC-V4-CONTENT: CashPrize shared over N=50; stages pay 0).
  rew.base_credits = race_credits(double(cash_prize_), Winnings::kDefault, 50, rew.final_place, true);
  rew.total_credits = RallyCatalog::championship_credits(cash_prize_, rew.final_place, diff_pct, assists_pct);
  rew.bonus_credits = rew.total_credits - rew.base_credits;
  save.credits += rew.total_credits;
  save.total_winnings += rew.total_credits;
  save.rally_winnings += rew.total_credits;

  // Rally XP & Wristband level (championships only; capped to improvement delta above previous best place).
  rew.old_rally_xp = save.rally_xp;
  rew.old_rally_level = save.rally_level;
  if (!is_heats_) {
    const int prev_xp = prev_best_place > 0 ? catalog.points_for_place(prev_best_place) : 0;
    const int new_xp = catalog.points_for_place(rew.final_place);
    rew.rally_xp_earned = std::max(0, new_xp - prev_xp);
    save.rally_xp += rew.rally_xp_earned;
    if (rew.final_place == 1) save.num_rally_victories += 1;
  }
  rew.new_rally_xp = save.rally_xp;
  save.rally_level = std::max(save.rally_level, catalog.wristband_for_xp(save.rally_xp));
  rew.new_rally_level = save.rally_level;
  rew.wristband_up = rew.new_rally_level > rew.old_rally_level;

  // Unlock chain:
  // - RALLY_HEATS unlocks all 28 stages + MS_RALLY_05 (275) + RALLY_HEATS (279).
  // - Championship unlocks its Rewards_EventUnlock target on a podium finish (1st-3rd).
  if (is_heats_) {
    if (add_unique_unlock(save.event_unlocks, event_id_)) rew.unlocked_events.push_back(event_id_);
    if (add_unique_unlock(save.event_unlocks, 275)) rew.unlocked_events.push_back(275);
    for (const auto& ch : catalog.championships) {
      for (const auto& st : ch.stages) {
        if (add_unique_unlock(save.event_unlocks, st.event_id)) rew.unlocked_events.push_back(st.event_id);
      }
    }
  } else if (unlocks_event_id_ > 0) {
    if (rew.final_place >= 1 && rew.final_place <= 3) {
      if (add_unique_unlock(save.event_unlocks, unlocks_event_id_)) rew.unlocked_events.push_back(unlocks_event_id_);
    }
  } else if (rew.final_place >= 1 && rew.final_place <= 3) {
    // Completed the final championship (MS_RALLY_02, Clear Springs) on the podium.
    rew.champion_unlocked = true;
  }

  return rew;
}

}  // namespace race
