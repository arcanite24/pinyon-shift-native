// An event's race from the game database (SPEC-EVENTS, Data chain):
// Events (keyed here by HorizonEventID, e.g. "FR22") -> Races (TrackId,
// NumLaps) -> Tracks (RouteId) and the opponents (EventParticipants ->
// AIPlayers; Ordinal is the car's Data_Car.Id), with the AI rows
// SPEC-RACE-AI reads (AISkills, AIRubberbands, AIRubberbandModeChoices,
// AITemperaments) and EventScoring.
#pragma once

#include <cstdint>
#include <filesystem>
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
  int mode = 0;  // 0 none, 1/2 under-braking/cornering, 3/4 over-, 5-7 lags (spec order)
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
};

struct EventData {
  std::string horizon_id;
  int64_t event_id = 0, track_id = 0;
  int route_id = 0, laps = 1;
  double track_length = 0;  // Tracks.Length (not the race distance)
  int player_grid_position = 0;
  int event_skill = 50, event_temperament = 5, event_rubberband = 0;
  double cash_prize = 0;
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
bool load_event(const std::filesystem::path& game_root, const std::string& horizon_id, Difficulty difficulty,
                EventData& out, std::string* error);

// The census (SPEC-EVENTS, Tests): row counts and how many races resolve
// to a route file and an AI line name.
struct EventCensus {
  int events = 0, races = 0, tracks = 0, resolved = 0;
};
bool event_census(const std::filesystem::path& game_root, EventCensus& out, std::string* error);

}  // namespace race
