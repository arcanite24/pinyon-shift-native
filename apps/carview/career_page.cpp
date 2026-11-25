#include "career_page.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "fh1data/stringtables.h"
#include "game_hud.h"
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

std::string pct_str(int cur, int tot) {
  if (tot <= 0) return "0%";
  const int pct = int(std::lround(100.0 * std::clamp(double(cur) / double(tot), 0.0, 1.0)));
  return std::to_string(pct) + "%";
}

}  // namespace

// Preserves all UTF-8 international text (Latin-1, Polish/Czech, Cyrillic, CJK, etc.)
// intact; only normalizes ASCII control characters (< 0x20 or 0x7F) to spaces.
std::string font_text(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c < 0x20u || c == 0x7Fu) {
      out.push_back(' ');
    } else {
      out.push_back(char(c));
    }
  }
  return out;
}

std::vector<std::string> CareerPage::tab_labels() const {
  return {
      loc_str("Career", "IDS_Progress", "PROGRESS"),
      loc_str("PauseMenu", "IDS_SponsorChallenges", "SPONSOR CHALLENGES"),
      loc_str("Career", "IDS_Statistics", "STATS"),
      loc_str("Career", "IDS_ContentPacks", "CONTENT PACKS"),
  };
}

void CareerPage::rebuild_lines() {
  lines_.clear();
  const auto& s = summary_;
  if (tab_ == CareerTab::kProgress) {
    const std::string wb = !s.level_name.empty()
                               ? font_text(s.level_name)
                               : (s.level >= 0 ? ("WRISTBAND TIER " + std::to_string(s.level + 1)) : "NO WRISTBAND YET");
    lines_.push_back("WRISTBAND: " + wb + " (" + std::to_string(s.wristband_points) + " PTS)");
    lines_.push_back("POPULARITY RANK: #" + std::to_string(std::clamp(s.popularity_rank, 1, 250)));
    lines_.push_back("EVENTS WON: " + std::to_string(s.victories) + " / " + std::to_string(std::max(s.races, s.victories)));
    lines_.push_back("ROADS DISCOVERED: " + std::to_string(s.roads_discovered) + " / " +
                     std::to_string(std::max(1, s.roads_total)));
    lines_.push_back("DISCOUNT SIGNS: " + std::to_string(s.signs_found) + " / " +
                     std::to_string(std::max(1, s.signs_total)));
    lines_.push_back("BARN FINDS: " + std::to_string(s.barns_found) + " / " + std::to_string(std::max(1, s.barns)));
    lines_.push_back("HORIZON OUTPOSTS: " + std::to_string(s.outposts_discovered) + " / " +
                     std::to_string(std::max(1, s.outposts_total)));
    if (s.club_total > 0) {
      lines_.push_back("1000 CLUB MEDALS: " + std::to_string(s.club_done) + " / " + std::to_string(s.club_total));
    }
    if (s.has_rally) {
      lines_.push_back("HORIZON RALLY: " + std::to_string(s.rally_victories) + " WINS  •  " +
                       std::to_string(s.rally_stages) + " STAGES");
    }
  } else if (tab_ == CareerTab::kSponsorChallenges) {
    lines_.push_back(loc_str("PauseMenu", "IDS_SponsorChallenges", "SPONSOR CHALLENGES"));
  } else if (tab_ == CareerTab::kStats) {
    const int win_pct = s.races > 0 ? int(std::lround(100.0 * double(s.victories) / double(s.races))) : 0;
    lines_.push_back("RACES COMPLETED: " + std::to_string(s.races));
    lines_.push_back("RACE VICTORIES: " + std::to_string(s.victories) + " (" + std::to_string(win_pct) + "%)");
    lines_.push_back("PODIUM FINISHES: " + std::to_string(s.podiums));
    lines_.push_back("CREDITS BALANCE: " + ui4::format_credits(s.credits));
    lines_.push_back("TOTAL WINNINGS: " + ui4::format_credits(s.total_winnings));
    lines_.push_back("CARS IN GARAGE: " + std::to_string(s.garage_cars));
    lines_.push_back("PR STUNTS COMPLETED: " + std::to_string(s.pr_stunts_done) + " / " +
                     std::to_string(std::max(1, s.pr_stunts_total)));
    if (s.total_distance_km > 0.0) {
      char db[64];
      std::snprintf(db, sizeof db, "DISTANCE DRIVEN: %.1f KM", s.total_distance_km);
      lines_.push_back(db);
    }
    if (s.top_speed_kmh > 0.0) {
      char sb[64];
      std::snprintf(sb, sizeof sb, "TOP SPEED RECORD: %.0f KM/H", s.top_speed_kmh);
      lines_.push_back(sb);
    }
    if (s.has_rally) {
      lines_.push_back("RALLY WINNINGS: " + ui4::format_credits(s.rally_winnings));
    }
  } else {
    lines_.push_back(loc_str("Career", "IDS_ContentPacks", "CONTENT PACKS") + "  " + std::to_string(owned_) + "/" +
                     std::to_string(packs_) + " OWNED");
    for (const auto& p : packs_list_) {
      lines_.push_back("  " + font_text(p.name) +
                       (p.owned() ? "" : !p.installed ? "  (NOT INSTALLED)" : !p.enabled ? "  (DISABLED)" : "  (NO HEADER)"));
    }
  }
  if (lines_.empty()) selected_ = 0;
  else selected_ = std::min(selected_, lines_.size() - 1);
}

