#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "world/road_follow.h"

namespace carview {
namespace {

uint8_t collision_surface_hint(const world::Collision& col, int surf_idx) {
  const auto& surfs = col.surfaces();
  if (surf_idx < 0 || size_t(surf_idx) >= surfs.size()) return 0;
  const auto& sf = surfs[size_t(surf_idx)];
  const std::string name = lower_ascii(sf.name);
  const std::string cat = lower_ascii(sf.category);
  if (cat == "guardrail" || name.find("guardrail") != std::string::npos || name.find("metal") != std::string::npos ||
      name.find("steel") != std::string::npos || name.find("rail") != std::string::npos) {
    return 3;  // Metal/Guardrail
  }
  if (name.find("wood") != std::string::npos || name.find("timber") != std::string::npos ||
      name.find("plank") != std::string::npos || name.find("fence") != std::string::npos) {
    return 4;  // Wood/Prop
  }
  if (cat == "softworld" || sf.off_roadness >= 0.25f || name.find("dirt") != std::string::npos ||
      name.find("grass") != std::string::npos || name.find("gravel") != std::string::npos ||
      name.find("sand") != std::string::npos || name.find("mud") != std::string::npos ||
      name.find("snow") != std::string::npos) {
    return 2;  // Dirt/Grass
  }
  return 0;  // Asphalt/Concrete
}

uint8_t landing_surface_hint(const world::Collision& col, int surf_idx) {
  const auto& surfs = col.surfaces();
  if (surf_idx < 0 || size_t(surf_idx) >= surfs.size()) return 0;
  const auto& sf = surfs[size_t(surf_idx)];
  const std::string name = lower_ascii(sf.name);
  if (name.find("mud") != std::string::npos) return 3;
  if (name.find("grass") != std::string::npos) return 2;
  if (sf.off_roadness >= 0.2f || name.find("dirt") != std::string::npos || name.find("gravel") != std::string::npos ||
      name.find("sand") != std::string::npos || lower_ascii(sf.category) == "softworld") {
    return 1;
  }
  return 0;
}

int parse_barn_numeric_id(const std::string& barn_name) {
  int val = 0;
  bool any = false;
  for (char c : barn_name) {
    if (c >= '0' && c <= '9') {
      val = val * 10 + (c - '0');
      any = true;
    }
  }
  return any ? val : 0;
}

void sync_pending_barns_to_save(DriveApp& app) {
  app.saved.pending_barn_restorations.clear();
  for (const auto& br : app.pending_barn_restorations) {
    drive::PendingBarnRestoration p{};
    p.barn_id = parse_barn_numeric_id(br.barn_id);
    p.car_id = int(br.car_id);
    p.remaining_sec = float(std::max(0.0, br.ready_after_s - br.elapsed_s));
    app.saved.pending_barn_restorations.push_back(p);
  }
}

void restore_pending_barns_from_save(DriveApp& app, const drive::DriveSave& s) {
  app.pending_barn_restorations.clear();
  for (const auto& p : s.pending_barn_restorations) {
    DriveApp::PendingBarnRestore br;
    br.car_id = p.car_id;
    br.ready_after_s = std::max(0.0, double(p.remaining_sec));
    br.elapsed_s = 0.0;
    for (const auto& b : app.collectibles.barns) {
      if ((p.car_id > 0 && b.car_id == p.car_id) ||
          (p.barn_id > 0 && parse_barn_numeric_id(b.name) == p.barn_id)) {
        br.barn_id = b.name;
        br.car_name = barn_car_label(b);
        if (!b.car_name_key.empty()) {
          const uint32_t ref = uint32_t(fh1::string_hash("Barnfind")) << 16 | fh1::string_hash(b.car_name_key);
          const std::string t = carview::game_text("_&" + std::to_string(ref));
          if (!t.empty() && t[0] != '_') br.car_name = t;
        }
        break;
      }
    }
    if (br.barn_id.empty()) br.barn_id = "barnfind_" + std::to_string(p.barn_id);
    if (br.car_name.empty()) {
      if (auto it = app.baked_ids.find(p.car_id); it != app.baked_ids.end()) {
        br.car_name = carview::car_name(it->second);
      } else {
        br.car_name = "BARN FIND";
      }
    }
    app.pending_barn_restorations.push_back(std::move(br));
  }
}

}  // namespace

void prime_world_and_snap_vehicle(DriveSession& s, std::array<double, 3> target_pos, double target_heading);

void DriveSession::apply_story_action(const StoryAction& sa) {
            if (sa.play_fmv) {
              const auto mp = carview::FmvPlayer::resolve_movie(app.game_root_of(), sa.fmv_id);
              if (!mp.empty() && app.fmv_player.open(mp, app.language)) {
                app.audio_host.set_fmv_audio(
                    [](void* ctx, float* out, int fr) { return static_cast<carview::FmvPlayer*>(ctx)->read_audio(out, fr); },
                    &app.fmv_player);
              }
            }
            if (sa.enter_title_screen && !app.no_title) app.show_title = true;
            if (sa.swap_car && !sa.car_media.empty() && sa.car_media != app.car) {
              if (std::find(app.baked_cars.begin(), app.baked_cars.end(), sa.car_media) != app.baked_cars.end()) {
                if (sa.teleport) {
                  app.spawn[0] = sa.pos[0];
                  app.spawn[1] = sa.pos[1];
                  app.spawn[2] = sa.pos[2];
                  app.spawn[3] = sa.heading;
                  app.have_spawn = true;
                  app.fast_travel = true;
                }
                app.next_car = sa.car_media;
                running = false;
                return;
              }
            }
            if (sa.teleport) {
              prime_world_and_snap_vehicle(*this, sa.pos, sa.heading);
            }
            if (sa.set_gps_target && app.gps_solver.ready()) {
              const drive::Vec3 at = vehicle.position();
              fire_satnav_event(app.gps_nav.set_destination(sa.gps_xz[0], sa.gps_xz[1], traffic::GpsDestKind::kFestival,
                                                        "HORIZON FESTIVAL MAIN GATE", at.x, at.z));
            }
            if (sa.launch_qualify_race) {
              app.next_event = std::to_string(sa.qualify_route_id);
              app.next_event_fallback.clear();
              running = false;
            }
            if (sa.grant_starter) {
              app.award_garage_car(race::kStarterCarId, race::kStarterCarColour, true);
              if (app.career.credits < 15000) app.career.credits = 15000;
            }
            if (!sa.radio_immediate_event.empty()) app.audio_host.radio_trigger_immediate(sa.radio_immediate_event);
            if (!sa.vo_trigger.empty()) app.audio_host.trigger_vo(sa.vo_trigger, false);
            if (sa.open_autoshow) open_shop_or_autoshow(true);
            if (sa.intro_finished) app.saved.intro_completed = true;
}

std::string DriveSession::find_baked_car(const std::string& media, int64_t car_id) {
            if (car_id > 0) {
              if (auto it = app.baked_ids.find(car_id); it != app.baked_ids.end()) return it->second;
            }
            const std::string want = lower_ascii(media);
            for (const auto& bc : app.baked_cars) {
              if (lower_ascii(bc) == want) return bc;
            }
            return {};
}

void DriveSession::apply_campaign_action(const race::CampaignAction& ca) {
            if (ca.grant_starter) {
              app.award_garage_car(race::kStarterCarId, race::kStarterCarColour, true);
              if (app.career.credits < 15000) app.career.credits = 15000;
            }
            if (ca.collect_yellow_wristband && app.collected_wristband < 0) app.collected_wristband = 0;
            if (ca.intro_completed) app.saved.intro_completed = true;
            if (ca.save_requested) save_due = true;

            // Check whether a car swap requires restarting the session loop.
            std::string swap_target;
            if (ca.swap_car && !ca.car_media.empty()) {
              swap_target = find_baked_car(ca.car_media, ca.car_id);
            }
            const bool needs_swap = !swap_target.empty() && lower_ascii(swap_target) != lower_ascii(app.car);
            if (needs_swap) {
              // Persist teleport, GPS, qualify-race launch, Autoshow, and Viper radio lock across the car swap!
              app.pending_handoff = DriveApp::PendingCampaignHandoff{};
              app.pending_handoff.active = true;
              if (ca.teleport) {
                app.pending_handoff.has_teleport = true;
                app.pending_handoff.pos = ca.pos;
                app.pending_handoff.heading = ca.heading;
                app.spawn[0] = ca.pos[0];
                app.spawn[1] = ca.pos[1];
                app.spawn[2] = ca.pos[2];
                app.spawn[3] = ca.heading;
                app.have_spawn = true;
                app.fast_travel = true;
              }
              if (ca.set_gps) {
                app.pending_handoff.has_gps = true;
                app.pending_handoff.gps_label = ca.gps_label;
                app.pending_handoff.gps_xz = ca.gps_xz;
              }
              if (ca.launch_event && (!ca.event_id.empty() || ca.route_id > 0)) {
                app.pending_handoff.has_event = true;
                app.pending_handoff.event_id = ca.event_id;
                app.pending_handoff.route_id = ca.route_id;
              }
              app.pending_handoff.open_autoshow = ca.open_autoshow;
              app.pending_handoff.lock_viper_radio = ca.lock_viper_radio;
              app.campaign.save_to_save(app.saved);
              app.next_car = swap_target;
              running = false;
              return;
            }

            if (ca.teleport) {
              prime_world_and_snap_vehicle(*this, ca.pos, ca.heading);
            }
            if (ca.set_gps && app.gps_solver.ready()) {
              const drive::Vec3 at = vehicle.position();
              fire_satnav_event(app.gps_nav.set_destination(ca.gps_xz[0], ca.gps_xz[1], traffic::GpsDestKind::kFestival,
                                                        ca.gps_label, at.x, at.z));
            }
            if (ca.launch_event && (!ca.event_id.empty() || ca.route_id > 0)) {
              app.next_event = !ca.event_id.empty() ? ca.event_id : std::to_string(ca.route_id);
              app.next_event_fallback =
                  (!ca.event_id.empty() && ca.route_id > 0) ? std::to_string(ca.route_id) : std::string{};
              running = false;
            }
            if (ca.lock_viper_radio) {
              if (!app.audio_host.radio_select_station_by_name("Bass Arena")) {
                app.audio_host.radio_select_station_by_name("Bass");
              }
              app.audio_host.radio_trigger_immediate("AliceOpeningInterview1");
            }
            if (ca.open_autoshow) app.pending_autoshow_open = true;
            if (ca.enter_free_roam && app.race) {
              app.campaign_owns_post_race = false;
              results_page = 0;
              if (!app.race->post_race_spawn(app.spawn)) {
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
              }
              app.have_spawn = true;
              app.fast_travel = true;
              app.leave_race = true;
              running = false;
            }
}

void DriveSession::run_window_loop() {
        {
          int pad_count = 0;
          if (SDL_JoystickID* pids = SDL_GetGamepads(&pad_count)) {
            for (int pi = 0; pi < pad_count; ++pi) {
              if (SDL_Gamepad* pad = SDL_OpenGamepad(pids[pi])) pads.push_back(pad);
            }
            SDL_free(pids);
          }
        }
        slice12.set_window(app.window);
        if (!app.save_path.empty() && slice12.trainer.clean_user_dir().empty()) {
          slice12.trainer.configure_save_root(
              app.save_path.parent_path(), app.slice12_modded_save || app.mods_session.isolation().isolated);
        }
        if (app.pending_barn_restorations.empty() && app.have_saved && !app.saved.pending_barn_restorations.empty()) {
          restore_pending_barns_from_save(app, app.saved);
        }
        slice12.sync_pad_capabilities(pads);
        slice12.sync_wheel_devices();
        if (app.window) {
          SDL_ShowWindow(app.window);
          SDL_RaiseWindow(app.window);
          SDL_PumpEvents();
        }
        last_tick_ns = SDL_GetTicksNS();
  title_at = 0;
        accumulator = 0;
        was_sim_paused = false;
        drawn_before = vehicle.pose();
        running = true;
        std::printf("controls: W/Up throttle, S/Down brake (held at a stop: reverse), A/D or Left/Right steer, "
                    "Space handbrake, R put back on the road, V chase/front view, C (held) look back, H hide/show "
                    "the HUD, F12 save the pose (carview-pose-N.txt), Esc pause menu.\n"
                    "gamepad: right trigger throttle, left trigger brake, left stick steer, A handbrake, right stick "
                    "look around, Y (held) look back, Start pause menu, Back view.\n"
                    "car picker: [ and ] (LB and RB) choose among the %zu baked cars, Enter (X) drives it.\n",
                    app.baked_cars.size());
  sync_pick();
  app.sync_picker = [this]() { sync_pick(); };
  if (!app.record_session_dir.empty() && !app.playtest_recorder.active()) {
    std::string rec_err;
    if (app.playtest_recorder.start(app.record_session_dir, &rec_err)) {
      std::printf("playtest: recording session to %s (F11 or Back+RB for bug capture)\n",
                  app.playtest_recorder.session_dir().string().c_str());
    } else {
      std::fprintf(stderr, "carview: --record-session failed: %s\n", rec_err.c_str());
    }
  }
  if (app.playtest_recorder.active()) host.renderer.set_gpu_timing(true);
  bool playtest_bug_chord_prev = false;
  bool playtest_bug_capture_waiting = false;
  int playtest_bug_capture_retries = 0;
  PlaytestFrameSample playtest_bug_sample{};
  double air_time_s = 0.0;
  double prev_vel_y = 0.0;
  double min_air_vel_y = 0.0;
  while (running) {
    poll_sdl_events();
    if (!running) break;
    if (app_lifecycle.backgrounded || window_minimized || window_occluded) {
      SDL_WaitEventTimeout(nullptr, app_lifecycle.backgrounded ? 50 : 16);
      last_tick_ns = SDL_GetTicksNS();
      continue;
    }
    drive::Input in;
    double rig_look = 0.0;
    sample_driving_input(in, rig_look);
          const uint64_t now = SDL_GetTicksNS();
          const double raw_frame_ms = (now >= last_tick_ns) ? double(now - last_tick_ns) * 1e-6 : 0.0;
          const double frame_dt = std::min(0.25, double(now - last_tick_ns) * 1e-9);
          title_orbit_time += frame_dt;
          if (!handoff_applied && app.pending_handoff.active && !app.headless) {
            handoff_applied = true;
            const DriveApp::PendingCampaignHandoff h = app.pending_handoff;
            app.pending_handoff = {};
            if (h.has_teleport) {
              prime_world_and_snap_vehicle(*this, h.pos, h.heading);
            }
            if (h.has_gps && app.gps_solver.ready()) {
              const drive::Vec3 at = vehicle.position();
              fire_satnav_event(app.gps_nav.set_destination(h.gps_xz[0], h.gps_xz[1], traffic::GpsDestKind::kFestival,
                                                        h.gps_label, at.x, at.z));
            }
            if (h.lock_viper_radio) {
              if (!app.audio_host.radio_select_station_by_name("Bass Arena")) {
                app.audio_host.radio_select_station_by_name("Bass");
              }
              app.audio_host.radio_trigger_immediate("AliceOpeningInterview1");
            }
            if (h.open_autoshow) app.pending_autoshow_open = true;
            if (h.has_event && (!h.event_id.empty() || h.route_id > 0)) {
              app.next_event = !h.event_id.empty() ? h.event_id : std::to_string(h.route_id);
              app.next_event_fallback =
                  (!h.event_id.empty() && h.route_id > 0) ? std::to_string(h.route_id) : std::string{};
              running = false;
              break;
            }
          }
          if (!queued_campaign_actions.empty()) {
            auto batch = std::move(queued_campaign_actions);
            queued_campaign_actions.clear();
            for (const auto& ca : batch) apply_campaign_action(ca);
          }
          if (app.campaign_new_career_pending && !app.headless) {
            app.campaign_new_career_pending = false;
            app.show_title = false;
            app.story_intro.reset();
            host.renderer.reset_auto_exposure();
            std::array<double, 3> gate = {-1360.0, 2.0, -210.0};
            if (const auto* rc = app.have_career_catalog ? app.career_catalog.find_site(race::FestivalSite::kRaceCentral) : nullptr)
              gate = rc->pos;
            apply_campaign_action(app.campaign.start_new_career(app.fmv_player, &app.audio_host, gate));
          }
          if (!app.boot_intro_checked && !app.headless) {
            app.boot_intro_checked = true;
            if (!app.cli_campaign_stage.empty()) {
              app.show_title = false;
              app.story_intro.reset();
              race::CampaignAction ca;
              if (app.campaign.apply_cli_stage(app.cli_campaign_stage, app.fmv_player, &app.audio_host, ca))
                apply_campaign_action(ca);
            } else if (!app.cli_fmv_path.empty()) {
              if (app.fmv_player.open(app.cli_fmv_path, app.language)) {
                app.audio_host.set_fmv_audio(
                    [](void* ctx, float* out, int fr) { return static_cast<carview::FmvPlayer*>(ctx)->read_audio(out, fr); },
                    &app.fmv_player);
              }
            } else if (app.story_intro_cli) {
              app.show_title = false;
              app.story_intro.reset();
              std::array<double, 3> gate = {-1360.0, 2.0, -210.0};
              if (const auto* rc = app.have_career_catalog ? app.career_catalog.find_site(race::FestivalSite::kRaceCentral) : nullptr)
                gate = rc->pos;
              apply_campaign_action(app.campaign.start_new_career(app.fmv_player, &app.audio_host, gate));
            } else if (!app.skip_intro_movies && app.show_title) {
              // StoryIntro strictly owns pre-Title boot-splash FMV playback ONLY;
              // CampaignDirector owns 100% of Minute-0 -> Endgame career onboarding.
              app.show_title = false;
              apply_story_action(app.story_intro.begin_boot(app.game_root_of(), app.skip_intro_movies, app.start_in_single_player,
                                                        false));
            }
          }
          if (app.fmv_player.active()) {
            if (!app.fmv_player.update(frame_dt, true)) {
              app.audio_host.set_fmv_audio(nullptr, nullptr);
              host.renderer.reset_auto_exposure();
              if (title_attract_playing) {
                title_attract_playing = false;
                title_idle_s = 0.0;
                if (!app.no_title) app.show_title = true;
              }
            }
          }
          // Authentic PressStart.wmv seamless looping animated title screen backdrop when present & decodable;
          // otherwise falls back seamlessly to the live 3D Colorado Festival scenic overlook.
          // After 53 s idle on Press Start (retail attract timer), launch Forza_Tone.wmv / FMV_01.wmv.
          if (app.show_title && !app.fmv_player.active() && !app.headless) {
            if (!app.title_video_tried) {
              app.title_video_tried = true;
              const auto mp = carview::FmvPlayer::resolve_movie(app.game_root_of(), carview::FmvMovieId::kPressStart,
                                                                app.state_root);
              if (!mp.empty()) app.title_video.open(mp, app.language, 1280, 720, nullptr, true);
            }
            if (app.title_video.active()) {
              app.title_video.update(frame_dt, false);
            }
            if (!app.title_menu_phase && !settings_page.is_open()) {
              title_idle_s += frame_dt;
              if (title_idle_s >= 53.0) {
                title_idle_s = 0.0;
                auto ap = carview::FmvPlayer::resolve_movie(app.game_root_of(), carview::FmvMovieId::kForzaTone,
                                                            app.state_root);
                if (ap.empty()) {
                  ap = carview::FmvPlayer::resolve_movie(app.game_root_of(), carview::FmvMovieId::kFmv01Opening,
                                                         app.state_root);
                }
                if (!ap.empty() && app.fmv_player.open(ap, app.language, 1280, 720)) {
                  if (app.title_video.active()) app.title_video.close();
                  app.title_video_tried = false;
                  title_attract_playing = true;
                  app.show_title = false;
                  app.audio_host.set_fmv_audio(
                      [](void* ctx, float* out, int fr) {
                        return static_cast<carview::FmvPlayer*>(ctx)->read_audio(out, fr);
                      },
                      &app.fmv_player);
                }
              }
            } else {
              title_idle_s = 0.0;
            }
          } else if (app.title_video.active()) {
            app.title_video.close();
          }
          if (pending_title_resume_gather) {
            if (host.streamer) {
              std::string rerr;
              bool changed = false;
              const bool ok = host.streamer->update(app.saved_resume_spawn[0], app.saved_resume_spawn[2], &changed, &rerr);
              if (!ok || changed || !host.streamer->gathering()) {
                if (ok && changed && host.streamer->prepared().has_value()) {
                  host.ground = &host.streamer->ground();
                  host.follower.set_ground(&host.streamer->ground());
                  host.set_staged_world();
                }
                world::Spawn rsp;
                const char* rhow = nullptr;
                if (world::choose_spawn(*host.streamer, app.saved_resume_spawn, nullptr, nullptr, rsp, &rhow)) {
                  vehicle.place({rsp.position[0], rsp.position[1] + 0.03, rsp.position[2]}, rsp.heading);
                  const drive::Vec3 vp = vehicle.position();
                  const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                  drive::Hit wh;
                  if (host.streamer->ground().raycast({vp.x, vp.y + 0.8, vp.z}, {0, 1, 0}, 3.5, wh) ||
                      host.streamer->ground().raycast({vp.x, vp.y + 0.7, vp.z}, vf, 2.5, wh)) {
                    world::recover(*host.streamer, vehicle);
                  }
                }
                drawn_before = vehicle.pose();
                pending_title_resume_gather = false;
              }
            } else {
              pending_title_resume_gather = false;
            }
          }
          if (loading_screen.active()) {
            loading_screen.update(frame_dt);
            const double elapsed = loading_screen.elapsed();
            const double min_hold = std::max(0.25, loading_min_until);
            const bool waiting_world = pending_title_resume_gather || host.showroom.has_pending_world();
            const float prog = waiting_world ? float(std::clamp(elapsed / (min_hold + 2.0), 0.10, 0.92))
                                             : float(std::clamp(elapsed / min_hold, 0.05, 1.0));
            loading_screen.set_progress(prog);
            if (!waiting_world && elapsed >= min_hold) {
              loading_screen.stop();
            }
          }
          if (app.story_intro.stage() == carview::StoryStage::kBootSplash) {
            const drive::Vec3 vp = vehicle.position();
            const bool rf = app.race && app.race->player_finished();
            apply_story_action(app.story_intro.update(frame_dt, {vp.x, vp.z}, app.fmv_player.active(), rf));
          }
          if (!app.show_title && !app.headless) {
            const drive::Vec3 vp = vehicle.position();
            const bool rf = app.race && app.race->player_finished();
            const bool* kb_skip = SDL_GetKeyboardState(nullptr);
            bool hold_skip = kb_skip && (kb_skip[SDL_SCANCODE_SPACE] || kb_skip[SDL_SCANCODE_RETURN]);
            for (SDL_Gamepad* pad : pads)
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) hold_skip = true;
            apply_campaign_action(app.campaign.update(frame_dt, {vp.x, vp.z}, app.fmv_player, &app.audio_host, rf, hold_skip));
            // Deferred event launch: once a Boss/Showcase/Finale/Pre-Race intro finishes or is skipped, enter the race!
            if (!pending_event_launch.empty() && !app.campaign.is_blocking_gameplay() && running) {
              app.next_event = pending_event_launch;
              pending_event_launch.clear();
              running = false;
            }
            // Deduplicated post-race handoff: once CampaignDirector's RewardTally/Victory/Credits finishes, exit race cleanly.
            if (app.campaign_owns_post_race && app.race && app.race->player_finished() &&
                app.campaign.flow().stage() == race::CampaignStage::kFreeRoam && running) {
              app.campaign_owns_post_race = false;
              results_page = 0;
              if (!app.race->post_race_spawn(app.spawn)) {
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
              }
              app.have_spawn = true;
              app.leave_race = true;
              running = false;
            }
            // Tutorial 2-car Autoshow handoff once parts_db is ready in Free Roam.
            if (app.pending_autoshow_open && !app.campaign.is_blocking_gameplay() && !autoshow.is_open() && !app.race &&
                app.parts_db.valid() && app.parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
              app.pending_autoshow_open = false;
              open_shop_or_autoshow(true);
            }
            // Barn find restoration completion timer -> triggers Dak Stewart's on_barn_restored cinematic!
            if (!app.pending_barn_restorations.empty() && !app.race && !app.campaign.is_blocking_gameplay() && !menu.open &&
                !map_open && !autoshow.is_open() && !shop.is_open() && !garage_open && !events_page.is_open()) {
              app.pending_barn_restorations.front().elapsed_s += frame_dt;
              if (app.pending_barn_restorations.front().elapsed_s >= app.pending_barn_restorations.front().ready_after_s) {
                const auto br = app.pending_barn_restorations.front();
                app.pending_barn_restorations.erase(app.pending_barn_restorations.begin());
                sync_pending_barns_to_save(app);
                save_due = true;
                if (br.car_id > 0) {
                  app.award_garage_car(br.car_id, 0, false);
                  app.garage_picker();
                  save_now("barn restored");
                }
                popup = {"BARN FIND RESTORED!", br.car_name + " DELIVERED TO YOUR GARAGE", vehicle.time(), -1,
                         carview::ui::rgba(74, 238, 97)};
                app.audio_host.trigger_ui(audio::UiCue::kCarAdded);
                apply_campaign_action(app.campaign.on_barn_restored(br.barn_id, br.car_name, &app.audio_host));
              }
            }
          }
          {
            const drive::Vec3 vp = vehicle.position();
            const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            app.audio_host.update_world_acoustics(float(vp.x), float(vp.y), float(vp.z), std::atan2(float(vf.x), float(vf.z)));
            app.audio_host.radio_set_time_of_day(app.tod_clock.hour());
          }
          const bool trainer_freecam_active = slice12.trainer.effective_free_camera();
          const bool menu_paused = app.show_title || app.fmv_player.active() || loading_screen.active() || ceremony.active ||
                                   app.campaign.is_blocking_gameplay() || grid_menu_open || menu.open ||
                                   map_open || events_page.is_open() || outpost_hub_open || garage_open ||
                                   shop.is_open() || autoshow.is_open() || career_page.is_open() ||
                                   settings_page.is_open() || paint_open || club_open || car_club_open || app.rally_hub_open ||
                                   app.rally_service_open || app.rally_results_open || photo_mode.active() ||
                                   replay_viewer.active() || replay_viewer.browser_open() || achievements.screen_open() ||
                                   mods_page.is_open() || save_page.is_open() || slice12.trainer_overlay.is_open() ||
                                   trainer_freecam_active;
          const bool sim_paused = menu_paused || rewind_buf.active();
          // Poll queued F10 Trainer GPS & Teleport requests (last/saved map road).
          {
            const double now_wall_s = double(now) * 1e-9;
            if (const auto gps_pt = slice12.trainer.consume_gps(app.race != nullptr);
                gps_pt.has_value() && app.gps_solver.ready()) {
              const drive::Vec3 at = vehicle.position();
              fire_satnav_event(app.gps_nav.set_destination((*gps_pt)[0], (*gps_pt)[2], traffic::GpsDestKind::kGeneric,
                                                        "TRAINER ROAD", at.x, at.z));
              notice = "GPS ROUTED TO TRAINER ROAD";
              notice_until = vehicle.time() + 3.0;
            }
            if (const auto tp_pt = slice12.trainer.consume_teleport(now_wall_s, true, app.race != nullptr);
                tp_pt.has_value()) {
              app.spawn[0] = (*tp_pt)[0];
              app.spawn[1] = (*tp_pt)[1];
              app.spawn[2] = (*tp_pt)[2];
              app.spawn[3] = 0.0;
              app.have_spawn = true;
              app.fast_travel = true;
              app.next_car = app.car;
              running = false;
            }
          }
          achievements.update(frame_dt);
          save_page.update(frame_dt);
          if (!app.save_session.notices.empty()) {
            notice = app.save_session.notices.front().first;
            notice_until = vehicle.time() + app.save_session.notices.front().second;
            app.save_session.notices.erase(app.save_session.notices.begin());
          }
          // Native mods' per-frame hook (shift_mod_api.h).
          if (app.mods_session.plugins().alive()) {
                        const drive::Vec3 mp = vehicle.position();
            const drive::Vec3 mf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            ShiftModFrame frame{};
            frame.size = sizeof frame;
            frame.frame = mod_frame_counter++;
            frame.time = vehicle.time();
            frame.dt = frame_dt;
            frame.position[0] = float(mp.x), frame.position[1] = float(mp.y), frame.position[2] = float(mp.z);
            frame.heading = std::atan2(float(mf.x), float(mf.z));
            frame.speed = float(vehicle.speed());
            frame.rpm = float(vehicle.rpm());
            frame.gear = vehicle.gear();
            frame.paused = menu_paused ? 1 : 0;
            app.mods_session.tick(frame);
            for (const auto& [text, seconds] : app.mods_session.notices) notice = text, notice_until = vehicle.time() + seconds;
            app.mods_session.notices.clear();
          }
          if (sim_paused != was_sim_paused) {
            rig_accel.reset();
            air_time_s = 0.0;
            min_air_vel_y = 0.0;
            prev_vel_y = vehicle.velocity().y;
            was_sim_paused = sim_paused;
          }
          if (!menu_paused) rewind_buf.update(frame_dt);
          if (rewind_buf.consume_view_replay_request()) {
            drive::RewindCameraState cs;
            uint64_t snap_id = 0u;
            if (rewind_buf.cancel(vehicle, &cs, nullptr, nullptr, nullptr, nullptr, &snap_id)) {
              restore_world_rewind(snap_id, false);
              if (have_rig && cs.rig.has_value()) rig = *cs.rig;
              if (cs.has_view) last_cam_view = cs.view, have_cam_view = true;
              app.front = cs.front_view;
              rig_accel.reset();
            }
            open_replay_viewer(false);
          }
          if (replay_viewer.active() && !photo_mode.active()) {
            replay_viewer.update(frame_dt);
          }
          if (photo_mode.active()) {
            carview::PhotoCameraInput pcin{};
            const bool* kb = SDL_GetKeyboardState(nullptr);
            if (kb[SDL_SCANCODE_W]) pcin.move_forward += 1.0f;
            if (kb[SDL_SCANCODE_S]) pcin.move_forward -= 1.0f;
            if (kb[SDL_SCANCODE_D]) pcin.move_right += 1.0f;
            if (kb[SDL_SCANCODE_A]) pcin.move_right -= 1.0f;
            if (kb[SDL_SCANCODE_E]) pcin.move_up += 1.0f;
            if (kb[SDL_SCANCODE_Q]) pcin.move_up -= 1.0f;
            if (kb[SDL_SCANCODE_L]) pcin.look_yaw += 1.0f;
            if (kb[SDL_SCANCODE_J]) pcin.look_yaw -= 1.0f;
            if (kb[SDL_SCANCODE_I]) pcin.look_pitch += 1.0f;
            if (kb[SDL_SCANCODE_K]) pcin.look_pitch -= 1.0f;
            if (kb[SDL_SCANCODE_Z]) pcin.zoom += 1.0f;
            if (kb[SDL_SCANCODE_X]) pcin.zoom -= 1.0f;
            if (kb[SDL_SCANCODE_COMMA]) pcin.roll -= 1.0f;
            if (kb[SDL_SCANCODE_PERIOD]) pcin.roll += 1.0f;
            for (SDL_Gamepad* pad : pads) {
              pcin.move_right += float(drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0, 0.16, 0.95));
              pcin.move_forward -= float(drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0, 0.16, 0.95));
              pcin.look_yaw += float(drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0, 0.16, 0.95));
              pcin.look_pitch -= float(drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0, 0.16, 0.95));
              pcin.move_up += float(std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0) -
                                    std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0));
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) pcin.zoom += 1.0f;
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) pcin.zoom -= 1.0f;
            }
            auto gb = [&](double x, double y, double z, double& gy) {
              drive::Hit hit;
              if (!host.ground || !host.ground->raycast({x, y, z}, {0, -1, 0}, 80.0, hit)) return false;
              gy = y - hit.distance;
              return true;
            };
            auto wr = [&](const std::array<double, 3>& orig, const std::array<double, 3>& dir, double max_d,
                          double& hit_d) {
              drive::Hit hit;
              if (!host.ground ||
                  !host.ground->raycast({orig[0], orig[1], orig[2]}, {dir[0], dir[1], dir[2]}, max_d, hit)) {
                return false;
              }
              hit_d = hit.distance;
              return true;
            };
            photo_mode.step(frame_dt, pcin, gb, wr);
          }
          // Trainer Free Camera fly controls (active when F10 Trainer > World > Free Camera is ON).
                    if (trainer_freecam_active && !slice12.trainer_overlay.is_open() && !photo_mode.active()) {
            if (!trainer_freecam_was_active) {
              if (have_cam_view) {
                slice12.trainer.free_cam.reset_from_pose(last_cam_view.eye, last_cam_view.forward);
              } else {
                const drive::Vec3 vp = vehicle.position();
                const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                slice12.trainer.free_cam.reset_from_pose({vp.x - vf.x * 5.0, vp.y + 2.5, vp.z - vf.z * 5.0},
                                                         {vf.x, 0.0, vf.z});
              }
            }
            const bool* kb = SDL_GetKeyboardState(nullptr);
            double mf = 0, mr = 0, mu = 0, ty = 0, tp = 0;
            if (kb[SDL_SCANCODE_W] || kb[SDL_SCANCODE_UP]) mf += 1.0;
            if (kb[SDL_SCANCODE_S] || kb[SDL_SCANCODE_DOWN]) mf -= 1.0;
            if (kb[SDL_SCANCODE_D] || kb[SDL_SCANCODE_RIGHT]) mr += 1.0;
            if (kb[SDL_SCANCODE_A] || kb[SDL_SCANCODE_LEFT]) mr -= 1.0;
            if (kb[SDL_SCANCODE_E] || kb[SDL_SCANCODE_SPACE]) mu += 1.0;
            if (kb[SDL_SCANCODE_Q] || kb[SDL_SCANCODE_LCTRL]) mu -= 1.0;
            if (kb[SDL_SCANCODE_L]) ty += 1.0;
            if (kb[SDL_SCANCODE_J]) ty -= 1.0;
            if (kb[SDL_SCANCODE_I]) tp += 1.0;
            if (kb[SDL_SCANCODE_K]) tp -= 1.0;
            bool fast_fly = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
            for (SDL_Gamepad* pad : pads) {
              mr += drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0, 0.16, 0.95);
              mf -= drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0, 0.16, 0.95);
              ty += drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0, 0.16, 0.95);
              tp -= drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0, 0.16, 0.95);
              mu += std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0) -
                    std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0);
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) fast_fly = true;
            }
            slice12.trainer.free_cam.step(frame_dt, mf, mr, mu, ty, tp, fast_fly);
          }
          trainer_freecam_was_active = trainer_freecam_active;
          // Paused: the car holds still (no steps, no time to catch up).
          const double game_spd = slice12.trainer.effective_game_speed();
          tod_frame_dt = frame_dt * game_spd;
          {
            const double fixed_h = slice12.trainer.effective_fixed_hour();
            if (fixed_h >= 0.0) {
              app.tod_clock.set_hour(fixed_h);
              if (traffic) traffic->set_hour(app.tod_clock.hour());
            } else if (!sim_paused) {
              if (app.tod_system.loaded_tod && !app.race) {
                const double spd = app.tod_system.active_preset(app.tod_clock).scalar("TimeSpeed", float(app.tod_clock.tod_sec / 60.0), 28.0f);
                const double prev_scale = app.tod_clock.speed_scale;
                if (slice12.trainer.config.cheats_enabled && slice12.trainer.config.day_length_minutes > 0.0) {
                  app.tod_clock.speed_scale = slice12.trainer.effective_day_length_scale();
                }
                app.tod_clock.advance(frame_dt * game_spd, spd);
                app.tod_clock.speed_scale = prev_scale;
                if (traffic) traffic->set_hour(app.tod_clock.hour());
              }
            }
          }
          if (!sim_paused) {
            accumulator += frame_dt * game_spd;
          }
          last_tick_ns = now;
          // P0-G1: Hot-reload DriveSave + CareerGarage from disk after SaveProfilePage restore/import/profile switch
          // BEFORE any autosave check so restored/imported/switched saves are never overwritten by stale memory.
          {
            std::filesystem::path reload_dir;
            SaveProfilePage::ReloadReason reload_reason = SaveProfilePage::ReloadReason::kNone;
            if (save_page.consume_reload_from_disk(&reload_dir, &reload_reason)) {
              if (!reload_dir.empty()) {
                app.save_path = reload_dir / "save.txt";
                app.garage_path = reload_dir / "garage.txt";
                achievements_path = reload_dir / "achievements.txt";
              }
              drive::DriveSave disk_save;
              std::string rerr;
              const bool loaded_save = !app.save_path.empty() && drive::read_save(app.save_path, disk_save, &rerr);
              if (loaded_save) {
                app.saved = disk_save;
                app.have_saved = true;
                app.career = race::CareerRecord{};
                app.career.credits = disk_save.credits;
                app.career.total_winnings = disk_save.total_winnings;
                app.career.wristband_points = disk_save.wristband_points;
                app.career.num_races = disk_save.num_races;
                app.career.num_victories = disk_save.num_victories;
                app.career.num_podiums = disk_save.num_podiums;
                app.career.time_in_festival = disk_save.time_festival;
                app.career.time_in_street = disk_save.time_street;
                app.career.time_in_exhibition = disk_save.time_exhibition;
                app.career.top_speed = disk_save.top_speed;
                for (const auto& [id, place] : disk_save.race_results) app.career.race_places[id] = place;
                for (const auto& [id, place] : disk_save.event_results) app.career.event_places[id] = place;
                app.career.prize_cars = disk_save.prize_cars;
                app.career.driver_xp = disk_save.driver_xp;
                app.career.driver_level = app.have_prog_tables
                                              ? app.prog_tables.driver_level_for_xp(app.career.driver_xp)
                                              : std::max(1, disk_save.level);
                for (const auto& [mid, xp] : disk_save.make_affinity) app.career.make_affinity_xp[mid] = xp;
                if (disk_save.difficulty >= 0 && disk_save.difficulty <= 4)
                  app.race_options.difficulty = race::Difficulty(disk_save.difficulty);
                if (disk_save.ai_mode >= 0 && disk_save.ai_mode <= 2)
                  app.race_options.ai_mode = race::AiMode(disk_save.ai_mode);
                app.dynamic_ai_skill = disk_save.ai_skill;
                app.h2h_races = disk_save.h2h_races;
                app.h2h_wins = disk_save.h2h_wins;
                app.collected_wristband =
                    disk_save.collected_wristband >= 0
                        ? disk_save.collected_wristband
                        : (app.have_career_catalog
                               ? std::max(0, app.career_catalog.earned_wristband(app.career.wristband_points))
                               : std::max(0, app.wristbands.level_index(app.career.wristband_points)));
                app.max_skills_car = disk_save.max_skills_car;
                app.treasure_map_setting = disk_save.treasure_map;
                app.campaign.load_from_save(disk_save);
                if (disk_save.tod_sec >= 0.0 && disk_save.tod_sec < 86400.0) app.tod_clock.tod_sec = disk_save.tod_sec;
                app.outpost_progress.discovered_outposts.clear();
                app.outpost_progress.discovered_outposts.insert(disk_save.discovered_outposts.begin(),
                                                                disk_save.discovered_outposts.end());
                app.outpost_progress.completed_pr_stunts.clear();
                app.outpost_progress.completed_pr_stunts.insert(disk_save.completed_pr_stunts.begin(),
                                                                disk_save.completed_pr_stunts.end());
                if (app.road_discovery.ready() && !disk_save.road_discovery_words.empty()) {
                  app.road_discovery.load_words(disk_save.road_discovery_nodes, disk_save.road_discovery_words);
                }
                if (app.speed_tracker) {
                  app.speed_tracker->load_records(disk_save.speed_traps, disk_save.speed_zones);
                }
                if (app.have_skills_data) {
                  app.skill_tracker = std::make_unique<freeroam::SkillTracker>(
                      app.skills_data, disk_save.popularity_points, disk_save.fame_rewards_claimed,
                      disk_save.max_skills_score, app.max_skills_car);
                  for (const auto& sp : disk_save.sponsors)
                    app.skill_tracker->set_sponsor_progress(sp.id, sp.count, sp.claimed);
                }
                if (!app.collectibles.signs.empty()) {
                  app.progress = freeroam::Progress{};
                  progress_from_save(disk_save, app.collectibles, app.progress);
                  app.progress_loaded = true;
                }
                promo_tracker.load(disk_save.promo_cars, std::max(240, int(app.baked_cars.size())));
                if (disk_save.has_controls) live_controls = disk_save.controls;
                restore_pending_barns_from_save(app, disk_save);
                club_check();
              } else {
                app.saved = drive::DriveSave{};
                app.have_saved = false;
                app.career = race::CareerRecord{};
                app.pending_barn_restorations.clear();
                app.h2h_races = 0;
                app.h2h_wins = 0;
                club_check();
              }
              garage::Garage disk_garage;
              std::string gerr;
              if (!app.garage_path.empty() && garage::read_garage(app.garage_path, disk_garage, &gerr)) {
                app.career_garage = std::move(disk_garage);
                app.have_garage = true;
                app.garage_picker();
              } else if (!loaded_save) {
                app.career_garage = garage::Garage{};
                app.have_garage = false;
              }
              if (app.demo_achievements_png.empty()) achievements.load_state(achievements_path);
              save_due = false;
              next_save = vehicle.time() + 30.0;
              if (!reload_dir.empty()) {
                slice12.trainer.configure_save_root(
                    reload_dir, app.slice12_modded_save || app.mods_session.isolation().isolated);
              }
              const char* reason_msg = "SAVE RELOADED FROM DISK";
              if (reload_reason == SaveProfilePage::ReloadReason::kBackupRestored)
                reason_msg = "BACKUP RESTORED & RELOADED";
              else if (reload_reason == SaveProfilePage::ReloadReason::kProfileSwitched)
                reason_msg = "PROFILE SWITCHED & RELOADED";
              else if (reload_reason == SaveProfilePage::ReloadReason::kSaveImported)
                reason_msg = "SAVE IMPORTED & RELOADED";
              notice = reason_msg;
              notice_until = vehicle.time() + 4.0;
              std::printf("save: %s (%s)\n", reason_msg, app.save_path.string().c_str());
            }
          }
          // P0-G3: Live F6/F10 modded-save isolation before any save write occurs.
          slice12.trainer.sync_modded_isolation();
          if (slice12.trainer.consume_modded_isolation_triggered()) {
            const auto& dir = slice12.trainer.active_user_dir();
            if (!dir.empty()) {
              app.save_path = dir / "save.txt";
              app.garage_path = dir / "garage.txt";
              achievements_path = dir / "achievements.txt";
            }
            app.slice12_modded_save = true;
            ach_isolated = true;
            slice12.save_tree_label = "user-modded";
            notice = "CHEATS ACTIVE — SAVE ISOLATED TO USER-MODDED";
            notice_until = vehicle.time() + 4.0;
            std::printf("trainer: modded isolation active -> %s\n", app.save_path.string().c_str());
          }
          if (vehicle.time() >= next_save) {
            sync_pending_barns_to_save(app);
            save_now("timer");
            next_save = vehicle.time() + 30;
          }
          if (save_due && !map_open) {  // a collectible or award: micro-save
            save_due = false;
            sync_pending_barns_to_save(app);
            save_now("collectible");
            eval_achievements(true);
          }
          if (!sim_paused && vehicle.time() >= next_achievement_eval) {
            eval_achievements(true);
            next_achievement_eval = vehicle.time() + 1.0;
          }
          double frame_world_impact = 0.0;
          double frame_car_impact = 0.0;
          double frame_object_impact = 0.0;
          step_object_impact_accum = 0.0;
          step_car_impact_accum = 0.0;
          while (!sim_paused && accumulator >= drive::Vehicle::kStep) {
            drawn_before = vehicle.pose();
            drive::Input step_in = app.cruise ? host.follower.input(vehicle) : in;
            if (!app.cruise) {
              step_in.shift = pending_shift;
              pending_shift = 0;
            }
            if (!step(step_in)) {
              running = false;
              app.status = 1;
              std::fprintf(stderr, "carview: %s\n", app.error.c_str());
              break;
            }
            if (vehicle.hand_of_god_triggered()) {
              vehicle.clear_hand_of_god();
              put_back();
              air_time_s = 0.0;
              min_air_vel_y = 0.0;
              prev_vel_y = 0.0;
            }
            // P0-A4: Detect jump landings (>0.25 s airborne -> re-ground) and fire Car_Landings.fev.
            {
              const auto& ws_land = vehicle.wheels();
              int grounded = 0;
              int land_surf = -1;
              for (int wi = 0; wi < drive::kWheels; ++wi) {
                if (ws_land[size_t(wi)].contact) {
                  ++grounded;
                  if (land_surf < 0) land_surf = ws_land[size_t(wi)].surface;
                }
              }
              const double vy = vehicle.velocity().y;
              if (grounded == 0) {
                air_time_s += drive::Vehicle::kStep;
                min_air_vel_y = std::min(min_air_vel_y, vy);
              } else {
                if (air_time_s > 0.25) {
                  const double impact_vy = std::max({0.0, -prev_vel_y, -min_air_vel_y});
                  if (impact_vy >= 1.2 || air_time_s >= 0.35) {
                    const drive::Vec3 vp = vehicle.position();
                    audio::LandingHit lhit{};
                    lhit.impact_speed_mps = float(impact_vy);
                    lhit.air_time_s = float(air_time_s);
                    lhit.pos = {float(vp.x), float(vp.y), float(vp.z)};
                    lhit.surface_hint = landing_surface_hint(host.collision, land_surf);
                    app.audio_host.trigger_landing(lhit);
                  }
                }
                air_time_s = 0.0;
                min_air_vel_y = 0.0;
              }
              prev_vel_y = vy;
            }
            {
              drive::RewindCameraState cs;
              if (have_rig) cs.rig = rig;
              cs.view = last_cam_view;
              cs.has_view = have_cam_view;
              cs.front_view = app.front;
              const double rc = app.race ? app.race->session().clock() : vehicle.time();
              const bool cap_ok = !app.race || (app.race->session().clock() >= 0.0 && !app.race->player_finished());
              const auto sh = app.skill_tracker ? app.skill_tracker->hud() : freeroam::SkillHudState{};
              const double sk_timer = app.skill_tracker ? app.skill_tracker->chain_remaining_s(vehicle.time()) : 0.0;
              const uint64_t captured_id = rewind_buf.record_step(
                  vehicle.time(), rc, vehicle, cs, cap_ok, uint64_t(std::max(0, sh.chain_points)),
                  std::max(1, sh.multiplier), sk_timer);
              if (captured_id != 0u) capture_world_rewind(captured_id);
              // 60 Hz gameplay replay ring-buffer recording (player + opponents + nearby traffic).
              if (replay_rec.active()) {
                uint8_t pl_place = 0;
                if (app.race) {
                  pl_place = uint8_t(std::clamp(app.race->player_place(), 0, 255));
                  const int cur_lap = app.race->session().lap(app.race->player_index());
                  if (cur_lap > replay_last_lap) {
                    replay_last_lap = cur_lap;
                    replay_rec.add_marker(vehicle.time(), "LAP " + std::to_string(cur_lap));
                  }
                  if (app.race->player_finished() && !replay_finish_marked) {
                    replay_finish_marked = true;
                    replay_rec.add_marker(vehicle.time(), "FINISH");
                  }
                }
                // ReplayRecorder samples at 60 Hz: skip assembling opponent/traffic sample vectors
                // on intermediate 120 Hz steps that maybe_record() would immediately discard.
                thread_local double s_last_replay_sample_t = -1e9;
                thread_local std::vector<carview::RaceView::OpponentTag> s_replay_otags;
                const double vtime = vehicle.time();
                if (replay_rec.frame_count() == 0 || vtime < s_last_replay_sample_t ||
                    vtime - s_last_replay_sample_t >= (1.0 / 60.0) - 1e-4) {
                  std::vector<drive::ReplayCarSample> rcars;
                  rcars.reserve(24);
                  rcars.push_back(drive::ReplayCarSample::from_vehicle(
                      0u, vehicle, player_lamps, in.throttle, in.brake, in.handbrake ? 1.0 : 0.0, in.steer, pl_place));
                  if (app.race) {
                    app.race->opponent_tags(s_replay_otags);
                    uint32_t slot = 1;
                    for (const auto& ot : s_replay_otags) {
                      drive::ReplayCarSample os{};
                      os.slot = slot++;
                      os.pos[0] = float(ot.x), os.pos[1] = float(ot.y), os.pos[2] = float(ot.z);
                      os.vel[0] = float(ot.vx), os.vel[1] = float(ot.vy), os.vel[2] = float(ot.vz);
                      os.speed_mps = float(ot.speed);
                      os.rpm = float(ot.rpm);
                      const double hdg = std::atan2(ot.vx, ot.vz);
                      os.quat[0] = float(std::cos(0.5 * hdg));
                      os.quat[2] = float(std::sin(0.5 * hdg));
                      rcars.push_back(os);
                    }
                  }
                  if (traffic && traffic->cars()) {
                    const drive::Vec3 pat = vehicle.position();
                    uint32_t tslot = 100;
                    for (const auto& tc : *traffic->cars()) {
                      const double dx = tc.position.x - pat.x, dz = tc.position.z - pat.z;
                      if (dx * dx + dz * dz > 160.0 * 160.0) continue;
                      drive::ReplayCarSample ts{};
                      ts.slot = tslot++;
                      ts.pos[0] = float(tc.position.x), ts.pos[1] = float(tc.position.y), ts.pos[2] = float(tc.position.z);
                      ts.speed_mps = float(tc.speed);
                      ts.quat[0] = float(std::cos(0.5 * tc.heading));
                      ts.quat[2] = float(std::sin(0.5 * tc.heading));
                      rcars.push_back(ts);
                      if (tslot >= 116) break;
                    }
                  }
                  if (replay_rec.maybe_record(vtime, std::move(rcars))) {
                    s_last_replay_sample_t = vtime;
                  }
                }
              }
            }
            frame_world_impact = std::max(frame_world_impact, vehicle.body_contact_force());
            const double traffic_imp = traffic ? traffic->player_impulse() : 0.0;
            const double race_imp = app.race ? app.race->player_impulse() : 0.0;
            frame_car_impact = std::max({frame_car_impact, traffic_imp, race_imp, step_car_impact_accum});
            frame_object_impact = std::max(frame_object_impact, step_object_impact_accum);
            if (frame_car_impact > 1.0) {
              const double dv_imp = frame_car_impact / std::max(500.0, vehicle.params().mass);
              cam_car_coll_long = std::max(cam_car_coll_long, dv_imp * 0.85);
              cam_car_coll_lat = std::max(cam_car_coll_lat, dv_imp * 0.55);
            }
            {
              const double mass = std::max(100.0, vehicle.params().mass);
              const auto& bc = vehicle.strongest_body_contact();
              const double dv_world = bc.force * drive::Vehicle::kStep / mass;
              const double dv_race = race_imp / mass;
              const double dv_traffic = traffic_imp / mass;
              if (dv_world > 1.0) {
                audio::CollisionHit hit{};
                hit.rel_speed_mps = float(dv_world);
                hit.pos = {float(bc.point.x), float(bc.point.y), float(bc.point.z)};
                hit.surface_hint = collision_surface_hint(host.collision, bc.surface);
                app.audio_host.trigger_collision(hit);
                const drive::Vec3 rel = bc.point - vehicle.position();
                const drive::Quat q = vehicle.orientation();
                const drive::Quat qinv{-q.x, -q.y, -q.z, q.w};
                const drive::Vec3 loc = drive::rotate(qinv, rel);
                car_damage.record_impact(float(loc.x), float(loc.y), float(loc.z), float(dv_world));
              }
              if (dv_race > 1.0 && app.race) {
                const std::array<float, 3> rloc = app.race->player_impulse_local_pos();
                const drive::Vec3 wpos =
                    vehicle.position() + drive::rotate(vehicle.orientation(), {double(rloc[0]), double(rloc[1]), double(rloc[2])});
                audio::CollisionHit hit{};
                hit.rel_speed_mps = float(dv_race);
                hit.pos = {float(wpos.x), float(wpos.y), float(wpos.z)};
                hit.is_car_on_car = true;
                hit.surface_hint = 1;
                app.audio_host.trigger_collision(hit);
                car_damage.record_impact(rloc[0], rloc[1], rloc[2], float(dv_race));
              } else if (dv_traffic > 1.0) {
                const drive::Vec3 vp = vehicle.position();
                const drive::Quat q = vehicle.orientation();
                const drive::Quat qinv{-q.x, -q.y, -q.z, q.w};
                std::array<float, 3> tloc = {0.0f, 0.5f, 1.5f};
                drive::Vec3 wpos = vp;
                if (traffic && traffic->cars()) {
                  double best_d2 = 100.0;
                  const double half_w = std::max(0.5, vehicle.params().size.x * 0.5);
                  const double half_l = std::max(1.2, vehicle.params().size.z * 0.5);
                  for (const auto& tc : *traffic->cars()) {
                    const drive::Vec3 rel{tc.position.x - vp.x, tc.position.y - vp.y, tc.position.z - vp.z};
                    const double d2 = rel.x * rel.x + rel.y * rel.y + rel.z * rel.z;
                    if (d2 < best_d2 && d2 > 1e-6) {
                      best_d2 = d2;
                      const drive::Vec3 loc = drive::rotate(qinv, rel);
                      tloc = {float(std::clamp(loc.x, -half_w, half_w)),
                              float(std::clamp(loc.y + 0.4, 0.1, 1.2)),
                              float(std::clamp(loc.z, -half_l, half_l))};
                      wpos = vp + drive::rotate(q, {double(tloc[0]), double(tloc[1]), double(tloc[2])});
                    }
                  }
                }
                audio::CollisionHit hit{};
                hit.rel_speed_mps = float(dv_traffic);
                hit.pos = {float(wpos.x), float(wpos.y), float(wpos.z)};
                hit.is_car_on_car = true;
                hit.surface_hint = 1;
                app.audio_host.trigger_collision(hit);
                car_damage.record_impact(tloc[0], tloc[1], tloc[2], float(dv_traffic));
              }
              const auto& ws = vehicle.wheels();
              const auto& col_surfs = host.collision.surfaces();
              bool offroad = false;
              for (int wi = 0; wi < drive::kWheels; ++wi) {
                if (!ws[size_t(wi)].contact) continue;
                const int si = ws[size_t(wi)].surface;
                if (si >= 0 && size_t(si) < col_surfs.size()) {
                  if (col_surfs[size_t(si)].off_roadness >= 0.5f) {
                    offroad = true;
                    break;
                  }
                } else if (si >= 2) {
                  offroad = true;
                  break;
                }
              }
              const std::array<double, 4> slips = {ws[0].slip_ratio, ws[1].slip_ratio, ws[2].slip_ratio, ws[3].slip_ratio};
              const double rpm_n = vehicle.rpm() / std::max(1000.0, vehicle.params().redline_rpm);
              if (car_damage.mode == drive::DamageMode::kSimulation) {
                car_damage.trans_damage = std::max(car_damage.trans_damage, float(vehicle.transmission_damage()));
              }
              car_damage.step(drive::Vehicle::kStep, vehicle.speed(), offroad, step_in.throttle, rpm_n, slips);
              const drive::DamageMultipliers dm = car_damage.multipliers();
              drive::Vehicle::DamageMods vdm;
              vdm.engine_torque = dm.engine_torque;
              vdm.shift_time_add = dm.shift_time_add;
              vdm.brake_power = dm.brake_power;
              vdm.aero_drag = dm.aero_drag;
              vdm.aero_downforce = dm.aero_downforce;
              vdm.tyre_grip = dm.tyre_grip;
              vdm.suspension_scale = dm.suspension_scale;
              vdm.steer_pull_rad = dm.steer_pull_rad;
              vehicle.set_damage_mods(vdm);
              host.showroom.set_damage_state(car_damage);
            }
            accumulator -= drive::Vehicle::kStep;
          }
          double rumble_low_f = 0.0, rumble_high_f = 0.0;
          if (!pads.empty() && gamepad_rumble.config().loaded) {
            drive::RumbleInput ri;
            const auto& vp = vehicle.params();
            const double rpm_span = std::max(1.0, vp.redline_rpm - vp.idle_rpm);
            ri.engine_norm_rpm = std::clamp((vehicle.rpm() - vp.idle_rpm) / rpm_span, 0.0, 1.0);
            ri.transmission_rpm = std::clamp(std::fabs(vehicle.clutch_output_rpm()) / std::max(1.0, vp.redline_rpm), 0.0, 1.5);
            const auto& ws = vehicle.wheels();
            double fl_load = 0, fl_slip = 0, rl_load = 0, rl_slip = 0;
            for (int wi = 0; wi < 4; ++wi) {
              const double load = std::max(0.0, ws[size_t(wi)].load);
              const double slip = std::hypot(ws[size_t(wi)].slip_ratio, ws[size_t(wi)].slip_angle);
              if (wi < 2) fl_load += load, fl_slip += load * slip;
              else rl_load += load, rl_slip += load * slip;
            }
            ri.tire_traction_front = fl_load > 1e-3 ? fl_slip / fl_load : 0.0;
            ri.tire_traction_rear = rl_load > 1e-3 ? rl_slip / rl_load : 0.0;
            ri.tire_traction = (fl_load + rl_load) > 1e-3 ? (fl_slip + rl_slip) / (fl_load + rl_load) : 0.0;
            ri.air_friction = std::clamp((vehicle.speed() * vehicle.speed()) / (80.0 * 80.0), 0.0, 1.0);
            // Surface bumpiness: use exact 2 * Bumpiness.Rumble from Vehicle
            // when available, else fallback to off-road/rough surface estimate
            // (0 on smooth asphalt so highway cruising never buzzes constantly).
            double bump_2x = vehicle.surface_bump_rumble();
            if (bump_2x <= 0.0) {
              double max_bump = 0.0;
              const auto& surfs = host.collision.surfaces();
              for (int wi = 0; wi < 4; ++wi) {
                if (!ws[size_t(wi)].contact) continue;
                const int si = ws[size_t(wi)].surface;
                double bump = 0.0;
                if (si >= 0 && size_t(si) < surfs.size()) {
                  const auto& sf = surfs[size_t(si)];
                  if (sf.off_roadness > 0.0f) bump = 0.04 + 0.36 * sf.off_roadness;
                  else if (sf.friction_scale < 0.95f) bump = 0.14;
                }
                max_bump = std::max(max_bump, bump);
              }
              bump_2x = 2.0 * max_bump;
            }
            const double abs_speed = std::fabs(vehicle.speed());
            ri.speed_mps = abs_speed;
            ri.smooth = true;
            ri.surface_bumpiness = bump_2x * (abs_speed / 22.352);
            ri.car_to_car_impulse = frame_car_impact / 100.0;
            // Convert body contact force (N) to step impulse (N*s) before mapping
            // onto ControllerRumble.xml's CarToWorldCollision impulse curve.
            ri.car_to_world_impulse = (frame_world_impact * drive::Vehicle::kStep) / 35.0;
            ri.car_to_object_impulse = frame_object_impact;
            ri.on_shift = !sim_paused && (vehicle.gear() != prev_rumble_gear);
            prev_rumble_gear = vehicle.gear();
            ri.on_engine_sputter = (vehicle.rpm() >= vp.redline_rpm * 0.985 && vehicle.engine_throttle() > 0.5);
            const uint32_t cur_bf_rumble = app.audio_host.telemetry().backfire_count;
            ri.on_backfire = !sim_paused && (cur_bf_rumble > prev_rumble_backfire);
            prev_rumble_backfire = cur_bf_rumble;
            ri.paused = sim_paused;
            ri.enabled = rumble_enabled;
            ri.scale = rumble_scale;
            const auto ro = gamepad_rumble.update(frame_dt, ri);
            // Xbox One / Series Impulse Triggers: LT = front brake lock / ABS pulse,
            // RT = rear traction loss / wheelspin (zero when stationary/paused).
            uint16_t lt_trig = 0, rt_trig = 0;
            if (!sim_paused && rumble_enabled && rumble_scale > 0.0 && abs_speed > 0.5) {
              const double front_lock = std::max({0.0, -ws[0].slip_ratio - 0.7, -ws[1].slip_ratio - 0.7});
              const double lt_f = std::clamp(
                  ((in.brake > 0.05 ? front_lock * 0.75 : 0.0) + (vehicle.abs_active() ? 0.55 : 0.0)) * rumble_scale,
                  0.0, 1.0);
              const double rt_f = std::clamp((ri.tire_traction_rear - 0.85) * 0.55 * rumble_scale, 0.0, 1.0);
              lt_trig = uint16_t(lt_f * 65535.0);
              rt_trig = uint16_t(rt_f * 65535.0);
            }
            for (SDL_Gamepad* pad : pads) {
              SDL_RumbleGamepad(pad, ro.left_u16(), ro.right_u16(), 100);
              SDL_RumbleGamepadTriggers(pad, lt_trig, rt_trig, 100);
            }
            rumble_low_f = ro.left_u16() / 65535.0;
            rumble_high_f = ro.right_u16() / 65535.0;
          } else if (!pads.empty()) {
            for (SDL_Gamepad* pad : pads) {
              SDL_RumbleGamepad(pad, 0, 0, 0);
              SDL_RumbleGamepadTriggers(pad, 0, 0, 0);
            }
          }
          slice12.tick(vehicle, rig, pads, !sim_paused && !app.show_title, frame_dt, now,
                       uint32_t(now / 1000000ULL), rumble_low_f, rumble_high_f);
          if (now - title_at > 100000000) {  // the speedometer, ten times a second
            title_at = now;
            char title[320], gear[8];
            if (vehicle.gear() < 0) std::snprintf(gear, sizeof gear, "R");
            else if (vehicle.gear() == 0) std::snprintf(gear, sizeof gear, "N");
            else std::snprintf(gear, sizeof gear, "%d", vehicle.gear());
            {
              const drive::Vec3 at = vehicle.position();
              const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
              std::snprintf(title, sizeof title,
                            "carview drive - %s - %3.0f km/h  gear %s  %5.0f rpm%s   at %.0f %.0f %.0f heading %.0f",
                            app.car.c_str(), std::fabs(vehicle.speed()) * 3.6, gear, vehicle.rpm(),
                            vehicle.abs_active() ? "  ABS" : "", at.x, at.y, at.z, std::atan2(f.x, f.z) * 57.29578);
            }
            if (picking && pick < app.baked_cars.size()) {
              const size_t n = std::strlen(title);
              std::snprintf(title + n, sizeof title - n, "   |   pick %zu/%zu: %s (Enter or X to drive it)", pick + 1,
                            app.baked_cars.size(), app.baked_cars[pick].c_str());
            }
            SDL_SetWindowTitle(app.window, title);
          }
    PlaytestFrameSample playtest_sample{};
    bool playtest_have_sample = false;
    if (app.playtest_recorder.active()) {
      const drive::Vec3 vp = vehicle.position();
      const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
      playtest_sample.t = app.playtest_recorder.elapsed_seconds();
      playtest_sample.frame_ms = raw_frame_ms;
      playtest_sample.cpu_ms = host.last_frame_cpu_ms;
      playtest_sample.gpu_ms = host.renderer.gpu_frame_ms();
      playtest_sample.x = vp.x;
      playtest_sample.y = vp.y;
      playtest_sample.z = vp.z;
      playtest_sample.heading = std::atan2(vf.x, vf.z);
      playtest_sample.speed_kmh = std::fabs(vehicle.speed()) * 3.6;
      playtest_sample.gear = vehicle.gear();
      playtest_sample.rpm = vehicle.rpm();
      playtest_sample.car = app.car;
      playtest_sample.tod_hour = app.tod_clock.hour();
      playtest_sample.chunks_loaded =
          host.streamer
              ? uint32_t(host.streamer->stats().world_models + host.streamer->stats().far_models)
              : host.showroom.gpu_resident_draws();
      playtest_sample.paused = sim_paused;
      playtest_sample.in_race = app.race != nullptr;
      playtest_sample.audio_stalls = app.audio_host.telemetry().stalls;
      playtest_sample.camera_mode =
          have_rig ? drive::camera_mode_name(rig.mode()) : (app.front ? "front" : "chase");
      playtest_sample.has_road_y = false;
      if (host.ground && std::isfinite(vp.x) && std::isfinite(vp.y) && std::isfinite(vp.z)) {
        drive::Hit ghit{};
        if (host.ground->raycast({vp.x, vp.y + 2.2, vp.z}, {0.0, -1.0, 0.0}, 100.0, ghit)) {
          playtest_sample.road_y = (vp.y + 2.2) - ghit.distance;
          playtest_sample.has_road_y = true;
        } else if (host.ground->raycast({vp.x, vp.y + 80.0, vp.z}, {0.0, -1.0, 0.0}, 160.0, ghit)) {
          playtest_sample.road_y = (vp.y + 80.0) - ghit.distance;
          playtest_sample.has_road_y = true;
        }
      }
      playtest_have_sample = true;

      bool bug_chord_now = false;
      if (const bool* kb = SDL_GetKeyboardState(nullptr); kb && kb[SDL_SCANCODE_F11]) bug_chord_now = true;
      if (!bug_chord_now) {
        for (SDL_Gamepad* pad : pads) {
          if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
              SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
            bug_chord_now = true;
            break;
          }
        }
      }
      if (bug_chord_now && !playtest_bug_chord_prev && !playtest_bug_capture_waiting &&
          app.playtest_recorder.can_trigger_bug_mark(playtest_sample.t)) {
        playtest_bug_sample = playtest_sample;
        playtest_bug_capture_waiting = true;
        playtest_bug_capture_retries = 0;
      }
      playtest_bug_chord_prev = bug_chord_now;

      if (playtest_bug_capture_waiting && pending_shot == PendingShot::kNone && !host.renderer.capture_pending()) {
        host.renderer.request_capture(true);
      }
    }
    if (!render_window_frame(now, frame_dt, menu_paused, trainer_freecam_active)) break;
    if (playtest_have_sample) {
      playtest_sample.cpu_ms = host.last_frame_cpu_ms;
      playtest_sample.gpu_ms = host.renderer.gpu_frame_ms();
      app.playtest_recorder.record_frame(playtest_sample);
      if (playtest_bug_capture_waiting && pending_shot == PendingShot::kNone) {
        std::vector<uint8_t> cap_rgba;
        uint32_t cap_w = 0, cap_h = 0;
        if (host.renderer.take_captured_frame(cap_rgba, cap_w, cap_h)) {
          playtest_bug_sample.cpu_ms = playtest_sample.cpu_ms;
          playtest_bug_sample.gpu_ms = playtest_sample.gpu_ms;
          const std::string banner =
              app.playtest_recorder.save_bug_bookmark(playtest_bug_sample, std::move(cap_rgba), cap_w, cap_h);
          if (!banner.empty()) {
            notice = banner;
            notice_until = vehicle.time() + 2.0;
            std::printf("playtest: %s\n", banner.c_str());
          }
          playtest_bug_capture_waiting = false;
        } else if (++playtest_bug_capture_retries >= 5) {
          const std::string banner = app.playtest_recorder.save_bug_bookmark(playtest_bug_sample, {}, 0, 0);
          if (!banner.empty()) {
            notice = banner;
            notice_until = vehicle.time() + 2.0;
            std::printf("playtest: %s\n", banner.c_str());
          }
          playtest_bug_capture_waiting = false;
        }
      }
    }
  }
        if (app.playtest_recorder.active()) {
          const bool will_reload =
              (app.status == 0 && (app.rally_world_switch != 0 || app.leave_race || !app.next_event.empty() ||
                                   !app.next_car.empty() || hot_reload_car || app.again));
          if (will_reload) {
            app.playtest_recorder.flush();
          } else {
            app.playtest_recorder.finish();
            const auto& sum = app.playtest_recorder.last_summary();
            std::printf("playtest: finished (%llu frames, p50=%.2f ms, p95=%.2f ms, p99=%.2f ms, max=%.2f ms, "
                        "hitches=%llu, ground_anomalies=%llu, bugs=%zu) -> %s\n",
                        static_cast<unsigned long long>(sum.total_frames), sum.p50_ms, sum.p95_ms, sum.p99_ms,
                        sum.max_ms, static_cast<unsigned long long>(sum.hitch_count),
                        static_cast<unsigned long long>(sum.ground_anomaly_count), sum.bugs.size(),
                        (app.playtest_recorder.session_dir() / "summary.json").string().c_str());
          }
        }
        app.sync_picker = nullptr;
        slice12.shutdown(pads);
        for (SDL_Gamepad* pad : pads) {
          SDL_RumbleGamepad(pad, 0, 0, 0);
          SDL_RumbleGamepadTriggers(pad, 0, 0, 0);
          SDL_CloseGamepad(pad);
        }
        app.audio_host.stop_device();
}

}  // namespace carview
