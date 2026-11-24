// 1000 Club UI & Live Medal Ceremony HUD (apps/carview/club_view.*):
// - Full 1000 Club Pause Menu screen with car medal slots (up to 6),
//   localized challenge titles & {0}/{1} descriptions, live progress bars,
//   first-visit intro banner, and 1000 Club medal achievement counters.
// - Live Medal Unlock Ceremony banner & in-progress challenge HUD strip.
#pragma once

#include <string>
#include <vector>

#include "freeroam/club.h"
#include "ui.h"

namespace carview {

struct ClubChallengeRow {
  std::string title;
  std::string description;
  std::string hint;
  std::string status;
  double progress = 0.0;  // 0..1
  bool completed = false;
  bool has_save_progress = false;
};

struct ClubScreenState {
  bool active = false;
  bool challenges_begun = false;
  bool show_intro = false;
  std::string gate_reason;  // non-empty if blocked (needs green wristband, DLC, garage car, etc.)
  std::string car_name;
  int selected = 0;
  int car_medals = 0;
  int car_total = 0;
  std::vector<ClubChallengeRow> rows;
  freeroam::club::ClubMedalStats stats{};
};

struct ClubMedalCeremony {
  bool active = false;
  std::string challenge_title;
  std::string challenge_desc;
  std::string car_name;
  int car_medals = 0;
  int car_total = 0;
  int total_medals = 0;
  int cars_with_medal = 0;
  double age = 0.0;        // s since awarded
  double duration = 5.5;   // s total display duration
  bool dock_bottom_right = false;  // avoids overlapping the 1000 Club pause header when open
};

struct ClubActiveStrip {
  bool active = false;
  std::string title;
  std::string status;
  double progress = 0.0;
};

// Renders the 1000 Club Pause Screen over the live 3D scene.
void draw_club_screen(ui::List& out, int w, int h, const ClubScreenState& st);

// Renders the celebratory live 1000 Club Medal Unlocked HUD banner.
void draw_club_medal_ceremony(ui::List& out, int w, int h, const ClubMedalCeremony& c);

// Renders the compact live challenge-in-progress strip while driving.
void draw_club_active_strip(ui::List& out, int w, int h, const ClubActiveStrip& strip);

// Offline Car Club (451_CLUB_SETTINGS): club name, 4-char [TAG], and shared Club Garage.
struct CarClubCarEntry {
  int car_id = 0;
  std::string media_name;
  std::string display_name;
  std::string class_pi;
  bool shared = false;
  bool active = false;
};

struct CarClubScreenState {
  bool active = false;
  std::string club_name;
  std::string club_tag;
  int selected = 0;  // 0: cycle preset/edit club, 1: cycle tag, 2+: garage cars
  std::vector<CarClubCarEntry> cars;
  std::string notice;
};

// Sanitize a Car Club tag to 1..4 uppercase ASCII alphanumeric characters.
std::string sanitize_club_tag(const std::string& raw);
// Toggle a car ID in the club's shared garage list (returns true if now shared).
bool toggle_club_shared_car(std::vector<int>& shared_ids, int car_id);
// Renders the Offline Car Club & Shared Club Garage UI4 screen.
void draw_car_club_screen(ui::List& out, int w, int h, const CarClubScreenState& st);

}  // namespace carview
