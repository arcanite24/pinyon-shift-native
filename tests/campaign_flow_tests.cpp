#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "campaign_director.h"
#include "first_time_career_ui.h"
#include "campaign_ui.h"
#include "drive/save.h"
#include "fmv_player.h"
#include "race/campaign_flow.h"
#include "race/showcase.h"
#include "ui.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

void test_easing_and_transitions() {
  check(std::fabs(race::ease_out_cubic(0.0) - 0.0) < 1e-6, "ease_out_cubic(0) == 0");
  check(std::fabs(race::ease_out_cubic(1.0) - 1.0) < 1e-6, "ease_out_cubic(1) == 1");
  check(race::ease_out_cubic(0.5) > 0.75, "ease_out_cubic decelerates smoothly");
  check(std::fabs(race::ease_in_out_cubic(0.5) - 0.5) < 1e-6, "ease_in_out_cubic(0.5) == 0.5");
  check(race::ease_out_back(0.65) > 1.0, "ease_out_back overshoots cleanly before settling");

  race::TransitionState tr;
  tr.start(race::TransitionKind::kBrushWipe, 0.6, "HORIZON FESTIVAL");
  check(tr.active, "transition starts active");
  check(!tr.update(0.15), "midpoint not reached at 0.15s");
  check(tr.cover_alpha() > 0.3f && tr.cover_alpha() < 0.99f, "cover_alpha rises before midpoint");
  check(tr.update(0.20), "midpoint fires once crossing 0.30s");
  check(!tr.update(0.15), "midpoint does not fire twice");
  check(tr.cover_alpha() > 0.0f && tr.cover_alpha() < 1.0f, "cover_alpha falls after midpoint");
  tr.update(0.25);
  check(!tr.active && tr.cover_alpha() == 0.0f, "transition finishes cleanly");
}

