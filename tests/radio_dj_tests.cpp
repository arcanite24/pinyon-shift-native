// The radio DJ's gameplay triggers (SPEC-RADIO, audio/radio_dj.h): the
// wristband rules of radiodj.xml and the activities' RadioDJRequests on
// made-up data, and every gameplayEvent name the engine can send checked
// against a synthetic RadioSystem.xml that holds the disc's event names in
// the sections they sit in. With --corpus (SHIFT_GAME_ROOT; exit 77
// without), the same check against the player's RadioSystem.xml,
// radiodj.xml, barnfinds.xml, street_race_hubs.xml and Fame.xml.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "audio/radio.h"
#include "audio/radio_dj.h"
#include "freeroam/collectibles.h"
#include "freeroam/skills.h"
#include "race/campaign_flow.h"
#include "race/career.h"
#include "story_intro.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

using audio::RadioChannel;
using audio::RadioDjCue;

// The disc's gameplayEvent names (RadioSystem.xml, uncommented entries), by
// the section that holds them.
std::vector<std::string> disc_specials() {
  std::vector<std::string> v;
  for (int i = 1; i <= 9; ++i) {
    v.push_back("BarnFindLocationHint" + std::to_string(i));
    v.push_back("BarnFindSpotted" + std::to_string(i));
  }
  for (int i = 1; i <= 3; ++i) v.push_back("StreetRaceOwnerHub" + std::to_string(i));
  v.push_back("PlayerIsMostPopular");
  for (int i = 1; i <= 7; ++i) v.push_back("CongratsWinnersOfWristband" + std::to_string(i));
  return v;
}

std::vector<std::string> disc_immediates() {
  std::vector<std::string> v = {"AliceOpeningInterview1", "AliceOpeningInterview2A", "AliceOpeningInterview3",
                                "GoodLuck", "IntroCutscene", "TicketGiveawayReminder1", "TicketGiveawayReminder2",
                                "TicketGiveaway", "TonsOfRides", "E3Intro", "AllTicketsGone", "LinkToSong"};
  for (int i = 1; i <= 9; ++i) v.push_back("FestivalTicketsDepleting" + std::to_string(i));
  return v;
}

std::vector<std::string> disc_festival_updates() {
  std::vector<std::string> v = {"DukeOpinionOfPlayerInterview",     "DukeDefeatedByPlayerInterview",
                                "AliOpinionOfPlayerInterview",      "AliDefeatedByPlayerInterview",
                                "RamonaOpinionOfPlayerInterview",   "RamonaDefeatedByPlayerInterview",
                                "MarkoOpinionOfPlayerInterview",    "ZakiOpinionOfPlayerInterview",
                                "HarpersOpinionOfPlayerInterview",  "HarpersDefeatedByPlayerInterview",
                                "PlayerRecievesFirstWristband",     "PlayerCompetingInSecondWristband",
                                "PlayerRecievesThirdWristband",     "PlayerCompetingInFourthWristband",
                                "PlayerCompetingInFifthWristband",  "PlayerCompetingInSixthWristband",
                                "PlayerRecievesFinalWristband"};
  // FestivalUpdateWristband2, 5 and 6 are commented out on the disc.
  for (int i : {1, 3, 4, 7}) v.push_back("FestivalUpdateWristband" + std::to_string(i));
  for (int r : {1, 25, 50, 75, 100, 125, 150, 175}) v.push_back("FestivalUpdateSkillLevel" + std::to_string(r));
  return v;
}

