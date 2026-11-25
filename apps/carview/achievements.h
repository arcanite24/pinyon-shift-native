// Achievements system, live unlock evaluator, persistence, HUD tracker chip,
// animated unlock toast, and Pinyon Shift-style F7 Achievements screen (Slice 10).
// Names, descriptions, gamerscore, and 64x64 icons are read from the player's
// own Xbox 360 spa.bin packages via fh1::SpaDatabase.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "drive/save.h"
#include "fh1data/spa.h"
#include "ui.h"

namespace race {
struct CareerCatalog;
}

namespace carview {

enum class AchievementCategory : uint8_t {
  kCareer = 0,
  kRacing,
  kFame,
  kExplore,
  kFestival,
  kOnline,
  kRally,
  kClub,
  kAll,  // pseudo-category for the ALL tab
};

const char* achievement_category_label(AchievementCategory c);

enum class AchievementAvailability : uint8_t {
  kAvailable = 0,
  kNeedsOnline,
  kNeedsRivals,
};

enum class AchievementSort : uint8_t {
  kTitleOrder = 0,
  kNearest,
  kLockedFirst,
  kRecent,
  kValue,
  kCount,
};

const char* achievement_sort_label(AchievementSort s);
// Clean ASCII badge text ("[UNLOCKED]" / "[LOCKED]" / "[SECRET]") — zero emojis.
const char* achievement_status_badge_text(bool unlocked, bool secret);

struct AchievementProgress {
  uint32_t current = 0;
  uint32_t target = 0;
  float fraction() const {
    return target > 0 ? float(std::min(current, target)) / float(target) : 0.0f;
  }
};

struct WristbandTierInfo {
  const char* name;
  const char* texture_key;  // Horizon.zip member (without .xds)
  uint32_t colour;
  float threshold;          // fraction of offline available gamerscore
};

// Returns -1 below Yellow (5%), 0..6 for Yellow..Gold, 7 for Platinum (100%).
int achievement_completion_tier(uint32_t earned_g, uint32_t available_g);
const WristbandTierInfo& wristband_tier_info(int tier);

// Snapshot of gameplay & career counters used for live unlock & progress checks.
struct AchievementGameplayState {
  bool career_started = false;
  bool finale_complete = false;
  int wristbands_earned = 0;       // 0..7
  int num_races = 0;
  int num_victories = 0;
  int num_podiums = 0;
  int street_races_won = 0;
  int festival_races_won = 0;
  int race_encounters_won = 0;
  int discount_signs_found = 0;
  int barns_found = 0;
  int roads_discovered = 0;
  int outposts_discovered = 0;
  int total_outposts = 10;         // Colorado has 10 regional Horizon Outposts
  int pr_stunts_completed = 0;
  int garage_cars = 0;
  int promo_cars_spotted = 0;
  double total_distance_m = 0.0;
  double top_speed_mps = 0.0;
  int64_t total_winnings = 0;
  int64_t max_skills_score = 0;
  double popularity_points = 0.0;
  int popularity_rank = 250;       // 250 (starting) down to 1 (#1 Popularity Rank)
  int driver_level = 0;
  bool has_rally = false;
  int rally_stages_done = 0;
  int rally_victories = 0;
  int rally_level = 0;
  int club_medals_total = 0;
  int club_cars_with_medal = 0;
  int club_ferrari_cars = 0;
  int club_million_cr_cars = 0;
  int club_shelby_cars = 0;
  int club_lamborghini_cars = 0;
  int club_nissan_cars = 0;
  int club_bmw_cars = 0;
  int club_british_cars = 0;
  int club_ford_cars = 0;
  // SPA-rule inputs added for the 1:1 conditions (PROG-16, V4-19). A total of
  // 0 means "unknown": the "win every ..." rules never unlock on it.
  int autoshow_purchases = 0;          // 156
  int dirt_races_won = 0;              // 159
  uint32_t festival_venues = 0;        // 166 (drive::AchievementSignals::Venue bits)
  int paint_jobs_saved = 0;            // 167
  bool loyalty_cars_received = false;  // 169
  int hard_races_won = 0;              // 183
  int speed_traps_set = 0;             // 164
  int speed_zones_set = 0;             // 164
  int rivals_beaten = 0;               // 165
  int festival_races_total = 0;        // 184, 188 (distinct Festival Races in the catalog)
  int street_races_total = 0;          // 185, 188
  int showcases_won = 0, showcases_total = 0;              // 186, 188
  int star_showdowns_won = 0, star_showdowns_total = 0;    // 187, 188
  int pr_stunts_total = 30;            // 189: Colorado's 10 outposts hold 30 PR stunts
  int sponsor_challenges_completed = 0;  // 190, 191
  int discount_signs_total = 100;      // 194
  int roads_total = 216;               // 195: Colorado's named roads
  int rally_cars_obtained = 0;         // 208
  bool rally_champion = false;         // 209
  int rally_hard_manual_stage_wins = 0;  // 210
  double rally_air_m = 0.0;            // 211
  int rally_rivals_events = 0;         // 212
  int rally_flawless_stage_wins = 0;   // 214
  int rally_cars_purchased = 0;        // 215