void test_minute0_to_endgame_state_machine() {
  race::CampaignFlow flow;
  // 1. Start New Career (with movies enabled) -> Opening Cutscene.
  {
    auto act = flow.start_new_career(false, {-1360.0, 2.0, -210.0});
    check(flow.stage() == race::CampaignStage::kOpeningCutscene, "New Career enters OpeningCutscene");
    check(act.play_fmv && act.fmv_cutscene == race::CutsceneKind::kOpeningFmv, "requests FMV_01 opening");
    check(flow.is_onboarding() && flow.is_blocking_gameplay(), "opening cutscene blocks gameplay");
  }
  // 2. Opening FMV ends -> Viper Intro Drive with GPS & progressive HUD reveal.
  {
    auto act = flow.update(0.5, {0.0, 0.0}, false, false);
    check(flow.stage() == race::CampaignStage::kViperIntroDrive, "FMV finish enters ViperIntroDrive");
    check(act.swap_car && act.car_media == race::CampaignFlow::kViperMedia, "swaps to 2013 SRT Viper GTS");
    check(act.set_gps, "sets GPS to Horizon Festival entrance");
    check(act.lock_viper_radio, "locks Bass Arena 2-track opening playlist during Viper drive");
    check(race::CampaignFlow::kViperOpeningPlaylist.size() == 2, "Viper opening playlist has 2 tracks");
    check(flow.completed_cutscenes().count("fmv_01_opening") == 1, "records opening cutscene completed");
    check(flow.hud_reveal_alpha() < 0.05f, "HUD starts hidden at t=0 of Viper drive");
  }
  // 3. Drive along highway -> progressive HUD reveals + Scott Tyler & Alice Hart VO beats trigger.
  {
    flow.update(1.5, {1000.0, 500.0}, false, false);
    check(flow.hud_reveal_alpha() > 0.5f, "HUD reveals progressively during Viper drive");
    check(flow.dialogs().active(), "Scott Tyler opening radio beat triggers");
    check(flow.dialogs().current().speaker == race::CharacterId::kScottTyler, "first Viper beat is Scott Tyler");
    flow.update(0.5, {-200.0, 0.0}, false, false);  // within 2200 m -> triggers Alice Hart beat
    check(flow.heard_dialogs().count("m0_viper_beat1") == 1, "m0_viper_beat1 recorded in heard_dialogs");
    check(flow.heard_dialogs().count("m0_viper_beat2") == 1, "m0_viper_beat2 recorded in heard_dialogs");
  }
  // 4. Arrive at Festival gate -> Corrado VR6 qualification race.
  {
    auto act = flow.update(0.2, {-1340.0, -200.0}, false, false);
    check(flow.stage() == race::CampaignStage::kQualifyRace, "arrival triggers QualifyRace");
    check(act.swap_car && act.car_media == race::CampaignFlow::kCorradoMedia, "swaps to 1995 VW Corrado VR6");
    check(act.launch_event && act.route_id == 16, "launches FESTIVAL_SECOND / route 16");
  }
  // 5. Finish qualification race -> Festival Registration & Yellow Wristband + Corrado handoff.
  {
    auto act = flow.update(0.2, {-1360.0, -210.0}, false, true);
    check(flow.stage() == race::CampaignStage::kFestivalRegistration, "qualify finish enters FestivalRegistration");
    check(act.grant_starter && act.collect_yellow_wristband, "grants Corrado starter & Yellow Wristband");
    check(flow.dialogs().active(), "Alice/Dak/Holly orientation dialog queued");
  }
  // 6. Advance registration -> seamless Free Roam transition with tutorial Autoshow handoff & initial objective.
  {
    auto act = flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kFreeRoam, "registration advances to FreeRoam");
    check(act.intro_completed, "marks intro_completed");
    check(act.open_autoshow && act.grant_starter && act.collect_yellow_wristband,
          "registration triggers tutorial 2-car Autoshow handoff");
    auto obj = flow.next_objective(0, 0, 250);
    check(obj.valid && !obj.title.empty(), "FreeRoam has valid next campaign objective");
  }
  // 7. Wristband progression (Yellow -> Green -> ... -> Gold) & Boss / Showcase unlocks.
  {
    check(race::wristband_milestones().size() == 7, "all 7 wristbands defined");
    check(race::boss_rivals().size() == 7, "all 7 Star Showdown boss rivals defined");
    check(race::showcase_milestones().size() == 10, "all 10 Showcases defined");

    flow.on_wristband_points_changed(220, 0);
    check(flow.unlocked_bosses().count("ramona") == 1, "Ramona Cravache unlocks at 220 pts");
    flow.on_popularity_rank_changed(250, 175);
    check(flow.unlocked_showcases().count("PLANE_RACE_001") == 1, "P-51 Mustang Showcase unlocks");
    check(flow.unlocked_showcases().count("PLANE_RACE_004") == 1, "Hot-Air Balloon Showcase unlocks at rank 225");
    check(flow.unlocked_showcases().count("PLANE_RACE_002") == 1, "Helicopter Showcase unlocks at rank 175");

    // Collect Green Wristband -> Wristband Ceremony.
    flow.on_wristband_collected(1, "2012 Ford Mustang Boss 302", 10);
    check(flow.stage() == race::CampaignStage::kWristbandCeremony, "collecting wristband enters WristbandCeremony");
    check(flow.cinematic().colour_key == "Green", "ceremony uses Green wristband key");
    flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kFreeRoam, "ceremony returns to FreeRoam");
  }
  // 8. Barn Find discovery & Dak restoration handoff.
  {
    flow.on_barn_find(false, "barnfind_01", "");
    check(flow.stage() == race::CampaignStage::kFreeRoam, "barn rumour stays non-blocking in FreeRoam");
    flow.on_barn_find(true, "barnfind_01", "1956 Ford F-100");
    check(flow.stage() == race::CampaignStage::kBarnDiscovery, "barn discovery enters BarnDiscovery cinematic");
    flow.advance_or_skip();
    flow.on_barn_restored("barnfind_01", "1956 Ford F-100");
    check(flow.stage() == race::CampaignStage::kBarnRestored, "barn restored enters BarnRestored handoff");
    flow.advance_or_skip();
  }
  // 8b. Winning HEADLINE_EVENT unlocks Darius Flynt NEM_FINAL showdown (never skips straight to Credits!).
  {
    race::CareerEvent headline_ev;
    headline_ev.horizon_id = "HEADLINE_EVENT";
    headline_ev.style = 6;
    headline_ev.level = 6;
    flow.on_enter_event(headline_ev);
    check(flow.stage() == race::CampaignStage::kFinaleIntro && flow.is_blocking_gameplay(),
          "HEADLINE_EVENT enters blocking FinaleIntro before launch");
    flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kFreeRoam, "HEADLINE_EVENT intro exits cleanly for deferred launch");

    race::RaceAward haw;
    haw.base_credits = 50000;
    haw.credits = 50000;
    flow.on_race_finished(&headline_ev, haw, 1, 8, 15, 5, 250000, 300000, 10000, 6, "");
    check(!flow.finale_complete(), "HEADLINE_EVENT win does NOT prematurely mark finale_complete");
    check(flow.pending_darius_showdown(), "HEADLINE_EVENT win unlocks pending_darius_showdown");
    check(!flow.reward_tally().is_finale, "HEADLINE_EVENT tally does NOT skip to Credits");
    check(flow.reward_tally().next_unlock_headline.find("DARIUS FLYNT") != std::string::npos,
          "HEADLINE_EVENT tally announces Darius Flynt Showdown unlock");

    // RACE-01: on_race_finished runs after the finish banner, so the tally opens on
    // Stage 2 (Credits) -> Stage 3 (Wristband/Pop) -> FreeRoam!
    check(flow.stage() == race::CampaignStage::kRewardTally &&
              flow.reward_tally().stage == race::PostRaceStage::kCreditsEarned,
          "HEADLINE_EVENT finish opens the tally on the credits page");
    flow.advance_or_skip();  // finish Stage 2 animation
    flow.advance_or_skip();  // Stage 2 -> Stage 3 (Wristband & Popularity)
    flow.advance_or_skip();  // finish Stage 3 animation
    flow.advance_or_skip();  // -> FreeRoam (must NOT enter FinaleVictory/Credits yet!)
    check(flow.stage() == race::CampaignStage::kFreeRoam,
          "HEADLINE_EVENT win returns to FreeRoam so player can race Darius Flynt");
    const auto obj = flow.next_objective(10000, 6, 5);
    check(obj.valid && obj.title.find("DARIUS FLYNT") != std::string::npos,
          "next_objective routes directly to Darius Flynt showroom challenge");

    // Verify pending_darius_showdown persists across save/load round-trip.
    race::CampaignSaveState saved_mid;
    flow.save_to(saved_mid);
    race::CampaignFlow restored_mid;
    restored_mid.load_from(saved_mid);
    check(restored_mid.pending_darius_showdown() && !restored_mid.finale_complete(),
          "pending_darius_showdown persists cleanly across save/load");
  }
  // 9. Star Showdown & Darius Flynt Finale (NEM_FINAL) -> Stage 1 Results -> Stage 2 Credits -> Stage 3 Wristband -> FinaleVictory -> Credits -> FreeRoam!
  {
    race::CareerEvent finale_ev;
    finale_ev.horizon_id = "NEM_FINAL";
    finale_ev.style = 5;
    finale_ev.level = 6;
    flow.on_enter_event(finale_ev);
    check(flow.stage() == race::CampaignStage::kFinaleIntro && flow.is_blocking_gameplay(),
          "NEM_FINAL enters blocking FinaleIntro");
    flow.advance_or_skip();

    race::RaceAward aw;
    aw.base_credits = 50000;
    aw.difficulty_pct = 30;
    aw.difficulty_bonus = 15000;
    aw.credits = 65000;
    aw.wristband_points = 0;
    flow.on_race_finished(&finale_ev, aw, 1, 2, 5, 1, 300000, 365000, 10000, 6, "2010 Ferrari 599XX");
    // RACE-01: the finish banner (and its RACE RESULTS popup) came before this call;
    // the campaign's tally is only the credits and points pages after it.
    check(flow.stage() == race::CampaignStage::kRewardTally && flow.is_blocking_gameplay() &&
              flow.reward_tally().stage == race::PostRaceStage::kCreditsEarned,
          "leaving the finish banner opens the blocking tally on Stage 2 Credits Earned");
    check(flow.reward_tally().active, "RewardTally active");

    // Step Stage 2 partway and verify animated rolling credits.
    flow.update(1.0, {0.0, 0.0}, false, false);
    check(flow.reward_tally().displayed_credits() > 0 && flow.reward_tally().displayed_credits() < 65000,
          "credits counter rolls smoothly on Stage 2");

    // First skip completes Stage 2 animation; second skip advances to Stage 3 (Wristband & Popularity + Prize Car).
    flow.advance_or_skip();
    check(flow.reward_tally().stage_animation_done(), "first skip finishes Stage 2 credits animation");
    check(flow.reward_tally().displayed_credits() == 65000, "Stage 2 finishes at full credits");
    flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kRewardTally &&
              flow.reward_tally().stage == race::PostRaceStage::kWristbandAndPrize,
          "next continue advances to Stage 3 Wristband & Popularity + Prize Car");

    // Step Stage 3 partway and verify animated popularity climb.
    flow.update(0.75, {0.0, 0.0}, false, false);
    check(flow.reward_tally().displayed_pop_rank() < 5 && flow.reward_tally().displayed_pop_rank() >= 1,
          "popularity rank climbs smoothly on Stage 3");
    flow.advance_or_skip();
    check(flow.reward_tally().animation_done(), "skip completes Stage 3 animation");
    flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kFinaleVictory, "Finale win enters FinaleVictory");
    check(flow.finale_complete() && !flow.pending_darius_showdown(),
          "NEM_FINAL win marks finale_complete and clears pending_darius_showdown");
    check(flow.cinematic().subheadline.find("50,000 CR") != std::string::npos &&
              flow.cinematic().subheadline.find("5,000,000") == std::string::npos,
          "FinaleVictory displays authentic 50,000 CR reward (no fake 5,000,000 CR)");
    flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kCredits, "FinaleVictory transitions to Credits");
    flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kFreeRoam, "Credits returns cleanly to endless FreeRoam");
  }
}

