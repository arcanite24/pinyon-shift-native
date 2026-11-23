// carview --drive: the game's HUD, title screen, and menus on the UI layer (ui.h).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "freeroam/festival_ai.h"
#include "freeroam/speed_zones.h"
#include "rally_hub.h"
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
  int results_page = 0;    // 0 FINISHED banner, 1 CASH REWARD, 2 POINTS EARNED, 3 NEW CAR WON
  bool results_table = false;
  int64_t balance = -1;
  std::string event_type;  // "FESTIVAL RACE", "STREET RACE", "SHOWCASE", etc.
  std::string won_car_name;  // non-empty when a prize car was won (page 3)
  // Pre-race Grid Menu (race-grid.png parity).
  bool grid_menu_open = false;
  std::vector<std::string> grid_menu;
  int grid_selected = 0;
  std::string grid_car_info;
};

// Title screen state (120_P_PRE_SPLASH parity).
struct TitleScreenHud {
  bool active = false;
  bool menu_phase = false;  // false: splash ("PRESS ENTER / [A] TO START"), true: title menu
  bool has_save = false;
  bool confirm_new_career = false;
  int confirm_selected = 1;  // 0 CONFIRM, 1 CANCEL
  std::vector<std::string> menu;
  int selected = 0;
  // Save preview card on right side when hovering CONTINUE CAREER.
  std::string save_band_name, save_car_name, save_car_class;
  int save_car_pi = 0, save_band_points = 0, save_wins = 0, save_races = 0, save_pop_rank = 250;
  int64_t save_credits = 0;
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
};

// Top-centre Skill Chain & Popularity Rank disc (race-hud.png, race-hud-skill-banked.png).
struct SkillChainHud {
  bool active = false;
  int chain_points = 0;
  int multiplier = 1;
  std::string last_skill;
  int last_points = 0;
  double last_skill_at = -1e9;
  int banked_total = 0;
  double banked_at = -1e9;
  double lost_at = -1e9;
  int popularity_rank = 250;
  double popularity_progress = 0.0;
  double rank_up_at = -1e9;
};

// Projected overhead opponent name tag in races (race-hud.png).
struct NameTagHud {
  float screen_x = 0, screen_y = 0;
  std::string name;
  bool leader = false;
};

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
};

// Interactive Pause Map overlay card (hovered destination info, filter tabs,
// roads discovered counter, and legend/help bar).
struct MapOverlayHud {
  bool active = false;
  std::string title;
  std::string subtitle;
  std::string action_hint;  // e.g. "ENTER EVENT", "FAST TRAVEL"
  std::string gps_hint;     // e.g. "SET GPS ROUTE", "CLEAR GPS"
  bool gps_active = false;
  double distance_m = -1;
  int active_filter = 0;
  std::vector<std::string> filter_tabs;
  int roads_discovered = 0;
  int roads_total = 216;
  double zoom = 1.0;
  bool treasure_map = false;
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
};

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

struct DriveHud {
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
  double kmh = 0, rpm = 0, redline_rpm = 7000;
  double rpm_rate = 0.0;  // smoothed RPM/s acceleration rate for the Horizon pink needle trail
  int gear = 1;
  bool abs = false;
  bool tcs = false;
  bool driving = true;
  bool touch_hud_active = false;  // compacts & lifts tach above Android touch pedals
  bool metric_units = true;       // true: km/h & km/m, false: MPH & mi/ft (global Units setting)
  bool subtitle_active = false;   // true when bottom-center campaign subtitles are visible (lifts bottom banners)
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
};
inline constexpr double kPopupSeconds = 4.5;

void build_drive_hud(ui::List& out, const DriveHud& state, float w, float h);
float drive_hud_dial_height(float h);

void set_car_names(std::function<std::string(const std::string&)> source);
std::string car_name(const std::string& media);
void set_game_text(std::function<std::string(const std::string&)> source);
std::string game_text(const std::string& value);
void set_ui_textures(std::function<bool(const std::string&, std::array<float, 4>&)> source);
bool ui_texture(const std::string& path, std::array<float, 4>& uv);

std::string thousands(int64_t v);
const char* place_suffix(int place);
std::string race_clock(double seconds);
std::string format_speed_unit(double mph, bool metric, bool decimals = true);
std::string format_distance_unit(double meters, bool metric);
std::string format_gap_unit(double meters, bool metric);
// Strips format placeholders (e.g. "{0} mph" -> "mph", "%s km/h" -> "km/h")
// and falls back cleanly when the string table entry is empty or unresolved.
std::string format_unit_label(std::string_view raw, std::string_view fallback);
bool should_show_gps_turn_banner(const GpsHud& gps);
bool should_show_gps_turn_distance(const GpsHud& gps);

}  // namespace carview
