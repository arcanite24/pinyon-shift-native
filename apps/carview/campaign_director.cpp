#include "campaign_director.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio_host.h"
#include "race/route.h"

namespace carview {
namespace {

std::array<double, 3> local_to_world(const std::array<double, 3>& origin, double heading, double right, double up,
                                     double forward) {
  const double s = std::sin(heading);
  const double c = std::cos(heading);
  // Car forward = (sin(h), 0, cos(h)), car right = (cos(h), 0, -sin(h)).
  return {
      origin[0] + right * c + forward * s,
      origin[1] + up,
      origin[2] - right * s + forward * c,
  };
}

std::array<double, 3> normalize_vec(const std::array<double, 3>& v) {
  const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len < 1e-6) return {0.0, 0.0, 1.0};
  return {v[0] / len, v[1] / len, v[2] / len};
}

constexpr double kPi = 3.14159265358979323846;

// The old onboarding stage strings a save may hold, mapped into the
// first-time career.
bool ftc_stage_for_legacy(std::string_view s, race::FtcStage& out) {
  if (s == "intro" || s == "opening" || s == "opening_cutscene" || s == "viper_drive" || s == "viper" ||
      s == "intro_drive") {
    out = race::FtcStage::kOpeningCutscene;
    return true;
  }
  if (s == "qualify" || s == "corrado_qualify") {
    out = race::FtcStage::kRaceToFestival;
    return true;
  }
  if (s == "registration" || s == "starter_handoff") {
    out = race::FtcStage::kFestivalArriveMovie;
    return true;
  }
  return false;
}

constexpr std::string_view kFtcSavePrefix = "ftc:";

// CCutsceneAudioTrigger: the trigger names its FEV group and event
// ("UIInGame/UI/Cutscenes" + "CS_OpeningSequence"); the event plays by that
// path, else as a ui4audio play name of the same name.
template <class Host>
bool play_ui_name(Host* audio, std::string_view group, std::string_view play) {
  if constexpr (requires(Host* a, std::string_view n) { a->play_ui(n); }) {
    if (!group.empty()) {
      std::string path(group);
      path += '/';
      path += play;
      if (audio->play_ui_event(path)) return true;
    }
    return audio->play_ui(play);
  } else {
    (void)group;
    (void)audio;
    static bool told = false;
    if (!told) {
      told = true;
      std::fprintf(stderr, "carview: cutscene audio trigger %.*s: no ui4audio playback on this build (silent)\n",
                   int(play.size()), play.data());
    }
    return false;
  }
}

// The first place a cutscene frames: its first player-car placement or
// camera node.
bool first_place(const cutscene::Cutscene& cs, std::array<double, 3>& out) {
  for (const auto& e : cs.events)
    if (e.type == cutscene::EventType::kPlacePlayerCar || e.type == cutscene::EventType::kCameraNode) {
      out = e.position();
      return true;
    }
  return false;
}

FmvMovieId movie_for_cutscene(race::CutsceneKind kind) {
  switch (kind) {
    case race::CutsceneKind::kOpeningFmv: return FmvMovieId::kFmv01Opening;
    case race::CutsceneKind::kWristbandCeremonyFmv: return FmvMovieId::kWristbandGet;
    case race::CutsceneKind::kFinaleCreditsFmv: return FmvMovieId::kFmv04Credits;
    default: return FmvMovieId::kFmv01Opening;
  }
}

}  // namespace

std::array<double, 3> eval_cubic_bezier(const std::array<double, 3>& p0, const std::array<double, 3>& p1,
                                        const std::array<double, 3>& p2, const std::array<double, 3>& p3, double t) {
  t = std::clamp(t, 0.0, 1.0);
  const double u = 1.0 - t;
  const double b0 = u * u * u;
  const double b1 = 3.0 * u * u * t;
  const double b2 = 3.0 * u * t * t;
  const double b3 = t * t * t;
  return {
      b0 * p0[0] + b1 * p1[0] + b2 * p2[0] + b3 * p3[0],
      b0 * p0[1] + b1 * p1[1] + b2 * p2[1] + b3 * p3[1],
      b0 * p0[2] + b1 * p1[2] + b2 * p2[2] + b3 * p3[2],
  };
}

void CampaignDirector::configure(const std::filesystem::path& game_root, std::string_view language,
                                 bool skip_intro_movies) {
  game_root_ = game_root;
  language_ = language.empty() ? "EN" : std::string(language);
  skip_intro_movies_ = skip_intro_movies;
}