void test_dialog_queue_ducking_and_waveforms() {
  race::DialogQueue q;
  std::set<std::string> heard;

  race::DialogCue c1;
  c1.id = "dlg_one";
  c1.speaker = race::CharacterId::kAliceHart;
  c1.vo_trigger = "Welcome";
  c1.subtitle = "Welcome to Colorado!";
  c1.duration_s = 3.0;
  c1.one_shot = true;

  check(q.enqueue(c1, &heard), "first enqueue succeeds");
  check(!q.enqueue(c1, &heard), "duplicate one-shot dialog is suppressed");
  check(q.active(), "queue is active");

  std::string vo, radio;
  check(q.consume_pending_audio(vo, radio) && vo == "Welcome", "consumes pending VO trigger once");
  check(!q.consume_pending_audio(vo, radio), "audio trigger does not refire");

  q.update(0.4);
  check(q.duck_factor() > 0.5f, "audio ducking engages quickly during speech");
  check(q.banner_slide() > 0.8f, "call-in banner slides in smoothly");
  const auto bars = q.waveform_bars(10);
  check(bars.size() == 10, "waveform returns requested bar count");
  bool varied = false;
  for (size_t i = 1; i < bars.size(); ++i)
    if (std::fabs(bars[i] - bars[0]) > 0.01f) varied = true;
  check(varied, "animated waveform bars vary across spectrum");

  q.update(3.5);
  check(!q.active(), "cue expires after duration");
  for (int i = 0; i < 10; ++i) q.update(0.15);
  check(q.duck_factor() < 0.05f, "audio ducking releases smoothly after speech");
}

void test_save_load_roundtrip() {
  carview::CampaignDirector dir;
  dir.configure({}, "EN", true);
  dir.flow().start_new_career(true);
  dir.flow().update(1.0, {1000.0, 500.0}, false, false);
  dir.flow().on_wristband_points_changed(750, 1);
  dir.flow().on_popularity_rank_changed(250, 100);

  drive::DriveSave sv;
  sv.car = "VW_Corrado_95";
  sv.position = {100.0, 20.0, -50.0};
  sv.heading = 1.25;
  dir.save_to_save(sv);

  const std::string text = drive::save_to_text(sv);
  drive::DriveSave sv2;
  std::string err;
  check(drive::save_from_text(text, sv2, &err), "DriveSave with campaign state round-trips");
  check(sv2.campaign_stage == "viper_drive", "campaign_stage preserved");
  check(!sv2.heard_dialogs.empty(), "heard_dialogs preserved");
  check(sv2.unlocked_bosses.size() >= 2, "unlocked_bosses preserved");
  check(sv2.unlocked_showcases.size() >= 4, "unlocked_showcases preserved");

  carview::CampaignDirector dir2;
  dir2.load_from_save(sv2);
  // An old save's onboarding stage resumes in the first-time career.
  check(dir2.flow().stage() == race::CampaignStage::kFreeRoam, "old onboarding stage leaves CampaignFlow in free roam");
  drive::DriveSave sv3;
  dir2.save_to_save(sv3);
  check(sv3.campaign_stage == "ftc:opening_cutscene", "old viper_drive save resumes at the opening cutscene");
  check(dir2.flow().unlocked_bosses().count("ali") == 1, "director restores unlocked bosses");
  check(dir2.flow().unlocked_showcases().count("PLANE_RACE_003") == 1, "director restores unlocked showcases");
}

