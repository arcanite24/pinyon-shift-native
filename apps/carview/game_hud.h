// carview --drive: the game's HUD, title screen, and menus on the UI layer (ui.h).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace carview {

// ---- Pure HUD / Race / Rewind / Map Logic Helpers (standalone-testable) ----

// R3-UI-03: Tri-state ABS / TCS / STM assist badge state & ~7 Hz amber intervention latch.
enum class AssistBadgeState : uint8_t { kDisabled = 0, kEnabledIdle = 1, kIntervening = 2 };

inline AssistBadgeState compute_assist_badge_state(bool enabled, bool active, double flash_timer) {
  if (flash_timer > 0.0 || active) return AssistBadgeState::kIntervening;
  return enabled ? AssistBadgeState::kEnabledIdle : AssistBadgeState::kDisabled;
}

inline bool assist_blink_on(double time_s) {
  return std::sin(time_s * 6.28318530717958647692 * 7.0) > 0.0;
}

struct AssistFlashLatch {
  static constexpr double kLatchDurationSec = 0.25;
  double abs_timer = 0.0;
  double tcs_timer = 0.0;
  double stm_timer = 0.0;
  double last_time = -1.0;

  void reset() {
    abs_timer = tcs_timer = stm_timer = 0.0;
    last_time = -1.0;
  }

  void update(double time_s, bool abs_active, bool tcs_active, bool stm_active) {
    if (last_time < 0.0 || time_s < last_time - 0.5) {
      abs_timer = abs_active ? kLatchDurationSec : 0.0;
      tcs_timer = tcs_active ? kLatchDurationSec : 0.0;
      stm_timer = stm_active ? kLatchDurationSec : 0.0;
      last_time = time_s;
      return;
    }
    const double dt = std::clamp(time_s - last_time, 0.0, 0.25);
    last_time = time_s;
    abs_timer = abs_active ? kLatchDurationSec : std::max(0.0, abs_timer - dt);
    tcs_timer = tcs_active ? kLatchDurationSec : std::max(0.0, tcs_timer - dt);
    stm_timer = stm_active ? kLatchDurationSec : std::max(0.0, stm_timer - dt);
  }
};

// R3-UI-04: Missed Checkpoint detection & automatic return-to-track countdown tracker.
struct MissedCheckpointTracker {
  static constexpr double kCountdownSec = 4.0;
  static constexpr double kBypassProgressMarginM = 16.0;
  static constexpr double kBypassPlaneMarginM = 10.0;
  static constexpr double kOffCorridorTriggerM = 28.0;

  bool active = false;
  double countdown_s = 0.0;
  size_t missed_gate_index = 0;

  void reset() {
    active = false;
    countdown_s = 0.0;
    missed_gate_index = 0;
  }

  // Advances missed-checkpoint state for one step.
  // Returns true on the step when the countdown reaches 0 and an auto-reset should trigger.
  bool step(double dt, bool racing, bool gate_passed, size_t next_gate_idx, double player_progress,
            double gate_progress, double gate_plane_side, double dist_to_gate_m, double gate_reach_m,
            double off_route_m) {
    if (!racing || gate_passed) {
      reset();
      return false;
    }
    if (active && next_gate_idx != missed_gate_index) {
      reset();
      return false;
    }
    const double eff_reach = (gate_reach_m > 0.0 && gate_reach_m < 1e6) ? gate_reach_m : 18.0;
    const bool bypassed_by_progress = (player_progress > gate_progress + kBypassProgressMarginM);
    const bool bypassed_gate_plane =
        (gate_plane_side > kBypassPlaneMarginM) && (dist_to_gate_m > eff_reach) && (dist_to_gate_m < 400.0);
    const bool off_corridor_past_gate =
        (off_route_m > kOffCorridorTriggerM) && (player_progress > gate_progress - 5.0 || gate_plane_side > 0.0);
    if (!active) {
      if (bypassed_by_progress || bypassed_gate_plane || off_corridor_past_gate) {
        active = true;
        countdown_s = kCountdownSec;
        missed_gate_index = next_gate_idx;
      }
      return false;
    }
    if (!bypassed_by_progress && gate_plane_side < -2.0 && off_route_m < 10.0) {
      reset();
      return false;
    }
    countdown_s = std::max(0.0, countdown_s - std::max(0.0, dt));
    if (countdown_s <= 0.0) {
      reset();
      return true;
    }
    return false;
  }
};

