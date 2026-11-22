#include "audio_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <system_error>

#include "cars/mods.h"
#include "cars/sources.h"

namespace carview {
namespace {

float parse_pct(const std::string& s, float fallback, float lo, float hi) {
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return fallback;
  return std::clamp(float(v / 100.0), lo, hi);
}

}  // namespace

bool AudioConfig::parse_arg(const std::vector<std::string>& args, size_t& i, AudioConfig& out) {
  const std::string& a = args[i];
  auto has_next = [&]() { return i + 1 < args.size(); };
  if (a == "--no-sound") {
    out.sound = false;
    return true;
  }
  if (a == "--no-radio") {
    out.radio_on = false;
    return true;
  }
  if (a == "--no-radio-dj") {
    out.radio_dj = false;
    return true;
  }
  if (a == "--mute") {
    out.mute = true;
    return true;
  }
  if (a == "--full-dynamic-range") {
    out.full_dynamic_range = true;
    return true;
  }
  if (a == "--your-music") {
    out.your_music = true;
    return true;
  }
  if (a == "--your-music-shuffle") {
    out.your_music_shuffle = true;
    return true;
  }
  if (a == "--master-volume" && has_next()) {
    out.master_volume = parse_pct(args[++i], 1.0f, 0.0f, 1.0f);
    return true;
  }
  if (a == "--radio-volume" && has_next()) {
    out.radio_volume = parse_pct(args[++i], 1.0f, 0.0f, 1.0f);
    return true;
  }
  if (a == "--your-music-volume" && has_next()) {
    out.your_music_volume = parse_pct(args[++i], 1.0f, 0.0f, 2.0f);
    return true;
  }
  struct BusFlag {
    const char* flag;
    audio::Bus bus;
  };
  static constexpr BusFlag kBusFlags[] = {
      {"--mix-engine", audio::Bus::kEngine},      {"--mix-other-cars", audio::Bus::kOtherCars},
      {"--mix-tyres", audio::Bus::kTyres},        {"--mix-wind", audio::Bus::kWind},
      {"--mix-ambience", audio::Bus::kAmbience},  {"--mix-music", audio::Bus::kMusic},
      {"--mix-ui", audio::Bus::kUi},              {"--mix-vo", audio::Bus::kVo},
  };
  for (const auto& bf : kBusFlags) {
    if (a == bf.flag && has_next()) {
      out.bus_mix[size_t(bf.bus)] = parse_pct(args[++i], 1.0f, 0.0f, 2.0f);
      return true;
    }
  }
  return false;
}

AudioConfig AudioConfig::from_tokens(const std::vector<std::string>& tokens) {
  AudioConfig cfg;
  for (size_t i = 0; i < tokens.size(); ++i) parse_arg(tokens, i, cfg);
  return cfg;
}

AudioHost::AudioHost() {
  snap_paused_ = engine_.find_snapshot("Paused");
  snap_menu_ = engine_.find_snapshot("Menu");
  snap_title_ = engine_.find_snapshot("Title");
  snap_loading_ = engine_.find_snapshot("Loading");
}

AudioHost::~AudioHost() {
  stop_device();
  clear_car();
}

void AudioHost::apply_config(const AudioConfig& cfg, const std::filesystem::path& music_dir) {
  const bool enabling_music = cfg.your_music && (!cfg_.your_music || !user_music_ready_);
  cfg_ = cfg;
  if (!music_dir.empty()) music_dir_ = music_dir;
  engine_.set_master_volume(cfg_.master_volume);
  engine_.set_mute(cfg_.mute || !cfg_.sound);
  engine_.set_full_dynamic_range(cfg_.full_dynamic_range);
  for (int b = 0; b < audio::kBuses; ++b) engine_.set_bus_level(audio::Bus(b), cfg_.bus_mix[size_t(b)]);
  engine_.radio_set_dj(cfg_.radio_dj);
  engine_.radio_set_volume(cfg_.radio_on ? cfg_.radio_volume : 0.0f);
  if (enabling_music && !music_dir_.empty()) init_user_music(music_dir_, user_music_non_blocking_);
  engine_.user_music_enable(cfg_.your_music);
  engine_.user_music_set_shuffle(cfg_.your_music_shuffle);
  engine_.user_music_set_volume(cfg_.your_music_volume);
}

bool AudioHost::load_car(const std::filesystem::path& sound_dir, const std::filesystem::path& game_root,
                         const std::filesystem::path& state_root, const std::string& car, audio::UpgradeLevels levels,
                         double idle_rpm, double redline_rpm, std::string* note) {
  const bool was_running = engine_.running();
  if (was_running) stop_device();
  clear_car();

  auto baked = std::make_unique<audio::BakedSound>();
  std::string err;
  bool ok = !sound_dir.empty() && audio::load_baked_sound(sound_dir, car, *baked, &err, levels);
  if (!ok && !game_root.empty()) {
    if (!sound_files_tried_) {
      sound_files_tried_ = true;
      cars::Install install;
      std::string ie;
      if (cars::discover_install(game_root, state_root, {}, install, &ie)) {
        auto sf = std::make_unique<audio::SoundFiles>();
        if (sf->open(install, &ie)) sound_files_ = std::move(sf);
      }
    }
    if (sound_files_) {
      const std::filesystem::path cache = !sound_dir.empty()
                                              ? sound_dir
                                              : (!state_root.empty() ? state_root / "sound-cache" : std::filesystem::path{});
      ok = audio::decode_engine_sound_runtime(*sound_files_, car, cache, *baked, &err, levels);
    }
  }
  if (!ok && (levels.intake > 0 || levels.exhaust > 0) && !sound_dir.empty()) {
    // Fallback to stock baked loops if runtime decode failed (e.g. no ffmpeg).
    ok = audio::load_baked_sound(sound_dir, car, *baked, &err, {0, 0});
  }
  if (ok && !baked->loaded.sound.groups.empty()) {
    auto ep = std::make_unique<audio::EnginePlayer>(baked->loaded.sound, baked->table(), kAudioRate, idle_rpm,
                                                    redline_rpm);
    ep->master = kEngineMaster;
    baked_sound_ = std::move(baked);
    engine_.set_engine_player(std::move(ep));
    has_car_ = true;
    if (note) {
      *note = "engine sound: " + std::to_string(baked_sound_->loaded.sound.groups.size()) + " groups (L" +
              std::to_string(levels.intake) + "/L" + std::to_string(levels.exhaust) + ") from " +
              cars::mods::resolve(sound_dir / audio::baked_sound_key(car, levels)).string();
    }
  } else {
    if (note) *note = err.empty() ? "no loops" : err;
  }
  if (was_running) start_device();
  return has_car_;
}

void AudioHost::clear_car() {
  engine_.set_engine_player(nullptr);
  if (!engine_.running()) engine_.reclaim();
  baked_sound_.reset();
  has_car_ = false;
}

bool AudioHost::init_radio(const std::filesystem::path& game_root, std::string_view language,
                           std::string_view saved_station, bool non_blocking) {
  radio_non_blocking_ = non_blocking;
  if (!radio_tried_) {
    radio_tried_ = true;
    std::string why;
    if (audio::load_radio(game_root, radio_data_, &why, language)) {
      radio_ = std::make_unique<audio::Radio>(radio_data_, std::random_device{}(), cfg_.radio_dj);
      int start_station = radio_data_.cycle.empty() ? -1 : radio_data_.cycle[0];
      if (!saved_station.empty()) {
        const int found = radio_->find_station(saved_station);
        if (found >= 0) start_station = found;
      }
      if (start_station >= 0) radio_->select(start_station);
      std::printf("radio: %zu stations in the cycle, %zu tracks, %zu voice clips (%s)\n", radio_data_.cycle.size(),
                  radio_data_.music.size(), radio_data_.vo.size(), radio_data_.language.c_str());
    } else {
      std::fprintf(stderr, "carview: note: no radio: %s\n", why.c_str());
    }
  }
  if (radio_) {
    auto rp = std::make_unique<audio::RadioPlayer>(*radio_, kAudioRate, non_blocking);
    rp->master = kRadioMaster;
    rp->volume = cfg_.radio_on ? double(cfg_.radio_volume) : 0.0;
    engine_.set_radio(radio_.get(), std::move(rp));
    return true;
  }
  return false;
}

void AudioHost::init_user_music(const std::filesystem::path& music_dir, bool non_blocking) {
  music_dir_ = music_dir;
  user_music_non_blocking_ = non_blocking;
  if (music_dir_.empty()) return;
  std::error_code ec;
  if (cfg_.your_music) std::filesystem::create_directories(music_dir_, ec);
  auto um = std::make_unique<audio::UserMusicPlayer>(music_dir_, kAudioRate, std::random_device{}(), non_blocking);
  um->set_shuffle(cfg_.your_music_shuffle);
  um->set_volume(cfg_.your_music_volume);
  um->set_enabled(cfg_.your_music);
  const int count = um->track_count();
  if (count > 0 || cfg_.your_music)
    std::printf("your music: %d track(s) in %s (%s)\n", count, music_dir_.string().c_str(),
                cfg_.your_music ? "on" : "off");
  engine_.set_user_music(std::move(um));
  user_music_ready_ = count > 0;
}

void AudioHost::start_device() {
  if (stream_ || !cfg_.sound || !has_audio()) return;
  const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kAudioRate};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) {
    std::fprintf(stderr, "carview: no audio device: %s\n", SDL_GetError());
    return;
  }
  SDL_ResumeAudioStreamDevice(stream_);
  audio::AudioEngine::Sink sink;
  sink.target_buffer_frames = int(0.06 * kAudioRate);  // 60 ms ahead
  sink.block_frames = 480;                             // 10 ms render blocks
  SDL_AudioStream* s = stream_;
  sink.queued_frames = [s]() { return SDL_GetAudioStreamQueued(s) / int(2 * sizeof(float)); };
  sink.write = [s](const float* stereo, int frames) {
    SDL_PutAudioStreamData(s, stereo, frames * 2 * int(sizeof(float)));
  };
  engine_.start(std::move(sink));
}

