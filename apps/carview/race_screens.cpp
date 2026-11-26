#include "race_screens.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "game_hud.h"
#include "pause_screen.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

using race_flow::Screen;

constexpr uint32_t kWhite = ui::rgba(255, 255, 255);
constexpr uint32_t kInk = ui::rgba(20, 20, 22);
constexpr uint32_t kTapeBlack = ui::rgba(12, 12, 12);
constexpr uint32_t kTapePink = ui4::kPink;
constexpr uint32_t kPanelGrey = ui::rgba(214, 214, 214);
constexpr uint32_t kTicketGreen = ui::rgba(4, 221, 125);
constexpr uint32_t kTicketInk = ui::rgba(46, 46, 46);

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

int child(const SceneDraw& sd, int parent, std::string_view name) {
  if (parent < 0) return -1;
  for (int c : sd.children_of(parent))
    if (sd.object_name(c) == name) return c;
  return -1;
}

std::string credits_text(int64_t v) { return format1(text_of(race_text::kCredits), thousands(v)); }

std::string ordinal(int place) { return std::to_string(place) + upper(place_suffix(place)); }

// Moves `node` (and its subtree) so its anchor lands at (x, y).
void place_at(SceneScreen& sc, int node, float x, float y) {
  if (node < 0) return;
  const auto a = sc.scene().at(node);
  sc.at(node).dx = x - a[0];
  sc.at(node).dy = y - a[1];
}

// Crops a tape group's crisp/smudge tapes to `width` past the text and tints
// them.
void fit_tapes(SceneScreen& sc, int group, float width, uint32_t tint) {
  const SceneDraw& sd = sc.scene();
  if (group < 0) return;
  for (int c : sd.children_of(group)) {
    const std::string name(sd.object_name(c));
    std::string low = name;
    for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    const bool tape = low.find("crisp") != std::string::npos || low.find("smudge") != std::string::npos;
    const bool dirt = low.find("dirt") != std::string::npos;
    if (tape) {
      sc.at(c).tint = tint;
      if (width > 0) sc.at(c).visible_width = width;
    } else if (dirt) {
      sc.at(c).tint = tint;
    }
  }
}

// Shows a node and every node under it (a scene resting at alpha 0).
void show_tree(SceneScreen& sc, int node) {
  if (node < 0) return;
  sc.show(node);
  for (int c : sc.scene().children_of(node)) show_tree(sc, c);
}

// A scene text's anchor sits about a quarter of its size below its middle
// (FINISHED's E42 box in 16b_finish_banner_orig.png).
constexpr float kTextMidAbove = 0.25f;

float ease(double t, double over) { return over <= 0 ? 1.0f : float(std::clamp(t / over, 0.0, 1.0)); }

// --- PRE_AND_POST_RACE / H_GENERIC_TITLE: the event titles --------------

// The tapes run a little past their text (12_race_intro_card_orig.png: the
// pink tape ends ~30 px past TRAILBLAZERS; the black one ~25 px past RACE).
constexpr float kTitleTapePad = 64.0f;
constexpr float kTitleTapeEnd = 30.0f;

void bind_titles(SceneScreen& sc, const RaceHud& r) {
  const SceneDraw& sd = sc.scene();
  const int mini = sc.node("MiniTape"), main = sc.node("TitleTapeText");
  const std::string type = r.event_type.empty() ? text_of(race_text::kFestivalRace) : upper(r.event_type);
  const std::string title = upper(r.title.empty() ? r.event : r.title);
  sc.text(mini, type);
  sc.text(main, title);
  if (mini >= 0) fit_tapes(sc, sc.node("MiniTitle"), sd.measure(mini, type) + kTitleTapePad, kTapeBlack);
  if (main >= 0) {
    const float tw = sd.measure(main, title);
    fit_tapes(sc, sc.node("MainTitle"), tw + kTitleTapePad, kTapePink);
    // The tape's right end follows the text (it ends ~30 px past it): the
    // end piece's shown part (its left, cropped) ends there.
    if (const int right = sc.node("MainTitle/Crisp_right"); right >= 0) {
      const auto rr = sd.rect(right);
      const float shown = std::min(tw + kTitleTapePad, rr[2] - rr[0]);
      sc.at(right).dx = sd.at(main)[0] + tw + kTitleTapeEnd - (rr[0] + shown);
    }
    // interim (SPEC_QUESTIONS Q99): the crisp tape's sheen draws as a dark gloss over the pink
    // (and, one texture, is not cropped with the tape); the capture shows a
    // flat pink tape, so it is hidden.
    sc.hide(sc.node("MainTitle/Crisp/specular"));
  }
}

bool draw_title_card(ui::List& l, const RaceHud& r, float w, float h) {
  SceneScreen sc(kRaceTitleScene);
  if (!sc.ok()) return false;
  sc.bindings().fit_text = false;
  for (const char* n : {"Winnings", "ButtonBar", "NemesisGroup", "FreeRoam", "Layer2"}) sc.hide(sc.node(n));
  bind_titles(sc, r);
  // The card is gone within 4 frames of START.
  sc.bindings().opacity = 1.0f;
  sc.draw(l, SceneScreen::frame(w, h));
  return true;
}

bool draw_grid_menu(ui::List& l, const RaceHud& r, float w, float h, bool with_back = false) {
  SceneScreen sc(kGridMenuScene);
  if (!sc.ok()) return false;
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.bindings().fit_text = false;
  sc.hide(sc.node("ButtonBar"));
  bind_titles(sc, r);
  // The menu grows in over ~10 frames (UI_LAYOUTS b1 States).
  const float grow = ease(r.screen_t, race_flow::timing::kMenuGrow);
  const int menu = sc.node("Menu");
  if (menu >= 0) {
    sc.at(menu).scale = 0.8f + 0.2f * grow;
    sc.at(menu).alpha = grow;
  }
  bind_menu_rows(sc, l, f, r.grid_menu, r.grid_selected);
  sc.draw(l, f);
  // The original's grid help bar is SELECT alone (13_prerace_menu_orig.png).
  if (with_back)
    ui4::button_bar(l, {{"A", text_of(race_text::kSelect)}, {"B", text_of(race_text::kBack)}}, w, h);
  else
    ui4::button_bar(l, {{"A", text_of(race_text::kSelect)}}, w, h);
  return true;
}

}  // namespace

