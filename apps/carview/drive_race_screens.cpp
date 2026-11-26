// The original's screens around a race (race_flow.h) in the drive session:
// the marker's event summary and car select, the grid's title card and menu,
// and the rewards from the finish banner to the load back to free roam.
#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "race/rewards.h"
#include "race_screens.h"
#include "world/collision.h"

namespace carview {

namespace {

using S = race_flow::Screen;

race_flow::Kind race_kind(const DriveApp& app) {
  if (!app.race) return race_flow::Kind::kFestival;
  if (app.race->rivals_solo()) return race_flow::Kind::kRivals;
  if (const auto* ev = app.career_catalog.find_event(app.race_event)) return race_flow::kind_of_style(ev->style);
  return race_flow::Kind::kFestival;
}

}  // namespace

// --- The event summary (festival_event_entry_flow) ------------------------

void DriveSession::open_event_summary(const race::CareerEvent& ev) {
  event_car_selected = 0;
  event_car_specs = true;
  event_car_media.clear();
  event_summary_hud = event_ticket(ev, &event_cars);
  summary_event = &ev;
  summary_restrictions = false;
  summary_opened_at = vehicle.time();
  entry_screens.start(race_flow::entry_steps(event_summary_hud.eligible));
  events_page.close();
  app.audio_host.trigger_ui(audio::UiCue::kAccept);
}

EventSummaryHud DriveSession::event_ticket(const race::CareerEvent& ev,
                                           std::vector<EventCar>* cars) {
  int64_t cid = 0;
  int clid = 2;
  current_car_info(cid, clid);
  const auto names = app.class_name_list();
  const auto elig = app.career_catalog.check_eligibility(ev, cid, clid, names, slice12.trainer.effective_any_car());
  // Car select lists the garage's eligible cars (ShowRegisterEventCarScreen).
  std::vector<EventCar> eligible_cars;
  if (!elig.eligible && app.have_garage) {
    for (const auto& gc : app.career_garage.cars) {
      const int gcls = app.class_bands.empty() ? gc.class_id : garage::class_of(gc.performance_index, app.class_bands);
      if (!app.career_catalog.check_eligibility(ev, gc.car_id, gcls, names).eligible) continue;
      if (auto it = app.baked_ids.find(gc.car_id); it != app.baked_ids.end())
        eligible_cars.push_back({carview::car_name(it->second), it->second, gc.car_id, gc.performance_index});
    }
  }

  EventSummaryHud h;
  h.style = ev.style;
  h.name = race::resolve_event_display_title(&ev);
  h.type = event_type_text(ev.style);
  h.ticket = event_ticket_texture(ev.style, ev.ticket);
  if (!ev.track_name_ref.empty()) {
    const std::string tn = carview::game_text(ev.track_name_ref);
    if (tn != ev.track_name_ref) h.track = tn;
  }
  h.track_info = event_track_info(ev.track_surface, ev.laps > 1);
  h.points = app.wristbands.points_for(ev.level, 1);
  h.credits = int64_t(std::llround(ev.cash_prize));
  if (ev.target_class > 0 && size_t(ev.target_class) < names.size()) h.car_class = names[size_t(ev.target_class)];
  if (!ev.restriction_desc_ref.empty()) {
    const std::string rd = carview::game_text(ev.restriction_desc_ref);
    if (rd != ev.restriction_desc_ref) h.restriction = rd;
  }
  if (!h.car_class.empty()) h.restriction_detail = format1(text_of(race_text::kClassOrBelow), h.car_class);
  h.eligible = elig.eligible;
  h.eligibility = text_of(elig.eligible             ? race_text::kCurrentCarEligible
                          : !eligible_cars.empty() ? race_text::kGarageCarsEligible
                                                   : race_text::kNoCarsEligible);
  if (elig.uses_loaner && elig.loaner_car_id > 0)
    if (const auto* cs = app.career_catalog.find_car(elig.loaner_car_id)) h.provided_car = carview::car_name(cs->media_name);
  if (const int best = app.career_catalog.best_place(ev, app.career); best > 0)
    h.best_finish = std::to_string(best) + place_suffix(best);
  if (cars) *cars = std::move(eligible_cars);
  return h;
}

void DriveSession::close_event_summary() {
  entry_screens.stop();
  summary_event = nullptr;
  summary_restrictions = false;
}

void DriveSession::event_summary_accept() {
  switch (entry_screens.screen()) {
    case S::kEventSummary:
      if (summary_restrictions) {
        summary_restrictions = false;
        app.audio_host.trigger_ui(audio::UiCue::kAccept);
        return;
      }
      if (entry_screens.steps().size() > 2 && event_cars.empty()) {
        // No car in the garage qualifies: the summary stays.
        notice = text_of(race_text::kNoCarsEligible);
        notice_until = vehicle.time() + 3.0;
        app.audio_host.trigger_ui(audio::UiCue::kCancel);
        return;
      }
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      entry_screens.next();
      return;
    case S::kCarSelect:
      if (event_cars.empty()) return;
      event_car_media = event_cars[size_t(std::clamp(event_car_selected, 0, int(event_cars.size()) - 1))].media;
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      entry_screens.next();
      return;
    default: return;
  }
}

void DriveSession::event_summary_back() {
  switch (entry_screens.screen()) {
    case S::kEventSummary:
      app.audio_host.trigger_ui(audio::UiCue::kCancel);
      if (summary_restrictions) summary_restrictions = false;
      else close_event_summary();
      return;
    case S::kCarSelect:
      app.audio_host.trigger_ui(audio::UiCue::kCancel);
      entry_screens.start(race_flow::entry_steps(false));
      return;
    default: return;
  }
}

void DriveSession::event_summary_move(int by) {
  if (entry_screens.screen() != S::kCarSelect || event_cars.empty()) return;
  event_car_selected = wrap_row(event_car_selected, by, int(event_cars.size()));
  app.audio_host.trigger_ui(audio::UiCue::kMove);
}

void DriveSession::event_summary_restrictions() {
  if (entry_screens.screen() == S::kCarSelect) {
    // Y TOGGLE: the selected car's panel.
    event_car_specs = !event_car_specs;
    app.audio_host.trigger_ui(audio::UiCue::kMove);
    return;
  }
  if (entry_screens.screen() != S::kEventSummary) return;
  summary_restrictions = !summary_restrictions;
  app.audio_host.trigger_ui(audio::UiCue::kMove);
}

// --- The race ---------------------------------------------------------------

void DriveSession::update_race_screens(double dt) {
  // The entry: after the fade, the event loads.
  if (entry_screens.active()) {
    const bool fading = entry_screens.screen() == S::kEntryFade;
    entry_screens.update(dt);
    if (fading && !entry_screens.active()) {
      const race::CareerEvent* ev = summary_event;
      const std::string car = event_car_media;
      close_event_summary();
      if (ev && try_enter_event(*ev, car) && !running) entry_black = true;
    }
  }
  event_summary_hud.screen = entry_screens.screen();
  event_summary_hud.t = vehicle.time() - summary_opened_at;
  event_summary_hud.restrictions_view = summary_restrictions;
  event_summary_hud.cars.clear();
  for (const auto& c : event_cars) event_summary_hud.cars.push_back(c.name);
  event_summary_hud.car_selected = event_car_selected;
  // Car select on the car list (108_C_BUY_CARSELECT, 11_car_select_orig.png).
  if (entry_screens.screen() == S::kCarSelect) {
    namespace t = carview::car_list_text;
    CarListHud& cl = event_summary_hud.car_list;
    cl = CarListHud{};
    cl.title = text_of(t::kCarSelect);
    for (const auto& c : event_cars) {
      CarTile tile;
      tile.car_id = c.car_id;
      cl.tiles.push_back(std::move(tile));
    }
    cl.selected = std::clamp(event_car_selected, 0, std::max(0, int(event_cars.size()) - 1));
    if (!event_cars.empty() && event_car_specs) {
      const auto& c = event_cars[size_t(cl.selected)];
      cl.details = car_list_details(c.car_id, c.raw_pi);
    }
    // interim (SPEC_QUESTIONS Q97): the original's X CAR LIST and SEARCH are
    // not built; A, B and Y TOGGLE are.
    cl.prompts = {{"A", text_of(t::kSelect)}, {"B", text_of(t::kBack)}, {"Y", text_of(t::kToggle)}};
  }
  event_summary_hud.fade =
      entry_screens.screen() == S::kEntryFade ? std::clamp(entry_screens.elapsed() / race_flow::timing::kEntryFade, 0.0, 1.0)
                                              : 0.0;

  // base_festival_race_flow.xml speaks (radiodj.post_race) before the
  // results, then nothing over the rewards: the campaign's post-race call-in
  // waits from the finish until free roam.
  app.campaign.flow().dialogs().set_held(app.race && !app.headless && app.race->player_finished());
  sync_h2h_screens(dt);
  if (!app.race || app.headless) {
    race_screens.stop();
    if (race_cuts.active()) race_cuts.stop();
    race_cut_name.clear();
    // Headless runs have no screens to step through: the campaign's outro
    // (finale, boss, showcase) follows the finish at once.
    if (app.race && app.headless && app.race->player_finished())
      apply_campaign_action(app.campaign.flow().on_post_race_done());
    rally_post_tick(dt, {});
    return;
  }
  race_screens.update(dt);
  // A wristband to collect: the points page waits (PLEASE WAIT), then moves on.
  if (race_screens.screen() == S::kPoints && wristband_to_collect() &&
      race_screens.elapsed() >= race_flow::timing::kPointsCollect)
    race_screens.next();
  // The pre-race: the count starts when the flow reaches it.
  if (grid_menu_open && race_screens.screen() == S::kCountdown) {
    grid_menu_open = false;
    pre_race_blend_t = 0.0;
    race_screens.stop();
  }
  if (race_quitting) {
    // QUIT RACE: after the black-out, the load (kDone holds the black).
    if (race_screens.screen() == S::kDone && !app.leave_race) {
      app.leave_race = true;
      running = false;
    }
  } else {
    // The finish: the banner when the player crosses the line.
    if (!grid_menu_open && !race_screens.active() && results_page == 0 && app.race->player_finished())
      race_screens.start({S::kFinishBanner});
    if (race_screens.screen() == S::kDone && !app.leave_race) finish_race_screens();
  }
  sync_race_cutscene();
  const auto cut_events = race_cuts.update(dt, false);
  rally_post_tick(dt, cut_events);
  rally_pep_talk_tick();
}

void DriveSession::sync_h2h_screens(double dt) {
  if (app.race || app.headless || !festival_mgr) {
    h2h_screens.stop();
    h2h_was_active = false;
    return;
  }
  const freeroam::HeadToHeadHud hh = festival_mgr->hud(vehicle.time());
  // The challenge starts: the intro names the rival, the car and the reward.
  if (hh.active && !h2h_was_active) {
    h2h_name = hh.opponent_name, h2h_class = hh.opponent_class, h2h_pi = hh.opponent_pi;
    h2h_reward = hh.wager_credits;
    h2h_make.clear(), h2h_car.clear();
    for (const auto& c : festival_mgr->cars()) {
      if (!c.in_h2h) continue;
      if (c.spec.car_id > 0) {
        const CarListDetails d = car_list_details(c.spec.car_id);
        h2h_car = d.model;
        // "2012 Jaguar": the make after the year.
        const size_t sp = d.year_make.find(' ');
        h2h_make = sp == std::string::npos ? d.year_make : d.year_make.substr(sp + 1);
      }
      if (h2h_car.empty()) h2h_car = carview::car_name(c.spec.car_media);
      break;
    }
    h2h_screens.start({S::kEncounterIntro});
  }
  h2h_was_active = hh.active;
  // The finish: a win shows the banner and the cash; a loss nothing.
  if (hh.finish_banner && hh.finish_at > h2h_finish_seen) {
    h2h_finish_seen = hh.finish_at;
    race_flow::PostFacts f;
    f.kind = race_flow::Kind::kHeadToHead;
    f.won = hh.finish_won;
    f.credits = hh.finish_credits;
    auto steps = race_flow::post_steps(f);
    if (steps.size() > 1) {
      h2h_reward = hh.finish_credits;
      h2h_screens.start(std::move(steps));
    }
  }
  h2h_screens.update(dt);
  if (h2h_screens.screen() == S::kDone) h2h_screens.stop();
}

void DriveSession::sync_race_cutscene() {
  // The cutscene each step of the flow plays (game_festival_race_flow.xml,
  // game_race_flow_states.xml); steps without one keep the last.
  if (rally_champion_cut) return;  // the Rally's champion sequence owns the view
  const char* want = nullptr;
  switch (race_screens.screen()) {
    case S::kTitleCard: want = "prerace_intro"; break;
    case S::kGridMenu: want = "prerace_looping_01"; break;
    case S::kRivalIntro: want = "nemesis_show"; break;
    case S::kFinishBanner: want = "postrace_finishline"; break;
    case S::kCashReward: want = "postrace_transition_01"; break;
    case S::kPoints: want = "postrace_transition_02"; break;
    case S::kRewardCar: want = "postrace_rewardcar_transition_01"; break;
    default: break;
  }
  // START RACE: the count runs over prerace_countdown (its Follow cut hands
  // the view back to the driving camera).
  const bool pre = race_cut_name == "prerace_intro" || race_cut_name == "prerace_looping_01" ||
                   race_cut_name == "nemesis_show";
  if (!want && pre && !grid_menu_open && !race_screens.active() && !app.race->player_finished())
    want = "prerace_countdown";
  if (!want || race_cut_name == want) return;
  race_cut_name = want;
  const auto* cev = app.have_career_catalog ? app.career_catalog.find_event(app.race_event) : nullptr;
  if (!race_cuts.load_race(app.game_root_of(), cev ? cev->track_dev_name : std::string{})) return;
  // The route's frames the cams target (start_location_00, ...): headings
  // from their facings.
  std::vector<std::pair<std::string, cutscene::Frame>> nodes;
  for (const auto& t : app.race->route().transforms)
    nodes.push_back({t.name, {{t.x, t.y, t.z}, std::atan2(t.fx, t.fz) * 180.0 / 3.14159265358979323846}});
  race_cuts.set_scene_nodes(std::move(nodes));
  race_cuts.set_car_parts(car_parts(vehicle, host.showroom.bounds_min(), host.showroom.bounds_max()));
  if (!race_cuts.play_race(want)) race_cuts.stop();
}

bool DriveSession::race_cutscene_camera(drive::CameraView& out) const {
  if (!app.race || !race_cuts.active()) return false;
  const drive::Vec3 vp = vehicle.position();
  const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
  return race_cuts.camera({vp.x, vp.y, vp.z}, std::atan2(vf.x, vf.z), out);
}

void DriveSession::grid_menu_accept() {
  switch (kGridRows[size_t(std::clamp(grid_menu_selected, 0, int(std::size(kGridRows)) - 1))]) {
    case GridRow::kStartRace:
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      race_screens.next();
      if (race_screens.screen() == S::kCountdown || !race_screens.active()) {
        grid_menu_open = false;
        pre_race_blend_t = 0.0;
        race_screens.stop();
      }
      return;
    case GridRow::kDifficulty:
      // The original's difficulty screen (300_I_OPT_ASSISTS_DIFFICULTY):
      // A SAVE, B CANCEL back to the grid; its rows reach the race live.
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      open_options_page(carview::OptionsPage::kDifficulty);
      return;
    case GridRow::kOptions:
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      settings_page.open();
      options_page = carview::OptionsPage::kNone;  // the engine's page, not an option page
      return;
    case GridRow::kQuitRace:
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      begin_race_quit();
      return;
  }
}

void DriveSession::race_continue() {
  if (results_table) return;  // the table takes only B
  if (!race_screens.active()) return;
  switch (race_screens.screen()) {
    case S::kFinishBanner: {
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      // RACE-05: the original updates and saves the profile on the finish
      // banner's done exit, so Continue is what pays the race.
      commit_race_award();
      const RaceHud hi = app.race->hud_info();
      race_flow::PostFacts f;
      f.kind = race_kind(app);
      f.crossed_line = hi.place > 0 && hi.finish_time > 0;
      f.won = hi.place == 1;
      f.credits = hi.credits;
      f.career_xp_event = hi.wristband > 0 && f.kind != race_flow::Kind::kRivals;
      f.reward_car = !won_car_name.empty();
      f.event_car = !app.event_car_name.empty();
      race_screens.start(race_flow::post_steps(f));
      race_screens.next();  // past the banner
      results_page = 1;
      points_at = vehicle.time();
      // PrepareForRewardsFlow: the AI cars go and the car waits at the
      // post-race location (post_race_location_00).
      if (race_flow::hides_opponents(f.kind)) app.race->set_rivals_solo(true);
      // place, not reset: the vehicle's clock runs on (the notices and the
      // screens' timers read it), and the fall check starts from here.
      double at[4];
      if (app.race->post_race_spawn(at)) {
        double gy = at[1];
        if (host.ground) world::ground_height(*host.ground, at[0], at[2], at[1] + 4.0, gy);
        vehicle.place({at[0], gy + 0.05, at[2]}, at[3]);
        drawn_before = vehicle.pose();
        have_touch = true;
        last_touch_y = vehicle.position().y;
        camera_pull = CameraPull{};
        notice_until = 0;
      }
      return;
    }
    case S::kCashReward:
      if (!race_screens.ready()) return;
      [[fallthrough]];
    case S::kPoints:
      if (race_screens.screen() == S::kPoints && wristband_to_collect()) return;  // no A: PLEASE WAIT
      [[fallthrough]];
    case S::kRewardCar:
    case S::kTombola:
    case S::kRivalOffer:
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      race_screens.next();
      return;
    case S::kStayInCar:
      // YES: the event's car stays for free roam.
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      app.pre_loaner_car.clear();
      app.pre_loaner_garage_id = 0;
      race_screens.next();
      return;
    default: return;
  }
}

bool DriveSession::wristband_to_collect() const {
  // As the points page's band_up (drive_hud_sync).
  return app.have_career_catalog && !app.wristbands.levels.empty() &&
         app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband) >= 0;
}

