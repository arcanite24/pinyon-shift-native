#include "pause_screen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include "game_hud.h"
#include "race_screens.h"
#include "scene_timeline.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

using Action = PauseMenu::Action;
using Screen = PauseMenu::Screen;

constexpr uint32_t kWhite = ui::rgba(255, 255, 255);
constexpr uint32_t kInk = ui::rgba(20, 20, 22);
// The rows' tapes: unselected black (the scene's Dirt_Tile material), the
// selected row the Horizon pink.
constexpr uint32_t kRowBlack = ui::rgba(12, 12, 12);
constexpr uint32_t kRowPink = ui4::kPink;
// Sponsor Challenges' progress fill (BARBASE yellow) and panes.
constexpr uint32_t kSponsorFill = ui::rgba(0xDA, 0xFF, 0x00);
constexpr uint32_t kSponsorPane = ui::rgba(24, 24, 24);
constexpr uint32_t kSponsorBarBack = ui::rgba(96, 96, 96);

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

uint32_t mix(uint32_t a, uint32_t b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  auto ch = [&](int sh) {
    const float x = float((a >> sh) & 0xFF), y = float((b >> sh) & 0xFF);
    return uint32_t(std::lround(x + (y - x) * t)) << sh;
  };
  return ch(0) | ch(8) | ch(16) | ch(24);
}

// The direct child of `parent` called `name` (a Button's own tapes, not its
// MessageIndicator's).
int child(const SceneDraw& sd, int parent, std::string_view name) {
  if (parent < 0) return -1;
  for (int c : sd.children_of(parent))
    if (sd.object_name(c) == name) return c;
  return -1;
}

// The SponsorBars rows are SponsorBar, SponsorBar2 .. SponsorBar6.
std::string sponsor_bar_name(int i) { return i == 0 ? "SponsorBar" : "SponsorBar" + std::to_string(i + 1); }

void help_bar(ui::List& l, std::vector<ui4::Prompt> prompts, float w, float h) { ui4::button_bar(l, prompts, w, h); }

// --- 925_PAUSE_MENU -------------------------------------------------------

// The original's list sits left of the scene's laid-out Option_List
// (measured on 07_pause_menu_orig.png: QUIT's text box at 201,452).
constexpr float kListDx = -45.0f, kListDy = -62.0f;
// The focused row's larger text sits a little higher than a scale about its
// anchor puts it (MAP's text from y 140 in 07_pause_menu_orig.png, SETTINGS's
// from 192 in 29_message_center_offline_orig.png; the other rows keep the
// scene's 50 px pitch).
constexpr float kFocusDy = -5.0f;
// With the rows' slides (MENU-17), the rows above the focus rest 7 px up
// and those below 6 px down, so the list's rest is 7 px higher than the
// measurement above, which had the spread folded in. Then the focused row
// needs no kFocusDy. This places SETTINGS at 193 (MAP focused, 07_) and 194
// (QUIT focused, 07b_) against the captures' 199 - 6 and 187 + 7.
constexpr float kListTimelineDy = -7.0f;
constexpr float kRowAboveDy = -7.0f, kRowBelowDy = 6.0f;  // BLURREDABOVE / BLURREDBELOW, for the hit boxes
// The player bar's packing (its stackers are not run), measured on the same
// capture: the objective ends at x 1030, left of the wristband; each pill
// starts a few pixels left of its right-aligned text.
constexpr float kObjectiveRight = 1030.0f;
constexpr float kPillPad = 10.0f;

// Moves a pill that runs off the right edge so it starts `kPillPad` left of
// the right-aligned `text`.
void fit_pill(SceneScreen& sc, int pill, int text, const std::string& s) {
  const SceneDraw& sd = sc.scene();
  if (pill < 0 || text < 0) return;
  const float left = sd.at(text)[0] - sd.measure(text, s) - kPillPad;
  sc.at(pill).dx = left - sd.rect(pill)[0];
}

}  // namespace

