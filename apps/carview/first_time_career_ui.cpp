#include "first_time_career_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "fh1data/stringtables.h"
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
  };
  for (const auto& r : kRows)
    if (id == r.id) return r.text;
  return "";
}

std::string text_for(std::string_view table, std::string_view id) {
  return loc_str(table, id, fallback_for(id));
}

// A line with the string tables' {{colour='r g b a'}} markup: each run in its
// colour, centred on cx as a whole.
void coloured_line(ui::List& l, float cx, float y, std::string_view text, ui::TextStyle base) {
  struct Run {
    std::string text;
    uint32_t color;
  };
  std::vector<Run> runs;
  uint32_t color = base.color;
  size_t i = 0;
  while (i < text.size()) {
    const size_t open = text.find("{{colour='", i);
    if (open == std::string_view::npos) {
      runs.push_back({std::string(text.substr(i)), color});
      break;
    }
    if (open > i) runs.push_back({std::string(text.substr(i, open - i)), color});
    const size_t close = text.find("'}}", open);
    if (close == std::string_view::npos) break;
    int r = 255, g = 255, b = 255, a = 255;
    std::sscanf(std::string(text.substr(open + 10, close - open - 10)).c_str(), "%d %d %d %d", &r, &g, &b, &a);
    color = ui::rgba(uint8_t(r), uint8_t(g), uint8_t(b), uint8_t(a));
    i = close + 3;
  }
  base.align = ui::Align::kLeft;
  float total = 0;
  for (const auto& run : runs) total += ui::List::measure(run.text, base);
  float x = cx - 0.5f * total;
  for (const auto& run : runs) {
    ui::TextStyle st = base;
    st.color = run.color;
    l.text(x, y, run.text, st);
    x += ui::List::measure(run.text, base);
  }
}

// H_OPENING_TITLES: the four opening title cards over the diner cutscenes,
// timed by the scene's PLAY animation (race::opening_title_alpha, Q164).
// Interim: the cards are centred and the shadow layers are not drawn; the
// scene's placement is not read yet.
void draw_opening_titles(ui::List& l, double t, float w, float h) {
  static constexpr const char* kCards[] = {"OPENINGTITLES/TITLEMICROSOFT", "OPENINGTITLES/TITLETURN10",
                                           "OPENINGTITLES/TITLEPLAYGROUND", "OPENINGTITLES/HORIZONLOGO"};
  static_assert(std::size(kCards) == race::kOpeningTitleCards);
  int i = 0;
  float a = 0.0f;
  for (int c = 0; c < race::kOpeningTitleCards && a <= 0.0f; ++c) {
    i = c;
    a = race::opening_title_alpha(c, t);
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
  if (!ftc.hud_instruction().empty()) {
    const std::string text = text_for("InGame", ftc.hud_instruction());
    l.text(0.5f * w, 0.12f * h, text, ui4::style(ui::Font::kB, 22.0f * s, ui4::kWhite, ui::Align::kCenter));
  }
  if (ftc.tickets_remaining() >= 0) {
    std::string line = text_for("InGame", "IDS_HudInstruction_Tickets_Remaining");
    const auto at = line.find("{0}");
    if (at != std::string::npos) line.replace(at, 3, std::to_string(ftc.tickets_remaining()));
    coloured_line(l, 0.5f * w, 0.12f * h + 28.0f * s, line,
                  ui4::style(ui::Font::kB, 20.0f * s, ui4::kWhite, ui::Align::kCenter));
  }
}

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
