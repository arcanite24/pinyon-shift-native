#include "first_time_career_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "fh1data/stringtables.h"
#include "fh1data/ui_scene.h"
#include "hud_scene.h"
#include "scene_timeline.h"
#include "ui4_chrome.h"

namespace carview::ftc_ui {
namespace {

std::string loc_str(std::string_view table, std::string_view id, const char* fallback) {
  const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
  const std::string ref = "_&" + std::to_string(packed);
  const std::string resolved = ui::lookup_text(ref);
  if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) return resolved;
  return fallback;
}

// The shipped English (InGame / HelpButtons), for a disc without the string
// tables loaded.
const char* fallback_for(std::string_view id) {
  struct Row {
    const char* id;
    const char* text;
  };
  static constexpr Row kRows[] = {
      {"IDS_HudInstruction_Travel_To_Festival", "Drive to the festival"},
      {"IDS_HudInstruction_Race_To_Festival", "Race to the festival"},
      {"IDS_HudInstruction_Travel_To_Workshop", "Drive to the garage to meet Dak"},
      {"IDS_HudInstruction_Do_All_Comers_Final", "Head to the Horizon Heats"},
      {"IDS_HudInstruction_Travel_To_RaceCentral", "Drive to Race Central to collect your first wristband"},
      {"IDS_HudInstruction_Go_To_Red_Rock", "Get to the Showcase event in Red Rock"},
      {"IDS_HudInstruction_Tickets_Remaining", "Heats Places Remaining: {{colour='255 236 95 255'}}{0}"},
      {"IDS_HudMsg_RaceTo", "RACE TO"},
      {"IDS_HudMsg_TheFestival", "THE FESTIVAL"},
      {"IDS_HudMsg_EnterThe", "ENTER THE"},
      {"IDS_HudMsg_HorizonHeats", "HORIZON HEATS"},
      {"IDS_HudMsg_DriveTo", "DRIVE TO"},
      {"IDS_HudMsg_RaceCentral", "RACE CENTRAL"},
      {"IDS_HudMsg_ShowcaseEvent", "SHOWCASE EVENT"},
      {"IDS_InputHint_Accelerate_L", "PRESS"},
      {"IDS_InputHint_Accelerate_R", "TO ACCELERATE"},
      {"IDS_InputHint_Brake_L", "PRESS"},
      {"IDS_InputHint_Brake_R", "TO BRAKE"},
      {"IDS_InputHint_Rewind_L", "PRESS"},
      {"IDS_InputHint_Rewind_R", "TO REWIND"},
      {"IDS_InputHint_EBrake_L", "PRESS"},
      {"IDS_InputHint_EBrake_R", "TO USE E-BRAKE"},
      {"IDS_InputHint_Camera_L", "PRESS"},
      {"IDS_InputHint_Camera_R", "TO CHANGE THE CAMERA"},
      {"IDS_InputHint_Map_L", "PRESS"},
      {"IDS_InputHint_Map_R", "TO OPEN THE MAP"},
      {"IDS_Get_to_the_Festival", "GET TO THE FESTIVAL"},
      {"IDS_Places_Left", "PLACES LEFT"},
      {"IDS_MapTip_SetRouteToHeats", "Please set a route to the Horizon Heats event."},
  };
  for (const auto& r : kRows)
    if (id == r.id) return r.text;
  return "";
}

std::string text_for(std::string_view table, std::string_view id) {
  return loc_str(table, id, fallback_for(id));
}

// H_OPENING_TITLES: the four opening title cards over the diner cutscenes,
// timed by the scene's PLAY animation (race::opening_title_alpha, Q164).
// Interim: the cards are centred and the shadow layers are not drawn; the
// scene's placement is not read yet.
// The cards' key times (seconds) from the scene's PLAY slide: each card
// layer's enabled opacity curve. False without the scene or a card's curve.
bool scene_card_keys(std::array<std::array<double, 4>, race::kOpeningTitleCards>& out) {
  const fh1::UiScene* sc = ui_scene("H_OPENING_TITLES");
  static constexpr const char* kLayers[] = {"_Microsoft", "_Turn10", "_Playground", "_Horizon"};
  static_assert(std::size(kLayers) == race::kOpeningTitleCards);
  for (int c = 0; c < race::kOpeningTitleCards; ++c) {
    const auto k = slide_curve_keys(sc, 0, "PLAY", kLayers[c], fh1::ui_attr::kOpacity);
    if (k.size() != 4) return false;
    for (size_t j = 0; j < 4; ++j) out[size_t(c)][j] = k[j] / 1000.0;
  }
  return true;
}

void draw_opening_titles(ui::List& l, double t, float w, float h) {
  static constexpr const char* kCards[] = {"OPENINGTITLES/TITLEMICROSOFT", "OPENINGTITLES/TITLETURN10",
                                           "OPENINGTITLES/TITLEPLAYGROUND", "OPENINGTITLES/HORIZONLOGO"};
  static_assert(std::size(kCards) == race::kOpeningTitleCards);
  // The timing from the scene's own curves when it is there (the table in
  // race::opening_title_alpha otherwise).
  std::array<std::array<double, 4>, race::kOpeningTitleCards> keys{};
  const bool from_scene = scene_card_keys(keys);
  int i = 0;
  float a = 0.0f;
  for (int c = 0; c < race::kOpeningTitleCards && a <= 0.0f; ++c) {
    i = c;
    a = from_scene ? race::opening_title_alpha(keys[size_t(c)], t) : race::opening_title_alpha(c, t);
  }
  if (a <= 0.0f) return;
  std::array<float, 4> uv{};
  if (!ui4::texture(kCards[i], uv)) return;
  const float aspect = std::max(0.1f, std::abs(uv[2] - uv[0]) / std::max(1e-6f, std::abs(uv[3] - uv[1])));
  const float bw = 0.42f * w, bh = bw / aspect;
  const float cx = 0.5f * w, cy = 0.5f * h;
  l.image(cx - 0.5f * bw, cy - 0.5f * bh, cx + 0.5f * bw, cy + 0.5f * bh, uv,
          ui::rgba(255, 255, 255, uint8_t(std::lround(a * 255.0f))));
}

}  // namespace

