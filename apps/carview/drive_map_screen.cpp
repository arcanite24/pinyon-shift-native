// The scene-driven pause map's session glue (docs/UI_SCENE_SCREENS.md, "The
// map"): the free cursor and zoom, the icon under the cursor and its popup,
// the help bar, the FILTER popup and fast travel with its dialogs.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

#include "drive_session.h"
#include "freeroam/fast_travel.h"

namespace carview {
namespace {

// The cursor locks onto an icon within this many canvas px (the original's
// lock-on ring, 90 px across, in map_popup_event_orig.png).
constexpr double kSnapPx = 30.0;

double now_s() { return double(SDL_GetTicks()) / 1000.0; }

// The profile's name, as the original shows its gamertag over the player
// (race_view.cpp's rule): SHIFT_PLAYER_NAME, else the OS account, else User.
const std::string& player_name() {
  static const std::string me = [] {
    for (const char* v : {"SHIFT_PLAYER_NAME", "USERNAME", "USER"})
      if (const char* e = std::getenv(v); e && *e) return std::string(e).substr(0, 15);
    return std::string("User");
  }();
  return me;
}

const freeroam::FastTravelTunables& tunables(const DriveApp& app) {
  static const freeroam::FastTravelTunables t = [&] {
    freeroam::FastTravelTunables out;
    freeroam::load_fast_travel_tunables(app.game_root_of(), out, nullptr);
    return out;
  }();
  return t;
}

// The fast travel tag of a marker (MapProfile activity_type).
std::string travel_tag(const MapMarker& m) {
  if (m.kind == MapMarker::kOutpost) return "gas_station";
  if (m.kind == MapMarker::kFestivalSite) return m.icon_tag;
  return {};
}

}  // namespace

void DriveSession::map_cursor_world() {
  if (map_w <= 64 || map_h <= 64) {
    map_px = map_cx, map_pz = map_cz;
    return;
  }
  const SceneFrame f = SceneScreen::frame(float(map_w), float(map_h));
  const double mpp = app.map_view.map_scale(map_w, map_h, map_zoom);
  map_px = map_cx + (double(f.x(map_cursor.x)) - 0.5 * map_w) * mpp;
  map_pz = map_cz - (double(f.y(map_cursor.y)) - 0.5 * map_h) * mpp;
}

void DriveSession::map_screen_open() {
  map_cursor.reset();
  map_opened_at = now_s();
  map_base_zoom = map_zoom;
  map_filter_open = false;
  map_filter_row = 0;
  map_dialog = MapDialog::kNone;
  map_cursor_world();
}

void DriveSession::map_screen_step(double dt, float sx, float sy, float zoom_axis) {
  if (map_filter_open || map_dialog != MapDialog::kNone || map_w <= 64 || map_h <= 64) return;
  const SceneFrame f = SceneScreen::frame(float(map_w), float(map_h));
  bool changed = false;
  if (std::fabs(sx) > 1e-4f || std::fabs(sy) > 1e-4f) {
    const auto scroll = map_cursor.step(dt, sx, sy, map_screen_hud.locked);
    changed = true;
    if (scroll[0] != 0.0f || scroll[1] != 0.0f) {
      const double mpp = app.map_view.map_scale(map_w, map_h, map_zoom);
      const double px_m = double(f.s) * mpp;
      double ncx = map_cx + scroll[0] * px_m, ncz = map_cz - scroll[1] * px_m;
      app.map_view.clamp_camera(map_w, map_h, map_zoom, ncx, ncz);
      // What the view could not take moves the cursor on to the edge.
      map_cursor.absorb(scroll[0] - float((ncx - map_cx) / px_m), scroll[1] + float((ncz - map_cz) / px_m));
      if (ncx != map_cx || ncz != map_cz) {
        map_cx = ncx, map_cz = ncz;
        map_dirty = true;
      }
    }
  }
  if (std::fabs(zoom_axis) > 1e-4f) {
    const double nz = MapZoom::step(map_zoom, map_base_zoom, zoom_axis, dt);
    if (nz != map_zoom) {
      // About the cursor: the world point under it stays put.
      map_cursor_world();
      const double px = map_px, pz = map_pz;
      map_zoom = nz;
      const double mpp = app.map_view.map_scale(map_w, map_h, map_zoom);
      map_cx = px - (double(f.x(map_cursor.x)) - 0.5 * map_w) * mpp;
      map_cz = pz + (double(f.y(map_cursor.y)) - 0.5 * map_h) * mpp;
      app.map_view.clamp_camera(map_w, map_h, map_zoom, map_cx, map_cz);
      map_dirty = changed = true;
    }
  }
  if (changed) {
    map_cam.reset(map_cx, map_cz, map_zoom);
    const int was = map_selected_marker;
    update_map_selection();
    // The selected icon grows and the rest dim: a new raster.
    if (map_selected_marker != was) map_dirty = true;
  }
}

void DriveSession::map_screen_fill() {
  MapScreenHud& ms = map_screen_hud;
  ms = MapScreenHud{};
  ms.active = true;
  ms.age = now_s() - map_opened_at;
  ms.cursor = {map_cursor.x, map_cursor.y};
  ms.filter = map_filter;
  ms.filter_open = map_filter_open;
  ms.filter_row = map_filter_row;
  ms.dialog = map_dialog;
  if (map_dialog == MapDialog::kConfirm) {
    ms.dialog_body = map_dialog_cost > 0
                         ? format1(text_of(map_text::kFastTravelAsk), thousands(map_dialog_cost))
                         : text_of(map_text::kFastTravelAskFree);
  } else if (map_dialog == MapDialog::kNoCredits) {
    ms.dialog_body = text_of(map_text::kFastTravelNoCredits);
  }
  if (map_w <= 64 || map_h <= 64) return;
  const SceneFrame f = SceneScreen::frame(float(map_w), float(map_h));
  const double mpp = app.map_view.map_scale(map_w, map_h, map_zoom);
  auto to_canvas = [&](double x, double z) -> std::array<float, 2> {
    const double sx = 0.5 * map_w + (x - map_cx) / mpp, sy = 0.5 * map_h - (z - map_cz) / mpp;
    return {float((sx - f.ox) / f.s), float((sy - f.oy) / f.s)};
  };
  MapPopupHud& p = ms.popup;
  const bool route = app.gps_nav.active();
  std::string x_label;
  if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
    const MapMarker& m = cached_map_markers[size_t(map_selected_marker)];
    const auto c = to_canvas(m.x, m.z);
    p.x = c[0], p.y = c[1];
    ms.locked = true;
    switch (m.kind) {
      case MapMarker::kEvent:
        p.title = m.title;
        p.class_name = m.class_name;
        p.money = m.money;
        if (m.icon_tag == "exhibition") {
          p.kind = MapPopupKind::kExhibition;
        } else if (m.icon_tag == "nemesisrace") {
          p.kind = MapPopupKind::kNemesis;
          p.line = m.money;
        } else {
          p.kind = MapPopupKind::kCareerRace;
          p.points = m.points;
        }
        x_label = text_of(map_text::kDetails);
        break;
      case MapMarker::kStreetHub:
        p.kind = MapPopupKind::kStreetRace;
        p.title = text_of({"Activities", "IDS_Description_StreetRaceHub", 0, "STREET RACE HUB"});
        p.line = m.popup_line;
        for (int v : m.pips) p.pips.push_back(MapPip(v));
        x_label = text_of(map_text::kDetails);
        break;
      case MapMarker::kOutpost:
        p.kind = MapPopupKind::kOutpost;
        p.title = text_of({"Activities", "IDS_Description_GasStation", 0, "HORIZON OUTPOST"});
        p.line = m.popup_line;
        for (int v : m.pips) p.pips.push_back(MapPip(v));
        p.discount_label = text_of({"Activities", "IDS_GasStation_Discount", 0, "Discount:"});
        p.discount_value = m.discount;
        break;
      case MapMarker::kFestivalSite:
        p.kind = m.popup_line.empty() ? MapPopupKind::kSimple : MapPopupKind::kGeneric;
        p.title = m.title;
        p.line = m.popup_line;
        break;
      case MapMarker::kSpeedTrap:
      case MapMarker::kSpeedZone:
        p.kind = m.popup_line.empty() ? MapPopupKind::kSimple : MapPopupKind::kGeneric;
        p.title = m.title;
        p.line = m.popup_line;
        // The original's X LEADERBOARD: the engine has no leaderboards, so
        // no prompt.
        break;
      default:
        p.kind = MapPopupKind::kSimple;
        p.title = m.title;
        break;
    }
    // X on a destination: the fee.
    int64_t cost = 0;
    if (map_travel_quote(m, cost))
      x_label = format1(text_of(map_text::kFastTravelButton), format1(text_of(map_text::kCredits), thousands(cost)));
  } else {
    // The player (their name over the arrow, map_open_orig.png), then a
    // found or revealed sign or barn.
    const drive::Vec3 at = vehicle.position();
    const auto pc = to_canvas(at.x, at.z);
    const double snap = kSnapPx;
    auto near = [&](const std::array<float, 2>& c) {
      return std::hypot(double(c[0] - map_cursor.x), double(c[1] - map_cursor.y)) <= snap;
    };
    if (near(pc)) {
      p.kind = MapPopupKind::kSimple;
      p.title = player_name();
      p.x = pc[0], p.y = pc[1];
      ms.locked = true;
    } else if (app.progress_loaded) {
      const bool reveal = app.reveal || app.treasure_map_setting || slice12.trainer.effective_reveal_collectibles();
      if (map_filter.shows(MapCategory::kDiscountSign)) {
        for (size_t i = 0; i < app.collectibles.signs.size() && p.kind == MapPopupKind::kNone; ++i) {
          const auto& sign = app.collectibles.signs[i];
          if (!reveal && !app.progress.signs_found.count(sign.id)) continue;
          const auto c = to_canvas(sign.at.x, sign.at.z);
          if (!near(c)) continue;
          p.kind = MapPopupKind::kSimple;
          p.title = text_of({"Activities", "IDS_Flyer", 0, "DISCOUNT SIGN"}) + " " + std::to_string(i + 1);
          p.x = c[0], p.y = c[1];
          ms.locked = true;
        }
      }
      if (p.kind == MapPopupKind::kNone && map_filter.shows(MapCategory::kBarnFind)) {
        for (size_t i = 0; i < app.collectibles.barns.size() && p.kind == MapPopupKind::kNone; ++i) {
          const auto& b = app.collectibles.barns[i];
          const int st = i < app.progress.barns.size() ? app.progress.barns[i].status : freeroam::kBarnNotSpawned;
          if (st != freeroam::kBarnFound && !reveal) continue;
          const auto c = to_canvas(b.at.x, b.at.z);
          if (!near(c)) continue;
          p.kind = MapPopupKind::kSimple;
          p.title = text_of({"Activities", "IDS_Description_BarnFind", 0, "BARN FIND"});
          p.x = c[0], p.y = c[1];
          ms.locked = true;
        }
      }
    }
  }
  // The help bar (map_popup_outpost_orig.png): A route, B exit, X the
  // icon's action, LB the filter.
  ms.prompts.push_back({"A", text_of(route ? map_text::kClearRoute : map_text::kSetRoute)});
  ms.prompts.push_back({"B", text_of(map_text::kExitMap)});
  if (x_label.empty() && map_selected_marker < 0 && !app.race && slice12.trainer.effective_fast_travel_anywhere())
    x_label = format1(text_of(map_text::kFastTravelButton), format1(text_of(map_text::kCredits), thousands(0)));
  if (!x_label.empty()) ms.prompts.push_back({"X", x_label});
  ms.prompts.push_back({"LB", text_of(map_text::kFilter)});
  if (const int pending = app.have_career_catalog
                              ? app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband)
                              : -1;
      pending >= 0 && size_t(pending) < app.career_catalog.wristbands.size()) {
    std::string colour = app.career_catalog.wristbands[size_t(pending)].colour_key;
    for (char& ch : colour) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    ms.band_texture = "WRISTBANDS/" + colour;
  }
}

