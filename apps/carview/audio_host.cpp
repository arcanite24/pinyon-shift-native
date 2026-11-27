#include "audio_host.h"

#include <algorithm>
#include <cmath>
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

// The events the map's placements play (the bed player decodes only these).
std::vector<uint32_t> placed_ambience_events(const audio::WorldAcousticsData& d) {
  std::vector<uint32_t> out;
  out.reserve(64);
  for (const auto& em : d.emitters())
    if (em.playable) out.push_back(em.event_hash != 0u ? em.event_hash : audio::ambience_event_hash(em.event_name));
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// CHudRadioCycleLogos (first_time_career Radio_Unlock): cycles the HUD radio
// popup strip across the unlocked FM stations so the player sees them.
struct RadioLogoCycleState {
  bool active = false;
  std::chrono::steady_clock::time_point t0{};
  std::vector<int> stations;
};

std::mutex& logo_cycle_mutex() {
  static std::mutex mu;
  return mu;
}

std::map<const AudioHost*, RadioLogoCycleState>& logo_cycles() {
  static std::map<const AudioHost*, RadioLogoCycleState> map;
  return map;
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
  if (a == "--no-satnav") {
    out.satnav_on = false;
    return true;
  }
  if (a == "--satnav") {
    out.satnav_on = true;
    return true;
  }
  if (a == "--mute") {
    out.mute = true;
    return true;
  }
  if (a == "--full-dynamic-range") {
    out.full_dynamic_range = true;
    out.playback_system = audio::PlaybackSystem::kFullDynamicRange;
    return true;
  }
  if (a == "--tv-speakers") {
    out.playback_system = audio::PlaybackSystem::kTv;
    return true;
  }
  if (a == "--playback-system" && has_next()) {
    std::string v = args[++i];
    for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (v == "tv" || v == "television") out.playback_system = audio::PlaybackSystem::kTv;
    else if (v == "headphones" || v == "headphone") out.playback_system = audio::PlaybackSystem::kHeadphones;
    else if (v == "car" || v == "caraudio" || v == "car_audio") out.playback_system = audio::PlaybackSystem::kCarAudio;
    else if (v == "full" || v == "fdr" || v == "fulldynamicrange") {
      out.playback_system = audio::PlaybackSystem::kFullDynamicRange;
      out.full_dynamic_range = true;
    } else {
      out.playback_system = audio::PlaybackSystem::kHomeTheater;
    }
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
  {
    std::lock_guard<std::mutex> lk(logo_cycle_mutex());
    logo_cycles().erase(this);
  }
  if (acoustics_loader_.joinable()) acoustics_loader_.join();
  stop_other_worker();
  stop_device();
  clear_car();
}

void AudioHost::start_other_worker() {
  if (other_worker_.joinable()) return;
  const auto sound_dir = other_sound_dir_, game_root = other_game_root_, state_root = other_state_root_;
  other_worker_ = std::thread([this, sound_dir, game_root, state_root]() {
    std::unique_ptr<audio::SoundFiles> files;  // the worker's own: archives are not shared across threads
    if (!game_root.empty()) {
      cars::Install install;
      std::string ie;
      if (cars::discover_install(game_root, state_root, {}, install, &ie)) {
        auto sf = std::make_unique<audio::SoundFiles>();
        if (sf->open(install, &ie)) files = std::move(sf);
      }
    }
    const std::filesystem::path cache =
        !sound_dir.empty() ? sound_dir : (!state_root.empty() ? state_root / "sound-cache" : std::filesystem::path{});
    for (;;) {
      std::string media;
      {
        std::unique_lock<std::mutex> lk(other_mu_);
        other_cv_.wait(lk, [&] { return other_stop_ || !other_jobs_.empty(); });
        if (other_stop_) return;
        media = other_jobs_.front();
        other_jobs_.erase(other_jobs_.begin());
      }
      auto baked = std::make_shared<audio::BakedSound>();
      std::string err;
      bool ok = !sound_dir.empty() && audio::load_baked_sound(sound_dir, media, *baked, &err);
      if (!ok && files) ok = audio::decode_engine_sound_runtime(*files, media, cache, *baked, &err);
      if (!ok) std::fprintf(stderr, "carview: note: no engine sound for %s: %s\n", media.c_str(), err.c_str());
      std::lock_guard<std::mutex> lk(other_mu_);
      auto& s = other_sounds_[cars::lower(media)];
      s.done = true;
      s.ok = ok;
      if (ok) s.baked = std::move(baked);
      --other_pending_;
      other_cv_.notify_all();
    }
  });
}

void AudioHost::stop_other_worker() {
  {
    std::lock_guard<std::mutex> lk(other_mu_);
    other_stop_ = true;
  }
  other_cv_.notify_all();
  if (other_worker_.joinable()) other_worker_.join();
}

void AudioHost::prefetch_other_car_sound(std::string_view media) {
  if (media.empty() || (other_game_root_.empty() && other_sound_dir_.empty())) return;
  const std::string key = cars::lower(std::string(media));
  {
    std::lock_guard<std::mutex> lk(other_mu_);
    auto [it, fresh] = other_sounds_.try_emplace(key);
    it->second.last_use = other_frame_;
    if (!fresh) return;
    other_jobs_.push_back(std::string(media));
    ++other_pending_;
  }
  start_other_worker();
  other_cv_.notify_all();
}

void AudioHost::wait_other_car_sounds() {
  std::unique_lock<std::mutex> lk(other_mu_);
  other_cv_.wait(lk, [&] { return other_pending_ <= 0 || other_stop_; });
}

void AudioHost::bind_other_car_sound(uint32_t car_id, std::string_view media, double idle_rpm, double redline_rpm) {
  if (car_id == 0u || media.empty()) return;
  if (other_game_root_.empty() && other_sound_dir_.empty()) return;
  auto& b = other_bindings_[car_id];
  const std::string key = cars::lower(std::string(media));
  if (b.media != key) {
    if (b.attached) engine_.set_other_car_engine(car_id, nullptr);
    b = {};
    b.media = key;
    prefetch_other_car_sound(media);
  }
  b.idle = idle_rpm;
  b.redline = redline_rpm;
  b.last_frame = other_frame_;
}

void AudioHost::update_other_car_sounds() {
  ++other_frame_;
  if (other_game_root_.empty() && other_sound_dir_.empty()) return;
  if (!other_owned_ && has_sfx_) {
    // From here a car plays its own sound or, while that loads, nothing.
    engine_.set_other_car_engines_owned(true);
    other_owned_ = true;
  }
  constexpr uint64_t kReleaseFrames = 120;  // about two seconds unbound
  int attached = 0;
  for (auto it = other_bindings_.begin(); it != other_bindings_.end();) {
    auto& [id, b] = *it;
    if (other_frame_ - b.last_frame > kReleaseFrames) {
      if (b.attached) engine_.set_other_car_engine(id, nullptr);
      it = other_bindings_.erase(it);
      continue;
    }
    if (!b.attached) {
      std::shared_ptr<audio::BakedSound> baked;
      {
        std::lock_guard<std::mutex> lk(other_mu_);
        const auto s = other_sounds_.find(b.media);
        if (s != other_sounds_.end() && s->second.done && s->second.ok) baked = s->second.baked;
        if (s != other_sounds_.end()) s->second.last_use = other_frame_;
      }
      if (baked && !baked->loaded.sound.groups.empty()) {
        const auto& snd = baked->loaded.sound;
        const double idle = b.idle > 0 ? b.idle : snd.audio_rpm_idle;
        const double redline = b.redline > idle ? b.redline : std::max(snd.audio_rpm_redline, idle + 1000.0);
        auto ep = std::make_unique<audio::EnginePlayer>(snd, baked->table(), kAudioRate, idle, redline, nullptr);
        ep->master = kEngineMaster;
    if (focus_descriptor_scalar_ >= 0) ep->descriptor_scalar = focus_descriptor_scalar_;
        ep->positional = true;  // a 3D point source; SfxEngine places it
        ep->attach_owner(baked);
        engine_.set_other_car_engine(id, std::move(ep));
        b.attached = true;
      }
    } else {
      std::lock_guard<std::mutex> lk(other_mu_);
      if (const auto s = other_sounds_.find(b.media); s != other_sounds_.end()) s->second.last_use = other_frame_;
    }
    attached += b.attached ? 1 : 0;
    ++it;
  }
  other_attached_ = attached;
  // Keep the decoded sounds of the last minute of cars; the attached players
  // pin their own.
  std::lock_guard<std::mutex> lk(other_mu_);
  constexpr uint64_t kKeepFrames = 3600;
  for (auto s = other_sounds_.begin(); s != other_sounds_.end();) {
    if (s->second.done && other_frame_ - s->second.last_use > kKeepFrames) s = other_sounds_.erase(s);
    else ++s;
  }
}

void AudioHost::apply_config(const AudioConfig& cfg, const std::filesystem::path& music_dir) {
  const bool enabling_music = cfg.your_music && (!cfg_.your_music || !user_music_ready_);
  cfg_ = cfg;
  if (!music_dir.empty()) music_dir_ = music_dir;
  engine_.set_master_volume(cfg_.master_volume);
  engine_.set_mute(cfg_.mute || !cfg_.sound || window_minimized_);
  engine_.set_playback_system(cfg_.full_dynamic_range ? audio::PlaybackSystem::kFullDynamicRange : cfg_.playback_system);
  engine_.set_full_dynamic_range(cfg_.full_dynamic_range);
  // Propagate the 8 top-level UI mix sliders so Mixer applies each slider cleanly to its sub-buses.
  for (int b = 0; b < audio::kUiSliderBuses; ++b) engine_.set_bus_level(audio::Bus(b), cfg_.bus_mix[size_t(b)]);
  engine_.radio_set_dj(cfg_.radio_dj && !flow_dj_off_);
  engine_.radio_set_volume(cfg_.radio_on ? cfg_.radio_volume : 0.0f);
  engine_.runtime_radio_set_volume(cfg_.radio_on ? cfg_.radio_volume : 0.0f);
  engine_.set_satnav_enabled(cfg_.satnav_on);
  if (!music_dir_.empty() && !runtime_radio_configured_) {
    init_runtime_radio(music_dir_.parent_path());
  }
  if (enabling_music && !music_dir_.empty()) init_user_music(music_dir_, user_music_non_blocking_);
  engine_.user_music_enable(cfg_.your_music);
  engine_.user_music_set_shuffle(cfg_.your_music_shuffle);
  engine_.user_music_set_volume(cfg_.your_music_volume);
}

bool AudioHost::load_car(const std::filesystem::path& sound_dir, const std::filesystem::path& game_root,
                         const std::filesystem::path& state_root, const std::string& car, audio::UpgradeLevels levels,
                         double idle_rpm, double redline_rpm, std::string* note) {
  // Decode and prepare the new car completely off the audio thread before swapping live,
  // avoiding stop_device()/start_device() dropouts and keeping radio/ambience continuous.
  if (!other_worker_.joinable()) {
    // The other cars' sounds come from the same files.
    other_sound_dir_ = sound_dir;
    other_game_root_ = game_root;
    other_state_root_ = state_root;
  }
  auto baked = std::make_shared<audio::BakedSound>();
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
                                                    redline_rpm, baked->intake_chuff());
    ep->master = kEngineMaster;
    if (focus_descriptor_scalar_ >= 0) ep->descriptor_scalar = focus_descriptor_scalar_;
    // Pin `baked` to the EnginePlayer so any previous car's BakedSound stays alive until
    // the audio thread retires and the main thread reclaims the old EnginePlayer.
    ep->attach_owner(baked);
    baked_sound_ = baked;
    engine_.set_engine_player(std::move(ep));
    has_car_ = true;
    if (has_sfx_ && !sfx_game_root_.empty()) {
      const auto& burble = baked_sound_->loaded.sound.burble;
      if (!burble.wavebank.empty()) {
        engine_.reload_sfx_burble(sfx_game_root_, sfx_cache_dir_, burble.wavebank, burble.burbles, burble.backfires);
      }
    }
    if (note) {
      *note = "engine sound: " + std::to_string(baked_sound_->loaded.sound.groups.size()) + " groups (L" +
              std::to_string(levels.intake) + "/L" + std::to_string(levels.exhaust) + ") from " +
              cars::mods::resolve(sound_dir / audio::baked_sound_key(car, levels)).string();
    }
  } else {
    clear_car();
    if (note) *note = err.empty() ? "no loops" : err;
  }
  return has_car_;
}

