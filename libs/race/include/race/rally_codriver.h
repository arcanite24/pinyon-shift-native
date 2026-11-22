// Horizon Rally Co-Driver Pace Notes (SPEC-V4-CONTENT, Rally pace-note route data):
// Parses CoDriverAudio.xml (sample id -> FMOD group, cue, icon) and ColoradoDirt
// TrackRouteNNN.xml RallyCall trigger gates, detects forward gate crossings in
// order (supporting rewind), queues audio cues for CoDriverVoPlayer, and builds
// authentic directional pace-note HUD state (1-6 left/right, crest, jump,
// caution, hairpin, square).
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace race {

struct CoDriverAudioEntry {
  std::string play_id;  // Sample id in TrackRouteNNN.xml (e.g. "MediumRight", "50", "Caution")
  std::string group;    // FMOD group (e.g. "CoDriver/Turns/Right")
  std::string cue;      // FMOD cue name (e.g. "MediumRight")
  std::string icon;     // Icon name (e.g. "MediumRight", "HairpinLeft", "None")
};

struct CoDriverAudioMap {
  std::map<std::string, CoDriverAudioEntry> by_id;  // lowercase play_id -> entry
  const CoDriverAudioEntry* find(std::string_view play_id) const;
};

struct RallyCallGate {
  std::string name;
  double x = 0.0, y = 0.0, z = 0.0;
  double fx = 0.0, fz = 1.0;  // unit horizontal facing along road
  double width = 20.0;        // trigger gate width (m)
  std::vector<std::string> sample_ids;
};

bool parse_codriver_audio_xml(std::string_view xml, CoDriverAudioMap& out, std::string* error = nullptr);
bool load_codriver_audio_map(const std::filesystem::path& rally_package_dir, CoDriverAudioMap& out,
                             std::string* error = nullptr);

// Parses <RallyCalls><RallyCall ...> blocks from a ColoradoDirt TrackRouteNNN.xml.
bool parse_rally_calls_xml(std::string_view xml, std::vector<RallyCallGate>& out, std::string* error = nullptr);
// Loads TrackRouteNNN.xml from either `stage_world/Ribbon_00/` (loose) or
// `rally_package_dir/Media/DLCZips/1600_pri_65.zip`.
bool load_rally_calls(const std::filesystem::path& stage_world, const std::filesystem::path& rally_package_dir,
                      int route_id, std::vector<RallyCallGate>& out, std::string* error = nullptr);

// Normalized pace-note icon for HUD rendering.
enum class PaceIconKind : uint8_t {
  kNone = 0,
  kEasyLeft,       // 6/5 Left (green)
  kEasyRight,      // 6/5 Right (green)
  kMediumLeft,     // 4/3 Left (yellow)
  kMediumRight,    // 4/3 Right (yellow)
  kHardLeft,       // 2 Left (orange)
  kHardRight,      // 2 Right (orange)
  kSquareLeft,     // 90 deg Left (red)
  kSquareRight,    // 90 deg Right (red)
  kHairpinLeft,    // 180 deg Left (red)
  kHairpinRight,   // 180 deg Right (red)
  kCrest,          // Crest / Over Crest
  kJump,           // Jump / Big Jump
  kCaution,        // Caution / Hazard
};

struct PaceNoteIcon {
  PaceIconKind kind = PaceIconKind::kNone;
  int severity = 0;         // 1 (Hairpin/Square) .. 6 (Easy), 0 for hazards
  int dir = 0;              // -1 left, +1 right, 0 hazard
  std::string label;        // short text: "6L", "4R", "2L", "SQ R", "HP L", "CREST", "JUMP", "!"
  bool active = false;
};

struct PaceNoteHudState {
  bool active = false;
  std::vector<PaceNoteIcon> icons;
  std::vector<std::string> modifiers;  // e.g. "50", "DON'T CUT", "CREST", "CAUTION", "GRAVEL"
  std::string phrase;                  // e.g. "HARD LEFT  •  DON'T CUT  •  50"
  double age_s = 0.0;
};

struct CoDriverAudioCue {
  std::string sample_id;
  std::string group;
  std::string cue;
};

// Classifies a CoDriverAudio icon/sample id into a HUD icon.
PaceNoteIcon classify_pace_icon(std::string_view icon_name, std::string_view sample_id);

class RallyPaceNoteTracker {
 public:
  RallyPaceNoteTracker() = default;
  void load(std::vector<RallyCallGate> gates, const CoDriverAudioMap& audio_map);
  void reset();

  size_t gate_count() const { return gates_.size(); }
  size_t fired_count() const;

  // Advances with the car's position (game coordinates) and race clock `time_s`.
  // Crossed gates enqueue their ordered audio cues & update HUD state.
  // Rewinding (`time_s < last_time_s - 0.05`) un-fires gates crossed after `time_s`.
  int observe(double time_s, double x, double z);

  // Pops the next queued audio cue to play on CoDriverVoPlayer (if any).
  bool pop_audio_cue(CoDriverAudioCue& out);
  size_t pending_audio_cues() const { return audio_queue_.size(); }

  // Advances active sample highlight as audio finishes a cue.
  void on_audio_cue_finished(double time_s);

  // Returns the current HUD pace-note strip (fades after `hold_seconds`).
  PaceNoteHudState hud(double time_s, double hold_seconds = 4.5) const;

 private:
  struct FiredCall {
    size_t gate_index = 0;
    double fired_at = 0.0;
  };

  std::vector<RallyCallGate> gates_;
  CoDriverAudioMap audio_;
  std::vector<std::optional<double>> fired_at_;

  bool has_prev_ = false;
  double prev_x_ = 0.0, prev_z_ = 0.0;
  double last_time_ = 0.0;

  std::deque<CoDriverAudioCue> audio_queue_;
  int active_gate_ = -1;
  double active_since_ = -1e9;
  size_t active_sample_idx_ = 0;
};

}  // namespace race
