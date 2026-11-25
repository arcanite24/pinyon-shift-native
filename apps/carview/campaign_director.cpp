#include "campaign_director.h"

#include <algorithm>
#include <cmath>

#include "audio_host.h"

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

void CampaignDirector::load_from_save(const drive::DriveSave& save) {
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
    audio->radio_trigger_immediate("AliceOpeningInterview1");
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
  skip_hold_progress_ = 0.0f;
  const auto prev_stage = flow_.stage();
  auto act = flow_.start_new_career(skip_intro_movies_, festival_gate);
  if (audio && prev_stage == race::CampaignStage::kViperIntroDrive &&
      flow_.stage() != race::CampaignStage::kViperIntroDrive) {
    audio->radio_lock_viper_intro(false);
  }
  dispatch_action_media(act, fmv, audio);
  return act;
}

bool CampaignDirector::apply_cli_stage(std::string_view cli_stage, FmvPlayer& fmv, AudioHost* audio,
                                       race::CampaignAction& out_action) {
  skip_hold_progress_ = 0.0f;
  const auto prev_stage = flow_.stage();
  if (!flow_.jump_to_cli_stage(cli_stage, out_action)) return false;
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
    const std::string& prize_car_name, const race::CareerCatalog* catalog, FmvPlayer& fmv, AudioHost* audio) {
  auto act = flow_.on_race_finished(ev, award, place, total_cars, old_pop_rank, new_pop_rank, balance_before,
                                    balance_after, wb_points_after, collected_wristband, prize_car_name, catalog);
  dispatch_action_media(act, fmv, audio);
  return act;
}

race::CampaignAction CampaignDirector::on_wristband_collected(int wristband_level, const std::string& bonus_car_name,
                                                              int newly_unlocked_events, FmvPlayer& fmv,
                                                              AudioHost* audio) {
  auto act = flow_.on_wristband_collected(wristband_level, bonus_car_name, newly_unlocked_events);
  if (audio) audio->trigger_ui(audio::UiCue::kWristbandUnlock);
  dispatch_action_media(act, fmv, audio);
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
  const auto st = flow_.stage();
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
