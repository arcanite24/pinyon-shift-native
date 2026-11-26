// carview: the 3D Festival Autoshow (SPEC-AUTOSHOW-PAINT).
// Supports the Make Carousel ("All Affordable" + alphabetical List_CarMake),
// 2-car tutorial first-purchase flow (cars 1323 & 1009 on new career),
// live 3D car preview, factory Combo_Colors swatch bar picker, and full car
// spec / 5-stat bar data.
#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "cars/shader_settings.h"
#include "garage/garage.h"

namespace carview {

struct AutoshowSwatch {
  int64_t id = 0;
  int64_t sequence = 0;
  uint32_t rgb = 0;
  bool metallic = false;
  bool has_stripe = false;
  uint32_t stripe_rgb = 0;
};

// Open-engine extra finish presets available alongside authentic factory paint swatches.
enum class AutoshowPaintFinish {
  kFactory = 0,
  kGloss,
  kMetallic,
  kMatte,
  kTwoTone,
  kChrome,
  kCarbon,
  kCount
};

const char* autoshow_paint_finish_name(AutoshowPaintFinish f);

struct AutoshowCaliperOption {
  const char* name = "STOCK";
  bool active = false;
  uint32_t rgb = 0;
};

// Baseline stats of the player's currently driven car for live Autoshow spec comparison.
struct AutoshowComparisonStats {
  bool valid = false;
  int64_t car_id = 0;
  std::string display_name;
  int class_id = 0;
  int shown_pi = 100;
  double raw_pi = 0.0;
  int drive_type_id = 2;
  double speed_rating = 5.0;
  double handling_rating = 5.0;
  double accel_rating = 5.0;
  double launch_rating = 5.0;
  double braking_rating = 5.0;
  double peak_power_kw = 0.0;
  double peak_torque_nm = 0.0;
  double curb_weight_kg = 0.0;
};

// Signed delta (candidate - current) used by the right-docked Spec Comparison card.
struct AutoshowSpecDelta {
  bool has_baseline = false;
  int pi_delta = 0;
  double speed_delta = 0.0;
  double handling_delta = 0.0;
  double accel_delta = 0.0;
  double launch_delta = 0.0;
  double braking_delta = 0.0;
  double power_kw_delta = 0.0;
  double torque_nm_delta = 0.0;
  double weight_kg_delta = 0.0;
};

struct AutoshowCar {
  cars::DealerCar dealer;
  int64_t make_id = 0;
  std::string make_name;
  std::string display_name;
  int drive_type_id = 2;  // 1=FWD, 2=RWD, 3=AWD
  double speed_rating = 5.0;
  double handling_rating = 5.0;
  double accel_rating = 5.0;
  double launch_rating = 5.0;
  double braking_rating = 5.0;
  double peak_power_kw = 0.0;
  double peak_torque_nm = 0.0;
  double curb_weight_kg = 0.0;
  std::vector<AutoshowSwatch> swatches;  // ordered by Sequence
};

struct AutoshowMake {
  int64_t make_id = -1;  // -1 = "All Affordable", -2 = "First Car (Tutorial)", -3 = "Hidden", -4 = "All Cars"
  std::string name;
  size_t car_count = 0;
};

enum class AutoshowSort {
  kDefault = 0,
  kPrice,
  kYear,
  kPi,
  kName,
  kCount
};

const char* autoshow_sort_name(AutoshowSort s);

// Authentic 3-step FH1 Autoshow flow:
// 115_C_BUY_MFRSELECT -> 108_C_BUY_CARSELECT -> 114_C_BUY_BUYCAR_COLOR
enum class AutoshowStep {
  kMakeSelect = 0,
  kCarSelect = 1,
  kColorSelect = 2,
};

class Autoshow {
 public:
  // Opens the Autoshow. `baked`: lower-case media names (if empty, lists all
  // database cars so tests & full catalog browsing work). When `tutorial_first`
  // is true (new career with 0 purchases), restricts to the 2 first-purchase
  // cars (1323 and 1009, SPEC-AUTOSHOW-PAINT).
  bool open(const cars::PartsDb& db, const std::set<std::string>& baked, std::string& why,
            int64_t credits = 1000000000, bool tutorial_first = false,
            bool include_hidden = false, double price_multiplier = 1.0,
            const garage::Garage* owned_garage = nullptr);
  void close() {
    open_ = false;
    buy_confirm_open_ = false;
  }
  bool is_open() const { return open_; }