// R3-UI-05: Live Rewind scrubber bar telemetry.
struct RewindScrubberHud {
  float buffer_fraction = 1.0f;    // [0..1] filled capacity of rolling rewind buffer
  float playhead_fraction = 1.0f;  // [0..1] scrub position within populated buffer (0 = oldest, 1 = latest)
  int current_slot = 0;
  int total_slots = 0;
  double seconds_rewound = 0.0;    // >= 0: seconds rewound from trigger point ("-X.Xs")
  double seconds_remaining = 0.0;  // >= 0: remaining rewindable seconds back to oldest slot
  double anim_time = 0.0;
};

inline RewindScrubberHud compute_rewind_scrubber(int keyframe_count, int max_keyframes, int selected_slot,
                                                 double oldest_time, double latest_time, double playback_time,
                                                 double anim_time = 0.0) {
  RewindScrubberHud out;
  const int max_kf = std::max(1, max_keyframes);
  const int count = std::clamp(keyframe_count, 0, max_kf);
  out.total_slots = count;
  out.current_slot = std::clamp(selected_slot, 0, std::max(0, count - 1));
  out.buffer_fraction = std::clamp(float(count) / float(max_kf), 0.0f, 1.0f);
  const double span = std::max(0.0, latest_time - oldest_time);
  if (span > 1e-4) {
    out.playhead_fraction = float(std::clamp((playback_time - oldest_time) / span, 0.0, 1.0));
    out.seconds_rewound = std::max(0.0, latest_time - playback_time);
    out.seconds_remaining = std::max(0.0, playback_time - oldest_time);
  } else if (count > 1) {
    out.playhead_fraction = std::clamp(float(out.current_slot) / float(count - 1), 0.0f, 1.0f);
    out.seconds_rewound = double(std::max(0, (count - 1) - out.current_slot)) * 2.5;
    out.seconds_remaining = double(out.current_slot) * 2.5;
  } else {
    out.playhead_fraction = 1.0f;
    out.seconds_rewound = 0.0;
    out.seconds_remaining = 0.0;
  }
  out.anim_time = anim_time;
  return out;
}

// R3-UI-12: De-duplicated World Map icon legend entries (bottom-right road bar owns DRIVEN/UNDRIVEN).
struct MapLegendItem {
  const char* label = "";
  uint32_t color = 0;
  bool is_line = false;
  bool is_new_tag = false;
};

inline std::vector<MapLegendItem> map_legend_entries() {
  auto pack = [](uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) -> uint32_t {
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
  };
  return {
      {"FESTIVAL EVENT", pack(249, 0, 100), false, false},
      {"OUTPOST HUB", pack(255, 146, 36), false, false},
      {"STREET HUB", pack(44, 140, 232), false, false},
      {"PR STUNT", pack(218, 255, 0), false, false},
      {"SPEED CAMERA", pack(56, 206, 255), false, false},
      {"BARN / SIGN", pack(246, 206, 78), false, false},
      {"NEW EVENT", pack(249, 0, 100), false, true},
      {"GPS ROUTE", pack(74, 238, 97), true, false},
  };
}

