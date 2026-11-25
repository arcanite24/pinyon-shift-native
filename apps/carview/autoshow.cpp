#include "autoshow.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "drive/trainer.h"
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

std::string resolve_localized_make(const std::string& disp_key, const std::string& icon_base,
                                   const std::string& mfr_code) {
  if (!disp_key.empty()) {
    std::string loc = disp_key.rfind("_&", 0) == 0 ? game_text(disp_key) : disp_key;
    if (!loc.empty() && loc.rfind("_&", 0) != 0) return loc;
  }
  std::string name = pretty_make(icon_base);
  if (name.empty() || name == "Other") name = mfr_code;
  return name.empty() ? std::string("Other") : name;
}

}  // namespace

const char* autoshow_paint_finish_name(AutoshowPaintFinish f) {
  switch (f) {
    case AutoshowPaintFinish::kGloss: return "GLOSS";
    case AutoshowPaintFinish::kMetallic: return "METALLIC";
    case AutoshowPaintFinish::kMatte: return "MATTE";
    case AutoshowPaintFinish::kTwoTone: return "TWO-TONE";
    case AutoshowPaintFinish::kChrome: return "CHROME";
    case AutoshowPaintFinish::kCarbon: return "CARBON";
    case AutoshowPaintFinish::kFactory:
    default: return "FACTORY";
  }
}

const char* autoshow_sort_name(AutoshowSort s) {
  switch (s) {
    case AutoshowSort::kPrice: return "PRICE";
    case AutoshowSort::kYear: return "YEAR";
    case AutoshowSort::kPi: return "PI";
    case AutoshowSort::kName: return "NAME";
    case AutoshowSort::kDefault:
    default: return "CLASS";
  }
}

