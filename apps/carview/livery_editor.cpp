#include "livery_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "hud_scene.h"
#include "ui4_chrome.h"

namespace carview {

const char* paint_region_name(PaintRegion r) {
  switch (r) {
    case PaintRegion::kBody: return "BODY";
    case PaintRegion::kHood: return "HOOD";
    case PaintRegion::kWing: return "REAR WING";
    case PaintRegion::kMirrors: return "MIRRORS";
    case PaintRegion::kRims: return "RIMS";
    case PaintRegion::kCalipers: return "BRAKE CALIPERS";
    case PaintRegion::kWindowTint: return "WINDOW TINT";
    default: return "BODY";
  }
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
  if (db && db->is_open()) {
    cars::query_special_colors(*db, specials_, nullptr);
  }
  if (!game_root.empty()) {
    const auto lzip = game_root / "media" / "Livery.zip";
    if (std::filesystem::exists(lzip)) lib_.open(lzip, nullptr);
  }
  cars::rgb_to_hsv(doc_.paint.body.rgb, cur_h_, cur_s_, cur_v_);
  tab_ = EditorTab::kPaint;
  region_ = PaintRegion::kBody;
  special_idx_ = 0;
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
    case PaintRegion::kCalipers: return doc_.paint.calipers;
    default: return doc_.paint.body;
  }
}

