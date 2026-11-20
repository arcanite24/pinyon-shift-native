// drive: runs the engine's car physics headless on flat ground with a
// scripted input and writes a trace (M3, first slice: no collision yet).
//
//   drive --bake <dir> --car <media name> [--script <file> | --run <name>]
//         [--seconds N] [--trace <file.csv>]
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
#include <map>
#include <sstream>
#include <string>

#include "cars/bake_format.h"
#include "cars/sources.h"
#include "drive/from_bake.h"
#include "drive/vehicle.h"

namespace fs = std::filesystem;

namespace {

const std::map<std::string, std::string> kRuns = {
    {"rest", "0 0 0 0\n"},
    {"launch", "0 0 1 0\n"},
    {"brake", "0 0 1 0\n"},  // the brake is applied at 100 km/h by the run itself
    {"circle", "0 0 0 0\n1 0.5 0.35 0\n"},
    {"reverse", "0 0 0 1\n"},
};

}  // namespace

int main(int argc, char** argv) {
  fs::path bake_dir, script_path, trace_path;
  std::string car, run = "launch";
  double seconds = 20;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--bake") bake_dir = next();
    else if (a == "--car") car = next();
    else if (a == "--script") script_path = next();
    else if (a == "--run") run = next();
    else if (a == "--seconds") seconds = std::stod(next());
    else if (a == "--trace") trace_path = next();
    else {
      std::fprintf(stderr, "usage: drive --bake <dir> --car <name> [--script <file> | --run "
                           "rest|launch|brake|circle|reverse] [--seconds N] [--trace <csv>]\n");
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
  const drive::FlatGround ground;
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
    drive::Input in = drive::script_input(script, v.time());
    if (run == "brake" && script_path.empty()) {
      if (!braking && v.speed() >= 100 / 3.6) {
        braking = true;
        brake_start = v.time();
        brake_from = v.position();
      }
      if (braking) in = {0, 0, 1, false};
    }
    v.step(in, ground);
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
  std::printf("position %.3f %.3f %.3f\n", p.x, p.y, p.z);
  for (int i = 0; i < drive::kWheels; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    std::printf("wheel %d centre y %.4f (rest %.4f), load %.0f N\n", i, w.centre.y + p.y,
                params.wheel_centres[size_t(i)].y, w.load);
  }
  return 0;
}