void test_director_first_time_career() {
  carview::CampaignDirector dir;
  dir.configure({}, "EN", false);
  carview::FmvPlayer fmv;
  const auto act = dir.start_new_career(fmv, nullptr);
  check(dir.ftc_active() && dir.ftc().stage() == race::FtcStage::kControls, "new career opens the controls screen");
  check(!fmv.active() && !act.play_fmv, "no FMV at career start");
  check(!dir.flow().is_onboarding(), "no invented onboarding stage");
  check(dir.is_blocking_gameplay(), "controls screen blocks driving");
  for (int i = 0; i < 6; ++i) dir.update(0.1, {0, 0}, fmv, nullptr, false, false);
  dir.advance_or_skip(fmv, nullptr);
  const auto a2 = dir.update(0.1, {0, 0}, fmv, nullptr, false, false);
  check(dir.ftc().stage() == race::FtcStage::kOpeningCutscene, "confirm starts the opening");
  check(a2.swap_car && a2.car_media == "VIP_Viper_13" && a2.teleport, "the Viper placed for the opening");
  check(!fmv.active(), "the opening is in-engine, not FMV_01");
  drive::DriveSave sv;
  dir.save_to_save(sv);
  check(sv.campaign_stage == "ftc:opening_cutscene" && !sv.intro_completed, "first-time career stage saved");
  carview::CampaignDirector back;
  back.load_from_save(sv);
  drive::DriveSave sv2;
  back.save_to_save(sv2);
  check(sv2.campaign_stage == "ftc:opening_cutscene", "first-time career stage restored");
}

void test_director_cameras_cli_stages_and_ui_raster() {
  // Cubic Bezier endpoints check.
  const auto b0 = carview::eval_cubic_bezier({0, 0, 0}, {1, 2, 0}, {3, 2, 0}, {4, 0, 0}, 0.0);
  const auto b1 = carview::eval_cubic_bezier({0, 0, 0}, {1, 2, 0}, {3, 2, 0}, {4, 0, 0}, 1.0);
  check(std::fabs(b0[0] - 0.0) < 1e-6 && std::fabs(b1[0] - 4.0) < 1e-6, "cubic bezier endpoints exact");

  carview::CampaignDirector dir;
  dir.configure({}, "EN", false);
  carview::FmvPlayer fmv;

  const char* stages[] = {"intro", "viper_drive", "dialog", "registration", "wristband_yellow",
                          "wristband_gold", "reward_tally", "showcase", "boss_ramona", "boss_darius",
                          "finale_intro", "barn_find", "barn_restored", "finale", "credits"};
  for (const char* st : stages) {
    race::CampaignAction act;
    check(dir.apply_cli_stage(st, fmv, nullptr, act), "CLI stage accepted");
    carview::ui::List list;
    carview::campaign_ui::DrawContext ctx;
    ctx.screen_w = 640.0f;
    ctx.screen_h = 360.0f;
    if (fmv.active()) {
      fmv.update(1.0, false);
      fmv.draw(list, ctx.screen_w, ctx.screen_h, {}, true, 0.4f);
      fmv.close();
    } else if (dir.ftc_active()) {
      carview::ftc_ui::draw(list, dir.ftc(), ctx.screen_w, ctx.screen_h, 0, false);
      carview::ftc_ui::draw_overlay(list, dir.screen_overlay(), ctx.screen_w, ctx.screen_h);
    } else {
      carview::campaign_ui::draw(list, dir.flow(), ctx);
    }
    check(!list.empty(), "UI4 campaign draw produces visible items");

    if (dir.is_cinematic_camera_active()) {
      drive::CameraView cam{};
      check(dir.compute_camera({0.0, 10.0, 0.0}, 0.5, cam), "cinematic camera computes valid pose");
      check(std::isfinite(cam.eye[0]) && std::isfinite(cam.forward[0]) && cam.fov_degrees > 20.0,
            "cinematic camera pose is finite and valid");
    }
  }
}