void bind_player_bar(SceneScreen& sc, const DriveHud& st, const std::string& band_texture) {
  const SceneDraw& sd = sc.scene();
  // Every node under the PlayerBar: a screen sharing the bar has its own
  // Points, Wristband, TEXT_PI, Pill01 ... (H_EVENT_SUMMARY, the car lists).
  const int bar = sc.node("PlayerBar");
  const int car = sc.node("MiddleRow/TEXT_CAR", bar);
  // Left-aligned PlayerBar (108_C_BUY_CARSELECT, 115_C_BUY_MFRSELECT,
  // 527_C_UPG_UPGRADES) sits top-left with left-aligned texts and pills that
  // crop on the right; right-aligned PlayerBar (925_PAUSE_MENU, H_MAP,
  // H_EVENT_SUMMARY) sits top-right with right-aligned texts.
  const bool left_bar = (car >= 0) && (sd.at(car)[0] < 640.0f);
  // Top row: the wristband objective and the wristband.
  const int next = sc.node("TopRow/SP/Next_Level", bar), points = sc.node("TopRow/SP/Points", bar);
  const int band = sc.node("TopRow/SP/Wristband", bar);
  const int top_pill = sc.node("TopRow/SP/Pill01", bar);
  // A band waiting to be collected shows that band, its objective in its
  // colour with no tape behind on the right-aligned pause bar, or on its dark
  // pill next to the wristband on the left-aligned car-select bar (outpost-a1.png).
  const bool collect = !st.band_next.empty() && st.band_collect_ready;
  const bool no_top = band_texture == kNoPlayerBarTopRow;
  if (no_top) {
    sc.hide(sc.node("TopRow", bar));
  } else if (!st.band_next.empty()) {
    const std::string colour = upper(st.band_next);
    if (collect) {
      const std::string objective = format1(text_of(pause_text::kCollectWristband), colour);
      sc.text(next, objective);
      if (next >= 0) {
        sc.at(next).tint = ui4::wristband_colour(st.band_next);
        if (!left_bar) {
          sc.at(next).dx = kObjectiveRight - sd.measure(next, objective) - sd.at(next)[0];
        } else if (top_pill >= 0) {
          const auto pr = sd.rect(top_pill);
          const float right = sd.at(next)[0] + sd.measure(next, objective) + kPillPad;
          sc.at(top_pill).visible_width = std::max(120.0f, right - pr[0]);
          sc.at(top_pill).tint = ui::rgba(14, 14, 16, 240);
        }
      }
      sc.hide(points);
      if (!left_bar) {
        const int sp = sc.node("TopRow/SP", bar);
        for (const char* tape : {"Smudge_Tape", "Crisp_Tape", "Dirt_Tile", "top_pill"}) sc.hide(child(sd, sp, tape));
      }
    } else {
      sc.text(next, "Nxt Lvl");
      const std::string pts_str =
          st.band_hi > 0 ? (thousands(std::max(0, st.band_hi - st.band_points)) + " PTS") : std::string{};
      if (!pts_str.empty()) sc.text(points, pts_str);
      if (left_bar && top_pill >= 0) {
        const auto pr = sd.rect(top_pill);
        float right = pr[0] + 180.0f;
        if (next >= 0) right = std::max(right, sd.at(next)[0] + sd.measure(next, "Nxt Lvl") + kPillPad);
        if (points >= 0 && !pts_str.empty())
          right = std::max(right, sd.at(points)[0] + sd.measure(points, pts_str) + kPillPad);
        sc.at(top_pill).visible_width = std::max(140.0f, right - pr[0]);
        sc.at(top_pill).tint = ui::rgba(14, 14, 16, 240);
      }
    }
  } else {
    sc.hide(next);
    sc.hide(points);
    if (left_bar && top_pill >= 0) sc.hide(top_pill);
  }
  const std::string shown_band = collect ? st.band_next : st.band_name;
  const std::string band_tex = !band_texture.empty() ? band_texture
                               : shown_band.empty()      ? std::string()
                                                         : "WRISTBANDS/" + upper(shown_band.substr(0, shown_band.find(' ')));
  if (band >= 0 && !band_tex.empty() && !no_top) sc.at(band).texture = band_tex;
  // Middle row: the car and its class / PI.
  const int pi = sc.node("MiddleRow/CLASS_SM/Group/TEXT_PI", bar);
  const int cls = sc.node("MiddleRow/CLASS_SM/Group/CLASS_A", bar);
  std::string name = st.club_tag.empty() ? st.car_name : "[" + st.club_tag + "] " + st.car_name;
  sc.text(car, name);
  if (left_bar) {
    if (const int mp = sc.node("MiddleRow/pill_middle", bar); mp >= 0) {
      const auto pr = sd.rect(mp);
      const float right = sd.at(car)[0] + sd.measure(car, name) + kPillPad;
      sc.at(mp).visible_width = std::max(120.0f, right - pr[0]);
      sc.at(mp).tint = ui::rgba(14, 14, 16, 244);
    }
  } else {
    fit_pill(sc, sc.node("middle_Pill", bar), car, name);
  }
  if (!st.car_class.empty()) {
    sc.text(pi, std::to_string(st.car_pi));
    if (pi >= 0) sc.at(pi).tint = kInk;
    if (cls >= 0) sc.at(cls).texture = "PICLASS/CLASS_" + upper(st.car_class);
  } else {
    sc.hide(sc.node("MiddleRow/CLASS_SM", bar));
  }
  // Bottom row: credits.
  if (st.credits >= 0) {
    const int cr = sc.node("BottomRow/TEXT_CR", bar);
    const int cr_pill = sc.node("BottomRow/Pill01", bar);
    const std::string credits = format1(text_of(pause_text::kCredits), thousands(st.credits));
    sc.text(cr, credits);
    if (left_bar) {
      if (cr_pill >= 0 && cr >= 0) {
        const auto pr = sd.rect(cr_pill);
        const float right = sd.at(cr)[0] + sd.measure(cr, credits) + kPillPad + 4.0f;
        sc.at(cr_pill).visible_width = std::max(96.0f, right - pr[0]);
        sc.at(cr_pill).tint = ui::rgba(236, 236, 236, 248);
      }
    } else {
      fit_pill(sc, cr_pill, cr, credits);
    }
    if (cr >= 0) sc.at(cr).tint = kInk;
  } else {
    sc.hide(sc.node("BottomRow", bar));
  }
}

