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
  // R3-UI-11: Adjacent-station carousel metadata (all optional/defaulted).
  std::string prev_station_name;
  std::string prev_station_label;
  std::string next_station_name;
  std::string next_station_label;
  std::string station_frequency;  // e.g. "94.7 FM"
  std::string station_genre;      // e.g. "ELECTRO & HOUSE"
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

// Draws authentic FH1 Race Central / Street Race Hub (H_STREET_RACE_HUB +3.0 deg Event Tickets).
void draw_events_ui4(ui::List& l, float screen_w, float screen_h, const EventsPage& page,
                     const std::vector<garage::ClassBand>& bands);

// Outpost PR Stunt / Mission Ticket card for H_MISSIONS_HUB.
struct OutpostMissionCard {
  std::string title;
  std::string kind_label;   // "SPEED STUNT", "SKILL STUNT", "PHOTO OP", "GARAGE", "FAST TRAVEL"
  int art_style = 10;       // 10=Speed, 11=Skill, 12=Photo, 1=Festival, 2=Street
  int event_id = 0;
  std::string target_line;  // e.g. "TARGET: 145 MPH"
  std::string reward_line;  // e.g. "25,000 Cr  +  2,500 POP"
  std::string status_line;  // e.g. "NEW PR STUNT" or "COMPLETED"
  bool completed = false;
  bool is_stunt = true;
  std::vector<std::string> detail_lines;
  bool is_garage = false;  // the garage service card (its art), not read from the title
};

// Draws authentic FH1 Outpost PR Stunts Hub (H_MISSIONS_HUB +3.0 deg tilted Mission Tickets).
void draw_outpost_hub_ui4(ui::List& l, float screen_w, float screen_h, std::string_view outpost_name,
                          const std::vector<OutpostMissionCard>& cards, int selected_index);

// Draws authentic FH1/Pinyon Shift tabbed Options & live F6 Settings overlay
// (H_FIXED_SETTING_LIST / 300_I_OPT_ASSISTS_DIFFICULTY / 837_I_AUDIO_OPTIONS / 831_C_OPT_CONTROLLER).
void draw_settings_ui4(ui::List& l, float screen_w, float screen_h, const SettingsPage& page,
                       bool live_overlay = false);

// Returns true when `s` is empty or looks like an unresolved ASCII string-table
// identifier key (`IDS_*`, `UI_*`, `STR_*`, `_&*`, `snake_case_id`). Strings
// containing non-ASCII UTF-8 bytes (JP, CHT, CHS, KO, Cyrillic, accented Latin)
// are localized human text and always return false.
bool looks_like_raw_key(std::string_view s);

// ------------------------------------------------ Animated Loading Screen

// The original's load screen (2_C_INI_LOADING, frontend_screens.h): a tip
// card from one LoadingDefs.xml group (its `tipcount` tips, picked at random
// or in file order, each held its `displaytime`), PLEASE WAIT and the
// spinner; no progress bar. On event loads the event's name (an engine
// request) on the scene's big pink title.
class LoadingScreen {
 public:
  static constexpr double kFadeInSec = 0.25;
  static constexpr double kFadeOutSec = 0.25;
  // The original's shortest loads: from the title into free roam, and into
  // and out of an event (fast travel has none).
  static constexpr double kTitleMinSeconds = 1.0;
  static constexpr double kEventMinSeconds = 3.0;
  // A tip's hold when its group gives none (LoadingDefs.xml's displaytime).
  static constexpr double kDefaultTipSeconds = 8.0;

  // `event_title`: the event's name on event loads, else empty. `subtitle`
  // is kept for the stand-in drawing when the scene is missing.
  void start(std::string event_title = {}, std::string subtitle = {});
  void stop();
  bool active() const { return active_; }
  bool fading_out() const { return fading_out_; }
  bool visible() const { return active_ || fading_out_; }
  float veil_alpha() const;

  // Loads `group`'s tips from media/ui/LoadingDefs.xml (the title update's
  // copy in <state_root>/title-update-v4/media.zip first; resolving Tips:IDS_*
  // through game_text()) and picks this load's: the group's `tipcount`, at
  // random or, for `order="sequential"`, the next ones in file order (kept
  // across loads). A missing group falls back to freeroam's.
  bool load_definitions(const std::filesystem::path& game_root, const std::string& group = "freeroam",
                        const std::filesystem::path& state_root = {});
  void set_group(const std::string& group) { group_ = group; }
  const std::string& group() const { return group_; }
  void set_bg_index(int idx) { bg_idx_ = idx; }
  // Replaces the tips (engine tips, tests), each held kDefaultTipSeconds.
  void set_tips(std::vector<std::string> tips);
  // The random picks' seed (tests); a fresh seed otherwise.
  void set_seed(uint32_t seed) { rng_ = seed ? seed : 1u; }
  // Kept for callers; the original's screen shows no progress.
  void set_progress(float p);
  void update(double dt);

  float progress() const { return progress_; }
  double elapsed() const { return elapsed_; }
  int tip_index() const { return tip_idx_; }
  int tip_count() const { return int(tips_.size()); }
  const std::string& current_tip() const;
  const std::string& current_tip_title() const;
  const std::string& event_title() const { return title_; }

  void draw(ui::List& l, float screen_w, float screen_h) const;

 private:
  struct PoolTip {
    std::string title, message;
    double display_time = kDefaultTipSeconds;
  };
  void pick_tips();
  uint32_t next_random();

  bool active_ = false;
  bool fading_out_ = false;
  double fade_out_elapsed_ = 0.0;
  std::string title_;
  std::string subtitle_;
  std::string group_ = "freeroam";
  int bg_idx_ = 1;  // 1..10 -> LOADING/BGLOADINGIMAGES/IMAGE<N>
  // The group's tips and its tipcount / order.
  std::vector<PoolTip> pool_;
  int pool_tip_count_ = 1;
  bool pool_sequential_ = false;
  bool fresh_pick_ = false;  // load_definitions picked this load's tips; start() keeps them
  // This load's tips.
  std::vector<std::string> tip_titles_;
  std::vector<std::string> tips_;
  std::vector<double> tip_times_;
  int tip_idx_ = 0;
  double elapsed_ = 0.0;
  double tip_timer_ = 0.0;
  float progress_ = 0.0f;
  uint32_t rng_ = 0;
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
