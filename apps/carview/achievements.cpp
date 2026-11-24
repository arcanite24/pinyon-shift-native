#include "achievements.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#include <stb_image.h>

#include "game_hud.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

using ui::Align;
using ui::Font;
using ui::rgba;
using ui::TextStyle;

struct MetaSpec {
  uint16_t id;
  uint16_t default_g;
  AchievementCategory category;
  AchievementAvailability availability;
};

using C = AchievementCategory;
using A = AchievementAvailability;

// Clean-room structural table of FH1's 70 achievement IDs (155-205 base,
// 206-215 Rally, 216-225 1000 Club; 163 unused). Names and descriptions always
// come from the player's own spa.bin via fh1::SpaDatabase.
constexpr MetaSpec kFh1Meta[] = {
    {155, 10, C::kCareer, A::kAvailable},     {156, 10, C::kFestival, A::kAvailable},
    {157, 15, C::kCareer, A::kAvailable},     {158, 15, C::kCareer, A::kAvailable},
    {159, 10, C::kRacing, A::kAvailable},     {160, 20, C::kRacing, A::kAvailable},
    {161, 25, C::kRacing, A::kAvailable},     {162, 15, C::kRacing, A::kAvailable},
    {164, 25, C::kExplore, A::kAvailable},    {165, 10, C::kRacing, A::kNeedsRivals},
    {166, 10, C::kFestival, A::kAvailable},   {167, 10, C::kFestival, A::kAvailable},
    {168, 10, C::kFestival, A::kNeedsOnline}, {169, 25, C::kFestival, A::kAvailable},
    {170, 10, C::kFame, A::kAvailable},       {171, 15, C::kFame, A::kAvailable},
    {172, 20, C::kFame, A::kAvailable},       {173, 25, C::kFame, A::kAvailable},
    {174, 30, C::kFame, A::kAvailable},       {175, 15, C::kCareer, A::kAvailable},
    {176, 20, C::kCareer, A::kAvailable},     {177, 25, C::kCareer, A::kAvailable},
    {178, 30, C::kCareer, A::kAvailable},     {179, 35, C::kCareer, A::kAvailable},
    {180, 40, C::kCareer, A::kAvailable},     {181, 50, C::kCareer, A::kAvailable},
    {182, 50, C::kCareer, A::kAvailable},     {183, 15, C::kRacing, A::kAvailable},
    {184, 20, C::kRacing, A::kAvailable},     {185, 25, C::kRacing, A::kAvailable},
    {186, 20, C::kRacing, A::kAvailable},     {187, 30, C::kRacing, A::kAvailable},
    {188, 50, C::kRacing, A::kAvailable},     {189, 15, C::kFame, A::kAvailable},
    {190, 20, C::kFame, A::kAvailable},       {191, 25, C::kFame, A::kAvailable},
    {192, 10, C::kExplore, A::kAvailable},    {193, 15, C::kExplore, A::kAvailable},
    {194, 30, C::kExplore, A::kAvailable},    {195, 25, C::kExplore, A::kAvailable},
    {196, 10, C::kOnline, A::kNeedsOnline},   {197, 10, C::kOnline, A::kNeedsOnline},
    {198, 15, C::kOnline, A::kNeedsOnline},   {199, 15, C::kOnline, A::kNeedsOnline},
    {200, 15, C::kOnline, A::kNeedsOnline},   {201, 20, C::kOnline, A::kNeedsOnline},
    {202, 10, C::kOnline, A::kNeedsOnline},   {203, 10, C::kOnline, A::kNeedsOnline},
    {204, 15, C::kOnline, A::kNeedsOnline},   {205, 20, C::kOnline, A::kNeedsOnline},
    // Horizon Rally (206-215)
    {206, 20, C::kRally, A::kAvailable},      {207, 25, C::kRally, A::kAvailable},
    {208, 25, C::kRally, A::kAvailable},      {209, 25, C::kRally, A::kAvailable},
    {210, 25, C::kRally, A::kAvailable},      {211, 25, C::kRally, A::kAvailable},
    {212, 20, C::kRally, A::kNeedsRivals},    {213, 25, C::kRally, A::kNeedsOnline},
    {214, 30, C::kRally, A::kAvailable},      {215, 30, C::kRally, A::kAvailable},
    // 1000 Club (216-225)
    {216, 15, C::kClub, A::kAvailable},       {217, 20, C::kClub, A::kAvailable},
    {218, 25, C::kClub, A::kAvailable},       {219, 25, C::kClub, A::kAvailable},
    {220, 25, C::kClub, A::kAvailable},       {221, 25, C::kClub, A::kAvailable},
    {222, 25, C::kClub, A::kAvailable},       {223, 30, C::kClub, A::kAvailable},
    {224, 30, C::kClub, A::kAvailable},       {225, 30, C::kClub, A::kAvailable},
};

const MetaSpec* find_meta(uint16_t id) {
  for (const auto& m : kFh1Meta)
    if (m.id == id) return &m;
  return nullptr;
}