const cars::PaintRegionSpec& LiveryEditor::active_region_spec() const {
  switch (region_) {
    case PaintRegion::kHood: return doc_.paint.hood;
    case PaintRegion::kWing: return doc_.paint.wing;
    case PaintRegion::kMirrors: return doc_.paint.mirrors;
    case PaintRegion::kRims: return doc_.paint.rims;
    case PaintRegion::kCalipers: return doc_.paint.calipers;
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

void LiveryEditor::move_finish(int by) {
  if (region_ == PaintRegion::kWindowTint) return;
  if (region_ == PaintRegion::kRims && wheel_is_chrome_) return;
  auto& spec = active_region_spec();
  const int n = int(cars::PaintFinish::kCount);
  spec.finish = cars::PaintFinish(((int(spec.finish) + by) % n + n) % n);
  spec.active = true;
  dirty_ = true;
}

void LiveryEditor::step_hsv(float dh, float ds, float dv) {
  if (region_ == PaintRegion::kWindowTint) {
    step_window_tint(dh + dv);
    return;
  }
  if (region_ == PaintRegion::kRims && wheel_is_chrome_) return;
  cur_h_ = std::fmod(cur_h_ + dh + 1.0f, 1.0f);
  cur_s_ = std::clamp(cur_s_ + ds, 0.0f, 1.0f);
  cur_v_ = std::clamp(cur_v_ + dv, 0.0f, 1.0f);
  auto& spec = active_region_spec();
  spec.rgb = cars::hsv_to_rgb(cur_h_, cur_s_, cur_v_);
  spec.active = true;
  dirty_ = true;
}

void LiveryEditor::step_window_tint(float delta) {
  doc_.paint.window_tint = std::clamp(doc_.paint.window_tint + delta, 0.0f, 1.0f);
  dirty_ = true;
}

void LiveryEditor::set_swatch(int row, int col, int rows, int cols) {
  if (rows <= 0 || cols <= 0) return;
  row = std::clamp(row, 0, rows - 1);
  col = std::clamp(col, 0, cols - 1);
  if (region_ == PaintRegion::kWindowTint) {
    const float t = (cols > 1) ? float(col) / float(cols - 1) : 0.5f;
    doc_.paint.window_tint = std::clamp(1.0f - t, 0.0f, 1.0f);
    dirty_ = true;
    return;
  }
  if (region_ == PaintRegion::kRims && wheel_is_chrome_) return;
  cur_h_ = float(col) / float(cols);
  cur_s_ = (row == 0) ? 0.0f : (0.35f + 0.21f * float(row));
  cur_v_ = (row == 0) ? ((cols > 1) ? float(col) / float(cols - 1) : 1.0f) : (1.0f - 0.16f * float(row - 1));
  auto& spec = active_region_spec();
  spec.rgb = cars::hsv_to_rgb(cur_h_, cur_s_, cur_v_);
  spec.active = true;
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
  if (region_ == PaintRegion::kRims && wheel_is_chrome_) return;
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
  dirty_ = true;
}

void LiveryEditor::add_vinyl_layer(const std::string& shape_path, uint32_t rgb) {
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
  std::swap(doc_.layers[layer_idx_], doc_.layers[size_t(dst)]);
  layer_idx_ = size_t(dst);
  dirty_ = true;
}

void LiveryEditor::transform_layer(float dx, float dy, float dsx, float dsy, float drot) {
  if (layer_idx_ >= doc_.layers.size()) return;
  auto& l = doc_.layers[layer_idx_];
  l.x = std::clamp(l.x + dx, 0.0f, 1.0f);
  l.y = std::clamp(l.y + dy, 0.0f, 1.0f);
  l.scale_x = std::clamp(l.scale_x + dsx, 0.02f, 2.0f);
  l.scale_y = std::clamp(l.scale_y + dsy, 0.02f, 2.0f);
  l.rot_deg = std::fmod(l.rot_deg + drot + 360.0f, 360.0f);
  dirty_ = true;
}

void LiveryEditor::cycle_layer_shape(int by) {
  if (layer_idx_ >= doc_.layers.size()) return;
  const size_t total = lib_.total_shapes();
  if (total == 0) {
    static const char* kBuiltins[] = {"@rect", "@circle", "@star", "@chevron"};
    shape_cursor_ = (shape_cursor_ + size_t(by + 4)) % 4;
    doc_.layers[layer_idx_].shape_path = kBuiltins[shape_cursor_];
  } else {
    shape_cursor_ = size_t(((int(shape_cursor_) + by) % int(total) + int(total)) % int(total));
    if (shape_cursor_ < lib_.vinyl_names().size()) {
      doc_.layers[layer_idx_].shape_path = lib_.vinyl_names()[shape_cursor_];
    } else {
      doc_.layers[layer_idx_].shape_path = lib_.decal_names()[shape_cursor_ - lib_.vinyl_names().size()];
    }
  }
  dirty_ = true;
}

void LiveryEditor::cycle_layer_plane(int by) {
  if (layer_idx_ >= doc_.layers.size()) return;
  const int n = int(cars::LiveryPlaneKind::kCount);
  auto& l = doc_.layers[layer_idx_];
  l.plane = cars::LiveryPlaneKind(((int(l.plane) + by) % n + n) % n);
  dirty_ = true;
}

void LiveryEditor::recolor_layer_hsv(float dh, float ds, float dv) {
  if (layer_idx_ >= doc_.layers.size()) return;
  float h = 0, s = 0, v = 0;
  cars::rgb_to_hsv(doc_.layers[layer_idx_].rgb, h, s, v);
  h = std::fmod(h + dh + 1.0f, 1.0f);
  s = std::clamp(s + ds, 0.0f, 1.0f);
  v = std::clamp(v + dv, 0.0f, 1.0f);
  doc_.layers[layer_idx_].rgb = cars::hsv_to_rgb(h, s, v);
  dirty_ = true;
}

bool LiveryEditor::save(const std::filesystem::path& path, std::string* error) const {
  return cars::save_livery_file(path, doc_, error);
}

bool LiveryEditor::load(const std::filesystem::path& path, std::string* error) {
  if (!cars::load_livery_file(path, doc_, error)) return false;
  dirty_ = true;
  return true;
}

bool LiveryEditor::composite_1024(const std::vector<cars::bake::LiveryPlane>* planes, std::vector<uint8_t>& out_rgba,
                                  uint32_t size) {
  return cars::composite_livery(doc_, lib_.is_open() ? &lib_ : nullptr, planes, out_rgba, size);
}

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
  static const std::vector<std::string> kTabs = {"PAINT & FINISHES", "SPECIAL (102)", "VINYL LAYERS"};
  {
    ui4::TiltScope tab_rot(list, w - 310.0f * s, 40.0f * s, tilt_rad * 0.6f);
    ui4::tab_strip(list, s, w - 580.0f * s, 24.0f * s, w - 48.0f * s, kTabs, int(editor.tab()), true, ui4::kPink);
  }

  // Top-right 506_C_LIV_LAYER_SELECT LIV_CLIPBOARD ([1040,55..1184,177]) when in Vinyl Editor!
  if (vinyl_tab) {
    const float cb_x0 = w - 240.0f * s, cb_y0 = 66.0f * s, cb_x1 = w - 56.0f * s, cb_y1 = 176.0f * s;
    std::array<float, 4> clip_uv{};
    if (ui4::texture("LIVERY/LIV_CLIPBOARD", clip_uv)) {
      list.image(cb_x0, cb_y0, cb_x1, cb_y1, clip_uv, ui::rgba(255, 255, 255, 235));
    } else {
      ui4::panel01(list, cb_x0, cb_y0, cb_x1, cb_y1, s, ui4::kCharcoalDark, ui4::kPink);
    }
    list.text(cb_x0 + 14.0f * s, cb_y0 + 12.0f * s, "LIV CLIPBOARD",
              ui4::style(ui::Font::kE, 12.0f * s, ui4::kPink));
    list.text(cb_x0 + 14.0f * s, cb_y0 + 34.0f * s,
              std::to_string(editor.document().layers.size()) + " LAYERS",
              ui4::style(ui::Font::kE, 17.0f * s, ui4::kWhite));
    list.text(cb_x0 + 14.0f * s, cb_y0 + 58.0f * s,
              std::to_string(editor.library_shape_count()) + " SHAPES READY",
              ui4::style(ui::Font::kB, 11.0f * s, ui4::kDim));
    if (!editor.document().layers.empty()) {
      const auto& al = editor.document().layers[std::min(editor.selected_layer(), editor.document().layers.size() - 1)];
      char tb[64];
      std::snprintf(tb, sizeof tb, "%s  •  ROT %.0f°", cars::livery_plane_name(al.plane), al.rot_deg);
      list.text(cb_x0 + 14.0f * s, cb_y0 + 82.0f * s, tb, ui4::style(ui::Font::kB, 10.5f * s, ui4::kYellow));
    }
  } else if (editor.tab() == EditorTab::kPaint) {
    // Slim top-left region pill strip with authentic T:C_Map/C_LIV_Map/C_LIV_PaintCar_* icons (never blocks car!).
    static const char* kRegionIcons[7] = {
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Body",    "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Hood",
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Wing",    "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Mirror",
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Rim",     "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Caliper",
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Body",
    };
    ui4::TiltScope reg_rot(list, 310.0f * s, 88.0f * s, tilt_rad * 0.6f);
    float rx = 56.0f * s;
    const float ry = 74.0f * s;
    // Measure each region tab width dynamically so long labels (REAR WING, BRAKE CALIPERS, WINDOW TINT) never clip.
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

      ui4::title_tape(list, s, 86.0f * s, 456.0f * s,
                      std::string("PAINT  //  ") + paint_region_name(editor.region()), ui4::kBrushBlack, 30.0f, 440.0f);

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
      const std::string fname = is_tint ? std::string(tint_buf) : std::string(cars::paint_finish_name(active_spec.finish));
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
    } else if (editor.tab() == EditorTab::kSpecialColors) {
      // Bottom +3.0 deg Special Two-Tone Paint Carousel
      const auto& sp = editor.special_colors();
      ui4::title_tape(list, s, 86.0f * s, 456.0f * s,
                      "SPECIAL TWO-TONE PAINTS (" + std::to_string(sp.size()) + ")", ui4::kBrushBlack, 30.0f, 560.0f);
      const int n = int(sp.size());
      const int sel = int(editor.selected_special());
      const int count = std::min(n, 6);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float gap = 10.0f * s;
      const float cw = (blade_w - gap * float(std::max(0, count - 1))) / float(std::max(1, count));
      const float by = 514.0f * s, bh = 106.0f * s;
      float bx = left_x;
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
        char buf[48];
        std::snprintf(buf, sizeof buf, "SPECIAL #%lld", (long long)sc.id);
        list.text(bx + 10.0f * s, by + 70.0f * s - lift, buf,
                  ui4::style(act ? ui::Font::kE : ui::Font::kB, 12.5f * s, ui4::kWhite));
        char sub[48];
        std::snprintf(sub, sizeof sub, "TWO-TONE %.1f", sc.two_tone_power);
        list.text(bx + 10.0f * s, by + 88.0f * s - lift, sub,
                  ui4::style(ui::Font::kB, 10.5f * s, act ? ui4::kYellow : ui4::kDim));
        bx += cw + gap;
      }
    } else {
      // 506_C_LIV_LAYER_SELECT: Bottom +3.0 deg Horizontal Layer Strip (y=498..618, 173px pitch)
      const auto& layers = editor.document().layers;
      ui4::title_tape(list, s, 86.0f * s, 434.0f * s,
                      "VINYL LAYERS (" + std::to_string(layers.size()) + ")", ui4::kBrushBlack, 32.0f, 480.0f);

      const float by = 498.0f * s, bh = 114.0f * s;
      const int n = int(layers.size());
      const int sel = n > 0 ? int(std::min(editor.selected_layer(), layers.size() - 1)) : 0;
      const int slots = std::max(5, std::min(n + 1, 6));
      const int first = std::clamp(sel - slots / 2, 0, std::max(0, std::max(n, slots) - slots));
      const float gap = 10.0f * s;
      const float cw = (blade_w - gap * float(slots - 1)) / float(slots);

      std::array<float, 4> bg_uv{}, empty_uv{}, foc_uv{};
      const bool has_bg = ui4::texture("LIVERY/LIV_BG_GROUP", bg_uv);
      const bool has_empty = ui4::texture("LIVERY/LIV_ICON_EMPTY", empty_uv);
      const bool has_foc = ui4::texture("LIVERY/LIV_FOCUS_FRAME", foc_uv);

      float bx = left_x;
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
          char idx_b[16];
          std::snprintf(idx_b, sizeof idx_b, "LAYER %02d", idx + 1);
          list.text(bx + 10.0f * s, by + 8.0f * s - lift, idx_b, ui4::style(ui::Font::kE, 11.0f * s, ui4::kYellow));
          list.rect(bx + cw - 36.0f * s, by + 8.0f * s - lift, bx + cw - 10.0f * s, by + 34.0f * s - lift,
                    ui::rgba(uint8_t((ly.rgb >> 16) & 0xFF), uint8_t((ly.rgb >> 8) & 0xFF), uint8_t(ly.rgb & 0xFF), 255));
          list.frame(bx + cw - 36.0f * s, by + 8.0f * s - lift, bx + cw - 10.0f * s, by + 34.0f * s - lift,
                     ui4::kWhite, 1.0f * s);

          std::string sname = ly.shape_path.empty() ? "VINYL SHAPE" : ly.shape_path;
          ui::TextStyle st = ui4::style(act ? ui::Font::kE : ui::Font::kB, 12.5f * s, ui4::kWhite, ui::Align::kLeft, true);
          ui4::fit_text(sname, st, cw - 20.0f * s, 8.5f * s);
          list.text(bx + 10.0f * s, by + 46.0f * s - lift, sname, st);

          char sub[64];
          std::snprintf(sub, sizeof sub, "%s  •  %.0f°", cars::livery_plane_name(ly.plane), ly.rot_deg);
          list.text(bx + 10.0f * s, by + 70.0f * s - lift, sub,
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
          list.text(bx + 0.5f * cw, by + 74.0f * s - lift, "EMPTY SLOT",
                    ui4::style(ui::Font::kB, 11.0f * s, ui4::kDim, ui::Align::kCenter));
          list.text(bx + 0.5f * cw, by + 90.0f * s - lift, "[Y] ADD VINYL",
                    ui4::style(ui::Font::kE, 10.0f * s, ui4::kPinkHi, ui::Align::kCenter));
        }
        bx += cw + gap;
      }
    }
  }

  if (!notice.empty()) {
    ui4::brush(list, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    ui4::fit_text(notice, ns, 340.0f * s, 9.0f * s);
    list.text(w - 68.0f * s, h - 60.0f * s, notice, ns);
  }

  ui4::button_bar(list,
                  {{"A", "SAVE LIVERY"},
                   {"LB", "TAB"},
                   {"DPAD_LR", vinyl_tab ? "TRANSFORM" : "FINISH / COLOR"},
                   {"Y", "ADD VINYL"},
                   {"X", "DELETE"},
                   {"B", "BACK"}},
                  w, h, 56.0f);
}

}  // namespace carview
