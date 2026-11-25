#include "audio/ui_audio.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include "audio/sfx_engine.h"
#include "cars/mods.h"

namespace audio {
namespace {

bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// The first wavetable entry of a sound definition (an entry list with several
// entries is the event's random variation; the first stands for them all).
bool wavetable_of(const FevProject& proj, uint16_t sound_def, std::filesystem::path& bank, uint32_t& sub) {
  if (sound_def >= proj.sound_defs.size()) return false;
  for (const auto& en : proj.sound_defs[sound_def].entries) {
    if (en.entry_type != 0 || en.wavebank_name.empty()) continue;
    bank = cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb"));
    sub = en.subsound_index;
    return true;
  }
  return false;
}

// The sounds an event plays at rest: every instance audible with each
// parameter at its minimum, one per layer and placed by the layer's pan.
std::vector<UiAudioCatalog::Layer> resting_layers(const FevProject& proj, const FevEvent& ev) {
  std::vector<UiAudioCatalog::Layer> out;
  auto add = [&](uint16_t sound_def, float gain, const FevLayer* ly) {
    UiAudioCatalog::Layer l;
    if (!wavetable_of(proj, sound_def, l.bank, l.subsound)) return;
    l.gain = gain;
    if (ly) fev_layer_pan(*ly, l.left, l.right);
    out.push_back(std::move(l));
  };
  if (ev.is_simple()) {
    // A simple event's instances are its random alternatives: play the first.
    if (!ev.simple_instances.empty()) {
      const auto& si = ev.simple_instances.front();
      add(si.sound_def_index, std::max(0.0f, ev.volume) * std::max(0.0f, si.volume), nullptr);
    }
    return out;
  }
  std::array<FevInstanceLevel, 32> lv{};
  const int n = evaluate_fev_event(ev, nullptr, 0, lv.data(), int(lv.size()));
  for (int i = 0; i < n; ++i) {
    const auto& l = lv[size_t(i)];
    if (l.gain > 0.0f) add(l.sound_def, l.gain, &ev.layers[l.layer]);
  }
  return out;
}

}  // namespace

void fev_layer_pan(const FevLayer& layer, float& left, float& right) {
  constexpr float kCentre = 0.70710678f;
  left = right = kCentre;
  for (const auto& e : layer.envelopes)
    if (e.parent_index < 0 && e.dsp_name == "Speaker Level") return;  // an LFE stem
  for (size_t r = 0; r < layer.envelopes.size(); ++r) {
    const auto& root = layer.envelopes[r];
    if (root.parent_index >= 0 || !root.dsp_name.empty() || !(root.flags & kFevEnvSurroundPan)) continue;
    float lr = 0.5f, fb = 1.0f;
    auto take = [&](const FevEnvelope& e) {
      const float y = std::clamp(e.eval(0.0f), 0.0f, 1.0f);
      if (e.dsp_param_index == 0) lr = y;
      else if (e.dsp_param_index == 1) fb = y;
    };
    take(root);
    for (const auto& e : layer.envelopes)
      if (e.parent_index == int32_t(r)) take(e);
    // Constant-power left/right; a rear stem folds into its side at -3 dB.
    constexpr float kHalfPi = 1.57079633f;
    const float rear = kCentre + (1.0f - kCentre) * fb;
    left = std::max(0.0f, std::sin(lr * kHalfPi)) * rear;
    right = std::max(0.0f, std::cos(lr * kHalfPi)) * rear;
    return;
  }
}

std::string_view ui_cue_play_name(UiCue cue, UiRaceKind kind) {
  switch (cue) {
    case UiCue::kAccept: return ui_play::kAccept;
    case UiCue::kCancel: return ui_play::kCancel;
    case UiCue::kMove: return ui_play::kGridScroll;
    case UiCue::kError: return ui_play::kDenyAccess;
    case UiCue::kMenuOpen: return ui_play::kMenuVisible;
    case UiCue::kRewindTapeStart: return "Skip";  // ui4audio binds Skip to Mulligan/TapeStart
    case UiCue::kCountdown321:
      switch (kind) {
        case UiRaceKind::kShowcase: return "321MomentStartShowcase";
        case UiRaceKind::kStreet: return "321MomentStartStreet";
        default: return "321MomentStartFestival";
      }
    case UiCue::kCountdownTick:
      switch (kind) {
        case UiRaceKind::kShowcase: return "321CountShowCase";
        case UiRaceKind::kStreet: return {};  // ui4audio has no street count cue
        default: return "321CountFestival";
      }
    case UiCue::kCountdownGo:
      switch (kind) {
        case UiRaceKind::kShowcase: return "321GoShowcase";
        case UiRaceKind::kStreet: return "321GoStreet";
        default: return "321GoFestival";
      }
    case UiCue::kRaceFinish:
      switch (kind) {
        case UiRaceKind::kShowcase: return "RaceCompleteShowcase";
        case UiRaceKind::kStreet: return "RaceCompleteStreet";
        case UiRaceKind::kNemesis: return "RaceCompleteNemesis";
        default: return "RaceCompleteFestival";
      }
    // A lap line is a checkpoint: ui4audio has no lap cue of its own.
    case UiCue::kCheckpoint:
    case UiCue::kLap: return "CheckpointPassed";
    case UiCue::kSignSmash: return "Hud_Collection";
    case UiCue::kSkillAward: return "Skills_Slam";
    case UiCue::kSkillMultiplier: return "Multi_Slam";
    case UiCue::kSkillBank: return "Bank_Into_Pool";
    case UiCue::kSkillBreak: return "Lose_Points";
    case UiCue::kRankUp: return "Hud_FameLevelUp";
    case UiCue::kAchievementReward: return "Hud_GeneralNotificationAppear";
    case UiCue::kSpeedCamera: return "SpeedTrapFlash";
    case UiCue::kPhotoShutter: return ui_play::kTakePhotoAccept;
    case UiCue::kPurchase: return "Hud_Buy";
    case UiCue::kCarAdded: return "CarAddedToGarage";
    case UiCue::kH2hGo: return "RaceEncounter_Go";
    case UiCue::kH2hLoss: return "RaceEncounter_Lost";
    // Per wristband: ui_wristband_unlock_play(level) (the host knows the level).
    case UiCue::kWristbandUnlock:
    case UiCue::kRewindTapeStop: return {};
  }
  return {};
}

std::string_view ui_cue_event_path(UiCue cue) {
  // The rewind controller plays the tape's stop itself (no ui4audio entry).
  if (cue == UiCue::kRewindTapeStop) return "UIInGame/UI/Mulligan/TapeStop";
  return {};
}

std::string ui_wristband_unlock_play(int level) {
  const int l = std::clamp(level, 1, 7);
  return "CS_WristbandUnlock0" + std::to_string(l);
}

std::vector<std::string> engine_ui_play_names() {
  std::vector<std::string> out;
  auto add = [&](std::string_view s) {
    if (s.empty()) return;
    for (const auto& o : out)
      if (iequal(o, s)) return;
    out.emplace_back(s);
  };
  for (int c = 0; c <= int(UiCue::kWristbandUnlock); ++c)
    for (int k = 0; k <= int(UiRaceKind::kNemesis); ++k) add(ui_cue_play_name(UiCue(c), UiRaceKind(k)));
  for (int l = 1; l <= 7; ++l) add(ui_wristband_unlock_play(l));
  for (std::string_view s :
       {ui_play::kAccept, ui_play::kCancel, ui_play::kDenyAccess, ui_play::kGridScroll, ui_play::kVscroll,
        ui_play::kHscroll, ui_play::kTab, ui_play::kSliderStepIncrement, ui_play::kSliderStepDecrement,
        ui_play::kSpinner, ui_play::kSpinnerEnd, ui_play::kToggleView, ui_play::kMenuVisible, ui_play::kMenuHide,
        ui_play::kPause, ui_play::kUnPause, ui_play::kPopUp, ui_play::kPopupError, ui_play::kMapSnap,
        ui_play::kMapRouteAdd, ui_play::kMapRouteRemove, ui_play::kTakePhotoAccept, ui_play::kCameraFocus,
        ui_play::kLoadingScreen})
    add(s);
  return out;
}

int UiAudioCatalog::find_event(std::string_view path) const {
  for (size_t i = 0; i < events_.size(); ++i)
    if (iequal(events_[i].path, path)) return int(i);
  return kUnknown;
}

int UiAudioCatalog::intern(const std::string& path, const std::vector<const FevProject*>& projects, bool looping) {
  if (const int at = find_event(path); at >= 0) {
    events_[size_t(at)].looping = events_[size_t(at)].looping || looping;
    return at;
  }
  Event e;
  e.path = path;
  e.looping = looping;
  for (const FevProject* proj : projects) {
    const FevEvent* ev = proj ? proj->find_event(path) : nullptr;
    if (!ev) continue;
    e.layers = resting_layers(*proj, *ev);
    for (auto& l : e.layers) l.sample = samples_++;
    e.on_disc = true;
    e.playable = !e.layers.empty();
    break;
  }
  events_.push_back(std::move(e));
  return int(events_.size()) - 1;
}

void UiAudioCatalog::build(const UiAudioMap& map, const std::vector<const FevProject*>& projects) {
  map_ = map;
  events_.clear();
  samples_ = 0;
  auto add_entries = [&](const std::vector<UiAudioEntry>& list) {
    for (const auto& e : list)
      if (!e.is_blank()) intern(e.full_path(), projects, e.is_looping);
  };
  add_entries(map_.defaults);
  for (const auto& [name, list] : map_.overrides) add_entries(list);
  for (int c = 0; c <= int(UiCue::kWristbandUnlock); ++c) {
    const std::string_view p = ui_cue_event_path(UiCue(c));
    if (!p.empty()) intern(std::string(p), projects, false);
  }
}

bool UiAudioCatalog::load(const std::filesystem::path& game_root, std::string* error) {
  UiAudioMap map;
  if (!map.load(game_root, error)) return false;
  const auto audio_dir = game_root / "media" / "audio";
  FevProject ui, general;
  const bool have_ui = load_fev(audio_dir / "UI" / "UIInGame" / "UIInGame.fev", ui, error);
  const bool have_general = load_fev(audio_dir / "gamemodes" / "General.fev", general, nullptr);
  std::vector<const FevProject*> projects;
  if (have_ui) projects.push_back(&ui);
  if (have_general) projects.push_back(&general);
  build(map, projects);
  return have_ui;
}

int UiAudioCatalog::resolve(std::string_view play, std::string_view scene) const {
  const UiAudioEntry* e = map_.resolve(play, scene);
  if (!e) return kUnknown;
  if (e->is_blank()) return kSilent;
  const int at = find_event(e->full_path());
  if (at < 0 || !events_[size_t(at)].playable) return kSilent;
  return at;
}

}  // namespace audio
