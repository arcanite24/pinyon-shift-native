// Authentic Forza Horizon 1 (UI4) campaign presentation layer:
//   - Non-blocking character call-in banner (speaker name, role tag, animated
//     multi-bar audio waveform, smooth slide-in/out) + localized subtitle strip
//   - Progressive Minute-0 Viper drive objective tracker & GPS cue
//   - Wristband Ceremony, Boss Rival / Darius Flynt Star Showdown intro cards,
//     Showcase intro/outro, Barn Find reveal, Pre/Post-Race lower-thirds, and
//     Horizon Festival Credits roll with cinema letterbox bars
//   - Animated Post-Race & Milestone Reward Tally (rolling Credits, Popularity
//     rank climb, Wristband progress bar fill with tier-up flash, next-unlock
//     toast, and auto-set GPS prompt)
//   - Smooth eased FH1 magenta brush-wipe, cross-fade, and slide transitions
#pragma once

#include <string>

#include "race/campaign_flow.h"
#include "ui.h"

namespace carview::campaign_ui {

struct DrawContext {
  float screen_w = 1280.0f;
  float screen_h = 720.0f;
  int wristband_points = 0;
  int collected_wristband = 0;
  int popularity_rank = 250;
  bool skip_vo_with_a = true;
  bool metric_units = false;  // miles by default, as the original (Q277)
  bool suppress_subtitle = false;
  bool suppress_callin = false;
  // The race's own screens (race_flow.h) are up: the objective tracker and
  // the call-ins wait for free roam.
  bool race_screens = false;
  const race::CareerCatalog* catalog = nullptr;
};

// Master campaign overlay draw entry point (called cleanly at the end of HUD build).
void draw(ui::List& l, const race::CampaignFlow& flow, const DrawContext& ctx);

// Individual UI4 components (also directly testable in unit tests).
void draw_objective_tracker(ui::List& l, const race::CampaignFlow& flow, const DrawContext& ctx);
void draw_dialog_and_subtitles(ui::List& l, const race::DialogQueue& dialogs, const DrawContext& ctx);
void draw_cinematic_overlay(ui::List& l, const race::CampaignFlow& flow, const DrawContext& ctx);
void draw_reward_tally(ui::List& l, const race::RewardTallyState& tally, float screen_w, float screen_h);
void draw_credits_roll(ui::List& l, const race::CampaignFlow& flow, float screen_w, float screen_h);
void draw_transition(ui::List& l, const race::TransitionState& trans, float screen_w, float screen_h);

}  // namespace carview::campaign_ui
