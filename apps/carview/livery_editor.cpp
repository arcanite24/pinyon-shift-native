#include "livery_editor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "fh1data/stringtables.h"
#include "game_hud.h"
#include "hud_scene.h"
#include "scene_screen.h"
#include "ui4_chrome.h"

namespace carview {

namespace {

std::string loc_str(std::string_view table, std::string_view id, const char* fallback) {
  const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
  const std::string ref = "_&" + std::to_string(packed);
  std::string resolved = game_text(ref);
  if (resolved.empty() || resolved == ref || resolved.rfind("_&", 0) == 0) {
    resolved = ui::lookup_text(ref);
  }
  if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) return resolved;
  return fallback;
}

std::string loc_key(const std::string& key, const std::string& fallback) {
  if (!key.empty()) {
    const std::string resolved = game_text(key);
    if (!resolved.empty() && resolved != key && resolved.rfind("_&", 0) != 0) return resolved;
  }
  return fallback;
}

bool looks_like_internal_key(const std::string& s) {
  if (s.empty()) return true;
  if (s.rfind("_&", 0) == 0 || s.rfind("IDS_", 0) == 0 || s.rfind("STR_", 0) == 0 ||
      s.rfind("KEY_", 0) == 0 || s.rfind("SPECIAL_", 0) == 0) {
    return true;
  }
  bool all_digits = true;
  for (unsigned char c : s) {
    if (!std::isdigit(c)) {
      all_digits = false;
      break;
    }
  }
  return all_digits;
}

}  // namespace

const char* paint_region_name(PaintRegion r) {
  switch (r) {
    case PaintRegion::kBody: return "BODY";
    case PaintRegion::kHood: return "HOOD";
    case PaintRegion::kWing: return "REAR WING";
    case PaintRegion::kMirrors: return "MIRRORS";
    case PaintRegion::kRims: return "RIMS";
    case PaintRegion::kWindowTint: return "WINDOW TINT";
    default: return "BODY";
  }
}

const char* shape_category_name(ShapeCategory cat) {
  switch (cat) {
    case ShapeCategory::kVinyls: return "VINYLS";
    case ShapeCategory::kDecals: return "MFG DECALS";
    case ShapeCategory::kBuiltins: return "PRIMITIVES";
    default: return "ALL SHAPES";
  }
}

std::string oem_swatch_label(size_t index) {
  return "OEM #" + std::to_string(index + 1);
}

const char* paint_finish_display_name(cars::PaintFinish finish) {
  switch (finish) {
    case cars::PaintFinish::kNormal: return "FACTORY";
    case cars::PaintFinish::kMetallic: return "METALLIC";
    case cars::PaintFinish::kMatte: return "MATTE";
    case cars::PaintFinish::kTwoTone: return "TWO-TONE";
    case cars::PaintFinish::kChrome: return "CHROME";
    case cars::PaintFinish::kCarbon: return "CARBON FIBER";
    default: return "FACTORY";
  }
}

std::string paint_hue_name(uint32_t rgb) {
  float h = 0.0f, s = 0.0f, v = 0.0f;
  cars::rgb_to_hsv(rgb, h, s, v);
  if (v < 0.14f) return "BLACK";
  if (s < 0.12f) {
    if (v > 0.88f) return "WHITE";
    if (v > 0.64f) return "SILVER";
    if (v > 0.36f) return "GUNMETAL";
    return "CHARCOAL";
  }
  const float deg = h * 360.0f;
  if (deg >= 18.0f && deg < 46.0f && v < 0.55f) return "BRONZE";
  if (deg >= 12.0f && deg < 40.0f && v < 0.40f) return "BROWN";
  if (deg < 15.0f || deg >= 345.0f) return (v < 0.58f) ? "CRIMSON" : "RED";
  if (deg < 36.0f) return "ORANGE";
  if (deg < 50.0f) return (s > 0.55f && v < 0.85f) ? "GOLD" : "AMBER";
  if (deg < 72.0f) return "YELLOW";
  if (deg < 105.0f) return "LIME";
  if (deg < 150.0f) return (v < 0.55f) ? "EMERALD" : "GREEN";
  if (deg < 178.0f) return "TEAL";
  if (deg < 200.0f) return "CYAN";
  if (deg < 220.0f) return (v > 0.78f) ? "SKY BLUE" : "BLUE";
  if (deg < 252.0f) return (s > 0.65f) ? "COBALT" : "BLUE";
  if (deg < 276.0f) return "INDIGO";
  if (deg < 305.0f) return "PURPLE";
  if (deg < 330.0f) return "MAGENTA";
  return "PINK";
}

const char* special_color_finish_subtitle(const cars::SpecialColorRow& sc) {
  const auto fin = static_cast<cars::PaintFinish>(sc.finish);
  if (fin == cars::PaintFinish::kTwoTone) {
    if (sc.two_tone_power >= 3.0f) return "PEARL";
    if (sc.two_tone_power <= 1.25f) return "SEMI-GLOSS";
    return "TWO-TONE";
  }
  return paint_finish_display_name(fin);
}

std::string special_color_display_name(const cars::SpecialColorRow& sc) {
  if (!sc.display_key.empty()) {
    const std::string localized = game_text(sc.display_key);
    if (!localized.empty() && localized != sc.display_key && !looks_like_internal_key(localized)) {
      return localized;
    }
    if (!looks_like_internal_key(sc.display_key)) {
      std::string cleaned = sc.display_key;
      for (char& ch : cleaned) {
        if (ch == '_' || ch == '-') ch = ' ';
        else ch = char(std::toupper(static_cast<unsigned char>(ch)));
      }
      return cleaned;
    }
  }
  const auto fin = static_cast<cars::PaintFinish>(sc.finish);
  const std::string pri = paint_hue_name(sc.pri_rgb);
  if (fin == cars::PaintFinish::kCarbon) {
    return std::string(paint_finish_display_name(fin));
  }
  if (fin == cars::PaintFinish::kChrome) {
    return (pri == "WHITE" || pri == "SILVER") ? "CHROME" : ("CHROME " + pri);
  }
  if (fin == cars::PaintFinish::kTwoTone) {
    const char* sub = special_color_finish_subtitle(sc);
    if (sc.sec_valid && sc.sec_rgb != sc.pri_rgb) {
      const std::string sec = paint_hue_name(sc.sec_rgb);
      if (sec != pri) {
        return std::string(sub) + " " + pri + "/" + sec;
      }
    }
    return std::string(sub) + " " + pri;
  }
  return std::string(paint_finish_display_name(fin)) + " " + pri;
}

std::string manufacturer_color_display_name(const cars::bake::Colour& c) {
  const std::string hue = paint_hue_name(c.rgb);
  return std::string(c.metallic ? "METALLIC " : "FACTORY ") + hue;
}

bool LiveryEditor::open(const cars::PartsDb* db, const std::filesystem::path& game_root, int64_t car_id,
                        const std::filesystem::path& liv_path, std::string& message) {
  doc_ = cars::LiveryDocument{};
  doc_.car_id = car_id;
  doc_.paint.body.active = true;
  if (!liv_path.empty() && std::filesystem::exists(liv_path)) {
    cars::load_livery_file(liv_path, doc_, nullptr);
    doc_.car_id = car_id;
  }
  undo_stack_.clear();
  last_continuous_undo_s_ = -1e9;
  vinyl_preview_pending_ = false;
  last_vinyl_upload_s_ = -1e9;
  last_vinyl_dirty_s_ = -1e9;
  catalog_open_ = false;
  catalog_.clear();
  catalog_idx_ = 0;
  if (!liv_path.empty() && liv_path.has_parent_path()) {
    catalog_dir_ = liv_path.parent_path();
    scan_design_catalog(catalog_dir_);
    vg_dir_ = catalog_dir_ / "vinyl_groups";
    scan_vinyl_group_catalog(vg_dir_);
  } else {
    vg_dir_.clear();
    vg_catalog_.clear();
    vg_idx_ = 0;
  }
  uint32_t max_gid = 0;
  for (const auto& ly : doc_.layers) {
    if (ly.group_id > 0 && uint32_t(ly.group_id) > max_gid) max_gid = uint32_t(ly.group_id);
  }
  next_group_id_ = max_gid + 1;
  plane_masks_.planes.clear();
  if (db && db->is_open()) {
    cars::query_special_colors(*db, specials_, nullptr);
    if (!game_root.empty() && car_id > 0) {
      cars::CarPhysics phys;
      if (db->physics(car_id, {}, phys, nullptr) && !phys.media_name.empty()) {
        cars::load_livery_masks_for_car(game_root, phys.media_name, plane_masks_, nullptr);
      }
    }
  }
  if (!game_root.empty()) {
    const auto lzip = game_root / "media" / "Livery.zip";
    if (std::filesystem::exists(lzip)) lib_.open(lzip, nullptr);
  }
  cars::rgb_to_hsv(doc_.paint.body.rgb, cur_h_, cur_s_, cur_v_);
  record_previous_color(doc_.paint.body.rgb, doc_.paint.body.finish);
  tab_ = EditorTab::kPaint;
  region_ = PaintRegion::kBody;
  shape_cat_ = ShapeCategory::kAll;
  special_idx_ = 0;
  mfg_idx_ = 0;
  prev_idx_ = 0;
  layer_idx_ = 0;
  dirty_ = false;
  open_ = true;
  message = "Paint Shop ready (" + std::to_string(specials_.size()) + " special colors, " +
            std::to_string(lib_.total_shapes()) + " vinyl shapes)";
  return true;
}

cars::PaintRegionSpec& LiveryEditor::active_region_spec() {
  switch (region_) {
    case PaintRegion::kHood: return doc_.paint.hood;
    case PaintRegion::kWing: return doc_.paint.wing;
    case PaintRegion::kMirrors: return doc_.paint.mirrors;
    case PaintRegion::kRims: return doc_.paint.rims;
    default: return doc_.paint.body;
  }
}

const cars::PaintRegionSpec& LiveryEditor::active_region_spec() const {
  switch (region_) {
    case PaintRegion::kHood: return doc_.paint.hood;
    case PaintRegion::kWing: return doc_.paint.wing;
    case PaintRegion::kMirrors: return doc_.paint.mirrors;
    case PaintRegion::kRims: return doc_.paint.rims;
    default: return doc_.paint.body;
  }
}

void LiveryEditor::move_tab(int by) {
  const int n = int(EditorTab::kCount);
  tab_ = EditorTab(((int(tab_) + by) % n + n) % n);
}

void LiveryEditor::set_region(PaintRegion r) {
  const int n = int(PaintRegion::kCount);
  const int idx = std::clamp(int(r), 0, n - 1);
  region_ = PaintRegion(idx);
  if (region_ != PaintRegion::kWindowTint) {
    cars::rgb_to_hsv(active_region_spec().rgb, cur_h_, cur_s_, cur_v_);
  }
}

void LiveryEditor::move_region(int by) {
  const int n = int(PaintRegion::kCount);
  set_region(PaintRegion(((int(region_) + by) % n + n) % n));
}

void LiveryEditor::record_previous_color(uint32_t rgb, cars::PaintFinish finish) {
  for (auto it = prev_colors_.begin(); it != prev_colors_.end(); ++it) {
    if (it->rgb == rgb && it->finish == finish) {
      prev_colors_.erase(it);
      break;
    }
  }
  prev_colors_.insert(prev_colors_.begin(), PreviousColorEntry{rgb, finish});
  if (prev_colors_.size() > 16) prev_colors_.resize(16);
}

void LiveryEditor::push_undo() {
  constexpr size_t kMaxUndoDepth = 64;
  undo_stack_.push_back(UndoSnapshot{doc_, layer_idx_});
  if (undo_stack_.size() > kMaxUndoDepth) {
    undo_stack_.erase(undo_stack_.begin());
  }
}

void LiveryEditor::push_undo_coalesced(double now_s) {
  if (undo_stack_.empty() || (now_s - last_continuous_undo_s_) > 0.45) {
    push_undo();
  }
  last_continuous_undo_s_ = now_s;
}

