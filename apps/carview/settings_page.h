// The pause menu's settings page: display, audio, camera, controls (rumble,
// layout presets, inside/outside deadzones, rebindable keyboard keys) and
// assist toggles. Changes apply live with no restart where supported, and
// save back to settings.txt (settings.h).
#pragma once

#include <string>
#include <vector>

#include "drive/controls.h"

namespace carview {

struct LiveSettings {
  int camera_mode = -1;  // -1: unchanged/default, 0..4: CameraMode
  int difficulty = -1;   // -1: unchanged/default, 0..4: Easy/Med/Hard/Pro/Hard+
  int ai_mode = 0;       // 0: stock, 1: fair (no rubber-band), 2: hard (+1 tier, max aggro, no rb)
  int damage_mode = 1;   // 0=None, 1=Cosmetic, 2=Simulation
  bool rubberband = true;
  bool radio = true;
  bool sound = true;
  bool satnav = true;
  bool treasure_map = false;
  bool motion_blur = true;
  bool dof = true;
  bool skip_intro_movies = false;
  bool start_in_single_player = false;
  bool skip_vo_with_a = true;
  std::string language = "EN";
  bool photo_through_walls = false;
  bool photo_wider_limits = false;
  double day_length_scale = 1.0;  // 1.0 = 30.5m, 2.0 = 15m, 0.5 = 61m, 0.02116 = realtime, 0.0 = frozen
  double tod_preset_hour = -1.0;  // < 0: keep current clock, >= 0: jump clock to hour
  drive::ControlSettings controls;
};

class SettingsPage {
 public:
  enum class Tab : int { kDisplay = 0, kAudio = 1, kGameplay = 2, kControls = 3, kPinyon = 4, kCount = 5 };

  struct DisplayRow {
    std::string name;
    std::string value;
    Tab tab = Tab::kDisplay;
    int flat_index = 0;
    bool is_rebind = false;
    bool is_save = false;
    bool capturing = false;
    bool selected = false;
  };

  // `tokens`: the file's options as command-line tokens (read_settings).
  void load(const std::vector<std::string>& tokens);
  bool is_open() const { return open_; }
  void open() { open_ = true, row_ = 0, capturing_ = false; }
  void close() { open_ = false, capturing_ = false; }
  void move(int by);  // up and down, wrapping over the rows and the save line
  void step(int by);  // left and right on the selected row
  void switch_tab(int by);  // jumps selection to the first setting in the next/prev category tab
  Tab active_tab() const;
  std::vector<DisplayRow> display_rows() const;
  bool on_save_line() const { return row_ == int(rows_.size() + rebinds_.size()); }
  bool on_rebind_row() const {
    return row_ >= int(rows_.size()) && row_ < int(rows_.size() + rebinds_.size());
  }
  bool capturing_key() const { return capturing_; }
  void begin_rebind();
  bool commit_rebind(const std::string& key_name);
  void cancel_rebind() { capturing_ = false; }
  // The file's tokens with the page's rows and key bindings applied (write_settings).
  std::vector<std::string> tokens() const;
  LiveSettings live_settings() const;
  void fill(std::string& title, std::vector<std::string>& lines, int& selected) const;
  // A row's shown choice, by its name ("difficulty", "key throttle"), for tests.
  std::string value(const std::string& name) const;

 private:
  struct Choice {
    const char* label;
    std::vector<std::string> tokens;  // what the choice writes (none: the default)
  };
  struct Row {
    const char* name;
    std::vector<Choice> choices;
    size_t pick = 0;
  };
  struct RebindRow {
    const char* name;
    drive::ControlAction action;
  };
  std::vector<Row> rows_ = build();
  std::vector<RebindRow> rebinds_ = build_rebinds();
  drive::KeyBindings keys_ = drive::KeyBindings::defaults();
  std::vector<std::string> other_;  // the file's tokens no row owns
  bool open_ = false;
  bool capturing_ = false;
  int row_ = 0;
  static std::vector<Row> build();
  static std::vector<RebindRow> build_rebinds();
};

}  // namespace carview
