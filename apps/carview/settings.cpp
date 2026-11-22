#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>
#include <map>

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
    {"hour", true, "hour"},
    {"no-collectibles", false, "collectibles"},
    {"play", true, "play"},
    {"no-mods", false, "mods"},
    {"mods", true, "mods"},
    {"language", true, "language"},
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
};

const Kept* kept(const std::string& key) {
  for (const Kept& k : kKept)
    if (key == k.key) return &k;
  return nullptr;
}

std::filesystem::path user_folder() {
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

std::filesystem::path default_mods_root() {
  const auto u = user_folder();
  return u.empty() ? u : u / "mods";
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
