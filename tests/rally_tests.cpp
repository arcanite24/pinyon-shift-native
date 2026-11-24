// Unit & corpus tests for Horizon Rally Expansion (Slice 8):
// - 4-stage cumulative standings, stage/overall ranking, Service Area progression
// - Rally XP (RallyScoring), RallyWristbandLevels, credits payout, DriveSave round-trip
// - Restriction bit 0x80 (0000000084) + Rally Kit inspection
// - Co-Driver RallyCall gate crossing order, rewind, icon classification & Bus::kVo audio
// - Real v4 + Rally DLC corpus verification when SHIFT_STATE_ROOT is set.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/codriver_vo.h"
#include "data/event_restrictions.h"
#include "drive/save.h"
#include "garage/rally_kit.h"
#include "race/rally_championship.h"
#include "race/rally_codriver.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

race::RallyCatalog make_synthetic_catalog() {
  race::RallyCatalog cat;
  cat.loaded = true;
  cat.scoring.assign(51, 0);
  cat.scoring[1] = 60;
  cat.scoring[2] = 57;
  cat.scoring[3] = 55;
  for (int p = 4; p <= 50; ++p) cat.scoring[size_t(p)] = std::max(1, 58 - p);

  cat.wristbands = {
      {0, 0, "Horizon Rally Yellow", "Yellow"},
      {1, 310, "Horizon Rally Champion", "Green"},
  };

  cat.field.skill_variance = 0.05;
  cat.field.global = {{{0.70, 0.85}, {0.75, 0.90}, {0.82, 0.95}, {0.88, 1.00}}};
  for (int i = 1; i <= 49; ++i) {
    cat.field.drivers.push_back({100 + i, double(i) / 50.0});
    cat.ai_names[100 + i] = "AI Driver " + std::to_string(i);
  }

  cat.heats.event_id = 279;
  cat.heats.route_id = 24;
  cat.heats.player_car_id = 1626;
  cat.heats.cash_prize = 45000;
  cat.heats.track.track_id = 1137;
  cat.heats.track.length = 4548.0;
  cat.heats.track.finish_line = 0.9821;
  cat.heats.track.best = {17.0, 17.0, 17.0, 17.0, 18.1};

  race::RallyChampionshipDef ch1;
  ch1.series_id = 1;
  ch1.event_id = 275;
  ch1.horizon_id = "MS_RALLY_05";
  ch1.name = "Beaumont Rally";
  ch1.target_class = 4;
  ch1.cash_prize = 10000;
  ch1.unlock_points_req = 0;
  ch1.unlocks_event_id = 274;
  for (int s = 1; s <= 4; ++s) {
    race::RallyStageInfo st;
    st.event_id = 258 + s;
    st.stage_index = s;
    st.track_id = 1120 + s;
    st.route_id = 6 + s;
    st.horizon_id = "RALLY_5" + std::string(1, char('A' + s - 1));
    st.name = "Beaumont Stage " + std::to_string(s);
    st.track.track_id = st.track_id;
    st.track.route_id = st.route_id;
    st.track.length = 4000.0;
    st.track.finish_line = 0.9821;
    st.track.splits = {0.20, 0.40, 0.60, 0.80};
    st.track.best = {20.0, 20.0, 20.0, 20.0, 20.0};
    ch1.stages.push_back(st);
  }
  cat.championships.push_back(ch1);

  race::RallyChampionshipDef ch2 = ch1;
  ch2.series_id = 2;
  ch2.event_id = 274;
  ch2.horizon_id = "MS_RALLY_04";
  ch2.name = "Montano Plains Rally";
  ch2.target_class = 5;
  ch2.cash_prize = 15000;
  ch2.unlocks_event_id = 272;
  cat.championships.push_back(ch2);

  race::RallyChampionshipDef ch_final = ch1;
  ch_final.series_id = 7;
  ch_final.event_id = 272;
  ch_final.horizon_id = "MS_RALLY_02";
  ch_final.name = "Clear Springs Rally";
  ch_final.target_class = 6;
  ch_final.cash_prize = 50000;
  ch_final.unlock_points_req = 310;
  ch_final.unlocks_event_id = 0;
  cat.championships.push_back(ch_final);

  return cat;
}

