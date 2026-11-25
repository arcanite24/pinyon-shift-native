#include "first_time_career_ui.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

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

// English fallbacks; InGame.str carries the shipped text. The instruction
// fallbacks other than the tickets line are interim wording.
const char* fallback_for(std::string_view id) {
  if (id == "IDS_Accelerate_Message") return "HOLD DOWN THE RIGHT TRIGGER TO GO.";
  if (id == "IDS_HudInstruction_Race_To_Festival") return "RACE TO THE HORIZON FESTIVAL";
  if (id == "IDS_HudInstruction_Do_All_Comers_Heat") return "ENTER THE ALL COMERS HEAT";
  if (id == "IDS_HudInstruction_Travel_To_RaceCentral") return "TRAVEL TO RACE CENTRAL";
  return "";
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
  if (!ftc.hud_visible()) return;
  if (!ftc.hud_instruction().empty()) {
    const std::string text = loc_str("InGame", ftc.hud_instruction(), fallback_for(ftc.hud_instruction()));
    l.text(0.5f * w, 0.18f * h, text, ui4::style(ui::Font::kE, 30.0f * s, ui4::kWhite, ui::Align::kCenter));
  }
  if (ftc.tickets_remaining() >= 0) {
    std::string line = loc_str("InGame", "IDS_HudInstruction_Tickets_Remaining", "Heats Places Remaining: {0}");
    const auto at = line.find("{0}");
    if (at != std::string::npos) line.replace(at, 3, std::to_string(ftc.tickets_remaining()));
    l.text(0.5f * w, 0.24f * h, line, ui4::style(ui::Font::kB, 20.0f * s, ui4::kWhite, ui::Align::kCenter));
  }
  if (!ftc.tutor_prompt().empty()) {
    const std::string text = loc_str("InGame", ftc.tutor_prompt(), fallback_for(ftc.tutor_prompt()));
    l.text(0.5f * w, 0.78f * h, text, ui4::style(ui::Font::kB, 22.0f * s, ui4::kWhite, ui::Align::kCenter));
  }
}

void draw_overlay(ui::List& l, const cutscene::Overlay& o, float w, float h) {
  if (o.alpha <= 0.002f) return;
  auto c8 = [](float v) { return uint8_t(std::clamp(int(std::lround(v * 255.0f)), 0, 255)); };
  l.rect(0, 0, w, h, ui::rgba(c8(o.rgb[0]), c8(o.rgb[1]), c8(o.rgb[2]), c8(o.alpha)));
}

}  // namespace carview::ftc_ui
