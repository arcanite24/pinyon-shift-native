// 3D Festival Hub turntable scene (uiautoshow, uiworkshop / Dak's Garage,
// uipaintshop, My Garage, Barn Find Restoration): studio plinth + drop-shadow
// floor fan geometry, studio lighting & turntable orbit camera, Garage browser
// (sort/filter/sell), Barn Find restoration card, and Forza Horizon 1-styled
// UI overlay builders (ui::List).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "autoshow.h"
#include "cars/physics.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "showroom.h"
#include "ui.h"
#include "upgrade_shop.h"

namespace carview {

enum class HubRoom {
  kNone = 0,
  kAutoshow,   // uiautoshow
  kWorkshop,   // uiworkshop (Dak's Garage / Upgrade Shop & Tuning)
  kPaintShop,  // uipaintshop (Paint & Livery Editor)
  kGarage,     // My Garage (3D preview, sort/filter, Sell Car)
  kBarnFind,   // Barn Find Restoration Card
};

enum class HubCameraFocus {
  kTurntable = 0,
  kFront,
  kRear,
  kWheels,
  kHood,
  kRoof,
  kSide,
  kInterior,
  kPaint,
  kRearThreeQuarter,
};

const char* hub_room_name(HubRoom room);
HubCameraFocus focus_for_part_enum(int part_enum);

// Smooth turntable camera for 3D Festival Hub screens.
struct HubCamera {
  float yaw = 0.62f;        // radians (front 3/4 view)
  float pitch = 0.16f;      // radians above horizon
  float distance = 5.15f;   // metres from target
  float target[3] = {0.0f, 0.54f, 0.0f};
  float target_yaw = 0.62f;
  float target_pitch = 0.16f;
  float target_dist = 5.15f;
  float target_pos[3] = {0.0f, 0.54f, 0.0f};
  float lens_shift_x = 0.0f;
  float lens_shift_y = 0.0f;
  float target_lens_shift_x = 0.0f;
  float target_lens_shift_y = 0.0f;
  bool has_explicit_framing = false;
  bool auto_orbit = true;
  float orbit_speed = 0.16f;      // rad/s slow showroom turntable rotation
  float idle_orbit_delay = 3.5f;  // seconds of idle before resuming gentle auto-orbit
  float manual_idle_s = 0.0f;
  bool user_orbited = false;
  float orbit_blend = 1.0f;

  void set_focus(HubCameraFocus focus);
  void set_framing(float shift_x, float shift_y);
  void nudge_orbit(float dyaw, float dpitch, float ddist = 0.0f);
  void step(float dt);
};

// Builds the 3D studio turntable stage: circular showroom plinth, radial
// drop-shadow floor fan directly underneath the car's footprint, and outer
// studio floor/cyclorama coloured per Festival Hub room.
WorldTriangles build_hub_stage(HubRoom room, float car_half_w = 0.95f, float car_half_l = 2.20f,
                               float floor_y = 0.0f);

// Fills a ShowroomFrame with camera projection & studio lighting tuned for
// `room` (compatible with staticcarcubemap.xpr studio reflections).
void build_hub_frame(HubRoom room, const HubCamera& cam, uint32_t width, uint32_t height, ShowroomFrame& out,
                     bool reverse_z = false);

// Background clear colour (sRGB 0..1) for each Festival Hub room.
std::array<float, 3> hub_background_color(HubRoom room);

// --- My Garage Browser (3D preview, sort/filter, Favorites, 50% / 100 CR Sell Car) ---

enum class GarageSort { kClass = 0, kMake, kValue, kDrivetrain, kPi, kYear, kCount };
const char* garage_sort_name(GarageSort s);

struct GarageEntry {
  int64_t garage_id = 0;
  int64_t car_id = 0;
  std::string media_name;
  std::string display_name;
  std::string make_name;
  int year = 0;
  int class_id = 0;
  double raw_pi = 0.0;
  int shown_pi = 100;
  int drive_type_id = 2;  // 1=FWD, 2=RWD, 3=AWD
  int64_t value_cr = 0;
  int64_t sell_cr = 0;
  int colour_seq = 0;
  bool is_current = false;
  bool favorite = false;
  bool is_prize = false;
  // 0..10 stat ratings & real specs readout (HP/kW, Torque, Weight, Displacement, Aspiration).
  double speed_rating = 5.0;
  double handling_rating = 5.0;
  double accel_rating = 5.0;
  double launch_rating = 5.0;
  double braking_rating = 5.0;
  double peak_power_kw = 0.0;
  double peak_torque_nm = 0.0;
  double curb_weight_kg = 0.0;
  double displacement_cc = 0.0;
  std::string aspiration;
};

class GarageBrowser {
 public:
  void refresh(const garage::Garage& g, const cars::PartsDb* db, const std::vector<garage::ClassBand>& bands);
  void move(int by);
  void select_index(size_t idx) {
    if (idx < visible_.size()) selected_ = idx;
  }
  void cycle_sort(int by = 1);
  void set_sort(GarageSort s) {
    sort_ = s;
    apply_filter_and_sort();
  }
  void cycle_class_filter(int by = 1);  // -1 = All, 0..10 = F..X
  void set_class_filter(int cls) {
    class_filter_ = (cls < 0 || cls > 10) ? -1 : cls;
    selected_ = 0;
    apply_filter_and_sort();
  }
  void toggle_favorites_filter();
  void set_favorites_filter(bool on);
  bool favorites_only() const { return favorites_only_; }
  bool toggle_favorite_selected(garage::Garage& g, const cars::PartsDb* db,
                                const std::vector<garage::ClassBand>& bands);

