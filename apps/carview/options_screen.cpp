#include "options_screen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include "drive/controls.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

// 300's OPT_SPINNER_ITEM_LG .. LG10; 837's two sliders and LG2 .. LG9; 831's
// LG .. LG4.
constexpr int kItems = 10;
constexpr int kControlsItems = 4;
// Measured on 24_options_difficulty_orig.png (canvas px): the description's
// panel and its text's width, the arrows' half size. The panel follows its
// group (300's Group_DESCRIPTION is at kPanelGroup).
constexpr float kPanel[4] = {820.0f, 187.0f, 1100.0f, 473.0f};
constexpr float kPanelGroup[2] = {820.2f, 202.8f};
// The slider's bar: StatBarEmpty's crop is not decoded; its length as on
// 26_my_profile_stats_orig.png (the same bar textures).
constexpr float kSliderLength = 121.0f;
constexpr float kDescriptionWidth = 240.0f;
constexpr float kRowSlope = 0.052f;
// TOTAL RACE BONUS ends before its value, right of the rows.
constexpr float kBonusLabelEnd = 748.0f;
constexpr float kBonusValueX = 792.0f;
constexpr float kArrowHalf[2] = {5.0f, 7.5f};
constexpr uint32_t kInk = ui::rgba(20, 20, 22);
constexpr uint32_t kWhite = ui::rgba(255, 255, 255);

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string lg(int n) { return n <= 1 ? std::string() : std::to_string(n); }

// The page's k-th row item.
std::string item_root(OptionsPage p, int k) {
  if (p == OptionsPage::kControls)
    return "Group_BODY/Group_MENU/Group_SPINNER/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG" + lg(k + 1);
  const std::string items = "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/";
  if (p == OptionsPage::kAudio)
    return k < 2 ? items + "OPT_SLIDER_ITEM_LG" + lg(k + 1) : items + "OPT_SPINNER_ITEM_LG" + lg(k);
  return items + "OPT_SPINNER_ITEM_LG" + lg(k + 1);
}

int item_count(OptionsPage p) { return p == OptionsPage::kControls ? kControlsItems : kItems; }

std::string title_path(OptionsPage p) {
  return p == OptionsPage::kControls ? "Group_BODY/Group_MENU/TitleTape/Group/TitleTapeText"
                                     : "Group_BLADE/TitleTape/Group/TitleTapeText";
}

// Assists.str and InGame.str's values.
constexpr StringRef kOn{"Assists", "IDS_Rewind_On", 0, "ON"};
constexpr StringRef kOff{"Assists", "IDS_Rewind_Off", 0, "OFF"};

struct ValueText {
  const char* id;     // empty: any row
  const char* value;  // the engine's
  StringRef text;
};

constexpr ValueText kValues[] = {
    {"difficulty preset", "easy", {"Assists", "IDS_MetaDifficulty_Easy", 0, "EASY"}},
    {"difficulty preset", "medium", {"Assists", "IDS_MetaDifficulty_Medium", 0, "MEDIUM"}},
    {"difficulty preset", "hard", {"Assists", "IDS_MetaDifficulty_Hard", 0, "HARD"}},
    {"difficulty preset", "insane", {"Assists", "IDS_MetaDifficulty_Insane", 0, "EXPERT"}},
    {"difficulty preset", "custom", {"Assists", "IDS_MetaDifficulty_Custom", 0, "CUSTOM"}},
    {"braking", "abs on", {"Assists", "IDS_Braking_ABSOn", 0, "ABS ON"}},
    {"braking", "abs off", {"Assists", "IDS_Braking_ABSOff", 0, "ABS OFF"}},
    {"braking", "assisted", {"Assists", "IDS_Braking_Assisted", 0, "ASSISTED"}},
    {"steering", "normal", {"Assists", "IDS_Steering_Normal", 0, "NORMAL"}},
    {"steering", "assisted", {"Assists", "IDS_Steering_Assisted", 0, "ASSISTED"}},
    {"steering", "simulation", {"Assists", "IDS_Steering_Simulation", 0, "SIMULATION"}},
    {"shifting", "automatic", {"Assists", "IDS_Shifting_Automatic", 0, "AUTOMATIC"}},
    {"shifting", "manual", {"Assists", "IDS_Shifting_Manual", 0, "MANUAL"}},
    {"shifting", "manual+clutch", {"Assists", "IDS_Shifting_ManualWithClutch", 0, "MANUAL W/CLUTCH"}},
    {"driving line", "full", {"Assists", "IDS_SuggestedLine_Full", 0, "FULL"}},
    {"driving line", "braking", {"Assists", "IDS_SuggestedLine_Braking", 0, "BRAKING ONLY"}},
    // COSMETIC DAMAGE is ON or OFF; the engine's other modes are its own.
    {"damage", "cosmetic", kOn},
    {"damage", "none", kOff},
    {"difficulty", "easy", {"AIDiff", "IDS_DisplayName_0", 0, "EASY"}},
    {"difficulty", "hard", {"AIDiff", "IDS_DisplayName_2", 0, "HARD"}},
    {"minimap", "rotating", {"InGame", "IDS_HudOptionValue_Rotating", 0, "ROTATING"}},
    {"minimap", "north-up", {"InGame", "IDS_HudOptionValue_Fixed", 0, "FIXED"}},
    {"speedometer", "analog", {"InGame", "IDS_HudOptionValue_Analog", 0, "ANALOG"}},
    {"speedometer", "digital", {"InGame", "IDS_HudOptionValue_Digital", 0, "DIGITAL"}},
    {"units", "metric (km/h)", {"InGame", "IDS_HudOptionValue_Metric", 0, "METRIC"}},
    {"units", "imperial (mph)", {"InGame", "IDS_HudOptionValue_English", 0, "ENGLISH"}},
    // PLAYBACK SYSTEM: the full dynamic range is the home theater's mix.
    {"full dynamic range", "on", {"MyProfile", "IDS_PlaybackSystem_Option_Theater", 0, "HOME THEATER"}},
    {"full dynamic range", "off", {"MyProfile", "IDS_PlaybackSystem_Option_TV", 0, "TV"}},
    {"radio", "off", {"MyProfile", "IDS_RadioStation_Option_Off", 0, "OFF"}},
    {"", "on", kOn},
    {"", "off", kOff},
};

}  // namespace

