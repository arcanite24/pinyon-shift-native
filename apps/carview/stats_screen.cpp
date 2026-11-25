#include "stats_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pause_screen.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr int kItems = 14;  // StatItem .. StatItem14
// GAME PROGRESS's bar as drawn on 26_my_profile_stats_orig.png (its crop is
// not decoded): 121 canvas px from its left end.
constexpr float kBarLength = 121.0f;
// The LB and RB buttons' extents (their arrows and icons, canvas px).
constexpr float kLbHit[4] = {215.0f, 180.0f, 292.0f, 218.0f};
constexpr float kRbHit[4] = {900.0f, 143.0f, 972.0f, 183.0f};
// The rows sit 18 px lower on 26_my_profile_stats_orig.png than the scene
// places them (GAME PROGRESS's label at 230, not 212).
constexpr float kItemsDrop = 18.0f;
constexpr float kRowSlope = 0.052f;
constexpr float kBarRaise = -15.0f;
// The arrows' colour (RB's material; LB's pink is a state the capture does
// not show).
constexpr uint32_t kArrowTint = ui::rgba(232, 232, 232);

std::string item_root(int k) {
  return "Background/STATS/Items/StatItem" + (k == 0 ? std::string() : std::to_string(k + 1));
}

std::string thousands(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v), out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i && (s.size() - i) % 3 == 0) out += ',';
    out += s[i];
  }
  return v < 0 ? "-" + out : out;
}

std::string credits(int64_t v) { return thousands(v) + " Cr"; }

constexpr StringRef kUltimate[13] = {
    {"MyProfile", "IDS_Num_Perfect_Drifts", 0, "ULTIMATE DRIFT SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_Speeds", 0, "ULTIMATE SPEED SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_Drafts", 0, "ULTIMATE DRAFT SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_Passes", 0, "ULTIMATE PASS SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_Burnouts", 0, "ULTIMATE BURNOUT SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_Airs", 0, "ULTIMATE AIR SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_TwoWheels", 0, "ULTIMATE TWO-WHEELS SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_EDrifts", 0, "ULTIMATE E-DRIFT SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_NearMisses", 0, "ULTIMATE NEAR-MISS SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_Destructions", 0, "ULTIMATE WRECKAGE SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_Turn180s", 0, "ULTIMATE ONE-EIGHTY SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_JTurns", 0, "ULTIMATE J-TURN SKILLS"},
    {"MyProfile", "IDS_Num_Perfect_LawBreakers", 0, "ULTIMATE LAW-BREAKER SKILLS"},
};

}  // namespace

std::string stats_tab_name(StatsTab t) {
  return text_of(t == StatsTab::kRecords ? stats_text::kRecords
                 : t == StatsTab::kTime  ? stats_text::kTime
                                         : stats_text::kGeneral);
}

std::string stats_time_text(double seconds) {
  const int64_t s = seconds > 0 ? int64_t(std::floor(seconds)) : 0;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld h %02d:%02d", (long long)(s / 3600), int(s / 60 % 60), int(s % 60));
  return buf;
}

std::string stats_ordinal(int n) {
  const int t = n % 100, u = n % 10;
  const char* suffix = (t >= 11 && t <= 13) ? "th" : u == 1 ? "st" : u == 2 ? "nd" : u == 3 ? "rd" : "th";
  return std::to_string(n) + suffix;
}

