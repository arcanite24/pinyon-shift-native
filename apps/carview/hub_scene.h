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
};

const char* hub_room_name(HubRoom room);
HubCameraFocus focus_for_part_enum(int part_enum);

// Smooth turntable camera for 3D Festival Hub screens.
struct HubCamera {
  float yaw = 0.62f;        // radians (front 3/4 view)
  float pitch = 0.17f;      // radians above horizon
  float distance = 4.9f;    // metres from target
  float target[3] = {0.0f, 0.55f, 0.0f};
  float target_yaw = 0.62f;
  float target_pitch = 0.17f;
  float target_dist = 4.9f;
  float target_pos[3] = {0.0f, 0.55f, 0.0f};
  bool auto_orbit = true;
  float orbit_speed = 0.18f;  // rad/s slow showroom turntable rotation

  void set_focus(HubCameraFocus focus);
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

// --- My Garage Browser (3D preview, sort/filter, 50% Sell Car) ---

enum class GarageSort { kClass = 0, kMake, kValue, kDrivetrain, kPi, kCount };
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
};

class GarageBrowser {
 public:
  void refresh(const garage::Garage& g, const cars::PartsDb* db, const std::vector<garage::ClassBand>& bands);
  void move(int by);
  void cycle_sort(int by = 1);
  void cycle_class_filter(int by = 1);  // -1 = All, 1..9 = E..R1
  GarageSort sort_mode() const { return sort_; }
  int class_filter() const { return class_filter_; }

  const std::vector<GarageEntry>& visible() const { return visible_; }
  size_t selected_index() const { return selected_; }
  const GarageEntry* selected_entry() const { return selected_ < visible_.size() ? &visible_[selected_] : nullptr; }

  // Sells the currently selected car for 50% value (refuses active or last car).
  bool sell_selected(garage::Garage& g, const cars::PartsDb* db, const std::vector<garage::ClassBand>& bands,
                     int64_t& credits, std::string& message);

 private:
  void apply_filter_and_sort();
  std::vector<GarageEntry> all_;
  std::vector<GarageEntry> visible_;
  GarageSort sort_ = GarageSort::kClass;
  int class_filter_ = -1;  // -1 = All
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

void draw_autoshow_hud(ui::List& list, float w, float h, const Autoshow& autoshow,
                       const std::vector<garage::ClassBand>& bands, int64_t credits,
                       const std::string& notice = {});

void draw_upgrade_shop_hud(ui::List& list, float w, float h, const UpgradeShop& shop,
                           const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits,
                           const std::string& car_title, const std::string& notice = {});

void draw_garage_hud(ui::List& list, float w, float h, const GarageBrowser& browser,
                     const std::vector<garage::ClassBand>& bands, int64_t credits, const std::string& notice = {});

void draw_barn_find_hud(ui::List& list, float w, float h, const BarnFindCard& card);

// Helper: Forza class badge colour (0xAARRGGBB) matching FH1 class colours (F..X).
uint32_t class_badge_color(int class_id);

}  // namespace carview