void test_championship_standings_and_rewards() {
  const auto cat = make_synthetic_catalog();
  drive::DriveSave save;
  save.car = "SUB_WRXSTi_05";
  save.position = {0, 0, 0};
  save.heading = 0;

  // Before Heats: Beaumont is locked.
  CHECK(!race::is_rally_heats_completed(save));
  CHECK(!cat.is_championship_unlocked(275, save.rally_xp, false, save.event_unlocks, save.event_results));

  // Run RALLY_HEATS intro.
  race::RallyChampionshipSession heats;
  CHECK(heats.start_heats(cat, 1, 42, "PLAYER"));
  CHECK(heats.total_stages() == 1);
  std::vector<double> ai_heats(49, 120.0);
  ai_heats[0] = 110.0;  // one AI faster -> player is 2nd
  const auto hfin = heats.record_stage_finish(cat, 115.0, ai_heats);
  CHECK(hfin.championship_complete);
  CHECK(hfin.stage_place == 2 && hfin.overall_place == 2);
  const auto hrew = heats.finalize(cat, save, 15, 0);
  CHECK(hrew.is_heats && hrew.rally_xp_earned == 0 && hrew.total_credits > 0);
  CHECK(race::is_rally_heats_completed(save));
  CHECK(cat.is_championship_unlocked(275, save.rally_xp, true, save.event_unlocks, save.event_results));
  CHECK(!cat.is_championship_unlocked(274, save.rally_xp, true, save.event_unlocks, save.event_results));

  // Run full 4-stage Beaumont championship with explicit cumulative time verification.
  race::RallyChampionshipSession sess;
  CHECK(sess.start_championship(cat, 275, 1, 77, "PLAYER"));
  CHECK(sess.total_stages() == 4);

  // Stage 1: AI#0 = 100s, Player = 102s (+2s, 2nd), rest = 110s.
  std::vector<double> st1(49, 110.0);
  st1[0] = 100.0;
  auto f1 = sess.record_stage_finish(cat, 102.0, st1);
  CHECK(!f1.championship_complete && sess.in_service_area());
  CHECK(f1.stage_place == 2 && f1.overall_place == 2 && near(f1.overall_gap_to_leader, 2.0));
  CHECK(sess.advance_from_service_area() && sess.current_stage_index() == 1);

  // Stage 2: AI#0 = 105s (cum 205), Player = 101s (cum 203 -> overtakes to 1st overall!), rest = 110s.
  std::vector<double> st2(49, 110.0);
  st2[0] = 105.0;
  auto f2 = sess.record_stage_finish(cat, 101.0, st2);
  CHECK(!f2.championship_complete && sess.in_service_area());
  CHECK(f2.stage_place == 1 && f2.overall_place == 1 && near(f2.player_total_time, 203.0));
  CHECK(sess.advance_from_service_area() && sess.current_stage_index() == 2);

  // Stage 3 & Stage 4: Player wins overall with 403.0s total.
  std::vector<double> st3(49, 105.0);
  auto f3 = sess.record_stage_finish(cat, 100.0, st3);
  CHECK(!f3.championship_complete && f3.overall_place == 1);
  CHECK(sess.advance_from_service_area() && sess.current_stage_index() == 3);

  std::vector<double> st4(49, 104.0);
  auto f4 = sess.record_stage_finish(cat, 100.0, st4);
  CHECK(f4.championship_complete && !sess.in_service_area());
  CHECK(f4.overall_place == 1 && near(f4.player_total_time, 403.0));

  const auto rew = sess.finalize(cat, save, 15, 20);
  CHECK(!rew.is_heats && rew.final_place == 1);
  CHECK(rew.rally_xp_earned == 60 && save.rally_xp == 60);
  CHECK(save.num_rally_victories == 1 && save.num_rallies == 2 && save.num_rally_stages == 5);
  CHECK(rew.base_credits > 0 && rew.total_credits > rew.base_credits);
  CHECK(cat.is_championship_unlocked(274, save.rally_xp, true, save.event_unlocks, save.event_results));

  // Clear Springs (272) stays locked until RallyXP >= 310 (Green Rally Champion level).
  CHECK(!cat.is_championship_unlocked(272, 250, true, {272}, save.event_results));
  CHECK(cat.is_championship_unlocked(272, 310, true, {272}, save.event_results));
  CHECK(cat.wristband_for_xp(309) == 0 && cat.wristband_for_xp(310) == 1);

  // DriveSave round-trip preserves all Rally counters & unlocks.
  const std::string txt = drive::save_to_text(save);
  drive::DriveSave reloaded;
  std::string err;
  CHECK(drive::save_from_text(txt, reloaded, &err));
  CHECK(reloaded.has_rally && reloaded.num_rallies == save.num_rallies &&
        reloaded.num_rally_stages == save.num_rally_stages &&
        reloaded.num_rally_victories == save.num_rally_victories &&
        reloaded.rally_xp == save.rally_xp && reloaded.rally_level == save.rally_level &&
        reloaded.rally_winnings == save.rally_winnings);
}

