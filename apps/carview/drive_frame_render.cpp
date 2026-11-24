#include "drive_session.h"

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
          const audio::View cur_audio_view = audio_view();
          const bool any_menu_snapshot = menu_paused && !app.show_title && !app.fmv_player.active() && !photo_mode.active() &&
                                         !replay_viewer.active() && !trainer_freecam_active;
          app.audio_host.update_frame(sound_inputs(cur_audio_view),
                                  menu_paused || rewind_buf.is_rewinding() || rewind_buf.is_paused_at_keyframe(),
                                  any_menu_snapshot, app.show_title, false);
          {
            audio::VehicleAudioState vst{};
            const auto& vp = vehicle.params();
            const drive::Vec3 vpos = vehicle.position();
            const drive::Vec3 vvel = vehicle.velocity();
            const drive::Vec3 vright = drive::rotate(vehicle.orientation(), {1, 0, 0});
            vst.speed_mph = float(std::fabs(vehicle.speed()) * 2.2369363);
            vst.top_speed_mph = float(std::clamp(
                double(drive::CockpitAnimState::estimate_dial_max_speed_mps(vehicle.params())) * 2.2369363, 90.0,
                280.0));
            vst.rpm = float(vehicle.rpm());
            vst.idle_rpm = float(vp.idle_rpm);
            vst.redline_rpm = float(vp.redline_rpm);
            vst.throttle = float(vehicle.engine_throttle());
            vst.gear = vehicle.gear();
            vst.shifting = vehicle.shifting();
            vst.clutch_pos = float(1.0 - vehicle.clutch());
            vst.forced_induction = vp.has_turbo ? 1 : (vp.has_supercharger ? 2 : 0);
            vst.turbo_psi = float(vehicle.boost_psi());
            vst.turbo_norm_rpm = vp.has_turbo && vp.boost_max_psi > 1.0
                                     ? float(std::clamp(vehicle.boost_psi() / vp.boost_max_psi, 0.0, 1.2))
                                     : 0.0f;
            vst.bov_open = vehicle.bov_open();
            vst.supercharger_norm_rpm = vp.has_supercharger && vp.redline_rpm > 1.0
                                            ? float(std::clamp(vehicle.rpm() / vp.redline_rpm, 0.0, 1.2))
                                            : 0.0f;
            vst.trans_level = audio_trans_level;
            vst.turbo_level = audio_turbo_level;
            vst.sc_level = audio_sc_level;
            vst.backfire_level = std::max(audio_upgrades.exhaust, vp.has_turbo ? 1 : 0);
            const auto& body_c = vehicle.strongest_body_contact();
            vst.scraping = body_c.force > 150.0 && vst.speed_mph > 4.0f;
            vst.scrape_intensity = vst.scraping ? float(std::clamp(body_c.force / 20000.0, 0.2, 1.0)) : 0.0f;
            const auto& ws = vehicle.wheels();
            const auto& surfs = host.collision.surfaces();
            for (int wi = 0; wi < 4; ++wi) {
              auto& wa = vst.wheels[size_t(wi)];
              const auto& wstate = ws[size_t(wi)];
              wa.slip_combined = float(std::hypot(wstate.slip_ratio, wstate.slip_angle));
              wa.wheel_rpm = float(wstate.omega * 9.5492966);
              const double sload = std::max(100.0, vehicle.static_load(wi));
              wa.load = wstate.contact ? float(std::clamp(wstate.load / sload, 0.0, 2.0)) : 0.0f;
              const int axle = wi < 2 ? 0 : 1;
              if (wstate.contact && wstate.compression > vp.bump_travel[size_t(axle)] * 0.92)
                wa.bottom_out_force = float(wstate.compression - vp.bump_travel[size_t(axle)] * 0.92);
              uint8_t sgrp = 0;
              if (wstate.surface >= 0 && size_t(wstate.surface) < surfs.size()) {
                const auto& sf = surfs[size_t(wstate.surface)];
                if (sf.off_roadness > 0.5f) sgrp = 2;
                else if (sf.off_roadness > 0.05f) sgrp = 1;
              }
              wa.surface_group = sgrp;
            }
            vst.listener_pos = {float(vpos.x), float(vpos.y), float(vpos.z)};
            vst.listener_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
            vst.listener_right = {float(vright.x), float(vright.y), float(vright.z)};
            vst.cockpit_view = cur_audio_view == audio::View::kCockpit;
            vst.in_race = app.race != nullptr;
            vst.rewinding = rewind_buf.is_rewinding();
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
              const bool aero_car = (vp.aero_df_rear + vp.aero_part_df_rear) > 0.35;
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
            struct Cand {
              double d2 = 0;
              audio::OtherCarAudio car{};
            };
            std::vector<Cand> cands;
            if (app.race) {
              std::vector<carview::RaceView::OpponentTag> otags;
              app.race->opponent_tags(otags);
              for (size_t oi = 0; oi < otags.size(); ++oi) {
                const auto& ot = otags[oi];
                const double dx = ot.x - vpos.x, dy = ot.y - vpos.y, dz = ot.z - vpos.z;
                const double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > 220.0 * 220.0) continue;
                audio::OtherCarAudio oc{};
                oc.active = true;
                oc.is_traffic = false;
                oc.variant = uint8_t(oi & 3u);
                oc.pos = {float(ot.x), float(ot.y), float(ot.z)};
                oc.vel = {float(ot.vx), float(ot.vy), float(ot.vz)};
                oc.rpm = float(ot.rpm);
                oc.speed_mph = float(std::fabs(ot.speed) * 2.2369363);
                cands.push_back({d2, oc});
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
                oc.pos = {float(tc.position.x), float(tc.position.y), float(tc.position.z)};
                oc.vel = {float(std::sin(tc.heading) * tc.speed), 0.0f, float(std::cos(tc.heading) * tc.speed)};
                oc.rpm = float(1100.0 + std::fabs(tc.speed) * 90.0);
                oc.speed_mph = float(std::fabs(tc.speed) * 2.2369363);
                cands.push_back({d2, oc});
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
                oc.variant = uint8_t(fc.id & 3u);
                oc.pos = {float(fc.position.x), float(fc.position.y), float(fc.position.z)};
                oc.vel = {float(fc.vx), float(fc.vy), float(fc.vz)};
                oc.rpm = float(fc.rpm);
                oc.speed_mph = float(std::fabs(fc.speed) * 2.2369363);
                cands.push_back({d2, oc});
              }
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            const size_t n_other = std::min(cands.size(), size_t(audio::VehicleAudioState::kMaxOtherCars));
            for (size_t oi = 0; oi < n_other; ++oi) vst.other_cars[oi] = cands[oi].car;

            bool in_countdown = false;
            int countdown_digit = -1;
            if (app.race) {
              const auto rh = app.race->hud_info();
              in_countdown = rh.phase == carview::RaceHud::kCountdown;
              countdown_digit = rh.digit;
            }
            app.audio_host.update_vehicle(vst, app.race != nullptr, rewind_buf.is_rewinding(), in_countdown, countdown_digit);
          }
          int w = 0, h = 0;
          SDL_GetWindowSizeInPixels(app.window, &w, &h);
          // Pose car & camera BEFORE building projected world HUD markers and opponent name tags!
          const drive::Vehicle::Pose stepped = vehicle.pose();
          const bool rewind_scrub_view = rewind_buf.is_rewinding() || rewind_buf.is_paused_at_keyframe();
          if (replay_viewer.active()) {
            if (const auto* fc = replay_viewer.focused_car()) {
              drive::Vehicle::Pose rp = stepped;
              fc->apply_to_pose(rp);
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
          tod_cockpit_cam = have_rig && rig.mode() == drive::CameraMode::kCockpit;
          place_world();
          // 3D Driving / Braking Line ribbon on the road (SPEC-CAR-PHYSICS).
          if (app.race && !grid_menu_open && !app.race->player_finished() &&
              driving_line_mode != drive::DrivingLineMode::kOff) {
            drive::build_driving_line(app.race->racing_line(), braking_table, app.race->player_progress(),
                                      std::max(0.0, vehicle.speed()), driving_line_mode, dl_samples);
            drive::build_driving_line_mesh(dl_samples, 0.55, 0.08, dl_verts);
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
          } else {
            host.showroom.set_world_overlay({});
          }
          const float aspect = h > 0 ? float(w) / float(h) : 1.0f;
          drive::CameraView rw_cam{};
          drive::CameraView rv_cam{};
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
          }
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
          ShowroomFrame cam_frame =
              photo_mode.active()
                  ? camera_view_frame(photo_mode.camera().view(), info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                  : trainer_freecam_active
                        ? camera_view_frame(fc_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                        : replay_viewer.active()
                        ? camera_view_frame(rv_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                        : app.show_title
                        ? scenic_orbit_frame(info, vehicle, aspect,
                                             std::atan2(drive::rotate(vehicle.orientation(), {0, 0, 1}).x,
                                                        drive::rotate(vehicle.orientation(), {0, 0, 1}).z) +
                                                 0.58 + 0.34 * std::sin(title_orbit_time * 0.32),
                                             5.8f, 1.48f, host.renderer.reverse_z(), host.ground, &host.cam_proj)
                        : camp_cam_active
                        ? camera_view_frame(camp_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
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
          if (photo_mode.active() || replay_viewer.active() || app.show_title || camp_cam_active ||
              (app.race && grid_menu_open) || ceremony.active || rewind_scrub_view)
            host.showroom.set_draw_car(!replay_viewer.active() || replay_viewer.camera_mode() != carview::ReplayCameraMode::kBumper);
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
                world_marker.prompt = elig.eligible ? "ENTER NOW" : "INELIGIBLE";
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
            }
          }
          // Update projected Opponent Overhead Name Tags in races (race-hud.png),
          // Airborne Showcases, and free-roam Festival AI racers.
          name_tags.clear();
          if (app.race && !grid_menu_open && !app.race->player_finished() && w > 0 && h > 0) {
            std::vector<carview::RaceView::OpponentTag> otags;
            app.race->opponent_tags(otags);
            const drive::Vec3 at = vehicle.position();
            for (const auto& ot : otags) {
              const double d = std::hypot(ot.x - at.x, ot.z - at.z);
              if (d > 110.0) continue;
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, ot.x, ot.y + 1.85, ot.z, float(w), float(h), sx, sy))
                name_tags.push_back({sx, sy, ot.name, ot.leader});
            }
            carview::RaceView::OpponentTag stag;
            if (app.race->showcase_tag(stag)) {
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, stag.x, stag.y, stag.z, float(w), float(h), sx, sy))
                name_tags.push_back({sx, sy, stag.name, stag.leader});
            }
          } else if (!app.race && festival_mgr && w > 0 && h > 0) {
            const drive::Vec3 at = vehicle.position();
            for (const auto& fc : festival_mgr->cars()) {
              if (std::hypot(fc.position.x - at.x, fc.position.z - at.z) > 120.0) continue;
              float sx = 0, sy = 0;
              if (project_world_point(cam_frame, info, vehicle, fc.position.x, fc.position.y + 1.85, fc.position.z,
                                      float(w), float(h), sx, sy))
                name_tags.push_back({sx, sy, "[" + fc.spec.car_class + " " + std::to_string(fc.spec.pi) + "] " + fc.spec.driver_name,
                                     fc.in_h2h});
            }
          }

          if (photo_mode.active()) {
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
            double pan_x = 0.0, pan_z = 0.0, zoom_rate = 0.0;
            const bool* ks = SDL_GetKeyboardState(nullptr);
            if (ks) {
              if (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A]) pan_x -= 1.0;
              if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D]) pan_x += 1.0;
              if (ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_W]) pan_z += 1.0;
              if (ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_S]) pan_z -= 1.0;
              if (ks[SDL_SCANCODE_EQUALS] || ks[SDL_SCANCODE_KP_PLUS]) zoom_rate += 0.85;
              if (ks[SDL_SCANCODE_MINUS] || ks[SDL_SCANCODE_KP_MINUS]) zoom_rate -= 0.85;
            }
            for (SDL_Gamepad* pad : pads) {
              const double sx = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0, 0.16, 0.96);
              const double sy = drive::apply_deadzone_signed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0, 0.16, 0.96);
              const double z_in = std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0);
              const double z_out = std::max(0.0, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0);
              pan_x += sx;
              pan_z -= sy;
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) pan_x -= 1.0;
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) pan_x += 1.0;
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) pan_z += 1.0;
              if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) pan_z -= 1.0;
              if (z_in > 0.06 || z_out > 0.06) zoom_rate += (z_in - z_out);
            }
            const bool map_cam_active = !map_dragging &&
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
          const auto wf_t0 = std::chrono::steady_clock::now();
          if (!host.renderer.draw_frame(FrameConstants{}, app.error)) {
            std::fprintf(stderr, "carview: %s\n", app.error.c_str());
            app.status = 1;
            return false;
          }
          host.last_frame_cpu_ms =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wf_t0).count();
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
          // Process real Vulkan swapchain / color-target PNG captures (F8, F12, Photo Mode 4x SSAA).
          if (pending_shot != PendingShot::kNone) {
            std::vector<uint8_t> cap_rgba;
            uint32_t cap_w = 0, cap_h = 0;
            if (host.renderer.take_captured_frame(cap_rgba, cap_w, cap_h)) {
              if (pending_shot == PendingShot::kPhotoMode && photo_mode.accumulator().active()) {
                photo_mode.accumulator().add_frame(cap_rgba, cap_w, cap_h);
                if (photo_mode.accumulator().complete()) {
                  std::vector<uint8_t> final_rgba;
                  uint32_t fw = 0, fh = 0;
                  if (photo_mode.accumulator().resolve(final_rgba, fw, fh)) {
                    std::filesystem::path saved_png;
                    std::string werr;
                    if (carview::write_photo_png(photos_dir, final_rgba, fw, fh, "photo", saved_png, &werr)) {
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

}  // namespace carview