const std::vector<OptionsRowSpec>& options_rows(OptionsPage p) {
  // 24_options_difficulty_orig.png's rows.
  static const std::vector<OptionsRowSpec> difficulty = {
      {"difficulty preset", {"Assists", "IDS_MetaDifficulty", 0, "GAME DIFFICULTY"},
       {"Assists", "IDS_Desc_MetaDifficulty", 0, ""}},
      {"braking", {"Assists", "IDS_Braking", 0, "BRAKING"}, {"Assists", "IDS_Desc_Braking", 0, ""}},
      {"steering", {"Assists", "IDS_Steering", 0, "STEERING"}, {"Assists", "IDS_Desc_Steering", 0, ""}},
      {"traction control", {"Assists", "IDS_TractionControl", 0, "TRACTION CONTROL"},
       {"Assists", "IDS_Desc_TractionControl", 0, ""}},
      {"shifting", {"Assists", "IDS_Shifting", 0, "SHIFTING"}, {"Assists", "IDS_Desc_Shifting", 0, ""}},
      {"driving line", {"Assists", "IDS_SuggestedLine", 0, "DRIVING LINE"},
       {"Assists", "IDS_Desc_SuggestedLine", 0, ""}},
      {"damage", {"Assists", "IDS_DamageDifficulty", 0, "COSMETIC DAMAGE"},
       {"Assists", "IDS_Desc_DamageDifficulty", 0, ""}},
      {"rewind", {"Assists", "IDS_Rewind", 0, "REWIND"}, {"Assists", "IDS_Desc_Rewind", 0, ""}},
      {"difficulty", {"Assists", "IDS_AIDifficulty", 0, "OPPONENT DIFFICULTY"},
       {"Assists", "IDS_Desc_AIDifficulty", 0, ""}},
  };
  // 25_options_hud_orig.png's rows.
  static const std::vector<OptionsRowSpec> hud = {
      {"camera", {"InGame", "IDS_HudOption_Camera", 0, "CAMERA VIEW"}, {"InGame", "IDS_HudOption_Camera_Desc", 0, ""}},
      {"camera effects", {"InGame", "IDS_HudOption_CameraEffects", 0, "CAMERA MOTION EFFECTS"},
       {"InGame", "IDS_HudOption_CameraEffects_Desc", 0, ""}},
      {"minimap", {"InGame", "IDS_HudOption_Map", 0, "MAP"}, {"InGame", "IDS_HudOption_Map_Desc", 0, ""}},
      {"lap time and position", {"InGame", "IDS_HudOption_Timers", 0, "LAP TIME AND POSITION"},
       {"InGame", "IDS_HudOption_Timers_Desc", 0, ""}},
      {"wrong way warning", {"InGame", "IDS_HudOption_WrongWay", 0, "WRONG WAY WARNING"},
       {"InGame", "IDS_HudOption_WrongWay_Desc", 0, ""}},
      {"names over cars", {"InGame", "IDS_HudOption_Names", 0, "NAMES OVER CARS"},
       {"InGame", "IDS_HudOption_Names_Desc", 0, ""}},
      {"speedometer", {"InGame", "IDS_HudOption_Speedometer", 0, "SPEEDOMETER"},
       {"InGame", "IDS_HudOption_Speedometer_Desc", 0, ""}},
      {"units", {"InGame", "IDS_HudOption_Units", 0, "UNITS"}, {"InGame", "IDS_HudOption_Units_Desc", 0, ""}},
      {"popularity and skills", {"InGame", "IDS_HudOption_PopularitySkills", 0, "POPULARITY AND SKILLS"},
       {"InGame", "IDS_HudOption_PopularitySkills_Desc", 0, ""}},
  };
  // 837's rows, by MyProfile.str's AUDIO option names (no capture, Q135):
  // SFX VOLUME is the engine's master volume, RADIO STATION its radio.
  static const std::vector<OptionsRowSpec> audio = {
      {"master volume", {"MyProfile", "IDS_SFXVolume", 0, "SFX VOLUME"},
       {"MyProfile", "IDS_AudioOption_SFXVolume_Desc", 0, ""}, true},
      {"radio volume", {"MyProfile", "IDS_RadioVolume", 0, "RADIO VOLUME"},
       {"MyProfile", "IDS_AudioOption_RadioVolume_Desc", 0, ""}, true},
      {"radio", {"MyProfile", "IDS_RadioStation", 0, "RADIO STATION"},
       {"MyProfile", "IDS_AudioOption_RadioStation_Desc", 0, ""}},
      {"radio dj", {"MyProfile", "IDS_RadioDJ", 0, "RADIO DJ"}, {"MyProfile", "IDS_AudioOption_RadioDJ_Desc", 0, ""}},
      {"satnav", {"MyProfile", "IDS_SatNav", 0, "GPS VOICE"}, {"MyProfile", "IDS_SatNav_Desc", 0, ""}},
      {"full dynamic range", {"MyProfile", "IDS_PlaybackSystem", 0, "PLAYBACK SYSTEM"},
       {"MyProfile", "IDS_AudioOption_PlaybackSystem_Desc", 0, ""}},
  };
  // 23_options_controls_orig.png's rows; no description panel. SWITCH GEAR
  // UP/DOWN has no engine row (Q135).
  constexpr StringRef kNoText{"", "", 0, "", true};
  static const std::vector<OptionsRowSpec> controls = {
      {"controller layout", {"MyProfile", "IDS_Option_Layout", 0, "LAYOUT"}, kNoText},
      {"rumble", {"MyProfile", "IDS_Option_RumbleSetting", 0, "VIBRATION"}, kNoText},
      {"", {"MyProfile", "IDS_Option_InvertGearUpDown", 0, "SWITCH GEAR UP/DOWN"}, kNoText},
      {"switch handbrake/clutch", {"MyProfile", "IDS_Option_InvertHandbrakeClutch", 0, "SWITCH HANDBRAKE/CLUTCH"},
       kNoText},
  };
  static const std::vector<OptionsRowSpec> none;
  switch (p) {
    case OptionsPage::kDifficulty: return difficulty;
    case OptionsPage::kHud: return hud;
    case OptionsPage::kAudio: return audio;
    case OptionsPage::kControls: return controls;
    default: return none;
  }
}

