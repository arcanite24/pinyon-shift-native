// carview: Paint Shop (Normal/Metallic/Matte/Special 102 two-tone paints,
// Chrome/Carbon/Wood/Camo finishes, HSV colour grid, per-region painting) and
// Vinyl/Livery Editor (Livery.zip 1,372 shapes, 1024x1024 compositor, .liv I/O).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cars/bake_format.h"
#include "cars/livery.h"
#include "cars/physics.h"
#include "cars/shader_settings.h"
#include "ui.h"

namespace carview {

enum class PaintRegion : int {
  kBody = 0,
  kHood = 1,
  kWing = 2,
  kMirrors = 3,
  kRims = 4,
  kCalipers = 5,
  kWindowTint = 6,
  kCount = 7,
};

const char* paint_region_name(PaintRegion r);

enum class EditorTab : int {
  kPaint = 0,               // Per-region finish + HSV colour grid
  kManufacturerColors = 1,  // Factory OEM manufacturer swatches for this car
  kSpecialColors = 2,       // 102 List_SpecialColors two-tone paints
  kPreviousColors = 3,      // Recently applied custom/paint colours
  kVinylLayers = 4,         // Livery.zip vinyl & decal layer stack
  kCount = 5,
};

enum class ShapeCategory : int {
  kAll = 0,
  kVinyls = 1,
  kDecals = 2,
  kBuiltins = 3,
  kCount = 4,
};

const char* shape_category_name(ShapeCategory cat);

struct PreviousColorEntry {
  uint32_t rgb = 0xCC2222u;
  cars::PaintFinish finish = cars::PaintFinish::kNormal;
};

class LiveryEditor {
 public:
  bool open(const cars::PartsDb* db, const std::filesystem::path& game_root, int64_t car_id,
            const std::filesystem::path& liv_path, std::string& message);
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  void set_tab(EditorTab t) { tab_ = t; }
  void move_tab(int by);
  EditorTab tab() const { return tab_; }

  // Paint Shop controls.
  void set_region(PaintRegion r);
  void move_region(int by);
  PaintRegion region() const { return region_; }
  void move_finish(int by);
  void step_hsv(float dh, float ds, float dv);
  void set_swatch(int row, int col, int rows = 4, int cols = 18);
  void step_window_tint(float delta);
  void select_special_index(size_t idx);
  void move_special_color(int by);
  void apply_special_color();

  // Manufacturer & Previous Colors tabs.
  void set_manufacturer_colors(const std::vector<cars::bake::Colour>& colors);
  const std::vector<cars::bake::Colour>& manufacturer_colors() const { return mfg_colors_; }
  size_t selected_manufacturer_color() const { return mfg_idx_; }
  void select_manufacturer_index(size_t idx);
  void move_manufacturer_color(int by);
  void apply_manufacturer_color();

  const std::vector<PreviousColorEntry>& previous_colors() const { return prev_colors_; }
  size_t selected_previous_color() const { return prev_idx_; }
  void select_previous_index(size_t idx);
  void move_previous_color(int by);
  void apply_previous_color();

  // Vinyl Layer Stack controls.
  void add_vinyl_layer(const std::string& shape_path = {}, uint32_t rgb = 0xFFFFFFu);
  void delete_selected_layer();
  void select_layer(int by);
  void select_layer_index(size_t idx);
  void move_layer_order(int by);
  void transform_layer(float dx, float dy, float dsx, float dsy, float drot);
  void cycle_layer_shape(int by);
  void cycle_shape_category(int by);
  ShapeCategory shape_category() const { return shape_cat_; }
  void cycle_layer_plane(int by);
  void recolor_layer_hsv(float dh, float ds, float dv);
  void toggle_layer_flip(bool horizontal);
  void adjust_layer_skew(float dskew);
  void adjust_layer_alpha(float dalpha);
  void toggle_layer_mask();
  void toggle_layer_group();
  bool mirror_selected_layer();
  bool copy_selected_layer();
  bool paste_layer();
  bool has_clipboard() const { return clipboard_.has_value(); }

  // Persistence (.liv) & 1024x1024 compositor.
  bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
  bool load(const std::filesystem::path& path, std::string* error = nullptr);
  void set_plane_masks(cars::LiveryMaskSet masks) { plane_masks_ = std::move(masks); }
  const cars::LiveryMaskSet& plane_masks() const { return plane_masks_; }
  bool composite_1024(const std::vector<cars::bake::LiveryPlane>* planes, std::vector<uint8_t>& out_rgba,
                      uint32_t size = 1024);

  const cars::LiveryDocument& document() const { return doc_; }
  cars::LiveryDocument& document() { return doc_; }
  const cars::PaintRegionSpec& current_region_spec() const { return active_region_spec(); }
  const std::vector<cars::SpecialColorRow>& special_colors() const { return specials_; }
  size_t selected_special() const { return special_idx_; }
  size_t selected_layer() const { return layer_idx_; }
  size_t library_shape_count() const { return lib_.total_shapes(); }
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }
  bool wheel_is_chrome() const { return wheel_is_chrome_; }
  void set_wheel_is_chrome(bool c) { wheel_is_chrome_ = c; }
  bool wheel_is_stock() const { return wheel_is_stock_; }
  void set_wheel_is_stock(bool s) { wheel_is_stock_ = s; }
  bool rims_paint_blocked() const { return wheel_is_chrome_ || wheel_is_stock_; }

 private:
  cars::PaintRegionSpec& active_region_spec();
  const cars::PaintRegionSpec& active_region_spec() const;
  void record_previous_color(uint32_t rgb, cars::PaintFinish finish);

  bool open_ = false;
  bool dirty_ = false;
  bool wheel_is_chrome_ = false;
  bool wheel_is_stock_ = false;
  EditorTab tab_ = EditorTab::kPaint;
  PaintRegion region_ = PaintRegion::kBody;
  ShapeCategory shape_cat_ = ShapeCategory::kAll;
  cars::LiveryDocument doc_;
  cars::LiveryLibrary lib_;
  cars::LiveryMaskSet plane_masks_;
  std::vector<cars::SpecialColorRow> specials_;
  std::vector<cars::bake::Colour> mfg_colors_;
  std::vector<PreviousColorEntry> prev_colors_;
  std::optional<cars::VinylLayer> clipboard_;
  uint32_t next_group_id_ = 1;
  size_t special_idx_ = 0;
  size_t mfg_idx_ = 0;
  size_t prev_idx_ = 0;
  size_t layer_idx_ = 0;
  size_t shape_cursor_ = 0;
  float cur_h_ = 0.0f, cur_s_ = 0.85f, cur_v_ = 0.85f;
};

// Draws the Forza Horizon 1-styled Paint Shop & Vinyl/Livery Editor HUD overlay.
void draw_livery_editor_hud(ui::List& list, float w, float h, const LiveryEditor& editor,
                            const std::string& car_title, const std::string& notice = {});

}  // namespace carview