constexpr WristbandTierInfo kTiers[9] = {
    {"UNRANKED", "", rgba(155, 162, 178), 0.0f},
    {"YELLOW", "WRISTBANDS/YELLOWXL", rgba(255, 222, 0), 0.05f},
    {"GREEN", "WRISTBANDS/GREENXL", rgba(90, 210, 40), 0.15f},
    {"BLUE", "WRISTBANDS/BLUEXL", rgba(30, 150, 255), 0.30f},
    {"PINK", "WRISTBANDS/PINKXL", rgba(255, 40, 170), 0.45f},
    {"ORANGE", "WRISTBANDS/ORANGEXL", rgba(255, 110, 20), 0.60f},
    {"PURPLE", "WRISTBANDS/PURPLEXL", rgba(160, 82, 240), 0.75f},
    {"GOLD", "WRISTBANDS/GOLDXL", rgba(244, 194, 68), 0.90f},
    {"PLATINUM", "WRISTBANDS/PLATINUM", rgba(220, 232, 250), 1.00f},
};

float ease_out3(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const float u = 1.0f - t;
  return 1.0f - u * u * u;
}

int64_t now_utc() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string format_utc_date(int64_t utc) {
  if (utc <= 0) return {};
  const std::time_t tt = std::time_t(utc);
  std::tm tm_buf{};
#ifdef _WIN32
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  static constexpr const char* kMonths[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                              "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  char buf[32];
  const int m = std::clamp(tm_buf.tm_mon, 0, 11);
  std::snprintf(buf, sizeof buf, "%02d %s %04d", tm_buf.tm_mday, kMonths[m], tm_buf.tm_year + 1900);
  return buf;
}

std::vector<std::string> wrap_lines(std::string_view text, const TextStyle& style, float max_w, int max_lines) {
  std::vector<std::string> lines;
  std::string cur;
  size_t pos = 0;
  while (pos < text.size() && int(lines.size()) < max_lines) {
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size()) break;
    size_t next = text.find(' ', pos);
    if (next == std::string_view::npos) next = text.size();
    const std::string word(text.substr(pos, next - pos));
    pos = next;
    const std::string candidate = cur.empty() ? word : (cur + " " + word);
    if (!cur.empty() && ui::List::measure(candidate, style) > max_w) {
      lines.push_back(cur);
      cur = word;
    } else {
      cur = candidate;
    }
  }
  if (!cur.empty() && int(lines.size()) < max_lines) lines.push_back(cur);
  return lines;
}

// Evaluates known progress & unlock conditions against gameplay state.
std::optional<AchievementProgress> eval_condition(uint16_t id, const AchievementGameplayState& s, bool& should_unlock) {
  should_unlock = false;
  auto counter = [&](int cur, int target) -> std::optional<AchievementProgress> {
    const uint32_t c = uint32_t(std::max(0, cur));
    const uint32_t t = uint32_t(std::max(1, target));
    should_unlock = (c >= t);
    return AchievementProgress{c, t};
  };
  switch (id) {
    case 155: should_unlock = s.career_started || s.num_races > 0 || s.wristbands_earned > 0; return std::nullopt;
    case 156: return counter(s.garage_cars, 1);
    case 157: return counter(s.num_races, 1);
    case 158: return counter(s.num_victories, 5);
    case 159: return counter(s.num_victories, 1);
    case 160: return counter(s.num_podiums, 10);
    case 161: return counter(s.num_victories, 25);
    case 162: return counter(s.race_encounters_won + s.street_races_won, 10);
    case 164: return counter(s.roads_discovered, 216);
    case 166: return counter(s.garage_cars, 10);
    case 167: return counter(s.promo_cars_spotted, 1);
    case 169: return counter(int(std::min<int64_t>(s.total_winnings / 1000, 1000000)), 1000);
    case 170: return counter(int(s.popularity_points / 1000.0), 10);
    case 171: return counter(int(s.popularity_points / 1000.0), 50);
    case 172: return counter(int(s.popularity_points / 1000.0), 100);
    case 173: return counter(int(s.popularity_points / 1000.0), 250);
    case 174: return counter(int(s.popularity_points / 1000.0), 500);
    case 175: return counter(s.wristbands_earned, 1);
    case 176: return counter(s.wristbands_earned, 2);
    case 177: return counter(s.wristbands_earned, 3);
    case 178: return counter(s.wristbands_earned, 4);
    case 179: return counter(s.wristbands_earned, 5);
    case 180: return counter(s.wristbands_earned, 6);
    case 181: return counter(s.wristbands_earned, 7);
    case 182: return counter(s.wristbands_earned, 7);
    case 183: return counter(s.festival_races_won, 10);
    case 184: return counter(s.street_races_won, 10);
    case 185: return counter(s.num_victories, 50);
    case 186: should_unlock = s.top_speed_mps >= 89.4; return counter(int(s.top_speed_mps * 2.23694), 200);
    case 187: return counter(s.outposts_discovered, 6);
    case 188: return counter(s.num_victories, 70);
    case 189: return counter(s.pr_stunts_completed, 1);
    case 190: return counter(s.pr_stunts_completed, 10);
    case 191: return counter(int(std::min<int64_t>(s.max_skills_score, 1000000)), 50000);
    case 192: return counter(s.barns_found, 1);
    case 193: return counter(s.discount_signs_found, 10);
    case 194: return counter(s.discount_signs_found, 100);
    case 195: return counter(s.barns_found, 9);
    // Horizon Rally (206-215)
    case 206: return counter(s.rally_stages_done, 1);
    case 207: return counter(s.rally_victories, 1);
    case 208: return counter(s.rally_stages_done, 10);
    case 209: return counter(s.rally_victories, 3);
    case 210: return counter(s.rally_victories, 7);
    case 211: return counter(s.rally_level, 5);
    case 214: return counter(s.rally_stages_done, 28);
    case 215: return counter(s.rally_level, 10);
    // 1000 Club (216-225)
    case 216: return counter(s.club_medals_total, 1);
    case 217: return counter(s.club_ferrari_cars, 14);
    case 218: return counter(s.club_million_cr_cars, 14);
    case 219: return counter(s.club_shelby_cars, 3);
    case 220: return counter(s.club_lamborghini_cars, 8);
    case 221: return counter(s.club_nissan_cars, 7);
    case 222: return counter(s.club_bmw_cars, 10);
    case 223: return counter(s.club_british_cars, 20);
    case 224: return counter(s.club_ford_cars, 13);
    case 225: return counter(s.club_cars_with_medal, 146);
    default: return std::nullopt;
  }
}

