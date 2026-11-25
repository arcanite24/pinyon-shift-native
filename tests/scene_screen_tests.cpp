// Scene-driven screens against the player's game files (LABELS corpus;
// skipped, exit 77, without SHIFT_GAME_ROOT): the pause menu's scene
// (925_PAUSE_MENU) and the two screens it opens (H_SPONSOR_CHALLENGE_STATUS,
// 802_MESSAGE_CENTER) have every node the screens bind, every string they
// bind resolves in the EN string tables (SHIFT_STATE_ROOT adds v4 and the
// packages), the sponsor rows resolve from the game's sponsor data, and the
// navigation cues name non-blank ui4audio.xml events. With
// SHIFT_SCENE_SCREEN_SHOT_DIR the screens are also drawn to PNGs there, over
// SHIFT_SCENE_SCREEN_BACKDROP (a 1280x720 PNG) when given.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "audio/event_configs.h"
#include "cars/sources.h"
#include "cars/strings.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"
#include "fh1data/ui_overlay.h"
#include "fh1data/ui_scene.h"
#include "fh1data/xds.h"
#include "freeroam/skills.h"
#include "game_hud.h"
#include "hud_scene.h"
#include "car_list_screen.h"
#include "hub_menus.h"
#include "hud_screen.h"
#include "pause_screen.h"
#include "race_screens.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "ui.h"

namespace {

int g_failures = 0;
int g_checks = 0;
void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

using carview::PauseMenu;
using carview::SceneDraw;

int child(const SceneDraw& sd, int parent, std::string_view name) {
  if (parent < 0) return -1;
  for (int c : sd.children_of(parent))
    if (sd.object_name(c) == name) return c;
  return -1;
}

void check_nodes(const SceneDraw& sd, std::string_view scene, const std::vector<const char*>& paths) {
  for (const char* p : paths) {
    const std::string_view path(p);
    const int n = path.find('/') != std::string_view::npos ? sd.find_path(path) : sd.find(path);
    check(n >= 0, std::string(scene) + ": node " + p);
  }
}

void check_ref(const carview::StringRef& r, const std::string& what) {
  const auto t = carview::lookup(r);
  check(t.has_value() && !t->empty(), what + " resolves (" + std::string(r.table) + "/" +
                                          (r.id.empty() ? std::to_string(r.raw) : std::string(r.id)) + ")");
}

}  // namespace