namespace {

// The rows' own slides (MENU-17, docs/UI_SCENE_ANIMATION.md). The focused
// row is on FOCUSED: scale 1.2, its tapes pink (250, 0, 100), its clip-on
// in. The rows above it rest 7 px up (BLURREDABOVE) and the rows below 6 px
// down (BLURREDBELOW): 07_pause_menu_orig.png (MAP focused) and
// 07b_pause_menu_scrolled_orig.png (QUIT focused) put SETTINGS..MY PROFILE
// 12-13 px apart. While the focus moves, the new row plays FOCUSABOVE or
// FOCUSBELOW and the old one BLURBELOW or BLURABOVE (150 ms), scrubbed by
// the measured cross-fade (`fade`, over kPauseFadeSeconds). False without
// the scene's timeline; the measured looks are drawn then.
bool pause_rows_anim(SceneScreen& sc, const PauseScreenHud& ps, int first, int slots) {
  static SceneTimeline tl;
  tl.bind(sc.scene().scene());
  if (!tl.ok() || !scene_timelines_enabled()) return false;
  const int n = int(ps.rows.size());
  const bool moving = ps.from >= 0 && ps.from != ps.selected && ps.fade < 1.0;
  for (int slot = 0; slot < slots; ++slot) {
    const int btn = sc.node("Button" + std::to_string(slot));
    const int i = first + slot;
    const int comp = tl.item_of_node(btn);
    if (btn < 0 || comp < 0 || i >= n) continue;
    const char* want = i < ps.selected ? "BLURREDABOVE" : i > ps.selected ? "BLURREDBELOW" : "FOCUSED";
    bool scrub = false;
    if (moving && i == ps.selected) want = ps.selected > ps.from ? "FOCUSBELOW" : "FOCUSABOVE", scrub = true;
    if (moving && i == ps.from) want = ps.from > ps.selected ? "BLURBELOW" : "BLURABOVE", scrub = true;
    if (tl.slide(uint32_t(comp)) != want) tl.go_to(uint32_t(comp), want);
    // The move's slides run 150 ms (their curves' last key).
    tl.set_time(uint32_t(comp), scrub ? float(ps.fade) * 150.0f : 0.0f);
  }
  // Only the rows' looks: the list's own slide (Option_List's Slide1, which
  // the game scrubs with its SIZE events) is not sent, so kListDx / kListDy
  // still place the list (Q187).
  SceneBindings all;
  tl.apply(all);
  std::vector<int> stack;
  for (int slot = 0; slot < slots; ++slot) stack.push_back(sc.node("Button" + std::to_string(slot)));
  while (!stack.empty()) {
    const int nd = stack.back();
    stack.pop_back();
    if (nd < 0) continue;
    if (auto it = all.anim.find(nd); it != all.anim.end()) sc.bindings().anim[nd] = it->second;
    for (int c : sc.scene().children_of(nd)) stack.push_back(c);
  }
  return true;
}

void draw_list(ui::List& l, const DriveHud& st, const PauseScreenHud& ps, float w, float h, bool help = true) {
  SceneScreen sc(kPauseScene);
  const SceneDraw& sd = sc.scene();
  const SceneFrame f = SceneScreen::frame(w, h);
  // The scene's own help bar is BUTTON_BAR_SCENE's (drawn below).
  sc.hide(sc.node("Layer2"));
  // The original draws the rows and the player bar at full size past their
  // text boxes (SPONSOR CHALLENGES, COLLECT .. WRISTBAND).
  sc.bindings().fit_text = false;
  const int n = int(ps.rows.size());
  // The scene's rows: the title update's scene has nine (Button0..8), for the
  // packages' 1000 CLUB and HORIZON RALLY (club_pause.png); the disc's has
  // seven, so the nine EXTRAS rows scroll there. The rest stay hidden.
  int slots = 0;
  while (slots < 9 && sc.node("Button" + std::to_string(slots)) >= 0) ++slots;
  const int first = window_first(ps.selected, n, slots);
  const bool timeline = pause_rows_anim(sc, ps, first, slots);
  // The list group, placed where the original draws it.
  const float list_dy = kListDy + (timeline ? kListTimelineDy : 0.0f);
  const int list = sc.node("Option_List/Option_List");
  if (list >= 0) sc.at(list).dx = kListDx, sc.at(list).dy = list_dy;
  for (int slot = 0; slot < slots; ++slot) {
    const int btn = sc.node("Button" + std::to_string(slot));
    const int i = first + slot;
    if (btn < 0) continue;
    if (i >= n) {
      sc.hide(btn);
      continue;
    }
    const int body = child(sd, btn, "Button");
    const int text = child(sd, body, "Text");
    sc.text(text, ps.rows[size_t(i)]);
    float sel = 0.0f;
    if (i == ps.selected) sel = float(ps.fade);
    else if (i == ps.from) sel = 1.0f - float(ps.fade);
    const uint32_t tape = mix(kRowBlack, kRowPink, sel);
    // The tapes end a little past the text (their crop follows its width).
    const float tw = sd.measure(text, ps.rows[size_t(i)]);
    for (const char* name : {"Dirt_Tile", "Crisp_Tape", "Smudge_Tape"}) {
      const int t = child(sd, body, name);
      if (t < 0) continue;
      if (!timeline) sc.at(t).tint = tape;
      if (std::string_view(name) != "Dirt_Tile") sc.at(t).visible_width = tw + 64.0f;
    }
    if (const int clip = child(sd, body, "Clipon"); !timeline && clip >= 0 && sel > 0.0f) sc.show(clip, sel);
    float row_dy = 0.0f;
    if (!timeline) {
      row_dy = i == ps.selected ? kFocusDy : 0.0f;
      if (row_dy != 0.0f) sc.at(btn).dy = row_dy;
      if (i == ps.selected) sc.at(btn).scale = kPauseFocusScale;
    } else {
      // The slides move the rows; the hit boxes follow their rests.
      row_dy = i < ps.selected ? kRowAboveDy : i > ps.selected ? kRowBelowDy : 0.0f;
    }
    const float k = i == ps.selected ? (timeline ? 1.2f : kPauseFocusScale) : 1.0f;
    // Mouse: the row's tapes.
    const auto c = sd.at(text);
    const float x0 = c[0] + kListDx - 30.0f, x1 = c[0] + kListDx + k * tw + 30.0f;
    const float y0 = c[1] + list_dy + row_dy - 24.0f * k, y1 = c[1] + list_dy + row_dy + 24.0f * k;
    l.add_hit(f.x(x0), f.y(y0), f.x(x1), f.y(y1), ui::kHitRow, i);
  }
  if (ps.screen == Screen::kExtras) {
    const int title = sc.node("TEXT_SCREEN_TITLE");
    sc.text(title, text_of(pause_text::kExtras));
    sc.show(title);
  }
  bind_player_bar(sc, st, ps.band_texture);
  sc.draw(l, f);

  if (!help) {
  } else if (ps.screen == Screen::kExtras) {
    help_bar(l, {{"A", text_of(pause_text::kSelect)}, {"B", text_of(pause_text::kBack)}}, w, h);
  } else {
    help_bar(l,
             {{"A", text_of(pause_text::kSelect)},
              {"B", text_of(pause_text::kResume)},
              {"X", text_of(pause_text::kExtras)}},
             w, h);
  }
}

// --- H_SPONSOR_CHALLENGE_STATUS -------------------------------------------

// INTERIM: the scene's BigPane01 panes do not draw where the original does
// (their crop is not decoded), so they are hidden and these panes, measured on
// 27_sponsor_challenges_orig.png about each SponsorBar node, are drawn instead.
struct Pane {
  float x0, y0, x1, y1;
};
constexpr Pane kSponsorBackPane{-486.0f, -30.0f, -103.0f, 38.0f};
constexpr Pane kProgressBackPane{-98.0f, -30.0f, 237.0f, 38.0f};
// About TEXT_POSITION (right-aligned): the yellow chart-position pill, and
// about TEXT_POSITION_TITLE the dark band behind the title.
constexpr Pane kPositionPill{-68.0f, -8.0f, 46.0f, 19.0f};
constexpr Pane kPositionTitleBand{-300.0f, -10.0f, 10.0f, 20.0f};
constexpr uint32_t kPositionYellow = ui::rgba(0xDA, 0xFF, 0x00);
// About the two REWARD texts: the dark title pane and the light value pane.
constexpr Pane kRewardTitlePane{-74.0f, -22.0f, 76.0f, 12.0f};
constexpr Pane kRewardPane{-71.0f, -15.0f, 75.0f, 19.0f};
constexpr uint32_t kRewardWhite = ui::rgba(232, 232, 232);
// The details text's line width (the scene's box is narrower than the
// original's lines).
constexpr float kDetailsWidth = 280.0f;
// The original's details text is a size step larger and starts lower.
constexpr float kDetailsScale = 1.1f, kDetailsDy = 14.0f;
// The popularity chart's length: 166th toward 150th fills 84% of the bar
// (27_sponsor_challenges_orig.png), i.e. measured from 250th.
constexpr int kPopularityChartSize = 250;

void pane(ui::List& l, const SceneFrame& f, const SceneDraw& sd, int node, const Pane& p, uint32_t colour) {
  if (node < 0) return;
  const auto c = sd.at(node);
  const float angle = sd.angle(node);
  if (angle != 0.0f) l.turn(f.x(c[0]), f.y(c[1]), -angle);
  l.rect(f.x(c[0] + p.x0), f.y(c[1] + p.y0), f.x(c[0] + p.x1), f.y(c[1] + p.y1), colour);
  if (angle != 0.0f) l.unturn();
}

void draw_sponsors(ui::List& l, const DriveHud& st, const PauseScreenHud& ps, float w, float h) {
  SceneScreen sc(kSponsorScene);
  if (!sc.ok()) return;
  const SceneDraw& sd = sc.scene();
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.hide(sc.node("ButtonBar"));
  sc.hide(sc.node("NAV_POSITION_INDICATOR"));
  sc.bindings().fit_text = false;
  for (int m : sd.find_all("BigPane01", sc.node("SponsorBars"))) sc.hide(m);
  for (int m : sd.find_all("BigPane01", sc.node("TitleBar"))) sc.hide(m);
  pane(l, f, sd, sc.node("TEXT_POSITION_TITLE"), kPositionTitleBand, kSponsorPane);
  pane(l, f, sd, sc.node("TEXT_POSITION"), kPositionPill, kPositionYellow);

  static const std::vector<SponsorChallengeRow> kNone;
  const auto& rows = ps.sponsors ? *ps.sponsors : kNone;
  const int n = int(rows.size());
  const int sel = n > 0 ? std::clamp(ps.sponsor_selected, 0, n - 1) : -1;

  sc.text(sc.node("TitleTapeText"), text_of(pause_text::kSponsorTitle));
  sc.text(sc.node("TEXT_POSITION_TITLE"), text_of(pause_text::kPopularityChart));
  const int rank = std::max(1, st.popularity_rank);
  sc.text(sc.node("TEXT_POSITION"), std::to_string(rank));
  sc.text(sc.node("TEXT_TH"), place_suffix(rank));
  if (const int t = sc.node("TEXT_POSITION"); t >= 0) sc.at(t).tint = kInk;
  if (const int t = sc.node("TEXT_TH"); t >= 0) sc.at(t).tint = kInk;
  sc.text(sc.node("TEXT_DETAILS_TITLE"), text_of(pause_text::kDetails));
  const int reward_title = sc.node("RewardTitle"), reward = sc.node("Reward");
  for (int box : {reward_title, reward})
    for (int m : sd.find_all("BigPane01", box)) sc.hide(m);
  const int reward_title_text = sc.node("TEXT_CAR_NAME", reward_title);
  const int reward_text = sc.node("TEXT_CAR_NAME", reward);
  sc.text(reward_title_text, text_of(pause_text::kReward));
  // The details panel goes under the reward panes: drawn first, then hidden.
  const int panel = sc.node("HUDPanel01");
  sc.draw(l, f, panel);
  sc.hide(panel);
  pane(l, f, sd, reward_title_text, kRewardTitlePane, kSponsorPane);
  if (sel >= 0) {
    const auto& r = rows[size_t(sel)];
    const int challenge = sc.node("TEXT_CHALLENGE");
    sc.text(challenge, sc.wrap(challenge, r.completed ? text_of(pause_text::kChallengeCompleted) : r.description,
                               kDetailsWidth / kDetailsScale));
    if (challenge >= 0) sc.at(challenge).scale = kDetailsScale, sc.at(challenge).dy = kDetailsDy;
    if (!r.completed) {
      sc.text(reward_text, format1(text_of(pause_text::kCredits), thousands(r.reward_cr)));
      if (reward_text >= 0) sc.at(reward_text).tint = kInk;
      pane(l, f, sd, reward_text, kRewardPane, kRewardWhite);
    }
  }

  constexpr int kSlots = 6;
  const int first = window_first(sel, n, kSlots);
  struct Fill {
    int node;
    float frac;
    float alpha;
  };
  std::vector<Fill> fills;
  for (int slot = 0; slot < kSlots; ++slot) {
    const int bar = sc.node(sponsor_bar_name(slot));
    const int i = first + slot;
    if (bar < 0) continue;
    if (i >= n) {
      sc.hide(bar);
      continue;
    }
    const auto& r = rows[size_t(i)];
    const bool on = i == sel;
    const int detail = child(sd, bar, "SponsorDetail");
    sc.show(detail);
    sc.text(child(sd, detail, "TEXT_SPONSOR"), r.sponsor_name);
    sc.text(child(sd, detail, "TEXT_TITLE"), r.challenge_title);
    const int rk = child(sd, bar, "Rank");
    sc.show(rk);
    sc.text(child(sd, rk, "TEXT_TITLE"), text_of(pause_text::kRank) + " " + std::to_string(r.tier));
    const int pb = child(sd, bar, "ProgressBar");
    if (!r.fame && !r.completed) {
      const int pt = child(sd, pb, "TEXT_PROGRESS");
      sc.show(pt);
      const std::string progress = std::to_string(r.current) + "/" + std::to_string(r.target);
      sc.text(pt, progress);
      // The (right-aligned) text over the bar's end, on the RANK line.
      if (const int mid = child(sd, pb, "middle"); mid >= 0 && pt >= 0) {
        sc.at(pt).dx = sd.rect(mid)[2] - sd.at(pt)[0];
        if (const int rt = child(sd, rk, "TEXT_TITLE"); rt >= 0) sc.at(pt).dy = sd.at(rt)[1] - sd.at(pt)[1];
      }
    }
    if (const int logo = child(sd, bar, "SponsorLogo"); logo >= 0 && !r.logo_texture.empty())
      sc.at(logo).texture = r.logo_texture;
    pane(l, f, sd, bar, kSponsorBackPane, on ? kRowPink : kSponsorPane);
    pane(l, f, sd, bar, kProgressBackPane, on ? kRowPink : kSponsorPane);
    // The bar's back is the full-width `middle`; its fill (the scaled Group)
    // is drawn after the scene at the progress fraction.
    const int back = child(sd, pb, "middle");
    if (back >= 0) sc.at(back).tint = kSponsorBarBack;
    sc.hide(child(sd, pb, "Group"));
    float frac = 0.0f;
    if (r.completed) {
      frac = 1.0f;
    } else if (r.fame) {
      const float span = float(std::max(1, kPopularityChartSize - r.target));
      frac = float(kPopularityChartSize - r.current) / span;
    } else if (r.target > 0) {
      frac = float(r.current) / float(r.target);
    }
    fills.push_back({back, std::clamp(frac, 0.0f, 1.0f), 1.0f});
  }
  sc.draw(l, f);
  for (const auto& fl : fills) {
    if (fl.node < 0 || fl.frac <= 0.0f) continue;
    const auto r = sd.rect(fl.node);
    const float wdt = r[2] - r[0];
    sd.model(l, f, fl.node, fl.alpha, -0.5f * wdt * (1.0f - fl.frac), 0.0f, 0.0f, kSponsorFill, {}, 0.0f, fl.frac, 1.0f);
  }
  help_bar(l, {{"B", text_of(pause_text::kBack)}}, w, h);
}

// Offline MESSAGE CENTER shows the retail NO NETWORK ACCESS H_MESSAGE_POPUP
// over the pause menu rows (29_message_center_offline_orig.png).

}  // namespace

