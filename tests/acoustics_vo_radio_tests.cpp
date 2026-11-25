#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/radio.h"
#include "audio/vo_player.h"
#include "audio/world_acoustics.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

audio::PcmSample make_tone(int rate, double seconds, double hz, float amp = 0.5f) {
  audio::PcmSample s;
  s.rate = rate;
  const size_t n = size_t(rate * seconds);
  s.data.resize(n);
  for (size_t i = 0; i < n; ++i)
    s.data[i] = amp * std::sin(float(2.0 * 3.14159265 * hz * double(i) / double(rate)));
  return s;
}

void test_vo_player_and_skip_with_a() {
  const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<DialogueScript>
  <Trigger id="1040BarnFindAtWorkshop">
    <Filename id="Dak_BarnFind_1040BarnFindAtWorkshop_1" soundbankIndex="485" />
  </Trigger>
  <Trigger id="AllComersEventActivated">
    <Filename id="Dak_Tutorial_AllComersEventActivated_1" soundbankIndex="581" />
  </Trigger>
</DialogueScript>)";
  audio::VoScript script;
  std::string err;
  check(audio::parse_dialogue_script(xml, script, &err), "parse_dialogue_script succeeds");
  check(script.trigger_count() == 2, "parsed 2 VO triggers");
  const auto* e = script.find("1040barnfindatworkshop");
  check(e && e->size() == 1 && (*e)[0].soundbank_index == 485, "case-insensitive VO trigger lookup matches index 485");

  audio::VoPlayer vp(48000, false);
  vp.set_clip("line_1", make_tone(48000, 1.0, 500.0, 0.4f));
  vp.set_clip("line_2", make_tone(48000, 1.0, 800.0, 0.4f));
  check(vp.trigger("line_1"), "triggers line_1");
  check(vp.trigger("line_2"), "queues line_2");
  check(vp.playing() && vp.current_trigger() == "line_1", "line_1 is playing");

  std::vector<float> buf(480 * 2, 0.0f);
  check(vp.render(buf.data(), 480), "render returns true while VO active");
  double energy = 0.0;
  for (float v : buf) energy += double(v) * double(v);
  check(energy > 1.0, "VO renders audible signal");

  // Pinyon Shift "Skip Voice-Overs with A" advances immediately to line_2, then stops.
  vp.skip_current();
  check(vp.playing() && vp.current_trigger() == "line_2", "skip_current advances to queued line_2");
  vp.skip_current();
  check(!vp.playing(), "second skip_current silences VO");

  // Verify AudioEngine VOPlaying snapshot ducking & telemetry.
  audio::AudioEngine eng(48000);
  auto vp_ptr = std::make_unique<audio::VoPlayer>(48000, false);
  vp_ptr->set_clip("barn_hint", make_tone(48000, 0.5, 440.0, 0.5f));
  eng.set_vo_player(std::move(vp_ptr));
  eng.trigger_vo("barn_hint");
  std::vector<float> out(480 * 2, 0.0f);
  eng.render_offline(out.data(), 480);
  check(eng.telemetry().vo_playing, "AudioEngine telemetry reports vo_playing=true");
  eng.skip_vo();
  eng.render_offline(out.data(), 480);
  check(!eng.telemetry().vo_playing, "AudioEngine skip_vo clears vo_playing");
}

void test_full_radio_depth_and_3d_stations() {
  const std::string sys_xml = R"(
<RadioSystem>
  <System maxTracksBetweenDJ="2" masterLevel="1.0" musicLevel="0.85" dialogueLevel="1.0" identLevel="0.3"
          duckValue="0.3" duckFadeDownTime="0.1" duckFadeUpTime="0.5" stationChangeFadeTimeUser="0.25"
          stationChangeFadeTimeNormal="1.25" HUDNewTrackDisplayDelayFreeRoam="2.0" />
  <RadioStation name="Radio1">
    <Playlist noRepeat="1">
      <MusicTrack name="Track Day" artist="Artist A" fmodname="R1_Day" likelihoodDaytime="5" likelihoodMorning="0" likelihoodEvening="0" likelihoodNight="0"/>
      <MusicTrack name="Track Night" artist="Artist B" fmodname="R1_Night" likelihoodDaytime="0" likelihoodMorning="0" likelihoodEvening="0" likelihoodNight="5"/>
    </Playlist>
    <Idents noRepeat="0"><Ident fmodName="R1_Ident1"/></Idents>
    <DJRegularDialogue noRepeat="0"><SimpleDialogue fmodName="R1_DJ1"/></DJRegularDialogue>
    <DJSpecialDialogue>
      <SpecialDialogue gameplayEvent="BarnFindLocationHint1" fmodName="R1_Barn1"/>
    </DJSpecialDialogue>
    <DJImmediateDialogue>
      <SpecialDialogue gameplayEvent="AliceOpeningInterview1" fmodName="R1_Imm1"/>
    </DJImmediateDialogue>
    <DJFestivalUpdateLeadIn noRepeat="0"><SimpleDialogue fmodName="R1_LeadIn"/></DJFestivalUpdateLeadIn>
    <DJFestivalUpdateLeadOut noRepeat="0"><SimpleDialogue fmodName="R1_LeadOut"/></DJFestivalUpdateLeadOut>
  </RadioStation>
  <RadioStation name="Radio4_Silent" isOffStation="true" />
  <RadioStation name="Radio5_3d_PreRace" isOffStation="false" reverbWetLevel="-15.0" volume="0.35" 3dPanLevel="1.0">
    <Playlist noRepeat="0">
      <MusicTrack name="PreRace1" artist="None" fmodname="Pre_R1"/>
    </Playlist>
  </RadioStation>
  <FestivalUpdates>
    <FestivalUpdate gameplayEvent="DukeDefeatedByPlayerInterview" fmodName="FU_Duke"/>
    <FestivalUpdate gameplayEvent="AnecdoteInterview4" fmodName="FU_Vox4"/>
  </FestivalUpdates>
</RadioSystem>)";
  const std::string music_info = R"(
