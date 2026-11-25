// Every free-roam cutscene on the player's disc: counts, the first-time
// career cutscenes' lengths, the fields the player treats as constant, and
// the framing convention (cams around a placed car keep it in view).
// Skipped (exit 77) without SHIFT_GAME_ROOT.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "cutscene/cutscene.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    ++g_failures;
  }
}

struct Expect {
  const char* name;
  double duration;
  int cams;
};

// The first-time career's cutscenes (first_time_career.xml and the intro flow).
constexpr Expect kCareer[] = {
    {"Opening_Cutscene", 39.0, 11},     {"NEW_Diner_Cutscene_1", 6.7, 2}, {"New_Diner_Cutscene_2", 59.0, 15},
    {"New_Diner_Cutscene_3", 2.5, 1},   {"Radio_Unlock", 8.7, 2},         {"Wristband_Intro", 33.5, 6},
    {"Festival_Race_Outro", 18.3, -1},
};

double angle_deg(const std::array<double, 3>& a, const std::array<double, 3>& b) {
  const double na = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  const double nb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  const double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (na * nb);
  return std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
}

}  // namespace

int main() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root || !*root) {
    std::printf("cutscene_corpus_tests: SHIFT_GAME_ROOT not set, skipping\n");
    return 77;
  }
  cutscene::Library lib;
  std::string err;
  if (!cutscene::load_freeroam_cutscenes(root, lib, &err)) {
    std::fprintf(stderr, "FAIL: load: %s\n", err.c_str());
    return 1;
  }

  int with_cams = 0, cams = 0, keys = 0, events = 0;
  std::map<std::string, int> by_type;
  for (const auto& cs : lib.cutscenes) {
    if (!cs.cams.empty()) ++with_cams;
    cams += int(cs.cams.size());
    events += int(cs.events.size());
    check(cs.declared_cams == int(cs.cams.size()), cs.name + ": NumCams matches the Cam elements");
    for (const auto& e : cs.events) ++by_type[cutscene::event_type_name(e.type)];
    for (const auto& c : cs.cams) {
      keys += int(c.keys.size());
      check(!c.keys.empty(), cs.name + ": every cam has keys");
      // The fields the player treats as constant (zero / AffectsPath on the disc).
      check(c.ease_in == 0.0 && c.ease_out == 0.0 && c.const_vel_speed == 0.0, cs.name + ": no ease / constant speed");
      check(c.time_anim_type == "AffectsPath", cs.name + ": TimeAnimType AffectsPath");
      for (const auto& k : c.keys) check(k.time_c0 == 0.0, cs.name + ": TimeC0 is zero");
      for (const auto* f : {&c.fade_in, &c.fade_out})
        check(!f->active() || (f->color[0] == 0 && f->color[1] == 0 && f->color[2] == 0), cs.name + ": fades are black");
    }
  }
  std::printf("cutscenes %zu, with cams %d, cams %d, keys %d, events %d\n", lib.cutscenes.size(), with_cams, cams, keys,
              events);
  for (const auto& [type, n] : by_type) std::printf("  %-16s %d\n", type.c_str(), n);
  check(lib.cutscenes.size() == 78, "78 cutscenes");
  check(with_cams == 75, "75 with cams");
  check(cams == 254, "254 cams");

  for (const auto& x : kCareer) {
    const auto* cs = lib.find(x.name);
    check(cs != nullptr, std::string(x.name) + " present");
    if (!cs) continue;
    std::printf("%-22s %6.2f s  %2zu cams  %3zu events  %s  loop-from %.2f\n", cs->name.c_str(), cs->duration,
                cs->cams.size(), cs->events.size(), cs->mode == cutscene::PlaybackMode::kLooping ? "Looping" : "Normal",
                cs->loop_from);
    check(std::fabs(cs->duration - x.duration) < 1e-3, std::string(x.name) + " duration");
    if (x.cams >= 0) check(int(cs->cams.size()) == x.cams, std::string(x.name) + " cam count");
    // The cuts tile the cutscene.
    double end = 0;
    for (const auto& c : cs->cams) end = std::max(end, c.end());
    if (!cs->cams.empty()) check(end >= cs->duration - 0.05, std::string(x.name) + " cams cover the duration");
  }
  if (const auto* op = lib.find("Opening_Cutscene")) {
    check(op->cams.size() > 0 && op->cams[0].fade_in.active() && std::fabs(op->cams[0].fade_in.duration - 2.0) < 1e-6 &&
              op->cams[0].fade_in.curve == cutscene::CurveType::kS,
          "Opening_Cutscene opens with a 2 s S-curve fade from black");
    bool flynt = false;
    for (const auto& e : op->events)
      if (e.type == cutscene::EventType::kCar && int(e.number("CarID")) == 1171) flynt = true;
    check(flynt, "Opening_Cutscene places Flynt's car (1171)");
  }
  if (const auto* wb = lib.find("Wristband_Intro")) {
    bool exit_vo = false;
    for (const auto& e : wb->events)
      if (e.type == cutscene::EventType::kVoiceOver && e.number("exitCutsceneOnFinish") == 1.0) exit_vo = true;
    check(exit_vo, "Wristband_Intro ends on its voice-over");
  }

  // Framing: cams relative to a camera node placed on the player car (or to
  // the car itself) should look at the car. Score the angle between each
  // key's forward and the direction to the car (0.6 m up) under the
  // library's convention.
  std::vector<double> angles;
  int inside = 0;
  for (const auto& cs : lib.cutscenes) {
    std::vector<std::array<double, 3>> cars;
    std::map<std::string, std::array<double, 3>> nodes;
    for (const auto& e : cs.events) {
      if (e.type == cutscene::EventType::kPlacePlayerCar) cars.push_back(e.position());
      if (e.type == cutscene::EventType::kCameraNode) nodes[e.name] = e.position();
    }
    for (const auto& c : cs.cams) {
      if (c.in_cockpit) continue;
      if (!c.target_name.empty()) {
        auto it = nodes.find(c.target_name);
        if (it == nodes.end() || cars.empty()) continue;
        double best = 1e9;
        for (const auto& p : cars)
          best = std::min(best, std::hypot(p[0] - it->second[0], p[1] - it->second[1], p[2] - it->second[2]));
        if (best > 1.5) continue;
      }
      for (const auto& k : c.keys) {
        if (std::hypot(k.pos[0], k.pos[2]) < 2.5) continue;
        cutscene::Pose p;
        p.pos = k.pos;
        p.yaw = k.yaw;
        p.pitch = k.pitch;
        p.fov = k.fov;
        const auto w = cutscene::to_world(p, cutscene::Frame{{0, 0, 0}, 0});
        const double a = angle_deg(w.forward, {-w.eye[0], 0.6 - w.eye[1], -w.eye[2]});
        angles.push_back(a);
        if (a < k.fov * 0.5 * 1.78) ++inside;
      }
    }
  }
  std::sort(angles.begin(), angles.end());
  const double median = angles.empty() ? 180.0 : angles[angles.size() / 2];
  const double frac = angles.empty() ? 0.0 : double(inside) / double(angles.size());
  std::printf("framing: %zu keys, median angle to the car %.1f deg, %.0f%% in frame\n", angles.size(), median,
              frac * 100.0);
  check(angles.size() > 100, "enough framing samples");
  check(median < 20.0 && frac > 0.75, "cams around the car keep it in frame (yaw right, pitch down)");

  if (g_failures) {
    std::fprintf(stderr, "cutscene_corpus_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("cutscene_corpus_tests: ok\n");
  return 0;
}
