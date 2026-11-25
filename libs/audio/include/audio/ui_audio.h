// UI sound cues by ui4audio.xml play name (SPEC-AUDIO-EVENTS "UI, rewind
// tape and countdowns"). The game's UI fires symbolic play names; the map
// binds each to a UIInGame.fev / General.fev event, and a scene can swap in
// an override block (PostRace, Trophy, Showroom) for its own play names.
//
// UiAudioCatalog is built once at load and immutable after: the main thread
// resolves (play name, scene) to an event index, and the audio thread plays
// that index, so no strings cross to the audio thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "audio/event_configs.h"
#include "audio/fev.h"

namespace audio {

enum class UiCue : uint8_t;

// The race type the countdown and race-complete cues are chosen by
// (ui4audio's 321MomentStart* / 321Count* / 321Go* / RaceComplete*).
enum class UiRaceKind : uint8_t { kFestival = 0, kShowcase, kStreet, kNemesis };

// The ui4audio play name an engine UiCue fires, or empty when the cue has no
// ui4audio entry (then ui_cue_event_path names the event the game's code
// plays directly, or the cue has no original sound).
std::string_view ui_cue_play_name(UiCue cue, UiRaceKind kind = UiRaceKind::kFestival);
// The FEV event a cue without a ui4audio play name plays directly (the rewind
// tape's stop), or empty.
std::string_view ui_cue_event_path(UiCue cue);
// The wristband award sting for wristband `level` (1..7): CS_WristbandUnlock0N.
std::string ui_wristband_unlock_play(int level);
// Every ui4audio play name the engine's own code can fire (the UiCue map over
// every race kind, the wristband stings and the named widget cues below).
std::vector<std::string> engine_ui_play_names();
// A cutscene's CCutsceneAudioTrigger names its sound by FEV group and event
// (group "UIInGame/UI/Cutscenes", event "CS_OpeningSequence"), not by a
// ui4audio play name: the catalog holds every event under that group too.
inline constexpr std::string_view kCutsceneUiGroup = "UIInGame/UI/Cutscenes";
bool is_cutscene_ui_event(std::string_view path);

// Widget cues the engine fires by name (ui4audio defaults).
namespace ui_play {
inline constexpr std::string_view kAccept = "Accept";
inline constexpr std::string_view kCancel = "Cancel";
inline constexpr std::string_view kDenyAccess = "DenyAccess";
inline constexpr std::string_view kGridScroll = "GridScroll";
inline constexpr std::string_view kVscroll = "Vscroll";
inline constexpr std::string_view kHscroll = "Hscroll";
inline constexpr std::string_view kTab = "Tab";
inline constexpr std::string_view kSliderStepIncrement = "SliderStep_Increment";
inline constexpr std::string_view kSliderStepDecrement = "SliderStep_Decrement";
inline constexpr std::string_view kSpinner = "Spinner";
inline constexpr std::string_view kSpinnerEnd = "SpinnerEnd";
inline constexpr std::string_view kToggleView = "ToggleView";
inline constexpr std::string_view kMenuVisible = "MenuVisible";
inline constexpr std::string_view kMenuHide = "MenuHide";
inline constexpr std::string_view kPause = "Pause";
inline constexpr std::string_view kUnPause = "UnPause";
inline constexpr std::string_view kPopUp = "PopUp";
inline constexpr std::string_view kPopupError = "PopupError";
inline constexpr std::string_view kMapSnap = "Map_Snap";
inline constexpr std::string_view kMapRouteAdd = "Map_RouteAdd";
inline constexpr std::string_view kMapRouteRemove = "Map_RouteRemove";
inline constexpr std::string_view kTakePhotoAccept = "TakePhotoAccept";
inline constexpr std::string_view kCameraFocus = "CameraFocus";
inline constexpr std::string_view kLoadingScreen = "LoadingScreen";
}  // namespace ui_play

class UiAudioCatalog {
 public:
  // One sound an event plays: a UI event's layers start together (the
  // countdowns are five mono channels of one 5.1 mix, a race-complete sting
  // is a stereo stream plus an LFE layer).
  struct Layer {
    std::filesystem::path bank;  // the .fsb holding the sample
    uint32_t subsound = 0;
    float gain = 1.0f;           // event volume x instance volume x the layer's volume envelopes at rest
    float left = 0.70710678f;    // stereo placement from the layer's surround pan
    float right = 0.70710678f;
    int sample = -1;             // catalog-wide sample slot (SfxEngine::trigger_ui_sample)
  };
  struct Event {
    std::string path;            // "<project>/<group>/.../<event>" as on disc
    bool looping = false;        // ui4audio isLooping
    bool on_disc = false;        // the FEV event exists (some are empty on the disc: silent in the game)
    bool playable = false;       // the event and a wavetable sample were found
    std::vector<Layer> layers;
  };
  static constexpr int kUnknown = -1;  // no such play name
  static constexpr int kSilent = -2;   // a Blank entry, or an event with no sample

  // Reads ui4audio.xml and resolves its events in UI/UIInGame/UIInGame.fev and
  // gamemodes/General.fev under `game_root`/media/audio.
  bool load(const std::filesystem::path& game_root, std::string* error = nullptr);
  // Builds from an already parsed map and projects (tests).
  void build(const UiAudioMap& map, const std::vector<const FevProject*>& projects);

  const UiAudioMap& map() const { return map_; }
  const std::vector<Event>& events() const { return events_; }
  bool empty() const { return events_.empty(); }
  // The event `play` resolves to with `scene`'s overrides applied.
  int resolve(std::string_view play, std::string_view scene = {}) const;
  // The event index of an FEV path (case-insensitive), or kUnknown.
  int find_event(std::string_view path) const;
  // The playable event at an FEV path, kSilent when it has no sample, else kUnknown.
  int resolve_event(std::string_view path) const;
  const Event* event(int index) const {
    return index >= 0 && size_t(index) < events_.size() ? &events_[size_t(index)] : nullptr;
  }
  int sample_count() const { return samples_; }

 private:
  int intern(const std::string& path, const std::vector<const FevProject*>& projects, bool looping);
  UiAudioMap map_;
  std::vector<Event> events_;
  int samples_ = 0;
};

// The stereo placement of a layer from its surround-pan envelopes (left/right
// and back/front, 1 = left / front, read at rest), else centred; a layer sent
// through a Speaker Level unit (the LFE stems) folds to the centre.
void fev_layer_pan(const FevLayer& layer, float& left, float& right);

}  // namespace audio
