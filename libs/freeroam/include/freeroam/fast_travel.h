// Fast travel from the pause map: which places the map can travel to, and
// what an outpost costs.
//
// The rule (docs/UI_SCENE_SCREENS.md, "The map" / "Fast travel"):
// - Horizon Outposts cost `TotalCost` less a discount for the missions done
//   at that outpost: `Discount_<n>_Missions` for n done (n capped at 3), and
//   nothing once every mission there is done. The cost is per destination
//   outpost: only its own missions (speed stunt, photo shoot, PR stunt)
//   count. With the retail tunables and three missions an outpost: 10,000,
//   7,500, 5,000 and 0 Cr for 0, 1, 2 and 3 done.
// - The festival's places (Race Central, the workshop, the autoshow, the
//   paint shop, the car club, the DLC centre) are free.
// - Events, street race hubs, speed traps and zones, discount signs and barn
//   finds are not destinations.
// - The player pays only if they have the credits; the fee is taken when the
//   confirm dialog is accepted.
//
// The tunables are `Activities/Outposts/*` in gametunablesettings.zip's
// GlobalRegistry.xml (retail: 10000; 0, 0.25, 0.5, 1.0).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace freeroam {

struct OutpostCatalog;
struct OutpostProgress;

struct FastTravelTunables {
  int64_t total_cost = 10000;                     // Activities/Outposts/TotalCost
  std::array<float, 4> discount{0.0f, 0.25f, 0.5f, 1.0f};  // Discount_0..3_Missions
};

// Reads the Activities/Outposts values from GlobalRegistry.xml's text; keeps
// the defaults for any value it lacks. False when the text is not XML.
bool parse_fast_travel_tunables(std::string_view global_registry_xml, FastTravelTunables& out,
                                std::string* error = nullptr);
// From `<game_root>/media/gametunablesettings.zip`.
bool load_fast_travel_tunables(const std::filesystem::path& game_root, FastTravelTunables& out,
                               std::string* error = nullptr);

// The fee to travel to an outpost with `missions_done` of its
// `missions_total` missions done.
int64_t outpost_fast_travel_cost(const FastTravelTunables& t, int missions_total, int missions_done);
// The "Discount:" percentage the map and the post-mission card show: 25 per
// mission done, 100 once all are done.
int outpost_discount_percent(int missions_total, int missions_done);

// The outpost's missions and how many of them are done (0, 0 when unknown).
struct OutpostMissions {
  int total = 0;
  int done = 0;
};
OutpostMissions outpost_missions(const OutpostCatalog& catalog, const OutpostProgress& progress,
                                 std::string_view outpost_id);

// What a map place is to fast travel.
enum class FastTravelKind {
  kNone = 0,  // not a destination
  kOutpost,   // the outpost fee
  kFree,      // a festival place: free
};
// By the map tag of the place (MapProfile `activity_type`): gas_station,
// racecentral, workshop, autoshow, paintshop, carclub, dlccenter.
FastTravelKind fast_travel_kind(std::string_view map_tag);

// The quoted cost and whether the player can pay it.
struct FastTravelQuote {
  FastTravelKind kind = FastTravelKind::kNone;
  int64_t cost = 0;
  bool affordable = false;
  bool eligible() const { return kind != FastTravelKind::kNone; }
};
FastTravelQuote quote_fast_travel(const FastTravelTunables& t, std::string_view map_tag, int missions_total,
                                  int missions_done, int64_t credits);

}  // namespace freeroam