StringRef pause_row_text(PauseMenu::Action a, bool in_race) {
  switch (a) {
    case Action::kMap: return {"PauseMenu", "IDS_Map", 0, "MAP"};
    // The retail second row is MULTIPLAYER; the engine has none and puts its
    // settings there (as the recomp's settings option does).
    case Action::kSettings: return {"PauseMenu", {}, 0xDD6B, "SETTINGS", true};
    case Action::kPhotoMode: return {"PauseMenu", "IDS_Photo", 0, "PHOTO MODE"};
    case Action::kMessageCentre: return {"PauseMenu", "IDS_MessageCentre", 0, "MESSAGE CENTER"};
    case Action::kSponsorChallenges: return {"PauseMenu", "IDS_SponsorChallenge", 0, "SPONSOR CHALLENGES"};
    case Action::kCareer: return {"PauseMenu", "IDS_MyProfile", 0, "MY PROFILE"};
    case Action::kQuitToTitle: return {"PauseMenu", "IDS_Quit", 0, "QUIT"};
    case Action::kRestartRace: return {"PauseMenu", "IDS_Restart", 0, "RESTART"};
    case Action::kQuitRace: return {"PauseMenu", "IDS_QuitRace", 0, "QUIT RACE"};
    case Action::kResume: return pause_text::kResume;
    // The extras: the engine's own screens.
    case Action::kEvents: return {{}, {}, 0, "EVENTS", true};
    case Action::kGarage: return {{}, {}, 0, "GARAGE", true};
    case Action::kUpgrades: return {{}, {}, 0, "UPGRADES", true};
    case Action::kPaint: return {{}, {}, 0, "PAINT", true};
    case Action::kAutoshow: return {{}, {}, 0, "AUTOSHOW", true};
    // The title update's packages' rows (their own tables).
    case Action::kRally: return {"Main", "IDS_HorizonRally", 0, "HORIZON RALLY"};
    case Action::kClub: return {"CarChallenges", "IDS_PauseMenu_CarChallenges", 0, "1000 CLUB"};
    case Action::kCarClub: return {{}, {}, 0, "CAR CLUB", true};
    case Action::kAchievements: return {{}, {}, 0, "ACHIEVEMENTS", true};
    case Action::kCareerPage: return {{}, {}, 0, "CAREER", true};
    case Action::kReplay: return {{}, {}, 0, in_race ? "VIEW REPLAY" : "REPLAYS", true};
    case Action::kReset: return {{}, {}, 0, "RESET TO ROAD", true};
    case Action::kDifficulty: return {{}, {}, 0, "DIFFICULTY", true};
    case Action::kQuit: return {{}, {}, 0, "QUIT", true};
  }
  return {};
}

