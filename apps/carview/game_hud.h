// carview --drive: the game's HUD and menus on the UI layer (ui.h). Ours:
// the game's own HUD is not specified. A speedometer dial (rpm arc with the
// redline, speed, gear), the race's position, lap and time panels, the
// standings, the countdown, the results card with its rewards, toasts, and
// the menus (the pause menu and its pages) as a side panel.
#pragma once

#include <cstdint>
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
};
struct RaceHud {
  enum Phase { kGrid, kCountdown, kRacing, kFinished } phase = kGrid;
  std::string event;
  bool circuit = false, stage = false;
  int lap = 0, laps = 1, place = 0, cars = 1, digit = 0;
  double clock = 0, progress = 0, best_lap = -1, last_lap = -1, finish_time = -1;
  int64_t credits = 0;
  int wristband = 0;
  int stage_place = 0, stage_field = 0;
  std::vector<RaceHudRow> rows;  // by place
};

struct DriveHud {
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
};

// Builds the HUD for a w x h screen into `out` (appended).
void build_drive_hud(ui::List& out, const DriveHud& state, float w, float h);

// The dial's top edge (px from the bottom), for things stacked above it.
float drive_hud_dial_height(float h);

// "12,500".
std::string thousands(int64_t v);
// "1ST", "2ND", ...
const char* place_suffix(int place);
// "1:23.45".
std::string race_clock(double seconds);

}  // namespace carview