  // Authentic 3-step flow navigation (115 -> 108 -> 114).
  AutoshowStep step() const { return step_; }
  void set_step(AutoshowStep s) {
    step_ = s;
    if (s != AutoshowStep::kColorSelect) buy_confirm_open_ = false;
  }
  // Advances step (Make -> Car -> Color); returns true if already on Color (ready to buy).
  bool advance_step();
  // Steps back (Color -> Car -> Make); returns false if already at first step (caller should close).
  bool back_step();
  // Current breadcrumb path ("MANUFACTURERS" -> "<MAKE>" -> "SELECT PAINT" -> "CONFIRM PURCHASE").
  std::string breadcrumb_text() const;

  // Make Carousel navigation ("All Affordable", "All Cars", + alphabetical localized List_CarMake).
  void move_make(int by, int64_t credits);
  void page_move_make(int pages, int64_t credits, int page_size = 6);
  void select_make(size_t index, int64_t credits);
  void toggle_all_cars(int64_t credits);
  bool all_cars_mode() const { return all_cars_mode_; }
  const std::vector<AutoshowMake>& makes() const { return makes_; }
  size_t selected_make_index() const { return make_index_; }
  // The original's manufacturer strip (115_C_BUY_MFRSELECT): the makes, and
  // the engine's Hidden & Showcase tab after them. "All Affordable" is its Y
  // (select_affordable); "All Cars" is the car select's SEARCH.
  std::vector<size_t> strip_makes() const;
  // Moves along the strip (wrapping); off the strip (after Y), `by` 0 returns
  // to the strip's last make.
  void move_strip_make(int by, int64_t credits);
  void select_affordable(int64_t credits);
  bool tutorial_mode() const { return tutorial_mode_; }
  // When set, open() lists only the Data_Car ids it accepts (Horizon Rally's first
  // Autoshow); the caller clears it.
  std::function<bool(int64_t)> only_cars;
  const std::string& empty_reason() const { return empty_reason_; }

  // Sorting & filtering (Price/Year/PI/Name, Class F..X, Drivetrain FWD/RWD/AWD).
  void cycle_sort(int by = 1);
  void set_sort(AutoshowSort s);
  AutoshowSort sort_mode() const { return sort_; }
  void cycle_class_filter(int by = 1);  // -1 = All, 0..10 = F..X
  void set_class_filter(int cls);
  int class_filter() const { return class_filter_; }
  void cycle_drivetrain_filter(int by = 1);  // 0 = All, 1 = FWD, 2 = RWD, 3 = AWD
  void set_drivetrain_filter(int dt);
  int drivetrain_filter() const { return drive_filter_; }

  // Owned cars badge tracking & baseline current-car comparison stats (synced from CareerGarage).
  void set_owned_cars(const garage::Garage& g);
  void set_owned_car_ids(std::set<int64_t> ids) { owned_car_ids_ = std::move(ids); }
  bool is_owned(int64_t car_id) const { return owned_car_ids_.count(car_id) != 0; }
  void set_comparison_car(const AutoshowComparisonStats& stats) { comparison_car_ = stats; }
  void clear_comparison_car() { comparison_car_ = {}; }
  const AutoshowComparisonStats& comparison_car() const { return comparison_car_; }
  AutoshowSpecDelta compare_car(const AutoshowCar& car, const std::vector<garage::ClassBand>& bands) const;

  // Test Drive hook: returns true and fills media/car_id/colour_seq for the selected car.
  bool prepare_test_drive(std::string& media, int64_t& car_id, int64_t& colour_seq) const;

