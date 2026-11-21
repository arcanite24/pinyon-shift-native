#include "career_page.h"

#include <algorithm>
#include <cstring>

namespace carview {
namespace {

std::string credits_text(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  for (int k = int(s.size()) - 3; k > 0; k -= 3) s.insert(size_t(k), ",");
  return (v < 0 ? "-" : "") + s;
}

}  // namespace

std::string font_text(const std::string& s) {
  static const char* kMarks = "/_-.:[]>&()\\,!? ";
  std::string out;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    (c != 0 && std::strchr(kMarks, c));
    out += ok ? c : ' ';
  }
  return out;
}

void CareerPage::open(const CareerSummary& s, std::vector<cars::PackInfo> packs) {
  lines_.clear();
  lines_.push_back("level  " + (s.level_name.empty() ? std::string("none") : font_text(s.level_name)) + "  " +
                   std::to_string(s.wristband_points) + " points");
  lines_.push_back("credits  " + credits_text(s.credits) + " CR");
  lines_.push_back("total winnings  " + credits_text(s.total_winnings) + " CR");
  lines_.push_back("races " + std::to_string(s.races) + "  wins " + std::to_string(s.victories) + "  podiums " +
                   std::to_string(s.podiums));
  lines_.push_back("garage  " + std::to_string(s.garage_cars) + " cars");
  lines_.push_back("1000 club  " + std::to_string(s.club_cars) + " cars  " + std::to_string(s.club_done) + "/" +
                   std::to_string(s.club_total) + " challenges");
  if (s.has_rally) {
    lines_.push_back("rally  level " + std::to_string(s.rally_level) + "  " + std::to_string(s.rally_xp) + " xp" +
                     (s.rally_owned ? "" : "  (pack not owned)"));
    lines_.push_back("rally  " + std::to_string(s.rallies) + " events  " + std::to_string(s.rally_stages) +
                     " stages  " + std::to_string(s.rally_victories) + " wins  " + credits_text(s.rally_winnings) + " CR");
  }
  owned_ = 0, packs_ = int(packs.size());
  for (const auto& p : packs) owned_ += p.owned();
  lines_.push_back("packs owned  " + std::to_string(owned_) + "/" + std::to_string(packs_));
  for (const auto& p : packs)
    lines_.push_back("  " + font_text(p.name) +
                     (p.owned() ? "" : !p.installed ? "  (not installed)" : !p.enabled ? "  (disabled)" : "  (no header)"));
  selected_ = 0;
  open_ = true;
}

void CareerPage::move(int by) {
  if (lines_.empty()) return;
  const int n = int(lines_.size());
  selected_ = size_t(std::clamp(int(selected_) + by, 0, n - 1));
}

void CareerPage::fill(std::string& title, std::vector<std::string>& lines, int& selected) const {
  if (!open_) return;
  title = "career";
  // A window of rows around the selection, as the Autoshow's.
  constexpr int kRows = 7;
  const int n = int(lines_.size());
  const int first = std::clamp(int(selected_) - kRows / 2, 0, std::max(0, n - kRows));
  lines.assign(lines_.begin() + first, lines_.begin() + std::min(n, first + kRows));
  selected = int(selected_) - first;
}

}  // namespace carview
