// --autoplay's per-frame part (autoplay.h): the script's due keys go in as
// SDL key events, ahead of the frame's poll, and the beat log records the
// flow's observable state when it changes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include "drive_session.h"

namespace carview {

namespace {
bool autoplay_key(const std::string& name, SDL_Keycode& key, SDL_Scancode& sc) {
  struct K {
    const char* n;
    SDL_Keycode k;
    SDL_Scancode s;
  };
  static const K keys[] = {
      {"return", SDLK_RETURN, SDL_SCANCODE_RETURN},  {"escape", SDLK_ESCAPE, SDL_SCANCODE_ESCAPE},
      {"space", SDLK_SPACE, SDL_SCANCODE_SPACE},     {"up", SDLK_UP, SDL_SCANCODE_UP},
      {"down", SDLK_DOWN, SDL_SCANCODE_DOWN},        {"left", SDLK_LEFT, SDL_SCANCODE_LEFT},
      {"right", SDLK_RIGHT, SDL_SCANCODE_RIGHT},     {"backspace", SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE},
      {"tab", SDLK_TAB, SDL_SCANCODE_TAB},           {"x", SDLK_X, SDL_SCANCODE_X},
      {"y", SDLK_Y, SDL_SCANCODE_Y},                 {"m", SDLK_M, SDL_SCANCODE_M},
  };
  for (const auto& k : keys) {
    if (name == k.n) {
      key = k.k, sc = k.s;
      return true;
    }
  }
  return false;
}
}  // namespace

void DriveSession::autoplay_frame() {
  auto& ap = app.autoplay;
  // The cursor onto the sign-up place of the event the career's locked map
  // asks for (the heats, then the showcase; the map pans as the stick would;
  // the harness stands in for the player finding the marker).
  auto cursor_to_career_event = [&]() {
    std::array<double, 3> hp{};
    const int64_t target = ftc_map_target().event_id;
    if (!map_open || !target || !app.campaign.ftc().event_trigger(target, hp)) return false;
    map_cam.reset(hp[0], hp[2], map_zoom);
    map_cx = map_cam.cx();
    map_cz = map_cam.cz();
    map_cursor.reset();
    if (map_w > 64 && map_h > 64) {
      const SceneFrame fr = SceneScreen::frame(float(map_w), float(map_h));
      const double mpp = app.map_view.map_scale(map_w, map_h, map_zoom);
      map_cursor.absorb(float((0.5 * map_w + (hp[0] - map_cx) / mpp - fr.ox) / fr.s) - MapCursor::kCx,
                        float((0.5 * map_h - (hp[2] - map_cz) / mpp - fr.oy) / fr.s) - MapCursor::kCy);
    }
    map_dirty = true;
    update_map_selection();
    ap.note("mapto event " + std::to_string(target));
    return true;
  };
  for (const std::string& name : ap.take_due_keys()) {
    if (name == "mapto:heats" || name == "mapto:career") {
      if (!cursor_to_career_event()) ap.note("mapto: no map or no career event");
      continue;
    }
    if (name.rfind("launch:", 0) == 0) {
      app.next_event = name.substr(7);
      app.next_event_fallback.clear();
      running = false;
      ap.note("launch " + app.next_event);
      continue;
    }
    std::string send = name;
    if (name == "advance") {
      // Return where a press moves the career on (a race and its results, a
      // movie, a blocking career beat, controls held by the career, the
      // career's SIGN ME UP! and locked map), not in free roam, where it would open the
      // festival's hub.
      const bool held = app.campaign.ftc_active() && !app.campaign.ftc().controls_enabled();
      const bool sign_up = !app.race && app.campaign.ftc_sign_up_event() > 0;
      // The career's locked map: the cursor onto the event it asks for,
      // then A (the route there lets the map go).
      const bool locked_map = map_open && map_heats_close_left < 0.0 && app.campaign.ftc_active() &&
                              app.campaign.ftc().map_locked() && cursor_to_career_event();
      if (!(app.race || app.fmv_player.active() || app.campaign.is_blocking_gameplay() || held || app.show_title ||
            sign_up || locked_map))
        continue;
      send = "return";
    }
    if (name == "mapclose") {
      // B on an open map that lets the player out (not the career's locked
      // one); nothing when no map is up.
      if (!map_open || (app.campaign.ftc_active() && app.campaign.ftc().map_locked())) continue;
      ap.note("mapclose");
      send = "escape";
    }
    SDL_Keycode key = 0;
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    if (!autoplay_key(send, key, sc)) {
      ap.note("unknown key " + name);
      continue;
    }
    for (const bool down : {true, false}) {
      SDL_Event e{};
      e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      e.key.windowID = app.window ? SDL_GetWindowID(app.window) : 0;
      e.key.key = key;
      e.key.scancode = sc;
      e.key.down = down;
      e.key.repeat = false;
      SDL_PushEvent(&e);
    }
  }
  // `drive auto` stuck (under 1 m/s for 6 s outside a race, menus and the
  // career's held car): the car is put back on the sat-nav route 40 m on.
  {
    static double stuck_since = -1.0;
    const double now = ap.now();
    const auto& route = app.gps_nav.route();
    const bool driving = ap.drive_auto() && !app.race && !app.show_title && !app.fmv_player.active() &&
                         !app.campaign.is_blocking_gameplay() &&
                         (!app.campaign.ftc_active() || app.campaign.ftc().controls_enabled());
    // The career's event to sign up for, and where: with the sat-nav's
    // DESTINATION REACHED short of the sign-up sphere the route is gone, so
    // a car stuck near it is put on the sign-up place (a harness nudge).
    std::array<double, 3> sign_pos{};
    const auto ftc_stage = app.campaign.ftc().stage();
    const int64_t sign_id = ftc_stage == race::FtcStage::kHeats      ? race::FirstTimeCareer::kAllComerFinal
                            : ftc_stage == race::FtcStage::kShowcase ? race::FirstTimeCareer::kMustangRace
                                                                     : 0;
    const drive::Vec3 vp = vehicle.position();
    const bool near_sign_up = app.campaign.ftc_active() && sign_id && app.campaign.ftc_sign_up_event() == 0 &&
                              app.campaign.ftc().event_trigger(sign_id, sign_pos) &&
                              std::hypot(sign_pos[0] - vp.x, sign_pos[2] - vp.z) < 400.0;
    const bool sign_up_stuck = ap.drive_auto() && !app.race && !app.fmv_player.active() && near_sign_up &&
                               app.campaign.ftc().controls_enabled() && std::abs(vehicle.speed()) <= 1.0;
    if (sign_up_stuck) {
      if (stuck_since < 0.0) {
        stuck_since = now;
      } else if (now - stuck_since > 6.0) {
        stuck_since = -1.0;
        const double h = std::atan2(sign_pos[0] - vp.x, sign_pos[2] - vp.z);
        vehicle.reset({sign_pos[0], sign_pos[1] + 1.0, sign_pos[2]}, h);
        ap.note("autoplay unstuck onto the sign-up place");
        std::printf("autoplay: unstuck onto the sign-up place of event %lld\n", (long long)sign_id);
      }
    } else if (!driving || vehicle.speed() > 1.0 || !route.valid || route.points.size() < 2) {
      stuck_since = -1.0;
    } else if (stuck_since < 0.0) {
      stuck_since = now;
    } else if (now - stuck_since > 6.0) {
      stuck_since = -1.0;
      const drive::Vec3 p = vehicle.position();
      size_t best = 0;
      double bd = 1e30;
      for (size_t i = 0; i < route.points.size(); ++i) {
        const double dx = route.points[i].x - p.x, dz = route.points[i].z - p.z;
        if (dx * dx + dz * dz < bd) bd = dx * dx + dz * dz, best = i;
      }
      size_t j = best;
      double run = 0.0;
      while (j + 1 < route.points.size() && run < 40.0) {
        run += std::hypot(route.points[j + 1].x - route.points[j].x, route.points[j + 1].z - route.points[j].z);
        ++j;
      }
      const size_t k = std::min(j + 1, route.points.size() - 1);
      const double h = std::atan2(route.points[k].x - route.points[j].x, route.points[k].z - route.points[j].z);
      vehicle.reset({route.points[j].x, route.points[j].y + 1.0, route.points[j].z}, h);
      ap.note("autoplay unstuck");
    }
  }
  // The beat log.
  const auto& c = app.campaign;
  const auto& f = c.ftc();
  ap.observe("title", app.show_title ? "phase" + std::to_string(int(app.title.phase)) + "/popup" +
                                           std::to_string(int(app.title.popup))
                                     : "off");
  ap.observe("fmv", app.fmv_player.active() ? app.fmv_player.title() : "");
  ap.observe("story", std::to_string(int(app.story_intro.stage())));
  ap.observe("ftc", race::ftc_stage_name(f.stage()));
  ap.observe("ftc_flags", std::string(f.controls_enabled() ? "controls " : "") + (f.ai_driving() ? "ai " : "") +
                              (f.hud_visible() ? "hud " : "") + (f.map_locked() ? "maplock " : ""));
  ap.observe("instruction", f.hud_instruction() + "|" + f.large_instruction_1() + "|" + f.large_instruction_2());
  ap.observe("tickets", std::to_string(f.tickets_remaining()));
  int card = -1;
  if (f.show_opening_credits())
    for (int i = 0; i < race::kOpeningTitleCards; ++i)
      if (race::opening_title_alpha(i, f.opening_credits_time()) > 0.5f) card = i;
  ap.observe("credits_card", f.show_opening_credits() ? std::to_string(card) : "off");
  const auto* cs = c.cutscenes().current();
  ap.observe("cutscene", cs ? cs->name : "");
  ap.observe("tutor_hint", std::to_string(int(c.hint_message())));
  const auto ov = c.screen_overlay();
  const float a = std::round(ov.alpha * 4.0f) / 4.0f;
  char ovs[48];
  std::snprintf(ovs, sizeof ovs, "%.2f rgb %.0f%.0f%.0f", a, ov.rgb[0], ov.rgb[1], ov.rgb[2]);
  ap.observe("overlay", a > 0.0f ? ovs : "0");
  ap.observe("race", app.race ? "on page " + std::to_string(results_page) : "off");
  {
    const auto tel = app.audio_host.telemetry();
    ap.observe("radio", "station " + std::to_string(tel.radio_selected) + " clock10 " +
                            std::to_string(int(tel.radio_clock / 10.0)) + (tel.user_music_active ? " user" : ""));
  }
  ap.observe("radio_popup", radio_popup_gpu.alpha > 0.5f ? radio_popup_gpu.station_name + " / " + radio_popup_gpu.title : "");
  ap.observe("screens", std::string(map_open ? "map " : "") + (autoshow.is_open() ? "autoshow " : "") +
                            (hub_menu.open() ? "hub " : "") + (show_hud ? "hud" : ""));
  const drive::Vec3 p = vehicle.position();
  char pos[64];
  std::snprintf(pos, sizeof pos, "%.0f %.0f", std::round(p.x / 100.0) * 100.0, std::round(p.z / 100.0) * 100.0);
  ap.observe("pos100", pos);
  ap.observe("car", app.car);
}

}  // namespace carview