void AudioHost::clear_car() {
  engine_.set_engine_player(nullptr);
  if (!engine_.running()) engine_.reclaim();
  baked_sound_.reset();
  has_car_ = false;
}

void AudioHost::init_runtime_radio(const std::filesystem::path& user_dir, const std::filesystem::path& play_dir) {
  std::filesystem::path resolved_user = user_dir;
  if (resolved_user.empty()) {
    if (!music_dir_.empty()) resolved_user = music_dir_.parent_path();
    else if (const char* sr = std::getenv("SHIFT_STATE_ROOT"); sr && *sr) resolved_user = sr;
  }
  std::filesystem::path resolved_play = play_dir;
  if (resolved_play.empty()) {
    if (const char* pd = std::getenv("SHIFT_PLAY_DIR"); pd && *pd) {
      resolved_play = pd;
    } else {
      std::error_code ec;
      resolved_play = std::filesystem::current_path(ec);
    }
  }
  const std::filesystem::path user_cfg =
      resolved_user.empty() ? std::filesystem::path{} : (resolved_user / "radio_stations.json");
  const std::filesystem::path play_cfg =
      resolved_play.empty() ? std::filesystem::path{} : (resolved_play / "radio_stations.json");
  const std::filesystem::path cache_dir =
      resolved_user.empty() ? std::filesystem::path{} : (resolved_user / "radio_cache");

  runtime_radio_mgr_.configure(user_cfg, play_cfg, cache_dir);
  runtime_radio_mgr_.reload();
  runtime_radio_configured_ = true;
  last_radio_poll_ = std::chrono::steady_clock::now();
  sync_runtime_radio(true);
}

void AudioHost::ensure_runtime_radio_configured() {
  if (!runtime_radio_configured_) init_runtime_radio({});
}

void AudioHost::sync_runtime_radio(bool preserve_selection) {
  std::string prev_custom_id;
  int prev_station_idx = -1;
  if (preserve_selection) {
    if (active_custom_idx_ >= 0 && size_t(active_custom_idx_) < custom_stations_.size()) {
      prev_custom_id = custom_stations_[size_t(active_custom_idx_)].config.id;
    } else if (!unified_cycle_.empty() && unified_pos_ >= 0 && size_t(unified_pos_) < unified_cycle_.size()) {
      prev_station_idx = unified_cycle_[size_t(unified_pos_)];
    }
  }

  custom_stations_ = runtime_radio_mgr_.enabled_stations();

  // Rebuild main-thread radio_data_ view: stock stations + enabled custom stations.
  radio_data_ = stock_radio_data_;
  stock_off_station_ = -1;
  std::vector<int> stock_music_cycle;
  for (int idx : stock_radio_data_.cycle) {
    if (idx >= 0 && size_t(idx) < stock_radio_data_.stations.size()) {
      if (stock_radio_data_.stations[size_t(idx)].off) {
        stock_off_station_ = idx;
      } else {
        stock_music_cycle.push_back(idx);
      }
    }
  }

  const int base_count = int(radio_data_.stations.size());
  unified_cycle_ = stock_music_cycle;
  for (size_t i = 0; i < custom_stations_.size(); ++i) {
    const auto& cs = custom_stations_[i];
    audio::RadioStation st;
    st.name = cs.config.name;
    st.off = false;
    st.is_3d = false;
    st.volume = double(cs.config.volume);
    audio::RadioTrack placeholder;
    if (!cs.tracks.empty()) {
      placeholder.title = cs.tracks[0].title;
      placeholder.artist = cs.tracks[0].artist;
    } else if (cs.youtube_pending) {
      placeholder.title = "Loading YouTube playlist...";
      placeholder.artist = cs.config.genre;
    } else {
      placeholder.title = cs.config.genre.empty() ? "No tracks" : cs.config.genre;
      placeholder.artist = cs.config.name;
    }
    st.tracks.push_back(std::move(placeholder));
    const int st_idx = base_count + int(i);
    radio_data_.stations.push_back(std::move(st));
    unified_cycle_.push_back(st_idx);
  }
  if (stock_off_station_ >= 0) {
    unified_cycle_.push_back(stock_off_station_);
  } else if (!custom_stations_.empty()) {
    audio::RadioStation off_st;
    off_st.name = "Radio4_Silent";
    off_st.off = true;
    stock_off_station_ = int(radio_data_.stations.size());
    radio_data_.stations.push_back(std::move(off_st));
    unified_cycle_.push_back(stock_off_station_);
  }
  radio_data_.cycle = unified_cycle_;

  // Build and install RuntimeRadioPlayer on AudioEngine.
  if (!custom_stations_.empty()) {
    auto player = std::make_unique<audio::RuntimeRadioPlayer>(kAudioRate, std::random_device{}(), radio_non_blocking_);
    player->set_volume(cfg_.radio_on ? cfg_.radio_volume : 0.0f);
    player->set_stations(custom_stations_, runtime_radio_mgr_.cache_dir());
    engine_.set_runtime_radio(std::move(player));
  } else {
    engine_.set_runtime_radio(nullptr);
  }

  // Restore selection cleanly.
  if (!prev_custom_id.empty()) {
    int found_custom = -1;
    for (size_t i = 0; i < custom_stations_.size(); ++i) {
      if (custom_stations_[i].config.id == prev_custom_id) {
        found_custom = int(i);
        break;
      }
    }
    if (found_custom >= 0) {
      const int target_st = base_count + found_custom;
      for (size_t p = 0; p < unified_cycle_.size(); ++p) {
        if (unified_cycle_[p] == target_st) {
          select_unified_station(int(p), false);
          return;
        }
      }
    }
    // Custom station was removed or disabled: fall back to first station or off.
    active_custom_idx_ = -1;
    active_custom_station_ = -1;
    if (!unified_cycle_.empty()) select_unified_station(0, false);
  } else if (prev_station_idx >= 0) {
    for (size_t p = 0; p < unified_cycle_.size(); ++p) {
      if (unified_cycle_[p] == prev_station_idx) {
        unified_pos_ = int(p);
        break;
      }
    }
  } else if (unified_pos_ < 0 || size_t(unified_pos_) >= unified_cycle_.size()) {
    unified_pos_ = 0;
  }
}

