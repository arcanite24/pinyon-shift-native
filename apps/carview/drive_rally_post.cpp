// The Horizon Rally's post sequence after a championship's last stage
// (game_rally_race_flow: postrace_results, CareerRewardsFlow and the
// RallyChampionSequence): stepping on A, the step clock and the champion's
// cutscene (Rally_Champion_Cutscene under H_HORIZON_CHAMPION).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "drive_session.h"
#include "race/rally_dialogue.h"

namespace carview {

void prime_world_and_place_vehicle_as_authored(DriveSession& s, std::array<double, 3> target_pos,
                                               double target_heading);

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr const char* kChampionCutscene = "Rally_Champion_Cutscene";

// The ui4audio player for a cutscene's CCutsceneAudioTrigger (its FEV path,
// else the play name; campaign_director.cpp's rule).
void play_cutscene_audio(AudioHost& audio, const std::string& group, const std::string& event) {
  if (!group.empty() && audio.play_ui_event(group + "/" + event)) return;
  (void)audio.play_ui(event);
}

std::string rally_champion_vo(const race::CareerCatalog& catalog, const std::map<int64_t, int>& event_places) {
  // CPlayRallyChampionDialogue: ChampionAgain once the Festival's headline
  // race (CareerEventStyle 6) is won, else HorizonRallyChampion.
  for (const auto& ev : catalog.events) {
    if (ev.style != 6) continue;
    const auto it = event_places.find(ev.event_id);
    if (it != event_places.end() && it->second == 1) return "ChampionAgain";
  }
  return "HorizonRallyChampion";
}

}  // namespace

void DriveSession::rally_post_enter_step() {
  auto& rs = app.rally_results_state;
  rs.step_t = 0.0;
  rs.show_table = false;
  const auto steps = rally_post_steps(rs);
  if (rs.step < 0 || rs.step >= int(steps.size())) return;
  std::fprintf(stderr, "carview: rally post step %d of %zu (kind %d, place %d)\n", rs.step + 1, steps.size(),
               int(steps[size_t(rs.step)]), rs.final_place);
  if (steps[size_t(rs.step)] != RallyPostStep::kChampion) return;
  // RallyChampionSequence: black_out, CLoadCutscene Rally_Champion_Cutscene
  // (the last stage's track set), CPlayRallyChampionDialogue, the cutscene
  // under the champion's screen.
  app.audio_host.trigger_vo(rally_champion_vo(app.career_catalog, app.career.event_places), false);
  rs.champion_cutscene = false;
  // interim (SPEC_QUESTIONS Q253): the cutscene loads and runs, but live its
  // CarSpace cams frame the ground and not the placed car (the place and
  // target frames are not resolved in the Rally world yet), so the screen
  // holds its 9.2 s over black instead.
  constexpr bool kChampionCutsceneLive = false;
  if (!kChampionCutsceneLive || app.headless || !app.race || app.rally_catalog.package_dir.empty() || rs.stage_dev_name.empty()) return;
  const auto xml = app.rally_catalog.package_dir / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "gamemodes" /
                   "ColoradoDirt" / "Cutscenes" / "Tracks" / ("cutscenes_" + rs.stage_dev_name + ".xml");
  if (!race_cuts.library().find(kChampionCutscene) && !race_cuts.add_file(xml)) {
    std::fprintf(stderr, "carview: rally champion: no %s\n", xml.string().c_str());
    return;
  }
  race_cuts.set_car_parts(car_parts(vehicle, host.showroom.bounds_min(), host.showroom.bounds_max()));
  if (!race_cuts.play(kChampionCutscene, false)) return;
  race_cut_name = kChampionCutscene;
  rally_champion_cut = true;
  rs.champion_cutscene = true;
  std::fprintf(stderr, "carview: rally champion: %s (%s)\n", kChampionCutscene, rs.stage_dev_name.c_str());
}

void DriveSession::rally_post_next() {
  auto& rs = app.rally_results_state;
  const auto steps = rally_post_steps(rs);
  if (rs.step + 1 < int(steps.size())) {
    ++rs.step;
    rally_post_enter_step();
    return;
  }
  rally_post_finish();
}