  // Car list navigation within the active make / filter.
  void move(int by);
  void page_move_car(int pages, int page_size = 5);
  void select_index(size_t idx) {
    if (idx < visible_.size()) {
      selected_ = idx;
      colour_index_ = 0;
      paint_finish_ = AutoshowPaintFinish::kFactory;
      caliper_index_ = 0;
      buy_confirm_open_ = false;
    }
  }
  void jump_class(int by);
  const std::vector<AutoshowCar>& visible_cars() const { return visible_; }
  size_t selected_index() const { return selected_; }
  const AutoshowCar* selected_car() const {
    return selected_ < visible_.size() ? &visible_[selected_] : nullptr;
  }

  // Factory colour swatch bar picker (scrolling window supporting all 1..31+ swatches)
  // plus open-engine extra finish & caliper paint options.
  void move_colour(int by);
  void select_colour_index(size_t idx);
  void swatch_window(size_t max_visible, size_t& first_out, size_t& count_out) const;
  size_t selected_colour_index() const { return colour_index_; }
  int64_t selected_colour_sequence() const;
  const AutoshowSwatch* selected_swatch() const;

  void cycle_paint_finish(int by = 1);
  void set_paint_finish(AutoshowPaintFinish f);
  AutoshowPaintFinish paint_finish() const { return paint_finish_; }
  void cycle_caliper_option(int by = 1);
  void set_caliper_index(size_t idx);
  size_t caliper_index() const { return caliper_index_; }
  static const std::vector<AutoshowCaliperOption>& caliper_options();
  const AutoshowCaliperOption& selected_caliper() const;
  cars::CarPaintSpec preview_paint_spec() const;

  // Spec panel visibility, showroom camera preset cycling, and purchase confirmation guard.
  bool show_specs() const { return show_specs_; }
  void set_show_specs(bool on) { show_specs_ = on; }
  void toggle_specs() { show_specs_ = !show_specs_; }
  int camera_preset() const { return camera_preset_; }
  void set_camera_preset(int preset);
  void cycle_camera_preset();

  bool can_afford_selected(int64_t credits) const;
  int64_t credits_shortfall(int64_t credits) const;
  bool buy_confirm_open() const { return buy_confirm_open_; }
  void set_buy_confirm_open(bool on) { buy_confirm_open_ = on; }
  bool request_buy_confirm(int64_t credits, std::string& message);

  // Buys the selected car in its currently picked factory colour into `g`.
  bool buy(const cars::PartsDb& db, garage::Garage& g, int64_t& credits, std::string& media, std::string& message);

  // Legacy text panel helper (kept for pause-menu compatibility).
  void fill(const std::vector<garage::ClassBand>& bands, int64_t credits, std::string& title,
            std::vector<std::string>& lines, int& selected) const;

 private:
  void rebuild_visible(int64_t credits);
  void reset_paint_overrides();

  std::vector<AutoshowCar> catalog_;
  std::vector<AutoshowMake> makes_;
  std::vector<AutoshowCar> visible_;
  std::vector<AutoshowComparisonStats> garage_meta_;  // full DB metadata for owned cars filtered out of catalog_
  std::set<int64_t> owned_car_ids_;
  AutoshowComparisonStats comparison_car_;
  int64_t last_garage_selected_id_ = -1;
  size_t last_garage_size_ = 0;
  size_t make_index_ = 0;
  size_t strip_make_ = SIZE_MAX;  // the strip's last make (a makes_ index)
  size_t selected_ = 0;
  size_t colour_index_ = 0;
  AutoshowPaintFinish paint_finish_ = AutoshowPaintFinish::kFactory;
  size_t caliper_index_ = 0;
  AutoshowStep step_ = AutoshowStep::kMakeSelect;
  AutoshowSort sort_ = AutoshowSort::kDefault;
  int class_filter_ = -1;
  int drive_filter_ = 0;
  bool all_cars_mode_ = false;
  bool show_specs_ = true;
  bool buy_confirm_open_ = false;
  int camera_preset_ = 0;
  int64_t last_credits_ = 1000000000;
  bool tutorial_mode_ = false;
  std::string empty_reason_;
  bool open_ = false;
};

}  // namespace carview