void CampaignDirector::load_from_save(const drive::DriveSave& in, int popularity_rank) {
  drive::DriveSave seeded;
  const bool from_profile = race::imported_campaign_needs_seed(in);
  if (from_profile) {
    seeded = in;
    race::seed_imported_campaign(seeded, popularity_rank);
  }
  const drive::DriveSave& save = from_profile ? seeded : in;
  race::CampaignSaveState st;
  st.campaign_stage = save.campaign_stage;
  st.completed_cutscenes = save.completed_cutscenes;
  st.heard_dialogs = save.heard_dialogs;
  st.unlocked_bosses = save.unlocked_bosses;
  st.unlocked_showcases = save.unlocked_showcases;
  st.finale_complete = save.finale_complete;
  if (st.campaign_stage.empty() && !save.intro_completed) {
    st.campaign_stage = "viper_drive";
  }
  // A first-time career in progress (or an old save's onboarding stage)
  // resumes in race::FirstTimeCareer; CampaignFlow waits in free roam.
  race::FtcStage fs{};
  const std::string_view stage = st.campaign_stage;
  if ((stage.substr(0, kFtcSavePrefix.size()) == kFtcSavePrefix &&
       race::parse_ftc_stage(stage.substr(kFtcSavePrefix.size()), fs)) ||
      ftc_stage_for_legacy(stage, fs)) {
    ftc_resume_pending_ = fs != race::FtcStage::kComplete && fs != race::FtcStage::kNotStarted;
    ftc_resume_stage_ = fs;
    st.campaign_stage = "freeroam";
  }
  flow_.load_from(st);
}

void CampaignDirector::save_to_save(drive::DriveSave& save) const {
  race::CampaignSaveState st;
  flow_.save_to(st);
  save.campaign_stage = std::move(st.campaign_stage);
  save.completed_cutscenes = std::move(st.completed_cutscenes);
  save.heard_dialogs = std::move(st.heard_dialogs);
  save.unlocked_bosses = std::move(st.unlocked_bosses);
  save.unlocked_showcases = std::move(st.unlocked_showcases);
  save.finale_complete = st.finale_complete;
  if (ftc_.active() || ftc_resume_pending_) {
    // The activity's last save checkpoint (none before the diner).
    const race::FtcStage s = ftc_resume_pending_ ? ftc_resume_stage_ : ftc_.checkpoint();
    if (s != race::FtcStage::kComplete) {
      save.campaign_stage = std::string(kFtcSavePrefix) + race::ftc_stage_name(s);
      save.intro_completed = false;
      return;
    }
    // The rewards are in (the yellow wristband): the career is over.
    save.intro_completed = true;
    return;
  }
  if (!flow_.is_onboarding()) save.intro_completed = true;
}

void CampaignDirector::dispatch_action_media(const race::CampaignAction& act, FmvPlayer& fmv, AudioHost* audio) {
  if (act.play_fmv) {
    const FmvMovieId mid = movie_for_cutscene(act.fmv_cutscene);
    const double fb_dur = act.fmv_cutscene == race::CutsceneKind::kFinaleCreditsFmv ? 16.0 : 8.0;
    if (fmv.open_or_fallback(game_root_, mid, language_, 1280, 720, fb_dur) && audio) {
      audio->set_fmv_audio(
          [](void* ctx, float* out, int frames) { return static_cast<FmvPlayer*>(ctx)->read_audio(out, frames); },
          &fmv);
    }
  }
  if (act.lock_viper_radio && audio) {
    // Authentic FH1 Minute-0 Viper opening: tune to Horizon Bass Arena for the
    // 2-track festival drive playlist ("Levels (Skrillex Remix)" -> "Language")
    // and lock the radio until the Viper opening drive completes.
    if (!audio->radio_select_station_by_name("Bass Arena")) {
      audio->radio_select_station_by_name("Bass");
    }
    audio->radio_lock_viper_intro(true);
    audio->radio_trigger_immediate(audio::kViperOpeningRadioEvent);
  }
  if (act.intro_completed && audio) {
    audio->radio_lock_viper_intro(false);
  }
  pump_audio(audio);
}

void CampaignDirector::pump_audio(AudioHost* audio) {
  if (!audio) return;
  std::string vo_trig, radio_ev;
  if (flow_.dialogs().consume_pending_audio(vo_trig, radio_ev)) {
    if (!vo_trig.empty()) audio->trigger_vo(vo_trig, false);
    if (!radio_ev.empty()) audio->radio_trigger_immediate(radio_ev);
  }
  audio->set_dialog_ducking(flow_.dialogs().active());
}

race::CampaignAction CampaignDirector::start_new_career(FmvPlayer& fmv, AudioHost* audio,
                                                        std::array<double, 3> festival_gate) {
  (void)festival_gate;
  skip_hold_progress_ = 0.0f;
  if (audio) audio->radio_lock_viper_intro(false);
  // The original's first-time career (intro_flow / first_time_career.xml):
  // no FMV at the start, the opening is the in-engine Opening_Cutscene.
  race::CampaignAction none;
  flow_.jump_to_cli_stage("freeroam", none);
  cutscenes_.ensure_loaded(game_root_);
  ftc_resume_pending_ = false;
  ftc_pending_.clear();
  tutor_ = race::DrivingTutor{};
  prime_ftc_world();
  return apply_ftc(ftc_.start(), fmv, audio);
}

