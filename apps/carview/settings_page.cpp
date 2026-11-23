#include "settings_page.h"

#include <algorithm>
#include <cstdlib>

namespace carview {

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
        {"low-power", {"--preset", "low-power"}}}},
      {"shadows",
       {{"default", {}}, {"off", {"--shadows", "off"}}, {"low", {"--shadows", "low"}}, {"high", {"--shadows", "high"}}}},
      {"bloom", {{"default", {}}, {"on", {"--bloom"}}, {"off", {"--no-bloom"}}}},
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
      {"traction control", {{"off", {}}, {"on", {"--assist-tcs", "1"}}}},
      {"stability control", {{"on", {}}, {"off", {"--assist-stm", "0"}}}},
      {"shifting",
       {{"automatic", {}},
        {"manual", {"--assist-shifting", "1"}},
        {"manual+clutch", {"--assist-shifting", "2"}}}},
      {"driving line",
       {{"off", {}}, {"braking", {"--assist-line", "1"}}, {"full", {"--assist-line", "2"}}}},
      {"rewind", {{"on", {}}, {"off", {"--assist-rewind", "0"}}}},
      {"damage",
       {{"cosmetic", {}},
        {"none", {"--damage", "none"}},
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
      {"key camera", drive::ControlAction::kSwitchCamera},
      {"key look back", drive::ControlAction::kLookBack},
      {"key shift up", drive::ControlAction::kShiftUp},
      {"key shift down", drive::ControlAction::kShiftDown},
      {"key rewind", drive::ControlAction::kRewind},
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
  bool seen_540p = false;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string& t = tokens[i];
    const bool has_value = i + 1 < tokens.size() && tokens[i + 1].rfind("--", 0) != 0;
    if (t == "--keys" && has_value) {
      keys_.deserialize(tokens[i + 1]);
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
    for (Row& r : rows_)
      for (size_t c = 0; c < r.choices.size(); ++c) {
        const auto& w = r.choices[c].tokens;
        if (w.empty() || w[0] != t) continue;
        if (w.size() == 1 || (w.size() >= 2 && has_value && w[1] == tokens[i + 1])) {
          if (t == "--render-scale" && has_value && tokens[i + 1] == "1" && seen_540p) r.pick = 2;
          else if (w.size() == 3 && !(t == "--render-scale" && has_value && tokens[i + 1] == "1" && seen_540p))
            continue;
          else r.pick = c;
          owned = true;
        } else if (w.size() >= 2 && has_value) {
          owned = true;
        }
      }
    if (!owned && (t == "--traffic" || t == "--rumble" || t == "--camera-effects" ||
                   t == "--motion-blur" || t == "--dof" || t == "--satnav" || t == "--no-treasure-map" ||
                   t == "--no-skip-intro-movies" || t == "--no-start-in-single-player" ||
                   t == "--skip-vo-with-a" || t == "--no-scene-540p" || t == "--no-vrs" ||
                   t == "--no-hdr" || t == "--no-auto-hdr" || t == "--no-hdr-compare" ||
                   t == "--hud-16-9" || t == "--no-hex-tiling")) owned = true;
    const bool takes_value =
        has_value && (t == "--preset" || t == "--shadows" || t == "--difficulty" || t == "--ai-mode" || t == "--camera" ||
                      t == "--size" || t == "--msaa" || t == "--exposure" || t == "--hour" || t == "--play" ||
                      t == "--mods" || t == "--language" || t == "--rumble-scale" || t == "--controller-layout" ||
                      t == "--steer-dz-in" || t == "--steer-dz-out" || t == "--throttle-dz-in" ||
                      t == "--throttle-dz-out" || t == "--brake-dz-in" || t == "--brake-dz-out" ||
                      t == "--assist-braking" || t == "--assist-steering" || t == "--assist-tcs" ||
                      t == "--assist-stm" || t == "--assist-shifting" || t == "--assist-line" ||
                      t == "--assist-rewind" || t == "--master-volume" || t == "--radio-volume" ||
                      t == "--mix-engine" || t == "--mix-other-cars" || t == "--mix-tyres" || t == "--mix-wind" ||
                      t == "--mix-ambience" || t == "--mix-music" || t == "--mix-ui" || t == "--mix-vo" ||
                      t == "--your-music-volume" || t == "--day-length" || t == "--tod-preset" ||
                      t == "--wheel-sat" || t == "--wheel-dz" || t == "--wheel-curve" || t == "--wheel-pedal-dz" ||
                      t == "--wheel-steering" || t == "--wheel-throttle" || t == "--wheel-brake" ||
                      t == "--wheel-clutch" || t == "--wheel-handbrake" || t == "--wheel-buttons" ||
                      t == "--wheel-ffb-mode" || t == "--wheel-ffb-strength" || t == "--wheel-ffb-engine-rumble" ||
                      t == "--wheel-ffb-road-rumble" || t == "--ds-triggers" || t == "--ds-trigger-strength" ||
                      t == "--ds-lightbar" || t == "--gyro-sensitivity" || t == "--head-tracking-port" ||
                      t == "--head-tracking-invert" || t == "--chase-height" || t == "--chase-distance" ||
                      t == "--chase-fov" || t == "--chase-stiffness" || t == "--driving-view" ||
                      t == "--immersive-camera" || t == "--immersive-gforce" || t == "--immersive-lean" ||
                      t == "--immersive-shake" || t == "--immersive-head" || t == "--telemetry-udp-format" ||
                      t == "--telemetry-udp-target" || t == "--trainer-saved-road" || t == "--pacing-prices" ||
                      t == "--pacing-rewards" ||
                      t == "--render-scale" || t == "--aa" || t == "--ssao" || t == "--contact-shadows" ||
                      t == "--tonemapper" || t == "--velocity-blur" || t == "--lut" ||
                      t == "--lut-strength" || t == "--texture-cap" || t == "--vsync" || t == "--fps-cap" ||
                      t == "--output" || t == "--sharpening" || t == "--frame-gen" || t == "--hdr-paper-white" ||
                      t == "--hdr-highlights" || t == "--hdr-peak" || t == "--hud-scale" || t == "--safe-zone");
    if (!owned) {
      other_.push_back(t);
      if (takes_value) other_.push_back(tokens[i + 1]);
    }
    if (takes_value) ++i;
  }
}

void SettingsPage::move(int by) {
  if (capturing_) return;
  const int n = int(rows_.size() + rebinds_.size()) + 1;
  row_ = ((row_ + by) % n + n) % n;
}

void SettingsPage::step(int by) {
  if (capturing_ || row_ >= int(rows_.size())) return;
  Row& r = rows_[size_t(row_)];
  const int n = int(r.choices.size());
  r.pick = size_t(((int(r.pick) + by) % n + n) % n);
}

namespace {
SettingsPage::Tab classify_setting_row(const std::string& name) {
  if (name == "preset" || name == "shadows" || name == "bloom" || name == "camera" || name == "camera effects" ||
      name == "motion blur" || name == "depth of field" || name == "day length" || name == "time of day" ||
      name == "camera editor" || name.rfind("chase ", 0) == 0 || name == "immersive camera" ||
      name == "render scale" || name == "output scaling" || name == "sharpening" || name == "anti-aliasing" ||
      name == "ambient occlusion" || name == "contact shadows" || name == "tonemapper" ||
      name == "terrain hex-tiling" || name == "velocity motion blur" ||
      name == "colour lut" || name == "lut strength" || name == "vrs shading" ||
      name == "texture size" || name == "vsync" || name == "frame rate" || name == "frame gen" ||
      name == "hdr" || name == "windows auto-hdr" || name == "hdr paper white" || name == "hdr highlights" ||
      name == "hdr peak" || name == "hdr compare" || name == "hud 16:9 lock" || name == "hud scale" ||
      name == "safe zone") {
    return SettingsPage::Tab::kDisplay;
  }
  if (name == "radio" || name == "sound" || name == "satnav" || name == "master volume" || name == "mute" ||
      name == "full dynamic range" || name == "radio dj" || name == "radio volume" || name.rfind("mix ", 0) == 0 ||
      name.rfind("your music", 0) == 0) {
    return SettingsPage::Tab::kAudio;
  }
  if (name == "difficulty" || name == "rubber-banding" || name == "traffic" || name == "collectibles" ||
      name == "treasure map" || name == "braking" || name == "steering" || name == "traction control" ||
      name == "stability control" || name == "shifting" || name == "driving line" || name == "rewind" ||
      name == "damage" || name == "ai opponents" || name.rfind("telemetry", 0) == 0) {
    return SettingsPage::Tab::kGameplay;
  }
  if (name == "rumble" || name == "rumble scale" || name == "controller layout" ||
      name.find("deadzone") != std::string::npos || name.rfind("wheel ", 0) == 0 ||
      name.rfind("dualsense ", 0) == 0 || name.rfind("gyro ", 0) == 0 || name.rfind("head tracking", 0) == 0) {
    return SettingsPage::Tab::kControls;
  }
  return SettingsPage::Tab::kPinyon;
}
}  // namespace

SettingsPage::Tab SettingsPage::active_tab() const {
  if (row_ >= 0 && row_ < int(rows_.size())) return classify_setting_row(rows_[size_t(row_)].name);
  if (on_rebind_row()) return Tab::kControls;
  return Tab::kPinyon;
}

void SettingsPage::switch_tab(int by) {
  if (capturing_) return;
  const int count = int(Tab::kCount);
  const int next = ((int(active_tab()) + by) % count + count) % count;
  const Tab target = Tab(next);
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (classify_setting_row(rows_[i].name) == target) {
      row_ = int(i);
      return;
    }
  }
  if (target == Tab::kControls && !rebinds_.empty()) row_ = int(rows_.size());
}