std::vector<StatRow> stats_rows(StatsTab t, const StatsData& d) {
  std::vector<StatRow> rows;
  auto add = [&](StringRef label, std::string value, double progress = -1) {
    rows.push_back({text_of(label), std::move(value), progress});
  };
  if (t == StatsTab::kGeneral) {
    // 26_my_profile_stats_orig.png, top to bottom.
    char pct[16];
    const double p = std::clamp(d.game_progress, 0.0, 1.0);
    std::snprintf(pct, sizeof pct, "%.1f%%", p * 100.0);
    add({"MyProfile", "IDS_Career_Complete", 0, "GAME PROGRESS"}, pct, p);
    add({"CarHistory", "IDS_Display_Time_Driven", 0, "TIME DRIVEN"}, stats_time_text(d.time_driven_s));
    add({"CarHistory", "IDS_Display_Num_Victories", 0, "NUMBER OF VICTORIES"}, thousands(d.victories));
    add({"CarHistory", "IDS_Display_Num_Podiums", 0, "NUMBER OF PODIUMS"}, thousands(d.podiums));
    add({"CarHistory", "IDS_Display_Num_Races", 0, "NUMBER OF RACES"}, thousands(d.races));
    add({"MyProfile", "IDS_Cars_In_Garage", 0, "NUMBER OF CARS IN GARAGE"}, thousands(d.cars_in_garage));
    add({"CarHistory", "IDS_Display_Total_Winnings", 0, "TOTAL WINNINGS"}, credits(d.total_winnings));
    add({"MyProfile", "IDS_Garage_Value", 0, "GARAGE VALUE"}, credits(d.garage_value));
    add({"MyProfile", "IDS_Garage_Parts_Value", 0, "GARAGE PARTS VALUE"}, credits(d.parts_value));
    add({"MyProfile", "IDS_Popularity_Level", 0, "POPULARITY LEVEL"},
        d.popularity_rank > 0 ? stats_ordinal(d.popularity_rank) : std::string("-"));
  } else if (t == StatsTab::kRecords) {
    add({"MyProfile", "IDS_Highest_Skills_Score", 0, "HIGHEST SKILL CHAIN SCORE"}, thousands(d.max_skills_score));
    for (size_t i = 0; i < d.ultimate_skills.size(); ++i) add(kUltimate[i], thousands(d.ultimate_skills[i]));
  } else {
    add({"MyProfile", "IDS_TimeIn_FestivalRaces", 0, "TIME IN FESTIVAL RACES"}, stats_time_text(d.time_festival_s));
    add({"MyProfile", "IDS_TimeIn_StreetRaces", 0, "TIME IN STREET RACES"}, stats_time_text(d.time_street_s));
    add({"MyProfile", "IDS_TimeIn_ExhibitionRaces", 0, "TIME IN SHOWCASES"}, stats_time_text(d.time_showcase_s));
    for (StringRef r : {StringRef{"MyProfile", "IDS_Time_Spent_In_Menus", 0, "TIME SPENT IN THE MENUS"},
                        StringRef{"MyProfile", "IDS_Time_In_Upgrades", 0, "TIME SPENT UPGRADING"},
                        StringRef{"MyProfile", "IDS_Time_In_Tuning", 0, "TIME SPENT TUNING"},
                        StringRef{"MyProfile", "IDS_Time_In_Paintshop", 0, "TIME SPENT EDITING LIVERIES"},
                        StringRef{"MyProfile", "IDS_Time_In_Test_Drive", 0, "TIME SPENT TEST DRIVING"},
                        StringRef{"MyProfile", "IDS_Time_In_Showroom", 0, "TIME SPENT IN AUTOVISTA"}})
      add(r, stats_time_text(0));
  }
  return rows;
}

const std::vector<const char*>& stats_nodes() {
  static const std::vector<const char*> nodes = {
      "Background/TitleTape/Group/TitleTapeText",
      "Background/STATS/Items/StatItem/Pill/Statistic/TEXT_LABEL",
      "Background/STATS/Items/StatItem/Pill/Statistic/TEXT_VALUE",
      "Background/STATS/Items/StatItem/Pill/ProgressBar/Group/StatBarFill",
      "Background/STATS/Items/StatItem/Pill/ProgressBar/PROGRESS_BAR_STATIC_BKG/StatBarEmpty",
      "Background/STATS/Items/StatItem14/Pill/Statistic/TEXT_VALUE",
      "Background/STATS/NAV_POSITION_INDICATOR",
      "Background/TABS2/GROUP_ITEMS/TAB_CURRENT/Group/Text_Tab",
      "Background/TABS2/GROUP_LB/LBbutton",
      "Background/TABS2/GROUP_RB/RBbutton",
      "ButtonBar",
      "PlayerBar",
  };
  return nodes;
}