void bind_menu_rows(SceneScreen& sc, ui::List& l, const SceneFrame& f, const std::vector<std::string>& rows,
                    int selected, float extra_dy) {
  const SceneDraw& sd = sc.scene();
  const int list = sc.node("Option_List");
  if (list >= 0) sc.at(list).dx = race_layout::kMenuDx, sc.at(list).dy = race_layout::kMenuDy + extra_dy;
  const int n = int(rows.size());
  for (int slot = 0; slot < 7; ++slot) {
    const int btn = sc.node("Button" + std::to_string(slot));
    if (btn < 0) continue;
    if (slot >= n) {
      sc.hide(btn);
      continue;
    }
    const int body = child(sd, btn, "Button");
    const int text = child(sd, body, "Text");
    const std::string& label = rows[size_t(slot)];
    sc.text(text, label);
    const bool sel = slot == selected;
    const float tw = sd.measure(text, label);
    for (const char* name : {"Dirt_Tile", "Crisp_Tape", "Smudge_Tape"}) {
      const int t = child(sd, body, name);
      if (t < 0) continue;
      sc.at(t).tint = sel ? kTapePink : kTapeBlack;
      if (std::string_view(name) != "Dirt_Tile") sc.at(t).visible_width = tw + 64.0f;
    }
    if (const int clip = child(sd, body, "Clipon"); clip >= 0 && sel) sc.show(clip);
    if (sel) {
      sc.at(btn).scale = race_layout::kMenuFocusScale;
      sc.at(btn).dy = race_layout::kMenuFocusDy;
    }
    const auto c = sd.at(text);
    const float k = sel ? race_layout::kMenuFocusScale : 1.0f;
    const float x0 = c[0] + race_layout::kMenuDx - 30.0f, x1 = c[0] + race_layout::kMenuDx + k * tw + 30.0f;
    const float yc = c[1] + race_layout::kMenuDy + extra_dy + (sel ? race_layout::kMenuFocusDy : 0.0f);
    l.add_hit(f.x(x0), f.y(yc - 24.0f * k), f.x(x1), f.y(yc + 24.0f * k), ui::kHitRow, slot);
  }
}

