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
                                           std::vector<std::pair<std::string, std::string>>* cars) {
  int64_t cid = 0;
  int clid = 2;
  current_car_info(cid, clid);
  const auto names = app.class_name_list();
  const auto elig = app.career_catalog.check_eligibility(ev, cid, clid, names, slice12.trainer.effective_any_car());
  // Car select lists the garage's eligible cars (ShowRegisterEventCarScreen).
  std::vector<std::pair<std::string, std::string>> eligible_cars;
  if (!elig.eligible && app.have_garage) {
    for (const auto& gc : app.career_garage.cars) {
      const int gcls = app.class_bands.empty() ? gc.class_id : garage::class_of(gc.performance_index, app.class_bands);
      if (!app.career_catalog.check_eligibility(ev, gc.car_id, gcls, names).eligible) continue;
      if (auto it = app.baked_ids.find(gc.car_id); it != app.baked_ids.end())
        eligible_cars.push_back({carview::car_name(it->second), it->second});
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
      event_car_media = event_cars[size_t(std::clamp(event_car_selected, 0, int(event_cars.size()) - 1))].second;
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
      if (ev) try_enter_event(*ev, car);
    }
  }
  event_summary_hud.screen = entry_screens.screen();
  event_summary_hud.t = vehicle.time() - summary_opened_at;
  event_summary_hud.restrictions_view = summary_restrictions;
  event_summary_hud.cars.clear();
  for (const auto& c : event_cars) event_summary_hud.cars.push_back(c.first);
  event_summary_hud.car_selected = event_car_selected;
  event_summary_hud.fade =
      entry_screens.screen() == S::kEntryFade ? std::clamp(entry_screens.elapsed() / race_flow::timing::kEntryFade, 0.0, 1.0)
                                              : 0.0;

  // base_festival_race_flow.xml speaks (radiodj.post_race) before the
  // results, then nothing over the rewards: the campaign's post-race call-in
  // waits from the finish until free roam.
  app.campaign.flow().dialogs().set_held(app.race && !app.headless && app.race->player_finished());
  if (!app.race || app.headless) {
    race_screens.stop();
    // Headless runs have no screens to step through: the campaign's outro
    // (finale, boss, showcase) follows the finish at once.
    if (app.race && app.headless && app.race->player_finished())
      apply_campaign_action(app.campaign.flow().on_post_race_done());
    return;
  }
  race_screens.update(dt);
  // The pre-race: the count starts when the flow reaches it.
  if (grid_menu_open && race_screens.screen() == S::kCountdown) {
    grid_menu_open = false;
    pre_race_blend_t = 0.0;
    race_screens.stop();
  }
  // The finish: the banner when the player crosses the line.
  if (!grid_menu_open && !race_screens.active() && results_page == 0 && app.race->player_finished())
    race_screens.start({S::kFinishBanner});
  if (race_screens.screen() == S::kDone) finish_race_screens();
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
      // interim (SPEC_QUESTIONS Q97): the original opens its difficulty
      // screen; the engine steps the difficulty here.
      cycle_difficulty(+1);
      return;
    case GridRow::kOptions:
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      settings_page.open();
      options_page = carview::OptionsPage::kNone;  // the engine's page, not an option page
      return;
    case GridRow::kQuitRace:
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
      grid_menu_open = false;
      race_screens.stop();
      app.leave_race = true;
      running = false;
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

void DriveSession::race_back() {
  if (race_screens.screen() == S::kStayInCar) {
    app.audio_host.trigger_ui(audio::UiCue::kCancel);
    race_screens.next();
  }
}

void DriveSession::finish_race_screens() {
  race_screens.stop();
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