uint32_t value_stripe_colour(uint16_t g) {
  if (g >= 30) return rgba(244, 194, 68);   // Gold
  if (g >= 15) return rgba(196, 206, 222);  // Silver
  return rgba(206, 134, 78);                // Bronze
}

}  // namespace

const char* achievement_category_label(AchievementCategory c) {
  switch (c) {
    case AchievementCategory::kAll: return "ALL";
    case AchievementCategory::kCareer: return "CAREER";
    case AchievementCategory::kRacing: return "RACING";
    case AchievementCategory::kFame: return "FAME";
    case AchievementCategory::kExplore: return "EXPLORE";
    case AchievementCategory::kFestival: return "FESTIVAL";
    case AchievementCategory::kOnline: return "ONLINE";
    case AchievementCategory::kRally: return "RALLY";
    case AchievementCategory::kClub: return "1000 CLUB";
    default: return "ALL";
  }
}

const char* achievement_sort_label(AchievementSort s) {
  switch (s) {
    case AchievementSort::kTitleOrder: return "TITLE ORDER";
    case AchievementSort::kNearest: return "NEAREST";
    case AchievementSort::kLockedFirst: return "LOCKED FIRST";
    case AchievementSort::kRecent: return "RECENT";
    case AchievementSort::kValue: return "VALUE (G)";
    default: return "TITLE ORDER";
  }
}

int achievement_completion_tier(uint32_t earned_g, uint32_t available_g) {
  if (available_g == 0) return -1;
  if (earned_g >= available_g) return 7;
  const float share = float(earned_g) / float(available_g);
  int tier = -1;
  for (int i = 0; i < 7; ++i) {
    if (share >= kTiers[i + 1].threshold) tier = i;
  }
  return tier;
}

const WristbandTierInfo& wristband_tier_info(int tier) {
  return kTiers[std::clamp(tier + 1, 0, 8)];
}

AchievementGameplayState AchievementGameplayState::from_drive_save(const drive::DriveSave& save,
                                                                   int roads_discovered_count, int race_encounters) {
  AchievementGameplayState st;
  st.career_started = save.career_started;
  if (save.collected_wristband >= 0) {
    st.wristbands_earned = std::clamp(save.collected_wristband + 1, 0, 7);
  } else if (save.wristband_points > 0) {
    static constexpr int kThresholds[7] = {100, 1000, 3500, 8000, 15000, 26000, 42000};
    for (int t : kThresholds)
      if (save.wristband_points >= t) ++st.wristbands_earned;
  }
  st.num_races = save.num_races;
  st.num_victories = save.num_victories;
  st.num_podiums = save.num_podiums;
  for (const auto& [eid, place] : save.event_results) {
    if (place == 1) ++st.festival_races_won;
  }
  if (save.time_street > 1.0 && save.num_victories > 0) {
    st.street_races_won = std::max(1, save.num_victories / 4);
  }
  st.race_encounters_won = race_encounters;
  st.discount_signs_found = int(save.signs_found.size());
  for (const auto& b : save.barns) {
    if (b.status == 2) ++st.barns_found;
  }
  st.roads_discovered = roads_discovered_count;
  st.outposts_discovered = int(save.discovered_outposts.size());
  st.pr_stunts_completed = int(save.completed_pr_stunts.size());
  st.garage_cars = int(save.garage.size());
  st.promo_cars_spotted = int(save.promo_cars.size());
  st.total_distance_m = save.total_distance;
  st.top_speed_mps = save.top_speed;
  st.total_winnings = save.total_winnings;
  st.max_skills_score = save.max_skills_score;
  st.popularity_points = save.popularity_points;
  st.driver_level = save.level;
  st.has_rally = save.has_rally;
  st.rally_stages_done = save.num_rally_stages;
  st.rally_victories = save.num_rally_victories;
  st.rally_level = save.rally_level;
  for (const auto& c : save.club) {
    bool any = false;
    for (const auto& [key, done] : c.challenges) {
      if (done) {
        ++st.club_medals_total;
        any = true;
      }
    }
    if (any) ++st.club_cars_with_medal;
  }
  return st;
}

