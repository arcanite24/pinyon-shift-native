// F10 Trainer UI4 Overlay (authentic ui4_chrome.* presentation):
// Tabbed Player / World / Vehicle / Debug menu operating strictly on the
// isolated `user-modded` cheat profile tree, plus Festival Pacing & Hidden Cars.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "drive/trainer.h"
#include "ui.h"

namespace carview {

struct TrainerActionOutcome {
  bool close_overlay = false;
  bool credits_changed = false;
  bool wristband_changed = false;
  bool saved_road_changed = false;
  bool settings_dirty = false;
  bool isolation_triggered = false;
  std::string message;
};

class TrainerOverlay {
 public:
  enum class Tab : int { kPlayer = 0, kWorld = 1, kVehicle = 2, kDebug = 3, kCount = 4 };

  struct RowView {
    std::string label;
    std::string value;
    std::string detail;
    bool enabled = true;
    bool is_action = false;
    bool selected = false;
  };

  void open() { open_ = true, row_ = 0; }
  void close() { open_ = false; }
  void toggle() { open_ ? close() : open(); }
  bool is_open() const { return open_; }

  void switch_tab(int by);
  void set_tab(Tab t);
  Tab active_tab() const { return tab_; }

  void move(int by);
  void step(int by, drive::TrainerState& state);
  TrainerActionOutcome activate(drive::TrainerState& state, int64_t& credits, int& wristband_points,
                                int& collected_wristband, double now_s, bool in_race);

  std::vector<RowView> rows(const drive::TrainerState& state) const;
  int selected_row() const { return row_; }
  const std::string& last_message() const { return last_message_; }

 private:
  int row_count() const;

  bool open_ = false;
  Tab tab_ = Tab::kPlayer;
  int row_ = 0;
  int set_credits_idx_ = 2;  // 1,000,000 CR default
  int add_credits_idx_ = 1;  // +1,000,000 CR default
  int wristband_idx_ = 0;    // Yellow..Gold
  std::string last_message_;
};

// Draws the authentic FH1 UI4 F10 Trainer overlay (Player / World / Vehicle / Debug).
void draw_trainer_ui4(ui::List& l, float screen_w, float screen_h, const TrainerOverlay& overlay,
                      const drive::TrainerState& state, int64_t credits, int wristband_level,
                      const std::string& save_tree_label = "user-modded");

}  // namespace carview
