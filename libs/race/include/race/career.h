// Career progression catalog & rules: wristbands, event unlocks, Race Central
// collection, car restriction eligibility, Street Race Hubs, and Festival
// world triggers. All data is read at runtime from the player's extracted disc
// (gamedb.slt, gamemodes.zip, GameObjs.xml).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "data/event_restrictions.h"
#include "race/rewards.h"

namespace cars {
class PartsDb;
}

namespace race {

// Starter career car (SPEC-GARAGE-UPGRADES: 1995 VW Corrado VR6, red).
inline constexpr int64_t kStarterCarId = 309;
inline constexpr const char* kStarterCarMedia = "VW_Corrado_95";
inline constexpr int kStarterCarColour = 2;

struct WristbandTier {
  int id = 0;               // 0..6 (Yellow..Gold)
  int xp = 0;               // CareerWristbandLevels.XP
  std::string name_ref;     // `_&...` string-table ref
  std::string colour_key;   // "Yellow", "Green", "Blue", "Pink", "Orange", "Purple", "Gold"
  uint32_t argb = 0;        // CareerWristbandLevels.Color
  int64_t reward_car_id = 0;  // Rewards_Wristband.RewardCarId (0: none)
};

struct CareerEvent {
  int64_t event_id = 0;
  std::string horizon_id;
  std::string name_ref;              // Events.Name (`_&...`)
  std::string restriction_desc_ref;  // Events.RestrictionDescription (`_&...`)
  int level = 0;                     // Events.Level (-1 qualifier, 0..6 Yellow..Gold)
  int style = 0;                     // 1 Festival, 2 Street, 3 Showcase, 5 Nemesis, 6 Headline
  int career_type = 0;               // 1/2 Festival, 4 Street, 7/8 Showcase, 11 Nemesis, 12 Headline
  int unlock_points_req = 0;         // Events.UnlockPointsReq
  int popularity_rank_req = 0;       // Events.PopularityPointsReq (250..1 for Showcases)
  int hub_id = 0;                    // Events.HubId (1..3 for Street Races)
  int target_class = 0;              // Events.TargetClass (CarClasses.Id)
  double cash_prize = 0;
  int laps = 1, route_id = 0, ribbon = 0, opponents = 0;
  int64_t player_car_id = 0;         // Events.PlayerCarId (loaner car for Showcases)
  int64_t prize_car_id = 0;          // Rewards_EventPrizes.RewardCarId
  int prize_car_colour = 0;          // Rewards_EventPrizes.RewardCarColorSequence
  // Rewards_EventPrizes.RewardCarUpgradeClass (CarClasses.Id the prize car
  // arrives tuned to; -1: none), LiveryLevel and TuningLevel.
  int prize_car_upgrade_class = -1;
  int prize_car_livery_level = 0;
  int prize_car_tuning_level = 0;
  int bonus_id = 0;                  // Events.BonusId (EventBonuses.Id; display only)
  int64_t prereq_event_id = 0;       // Rewards_EventUnlock prerequisite EventId (must win)
  shift::data::EventRestrictions restrictions;
  int required_engine_placement = 0;  // 0 = any, 1 = Front, 2 = Mid, 3 = Rear
  int required_cylinders = 0;         // 0 = any, else required cylinder count
  bool has_tod_start = false;
  double tod_start_sec = 0.0;         // Events.TimeOfDayStart (seconds since midnight [0..86400))
  double tod_end_sec = 0.0;           // Events.TimeOfDayEnd (seconds since midnight [0..86400))
  // The event ticket's lines (H_EVENT_SUMMARY): its race track's name and
  // surface, and the ticket texture's number.
  std::string track_name_ref;  // Tracks.DisplayName (`_&...`)
  int track_surface = 0;       // Tracks.SurfaceTypeRibbonConfiguration
  std::string track_dev_name;  // Tracks.DevName: the track's race cutscenes (FESTIVAL_FEST_001)
  std::string ticket;          // Events.EventTicket ("043": EventTickets/<style>/043)
  bool has_trigger = false;
  std::array<double, 3> trigger_pos{};
  double trigger_radius = 25.0;
};

// One EventBonuses row. BonusTypeId groups by three: 0-2 Affinity, 3-5
// Driver Level, 6-8 Credits, 9 Mystery; within a group flat, percent and
// random. The original only labels the event card with it: no payout code
// reads the table (every career event's BonusId is 1, "Affinity +500").
struct EventBonus {
  enum class Kind { kAffinity, kDriverLevel, kCredits, kMystery };
  enum class Form { kFlat, kPercent, kRandom };
  int id = 0;
  int type_id = 0;  // EventBonuses.BonusTypeId
  int amount = 0;   // EventBonuses.Amount
  Kind kind() const {
    return type_id >= 9 ? Kind::kMystery : type_id >= 6 ? Kind::kCredits : type_id >= 3 ? Kind::kDriverLevel
                                                                                        : Kind::kAffinity;
  }
  Form form() const { return type_id >= 9 ? Form::kRandom : Form(type_id % 3); }
};

struct StreetHub {
  int id = 0;  // 1..3
  std::string name_ref;
  int unlock_points_req = 0;
  std::vector<int64_t> initial_events;
  bool has_trigger = false;
  std::array<double, 3> trigger_pos{};
  double trigger_radius = 24.0;
  // The activity's RadioDJRequests special lines (StreetRaceOwnerHub<n>).
  std::vector<std::string> radio_specials;
};

struct FestivalSite {
  enum Kind { kRaceCentral, kAutoshow, kWorkshop, kPaintShop };
  Kind kind = kRaceCentral;
  std::string name;     // display fallback ("RACE CENTRAL", etc.)
  std::string map_tag;  // MapProfile icon key ("racecentral", "autoshow", "workshop", "paintshop")
  std::array<double, 3> pos{};
  double radius = 24.0;
};

struct CarSpec {
  int64_t car_id = 0;
  std::string media_name;
  int base_cost = 0;
  double stock_pi = 0;
  int stock_class = 0;
  int engine_placement = 0;  // Data_Car.EnginePlacementID (1=Front, 2=Mid, 3=Rear)
  int cylinder_count = 0;    // resolved cylinder count from Data_Car.CylinderID -> List_Cylinders.Number
  shift::data::CarProperties props;
};

struct EligibilityResult {
  bool eligible = true;
  bool uses_loaner = false;
  int64_t loaner_car_id = 0;
  std::string reason;
};

struct CareerCatalog {
  std::vector<WristbandTier> wristbands;  // sorted by ascending XP
  std::vector<CareerEvent> events;        // playable career events (ribbon >= 0)
  std::vector<StreetHub> hubs;            // Street Race Hubs (1..3)
  std::map<int, EventBonus> event_bonuses; // EventBonuses by Id (labels only)
  std::vector<FestivalSite> sites;        // Festival locations (Race Central, Autoshow, Dak's Garage, Paint Shop)
  std::map<int64_t, CarSpec> cars;        // Data_Car by Id
  std::map<std::string, int64_t> car_id_by_media;  // lower-case MediaName -> Data_Car.Id
  std::map<int, std::string> make_names;             // List_CarMake.ID -> display name
  std::map<int, std::string> country_names;          // List_Country.CountryID -> name
  std::map<int, std::string> region_names;           // List_Region.Id -> name
  std::map<int, std::string> body_family_names;      // List_FamilyBody.Id -> name
  std::map<int, std::string> model_family_names;     // List_FamilyModel.Id -> name
  std::map<int, std::string> engine_placement_names; // List_EnginePlacement.ID -> name

