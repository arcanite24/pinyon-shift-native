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
  audio::Snapshot dd;
  dd.name = "DialogDuck";
  dd.dest[int(audio::Bus::kMusic)] = 0.25f;
  dd.dest[int(audio::Bus::kEngine)] = 0.50f;
  dd.dest[int(audio::Bus::kOtherCars)] = 0.50f;
  dd.dest[int(audio::Bus::kTyres)] = 0.60f;
  dd.dest[int(audio::Bus::kWind)] = 0.55f;
  dd.dest[int(audio::Bus::kAmbience)] = 0.45f;
  dd.fade = 0.18f;
  dd.return_fade = 0.35f;
  snap_dialog_duck_ = engine_.register_snapshot(std::move(dd));
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
                                                    redline_rpm, baked->intake_chuff());
    ep->master = kEngineMaster;
    baked_sound_ = std::move(baked);
    engine_.set_engine_player(std::move(ep));
    has_car_ = true;
    if (has_sfx_ && !sfx_game_root_.empty()) {
      const std::string bb = car_burble_bank();
      if (!bb.empty()) engine_.reload_sfx_burble(sfx_game_root_, sfx_cache_dir_, bb);
    }
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
      engine_.radio_select(stock_off_station_);
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
      engine_.radio_select(st_idx);
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
  ensure_runtime_radio_configured();
  if (!radio_tried_) {
    radio_tried_ = true;
    std::string why;
    if (audio::load_radio(game_root, stock_radio_data_, &why, language)) {
      radio_ = std::make_unique<audio::Radio>(stock_radio_data_, std::random_device{}(), cfg_.radio_dj);
      int start_station = stock_radio_data_.cycle.empty() ? -1 : stock_radio_data_.cycle[0];
      if (!saved_station.empty()) {
        const int found = radio_->find_station(saved_station);
        if (found >= 0) start_station = found;
      }
      if (start_station >= 0) radio_->select(start_station);
      std::printf("radio: %zu stations in the cycle, %zu tracks, %zu voice clips (%s)\n",
                  stock_radio_data_.cycle.size(), stock_radio_data_.music.size(), stock_radio_data_.vo.size(),
                  stock_radio_data_.language.c_str());
    } else {
      std::fprintf(stderr, "carview: note: no radio: %s\n", why.c_str());
    }
  }
  sync_runtime_radio(false);
  if (!custom_stations_.empty()) {
    std::printf("runtime radio: %zu custom station(s) appended to cycle\n", custom_stations_.size());
  }
  if (radio_) {
    auto rp = std::make_unique<audio::RadioPlayer>(*radio_, kAudioRate, non_blocking);
    rp->master = kRadioMaster;
    rp->volume = cfg_.radio_on ? double(cfg_.radio_volume) : 0.0;
    // Register canonical Colorado 3D location stations (Festival stages, Autoshow, Workshop, Paintshop).
    std::vector<audio::LocationRadioEmitter> locs;
    auto add_loc = [&](std::string_view st_name, std::array<float, 3> pos, float max_d) {
      const int idx = radio_->find_station(st_name);
      if (idx >= 0) locs.push_back({idx, pos, 14.0f, max_d});
    };
    add_loc("Radio5_3d_PreRace", {-1468.0f, 26.0f, -345.0f}, 280.0f);
    add_loc("Radio5_3d_PreRace", {-1398.0f, 1.0f, -22.0f}, 240.0f);
    add_loc("Radio6_3d_Autoshow", {-1020.0f, 0.0f, -260.0f}, 160.0f);
    add_loc("Radio7_3d_Workshop", {-960.0f, 0.0f, -180.0f}, 160.0f);
    add_loc("Radio8_3d_Paintshop", {-1080.0f, 0.0f, -190.0f}, 160.0f);
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
    if (sfx->load(game_root, cache_dir, burble_bank, &snote)) {
      std::printf("%s\n", snote.c_str());
      engine_.set_sfx_engine(std::move(sfx));
      has_sfx_ = true;
    }
    tires_cfg_.load(game_root, nullptr);
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

uint8_t AudioHost::map_tire_surface_group(std::string_view physics_surface) const {
  auto low = [](std::string_view sv) {
    std::string s(sv);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  if (const auto* e = tires_cfg_.find_surface(physics_surface)) {
    const std::string g = low(e->lod1_group);
    if (g.find("offroad") != std::string::npos || g.find("dirt") != std::string::npos ||
        g.find("gravel") != std::string::npos || g.find("sand") != std::string::npos) {
      return 1;
    }
    if (g.find("grass") != std::string::npos) return 2;
    if (g.find("brick") != std::string::npos || g.find("cobble") != std::string::npos) return 3;
    if (g.find("trackway") != std::string::npos || g.find("wood") != std::string::npos) return 4;
    return 0;
  }
  const std::string s = low(physics_surface);
  if (s.find("grass") != std::string::npos) return 2;
  if (s.find("dirt") != std::string::npos || s.find("gravel") != std::string::npos ||
      s.find("sand") != std::string::npos || s.find("mud") != std::string::npos) {
    return 1;
  }
  if (s.find("brick") != std::string::npos || s.find("cobble") != std::string::npos) return 3;
  if (s.find("wood") != std::string::npos || s.find("track") != std::string::npos) return 4;
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
  if (!vo_tried_) {
    vo_tried_ = true;
    auto vp = std::make_unique<audio::VoPlayer>(kAudioRate, true);
    std::string note;
    if (vp->load(game_root, language, &note)) {
      std::printf("%s\n", note.c_str());
      engine_.set_vo_player(std::move(vp));
      has_vo_ = true;
    }
  }
  return has_vo_;
}

bool AudioHost::init_acoustics(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
                               const std::filesystem::path& world_dir, const std::filesystem::path& rally_package) {
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
    if (acoustics_.load_configs(game_root, &note)) {
      std::printf("%s\n", note.c_str());
      has_acoustics_ = true;
    }
    const int beds = engine_.load_ambience_beds(game_root, cache_dir);
    if (beds > 0) {
      std::printf("world acoustics: queued %d AMB_*.fev ambience beds for async decode\n", beds);
    }
  }

  if (acoustics_world_key_ != world_key) {
    acoustics_.clear_soundscapes();
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
      const int tiles = acoustics_.load_soundscapes_from_archive(archive_path);
      if (tiles > 0) {
        has_acoustics_ = true;
        std::printf("world acoustics (%s): loaded %d .soundscape tiles (%zu reverb zones, %zu ambience zones)\n",
                    world_key.c_str(), tiles, acoustics_.reverb_zone_count(), acoustics_.ambience_zone_count());
      }
    }
  }
  return has_acoustics_;
}