void test_r2_5_campaign_parity() {
  // 1. Exact boss spelling ("HAILEY HARPER") & retail gamedb.slt wristband event counts (70 Festival events total).
  {
    const auto& prof = race::character_profile(race::CharacterId::kHaileyHarper);
    check(std::string(prof.display_name) == "HAILEY HARPER", "character profile uses HAILEY HARPER spelling");
    const auto* hailey = race::find_boss_by_key("hailey");
    const auto* hailey_legacy = race::find_boss_by_key("haley");
    check(hailey != nullptr && std::string(hailey->rival_name) == "HAILEY HARPER", "boss rival uses HAILEY HARPER");
    check(hailey_legacy == hailey, "legacy 'haley' lookup resolves cleanly to Hailey Harper");

    const auto& bands = race::wristband_milestones();
    const int expected_counts[7] = {3, 7, 8, 9, 12, 14, 16};
    check(bands.size() == 7, "7 wristband tiers defined");
    int total_wb_events = 0;
    for (size_t i = 0; i < bands.size() && i < 7; ++i) {
      check(bands[i].unlocked_events_hint == expected_counts[i], "retail gamedb.slt wristband event count matches");
      total_wb_events += bands[i].unlocked_events_hint;
    }
    check(total_wb_events == 69, "69 wristband-tiered Festival events (+1 qualifier = 70 total)");
    check(std::string(bands[5].announcement).find("Hailey Harper") != std::string::npos,
          "Purple wristband announcement spells Hailey Harper correctly");

    race::CampaignFlow reg_flow;
    race::CampaignAction act;
    check(reg_flow.jump_to_cli_stage("registration", act), "registration CLI stage works");
    check(reg_flow.cinematic().detail_line.find("3 YELLOW WRISTBAND EVENTS") != std::string::npos,
          "Festival registration cites 3 Yellow Wristband events (not fabricated 8)");
  }

  // 2. on_wristband_collected() must NOT prematurely unlock bosses; check_boss_unlocks() fires NemesisEventUnlocked1..6.
  {
    race::CampaignFlow flow;
    flow.skip_onboarding_all();
    flow.dialogs().clear();

    const int thresholds[6] = {220, 750, 1450, 2500, 4100, 6400};
    const char* keys[6] = {"ramona", "ali", "duke", "marko", "zaki", "hailey"};
    const char* vo_ids[6] = {"NemesisEventUnlocked1", "NemesisEventUnlocked2", "NemesisEventUnlocked3",
                             "NemesisEventUnlocked4", "NemesisEventUnlocked5", "NemesisEventUnlocked6"};

    for (int lvl = 0; lvl < 6; ++lvl) {
      flow.on_wristband_collected(lvl, "", 0);
      flow.advance_or_skip();
      flow.dialogs().clear();
      check(flow.unlocked_bosses().count(keys[lvl]) == 0,
            "on_wristband_collected does NOT prematurely insert boss into unlocked_bosses");

      // Below unlock_points -> still locked.
      flow.on_wristband_points_changed(thresholds[lvl] - 1, lvl);
      check(flow.unlocked_bosses().count(keys[lvl]) == 0, "boss stays locked below unlock_points");

      // Reaching unlock_points -> unlocks boss and queues NemesisEventUnlocked call-in!
      flow.on_wristband_points_changed(thresholds[lvl], lvl);
      check(flow.unlocked_bosses().count(keys[lvl]) == 1, "boss unlocks at exact unlock_points");
      std::string vo, radio;
      check(flow.dialogs().consume_pending_audio(vo, radio) && vo == vo_ids[lvl],
            "NemesisEventUnlocked call-in fires when boss unlocks");
    }

    // Collecting Gold wristband (level 6) and reaching 10,000 pts must NOT prematurely unlock Darius before HEADLINE_EVENT win!
    flow.on_wristband_collected(6, "", 0);
    flow.advance_or_skip();
    flow.on_wristband_points_changed(10000, 6);
    check(flow.unlocked_bosses().count("darius") == 0, "Darius does NOT unlock before winning HEADLINE_EVENT");
    check(!flow.pending_darius_showdown(), "pending_darius_showdown stays false before HEADLINE_EVENT win");

    // Legacy save with 'darius' in unlocked_bosses but WITHOUT 'headline_event_won' must NOT activate Darius early!
    race::CampaignSaveState legacy;
    legacy.campaign_stage = "freeroam";
    legacy.unlocked_bosses = {"ramona", "haley", "darius"};
    legacy.finale_complete = false;
    race::CampaignFlow loaded;
    loaded.load_from(legacy);
    check(!loaded.pending_darius_showdown(), "load_from does NOT set pending_darius_showdown without headline_event_won");
    check(loaded.unlocked_bosses().count("hailey") == 1, "load_from normalizes legacy 'haley' boss key to 'hailey'");

    // With 'headline_event_won' present, load_from sets pending_darius_showdown cleanly.
    legacy.completed_cutscenes = {"headline_event_won"};
    loaded.load_from(legacy);
    check(loaded.pending_darius_showdown(), "load_from sets pending_darius_showdown when headline_event_won is recorded");
  }

  // 3. Pre-race grid cinematic blocks gameplay so event launch waits until flyby finishes/skips cleanly.
  {
    race::CampaignFlow flow;
    flow.skip_onboarding_all();
    race::CareerEvent fest_ev;
    fest_ev.horizon_id = "FR02";
    fest_ev.style = 1;
    fest_ev.level = 0;
    flow.on_enter_event(fest_ev);
    check(flow.stage() == race::CampaignStage::kPreRaceGrid, "standard event enters kPreRaceGrid");
    check(flow.is_cinematic_active(), "kPreRaceGrid has cinematic active");
    check(flow.is_blocking_gameplay(), "kPreRaceGrid blocks gameplay so countdown does not start prematurely");
    flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kFreeRoam && !flow.is_blocking_gameplay() && !flow.is_cinematic_active(),
          "skipping kPreRaceGrid exits cleanly to FreeRoam with no camera bleed");
  }
}

