#include "autoshow.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

#include "game_hud.h"

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

std::string col_text(sqlite3_stmt* st, int col) {
  const auto* p = sqlite3_column_text(st, col);
  return p ? reinterpret_cast<const char*>(p) : "";
}

std::string pretty_make(const std::string& icon_base) {
  if (icon_base.empty()) return "Other";
  std::string out;
  for (size_t i = 0; i < icon_base.size(); ++i) {
    const char c = icon_base[i];
    if (c == '_') {
      out += ' ';
      continue;
    }
    if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
        std::islower(static_cast<unsigned char>(icon_base[i - 1]))) {
      out += ' ';
    }
    out += c;
  }
  return out;
}

}  // namespace

bool Autoshow::open(const cars::PartsDb& db, const std::set<std::string>& baked, std::string& why,
                    int64_t credits, bool tutorial_first) {
  open_ = false;
  tutorial_mode_ = tutorial_first;
  catalog_.clear();
  makes_.clear();
  visible_.clear();
  empty_reason_.clear();

  std::vector<cars::DealerCar> all;
  if (!db.dealer(all, &why)) return false;

  // Load List_CarMake readable names.
  std::map<int64_t, std::string> make_names;
  db.each("SELECT ID, IconPathBase, ManufacturerCode FROM List_CarMake", {},
          [&](sqlite3_stmt* st) {
            const int64_t id = sqlite3_column_int64(st, 0);
            std::string name = pretty_make(col_text(st, 1));
            if (name.empty() || name == "Other") name = col_text(st, 2);
            make_names[id] = name;
          },
          nullptr);

  // Load Data_Car extra display & rating fields.
  struct CarMeta {
    int64_t make_id = 0;
    std::string display_name;
    int drive_type = 2;
    double speed = 5, handling = 5, accel = 5, launch = 5, braking = 5;
    double power_kw = 0, torque_nm = 0, weight_kg = 0;
  };
  std::map<int64_t, CarMeta> metas;
  db.each(
      "SELECT Id, MakeID, DisplayName, DriveTypeID, SpeedRating, HandlingRating, AccelerationRating, "
      "LaunchRating, BrakingRating, SimPeakPower, SimPeakTorque, CurbWeight FROM Data_Car",
      {},
      [&](sqlite3_stmt* st) {
        CarMeta m;
        const int64_t id = sqlite3_column_int64(st, 0);
        m.make_id = sqlite3_column_int64(st, 1);
        m.display_name = col_text(st, 2);
        m.drive_type = sqlite3_column_int(st, 3);
        m.speed = sqlite3_column_double(st, 4);
        m.handling = sqlite3_column_double(st, 5);
        m.accel = sqlite3_column_double(st, 6);
        m.launch = sqlite3_column_double(st, 7);
        m.braking = sqlite3_column_double(st, 8);
        // FH1 stores SimPeakPower in 100 W (0.1 kW), SimPeakTorque in 100 Nm, CurbWeight in 100 kg.
        m.power_kw = sqlite3_column_double(st, 9) * 0.1;
        m.torque_nm = sqlite3_column_double(st, 10) * 100.0;
        m.weight_kg = sqlite3_column_double(st, 11) * 100.0;
        metas[id] = std::move(m);
      },
      nullptr);

  // Load factory Combo_Colors swatches & stripe exceptions.
  std::map<int64_t, std::vector<AutoshowSwatch>> swatches_by_car;
  db.each("SELECT ID, Ordinal, Sequence, Metallic, RGB FROM Combo_Colors ORDER BY Ordinal, Sequence, ID", {},
          [&](sqlite3_stmt* st) {
            AutoshowSwatch sw;
            sw.id = sqlite3_column_int64(st, 0);
            const int64_t car_id = sqlite3_column_int64(st, 1);
            sw.sequence = sqlite3_column_int64(st, 2);
            sw.metallic = sqlite3_column_int(st, 3) != 0;
            sw.rgb = uint32_t(sqlite3_column_int64(st, 4));
            swatches_by_car[car_id].push_back(sw);
          },
          nullptr);
  db.each("SELECT Ordinal, Sequence, HasStripe, ColorStripe FROM Combo_Colors_Exceptions WHERE HasStripe != 0", {},
          [&](sqlite3_stmt* st) {
            auto it = swatches_by_car.find(sqlite3_column_int64(st, 0));
            if (it == swatches_by_car.end()) return;
            const int64_t seq = sqlite3_column_int64(st, 1);
            const uint32_t col = uint32_t(sqlite3_column_int64(st, 3));
            for (auto& sw : it->second) {
              if (sw.sequence == seq && !sw.has_stripe) {
                sw.has_stripe = true;
                sw.stripe_rgb = col;
              }
            }
          },
          nullptr);

  for (auto& c : all) {
    if (c.base_cost <= 0) continue;
    if (!baked.empty() && !baked.count(lower(c.media_name))) continue;
    // SPEC-AUTOSHOW-PAINT: 2-car tutorial list (ids 1323 and 1009).
    if (tutorial_mode_ && c.id != 1323 && c.id != 1009) continue;

    AutoshowCar entry;
    entry.dealer = c;
    if (auto mit = metas.find(c.id); mit != metas.end()) {
      entry.make_id = mit->second.make_id;
      entry.display_name = mit->second.display_name;
      entry.drive_type_id = mit->second.drive_type;
      entry.speed_rating = mit->second.speed;
      entry.handling_rating = mit->second.handling;
      entry.accel_rating = mit->second.accel;
      entry.launch_rating = mit->second.launch;
      entry.braking_rating = mit->second.braking;
      entry.peak_power_kw = mit->second.power_kw;
      entry.peak_torque_nm = mit->second.torque_nm;
      entry.curb_weight_kg = mit->second.weight_kg;
    }
    if (auto mn = make_names.find(entry.make_id); mn != make_names.end()) {
      entry.make_name = mn->second;
    } else {
      entry.make_name = "Other";
    }
    if (auto sw = swatches_by_car.find(c.id); sw != swatches_by_car.end()) {
      entry.swatches = std::move(sw->second);
    }
    catalog_.push_back(std::move(entry));
  }

  // Fallback if tutorial_mode filtered everything (e.g., neither 1323 nor 1009 baked).
  if (catalog_.empty() && tutorial_mode_) {
    tutorial_mode_ = false;
    return open(db, baked, why, credits, false);
  }
  if (catalog_.empty()) {
    why = "no cars to buy";
    return false;
  }

  // Build Make Carousel: "All Affordable" first (or "First Car" in tutorial),
  // followed by all makes with >= 1 buyable car sorted alphabetically.
  if (tutorial_mode_) {
    makes_.push_back({-2, "First Car", catalog_.size()});
  } else {
    size_t affordable = 0;
    for (const auto& c : catalog_)
      if (c.dealer.base_cost <= credits) ++affordable;
    makes_.push_back({-1, "All Affordable", affordable});

    std::map<int64_t, std::pair<std::string, size_t>> by_make;
    for (const auto& c : catalog_) {
      auto& slot = by_make[c.make_id];
      if (slot.first.empty()) slot.first = c.make_name;
      ++slot.second;
    }
    std::vector<AutoshowMake> alpha;
    for (const auto& [mid, info] : by_make) alpha.push_back({mid, info.first, info.second});
    std::sort(alpha.begin(), alpha.end(), [](const AutoshowMake& a, const AutoshowMake& b) {
      return lower(a.name) < lower(b.name);
    });
    makes_.insert(makes_.end(), alpha.begin(), alpha.end());
  }

  make_index_ = 0;
  selected_ = 0;
  colour_index_ = 0;
  rebuild_visible(credits);
  // If "All Affordable" is empty on open, stay open and display "no affordable cars"
  // (user can press Left/Right on Make Carousel to browse all makes).
  open_ = true;
  return true;
}