void CampaignDirector::prime_ftc_world() {
  // Route 170 (the opening's CLoadRoute): the race to the festival's route.
  race::Route route;
  if (race::load_route(game_root_, race::FirstTimeCareer::kInitialDriveRoute, route, nullptr)) {
    std::vector<std::array<double, 3>> wps;
    for (const auto* t : route.waypoints()) wps.push_back({t->x, t->y, t->z});
    ftc_.set_route_waypoints(std::move(wps));
  }
  if (!catalog_) return;
  // The heats final signs up in the FESTIVAL_SECOND_NODE race activation's
  // 27 m sphere; the showcase at its own trigger.
  if (const auto* ev = catalog_->find_event_by_id(race::FirstTimeCareer::kAllComerFinal))
    ftc_.set_event_trigger(ev->event_id, ev->trigger_pos, race::FirstTimeCareer::kHeatsSignUpRadius);
  if (const auto* ev = catalog_->find_event_by_id(race::FirstTimeCareer::kMustangRace))
    ftc_.set_event_trigger(ev->event_id, ev->trigger_pos, ev->trigger_radius);
  if (const auto* rc = catalog_->find_site(race::FestivalSite::kRaceCentral)) ftc_.set_race_central(rc->pos, rc->radius);
}

std::vector<CampaignDirector::CutsceneCar> CampaignDirector::take_cutscene_cars() {
  std::vector<CutsceneCar> out;
  out.swap(cutscene_cars_);
  return out;
}

std::vector<std::array<double, 3>> CampaignDirector::take_cutscene_preloads() {
  std::vector<std::array<double, 3>> out;
  out.swap(cutscene_preloads_);
  return out;
}

void CampaignDirector::update_tutor(double dt) {
  map_hint_left_ = std::max(0.0, map_hint_left_ - std::max(0.0, dt));
  if (!tutor_.active()) return;
  // The hints wait while the player has no control (cutscenes, screens).
  if (cutscenes_.active() || (ftc_.active() && !ftc_.controls_enabled()) || ftc_in_.screen_open) return;
  race::TutorInput t;
  t.dt = dt;
  t.throttle = ftc_in_.throttle;
  t.speed_mps = ftc_in_.speed_mps;
  t.impact = ftc_in_.impact;
  t.rewind_available = ftc_in_.rewind_available;
  t.rewind_pressed = ftc_in_.rewind_pressed;
  t.ebrake_pressed = ftc_in_.ebrake_pressed;
  t.turn_ahead = ftc_in_.turn_ahead;
  t.turn_distance_m = ftc_in_.turn_distance_m;
  tutor_.update(t);
}

std::vector<race::FtcCommands> CampaignDirector::take_ftc_commands() {
  std::vector<race::FtcCommands> out;
  out.swap(ftc_pending_);
  return out;
}

bool CampaignDirector::take_place_car(std::array<double, 3>& pos, double& heading_rad, double& speed_mps) {
  if (!place_pending_) return false;
  place_pending_ = false;
  pos = place_pos_;
  heading_rad = place_heading_;
  speed_mps = place_speed_;
  return true;
}

bool CampaignDirector::take_stream_anchor(const std::array<double, 3>& car_pos, double car_heading_rad,
                                          std::array<double, 3>& pos, double& heading_rad) {
  const cutscene::Cutscene* cs = cutscenes_.current();
  if (!cs) {
    anchor_cs_ = nullptr;
    anchor_cam_ = -1;
    return false;
  }
  const int cam = cs->cam_at(cutscenes_.time());
  if (cs == anchor_cs_ && cam == anchor_cam_) return false;
  anchor_cs_ = cs;
  anchor_cam_ = cam;
  drive::CameraView v;
  if (!cutscenes_.camera(car_pos, car_heading_rad, v)) return false;
  if (std::hypot(v.eye[0] - car_pos[0], v.eye[2] - car_pos[2]) < 300.0) return false;
  const double n = std::max(1e-6, std::hypot(v.forward[0], v.forward[2]));
  pos = {v.eye[0] - v.forward[0] / n * 12.0, v.eye[1], v.eye[2] - v.forward[2] / n * 12.0};
  heading_rad = std::atan2(v.forward[0], v.forward[2]);
  return true;
}

bool CampaignDirector::start_cutscene_capture(std::string_view name, double t) {
  if (!cutscenes_.ensure_loaded(game_root_) || !cutscenes_.play(name, false)) return false;
  // The cutscene's area streams first, as before the original plays one.
  std::array<double, 3> at{};
  if (cutscenes_.current() && first_place(*cutscenes_.current(), at)) cutscene_preloads_.push_back(at);
  handle_cutscene_events(cutscenes_.seek_and_freeze(t), nullptr);
  place_speed_ = 0.0;  // a still frame: the car stays where it is placed
  return true;
}

