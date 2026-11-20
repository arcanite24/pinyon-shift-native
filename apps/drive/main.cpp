// drive: runs the engine's car physics headless with a scripted input and
// writes a trace (M3): on flat ground, or with --world on Colorado's render
// triangles (the stand-in ground of libs/world until COLLISION.md).
//
//   drive --bake <dir> --car <media name> [--script <file> | --run <name>]
//         [--seconds N] [--trace <file.csv>]
//         [--world <colorado dir> [--cache <index file>] [--near x z]
//          [--spawn x y z heading] [--materials]]
//
// --world streams the models around the car and starts at a stand-in spawn:
// the road triangle nearest --near (x, z in the game frame, the file frame
// with z negated; default the free-roam spot of WORLD_GEOMETRY's frame
// dump, near the festival), or --spawn.
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
  bool near_given = false, have_spawn = false, materials = false;
  double cruise_kmh = 40, grade = 0;
  bool have_grade = false;
  double spawn[4] = {0, 0, 0, 0};
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
                           "[--spawn x y z heading] [--materials]]\n");
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
  std::unique_ptr<world::Streamer> streamer;
  if (!world_dir.empty()) {
    if (cache_path.empty()) cache_path = fs::path("out") / "world" / "colorado.index";
    if (!colorado.open(world_dir, cache_path, &error)) {
      std::fprintf(stderr, "drive: %s\n", error.c_str());
      return 1;
    }
    if (!error.empty()) std::fprintf(stderr, "drive: note: %s\n", error.c_str());
    streamer = std::make_unique<world::Streamer>(colorado, world::GatherOptions{});
    const double sx = have_spawn ? spawn[0] : near_x, sz = have_spawn ? spawn[2] : near_z;
    if (!streamer->update(sx, sz, nullptr, &error)) {
      std::fprintf(stderr, "drive: %s\n", error.c_str());
      return 1;
    }
    const auto& st = streamer->stats();
    std::printf("world: %zu models indexed, %zu prop placements; gathered %zu world-space models, %zu props, "
                "%zu triangles (%zu in the ground, BVH depth %d)\n",
                colorado.models().size(), colorado.placement_count(), st.world_models, st.props,
                streamer->mesh().triangles(), streamer->mesh_ground().triangle_count(),
                streamer->mesh_ground().depth());
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
    world::Spawn s;
    if (have_spawn) {
      s.position = {spawn[0], spawn[1], spawn[2]};
      s.heading = spawn[3];
    } else if (!world::find_spawn(streamer->mesh(), near_x, near_z, s, near_given ? nullptr : &near_heading)) {
      std::fprintf(stderr, "drive: no road triangle near %.1f %.1f\n", near_x, near_z);
      return 1;
    }
    std::printf("spawn (stand-in): %.3f %.3f %.3f heading %.4f rad, %.1f m from the search point\n",
                s.position[0], s.position[1], s.position[2], s.heading, s.distance);
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
  std::ofstream trace;
  if (!trace_path.empty()) {
    trace.open(trace_path, std::ios::binary);
    trace << drive::trace_header() << "\n";
  }
  const int steps = int(std::lround(seconds / drive::Vehicle::kStep));
  double t100 = -1, top = 0, brake_start = -1;
  drive::Vec3 brake_from{};
  bool braking = false;
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
  std::printf("position %.3f %.3f %.3f (moved %.1f m, height change %.2f m)\n", p.x, p.y, p.z,
              std::hypot(p.x - start.x, p.z - start.z), p.y - start.y);
  for (int i = 0; i < drive::kWheels; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    std::printf("wheel %d centre y %.4f (rest %.4f), load %.0f N, contact %d\n", i, w.centre.y,
                params.wheel_centres[size_t(i)].y, w.load, int(w.contact));
  }
  return 0;
}
