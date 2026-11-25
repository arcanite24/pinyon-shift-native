#include "frontend_screens.h"

#include <algorithm>
#include <cmath>

#include "game_hud.h"
#include "race_screens.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

using Phase = TitleMenu::Phase;
using Popup = TitleMenu::Popup;
using Row = TitleMenu::Row;

constexpr uint32_t kWhite = ui::rgba(255, 255, 255);
constexpr uint32_t kInk = ui::rgba(20, 20, 22);

// The direct child of `parent` called `name`.
int child(const SceneDraw& sd, int parent, std::string_view name) {
  if (parent < 0) return -1;
  for (int c : sd.children_of(parent))
    if (sd.object_name(c) == name) return c;
  return -1;
}

float ease(double t, double len) {
  if (len <= 0.0) return 1.0f;
  const float u = float(std::clamp(t / len, 0.0, 1.0));
  return u * u * (3.0f - 2.0f * u);
}

// The tape behind a selected row ends a little past its text.
constexpr float kRowTapePad = 34.0f;
// The tip title's tape ends just past its text (03_loading_tip_orig.png).
constexpr float kTipTapePad = 18.0f;

// The popup over the menu, if any.
bool draw_title_popup(ui::List& l, const TitleMenu& m, float w, float h) {
  switch (m.popup) {
    case Popup::kNone: return false;
    case Popup::kNoNetwork:
      return draw_message_popup(l, text_of(title_text::kNoNetworkTitle), text_of(title_text::kNoNetworkBody),
                                {{"A", text_of(title_text::kContinue)}}, w, h);
    case Popup::kProfileCorrupt:
      return draw_message_popup(l, text_of(title_text::kProfileCorruptTitle),
                                text_of(title_text::kProfileCorruptBody), {{"A", text_of(title_text::kContinue)}}, w,
                                h);
    case Popup::kConfirmNewCareer:
      return draw_message_popup(l, text_of(title_text::kNewCareerTitle), text_of(title_text::kNewCareerBody),
                                {{"A", text_of(title_text::kYes)}, {"B", text_of(title_text::kNo)}}, w, h);
  }
  return false;
}

}  // namespace

StringRef title_row_text(TitleMenu::Row r) {
  switch (r) {
    case Row::kSinglePlayer: return title_text::kSinglePlayer;
    case Row::kMultiplayer: return title_text::kMultiplayer;
    case Row::kNewCareer: return title_text::kNewCareer;
    case Row::kFreeRoam: return title_text::kFreeRoam;
    case Row::kSettings: return title_text::kSettings;
    case Row::kQuit: return title_text::kQuit;
  }
  return title_text::kSinglePlayer;
}