void CampaignDirector::handle_cutscene_events(const std::vector<const cutscene::Event*>& events, AudioHost* audio) {
  for (const auto* e : events) {
    switch (e->type) {
      case cutscene::EventType::kPlacePlayerCar:
        place_pending_ = true;
        place_pos_ = e->position();
        place_heading_ = e->rotation_deg() * kPi / 180.0;
        place_speed_ = cutscene::place_speed_mps(*e);  // initialSpeed is mph
        break;
      case cutscene::EventType::kVoiceOver:
        if (audio && !e->attr("event").empty()) audio->trigger_vo(e->attr("event"), false);
        break;
      case cutscene::EventType::kAudio:
        if (audio && !e->attr("event").empty()) play_ui_name(audio, e->attr("group"), e->attr("event"));
        break;
      case cutscene::EventType::kMixSnapshot:
        if (audio && !e->attr("snapshot").empty()) audio->activate_mix_snapshot(e->attr("snapshot"));
        break;
      case cutscene::EventType::kCar: {
        CutsceneCar car;
        car.name = e->name;
        car.car_id = int64_t(e->number("CarID"));
        car.pos = e->position();
        car.heading_rad = e->rotation_deg() * kPi / 180.0;
        cutscene_cars_.push_back(std::move(car));
        break;
      }
      case cutscene::EventType::kReleaseCar: {
        CutsceneCar car;
        car.name = e->attr("carName", e->name);
        car.release = true;
        car.speed_mps = cutscene::place_speed_mps(*e);
        cutscene_cars_.push_back(std::move(car));
        break;
      }
      default:
        // Animations (props, people) and the driver show / hide: no engine
        // backend yet (interim).
        break;
    }
  }
}

race::CampaignAction CampaignDirector::apply_ftc(const race::FtcCommands& c, FmvPlayer& fmv, AudioHost* audio) {
  race::CampaignAction act;
  // A black out lasts until the next fade, cutscene or movie, or until the
  // flow moves to a stage that scripts no fade of its own (interim, Q61 e).
  if (ftc_.stage() != ftc_last_stage_ && c.fades.empty()) cutscenes_.clear_fade();
  ftc_last_stage_ = ftc_.stage();
  for (const auto& f : c.fades) cutscenes_.fade(f);
  if (c.activate_tutor) tutor_.activate();
  // The map hint: on screen as long as a tutor hint (interim, Q63).
  if (c.show_map_hint) map_hint_left_ = race::DrivingTutor::kBrakeMessageTimeout;
  if (c.open_autoshow && c.autoshow_restricted) ftc_autoshow_restricted_ = true;
  if (!c.preload_cutscene.empty() && cutscenes_.ensure_loaded(game_root_)) {
    // CPrepareCutscene: the world around the cutscene's first place streams
    // before it plays.
    std::array<double, 3> at{};
    if (const auto* cs = cutscenes_.library().find(c.preload_cutscene); cs && first_place(*cs, at))
      cutscene_preloads_.push_back(at);
  }
  if (!c.play_movie.empty()) cutscenes_.clear_fade();
  if (c.stop_cutscene) cutscenes_.stop();
  if (!c.play_cutscene.empty()) {
    cutscenes_.ensure_loaded(game_root_);
    cutscenes_.play(c.play_cutscene, c.hold_cutscene);
  }
  if (!c.play_movie.empty()) {
    const FmvMovieId id = c.play_movie == "FMV_02" ? FmvMovieId::kFmv02Ceremony : FmvMovieId::kFmv01Opening;
    if (fmv.open_or_fallback(game_root_, id, language_, 1280, 720, 8.0) && audio) {
      audio->set_fmv_audio(
          [](void* ctx, float* out, int frames) { return static_cast<FmvPlayer*>(ctx)->read_audio(out, frames); },
          &fmv);
    }
  }
  if (audio) {
    // The flow's radio states in its order: DJ, playlist, switching, idents.
    if (c.set_radio_dj) audio->radio_set_flow_dj(c.radio_dj);
    // CSetRadioPlaylist normalBehaviourAfterListPlayed="true".
    if (!c.radio_playlist.empty()) audio->radio_set_playlist(c.radio_playlist, true);
    if (c.set_radio_switching) audio->radio_set_switching(c.radio_switching);
    if (c.set_silent_idents) audio->radio_set_silent_idents(c.silent_idents);
    if (c.stop_dj) audio->radio_stop_dj();
    for (const auto& m : c.mix_snapshots) audio->activate_mix_snapshot(m);
    for (const auto& v : c.vo) audio->trigger_vo(v, false);
    for (const auto& d : c.dj_lines) audio->radio_trigger_immediate(d);
  }
  if (c.swap_car) {
    act.swap_car = true;
    act.car_media = c.car_media;
    act.car_id = c.car_id;
  }
  if (c.place_car) {
    act.teleport = true;
    act.pos = c.pos;
    act.heading = c.heading_deg * kPi / 180.0;
  }
  if (c.set_destination && (c.destination_xz[0] != 0.0 || c.destination_xz[1] != 0.0)) {
    act.set_gps = true;
    act.gps_label = c.destination_label;
    act.gps_xz = c.destination_xz;
  }
  if (c.launch_event_id) {
    act.launch_event = true;
    act.event_id = std::to_string(c.launch_event_id);
  }
  act.open_autoshow = c.open_autoshow;
  act.collect_yellow_wristband = c.collect_yellow_wristband;
  act.intro_completed = c.intro_completed;
  act.save_requested = c.save_requested;
  ftc_pending_.push_back(c);
  return act;
}