void AchievementsSystem::init(const fh1::SpaDatabase& spa) {
  entries_.clear();
  raw_icons_ = spa.images;
  uploaded_icons_.clear();

  if (!spa.empty()) {
    entries_.reserve(spa.achievements.size());
    for (const auto& sa : spa.achievements) {
      AchievementEntry e;
      e.id = sa.id;
      e.image_id = sa.image_id;
      e.gamerscore = sa.gamerscore;
      e.name = sa.label.empty() ? ("ACHIEVEMENT #" + std::to_string(sa.id)) : sa.label;
      e.description = sa.description;
      e.unachieved_description = sa.unachieved_description;
      if (const auto* m = find_meta(sa.id)) {
        e.category = m->category;
        e.availability = m->availability;
      } else {
        e.category = AchievementCategory::kCareer;
        e.availability = AchievementAvailability::kAvailable;
      }
      entries_.push_back(std::move(e));
    }
  } else {
    // Structural fallback (base 50 achievements, zero game prose) when no spa.bin is present.
    for (const auto& m : kFh1Meta) {
      if (m.id > 205) break;
      AchievementEntry e;
      e.id = m.id;
      e.gamerscore = m.default_g;
      e.category = m.category;
      e.availability = m.availability;
      e.name = "ACHIEVEMENT " + std::to_string(m.id);
      e.description = "Load game spa.bin for localized achievement details.";
      e.unachieved_description = e.description;
      entries_.push_back(std::move(e));
    }
  }

  rebuild_tabs();
  rebuild_visible();
}

bool AchievementsSystem::load_state(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  if (!std::getline(in, line) || line.rfind("shift-achievements", 0) != 0) return false;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string key;
    if (!(ls >> key)) continue;
    if (key == "unlock") {
      uint16_t id = 0;
      int64_t ts = 0;
      if (ls >> id >> ts) {
        for (auto& e : entries_) {
          if (e.id == id) {
            e.unlocked = true;
            e.unlock_utc = ts;
            e.is_new = false;
            break;
          }
        }
      }
    } else if (key == "tracked") {
      uint16_t id = 0;
      if (ls >> id) tracked_id_ = id;
    }
  }
  if (const auto* tr = find(tracked_id_); !tr || tr->unlocked) tracked_id_ = 0;
  rebuild_visible();
  return true;
}

bool AchievementsSystem::save_state(const std::filesystem::path& path) const {
  if (path.empty()) return false;
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "shift-achievements 1\n";
    if (tracked_id_ != 0) out << "tracked " << tracked_id_ << "\n";
    for (const auto& e : entries_) {
      if (e.unlocked) out << "unlock " << e.id << " " << e.unlock_utc << "\n";
    }
    if (!out) return false;
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
    if (ec) return false;
  }
  return true;
}

bool AchievementsSystem::unlock(uint16_t id, int64_t utc_now, bool trigger_toast) {
  for (auto& e : entries_) {
    if (e.id != id) continue;
    if (e.unlocked) return false;
    e.unlocked = true;
    e.is_new = true;
    e.unlock_utc = utc_now > 0 ? utc_now : now_utc();
    if (tracked_id_ == id) tracked_id_ = 0;
    if (trigger_toast) {
      const uint32_t earned = earned_gamerscore();
      const int tier = completion_tier();
      AchievementToast toast;
      toast.id = e.id;
      toast.image_id = e.image_id;
      toast.gamerscore = e.gamerscore;
      toast.name = e.name;
      toast.subtitle = std::to_string(earned) + "G TOTAL  •  " + wristband_tier_info(tier).name + " WRISTBAND";
      toast.large_gold = e.gamerscore >= 50;
      toast.elapsed = 0.0;
      toast.duration = toast.large_gold ? 6.0 : 5.0;
      toasts_.push_back(std::move(toast));
      if (on_unlock_sound_) on_unlock_sound_();
    }
    rebuild_visible();
    return true;
  }
  return false;
}

int AchievementsSystem::evaluate(const AchievementGameplayState& state, bool trigger_toasts) {
  int newly_unlocked = 0;
  const int64_t utc = now_utc();
  for (auto& e : entries_) {
    if (e.availability != AchievementAvailability::kAvailable) {
      e.progress.reset();
      continue;
    }
    bool should_unlock = false;
    auto prog = eval_condition(e.id, state, should_unlock);
    if (!e.unlocked && should_unlock) {
      if (unlock(e.id, utc, trigger_toasts)) ++newly_unlocked;
    }
    if (!e.unlocked && prog && prog->target > 1 && prog->current < prog->target) {
      e.progress = prog;
    } else {
      e.progress.reset();
    }
  }
  rebuild_visible();
  return newly_unlocked;
}

void AchievementsSystem::open_screen() {
  screen_open_ = true;
  screen_time_ = 0.0;
  rebuild_tabs();
  rebuild_visible();
}

void AchievementsSystem::close_screen() {
  screen_open_ = false;
  for (auto& e : entries_) e.is_new = false;
}

void AchievementsSystem::switch_tab(int delta) {
  if (tabs_.empty()) return;
  const int n = int(tabs_.size());
  tab_idx_ = size_t(((int(tab_idx_) + delta) % n + n) % n);
  focus_index_ = 0;
  scroll_row_ = 0;
  rebuild_visible();
}

void AchievementsSystem::select_tab(AchievementCategory cat) {
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i] == cat) {
      tab_idx_ = i;
      focus_index_ = 0;
      scroll_row_ = 0;
      rebuild_visible();
      return;
    }
  }
}