void DriveSession::rally_post_finish() {
  auto& rs = app.rally_results_state;
  const auto steps = rally_post_steps(rs);
  const bool champion_shown =
      std::find(steps.begin(), steps.end(), RallyPostStep::kChampion) != steps.end();
  if (rally_champion_cut) {
    race_cuts.stop();
    rally_champion_cut = false;
    race_cut_name.clear();
  }
  // CSetHasSeenRallyEndSequence: the champion's sequence plays once.
  if (champion_shown && !app.saved.rally_end_seen) app.saved.rally_end_seen = true;
  app.rally_results_open = false;
  // The intro race's end (game_rally_intro_flow): to the first Autoshow,
  // then the hub's first visit.
  const bool intro_heats = app.rally_session.is_heats() && app.saved.rally_flow == 1;
  app.rally_session.retire();
  if (intro_heats) {
    app.audio_host.trigger_vo("PostraceHeatsQualification", false);
    app.audio_host.trigger_vo("ToTheAutoshow", false);
    app.saved.rally_flow = 2;
    app.rally_first_autoshow_pending = true;
    app.rally_hub_open = false;
  } else {
    // The flow loads into the Rally's free roam: the hub.
    app.rally_hub_open = true;
  }
  // The championship's results, Rally XP and flags reach the profile before
  // the load (the session's exit save is skipped when leaving a race).
  save_now("rally championship");
  app.leave_race = true;
  running = false;
}

void DriveSession::rally_pep_talk_tick() {
  if (rally_pep_talk_done || !app.race || !app.race->is_stage() || !app.rally_session.active() ||
      app.rally_session.is_heats())
    return;
  // The race is held while the grid menu is open: the clock moving below 0
  // is the countdown (game_rally_race_flow's countdown state).
  const double clock = app.race->session().clock();
  const bool counting = rally_pep_have_clock && clock > rally_pep_prev_clock && clock < 0.0;
  rally_pep_prev_clock = clock;
  rally_pep_have_clock = true;
  if (!counting || grid_menu_open) return;
  rally_pep_talk_done = true;
  const int stage = app.rally_session.current_stage_index() + 1;
  const auto* ch = app.rally_catalog.find_championship(app.rally_session.event_id());
  const auto& field = app.rally_session.overall_standings();
  // The leader so far (no standings before the first stage).
  const bool leads = !field.empty() && app.rally_session.player_overall_place() == 1;
  const auto line = race::rally_pep_talk(stage, ch && ch->series_id == race::kRallyFinalSeries, leads);
  if (!line.trigger.empty()) app.audio_host.trigger_vo(line.trigger, false, line.variant);
}

void DriveSession::rally_post_tick(double dt, const std::vector<const cutscene::Event*>& events) {
  if (!app.rally_results_open) return;
  auto& rs = app.rally_results_state;
  rs.step_t += dt;
  const auto steps = rally_post_steps(rs);
  const RallyPostStep step = steps[size_t(std::clamp(rs.step, 0, int(steps.size()) - 1))];
  if (step != RallyPostStep::kChampion) return;
  if (rally_champion_cut) {
    for (const auto* e : events) {
      switch (e->type) {
        case cutscene::EventType::kPlacePlayerCar:
          // PlacePlayerCar_0: the car where the cutscene authors it.
          prime_world_and_place_vehicle_as_authored(*this, e->position(), e->rotation_deg() * kPi / 180.0);
          vehicle.set_velocity({0, 0, 0}, {0, 0, 0});
          break;
        case cutscene::EventType::kAudio:
          if (!e->attr("event").empty()) play_cutscene_audio(app.audio_host, e->attr("group"), e->attr("event"));
          break;
        case cutscene::EventType::kVoiceOver:
          if (!e->attr("event").empty()) app.audio_host.trigger_vo(e->attr("event"), false);
          break;
        default:
          // The crowd (CCutsceneAnimationTrigger) and HideAllCars are not
          // built (interim, SPEC_QUESTIONS Q253).
          break;
      }
    }
  }
  // doneOnCutsceneFinish: the screen closes when the cutscene ends (its 9.2 s
  // without it).
  const bool done = rally_champion_cut ? !race_cuts.active() : rs.step_t >= kRallyChampionSeconds;
  if (done) rally_post_next();
}

}  // namespace carview