void AudioHost::trigger_vo(std::string_view trigger_id, bool interrupt) {
  if (has_vo_) engine_.trigger_vo(trigger_id, interrupt);
}

void AudioHost::skip_vo() {
  if (has_vo_) engine_.skip_vo();
}

void AudioHost::set_dialog_ducking(bool active) {
  if (active == last_dialog_duck_) return;
  last_dialog_duck_ = active;
  if (engine_.has_snapshot_matrix()) {
    engine_.activate_matrix_snapshot(active ? "VOPlaying" : "VONotPlaying");
    return;
  }
  if (snap_dialog_duck_ >= 0) engine_.set_snapshot(snap_dialog_duck_, active);
}

void AudioHost::trigger_codriver(std::string_view cue_path) {
  if (has_codriver_) engine_.trigger_codriver(cue_path);
}

void AudioHost::clear_codriver() {
  if (has_codriver_) engine_.clear_codriver();
}

void AudioHost::radio_set_time_of_day(double hour) {
  if (!radio_ || !std::isfinite(hour)) return;
  if (have_tod_sent_ && std::fabs(hour - last_tod_hour_) < 0.05) return;
  have_tod_sent_ = true;
  last_tod_hour_ = hour;
  engine_.radio_set_time_of_day(hour);
}

void AudioHost::radio_trigger_special(std::string_view gameplay_event) {
  if (radio_) engine_.radio_trigger_special(gameplay_event);
}

void AudioHost::radio_trigger_immediate(std::string_view gameplay_event) {
  if (radio_) engine_.radio_trigger_immediate(gameplay_event);
}

void AudioHost::radio_trigger_festival_update(std::string_view gameplay_event) {
  if (radio_) engine_.radio_trigger_festival_update(gameplay_event);
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
  if (has_acoustics_) engine_.set_acoustics_sample(acoustics_.evaluate(x, y, z));
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
}

