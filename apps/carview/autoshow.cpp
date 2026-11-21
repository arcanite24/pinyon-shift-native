#include "autoshow.h"

#include <algorithm>
#include <cstdio>

namespace carview {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return s;
}

std::string credits_text(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  for (int k = int(s.size()) - 3; k > 0; k -= 3) s.insert(size_t(k), ",");
  return (v < 0 ? "-" : "") + s;
}

}  // namespace

bool Autoshow::open(const cars::PartsDb& db, const std::set<std::string>& baked, std::string& why) {
  open_ = false;
  std::vector<cars::DealerCar> all;
  if (!db.dealer(all, &why)) return false;
  cars_.clear();
  for (auto& c : all)
    if (c.base_cost > 0 && baked.count(lower(c.media_name))) cars_.push_back(std::move(c));
  if (cars_.empty()) {
    why = "no cars to buy";
    return false;
  }
  selected_ = std::min(selected_, cars_.size() - 1);
  open_ = true;
  return true;
}

void Autoshow::move(int by) {
  if (cars_.empty()) return;
  const int n = int(cars_.size());
  selected_ = size_t(((int(selected_) + by) % n + n) % n);
}

void Autoshow::jump_class(int by) {
  if (cars_.empty()) return;
  const int64_t now = cars_[selected_].class_id;
  if (by > 0) {
    for (size_t k = selected_; k < cars_.size(); ++k)
      if (cars_[k].class_id != now) {
        selected_ = k;
        return;
      }
    selected_ = 0;
  } else {
    // The first car of the class before this one (or of this class when
    // not on its first car).
    size_t k = selected_;
    while (k > 0 && cars_[k - 1].class_id == now) --k;
    if (k != selected_) {
      selected_ = k;
      return;
    }
    if (k == 0) k = cars_.size();
    const int64_t before = cars_[k - 1].class_id;
    while (k > 0 && cars_[k - 1].class_id == before) --k;
    selected_ = k;
  }
}

bool Autoshow::buy(const cars::PartsDb& db, garage::Garage& g, int64_t& credits, std::string& media,
                   std::string& message) {
  if (cars_.empty()) return false;
  const cars::DealerCar& c = cars_[selected_];
  if (credits < c.base_cost) {
    message = "insufficient CR: " + credits_text(c.base_cost) + " CR";
    return false;
  }
  std::map<std::string, int64_t> stock;
  if (!db.stock_slots(c.id, stock, &message)) return false;
  int64_t id = 0;
  if (!garage::buy_car(g, c.id, c.base_cost, c.performance_index, int(c.class_id), int(c.first_colour), stock, credits,
                       &id))
    return false;
  media = c.media_name;
  message = c.media_name + " bought for " + credits_text(c.base_cost) + " CR";
  return true;
}

void Autoshow::fill(const std::vector<garage::ClassBand>& bands, int64_t credits, std::string& title,
                    std::vector<std::string>& lines, int& selected) const {
  lines.clear();
  title = "autoshow  " + std::to_string(selected_ + 1) + "/" + std::to_string(cars_.size()) + "  available " +
          credits_text(credits) + " CR";
  const int n = int(cars_.size());
  const int first = std::clamp(int(selected_) - 3, 0, std::max(0, n - 7));
  for (int k = first; k < std::min(n, first + 7); ++k) {
    const auto& c = cars_[size_t(k)];
    const std::string cls = bands.empty() ? "?" : bands[size_t(garage::class_of(c.performance_index, bands))].name;
    char line[160];
    std::snprintf(line, sizeof line, "%s %d  %s  %s CR", cls.c_str(), garage::shown_index(c.performance_index, bands),
                  c.media_name.c_str(), credits_text(c.base_cost).c_str());
    lines.push_back(line);
  }
  selected = int(selected_) - first;
}

}  // namespace carview