<SoundBankInfoList>
  <Bank Name="R1_Day"><SoundBankInfo SoundBankIndex="0" FileLength="100000" OffsetEventStart="20000" OffsetIdentStart="95000"/></Bank>
  <Bank Name="R1_Night"><SoundBankInfo SoundBankIndex="1" FileLength="100000" OffsetEventStart="20000" OffsetIdentStart="95000"/></Bank>
  <Bank Name="Pre_R1"><SoundBankInfo SoundBankIndex="2" FileLength="100000" OffsetEventStart="10000" OffsetIdentStart="95000"/></Bank>
</SoundBankInfoList>)";
  const std::string vo_info = R"(
<SoundBankInfoList>
  <Bank Name="R1_Ident1"><SoundBankInfo SoundBankIndex="0" FileLength="5000" OffsetStartNextTrack="4000"/></Bank>
  <Bank Name="R1_DJ1"><SoundBankInfo SoundBankIndex="1" FileLength="6000"/></Bank>
  <Bank Name="R1_Barn1"><SoundBankInfo SoundBankIndex="2" FileLength="8000"/></Bank>
  <Bank Name="R1_Imm1"><SoundBankInfo SoundBankIndex="3" FileLength="7000"/></Bank>
  <Bank Name="R1_LeadIn"><SoundBankInfo SoundBankIndex="4" FileLength="3000"/></Bank>
  <Bank Name="R1_LeadOut"><SoundBankInfo SoundBankIndex="5" FileLength="3000"/></Bank>
  <Bank Name="FU_Duke"><SoundBankInfo SoundBankIndex="6" FileLength="10000"/></Bank>
  <Bank Name="FU_Vox4"><SoundBankInfo SoundBankIndex="7" FileLength="9000"/></Bank>
</SoundBankInfoList>)";

  audio::RadioData rd;
  std::string err;
  check(audio::parse_radio(sys_xml, music_info, vo_info, rd, &err), "parse_radio succeeds");
  check(rd.levels.max_tracks_between_dj == 2, "parsed maxTracksBetweenDJ == 2");
  check(std::fabs(rd.levels.change_fade_normal - 1.25) < 1e-4, "parsed stationChangeFadeTimeNormal == 1.25");
  check(rd.cycle.size() == 2, "player cycle has Radio1 + Radio4_Silent");
  check(rd.location_stations.size() == 1, "1 3D location station (Radio5_3d_PreRace) parsed");
  check(rd.stations[size_t(rd.location_stations[0])].is_3d, "Radio5_3d_PreRace marked is_3d");
  check(std::fabs(rd.stations[size_t(rd.location_stations[0])].volume - 0.35) < 1e-4, "3D station volume parsed");
  check(rd.festival_updates.size() == 2, "2 FestivalUpdates parsed");

  const std::string vp_xml = R"(<VoxPops><Festival gameplayEvent="AnecdoteInterview4"/><Setup NoRepeat="0" IntervalMin="120.0" IntervalMax="600.0"/></VoxPops>)";
  check(audio::parse_radio_vox_pops(vp_xml, rd.vox_pops), "parse_radio_vox_pops succeeds");
  check(rd.vox_pops.events.size() == 1 && rd.vox_pops.events[0] == "AnecdoteInterview4", "VoxPop event parsed");

  audio::Radio radio(rd, 42, true);
  radio.select(0);
  check(!radio.last_change_was_user(), "programmatic select uses normal station fade");
  radio.press(1);
  check(radio.last_change_was_user(), "D-pad press uses user station fade");
  radio.press(-1);
  check(radio.schedule(rd.location_stations[0]) != nullptr, "3D location station has active StationSchedule");
  check(radio.trigger_immediate_dialogue("AliceOpeningInterview1"), "trigger_immediate_dialogue succeeds");
  std::vector<audio::RadioEvent> evs;
  radio.schedule(0)->audible(0.0, 2.0, evs);
  bool saw_imm = false;
  for (const auto& e : evs)
    if (e.kind == audio::RadioKind::kDj) saw_imm = true;
  check(saw_imm, "immediate dialogue audible at current clock");

  // Trigger FestivalUpdate (LeadIn + Update + LeadOut) & SpecialDialogue.
  check(radio.trigger_festival_update("DukeDefeatedByPlayerInterview"), "trigger_festival_update queued");
  check(radio.trigger_special_dialogue("BarnFindLocationHint1"), "trigger_special_dialogue queued");
  radio.schedule(0)->audible(0.0, 250.0, evs);
  int fest_clips = 0;
  for (const auto& e : evs)
    if (e.kind == audio::RadioKind::kFestivalUpdate) ++fest_clips;
  check(fest_clips == 3, "FestivalUpdate emits LeadIn + Update + LeadOut (3 clips)");
}