void draw(ui::List& l, const race::FirstTimeCareer& ftc, float w, float h, int controller_layout, bool swap) {
  const float s = h / 720.0f;
  if (ftc.show_controls_screen()) {
    // The controller loading screen (ControllerLoadingBackground and the
    // layout diagram) while the opening loads.
    l.rect(0, 0, w, h, ui::rgba(8, 8, 10, 255));
    ui4::draw_controller_layout_diagram(l, 0.12f * w, 0.14f * h, 0.88f * w, 0.86f * h, s, controller_layout, swap);
    return;
  }
  if (ftc.show_opening_credits()) draw_opening_titles(l, ftc.opening_credits_time(), w, h);
  if (!ftc.hud_visible()) return;
  if (!ftc.large_instruction_1().empty()) {
    // The large two-line instruction as a stage starts.
    const std::string a = text_for("InGame", ftc.large_instruction_1());
    const std::string b = text_for("InGame", ftc.large_instruction_2());
    l.text(0.5f * w, 0.30f * h, a, ui4::style(ui::Font::kE, 34.0f * s, ui4::kWhite, ui::Align::kCenter));
    l.text(0.5f * w, 0.30f * h + 40.0f * s, b, ui4::style(ui::Font::kE, 54.0f * s, ui4::kWhite, ui::Align::kCenter));
  }
  if (!ftc.hud_instruction().empty()) ui4::draw_info_tip(l, text_for("InGame", ftc.hud_instruction()), w, h);
  if (ftc.tickets_remaining() >= 0) {
    // The places counter, top right: GET TO THE FESTIVAL over PLACES LEFT
    // and the count (measured on f00420_first30a_orig.png at 1280x720).
    const uint32_t band = ui::rgba(0, 0, 0, 110);
    const std::string top = text_for("InGame", "IDS_Get_to_the_Festival");
    const std::string mid = text_for("InGame", "IDS_Places_Left");
    const std::string num = std::to_string(ftc.tickets_remaining());
    const auto ts = ui4::style(ui::Font::kE, 24.0f * s, ui4::kWhite, ui::Align::kRight);
    const auto ms = ui4::style(ui::Font::kE, 19.0f * s, ui4::kWhite, ui::Align::kRight);
    const auto ns = ui4::style(ui::Font::kE, 54.0f * s, ui4::kWhite, ui::Align::kRight);
    const float right = 1178.0f * s + (w - 1280.0f * s);
    const float nw = ui::List::measure(num, ns);
    l.rect(right - ui::List::measure(top, ts) - 10.0f * s, 36.0f * s, right + 8.0f * s, 61.0f * s, band);
    l.rect(right + 27.0f * s - nw - ui::List::measure(mid, ms) - 14.0f * s, 60.0f * s, right + 27.0f * s - nw, 80.0f * s,
           band);
    l.text(right, 36.0f * s, top, ts);
    l.text(right + 27.0f * s - nw - 4.0f * s, 60.0f * s, mid, ms);
    l.text(right + 27.0f * s, 56.0f * s, num, ns);
  }
}

std::string tip_text(std::string_view table, std::string_view id) { return text_for(table, id); }

void draw_hint(ui::List& l, race::TutorMessage m, float w, float h) {
  if (m == race::TutorMessage::kNone) return;
  const race::TutorHint hint = race::tutor_hint(m);
  const float s = h / 720.0f;
  const auto st = ui4::style(ui::Font::kB, 22.0f * s, ui4::kWhite, ui::Align::kLeft);
  const std::string left = text_for("HelpButtons", hint.left_id);
  const std::string right = text_for("HelpButtons", hint.right_id);
  const float r = 13.0f * s, gap = 8.0f * s;
  const float lw = ui::List::measure(left, st), rw = ui::List::measure(right, st);
  const float total = lw + gap + 2.0f * r + gap + rw;
  const float y = 0.80f * h;
  float x = 0.5f * w - 0.5f * total;
  l.text(x, y, left, st);
  x += lw + gap;
  ui4::button_glyph(l, x + r, y + 0.5f * st.size * 0.8f, r, hint.button);
  x += 2.0f * r + gap;
  l.text(x, y, right, st);
}

void draw_overlay(ui::List& l, const cutscene::Overlay& o, float w, float h) {
  if (o.alpha <= 0.002f) return;
  auto c8 = [](float v) { return uint8_t(std::clamp(int(std::lround(v * 255.0f)), 0, 255)); };
  l.rect(0, 0, w, h, ui::rgba(c8(o.rgb[0]), c8(o.rgb[1]), c8(o.rgb[2]), c8(o.alpha)));
}

}  // namespace carview::ftc_ui