void AudioHost::select_unified_station(int unified_pos, bool show_popup) {
  if (unified_cycle_.empty()) return;
  const int n = int(unified_cycle_.size());
  unified_pos_ = ((unified_pos % n) + n) % n;
  const int st_idx = unified_cycle_[size_t(unified_pos_)];
  const int stock_count = int(stock_radio_data_.stations.size());

  if (st_idx >= stock_count && st_idx < stock_count + int(custom_stations_.size())) {
    const int custom_idx = st_idx - stock_count;
    active_custom_idx_ = custom_idx;
    active_custom_station_ = st_idx;
    if (radio_ && stock_off_station_ >= 0 && stock_off_station_ < stock_count) {
      engine_.radio_select(stock_off_station_, show_popup);
    }
    engine_.runtime_radio_select(custom_idx);
    engine_.runtime_radio_set_volume(cfg_.radio_on ? cfg_.radio_volume : 0.0f);
    if (show_popup) {
      custom_popup_t0_ = std::chrono::steady_clock::now();
      custom_popup_active_ = true;
      custom_popup_station_ = st_idx;
    }
  } else {
    active_custom_idx_ = -1;
    active_custom_station_ = -1;
    engine_.runtime_radio_select(-1);
    if (radio_ && st_idx >= 0 && st_idx < stock_count) {
      engine_.radio_select(st_idx, show_popup);
    }
    if (show_popup) {
      custom_popup_t0_ = std::chrono::steady_clock::now();
      custom_popup_active_ = true;
      custom_popup_station_ = st_idx;
    } else {
      custom_popup_active_ = false;
    }
  }
}

double AudioHost::compute_custom_popup_alpha() const {
  if (!custom_popup_active_) return 0.0;
  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - custom_popup_t0_).count();
  if (dt < 0.0 || dt >= audio::Radio::kPopupShow) return 0.0;
  if (dt < audio::Radio::kPopupFade) return dt / audio::Radio::kPopupFade;
  if (dt > audio::Radio::kPopupShow - audio::Radio::kPopupFade) {
    return (audio::Radio::kPopupShow - dt) / audio::Radio::kPopupFade;
  }
  return 1.0;
}

bool AudioHost::init_radio(const std::filesystem::path& game_root, std::string_view language,
                           std::string_view saved_station, bool non_blocking) {
  radio_non_blocking_ = non_blocking;
  if (!game_root.empty()) radio_game_root_ = game_root;
  ensure_runtime_radio_configured();
  if (!radio_tried_) {
    radio_tried_ = true;
    radio_language_ = std::string(language.empty() ? "EN" : language);
    std::string why;
    if (audio::load_radio(game_root, stock_radio_data_, &why, radio_language_)) {
      radio_ = std::make_unique<audio::Radio>(stock_radio_data_, std::random_device{}(), cfg_.radio_dj);
      int start_station = stock_radio_data_.cycle.empty() ? -1 : stock_radio_data_.cycle[0];
      if (!saved_station.empty()) {
        const int found = radio_->find_station(saved_station);
        if (found >= 0) start_station = found;
      }
      if (start_station >= 0) radio_->select(start_station);
      // A radio built after the first-time career set its states takes them
      // (the engine does not own it yet).
      radio_->set_dj_enabled(cfg_.radio_dj && !flow_dj_off_);
      radio_->set_switching_enabled(switching_enabled_);
      radio_->set_silent_idents(flow_silent_idents_);
      if (!flow_playlist_.empty()) radio_->set_playlist(flow_playlist_, flow_playlist_then_normal_);
      std::printf("radio: %zu stations in the cycle, %zu tracks, %zu voice clips (%s)\n",
                  stock_radio_data_.cycle.size(), stock_radio_data_.music.size(), stock_radio_data_.vo.size(),
                  stock_radio_data_.language.c_str());
    } else {
      std::fprintf(stderr, "carview: note: no radio: %s\n", why.c_str());
    }
    std::string dj_why;
    if (!audio::load_radio_dj_rules(game_root, radio_dj_rules_, &dj_why))
      std::fprintf(stderr, "carview: note: no radio DJ wristband lines: %s\n", dj_why.c_str());
  }
  sync_runtime_radio(false);
  if (!custom_stations_.empty()) {
    std::printf("runtime radio: %zu custom station(s) appended to cycle\n", custom_stations_.size());
  }
  if (radio_) {
    auto rp = std::make_unique<audio::RadioPlayer>(*radio_, kAudioRate, non_blocking);
    rp->master = kRadioMaster;
    rp->volume = cfg_.radio_on ? double(cfg_.radio_volume) : 0.0;
    // Register canonical Colorado 3D location stations (Festival stages, Autoshow,
    // Workshop, Paintshop, Street Race Hubs, and Outposts).
    auto locs = audio::default_colorado_location_emitters(*radio_);
    if (!locs.empty()) rp->set_location_emitters(std::move(locs));
    engine_.set_radio(radio_.get(), std::move(rp));
  }
  if (!saved_station.empty()) {
    if (!radio_select_station_by_name(saved_station) && radio_) {
      const int sel = radio_->selected();
      for (size_t p = 0; p < unified_cycle_.size(); ++p) {
        if (unified_cycle_[p] == sel) {
          unified_pos_ = int(p);
          break;
        }
      }
    }
  } else if (radio_) {
    const int sel = radio_->selected();
    for (size_t p = 0; p < unified_cycle_.size(); ++p) {
      if (unified_cycle_[p] == sel) {
        unified_pos_ = int(p);
        break;
      }
    }
  } else if (!custom_stations_.empty()) {
    select_unified_station(0, false);
  }
  return has_radio();
}

bool AudioHost::reload_radio(std::string_view language, std::string_view saved_station) {
  if (radio_game_root_.empty()) return false;
  const std::string lang = std::string(language.empty() ? "EN" : language);
  if (radio_ && lang == radio_language_) return true;
  std::string current_station(saved_station);
  if (current_station.empty() && radio_) {
    const int sel = radio_->selected();
    if (sel >= 0 && size_t(sel) < stock_radio_data_.stations.size()) {
      current_station = stock_radio_data_.stations[size_t(sel)].name;
    }
  }
  const bool was_running = engine_.running();
  if (was_running) stop_device();
  engine_.set_radio(nullptr, nullptr);
  radio_.reset();
  stock_radio_data_ = {};
  radio_tried_ = false;
  const bool ok = init_radio(radio_game_root_, lang, current_station, radio_non_blocking_);
  if (was_running) start_device();
  return ok;
}

std::string AudioHost::car_burble_bank() const {
  return baked_sound_ ? baked_sound_->loaded.sound.burble.wavebank : std::string{};
}

bool AudioHost::init_sfx(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                         const std::string& burble_bank, const std::filesystem::path& state_root) {
  if (game_root.empty()) return false;
  if (!mixer_configs_tried_) {
    mixer_configs_tried_ = true;
    const bool was_running = engine_.running();
    if (was_running) stop_device();
    std::string mnote;
    if (engine_.load_mixer_configs(game_root, &mnote, state_root)) {
      std::printf("%s\n", mnote.c_str());
      engine_.activate_matrix_snapshot("FreeRoam");
    }
    if (was_running) start_device();
  }
  if (!sfx_tried_) {
    sfx_tried_ = true;
    sfx_game_root_ = game_root;
    sfx_cache_dir_ = cache_dir;
    auto sfx = std::make_unique<audio::SfxEngine>(kAudioRate);
    std::string snote;
    if (sfx->load(game_root, cache_dir, burble_bank, &snote, state_root)) {
      std::printf("%s\n", snote.c_str());
      if (!state_root.empty()) {
        std::string rnote;
        if (sfx->load_rally_sfx(state_root, cache_dir, &rnote) && !rnote.empty()) {
          std::printf("%s\n", rnote.c_str());
        }
        rally_sfx_tried_ = true;
      }
      ui_catalog_ = sfx->ui_catalog();
      if (ui_catalog_)
        std::printf("sfx engine: ui4audio %zu events, %d samples\n", ui_catalog_->events().size(),
                    ui_catalog_->sample_count());
      engine_.set_sfx_engine(std::move(sfx));
      has_sfx_ = true;
    }
    tires_cfg_.load(game_root, nullptr, state_root);
    engine_.init_ui_music(game_root, cache_dir);
  }
  return has_sfx_;
}

bool AudioHost::init_satnav(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                            std::string_view language, bool metric) {
  if (game_root.empty()) return false;
  satnav_game_root_ = game_root;
  satnav_cache_dir_ = cache_dir;
  satnav_language_ = std::string(language.empty() ? "EN" : language);
  satnav_metric_ = metric;
  if (!satnav_tried_) {
    satnav_tried_ = true;
    auto sn = std::make_unique<audio::SatNavVoPlayer>(kAudioRate);
    sn->set_enabled(cfg_.satnav_on);
    std::string snote;
    if (sn->load(game_root, cache_dir, satnav_language_, satnav_metric_, &snote)) {
      std::printf("%s\n", snote.c_str());
      engine_.set_satnav_vo(std::move(sn));
      has_satnav_ = true;
    }
  }
  return has_satnav_;
}

bool AudioHost::reload_satnav(std::string_view language, bool metric) {
  if (satnav_game_root_.empty()) return false;
  const std::string lang = std::string(language.empty() ? "EN" : language);
  if (has_satnav_ && lang == satnav_language_ && metric == satnav_metric_) return true;
  satnav_language_ = lang;
  satnav_metric_ = metric;
  auto sn = std::make_unique<audio::SatNavVoPlayer>(kAudioRate);
  sn->set_enabled(cfg_.satnav_on);
  std::string snote;
  if (sn->load(satnav_game_root_, satnav_cache_dir_, satnav_language_, satnav_metric_, &snote)) {
    std::printf("%s\n", snote.c_str());
    engine_.set_satnav_vo(std::move(sn));
    has_satnav_ = true;
    return true;
  }
  return false;
}

