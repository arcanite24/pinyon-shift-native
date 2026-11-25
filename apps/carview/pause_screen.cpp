#include "pause_screen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include "game_hud.h"
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
  // Top row: the wristband objective and the wristband.
  // By path: a screen sharing the bar (H_EVENT_SUMMARY) has its own Points
  // and Wristband nodes.
  const int next = sc.node("TopRow/SP/Next_Level"), points = sc.node("TopRow/SP/Points");
  const int band = sc.node("TopRow/SP/Wristband");
  // A band waiting to be collected shows that band, its objective in its
  // colour with no tape behind (07_pause_menu_orig.png).
  const bool collect = !st.band_next.empty() && st.band_collect_ready;
  if (!st.band_next.empty()) {
    const std::string colour = upper(st.band_next);
    if (collect) {
      const std::string objective = format1(text_of(pause_text::kCollectWristband), colour);
      sc.text(next, objective);
      if (next >= 0) {
        sc.at(next).tint = ui4::wristband_colour(st.band_next);
        sc.at(next).dx = kObjectiveRight - sd.measure(next, objective) - sd.at(next)[0];
      }
      sc.hide(points);
      const int sp = sc.node("TopRow/SP");
      for (const char* tape : {"Smudge_Tape", "Crisp_Tape", "Dirt_Tile", "top_pill"}) sc.hide(child(sd, sp, tape));
    } else {
      sc.text(next, "Nxt Lvl");
      if (st.band_hi > 0) sc.text(points, thousands(std::max(0, st.band_hi - st.band_points)) + " PTS");
    }
  } else {
    sc.hide(next);
    sc.hide(points);
  }
  const std::string shown_band = collect ? st.band_next : st.band_name;
  const std::string band_tex = !band_texture.empty() ? band_texture
                               : shown_band.empty()      ? std::string()
                                                         : "WRISTBANDS/" + upper(shown_band.substr(0, shown_band.find(' ')));
  if (band >= 0 && !band_tex.empty()) sc.at(band).texture = band_tex;
  // Middle row: the car and its class / PI.
  const int car = sc.node("TEXT_CAR"), pi = sc.node("TEXT_PI"), cls = sc.node("CLASS_A");
  std::string name = st.club_tag.empty() ? st.car_name : "[" + st.club_tag + "] " + st.car_name;
  sc.text(car, name);
  fit_pill(sc, sc.node("middle_Pill"), car, name);
  if (!st.car_class.empty()) {
    sc.text(pi, std::to_string(st.car_pi));
    if (pi >= 0) sc.at(pi).tint = kInk;
    if (cls >= 0) sc.at(cls).texture = "PICLASS/CLASS_" + upper(st.car_class);
  } else {
    sc.hide(sc.node("CLASS_SM"));
  }
  // Bottom row: credits.
  if (st.credits >= 0) {
    const int cr = sc.node("TEXT_CR");
    const std::string credits = format1(text_of(pause_text::kCredits), thousands(st.credits));
    sc.text(cr, credits);
    fit_pill(sc, sc.node("Pill01"), cr, credits);
    if (cr >= 0) sc.at(cr).tint = kInk;
  } else {
    sc.hide(sc.node("BottomRow"));
  }
}