race::CampaignAction CampaignDirector::update_ftc(double dt, FmvPlayer& fmv, AudioHost* audio) {
  if (ftc_resume_pending_) {
    ftc_resume_pending_ = false;
    cutscenes_.ensure_loaded(game_root_);
    prime_ftc_world();
    // A save past the hand-over: the tutor is already on.
    tutor_ = race::DrivingTutor{};
    if (ftc_resume_stage_ > race::FtcStage::kInitialDrive) tutor_.activate();
    return apply_ftc(ftc_.resume(ftc_resume_stage_), fmv, audio);
  }
  if (ftc_in_.loading) return {};
  update_tutor(dt);
  bool vo_playing = false;
  if (audio) vo_playing = audio->telemetry().vo_playing;
  handle_cutscene_events(cutscenes_.update(dt, vo_playing), audio);
  race::FtcInput in;
  in.dt = dt;
  in.player_pos = ftc_in_.player_pos;
  in.cutscene_active = cutscenes_.active();
  in.movie_active = fmv.active();
  in.throttle = ftc_in_.throttle;
  in.confirm = ftc_confirm_;
  in.screen_open = ftc_in_.screen_open;
  in.race_finished = ftc_race_finished_;
  in.race_event_id = ftc_race_event_;
  in.race_place = ftc_race_place_;
  in.racing = ftc_in_.racing;
  ftc_confirm_ = false;
  ftc_race_finished_ = false;
  auto act = apply_ftc(ftc_.update(in), fmv, audio);
  if (ftc_.complete()) {
    act.intro_completed = true;
    act.save_requested = true;
  }
  return act;
}

bool CampaignDirector::apply_cli_stage(std::string_view cli_stage, FmvPlayer& fmv, AudioHost* audio,
                                       race::CampaignAction& out_action) {
  skip_hold_progress_ = 0.0f;
  const auto prev_stage = flow_.stage();
  race::FtcStage fs{};
  if ((cli_stage.substr(0, kFtcSavePrefix.size()) == kFtcSavePrefix &&
       race::parse_ftc_stage(cli_stage.substr(kFtcSavePrefix.size()), fs)) ||
      ftc_stage_for_legacy(cli_stage, fs)) {
    race::CampaignAction none;
    flow_.jump_to_cli_stage("freeroam", none);
    cutscenes_.ensure_loaded(game_root_);
    prime_ftc_world();
    tutor_ = race::DrivingTutor{};
    if (fs > race::FtcStage::kInitialDrive) tutor_.activate();
    out_action = apply_ftc(ftc_.resume(fs), fmv, audio);
    return true;
  }
  if (!flow_.jump_to_cli_stage(cli_stage, out_action)) return false;
  if (audio && ftc_.active()) {
    // Out of the opening: the radio as the opening found it.
    audio->radio_set_flow_dj(true);
    audio->radio_set_switching(true);
    audio->radio_set_silent_idents(false);
    audio->radio_cancel_playlist();
  }
  ftc_ = race::FirstTimeCareer{};
  ftc_resume_pending_ = false;
  cutscenes_.stop();
  if (audio && prev_stage == race::CampaignStage::kViperIntroDrive &&
      flow_.stage() != race::CampaignStage::kViperIntroDrive) {
    audio->radio_lock_viper_intro(false);
  }
  dispatch_action_media(out_action, fmv, audio);
  return true;
}

race::CampaignAction CampaignDirector::update(double dt, std::array<double, 2> player_xz, FmvPlayer& fmv,
                                              AudioHost* audio, bool race_finished, bool holding_skip) {
  dt = std::clamp(dt, 0.0, 0.25);
  if (holding_skip && (fmv.active() || flow_.is_cinematic_active())) {
    skip_hold_progress_ = std::min(1.0f, skip_hold_progress_ + float(dt / 0.75));
    flow_.cinematic().skip_hold_progress = skip_hold_progress_;
    if (skip_hold_progress_ >= 1.0f) {
      skip_hold_progress_ = 0.0f;
      return advance_or_skip(fmv, audio);
    }
  } else {
    skip_hold_progress_ = std::max(0.0f, skip_hold_progress_ - float(dt * 3.0));
    flow_.cinematic().skip_hold_progress = skip_hold_progress_;
  }

  if (ftc_.active() || ftc_resume_pending_) return update_ftc(dt, fmv, audio);
  update_tutor(dt);
  if (audio) {
    const auto tel = audio->telemetry();
    flow_.dialogs().set_vo_telemetry(tel.vo_playing, tel.vo_elapsed_s, tel.vo_duration_s, tel.vo_rms,
                                     tel.vo_active_trigger.data());
  }
  const auto prev_stage = flow_.stage();
  auto act = flow_.update(dt, player_xz, fmv.active(), race_finished);
  if (audio && prev_stage == race::CampaignStage::kViperIntroDrive &&
      flow_.stage() != race::CampaignStage::kViperIntroDrive) {
    audio->radio_lock_viper_intro(false);
  }
  dispatch_action_media(act, fmv, audio);
  return act;
}

