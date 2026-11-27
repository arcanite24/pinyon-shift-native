#include "drive_session.h"
#include "cutscene_runtime.h"
#include "hud_screen.h"
#include "focus_audio_state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace carview {

bool DriveSession::render_window_frame(uint64_t now, double frame_dt, bool menu_paused,
                                       bool trainer_freecam_active) {
          // Ensure player vehicle starts cleanly snapped onto the road surface on race grid
          // before the first camera/frame render (matches RaceView::load_cars AI ground snap).
          if (app.race && host.ground && vehicle.time() <= 1e-6 && !app.race->session().started()) {
            const drive::Vec3 vp = vehicle.position();
            if (std::isfinite(vp.x) && std::isfinite(vp.y) && std::isfinite(vp.z)) {
              drive::Hit ghit{};
              double road_y = vp.y;
              bool hit_ok = false;
              if (host.ground->raycast({vp.x, vp.y + 5.0, vp.z}, {0.0, -1.0, 0.0}, 50.0, ghit)) {
                road_y = (vp.y + 5.0) - ghit.distance;
                hit_ok = true;
              } else if (host.ground->raycast({vp.x, vp.y + 60.0, vp.z}, {0.0, -1.0, 0.0}, 120.0, ghit)) {
                road_y = (vp.y + 60.0) - ghit.distance;
                hit_ok = true;
              }
              if (hit_ok && std::fabs(vp.y - (road_y + 0.05)) > 0.35) {
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                vehicle.reset({vp.x, road_y + 0.05, vp.z}, std::atan2(f.x, f.z));
                drawn_before = vehicle.pose();
              }
            }
          }
          drive::CameraView rv_cam{};
          bool have_rv_cam = false;
          if (replay_viewer.active() && !photo_mode.active()) {
            float oy = 0.0f, op = 0.0f, oz = 0.0f;
            const bool* kb = SDL_GetKeyboardState(nullptr);
            if (kb[SDL_SCANCODE_A]) oy -= 1.0f;
            if (kb[SDL_SCANCODE_D]) oy += 1.0f;
            if (kb[SDL_SCANCODE_W]) op += 1.0f;
            if (kb[SDL_SCANCODE_S]) op -= 1.0f;
            if (kb[SDL_SCANCODE_Q]) oz -= 1.0f;
            if (kb[SDL_SCANCODE_E]) oz += 1.0f;
            auto gb = [&](double x, double y, double z, double& gy) {
              drive::Hit hit;
              if (!host.ground || !host.ground->raycast({x, y, z}, {0, -1, 0}, 80.0, hit)) return false;
              gy = y - hit.distance;
              return true;
            };
            rv_cam = replay_viewer.compute_camera(frame_dt, oy, op, oz, gb);
            have_rv_cam = true;
          }
          const audio::View cur_audio_view = audio_view();
          const auto& rw_audio = rewind_buf.audio();
          const bool rewind_tape_cue = rewind_buf.consume_tape_start_cue();
          if (rewind_tape_cue && rewind_buf.ui_state() == drive::RewindUiState::kLoopRewind) {
            // Initial Rewind entry triggers kRewindTapeStart via AudioHost::update_vehicle's
            // snapshot transition; Rewind Further loops fire the cue explicitly here.
            app.audio_host.trigger_ui(audio::UiCue::kRewindTapeStart);
          }
          const bool rewind_audio_hold = rewind_buf.is_rewinding() || rewind_buf.is_paused_at_keyframe();
          const bool rewind_settling = rewind_buf.is_settling();
          const bool rewind_snapshot_on = rw_audio.snapshot_active || rw_audio.dsp_active || rw_audio.blend > 0.01;
          const bool rewind_audio_frozen = rewind_buf.is_paused_at_keyframe() || rewind_settling;
          const bool exterior_ui_open =
              !app.show_title && !app.fmv_player.active() &&
              (autoshow.is_open() || shop.is_open() || paint_open || garage_open || events_page.is_open() ||
               map_open || achievements.screen_open() || career_page.is_open() || hub_menu.open() || outpost_hub_open ||
               app.rally_hub_open || app.rally_service_open);
          const bool pause_snapshot_active =
              !app.show_title && !app.fmv_player.active() &&
              (menu.open || settings_page.is_open() || mods_page.is_open() || save_page.is_open() || club_open || car_club_open ||
               slice12.trainer_overlay.is_open() || rewind_snapshot_on ||
               (replay_viewer.active() && replay_viewer.paused()) ||
               (menu_paused && !exterior_ui_open && !photo_mode.active() && !replay_viewer.active() &&
                !trainer_freecam_active));
          audio::EnginePlayer::Update eng_in = sound_inputs(cur_audio_view);
          if (replay_viewer.active()) {
            const auto rcm = replay_viewer.camera_mode();
            const audio::View r_view = (rcm == ReplayCameraMode::kCockpit) ? audio::View::kCockpit
                                       : (rcm == ReplayCameraMode::kBumper || rcm == ReplayCameraMode::kHood)
                                           ? audio::View::kFront
                                           : audio::View::kFollow;
            eng_in.view = r_view;
            if (const auto* rfc = replay_viewer.focused_car()) {
              const double rate = replay_viewer.paused() ? 0.0 : std::clamp(std::fabs(replay_viewer.speed()), 0.25, 2.0);
              const auto& vp = vehicle.params();
              const double rec_rpm = std::clamp(double(rfc->rpm) * rate, vp.idle_rpm * 0.5, vp.redline_rpm * 1.15);
              const double rec_thr = (double(rfc->throttle_u8) / 255.0) * (rate > 0.0 ? 1.0 : 0.0);
              const double torque = vehicle.engine_torque(rec_rpm, rec_thr);
              const double full = vehicle.engine_torque(rec_rpm, 1.0);
              const double closed = vehicle.engine_torque(rec_rpm, 0.0);
              eng_in.rpm = rec_rpm;
              eng_in.throttle = rec_thr;
              eng_in.pos_torque = (torque > 0.0 && full > 0.0) ? (torque / full) : 0.0;
              eng_in.neg_torque = (torque < 0.0 && closed < 0.0) ? (torque / closed) : 0.0;
            }
          } else if (rewind_audio_frozen) {
            eng_in.throttle = 0.0;
            eng_in.pos_torque = 0.0;
            eng_in.neg_torque = 0.0;
          } else if (rw_audio.blend > 0.001) {
            const double focus_gain = std::clamp(rw_audio.focus_car_gain(), 0.0, 1.0);
            eng_in.throttle *= focus_gain;
            eng_in.pos_torque *= focus_gain;
            eng_in.neg_torque *= focus_gain;
          }
          app.audio_host.update_frame(eng_in, pause_snapshot_active, exterior_ui_open,
                                      app.show_title, loading_screen.active());
          {
            using UiMusic = audio::AudioEngine::UiMusicMode;
            UiMusic ui_mus = UiMusic::kNone;
            if (!app.fmv_player.active()) {
              if (loading_screen.active()) {
                // No music bed: the original's loads only duck the radio
                // (AudioMixerSnapshots.xml Loading: GameMusic 0.25, MenuMusic 0).
                ui_mus = UiMusic::kNone;
              } else if (app.show_title) {
                ui_mus = UiMusic::kTitle;
              } else if (map_open) {
                ui_mus = UiMusic::kMap;
              } else if (app.campaign.flow().stage() == race::CampaignStage::kPostRaceFinish ||
                         app.campaign.flow().stage() == race::CampaignStage::kRewardTally ||
                         app.campaign.flow().stage() == race::CampaignStage::kBossVictory ||
                         app.campaign.flow().stage() == race::CampaignStage::kFinaleVictory) {
                ui_mus = UiMusic::kPostRaceVictory;
              } else if (autoshow.is_open() || shop.is_open() || paint_open || garage_open || events_page.is_open() ||
                         outpost_hub_open || app.rally_hub_open || app.rally_service_open) {
                ui_mus = UiMusic::kHomespace;
              }
            }
            app.audio_host.set_ui_music(ui_mus);
          }
          {
            audio::VehicleAudioState vst{};
            const auto& vp = vehicle.params();
            const drive::Vec3 vpos = vehicle.position();
            const drive::Vec3 vvel = vehicle.velocity();
            const drive::Vec3 vright = drive::rotate(vehicle.orientation(), {1, 0, 0});
            fill_focus_audio_state(vehicle, eng_in, host.collision, app.audio_host, vst);
            vst.trans_level = audio_trans_level;
            vst.turbo_level = audio_turbo_level;
            vst.sc_level = audio_sc_level;
            vst.sc_is_centrifugal = audio_sc_is_centrifugal;
            // Backfire level derives strictly from exhaust/cam upgrade levels (stock (0,0) = 0 even with stock turbo).
            vst.backfire_level = std::clamp(std::max(audio_upgrades.exhaust, audio_camshaft_level), 0, 3);
            vst.engine_damage = std::clamp(car_damage.engine_damage, 0.0f, 1.0f);
            vst.drivetrain_damage = std::clamp(
                std::max({car_damage.trans_damage, car_damage.clutch_damage, float(vehicle.transmission_damage()),
                          float(vehicle.clutch_damage())}),
                0.0f, 1.0f);
            {
              uint8_t gmask = 0u;
              if (car_damage.glass_crack >= 0.35f || car_damage.zone_front >= 0.65f) gmask |= (1u << 0);
              if (car_damage.zone_left >= 0.55f || car_damage.zone_right >= 0.55f) gmask |= (1u << 1);
              if (car_damage.zone_rear >= 0.60f) gmask |= (1u << 2);
              if (car_damage.taillight_rl_broken() || car_damage.taillight_rr_broken() ||
                  car_damage.headlight_fl_broken() || car_damage.headlight_fr_broken()) {
                gmask |= (1u << 3);
              }
              if (car_damage.zone_roof >= 0.45f || car_damage.glass_crack >= 0.85f) gmask |= (1u << 4);
              vst.glass_broken_mask = gmask;
            }
            const bool garage_in_workshop =
                garage_open && hub_menu.open() && hub_menu.top() == carview::HubMenu::kWorkshop;
            const bool garage_in_paint =
                garage_open && hub_menu.open() && hub_menu.top() == carview::HubMenu::kPaintBooth;
            vst.hub_ambience =
                autoshow.is_open() ? 1 : ((paint_open || garage_in_paint) ? 2 : ((shop.is_open() || garage_in_workshop) ? 3 : 0));
            {
              // The Autoshow is the homespace showroom scene, whose ui4audio
              // override silences Accept. Other screens set their own scene.
              constexpr std::string_view kShowroomScene = "075_SHOWROOM_HOMESPACE";
              if (autoshow.is_open()) app.audio_host.set_ui_scene(kShowroomScene);
              else if (app.audio_host.ui_scene() == kShowroomScene) app.audio_host.set_ui_scene({});
            }
            const auto& surfs = host.collision.surfaces();
            auto set_listener_from_cam = [&](const drive::CameraView& cv) {
              const auto& cf = cv.forward;
              const auto& cu = cv.up;
              // Right-handed/left-handed basis: cross(up, forward) points right in our Y-up +Z-forward world.
              double rx = cu[1] * cf[2] - cu[2] * cf[1];
              double ry = cu[2] * cf[0] - cu[0] * cf[2];
              double rz = cu[0] * cf[1] - cu[1] * cf[0];
              const double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
              if (rl > 1e-6) {
                rx /= rl;
                ry /= rl;
                rz /= rl;
              } else {
                rx = vright.x;
                ry = vright.y;
                rz = vright.z;
              }
              vst.listener_pos = {float(cv.eye[0]), float(cv.eye[1]), float(cv.eye[2])};
              vst.listener_right = {float(rx), float(ry), float(rz)};
            };
            if (have_rv_cam) {
              set_listener_from_cam(rv_cam);
              if (replay_viewer.camera_mode() == ReplayCameraMode::kTrackSide) {
                vst.listener_vel = {0.0f, 0.0f, 0.0f};
              } else if (const auto* rfc = replay_viewer.focused_car()) {
                const float rate = replay_viewer.paused()
                                       ? 0.0f
                                       : float(std::clamp(std::fabs(replay_viewer.speed()), 0.25, 2.0));
                vst.listener_vel = {rfc->vel[0] * rate, rfc->vel[1] * rate, rfc->vel[2] * rate};
              } else {
                vst.listener_vel = {0.0f, 0.0f, 0.0f};
              }
            } else if (have_cam_view) {
              set_listener_from_cam(last_cam_view);
              vst.listener_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
            } else {
              vst.listener_pos = {float(vpos.x), float(vpos.y), float(vpos.z)};
              vst.listener_right = {float(vright.x), float(vright.y), float(vright.z)};
              vst.listener_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
            }
            vst.cockpit_view = replay_viewer.active()
                                   ? (replay_viewer.camera_mode() == ReplayCameraMode::kCockpit)
                                   : (cur_audio_view == audio::View::kCockpit);
            vst.in_race = app.race != nullptr;
            if (app.race) {
              // ui4audio's countdown / race-complete cues by race type
              // (Events.CareerTypeId: 4 street, 11 nemesis, 7/8 showcase).
              const int ct = app.race->event().career_type;
              app.audio_host.set_race_audio_kind(app.race->is_showcase() || ct == 7 || ct == 8 ? audio::UiRaceKind::kShowcase
                                                 : ct == 4                                     ? audio::UiRaceKind::kStreet
                                                 : ct == 11                                    ? audio::UiRaceKind::kNemesis
                                                                                               : audio::UiRaceKind::kFestival);
            }
            vst.rewinding = rewind_buf.is_rewinding();
            if (vst.rewinding) {
              vst.rewind_speed = float(std::fabs(rw_audio.playback_speed));
              if (rewind_buf.keyframe_count() >= 2) {
                const double t_old = rewind_buf.keyframe(0).replay_time;
                const double t_new = rewind_buf.keyframe(rewind_buf.keyframe_count() - 1).replay_time;
                const double span = std::max(0.25, t_new - t_old);
                vst.rewind_progress = float(std::clamp((t_new - rewind_buf.playback_time()) / span, 0.0, 1.0));
              }
            }
            {
              const std::string lc = lower_ascii(app.car);
              vst.is_electric = (lc.find("leaf") != std::string::npos || lc.find("tesla") != std::string::npos ||
                                 lc.find("electric") != std::string::npos ||
                                 (vp.idle_rpm < 300.0 && vp.gears.size() <= 2));
              // Roadside wall/guardrail proximity for Reflections.fev slap-back bed.
              float prox = 0.0f;
              if (host.ground && vst.speed_mph > 15.0f) {
                const drive::Vec3 ray_origin{vpos.x, vpos.y + 0.9, vpos.z};
                drive::Hit rh{};
                if (host.ground->raycast(ray_origin, vright, 12.0, rh) && std::fabs(rh.normal.y) < 0.6)
                  prox = std::max(prox, float(1.0 - std::clamp(rh.distance / 12.0, 0.0, 1.0)));
                const drive::Vec3 vleft{-vright.x, -vright.y, -vright.z};
                if (host.ground->raycast(ray_origin, vleft, 12.0, rh) && std::fabs(rh.normal.y) < 0.6)
                  prox = std::max(prox, float(1.0 - std::clamp(rh.distance / 12.0, 0.0, 1.0)));
              }
              vst.reflection_proximity = prox;
              const bool aero_car = host.showroom.has_active_aero();
              vst.active_aero_deploy =
                  aero_car ? ((vst.speed_mph > 75.0f || (vst.speed_mph > 55.0f && brake_glow(vehicle) > 0.5f)) ? 1.0f : 0.0f)
                           : 0.0f;
            }
            if (app.race && app.race->is_showcase()) {
              const auto& sc = app.race->showcase();
              const auto& sp = sc.pose();
              vst.showcase.active = true;
              vst.showcase.kind = uint8_t(sc.spec().kind);
              vst.showcase.pos = {float(sp.position[0]), float(sp.position[1]), float(sp.position[2])};
              const double cp = std::cos(sp.pitch);
              vst.showcase.vel = {float(std::sin(sp.heading) * cp * sp.speed_mps),
                                  float(std::sin(sp.pitch) * sp.speed_mps),
                                  float(std::cos(sp.heading) * cp * sp.speed_mps)};
              vst.showcase.speed_mps = float(sp.speed_mps);
              vst.showcase.bank = float(sp.roll);
              vst.showcase.pitch = float(sp.pitch);
            }

            // Gather up to kMaxOtherCars nearest race opponents and traffic cars for 3D audio.
            auto classify_other_archetype = [](const std::string& media_or_name, bool is_traffic,
                                               uint32_t seed) -> uint8_t {
              if (is_traffic) return 6;  // Traffic
              if (!media_or_name.empty()) {
                const std::string s = lower_ascii(media_or_name);
                if (s.find("leaf") != std::string::npos || s.find("tesla") != std::string::npos ||
                    s.find("electric") != std::string::npos || s.find("prius") != std::string::npos) {
                  return 5;  // EV
                }
                if (s.find("lambo") != std::string::npos || s.find("ferrari") != std::string::npos ||
                    s.find("enzo") != std::string::npos || s.find("aventador") != std::string::npos ||
                    s.find("murcielago") != std::string::npos || s.find("gallardo") != std::string::npos ||
                    s.find("viper") != std::string::npos || s.find("lfa") != std::string::npos ||
                    s.find("zonda") != std::string::npos || s.find("huayra") != std::string::npos ||
                    s.find("bugatti") != std::string::npos || s.find("veyron") != std::string::npos ||
                    s.find("agera") != std::string::npos || s.find("mclaren") != std::string::npos ||
                    s.find("aston") != std::string::npos || s.find("v10") != std::string::npos ||
                    s.find("v12") != std::string::npos || s.find("_r8") != std::string::npos) {
                  return 3;  // V10/V12
                }
                if (s.find("skyline") != std::string::npos || s.find("gtr") != std::string::npos ||
                    s.find("supra") != std::string::npos || s.find("evo") != std::string::npos ||
                    s.find("impreza") != std::string::npos || s.find("wrx") != std::string::npos ||
                    s.find("rx7") != std::string::npos || s.find("silvia") != std::string::npos ||
                    s.find("cosworth") != std::string::npos || s.find("quattro") != std::string::npos ||
                    s.find("turbo") != std::string::npos || s.find("rs200") != std::string::npos) {
                  return 4;  // Turbo
                }
                if (s.find("mustang") != std::string::npos || s.find("camaro") != std::string::npos ||
                    s.find("corvette") != std::string::npos || s.find("challenger") != std::string::npos ||
                    s.find("charger") != std::string::npos || s.find("shelby") != std::string::npos ||
                    s.find("chevelle") != std::string::npos || s.find("amg") != std::string::npos ||
                    s.find("sls") != std::string::npos || s.find("ford_gt") != std::string::npos ||
                    s.find("raptor") != std::string::npos || s.find("v8") != std::string::npos) {
                  return 2;  // V8
                }
                if (s.find("350z") != std::string::npos || s.find("370z") != std::string::npos ||
                    s.find("porsche") != std::string::npos || s.find("911") != std::string::npos ||
                    s.find("nsx") != std::string::npos || s.find("vr6") != std::string::npos ||
                    s.find("v6") != std::string::npos || s.find("m3") != std::string::npos) {
                  return 1;  // V6 / I6
                }
                if (s.find("civic") != std::string::npos || s.find("miata") != std::string::npos ||
                    s.find("golf") != std::string::npos || s.find("mini") != std::string::npos ||
                    s.find("focus") != std::string::npos || s.find("elise") != std::string::npos ||
                    s.find("abarth") != std::string::npos || s.find("s2000") != std::string::npos) {
                  return 0;  // I4
                }
              }
              return uint8_t(seed % 5u);
            };
            const uint8_t default_other_sgrp = app.colorado_world() ? 0 : 1;
            struct Cand {
              double d2 = 0;
              audio::OtherCarAudio car{};
              std::string media;  // Data_Car.MediaName: the car's own engine sound
              double idle_rpm = 0, redline_rpm = 0;
            };
            std::vector<Cand> cands;
            if (replay_viewer.active()) {
              const uint32_t focus_slot = replay_viewer.focus_slot();
              const float rate = replay_viewer.paused()
                                     ? 0.0f
                                     : float(std::clamp(std::fabs(replay_viewer.speed()), 0.25, 2.0));
              const float fov_rad = float((have_rv_cam ? rv_cam.fov_degrees : 48.0) * (3.14159265358979323846 / 180.0));
              const float fov_rolloff = std::max(0.25f, audio::replay_fov_rolloff_scale(fov_rad));
              for (const auto& rc : replay_viewer.current_cars()) {
                if (rc.slot == focus_slot) continue;
                const float dx = rc.pos[0] - vst.listener_pos.x;
                const float dy = rc.pos[1] - vst.listener_pos.y;
                const float dz = rc.pos[2] - vst.listener_pos.z;
                const double d2 = double(dx) * dx + double(dy) * dy + double(dz) * dz;
                if (d2 > 260.0 * 260.0) continue;
                const auto* rinfo = replay_viewer.recording().find_car_info(rc.slot);
                audio::OtherCarAudio oc{};
                oc.active = rate > 0.0f;
                oc.is_traffic = rinfo ? rinfo->is_traffic : (rc.slot >= 100u);
                oc.car_id = 0x10000000u | ((rc.slot + 1u) & 0x0FFFFFFFu);
                oc.archetype = classify_other_archetype(rinfo ? rinfo->car_media : std::string{}, oc.is_traffic, rc.slot);
                oc.variant = (oc.archetype <= 3u) ? oc.archetype : uint8_t(rc.slot & 3u);
                // Pass true 3D world position + fov_rolloff_scale separately so azimuth & Doppler stay exact.
                oc.pos = {rc.pos[0], rc.pos[1], rc.pos[2]};
                oc.vel = {rc.vel[0] * rate, rc.vel[1] * rate, rc.vel[2] * rate};
                oc.rpm = std::max(800.0f, rc.rpm * rate);
                oc.speed_mph = float(std::fabs(rc.speed_mps) * 2.2369363) * rate;
                oc.pitch_scale = 0.84f + 0.36f * float((rc.slot * 73u) & 0xFFu) / 255.0f;
                oc.slip = (rc.brake_u8 > 80 || std::abs(int(rc.steer_i8)) > 48) ? 0.38f : 0.10f;
                oc.fov_rolloff_scale = fov_rolloff;
                oc.surface_group = default_other_sgrp;
                cands.push_back({d2, oc, rinfo ? rinfo->car_media : std::string{}});
              }
            } else if (!rewind_audio_frozen) {
              if (app.race) {
                std::vector<carview::RaceView::OpponentTag> otags;
                app.race->opponent_tags(otags);
                for (size_t oi = 0; oi < otags.size(); ++oi) {
                  const auto& ot = otags[oi];
                  const double dx = ot.x - vpos.x, dy = ot.y - vpos.y, dz = ot.z - vpos.z;
                  const double d2 = dx * dx + dy * dy + dz * dz;
                  app.audio_host.prefetch_other_car_sound(ot.car_media);  // the whole field decodes early
                  if (d2 > 220.0 * 220.0) continue;
                  audio::OtherCarAudio oc{};
                  oc.active = true;
                  oc.is_traffic = false;
                  oc.car_id = 0x20000000u | (uint32_t(oi + 1u) & 0x0FFFFFFFu);
                  // Distinct per-opponent engine pitch/timbre identity + cornering tyre scrub.
                  uint32_t nh = 2166136261u ^ uint32_t(oi * 37u);
                  for (char ch : ot.name) nh = (nh ^ uint8_t(ch)) * 16777619u;
                  oc.archetype = classify_other_archetype(ot.car_media.empty() ? ot.name : ot.car_media, false, nh);
                  oc.variant = (oc.archetype <= 3u) ? oc.archetype : uint8_t(oi & 3u);
                  oc.pos = {float(ot.x), float(ot.y), float(ot.z)};
                  oc.vel = {float(ot.vx), float(ot.vy), float(ot.vz)};
                  oc.rpm = float(ot.rpm);
                  oc.speed_mph = float(std::fabs(ot.speed) * 2.2369363);
                  oc.pitch_scale = 0.84f + 0.36f * float(nh & 0xFFu) / 255.0f;
                  oc.slip = (oc.speed_mph > 25.0f && ot.rpm > 4500.0) ? 0.40f : 0.12f;
                  oc.surface_group = default_other_sgrp;
                  oc.throttle = float(ot.throttle);
                  cands.push_back({d2, oc, ot.car_media, ot.idle_rpm, ot.redline_rpm});
                }
              }
              if (traffic && traffic->cars()) {
                for (const auto& tc : *traffic->cars()) {
                  const double dx = tc.position.x - vpos.x, dy = tc.position.y - vpos.y, dz = tc.position.z - vpos.z;
                  const double d2 = dx * dx + dy * dy + dz * dz;
                  if (d2 > 140.0 * 140.0) continue;
                  audio::OtherCarAudio oc{};
                  oc.active = true;
                  oc.is_traffic = true;
                  oc.car_id = 0x30000000u | ((tc.id + 1u) & 0x0FFFFFFFu);
                  oc.archetype = 6;  // Traffic
                  oc.pos = {float(tc.position.x), float(tc.position.y), float(tc.position.z)};
                  oc.vel = {float(std::sin(tc.heading) * tc.speed), 0.0f, float(std::cos(tc.heading) * tc.speed)};
                  oc.rpm = tc.rpm > 100.0 ? float(tc.rpm) : float(1100.0 + std::fabs(tc.speed) * 90.0);
                  oc.speed_mph = float(std::fabs(tc.speed) * 2.2369363);
                  oc.pitch_scale = 0.82f + 0.30f * float((uint32_t(tc.model) * 73u) & 0xFFu) / 255.0f;
                  oc.slip = (tc.target < tc.speed - 2.0 || std::fabs(tc.steer_angle) > 0.12) ? 0.28f : 0.05f;
                  oc.surface_group = default_other_sgrp;
                  const int64_t shown = traffic->shown_model(tc);
                  const drive::CarParams* tp = traffic->params_of(shown);
                  cands.push_back({d2, oc, traffic->media_of(shown), tp ? tp->idle_rpm : 0.0, tp ? tp->redline_rpm : 0.0});
                }
              }
              if (festival_mgr && !app.race) {
                const auto& fcars = festival_mgr->cars();
                for (size_t fi = 0; fi < fcars.size(); ++fi) {
                  const auto& fc = fcars[fi];
                  const double dx = fc.position.x - vpos.x, dy = fc.position.y - vpos.y, dz = fc.position.z - vpos.z;
                  const double d2 = dx * dx + dy * dy + dz * dz;
                  if (d2 > 220.0 * 220.0) continue;
                  audio::OtherCarAudio oc{};
                  oc.active = true;
                  oc.is_traffic = false;
                  oc.car_id = 0x40000000u | ((fc.id + 1u) & 0x0FFFFFFFu);
                  oc.archetype = classify_other_archetype(fc.spec.car_media, false, fc.id);
                  oc.variant = (oc.archetype <= 3u) ? oc.archetype : uint8_t(fc.id & 3u);
                  oc.pos = {float(fc.position.x), float(fc.position.y), float(fc.position.z)};
                  oc.vel = {float(fc.vx), float(fc.vy), float(fc.vz)};
                  oc.rpm = float(fc.rpm);
                  oc.speed_mph = float(std::fabs(fc.speed) * 2.2369363);
                  oc.pitch_scale = 0.85f + 0.34f * float((uint32_t(fc.id) * 97u) & 0xFFu) / 255.0f;
                  oc.slip = fc.braking ? 0.38f : 0.10f;
                  oc.surface_group = default_other_sgrp;
                  cands.push_back({d2, oc, fc.spec.car_media});
                }
              }
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            const size_t n_other = std::min(cands.size(), size_t(audio::VehicleAudioState::kMaxOtherCars));
            for (size_t oi = 0; oi < n_other; ++oi) {
              vst.other_cars[oi] = cands[oi].car;
              app.audio_host.bind_other_car_sound(cands[oi].car.car_id, cands[oi].media, cands[oi].idle_rpm,
                                                  cands[oi].redline_rpm);
            }
            // The next few nearest start decoding before they are heard.
            for (size_t oi = n_other; oi < std::min(cands.size(), n_other + 4); ++oi)
              app.audio_host.prefetch_other_car_sound(cands[oi].media);
            app.audio_host.update_other_car_sounds();

            bool in_countdown = false;
            int countdown_digit = -1;
            if (app.race) {
              const auto rh = app.race->hud_info();
              in_countdown = rh.phase == carview::RaceHud::kCountdown;
              countdown_digit = rh.digit;
            }
            if (replay_viewer.active()) {
              if (const auto* rfc = replay_viewer.focused_car()) {
                const float rate = replay_viewer.paused() ? 0.0f : float(std::clamp(std::fabs(replay_viewer.speed()), 0.25, 2.0));
                vst.speed_mph = float(std::fabs(rfc->speed_mps) * 2.2369363) * rate;
                vst.rpm = std::clamp(rfc->rpm * rate, float(vp.idle_rpm) * 0.5f, float(vp.redline_rpm) * 1.15f);
                vst.throttle = (float(rfc->throttle_u8) / 255.0f) * (rate > 0.0f ? 1.0f : 0.0f);
                vst.engine_load = float(std::clamp(eng_in.pos_torque, 0.0, 1.0));
                vst.gear = rfc->gear;
                vst.engine_damage = std::clamp(rfc->damage[0], 0.0f, 1.0f);
                vst.drivetrain_damage = std::clamp(rfc->damage[1], 0.0f, 1.0f);
                vst.shifting = false;
                vst.scraping = false;
                vst.scrape_intensity = 0.0f;
                for (int wi = 0; wi < 4; ++wi) {
                  vst.wheels[size_t(wi)].wheel_rpm = float(rfc->wheel_omega[wi] * 9.5492966) * rate;
                  vst.wheels[size_t(wi)].slip_combined = rate > 0.0f ? rfc->wheel_slip[wi] : 0.0f;
                }
              }
            } else if (rewind_audio_frozen) {
              // Keep world/tyre/engine live SFX muted while paused at a Rewind keyframe or during 1.0s kSettling.
              vst.speed_mph = 0.0f;
              vst.throttle = 0.0f;
              vst.engine_load = 0.0f;
              vst.scraping = false;
              vst.scrape_intensity = 0.0f;
              for (auto& wa : vst.wheels) {
                wa.slip_combined = 0.0f;
                wa.wheel_rpm = 0.0f;
                wa.bottom_out_force = 0.0f;
              }
            } else if (rw_audio.blend > 0.001) {
              // Smoothly duck secondary vehicle SFX channels alongside Rewind mixer snapshot & DSP.
              const float fx_gain = float(std::clamp(rw_audio.effects_gain(), 0.0, 1.0));
              const float car_gain = float(std::clamp(rw_audio.focus_car_gain(), 0.0, 1.0));
              vst.throttle *= car_gain;
              vst.engine_load *= car_gain;
              vst.scrape_intensity *= fx_gain;
              if (fx_gain < 0.02f) vst.scraping = false;
              for (auto& wa : vst.wheels) {
                wa.slip_combined *= fx_gain;
                wa.bottom_out_force *= fx_gain;
              }
            }
            const bool rewind_audio_active = rewind_audio_hold || rw_audio.snapshot_active || rw_audio.dsp_active;
            app.audio_host.update_vehicle(vst, app.race != nullptr, rewind_audio_active, in_countdown, countdown_digit);
          }
          // Airborne Showcase proximity flyby rumble (airborne_challenges.xml RumbleWhenNearPlayer).
          if (app.race && app.race->is_showcase() && !menu_paused && !rewind_buf.active() && rumble_enabled &&
              rumble_scale > 0.0 && !pads.empty()) {
            const double flyby = app.race->showcase_rumble();
            if (flyby > 0.02) {
              const uint16_t lo = uint16_t(std::clamp(flyby * rumble_scale, 0.0, 1.0) * 65535.0);
              const uint16_t hi = uint16_t(std::clamp(flyby * 0.65 * rumble_scale, 0.0, 1.0) * 65535.0);
              for (SDL_Gamepad* pad : pads) SDL_RumbleGamepad(pad, lo, hi, 100);
            }
          }
          // Airborne Showcase aircraft VFX: twin Merlin V12 exhaust, wingtip vortex trails, and low-pass prop-wash dust.
          if (app.race && app.race->is_showcase() && !menu_paused && !rewind_buf.active() && !app.headless &&
              (!app.bench || app.bench_full_fx) && !app.show_title && !app.fmv_player.active()) {
            app.race->emit_showcase_vfx(host.particle_sys, app.vfx_lib, vehicle.position(),
                                        float(std::clamp(frame_dt, 0.0, 0.05)));
          }
          // Player glass & lamp shatter particle bursts whenever car_damage fractures a new pane/lamp.
          if (!app.headless && (!app.bench || app.bench_full_fx) && !app.show_title && !app.fmv_player.active()) {
            struct GlassMaskTracker {
              std::string car_name;
              uint16_t prev_mask = 0u;
              bool initialized = false;
            };
            static std::unordered_map<const DriveSession*, GlassMaskTracker> s_glass_trackers;
            auto& gtrk = s_glass_trackers[this];
            const bool dmg_on = (car_damage.mode != drive::DamageMode::kNone);
            uint16_t cur_gmask = 0u;
            if (dmg_on) {
              if (car_damage.glass_crack >= 0.35f || car_damage.zone_front >= 0.65f) cur_gmask |= (1u << 0);
              if (car_damage.zone_left >= 0.55f) cur_gmask |= (1u << 1);
              if (car_damage.zone_right >= 0.55f) cur_gmask |= (1u << 2);
              if (car_damage.zone_rear >= 0.60f) cur_gmask |= (1u << 3);
              if (car_damage.zone_roof >= 0.45f || car_damage.glass_crack >= 0.85f) cur_gmask |= (1u << 4);
              if (car_damage.headlight_fl_broken()) cur_gmask |= (1u << 5);
              if (car_damage.headlight_fr_broken()) cur_gmask |= (1u << 6);
              if (car_damage.taillight_rl_broken()) cur_gmask |= (1u << 7);
              if (car_damage.taillight_rr_broken()) cur_gmask |= (1u << 8);
            }
            if (!gtrk.initialized || gtrk.car_name != app.car) {
              gtrk.car_name = app.car;
              gtrk.prev_mask = cur_gmask;
              gtrk.initialized = true;
            } else {
              const uint16_t newly_broken = uint16_t(cur_gmask & ~gtrk.prev_mask);
              if (newly_broken != 0u && !menu_paused && !rewind_buf.active() && !replay_viewer.active()) {
                const drive::Vec3 vp = vehicle.position();
                const drive::Quat vq = vehicle.orientation();
                const drive::Vec3 vv = vehicle.velocity();
                const std::array<float, 3> cvel{float(vv.x), float(vv.y), float(vv.z)};
                const double hw = std::clamp(vehicle.params().size.x * 0.5, 0.72, 1.15);
                const double hl = std::clamp(vehicle.params().size.z * 0.5, 1.65, 2.70);
                const std::array<drive::Vec3, 9> pane_pos = {
                    drive::Vec3{0.0, 0.92, hl * 0.28},
                    drive::Vec3{-hw * 0.92, 0.86, -hl * 0.04},
                    drive::Vec3{hw * 0.92, 0.86, -hl * 0.04},
                    drive::Vec3{0.0, 0.90, -hl * 0.34},
                    drive::Vec3{0.0, 1.24, -hl * 0.04},
                    drive::Vec3{-hw * 0.74, 0.52, hl * 0.96},
                    drive::Vec3{hw * 0.74, 0.52, hl * 0.96},
                    drive::Vec3{-hw * 0.74, 0.60, -hl * 0.96},
                    drive::Vec3{hw * 0.74, 0.60, -hl * 0.96},
                };
                const std::array<drive::Vec3, 9> pane_dir = {
                    drive::Vec3{0.0, 0.65, 0.76},   drive::Vec3{-0.88, 0.47, 0.0},
                    drive::Vec3{0.88, 0.47, 0.0},   drive::Vec3{0.0, 0.65, -0.76},
                    drive::Vec3{0.0, 1.0, 0.0},     drive::Vec3{-0.30, 0.35, 0.89},
                    drive::Vec3{0.30, 0.35, 0.89},  drive::Vec3{-0.30, 0.35, -0.89},
                    drive::Vec3{0.30, 0.35, -0.89},
                };
                for (int bit = 0; bit < 9; ++bit) {
                  if ((newly_broken & (1u << bit)) == 0u) continue;
                  const drive::Vec3 wp = vp + drive::rotate(vq, pane_pos[size_t(bit)]);
                  const drive::Vec3 wd = drive::rotate(vq, pane_dir[size_t(bit)]);
                  host.particle_sys.spawn_glass_shatter(
                      {float(wp.x), float(wp.y), float(wp.z)}, cvel, {float(wd.x), float(wd.y), float(wd.z)});
                }
              }
              gtrk.prev_mask = cur_gmask;
            }
          }
          // Multi-vehicle Race AI & Festival AI tire smoke, dirt/gravel dust, skidmarks, and downshift backfires.
          if (!menu_paused && !rewind_buf.active() && !replay_viewer.active() && !app.headless &&
              (!app.bench || app.bench_full_fx) && !app.show_title && !app.fmv_player.active() && frame_dt > 0.0) {
            constexpr size_t kAiSlots = world::SkidMarkSystem::kMaxSkidVehicles - 1u;  // slots 1..7
            struct AiKinState {
              double prev_speed = 0.0;
              double prev_heading = 0.0;
              double prev_rpm = 0.0;
              float backfire_cd = 0.0f;
              bool valid = false;
            };
            struct AiVfxSessionState {
              std::array<uint64_t, kAiSlots> slot_ids{};
              std::unordered_map<uint64_t, AiKinState> kin;
            };
            static std::unordered_map<const DriveSession*, AiVfxSessionState> s_ai_vfx_states;
            auto& ai_st = s_ai_vfx_states[this];
            const float vfx_dt = float(std::clamp(frame_dt, 1e-4, 0.05));
            const drive::Vec3 ppos = vehicle.position();

            struct AiCandidate {
              uint64_t id = 0u;
              double dist2 = 0.0;
              double x = 0.0, y = 0.0, z = 0.0;
              double vx = 0.0, vy = 0.0, vz = 0.0;
              double speed = 0.0;
              double heading = 0.0;
              double yaw_rate = 0.0;
              double rpm = 0.0;
              bool has_heading = false;
              bool has_yaw_rate = false;
              bool braking = false;
            };
            std::vector<AiCandidate> ai_cands;
            if (app.race) {
              std::vector<carview::RaceView::OpponentTag> otags;
              app.race->opponent_tags(otags);
              ai_cands.reserve(otags.size());
              for (size_t oi = 0; oi < otags.size(); ++oi) {
                const auto& ot = otags[oi];
                const double dx = ot.x - ppos.x, dy = ot.y - ppos.y, dz = ot.z - ppos.z;
                const double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > 140.0 * 140.0 || !std::isfinite(d2)) continue;
                AiCandidate c{};
                c.id = 0x10000000ull | uint64_t(oi + 1u);
                c.dist2 = d2;
                c.x = ot.x; c.y = ot.y; c.z = ot.z;
                c.vx = ot.vx; c.vy = ot.vy; c.vz = ot.vz;
                c.speed = std::hypot(ot.vx, ot.vz);
                if (c.speed > 0.6) {
                  c.heading = std::atan2(ot.vx, ot.vz);
                  c.has_heading = true;
                }
                c.rpm = ot.rpm;
                ai_cands.push_back(c);
              }
            } else if (festival_mgr) {
              const auto& fcars = festival_mgr->cars();
              ai_cands.reserve(fcars.size());
              for (const auto& fc : fcars) {
                if (fc.ghosted()) continue;
                const double dx = fc.position.x - ppos.x, dy = fc.position.y - ppos.y, dz = fc.position.z - ppos.z;
                const double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > 140.0 * 140.0 || !std::isfinite(d2)) continue;
                AiCandidate c{};
                c.id = 0x20000000ull | uint64_t(fc.id + 1u);
                c.dist2 = d2;
                c.x = fc.position.x; c.y = fc.position.y; c.z = fc.position.z;
                c.vx = fc.vx; c.vy = fc.vy; c.vz = fc.vz;
                c.speed = std::fabs(fc.speed);
                c.heading = fc.heading;
                c.has_heading = true;
                c.yaw_rate = fc.yaw_rate + fc.spin_yaw_rate;
                c.has_yaw_rate = true;
                c.rpm = fc.rpm;
                c.braking = fc.braking;
                ai_cands.push_back(c);
              }
            }

            if (ai_cands.size() > kAiSlots) {
              std::partial_sort(ai_cands.begin(), ai_cands.begin() + kAiSlots, ai_cands.end(),
                                [](const AiCandidate& a, const AiCandidate& b) { return a.dist2 < b.dist2; });
              ai_cands.resize(kAiSlots);
            }

            // Keep persistent AI-ID -> skidmark slot [1..7] bindings so tire strips stay unbroken.
            std::array<const AiCandidate*, kAiSlots> slot_cand{};
            std::vector<const AiCandidate*> unassigned;
            for (const auto& c : ai_cands) {
              bool matched = false;
              for (size_t s = 0; s < kAiSlots; ++s) {
                if (ai_st.slot_ids[s] == c.id) {
                  slot_cand[s] = &c;
                  matched = true;
                  break;
                }
              }
              if (!matched) unassigned.push_back(&c);
            }
            for (size_t s = 0; s < kAiSlots; ++s) {
              if (!slot_cand[s] && ai_st.slot_ids[s] != 0u) {
                host.skid_sys.break_vehicle(s + 1u);
                ai_st.slot_ids[s] = 0u;
              }
            }
            size_t ua_i = 0;
            for (size_t s = 0; s < kAiSlots && ua_i < unassigned.size(); ++s) {
              if (!slot_cand[s]) {
                slot_cand[s] = unassigned[ua_i++];
                ai_st.slot_ids[s] = slot_cand[s]->id;
                host.skid_sys.break_vehicle(s + 1u);
              }
            }

            const auto& surfs = host.collision.surfaces();
            const bool is_rally = app.vfx_lib.is_rally_world();
            const float cur_time_s = float(vehicle.time());
            for (size_t s = 0; s < kAiSlots; ++s) {
              const AiCandidate* cp = slot_cand[s];
              if (!cp) continue;
              const AiCandidate& c = *cp;
              auto& ks = ai_st.kin[c.id];
              ks.backfire_cd = std::max(0.0f, ks.backfire_cd - vfx_dt);

              const double hdg = c.has_heading ? c.heading : ks.prev_heading;
              double yaw_r = c.yaw_rate;
              double decel_g = 0.0;
              if (ks.valid) {
                if (!c.has_yaw_rate && c.has_heading) {
                  double dh = hdg - ks.prev_heading;
                  constexpr double kPi = 3.14159265358979323846;
                  while (dh > kPi) dh -= 2.0 * kPi;
                  while (dh < -kPi) dh += 2.0 * kPi;
                  yaw_r = dh / double(vfx_dt);
                }
                decel_g = std::max(0.0, (ks.prev_speed - c.speed) / double(vfx_dt)) / 9.81;
              }
              const double lat_g = std::fabs(c.speed * yaw_r) / 9.81;

              // Probe real terrain surface & road height under this AI car.
              double gnd_y = c.y;
              std::array<float, 3> surf_nrm{0.0f, 1.0f, 0.0f};
              fh1::SurfaceType surf{};
              int surf_idx = -1;
              if (host.ground) {
                drive::Hit hit{};
                if (host.ground->raycast({c.x, c.y + 1.5, c.z}, {0.0, -1.0, 0.0}, 6.0, hit)) {
                  gnd_y = (c.y + 1.5) - hit.distance;
                  surf_nrm = {float(hit.normal.x), float(hit.normal.y), float(hit.normal.z)};
                  surf_idx = hit.surface;
                }
              }
              if (surf_idx >= 0 && size_t(surf_idx) < surfs.size()) {
                surf = surfs[size_t(surf_idx)];
              } else if (is_rally) {
                surf.skid_r = 255; surf.skid_g = 240; surf.skid_b = 215;
                surf.skid_a = 135;
                surf.skid_texture_index = 1;
                surf.skid_min_intensity = 0.45f;
                surf.dust_weight = 0.85f;
                surf.dirt_weight = 0.85f;
                surf.gravel_weight = 0.65f;
                surf.off_roadness = 0.85f;
              } else {
                surf.skid_a = 195;
                surf.skid_texture_index = 0;
                surf.skid_min_intensity = 0.0f;
                surf.smoke_weight = 1.0f;
              }

              const float fx = float(std::sin(hdg));
              const float fz = float(std::cos(hdg));
              const float rx = fz;
              const float rz = -fx;
              const bool offroad_surf =
                  (surf.off_roadness > 0.25f || surf.dust_weight > 0.1f || surf.dirt_weight > 0.1f ||
                   surf.gravel_weight > 0.1f);
              const float slip = std::clamp(
                  float(lat_g * 0.68 + decel_g * 0.55) + (c.braking && c.speed > 10.0 ? 0.32f : 0.0f) +
                      (offroad_surf && c.speed > 6.0 ? 0.22f : 0.0f),
                  0.0f, 1.55f);

              world::VehicleVfxInput ai_vin{};
              ai_vin.vehicle_slot = uint32_t(s + 1u);
              ai_vin.car_pos = {float(c.x), float(gnd_y + 0.5), float(c.z)};
              ai_vin.car_vel = {float(c.vx), float(c.vy), float(c.vz)};
              ai_vin.car_forward = {fx, 0.0f, fz};
              ai_vin.car_right = {rx, 0.0f, rz};
              ai_vin.car_up = surf_nrm;

              static constexpr float kWheelOffsets[4][2] = {
                  {-0.78f, 1.32f}, {0.78f, 1.32f}, {-0.78f, -1.32f}, {0.78f, -1.32f},
              };
              const bool moving = c.speed > 1.2;
              for (int wi = 0; wi < 4; ++wi) {
                auto& wv = ai_vin.wheels[size_t(wi)];
                wv.grounded = moving;
                if (!moving) {
                  host.skid_sys.break_wheel(world::SkidMarkSystem::wheel_slot_for(s + 1u, size_t(wi)));
                  continue;
                }
                const float lx = kWheelOffsets[wi][0];
                const float lz = kWheelOffsets[wi][1];
                wv.contact_pos = {float(c.x) + rx * lx + fx * lz, float(gnd_y), float(c.z) + rz * lx + fz * lz};
                wv.surface_normal = surf_nrm;
                wv.wheel_right = {rx, 0.0f, rz};
                wv.wheel_forward = {fx, 0.0f, fz};
                wv.tyre_width_m = 0.24f;
                // Rear wheels carry slightly higher slip during hard cornering/acceleration.
                wv.slip_combined = (wi >= 2) ? slip : (slip * 0.85f);
                wv.wheel_speed_mps = float(c.speed);
                wv.surface_index = surf_idx;
                wv.surface = surf;
                host.skid_sys.update_vehicle_wheel(s + 1u, size_t(wi), wv, cur_time_s);
              }

              // Occasional AI exhaust backfires on high-RPM downshifts / hard braking into corners.
              if (ks.valid && ks.backfire_cd <= 0.0f && c.speed > 14.0 &&
                  ((ks.prev_rpm > 4800.0 && c.rpm < ks.prev_rpm - 600.0) ||
                   (c.braking && decel_g > 0.55 && c.rpm > 4200.0))) {
                const float ex_z = -2.10f;
                const float ex_y = float(gnd_y) + 0.30f;
                ai_vin.exhaust_tips[0] = {float(c.x) - rx * 0.36f + fx * ex_z, ex_y,
                                          float(c.z) - rz * 0.36f + fz * ex_z};
                ai_vin.exhaust_tips[1] = {float(c.x) + rx * 0.36f + fx * ex_z, ex_y,
                                          float(c.z) + rz * 0.36f + fz * ex_z};
                ai_vin.exhaust_tip_count = 2;
                ai_vin.audio_backfire_pulses = 1;
                ks.backfire_cd = 2.0f + float(c.id % 5u) * 0.35f;
              }

              if (moving || ai_vin.audio_backfire_pulses > 0u) {
                host.particle_sys.update_vehicle(ai_vin, vfx_dt);
              }

              ks.prev_speed = c.speed;
              if (c.has_heading) ks.prev_heading = hdg;
              ks.prev_rpm = c.rpm;
              ks.valid = true;
            }
          }
          int w = 0, h = 0;
          SDL_GetWindowSizeInPixels(app.window, &w, &h);
          // Pose car & camera BEFORE building projected world HUD markers and opponent name tags!
          const drive::Vehicle::Pose stepped = vehicle.pose();
          const bool rewind_scrub_view = rewind_buf.is_rewinding() || rewind_buf.is_paused_at_keyframe();
          if (replay_viewer.active()) {
            // Keep the player car mesh at the player's replay pose (slot 0), never overwriting it
            // with a focused AI car's pose (the AI car renders on its own guest instance).
            const drive::ReplayCarSample* player_rc = nullptr;
            for (const auto& rc : replay_viewer.current_cars()) {
              if (rc.slot == 0u) {
                player_rc = &rc;
                break;
              }
            }
            if (!player_rc) player_rc = replay_viewer.focused_car();
            if (player_rc) {
              drive::Vehicle::Pose rp = stepped;
              player_rc->apply_to_pose(rp);
              vehicle.set_pose(rp);
            }
          } else if (rewind_scrub_view) {
            vehicle.set_pose(rewind_buf.sampled_pose());
            apply_world_rewind_scrub();
          } else {
            vehicle.set_pose(drive::Vehicle::blend(drawn_before, stepped, accumulator / drive::Vehicle::kStep));
            if (app.race) app.race->set_blend(accumulator / drive::Vehicle::kStep);
            if (traffic) traffic->set_blend(accumulator / drive::Vehicle::kStep);
          }
          wheel_poses(vehicle, poses);
          if (photo_mode.active()) {
            const float wb = photo_mode.effects().wheel_blur_omega_boost();
            for (int wi = 0; wi < 4; ++wi) poses[wi].omega += wb;
          }
          host.showroom.set_wheel_poses(poses);
          host.showroom.set_braking(brake_glow(vehicle));
          host.showroom.set_rotor_heat(rotor_heat);
          {
            const float spd_mph = float(std::fabs(vehicle.speed()) * 2.2369363);
            const bool aero_car = host.showroom.has_active_aero();
            const float aero_tgt =
                aero_car ? ((spd_mph > 75.0f || (spd_mph > 55.0f && brake_glow(vehicle) > 0.5f)) ? 1.0f : 0.0f) : 0.0f;
            const bool req_headlamps =
                (light_override == 1 || light_override == 2) ||
                (light_override == 0 && drive::CarLampState::tod_requests_headlamps(app.tod_clock.tod_sec));
            const float popup_tgt = req_headlamps ? 1.0f : 0.0f;
            host.showroom.set_animated_car_parts(aero_tgt, popup_tgt, float(vehicle.time()),
                                                 app.headless ? 10.0f : float(std::clamp(frame_dt, 0.001, 0.1)));
          }
          tod_cockpit_cam = have_rig && rig.mode() == drive::CameraMode::kCockpit;
          place_world();
          // 3D Driving / Braking Line ribbon on the road (SPEC-CAR-PHYSICS):
          // active during both races and free-roam / intro-drive GPS routes.
          update_driving_line_overlay();
          const float aspect = h > 0 ? float(w) / float(h) : 1.0f;
          drive::CameraView rw_cam{};
          drive::CameraView fc_cam{};
          if (trainer_freecam_active) {
            const auto& fc = slice12.trainer.free_cam;
            const double cp = std::cos(fc.pitch_rad), sp = std::sin(fc.pitch_rad);
            fc_cam.eye = fc.pos;
            fc_cam.forward = {std::sin(fc.yaw_rad) * cp, sp, std::cos(fc.yaw_rad) * cp};
            fc_cam.up = {0.0, 1.0, 0.0};
            fc_cam.fov_degrees = 60.0;
          }
          drive::CameraView camp_cam{};
          bool camp_cam_active = false;
          if (!app.show_title && !photo_mode.active() && !trainer_freecam_active && !replay_viewer.active() &&
              app.campaign.is_cinematic_camera_active()) {
            const drive::Vec3 vp = vehicle.position();
            const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            camp_cam_active = app.campaign.compute_camera({vp.x, vp.y, vp.z}, std::atan2(vf.x, vf.z), camp_cam);
          }
          // The race's cutscene camera (sync_race_cutscene): the grid, the
          // count's first cut and the rewards.
          drive::CameraView race_cut_cam{};
          const bool race_cut_active = !app.show_title && !photo_mode.active() && !trainer_freecam_active &&
                                       !replay_viewer.active() && !camp_cam_active && race_cutscene_camera(race_cut_cam);
          const bool pause_freeze = menu_paused && !app.show_title && !app.fmv_player.active() && !photo_mode.active() &&
                                    !replay_viewer.active() && !trainer_freecam_active && !(app.race && grid_menu_open);
          // Smooth pre-race grid-to-drive camera swoop (CAMERAS.md:85, UI_LAYOUTS.md:121-125).
          drive::CameraView pre_race_cam{};
          bool pre_race_cam_active = false;
          if (pre_race_blend_t >= 0.0) {
            if (!pause_freeze) pre_race_blend_t += frame_dt;
            if (pre_race_blend_t >= 1.2) {
              pre_race_blend_t = -1.0;
            } else {
              // Step driving rig so target chase/cockpit view is live and settled.
              if (!pause_freeze || !have_cam_view) (void)drive_frame(aspect, pause_freeze ? 0.0 : frame_dt);
              if (have_cam_view) {
                const drive::Vec3 vp = vehicle.position();
                const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
                const double hdg = std::atan2(vf.x, vf.z);
                const double hero_ang = hdg + 0.52;
                const std::array<double, 3> hero_eye{
                    vp.x + std::sin(hero_ang) * 3.9,
                    vp.y + 0.98,
                    vp.z + std::cos(hero_ang) * 3.9,
                };
                const std::array<double, 3> hero_at{vp.x + vf.x * 0.4, vp.y + 0.72, vp.z + vf.z * 0.4};
                double hfx = hero_at[0] - hero_eye[0], hfy = hero_at[1] - hero_eye[1], hfz = hero_at[2] - hero_eye[2];
                const double hfl = std::max(1e-6, std::sqrt(hfx * hfx + hfy * hfy + hfz * hfz));
                hfx /= hfl;
                hfy /= hfl;
                hfz /= hfl;
                const double u = std::clamp(pre_race_blend_t / 1.15, 0.0, 1.0);
                const double ease = u * u * (3.0 - 2.0 * u);
                for (int k = 0; k < 3; ++k) {
                  pre_race_cam.eye[size_t(k)] = hero_eye[size_t(k)] + (last_cam_view.eye[size_t(k)] - hero_eye[size_t(k)]) * ease;
                }
                const double fx = hfx + (last_cam_view.forward[0] - hfx) * ease;
                const double fy = hfy + (last_cam_view.forward[1] - hfy) * ease;
                const double fz = hfz + (last_cam_view.forward[2] - hfz) * ease;
                const double fl = std::max(1e-6, std::sqrt(fx * fx + fy * fy + fz * fz));
                pre_race_cam.forward = {fx / fl, fy / fl, fz / fl};
                pre_race_cam.up = last_cam_view.up;
                pre_race_cam.fov_degrees = 44.0 + (last_cam_view.fov_degrees - 44.0) * ease;
                pre_race_cam.near_clip = last_cam_view.near_clip;
                pre_race_cam.mode = last_cam_view.mode;
                pre_race_cam_active = true;
              }
            }
          }
          HubRoom active_hub_room = HubRoom::kNone;
          if (!app.show_title && !app.fmv_player.active() && !photo_mode.active() && !replay_viewer.active() &&
              !trainer_freecam_active && !menu.open) {
            if (autoshow.is_open()) active_hub_room = HubRoom::kAutoshow;
            else if (shop.is_open()) active_hub_room = HubRoom::kWorkshop;
            else if (paint_open) active_hub_room = HubRoom::kPaintShop;
            else if (garage_open || outpost_hub_open) active_hub_room = HubRoom::kGarage;
            // The paint booth's and the workshop's lists are shown in their
            // rooms (20_paint_shop_orig.png).
            else if (hub_menu.open() && hub_menu.top() == carview::HubMenu::kPaintBooth)
              active_hub_room = HubRoom::kPaintShop;
            else if (hub_menu.open() && (hub_menu.top() == carview::HubMenu::kWorkshop ||
                                         hub_menu.top() == carview::HubMenu::kUpgradeChoice))
              active_hub_room = HubRoom::kWorkshop;
          }
          ShowroomFrame hub_cam_frame{};
          bool hide_for_garage_preview = false;
          static HubCameraFocus s_last_hub_focus = HubCameraFocus::kTurntable;
          static std::string s_last_hub_car;
          static HubRoom s_cached_stage_room = HubRoom::kNone;
          static float s_cached_stage_hw = 0.0f, s_cached_stage_hl = 0.0f, s_cached_stage_fy = 0.0f;
          static WorldTriangles s_cached_stage_wt;
          if (active_hub_room != HubRoom::kNone) {
            const Showroom* preview_sr = &host.showroom;
            std::string cur_preview_car = app.car;
            int64_t garage_preview_seq = -1;
            if (active_hub_room == HubRoom::kAutoshow) {
              host.showroom.set_instances({});
              if (const auto* sel_as = autoshow.selected_car(); sel_as && !sel_as->dealer.media_name.empty()) {
                cur_preview_car = sel_as->dealer.media_name;
                if (auto it = festival_guests.find(cur_preview_car); it != festival_guests.end() && it->second) {
                  preview_sr = it->second.get();
                }
              }
            } else if (active_hub_room == HubRoom::kGarage && garage_open) {
              if (app.have_garage && !app.career_garage.cars.empty()) {
                if (const auto* ge = garage_browser.selected_entry()) {
                  if (!ge->media_name.empty()) cur_preview_car = ge->media_name;
                  else if (auto bit = app.baked_ids.find(ge->car_id); bit != app.baked_ids.end()) cur_preview_car = bit->second;
                  garage_preview_seq = ge->colour_seq;
                }
              } else if (garage_pick < app.baked_cars.size()) {
                cur_preview_car = app.baked_cars[garage_pick];
              }
              if (!cur_preview_car.empty() && lower_ascii(cur_preview_car) != lower_ascii(app.car)) {
                const std::string gkey =
                    garage_preview_seq > 0 ? (cur_preview_car + "#" + std::to_string(garage_preview_seq)) : cur_preview_car;
                auto it = festival_guests.find(gkey);
                if (it == festival_guests.end() && garage_preview_seq > 0) it = festival_guests.find(cur_preview_car);
                if (it == festival_guests.end() && festival_guests.size() < 24) {
                  auto gs = std::make_unique<Showroom>(host.renderer);
                  gs->set_guest();
                  ShowroomOptions opt{};
                  opt.game_root = app.game_root_of();
                  if (garage_preview_seq > 0) opt.match_sequence = garage_preview_seq;
                  std::string ge_err;
                  if (gs->load(app.bake_dir, cur_preview_car, opt, ge_err) && gs->upload(ge_err)) {
                    gs->load_car_lamps(app.game_root_of(), cur_preview_car);
                    it = festival_guests.emplace(gkey, std::move(gs)).first;
                  } else {
                    it = festival_guests.emplace(gkey, nullptr).first;
                  }
                }
                if (it != festival_guests.end() && it->second) {
                  preview_sr = it->second.get();
                  hide_for_garage_preview = true;
                  Showroom::Instance inst{};
                  inst.car = it->second.get();
                  inst.model[0] = 1.0f;
                  inst.model[5] = 1.0f;
                  inst.model[10] = 1.0f;
                  inst.model[13] = host.showroom.bounds_min()[1] - it->second->bounds_min()[1];
                  inst.model[15] = 1.0f;
                  host.showroom.set_instances({inst});
                } else {
                  host.showroom.set_instances({});
                }
              } else {
                host.showroom.set_instances({});
              }
            } else if (active_hub_room != HubRoom::kGarage) {
              host.showroom.set_instances({});
            }

            // MENU-02 / Q262 / Q365: Autoshow, Workshop and Paint Shop load their
            // own interior room tracks (uiautoshow, uiworkshop, uipaintshop).
            // MY GARAGE opened inside the workshop or paint booth keeps that room;
            // opened from an Outpost or free roam it stays over the live Colorado
            // world with the close low orbit camera (first30l outpost-garage2,
            // mode=21, fov=48.14 deg) rather than a synthetic dark studio stage.
            const HubRoom scene_room =
                (active_hub_room == HubRoom::kGarage && hub_menu.open() &&
                 hub_menu.top() == carview::HubMenu::kWorkshop)
                    ? HubRoom::kWorkshop
                : (active_hub_room == HubRoom::kGarage && hub_menu.open() &&
                   hub_menu.top() == carview::HubMenu::kPaintBooth)
                    ? HubRoom::kPaintShop
                    : active_hub_room;
            const bool room_track = carview::hub_room_track(scene_room) != nullptr && !app.cli_hub_studio &&
                                    host.enter_room_world(scene_room);
            const bool live_world_garage =
                active_hub_room == HubRoom::kGarage && !room_track && !app.cli_hub_studio;
            if (!room_track) host.leave_room_world();

            const HubCameraFocus desired_focus =
                (scene_room == HubRoom::kWorkshop)
                    ? carview::focus_for_part_enum(shop.active_part_enum())
                    : (scene_room == HubRoom::kPaintShop)
                          ? (livery_editor.is_open() ? HubCameraFocus::kSide : HubCameraFocus::kTurntable)
                          : HubCameraFocus::kTurntable;
            // MENU-03: the disc's camera for the screen (a camera.zip sequence
            // or a UICarTargetCameraPresets.xml preset), else the engine's focus.
            const bool colour_select =
                scene_room == HubRoom::kAutoshow && autoshow.step() == carview::AutoshowStep::kColorSelect;
            const carview::HubCameraShot shot = carview::hub_camera_shot(
                scene_room, scene_room == HubRoom::kWorkshop && shop.is_open() ? shop.active_part_enum() : -1,
                livery_editor.is_open(), int(livery_editor.region()), colour_select);
            const cutscene::Cutscene* sequence =
                shot.sequence_member.empty() ? nullptr : hub_sequence(shot.sequence_member, shot.sequence);
            const std::string_view want_preset = sequence ? std::string_view{} : shot.preset;
            const carview::CarCameraPreset* preset = hub_preset(want_preset);
            static std::string s_last_hub_preset;
            static const cutscene::Cutscene* s_last_hub_sequence = nullptr;
            const bool room_changed = (active_hub_room != last_hub_room);
            if (room_changed) {
              hub_cam = HubCamera{};
              last_hub_room = active_hub_room;
              s_last_hub_sequence = nullptr;
            }
            {
              const auto& pb0 = preview_sr->bounds_min();
              const auto& pb1 = preview_sr->bounds_max();
              hub_cam.ground_y = pb0[1];
              hub_cam.parts = car_parts(vehicle, pb0, pb1);
            }
            // Only reset the camera when the hub screen, the shot or the
            // highlighted car changes, keeping the orbit and the stick's look.
            // A new car in the Autoshow's or MY GARAGE's browse keeps the orbit running.
            if (room_changed || desired_focus != s_last_hub_focus ||
                (!live_world_garage && cur_preview_car != s_last_hub_car) ||
                want_preset != s_last_hub_preset || sequence != s_last_hub_sequence) {
              if (sequence)
                hub_cam.play(sequence);
              else if (preset)
                hub_cam.set_preset(*preset);
              else if (live_world_garage) {
                // Close low orbit around the parked car in Colorado matching
                // first30l outpost-garage2 (mode=21, fov=48.14 deg).
                hub_cam.play(nullptr);
                hub_cam.set_focus(HubCameraFocus::kTurntable);
                hub_cam.yaw = hub_cam.target_yaw = -0.82f;
                hub_cam.pitch = hub_cam.target_pitch = 0.13f;
                hub_cam.distance = hub_cam.target_dist = 3.65f;
                const float cy = host.showroom.bounds_min()[1] + 0.56f;
                hub_cam.target[0] = hub_cam.target_pos[0] = 0.30f;
                hub_cam.target[1] = hub_cam.target_pos[1] = cy;
                hub_cam.target[2] = hub_cam.target_pos[2] = 0.0f;
                hub_cam.fov_deg = hub_cam.target_fov_deg = 48.14f;
                hub_cam.auto_orbit = true;
                hub_cam.orbit_speed = 0.14f;
              } else {
                hub_cam.play(nullptr);
                hub_cam.set_focus(desired_focus);
              }
              s_last_hub_focus = desired_focus;
              s_last_hub_car = cur_preview_car;
              s_last_hub_preset = std::string(want_preset);
              s_last_hub_sequence = sequence;
            }
            const float hub_dt = float(std::clamp(frame_dt, 0.0, 0.1));
            for (SDL_Gamepad* pad : pads) {
              const double rx = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0, 0.18, 0.95);
              const double ry = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0, 0.18, 0.95);
              if (std::fabs(rx) > 1e-3 || std::fabs(ry) > 1e-3) {
                hub_cam.nudge_orbit(float(rx) * 1.8f * hub_dt, float(-ry) * 1.1f * hub_dt, 0.0f);
              }
            }
            hub_cam.step(hub_dt);

            if (live_world_garage) {
              const auto cv = hub_cam.view();
              const V3 eye{cv.eye[0], cv.eye[1], cv.eye[2]};
              const V3 at{cv.eye[0] + cv.forward[0], cv.eye[1] + cv.forward[1], cv.eye[2] + cv.forward[2]};
              const V3 up{cv.up[0], cv.up[1], cv.up[2]};
              const float fov = cv.fov_deg > 1.0f ? cv.fov_deg : 48.14f;
              hub_cam_frame = look_frame(eye, at, up, aspect, false, host.renderer.reverse_z(), fov, 0.1f, &host.cam_proj);
              s_cached_stage_room = HubRoom::kNone;
            } else {
            carview::build_hub_frame(active_hub_room, hub_cam, uint32_t(std::max(1, w)), uint32_t(std::max(1, h)),
                                     hub_cam_frame, host.renderer.reverse_z());

            // Render the dedicated 3D studio turntable stage & per-room background colour while
            // keeping the streamed Colorado GPU world resident (shifted below far clip).
            const auto hub_bg = carview::hub_background_color(active_hub_room);
            host.showroom.set_background(hub_bg[0], hub_bg[1], hub_bg[2]);
            const auto& bmin = preview_sr->bounds_min();
            const auto& bmax = preview_sr->bounds_max();
            const float hw = std::clamp(0.5f * (bmax[0] - bmin[0]), 0.70f, 1.45f);
            const float hl = std::clamp(0.5f * (bmax[2] - bmin[2]), 1.60f, 3.00f);
            const float fy = bmin[1];
            if (room_track) {
              const carview::HubRoomScenery* rs = host.room_world_scenery();
              const float lift = fy - (rs ? rs->floor_y : 0.0f);
              const float yaw = carview::hub_room_yaw(scene_room);
              const float cy = std::cos(yaw), sy = std::sin(yaw);
              // The room's game frame (the car's body axes, +Z forward) turned
              // by the room's yaw, then mirrored into the car model's frame
              // (-Z forward; drive::model_to_world's B), the floor at the
              // car's wheels.
              const float room_xf[16] = {
                  cy, 0.0f, sy, 0.0f,
                  0.0f, 1.0f, 0.0f, 0.0f,
                  sy, 0.0f, -cy, 0.0f,
                  0.0f, lift, 0.0f, 1.0f,
              };
              host.showroom.set_world_transform(room_xf);
              // The car's key light comes from the room's ShadowLightDirection
              // (the game frame, the way the light travels; z mirrored as
              // in room_xf).
              float room_sun[3] = {0.45f, 0.78f, -0.42f};
              if (rs) {
                const auto& d = rs->light_travel;
                const float x = -(cy * d[0] + sy * d[2]), y = -d[1], z = -(sy * d[0] - cy * d[2]);
                const float len = std::sqrt(x * x + y * y + z * z);
                if (len > 1e-4f) {
                  room_sun[0] = x / len;
                  room_sun[1] = y / len;
                  room_sun[2] = z / len;
                }
              }
              const float room_up[3] = {0.0f, 1.0f, 0.0f};
              host.showroom.set_world_light(room_sun, room_up, 1.0f);
              // The room's light (hub_room.h): no sun, no fog; the surfaces
              // take their lightmaps through world_frag.glsl's interior
              // branch (day_night_post -1 selects it), the car the room's
              // key light.
              if (rs) {
                world::TodState& t = host.showroom.tod_state();
                t.sun_color = {0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 3; ++k) {
                  t.hl_top[size_t(k)] = rs->hemi_top[size_t(k)];
                  t.hl_bottom[size_t(k)] = rs->hemi_bottom[size_t(k)];
                  t.ambient_color[size_t(k)] = rs->ibl_ambient[size_t(k)];
                  t.car_sun_color[size_t(k)] = rs->car_key[size_t(k)];
                  t.car_hl_top[size_t(k)] = rs->cube_mean[size_t(k)];
                  t.car_hl_bottom[size_t(k)] = rs->cube_mean[size_t(k)];
                }
                t.day_night_post = -1.0f;
                t.ibl_ambient_scale = 1.0f;
                t.switch_on_lights = 0.0f;
                t.fog_density = 0.0f;
                t.fog_start = 1.0e6f;
                t.fog_height_fade_amount = 0.0f;
              }
              host.showroom.set_debris_mesh({});
              host.showroom.set_skidmarks({}, {});
              host.showroom.set_particles({}, {}, {}, 0.25f, false);
              host.showroom.set_showcase_mesh({});
              host.showroom.set_world_overlay({});
              s_cached_stage_room = HubRoom::kNone;
            } else {
            if (s_cached_stage_room != active_hub_room || std::fabs(s_cached_stage_hw - hw) > 0.02f ||
                std::fabs(s_cached_stage_hl - hl) > 0.02f || std::fabs(s_cached_stage_fy - fy) > 0.02f) {
              s_cached_stage_room = active_hub_room;
              s_cached_stage_hw = hw;
              s_cached_stage_hl = hl;
              s_cached_stage_fy = fy;
              s_cached_stage_wt = carview::build_hub_stage(active_hub_room, hw, hl, fy);
            }
            constexpr float kHubStageYOffset = 2000.0f;
            const float hub_world_xf[16] = {
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, -kHubStageYOffset, 0.0f, 1.0f,
            };
            host.showroom.set_world_transform(hub_world_xf);
            const float studio_sun[3] = {0.45f, 0.78f, -0.42f};
            const float studio_up[3] = {0.0f, 1.0f, 0.0f};
            host.showroom.set_world_light(studio_sun, studio_up, 1.0f);
            const double ox = host.showroom.world_origin_x();
            const double oz = host.showroom.world_origin_z();
            std::vector<world::DebrisVertex> stage_verts;
            const size_t n_tris = s_cached_stage_wt.positions.size() / 3u;
            stage_verts.reserve(n_tris * 3u);
            auto lin_to_u8_srgb = [](float c) -> uint8_t {
              const float s = std::pow(std::clamp(c, 0.0f, 1.0f), 1.0f / 2.2f);
              return uint8_t(std::clamp(std::lround(s * 255.0f), 0l, 255l));
            };
            for (size_t t = 0; t < n_tris; ++t) {
              const uint8_t grp = t < s_cached_stage_wt.group.size() ? s_cached_stage_wt.group[t] : 0u;
              const std::array<float, 3> col =
                  grp < s_cached_stage_wt.colours.size() ? s_cached_stage_wt.colours[grp] : std::array<float, 3>{0.2f, 0.2f, 0.22f};
              const std::array<uint8_t, 4> rgba{lin_to_u8_srgb(col[0]), lin_to_u8_srgb(col[1]),
                                                lin_to_u8_srgb(col[2]), 255u};
              for (size_t k = 0; k < 3u; ++k) {
                const size_t vi = t * 3u + k;
                const auto& p = s_cached_stage_wt.positions[vi];
                const auto nrm = vi < s_cached_stage_wt.normals.size() ? s_cached_stage_wt.normals[vi]
                                                                       : std::array<float, 3>{0.0f, 1.0f, 0.0f};
                world::DebrisVertex dv{};
                dv.pos = {float(double(p[0]) + ox), p[1] + kHubStageYOffset, float(double(p[2]) + oz)};
                dv.normal = nrm;
                dv.rgba = rgba;
                dv.texture_record = world::kNoTexture;
                stage_verts.push_back(dv);
              }
            }
            host.showroom.set_debris_mesh(stage_verts);
            host.showroom.set_skidmarks({}, {});
            host.showroom.set_particles({}, {}, {}, 0.25f, false);
            host.showroom.set_showcase_mesh({});
            host.showroom.set_world_overlay({});
            }
            }
          } else if (last_hub_room != HubRoom::kNone) {
            host.showroom.set_background(0.42f, 0.55f, 0.72f);
            host.showroom.set_draw_car(true);
            last_hub_room = HubRoom::kNone;
            s_last_hub_car.clear();
          }
          // The room's track stays drawn under a menu opened over its screen;
          // it goes when the room's screens close.
          if (host.room_world != HubRoom::kNone && !autoshow.is_open() && !shop.is_open() && !paint_open &&
              !(hub_menu.open() && (hub_menu.top() == carview::HubMenu::kPaintBooth ||
                                    hub_menu.top() == carview::HubMenu::kWorkshop ||
                                    hub_menu.top() == carview::HubMenu::kUpgradeChoice)))
            host.leave_room_world();
          ShowroomFrame cam_frame =
              photo_mode.active()
                  ? camera_view_frame(photo_mode.camera().view(), info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                  : trainer_freecam_active
                        ? camera_view_frame(fc_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                        : replay_viewer.active()
                        ? camera_view_frame(rv_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                        : active_hub_room != HubRoom::kNone
                        ? hub_cam_frame
                        : app.show_title
                        ? scenic_orbit_frame(info, vehicle, aspect,
                                             std::atan2(drive::rotate(vehicle.orientation(), {0, 0, 1}).x,
                                                        drive::rotate(vehicle.orientation(), {0, 0, 1}).z) +
                                                 0.58 + 0.34 * std::sin(title_orbit_time * 0.32),
                                             5.8f, 1.48f, host.renderer.reverse_z(), host.ground, &host.cam_proj)
                        : camp_cam_active
                        ? camera_view_frame(camp_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                        : race_cut_active
                        ? camera_view_frame(race_cut_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                        : (app.race && grid_menu_open)
                              ? scenic_orbit_frame(info, vehicle, aspect,
                                                   std::atan2(drive::rotate(vehicle.orientation(), {0, 0, 1}).x,
                                                              drive::rotate(vehicle.orientation(), {0, 0, 1}).z) +
                                                       0.58 + 0.12 * std::sin(title_orbit_time * 0.4),
                                                   5.6f, 1.35f, host.renderer.reverse_z(), host.ground, &host.cam_proj)
                              : pre_race_cam_active
                              ? camera_view_frame(pre_race_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                              : (rewind_scrub_view && rewind_buf.sampled_camera(rw_cam))
                                    ? camera_view_frame(rw_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                                    : (pause_freeze && have_rig && !app.front && have_cam_view)
                                          ? camera_view_frame(last_cam_view, info, vehicle, aspect, host.renderer.reverse_z(),
                                                              &host.cam_proj)
                                          : drive_frame(aspect, pause_freeze ? 0.0 : frame_dt);
          float w_jitter_uv_x = 0.0f, w_jitter_uv_y = 0.0f;
          const bool temporal_freeze = pause_freeze || rewind_buf.is_paused_at_keyframe();
          if (photo_mode.active() && photo_mode.accumulator().active()) {
            float jx = 0.0f, jy = 0.0f;
            photo_mode.accumulator().jitter_ndc(jx, jy);
            apply_ndc_jitter(cam_frame, jx, jy);
          } else if (host.renderer.active_upscaler().temporal && !temporal_freeze) {
            const auto rex = host.renderer.render_width()
                                 ? carview::modern::Extent{host.renderer.render_width(), host.renderer.render_height()}
                                 : host.renderer.expected_render_extent(uint32_t(w), uint32_t(h));
            const uint32_t jphase = carview::modern::jitter_phase_count(rex, {uint32_t(w), uint32_t(h)});
            const auto j = carview::modern::halton_jitter(host.temporal_frame_idx++, jphase);
            apply_ndc_jitter(cam_frame, 2.0f * j[0] / float(std::max(1u, rex.w)),
                             2.0f * j[1] / float(std::max(1u, rex.h)));
            w_jitter_uv_x = j[0] / float(std::max(1u, rex.w));
            w_jitter_uv_y = j[1] / float(std::max(1u, rex.h));
          }
          if (photo_mode.active() || replay_viewer.active() || active_hub_room != HubRoom::kNone || app.show_title ||
              camp_cam_active || race_cut_active || (app.race && grid_menu_open) || ceremony.active || rewind_scrub_view) {
            const bool hide_for_replay =
                replay_viewer.active() && replay_viewer.focus_slot() == 0u &&
                replay_viewer.camera_mode() == carview::ReplayCameraMode::kBumper;
            const bool hide_for_photo = photo_mode.active() && photo_mode.effects().hide_car;
            host.showroom.set_draw_car(!hide_for_replay && !hide_for_photo && !hide_for_garage_preview);
          }
          host.renderer.set_photo_pt_accumulate(photo_mode.active() &&
                                           host.renderer.active_rt_mode() != carview::rt::RtMode::kOff);
          host.showroom.set_frame(cam_frame);

          // Update projected Free-Roam World Event / Festival Marker.
          active_trigger = carview::WorldTrigger{};
          world_marker = carview::WorldMarkerHud{};
          if (!app.race && !app.show_title && (app.have_career_catalog || app.have_outposts) && w > 0 && h > 0) {
            const drive::Vec3 at = vehicle.position();
            if (app.have_career_catalog) {
              active_trigger = carview::select_world_trigger(app.career_catalog, app.career, app.collected_wristband,
                                                             app.current_pop_rank(), at.x, at.z);
              // In the first-time career's sign-up sphere its event is the
              // one in range (not the workshop or paint shop beside it).
              if (const int64_t fe = app.campaign.ftc_sign_up_event(); fe > 0) {
                for (const auto& ev : app.career_catalog.events) {
                  if (ev.event_id != fe) continue;
                  active_trigger = carview::WorldTrigger{};
                  active_trigger.kind = carview::WorldTrigger::kEvent;
                  active_trigger.event = &ev;
                  active_trigger.in_range = true;
                  active_trigger.dist = std::hypot(ev.trigger_pos[0] - at.x, ev.trigger_pos[2] - at.z);
                  break;
                }
              }
            }
            if (app.have_outposts) {
              for (const auto& op : app.outposts.outposts) {
                const double d = std::hypot(op.trigger.x - at.x, op.trigger.z - at.z);
                const double max_d = std::max(160.0, op.discovery_radius);
                if (d > max_d) continue;
                const bool in_r = d <= op.trigger_radius;
                const bool better = active_trigger.kind == carview::WorldTrigger::kNone ||
                                    (in_r && !active_trigger.in_range) ||
                                    (in_r == active_trigger.in_range && d < active_trigger.dist);
                if (better) {
                  active_trigger = carview::WorldTrigger{};
                  active_trigger.kind = carview::WorldTrigger::kOutpost;
                  active_trigger.outpost = &op;
                  active_trigger.in_range = in_r;
                  active_trigger.dist = d;
                }
              }
            }
            if (active_trigger.kind != carview::WorldTrigger::kNone) {
              std::array<double, 3> p{};
              if (active_trigger.kind == carview::WorldTrigger::kOutpost && active_trigger.outpost) {
                const auto& op = *active_trigger.outpost;
                p = {op.trigger.x, op.trigger.y, op.trigger.z};
                world_marker.is_festival = true;
                world_marker.title = op.display_name;
                int done_st = 0;
                for (const auto& mid : op.mission_ids) done_st += app.outpost_progress.is_stunt_completed(mid);
                world_marker.subtitle = "HORIZON OUTPOST  •  PR STUNTS " + std::to_string(done_st) + "/" +
                                        std::to_string(op.mission_ids.size());
                world_marker.prompt = "ENTER OUTPOST HUB";
                world_marker.badge = app.outpost_progress.is_outpost_discovered(op.id) ? "HUB" : "NEW";
              } else if (active_trigger.site) {
                p = active_trigger.site->pos;
                world_marker.is_festival = true;
                world_marker.title = active_trigger.site->name;
                const bool wb_ready = active_trigger.site->kind == race::FestivalSite::kRaceCentral &&
                                      app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband) >= 0;
                world_marker.subtitle = wb_ready ? "NEW WRISTBAND READY TO COLLECT!" : "HORIZON FESTIVAL";
                world_marker.prompt = wb_ready ? "COLLECT WRISTBAND" : "ENTER NOW";
                if (wb_ready) world_marker.badge = "NEW";
              } else if (active_trigger.hub) {
                p = active_trigger.hub->trigger_pos;
                world_marker.title = "STREET RACE HUB " + std::to_string(active_trigger.hub->id);
                world_marker.subtitle = "STREET RACING";
                world_marker.prompt = "ENTER HUB";
              } else if (active_trigger.event) {
                p = active_trigger.event->trigger_pos;
                const std::string ename = carview::game_text(active_trigger.event->name_ref);
                world_marker.title = (!ename.empty() && ename.rfind("_&", 0) != 0) ? ename : active_trigger.event->horizon_id;
                int64_t cid = 0;
                int clid = 2;
                current_car_info(cid, clid);
                const auto elig = app.career_catalog.check_eligibility(*active_trigger.event, cid, clid, app.class_name_list());
                world_marker.eligible = elig.eligible;
                world_marker.subtitle = elig.eligible
                                            ? (std::string(race::CareerCatalog::event_style_label(*active_trigger.event)) +
                                               "  •  " + carview::thousands(int64_t(active_trigger.event->cash_prize)) + " Cr")
                                            : elig.reason;
                world_marker.prompt = elig.eligible ? carview::text_of(carview::hud_text::kEnterNow) : elig.reason;
                // A sponsored event reads "HORIZON PRESENTS" over its name and
                // asks SIGN ME UP! (09_event_marker_prompt_orig.png).
                world_marker.sponsor = carview::event_sponsor(active_trigger.event->event_id);
                if (!world_marker.sponsor.empty()) {
                  world_marker.title = carview::event_name_without_sponsor(world_marker.title, world_marker.sponsor);
                  if (elig.eligible) world_marker.prompt = carview::text_of(carview::hud_text::kSignMeUp);
                }
                // The icon's ring is the event's wristband colour: P2P0 is the
                // festival's white, then one per level (The Gauntlet, level 1,
                // shows P2P2's green).
                world_marker.icon = std::string("ICONS/ACTIVATIONZONEICONS/") +
                                    (active_trigger.event->laps > 1 ? "CIRCUIT" : "P2P") +
                                    std::to_string(std::clamp(active_trigger.event->level + 1, 0, 7));
                const int bp = app.career_catalog.best_place(*active_trigger.event, app.career);
                if (bp == 0) world_marker.badge = "NEW";
                else if (bp == 1) world_marker.badge = "1ST";
              }
              float sx = 0.5f * float(w), sy = 0.35f * float(h);
              const bool on_screen = project_world_point(cam_frame, info, vehicle, p[0], p[1] + 2.6, p[2], float(w),
                                                         float(h), sx, sy);
              if (on_screen || active_trigger.in_range) {
                world_marker.visible = true;
                world_marker.in_range = active_trigger.in_range;
                world_marker.screen_x = on_screen ? sx : 0.5f * float(w);
                world_marker.screen_y = on_screen ? sy : 0.32f * float(h);
                world_marker.distance_m = active_trigger.dist;
              }
              // The first-time career's event in its sign-up sphere: the
              // banner asks SIGN ME UP! and X signs up (first30e r7 h00318).
              if (const int64_t fe = app.campaign.ftc_sign_up_event();
                  fe > 0 && active_trigger.event && active_trigger.event->event_id == fe) {
                world_marker.visible = true;
                world_marker.in_range = true;
                if (!on_screen) {
                  world_marker.screen_x = 0.5f * float(w);
                  world_marker.screen_y = 0.32f * float(h);
                }
                world_marker.prompt = carview::text_of(carview::hud_text::kSignMeUp);
              }
            }
          }
          // Update projected Opponent Overhead Name Tags in races (race-hud.png),
          // Airborne Showcases, and free-roam Festival AI racers.
          name_tags.clear();
          struct ProjectedTag {
            double dist = 0.0;
            NameTagHud tag;
          };
          std::vector<ProjectedTag> projected_tags;
          if (app.race && !grid_menu_open && !app.race->player_finished() && w > 0 && h > 0) {
            std::vector<carview::RaceView::OpponentTag> otags;
            app.race->opponent_tags(otags);
            const drive::Vec3 at = vehicle.position();
            // The original's race tags read the driver's surname (MALONE; Q361).
            auto surname = [](const std::string& n) {
              const size_t sp = n.find_last_of(' ');
              return sp == std::string::npos || sp + 1 >= n.size() ? n : n.substr(sp + 1);
            };
            for (const auto& ot : otags) {
              const double d = std::hypot(ot.x - at.x, ot.z - at.z);
              if (d > carview::kNameTagMaxM) continue;
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, ot.x, ot.y + 1.85, ot.z, float(w), float(h), sx, sy) &&
                  sx >= 0.0f && sx <= float(w) && sy >= 0.0f && sy <= float(h))
                projected_tags.push_back({d, {sx, sy, surname(ot.name), ot.leader, d}});
            }
            carview::RaceView::OpponentTag stag;
            if (app.race->showcase_tag(stag)) {
              const double d = std::hypot(stag.x - at.x, stag.z - at.z);
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, stag.x, stag.y, stag.z, float(w), float(h), sx, sy) &&
                  sx >= 0.0f && sx <= float(w) && sy >= 0.0f && sy <= float(h))
                projected_tags.push_back({d, {sx, sy, stag.name, stag.leader, d}});
            }
          } else if (!app.race && festival_mgr && w > 0 && h > 0) {
            const drive::Vec3 at = vehicle.position();
            for (const auto& fc : festival_mgr->cars()) {
              const double d = std::hypot(fc.position.x - at.x, fc.position.z - at.z);
              if (d > carview::kNameTagMaxM) continue;
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, fc.position.x, fc.position.y + 1.85, fc.position.z,
                                      float(w), float(h), sx, sy) &&
                  sx >= 0.0f && sx <= float(w) && sy >= 0.0f && sy <= float(h))
                projected_tags.push_back(
                    {d, {sx, sy, "[" + fc.spec.car_class + " " + std::to_string(fc.spec.pi) + "] " + fc.spec.driver_name,
                         fc.in_h2h, d}});
            }
          }
          if (!projected_tags.empty()) {
            std::sort(projected_tags.begin(), projected_tags.end(), [](const ProjectedTag& a, const ProjectedTag& b) {
              if (a.tag.leader != b.tag.leader && std::fabs(a.dist - b.dist) < 5.0) return a.tag.leader;
              return a.dist < b.dist;
            });
            const size_t keep = std::min<size_t>(projected_tags.size(), 10);
            name_tags.reserve(keep);
            for (size_t i = 0; i < keep; ++i) name_tags.push_back(std::move(projected_tags[i].tag));
          }

          if (photo_mode.active()) {
            PhotoModeController::set_text_resolver([](const std::string& ref) { return carview::game_text(ref); });
            float cfw[16];
            compute_clip_from_world(cam_frame, info, vehicle, cfw);
            photo_mode.update_live_promo_preview(
                promo_tracker.evaluate(cfw, photo_mode.camera().eye(), gather_promo_candidates(), false));
            carview::ui::List plist;
            photo_mode.build_hud(plist, float(w), float(h));
            upload_ui_atlas(host.renderer, !ui_atlas_synced);
            ui_atlas_synced = true;
            host.renderer.set_ui(plist.items());
            for (int layer : {0, 1, 2, 3}) host.renderer.set_overlay({}, 0, 0, 0, 0, layer);
          } else if (replay_viewer.active() || replay_viewer.browser_open()) {
            ReplayViewer::set_text_resolver([](const std::string& ref) { return carview::game_text(ref); });
            replay_viewer.set_metric_units(app.metric_units);
            carview::ui::List rlist;
            replay_viewer.build_hud(rlist, float(w), float(h));
            upload_ui_atlas(host.renderer, !ui_atlas_synced);
            ui_atlas_synced = true;
            host.renderer.set_ui(rlist.items());
            for (int layer : {0, 1, 2, 3}) host.renderer.set_overlay({}, 0, 0, 0, 0, layer);
          } else if (achievements.screen_open()) {
            carview::ui::List alist;
            achievements.build_screen_hud(alist, float(w), float(h));
            achievements.build_hud_overlays(alist, float(w), float(h), false);
            upload_ui_atlas(host.renderer, !ui_atlas_synced);
            ui_atlas_synced = true;
            host.renderer.set_ui(alist.items());
            for (int layer : {0, 1, 2, 3}) host.renderer.set_overlay({}, 0, 0, 0, 0, layer);
          } else if (map_open) {
            // Smooth dt-integrated velocity panning, exponential zoom interpolation, and gentle magnetic snap
            // for keyboard arrows/WASD, gamepad D-pad, analog stick, and triggers.
            // The original's controls (map_cursor.h): the left stick, the
            // D-pad and the arrows move a free cursor that scrolls the map at
            // the screen's edge; the right stick zooms about it. The mouse
            // still drags the camera.
            double pan_x = 0.0, pan_z = 0.0, zoom_rate = 0.0;
            float cur_x = 0.0f, cur_y = 0.0f, zoom_axis = 0.0f;
            const bool* ks = SDL_GetKeyboardState(nullptr);
            if (ks && !map_filter_open) {
              if (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A]) cur_x -= 1.0f;
              if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D]) cur_x += 1.0f;
              if (ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_W]) cur_y -= 1.0f;
              if (ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_S]) cur_y += 1.0f;
              if (ks[SDL_SCANCODE_EQUALS] || ks[SDL_SCANCODE_KP_PLUS]) zoom_axis += 1.0f;
              if (ks[SDL_SCANCODE_MINUS] || ks[SDL_SCANCODE_KP_MINUS]) zoom_axis -= 1.0f;
            }
            for (SDL_Gamepad* pad : pads) {
              const double sx = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0, 0.16, 0.96);
              const double sy = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0, 0.16, 0.96);
              const double ry = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0, 0.2, 0.96);
              cur_x += float(sx);
              cur_y += float(sy);
              zoom_axis -= float(ry);
              if (!map_filter_open) {
                if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) cur_x -= 1.0f;
                if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) cur_x += 1.0f;
                if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) cur_y -= 1.0f;
                if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) cur_y += 1.0f;
              }
            }
            // A barn rumour's reveal moves the camera and holds the cursor.
            if (map_reveal_step(frame_dt)) cur_x = cur_y = zoom_axis = 0.0f;
            map_screen_step(frame_dt, std::clamp(cur_x, -1.0f, 1.0f), std::clamp(cur_y, -1.0f, 1.0f),
                            std::clamp(zoom_axis, -1.0f, 1.0f));
            const bool map_cam_active = !map_dragging && !map_reveal_locked &&
                                        (std::fabs(pan_x) > 1e-4 || std::fabs(pan_z) > 1e-4 || std::fabs(zoom_rate) > 1e-4 ||
                                         std::fabs(map_cam.vx()) > 1e-4 || std::fabs(map_cam.vz()) > 1e-4 ||
                                         std::fabs(map_cam.zoom() - map_cam.target_zoom()) > 1e-4);
            if (map_cam_active) {
              std::vector<MapMarker> vis_markers;
              vis_markers.reserve(cached_map_markers.size());
              for (const auto& cm : cached_map_markers) {
                if (map_marker_visible(cm)) vis_markers.push_back(cm);
              }
              if (map_cam.step(frame_dt, pan_x, pan_z, zoom_rate, vis_markers)) {
                map_cx = map_cam.cx();
                map_cz = map_cam.cz();
                map_zoom = map_cam.zoom();
                map_dirty = true;
              }
            }
            if (w != map_w || h != map_h) map_dirty = true;
            if (map_anim_due()) map_dirty = true;  // the icons' animations (30 Hz)
            if (map_dirty && w > 64 && h > 64) {
              map_dirty = false;
              map_w = w, map_h = h;
              // Reuse cached_map_markers while paused on the World Map instead of rebuilding
              // every career/outpost/event string via map_scene() on every pan frame.
              MapScene ms;
              if (!app.race && cached_map_markers.empty() && (app.have_career_catalog || app.have_outposts)) {
                ms = map_scene();
                cached_map_markers = ms.markers;
              } else {
                const drive::Vec3 at = vehicle.position();
                const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
                ms.x = at.x;
                ms.z = at.z;
                ms.heading = std::atan2(f.x, f.z);
                ms.speed_mps = std::fabs(vehicle.speed());
                ms.data = app.progress_loaded ? &app.collectibles : nullptr;
                ms.progress = app.progress_loaded ? &app.progress : nullptr;
                ms.road_discovery = app.road_discovery.ready() ? &app.road_discovery : nullptr;
                ms.reveal = app.reveal || app.treasure_map_setting || slice12.trainer.effective_reveal_collectibles();
                if (app.race) {
                  app.race->opponent_positions(ms.opponents);
                  app.race->route_near(ms.route);
                } else {
                  if (app.gps_nav.active()) {
                    app.gps_nav.remaining_polyline(ms.gps_route);
                    ms.has_gps_dest = true;
                    ms.gps_dest_x = app.gps_nav.route().snapped_dest_x;
                    ms.gps_dest_z = app.gps_nav.route().snapped_dest_z;
                  }
                  if (festival_mgr) {
                    for (const auto& fc : festival_mgr->cars()) ms.festival_ai.push_back({fc.position.x, fc.position.z});
                    if (festival_mgr->h2h_active()) ms.route = festival_mgr->h2h_route();
                  }
                  ms.markers = cached_map_markers;
                }
              }
              ms.filter = map_filter;
              update_map_selection();
              ms.selected_marker = map_selected_marker;
              map_anim_fill(ms);
              // The filter popup and the dialogs blur the map behind them.
              ms.blur = map_filter_open || map_dialog != MapDialog::kNone;
              // 100% edge-to-edge fullscreen viewport (0, 0, w, h) — zero outer borders!
              HudImage img = app.map_view.pause_map(ms, w, h, map_zoom, map_cx, map_cz);
              host.renderer.set_overlay(std::move(img.rgba), img.width, img.height, 0, 0, 2);
            }
            set_hud(w, h, "", nullptr);  // draws 1:1 H_MAP HUD overlay on UI layer!
          } else if (show_hud || app.show_title || ceremony.active || app.campaign.is_blocking_gameplay() ||
                     app.campaign.flow().dialogs().active() || app.fmv_player.active()) {
            set_minimap(h, false);
            char line[160] = "";
            const bool show_pick_banner = picking && pick < app.baked_cars.size();
            if (show_pick_banner)
              std::snprintf(line, sizeof line, "car %zu/%zu: %s - Enter drives it", pick + 1, app.baked_cars.size(),
                            carview::car_name(app.baked_cars[pick]).c_str());
            {
              // The career's cutscenes hold the radio popup with the HUD.
              const bool hold = app.campaign.ftc_active() &&
                                (app.campaign.is_blocking_gameplay() || !app.campaign.ftc().hud_visible());
              if (hold != radio_popup_held) {
                radio_popup_held = hold;
                app.audio_host.radio_hold_popup(hold);
              }
            }
            if (!app.show_title && !ceremony.active) set_radio_popup(h);
            set_hud(w, h,
                    show_pick_banner                 ? std::string(line)
                    : vehicle.time() < notice_until  ? notice
                    : app.race ? race_banner()
                                 : vehicle.time() < kIntroSeconds ? intro
                                                     : club_banner(),
                    &menu);
          }
          host.pump_deferred_world();
          vehicle.set_pose(stepped);
          host.renderer.set_camera_projection(host.cam_proj.near_z, host.cam_proj.far_z, host.cam_proj.proj00, host.cam_proj.proj11);
          host.renderer.set_camera_vp(host.cam_proj.vp_unjittered, w_jitter_uv_x, w_jitter_uv_y);
          const int active_brightness =
              (app.show_title && app.title.phase == carview::TitleMenu::Phase::kVideo) ? app.title.brightness
                                                                                       : app.display_brightness;
          host.renderer.set_display_brightness_offset(carview::Renderer::brightness_offset_for(active_brightness));
          app.fmv_player.set_subtitles_enabled(app.subtitles_enabled);
          host.renderer.set_skip_scene(map_overlay.active || app.fmv_player.active());
          static carview::AndroidPerfHintSession s_adpf;
          const int adpf_target =
              host.active_graphics.fps_cap > 0 ? host.active_graphics.fps_cap : std::max(60, host.display_hz);
          if (!s_adpf.active()) s_adpf.init(adpf_target);
          else s_adpf.update_target_fps(adpf_target);
          carview::set_android_refresh_rate_hz(host.active_graphics.fps_cap == 0 ? 120 : adpf_target);
          const auto wf_t0 = std::chrono::steady_clock::now();
          if (!host.renderer.draw_frame(FrameConstants{}, app.error)) {
            std::fprintf(stderr, "carview: %s\n", app.error.c_str());
            app.status = 1;
            return false;
          }
          const auto wf_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wf_t0).count();
          host.last_frame_cpu_ms = double(wf_ns) * 1e-6;
          s_adpf.report_frame_ns(wf_ns);
