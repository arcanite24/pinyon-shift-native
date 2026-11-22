// carview --drive: the game's HUD, title screen, and menus on the UI layer (ui.h).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
  int64_t credits = 0;
  int wristband = 0;
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

// Interactive Pause Map overlay card (hovered destination info + help bar).
struct MapOverlayHud {
  bool active = false;
  std::string title;
  std::string subtitle;
  std::string action_hint;  // e.g. "ENTER EVENT", "FAST TRAVEL"
  double distance_m = -1;
};

// The career page's board (right of the career page list).
struct CareerBoard {
  std::string band_name, band_next;
  int points = 0, band_lo = 0, band_hi = -1;
  int64_t credits = 0, winnings = 0;
  int races = 0, wins = 0, podiums = 0, garage = 0;
  int signs = 0, signs_total = 100, barns = 0, barns_total = 0;
  int club_cars = 0, club_done = 0, club_total = 0;
  int popularity_rank = 250;
  int64_t max_skills = 0;
  bool rally = false;
  int rally_level = 0, rally_xp = 0, rallies = 0, rally_wins = 0;
  int packs_owned = 0, packs_total = 0;
};

struct DriveHud {
  const CareerBoard* career = nullptr;
  const TitleScreenHud* title_screen = nullptr;
  const WorldMarkerHud* world_marker = nullptr;
  const SkillChainHud* skills = nullptr;
  const CeremonyHud* ceremony = nullptr;
  const MapOverlayHud* map_overlay = nullptr;
  std::vector<NameTagHud> name_tags;
  double kmh = 0, rpm = 0, redline_rpm = 7000;
  int gear = 1;
  bool abs = false;
  bool driving = true;
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

}  // namespace carview