// R3-UI-12: Clickable top filter tab strip layout helper for World Map overlay.
struct MapFilterTabRect {
  int filter_index = 0;
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

inline std::vector<MapFilterTabRect> layout_map_filter_tabs(const std::vector<std::string>& tabs, float start_x,
                                                            float y0, float h_px, float scale, float max_right_x) {
  std::vector<MapFilterTabRect> out;
  if (tabs.empty()) return out;
  const float gap = 4.0f * scale;
  const float min_w = 42.0f * scale;
  float x = start_x;
  out.reserve(tabs.size());
  for (size_t i = 0; i < tabs.size(); ++i) {
    const float est_w = std::max(min_w, (float(tabs[i].size()) * 7.2f + 16.0f) * scale);
    const float x1 = std::min(max_right_x, x + est_w);
    if (x1 <= x + 12.0f * scale) break;
    out.push_back({int(i), x, y0, x1, y0 + h_px});
    x = x1 + gap;
  }
  return out;
}

}  // namespace carview

#ifndef CARVIEW_GAME_HUD_LOGIC_ONLY

#include <map>

#include "freeroam/festival_ai.h"
#include "freeroam/skills.h"
#include "freeroam/speed_zones.h"
#include "hud_notify.h"
#include "race_flow.h"
#include "rally_hub.h"
#include "title_menu.h"
#include "ui.h"