void test_restriction_0x80_and_rally_parts() {
  const auto d = shift::data::decode_restriction_value("0000000084");
  CHECK(d.ok() && d.set.flag34 && d.set.rally_upgraded);
  CHECK(shift::data::encode_restriction_value(d.set) == "0000000084");

  shift::data::CarProperties car;
  car.rally_upgraded = false;
  CHECK(!shift::data::car_passes(d.set, car));
  car.rally_upgraded = true;
  CHECK(shift::data::car_passes(d.set, car));

  CHECK(garage::is_rally_part(8, 5));   // Rally tyres
  CHECK(garage::is_rally_part(5, 4));   // Rally suspension
  CHECK(garage::is_rally_part(31, 4));  // Rally transmission
  CHECK(!garage::is_rally_part(8, 3));  // Race tyres != Rally tyres
}

void test_codriver_pace_notes_and_audio() {
  const char* kAudioXml = R"(
    <CoDriverAudio>
      <event play="50" group="CoDriver/Distance" cue="50" icon="None"/>
      <event play="HardLeft" group="CoDriver/Turns/Left" cue="HardLeft" icon="HardLeft"/>
      <event play="DontCut" group="CoDriver/AdditionalInfo" cue="DontCut" icon="None"/>
      <event play="Caution" group="CoDriver/AdditionalInfo" cue="Caution" icon="None"/>
      <event play="HairpinRight" group="CoDriver/Turns/Right" cue="HairpinRight" icon="HairpinRight"/>
      <event play="OverCrest" group="CoDriver/AdditionalInfo" cue="OverCrest" icon="None"/>
    </CoDriverAudio>
  )";
  race::CoDriverAudioMap map;
  CHECK(race::parse_codriver_audio_xml(kAudioXml, map));
  CHECK(map.by_id.size() == 6);
  CHECK(map.find("HardLeft") != nullptr && map.find("HardLeft")->icon == "HardLeft");

  const char* kRouteXml = R"(
    <TrackRoute>
      <RallyCalls>
        <RallyCall name="gate_1" width="20">
          <Samples><Sample id="50"/><Sample id="HardLeft"/><Sample id="DontCut"/></Samples>
          <Transform pos.x="0" pos.y="0" pos.z="50" facing.x="0" facing.y="0" facing.z="1"/>
        </RallyCall>
        <RallyCall name="gate_2" width="20">
          <Samples><Sample id="Caution"/><Sample id="HairpinRight"/><Sample id="OverCrest"/></Samples>
          <Transform pos.x="0" pos.y="0" pos.z="120" facing.x="0" facing.y="0" facing.z="1"/>
        </RallyCall>
      </RallyCalls>
    </TrackRoute>
  )";
  std::vector<race::RallyCallGate> gates;
  CHECK(race::parse_rally_calls_xml(kRouteXml, gates));
  CHECK(gates.size() == 2 && gates[0].sample_ids.size() == 3 && gates[1].sample_ids.size() == 3);

  race::RallyPaceNoteTracker tracker;
  tracker.load(gates, map);
  // Start before gate 1.
  CHECK(tracker.observe(0.0, 0.0, 10.0) == 0);
  // Miss laterally outside gate width (x=25 > 20/2) -> must NOT trigger.
  CHECK(tracker.observe(1.0, 25.0, 60.0) == 0);
  CHECK(tracker.fired_count() == 0);

  // Reset and cross gate 1 then gate 2 in order.
  tracker.reset();
  CHECK(tracker.observe(0.0, 0.0, 10.0) == 0);
  CHECK(tracker.observe(1.0, 2.0, 60.0) == 1);
  CHECK(tracker.fired_count() == 1);
  const auto hud1 = tracker.hud(1.5);
  CHECK(hud1.active && hud1.icons.size() == 1 && hud1.icons[0].kind == race::PaceIconKind::kHardLeft);
  CHECK(hud1.modifiers.size() == 2);  // 50M + DON'T CUT

  race::CoDriverAudioCue c0, c1, c2;
  CHECK(tracker.pop_audio_cue(c0) && c0.cue == "50");
  CHECK(tracker.pop_audio_cue(c1) && c1.cue == "HardLeft");
  CHECK(tracker.pop_audio_cue(c2) && c2.cue == "DontCut");

  // Cross gate 2 at t=3.0.
  CHECK(tracker.observe(3.0, 0.0, 130.0) == 1);
  CHECK(tracker.fired_count() == 2);
  const auto hud2 = tracker.hud(3.2);
  CHECK(hud2.active && hud2.icons.size() == 3);  // Caution, HairpinRight, Crest

  // Rewind to t=2.0 -> gate 2 un-fires, gate 1 stays fired!
  CHECK(tracker.observe(2.0, 0.0, 90.0) == 0);
  CHECK(tracker.fired_count() == 1);
  CHECK(tracker.observe(3.5, 0.0, 135.0) == 1);
  CHECK(tracker.fired_count() == 2);

  // CoDriverVoPlayer renders onto Bus::kVo inside AudioEngine, reports vo_playing,
  // and increments codriver_completed_cues when the clip finishes.
  audio::AudioEngine eng(48000);
  auto cd = std::make_unique<audio::CoDriverVoPlayer>(48000);
  audio::PcmSample tone;
  tone.rate = 48000;
  tone.data.assign(480, 0.5f);
  cd->set_clip("CoDriver/Turns/Left/HardLeft", std::move(tone));
  eng.set_codriver_vo(std::move(cd));
  eng.trigger_codriver("CoDriver/Turns/Left/HardLeft");
  std::vector<float> pcm(480 * 2, 0.0f);
  eng.render_offline(pcm.data(), 480);
  float peak = 0.0f;
  for (float v : pcm) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.1f);
  // Render a second block so the 480-sample clip finishes and bumps completed_cues.
  eng.render_offline(pcm.data(), 480);
  CHECK(eng.telemetry().codriver_completed_cues >= 1u);

  // Speed-scaled lookahead triggers gate 1 early at 50 m/s (lookahead ~13.5 m before z=50).
  tracker.reset();
  CHECK(tracker.observe(0.0, 0.0, 30.0, 50.0) == 0);
  CHECK(tracker.observe(0.2, 0.0, 42.0, 50.0) == 1);  // crossed at z~36.5 with lookahead!
  // Stale-call drop: crossing gate 2 before popping gate 1's 3 cues replaces them with gate 2's cues.
  CHECK(tracker.pending_audio_cues() == 3);
  CHECK(tracker.observe(1.5, 0.0, 125.0, 50.0) == 1);
  CHECK(tracker.pending_audio_cues() == 3);
  race::CoDriverAudioCue first_after_drop;
  CHECK(tracker.pop_audio_cue(first_after_drop) && first_after_drop.cue == "Caution");
}