// A RadioSystem.xml with one station whose special and immediate maps, and
// the FestivalUpdates list, hold those names; every line is the one clip.
std::string synthetic_system() {
  std::string x = "<RadioSystem><RadioStation name=\"Radio1\">"
                  "<Playlist noRepeat=\"0\"><MusicTrack name=\"S\" artist=\"A\" fmodname=\"T_One\"/></Playlist>"
                  "<DJSpecialDialogue>";
  for (const auto& e : disc_specials()) x += "<SpecialDialogue gameplayEvent=\"" + e + "\" fmodName=\"D_Line\"/>";
  x += "</DJSpecialDialogue><DJImmediateDialogue>";
  for (const auto& e : disc_immediates()) x += "<SpecialDialogue gameplayEvent=\"" + e + "\" fmodName=\"D_Line\"/>";
  x += "</DJImmediateDialogue></RadioStation><FestivalUpdates>";
  for (const auto& e : disc_festival_updates()) x += "<FestivalUpdate gameplayEvent=\"" + e + "\" fmodName=\"D_Line\"/>";
  x += "</FestivalUpdates></RadioSystem>";
  return x;
}

const char* kMusicInfo = R"(<SoundBanks>
  <Bank Name="T_One"><SoundBankInfo SoundBankIndex="0" OffsetEventStart="1000" OffsetIdentStart="9000" FileLength="10000"/></Bank>
</SoundBanks>)";
const char* kVoInfo = R"(<SoundBanks>
  <Bank Name="D_Line"><SoundBankInfo SoundBankIndex="0" OffsetStartNextTrack="0" FileLength="4000"/></Bank>
</SoundBanks>)";

// radiodj.xml's shape: the award state with a cancel, the family without a
// level, per-level interviews and one "after n events" update (one with the
// attribute misspelt, as the disc has it), and the ready state.
const char* kRules = R"(<StateGroup id="radiodj">
  <State id="festival_wristband">
    <Entry id="enter" target=".exit"/><Exit id="exit"/>
    <!-- a comment -->
    <Behaviour id="CRadioDJFestivalWristband" gameplayEvent="CancelPreviousTier" isFestivalUpdate="true" level="-1"/>
    <Behaviour id="CRadioDJFestivalWristband" gameplayEvent="FestivalUpdateWristband" isFestivalUpdate="true"/>
    <Behaviour id="CRadioDJFestivalWristband" gameplayEvent="AliOpinionOfPlayerInterview" isFestivalUpdate="true" level="1"/>
    <Behaviour id="CRadioDJFestivalWristband" gameplayEvent="AliDefeatedByPlayerInterview" isFestivalUpdate="true" level="2"/>
    <Behaviour id="CRadioDJFestivalWristband" gameplayEvent="PlayerRecievesFirstWristband" isFestivalUpdate="true" level="0" playAfterEventsNum="2" playAfterEventsMinPos="8"/>
    <Behaviour id="CRadioDJFestivalWristband" gameplayEvent="PlayerRecievesFinalWristband" isFestivalUpdate="true" level="6" pplayAfterEventsNum="2" playAfterEventsMinPos="2"/>
  </State>
  <State id="special_wristband">
    <Entry id="enter" target=".exit"/><Exit id="exit"/>
    <Behaviour id="CRadioDJFestivalWristband" gameplayEvent="CongratsWinnersOfWristband" isFestivalUpdate="false"/>
  </State>
  <State id="post_race"><Entry id="enter" target=".exit"/><Exit id="exit"/></State>
</StateGroup>)";

bool has(const std::vector<RadioDjCue>& v, RadioChannel ch, const std::string& ev) {
  return std::find(v.begin(), v.end(), RadioDjCue{ch, ev}) != v.end();
}

// Names the engine sends from its own code, not from the game's data.
std::vector<RadioDjCue> engine_literal_cues() {
  std::vector<RadioDjCue> out;
  out.push_back({RadioChannel::kImmediate, audio::kViperOpeningRadioEvent});
  for (const auto& wb : race::wristband_milestones())
    if (wb.radio_event && *wb.radio_event) out.push_back({RadioChannel::kImmediate, wb.radio_event});
  for (auto st : {carview::StoryStage::kBootSplash, carview::StoryStage::kOpeningFmv, carview::StoryStage::kViperDrive,
                  carview::StoryStage::kCorradoQualify, carview::StoryStage::kFestivalIntroBeats,
                  carview::StoryStage::kAutoshowHandoff})
    for (const auto& b : carview::StoryIntro::beats_for(st))
      if (!b.radio_event.empty()) out.push_back({RadioChannel::kImmediate, b.radio_event});
  return out;
}

