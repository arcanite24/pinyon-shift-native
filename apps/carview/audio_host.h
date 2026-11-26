// Carview audio host (ours, Slice 2): owns the dedicated AudioEngine render
// thread, SDL_AudioStream sink, runtime XMA2 sound loader, off-thread radio
// and Your Music player, and live-applying AudioConfig (Master Volume/Mute,
// Full Dynamic Range, Sound Mix 0-200%, Radio DJ/Volume, Your Music).
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/bake.h"
#include "audio/radio.h"
#include "audio/radio_dj.h"
#include "audio/runtime_radio.h"
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
  audio::PlaybackSystem playback_system = audio::PlaybackSystem::kHomeTheater;
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

  // Other cars play their own engine sound (SPEC-ENGINE-SOUND; the focus
  // car's loader and banks, heard at the Default view). Each frame, bind the
  // cars about to be heard by their OtherCarAudio::car_id and media name
  // (idle/redline: the car's physics rpm range, <= 0 = its tuning's); the
  // sound decodes on a worker and plays once ready; a car not bound for two
  // seconds is released. Without the disc's sound files nothing changes.
  void bind_other_car_sound(uint32_t car_id, std::string_view media, double idle_rpm = 0, double redline_rpm = 0);
  void prefetch_other_car_sound(std::string_view media);
  void update_other_car_sounds();
  int other_car_sounds_attached() const { return other_attached_; }
  // Blocks until every requested sound has decoded or failed (tests, tools).
  void wait_other_car_sounds();

  // Initializes the radio once (honouring cars::mods and `language`) and
  // attaches a RadioPlayer to AudioEngine.
  bool init_radio(const std::filesystem::path& game_root, std::string_view language, std::string_view saved_station,
                  bool non_blocking);
  bool reload_radio(std::string_view language, std::string_view saved_station = {});
  // Configures runtime custom radio stations (<user>/radio_stations.json and <play>/radio_stations.json).
  void init_runtime_radio(const std::filesystem::path& user_dir, const std::filesystem::path& play_dir = {});
  audio::RuntimeRadioManager& runtime_radio_manager() { return runtime_radio_mgr_; }
  const audio::RuntimeRadioManager& runtime_radio_manager() const { return runtime_radio_mgr_; }
  const audio::RadioData& radio_data() const { return radio_data_; }
  bool has_radio() const { return radio_ != nullptr || !custom_stations_.empty(); }

  // Loads AudioMixerSnapshots/GlobalDSP/AudioEngineConfig + FEV/FSB4 SfxEngine
  // (async background FSB decode with disk cache).
  bool init_sfx(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                const std::string& burble_bank = {}, const std::filesystem::path& state_root = {});
  bool init_satnav(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                   std::string_view language = "EN", bool metric = true);
  bool reload_satnav(std::string_view language, bool metric);
  bool init_codriver(const std::filesystem::path& rally_package_dir, const std::filesystem::path& cache_dir,
                     std::string_view language = "EN");
  void set_ui_music(audio::AudioEngine::UiMusicMode mode);
  // A load drawn outside the frame loop (the world host's): the Loading mix
  // (the radio ducked, no menu music) until it ends, as update_frame's
  // `loading` gives the session's.
  void set_blocking_load(bool on);
  uint8_t map_tire_surface_group(std::string_view physics_surface) const;
  int16_t map_tire_surface_index(std::string_view physics_surface) const;
  bool init_vo(const std::filesystem::path& game_root, std::string_view language = "EN");
  bool reload_vo(std::string_view language);
  bool init_acoustics(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir = {},
                      const std::filesystem::path& world_dir = {}, const std::filesystem::path& rally_package = {},
                      bool async_soundscapes = false);
  const std::string& acoustics_world_key() const { return acoustics_world_key_; }
  size_t acoustics_reverb_zones() const {
    std::lock_guard<std::mutex> lk(acoustics_mu_);
    return acoustics_.reverb_zone_count();
  }
  size_t acoustics_ambience_zones() const {
    std::lock_guard<std::mutex> lk(acoustics_mu_);
    return acoustics_.ambience_zone_count();
  }
  bool in_tunnel() const { return last_in_tunnel_; }
  std::string car_burble_bank() const;

  // Initializes Your Music from `music_dir` (<state>/music).
  void init_user_music(const std::filesystem::path& music_dir, bool non_blocking);

  // Opens SDL_AudioStream and starts the dedicated audio render thread.
  void start_device();
  // Starts the dedicated audio render thread against a paced null sink for headless benchmarks.
  void start_benchmark_device();
  void stop_device();
  bool running() const { return engine_.running(); }
  void set_fmv_audio(audio::AudioEngine::FmvAudioFn fn, void* ctx);
  bool has_audio() const {
    return has_car_ || has_sfx_ || has_satnav_ || has_vo_ || has_codriver_ ||
           has_acoustics_.load(std::memory_order_acquire) || has_fmv_audio_ || radio_ != nullptr ||
           !custom_stations_.empty() || user_music_ready_;
  }

  // Per-frame lock-free updates (main thread).
  void update_frame(const audio::EnginePlayer::Update& u, bool paused, bool menu_open, bool on_title,
                    bool loading = false);
  void update_vehicle(const audio::VehicleAudioState& st, bool in_race, bool rewinding, bool countdown_active,
                      int countdown_digit, float countdown_seq_elapsed_s = -1.0f);
  void set_homespace_mode(int mode);
  void set_speed_zone_active(bool active);
  void trigger_speed_cam();
  void set_window_minimized(bool minimized);
  void set_pre_event(bool active);
  // The ambience events' PreRaceAmbience and CrowdExcitement (0..1). Which
  // game state drives them is not decoded (docs/SPEC_QUESTIONS.md Q80), so
  // nothing calls these yet and both stay at their minimum.
  void set_pre_race_ambience(float v);
  void set_crowd_excitement(float v);
  void set_barnfind_active(bool active);
  void set_race_encounter_active(bool active);
  void trigger_collision(const audio::CollisionHit& hit);
  void trigger_landing(const audio::LandingHit& hit);
  void set_rally_world(bool on);
  void set_radio_rally_mode(bool on);
  // Plays a cue as the game does: through ui4audio.xml's play name for it,
  // with the current UI scene's overrides (falls back to the engine's own cue
  // sounds when the disc's ui4audio.xml / UIInGame.fev are missing).
  void trigger_ui(audio::UiCue cue);
  // ui4audio play names (audio/ui_audio.h). The scene the UI is on picks the
  // override block (PostRace, Trophy, Showroom); empty = the defaults.
  void set_ui_scene(std::string_view scene);
  const std::string& ui_scene() const { return ui_scene_; }
  // Fires a ui4audio play name with the current scene / with `scene`. False
  // when the name is unknown (reported once) or the entry is Blank.
  bool play_ui(std::string_view play);
  bool play_ui(std::string_view play, std::string_view scene);
  // A UI event by FEV path (a cutscene's CCutsceneAudioTrigger group/event).
  bool play_ui_event(std::string_view path);
  // The race type the countdown and race-complete cues are chosen by.
  void set_race_audio_kind(audio::UiRaceKind kind) { race_audio_kind_ = kind; }
  const audio::UiAudioCatalog* ui_catalog() const { return ui_catalog_.get(); }
  void trigger_satnav(audio::SatNavCue cue);
  void set_satnav_enabled(bool on);
  // A flow's or a cutscene's mixer snapshot by its AudioMixerSnapshots.xml
  // name (OpeningCutscene, FreeRoam, VOPlayingInstant...): it replaces the
  // active snapshot of its group. Without the matrix it does nothing.
  void activate_mix_snapshot(std::string_view name);
  void trigger_vo(std::string_view trigger_id, bool interrupt = false, int variant = -1);
  void skip_vo();
  void set_dialog_ducking(bool active);
  void trigger_codriver(std::string_view cue_path, bool supersede = false);
  void clear_codriver();
  void radio_set_time_of_day(double hour);
  void radio_trigger_special(std::string_view gameplay_event);
  void radio_trigger_immediate(std::string_view gameplay_event);
  void radio_trigger_festival_update(std::string_view gameplay_event);
  // The DJ lines the game's flow data asks for (audio/radio_dj.h):
  // radiodj.xml's wristband behaviours and the activities' RadioDJRequests.
  void radio_cue(const audio::RadioDjCue& cue);
  void radio_wristband_awarded(int level);  // Race Central awarded wristband `level` (0 based)
  void radio_wristband_ready(int level);    // wristband `level` became ready to collect
  void radio_festival_event_finished(int event_level, int position);
  // A special line `delay_s` seconds from now (wall clock), dropped by
  // radio_cancel_scheduled; the radio's queue takes it when due.
  void radio_schedule_special(std::string_view gameplay_event, double delay_s);
  void radio_cancel_scheduled(std::string_view gameplay_event);
  const audio::RadioDjRules& radio_dj_rules() const { return radio_dj_rules_; }
  void radio_lock_viper_intro(bool lock);
  // The first-time career flow's radio states: the playlist (by RadioSystem.xml
  // fmodname), station switching, the DJ (on only when the DJ option is on too),
  // silent idents and cutting the DJ line playing.
  bool radio_set_playlist(const std::vector<std::string>& fmodnames, bool then_normal);
  void radio_set_switching(bool on);
  void radio_set_flow_dj(bool on);
  void radio_set_silent_idents(bool on);
  void radio_stop_dj();
  void radio_hold_popup(bool held);  // while the career's cutscenes hide the HUD
  void radio_cancel_playlist();
  // Out of the first-time career: the DJ per its option, switching on, idents
  // audible, no playlist.
  bool radio_flow_overridden() const { return !switching_enabled_ || flow_dj_off_ || flow_silent_idents_; }
  void radio_reset_flow();
  bool radio_select_station_by_name(std::string_view substr);
  void update_world_acoustics(float x, float y, float z, float heading_rad);
  void radio_press(int dir);
  bool handle_key(SDL_Keycode key, SDL_Keymod mod, std::string& notice);
  audio::AudioTelemetry telemetry();

  // Synchronous offline rendering for headless --audio-out.
  void render_offline(const audio::EnginePlayer::Update& u, float* out, int frames);
  // True while the SFX banks are still decoding in the background.
  bool sfx_decoding() const { return engine_.sfx_decoding(); }

  // Writes radio station/DJ/volume into `save`.
  void save_radio_state(drive::DriveSave& save);

 private:
  enum class GameplayOverlay : uint8_t {
    kNone = 0,
    kMenu,
    kInterior,
    kPreEvent,
    kBarnfind,
    kRaceEncounter,
    kVideo,
    kPaused,
    kRewind,
    kLoading,
  };
  void ensure_device_started();
  void sync_gameplay_snapshot();

  AudioConfig cfg_;
  audio::AudioEngine engine_{kAudioRate};
  SDL_AudioStream* stream_ = nullptr;
  bool has_fmv_audio_ = false;
  bool device_open_failed_ = false;
  std::chrono::steady_clock::time_point last_device_try_tp_{};
  GameplayOverlay last_overlay_ = GameplayOverlay::kNone;
  bool applied_in_race_ = false;

  int snap_paused_ = -1;
  int snap_menu_ = -1;
  int snap_title_ = -1;
  int snap_loading_ = -1;
  bool last_dialog_duck_ = false;

  std::unique_ptr<audio::SoundFiles> sound_files_;
  bool sound_files_tried_ = false;
  std::shared_ptr<audio::BakedSound> baked_sound_;
  bool has_car_ = false;

  audio::RadioData stock_radio_data_;  // immutable after init_radio (referenced by audio-thread Radio)
  audio::RadioData radio_data_;        // main-thread view: stock stations + enabled custom stations
  std::unique_ptr<audio::Radio> radio_;
  bool radio_tried_ = false;
  bool radio_non_blocking_ = true;
  std::filesystem::path radio_game_root_;
  std::string radio_language_ = "EN";
  audio::RadioDjRules radio_dj_rules_;
  audio::RadioDjWristbands radio_dj_wristbands_{&radio_dj_rules_};
  struct ScheduledRadioLine {
    std::string event;
    std::chrono::steady_clock::time_point due;
  };
  std::vector<ScheduledRadioLine> radio_scheduled_;
  std::vector<std::string> radio_missed_;  // names already reported missing
  // Reports (once per name) a trigger RadioSystem.xml has no line for.
  void note_radio_miss(audio::RadioChannel channel, std::string_view gameplay_event);

  // Runtime custom radio station manager & unified station cycle.
  void ensure_runtime_radio_configured();
  void sync_runtime_radio(bool preserve_selection);
  void select_unified_station(int unified_pos, bool show_popup);
  double compute_custom_popup_alpha() const;

  audio::RuntimeRadioManager runtime_radio_mgr_;
  bool runtime_radio_configured_ = false;
  std::vector<audio::RuntimeStation> custom_stations_;
  std::vector<int> unified_cycle_;  // indices into radio_data_.stations
  int unified_pos_ = 0;
  int stock_off_station_ = -1;
  int active_custom_idx_ = -1;      // index into custom_stations_, or -1 when stock/off
  int active_custom_station_ = -1;  // index into radio_data_.stations
  uint32_t last_custom_track_seq_ = 0;
  std::chrono::steady_clock::time_point last_radio_poll_{};
  std::chrono::steady_clock::time_point custom_popup_t0_{};
  bool custom_popup_active_ = false;
  int custom_popup_station_ = -1;

  std::filesystem::path music_dir_;
  bool user_music_ready_ = false;
  bool user_music_non_blocking_ = true;

  bool mixer_configs_tried_ = false;
  // Other cars' engine sounds: decoded by media on one worker with its own
  // sound files, bound by car id on the main thread.
  struct OtherSound {
    std::shared_ptr<audio::BakedSound> baked;
    bool done = false, ok = false;
    uint64_t last_use = 0;
  };
  struct OtherBinding {
    std::string media;  // lower case
    double idle = 0, redline = 0;
    uint64_t last_frame = 0;
    bool attached = false;
  };
  void start_other_worker();
  void stop_other_worker();
  std::filesystem::path other_sound_dir_, other_game_root_, other_state_root_;
  std::mutex other_mu_;
  std::condition_variable other_cv_;
  std::vector<std::string> other_jobs_;           // guarded by other_mu_
  std::map<std::string, OtherSound> other_sounds_;  // lower-case media; guarded by other_mu_
  std::thread other_worker_;
  bool other_stop_ = false;  // guarded by other_mu_
  int other_pending_ = 0;    // guarded by other_mu_
  std::map<uint32_t, OtherBinding> other_bindings_;
  uint64_t other_frame_ = 0;
  int other_attached_ = 0;
  bool other_owned_ = false;

  bool sfx_tried_ = false;
  bool has_sfx_ = false;
  std::shared_ptr<const audio::UiAudioCatalog> ui_catalog_;  // from the SFX engine, immutable
  std::string ui_scene_;
  audio::UiRaceKind race_audio_kind_ = audio::UiRaceKind::kFestival;
  std::vector<std::string> ui_unknown_;  // play names already reported unknown
  bool wristband_sting_pending_ = false;  // kWristbandUnlock waits for the level
  std::filesystem::path sfx_game_root_;
  std::filesystem::path sfx_cache_dir_;
  bool satnav_tried_ = false;
  bool has_satnav_ = false;
  std::filesystem::path satnav_game_root_;
  std::filesystem::path satnav_cache_dir_;
  std::string satnav_language_ = "EN";
  bool satnav_metric_ = true;
  audio::TiresConfig tires_cfg_;
  audio::AudioEngine::UiMusicMode last_ui_music_ = audio::AudioEngine::UiMusicMode::kNone;
  bool codriver_tried_ = false;
  bool has_codriver_ = false;
  bool vo_tried_ = false;
  bool has_vo_ = false;
  std::filesystem::path vo_game_root_;
  std::string vo_language_ = "EN";
  bool acoustics_tried_ = false;
  std::atomic<bool> has_acoustics_{false};
  bool rally_sfx_tried_ = false;
  bool rally_acoustics_tried_ = false;
  bool rally_world_ = false;
  bool last_radio_rally_mode_ = false;
  std::string acoustics_world_key_;
  mutable std::mutex acoustics_mu_;
  std::thread acoustics_loader_;
  uint64_t acoustics_gen_ = 0;
  audio::WorldAcousticsData acoustics_;
  bool last_paused_ = false;
  bool last_menu_ = false;
  bool last_loading_ = false;
  bool last_on_title_ = false;
  bool have_on_title_ = false;
  bool last_in_race_ = false;
  bool last_rewinding_ = false;
  bool last_countdown_active_ = false;
  int last_countdown_digit_ = -1;
  std::chrono::steady_clock::time_point countdown_t0_{};
  audio::CarDspMode last_dsp_mode_ = audio::CarDspMode::kOff;
  audio::View last_audio_view_ = audio::View::kFollow;
  int homespace_mode_ = 0;
  int last_hub_ambience_ = 0;
  bool speed_zone_active_ = false;
  bool window_minimized_ = false;
  bool pre_event_active_ = false;
  float amb_pre_race_ = 0.0f, amb_crowd_ = 0.0f;
  float amb_sent_[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  void sync_ambience_state(bool in_race);
  bool barnfind_active_ = false;
  bool race_encounter_active_ = false;
  bool viper_intro_locked_ = false;
  bool switching_enabled_ = true;
  bool flow_dj_off_ = false;
  bool flow_silent_idents_ = false;
  std::vector<std::string> flow_playlist_;  // set before the radio was built
  bool flow_playlist_then_normal_ = true;

  bool have_acoustics_sent_ = false;
  bool last_in_tunnel_ = false;
  std::array<float, 3> last_acoustics_pos_{0.0f, 0.0f, 0.0f};
  float last_acoustics_heading_ = 0.0f;
  std::chrono::steady_clock::time_point last_acoustics_tp_{};
  bool have_tod_sent_ = false;
  double last_tod_hour_ = -100.0;
};

}  // namespace carview