int main() {
  const char* root_env = std::getenv("SHIFT_GAME_ROOT");
  if (!root_env || !*root_env) {
    std::printf("scene_screen_tests: SKIP (SHIFT_GAME_ROOT not set)\n");
    return 77;
  }
  const std::filesystem::path game_root = root_env;
  const char* state_env = std::getenv("SHIFT_STATE_ROOT");
  const std::filesystem::path state_root = state_env ? state_env : "";

  // Scenes as the game mounts them: v4's media.zip over the disc's UI.zip
  // (fh1::UiOverlay; v4 replaces 108, 115, 554, H_STATISTICS ...).
  check(std::filesystem::exists(game_root / "media" / "UI.zip"), "UI.zip exists");
  fh1::UiOverlay overlay(game_root, fh1::UiOverlay::v4_media_zip_of(state_root));
  std::map<std::string, std::unique_ptr<fh1::UiScene>> scenes;
  carview::set_ui_scenes([&](const std::string& name) -> const fh1::UiScene* {
    auto& slot = scenes[name];
    if (!slot) {
      auto sc = std::make_unique<fh1::UiScene>();
      std::string e;
      if (overlay.load_scene(name, *sc, &e)) slot = std::move(sc);
      else std::fprintf(stderr, "scene %s: %s\n", name.c_str(), e.c_str());
    }
    return slot.get();
  });

  carview::install_scene_help_bar();

  // EN string tables (disc, v4, packages).
  cars::Install inst;
  std::string why;
  check(cars::discover_install(game_root, state_root, {}, inst, &why), "discover_install: " + why);
  fh1::StringTables tables;
  check(cars::load_string_tables(inst, tables, &why, "EN"), "EN string tables load");
  carview::set_game_text([&](const std::string& v) -> std::string {
    if (auto t = tables.resolve(v)) return *t;
    return v;
  });

  // 1. The scenes and the nodes the screens bind.
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kPauseScene)));
    check(sd.ok(), "925_PAUSE_MENU loads");
    for (int i = 0; i < 7; ++i) {
      const int btn = sd.find("Button" + std::to_string(i));
      check(btn >= 0, "925_PAUSE_MENU: Button" + std::to_string(i));
      const int body = child(sd, btn, "Button");
      for (const char* c : {"Text", "Dirt_Tile", "Crisp_Tape", "Smudge_Tape", "Clipon"})
        check(child(sd, body, c) >= 0, "925_PAUSE_MENU: Button" + std::to_string(i) + "/Button/" + c);
    }
    check_nodes(sd, "925_PAUSE_MENU",
                {"Option_List/Option_List", "Layer2", "TEXT_SCREEN_TITLE", "Next_Level", "Points", "Wristband",
                 "TEXT_CAR", "TEXT_PI", "CLASS_A", "CLASS_SM", "TEXT_CR", "BottomRow", "BG_Noise"});
  }
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kSponsorScene)));
    check(sd.ok(), "H_SPONSOR_CHALLENGE_STATUS loads");
    for (int i = 0; i < 6; ++i) {
      const std::string name = i == 0 ? "SponsorBar" : "SponsorBar" + std::to_string(i + 1);
      const int bar = sd.find(name);
      check(bar >= 0, "H_SPONSOR_CHALLENGE_STATUS: " + name);
      const int detail = child(sd, bar, "SponsorDetail"), rank = child(sd, bar, "Rank");
      const int pb = child(sd, bar, "ProgressBar");
      check(child(sd, detail, "TEXT_SPONSOR") >= 0 && child(sd, detail, "TEXT_TITLE") >= 0,
            name + ": SponsorDetail texts");
      check(child(sd, rank, "TEXT_TITLE") >= 0, name + ": Rank/TEXT_TITLE");
      check(child(sd, pb, "middle") >= 0 && child(sd, pb, "TEXT_PROGRESS") >= 0 && child(sd, pb, "Group") >= 0,
            name + ": ProgressBar parts");
      check(child(sd, bar, "SponsorLogo") >= 0, name + ": SponsorLogo");
      for (const char* pane : {"Sponsor_Back", "Progress_Back"})
        check(child(sd, child(sd, bar, pane), "BigPane01") >= 0, name + ": " + pane + "/BigPane01");
    }
    check_nodes(sd, "H_SPONSOR_CHALLENGE_STATUS",
                {"TitleTapeText", "TEXT_POSITION_TITLE", "TEXT_POSITION", "TEXT_TH", "TEXT_DETAILS_TITLE",
                 "RewardTitle/TEXT_CAR_NAME", "Reward/TEXT_CAR_NAME", "TEXT_CHALLENGE", "ButtonBar",
                 "NAV_POSITION_INDICATOR"});
  }
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kMessageScene)));
    check(sd.ok(), "802_MESSAGE_CENTER loads");
    check_nodes(sd, "802_MESSAGE_CENTER",
                {"TitleTapeText", "TEXT_MESSAGE_TITLE", "TEXT_MESSAGE", "MESSAGE_MENU", "LOADING_ICON", "HELP_BUTTON_BAR",
                 "NAV_POSITION_INDICATOR"});
  }

  {
    // The driving HUD's bound nodes (hud_screen.cpp), by element.
    const SceneDraw sd(carview::ui_scene(std::string(carview::kHudScene)));
    check(sd.ok(), "947_HUD loads");
    check_nodes(sd, "947_HUD",
                {// The dial.
                 "GAUGE_ANALOG", "GAUGE_ANALOG/ASSIST_ICONS", "ASSIST_ICONS/HUD_SHIFT_LIGHT", "GAUGE_ANALOG/FUEL_ANALOG",
                 "GAUGE_ANALOG/RPM_ANALOG", "RPM_ANALOG/Elements", "Elements/_P_Tach_Needle", "Elements/Large_Hatches",
                 "Elements/Mid_Hatches", "Elements/Small_Hatches", "Elements/Numbers", "Elements/RPM_and_MPH",
                 "_P_Tach_Needle/Group/Rectangle", "_P_Tach_Needle/Group/Swooshes", "Swooshes/Down/DownTrail",
                 "Swooshes/Up/Uptrail", "GAUGE_ANALOG/Speedo", "Speedo/Group/digit0", "GAUGE_ANALOG/TEXT_GEAR_ANALOG",
                 "GAUGE_ANALOG/TEXT_SPEED_UNIT_ANALOG", "Rectangle_ON", "Rectangle_OFF", "Glow",
                 // The race blocks.
                 "EndConditionBlock/TEXT_ENDCON_CURRENT", "EndConditionBlock/TEXT_ENDCON_FINAL",
                 "EndConditionBlock/TEXT_ENDCON_SLASH", "EndConditionBlock/TEXT_ENDCON_UNITS", "PlaceBlock/TEXT_PLACE",
                 "PlaceBlock/TEXT_PLACE_LABEL", "PlaceBlock/TEXT_PLACE_MAX", "PlaceBlock/TEXT_PLACE_SLASH",
                 "HUDTimes/LINE1", "LINE1/TEXT_TITLE_1", "LINE1/value/TEXT_VALUE_1", "PositionIndicators",
                 "TEXT_GAMERTAG", "TEXT_POSITION",
                 // The skill chain and the popularity disc.
                 "DrivingSkill/CompleteComponent/ScoreGroup/anim_helper", "CompleteComponent/SkillGroup/Skill1",
                 "Skill1/anim_helper/Skill", "SkillGroup/Skill2", "Total/anim_helper/TEXT_TOTAL", "Multiplier",
                 "anim_helper/TEXT_MULTIPLIER", "TEXT_X", "Smudge_Tape", "TEXT_POP_SKILLNAME", "SkillValue",
                 "ComboIcon", "PopularityBar", "PopularityBar/ValueScale/Stars", "ValueScale/ValueType",
                 "ValueType/TEXT_LEVEL", "ValueType/TEXT_LEVEL_SUFFIX", "CircleFlash", "Flash_SmokeRing",
                 "Flash_SmokeRing2",
                 // The event prompt.
                 "ActivationPrompt/All", "All/Title", "All/Event_Name", "All/Call_to_Action", "Call_to_Action/Default",
                 "Call_to_Action/Locked",
                 // The notifications.
                 "CombinedNotification/State/DoubleTape", "DoubleTape/RankIcon/Bar/Text", "DoubleTape/RankIcon/Bar/Bar",
                 "DoubleTape/Icons/MiscIcon", "DoubleTape/Icons/MailIcon", "Icons/SponsorScaler/Sponsor",
                 "DoubleTape/TEXT1", "DoubleTape/TEXT2", "DoubleTape/TEXT3", "DoubleTape/Panel", "DoubleTape/Panel2",
                 "PopularityUnlock/MainMessage/TEXT_TITLE", "MainMessage/Crisp_Tape", "PopularityUnlock/ShowMe/TEXT_SHOW",
                 // The start and the wrong way sign.
                 "I_321_Go/Group/GO", "I_321_Go/Group/TEXT_COUNT", "I_321_Go/Group/Paused", "WrongWay/Sign_Top"});
  }
  {
    // The help bar's buttons (help_bar_screen.cpp).
    const SceneDraw sd(carview::ui_scene(std::string(carview::kButtonBarScene)));
    check(sd.ok(), "BUTTON_BAR_SCENE loads");
    const int buttons = sd.find_path("ButtonBar/Buttons");
    check(buttons >= 0, "BUTTON_BAR_SCENE: node ButtonBar/Buttons");
    for (int k = 0; k < 7 && buttons >= 0; ++k) {
      const std::string name = "HelpButton" + std::to_string(k);
      const int b = sd.find(name, buttons);
      check(b >= 0, "BUTTON_BAR_SCENE: node " + name);
      if (b < 0) continue;
      for (const char* part : {"Text_BUTTON", "Pill", "BUTTON_LOGO"})
        check(sd.find(part, b) >= 0, "BUTTON_BAR_SCENE: node " + name + "/" + part);
    }
  }

  // The race screens' scenes (race_screens.cpp).
  for (const auto& [scene, paths] : carview::race_screen_nodes()) {
    const SceneDraw sd(carview::ui_scene(std::string(scene)));
    check(sd.ok(), std::string(scene) + " loads");
    if (sd.ok()) check_nodes(sd, scene, paths);
  }

  // The generic menus' scene (hub_menus.cpp binds the race grid menu's rows).
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kGenericMenuScene)));
    check(sd.ok(), "H_GENERIC loads");
    if (sd.ok()) {
      check_nodes(sd, carview::kGenericMenuScene, {"Option_List", "ButtonBar"});
      for (int i = 0; i < 7; ++i) {
        const std::string b = "Button" + std::to_string(i);
        const int btn = sd.find(b);
        check(btn >= 0, "H_GENERIC: " + b);
      }
    }
  }

  // The manufacturer select (115_C_BUY_MFRSELECT, v4's).
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kMakeListScene)));
    check(sd.ok(), "115_C_BUY_MFRSELECT loads");
    if (sd.ok()) {
      check_nodes(sd, carview::kMakeListScene, carview::make_list_nodes());
      // The cursor sits on the fourth slot, as on 108.
      const auto cur = sd.at(sd.find_path("MANUFACTURER_LIST/HIGHLIGHT/CursorBig"));
      const auto slot = sd.at(sd.find("CAR_LIST_ITEM_" + std::to_string(carview::kCarListCursorSlot)));
      check(std::fabs(cur[0] - slot[0]) < 1.0f && std::fabs(cur[1] - slot[1]) < 1.0f, "115: the cursor on slot 3");
    }
  }

  // The car list (108_C_BUY_CARSELECT, v4's).
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kCarListScene)));
    check(sd.ok(), "108_C_BUY_CARSELECT loads");
    if (sd.ok()) {
      check_nodes(sd, carview::kCarListScene, carview::car_list_nodes());
      for (int k = 0; k < carview::kCarListSlots; ++k)
        check(sd.find("CAR_LIST_ITEM_" + std::to_string(k)) >= 0, "108: CAR_LIST_ITEM_" + std::to_string(k));
      // The cursor sits on the fourth slot.
      const auto cur = sd.at(sd.find_path("CAR_LIST/HIGHLIGHT/CursorBig"));
      const auto slot = sd.at(sd.find("CAR_LIST_ITEM_" + std::to_string(carview::kCarListCursorSlot)));
      check(std::abs(cur[0] - slot[0]) < 1 && std::abs(cur[1] - slot[1]) < 1, "108: the cursor on slot 3");
    }
  }

  // 2. Every string the screens bind resolves in the EN tables.
  {
    using namespace carview::race_text;
    for (const carview::StringRef* r :
         {&kEnterEvent, &kBack, &kRestrictions, &kSelect, &kContinue, &kRestart, &kRaceResults, &kViewReplay, &kYes,
          &kNo, &kStartRace, &kDifficulty, &kOptions, &kQuitRace, &kFestivalRace, &kStreetRace, &kShowcaseRace,
          &kHeadlineRace, &kStarShowdown, &kCircuit, &kPointToPoint, &kAsphalt, &kMixedSurface, &kOffRoad,
          &kTwoWords, &kPointsAvailable, &kEntryRestrictions, &kClassOrBelow, &kCarProvided, &kBestFinish, &kBeat,
          &kReward, &kCurrentCarEligible, &kGarageCarsEligible, &kNoCarsEligible, &kPts, &kNoRestrictions,
          &kCredits, &kFinished, &kCashReward, &kFinishedPlace, &kMileageBonus, &kDifficultyBonus, &kTotal,
          &kBalance, &kPlace, &kPointsEarned, &kPointsN, &kCollectIt, &kWristbandUnlocked, &kPointsToNext,
          &kPointToNext, &kPleaseWait, &kYouWonACar, &kStayInCar, &kStayInCarQ, &kCarSelectTitle})
      check_ref(*r, "race screen " + std::string(r->fallback));
    std::vector<std::string> rows;
    for (auto g : carview::kGridRows) rows.push_back(carview::text_of(carview::grid_row_text(g)));
    check((rows == std::vector<std::string>{"START RACE", "DIFFICULTY", "OPTIONS", "QUIT RACE"}),
          "grid menu rows read START RACE .. QUIT RACE from the tables");
    check(carview::event_track_info(0, false) == "ASPHALT POINT-TO-POINT",
          "The Gauntlet's track line: " + carview::event_track_info(0, false));
    check(carview::event_ticket_texture(1, "043") == "EVENTTICKETS/FESTIVALRACES/043", "event 43's ticket");
  }
  {
    // The generic menus (MY PROFILE, OPTIONS, the paint booth, the workshop,
    // photo mode) read their rows from the tables.
    using namespace carview::hub_menu_text;
    for (const carview::StringRef* r :
         {&kOptions, &kStats, &kAchievements, &kTeamCredits, &kControls, &kDifficulty, &kHud, &kAudio, &kVideo,
          &kPaintCar, &kAddDecals, &kCreateVinylGroup, &kMyVinylGroups, &kFm4VinylGroups, &kClearDesign, &kChangeCar,
          &kUpgradeCar, &kWorkshopPaint, &kBarnFinds, &kAutoUpgrade, &kCustomUpgrade, &kTakePhoto, &kViewPhoto,
          &kPhotoShare, &kSelect, &kBack, &kAdvanced})
      check_ref(*r, "hub menu " + std::string(r->fallback));
    auto rows_of = [](carview::HubMenu m) {
      std::vector<std::string> out;
      for (const auto& r : carview::hub_menu_rows(m)) out.push_back(carview::text_of(r.text));
      return out;
    };
    using HM = carview::HubMenu;
    using SV = std::vector<std::string>;
    check(rows_of(HM::kProfile) == SV{"OPTIONS", "STATS", "ACHIEVEMENTS", "TEAM CREDITS"},
          "MY PROFILE rows (21_my_profile_menu_orig.png)");
    check(rows_of(HM::kOptions) == SV{"CONTROLS", "DIFFICULTY", "HUD", "AUDIO", "VIDEO"},
          "OPTIONS rows (22_options_menu_orig.png)");
    check(rows_of(HM::kPaintBooth) ==
              SV{"PAINT CAR", "ADD DECALS", "CREATE VINYL GROUP", "MY VINYL GROUPS", "FM4 VINYL GROUPS", "CLEAR DESIGN"},
          "paint booth rows (20_paint_shop_orig.png)");
    check(rows_of(HM::kPhotoSelect) == SV{"TAKE PHOTO", "VIEW PHOTO", "PHOTO SHARE"},
          "photo mode rows (31_photo_mode_menu_orig.png)");
    for (const auto& r : rows_of(HM::kWorkshop)) check(r.rfind("IDS_", 0) != 0, "workshop row resolves: " + r);
    // The stack: OPTIONS from MY PROFILE, B back to it, B again closes.
    carview::HubMenuStack hs;
    hs.show(HM::kProfile);
    carview::HubMenuAction act{};
    check(hs.action(act) && act == carview::HubMenuAction::kOptions, "MY PROFILE opens on OPTIONS");
    hs.push(HM::kOptions);
    hs.move(-1);
    check(hs.selected() == 4, "OPTIONS wraps up to VIDEO");
    check(hs.back() && hs.top() == HM::kProfile, "B returns to MY PROFILE");
    check(!hs.back() && !hs.open(), "B on MY PROFILE closes the list");
    carview::HubMenuHud hh;
    hs.show(HM::kOptions);
    carview::fill_hub_menu(hs, hh);
    check(hh.prompts.size() == 3 && hh.prompts[0].second == "SELECT" && hh.prompts[1].second == "BACK",
          "OPTIONS help bar: A SELECT, B BACK, X the engine's settings");
  }
  {
    using namespace carview::car_list_text;
    for (const carview::StringRef* r :
         {&kCarSelect, &kHorizonRecommends, &kManufacturerSelect, &kMyGarage, &kBarnFinds, &kTradeIn, &kSpeed,
          &kHandling, &kAcceleration, &kLaunch, &kBraking, &kSelect, &kBack, &kToggle, &kSearch, &kSellCar, &kCredits,
          &kBarnLocked, &kBarnWrecked, &kBarnReady, &kBarnCollected, &kCollectTitle, &kCollectMessage, &kWreckedTitle,
          &kWreckedMessage, &kLockedTitle, &kLockedMessage, &kCantSellTitle, &kCantSellMessage, &kViewOnMap, &kOk})
      check_ref(*r, "car list " + std::string(r->fallback));
    check(carview::text_of(kHorizonRecommends) == "HORIZON RECOMMENDS", "19's title from the tables");
    check(carview::car_list_credits(120000) == "120,000 Cr", "the cursor's price: " + carview::car_list_credits(120000));
  }
  {
    // The driving HUD's labels and every notification's lines.
    using namespace carview::hud_text;
    for (const carview::StringRef* r :
         {&kKmh, &kMph, &kEnterNow, &kSignMeUp, &kGo, &kProgress, &kPlace, &kLaps, &kRace, &kLap, &kLast, &kBest})
      check_ref(*r, "HUD label " + std::string(r->fallback));
    for (const auto& n : carview::notify_strings())
      for (const carview::StringRef* r : {&n.tag, &n.title, &n.message})
        if (!r->table.empty()) check_ref(*r, "notification line " + std::string(r->fallback));
  }
  for (const char* id : {"IDS_HudOption_Timers", "IDS_HudOption_WrongWay", "IDS_HudOption_Names",
                         "IDS_HudOption_PopularitySkills"})
    check_ref({"InGame", id, 0, id}, std::string("HUD toggle ") + id);
  {
    PauseMenu pm;
    for (bool race : {false, true}) {
      pm.show(race);
      for (PauseMenu::Action a : pm.items()) {
        const carview::StringRef r = carview::pause_row_text(a, race);
        check_ref(r, std::string(race ? "race" : "free roam") + " row " + std::string(r.fallback));
      }
    }
    using namespace carview::pause_text;
    for (const carview::StringRef* r :
         {&kSelect, &kResume, &kBack, &kCollectWristband, &kCredits, &kSponsorTitle, &kPopularityChart, &kRank,
          &kReward, &kDetails, &kChallengeCompleted, &kMessageTitle, &kNoMessages, &kYouHaveNoMessages,
          &kCheckBackLater})
      check_ref(*r, std::string(r->fallback));
    // The free roam rows read as the original's (the SETTINGS slot's own text
    // is the retail MULTIPLAYER).
    pm.show(false);
    carview::HudState st;
    pm.fill(st);
    check((st.menu == std::vector<std::string>{"MAP", "SETTINGS", "PHOTO MODE", "MESSAGE CENTER", "SPONSOR CHALLENGES",
                                               "MY PROFILE", "QUIT"}),
          "free roam rows read MAP .. QUIT from the tables");
  }

  // 3. The sponsor rows from the game's sponsor data.
  freeroam::SkillsData skills;
  check(freeroam::load_skills_data(game_root, skills, &why), "skills data loads: " + why);
  std::map<std::string, freeroam::SponsorProgress> progress;
  progress["Air"] = {102, 2};
  const auto rows = carview::sponsor_challenge_rows(skills, &progress, 166, 3);
  check(rows.size() >= 6, "at least six sponsor challenges");
  for (const auto& r : rows) {
    check(!r.sponsor_name.empty() && r.sponsor_name.rfind("IDS_", 0) != 0, "sponsor name resolves: " + r.sponsor_name);
    check(!r.challenge_title.empty() && r.challenge_title.rfind("IDS_", 0) != 0,
          "challenge name resolves: " + r.challenge_title);
    check(!r.description.empty() && r.description.find("{0}") == std::string::npos,
          "details resolve with the target: " + r.challenge_title);
    check(r.logo_texture.rfind("SPONSORLOGOTILE/", 0) == 0, "logo texture: " + r.challenge_title);
  }
  if (!rows.empty()) {
    check(rows[0].fame && rows[0].current == 166 && rows[0].tier == 4 && rows[0].target == 150,
          "popularity challenge: chart 166th, rank 4 (150th)");
    check(rows[0].description.find("150TH") != std::string::npos, "popularity details name the 150TH: " + rows[0].description);
  }
  for (const auto& r : rows)
    if (r.challenge_title == "AIR CHALLENGE")
      check(r.current == 102 && r.tier == 3 && r.target == 150, "AIR CHALLENGE: 102/150, rank 3");

  // 4. The navigation cues name non-blank ui4audio.xml events.
  {
    audio::UiAudioMap map;
    check(map.load(game_root, &why), "ui4audio.xml loads: " + why);
    for (auto e : {carview::UiEvent::kOpen, carview::UiEvent::kClose, carview::UiEvent::kMove, carview::UiEvent::kAccept,
                   carview::UiEvent::kBack}) {
      const std::string_view play = carview::ui_play_name(e);
      const auto* entry = map.resolve(play, carview::kPauseScene);
      check(entry && !entry->is_blank(), "cue " + std::string(play) + " plays a sound");
    }
  }

  // 5. Optional renders.
  if (const char* shot_env = std::getenv("SHIFT_SCENE_SCREEN_SHOT_DIR"); shot_env && *shot_env) {
    const std::filesystem::path out_dir = shot_env;
    std::filesystem::create_directories(out_dir);
    if (auto fzip = fh1::Archive::open(game_root / "media" / "ui" / "Fonts.zip", nullptr))
      carview::ui::load_fonts_from_zip(*fzip);
    std::set<std::string> failed;
    carview::set_ui_textures([&](const std::string& path, std::array<float, 4>& uv) -> bool {
      if (carview::ui::atlas_find(path, uv)) return true;
      if (failed.count(path)) return false;
      const fh1::UiOverlay::File f = overlay.texture(path);
      const auto x = f.ok() ? fh1::decode_xds(f.read.data, false) : fh1::XdsResult{};
      if (x.levels.empty() || !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
        failed.insert(path);
        return false;
      }
      return true;
    });
    constexpr uint32_t W = 1280, H = 720;
    std::vector<uint8_t> backdrop(size_t(W) * H * 4, 0);
    for (size_t i = 0; i < backdrop.size(); i += 4) backdrop[i] = 70, backdrop[i + 1] = 92, backdrop[i + 2] = 64, backdrop[i + 3] = 255;
    if (const char* bg = std::getenv("SHIFT_SCENE_SCREEN_BACKDROP"); bg && *bg) {
      int w = 0, h = 0, n = 0;
      if (uint8_t* px = stbi_load(bg, &w, &h, &n, 4)) {
        if (w == int(W) && h == int(H)) backdrop.assign(px, px + size_t(W) * H * 4);
        stbi_image_free(px);
      }
    }
    carview::DriveHud d;
    d.driving = false;
    d.pause = true;
    d.menu_title = "paused";
    d.band_name = "GREEN WRISTBAND";
    d.band_next = "BLUE";
    d.band_level = 3;
    d.band_collect_ready = true;
    d.car_name = "Mazda MX-5 Miata";
    d.car_class = "A";
    d.car_pi = 600;
    d.credits = 167700;
    d.popularity_rank = 166;
    auto shot = [&](const std::string& file, PauseMenu& pm, bool race) {
      carview::PauseScreenHud ps;
      carview::fill_pause_screen(pm, 1e9, ps);
      ps.in_race = race;
      ps.sponsors = &rows;
      d.pause_screen = &ps;
      carview::ui::List l;
      carview::build_drive_hud(l, d, float(W), float(H));
      std::vector<uint8_t> fb = backdrop;
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / file;
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write " + file);
    };
    PauseMenu pm;
    pm.show(false);
    shot("pause_roam.png", pm, false);
    pm.move(2);
    shot("pause_roam_photo.png", pm, false);
    pm.open_extras();
    shot("pause_extras.png", pm, false);
    pm.back();
    pm.open_screen(PauseMenu::Screen::kSponsorChallenges);
    shot("sponsor_challenges.png", pm, false);
    pm.back();
    pm.open_screen(PauseMenu::Screen::kMessageCentre);
    shot("message_centre.png", pm, false);
    pm.show(true);
    shot("pause_race.png", pm, true);

    // The driving HUD, over the original's captures when
    // SHIFT_SCENE_SCREEN_HUD_DIR names their folder (the calibration loop:
    // the engine's HUD drawn on the original's).
    const char* hud_env = std::getenv("SHIFT_SCENE_SCREEN_HUD_DIR");
    const std::filesystem::path hud_dir = hud_env ? hud_env : "";
    auto hud_shot = [&](const std::string& file, const std::string& orig, const carview::DriveHud& hd) {
      std::vector<uint8_t> fb = backdrop;
      if (!hud_dir.empty()) {
        int w = 0, h = 0, n = 0;
        const std::filesystem::path src = std::filesystem::path(orig).is_absolute() ? orig : hud_dir / orig;
        if (uint8_t* px = stbi_load(src.string().c_str(), &w, &h, &n, 4)) {
          if (w == int(W) && h == int(H)) fb.assign(px, px + size_t(W) * H * 4);
          stbi_image_free(px);
        }
      }
      carview::ui::List l;
      carview::build_drive_hud(l, hd, float(W), float(H));
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / file;
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write " + file);
    };
    carview::DriveHud roam;
    roam.driving = true;
    roam.time = 10.0;
    roam.kmh = 57;
    roam.rpm = 6900;
    roam.redline_rpm = 7000;
    roam.gear = 3;
    roam.abs_enabled = roam.tcs_enabled = true;
    hud_shot("hud_roam.png", "05_roam_hud_day_drive_orig.png", roam);
    carview::DriveHud r34 = roam;
    r34.kmh = 0;
    r34.rpm = 900;
    r34.redline_rpm = 8000;
    r34.gear = 1;
    hud_shot("hud_roam_r34.png", "15_countdown_orig.png", r34);

    // The event prompt in an activation zone (09_event_marker_prompt_orig.png).
    carview::WorldMarkerHud wm;
    wm.visible = wm.in_range = true;
    wm.sponsor = carview::event_sponsor(43);
    check(wm.sponsor == "HORIZON presents", "Events/IDS_Sponsor_43 reads HORIZON presents: " + wm.sponsor);
    wm.title = carview::event_name_without_sponsor(carview::text_of({"Events", "IDS_Name_43", 0, "?"}), wm.sponsor);
    check(wm.title == "The Gauntlet", "event 43's name without its sponsor: " + wm.title);
    wm.prompt = carview::text_of(carview::hud_text::kSignMeUp);
    wm.icon = std::getenv("SHIFT_HUD_PROMPT_ICON") ? std::getenv("SHIFT_HUD_PROMPT_ICON") : "ICONS/ACTIVATIONZONEICONS/P2P2";
    carview::DriveHud prompt_d = roam;
    prompt_d.kmh = 0, prompt_d.rpm = 900, prompt_d.gear = 1;
    prompt_d.world_marker = &wm;
    hud_shot("hud_prompt.png", "09_event_marker_prompt_orig.png", prompt_d);

    // A sprint (16_race_hud_orig.png) and a circuit (the oracle's
    // race-hud-notification.png, by absolute path in SHIFT_SCENE_SCREEN_ORACLE).
    carview::RaceHud sprint;
    sprint.phase = carview::RaceHud::kRacing;
    sprint.place = 8, sprint.cars = 8, sprint.clock = 16.851, sprint.progress = 0.0;
    const char* names[] = {"Peter Houston", "Ana Ruiz", "Tom Hale", "Lee Park", "Sam Cole", "Josh Pollock",
                           "Modesto Estrada", "User"};
    for (int i = 0; i < 8; ++i) {
      carview::RaceHudRow row;
      row.name = names[i], row.place = i + 1, row.player = i == 7;
      sprint.rows.push_back(row);
    }
    carview::DriveHud race_d = roam;
    race_d.kmh = 73, race_d.rpm = 5200, race_d.gear = 2;
    race_d.race = &sprint;
    carview::SkillChainHud sprint_sk;
    sprint_sk.active = true, sprint_sk.chain_points = 250, sprint_sk.multiplier_f = 1.1f;
    sprint_sk.last_skill = "GREAT ONE EIGHTY", sprint_sk.last_points = 150, sprint_sk.last_skill_at = 9.5;
    race_d.skills = &sprint_sk;
    hud_shot("hud_race.png", "16_race_hud_orig.png", race_d);
    // The start signal (15b_countdown_go_orig.png): GO! alone over the road;
    // and the wrong way sign.
    {
      carview::RaceHud go = sprint;
      go.place = 7, go.clock = 0.103;
      carview::DriveHud go_d = race_d;
      go_d.kmh = 0, go_d.rpm = 1000, go_d.gear = 1, go_d.skills = nullptr;
      go_d.race = &go;
      hud_shot("hud_race_go.png", "15b_countdown_go_orig.png", go_d);
      // The interim count (SPEC_QUESTIONS Q74) at 2.
      go.phase = carview::RaceHud::kCountdown, go.clock = -1.6, go.digit = 2;
      hud_shot("hud_race_count.png", "15_countdown_orig.png", go_d);
      go.phase = carview::RaceHud::kRacing;
      go.clock = 20.0, go.wrong_way = true;
      hud_shot("hud_race_wrong_way.png", "16_race_hud_orig.png", go_d);
    }
    carview::RaceHud circuit;
    circuit.phase = carview::RaceHud::kRacing;
    circuit.circuit = true, circuit.lap = 4, circuit.laps = 4, circuit.place = 4, circuit.cars = 8;
    circuit.clock = 164.698, circuit.lap_time = 39.697, circuit.last_lap = 40.336, circuit.best_lap = 40.336;
    const char* cnames[] = {"Alan Brewer", "Kim Novak", "Peter Bankov", "User", "Don Funk", "Ira Bell", "Joe Kerr",
                            "Max Lane"};
    for (int i = 0; i < 8; ++i) {
      carview::RaceHudRow row;
      row.name = cnames[i], row.place = i + 1, row.player = i == 3;
      circuit.rows.push_back(row);
    }
    carview::DriveHud circuit_d = r34;
    circuit_d.kmh = 143, circuit_d.rpm = 7600, circuit_d.gear = 3;
    circuit_d.race = &circuit;
    carview::SkillChainHud circuit_sk;
    circuit_sk.active = true, circuit_sk.chain_points = 583, circuit_sk.multiplier_f = 1.4f;
    circuit_sk.last_skill = "GREAT DRAFTING", circuit_sk.last_points = 150, circuit_sk.last_skill_at = 9.5;
    circuit_d.skills = &circuit_sk;
    // The oracle's sponsor notification: RANK 4 UNLOCKED! over the reward and
    // the drafting challenge, with its sponsor's logo.
    {
      const auto t = carview::notify_text(carview::NotifyKind::kSponsorRank, {"4"}, {"+15,000"}, {});
      check(t.tag == "RANK 4 UNLOCKED!" && t.title == "+15,000 Cr", "sponsor notification lines: " + t.tag + " / " + t.title);
      circuit_d.popup_kind = carview::NotifyKind::kSponsorRank, circuit_d.popup_icon = carview::NotifyIcon::kSponsor;
      circuit_d.popup_tag = t.tag, circuit_d.popup_title = t.title;
      circuit_d.popup_at = circuit_d.time - 1.5;
      for (size_t i = 0; i < rows.size() && i < skills.sponsor_challenges.size(); ++i)
        if (skills.sponsor_challenges[i].id == "Draft")
          circuit_d.popup_text = rows[i].challenge_title, circuit_d.popup_logo = rows[i].logo_texture;
      check(circuit_d.popup_text == "DRAFTING CHALLENGE", "drafting challenge name: " + circuit_d.popup_text);
    }
    const char* oracle = std::getenv("SHIFT_SCENE_SCREEN_ORACLE");
    hud_shot("hud_race_circuit.png", oracle ? std::string(oracle) + "/race-hud-notification.png" : "", circuit_d);
    circuit_d.popup_title.clear();
    // A chain just banked: the total and the popularity disc.
    carview::SkillChainHud banked_sk;
    banked_sk.banked_total = 100, banked_sk.banked_at = 9.0, banked_sk.popularity_rank = 165;
    banked_sk.prev_popularity_rank = 165, banked_sk.pop_anim_progress = 0.3, banked_sk.pop_rank_anim_t = 1.0;
    circuit_d.skills = &banked_sk;
    circuit.lap = 1, circuit.place = 7, circuit.clock = 38.635, circuit.lap_time = 34.668;
    circuit.last_lap = circuit.best_lap = -1;
    hud_shot("hud_race_banked.png", oracle ? std::string(oracle) + "/race-hud-skill-banked.png" : "", circuit_d);
    // A free-roam notification (the discount signs) and the showcase banner.
    {
      carview::DriveHud n = roam;
      const auto t = carview::notify_text(carview::NotifyKind::kDiscountSigns, {}, {"12", "100"}, {"12%"});
      check(t.title == "12/100 DISCOUNT SIGNS SMASHED" && t.message == "Total upgrades discount: 12%",
            "discount sign lines: " + t.title + " / " + t.message);
      n.popup_kind = carview::NotifyKind::kDiscountSigns, n.popup_title = t.title, n.popup_text = t.message;
      n.popup_at = n.time - 1.5;
      hud_shot("hud_notify_signs.png", "05_roam_hud_day_drive_orig.png", n);
      const auto u = carview::notify_text(carview::NotifyKind::kShowcaseUnlocked, {}, {}, {});
      n.popup_kind = carview::NotifyKind::kShowcaseUnlocked, n.popup_title = u.title, n.popup_text = u.message;
      check(u.title == "SHOWCASE EVENT UNLOCKED!" && u.message == "Show me!", "showcase lines: " + u.title);
      hud_shot("hud_notify_showcase.png", "05_roam_hud_day_drive_orig.png", n);
    }

    // The race screens (race_screens.h) over the original's captures.
    {
      using S = carview::race_flow::Screen;
      carview::EventSummaryHud es;
      es.type = carview::event_type_text(1);
      es.ticket = carview::event_ticket_texture(1, "043");
      es.track = carview::text_of({"Events", "IDS_Name_43", 0, "?"}) == "" ? "" : "GLADSTONE CANYON RUN";
      es.track_info = carview::event_track_info(0, false);
      es.points = 110, es.credits = 6000, es.car_class = "A", es.restriction = "OPEN";
      es.eligibility = carview::text_of(carview::race_text::kCurrentCarEligible);
      carview::DriveHud sum_d = roam;
      sum_d.band_name = "GREEN WRISTBAND", sum_d.band_next = "BLUE", sum_d.band_collect_ready = true;
      sum_d.car_name = "Mazda MX-5 Miata", sum_d.car_class = "A", sum_d.car_pi = 600, sum_d.credits = 167700;
      sum_d.event_summary = &es;
      hud_shot("race_summary.png", "10_event_card_prerace_orig.png", sum_d);
      es.restrictions_view = true;
      hud_shot("race_summary_restrictions.png", "10_event_card_prerace_orig.png", sum_d);

      carview::RaceHud rr;
      rr.phase = carview::RaceHud::kGrid;
      rr.event_type = "FESTIVAL RACE";
      rr.title = "BBS JAPANESE TRAILBLAZERS";
      rr.screen = S::kTitleCard;
      carview::DriveHud rd = roam;
      rd.race = &rr;
      hud_shot("race_title_card.png", "12_race_intro_card_orig.png", rd);
      rr.title = "HORIZON PRESENTS THE GAUNTLET";
      rr.screen = S::kGridMenu, rr.screen_t = 5.0;
      for (auto g : carview::kGridRows) rr.grid_menu.push_back(carview::text_of(carview::grid_row_text(g)));
      hud_shot("race_grid_menu.png", "13_prerace_menu_orig.png", rd);
      rr.screen = S::kRivalIntro, rr.rival_name = "Ali Howard", rr.rival_reward = 1500;
      hud_shot("race_rival_intro.png", "14_rival_intro_orig.png", rd);
      rr.phase = carview::RaceHud::kFinished;
      rr.place = 5;
      rr.screen = S::kFinishBanner, rr.screen_t = 5.0;
      hud_shot("race_finished.png", "16b_finish_banner_orig.png", rd);
      rr.screen = S::kCashReward, rr.screen_t = 3.0;
      rr.base_credits = 0, rr.mileage_bonus = 400, rr.difficulty_pct = 40, rr.credits = 560, rr.balance = 118010;
      hud_shot("race_cash.png", "17_results_cash_reward_orig.png", rd);
      rr.screen = S::kPoints;
      rr.band_before = 2970, rr.band_after = 3080, rr.band_lo = 2000, rr.band_hi = 4100;
      rr.band_name = "ORANGE WRISTBAND", rr.band_next = "PURPLE";
      hud_shot("race_points.png", "17_results_cash_reward_orig.png", rd);
      rr.screen = S::kRewardCar, rr.won_car_name = "Nissan GT-R R34";
      hud_shot("race_won_car.png", "16b_finish_banner_orig.png", rd);
      rr.screen = S::kStayInCar, rr.stay_car = "Nissan GT-R R34";
      hud_shot("race_stay_in_car.png", "18_post_race_stay_in_car_orig.png", rd);
    }

    // The generic menus over the original's captures of them.
    auto menu_shot = [&](const std::string& file, const std::string& orig, carview::HubMenu m, int selected) {
      std::vector<uint8_t> fb = backdrop;
      if (!hud_dir.empty()) {
        int w = 0, h = 0, n = 0;
        if (uint8_t* px = stbi_load((hud_dir / orig).string().c_str(), &w, &h, &n, 4)) {
          if (w == int(W) && h == int(H)) fb.assign(px, px + size_t(W) * H * 4);
          stbi_image_free(px);
        }
      }
      carview::HubMenuStack hs;
      hs.show(m);
      hs.select(selected);
      carview::HubMenuHud hh;
      carview::fill_hub_menu(hs, hh);
      carview::ui::List l;
      check(carview::draw_hub_menu(l, hh, float(W), float(H)), "draw " + file);
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / file;
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write " + file);
    };
    menu_shot("menu_paint_booth.png", "20_paint_shop_orig.png", carview::HubMenu::kPaintBooth, 0);
    menu_shot("menu_my_profile.png", "21_my_profile_menu_orig.png", carview::HubMenu::kProfile, 0);
    menu_shot("menu_options.png", "22_options_menu_orig.png", carview::HubMenu::kOptions, 0);
    menu_shot("menu_photo.png", "31_photo_mode_menu_orig.png", carview::HubMenu::kPhotoSelect, 0);
    menu_shot("menu_workshop.png", "", carview::HubMenu::kWorkshop, 0);

    // The car list over 19_autoshow_orig.png: HORIZON RECOMMENDS on the
    // 2012 Jaguar XKR-S (the capture's own numbers).
    {
      carview::CarListHud cl;
      cl.title = carview::text_of(carview::car_list_text::kHorizonRecommends);
      cl.tiles = {{1009, carview::car_list_credits(120000)}, {1020, carview::car_list_credits(85000)},
                  {1103, carview::car_list_credits(150000)}};
      cl.selected = 0;
      cl.details.valid = true;
      cl.details.car_id = 1009;
      cl.details.model = "XKR-S";
      cl.details.year_make = "2012 Jaguar";
      cl.details.flag =
          carview::ui_texture_key(R"(GAME:\Media\UI\Textures\UI4\Logos\Country_Flags\L_FLA_UnitedKingdom_flag.tga)");
      check(cl.details.flag == "T:UI4/LOGOS/COUNTRY_FLAGS/L_FLA_UNITEDKINGDOM_FLAG", "flag key: " + cl.details.flag);
      cl.details.car_class = "A";
      cl.details.pi = 574;
      cl.details.drive_type = 2;
      cl.details.stats = {8.8, 8.4, 7.7, 6.6, 8.3};
      using namespace carview::car_list_text;
      cl.prompts = {{"A", carview::text_of(kSelect)}, {"B", carview::text_of(kBack)}, {"Y", carview::text_of(kToggle)},
                    {"BACK", carview::text_of(kSearch)}};
      std::vector<uint8_t> fb = backdrop;
      if (!hud_dir.empty()) {
        int w = 0, h = 0, n = 0;
        if (uint8_t* px = stbi_load((hud_dir / "19_autoshow_orig.png").string().c_str(), &w, &h, &n, 4)) {
          if (w == int(W) && h == int(H)) fb.assign(px, px + size_t(W) * H * 4);
          stbi_image_free(px);
        }
      }
      carview::ui::List l;
      check(carview::draw_car_list(l, d, cl, float(W), float(H)), "draw car list");
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / "car_list_recommends.png";
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write car list");
    }
    // The garage's lists on 108: MY GARAGE (no cursor line) and BARN FINDS
    // with a ready barn's COLLECT CAR popup.
    {
      using namespace carview::car_list_text;
      carview::CarListHud cl;
      cl.title = carview::text_of(kMyGarage);
      cl.tiles = {{1009, ""}, {1105, ""}, {1040, ""}, {251, ""}};
      cl.selected = 1;
      cl.details.valid = true;
      cl.details.car_id = 1105;
      cl.details.model = "DB5 Vantage";
      cl.details.year_make = "1964 Aston Martin";
      cl.details.car_class = "C";
      cl.details.pi = 446;
      cl.details.drive_type = 2;
      cl.details.stats = {5.6, 4.9, 4.4, 4.6, 3.9};
      cl.prompts = {{"A", carview::text_of(kSelect)}, {"B", carview::text_of(kBack)}, {"Y", carview::text_of(kToggle)}};
      std::vector<uint8_t> fb = backdrop;
      carview::ui::List l;
      check(carview::draw_car_list(l, d, cl, float(W), float(H)), "draw my garage");
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / "car_list_garage.png";
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write my garage");

      cl.title = carview::text_of(kBarnFinds);
      cl.tiles = {{1105, carview::text_of(kBarnReady), carview::CarTileState::kReady},
                  {1040, carview::text_of(kBarnWrecked), carview::CarTileState::kNotReady},
                  {251, carview::text_of(kBarnLocked), carview::CarTileState::kUndiscovered},
                  {1154, carview::text_of(kBarnCollected), carview::CarTileState::kNormal}};
      cl.selected = 0;
      cl.popup = {true, carview::text_of(kCollectTitle), carview::text_of(kCollectMessage),
                  {{"A", carview::text_of(kOk)}, {"B", carview::text_of(kBack)}}};
      check(cl.popup.title == "COLLECT CAR" && cl.popup.prompts[0].second == "OK", "COLLECT CAR popup's strings");
      std::vector<uint8_t> fb2 = backdrop;
      carview::ui::List l2;
      check(carview::draw_car_list(l2, d, cl, float(W), float(H)), "draw barn finds");
      check(l2.items().size() > l.items().size(), "the popup draws over the list");
      carview::ui::rasterize(l2.items(), fb2, W, H);
      const auto p2 = out_dir / "car_list_barn_finds.png";
      check(stbi_write_png(p2.string().c_str(), int(W), int(H), 4, fb2.data(), int(W * 4)) != 0,
            "write barn finds");
    }
    // The manufacturer select on 115: Aston Martin, Audi (the cursor),
    // Bentley, BMW and the engine's Hidden & Showcase tab, Audi's profile.
    {
      using namespace carview::car_list_text;
      carview::MakeListHud ml;
      ml.title = carview::text_of(kManufacturerSelect);
      check(ml.title == "MANUFACTURER SELECT", "115's title: " + ml.title);
      const char* logos[4] = {"AstonMartin", "Audi", "Bentley", "BMW"};
      for (int i = 0; i < 4; ++i) {
        carview::MakeTile t;
        t.id = 10 + i;
        t.logo = carview::ui_texture_key(std::string(R"(GAME:\Media\UI\Textures\UI4\Logos\Manufacturer_Logos_I\L_MAN_)") +
                                         logos[i] + "_Logo.tga");
        std::array<float, 4> uv{};
        check(carview::ui4::texture(t.logo, uv), std::string("115: the logo ") + t.logo);
        t.label = logos[i];
        ml.tiles.push_back(t);
      }
      ml.tiles.push_back({99, "", "Hidden & Showcase"});
      ml.selected = 1;
      ml.cursor = "AUDI";
      ml.available = carview::format1(carview::text_of(kAvailable), "12");
      check(ml.available == "12 AVAILABLE", "115's count: " + ml.available);
      // List_CarMake.Profile is "N/A" on the disc, so the info panel is
      // not shown.
      check(carview::game_text("_&857946860") == "N/A", "Audi's profile: " + carview::game_text("_&857946860"));
      ml.prompts = {{"A", carview::text_of(kSelect)}, {"B", carview::text_of(kBack)}, {"Y", carview::text_of(kAffordableCars)}};
      check(ml.prompts[2].second == "ALL AFFORDABLE CARS", "115's Y: " + ml.prompts[2].second);
      std::vector<uint8_t> fb = backdrop;
      carview::ui::List l;
      check(carview::draw_make_list(l, d, ml, float(W), float(H)), "draw make list");
      int hits = 0;
      for (const auto& z : l.hits())
        if (z.group == carview::ui::kHitRow) ++hits;
      check(hits == 5, "115: a hit per shown make (" + std::to_string(hits) + ")");
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / "make_list.png";
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write make list");
      // A profile shows the info panel, its text cut at the panel's foot.
      ml.info_title = "AUDI";
      ml.info_text.clear();
      for (int i = 0; i < 40; ++i) ml.info_text += "Vorsprung durch Technik. ";
      carview::ui::List l2;
      check(carview::draw_make_list(l2, d, ml, float(W), float(H)), "draw make list with a profile");
      check(l2.items().size() > l.items().size(), "115: the profile's panel draws");
      // Y with nothing affordable.
      ml.popup = {true, carview::text_of(kNoneAffordableTitle), carview::text_of(kNoneAffordableMessage),
                  {{"A", carview::text_of(kOk)}}};
      check(ml.popup.title == "NONE AFFORDABLE", "115's popup: " + ml.popup.title);
      carview::ui::List l3;
      check(carview::draw_make_list(l3, d, ml, float(W), float(H)), "draw make list with NONE AFFORDABLE");
      check(l3.items().size() > l2.items().size(), "115: the popup draws over the strip");
    }
  }

  if (g_failures > 0) {
    std::fprintf(stderr, "scene_screen_tests: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("scene_screen_tests: PASS (%d checks)\n", g_checks);
  return 0;
}
