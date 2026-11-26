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
#include "traffic/scripted.h"

namespace carview {

void prime_world_and_place_vehicle_as_authored(DriveSession& s, std::array<double, 3> target_pos,
                                               double target_heading);
void prime_world_at(DriveSession& s, double x, double z);
void preload_world_at_async(DriveSession& s, double x, double z);

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

constexpr double kFtcPi = 3.14159265358979323846;

struct ScriptedCarPreset {
  int64_t car_id;
  const char* media;
  int64_t paint_rgb;  // -1 = model default
};

constexpr ScriptedCarPreset kKnownScriptedCars[] = {
    {1171, "FER_599XX_10", 0xC81018},          // Darius Flynt's Ferrari 599XX
    {1544, "VIP_Viper_13", 0xF2B800},          // Opening Viper
    {1565, "VW_IECorrado_95", 0xDC001F},       // Diner Corrado (FestivalCar_4)
    {309, "VW_Corrado_95", 0xDC001F},          // Player Corrado
    {1399, "FOR_FocusST_12", 0xD89818},        // Diner Focus ST (FestivalCar_0)
    {461, "TOY_SupraRZ_98", 0xA81820},         // Diner Supra RZ (FestivalCar_1)
    {289, "CHE_CamaroSS_69", 0x26486E},        // Diner Camaro SS (FestivalCar_2)
    {375, "MIT_LancerEvoVIGSR_99", 0xEAEAEA},  // Diner Evo VI (FestivalCar_3)
};

constexpr const char* kFallbackScriptedMedia[] = {
    "FOR_FocusST_12",
    "TOY_SupraRZ_98",
    "CHE_CamaroSS_69",
    "MIT_LancerEvoVIGSR_99",
};

std::string resolve_scripted_car_media(const DriveApp& app, int64_t car_id, std::string_view trigger_name,
                                       int64_t& paint_rgb_out) {
  paint_rgb_out = -1;
  for (const auto& p : kKnownScriptedCars) {
    if (p.car_id == car_id) {
      paint_rgb_out = p.paint_rgb;
      // Prefer the exact media name if baked; for VW_IECorrado_95 fall back to VW_Corrado_95 if absent.
      if (car_id == 1565 && !app.baked_ids.count(1565)) {
        if (auto it = app.baked_ids.find(309); it != app.baked_ids.end()) return it->second;
      }
      return p.media;
    }
  }
  if (car_id > 0) {
    if (auto it = app.baked_ids.find(car_id); it != app.baked_ids.end() && !it->second.empty()) return it->second;
    if (app.have_career_catalog) {
      if (const auto* spec = app.career_catalog.find_car(car_id); spec && !spec->media_name.empty())
        return spec->media_name;
    }
  }
  // CarID == -1 placeholders (Skills_Unlock, StreetRaceHub_*): pick deterministically by trigger name.
  size_t h = 0;
  for (char ch : trigger_name) h = h * 31u + size_t(static_cast<unsigned char>(ch));
  return kFallbackScriptedMedia[h % std::size(kFallbackScriptedMedia)];
}

struct FtcScriptedSessionState {
  const DriveSession* owner = nullptr;
  race::FtcStage last_stage = race::FtcStage::kNotStarted;
  const cutscene::Cutscene* last_cutscene = nullptr;
  bool race_rivals_seeded = false;
  double guidance_route_len = -1.0;
  double guidance_dest_x = 0.0;
  double guidance_dest_z = 0.0;
  traffic::ScriptedCars cars;