bool draw_title_screen(ui::List& l, const TitleScreenHud& ts, float w, float h) {
  SceneScreen sc(kPreSplashScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  const TitleMenu& m = ts.menu;
  const SceneFrame f = SceneScreen::frame(w, h);
  // Every node the screen keeps rests at alpha 0 before its slide runs;
  // draw them at full and hide the rest.
  sc.bindings().ignore_zero_alpha = true;
  sc.bindings().fit_text = false;
  // The movie (drawn under by the caller), the black fader, the Kinect and
  // controller prompts, the install pane and the scene's own help bar
  // (BUTTON_BAR_SCENE's is drawn below).
  for (const char* n : {"Video", "Fader", "Start", "Install", "ButtonBar", "BREADCRUMB", "menu_shadow"})
    sc.hide(sc.node(n));

  // Press Start: the logo and PRESS START (Content), where the original's
  // intro leaves them; they go out over the profile step.
  const int content = sc.node("Content");
  if (content >= 0) {
    float a = 0.0f;
    if (m.phase == Phase::kPressStart) a = 1.0f;
    else if (m.phase == Phase::kProfile) a = 1.0f - ease(m.phase_t, TitleMenu::kProfileSeconds);
    if (a <= 0.0f) {
      sc.hide(content);
    } else {
      sc.at(content).dx = title_layout::kContentDx;
      sc.at(content).dy = title_layout::kContentDy;
      if (a < 1.0f) sc.show(content, a);
      if (const int tape = sc.node("TitleTape"); tape >= 0 && title_layout::kPressStartScale != 1.0f)
        sc.at(tape).scale = title_layout::kPressStartScale;
      sc.text(sc.node("TitleTapeText"), text_of(title_text::kPressStart));
      // Anywhere on Press Start confirms (the mouse).
      l.add_hit(0, 0, w, h, ui::kHitConfirm, 0);
    }
  }

  // The menu: SPLASH_MENU's rows, the selected one on its pink tape, the
  // others plain.
  const int menu = sc.node("Menu");
  if (!m.menu_visible()) {
    sc.hide(menu);
  } else {
    const auto rows = m.rows();
    for (int slot = 0; slot < 5; ++slot) {
      const int btn = sc.node("SPLASH_MENU_BUTTON" + std::to_string(slot + 1));
      if (btn < 0) continue;
      if (slot >= int(rows.size())) {
        sc.hide(btn);
        continue;
      }
      const int group = child(sd, btn, "Group");
      const int text = child(sd, group, "TEXT_BUTTON");
      const int tape = child(sd, group, "Tape");
      const std::string label = text_of(title_row_text(rows[size_t(slot)]));
      sc.text(text, label);
      const bool sel = slot == m.selected && m.popup == Popup::kNone;
      if (text >= 0) sc.at(text).tint = sel ? kWhite : title_layout::kRowUnselected;
      // The text node draws at its group's scale (0.7).
      const float tw = text >= 0 ? sd.measure(text, label) : 0.0f;
      if (!sel) {
        sc.hide(tape);
        if (text >= 0) sc.at(text).dx = title_layout::kRowUnselectedDx;
      } else {
        sc.at(btn).scale = title_layout::kRowSelectedScale;
        sc.at(btn).dx = title_layout::kRowSelectedDx;
        for (const char* t : {"Crisp_Tape", "smudge_tape"})
          if (const int n = child(sd, tape, t); n >= 0) sc.at(n).visible_width = tw + kRowTapePad;
      }
      if (text >= 0) {
        const auto c = sd.at(text);
        l.add_hit(f.x(c[0] - 12.0f), f.y(c[1] - 20.0f), f.x(c[0] + tw + 40.0f), f.y(c[1] + 20.0f), ui::kHitRow,
                  slot);
      }
    }
  }
  sc.draw(l, f);

  // The help bar: the menu's SELECT and BACK; the engine's EXTRAS (X) beside
  // them on the menu, as the pause menu has it.
  if (m.menu_visible() && m.popup == Popup::kNone) {
    std::vector<ui4::Prompt> prompts{{"A", text_of(title_text::kSelect)}, {"B", text_of(title_text::kBack)}};
    if (m.phase == Phase::kMenu) prompts.push_back({"X", text_of(title_text::kExtras)});
    ui4::button_bar(l, prompts, w, h);
  }
  draw_title_popup(l, m, w, h);
  return true;
}

bool draw_loading_scene(ui::List& l, const LoadingSceneHud& hud, float w, float h) {
  SceneScreen sc(kLoadingScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.bindings().ignore_zero_alpha = true;
  sc.bindings().fit_text = false;
  sc.hide(sc.node("Fade"));
  sc.hide(sc.node("TipImage"));
  // The backdrop over the whole screen (the scene's BG fills the canvas;
  // wider screens get the bars filled black first). None: the card alone
  // (over the original's frame in the corpus test).
  if (const int bg = sc.node("BG"); bg >= 0) {
    if (hud.backdrop.empty()) {
      sc.hide(bg);
    } else {
      l.rect(0, 0, w, h, ui::rgba(0, 0, 0));
      sc.at(bg).texture = hud.backdrop;
    }
  }

  // The tip's title on its black tapes.
  const int title_group = sc.node("Title_Text");
  const int title = child(sd, title_group, "TitleText");
  if (hud.tip_title.empty()) {
    sc.hide(title_group);
  } else if (title >= 0) {
    sc.text(title, hud.tip_title);
    sc.at(title).dx = loading_layout::kTipTitleDx;
    sc.at(title).dy = loading_layout::kTipTitleDy;
    const float tw = sd.measure(title, hud.tip_title);
    for (const char* t : {"smudge_tape", "crisp_tape"})
      if (const int n = child(sd, title_group, t); n >= 0) sc.at(n).visible_width = tw + kTipTapePad;
    if (const int n = child(sd, title_group, "dirt"); n >= 0) sc.hide(n);
  }

  // The tip's message, a line on each white pill.
  const int body = sc.node("Body_Text");
  std::vector<std::string> lines;
  {
    const int first = sc.node("Line_1/TipText");
    const std::string wrapped = first >= 0 ? sc.wrap(first, hud.tip_body) : hud.tip_body;
    size_t from = 0;
    for (size_t nl; (nl = wrapped.find('\n', from)) != std::string::npos; from = nl + 1)
      lines.push_back(wrapped.substr(from, nl - from));
    if (!hud.tip_body.empty()) lines.push_back(wrapped.substr(from));
  }
  for (int i = 0; i < 8; ++i) {
    const int line = sc.node("Line_" + std::to_string(i + 1), body);
    if (line < 0) continue;
    if (i >= int(lines.size())) {
      sc.hide(line);
      continue;
    }
    const int text = child(sd, line, "TipText");
    sc.text(text, lines[size_t(i)]);
    if (text >= 0) {
      sc.at(text).tint = kInk;
      sc.at(text).dy = loading_layout::kTipLineDy;
    }
    const float tw = text >= 0 ? sd.measure(text, lines[size_t(i)]) : 0.0f;
    if (const int pill = child(sd, line, "Pill_Big"); pill >= 0) sc.at(pill).visible_width = tw + 22.0f;
  }

  // The scene's big pink title: the event's name on event loads (kept by
  // request), else hidden as the original's frames show it.
  const int stat = sc.node("Stat_Text");
  const int stat_text = child(sd, stat, "TitleText");
  if (hud.event_title.empty() || stat_text < 0) {
    sc.hide(stat);
  } else {
    sc.text(stat_text, hud.event_title);
    sc.at(stat_text).dx = loading_layout::kStatDx;
    sc.at(stat_text).dy = loading_layout::kStatDy;
    const float tw = sd.measure(stat_text, hud.event_title);
    for (const char* t : {"SmudgeTapeBig01", "CrispTapeBig01", "CrispTapeBig02"})
      if (const int n = child(sd, stat, t); n >= 0) sc.at(n).visible_width = tw + 80.0f;
  }

  // PLEASE WAIT and the turning spinner, where the original shows them.
  if (const int info = sc.node("Info"); info >= 0) {
    sc.at(info).dx = loading_layout::kInfoDx;
    sc.at(info).dy = loading_layout::kInfoDy;
    sc.text(child(sd, info, "TEXT_LOADING"), text_of(title_text::kPleaseWait));
    if (const int sp = sc.node("Spinner", info); sp >= 0)
      sc.at(sp).turn = -float(std::fmod(hud.t * loading_layout::kSpinnerRate, 6.2831853));
  }
  sc.draw(l, f);
  return true;
}

std::string loading_group_for_event(int style, int level) {
  switch (style) {
    case 1:  // Festival
      if (level < 0) return "festival_heats";
      if (level <= 6) return "festival_" + std::to_string(level + 1);
      return "festivalevent";
    case 2: return "streetrace";
    case 3: return "showcase";
    default: return "festivalevent";
  }
}

const std::vector<std::pair<std::string_view, std::vector<const char*>>>& frontend_scene_nodes() {
  static const std::vector<std::pair<std::string_view, std::vector<const char*>>> k = {
      {kPreSplashScene,
       {"Video", "Fader", "Start", "Install", "ButtonBar", "BREADCRUMB", "menu_shadow", "Content", "TitleTape",
        "TitleTapeText", "Logo", "Menu", "SPLASH_MENU_BUTTON1/Group/TEXT_BUTTON", "SPLASH_MENU_BUTTON1/Group/Tape",
        "SPLASH_MENU_BUTTON2/Group/TEXT_BUTTON", "SPLASH_MENU_BUTTON5/Group/Tape/Crisp_Tape",
        "SPLASH_MENU_BUTTON5/Group/Tape/smudge_tape"}},
      {kLoadingScene,
       {"Fade", "TipImage", "BG", "Title_Text", "Title_Text/TitleText", "Title_Text/crisp_tape", "Body_Text",
        "Line_1/TipText", "Line_1/Pill_Big", "Line_8/TipText", "Stat_Text", "Stat_Text/TitleText",
        "Stat_Text/SmudgeTapeBig01", "Info", "Info/TEXT_LOADING", "Info/Spinner", "Spinner/LoadSpinner"}},
  };
  return k;
}

}  // namespace carview