void test_world_acoustics_and_reverb() {
  const std::string rev_xml = R"(
<ReverbData><ReverbTemplates>
  <Template Name="0_TrackOpen" DecayTimeSec="1.2" DecayHFRatio="2.0" NearDry="1.0" NearWet="0.0" RoomLevelmB="-800" ReverbLevelmB="1500" SnapshotMix="NotInTunnel"/>
  <Template Name="12_PGTunnel" DecayTimeSec="2.8" DecayHFRatio="2.0" NearDry="1.0" NearWet="0.8" RoomLevelmB="-200" ReverbLevelmB="1200" ReverbDelaySec="0.015" SnapshotMix="InTunnel"/>
</ReverbTemplates></ReverbData>)";
  const std::string amb_xml = R"(
<AmbienceData><AmbienceTemplates>
  <Template Name="0_Default" SnapshotMix="DefaultLocation" BackgroundStreamName="Default_Quad_Plain" BackgroundStreamStartBelowSpeed="35.0" BackgroundStreamStopAboveSpeed="40.0"/>
  <Template Name="2_Mountains" SnapshotMix="Mountains" BackgroundStreamName="Mountains_Quad_Mountains"/>
</AmbienceTemplates></AmbienceData>)";
  const std::string ss_xml = R"(
<AudioTile eventfilecrc="0" soundbankfilecrc="0">
  <TrackAudio soundbank=""><Track3D>
    <Object objectName="Festival_Stage_Music_A" eventPath="" eventName="Festival_Stage_Music_A">
      <Position1 x="-1450" y="10" z="-350"/><Position2 x="-1450" y="10" z="-350"/>
    </Object>
  </Track3D></TrackAudio>
  <ReverbZones>
    <ReverbZone Template0="0" Template1="1" MinHeight="0" MaxHeight="200">
      <ReverbTriangle>
        <ReverbTrianglePoint positionX="0" positionZ="0" value="0"/>
        <ReverbTrianglePoint positionX="100" positionZ="0" value="1"/>
        <ReverbTrianglePoint positionX="0" positionZ="100" value="1"/>
      </ReverbTriangle>
    </ReverbZone>
  </ReverbZones>
  <AmbienceZones>
    <AmbienceZone Template="1" MinHeight="0" MaxHeight="500">
      <AmbienceTriangle>
        <AmbienceTrianglePoint positionX="0" positionZ="0"/>
        <AmbienceTrianglePoint positionX="100" positionZ="0"/>
        <AmbienceTrianglePoint positionX="0" positionZ="100"/>
      </AmbienceTriangle>
    </AmbienceZone>
  </AmbienceZones>