void DriveSession::map_travel_to(double x, double z, double heading) {
  close_map();
  app.spawn[0] = x;
  app.spawn[1] = 0.0;
  app.spawn[2] = z;
  app.spawn[3] = heading;
  app.have_spawn = true;
  app.fast_travel = true;
  app.next_car = app.car;
  running = false;
}

bool DriveSession::map_travel_quote(const MapMarker& m, int64_t& cost) {
  // The engine's trainer "fast travel anywhere" makes every destination,
  // found or not, free.
  const bool anywhere = slice12.trainer.effective_fast_travel_anywhere();
  const std::string tag = travel_tag(m);
  if (tag.empty() || app.race || (m.kind == MapMarker::kOutpost && !m.discovered && !anywhere)) return false;
  if (anywhere) {
    cost = 0;
    return true;
  }
  freeroam::OutpostMissions om;
  if (m.kind == MapMarker::kOutpost && app.have_outposts)
    om = freeroam::outpost_missions(app.outposts, app.outpost_progress, m.id);
  const auto q = freeroam::quote_fast_travel(tunables(app), tag, om.total, om.done, app.career.credits);
  if (!q.eligible()) return false;
  cost = q.cost;
  return true;
}

void DriveSession::map_fast_travel_request() {
  int64_t cost = 0;
  if (map_selected_marker < 0 || size_t(map_selected_marker) >= cached_map_markers.size()) {
    // The trainer also travels to the bare cursor.
    if (app.race || !slice12.trainer.effective_fast_travel_anywhere()) return;
  } else if (!map_travel_quote(cached_map_markers[size_t(map_selected_marker)], cost)) {
    return;
  }
  const bool affordable = cost <= 0 || app.career.credits >= cost;
  map_dialog_marker = map_selected_marker;
  map_dialog_px = map_px, map_dialog_pz = map_pz;
  map_dialog_cost = cost;
  map_dialog = affordable ? MapDialog::kConfirm : MapDialog::kNoCredits;
  app.audio_host.trigger_ui(affordable ? audio::UiCue::kAccept : audio::UiCue::kError);
}

