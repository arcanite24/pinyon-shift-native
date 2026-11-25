#include "hub_menus.h"

#include <algorithm>

#include "race_screens.h"
#include "ui4_chrome.h"

namespace carview {

std::vector<HubMenuRow> hub_menu_rows(HubMenu m) {
  namespace t = hub_menu_text;
  using A = HubMenuAction;
  switch (m) {
    case HubMenu::kProfile:
      return {{t::kOptions, A::kOptions},
              {t::kStats, A::kStats},
              {t::kAchievements, A::kAchievements},
              {t::kTeamCredits, A::kTeamCredits}};
    case HubMenu::kOptions:
      return {{t::kControls, A::kControls},
              {t::kDifficulty, A::kDifficulty},
              {t::kHud, A::kHud},
              {t::kAudio, A::kAudio},
              {t::kVideo, A::kVideo}};
    case HubMenu::kPaintBooth:
      return {{t::kPaintCar, A::kPaintCar},
              {t::kAddDecals, A::kAddDecals},
              {t::kCreateVinylGroup, A::kCreateVinylGroup},
              {t::kMyVinylGroups, A::kMyVinylGroups},
              {t::kFm4VinylGroups, A::kFm4VinylGroups},
              {t::kClearDesign, A::kClearDesign}};
    case HubMenu::kWorkshop:
      // interim (SPEC_QUESTIONS Q131): the order of Workshop.str's rows is
      // not captured; the string table's listing order.
      return {{t::kChangeCar, A::kChangeCar},
              {t::kUpgradeCar, A::kUpgradeCar},
              {t::kWorkshopPaint, A::kPaintCar},
              {t::kBarnFinds, A::kBarnFinds}};
    case HubMenu::kUpgradeChoice:
      return {{t::kAutoUpgrade, A::kAutoUpgrade}, {t::kCustomUpgrade, A::kCustomUpgrade}};
    case HubMenu::kPhotoSelect:
      return {{t::kTakePhoto, A::kTakePhoto}, {t::kViewPhoto, A::kViewPhoto}, {t::kPhotoShare, A::kPhotoShare}};
  }
  return {};
}

bool hub_menu_extra(HubMenu m, HubMenuRow& out) {
  if (m == HubMenu::kOptions) {
    out = {hub_menu_text::kEngineSettings, HubMenuAction::kEngineSettings};
    return true;
  }
  return false;
}

void HubMenuStack::move(int by) {
  if (levels.empty()) return;
  const int n = int(hub_menu_rows(levels.back().menu).size());
  levels.back().selected = wrap_row(levels.back().selected, by, n);
}

void HubMenuStack::select(int row) {
  if (levels.empty()) return;
  const int n = int(hub_menu_rows(levels.back().menu).size());
  if (row >= 0 && row < n) levels.back().selected = row;
}

bool HubMenuStack::action(HubMenuAction& out) const {
  if (levels.empty()) return false;
  const auto rows = hub_menu_rows(levels.back().menu);
  if (rows.empty()) return false;
  out = rows[size_t(std::clamp(levels.back().selected, 0, int(rows.size()) - 1))].action;
  return true;
}

void fill_hub_menu(const HubMenuStack& stack, HubMenuHud& out) {
  out = {};
  if (!stack.open()) return;
  for (const auto& r : hub_menu_rows(stack.top())) out.rows.push_back(text_of(r.text));
  out.selected = stack.selected();
  // Pairs 20, 21, 22 and 31: A SELECT, B BACK.
  out.prompts = {{"A", text_of(hub_menu_text::kSelect)}, {"B", text_of(hub_menu_text::kBack)}};
  HubMenuRow extra{};
  if (hub_menu_extra(stack.top(), extra)) out.prompts.push_back({"X", text_of(extra.text)});
}

bool draw_hub_menu(ui::List& l, const HubMenuHud& m, float w, float h) {
  SceneScreen sc(kGenericMenuScene);
  if (!sc.ok()) return false;
  const SceneFrame f = SceneScreen::frame(w, h);
  sc.bindings().fit_text = false;
  sc.hide(sc.node("ButtonBar"));
  // The list centres on its row count (hub_menu_layout::list_dy).
  bind_menu_rows(sc, l, f, m.rows, m.selected, hub_menu_layout::list_dy(int(m.rows.size())));
  sc.draw(l, f);
  if (!m.notice.empty()) {
    // The engine's notice (a row it has no screen for yet), where the hub
    // screens put theirs.
    const float s = std::max(0.25f, h / 720.0f);
    ui4::brush(l, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    std::string text = m.notice;
    ui4::fit_text(text, ns, 340.0f * s, 9.0f * s);
    l.text(w - 68.0f * s, h - 60.0f * s, text, ns);
  }
  std::vector<ui4::Prompt> prompts;
  for (const auto& [b, label] : m.prompts) prompts.push_back({b.c_str(), label});
  ui4::button_bar(l, prompts, w, h);
  return true;
}

}  // namespace carview