</AudioTile>)";

  audio::WorldAcousticsData ac;
  std::vector<audio::ReverbTemplate> rts;
  std::vector<audio::AmbienceTemplate> ats;
  audio::SoundscapeTile tile;
  check(audio::parse_reverb_xml(rev_xml, rts) && rts.size() == 2, "parsed 2 reverb templates");
  check(audio::parse_ambience_xml(amb_xml, ats) && ats.size() == 2, "parsed 2 ambience templates");
  check(audio::parse_soundscape_xml(ss_xml, tile), "parsed .soundscape XML tile");
  ac.set_reverb_templates(rts);
  ac.set_ambience_templates(ats);
  ac.add_tile(std::move(tile));

  // Outside zone -> open road (NotInTunnel).
  const auto s_open = ac.evaluate(500.0f, 50.0f, 500.0f);
  check(!s_open.in_tunnel, "open road is NotInTunnel");
  check(s_open.ambience_snapshot == "DefaultLocation", "open road defaults to DefaultLocation");

  // Inside tunnel triangle near (80, 50, 10) where blend ~ 0.9 -> InTunnel + Mountains!
  const auto s_tun = ac.evaluate(80.0f, 50.0f, 10.0f);
  check(s_tun.in_tunnel, "inside tunnel triangle reports InTunnel");
  check(s_tun.reverb.decay_time_s > 2.2f, "tunnel barycentric decay interpolated toward 2.8s");
  check(s_tun.ambience_snapshot == "Mountains", "ambience zone reports Mountains");

  // Near Festival stage emitter -> high festival_proximity & Festival snapshot.
  const auto s_fest = ac.evaluate(-1450.0f, 10.0f, -340.0f);
  check(s_fest.festival_proximity > 0.9f && s_fest.ambience_snapshot == "Festival", "near stage reports Festival");

  // Verify EnvironmentalReverb produces a measurable reverb tail after an impulse
  // (both in-place process() and send-bus process_wet() used for multi-bus reverb).
  audio::EnvironmentalReverb rev;
  rev.prepare(48000);
  rev.set_parameters(s_tun.reverb);
  std::vector<float> buf(4800 * 2, 0.0f);
  buf[0] = 1.0f;
  buf[1] = 1.0f;
  rev.process(buf.data(), 4800);
  double tail_e = 0.0;
  for (size_t i = 2000 * 2; i < buf.size(); ++i) tail_e += double(buf[i]) * double(buf[i]);
  check(tail_e > 1e-4, "EnvironmentalReverb produces audible tunnel reverb tail");

  audio::EnvironmentalReverb rev_wet;
  rev_wet.prepare(48000);
  rev_wet.set_parameters(s_tun.reverb);
  std::vector<float> dry_in(4800 * 2, 0.0f), wet_out(4800 * 2, 0.0f);
  dry_in[0] = 1.0f;
  dry_in[1] = 1.0f;
  check(rev_wet.process_wet(dry_in.data(), wet_out.data(), 4800), "process_wet active for tunnel reverb");
  double wet_tail = 0.0;
  for (size_t i = 2000 * 2; i < wet_out.size(); ++i) wet_tail += double(wet_out[i]) * double(wet_out[i]);
  check(wet_tail > 1e-4, "EnvironmentalReverb::process_wet produces stereo wet tail");

  // Verify AmbienceBedPlayer plays real injected PCM regional & crowd loops (Pillar 5 P0-12).
  {
    audio::AmbienceBedPlayer amb(48000);
    amb.set_bed_sample(audio::AmbienceBedSlot::kMountains, make_tone(48000, 0.5, 310.0, 0.6f));
    amb.set_bed_sample(audio::AmbienceBedSlot::kCrowd, make_tone(48000, 0.5, 190.0, 0.6f));
    std::vector<float> abuf(4800 * 2, 0.0f);
    amb.render(s_tun, 10.0f, abuf.data(), 4800, 12.0f);  // s_tun has Mountains ambience (day)
    check(amb.loaded_bed_count() == 2, "AmbienceBedPlayer drained 2 PCM beds");
    double amb_day_e = 0.0;
    for (float v : abuf) amb_day_e += double(v) * double(v);
    check(amb_day_e > 0.5, "AmbienceBedPlayer renders strong real PCM regional bed");

    // Verify ToD night ambience attenuates open-countryside daytime breeze bed cleanly.
    audio::AmbienceBedPlayer amb_night(48000);
    amb_night.set_bed_sample(audio::AmbienceBedSlot::kMountains, make_tone(48000, 0.5, 310.0, 0.6f));
    std::vector<float> nbuf(4800 * 2, 0.0f);
    amb_night.render(s_tun, 10.0f, nbuf.data(), 4800, 23.0f);
    double amb_night_e = 0.0;
    for (float v : nbuf) amb_night_e += double(v) * double(v);
    check(amb_night_e > 0.1 && amb_night_e < amb_day_e, "Night ToD scales down daytime regional nature bed");
  }

  // P1: WorldAcousticsData::clear_soundscapes() cleanly swaps Colorado -> ColoradoDirt zones.
  {
    audio::WorldAcousticsData sw;
    audio::SoundscapeTile t1;
    audio::ReverbZone rz1;
    rz1.template0 = 0;
    rz1.template1 = 1;
    rz1.min_x = -10;
    rz1.max_x = 10;
    rz1.min_z = -10;
    rz1.max_z = 10;
    t1.reverb_zones.push_back(rz1);
    sw.add_tile(t1);
    check(sw.reverb_zone_count() == 1, "Colorado soundscape tile added");
    sw.clear_soundscapes();
    check(sw.reverb_zone_count() == 0 && sw.ambience_zone_count() == 0,
          "clear_soundscapes resets zones cleanly for Rally world switch");
  }

  // Verify real disc files if available (24 reverb templates, 6 ambience templates, real RadioSystem + DialogueScript).
  const std::filesystem::path game_root = "../horizon1-recomp/.local/game/base";
  std::error_code ec;
  if (std::filesystem::exists(game_root / "media/tracks/colorado/colorado_reverb.xml", ec)) {
    audio::WorldAcousticsData disc_ac;
    std::string note;
    check(disc_ac.load_configs(game_root, &note), "loads real disc acoustics configs");
    check(disc_ac.reverb_templates().size() == 24, "colorado_reverb.xml has all 24 I3DL2 templates");
    check(disc_ac.ambience_templates().size() == 6, "colorado_ambience.xml has all 6 templates");
    check(disc_ac.emitters().size() >= 5, "ColoradoRibbon_00.xml has 5+ festival emitters");

    audio::RadioData disc_rd;
    std::string rerr;
    check(audio::load_radio(game_root, disc_rd, &rerr, "EN"), "loads real disc RadioSystem + VoxPops");
    check(disc_rd.location_stations.size() == 5, "disc has all 5 3D location stations (Radio5..Radio9)");
    check(disc_rd.festival_updates.size() >= 20, "disc has 20+ FestivalUpdates");
    check(!disc_rd.vox_pops.events.empty(), "disc RadioDJVoxPops.xml parsed");

    audio::VoPlayer disc_vo(48000, true);
    check(disc_vo.load(game_root, "EN", &note), "loads real disc DialogueScript_EN.xml + VO_EN_Stream.fsb");
    check(disc_vo.script().trigger_count() > 200, "DialogueScript_EN.xml has >200 triggers");
  }
}