void Autoshow::rebuild_visible(int64_t credits) {
  visible_.clear();
  empty_reason_.clear();
  if (makes_.empty()) return;
  const AutoshowMake& mk = makes_[std::min(make_index_, makes_.size() - 1)];
  if (mk.make_id == -1) {
    for (const auto& c : catalog_)
      if (c.dealer.base_cost <= credits) visible_.push_back(c);
    if (visible_.empty()) empty_reason_ = "no affordable cars";
  } else if (mk.make_id == -2) {
    visible_ = catalog_;
  } else {
    for (const auto& c : catalog_)
      if (c.make_id == mk.make_id) visible_.push_back(c);
  }
  selected_ = visible_.empty() ? 0 : std::min(selected_, visible_.size() - 1);
  colour_index_ = 0;
}

void Autoshow::move_make(int by, int64_t credits) {
  if (makes_.empty()) return;
  const int n = int(makes_.size());
  make_index_ = size_t(((int(make_index_) + by) % n + n) % n);
  selected_ = 0;
  rebuild_visible(credits);
}

void Autoshow::select_make(size_t index, int64_t credits) {
  if (makes_.empty()) return;
  make_index_ = std::min(index, makes_.size() - 1);
  selected_ = 0;
  rebuild_visible(credits);
}

void Autoshow::move(int by) {
  if (visible_.empty()) return;
  const int n = int(visible_.size());
  selected_ = size_t(((int(selected_) + by) % n + n) % n);
  colour_index_ = 0;
}