bool LiveryEditor::undo() {
  if (undo_stack_.empty()) return false;
  UndoSnapshot snap = std::move(undo_stack_.back());
  undo_stack_.pop_back();
  doc_ = std::move(snap.doc);
  layer_idx_ = doc_.layers.empty() ? 0 : std::min(snap.layer_idx, doc_.layers.size() - 1);
  if (region_ != PaintRegion::kWindowTint) {
    cars::rgb_to_hsv(active_region_spec().rgb, cur_h_, cur_s_, cur_v_);
  }
  last_continuous_undo_s_ = -1e9;
  dirty_ = true;
  return true;
}

void LiveryEditor::move_finish(int by) {
  if (region_ == PaintRegion::kWindowTint) return;
  if (region_ == PaintRegion::kRims && rims_paint_blocked()) return;
  push_undo();
  auto& spec = active_region_spec();
  // Retail 501_C_LIV_COLOR paint finishes: Normal (Gloss/Factory), Metallic, Matte.
  // Special finishes (Two-Tone/Pearl/Chrome/Carbon) are selected from Special Colors.
  static constexpr cars::PaintFinish kRetailFinishes[] = {
      cars::PaintFinish::kNormal,
      cars::PaintFinish::kMetallic,
      cars::PaintFinish::kMatte,
  };
  constexpr int n = int(sizeof(kRetailFinishes) / sizeof(kRetailFinishes[0]));
  int cur = 0;
  for (int i = 0; i < n; ++i) {
    if (kRetailFinishes[i] == spec.finish) {
      cur = i;
      break;
    }
  }
  spec.finish = kRetailFinishes[((cur + by) % n + n) % n];
  spec.active = true;
  record_previous_color(spec.rgb, spec.finish);
  dirty_ = true;
}

void LiveryEditor::step_hsv(float dh, float ds, float dv) {
  if (region_ == PaintRegion::kWindowTint) {
    step_window_tint(dh + dv);
    return;
  }
  if (region_ == PaintRegion::kRims && rims_paint_blocked()) return;
  push_undo();
  cur_h_ = std::fmod(cur_h_ + dh + 1.0f, 1.0f);
  cur_s_ = std::clamp(cur_s_ + ds, 0.0f, 1.0f);
  cur_v_ = std::clamp(cur_v_ + dv, 0.0f, 1.0f);
  auto& spec = active_region_spec();
  spec.rgb = cars::hsv_to_rgb(cur_h_, cur_s_, cur_v_);
  spec.active = true;
  record_previous_color(spec.rgb, spec.finish);
  dirty_ = true;
}

void LiveryEditor::step_window_tint(float delta) {
  push_undo();
  doc_.paint.window_tint = std::clamp(doc_.paint.window_tint + delta, 0.0f, 1.0f);
  dirty_ = true;
}

void LiveryEditor::set_swatch(int row, int col, int rows, int cols) {
  if (rows <= 0 || cols <= 0) return;
  row = std::clamp(row, 0, rows - 1);
  col = std::clamp(col, 0, cols - 1);
  if (region_ == PaintRegion::kWindowTint) {
    push_undo();
    const float t = (cols > 1) ? float(col) / float(cols - 1) : 0.5f;
    doc_.paint.window_tint = std::clamp(1.0f - t, 0.0f, 1.0f);
    dirty_ = true;
    return;
  }
  if (region_ == PaintRegion::kRims && rims_paint_blocked()) return;
  push_undo();
  cur_h_ = float(col) / float(cols);
  cur_s_ = (row == 0) ? 0.0f : (0.35f + 0.21f * float(row));
  cur_v_ = (row == 0) ? ((cols > 1) ? float(col) / float(cols - 1) : 1.0f) : (1.0f - 0.16f * float(row - 1));
  auto& spec = active_region_spec();
  spec.rgb = cars::hsv_to_rgb(cur_h_, cur_s_, cur_v_);
  spec.active = true;
  record_previous_color(spec.rgb, spec.finish);
  dirty_ = true;
}

void LiveryEditor::select_special_index(size_t idx) {
  if (specials_.empty()) return;
  special_idx_ = std::min(idx, specials_.size() - 1);
}

void LiveryEditor::move_special_color(int by) {
  if (specials_.empty()) return;
  const int n = int(specials_.size());
  special_idx_ = size_t(((int(special_idx_) + by) % n + n) % n);
}

void LiveryEditor::apply_special_color() {
  if (specials_.empty() || region_ == PaintRegion::kWindowTint) return;
  if (region_ == PaintRegion::kRims && rims_paint_blocked()) return;
  push_undo();
  const auto& sc = specials_[std::min(special_idx_, specials_.size() - 1)];
  auto& spec = active_region_spec();
  spec.active = true;
  spec.rgb = sc.pri_rgb;
  spec.sec_rgb = sc.sec_valid ? sc.sec_rgb : sc.pri_rgb;
  spec.two_tone_scale = sc.two_tone_scale > 0.0f ? sc.two_tone_scale : 0.35f;
  spec.two_tone_bias = sc.two_tone_bias;
  spec.two_tone_power = sc.two_tone_power > 0.0f ? sc.two_tone_power : 3.0f;
  spec.finish = (sc.sec_valid && sc.two_tone_scale > 0.0f) ? cars::PaintFinish::kTwoTone
                : sc.finish == 2                           ? cars::PaintFinish::kMatte
                                                           : cars::PaintFinish::kMetallic;
  cars::rgb_to_hsv(spec.rgb, cur_h_, cur_s_, cur_v_);
  record_previous_color(spec.rgb, spec.finish);
  dirty_ = true;
}

void LiveryEditor::set_manufacturer_colors(const std::vector<cars::bake::Colour>& colors) {
  mfg_colors_ = colors;
  mfg_idx_ = 0;
}

void LiveryEditor::select_manufacturer_index(size_t idx) {
  if (mfg_colors_.empty()) return;
  mfg_idx_ = std::min(idx, mfg_colors_.size() - 1);
}

void LiveryEditor::move_manufacturer_color(int by) {
  if (mfg_colors_.empty()) return;
  const int n = int(mfg_colors_.size());
  mfg_idx_ = size_t(((int(mfg_idx_) + by) % n + n) % n);
}

void LiveryEditor::apply_manufacturer_color() {
  if (mfg_colors_.empty() || region_ == PaintRegion::kWindowTint) return;
  if (region_ == PaintRegion::kRims && rims_paint_blocked()) return;
  push_undo();
  const auto& mc = mfg_colors_[std::min(mfg_idx_, mfg_colors_.size() - 1)];
  auto& spec = active_region_spec();
  spec.active = true;
  spec.rgb = mc.rgb;
  spec.finish = mc.metallic ? cars::PaintFinish::kMetallic : cars::PaintFinish::kNormal;
  cars::rgb_to_hsv(spec.rgb, cur_h_, cur_s_, cur_v_);
  record_previous_color(spec.rgb, spec.finish);
  dirty_ = true;
}

void LiveryEditor::select_previous_index(size_t idx) {
  if (prev_colors_.empty()) return;
  prev_idx_ = std::min(idx, prev_colors_.size() - 1);
}

void LiveryEditor::move_previous_color(int by) {
  if (prev_colors_.empty()) return;
  const int n = int(prev_colors_.size());
  prev_idx_ = size_t(((int(prev_idx_) + by) % n + n) % n);
}

void LiveryEditor::apply_previous_color() {
  if (prev_colors_.empty() || region_ == PaintRegion::kWindowTint) return;
  if (region_ == PaintRegion::kRims && rims_paint_blocked()) return;
  push_undo();
  const auto entry = prev_colors_[std::min(prev_idx_, prev_colors_.size() - 1)];
  auto& spec = active_region_spec();
  spec.active = true;
  spec.rgb = entry.rgb;
  spec.finish = entry.finish;
  cars::rgb_to_hsv(spec.rgb, cur_h_, cur_s_, cur_v_);
  dirty_ = true;
}

void LiveryEditor::reset_to_factory_paint() {
  push_undo();
  doc_.paint = cars::CarPaintSpec{};
  doc_.layers.clear();
  layer_idx_ = 0;
  vinyl_preview_pending_ = true;
  if (!mfg_colors_.empty()) {
    doc_.paint.body.active = true;
    doc_.paint.body.rgb = mfg_colors_.front().rgb;
    doc_.paint.body.finish = mfg_colors_.front().metallic ? cars::PaintFinish::kMetallic : cars::PaintFinish::kNormal;
  } else {
    doc_.paint.body.active = false;
  }
  doc_.paint.window_tint = 0.0f;
  if (region_ != PaintRegion::kWindowTint) {
    cars::rgb_to_hsv(active_region_spec().rgb, cur_h_, cur_s_, cur_v_);
  }
  dirty_ = true;
}

void LiveryEditor::add_vinyl_layer(const std::string& shape_path, uint32_t rgb) {
  push_undo();
  cars::VinylLayer l;
  if (!shape_path.empty()) {
    l.shape_path = shape_path;
  } else if (!lib_.vinyl_names().empty()) {
    l.shape_path = lib_.vinyl_names()[shape_cursor_ % lib_.vinyl_names().size()];
  } else {
    l.shape_path = "@star";
  }
  l.rgb = rgb;
  l.x = 0.5f;
  l.y = 0.5f;
  l.scale_x = 0.25f;
  l.scale_y = 0.25f;
  doc_.layers.push_back(l);
  layer_idx_ = doc_.layers.size() - 1;
  dirty_ = true;
}

void LiveryEditor::delete_selected_layer() {
  if (doc_.layers.empty()) return;
  push_undo();
  doc_.layers.erase(doc_.layers.begin() + ptrdiff_t(std::min(layer_idx_, doc_.layers.size() - 1)));
  if (!doc_.layers.empty()) layer_idx_ = std::min(layer_idx_, doc_.layers.size() - 1);
  else layer_idx_ = 0;
  dirty_ = true;
}

void LiveryEditor::select_layer(int by) {
  if (doc_.layers.empty()) return;
  const int n = int(doc_.layers.size());
  layer_idx_ = size_t(((int(layer_idx_) + by) % n + n) % n);
}

void LiveryEditor::select_layer_index(size_t idx) {
  if (doc_.layers.empty()) return;
  layer_idx_ = std::min(idx, doc_.layers.size() - 1);
}

void LiveryEditor::move_layer_order(int by) {
  if (doc_.layers.size() < 2) return;
  const int dst = std::clamp(int(layer_idx_) + by, 0, int(doc_.layers.size()) - 1);
  if (size_t(dst) == layer_idx_) return;
  push_undo();
  std::swap(doc_.layers[layer_idx_], doc_.layers[size_t(dst)]);
  layer_idx_ = size_t(dst);
  dirty_ = true;
}

void LiveryEditor::apply_transform_impl(float dx, float dy, float dsx, float dsy, float drot) {
  if (layer_idx_ >= doc_.layers.size()) return;
  const int gid = doc_.layers[layer_idx_].group_id;
  if (gid == 0) {
    auto& l = doc_.layers[layer_idx_];
    l.x = std::clamp(l.x + dx, 0.0f, 1.0f);
    l.y = std::clamp(l.y + dy, 0.0f, 1.0f);
    l.scale_x = std::clamp(l.scale_x + dsx, 0.02f, 2.0f);
    l.scale_y = std::clamp(l.scale_y + dsy, 0.02f, 2.0f);
    l.rot_deg = std::fmod(l.rot_deg + drot + 360.0f, 360.0f);
    dirty_ = true;
    return;
  }

  // Vinyl Group multi-layer transform: move, scale, and rotate all members
  // sharing `group_id` cohesively around the group's centroid.
  float cx = 0.0f, cy = 0.0f;
  int count = 0;
  for (const auto& ly : doc_.layers) {
    if (ly.group_id == gid) {
      cx += ly.x;
      cy += ly.y;
      ++count;
    }
  }
  if (count <= 0) return;
  cx /= float(count);
  cy /= float(count);

  const float pivot_sx = std::max(0.02f, doc_.layers[layer_idx_].scale_x);
  const float pivot_sy = std::max(0.02f, doc_.layers[layer_idx_].scale_y);
  const float rx = std::clamp(pivot_sx + dsx, 0.02f, 2.0f) / pivot_sx;
  const float ry = std::clamp(pivot_sy + dsy, 0.02f, 2.0f) / pivot_sy;
  const float rad = drot * (3.14159265358979323846f / 180.0f);
  const float cs = std::cos(rad);
  const float sn = std::sin(rad);

  for (auto& ly : doc_.layers) {
    if (ly.group_id != gid) continue;
    float ox = (ly.x - cx) * rx;
    float oy = (ly.y - cy) * ry;
    if (std::fabs(drot) > 1e-5f) {
      const float nx = ox * cs - oy * sn;
      const float ny = ox * sn + oy * cs;
      ox = nx;
      oy = ny;
    }
    ly.x = std::clamp(cx + ox + dx, 0.0f, 1.0f);
    ly.y = std::clamp(cy + oy + dy, 0.0f, 1.0f);
    ly.scale_x = std::clamp(ly.scale_x * rx, 0.02f, 2.0f);
    ly.scale_y = std::clamp(ly.scale_y * ry, 0.02f, 2.0f);
    ly.rot_deg = std::fmod(ly.rot_deg + drot + 360.0f, 360.0f);
  }
  dirty_ = true;
}

