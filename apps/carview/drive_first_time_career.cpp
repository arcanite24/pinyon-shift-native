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

// ACHIEVEMENT_HORIZON_ROOKIE -> "horizon rookie": the achievement list's name.
std::string achievement_name(std::string_view id) {
  constexpr std::string_view kPrefix = "ACHIEVEMENT_";
  if (id.substr(0, kPrefix.size()) == kPrefix) id.remove_prefix(kPrefix.size());
  std::string n(id);
  std::replace(n.begin(), n.end(), '_', ' ');
  return n;
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
  // The original gives the opening's car a race behaviour on its route; the
  // engine drives the sat-nav route the career set (route 170's own path is
  // not loaded; interim, SPEC_QUESTIONS Q66).
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
  if (c.cutscenes().active())
    c.cutscenes().set_car_parts(car_parts(vehicle, host.showroom.bounds_min(), host.showroom.bounds_max()));
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
      prime_world_and_place_vehicle_as_authored(*this, pos, heading);
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
      app.tod_clock.tod_sec = std::fmod(std::max(0.0, cmd.time_of_day_s), 86400.0);
      if (traffic) traffic->set_hour(app.tod_clock.hour());
    }
    if (cmd.set_time_locked) app.tod_clock.paused = cmd.time_locked;
    if (cmd.set_civilian_traffic && traffic) traffic->set_civilian(cmd.civilian_traffic);
    if (cmd.give_money) app.career.credits += cmd.give_money;
    if (!cmd.achievement.empty()) {
      // ACHIEVEMENT_<NAME>: the achievement list's entry of that name.
      const std::string want = achievement_name(cmd.achievement);
      bool found = false;
      for (uint16_t id = 1; id < 512 && !found; ++id) {
        const auto* e = achievements.find(id);
        if (e && iequal(e->name, want)) {
          achievements.unlock(id);
          found = true;
        }
      }
      if (!found) std::fprintf(stderr, "carview: first-time career: no achievement \"%s\"\n", want.c_str());
    }
    if (cmd.open_map) open_map();
    if (cmd.place_car || cmd.swap_car || cmd.set_destination) ftc_autopilot.reset();
    // show_map_hint: drawn by FirstTimeCareerUi from the career's state.
  }
}

}  // namespace carview