std::string_view options_scene(OptionsPage p) {
  switch (p) {
    case OptionsPage::kDifficulty:
    case OptionsPage::kHud: return kOptionsListScene;
    case OptionsPage::kAudio: return kAudioOptionsScene;
    case OptionsPage::kControls: return kControlsScene;
    default: return {};
  }
}

std::string options_title(OptionsPage p) {
  namespace t = options_text;
  switch (p) {
    case OptionsPage::kDifficulty: return text_of(t::kDifficulty);
    case OptionsPage::kHud: return text_of(t::kHud);
    case OptionsPage::kAudio: return text_of(t::kAudio);
    case OptionsPage::kControls: return text_of(t::kControls);
    default: return {};
  }
}

float options_percent(std::string_view v) {
  if (v.size() < 2 || v.back() != '%') return -1.0f;
  int n = 0;
  for (char c : v.substr(0, v.size() - 1)) {
    if (c < '0' || c > '9') return -1.0f;
    n = n * 10 + (c - '0');
  }
  return float(n) / 100.0f;
}

std::string options_slider_step(const std::vector<std::string>& choices, std::string_view value, int by) {
  const float at = options_percent(value);
  if (at < 0.0f || by == 0) return std::string(value);
  // The nearest choice past the value in the step's direction.
  const std::string* best = nullptr;
  float best_at = 0.0f;
  for (const std::string& c : choices) {
    const float p = options_percent(c);
    if (p < 0.0f || (by > 0 ? p <= at : p >= at)) continue;
    if (!best || (by > 0 ? p < best_at : p > best_at)) best = &c, best_at = p;
  }
  return best ? *best : std::string(value);
}