namespace carview {

// What the HUD shows of a race (RaceView::hud_info).
struct RaceHudRow {
  std::string name;  // the driver's name, or player's name
  int place = 0;
  bool player = false, finished = false;
  double time = -1;  // the finish time, or -1
  double gap = -1;   // s behind the leader while racing (-1: the leader)
  std::string car, car_class;
  int pi = -1;
  double best_lap = -1;
};

struct RaceHud {
  enum Phase { kGrid, kCountdown, kRacing, kFinished } phase = kGrid;
  std::string event;
  bool circuit = false, stage = false;
  int lap = 0, laps = 1, place = 0, cars = 1, digit = 0;
  double clock = 0, progress = 0, best_lap = -1, last_lap = -1, finish_time = -1;
  double lap_time = 0;  // s into the current lap
  // Cash reward breakdown (SPEC-UI-LAYOUTS c).
  int64_t base_credits = 0;
  int64_t mileage_bonus = 0;
  int difficulty_pct = 0;
  int64_t difficulty_bonus = 0;
  int assists_pct = 0;
  int64_t assists_bonus = 0;
  int64_t bounty_bonus = 0;
  int64_t skills_bonus = 0;
  int64_t nemesis_bonus = 0;
  int64_t credits = 0;
  int wristband = 0;
  int driver_xp = 0, driver_level = 0, affinity_xp = 0, affinity_level = 0;
  int64_t affinity_bonus_credits = 0;
  // Extended Career / Driver Level & Manufacturer Affinity breakdown for Post-Race UI.
  int driver_xp_total = 0;
  int driver_next_xp = 0;
  std::string affinity_make;
  int affinity_xp_total = 0;
  int affinity_next_xp = 0;
  double affinity_discount_pct = 0.0;
  std::string gift_car_name;  // non-empty when a Driver Level / Affinity gift car was earned
  // Live lap split delta vs best/target lap in seconds (<0 green/faster, >0 red/slower).
  double lap_split_delta = 0.0;
  bool has_lap_split_delta = false;
  // Timestamp (matching r.clock) when the active results_page was entered (<0: auto-tracked).
  double page_enter_time = -1.0;
  // Airborne Showcase live progress & gap (PLANE_RACE_001..007).
  bool showcase = false;
  std::string showcase_aircraft;
  double showcase_progress = 0.0;
  double showcase_gap_m = 0.0;  // + when player is ahead of the aircraft
  int stage_place = 0, stage_field = 0;
  std::vector<RaceHudRow> rows;  // by place
  // Wristband progress after the award.
  int band_before = -1, band_after = -1, band_lo = 0, band_hi = -1;
  std::string band_name, band_next;
  bool band_up = false;  // true -> COLLECT IT FROM RACE CENTRAL
  bool wrong_way = false;
  bool missed_checkpoint = false;
  double missed_checkpoint_countdown_s = 0.0;
  int results_page = 0;    // 0 FINISHED banner, 1 CASH REWARD, 2 POINTS & CAREER XP, 3 NEW CAR WON, 4 CAREER/AFFINITY XP
  bool results_table = false;
  int64_t balance = -1;
  std::string event_type;  // "FESTIVAL RACE", "STREET RACE", "SHOWCASE", etc.
  std::string won_car_name;  // non-empty when a prize car was won (page 3)
  // Pre-race Grid Menu (race-grid.png parity).
  bool grid_menu_open = false;
  std::vector<std::string> grid_menu;
  int grid_selected = 0;
  std::string grid_car_info;
  // The original's screen order around the race (race_flow.h), drawn from
  // the game's scenes (race_screens.h); kNone: the hand-drawn pages above.
  race_flow::Screen screen = race_flow::Screen::kNone;
  double screen_t = 0;      // seconds on `screen`
  std::string title;        // the event's display name (HORIZON PRESENTS THE GAUNTLET)
  std::string rival_name;   // the callout's rival (Beat {0})
  int64_t rival_reward = 0;
  std::string stay_car;     // STAY IN CAR's car name
  int stay_selected = 0;    // 0 YES, 1 NO
};

// The front end (120_P_PRE_SPLASH, title_menu.h): the model's state for
// one frame. The PressStart movie draws under it.
struct TitleScreenHud {
  bool active = false;
  TitleMenu menu;
};

// Free-roam world-space projected event / Festival trigger marker (hud-freeroam-event-marker.png).
struct WorldMarkerHud {
  bool visible = false;
  bool in_range = false;  // player within trigger radius -> show ENTER NOW [X]
  float screen_x = 640, screen_y = 300;
  std::string title;     // e.g. "RED ROCK RAMPAGE", "RACE CENTRAL"
  std::string subtitle;  // e.g. "FESTIVAL RACE  •  CLASS C", "COLLECT GREEN WRISTBAND!"
  std::string prompt;    // e.g. "ENTER NOW", "COLLECT WRISTBAND"
  std::string badge;     // e.g. "NEW", "1ST", "LOCKED"
  bool is_festival = false;
  bool eligible = true;
  double distance_m = 0;
  // The prompt's sponsor line ("HORIZON presents"; empty: none) and its
  // icon (an ICONS/ACTIVATIONZONEICONS texture; empty: the scene's).
  std::string sponsor;
  std::string icon;
};

// Top-centre Skill Chain & Popularity Rank disc (race-hud.png, race-hud-skill-banked.png).
struct SkillChainHud {
  bool active = false;
  int chain_points = 0;
  int multiplier = 1;
  float multiplier_f = 1.0f;       // exact 1.0x..5.0x in 0.1x steps
  double chain_remaining_s = 0.0;  // remaining seconds before banking
  double chain_timeout_s = 3.0;    // total bank window duration (s)
  std::string last_skill;
  int last_points = 0;
  double last_skill_at = -1e9;
  int banked_total = 0;
  double banked_at = -1e9;
  double lost_at = -1e9;
  int popularity_rank = 250;
  double popularity_progress = 0.0;
  double rank_up_at = -1e9;
  // Smooth animation channels (populated by PopularityBadgeAnimator or derived from time).
  int prev_popularity_rank = -1;     // rank prior to most recent rank-up (<0: infer rank + 1)
  double pop_anim_progress = -1.0;   // smoothly interpolated radial arc [0..1] (<0: derive)
  double pop_rank_anim_t = -1.0;     // normalized rank-up celebration phase [0..1] (<0: derive)
};

// Stateful animator for the Popularity Rank HUD disc (smooth ring fill, rank-up flip/pop, skill pulse).
struct PopularityBadgeAnimator {
  bool initialized = false;
  int last_rank = 250;
  int prev_rank = 250;
  int last_chain_points = 0;
  double last_banked_at = -1e9;
  double anim_progress = 0.0;
  double rank_anim_t = 1.0;
  double last_time = -1.0;