std::string PauseMenu::label(Action a) const {
  // The extras whose text changes with the game's state.
  if (a == kEvents && !events_label.empty()) return events_label;
  if (a == kRally && in_rally_world) return "RALLY HUB";
  if (a == kDifficulty && !difficulty_label.empty()) return difficulty_label;
  return text_of(pause_row_text(a, in_race));
}

void PauseMenu::fill(HudState& st) const {
  if (!open) return;
  st.menu_title = "paused";
  st.menu.clear();
  for (Action a : rows()) st.menu.push_back(label(a));
  st.menu_selected = std::clamp(selected, 0, std::max(0, int(st.menu.size()) - 1));
}

void fill_pause_screen(const PauseMenu& menu, double now, PauseScreenHud& out) {
  out.screen = menu.screen;
  out.in_race = menu.in_race;
  out.rows.clear();
  for (Action a : menu.rows()) out.rows.push_back(menu.label(a));
  out.selected = std::clamp(menu.selected, 0, std::max(0, int(out.rows.size()) - 1));
  out.from = menu.shown_from;
  out.fade = kPauseFadeSeconds > 0 ? std::clamp((now - menu.shown_at) / kPauseFadeSeconds, 0.0, 1.0) : 1.0;
  if (out.fade >= 1.0) out.from = -1;
  out.sponsor_selected = menu.sponsor_selected;
  out.confirm_race = menu.confirm_action == Action::kQuitRace;
}