void AudioHost::stop_device() {
  engine_.stop();
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
}

void AudioHost::update_frame(const audio::EnginePlayer::Update& u, bool paused, bool menu_open, bool on_title,
                             bool loading) {
  engine_.set_engine_input(u);
  engine_.set_snapshot(snap_paused_, paused);
  engine_.set_snapshot(snap_menu_, menu_open);
  engine_.set_snapshot(snap_title_, on_title);
  engine_.set_snapshot(snap_loading_, loading);
}

void AudioHost::radio_press(int dir) {
  if (!radio_) return;
  engine_.radio_press(dir);
}

bool AudioHost::handle_key(SDL_Keycode key, SDL_Keymod mod, std::string& notice) {
  if (key == SDLK_F9) {
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
    if (shift) engine_.user_music_prev();
    else engine_.user_music_next();
    const auto tel = engine_.telemetry();
    if (tel.user_music_active) {
      notice = std::string("your music: ") + (shift ? "previous track" : "next track");
    } else {
      notice = "your music: off (enable in settings, add mp3/wav to music folder)";
    }
    return true;
  }
  return false;
}

audio::AudioTelemetry AudioHost::telemetry() { return engine_.telemetry(); }

void AudioHost::render_offline(const audio::EnginePlayer::Update& u, float* out, int frames) {
  engine_.set_engine_input(u);
  engine_.render_offline(out, frames);
}

void AudioHost::save_radio_state(drive::DriveSave& save) {
  const auto tel = engine_.telemetry();
  if (radio_ && tel.radio_selected >= 0 && size_t(tel.radio_selected) < radio_data_.stations.size()) {
    save.radio_station = radio_data_.stations[size_t(tel.radio_selected)].name;
  } else if (radio_) {
    save.radio_station = radio_->selected_name();
  }
  save.radio_dj = cfg_.radio_dj;
  save.radio_volume = double(cfg_.radio_volume);
}

}  // namespace carview
