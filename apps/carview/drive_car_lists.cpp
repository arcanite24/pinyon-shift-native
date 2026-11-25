// The session's car lists on the original's 108_C_BUY_CARSELECT
// (car_list_screen.h): the Autoshow's car select and the garage.
#include "drive_session.h"

#include <cmath>
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

std::string DriveSession::car_model(int64_t car_id) {
  if (!car_models_built) {
    car_models_built = true;
    if (app.shared_db) {
      app.shared_db->each(
          "SELECT Id, COALESCE(ModelShort, '') FROM Data_Car", {},
          [&](sqlite3_stmt* st) {
            const auto* ref = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            if (!ref || !*ref) return;
            const std::string text = carview::game_text(ref);
            if (!text.empty() && text[0] != '_' && text[0] != '&') car_models[sqlite3_column_int64(st, 0)] = text;
          },
          nullptr);
    }
  }
  const auto it = car_models.find(car_id);
  return it == car_models.end() ? std::string{} : it->second;
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
    d.model = car_model(s->dealer.id);
    if (d.model.empty()) d.model = model_of(carview::car_name(s->dealer.media_name), s->make_name);
    d.year_make = std::to_string(s->dealer.year) + " " + s->make_name;
    const auto& bands = app.class_bands;
    if (!bands.empty()) {
      const int ci = garage::class_of(s->dealer.performance_index, bands);
      if (ci >= 0 && ci < int(bands.size())) d.car_class = bands[size_t(ci)].name;
      d.pi = garage::shown_index(s->dealer.performance_index, bands);
    }
    d.drive_type = s->drive_type_id;
    if (app.have_career_catalog)
      if (const auto* spec = app.career_catalog.find_car(s->dealer.id)) {
        d.engine_placement = spec->engine_placement;
        d.flag = country_flag(int(spec->props.country));
      }
    d.stats = {s->speed_rating, s->handling_rating, s->accel_rating, s->launch_rating, s->braking_rating};
    if (!autoshow.show_specs()) d.valid = false;  // Y TOGGLE hides the panel
  }
  out.prompts = {{"A", carview::text_of(t::kSelect)},
                 {"B", carview::text_of(t::kBack)},
                 {"Y", carview::text_of(t::kToggle)},
                 {"BACK", carview::text_of(t::kSearch)}};
}

}  // namespace carview