void AudioHost::set_ui_music(audio::AudioEngine::UiMusicMode mode) {
  if (mode == last_ui_music_) return;
  last_ui_music_ = mode;
  engine_.set_ui_music(mode);
}

void AudioHost::set_blocking_load(bool on) {
  if (on) set_ui_music(audio::AudioEngine::UiMusicMode::kNone);
  last_loading_ = on;
  if (engine_.has_snapshot_matrix()) sync_gameplay_snapshot();
  else engine_.set_snapshot(snap_loading_, on);
}

uint8_t AudioHost::map_tire_surface_group(std::string_view physics_surface) const {
  auto low = [](std::string_view sv) {
    std::string s(sv);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string s = low(physics_surface);
  if (s.find("mud") != std::string::npos) return 5;
  if (const auto* e = tires_cfg_.find_surface(physics_surface)) {
    const std::string g = low(e->lod1_group);
    if (g.find("mud") != std::string::npos) return 5;
    if (g.find("offroad") != std::string::npos || g.find("dirt") != std::string::npos ||
        g.find("gravel") != std::string::npos || g.find("sand") != std::string::npos) {
      return 1;
    }
    if (g.find("grass") != std::string::npos) return 2;
    if (g.find("brick") != std::string::npos || g.find("cobble") != std::string::npos) return 3;
    if (g.find("trackway") != std::string::npos || g.find("wood") != std::string::npos) return 4;
    return 0;
  }
  if (s.find("grass") != std::string::npos) return 2;
  if (s.find("dirt") != std::string::npos || s.find("gravel") != std::string::npos ||
      s.find("sand") != std::string::npos) {
    return 1;
  }
  if (s.find("brick") != std::string::npos || s.find("cobble") != std::string::npos) return 3;
  if (s.find("wood") != std::string::npos || s.find("track") != std::string::npos) return 4;
  return 0;
}

int16_t AudioHost::map_tire_surface_index(std::string_view physics_surface) const {
  if (const auto* e = tires_cfg_.find_surface(physics_surface)) {
    return int16_t(e->surface_index);
  }
  std::string s(physics_surface);
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (s.find("leaf") != std::string::npos || s.find("leaves") != std::string::npos) return 51;
  if (s.find("mud") != std::string::npos) return 20;
  if (s.find("grass") != std::string::npos) return 15;
  if (s.find("dirt") != std::string::npos || s.find("gravel") != std::string::npos ||
      s.find("sand") != std::string::npos) {
    return 10;
  }
  if (s.find("brick") != std::string::npos || s.find("cobble") != std::string::npos) return 30;
  if (s.find("wood") != std::string::npos || s.find("track") != std::string::npos) return 40;
  return 0;
}

bool AudioHost::init_codriver(const std::filesystem::path& rally_package_dir, const std::filesystem::path& cache_dir,
                              std::string_view language) {
  if (rally_package_dir.empty()) return false;
  if (!codriver_tried_) {
    codriver_tried_ = true;
    auto cd = std::make_unique<audio::CoDriverVoPlayer>(kAudioRate);
    std::string note;
    if (cd->load(rally_package_dir, cache_dir, language, &note)) {
      std::printf("%s\n", note.c_str());
      engine_.set_codriver_vo(std::move(cd));
      has_codriver_ = true;
    }
  }
  return has_codriver_;
}

bool AudioHost::init_vo(const std::filesystem::path& game_root, std::string_view language) {
  if (game_root.empty()) return false;
  vo_game_root_ = game_root;
  engine_.set_vo_base_root(game_root);
  if (!vo_tried_) {
    vo_tried_ = true;
    vo_language_ = std::string(language.empty() ? "EN" : language);
    auto vp = std::make_unique<audio::VoPlayer>(kAudioRate, true);
    std::string note;
    if (vp->load(game_root, vo_language_, &note)) {
      std::printf("%s\n", note.c_str());
      engine_.set_vo_player(std::move(vp));
      has_vo_ = true;
    }
  }
  return has_vo_;
}

bool AudioHost::reload_vo(std::string_view language) {
  if (vo_game_root_.empty()) return false;
  const std::string lang = std::string(language.empty() ? "EN" : language);
  if (has_vo_ && lang == vo_language_) return true;
  vo_language_ = lang;
  auto vp = std::make_unique<audio::VoPlayer>(kAudioRate, true);
  std::string note;
  if (vp->load(vo_game_root_, vo_language_, &note)) {
    std::printf("%s\n", note.c_str());
    engine_.set_vo_player(std::move(vp));
    has_vo_ = true;
    return true;
  }
  return false;
}

bool AudioHost::init_acoustics(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                               const std::filesystem::path& world_dir, const std::filesystem::path& rally_package,
                               bool async_soundscapes) {
  if (game_root.empty()) return false;
  auto low = [](std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string wname = low(world_dir.filename().string().empty() ? world_dir.parent_path().filename().string()
                                                                      : world_dir.filename().string());
  const bool is_rally = wname.find("coloradodirt") != std::string::npos ||
                        low(world_dir.string()).find("coloradodirt") != std::string::npos;
  const std::string world_key = is_rally ? "coloradodirt" : "colorado";

  if (!acoustics_tried_) {
    acoustics_tried_ = true;
    std::string note;
    bool ok = false;
    {
      std::lock_guard<std::mutex> lk(acoustics_mu_);
      ok = acoustics_.load_configs(game_root, &note);
    }
    if (ok) {
      std::printf("%s\n", note.c_str());
      has_acoustics_.store(true, std::memory_order_release);
    }
    const int beds = engine_.load_ambience_beds(game_root, cache_dir);
    if (beds > 0) {
      std::printf("world acoustics: queued %d AMB_*.fev ambience beds for async decode\n", beds);
    }
    std::lock_guard<std::mutex> lk(acoustics_mu_);
    engine_.want_ambience_events(placed_ambience_events(acoustics_));
  }
  if (!rally_acoustics_tried_ && !rally_package.empty()) {
    rally_acoustics_tried_ = true;
    std::string rnote;
    bool rok = false;
    {
      std::lock_guard<std::mutex> lk(acoustics_mu_);
      rok = acoustics_.load_rally_configs(rally_package, &rnote);
    }
    if (rok && !rnote.empty()) {
      std::printf("%s\n", rnote.c_str());
      has_acoustics_.store(true, std::memory_order_release);
    }
    const int rbeds = engine_.load_rally_ambience_beds(rally_package, cache_dir);
    if (rbeds > 0) {
      std::printf("world acoustics (rally): queued %d ColoradoDirt AMB_*.fev beds\n", rbeds);
    }
    std::string rvonote;
    if (engine_.load_rally_vo(rally_package, satnav_language_, &rvonote) && !rvonote.empty()) {
      std::printf("%s\n", rvonote.c_str());
    }
  }
  set_rally_world(is_rally);

  if (acoustics_world_key_ != world_key) {
    if (acoustics_loader_.joinable()) acoustics_loader_.join();
    uint64_t my_gen = 0;
    {
      std::lock_guard<std::mutex> lk(acoustics_mu_);
      acoustics_.clear_soundscapes();
      my_gen = ++acoustics_gen_;
    }
    last_in_tunnel_ = false;
    acoustics_world_key_ = world_key;
    std::error_code ec;
    std::filesystem::path archive_path;
    if (is_rally) {
      // Horizon Rally Expansion: locate ColoradoDirt_pri_65.zip or ColoradoDirt/bin.zip.
      std::vector<std::filesystem::path> cands;
      if (!rally_package.empty()) {
        cands.push_back(rally_package / "Media" / "DLCZips" / "ColoradoDirt_pri_65.zip");
        cands.push_back(rally_package / "media" / "dlczips" / "ColoradoDirt_pri_65.zip");
      }
      if (!world_dir.empty()) {
        cands.push_back(world_dir / "ColoradoDirt_pri_65.zip");
        cands.push_back(world_dir.parent_path().parent_path().parent_path().parent_path() / "ColoradoDirt_pri_65.zip");
        cands.push_back(world_dir / "bin.zip");
        cands.push_back(world_dir / "Bin.zip");
      }
      cands.push_back(cars::mods::resolve(game_root / "media" / "tracks" / "coloradodirt" / "bin.zip"));
      for (const auto& c : cands) {
        if (!c.empty() && std::filesystem::exists(c, ec)) {
          archive_path = c;
          break;
        }
      }
    } else {
      const auto bin_lower = cars::mods::resolve(game_root / "media" / "tracks" / "colorado" / "bin.zip");
      const auto bin_upper = cars::mods::resolve(game_root / "media" / "tracks" / "colorado" / "Bin.zip");
      archive_path = std::filesystem::exists(bin_lower, ec) ? bin_lower : bin_upper;
    }
    if (!archive_path.empty() && std::filesystem::exists(archive_path, ec)) {
      if (async_soundscapes) {
        audio::WorldAcousticsData base_copy;
        {
          std::lock_guard<std::mutex> lk(acoustics_mu_);
          base_copy = acoustics_;
        }
        acoustics_loader_ = std::thread([this, archive_path, world_key, my_gen, data = std::move(base_copy)]() mutable {
          const int tiles = data.load_soundscapes_from_archive(archive_path);
          if (tiles > 0) engine_.want_ambience_events(placed_ambience_events(data));
          if (tiles > 0) {
            std::lock_guard<std::mutex> lk(acoustics_mu_);
            if (acoustics_gen_ == my_gen) {
              const size_t rz = data.reverb_zone_count();
              const size_t az = data.ambience_zone_count();
              acoustics_ = std::move(data);
              has_acoustics_.store(true, std::memory_order_release);
              std::printf("world acoustics (%s): loaded %d .soundscape tiles (%zu reverb zones, %zu ambience zones)\n",
                          world_key.c_str(), tiles, rz, az);
            }
          }
        });
      } else {
        int tiles = 0;
        size_t rz = 0, az = 0;
        {
          std::lock_guard<std::mutex> lk(acoustics_mu_);
          tiles = acoustics_.load_soundscapes_from_archive(archive_path);
          if (tiles > 0) engine_.want_ambience_events(placed_ambience_events(acoustics_));
          rz = acoustics_.reverb_zone_count();
          az = acoustics_.ambience_zone_count();
        }
        if (tiles > 0) {
          has_acoustics_.store(true, std::memory_order_release);
          std::printf("world acoustics (%s): loaded %d .soundscape tiles (%zu reverb zones, %zu ambience zones)\n",
                      world_key.c_str(), tiles, rz, az);
        }
      }
    }
  }
  return has_acoustics_.load(std::memory_order_acquire);
}

void AudioHost::trigger_vo(std::string_view trigger_id, bool interrupt, int variant) {
  if (has_vo_) engine_.trigger_vo(trigger_id, interrupt, variant);
}

void AudioHost::skip_vo() {
  if (has_vo_) engine_.skip_vo();
}

void AudioHost::set_dialog_ducking(bool active) {
  if (active == last_dialog_duck_) return;
  last_dialog_duck_ = active;
  // Routed through AudioEngine so UI dialog ducking and VoPlayer::playing()
  // share a single unified ducking state and never cancel each other out.
  engine_.set_dialog_ducking(active);
}

void AudioHost::trigger_codriver(std::string_view cue_path, bool supersede) {
  if (has_codriver_) engine_.trigger_codriver(cue_path, supersede);
}

void AudioHost::clear_codriver() {
  if (has_codriver_) engine_.clear_codriver();
}

void AudioHost::radio_set_time_of_day(double hour) {
  // The ambience events read it too (CurrentTime), radio or not.
  if (!std::isfinite(hour)) return;
  if (have_tod_sent_ && std::fabs(hour - last_tod_hour_) < 0.05) return;
  have_tod_sent_ = true;
  last_tod_hour_ = hour;
  engine_.radio_set_time_of_day(hour);
}

void AudioHost::note_radio_miss(audio::RadioChannel channel, std::string_view gameplay_event) {
  if (!radio_ || gameplay_event.empty() || audio::radio_has_event(stock_radio_data_, channel, gameplay_event)) return;
  for (const auto& m : radio_missed_)
    if (audio::iequals(m, gameplay_event)) return;
  radio_missed_.emplace_back(gameplay_event);
  std::fprintf(stderr, "carview: note: RadioSystem.xml has no %s line for gameplayEvent %.*s\n",
               channel == audio::RadioChannel::kSpecial     ? "special"
               : channel == audio::RadioChannel::kImmediate ? "immediate"
                                                            : "festival update",
               int(gameplay_event.size()), gameplay_event.data());
}

void AudioHost::radio_trigger_special(std::string_view gameplay_event) {
  note_radio_miss(audio::RadioChannel::kSpecial, gameplay_event);
  if (radio_) engine_.radio_trigger_special(gameplay_event);
}

void AudioHost::radio_trigger_immediate(std::string_view gameplay_event) {
  switch (audio::parse_radio_dj_state(gameplay_event)) {
    case audio::RadioDjState::kFestivalWristband:
      radio_wristband_awarded(0);
      return;
    case audio::RadioDjState::kSpecialWristband:
      radio_wristband_ready(0);
      return;
    case audio::RadioDjState::kPostRace:
      // Recognized empty radiodj.post_race state.
      return;
    case audio::RadioDjState::kCycleRadioLogos: {
      // CHudRadioCycleLogos (first_time_career Radio_Unlock): cycle through the
      // unlocked FM stations on the HUD radio popup strip.
      std::vector<int> stations;
      for (int idx : radio_data_.cycle) {
        if (idx >= 0 && size_t(idx) < radio_data_.stations.size() && !radio_data_.stations[size_t(idx)].off) {
          stations.push_back(idx);
        }
      }
      if (stations.empty()) {
        for (size_t i = 0; i < radio_data_.stations.size(); ++i) {
          if (!radio_data_.stations[i].off) stations.push_back(int(i));
        }
      }
      if (!stations.empty()) {
        std::lock_guard<std::mutex> lk(logo_cycle_mutex());
        auto& st = logo_cycles()[this];
        st.active = true;
        st.t0 = std::chrono::steady_clock::now();
        st.stations = std::move(stations);
      }
      return;
    }
    case audio::RadioDjState::kNone:
      break;
  }
  if (radio_) {
    audio::RadioChannel ch = audio::RadioChannel::kImmediate;
    if (audio::resolve_radio_event_channel(stock_radio_data_, audio::RadioChannel::kImmediate, gameplay_event, ch) &&
        ch != audio::RadioChannel::kImmediate) {
      if (ch == audio::RadioChannel::kFestivalUpdate) {
        radio_trigger_festival_update(gameplay_event);
      } else {
        radio_trigger_special(gameplay_event);
      }
      return;
    }
  }
  note_radio_miss(audio::RadioChannel::kImmediate, gameplay_event);
  if (radio_) engine_.radio_trigger_immediate(gameplay_event);
}

void AudioHost::radio_trigger_festival_update(std::string_view gameplay_event) {
  note_radio_miss(audio::RadioChannel::kFestivalUpdate, gameplay_event);
  if (radio_) engine_.radio_trigger_festival_update(gameplay_event);
}

void AudioHost::radio_cue(const audio::RadioDjCue& cue) {
  switch (cue.channel) {
    case audio::RadioChannel::kSpecial: radio_trigger_special(cue.event); break;
    case audio::RadioChannel::kImmediate: radio_trigger_immediate(cue.event); break;
    case audio::RadioChannel::kFestivalUpdate: radio_trigger_festival_update(cue.event); break;
  }
}

void AudioHost::radio_wristband_awarded(int level) {
  if (wristband_sting_pending_) {
    wristband_sting_pending_ = false;
    play_ui(audio::ui_wristband_unlock_play(level + 1));  // `level` is 0 based, the stings 01..07
  }
  bool cancel = false;
  const auto cues = radio_dj_wristbands_.on_awarded(level, &cancel);
  if (cancel && radio_) engine_.radio_cancel_festival_updates();
  for (const auto& c : cues) radio_cue(c);
}

void AudioHost::radio_wristband_ready(int level) {
  for (const auto& c : radio_dj_wristbands_.on_ready(level)) radio_cue(c);
}

void AudioHost::radio_festival_event_finished(int event_level, int position) {
  for (const auto& c : radio_dj_wristbands_.on_event_finished(event_level, position)) radio_cue(c);
}

void AudioHost::radio_schedule_special(std::string_view gameplay_event, double delay_s) {
  if (gameplay_event.empty()) return;
  if (delay_s <= 0.0) {
    radio_trigger_special(gameplay_event);
    return;
  }
  radio_cancel_scheduled(gameplay_event);
  radio_scheduled_.push_back({std::string(gameplay_event),
                              std::chrono::steady_clock::now() +
                                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(delay_s))});
}

void AudioHost::radio_cancel_scheduled(std::string_view gameplay_event) {
  std::erase_if(radio_scheduled_, [&](const ScheduledRadioLine& l) { return audio::iequals(l.event, gameplay_event); });
}

void AudioHost::radio_lock_viper_intro(bool lock) {
  viper_intro_locked_ = lock;
  if (radio_) engine_.radio_lock_viper_intro(lock);
}

bool AudioHost::radio_set_playlist(const std::vector<std::string>& fmodnames, bool then_normal) {
  if (fmodnames.empty()) return false;
  if (!radio_) {
    // Played once the radio is built.
    flow_playlist_ = fmodnames;
    flow_playlist_then_normal_ = then_normal;
    return false;
  }
  flow_playlist_.clear();
  // The station carrying the first track, selected here too so the station
  // cycle (custom stations included) agrees with the audio thread's pick.
  for (size_t i = 0; i < stock_radio_data_.stations.size(); ++i) {
    const auto& st = stock_radio_data_.stations[i];
    if (st.off) continue;
    const bool has = std::any_of(st.tracks.begin(), st.tracks.end(), [&](const audio::RadioTrack& t) {
      return t.name.size() == fmodnames[0].size() &&
             std::equal(t.name.begin(), t.name.end(), fmodnames[0].begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
             });
    });
    if (!has) continue;
    bool in_cycle = false;
    for (size_t p = 0; p < unified_cycle_.size(); ++p)
      if (unified_cycle_[p] == int(i)) {
        select_unified_station(int(p), false);
        in_cycle = true;
        break;
      }
    if (!in_cycle) {
      active_custom_idx_ = -1;
      active_custom_station_ = -1;
      engine_.runtime_radio_select(-1);
    }
    engine_.radio_set_playlist(fmodnames, then_normal);
    return true;
  }
  return false;
}