  static AchievementGameplayState from_drive_save(const drive::DriveSave& save, int roads_discovered_count = 0,
                                                  int race_encounters = 0,
                                                  const race::CareerCatalog* catalog = nullptr,
                                                  int popularity_rank = 0, int total_outposts = 10);
};

// How the engine treats one FH1 achievement id (155-225).
enum class AchievementRule : uint8_t {
  kNone = 0,     // not an FH1 achievement id (163, out of range)
  kOffline,      // evaluated by eval rules against AchievementGameplayState
  kOnlineOnly,   // Xbox Live only: never unlocks offline
};
AchievementRule achievement_rule(uint16_t id);
// The SPA's English name and gamerscore, carried for runs without a spa.bin
// (nullptr / 0 for ids that are not FH1 achievements).
const char* achievement_fallback_name(uint16_t id);
uint16_t achievement_fallback_gamerscore(uint16_t id);

struct AchievementEntry {
  uint16_t id = 0;
  uint32_t image_id = 0;
  uint16_t gamerscore = 0;
  AchievementCategory category = AchievementCategory::kCareer;
  AchievementAvailability availability = AchievementAvailability::kAvailable;
  std::string name;
  std::string description;
  std::string unachieved_description;
  bool is_secret = false;
  bool unlocked = false;
  bool is_new = false;
  int64_t unlock_utc = 0;
  std::optional<AchievementProgress> progress;

  bool available() const { return unlocked || availability == AchievementAvailability::kAvailable; }
  bool is_masked_secret() const { return is_secret && !unlocked; }
  std::string display_name() const { return is_masked_secret() ? "SECRET ACHIEVEMENT" : name; }
  std::string display_description() const {
    if (is_masked_secret()) return "CONTINUE PLAYING TO UNLOCK THIS SECRET ACHIEVEMENT.";
    return (!unlocked && !unachieved_description.empty()) ? unachieved_description : description;
  }
};

struct AchievementToast {
  uint16_t id = 0;
  uint32_t image_id = 0;
  uint16_t gamerscore = 0;
  std::string name;
  std::string subtitle;
  bool large_gold = false;  // >= 50G or category completion
  double elapsed = 0.0;
  double duration = 5.0;
};

class AchievementsSystem {
 public:
  // Initializes entries from loaded `spa`: names, descriptions and gamerscore
  // come from it. With an empty `spa` (no spa.bin under the game or state
  // root) it falls back to the base 50 with the SPA's names and gamerscores.
  void init(const fh1::SpaDatabase& spa);
  void set_unlock_sound_callback(std::function<void()> cb) { on_unlock_sound_ = std::move(cb); }