bool draw_stats_screen(ui::List& l, const DriveHud& st, const StatsHud& s, float w, float h) {
  SceneScreen sc(kStatsScene);
  if (!sc.ok()) return false;
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.bindings().fit_text = false;
  sc.hide(sc.node("ButtonBar"));
  // The rows fit: no scroll bar.
  sc.hide(sc.node("Background/STATS/NAV_POSITION_INDICATOR"));
  sc.text(sc.node("Background/TitleTape/Group/TitleTapeText"), text_of(stats_text::kTitle));
  sc.text(sc.node("Background/TABS2/GROUP_ITEMS/TAB_CURRENT/Group/Text_Tab"), stats_tab_name(s.tab));

  if (const int items = sc.node("Background/STATS/Items"); items >= 0) sc.at(items).dy = kItemsDrop;
  // RB's arrow points right: the same texture as LB's, mirrored (the
  // model's turn is not decoded), drawn after the scene.
  const int lb_arrow = sc.node("Background/TABS2/GROUP_LB/LBbutton/Group/Arrow");
  const int rb_arrow = sc.node("Background/TABS2/GROUP_RB/RBbutton/Group/Arrow");
  if (lb_arrow >= 0) sc.at(lb_arrow).tint = kArrowTint;
  const std::string arrow_key = rb_arrow >= 0 ? sc.scene().texture_key_of(rb_arrow) : std::string();
  const auto rb_rect = rb_arrow >= 0 ? sc.scene().rect(rb_arrow) : std::array<float, 4>{};
  sc.hide(rb_arrow);
  for (int k = 0; k < kItems; ++k) {
    const int pill = sc.node(item_root(k) + "/Pill");
    if (pill < 0) continue;
    const int stat = sc.node("Statistic", pill);
    const int bar = sc.node("ProgressBar", pill);
    const bool used = k < int(s.rows.size());
    // The rows past the tab's stay as empty pills (the original's GENERAL
    // has four).
    sc.text(sc.node("TEXT_LABEL", stat), used ? s.rows[size_t(k)].label : std::string());
    sc.text(sc.node("TEXT_VALUE", stat), used ? s.rows[size_t(k)].value : std::string());
    if (!used || s.rows[size_t(k)].progress < 0) {
      sc.hide(bar);
      continue;
    }
    // On the capture the bar shares its value's line, 15 px above where
    // the scene puts it.
    if (bar >= 0) sc.at(bar).dy = kBarRaise, sc.at(bar).dx = -kBarRaise * kRowSlope;
    const int fill = sc.node("Group/StatBarFill", bar);
    const int empty = sc.node("PROGRESS_BAR_STATIC_BKG/StatBarEmpty", bar);
    if (empty >= 0) sc.at(empty).visible_width = kBarLength;
    if (fill >= 0) sc.at(fill).visible_width = float(kBarLength * std::clamp(s.rows[size_t(k)].progress, 0.0, 1.0));
  }

  bind_player_bar(sc, st);
  sc.draw(l, f);
  std::array<float, 4> uv{};
  if (!arrow_key.empty() && ui4::texture(arrow_key, uv)) {
    std::swap(uv[0], uv[2]);
    l.image_rotated(f.x(0.5f * (rb_rect[0] + rb_rect[2])), f.y(0.5f * (rb_rect[1] + rb_rect[3])),
                    0.5f * (rb_rect[2] - rb_rect[0]) * f.s, 0.5f * (rb_rect[3] - rb_rect[1]) * f.s, -kRowSlope, uv,
                    kArrowTint);
  }
  l.add_hit(f.x(kLbHit[0]), f.y(kLbHit[1]), f.x(kLbHit[2]), f.y(kLbHit[3]), ui::kHitTab, 0);
  l.add_hit(f.x(kRbHit[0]), f.y(kRbHit[1]), f.x(kRbHit[2]), f.y(kRbHit[3]), ui::kHitTab, 1);
  std::vector<ui4::Prompt> prompts;
  for (const auto& [b, label] : s.prompts) prompts.push_back({b.c_str(), label});
  ui4::button_bar(l, prompts, w, h);
  return true;
}

}  // namespace carview