void LiveryEditor::transform_layer(float dx, float dy, float dsx, float dsy, float drot) {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  apply_transform_impl(dx, dy, dsx, dsy, drot);
}

void LiveryEditor::transform_layer_continuous(float dx, float dy, float dsx, float dsy, float drot, double now_s) {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo_coalesced(now_s);
  apply_transform_impl(dx, dy, dsx, dsy, drot);
  mark_vinyl_preview_dirty(now_s);
}

void LiveryEditor::cycle_layer_shape(int by) {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  static const char* kBuiltins[] = {"@rect", "@circle", "@star", "@chevron"};
  const size_t nv = lib_.vinyl_names().size();
  const size_t nd = lib_.decal_names().size();
  auto step_idx = [&](size_t count) -> size_t {
    if (count == 0) return 0;
    const int n = int(count);
    shape_cursor_ = size_t(((int(shape_cursor_) + by) % n + n) % n);
    return shape_cursor_;
  };
  if (shape_cat_ == ShapeCategory::kBuiltins || (nv == 0 && nd == 0)) {
    doc_.layers[layer_idx_].shape_path = kBuiltins[step_idx(4)];
  } else if (shape_cat_ == ShapeCategory::kVinyls && nv > 0) {
    doc_.layers[layer_idx_].shape_path = lib_.vinyl_names()[step_idx(nv)];
  } else if (shape_cat_ == ShapeCategory::kDecals && nd > 0) {
    doc_.layers[layer_idx_].shape_path = lib_.decal_names()[step_idx(nd)];
  } else {
    const size_t total = nv + nd;
    const size_t idx = step_idx(total);
    doc_.layers[layer_idx_].shape_path = (idx < nv) ? lib_.vinyl_names()[idx] : lib_.decal_names()[idx - nv];
  }
  dirty_ = true;
}

void LiveryEditor::cycle_shape_category(int by) {
  const int n = int(ShapeCategory::kCount);
  shape_cat_ = ShapeCategory(((int(shape_cat_) + by) % n + n) % n);
  shape_cursor_ = 0;
  if (layer_idx_ < doc_.layers.size()) cycle_layer_shape(0);
}

void LiveryEditor::cycle_layer_plane(int by) {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  const int n = int(cars::LiveryPlaneKind::kCount);
  const int gid = doc_.layers[layer_idx_].group_id;
  if (gid == 0) {
    auto& l = doc_.layers[layer_idx_];
    l.plane = cars::LiveryPlaneKind(((int(l.plane) + by) % n + n) % n);
  } else {
    const auto next_plane = cars::LiveryPlaneKind(((int(doc_.layers[layer_idx_].plane) + by) % n + n) % n);
    for (auto& ly : doc_.layers)
      if (ly.group_id == gid) ly.plane = next_plane;
  }
  dirty_ = true;
}

void LiveryEditor::recolor_layer_hsv(float dh, float ds, float dv) {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  const int gid = doc_.layers[layer_idx_].group_id;
  auto step_one = [&](cars::VinylLayer& ly) {
    float h = 0, s = 0, v = 0;
    cars::rgb_to_hsv(ly.rgb, h, s, v);
    h = std::fmod(h + dh + 1.0f, 1.0f);
    s = std::clamp(s + ds, 0.0f, 1.0f);
    v = std::clamp(v + dv, 0.0f, 1.0f);
    ly.rgb = cars::hsv_to_rgb(h, s, v);
  };
  if (gid == 0) {
    step_one(doc_.layers[layer_idx_]);
  } else {
    for (auto& ly : doc_.layers)
      if (ly.group_id == gid) step_one(ly);
  }
  dirty_ = true;
}

void LiveryEditor::toggle_layer_flip(bool horizontal) {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  const int gid = doc_.layers[layer_idx_].group_id;
  if (gid == 0) {
    auto& l = doc_.layers[layer_idx_];
    if (horizontal) l.flip_x = !l.flip_x;
    else l.flip_y = !l.flip_y;
  } else {
    float cx = 0.0f, cy = 0.0f;
    int count = 0;
    for (const auto& ly : doc_.layers) {
      if (ly.group_id == gid) { cx += ly.x; cy += ly.y; ++count; }
    }
    if (count > 0) { cx /= float(count); cy /= float(count); }
    for (auto& ly : doc_.layers) {
      if (ly.group_id != gid) continue;
      if (horizontal) {
        ly.flip_x = !ly.flip_x;
        ly.x = std::clamp(2.0f * cx - ly.x, 0.0f, 1.0f);
      } else {
        ly.flip_y = !ly.flip_y;
        ly.y = std::clamp(2.0f * cy - ly.y, 0.0f, 1.0f);
      }
    }
  }
  dirty_ = true;
}

void LiveryEditor::adjust_layer_skew(float dskew) {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  const int gid = doc_.layers[layer_idx_].group_id;
  if (gid == 0) {
    auto& l = doc_.layers[layer_idx_];
    l.skew = std::clamp(l.skew + dskew, -2.0f, 2.0f);
  } else {
    for (auto& ly : doc_.layers)
      if (ly.group_id == gid) ly.skew = std::clamp(ly.skew + dskew, -2.0f, 2.0f);
  }
  dirty_ = true;
}

void LiveryEditor::adjust_layer_alpha(float dalpha) {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  const int gid = doc_.layers[layer_idx_].group_id;
  if (gid == 0) {
    auto& l = doc_.layers[layer_idx_];
    l.alpha = std::clamp(l.alpha + dalpha, 0.05f, 1.0f);
  } else {
    for (auto& ly : doc_.layers)
      if (ly.group_id == gid) ly.alpha = std::clamp(ly.alpha + dalpha, 0.05f, 1.0f);
  }
  dirty_ = true;
}

void LiveryEditor::toggle_layer_mask() {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  auto& l = doc_.layers[layer_idx_];
  l.is_mask = !l.is_mask;
  dirty_ = true;
}

void LiveryEditor::toggle_layer_group() {
  if (layer_idx_ >= doc_.layers.size()) return;
  push_undo();
  auto& l = doc_.layers[layer_idx_];
  if (l.group_id != 0) {
    l.group_id = 0;
  } else if (layer_idx_ > 0 && doc_.layers[layer_idx_ - 1].group_id != 0) {
    l.group_id = doc_.layers[layer_idx_ - 1].group_id;
  } else {
    const uint32_t gid = next_group_id_++;
    l.group_id = int(gid);
    if (layer_idx_ > 0) doc_.layers[layer_idx_ - 1].group_id = int(gid);
  }
  dirty_ = true;
}

bool LiveryEditor::group_layers(const std::vector<size_t>& indices) {
  if (indices.size() < 2 || doc_.layers.empty()) return false;
  std::vector<size_t> valid;
  valid.reserve(indices.size());
  for (size_t idx : indices) {
    if (idx < doc_.layers.size() && std::find(valid.begin(), valid.end(), idx) == valid.end()) {
      valid.push_back(idx);
    }
  }
  if (valid.size() < 2) return false;
  push_undo();
  const int gid = int(next_group_id_++);
  for (size_t idx : valid) doc_.layers[idx].group_id = gid;
  layer_idx_ = valid.front();
  dirty_ = true;
  return true;
}

bool LiveryEditor::ungroup_selected_group() {
  if (layer_idx_ >= doc_.layers.size()) return false;
  const int gid = doc_.layers[layer_idx_].group_id;
  if (gid == 0) return false;
  push_undo();
  for (auto& ly : doc_.layers) {
    if (ly.group_id == gid) ly.group_id = 0;
  }
  dirty_ = true;
  return true;
}

bool LiveryEditor::transform_selected_group(float dx, float dy, float scale_x_factor, float scale_y_factor,
                                            float drot_deg) {
  if (layer_idx_ >= doc_.layers.size()) return false;
  const int gid = doc_.layers[layer_idx_].group_id;
  push_undo();
  cars::VinylGroup vg;
  std::vector<size_t> member_indices;
  for (size_t i = 0; i < doc_.layers.size(); ++i) {
    if (gid != 0 ? (doc_.layers[i].group_id == gid) : (i == layer_idx_)) {
      vg.layers.push_back(doc_.layers[i]);
      member_indices.push_back(i);
    }
  }
  cars::transform_vinyl_group(vg, dx, dy, scale_x_factor, scale_y_factor, drot_deg);
  for (size_t k = 0; k < member_indices.size(); ++k) {
    doc_.layers[member_indices[k]] = vg.layers[k];
  }
  dirty_ = true;
  return true;
}

bool LiveryEditor::duplicate_selected_layer() {
  if (layer_idx_ >= doc_.layers.size()) return false;
  push_undo();
  const int gid = doc_.layers[layer_idx_].group_id;
  if (gid == 0) {
    cars::VinylLayer dup = doc_.layers[layer_idx_];
    dup.x = std::clamp(dup.x + 0.03f, 0.0f, 1.0f);
    dup.y = std::clamp(dup.y + 0.03f, 0.0f, 1.0f);
    doc_.layers.insert(doc_.layers.begin() + ptrdiff_t(layer_idx_ + 1), dup);
    layer_idx_ += 1;
  } else {
    const int new_gid = int(next_group_id_++);
    std::vector<cars::VinylLayer> copies;
    for (const auto& ly : doc_.layers) {
      if (ly.group_id != gid) continue;
      cars::VinylLayer dup = ly;
      dup.group_id = new_gid;
      dup.x = std::clamp(dup.x + 0.03f, 0.0f, 1.0f);
      dup.y = std::clamp(dup.y + 0.03f, 0.0f, 1.0f);
      copies.push_back(std::move(dup));
    }
    const size_t ins = doc_.layers.size();
    doc_.layers.insert(doc_.layers.end(), copies.begin(), copies.end());
    layer_idx_ = ins;
  }
  dirty_ = true;
  return true;
}

bool LiveryEditor::mirror_selected_layer() {
  if (layer_idx_ >= doc_.layers.size()) return false;
  push_undo();
  const int gid = doc_.layers[layer_idx_].group_id;
  if (gid != 0) {
    cars::VinylGroup src;
    for (const auto& ly : doc_.layers) {
      if (ly.group_id == gid) src.layers.push_back(ly);
    }
    cars::VinylGroup mirrored = cars::mirror_vinyl_group(src, true);
    const int new_gid = int(next_group_id_++);
    const size_t ins = doc_.layers.size();
    for (auto& ml : mirrored.layers) {
      ml.group_id = new_gid;
      doc_.layers.push_back(std::move(ml));
    }
    layer_idx_ = ins;
    dirty_ = true;
    return true;
  }
  cars::VinylGroup single;
  single.layers.push_back(doc_.layers[layer_idx_]);
  cars::VinylGroup mirrored = cars::mirror_vinyl_group(single, true);
  doc_.layers.insert(doc_.layers.begin() + ptrdiff_t(layer_idx_ + 1), mirrored.layers.front());
  layer_idx_ += 1;
  dirty_ = true;
  return true;
}