void AchievementsSystem::move_focus(int dx, int dy) {
  if (visible_.empty()) return;
  constexpr int kCols = 5;
  const int n = int(visible_.size());
  focus_index_ = std::clamp(focus_index_ + dx + dy * kCols, 0, n - 1);
  const int row = focus_index_ / kCols;
  const int vis_rows = (active_tab() == AchievementCategory::kAll && !almost_there_.empty()) ? 2 : 3;
  if (row < scroll_row_) scroll_row_ = row;
  if (row >= scroll_row_ + vis_rows) scroll_row_ = row - vis_rows + 1;
  entries_[visible_[size_t(focus_index_)]].is_new = false;
}

void AchievementsSystem::cycle_sort() {
  sort_ = static_cast<AchievementSort>((int(sort_) + 1) % int(AchievementSort::kCount));
  focus_index_ = 0;
  scroll_row_ = 0;
  rebuild_visible();
}

bool AchievementsSystem::toggle_track_focused() {
  const auto* e = focused_entry();
  if (!e || e->unlocked || !e->available()) return false;
  tracked_id_ = (tracked_id_ == e->id) ? 0 : e->id;
  return tracked_id_ != 0;
}

void AchievementsSystem::set_tracked_id(uint16_t id) {
  const auto* e = find(id);
  tracked_id_ = (e && !e->unlocked && e->available()) ? id : 0;
}

void AchievementsSystem::update(double dt) {
  dt = std::clamp(dt, 0.0, 0.1);
  global_time_ += dt;
  if (screen_open_) screen_time_ += dt;
  if (!toasts_.empty()) {
    toasts_.front().elapsed += dt;
    if (toasts_.front().elapsed >= toasts_.front().duration) {
      toasts_.erase(toasts_.begin());
    }
  }
}

uint32_t AchievementsSystem::earned_gamerscore() const {
  uint32_t sum = 0;
  for (const auto& e : entries_)
    if (e.unlocked) sum += e.gamerscore;
  return sum;
}

uint32_t AchievementsSystem::total_gamerscore() const {
  uint32_t sum = 0;
  for (const auto& e : entries_) sum += e.gamerscore;
  return sum;
}

uint32_t AchievementsSystem::available_gamerscore() const {
  uint32_t sum = 0;
  for (const auto& e : entries_)
    if (e.available()) sum += e.gamerscore;
  return sum;
}

int AchievementsSystem::unlocked_count() const {
  int n = 0;
  for (const auto& e : entries_)
    if (e.unlocked) ++n;
  return n;
}

const AchievementEntry* AchievementsSystem::find(uint16_t id) const {
  for (const auto& e : entries_)
    if (e.id == id) return &e;
  return nullptr;
}

const AchievementEntry* AchievementsSystem::focused_entry() const {
  if (visible_.empty()) return nullptr;
  const int idx = std::clamp(focus_index_, 0, int(visible_.size()) - 1);
  return &entries_[visible_[size_t(idx)]];
}

void AchievementsSystem::rebuild_tabs() {
  const AchievementCategory prev = active_tab();
  tabs_.clear();
  tabs_.push_back(AchievementCategory::kAll);
  constexpr AchievementCategory kOrder[] = {
      AchievementCategory::kCareer,  AchievementCategory::kRacing, AchievementCategory::kFame,
      AchievementCategory::kExplore, AchievementCategory::kFestival, AchievementCategory::kOnline,
      AchievementCategory::kRally,   AchievementCategory::kClub,
  };
  for (AchievementCategory c : kOrder) {
    bool any = false;
    for (const auto& e : entries_) {
      if (e.category == c) {
        any = true;
        break;
      }
    }
    if (any) tabs_.push_back(c);
  }
  tab_idx_ = 0;
  for (size_t i = 0; i < tabs_.size(); ++i)
    if (tabs_[i] == prev) tab_idx_ = i;
}

void AchievementsSystem::rebuild_visible() {
  const AchievementCategory cat = active_tab();
  visible_.clear();
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (cat == AchievementCategory::kAll || entries_[i].category == cat) visible_.push_back(i);
  }
  std::stable_sort(visible_.begin(), visible_.end(), [&](size_t ia, size_t ib) {
    const auto& a = entries_[ia];
    const auto& b = entries_[ib];
    switch (sort_) {
      case AchievementSort::kTitleOrder: return a.id < b.id;
      case AchievementSort::kNearest: {
        const float fa = (!a.unlocked && a.progress) ? a.progress->fraction() : (a.unlocked ? -2.0f : -1.0f);
        const float fb = (!b.unlocked && b.progress) ? b.progress->fraction() : (b.unlocked ? -2.0f : -1.0f);
        if (std::fabs(fa - fb) > 1e-4f) return fa > fb;
        return a.id < b.id;
      }
      case AchievementSort::kLockedFirst:
        if (a.unlocked != b.unlocked) return !a.unlocked;
        return a.id < b.id;
      case AchievementSort::kRecent:
        if (a.unlocked != b.unlocked) return a.unlocked;
        if (a.unlock_utc != b.unlock_utc) return a.unlock_utc > b.unlock_utc;
        return a.id < b.id;
      case AchievementSort::kValue:
        if (a.gamerscore != b.gamerscore) return a.gamerscore > b.gamerscore;
        return a.id < b.id;
      default: return a.id < b.id;
    }
  });

  // Top 3 locked achievements with highest progress fraction for "ALMOST THERE" row.
  almost_there_.clear();
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    if (!e.unlocked && e.available() && e.progress && e.progress->fraction() > 0.0f) {
      almost_there_.push_back(i);
    }
  }
  std::sort(almost_there_.begin(), almost_there_.end(), [&](size_t ia, size_t ib) {
    const float fa = entries_[ia].progress->fraction();
    const float fb = entries_[ib].progress->fraction();
    if (std::fabs(fa - fb) > 1e-4f) return fa > fb;
    return entries_[ia].gamerscore > entries_[ib].gamerscore;
  });
  if (almost_there_.size() > 3) almost_there_.resize(3);

  if (visible_.empty()) focus_index_ = 0;
  else focus_index_ = std::clamp(focus_index_, 0, int(visible_.size()) - 1);
}

