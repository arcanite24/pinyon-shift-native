#include "livery_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
  // Header + 3 tabs.
  list.rect(0, 0, w, 90.0f, ui::rgba(12, 15, 22, 232));
  list.rect(0, 88.0f, w, 91.0f, ui::rgba(36, 178, 240, 240));

  ui::TextStyle ts;
  ts.size = 23.0f;
  ts.color = ui::rgba(255, 255, 255);
  ts.font = ui::Font::kE;
  ts.slant = 0.14f;
  list.text(28.0f, 14.0f, "PAINT SHOP & VINYL EDITOR  //  " + car_title, ts);

  const char* tabs[3] = {"PAINT & FINISHES", "SPECIAL (102)", "VINYL LAYERS"};
  float tx = 28.0f;
  for (int i = 0; i < 3; ++i) {
    const bool act = (EditorTab(i) == editor.tab());
    const float tw = 224.0f;
    list.rect(tx, 50.0f, tx + tw, 80.0f, act ? ui::rgba(36, 178, 240, 245) : ui::rgba(28, 34, 46, 210), 4.0f);
    ui::TextStyle cs;
    cs.size = 11.5f;
    cs.color = act ? ui::rgba(12, 16, 22) : ui::rgba(202, 210, 224);
    cs.align = ui::Align::kCenter;
    cs.font = ui::Font::kB;
    list.text(tx + tw * 0.5f, 59.0f, tabs[i], cs);
    tx += tw + 8.0f;
  }

  // Left control card.
  const float lx = 28.0f, ly = 108.0f, lw = 420.0f, lh = h - 164.0f;
  list.rect(lx, ly, lx + lw, ly + lh, ui::rgba(14, 18, 26, 226), 8.0f);
  list.frame(lx, ly, lx + lw, ly + lh, ui::rgba(255, 255, 255, 36), 1.0f, 8.0f);

  ui::TextStyle lbl;
  lbl.size = 13.5f;
  lbl.color = ui::rgba(232, 238, 248);
  lbl.font = ui::Font::kB;

  if (editor.tab() == EditorTab::kPaint) {
    // Region list + HSV colour swatch grid.
    list.text(lx + 16.0f, ly + 14.0f, "PAINT REGION", lbl);
    float ry = ly + 38.0f;
    for (int i = 0; i < int(PaintRegion::kCount); ++i) {
      const bool act = (PaintRegion(i) == editor.region());
      list.rect(lx + 12.0f, ry, lx + lw - 12.0f, ry + 32.0f,
                act ? ui::rgba(36, 178, 240, 232) : ui::rgba(26, 32, 44, 195), 4.0f);
      ui::TextStyle rs = lbl;
      rs.size = 12.5f;
      rs.color = act ? ui::rgba(12, 16, 22) : ui::rgba(220, 226, 236);
      list.text(lx + 22.0f, ry + 9.0f, paint_region_name(PaintRegion(i)), rs);
      ry += 36.0f;
    }

    // Active finish + HSV palette grid preview.
    const auto& body = editor.document().paint.body;
    list.text(lx + 16.0f, ry + 12.0f,
              std::string("FINISH: ") + cars::paint_finish_name(body.finish), lbl);
    const float gx = lx + 16.0f, gy = ry + 38.0f;
    constexpr int kCols = 12, kRows = 5;
    const float cw = (lw - 32.0f) / float(kCols);
    const float ch = 22.0f;
    for (int r = 0; r < kRows; ++r) {
      for (int c = 0; c < kCols; ++c) {
        const uint32_t col = cars::hsv_to_rgb(float(c) / float(kCols), 0.35f + 0.16f * float(r),
                                              1.0f - 0.14f * float(r));
        list.rect(gx + float(c) * cw, gy + float(r) * ch, gx + float(c + 1) * cw - 2.0f,
                  gy + float(r + 1) * ch - 2.0f,
                  ui::rgba(uint8_t((col >> 16) & 0xFF), uint8_t((col >> 8) & 0xFF), uint8_t(col & 0xFF), 255), 2.0f);
      }
    }
  } else if (editor.tab() == EditorTab::kSpecialColors) {
    list.text(lx + 16.0f, ly + 14.0f,
              "LIST_SPECIALCOLORS (" + std::to_string(editor.special_colors().size()) + " TWO-TONE PAINTS)", lbl);
    const auto& sp = editor.special_colors();
    const int n = int(sp.size());
    const int sel = int(editor.selected_special());
    const int first = std::clamp(sel - 4, 0, std::max(0, n - 10));
    float ry = ly + 42.0f;
    for (int i = first; i < std::min(n, first + 10); ++i) {
      const auto& sc = sp[size_t(i)];
      const bool act = (i == sel);
      list.rect(lx + 12.0f, ry, lx + lw - 12.0f, ry + 38.0f,
                act ? ui::rgba(36, 178, 240, 232) : ui::rgba(26, 32, 44, 195), 4.0f);
      const uint32_t c1 = sc.pri_rgb, c2 = sc.sec_valid ? sc.sec_rgb : sc.pri_rgb;
      list.rect(lx + 20.0f, ry + 6.0f, lx + 46.0f, ry + 32.0f,
                ui::rgba(uint8_t((c1 >> 16) & 0xFF), uint8_t((c1 >> 8) & 0xFF), uint8_t(c1 & 0xFF), 255), 3.0f);
      list.rect(lx + 48.0f, ry + 6.0f, lx + 74.0f, ry + 32.0f,
                ui::rgba(uint8_t((c2 >> 16) & 0xFF), uint8_t((c2 >> 8) & 0xFF), uint8_t(c2 & 0xFF), 255), 3.0f);
      char buf[64];
      std::snprintf(buf, sizeof buf, "SPECIAL #%lld  (PWR %.1f)", (long long)sc.id, sc.two_tone_power);
      ui::TextStyle st = lbl;
      st.size = 12.5f;
      st.color = act ? ui::rgba(12, 16, 22) : ui::rgba(232, 238, 248);
      list.text(lx + 86.0f, ry + 12.0f, buf, st);
      ry += 42.0f;
    }
  } else {
    const auto& layers = editor.document().layers;
    list.text(lx + 16.0f, ly + 14.0f,
              "VINYL LAYERS (" + std::to_string(layers.size()) + ")   |   LIB: " +
                  std::to_string(editor.library_shape_count()),
              lbl);
    float ry = ly + 42.0f;
    for (size_t i = 0; i < std::min<size_t>(layers.size(), 10); ++i) {
      const auto& l = layers[i];
      const bool act = (i == editor.selected_layer());
      list.rect(lx + 12.0f, ry, lx + lw - 12.0f, ry + 38.0f,
                act ? ui::rgba(36, 178, 240, 232) : ui::rgba(26, 32, 44, 195), 4.0f);
      list.rect(lx + 18.0f, ry + 7.0f, lx + 42.0f, ry + 31.0f,
                ui::rgba(uint8_t((l.rgb >> 16) & 0xFF), uint8_t((l.rgb >> 8) & 0xFF), uint8_t(l.rgb & 0xFF), 255),
                3.0f);
      ui::TextStyle st = lbl;
      st.size = 12.0f;
      st.color = act ? ui::rgba(12, 16, 22) : ui::rgba(236, 242, 250);
      list.text(lx + 52.0f, ry + 6.0f, l.shape_path, st);
      char sub[96];
      std::snprintf(sub, sizeof sub, "%s XY(%.2f,%.2f) S(%.2f,%.2f) R%.0f", cars::livery_plane_name(l.plane), l.x,
                    l.y, l.scale_x, l.scale_y, l.rot_deg);
      st.size = 9.0f;
      st.color = act ? ui::rgba(24, 30, 38) : ui::rgba(176, 186, 202);
      list.text(lx + 52.0f, ry + 23.0f, sub, st);
      ry += 42.0f;
    }
  }

  // Footer.
  list.rect(0, h - 38.0f, w, h, ui::rgba(10, 12, 18, 228));
  ui::TextStyle ft;
  ft.size = 12.0f;
  ft.color = ui::rgba(196, 204, 218);
  list.text(28.0f, h - 25.0f,
            notice.empty()
                ? "[Q/E] TAB  [UP/DOWN] SELECT  [LEFT/RIGHT] FINISH/COLOR  [N] ADD VINYL  [DEL] REMOVE  [ENTER] SAVE .LIV"
                : notice,
            ft);
}

}  // namespace carview
