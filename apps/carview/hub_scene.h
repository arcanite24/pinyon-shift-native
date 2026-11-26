// 3D Festival Hub turntable scene (uiautoshow, uiworkshop / Dak's Garage,
// uipaintshop, My Garage, Barn Find Restoration): studio plinth + drop-shadow
// floor fan geometry, studio lighting & turntable orbit camera, Garage browser
// (sort/filter/sell), Barn Find restoration card, and Forza Horizon 1-styled
// UI overlay builders (ui::List).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "autoshow.h"
#include "cars/physics.h"
#include "cutscene/cutscene.h"
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

// A camera preset from media/UICarTargetCameraPresets.xml: the eye and its
// look direction in the car's frame (the game's, +Z to the car's front), the
// vertical field of view in degrees, and the orbit limits the engine does
// not apply yet (Q129).
struct CarCameraPreset {
  std::string name;
  int ordinal = -1;  // -1: every car; else one car's own
  float pos[3] = {};
  float face[3] = {};
  float fov = 0.0f;
  bool allow_zoom = false;
};
std::vector<CarCameraPreset> parse_car_camera_presets(std::string_view xml);
// The preset for every car (ordinal -1) by name, or null.
const CarCameraPreset* find_car_camera_preset(const std::vector<CarCameraPreset>& presets, std::string_view name);
// The preset a hub screen uses: the workshop's by the selected part (part
// enum, -1 for AUTO UPGRADE and tuning), the paint shop's main menu or its
// editor, the Autoshow's colour select. Empty: the engine's own camera
// (the Autoshow's standard view and the garage).
std::string_view hub_camera_preset_name(HubRoom room, int part_enum, bool editor_open, bool colour_select);

// MENU-03: the camera a hub screen shows, as the original picks it. A room's
// own lists (the Autoshow's browse, the paint booth's and the workshop's
// menus) play camera.zip's HomeSpaceCams.xml (CUI4AnimatedCamera sequence
// 0); a workshop part plays its WorkshopCams.xml sequence; the paint
// editor's regions and the Autoshow's colour select aim at their
// UICarTargetCameraPresets.xml presets (CPaintMenu, CCarColorSelect).
// Neither: the engine's own camera (the garage, Barn Finds).
struct HubCameraShot {
  std::string_view sequence_member;  // camera.zip member, empty for none
  std::string_view sequence;         // the sequence's name in it ("" for a <Cams> root)
  std::string_view preset;           // a preset's name, empty for none
};
// `paint_region`: the editor's PaintRegion (0 body .. 6 window tint);
// `part_enum`: the upgrade shop's selected part, -1 off its part lists.
HubCameraShot hub_camera_shot(HubRoom room, int part_enum, bool editor_open, int paint_region, bool colour_select);
// The WorkshopCams.xml sequence for an upgrade part (the body parts and the
// wheels), empty for the parts without one.
std::string_view workshop_part_sequence(int part_enum);
// The UICarTargetCameraPresets.xml preset for an engine or drivetrain part
// (Upgrades_Engine_Front, Upgrades_Flywheel, Upgrades_Drivetrain), empty
// for the others.
std::string_view workshop_part_preset(int part_enum);

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
  float fov_deg = 0.0f;  // vertical; 0: the room's own
  float target_fov_deg = 0.0f;

  // MENU-03: CUI4CarTargetCamera's move to a preset is a cubic Hermite
  // (zero end tangents) over 0.75 s, the yaw the short way round; the
  // HorizonAutoshow path snaps. While blend_t < blend_len the orbit runs
  // from the from_* values to the targets.
  static constexpr float kPresetBlendSeconds = 0.75f;
  float blend_t = 0.0f, blend_len = 0.0f;
  float from_yaw = 0.0f, from_pitch = 0.0f, from_dist = 0.0f, from_fov = 0.0f;
  float from_pos[3] = {};

  // MENU-03: an animated sequence (CUI4AnimatedCamera: camera.zip's
  // HomeSpaceCams.xml for the rooms' own screens, WorkshopCams.xml's
  // part sequences), keyed in the car's body axes (+X right, +Y up, +Z
  // forward, origin on the ground), looped. Null: the orbit and presets.
  const cutscene::Cutscene* sequence = nullptr;
  double sequence_t = 0.0;
  cutscene::CarParts parts;  // the PartSpace keys' parts, body axes
  float ground_y = 0.0f;     // the ground point's height in the car model's frame

  void set_focus(HubCameraFocus focus);
  // Aims at the preset: its eye and direction (the car's frame mirrored
  // into the engine's, -Z forward) as yaw, pitch and distance about the
  // ray's nearest point to the car's origin, and its field of view. Blends
  // there from the current view (kPresetBlendSeconds) unless `snap`.
  void set_preset(const CarCameraPreset& p, bool snap = false);
  // Plays `seq` from its start (a cut); the same sequence again keeps its
  // time. Null stops it (the orbit takes over from the sequence's view).
  void play(const cutscene::Cutscene* seq);
  void set_framing(float shift_x, float shift_y);
  void nudge_orbit(float dyaw, float dpitch, float ddist = 0.0f);
  void step(float dt);
  // The view in the car model's frame: the eye, the look direction, the up
  // vector and the vertical field of view (0: the room's own).
  struct View {
    float eye[3], forward[3], up[3];
    float fov_deg;
  };
  View view() const;
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