void rules_and_tracker() {
  audio::RadioDjRules rules;
  std::string why;
  CHECK(audio::parse_radio_dj_rules(kRules, rules, &why));
  CHECK(rules.festival_wristband.size() == 6 && rules.special_wristband.size() == 1);
  if (rules.festival_wristband.size() != 6 || rules.special_wristband.size() != 1) return;
  CHECK(rules.festival_wristband[0].cancels());
  CHECK(rules.festival_wristband[1].level == audio::RadioDjWristbandRule::kAnyLevel);
  CHECK(rules.festival_wristband[4].after_events == 2 && rules.festival_wristband[4].min_position == 8);
  CHECK(rules.festival_wristband[5].after_events == 0);  // the misspelt attribute is not read
  CHECK(rules.special_wristband[0].channel == RadioChannel::kSpecial);

  // The family takes the wristband's 1-based number.
  CHECK(audio::wristband_rule_event(rules.festival_wristband[1], 0) == "FestivalUpdateWristband1");
  CHECK(audio::wristband_rule_event(rules.festival_wristband[1], 6) == "FestivalUpdateWristband7");
  CHECK(audio::wristband_rule_event(rules.festival_wristband[2], 0).empty());
  CHECK(audio::wristband_rule_event(rules.festival_wristband[0], 3).empty());

  audio::RadioDjWristbands wb(&rules);
  bool cancel = false;
  auto green = wb.on_awarded(1, &cancel);
  CHECK(cancel);
  CHECK(green.size() == 2);
  CHECK(has(green, RadioChannel::kFestivalUpdate, "FestivalUpdateWristband2"));
  CHECK(has(green, RadioChannel::kFestivalUpdate, "AliOpinionOfPlayerInterview"));
  CHECK(wb.on_awarded(1).empty());  // once per wristband
  auto blue = wb.on_awarded(2);
  CHECK(has(blue, RadioChannel::kFestivalUpdate, "AliDefeatedByPlayerInterview"));
  auto gold = wb.on_awarded(6);
  CHECK(has(gold, RadioChannel::kFestivalUpdate, "PlayerRecievesFinalWristband"));

  // Ready to collect: the special family, once per wristband.
  auto ready = wb.on_ready(2);
  CHECK(ready.size() == 1 && has(ready, RadioChannel::kSpecial, "CongratsWinnersOfWristband3"));
  CHECK(wb.on_ready(2).empty());

  // After two yellow events at 8th or better.
  CHECK(wb.on_event_finished(0, 9).empty());  // too far back
  CHECK(wb.on_event_finished(1, 1).empty());  // another tier
  CHECK(wb.on_event_finished(0, 8).empty());
  auto first = wb.on_event_finished(0, 3);
  CHECK(first.size() == 1 && has(first, RadioChannel::kFestivalUpdate, "PlayerRecievesFirstWristband"));
  CHECK(wb.on_event_finished(0, 1).empty());  // once

  const auto all = audio::all_wristband_cues(rules, 7);
  CHECK(has(all, RadioChannel::kSpecial, "CongratsWinnersOfWristband7"));
  CHECK(has(all, RadioChannel::kFestivalUpdate, "PlayerRecievesFirstWristband"));
  CHECK(!has(all, RadioChannel::kFestivalUpdate, "CancelPreviousTier"));
}

