#include "livery_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

void LiveryEditor::move_region(int by) {
  const int n = int(PaintRegion::kCount);
  region_ = PaintRegion(((int(region_) + by) % n + n) % n);
  if (region_ != PaintRegion::kWindowTint) {
    cars::rgb_to_hsv(active_region_spec().rgb, cur_h_, cur_s_, cur_v_);
  }
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
  ui4::subtle_vignette(list, w, h, 0.28f);

  // Top-left iconic tilted FH1 Paint Shop header.
  ui4::title_banner(list, s, 56.0f * s, 24.0f * s,
                    editor.tab() == EditorTab::kVinylLayers ? "VINYL EDITOR" : "PAINT SHOP",
                    car_title.empty() ? "HORIZON PAINT BOOTH" : car_title, true, 460.0f);

  // Top-right 3-tab strip with LB / RB controller bumpers.
  static const std::vector<std::string> kTabs = {"PAINT & FINISHES", "SPECIAL (102)", "VINYL LAYERS"};
  ui4::tab_strip(list, s, w - 620.0f * s, 38.0f * s, w - 56.0f * s, kTabs, int(editor.tab()), true, ui4::kPink);

  // Left authentic FH1 sharp distressed control panel (unobstructed 3D car on right!).
  const float lx = 56.0f * s, ly = 114.0f * s, lw = 432.0f * s, lh = h - 196.0f * s;
  ui4::panel(list, lx, ly, lx + lw, ly + lh, s, ui4::kCharcoalDark, ui4::kPink, 0, true);

  if (editor.tab() == EditorTab::kPaint) {
    // Region selector pills with authentic Textures.zip C_LIV_PaintCar_* icons!
    ui4::offwhite_strip(list, lx + 12.0f * s, ly + 10.0f * s, lx + lw - 12.0f * s, ly + 34.0f * s, s, false);
    list.text(lx + 22.0f * s, ly + 15.0f * s, "PAINT REGION", ui4::style(ui::Font::kE, 13.5f * s, ui4::kInk));

    static const char* kRegionIcons[7] = {
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Body",    "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Hood",
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Wing",    "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Mirror",
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Rim",     "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Caliper",
        "T:C_Map/C_LIV_Map/C_LIV_PaintCar_Body",
    };

    // 2-column compact region selector grid so HSV swatch grid has spacious room below.
    const float col_w = (lw - 32.0f * s) * 0.5f;
    for (int i = 0; i < int(PaintRegion::kCount); ++i) {
      const bool act = (PaintRegion(i) == editor.region());
      const int col = i % 2;
      const int row = i / 2;
      const float rx0 = lx + 12.0f * s + float(col) * (col_w + 8.0f * s);
      const float ry0 = ly + 42.0f * s + float(row) * 38.0f * s;
      if (act) {
        ui4::brush(list, rx0 - 2.0f * s, ry0, rx0 + col_w + 2.0f * s, ry0 + 34.0f * s, ui4::kPink);
      } else {
        ui4::pill(list, rx0, ry0 + 1.0f * s, rx0 + col_w, ry0 + 33.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu);
      }
      std::array<float, 4> ruv{};
      float tx = rx0 + 14.0f * s;
      if (ui4::texture(kRegionIcons[i], ruv)) {
        list.image(rx0 + 8.0f * s, ry0 + 5.0f * s, rx0 + 32.0f * s, ry0 + 29.0f * s, ruv,
                   act ? ui4::kWhite : ui4::kDim);
        tx = rx0 + 38.0f * s;
      }
      ui::TextStyle rs = ui4::style(act ? ui::Font::kE : ui::Font::kB, 12.0f * s, act ? ui4::kWhite : ui4::kDim);
      ui4::fit_text(paint_region_name(PaintRegion(i)), rs, rx0 + col_w - tx - 8.0f * s, 8.5f * s);
      list.text(tx, ry0 + 10.0f * s, paint_region_name(PaintRegion(i)), rs);
    }

    // Active finish selector bar + live current swatch preview!
    const float fin_y = ly + 206.0f * s;
    const auto& body = editor.document().paint.body;
    const uint32_t cur_rgb = body.rgb;
    const uint32_t cur_col =
        ui::rgba(uint8_t((cur_rgb >> 16) & 0xFF), uint8_t((cur_rgb >> 8) & 0xFF), uint8_t(cur_rgb & 0xFF), 255);
    list.rect(lx + 12.0f * s, fin_y, lx + lw - 12.0f * s, fin_y + 44.0f * s, ui::rgba(32, 34, 42, 224));
    list.rect(lx + 12.0f * s, fin_y, lx + 16.0f * s, fin_y + 44.0f * s, ui4::kPink);
    list.text(lx + 26.0f * s, fin_y + 7.0f * s, "FINISH TYPE", ui4::style(ui::Font::kB, 10.0f * s, ui4::kDim));
    list.text(lx + 26.0f * s, fin_y + 21.0f * s,
              std::string("<  ") + cars::paint_finish_name(body.finish) + "  >",
              ui4::style(ui::Font::kE, 15.5f * s, ui4::kWhite));
    // Live colour chip frame on right of finish bar.
    const float cf_x0 = lx + lw - 74.0f * s, cf_y0 = fin_y + 6.0f * s;
    list.rect(cf_x0, cf_y0, cf_x0 + 54.0f * s, cf_y0 + 32.0f * s, cur_col);
    std::array<float, 4> cfr_uv{};
    if (ui4::texture("LIVERY/CAR_PAINT_CURRENT_FRAME", cfr_uv)) {
      list.image(cf_x0 - 2.0f * s, cf_y0 - 2.0f * s, cf_x0 + 56.0f * s, cf_y0 + 34.0f * s, cfr_uv);
    } else {
      list.frame(cf_x0, cf_y0, cf_x0 + 54.0f * s, cf_y0 + 32.0f * s, ui4::kWhite, 1.5f * s);
    }

    // Authentic FH1 12x7 HSV Colour Swatch Grid with Livery/CAR_PAINT_GRID_* finish shading!
    const float gy_hdr = fin_y + 56.0f * s;
    list.text(lx + 14.0f * s, gy_hdr, "COLOR SWATCHES (HSV / FINISH)",
              ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    const float gx = lx + 14.0f * s, gy = gy_hdr + 20.0f * s;
    constexpr int kCols = 12, kRows = 7;
    const float cw = (lw - 28.0f * s) / float(kCols);
    const float ch = std::min(28.0f * s, (ly + lh - gy - 14.0f * s) / float(kRows));
    std::array<float, 4> chip_uv{}, met_uv{}, mat_uv{}, foc_uv{};
    const bool has_chip = ui4::texture("LIVERY/CAR_PAINT_GRID_CHIP", chip_uv);
    const bool has_met = ui4::texture("LIVERY/CAR_PAINT_GRID_METALLIC", met_uv);
    const bool has_mat = ui4::texture("LIVERY/CAR_PAINT_GRID_MATTE", mat_uv);
    const bool has_foc = ui4::texture("LIVERY/CAR_PAINT_GRID_FOCUS", foc_uv);
    const bool use_met = (body.finish == cars::PaintFinish::kMetallic || body.finish == cars::PaintFinish::kTwoTone);
    const bool use_mat = (body.finish == cars::PaintFinish::kMatte);

    float cur_h = 0, cur_s = 0, cur_v = 0;
    cars::rgb_to_hsv(cur_rgb, cur_h, cur_s, cur_v);
    int best_r = 0, best_c = 0;
    float best_d = 1e9f;

    for (int r = 0; r < kRows; ++r) {
      for (int c = 0; c < kCols; ++c) {
        const float sh = float(c) / float(kCols);
        const float ss = (r == 0) ? 0.0f : (0.28f + 0.12f * float(r));
        const float sv = (r == 0) ? (float(c) / float(kCols - 1)) : (1.0f - 0.105f * float(r - 1));
        const uint32_t col = cars::hsv_to_rgb(sh, ss, sv);
        const uint32_t rgba_col =
            ui::rgba(uint8_t((col >> 16) & 0xFF), uint8_t((col >> 8) & 0xFF), uint8_t(col & 0xFF), 255);
        const float x0 = gx + float(c) * cw;
        const float y0 = gy + float(r) * ch;
        const float x1 = x0 + cw - 2.0f * s;
        const float y1 = y0 + ch - 2.0f * s;
        // Solid full-chroma swatch base, then subtle finish sheen overlay so all 84 HSV colours read vividly.
        list.rect(x0, y0, x1, y1, rgba_col);
        if (use_met && has_met) {
          list.image(x0, y0, x1, y1, met_uv, ui::rgba(255, 255, 255, 92));
        } else if (use_mat && has_mat) {
          list.image(x0, y0, x1, y1, mat_uv, ui::rgba(255, 255, 255, 72));
        } else if (has_chip) {
          list.image(x0, y0, x1, y1, chip_uv, ui::rgba(255, 255, 255, 64));
        }
        list.frame(x0, y0, x1, y1, ui::rgba(0, 0, 0, 120), 1.0f * s);

        const float dh = std::min(std::fabs(sh - cur_h), 1.0f - std::fabs(sh - cur_h));
        const float d = dh * 2.0f + std::fabs(ss - cur_s) + std::fabs(sv - cur_v);
        if (d < best_d) best_d = d, best_r = r, best_c = c;
      }
    }
    const float fx0 = gx + float(best_c) * cw;
    const float fy0 = gy + float(best_r) * ch;
    list.frame(fx0 - 2.0f * s, fy0 - 2.0f * s, fx0 + cw, fy0 + ch, ui4::kWhite, 2.5f * s);
    list.frame(fx0 - 4.0f * s, fy0 - 4.0f * s, fx0 + cw + 2.0f * s, fy0 + ch + 2.0f * s, ui4::kPink, 1.5f * s);
  } else if (editor.tab() == EditorTab::kSpecialColors) {
    ui4::offwhite_strip(list, lx + 12.0f * s, ly + 10.0f * s, lx + lw - 12.0f * s, ly + 34.0f * s, s, false);
    list.text(lx + 22.0f * s, ly + 15.0f * s,
              "SPECIAL TWO-TONE PAINTS (" + std::to_string(editor.special_colors().size()) + ")",
              ui4::style(ui::Font::kE, 13.5f * s, ui4::kInk));
    const auto& sp = editor.special_colors();
    const int n = int(sp.size());
    const int sel = int(editor.selected_special());
    const int max_vis = std::max(1, int((lh - 48.0f * s) / (44.0f * s)));
    const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, n - max_vis));
    float ry = ly + 44.0f * s;
    for (int i = first; i < std::min(n, first + max_vis); ++i) {
      const auto& sc = sp[size_t(i)];
      const bool act = (i == sel);
      if (act) ui4::brush(list, lx + 6.0f * s, ry, lx + lw - 6.0f * s, ry + 40.0f * s, ui4::kPink);
      else list.rect(lx + 12.0f * s, ry, lx + lw - 12.0f * s, ry + 38.0f * s,
                     (i % 2 == 0) ? ui::rgba(32, 34, 40, 215) : ui::rgba(24, 26, 32, 215));
      const uint32_t c1 = sc.pri_rgb, c2 = sc.sec_valid ? sc.sec_rgb : sc.pri_rgb;
      list.rect(lx + 20.0f * s, ry + 6.0f * s, lx + 48.0f * s, ry + 32.0f * s,
                ui::rgba(uint8_t((c1 >> 16) & 0xFF), uint8_t((c1 >> 8) & 0xFF), uint8_t(c1 & 0xFF), 255));
      list.rect(lx + 50.0f * s, ry + 6.0f * s, lx + 78.0f * s, ry + 32.0f * s,
                ui::rgba(uint8_t((c2 >> 16) & 0xFF), uint8_t((c2 >> 8) & 0xFF), uint8_t(c2 & 0xFF), 255));
      char buf[64];
      std::snprintf(buf, sizeof buf, "SPECIAL #%lld   •   TWO-TONE PWR %.1f", (long long)sc.id, sc.two_tone_power);
      list.text(lx + 88.0f * s, ry + 13.0f * s, buf,
                ui4::style(act ? ui::Font::kE : ui::Font::kB, 12.5f * s, ui4::kWhite));
      ry += 44.0f * s;
    }
  } else {
    // Authentic FH1 Vinyl Layer Stack chrome!
    const auto& layers = editor.document().layers;
    ui4::offwhite_strip(list, lx + 12.0f * s, ly + 10.0f * s, lx + lw - 12.0f * s, ly + 34.0f * s, s, false);
    list.text(lx + 22.0f * s, ly + 15.0f * s,
              "VINYL LAYER STACK (" + std::to_string(layers.size()) + ")  •  " +
                  std::to_string(editor.library_shape_count()) + " SHAPES",
              ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));
    const int n = int(layers.size());
    const int sel = n > 0 ? int(std::min(editor.selected_layer(), layers.size() - 1)) : 0;
    const int max_vis = std::max(1, int((lh - 48.0f * s) / (46.0f * s)));
    const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, n - max_vis));
    float ry = ly + 44.0f * s;
    for (int i = first; i < std::min(n, first + max_vis); ++i) {
      const auto& l = layers[size_t(i)];
      const bool act = (i == sel);
      if (act) ui4::brush(list, lx + 6.0f * s, ry, lx + lw - 6.0f * s, ry + 42.0f * s, ui4::kPink);
      else list.rect(lx + 12.0f * s, ry, lx + lw - 12.0f * s, ry + 40.0f * s,
                     (i % 2 == 0) ? ui::rgba(32, 34, 40, 215) : ui::rgba(24, 26, 32, 215));
      char idx_b[12];
      std::snprintf(idx_b, sizeof idx_b, "%02d", i + 1);
      list.text(lx + 20.0f * s, ry + 13.0f * s, idx_b, ui4::style(ui::Font::kE, 13.0f * s, ui4::kYellow));
      list.rect(lx + 46.0f * s, ry + 7.0f * s, lx + 72.0f * s, ry + 33.0f * s,
                ui::rgba(uint8_t((l.rgb >> 16) & 0xFF), uint8_t((l.rgb >> 8) & 0xFF), uint8_t(l.rgb & 0xFF), 255));
      list.frame(lx + 46.0f * s, ry + 7.0f * s, lx + 72.0f * s, ry + 33.0f * s, ui4::kWhite, 1.0f * s);

      ui::TextStyle st = ui4::style(act ? ui::Font::kE : ui::Font::kB, 12.5f * s, ui4::kWhite, ui::Align::kLeft, true);
      ui4::fit_text(l.shape_path, st, lw - 102.0f * s, 9.0f * s);
      list.text(lx + 82.0f * s, ry + 6.0f * s, l.shape_path, st);
      char sub[96];
      std::snprintf(sub, sizeof sub, "%s  •  POS(%.2f,%.2f)  SCALE(%.2f,%.2f)  ROT %.0f",
                    cars::livery_plane_name(l.plane), l.x, l.y, l.scale_x, l.scale_y, l.rot_deg);
      list.text(lx + 82.0f * s, ry + 24.0f * s, sub,
                ui4::style(ui::Font::kB, 9.5f * s, act ? ui4::kWhite : ui4::kDim));
      ry += 46.0f * s;
    }
    if (layers.empty()) {
      list.text(lx + 0.5f * lw, ly + 120.0f * s, "NO VINYL LAYERS YET — PRESS [Y] TO ADD",
                ui4::style(ui::Font::kB, 13.0f * s, ui4::kDim, ui::Align::kCenter));
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
                   {"DPAD_LR", editor.tab() == EditorTab::kVinylLayers ? "TRANSFORM" : "FINISH / COLOR"},
                   {"Y", "ADD VINYL"},
                   {"X", "DELETE"},
                   {"B", "BACK"}},
                  w, h, 56.0f);
}

}  // namespace carview
