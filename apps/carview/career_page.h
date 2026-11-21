// carview --drive: the career page, opened from the pause menu. A read-only
// list of the career's records (SPEC-EVENT-REWARDS: credits, winnings,
// wristband points and level) and of the content packages the state root
// owns (SPEC-V4-CONTENT, Ownership). Ours: one scrolled list in place of
// the game's career and marketplace screens.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cars/sources.h"

namespace carview {

struct CareerSummary {
  std::string level_name;  // the wristband level reached (empty: none)
  int wristband_points = 0, races = 0, victories = 0, podiums = 0;
  int64_t credits = 0, total_winnings = 0;
  int garage_cars = 0;
  int signs_found = 0, barns_found = 0, barns = 0;  // free roam (SPEC-FREEROAM-COLLECTIBLES)
  int level = -1;  // the wristband level's index, shown when its name is a string key
  int club_cars = 0, club_done = 0, club_total = 0;
  bool has_rally = false;
  int rallies = 0, rally_stages = 0, rally_victories = 0, rally_xp = 0, rally_level = 0;
  int64_t rally_winnings = 0;
  bool rally_owned = true;  // the Rally package (offer 1600) is owned
};

class CareerPage {
 public:
  void open(const CareerSummary& s, std::vector<cars::PackInfo> packs);
  void close() { open_ = false; }
  bool is_open() const { return open_; }
  void move(int by);
  void fill(std::string& title, std::vector<std::string>& lines, int& selected) const;

 private:
  std::vector<std::string> lines_;
  int owned_ = 0, packs_ = 0;
  size_t selected_ = 0;
  bool open_ = false;
};

// Keeps the characters the HUD font draws (letters, digits and a few
// marks); others become spaces.
std::string font_text(const std::string& s);

}  // namespace carview
