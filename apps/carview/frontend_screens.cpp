#include "frontend_screens.h"

#include <algorithm>
#include <cmath>

#include "game_hud.h"
#include "race_screens.h"
#include "scene_timeline.h"
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

// The front end's timeline (docs/UI_SCENE_ANIMATION.md): the scene's root
// slides (Start, Menu_Intro, Menu, Start_Intro) and each row's (Blurred,
// Focus, Focused, Blur), driven from the model's phase as the title's
// contract fires them. The model is a value each frame, so the timeline
// follows its phase and time: a new phase goes to its slide, the time in it
// advances the timeline.
struct TitleAnim {
  SceneTimeline tl;
  bool init = false;
  Phase phase = Phase::kPressStart;
  double phase_t = 0.0;
  int focus = -1;  // the focused row's slot
  uint32_t row_comp[5] = {};
};
TitleAnim& title_anim() {
  static TitleAnim a;
  return a;
}

// The rows' components (their buttons' items).
void title_rows(TitleAnim& a, const SceneDraw& sd) {
  for (int slot = 0; slot < 5; ++slot) {
    const int item = a.tl.item_of_node(title_row_nodes(sd, slot).button);
    a.row_comp[slot] = item >= 0 ? uint32_t(item) : 0u;
  }
}

void title_focus(TitleAnim& a, int focus, bool animate) {
  for (int slot = 0; slot < 5; ++slot) {
    const uint32_t c = a.row_comp[slot];
    if (!c) continue;
    if (!animate) a.tl.go_to(c, slot == focus ? "Focused" : "Blurred");
    else if (slot == focus && slot != a.focus) a.tl.go_to(c, "Focus");
    else if (slot == a.focus && slot != focus) a.tl.go_to(c, "Blur");
  }
  a.focus = focus;
}

bool title_timeline(SceneScreen& sc, const TitleMenu& m) {
  TitleAnim& a = title_anim();
  const bool fresh = a.tl.scene() != sc.scene().scene();
  a.tl.bind(sc.scene().scene());
  if (!a.tl.ok()) return false;
  const int focus = (m.menu_visible() || m.phase == Phase::kProfile) && m.popup == Popup::kNone ? m.selected : -1;
  // A model seen anew (another phase it did not step into, or its time gone
  // back): its phase's slide from the start.
  const bool same = a.init && !fresh && a.phase == m.phase && m.phase_t >= a.phase_t;
  const bool stepped = a.init && !fresh && a.phase != m.phase;
  double dt = m.phase_t;
  if (same) {
    dt = m.phase_t - a.phase_t;
  } else if (stepped && a.phase == Phase::kPressStart && m.phase == Phase::kProfile) {
    a.tl.go_to(0, "Menu_Intro");  // MENU_INTRO: the logo goes, the menu comes
  } else if (stepped && a.phase == Phase::kProfile && m.phase == Phase::kMenu) {
    dt = std::max(0.0, TitleMenu::kProfileSeconds - a.phase_t) + m.phase_t;  // Menu_Intro plays on
  } else if (stepped && m.phase == Phase::kPressStart) {
    a.tl.go_to(0, "Start_Intro");  // back to Press Start: the logo comes back
  } else if (stepped && m.menu_visible() && (a.phase == Phase::kMenu || a.phase == Phase::kExtras)) {
    // EXTRAS and back: the same menu, its rows refocused below.
  } else {
    a.tl.reset();
    title_rows(a, sc.scene());
    a.tl.go_to(0, m.phase == Phase::kPressStart ? "Start" : m.phase == Phase::kProfile ? "Menu_Intro" : "Menu");
    a.focus = -1;
    title_focus(a, focus, false);
  }
  if (!a.init || fresh) title_rows(a, sc.scene());
  if (focus != a.focus) title_focus(a, focus, true);
  a.tl.advance(float(dt));
  a.init = true;
  a.phase = m.phase;
  a.phase_t = m.phase_t;
  return sc.play(a.tl);
}

