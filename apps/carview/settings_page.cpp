#include "settings_page.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

#include "race/rewards.h"
#include "settings.h"

namespace carview {
namespace {

SettingsPage::TextResolver& settings_text_resolver() {
  static SettingsPage::TextResolver fn;
  return fn;
}

std::string resolve_setting_loc(std::string_view table, std::string_view id, const char* fallback) {
  if (const auto& fn = settings_text_resolver()) {
    const std::string resolved = fn(table, id, fallback);
    if (!resolved.empty()) return resolved;
  }
  return fallback;
}

struct Kept {
  const char* key;
  bool value;         // takes a value
  const char* group;  // options that set the same thing: the last one stays
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
    {"switch-handbrake-clutch", false, "switch-handbrake-clutch"},
    {"no-switch-handbrake-clutch", false, "switch-handbrake-clutch"},
    {"hud-speedo", true, "hud-speedo"},
    {"minimap-mode", true, "minimap-mode"},
    // The original's HUD options, by their profile names (Options/Timers..).
    {"no-hud-timers", false, "hud-timers"},
    {"no-hud-wrong-way", false, "hud-wrong-way"},
    {"no-hud-names-over-cars", false, "hud-names-over-cars"},
    {"no-hud-popularity-skills", false, "hud-popularity-skills"},
    {"hud-tracked-achievement", false, "hud-tracked-achievement"},
    {"hud-barn-radar", false, "hud-barn-radar"},
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
    {"engine-steer-assist", false, "engine-steer-assist"},
    {"damage", true, "damage"},
    {"hour", true, "hour"},
    {"day-length", true, "day-length"},
    {"tod-preset", true, "tod-preset"},
    {"collectibles", false, "collectibles"},
    {"no-collectibles", false, "collectibles"},
    {"play", true, "play"},
    {"no-mods", false, "mods"},
    {"mods", true, "mods"},
    {"language", true, "language"},
    {"units", true, "units"},
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
    {"motion-blur", false, "motion-blur"},
    {"no-motion-blur", false, "motion-blur"},
    {"dof", false, "dof"},
    {"no-dof", false, "dof"},
    {"ai-mode", true, "ai-mode"},
    {"treasure-map", false, "treasure-map"},
    {"no-treasure-map", false, "treasure-map"},
    {"satnav", false, "satnav"},
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
    {"no-head-tracking", false, "head-tracking"},
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
    {"contact-shadows", true, "contact-shadows"},
    {"tonemapper", true, "tonemapper"},
    {"hex-tiling", false, "hex-tiling"},
    {"no-hex-tiling", false, "hex-tiling"},
    {"velocity-blur", true, "velocity-blur"},
    {"shadow-pcss", true, "shadow-pcss"},
    {"ssr", true, "ssr"},
    {"volumetric-fog", true, "volumetric-fog"},
    {"car-detail", false, "car-detail"},
    {"no-car-detail", false, "car-detail"},
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
    // Hardware Ray Tracing & Path Tracing (RT-1..RT-4).
    {"rt", true, "rt"},
    {"rt-bounces", true, "rt-bounces"},
    {"rt-spp", true, "rt-spp"},
    {"rt-denoise", false, "rt-denoise"},
    {"no-rt-denoise", false, "rt-denoise"},
    // Multi-backend RHI selection (auto|vulkan|metal|dx12).
    {"graphics_backend", true, "graphics_backend"},
    {"backend", true, "graphics_backend"},
};

const Kept* kept(const std::string& key) {
  for (const Kept& k : kKept)
    if (key == k.key) return &k;
  return nullptr;
}

bool parse_strict_number(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return false;
  out = v;
  return true;
}

bool token_values_equal(const std::string& a, const std::string& b) {
  if (a == b) return true;
  double da = 0.0, db = 0.0;
  if (parse_strict_number(a, da) && parse_strict_number(b, db)) {
    return std::fabs(da - db) < 1e-4;
  }
  return false;
}

bool is_control_or_assist_flag(const std::string& t) {
  return t == "--keys" || t == "--camera-effects" || t == "--no-camera-effects" || t == "--rumble" ||
         t == "--no-rumble" || t == "--rumble-scale" || t == "--controller-layout" || t == "--steer-dz-in" ||
         t == "--steer-dz-out" || t == "--throttle-dz-in" || t == "--throttle-dz-out" || t == "--brake-dz-in" ||
         t == "--brake-dz-out" || t == "--assist-braking" || t == "--assist-steering" || t == "--assist-tcs" ||
         t == "--assist-stm" || t == "--assist-shifting" || t == "--assist-line" || t == "--assist-rewind" ||
         t == "--engine-steer-assist";
}

struct PresetSotaDefaults {
  modern::SsaoQuality ssao = modern::SsaoQuality::kOff;
  modern::ContactShadows contact_shadows = modern::ContactShadows::kOff;
  bool hex_tiling = false;
  modern::VelocityBlur velocity_blur = modern::VelocityBlur::kOff;
  modern::ShadowPcss shadow_pcss = modern::ShadowPcss::kOff;
  modern::SsrQuality ssr = modern::SsrQuality::kOff;
  modern::VolumetricFog volumetric_fog = modern::VolumetricFog::kOff;
  bool car_detail = false;
};

bool preset_sota_defaults(const std::string& preset_raw, PresetSotaDefaults& out) {
  std::string p = preset_raw;
  for (char& ch : p) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  if (p == "ultra") {
    out = {modern::SsaoQuality::kUltra, modern::ContactShadows::kUltra, true, modern::VelocityBlur::kHigh,
           modern::ShadowPcss::kUltra,  modern::SsrQuality::kFull,      modern::VolumetricFog::kHalf, true};
    return true;
  }
  if (p == "high" || p == "quality-60" || p == "performance-120") {
    out = {modern::SsaoQuality::kHigh, modern::ContactShadows::kHigh, true, modern::VelocityBlur::kLow,
           modern::ShadowPcss::kHigh,  modern::SsrQuality::kHalf,     modern::VolumetricFog::kQuarter, true};
    return true;
  }
  if (p == "low" || p == "low-spec-60" || p == "balanced-40") {
    out = {modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kOff,
           modern::ShadowPcss::kLow,  modern::SsrQuality::kOff,     modern::VolumetricFog::kOff, false};
    return true;
  }
  if (p == "smooth-60" || p == "smooth_60") {
    out = {modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kOff,
           modern::ShadowPcss::kOff,  modern::SsrQuality::kOff,     modern::VolumetricFog::kOff, false};
    return true;
  }
  if (p == "quality-30" || p == "quality_30") {
    out = {modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kLow,
           modern::ShadowPcss::kLow,  modern::SsrQuality::kOff,     modern::VolumetricFog::kQuarter, false};
    return true;
  }
  if (p == "low-power" || p == "low_power" || p == "ultra-low-30") {
    out = {};
    return true;
  }
  return false;
}

std::vector<std::string> explicit_off_tokens_for_row(const char* name) {
  const std::string n(name);
  if (n == "ambient occlusion") return {"--ssao", "off"};
  if (n == "contact shadows") return {"--contact-shadows", "off"};
  if (n == "terrain hex-tiling") return {"--no-hex-tiling"};
  if (n == "velocity motion blur") return {"--velocity-blur", "off"};
  if (n == "shadow softness (pcss)") return {"--shadow-pcss", "off"};
  if (n == "screen-space reflections") return {"--ssr", "off"};
  if (n == "volumetric fog & beams") return {"--volumetric-fog", "off"};
  if (n == "car clearcoat detail") return {"--no-car-detail"};
  return {};
}

}  // namespace

bool is_settings_option(const std::string& arg, bool* takes_value) {
  if (arg.rfind("--", 0) != 0) return false;
  const Kept* k = kept(arg.substr(2));
  if (!k) return false;
  if (takes_value) *takes_value = k->value;
  return true;
}

namespace {
bool& settings_file_disabled_flag() {
  static bool disabled = false;
  return disabled;
}
}  // namespace

void set_settings_file_disabled(bool disabled) { settings_file_disabled_flag() = disabled; }
bool settings_file_disabled() { return settings_file_disabled_flag(); }

std::vector<std::string> read_settings(const std::filesystem::path& file, std::vector<std::string>* notes) {
  std::vector<std::string> out;
  if (settings_file_disabled()) return out;
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
    std::string value;
    if (sp != std::string::npos) {
      const size_t vs = line.find_first_not_of(" \t", sp);
      if (vs != std::string::npos) value = line.substr(vs);
    }
    const Kept* k = kept(key);
    if (!k) {
      if (notes) notes->push_back(file.string() + ":" + std::to_string(n) + ": not a setting (" + key + ")");
      continue;
    }
    if (k->value == value.empty()) {
      if (notes)
        notes->push_back(file.string() + ":" + std::to_string(n) + ": " + key +
                         (k->value ? " needs a value" : " takes no value"));
      continue;
    }
    out.push_back("--" + key);
    if (k->value) out.push_back(value);
  }
  return out;
}

bool write_settings(const std::filesystem::path& file, const std::vector<std::string>& args, std::string* error) {
  if (settings_file_disabled()) {
    if (error) *error = "settings are not saved in this run (--no-settings)";
    return false;
  }
  // The last option of each group, in the order first seen.
  std::vector<std::string> order;
  std::map<std::string, std::string> line;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i].rfind("--", 0) != 0) continue;
    const Kept* k = kept(args[i].substr(2));
    if (!k) continue;
    std::string l = (std::string(k->group) == "graphics_backend") ? std::string("graphics_backend") : std::string(k->key);
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