void AudioHost::radio_set_switching(bool on) {
  switching_enabled_ = on;
  if (radio_) engine_.radio_set_switching(on);
}

void AudioHost::radio_set_flow_dj(bool on) {
  flow_dj_off_ = !on;
  engine_.radio_set_dj(cfg_.radio_dj && !flow_dj_off_);
}

void AudioHost::radio_set_silent_idents(bool on) {
  flow_silent_idents_ = on;
  if (radio_) engine_.radio_set_silent_idents(on);
}

void AudioHost::radio_reset_flow() {
  flow_playlist_.clear();
  radio_set_flow_dj(true);
  radio_set_switching(true);
  radio_set_silent_idents(false);
  radio_cancel_playlist();
}

void AudioHost::radio_hold_popup(bool held) {
  if (radio_) engine_.radio_hold_popup(held);
}

void AudioHost::radio_stop_dj() {
  if (radio_) engine_.radio_stop_dj();
}

void AudioHost::radio_cancel_playlist() {
  if (radio_) engine_.radio_cancel_playlist();
}

bool AudioHost::radio_select_station_by_name(std::string_view substr) {
  if (!has_radio() || substr.empty()) return false;
  auto lower = [](std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
  };
  const std::string want = lower(substr);
  for (size_t i = 0; i < radio_data_.stations.size(); ++i) {
    if (lower(radio_data_.stations[i].name).find(want) != std::string::npos) {
      for (size_t p = 0; p < unified_cycle_.size(); ++p) {
        if (unified_cycle_[p] == int(i)) {
          select_unified_station(int(p), false);
          return true;
        }
      }
      if (radio_ && i < stock_radio_data_.stations.size()) {
        active_custom_idx_ = -1;
        active_custom_station_ = -1;
        engine_.runtime_radio_select(-1);
        engine_.radio_select(int(i));
        return true;
      }
    }
  }
  return false;
}

