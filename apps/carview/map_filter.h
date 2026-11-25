// The pause map's icon filter (docs/UI_SCENE_SCREENS.md, "The map"): the
// original's FILTER popup, LB on the map, is a list of 12 check rows, each
// an InGame string, all ticked by default:
//
//   All, New, Not Won, Rivals, Festival Events, Showcases, Star Showdowns,
//   Outposts, Street Race Hubs, Speed Cameras, Discount Signs, Barn Finds
//
// A toggles the row under the cursor; A on All unticks (or ticks) every row.
// An icon shows when its own row is ticked, or when it is new and New is
// ticked, or an event not won yet and Not Won is ticked, or a rival's and
// Rivals is ticked. The festival's facilities (Race Central, the workshop,
// the autoshow, the paint shop, the car club, the DLC centre) and race
// encounters have no row and always show.
#pragma once

#include <array>
#include <cstdint>

namespace carview {

enum class MapFilterRow : int {
  kAll = 0,
  kNew,
  kNotWon,
  kRivals,
  kFestivalEvents,
  kShowcases,
  kStarShowdowns,
  kOutposts,
  kStreetRaceHubs,
  kSpeedCameras,
  kDiscountSigns,
  kBarnFinds,
  kCount
};
inline constexpr int kMapFilterRows = int(MapFilterRow::kCount);

// The row's label: InGame `IDS_Filter_*`.
inline const char* map_filter_string_id(MapFilterRow r) {
  switch (r) {
    case MapFilterRow::kAll: return "IDS_Filter_All";
    case MapFilterRow::kNew: return "IDS_Filter_New";
    case MapFilterRow::kNotWon: return "IDS_Filter_NotWon";
    case MapFilterRow::kRivals: return "IDS_Filter_Rivals";
    case MapFilterRow::kFestivalEvents: return "IDS_Filter_FestivalEvents";
    case MapFilterRow::kShowcases: return "IDS_Filter_Showcases";
    case MapFilterRow::kStarShowdowns: return "IDS_Filter_NemesisRaces";
    case MapFilterRow::kOutposts: return "IDS_Filter_Outposts";
    case MapFilterRow::kStreetRaceHubs: return "IDS_Filter_StreetRaceHubs";
    case MapFilterRow::kSpeedCameras: return "IDS_Filter_SpeedCameras";
    case MapFilterRow::kDiscountSigns: return "IDS_Filter_Flyers";
    case MapFilterRow::kBarnFinds: return "IDS_Filter_BarnFinds";
    default: return "IDS_Filter_All";
  }
}
// The English text, for when the string tables are missing.
inline const char* map_filter_fallback(MapFilterRow r) {
  switch (r) {
    case MapFilterRow::kAll: return "All";
    case MapFilterRow::kNew: return "New";
    case MapFilterRow::kNotWon: return "Not Won";
    case MapFilterRow::kRivals: return "Rivals";
    case MapFilterRow::kFestivalEvents: return "Festival Events";
    case MapFilterRow::kShowcases: return "Showcases";
    case MapFilterRow::kStarShowdowns: return "Star Showdowns";
    case MapFilterRow::kOutposts: return "Outposts";
    case MapFilterRow::kStreetRaceHubs: return "Street Race Hubs";
    case MapFilterRow::kSpeedCameras: return "Speed Cameras";
    case MapFilterRow::kDiscountSigns: return "Discount Signs";
    case MapFilterRow::kBarnFinds: return "Barn Finds";
    default: return "All";
  }
}

// What a map icon is to the filter.
enum class MapCategory : int {
  kAlways = 0,     // facilities, race encounters: no row
  kFestivalEvent,  // career races
  kShowcase,       // exhibitions
  kStarShowdown,   // nemesis races
  kOutpost,        // Horizon Outposts
  kStreetRaceHub,
  kSpeedCamera,    // speed traps and zones
  kDiscountSign,   // flyers
  kBarnFind,       // barns and their rumour areas
};

inline MapFilterRow map_category_row(MapCategory c) {
  switch (c) {
    case MapCategory::kFestivalEvent: return MapFilterRow::kFestivalEvents;
    case MapCategory::kShowcase: return MapFilterRow::kShowcases;
    case MapCategory::kStarShowdown: return MapFilterRow::kStarShowdowns;
    case MapCategory::kOutpost: return MapFilterRow::kOutposts;
    case MapCategory::kStreetRaceHub: return MapFilterRow::kStreetRaceHubs;
    case MapCategory::kSpeedCamera: return MapFilterRow::kSpeedCameras;
    case MapCategory::kDiscountSign: return MapFilterRow::kDiscountSigns;
    case MapCategory::kBarnFind: return MapFilterRow::kBarnFinds;
    default: return MapFilterRow::kCount;
  }
}

struct MapFilterSet {
  std::array<bool, kMapFilterRows> ticked{true, true, true, true, true, true,
                                         true, true, true, true, true, true};

  bool is_ticked(MapFilterRow r) const { return int(r) >= 0 && int(r) < kMapFilterRows && ticked[size_t(r)]; }
  bool all_ticked() const {
    for (int i = 1; i < kMapFilterRows; ++i)
      if (!ticked[size_t(i)]) return false;
    return true;
  }
  // A on a row. All sets every row to its own new state; another row
  // leaves All ticked only while every row is.
  void toggle(MapFilterRow r) {
    if (int(r) < 0 || int(r) >= kMapFilterRows) return;
    if (r == MapFilterRow::kAll) {
      const bool to = !ticked[0];
      ticked.fill(to);
      return;
    }
    ticked[size_t(r)] = !ticked[size_t(r)];
    ticked[0] = all_ticked();
  }
  // Whether an icon of `c` shows.
  bool shows(MapCategory c, bool is_new = false, bool not_won = false, bool rival = false) const {
    if (c == MapCategory::kAlways) return true;
    if (is_ticked(map_category_row(c))) return true;
    if (is_new && is_ticked(MapFilterRow::kNew)) return true;
    if (not_won && is_ticked(MapFilterRow::kNotWon)) return true;
    if (rival && is_ticked(MapFilterRow::kRivals)) return true;
    return false;
  }
};

}  // namespace carview
