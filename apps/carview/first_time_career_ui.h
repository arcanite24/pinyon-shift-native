// The first-time career's screens and HUD text: the controller loading
// screen, the opening titles (H_OPENING_TITLES), the stage's HUD
// instruction and its large two-line form, the race to the festival's
// places counter, the driving tutor's input hints, plus the full-screen
// fade overlay. Strings are the shipped InGame / HelpButtons entries.
#pragma once

#include "cutscene/cutscene.h"
#include "race/driving_tutor.h"
#include "race/first_time_career.h"
#include "ui.h"

namespace carview::ftc_ui {

void draw(ui::List& l, const race::FirstTimeCareer& ftc, float screen_w, float screen_h, int controller_layout,
          bool swap_handbrake_clutch);
// An input hint ("PRESS <button> TO ACCELERATE"); kNone draws nothing.
void draw_hint(ui::List& l, race::TutorMessage m, float screen_w, float screen_h);
// The black / white fade over the whole frame (drawn last, over movies).
void draw_overlay(ui::List& l, const cutscene::Overlay& o, float screen_w, float screen_h);

}  // namespace carview::ftc_ui