void CareerPage::open(const CareerSummary& s, std::vector<cars::PackInfo> packs) {
  summary_ = s;
  packs_list_ = std::move(packs);
  owned_ = 0;
  packs_ = int(packs_list_.size());
  for (const auto& p : packs_list_) owned_ += p.owned();
  tab_ = CareerTab::kProgress;
  selected_ = 0;
  sponsor_selected_ = 0;
  rebuild_lines();
  open_ = true;
}

void CareerPage::set_tab(CareerTab tab) {
  const int clamped = std::clamp(int(tab), 0, int(CareerTab::kCount) - 1);
  tab_ = static_cast<CareerTab>(clamped);
  selected_ = 0;
  rebuild_lines();
}

void CareerPage::switch_tab(int delta) {
  const int n = int(CareerTab::kCount);
  const int next = ((int(tab_) + delta) % n + n) % n;
  set_tab(static_cast<CareerTab>(next));
}

void CareerPage::move(int by) {
  if (lines_.empty()) return;
  const int n = int(lines_.size());
  selected_ = size_t(std::clamp(int(selected_) + by, 0, n - 1));
}

void CareerPage::select_visible(int vis_idx) {
  if (lines_.empty()) return;
  constexpr int kRows = 7;
  const int n = int(lines_.size());
  const int first = std::clamp(int(selected_) - kRows / 2, 0, std::max(0, n - kRows));
  selected_ = size_t(std::clamp(first + vis_idx, 0, n - 1));
}

int CareerPage::visible_selected() const {
  if (lines_.empty()) return 0;
  constexpr int kRows = 7;
  const int n = int(lines_.size());
  const int first = std::clamp(int(selected_) - kRows / 2, 0, std::max(0, n - kRows));
  return int(selected_) - first;
}

void CareerPage::move_sponsor(int by, int count) {
  if (count <= 0) {
    sponsor_selected_ = 0;
    return;
  }
  sponsor_selected_ = ((sponsor_selected_ + by) % count + count) % count;
}

void CareerPage::select_sponsor(int idx, int count) {
  if (count <= 0) {
    sponsor_selected_ = 0;
    return;
  }
  sponsor_selected_ = std::clamp(idx, 0, count - 1);
}

void CareerPage::fill(std::string& title, std::vector<std::string>& lines, int& selected) const {
  if (!open_) return;
  const auto tabs = tab_labels();
  const int ti = std::clamp(int(tab_), 0, int(tabs.size()) - 1);
  title = tabs[size_t(ti)];
  constexpr int kRows = 7;
  const int n = int(lines_.size());
  const int first = std::clamp(int(selected_) - kRows / 2, 0, std::max(0, n - kRows));
  lines.assign(lines_.begin() + first, lines_.begin() + std::min(n, first + kRows));
  selected = int(selected_) - first;
}

