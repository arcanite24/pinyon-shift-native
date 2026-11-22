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
  int64_t make_id = -1;  // -1 = "All Affordable", -2 = "First Car (Tutorial)"
  std::string name;
  size_t car_count = 0;
};

class Autoshow {
 public:
  // Opens the Autoshow. `baked`: lower-case media names (if empty, lists all
  // database cars so tests & full catalog browsing work). When `tutorial_first`
  // is true (new career with 0 purchases), restricts to the 2 first-purchase
  // cars (1323 and 1009, SPEC-AUTOSHOW-PAINT).
  bool open(const cars::PartsDb& db, const std::set<std::string>& baked, std::string& why,
            int64_t credits = 1000000000, bool tutorial_first = false);
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  // Make Carousel navigation ("All Affordable" + alphabetical List_CarMake).
  void move_make(int by, int64_t credits);
  void select_make(size_t index, int64_t credits);
  const std::vector<AutoshowMake>& makes() const { return makes_; }
  size_t selected_make_index() const { return make_index_; }
  bool tutorial_mode() const { return tutorial_mode_; }
  const std::string& empty_reason() const { return empty_reason_; }

  // Car list navigation within the active make / filter.
  void move(int by);
  void jump_class(int by);
  const std::vector<AutoshowCar>& visible_cars() const { return visible_; }
  size_t selected_index() const { return selected_; }
  const AutoshowCar* selected_car() const {
    return selected_ < visible_.size() ? &visible_[selected_] : nullptr;
  }

  // Factory colour swatch bar picker for the currently selected car.
  void move_colour(int by);
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
  size_t make_index_ = 0;
  size_t selected_ = 0;
  size_t colour_index_ = 0;
  bool tutorial_mode_ = false;
  std::string empty_reason_;
  bool open_ = false;
};

}  // namespace carview