void AudioHost::update_world_acoustics(float x, float y, float z, float heading_rad) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(heading_rad)) return;
  const auto now = std::chrono::steady_clock::now();
  if (have_acoustics_sent_) {
    const float dx = x - last_acoustics_pos_[0];
    const float dy = y - last_acoustics_pos_[1];
    const float dz = z - last_acoustics_pos_[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float dh = std::fabs(heading_rad - last_acoustics_heading_);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_acoustics_tp_).count();
    // Avoid 60-240 Hz SPSC command spam and per-frame emitter string allocations.
    if (d2 < 0.25f && dh < 0.02f && elapsed_ms < 50) return;
    if (elapsed_ms < 25 && d2 < 25.0f && dh < 0.15f) return;
  }
  have_acoustics_sent_ = true;
  last_acoustics_pos_ = {x, y, z};
  last_acoustics_heading_ = heading_rad;
  last_acoustics_tp_ = now;
  if (radio_) engine_.set_listener_pose({x, y, z}, heading_rad);
  if (has_acoustics_.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> lk(acoustics_mu_, std::try_to_lock);
    if (lk.owns_lock()) {
      const auto sample = acoustics_.evaluate(x, y, z, heading_rad);
      last_in_tunnel_ = sample.in_tunnel;
      engine_.set_acoustics_sample(sample);
    }
  }
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
  if (stream_ || engine_.running() || !cfg_.sound || !has_audio()) return;
  last_device_try_tp_ = std::chrono::steady_clock::now();
  const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kAudioRate};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) {
    if (!device_open_failed_) {
      std::fprintf(stderr, "carview: no audio device: %s\n", SDL_GetError());
    }
    device_open_failed_ = true;
    return;
  }
  device_open_failed_ = false;
  if (!SDL_ResumeAudioStreamDevice(stream_)) {
    std::fprintf(stderr, "carview: failed to resume audio device: %s\n", SDL_GetError());
  }
  audio::AudioEngine::Sink sink;
  sink.target_buffer_frames = int(0.10 * kAudioRate);  // 100 ms ahead for hitch-immune playback
  sink.block_frames = 480;                             // 10 ms render blocks
  SDL_AudioStream* s = stream_;
  sink.queued_frames = [s]() {
    const int bytes = SDL_GetAudioStreamQueued(s);
    if (bytes < 0) return -1;
    return bytes / int(2 * sizeof(float));
  };
  sink.write = [s](const float* stereo, int frames) {
    if (frames > 0) SDL_PutAudioStreamData(s, stereo, frames * 2 * int(sizeof(float)));
  };
  engine_.start(std::move(sink));
}

void AudioHost::ensure_device_started() {
  if (stream_ || engine_.running() || !cfg_.sound || !has_audio()) return;
  if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) return;
  if (device_open_failed_) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_device_try_tp_ < std::chrono::seconds(2)) return;
  }
  start_device();
}

void AudioHost::set_fmv_audio(audio::AudioEngine::FmvAudioFn fn, void* ctx) {
  const bool next = (fn != nullptr);
  const bool changed = (next != has_fmv_audio_);
  has_fmv_audio_ = next;
  engine_.set_fmv_audio(fn, ctx);
  if (has_fmv_audio_) ensure_device_started();
  if (changed && engine_.has_snapshot_matrix()) sync_gameplay_snapshot();
}

void AudioHost::start_benchmark_device() {
  if (engine_.running() || !cfg_.sound || !has_audio()) return;
  auto start_tp = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
  auto written = std::make_shared<int64_t>(0);
  audio::AudioEngine::Sink sink;
  sink.target_buffer_frames = int(0.04 * kAudioRate);  // 40 ms virtual queue
  sink.block_frames = 480;                             // 10 ms render blocks
  sink.queued_frames = [start_tp, written]() {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - *start_tp).count();
    const int64_t played = int64_t(elapsed * double(kAudioRate));
    const int64_t queued = *written - played;
    return queued > 0 ? int(std::min<int64_t>(queued, kAudioRate)) : 0;
  };
  sink.write = [written](const float* /*stereo*/, int frames) { *written += frames; };
  engine_.start(std::move(sink));
}

void AudioHost::stop_device() {
  engine_.stop();
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  device_open_failed_ = false;
}

void AudioHost::sync_gameplay_snapshot() {
  if (!engine_.has_snapshot_matrix()) return;
  const bool interior_active = (homespace_mode_ > 0 || last_hub_ambience_ > 0);
  const GameplayOverlay want = last_loading_          ? GameplayOverlay::kLoading
                               : has_fmv_audio_       ? GameplayOverlay::kVideo
                               : last_rewinding_      ? GameplayOverlay::kRewind
                               : barnfind_active_     ? GameplayOverlay::kBarnfind
                               : last_paused_         ? GameplayOverlay::kPaused
                               : interior_active      ? GameplayOverlay::kInterior
                               : pre_event_active_    ? GameplayOverlay::kPreEvent
                               : (last_on_title_ || last_menu_) ? GameplayOverlay::kMenu
                               : race_encounter_active_         ? GameplayOverlay::kRaceEncounter
                                                                : GameplayOverlay::kNone;
  if (want != last_overlay_) {
    last_overlay_ = want;
    switch (want) {
      case GameplayOverlay::kLoading: engine_.activate_matrix_snapshot("Loading"); break;
      case GameplayOverlay::kVideo: engine_.activate_matrix_snapshot("Video"); break;
      case GameplayOverlay::kRewind: engine_.activate_matrix_snapshot("Rewind"); break;
      case GameplayOverlay::kBarnfind: engine_.activate_matrix_snapshot("Barnfind"); break;
      case GameplayOverlay::kPaused: engine_.activate_matrix_snapshot("Paused"); break;
      case GameplayOverlay::kInterior: engine_.activate_matrix_snapshot("UIInterior"); break;
      case GameplayOverlay::kPreEvent: engine_.activate_matrix_snapshot("PreEvent"); break;
      case GameplayOverlay::kMenu: engine_.activate_matrix_snapshot("UIExterior"); break;
      case GameplayOverlay::kRaceEncounter: engine_.activate_matrix_snapshot("RaceEncounter"); break;
      case GameplayOverlay::kNone:
        if (last_in_race_ != applied_in_race_) {
          applied_in_race_ = last_in_race_;
          engine_.activate_matrix_snapshot(last_in_race_ ? "Race" : "FreeRoam");
        } else {
          engine_.restore_gameplay_snapshot();
        }
        break;
    }
  } else if (want == GameplayOverlay::kNone && last_in_race_ != applied_in_race_) {
    applied_in_race_ = last_in_race_;
    engine_.activate_matrix_snapshot(last_in_race_ ? "Race" : "FreeRoam");
  }
}

void AudioHost::update_frame(const audio::EnginePlayer::Update& u, bool paused, bool menu_open, bool on_title,
                             bool loading) {
  // Ensure the SDL playback stream and audio render thread are running during interactive sessions
  // (no-op when already started, in benchmark mode, or when SDL_INIT_AUDIO is not active).
  ensure_device_started();
  if (!radio_scheduled_.empty()) {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> due;
    std::erase_if(radio_scheduled_, [&](const ScheduledRadioLine& l) {
      if (l.due > now) return false;
      due.push_back(l.event);
      return true;
    });
    for (const auto& ev : due) radio_trigger_special(ev);
  }
  audio::EnginePlayer::Update clean_u = u;
  if (!std::isfinite(clean_u.rpm)) clean_u.rpm = 0.0;
  if (!std::isfinite(clean_u.throttle)) clean_u.throttle = 0.0;
  if (!std::isfinite(clean_u.pos_torque)) clean_u.pos_torque = 0.0;
  if (!std::isfinite(clean_u.neg_torque)) clean_u.neg_torque = 0.0;
  last_audio_view_ = clean_u.view;
  engine_.set_engine_input(clean_u);
  if (!have_on_title_ || on_title != last_on_title_) {
    have_on_title_ = true;
    last_on_title_ = on_title;
    // The own Title snapshot only without the game's matrix; with it the
    // title is the matrix's UIExterior state (sync_gameplay_snapshot).
    engine_.set_snapshot(snap_title_, on_title && !engine_.has_snapshot_matrix());
  }

  // Hot-reload radio_stations.json and drain async YouTube playlist resolutions (~1 Hz).
  if (runtime_radio_configured_) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_radio_poll_ >= std::chrono::seconds(1)) {
      last_radio_poll_ = now;
      if (runtime_radio_mgr_.poll_updates()) {
        sync_runtime_radio(true);
      }
    }
  }

  last_loading_ = loading;
  last_paused_ = paused;
  last_menu_ = menu_open;
  if (engine_.has_snapshot_matrix()) {
    sync_gameplay_snapshot();
  } else {
    engine_.set_snapshot(snap_paused_, paused);
    engine_.set_snapshot(snap_menu_, menu_open);
    engine_.set_snapshot(snap_loading_, loading);
  }
}

