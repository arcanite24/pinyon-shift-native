#include "options_screen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr int kItems = 10;  // OPT_SPINNER_ITEM_LG .. LG10
// Measured on 24_options_difficulty_orig.png (canvas px): the description's
// panel and its text's width, the arrows' half size.
constexpr float kPanel[4] = {820.0f, 187.0f, 1100.0f, 473.0f};
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

std::string item_root(int k) {
  return "Group_BLADE/Group_MENU/LIST_SPINNER/Group_Items/OPT_SPINNER_ITEM_LG" + (k == 0 ? std::string() : std::to_string(k + 1));
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
  static const std::vector<OptionsRowSpec> none;
  return p == OptionsPage::kDifficulty ? difficulty : p == OptionsPage::kHud ? hud : none;
}

std::string options_value_text(std::string_view id, std::string_view engine_value) {
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

bool draw_options_list(ui::List& l, const OptionsHud& o, float w, float h) {
  SceneScreen sc(kOptionsListScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.bindings().fit_text = false;
  sc.hide(sc.node("Group_FOOTER/ButtonBar"));
  // The list's scroll bar: the pages fit without scrolling.
  sc.hide(sc.node("Group_BLADE/Group_MENU/LIST_SPINNER/NAV_POSITION_INDICATOR"));
  sc.text(sc.node("Group_BLADE/TitleTape/Group/TitleTapeText"), o.title);

  const int n = int(o.rows.size());
  // The arrows' bodies sit off their buttons (the model's pivot is not
  // decoded), so they are drawn at the buttons after the scene.
  struct Arrow {
    float x, y;
    bool next;
  };
  std::vector<Arrow> arrows_at;
  std::string arrow_key;
  for (int k = 0; k < kItems; ++k) {
    const int item = sc.node(item_root(k));
    const int g = sc.node("Group", item);
    if (g < 0) continue;
    const int label = sc.node("Text_Label", g), current = sc.node("Text_Current", g);
    const int bar = sc.node("SELECTION_BAR", g), arrows = sc.node("ARROWS", g);
    sc.hide(sc.node("EMPTY_BAR", g));
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
    sc.hide(arrows);
    for (const char* name : {"BUTTON_ARROW_PREV", "BUTTON_ARROW_NEXT"}) {
      const int b = sc.node(name, arrows);
      if (b < 0) continue;
      const auto a = sd.at(b);
      arrows_at.push_back({a[0], a[1], name[13] == 'N'});
      if (arrow_key.empty()) arrow_key = sd.texture_key_of(sc.node("Front_Body", b));
    }
    if (k == o.selected)
      for (int p : {sc.node("Pill_left", bar), sc.node("Pill_right", bar)})
        if (p >= 0) sc.at(p).tint = ui4::kPink;
    if (label >= 0) {
      const auto a = sd.at(label);
      l.add_hit(f.x(a[0] - 12.0f), f.y(a[1] - 16.0f), f.x(a[0] + 580.0f), f.y(a[1] + 8.0f), ui::kHitRow, k);
    }
  }

  const int desc_group = sc.node("Group_BLADE/Group_DESCRIPTION");
  if (o.description.empty()) {
    sc.hide(desc_group);
  } else {
    sc.text(sc.node("TEXT_TITLE", desc_group), o.description_title);
    const int desc = sc.node("TEXT_DESCRIPTION", desc_group);
    sc.text(desc, sc.wrap(desc, o.description, kDescriptionWidth));
    if (desc >= 0) sc.at(desc).dy = 6.0f;  // clear of the title's pill
    // The dirt panel's crop is not decoded: a dark panel over the extent
    // measured on 24_options_difficulty_orig.png.
    sc.hide(sc.node("Group_NINE_GRID_DESCRIPTION/Panel", desc_group));
    l.turn(f.x(0.5f * (kPanel[0] + kPanel[2])), f.y(0.5f * (kPanel[1] + kPanel[3])), -0.052f);
    l.rect(f.x(kPanel[0]), f.y(kPanel[1]), f.x(kPanel[2]), f.y(kPanel[3]), ui::rgba(14, 14, 16, 205));
    l.unturn();
  }
  sc.draw(l, f);
  std::array<float, 4> uv{};
  if (!arrow_key.empty() && ui4::texture(arrow_key, uv)) {
    for (const Arrow& a : arrows_at) {
      std::array<float, 4> u = uv;
      if (a.next) std::swap(u[0], u[2]);  // the next arrow is the prev's mirror
      l.image_rotated(f.x(a.x), f.y(a.y), kArrowHalf[0] * f.s, kArrowHalf[1] * f.s, -0.052f, u, kWhite);
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