  void reset();
  void update(const SkillChainHud& sk, double time);
  void apply(SkillChainHud& sk) const;
};

struct PopularityBadgeAnimFrame {
  float radial_progress = 0.05f;
  float disc_scale = 1.0f;
  float flip_phase = 1.0f;
  int display_rank = 250;
  float text_scale_y = 1.0f;
  float text_offset_y = 0.0f;
  float text_alpha = 1.0f;
  float ring_flash_alpha = 0.0f;
  float ring_flash_angle = -1.5708f;
  float skill_pulse_alpha = 0.0f;
};

PopularityBadgeAnimFrame evaluate_popularity_badge_anim(const SkillChainHud& sk, double time);

// Projected overhead opponent / Festival AI car name tag (race-hud.png).
struct NameTagHud {
  float screen_x = 0, screen_y = 0;
  std::string name;
  bool leader = false;
  double distance_m = -1.0;  // world distance in meters (<0 if caller did not populate)
  std::string car_class;     // optional class letter ("E".."X")
  int pi = -1;               // optional PI rating (>0)
};

struct NameTagLod {
  float scale = 1.0f;
  float alpha = 1.0f;
  bool visible = true;
};
NameTagLod compute_name_tag_lod(double distance_m);

struct ResolvedNameTag {
  size_t source_index = 0;
  float orig_x = 0.0f, orig_y = 0.0f;
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
  float scale = 1.0f;
  float alpha = 1.0f;
  float badge_w = 0.0f;
  float cls_w = 0.0f;
  float pi_w = 0.0f;
  bool leader = false;
  bool has_badge = false;
  std::string car_class;
  std::string pi_text;
  std::string driver;
};

struct WorldMarkerLayout {
  bool visible = false;
  float anchor_x = 0.0f, anchor_y = 0.0f;
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
  float scale = 1.0f;
  float alpha = 1.0f;
};

void layout_world_labels(const std::vector<NameTagHud>& tags, const WorldMarkerHud* world_marker, bool in_race,
                         float w, float h, std::vector<ResolvedNameTag>& out_tags,
                         WorldMarkerLayout* out_marker = nullptr);

void render_world_labels_and_nameplates(ui::List& l, const std::vector<NameTagHud>& tags,
                                        const WorldMarkerHud* world_marker, bool in_race, float w, float h,
                                        bool metric_units);

struct PopupTitleParts {
  std::string label;
  std::string counter;
};
PopupTitleParts split_popup_title_counter(std::string_view title);

// Celebratory overlay for Wristband Collection at Race Central.
struct CeremonyHud {
  bool active = false;
  std::string headline;     // "NEW WRISTBAND EARNED!"
  std::string band_name;    // "GREEN WRISTBAND"
  std::string band_colour;  // "GREEN" (for WRISTBANDS/GREENXL)
  std::string subtitle;     // "NEW FESTIVAL EVENTS UNLOCKED"
  std::string bonus_car;    // e.g. "1997 Mazda RX-7" (if Rewards_Wristband car awarded)
};

// Free-roam GPS turn-by-turn HUD prompt (above minimap).
struct GpsHud {
  bool active = false;
  int turn_kind = 0;  // matches traffic::GpsTurnKind
  std::string turn_text;
  std::string dest_label;
  double dist_to_turn_m = 0;
  double dist_to_dest_m = 0;
  bool confirmed_uturn = false;  // true only when sustained wrong-way driving on an active route
  bool recalculating = false;    // subtle pulsing "RECALCULATING..." SatNav pill after wrong turn
  double time = 0.0;             // HUD clock for recalculating pulse animation
};

// Interactive Pause Map overlay card (hovered destination info, filter tabs,
// roads discovered counter, and legend/help bar).
struct MapScreenHud;
struct MapOverlayHud {
  bool active = false;
  // The scene-driven map (map_screen.h); null: the interim overlay below.
  const MapScreenHud* screen = nullptr;
  std::string title;
  std::string subtitle;
  std::string detail_line;      // rich event / PR stunt / barn find rumour metadata line
  std::string marker_icon_tag;  // MapProfile icon tag for marker-accurate hover tooltip badge
  std::string action_hint;      // e.g. "ENTER EVENT", "FAST TRAVEL"
  std::string gps_hint;         // e.g. "SET GPS ROUTE", "CLEAR GPS"
  bool gps_active = false;
  double distance_m = -1;
  int active_filter = 0;
  std::vector<std::string> filter_tabs;
  int roads_discovered = 0;
  int roads_total = 216;
  double zoom = 1.0;
  double center_x = 0.0, center_z = 0.0;
  double scale_mpp = 0.0;  // metres per pixel (>0 enables world-projected Colorado region labels)
  bool treasure_map = false;
  bool show_legend = true;  // toggleable icon Legend panel
};

// One sponsor challenge row for H_SPONSOR_CHALLENGE_STATUS (18 real SponsorLogoTile/*.xds logos).
struct SponsorChallengeRow {
  std::string sponsor_name;     // e.g. "OAKLEY", "HKS", "REDBULL", "ADIDAS", "BONDURANT", "GOPRO"
  std::string challenge_title;  // e.g. "DRIFT KING", "NEAR MISS", "AIRBORNE", "TOP SPEED"
  std::string logo_key;         // passed to ui4::sponsor_logo_tile()
  int current = 0;
  int target = 25;
  int tier = 1;
  int max_tiers = 5;
  int64_t reward_cr = 10000;
  std::string description;   // the challenge's details text, its target filled in
  std::string logo_texture;  // SPONSORLOGOTILE/<NAME> (the scene's SponsorLogo); empty: logo_key
  bool fame = false;         // the popularity challenge: `current` is the chart position
  int previous_target = 0;   // fame: the last tier's position (the bar's start)
  bool completed = false;    // every tier claimed
};

// The Sponsor Challenges rows from the game's sponsor data and the profile's
// progress: each challenge's sponsor, name and details resolved in the
// SponsorshipChallenges table, its next tier, and the popularity chart
// position for the popularity challenge.
std::vector<SponsorChallengeRow> sponsor_challenge_rows(const freeroam::SkillsData& data,
                                                        const std::map<std::string, freeroam::SponsorProgress>* progress,
                                                        int popularity_rank, int fame_claimed);

// The career page's board (right of the career page list).
struct CareerBoard {
  std::string band_name, band_next;
  int points = 0, band_lo = 0, band_hi = -1;
  int64_t credits = 0, winnings = 0;
  int races = 0, wins = 0, podiums = 0, garage = 0;
  int signs = 0, signs_total = 100, barns = 0, barns_total = 0;
  int roads = 0, roads_total = 216;
  int outposts = 0, outposts_total = 0;
  int pr_stunts = 0, pr_stunts_total = 0;
  int club_cars = 0, club_done = 0, club_total = 0;
  int popularity_rank = 250;
  int64_t max_skills = 0;
  int driver_level = 1, driver_xp = 0;
  int speed_traps = 0, speed_traps_total = 0;
  int speed_zones = 0, speed_zones_total = 0;
  int h2h_wins = 0, h2h_races = 0;
  int sponsors_tiers = 0;
  bool rally = false;
  int rally_level = 0, rally_xp = 0, rallies = 0, rally_wins = 0;
  int packs_owned = 0, packs_total = 0;
  std::vector<SponsorChallengeRow> sponsors;
  bool show_sponsors = false;
  int sponsor_selected = 0;
};

// Draws authentic FH1 H_SPONSOR_CHALLENGE_STATUS (+3.0 deg 6-row SponsorLogoTile/* + #DAFF00 BARBASE26PX).
void draw_sponsor_challenges_ui4(ui::List& l, float w, float h, const std::vector<SponsorChallengeRow>& rows,
                                 int selected, int64_t credits = -1);

// Active Outpost PR Stunt HUD (Speed Trap, Skill/Drift, or Photo Op).
struct PrStuntHud {
  bool active = false;
  std::string kind_badge;   // e.g. "SPEED STUNT", "SKILL STUNT", "PHOTO OP"
  std::string title;
  std::string instruction;
  std::string status_line;
  double time_remaining_s = 0.0;
  double progress = 0.0;    // 0..1
  bool in_zone = false;
  bool is_photo = false;
};

struct PauseScreenHud;
struct EventSummaryHud;

struct DriveHud {
  // The pause menu from its scene (pause_screen.h); null: the menu (if any)
  // is drawn by menu_panel.
  const PauseScreenHud* pause_screen = nullptr;
  // The event summary at a marker (race_screens.h); null: closed.
  const EventSummaryHud* event_summary = nullptr;
  const CareerBoard* career = nullptr;
  const TitleScreenHud* title_screen = nullptr;
  const WorldMarkerHud* world_marker = nullptr;
  const SkillChainHud* skills = nullptr;
  const CeremonyHud* ceremony = nullptr;
  const MapOverlayHud* map_overlay = nullptr;
  const GpsHud* gps = nullptr;
  const PrStuntHud* pr_stunt = nullptr;
  const freeroam::SpeedActivityHud* speed_hud = nullptr;
  const freeroam::HeadToHeadHud* h2h = nullptr;
  const RallyStageHud* rally_stage = nullptr;
  const RallyHubState* rally_hub = nullptr;
  const RallyServiceAreaState* rally_service = nullptr;
  const RallyChampResultsState* rally_results = nullptr;
  int driver_level = 1, driver_xp = 0, driver_next_xp = 0;
  int affinity_level = 0;
  double affinity_discount_pct = 0.0;
  std::string sponsor_title, sponsor_detail;
  double sponsor_at = -1e9;
  std::vector<NameTagHud> name_tags;
  HudToggles hud;  // the original's HUD options
  double kmh = 0, rpm = 0, redline_rpm = 7000;
  double rpm_rate = 0.0;  // smoothed RPM/s acceleration rate for the Horizon pink needle trail
  int gear = 1;
  bool abs = false;
  bool tcs = false;
  bool stm = false;
  bool abs_enabled = true;
  bool tcs_enabled = false;
  bool stm_enabled = false;
  double abs_flash_timer = 0.0;
  double tcs_flash_timer = 0.0;
  double stm_flash_timer = 0.0;
  float rewind_buffer_fraction = 0.0f;
  int rewind_slot = 0;
  int rewind_total_slots = 0;
  double rewind_seconds_remaining = 0.0;
  bool has_boost = false;
  double boost_psi = 0.0;
  double boost_max_psi = 18.0;
  int speedo_mode = 0;  // 0=Analog, 1=Digital, 2=Off
  double barn_radar_proximity = 0.0;  // 0..1 pulsing barn find proximity radar inside rumor circle
  std::string club_tag;               // optional 4-char Car Club [TAG]
  bool driving = true;
  bool touch_hud_active = false;  // compacts & lifts tach above Android touch pedals
  bool metric_units = true;       // true: km/h & km/m, false: MPH & mi/ft (global Units setting)
  bool subtitle_active = false;   // true when bottom-center campaign subtitles are visible (lifts bottom banners)
  int subtitle_lines = 0;         // wrapped subtitle line count (0 = default 3-line clearance)
  float subtitle_box_h_canvas = 0.0f;  // exact subtitle box height in 720p canvas units (0 = derive from lines)
  const RaceHud* race = nullptr;
  std::string toast;
  std::string menu_title;
  std::vector<std::string> menu;
  int menu_selected = 0;
  // Optional right-hand detail card for sub-menus (Events / Garage / Autoshow).
  std::string detail_title;
  std::vector<std::string> detail_lines;
  std::string menu_footer = "ENTER SELECT   ESC BACK";
  double time = 0;
  int64_t credits = -1;
  std::string band_name;
  int band_points = 0, band_lo = 0, band_hi = -1;
  bool pause = false;
  bool band_collect_ready = false;  // true only when next wristband is ready at Race Central
  std::string band_next, car_name, car_class;
  int band_level = 0, car_pi = 0, popularity_rank = 250;
  std::string popup_title, popup_text;
  double popup_at = -1e9;
  double popup_progress = -1;
  uint32_t popup_colour = 0;
  // The notification's kind and icon, its tag (a sponsor's RANK 4
  // UNLOCKED!) and the sponsor logo texture (CombinedNotification).
  NotifyKind popup_kind = NotifyKind::kEngine;
  NotifyIcon popup_icon = NotifyIcon::kMisc;
  std::string popup_tag, popup_logo;
};
inline constexpr double kPopupSeconds = 4.5;

void build_drive_hud(ui::List& out, const DriveHud& state, float w, float h);
float drive_hud_dial_height(float h);

void set_car_names(std::function<std::string(const std::string&)> source);
std::string car_name(const std::string& media);
void set_game_text(std::function<std::string(const std::string&)> source);
std::string game_text(const std::string& value);
// The table text as stored, before game_text() strips its markup (and with it
// the "{0}" format slots).
std::string game_text_raw(const std::string& value);
std::string strip_ui_inline_tags(std::string s);
void set_ui_textures(std::function<bool(const std::string&, std::array<float, 4>&)> source);
bool ui_texture(const std::string& path, std::array<float, 4>& uv);

std::string thousands(int64_t v);
const char* place_suffix(int place);
std::string race_clock(double seconds);
std::string format_speed_unit(double mph, bool metric, bool decimals = true);
std::string format_distance_unit(double meters, bool metric);
std::string format_gap_unit(double meters, bool metric);
std::string format_boost_unit(double boost_psi, bool metric);
std::string format_split_delta(double delta_s);
// Elapsed seconds since entering the current post-race results page (measures from
// r.page_enter_time when >= 0, else auto-tracks per-page entry time against r.clock).
double post_race_page_elapsed(const RaceHud& r);
void reset_post_race_page_timer();
// Strips format placeholders (e.g. "{0} mph" -> "mph", "%s km/h" -> "km/h")
// and falls back cleanly when the string table entry is empty or unresolved.
std::string format_unit_label(std::string_view raw, std::string_view fallback);
bool should_show_gps_turn_banner(const GpsHud& gps);
bool should_show_gps_turn_distance(const GpsHud& gps);
// Computes dynamic bottom-center banner lift (720p canvas units) so bottom banners
// clear multi-line campaign subtitles + speaker badge without overlap.
float compute_subtitle_bottom_lift(bool subtitle_active, int subtitle_lines = 0, float subtitle_box_h_canvas = 0.0f);

enum class RewindOverlayAction {
  kNone = 0,
  kResume,
  kRewindFurther,
  kViewReplay,
  kCancel,
};

// Draws the 921_I_REWIND overlay with clickable ui4::kHitButton button-bar hit zones
// and live animated scrubber bar when `scrubber` is provided.
void build_rewind_overlay(ui::List& list, float w, float h, bool rewinding, bool loop_rewind, bool paused_at_keyframe,
                          bool at_oldest_slot, bool settling, const RewindScrubberHud* scrubber = nullptr);
// Maps a clicked ui4::kHitButton index from build_rewind_overlay to its RewindOverlayAction.
RewindOverlayAction rewind_overlay_action_for_button(bool rewinding, bool paused_at_keyframe, bool at_oldest_slot,
                                                     int button_index);

}  // namespace carview

#endif  // CARVIEW_GAME_HUD_LOGIC_ONLY