void test_real_v4_corpus() {
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  if (!state || !*state) return;

  race::RallyCatalog cat;
  std::string err;
  CHECK(race::load_rally_catalog(state, nullptr, nullptr, cat, &err));
  if (!err.empty()) std::printf("rally corpus note: %s\n", err.c_str());
  CHECK(cat.loaded);
  CHECK(cat.championships.size() == 7);
  size_t total_stages = 0;
  for (const auto& ch : cat.championships) total_stages += ch.stages.size();
  CHECK(total_stages == 28);
  CHECK(cat.scoring.size() == 51 && cat.scoring[1] == 60);
  CHECK(cat.wristbands.size() == 2 && cat.wristbands[1].xp_required == 310);
  CHECK(cat.field.drivers.size() == 49);
  CHECK(cat.heats.event_id == 279 && cat.heats.route_id == 24);
  CHECK(!cat.package_dir.empty());

  race::CoDriverAudioMap cd_map;
  CHECK(race::load_codriver_audio_map(cat.package_dir, cd_map, &err));
  CHECK(cd_map.by_id.size() == 104);

  std::vector<race::RallyCallGate> gates;
  CHECK(race::load_rally_calls({}, cat.package_dir, 7, gates, &err));
  CHECK(!gates.empty());
  std::printf("rally corpus: %zu championships, %zu stages, %zu CoDriverAudio events, route 7 has %zu RallyCall gates\n",
              cat.championships.size(), total_stages, cd_map.by_id.size(), gates.size());
}

}  // namespace

int main() {
  test_championship_standings_and_rewards();
  test_restriction_0x80_and_rally_parts();
  test_codriver_pace_notes_and_audio();
  test_real_v4_corpus();
  if (failures) {
    std::printf("rally_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("rally_tests: all passed\n");
  return 0;
}