bool AchievementsSystem::ensure_icon_uv(uint32_t image_id, bool grey, std::array<float, 4>& uv) const {
  if (image_id == 0) return false;
  const std::string key = (grey ? "ach.g." : "ach.c.") + std::to_string(image_id);
  if (ui::atlas_find(key, uv)) return true;
  if (uploaded_icons_.count(image_id)) return false;
  uploaded_icons_.insert(image_id);

  const auto it = raw_icons_.find(image_id);
  if (it == raw_icons_.end() || it->second.empty()) return false;
  int iw = 0, ih = 0, ic = 0;
  unsigned char* px = stbi_load_from_memory(it->second.data(), int(it->second.size()), &iw, &ih, &ic, 4);
  if (!px || iw <= 0 || ih <= 0) {
    if (px) stbi_image_free(px);
    return false;
  }
  const size_t nbytes = size_t(iw) * size_t(ih) * 4;
  std::array<float, 4> c_uv{}, g_uv{};
  const bool ok_c = ui::atlas_image("ach.c." + std::to_string(image_id), uint32_t(iw), uint32_t(ih),
                                    std::span<const uint8_t>(px, nbytes), c_uv);
  std::vector<uint8_t> gpx(nbytes);
  for (size_t i = 0; i < nbytes; i += 4) {
    const uint8_t l = uint8_t(0.299f * float(px[i]) + 0.587f * float(px[i + 1]) + 0.114f * float(px[i + 2]));
    const uint8_t dim = uint8_t(float(l) * 0.72f);
    gpx[i + 0] = dim;
    gpx[i + 1] = dim;
    gpx[i + 2] = dim;
    gpx[i + 3] = px[i + 3];
  }
  stbi_image_free(px);
  const bool ok_g = ui::atlas_image("ach.g." + std::to_string(image_id), uint32_t(iw), uint32_t(ih), gpx, g_uv);
  if (grey && ok_g) {
    uv = g_uv;
    return true;
  }
  if (!grey && ok_c) {
    uv = c_uv;
    return true;
  }
  return false;
}