bool DriveSession::popup_backdrop() const {
  return menu.popup_open() || race_screens.screen() == S::kStayInCar;
}

void DriveSession::race_back() {
  if (race_screens.screen() == S::kStayInCar) {
    app.audio_host.trigger_ui(audio::UiCue::kCancel);
    race_screens.next();
  }
}

void DriveSession::begin_race_quit() {
  // interim (SPEC_QUESTIONS Q172): QUIT RACE, from the grid or the pause
  // menu, fades to black as the race's other exits do before the load
  // (post_steps with quit: kBlackOut, kDone).
  grid_menu_open = false;
  if (app.headless) {
    // No screens headless (update_race_screens stops them): out at once.
    app.leave_race = true;
    running = false;
    return;
  }
  race_flow::PostFacts f;
  f.kind = race_kind(app);
  f.quit = true;
  race_screens.start(race_flow::post_steps(f));
  race_quitting = true;
}

void DriveSession::finish_race_screens() {
  // The flow stays on kDone (drawn black) until the session ends: stopped
  // here, the last frames before the load fell back to the engine's own
  // results page.
  results_page = 0;
  app.event_car_name.clear();
  apply_campaign_action(app.campaign.flow().on_post_race_done());
  if (!app.race->post_race_spawn(app.spawn)) {
    const drive::Vec3 at = vehicle.position();
    const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
    app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
  }
  app.have_spawn = true;
  app.leave_race = true;
  running = false;
}

}  // namespace carview