std::vector<SettingsPage::Row> SettingsPage::build() {
  auto mix_row = [](const char* name, const char* flag) -> Row {
    return {name,
            {{"100%", {}},
             {"125%", {flag, "125"}},
             {"150%", {flag, "150"}},
             {"175%", {flag, "175"}},
             {"200%", {flag, "200"}},
             {"0%", {flag, "0"}},
             {"25%", {flag, "25"}},
             {"50%", {flag, "50"}},
             {"75%", {flag, "75"}}}};
  };
  auto vol_row = [](const char* name, const char* flag) -> Row {
    return {name,
            {{"100%", {}},
             {"0%", {flag, "0"}},
             {"25%", {flag, "25"}},
             {"50%", {flag, "50"}},
             {"75%", {flag, "75"}}}};
  };
  return {
      {"preset",
       {{"default", {}},
        {"low", {"--preset", "low"}},
        {"high", {"--preset", "high"}},
        {"ultra", {"--preset", "ultra"}},
        {"low-power", {"--preset", "low-power"}},
        {"smooth-60", {"--preset", "smooth-60"}},
        {"quality-30", {"--preset", "quality-30"}}}},
      {"shadows",
       {{"default", {}}, {"off", {"--shadows", "off"}}, {"low", {"--shadows", "low"}}, {"high", {"--shadows", "high"}}}},
      {"bloom", {{"default", {}}, {"on", {"--bloom"}}, {"off", {"--no-bloom"}}}},
      {"difficulty preset",
       {{"easy", {}}, {"medium", {}}, {"hard", {}}, {"insane", {}}, {"custom", {}}},
       1},
      {"difficulty",
       {{"default", {}},
        {"easy", {"--difficulty", "easy"}},
        {"medium", {"--difficulty", "med"}},
        {"hard", {"--difficulty", "hard"}},
        {"pro", {"--difficulty", "pro"}},
        {"hard+ (pinyon)", {"--difficulty", "hard+"}}}},
      {"rubber-banding", {{"on", {}}, {"off", {"--no-rubberband"}}}},
      {"traffic", {{"on", {}}, {"off", {"--no-traffic"}}}},
      {"collectibles", {{"on", {}}, {"off", {"--no-collectibles"}}}},
      {"treasure map", {{"off", {}}, {"on", {"--treasure-map"}}}},
      {"radio", {{"on", {}}, {"off", {"--no-radio"}}}},
      {"sound", {{"on", {}}, {"off", {"--no-sound"}}}},
      {"satnav", {{"on", {}}, {"off", {"--no-satnav"}}}},
      {"camera",
       {{"default", {}},
        {"chase low", {"--camera", "0"}},
        {"chase high", {"--camera", "1"}},
        {"cockpit", {"--camera", "2"}},
        {"hood", {"--camera", "3"}},
        {"bumper", {"--camera", "4"}}}},
      {"camera effects", {{"on", {}}, {"off", {"--no-camera-effects"}}}},
      {"rumble", {{"on", {}}, {"off", {"--no-rumble"}}}},
      {"rumble scale",
       {{"100%", {}},
        {"75%", {"--rumble-scale", "0.75"}},
        {"50%", {"--rumble-scale", "0.50"}},
        {"25%", {"--rumble-scale", "0.25"}},
        {"0%", {"--rumble-scale", "0.00"}}}},
      {"controller layout",
       {{"layout 1", {}},
        {"layout 2", {"--controller-layout", "1"}},
        {"layout 3", {"--controller-layout", "2"}},
        {"layout 4", {"--controller-layout", "3"}},
        {"layout 5", {"--controller-layout", "4"}}}},
      {"switch handbrake/clutch", {{"off", {}}, {"on", {"--switch-handbrake-clutch"}}}},
      {"speedometer", {{"analog", {}}, {"digital", {"--hud-speedo", "digital"}}, {"off", {"--hud-speedo", "off"}}}},
      {"minimap", {{"rotating", {}}, {"north-up", {"--minimap-mode", "north-up"}}, {"off", {"--minimap-mode", "off"}}}},
      {"lap time and position", {{"on", {}}, {"off", {"--no-hud-timers"}}}},
      {"wrong way warning", {{"on", {}}, {"off", {"--no-hud-wrong-way"}}}},
      {"names over cars", {{"on", {}}, {"off", {"--no-hud-names-over-cars"}}}},
      {"popularity and skills", {{"on", {}}, {"off", {"--no-hud-popularity-skills"}}}},
      {"tracked achievement", {{"off", {}}, {"on", {"--hud-tracked-achievement"}}}},
      {"barn find radar", {{"off", {}}, {"on", {"--hud-barn-radar"}}}},
      {"units", {{"metric (km/h)", {}}, {"imperial (mph)", {"--units", "imperial"}}}},
      {"steer deadzone in",
       {{"24%", {}},
        {"0%", {"--steer-dz-in", "0.00"}},
        {"5%", {"--steer-dz-in", "0.05"}},
        {"10%", {"--steer-dz-in", "0.10"}},
        {"15%", {"--steer-dz-in", "0.15"}},
        {"20%", {"--steer-dz-in", "0.20"}},
        {"30%", {"--steer-dz-in", "0.30"}},
        {"40%", {"--steer-dz-in", "0.40"}}}},
      {"steer deadzone out",
       {{"95%", {}},
        {"70%", {"--steer-dz-out", "0.70"}},
        {"80%", {"--steer-dz-out", "0.80"}},
        {"85%", {"--steer-dz-out", "0.85"}},
        {"90%", {"--steer-dz-out", "0.90"}},
        {"100%", {"--steer-dz-out", "1.00"}}}},
      {"throttle deadzone in",
       {{"15%", {}},
        {"0%", {"--throttle-dz-in", "0.00"}},
        {"5%", {"--throttle-dz-in", "0.05"}},
        {"10%", {"--throttle-dz-in", "0.10"}},
        {"20%", {"--throttle-dz-in", "0.20"}},
        {"30%", {"--throttle-dz-in", "0.30"}}}},
      {"throttle deadzone out",
       {{"90%", {}},
        {"70%", {"--throttle-dz-out", "0.70"}},
        {"80%", {"--throttle-dz-out", "0.80"}},
        {"85%", {"--throttle-dz-out", "0.85"}},
        {"95%", {"--throttle-dz-out", "0.95"}},
        {"100%", {"--throttle-dz-out", "1.00"}}}},
      {"brake deadzone in",
       {{"15%", {}},
        {"0%", {"--brake-dz-in", "0.00"}},
        {"5%", {"--brake-dz-in", "0.05"}},
        {"10%", {"--brake-dz-in", "0.10"}},
        {"20%", {"--brake-dz-in", "0.20"}},
        {"30%", {"--brake-dz-in", "0.30"}}}},
      {"brake deadzone out",
       {{"95%", {}},
        {"70%", {"--brake-dz-out", "0.70"}},
        {"80%", {"--brake-dz-out", "0.80"}},
        {"85%", {"--brake-dz-out", "0.85"}},
        {"90%", {"--brake-dz-out", "0.90"}},
        {"100%", {"--brake-dz-out", "1.00"}}}},
      {"braking",
       {{"abs on", {}}, {"assisted", {"--assist-braking", "0"}}, {"abs off", {"--assist-braking", "2"}}}},
      {"steering",
       {{"normal", {}}, {"assisted", {"--assist-steering", "0"}}, {"simulation", {"--assist-steering", "2"}}}},
      // A new profile's defaults (FEEL-12, SPEC_QUESTIONS Q71): TCS on, STM off.
      {"traction control", {{"on", {}}, {"off", {"--assist-tcs", "0"}}}},
      {"stability control", {{"off", {}}, {"on", {"--assist-stm", "1"}}}},
      {"shifting",
       {{"automatic", {}},
        {"manual", {"--assist-shifting", "1"}},
        {"manual+clutch", {"--assist-shifting", "2"}}}},
      {"driving line",
       {{"full", {}}, {"braking", {"--assist-line", "1"}}, {"off", {"--assist-line", "0"}}}},
      {"rewind", {{"on", {}}, {"off", {"--assist-rewind", "0"}}}},
      {"damage",
       {{"cosmetic", {}},
        {"none", {"--damage", "none"}},
        {"limited", {"--damage", "limited"}},
        {"simulation", {"--damage", "simulation"}}}},
      vol_row("master volume", "--master-volume"),
      {"mute", {{"off", {}}, {"on", {"--mute"}}}},
      {"full dynamic range", {{"off", {}}, {"on", {"--full-dynamic-range"}}}},
      {"radio dj", {{"on", {}}, {"off", {"--no-radio-dj"}}}},
      vol_row("radio volume", "--radio-volume"),
      mix_row("mix engine", "--mix-engine"),
      mix_row("mix other cars", "--mix-other-cars"),
      mix_row("mix tyres", "--mix-tyres"),
      mix_row("mix wind", "--mix-wind"),
      mix_row("mix ambience", "--mix-ambience"),
      mix_row("mix music", "--mix-music"),
      mix_row("mix ui", "--mix-ui"),
      mix_row("mix vo", "--mix-vo"),
      {"your music", {{"off", {}}, {"on", {"--your-music"}}}},
      {"your music shuffle", {{"off", {}}, {"on", {"--your-music-shuffle"}}}},
      vol_row("your music volume", "--your-music-volume"),
      {"day length",
       {{"30 min (default)", {}},
        {"15 min", {"--day-length", "15m"}},
        {"60 min", {"--day-length", "60m"}},
        {"real-time", {"--day-length", "realtime"}},
        {"frozen", {"--day-length", "frozen"}}}},
      {"time of day",
       {{"auto", {}},
        {"dawn (06:00)", {"--tod-preset", "6"}},
        {"noon (12:00)", {"--tod-preset", "12"}},
        {"sunset (19:00)", {"--tod-preset", "19"}},
        {"midnight (00:00)", {"--tod-preset", "0"}}}},
      {"motion blur", {{"on", {}}, {"off", {"--no-motion-blur"}}}},
      {"depth of field", {{"on", {}}, {"off", {"--no-dof"}}}},
      {"photo camera through walls", {{"off", {}}, {"on", {"--photo-through-walls"}}}},
      {"wider photo limits", {{"off", {}}, {"on", {"--photo-wider-limits"}}}},
      {"steering helpers", {{"off", {}}, {"on", {"--engine-steer-assist"}}}},
      {"ai opponents",
       {{"stock", {}},
        {"no rubber-banding", {"--ai-mode", "fair"}},
        {"hard (+1 tier)", {"--ai-mode", "hard"}}}},
      {"skip intro movies", {{"off", {}}, {"on", {"--skip-intro-movies"}}}},
      {"start in single player", {{"off", {}}, {"on", {"--start-in-single-player"}}}},
      {"skip vo with a", {{"on", {}}, {"off", {"--no-skip-vo-with-a"}}}},
      {"language",
       {{"english (us)", {}},
        {"english (uk)", {"--language", "GB"}},
        {"francais", {"--language", "FR"}},
        {"deutsch", {"--language", "DE"}},
        {"italiano", {"--language", "IT"}},
        {"espanol (es)", {"--language", "ES"}},
        {"espanol (mx)", {"--language", "MX"}},
        {"portugues (br)", {"--language", "BR"}},
        {"nederlands", {"--language", "NL"}},
        {"dansk", {"--language", "DA"}},
        {"norsk", {"--language", "NB"}},
        {"svenska", {"--language", "SV"}},
        {"suomi", {"--language", "FI"}},
        {"polski", {"--language", "PL"}},
        {"cestina", {"--language", "CZ"}},
        {"magyar", {"--language", "HU"}},
        {"russian", {"--language", "RU"}},
        {"japanese", {"--language", "JP"}},
        {"korean", {"--language", "KO"}},
        {"chinese (trad)", {"--language", "CHT"}}}},
      // Slice 11: Render scale, output scaling, AA, SSAO, LUT, VRS, texture cap, VSync, FPS cap, HDR, HUD layout.
      {"render scale",
       {{"1x (720p)", {}},
        {"native", {"--render-scale", "0"}},
        {"540p 1x", {"--render-scale", "1", "--scene-540p"}},
        {"2x (1440p)", {"--render-scale", "2"}},
        {"3x (2160p)", {"--render-scale", "3"}},
        {"4x (2880p)", {"--render-scale", "4"}}}},
      {"output scaling",
       {{"bilinear", {}},
        {"cas", {"--output", "cas"}},
        {"fsr 1", {"--output", "fsr1"}},
        {"fsr 2", {"--output", "fsr2"}},
        {"fsr 3", {"--output", "fsr3"}},
        {"dlss", {"--output", "dlss"}}}},
      {"sharpening",
       {{"50%", {}},
        {"0%", {"--sharpening", "0"}},
        {"25%", {"--sharpening", "25"}},
        {"75%", {"--sharpening", "75"}},
        {"100%", {"--sharpening", "100"}}}},
      {"anti-aliasing",
       {{"off", {}}, {"fxaa", {"--aa", "fxaa"}}, {"fxaa + hud", {"--aa", "fxaa-hud"}}}},
      {"ambient occlusion",
       {{"off", {}},
        {"low", {"--ssao", "low"}},
        {"medium", {"--ssao", "medium"}},
        {"high", {"--ssao", "high"}},
        {"ultra", {"--ssao", "ultra"}}}},
      {"contact shadows",
       {{"off", {}},
        {"low", {"--contact-shadows", "low"}},
        {"high", {"--contact-shadows", "high"}},
        {"ultra", {"--contact-shadows", "ultra"}}}},
      {"tonemapper",
       {{"hable fh1", {}},
        {"agx", {"--tonemapper", "agx"}},
        {"gt filmic", {"--tonemapper", "gt"}},
        {"pbr neutral", {"--tonemapper", "pbr-neutral"}}}},
      {"terrain hex-tiling", {{"off", {}}, {"on", {"--hex-tiling"}}}},
      {"velocity motion blur",
       {{"off", {}},
        {"low", {"--velocity-blur", "low"}},
        {"high", {"--velocity-blur", "high"}}}},
      {"shadow softness (pcss)",
       {{"off", {}},
        {"low", {"--shadow-pcss", "low"}},
        {"high", {"--shadow-pcss", "high"}},
        {"ultra", {"--shadow-pcss", "ultra"}}}},
      {"screen-space reflections",
       {{"off", {}},
        {"half-res", {"--ssr", "half"}},
        {"full-res", {"--ssr", "full"}}}},
      {"volumetric fog & beams",
       {{"off", {}},
        {"quarter-res", {"--volumetric-fog", "quarter"}},
        {"half-res", {"--volumetric-fog", "half"}}}},
      {"car clearcoat detail", {{"off", {}}, {"on", {"--car-detail"}}}},
      {"ray tracing",
       {{"off", {}},
        {"hybrid rt", {"--rt", "hybrid"}},
        {"path traced", {"--rt", "pt"}},
        {"photo mode pt", {"--rt", "photo-pt"}}}},
      {"rt bounces",
       {{"2 bounces", {}},
        {"1 bounce", {"--rt-bounces", "1"}},
        {"3 bounces", {"--rt-bounces", "3"}},
        {"4 bounces", {"--rt-bounces", "4"}}}},
      {"rt samples",
       {{"1 spp", {}},
        {"2 spp", {"--rt-spp", "2"}},
        {"4 spp", {"--rt-spp", "4"}},
        {"8 spp", {"--rt-spp", "8"}}}},
      {"rt denoiser", {{"on (svgf)", {}}, {"off", {"--no-rt-denoise"}}}},
      {"graphics backend",
       {{"auto", {}},
        {"vulkan 1.3", {"--backend", "vulkan"}},
        {"directx 12", {"--backend", "dx12"}},
        {"metal 3", {"--backend", "metal"}}}},
      {"colour lut", {{"none", {}}}},
      {"lut strength",
       {{"100%", {}},
        {"75%", {"--lut-strength", "75"}},
        {"50%", {"--lut-strength", "50"}},
        {"25%", {"--lut-strength", "25"}},
        {"0%", {"--lut-strength", "0"}}}},
      {"vrs shading", {{"off", {}}, {"on", {"--vrs"}}}},
      {"texture size",
       {{"full", {}},
        {"1024", {"--texture-cap", "1024"}},
        {"512", {"--texture-cap", "512"}},
        {"256", {"--texture-cap", "256"}}}},
      {"vsync",
       {{"fifo", {}}, {"mailbox", {"--vsync", "mailbox"}}, {"immediate", {"--vsync", "immediate"}}}},
      {"frame rate",
       {{"60 fps", {}},
        {"display", {"--fps-cap", "display"}},
        {"30 fps", {"--fps-cap", "30"}},
        {"40 fps", {"--fps-cap", "40"}},
        {"120 fps", {"--fps-cap", "120"}},
        {"144 fps", {"--fps-cap", "144"}},
        {"240 fps", {"--fps-cap", "240"}}}},
      {"frame gen",
       {{"off", {}}, {"fsr 3", {"--frame-gen", "fsr3"}}, {"dlss", {"--frame-gen", "dlss"}}}},
      {"hdr", {{"off", {}}, {"on", {"--hdr"}}}},
      {"windows auto-hdr", {{"off", {}}, {"on", {"--auto-hdr"}}}},
      {"hdr paper white",
       {{"203 nits", {}},
        {"120 nits", {"--hdr-paper-white", "120"}},
        {"160 nits", {"--hdr-paper-white", "160"}},
        {"250 nits", {"--hdr-paper-white", "250"}},
        {"300 nits", {"--hdr-paper-white", "300"}},
        {"400 nits", {"--hdr-paper-white", "400"}}}},
      {"hdr highlights",
       {{"50%", {}},
        {"0%", {"--hdr-highlights", "0"}},
        {"25%", {"--hdr-highlights", "25"}},
        {"75%", {"--hdr-highlights", "75"}},
        {"100%", {"--hdr-highlights", "100"}}}},
      {"hdr peak",
       {{"1000 nits", {}},
        {"400 nits", {"--hdr-peak", "400"}},
        {"600 nits", {"--hdr-peak", "600"}},
        {"800 nits", {"--hdr-peak", "800"}},
        {"1400 nits", {"--hdr-peak", "1400"}},
        {"2000 nits", {"--hdr-peak", "2000"}}}},
      {"hdr compare", {{"off", {}}, {"on", {"--hdr-compare"}}}},
      {"hud 16:9 lock", {{"on", {}}, {"off", {"--no-hud-16-9"}}}},
      {"hud scale",
       {{"100%", {}},
        {"80%", {"--hud-scale", "80"}},
        {"90%", {"--hud-scale", "90"}},
        {"110%", {"--hud-scale", "110"}},
        {"120%", {"--hud-scale", "120"}}}},
      {"safe zone",
       {{"0%", {}},
        {"2%", {"--safe-zone", "2"}},
        {"5%", {"--safe-zone", "5"}},
        {"8%", {"--safe-zone", "8"}},
        {"10%", {"--safe-zone", "10"}}}},
      // Slice 12: Camera Editor, Immersive Camera & Head Tracking (Display / Controls).
      {"camera editor", {{"normal", {}}, {"wheel view", {"--driving-view", "wheel"}}, {"replay view", {"--driving-view", "replay"}}}},
      {"chase height",
       {{"default", {}},
        {"-0.50 m", {"--chase-height", "-0.50"}},
        {"-0.25 m", {"--chase-height", "-0.25"}},
        {"+0.25 m", {"--chase-height", "0.25"}},
        {"+0.50 m", {"--chase-height", "0.50"}},
        {"+0.75 m", {"--chase-height", "0.75"}},
        {"+1.00 m", {"--chase-height", "1.00"}}}},
      {"chase distance",
       {{"default", {}},
        {"-0.50 m", {"--chase-distance", "-0.50"}},
        {"+0.50 m", {"--chase-distance", "0.50"}},
        {"+1.00 m", {"--chase-distance", "1.00"}},
        {"+1.50 m", {"--chase-distance", "1.50"}},
        {"+2.00 m", {"--chase-distance", "2.00"}}}},
      {"chase fov",
       {{"default", {}},
        {"85%", {"--chase-fov", "0.85"}},
        {"95%", {"--chase-fov", "0.95"}},
        {"105%", {"--chase-fov", "1.05"}},
        {"115%", {"--chase-fov", "1.15"}},
        {"125%", {"--chase-fov", "1.25"}}}},
      {"chase stiffness",
       {{"default", {}},
        {"50%", {"--chase-stiffness", "0.50"}},
        {"75%", {"--chase-stiffness", "0.75"}},
        {"125%", {"--chase-stiffness", "1.25"}},
        {"150%", {"--chase-stiffness", "1.50"}},
        {"200%", {"--chase-stiffness", "2.00"}}}},
      {"immersive camera",
       {{"off", {}},
        {"subtle", {"--immersive-camera", "subtle"}},
        {"default", {"--immersive-camera", "default"}},
        {"strong", {"--immersive-camera", "strong"}}}},
      {"head tracking (opentrack)", {{"off", {}}, {"on (udp 4242)", {"--head-tracking"}}}},
      // Slice 12: Racing Wheels, Separate Devices, DirectInput FFB & DualSense (Controls).
      {"wheel steering saturation",
       {{"100%", {}},
        {"50%", {"--wheel-sat", "50"}},
        {"60%", {"--wheel-sat", "60"}},
        {"75%", {"--wheel-sat", "75"}},
        {"90%", {"--wheel-sat", "90"}}}},
      {"wheel separate devices", {{"off", {}}, {"on (multi-usb)", {"--wheel-separate"}}}},
      {"wheel 900 deg linear", {{"off", {}}, {"on", {"--wheel-900-linear"}}}},
      {"wheel force feedback",
       {{"off", {}}, {"rumble only", {"--wheel-ffb-mode", "rumble"}}, {"full (directinput)", {"--wheel-ffb-mode", "full"}}}},
      {"wheel ffb strength",
       {{"50%", {}},
        {"25%", {"--wheel-ffb-strength", "25"}},
        {"75%", {"--wheel-ffb-strength", "75"}},
        {"100%", {"--wheel-ffb-strength", "100"}}}},
      {"wheel ffb invert", {{"off", {}}, {"on", {"--wheel-ffb-invert"}}}},
      {"dualsense adaptive triggers",
       {{"off", {}}, {"brake", {"--ds-triggers", "brake"}}, {"full", {"--ds-triggers", "full"}}}},
      {"dualsense trigger strength",
       {{"100%", {}},
        {"75%", {"--ds-trigger-strength", "75"}},
        {"50%", {"--ds-trigger-strength", "50"}},
        {"25%", {"--ds-trigger-strength", "25"}}}},
      {"dualsense gear click", {{"off", {}}, {"on", {"--ds-gear-click"}}}},
      {"dualsense lightbar",
       {{"player", {}}, {"rpm", {"--ds-lightbar", "rpm"}}, {"off", {"--ds-lightbar", "off"}}}},
      {"gyro tilt steering", {{"off", {}}, {"on", {"--gyro-steering"}}}},
      // Slice 12: Telemetry Out, Trainer, Festival Pacing & Hidden Cars (Gameplay / Pinyon).
      {"telemetry out (udp)", {{"off", {}}, {"on (127.0.0.1:5300)", {"--telemetry-udp"}}}},
      {"telemetry format", {{"fm7 dash (311b)", {}}, {"horizon 4/5 (324b)", {"--telemetry-udp-format", "horizon"}}}},
      {"telemetry moving only", {{"off", {}}, {"on", {"--telemetry-udp-moving-only"}}}},
      {"f10 trainer (cheats)", {{"off", {}}, {"on (user-modded)", {"--trainer-cheats"}}}},
      {"festival pacing", {{"off", {}}, {"on (user-modded)", {"--festival-pacing"}}}},
      {"pacing prices",
       {{"0.5x", {}},
        {"0.25x", {"--pacing-prices", "0.25"}},
        {"1.0x", {"--pacing-prices", "1.0"}},
        {"2.0x", {"--pacing-prices", "2.0"}},
        {"3.0x", {"--pacing-prices", "3.0"}}}},
      {"pacing rewards",
       {{"2.0x", {}},
        {"0.5x", {"--pacing-rewards", "0.5"}},
        {"1.0x", {"--pacing-rewards", "1.0"}},
        {"3.0x", {"--pacing-rewards", "3.0"}},
        {"5.0x", {"--pacing-rewards", "5.0"}}}},
      {"hidden cars (autoshow)", {{"off", {}}, {"unlocked (4 cars)", {"--hidden-cars"}}}},
  };
}

