// carview: Paint Shop (Normal/Metallic/Matte/Special 102 two-tone paints,
// Chrome/Carbon finishes, HSV colour grid, per-region painting) and
// Vinyl/Livery Editor (Livery.zip 1,372 shapes, 1024x1024 compositor, .liv I/O).
#pragma once

#include <algorithm>
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
  kWindowTint = 5,
  kCount = 6,
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
// Formats a 0-based manufacturer swatch index as 1-based "OEM #1" .. "OEM #N".
std::string oem_swatch_label(size_t index);
// Human-readable FH1 paint finish & color display names (for Paint Shop UI).
const char* paint_finish_display_name(cars::PaintFinish finish);
std::string paint_hue_name(uint32_t rgb);
std::string special_color_display_name(const cars::SpecialColorRow& sc);
const char* special_color_finish_subtitle(const cars::SpecialColorRow& sc);
std::string manufacturer_color_display_name(const cars::bake::Colour& c);

struct PreviousColorEntry {
  uint32_t rgb = 0xCC2222u;
  cars::PaintFinish finish = cars::PaintFinish::kNormal;
};

struct DesignCatalogEntry {
  std::string name;
  std::filesystem::path path;
  int64_t car_id = 0;
  size_t layer_count = 0;
};

struct VinylGroupCatalogEntry {
  std::string name;
  std::filesystem::path path;
  size_t layer_count = 0;
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
  // Resets all paint regions & window tint back to factory OEM paint (undoable).
  void reset_to_factory_paint();

  // Undo stack (Ctrl+Z / LB undo).
  bool undo();
  bool can_undo() const { return !undo_stack_.empty(); }
  size_t undo_depth() const { return undo_stack_.size(); }
  void clear_undo() { undo_stack_.clear(); }

  // Vinyl Layer Stack controls (all transform/color/alpha/skew/flip ops act on
  // every layer sharing `group_id` when the selected layer is grouped).
  void add_vinyl_layer(const std::string& shape_path = {}, uint32_t rgb = 0xFFFFFFu);
  void delete_selected_layer();
  void select_layer(int by);
  void select_layer_index(size_t idx);
  void move_layer_order(int by);
  void transform_layer(float dx, float dy, float dsx, float dsy, float drot);
  void transform_layer_continuous(float dx, float dy, float dsx, float dsy, float drot, double now_s);
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
  bool group_layers(const std::vector<size_t>& indices);
  bool ungroup_selected_group();
  bool transform_selected_group(float dx, float dy, float scale_x_factor, float scale_y_factor, float drot_deg);
  bool duplicate_selected_layer();
  bool mirror_selected_layer();
  bool copy_selected_layer();
  bool paste_layer();
  bool has_clipboard() const { return clipboard_.has_value(); }

  // Reusable Vinyl Group `.vg` stamp catalog (save/load/stamp across cars & sides).
  void scan_vinyl_group_catalog(const std::filesystem::path& groups_dir);
  const std::vector<VinylGroupCatalogEntry>& vinyl_group_catalog() const { return vg_catalog_; }
  size_t selected_vinyl_group() const { return vg_idx_; }
  void move_vinyl_group_selection(int by);
  void select_vinyl_group_index(size_t idx);
  bool save_selected_group_stamp(const std::filesystem::path& groups_dir, const std::string& group_name,
                                 std::string* error = nullptr);
  bool stamp_vinyl_group_file(const std::filesystem::path& vg_path, cars::LiveryPlaneKind target_plane,
                              float cx = 0.5f, float cy = 0.5f, std::string* error = nullptr);
  bool stamp_selected_vinyl_group(std::string* error = nullptr);
  const std::filesystem::path& vinyl_groups_dir() const { return vg_dir_; }

