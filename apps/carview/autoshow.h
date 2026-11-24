// carview: the 3D Festival Autoshow (SPEC-AUTOSHOW-PAINT).
// Supports the Make Carousel ("All Affordable" + alphabetical List_CarMake),
// 2-car tutorial first-purchase flow (cars 1323 & 1009 on new career),
// live 3D car preview, factory Combo_Colors swatch bar picker, and full car
// spec / 5-stat bar data.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "cars/physics.h"
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
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  // Authentic 3-step flow navigation (115 -> 108 -> 114).
  AutoshowStep step() const { return step_; }
  void set_step(AutoshowStep s) { step_ = s; }
  // Advances step (Make -> Car -> Color); returns true if already on Color (ready to buy).
  bool advance_step();
  // Steps back (Color -> Car -> Make); returns false if already at first step (caller should close).
  bool back_step();

  // Make Carousel navigation ("All Affordable", "All Cars", + alphabetical localized List_CarMake).
  void move_make(int by, int64_t credits);
  void select_make(size_t index, int64_t credits);
  void toggle_all_cars(int64_t credits);
  bool all_cars_mode() const { return all_cars_mode_; }
  const std::vector<AutoshowMake>& makes() const { return makes_; }
  size_t selected_make_index() const { return make_index_; }
  bool tutorial_mode() const { return tutorial_mode_; }
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

  // Owned cars badge tracking (synced from CareerGarage).
  void set_owned_cars(const garage::Garage& g);
  void set_owned_car_ids(std::set<int64_t> ids) { owned_car_ids_ = std::move(ids); }
  bool is_owned(int64_t car_id) const { return owned_car_ids_.count(car_id) != 0; }

  // Test Drive hook: returns true and fills media/car_id/colour_seq for the selected car.
  bool prepare_test_drive(std::string& media, int64_t& car_id, int64_t& colour_seq) const;

  // Car list navigation within the active make / filter.
  void move(int by);
  void select_index(size_t idx) {
    if (idx < visible_.size()) {
      selected_ = idx;
      colour_index_ = 0;
    }
  }
  void jump_class(int by);
  const std::vector<AutoshowCar>& visible_cars() const { return visible_; }
  size_t selected_index() const { return selected_; }
  const AutoshowCar* selected_car() const {
    return selected_ < visible_.size() ? &visible_[selected_] : nullptr;
  }

  // Factory colour swatch bar picker (scrolling window supporting all 1..31+ swatches).
  void move_colour(int by);
  void select_colour_index(size_t idx);
  void swatch_window(size_t max_visible, size_t& first_out, size_t& count_out) const;
  size_t selected_colour_index() const { return colour_index_; }
  int64_t selected_colour_sequence() const;
  const AutoshowSwatch* selected_swatch() const;

  // Buys the selected car in its currently picked factory colour into `g`.
  bool buy(const cars::PartsDb& db, garage::Garage& g, int64_t& credits, std::string& media, std::string& message);

  // Legacy text panel helper (kept for pause-menu compatibility).
  void fill(const std::vector<garage::ClassBand>& bands, int64_t credits, std::string& title,
            std::vector<std::string>& lines, int& selected) const;

 private:
  void rebuild_visible(int64_t credits);

  std::vector<AutoshowCar> catalog_;
  std::vector<AutoshowMake> makes_;
  std::vector<AutoshowCar> visible_;
  std::set<int64_t> owned_car_ids_;
  size_t make_index_ = 0;
  size_t selected_ = 0;
  size_t colour_index_ = 0;
  AutoshowStep step_ = AutoshowStep::kMakeSelect;
  AutoshowSort sort_ = AutoshowSort::kDefault;
  int class_filter_ = -1;
  int drive_filter_ = 0;
  bool all_cars_mode_ = false;
  int64_t last_credits_ = 1000000000;
  bool tutorial_mode_ = false;
  std::string empty_reason_;
  bool open_ = false;
};

}  // namespace carview
