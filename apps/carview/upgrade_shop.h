// carview: Dak's Garage / Upgrade Shop (SPEC-GARAGE-UPGRADES).
// Full 8-category workshop: Engine, Platform & Handling, Drivetrain,
// Tires & Rims (incl. 285+ aftermarket List_Wheels rims), Aero & Appearance,
// Conversions (Engine/Drivetrain/Body/Aspiration), Auto Upgrade (target class
// E..R1), and 35-slider Car Tuning. Recomputes PI & 5-stat diffs live.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "cars/visual_setup.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "garage/rally_kit.h"
#include "garage/tuning.h"

namespace carview {

enum class ShopCategory {
  kEngine = 0,
  kPlatform,
  kDrivetrain,
  kTiresRims,
  kAero,
  kConversions,
  kAutoUpgrade,
  kTuning,
  kCount
};

const char* shop_category_name(ShopCategory cat);

class UpgradeShop {
 public:
  // Opens the Upgrade Shop for `garage_id`. When `phys_settings`, `pi_cfg`,
  // and `bands` are provided, live PI & 5-stat diff previews, Auto Upgrade,
  // and PI recompute on fit are active.
  bool open(const cars::PartsDb& db, garage::Garage& g, int64_t garage_id, std::string& why,
            const cars::PhysicsSettings* phys_settings = nullptr, const garage::PiConfig* pi_cfg = nullptr,
            const std::vector<garage::ClassBand>* bands = nullptr, double price_multiplier = 1.0);
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  // Category tab navigation.
  void move_category(int by);
  void set_category(ShopCategory cat);
  ShopCategory category() const { return category_; }

  // Navigation within the active category (slots/parts, auto-upgrade class, or tuning slider).
  void move_slot(int by);
  void move_part(int by);
  void select_row(size_t idx) {
    const size_t cur = selected_row();
    if (idx != cur) move_slot(int(idx) - int(cur));
  }

  // Fits the shown part / conversion, applies Auto Upgrade, or resets Tuning slider.
  bool fit(int discount_percent, int64_t& credits, std::string& message);
  // Inspects / plans / installs the full Horizon Rally Conversion Kit (tyres, suspension, transmission, aero).
  garage::RallyKitPlan rally_kit_plan(int discount_percent, bool include_optional_aero = true) const;
  bool fit_rally_kit(int discount_percent, int64_t& credits, std::string& message, bool include_optional_aero = true);
  // Sells an owned non-fitted non-stock part for 50% resale value.
  bool sell_shown_part(int64_t& credits, std::string& message);
  // Resets all tuning sliders back to stock defaults (when on Tuning tab).
  void reset_tuning();

  // Whether any part or tuning changed since opening (signals car rebuild / save).
  bool changed() const { return changed_; }
  bool visual_dirty() const { return visual_dirty_; }
  void clear_visual_dirty() { visual_dirty_ = false; }

  // Live PI & 5-stat preview (current car vs hovered part / auto-upgrade target).
  const garage::PiResult& current_pi() const { return current_pi_; }
  const garage::PiResult& preview_pi() const { return preview_pi_; }
  bool has_preview_pi() const { return has_preview_pi_; }
  const garage::AutoUpgradePlan& auto_plan() const { return auto_plan_; }

  // Current category/slot inspection for rich 3D hub rendering.
  struct SlotView {
    std::string label;
    std::string part_label;
    std::string state;  // "FITTED", "OWNED", "STOCK", or "<price> CR"
    int level = 0;
    int64_t price = 0;
    bool fitted = false;
    bool owned = false;
    bool is_stock = false;
    int part_enum = -1;
  };
  std::vector<SlotView> visible_rows(int discount_percent) const;
  size_t selected_row() const;
  int active_part_enum() const;  // -1 for Auto Upgrade / Tuning

  // Tuning sliders state (when category() == kTuning).
  const std::vector<garage::TuneSlider>& tuning_sliders() const { return sliders_; }
  size_t selected_slider() const { return slider_index_; }

  // Builds a VisualSetup reflecting the currently hovered part (live 3D preview!).
  bool preview_visual(cars::VisualSetup& out) const;

  // Legacy pause-menu text panel helper.
  void fill(const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits, std::string& title,
            std::vector<std::string>& lines, int& selected) const;

 private:
  struct Slot {
    int part_enum = 0;
    std::string column;
    std::string table;
    std::string display_label;
    ShopCategory category = ShopCategory::kEngine;
    std::vector<cars::PartRow> parts;
    std::vector<std::string> part_names;  // authentic localized FH1 part/swap/rim names
    size_t shown = 0;
  };

  bool reload_slots(std::string& why);
  void refresh_current_and_preview();
  size_t fitted_index(const Slot& s) const;
  std::vector<size_t> category_slot_indices(ShopCategory cat) const;

  const cars::PartsDb* db_ = nullptr;
  garage::Garage* garage_ = nullptr;
  int64_t garage_id_ = 0;
  const cars::PhysicsSettings* phys_settings_ = nullptr;
  const garage::PiConfig* pi_cfg_ = nullptr;
  const std::vector<garage::ClassBand>* bands_ = nullptr;
  double price_multiplier_ = 1.0;

  std::vector<Slot> slots_;
  ShopCategory category_ = ShopCategory::kEngine;
  size_t selected_in_cat_ = 0;

  // Auto Upgrade targets (class IDs 1..9 = E..R1).
  std::vector<int> auto_targets_;
  size_t auto_index_ = 0;
  garage::AutoUpgradePlan auto_plan_;

  // Tuning sliders.
  std::vector<garage::TuneSlider> sliders_;
  size_t slider_index_ = 0;

  garage::PiResult current_pi_;
  garage::PiResult preview_pi_;
  bool has_preview_pi_ = false;
  bool open_ = false;
  bool changed_ = false;
  bool visual_dirty_ = false;
};

}  // namespace carview
