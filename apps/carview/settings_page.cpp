#include "settings_page.h"

#include <algorithm>

namespace carview {

std::vector<SettingsPage::Row> SettingsPage::build() {
  return {
      {"preset", {{"default", {}}, {"low", {"--preset", "low"}}, {"high", {"--preset", "high"}}}},
      {"shadows",
       {{"default", {}}, {"off", {"--shadows", "off"}}, {"low", {"--shadows", "low"}}, {"high", {"--shadows", "high"}}}},
      {"bloom", {{"default", {}}, {"on", {"--bloom"}}, {"off", {"--no-bloom"}}}},
      {"difficulty",
       {{"default", {}},
        {"easy", {"--difficulty", "easy"}},
        {"medium", {"--difficulty", "med"}},
        {"hard", {"--difficulty", "hard"}},
        {"pro", {"--difficulty", "pro"}}}},
      {"rubber-banding", {{"on", {}}, {"off", {"--no-rubberband"}}}},
      {"traffic", {{"on", {}}, {"off", {"--no-traffic"}}}},
      {"collectibles", {{"on", {}}, {"off", {"--no-collectibles"}}}},
      {"radio", {{"on", {}}, {"off", {"--no-radio"}}}},
      {"sound", {{"on", {}}, {"off", {"--no-sound"}}}},
      {"camera",
       {{"default", {}},
        {"chase low", {"--camera", "0"}},
        {"chase high", {"--camera", "1"}},
        {"cockpit", {"--camera", "2"}},
        {"hood", {"--camera", "3"}},
        {"bumper", {"--camera", "4"}}}},
  };
}

void SettingsPage::load(const std::vector<std::string>& tokens) {
  rows_ = build();
  other_.clear();
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string& t = tokens[i];
    const bool has_value = i + 1 < tokens.size() && tokens[i + 1].rfind("--", 0) != 0;
    bool owned = false;
    for (Row& r : rows_)
      for (size_t c = 0; c < r.choices.size(); ++c) {
        const auto& w = r.choices[c].tokens;
        if (w.empty() || w[0] != t) continue;
        if (w.size() == 1 || (has_value && w[1] == tokens[i + 1])) r.pick = c, owned = true;
        else if (w.size() == 2 && has_value) owned = true;  // a value no choice names: the row keeps its pick
      }
    // A token with a value is skipped with it; a row's flag "traffic"
    // (on, the default) is owned by its row too.
    if (!owned && t == "--traffic") owned = true;
    const bool takes_value = has_value && (t == "--preset" || t == "--shadows" || t == "--difficulty" ||
                                           t == "--camera" || t == "--size" || t == "--msaa" || t == "--exposure" ||
                                           t == "--hour" || t == "--play" || t == "--mods");
    if (!owned) {
      other_.push_back(t);
      if (takes_value) other_.push_back(tokens[i + 1]);
    }
    if (takes_value) ++i;
  }
}

void SettingsPage::move(int by) {
  const int n = int(rows_.size()) + 1;
  row_ = ((row_ + by) % n + n) % n;
}

void SettingsPage::step(int by) {
  if (on_save_line()) return;
  Row& r = rows_[size_t(row_)];
  const int n = int(r.choices.size());
  r.pick = size_t(((int(r.pick) + by) % n + n) % n);
}

std::vector<std::string> SettingsPage::tokens() const {
  std::vector<std::string> out = other_;
  for (const Row& r : rows_) {
    const auto& w = r.choices[r.pick].tokens;
    out.insert(out.end(), w.begin(), w.end());
  }
  return out;
}

void SettingsPage::fill(std::string& title, std::vector<std::string>& lines, int& selected) const {
  title = "settings: left/right change, enter on save";
  lines.clear();
  for (const Row& r : rows_) lines.push_back(std::string(r.name) + "  < " + r.choices[r.pick].label + " >");
  lines.push_back("save (applies on the next start)");
  selected = row_;
}

std::string SettingsPage::value(const std::string& name) const {
  for (const Row& r : rows_)
    if (name == r.name) return r.choices[r.pick].label;
  return {};
}

}  // namespace carview