void DriveSession::map_fast_travel_confirm() {
  map_dialog = MapDialog::kNone;
  if (map_dialog_marker < 0) {
    if (!slice12.trainer.effective_fast_travel_anywhere()) return;
    app.audio_host.trigger_ui(audio::UiCue::kAccept);
    map_travel_to(map_dialog_px, map_dialog_pz, 0.0);
    return;
  }
  if (size_t(map_dialog_marker) >= cached_map_markers.size()) return;
  const MapMarker m = cached_map_markers[size_t(map_dialog_marker)];
  if (map_dialog_cost > 0) {
    if (app.career.credits < map_dialog_cost) return;
    app.career.credits -= map_dialog_cost;
  }
  // The original saves the profile before it travels.
  save_now("fast travel");
  app.audio_host.trigger_ui(audio::UiCue::kAccept);
  if (m.kind == MapMarker::kOutpost && app.have_outposts) {
    if (const auto* op = app.outposts.find_outpost(m.id)) {
      map_travel_to(op->spawn.x, op->spawn.z, std::atan2(op->spawn.z_axis[0], op->spawn.z_axis[2]));
      return;
    }
  }
  map_travel_to(m.x, m.z, 0.0);
}

bool DriveSession::map_screen_event(const SDL_Event& e) {
  enum class Key { kNone, kA, kB, kX, kLB, kUp, kDown };
  Key k = Key::kNone;
  if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
    switch (e.gbutton.button) {
      case SDL_GAMEPAD_BUTTON_SOUTH: k = Key::kA; break;
      case SDL_GAMEPAD_BUTTON_EAST:
      case SDL_GAMEPAD_BUTTON_BACK:
      case SDL_GAMEPAD_BUTTON_START: k = Key::kB; break;
      case SDL_GAMEPAD_BUTTON_WEST: k = Key::kX; break;
      case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: k = Key::kLB; break;
      case SDL_GAMEPAD_BUTTON_DPAD_UP: k = Key::kUp; break;
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN: k = Key::kDown; break;
      default: break;
    }
  } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
    switch (e.key.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_SPACE: k = Key::kA; break;
      case SDLK_ESCAPE:
      case SDLK_M:
      case SDLK_BACKSPACE: k = Key::kB; break;
      case SDLK_F: k = Key::kX; break;
      case SDLK_Q: k = Key::kLB; break;
      case SDLK_UP:
      case SDLK_W: k = Key::kUp; break;
      case SDLK_DOWN:
      case SDLK_S: k = Key::kDown; break;
      default: break;
    }
  }
  if (map_dialog != MapDialog::kNone) {
    if (k == Key::kA) {
      if (map_dialog == MapDialog::kConfirm) map_fast_travel_confirm();
      else map_dialog = MapDialog::kNone, app.audio_host.trigger_ui(audio::UiCue::kAccept);
    } else if (k == Key::kB && map_dialog == MapDialog::kConfirm) {
      map_dialog = MapDialog::kNone;
      app.audio_host.trigger_ui(audio::UiCue::kCancel);
    }
    map_dirty = true;
    return true;  // the dialog takes every input
  }
  if (map_filter_open) {
    if (k == Key::kUp && map_filter_row > 0) {
      --map_filter_row;
      app.audio_host.trigger_ui(audio::UiCue::kMove);
    } else if (k == Key::kDown && map_filter_row + 1 < kMapFilterRows) {
      ++map_filter_row;
      app.audio_host.trigger_ui(audio::UiCue::kMove);
    } else if (k == Key::kA) {
      map_filter.toggle(MapFilterRow(map_filter_row));
      app.audio_host.trigger_ui(audio::UiCue::kAccept);
    } else if (k == Key::kB || k == Key::kLB) {
      map_filter_open = false;
      app.audio_host.trigger_ui(audio::UiCue::kCancel);
      update_map_selection();
    }
    map_dirty = true;
    return true;
  }
  if (k == Key::kLB) {
    map_filter_open = true;
    map_filter_row = 0;
    map_dirty = true;
    app.audio_host.trigger_ui(audio::UiCue::kAccept);
    return true;
  }
  if (k == Key::kX) {
    map_fast_travel_request();
    if (map_dialog != MapDialog::kNone) {
      map_dirty = true;
      return true;
    }
    // DETAILS: the original shows the event's ticket over the map
    // (map_popup_event_details_orig.png); the engine's events page is the
    // interim.
    if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
      const MapMarker& m = cached_map_markers[size_t(map_selected_marker)];
      if ((m.kind == MapMarker::kEvent || m.kind == MapMarker::kStreetHub) && !app.race) {
        const int hub = m.kind == MapMarker::kStreetHub ? m.site_or_hub : 0;
        close_map();
        open_events_browser(hub);
      }
    }
    return true;
  }
  // The mouse wheel and +/- zoom like the right stick (to 2.6 times, never
  // past the opening scale); the original has no centre-on-car.
  if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0.0f) {
    map_screen_step(0.12, 0.0f, 0.0f, e.wheel.y > 0.0f ? 1.0f : -1.0f);
    return true;
  }
  if (e.type == SDL_EVENT_KEY_DOWN) {
    switch (e.key.key) {
      case SDLK_EQUALS:
      case SDLK_PLUS:
      case SDLK_KP_PLUS:
      case SDLK_MINUS:
      case SDLK_KP_MINUS:
      case SDLK_TAB:
      case SDLK_E: return true;
      default: break;
    }
  }
  if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH ||
                                                  e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
    return true;
  return false;
}

}  // namespace carview