std::vector<std::pair<std::string, std::string>> controls_pad_labels(int layout_id, bool swap_handbrake_clutch,
                                                                      int shifting) {
  using drive::PadControl;
  const drive::ControllerLayout lay =
      drive::apply_handbrake_clutch_swap(drive::default_controller_layout(layout_id), swap_handbrake_clutch);
  const std::string not_used = text_of({"MyProfile", "IDS_NotUsed", 0, "NOT USED"});
  // An action's label; the engine's extras (photo mode, telemetry) and the
  // gears a shifting mode does not use read NOT USED.
  auto label = [&](PadControl c) -> std::string {
    const std::string_view a = lay.action_for(c);
    auto my = [](const char* id, const char* fb) { return text_of({"MyProfile", id, 0, fb}); };
    if (a == "Gas") return my("IDS_Gas", "GAS");
    if (a == "Brake") return my("IDS_Brake", "BRAKE");
    if (a == "Handbrake") return my("IDS_Handbrake", "HANDBRAKE");
    if (a == "Rewind") return my("IDS_Rewind", "REWIND");
    if (a == "SwitchCamera") return my("IDS_SwitchCamera", "SWITCH CAMERA");
    if (a == "Map") return my("IDS_Map", "MAP");
    if (a == "LookStraightBack") return my("IDS_LookStraightBack", "LOOK STRAIGHT BACK");
    if (a == "RadioLeft" || a == "RadioRight") return my("IDS_Radio", "RADIO");
    if (a == "Clutch") return shifting == 2 ? my("IDS_Clutch", "CLUTCH") : not_used;
    if (a == "ShiftUp") return shifting > 0 ? my("IDS_ShiftUp", "SHIFT UP") : not_used;
    // 23_options_controls_orig.png (automatic): the shift-down button
    // enters events (ACTIVATE).
    if (a == "ShiftDown") return shifting > 0 ? my("IDS_ShiftDown", "SHIFT DOWN") : my("IDS_Activate", "ACTIVATE");
    return not_used;
  };
  return {
      {"START", text_of({"MyProfile", "IDS_Pause", 0, "PAUSE"})},
      {"LT", label(PadControl::kLeftTrigger)},
      {"LB", label(PadControl::kLeftShoulder)},
      {"BACK", label(PadControl::kBack)},
      {"LTS", text_of({"MyProfile", "IDS_Steering", 0, "STEERING"})},
      {"LTSB", label(PadControl::kLeftStickClick)},
      {"DPAD", label(PadControl::kDPadUp)},
      {"LEFT", label(PadControl::kDPadLeft)},
      {"RIGHT", label(PadControl::kDPadRight)},
      {"DOWN", label(PadControl::kDPadDown)},
      {"RT", label(PadControl::kRightTrigger)},
      {"RB", label(PadControl::kRightShoulder)},
      {"Y", label(PadControl::kButtonY)},
      {"B", label(PadControl::kButtonB)},
      {"X", label(PadControl::kButtonX)},
      {"A", label(PadControl::kButtonA)},
      {"RTS", text_of({"MyProfile", "IDS_FreeLook", 0, "FREE LOOK"})},
      {"RTSB", label(PadControl::kRightStickClick)},
  };
}

