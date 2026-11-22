#include "events_page.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "game_hud.h"

namespace carview {
namespace {

const char* tab_name(EventsPage::Tab t) {
  switch (t) {
    case EventsPage::kAll: return "ALL EVENTS";
    case EventsPage::kFestival: return "FESTIVAL";
    case EventsPage::kStreet: return "STREET RACES";
    case EventsPage::kShowcase: return "SHOWCASES";
    case EventsPage::kShowdown: return "SHOWDOWNS";
    default: return "EVENTS";
  }
}

std::string resolve_event_name(const race::CareerEvent& ev) {
  if (!ev.name_ref.empty()) {
    const std::string t = game_text(ev.name_ref);
    if (!t.empty() && t.rfind("_&", 0) != 0) return t;
  }
  return ev.horizon_id;
}

}  // namespace

void EventsPage::open(const race::CareerCatalog& catalog, const race::CareerRecord& career, int collected_wristband,
                      int popularity_rank, int64_t current_car_id, int current_class_id,
                      const std::vector<garage::ClassBand>& bands, int hub_filter) {
  catalog_ = &catalog;
  career_ = &career;
  collected_wristband_ = collected_wristband;
  popularity_rank_ = popularity_rank;
  car_id_ = current_car_id;
  car_class_id_ = current_class_id;
  class_names_.clear();
  for (const auto& b : bands) class_names_.push_back(b.name);
  hub_filter_ = hub_filter;
  tab_ = hub_filter > 0 ? kStreet : kAll;
  selected_ = 0;
  open_ = true;
  rebuild();
}

void EventsPage::rebuild() {
  entries_.clear();
  if (!catalog_ || !career_) return;

  // If a new wristband is ready to collect at Race Central (and not in a street hub), show it at the very top!
  if (hub_filter_ == 0) {
    const int pending = catalog_->pending_wristband(career_->wristband_points, collected_wristband_);
    if (pending >= 0 && size_t(pending) < catalog_->wristbands.size()) {
      Entry e;
      e.is_collect_wristband = true;
      e.wristband_index = pending;
      e.unlocked = true;
      e.eligible = true;
      std::string cname = catalog_->wristbands[size_t(pending)].colour_key;
      for (char& c : cname) c = char(std::toupper(static_cast<unsigned char>(c)));
      e.display_name = "COLLECT " + cname + " WRISTBAND!";
      entries_.push_back(std::move(e));
    }
  }

  std::vector<Entry> unlocked_list, locked_list;
  for (const auto& ev : catalog_->events) {
    if (hub_filter_ > 0) {
      if (ev.style != 2 || ev.hub_id != hub_filter_) continue;
    } else {
      if (tab_ == kFestival && ev.style != 1) continue;
      if (tab_ == kStreet && ev.style != 2) continue;
      if (tab_ == kShowcase && ev.style != 3) continue;
      if (tab_ == kShowdown && ev.style != 5 && ev.style != 6) continue;
    }

    Entry e;
    e.event = &ev;
    e.unlocked = catalog_->is_event_unlocked(ev, *career_, collected_wristband_, popularity_rank_);
    e.best_place = catalog_->best_place(ev, *career_);
    e.display_name = resolve_event_name(ev);
    if (e.unlocked) {
      const auto elig = catalog_->check_eligibility(ev, car_id_, car_class_id_, class_names_);
      e.eligible = elig.eligible;
      e.uses_loaner = elig.uses_loaner;
      e.reason = elig.reason;
      unlocked_list.push_back(std::move(e));
    } else {
      if (ev.style == 3 && ev.popularity_rank_req > 0) {
        e.reason = "REACH POPULARITY RANK " + std::to_string(ev.popularity_rank_req);
      } else if (ev.prereq_event_id > 0) {
        e.reason = "WIN PREREQUISITE EVENT FIRST";
      } else if (ev.style == 6) {
        e.reason = "GOLD WRISTBAND & " + thousands(ev.unlock_points_req) + " PTS";
      } else if (ev.level > collected_wristband_ && ev.level >= 0 &&
                 size_t(ev.level) < catalog_->wristbands.size()) {
        std::string wb = catalog_->wristbands[size_t(ev.level)].colour_key;
        for (char& c : wb) c = char(std::toupper(static_cast<unsigned char>(c)));
        e.reason = "COLLECT " + wb + " WRISTBAND AT RACE CENTRAL";
      } else {
        e.reason = "NEEDS " + thousands(ev.unlock_points_req) + " WRISTBAND PTS";
      }
      locked_list.push_back(std::move(e));
    }
  }
  // Put uncompleted unlocked events first, then completed unlocked, then upcoming locked events (capped so list stays crisp).
  std::stable_sort(unlocked_list.begin(), unlocked_list.end(), [](const Entry& a, const Entry& b) {
    const bool ac = a.best_place > 0, bc = b.best_place > 0;
    if (ac != bc) return !ac;
    return a.event->level < b.event->level;
  });
  for (auto& e : unlocked_list) entries_.push_back(std::move(e));
  const size_t lock_cap = std::min<size_t>(locked_list.size(), 8);
  for (size_t i = 0; i < lock_cap; ++i) entries_.push_back(std::move(locked_list[i]));
  if (entries_.empty()) selected_ = 0;
  else selected_ = std::min(selected_, entries_.size() - 1);
}

void EventsPage::move(int by) {
  if (entries_.empty()) return;
  const int n = int(entries_.size());
  selected_ = size_t(((int(selected_) + by) % n + n) % n);
}

void EventsPage::switch_tab(int by) {
  if (hub_filter_ > 0) return;
  const int t = ((int(tab_) + by) % kTabCount + kTabCount) % kTabCount;
  tab_ = Tab(t);
  selected_ = 0;
  rebuild();
}

const EventsPage::Entry* EventsPage::selected_entry() const {
  if (entries_.empty() || selected_ >= entries_.size()) return nullptr;
  return &entries_[selected_];
}

void EventsPage::fill(std::string& title, std::vector<std::string>& lines, int& selected, std::string& detail_title,
                      std::vector<std::string>& detail_lines) const {
  lines.clear();
  detail_title.clear();
  detail_lines.clear();
  if (!open_) return;

  if (hub_filter_ > 0) {
    title = "STREET RACE HUB " + std::to_string(hub_filter_) + ": ENTER TO RACE   ESC BACK";
  } else {
    title = std::string("RACE CENTRAL - ") + tab_name(tab_) + ": LEFT/RIGHT FILTER   ENTER RACE   ESC BACK";
  }

  if (entries_.empty()) {
    lines.push_back("NO EVENTS AVAILABLE YET");
    selected = 0;
    return;
  }

  constexpr int kRows = 7;
  const int n = int(entries_.size());
  const int first = std::clamp(int(selected_) - kRows / 2, 0, std::max(0, n - kRows));
  for (int i = first; i < std::min(n, first + kRows); ++i) {
    const Entry& e = entries_[size_t(i)];
    if (e.is_collect_wristband) {
      lines.push_back("* " + e.display_name);
      continue;
    }
    std::string tag;
    if (!e.unlocked) tag = "[LOCKED] ";
    else if (e.best_place == 1) tag = "[1ST] ";
    else if (e.best_place > 0) tag = "[" + std::to_string(e.best_place) + place_suffix(e.best_place) + "] ";
    else tag = "[NEW] ";
    lines.push_back(tag + e.display_name);
  }
  selected = int(selected_) - first;

  const Entry& sel = entries_[selected_];
  if (sel.is_collect_wristband && catalog_ && sel.wristband_index >= 0 &&
      size_t(sel.wristband_index) < catalog_->wristbands.size()) {
    const auto& wb = catalog_->wristbands[size_t(sel.wristband_index)];
    detail_title = "RACE CENTRAL WRISTBAND CEREMONY";
    detail_lines.push_back("YOU HAVE EARNED ENOUGH POINTS FOR THE");
    detail_lines.push_back(wb.colour_key + " WRISTBAND!");
    detail_lines.push_back("PRESS ENTER / [A] TO COLLECT IT NOW");
    detail_lines.push_back("AND UNLOCK NEW FESTIVAL EVENTS.");
    if (wb.reward_car_id > 0) {
      if (const auto* cs = catalog_->find_car(wb.reward_car_id))
        detail_lines.push_back("BONUS CAR: " + car_name(cs->media_name));
    }
    return;
  }

  if (sel.event) {
    const auto& ev = *sel.event;
    detail_title = race::CareerCatalog::event_style_label(ev);
    std::string cls = "ANY CLASS";
    if (ev.target_class > 0 && size_t(ev.target_class) < class_names_.size())
      cls = "CLASS " + class_names_[size_t(ev.target_class)] + " OR LOWER";
    detail_lines.push_back("EVENT: " + sel.display_name);
    detail_lines.push_back("PRIZE: " + thousands(int64_t(ev.cash_prize)) + " Cr   OPPONENTS: " +
                           std::to_string(ev.opponents));
    if (ev.player_car_id > 0) {
      const auto* cs = catalog_->find_car(ev.player_car_id);
      detail_lines.push_back("SHOWCASE CAR PROVIDED: " + (cs ? car_name(cs->media_name) : std::string("SPECIAL")));
    } else {
      detail_lines.push_back("CLASS LIMIT: " + cls);
    }
    if (!ev.restriction_desc_ref.empty()) {
      const std::string rd = game_text(ev.restriction_desc_ref);
      if (!rd.empty() && rd.rfind("_&", 0) != 0) detail_lines.push_back("RULE: " + rd);
    }
    if (ev.prize_car_id > 0) {
      if (const auto* cs = catalog_->find_car(ev.prize_car_id))
        detail_lines.push_back("1ST PLACE PRIZE CAR: " + car_name(cs->media_name));
    }
    if (sel.best_place > 0)
      detail_lines.push_back("BEST FINISH: " + std::to_string(sel.best_place) + place_suffix(sel.best_place));
    if (!sel.unlocked) detail_lines.push_back("LOCKED: " + sel.reason);
    else if (!sel.eligible) detail_lines.push_back("INELIGIBLE: " + sel.reason);
    else detail_lines.push_back("READY TO RACE - PRESS ENTER / [A]");
  }
}

WorldTrigger select_world_trigger(const race::CareerCatalog& catalog, const race::CareerRecord& career,
                                  int collected_wristband, int popularity_rank, double x, double z, double max_dist) {
  WorldTrigger best;
  auto consider = [&](WorldTrigger::Kind k, const std::array<double, 3>& pos, double radius,
                      const race::CareerEvent* ev, const race::StreetHub* hub, const race::FestivalSite* site) {
    const double d = std::hypot(pos[0] - x, pos[2] - z);
    if (d > max_dist) return;
    const bool inside = d <= radius;
    if (best.kind == WorldTrigger::kNone || (inside && !best.in_range) ||
        (inside == best.in_range && d < best.dist)) {
      best = {k, ev, hub, site, inside, d};
    }
  };
  for (const auto& site : catalog.sites)
    consider(WorldTrigger::kFestivalSite, site.pos, site.radius, nullptr, nullptr, &site);
  for (const auto& hub : catalog.hubs)
    if (hub.has_trigger && catalog.is_hub_unlocked(hub, career, collected_wristband))
      consider(WorldTrigger::kStreetHub, hub.trigger_pos, hub.trigger_radius, nullptr, &hub, nullptr);
  for (const auto& ev : catalog.events)
    if (ev.has_trigger && catalog.is_event_unlocked(ev, career, collected_wristband, popularity_rank))
      consider(WorldTrigger::kEvent, ev.trigger_pos, ev.trigger_radius, &ev, nullptr, nullptr);
  return best;
}

}  // namespace carview