void test_phase_r2_5_acoustics_vo_radio_parity() {
  // 1. EnvironmentalReverb early slap-back reflections driven by reflections_level_mb.
  {
    audio::ReverbTemplate rt_early{};
    rt_early.near_dry = 1.0f;
    rt_early.near_wet = 0.85f;
    rt_early.room_level_mb = 0.0f;
    rt_early.reflections_level_mb = 0.0f;      // strong early reflections
    rt_early.reflections_delay_s = 0.010f;     // 10 ms initial slap-back
    rt_early.reverb_level_mb = -10000.0f;      // isolate early reflections from late tail

    audio::ReverbTemplate rt_muted = rt_early;
    rt_muted.reflections_level_mb = -10000.0f;  // mute early reflections too

    audio::EnvironmentalReverb rev_on, rev_off;
    rev_on.prepare(48000);
    rev_off.prepare(48000);
    rev_on.set_parameters(rt_early);
    rev_off.set_parameters(rt_muted);
    // Prime parameter smoothing with one silent block.
    std::vector<float> dummy_in(2400 * 2, 0.0f), dummy_out(2400 * 2, 0.0f);
    rev_on.process_wet(dummy_in.data(), dummy_out.data(), 2400);
    rev_off.process_wet(dummy_in.data(), dummy_out.data(), 2400);
    check(rev_on.early_gain() > 0.05f, "EnvironmentalReverb computes non-zero early reflection gain");
    check(rev_off.early_gain() < 1e-3f, "Muted reflections_level_mb zeroes early reflection gain");

    std::vector<float> imp(3200 * 2, 0.0f), wet_on(3200 * 2, 0.0f), wet_off(3200 * 2, 0.0f);
    imp[0] = 1.0f;
    imp[1] = 1.0f;
    check(rev_on.process_wet(imp.data(), wet_on.data(), 3200), "early-only reverb is active");
    rev_off.process_wet(imp.data(), wet_off.data(), 3200);
    double early_e_on = 0.0, early_e_off = 0.0;
    for (size_t i = 400 * 2; i < 2800 * 2; ++i) {
      early_e_on += double(wet_on[i]) * double(wet_on[i]);
      early_e_off += double(wet_off[i]) * double(wet_off[i]);
    }
    check(early_e_on > 1e-3 && early_e_on > early_e_off * 100.0,
          "EnvironmentalReverb early slap-back taps produce measurable reflections");
  }

  // 2. Bus::kCollision and Bus::kSuspension are included in EnvironmentalReverb send mix.
  {
    audio::AudioEngine eng(48000);
    auto sfx = std::make_unique<audio::SfxEngine>(48000);
    audio::PcmSample short_hit;
    short_hit.rate = 48000;
    short_hit.data.assign(240, 0.8f);  // 5 ms sharp impulse
    sfx->set_sample(audio::SfxSoundId::kImpact3d, short_hit);
    sfx->set_sample(audio::SfxSoundId::kCarLandingHeavy, short_hit);
    eng.set_sfx_engine(std::move(sfx));

    audio::AcousticsSample tun{};
    tun.in_tunnel = true;
    tun.reverb.near_dry = 1.0f;
    tun.reverb.near_wet = 0.9f;
    tun.reverb.decay_time_s = 2.6f;
    tun.reverb.room_level_mb = -100.0f;
    tun.reverb.reflections_level_mb = -200.0f;
    tun.reverb.reverb_level_mb = 1200.0f;
    eng.set_acoustics_sample(tun);
    // Warm up reverb smoother.
    std::vector<float> warm(2400 * 2, 0.0f);
    eng.render_offline(warm.data(), 2400);

    audio::CollisionHit ch{};
    ch.rel_speed_mps = 20.0f;
    ch.pos = {1.0f, 0.0f, 0.0f};
    eng.trigger_collision(ch);
    std::vector<float> out(4800 * 2, 0.0f);
    eng.render_offline(out.data(), 4800);
    // Dry 5 ms hit ends at frame 240; energy after frame 1500 (~31 ms) comes from reverb tail!
    double col_tail = 0.0;
    for (size_t i = 1500 * 2; i < out.size(); ++i) col_tail += double(out[i]) * double(out[i]);
    check(col_tail > 1e-4, "Bus::kCollision feeds EnvironmentalReverb tail");

    // Same for Bus::kSuspension landing hit.
    std::vector<float> settle(24000 * 2, 0.0f);
    eng.render_offline(settle.data(), 24000);
    audio::LandingHit lh{};
    lh.impact_speed_mps = 8.0f;
    lh.air_time_s = 0.9f;
    eng.trigger_landing(lh);
    std::fill(out.begin(), out.end(), 0.0f);
    eng.render_offline(out.data(), 4800);
    double susp_tail = 0.0;
    for (size_t i = 1500 * 2; i < out.size(); ++i) susp_tail += double(out[i]) * double(out[i]);
    check(susp_tail > 1e-4, "Bus::kSuspension feeds EnvironmentalReverb tail");
  }

  // 3. AmbienceBedPlayer loop-boundary crossfade smoothing eliminates seam clicks.
  {
    audio::AmbienceBedPlayer amb(48000);
    audio::PcmSample ramp;
    ramp.rate = 48000;
    ramp.data.resize(4096);
    for (size_t i = 0; i < ramp.data.size(); ++i) {
      ramp.data[i] = -0.8f + 1.6f * float(i) / float(ramp.data.size() - 1);  // huge 1.6 seam jump!
    }
    amb.set_bed_sample(audio::AmbienceBedSlot::kDefault, std::move(ramp));
    audio::AcousticsSample ac{};
    ac.ambience_snapshot = "DefaultLocation";
    // Render across several full loop wraps.
    std::vector<float> buf(16000 * 2, 0.0f);
    amb.render(ac, 10.0f, buf.data(), 16000, 12.0f);
    float max_step = 0.0f;
    for (int f = 1; f < 16000; ++f) {
      max_step = std::max(max_step, std::fabs(buf[2 * size_t(f)] - buf[2 * size_t(f - 1)]));
    }
    check(max_step < 0.02f, "AmbienceBedPlayer loop-boundary crossfade eliminates seam click");
  }

  // 4. Unified VO ducking: VoPlayer active status and set_dialog_ducking() never cancel each other.
  {
    const std::string snap_xml = R"(
      <AudioMixerSnapshots>
        <FreeRoam group="Gameplay">
          <GameMusic active="1" destvolume="1.0"/>
        </FreeRoam>
        <VOPlaying group="VO" fadein="0.0" fadeout="0.0">
          <GameMusic active="1" destvolume="0.25"/>
        </VOPlaying>
        <VONotPlaying group="VO" fadein="0.0" fadeout="0.0">
          <GameMusic active="1" destvolume="1.0"/>
        </VONotPlaying>
      </AudioMixerSnapshots>
    )";
    audio::AudioEngine eng(48000);
    check(eng.load_snapshot_matrix_xml(snap_xml), "loaded VO ducking snapshot matrix");
    auto vp = std::make_unique<audio::VoPlayer>(48000, false);
    vp->set_clip("intro_line", make_tone(48000, 2.0, 440.0, 0.4f));
    eng.set_vo_player(std::move(vp));

    std::vector<float> out(480 * 2, 0.0f);
    eng.trigger_vo("intro_line");
    eng.set_dialog_ducking(true);
    eng.render_offline(out.data(), 480);
    check(eng.matrix_bus_gain(audio::Bus::kMusic) < 0.35f, "VOPlaying ducks music while VO + dialog active");

    // Clearing UI dialog ducking while VoPlayer is STILL speaking must NOT un-duck VO!
    eng.set_dialog_ducking(false);
    eng.render_offline(out.data(), 480);
    check(eng.matrix_bus_gain(audio::Bus::kMusic) < 0.35f,
          "set_dialog_ducking(false) does not cancel active VoPlayer ducking");

    // Skipping VO now fades out over 10 ms (480 frames), then releases VOPlaying cleanly.
    eng.skip_vo();
    eng.render_offline(out.data(), 480);  // completes 10 ms click-free skip fade-out
    eng.render_offline(out.data(), 480);  // fade finished -> VONotPlaying active
    check(eng.matrix_bus_gain(audio::Bus::kMusic) > 0.90f, "VONotPlaying restores music once both VO and dialog end");
  }

  // 5. Outpost & Street-Hub 3D radio bleed emitters alongside Festival stages.
  {
    const std::string sys_xml = R"(
<RadioSystem>
  <System maxTracksBetweenDJ="2" masterLevel="1.0" musicLevel="0.85" dialogueLevel="1.0" identLevel="0.3"
          duckValue="0.3" duckFadeDownTime="0.1" duckFadeUpTime="0.5" stationChangeFadeTimeUser="0.25"
          stationChangeFadeTimeNormal="1.25" HUDNewTrackDisplayDelayFreeRoam="2.0" />
  <RadioStation name="Radio1">
    <Playlist noRepeat="0"><MusicTrack name="T1" artist="A" fmodname="M0"/></Playlist>
  </RadioStation>
  <RadioStation name="Radio4_Silent" isOffStation="true" />
  <RadioStation name="Radio5_3d_PreRace" isOffStation="false" volume="0.5" 3dPanLevel="1.0">
    <Playlist noRepeat="0"><MusicTrack name="Pre" artist="A" fmodname="M0"/></Playlist>
  </RadioStation>
  <RadioStation name="Radio6_3d_Autoshow" isOffStation="false" volume="0.5" 3dPanLevel="1.0">
    <Playlist noRepeat="0"><MusicTrack name="Auto" artist="A" fmodname="M0"/></Playlist>
  </RadioStation>
  <RadioStation name="Radio7_3d_Workshop" isOffStation="false" volume="0.5" 3dPanLevel="1.0">
    <Playlist noRepeat="0"><MusicTrack name="Work" artist="A" fmodname="M0"/></Playlist>
  </RadioStation>
  <RadioStation name="Radio8_3d_Paintshop" isOffStation="false" volume="0.5" 3dPanLevel="1.0">
    <Playlist noRepeat="0"><MusicTrack name="Paint" artist="A" fmodname="M0"/></Playlist>
  </RadioStation>
  <RadioStation name="Radio9_3d_Streetrace" isOffStation="false" volume="0.5" 3dPanLevel="1.0">
    <Playlist noRepeat="0"><MusicTrack name="Street" artist="A" fmodname="M0"/></Playlist>
  </RadioStation>
</RadioSystem>)";
    const std::string music_info = R"(
