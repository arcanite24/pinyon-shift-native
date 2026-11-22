// Race Central, Festival Events, and Street Race Hub browser.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "garage/garage.h"
#include "race/career.h"
#include "race/rewards.h"

namespace carview {

class EventsPage {
 public:
  enum Tab { kAll = 0, kFestival = 1, kStreet = 2, kShowcase = 3, kShowdown = 4, kTabCount = 5 };

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
  };

  // Opens the career events browser (hub_filter == 0: all events / Race Central;
  // hub_filter > 0: only Street Races belonging to that Street Hub).
  void open(const race::CareerCatalog& catalog, const race::CareerRecord& career, int collected_wristband,
            int popularity_rank, int64_t current_car_id, int current_class_id,
            const std::vector<garage::ClassBand>& bands, int hub_filter = 0);
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  void move(int by);
  void switch_tab(int by);
  const Entry* selected_entry() const;
  int hub_filter() const { return hub_filter_; }

  void fill(std::string& title, std::vector<std::string>& lines, int& selected, std::string& detail_title,
            std::vector<std::string>& detail_lines) const;

 private:
  void rebuild();

  const race::CareerCatalog* catalog_ = nullptr;
  const race::CareerRecord* career_ = nullptr;
  int collected_wristband_ = 0;
  int popularity_rank_ = 250;
  int64_t car_id_ = 0;
  int car_class_id_ = 0;
  std::vector<std::string> class_names_;
  int hub_filter_ = 0;
  Tab tab_ = kAll;
  std::vector<Entry> entries_;
  size_t selected_ = 0;
  bool open_ = false;
};

struct WorldTrigger {
  enum Kind { kNone, kEvent, kStreetHub, kFestivalSite } kind = kNone;
  const race::CareerEvent* event = nullptr;
  const race::StreetHub* hub = nullptr;
  const race::FestivalSite* site = nullptr;
  bool in_range = false;
  double dist = 1e9;
};

// Selects the nearest unlocked world trigger within `max_dist` metres,
// preferring triggers whose `in_range` radius encloses `(x, z)`.
WorldTrigger select_world_trigger(const race::CareerCatalog& catalog, const race::CareerRecord& career,
                                  int collected_wristband, int popularity_rank, double x, double z,
                                  double max_dist = 140.0);

}  // namespace carview
