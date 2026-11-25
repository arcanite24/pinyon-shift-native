// The help bar on BUTTON_BAR_SCENE's own nodes (ButtonBar/Buttons/
// HelpButton0..6), drawn for ui4::button_bar() once install_scene_help_bar()
// has run. Kept apart from hud_screen.cpp so the libraries that only use the
// HUD's helpers do not pull in the ui4 chrome.
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "hud_screen.h"

namespace carview {
namespace {

// The original's packing, measured on 07_pause_menu_orig.png (its stackers
// are not run): a button's glyph sits with its left edge 15 px left of the
// button's origin, the label 5 px after the glyph, and the pill (its crop
// clear for 3 px at the left) from 19 px left of the origin to 11 px past
// the label; the next button's pill starts 5 px after the last one's.
constexpr float kGlyphLead = 15.0f, kGlyphR = 11.0f, kLabelGap = 5.0f;
constexpr float kPillLead = 19.0f, kPillClear = 3.0f, kPillPad = 11.0f, kPillFeather = 0.0f, kButtonGap = 5.0f;
// The first pill's left edge in the scene (button_bar()'s default).
constexpr float kFirstPill = 70.0f;

struct Glyph {
  const char* button;
  float cx, cy;
};

}  // namespace

bool draw_help_bar(ui::List& l, const std::vector<ui4::Prompt>& items, float w, float h, float left_x_canvas) {
  SceneScreen sc(kButtonBarScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  const int buttons = sc.node("ButtonBar/Buttons");
  if (buttons < 0) return false;
  sc.bindings().fit_text = false;
  const float s = h / 720.0f;
  const SceneFrame f = hud_frame(HudAnchor::kBottomLeft, w, h, s);
  const float right_limit = w / s - 40.0f;

  std::vector<Glyph> glyphs;
  size_t item = 0;
  float x = sd.at(sd.find("HelpButton0", buttons))[0] + (left_x_canvas - kFirstPill);
  int slot = 0;
  for (; slot < 7; ++slot) {
    const int b = sd.find("HelpButton" + std::to_string(slot), buttons);
    if (b < 0) break;
    while (item < items.size() && items[item].label.empty()) ++item;
    if (item >= items.size()) break;
    const ui4::Prompt& p = items[item];
    const int text = sd.find("Text_BUTTON", b), pill = sd.find("Pill", b), logo = sd.find("BUTTON_LOGO", b);
    const float hw = ui4::button_glyph_half_width(p.button, kGlyphR);
    const float gcx = x - kGlyphLead + hw;
    const float label_left = gcx + hw + kLabelGap;
    const float tw = sd.measure(text, p.label);
    const float pill_left = x - kPillLead, pill_right = label_left + tw + kPillPad;
    if (slot > 0 && pill_right > right_limit) break;
    // The button moved to its place; its children placed against it.
    const float bdx = x - sd.at(b)[0];
    sc.at(b).dx = bdx;
    sc.hide(logo);
    sc.text(text, p.label);
    if (text >= 0) sc.at(text).dx = label_left - sd.at(text)[0] - bdx;
    if (pill >= 0) sc.at(pill).visible_width = pill_right - (pill_left - kPillClear) + kPillFeather;
    glyphs.push_back({p.button, gcx, logo >= 0 ? sd.at(logo)[1] : 668.0f});
    l.add_hit(f.x(pill_left), f.y(654.0f), f.x(pill_right), f.y(682.0f), ui4::kHitButton, slot);
    x = pill_right + kButtonGap + kPillLead;
    ++item;
  }
  for (int rest = slot; rest < 7; ++rest) sc.hide(sd.find("HelpButton" + std::to_string(rest), buttons));
  if (glyphs.empty()) return true;
  sc.draw(l, f, sc.node("ButtonBar"));
  for (const Glyph& g : glyphs) ui4::button_glyph(l, f.x(g.cx), f.y(g.cy), kGlyphR * s, g.button);
  return true;
}

void install_scene_help_bar() { ui4::set_button_bar_drawer(&draw_help_bar); }

}  // namespace carview
