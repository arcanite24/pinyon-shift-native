// The 1000 Club's screens drawn on the title update's scenes (club_menu.h).
// Calibrated against the original's captures where they exist: the home
// ticket (club_home), the car list (club_carlist), the tutorial popup
// (club_tutorial), SELECT ACTION (club_select_action) and the free roam
// prompts (club_begin). The achievements and details screens have no
// capture: they draw at the scenes' rest (SPEC_QUESTIONS Q143).
#include <algorithm>
#include <cctype>
#include <cmath>

#include "club_menu.h"
#include "game_hud.h"
#include "pause_screen.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kWhite = ui::rgba(255, 255, 255);
constexpr uint32_t kInk = ui::rgba(20, 20, 22);
constexpr uint32_t kGold = ui::rgba(255, 210, 0);
constexpr uint32_t kTapeDark = ui::rgba(22, 22, 24);

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

void show_tree(SceneScreen& sc, int node, float alpha = 1.0f) {
  if (node < 0) return;
  sc.show(node, alpha);
  for (int c : sc.scene().children_of(node)) show_tree(sc, c, alpha);
}

// A bar's fill cut to `frac` from its left end.
void fill_bar(SceneScreen& sc, int node, double frac) {
  if (node < 0) return;
  const auto r = sc.scene().rect(node);
  const float s = float(std::clamp(frac, 0.0, 1.0));
  if (s <= 0.0f) {
    sc.hide(node);
    return;
  }
  sc.at(node).stretch_x = s;
  sc.at(node).dx = -(1.0f - s) * 0.5f * (r[2] - r[0]);
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  size_t from = 0;
  for (size_t nl; (nl = s.find('\n', from)) != std::string::npos; from = nl + 1)
    out.push_back(s.substr(from, nl - from));
  out.push_back(s.substr(from));
  return out;
}

void hide_scene_button_bar(SceneScreen& sc) {
  sc.hide(sc.node("ButtonBar"));
}

void player_bar(SceneScreen& sc, const DriveHud* hud) {
  if (hud)
    bind_player_bar(sc, *hud);
  else
    sc.hide(sc.node("PlayerBar"));
}

// --- H_MESSAGE_POPUP: the tutorial and the unavailable notice ---------------

// club_tutorial.png: TUTORIAL's middle at y 268 over six lines (one blank),
// the lines 26 px apart from y 299, the panel from y 280 to 40 px under the
// last line; the stay-in-car popup (two lines) has its title at y 307. The
// title rises ~9.75 px a line past two.
constexpr float kMsgDim = 0.45f;
constexpr float kMsgWidth = 400.0f;
constexpr float kMsgTitleRest = 295.6f, kMsgMessageRest = 337.1f;
constexpr float kMsgTitleMidTwoLines = 307.0f, kMsgRisePerLine = 9.75f;
constexpr float kMsgTitleMidAbove = 11.4f;  // a title's middle sits this far under its anchor
constexpr float kMsgFirstLineBelowTitle = 31.0f, kMsgLinePitch = 26.0f;
constexpr float kMsgLineMidBelowAnchor = 1.0f;
constexpr float kMsgPanelBelowTitle = 12.0f, kMsgPanelUnderLast = 40.0f;
constexpr float kMsgPanelShown = 332.0f;
// The popups' titles sit lower on their pills than the scenes' text anchors
// draw them (club_select_action.png, rally_confirm.png).
constexpr float kPopupTitleDy = 12.0f;

