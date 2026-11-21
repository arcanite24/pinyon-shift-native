// carview --drive: the game's HUD and menus on the UI layer (ui.h). Ours:
// the game's own HUD is not specified. A speedometer dial (rpm arc with the
// redline, speed, gear), the race's position, lap and time panels, the
// standings, the countdown, the results card with its rewards, toasts, and
// the menus (the pause menu and its pages) as a side panel.
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
  std::string name;  // the car's name, or "YOU"
  int place = 0;
  bool player = false, finished = false;
  double time = -1;  // the finish time, or -1
  double gap = -1;   // s behind the leader while racing (-1: the leader)
  // The RACE RESULTS table's columns (SPEC-UI-LAYOUTS c): the car's short
  // name, its class and shown index (empty / < 0: none), the best lap.
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
  int64_t credits = 0;
  int wristband = 0;
  int stage_place = 0, stage_field = 0;
  std::vector<RaceHudRow> rows;  // by place
  // The wristband after the award (the results card; band_after < 0:
  // none): the points before and after, the band's threshold and the
  // next band's (-1: the top band), the names, and whether it went up.
  int band_before = -1, band_after = -1, band_lo = 0, band_hi = -1;
  std::string band_name, band_next;
  bool band_up = false;
  bool wrong_way = false;  // the player drives against the race (ours)
  int results_page = 0;
  bool results_table = false;  // the RACE RESULTS table over the FINISHED banner (X; B closes)    // after the finish: 0 the FINISHED banner, 1 the cash reward
  int64_t balance = -1;    // the credits after the award (the cash reward page)
  std::string event_type;  // the title card's event type line (a string-table text)
};

// The career page's board (ours), right of the page's list: the wristband
// and its progress, money, race records and the collections.
struct CareerBoard {
  std::string band_name, band_next;  // the level reached ("" : none), the next
  int points = 0, band_lo = 0, band_hi = -1;  // wristband points, the band's threshold and the next's
  int64_t credits = 0, winnings = 0;
  int races = 0, wins = 0, podiums = 0, garage = 0;
  int signs = 0, signs_total = 100, barns = 0, barns_total = 0;
  int club_cars = 0, club_done = 0, club_total = 0;
  bool rally = false;
  int rally_level = 0, rally_xp = 0, rallies = 0, rally_wins = 0;
  int packs_owned = 0, packs_total = 0;
};

struct DriveHud {
  const CareerBoard* career = nullptr;  // the career page's board, beside the menu
  double kmh = 0, rpm = 0, redline_rpm = 7000;
  int gear = 1;  // < 0 reverse, 0 neutral
  bool abs = false;
  bool driving = true;          // the dial and race panels (false: a menu-only page)
  const RaceHud* race = nullptr;
  std::string toast;            // a line at the top (hints, the car's name)
  std::string menu_title;       // a menu or page; empty: none
  std::vector<std::string> menu;
  int menu_selected = 0;
  std::string menu_footer = "ENTER SELECT   ESC BACK";
  double time = 0;              // s, for the animations
  // The career chip in free roam (top right; credits < 0: none): credits,
  // the wristband's name, points, its threshold and the next's (-1: top).
  int64_t credits = -1;
  std::string band_name;
  int band_points = 0, band_lo = 0, band_hi = -1;
  // The pause menu's panels (SPEC-UI-LAYOUTS d): the next band's name, the
  // band's level (1 up), the car's name, class and index.
  bool pause = false;
  std::string band_next, car_name, car_class;
  int band_level = 0, car_pi = 0;
  // A discovery card (top centre): a sign, a barn find, a medal. Shown
  // for kPopupSeconds from popup_at (drive time); empty title: none.
  std::string popup_title, popup_text;
  double popup_at = -1e9;
  double popup_progress = -1;  // 0..1: a meter under the text (-1: none)
  uint32_t popup_colour = 0;   // 0: the accent
};
inline constexpr double kPopupSeconds = 4.5;

// Builds the HUD for a w x h screen into `out` (appended).
void build_drive_hud(ui::List& out, const DriveHud& state, float w, float h);

// The dial's top edge (px from the bottom), for things stacked above it.
float drive_hud_dial_height(float h);

// A car's name for the screen ("Nissan Skyline GT-R V-spec II"), from the
// source set (the string tables' names); the media name without one.
void set_car_names(std::function<std::string(const std::string&)> source);
std::string car_name(const std::string& media);
// A database text value (`_&N` -> the string table's text), from the source
// set; the value itself without one.
void set_game_text(std::function<std::string(const std::string&)> source);
std::string game_text(const std::string& value);
// The game's UI textures by their path under HORIZON (FORMAT-UISCENES,
// "WRISTBANDS/ORANGE"): the source puts one into the UI atlas and gives its
// uv rectangle; false without it (the HUD draws its interim).
void set_ui_textures(std::function<bool(const std::string&, std::array<float, 4>&)> source);
bool ui_texture(const std::string& path, std::array<float, 4>& uv);

// "12,500".
std::string thousands(int64_t v);
// "1ST", "2ND", ...
const char* place_suffix(int place);
// "1:23.45".
std::string race_clock(double seconds);

}  // namespace carview
