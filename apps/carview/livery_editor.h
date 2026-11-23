// carview: Paint Shop (Normal/Metallic/Matte/Special 102 two-tone paints,
// Chrome/Carbon/Wood/Camo finishes, HSV colour grid, per-region painting) and
// Vinyl/Livery Editor (Livery.zip 1,372 shapes, 1024x1024 compositor, .liv I/O).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
  kPaint = 0,          // Per-region finish + HSV colour grid
  kSpecialColors = 1,  // 102 List_SpecialColors two-tone paints
  kVinylLayers = 2,    // Livery.zip vinyl & decal layer stack
  kCount = 3,
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
  void move_region(int by);
  PaintRegion region() const { return region_; }
  void move_finish(int by);
  void step_hsv(float dh, float ds, float dv);
  void step_window_tint(float delta);
  void move_special_color(int by);
  void apply_special_color();

  // Vinyl Layer Stack controls.
  void add_vinyl_layer(const std::string& shape_path = {}, uint32_t rgb = 0xFFFFFFu);
  void delete_selected_layer();
  void select_layer(int by);
  void move_layer_order(int by);
  void transform_layer(float dx, float dy, float dsx, float dsy, float drot);
  void cycle_layer_shape(int by);
  void cycle_layer_plane(int by);
  void recolor_layer_hsv(float dh, float ds, float dv);

  // Persistence (.liv) & 1024x1024 compositor.
  bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
  bool load(const std::filesystem::path& path, std::string* error = nullptr);
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

 private:
  cars::PaintRegionSpec& active_region_spec();
  const cars::PaintRegionSpec& active_region_spec() const;

  bool open_ = false;
  bool dirty_ = false;
  bool wheel_is_chrome_ = false;
  EditorTab tab_ = EditorTab::kPaint;
  PaintRegion region_ = PaintRegion::kBody;
  cars::LiveryDocument doc_;
  cars::LiveryLibrary lib_;
  std::vector<cars::SpecialColorRow> specials_;
  size_t special_idx_ = 0;
  size_t layer_idx_ = 0;
  size_t shape_cursor_ = 0;
  float cur_h_ = 0.0f, cur_s_ = 0.85f, cur_v_ = 0.85f;
};

// Draws the Forza Horizon 1-styled Paint Shop & Vinyl/Livery Editor HUD overlay.
void draw_livery_editor_hud(ui::List& list, float w, float h, const LiveryEditor& editor,
                            const std::string& car_title, const std::string& notice = {});

}  // namespace carview