enum class GarageTab { kMyCars = 0, kRestorationBay = 1 };

struct BarnRestorationEntry {
  std::string barn_id;
  std::string car_name;
  int64_t car_id = 0;
  double ready_after_s = 45.0;
  double elapsed_s = 0.0;
  bool ready_to_claim = false;

  float progress() const {
    if (ready_to_claim || ready_after_s <= 0.0) return 1.0f;
    return float(std::clamp(elapsed_s / ready_after_s, 0.0, 1.0));
  }
  double remaining_s() const {
    if (ready_to_claim) return 0.0;
    return std::max(0.0, ready_after_s - elapsed_s);
  }
};

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

  // Dedicated "REMOVE CAR" action (two-step confirmation; blocked on the last remaining car
  // or currently active car unless `allow_switch_active` is true; prize/barn cars ALWAYS yield
  // 0 CR on removal so prize cars cannot be exploited for Autoshow refunds).
  int64_t remove_confirm_garage_id() const { return remove_confirm_id_; }
  void cancel_remove_confirm() { remove_confirm_id_ = -1; }
  bool remove_selected(garage::Garage& g, const cars::PartsDb* db, const std::vector<garage::ClassBand>& bands,
                       int64_t& credits, std::string& message, bool confirmed = false,
                       bool allow_switch_active = false);

  // Dak's Garage Barn Find Restoration Bay tab.
  GarageTab active_tab() const { return tab_; }
  void set_tab(GarageTab t);
  void toggle_tab();
  void set_restorations(std::vector<BarnRestorationEntry> items);
  const std::vector<BarnRestorationEntry>& restorations() const { return restorations_; }
  size_t selected_restoration_index() const { return selected_restoration_; }
  void select_restoration_index(size_t idx) {
    if (idx < restorations_.size()) selected_restoration_ = idx;
  }
  const BarnRestorationEntry* selected_restoration() const {
    return selected_restoration_ < restorations_.size() ? &restorations_[selected_restoration_] : nullptr;
  }
  int ready_restoration_count() const;
  // Handles a mouse click on Garage top tab/filter hit-zones (kHitTab 0..4):
  // 0 = MY CARS, 1 = RESTORATION BAY, 2 = SORT, 3 = CLASS FILTER, 4 = FAVORITES.
  bool handle_tab_click(int tab_index);

  using AwardCarCallback =
      std::function<std::string(int64_t car_id, int colour_seq, bool select_it, bool is_prize)>;
  // Claims the selected restoration if `ready_to_claim` is true, awarding the car with
  // `is_prize = true` via `award_fn` (or directly into `g` via `garage::award_prize_car`)
  // and removing it from `restorations_`.
  bool claim_selected_restoration(garage::Garage& g, const cars::PartsDb* db,
                                  const std::vector<garage::ClassBand>& bands, const AwardCarCallback& award_fn,
                                  std::string& claimed_barn_id_out, std::string& message);
  bool claim_selected_restoration(garage::Garage& g, const cars::PartsDb* db,
                                  const std::vector<garage::ClassBand>& bands, std::string& claimed_barn_id_out,
                                  std::string& message);

 private:
  void apply_filter_and_sort();
  std::vector<GarageEntry> all_;
  std::vector<GarageEntry> visible_;
  GarageSort sort_ = GarageSort::kClass;
  int class_filter_ = -1;  // -1 = All, 0..10 = F..X
  bool favorites_only_ = false;
  size_t selected_ = 0;
  int64_t remove_confirm_id_ = -1;
  GarageTab tab_ = GarageTab::kMyCars;
  std::vector<BarnRestorationEntry> restorations_;
  size_t selected_restoration_ = 0;
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