void draw_career_page_ui4(ui::List& l, float w, float h, const CareerPage& page,
                          const std::vector<SponsorChallengeRow>& sponsors, int64_t credits_override,
                          float open_progress) {
  if (!page.is_open()) return;
  const auto& s = page.summary();
  const int64_t credits = credits_override >= 0 ? credits_override : s.credits;
  const CareerTab tab = page.active_tab();

  ui4::TiltedBladeSpec spec;
  spec.title = loc_str("PauseMenu", "IDS_MyProfile", "MY PROFILE");
  spec.eyebrow = loc_str("Career", "IDS_HorizonFestival", "HORIZON FESTIVAL  •  COLORADO");
  spec.scene_hint = "H_STATISTICS";
  spec.tabs = page.tab_labels();
  spec.active_tab = int(tab);
  spec.show_tab_bumpers = true;
  spec.open_progress = open_progress;

  spec.status_kicker = !s.level_name.empty() ? font_text(s.level_name) : loc_str("Career", "IDS_Wristband", "WRISTBAND");
  spec.status_value = ui4::format_credits(credits);
  spec.status_tag = "RANK #" + std::to_string(std::clamp(s.popularity_rank, 1, 250));
  spec.status_color = !s.level_name.empty() ? ui4::wristband_colour(s.level_name) : ui4::kPink;
  spec.max_visible = 8;

  if (tab == CareerTab::kProgress) {
    spec.list_header = loc_str("Career", "IDS_FestivalProgress", "FESTIVAL & EXPLORATION PROGRESS");
    spec.list_header_right = std::to_string(s.wristband_points) + " PTS";
    spec.selected_index = int(page.selected_index());

    auto add_prog_row = [&](std::string lbl, std::string sub, int cur, int tot, uint32_t col,
                            std::string val_override = {}) {
      ui4::BladeRow r;
      r.label = std::move(lbl);
      r.subtitle = std::move(sub);
      r.has_slider = (tot > 0);
      r.slider_norm = tot > 0 ? std::clamp(float(cur) / float(tot), 0.0f, 1.0f) : 0.0f;
      r.slider_color = col;
      r.value = val_override.empty() ? (std::to_string(cur) + " / " + std::to_string(tot)) : std::move(val_override);
      r.value_color = (tot > 0 && cur >= tot) ? ui4::kGreen : col;
      spec.rows.push_back(std::move(r));
    };

    const std::string wb_lbl = !s.level_name.empty() ? font_text(s.level_name) : "ENTRY WRISTBAND";
    add_prog_row(loc_str("Career", "IDS_Wristband", "WRISTBAND"), wb_lbl, std::max(0, s.level + 1), 7,
                 ui4::wristband_colour(wb_lbl), std::to_string(s.wristband_points) + " PTS");
    const int rank_climbed = std::clamp(250 - std::clamp(s.popularity_rank, 1, 250), 0, 249);
    add_prog_row(loc_str("Career", "IDS_Popularity", "POPULARITY RANK"), "CLIMB TO FESTIVAL #1", rank_climbed, 249,
                 ui4::kYellow, "#" + std::to_string(std::clamp(s.popularity_rank, 1, 250)));
    add_prog_row(loc_str("Career", "IDS_EventsWon", "EVENTS WON"),
                 std::to_string(s.podiums) + " PODIUMS  •  " + std::to_string(s.races) + " RACES", s.victories, 70,
                 ui4::kPink);
    add_prog_row(loc_str("Career", "IDS_Roads", "ROADS DISCOVERED"),
                 pct_str(s.roads_discovered, std::max(1, s.roads_total)) + " OF COLORADO DRIVEN", s.roads_discovered,
                 std::max(1, s.roads_total), ui4::kGreen);
    add_prog_row(loc_str("Career", "IDS_DiscountSigns", "UPGRADE DISCOUNT SIGNS"),
                 std::to_string(std::min(100, s.signs_found)) + "% UPGRADE DISCOUNT", s.signs_found,
                 std::max(1, s.signs_total), ui4::kAmber);
    add_prog_row(loc_str("Career", "IDS_BarnFinds", "BARN FINDS"), "RESTORED CLASSIC CARS", s.barns_found,
                 std::max(1, s.barns), ui4::kAmber);
    add_prog_row(loc_str("Career", "IDS_Outposts", "HORIZON OUTPOSTS"),
                 std::to_string(s.pr_stunts_done) + " / " + std::to_string(std::max(1, s.pr_stunts_total)) + " PR STUNTS",
                 s.outposts_discovered, std::max(1, s.outposts_total), ui4::kBlue);

    spec.detail_header = loc_str("Career", "IDS_ProgressOverview", "CAREER OVERVIEW");
    spec.detail_title = wb_lbl;
    spec.detail_title_color = ui4::wristband_colour(wb_lbl);
    spec.detail_kvs = {
        {"POPULARITY RANK", "#" + std::to_string(std::clamp(s.popularity_rank, 1, 250)), ui4::kYellow},
        {"TOTAL WINNINGS", ui4::format_credits(s.total_winnings), ui4::kGreen},
        {"CARS IN GARAGE", std::to_string(s.garage_cars), ui4::kWhite},
        {"ROADS DRIVEN", std::to_string(s.roads_discovered) + " / " + std::to_string(std::max(1, s.roads_total)),
         ui4::kWhite},
        {"DISCOUNT SIGNS", std::to_string(s.signs_found) + " / " + std::to_string(std::max(1, s.signs_total)),
         ui4::kAmber},
        {"BARN FINDS", std::to_string(s.barns_found) + " / " + std::to_string(std::max(1, s.barns)), ui4::kAmber},
        {"OUTPOSTS", std::to_string(s.outposts_discovered) + " / " + std::to_string(std::max(1, s.outposts_total)),
         ui4::kWhite},
    };
  } else if (tab == CareerTab::kSponsorChallenges) {
    spec.list_header = loc_str("PauseMenu", "IDS_SponsorChallenges", "SPONSOR CHALLENGES");
    spec.list_header_right = std::to_string(sponsors.size()) + " CHALLENGES";
    spec.selected_index = page.sponsor_selected();
    spec.empty_message = "NO SPONSOR CHALLENGES LOADED FOR THIS PROFILE.";

    for (size_t i = 0; i < sponsors.size(); ++i) {
      const auto& sc = sponsors[i];
      ui4::BladeRow r;
      r.label = font_text(sc.challenge_title);
      r.subtitle = font_text(sc.sponsor_name) + "  •  TIER " + std::to_string(sc.tier) + " / " +
                   std::to_string(std::max(1, sc.max_tiers));
      r.has_slider = true;
      r.slider_norm = sc.target > 0 ? std::clamp(float(sc.current) / float(sc.target), 0.0f, 1.0f) : 0.0f;
      r.slider_color = ui4::kYellow;
      r.value = std::to_string(sc.current) + " / " + std::to_string(std::max(1, sc.target));
      r.value_color = (sc.current >= sc.target && sc.target > 0) ? ui4::kGreen : ui4::kYellow;
      r.badge_tag = "+" + ui4::format_credits(sc.reward_cr);
      r.badge_color = ui4::kAmber;
      spec.rows.push_back(std::move(r));
    }

    spec.detail_header = loc_str("Career", "IDS_SponsorDetails", "SPONSOR PAYOUT");
    if (!sponsors.empty()) {
      const auto& sel_sc = sponsors[size_t(std::clamp(page.sponsor_selected(), 0, int(sponsors.size()) - 1))];
      spec.detail_title = font_text(sel_sc.sponsor_name);
      spec.detail_title_color = ui4::kPink;
      spec.detail_paragraphs = {font_text(sel_sc.challenge_title)};
      spec.detail_kvs = {
          {"CURRENT TIER", std::to_string(sel_sc.tier) + " / " + std::to_string(std::max(1, sel_sc.max_tiers)),
           ui4::kWhite},
          {"PROGRESS", std::to_string(sel_sc.current) + " / " + std::to_string(std::max(1, sel_sc.target)),
           ui4::kYellow},
          {"TIER REWARD", ui4::format_credits(sel_sc.reward_cr), ui4::kGreen},
      };
      spec.detail_progress =
          sel_sc.target > 0 ? std::clamp(float(sel_sc.current) / float(sel_sc.target), 0.0f, 1.0f) : 0.0f;
      spec.detail_progress_label = "NEXT SPONSOR TIER";
      spec.detail_progress_color = ui4::kYellow;
    } else {
      spec.detail_title = "SPONSOR CHALLENGES";
      spec.detail_paragraphs = {"Perform drifts, near misses, speed feats, and air skills in Colorado to earn Credit payouts from Horizon sponsors."};
    }
  } else if (tab == CareerTab::kStats) {
    spec.list_header = loc_str("Career", "IDS_DrivingStats", "DRIVING & CAREER STATISTICS");
    spec.list_header_right = std::to_string(s.races) + " RACES";
    spec.selected_index = int(page.selected_index());
    const int win_pct = s.races > 0 ? int(std::lround(100.0 * double(s.victories) / double(s.races))) : 0;

    auto add_stat = [&](std::string lbl, std::string val, uint32_t col = ui4::kWhite) {
      ui4::BladeRow r;
      r.label = std::move(lbl);
      r.value = std::move(val);
      r.value_color = col;
      spec.rows.push_back(std::move(r));
    };
    add_stat("RACES COMPLETED", std::to_string(s.races));
    add_stat("RACE VICTORIES", std::to_string(s.victories) + " (" + std::to_string(win_pct) + "%)", ui4::kYellow);
    add_stat("PODIUM FINISHES", std::to_string(s.podiums));
    add_stat("TOTAL CAREER WINNINGS", ui4::format_credits(s.total_winnings), ui4::kGreen);
    add_stat("CURRENT CREDITS", ui4::format_credits(credits), ui4::kOffWhite);
    add_stat("CARS IN GARAGE", std::to_string(s.garage_cars));
    add_stat("PR STUNTS COMPLETED",
             std::to_string(s.pr_stunts_done) + " / " + std::to_string(std::max(1, s.pr_stunts_total)));
    if (s.club_total > 0) {
      add_stat("1000 CLUB MEDALS", std::to_string(s.club_done) + " / " + std::to_string(s.club_total), ui4::kAmber);
    }
    if (s.has_rally) {
      add_stat("RALLY VICTORIES / STAGES",
               std::to_string(s.rally_victories) + " / " + std::to_string(s.rally_stages), ui4::kAmber);
    }

    spec.detail_header = loc_str("Career", "IDS_DriverRecord", "DRIVER RECORD");
    spec.detail_title = "COLORADO STATS";
    spec.detail_title_color = ui4::kPink;
    spec.detail_kvs = {
        {"WIN RATE", std::to_string(win_pct) + "%", ui4::kYellow},
        {"WRISTBAND PTS", std::to_string(s.wristband_points), ui4::kWhite},
        {"GARAGE SIZE", std::to_string(s.garage_cars) + " CARS", ui4::kWhite},
        {"1000 CLUB CARS", std::to_string(s.club_cars), ui4::kAmber},
    };
  } else {
    // 4th sub-tab: Compact installed expansion / DLC content pack status.
    spec.list_header = loc_str("Career", "IDS_ContentPacks", "CONTENT PACKS");
    spec.list_header_right = std::to_string(page.packs_owned()) + " / " + std::to_string(page.packs_total()) + " OWNED";
    spec.selected_index = int(page.selected_index());
    spec.empty_message = "NO EXPANSION OR CAR PACKS DETECTED.";

    for (const auto& p : page.packs()) {
      ui4::BladeRow r;
      r.label = font_text(p.name);
      r.value = p.owned() ? "OWNED" : (!p.installed ? "NOT INSTALLED" : (!p.enabled ? "DISABLED" : "NO HEADER"));
      r.value_color = p.owned() ? ui4::kGreen : ui4::kDim;
      r.badge_icon_kind = p.owned() ? int(ui4::BadgeIconKind::kComplete) : int(ui4::BadgeIconKind::kLocked);
      spec.rows.push_back(std::move(r));
    }

    spec.detail_header = "INSTALLED PACKAGES";
    spec.detail_title = std::to_string(page.packs_owned()) + " / " + std::to_string(page.packs_total()) + " ACTIVE";
    spec.detail_title_color = page.packs_owned() > 0 ? ui4::kGreen : ui4::kAmber;
    spec.detail_paragraphs = {
        "Installed Forza Horizon expansion and car packs detected in your state root appear here.",
    };
    spec.detail_kvs = {
        {"HORIZON RALLY", s.rally_owned ? "OWNED" : "NOT OWNED", s.rally_owned ? ui4::kGreen : ui4::kDim},
        {"1000 CLUB", s.club_total > 0 ? "ACTIVE" : "READY", ui4::kAmber},
    };
  }

  spec.prompts = {
      {"LB", loc_str("HelpButtons", "IDS_PrevTab", "PREV TAB")},
      {"RB", loc_str("HelpButtons", "IDS_NextTab", "NEXT TAB")},
      {"B", loc_str("HelpButtons", "IDS_Back", "BACK")},
  };
  ui4::tilted_blade_screen(l, w, h, spec);
}

}  // namespace carview