void requests() {
  audio::XmlNode root;
  std::string why;
  CHECK(audio::parse_xml(R"(<RadioDJRequests>
      <Regular group="20"/>
      <Special gameplayEvent="BarnFindLocationHint3" timeDelayMin="0.0" timeDelayMax="0.0" misc="-1" enabled="true" topPriority="true"/>
      <Special gameplayEvent="BarnFindSpotted3" timeDelayMin="600.0" timeDelayMax="1200.0" misc="1219" enabled="false" topPriority="false"/>
      <Immediate gameplayEvent="GoodLuck" timeDelay="2.5" triggerId="1005" enabled="true"/>
    </RadioDJRequests>)",
                         root, &why));
  const auto r = audio::parse_radio_dj_requests(root);
  CHECK(r.size() == 3);
  if (r.size() != 3) return;
  CHECK(r[0].event == "BarnFindLocationHint3" && r[0].enabled && r[0].top_priority && r[0].delay_max_s == 0.0);
  CHECK(r[1].event == "BarnFindSpotted3" && !r[1].enabled && r[1].delay_min_s == 600.0 && r[1].delay_max_s == 1200.0 &&
        r[1].misc == 1219);
  CHECK(r[2].channel == RadioChannel::kImmediate && r[2].delay_min_s == 2.5 && r[2].delay_max_s == 2.5);
}

void synthetic_names() {
  audio::RadioData d;
  std::string why;
  CHECK(audio::parse_radio(synthetic_system(), kMusicInfo, kVoInfo, d, &why));
  // Exact lookups, any case, in the map each channel reads.
  CHECK(audio::radio_has_event(d, RadioChannel::kSpecial, "barnfindspotted4"));
  CHECK(!audio::radio_has_event(d, RadioChannel::kFestivalUpdate, "BarnFindSpotted4"));
  CHECK(!audio::radio_has_event(d, RadioChannel::kSpecial, "BarnFindRumor"));
  CHECK(!audio::radio_has_event(d, RadioChannel::kFestivalUpdate, "PopularityRankUp"));
  // The schedule queues what the maps hold and refuses the rest.
  audio::StationSchedule sc(d, 0, 1, 0.0);
  CHECK(sc.queue_special_dialogue("BarnFindLocationHint1"));
  CHECK(!sc.queue_special_dialogue("BarnFindRumor"));
  CHECK(sc.queue_festival_update("FestivalUpdateSkillLevel175"));

  // Every name the engine's own code sends.
  for (const auto& c : engine_literal_cues()) {
    const bool ok = audio::radio_has_event(d, c.channel, c.event);
    if (!ok) std::printf("  missing: %s\n", c.event.c_str());
    CHECK(ok);
  }
  // The rules' names for the seven wristbands: all present but the three
  // FestivalUpdateWristband lines the disc comments out.
  audio::RadioDjRules rules;
  CHECK(audio::parse_radio_dj_rules(kRules, rules, &why));
  std::set<std::string> missing;
  for (const auto& c : audio::all_wristband_cues(rules, 7))
    if (!audio::radio_has_event(d, c.channel, c.event)) missing.insert(c.event);
  CHECK((missing == std::set<std::string>{"FestivalUpdateWristband2", "FestivalUpdateWristband5",
                                          "FestivalUpdateWristband6"}));
}

