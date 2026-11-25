// Horizon Rally Expansion UI & HUD (apps/carview/rally_hub.*):
// - Live Co-Driver Pace-Note directional icon strip & Rally Stage Split Timer
// - Horizon Rally Hub on H_RALLY_HUB over BgLoop.wmv (game_rally_free_roam_flow's
//   rally_hub: the unlocked championships' tickets), with the engine's EXTRAS
//   (heats replay, rally kit, ColoradoDirt free roam, return to Colorado)
// - Between-Stage Service Area (4-stage cumulative standings, car repair,
//   tuning/upgrades, continue/retire)
// - Stage & Championship Final Results screen
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "garage/rally_kit.h"
#include "race/rally_championship.h"
#include "race/rally_codriver.h"
#include "race/rivals.h"
#include "scene_screen.h"
#include "ui.h"

namespace carview {

// Live in-stage Rally HUD: Co-Driver Pace-Note icons + 4-split timer & delta.
struct RallyStageHud {
  bool active = false;
  std::string championship_name;
  std::string stage_name;
  int stage_number = 1;       // 1..4
  int total_stages = 4;
  double stage_clock = 0.0;
  double progress = 0.0;      // 0..1
  int total_splits = 4;
  int splits_passed = 0;
  double last_split_delta = 0.0;  // s vs best split (<0 green faster, >0 red slower)
  double last_split_age = 99.0;   // s since last split crossed
  int overall_place = 0;          // prior cumulative place (0 on stage 1)
  race::PaceNoteHudState pace;
};

// Horizon Rally Hub screen model.
struct RallyHubItem {
  enum Kind { kHeats, kChampionship, kRivals, kFitRallyKit, kFreeRoam, kReturnColorado };
  Kind kind = kChampionship;
  int64_t event_id = 0;
  std::string title;
  std::string class_badge;   // "B", "A", "S", "R3", "LOAN", "RIVAL", "KIT"
  std::string status_badge;  // "NEW", "1ST", "LOCKED", "READY"
  bool unlocked = true;
  bool eligible = true;
  std::string lock_reason;
  int64_t cash_prize = 0;
  int stages = 4;
  std::vector<std::string> stage_names;
  // Rivals / Time Trials metadata (populated for `kind == kRivals`).
  int route_id = 0;
  double pb_time_s = 0.0;
  bool pb_clean = false;
  int rival_tier_index = 1;  // 0 = PB, 1 = Bronze, 2 = Silver, 3 = Gold
  std::string rival_name;
  double target_time_s = 0.0;
  int64_t bounty_cr = 0;
  std::array<race::RivalTarget, 3> rival_tiers{};
  // The ticket (championships): Events.EventTicket, the restriction text, and
  // the best finish (0 none).
  std::string ticket;
  std::string restriction;
  int best_place = 0;
};

struct RallyHubState {
  bool active = false;
  int selected = 0;
  int rally_level = 0;
  int rally_xp = 0;
  int next_level_xp = 310;
  std::string wristband_name = "Horizon Rally Yellow";
  int num_rallies = 0;
  int num_stages = 0;
  int num_victories = 0;
  int64_t rally_winnings = 0;
  int64_t credits = 0;
  std::string car_name;
  std::string car_class;
  int car_pi = 0;
  garage::RallyKitStatus kit_status{};
  int64_t rally_kit_cost = 0;
  // items[0 .. tickets) are the unlocked championships (the hub's tickets, in
  // MultiStageSeries order); the rest are the engine's EXTRAS.
  std::vector<RallyHubItem> items;
  int tickets = 0;
  std::string notice;
  std::string band_texture;  // WRISTBANDS/<thumbnail> of the rally wristband
  std::string band_next;     // the next rally wristband's name (" at the top)
  bool track_layout = false; // Y: the focused ticket's reverse (TRACK LAYOUT)
  bool help_hidden = false;  // first_time_hub_flow's CDisableRallyRaceSelectHelpBar
  bool extras_open = false;  // the engine's EXTRAS list over the hub
  int extras_selected = 0;   // index into items[tickets ..]
};

// Between-Stage Service Area screen model.
struct RallyServiceAreaState {
  bool active = false;
  int selected = 0;  // 0 Continue, 1 Repair Damage, 2 Tuning & Upgrades, 3 Retire
  std::string championship_name;
  std::string completed_stage_name;
  std::string next_stage_name;
  int completed_stage = 1;  // 1..3
  int total_stages = 4;
  double last_stage_time = 0.0;
  int stage_place = 1;
  double total_time = 0.0;
  int overall_place = 1;
  double gap_to_leader = 0.0;
  double damage_pct = 0.0;  // 0..100
  bool repaired = false;
  std::string car_name;
  std::string car_class;
  int car_pi = 0;
  std::vector<race::RallyStandingRow> standings;
  std::string notice;
};

// Final Championship / Heats Results screen model.
struct RallyChampResultsState {
  bool active = false;
  bool is_heats = false;
  std::string title;
  int final_place = 1;
  int field_size = 50;
  double total_time = 0.0;
  double gap_to_leader = 0.0;
  int rally_xp_earned = 0;
  int old_rally_xp = 0;
  int new_rally_xp = 0;
  int next_level_xp = 310;
  std::string wristband_name;
  bool wristband_up = false;
  bool champion_unlocked = false;
  int64_t base_credits = 0;
  int64_t bonus_credits = 0;
  int64_t total_credits = 0;
  int64_t new_balance = 0;
  std::string unlocked_text;
  std::vector<race::RallyStandingRow> standings;
};

// Draws live Co-Driver pace-note icons + split timer strip during a Rally stage.
void build_rally_pace_notes_hud(ui::List& out, const RallyStageHud& st, float w, float h);

struct DriveHud;
// Draws the Horizon Rally hub on H_RALLY_HUB (the player bar from `hud`; the
// BgLoop.wmv backdrop is the caller's). False without the scene.
bool build_rally_hub_screen(ui::List& out, const DriveHud* hud, const RallyHubState& st, float w, float h);
// The hub's scene nodes and strings (checked by the corpus tests).
const std::vector<const char*>& rally_hub_nodes();
std::vector<StringRef> rally_hub_text_refs();
// The ticket index of championship `event_id` on the hub (0 when it is not
// one of the unlocked tickets).
int rally_hub_ticket_index(const race::RallyCatalog& catalog, const drive::DriveSave& save, int64_t event_id);
// The engine's EXTRAS row label of an item (heats, kit, free roam, return).
std::string rally_hub_extra_label(const RallyHubItem& it);

// Draws the between-stage Service Area & 4-stage cumulative standings screen.
void build_rally_service_area_screen(ui::List& out, const RallyServiceAreaState& st, float w, float h);

// Draws the final Rally Championship / Heats results & Rally XP/credits screen.
void build_rally_championship_results_screen(ui::List& out, const RallyChampResultsState& st, float w, float h);
// rally.str's results strings those screens use (checked by the corpus test).
std::vector<StringRef> rally_results_text_refs();

// Formats a rally time as MM:SS.mmm or +SS.mm.
std::string rally_time_str(double seconds);
std::string rally_gap_str(double seconds);

// Populates UI state structs cleanly from engine models (keeps showroom_main modular).
RallyHubState make_rally_hub_state(const race::RallyCatalog& catalog, const drive::DriveSave& save,
                                   const garage::Garage* garage, const cars::PartsDb* db,
                                   const std::vector<garage::ClassBand>& bands, const std::string& car_media,
                                   int car_class_id, int car_pi, int discount_pct, int selected,
                                   const std::string& notice, const std::filesystem::path& ghosts_dir = {});

RallyServiceAreaState make_rally_service_state(const race::RallyChampionshipSession& session,
                                               const std::string& car_name, const std::string& car_class, int car_pi,
                                               double damage_pct, bool repaired, int selected,
                                               const std::string& notice);

RallyChampResultsState make_rally_results_state(const race::RallyCatalog& catalog,
                                                const race::RallyChampionshipSession& session,
                                                const race::RallyChampionshipReward& reward, int64_t new_balance);

}  // namespace carview
