// drive: runs the engine's car physics headless with a scripted input and
// writes a trace (M3): on flat ground, or with --world on Colorado's
// collision (COLLISION.md `.fiz` tiles).
//
//   drive --bake <dir> --car <media name> [--script <file> | --run <name>]
//         [--seconds N] [--trace <file.csv>]
//         [--world <colorado dir> [--cache <index file>] [--near x z]
//          [--spawn x y z heading] [--materials] [--render-ground]]
//
// --world streams the collision and the render models around the car and
// starts at COLLISION.md's measured free-roam spawn by the festival, or at
// the road triangle nearest --near (x, z in the game frame, the file frame
// with z negated), or at --spawn; the car is dropped onto the collision
// below the point. --render-ground drives on the earlier stand-in (the
// render triangles) instead of the collision.
// --grade G drives on a plane mesh (drive::MeshGround) rising G (rise over
// run) towards +z, instead of the flat ground.
// --run cruise (with --world) drives itself along the road at --speed km/h
// (default 40), with libs/world's test driver.
// --materials prints the triangles per material name of the first gather
// with our surface class.
//
// Built-in runs: rest (no input), launch (full throttle), brake (full
// throttle to 100 km/h, then full brake), circle (part throttle, half
// steer right), reverse (brake held from rest). Prints a summary: the
// 0-100 km/h time, the top speed, the stopping distance from 100 km/h and
// the rest pose. All physics constants are placeholders (libs/drive).

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "cars/bake_format.h"
#include "cars/sources.h"
#include "drive/from_bake.h"
#include "drive/mesh_ground.h"
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
};

}  // namespace

int main(int argc, char** argv) {
  fs::path bake_dir, script_path, trace_path, world_dir, cache_path;
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
    else if (a == "--seconds") seconds = std::stod(next());
    else if (a == "--trace") trace_path = next();
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

  const drive::CarParams params = drive::params_from_bake(bake.car);
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
        std::printf("probe: centroid %.2f %.2f %.2f normal %.2f %.2f %.2f %s\n", c[0], c[1], c[2], n[0], n[1],
                    n[2], collision.surface_name(cm.surface[t]).c_str());
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
    if (braking && std::fabs(v.speed()) < 0.05) break;
  }
  const drive::Vec3 p = v.position();
  std::printf("car %s, run %s, %.2f s simulated (placeholder physics)\n", bake.car.media_name.c_str(),
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