void LiveryEditor::scan_vinyl_group_catalog(const std::filesystem::path& groups_dir) {
  vg_catalog_.clear();
  vg_idx_ = 0;
  vg_dir_ = groups_dir;
  if (groups_dir.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_directory(groups_dir, ec)) return;
  for (const auto& entry : std::filesystem::directory_iterator(groups_dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (entry.path().extension() != ".vg") continue;
    cars::VinylGroup tmp;
    if (!cars::load_vinyl_group_file(entry.path(), tmp, nullptr)) continue;
    VinylGroupCatalogEntry e;
    e.path = entry.path();
    e.name = tmp.name.empty() ? entry.path().stem().string() : tmp.name;
    e.layer_count = tmp.layers.size();
    vg_catalog_.push_back(std::move(e));
  }
  std::sort(vg_catalog_.begin(), vg_catalog_.end(),
            [](const VinylGroupCatalogEntry& a, const VinylGroupCatalogEntry& b) { return a.name < b.name; });
}

void LiveryEditor::move_vinyl_group_selection(int by) {
  if (vg_catalog_.empty()) return;
  const int n = int(vg_catalog_.size());
  vg_idx_ = size_t(((int(vg_idx_) + by) % n + n) % n);
}

void LiveryEditor::select_vinyl_group_index(size_t idx) {
  if (vg_catalog_.empty()) return;
  vg_idx_ = std::min(idx, vg_catalog_.size() - 1);
}

bool LiveryEditor::save_selected_group_stamp(const std::filesystem::path& groups_dir, const std::string& group_name,
                                             std::string* error) {
  if (layer_idx_ >= doc_.layers.size()) {
    if (error) *error = "no vinyl layer selected";
    return false;
  }
  const std::filesystem::path dir = !groups_dir.empty() ? groups_dir : vg_dir_;
  if (dir.empty()) {
    if (error) *error = "no vinyl groups directory";
    return false;
  }
  const int gid = doc_.layers[layer_idx_].group_id;
  cars::VinylGroup vg;
  const std::string clean_name = group_name.empty() ? ("Group_" + std::to_string(vg_catalog_.size() + 1)) : group_name;
  if (gid != 0) {
    vg = cars::extract_vinyl_group(doc_.layers, gid, clean_name);
  } else {
    vg.name = clean_name;
    cars::VinylLayer one = doc_.layers[layer_idx_];
    one.x = 0.5f;
    one.y = 0.5f;
    one.group_id = 0;
    vg.layers.push_back(std::move(one));
  }
  std::string stem;
  for (unsigned char ch : clean_name) {
    if (std::isalnum(ch) || ch == '-' || ch == '_') stem.push_back(char(ch));
    else if (ch == ' ') stem.push_back('_');
  }
  if (stem.empty()) stem = "vinyl_group";
  const auto dst = dir / (stem + ".vg");
  if (!cars::save_vinyl_group_file(dst, vg, error)) return false;
  scan_vinyl_group_catalog(dir);
  for (size_t i = 0; i < vg_catalog_.size(); ++i) {
    if (vg_catalog_[i].path == dst) {
      vg_idx_ = i;
      break;
    }
  }
  return true;
}

bool LiveryEditor::stamp_vinyl_group_file(const std::filesystem::path& vg_path, cars::LiveryPlaneKind target_plane,
                                          float cx, float cy, std::string* error) {
  cars::VinylGroup vg;
  if (!cars::load_vinyl_group_file(vg_path, vg, error)) return false;
  push_undo();
  const int new_gid = int(next_group_id_++);
  auto stamped = cars::instantiate_vinyl_group(vg, target_plane, cx, cy, 1.0f, 1.0f, 0.0f, new_gid);
  if (stamped.empty()) {
    undo_stack_.pop_back();
    if (error) *error = "vinyl group has no layers";
    return false;
  }
  const size_t ins = doc_.layers.size();
  for (auto& ly : stamped) doc_.layers.push_back(std::move(ly));
  layer_idx_ = ins;
  dirty_ = true;
  return true;
}

bool LiveryEditor::stamp_selected_vinyl_group(std::string* error) {
  if (vg_catalog_.empty() || vg_idx_ >= vg_catalog_.size()) {
    if (error) *error = "no vinyl group selected";
    return false;
  }
  const cars::LiveryPlaneKind target_plane =
      (layer_idx_ < doc_.layers.size()) ? doc_.layers[layer_idx_].plane : cars::LiveryPlaneKind::kFullAtlas;
  return stamp_vinyl_group_file(vg_catalog_[vg_idx_].path, target_plane, 0.5f, 0.5f, error);
}

bool LiveryEditor::copy_selected_layer() {
  if (layer_idx_ >= doc_.layers.size()) return false;
  clipboard_ = doc_.layers[layer_idx_];
  return true;
}

bool LiveryEditor::paste_layer() {
  if (!clipboard_.has_value()) return false;
  push_undo();
  cars::VinylLayer copy = *clipboard_;
  copy.x = std::clamp(copy.x + 0.03f, 0.0f, 1.0f);
  copy.y = std::clamp(copy.y + 0.03f, 0.0f, 1.0f);
  if (doc_.layers.empty()) {
    doc_.layers.push_back(copy);
    layer_idx_ = 0;
  } else {
    const size_t ins = std::min(layer_idx_ + 1, doc_.layers.size());
    doc_.layers.insert(doc_.layers.begin() + ptrdiff_t(ins), copy);
    layer_idx_ = ins;
  }
  dirty_ = true;
  return true;
}

void LiveryEditor::mark_vinyl_preview_dirty(double now_s) {
  vinyl_preview_pending_ = true;
  last_vinyl_dirty_s_ = now_s;
}

bool LiveryEditor::needs_vinyl_preview_upload(double now_s) const {
  if (!vinyl_preview_pending_) return false;
  return (now_s - last_vinyl_upload_s_) >= preview_throttle_s_;
}

void LiveryEditor::mark_vinyl_preview_uploaded(double now_s) {
  vinyl_preview_pending_ = false;
  last_vinyl_upload_s_ = now_s;
}

void LiveryEditor::scan_design_catalog(const std::filesystem::path& liveries_dir) {
  catalog_.clear();
  catalog_idx_ = 0;
  catalog_dir_ = liveries_dir;
  if (liveries_dir.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_directory(liveries_dir, ec)) return;
  for (const auto& entry : std::filesystem::directory_iterator(liveries_dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (entry.path().extension() != ".liv") continue;
    cars::LiveryDocument tmp;
    if (!cars::load_livery_file(entry.path(), tmp, nullptr)) continue;
    DesignCatalogEntry de;
    de.path = entry.path();
    de.name = tmp.name.empty() ? entry.path().stem().string() : tmp.name;
    de.car_id = tmp.car_id;
    de.layer_count = tmp.layers.size();
    catalog_.push_back(std::move(de));
  }
  std::sort(catalog_.begin(), catalog_.end(),
            [](const DesignCatalogEntry& a, const DesignCatalogEntry& b) { return a.name < b.name; });
}

void LiveryEditor::move_design_selection(int by) {
  if (catalog_.empty()) return;
  const int n = int(catalog_.size());
  catalog_idx_ = size_t(((int(catalog_idx_) + by) % n + n) % n);
}

void LiveryEditor::select_design_index(size_t idx) {
  if (catalog_.empty()) return;
  catalog_idx_ = std::min(idx, catalog_.size() - 1);
}

bool LiveryEditor::save_to_catalog(const std::filesystem::path& liveries_dir, const std::string& design_name,
                                   std::string* error) {
  if (liveries_dir.empty()) {
    if (error) *error = "no liveries directory";
    return false;
  }
  std::string clean_name = design_name.empty() ? ("Design_" + std::to_string(doc_.car_id)) : design_name;
  std::string stem;
  for (unsigned char ch : clean_name) {
    if (std::isalnum(ch) || ch == '-' || ch == '_') stem.push_back(char(ch));
    else if (ch == ' ') stem.push_back('_');
  }
  if (stem.empty()) stem = "design";
  doc_.name = clean_name;
  const auto dst = liveries_dir / (stem + ".liv");
  if (!cars::save_livery_file(dst, doc_, error)) return false;
  scan_design_catalog(liveries_dir);
  for (size_t i = 0; i < catalog_.size(); ++i) {
    if (catalog_[i].path == dst) {
      catalog_idx_ = i;
      break;
    }
  }
  return true;
}

bool LiveryEditor::load_selected_design(std::string* error) {
  if (catalog_.empty() || catalog_idx_ >= catalog_.size()) {
    if (error) *error = "no design selected";
    return false;
  }
  push_undo();
  const int64_t keep_car_id = doc_.car_id;
  if (!cars::load_livery_file(catalog_[catalog_idx_].path, doc_, error)) {
    undo_stack_.pop_back();
    return false;
  }
  if (keep_car_id > 0) doc_.car_id = keep_car_id;
  layer_idx_ = 0;
  if (region_ != PaintRegion::kWindowTint) {
    cars::rgb_to_hsv(active_region_spec().rgb, cur_h_, cur_s_, cur_v_);
  }
  dirty_ = true;
  return true;
}

bool LiveryEditor::save(const std::filesystem::path& path, std::string* error) const {
  return cars::save_livery_file(path, doc_, error);
}

bool LiveryEditor::load(const std::filesystem::path& path, std::string* error) {
  push_undo();
  if (!cars::load_livery_file(path, doc_, error)) {
    undo_stack_.pop_back();
    return false;
  }
  dirty_ = true;
  return true;
}

bool LiveryEditor::composite_1024(const std::vector<cars::bake::LiveryPlane>* planes, std::vector<uint8_t>& out_rgba,
                                  uint32_t size) {
  return cars::composite_livery(doc_, lib_.is_open() ? &lib_ : nullptr, planes, out_rgba, size,
                                plane_masks_.empty() ? nullptr : &plane_masks_);
}

const cars::VinylMesh* LiveryEditor::preview_shape_mesh(const std::string& shape_path) const {
  if (shape_path.empty() || shape_path[0] == '@' || !lib_.is_open()) return nullptr;
  return lib_.load_shape(shape_path, nullptr);
}

bool LiveryEditor::handle_step_click(int dir) {
  if (dir == 0) return false;
  switch (tab_) {
    case EditorTab::kPaint:
      if (region_ == PaintRegion::kWindowTint) step_window_tint(0.10f * float(dir));
      else move_finish(dir);
      return true;
    case EditorTab::kManufacturerColors:
      move_manufacturer_color(dir);
      return true;
    case EditorTab::kSpecialColors:
      move_special_color(dir);
      return true;
    case EditorTab::kPreviousColors:
      move_previous_color(dir);
      return true;
    case EditorTab::kVinylLayers:
      if (!doc_.layers.empty()) {
        select_layer(dir);
        return true;
      }
      return false;
    default:
      return false;
  }
}

bool LiveryEditor::handle_button_bar_click(int button_index) {
  const bool vinyl_tab = (tab_ == EditorTab::kVinylLayers);
  switch (button_index) {
    case 1:
      if (vinyl_tab && can_undo()) return undo();
      move_tab(1);
      return true;
    case 2:
      return handle_step_click(1);
    case 3:
      if (vinyl_tab) add_vinyl_layer();
      else move_region(1);
      return true;
    case 4:
      if (vinyl_tab) delete_selected_layer();
      else reset_to_factory_paint();
      return true;
    default:
      return false;
  }
}

namespace {

// Rasterizes a crisp 32x32 anti-aliased preview thumbnail for a vinyl shape
// (either an authored Livery.zip VinylMesh or a built-in @primitive) and caches
// it in the transient UI atlas so subsequent frames are O(1) lookups.
bool ensure_vinyl_thumbnail_uv(const std::string& shape_path, const cars::VinylMesh* mesh,
                               bool flip_x, bool flip_y, std::array<float, 4>& out_uv,
                               bool& out_is_multicolor) {
  out_is_multicolor = (mesh && mesh->is_decal);
  const std::string key = "liv.thumb." + (shape_path.empty() ? std::string("@rect") : shape_path) +
                          (mesh ? ".m" : ".p") + (flip_x ? ".fx" : "") + (flip_y ? ".fy" : "");
  if (ui::atlas_find(key, out_uv)) return true;

  constexpr uint32_t kSz = 32;
  std::vector<uint8_t> rgba(kSz * kSz * 4u, 0u);
  auto put_px = [&](uint32_t px, uint32_t py, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (px >= kSz || py >= kSz || a == 0) return;
    const size_t o = (size_t(py) * kSz + px) * 4u;
    const float sa = float(a) / 255.0f;
    const float da = float(rgba[o + 3]) / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 1e-4f) return;
    rgba[o + 0] = uint8_t(std::clamp(std::lround((float(r) * sa + float(rgba[o + 0]) * da * (1.0f - sa)) / oa), 0L, 255L));
    rgba[o + 1] = uint8_t(std::clamp(std::lround((float(g) * sa + float(rgba[o + 1]) * da * (1.0f - sa)) / oa), 0L, 255L));
    rgba[o + 2] = uint8_t(std::clamp(std::lround((float(b) * sa + float(rgba[o + 2]) * da * (1.0f - sa)) / oa), 0L, 255L));
    rgba[o + 3] = uint8_t(std::clamp(std::lround(oa * 255.0f), 0L, 255L));
  };

  if (mesh && !mesh->vertices.empty() && mesh->indices.size() >= 3) {
    const float mw = std::max(1e-4f, mesh->max_x - mesh->min_x);
    const float mh = std::max(1e-4f, mesh->max_y - mesh->min_y);
    const float mdim = std::max(mw, mh);
    const float mcx = 0.5f * (mesh->min_x + mesh->max_x);
    const float mcy = 0.5f * (mesh->min_y + mesh->max_y);
    const float pad = 3.0f;
    const float span = float(kSz) - 2.0f * pad;
    auto map_x = [&](float vx) {
      float nx = (vx - mcx) / mdim;
      if (flip_x) nx = -nx;
      return 0.5f * float(kSz) + nx * span;
    };
    auto map_y = [&](float vy) {
      float ny = (vy - mcy) / mdim;
      if (flip_y) ny = -ny;
      return 0.5f * float(kSz) + ny * span;
    };
    auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
      return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    };
    for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
      const uint32_t i0 = mesh->indices[t], i1 = mesh->indices[t + 1], i2 = mesh->indices[t + 2];
      if (i0 >= mesh->vertices.size() || i1 >= mesh->vertices.size() || i2 >= mesh->vertices.size()) continue;
      const auto& v0 = mesh->vertices[i0];
      const auto& v1 = mesh->vertices[i1];
      const auto& v2 = mesh->vertices[i2];
      const float x0 = map_x(v0.x), y0 = map_y(v0.y);
      const float x1 = map_x(v1.x), y1 = map_y(v1.y);
      const float x2 = map_x(v2.x), y2 = map_y(v2.y);
      const float area = edge(x0, y0, x1, y1, x2, y2);
      if (std::fabs(area) < 1e-4f) continue;
      const int min_px = std::clamp(int(std::floor(std::min({x0, x1, x2}))), 0, int(kSz) - 1);
      const int max_px = std::clamp(int(std::ceil(std::max({x0, x1, x2}))), 0, int(kSz) - 1);
      const int min_py = std::clamp(int(std::floor(std::min({y0, y1, y2}))), 0, int(kSz) - 1);
      const int max_py = std::clamp(int(std::ceil(std::max({y0, y1, y2}))), 0, int(kSz) - 1);
      const uint32_t c0 = v0.argb;
      const uint8_t r = mesh->is_decal ? uint8_t((c0 >> 16) & 0xFFu) : 255u;
      const uint8_t g = mesh->is_decal ? uint8_t((c0 >> 8) & 0xFFu) : 255u;
      const uint8_t b = mesh->is_decal ? uint8_t(c0 & 0xFFu) : 255u;
      const uint8_t a_base = mesh->is_decal ? uint8_t((c0 >> 24) & 0xFFu) : 255u;
      for (int py = min_py; py <= max_py; ++py) {
        for (int px = min_px; px <= max_px; ++px) {
          int hits = 0;
          for (int sy = 0; sy < 2; ++sy) {
            for (int sx = 0; sx < 2; ++sx) {
              const float qx = float(px) + 0.25f + 0.5f * float(sx);
              const float qy = float(py) + 0.25f + 0.5f * float(sy);
              const float w0 = edge(x1, y1, x2, y2, qx, qy);
              const float w1 = edge(x2, y2, x0, y0, qx, qy);
              const float w2 = edge(x0, y0, x1, y1, qx, qy);
              const bool in = (area > 0.0f) ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                                            : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
              if (in) ++hits;
            }
          }
          if (hits > 0) {
            put_px(uint32_t(px), uint32_t(py), r, g, b, uint8_t((int(a_base) * hits) / 4));
          }
        }
      }
    }
  } else {
    // Crisp built-in vector primitive preview (@circle, @star, @chevron, @stripe, @rect, or shield fallback).
    std::string sp = shape_path;
    for (char& c : sp) c = char(std::tolower(static_cast<unsigned char>(c)));
    const bool is_circle = (sp.find("circle") != std::string::npos);
    const bool is_star = (sp.find("star") != std::string::npos);
    const bool is_chevron = (sp.find("chevron") != std::string::npos || sp.find("arrow") != std::string::npos);
    const bool is_stripe = (sp.find("stripe") != std::string::npos);
    const bool is_rect = (sp.empty() || sp.find("rect") != std::string::npos || sp.find("square") != std::string::npos);
    for (uint32_t py = 0; py < kSz; ++py) {
      for (uint32_t px = 0; px < kSz; ++px) {
        int hits = 0;
        for (int sy = 0; sy < 2; ++sy) {
          for (int sx = 0; sx < 2; ++sx) {
            float u = ((float(px) + 0.25f + 0.5f * float(sx)) / float(kSz)) * 2.0f - 1.0f;
            float v = ((float(py) + 0.25f + 0.5f * float(sy)) / float(kSz)) * 2.0f - 1.0f;
            if (flip_x) u = -u;
            if (flip_y) v = -v;
            bool in = false;
            if (is_circle) {
              in = (u * u + v * v) <= 0.78f * 0.78f;
            } else if (is_star) {
              const float r = std::hypot(u, v);
              const float ang = std::atan2(v, u);
              const float sector = std::fmod(ang + 3.14159265f * 2.5f, 3.14159265f * 0.4f);
              const float t = std::fabs(sector - 3.14159265f * 0.2f) / (3.14159265f * 0.2f);
              const float rad = 0.34f + 0.50f * (1.0f - t);
              in = (r <= rad);
            } else if (is_chevron) {
              const float band = v + 0.55f * std::fabs(u);
              in = (std::fabs(u) <= 0.80f && band >= -0.22f && band <= 0.32f);
            } else if (is_stripe) {
              in = (std::fabs(v) <= 0.82f && (std::fabs(u + 0.34f) <= 0.20f || std::fabs(u - 0.34f) <= 0.20f));
            } else if (is_rect) {
              in = (std::fabs(u) <= 0.74f && std::fabs(v) <= 0.56f);
            } else {
              // Stylised racing badge diamond/chevron silhouette for external shapes when Livery.zip is absent.
              in = (std::fabs(u) + std::fabs(v) <= 0.82f) && !(std::fabs(u) + std::fabs(v) <= 0.28f);
            }
            if (in) ++hits;
          }
        }
        if (hits > 0) {
          put_px(px, py, 255u, 255u, 255u, uint8_t((255 * hits) / 4));
        }
      }
    }
  }
  return ui::atlas_image(key, kSz, kSz, rgba, out_uv, /*transient=*/true);
}