// The tape behind a selected row ends a little past its text.
constexpr float kRowTapePad = 34.0f;
// SPLASH_MENU's row tape colour (the rows' Material845, 0.98 0 0.39).
constexpr uint32_t kRowTape = ui::rgba(250, 0, 99);
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

TitleRowNodes title_row_nodes(const SceneDraw& sd, int slot) {
  TitleRowNodes r;
  r.button = sd.find("SPLASH_MENU_BUTTON" + std::to_string(slot + 1));
  r.group = child(sd, r.button, "Group");
  if (slot == 0 && r.button < 0) {
    r.button = sd.find("SPLASH_MENU_BUTTON_SPECIAL");
    const int g = child(sd, r.button, "Group");
    r.group = child(sd, g, "NormalMode");
    r.special = child(sd, g, "SpecialMode");
  }
  return r;
}

namespace {

// Greedy word wrap to `width` px in `ts`.
std::vector<std::string> wrap_words(std::string_view text, const ui::TextStyle& ts, float width) {
  std::vector<std::string> lines;
  std::string line;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find(' ', at);
    if (end == std::string_view::npos) end = text.size();
    const std::string word(text.substr(at, end - at));
    const std::string next = line.empty() ? word : line + " " + word;
    if (!line.empty() && ui::List::measure(next, ts) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = next;
    }
    at = end + 1;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

}  // namespace

// A new profile's VIDEO screen (830_GAMMA_ADJUST) as orig1 f00030 shows it
// (1280x720 canvas, measured): the VIDEO tape top left, the black test panel
// with its three boxes (NOT / BARELY / EASILY VISIBLE, dark text on darker
// fills), BRIGHTNESS's slider and SUBTITLES's spinner under it on the pink
// and dark bars, the DESCRIPTION panel right, A SAVE and Y RESET TO DEFAULT.
// The whole block is turned 3 degrees as the scene's is. Interim: drawn by
// hand at the capture's places. The scene's text nodes rest where the
// capture has them (Text_Label 231,551, TEXT_DESCRIPTION 820,179), so the
// follow-up is to bind 830 through SceneScreen as 837's sliders are.
bool draw_video_screen(ui::List& l, const TitleMenu& m, float w, float h) {
  namespace t = title_text;
  const SceneFrame f = SceneScreen::frame(w, h);
  const float s = f.s;
  constexpr float kTurn = -0.052f;
  ui4::title_tape(l, s, f.x(150.0f), f.y(110.0f), text_of(t::kVideo), ui4::kBrushBlack, 46.0f, 300.0f);

  // The test panel and its boxes.
  l.turn(f.x(504.0f), f.y(343.0f), kTurn);
  l.rect(f.x(214.0f), f.y(168.0f), f.x(794.0f), f.y(518.0f), ui::rgba(3, 3, 3, 250));
  struct Box {
    float y0, y1;
    uint32_t fill;
    StringRef text;
  };
  const Box boxes[] = {{188.0f, 282.0f, ui::rgba(5, 5, 5), t::kNotVisible},
                       {282.0f, 380.0f, ui::rgba(9, 9, 9), t::kBarelyVisible},
                       {380.0f, 494.0f, ui::rgba(20, 20, 20), t::kEasilyVisible}};
  const ui::TextStyle box_ts = ui4::style(ui::Font::kD, 38.0f * s, ui::rgba(3, 3, 3), ui::Align::kCenter);
  for (const Box& b : boxes) {
    l.rect(f.x(236.0f), f.y(b.y0), f.x(772.0f), f.y(b.y1), b.fill);
    l.text(f.x(504.0f), f.y(0.5f * (b.y0 + b.y1) - 18.0f), text_of(b.text), box_ts);
  }

  // The rows: the selected on pink, the other on a dark bar.
  const ui::TextStyle label_ts = ui4::style(ui::Font::kD, 17.0f * s, kWhite);
  const ui::TextStyle value_ts = ui4::style(ui::Font::kB, 17.0f * s, kWhite, ui::Align::kCenter);
  for (int row = 0; row < 2; ++row) {
    const float y0 = 520.0f + 28.0f * float(row), y1 = y0 + 26.0f;
    const bool sel = row == m.video_row;
    l.rect(f.x(222.0f), f.y(y0), f.x(806.0f), f.y(y1), sel ? ui4::kPink : ui::rgba(33, 33, 33, 235));
    l.text(f.x(232.0f), f.y(y0 + 4.0f), text_of(row == 0 ? t::kBrightness : t::kSubtitles), label_ts);
    ui4::opt_arrow(l, f.x(558.0f), f.y(y0 + 13.0f), 18.0f * s, -1);
    ui4::opt_arrow(l, f.x(794.0f), f.y(y0 + 13.0f), 18.0f * s, 1);
    if (row == 0) {
      ui4::opt_slider(l, f.x(594.0f), f.y(y0 + 7.0f), f.x(732.0f), f.y(y1 - 7.0f), float(m.brightness) / 100.0f, s,
                      ui::rgba(232, 234, 232), sel);
      l.text(f.x(752.0f), f.y(y0 + 4.0f), std::to_string(m.brightness), value_ts);
    } else {
      l.text(f.x(678.0f), f.y(y0 + 4.0f), text_of(m.subtitles ? t::kEnable : t::kDisable), value_ts);
    }
    l.add_hit(f.x(222.0f), f.y(y0), f.x(806.0f), f.y(y1), ui::kHitRow, row);
  }
  l.unturn();

  // DESCRIPTION: its title on an off-white strip, the row's text under it.
  l.turn(f.x(945.0f), f.y(250.0f), kTurn);
  l.rect(f.x(806.0f), f.y(170.0f), f.x(1084.0f), f.y(362.0f), ui::rgba(14, 14, 16, 205));
  ui4::offwhite_strip(l, f.x(812.0f), f.y(140.0f), f.x(1068.0f), f.y(170.0f), s);
  l.text(f.x(818.0f), f.y(149.0f), text_of(t::kDescription), ui4::style(ui::Font::kB, 18.0f * s, kInk));
  const ui::TextStyle desc_ts =
      ui4::style(ui::Font::kC, 13.0f * s, ui::rgba(222, 222, 222), ui::Align::kLeft, true);
  const std::string desc = text_of(m.video_row == 0 ? t::kBrightnessDesc : t::kSubtitlesDesc);
  float y = 182.0f;
  for (const std::string& line : wrap_words(desc, desc_ts, 250.0f * s)) {
    l.text(f.x(822.0f), f.y(y), line, desc_ts);
    y += 27.0f;
  }
  l.unturn();

  ui4::button_bar(l, {{"A", text_of(t::kSave)}, {"Y", text_of(t::kResetDefault)}}, w, h);
  return true;
}

bool draw_title_screen(ui::List& l, const TitleScreenHud& ts, float w, float h) {
  if (ts.menu.phase == Phase::kVideo) return draw_video_screen(l, ts.menu, w, h);
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

  // The scene's timeline: the logo's and the menu's fades, the rows' looks.
  const bool timeline = title_timeline(sc, m);

  // Press Start: the logo and PRESS START (Content), where the original's
  // intro leaves them; Menu_Intro fades them out.
  const int content = sc.node("Content");
  if (content >= 0) {
    if (m.menu_visible()) {
      sc.hide(content);
    } else {
      sc.at(content).dx = title_layout::kContentDx;
      sc.at(content).dy = title_layout::kContentDy;
      // No timeline: the logo fades over the step's first 0.35 s.
      if (!timeline && m.phase == Phase::kProfile) {
        const float u = float(std::clamp(m.phase_t / 0.35, 0.0, 1.0));
        sc.show(content, 1.0f - u * u * (3.0f - 2.0f * u));
      }
      if (const int tape = sc.node("TitleTape"); tape >= 0 && title_layout::kPressStartScale != 1.0f)
        sc.at(tape).scale = title_layout::kPressStartScale;
      sc.text(sc.node("TitleTapeText"), text_of(title_text::kPressStart));
      // Anywhere on Press Start confirms (the mouse).
      if (m.phase == Phase::kPressStart) l.add_hit(0, 0, w, h, ui::kHitConfirm, 0);
    }
  }

  // The menu: SPLASH_MENU's rows, their looks from their slides (the
  // focused one on its pink tape). Menu_Intro fades it in over the profile
  // step.
  const int menu = sc.node("Menu");
  if (!m.menu_visible() && (m.phase != Phase::kProfile || !timeline)) {
    sc.hide(menu);
  } else {
    const auto rows = m.phase == Phase::kProfile ? std::vector<Row>{Row::kSinglePlayer, Row::kMultiplayer} : m.rows();
    for (int slot = 0; slot < 5; ++slot) {
      const TitleRowNodes rn = title_row_nodes(sd, slot);
      const int btn = rn.button;
      if (btn < 0) continue;
      if (slot >= int(rows.size())) {
        sc.hide(btn);
        continue;
      }
      sc.hide(rn.special);
      const int group = rn.group;
      const int text = child(sd, group, "TEXT_BUTTON");
      const int tape = child(sd, group, "Tape");
      const std::string label = text_of(title_row_text(rows[size_t(slot)]));
      sc.text(text, label);
      const bool sel = slot == m.selected && m.popup == Popup::kNone;
      // The text node draws at its group's scale (0.7).
      const float tw = text >= 0 ? sd.measure(text, label) : 0.0f;
      for (const char* t : {"Crisp_Tape", "smudge_tape"})
        if (const int n = child(sd, tape, t); n >= 0) sc.at(n).visible_width = tw + kRowTapePad;
      if (!timeline) {
        // No timeline: the looks measured on 02_title_menu_orig.png.
        if (text >= 0) sc.at(text).tint = sel ? kWhite : title_layout::kRowUnselected;
        if (!sel) sc.hide(tape);
        if (sel) sc.at(btn).scale = title_layout::kRowSelectedScale;
        for (const char* t : {"Crisp_Tape", "smudge_tape", "Dirt"})
          if (const int n = child(sd, tape, t); n >= 0 && sel) sc.at(n).tint = kRowTape;
      }
      if (!sel) {
        if (text >= 0) sc.at(text).dx = title_layout::kRowUnselectedDx;
      } else {
        sc.at(btn).dx = title_layout::kRowSelectedDx;
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
  // BG draws first, alone: its depth would put it over Info.
  if (const int bg = sc.node("BG"); bg >= 0) {
    if (!hud.backdrop.empty()) {
      l.rect(0, 0, w, h, ui::rgba(0, 0, 0));
      sc.at(bg).texture = hud.backdrop;
      sc.draw(l, f, bg);
    }
    sc.hide(bg);
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
       {"Video", "Fader", "Start", "Install", "ButtonBar", "menu_shadow", "Content", "TitleTape", "TitleTapeText",
        "Logo", "Menu", "SPLASH_MENU_BUTTON2/Group/TEXT_BUTTON", "SPLASH_MENU_BUTTON5/Group/Tape/Crisp_Tape",
        "SPLASH_MENU_BUTTON5/Group/Tape/smudge_tape"}},
      {kLoadingScene,
       {"Fade", "TipImage", "BG", "Title_Text", "Title_Text/TitleText", "Title_Text/crisp_tape", "Body_Text",
        "Line_1/TipText", "Line_1/Pill_Big", "Line_8/TipText", "Stat_Text", "Stat_Text/TitleText",
        "Stat_Text/SmudgeTapeBig01", "Info", "Info/TEXT_LOADING", "Info/Spinner", "Spinner/LoadSpinner"}},
  };
  return k;
}

}  // namespace carview