  GarageSort sort_mode() const { return sort_; }
  int class_filter() const { return class_filter_; }

  const std::vector<GarageEntry>& visible() const { return visible_; }
  size_t selected_index() const { return selected_; }
  const GarageEntry* selected_entry() const { return selected_ < visible_.size() ? &visible_[selected_] : nullptr; }

  // Sells the currently selected car (100 CR for prize/gift cars, 50% for purchased; refuses active or last car).
  bool sell_selected(garage::Garage& g, const cars::PartsDb* db, const std::vector<garage::ClassBand>& bands,
                     int64_t& credits, std::string& message);

 private:
  void apply_filter_and_sort();
  std::vector<GarageEntry> all_;
  std::vector<GarageEntry> visible_;
  GarageSort sort_ = GarageSort::kClass;
  int class_filter_ = -1;  // -1 = All, 0..10 = F..X
  bool favorites_only_ = false;
  size_t selected_ = 0;
};

// --- Barn Find Restoration Card ---
struct BarnFindCard {
  bool visible = false;
  std::string barn_name;
  std::string car_name;
  int64_t car_id = 0;
  std::string class_name = "C";
  int shown_pi = 350;
  int year = 1965;
  float restore_progress = 1.0f;  // 0..1 (1.0 = restored and delivered to garage)
  std::string subtitle = "RESTORED BY DAK AND DELIVERED TO YOUR GARAGE";
};

// --- Forza Horizon 1-styled 3D Festival Hub UI Overlays ---

struct UpgradeSpecStrings {
  std::string power;   // e.g. "151 HP  ->  164 HP" or "113 kW  ->  122 kW"
  std::string torque;  // e.g. "173 LB-FT  ->  184 LB-FT" or "235 NM  ->  250 NM"
  std::string weight;  // e.g. "2,734 LBS  ->  2,734 LBS" or "1,240 KG  ->  1,240 KG"
  std::string perf;    // e.g. "0-60: 7.85s   •   60-0: 73 FT   •   1.54G" or "0-100: 8.13s   •   100-0: 23.9 M   •   1.54G"
};

UpgradeSpecStrings format_upgrade_spec_strings(const garage::CarStats& cur, const garage::CarStats& prv,
                                               bool metric_units);
std::string format_autoshow_spec_line(double peak_power_kw, double peak_torque_nm, double curb_weight_kg,
                                      bool metric_units);

// Non-clipping canonical layout geometry for the Horizon Autoshow HUD.
// Guarantees every UI surface stays strictly outside `hero_car_viewport`.
struct AutoshowRect {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  float width() const { return x1 - x0; }
  float height() const { return y1 - y0; }
  bool valid() const { return x1 > x0 && y1 > y0; }
  bool intersects(const AutoshowRect& o) const {
    return valid() && o.valid() && x0 < o.x1 && x1 > o.x0 && y0 < o.y1 && y1 > o.y0;
  }
};

struct AutoshowLayout {
  float scale = 1.0f;
  AutoshowRect top_bar{};
  AutoshowRect filter_bar{};
  AutoshowRect hero_car_viewport{};
  AutoshowRect spec_card{};
  bool spec_card_visible = false;
  AutoshowRect rail_header{};
  AutoshowRect carousel_rail{};
  AutoshowRect swatch_strip{};
  AutoshowRect finish_strip{};
  AutoshowRect button_bar{};
  AutoshowRect confirm_modal{};
};

AutoshowLayout compute_autoshow_layout(float w, float h, AutoshowStep step, bool show_specs,
                                       bool show_filters);

void draw_autoshow_hud(ui::List& list, float w, float h, const Autoshow& autoshow,
                       const std::vector<garage::ClassBand>& bands, int64_t credits,
                       const std::string& notice = {}, bool metric_units = false);

void draw_upgrade_shop_hud(ui::List& list, float w, float h, const UpgradeShop& shop,
                           const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits,
                           const std::string& car_title, const std::string& notice = {}, bool metric_units = false);

void draw_garage_hud(ui::List& list, float w, float h, const GarageBrowser& browser,
                     const std::vector<garage::ClassBand>& bands, int64_t credits, const std::string& notice = {},
                     bool metric_units = false);

void draw_barn_find_hud(ui::List& list, float w, float h, const BarnFindCard& card);

// Helper: Forza class badge colour (0xAARRGGBB) matching FH1 class colours (F..X).
uint32_t class_badge_color(int class_id);

}  // namespace carview
