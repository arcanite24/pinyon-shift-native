// The gameplay side of the radio DJ (SPEC-RADIO, "DJ lines"): which
// RadioSystem.xml gameplayEvent fires when, as the game's own data says.
// - An activity's <RadioDJRequests> in media/gamemodes.zip (barn finds,
//   street race hubs): <Special|Immediate gameplayEvent timeDelayMin
//   timeDelayMax misc enabled topPriority>.
// - radiodj.xml's CRadioDJFestivalWristband behaviours: the state
//   festival_wristband (entered when Race Central awards a wristband,
//   race_central.xml award_wristband) and special_wristband (entered in free
//   roam when a wristband is ready to collect, game_free_roam_flow.xml).
//   "level" is the zero-based wristband (the file's own note); a behaviour
//   without one names the event family, which takes the wristband's 1-based
//   number (FestivalUpdateWristband1-7, CongratsWinnersOfWristband1-7);
//   level -1 cancels the previous tier's queued updates.
// - Fame.xml's radioFestivalUpdate (freeroam::SkillEvent).
// The lookup in the radio is exact (any case), so every name the engine
// sends must be one of these; radio_has_event() checks one against the
// parsed RadioSystem.xml.
// Ours: when a disabled request (BarnFindSpotted<n>) is enabled is not in the
// data; the engine schedules it inside its time window after the rumour.
#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "audio/radio.h"
#include "audio/xml.h"

namespace audio {

// The immediate line the opening Viper drive starts the radio with.
inline constexpr const char* kViperOpeningRadioEvent = "AliceOpeningInterview1";

// The map a trigger reads: a station's DJSpecialDialogue or
// DJImmediateDialogue, or the shared FestivalUpdates list.
enum class RadioChannel { kSpecial, kImmediate, kFestivalUpdate };

struct RadioDjRequest {
  RadioChannel channel = RadioChannel::kSpecial;
  std::string event;
  double delay_min_s = 0, delay_max_s = 0;
  int misc = -1;
  bool enabled = true, top_priority = false;
};

// The <Special>/<Immediate> children of one <RadioDJRequests> element.
std::vector<RadioDjRequest> parse_radio_dj_requests(const XmlNode& requests);

struct RadioDjWristbandRule {
  static constexpr int kAnyLevel = -2;  // no level: the event takes the wristband's number
  std::string event;
  RadioChannel channel = RadioChannel::kFestivalUpdate;  // isFestivalUpdate="false": special
  int level = kAnyLevel;
  int after_events = 0;  // playAfterEventsNum (0: on the award itself)
  int min_position = 0;  // playAfterEventsMinPos
  bool cancels() const { return level == -1; }
};

struct RadioDjRules {
  std::vector<RadioDjWristbandRule> festival_wristband;  // on the award
  std::vector<RadioDjWristbandRule> special_wristband;   // when one is ready to collect
};

bool parse_radio_dj_rules(std::string_view radiodj_xml, RadioDjRules& out, std::string* error = nullptr);
// media/gamemodes.zip radiodj.xml under `game_root`.
bool load_radio_dj_rules(const std::filesystem::path& game_root, RadioDjRules& out, std::string* error = nullptr);

struct RadioDjCue {
  RadioChannel channel = RadioChannel::kFestivalUpdate;
  std::string event;
  bool operator==(const RadioDjCue&) const = default;
};

// The event a rule names for wristband `level` (0 based); empty when the
// rule is for another wristband or only cancels.
std::string wristband_rule_event(const RadioDjWristbandRule& rule, int level);

// One career's wristband DJ lines (each fires once).
class RadioDjWristbands {
 public:
  explicit RadioDjWristbands(const RadioDjRules* rules = nullptr) : rules_(rules) {}
  void set_rules(const RadioDjRules* rules) { rules_ = rules; }
  // Race Central awarded wristband `level`. `cancel_pending` is set when
  // the rules cancel the previous tier's queued updates first.
  std::vector<RadioDjCue> on_awarded(int level, bool* cancel_pending = nullptr);
  // Wristband `level` became ready to collect.
  std::vector<RadioDjCue> on_ready(int level);
  // A festival event of wristband tier `event_level` finished at `position`
  // (1 = first).
  std::vector<RadioDjCue> on_event_finished(int event_level, int position);

 private:
  const RadioDjRules* rules_ = nullptr;
  std::set<int> awarded_, ready_;
  std::vector<int> counts_;  // per festival_wristband rule
  std::set<size_t> fired_;
};

// Every cue the rules can emit for wristbands 0..levels-1.
std::vector<RadioDjCue> all_wristband_cues(const RadioDjRules& rules, int levels);

// Whether `event` is in the map `channel` reads (special and immediate: any
// station's).
bool radio_has_event(const RadioData& data, RadioChannel channel, std::string_view event);

// Flow/state names that callers (such as first_time_career) may pass instead of
// a literal RadioSystem.xml gameplayEvent name.
enum class RadioDjState {
  kNone,
  kFestivalWristband,  // radiodj.festival_wristband (award wristband updates)
  kSpecialWristband,   // radiodj.special_wristband (wristband ready to collect)
  kPostRace,           // radiodj.post_race (empty state on disc)
  kCycleRadioLogos,    // first_time_career CycleRadioLogos (HUD-only CHudRadioCycleLogos)
};

// Recognises a flow/state name (case-insensitive, with optional "radiodj." prefix).
RadioDjState parse_radio_dj_state(std::string_view name);

// Resolves which RadioSystem.xml channel carries `event`, preferring `preferred`
// before checking the remaining channels (immediate, festival update, special).
bool resolve_radio_event_channel(const RadioData& data, RadioChannel preferred, std::string_view event,
                                 RadioChannel& out_channel);

}  // namespace audio