void test_pt_e_campaign_and_post_race_parity() {
  // 1. PT-39 & PT-42: Localized event title resolution NEVER displays raw internal DB keys.
  {
    check(race::resolve_event_display_title("PLANE_RACE_001") == "MUSTANG VS MUSTANG",
          "PLANE_RACE_001 resolves to MUSTANG VS MUSTANG (never raw key)");
    check(race::resolve_event_display_title("FESTIVAL_SECOND") == "HORIZON QUALIFIER",
          "FESTIVAL_SECOND resolves to HORIZON QUALIFIER");
    check(race::resolve_event_display_title("FR05") == "FESTIVAL RACE 05",
          "FR05 resolves to FESTIVAL RACE 05");
    check(race::resolve_event_display_title("SR03") == "STREET RACE 03",
          "SR03 resolves to STREET RACE 03");
    check(race::resolve_event_display_title("RED_ROCK_RUN_001") == "RED ROCK RUN",
          "snake_case fallback prettifies cleanly");

    race::set_campaign_text_resolver([](std::string_view ref) -> std::string {
      if (ref == "_&123456") return "Mustang vs. Mustang";
      return std::string(ref);
    });
    race::CareerEvent sc_ev;
    sc_ev.horizon_id = "PLANE_RACE_001";
    sc_ev.name_ref = "_&123456";
    sc_ev.style = 3;
    check(race::resolve_event_display_title(&sc_ev) == "Mustang vs. Mustang",
          "localized stringtable name_ref takes priority over fallback");
    race::set_campaign_text_resolver({});

    race::CampaignFlow flow;
    flow.skip_onboarding_all();
    race::CareerEvent raw_fr;
    raw_fr.horizon_id = "FR05";
    raw_fr.style = 1;
    flow.on_enter_event(raw_fr);
    check(flow.cinematic().headline == "FESTIVAL RACE 05",
          "Pre-race grid cinematic never shows raw FR05 internal key");
    flow.advance_or_skip();

    // Post-race sequential flow for PLANE_RACE_001 Showcase with prize car.
    race::CareerEvent plane_ev;
    plane_ev.horizon_id = "PLANE_RACE_001";
    plane_ev.style = 3;
    race::RaceAward aw;
    aw.base_credits = 5000;
    aw.mileage_bonus = 330;
    aw.difficulty_pct = 15;
    aw.difficulty_bonus = 800;
    aw.skills_bonus = 2700;
    aw.credits = 8830;
    aw.wristband_points = 0;
    flow.on_race_finished(&plane_ev, aw, 1, 2, 250, 247, 22500, 31330, 30, 0, "1970 Ford Mustang Boss 429");
    check(flow.reward_tally().event_title == "MUSTANG VS MUSTANG",
          "Showcase reward tally resolves PLANE_RACE_001 to MUSTANG VS MUSTANG");
    check(flow.reward_tally().event_title.find("PLANE_RACE") == std::string::npos,
          "Raw PLANE_RACE_001 never leaks into reward_tally.event_title");
    check(flow.reward_tally().results_rows.size() == 2,
          "Showcase Stage 1 populates 2-entry podium standings (YOU vs P-51 MUSTANG)");

    carview::campaign_ui::DrawContext ctx;
    ctx.screen_w = 1280.0f;
    ctx.screen_h = 720.0f;

    // RACE-01: the finish banner and its RACE RESULTS popup came first; the tally
    // opens on Stage 2 (CREDITS EARNED).
    check(flow.stage() == race::CampaignStage::kRewardTally &&
              flow.reward_tally().stage == race::PostRaceStage::kCreditsEarned,
          "the tally opens on Stage 2 CREDITS EARNED");
    flow.update(1.5, {0.0, 0.0}, false, false);
    carview::ui::List l2;
    carview::campaign_ui::draw(l2, flow, ctx);
    check(l2.items().size() > 25, "Stage 2 CREDITS EARNED renders rolling breakdown & balance");

    // Advance -> Stage 3 (WRISTBAND & POPULARITY + PRIZE CAR AWARDED).
    flow.advance_or_skip();  // snap Stage 2
    flow.advance_or_skip();  // enter Stage 3
    check(flow.reward_tally().stage == race::PostRaceStage::kWristbandAndPrize,
          "Stage 2 advances to Stage 3 WRISTBAND & POPULARITY + PRIZE CAR");
    flow.update(1.2, {0.0, 0.0}, false, false);
    carview::ui::List l3;
    carview::campaign_ui::draw(l3, flow, ctx);
    check(l3.items().size() > 25, "Stage 3 renders Wristband, Popularity & Prize Car Awarded card");

    flow.advance_or_skip();  // snap Stage 3
    flow.advance_or_skip();  // -> kShowcaseOutro
    check(flow.stage() == race::CampaignStage::kShowcaseOutro,
          "Winning Showcase exits Stage 3 cleanly into ShowcaseOutro");
  }

  // 2. PT-41: Starter car onboarding after qualification race (First Festival Car / Welcome to Autoshow).
  {
    race::CampaignFlow flow;
    flow.start_new_career(true);
    flow.advance_or_skip();  // Viper -> Qualify
    check(flow.stage() == race::CampaignStage::kQualifyRace, "onboarding enters QualifyRace");
    flow.update(0.2, {-1360.0, -210.0}, false, true);  // Finish Corrado qualification race
    check(flow.stage() == race::CampaignStage::kFestivalRegistration,
          "finishing qualification race enters kFestivalRegistration");
    check(flow.is_blocking_gameplay(),
          "kFestivalRegistration blocks gameplay so A/Enter advances cleanly without premature Autoshow");
    check(flow.cinematic().eyebrow.find("FIRST FESTIVAL CAR") != std::string::npos,
          "FestivalRegistration displays First Festival Car onboarding framing");
    check(flow.cinematic().subheadline.find("CORRADO") != std::string::npos &&
              flow.cinematic().subheadline.find("15,000 CR") != std::string::npos,
          "FestivalRegistration highlights starter VW Corrado VR6 and 15,000 Cr bonus");

    carview::ui::List lreg;
    carview::campaign_ui::DrawContext ctx;
    ctx.screen_w = 1280.0f;
    ctx.screen_h = 720.0f;
    carview::campaign_ui::draw(lreg, flow, ctx);
    check(lreg.items().size() > 30, "FestivalRegistration renders Welcome to Autoshow & Yellow Wristband card");

    auto act = flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kFreeRoam && act.open_autoshow && act.grant_starter &&
              act.collect_yellow_wristband,
          "advancing registration hands off to curated starter Autoshow + Yellow Wristband");
    check(flow.starter_autoshow_onboarding(),
          "starter_autoshow_onboarding flag armed for curated Autoshow launch");
    check(flow.consume_starter_autoshow_onboarding() && !flow.starter_autoshow_onboarding(),
          "consume_starter_autoshow_onboarding returns true once then clears");
    const auto obj = flow.next_objective(30, 0, 250);
    check(obj.valid && obj.title.find("GREEN WRISTBAND") != std::string::npos,
          "post-onboarding Free Roam activates Green Wristband objective even with 30 qualifier pts");
  }
}