void AudioHost::update_vehicle(const audio::VehicleAudioState& st, bool in_race, bool rewinding,
                               bool countdown_active, int countdown_digit, float countdown_seq_elapsed_s) {
  audio::VehicleAudioState eff = st;
  const audio::View active_view =
      st.cockpit_view ? audio::View::kCockpit
                      : (last_audio_view_ == audio::View::kFront ? audio::View::kFront : audio::View::kFollow);
  if (baked_sound_) {
    const auto& es = baked_sound_->loaded.sound;
    eff.trans_vol_scale = float(es.transmission_volume);
    eff.shift_vol_scale = float(es.shift_volume);
    eff.turbo_vol_scale = float(es.turbo_volume);
    eff.sc_vol_scale = float(es.supercharger_volume);
    eff.backfire_vol_scale = float(es.backfire_volume);
    eff.burble_vol_scale = float(es.burble_volume > 0.0 ? es.burble_volume : es.backfire_volume);
    eff.exh_noise_vol_scale = float(es.exhaust_noise_volume > 0.0 ? es.exhaust_noise_volume : 1.0);
    eff.susp_vol_scale = float(es.suspension_volume);
    eff.ev_vol_scale = float(es.electric_motor_volume);
    eff.num_cylinders = es.num_cylinders;
    eff.blowoff_level = es.blowoff_level;
    eff.blowoff_choice = es.blowoff_choice;
    if (es.burble.max_time_ms > 0.0) {
      const double span = std::max(500.0, es.audio_rpm_redline - es.audio_rpm_idle);
      const auto to_norm = [&](double r) -> float {
        if (r <= 2.0) return float(std::clamp(r, 0.0, 1.25));
        return float(std::clamp((r - es.audio_rpm_idle) / span, 0.0, 1.25));
      };
      eff.burble_min_norm_rpm = to_norm(es.burble.min_rpm);
      eff.burble_max_norm_rpm = to_norm(es.burble.max_rpm > 0.0 ? es.burble.max_rpm : 1.0);
      eff.burble_min_time_ms = float(es.burble.min_time_ms);
      eff.burble_max_time_ms = float(es.burble.max_time_ms);
      eff.burble_silence_weight = float(es.burble.silence_weighting);
    }
    if (es.has_other_volumes) {
      const auto& ov = es.other_volumes[size_t(active_view)];
      eff.view_wind_scale = float(ov.wind);
      eff.view_tyre_scale = float(ov.tires);
      eff.view_trans_scale = float(ov.transmission);
      eff.view_turbo_scale = float(ov.turbo);
      eff.view_sc_scale = float(ov.supercharger);
      eff.view_backfire_scale = float(ov.backfire);
      eff.view_burble_scale = float(ov.burble);
      eff.view_susp_scale = float(ov.suspension);
      eff.view_ambience_scale = float(ov.reflections);
      eff.view_other_cars_scale = (active_view == audio::View::kCockpit) ? float(std::min(ov.wind, ov.reflections)) : 1.0f;
      eff.view_damage_scale = float(ov.engine_damage);
      eff.view_exh_noise_scale = float(ov.exhaust_noise);
      eff.view_ev_scale = float(ov.electric_motor);
      eff.view_lfe_scale = float(ov.engine_lfe);
      eff.view_refl_scale = float(ov.reflections);
    } else if (st.cockpit_view) {
      eff.view_wind_scale = 0.72f;
      eff.view_tyre_scale = 0.78f;
      eff.view_trans_scale = 1.12f;
      eff.view_turbo_scale = 0.85f;
      eff.view_sc_scale = 0.90f;
      eff.view_backfire_scale = 0.72f;
      eff.view_burble_scale = 0.72f;
      eff.view_susp_scale = 1.05f;
      eff.view_ambience_scale = 0.68f;
      eff.view_other_cars_scale = 0.68f;
      eff.view_damage_scale = 1.10f;
      eff.view_exh_noise_scale = 0.72f;
      eff.view_refl_scale = 0.68f;
    }
  } else if (st.cockpit_view) {
    eff.view_wind_scale = 0.72f;
    eff.view_tyre_scale = 0.78f;
    eff.view_trans_scale = 1.12f;
    eff.view_turbo_scale = 0.85f;
    eff.view_sc_scale = 0.90f;
    eff.view_backfire_scale = 0.72f;
    eff.view_burble_scale = 0.72f;
    eff.view_susp_scale = 1.05f;
    eff.view_ambience_scale = 0.68f;
    eff.view_other_cars_scale = 0.68f;
    eff.view_damage_scale = 1.10f;
    eff.view_exh_noise_scale = 0.72f;
    eff.view_refl_scale = 0.68f;
  }
  eff.rally_mode = eff.rally_mode || rally_world_;
  if (homespace_mode_ > 0 && eff.hub_ambience == 0) {
    eff.hub_ambience = uint8_t(homespace_mode_);
  }
  last_hub_ambience_ = int(eff.hub_ambience);
  engine_.set_vehicle_state(eff);
  set_radio_rally_mode(in_race && rally_world_);

  // Rewind snapshot + TapeStart / TapeStop transitions (plus clearing stale Co-Driver queue).
  if (rewinding != last_rewinding_) {
    if (rewinding) {
      trigger_ui(audio::UiCue::kRewindTapeStart);
      clear_codriver();
    } else {
      trigger_ui(audio::UiCue::kRewindTapeStop);
    }
  }
  last_rewinding_ = rewinding;
  last_in_race_ = in_race;
  sync_ambience_state(in_race);
  if (engine_.has_snapshot_matrix()) {
    sync_gameplay_snapshot();
  }

  // ThreeTwoOneDSPConfig windowing: honor GlobalDSPConfig [countdown_start_s, countdown_end_s].
  bool countdown_dsp_in_window = false;
  if (countdown_active) {
    const auto& gdsp = engine_.global_dsp();
    const double start_s = double(gdsp.countdown_start_s);
    const double end_s = double(std::max(gdsp.countdown_end_s, gdsp.countdown_start_s + 0.1f));
    if (countdown_seq_elapsed_s >= 0.0f) {
      countdown_dsp_in_window =
          double(countdown_seq_elapsed_s) >= start_s && double(countdown_seq_elapsed_s) <= end_s;
    } else {
      if (!last_countdown_active_) countdown_t0_ = std::chrono::steady_clock::now();
      const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - countdown_t0_).count();
      const double seq_s = start_s + elapsed_s;
      countdown_dsp_in_window = (seq_s >= start_s && seq_s <= end_s);
    }
  }

  const audio::CarDspMode dsp = rewinding               ? audio::CarDspMode::kRewind
                                : countdown_dsp_in_window ? audio::CarDspMode::kCountdown
                                : st.cockpit_view       ? audio::CarDspMode::kCockpit
                                                        : audio::CarDspMode::kOff;
  if (dsp != last_dsp_mode_) {
    last_dsp_mode_ = dsp;
    engine_.set_car_bus_dsp_mode(dsp);
  }

  // Trigger the pre-race 3-2-1 countdown track ONCE at countdown start; subsequent digits use
  // short kCountdownTick (suppressed while kCountdown321 plays), followed by a single kCountdownGo.
  if (countdown_active) {
    if (!last_countdown_active_) {
      last_countdown_digit_ = countdown_digit;
      trigger_ui(countdown_digit <= 0 ? audio::UiCue::kCountdownGo : audio::UiCue::kCountdown321);
    } else if (countdown_digit != last_countdown_digit_) {
      last_countdown_digit_ = countdown_digit;
      trigger_ui(countdown_digit <= 0 ? audio::UiCue::kCountdownGo : audio::UiCue::kCountdownTick);
    }
  } else if (last_countdown_active_) {
    if (last_countdown_digit_ == 1) trigger_ui(audio::UiCue::kCountdownGo);
    last_countdown_digit_ = -1;
  }
  last_countdown_active_ = countdown_active;
}

void AudioHost::set_homespace_mode(int mode) {
  const int clamped = std::clamp(mode, 0, 3);
  if (clamped == homespace_mode_) return;
  homespace_mode_ = clamped;
  engine_.set_homespace_mode(clamped);
  if (engine_.has_snapshot_matrix()) sync_gameplay_snapshot();
}

void AudioHost::set_speed_zone_active(bool active) {
  if (active == speed_zone_active_) return;
  speed_zone_active_ = active;
  if (engine_.has_snapshot_matrix()) {
    engine_.activate_matrix_snapshot(active ? "AverageSpeedEnter" : "AverageSpeedExit");
  }
}

void AudioHost::trigger_speed_cam() {
  if (engine_.has_snapshot_matrix()) {
    engine_.activate_matrix_snapshot("SpeedCam");
  }
  trigger_ui(audio::UiCue::kSpeedCamera);
}

void AudioHost::set_window_minimized(bool minimized) {
  if (minimized == window_minimized_) return;
  window_minimized_ = minimized;
  engine_.set_mute(cfg_.mute || !cfg_.sound || window_minimized_);
}