std::vector<SettingsPage::RebindRow> SettingsPage::build_rebinds() {
  return {
      {"key throttle", drive::ControlAction::kThrottle},
      {"key brake", drive::ControlAction::kBrake},
      {"key steer left", drive::ControlAction::kSteerLeft},
      {"key steer right", drive::ControlAction::kSteerRight},
      {"key handbrake", drive::ControlAction::kHandbrake},
      {"key clutch", drive::ControlAction::kClutch},
      {"key shift up", drive::ControlAction::kShiftUp},
      {"key shift down", drive::ControlAction::kShiftDown},
      {"key camera", drive::ControlAction::kSwitchCamera},
      {"key look back", drive::ControlAction::kLookBack},
      {"key look left", drive::ControlAction::kLookLeft},
      {"key look right", drive::ControlAction::kLookRight},
      {"key rewind", drive::ControlAction::kRewind},
      {"key reset car", drive::ControlAction::kReset},
      {"key world map", drive::ControlAction::kMap},
      {"key radio prev", drive::ControlAction::kRadioPrev},
      {"key radio next", drive::ControlAction::kRadioNext},
      {"key activate", drive::ControlAction::kActivate},
      {"key photo mode", drive::ControlAction::kPhotoMode},
      {"key telemetry", drive::ControlAction::kTelemetry},
  };
}

void SettingsPage::set_available_luts(const std::vector<std::string>& names) {
  lut_files_ = names;
  for (Row& r : rows_) {
    if (std::string(r.name) != "colour lut") continue;
    const std::string cur_val = r.choices[r.pick].tokens.size() == 2 ? r.choices[r.pick].tokens[1] : "";
    r.choices = {{"none", {}}};
    for (const std::string& n : lut_files_) {
      if (!n.empty()) r.choices.push_back({n, {"--lut", n}});
    }
    if (!cur_val.empty()) {
      bool found = false;
      for (size_t c = 0; c < r.choices.size(); ++c) {
        if (r.choices[c].tokens.size() == 2 && r.choices[c].tokens[1] == cur_val) {
          r.pick = c;
          found = true;
          break;
        }
      }
      if (!found) {
        r.choices.push_back({cur_val, {"--lut", cur_val}});
        r.pick = r.choices.size() - 1;
      }
    } else {
      r.pick = 0;
    }
    break;
  }
}