  // Persists unlocked IDs, UTC unlock timestamps, seen set, and tracked ID.
  bool load_state(const std::filesystem::path& path);
  bool save_state(const std::filesystem::path& path) const;

  // Evaluates progress counters and live unlock rules against `state`.
  // Returns the number of newly unlocked achievements this call.
  int evaluate(const AchievementGameplayState& state, bool trigger_toasts = true);
  // Manually unlocks one achievement by ID (for scripted demos & tests).
  bool unlock(uint16_t id, int64_t utc_now = 0, bool trigger_toast = true);

  // Screen & interaction controls (F7 Achievements Screen).
  void open_screen();
  void close_screen();
  bool screen_open() const { return screen_open_; }
  void switch_tab(int delta);
  void select_tab(AchievementCategory cat);
  void move_focus(int dx, int dy);
  int focus_index() const { return focus_index_; }
  int scroll_row() const { return scroll_row_; }
  int visible_count() const { return int(visible_.size()); }
  void select_focus_index(int idx);
  void cycle_sort();
  bool toggle_track_focused();
  void set_tracked_id(uint16_t id);
  uint16_t tracked_id() const { return tracked_id_; }

  void update(double dt);

  // Summary queries.
  uint32_t earned_gamerscore() const;
  uint32_t total_gamerscore() const;
  uint32_t available_gamerscore() const;
  int unlocked_count() const;
  int total_count() const { return int(entries_.size()); }
  int completion_tier() const { return achievement_completion_tier(earned_gamerscore(), available_gamerscore()); }
  const AchievementEntry* find(uint16_t id) const;
  const AchievementEntry* focused_entry() const;
  const AchievementToast* active_toast() const { return toasts_.empty() ? nullptr : &toasts_.front(); }
  const std::vector<AchievementCategory>& tabs() const { return tabs_; }
  AchievementCategory active_tab() const { return tabs_.empty() ? AchievementCategory::kAll : tabs_[tab_idx_]; }
  AchievementSort sort_mode() const { return sort_; }

  // Detects whether an active Race HUD or Campaign Objective banner already
  // occupies the top-left HUD corner in `list`.
  static bool top_left_hud_occupied(const ui::List& list, float w, float h, float* max_bottom_y = nullptr);

  // Renders the full F7 Achievements Screen when open.
  void build_screen_hud(ui::List& out, float w, float h) const;
  // Renders the free-roam tracked achievement chip and animated unlock toast.
  // Automatically hides or smoothly offsets the tracked chip whenever an active
  // Race HUD or Campaign Objective banner occupies the same top-left corner.
  void build_hud_overlays(ui::List& out, float w, float h, bool show_track_chip,
                          bool top_left_occupied = false, bool hide_when_occupied = false) const;

 private:
  void rebuild_tabs();
  void rebuild_visible();
  void clamp_scroll();
  bool ensure_icon_uv(uint32_t image_id, bool grey, std::array<float, 4>& uv) const;

  std::vector<AchievementEntry> entries_;
  std::map<uint32_t, std::vector<uint8_t>> raw_icons_;
  mutable std::set<uint32_t> uploaded_icons_;

  std::vector<AchievementCategory> tabs_;
  size_t tab_idx_ = 0;
  AchievementSort sort_ = AchievementSort::kTitleOrder;
  std::vector<size_t> almost_there_;  // indices into entries_ (top 3 nearest on ALL tab)
  std::vector<size_t> visible_;       // filtered & sorted indices into entries_
  int focus_index_ = 0;
  int scroll_row_ = 0;

  uint16_t tracked_id_ = 0;
  bool screen_open_ = false;
  double screen_time_ = 0.0;
  double global_time_ = 0.0;
  std::vector<AchievementToast> toasts_;
  std::function<void()> on_unlock_sound_;
};

}  // namespace carview