void AudioHost::sync_ambience_state(bool in_race) {
  // InRace: the race is running (the same flag that picks the Race snapshot).
  // CameraType's two values are not decoded (Q80): it stays at 0.
  const float want[4] = {in_race ? 1.0f : 0.0f, amb_pre_race_, 0.0f, amb_crowd_};
  if (std::equal(std::begin(want), std::end(want), std::begin(amb_sent_))) return;
  std::copy(std::begin(want), std::end(want), std::begin(amb_sent_));
  engine_.set_ambience_state(want[0], want[1], want[2], want[3]);
}

void AudioHost::set_pre_race_ambience(float v) { amb_pre_race_ = std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f); }

void AudioHost::set_crowd_excitement(float v) { amb_crowd_ = std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f); }

void AudioHost::set_pre_event(bool active) {
  if (active == pre_event_active_) return;
  pre_event_active_ = active;
  if (engine_.has_snapshot_matrix()) sync_gameplay_snapshot();
}

void AudioHost::set_barnfind_active(bool active) {
  if (active == barnfind_active_) return;
  barnfind_active_ = active;
  if (engine_.has_snapshot_matrix()) sync_gameplay_snapshot();
}

void AudioHost::set_race_encounter_active(bool active) {
  if (active == race_encounter_active_) return;
  race_encounter_active_ = active;
  if (engine_.has_snapshot_matrix()) sync_gameplay_snapshot();
}

void AudioHost::trigger_collision(const audio::CollisionHit& hit) { engine_.trigger_collision(hit); }

void AudioHost::trigger_landing(const audio::LandingHit& hit) { engine_.trigger_landing(hit); }

void AudioHost::set_rally_world(bool on) {
  rally_world_ = on;
  acoustics_.set_rally_world(on);
  engine_.set_rally_world(on);
}

void AudioHost::set_radio_rally_mode(bool on) {
  if (on == last_radio_rally_mode_) return;
  last_radio_rally_mode_ = on;
  engine_.set_radio_rally_mode(on);
}

void AudioHost::trigger_ui(audio::UiCue cue) {
  if (!ui_catalog_) {
    engine_.trigger_ui(cue);
    return;
  }
  if (cue == audio::UiCue::kWristbandUnlock) {
    // The sting is per wristband: radio_wristband_awarded(level) plays it.
    wristband_sting_pending_ = true;
    return;
  }
  if (const auto play = audio::ui_cue_play_name(cue, race_audio_kind_); !play.empty()) {
    play_ui(play);
    return;
  }
  if (const auto path = audio::ui_cue_event_path(cue); !path.empty()) {
    if (const auto* ev = ui_catalog_->event(ui_catalog_->find_event(path)); ev && ev->playable)
      engine_.trigger_ui_event(*ev);
  }
}

void AudioHost::set_ui_scene(std::string_view scene) {
  if (scene != ui_scene_) ui_scene_ = std::string(scene);
}

bool AudioHost::play_ui(std::string_view play) { return play_ui(play, ui_scene_); }

bool AudioHost::play_ui(std::string_view play, std::string_view scene) {
  if (!ui_catalog_ || play.empty()) return false;
  const int at = ui_catalog_->resolve(play, scene);
  if (at == audio::UiAudioCatalog::kUnknown) {
    if (std::find(ui_unknown_.begin(), ui_unknown_.end(), play) == ui_unknown_.end()) {
      ui_unknown_.emplace_back(play);
      std::fprintf(stderr, "carview: note: ui4audio has no play name '%.*s'\n", int(play.size()), play.data());
    }
    return false;
  }
  const auto* ev = ui_catalog_->event(at);
  if (!ev) return false;  // Blank in this scene, or no sample
  engine_.trigger_ui_event(*ev);
  return true;
}

bool AudioHost::play_ui_event(std::string_view path) {
  if (!ui_catalog_ || path.empty()) return false;
  const int at = ui_catalog_->resolve_event(path);
  const auto* ev = ui_catalog_->event(at);
  if (!ev) return false;
  engine_.trigger_ui_event(*ev);
  return true;
}

void AudioHost::activate_mix_snapshot(std::string_view name) {
  if (!engine_.has_snapshot_matrix() || engine_.find_matrix_snapshot(name) < 0) return;
  engine_.activate_matrix_snapshot(name);
}

void AudioHost::trigger_satnav(audio::SatNavCue cue) {
  if (cfg_.satnav_on) engine_.trigger_satnav(cue);
}

void AudioHost::set_satnav_enabled(bool on) {
  cfg_.satnav_on = on;
  engine_.set_satnav_enabled(on);
}

void AudioHost::radio_press(int dir) {
  if (!has_radio() || viper_intro_locked_ || !switching_enabled_) return;
  if (custom_stations_.empty() && radio_) {
    engine_.radio_press(dir);
    return;
  }
  if (unified_cycle_.empty()) return;
  select_unified_station(unified_pos_ + (dir >= 0 ? 1 : -1), true);
}

bool AudioHost::handle_key(SDL_Keycode key, SDL_Keymod mod, std::string& notice) {
  if (key == SDLK_F9) {
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
    const auto tel = engine_.telemetry();
    if (tel.user_music_active) {
      if (shift) engine_.user_music_prev();
      else engine_.user_music_next();
      notice = std::string("your music: ") + (shift ? "previous track" : "next track");
    } else if (active_custom_idx_ >= 0) {
      if (shift) engine_.runtime_radio_prev();
      else engine_.runtime_radio_next();
      custom_popup_t0_ = std::chrono::steady_clock::now();
      custom_popup_active_ = true;
      custom_popup_station_ = active_custom_station_;
      notice = std::string("custom radio: ") + (shift ? "previous track" : "next track");
    } else {
      notice = "your music: off (enable in settings, add mp3/wav to music folder)";
    }
    return true;
  }
  return false;
}

audio::AudioTelemetry AudioHost::telemetry() {
  audio::AudioTelemetry tel = engine_.telemetry();
  if (active_custom_idx_ >= 0 && active_custom_station_ >= 0 &&
      size_t(active_custom_station_) < radio_data_.stations.size()) {
    auto& st = radio_data_.stations[size_t(active_custom_station_)];
    if (st.tracks.empty()) st.tracks.emplace_back();
    if (tel.runtime_radio_title[0] != '\0') {
      st.tracks[0].title = tel.runtime_radio_title.data();
      st.tracks[0].artist = tel.runtime_radio_artist.data();
    }
    if (tel.runtime_radio_track_seq != 0 && tel.runtime_radio_track_seq != last_custom_track_seq_) {
      last_custom_track_seq_ = tel.runtime_radio_track_seq;
      custom_popup_t0_ = std::chrono::steady_clock::now();
      custom_popup_active_ = true;
      custom_popup_station_ = active_custom_station_;
    }
    tel.radio_selected = active_custom_station_;
    const double alpha = compute_custom_popup_alpha();
    if (alpha > 0.0) {
      tel.radio_popup.station = active_custom_station_;
      tel.radio_popup.track = 0;
      tel.radio_popup.alpha = alpha;
    } else {
      tel.radio_popup = {};
    }
  } else if (custom_popup_active_ && custom_popup_station_ >= 0 &&
             size_t(custom_popup_station_) < radio_data_.stations.size()) {
    const double alpha = compute_custom_popup_alpha();
    if (alpha > 0.0 && tel.radio_popup.alpha < alpha) {
      tel.radio_popup.station = custom_popup_station_;
      tel.radio_popup.track = tel.radio_popup.station == custom_popup_station_ ? tel.radio_popup.track : -1;
      tel.radio_popup.alpha = alpha;
    } else if (alpha <= 0.0) {
      custom_popup_active_ = false;
    }
  }
  {
    std::lock_guard<std::mutex> lk(logo_cycle_mutex());
    if (auto it = logo_cycles().find(this); it != logo_cycles().end() && it->second.active) {
      auto& st = it->second;
      constexpr double kStepS = 1.1;
      constexpr double kFadeS = 0.2;
      const double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - st.t0).count();
      const double total = kStepS * double(st.stations.size());
      if (elapsed >= 0.0 && elapsed < total && !st.stations.empty()) {
        const size_t idx = std::min(st.stations.size() - 1, size_t(elapsed / kStepS));
        double alpha = 1.0;
        if (elapsed < kFadeS) alpha = elapsed / kFadeS;
        else if (elapsed > total - kFadeS) alpha = (total - elapsed) / kFadeS;
        tel.radio_popup.station = st.stations[idx];
        tel.radio_popup.track = -1;
        tel.radio_popup.alpha = std::clamp(alpha, 0.0, 1.0);
        tel.radio_popup.press = true;
      } else {
        st.active = false;
      }
    }
  }
  return tel;
}

void AudioHost::render_offline(const audio::EnginePlayer::Update& u, float* out, int frames) {
  engine_.set_engine_input(u);
  engine_.render_offline(out, frames);
}

void AudioHost::save_radio_state(drive::DriveSave& save) {
  const auto tel = telemetry();
  if (has_radio() && tel.radio_selected >= 0 && size_t(tel.radio_selected) < radio_data_.stations.size()) {
    save.radio_station = radio_data_.stations[size_t(tel.radio_selected)].name;
  } else if (radio_) {
    save.radio_station = radio_->selected_name();
  }
  save.radio_dj = cfg_.radio_dj;
  save.radio_volume = double(cfg_.radio_volume);
  save.satnav_enabled = cfg_.satnav_on;
}

}  // namespace carview
