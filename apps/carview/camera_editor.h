// Live Camera Editor, Immersive Camera, and OpenTrack Head Tracking UI4
// settings overlay (integrated into F6 Settings using ui4_chrome.*).
#pragma once

#include <string>
#include <vector>

#include "drive/cameras.h"
#include "drive/head_tracking.h"
#include "ui.h"

namespace carview {

class CameraEditorOverlay {
 public:
  enum class Tab : int { kCameraEditor = 0, kImmersive = 1, kHeadTracking = 2, kCount = 3 };

  struct RowView {
    std::string label;
    std::string value;
    std::string detail;
    double norm_bar = -1.0;  // 0..1 when numeric slider, <0 for toggle/choice/action
    bool is_action = false;
    bool selected = false;
  };

  void open(const drive::CameraEditorConfig& ed, const drive::ImmersiveCameraConfig& imm,
            const drive::HeadTrackingConfig& ht);
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  void switch_tab(int by);
  void set_tab(Tab t);
  Tab active_tab() const { return tab_; }

  void move(int by);
  void step(int by);
  bool activate();  // returns true if reset/save action executed

  const drive::CameraEditorConfig& editor_config() const { return editor_; }
  const drive::ImmersiveCameraConfig& immersive_config() const { return immersive_; }
  const drive::HeadTrackingConfig& head_tracking_config() const { return head_; }
  const std::string& status_message() const { return status_; }

  std::vector<RowView> rows() const;
  int selected_row() const { return row_; }

 private:
  int row_count() const;

  bool open_ = false;
  Tab tab_ = Tab::kCameraEditor;
  int row_ = 0;
  drive::CameraEditorConfig editor_;
  drive::ImmersiveCameraConfig immersive_;
  drive::HeadTrackingConfig head_;
  std::string status_;
};

// Draws the authentic FH1 UI4 Live Camera Editor & Immersive Camera overlay.
void draw_camera_editor_ui4(ui::List& l, float screen_w, float screen_h, const CameraEditorOverlay& overlay,
                            const drive::HeadPose& live_head_pose = {});

}  // namespace carview