  void reset(const DriveSession* new_owner) {
    owner = new_owner;
    last_stage = race::FtcStage::kNotStarted;
    last_cutscene = nullptr;
    race_rivals_seeded = false;
    guidance_route_len = -1.0;
    guidance_dest_x = 0.0;
    guidance_dest_z = 0.0;
    cars.clear();
    cars.clear_guidance_route();
  }
};

FtcScriptedSessionState& scripted_state(const DriveSession& s) {
  static FtcScriptedSessionState st;
  if (st.owner != &s) st.reset(&s);
  return st;
}

const traffic::Roads* active_scripted_roads(const DriveSession& s) {
  if (s.app.gps_solver.ready()) return &s.app.gps_solver.roads();
  if (!s.festival_roads.roads().empty()) return &s.festival_roads;
  return nullptr;
}

bool stage_keeps_scripted_cars(race::FtcStage st) {
  return st == race::FtcStage::kControls || st == race::FtcStage::kOpeningCutscene ||
         st == race::FtcStage::kInitialDrive || st == race::FtcStage::kDinerCutscenes ||
         st == race::FtcStage::kRaceToFestival || st == race::FtcStage::kFestivalArrive;
}

void sync_scripted_guidance(FtcScriptedSessionState& st, const DriveSession& s) {
  const auto& route = s.app.gps_nav.route();
  if (!route.valid || route.points.size() < 2) return;
  if (st.guidance_route_len == route.total_length && st.guidance_dest_x == route.dest_x &&
      st.guidance_dest_z == route.dest_z)
    return;
  std::vector<traffic::P3> pts;
  pts.reserve(route.points.size());
  for (const auto& p : route.points) pts.push_back({p.x, p.y, p.z});
  st.cars.set_guidance_route(std::move(pts));
  st.guidance_route_len = route.total_length;
  st.guidance_dest_x = route.dest_x;
  st.guidance_dest_z = route.dest_z;
}

void seed_race_to_festival_rivals(FtcScriptedSessionState& st, DriveSession& s) {
  if (st.race_rivals_seeded) return;
  st.race_rivals_seeded = true;
  const traffic::Roads* roads = active_scripted_roads(s);
  const auto& route = s.app.gps_nav.route();
  if (!roads || !route.valid || route.points.size() < 4) return;

  DriveWorldHost* h = &s.host;
  auto gfn = [h](double x, double y_hint, double z, double& out_y) {
    return query_cutscene_ground_snap(h->ground, x, y_hint, z, out_y);
  };

  struct RivalDef {
    const char* name;
    int64_t car_id;
    double dist_ahead_m;
    double speed_mps;
  };
  constexpr RivalDef kRivals[] = {
      {"FestivalRival_0", 1399, 55.0, 35.0},   // Focus ST
      {"FestivalRival_1", 461, 105.0, 37.5},   // Supra RZ
      {"FestivalRival_2", 289, 160.0, 36.0},   // Camaro SS
      {"FestivalRival_3", 375, 220.0, 38.5},   // Lancer Evo VI
  };

  // Walk along the active GPS route to place each rival cleanly on the highway ahead.
  for (const auto& rv : kRivals) {
    double acc = 0.0;
    traffic::P3 pos{route.points[0].x, route.points[0].y, route.points[0].z};
    double heading = 0.0;
    for (size_t i = 0; i + 1 < route.points.size(); ++i) {
      const auto& a = route.points[i];
      const auto& b = route.points[i + 1];
      const double seg = std::hypot(b.x - a.x, b.z - a.z);
      if (acc + seg >= rv.dist_ahead_m && seg > 1e-3) {
        const double f = (rv.dist_ahead_m - acc) / seg;
        pos = {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f};
        heading = std::atan2(b.x - a.x, b.z - a.z);
        break;
      }
      acc += seg;
    }
    int64_t paint_rgb = -1;
    traffic::ScriptedCars::SpawnParams sp{};
    sp.name = rv.name;
    sp.car_id = rv.car_id;
    sp.car_media = resolve_scripted_car_media(s.app, rv.car_id, rv.name, paint_rgb);
    sp.paint_rgb = paint_rgb;
    sp.pos = pos;
    sp.heading_rad = heading;
    sp.snap_to_ground = true;
    st.cars.spawn(sp, gfn);

    traffic::ScriptedCars::ReleaseParams rp{};
    rp.name = rv.name;
    rp.initial_speed_mps = rv.speed_mps;
    rp.despawn_time_s = 0.0;
    st.cars.release(rp, roads, gfn);
  }
}

}  // namespace

const traffic::ScriptedCars* ftc_scripted_cars(const DriveSession& s) {
  auto& st = scripted_state(s);
  if (st.cars.empty()) return nullptr;
  return &st.cars;
}