race::CampaignAction CampaignDirector::advance_or_skip(FmvPlayer& fmv, AudioHost* audio) {
  skip_hold_progress_ = 0.0f;
  if (ftc_.active()) {
    // Movies skip (confirmskip); in-engine cutscenes do not; a press on the
    // controls screen moves on.
    if (fmv.active()) {
      fmv.skip();
      if (audio) audio->set_fmv_audio(nullptr, nullptr);
    } else {
      ftc_confirm_ = true;
    }
    return {};
  }
  if (fmv.active()) {
    fmv.skip();
    if (audio) audio->set_fmv_audio(nullptr, nullptr);
  }
  if (audio) audio->trigger_ui(audio::UiCue::kAccept);
  const auto prev_stage = flow_.stage();
  auto act = flow_.advance_or_skip();
  if (audio && prev_stage == race::CampaignStage::kViperIntroDrive &&
      flow_.stage() != race::CampaignStage::kViperIntroDrive) {
    audio->radio_lock_viper_intro(false);
  }
  dispatch_action_media(act, fmv, audio);
  return act;
}

race::CampaignAction CampaignDirector::skip_onboarding_all(FmvPlayer& fmv, AudioHost* audio) {
  skip_hold_progress_ = 0.0f;
  if (fmv.active()) {
    fmv.skip();
    if (audio) audio->set_fmv_audio(nullptr, nullptr);
  }
  const auto prev_stage = flow_.stage();
  auto act = flow_.skip_onboarding_all();
  if (audio && prev_stage == race::CampaignStage::kViperIntroDrive &&
      flow_.stage() != race::CampaignStage::kViperIntroDrive) {
    audio->radio_lock_viper_intro(false);
  }
  dispatch_action_media(act, fmv, audio);
  return act;
}

race::CampaignAction CampaignDirector::on_enter_event(const race::CareerEvent& ev, FmvPlayer& fmv, AudioHost* audio) {
  auto act = flow_.on_enter_event(ev);
  dispatch_action_media(act, fmv, audio);
  return act;
}

race::CampaignAction CampaignDirector::on_race_finished(
    const race::CareerEvent* ev, const race::RaceAward& award, int place, int total_cars, int old_pop_rank,
    int new_pop_rank, int64_t balance_before, int64_t balance_after, int wb_points_after, int collected_wristband,
    const std::string& prize_car_name, const race::CareerCatalog* catalog, FmvPlayer& fmv, AudioHost* audio,
    const std::vector<race::RaceFinishStanding>& standings) {
  if (ftc_.active() && ev) {
    ftc_race_finished_ = true;
    ftc_race_event_ = ev->event_id;
    ftc_race_place_ = place;
  }
  auto act = flow_.on_race_finished(ev, award, place, total_cars, old_pop_rank, new_pop_rank, balance_before,
                                    balance_after, wb_points_after, collected_wristband, prize_car_name, catalog,
                                    standings);
  dispatch_action_media(act, fmv, audio);
  return act;
}

bool CampaignDirector::on_viper_convoy_pass(int passed_delta, AudioHost* audio) {
  const bool changed = flow_.on_viper_convoy_pass(passed_delta);
  if (changed) pump_audio(audio);
  return changed;
}

race::CampaignAction CampaignDirector::on_wristband_collected(int wristband_level, const std::string& bonus_car_name,
                                                              int newly_unlocked_events, FmvPlayer& fmv,
                                                              AudioHost* audio) {
  auto act = flow_.on_wristband_collected(wristband_level, bonus_car_name, newly_unlocked_events);
  if (audio) audio->trigger_ui(audio::UiCue::kWristbandUnlock);
  dispatch_action_media(act, fmv, audio);
  // race_central.xml award_wristband -> radiodj.festival_wristband.
  if (audio) audio->radio_wristband_awarded(wristband_level);
  return act;
}

race::CampaignAction CampaignDirector::on_barn_find(bool discovered, const std::string& barn_id,
                                                    const std::string& car_name, AudioHost* audio) {
  auto act = flow_.on_barn_find(discovered, barn_id, car_name);
  pump_audio(audio);
  return act;
}

race::CampaignAction CampaignDirector::on_barn_restored(const std::string& barn_id, const std::string& car_name,
                                                        AudioHost* audio) {
  auto act = flow_.on_barn_restored(barn_id, car_name);
  pump_audio(audio);
  return act;
}

void CampaignDirector::on_popularity_rank_changed(int old_rank, int new_rank, AudioHost* audio) {
  flow_.on_popularity_rank_changed(old_rank, new_rank);
  pump_audio(audio);
}

