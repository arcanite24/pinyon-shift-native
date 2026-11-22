#include "settings_page.h"

#include <algorithm>
#include <cstdlib>

namespace carview {

std::vector<SettingsPage::Row> SettingsPage::build() {
  return {
      {"preset", {{"default", {}}, {"low", {"--preset", "low"}}, {"high", {"--preset", "high"}}}},
      {"shadows",
       {{"default", {}}, {"off", {"--shadows", "off"}}, {"low", {"--shadows", "low"}}, {"high", {"--shadows", "high"}}}},
      {"bloom", {{"default", {}}, {"on", {"--bloom"}}, {"off", {"--no-bloom"}}}},
      {"difficulty",
       {{"default", {}},
        {"easy", {"--difficulty", "easy"}},
        {"medium", {"--difficulty", "med"}},
        {"hard", {"--difficulty", "hard"}},
        {"pro", {"--difficulty", "pro"}}}},
      {"rubber-banding", {{"on", {}}, {"off", {"--no-rubberband"}}}},
      {"traffic", {{"on", {}}, {"off", {"--no-traffic"}}}},
      {"collectibles", {{"on", {}}, {"off", {"--no-collectibles"}}}},
      {"radio", {{"on", {}}, {"off", {"--no-radio"}}}},
      {"sound", {{"on", {}}, {"off", {"--no-sound"}}}},
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

void SettingsPage::load(const std::vector<std::string>& tokens) {
  rows_ = build();
  rebinds_ = build_rebinds();
  keys_ = drive::KeyBindings::defaults();
  other_.clear();
  capturing_ = false;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string& t = tokens[i];
    const bool has_value = i + 1 < tokens.size() && tokens[i + 1].rfind("--", 0) != 0;
    if (t == "--keys" && has_value) {
      keys_.deserialize(tokens[i + 1]);
      ++i;
      continue;
    }
    bool owned = false;
    for (Row& r : rows_)
      for (size_t c = 0; c < r.choices.size(); ++c) {
        const auto& w = r.choices[c].tokens;
        if (w.empty() || w[0] != t) continue;
        if (w.size() == 1 || (has_value && w[1] == tokens[i + 1])) r.pick = c, owned = true;
        else if (w.size() == 2 && has_value) owned = true;  // a value no choice names: the row keeps its pick
      }
    if (!owned && (t == "--traffic" || t == "--rumble" || t == "--camera-effects")) owned = true;
    const bool takes_value =
        has_value && (t == "--preset" || t == "--shadows" || t == "--difficulty" || t == "--camera" ||
                      t == "--size" || t == "--msaa" || t == "--exposure" || t == "--hour" || t == "--play" ||
                      t == "--mods" || t == "--language" || t == "--rumble-scale" || t == "--controller-layout" ||
                      t == "--steer-dz-in" || t == "--steer-dz-out" || t == "--throttle-dz-in" ||
                      t == "--throttle-dz-out" || t == "--brake-dz-in" || t == "--brake-dz-out" ||
                      t == "--assist-braking" || t == "--assist-steering" || t == "--assist-tcs" ||
                      t == "--assist-stm" || t == "--assist-shifting" || t == "--assist-line" ||
                      t == "--assist-rewind");
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

  ls.rubberband = pick_label("rubber-banding") != "off";
  ls.radio = pick_label("radio") != "off";
  ls.sound = pick_label("sound") != "off";

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