bool pause_popup_text(const PauseScreenHud& ps, std::string& title, std::string& message,
                      std::vector<std::pair<std::string, std::string>>& prompts) {
  if (ps.screen == Screen::kOffline || ps.screen == Screen::kMessageCentre) {
    title = text_of(pause_text::kNoNetworkTitle);
    message = text_of(pause_text::kNoNetworkBody);
    // interim (SPEC_QUESTIONS Q167): the popup's help bar is not captured.
    prompts = {{"A", text_of(pause_text::kOk)}};
    return true;
  }
  if (ps.screen == Screen::kConfirm) {
    if (ps.confirm_race) {
      title = text_of(pause_text::kQuitRaceTitle);
      message = text_of(pause_text::kQuitRaceDesc);
      // interim (SPEC_QUESTIONS Q167): QUIT RACE's popup is not captured;
      // its prompts follow QUIT's.
      prompts = {{"A", text_of(pause_text::kQuitButton)}, {"B", text_of(pause_text::kCancel)}};
    } else {
      title = text_of(pause_text::kQuitTitle);
      message = text_of(pause_text::kQuitDesc);
      prompts = {{"A", text_of(pause_text::kExitSinglePlayer)}, {"B", text_of(pause_text::kCancel)}};
    }
    return true;
  }
  return false;
}