void SettingsPage::load(const std::vector<std::string>& tokens) {
  rows_ = build();
  set_available_luts(lut_files_);
  rebinds_ = build_rebinds();
  keys_ = drive::KeyBindings::defaults();
  other_.clear();
  capturing_ = false;
  dirty_ = false;
  has_explicit_controls_ = false;
  tab_ = Tab::kDisplay;
  row_ = 0;
  bool seen_540p = false;
  auto reset_row = [&](const char* name) {
    for (Row& r : rows_) {
      if (std::string(r.name) == name) {
        r.pick = 0;
        break;
      }
    }
  };
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string& t = tokens[i];
    const bool has_value = i + 1 < tokens.size() && tokens[i + 1].rfind("--", 0) != 0;
    if (is_control_or_assist_flag(t)) has_explicit_controls_ = true;
    if (t == "--keys" && has_value) {
      keys_.deserialize(tokens[i + 1]);
      ++i;
      continue;
    }
    if ((t == "--backend" || t == "--graphics_backend") && has_value) {
      std::string v = tokens[i + 1];
      for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
      size_t pick = 0;
      if (v == "vulkan" || v == "vk") pick = 1;
      else if (v == "dx12" || v == "d3d12" || v == "directx12" || v == "direct3d12") pick = 2;
      else if (v == "metal" || v == "mtl") pick = 3;
      for (Row& r : rows_) {
        if (std::string(r.name) == "graphics backend" && pick < r.choices.size()) {
          r.pick = pick;
          break;
        }
      }
      ++i;
      continue;
    }
    if (t == "--scene-540p") {
      seen_540p = true;
      for (Row& r : rows_) {
        if (std::string(r.name) == "render scale") r.pick = 2;  // 540p 1x
      }
      continue;
    }
    if (t == "--no-scene-540p") {
      seen_540p = false;
      for (Row& r : rows_) {
        if (std::string(r.name) == "render scale" && r.pick == 2) r.pick = 0;
      }
      continue;
    }
    if (t == "--lut" && has_value) {
      const std::string& lut_name = tokens[i + 1];
      for (Row& r : rows_) {
        if (std::string(r.name) != "colour lut") continue;
        bool matched = false;
        for (size_t c = 0; c < r.choices.size(); ++c) {
          if (r.choices[c].tokens.size() == 2 && r.choices[c].tokens[1] == lut_name) {
            r.pick = c;
            matched = true;
            break;
          }
        }
        if (!matched) {
          r.choices.push_back({lut_name, {"--lut", lut_name}});
          r.pick = r.choices.size() - 1;
        }
      }
      ++i;
      continue;
    }
    bool owned = false;
    bool row_takes_value = false;
    for (Row& r : rows_) {
      bool row_owns_flag = false;
      bool matched_choice = false;
      for (size_t c = 0; c < r.choices.size(); ++c) {
        const auto& w = r.choices[c].tokens;
        if (w.empty() || w[0] != t) continue;
        row_owns_flag = true;
        if (w.size() >= 2) row_takes_value = true;
        if (w.size() == 1 || (w.size() >= 2 && has_value && token_values_equal(w[1], tokens[i + 1]))) {
          if (t == "--render-scale" && has_value && tokens[i + 1] == "1" && seen_540p) {
            r.pick = 2;
            matched_choice = true;
          } else if (w.size() == 3 && !(t == "--render-scale" && has_value && tokens[i + 1] == "1" && seen_540p)) {
            continue;
          } else {
            r.pick = c;
            matched_choice = true;
          }
          owned = true;
        } else if (w.size() >= 2 && has_value) {
          owned = true;
        }
      }
      // Explicit default values (e.g. "--assist-line 2", "--driving-view normal",
      // "--chase-height 0.00", "--ssr off", "--render-scale 1" without --scene-540p) reset to pick 0.
      if (row_owns_flag && !matched_choice && row_takes_value && has_value) {
        if (!(t == "--render-scale" && tokens[i + 1] == "1" && seen_540p)) {
          r.pick = 0;
          if (!explicit_off_tokens_for_row(r.name).empty()) r.explicit_default = true;
        }
      } else if (row_owns_flag && matched_choice) {
        r.explicit_default = false;
      }
    }
    if (!owned) {
      struct DefaultFlag {
        const char* flag;
        const char* row;
      };
      static constexpr DefaultFlag kResetFlags[] = {
          {"--traffic", "traffic"},
          {"--collectibles", "collectibles"},
          {"--rumble", "rumble"},
          {"--camera-effects", "camera effects"},
          {"--motion-blur", "motion blur"},
          {"--dof", "depth of field"},
          {"--satnav", "satnav"},
          {"--no-treasure-map", "treasure map"},
          {"--no-skip-intro-movies", "skip intro movies"},
          {"--no-start-in-single-player", "start in single player"},
          {"--skip-vo-with-a", "skip vo with a"},
          {"--no-vrs", "vrs shading"},
          {"--no-hdr", "hdr"},
          {"--no-auto-hdr", "windows auto-hdr"},
          {"--no-hdr-compare", "hdr compare"},
          {"--hud-16-9", "hud 16:9 lock"},
          {"--no-hex-tiling", "terrain hex-tiling"},
          {"--no-car-detail", "car clearcoat detail"},
          {"--rt-denoise", "rt denoiser"},
          {"--no-head-tracking", "head tracking (opentrack)"},
          {"--no-switch-handbrake-clutch", "switch handbrake/clutch"},
      };
      for (const auto& rf : kResetFlags) {
        if (t == rf.flag) {
          reset_row(rf.row);
          if (!explicit_off_tokens_for_row(rf.row).empty()) {
            for (Row& r : rows_) {
              if (std::string(r.name) == rf.row) {
                r.explicit_default = true;
                break;
              }
            }
          }
          owned = true;
          break;
        }
      }
    }
    bool opt_takes_value = false;
    is_settings_option(t, &opt_takes_value);
    const bool takes_value =
        has_value && (row_takes_value || opt_takes_value || t == "--size" || t == "--msaa" || t == "--exposure" ||
                      t == "--hour" || t == "--play" || t == "--mods");
    if (!owned) {
      other_.push_back(t);
      if (takes_value) other_.push_back(tokens[i + 1]);
    }
    if (takes_value) ++i;
  }
  sync_difficulty_preset();
}

namespace {
SettingsPage::Tab classify_setting_row(const std::string& name) {
  if (name == "preset" || name == "shadows" || name == "bloom" || name == "camera" || name == "camera effects" ||
      name == "motion blur" || name == "depth of field" || name == "day length" || name == "time of day" ||
      name == "camera editor" || name.rfind("chase ", 0) == 0 || name == "immersive camera" ||
      name == "render scale" || name == "output scaling" || name == "sharpening" || name == "anti-aliasing" ||
      name == "ambient occlusion" || name == "contact shadows" || name == "tonemapper" ||
      name == "terrain hex-tiling" || name == "velocity motion blur" ||
      name == "shadow softness (pcss)" || name == "screen-space reflections" ||
      name == "volumetric fog & beams" || name == "car clearcoat detail" ||
      name == "ray tracing" || name == "rt bounces" || name == "rt samples" || name == "rt denoiser" ||
      name == "graphics backend" ||
      name == "colour lut" || name == "lut strength" || name == "vrs shading" ||
      name == "texture size" || name == "vsync" || name == "frame rate" || name == "frame gen" ||
      name == "hdr" || name == "windows auto-hdr" || name == "hdr paper white" || name == "hdr highlights" ||
      name == "hdr peak" || name == "hdr compare") {
    return SettingsPage::Tab::kDisplay;
  }
  if (name == "radio" || name == "sound" || name == "satnav" || name == "master volume" || name == "mute" ||
      name == "full dynamic range" || name == "radio dj" || name == "radio volume" || name.rfind("mix ", 0) == 0 ||
      name.rfind("your music", 0) == 0) {
    return SettingsPage::Tab::kAudio;
  }
  if (name == "difficulty preset" || name == "difficulty" || name == "rubber-banding" || name == "traffic" ||
      name == "collectibles" || name == "treasure map" || name == "braking" || name == "steering" ||
      name == "traction control" || name == "stability control" || name == "shifting" || name == "driving line" ||
      name == "rewind" || name == "damage" || name == "ai opponents" || name == "units" || name == "speedometer" ||
      name == "minimap" || name == "lap time and position" || name == "wrong way warning" ||
      name == "names over cars" || name == "popularity and skills" || name == "tracked achievement" ||
      name == "barn find radar" ||
      name == "hud 16:9 lock" || name == "hud scale" || name == "safe zone" ||
      name.rfind("telemetry", 0) == 0) {
    return SettingsPage::Tab::kGameplay;
  }
  if (name == "rumble" || name == "rumble scale" || name == "controller layout" ||
      name == "switch handbrake/clutch" ||
      name.find("deadzone") != std::string::npos || name.rfind("wheel ", 0) == 0 ||
      name.rfind("dualsense ", 0) == 0 || name.rfind("gyro ", 0) == 0 || name.rfind("head tracking", 0) == 0) {
    return SettingsPage::Tab::kControls;
  }
  return SettingsPage::Tab::kPinyon;
}
}  // namespace

std::vector<int> SettingsPage::tab_flat_indices(Tab t) const {
  std::vector<int> out;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (classify_setting_row(rows_[i].name) == t) out.push_back(int(i));
  }
  if (t == Tab::kControls) {
    for (size_t i = 0; i < rebinds_.size(); ++i) out.push_back(int(rows_.size() + i));
  }
  out.push_back(int(rows_.size() + rebinds_.size()));  // SAVE & APPLY SETTINGS
  return out;
}

void SettingsPage::open() {
  open_ = true;
  capturing_ = false;
  dirty_ = false;
  tab_ = Tab::kDisplay;
  const auto indices = tab_flat_indices(tab_);
  row_ = indices.empty() ? 0 : indices.front();
}

void SettingsPage::move(int by) {
  if (capturing_) return;
  const auto indices = tab_flat_indices(tab_);
  const int n = int(indices.size());
  if (n <= 0) return;
  int pos = 0;
  for (int i = 0; i < n; ++i) {
    if (indices[size_t(i)] == row_) {
      pos = i;
      break;
    }
  }
  pos = ((pos + by) % n + n) % n;
  row_ = indices[size_t(pos)];
}

void SettingsPage::step(int by) {
  if (capturing_ || row_ < 0 || row_ >= int(rows_.size())) return;
  Row& r = rows_[size_t(row_)];
  const std::string rname(r.name);
  if (rname == "difficulty preset") {
    // Cycle EASY (0) <-> MEDIUM (1) <-> HARD (2) <-> INSANE (3); leave CUSTOM (4) on step.
    if (r.pick >= 4) {
      r.pick = (by >= 0) ? 0u : 3u;
    } else {
      r.pick = size_t(((int(r.pick) + by) % 4 + 4) % 4);
    }
    apply_difficulty_preset(r.pick);
    dirty_ = true;
    return;
  }
  const int n = int(r.choices.size());
  r.pick = size_t(((int(r.pick) + by) % n + n) % n);
  r.explicit_default = (r.pick == 0 && !explicit_off_tokens_for_row(r.name).empty());
  if (rname == "preset") {
    const auto& pt = r.choices[r.pick].tokens;
    PresetSotaDefaults pdef;
    if (pt.size() == 2 && preset_sota_defaults(pt[1], pdef)) {
      auto apply_sota_pick = [&](const char* rname, const std::string& val, bool is_flag_on) {
        for (Row& sr : rows_) {
          if (std::string(sr.name) != rname) continue;
          sr.explicit_default = false;
          if (!val.empty()) {
            for (size_t c = 0; c < sr.choices.size(); ++c) {
              if (sr.choices[c].tokens.size() >= 2 && sr.choices[c].tokens[1] == val) {
                sr.pick = c;
                return;
              }
            }
          }
          sr.pick = is_flag_on && sr.choices.size() > 1 ? 1 : 0;
          return;
        }
      };
      const char* ssao_s = pdef.ssao == modern::SsaoQuality::kLow ? "low"
                           : pdef.ssao == modern::SsaoQuality::kMedium ? "medium"
                           : pdef.ssao == modern::SsaoQuality::kHigh ? "high"
                           : pdef.ssao == modern::SsaoQuality::kUltra ? "ultra"
                                                                      : "";
      apply_sota_pick("ambient occlusion", ssao_s, false);
      apply_sota_pick("contact shadows",
                      pdef.contact_shadows == modern::ContactShadows::kOff
                          ? ""
                          : modern::contact_shadows_name(pdef.contact_shadows),
                      false);
      apply_sota_pick("terrain hex-tiling", "", pdef.hex_tiling);
      apply_sota_pick("velocity motion blur",
                      pdef.velocity_blur == modern::VelocityBlur::kOff ? "" : modern::velocity_blur_name(pdef.velocity_blur),
                      false);
      apply_sota_pick("shadow softness (pcss)",
                      pdef.shadow_pcss == modern::ShadowPcss::kOff ? "" : modern::shadow_pcss_name(pdef.shadow_pcss),
                      false);
      apply_sota_pick("screen-space reflections",
                      pdef.ssr == modern::SsrQuality::kOff ? "" : modern::ssr_name(pdef.ssr), false);
      apply_sota_pick("volumetric fog & beams",
                      pdef.volumetric_fog == modern::VolumetricFog::kOff
                          ? ""
                          : modern::volumetric_fog_name(pdef.volumetric_fog),
                      false);
      apply_sota_pick("car clearcoat detail", "", pdef.car_detail);
    }
  }
  if (rname == "difficulty" || rname == "braking" || rname == "steering" || rname == "traction control" ||
      rname == "stability control" || rname == "shifting" || rname == "driving line" || rname == "rewind" ||
      rname == "damage") {
    sync_difficulty_preset();
  }
  dirty_ = true;
}