bool Autoshow::open(const cars::PartsDb& db, const std::set<std::string>& baked, std::string& why,
                    int64_t credits, bool tutorial_first, bool include_hidden, double price_multiplier,
                    const garage::Garage* owned_garage) {
  open_ = false;
  tutorial_mode_ = tutorial_first;
  catalog_.clear();
  makes_.clear();
  visible_.clear();
  empty_reason_.clear();
  sort_ = AutoshowSort::kDefault;
  class_filter_ = -1;
  drive_filter_ = 0;
  all_cars_mode_ = false;
  show_specs_ = true;
  buy_confirm_open_ = false;
  camera_preset_ = 0;
  paint_finish_ = AutoshowPaintFinish::kFactory;
  caliper_index_ = 0;
  comparison_car_ = {};
  last_garage_selected_id_ = -1;
  last_garage_size_ = 0;
  last_credits_ = credits;
  owned_car_ids_.clear();

  std::vector<cars::DealerCar> all;
  if (!db.dealer(all, &why, include_hidden)) return false;

  drive::FestivalPacingConfig pacing;
  pacing.enabled = std::fabs(price_multiplier - 1.0) > 1e-4;
  pacing.price_multiplier = price_multiplier;

  // Load List_CarMake localized display names (falling back to IconPathBase / ManufacturerCode
  // when DisplayName is absent on minimal synthetic DBs).
  std::map<int64_t, std::string> make_names;
  if (!db.each("SELECT ID, COALESCE(DisplayName, ''), IconPathBase, ManufacturerCode FROM List_CarMake", {},
               [&](sqlite3_stmt* st) {
                 const int64_t id = sqlite3_column_int64(st, 0);
                 make_names[id] = resolve_localized_make(col_text(st, 1), col_text(st, 2), col_text(st, 3));
               },
               nullptr)) {
    db.each("SELECT ID, IconPathBase, ManufacturerCode FROM List_CarMake", {},
            [&](sqlite3_stmt* st) {
              const int64_t id = sqlite3_column_int64(st, 0);
              make_names[id] = resolve_localized_make("", col_text(st, 1), col_text(st, 2));
            },
            nullptr);
  }

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
    const drive::HiddenCarInfo* hc = include_hidden ? drive::find_hidden_car(c.id) : nullptr;
    if (hc && c.base_cost <= 0) c.base_cost = hc->fallback_cost;
    if (c.base_cost <= 0) continue;
    c.base_cost = pacing.scale_price(c.base_cost, c.is_dlc);
    if (!baked.empty() && !baked.count(lower(c.media_name))) continue;
    // SPEC-AUTOSHOW-PAINT: 2-car tutorial list (ids 1323 and 1009).
    if (tutorial_mode_ && c.id != 1323 && c.id != 1009) continue;
    // Horizon Rally's first Autoshow (CSetRestrictionsForFirstRallyAutoshow): its cars only.
    if (only_cars && !only_cars(c.id)) continue;

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
    if (hc) {
      entry.display_name = hc->display_name;
      if (entry.make_name.empty()) entry.make_name = hc->make_name;
    }
    if (auto mn = make_names.find(entry.make_id); mn != make_names.end()) {
      entry.make_name = mn->second;
    } else if (entry.make_name.empty()) {
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
    return open(db, baked, why, credits, false, include_hidden, price_multiplier, owned_garage);
  }
  if (catalog_.empty()) {
    why = "no cars to buy";
    return false;
  }

  // Build Make Carousel: "All Affordable" first (or "First Car" in tutorial),
  // followed by "All Cars", optional "Hidden & Showcase" tab when Hidden Cars unlock is active,
  // and all manufacturers with >= 1 buyable car sorted alphabetically.
  if (tutorial_mode_) {
    makes_.push_back({-2, "First Car", catalog_.size()});
  } else {
    size_t affordable = 0;
    size_t hidden_count = 0;
    for (const auto& c : catalog_) {
      if (c.dealer.base_cost <= credits) ++affordable;
      if (drive::is_hidden_car_id(c.dealer.id)) ++hidden_count;
    }
    makes_.push_back({-1, "All Affordable", affordable});
    makes_.push_back({-4, "All Cars", catalog_.size()});
    if (include_hidden && hidden_count > 0) {
      makes_.push_back({-3, "Hidden & Showcase", hidden_count});
    }

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
  if (owned_garage) set_owned_cars(*owned_garage);
  // Retail FH1 enters Autoshow on the Make Carousel (115_C_BUY_MFRSELECT) unless in tutorial mode.
  step_ = (!tutorial_mode_ && makes_.size() > 1) ? AutoshowStep::kMakeSelect : AutoshowStep::kCarSelect;
  rebuild_visible(credits);
  open_ = true;
  return true;
}

bool Autoshow::advance_step() {
  if (step_ == AutoshowStep::kMakeSelect) {
    step_ = AutoshowStep::kCarSelect;
    buy_confirm_open_ = false;
    return false;
  }
  if (step_ == AutoshowStep::kCarSelect) {
    if (selected_car() && !selected_car()->swatches.empty()) {
      step_ = AutoshowStep::kColorSelect;
      buy_confirm_open_ = false;
      return false;
    }
    return true;
  }
  return true;
}

bool Autoshow::back_step() {
  if (buy_confirm_open_) {
    buy_confirm_open_ = false;
    return true;
  }
  if (step_ == AutoshowStep::kColorSelect) {
    step_ = AutoshowStep::kCarSelect;
    return true;
  }
  if (step_ == AutoshowStep::kCarSelect && !tutorial_mode_ && makes_.size() > 1) {
    step_ = AutoshowStep::kMakeSelect;
    return true;
  }
  return false;
}

std::string Autoshow::breadcrumb_text() const {
  if (tutorial_mode_) {
    if (buy_confirm_open_) return "FIRST CAR  >  SELECT PAINT  >  CONFIRM PURCHASE";
    if (step_ == AutoshowStep::kColorSelect) return "FIRST CAR  >  SELECT PAINT";
    return "FIRST CAR SELECTION";
  }
  const std::string mk =
      all_cars_mode_ ? std::string("ALL CARS")
                     : (makes_.empty() ? std::string("ALL")
                                       : makes_[std::min(make_index_, makes_.size() - 1)].name);
  if (step_ == AutoshowStep::kMakeSelect) {
    return "MANUFACTURERS";
  }
  if (step_ == AutoshowStep::kCarSelect) {
    return "MANUFACTURERS  >  " + mk;
  }
  if (buy_confirm_open_) {
    return "MANUFACTURERS  >  " + mk + "  >  SELECT PAINT  >  CONFIRM PURCHASE";
  }
  return "MANUFACTURERS  >  " + mk + "  >  SELECT PAINT";
}

void Autoshow::rebuild_visible(int64_t credits) {
  last_credits_ = credits;
  visible_.clear();
  empty_reason_.clear();
  if (makes_.empty()) return;
  const AutoshowMake& mk = makes_[std::min(make_index_, makes_.size() - 1)];
  auto matches_filters = [&](const AutoshowCar& c) {
    if (class_filter_ >= 0 && int(c.dealer.class_id) != class_filter_) return false;
    if (drive_filter_ > 0 && c.drive_type_id != drive_filter_) return false;
    return true;
  };

  if (all_cars_mode_ || mk.make_id == -4 || mk.make_id == -2) {
    for (const auto& c : catalog_)
      if (matches_filters(c)) visible_.push_back(c);
  } else if (mk.make_id == -1) {
    for (const auto& c : catalog_)
      if (c.dealer.base_cost <= credits && matches_filters(c)) visible_.push_back(c);
    if (visible_.empty() && class_filter_ < 0 && drive_filter_ == 0) empty_reason_ = "no affordable cars";
  } else if (mk.make_id == -3) {
    for (const auto& c : catalog_)
      if (drive::is_hidden_car_id(c.dealer.id) && matches_filters(c)) visible_.push_back(c);
  } else {
    for (const auto& c : catalog_)
      if (c.make_id == mk.make_id && matches_filters(c)) visible_.push_back(c);
  }

  if (visible_.empty() && empty_reason_.empty()) {
    empty_reason_ = "no matching cars";
  }

  if (sort_ != AutoshowSort::kDefault) {
    auto name_of = [](const AutoshowCar& c) {
      std::string n = car_name(c.dealer.media_name);
      if (n.empty()) n = c.display_name;
      return lower(n);
    };
    std::stable_sort(visible_.begin(), visible_.end(), [&](const AutoshowCar& a, const AutoshowCar& b) {
      switch (sort_) {
        case AutoshowSort::kPrice:
          if (a.dealer.base_cost != b.dealer.base_cost) return a.dealer.base_cost < b.dealer.base_cost;
          return a.dealer.performance_index < b.dealer.performance_index;
        case AutoshowSort::kYear:
          if (a.dealer.year != b.dealer.year) return a.dealer.year > b.dealer.year;
          return a.dealer.performance_index > b.dealer.performance_index;
        case AutoshowSort::kPi:
          if (std::fabs(a.dealer.performance_index - b.dealer.performance_index) > 1e-6)
            return a.dealer.performance_index > b.dealer.performance_index;
          return a.dealer.base_cost < b.dealer.base_cost;
        case AutoshowSort::kName:
          return name_of(a) < name_of(b);
        case AutoshowSort::kDefault:
        default:
          return false;
      }
    });
  }

  selected_ = visible_.empty() ? 0 : std::min(selected_, visible_.size() - 1);
  colour_index_ = 0;
  reset_paint_overrides();
}

void Autoshow::reset_paint_overrides() {
  paint_finish_ = AutoshowPaintFinish::kFactory;
  caliper_index_ = 0;
  buy_confirm_open_ = false;
}

void Autoshow::move_make(int by, int64_t credits) {
  if (makes_.empty()) return;
  all_cars_mode_ = false;
  const int n = int(makes_.size());
  make_index_ = size_t(((int(make_index_) + by) % n + n) % n);
  selected_ = 0;
  rebuild_visible(credits);
}

void Autoshow::page_move_make(int pages, int64_t credits, int page_size) {
  if (makes_.empty() || pages == 0) return;
  const int step = std::max(1, page_size) * pages;
  move_make(step, credits);
}

void Autoshow::select_make(size_t index, int64_t credits) {
  if (makes_.empty()) return;
  all_cars_mode_ = false;
  make_index_ = std::min(index, makes_.size() - 1);
  selected_ = 0;
  rebuild_visible(credits);
}

void Autoshow::toggle_all_cars(int64_t credits) {
  if (tutorial_mode_) return;
  all_cars_mode_ = !all_cars_mode_;
  selected_ = 0;
  rebuild_visible(credits);
}

void Autoshow::cycle_sort(int by) {
  const int n = int(AutoshowSort::kCount);
  sort_ = AutoshowSort(((int(sort_) + by) % n + n) % n);
  selected_ = 0;
  rebuild_visible(last_credits_);
}

void Autoshow::set_sort(AutoshowSort s) {
  sort_ = s;
  selected_ = 0;
  rebuild_visible(last_credits_);
}

void Autoshow::cycle_class_filter(int by) {
  static constexpr int kCycle[] = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  constexpr int n = int(sizeof(kCycle) / sizeof(kCycle[0]));
  int idx = 0;
  for (int i = 0; i < n; ++i)
    if (kCycle[i] == class_filter_) idx = i;
  idx = ((idx + by) % n + n) % n;
  class_filter_ = kCycle[idx];
  selected_ = 0;
  rebuild_visible(last_credits_);
}

void Autoshow::set_class_filter(int cls) {
  class_filter_ = (cls < 0 || cls > 10) ? -1 : cls;
  selected_ = 0;
  rebuild_visible(last_credits_);
}

void Autoshow::cycle_drivetrain_filter(int by) {
  constexpr int n = 4;  // 0=All, 1=FWD, 2=RWD, 3=AWD
  drive_filter_ = ((drive_filter_ + by) % n + n) % n;
  selected_ = 0;
  rebuild_visible(last_credits_);
}

void Autoshow::set_drivetrain_filter(int dt) {
  drive_filter_ = (dt >= 1 && dt <= 3) ? dt : 0;
  selected_ = 0;
  rebuild_visible(last_credits_);
}

void Autoshow::set_owned_cars(const garage::Garage& g) {
  // Fast path: avoid heap allocations on high-rate UI/mouse events when garage did not change.
  bool unchanged = (g.cars.size() == last_garage_size_ && g.selected == last_garage_selected_id_ &&
                    !g.cars.empty() && owned_car_ids_.size() <= g.cars.size());
  if (unchanged) {
    for (const auto& c : g.cars) {
      if (owned_car_ids_.count(c.car_id) == 0) {
        unchanged = false;
        break;
      }
    }
  }
  if (unchanged && (comparison_car_.valid || !g.selected_car())) return;

  owned_car_ids_.clear();
  for (const auto& c : g.cars) owned_car_ids_.insert(c.car_id);
  last_garage_size_ = g.cars.size();
  last_garage_selected_id_ = g.selected;

  // Populate live spec comparison baseline from the player's currently active garage car.
  if (const garage::Car* cur = g.selected_car()) {
    AutoshowComparisonStats cmp{};
    cmp.valid = true;
    cmp.car_id = cur->car_id;
    cmp.class_id = cur->class_id;
    cmp.raw_pi = cur->performance_index;
    cmp.shown_pi = int(std::ceil(cur->performance_index * 999.0));
    for (const auto& entry : catalog_) {
      if (entry.dealer.id == cur->car_id) {
        cmp.display_name = car_name(entry.dealer.media_name);
        if (cmp.display_name.empty()) cmp.display_name = entry.display_name;
        cmp.drive_type_id = entry.drive_type_id;
        cmp.speed_rating = entry.speed_rating;
        cmp.handling_rating = entry.handling_rating;
        cmp.accel_rating = entry.accel_rating;
        cmp.launch_rating = entry.launch_rating;
        cmp.braking_rating = entry.braking_rating;
        cmp.peak_power_kw = entry.peak_power_kw;
        cmp.peak_torque_nm = entry.peak_torque_nm;
        cmp.curb_weight_kg = entry.curb_weight_kg;
        break;
      }
    }
    if (cur->cached_stats.peak_power_kw > 0.0) {
      cmp.speed_rating = cur->cached_stats.speed_rating;
      cmp.handling_rating = cur->cached_stats.handling_rating;
      cmp.accel_rating = cur->cached_stats.accel_rating;
      cmp.launch_rating = cur->cached_stats.launch_rating;
      cmp.braking_rating = cur->cached_stats.braking_rating;
      cmp.peak_power_kw = cur->cached_stats.peak_power_kw;
      cmp.peak_torque_nm = cur->cached_stats.peak_torque_nm;
      cmp.curb_weight_kg = cur->cached_stats.curb_weight_kg;
    }
    if (cmp.display_name.empty()) cmp.display_name = "CURRENT CAR";
    comparison_car_ = std::move(cmp);
  }
}

AutoshowSpecDelta Autoshow::compare_car(const AutoshowCar& car,
                                        const std::vector<garage::ClassBand>& bands) const {
  AutoshowSpecDelta out{};
  if (!comparison_car_.valid) return out;
  out.has_baseline = true;
  const int cand_pi = bands.empty() ? int(std::ceil(car.dealer.performance_index * 999.0))
                                    : garage::shown_index(car.dealer.performance_index, bands);
  const int base_pi =
      (comparison_car_.raw_pi > 0.0 && !bands.empty())
          ? garage::shown_index(comparison_car_.raw_pi, bands)
          : comparison_car_.shown_pi;
  out.pi_delta = cand_pi - base_pi;
  out.speed_delta = car.speed_rating - comparison_car_.speed_rating;
  out.handling_delta = car.handling_rating - comparison_car_.handling_rating;
  out.accel_delta = car.accel_rating - comparison_car_.accel_rating;
  out.launch_delta = car.launch_rating - comparison_car_.launch_rating;
  out.braking_delta = car.braking_rating - comparison_car_.braking_rating;
  out.power_kw_delta = car.peak_power_kw - comparison_car_.peak_power_kw;
  out.torque_nm_delta = car.peak_torque_nm - comparison_car_.peak_torque_nm;
  out.weight_kg_delta = car.curb_weight_kg - comparison_car_.curb_weight_kg;
  return out;
}

bool Autoshow::prepare_test_drive(std::string& media, int64_t& car_id, int64_t& colour_seq) const {
  const AutoshowCar* c = selected_car();
  if (!c || c->dealer.media_name.empty()) return false;
  media = c->dealer.media_name;
  car_id = c->dealer.id;
  colour_seq = selected_colour_sequence();
  return true;
}

void Autoshow::move(int by) {
  if (visible_.empty()) return;
  const int n = int(visible_.size());
  selected_ = size_t(((int(selected_) + by) % n + n) % n);
  colour_index_ = 0;
  reset_paint_overrides();
}

void Autoshow::page_move_car(int pages, int page_size) {
  if (visible_.empty() || pages == 0) return;
  const int step = std::max(1, page_size) * pages;
  move(step);
}

void Autoshow::jump_class(int by) {
  if (visible_.empty()) return;
  const int64_t now = visible_[selected_].dealer.class_id;
  if (by > 0) {
    for (size_t k = selected_; k < visible_.size(); ++k)
      if (visible_[k].dealer.class_id != now) {
        selected_ = k;
        colour_index_ = 0;
        reset_paint_overrides();
        return;
      }
    selected_ = 0;
    colour_index_ = 0;
    reset_paint_overrides();
  } else {
    size_t k = selected_;
    while (k > 0 && visible_[k - 1].dealer.class_id == now) --k;
    if (k != selected_) {
      selected_ = k;
      colour_index_ = 0;
      reset_paint_overrides();
      return;
    }
    if (k == 0) k = visible_.size();
    const int64_t before = visible_[k - 1].dealer.class_id;
    while (k > 0 && visible_[k - 1].dealer.class_id == before) --k;
    selected_ = k;
    colour_index_ = 0;
    reset_paint_overrides();
  }
}

void Autoshow::move_colour(int by) {
  const AutoshowCar* c = selected_car();
  if (!c || c->swatches.empty()) return;
  const int n = int(c->swatches.size());
  colour_index_ = size_t(((int(colour_index_) + by) % n + n) % n);
  buy_confirm_open_ = false;
}

void Autoshow::select_colour_index(size_t idx) {
  const AutoshowCar* c = selected_car();
  if (!c || c->swatches.empty()) return;
  colour_index_ = std::min(idx, c->swatches.size() - 1);
  buy_confirm_open_ = false;
}

void Autoshow::swatch_window(size_t max_visible, size_t& first_out, size_t& count_out) const {
  first_out = 0;
  count_out = 0;
  const AutoshowCar* c = selected_car();
  if (!c || c->swatches.empty() || max_visible == 0) return;
  const size_t total = c->swatches.size();
  count_out = std::min(total, max_visible);
  if (total <= count_out) {
    first_out = 0;
    return;
  }
  const size_t sel = std::min(colour_index_, total - 1);
  const size_t half = count_out / 2;
  if (sel <= half) {
    first_out = 0;
  } else if (sel + (count_out - half) >= total) {
    first_out = total - count_out;
  } else {
    first_out = sel - half;
  }
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

void Autoshow::cycle_paint_finish(int by) {
  const int n = int(AutoshowPaintFinish::kCount);
  paint_finish_ = AutoshowPaintFinish(((int(paint_finish_) + by) % n + n) % n);
  buy_confirm_open_ = false;
}

void Autoshow::set_paint_finish(AutoshowPaintFinish f) {
  const int v = std::clamp(int(f), 0, int(AutoshowPaintFinish::kCount) - 1);
  paint_finish_ = AutoshowPaintFinish(v);
  buy_confirm_open_ = false;
}

const std::vector<AutoshowCaliperOption>& Autoshow::caliper_options() {
  static const std::vector<AutoshowCaliperOption> kOpts = {
      {"STOCK", false, 0x000000u},
      {"CRIMSON", true, 0xCC1820u},
      {"YELLOW", true, 0xF2C618u},
      {"COBALT", true, 0x1A6ED8u},
      {"GOLD", true, 0xC89828u},
      {"SILVER", true, 0xC8CDD6u},
      {"OBSIDIAN", true, 0x181A1Eu},
  };
  return kOpts;
}

void Autoshow::cycle_caliper_option(int by) {
  const int n = int(caliper_options().size());
  caliper_index_ = size_t(((int(caliper_index_) + by) % n + n) % n);
  buy_confirm_open_ = false;
}

void Autoshow::set_caliper_index(size_t idx) {
  const auto& opts = caliper_options();
  if (!opts.empty()) caliper_index_ = std::min(idx, opts.size() - 1);
  buy_confirm_open_ = false;
}

const AutoshowCaliperOption& Autoshow::selected_caliper() const {
  const auto& opts = caliper_options();
  return opts[std::min(caliper_index_, opts.size() - 1)];
}

cars::CarPaintSpec Autoshow::preview_paint_spec() const {
  cars::CarPaintSpec spec{};
  if (const AutoshowSwatch* sw = selected_swatch()) {
    spec.body.active = true;
    spec.body.rgb = sw->rgb;
    switch (paint_finish_) {
      case AutoshowPaintFinish::kGloss: spec.body.finish = cars::PaintFinish::kNormal; break;
      case AutoshowPaintFinish::kMetallic: spec.body.finish = cars::PaintFinish::kMetallic; break;
      case AutoshowPaintFinish::kMatte: spec.body.finish = cars::PaintFinish::kMatte; break;
      case AutoshowPaintFinish::kTwoTone:
        spec.body.finish = cars::PaintFinish::kTwoTone;
        spec.body.sec_rgb = sw->has_stripe && sw->stripe_rgb != 0 ? sw->stripe_rgb : 0xDCE4F2u;
        break;
      case AutoshowPaintFinish::kChrome: spec.body.finish = cars::PaintFinish::kChrome; break;
      case AutoshowPaintFinish::kCarbon: spec.body.finish = cars::PaintFinish::kCarbon; break;
      case AutoshowPaintFinish::kFactory:
      default:
        spec.body.finish = sw->metallic ? cars::PaintFinish::kMetallic : cars::PaintFinish::kNormal;
        break;
    }
  }
  const auto& cal = selected_caliper();
  if (cal.active) {
    spec.calipers.active = true;
    spec.calipers.rgb = cal.rgb;
    spec.calipers.finish = cars::PaintFinish::kMetallic;
  }
  return spec;
}

void Autoshow::set_camera_preset(int preset) {
  constexpr int kPresets = 5;
  camera_preset_ = ((preset % kPresets) + kPresets) % kPresets;
}

void Autoshow::cycle_camera_preset() {
  set_camera_preset(camera_preset_ + 1);
}

bool Autoshow::can_afford_selected(int64_t credits) const {
  const AutoshowCar* entry = selected_car();
  return entry != nullptr && credits >= entry->dealer.base_cost;
}

int64_t Autoshow::credits_shortfall(int64_t credits) const {
  const AutoshowCar* entry = selected_car();
  if (!entry || credits >= entry->dealer.base_cost) return 0;
  return entry->dealer.base_cost - credits;
}

bool Autoshow::request_buy_confirm(int64_t credits, std::string& message) {
  const AutoshowCar* entry = selected_car();
  if (!entry) {
    buy_confirm_open_ = false;
    message = empty_reason_.empty() ? "NO CAR SELECTED" : empty_reason_;
    return false;
  }
  if (credits < entry->dealer.base_cost) {
    buy_confirm_open_ = false;
    message = "INSUFFICIENT CREDITS — NEED " + credits_text(entry->dealer.base_cost) + " CR (" +
              credits_text(entry->dealer.base_cost - credits) + " CR SHORT)";
    return false;
  }
  buy_confirm_open_ = true;
  message = "CONFIRM PURCHASE FOR " + credits_text(entry->dealer.base_cost) + " CR";
  return true;
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
    buy_confirm_open_ = false;
    return false;
  }
  std::map<std::string, int64_t> stock;
  if (!db.stock_slots(c.id, stock, &message)) return false;
  int64_t id = 0;
  const int colour_seq = int(selected_colour_sequence());
  if (!garage::buy_car(g, c.id, c.base_cost, c.performance_index, int(c.class_id), colour_seq, stock, credits, &id))
    return false;
  owned_car_ids_.insert(c.id);
  buy_confirm_open_ = false;
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
