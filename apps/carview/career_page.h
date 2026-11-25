// carview --drive: Authentic Forza Horizon 1 Progress / Career screen with 4
// sub-tabs (PROGRESS, SPONSOR CHALLENGES, STATS, CONTENT PACKS), UTF-8 safe
// text handling, and localized StringTables labels.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cars/sources.h"
#include "ui.h"

namespace carview {

struct SponsorChallengeRow;

enum class CareerTab : int {
  kProgress = 0,
  kSponsorChallenges = 1,
  kStats = 2,
  kContentPacks = 3,
  kCount = 4,
};

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
  // Extended FH1 Progress & Exploration metrics (optional, default-initialized for backwards compatibility).
  int popularity_rank = 250;
  int roads_discovered = 0, roads_total = 216;
  int signs_total = 100;
  int outposts_discovered = 0, outposts_total = 10;
  int pr_stunts_done = 0, pr_stunts_total = 30;
  double total_distance_km = 0.0;
  double top_speed_kmh = 0.0;
};

class CareerPage {
 public:
  void open(const CareerSummary& s, std::vector<cars::PackInfo> packs);
  void close() {
    open_ = false;
    tab_ = CareerTab::kProgress;
  }
  bool is_open() const { return open_; }

  // 4-tab FH1 Progress navigation (PROGRESS / SPONSOR CHALLENGES / STATS / CONTENT PACKS).
  CareerTab active_tab() const { return tab_; }
  void set_tab(CareerTab tab);
  void switch_tab(int delta);
  std::vector<std::string> tab_labels() const;

  void move(int by);
  void select_visible(int vis_idx);
  size_t selected_index() const { return selected_; }
  int visible_selected() const;
  bool show_sponsors() const { return tab_ == CareerTab::kSponsorChallenges; }
  void set_show_sponsors(bool show) {
    set_tab(show ? CareerTab::kSponsorChallenges : CareerTab::kProgress);
  }
  void toggle_sponsors() {
    set_tab(tab_ == CareerTab::kSponsorChallenges ? CareerTab::kProgress : CareerTab::kSponsorChallenges);
  }
  int sponsor_selected() const { return sponsor_selected_; }
  void move_sponsor(int by, int count);
  void select_sponsor(int idx, int count);
  void fill(std::string& title, std::vector<std::string>& lines, int& selected) const;
  const CareerSummary& summary() const { return summary_; }
  const std::vector<cars::PackInfo>& packs() const { return packs_list_; }
  int packs_owned() const { return owned_; }
  int packs_total() const { return packs_; }

 private:
  void rebuild_lines();

  std::vector<std::string> lines_;
  std::vector<cars::PackInfo> packs_list_;
  CareerSummary summary_;
  int owned_ = 0, packs_ = 0;
  size_t selected_ = 0;
  bool open_ = false;
  CareerTab tab_ = CareerTab::kProgress;
  int sponsor_selected_ = 0;
};

// Renders the 4-tab FH1 Progress / Sponsor Challenges / Stats / Content Packs screen via UI4 chrome.
void draw_career_page_ui4(ui::List& l, float w, float h, const CareerPage& page, int64_t credits_override = -1,
                          float open_progress = 1.0f);
void draw_career_page_ui4(ui::List& l, float w, float h, const CareerPage& page,
                          const std::vector<SponsorChallengeRow>& sponsors, int64_t credits_override = -1,
                          float open_progress = 1.0f);

// Sanitizes control characters to spaces while preserving all UTF-8 international text intact.
std::string font_text(const std::string& s);

}  // namespace carview