int SettingsPage::selected_in_tab() const {
  const auto indices = tab_flat_indices(tab_);
  for (size_t i = 0; i < indices.size(); ++i) {
    if (indices[i] == row_) return int(i);
  }
  return 0;
}

int SettingsPage::tab_row_count() const { return int(tab_flat_indices(tab_).size()); }

void SettingsPage::switch_tab(int by) {
  if (capturing_) return;
  const int count = int(Tab::kCount);
  tab_ = Tab(((int(tab_) + by) % count + count) % count);
  const auto indices = tab_flat_indices(tab_);
  row_ = indices.empty() ? 0 : indices.front();
}

bool SettingsPage::select_id(const std::string& id) {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (id == rows_[i].name) {
      select_row(int(i));
      return true;
    }
  }
  return false;
}

void SettingsPage::reset_rows(const std::vector<std::string>& ids) {
  const std::vector<Row> fresh = build();
  for (size_t i = 0; i < rows_.size() && i < fresh.size(); ++i) {
    for (const std::string& id : ids) {
      // The preset follows the rows it sets (below), as on load.
      if (id == "difficulty preset" || id != rows_[i].name) continue;
      size_t pick = fresh[i].pick;
      // OPPONENT DIFFICULTY resets to a level the original names, a new
      // profile's MEDIUM (Q71), not the engine's "default" (keep the save's).
      if (id == "difficulty")
        for (size_t c = 0; c < rows_[i].choices.size(); ++c)
          if (rows_[i].choices[c].label == "medium") pick = c;
      if (rows_[i].pick != pick || rows_[i].explicit_default) {
        rows_[i].pick = pick;
        rows_[i].explicit_default = false;
        dirty_ = true;
      }
    }
  }
  sync_difficulty_preset();
}

void SettingsPage::select_row(int flat_idx) {
  if (capturing_) return;
  const int n = int(rows_.size() + rebinds_.size()) + 1;
  if (flat_idx < 0 || flat_idx >= n) return;
  row_ = flat_idx;
  if (flat_idx < int(rows_.size())) {
    tab_ = classify_setting_row(rows_[size_t(flat_idx)].name);
  } else if (on_rebind_row()) {
    tab_ = Tab::kControls;
  }
  // On the SAVE & APPLY line, preserve the current tab_.
}

void SettingsPage::select_tab(int tab_idx) {
  if (capturing_) return;
  const int count = int(Tab::kCount);
  if (tab_idx < 0 || tab_idx >= count) return;
  tab_ = Tab(tab_idx);
  const auto indices = tab_flat_indices(tab_);
  row_ = indices.empty() ? 0 : indices.front();
}

std::vector<SettingsPage::DisplayRow> SettingsPage::display_rows() const {
  std::vector<DisplayRow> out;
  out.reserve(rows_.size() + rebinds_.size() + 1);
  for (size_t i = 0; i < rows_.size(); ++i) {
    out.push_back({format_row_name(rows_[i].name), format_row_value(rows_[i]), classify_setting_row(rows_[i].name),
                   int(i), false, false, false, row_ == int(i), rows_[i].name});
  }
  for (size_t i = 0; i < rebinds_.size(); ++i) {
    const int idx = int(rows_.size() + i);
    const bool cap = capturing_ && row_ == idx;
    out.push_back({format_row_name(rebinds_[i].name),
                   cap ? "PRESS KEY..." : keys_.map[size_t(rebinds_[i].action)].primary, Tab::kControls, idx, true,
                   false, cap, row_ == idx});
  }
  const int save_idx = int(rows_.size() + rebinds_.size());
  out.push_back({resolve_setting_loc("HelpButtons", "IDS_Save", "SAVE & APPLY SETTINGS"), "CONFIRM", active_tab(),
                 save_idx, false, true, false, row_ == save_idx});
  return out;
}

void SettingsPage::begin_rebind() {
  if (on_rebind_row()) capturing_ = true;
}

bool SettingsPage::commit_rebind(const std::string& key_name) {
  if (!capturing_ || !on_rebind_row() || key_name.empty()) return false;
  const auto& rb = rebinds_[size_t(row_) - rows_.size()];
  keys_.set_primary(rb.action, key_name);
  capturing_ = false;
  dirty_ = true;
  return true;
}

std::vector<std::string> SettingsPage::tokens() const {
  std::vector<std::string> out = other_;
  for (const Row& r : rows_) {
    const auto& w = r.choices[r.pick].tokens;
    if (!w.empty()) {
      out.insert(out.end(), w.begin(), w.end());
    } else if (r.explicit_default) {
      const auto off_toks = explicit_off_tokens_for_row(r.name);
      out.insert(out.end(), off_toks.begin(), off_toks.end());
    }
  }
  const std::string def_kb = drive::KeyBindings::defaults().serialize();
  const std::string cur_kb = keys_.serialize();
  if (cur_kb != def_kb && !cur_kb.empty()) {
    out.push_back("--keys");
    out.push_back(cur_kb);
  }
  return out;
}

LiveSettings SettingsPage::live_settings() const {
  LiveSettings ls;
  auto pick_val = [&](const char* name) -> std::string {
    for (const Row& r : rows_)
      if (std::string(r.name) == name) {
        const auto& w = r.choices[r.pick].tokens;
        return w.size() == 2 ? w[1] : "";
      }
    return "";
  };
  auto pick_label = [&](const char* name) -> std::string {
    for (const Row& r : rows_)
      if (std::string(r.name) == name) return r.choices[r.pick].label;
    return "";
  };
  auto num_or = [&](const char* name, double fallback) {
    const std::string v = pick_val(name);
    return v.empty() ? fallback : std::strtod(v.c_str(), nullptr);
  };

  const std::string cam = pick_val("camera");
  if (!cam.empty()) ls.camera_mode = std::atoi(cam.c_str());

  const std::string diff = pick_val("difficulty");
  if (diff == "easy") ls.difficulty = 0;
  else if (diff == "med") ls.difficulty = 1;
  else if (diff == "hard") ls.difficulty = 2;
  else if (diff == "pro") ls.difficulty = 3;
  else if (diff == "hard+") ls.difficulty = 4;

  const std::string aim = pick_val("ai opponents");
  if (aim == "fair") ls.ai_mode = 1;
  else if (aim == "hard") ls.ai_mode = 2;
  else ls.ai_mode = 0;

  ls.rubberband = pick_label("rubber-banding") != "off" && ls.ai_mode == 0 && ls.difficulty != 4;
  ls.radio = pick_label("radio") != "off";
  ls.sound = pick_label("sound") != "off";
  ls.satnav = pick_label("satnav") != "off";
  ls.treasure_map = pick_label("treasure map") == "on";
  ls.motion_blur = pick_label("motion blur") != "off";
  ls.dof = pick_label("depth of field") != "off";
  ls.skip_intro_movies = pick_label("skip intro movies") == "on";
  ls.start_in_single_player = pick_label("start in single player") == "on";
  ls.skip_vo_with_a = pick_label("skip vo with a") != "off";
  ls.metric_units = pick_val("units") != "imperial";
  {
    const std::string sp = pick_val("speedometer");
    ls.hud_speedo_mode = (sp == "digital") ? 1 : (sp == "off") ? 2 : 0;
    const std::string mm = pick_val("minimap");
    ls.minimap_mode = (mm == "north-up") ? 1 : (mm == "off") ? 2 : 0;
  }
  ls.hud.timers = pick_label("lap time and position") != "off";
  ls.hud.wrong_way = pick_label("wrong way warning") != "off";
  ls.hud.names_over_cars = pick_label("names over cars") != "off";
  ls.hud.popularity_skills = pick_label("popularity and skills") != "off";
  ls.hud.tracked_achievement = pick_label("tracked achievement") == "on";
  ls.hud.barn_radar = pick_label("barn find radar") == "on";
  {
    const std::string lg = pick_val("language");
    ls.language = lg.empty() ? "EN" : lg;
  }

  const std::string dmg = pick_val("damage");
  if (dmg == "none") ls.damage_mode = 0;
  else if (dmg == "simulation") ls.damage_mode = 2;
  else if (dmg == "limited") ls.damage_mode = 3;  // Q102
  else ls.damage_mode = 1;
  ls.photo_through_walls = pick_label("photo camera through walls") == "on";
  ls.photo_wider_limits = pick_label("wider photo limits") == "on";

  const std::string dl = pick_val("day length");
  if (dl == "15m") ls.day_length_scale = 2.0;
  else if (dl == "60m") ls.day_length_scale = 0.5;
  else if (dl == "realtime") ls.day_length_scale = 1827.9 / 86400.0;
  else if (dl == "frozen") ls.day_length_scale = 0.0;
  else ls.day_length_scale = 1.0;

  const std::string tp = pick_val("time of day");
  ls.tod_preset_hour = tp.empty() ? -1.0 : std::strtod(tp.c_str(), nullptr);

  auto& c = ls.controls;
  c.camera_effects = pick_label("camera effects") != "off";
  c.rumble_enabled = pick_label("rumble") != "off";
  c.rumble_scale = num_or("rumble scale", 1.0);
  const std::string lay = pick_val("controller layout");
  c.layout_id = lay.empty() ? 0 : std::atoi(lay.c_str());
  c.switch_handbrake_clutch = pick_label("switch handbrake/clutch") == "on";
  c.deadzones.steering_inside = num_or("steer deadzone in", 0.24);
  c.deadzones.steering_outside = num_or("steer deadzone out", 0.95);
  c.deadzones.throttle_inside = num_or("throttle deadzone in", 0.15);
  c.deadzones.throttle_outside = num_or("throttle deadzone out", 0.90);
  c.deadzones.brake_inside = num_or("brake deadzone in", 0.15);
  c.deadzones.brake_outside = num_or("brake deadzone out", 0.95);
  c.deadzones.clamp_valid();
  c.keys = keys_;

  const std::string brk = pick_val("braking");
  c.assists.braking = brk.empty() ? 1 : std::atoi(brk.c_str());
  const std::string str = pick_val("steering");
  c.assists.steering = str.empty() ? 1 : std::atoi(str.c_str());
  c.assists.traction_control = pick_label("traction control") == "on";
  c.assists.stability_control = pick_label("stability control") == "on";
  const std::string shf = pick_val("shifting");
  c.assists.shifting = shf.empty() ? 0 : std::atoi(shf.c_str());
  const std::string ln = pick_val("driving line");
  c.assists.driving_line = ln.empty() ? 2 : std::atoi(ln.c_str());
  c.assists.rewind = pick_label("rewind") != "off";
  c.assists.engine_steer_assist = pick_label("steering helpers") == "on";
  // Slice 12 live settings.
  ls.camera_editor.chase_height_m = num_or("chase height", 0.0);
  ls.camera_editor.chase_distance_m = num_or("chase distance", 0.0);
  ls.camera_editor.chase_fov_scale = num_or("chase fov", 1.0);
  ls.camera_editor.chase_stiffness = num_or("chase stiffness", 1.0);
  {
    const std::string dv = pick_val("camera editor");
    if (dv == "wheel") ls.camera_editor.driving_view = drive::DrivingView::kWheel;
    else if (dv == "replay") ls.camera_editor.driving_view = drive::DrivingView::kReplay;
    else ls.camera_editor.driving_view = drive::DrivingView::kNormal;
  }
  ls.immersive_camera.apply_preset(drive::parse_immersive_preset(pick_val("immersive camera")));
  ls.head_tracking.enabled = pick_label("head tracking (opentrack)") != "off";

  ls.wheel_shaping.steering_saturation = int(num_or("wheel steering saturation", 100.0));
  ls.wheel_separate_devices = pick_label("wheel separate devices") != "off";
  ls.wheel_900_linear = pick_label("wheel 900 deg linear") != "off";
  ls.wheel_ffb.mode = drive::parse_wheel_ffb_mode(pick_val("wheel force feedback"));
  ls.wheel_ffb.strength_percent = int(num_or("wheel ffb strength", 50.0));
  ls.wheel_ffb.invert = pick_label("wheel ffb invert") != "off";

  ls.dualsense.trigger_mode = drive::dualsense::parse_trigger_mode(pick_val("dualsense adaptive triggers"));
  ls.dualsense.trigger_strength_pct = int(num_or("dualsense trigger strength", 100.0));
  ls.dualsense.gear_click = pick_label("dualsense gear click") != "off";
  ls.dualsense.lightbar_mode = drive::dualsense::parse_lightbar_mode(pick_val("dualsense lightbar"));
  ls.dualsense.gyro_steering = pick_label("gyro tilt steering") != "off";

  ls.telemetry_udp.enabled = pick_label("telemetry out (udp)") != "off";
  ls.telemetry_udp.format = drive::telemetry::parse_format(pick_val("telemetry format"));
  ls.telemetry_udp.moving_only = pick_label("telemetry moving only") != "off";

  ls.trainer_cheats = pick_label("f10 trainer (cheats)") != "off";
  ls.festival_pacing.enabled = pick_label("festival pacing") != "off";
  ls.festival_pacing.price_multiplier = num_or("pacing prices", 0.5);
  ls.festival_pacing.reward_multiplier = num_or("pacing rewards", 2.0);
  ls.hidden_cars = pick_label("hidden cars (autoshow)") != "off";
  {
    const std::string gb = pick_val("graphics backend");
    ls.graphics_backend = gb.empty() ? "auto" : gb;
  }
  ls.preset = pick_val("preset");
  ls.shadows = pick_val("shadows");
  {
    const std::string bl = pick_label("bloom");
    if (bl == "on") ls.bloom_mode = 1;
    else if (bl == "off") ls.bloom_mode = 0;
    else ls.bloom_mode = -1;
  }
  ls.traffic = pick_label("traffic") != "off";
  ls.collectibles = pick_label("collectibles") != "off";

  // Apply fine-grained overrides written by Camera Editor / Trainer overlays in settings.txt.
  {
    const std::vector<std::string> toks = tokens();
    // Apply preset SOTA graphics defaults first so explicit overrides win without
    // losing preset defaults when an unrelated setting is toggled live.
    bool ssao_explicit = false, sscs_explicit = false, hex_explicit = false, vel_explicit = false;
    bool pcss_explicit = false, ssr_explicit = false, vol_explicit = false, detail_explicit = false;
    for (size_t i = 0; i < toks.size(); ++i) {
      const std::string& t = toks[i];
      auto next_v = [&]() -> std::string { return (i + 1 < toks.size()) ? toks[++i] : std::string{}; };
      if (t == "--chase-height") ls.camera_editor.chase_height_m = std::atof(next_v().c_str());
      else if (t == "--chase-distance") ls.camera_editor.chase_distance_m = std::atof(next_v().c_str());
      else if (t == "--chase-fov") ls.camera_editor.chase_fov_scale = std::atof(next_v().c_str());
      else if (t == "--chase-stiffness") ls.camera_editor.chase_stiffness = std::atof(next_v().c_str());
      else if (t == "--driving-view") {
        const std::string dv = next_v();
        if (dv == "wheel") ls.camera_editor.driving_view = drive::DrivingView::kWheel;
        else if (dv == "replay") ls.camera_editor.driving_view = drive::DrivingView::kReplay;
        else ls.camera_editor.driving_view = drive::DrivingView::kNormal;
      } else if (t == "--immersive-gforce") ls.immersive_camera.gforce = std::atof(next_v().c_str());
      else if (t == "--immersive-lean") ls.immersive_camera.lean = std::atof(next_v().c_str());
      else if (t == "--immersive-shake") ls.immersive_camera.shake = std::atof(next_v().c_str());
      else if (t == "--immersive-head") ls.immersive_camera.head = std::atof(next_v().c_str());
      else if (t == "--head-tracking") ls.head_tracking.enabled = true;
      else if (t == "--no-head-tracking") ls.head_tracking.enabled = false;
      else if (t == "--head-tracking-port") ls.head_tracking.port = std::max(1, std::atoi(next_v().c_str()));
      else if (t == "--head-tracking-invert") {
        const std::string inv = next_v();
        if (inv.size() >= 3) {
          ls.head_tracking.invert[0] = (inv[0] == '1');
          ls.head_tracking.invert[1] = (inv[1] == '1');
          ls.head_tracking.invert[2] = (inv[2] == '1');
        }
      } else if (t == "--trainer-saved-road") {
        ls.trainer_saved_road = drive::parse_road_point(next_v());
      } else {
        if (t == "--ssao") ssao_explicit = true;
        else if (t == "--contact-shadows") sscs_explicit = true;
        else if (t == "--hex-tiling" || t == "--no-hex-tiling") hex_explicit = true;
        else if (t == "--velocity-blur") vel_explicit = true;
        else if (t == "--shadow-pcss") pcss_explicit = true;
        else if (t == "--ssr") ssr_explicit = true;
        else if (t == "--volumetric-fog") vol_explicit = true;
        else if (t == "--car-detail" || t == "--no-car-detail") detail_explicit = true;
        modern::apply_graphics_option(ls.graphics, toks, i);
      }
    }
    PresetSotaDefaults pdef;
    if (!ls.preset.empty() && preset_sota_defaults(ls.preset, pdef)) {
      if (!ssao_explicit) ls.graphics.ssao = pdef.ssao;
      if (!sscs_explicit) ls.graphics.contact_shadows = pdef.contact_shadows;
      if (!hex_explicit) ls.graphics.hex_tiling = pdef.hex_tiling;
      if (!vel_explicit) ls.graphics.velocity_blur = pdef.velocity_blur;
      if (!pcss_explicit) ls.graphics.shadow_pcss = pdef.shadow_pcss;
      if (!ssr_explicit) ls.graphics.ssr = pdef.ssr;
      if (!vol_explicit) ls.graphics.volumetric_fog = pdef.volumetric_fog;
      if (!detail_explicit) ls.graphics.car_detail = pdef.car_detail;
    }
    ls.graphics = modern::clamp_graphics(ls.graphics);
  }
  return ls;
}