namespace {

// --- H_POST_RACE_FINISHED ----------------------------------------------

// 16b_finish_banner_orig.png: the place's text from x 95 (its node rests at
// its intro start, x -895); the black tape behind the pink is the intro's.
// Measured on the same capture: FINISHED sits 45 px lower than its rest,
// the place 20 px lower and turned ~0.9 degrees less.
constexpr float kPlaceDx = 990.0f, kPlaceDy = 20.0f, kPlaceTurn = -0.016f, kFinishedDy = 45.0f;

bool draw_finished(ui::List& l, const RaceHud& r, float w, float h, float opacity, bool rally = false) {
  SceneScreen sc(kFinishedScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  sc.bindings().opacity = opacity;
  for (const char* n : {"ButtonBar", "Timer", "OnlineText", "OfflineText", "Tape"}) sc.hide(sc.node(n));
  const int title = sc.node("TEXT_TITLE"), pos = sc.node("TEXT_POSITION");
  const std::string fin = text_of(race_text::kFinished);
  sc.text(title, fin);
  if (title >= 0) fit_tapes(sc, sc.node("ScreenTitle"), sd.measure(title, fin) + kTitleTapePad, kTapeBlack);
  if (const int st = sc.node("ScreenTitle"); st >= 0) sc.at(st).dy = kFinishedDy;
  const std::string place = upper(format1(text_of(race_text::kPlace), ordinal(std::max(1, r.place))));
  sc.text(pos, place);
  if (pos >= 0) {
    sc.at(pos).dx = kPlaceDx, sc.at(pos).dy = kPlaceDy;
    sc.at(pos).extra_angle = kPlaceTurn;
    sc.at(pos).alpha = 1.0f;
  }
  fit_tapes(sc, sc.node("Tape2"), 0.0f, kTapePink);
  sc.draw(l, SceneScreen::frame(w, h));
  // A head-to-head's win (race_encounters.xml PlayWinCutScene): no restart,
  // results or replay in free roam.
  // interim (SPEC_QUESTIONS Q171): its help bar is not captured.
  // A Rally stage's banner (CPostRaceFinishPosition): CONTINUE,
  // RESTART only once the first Rally is completed (inferred), RACE RESULTS
  // offline, VIEW REPLAY with a replay. The engine shows A CONTINUE and
  // X RACE RESULTS (interim, Q251).
  if (opacity >= 1.0f && rally) {
    ui4::button_bar(l, {{"A", text_of(race_text::kContinue)}, {"X", text_of(race_text::kRaceResults)}}, w, h);
  } else if (opacity >= 1.0f && r.head_to_head) {
    ui4::button_bar(l, {{"A", text_of(race_text::kContinue)}}, w, h);
  } else if (opacity >= 1.0f) {
    ui4::button_bar(l,
                    {{"A", text_of(race_text::kContinue)},
                     {"Y", text_of(race_text::kRestart)},
                     {"X", text_of(race_text::kRaceResults)},
                     {"B", text_of(race_text::kViewReplay)}},
                    w, h);
  }
  return true;
}

// --- H_POST_RACE_CREDITS_BREAKDOWN ----------------------------------------

// 17_results_cash_reward_orig.png (UI_LAYOUTS c): CASH REWARD's box at
// 108,151; the lines from x 148 at y 243 / 271 / 299; TOTAL at 140,390; its
// value on the pink brush from x 515, centred ~30 px above TOTAL's middle; BALANCE: in its panel at 165,492. The
// scene's groups rest at their intro starts (the stackers are not run).
constexpr float kCashTitleX = 112.0f, kCashTitleY = 192.0f, kCashTitlePad = 24.0f;
constexpr float kCashLineX = 148.0f, kCashLineY = 262.0f, kCashLinePitch = 28.0f, kCashLinePad = 6.0f;
// The labels have no tape of their own in the capture (a dark smudge at most).
constexpr uint32_t kCashLabelTape = ui::rgba(12, 12, 12, 0);
constexpr float kCashValuePad = 14.0f;
constexpr float kCashValueGap = 10.0f;
// The values sit ~7 px above their labels' middles (0 Cr against FINISHED 5TH).
constexpr float kCashValueDy = -7.0f;
constexpr float kTotalX = 140.0f, kTotalY = 450.0f, kTotalPad = 28.0f;
constexpr float kTotalValueX = 515.0f, kTotalValueY = 418.0f;
constexpr float kBalanceX = 165.0f, kBalanceY = 500.0f, kBalancePad = 8.0f;

// Puts a tape model (cropped from its left) behind a text whose anchor is at
// (x, y), `tw` wide at font size `size`: the tapes' rest positions belong to
// the screen's intro (its stackers are not run).
void tape_at(SceneScreen& sc, int tape, float x, float y, float tw, float size, float pad, uint32_t tint) {
  if (tape < 0) return;
  const auto r = sc.scene().rect(tape);
  sc.at(tape).dx = (x - pad) - r[0];
  sc.at(tape).dy = (y - kTextMidAbove * size) - 0.5f * (r[1] + r[3]);
  sc.at(tape).visible_width = tw + 2.0f * pad;
  sc.at(tape).tint = tint;
}

// Moves a text node's anchor to (x, y).
void text_at(SceneScreen& sc, int node, float x, float y) {
  if (node < 0) return;
  const auto a = sc.scene().at(node);
  sc.at(node).dx = x - a[0];
  sc.at(node).dy = y - a[1];
}

// A text with its tapes (the group's crisp/smudge models) behind it; the
// group's dirt is hidden (its place is the intro's).
// The texts go over their tapes (the scene draws a group's text first):
// each is drawn after the scene.
struct LateText {
  int node;
  std::string s;
  float dx, dy;
  uint32_t colour;
};

void tape_text(SceneScreen& sc, std::vector<LateText>& late, int group, int text, const std::string& s, float x, float y,
               float size, float pad, uint32_t tint, uint32_t colour = kWhite) {
  const SceneDraw& sd = sc.scene();
  if (group < 0 || text < 0) return;
  const auto a = sd.at(text);
  late.push_back({text, s, x - a[0], y - a[1], colour});
  const float tw = sd.measure(text, s);
  for (int c : sd.children_of(group)) {
    std::string low(sd.object_name(c));
    for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (low.find("crisp") != std::string::npos || low.find("smudge") != std::string::npos)
      tape_at(sc, c, x, y, tw, size, pad, tint);
    else if (low.find("dirt") != std::string::npos)
      sc.hide(c);
  }
}

bool draw_cash(ui::List& l, const RaceHud& r, float w, float h) {
  // The banner goes out first (~60 frames after A).
  if (r.screen_t < race_flow::timing::kBannerOut)
    return draw_finished(l, r, w, h, 1.0f - ease(r.screen_t, race_flow::timing::kBannerOut));
  const double t = r.screen_t - race_flow::timing::kBannerOut;
  SceneScreen sc(kCashScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  sc.hide(sc.node("ButtonBar"));
  const int tg = sc.node("RotationHelper/ScreenTitle");
  std::vector<LateText> late;
  tape_text(sc, late, tg, child(sd, tg, "TEXT_TITLE"), text_of(race_text::kCashReward), kCashTitleX, kCashTitleY, 38.0f,
            kCashTitlePad, kTapeBlack);
  // The lines: the finish, the mileage, the difficulty and assists bonus.
  std::vector<std::pair<std::string, std::string>> lines;
  lines.push_back({format1(text_of(race_text::kFinishedPlace), ordinal(std::max(1, r.place))), credits_text(r.base_credits)});
  // The payout's order: FINISHED, BEAT THE STAR, MILEAGE BONUS, then the
  // DIFFICULTY & ASSISTS percentage. SKILLS is an XP-screen row, not cash.
  if (r.nemesis_bonus > 0) lines.push_back({text_of(race_text::kBeatTheStar), credits_text(r.nemesis_bonus)});
  if (r.mileage_bonus > 0) lines.push_back({text_of(race_text::kMileageBonus), credits_text(r.mileage_bonus)});
  if (r.difficulty_pct > 0)
    lines.push_back({text_of(race_text::kDifficultyBonus), "+" + std::to_string(r.difficulty_pct) + "%"});
  // The first line with the title; the rest within 30 frames.
  const int shown = t < race_flow::timing::kCashLines ? 1 : int(lines.size());
  for (int i = 0; i < 5; ++i) {
    const int item = sc.node("Breakdown/Item" + std::to_string(i + 1));
    if (item < 0) continue;
    if (i >= int(lines.size()) || i >= shown) {
      sc.hide(item);
      continue;
    }
    show_tree(sc, item);
    const int tt = child(sd, item, "TitleTape"), vt = child(sd, item, "ValueTape");
    const int label = child(sd, tt, "TEXT_LABEL"), value = child(sd, vt, "TEXT_VALUE");
    const float y = kCashLineY + kCashLinePitch * float(i);
    const auto& [ls, vs] = lines[size_t(i)];
    tape_text(sc, late, tt, label, ls, kCashLineX, y, 20.0f, kCashLinePad, kCashLabelTape);
    const float lw = label >= 0 ? sd.measure(label, ls) : 0.0f;
    tape_text(sc, late, vt, value, vs, kCashLineX + lw + kCashValueGap, y + kCashValueDy, 22.0f, kCashValuePad, kTapePink);
  }
  // The total and the balance (with the rest of the lines).
  const int total = sc.node("TotalAlign"), balance = sc.node("Balance");
  if (t < race_flow::timing::kCashLines) {
    sc.hide(total);
    sc.hide(balance);
  } else {
    const int ttl = sc.node("Total/Title"), vg = sc.node("Total/Value/Value");
    tape_text(sc, late, ttl, child(sd, ttl, "TEXT_WINNING"), text_of(race_text::kTotal), kTotalX, kTotalY, 66.0f,
              kTotalPad, kTapeBlack);
    tape_text(sc, late, vg, child(sd, vg, "TEXT_WINNING"), credits_text(r.credits), kTotalValueX, kTotalValueY, 66.0f,
              kTotalPad, kTapePink);
    const int bt = sc.node("TEXT_BALANCE_TITLE"), bv = sc.node("TEXT_BALANCE");
    if (r.balance >= 0 && bt >= 0 && bv >= 0) {
      const std::string btxt = text_of(race_text::kBalance), bval = credits_text(r.balance);
      sc.text(bt, btxt);
      sc.text(bv, bval);
      text_at(sc, bt, kBalanceX, kBalanceY);
      const float bw = sd.measure(bt, btxt);
      text_at(sc, bv, kBalanceX + bw + 6.0f, kBalanceY);
      sc.at(bt).tint = kInk;
      sc.at(bv).tint = kInk;
      for (int c : sd.children_of(balance)) {
        const std::string_view nm = sd.object_name(c);
        if (nm == "Pill")
          tape_at(sc, c, kBalanceX, kBalanceY, bw + 6.0f + sd.measure(bv, bval), 18.0f, kBalancePad, kPanelGrey);
        else if (nm != "TEXT_BALANCE" && nm != "TEXT_BALANCE_TITLE")
          sc.hide(c);
      }
    } else {
      sc.hide(balance);
    }
  }
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.draw(l, f);
  for (const auto& lt : late) sd.text(l, f, lt.node, lt.s, lt.colour, lt.dx, lt.dy, false);
  return true;
}
// --- PRE_AND_POST_RACE: the rival callout ---------------------------------

// 14_rival_intro_orig.png: Beat <name> on the big black tape (NemesisTape)
// from x 80, its middle at y 517, the tape ending ~25 px past it; under it
// REWARD (NemesisName's title) from x 80 at y 587 and the reward on the pink
// tape (NemesisName/Group) after it. The groups rest at their intro starts
// (NemesisTape off-screen left), so they are placed here.
constexpr float kRivalX = 80.0f, kRivalNameMidY = 517.0f, kRivalRewardMidY = 587.0f;
constexpr float kRivalNameSize = 79.0f, kRivalRewardSize = 40.0f;
constexpr float kRivalTapeEnd = 25.0f, kRivalRewardGap = 26.0f, kRivalRewardPad = 14.0f;
// The reward sits 12 px above REWARD (the line tilts up). interim
// (SPEC_QUESTIONS Q95): the tapes land lower than their static rects say;
// these offsets fit them to the capture (the big tape 69 px, the pink 20 px).
constexpr float kRivalRewardValueMidY = 575.0f, kRivalBigTapeDy = -69.0f, kRivalRewardTapeDy = -20.0f;

bool is_tape(const SceneDraw& sd, int node) {
  std::string low(sd.object_name(node));
  for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return low.find("crisp") != std::string::npos || low.find("smudge") != std::string::npos;
}

bool draw_rival_intro(ui::List& l, const RaceHud& r, float w, float h) {
  SceneScreen sc(kRaceTitleScene);
  if (!sc.ok()) return false;
  // nemesis_show's UI trigger: the callout from 0.8 s for 10 s.
  if (r.screen_t < race_flow::timing::kRivalUiAt ||
      r.screen_t >= race_flow::timing::kRivalUiAt + race_flow::timing::kRivalUiFor)
    return true;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  for (const char* n : {"Winnings", "ButtonBar", "FreeRoam", "Layer2", "TitleGroup"}) sc.hide(sc.node(n));
  std::vector<LateText> late;
  // Beat <name>: the scene draws this tape over its text, so the text goes
  // after the scene.
  const int big = sc.node("NemesisTape"), name = sc.node("TEXT_NEMESIS_NAME");
  if (big >= 0 && name >= 0) {
    const std::string beat = format1(text_of(race_text::kBeat), r.rival_name);
    const auto a = sd.at(name);
    const float dx = kRivalX - a[0], dy = kRivalNameMidY + kTextMidAbove * kRivalNameSize - a[1];
    sc.at(big).dx = dx;
    sc.at(big).dy = dy;
    late.push_back({name, beat, dx, dy, kWhite});
    const float end = kRivalX + sd.measure(name, beat) + kRivalTapeEnd;
    for (int c : sd.children_of(big)) {
      if (!is_tape(sd, c)) continue;
      sc.at(c).tint = kTapeBlack;
      sc.at(c).visible_width = end - (sd.rect(c)[0] + dx);
      sc.at(c).dy = kRivalBigTapeDy;
    }
  }
  // REWARD, then the reward on its pink tape.
  const int line = sc.node("NemesisName"), title = sc.node("TEXT_BONUS_TITLE"), bonus = sc.node("TEXT_BONUS");
  const int group = sc.node("NemesisName/Group");
  // No BEAT THE STAR (a headline race's star pays none): no REWARD line.
  if (r.rival_reward <= 0) sc.hide(line);
  if (line >= 0 && title >= 0 && r.rival_reward > 0) {
    const std::string rw = text_of(race_text::kReward);
    const auto a = sd.at(title);
    const float dx = kRivalX - a[0], dy = kRivalRewardMidY + kTextMidAbove * kRivalRewardSize - a[1];
    sc.at(line).dx = dx;
    sc.at(line).dy = dy;
    sc.text(title, rw);
    const float tw = sd.measure(title, rw);
    fit_tapes(sc, line, tw + kTitleTapePad, kTapeBlack);
    if (const int dirt = child(sd, line, "dirt"); dirt >= 0) sc.hide(dirt);
    // interim (SPEC_QUESTIONS Q99): the sheen, uncropped, runs past the tape.
    if (const int sp = sc.node("NemesisName/Crisp/specular"); sp >= 0) sc.hide(sp);
    if (group >= 0 && bonus >= 0) {
      const std::string reward = credits_text(r.rival_reward);
      const auto b = sd.at(bonus);
      const float x = kRivalX + tw + kRivalRewardGap;
      // The group moves with the line; it lands the reward after REWARD.
      const float vy = kRivalRewardValueMidY + kTextMidAbove * kRivalRewardSize;
      const float gdx = x - (b[0] + dx), gdy = vy - (b[1] + dy);
      sc.at(group).dx = gdx;
      sc.at(group).dy = gdy;
      sc.text(bonus, reward);
      const float y = vy;
      for (int c : sd.children_of(group)) {
        if (is_tape(sd, c)) {
          tape_at(sc, c, x, y, sd.measure(bonus, reward), kRivalRewardSize, kRivalRewardPad, kTapePink);
          sc.at(c).dx -= dx + gdx;  // tape_at places it absolutely; the groups' moves add on
          sc.at(c).dy -= dy + gdy - kRivalRewardTapeDy;
        } else if (std::string_view(sd.object_name(c)).find("dirt") != std::string_view::npos) {
          sc.hide(c);
        }
      }
    }
  }
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.draw(l, f);
  for (const auto& lt : late) sd.text(l, f, lt.node, lt.s, lt.colour, lt.dx, lt.dy, false);
  return true;
}

// --- H_RACE_ENCOUNTER ------------------------------------------------------

// A head-to-head's intro (CShowRaceEncounterIntroScreen): the rival's name,
// the car's make, its name and class, then REWARD and the reward on pink.
// interim (SPEC_QUESTIONS Q171): no capture; the scene's own rects, the
// tapes cut to their texts, and the reward lines (alpha 0 at rest, brought
// in by the intro) shown at their rects.
bool draw_encounter_intro(ui::List& l, const RaceHud& r, float w, float h) {
  SceneScreen sc(kEncounterScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  auto line = [&](const char* group, const char* text, const std::string& s, uint32_t tint) {
    const int g = sc.node(group), t = sc.node(text);
    if (s.empty() || g < 0 || t < 0) {
      sc.hide(g);
      return;
    }
    show_tree(sc, g);
    sc.text(t, s);
    fit_tapes(sc, g, sd.measure(t, s) + kTitleTapePad, tint);
  };
  line("Name", "TEXT_NAME", r.rival_name, kTapeBlack);
  line("Manufacturer", "TEXT_CARMAKE", upper(r.encounter_make), kTapeBlack);
  if (const int car = sc.node("TEXT_CAR_NAME"); car >= 0) sc.text(car, r.encounter_car);
  if (!r.encounter_class.empty()) {
    const int pi = sc.node("TEXT_PI");
    sc.text(pi, std::to_string(r.encounter_pi));
    if (pi >= 0) sc.at(pi).tint = kInk;
    if (const int cls = sc.node("CLASS_A"); cls >= 0) sc.at(cls).texture = "PICLASS/CLASS_" + upper(r.encounter_class);
  } else {
    sc.hide(sc.node("CLASS_SM"));
  }
  if (r.rival_reward > 0) {
    line("Reward", "TEXT_REWARD", text_of(race_text::kReward), kTapeBlack);
    line("Credits", "TEXT_CREDITS", credits_text(r.rival_reward), kTapePink);
  } else {
    sc.hide(sc.node("Reward"));
    sc.hide(sc.node("Credits"));
  }
  sc.draw(l, SceneScreen::frame(w, h));
  return true;
}

// --- H_POST_RACE_CAREER_POINTS ----------------------------------------------

// interim (SPEC_QUESTIONS Q96): the points screen's scene rests fully
// transparent (its intro is a state timeline). The engine shows its title,
// the main tape (the points earned) and the wristband bar at the measured
// rects of UI_LAYOUTS c (POINTS EARNED at 103,160; the bar under it); the
// wristband art and the COLLECT panel's packing are not reproduced.
constexpr float kPointsTitleY = 168.0f;

bool draw_points(ui::List& l, const RaceHud& r, float w, float h, std::string_view scene = kPointsScene,
                 const std::string* to_next = nullptr) {
  SceneScreen sc(scene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  sc.hide(sc.node("ButtonBar"));
  show_tree(sc, sc.node("RotationHelper"));
  for (const char* n : {"LargeTape", "CarName"}) sc.hide(sc.node(n));
  const int tgroup = sc.node("RotationHelper/Title");
  const int title = child(sd, tgroup, "TEXT_TITLE");
  const std::string pe = text_of(race_text::kPointsEarned);
  sc.text(title, pe);
  if (title >= 0) {
    sc.at(tgroup).dy = kPointsTitleY - sd.at(title)[1];
    fit_tapes(sc, tgroup, sd.measure(title, pe) + kTitleTapePad, kTapeBlack);
  }
  const int earned = std::max(0, r.band_after - std::max(0, r.band_before));
  const int main = sc.node("MainTape");
  sc.text(child(sd, main, "TEXT_TITLE"), "+" + std::to_string(earned));
  sc.text(child(sd, main, "TEXT_MAIN"), upper(format1(text_of(race_text::kPointsN), std::to_string(earned))));
  fit_tapes(sc, main, 0.0f, kTapePink);
  for (const char* n : {"TEXT_MAIN", "CrispTapeBig02", "SmudgeTapeBig02", "dirt2"}) sc.hide(child(sd, main, n));
  show_tree(sc, sc.node("LevelBar"));
  if (r.band_hi > 0) {
    const int left = std::max(0, r.band_hi - std::max(0, r.band_after));
    sc.text(sc.node("TEXT_POINTS_TO_NEXT"),
            format1(text_of(left == 1 ? race_text::kPointToNext : race_text::kPointsToNext), std::to_string(left)));
  }
  if (r.band_up) {
    sc.text(sc.node("TEXT_POINTS_TO_NEXT"), text_of(race_text::kCollectIt));
  }
  if (to_next) sc.text(sc.node("TEXT_POINTS_TO_NEXT"), *to_next);
  if (!r.band_name.empty())
    if (const int wc = sc.node("WristbandCurrent/Rectangle"); wc >= 0)
      sc.at(wc).texture = "WRISTBANDS/" + upper(r.band_name.substr(0, r.band_name.find(' ')));
  if (!r.band_next.empty())
    if (const int wn = sc.node("NextReward/Wristband"); wn >= 0)
      sc.at(wn).texture = "WRISTBANDS/" + upper(r.band_next.substr(0, r.band_next.find(' ')));
  sc.hide(sc.node("HeadlineEvent"));
  sc.hide(sc.node("GlowMover"));
  // The fill: the band's span covered.
  float frac = 0.0f;
  if (r.band_hi > r.band_lo) frac = float(std::max(0, r.band_after) - r.band_lo) / float(r.band_hi - r.band_lo);
  frac = std::clamp(frac, 0.0f, 1.0f);
  for (const char* n : {"FillBar", "FillBar2"})
    if (const int fb = sc.node(n); fb >= 0) {
      sc.at(fb).visible_width = std::max(1.0f, (sd.rect(fb)[2] - sd.rect(fb)[0]) * frac);
      sc.at(fb).tint = ui4::kYellow;
    }
  sc.draw(l, SceneScreen::frame(w, h));
  ui4::button_bar(l, {{"A", text_of(race_text::kContinue)}}, w, h);
  return true;
}

// --- H_POST_RACE_WON_CAR -----------------------------------------------------

// interim (SPEC_QUESTIONS Q96): the won car's card rests at its intro start
// like the banner (TEXT_POSITION off-screen); the engine shows YOU WON A CAR!
// as the title and the car's name on the pink tape, the banner's offsets.
bool draw_won_car(ui::List& l, const RaceHud& r, float w, float h) {
  SceneScreen sc(kWonCarScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  sc.hide(sc.node("ButtonBar"));
  const int title = sc.node("ScreenTitle/TEXT_TITLE"), pos = sc.node("TEXT_POSITION");
  const std::string won = text_of(race_text::kYouWonACar);
  sc.text(title, won);
  if (title >= 0) fit_tapes(sc, sc.node("ScreenTitle"), sd.measure(title, won) + kTitleTapePad, kTapeBlack);
  sc.text(pos, upper(r.won_car_name));
  if (pos >= 0) {
    sc.at(pos).dx = kPlaceDx, sc.at(pos).dy = kPlaceDy;
    sc.at(pos).extra_angle = kPlaceTurn;
    sc.at(pos).alpha = 1.0f;
  }
  fit_tapes(sc, sc.node("Tape2"), 0.0f, kTapePink);
  sc.hide(sc.node("CarName"));
  sc.draw(l, SceneScreen::frame(w, h));
  ui4::button_bar(l, {{"A", text_of(race_text::kContinue)}}, w, h);
  return true;
}

// --- H_MESSAGE_POPUP: STAY IN CAR ----------------------------------------------

// 18_post_race_stay_in_car_orig.png: the title on its pill from x 446 (its
// middle at y 307), the message's lines under it 35 px apart (its first
// line's middle at y 338), the description panel behind them from y 318 to
// 428 (45 px under the second line), and the scene behind dimmed (its
// Background). The panel rests at its intro start (low on the screen) and
// at its full height, and the scene's text draws its lines closer than the
// capture's, so both are placed here; the panel is cut to the lines.
constexpr float kPopupDim = 0.45f;
constexpr float kPopupMessageWidth = 390.0f;
constexpr float kPopupTitleDy = 12.0f, kPopupMessageDy = 8.0f, kPopupLinePitch = 35.0f;
// The panel's mask shows its model only from 332 px down at rest.
constexpr float kPopupPanelTop = 318.0f, kPopupPanelShown = 332.0f;
constexpr float kPopupPanelTwoLines = 110.0f;

}  // namespace

bool draw_message_popup(ui::List& l, const std::string& title_text, const std::string& message,
                        const std::vector<ui4::Prompt>& prompts, float w, float h) {
  SceneScreen sc(kPopupScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  sc.hide(sc.node("SpinnerBox"));
  sc.hide(sc.node("ButtonBar"));
  const int bg = sc.node("Background");
  if (bg >= 0) {
    sc.show(bg, kPopupDim);
    sc.at(bg).tint = ui::rgba(0, 0, 0);
  }
  const int title = sc.node("Title"), msg = sc.node("Message");
  sc.text(title, title_text);
  if (title >= 0) {
    sc.at(title).tint = kInk;
    sc.at(title).dy = kPopupTitleDy;
  }
  // The message, a line at a time at the capture's pitch.
  std::vector<std::string> lines;
  if (msg >= 0) {
    const std::string wrapped = sc.wrap(msg, message, kPopupMessageWidth);
    size_t from = 0;
    for (size_t nl; (nl = wrapped.find('\n', from)) != std::string::npos; from = nl + 1)
      lines.push_back(wrapped.substr(from, nl - from));
    lines.push_back(wrapped.substr(from));
    sc.at(msg).dy = kPopupMessageDy;
  }
  // The panel's shown part (from kPopupPanelShown down) squeezed about the
  // model's middle to the lines, its top at kPopupPanelTop.
  if (const int panel = sc.node("DescriptionBox/Panel"); panel >= 0) {
    const auto rc = sd.rect(panel);
    const float tall = rc[3] - rc[1], shown = tall - kPopupPanelShown;
    const float want = kPopupPanelTwoLines + kPopupLinePitch * (float(std::max<size_t>(lines.size(), 1)) - 2.0f);
    const float s = shown > 1.0f ? std::clamp(want / shown, 0.1f, 1.0f) : 1.0f;
    sc.at(panel).stretch_y = s;
    sc.at(panel).dy = kPopupPanelTop - (0.5f * (rc[1] + rc[3]) - 0.5f * s * tall + s * kPopupPanelShown);
  }
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.draw(l, f);
  for (size_t i = 0; i < lines.size(); ++i)
    sd.text(l, f, msg, lines[i], kWhite, 0.0f, kPopupMessageDy + kPopupLinePitch * float(i), false);
  ui4::button_bar(l, prompts, w, h);
  return true;
}

namespace {

bool draw_stay_in_car(ui::List& l, const RaceHud& r, float w, float h) {
  return draw_message_popup(l, text_of(race_text::kStayInCar), format1(text_of(race_text::kStayInCarQ), r.stay_car),
                            {{"A", text_of(race_text::kYes)}, {"B", text_of(race_text::kNo)}}, w, h);
}

// The black-out before the load: a darken, then black.
void draw_black_out(ui::List& l, const RaceHud& r, float w, float h) {
  // kDone holds the black the fade reached until free roam loads.
  const float a = r.screen == Screen::kDone ? 1.0f : ease(r.screen_t, race_flow::timing::kBlackFade);
  l.rect(0, 0, w, h, ui::rgba(0, 0, 0, uint8_t(std::lround(255.0f * a))));
}

// --- H_HORIZON_CHAMPION ------------------------------------------------------

// The champion's screen over Rally_Champion_Cutscene (game_rally_race_flow's
// RallyChampionSequence, CShowUIScreen_PostRaceHorizonChampion with
// doneOnCutsceneFinish). CHorizonChampion (82677D18) sets TEXT_TITLE to
// CareerEventUI IDS_HorizonRallyChampion in Rally mode (IDS_HorizonChampion
// otherwise) and TEXT_POSITION from the profile (inferred: the player's
// name); it adds no help buttons. The engine keeps no gamertag, so the
// position tape is hidden (interim, Q251). The scene rests on screen (no
// Tape2, no intro offset); its 1ST_IN / 1ST_OUT slides are not run.
bool draw_rally_champion_scene(ui::List& l, float w, float h) {
  SceneScreen sc(kChampionScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  for (const char* n : {"ButtonBar", "Timer", "OnlineText", "OfflineText", "TEXT_POSITION", "Tape"})
    sc.hide(sc.node(n));
  const int title = sc.node("TEXT_TITLE");
  const std::string t = upper(text_of(race_text::kRallyChampion));
  sc.text(title, t);
  if (title >= 0) fit_tapes(sc, sc.node("ScreenTitle"), sd.measure(title, t) + kTitleTapePad, kTapeBlack);
  sc.draw(l, SceneScreen::frame(w, h));
  return true;
}

}  // namespace

std::string rally_points_to_final(const RallyPostHud& r) {
  if (r.champion || r.final_req <= 0) return {};
  const int left = r.final_req - std::max(0, r.xp_after);
  if (left <= 0) return text_of(race_text::kWinToBeChampion);
  return format1(text_of(left == 1 ? race_text::kPointToRallyFinal : race_text::kPointsToRallyFinal),
                 std::to_string(left));
}

bool draw_rally_finished(ui::List& l, int place, float w, float h) {
  RaceHud r;
  r.place = place;
  return draw_finished(l, r, w, h, 1.0f, true);
}

bool draw_rally_earnings(ui::List& l, const RallyEarningsHud& e, float w, float h) {
  RaceHud r;
  r.place = e.place;
  r.base_credits = e.base;
  r.difficulty_pct = e.difficulty_pct;
  r.credits = e.total;
  r.balance = e.balance;
  r.screen_t = e.t;
  if (!draw_cash(l, r, w, h)) return false;
  if (e.t >= race_flow::timing::kBannerOut) ui4::button_bar(l, {{"A", text_of(race_text::kContinue)}}, w, h);
  return true;
}

bool draw_rally_prize_car(ui::List& l, const std::string& car_name, float w, float h) {
  RaceHud r;
  r.won_car_name = car_name;
  return draw_won_car(l, r, w, h);
}

bool draw_rally_points(ui::List& l, const RallyPostHud& p, float w, float h) {
  RaceHud r;
  r.band_before = p.xp_before;
  r.band_after = p.xp_after;
  r.band_lo = p.band_lo;
  r.band_hi = p.band_hi;
  r.band_name = p.band;
  r.band_next = p.band_next;
  const std::string to_final = rally_points_to_final(p);
  return draw_points(l, r, w, h, kRallyPointsScene, to_final.empty() ? nullptr : &to_final);
}

bool draw_rally_champion(ui::List& l, float w, float h) { return draw_rally_champion_scene(l, w, h); }

std::vector<StringRef> rally_champion_text_refs() { return {race_text::kRallyChampion}; }

const std::vector<std::pair<std::string_view, std::vector<const char*>>>& race_screen_nodes() {
  static const std::vector<std::pair<std::string_view, std::vector<const char*>>> k = {
      {kEventSummaryScene,
       {"TitleTapeText", "Ticket/bg", "Background_-_Restrictions", "TEXT_POINTS_AVAILABLE", "TEXT_RACE_INFO",
        "TEXT_TRACK_INFO", "CarClass/ClassIcon", "TEXT_CAR_RESTRICTIONS", "TEXT_ELIGIBILITY", "TEXT_POINTS",
        "TEXT_CREDITS", "CarInfo", "TEXT_TOPCAR", "TEXT_BOTTOMCAR", "RestrictionTitle", "TEXT_RESTRICTION_TITLE",
        "TEXT_RESTRICTION_VALUE", "Wristband", "ExtraInfo", "History", "TEXT_BEST_FINISH", "GROUP_TEST_TEXT",
        "PlayerBar", "ButtonBar", "Next_Level", "TEXT_CAR", "TEXT_CR"}},
      {kRaceTitleScene,
       {"Winnings", "ButtonBar", "NemesisGroup", "FreeRoam", "Layer2", "TitleGroup", "MiniTitle", "MainTitle",
        "MiniTape", "TitleTapeText", "NemesisTape", "TEXT_BONUS_TITLE", "TEXT_BONUS", "NemesisName/Group"}},
      {kGridMenuScene,
       {"ButtonBar", "Menu", "Option_List", "MiniTitle", "MainTitle", "MiniTape", "TitleTapeText", "Button0",
        "Button1", "Button2", "Button3"}},
      {kFinishedScene,
       {"ButtonBar", "Timer", "OnlineText", "OfflineText", "Tape", "Tape2", "ScreenTitle", "TEXT_TITLE",
        "TEXT_POSITION"}},
      {kCashScene,
       {"ButtonBar", "RotationHelper/ScreenTitle", "RotationHelper/ScreenTitle/TEXT_TITLE", "Breakdown/Item1",
        "Breakdown/Item2", "Breakdown/Item3", "Breakdown/Item4", "Breakdown/Item5", "TitleTape/TEXT_LABEL",
        "ValueTape/TEXT_VALUE", "TotalAlign", "Total/Title", "Total/Value/Value", "Balance", "TEXT_BALANCE",
        "TEXT_BALANCE_TITLE"}},
      {kPointsScene,
       {"ButtonBar", "RotationHelper", "RotationHelper/Title", "MainTape", "MainTape/TEXT_TITLE", "TEXT_MAIN",
        "LargeTape", "CarName", "LevelBar", "TEXT_POINTS_TO_NEXT", "FillBar", "FillBar2", "WristbandCurrent/Rectangle",
        "NextReward/Wristband", "HeadlineEvent", "GlowMover"}},
      {kWonCarScene, {"ButtonBar", "ScreenTitle/TEXT_TITLE", "TEXT_POSITION", "Tape2", "CarName", "ScreenTitle"}},
      {kRallyPointsScene,
       {"ButtonBar", "RotationHelper", "RotationHelper/Title", "MainTape", "MainTape/TEXT_TITLE", "TEXT_MAIN",
        "LargeTape", "CarName", "LevelBar", "TEXT_POINTS_TO_NEXT", "FillBar", "FillBar2", "HeadlineEvent",
        "GlowMover"}},
      {kChampionScene,
       {"ButtonBar", "Timer", "OnlineText", "OfflineText", "Tape", "ScreenTitle", "TEXT_TITLE", "TEXT_POSITION"}},
      {kPopupScene, {"SpinnerBox", "Background", "Title", "Message"}},
      {kEncounterScene,
       {"Name", "TEXT_NAME", "Manufacturer", "TEXT_CARMAKE", "TEXT_CAR_NAME", "CLASS_SM", "TEXT_PI", "CLASS_A", "Reward",
        "TEXT_REWARD", "Credits", "TEXT_CREDITS"}},
  };
  return k;
}

StringRef grid_row_text(GridRow r) {
  switch (r) {
    case GridRow::kStartRace: return race_text::kStartRace;
    case GridRow::kDifficulty: return race_text::kDifficulty;
    case GridRow::kOptions: return race_text::kOptions;
    case GridRow::kQuitRace: return race_text::kQuitRace;
  }
  return {};
}

std::string event_ticket_texture(int style, std::string_view ticket) {
  const char* folder = nullptr;
  switch (style) {
    case 1: folder = "FESTIVALRACES"; break;
    case 2: folder = "STREETRACES"; break;
    case 3: folder = "SHOWCASES"; break;
    case 5: folder = "NEMESISTICKETS"; break;
    case 6: folder = "HEADLINERACES"; break;
    default: break;
  }
  if (!folder || ticket.empty()) return "EVENTTICKETS/FESTIVALRACETEMPLATE";
  return std::string("EVENTTICKETS/") + folder + "/" + upper(std::string(ticket));
}

std::string event_track_info(int surface, bool circuit) {
  // interim (SPEC_QUESTIONS Q93): Tracks.SurfaceTypeRibbonConfiguration 0 is
  // asphalt (FR06, The Gauntlet: ASPHALT POINT-TO-POINT in
  // 10_event_card_prerace_orig.png); 1 mixed and 2 off road are assumed.
  const StringRef& s = surface == 2 ? race_text::kOffRoad : surface == 1 ? race_text::kMixedSurface : race_text::kAsphalt;
  std::string out = text_of(race_text::kTwoWords);
  const std::string a = text_of(s), b = text_of(circuit ? race_text::kCircuit : race_text::kPointToPoint);
  if (const size_t p = out.find("{0}"); p != std::string::npos) out.replace(p, 3, a);
  if (const size_t p = out.find("{1}"); p != std::string::npos) out.replace(p, 3, b);
  return out;
}

std::string event_type_text(int style) {
  switch (style) {
    case 2: return text_of(race_text::kStreetRace);
    case 3: return text_of(race_text::kShowcaseRace);
    case 5: return text_of(race_text::kStarShowdown);
    case 6: return text_of(race_text::kHeadlineRace);
    default: return text_of(race_text::kFestivalRace);
  }
}

// The fallback when 108_C_BUY_CARSELECT is missing: the garage's eligible
// cars on the grid menu's scene under a CAR SELECT title.
bool draw_car_select(ui::List& l, const EventSummaryHud& es, float w, float h) {
  RaceHud r;
  r.event_type = text_of(race_text::kCarSelectTitle);
  r.title = es.name;
  r.screen_t = 1.0;
  const int n = int(es.cars.size());
  const int sel = std::clamp(es.car_selected, 0, std::max(0, n - 1));
  const int first = window_first(sel, n, 7);
  for (int i = first; i < std::min(n, first + 7); ++i) r.grid_menu.push_back(upper(es.cars[size_t(i)]));
  r.grid_selected = sel - first;
  return draw_grid_menu(l, r, w, h, true);
}

void draw_fade(ui::List& l, double fade, float w, float h) {
  if (fade <= 0) return;
  l.rect(0, 0, w, h, ui::rgba(0, 0, 0, uint8_t(std::lround(255.0 * std::clamp(fade, 0.0, 1.0)))));
}

// The ticket's value lines rest at their stackers' intro start; measured on
// 10_event_card_prerace_orig.png, the points tape sits 17 px lower with 110
// PTS centred on it (y 357), the credits tape 6 px higher with 6,000 Cr on it
// (y 392), and the eligibility line starts right after OPEN (x 441).
constexpr float kTicketPointsDy = 17.0f, kTicketPointsTextDy = 30.0f;
constexpr float kTicketCreditsDy = -6.0f, kTicketCreditsTextDy = 16.0f;
constexpr float kTicketEligibilityDx = -80.0f;
constexpr const char* kTicketOpenText = "OPEN";  // the restriction kTicketEligibilityDx was measured after
constexpr float kTicketTapePad = 8.0f;

// Widens a ticket value tape (LeftEdge, Centre, RightEdge) leftwards to its
// right-aligned text plus a pad (6,000 Cr runs past the scene's tape).
void widen_ticket_tape(SceneScreen& sc, int tape, int text, const std::string& s) {
  const SceneDraw& sd = sc.scene();
  const int le = child(sd, tape, "LeftEdge"), ce = child(sd, tape, "Centre");
  if (le < 0 || ce < 0 || text < 0) return;
  const float left = sd.at(text)[0] - sd.measure(text, s) - kTicketTapePad;
  const auto rl = sd.rect(le), rc = sd.rect(ce);
  if (left >= rl[0]) return;
  const float dx = left - rl[0];
  sc.at(le).dx += dx;
  const float from = rl[2] + dx, to = rc[2];
  sc.at(ce).stretch_x = (to - from) / std::max(1.0f, rc[2] - rc[0]);
  sc.at(ce).dx += 0.5f * (from + to) - 0.5f * (rc[0] + rc[2]);
}

bool draw_event_ticket(ui::List& l, const DriveHud& st, const EventSummaryHud& es, float w, float h, bool player_bar) {
  SceneScreen sc(kEventSummaryScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  for (const char* n : {"ButtonBar", "GROUP_TEST_TEXT"}) sc.hide(sc.node(n));
  sc.text(sc.node("TitleTapeText"), upper(es.type));
  if (const int bg = sc.node("Ticket/bg"); bg >= 0 && !es.ticket.empty()) sc.at(bg).texture = es.ticket;
  sc.text(sc.node("TEXT_RACE_INFO"), upper(es.track));
  sc.text(sc.node("TEXT_TRACK_INFO"), upper(es.track_info));
  sc.text(sc.node("TEXT_POINTS_AVAILABLE"), text_of(race_text::kPointsAvailable));
  sc.text(sc.node("TEXT_POINTS"), thousands(es.points) + " " + text_of(race_text::kPts));
  sc.text(sc.node("TEXT_CREDITS"), credits_text(es.credits));
  // The value tapes take the ticket's colour scheme (set at run time by the
  // ColorScheme states, not decoded); interim (SPEC_QUESTIONS Q94): the
  // festival green and dark text sampled on 10_event_card_prerace_orig.png
  // for festival tickets, the scene's own colours for the others.
  if (es.style == 1) {
    for (const char* tape : {"Points/Tape", "Credits/Tape"}) {
      const int tp = sc.node(tape);
      for (int c : sd.children_of(tp)) {
        if (sd.object_name(c).rfind("TEXT_", 0) == 0) sc.at(c).tint = kTicketInk;
        else sc.at(c).tint = kTicketGreen;
      }
    }
  }
  if (const int tp = sc.node("Points/Tape"); tp >= 0) sc.at(tp).dy = kTicketPointsDy;
  if (const int tx = sc.node("TEXT_POINTS"); tx >= 0) sc.at(tx).dy = kTicketPointsTextDy;
  if (const int tp = sc.node("Credits/Tape"); tp >= 0) sc.at(tp).dy = kTicketCreditsDy;
  if (const int tx = sc.node("TEXT_CREDITS"); tx >= 0) sc.at(tx).dy = kTicketCreditsTextDy;
  // The eligibility line follows the restriction: measured after OPEN, and
  // pushed right by a longer restriction (MUST BE ...) so they never overlap.
  if (const int el = sc.node("TEXT_ELIGIBILITY"); el >= 0) {
    float dx = kTicketEligibilityDx;
    if (const int cr = sc.node("TEXT_CAR_RESTRICTIONS"); cr >= 0 && !es.restriction.empty())
      dx += std::max(0.0f, sd.measure(cr, upper(es.restriction)) - sd.measure(cr, kTicketOpenText));
    sc.at(el).dx = dx;
  }
  widen_ticket_tape(sc, sc.node("Points/Tape"), sc.node("TEXT_POINTS"), thousands(es.points) + " " + text_of(race_text::kPts));
  widen_ticket_tape(sc, sc.node("Credits/Tape"), sc.node("TEXT_CREDITS"), credits_text(es.credits));
  if (es.style == 1)
    if (const int pa = sc.node("TEXT_POINTS_AVAILABLE"); pa >= 0) sc.at(pa).tint = kTicketGreen;
  if (const int icon = sc.node("CarClass/ClassIcon"); icon >= 0) {
    if (es.car_class.empty()) sc.hide(icon);
    else sc.at(icon).texture = "PICLASS/CLASS_" + upper(es.car_class) + "_SM";
  }
  sc.text(sc.node("TEXT_CAR_RESTRICTIONS"), upper(es.restriction));
  sc.text(sc.node("TEXT_ELIGIBILITY"), upper(es.eligibility));
  if (es.provided_car.empty()) {
    sc.hide(sc.node("CarInfo"));
  } else {
    sc.text(sc.node("TEXT_TOPCAR"), text_of(race_text::kCarProvided));
    sc.text(sc.node("TEXT_BOTTOMCAR"), es.provided_car);
    sc.hide(sc.node("BottomLine/ClassIcon"));
  }
  if (es.best_finish.empty()) sc.hide(sc.node("History"));
  else sc.text(sc.node("TEXT_BEST_FINISH"), format1(text_of(race_text::kBestFinish), es.best_finish));
  if (es.restrictions_view) {
    // Y: the restrictions side of the ticket.
    const int info = sc.node("Ticket/Info");
    for (int c : sd.children_of(info))
      if (sd.object_name(c) != "RestrictionTitle") sc.hide(c);
    const int rt = sc.node("RestrictionTitle");
    sc.show(rt);
    for (int c : sd.children_of(rt)) sc.show(c);
    sc.text(sc.node("TEXT_RESTRICTION_TITLE"), text_of(race_text::kEntryRestrictions));
    const int rv = sc.node("TEXT_RESTRICTION_VALUE");
    sc.text(rv, sc.wrap(rv, es.restriction_detail.empty() ? text_of(race_text::kNoRestrictions) : es.restriction_detail));
    sc.hide(sc.node("Ticket/bg"));
    if (const int rb = sc.node("Background_-_Restrictions"); rb >= 0) {
      sc.show(rb);
      sc.at(rb).texture = es.type == text_of(race_text::kStreetRace) ? "EVENTTICKETS/STREETRACEFLYERRESTRICTIONS"
                                                                     : "EVENTTICKETS/EVENTTICKETRESTRICTIONS";
    }
  }
  if (player_bar) bind_player_bar(sc, st);
  else sc.hide(sc.node("PlayerBar"));
  sc.draw(l, SceneScreen::frame(w, h));
  return true;
}

bool draw_event_summary(ui::List& l, const DriveHud& st, const EventSummaryHud& es, float w, float h) {
  if (es.screen == Screen::kCarSelect)
    return (!es.car_list.tiles.empty() && draw_car_list(l, st, es.car_list, w, h)) || draw_car_select(l, es, w, h);
  if (!draw_event_ticket(l, st, es, w, h, true)) return false;
  if (es.screen == Screen::kEntryFade) {
    draw_fade(l, es.fade, w, h);
    return true;
  }
  ui4::button_bar(l,
                  {{"A", text_of(race_text::kEnterEvent)},
                   {"B", text_of(race_text::kBack)},
                   {"Y", text_of(race_text::kRestrictions)}},
                  w, h);
  return true;
}

bool draw_race_screen(ui::List& l, const DriveHud&, const RaceHud& r, float w, float h) {
  switch (r.screen) {
    case Screen::kTitleCard: return draw_title_card(l, r, w, h);
    case Screen::kGridMenu: return draw_grid_menu(l, r, w, h);
    case Screen::kRivalIntro: return draw_rival_intro(l, r, w, h);
    case Screen::kEncounterIntro: return draw_encounter_intro(l, r, w, h);
    case Screen::kFinishBanner: return draw_finished(l, r, w, h, 1.0f);
    case Screen::kCashReward: return draw_cash(l, r, w, h);
    case Screen::kPoints: return draw_points(l, r, w, h);
    case Screen::kRewardCar: return draw_won_car(l, r, w, h);
    case Screen::kStayInCar: return draw_stay_in_car(l, r, w, h);
    case Screen::kBlackOut:
    case Screen::kDone:
      draw_black_out(l, r, w, h);
      return true;
    default: return false;
  }
}

}  // namespace carview