std::vector<SettingsPage::DisplayRow> SettingsPage::display_rows() const {
  std::vector<DisplayRow> out;
  out.reserve(rows_.size() + rebinds_.size() + 1);
  for (size_t i = 0; i < rows_.size(); ++i) {
    out.push_back({rows_[i].name, rows_[i].choices[rows_[i].pick].label, classify_setting_row(rows_[i].name), int(i),
                   false, false, false, row_ == int(i)});
  }
  for (size_t i = 0; i < rebinds_.size(); ++i) {
    const int idx = int(rows_.size() + i);
    const bool cap = capturing_ && row_ == idx;
    out.push_back({rebinds_[i].name, cap ? "PRESS KEY..." : keys_.map[size_t(rebinds_[i].action)].primary,
                   Tab::kControls, idx, true, false, cap, row_ == idx});
  }
  const int save_idx = int(rows_.size() + rebinds_.size());
  out.push_back({"SAVE & APPLY SETTINGS", "CONFIRM", active_tab(), save_idx, false, true, false, row_ == save_idx});
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
  return true;
}

std::vector<std::string> SettingsPage::tokens() const {
  std::vector<std::string> out = other_;
  for (const Row& r : rows_) {
    const auto& w = r.choices[r.pick].tokens;
    out.insert(out.end(), w.begin(), w.end());
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
  {
    const std::string lg = pick_val("language");
    ls.language = lg.empty() ? "EN" : lg;
  }

  const std::string dmg = pick_val("damage");
  if (dmg == "none") ls.damage_mode = 0;
  else if (dmg == "simulation") ls.damage_mode = 2;
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
  c.assists.stability_control = pick_label("stability control") != "off";
  const std::string shf = pick_val("shifting");
  c.assists.shifting = shf.empty() ? 0 : std::atoi(shf.c_str());
  const std::string ln = pick_val("driving line");
  c.assists.driving_line = ln.empty() ? 0 : std::atoi(ln.c_str());
  c.assists.rewind = pick_label("rewind") != "off";
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
  // Apply fine-grained overrides written by Camera Editor / Trainer overlays in settings.txt.
  {
    const std::vector<std::string> toks = tokens();
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
        modern::apply_graphics_option(ls.graphics, toks, i);
      }
    }
    ls.graphics = modern::clamp_graphics(ls.graphics);
  }
  return ls;
}

void SettingsPage::fill(std::string& title, std::vector<std::string>& lines, int& selected) const {
  title = capturing_ ? "press a key to bind (esc cancels)"
                     : "settings: left/right change (live), enter to rebind or save";
  lines.clear();
  for (const Row& r : rows_) lines.push_back(std::string(r.name) + "  < " + r.choices[r.pick].label + " >");
  for (size_t i = 0; i < rebinds_.size(); ++i) {
    const bool cap = capturing_ && row_ == int(rows_.size() + i);
    const std::string& k = keys_.map[size_t(rebinds_[i].action)].primary;
    lines.push_back(std::string(rebinds_[i].name) + (cap ? "  [ press key... ]" : ("  [ " + k + " ]")));
  }
  lines.push_back("save & apply");
  selected = row_;
}

std::string SettingsPage::value(const std::string& name) const {
  for (const Row& r : rows_)
    if (name == r.name) return r.choices[r.pick].label;
  for (const RebindRow& rb : rebinds_)
    if (name == rb.name) return keys_.map[size_t(rb.action)].primary;
  return {};
}

}  // namespace carview