void test_r3_e1_campaign_parity() {
  // 1. R3-CAM-02: All 7 Airborne Showcases match showcase.cpp 1:1 with zero invented "STUNT JET".
  {
    int airborne_count = 0;
    for (const auto& sc : race::showcase_milestones()) {
      auto contains_jet = [](const char* s) {
        if (!s) return false;
        std::string u(s);
        for (char& c : u) c = char(std::toupper(static_cast<unsigned char>(c)));
        return u.find("STUNT JET") != std::string::npos || u.find("JET FIGHTER") != std::string::npos;
      };
      check(!contains_jet(sc.display_name) && !contains_jet(sc.opponent_label) && !contains_jet(sc.intro_line) &&
                !contains_jet(sc.victory_line),
            "Showcase catalog contains zero invented Stunt Jet copy");
      if (!sc.airborne) continue;
      ++airborne_count;
      const race::ShowcaseSpec spec = race::showcase_spec_for(sc.horizon_id);
      check(spec.event_id == sc.event_id, "Airborne Showcase event_id matches showcase.cpp 1:1");
      check(std::string(sc.opponent_label) == spec.display_name,
            "Airborne Showcase opponent_label matches showcase.cpp aircraft display_name 1:1");
    }
    check(airborne_count == 7, "all 7 Airborne Showcases verified against showcase.cpp");
    const auto* sc4 = race::find_showcase_by_event("PLANE_RACE_004");
    const auto* sc2 = race::find_showcase_by_event("PLANE_RACE_002");
    const auto* sc3 = race::find_showcase_by_event("PLANE_RACE_003");
    const auto* sc6 = race::find_showcase_by_event("PLANE_RACE_006");
    const auto* sc5 = race::find_showcase_by_event("PLANE_RACE_005");
    check(sc4 && std::string(sc4->opponent_label) == "HOT-AIR BALLOON", "PLANE_RACE_004 is Hot-Air Balloon");
    check(sc2 && std::string(sc2->opponent_label) == "HELICOPTER", "PLANE_RACE_002 is Helicopter");
    check(sc3 && std::string(sc3->opponent_label) == "BIPLANE", "PLANE_RACE_003 is Biplane");
    check(sc6 && std::string(sc6->opponent_label) == "P-51 MUSTANG", "PLANE_RACE_006 is P-51 Mustang");
    check(sc5 && std::string(sc5->opponent_label) == "HELICOPTER", "PLANE_RACE_005 is Helicopter");
  }

  // 2. R3-CAM-03: Real RaceFinishStanding entries preserved verbatim without fabricating bosses or +0.92s gaps.
  {
    race::CampaignFlow flow;
    flow.skip_onboarding_all();
    race::CareerEvent ev;
    ev.horizon_id = "FR02";
    ev.style = 1;
    race::RaceAward aw;
    aw.base_credits = 8000;
    aw.credits = 9200;
    aw.wristband_points = 100;

    std::vector<race::RaceFinishStanding> standings = {
        {1, "Elena Vance", "2002 Nissan Skyline GT-R", "A", 612, 118.42, 0.0, 58.91, false, false},
        {2, "PlayerOne", "1995 Volkswagen Corrado VR6", "D", 321, 120.05, 1.63, 59.40, true, false},
        {3, "Marcus Cole", "2008 Subaru Impreza WRX STI", "B", 498, 122.77, 4.35, 60.88, false, false},
    };
    flow.on_race_finished(&ev, aw, 2, 3, 250, 244, 15000, 24200, 100, 0, "", nullptr, standings);
    const auto& rows = flow.reward_tally().results_rows;
    check(rows.size() == 3, "preserves exact 3 real standings rows");
    check(rows[0].driver == "Elena Vance" && rows[0].car == "2002 Nissan Skyline GT-R" && rows[0].car_class == "A" &&
              rows[0].pi == 612 && std::fabs(rows[0].time_s - 118.42) < 1e-4 &&
              std::fabs(rows[0].best_lap_s - 58.91) < 1e-4,
          "1st place real driver/car/class/PI/time/best_lap preserved");
    check(rows[1].player && rows[1].driver == "PlayerOne" && std::fabs(rows[1].gap_s - 1.63) < 1e-4,
          "player row preserves real driver name and exact gap (not fabricated +0.92s)");
    check(rows[2].driver == "Marcus Cole" && std::fabs(rows[2].gap_s - 4.35) < 1e-4,
          "3rd place real driver and gap preserved");
    for (const auto& r : rows) {
      check(r.driver.find("RAMONA") == std::string::npos, "non-boss race never fabricates Ramona Cravache");
    }
  }

  // 3. R3-CAM-08: Corrado Qualification Race (FESTIVAL_SECOND) enforces top-3 podium cutoff with retry on fail.
  {
    race::CampaignFlow flow;
    flow.start_new_career(true);
    flow.advance_or_skip();  // Viper -> QualifyRace
    check(flow.stage() == race::CampaignStage::kQualifyRace, "starts in QualifyRace");

    race::CareerEvent qev;
    qev.horizon_id = "FESTIVAL_SECOND";
    qev.style = 1;
    race::RaceAward aw;
    aw.credits = 500;

    // 8th place finish -> MUST NOT advance past kQualifyRace, sets retry_required!
    flow.on_race_finished(&qev, aw, 8, 8, 250, 250, 0, 500, 0, -1, "");
    check(flow.stage() == race::CampaignStage::kQualifyRace,
          "8th place in FESTIVAL_SECOND does NOT advance past kQualifyRace");
    check(flow.qualify_retry_pending(), "8th place arms qualify_retry_pending");
    check(!flow.reward_tally().qualified && flow.reward_tally().retry_required,
          "8th place marks tally qualified=false, retry_required=true");
    check(flow.is_blocking_gameplay() && flow.is_cinematic_active(),
          "qualification failure retry screen blocks gameplay cleanly");

    // Stepping update with race_finished=true MUST NOT advance to kFestivalRegistration while retry is pending!
    flow.update(0.5, {-1360.0, -210.0}, false, true);
    check(flow.stage() == race::CampaignStage::kQualifyRace,
          "update(race_finished=true) does NOT bypass failed qualification");

    // Pressing Continue (advance_or_skip) relaunches FESTIVAL_SECOND!
    const auto retry_act = flow.advance_or_skip();
    check(flow.stage() == race::CampaignStage::kQualifyRace && !flow.qualify_retry_pending(),
          "advance_or_skip clears retry pending and stays in kQualifyRace");
    check(retry_act.launch_event && retry_act.event_id == "FESTIVAL_SECOND" && retry_act.route_id == 16,
          "retry action relaunches FESTIVAL_SECOND / route 16");

    // 4th place still fails cutoff (top 3 required).
    flow.on_race_finished(&qev, aw, 4, 8, 250, 250, 0, 500, 0, -1, "");
    check(flow.stage() == race::CampaignStage::kQualifyRace && flow.qualify_retry_pending(),
          "4th place fails top-3 qualification cutoff");
    flow.advance_or_skip();  // retry again

    // 3rd place (podium) passes cutoff -> advances to kFestivalRegistration!
    const auto pass_act = flow.on_race_finished(&qev, aw, 3, 8, 250, 249, 0, 3000, 30, -1, "");
    check(flow.stage() == race::CampaignStage::kFestivalRegistration,
          "3rd place qualifies and advances to kFestivalRegistration");
    check(!flow.qualify_retry_pending() && flow.reward_tally().qualified && !flow.reward_tally().retry_required,
          "qualifying finish clears retry state");
    check(pass_act.grant_starter && pass_act.collect_yellow_wristband,
          "qualifying finish triggers starter car & Yellow Wristband registration");
  }

  // 4. R3-CAM-15: Viper opening drive Festival-bound AI convoy pass tracking & countdown.
  {
    race::CampaignFlow flow;
    flow.start_new_career(true);
    check(flow.stage() == race::CampaignStage::kViperIntroDrive, "Viper drive stage active");
    check(flow.viper_convoy_passed() == 0 && flow.viper_convoy_remaining() == race::CampaignFlow::kViperConvoyTarget,
          "convoy counter starts at 0 / target");
    const auto obj0 = flow.next_objective(0, -1, 250);
    check(obj0.title == "DRIVE TO THE HORIZON FESTIVAL",
          "Viper intro objective stays 1:1 DRIVE TO THE HORIZON FESTIVAL");

    check(flow.on_viper_convoy_pass(1), "first convoy pass recorded");
    check(flow.viper_convoy_passed() == 1 && flow.viper_convoy_remaining() == 4, "convoy counter advances to 1/5");
    check(flow.heard_dialogs().count("m0_viper_convoy_first") == 1, "first convoy pass queues VO cue");

    check(flow.on_viper_convoy_pass(3), "passing 3 more reaches 4/5");
    check(flow.viper_convoy_remaining() == 1, "1 driver remaining");
    check(flow.heard_dialogs().count("m0_viper_convoy_last") == 1, "last driver remaining queues VO cue");

    check(flow.on_viper_convoy_pass(1), "final convoy pass clears target");
    check(flow.viper_convoy_passed() == 5 && flow.viper_convoy_remaining() == 0, "convoy counter caps at 5/5");
    check(flow.heard_dialogs().count("m0_viper_convoy_clear") == 1, "convoy clear queues VO cue");
    check(!flow.on_viper_convoy_pass(1), "extra passes beyond target ignored");
    const auto obj_done = flow.next_objective(0, -1, 250);
    check(obj_done.title == "DRIVE TO THE HORIZON FESTIVAL",
          "Viper intro objective remains DRIVE TO THE HORIZON FESTIVAL after ambient convoy passes");
  }
}

}  // namespace

int main() {
  test_easing_and_transitions();
  test_minute0_to_endgame_state_machine();
  test_dialog_queue_ducking_and_waveforms();
  test_save_load_roundtrip();
  test_director_first_time_career();
  test_director_cameras_cli_stages_and_ui_raster();
  test_r2_5_campaign_parity();
  test_pt_e_campaign_and_post_race_parity();
  test_r3_e1_campaign_parity();
  if (g_failures > 0) {
    std::fprintf(stderr, "campaign_flow_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("campaign_flow_tests: all checks passed\n");
  return 0;
}