#ifdef __ANDROID__
          // Lightweight Dynamic Resolution Scaling (live windowed gameplay only; off in --bench/--headless).
          if (!app.headless && !app.bench && host.active_graphics.scale >= 0) {
            static const bool s_drs_enabled = [] {
              const char* e = std::getenv("SHIFT_ANDROID_DRS");
              return !(e && *e == '0');
            }();
            if (s_drs_enabled) {
              static double s_ema_ms = 0.0;
              static uint64_t s_drs_frames = 0;
              static uint64_t s_drs_last_change = 0;
              ++s_drs_frames;
              const double cur_ms = host.last_frame_cpu_ms;
              s_ema_ms = (s_drs_frames == 1) ? cur_ms : (s_ema_ms * 0.92 + cur_ms * 0.08);
              const double budget_ms = 1000.0 / double(std::max(30, adpf_target));
              if (s_drs_frames > 45 && (s_drs_frames - s_drs_last_change) >= 90) {
                const float cur_drs = host.renderer.drs_scale();
                float next_drs = cur_drs;
                if (s_ema_ms > budget_ms * 1.15 && cur_drs > 0.70f) {
                  next_drs = std::max(0.70f, cur_drs - 0.05f);
                } else if (s_ema_ms < budget_ms * 0.80 && cur_drs < 1.0f) {
                  next_drs = std::min(1.0f, cur_drs + 0.05f);
                }
                if (std::fabs(next_drs - cur_drs) > 0.005f) {
                  host.renderer.set_drs_scale(next_drs);
                  s_drs_last_change = s_drs_frames;
                  std::fprintf(stderr, "[drs] scale %.2f -> %.2f (ema=%.2f ms, budget=%.2f ms)\n",
                               double(cur_drs), double(next_drs), s_ema_ms, budget_ms);
                }
              }
            }
          }