void AchievementsSystem::build_screen_hud(ui::List& out, float w, float h) const {
  if (!screen_open_) return;

  const uint32_t earned = earned_gamerscore();
  const uint32_t total = total_gamerscore();
  const uint32_t avail = available_gamerscore();
  const int tier = completion_tier();
  const auto& tinfo = wristband_tier_info(tier);
  const float intro = ease_out3(float(screen_time_ / 0.42));
  const uint32_t shown_earned = uint32_t(std::lround(float(earned) * intro));
  const float pct = avail > 0 ? float(earned) / float(avail) : 0.0f;

  char gs_buf[64], cnt_buf[48];
  std::snprintf(gs_buf, sizeof gs_buf, "%u / %u G  (%d%%)", shown_earned, total,
                int(std::lround(pct * 100.0f * intro)));
  std::snprintf(cnt_buf, sizeof cnt_buf, "%d OF %d UNLOCKED", unlocked_count(), total_count());

  ui4::TiltedBladeSpec spec;
  spec.title = "ACHIEVEMENTS";
  spec.eyebrow = "PINYON SHIFT // HORIZON";
  spec.scene_hint = "H_FIXED_SETTING_LIST";
  spec.status_kicker = std::string(tinfo.name);
  spec.status_value = gs_buf;
  spec.status_tag = cnt_buf;
  spec.status_color = tinfo.colour;

  spec.tabs.reserve(tabs_.size());
  for (AchievementCategory c : tabs_) {
    int c_done = 0, c_tot = 0;
    for (const auto& e : entries_) {
      if (c == AchievementCategory::kAll || e.category == c) {
        ++c_tot;
        if (e.unlocked) ++c_done;
      }
    }
    spec.tabs.push_back(std::string(achievement_category_label(c)) + " " + std::to_string(c_done) + "/" +
                        std::to_string(c_tot));
  }
  spec.active_tab = int(tab_idx_);
  spec.list_header = std::string(achievement_category_label(active_tab())) + " ACHIEVEMENTS";
  spec.list_header_right = "SORT: " + std::string(achievement_sort_label(sort_));
  spec.empty_message = "No achievements in this category.";
  spec.selected_index = focus_index_;
  spec.max_visible = 9;

  spec.rows.reserve(visible_.size());
  for (size_t vi = 0; vi < visible_.size(); ++vi) {
    const auto& e = entries_[visible_[vi]];
    ui4::BladeRow r;
    r.label = e.name;
    const std::string& desc =
        (!e.unlocked && !e.unachieved_description.empty()) ? e.unachieved_description : e.description;
    r.subtitle = desc;
    r.selected = (int(vi) == focus_index_);
    // Authentic 64x64 SPA PNG icon when available, falling back to badge/trophy/padlock icon!
    std::array<float, 4> icon_uv{};
    if (ensure_icon_uv(e.image_id, !e.unlocked, icon_uv)) {
      r.has_custom_icon = true;
      r.custom_icon_uv = icon_uv;
    }
    r.badge_icon_kind = e.is_new     ? int(ui4::BadgeIconKind::kNew)
                        : e.unlocked ? (e.gamerscore >= 30 ? int(ui4::BadgeIconKind::kGoldMedal)
                                                           : int(ui4::BadgeIconKind::kTrophy))
                                     : int(ui4::BadgeIconKind::kLocked);
    char gpill[16];
    std::snprintf(gpill, sizeof gpill, "%u G", e.gamerscore);
    r.value = gpill;
    r.value_color = e.unlocked ? value_stripe_colour(e.gamerscore) : ui4::kDim;
    if (e.is_new) {
      r.badge_tag = "NEW";
      r.badge_color = ui4::kPink;
    } else if (e.unlocked) {
      r.badge_tag = "UNLOCKED";
      r.badge_color = ui4::kGreen;
    } else if (tracked_id_ == e.id) {
      r.badge_tag = "TRACKED";
      r.badge_color = ui4::kAmber;
    } else if (!e.available()) {
      r.badge_tag = "OFFLINE";
      r.badge_color = rgba(70, 76, 88, 220);
    }
    spec.rows.push_back(std::move(r));
  }

  // Compact right-hand DIRTMASKS/PANEL01 detail card (tightly sized — ZERO empty charcoal void!)
  if (const auto* fe = focused_entry()) {
    spec.detail_header = "ACHIEVEMENT DETAILS";
    spec.detail_title = fe->name;
    spec.detail_title_color = fe->unlocked ? ui4::kPink : ui4::kBrushBlack;
    spec.detail_icon_kind = fe->unlocked ? int(ui4::BadgeIconKind::kTrophy) : int(ui4::BadgeIconKind::kLocked);
    std::array<float, 4> det_uv{};
    if (ensure_icon_uv(fe->image_id, !fe->unlocked, det_uv)) {
      spec.has_detail_icon_uv = true;
      spec.detail_icon_uv = det_uv;
    }
    const std::string& desc =
        (!fe->unlocked && !fe->unachieved_description.empty()) ? fe->unachieved_description : fe->description;
    spec.detail_paragraphs.push_back(desc);

    const std::string status_str = fe->unlocked ? "UNLOCKED"
                                   : !fe->available()
                                       ? (fe->availability == AchievementAvailability::kNeedsRivals
                                              ? "RIVALS (OFFLINE)"
                                              : "XBOX LIVE (OFFLINE)")
                                       : (tracked_id_ == fe->id ? "TRACKED ON HUD" : "LOCKED");
    spec.detail_kvs.push_back({"GAMERSCORE", std::to_string(fe->gamerscore) + " G", value_stripe_colour(fe->gamerscore)});
    spec.detail_kvs.push_back(
        {"STATUS", status_str, fe->unlocked ? ui4::kGreen : (tracked_id_ == fe->id ? ui4::kYellow : ui4::kDim)});
    spec.detail_kvs.push_back({"CATEGORY", achievement_category_label(fe->category), ui4::kWhite});
    if (fe->unlocked && fe->unlock_utc > 0) {
      spec.detail_kvs.push_back({"UNLOCKED ON", format_utc_date(fe->unlock_utc), ui4::kOffWhite});
    }
    if (!fe->unlocked && fe->progress) {
      char pb[48];
      std::snprintf(pb, sizeof pb, "PROGRESS  %u / %u  (%d%%)", fe->progress->current, fe->progress->target,
                    int(std::lround(fe->progress->fraction() * 100.0f)));
      spec.detail_progress = fe->progress->fraction();
      spec.detail_progress_label = pb;
      spec.detail_progress_color = ui4::kYellow;
    }
    if (!almost_there_.empty()) {
      const auto& ae = entries_[almost_there_.front()];
      spec.detail_footer = "ALMOST THERE: " + ae.name + " (" + std::to_string(ae.gamerscore) + "G)";
    }
  }

  spec.prompts = {{"A", "TRACK ON HUD"},
                  {"LB", "CATEGORY"},
                  {"Y", "SORT: " + std::string(achievement_sort_label(sort_))},
                  {"B", "BACK"}};
  ui4::tilted_blade_screen(out, w, h, spec);
}