void bind_pause_player_bar(SceneScreen& sc, const DriveHud& st, const std::string& band_texture) {
  bind_player_bar(sc, st, band_texture);
}

bool draw_pause_screen(ui::List& l, const DriveHud& st, const PauseScreenHud& ps, float w, float h) {
  switch (ps.screen) {
    case Screen::kSponsorChallenges:
      if (!SceneScreen(kSponsorScene).ok()) return false;
      draw_sponsors(l, st, ps, w, h);
      return true;
    case Screen::kList:
    case Screen::kExtras:
      if (!SceneScreen(kPauseScene).ok()) return false;
      draw_list(l, st, ps, w, h);
      return true;
    case Screen::kMessageCentre:
    case Screen::kOffline:
    case Screen::kConfirm: {
      // The rows stay under the popup (blurred with the frame in the
      // original; the engine blurs only the 3D frame behind the HUD).
      if (!SceneScreen(kPauseScene).ok()) return false;
      draw_list(l, st, ps, w, h, false);
      std::string title, message;
      std::vector<std::pair<std::string, std::string>> prompts;
      pause_popup_text(ps, title, message, prompts);
      std::vector<ui4::Prompt> pp;
      for (const auto& [b, s] : prompts) pp.push_back({b.c_str(), s});
      return draw_message_popup(l, title, message, pp, w, h);
    }
  }
  return false;
}

