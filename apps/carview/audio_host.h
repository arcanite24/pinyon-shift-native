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
  bool satnav_on = true;
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

  // Loads AudioMixerSnapshots/GlobalDSP/AudioEngineConfig + FEV/FSB4 SfxEngine
  // (async background FSB decode with disk cache).
  bool init_sfx(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                const std::string& burble_bank = {});
  bool init_satnav(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                   std::string_view language = "EN", bool metric = true);
  bool init_codriver(const std::filesystem::path& rally_package_dir, const std::filesystem::path& cache_dir,
                     std::string_view language = "EN");
  bool init_vo(const std::filesystem::path& game_root, std::string_view language = "EN");
  bool init_acoustics(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir = {},
                      const std::filesystem::path& world_dir = {}, const std::filesystem::path& rally_package = {});
  const std::string& acoustics_world_key() const { return acoustics_world_key_; }
  size_t acoustics_reverb_zones() const { return acoustics_.reverb_zone_count(); }
  size_t acoustics_ambience_zones() const { return acoustics_.ambience_zone_count(); }
  std::string car_burble_bank() const;

  // Initializes Your Music from `music_dir` (<state>/music).
  void init_user_music(const std::filesystem::path& music_dir, bool non_blocking);

  // Opens SDL_AudioStream and starts the dedicated audio render thread.
  void start_device();
  // Starts the dedicated audio render thread against a paced null sink for headless benchmarks.
  void start_benchmark_device();
  void stop_device();
  bool running() const { return engine_.running(); }
  void set_fmv_audio(audio::AudioEngine::FmvAudioFn fn, void* ctx) { engine_.set_fmv_audio(fn, ctx); }
  bool has_audio() const { return has_car_ || has_sfx_ || has_satnav_ || radio_ != nullptr || user_music_ready_; }

  // Per-frame lock-free updates (main thread).
  void update_frame(const audio::EnginePlayer::Update& u, bool paused, bool menu_open, bool on_title,
                    bool loading = false);
  void update_vehicle(const audio::VehicleAudioState& st, bool in_race, bool rewinding, bool countdown_active,
                      int countdown_digit);
  void trigger_collision(const audio::CollisionHit& hit);
  void trigger_ui(audio::UiCue cue);
  void trigger_satnav(audio::SatNavCue cue);
  void set_satnav_enabled(bool on);
  void trigger_vo(std::string_view trigger_id, bool interrupt = false);
  void skip_vo();
  void set_dialog_ducking(bool active);
  void trigger_codriver(std::string_view cue_path);
  void clear_codriver();
  void radio_set_time_of_day(double hour);
  void radio_trigger_special(std::string_view gameplay_event);
  void radio_trigger_immediate(std::string_view gameplay_event);
  void radio_trigger_festival_update(std::string_view gameplay_event);
  bool radio_select_station_by_name(std::string_view substr);
  void update_world_acoustics(float x, float y, float z, float heading_rad);
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
  int snap_dialog_duck_ = -1;
  bool last_dialog_duck_ = false;

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

  bool mixer_configs_tried_ = false;
  bool sfx_tried_ = false;
  bool has_sfx_ = false;
  std::filesystem::path sfx_game_root_;
  std::filesystem::path sfx_cache_dir_;
  bool satnav_tried_ = false;
  bool has_satnav_ = false;
  bool codriver_tried_ = false;
  bool has_codriver_ = false;
  bool vo_tried_ = false;
  bool has_vo_ = false;
  bool acoustics_tried_ = false;
  bool has_acoustics_ = false;
  std::string acoustics_world_key_;
  audio::WorldAcousticsData acoustics_;
  bool last_paused_ = false;
  bool last_menu_ = false;
  bool last_loading_ = false;
  bool last_in_race_ = false;
  bool last_rewinding_ = false;
  int last_countdown_digit_ = -1;
  audio::CarDspMode last_dsp_mode_ = audio::CarDspMode::kOff;
};

}  // namespace carview
