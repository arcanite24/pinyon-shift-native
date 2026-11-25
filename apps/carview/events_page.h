// Race Central, Festival Events, and Street Race Hub browser.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "garage/garage.h"
#include "race/career.h"
#include "race/rewards.h"
#include "race/rivals.h"

namespace freeroam {
struct Outpost;
}

namespace carview {

class EventsPage {
 public:
  enum Tab { kAll = 0, kFestival = 1, kStreet = 2, kShowcase = 3, kShowdown = 4, kRivals = 5, kTabCount = 6 };

  struct Entry {
    bool is_collect_wristband = false;
    int wristband_index = -1;
    const race::CareerEvent* event = nullptr;
    bool unlocked = false;
    bool eligible = false;
    bool uses_loaner = false;
    int best_place = 0;
    std::string display_name;
    std::string reason;
    // Rivals / Time Trials metadata (populated when `is_rivals == true`).
    bool is_rivals = false;
    double pb_time_s = 0.0;
    bool pb_clean = false;
    int rival_tier_index = 1;  // 0 = Personal Best, 1 = Bronze, 2 = Silver, 3 = Gold
    std::string rival_tier_label;
    std::string rival_name;
    double target_time_s = 0.0;
    int64_t bounty_cr = 0;
    std::array<race::RivalTarget, 3> rival_tiers{};
  };

  // Opens the career events browser (hub_filter == 0: all events / Race Central;
  // hub_filter > 0: only Street Races belonging to that Street Hub).
  void open(const race::CareerCatalog& catalog, const race::CareerRecord& career, int collected_wristband,
            int popularity_rank, int64_t current_car_id, int current_class_id,
            const std::vector<garage::ClassBand>& bands, int hub_filter = 0,
            const std::filesystem::path& ghosts_dir = {});
  void set_ghosts_dir(const std::filesystem::path& ghosts_dir) { ghosts_dir_ = ghosts_dir; }
  // The wristband points a win pays (the ticket's POINTS AVAILABLE).
  void set_wristband_tables(const race::WristbandTables* t) { wristbands_ = t; }
  void cycle_rival_tier(int by);
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  void move(int by);
  void switch_tab(int by);
  void select_index(size_t idx) {
    if (idx < entries_.size()) selected_ = idx;
  }
  void set_tab(int t) {
    if (t >= 0 && t < kTabCount && int(tab_) != t) {
      tab_ = Tab(t);
      rebuild();
    }
  }
  const Entry* selected_entry() const;
  const std::vector<Entry>& entries() const { return entries_; }
  size_t selected_index() const { return selected_; }
  Tab active_tab() const { return tab_; }
  int hub_filter() const { return hub_filter_; }

  void fill(std::string& title, std::vector<std::string>& lines, int& selected, std::string& detail_title,
            std::vector<std::string>& detail_lines) const;

 private:
  void rebuild();

  const race::CareerCatalog* catalog_ = nullptr;
  const race::CareerRecord* career_ = nullptr;
  const race::WristbandTables* wristbands_ = nullptr;
  int collected_wristband_ = 0;
  int popularity_rank_ = 250;
  int64_t car_id_ = 0;
  int car_class_id_ = 0;
  std::vector<std::string> class_names_;
  int hub_filter_ = 0;
  std::filesystem::path ghosts_dir_;
  Tab tab_ = kAll;
  std::vector<Entry> entries_;
  size_t selected_ = 0;
  bool open_ = false;
};

struct WorldTrigger {
  enum Kind { kNone, kEvent, kStreetHub, kFestivalSite, kOutpost } kind = kNone;
  const race::CareerEvent* event = nullptr;
  const race::StreetHub* hub = nullptr;
  const race::FestivalSite* site = nullptr;
  bool in_range = false;
  double dist = 1e9;
  const freeroam::Outpost* outpost = nullptr;
};

// Selects the nearest unlocked world trigger within `max_dist` metres,
// preferring triggers whose `in_range` radius encloses `(x, z)`.
WorldTrigger select_world_trigger(const race::CareerCatalog& catalog, const race::CareerRecord& career,
                                  int collected_wristband, int popularity_rank, double x, double z,
                                  double max_dist = 140.0);

}  // namespace carview