std::vector<SponsorChallengeRow> sponsor_challenge_rows(const freeroam::SkillsData& data,
                                                        const std::map<std::string, freeroam::SponsorProgress>* progress,
                                                        int popularity_rank, int fame_claimed) {
  std::vector<SponsorChallengeRow> out;
  auto table = [](const std::string& id, const std::string& fallback) {
    if (id.empty()) return fallback;
    return text_of({"SponsorshipChallenges", id, 0, fallback});
  };
  for (const auto& sc : data.sponsor_challenges) {
    SponsorChallengeRow r;
    r.sponsor_name = table(sc.sponsor_name, sc.id);
    r.challenge_title = table(sc.challenge_name, sc.id);
    r.logo_key = r.sponsor_name;
    if (!sc.logo.empty()) {
      std::string p = sc.logo;
      for (char& c : p)
        if (c == '\\') c = '/';
      const size_t slash = p.rfind('/');
      std::string file = p.substr(slash == std::string::npos ? 0 : slash + 1);
      if (const size_t dot = file.rfind('.'); dot != std::string::npos) file.resize(dot);
      r.logo_texture = "SPONSORLOGOTILE/" + upper(file);
    }
    r.max_tiers = std::max(1, int(sc.tiers.size()));
    r.fame = sc.type == "FameRankChallenge";
    int claimed = 0;
    if (r.fame) {
      claimed = fame_claimed;
      r.current = popularity_rank;
    } else if (progress) {
      if (auto it = progress->find(sc.id); it != progress->end()) {
        r.current = it->second.count;
        claimed = it->second.claimed_tiers;
      }
    }
    r.completed = claimed >= r.max_tiers;
    r.tier = std::min(r.max_tiers, claimed + 1);
    const size_t ti = size_t(r.tier - 1);
    if (ti < sc.tiers.size()) {
      r.target = std::max(1, sc.tiers[ti].number);
      r.reward_cr = sc.tiers[ti].credits;
    }
    if (r.fame) r.previous_target = ti > 0 && ti - 1 < sc.tiers.size() ? sc.tiers[ti - 1].number : 250;
    std::string desc = table(sc.description, "");
    if (!desc.empty()) {
      const std::string arg = r.fame ? std::to_string(r.target) + place_suffix(r.target) : std::to_string(r.target);
      r.description = format1(desc, arg);
    }
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace carview
