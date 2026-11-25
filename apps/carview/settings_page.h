// The pause menu's settings page: display, audio, camera, controls (rumble,
// layout presets, inside/outside deadzones, rebindable keyboard keys) and
// assist toggles. Changes apply live with no restart where supported, and
// save back to settings.txt (settings.h).
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "drive/cameras.h"
#include "drive/controls.h"
#include "drive/dualsense.h"
#include "drive/head_tracking.h"
#include "drive/telemetry_udp.h"
#include "drive/trainer.h"
#include "drive/wheel_ffb.h"
#include "render_modern.h"

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
  bool metric_units = true;  // true: Metric (km/h, km/m), false: Imperial (MPH, mi/ft)
  int hud_speedo_mode = 0;   // 0=Analog, 1=Digital, 2=Off
  int minimap_mode = 0;      // 0=Rotating, 1=North-Up, 2=Off
  std::string language = "EN";
  bool photo_through_walls = false;
  bool photo_wider_limits = false;
  double day_length_scale = 1.0;  // 1.0 = 30.5m, 2.0 = 15m, 0.5 = 61m, 0.02116 = realtime, 0.0 = frozen
  double tod_preset_hour = -1.0;  // < 0: keep current clock, >= 0: jump clock to hour
  drive::ControlSettings controls;
  // Slice 12: Wheels/FFB, DualSense, OpenTrack, Camera Editor, Immersive Camera, Telemetry, Trainer/Pacing/Hidden.
  drive::WheelShaping wheel_shaping;
  drive::WheelFfbConfig wheel_ffb;
  bool wheel_separate_devices = false;
  bool wheel_900_linear = false;
  drive::dualsense::Settings dualsense;
  drive::HeadTrackingConfig head_tracking;
  drive::CameraEditorConfig camera_editor;
  drive::ImmersiveCameraConfig immersive_camera;
  drive::telemetry::Config telemetry_udp;
  bool trainer_cheats = false;
  std::optional<std::array<double, 3>> trainer_saved_road;
  drive::FestivalPacingConfig festival_pacing;
  bool hidden_cars = false;
  std::string graphics_backend = "auto";
  std::string preset;      // empty: default, else "low"|"high"|"ultra"|"low-power"
  std::string shadows;     // empty: default, else "off"|"low"|"high"
  int bloom_mode = -1;     // -1: default, 0: off, 1: on
  bool traffic = true;
  bool collectibles = true;
  modern::GraphicsSettings graphics;
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
  void open();
  void close() { open_ = false, capturing_ = false; }
  void move(int by);  // up and down, wrapping within the active tab (including the save line)
  void step(int by);  // left and right on the selected row
  void switch_tab(int by);  // switches category tab and resets selection to row 0 of that tab
  void select_row(int flat_idx);
  void select_tab(int tab_idx);
  int selected() const { return row_; }
  int selected_in_tab() const;
  int tab_row_count() const;
  Tab active_tab() const { return tab_; }
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
  // Total FH1 difficulty + assists credit bonus percentage (0..135%).
  int difficulty_credit_bonus_pct() const;
  // Syncs row picks from live runtime state (for settings changed outside settings.txt).
  void sync_from_live(const LiveSettings& live);
  bool dirty() const { return dirty_; }
  void mark_clean() { dirty_ = false; }
  bool has_explicit_control_tokens() const { return has_explicit_controls_; }
  void fill(std::string& title, std::vector<std::string>& lines, int& selected) const;
  // A row's shown choice, by its name ("difficulty", "key throttle"), for tests.
  std::string value(const std::string& name) const;
  void set_available_luts(const std::vector<std::string>& names);

  // Optional localization resolver (e.g. bound to carview::game_text at runtime;
  // pure C++ so unit tests stay independent of fh1data / ui4_chrome).
  using TextResolver = std::function<std::string(std::string_view table, std::string_view id, const char* fallback)>;
  static void set_text_resolver(TextResolver fn);
  static std::string tab_label(Tab t);

  // Explicit controller diagram state for ui4::ControllerDiagramParams (no string scraping).
  struct ControllerDiagramSpec {
    bool enabled = false;
    int layout_id = 0;
    bool swap_handbrake_clutch = false;
    std::string highlight_action;
  };
  ControllerDiagramSpec controller_diagram_spec() const;
  template <typename Params>
  void fill_controller_diagram(Params& out) const {
    const ControllerDiagramSpec spec = controller_diagram_spec();
    out.enabled = spec.enabled;
    out.layout_id = spec.layout_id;
    out.swap_handbrake_clutch = spec.swap_handbrake_clutch;
    out.highlight_action = spec.highlight_action;
  }

  // Visual Controls tab controller layout diagram callouts (pure data, no UI dependency).
  struct ControllerDiagramCallout {
    std::string button;      // "LT", "LB", "RT", "RB", "A", "B", "X", "Y", "LS", "RS", "DPAD_LR", "BACK"
    std::string action;      // mapped action name ("BRAKE", "CLUTCH", "GAS", "HANDBRAKE", ...)
    bool left_side = true;   // true: left column of diagram, false: right column
    bool highlighted = false;
  };
  drive::ControllerLayout active_controller_layout() const;
  std::vector<ControllerDiagramCallout> controller_diagram_callouts() const;

 private:
  struct Choice {
    std::string label;
    std::vector<std::string> tokens;  // what the choice writes (none: the default)
  };
  struct Row {
    const char* name;
    std::vector<Choice> choices;
    size_t pick = 0;
    bool explicit_default = false;
  };
  struct RebindRow {
    const char* name;
    drive::ControlAction action;
  };
  bool metric_units_active() const;
  std::string format_row_value(const Row& r) const;
  std::string format_row_name(const char* raw_name) const;
  void apply_difficulty_preset(size_t preset_pick);
  void sync_difficulty_preset();
  std::vector<Row> rows_ = build();
  std::vector<RebindRow> rebinds_ = build_rebinds();
  drive::KeyBindings keys_ = drive::KeyBindings::defaults();
  std::vector<std::string> other_;  // the file's tokens no row owns
  std::vector<std::string> lut_files_;
  bool open_ = false;
  bool capturing_ = false;
  bool dirty_ = false;
  bool has_explicit_controls_ = false;
  Tab tab_ = Tab::kDisplay;
  int row_ = 0;
  std::vector<int> tab_flat_indices(Tab t) const;
  static std::vector<Row> build();
  static std::vector<RebindRow> build_rebinds();
};

}  // namespace carview