#endif
          const double now_s =
              std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
          const double wait_s = host.frame_pacer.wait_for(now_s);
          if (wait_s > 0.0005) SDL_DelayNS(Uint64(wait_s * 1e9));
          app.frame_stats.frame(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
          if (host.renderer.active_frame_gen() != carview::modern::FrameGen::kOff) app.frame_stats.generated(1);
          if (app.window_frames_limit && app.frame_stats.frames() >= app.window_frames_limit) {
            running = false;
            const auto vram = host.renderer.vram_stats();
            std::printf("window: %s\nwindow: vram %.1f MiB allocated in %u allocs, %.1f MiB device-local in use\n",
                        app.frame_stats.summary().c_str(), double(vram.allocation_bytes) / (1024.0 * 1024.0),
                        vram.allocation_count, double(vram.device_local_usage) / (1024.0 * 1024.0));
          }
          // --hub-shot (a harness): open the room's first screen once free roam
          // is up, capture it after the delay and quit (with a round trip:
          // close it, capture free roam, open it again, capture, quit).
          // Stages: 0 open, 1 wait, 2 room captured, 3 wait in free roam,
          // 4 free roam captured.
          static int s_hub_shot_stage = 0, s_hub_shot_pass = 0;
          static uint64_t s_hub_shot_frames = 0;
          static std::chrono::steady_clock::time_point s_hub_shot_t0;
          if (!app.cli_hub_shot_room.empty() && !app.show_title && !app.fmv_player.active() &&
              pending_shot == PendingShot::kNone) {
            ++s_hub_shot_frames;
            const std::string& r = app.cli_hub_shot_room;
            const bool due = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_hub_shot_t0).count() >=
                             app.cli_hub_shot_delay;
            if (s_hub_shot_stage == 0 && s_hub_shot_frames > 90) {
              if (r == "autoshow") open_shop_or_autoshow(true);
              else if (r == "upgrade") open_shop_or_autoshow(false);
              else if (r == "workshop") open_hub_menu(carview::HubMenu::kWorkshop);
              else if (r == "paintshop") open_hub_menu(carview::HubMenu::kPaintBooth);
              else if (r == "paint") open_paint();
              else if (r == "garage") open_garage_browser(false);
              else if (r == "workshop-garage") {  // the workshop's CHANGE CAR
                open_hub_menu(carview::HubMenu::kWorkshop);
                open_garage_browser(false);
              }
              else if (r == "profile") open_hub_menu(carview::HubMenu::kProfile);
              else if (r == "options") open_hub_menu(carview::HubMenu::kOptions);
              else if (r == "photo") open_hub_menu(carview::HubMenu::kPhotoSelect);
              else if (r == "difficulty") open_options_page(carview::OptionsPage::kDifficulty);
              else if (r == "hud") open_options_page(carview::OptionsPage::kHud);
              else if (r == "controls") open_options_page(carview::OptionsPage::kControls);
              else if (r == "stats") open_stats();
              s_hub_shot_stage = 1;
              s_hub_shot_t0 = std::chrono::steady_clock::now();
            } else if ((s_hub_shot_stage == 1 || s_hub_shot_stage == 3) && due) {
              pending_shot = PendingShot::kHubShot;
              host.renderer.request_capture(true);
              ++s_hub_shot_stage;
            }
          }
          // Process real Vulkan swapchain / color-target PNG captures (F8, F12, Photo Mode 4x SSAA).
          if (pending_shot != PendingShot::kNone) {
            std::vector<uint8_t> cap_rgba;
            uint32_t cap_w = 0, cap_h = 0;
            if (host.renderer.take_captured_frame(cap_rgba, cap_w, cap_h) && pending_shot == PendingShot::kHubShot) {
              for (size_t i = 3; i < cap_rgba.size(); i += 4) cap_rgba[i] = 255;
              std::filesystem::path png = app.cli_hub_shot_png;
              const char* suffix = s_hub_shot_stage == 4 ? "_roam" : s_hub_shot_pass > 0 ? "_again" : "";
              if (*suffix) png.replace_filename(png.stem().string() + suffix + png.extension().string());
              const bool ok = write_png(png.string(), cap_w, cap_h, 4, cap_rgba.data());
              std::printf("hub shot %s -> %s (%ux%u)%s\n", app.cli_hub_shot_room.c_str(), png.string().c_str(), cap_w,
                          cap_h, ok ? "" : " FAILED");
              pending_shot = PendingShot::kNone;
              if (app.cli_hub_shot_round_trip && s_hub_shot_pass == 0 && s_hub_shot_stage == 2) {
                // Out of the room: every hub screen closed.
                hub_menu.close();
                if (autoshow.is_open()) autoshow.close();
                if (shop.is_open()) shop.close();
                s_hub_shot_stage = 3;
                s_hub_shot_t0 = std::chrono::steady_clock::now();
              } else if (s_hub_shot_stage == 4) {
                s_hub_shot_stage = 0;  // the room again
                s_hub_shot_pass = 1;
              } else {
                running = false;
              }
            } else if (!cap_rgba.empty()) {
              if (pending_shot == PendingShot::kPhotoMode && photo_mode.accumulator().active()) {
                photo_mode.accumulator().add_frame(cap_rgba, cap_w, cap_h);
                if (photo_mode.accumulator().complete()) {
                  std::vector<uint8_t> final_rgba;
                  uint32_t fw = 0, fh = 0;
                  if (photo_mode.accumulator().resolve(final_rgba, fw, fh)) {
                    std::filesystem::path saved_png;
                    std::string werr;
                    if (carview::write_photo_png(photos_dir, final_rgba, fw, fh, "photo", saved_png, &werr,
                                                 photo_mode.effects().watermark)) {
                      float cfw[16];
                      compute_clip_from_world(cam_frame, info, vehicle, cfw);
                      const auto promo =
                          promo_tracker.evaluate(cfw, photo_mode.camera().eye(), gather_promo_candidates(), true);
                      if (promo.credits_awarded > 0) {
                        const int64_t pcr = slice12.trainer.pacing.scale_credits(promo.credits_awarded);
                        app.career.credits += pcr;
                        app.career.total_winnings += pcr;
                        save_now("horizon promo");
                      }
                      photo_mode.notify_photo_saved(saved_png, promo);
                    }
                  }
                  pending_shot = PendingShot::kNone;
                } else {
                  host.renderer.request_capture(false);
                }
              } else {
                const std::string pfx = pending_shot == PendingShot::kF12 ? "pose" : "shot";
                std::filesystem::path saved_png;
                std::string werr;
                if (carview::write_photo_png(photos_dir, cap_rgba, cap_w, cap_h, pfx, saved_png, &werr)) {
                  notice = "PHOTO SAVED: " + saved_png.filename().string();
                  notice_until = vehicle.time() + 3.5;
                } else {
                  notice = "PHOTO FAILED: " + werr;
                  notice_until = vehicle.time() + 3.5;
                }
                pending_shot = PendingShot::kNone;
              }
            }
          }
  return true;
}

