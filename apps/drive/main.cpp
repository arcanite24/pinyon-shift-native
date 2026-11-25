// drive: runs the engine's car physics headless with a scripted input and
// writes a trace (M3): on flat ground, or with --world on Colorado's
// collision (COLLISION.md `.fiz` tiles).
//
//   drive --bake <dir> --car <media name> [--script <file> | --run <name>]
//         [--seconds N] [--trace <file.csv>] [--physics-trace <file.jsonl> [--every N]]
//         [--world <colorado dir> [--cache <index file>] [--near x z]
//          [--spawn x y z heading] [--materials] [--render-ground]]
//
// --world streams the collision and the render models around the car and
// starts at COLLISION.md's measured free-roam spawn by the festival, or at
// the road triangle nearest --near (x, z in the game frame, the file frame
// with z negated), or at --spawn; the car is dropped onto the collision
// below the point. --render-ground drives on the earlier stand-in (the
// render triangles) instead of the collision.
// --physics-trace writes SPEC-CAR-PHYSICS's fh1.physics_trace events (JSONL),
// one every --every frames (default 6) of a 60 Hz clock on the sim time:
// frame = round(sim time x 60).
// --grade G drives on a plane mesh (drive::MeshGround) rising G (rise over
// run) towards +z, instead of the flat ground.
// --run cruise (with --world) drives itself along the road at --speed km/h
// (default 40), with libs/world's test driver.
// --run lap --line <lap.jsonl> (with --world) drives the M3 gate's lap
// (SPEC-CAR-PHYSICS, "Oracle lap for the M3 gate"): from the oracle's grid
// it settles --settle s (default 2) on the handbrake, then from the
// start signal replays the oracle's gas and brake against the distance
// along its line and steers by pure pursuit on it (--lookahead s of
// speed, default 0.5, at least 5 m). The race clock stops when the body
// point crosses the gate of --lap-json (default lap.json beside the line)
// or --gate x z nx nz. --physics-trace then writes every 60 Hz frame as a
// mode-3 sample and ends with a dlc.rally.race_trace event, for
// physics-behaviour.py compare.
// --ai N --line <lap.jsonl> (with --world) runs N AI cars of --car on the
// oracle's line (drive/ai_driver.h, M5 groundwork): staggered 15 m apart in
// three lanes, each at a skill of 0.92 to 0.99 of the line's speed, and
// prints the cost of the AI pass and of the cars' physics per 120 Hz step.
// --materials prints the triangles per material name of the first gather
// with our surface class.
//
// Built-in runs: rest (no input), launch (full throttle), brake (full
// throttle to 100 km/h, then full brake), circle (part throttle, half
// steer right), reverse (brake held from rest). Prints a summary: the
// 0-100 km/h time, the top speed, the stopping distance from 100 km/h and
// the rest pose. All physics constants are placeholders (libs/drive).

#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "cars/bake_format.h"
#include "cars/sources.h"
#include "drive/ai_driver.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "drive/mesh_ground.h"
#include "drive/oracle_lap.h"
#include "drive/vehicle.h"
#include "world/road_follow.h"
#include "world/stream.h"
#include "world/world.h"

namespace fs = std::filesystem;

namespace {

const std::map<std::string, std::string> kRuns = {
    {"rest", "0 0 0 0\n"},
    {"launch", "0 0 1 0\n"},
    {"brake", "0 0 1 0\n"},  // the brake is applied at 100 km/h by the run itself
    {"circle", "0 0 0 0\n1 0.5 0.35 0\n"},
    {"reverse", "0 0 0 1\n"},
    {"cruise", "0 0 0 0\n"},  // input from world::RoadFollower
    {"lap", "0 0 0 0\n"},     // input from drive::LineFollower
};

}  // namespace

