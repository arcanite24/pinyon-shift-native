#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>
#include <map>

#include "platform_support.h"

namespace carview {
namespace {

struct Kept {
  const char* key;
  bool value;        // takes a value
  const char* group; // options that set the same thing: the last one stays
};
constexpr Kept kKept[] = {
    {"preset", true, "preset"},
    {"size", true, "size"},
    {"msaa", true, "msaa"},
    {"shadows", true, "shadows"},
    {"bloom", false, "bloom"},
    {"no-bloom", false, "bloom"},
    {"no-post", false, "post"},
    {"exposure", true, "exposure"},
    {"no-sound", false, "sound"},
    {"no-radio", false, "radio"},
    {"traffic", false, "traffic"},
    {"no-traffic", false, "traffic"},
    {"difficulty", true, "difficulty"},
    {"no-rubberband", false, "rubberband"},
    {"camera", true, "camera"},
    {"camera-effects", false, "camera-effects"},
    {"no-camera-effects", false, "camera-effects"},
    {"rumble", false, "rumble"},
    {"no-rumble", false, "rumble"},
    {"rumble-scale", true, "rumble-scale"},
    {"controller-layout", true, "controller-layout"},
    {"steer-dz-in", true, "steer-dz-in"},
    {"steer-dz-out", true, "steer-dz-out"},
    {"throttle-dz-in", true, "throttle-dz-in"},
    {"throttle-dz-out", true, "throttle-dz-out"},
    {"brake-dz-in", true, "brake-dz-in"},
    {"brake-dz-out", true, "brake-dz-out"},
    {"keys", true, "keys"},
    {"assist-braking", true, "assist-braking"},
    {"assist-steering", true, "assist-steering"},
    {"assist-tcs", true, "assist-tcs"},
    {"assist-stm", true, "assist-stm"},
    {"assist-shifting", true, "assist-shifting"},
    {"assist-line", true, "assist-line"},
    {"assist-rewind", true, "assist-rewind"},
    {"damage", true, "damage"},
    {"hour", true, "hour"},
    {"day-length", true, "day-length"},
    {"tod-preset", true, "tod-preset"},
    {"no-collectibles", false, "collectibles"},
    {"play", true, "play"},
    {"no-mods", false, "mods"},
    {"mods", true, "mods"},
    {"language", true, "language"},
    {"skip-intro-movies", false, "skip-intro-movies"},
    {"no-skip-intro-movies", false, "skip-intro-movies"},
    {"start-in-single-player", false, "start-in-single-player"},
    {"no-start-in-single-player", false, "start-in-single-player"},
    {"skip-vo-with-a", false, "skip-vo-with-a"},
    {"no-skip-vo-with-a", false, "skip-vo-with-a"},
    {"master-volume", true, "master-volume"},
    {"mute", false, "mute"},
    {"full-dynamic-range", false, "full-dynamic-range"},
    {"no-radio-dj", false, "radio-dj"},
    {"radio-volume", true, "radio-volume"},
    {"mix-engine", true, "mix-engine"},
    {"mix-other-cars", true, "mix-other-cars"},
    {"mix-tyres", true, "mix-tyres"},
    {"mix-wind", true, "mix-wind"},
    {"mix-ambience", true, "mix-ambience"},
    {"mix-music", true, "mix-music"},
    {"mix-ui", true, "mix-ui"},
    {"mix-vo", true, "mix-vo"},
    {"your-music", false, "your-music"},
    {"your-music-shuffle", false, "your-music-shuffle"},
    {"your-music-volume", true, "your-music-volume"},
    {"photo-through-walls", false, "photo-through-walls"},
    {"photo-wider-limits", false, "photo-wider-limits"},
    {"no-motion-blur", false, "motion-blur"},
    {"no-dof", false, "dof"},
    {"ai-mode", true, "ai-mode"},
    {"treasure-map", false, "treasure-map"},
    {"no-satnav", false, "satnav"},
    // Slice 12: Racing wheels / Separate Devices / DirectInput FFB.
    {"wheel-sat", true, "wheel-sat"},
    {"wheel-dz", true, "wheel-dz"},
    {"wheel-curve", true, "wheel-curve"},
    {"wheel-pedal-dz", true, "wheel-pedal-dz"},
    {"wheel-separate", false, "wheel-separate"},
    {"wheel-900-linear", false, "wheel-900-linear"},
    {"wheel-steering", true, "wheel-steering"},
    {"wheel-throttle", true, "wheel-throttle"},
    {"wheel-brake", true, "wheel-brake"},
    {"wheel-clutch", true, "wheel-clutch"},
    {"wheel-handbrake", true, "wheel-handbrake"},
    {"wheel-buttons", true, "wheel-buttons"},
    {"wheel-ffb-mode", true, "wheel-ffb-mode"},
    {"wheel-ffb-strength", true, "wheel-ffb-strength"},
    {"wheel-ffb-invert", false, "wheel-ffb-invert"},
    {"wheel-ffb-engine-rumble", true, "wheel-ffb-engine-rumble"},
    {"wheel-ffb-road-rumble", true, "wheel-ffb-road-rumble"},
    {"wheel-ffb-trace", false, "wheel-ffb-trace"},
    // Slice 12: DualSense & Gyro Tilt Steering.
    {"ds-triggers", true, "ds-triggers"},
    {"ds-trigger-strength", true, "ds-trigger-strength"},
    {"ds-gear-click", false, "ds-gear-click"},
    {"ds-lightbar", true, "ds-lightbar"},
    {"no-ds-player-leds", false, "ds-player-leds"},
    {"gyro-steering", false, "gyro-steering"},
    {"gyro-sensitivity", true, "gyro-sensitivity"},
    // Slice 12: OpenTrack Head Tracking, Camera Editor & Immersive Camera.
    {"head-tracking", false, "head-tracking"},
    {"head-tracking-port", true, "head-tracking-port"},
    {"head-tracking-invert", true, "head-tracking-invert"},
    {"chase-height", true, "chase-height"},
    {"chase-distance", true, "chase-distance"},
    {"chase-fov", true, "chase-fov"},
    {"chase-stiffness", true, "chase-stiffness"},
    {"driving-view", true, "driving-view"},
    {"immersive-camera", true, "immersive-camera"},
    {"immersive-gforce", true, "immersive-gforce"},
    {"immersive-lean", true, "immersive-lean"},
    {"immersive-shake", true, "immersive-shake"},
    {"immersive-head", true, "immersive-head"},
    // Slice 12: UDP Telemetry Out.
    {"telemetry-udp", false, "telemetry-udp"},
    {"telemetry-udp-format", true, "telemetry-udp-format"},
    {"telemetry-udp-target", true, "telemetry-udp-target"},
    {"telemetry-udp-moving-only", false, "telemetry-udp-moving-only"},
    {"telemetry-probe", false, "telemetry-probe"},
    // Slice 12: F10 Trainer, Festival Pacing & Hidden Cars.
    {"trainer-cheats", false, "trainer-cheats"},
    {"trainer-saved-road", true, "trainer-saved-road"},
    {"festival-pacing", false, "festival-pacing"},
    {"pacing-prices", true, "pacing-prices"},
    {"pacing-rewards", true, "pacing-rewards"},
    {"hidden-cars", false, "hidden-cars"},
    // Slice 13B: Android Touch Controls & Turnip driver selection.
    {"touch-controls", true, "touch-controls"},
    {"touch-steer", true, "touch-steer"},
    {"android-gpu-driver", true, "android-gpu-driver"},
    // Slice 13A: Modding & Save Ecosystem.
    {"sync-folder", true, "sync-folder"},
    {"device-name", true, "device-name"},
    {"profile", true, "profile"},
    {"mods-native", false, "mods-native"},
    // Slice 11: Modern Graphics & HDR.
    {"render-scale", true, "render-scale"},
    {"scene-540p", false, "scene-540p"},
    {"no-scene-540p", false, "scene-540p"},
    {"aa", true, "aa"},
    {"ssao", true, "ssao"},
    {"lut", true, "lut"},
    {"lut-strength", true, "lut-strength"},
    {"vrs", false, "vrs"},
    {"no-vrs", false, "vrs"},
    {"texture-cap", true, "texture-cap"},
    {"vsync", true, "vsync"},
    {"fps-cap", true, "fps-cap"},
    {"output", true, "output"},
    {"sharpening", true, "sharpening"},
    {"frame-gen", true, "frame-gen"},
    {"hdr", false, "hdr"},
    {"no-hdr", false, "hdr"},
    {"auto-hdr", false, "auto-hdr"},
    {"no-auto-hdr", false, "auto-hdr"},
    {"hdr-paper-white", true, "hdr-paper-white"},
    {"hdr-highlights", true, "hdr-highlights"},
    {"hdr-peak", true, "hdr-peak"},
    {"hdr-compare", false, "hdr-compare"},
    {"no-hdr-compare", false, "hdr-compare"},
    {"hud-scale", true, "hud-scale"},
    {"safe-zone", true, "safe-zone"},
    {"hud-16-9", false, "hud-16-9"},
    {"no-hud-16-9", false, "hud-16-9"},
};

const Kept* kept(const std::string& key) {
  for (const Kept& k : kKept)
    if (key == k.key) return &k;
  return nullptr;
}

std::filesystem::path user_folder() {
  if (const char* base = SDL_GetBasePath()) {
#if defined(__APPLE__)
    constexpr bool kIsMac = true;
#else
    constexpr bool kIsMac = false;
#endif
    const auto portable = resolve_portable_data_root(std::filesystem::path(base), kIsMac);
    if (!portable.empty()) return portable;
  }
  std::filesystem::path out;
  if (char* pref = SDL_GetPrefPath("shift-engine", "drive")) {
    out = std::filesystem::path(pref).parent_path().parent_path();  // ".../shift-engine/drive/" -> ".../shift-engine"
    SDL_free(pref);
  }
  return out;
}

}  // namespace

std::filesystem::path settings_path() {
  if (const char* e = std::getenv("SHIFT_SETTINGS"); e && *e) return e;
  const auto u = user_folder();
  return u.empty() ? u : u / "settings.txt";
}

std::filesystem::path default_user_dir() { return user_folder(); }

std::filesystem::path default_mods_root() {
  const auto u = user_folder();
  return u.empty() ? u : u / "mods";
}

std::filesystem::path default_photos_dir() {
  const auto u = user_folder();
  return u.empty() ? std::filesystem::path("photos") : u / "photos";
}

std::filesystem::path default_replays_dir() {
  const auto u = user_folder();
  return u.empty() ? std::filesystem::path("replays") : u / "replays";
}

std::filesystem::path default_achievements_path() {
  const auto u = user_folder();
  return u.empty() ? std::filesystem::path("achievements.txt") : u / "achievements.txt";
}

std::vector<std::string> read_settings(const std::filesystem::path& file, std::vector<std::string>* notes) {
  std::vector<std::string> out;
  std::ifstream in(file);
  if (!in) return out;
  int n = 0;
  for (std::string line; std::getline(in, line);) {
    ++n;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t s = line.find_first_not_of(" \t");
    if (s == std::string::npos || line[s] == '#') continue;
    line = line.substr(s);
    if (line.rfind("--", 0) == 0) line = line.substr(2);
    const size_t sp = line.find(' ');
    const std::string key = line.substr(0, sp);
    const std::string value = sp == std::string::npos ? "" : line.substr(line.find_first_not_of(' ', sp));
    const Kept* k = kept(key);
    if (!k) {
      if (notes) notes->push_back(file.string() + ":" + std::to_string(n) + ": not a setting (" + key + ")");
      continue;
    }
    if (k->value == value.empty()) {
      if (notes)
        notes->push_back(file.string() + ":" + std::to_string(n) + ": " + key + (k->value ? " needs a value" : " takes no value"));
      continue;
    }
    out.push_back("--" + key);
    if (k->value) out.push_back(value);
  }
  return out;
}

bool write_settings(const std::filesystem::path& file, const std::vector<std::string>& args, std::string* error) {
  // The last option of each group, in the order first seen.
  std::vector<std::string> order;
  std::map<std::string, std::string> line;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i].rfind("--", 0) != 0) continue;
    const Kept* k = kept(args[i].substr(2));
    if (!k) continue;
    std::string l = k->key;
    if (k->value) {
      if (i + 1 >= args.size()) break;
      l += " " + args[++i];
    }
    if (!line.count(k->group)) order.push_back(k->group);
    line[k->group] = l;
  }
  std::error_code ec;
  if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "cannot write " + file.string();
    return false;
  }
  out << "# shift-engine settings: one option per line, the command line wins (carview --help)\n";
  for (const auto& g : order) out << line[g] << "\n";
  return bool(out);
}

}  // namespace carview
