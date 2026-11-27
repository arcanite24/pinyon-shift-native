// The session's car lists on the original's 108_C_BUY_CARSELECT
// (car_list_screen.h): the Autoshow's car select, the garage (MY GARAGE)
// and its Barn Finds.
#include "drive_session.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "garage/garage.h"

namespace carview {

const std::string& DriveSession::country_flag(int country_id) {
  if (!country_flags_built) {
    country_flags_built = true;
    if (app.shared_db) {
      app.shared_db->each(
          "SELECT CountryID, COALESCE(IconPath, '') FROM List_Country", {},
          [&](sqlite3_stmt* st) {
            const int id = int(sqlite3_column_int64(st, 0));
            const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            if (path && *path) country_flags[id] = carview::ui_texture_key(path);
          },
          nullptr);
    }
  }
  static const std::string kNone;
  const auto it = country_flags.find(country_id);
  return it == country_flags.end() ? kNone : it->second;
}

namespace {

std::string col(sqlite3_stmt* st, int i) {
  const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
  return t ? std::string(t) : std::string{};
}

// A string-table reference ("_&857938644") as its text; empty when the
// tables have none.
std::string table_text(const std::string& ref) {
  if (ref.empty()) return {};
  const std::string text = carview::game_text(ref);
  return (text.empty() || text[0] == '_' || text[0] == '&') ? std::string{} : text;
}

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

const DriveSession::CarListInfo* DriveSession::car_list_info(int64_t car_id) {
  if (!car_list_infos_built) {
    car_list_infos_built = true;
    if (app.shared_db) {
      app.shared_db->each(
          "SELECT c.Id, COALESCE(c.ModelShort, ''), c.Year, COALESCE(m.DisplayName, ''), "
          "COALESCE(m.IconPathBase, ''), c.CountryID, c.EnginePlacementID, c.DriveTypeID, c.PerformanceIndex, "
          "c.SpeedRating, c.HandlingRating, c.AccelerationRating, c.LaunchRating, c.BrakingRating "
          "FROM Data_Car c LEFT JOIN List_CarMake m ON m.ID = c.MakeID",
          {},
          [&](sqlite3_stmt* st) {
            CarListInfo ci;
            ci.model = table_text(col(st, 1));
            ci.year = sqlite3_column_int(st, 2);
            ci.make = table_text(col(st, 3));
            if (ci.make.empty()) ci.make = col(st, 4);
            ci.country = sqlite3_column_int(st, 5);
            ci.placement = sqlite3_column_int(st, 6);
            ci.drive_type = sqlite3_column_int(st, 7);
            ci.raw_pi = sqlite3_column_double(st, 8);
            for (int k = 0; k < 5; ++k) ci.stats[size_t(k)] = sqlite3_column_double(st, 9 + k);
            car_list_infos[sqlite3_column_int64(st, 0)] = std::move(ci);
          },
          nullptr);
    }
  }
  const auto it = car_list_infos.find(car_id);
  return it == car_list_infos.end() ? nullptr : &it->second;
}

carview::CarListDetails DriveSession::car_list_details(int64_t car_id, double raw_pi) {
  carview::CarListDetails d;
  const CarListInfo* ci = car_list_info(car_id);
  if (!ci) return d;
  d.valid = true;
  d.car_id = car_id;
  d.model = ci->model;
  d.year_make = std::to_string(ci->year) + " " + ci->make;
  d.flag = country_flag(ci->country);
  d.engine_placement = ci->placement;
  d.drive_type = ci->drive_type;
  d.stats = ci->stats;
  const auto& bands = app.class_bands;
  if (!bands.empty()) {
    const double pi = raw_pi >= 0 ? raw_pi : ci->raw_pi;
    const int cl = garage::class_of(pi, bands);
    if (cl >= 0 && cl < int(bands.size())) d.car_class = bands[size_t(cl)].name;
    d.pi = garage::shown_index(pi, bands);
  }
  return d;
}

namespace {

// "Jaguar XKR-S" -> "XKR-S": the panel's title is the model, its make on the
// line below.
std::string model_of(const std::string& display, const std::string& make) {
  if (!make.empty() && display.size() > make.size() + 1 && display.compare(0, make.size(), make) == 0 &&
      display[make.size()] == ' ')
    return display.substr(make.size() + 1);
  return display;
}

}  // namespace

void DriveSession::fill_autoshow_list(carview::CarListHud& out) {
  namespace t = carview::car_list_text;
  out = {};
  // The first car's list is the title's HORIZON RECOMMENDS (19_autoshow_orig.png);
  // a make's list is CAR SELECT.
  out.title = carview::text_of(autoshow.tutorial_mode() ? t::kHorizonRecommends : t::kCarSelect);
  for (const auto& c : autoshow.visible_cars()) {
    carview::CarTile tile;
    tile.car_id = c.dealer.id;
    tile.cursor = carview::car_list_credits(c.dealer.base_cost);
    out.tiles.push_back(std::move(tile));
  }
  out.selected = int(autoshow.selected_index());
  if (const carview::AutoshowCar* s = autoshow.selected_car()) {
    carview::CarListDetails& d = out.details;
    d.valid = true;
    d.car_id = s->dealer.id;
    const CarListInfo* ci = car_list_info(s->dealer.id);
    d.model = ci ? ci->model : std::string{};
    if (d.model.empty()) d.model = model_of(carview::car_name(s->dealer.media_name), s->make_name);
    d.year_make = std::to_string(s->dealer.year) + " " + s->make_name;
    const auto& bands = app.class_bands;
    if (!bands.empty()) {
      const int ci_ = garage::class_of(s->dealer.performance_index, bands);
      if (ci_ >= 0 && ci_ < int(bands.size())) d.car_class = bands[size_t(ci_)].name;
      d.pi = garage::shown_index(s->dealer.performance_index, bands);
    }
    d.drive_type = s->drive_type_id;
    if (ci) {
      d.engine_placement = ci->placement;
      d.flag = country_flag(ci->country);
    }
    d.stats = {s->speed_rating, s->handling_rating, s->accel_rating, s->launch_rating, s->braking_rating};
    if (!autoshow.show_specs()) d.valid = false;  // Y TOGGLE hides the panel
  }
  if (autoshow.tutorial_mode()) {
    // Retail first-car Autoshow (HORIZON RECOMMENDS) hides BACK and SEARCH prompts.
    out.prompts = {{"A", carview::text_of(t::kSelect)},
                   {"Y", carview::text_of(t::kToggle)}};
  } else {
    out.prompts = {{"A", carview::text_of(t::kSelect)},
                   {"B", carview::text_of(t::kBack)},
                   {"Y", carview::text_of(t::kToggle)},
                   {"BACK", carview::text_of(t::kSearch)}};
  }
}

const DriveSession::MakeInfo* DriveSession::make_info(int64_t make_id) {
  if (!make_infos_built) {
    make_infos_built = true;
    if (app.shared_db) {
      app.shared_db->each(
          "SELECT ID, COALESCE(IconPath, ''), COALESCE(Profile, '') FROM List_CarMake", {},
          [&](sqlite3_stmt* st) {
            MakeInfo mi;
            mi.logo = carview::ui_texture_key(col(st, 1));
            mi.profile = table_text(col(st, 2));
            // The disc's profiles are all "N/A" (List_CarMake.str).
            if (mi.profile == "N/A") mi.profile.clear();
            make_infos[sqlite3_column_int64(st, 0)] = std::move(mi);
          },
          nullptr);
    }
  }
  const auto it = make_infos.find(make_id);
  return it == make_infos.end() ? nullptr : &it->second;
}

void DriveSession::fill_make_list(carview::MakeListHud& out) {
  namespace t = carview::car_list_text;
  out = {};
  out.title = carview::text_of(t::kManufacturerSelect);
  // The strip holds the makes; after Y (All Affordable) the cursor returns
  // to the make it left.
  const std::vector<size_t> strip = autoshow.strip_makes();
  const auto& makes = autoshow.makes();
  if (std::find(strip.begin(), strip.end(), autoshow.selected_make_index()) == strip.end())
    autoshow.move_strip_make(0, app.career.credits);
  for (size_t k = 0; k < strip.size(); ++k) {
    const carview::AutoshowMake& mk = makes[strip[k]];
    carview::MakeTile tile;
    tile.id = int(strip[k]);
    if (const MakeInfo* mi = mk.make_id >= 0 ? make_info(mk.make_id) : nullptr) tile.logo = mi->logo;
    tile.label = mk.name;
    if (strip[k] == autoshow.selected_make_index()) {
      out.selected = int(k);
      out.cursor = upper(mk.name);
      out.available = carview::format1(carview::text_of(t::kAvailable), std::to_string(mk.car_count));
      if (const MakeInfo* mi = mk.make_id >= 0 ? make_info(mk.make_id) : nullptr; mi && !mi->profile.empty()) {
        out.info_title = out.cursor;
        out.info_text = mi->profile;
      }
    }
    out.tiles.push_back(std::move(tile));
  }
  out.prompts = {{"A", carview::text_of(t::kSelect)},
                 {"B", carview::text_of(t::kBack)},
                 {"Y", carview::text_of(t::kAffordableCars)}};
  if (make_none_affordable)
    out.popup = {true, carview::text_of(t::kNoneAffordableTitle), carview::text_of(t::kNoneAffordableMessage),
                 {{"A", carview::text_of(t::kOk)}}};
}

bool DriveSession::is_barn_car(int64_t car_id) const {
  for (const auto& b : app.collectibles.barns)
    if (b.car_id == car_id) return true;
  return false;
}

void DriveSession::fill_garage_list(carview::CarListHud& out) {
  namespace t = carview::car_list_text;
  out = {};
  if (garage_barn_finds) {
    // Every barn, its state on the tile and the cursor (Barnfind.str's
    // IDS_CarState_*); an undiscovered barn shows no car.
    out.title = carview::text_of(t::kBarnFinds);
    const auto& barns = app.collectibles.barns;
    for (size_t bi = 0; bi < barns.size(); ++bi) {
      const int status = bi < app.progress.barns.size() ? app.progress.barns[bi].status : freeroam::kBarnNotSpawned;
      carview::CarTile tile;
      tile.car_id = barns[bi].car_id;
      switch (status) {
        case freeroam::kBarnReady:
          tile.state = carview::CarTileState::kReady;
          tile.cursor = carview::text_of(t::kBarnReady);
          break;
        case freeroam::kBarnRestoring:
          tile.state = carview::CarTileState::kNotReady;
          tile.cursor = carview::text_of(t::kBarnWrecked);
          break;
        case freeroam::kBarnCollected:
          tile.cursor = carview::text_of(t::kBarnCollected);
          break;
        default:
          tile.state = carview::CarTileState::kUndiscovered;
          tile.cursor = carview::text_of(t::kBarnLocked);
          break;
      }
      out.tiles.push_back(std::move(tile));
    }
    if (!barns.empty()) barn_pick = std::min(barn_pick, barns.size() - 1);
    out.selected = int(barn_pick);
    if (!barns.empty() && garage_show_specs &&
        out.tiles[barn_pick].state != carview::CarTileState::kUndiscovered)
      out.details = car_list_details(barns[barn_pick].car_id);
    out.prompts = {{"A", carview::text_of(t::kSelect)},
                   {"B", carview::text_of(t::kBack)},
                   {"Y", carview::text_of(t::kToggle)}};
  } else {
    // Retail My Garage / Outpost Change Car title reads CAR SELECT (outpost-a1.png).
    out.title = carview::text_of(t::kCarSelect);
    // Tighten outdoor Car Select close flank orbit camera toward retail Q365 framing
    // (dist = 1.67..2.40 m, dy = 0.37..0.56 m, fov = 48.14 deg).
    const bool indoor_hub =
        hub_menu.open() &&
        (hub_menu.top() == carview::HubMenu::kWorkshop || hub_menu.top() == carview::HubMenu::kPaintBooth);
    if (!indoor_hub && !hub_cam.user_orbited && !hub_cam.sequence &&
        std::fabs(hub_cam.fov_deg - 48.14f) < 0.25f && hub_cam.target_dist > 2.45f) {
      hub_cam.set_outdoor_car_select();
    }
    const auto& vis = garage_browser.visible();
    for (const auto& e : vis) {
      carview::CarTile tile;
      tile.car_id = e.car_id;
      out.tiles.push_back(std::move(tile));
    }
    out.selected = int(garage_browser.selected_index());
    if (const GarageEntry* e = garage_browser.selected_entry(); e && garage_show_specs) {
      out.details = car_list_details(e->car_id, e->raw_pi);
      if (out.details.valid) {
        // The car as it stands (upgrades change its ratings and index).
        out.details.stats = {e->speed_rating, e->handling_rating, e->accel_rating, e->launch_rating,
                             e->braking_rating};
        if (out.details.model.empty())
          out.details.model = model_of(carview::car_name(e->media_name), e->make_name);
      }
    }
    // outpost-a1.png / 11_car_select_orig.png bar on 108_C_BUY_CARSELECT:
    // A SELECT, B BACK, X CAR LIST, Y TOGGLE, BACK SEARCH.
    out.prompts = {{"A", carview::text_of(t::kSelect)},
                   {"B", carview::text_of(t::kBack)},
                   {"X", carview::text_of(t::kCarList)},
                   {"Y", carview::text_of(t::kToggle)},
                   {"BACK", carview::text_of(t::kSearch)}};
  }
  // The popup over the list.
  auto& p = out.popup;
  const carview::StringRef ok = t::kOk;
  switch (garage_popup) {
    case GaragePopup::kCollect:
      p = {true, carview::text_of(t::kCollectTitle), carview::text_of(t::kCollectMessage),
           {{"A", carview::text_of(ok)}, {"B", carview::text_of(t::kBack)}}};
      break;
    case GaragePopup::kNotReady:
      p = {true, carview::text_of(t::kWreckedTitle), carview::text_of(t::kWreckedMessage),
           {{"A", carview::text_of(ok)}, {"X", carview::text_of(t::kViewOnMap)}}};
      break;
    case GaragePopup::kNotDiscovered: {
      p = {true, carview::text_of(t::kLockedTitle), carview::text_of(t::kLockedMessage),
           {{"A", carview::text_of(ok)}}};
      // A rumoured barn's circle is on the map.
      const size_t bi = barn_pick;
      if (bi < app.progress.barns.size() && app.progress.barns[bi].status == freeroam::kBarnRumour)
        p.prompts.push_back({"X", carview::text_of(t::kViewOnMap)});
      break;
    }
    case GaragePopup::kCantSell:
      p = {true, carview::text_of(t::kCantSellTitle), carview::text_of(t::kCantSellMessage),
           {{"A", carview::text_of(ok)}}};
      break;
    case GaragePopup::kNone: break;
  }
}

void DriveSession::barn_select() {
  const auto& barns = app.collectibles.barns;
  if (barns.empty()) return;
  barn_pick = std::min(barn_pick, barns.size() - 1);
  const int status =
      barn_pick < app.progress.barns.size() ? app.progress.barns[barn_pick].status : freeroam::kBarnNotSpawned;
  switch (status) {
    case freeroam::kBarnReady: garage_popup = GaragePopup::kCollect; break;
    case freeroam::kBarnRestoring: garage_popup = GaragePopup::kNotReady; break;
    case freeroam::kBarnCollected:
      // Already in the garage.
      app.audio_host.trigger_ui(audio::UiCue::kCancel);
      return;
    default: garage_popup = GaragePopup::kNotDiscovered; break;
  }
  app.audio_host.trigger_ui(audio::UiCue::kAccept);
}

void DriveSession::garage_popup_accept() {
  const GaragePopup was = garage_popup;
  garage_popup = GaragePopup::kNone;
  if (was != GaragePopup::kCollect) {
    app.audio_host.trigger_ui(audio::UiCue::kAccept);
    return;
  }
  // COLLECT CAR: "This Barn Find will now be added to your garage."
  const size_t bi = barn_pick;
  if (bi >= app.collectibles.barns.size()) return;
  const auto& b = app.collectibles.barns[bi];
  const bool collected = host.free_roam ? host.free_roam->collect_barn(bi)
                                        : freeroam::collect_barn(app.collectibles, app.progress, bi);
  if (!collected) {
    app.audio_host.trigger_ui(audio::UiCue::kCancel);
    return;
  }
  const std::string named = app.award_garage_car(b.car_id, 0, false, /*is_prize=*/true);
  app.audio_host.trigger_ui(audio::UiCue::kCarAdded);
  std::printf("barn finds: %s collected at the garage (Data_Car %d)\n", b.name.c_str(), b.car_id);
  (void)named;
  if (app.have_garage && !app.career_garage.cars.empty()) {
    const cars::PartsDb* gdb =
        (app.parts_db.valid() && app.parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            ? app.parts_db.get().get()
            : nullptr;
    garage_browser.refresh(app.career_garage, gdb, app.class_bands);
  }
  save_now("barn find collected");
}

void DriveSession::garage_popup_view_map() {
  const size_t bi = barn_pick;
  if (bi >= app.collectibles.barns.size()) return;
  const int status = bi < app.progress.barns.size() ? app.progress.barns[bi].status : freeroam::kBarnNotSpawned;
  const auto& b = app.collectibles.barns[bi];
  double x = 0, z = 0;
  if (status == freeroam::kBarnRumour) {
    x = b.hint_x, z = b.hint_z;  // the rumour's circle
  } else if (status == freeroam::kBarnRestoring) {
    x = b.at.x, z = b.at.z;  // the barn itself
  } else {
    return;
  }
  garage_popup = GaragePopup::kNone;
  garage_open = false;
  garage_barn_finds = false;
  garage_from = GarageFrom::kNone;
  hub_menu.close();  // the map replaces the workshop's list
  app.audio_host.trigger_ui(audio::UiCue::kAccept);
  open_map();
  if (!map_open) return;
  map_cam.reset(x, z, map_zoom);
  map_cx = map_cam.cx();
  map_cz = map_cam.cz();
  map_zoom = map_cam.zoom();
  map_dirty = true;
  update_map_selection();
}

}  // namespace carview