<SoundBankInfoList>
  <Bank Name="M0"><SoundBankInfo SoundBankIndex="0" FileLength="100000" OffsetEventStart="10000" OffsetIdentStart="95000"/></Bank>
</SoundBankInfoList>)";
    const std::string vo_info = R"(<SoundBankInfoList></SoundBankInfoList>)";
    audio::RadioData rd;
    std::string err;
    check(audio::parse_radio(sys_xml, music_info, vo_info, rd, &err), "parsed 5 3D location stations");
    audio::Radio radio(rd, 99, false);
    const auto emitters = audio::default_colorado_location_emitters(radio);
    check(emitters.size() == 14, "default_colorado_location_emitters covers Festival, Hubs, 3 Street Hubs & 6 Outposts");

    const int street_idx = radio.find_station("Radio9_3d_Streetrace");
    const int prerace_idx = radio.find_station("Radio5_3d_PreRace");
    int street_hubs = 0, outposts = 0;
    for (const auto& em : emitters) {
      if (em.station == street_idx) ++street_hubs;
      if (em.station == prerace_idx && em.max_distance < 200.0f) ++outposts;
    }
    check(street_hubs == 3, "all 3 Street Race Hubs registered on Radio9_3d_Streetrace");
    check(outposts == 6, "all 6 Colorado Outposts registered with 3D radio bleed");

    // Verify proximity math: listener at Carson Street Hub (-2100, 12, 1450) and Beaumont Outpost (-1120, 25, 2320)
    // is well within emitter max_distance, whereas far away (10000, 0, 10000) is out of range of every emitter.
    auto min_dist = [&](std::array<float, 3> p) {
      float best = 1e9f;
      for (const auto& em : emitters) {
        const float dx = em.pos[0] - p[0], dy = em.pos[1] - p[1], dz = em.pos[2] - p[2];
        best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
      }
      return best;
    };
    check(min_dist({-2100.0f, 12.0f, 1450.0f}) < 1.0f, "Carson Street Hub emitter exact proximity");
    check(min_dist({-1120.0f, 25.0f, 2320.0f}) < 1.0f, "Beaumont Outpost emitter exact proximity");
    check(min_dist({10000.0f, 0.0f, 10000.0f}) > 5000.0f, "Far listener is outside all 3D emitters");
  }
}

