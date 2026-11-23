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
  int pr_stunts_completed = 0;
  int garage_cars = 0;
  int promo_cars_spotted = 0;
  double total_distance_m = 0.0;
  double top_speed_mps = 0.0;
  int64_t total_winnings = 0;
  int64_t max_skills_score = 0;
  double popularity_points = 0.0;
  int driver_level = 0;
  bool has_rally = false;
  int rally_stages_done = 0;
  int rally_victories = 0;
  int rally_level = 0;
  int club_medals_total = 0;
  int club_cars_with_medal = 0;

  static AchievementGameplayState from_drive_save(const drive::DriveSave& save, int roads_discovered_count = 0,
                                                  int race_encounters = 0);
};

struct AchievementEntry {
  uint16_t id = 0;
  uint32_t image_id = 0;
  uint16_t gamerscore = 0;
  AchievementCategory category = AchievementCategory::kCareer;
  AchievementAvailability availability = AchievementAvailability::kAvailable;
  std::string name;
  std::string description;
  std::string unachieved_description;
  bool unlocked = false;
  bool is_new = false;
  int64_t unlock_utc = 0;
  std::optional<AchievementProgress> progress;

  bool available() const { return unlocked || availability == AchievementAvailability::kAvailable; }
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
  // Initializes entries from loaded `spa` (falling back to structural IDs if
  // `spa` is empty so tests and no-DLC installs still function cleanly).
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
  void select_focus_index(int idx) {
    if (visible_.empty()) {
      focus_index_ = 0;
      return;
    }
    focus_index_ = std::clamp(idx, 0, int(visible_.size()) - 1);
  }
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

  // Renders the full F7 Achievements Screen when open.
  void build_screen_hud(ui::List& out, float w, float h) const;
  // Renders the free-roam tracked achievement chip and animated unlock toast.
  void build_hud_overlays(ui::List& out, float w, float h, bool show_track_chip) const;

 private:
  void rebuild_tabs();
  void rebuild_visible();
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
