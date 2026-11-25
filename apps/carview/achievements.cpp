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

#include "fh1data/stringtables.h"
#include "freeroam/club.h"
#include "game_hud.h"
#include "race/career.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

std::string loc_str(std::string_view table, std::string_view id, const char* fallback) {
  const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
  const std::string ref = "_&" + std::to_string(packed);
  const std::string resolved = game_text(ref);
  if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) return resolved;
  return fallback;
}

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
    // Popularity Rank milestones (250th -> Top 200, Top 100, Top 50, Top 10, Rank #1).
    case 170: {
      const int climbed = std::clamp(250 - std::clamp(s.popularity_rank, 1, 250), 0, 249);
      return counter(climbed, 250 - 200);
    }
    case 171: {
      const int climbed = std::clamp(250 - std::clamp(s.popularity_rank, 1, 250), 0, 249);
      return counter(climbed, 250 - 100);
    }
    case 172: {
      const int climbed = std::clamp(250 - std::clamp(s.popularity_rank, 1, 250), 0, 249);
      return counter(climbed, 250 - 50);
    }
    case 173: {
      const int climbed = std::clamp(250 - std::clamp(s.popularity_rank, 1, 250), 0, 249);
      return counter(climbed, 250 - 10);
    }
    case 174: {
      const int climbed = std::clamp(250 - std::clamp(s.popularity_rank, 1, 250), 0, 249);
      return counter(climbed, 250 - 1);
    }
    case 175: return counter(s.wristbands_earned, 1);
    case 176: return counter(s.wristbands_earned, 2);
    case 177: return counter(s.wristbands_earned, 3);
    case 178: return counter(s.wristbands_earned, 4);
    case 179: return counter(s.wristbands_earned, 5);
    case 180: return counter(s.wristbands_earned, 6);
    case 181: return counter(s.wristbands_earned, 7);
    case 182: should_unlock = s.finale_complete; return std::nullopt;
    case 183: return counter(s.festival_races_won, 10);
    case 184: return counter(s.street_races_won, 10);
    case 185: return counter(s.num_victories, 50);
    case 186: should_unlock = s.top_speed_mps >= 89.4; return counter(int(s.top_speed_mps * 2.23694), 200);
    case 187: return counter(s.outposts_discovered, std::max(1, s.total_outposts));
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
    case 211: return counter(s.rally_stages_done, 4);
    case 214: return counter(s.rally_stages_done, 28);
    case 215: return counter((s.rally_level >= 1 || s.rally_victories >= 7) ? 1 : 0, 1);
    // 1000 Club (216-225) — unified thresholds shared with freeroam::club::compute_medal_stats()
    case 216: return counter(s.club_medals_total, freeroam::club::kClubThresholdFirstMedal);
    case 217: return counter(s.club_ferrari_cars, freeroam::club::kClubThresholdFerrari);
    case 218: return counter(s.club_million_cr_cars, freeroam::club::kClubThresholdMillionCr);
    case 219: return counter(s.club_shelby_cars, freeroam::club::kClubThresholdShelby);
    case 220: return counter(s.club_lamborghini_cars, freeroam::club::kClubThresholdLamborghini);
    case 221: return counter(s.club_nissan_cars, freeroam::club::kClubThresholdNissan);
    case 222: return counter(s.club_bmw_cars, freeroam::club::kClubThresholdBmw);
    case 223: return counter(s.club_british_cars, freeroam::club::kClubThresholdBritish);
    case 224: return counter(s.club_ford_cars, freeroam::club::kClubThresholdFord);
    case 225: return counter(s.club_cars_with_medal, freeroam::club::kClubThresholdMultiCars);
    default: return std::nullopt;
  }
}

uint32_t value_stripe_colour(uint16_t g) {
  if (g >= 30) return rgba(244, 194, 68);   // Gold
  if (g >= 15) return rgba(196, 206, 222);  // Silver
  return rgba(206, 134, 78);                // Bronze
}

constexpr int kGridCols = 5;
constexpr int kGridVisRows = 3;