int SettingsPage::difficulty_credit_bonus_pct() const {
  // The race award's own rule (race::total_difficulty_bonus_percent), so the
  // preview matches what a race pays.
  const LiveSettings ls = live_settings();
  const int diff_idx = ls.difficulty >= 0 ? ls.difficulty : 1;  // default Medium
  const auto& a = ls.controls.assists;
  race::DifficultyAssists d;
  d.rewind_on = a.rewind;
  d.braking = a.braking;
  d.steering = a.steering;
  d.tcs_on = a.traction_control;
  d.stm_on = a.stability_control;
  d.shifting = a.shifting;
  d.driving_line = a.driving_line;
  d.damage_mode = ls.damage_mode;
  return race::total_difficulty_bonus_percent(diff_idx, d);
}

void SettingsPage::sync_from_live(const LiveSettings& live) {
  auto set_by_label = [&](const char* name, const std::string& label) {
    for (Row& r : rows_) {
      if (std::string(r.name) != name) continue;
      for (size_t c = 0; c < r.choices.size(); ++c) {
        if (r.choices[c].label == label) {
          r.pick = c;
          return;
        }
      }
      return;
    }
  };
  auto set_by_val = [&](const char* name, const std::string& val) {
    for (Row& r : rows_) {
      if (std::string(r.name) != name) continue;
      if (val.empty()) {
        r.pick = 0;
        return;
      }
      for (size_t c = 0; c < r.choices.size(); ++c) {
        const auto& w = r.choices[c].tokens;
        if (w.size() >= 2 && token_values_equal(w[1], val)) {
          r.pick = c;
          return;
        }
      }
      r.pick = 0;
      return;
    }
  };
  auto set_by_num = [&](const char* name, double val, double default_val) {
    for (Row& r : rows_) {
      if (std::string(r.name) != name) continue;
      if (std::fabs(val - default_val) < 1e-4) {
        r.pick = 0;
        return;
      }
      for (size_t c = 1; c < r.choices.size(); ++c) {
        const auto& w = r.choices[c].tokens;
        double cv = 0.0;
        if (w.size() >= 2 && parse_strict_number(w[1], cv) && std::fabs(cv - val) < 1e-4) {
          r.pick = c;
          return;
        }
      }
      return;
    }
  };

  if (live.camera_mode >= 0 && live.camera_mode < drive::kCameraModes) {
    set_by_val("camera", std::to_string(live.camera_mode));
  }
  if (live.difficulty >= 0 && live.difficulty <= 4) {
    static constexpr const char* kDiff[5] = {"easy", "med", "hard", "pro", "hard+"};
    set_by_val("difficulty", kDiff[live.difficulty]);
  }
  set_by_val("ai opponents", live.ai_mode == 1 ? "fair" : live.ai_mode == 2 ? "hard" : "");
  if (live.ai_mode == 0 && live.difficulty != 4) {
    set_by_label("rubber-banding", live.rubberband ? "on" : "off");
  }
  set_by_label("traffic", live.traffic ? "on" : "off");
  set_by_label("collectibles", live.collectibles ? "on" : "off");
  set_by_label("radio", live.radio ? "on" : "off");
  set_by_label("sound", live.sound ? "on" : "off");
  set_by_label("satnav", live.satnav ? "on" : "off");
  set_by_label("treasure map", live.treasure_map ? "on" : "off");
  set_by_label("motion blur", live.motion_blur ? "on" : "off");
  set_by_label("depth of field", live.dof ? "on" : "off");
  set_by_label("skip intro movies", live.skip_intro_movies ? "on" : "off");
  set_by_label("start in single player", live.start_in_single_player ? "on" : "off");
  set_by_label("skip vo with a", live.skip_vo_with_a ? "on" : "off");
  set_by_val("units", live.metric_units ? "" : "imperial");
  set_by_val("speedometer", live.hud_speedo_mode == 1 ? "digital" : live.hud_speedo_mode == 2 ? "off" : "");
  set_by_val("minimap", live.minimap_mode == 1 ? "north-up" : live.minimap_mode == 2 ? "off" : "");
  set_by_label("lap time and position", live.hud.timers ? "on" : "off");
  set_by_label("wrong way warning", live.hud.wrong_way ? "on" : "off");
  set_by_label("names over cars", live.hud.names_over_cars ? "on" : "off");
  set_by_label("popularity and skills", live.hud.popularity_skills ? "on" : "off");
  set_by_label("tracked achievement", live.hud.tracked_achievement ? "on" : "off");
  set_by_label("barn find radar", live.hud.barn_radar ? "on" : "off");
  set_by_val("language", (live.language.empty() || live.language == "EN") ? "" : live.language);
  set_by_val("damage", live.damage_mode == 0   ? "none"
                       : live.damage_mode == 2 ? "simulation"
                       : live.damage_mode == 3 ? "limited"
                                               : "");
  set_by_label("photo camera through walls", live.photo_through_walls ? "on" : "off");
  set_by_label("wider photo limits", live.photo_wider_limits ? "on" : "off");

  if (std::fabs(live.day_length_scale - 2.0) < 1e-3) set_by_val("day length", "15m");
  else if (std::fabs(live.day_length_scale - 0.5) < 1e-3) set_by_val("day length", "60m");
  else if (live.day_length_scale <= 1e-6) set_by_val("day length", "frozen");
  else if (std::fabs(live.day_length_scale - (1827.9 / 86400.0)) < 1e-3) set_by_val("day length", "realtime");
  else set_by_val("day length", "");

  if (live.tod_preset_hour >= 0.0) set_by_num("time of day", live.tod_preset_hour, -1.0);

  const auto& c = live.controls;
  set_by_label("camera effects", c.camera_effects ? "on" : "off");
  set_by_label("rumble", c.rumble_enabled ? "on" : "off");
  set_by_num("rumble scale", c.rumble_scale, 1.0);
  set_by_val("controller layout", c.layout_id == 0 ? "" : std::to_string(c.layout_id));
  set_by_label("switch handbrake/clutch", c.switch_handbrake_clutch ? "on" : "off");
  set_by_num("steer deadzone in", c.deadzones.steering_inside, 0.24);
  set_by_num("steer deadzone out", c.deadzones.steering_outside, 0.95);
  set_by_num("throttle deadzone in", c.deadzones.throttle_inside, 0.15);
  set_by_num("throttle deadzone out", c.deadzones.throttle_outside, 0.90);
  set_by_num("brake deadzone in", c.deadzones.brake_inside, 0.15);
  set_by_num("brake deadzone out", c.deadzones.brake_outside, 0.95);
  keys_ = c.keys;

  set_by_val("braking", c.assists.braking == 1 ? "" : std::to_string(c.assists.braking));
  set_by_val("steering", c.assists.steering == 1 ? "" : std::to_string(c.assists.steering));
  set_by_label("traction control", c.assists.traction_control ? "on" : "off");
  set_by_label("stability control", c.assists.stability_control ? "on" : "off");
  set_by_val("shifting", c.assists.shifting == 0 ? "" : std::to_string(c.assists.shifting));
  set_by_val("driving line", c.assists.driving_line == 2 ? "" : std::to_string(c.assists.driving_line));
  set_by_label("rewind", c.assists.rewind ? "on" : "off");
  set_by_label("steering helpers", c.assists.engine_steer_assist ? "on" : "off");

  if (!live.preset.empty()) set_by_val("preset", live.preset);
  if (!live.shadows.empty()) set_by_val("shadows", live.shadows);
  if (live.bloom_mode == 1) set_by_label("bloom", "on");
  else if (live.bloom_mode == 0) set_by_label("bloom", "off");

  set_by_val("graphics backend", (live.graphics_backend.empty() || live.graphics_backend == "auto") ? "" : live.graphics_backend);

  // Slice 12 live state.
  set_by_val("camera editor", live.camera_editor.driving_view == drive::DrivingView::kWheel
                                  ? "wheel"
                                  : (live.camera_editor.driving_view == drive::DrivingView::kReplay ? "replay" : ""));
  set_by_num("chase height", live.camera_editor.chase_height_m, 0.0);
  set_by_num("chase distance", live.camera_editor.chase_distance_m, 0.0);
  set_by_num("chase fov", live.camera_editor.chase_fov_scale, 1.0);
  set_by_num("chase stiffness", live.camera_editor.chase_stiffness, 1.0);
  {
    const auto ip = live.immersive_camera.detect_preset();
    const char* iv = "";
    if (ip == drive::ImmersivePreset::kSubtle) iv = "subtle";
    else if (ip == drive::ImmersivePreset::kDefault) iv = "default";
    else if (ip == drive::ImmersivePreset::kStrong) iv = "strong";
    set_by_val("immersive camera", iv);
  }
  set_by_label("head tracking (opentrack)", live.head_tracking.enabled ? "on (udp 4242)" : "off");
  set_by_num("wheel steering saturation", double(live.wheel_shaping.steering_saturation), 100.0);
  set_by_label("wheel separate devices", live.wheel_separate_devices ? "on (multi-usb)" : "off");
  set_by_label("wheel 900 deg linear", live.wheel_900_linear ? "on" : "off");
  set_by_val("wheel force feedback", live.wheel_ffb.mode == drive::WheelFfbMode::kRumbleOnly
                                         ? "rumble"
                                         : (live.wheel_ffb.mode == drive::WheelFfbMode::kFull ? "full" : ""));
  set_by_num("wheel ffb strength", double(live.wheel_ffb.strength_percent), 50.0);
  set_by_label("wheel ffb invert", live.wheel_ffb.invert ? "on" : "off");
  set_by_val("dualsense adaptive triggers",
             live.dualsense.trigger_mode == drive::dualsense::TriggerMode::kBrake
                 ? "brake"
                 : (live.dualsense.trigger_mode == drive::dualsense::TriggerMode::kFull ? "full" : ""));
  set_by_num("dualsense trigger strength", double(live.dualsense.trigger_strength_pct), 100.0);
  set_by_label("dualsense gear click", live.dualsense.gear_click ? "on" : "off");
  set_by_val("dualsense lightbar",
             live.dualsense.lightbar_mode == drive::dualsense::LightbarMode::kRpm
                 ? "rpm"
                 : (live.dualsense.lightbar_mode == drive::dualsense::LightbarMode::kOff ? "off" : ""));
  set_by_label("gyro tilt steering", live.dualsense.gyro_steering ? "on" : "off");
  set_by_label("telemetry out (udp)", live.telemetry_udp.enabled ? "on (127.0.0.1:5300)" : "off");
  set_by_val("telemetry format",
             live.telemetry_udp.format == drive::telemetry::Format::kFh4Horizon ? "horizon" : "");
  set_by_label("telemetry moving only", live.telemetry_udp.moving_only ? "on" : "off");
  set_by_label("f10 trainer (cheats)", live.trainer_cheats ? "on (user-modded)" : "off");
  set_by_label("festival pacing", live.festival_pacing.enabled ? "on (user-modded)" : "off");
  set_by_num("pacing prices", live.festival_pacing.price_multiplier, 0.5);
  set_by_num("pacing rewards", live.festival_pacing.reward_multiplier, 2.0);
  set_by_label("hidden cars (autoshow)", live.hidden_cars ? "unlocked (4 cars)" : "off");

  // Modern graphics settings.
  const auto& g = live.graphics;
  if (g.base_540p && g.scale == 1) set_by_label("render scale", "540p 1x");
  else if (g.scale == 1) set_by_label("render scale", "1x (720p)");
  else set_by_val("render scale", std::to_string(g.scale));

  {
    const char* up = "";
    switch (g.upscaler) {
      case modern::Upscaler::kCas: up = "cas"; break;
      case modern::Upscaler::kFsr1: up = "fsr1"; break;
      case modern::Upscaler::kFsr2: up = "fsr2"; break;
      case modern::Upscaler::kFsr3: up = "fsr3"; break;
      case modern::Upscaler::kDlss: up = "dlss"; break;
      default: break;
    }
    set_by_val("output scaling", up);
  }
  set_by_num("sharpening", double(g.sharpening), 50.0);
  set_by_val("anti-aliasing", g.aa == modern::AaMode::kFxaaWorld
                                  ? "fxaa"
                                  : (g.aa == modern::AaMode::kFxaaAll ? "fxaa-hud" : ""));
  set_by_val("tonemapper",
             g.tonemapper == modern::Tonemapper::kHable ? "" : modern::tonemapper_name(g.tonemapper));

  // Only pin SOTA rows when no preset owns them or when they differ from the active preset defaults.
  PresetSotaDefaults pdef;
  const bool has_preset = !live.preset.empty() && preset_sota_defaults(live.preset, pdef);
  if (!has_preset || g.ssao != pdef.ssao) {
    const char* sv = "";
    switch (g.ssao) {
      case modern::SsaoQuality::kLow: sv = "low"; break;
      case modern::SsaoQuality::kMedium: sv = "medium"; break;
      case modern::SsaoQuality::kHigh: sv = "high"; break;
      case modern::SsaoQuality::kUltra: sv = "ultra"; break;
      default: break;
    }
    set_by_val("ambient occlusion", sv);
  }
  if (!has_preset || g.contact_shadows != pdef.contact_shadows) {
    set_by_val("contact shadows", g.contact_shadows == modern::ContactShadows::kOff
                                      ? ""
                                      : modern::contact_shadows_name(g.contact_shadows));
  }
  if (!has_preset || g.hex_tiling != pdef.hex_tiling) {
    set_by_label("terrain hex-tiling", g.hex_tiling ? "on" : "off");
  }
  if (!has_preset || g.velocity_blur != pdef.velocity_blur) {
    set_by_val("velocity motion blur", g.velocity_blur == modern::VelocityBlur::kOff
                                           ? ""
                                           : modern::velocity_blur_name(g.velocity_blur));
  }
  if (!has_preset || g.shadow_pcss != pdef.shadow_pcss) {
    set_by_val("shadow softness (pcss)",
               g.shadow_pcss == modern::ShadowPcss::kOff ? "" : modern::shadow_pcss_name(g.shadow_pcss));
  }
  if (!has_preset || g.ssr != pdef.ssr) {
    set_by_val("screen-space reflections", g.ssr == modern::SsrQuality::kOff ? "" : modern::ssr_name(g.ssr));
  }
  if (!has_preset || g.volumetric_fog != pdef.volumetric_fog) {
    set_by_val("volumetric fog & beams", g.volumetric_fog == modern::VolumetricFog::kOff
                                             ? ""
                                             : modern::volumetric_fog_name(g.volumetric_fog));
  }
  if (!has_preset || g.car_detail != pdef.car_detail) {
    set_by_label("car clearcoat detail", g.car_detail ? "on" : "off");
  }

  set_by_val("ray tracing", g.rt_mode == rt::RtMode::kOff ? "" : rt::rt_mode_name(g.rt_mode));
  set_by_num("rt bounces", double(g.rt_bounces), 2.0);
  set_by_num("rt samples", double(g.rt_spp), 1.0);
  set_by_label("rt denoiser", g.rt_denoise ? "on (svgf)" : "off");

  if (!g.lut.empty()) {
    for (Row& r : rows_) {
      if (std::string(r.name) != "colour lut") continue;
      bool matched = false;
      for (size_t idx = 0; idx < r.choices.size(); ++idx) {
        if (r.choices[idx].tokens.size() == 2 && r.choices[idx].tokens[1] == g.lut) {
          r.pick = idx;
          matched = true;
          break;
        }
      }
      if (!matched) {
        r.choices.push_back({g.lut, {"--lut", g.lut}});
        r.pick = r.choices.size() - 1;
      }
      break;
    }
  } else {
    set_by_label("colour lut", "none");
  }
  set_by_num("lut strength", double(g.lut_strength), 100.0);
  set_by_label("vrs shading", g.vrs ? "on" : "off");
  set_by_val("texture size", g.texture_cap == modern::TextureCap::kFull ? "" : std::to_string(int(g.texture_cap)));
  set_by_val("vsync", g.vsync == modern::VSync::kMailbox
                          ? "mailbox"
                          : (g.vsync == modern::VSync::kImmediate ? "immediate" : ""));
  if (g.fps_cap == 0) set_by_val("frame rate", "display");
  else if (g.fps_cap == 60) set_by_val("frame rate", "");
  else set_by_val("frame rate", std::to_string(g.fps_cap));
  set_by_val("frame gen", g.frame_gen == modern::FrameGen::kFsr3
                              ? "fsr3"
                              : (g.frame_gen == modern::FrameGen::kDlss ? "dlss" : ""));
  set_by_label("hdr", g.hdr ? "on" : "off");
  set_by_label("windows auto-hdr", g.auto_hdr ? "on" : "off");
  set_by_num("hdr paper white", double(g.paper_white), 203.0);
  set_by_num("hdr highlights", double(g.hdr_highlights), 50.0);
  set_by_num("hdr peak", double(g.peak_nits), 1000.0);
  set_by_label("hdr compare", g.hdr_compare ? "on" : "off");
  set_by_label("hud 16:9 lock", g.hud_lock_16_9 ? "on" : "off");
  set_by_num("hud scale", double(g.hud_scale), 100.0);
  set_by_num("safe zone", double(g.safe_zone), 0.0);
  sync_difficulty_preset();
}

