// The first-time career's screens and HUD text: the controller loading
// screen, the large HUD instruction, the tutor prompt and the race to the
// festival's places counter, plus the full-screen fade overlay.
#pragma once

#include "cutscene/cutscene.h"
#include "race/first_time_career.h"
#include "ui.h"

namespace carview::ftc_ui {

void draw(ui::List& l, const race::FirstTimeCareer& ftc, float screen_w, float screen_h, int controller_layout,
          bool swap_handbrake_clutch);
// The black / white fade over the whole frame (drawn last, over movies).
void draw_overlay(ui::List& l, const cutscene::Overlay& o, float screen_w, float screen_h);

}  // namespace carview::ftc_ui