void test_r3_aud_10_vo_telemetry_skip_fade_and_mono_pan() {
  // 1. Equal-power mono center pan (0.70710678f) and live telemetry accessors.
  audio::VoPlayer vp(48000, false);
  audio::PcmSample dc;
  dc.rate = 48000;
  dc.data.assign(48000, 1.0f);  // 1.0 s constant +1.0 clip
  vp.set_clip("Intro_Alice_Line", dc);

  check(vp.trigger("Intro_Alice_Line"), "triggers Intro_Alice_Line");
  check(vp.playing(), "VoPlayer reports playing");
  check(vp.active_trigger() == "intro_alice_line", "active_trigger() returns lowercase atomic-safe trigger");
  check(vp.active_trigger_hash() != 0, "active_trigger_hash() non-zero while active");
  check(std::fabs(vp.duration_s() - 1.0f) < 1e-3f, "duration_s() reports exact clip duration (1.0 s)");
  check(vp.elapsed_s() < 1e-4f, "elapsed_s() starts at 0.0 s");

  std::vector<float> buf(4800 * 2, 0.0f);  // 100 ms
  check(vp.render(buf.data(), 4800), "render returns true while playing");
  check(std::fabs(buf[0] - 0.70710678f) < 1e-4f && std::fabs(buf[1] - 0.70710678f) < 1e-4f,
        "mono VO uses equal-power 0.70710678f stereo pan");
  check(std::fabs(vp.elapsed_s() - 0.1f) < 5e-3f, "elapsed_s() advances to ~0.1 s after 4800 frames");
  check(vp.current_rms() > 0.5f && vp.current_rms() < 0.8f,
        "current_rms() reports live smoothed block RMS amplitude");

  // 2. 10 ms smooth fade-out on skip_current() prevents hard step clicks.
  const float pre_skip_l = buf[(4800 - 1) * 2];
  vp.skip_current();
  check(!vp.playing(), "skip_current() clears logical playing() immediately when queue empty");
  check(vp.active_trigger().empty(), "skip_current() clears active_trigger() when queue empty");

  std::vector<float> fade_buf(600 * 2, 0.0f);  // 12.5 ms (covers full 480-frame 10 ms fade + tail)
  check(vp.render(fade_buf.data(), 600), "render outputs 10 ms skip fade-out tail");
  float max_step = std::fabs(fade_buf[0] - pre_skip_l);
  for (int i = 1; i < 600; ++i) {
    max_step = std::max(max_step, std::fabs(fade_buf[size_t(i) * 2] - fade_buf[size_t(i - 1) * 2]));
  }
  check(max_step < 0.01f, "VoPlayer 10 ms skip fade-out eliminates hard step click");
  check(std::fabs(fade_buf[0]) > 0.65f, "skip fade-out starts continuously near pre-skip level");
  check(std::fabs(fade_buf[479 * 2]) < 1e-3f && std::fabs(fade_buf[550 * 2]) < 1e-5f,
        "skip fade-out reaches silence cleanly by 10 ms (480 frames)");

  std::fill(fade_buf.begin(), fade_buf.end(), 0.0f);
  check(!vp.render(fade_buf.data(), 480), "VoPlayer returns false once 10 ms fade-out completes");
  check(vp.elapsed_s() == 0.0f && vp.duration_s() == 0.0f, "telemetry resets to 0 after VO finishes");
}

}  // namespace

int main() {
  test_vo_player_and_skip_with_a();
  test_full_radio_depth_and_3d_stations();
  test_world_acoustics_and_reverb();
  test_phase_r2_5_acoustics_vo_radio_parity();
  test_r3_aud_10_vo_telemetry_skip_fade_and_mono_pan();
  if (g_failures > 0) {
    std::fprintf(stderr, "acoustics_vo_radio_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("acoustics_vo_radio_tests: all checks passed\n");
  return 0;
}