std::string options_value_text(std::string_view id, std::string_view engine_value) {
  // LAYOUT: "layout 2" -> DEFAULT LAYOUT 2.
  if (id == "controller layout" && engine_value.rfind("layout ", 0) == 0)
    return format1(text_of({"MyProfile", "IDS_ControllerLayoutFormat", 0, "DEFAULT LAYOUT {0}"}),
                   engine_value.substr(7));
  // GAME DIFFICULTY's value carries the engine's bonus ("MEDIUM (+40% CR)").
  std::string v(engine_value);
  if (id == "difficulty preset") {
    if (const size_t cut = v.find(" ("); cut != std::string::npos) v.resize(cut);
    for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  for (const ValueText& t : kValues) {
    if ((*t.id == 0 || id == t.id) && v == t.value) return text_of(t.text);
  }
  return upper(v);
}

const std::vector<const char*>& options_list_nodes() {
  static const std::vector<const char*> nodes = {
      "Group_BLADE/TitleTape/Group/TitleTapeText",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG/Group/Text_Label",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG/Group/Text_Current",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG/Group/SELECTION_BAR/Pill_left",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG10/Group/Text_Current",
      "Group_BLADE/Group_MENU/LIST_SPINNER/NAV_POSITION_INDICATOR",
      "Group_BLADE/Group_DESCRIPTION/TEXT_TITLE",
      "Group_BLADE/Group_DESCRIPTION/TEXT_DESCRIPTION",
      "Group_FOOTER/ButtonBar",
  };
  return nodes;
}

const std::vector<const char*>& options_audio_nodes() {
  static const std::vector<const char*> nodes = {
      "Group_BLADE/TitleTape/Group/TitleTapeText",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SLIDER_ITEM_LG/Group/Text_Label",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SLIDER_ITEM_LG/Group/Text_Setting",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SLIDER_ITEM_LG/Group/Pill_left",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SLIDER_ITEM_LG/Group/TUNING_SLIDER/Slider_Fill/Group/"
      "StatBarFill",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SLIDER_ITEM_LG/Group/TUNING_SLIDER/PROGRESS_BAR_STATIC_BKG/"
      "StatBarEmpty",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SLIDER_ITEM_LG2/Group/Text_Setting",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG2/Group/Text_Current",
      "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG9/Group/Text_Current",
      "Group_BLADE/Group_DESCRIPTION/TEXT_TITLE",
      "Group_BLADE/Group_DESCRIPTION/TEXT_DESCRIPTION",
      "Group_BLADE/Group_DESCRIPTION/Group_NINE_GRID_DESCRIPTION/BG",
      "Group_FOOTER/ButtonBar",
  };
  return nodes;
}

const std::vector<const char*>& options_controls_nodes() {
  static const std::vector<const char*> nodes = {
      "Group_BODY/Group_MENU/TitleTape/Group/TitleTapeText",
      "Group_BODY/Group_MENU/Group_SPINNER/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG/Group/Text_Label",
      "Group_BODY/Group_MENU/Group_SPINNER/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG4/Group/Text_Current",
      "Group_BODY/Group_MENU/Group_SPINNER/LIST_SPINNER/NAV_POSITION_INDICATOR",
      "Group_BODY/Group_MENU/Group_ADVANCED",
      "Group_BODY/Group_MENU/Group_CONTROLLER_DATA/Group_TEXT/Group_LT/TEXT_LT",
      "Group_BODY/Group_MENU/Group_CONTROLLER_DATA/Group_TEXT/Group_DPAD/Group_DOWN/TEXT_DPAD",
      "Group_BODY/Group_MENU/Group_CONTROLLER_DATA/Group_TEXT/Group_RTSB/TEXT_RTSB",
      "Group_BODY/Group_MENU/Group_CONTROLLER_DATA/Group_CONTROL/CONTROL_IMAGE",
      "Group_BODY/Group_MENU/Group_CONTROLLER_DATA/Group_NINE_GRID/BigPane01",
      "Group_FOOTER/ButtonBar",
  };
  return nodes;
}

namespace {

// CONTROLS as on 23_options_controls_orig.png (canvas px). The disc's
// layout is the screen's before its entry animation (not run): the body sits
// 66 px lower, the labels elsewhere, the lines off their buttons, and the
// dirt panel's crop is not decoded. So the body moves up, each label goes
// to its measured place (its left and right ends, its middle's height), the
// lines are drawn from label to button, and the panel is a dark quad.
constexpr float kControlsBody[2] = {-4.0f, -66.0f};
constexpr float kControlsPanel[4] = {209.0f, 242.0f, 1083.0f, 608.0f};  // turned -0.052 about its centre
struct PadPlace {
  const char* name;
  float left, right, cy;
  uint32_t tint;
};
constexpr uint32_t kPadGrey = ui::rgba(196, 196, 196);
constexpr PadPlace kPadPlaces[] = {
    {"START", 634.0f, 692.5f, 265.0f, kPadGrey},
    {"LT", 421.0f, 480.0f, 300.0f, kPadGrey},
    {"LB", 345.0f, 432.5f, 332.5f, kPadGrey},
    {"BACK", 397.5f, 436.0f, 382.5f, kPadGrey},
    {"LTS", 351.0f, 436.5f, 435.0f, kPadGrey},
    {"LTSB", 355.0f, 442.0f, 486.0f, kPadGrey},
    {"DPAD", 404.0f, 491.0f, 522.5f, kPadGrey},
    {"LEFT", 364.0f, 419.0f, 557.5f, kPadGrey},
    {"RIGHT", 471.5f, 527.0f, 551.5f, kPadGrey},
    {"DOWN", 407.5f, 494.0f, 586.0f, kPadGrey},
    {"RT", 800.0f, 834.0f, 281.0f, kPadGrey},
    {"RB", 851.5f, 999.0f, 304.0f, kPadGrey},
    // The face buttons' labels in their buttons' colours.
    {"Y", 855.0f, 926.0f, 357.5f, ui::rgba(255, 247, 95)},
    {"B", 856.5f, 943.5f, 386.5f, ui::rgba(208, 55, 62)},
    {"X", 858.5f, 946.5f, 415.0f, ui::rgba(54, 170, 213)},
    {"A", 860.0f, 971.5f, 443.5f, ui::rgba(164, 196, 55)},
    {"RTS", 814.0f, 908.5f, 517.5f, kPadGrey},
    {"RTSB", 582.5f, 775.0f, 538.0f, kPadGrey},
};
// The lines from each label to its button (and the d-pad's cross).
constexpr float kPadLines[][4] = {
    {489.0f, 308.5f, 545.0f, 320.0f},  // LT
    {444.0f, 338.5f, 530.0f, 352.5f},  // LB
    {449.0f, 386.5f, 604.0f, 406.5f},  // BACK
    {452.5f, 432.5f, 525.0f, 424.0f},  // LTS
    {455.0f, 473.5f, 545.0f, 426.0f},  // LTSB
    {507.5f, 516.5f, 597.5f, 462.5f},  // DPAD
    {428.5f, 554.5f, 463.5f, 553.0f},  // the cross
    {446.0f, 540.0f, 446.0f, 570.0f},
    {666.5f, 284.0f, 679.0f, 399.0f},  // START
    {789.0f, 296.0f, 744.0f, 314.0f},  // RT
    {839.0f, 320.0f, 765.0f, 336.0f},  // RB
    {839.0f, 364.0f, 762.5f, 379.0f},  // Y
    {842.5f, 390.0f, 785.0f, 395.0f},  // B
    {844.0f, 419.0f, 730.0f, 405.0f},  // X
    {845.0f, 447.5f, 757.5f, 426.0f},  // A
    {810.0f, 500.0f, 715.0f, 469.0f},  // RTS
    {689.0f, 517.5f, 696.0f, 467.5f},  // RTSB
};
constexpr uint32_t kPadLine = ui::rgba(210, 209, 208);

// The CONTROLS diagram's label node under Group_TEXT, by its name.
std::string pad_text_path(std::string_view name) {
  const std::string base = "Group_BODY/Group_MENU/Group_CONTROLLER_DATA/Group_TEXT/";
  if (name == "DPAD") return base + "Group_DPAD/TEXT_DPAD";
  if (name == "LEFT" || name == "RIGHT" || name == "DOWN" || name == "UP")
    return base + "Group_DPAD/Group_" + std::string(name) + "/TEXT_DPAD";
  return base + "Group_" + std::string(name) + "/TEXT_" + std::string(name);
}

}  // namespace

bool draw_options_list(ui::List& l, const OptionsHud& o, float w, float h) {
  const OptionsPage page = o.page == OptionsPage::kNone ? OptionsPage::kDifficulty : o.page;
  SceneScreen sc(options_scene(page));
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  const SceneFrame f = SceneScreen::frame(w, h);
  const bool controls = page == OptionsPage::kControls;
  sc.bindings().fit_text = false;
  sc.hide(sc.node("Group_FOOTER/ButtonBar"));
  // The list's scroll bar: the pages fit without scrolling.
  sc.hide(sc.node(controls ? "Group_BODY/Group_MENU/Group_SPINNER/LIST_SPINNER/NAV_POSITION_INDICATOR"
                           : "Group_BLADE/Group_MENU/LIST_SPINNER/NAV_POSITION_INDICATOR"));
  sc.text(sc.node(title_path(page)), o.title);
  const std::vector<OptionsRowSpec>& specs = options_rows(page);
  // CONTROLS's body moves up to its place on the capture; the rows' hits
  // and arrows follow it.
  const float odx = controls ? kControlsBody[0] : 0.0f, ody = controls ? kControlsBody[1] : 0.0f;
  if (controls)
    if (const int body = sc.node("Group_BODY"); body >= 0) sc.at(body).dx = odx, sc.at(body).dy = ody;

  const int n = int(o.rows.size());
  // The arrows' bodies sit off their buttons (the model's pivot is not
  // decoded), so they are drawn at the buttons after the scene.
  struct Arrow {
    float x, y;
    bool next;
    float alpha;
  };
  std::vector<Arrow> arrows_at;
  std::string arrow_key;
  for (int k = 0; k < item_count(page); ++k) {
    const int item = sc.node(item_root(page, k));
    const int g = sc.node("Group", item);
    if (g < 0) continue;
    const bool slider = k < int(specs.size()) && specs[size_t(k)].slider;
    const int label = sc.node("Text_Label", g);
    const int current = sc.node(slider ? "Text_Setting" : "Text_Current", g);
    // A slider's pills are the group's own; a spinner's are its SELECTION_BAR's.
    const int bar = slider ? g : sc.node("SELECTION_BAR", g);
    const int arrows = sc.node(slider ? "TUNING_SLIDER/Slider_Fill/ARROWS" : "ARROWS", g);
    sc.hide(sc.node("EMPTY_BAR", item));
    const bool bonus = k == n && !o.bonus.empty();
    if (k >= n && !bonus) {
      sc.hide(item);
      continue;
    }
    if (bonus) {
      // TOTAL RACE BONUS on a light bar under the rows, no arrows.
      const std::string bl = text_of(options_text::kTotalRaceBonus);
      sc.text(label, bl);
      sc.text(current, o.bonus);
      // Along the turned row: a step right rises with it.
      if (label >= 0) {
        NodeOverride& lo = sc.at(label);
        lo.dx = kBonusLabelEnd - (sd.at(label)[0] + sd.measure(label, bl));
        lo.dy = -lo.dx * kRowSlope;
      }
      if (current >= 0) {
        NodeOverride& co = sc.at(current);
        co.dx = kBonusValueX - sd.at(current)[0];
        co.dy = -co.dx * kRowSlope;
      }
      sc.hide(arrows);
      for (int p : {sc.node("Pill_left", bar), sc.node("Pill_right", bar)})
        if (p >= 0) sc.at(p).tint = kWhite;
      if (label >= 0) sc.at(label).tint = kInk;
      if (current >= 0) sc.at(current).tint = kInk;
      continue;
    }
    sc.text(label, o.rows[size_t(k)].first);
    sc.text(current, o.rows[size_t(k)].second);
    float fill = -1.0f;
    if (slider) {
      // The bar: its empty track and the value's share of it.
      fill = std::clamp(options_percent(o.rows[size_t(k)].second), 0.0f, 1.0f);
      const int track = sc.node("TUNING_SLIDER/PROGRESS_BAR_STATIC_BKG/StatBarEmpty", g);
      const int full = sc.node("TUNING_SLIDER/Slider_Fill/Group/StatBarFill", g);
      if (track >= 0) sc.at(track).visible_width = kSliderLength;
      if (full >= 0) sc.at(full).visible_width = std::max(0.5f, kSliderLength * fill);
      if (full >= 0 && fill <= 0.0f) sc.hide(full);
      // The value ends clear of the next arrow ("100%" in the engine's
      // font is wider than the scene's box).
      const int nb = sc.node("BUTTON_ARROW_NEXT", arrows);
      if (current >= 0 && nb >= 0) {
        const ui::Align al = sd.style(current, SceneFrame{}, 0).align;
        const float tail = al == ui::Align::kRight ? 0.0f : al == ui::Align::kCenter ? 0.5f : 1.0f;
        const float over =
            sd.at(current)[0] + tail * sd.measure(current, o.rows[size_t(k)].second) - (sd.at(nb)[0] - 9.0f);
        if (over > 0.0f) sc.at(current).dx = -over, sc.at(current).dy = over * kRowSlope;
      }
    }
    sc.hide(arrows);
    for (const char* name : {"BUTTON_ARROW_PREV", "BUTTON_ARROW_NEXT"}) {
      const int b = sc.node(name, arrows);
      if (b < 0) continue;
      const auto a = sd.at(b);
      const bool next = name[13] == 'N';
      // A slider's arrow dims at its end (837's PREV is a0.5 at 0%).
      const bool end = slider && (next ? fill >= 1.0f : fill <= 0.0f);
      arrows_at.push_back({a[0] + odx, a[1] + ody, next, end ? 0.5f : 1.0f});
      if (arrow_key.empty()) arrow_key = sd.texture_key_of(sc.node("Front_Body", b));
    }
    if (k == o.selected)
      for (int p : {sc.node("Pill_left", bar), sc.node("Pill_right", bar)})
        if (p >= 0) sc.at(p).tint = ui4::kPink;
    if (label >= 0) {
      // The row rises along its slope (30 px over its length, more than a
      // row), so its hit is cut into level pieces that follow it.
      auto a = sd.at(label);
      a[0] += odx, a[1] += ody;
      constexpr int kPieces = 6;
      constexpr float kX0 = -12.0f, kLen = 592.0f, kHalf = 12.0f;
      for (int s = 0; s < kPieces; ++s) {
        const float x0 = a[0] + kX0 + kLen * float(s) / kPieces, x1 = a[0] + kX0 + kLen * float(s + 1) / kPieces;
        const float cy = a[1] - 4.0f - kRowSlope * (0.5f * (x0 + x1) - a[0]);
        l.add_hit(f.x(x0), f.y(cy - kHalf), f.x(x1), f.y(cy + kHalf), ui::kHitRow, k);
      }
    }
  }

  if (controls) {
    // The diagram: X ADVANCED's list stays closed, the wheels' diagrams
    // (Group_BRISTOL is shown on the disc) are not this controller's.
    const std::string data = "Group_BODY/Group_MENU/Group_CONTROLLER_DATA/";
    sc.hide(sc.node("Group_BODY/Group_MENU/Group_ADVANCED"));
    for (const char* g : {"Group_WHEEL", "Group_PEDALS", "Group_BRISTOL", "Group_CONTROL/Line", "Group_NINE_GRID"})
      sc.hide(sc.node(data + g));
    l.turn(f.x(0.5f * (kControlsPanel[0] + kControlsPanel[2])), f.y(0.5f * (kControlsPanel[1] + kControlsPanel[3])),
           -0.052f);
    l.rect(f.x(kControlsPanel[0]), f.y(kControlsPanel[1]), f.x(kControlsPanel[2]), f.y(kControlsPanel[3]),
           ui::rgba(14, 14, 16, 205));
    l.unturn();
    // D-pad up is the group's own label (Group_UP's line is a0 on the disc).
    sc.hide(sc.node(pad_text_path("UP")));
    for (const auto& [name, text] : o.pad) {
      const int t = sc.node(pad_text_path(name));
      if (t < 0) continue;
      sc.text(t, text);
      for (const PadPlace& p : kPadPlaces) {
        if (name != p.name) continue;
        // The label's anchor (its alignment's end) onto the capture's.
        const ui::Align al = sd.style(t, SceneFrame{}, 0).align;
        const float af = al == ui::Align::kRight ? 1.0f : al == ui::Align::kCenter ? 0.5f : 0.0f;
        const float w0 = p.right - p.left, w1 = sd.measure(t, text);
        // Its width as drawn, centred on the capture's.
        const float x = 0.5f * (p.left + p.right) + (af - 0.5f) * (w0 > 0.0f ? w1 : 0.0f);
        NodeOverride& no = sc.at(t);
        no.dx = x - sd.at(t)[0] - odx;
        no.dy = p.cy - sd.at(t)[1] - ody;
        no.tint = p.tint;
      }
    }
  } else {
    const int desc_group = sc.node("Group_BLADE/Group_DESCRIPTION");
    if (o.description.empty()) {
      sc.hide(desc_group);
    } else {
      sc.text(sc.node("TEXT_TITLE", desc_group), o.description_title);
      const int desc = sc.node("TEXT_DESCRIPTION", desc_group);
      sc.text(desc, sc.wrap(desc, o.description, kDescriptionWidth));
      if (desc >= 0) sc.at(desc).dy = 6.0f;  // clear of the title's pill
      // The dirt panel's crop is not decoded: a dark panel over the extent
      // measured on 24_options_difficulty_orig.png, moved with its group.
      sc.hide(sc.node("Group_NINE_GRID_DESCRIPTION/Panel", desc_group));
      sc.hide(sc.node("Group_NINE_GRID_DESCRIPTION/BG", desc_group));
      const auto ga = sd.at(desc_group);
      const float ox = desc_group >= 0 ? ga[0] - kPanelGroup[0] : 0.0f;
      const float oy = desc_group >= 0 ? ga[1] - kPanelGroup[1] : 0.0f;
      const float p[4] = {kPanel[0] + ox, kPanel[1] + oy, kPanel[2] + ox, kPanel[3] + oy};
      l.turn(f.x(0.5f * (p[0] + p[2])), f.y(0.5f * (p[1] + p[3])), -0.052f);
      l.rect(f.x(p[0]), f.y(p[1]), f.x(p[2]), f.y(p[3]), ui::rgba(14, 14, 16, 205));
      l.unturn();
    }
  }
  sc.draw(l, f);
  if (controls)
    for (const auto& ln : kPadLines) l.line(f.x(ln[0]), f.y(ln[1]), f.x(ln[2]), f.y(ln[3]), 1.6f * f.s, kPadLine);
  std::array<float, 4> uv{};
  if (!arrow_key.empty() && ui4::texture(arrow_key, uv)) {
    for (const Arrow& a : arrows_at) {
      std::array<float, 4> u = uv;
      if (a.next) std::swap(u[0], u[2]);  // the next arrow is the prev's mirror
      const uint32_t tint = a.alpha < 1.0f ? ui::rgba(255, 255, 255, 128) : kWhite;
      l.image_rotated(f.x(a.x), f.y(a.y), kArrowHalf[0] * f.s, kArrowHalf[1] * f.s, -0.052f, u, tint);
    }
  }

  if (!o.notice.empty()) {
    const float s = std::max(0.25f, h / 720.0f);
    ui4::brush(l, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    std::string text = o.notice;
    ui4::fit_text(text, ns, 340.0f * s, 9.0f * s);
    l.text(w - 68.0f * s, h - 60.0f * s, text, ns);
  }
  std::vector<ui4::Prompt> prompts;
  for (const auto& [b, label] : o.prompts) prompts.push_back({b.c_str(), label});
  ui4::button_bar(l, prompts, w, h);
  return true;
}

}  // namespace carview
