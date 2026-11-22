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
  int difficulty = -1;   // -1: unchanged/default, 0..3: Easy/Med/Hard/Pro
  bool rubberband = true;
  bool radio = true;
  bool sound = true;
  double day_length_scale = 1.0;  // 1.0 = 30.5m, 2.0 = 15m, 0.5 = 61m, 0.02116 = realtime, 0.0 = frozen
  double tod_preset_hour = -1.0;  // < 0: keep current clock, >= 0: jump clock to hour
  drive::ControlSettings controls;
};

class SettingsPage {
 public:
  // `tokens`: the file's options as command-line tokens (read_settings).
  void load(const std::vector<std::string>& tokens);
  bool is_open() const { return open_; }
  void open() { open_ = true, row_ = 0, capturing_ = false; }
  void close() { open_ = false, capturing_ = false; }
  void move(int by);  // up and down, wrapping over the rows and the save line
  void step(int by);  // left and right on the selected row
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
