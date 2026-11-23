#include "career_page.h"

#include <algorithm>
#include <cstring>

namespace carview {

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
  summary_ = s;
  owned_ = 0, packs_ = int(packs.size());
  for (const auto& p : packs) owned_ += p.owned();
  // The records are on the board (game_hud.h CareerBoard); the list is the packs.
  lines_.push_back("content packs  " + std::to_string(owned_) + "/" + std::to_string(packs_) + " owned");
  for (const auto& p : packs)
    lines_.push_back("  " + font_text(p.name) +
                     (p.owned() ? "" : !p.installed ? "  (not installed)" : !p.enabled ? "  (disabled)" : "  (no header)"));
  selected_ = 0;
  show_sponsors_ = false;
  sponsor_selected_ = 0;
  open_ = true;
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
  title = "career";
  // A window of rows around the selection, as the Autoshow's.
  constexpr int kRows = 7;
  const int n = int(lines_.size());
  const int first = std::clamp(int(selected_) - kRows / 2, 0, std::max(0, n - kRows));
  lines.assign(lines_.begin() + first, lines_.begin() + std::min(n, first + kRows));
  selected = int(selected_) - first;
}

}  // namespace carview
