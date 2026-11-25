// SPEC-RACE-AI, Difficulty and rubber-banding: each opponent's skill,
// temperament and rubber-band row, its rolled braking and cornering, the
// torque cut while it leads the player, catch-up while it trails, and the
// race-start torque boost by grid place.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "race/event.h"

namespace race {

struct DriverSetup {
  int skill = 50, temperament = 5, rubberband_id = 0;
};
// Steps 1-3 of the spec: the event's values plus the driver's modifiers,
// skill clamped to 1-80 and temperament to 1-10; the rubber-band id uses
// event_rubberband whenever >= 1 (adding rubberband_modifier only for 1..10,
// preserving Street Race / Star Showdown ids > 10 verbatim), and falls back to
// the skill row's Rubberbands_id only when event_rubberband < 1.
DriverSetup assign_driver(int event_skill, int event_temperament, int event_rubberband, int skill_modifier,
                          int aggro_modifier, int rubberband_modifier, const EventData& rows);
// Assigns driver parameters with Pinyon Shift's HARD mode (+1 tier, max
// aggression temperament 1, skill 1 on Pro/HardPlus, rubber-banding disabled).
DriverSetup assign_driver_for_mode(int event_skill, int event_temperament, int event_rubberband, int skill_modifier,
                                   int aggro_modifier, int rubberband_modifier, Difficulty base_difficulty,
                                   AiMode ai_mode, const EventData& rows);

// The race-start and nemesis boosts' tunables: GlobalRegistry.xml
// `AI/Racing/RaceBehavior/RaceStartBoost/*` and `.../NemesisBoost/*`. The
// title registers each as a registry value whose compiled-in default is the
// old "code-only" figure (0.4, 0.1, 300, 700, 1.5, +0.3, 50/100/500 m) and
// loads GlobalRegistry.xml over them at boot, so the disc's values below
// are the ones that run (WORLD-05, C4). `from` reads them from the parsed
// registry (id -> value); a missing id keeps the initialiser.
struct RaceBoostTunables {
  double start_torque_first = 0.6;       // RaceStartBoost/TorqueScaleBoostForFirstCar
  double start_torque_last = 0.2;        // .../TorqueScaleBoostForLastCar
  double keep_full_first = 500;          // .../KeepFullBoostDistanceFirst (m)
  double keep_full_last = 250;           // .../KeepFullBoostDistanceLast (m)
  double decay = 1000;                   // .../DecayBoostOverThisManyMeters (m)
  double max_skill = 2.0;                // .../MaxSkillBoostFactor
  double gap_per_car = 20;               // .../GapPerCar (m)
  double min_speed_behind_player = 25;   // .../MinSpeedBoostBehindPlayer (m/s)
  double torque_behind_player = 0;       // .../TorqueScaleBoostBehindPlayer
  double torque_minimum = 0;             // .../TorqueScaleBoostMinimum
  double nemesis_skill = 1.1;            // NemesisBoost/SkillBoostFactor
  double nemesis_torque = 0.4;           // NemesisBoost/TorqueBoost
  double catchup_torque = 0.6;           // NemesisBoost/PlayerCatchupTorqueBoost
  double catchup_min = 0;                // NemesisBoost/PlayerCatchupMinDist (m)
  double catchup_max = 150;              // NemesisBoost/PlayerCatchupMaxDist (m)
  double catchup_end = 300;              // NemesisBoost/PlayerCatchupEndDist (m)
  static RaceBoostTunables from(const std::map<std::string, double>& registry);
};

// One race AI's boost this step (SPEC-RACE-AI, Difficulty and
// rubber-banding; decoded from the title's boost update). Inputs:
// - progress: the car's distance raced (m), player_progress the player's;
// - speed: the car's speed (m/s);
// - others: the other race-AI cars in the race, others_ahead those of them
//   further along than this car;
// - to_finish: m left to the finish;
// - nemesis: this car is a nemesis rival.
// The start boost runs while progress < KeepFirst + Decay. Its full-boost
// distance and torque are lerped from the first car's to the last's by
// others_ahead / others. A car more than GapPerCar x (others -
// others_ahead) m ahead of the player gets TorqueScaleBoostMinimum; one
// behind the player above MinSpeedBoostBehindPlayer gets
// TorqueScaleBoostBehindPlayer. The torque is held to the full-boost
// distance, then decays to 0 over Decay m, while the skill factor runs from
// MaxSkillBoostFactor down to 1 over the same span. With no start torque,
// a nemesis rival with another race AI ahead gets SkillBoostFactor and
// +TorqueBoost. A rival with no other race AI (one on one) more than
// PlayerCatchupEndDist from the finish and more than MinDist behind the
// player gets PlayerCatchupTorqueBoost ramped from MinDist to MaxDist
// (`one_on_one_catchup`: the title gates this on a game-state flag whose
// meaning is not traced; interim on, SPEC_QUESTIONS Q121).
// The torque factor is x (1 + torque); braking and cornering become
// drive::ai_boost(s, skill, cap) with the caps 1.0 and 1.25.
struct RaceBoost {
  double torque = 0.0;  // added to 1 in the torque factor
  double skill = 1.0;   // the skill multiplier m of drive::ai_boost
  const char* kind = "";  // "start_full", "start_decay", "nemesis" or ""
  bool active() const { return *kind != '\0'; }
};
RaceBoost race_boost(const RaceBoostTunables& t, double progress, double player_progress, double speed, int others,
                     int others_ahead, double to_finish, bool nemesis, bool one_on_one_catchup = true);

// Dynamic difficulty (GameTunableSettings.ini [DynamicDifficulty] +
// AISkillAdjustments): adjusts profile Main/AISkill (1..20, lower = harder)
// after a finished race in STOCK AI mode. Disabled when ai_mode != kStock.
struct DynamicDifficultyConfig {
  int skill_to_temperament_ratio = 4;
  int easy_upper_bound = 20;
  int medium_upper_bound = 16;
  int hard_upper_bound = 4;
  int pro_upper_bound = 1;
  int max_num_races = 3;
};
int lookup_skill_adjustment(int time_ratio_pct, const std::vector<std::pair<int, int>>& table);
int update_dynamic_ai_skill(int current_skill, double player_time, double ref_ai_time, int num_races_completed,
                            AiMode mode, const std::vector<std::pair<int, int>>& table,
                            const DynamicDifficultyConfig& cfg = {});
Difficulty difficulty_from_dynamic_skill(int ai_skill, const DynamicDifficultyConfig& cfg = {});

// The torque factor in torque-cut mode: 1 - r x TorqueCutFactor, r linear
// from 0 at StartTorqueCutDistance to 1 at MaxTorqueCutDistance (clamped).
double torque_cut(const RubberbandRow& row, double gap);
// The AI's acceleration limits from its rolled values (the spec's fits):
// corner a_c = cornering x 1.7 g, braking = braking x 2.0 g.
constexpr double kGravity = 9.81;
inline double corner_accel(double cornering) { return cornering * 1.7 * kGravity; }
inline double brake_decel(double braking) { return braking * 2.0 * kGravity; }

// One opponent's rubber-band state, updated every step.
class DriverAi {
 public:
  enum class State { kNormal, kMaxPerformance, kRubberband, kCatchUp };
  // `row` null: the built-in values (rubber band from 100 m with the
  // built-in mode choices, no torque cut, no catch-up). `enabled` false: the "no rubber-banding" option.
  void init(const SkillRow& skill, const RubberbandRow* row, uint64_t seed, bool enabled = true);
  // gap: metres along the route, positive while this car is ahead of the
  // player; places_behind: this car's place minus the player's.
  // dt: the time since the last update (the over- modes' hold).
  void update(double gap, int places_behind, double dt = 1.0 / 120.0);

  double braking() const { return braking_; }
  double cornering() const { return cornering_; }
  double torque() const { return torque_; }  // engine torque factor (cut x nominal)
  State state() const { return state_; }
  int mode() const { return state_ == State::kRubberband && torque_ < skill_.nominal_torque_scale ? 8 : mode_; }
  double amount() const { return amount_; }
  int rolls() const { return rolls_; }

 private:
  double uniform();
  void roll();
  void draw_mode();
  SkillRow skill_;
  RubberbandRow row_;
  bool has_row_ = false, enabled_ = true;
  uint64_t rng_ = 0;
  double base_braking_ = 1, base_cornering_ = 1;
  double braking_ = 1, cornering_ = 1, torque_ = 1;
  State state_ = State::kNormal;
  int mode_ = 0, rolls_ = 0;
  bool catch_up_ = false, max_ = false;
  double amount_ = 0, mistake_left_ = 0;
  double last_gap_ = 0;
  int last_places_ = 0;
  bool have_last_ = false;
};

}  // namespace race