void step_ftc_scripted_cars(DriveSession& s, double dt) {
  auto& st = scripted_state(s);
  if (!s.app.campaign.ftc_active() && !s.app.campaign.cutscenes().active()) {
    if (!st.cars.empty()) st.cars.clear();
    return;
  }
  if (st.cars.empty()) return;
  sync_scripted_guidance(st, s);
  DriveWorldHost* h = &s.host;
  auto gfn = [h](double x, double y_hint, double z, double& out_y) {
    return query_cutscene_ground_snap(h->ground, x, y_hint, z, out_y);
  };
  traffic::ScriptedCars::StepOptions opts{};
  const drive::Vec3 vp = s.vehicle.position();
  const drive::Vec3 vf = drive::rotate(s.vehicle.orientation(), {0, 0, 1});
  opts.has_player = true;
  opts.player_pos = {vp.x, vp.y, vp.z};
  opts.player_heading = std::atan2(vf.x, vf.z);
  opts.player_speed = std::abs(s.vehicle.speed());
  st.cars.step(dt, active_scripted_roads(s), gfn, opts);
}

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
  // it (the cutscene's own placement primes its area as it starts). Advisory
  // preloads run asynchronously so movies, loading screens, and lead-in cuts
  // never stall the main thread.
  for (const auto& at : c.take_cutscene_preloads()) {
    if (c.ftc_active() && c.ftc().controls_enabled()) continue;
    preload_world_at_async(*this, at[0], at[2]);
  }
  if (!c.cutscenes().active() && c.ftc_active() && c.ftc().controls_enabled() && host.streamer) {
    host.streamer->clear_preload();
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
  {
    auto& st = scripted_state(*this);
    const cutscene::Cutscene* cur_cs = c.cutscenes().current();
    const bool ftc_on = c.ftc_active();
    const race::FtcStage cur_stage = ftc_on ? c.ftc().stage() : race::FtcStage::kNotStarted;

    // Lifecycle cleanup: never leak scripted cutscene cars into normal free roam
    // or past the opening / diner / Race to Festival sequence.
    if (!ftc_on && !cur_cs) {
      if (!st.cars.empty()) st.cars.clear();
    } else if (ftc_on && cur_stage != st.last_stage) {
      if (!stage_keeps_scripted_cars(cur_stage) && !cur_cs) {
        st.cars.clear();
      } else if (cur_stage == race::FtcStage::kRaceToFestival) {
        // Player is now in the Corrado at the Diner: drop the parked prop Corrado.
        st.cars.erase("FestivalCar_4");
      }
      st.last_stage = cur_stage;
    }

    if (cur_cs != st.last_cutscene) {
      if (cur_cs) {
        if (iequal(cur_cs->name, "NEW_Diner_Cutscene_1")) {
          st.cars.clear();
          // Q279: asynchronously prime New_Diner_Cutscene_2 (422 m down the highway,
          // still well inside the 750 m collision radius of NEW_Diner_Cutscene_1)
          // while the 6.7 s approach shot plays so the cut lands without a stall.
          if (const auto* next_cs = c.cutscenes().library().find("New_Diner_Cutscene_2")) {
            for (const auto& ev : next_cs->events) {
              if (ev.type == cutscene::EventType::kPlacePlayerCar || ev.type == cutscene::EventType::kCameraNode) {
                const auto at = ev.position();
                preload_world_at_async(*this, at[0], at[2]);
                break;
              }
            }
          }
        } else if (iequal(cur_cs->name, "New_Diner_Cutscene_2")) {
          st.cars.clear();
        } else if (iequal(cur_cs->name, "New_Diner_Cutscene_3")) {
          // PlacePlayerCar_0 puts the player at FestivalCar_4's exact Diner spot.
          st.cars.erase("FestivalCar_4");
        }
      }
      st.last_cutscene = cur_cs;
    }

    sync_scripted_guidance(st, *this);
    DriveWorldHost* h = &host;
    auto gfn = [h](double x, double y_hint, double z, double& out_y) {
      return query_cutscene_ground_snap(h->ground, x, y_hint, z, out_y);
    };
    const traffic::Roads* roads = active_scripted_roads(*this);

    for (const auto& car : c.take_cutscene_cars()) {
      // Look up the triggering XML event for snapToGround / samePositionAsPlace / despawnTime.
      const cutscene::Event* src_ev = nullptr;
      if (cur_cs) {
        for (const auto& ev : cur_cs->events) {
          if (car.release && ev.type == cutscene::EventType::kReleaseCar && iequal(ev.name, car.name)) {
            src_ev = &ev;
            break;
          }
          if (!car.release && ev.type == cutscene::EventType::kCar && iequal(ev.name, car.name)) {
            src_ev = &ev;
            break;
          }
        }
      }
      if (car.release) {
        traffic::ScriptedCars::ReleaseParams rp{};
        rp.name = car.name;
        rp.initial_speed_mps = car.speed_mps;
        if (src_ev) {
          rp.use_release_pose = src_ev->number("samePositionAsPlace", 1.0) == 0.0 && src_ev->has("x");
          if (rp.use_release_pose) {
            const auto ep = src_ev->position();
            rp.pos = {ep[0], ep[1], ep[2]};
            rp.heading_rad = src_ev->rotation_deg() * kFtcPi / 180.0;
            rp.snap_to_ground = src_ev->number("snapToGround") != 0.0;
          }
          rp.despawn_time_s = src_ev->number("despawnTime");
        }
        const bool ok = st.cars.release(rp, roads, gfn);
        std::printf("cutscene: release %s -> %s at %.1f m/s%s\n", car.name.c_str(),
                    traffic::ScriptedCars::canonical_name(car.name).c_str(), car.speed_mps,
                    ok ? "" : " (not spawned)");
      } else {
        int64_t paint_rgb = -1;
        traffic::ScriptedCars::SpawnParams sp{};
        sp.name = car.name;
        sp.car_id = car.car_id;
        sp.car_media = resolve_scripted_car_media(app, car.car_id, car.name, paint_rgb);
        sp.paint_rgb = paint_rgb;
        sp.pos = {car.pos[0], car.pos[1], car.pos[2]};
        sp.heading_rad = car.heading_rad;
        sp.snap_to_ground = src_ev ? (src_ev->number("snapToGround") != 0.0) : false;
        // Darius Flynt's 599XX in Opening_Cutscene leads the player all the way to the Diner.
        sp.pace_ahead = cur_cs && iequal(cur_cs->name, "Opening_Cutscene") && car.car_id == 1171;
        st.cars.spawn(sp, gfn);
        std::printf("cutscene: spawned %s (%s, CarID %lld) at (%.1f, %.1f, %.1f)\n", sp.name.c_str(),
                    sp.car_media.c_str(), (long long)car.car_id, car.pos[0], car.pos[1], car.pos[2]);
      }
    }

    if (ftc_on && cur_stage == race::FtcStage::kRaceToFestival && !cur_cs && st.cars.cars().size() < 2) {
      seed_race_to_festival_rivals(st, *this);
    }
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
