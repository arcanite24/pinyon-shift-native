// DriveSession's side of the first-time career: feeds race::FirstTimeCareer
// (through CampaignDirector) the frame's inputs and applies the commands it
// returns that CampaignAction does not carry (time of day and its lock,
// money, the achievement, the map, cutscene car placement and the cutscene
// areas to stream). Car swaps, teleports, sat-nav, event launches and the
// Autoshow go through apply_campaign_action like the rest of the campaign.
// While the career has the controls (the opening, the festival arrival) the
// engine's AI driver drives the player's car on the sat-nav route.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "drive/ai_driver.h"
#include "drive_session.h"

namespace carview {

void prime_world_and_place_vehicle_as_authored(DriveSession& s, std::array<double, 3> target_pos,
                                               double target_heading);
void prime_world_at(DriveSession& s, double x, double z);

namespace {

bool iequal(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

// Known first-time-career achievement tokens -> FH1 SPA achievement IDs
// (so unlocking works regardless of SPA display title or language).
uint16_t ftc_achievement_spa_id(std::string_view id) {
  if (iequal(id, "ACHIEVEMENT_WELCOME_TO_HORIZON")) return 155;  // Welcome to Horizon
  if (iequal(id, "ACHIEVEMENT_HORIZON_ROOKIE")) return 175;      // A Wristed Development (Yellow Wristband)
  return 0;
}

// ACHIEVEMENT_HORIZON_ROOKIE -> "horizon rookie": fallback name match.
std::string achievement_name(std::string_view id) {
  constexpr std::string_view kPrefix = "ACHIEVEMENT_";
  if (id.substr(0, kPrefix.size()) == kPrefix) id.remove_prefix(kPrefix.size());
  std::string n(id);
  std::replace(n.begin(), n.end(), '_', ' ');
  return n;
}

bool query_cutscene_ground_snap(const drive::Ground* g, double x, double y_hint, double z, double& out_y) {
  if (!g) return false;
  double best_y = y_hint;
  double best_d = 1e30;
  bool best_flat = false;
  for (const double from : {y_hint + 35.0, y_hint + 1000.0}) {
    drive::Hit h{};
    if (!g->raycast({x, from, z}, {0, -1, 0}, 2000.0, h) || h.normal.y < 0.2) continue;
    const double y = from - h.distance;
    const bool flat = h.normal.y >= 0.5;
    const double d = std::abs(y - y_hint);
    if ((flat && !best_flat) || (flat == best_flat && d < best_d)) {
      best_flat = flat;
      best_d = d;
      best_y = y;
    }
  }
  if (best_d >= 1e29) return false;
  out_y = best_y;
  return true;
}

bool query_cutscene_eye_ground(const drive::Ground* g, double x, double eye_y, double z, double& out_y) {
  if (!g) return false;
  // Probe from 2.5 m above the eye so low cameras dipping beneath a road crown
  // or kerb are caught without hitting overhead bridges/arches (> 4.5 m up).
  constexpr double kProbeAbove = 2.5;
  constexpr double kProbeDepth = 5.5;
  drive::Hit h{};
  if (!g->raycast({x, eye_y + kProbeAbove, z}, {0, -1, 0}, kProbeDepth, h) || h.normal.y < 0.5) return false;
  out_y = (eye_y + kProbeAbove) - h.distance;
  return true;
}

}  // namespace

// The player car's parts a PartSpace cutscene cam can follow, in body axes
// (+X right, +Y up, +Z forward, origin on the ground). The wheels are the
// physics model's; the lamps sit at the car's corners at the front and rear
// track, with the overhangs split from the drawn length (interim: the
// original reads the parts' own positions from the car model).
cutscene::CarParts car_parts(const drive::Vehicle& v, const std::array<float, 3>& bmin,
                             const std::array<float, 3>& bmax) {
  const auto& w = v.params().wheel_centres;
  cutscene::CarParts parts = {
      {"wheelLF", {w[0].x, w[0].y, w[0].z}},
      {"wheelRF", {w[1].x, w[1].y, w[1].z}},
      {"wheelLR", {w[2].x, w[2].y, w[2].z}},
      {"wheelRR", {w[3].x, w[3].y, w[3].z}},
  };
  const double front_z = 0.5 * (w[0].z + w[1].z), rear_z = 0.5 * (w[2].z + w[3].z);
  const double wheelbase = std::max(1.5, front_z - rear_z);
  double length = double(bmax[2]) - double(bmin[2]);
  if (!(length > wheelbase + 0.5 && length < 8.0)) length = wheelbase + 1.8;
  const double overhang = 0.5 * (length - wheelbase);
  const double half_track_f = std::max(0.5, 0.5 * std::abs(w[1].x - w[0].x));
  const double half_track_r = std::max(0.5, 0.5 * std::abs(w[3].x - w[2].x));
  const double left = w[0].x < w[1].x ? -1.0 : 1.0;  // the left wheels' side
  const double lamp_y = 0.7;
  parts.push_back({"headlightL", {left * half_track_f, lamp_y, front_z + overhang}});
  parts.push_back({"headlightR", {-left * half_track_f, lamp_y, front_z + overhang}});
  parts.push_back({"taillightL", {left * half_track_r, lamp_y + 0.1, rear_z - overhang}});
  parts.push_back({"taillightR", {-left * half_track_r, lamp_y + 0.1, rear_z - overhang}});
  return parts;
}

struct DriveSession::FtcAutopilot {
  drive::RacingLine line;
  std::unique_ptr<drive::AiField> field;
  double route_length = -1.0, dest_x = 0.0, dest_z = 0.0;
};

drive::Input DriveSession::ftc_autopilot_input() {
  // In the original flow (first_time_career.xml SetupFestivalDrive), AI player
  // control is disabled before New_Diner_Cutscene_2 and New_Diner_Cutscene_3 so
  // the Viper does not keep driving into the Diner lot while the camera pans.
  if (const auto* cs = app.campaign.cutscenes().current()) {
    if (iequal(cs->name, "New_Diner_Cutscene_2") || iequal(cs->name, "New_Diner_Cutscene_3")) {
      drive::Input stop{};
      stop.brake = 1.0;
      return stop;
    }
  }
  // The original gives the car a race behaviour on route 170, whose
  // waypoints start at the diner and end in the festival; the engine drives
  // the sat-nav route the career set (to the diner under the opening, to
  // route 170's last waypoint at the festival arrival). The road between
  // waypoints is the sat-nav's (interim, SPEC_QUESTIONS Q159).
  const auto& route = app.gps_nav.route();
  if (!route.valid || route.points.size() < 2) return drive::Input{};
  if (!ftc_autopilot) ftc_autopilot = std::make_shared<FtcAutopilot>();
  auto& ap = *ftc_autopilot;
  if (!ap.field || ap.route_length != route.total_length || ap.dest_x != route.dest_x || ap.dest_z != route.dest_z) {
    std::vector<float> x, z;
    x.reserve(route.points.size());
    z.reserve(route.points.size());
    for (const auto& p : route.points) {
      x.push_back(float(p.x));
      z.push_back(float(p.z));
    }
    // The sat-nav's points are tens of metres apart: their corners are
    // rounded off (two passes of corner cutting) so the line's curvature,
    // and the speed planned from it, is the bend's rather than a straight.
    for (int pass = 0; pass < 2 && x.size() >= 3; ++pass) {
      std::vector<float> sx{x.front()}, sz{z.front()};
      for (size_t i = 0; i + 1 < x.size(); ++i) {
        sx.push_back(0.75f * x[i] + 0.25f * x[i + 1]);
        sz.push_back(0.75f * z[i] + 0.25f * z[i + 1]);
        sx.push_back(0.25f * x[i] + 0.75f * x[i + 1]);
        sz.push_back(0.25f * z[i] + 0.75f * z[i + 1]);
      }
      sx.push_back(x.back());
      sz.push_back(z.back());
      x = std::move(sx);
      z = std::move(sz);
    }
    ap.field.reset();
    // A road car on a public road: cornering kept well inside the grip, and
    // no faster than the 100 mph the opening places it at (interim).
    ap.line = drive::RacingLine::from_points(x, z, 6.5, 4.0, 6.0, 100.0 * cutscene::kMphToMps);
    if (ap.line.size() < 2) return drive::Input{};
    ap.field = std::make_unique<drive::AiField>(ap.line, size_t(1), drive::AiField::Options{});
    const drive::Vec3 p = vehicle.position();
    ap.field->place_near(0, p.x, p.z, 0.0, ap.line.length());
    ap.route_length = route.total_length;
    ap.dest_x = route.dest_x;
    ap.dest_z = route.dest_z;
  }
  const drive::AiCarState st = drive::ai_state(vehicle);
  drive::Input in;
  ap.field->drive(std::span<const drive::AiCarState>(&st, 1), std::span<drive::Input>(&in, 1));
  // Near the route's end the car slows to a stop.
  if (ap.field->progress(0) > ap.line.length() - 15.0) {
    in.throttle = 0.0;
    in.brake = std::max(in.brake, 0.6);
  }
  return in;
}

void DriveSession::feed_first_time_career() {
  auto& c = app.campaign;
  if (!c.ftc_active() && !c.tutor().active()) return;
  if (app.have_career_catalog) c.set_career_catalog(&app.career_catalog);
  // The sign-up ticket's time up: the career launches the event.
  if (ftc_ticket_at >= 0.0) {
    const double shown = vehicle.time() - ftc_ticket_at;
    if (!ftc_ticket_signed && shown >= kFtcTicketSeconds) {
      c.ftc_sign_up();
      ftc_ticket_signed = true;
    } else if (ftc_ticket_signed && shown >= kFtcTicketSeconds + 10.0) {
      ftc_ticket_at = -1.0;  // the launch did not happen: drop the ticket
    }
  }
  CampaignDirector::FtcFrameInput in;
  const drive::Vec3 vp = vehicle.position();
  in.player_pos = {vp.x, vp.y, vp.z};
  in.throttle = ftc_throttle;
  in.screen_open = map_open || autoshow.is_open() || app.pending_autoshow_open;
  in.loading = loading_screen.active() || host.showroom.has_pending_world();
  in.racing = app.race != nullptr;
  // The driving tutor's inputs.
  in.speed_mps = std::abs(vehicle.speed());
  in.impact = host.last_impact_vfx_time > ftc_last_impact_seen + 1e-9;
  ftc_last_impact_seen = host.last_impact_vfx_time;
  in.rewind_available = rewind_buf.can_trigger();
  in.rewind_pressed = rewind_buf.active();
  in.ebrake_pressed = ftc_handbrake;
  ftc_handbrake = false;
  const auto& g = app.gps_nav.guidance();
  using traffic::GpsTurnKind;
  in.turn_ahead = g.active && (g.next_turn == GpsTurnKind::kLeft || g.next_turn == GpsTurnKind::kSharpLeft ||
                               g.next_turn == GpsTurnKind::kRight || g.next_turn == GpsTurnKind::kSharpRight);
  in.turn_distance_m = g.dist_to_turn_m;
  c.set_ftc_frame_input(in);
}

void DriveSession::apply_first_time_career() {
  auto& c = app.campaign;
  // The cutscene areas to stream before they play. Not while the player
  // drives: recentring the world away from the car would drop the road under
  // it (the cutscene's own placement primes its area as it starts).
  for (const auto& at : c.take_cutscene_preloads()) {
    if (c.ftc_active() && c.ftc().controls_enabled()) continue;
    prime_world_at(*this, at[0], at[2]);
  }
  if (c.cutscenes().active()) {
    DriveWorldHost* h = &host;
    c.cutscenes().set_ground_query([h](double x, double y_hint, double z, double& out_y) {
      return query_cutscene_ground_snap(h->ground, x, y_hint, z, out_y);
    });
    c.cutscenes().set_eye_clearance_query([h](double x, double eye_y, double z, double& out_y) {
      return query_cutscene_eye_ground(h->ground, x, eye_y, z, out_y);
    });
    c.cutscenes().set_car_body_offset_y(double(info.lift));
    c.cutscenes().set_car_parts(car_parts(vehicle, host.showroom.bounds_min(), host.showroom.bounds_max()));
    // New_Diner_Cutscene_2 has TargetName="" cams (Cam[2], Cam[3]) intended to
    // frame the parked Corrado (FestivalCar_4) at the Diner before the player's
    // Viper is swapped to the Corrado. Anchor those cuts to FestivalCar_4.
    if (const auto* cs = c.cutscenes().current(); cs && iequal(cs->name, "New_Diner_Cutscene_2")) {
      std::optional<cutscene::Frame> hero_frame;
      for (const auto& ev : cs->events) {
        if (ev.type == cutscene::EventType::kCar && iequal(ev.name, "FestivalCar_4")) {
          cutscene::Frame f{ev.position(), ev.rotation_deg()};
          double gy = f.origin[1];
          if (query_cutscene_ground_snap(host.ground, f.origin[0], f.origin[1], f.origin[2], gy)) f.origin[1] = gy;
          hero_frame = f;
          break;
        }
      }
      c.cutscenes().set_car_frame_override(hero_frame);
    } else {
      c.cutscenes().set_car_frame_override(std::nullopt);
    }
  }
  std::array<double, 3> pos{};
  double heading = 0.0, speed = 0.0;
  if (c.take_place_car(pos, heading, speed)) {
    // A cutscene's PlacePlayerCar: the car where the cutscene authors it
    // (x/z and heading kept, dropped onto the surface there), moving at its
    // initial speed along its heading.
    prime_world_and_place_vehicle_as_authored(*this, pos, heading);
    if (speed > 0.0) {
      const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
      vehicle.set_velocity({f.x * speed, f.y * speed, f.z * speed}, {0, 0, 0});
    }
    ftc_autopilot.reset();
  }
  {
    const drive::Vec3 vp = vehicle.position();
    const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
    if (c.take_stream_anchor({vp.x, vp.y, vp.z}, std::atan2(vf.x, vf.z), pos, heading))
      prime_world_at(*this, pos[0], pos[2]);
  }
  for (const auto& car : c.take_cutscene_cars()) {
    // CCutsceneCarTrigger / CCutsceneReleaseCarTrigger: the engine's traffic
    // has no scripted spawn yet (interim; the cutscene's other cars are
    // missing from its frames).
    if (car.release)
      std::printf("cutscene: release %s at %.1f m/s (no scripted traffic yet)\n", car.name.c_str(), car.speed_mps);
    else
      std::printf("cutscene: car %s (CarID %lld) at (%.1f, %.1f, %.1f) (no scripted traffic yet)\n", car.name.c_str(),
                  (long long)car.car_id, car.pos[0], car.pos[1], car.pos[2]);
  }
  for (const auto& cmd : c.take_ftc_commands()) {
    if (cmd.set_time_of_day) {
      // A set time ends any request still running toward another.
      app.tod_clock.transition = false;
      app.tod_clock.rate_override = false;
      app.tod_clock.tod_sec = std::fmod(std::max(0.0, cmd.time_of_day_s), 86400.0);
      if (traffic) traffic->set_hour(app.tod_clock.hour());
    }
    if (cmd.set_time_locked) app.tod_clock.paused = cmd.time_locked;
    if (cmd.request_time_of_day) {
      // A time-of-day zone: the clock runs on to the zone's time and holds.
      const race::FtcTodRequest r =
          race::ftc_tod_request(app.tod_clock.tod_sec, cmd.tod_request_target_s, cmd.tod_request_duration_s);
      if (r.rate > 0.0) app.tod_clock.request(r.target_s, r.rate);
    }
    if (cmd.set_civilian_traffic && traffic) traffic->set_civilian(cmd.civilian_traffic);
    if (cmd.give_money) app.career.credits += cmd.give_money;
    if (!cmd.achievement.empty()) {
      // Resolve known FTC tokens by SPA ID first, then fall back to name match.
      bool found = false;
      if (const uint16_t spa_id = ftc_achievement_spa_id(cmd.achievement); spa_id != 0 && achievements.find(spa_id)) {
        achievements.unlock(spa_id);
        found = true;
      }
      if (!found) {
        const std::string want = achievement_name(cmd.achievement);
        for (uint16_t id = 1; id < 512 && !found; ++id) {
          const auto* e = achievements.find(id);
          if (e && iequal(e->name, want)) {
            achievements.unlock(id);
            found = true;
          }
        }
        if (!found) std::fprintf(stderr, "carview: first-time career: no achievement \"%s\"\n", want.c_str());
      }
    }
    if (cmd.show_loading && !app.headless) {
      // CShowLoadingScreen: the freeroam group's tip card, held the event
      // load's minimum (interim: the original's hold is not timed, Q312).
      loading_screen.load_definitions(app.game_root_of(), "freeroam", app.state_root);
      loading_screen.start({});
      loading_min_until = carview::LoadingScreen::kEventMinSeconds;
    }
    if (cmd.open_map) ftc_open_map_pending = true;
    if (cmd.place_car || cmd.swap_car || cmd.set_destination) ftc_autopilot.reset();
    // show_map_hint: drawn by FirstTimeCareerUi from the career's state.
  }
  // The career's map opens once nothing is over it: after the heats the
  // stage moves on at the award, under the race's reward pages, and the
  // original shows the rewards, a loading tip, then the map (first30e r10).
  // The race's way back to free roam restarts the session, so the
  // showcase's map is also opened whenever its lock is on and it is not up
  // (that covers a resume at the showcase too).
  const bool showcase_map = c.ftc_active() && c.ftc().stage() == race::FtcStage::kShowcase && c.ftc().map_locked();
  if ((ftc_open_map_pending || showcase_map) && !app.race && !app.fmv_player.active() && !loading_screen.active() &&
      !host.showroom.has_pending_world() && !app.show_title) {
    ftc_open_map_pending = false;
    if (!map_open) open_map();
  }
}

}  // namespace carview