void AchievementsSystem::build_hud_overlays(ui::List& out, float w, float h, bool show_track_chip) const {
  const float s = h / 720.0f;

  // 1. Tracked Achievement HUD chip (sharp gritty FH1 panel).
  if (show_track_chip && !screen_open_ && tracked_id_ != 0) {
    if (const auto* tr = find(tracked_id_); tr && !tr->unlocked) {
      const float cx0 = 56.0f * s, cy0 = 32.0f * s;
      const float cw = 296.0f * s, ch = 58.0f * s;
      ui4::panel(out, cx0, cy0, cx0 + cw, cy0 + ch, s, ui4::kCharcoalDark, ui4::kYellow, 0, true);
      std::array<float, 4> iuv{};
      if (ensure_icon_uv(tr->image_id, false, iuv)) {
        out.image(cx0 + 10.0f * s, cy0 + 8.0f * s, cx0 + 52.0f * s, cy0 + 50.0f * s, iuv, ui4::kWhite);
      } else {
        out.rect(cx0 + 10.0f * s, cy0 + 8.0f * s, cx0 + 52.0f * s, cy0 + 50.0f * s, rgba(255, 190, 48, 45));
      }
      out.text(cx0 + 60.0f * s, cy0 + 8.0f * s, "TRACKED ACHIEVEMENT", ui4::style(Font::kE, 9.0f * s, ui4::kYellow));
      std::string nm = tr->name.size() > 22 ? (tr->name.substr(0, 21) + "..") : tr->name;
      out.text(cx0 + 60.0f * s, cy0 + 21.0f * s, nm, ui4::style(Font::kB, 11.5f * s, ui4::kWhite));
      if (tr->progress) {
        char pb[24];
        std::snprintf(pb, sizeof pb, "%u / %u", tr->progress->current, tr->progress->target);
        out.text(cx0 + cw - 12.0f * s, cy0 + 21.0f * s, pb, ui4::style(Font::kE, 10.5f * s, ui4::kYellow, Align::kRight));
        const float px0 = cx0 + 60.0f * s, px1 = cx0 + cw - 12.0f * s, py0 = cy0 + 41.0f * s;
        out.rect(px0, py0, px1, py0 + 5.0f * s, rgba(255, 255, 255, 30));
        out.rect(px0, py0, px0 + (px1 - px0) * tr->progress->fraction(), py0 + 5.0f * s, ui4::kYellow);
      }
    }
  }

  // 2. Animated Achievement Unlock Toast (sharp gritty FH1 toast + pink/gold accent).
  if (!toasts_.empty()) {
    const auto& toast = toasts_.front();
    const float in_t = ease_out3(float(toast.elapsed / 0.18));
    const float out_t = toast.elapsed > (toast.duration - 0.30)
                            ? ease_out3(float((toast.elapsed - (toast.duration - 0.30)) / 0.30))
                            : 0.0f;
    const float slide = in_t * (1.0f - out_t);
    const float tw = (screen_open_ ? 506.0f : (toast.large_gold ? 520.0f : 480.0f)) * s;
    const float th = (toast.large_gold ? 90.0f : 82.0f) * s;
    const float x0 = screen_open_ ? (w - 56.0f * s - tw - 16.0f * s) : (0.5f * (w - tw));
    const float y0 = screen_open_
                         ? ((h - 96.0f * s - th) + (1.0f - slide) * 44.0f * s)
                         : ((-th - 16.0f * s) + (th + 46.0f * s) * slide);
    const uint32_t accent = toast.large_gold ? ui4::kYellow : ui4::kPink;

    ui4::panel(out, x0, y0, x0 + tw, y0 + th, s, ui4::kCharcoalDark, accent, 0, true);
    out.frame(x0, y0, x0 + tw, y0 + th, ui::fade(accent, 0.85f), 1.8f * s);

    if (toast.elapsed > 0.20 && toast.elapsed < 1.25) {
      const float st = float((toast.elapsed - 0.20) / 1.05);
      const float sx = x0 + tw * st;
      out.rect(std::max(x0, sx - 36.0f * s), y0 + 2.0f * s, std::min(x0 + tw, sx + 36.0f * s), y0 + th - 2.0f * s,
               rgba(255, 255, 255, 42));
    }

    const float pop = toast.elapsed < 0.28 ? (0.85f + 0.22f * std::sin(float(toast.elapsed / 0.28) * 3.14159f)) : 1.0f;
    const float isz = 58.0f * s * pop;
    const float icx = x0 + 48.0f * s, icy = y0 + 0.5f * th;
    std::array<float, 4> iuv{};
    if (ensure_icon_uv(toast.image_id, false, iuv)) {
      out.image(icx - 0.5f * isz, icy - 0.5f * isz, icx + 0.5f * isz, icy + 0.5f * isz, iuv, ui4::kWhite);
    } else {
      out.rect(icx - 0.5f * isz, icy - 0.5f * isz, icx + 0.5f * isz, icy + 0.5f * isz, ui::fade(accent, 0.35f));
    }
    const uint32_t kWhite = ui4::kWhite;
    const uint32_t kDim = ui4::kDim;

    // Text + counting-up gamerscore.
    const float tx = x0 + 92 * s;
    out.text(tx, y0 + 12 * s, "ACHIEVEMENT UNLOCKED",
             TextStyle{.size = 10.5f * s, .weight = 0.12f, .tracking = 0.18f, .color = accent, .font = Font::kE});
    out.text(tx, y0 + 31 * s, toast.name,
             TextStyle{.size = 17 * s, .weight = 0.12f, .color = kWhite, .font = Font::kE});
    out.text(tx, y0 + 57 * s, toast.subtitle,
             TextStyle{.size = 10.5f * s, .weight = 0.10f, .color = kDim, .font = Font::kB});

    const uint32_t g_shown = uint32_t(std::lround(float(toast.gamerscore) * ease_out3(float(toast.elapsed / 0.55))));
    out.rect(x0 + tw - 82 * s, y0 + 22 * s, x0 + tw - 16 * s, y0 + th - 22 * s, accent);
    out.text(x0 + tw - 49 * s, y0 + 0.5f * th - 11 * s, std::to_string(g_shown) + "G",
             TextStyle{.size = 20 * s, .weight = 0.12f, .color = toast.large_gold ? rgba(16, 16, 20) : kWhite,
                       .align = Align::kCenter, .font = Font::kE});
  }
}

}  // namespace carview