  // Debounced / throttled GPU livery preview upload state (eliminates per-tick
  // vkDeviceWaitIdle stalls during analog stick dragging).
  void mark_vinyl_preview_dirty(double now_s);
  bool needs_vinyl_preview_upload(double now_s) const;
  bool has_pending_vinyl_preview() const { return vinyl_preview_pending_; }
  void mark_vinyl_preview_uploaded(double now_s);
  void set_preview_throttle_seconds(double sec) { preview_throttle_s_ = std::max(0.0, sec); }
  double preview_throttle_seconds() const { return preview_throttle_s_; }

  // Design Catalog (save/load list backed by `liveries/*.liv`).
  void scan_design_catalog(const std::filesystem::path& liveries_dir);
  const std::vector<DesignCatalogEntry>& design_catalog() const { return catalog_; }
  size_t selected_design() const { return catalog_idx_; }
  void move_design_selection(int by);
  void select_design_index(size_t idx);
  bool save_to_catalog(const std::filesystem::path& liveries_dir, const std::string& design_name,
                       std::string* error = nullptr);
  bool load_selected_design(std::string* error = nullptr);
  bool catalog_open() const { return catalog_open_; }
  void set_catalog_open(bool open) { catalog_open_ = open; }
  void toggle_catalog() { catalog_open_ = !catalog_open_; }

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
  // Loads/retrieves a vinyl shape mesh for miniature HUD preview thumbnails.
  const cars::VinylMesh* preview_shape_mesh(const std::string& shape_path) const;
  // Handles left/right carousel & finish stepper arrows (`kHitStepLeft` / `kHitStepRight`).
  bool handle_step_click(int dir);
  // Handles bottom button_bar clicks (indices 0..5, including index 2 DPAD_LR).
  // Returns true if the editor state was updated in-place (save/back 0/5 return false for caller).
  bool handle_button_bar_click(int button_index);
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }
  bool wheel_is_chrome() const { return wheel_is_chrome_; }
  void set_wheel_is_chrome(bool c) { wheel_is_chrome_ = c; }
  bool wheel_is_stock() const { return wheel_is_stock_; }
  void set_wheel_is_stock(bool s) { wheel_is_stock_ = s; }
  bool rims_paint_blocked() const { return wheel_is_chrome_ || wheel_is_stock_; }

 private:
  struct UndoSnapshot {
    cars::LiveryDocument doc;
    size_t layer_idx = 0;
  };

  cars::PaintRegionSpec& active_region_spec();
  const cars::PaintRegionSpec& active_region_spec() const;
  void record_previous_color(uint32_t rgb, cars::PaintFinish finish);
  void push_undo();
  void push_undo_coalesced(double now_s);
  void apply_transform_impl(float dx, float dy, float dsx, float dsy, float drot);

  bool open_ = false;
  bool dirty_ = false;
  bool wheel_is_chrome_ = false;
  bool wheel_is_stock_ = false;
  EditorTab tab_ = EditorTab::kPaint;
  PaintRegion region_ = PaintRegion::kBody;
  ShapeCategory shape_cat_ = ShapeCategory::kAll;
  cars::LiveryDocument doc_;
  mutable cars::LiveryLibrary lib_;
  cars::LiveryMaskSet plane_masks_;
  std::vector<cars::SpecialColorRow> specials_;
  std::vector<cars::bake::Colour> mfg_colors_;
  std::vector<PreviousColorEntry> prev_colors_;
  std::optional<cars::VinylLayer> clipboard_;
  std::vector<UndoSnapshot> undo_stack_;
  double last_continuous_undo_s_ = -1e9;
  bool vinyl_preview_pending_ = false;
  double last_vinyl_upload_s_ = -1e9;
  double last_vinyl_dirty_s_ = -1e9;
  double preview_throttle_s_ = 0.12;
  std::vector<DesignCatalogEntry> catalog_;
  size_t catalog_idx_ = 0;
  bool catalog_open_ = false;
  std::filesystem::path catalog_dir_;
  std::vector<VinylGroupCatalogEntry> vg_catalog_;
  size_t vg_idx_ = 0;
  std::filesystem::path vg_dir_;
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
