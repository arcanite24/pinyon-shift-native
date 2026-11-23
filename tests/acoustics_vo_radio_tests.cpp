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
    amb.render(s_tun, 10.0f, abuf.data(), 4800);  // s_tun has Mountains ambience
    check(amb.loaded_bed_count() == 2, "AmbienceBedPlayer drained 2 PCM beds");
    double amb_e = 0.0;
    for (float v : abuf) amb_e += double(v) * double(v);
    check(amb_e > 0.5, "AmbienceBedPlayer renders strong real PCM regional bed");
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

}  // namespace

int main() {
  test_vo_player_and_skip_with_a();
  test_full_radio_depth_and_3d_stations();
  test_world_acoustics_and_reverb();
  if (g_failures > 0) {
    std::fprintf(stderr, "acoustics_vo_radio_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("acoustics_vo_radio_tests: all checks passed\n");
  return 0;
}
