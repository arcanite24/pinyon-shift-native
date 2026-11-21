#include "upgrade_shop.h"

#include <algorithm>
#include <cstdio>

namespace carview {

namespace {

// The slot's name for the list: its Career_Garage column, spaced.
std::string slot_name(const char* column) {
  std::string out;
  for (const char* c = column; *c; ++c) {
    if (c != column && *c >= 'A' && *c <= 'Z' && !(c[-1] >= 'A' && c[-1] <= 'Z')) out += ' ';
    out += char(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c);
  }
  return out;
}

std::string credits_text(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  for (int k = int(s.size()) - 3; k > 0; k -= 3) s.insert(size_t(k), ",");
  return (v < 0 ? "-" : "") + s;
}

}  // namespace

bool UpgradeShop::open(const cars::PartsDb& db, garage::Garage& g, int64_t garage_id, std::string& why) {
  open_ = false, changed_ = false;
  slots_.clear();
  const garage::Car* car = g.find(garage_id);
  if (!car) {
    why = "no garage car " + std::to_string(garage_id);
    return false;
  }
  std::vector<cars::PartRow> rows;
  if (!db.options(car->car_id, garage::fitted(*car), rows, &why)) return false;
  for (const auto& s : cars::upgrade_slots()) {
    // The conversions (engine, drivetrain and body swaps) re-key the
    // sub-parts; the shop leaves them out (interim).
    if (s.part_enum <= 2) continue;
    Slot slot;
    slot.slot = &s;
    for (const auto& r : rows)
      if (r.part_enum == s.part_enum) slot.parts.push_back(r);
    // A slot offers something only with more than its stock row; a slot
    // with no stock row (an induction the engine does not have) is an
    // aspiration conversion, left out unless the car has one fitted.
    if (slot.parts.size() < 2) continue;
    const bool has_stock = std::any_of(slot.parts.begin(), slot.parts.end(), [](const cars::PartRow& r) { return r.is_stock; });
    const auto fitted_now = car->slots.find(s.column);
    if (!has_stock && (fitted_now == car->slots.end() || fitted_now->second < 0)) continue;
    slots_.push_back(std::move(slot));
  }
  if (slots_.empty()) {
    why = "no upgrades for this car";
    return false;
  }
  garage_ = &g, garage_id_ = garage_id;
  for (auto& s : slots_) s.shown = fitted_index(s);
  selected_ = 0;
  open_ = true;
  return true;
}

size_t UpgradeShop::fitted_index(const Slot& s) const {
  const garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
  if (car) {
    const auto it = car->slots.find(s.slot->column);
    if (it != car->slots.end())
      for (size_t k = 0; k < s.parts.size(); ++k)
        if (s.parts[k].id == it->second) return k;
  }
  for (size_t k = 0; k < s.parts.size(); ++k)
    if (s.parts[k].is_stock) return k;
  return 0;
}

void UpgradeShop::move_slot(int by) {
  if (slots_.empty()) return;
  const int n = int(slots_.size());
  selected_ = size_t(((int(selected_) + by) % n + n) % n);
}

void UpgradeShop::move_part(int by) {
  if (slots_.empty()) return;
  Slot& s = slots_[selected_];
  const int n = int(s.parts.size());
  s.shown = size_t(((int(s.shown) + by) % n + n) % n);
}

bool UpgradeShop::fit(int discount_percent, int64_t& credits, std::string& message) {
  if (slots_.empty() || !garage_) return false;
  Slot& s = slots_[selected_];
  const cars::PartRow& part = s.parts[s.shown];
  if (s.shown == fitted_index(s)) {
    message = "already fitted";
    return false;
  }
  const garage::FitResult r = garage::fit_part(*garage_, garage_id_, part, s.slot->column, discount_percent, credits);
  if (!r.ok) {
    message = "not enough CR (" + credits_text(garage::discounted_price(part.price, discount_percent)) + " CR)";
    return false;
  }
  changed_ = true;
  message = slot_name(s.slot->column) + (r.refit ? " refitted" : " bought for " + credits_text(r.charged) + " CR");
  return true;
}

void UpgradeShop::fill(const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits,
                       std::string& title, std::vector<std::string>& lines, int& selected) const {
  lines.clear();
  const garage::Car* car = garage_ ? garage_->find(garage_id_) : nullptr;
  char head[160];
  const double pi = car ? car->performance_index : 0;
  std::snprintf(head, sizeof head, "upgrades  %s %d  %s CR  sign discount %d",
                bands.empty() ? "?" : bands[size_t(garage::class_of(pi, bands))].name.c_str(),
                garage::shown_index(pi, bands), credits_text(credits).c_str(), discount_percent);
  title = head;
  // Seven lines around the selected slot.
  const int n = int(slots_.size());
  const int first = std::clamp(int(selected_) - 3, 0, std::max(0, n - 7));
  for (int k = first; k < std::min(n, first + 7); ++k) {
    const Slot& s = slots_[size_t(k)];
    const cars::PartRow& p = s.parts[s.shown];
    const bool fitted = s.shown == fitted_index(s);
    const bool owned = garage_ && garage_->owns_part(garage_id_, p.part_enum, p.id);
    std::string state = fitted ? "fitted" : p.is_stock ? "stock" : owned ? "owned"
                                                                       : credits_text(garage::discounted_price(p.price, discount_percent)) + " CR";
    char line[160];
    std::snprintf(line, sizeof line, "%s  [%s %d]  %s", slot_name(s.slot->column).c_str(), p.is_stock ? "stock" : "level",
                  p.level, state.c_str());
    lines.push_back(line);
  }
  selected = int(selected_) - first;
}

}  // namespace carview