void AudioHost::update_frame(const audio::EnginePlayer::Update& u, bool paused, bool menu_open, bool on_title,
                             bool loading) {
  audio::EnginePlayer::Update clean_u = u;
  if (!std::isfinite(clean_u.rpm)) clean_u.rpm = 0.0;
  if (!std::isfinite(clean_u.throttle)) clean_u.throttle = 0.0;
  if (!std::isfinite(clean_u.pos_torque)) clean_u.pos_torque = 0.0;
  if (!std::isfinite(clean_u.neg_torque)) clean_u.neg_torque = 0.0;
  engine_.set_engine_input(clean_u);
  if (!have_on_title_ || on_title != last_on_title_) {
    have_on_title_ = true;
    last_on_title_ = on_title;
    engine_.set_snapshot(snap_title_, on_title);
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

  if (engine_.has_snapshot_matrix()) {
    if (loading != last_loading_) {
      last_loading_ = loading;
      if (loading) engine_.activate_matrix_snapshot("Loading");
      else if (!paused && !menu_open && !last_rewinding_) engine_.restore_gameplay_snapshot();
    }
    if (paused != last_paused_) {
      last_paused_ = paused;
      if (paused && !last_rewinding_) engine_.activate_matrix_snapshot("Paused");
      else if (!paused && !menu_open && !loading && !last_rewinding_) engine_.restore_gameplay_snapshot();
    }
    if (menu_open != last_menu_) {
      last_menu_ = menu_open;
      if (menu_open && !paused) engine_.activate_matrix_snapshot("UIExterior");
      else if (!menu_open && !paused && !loading && !last_rewinding_) engine_.restore_gameplay_snapshot();
    }
  } else {
    engine_.set_snapshot(snap_paused_, paused);
    engine_.set_snapshot(snap_menu_, menu_open);
    engine_.set_snapshot(snap_loading_, loading);
  }
}

void AudioHost::update_vehicle(const audio::VehicleAudioState& st, bool in_race, bool rewinding,
                               bool countdown_active, int countdown_digit) {
  audio::VehicleAudioState eff = st;
  if (baked_sound_) {
    const auto& es = baked_sound_->loaded.sound;
    eff.trans_vol_scale = float(es.transmission_volume);
    eff.shift_vol_scale = float(es.shift_volume);
    eff.turbo_vol_scale = float(es.turbo_volume);
    eff.sc_vol_scale = float(es.supercharger_volume);
    eff.backfire_vol_scale = float(es.backfire_volume);
    eff.susp_vol_scale = float(es.suspension_volume);
    eff.ev_vol_scale = float(es.electric_motor_volume);
    eff.num_cylinders = es.num_cylinders;
  }
  engine_.set_vehicle_state(eff);
  if (engine_.has_snapshot_matrix()) {
    if (in_race != last_in_race_) {
      last_in_race_ = in_race;
      engine_.activate_matrix_snapshot(in_race ? "Race" : "FreeRoam");
    }
    if (rewinding != last_rewinding_) {
      if (rewinding) {
        engine_.activate_matrix_snapshot("Rewind");
        engine_.trigger_ui(audio::UiCue::kRewindTapeStart);
      } else if (!last_paused_ && !last_menu_ && !last_loading_) {
        engine_.restore_gameplay_snapshot();
      }
    }
  } else if (rewinding && !last_rewinding_) {
    engine_.trigger_ui(audio::UiCue::kRewindTapeStart);
  }
  last_rewinding_ = rewinding;

  const audio::CarDspMode dsp = rewinding          ? audio::CarDspMode::kRewind
                                : countdown_active ? audio::CarDspMode::kCountdown
                                : st.cockpit_view  ? audio::CarDspMode::kCockpit
                                                   : audio::CarDspMode::kOff;
  if (dsp != last_dsp_mode_) {
    last_dsp_mode_ = dsp;
    engine_.set_car_bus_dsp_mode(dsp);
  }

  if (countdown_active && countdown_digit != last_countdown_digit_) {
    last_countdown_digit_ = countdown_digit;
    engine_.trigger_ui(countdown_digit <= 0 ? audio::UiCue::kCountdownGo : audio::UiCue::kCountdown321);
  } else if (!countdown_active) {
    if (last_countdown_digit_ == 1) engine_.trigger_ui(audio::UiCue::kCountdownGo);
    last_countdown_digit_ = -1;
  }
}

void AudioHost::trigger_collision(const audio::CollisionHit& hit) { engine_.trigger_collision(hit); }

void AudioHost::trigger_ui(audio::UiCue cue) { engine_.trigger_ui(cue); }

void AudioHost::trigger_satnav(audio::SatNavCue cue) {
  if (cfg_.satnav_on) engine_.trigger_satnav(cue);
}

void AudioHost::set_satnav_enabled(bool on) {
  cfg_.satnav_on = on;
  engine_.set_satnav_enabled(on);
}

void AudioHost::radio_press(int dir) {
  if (!has_radio()) return;
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
