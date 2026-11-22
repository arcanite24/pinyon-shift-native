// Authentic UI4 scene parity renderers, GPU vector-font radio popup, animated
// loading screen with cycling tips, and virtual on-screen keyboard (Slice 9).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fh1data/ui_scene.h"
#include "hud_scene.h"
#include "garage/garage.h"
#include "ui.h"

namespace carview {
class EventsPage;
class SettingsPage;

// ------------------------------------------------ GPU Vector-Font Radio Popup

struct RadioPopupGpuState {
  std::string station_name;   // "Radio1".."Radio4_Silent"
  std::string station_label;  // e.g. "HORIZON BASS ARENA" or "RADIO OFF"
  std::string title;
  std::string artist;
  bool off = false;
  float alpha = 0.0f;
};

// Draws the radio popup on the GPU UI layer using Loop-Blinn vector fonts / CJK
// bitmap fonts and Horizon.zip RS_* station logos (replacing CPU 5x7 draw_radio_popup).
void draw_radio_popup_gpu(ui::List& l, const RadioPopupGpuState& st, float screen_w, float screen_h);

// ------------------------------------------------ UI4 Scene Parity Renderers

struct ButtonBarItem {
  const char* button = "A";  // "A", "B", "X", "Y"
  std::string label;
};

// Draws BUTTON_BAR_SCENE (authentic Horizon pill + controller face glyphs).
void draw_button_bar_ui4(ui::List& l, const std::vector<ButtonBarItem>& items, float screen_w, float screen_h);

struct PauseMenuUi4State {
  std::vector<std::string> items;
  int selected = 0;
  float selection_anim = 1.0f;  // 0..1 cross-fade wipe on selection change
  std::string wristband_goal;
  std::string wristband_label;
  uint32_t wristband_colour = ui::rgba(0x0A, 0x9B, 0xFA);
  std::string car_name;
  std::string car_class;
  int car_pi = 0;
  std::string credits_text;
  std::vector<ButtonBarItem> help_bar;
};

// Draws 925_PAUSE_MENU using its loaded FBF/BSG/BGF scene (SUPER_STACKER 50 px
// pitch reflow, pink/black tape brushes, top-right career strip, BUTTON_BAR_SCENE).
void draw_pause_menu_ui4(ui::List& l, const PauseMenuUi4State& st, float screen_w, float screen_h);

struct MapChromeUi4State {
  std::string filter_label;
  std::string roads_label;  // e.g. "ROADS DISCOVERED 142 / 216"
  std::string hovered_title;
  std::string hovered_subtitle;
  std::vector<ButtonBarItem> help_bar;
};

// Draws H_MAP authentic UI4 chrome (header strip, roads counter, hover card, help bar).
void draw_map_chrome_ui4(ui::List& l, const MapChromeUi4State& st, float screen_w, float screen_h);

struct PrePostRaceUi4State {
  bool is_post_race = false;
  std::string event_type;
  std::string event_name;
  std::string place_text;   // e.g. "1ST PLACE" (post-race)
  std::vector<std::string> menu_items;
  int selected = 0;
  std::vector<ButtonBarItem> help_bar;
};

// Draws PRE_AND_POST_RACE authentic UI4 scene chrome (event card, grid menu / finish banner).
void draw_pre_post_race_ui4(ui::List& l, const PrePostRaceUi4State& st, float screen_w, float screen_h);

struct StreetRaceHubUi4State {
  std::string hub_name;
  std::string hub_subtitle;
  struct Entry {
    std::string name;
    std::string stake;
    std::string car_class;
    bool completed = false;
  };
  std::vector<Entry> races;
  int selected = 0;
  std::vector<ButtonBarItem> help_bar;
};

// Draws H_STREET_RACE_HUB authentic UI4 scene overlay.
void draw_street_race_hub_ui4(ui::List& l, const StreetRaceHubUi4State& st, float screen_w, float screen_h);

// Draws authentic FH1 Race Central / Events browser (tab strip + event list + detail card).
void draw_events_ui4(ui::List& l, float screen_w, float screen_h, const EventsPage& page,
                     const std::vector<garage::ClassBand>& bands);

// Draws authentic FH1/Pinyon Shift tabbed Options & live F6 Settings overlay.
void draw_settings_ui4(ui::List& l, float screen_w, float screen_h, const SettingsPage& page,
                       bool live_overlay = false);

// ------------------------------------------------ Animated Loading Screen

class LoadingScreen {
 public:
  void start(std::string destination_title, std::string subtitle = {});
  void stop() { active_ = false; }
  bool active() const { return active_; }

  // Loads categorized tips from media/ui/LoadingDefs.xml (resolving Tips:IDS_*
  // through game_text() / string tables when available) and selects matching
  // LOADING/BGLOADINGIMAGES/* & LOADING/TIPIMAGES/* assets.
  bool load_definitions(const std::filesystem::path& game_root, const std::string& group = "freeroam");
  void set_group(const std::string& group) { group_ = group; }
  void set_bg_index(int idx) { bg_idx_ = idx; }
  void set_tips(std::vector<std::string> tips);
  void set_progress(float p);
  void update(double dt);

  float progress() const { return progress_; }
  double elapsed() const { return elapsed_; }
  int tip_index() const { return tip_idx_; }
  const std::string& current_tip() const;
  const std::string& current_tip_title() const;

  void draw(ui::List& l, float screen_w, float screen_h) const;

 private:
  bool active_ = false;
  std::string title_;
  std::string subtitle_;
  std::string group_ = "freeroam";
  int bg_idx_ = 1;  // 1..10 -> LOADING/BGLOADINGIMAGES/IMAGE<N>
  std::vector<std::string> tip_titles_;
  std::vector<std::string> tips_;
  int tip_idx_ = 0;
  double elapsed_ = 0.0;
  double tip_timer_ = 0.0;
  float progress_ = 0.0f;
};

// ------------------------------------------------ Virtual On-Screen Keyboard

class OnScreenKeyboard {
 public:
  enum class Purpose { kGamertag, kLicensePlate, kLiveryName };

  void open(std::string title, std::string initial_text, int max_length = 20, Purpose purpose = Purpose::kGamertag);
  void close() { open_ = false; }
  bool is_open() const { return open_; }
  bool confirmed() const { return confirmed_; }
  bool cancelled() const { return cancelled_; }

  void move(int dcol, int drow);
  void press_selected();
  void type_char(char c);
  void backspace();
  void toggle_shift() { shifted_ = !shifted_; }
  void toggle_symbols() { symbols_ = !symbols_; }
  void confirm();
  void cancel();
  void update(double dt) { blink_t_ += dt; }

  const std::string& text() const { return text_; }
  const std::string& title() const { return title_; }
  int cursor_row() const { return row_; }
  int cursor_col() const { return col_; }
  bool shifted() const { return shifted_; }
  bool symbols() const { return symbols_; }

  void draw(ui::List& l, float screen_w, float screen_h) const;

 private:
  std::vector<std::vector<std::string>> active_layout() const;

  bool open_ = false;
  bool confirmed_ = false;
  bool cancelled_ = false;
  bool shifted_ = true;
  bool symbols_ = false;
  Purpose purpose_ = Purpose::kGamertag;
  std::string title_;
  std::string text_;
  int max_len_ = 20;
  int row_ = 0;
  int col_ = 0;
  double blink_t_ = 0.0;
};

}  // namespace carview