void SettingsPage::set_text_resolver(TextResolver fn) { settings_text_resolver() = std::move(fn); }

std::string SettingsPage::tab_label(Tab t) {
  switch (t) {
    case Tab::kDisplay: return resolve_setting_loc("PauseMenu", "IDS_Video", "DISPLAY");
    case Tab::kAudio: return resolve_setting_loc("PauseMenu", "IDS_Audio", "AUDIO");
    case Tab::kGameplay: return std::string("DIFFICULTY & HUD");
    case Tab::kControls: return resolve_setting_loc("PauseMenu", "IDS_Controller", "CONTROLS");
    case Tab::kPinyon: return std::string("ENGINE");
    default: return "OPTIONS";
  }
}

bool SettingsPage::metric_units_active() const {
  for (const Row& r : rows_) {
    if (std::string_view(r.name) == "units") {
      const auto& toks = r.choices[r.pick].tokens;
      return !(toks.size() >= 2 && toks[1] == "imperial");
    }
  }
  return true;
}

std::string SettingsPage::format_row_name(const char* raw_name) const {
  if (!settings_text_resolver()) return raw_name;
  const std::string_view n(raw_name);
  if (n == "difficulty preset") return resolve_setting_loc("Assists", "IDS_MetaDifficulty", raw_name);
  if (n == "difficulty") return resolve_setting_loc("Assists", "IDS_AIDifficulty", raw_name);
  if (n == "braking") return resolve_setting_loc("Assists", "IDS_Braking", raw_name);
  if (n == "steering") return resolve_setting_loc("Assists", "IDS_Steering", raw_name);
  if (n == "traction control") return resolve_setting_loc("Assists", "IDS_TractionControl", raw_name);
  if (n == "stability control") return resolve_setting_loc("Assists", "IDS_StabilityControl", raw_name);
  if (n == "shifting") return resolve_setting_loc("Assists", "IDS_Shifting", raw_name);
  if (n == "driving line") return resolve_setting_loc("Assists", "IDS_SuggestedLine", raw_name);
  if (n == "rewind") return resolve_setting_loc("Assists", "IDS_Rewind", raw_name);
  if (n == "units") return resolve_setting_loc("InGame", "IDS_HudOption_Units", raw_name);
  if (n == "speedometer") return resolve_setting_loc("InGame", "IDS_HudOption_Speedometer", raw_name);
  if (n == "minimap") return resolve_setting_loc("InGame", "IDS_HudOption_Map", raw_name);
  if (n == "lap time and position") return resolve_setting_loc("InGame", "IDS_HudOption_Timers", raw_name);
  if (n == "wrong way warning") return resolve_setting_loc("InGame", "IDS_HudOption_WrongWay", raw_name);
  if (n == "names over cars") return resolve_setting_loc("InGame", "IDS_HudOption_Names", raw_name);
  if (n == "popularity and skills") return resolve_setting_loc("InGame", "IDS_HudOption_PopularitySkills", raw_name);
  if (n == "rumble") return resolve_setting_loc("MyProfile", "IDS_Option_RumbleSetting", raw_name);
  if (n == "controller layout") return resolve_setting_loc("MyProfile", "IDS_Option_Layout", raw_name);
  if (n == "switch handbrake/clutch")
    return resolve_setting_loc("MyProfile", "IDS_Option_InvertHandbrakeClutch", raw_name);
  if (n == "master volume") return resolve_setting_loc("MyProfile", "IDS_MasterGameVolume", raw_name);
  if (n == "radio volume") return resolve_setting_loc("MyProfile", "IDS_RadioVolume", raw_name);
  return raw_name;
}