namespace {

void draw_list(ui::List& l, const DriveHud& st, const PauseScreenHud& ps, float w, float h) {
  SceneScreen sc(kPauseScene);
  const SceneDraw& sd = sc.scene();
  const SceneFrame f = SceneScreen::frame(w, h);
  // The scene's own help bar is BUTTON_BAR_SCENE's (drawn below).
  sc.hide(sc.node("Layer2"));
  // The original draws the rows and the player bar at full size past their
  // text boxes (SPONSOR CHALLENGES, COLLECT .. WRISTBAND).
  sc.bindings().fit_text = false;
  // The list group, placed where the original draws it.
  const int list = sc.node("Option_List/Option_List");
  if (list >= 0) sc.at(list).dx = kListDx, sc.at(list).dy = kListDy;

  const int n = int(ps.rows.size());
  constexpr int kSlots = 7;
  const int first = window_first(ps.selected, n, kSlots);
  for (int slot = 0; slot < kSlots; ++slot) {
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
      sc.at(t).tint = tape;
      if (std::string_view(name) != "Dirt_Tile") sc.at(t).visible_width = tw + 64.0f;
    }
    if (const int clip = child(sd, body, "Clipon"); clip >= 0 && sel > 0.0f) sc.show(clip, sel);
    const float row_dy = i == ps.selected ? kFocusDy : 0.0f;
    if (row_dy != 0.0f) sc.at(btn).dy = row_dy;
    const float k = i == ps.selected ? kPauseFocusScale : 1.0f;
    if (i == ps.selected) sc.at(btn).scale = kPauseFocusScale;
    // Mouse: the row's tapes.
    const auto c = sd.at(text);
    const float x0 = c[0] + kListDx - 30.0f, x1 = c[0] + kListDx + k * tw + 30.0f;
    const float y0 = c[1] + kListDy + row_dy - 24.0f * k, y1 = c[1] + kListDy + row_dy + 24.0f * k;
    l.add_hit(f.x(x0), f.y(y0), f.x(x1), f.y(y1), ui::kHitRow, i);
  }
  if (ps.screen == Screen::kExtras) {
    const int title = sc.node("TEXT_SCREEN_TITLE");
    sc.text(title, text_of(pause_text::kExtras));
    sc.show(title);
  }
  bind_player_bar(sc, st, ps.band_texture);
  sc.draw(l, f);

  if (ps.screen == Screen::kExtras) {
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

// --- 802_MESSAGE_CENTER (offline: no messages) ----------------------------

// INTERIM: offline, the original answers MESSAGE CENTER with a "NO NETWORK
// ACCESS" popup (29_message_center_offline_orig.png). The engine opens the
// message centre's scene with its title and an empty inbox instead; the
// scene's list, tabs, images and description pane draw out of place (their
// stackers and crops are not decoded), so they are hidden and a plain pane
// is drawn behind the message, about TEXT_MESSAGE_TITLE.
constexpr Pane kMessagePane{-24.0f, -22.0f, 520.0f, 96.0f};

void draw_messages(ui::List& l, const PauseScreenHud&, float w, float h) {
  SceneScreen sc(kMessageScene);
  if (!sc.ok()) return;
  const SceneDraw& sd = sc.scene();
  const SceneFrame f = SceneScreen::frame(w, h);
  for (const char* hidden : {"HELP_BUTTON_BAR", "MESSAGE_MENU", "LOADING_ICON", "NAV_POSITION_INDICATOR",
                             "TAB_CONTROL", "DescriptionBox", "MessageCenter_Image", "MessageCenter_ImageCar",
                             "MessageCenter_ImageError"})
    sc.hide(sc.node(hidden));
  const int title = sc.node("TEXT_MESSAGE_TITLE");
  pane(l, f, sd, title, kMessagePane, kSponsorPane);
  sc.text(sc.node("TitleTapeText"), text_of(pause_text::kMessageTitle));
  sc.text(sc.node("TEXT_MESSAGE_TITLE"), text_of(pause_text::kNoMessages));
  sc.text(sc.node("TEXT_MESSAGE"),
          text_of(pause_text::kYouHaveNoMessages) + "\n\n" + text_of(pause_text::kCheckBackLater));
  sc.bindings().fit_text = false;
  sc.draw(l, f);
  help_bar(l, {{"B", text_of(pause_text::kBack)}}, w, h);
}

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
    case Action::kRally: return {{}, {}, 0, "HORIZON RALLY", true};
    case Action::kClub: return {{}, {}, 0, "1000 CLUB", true};
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
}

bool draw_pause_screen(ui::List& l, const DriveHud& st, const PauseScreenHud& ps, float w, float h) {
  switch (ps.screen) {
    case Screen::kSponsorChallenges:
      if (!SceneScreen(kSponsorScene).ok()) return false;
      draw_sponsors(l, st, ps, w, h);
      return true;
    case Screen::kMessageCentre:
      if (!SceneScreen(kMessageScene).ok()) return false;
      draw_messages(l, ps, w, h);
      return true;
    case Screen::kList:
    case Screen::kExtras:
      if (!SceneScreen(kPauseScene).ok()) return false;
      draw_list(l, st, ps, w, h);
      return true;
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