// Draws crisp vector star/padlock/secret icon + clean ASCII status badge ([UNLOCKED]/[LOCKED]/[SECRET]).
void draw_achievement_fallback_badge(ui::List& out, float ix0, float iy0, float isz, bool secret, bool unlocked,
                                     float s) {
  const float cx = ix0 + 0.5f * isz;
  const float cy = iy0 + 0.42f * isz;
  const uint32_t bg = unlocked ? rgba(255, 200, 50, 44) : rgba(255, 255, 255, 16);
  out.rect(ix0, iy0, ix0 + isz, iy0 + isz, bg);
  out.frame(ix0, iy0, ix0 + isz, iy0 + isz,
            unlocked ? rgba(244, 194, 68, 160) : rgba(140, 146, 160, 95), std::max(1.0f, 1.0f * s));

  const float icon_r = isz * 0.24f;
  if (secret) {
    out.circle(cx, cy, icon_r, rgba(32, 36, 44, 235));
    out.text(cx, cy - 0.55f * icon_r * 1.4f, "?",
             ui4::style(Font::kE, std::max(12.0f * s, icon_r * 1.45f), ui4::kDim, Align::kCenter));
  } else if (unlocked) {
    if (!ui4::badge_icon(out, cx - icon_r * 1.15f, cy - icon_r * 1.15f, cx + icon_r * 1.15f, cy + icon_r * 1.15f,
                         ui4::BadgeIconKind::kTrophy)) {
      // Crisp 5-point vector star (never blank emoji).
      constexpr float kPi = 3.14159265f;
      const float r_out = icon_r;
      const float r_in = icon_r * 0.44f;
      const float stroke = std::max(1.6f, 2.0f * s);
      std::array<std::array<float, 2>, 10> pts{};
      for (int i = 0; i < 10; ++i) {
        const float a = -0.5f * kPi + float(i) * (kPi / 5.0f);
        const float rr = (i % 2 == 0) ? r_out : r_in;
        pts[size_t(i)] = {cx + rr * std::cos(a), cy + rr * std::sin(a)};
      }
      out.circle(cx, cy, r_in * 0.85f, ui4::kYellow);
      for (int i = 0; i < 10; ++i) {
        const auto& a = pts[size_t(i)];
        const auto& b = pts[size_t((i + 1) % 10)];
        out.line(a[0], a[1], b[0], b[1], stroke, ui4::kYellow);
      }
    }
  } else {
    if (!ui4::badge_icon(out, cx - icon_r * 1.10f, cy - icon_r * 1.10f, cx + icon_r * 1.10f, cy + icon_r * 1.10f,
                         ui4::BadgeIconKind::kLocked)) {
      // Crisp vector padlock (arched shackle + rectangular body + keyhole — never blank emoji).
      const float sh_r = icon_r * 0.56f;
      const float stroke = std::max(1.6f, 2.2f * s);
      out.arc(cx, cy - icon_r * 0.28f, sh_r, stroke, -1.5708f, 1.5708f, ui4::kDim, false);
      const float bx0 = cx - icon_r * 0.78f, bx1 = cx + icon_r * 0.78f;
      const float by0 = cy - icon_r * 0.16f, by1 = cy + icon_r * 0.86f;
      out.rect(bx0, by0, bx1, by1, rgba(164, 170, 182, 235), 2.0f * s);
      out.circle(cx, by0 + (by1 - by0) * 0.42f, std::max(1.8f, icon_r * 0.22f), rgba(20, 22, 28, 255));
      out.rect(cx - std::max(1.0f, icon_r * 0.10f), by0 + (by1 - by0) * 0.45f,
               cx + std::max(1.0f, icon_r * 0.10f), by0 + (by1 - by0) * 0.78f, rgba(20, 22, 28, 255));
    }
  }

  // Clean ASCII status tag along bottom edge of badge box.
  const char* tag = achievement_status_badge_text(unlocked, secret);
  const float tag_h = std::max(11.0f * s, isz * 0.22f);
  out.rect(ix0 + 1.0f * s, iy0 + isz - tag_h - 1.0f * s, ix0 + isz - 1.0f * s, iy0 + isz - 1.0f * s,
           unlocked ? rgba(46, 172, 92, 220) : rgba(18, 20, 26, 225));
  TextStyle ts = ui4::style(Font::kE, std::max(6.8f * s, tag_h * 0.62f), unlocked ? ui4::kInk : ui4::kDim,
                            Align::kCenter);
  ui4::fit_text(tag, ts, isz - 4.0f * s, 5.5f * s);
  out.text(cx, iy0 + isz - tag_h + 0.5f * (tag_h - ts.size) - 1.0f * s, tag, ts);
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

const char* achievement_status_badge_text(bool unlocked, bool secret) {
  if (secret && !unlocked) return "[SECRET]";
  return unlocked ? "[UNLOCKED]" : "[LOCKED]";
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
                                                                   int roads_discovered_count, int race_encounters,
                                                                   const race::CareerCatalog* catalog,
                                                                   int popularity_rank, int total_outposts) {
  AchievementGameplayState st;
  st.career_started = save.career_started;
  st.finale_complete = save.finale_complete;
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
  // Count actual Festival vs Street Race wins from event_results (never fabricate from num_victories).
  for (const auto& [eid, place] : save.event_results) {
    if (place != 1) continue;
    if (catalog) {
      if (const auto* ev = catalog->find_event_by_id(eid)) {
        if (ev->style == 2) {
          ++st.street_races_won;
        } else if (ev->style == 1 || ev->style == 5 || ev->style == 6) {
          ++st.festival_races_won;
        }
        continue;
      }
    }
    ++st.festival_races_won;
  }
  st.race_encounters_won = race_encounters;
  st.discount_signs_found = int(save.signs_found.size());
  for (const auto& b : save.barns) {
    if (b.status == 2) ++st.barns_found;
  }
  st.roads_discovered = roads_discovered_count;
  st.outposts_discovered = int(save.discovered_outposts.size());
  st.total_outposts = std::max(1, total_outposts);
  st.pr_stunts_completed = int(save.completed_pr_stunts.size());
  st.garage_cars = int(save.garage.size());
  st.promo_cars_spotted = int(save.promo_cars.size());
  st.total_distance_m = save.total_distance;
  st.top_speed_mps = save.top_speed;
  st.total_winnings = save.total_winnings;
  st.max_skills_score = save.max_skills_score;
  st.popularity_points = save.popularity_points;
  st.popularity_rank = (popularity_rank >= 1 && popularity_rank <= 250) ? popularity_rank : 250;
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
      e.is_secret = !sa.show_unachieved();
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

void AchievementsSystem::clamp_scroll() {
  const int n = int(visible_.size());
  if (n <= 0) {
    focus_index_ = 0;
    scroll_row_ = 0;
    return;
  }
  focus_index_ = std::clamp(focus_index_, 0, n - 1);
  const int total_rows = (n + kGridCols - 1) / kGridCols;
  const int max_scroll = std::max(0, total_rows - kGridVisRows);
  const int focus_row = focus_index_ / kGridCols;
  if (focus_row < scroll_row_) scroll_row_ = focus_row;
  if (focus_row >= scroll_row_ + kGridVisRows) scroll_row_ = focus_row - kGridVisRows + 1;
  scroll_row_ = std::clamp(scroll_row_, 0, max_scroll);
}

void AchievementsSystem::select_focus_index(int idx) {
  if (visible_.empty()) {
    focus_index_ = 0;
    scroll_row_ = 0;
    return;
  }
  focus_index_ = std::clamp(idx, 0, int(visible_.size()) - 1);
  clamp_scroll();
  entries_[visible_[size_t(focus_index_)]].is_new = false;
}

void AchievementsSystem::move_focus(int dx, int dy) {
  if (visible_.empty()) return;
  const int n = int(visible_.size());
  focus_index_ = std::clamp(focus_index_ + dx + dy * kGridCols, 0, n - 1);
  clamp_scroll();
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
  clamp_scroll();
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

  const float s = h / 720.0f;
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

  ui4::subtle_vignette(out, w, h, 0.34f);

  // Authentic FH1 tilted chrome across Achievements header, tabs, 5x3 grid, and detail card.
  {
    ui4::TiltScope tilt(out, 0.5f * w, 0.5f * h, ui4::kTiltRad * 0.50f);

    // Top title tape + gamerscore summary pill.
    ui4::title_tape(out, s, 64.0f * s, 30.0f * s, loc_str("MyProfile", "IDS_Achievements", "ACHIEVEMENTS"), ui4::kPink, 34.0f,
                    420.0f);
    {
      const float sx0 = w - 396.0f * s, sx1 = w - 56.0f * s, sy0 = 30.0f * s, sy1 = 84.0f * s;
      ui4::pill(out, sx0, sy0, sx1, sy1, ui4::kPillGrey, ui4::PillKind::kPill02);
      out.rect(sx0, sy0, sx0 + 5.0f * s, sy1, tinfo.colour);
      out.text(sx0 + 16.0f * s, sy0 + 7.0f * s, std::string(tinfo.name) + " TIER  •  " + cnt_buf,
               ui4::style(Font::kB, 11.5f * s, tinfo.colour));
      out.text(sx0 + 16.0f * s, sy0 + 25.0f * s, gs_buf, ui4::style(Font::kE, 18.0f * s, ui4::kWhite));
    }

    // Category tab strip (registers clickable ui::kHitTab hit-zones for every filter tab).
    std::vector<std::string> tab_labels;
    tab_labels.reserve(tabs_.size());
    for (AchievementCategory c : tabs_) {
      int c_done = 0, c_tot = 0;
      for (const auto& e : entries_) {
        if (c == AchievementCategory::kAll || e.category == c) {
          ++c_tot;
          if (e.unlocked) ++c_done;
        }
      }
      tab_labels.push_back(std::string(achievement_category_label(c)) + " " + std::to_string(c_done) + "/" +
                           std::to_string(c_tot));
    }
    ui4::tab_strip(out, s, 64.0f * s, 94.0f * s, w - 64.0f * s, tab_labels, int(tab_idx_), true, ui4::kPink);

    // Authentic FH1 5x3 Achievement Tile Grid scrolling smoothly via scroll_row_.
    constexpr int kCols = kGridCols;
    constexpr int kRows = kGridVisRows;
    constexpr int kVisCount = kCols * kRows;
    const int n = int(visible_.size());
    const int total_rows = std::max(1, (n + kCols - 1) / kCols);
    const int start_row = std::clamp(scroll_row_, 0, std::max(0, total_rows - kRows));
    const int end_row = std::min(total_rows, start_row + kRows);
    const int start_idx = start_row * kCols;

    const float gx0 = 64.0f * s, gy0 = 136.0f * s;
    const float grid_w = 736.0f * s, grid_h = 496.0f * s;
    ui4::panel(out, gx0, gy0, gx0 + grid_w, gy0 + grid_h, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    ui4::offwhite_strip(out, gx0 + 12.0f * s, gy0 + 10.0f * s, gx0 + grid_w - 12.0f * s, gy0 + 34.0f * s, s, false);
    out.text(gx0 + 22.0f * s, gy0 + 15.0f * s,
             std::string(achievement_category_label(active_tab())) + "  •  ROWS " + std::to_string(start_row + 1) + "-" +
                 std::to_string(end_row) + " / " + std::to_string(total_rows),
             ui4::style(Font::kE, 12.5f * s, ui4::kInk));

    // Clickable Sort pill (mapped to ui4::kHitButton index 2 so clicking immediately cycles sort!).
    {
      const std::string sort_txt =
          loc_str("HelpButtons", "IDS_Sort", "SORT") + ": " + std::string(achievement_sort_label(sort_));
      ui::TextStyle sts = ui4::style(Font::kE, 11.0f * s, ui4::kWhite, Align::kCenter);
      const float pill_w = std::clamp(ui::List::measure(sort_txt, sts) + 42.0f * s, 148.0f * s, 250.0f * s);
      const float sx1 = gx0 + grid_w - 14.0f * s;
      const float sx0 = sx1 - pill_w;
      const float sy0 = gy0 + 11.0f * s, sy1 = gy0 + 33.0f * s;
      out.add_hit(sx0, sy0, sx1, sy1, ui4::kHitButton, 2);
      ui4::pill(out, sx0, sy0, sx1, sy1, ui4::kPink, ui4::PillKind::kMenu);
      ui4::opt_arrow(out, sx0 + 11.0f * s, 0.5f * (sy0 + sy1), 10.0f * s, -1, ui4::kWhite);
      ui4::opt_arrow(out, sx1 - 11.0f * s, 0.5f * (sy0 + sy1), 10.0f * s, +1, ui4::kWhite);
      ui4::fit_text(sort_txt, sts, pill_w - 34.0f * s, 8.5f * s);
      out.text(0.5f * (sx0 + sx1), sy0 + 0.5f * ((sy1 - sy0) - sts.size), sort_txt, sts);
    }

    if (visible_.empty()) {
      out.text(gx0 + 0.5f * grid_w, gy0 + 0.5f * grid_h,
               std::string("NO ACHIEVEMENTS IN THIS CATEGORY."),
               ui4::style(Font::kB, 14.0f * s, ui4::kDim, Align::kCenter));
    } else {
      const float pad_x = 16.0f * s, pad_y = 44.0f * s;
      const float scroll_reserve = total_rows > kRows ? 12.0f * s : 0.0f;
      const float gap = 10.0f * s;
      const float cell_w = (grid_w - 2.0f * pad_x - scroll_reserve - float(kCols - 1) * gap) / float(kCols);
      const float cell_h = (grid_h - pad_y - 16.0f * s - float(kRows - 1) * gap) / float(kRows);
      for (int slot = 0; slot < kVisCount && (start_idx + slot) < n; ++slot) {
        const int vi = start_idx + slot;
        const auto& e = entries_[visible_[size_t(vi)]];
        const int col = slot % kCols;
        const int row = slot / kCols;
        const float tx0 = gx0 + pad_x + float(col) * (cell_w + gap);
        const float ty0 = gy0 + pad_y + float(row) * (cell_h + gap);
        const float tx1 = tx0 + cell_w;
        const float ty1 = ty0 + cell_h;
        const bool sel = (vi == focus_index_);
        out.add_hit(tx0, ty0, tx1, ty1, ui::kHitRow, vi);

        const uint32_t bg = sel ? ui4::kPink : (e.unlocked ? rgba(36, 40, 48, 238) : rgba(20, 22, 27, 232));
        out.rect(tx0, ty0, tx1, ty1, bg);
        out.frame(tx0, ty0, tx1, ty1,
                  sel ? ui4::kYellow : (e.unlocked ? value_stripe_colour(e.gamerscore) : rgba(72, 76, 86, 170)),
                  sel ? 2.5f * s : 1.2f * s);

        // Centered 64x64 SPA icon (or crisp UI4 vector star/padlock + ASCII status badge — zero emojis).
        const float isz = std::min(64.0f * s, cell_h * 0.48f);
        const float ix0 = 0.5f * (tx0 + tx1) - 0.5f * isz;
        const float iy0 = ty0 + 12.0f * s;
        std::array<float, 4> icon_uv{};
        if (!e.is_masked_secret() && ensure_icon_uv(e.image_id, !e.unlocked, icon_uv)) {
          out.image(ix0, iy0, ix0 + isz, iy0 + isz, icon_uv, ui4::kWhite);
        } else {
          draw_achievement_fallback_badge(out, ix0, iy0, isz, e.is_masked_secret(), e.unlocked, s);
        }

        // Gamerscore corner pill.
        char gp[16];
        std::snprintf(gp, sizeof gp, "%uG", e.gamerscore);
        out.rect(tx1 - 36.0f * s, ty0 + 4.0f * s, tx1 - 4.0f * s, ty0 + 20.0f * s,
                 e.unlocked ? value_stripe_colour(e.gamerscore) : rgba(14, 16, 20, 210));
        out.text(tx1 - 20.0f * s, ty0 + 6.5f * s, gp,
                 ui4::style(Font::kE, 10.0f * s, e.unlocked ? ui4::kInk : ui4::kDim, Align::kCenter));

        if (tracked_id_ == e.id && !e.unlocked) {
          out.rect(tx0 + 4.0f * s, ty0 + 4.0f * s, tx0 + 44.0f * s, ty0 + 18.0f * s, ui4::kAmber);
          out.text(tx0 + 24.0f * s, ty0 + 6.0f * s, "TRK",
                   ui4::style(Font::kE, 8.5f * s, ui4::kInk, Align::kCenter));
        } else if (e.is_new) {
          out.rect(tx0 + 4.0f * s, ty0 + 4.0f * s, tx0 + 42.0f * s, ty0 + 18.0f * s, ui4::kYellow);
          out.text(tx0 + 23.0f * s, ty0 + 6.0f * s, "NEW",
                   ui4::style(Font::kE, 8.5f * s, ui4::kInk, Align::kCenter));
        }

        // Tile title (masked when secret & locked).
        const std::string tname = e.display_name();
        TextStyle tns = ui4::style(Font::kB, 11.0f * s, sel || e.unlocked ? ui4::kWhite : ui4::kOffWhite, Align::kCenter);
        ui4::fit_text(tname, tns, cell_w - 12.0f * s, 8.0f * s);
        out.text(0.5f * (tx0 + tx1), ty1 - 32.0f * s, tname, tns);

        // Bottom progress bar or unlocked bar.
        const float bx0 = tx0 + 8.0f * s, bx1 = tx1 - 8.0f * s, by0 = ty1 - 10.0f * s, by1 = ty1 - 5.0f * s;
        out.rect(bx0, by0, bx1, by1, rgba(255, 255, 255, 26));
        if (e.unlocked) {
          out.rect(bx0, by0, bx1, by1, ui4::kGreen);
        } else if (!e.is_masked_secret() && e.progress) {
          out.rect(bx0, by0, bx0 + (bx1 - bx0) * e.progress->fraction(), by1, ui4::kYellow);
        }
      }
      if (total_rows > kRows) {
        ui4::scroll_indicator(out, gx0 + grid_w - 14.0f * s, gy0 + pad_y, gx0 + grid_w - 7.0f * s,
                              gy0 + grid_h - 16.0f * s, start_row, kRows, total_rows, s);
      }
    }

    // Right-hand Achievement Details card (DIRTMASKS/PANEL01).
    if (const auto* fe = focused_entry()) {
      const float dx0 = gx0 + grid_w + 18.0f * s;
      const float dx1 = w - 56.0f * s;
      const float dy0 = gy0;
      const float dy1 = gy0 + grid_h;
      ui4::panel01(out, dx0, dy0, dx1, dy1, s, ui4::kCharcoalDark, ui4::kPink);
      ui4::brush(out, dx0 - 6.0f * s, dy0 + 12.0f * s, dx1 + 6.0f * s, dy0 + 48.0f * s,
                 fe->unlocked ? ui4::kPink : ui4::kBrushBlack);
      TextStyle dts = ui4::style(Font::kE, 16.5f * s, ui4::kWhite);
      const std::string dname = fe->display_name();
      ui4::fit_text(dname, dts, (dx1 - dx0) - 28.0f * s, 10.5f * s);
      out.text(dx0 + 16.0f * s, dy0 + 19.0f * s, dname, dts);

      // Large detail icon (SPA PNG or crisp vector star/padlock badge — zero emojis).
      const float disz = 72.0f * s;
      std::array<float, 4> det_uv{};
      if (!fe->is_masked_secret() && ensure_icon_uv(fe->image_id, !fe->unlocked, det_uv)) {
        out.image(dx0 + 16.0f * s, dy0 + 62.0f * s, dx0 + 16.0f * s + disz, dy0 + 62.0f * s + disz, det_uv, ui4::kWhite);
      } else {
        draw_achievement_fallback_badge(out, dx0 + 16.0f * s, dy0 + 62.0f * s, disz, fe->is_masked_secret(),
                                        fe->unlocked, s);
      }

      const std::string status_str = fe->unlocked ? "UNLOCKED"
                                     : fe->is_masked_secret()
                                         ? "SECRET (LOCKED)"
                                         : !fe->available()
                                               ? (fe->availability == AchievementAvailability::kNeedsRivals
                                                      ? "RIVALS (OFFLINE)"
                                                      : "XBOX LIVE (OFFLINE)")
                                               : (tracked_id_ == fe->id ? "TRACKED ON HUD" : "LOCKED");
      out.text(dx0 + 100.0f * s, dy0 + 68.0f * s, std::to_string(fe->gamerscore) + " G",
               ui4::style(Font::kE, 22.0f * s, value_stripe_colour(fe->gamerscore)));
      out.text(dx0 + 100.0f * s, dy0 + 96.0f * s, status_str,
               ui4::style(Font::kE, 12.5f * s,
                          fe->unlocked ? ui4::kGreen : (tracked_id_ == fe->id ? ui4::kYellow : ui4::kDim)));
      out.text(dx0 + 100.0f * s, dy0 + 116.0f * s, achievement_category_label(fe->category),
               ui4::style(Font::kB, 11.5f * s, ui4::kOffWhite));

      // Wrapped description
      const TextStyle desc_st = ui4::style(Font::kB, 13.0f * s, ui4::kWhite, Align::kLeft, true);
      const auto dlines = wrap_lines(fe->display_description(), desc_st, (dx1 - dx0) - 32.0f * s, 5);
      float py = dy0 + 154.0f * s;
      for (const auto& dl : dlines) {
        out.text(dx0 + 16.0f * s, py, dl, desc_st);
        py += 20.0f * s;
      }

      if (fe->unlocked && fe->unlock_utc > 0) {
        out.text(dx0 + 16.0f * s, py + 12.0f * s, "UNLOCKED ON: " + format_utc_date(fe->unlock_utc),
                 ui4::style(Font::kB, 11.5f * s, ui4::kOffWhite));
      } else if (!fe->unlocked && !fe->is_masked_secret() && fe->progress) {
        const float pby = std::max(py + 16.0f * s, dy0 + 278.0f * s);
        char pb[48];
        std::snprintf(pb, sizeof pb, "PROGRESS  %u / %u  (%d%%)", fe->progress->current, fe->progress->target,
                      int(std::lround(fe->progress->fraction() * 100.0f)));
        out.text(dx0 + 16.0f * s, pby, pb, ui4::style(Font::kE, 12.0f * s, ui4::kYellow));
        out.rect(dx0 + 16.0f * s, pby + 20.0f * s, dx1 - 16.0f * s, pby + 30.0f * s, rgba(255, 255, 255, 28));
        out.rect(dx0 + 16.0f * s, pby + 20.0f * s,
                 dx0 + 16.0f * s + (dx1 - dx0 - 32.0f * s) * fe->progress->fraction(), pby + 30.0f * s, ui4::kYellow);
      }

      if (!almost_there_.empty()) {
        const auto& ae = entries_[almost_there_.front()];
        const std::string af = "ALMOST THERE: " + ae.display_name() + " (" + std::to_string(ae.gamerscore) + "G)";
        TextStyle afs = ui4::style(Font::kB, 11.5f * s, ui4::kYellow);
        ui4::fit_text(af, afs, (dx1 - dx0) - 32.0f * s, 8.5f * s);
        out.text(dx0 + 16.0f * s, dy1 - 30.0f * s, af, afs);
      }
    }
  }

  ui4::button_bar(out,
                  {{"A", std::string("TRACK ON HUD")},
                   {"LB", std::string("CATEGORY")},
                   {"Y", loc_str("HelpButtons", "IDS_Sort", "SORT") + ": " + std::string(achievement_sort_label(sort_))},
                   {"B", loc_str("HelpButtons", "IDS_Back", "BACK")}},
                  w, h, 64.0f);
}

bool AchievementsSystem::top_left_hud_occupied(const ui::List& list, float w, float h, float* max_bottom_y) {
  const float s = h / 720.0f;
  const float zone_x0 = 24.0f * s, zone_x1 = 380.0f * s;
  const float zone_y0 = 14.0f * s, zone_y1 = 112.0f * s;
  bool occupied = false;
  float max_y = 0.0f;
  for (const auto& it : list.items()) {
    const float iw = it.rect[2] - it.rect[0];
    const float ih = it.rect[3] - it.rect[1];
    // Ignore full-screen backdrops and edge vignettes.
    if (iw <= 1.0f || ih <= 1.0f || iw >= w * 0.65f || ih >= h * 0.55f) continue;
    if (it.rect[2] > zone_x0 && it.rect[0] < zone_x1 && it.rect[3] > zone_y0 && it.rect[1] < zone_y1) {
      occupied = true;
      max_y = std::max(max_y, it.rect[3]);
    }
  }
  // Follow a block that runs on below the zone in rows (a circuit's LAP, LAST
  // and BEST timer lines under RACE), so the chip goes under its last row.
  for (bool grew = occupied; grew;) {
    grew = false;
    for (const auto& it : list.items()) {
      const float iw = it.rect[2] - it.rect[0];
      const float ih = it.rect[3] - it.rect[1];
      if (iw <= 1.0f || ih <= 1.0f || iw >= w * 0.65f || ih >= h * 0.55f) continue;
      if (it.rect[2] > zone_x0 && it.rect[0] < zone_x1 && it.rect[1] < max_y + 12.0f * s && it.rect[3] > max_y &&
          it.rect[3] < h * 0.6f) {
        max_y = it.rect[3];
        grew = true;
      }
    }
  }
  if (max_bottom_y) *max_bottom_y = max_y;
  return occupied;
}

void AchievementsSystem::build_hud_overlays(ui::List& out, float w, float h, bool show_track_chip,
                                            bool top_left_occupied, bool hide_when_occupied) const {
  const float s = h / 720.0f;

  // 1. Tracked Achievement HUD chip (sharp gritty FH1 panel) with non-overlap guard against
  //    active Race HUD and Campaign Objective banners in the top-left corner.
  if (show_track_chip && !screen_open_ && tracked_id_ != 0) {
    if (const auto* tr = find(tracked_id_); tr && !tr->unlocked) {
      float occ_bot = 0.0f;
      const bool occupied = top_left_occupied || top_left_hud_occupied(out, w, h, &occ_bot);
      const float cx0 = 56.0f * s;
      const float cw = 296.0f * s, ch = 58.0f * s;
      float cy0 = 32.0f * s;
      bool draw_chip = true;
      if (occupied) {
        if (hide_when_occupied) {
          draw_chip = false;
        } else {
          cy0 = std::max(occ_bot + 14.0f * s, 118.0f * s);
          // Hide cleanly if offsetting below the banner would intrude into the minimap zone.
          if (cy0 + ch > h - 250.0f * s) draw_chip = false;
        }
      }
      if (draw_chip) {
        ui4::panel(out, cx0, cy0, cx0 + cw, cy0 + ch, s, ui4::kCharcoalDark, ui4::kYellow, 0, true);
        std::array<float, 4> iuv{};
        if (ensure_icon_uv(tr->image_id, false, iuv)) {
          out.image(cx0 + 10.0f * s, cy0 + 8.0f * s, cx0 + 52.0f * s, cy0 + 50.0f * s, iuv, ui4::kWhite);
        } else {
          out.rect(cx0 + 10.0f * s, cy0 + 8.0f * s, cx0 + 52.0f * s, cy0 + 50.0f * s, rgba(255, 190, 48, 45));
        }
        out.text(cx0 + 60.0f * s, cy0 + 8.0f * s,
                 std::string("TRACKED ACHIEVEMENT"),
                 ui4::style(Font::kE, 9.0f * s, ui4::kYellow));
        std::string nm = tr->name.size() > 22 ? (tr->name.substr(0, 21) + "..") : tr->name;
        out.text(cx0 + 60.0f * s, cy0 + 21.0f * s, nm, ui4::style(Font::kB, 11.5f * s, ui4::kWhite));
        if (tr->progress) {
          char pb[24];
          std::snprintf(pb, sizeof pb, "%u / %u", tr->progress->current, tr->progress->target);
          out.text(cx0 + cw - 12.0f * s, cy0 + 21.0f * s, pb,
                   ui4::style(Font::kE, 10.5f * s, ui4::kYellow, Align::kRight));
          const float px0 = cx0 + 60.0f * s, px1 = cx0 + cw - 12.0f * s, py0 = cy0 + 41.0f * s;
          out.rect(px0, py0, px1, py0 + 5.0f * s, rgba(255, 255, 255, 30));
          out.rect(px0, py0, px0 + (px1 - px0) * tr->progress->fraction(), py0 + 5.0f * s, ui4::kYellow);
        }
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