void Autoshow::jump_class(int by) {
  if (visible_.empty()) return;
  const int64_t now = visible_[selected_].dealer.class_id;
  if (by > 0) {
    for (size_t k = selected_; k < visible_.size(); ++k)
      if (visible_[k].dealer.class_id != now) {
        selected_ = k;
        colour_index_ = 0;
        return;
      }
    selected_ = 0;
    colour_index_ = 0;
  } else {
    size_t k = selected_;
    while (k > 0 && visible_[k - 1].dealer.class_id == now) --k;
    if (k != selected_) {
      selected_ = k;
      colour_index_ = 0;
      return;
    }
    if (k == 0) k = visible_.size();
    const int64_t before = visible_[k - 1].dealer.class_id;
    while (k > 0 && visible_[k - 1].dealer.class_id == before) --k;
    selected_ = k;
    colour_index_ = 0;
  }
}

void Autoshow::move_colour(int by) {
  const AutoshowCar* c = selected_car();
  if (!c || c->swatches.empty()) return;
  const int n = int(c->swatches.size());
  colour_index_ = size_t(((int(colour_index_) + by) % n + n) % n);
}

const AutoshowSwatch* Autoshow::selected_swatch() const {
  const AutoshowCar* c = selected_car();
  if (!c || c->swatches.empty()) return nullptr;
  return &c->swatches[std::min(colour_index_, c->swatches.size() - 1)];
}

int64_t Autoshow::selected_colour_sequence() const {
  if (const AutoshowSwatch* sw = selected_swatch()) return sw->sequence;
  if (const AutoshowCar* c = selected_car()) return c->dealer.first_colour;
  return 0;
}

bool Autoshow::buy(const cars::PartsDb& db, garage::Garage& g, int64_t& credits, std::string& media,
                   std::string& message) {
  const AutoshowCar* entry = selected_car();
  if (!entry) {
    message = empty_reason_.empty() ? "no car selected" : empty_reason_;
    return false;
  }
  const cars::DealerCar& c = entry->dealer;
  if (credits < c.base_cost) {
    message = "insufficient CR: " + credits_text(c.base_cost) + " CR";
    return false;
  }
  std::map<std::string, int64_t> stock;
  if (!db.stock_slots(c.id, stock, &message)) return false;
  int64_t id = 0;
  const int colour_seq = int(selected_colour_sequence());
  if (!garage::buy_car(g, c.id, c.base_cost, c.performance_index, int(c.class_id), colour_seq, stock, credits, &id))
    return false;
  media = c.media_name;
  message = car_name(c.media_name) + " bought for " + credits_text(c.base_cost) + " CR";
  return true;
}

void Autoshow::fill(const std::vector<garage::ClassBand>& bands, int64_t credits, std::string& title,
                    std::vector<std::string>& lines, int& selected) const {
  lines.clear();
  const std::string mk = makes_.empty() ? "autoshow" : makes_[std::min(make_index_, makes_.size() - 1)].name;
  if (visible_.empty()) {
    title = "autoshow [" + mk + "]  available " + credits_text(credits) + " CR";
    lines.push_back(empty_reason_.empty() ? "no cars" : empty_reason_);
    selected = 0;
    return;
  }
  title = "autoshow [" + mk + "]  " + std::to_string(selected_ + 1) + "/" + std::to_string(visible_.size()) +
          "  available " + credits_text(credits) + " CR";
  const int n = int(visible_.size());
  const int first = std::clamp(int(selected_) - 3, 0, std::max(0, n - 7));
  for (int k = first; k < std::min(n, first + 7); ++k) {
    const auto& c = visible_[size_t(k)].dealer;
    const std::string cls = bands.empty() ? "?" : bands[size_t(garage::class_of(c.performance_index, bands))].name;
    char line[160];
    std::snprintf(line, sizeof line, "%s %d  %s  %s CR", cls.c_str(), garage::shown_index(c.performance_index, bands),
                  car_name(c.media_name).c_str(), credits_text(c.base_cost).c_str());
    lines.push_back(line);
  }
  selected = int(selected_) - first;
}

}  // namespace carview
