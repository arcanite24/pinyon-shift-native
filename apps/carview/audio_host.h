// Carview audio host (ours, Slice 2): owns the dedicated AudioEngine render
// thread, SDL_AudioStream sink, runtime XMA2 sound loader, off-thread radio
// and Your Music player, and live-applying AudioConfig (Master Volume/Mute,
// Full Dynamic Range, Sound Mix 0-200%, Radio DJ/Volume, Your Music).
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/bake.h"
#include "audio/radio.h"
#include "audio/sources.h"
#include "audio/user_music.h"
#include "drive/save.h"

namespace carview {

struct AudioConfig {
  bool sound = true;
  bool radio_on = true;
  bool radio_dj = true;
  float radio_volume = 1.0f;
  float master_volume = 1.0f;
  bool mute = false;
  bool full_dynamic_range = false;
  std::array<float, audio::kBuses> bus_mix{};
  bool your_music = false;
  bool your_music_shuffle = false;
  float your_music_volume = 1.0f;

  AudioConfig() { bus_mix.fill(1.0f); }

  // Parses one option at `args[i]` (advancing `i` when a value is consumed).
  static bool parse_arg(const std::vector<std::string>& args, size_t& i, AudioConfig& out);
  // Parses a settings token list (e.g. SettingsPage::tokens()) into a fresh AudioConfig.
  static AudioConfig from_tokens(const std::vector<std::string>& tokens);
};

class AudioHost {
 public:
  static constexpr int kAudioRate = 48000;
  static constexpr double kEngineMaster = 0.6;
  static constexpr double kRadioMaster = 1.0;

  AudioHost();
  ~AudioHost();

  const AudioConfig& config() const { return cfg_; }
  void apply_config(const AudioConfig& cfg, const std::filesystem::path& music_dir = {});

  // Loads baked PCM or transparently decodes XMA2 on demand at runtime.
  bool load_car(const std::filesystem::path& sound_dir, const std::filesystem::path& game_root,
                const std::filesystem::path& state_root, const std::string& car, audio::UpgradeLevels levels,
                double idle_rpm, double redline_rpm, std::string* note = nullptr);
  void clear_car();

  // Initializes the radio once (honouring cars::mods and `language`) and
  // attaches a RadioPlayer to AudioEngine.
  bool init_radio(const std::filesystem::path& game_root, std::string_view language, std::string_view saved_station,
                  bool non_blocking);
  const audio::RadioData& radio_data() const { return radio_data_; }
  bool has_radio() const { return radio_ != nullptr; }

  // Initializes Your Music from `music_dir` (<state>/music).
  void init_user_music(const std::filesystem::path& music_dir, bool non_blocking);

  // Opens SDL_AudioStream and starts the dedicated audio render thread.
  void start_device();
  void stop_device();
  bool has_audio() const { return has_car_ || radio_ != nullptr || user_music_ready_; }

  // Per-frame lock-free updates (main thread).
  void update_frame(const audio::EnginePlayer::Update& u, bool paused, bool menu_open, bool on_title,
                    bool loading = false);
  void radio_press(int dir);
  bool handle_key(SDL_Keycode key, SDL_Keymod mod, std::string& notice);
  audio::AudioTelemetry telemetry();

  // Synchronous offline rendering for headless --audio-out.
  void render_offline(const audio::EnginePlayer::Update& u, float* out, int frames);

  // Writes radio station/DJ/volume into `save`.
  void save_radio_state(drive::DriveSave& save);

 private:
  AudioConfig cfg_;
  audio::AudioEngine engine_{kAudioRate};
  SDL_AudioStream* stream_ = nullptr;

  int snap_paused_ = -1;
  int snap_menu_ = -1;
  int snap_title_ = -1;
  int snap_loading_ = -1;

  std::unique_ptr<audio::SoundFiles> sound_files_;
  bool sound_files_tried_ = false;
  std::unique_ptr<audio::BakedSound> baked_sound_;
  bool has_car_ = false;

  audio::RadioData radio_data_;
  std::unique_ptr<audio::Radio> radio_;
  bool radio_tried_ = false;
  bool radio_non_blocking_ = true;

  std::filesystem::path music_dir_;
  bool user_music_ready_ = false;
  bool user_music_non_blocking_ = true;
};

}  // namespace carview