bool draw_message(ui::List& l, const std::string& title, const std::string& message,
                  const std::vector<ui4::Prompt>& buttons, float w, float h) {
  SceneScreen sc(kClubPopupScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  sc.hide(sc.node("SpinnerBox"));
  hide_scene_button_bar(sc);
  if (const int bg = sc.node("Background"); bg >= 0) {
    sc.show(bg, kMsgDim);
    sc.at(bg).tint = ui::rgba(0, 0, 0);
  }
  const int tn = sc.node("Title"), mn = sc.node("Message");
  std::vector<std::string> lines;
  if (mn >= 0) lines = split_lines(sc.wrap(mn, message, kMsgWidth));
  const float n = float(std::max<size_t>(lines.size(), 1));
  const float title_mid = std::min(kMsgTitleMidTwoLines, kMsgTitleMidTwoLines - kMsgRisePerLine * (n - 2.0f));
  sc.text(tn, upper(title));
  if (tn >= 0) {
    sc.at(tn).tint = kInk;
    sc.at(tn).dy = kPopupTitleDy;  // rally_confirm.png: the title sits mid-pill
  }
  // The title box (the title on its pill) moves as one.
  if (const int box = sc.node("TitleBox"); box >= 0) sc.at(box).dy = title_mid - kMsgTitleMidAbove - kMsgTitleRest;
  const float first_mid = title_mid + kMsgFirstLineBelowTitle;
  const float msg_dy = first_mid - kMsgLineMidBelowAnchor - kMsgMessageRest;
  if (const int panel = sc.node("DescriptionBox/Panel"); panel >= 0) {
    const auto rc = sd.rect(panel);
    const float tall = rc[3] - rc[1], shown = tall - kMsgPanelShown;
    const float top = title_mid + kMsgPanelBelowTitle;
    const float want = first_mid + kMsgLinePitch * (n - 1.0f) + kMsgPanelUnderLast - top;
    const float s = shown > 1.0f ? std::clamp(want / shown, 0.1f, 1.0f) : 1.0f;
    sc.at(panel).stretch_y = s;
    sc.at(panel).dy = top - (0.5f * (rc[1] + rc[3]) - 0.5f * s * tall + s * kMsgPanelShown);
  }
  if (mn >= 0) sc.at(mn).visible = false;
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.draw(l, f);
  for (size_t i = 0; i < lines.size(); ++i)
    sd.text(l, f, mn, lines[i], kWhite, 0.0f, msg_dy + kMsgLinePitch * float(i), false);
  ui4::button_bar(l, buttons, w, h);
  return true;
}

// --- H_POPUP_MENU: SELECT ACTION ---------------------------------------------

// club_select_action.png: the title on its pill at the scene's rest, one
// row (BEGIN CHALLENGES IN THIS CAR) on a pink pill, the dark panel shown
// from under the title to under the row (canvas 274..486) with the grey one
// a little below it. The panels' PANEL01 is offset down its mask (the
// material's V translate, 0.47 and 0.55 of the 512 model): only the part
// from there down shows, squeezed to the box (the scene's own window sizing
// is in its bgf; interim, SPEC_QUESTIONS Q143).
constexpr std::string_view kPopupMenuScene = "H_POPUP_MENU";
struct PopupPanel {
  const char* path;
  float shown_from;  // canvas px under the model's top where it shows
  float top, bottom;
};
constexpr PopupPanel kPopupPanels[] = {{"ContentBox/Panel", 240.0f, 230.0f, 486.0f},
                                       {"PanelGrey", 282.0f, 238.0f, 498.0f}};

bool draw_select_action(ui::List& l, float w, float h) {
  SceneScreen sc(kPopupMenuScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  for (const PopupPanel& p : kPopupPanels) {
    const int panel = sc.node(p.path);
    if (panel < 0) continue;
    const auto rc = sd.rect(panel);
    const float tall = rc[3] - rc[1], shown = tall - p.shown_from;
    const float s = shown > 1.0f ? std::clamp((p.bottom - p.top) / shown, 0.1f, 1.0f) : 1.0f;
    sc.at(panel).stretch_y = s;
    sc.at(panel).dy = p.top - (0.5f * (rc[1] + rc[3]) - 0.5f * s * tall + s * p.shown_from);
  }
  sc.bindings().fit_text = false;
  hide_scene_button_bar(sc);
  sc.text(sc.node("Title"), upper(text_of(club_text::kSelectAction)));
  if (const int t = sc.node("Title"); t >= 0) {
    sc.at(t).tint = kInk;
    sc.at(t).dy = kPopupTitleDy;
  }
  for (int i = 1; i <= 5; ++i) sc.hide(sc.node("Button" + std::to_string(i)));
  sc.hide(sc.node("ScrollBar"));
  const int b0 = sc.node("Button0");
  sc.text(sc.node("TEXT_BUTTON", b0), text_of(club_text::kBegin));
  if (const int pill = sc.node("Pill", b0); pill >= 0) sc.at(pill).tint = ui4::kPink;
  const SceneFrame f = SceneScreen::frame(w, h);
  l.rect(0, 0, w, h, ui::rgba(0, 0, 0, uint8_t(255 * kMsgDim)));
  sc.draw(l, f);
  ui4::button_bar(l, {{"A", text_of(club_text::kSelect)}, {"B", text_of(club_text::kCancel)}}, w, h);
  return true;
}

// --- H_CAR_CHALLENGE_HUB: the home tickets -------------------------------------

// The scene's carousel: Ticket4 is the focused slot (full size at the
// screen's middle, club_home.png's card), Ticket1..3 and 5..7 its
// neighbours at 0.7.
constexpr int kTicketSlots = 7, kFocusSlot = 3;

// The ticket's medal icon in its achievement window, the progress text's
// colours (club_home.png: the count gold, "/1" white).
void bind_achievement_ticket(SceneScreen& sc, int ticket, const ClubAchievementLine& a) {
  const SceneDraw& sd = sc.scene();
  const int ach = sc.node("Achievement", ticket);
  sc.hide(sc.node("TicketSingleCarBase", ach));
  sc.hide(sc.node("TicketSingleCarOverlay", ach));
  sc.hide(sc.node("ChallengeList", ach));
  const int prog = sc.node("AchievementProgress", ach);
  const int tape = child(sd, prog, "Tape");
  sc.text(child(sd, tape, "AchievementProgress"), upper(text_of(club_text::kAchievementProgress)));
  sc.text(child(sd, tape, "MedalProgress"), std::to_string(a.have) + "/" + std::to_string(a.target));
  if (const int bar = child(sd, tape, "ProgressBar"); bar >= 0) {
    // The bar's own `middle` is its fill; Group/middle is the track under
    // it (club_home.png: 0/1 shows the track alone).
    const double frac = a.target > 0 ? double(a.have) / double(a.target) : 0.0;
    fill_bar(sc, child(sd, bar, "middle"), frac);
  }
  // The tape under it is the scene's cyan at rest; club_home.png's is near black.
  for (const char* part : {"LeftEdge", "Centre", "RightEdge"}) {
    if (const int n = child(sd, tape, part); n >= 0) sc.at(n).tint = kTapeDark;
  }
  sc.text(sc.node("Gamerscore", ticket), std::to_string(a.unlocked ? a.gamerscore : 0));
  // interim (SPEC_QUESTIONS Q143): club_home.png shows no name over the
  // first medal's picture; the name stays hidden until a capture shows it.
  sc.hide(sc.node("AchievementName", ticket));
  sc.hide(sc.node("Description", ticket));
  sc.hide(sc.node("SocialChallengeInfo", ticket));
  sc.hide(sc.node("ViewAll", ticket));
}

void bind_current_car_ticket(SceneScreen& sc, int ticket, const ClubMenuData& d) {
  const SceneDraw& sd = sc.scene();
  const int ach = sc.node("Achievement", ticket);
  sc.hide(sc.node("CarChallengeTicketBase", ach));
  sc.hide(sc.node("CarChallengeTicketMiddle", ach));
  sc.hide(sc.node("AchievementProgress", ach));
  sc.show(sc.node("TicketSingleCarBase", ach));
  sc.show(sc.node("TicketSingleCarOverlay", ach));
  const int list = sc.node("ChallengeList", ach);
  sc.show(list);
  for (int i = 0; i < 6; ++i) {
    const int row = child(sd, list, "Challenge" + std::to_string(i + 1));
    if (row < 0) continue;
    if (size_t(i) >= d.challenges.size()) {
      sc.hide(row);
      continue;
    }
    const auto& c = d.challenges[size_t(i)];
    show_tree(sc, row);
    const int medal = child(sd, row, "Medal");
    sc.text(child(sd, medal, "TEXT_CHALLENGE_NUMBER"), std::to_string(i + 1));
    if (c.completed)
      sc.hide(child(sd, medal, "MedalGrey"));
    else
      sc.hide(child(sd, medal, "Medal"));
    sc.text(child(sd, row, "YourGamertag"), upper(c.title));
  }
  const ClubCarEntry* car =
      d.current >= 0 && size_t(d.current) < d.garage.size() ? &d.garage[size_t(d.current)] : nullptr;
  if (const int name = sc.node("AchievementName", ticket); name >= 0) {
    sc.text(name, upper(text_of(club_text::kCurrentChallenges)));
  }
  sc.text(sc.node("Gamerscore", ticket),
          car ? std::to_string(car->medals.earned) + "/" + std::to_string(car->medals.available) : std::string());
  sc.hide(sc.node("Gicon", ticket));
  sc.hide(sc.node("Description", ticket));
  sc.hide(sc.node("SocialChallengeInfo", ticket));
  sc.hide(sc.node("ViewAll", ticket));
  sc.hide(sc.node("AchievementIcon", ticket));
}

void bind_view_all_ticket(SceneScreen& sc, int ticket, const ClubMenuData& d) {
  sc.hide(sc.node("Achievement", ticket));
  sc.hide(sc.node("SocialChallengeInfo", ticket));
  sc.hide(sc.node("Gamerscore", ticket));
  sc.hide(sc.node("Gicon", ticket));
  sc.hide(sc.node("AchievementIcon", ticket));
  sc.hide(sc.node("Description", ticket));
  const int all = sc.node("ViewAll", ticket);
  sc.show(all);
  sc.text(sc.node("Gamertag", all), upper(text_of(club_text::kAllChallenges)));
  sc.text(sc.node("MedalValue", all), std::to_string(d.stats.total_medals));
  if (const int name = sc.node("AchievementName", ticket); name >= 0) sc.hide(name);
}

// The ticket picture's window in its CarImage model (club_home.png: the
// render runs from under the card's top band down to the gold edge, inside
// the card's sides).
constexpr float kPictureTop = 0.13f, kPictureBottom = 0.75f, kPictureInset = 0.02f;

int64_t ticket_car_id(const ClubMenuData& d) {
  if (d.current >= 0 && size_t(d.current) < d.garage.size()) return d.garage[size_t(d.current)].car_id;
  return 0;
}

bool draw_home(ui::List& l, const ClubMenu& m, const ClubMenuData& d, float w, float h, const DriveHud* hud) {
  SceneScreen sc(kClubHomeScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  hide_scene_button_bar(sc);
  player_bar(sc, hud);
  sc.text(sc.node("TitleTape/Group/TitleTapeText"), upper(text_of(club_text::kTitle)));
  const auto tickets = ClubMenu::tickets(d);
  const int sel = std::clamp(m.home, 0, int(tickets.size()) - 1);
  const SceneFrame f = SceneScreen::frame(w, h);
  struct Picture {
    int img = -1;
    int64_t id = 0;
    std::vector<int> over;  // the ticket's nodes drawn again over it
  };
  std::vector<Picture> pictures;
  for (int s = 0; s < kTicketSlots; ++s) {
    const int slot = sc.node("Ticket" + std::to_string(s + 1));
    if (slot < 0) continue;
    const int i = sel + (s - kFocusSlot);
    if (i < 0 || i >= int(tickets.size())) {
      sc.hide(slot);
      continue;
    }
    const int ticket = sc.node("VisibilityGroup/Ticket", slot);
    sc.hide(sc.node("TicketLoading", ticket));
    sc.hide(sc.node("WAIT_SPINNER", ticket));
    switch (tickets[size_t(i)]) {
      case ClubMenu::Ticket::kAchievement: {
        const int a = ClubMenu::next_achievement(d);
        if (a >= 0 && size_t(a) < d.achievements.size()) bind_achievement_ticket(sc, ticket, d.achievements[size_t(a)]);
        break;
      }
      case ClubMenu::Ticket::kCurrentCar:
        bind_current_car_ticket(sc, ticket, d);
        break;
      case ClubMenu::Ticket::kViewAll:
        bind_view_all_ticket(sc, ticket, d);
        break;
    }
    // The ticket's picture: the driven car's thumbnail over the card's
    // middle (CARCHALLENGETICKETMIDDLE is opaque there), under its
    // gamerscore, G and title. (TICKETSINGLECAROVERLAY over it darkens the
    // whole picture, so it stays under.)
    if (const int img = sc.node("CarImage", ticket); img >= 0) {
      if (const int64_t id = ticket_car_id(d); id > 0) {
        Picture p{img, id, {}};
        for (const char* over : {"AchievementName", "Gamerscore", "Gicon"}) {
          const int n = sc.node(over, ticket);
          if (n < 0) continue;
          const auto it = sc.bindings().by_node.find(n);
          if (it != sc.bindings().by_node.end() && it->second.visible == false) continue;
          p.over.push_back(n);
        }
        pictures.push_back(std::move(p));
        sc.hide(img);
      }
    }
  }
  sc.draw(l, f);
  for (const auto& p : pictures) {
    const auto r = sd.rect(p.img);
    const float top = r[1] + kPictureTop * (r[3] - r[1]), bottom = r[1] + kPictureBottom * (r[3] - r[1]);
    const float inset = kPictureInset * (r[2] - r[0]);
    ui4::car_thumbnail(l, f.x(r[0] + inset), f.y(top), f.x(r[2] - inset), f.y(bottom), p.id);
    for (int n : p.over) sc.draw(l, f, n);
  }
  if (m.popup == ClubMenu::Popup::kNone)
    ui4::button_bar(l, {{"A", text_of(club_text::kSelect)}, {"B", text_of(club_text::kBack)}}, w, h);
  return true;
}

// --- H_CAR_CHALLENGE_ACHIEVEMENTS ----------------------------------------------

constexpr int kAchievementRows = 6;
constexpr float kRowFocused = 1.0f, kRowIdle = 0.51f;
constexpr float kDescriptionWidth = 360.0f;

bool draw_achievements(ui::List& l, const ClubMenu& m, const ClubMenuData& d, float w, float h, const DriveHud* hud) {
  SceneScreen sc(kClubAchievementsScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  hide_scene_button_bar(sc);
  player_bar(sc, hud);
  sc.text(sc.node("TitleTape/Group/TitleTapeText"), upper(text_of(club_text::kAchievements)));
  const int n = int(d.achievements.size());
  const int sel = std::clamp(m.achievement, 0, std::max(0, n - 1));
  const int first = window_first(sel, n, kAchievementRows);
  const int bars = sc.node("MenuBars");
  for (int r = 0; r < kAchievementRows; ++r) {
    const int bar = sc.node(r == 0 ? std::string("AchievementBar") : "AchievementBar" + std::to_string(r + 1), bars);
    if (bar < 0) continue;
    const int i = first + r;
    if (i >= n) {
      sc.hide(bar);
      continue;
    }
    const auto& a = d.achievements[size_t(i)];
    const int det = child(sd, bar, "AchievementDetail");
    show_tree(sc, det, i == sel ? kRowFocused : kRowIdle);
    // interim (SPEC_QUESTIONS Q143): the row's Sponsor_Back pane rests far
    // up and right of the row (canvas 629..1653 x -286..226), a dark block
    // over the player bar; its place in a focused row is not known.
    sc.hide(child(sd, bar, "Sponsor_Back"));
    // The row names the achievement; its description is the box's
    // (DescriptionBox: TEXT_ACH_TITLE over TEXT_ACH_DESCRIPTION). The full
    // description here ran under TEXT_REQUIRED and the next row.
    sc.text(child(sd, det, "TEXT_ACH_TITLE"), a.name.empty() ? club_achievement_desc(a) : upper(a.name));
    sc.text(child(sd, det, "TEXT_REQUIRED"), a.unlocked ? std::string() : club_required_text(a));
    sc.text(child(sd, det, "TEXT_GAMERPOINTS"), std::to_string(a.gamerscore));
    if (const int pb = child(sd, det, "ProgressBar"); pb >= 0)
      fill_bar(sc, child(sd, pb, "ProgressBar"), a.target > 0 ? double(a.have) / double(a.target) : 0.0);
  }
  if (n > 0) {
    const auto& a = d.achievements[size_t(sel)];
    const int box = sc.node("DescriptionBox");
    sc.text(sc.node("TEXT_ACH_TITLE", box), a.name.empty() ? upper(text_of(club_text::kTitle)) : upper(a.name));
    const int desc = sc.node("TEXT_ACH_DESCRIPTION", box);
    // Wrapped to the box's panel (HUDPanel01, 400 px), not the rest text's
    // 143 px.
    sc.text(desc, sc.wrap(desc, club_achievement_desc(a), kDescriptionWidth));
  }
  sc.hide(sc.node("NAV_POSITION_INDICATOR"));
  sc.draw(l, SceneScreen::frame(w, h));
  if (m.popup == ClubMenu::Popup::kNone)
    ui4::button_bar(l, {{"A", text_of(club_text::kSelect)}, {"B", text_of(club_text::kBack)}}, w, h);
  return true;
}

// --- H_CAR_CHALLENGE_BUY_CARSELECT: an achievement's cars -----------------------

// club_carlist.png: the list's focused tile is CAR_LIST_ITEM_3 (its pink
// cursor and OWNED pill at rest), the cars after it to the right; the
// details panel (name, year and make, class and PI, picture, challenges)
// at the scene's rest; the achievement's text on the title tape.
constexpr int kCarSlots = 7, kCarFocus = 3;
// club_carlist.png's challenge rows are 27.5 px apart (the scene's rest:
// 22.3), and the dark HUDPanel01 behind them runs from under the photo to
// kRowPanelUnder below the last row. The panel's HUDPANEL01 shows only its
// model's top kHudPanelShown (the material's V translate, 0.36 of 512).
constexpr float kRowExtraPitch = 5.2f;
constexpr float kRowPanelTop = 20.0f, kRowPanelUnder = 24.0f;
constexpr float kHudPanelShown = 184.0f;

bool draw_car_list(ui::List& l, const ClubMenu& m, const ClubMenuData& d, float w, float h) {
  SceneScreen sc(kClubCarListScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  hide_scene_button_bar(sc);
  // interim (SPEC_QUESTIONS Q143): the scene's PlayerBar shares its node
  // names with the details panel; the club_carlist.png bar (top left) is
  // not drawn yet.
  sc.hide(sc.node("PlayerBar"));
  const auto cars = ClubMenu::cars_for(d, m.filter);
  const int n = int(cars.size());
  const int sel = std::clamp(m.car, 0, std::max(0, n - 1));
  // The title tape: the achievement still to earn.
  const ClubAchievementLine* ach = nullptr;
  for (const auto& a : d.achievements)
    if (a.id == m.filter) ach = &a;
  const int title = sc.node("TitleTape/TitleTape");
  sc.text(sc.node("TitleTapeText", title),
          ach ? club_achievement_desc(*ach) : upper(text_of(club_text::kAllChallenges)));
  sc.hide(sc.node("SubTitle/SubTitleTapeText", title));
  if (!ach) sc.hide(sc.node("SubTitle/AchievementIcon", title));
  sc.hide(sc.node("STATS"));
  sc.hide(sc.node("PURCHASED"));
  sc.hide(sc.node("PACK_NAME"));
  const SceneFrame f = SceneScreen::frame(w, h);
  const int group = sc.node("H_CAR_CHALLENGE_CAR_LIST/Group");
  std::vector<std::pair<int, int64_t>> tiles;
  for (int s = 0; s < kCarSlots; ++s) {
    const int item = child(sd, group, "CAR_LIST_ITEM_" + std::to_string(s));
    if (item < 0) continue;
    const int i = sel + (s - kCarFocus);
    const int icons = child(sd, item, "Icons"), image = child(sd, item, "IMAGE");
    if (i < 0 || i >= n) {
      sc.hide(icons);
      continue;  // an empty tile (club_carlist.png's left panes)
    }
    const ClubCarEntry& c = d.garage[size_t(cars[size_t(i)])];
    sc.show(icons);
    for (int ic : sd.children_of(icons)) sc.hide(ic);
    // The tile's corner icon: the driven car's wheel, else the garage's.
    sc.show(child(sd, icons, c.current ? "Ready_Icon" : "Not_Ready_Icon"));
    const int small = child(sd, icons, "SmallMedal");
    show_tree(sc, small);
    sc.text(child(sd, small, "TEXT_MEDAL_COUNT"), std::to_string(c.medals.earned));
    sc.text(child(sd, small, "TEXT_MEDAL_TOTAL"), "/" + std::to_string(c.medals.available));
    if (c.medals.available > 0 && c.medals.earned >= c.medals.available) sc.show(child(sd, icons, "MedalComplete"));
    sc.show(image);
    sc.hide(child(sd, image, "CAR_IMAGE_NEW"));
    sc.hide(child(sd, image, "CLUB_IMAGE"));
    tiles.emplace_back(child(sd, image, "CAR_IMAGE_NEW"), c.car_id);
  }
  // The cursor's pill: OWNED (every listed car is a garage car).
  const int hl = sc.node("HIGHLIGHT");
  // Its BigPane01 is the cursor's pane at rest, not in the capture (a black
  // block over the world above the tiles).
  sc.hide(child(sd, hl, "BigPane01"));
  if (n > 0)
    sc.text(child(sd, hl, "TEXT_CURSOR"), upper(text_of(club_text::kOwned)));
  else
    sc.hide(hl);
  // The details panel.
  const int stat = sc.node("CarStatsGroup/H_CAR_CHALLENGE_STAT");
  const int clist = sc.node("CHALLENGE_LIST/Group");
  int64_t photo_id = 0;
  if (n > 0) {
    const ClubCarEntry& c = d.garage[size_t(cars[size_t(sel)])];
    photo_id = c.car_id;
    sc.text(sc.node("TitleBar/TEXT_CAR_NAME", stat), c.name);
    const int info = sc.node("INFO", stat);
    sc.text(sc.node("TEXT_MANUFACTURER", info), c.year_make);
    if (!c.class_letter.empty()) {
      const int pi = sc.node("CLASS_SM/Group/TEXT_PI", info);
      sc.text(pi, std::to_string(c.pi));
      if (pi >= 0) sc.at(pi).tint = kInk;
      if (const int cls = sc.node("CLASS_SM/Group/CLASS_A", info); cls >= 0)
        sc.at(cls).texture = "PICLASS/CLASS_" + upper(c.class_letter);
    } else {
      sc.hide(sc.node("CLASS_SM", info));
    }
    sc.hide(sc.node("FLAG", info));
    sc.hide(sc.node("DRIVETRAIN", info));
    float last_row_y = 0.0f;
    for (int r = 0; r < 6; ++r) {
      const int row = child(sd, clist, "H_CAR_CHALLENGE_STAT_BARS_" + std::to_string(r + 1));
      if (row < 0) continue;
      if (size_t(r) >= c.challenge_titles.size()) {
        sc.hide(row);
        continue;
      }
      sc.at(row).dy = kRowExtraPitch * float(r);
      last_row_y = sd.at(row)[1] + kRowExtraPitch * float(r);
      const bool done = size_t(r) < c.completed.size() && c.completed[size_t(r)];
      sc.text(child(sd, row, "TEXT_NAME"), upper(c.challenge_titles[size_t(r)]));
      sc.text(child(sd, row, "TEXT_NAME2"), std::to_string(r + 1));
      if (done) {
        sc.show(child(sd, row, "MedalSmall"));
        sc.hide(child(sd, row, "MedalMenuInactive"));
      }
    }
    if (const int panel = sc.node("BG/HUDPanel01", stat); panel >= 0 && last_row_y > 0.0f) {
      const auto rc = sd.rect(panel);
      const float s = std::max(0.1f, (last_row_y + kRowPanelUnder - kRowPanelTop) / kHudPanelShown);
      sc.at(panel).stretch_y = s;
      sc.at(panel).dy = kRowPanelTop - (0.5f * (rc[1] + rc[3]) - 0.5f * s * (rc[3] - rc[1]));
    }
  } else {
    sc.hide(sc.node("CarStatsGroup"));
    sc.hide(sc.node("CHALLENGE_LIST"));
    sc.hide(sc.node("CAR"));
  }
  if (const int thumb = sc.node("CAR/Car_Thumb_"); thumb >= 0 && photo_id > 0) sc.hide(thumb);
  for (const auto& [img, id] : tiles) {
    const auto r = sd.rect(img);
    ui4::car_thumbnail(l, f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), id);
  }
  sc.draw(l, f);
  if (photo_id > 0) {
    if (const int thumb = sc.node("CAR/Car_Thumb_"); thumb >= 0) {
      const auto r = sd.rect(thumb);
      ui4::car_thumbnail(l, f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), photo_id);
    }
  }
  if (m.popup == ClubMenu::Popup::kNone)
    ui4::button_bar(l, {{"A", text_of(club_text::kSelectCar)}, {"B", text_of(club_text::kBack)}}, w, h);
  return true;
}

// --- H_CAR_CHALLENGE_DETAILS: the current car's challenges ----------------------

constexpr int kDetailRows = 6;
constexpr float kDetailWidth = 420.0f;
constexpr float kSmallTapeLift = -28.0f;

bool draw_details(ui::List& l, const ClubMenu& m, const ClubMenuData& d, float w, float h, const DriveHud* hud) {
  SceneScreen sc(kClubDetailsScene);
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  hide_scene_button_bar(sc);
  player_bar(sc, hud);
  const ClubCarEntry* car =
      d.current >= 0 && size_t(d.current) < d.garage.size() ? &d.garage[size_t(d.current)] : nullptr;
  sc.text(sc.node("TitleTapeSmall/Group/TitleTapeText"), upper(text_of(club_text::kCurrentChallenges)));
  // interim (SPEC_QUESTIONS Q143): at rest the small tape (y 135..170) lies
  // across the car's name tape (144..214); it is lifted clear of it.
  if (const int small = sc.node("TitleTapeSmall"); small >= 0) sc.at(small).dy = kSmallTapeLift;
  sc.text(sc.node("TitleTape/Group/TitleTapeText"), car ? upper(car->name) : std::string());
  const int list = sc.node("Challenge_List/ChallengeList");
  sc.text(sc.node("TEXT_CHALLENGE_TITLE"), upper(text_of(club_text::kChallenges)));
  sc.hide(sc.node("TEXT_CHALLENGE_DIFFICULTY"));
  const int n = int(d.challenges.size());
  const int sel = std::clamp(m.detail, 0, std::max(0, n - 1));
  for (int r = 0; r < kDetailRows; ++r) {
    const int bar = child(sd, list, r == 0 ? std::string("ChallengeBar") : "ChallengeBar" + std::to_string(r + 1));
    if (bar < 0) continue;
    if (r >= n) {
      sc.hide(bar);
      continue;
    }
    const auto& c = d.challenges[size_t(r)];
    sc.text(child(sd, bar, "TEXT_CHALLENGE"), upper(c.title));
    sc.hide(child(sd, bar, "TEXT_DIFFICULTY"));
    const int medal = child(sd, bar, "Medal");
    sc.text(child(sd, medal, "TEXT_CHALLENGE_NUMBER"), std::to_string(r + 1));
    sc.hide(child(sd, medal, c.completed ? "MedalGrey" : "Medal"));
    // interim (SPEC_QUESTIONS Q143): the scene's rows are all pink at rest;
    // the focused row keeps it and the others are dark, as the game's other
    // pill lists.
    if (const int hb = child(sd, bar, "HeaderBar"); hb >= 0 && r != sel) sc.at(hb).tint = kTapeDark;
  }
  const int box = sc.node("DetailsBox");
  if (n > 0) {
    const auto& c = d.challenges[size_t(sel)];
    sc.text(sc.node("TEXT_INFO_TITLE", box), upper(c.title));
    std::string info = c.description;
    if (!c.hint.empty()) info += "\n\n" + c.hint;
    if (!c.status.empty()) info += "\n\n" + c.status;
    const int tn = sc.node("TEXT_INFO", box);
    sc.text(tn, sc.wrap(tn, info, kDetailWidth));
  } else {
    sc.hide(box);
  }
  sc.draw(l, SceneScreen::frame(w, h));
  if (m.popup == ClubMenu::Popup::kNone) ui4::button_bar(l, {{"B", text_of(club_text::kBack)}}, w, h);
  return true;
}

}  // namespace

bool draw_scene_message(ui::List& l, const std::string& title, const std::string& message,
                        const std::vector<ui4::Prompt>& buttons, float w, float h) {
  return draw_message(l, title, message, buttons, w, h);
}

bool draw_rally_entry_popup(ui::List& l, RallyEntryPopup p, float w, float h) {
  switch (p) {
    case RallyEntryPopup::kNone:
      return false;
    case RallyEntryPopup::kGate:
      return draw_message(l, text_of(rally_text::kGateTitle), text_of(rally_text::kGateMsg),
                          {{"A", text_of(club_text::kOk)}}, w, h);
    case RallyEntryPopup::kConfirm:
      return draw_message(l, text_of(rally_text::kConfirmTitle), text_of(rally_text::kConfirmMsg),
                          {{"A", text_of(club_text::kOk)}, {"B", text_of(club_text::kCancel)}}, w, h);
  }
  return false;
}

bool draw_club_menu(ui::List& l, const ClubMenu& m, const ClubMenuData& d, float w, float h, const DriveHud* hud) {
  bool ok = true;
  switch (m.screen) {
    case ClubMenu::Screen::kClosed:
      return false;
    case ClubMenu::Screen::kIntro:
      // The video plays over this (the drive's FMV player); black behind.
      l.rect(0, 0, w, h, ui::rgba(0, 0, 0));
      return true;
    case ClubMenu::Screen::kHome:
      // Gated: the unavailable popup over the world, no home behind.
      if (m.popup != ClubMenu::Popup::kUnavailable) ok = draw_home(l, m, d, w, h, hud);
      break;
    case ClubMenu::Screen::kAchievements:
      ok = draw_achievements(l, m, d, w, h, hud);
      break;
    case ClubMenu::Screen::kCarList:
      ok = draw_car_list(l, m, d, w, h);
      break;
    case ClubMenu::Screen::kDetails:
      ok = draw_details(l, m, d, w, h, hud);
      break;
  }
  if (!ok) return false;
  switch (m.popup) {
    case ClubMenu::Popup::kNone:
      break;
    case ClubMenu::Popup::kTutorial:
      ok = draw_message(l, text_of(club_text::kTutorial), text_of(m.tutorial_text), {{"A", text_of(club_text::kOk)}}, w,
                        h);
      break;
    case ClubMenu::Popup::kUnavailable:
      ok = draw_message(l, text_of(d.gate_title), text_of(d.gate_message), {{"A", text_of(club_text::kOk)}}, w, h);
      break;
    case ClubMenu::Popup::kSelectAction:
      ok = draw_select_action(l, w, h);
      break;
  }
  return ok;
}

// --- 947_HUD: the challenge items, the details prompt, the tutorial's line ----

// club_begin.png: the tutorial's instruction at the top left (its text from
// x 95 at y 45, the info icon left of it), the details prompt at the bottom
// middle (its text's middle at y 674).
constexpr float kInstructionX = 95.0f, kInstructionY = 45.0f;
constexpr float kInstructionBarPad = 24.0f;
constexpr float kPromptY = 674.0f;
// The medal flash after a pass (Large_Medal): its first second.
constexpr double kLargeMedalSeconds = 1.0;

bool draw_club_hud(ui::List& l, const ClubHud& hud, float w, float h) {
  SceneScreen sc("947_HUD");
  if (!sc.ok()) return false;
  const SceneDraw& sd = sc.scene();
  sc.bindings().fit_text = false;
  const SceneFrame f = SceneScreen::frame(w, h);
  const int root = sc.node("HUDCarChallengePrompt");
  if (root < 0) return false;
  const int items = sc.node("Items", root);
  for (int i = 0; i < 6; ++i) {
    const int item = child(sd, items, "HUDCarChallengeItem" + std::to_string(i + 1));
    if (item < 0) continue;
    if (size_t(i) >= hud.items.size()) {
      sc.hide(item);
      continue;
    }
    const ClubHudItem& it = hud.items[size_t(i)];
    const int vis = child(sd, item, "visibilityHelper");
    show_tree(sc, vis);
    const int scale = sc.node("Medal/Scale_Helper", vis);
    sc.text(child(sd, scale, "NumberText"), std::to_string(it.number));
    for (const char* fx : {"Circle_Glow", "MedalFlash", "Medal_Shine"}) sc.hide(child(sd, scale, fx));
    const bool passed = it.result == ClubHudItem::Result::kPassed;
    const bool failed = it.result == ClubHudItem::Result::kFailed;
    sc.hide(child(sd, scale, passed ? "MedalActive" : "MedalComplete"));
    const int large = child(sd, vis, "Large_Medal");
    if (!passed || it.result_age > kLargeMedalSeconds) sc.hide(large);
    // Its glow and shine are the COMPLETE animation's flashes (a 200 px wash
    // at rest); the medal alone stays.
    for (const char* fx : {"Medal_Shine", "Circle_Glow"}) sc.hide(child(sd, large, fx));
    const int mode = child(sd, vis, "DisplayMode");
    const int result = child(sd, vis, "ResultText");
    if (passed || failed) {
      sc.hide(mode);
      sc.text(result, upper(text_of(passed ? club_text::kMedalEarned : club_text::kFailed)));
      if (result >= 0) sc.at(result).tint = passed ? kGold : kWhite;
    } else {
      sc.hide(result);
      sc.text(child(sd, mode, "DescriptionText"), it.text);
      sc.hide(child(sd, mode, "TimerText"));
    }
  }
  const int prompt = sc.node("PromptPosition/Prompt", root);
  int prompt_text = -1;
  std::string hint;
  if (hud.details_hint && prompt >= 0) {
    const int vis = child(sd, prompt, "visibilityHelper");
    show_tree(sc, vis);
    sc.hide(sc.node("ButtonFlash", vis));
    prompt_text = child(sd, vis, "PromptText");
    // "PRESS <dpad> TO VIEW CAR CHALLENGE DETAILS": the glyph between the
    // two halves.
    hint = upper(text_of(club_text::kDetailsHintR));
    const float left_w = sd.measure(prompt_text, upper(text_of(club_text::kDetailsHintL)));
    // The glyph's room: the D-pad (30 px, club_begin.png) or the keyboard set's
    // wider key cap, with a 4 px gap each side.
    const float glyph = std::max(34.0f, 2.0f * ui4::button_glyph_half_width("DPAD", 15.0f) + 8.0f);
    const float total = left_w + glyph + sd.measure(prompt_text, hint);
    const auto at = sd.at(prompt);
    const float x0 = 640.0f - 0.5f * total;
    sc.at(prompt).dy = kPromptY - at[1];
    if (const int icon = child(sd, vis, "ButtonIcon"); icon >= 0) {
      sc.at(icon).texture = "CONTROLLER/D_PAD";
      sc.at(icon).dx = x0 + left_w + 0.5f * glyph - sd.at(icon)[0];
    }
    if (prompt_text >= 0) {
      sc.at(prompt_text).dx = x0 + left_w + glyph - sd.at(prompt_text)[0];
      sc.text(prompt_text, hint);
    }
    sc.draw(l, f, root);
    if (prompt_text >= 0)
      sd.text(l, f, prompt_text, upper(text_of(club_text::kDetailsHintL)), kWhite, x0 - sd.at(prompt_text)[0],
              kPromptY - at[1], false);
  } else {
    if (prompt >= 0) sc.hide(prompt);
    sc.draw(l, f, root);
  }
  // The tutorial's line (HUDInstruction), drawn from its own copy of the
  // scene (its root is outside HUDCarChallengePrompt).
  if (!hud.instruction.empty()) {
    SceneScreen is("947_HUD");
    is.bindings().fit_text = false;
    const int ins = is.node("HUDInstruction");
    const int line = is.node("InstructionText/Line1", ins);
    const int text = is.node("Bar/Text", line);
    if (ins >= 0 && line >= 0 && text >= 0) {
      is.hide(is.node("InstructionText/Line2", ins));
      is.hide(is.node("ObjectiveIcon", ins));
      const auto ta = is.scene().at(text);
      is.at(line).dx = kInstructionX - ta[0];
      is.at(line).dy = kInstructionY - ta[1];
      is.text(text, hud.instruction);
      if (const int bar = is.node("Bar/Bar", line); bar >= 0)
        is.at(bar).visible_width = is.scene().measure(text, hud.instruction) + kInstructionBarPad;
      is.draw(l, f, ins);
    }
  }
  return true;
}

const std::vector<std::pair<std::string_view, std::vector<const char*>>>& club_screen_nodes() {
  static const std::vector<std::pair<std::string_view, std::vector<const char*>>> k = {
      {kClubHomeScene,
       {"TitleTape/Group/TitleTapeText",
        "Ticket1",
        "Ticket4",
        "Ticket7",
        "VisibilityGroup/Ticket",
        "Achievement",
        "AchievementProgress",
        "MedalProgress",
        "ProgressBar",
        "Gamerscore",
        "Gicon",
        "AchievementName",
        "ChallengeList",
        "TicketSingleCarBase",
        "ViewAll",
        "MedalValue",
        "CarImage",
        "AchievementIcon",
        "PlayerBar",
        "ButtonBar"}},
      {kClubAchievementsScene,
       {"TitleTape/Group/TitleTapeText", "MenuBars", "AchievementBar", "AchievementBar6", "AchievementDetail",
        "TEXT_ACH_TITLE", "TEXT_REQUIRED", "TEXT_GAMERPOINTS", "DescriptionBox", "TEXT_ACH_DESCRIPTION",
        "NAV_POSITION_INDICATOR", "PlayerBar", "ButtonBar"}},
      {kClubCarListScene,
       {"H_CAR_CHALLENGE_CAR_LIST/Group",
        "CAR_LIST_ITEM_0",
        "CAR_LIST_ITEM_3",
        "CAR_LIST_ITEM_6",
        "Icons",
        "SmallMedal",
        "TEXT_MEDAL_COUNT",
        "TEXT_MEDAL_TOTAL",
        "Ready_Icon",
        "Not_Ready_Icon",
        "MedalComplete",
        "CAR_IMAGE_NEW",
        "HIGHLIGHT",
        "TEXT_CURSOR",
        "CarStatsGroup/H_CAR_CHALLENGE_STAT",
        "TitleBar/TEXT_CAR_NAME",
        "TEXT_MANUFACTURER",
        "CLASS_SM/Group/TEXT_PI",
        "CHALLENGE_LIST/Group",
        "H_CAR_CHALLENGE_STAT_BARS_1",
        "H_CAR_CHALLENGE_STAT_BARS_6",
        "CAR/Car_Thumb_",
        "TitleTape/TitleTape",
        "TitleTapeText",
        "SubTitle/SubTitleTapeText",
        "SubTitle/AchievementIcon",
        "STATS",
        "PlayerBar",
        "ButtonBar"}},
      {kClubDetailsScene,
       {"TitleTapeSmall/Group/TitleTapeText", "TitleTape/Group/TitleTapeText", "Challenge_List/ChallengeList",
        "ChallengeBar", "ChallengeBar6", "TEXT_CHALLENGE", "TEXT_CHALLENGE_NUMBER", "HeaderBar", "TEXT_CHALLENGE_TITLE",
        "DetailsBox", "TEXT_INFO_TITLE", "TEXT_INFO", "PlayerBar", "ButtonBar"}},
      {kClubPopupScene, {"SpinnerBox", "Background", "Title", "TitleBox", "Message", "DescriptionBox/Panel"}},
      {kPopupMenuScene,
       {"Title", "Button0", "Button5", "TEXT_BUTTON", "Pill", "ScrollBar", "ContentBox/Panel", "PanelGrey"}},
      {"947_HUD",
       {"HUDCarChallengePrompt", "Items", "HUDCarChallengeItem1", "HUDCarChallengeItem6", "visibilityHelper",
        "Medal/Scale_Helper", "NumberText", "MedalActive", "MedalComplete", "Large_Medal", "DisplayMode",
        "DescriptionText", "ResultText", "PromptPosition/Prompt", "ButtonIcon", "PromptText", "HUDInstruction",
        "InstructionText/Line1", "Bar/Text"}},
  };
  return k;
}

}  // namespace carview