bool CampaignDirector::is_cinematic_camera_active() const {
  if (cutscenes_.active()) return true;
  const auto st = flow_.stage();
  if (st == race::CampaignStage::kQualifyRace && flow_.qualify_retry_pending()) return true;
  // Exclude kPreRaceGrid so the campaign flyby never bleeds into the live
  // race grid orbit or pre-race countdown camera swoop.
  return st == race::CampaignStage::kFestivalRegistration || st == race::CampaignStage::kWristbandCeremony ||
         st == race::CampaignStage::kShowcaseIntro || st == race::CampaignStage::kShowcaseOutro ||
         st == race::CampaignStage::kBossIntro || st == race::CampaignStage::kBossVictory ||
         st == race::CampaignStage::kBarnDiscovery || st == race::CampaignStage::kBarnRestored ||
         st == race::CampaignStage::kPostRaceFinish ||
         st == race::CampaignStage::kRewardTally || st == race::CampaignStage::kFinaleIntro ||
         st == race::CampaignStage::kFinaleVictory || st == race::CampaignStage::kCredits;
}

bool CampaignDirector::compute_camera(const std::array<double, 3>& car_pos, double car_heading,
                                      drive::CameraView& out_cam) const {
  if (cutscenes_.active()) return cutscenes_.camera(car_pos, car_heading, out_cam);
  if (!is_cinematic_camera_active()) return false;

  const auto& cin = flow_.cinematic();
  const double dur = std::max(2.0, cin.duration_s);
  const double raw_t = std::fmod(std::max(0.0, flow_.stage_time()), dur) / dur;
  const double t = race::ease_in_out_cubic(raw_t);

  std::array<double, 3> p0{}, p1{}, p2{}, p3{};
  std::array<double, 3> l0{}, l1{}, l2{}, l3{};
  double fov = 46.0;

  switch (flow_.stage()) {
    case race::CampaignStage::kWristbandCeremony:
    case race::CampaignStage::kFestivalRegistration:
      // Sweeping Festival Stage crane fly-in around the player's car.
      p0 = local_to_world(car_pos, car_heading, -6.2, 3.8, 7.2);
      p1 = local_to_world(car_pos, car_heading, -4.6, 2.2, 5.4);
      p2 = local_to_world(car_pos, car_heading, 2.8, 1.45, 5.1);
      p3 = local_to_world(car_pos, car_heading, 5.4, 1.85, 3.2);
      l0 = local_to_world(car_pos, car_heading, 0.0, 0.9, 0.5);
      l1 = local_to_world(car_pos, car_heading, 0.0, 0.75, 0.2);
      l2 = local_to_world(car_pos, car_heading, 0.0, 0.72, 0.0);
      l3 = local_to_world(car_pos, car_heading, 0.0, 0.80, -0.3);
      fov = 42.0;
      break;

    case race::CampaignStage::kBossIntro:
    case race::CampaignStage::kFinaleIntro:
    case race::CampaignStage::kFinaleVictory:
      // Dramatic low-angle Boss / Darius Flynt hero sweep with target offset so the
      // hero car sits cleanly on the right half of the frame, clear of the left dossier card.
      p0 = local_to_world(car_pos, car_heading, 5.2, 0.95, 5.8);
      p1 = local_to_world(car_pos, car_heading, 2.6, 0.85, 4.6);
      p2 = local_to_world(car_pos, car_heading, -2.0, 1.15, 4.4);
      p3 = local_to_world(car_pos, car_heading, -4.8, 1.50, 2.2);
      l0 = local_to_world(car_pos, car_heading, 1.2, 0.62, 0.6);
      l1 = local_to_world(car_pos, car_heading, 1.2, 0.68, 0.2);
      l2 = local_to_world(car_pos, car_heading, -1.2, 0.74, -0.2);
      l3 = local_to_world(car_pos, car_heading, -1.2, 0.82, -0.5);
      fov = 40.0;
      break;

    case race::CampaignStage::kShowcaseIntro:
    case race::CampaignStage::kShowcaseOutro:
      // Low-to-skyward Showcase reveal framing the car and horizon sky.
      p0 = local_to_world(car_pos, car_heading, -4.5, 1.0, 5.6);
      p1 = local_to_world(car_pos, car_heading, -2.5, 1.3, 4.6);
      p2 = local_to_world(car_pos, car_heading, 2.6, 2.1, 4.2);
      p3 = local_to_world(car_pos, car_heading, 5.6, 3.2, 1.8);
      l0 = local_to_world(car_pos, car_heading, 0.0, 0.75, 0.5);
      l1 = local_to_world(car_pos, car_heading, 0.0, 1.15, 0.3);
      l2 = local_to_world(car_pos, car_heading, 0.0, 1.35, 0.0);
      l3 = local_to_world(car_pos, car_heading, 0.0, 0.90, -0.2);
      fov = 48.0;
      break;

    case race::CampaignStage::kPreRaceGrid:
      // Low track-level grid flyby from rear flank to nose.
      p0 = local_to_world(car_pos, car_heading, 3.4, 1.05, -5.8);
      p1 = local_to_world(car_pos, car_heading, 3.2, 0.90, -1.2);
      p2 = local_to_world(car_pos, car_heading, 2.8, 1.00, 3.2);
      p3 = local_to_world(car_pos, car_heading, 1.6, 1.35, 6.2);
      l0 = local_to_world(car_pos, car_heading, 0.0, 0.65, 0.0);
      l1 = local_to_world(car_pos, car_heading, 0.0, 0.65, 0.4);
      l2 = local_to_world(car_pos, car_heading, 0.0, 0.68, 0.6);
      l3 = local_to_world(car_pos, car_heading, 0.0, 0.70, 0.2);
      fov = 44.0;
      break;

    case race::CampaignStage::kBarnDiscovery:
    case race::CampaignStage::kBarnRestored:
      // Slow cinematic push-in reveal.
      p0 = local_to_world(car_pos, car_heading, -5.8, 1.9, 7.6);
      p1 = local_to_world(car_pos, car_heading, -4.1, 1.5, 5.9);
      p2 = local_to_world(car_pos, car_heading, -2.8, 1.2, 4.7);
      p3 = local_to_world(car_pos, car_heading, -2.0, 1.05, 3.9);
      l0 = l1 = l2 = l3 = local_to_world(car_pos, car_heading, 0.0, 0.75, 0.2);
      fov = 42.0;
      break;

    case race::CampaignStage::kCredits: {
      // Wide golden-hour orbital sweep over Colorado.
      const double ang = car_heading + flow_.stage_time() * 0.16;
      const double r = 8.4;
      const std::array<double, 3> eye = {car_pos[0] + std::sin(ang) * r, car_pos[1] + 2.6 + 0.4 * std::sin(ang * 0.5),
                                         car_pos[2] + std::cos(ang) * r};
      const std::array<double, 3> look = {car_pos[0], car_pos[1] + 0.9, car_pos[2]};
      out_cam.eye = eye;
      out_cam.forward = normalize_vec({look[0] - eye[0], look[1] - eye[1], look[2] - eye[2]});
      out_cam.up = {0.0, 1.0, 0.0};
      out_cam.fov_degrees = 46.0;
      return true;
    }

    case race::CampaignStage::kPostRaceFinish:
      // Smooth heroic podium sweep framing the winning car on the right while Stage 1 RACE RESULTS displays on the left.
      p0 = local_to_world(car_pos, car_heading, -4.4, 1.45, 5.4);
      p1 = local_to_world(car_pos, car_heading, -2.0, 1.35, 5.8);
      p2 = local_to_world(car_pos, car_heading, 2.8, 1.40, 4.9);
      p3 = local_to_world(car_pos, car_heading, 5.2, 1.65, 2.2);
      l0 = local_to_world(car_pos, car_heading, -0.8, 0.72, 0.3);
      l1 = local_to_world(car_pos, car_heading, -0.8, 0.72, 0.2);
      l2 = local_to_world(car_pos, car_heading, -0.6, 0.74, 0.0);
      l3 = local_to_world(car_pos, car_heading, -0.6, 0.76, -0.2);
      fov = 42.0;
      break;

    default:
      // Post-race Reward Tally hero orbit keeping the player's car framed cleanly beside sequential reward cards.
      p0 = local_to_world(car_pos, car_heading, 5.0, 1.60, 4.8);
      p1 = local_to_world(car_pos, car_heading, 5.8, 1.50, 0.6);
      p2 = local_to_world(car_pos, car_heading, 3.8, 1.45, -4.6);
      p3 = local_to_world(car_pos, car_heading, -2.8, 1.70, -5.0);
      l0 = l1 = l2 = l3 = local_to_world(car_pos, car_heading, -0.5, 0.75, 0.0);
      fov = 43.0;
      break;
  }

  const auto eye = eval_cubic_bezier(p0, p1, p2, p3, t);
  const auto look = eval_cubic_bezier(l0, l1, l2, l3, t);
  out_cam.eye = eye;
  out_cam.forward = normalize_vec({look[0] - eye[0], look[1] - eye[1], look[2] - eye[2]});
  out_cam.up = {0.0, 1.0, 0.0};
  out_cam.fov_degrees = fov;
  return true;
}

}  // namespace carview

namespace carview {

bool CampaignDirector::is_blocking_gameplay() const {
  if (cutscenes_.active()) return true;
  if (ftc_.active()) {
    // The first-time career's scripted beats; the map and Autoshow stages
    // leave the host's screens free to open.
    switch (ftc_.stage()) {
      case race::FtcStage::kControls:
      case race::FtcStage::kOpeningCutscene:
      case race::FtcStage::kDinerCutscenes:
      case race::FtcStage::kFestivalArrive:
      case race::FtcStage::kFestivalArriveMovie:
      case race::FtcStage::kRadioUnlock:
      case race::FtcStage::kRaceCentralMovie:
      case race::FtcStage::kWristbandIntro:
      case race::FtcStage::kRewards: return true;
      default: return false;
    }
  }
  return flow_.is_blocking_gameplay();
}

bool CampaignDirector::cutscene_drives_player() const {
  // The opening only: the diner cutscenes and Festival_Race_Outro also give
  // the car to the AI, but their car is parked or out of shot and they keep
  // the paused sim (interim, Q66).
  return cutscenes_.active() && ftc_.active() && ftc_.stage() == race::FtcStage::kOpeningCutscene &&
         !ftc_.controls_enabled() && ftc_.ai_driving();
}

}  // namespace carview