std::string SettingsPage::format_row_value(const Row& r) const {
  const std::string_view n(r.name);
  if (n == "difficulty preset") {
    static constexpr const char* kPresetNames[5] = {"EASY", "MEDIUM", "HARD", "INSANE", "CUSTOM"};
    const size_t idx = std::min<size_t>(r.pick, 4u);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s (+%d%% CR)", kPresetNames[idx], difficulty_credit_bonus_pct());
    return buf;
  }
  if ((n == "chase height" || n == "chase distance") && r.pick > 0 && !metric_units_active()) {
    const auto& toks = r.choices[r.pick].tokens;
    double meters = 0.0;
    if (toks.size() >= 2 && parse_strict_number(toks[1], meters)) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%+.2f ft", meters * 3.28084);
      return buf;
    }
  }
  return r.choices[r.pick].label;
}

void SettingsPage::apply_difficulty_preset(size_t preset_pick) {
  auto set_row_label = [&](const char* name, const char* label) {
    for (Row& r : rows_) {
      if (std::string_view(r.name) != name) continue;
      for (size_t c = 0; c < r.choices.size(); ++c) {
        if (r.choices[c].label == label) {
          r.pick = c;
          r.explicit_default = false;
          return;
        }
      }
      return;
    }
  };
  switch (preset_pick) {
    case 0:  // EASY (+0% CR)
      set_row_label("difficulty", "easy");
      set_row_label("braking", "assisted");
      set_row_label("steering", "assisted");
      set_row_label("traction control", "on");
      set_row_label("stability control", "on");
      set_row_label("shifting", "automatic");
      set_row_label("driving line", "full");
      set_row_label("rewind", "on");
      set_row_label("damage", "none");
      break;
    case 1:  // MEDIUM (+25% CR): a new profile's assists (Q71)
      set_row_label("difficulty", "default");
      set_row_label("braking", "abs on");
      set_row_label("steering", "normal");
      set_row_label("traction control", "on");
      set_row_label("stability control", "off");
      set_row_label("shifting", "automatic");
      set_row_label("driving line", "full");
      set_row_label("rewind", "on");
      set_row_label("damage", "cosmetic");
      break;
    case 2:  // HARD (+75% CR)
      set_row_label("difficulty", "hard");
      set_row_label("braking", "abs on");
      set_row_label("steering", "normal");
      set_row_label("traction control", "off");
      set_row_label("stability control", "off");
      set_row_label("shifting", "manual");
      set_row_label("driving line", "braking");
      set_row_label("rewind", "on");
      set_row_label("damage", "cosmetic");
      break;
    case 3:  // INSANE (+140% CR)
      set_row_label("difficulty", "pro");
      set_row_label("braking", "abs off");
      set_row_label("steering", "simulation");
      set_row_label("traction control", "off");
      set_row_label("stability control", "off");
      set_row_label("shifting", "manual+clutch");
      set_row_label("driving line", "off");
      set_row_label("rewind", "off");
      set_row_label("damage", "simulation");
      break;
    default: break;
  }
}

void SettingsPage::sync_difficulty_preset() {
  const LiveSettings ls = live_settings();
  const int diff = ls.difficulty >= 0 ? ls.difficulty : 1;
  const auto& a = ls.controls.assists;
  size_t detected = 4;  // CUSTOM
  if (diff == 0 && a.braking == 0 && a.steering == 0 && a.traction_control && a.stability_control && a.shifting == 0 &&
      a.driving_line == 2 && a.rewind && ls.damage_mode == 0) {
    detected = 0;  // EASY
  } else if (diff == 1 && a.braking == 1 && a.steering == 1 && a.traction_control && !a.stability_control &&
             a.shifting == 0 && a.driving_line == 2 && a.rewind && ls.damage_mode == 1) {
    detected = 1;  // MEDIUM
  } else if (diff == 2 && a.braking == 1 && a.steering == 1 && !a.traction_control && !a.stability_control &&
             a.shifting == 1 && a.driving_line == 1 && a.rewind && ls.damage_mode == 1) {
    detected = 2;  // HARD
  } else if (diff == 3 && a.braking == 2 && a.steering == 2 && !a.traction_control && !a.stability_control &&
             a.shifting == 2 && a.driving_line == 0 && !a.rewind && ls.damage_mode == 2) {
    detected = 3;  // INSANE
  }
  for (Row& r : rows_) {
    if (std::string_view(r.name) == "difficulty preset") {
      r.pick = detected;
      break;
    }
  }
}

void SettingsPage::fill(std::string& title, std::vector<std::string>& lines, int& selected) const {
  title = capturing_ ? "press a key to bind (esc cancels)"
                     : "settings: left/right change (live), enter to rebind or save";
  lines.clear();
  for (const Row& r : rows_) lines.push_back(format_row_name(r.name) + "  < " + format_row_value(r) + " >");
  for (size_t i = 0; i < rebinds_.size(); ++i) {
    const bool cap = capturing_ && row_ == int(rows_.size() + i);
    const std::string& k = keys_.map[size_t(rebinds_[i].action)].primary;
    lines.push_back(format_row_name(rebinds_[i].name) + (cap ? "  [ press key... ]" : ("  [ " + k + " ]")));
  }
  lines.push_back("save & apply");
  selected = row_;
}

std::string SettingsPage::value(const std::string& name) const {
  for (const Row& r : rows_)
    if (name == r.name) return format_row_value(r);
  for (const RebindRow& rb : rebinds_)
    if (name == rb.name) return keys_.map[size_t(rb.action)].primary;
  return {};
}

std::vector<std::string> SettingsPage::choice_labels(const std::string& name) const {
  std::vector<std::string> out;
  for (const Row& r : rows_)
    if (name == r.name) {
      for (const auto& c : r.choices) out.emplace_back(c.label);
      break;
    }
  return out;
}

SettingsPage::ControllerDiagramSpec SettingsPage::controller_diagram_spec() const {
  ControllerDiagramSpec spec;
  const LiveSettings ls = live_settings();
  spec.enabled = (tab_ == Tab::kControls);
  spec.layout_id = ls.controls.layout_id;
  spec.swap_handbrake_clutch = ls.controls.switch_handbrake_clutch;
  if (!spec.enabled) return spec;

  std::string sel_name;
  if (row_ >= 0 && size_t(row_) < rows_.size()) sel_name = rows_[size_t(row_)].name;
  else if (on_rebind_row()) sel_name = rebinds_[size_t(row_ - int(rows_.size()))].name;
  for (char& c : sel_name) c = char(std::toupper(static_cast<unsigned char>(c)));

  if (sel_name.find("HANDBRAKE") != std::string::npos) spec.highlight_action = "HANDBRAKE";
  else if (sel_name.find("CLUTCH") != std::string::npos) spec.highlight_action = "CLUTCH";
  else if (sel_name.find("THROTTLE") != std::string::npos) spec.highlight_action = "GAS";
  else if (sel_name.find("BRAKE") != std::string::npos) spec.highlight_action = "BRAKE";
  else if (sel_name.find("STEER") != std::string::npos) spec.highlight_action = "STEER";
  else if (sel_name.find("SHIFT UP") != std::string::npos) spec.highlight_action = "SHIFT UP";
  else if (sel_name.find("SHIFT DOWN") != std::string::npos) spec.highlight_action = "SHIFT DOWN";
  else if (sel_name.find("REWIND") != std::string::npos) spec.highlight_action = "REWIND";
  else if (sel_name.find("CAMERA") != std::string::npos) spec.highlight_action = "CAMERA";
  else if (sel_name.find("LOOK BACK") != std::string::npos) spec.highlight_action = "LOOK BACK";
  else if (sel_name.find("MAP") != std::string::npos) spec.highlight_action = "MAP";
  else if (sel_name.find("RADIO") != std::string::npos) spec.highlight_action = "RADIO";
  else if (sel_name.find("PHOTO") != std::string::npos) spec.highlight_action = "PHOTO MODE";
  else if (sel_name.find("TELEMETRY") != std::string::npos) spec.highlight_action = "TELEMETRY";
  return spec;
}

drive::ControllerLayout SettingsPage::active_controller_layout() const {
  const LiveSettings ls = live_settings();
  return drive::apply_handbrake_clutch_swap(drive::default_controller_layout(ls.controls.layout_id),
                                            ls.controls.switch_handbrake_clutch);
}

std::vector<SettingsPage::ControllerDiagramCallout> SettingsPage::controller_diagram_callouts() const {
  const drive::ControllerLayout lay = active_controller_layout();
  auto pretty_action = [](std::string_view raw) -> std::string {
    if (raw == "Gas") return "GAS";
    if (raw == "Brake") return "BRAKE";
    if (raw == "Handbrake") return "HANDBRAKE";
    if (raw == "Clutch") return "CLUTCH";
    if (raw == "ShiftUp") return "SHIFT UP";
    if (raw == "ShiftDown") return "SHIFT DOWN";
    if (raw == "Rewind") return "REWIND";
    if (raw == "SwitchCamera") return "CAMERA";
    if (raw == "LookStraightBack") return "LOOK BACK";
    if (raw == "Map") return "MAP";
    if (raw == "RadioLeft" || raw == "RadioRight") return "RADIO";
    if (raw == "PhotoMode") return "PHOTO MODE";
    if (raw == "Telemetry") return "TELEMETRY";
    return std::string(raw);
  };
  const ControllerDiagramSpec dspec = controller_diagram_spec();

  auto make = [&](const char* btn, std::string act, bool left) {
    bool hi = false;
    if (!dspec.highlight_action.empty()) {
      if (dspec.highlight_action == act) hi = true;
      else if ((dspec.highlight_action == "HANDBRAKE" || dspec.highlight_action == "CLUTCH") &&
               (act == "HANDBRAKE" || act == "CLUTCH")) {
        hi = true;
      }
    }
    return ControllerDiagramCallout{btn, std::move(act), left, hi};
  };

  return {
      make("LT", pretty_action(lay.action_for(drive::PadControl::kLeftTrigger)), true),
      make("LB", pretty_action(lay.action_for(drive::PadControl::kLeftShoulder)), true),
      make("LS", "STEER", true),
      make("DPAD_LR", "RADIO", true),
      make("DPAD_UP", pretty_action(lay.action_for(drive::PadControl::kDPadUp)), true),
      make("DPAD_DN", pretty_action(lay.action_for(drive::PadControl::kDPadDown)), true),
      make("BACK", pretty_action(lay.action_for(drive::PadControl::kBack)), true),
      make("RT", pretty_action(lay.action_for(drive::PadControl::kRightTrigger)), false),
      make("RB", pretty_action(lay.action_for(drive::PadControl::kRightShoulder)), false),
      make("Y", pretty_action(lay.action_for(drive::PadControl::kButtonY)), false),
      make("B", pretty_action(lay.action_for(drive::PadControl::kButtonB)), false),
      make("X", pretty_action(lay.action_for(drive::PadControl::kButtonX)), false),
      make("A", pretty_action(lay.action_for(drive::PadControl::kButtonA)), false),
      make("RS", pretty_action(lay.action_for(drive::PadControl::kRightStickClick)), false),
  };
}

}  // namespace carview