std::string format_vinyl_shape_label(const std::string& shape_path) {
  if (shape_path.empty()) return "RECTANGLE";
  if (shape_path == "@rect") return "RECTANGLE";
  if (shape_path == "@circle") return "CIRCLE";
  if (shape_path == "@star") return "STAR";
  if (shape_path == "@chevron") return "CHEVRON";
  if (shape_path == "@stripe") return "RACING STRIPE";
  std::string s = shape_path;
  const size_t slash = s.find_last_of("/\\");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  const size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  for (char& c : s) {
    if (c == '_' || c == '-') c = ' ';
    else c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  return s.empty() ? "VINYL SHAPE" : s;
}

}  // namespace

void draw_livery_editor_hud(ui::List& list, float w, float h, const LiveryEditor& editor,
                            const std::string& car_title, const std::string& notice) {
  const float s = h / 720.0f;
  ui4::subtle_vignette(list, w, h, 0.18f);

  const bool vinyl_tab = (editor.tab() == EditorTab::kVinylLayers);
  const char* scene_name = vinyl_tab ? "506_C_LIV_LAYER_SELECT" : "501_C_LIV_COLOR";
  float tilt_rad = ui4::kTiltRad;
  if (const fh1::UiScene* sc_ptr = ui4::scene(scene_name)) {
    SceneDraw sc(sc_ptr, true);
    const int n = sc.find(vinyl_tab ? "C_LIV_LAYER_LIST" : "Group_BLADE");
    if (n >= 0) {
      const float a = -sc.angle(n);
      if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) tilt_rad = a;
    }
  }

  // Top-left slim car tag pill (NEVER a giant left wall covering the 3D car!).
  {
    const float tx0 = 56.0f * s, ty0 = 26.0f * s;
    ui4::pill(list, tx0, ty0, tx0 + 340.0f * s, ty0 + 28.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu, false, true);
    const std::string hdr = car_title.empty() ? "HORIZON PAINT BOOTH" : car_title;
    ui::TextStyle cs = ui4::style(ui::Font::kE, 13.0f * s, ui4::kWhite);
    ui4::fit_text(hdr, cs, 314.0f * s, 9.0f * s);
    list.text(tx0 + 14.0f * s, ty0 + 7.0f * s, hdr, cs);
  }

  // Top-right LIV_TABS with LBUMPER / RBUMPER (#F90064 active PILL_BIG).
  const std::vector<std::string> tabs = {
      std::string("PAINT"),
      std::string("MANUFACTURER"),
      std::string("SPECIAL"),
      std::string("PREVIOUS"),
      std::string("VINYLS"),
  };
  {
    ui4::TiltScope tab_rot(list, w - 360.0f * s, 40.0f * s, tilt_rad * 0.6f);
    ui4::tab_strip(list, s, w - 680.0f * s, 24.0f * s, w - 48.0f * s, tabs, int(editor.tab()), true, ui4::kPink);
  }

  // Top-right 506_C_LIV_LAYER_SELECT LIV_CLIPBOARD ([1040,55..1184,177]) when in Vinyl Editor!
  if (vinyl_tab) {
    const float cb_x0 = w - 252.0f * s, cb_y0 = 66.0f * s, cb_x1 = w - 56.0f * s, cb_y1 = 186.0f * s;
    bool drew_scene_clip = false;
    {
      SceneScreen sc("506_C_LIV_LAYER_SELECT");
      const int cb = sc.ok() ? sc.node("CLIPBOARD") : -1;
      if (sc.ok() && cb >= 0) {
        sc.hide(sc.node("ICON_LOCK", cb));
        sc.hide(sc.node("LAYER_GROUP", cb));
        sc.draw(list, SceneScreen::frame(w, h), cb);
        drew_scene_clip = true;
      }
    }
    if (!drew_scene_clip) {
      std::array<float, 4> clip_uv{};
      if (ui4::texture("LIVERY/LIV_CLIPBOARD", clip_uv)) {
        list.image(cb_x0, cb_y0, cb_x1, cb_y1, clip_uv, ui::rgba(255, 255, 255, 235));
      } else {
        ui4::panel01(list, cb_x0, cb_y0, cb_x1, cb_y1, s, ui4::kCharcoalDark, ui4::kPink);
      }
    }
    list.text(cb_x0 + 14.0f * s, cb_y0 + 10.0f * s,
              editor.has_clipboard() ? "CLIPBOARD: COPIED" : "LIV CLIPBOARD",
              ui4::style(ui::Font::kE, 11.5f * s, editor.has_clipboard() ? ui4::kYellow : ui4::kPink));
    list.text(cb_x0 + 14.0f * s, cb_y0 + 30.0f * s,
              std::to_string(editor.document().layers.size()) + " LAYERS  •  " +
                  std::to_string(editor.vinyl_group_catalog().size()) + " VG",
              ui4::style(ui::Font::kE, 14.0f * s, ui4::kWhite));
    list.text(cb_x0 + 14.0f * s, cb_y0 + 52.0f * s,
              std::string("CAT: ") + shape_category_name(editor.shape_category()) + " (" +
                  std::to_string(editor.library_shape_count()) + ")",
              ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
    if (!editor.document().layers.empty()) {
      const auto& al = editor.document().layers[std::min(editor.selected_layer(), editor.document().layers.size() - 1)];
      char tb[96];
      std::snprintf(tb, sizeof tb, "%s • ROT %.0f° • A %.0f%%", cars::livery_plane_name(al.plane), al.rot_deg,
                    al.alpha * 100.0f);
      list.text(cb_x0 + 14.0f * s, cb_y0 + 72.0f * s, tb, ui4::style(ui::Font::kB, 10.0f * s, ui4::kYellow));
      std::string flags_txt;
      if (al.is_mask) flags_txt += "[MASK] ";
      if (al.flip_x) flags_txt += "[FH] ";
      if (al.flip_y) flags_txt += "[FV] ";
      if (std::fabs(al.skew) > 0.01f) {
        char sk[24];
        std::snprintf(sk, sizeof sk, "[SK %.1f] ", al.skew);
        flags_txt += sk;
      }
      if (al.group_id != 0) flags_txt += "[G" + std::to_string(al.group_id) + "]";
      if (!flags_txt.empty()) {
        list.text(cb_x0 + 14.0f * s, cb_y0 + 92.0f * s, flags_txt, ui4::style(ui::Font::kB, 9.5f * s, ui4::kPinkHi));
      }
    }
  } else if (editor.tab() == EditorTab::kPaint || editor.tab() == EditorTab::kManufacturerColors ||
             editor.tab() == EditorTab::kPreviousColors) {
    // Slim top-left region pill strip with authentic T:C_Map/C_LIV_Map/C_LIV_PaintCar_* icons (never blocks car!).
    static const char* kRegionIcons[6] = {
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Body",   "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Hood",
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Wing",   "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Mirror",
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Rim",    "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Body",
    };
    ui4::TiltScope reg_rot(list, 310.0f * s, 88.0f * s, tilt_rad * 0.6f);
    float rx = 56.0f * s;
    const float ry = 74.0f * s;
    // Measure each region tab width dynamically so long labels (REAR WING, WINDOW TINT) never clip.
    std::array<float, int(PaintRegion::kCount)> rwidths{};
    std::array<bool, int(PaintRegion::kCount)> rhas_icon{};
    std::array<std::array<float, 4>, int(PaintRegion::kCount)> ruvs{};
    float total_w = 0.0f;
    const float tab_gap = 5.0f * s;
    for (int i = 0; i < int(PaintRegion::kCount); ++i) {
      const bool act = (PaintRegion(i) == editor.region());
      const std::string rname = paint_region_name(PaintRegion(i));
      rhas_icon[size_t(i)] = ui4::texture(kRegionIcons[i], ruvs[size_t(i)]);
      const ui::TextStyle rs = ui4::style(act ? ui::Font::kE : ui::Font::kB, 10.5f * s, ui4::kWhite);
      const float tw = ui::List::measure(rname, rs);
      const float icon_pad = rhas_icon[size_t(i)] ? 28.0f * s : 10.0f * s;
      rwidths[size_t(i)] = std::max(act ? 92.0f * s : 74.0f * s, icon_pad + tw + 14.0f * s);
      total_w += rwidths[size_t(i)] + (i > 0 ? tab_gap : 0.0f);
    }
    const float max_strip_w = std::max(320.0f * s, w - 112.0f * s);
    const float w_scale = (total_w > max_strip_w) ? (max_strip_w / total_w) : 1.0f;
    for (int i = 0; i < int(PaintRegion::kCount); ++i) {
      const bool act = (PaintRegion(i) == editor.region());
      const std::string rname = paint_region_name(PaintRegion(i));
      const float rw = rwidths[size_t(i)] * w_scale;
      list.add_hit(rx, ry - 1.0f * s, rx + rw, ry + 29.0f * s, ui4::kHitRegion, i);
      if (act) {
        ui4::brush(list, rx, ry - 1.0f * s, rx + rw, ry + 29.0f * s, ui4::kPink);
      } else {
        ui4::pill(list, rx, ry, rx + rw, ry + 28.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
      }
      float tx = rx + 8.0f * s;
      if (rhas_icon[size_t(i)]) {
        list.image(rx + 5.0f * s, ry + 4.0f * s, rx + 23.0f * s, ry + 22.0f * s, ruvs[size_t(i)],
                   act ? ui4::kWhite : ui4::kDim);
        tx = rx + 26.0f * s;
      }
      ui::TextStyle rs = ui4::style(act ? ui::Font::kE : ui::Font::kB, 10.5f * s, act ? ui4::kWhite : ui4::kDim);
      ui4::fit_text(rname, rs, std::max(24.0f * s, rx + rw - tx - 6.0f * s), 7.5f * s);
      list.text(tx, ry + 8.5f * s, rname, rs);
      rx += rw + tab_gap * w_scale;
    }
  }

  // ---- Bottom +3.0 deg Group_BLADE@(640,571) / C_LIV_LAYER_LIST@(640,539) — 3D car 100% unobstructed! ----
  {
    ui4::TiltScope rot(list, 0.5f * w, 0.5f * h, tilt_rad);
    const float left_x = 68.0f * s, right_x = w - 68.0f * s;
    const float blade_w = right_x - left_x;

    if (editor.tab() == EditorTab::kPaint) {
      // 501_C_LIV_COLOR: TitleTapeText@(87.4, 466) + Finish selector + Bottom +3.0 deg Swatch Ribbon (y=518..636)
      const bool is_tint = (editor.region() == PaintRegion::kWindowTint);
      const cars::PaintRegionSpec& active_spec = editor.current_region_spec();
      const uint8_t tint_shade =
          uint8_t(std::clamp(std::lround((1.0f - editor.document().paint.window_tint) * 190.0f + 18.0f), 18L, 208L));
      const uint32_t cur_rgb = is_tint
                                   ? ((uint32_t(tint_shade) << 16) | (uint32_t(tint_shade) << 8) | uint32_t(tint_shade))
                                   : active_spec.rgb;
      const uint32_t cur_col =
          ui::rgba(uint8_t((cur_rgb >> 16) & 0xFF), uint8_t((cur_rgb >> 8) & 0xFF), uint8_t(cur_rgb & 0xFF), 255);

      const std::string paint_title = std::string("PAINT  //  ") + paint_region_name(editor.region());
      bool drew_paint_tape = false;
      {
        SceneScreen sc("501_C_LIV_COLOR");
        const int tt_grp = sc.ok() ? sc.node("Group_BLADE/Group_BG/TitleTape") : -1;
        const int tt_txt = sc.ok() ? sc.node("TitleTapeText", tt_grp) : -1;
        if (sc.ok() && tt_grp >= 0 && tt_txt >= 0) {
          const SceneDraw& sd = sc.scene();
          sc.bindings().fit_text = false;
          sc.text(tt_txt, paint_title);
          const float want_r = sd.at(tt_txt)[0] + sd.measure(tt_txt, paint_title) + 36.0f;
          for (const char* tname : {"Crisp_Tape", "Smudge_Tape"}) {
            if (const int tn = sc.node(tname, tt_grp); tn >= 0) {
              const auto rr = sd.rect(tn);
              sc.at(tn).visible_width = std::clamp(want_r - rr[0], 120.0f, rr[2] - rr[0]);
            }
          }
          sc.hide(sc.node("Clipon", tt_grp));
          // Unturn temporarily because SceneScreen applies 501_C_LIV_COLOR's native tilt.
          list.unturn();
          sc.draw(list, SceneScreen::frame(w, h), tt_grp);
          list.turn(0.5f * w, 0.5f * h, tilt_rad);
          drew_paint_tape = true;
        }
      }
      if (!drew_paint_tape) {
        ui4::title_tape(list, s, 86.0f * s, 456.0f * s, paint_title, ui4::kBrushBlack, 30.0f, 440.0f);
      }

      // Finish selector pill with authentic OPT_BAR_ARROW + live CAR_PAINT_CURRENT_FRAME swatch right of title tape!
      const float fx0 = w - 456.0f * s, fy0 = 468.0f * s, fx1 = right_x;
      ui4::pill(list, fx0, fy0, fx1, fy0 + 34.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02, false, true);
      list.text(fx0 + 14.0f * s, fy0 + 10.0f * s, is_tint ? "TINT" : "FINISH",
                ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
      const float ar_l = fx0 + 78.0f * s, ar_r = fx1 - 78.0f * s;
      list.add_hit(ar_l - 18.0f * s, fy0, ar_l + 18.0f * s, fy0 + 34.0f * s, ui::kHitStepLeft, 0);
      list.add_hit(ar_r - 18.0f * s, fy0, ar_r + 18.0f * s, fy0 + 34.0f * s, ui::kHitStepRight, 0);
      ui4::opt_arrow(list, ar_l, fy0 + 17.0f * s, 13.0f * s, -1, ui4::kPinkHi);
      char tint_buf[32];
      std::snprintf(tint_buf, sizeof tint_buf, "%d%% DARKNESS",
                    int(std::lround(editor.document().paint.window_tint * 100.0f)));
      const std::string fname =
          is_tint ? std::string(tint_buf)
                  : loc_key("Paint:" + std::string(cars::paint_finish_name(active_spec.finish)),
                            paint_finish_display_name(active_spec.finish));
      ui::TextStyle fns = ui4::style(ui::Font::kE, 14.0f * s, ui4::kWhite, ui::Align::kCenter);
      ui4::fit_text(fname, fns, (ar_r - ar_l) - 24.0f * s, 9.5f * s);
      list.text(0.5f * (ar_l + ar_r), fy0 + 0.5f * (34.0f * s - fns.size), fname, fns);
      ui4::opt_arrow(list, ar_r, fy0 + 17.0f * s, 13.0f * s, +1, ui4::kPinkHi);

      const float cf_x0 = fx1 - 60.0f * s, cf_y0 = fy0 + 4.0f * s;
      list.rect(cf_x0, cf_y0, cf_x0 + 48.0f * s, cf_y0 + 26.0f * s, cur_col);
      std::array<float, 4> cfr_uv{};
      if (ui4::texture("LIVERY/CAR_PAINT_CURRENT_FRAME", cfr_uv)) {
        list.image(cf_x0 - 2.0f * s, cf_y0 - 2.0f * s, cf_x0 + 50.0f * s, cf_y0 + 28.0f * s, cfr_uv);
      } else {
        list.frame(cf_x0, cf_y0, cf_x0 + 48.0f * s, cf_y0 + 26.0f * s, ui4::kWhite, 1.5f * s);
      }

      // Bottom +3.0 deg Swatch Ribbon (18 cols x 4 rows = 72 vivid swatches, y=512..624, #D41C46 focus)
      const float gy = 512.0f * s, gh = 110.0f * s;
      std::array<float, 4> grad_uv{}, chip_uv{}, met_uv{}, mat_uv{}, foc_uv{};
      const bool has_grad = ui4::texture("LIVERY/CAR_PAINT_LIST_GRADIENT", grad_uv);
      const bool has_chip = ui4::texture("LIVERY/CAR_PAINT_GRID_CHIP", chip_uv);
      const bool has_met = ui4::texture("LIVERY/CAR_PAINT_GRID_METALLIC", met_uv);
      const bool has_mat = ui4::texture("LIVERY/CAR_PAINT_GRID_MATTE", mat_uv);
      const bool has_foc = ui4::texture("LIVERY/CAR_PAINT_GRID_FOCUS", foc_uv) ||
                           ui4::texture("LIVERY/CAR_PAINT_CURRENT_FOCUS", foc_uv);
      const bool use_met = !is_tint && (active_spec.finish == cars::PaintFinish::kMetallic ||
                                        active_spec.finish == cars::PaintFinish::kTwoTone);
      const bool use_mat = !is_tint && (active_spec.finish == cars::PaintFinish::kMatte);

      ui4::panel01(list, left_x - 8.0f * s, gy - 6.0f * s, right_x + 8.0f * s, gy + gh + 6.0f * s, s,
                   ui::rgba(16, 18, 22, 218), ui4::kPink);
      if (has_grad) {
        list.image(left_x - 8.0f * s, gy - 6.0f * s, right_x + 8.0f * s, gy + gh + 6.0f * s, grad_uv,
                   ui::rgba(255, 255, 255, 42));
      }

      constexpr int kCols = 18, kRows = 4;
      const float cw = blade_w / float(kCols);
      const float ch = gh / float(kRows);
      float cur_h = 0, cur_s = 0, cur_v = 0;
      cars::rgb_to_hsv(cur_rgb, cur_h, cur_s, cur_v);
      int best_r = 0, best_c = 0;
      float best_d = 1e9f;

      for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
          const float sh = float(c) / float(kCols);
          const float ss = (r == 0) ? 0.0f : (0.35f + 0.21f * float(r));
          const float sv = (r == 0) ? (float(c) / float(kCols - 1)) : (1.0f - 0.16f * float(r - 1));
          const uint32_t col = cars::hsv_to_rgb(sh, ss, sv);
          const uint32_t rgba_col =
              ui::rgba(uint8_t((col >> 16) & 0xFF), uint8_t((col >> 8) & 0xFF), uint8_t(col & 0xFF), 255);
          const float x0 = left_x + float(c) * cw + 1.5f * s;
          const float y0 = gy + float(r) * ch + 1.5f * s;
          const float x1 = x0 + cw - 3.0f * s;
          const float y1 = y0 + ch - 3.0f * s;
          list.add_hit(x0, y0, x1, y1, ui4::kHitSwatch, r * kCols + c);
          list.rect(x0, y0, x1, y1, rgba_col);
          if (use_met && has_met) list.image(x0, y0, x1, y1, met_uv, ui::rgba(255, 255, 255, 92));
          else if (use_mat && has_mat) list.image(x0, y0, x1, y1, mat_uv, ui::rgba(255, 255, 255, 72));
          else if (has_chip) list.image(x0, y0, x1, y1, chip_uv, ui::rgba(255, 255, 255, 64));
          list.frame(x0, y0, x1, y1, ui::rgba(0, 0, 0, 120), 1.0f * s);

          const float dh = std::min(std::fabs(sh - cur_h), 1.0f - std::fabs(sh - cur_h));
          const float d = dh * 2.0f + std::fabs(ss - cur_s) + std::fabs(sv - cur_v);
          if (d < best_d) best_d = d, best_r = r, best_c = c;
        }
      }
      // #D41C46 CAR_PAINT_CURRENT_FOCUS on active swatch
      const uint32_t kFocusRed = ui::rgba(0xD4, 0x1C, 0x46, 255);
      const float fx = left_x + float(best_c) * cw;
      const float fy = gy + float(best_r) * ch;
      if (has_foc) {
        list.image(fx - 3.0f * s, fy - 3.0f * s, fx + cw + 3.0f * s, fy + ch + 3.0f * s, foc_uv, kFocusRed);
      }
      list.frame(fx - 2.0f * s, fy - 2.0f * s, fx + cw + 2.0f * s, fy + ch + 2.0f * s, kFocusRed, 2.5f * s);
      list.frame(fx, fy, fx + cw, fy + ch, ui4::kWhite, 1.5f * s);
      if (editor.region() == PaintRegion::kRims && editor.rims_paint_blocked()) {
        list.text(left_x, gy + gh + 10.0f * s,
                  editor.wheel_is_chrome() ? "CHROME RIMS CANNOT BE PAINTED"
                                           : "STOCK RIMS CANNOT BE PAINTED — FIT AFTERMARKET RIMS IN UPGRADES",
                  ui4::style(ui::Font::kE, 11.5f * s, ui4::kYellow));
      }
    } else if (editor.tab() == EditorTab::kManufacturerColors) {
      const auto& mc = editor.manufacturer_colors();
      ui4::title_tape(list, s, 86.0f * s, 456.0f * s,
                      loc_str("Livery", "IDS_ManufacturerColors", "MANUFACTURER COLORS") + " (" +
                          std::to_string(mc.size()) + ")",
                      ui4::kBrushBlack, 30.0f, 540.0f);
      const int n = int(mc.size());
      const int sel = int(editor.selected_manufacturer_color());
      const int count = std::min(std::max(1, n), 8);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float by = 514.0f * s, bh = 106.0f * s;
      const float arrow_pad = 30.0f * s;
      const float c_left = left_x + arrow_pad, c_right = right_x - arrow_pad;
      list.add_hit(left_x - 4.0f * s, by, c_left - 4.0f * s, by + bh, ui::kHitStepLeft,
                   int(EditorTab::kManufacturerColors));
      list.add_hit(c_right + 4.0f * s, by, right_x + 4.0f * s, by + bh, ui::kHitStepRight,
                   int(EditorTab::kManufacturerColors));
      ui4::opt_arrow(list, left_x + 11.0f * s, by + 0.5f * bh, 15.0f * s, -1, n > 1 ? ui4::kPinkHi : ui4::kDim);
      ui4::opt_arrow(list, right_x - 11.0f * s, by + 0.5f * bh, 15.0f * s, +1, n > 1 ? ui4::kPinkHi : ui4::kDim);
      const float gap = 10.0f * s;
      const float cw = ((c_right - c_left) - gap * float(std::max(0, count - 1))) / float(std::max(1, count));
      float bx = c_left;
      for (int i = first; i < first + count && i < n; ++i) {
        const auto& c = mc[size_t(i)];
        const bool act = (i == sel);
        const float lift = act ? 8.0f * s : 0.0f;
        list.add_hit(bx, by - lift, bx + cw, by + bh - lift, ui::kHitRow, i);
        ui4::panel01(list, bx, by - lift, bx + cw, by + bh - lift, s,
                     act ? ui::rgba(36, 16, 30, 240) : ui4::kCharcoalDark, act ? ui4::kPink : 0u);
        if (act) list.frame(bx, by - lift, bx + cw, by + bh - lift, ui4::kPink, 2.0f * s);
        list.rect(bx + 10.0f * s, by + 12.0f * s - lift, bx + cw - 10.0f * s, by + 62.0f * s - lift,
                  ui::rgba(uint8_t((c.rgb >> 16) & 0xFF), uint8_t((c.rgb >> 8) & 0xFF), uint8_t(c.rgb & 0xFF), 255));
        const std::string mfg_lbl = manufacturer_color_display_name(c);
        ui::TextStyle ms = ui4::style(act ? ui::Font::kE : ui::Font::kB, 11.5f * s, ui4::kWhite);
        ui4::fit_text(mfg_lbl, ms, cw - 20.0f * s, 8.0f * s);
        list.text(bx + 10.0f * s, by + 70.0f * s - lift, mfg_lbl, ms);
        list.text(bx + 10.0f * s, by + 88.0f * s - lift, c.metallic ? "METALLIC" : "FACTORY",
                  ui4::style(ui::Font::kB, 10.0f * s, act ? ui4::kYellow : ui4::kDim));
        bx += cw + gap;
      }
    } else if (editor.tab() == EditorTab::kPreviousColors) {
      const auto& pc = editor.previous_colors();
      ui4::title_tape(list, s, 86.0f * s, 456.0f * s,
                      loc_str("Livery", "IDS_PreviousColors", "PREVIOUS COLORS") + " (" + std::to_string(pc.size()) + ")",
                      ui4::kBrushBlack, 30.0f, 500.0f);
      const int n = int(pc.size());
      const int sel = int(editor.selected_previous_color());
      const int count = std::min(std::max(1, n), 8);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float by = 514.0f * s, bh = 106.0f * s;
      const float arrow_pad = 30.0f * s;
      const float c_left = left_x + arrow_pad, c_right = right_x - arrow_pad;
      list.add_hit(left_x - 4.0f * s, by, c_left - 4.0f * s, by + bh, ui::kHitStepLeft,
                   int(EditorTab::kPreviousColors));
      list.add_hit(c_right + 4.0f * s, by, right_x + 4.0f * s, by + bh, ui::kHitStepRight,
                   int(EditorTab::kPreviousColors));
      ui4::opt_arrow(list, left_x + 11.0f * s, by + 0.5f * bh, 15.0f * s, -1, n > 1 ? ui4::kPinkHi : ui4::kDim);
      ui4::opt_arrow(list, right_x - 11.0f * s, by + 0.5f * bh, 15.0f * s, +1, n > 1 ? ui4::kPinkHi : ui4::kDim);
      const float gap = 10.0f * s;
      const float cw = ((c_right - c_left) - gap * float(std::max(0, count - 1))) / float(std::max(1, count));
      float bx = c_left;
      for (int i = first; i < first + count && i < n; ++i) {
        const auto& c = pc[size_t(i)];
        const bool act = (i == sel);
        const float lift = act ? 8.0f * s : 0.0f;
        list.add_hit(bx, by - lift, bx + cw, by + bh - lift, ui::kHitRow, i);
        ui4::panel01(list, bx, by - lift, bx + cw, by + bh - lift, s,
                     act ? ui::rgba(36, 16, 30, 240) : ui4::kCharcoalDark, act ? ui4::kPink : 0u);
        if (act) list.frame(bx, by - lift, bx + cw, by + bh - lift, ui4::kPink, 2.0f * s);
        list.rect(bx + 10.0f * s, by + 12.0f * s - lift, bx + cw - 10.0f * s, by + 62.0f * s - lift,
                  ui::rgba(uint8_t((c.rgb >> 16) & 0xFF), uint8_t((c.rgb >> 8) & 0xFF), uint8_t(c.rgb & 0xFF), 255));
        const std::string p_lbl = paint_hue_name(c.rgb);
        ui::TextStyle ps = ui4::style(act ? ui::Font::kE : ui::Font::kB, 11.5f * s, ui4::kWhite);
        ui4::fit_text(p_lbl, ps, cw - 20.0f * s, 8.0f * s);
        list.text(bx + 10.0f * s, by + 70.0f * s - lift, p_lbl, ps);
        list.text(bx + 10.0f * s, by + 88.0f * s - lift, paint_finish_display_name(c.finish),
                  ui4::style(ui::Font::kB, 9.5f * s, act ? ui4::kYellow : ui4::kDim));
        bx += cw + gap;
      }
    } else if (editor.tab() == EditorTab::kSpecialColors) {
      // Bottom +3.0 deg Special Two-Tone Paint Carousel
      const auto& sp = editor.special_colors();
      ui4::title_tape(list, s, 86.0f * s, 456.0f * s,
                      loc_str("Livery", "IDS_SpecialColors", "SPECIAL COLORS") + " (" + std::to_string(sp.size()) + ")",
                      ui4::kBrushBlack, 30.0f, 560.0f);
      const int n = int(sp.size());
      const int sel = int(editor.selected_special());
      const int count = std::min(n, 6);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float by = 514.0f * s, bh = 106.0f * s;
      const float arrow_pad = 30.0f * s;
      const float c_left = left_x + arrow_pad, c_right = right_x - arrow_pad;
      list.add_hit(left_x - 4.0f * s, by, c_left - 4.0f * s, by + bh, ui::kHitStepLeft,
                   int(EditorTab::kSpecialColors));
      list.add_hit(c_right + 4.0f * s, by, right_x + 4.0f * s, by + bh, ui::kHitStepRight,
                   int(EditorTab::kSpecialColors));
      ui4::opt_arrow(list, left_x + 11.0f * s, by + 0.5f * bh, 15.0f * s, -1, n > 1 ? ui4::kPinkHi : ui4::kDim);
      ui4::opt_arrow(list, right_x - 11.0f * s, by + 0.5f * bh, 15.0f * s, +1, n > 1 ? ui4::kPinkHi : ui4::kDim);
      const float gap = 10.0f * s;
      const float cw = ((c_right - c_left) - gap * float(std::max(0, count - 1))) / float(std::max(1, count));
      float bx = c_left;
      for (int i = first; i < first + count; ++i) {
        const auto& sc = sp[size_t(i)];
        const bool act = (i == sel);
        const float lift = act ? 8.0f * s : 0.0f;
        list.add_hit(bx, by - lift, bx + cw, by + bh - lift, ui::kHitRow, i);
        ui4::panel01(list, bx, by - lift, bx + cw, by + bh - lift, s,
                     act ? ui::rgba(36, 16, 30, 240) : ui4::kCharcoalDark, act ? ui4::kPink : 0u);
        if (act) list.frame(bx, by - lift, bx + cw, by + bh - lift, ui4::kPink, 2.0f * s);
        const uint32_t c1 = sc.pri_rgb, c2 = sc.sec_valid ? sc.sec_rgb : sc.pri_rgb;
        const float mid = bx + 0.5f * cw;
        list.rect(bx + 10.0f * s, by + 12.0f * s - lift, mid - 2.0f * s, by + 62.0f * s - lift,
                  ui::rgba(uint8_t((c1 >> 16) & 0xFF), uint8_t((c1 >> 8) & 0xFF), uint8_t(c1 & 0xFF), 255));
        list.rect(mid + 2.0f * s, by + 12.0f * s - lift, bx + cw - 10.0f * s, by + 62.0f * s - lift,
                  ui::rgba(uint8_t((c2 >> 16) & 0xFF), uint8_t((c2 >> 8) & 0xFF), uint8_t(c2 & 0xFF), 255));
        const std::string sp_lbl = special_color_display_name(sc);
        ui::TextStyle ss = ui4::style(act ? ui::Font::kE : ui::Font::kB, 12.0f * s, ui4::kWhite);
        ui4::fit_text(sp_lbl, ss, cw - 20.0f * s, 8.0f * s);
        list.text(bx + 10.0f * s, by + 70.0f * s - lift, sp_lbl, ss);
        list.text(bx + 10.0f * s, by + 88.0f * s - lift, special_color_finish_subtitle(sc),
                  ui4::style(ui::Font::kB, 10.5f * s, act ? ui4::kYellow : ui4::kDim));
        bx += cw + gap;
      }
    } else {
      // 506_C_LIV_LAYER_SELECT: Bottom +3.0 deg Horizontal Layer Strip (y=498..618, 173px pitch)
      const auto& layers = editor.document().layers;
      ui4::title_tape(list, s, 86.0f * s, 434.0f * s,
                      std::string("VINYL LAYERS") + " (" + std::to_string(layers.size()) + ")",
                      ui4::kBrushBlack, 32.0f, 480.0f);

      const float by = 498.0f * s, bh = 114.0f * s;
      const int n = int(layers.size());
      const int sel = n > 0 ? int(std::min(editor.selected_layer(), layers.size() - 1)) : 0;
      const int slots = std::max(5, std::min(n + 1, 6));
      const int first = std::clamp(sel - slots / 2, 0, std::max(0, std::max(n, slots) - slots));
      const float arrow_pad = 30.0f * s;
      const float c_left = left_x + arrow_pad, c_right = right_x - arrow_pad;
      list.add_hit(left_x - 4.0f * s, by, c_left - 4.0f * s, by + bh, ui::kHitStepLeft,
                   int(EditorTab::kVinylLayers));
      list.add_hit(c_right + 4.0f * s, by, right_x + 4.0f * s, by + bh, ui::kHitStepRight,
                   int(EditorTab::kVinylLayers));
      ui4::opt_arrow(list, left_x + 11.0f * s, by + 0.5f * bh, 15.0f * s, -1, n > 1 ? ui4::kPinkHi : ui4::kDim);
      ui4::opt_arrow(list, right_x - 11.0f * s, by + 0.5f * bh, 15.0f * s, +1, n > 1 ? ui4::kPinkHi : ui4::kDim);
      const float gap = 10.0f * s;
      const float cw = ((c_right - c_left) - gap * float(slots - 1)) / float(slots);

      std::array<float, 4> bg_uv{}, empty_uv{}, foc_uv{};
      const bool has_bg = ui4::texture("LIVERY/LIV_BG_GROUP", bg_uv);
      const bool has_empty = ui4::texture("LIVERY/LIV_ICON_EMPTY", empty_uv);
      const bool has_foc = ui4::texture("LIVERY/LIV_FOCUS_FRAME", foc_uv);

      float bx = c_left;
      for (int s_i = 0; s_i < slots; ++s_i) {
        const int idx = first + s_i;
        const bool is_layer = (idx < n);
        const bool act = is_layer ? (idx == sel) : (n == 0 && s_i == 0);
        const float lift = act ? 8.0f * s : 0.0f;
        list.add_hit(bx, by - lift, bx + cw, by + bh - lift, ui::kHitRow, idx);

        if (has_bg) {
          list.image(bx, by - lift, bx + cw, by + bh - lift, bg_uv,
                     act ? ui::rgba(52, 20, 42, 240) : ui::rgba(28, 30, 36, 224));
        } else {
          ui4::panel01(list, bx, by - lift, bx + cw, by + bh - lift, s,
                       act ? ui::rgba(40, 18, 34, 240) : ui4::kCharcoalDark, act ? ui4::kPink : 0u);
        }
        if (act) {
          if (has_foc) list.image(bx - 3.0f * s, by - 3.0f * s - lift, bx + cw + 3.0f * s, by + bh + 3.0f * s - lift,
                                  foc_uv, ui4::kPink);
          list.frame(bx, by - lift, bx + cw, by + bh - lift, ui4::kPink, 2.2f * s);
        }

        if (is_layer) {
          const auto& ly = layers[size_t(idx)];
          char idx_b[32];
          if (ly.group_id > 0) std::snprintf(idx_b, sizeof idx_b, "LAYER %02d [G%d]", idx + 1, ly.group_id);
          else std::snprintf(idx_b, sizeof idx_b, "LAYER %02d", idx + 1);
          list.text(bx + 10.0f * s, by + 8.0f * s - lift, idx_b, ui4::style(ui::Font::kE, 10.5f * s, ui4::kYellow));

          // Miniature vinyl shape preview thumbnail box in top-right of card.
          const float tsz = 36.0f * s;
          const float tx0 = bx + cw - tsz - 8.0f * s, ty0 = by + 8.0f * s - lift;
          list.rect(tx0, ty0, tx0 + tsz, ty0 + tsz, ui::rgba(14, 16, 22, 235));
          std::array<float, 4> thumb_uv{};
          bool is_multi = false;
          const cars::VinylMesh* mesh = editor.preview_shape_mesh(ly.shape_path);
          if (ensure_vinyl_thumbnail_uv(ly.shape_path, mesh, ly.flip_x, ly.flip_y, thumb_uv, is_multi)) {
            const uint32_t tint =
                is_multi ? ui4::kWhite
                         : ui::rgba(uint8_t((ly.rgb >> 16) & 0xFF), uint8_t((ly.rgb >> 8) & 0xFF),
                                    uint8_t(ly.rgb & 0xFF), 255);
            list.image(tx0 + 2.0f * s, ty0 + 2.0f * s, tx0 + tsz - 2.0f * s, ty0 + tsz - 2.0f * s, thumb_uv, tint);
          } else {
            list.rect(tx0 + 4.0f * s, ty0 + 4.0f * s, tx0 + tsz - 4.0f * s, ty0 + tsz - 4.0f * s,
                      ui::rgba(uint8_t((ly.rgb >> 16) & 0xFF), uint8_t((ly.rgb >> 8) & 0xFF),
                               uint8_t(ly.rgb & 0xFF), 255));
          }
          list.frame(tx0, ty0, tx0 + tsz, ty0 + tsz, act ? ui4::kPinkHi : ui4::kWhite, 1.0f * s);

          std::string sname = format_vinyl_shape_label(ly.shape_path);
          ui::TextStyle st = ui4::style(act ? ui::Font::kE : ui::Font::kB, 12.0f * s, ui4::kWhite, ui::Align::kLeft, true);
          ui4::fit_text(sname, st, cw - tsz - 22.0f * s, 8.0f * s);
          list.text(bx + 10.0f * s, by + 30.0f * s - lift, sname, st);

          char sub[64];
          std::snprintf(sub, sizeof sub, "%s  •  %.0f°", cars::livery_plane_name(ly.plane), ly.rot_deg);
          list.text(bx + 10.0f * s, by + 68.0f * s - lift, sub,
                    ui4::style(ui::Font::kB, 10.0f * s, act ? ui4::kOffWhite : ui4::kDim));
          char scb[48];
          std::snprintf(scb, sizeof scb, "SCALE %.2f x %.2f", ly.scale_x, ly.scale_y);
          list.text(bx + 10.0f * s, by + 88.0f * s - lift, scb,
                    ui4::style(ui::Font::kB, 9.5f * s, ui4::kDim));
        } else {
          if (has_empty) {
            const float esz = 38.0f * s;
            list.image(bx + 0.5f * (cw - esz), by + 22.0f * s - lift, bx + 0.5f * (cw + esz),
                       by + 22.0f * s + esz - lift, empty_uv, ui::rgba(255, 255, 255, 110));
          }
          list.text(bx + 0.5f * cw, by + 74.0f * s - lift, std::string("EMPTY SLOT"),
                    ui4::style(ui::Font::kB, 11.0f * s, ui4::kDim, ui::Align::kCenter));
          const std::string add_hint =
              ui4::glyph_label_for("Y") + " " + std::string("ADD VINYL");
          list.text(bx + 0.5f * cw, by + 90.0f * s - lift, add_hint,
                    ui4::style(ui::Font::kE, 10.0f * s, ui4::kPinkHi, ui::Align::kCenter));
        }
        bx += cw + gap;
      }
    }
  }

  // Design Catalog modal overlay (liveries/*.liv save/load list).
  if (editor.catalog_open()) {
    const float cx0 = 86.0f * s, cy0 = 112.0f * s, cx1 = 500.0f * s, cy1 = 442.0f * s;
    ui4::panel01(list, cx0, cy0, cx1, cy1, s, ui::rgba(16, 18, 24, 240), ui4::kPink);
    list.text(cx0 + 16.0f * s, cy0 + 12.0f * s,
              std::string("DESIGN CATALOG") + " (" +
                  std::to_string(editor.design_catalog().size()) + ")",
              ui4::style(ui::Font::kE, 14.0f * s, ui4::kPinkHi));
    const auto& cat = editor.design_catalog();
    if (cat.empty()) {
      list.text(cx0 + 16.0f * s, cy0 + 48.0f * s, "NO SAVED DESIGNS IN liveries/",
                ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim));
    } else {
      const int n = int(cat.size());
      const int sel = int(std::min(editor.selected_design(), cat.size() - 1));
      const int max_vis = std::min(n, 7);
      const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, n - max_vis));
      float ry = cy0 + 42.0f * s;
      for (int i = first; i < first + max_vis; ++i) {
        const bool act = (i == sel);
        list.add_hit(cx0 + 10.0f * s, ry, cx1 - 10.0f * s, ry + 34.0f * s, ui::kHitRow, i);
        ui4::pill(list, cx0 + 10.0f * s, ry, cx1 - 10.0f * s, ry + 34.0f * s,
                  act ? ui4::kPink : ui4::kPillGrey, ui4::PillKind::kPill02, false, act);
        list.text(cx0 + 20.0f * s, ry + 9.0f * s, cat[size_t(i)].name,
                  ui4::style(act ? ui::Font::kE : ui::Font::kB, 12.0f * s, ui4::kWhite));
        list.text(cx1 - 20.0f * s, ry + 10.0f * s,
                  std::to_string(cat[size_t(i)].layer_count) + " LAYERS",
                  ui4::style(ui::Font::kB, 10.5f * s, act ? ui4::kYellow : ui4::kDim, ui::Align::kRight));
        ry += 38.0f * s;
      }
    }
    const std::string cat_hint = ui4::glyph_label_for("A") + " LOAD   " + ui4::glyph_label_for("X") +
                                 " SAVE COPY   " + ui4::glyph_label_for("B") + " CLOSE";
    list.text(cx0 + 16.0f * s, cy1 - 24.0f * s, cat_hint, ui4::style(ui::Font::kB, 10.5f * s, ui4::kYellow));
  }

  if (!notice.empty()) {
    ui4::brush(list, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    ui4::fit_text(notice, ns, 340.0f * s, 9.0f * s);
    list.text(w - 68.0f * s, h - 60.0f * s, notice, ns);
  }

  const std::string undo_lbl =
      editor.can_undo() ? (std::string("UNDO") + " (" + std::to_string(editor.undo_depth()) + ")")
                        : std::string("TAB");
  // Concise prompts ensure all 6 buttons (indices 0..5, including index 2 DPAD_LR) fit cleanly at 1280px.
  ui4::button_bar(list,
                  {{"A", loc_str("HelpButtons", "IDS_Save", "SAVE")},
                   {"LB", vinyl_tab ? undo_lbl : std::string("TAB")},
                   {"DPAD_LR", vinyl_tab ? std::string("LAYER")
                                         : std::string("FINISH")},
                   {"Y", vinyl_tab ? loc_str("HelpButtons", "IDS_Add", "ADD")
                                   : std::string("REGION")},
                   {"X", vinyl_tab ? loc_str("HelpButtons", "IDS_Delete", "DELETE")
                                   : std::string("FACTORY")},
                   {"B", loc_str("HelpButtons", "IDS_Back", "BACK")}},
                  w, h, 56.0f);
}

}  // namespace carview