void DriveSession::update_driving_line_overlay() {
  struct DrivingLineOverlayState {
    drive::DrivingLineSmoother speed_smoother;
    double last_time_s = -1.0;
    bool last_race = false;
    drive::DrivingLineMode last_mode = drive::DrivingLineMode::kOff;
  };
  static std::unordered_map<const DriveSession*, DrivingLineOverlayState> dl_states;
  auto& dl_state = dl_states[this];

  const bool want_race_dl = app.race && !grid_menu_open && !app.race->player_finished() &&
                            driving_line_mode != drive::DrivingLineMode::kOff;
  const bool want_gps_dl = !app.race && !app.show_title && !app.fmv_player.active() &&
                           driving_line_mode != drive::DrivingLineMode::kOff &&
                           app.gps_nav.active() && app.gps_nav.route().valid &&
                           app.gps_nav.route().points.size() >= 2;
  if (!want_race_dl && !want_gps_dl) {
    dl_state.speed_smoother.reset();
    dl_state.last_time_s = -1.0;
    host.showroom.set_world_overlay({});
    return;
  }

  const double now_t = vehicle.time();
  if (dl_state.last_race != want_race_dl || dl_state.last_mode != driving_line_mode ||
      (dl_state.last_time_s >= 0.0 && now_t < dl_state.last_time_s - 0.01)) {
    dl_state.speed_smoother.reset();
  }
  const double dt_s =
      (dl_state.last_time_s >= 0.0 && now_t >= dl_state.last_time_s) ? (now_t - dl_state.last_time_s) : 0.0;
  dl_state.last_time_s = now_t;
  dl_state.last_race = want_race_dl;
  dl_state.last_mode = driving_line_mode;

  const double display_speed = dl_state.speed_smoother.update(std::max(0.0, vehicle.speed()), dt_s);
  drive::DrivingLineOptions dl_opt{};

  if (want_race_dl) {
    drive::build_driving_line(app.race->racing_line(), braking_table, app.race->player_progress(),
                              display_speed, driving_line_mode, dl_samples, dl_opt);
  } else {
    const auto& gr = app.gps_nav.route();
    const double prog = app.gps_nav.guidance().route_progress_m;
    const double win_lo = std::max(0.0, prog - 25.0);
    const double win_hi = prog + 320.0;
    std::vector<float> gx, gz;
    std::vector<double> gy;
    double slice_start_s = 0.0;
    bool have_start = false;
    const size_t npts = gr.points.size();
    for (size_t i = 0; i < npts; ++i) {
      const double si = (i < gr.s.size()) ? gr.s[i] : 0.0;
      if (si < win_lo && i + 1 < npts && ((i + 1 < gr.s.size()) ? gr.s[i + 1] : 0.0) < win_lo) continue;
      if (!have_start) {
        slice_start_s = si;
        have_start = true;
      }
      gx.push_back(float(gr.points[i].x));
      gy.push_back(gr.points[i].y);
      gz.push_back(float(gr.points[i].z));
      if (si > win_hi && gx.size() >= 3) break;
    }
    if (gx.size() >= 2) {
      drive::RacingLine gps_line = drive::RacingLine::from_points(gx, gz);
      if (gps_line.y.size() == gy.size()) {
        for (size_t i = 0; i < gy.size(); ++i) gps_line.y[i] = float(gy[i]);
      }
      const double local_prog = std::clamp(prog - slice_start_s, 0.0, gps_line.length());
      drive::build_driving_line(gps_line, braking_table, local_prog,
                                display_speed, driving_line_mode, dl_samples, dl_opt);
    } else {
      dl_samples.clear();
    }
  }
  // Snap visible sample heights onto the live collision/road surface so the ribbon
  // hugs hills and crowns crisply without dipping under asphalt or floating.
  if (host.ground && !dl_samples.empty()) {
    const double car_y = vehicle.position().y;
    const size_t n = dl_samples.size();
    for (size_t i = 0; i < n; ++i) {
      const bool needed = dl_samples[i].color.a > 0 ||
                          (i > 0 && dl_samples[i - 1].color.a > 0) ||
                          (i + 1 < n && dl_samples[i + 1].color.a > 0);
      if (!needed) continue;
      auto& s = dl_samples[i];
      const double probe_y = (std::fabs(double(s.y)) > 1e-3 ? double(s.y) : car_y) + 6.0;
      drive::Hit hit;
      if (host.ground->raycast({double(s.x), probe_y, double(s.z)}, {0.0, -1.0, 0.0}, 18.0, hit)) {
        s.y = float(probe_y - hit.distance);
      }
    }
  }
  drive::build_driving_line_mesh(dl_samples, dl_opt.half_width_m, dl_opt.lift_m, dl_verts);
  overlay_verts.clear();
  overlay_verts.reserve(dl_verts.size());
  const double ox = host.showroom.world_origin_x();
  const double oz = host.showroom.world_origin_z();
  for (const auto& dv : dl_verts) {
    carview::WorldVertex wv{};
    wv.position[0] = float(double(dv.x) - ox);
    wv.position[1] = dv.y;
    wv.position[2] = float(double(dv.z) - oz);
    wv.normal[1] = 127;
    wv.colour[0] = dv.r;
    wv.colour[1] = dv.g;
    wv.colour[2] = dv.b;
    wv.colour[3] = dv.a;
    overlay_verts.push_back(wv);
  }
  host.showroom.set_world_overlay(overlay_verts);
}

}  // namespace carview