  const CareerEvent* find_event(const std::string& horizon_id) const;
  const CareerEvent* find_event_by_id(int64_t event_id) const;
  const StreetHub* find_hub(int hub_id) const;
  const CarSpec* find_car(int64_t car_id) const;
  const CarSpec* find_car_by_media(const std::string& media) const;
  const FestivalSite* find_site(FestivalSite::Kind kind) const;
  const EventBonus* find_event_bonus(int bonus_id) const;

  // Highest wristband index (0..size-1) whose XP threshold <= wristband_points; -1 if none.
  int earned_wristband(int wristband_points) const;
  // Next wristband waiting to be collected at Race Central, or -1 if none pending.
  int pending_wristband(int wristband_points, int collected_wristband) const;

  // Event & hub unlock predicates.
  bool is_hub_unlocked(const StreetHub& hub, const CareerRecord& rec, int collected_wristband) const;
  int hub_events_completed(int hub_id, const CareerRecord& rec) const;
  // Races of the previous hub a hub needs: 0 (the original has no such chain).
  int hub_unlock_requirement(int hub_id) const;
  bool is_event_unlocked(const CareerEvent& ev, const CareerRecord& rec, int collected_wristband,
                         int popularity_rank) const;
  bool is_event_completed(const CareerEvent& ev, const CareerRecord& rec) const;
  int best_place(const CareerEvent& ev, const CareerRecord& rec) const;

  // Car eligibility check against TargetClass + EventRestrictions bucket 0.
  // When `any_car_override` is true (F10 Trainer), bypasses class & restriction checks.
  // When `car_rally_upgraded` is true, marks the active garage car as having a Rally kit fitted.
  EligibilityResult check_eligibility(const CareerEvent& ev, int64_t car_id, int car_class_id,
                                      const std::vector<std::string>& class_names,
                                      bool any_car_override = false,
                                      bool car_rally_upgraded = false) const;

  // Map icon tag for an event ("race", "exhibition", "streetrace", "nemesisrace").
  static const char* event_map_tag(const CareerEvent& ev);
  // Display category label ("FESTIVAL RACE", "STREET RACE", "SHOWCASE", "STAR SHOWDOWN", "HORIZON FINALE").
  static const char* event_style_label(const CareerEvent& ev);
};

// Loads the full career catalog from the merged v4+DLC database when
// `db` or `state_root` is provided (falling back to `<game_root>/media/db/gamedb.slt`),
// `gamemodes.zip`, and `tracks/colorado/Ribbon_00/GameObjs.xml`.
bool load_career_catalog(const std::filesystem::path& game_root, CareerCatalog& out, std::string* error);
bool load_career_catalog(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                         const cars::PartsDb* db, CareerCatalog& out, std::string* error);

}  // namespace race
