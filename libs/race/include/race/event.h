// An event's race from the game database (SPEC-EVENTS, Data chain):
// Events (keyed here by HorizonEventID, e.g. "FR22") -> Races (TrackId,
// NumLaps) -> Tracks (RouteId) and the opponents (EventParticipants ->
// AIPlayers; Ordinal is the car's Data_Car.Id), with the AI rows
// SPEC-RACE-AI reads (AISkills, AIRubberbands, AIRubberbandModeChoices,
// AITemperaments) and EventScoring.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace race {

enum class Difficulty { kEasy, kMed, kHard, kPro };
bool parse_difficulty(const std::string& s, Difficulty& out);

struct SkillRow {
  int id = 0;
  double braking_min = 1, braking_max = 1, cornering_min = 1, cornering_max = 1;
  double dist_behind_for_max = 1e30;  // m
  double nominal_torque_scale = 1;
  double far_behind_torque_boost = 1;
  int rubberband_id = 0;
};

struct RubberbandModeChoice {
  int mode = 0;  // AIRubberbandModes id; what each id does is not in RACE_AI.md (no effect yet)
  double frequency = 0, amount = 0;
};

struct RubberbandRow {
  int id = 0;  // 0: the built-in values (no row)
  double start_rubberbanding = 100, max_effect = 1e30;
  double start_torque_cut = 1e30, max_torque_cut = 1e30, torque_cut_factor = 0;
  double start_catch_up = 1e30, stop_catch_up = 1e30, catch_up_factor = 0, catch_up_gap_per_position = 0;
  std::vector<RubberbandModeChoice> modes;
};

struct TemperamentRow {
  int id = 0;
  double aggression = 0, trailing_distance = 7, car_clearance = 0.3, edge_clearance = 0.4;
};

struct Opponent {
  int64_t participant_id = 0;  // EventParticipants.ID (slot order, interim)
  int64_t car_id = 0;          // Ordinal = Data_Car.Id
  std::string car_media;       // Data_Car.MediaName
  int64_t ai_player_id = 0;
  std::string driver;          // AIPlayers.Name (a string id)
  int skill_modifier = 0, aggro_modifier = 0, rubberband_modifier = 0;
  double performance_index = 0;  // Data_Car.PerformanceIndex (raw)
};

struct EventData {
  std::string horizon_id;
  int64_t event_id = 0, track_id = 0;
  int route_id = 0, laps = 1;
  double track_length = 0;  // Tracks.Length (not the race distance)
  int player_grid_position = 0;
  int event_skill = 50, event_temperament = 5, event_rubberband = 0;
  double cash_prize = 0;
  // SPEC-EVENT-REWARDS: Races.Id, Events.Level, CareerEventStyle and
  // CareerTypeId, and the event's Rewards_EventPrizes car (0: none).
  int64_t race_id = 0;
  int level = 0, style = 0, career_type = 0;
  int64_t prize_car = 0;
  int target_class = 0;     // Events.TargetClass (CarClasses.Id)
  bool upgrade_ai = false;  // Events.UpgradeAI: the AI cars are auto-upgraded (SPEC-RACE-AI, open)
  std::vector<Opponent> opponents;  // by ascending ID
  std::vector<SkillRow> skills;     // every AISkills row, by id
  std::vector<RubberbandRow> rubberbands;
  std::vector<TemperamentRow> temperaments;
  std::vector<double> scoring;  // EventScoring credits by place (1-based at [0])

  const SkillRow* skill(int id) const;
  const RubberbandRow* rubberband(int id) const;
  const TemperamentRow* temperament(int id) const;
};

// The database is <game_root>/media/db/gamedb.slt.
// Data_Car.PerformanceIndex (raw) of the car with `media` (case-insensitive);
// -1 when the disc has no such car.
double stock_performance_index(const std::filesystem::path& game_root, const std::string& media);

bool load_event(const std::filesystem::path& game_root, const std::string& horizon_id, Difficulty difficulty,
                EventData& out, std::string* error);

// The census (SPEC-EVENTS, Tests): row counts and how many races resolve
// to a route file and an AI line name.
struct EventCensus {
  int events = 0, races = 0, tracks = 0, resolved = 0;
};
bool event_census(const std::filesystem::path& game_root, EventCensus& out, std::string* error);

// One line a race for the all-events census (SPEC-EVENTS, Race type):
// Events.CareerTypeId and Tracks.RibbonConfiguration name the race type.
struct EventSummary {
  std::string horizon_id;
  int career_type = 0, ribbon = 0, route_id = 0, laps = 1, opponents = 0;
};
bool list_events(const std::filesystem::path& game_root, std::vector<EventSummary>& out, std::string* error);

// Car names for the HUD and menus. Interim until the string tables are
// read (Data_Car.DisplayName is a string-table key): the make from
// List_CarMake.IconPathBase, the model from the media name's middle part,
// both split at their case and digit changes, and the year from Data_Car.
class CarNames {
 public:
  // The names from the string tables (FORMAT-STRINGTABLES: Data_Car
  // DisplayName (ModelShort without one) and Year, List_CarMake DisplayName,
  // English), merged from
  // the disc, v4 and the packages under `state_root` (may be empty). A car
  // whose reference does not resolve keeps the words of its media name.
  // Cached per root pair.
  bool load(const std::filesystem::path& game_root, const std::filesystem::path& state_root, std::string* error);
  size_t resolved() const { return resolved_; }
  // "Ferrari California"; the media name itself when unknown.
  std::string short_name(const std::string& media) const;
  // "2009 Ferrari California".
  std::string full_name(const std::string& media) const;
  // The words of a media name's model part ("CamaroZL1" -> "Camaro ZL1").
  static std::string split_words(const std::string& s);

 private:
  struct Row {
    std::string make, model;
    int year = 0;
  };
  std::map<std::string, Row> rows_;
  size_t resolved_ = 0;
};

}  // namespace race