int main(int argc, char** argv) {
  fs::path bake_dir, script_path, trace_path, world_dir, cache_path, physics_trace_path, line_path, lap_json_path;
  int ai_cars = 0;  // --ai N
  double settle = 2, lookahead_s = 0.5;
  bool have_gate = false, seconds_given = false;
  drive::Gate gate;
  int physics_every = 6;
  std::string car, run = "launch";
  double seconds = 20;
  double near_x = world::kDumpCameraX, near_z = world::kDumpCameraZ, near_heading = world::kDumpCameraHeading;
  bool near_given = false, have_spawn = false, materials = false, render_ground = false;
  double cruise_kmh = 40, grade = 0;
  bool have_grade = false, have_probe = false;
  double spawn[4] = {0, 0, 0, 0}, probe[3] = {0, 0, 0};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--bake") bake_dir = next();
    else if (a == "--car") car = next();
    else if (a == "--script") script_path = next();
    else if (a == "--run") run = next();
    else if (a == "--seconds") {
      seconds = std::stod(next());
      seconds_given = true;
    }
    else if (a == "--line") line_path = next();
    else if (a == "--ai") {
      ai_cars = std::max(0, std::stoi(next()));
      run = "lap";
    }
    else if (a == "--lap-json") lap_json_path = next();
    else if (a == "--settle") settle = std::max(0.0, std::stod(next()));
    else if (a == "--lookahead") lookahead_s = std::stod(next());
    else if (a == "--gate") {
      gate.x = std::stod(next());
      gate.z = std::stod(next());
      gate.nx = std::stod(next());
      gate.nz = std::stod(next());
      const double len = std::hypot(gate.nx, gate.nz);
      if (len > 0) gate.nx /= len, gate.nz /= len;
      have_gate = true;
    }
    else if (a == "--trace") trace_path = next();
    else if (a == "--physics-trace") physics_trace_path = next();
    else if (a == "--every") physics_every = std::max(1, std::stoi(next()));
    else if (a == "--world") world_dir = next();
    else if (a == "--cache") cache_path = next();
    else if (a == "--materials") materials = true;
    else if (a == "--speed") cruise_kmh = std::stod(next());
    else if (a == "--grade") {
      grade = std::stod(next());
      have_grade = true;
    }
    else if (a == "--render-ground") render_ground = true;
    else if (a == "--probe") {
      for (double& v : probe) v = std::stod(next());
      have_probe = true;
    }
    else if (a == "--near") {
      near_x = std::stod(next());
      near_z = std::stod(next());
      near_given = true;
    } else if (a == "--spawn") {
      for (double& v : spawn) v = std::stod(next());
      have_spawn = true;
    } else {
      std::fprintf(stderr, "usage: drive --bake <dir> --car <name> [--script <file> | --run "
                           "rest|launch|brake|circle|reverse] [--seconds N] [--trace <csv>]\n"
                           "             [--physics-trace <jsonl> [--every N]]\n"
                           "             [--run lap --line <lap.jsonl> [--lap-json <file> | --gate x z nx nz] "
                           "[--settle s] [--lookahead s]]\n"
                           "             [--world <colorado dir> [--cache <file>] [--near x z] "
                           "[--spawn x y z heading] [--materials] [--render-ground]]\n");
      return 2;
    }
  }
  if (bake_dir.empty() || car.empty()) {
    std::fprintf(stderr, "drive: --bake and --car are required\n");
    return 2;
  }
  cars::bake::Bake bake;
  std::string error;
  if (!cars::bake::read_file(bake_dir / "cars" / (cars::lower(car) + ".shiftbake"), bake, &error)) {
    std::fprintf(stderr, "drive: %s\n", error.c_str());
    return 1;
  }
  std::string text;
  if (!script_path.empty()) {
    std::ifstream in(script_path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  } else if (auto it = kRuns.find(run); it != kRuns.end()) {
    text = it->second;
  } else {
    std::fprintf(stderr, "drive: no run named %s\n", run.c_str());
    return 2;
  }
  std::vector<drive::ScriptLine> script;
  if (!drive::parse_script(text, script, &error)) {
    std::fprintf(stderr, "drive: %s\n", error.c_str());
    return 1;
  }

  // The oracle lap: its line, gate and grid.
  const bool lap = run == "lap" && script_path.empty();
  drive::OracleLine line;
  if (lap) {
    if (line_path.empty() || world_dir.empty()) {
      std::fprintf(stderr, "drive: --run lap needs --line <lap.jsonl> and --world\n");
      return 2;
    }
    auto slurp = [](const fs::path& p, std::string& out) {
      std::ifstream in(p, std::ios::binary);
      if (!in) return false;
      std::stringstream ss;
      ss << in.rdbuf();
      out = ss.str();
      return true;
    };
    std::string line_text, json_text;
    if (!slurp(line_path, line_text) || !drive::parse_oracle_line(line_text, line, &error)) {
      std::fprintf(stderr, "drive: %s: %s\n", line_path.string().c_str(), error.empty() ? "unreadable" : error.c_str());
      return 1;
    }
    if (!have_gate) {
      if (lap_json_path.empty()) lap_json_path = line_path.parent_path() / "lap.json";
      if (!slurp(lap_json_path, json_text) || !drive::parse_gate(json_text, gate, &error)) {
        std::fprintf(stderr, "drive: %s: %s (or pass --gate x z nx nz)\n", lap_json_path.string().c_str(),
                     error.empty() ? "unreadable" : error.c_str());
        return 1;
      }
    }
    if (!have_spawn) {
      spawn[0] = line.start.x;
      spawn[1] = line.start.y;
      spawn[2] = line.start.z;
      spawn[3] = line.start.yaw_deg * 3.14159265358979323846 / 180;
      have_spawn = true;
    }
    if (!seconds_given) seconds = settle + (line.race_seconds > 0 ? 2 * line.race_seconds : 120);
    std::printf("oracle line: %zu samples, grid %.3f %.3f %.3f yaw %.3f deg, race time %.3f s; gate %.3f %.3f "
                "towards %.5f %.5f\n",
                line.samples.size(), line.start.x, line.start.y, line.start.z, line.start.yaw_deg,
                line.race_seconds, gate.x, gate.z, gate.nx, gate.nz);
  }

  std::vector<std::string> physics_notes;
  // With --world <game>/media/tracks/colorado the disc is there for what a
  // bake lacks (the car's MAXData collision spheres).
  if (!world_dir.empty()) {
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    drive::set_disc_physics_root(world_dir.parent_path().parent_path().parent_path(),
                                 state ? std::filesystem::path(state) : std::filesystem::path());
  }
  const drive::CarParams params = drive::params_for_baked_car(bake.car, bake_dir, &physics_notes);
  for (const auto& n : physics_notes) std::fprintf(stderr, "drive: physics: %s\n", n.c_str());
  drive::Vehicle v(params);
  const drive::FlatGround flat;
  const drive::Ground* ground = &flat;
  const float gh = 5000, gy = float(grade * 5000);
  const drive::MeshGround plane({{-gh, -gy, -gh}, {gh, gy, gh}, {gh, -gy, -gh}, {-gh, -gy, -gh}, {-gh, gy, gh}, {gh, gy, gh}});
  if (have_grade) ground = &plane;
  world::World colorado;
  world::Collision collision;
  std::unique_ptr<world::Streamer> streamer;
  if (!world_dir.empty()) {
    if (cache_path.empty()) cache_path = fs::path("out") / "world" / "colorado.index";
    if (!colorado.open(world_dir, cache_path, &error)) {
      std::fprintf(stderr, "drive: %s\n", error.c_str());
      return 1;
    }
    if (!error.empty()) std::fprintf(stderr, "drive: note: %s\n", error.c_str());
    if (!render_ground) {
      std::string warning;
      if (!collision.open(world_dir, world_dir.parent_path().parent_path() / "physics.zip", &error, &warning)) {
        std::fprintf(stderr, "drive: collision: %s\n", error.c_str());
        return 1;
      }
      if (!warning.empty()) std::fprintf(stderr, "drive: note: %s", warning.c_str());
    }
    streamer = std::make_unique<world::Streamer>(colorado, world::GatherOptions{}, 200,
                                                 render_ground ? nullptr : &collision);
    const double sx = have_spawn ? spawn[0] : near_given ? near_x : world::kSpawnX;
    const double sz = have_spawn ? spawn[2] : near_given ? near_z : world::kSpawnZ;
    if (!streamer->update(sx, sz, nullptr, &error)) {
      std::fprintf(stderr, "drive: %s\n", error.c_str());
      return 1;
    }
    const auto& st = streamer->stats();
    std::printf("world: %zu models indexed, %zu prop placements; gathered %zu world-space models, %zu props, "
                "%zu triangles; ground %s: %zu triangles (BVH depth %d)\n",
                colorado.models().size(), colorado.placement_count(), st.world_models, st.props,
                streamer->mesh().triangles(),
                streamer->uses_collision() ? "the collision tiles" : "render triangles (stand-in)",
                streamer->mesh_ground().triangle_count(), streamer->mesh_ground().depth());
    if (materials) {
      std::vector<std::pair<size_t, std::string>> rows;
      for (const auto& [name, n] : st.materials) rows.push_back({n, name});
      std::sort(rows.rbegin(), rows.rend());
      for (const auto& [n, name] : rows)
        std::printf("material %-48s %8zu triangles  %s\n", name.c_str(), n,
                    world::surface_name(world::classify("", name)));
    }
    if (materials) {
      std::vector<std::pair<size_t, std::string>> rows;
      for (const auto& [name, n] : st.object_names) rows.push_back({n, name});
      std::sort(rows.rbegin(), rows.rend());
      for (const auto& [n, name] : rows)
        std::printf("object %-56s %8zu triangles  %s\n", name.c_str(), n,
                    world::surface_name(world::classify(name, "")));
    }
    if (have_probe && streamer->uses_collision()) {
      // The collision triangles within 4 m of the point.
      world::CollisionMesh cm;
      if (!collision.gather(probe[0], probe[2], 50, cm, &error)) return 1;
      for (size_t t = 0; t < cm.triangles(); ++t) {
        double c[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k)
          for (int i = 0; i < 3; ++i) c[i] += cm.positions[t * 3 + k][size_t(i)] / 3;
        if (std::hypot(c[0] - probe[0], c[2] - probe[2]) > 4 || std::fabs(c[1] - probe[1]) > 4) continue;
        const auto& a0 = cm.positions[t * 3];
        const auto& b0 = cm.positions[t * 3 + 1];
        const auto& c0 = cm.positions[t * 3 + 2];
        const double e1[3] = {b0[0] - a0[0], b0[1] - a0[1], b0[2] - a0[2]};
        const double e2[3] = {c0[0] - a0[0], c0[1] - a0[1], c0[2] - a0[2]};
        double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (double& x : n) x /= len > 0 ? len : 1;
        std::printf("probe: centroid %.2f %.2f %.2f normal %.2f %.2f %.2f %s tile %u word %04x cat %02x %s\n", c[0],
                    c[1], c[2], n[0], n[1], n[2], collision.surface_name(cm.surface[t]).c_str(), cm.tile[t],
                    t < cm.word.size() ? cm.word[t] : 0, cm.flags[t],
                    world::Collision::collides(cm, t) ? "collides" : "off");
      }
    }
    world::Spawn s;
    const char* how = "";
    const double near[2] = {near_x, near_z};
    if (!world::choose_spawn(*streamer, have_spawn ? spawn : nullptr, near_given ? near : nullptr,
                             near_given ? nullptr : &near_heading, s, &how)) {
      std::fprintf(stderr, "drive: no road triangle near %.1f %.1f\n", near_x, near_z);
      return 1;
    }
    drive::Hit under;
    streamer->ground().raycast({s.position[0], s.position[1] + 1, s.position[2]}, {0, -1, 0}, 3, under);
    std::printf("spawn %s: %.3f %.3f %.3f heading %.4f rad, on %s\n", how, s.position[0], s.position[1],
                s.position[2], s.heading,
                streamer->uses_collision() ? collision.surface_name(under.surface).c_str() : "the render mesh");
    v.reset({s.position[0], s.position[1], s.position[2]}, s.heading);
    ground = &streamer->ground();
  }
  if (ai_cars > 0 && lap && streamer) {
    const drive::RacingLine racing = drive::RacingLine::from_oracle(line);
    const size_t n = size_t(ai_cars);
    drive::AiField field(racing, n);
    std::vector<drive::Vehicle> cars;
    cars.reserve(n);
    constexpr float kLanes[3] = {0.0f, 2.5f, -2.5f};
    for (size_t i = 0; i < n; ++i) {
      cars.emplace_back(params);
      field.place(i, 5.0 + 15.0 * double(i), kLanes[i % 3], 0.99f - 0.01f * float(i % 8));
      double x = 0, z = 0, h = 0, y = line.start.y;
      field.start_pose(i, x, z, h);
      double gy = y;
      if (world::ground_height(streamer->ground(), x, z, y + 20, gy)) y = gy;
      cars[i].reset({x, y + 0.5, z}, h);
    }
    std::vector<drive::AiCarState> states(n);
    std::vector<drive::Input> inputs(n);
    const int steps = int(std::lround(seconds / drive::Vehicle::kStep));
    std::vector<double> ai_us, phys_us;
    ai_us.reserve(size_t(steps));
    phys_us.reserve(size_t(steps));
    double next_report = 10, next_stream = 1;
    using clock = std::chrono::steady_clock;
    for (int s = 0; s < steps; ++s) {
      const auto t0 = clock::now();
      for (size_t i = 0; i < n; ++i) states[i] = drive::ai_state(cars[i]);
      field.drive(states, inputs);
      const auto t1 = clock::now();
      for (size_t i = 0; i < n; ++i) cars[i].step(inputs[i], streamer->ground());
      const auto t2 = clock::now();
      ai_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
      phys_us.push_back(std::chrono::duration<double, std::micro>(t2 - t1).count());
      const double t = (s + 1) * drive::Vehicle::kStep;
      if (t >= next_stream) {
        // The collision around the pack's middle (one gather for every car).
        next_stream += 1;
        double cx = 0, cz = 0;
        for (const auto& c : cars) cx += c.position().x / double(n), cz += c.position().z / double(n);
        if (!streamer->update(cx, cz, nullptr, &error)) {
          std::fprintf(stderr, "drive: %s\n", error.c_str());
          return 1;
        }
      }
      if (t >= next_report) {
        next_report += 10;
        std::printf("t %.0f s:", t);
        for (size_t i = 0; i < n; ++i)
          std::printf(" [%zu %.0f m %.0f km/h %+.1f]", i, field.progress(i), cars[i].speed() * 3.6, field.cross_track(i));
        std::printf("\n");
      }
    }
    auto stats = [](std::vector<double> v, const char* what, size_t cars_n) {
      std::sort(v.begin(), v.end());
      double sum = 0;
      for (double x : v) sum += x;
      const double mean = v.empty() ? 0 : sum / double(v.size());
      std::printf("%s per 120 Hz step: mean %.1f us, p50 %.1f, p99 %.1f, max %.1f (%.2f us a car; %.3f ms a 60 Hz "
                  "frame)\n",
                  what, mean, v.empty() ? 0 : v[v.size() / 2], v.empty() ? 0 : v[v.size() * 99 / 100],
                  v.empty() ? 0 : v.back(), mean / double(cars_n), 2 * mean / 1000);
    };
    std::printf("ai: %zu cars on the oracle line (%.0f m, %zu points), %.0f s\n", n, racing.length(), racing.size(),
                seconds);
    stats(ai_us, "ai pass", n);
    stats(phys_us, "physics", n);
    double worst_cross = 0;
    int finished = 0;
    for (size_t i = 0; i < n; ++i) {
      worst_cross = std::max(worst_cross, double(std::fabs(field.cross_track(i))));
      finished += field.progress(i) >= racing.length() - 1;
    }
    std::printf("ai: %d of %zu at the line's end; last cross-track up to %.1f m\n", finished, n, worst_cross);
    return 0;
  }
  if (lap) {
    // SPEC-CAR-PHYSICS, "Oracle lap for the M3 gate".
    drive::LineFollower::Options fo;
    fo.lookahead_s = lookahead_s;
    if (const char* k = std::getenv("SHIFT_LAP_CROSS_GAIN")) fo.cross_gain = std::atof(k);
    // SHIFT_LAP_STEER=pursuit: the old pure-pursuit follower.
    if (const char* k = std::getenv("SHIFT_LAP_STEER")) fo.slip_rule = std::string(k) != "pursuit";
    const bool live_dirt = std::getenv("SHIFT_LAP_DIRT_REPLAY") == nullptr;
    drive::LineFollower follower(line, fo);
    drive::GateTimer timer(gate, follower.length() / 2);
    std::ofstream out;
    if (!physics_trace_path.empty()) out.open(physics_trace_path, std::ios::binary);
    const int steps = int(std::lround(seconds / drive::Vehicle::kStep));
    long long last_frame = LLONG_MIN;
    double distance = 0, t100 = -1, top = 0, max_cross = 0, sum_cross2 = 0, finish_speed = 0;
    int lap_steps = 0, saturated = 0, stm_steps = 0;
    double tcs_peak = 0;
    std::map<int, long long> surfaces;
    std::vector<double> ours_at;  // our race time at each 100 m of the oracle's path
    drive::Vec3 prev = v.position();
    double prev_t = -settle;
    bool finished = false;
    double after_finish = 0;
    for (int s = 0; s < steps; ++s) {
      const bool racing = v.time() >= settle - 1e-9;
      // Held on the handbrake until the start signal (the brake at rest
      // selects reverse).
      drive::Input in{0, 0, 0, true};
      if (racing) {
        v.set_ai_controls(true);  // the oracle lap is the AI driving (SPEC-CAR-PHYSICS)
        in = follower.input(v);
        // The AI's grip factor on dirt from the car's own wheels (CAR_PHYSICS,
        // grip on dirt; Q40: replaying the oracle's by distance leaves our
        // car at 1 where only ours is on dirt). SHIFT_LAP_DIRT_REPLAY=1
        // replays the oracle's file instead.
        if (live_dirt) {
          v.set_ai_dirt_grip(true);
        } else {
          double gf = 1, gr = 1;
          follower.grip_at(follower.progress(), gf, gr);
          v.set_ai_grip(gf, gr);
        }
      }
      v.step(in, *ground);
      if (v.time() > settle) {
        tcs_peak = std::max(tcs_peak, v.tcs_amount());
        if (v.stm_active()) ++stm_steps;
      }
      const double t = v.time() - settle;
      const drive::Vec3 at = v.position();
      if (streamer) {
        bool changed = false;
        if (!streamer->update(at.x, at.z, &changed, &error)) {
          std::fprintf(stderr, "drive: %s\n", error.c_str());
          return 1;
        }
        if (changed) ground = &streamer->ground();
      }
      if (t > 0) {
        distance += std::sqrt((at.x - prev.x) * (at.x - prev.x) + (at.y - prev.y) * (at.y - prev.y) +
                              (at.z - prev.z) * (at.z - prev.z));
        if (!finished) {
          ++lap_steps;
          const double c = std::fabs(follower.cross_track());
          max_cross = std::max(max_cross, c);
          sum_cross2 += c * c;
          saturated += follower.saturated();
          for (const auto& w : v.wheels())
            if (w.contact) ++surfaces[w.surface];
          if (t100 < 0 && v.speed() >= 100 / 3.6) t100 = t;
          top = std::max(top, v.speed());
          while (follower.progress() >= 100.0 * double(ours_at.size() + 1)) ours_at.push_back(t);
          if (timer.update(prev_t, prev.x, prev.z, t, at.x, at.z, follower.progress())) {
            finished = true;
            finish_speed = v.speed();
          }
        } else {
          after_finish += drive::Vehicle::kStep;
        }
      }
      if (out.is_open()) {
        const long long frame = std::llround(t * 60);
        if (frame != last_frame) out << drive::lap_trace_event(v, bake.car.car_id, t, distance) << "\n";
        last_frame = frame;
      }
      prev = at;
      prev_t = t;
      if (finished && after_finish >= 1.0) break;
    }
    if (out.is_open()) out << drive::race_trace_event(finished, timer.time()) << "\n";
    std::printf("car %s, run lap, %d lap steps (SPEC-CAR-PHYSICS, oracle lap)\n", bake.car.media_name.c_str(),
                lap_steps);
    if (finished) {
      const double ref = line.race_seconds;
      std::printf("race time: %.3f s (oracle %.3f s, %+.2f %%: %s the 2 %% gate), crossing at %.1f km/h\n",
                  timer.time(), ref, ref > 0 ? 100 * (timer.time() - ref) / ref : 0.0,
                  ref > 0 && std::fabs(timer.time() - ref) <= 0.02 * ref ? "within" : "outside",
                  finish_speed * 3.6);
    } else {
      std::printf("race time: none (DNF): progress %.1f of %.1f m after %.1f s\n", follower.progress(),
                  follower.length(), v.time() - settle);
    }
    std::printf("0-100 km/h: %.2f s, top speed %.1f km/h, distance %.1f m (oracle path %.1f m)\n", t100, top * 3.6,
                distance, follower.length());
    std::printf("line: cross-track max %.2f m, rms %.2f m; steer past full lock on %d of %d steps\n", max_cross,
                lap_steps ? std::sqrt(sum_cross2 / lap_steps) : 0.0, saturated, lap_steps);
    // SPEC-CAR-PHYSICS: under the title's AI the lap's TCS amount peaked at
    // 0.064 and STM never acted.
    std::printf("assists: TCS amount peak %.3f, STM on %d steps\n", tcs_peak, stm_steps);
    long long wheel_samples = 0;
    for (const auto& [k, n] : surfaces) wheel_samples += n;
    for (const auto& [k, n] : surfaces)
      std::printf("surface %d (%s): %.1f %% of wheel samples\n", k,
                  streamer && streamer->uses_collision() ? collision.surface_name(k).c_str() : "-",
                  wheel_samples ? 100.0 * double(n) / double(wheel_samples) : 0.0);
    size_t k = 0;
    for (const auto& sample : line.samples) {
      if (sample.t < 0) continue;
      while (k < ours_at.size() && sample.distance >= 100.0 * double(k + 1)) {
        std::printf("at %4.0f m: oracle %6.2f s, ours %6.2f s (%+.2f s)\n", 100.0 * double(k + 1), sample.t,
                    ours_at[k], ours_at[k] - sample.t);
        ++k;
      }
    }
    return finished ? 0 : 3;
  }

  const drive::Vec3 start = v.position();
  const bool cruise = run == "cruise" && script_path.empty();
  if (cruise && !streamer) {
    std::fprintf(stderr, "drive: --run cruise needs --world\n");
    return 2;
  }
  world::RoadFollower follower(cruise_kmh / 3.6);
  if (streamer) follower.rebuild(streamer->mesh());
  if (streamer) follower.set_ground(&streamer->ground());
  std::ofstream trace;
  if (!trace_path.empty()) {
    trace.open(trace_path, std::ios::binary);
    trace << drive::trace_header() << "\n";
  }
  std::ofstream physics_trace;
  if (!physics_trace_path.empty()) physics_trace.open(physics_trace_path, std::ios::binary);
  long long last_frame = -1;
  const int steps = int(std::lround(seconds / drive::Vehicle::kStep));
  double t100 = -1, top = 0, brake_start = -1;
  drive::Vec3 brake_from{};
  bool braking = false;
  int recoveries = 0;
  for (int s = 0; s < steps; ++s) {
    drive::Input in = cruise ? follower.input(v) : drive::script_input(script, v.time());
    if (run == "brake" && script_path.empty()) {
      if (!braking && v.speed() >= 100 / 3.6) {
        braking = true;
        brake_start = v.time();
        brake_from = v.position();
      }
      if (braking) in = {0, 0, 1, false};
    }
    v.step(in, *ground);
    // A cruise puts a stuck or rolled car back on the nearest road.
    if (cruise && follower.stuck(v, in)) {
      const drive::Vec3 at = v.position();
      world::Spawn back;
      if (world::recover(*streamer, v, &back)) {
        ++recoveries;
        std::printf("t %.2f s: stuck at %.1f %.1f %.1f, put back on the road %.1f m away\n", v.time(), at.x, at.y,
                    at.z, back.distance);
      }
    }
    if (streamer) {
      bool changed = false;
      const drive::Vec3 at = v.position();
      if (!streamer->update(at.x, at.z, &changed, &error)) {
        std::fprintf(stderr, "drive: %s\n", error.c_str());
        return 1;
      }
      if (changed) {
        std::printf("t %.2f s: gathered again around %.1f %.1f (%zu triangles)\n", v.time(), at.x, at.z,
                    streamer->mesh().triangles());
        ground = &streamer->ground();
        follower.rebuild(streamer->mesh());
        follower.set_ground(&streamer->ground());
      }
    }
    if (t100 < 0 && v.speed() >= 100 / 3.6) t100 = v.time();
    top = std::max(top, v.speed());
    if (trace) trace << drive::trace_row(v) << "\n";
    if (physics_trace.is_open()) {
      const long long frame = std::llround(v.time() * 60);
      if (frame != last_frame && frame % physics_every == 0)
        physics_trace << drive::physics_trace_event(v, bake.car.car_id, 60) << "\n";
      last_frame = frame;
    }
    if (braking && std::fabs(v.speed()) < 0.05) break;
  }
  const drive::Vec3 p = v.position();
  std::printf("car %s, run %s, %.2f s simulated (SPEC-CAR-PHYSICS)\n", bake.car.media_name.c_str(),
              script_path.empty() ? run.c_str() : script_path.string().c_str(), v.time());
  if (t100 > 0) std::printf("0-100 km/h: %.2f s\n", t100);
  std::printf("top speed: %.1f km/h, final speed %.1f km/h, gear %d, %.0f rpm\n", top * 3.6, v.speed() * 3.6,
              v.gear(), v.rpm());
  if (braking) {
    const double dx = p.x - brake_from.x, dz = p.z - brake_from.z;
    std::printf("stopping distance from 100 km/h: %.1f m in %.2f s\n", std::sqrt(dx * dx + dz * dz),
                v.time() - brake_start);
  }
  if (cruise) std::printf("recoveries: %d\n", recoveries);
  std::printf("position %.3f %.3f %.3f (moved %.1f m, height change %.2f m)\n", p.x, p.y, p.z,
              std::hypot(p.x - start.x, p.z - start.z), p.y - start.y);
  for (int i = 0; i < drive::kWheels; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    std::printf("wheel %d centre y %.4f (rest %.4f), load %.0f N, contact %d\n", i, w.centre.y,
                params.wheel_centres[size_t(i)].y, w.load, int(w.contact));
  }
  return 0;
}