int corpus(const char* root) {
  audio::RadioData d;
  std::string why;
  if (!audio::load_radio(root, d, &why)) {
    std::printf("FAIL load_radio: %s\n", why.c_str());
    return 1;
  }
  int checked = 0;
  auto expect = [&](const RadioDjCue& c, const char* from) {
    ++checked;
    if (!audio::radio_has_event(d, c.channel, c.event)) {
      std::printf("FAIL %s: %s is not in RadioSystem.xml's %s map\n", from, c.event.c_str(),
                  c.channel == RadioChannel::kSpecial     ? "special"
                  : c.channel == RadioChannel::kImmediate ? "immediate"
                                                          : "festival update");
      ++failures;
    }
  };
  // The parsed maps hold the disc's counts (comments skipped).
  size_t fu = d.festival_updates.size();
  CHECK(fu == disc_festival_updates().size());
  for (const auto& e : disc_festival_updates()) CHECK(audio::radio_has_event(d, RadioChannel::kFestivalUpdate, e));
  for (const auto& e : disc_specials()) CHECK(audio::radio_has_event(d, RadioChannel::kSpecial, e));
  for (const auto& e : disc_immediates()) CHECK(audio::radio_has_event(d, RadioChannel::kImmediate, e));

  for (const auto& c : engine_literal_cues()) expect(c, "engine");

  // radiodj.xml.
  audio::RadioDjRules rules;
  CHECK(audio::load_radio_dj_rules(root, rules, &why));
  CHECK(rules.festival_wristband.size() == 17 && rules.special_wristband.size() == 1);
  std::set<std::string> missing;
  for (const auto& c : audio::all_wristband_cues(rules, 7)) {
    ++checked;
    if (!audio::radio_has_event(d, c.channel, c.event)) missing.insert(c.event);
  }
  for (const auto& m : missing) std::printf("  radiodj.xml names %s, which RadioSystem.xml comments out\n", m.c_str());
  CHECK((missing == std::set<std::string>{"FestivalUpdateWristband2", "FestivalUpdateWristband5",
                                          "FestivalUpdateWristband6"}));

  // barnfinds.xml: each of the 9 barns asks for its own hint and sighting.
  freeroam::Collectibles col;
  CHECK(freeroam::load_collectibles(root, col, &why));
  CHECK(col.barns.size() == 9);
  std::set<std::string> hints, spotted;
  for (const auto& b : col.barns) {
    CHECK(b.radio_requests.size() == 2);
    for (const auto& r : b.radio_requests) {
      CHECK(!r.immediate);
      expect({RadioChannel::kSpecial, r.event}, b.name.c_str());
      if (r.event.rfind("BarnFindLocationHint", 0) == 0) {
        hints.insert(r.event);
        CHECK(r.enabled && r.delay_max_s == 0.0);
      } else if (r.event.rfind("BarnFindSpotted", 0) == 0) {
        spotted.insert(r.event);
        CHECK(!r.enabled && r.delay_min_s > 0.0 && r.delay_max_s >= r.delay_min_s);
      } else {
        CHECK(!"an unexpected barn request");
      }
    }
  }
  CHECK(hints.size() == 9 && spotted.size() == 9);

  // Fame.xml: the levels' festival updates.
  freeroam::SkillsData skills;
  CHECK(freeroam::load_skills_data(root, skills, &why));
  int fame = 0;
  for (const auto& lv : skills.fame_levels)
    if (!lv.radio_festival_update.empty()) {
      ++fame;
      expect({RadioChannel::kFestivalUpdate, lv.radio_festival_update}, "Fame.xml");
    }
  CHECK(fame == 8);

  // street_race_hubs.xml: one owner line per hub.
  race::CareerCatalog cat;
  if (race::load_career_catalog(root, cat, &why)) {
    int hub_lines = 0;
    for (const auto& h : cat.hubs)
      for (const auto& ev : h.radio_specials) {
        ++hub_lines;
        expect({RadioChannel::kSpecial, ev}, "street_race_hubs.xml");
      }
    CHECK(hub_lines == 3);
  } else {
    std::printf("FAIL load_career_catalog: %s\n", why.c_str());
    ++failures;
  }
  std::printf("radio_dj corpus: %d names checked\n", checked);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const bool want_corpus = argc > 1 && std::strcmp(argv[1], "--corpus") == 0;
  if (want_corpus) {
    const char* root = std::getenv("SHIFT_GAME_ROOT");
    if (!root || !*root) {
      std::printf("SHIFT_GAME_ROOT not set: skipped\n");
      return 77;
    }
    if (corpus(root)) return 1;
  } else {
    rules_and_tracker();
    requests();
    synthetic_names();
  }
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("radio_dj_tests%s: all passed\n", want_corpus ? " --corpus" : "");
  return failures ? 1 : 0;
}
