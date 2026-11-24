#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "hub_scene.h"

namespace carview {

bool DriveSession::run_demo_captures() {
      if (!app.demo_photo_png.empty()) {
        // Headless Slice 10 verification capture: Photo Mode UI + DoF/vignette + Horizon Promo.
        const drive::Vec3 pat = vehicle.position();
        const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
        const double phead = std::atan2(pf.x, pf.z);
        photo_mode.enter({pat.x, pat.y, pat.z}, phead,
                         carview::PhotoLimits::from_settings(live_photo_through_walls, true));
        if (host.renderer.active_rt_mode() == carview::rt::RtMode::kPhotoModePt) {
          photo_mode.effects().apply_preset(carview::PhotoPreset::kDefault);
          photo_mode.effects().aperture = 18.0f;
          photo_mode.effects().vignette = 10.0f;
          photo_mode.effects().shutter_speed = 0.0f;
          photo_mode.effects().exposure = 58.0f;
        } else {
          photo_mode.effects().apply_preset(carview::PhotoPreset::kHorizonSunset);
          photo_mode.effects().aperture = 68.0f;
          photo_mode.effects().vignette = 42.0f;
          photo_mode.effects().shutter_speed = 45.0f;
        }
        photo_mode.autofocus({pat.x, pat.y + 0.65, pat.z});
        wheel_poses(vehicle, poses);
        const float wboost = photo_mode.effects().wheel_blur_omega_boost();
        for (int wi = 0; wi < 4; ++wi) poses[wi].omega += wboost;
        host.showroom.set_wheel_poses(poses);
        host.showroom.set_braking(0.35f);
        host.showroom.set_draw_car(true);
        tod_cockpit_cam = false;
        place_world();
        const float aspect = float(app.width) / float(app.height);
        const ShowroomFrame pframe =
            camera_view_frame(photo_mode.camera().view(), info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj);
        host.showroom.set_frame(pframe);
        float cfw[16];
        compute_clip_from_world(pframe, info, vehicle, cfw);
        const auto promo_res = promo_tracker.evaluate(cfw, photo_mode.camera().eye(), gather_promo_candidates(), true);
        photo_mode.notify_photo_saved(app.demo_photo_png, promo_res);
        photo_mode.step(0.4, {});  // let shutter flash settle while keeping the Horizon Promo banner visible
        const bool photo_rt_on = host.renderer.active_rt_mode() != carview::rt::RtMode::kOff;
        if (photo_rt_on && host.renderer.active_rt_mode() == carview::rt::RtMode::kPhotoModePt) {
          photo_mode.toggle_ui_hidden();
        }
        carview::ui::List plist;
        photo_mode.build_hud(plist, float(app.width), float(app.height));
        upload_ui_atlas(host.renderer, !ui_atlas_synced);
        ui_atlas_synced = true;
        host.renderer.set_ui(plist.items());
        host.renderer.set_camera_projection(host.cam_proj.near_z, host.cam_proj.far_z, host.cam_proj.proj00, host.cam_proj.proj11);
        host.renderer.set_camera_vp(pframe.view_projection, 0.0f, 0.0f);
        if (photo_rt_on) {
          host.renderer.set_photo_pt_accumulate(true, true);
          const int spp_per_frame = std::max(1, host.renderer.graphics_settings().rt_spp);
          const int accum_frames = std::clamp(64 / spp_per_frame, 8, 64);
          for (int ai = 0; ai < accum_frames - 1; ++ai) {
            double rec_ms = 0.0, gpu_ms = 0.0;
            if (!host.renderer.render_timed(FrameConstants{}, app.width, app.height, rec_ms, gpu_ms, app.error)) break;
          }
          std::printf("photo mode PT: accumulated %d frames x %d spp = %d spp\n", accum_frames, spp_per_frame,
                      accum_frames * spp_per_frame);
        }
        std::vector<uint8_t> rgba;
        if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
          std::fprintf(stderr, "carview: %s\n", app.error.c_str());
          app.status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(app.demo_photo_png.string(), app.width, app.height, 4, rgba.data())) app.status = 1;
          else std::printf("demo photo mode -> %s (spotted %d/%d, +%lld Cr)\n", app.demo_photo_png.string().c_str(),
                           promo_res.total_spotted, promo_res.total_cars, (long long)promo_res.credits_awarded);
        }
        if (photo_rt_on) host.renderer.set_photo_pt_accumulate(false, true);
        photo_mode.exit();
      } else if (!app.demo_replay_png.empty()) {
        // Headless Slice 10 verification capture: Replay Viewer HUD with timeline scrubber, telemetry & track-side camera.
        drive::ReplayMetadata rm;
        rm.event_name = "RED ROCK CANYON SPRINT";
        rm.world_name = "Colorado";
        rm.tod_sec = app.tod_clock.tod_sec;
        rm.sample_hz = 60.0;
        rm.is_race = true;
        rm.player_place = 1;
        rm.field_size = 8;
        rm.roster.push_back({0u, int32_t(info.car_id), app.car, "PLAYER - " + carview::car_name(app.car), true, false});
        replay_rec.begin(rm, 600, false);
        replay_rec.add_marker(0.0, "START");
        // Drive forward for 4.5 seconds on Colorado road to capture genuine physics motion + speed.
        const int demo_steps = int(std::lround(4.5 / drive::Vehicle::kStep));
        for (int ds = 0; ds <= demo_steps; ++ds) {
          const drive::Input di = ds == 0 ? drive::Input{} : (app.cruise ? host.follower.input(vehicle) : drive::Input{0.0, 0.92, 0.0, false});
          if (ds > 0 && !step(di)) break;
          std::vector<drive::ReplayCarSample> frame_cars;
          frame_cars.push_back(drive::ReplayCarSample::from_vehicle(0u, vehicle, player_lamps, di.throttle, di.brake,
                                                                    di.handbrake ? 1.0 : 0.0, di.steer, 1));
          replay_rec.maybe_record(vehicle.time(), std::move(frame_cars));
          if (ds == demo_steps / 2) replay_rec.add_marker(vehicle.time(), "SECTOR 1");
        }
        replay_rec.add_marker(vehicle.time(), "FINISH");
        replay_viewer.open(replay_rec.snapshot(), true);
        replay_viewer.set_camera_mode(carview::ReplayCameraMode::kTrackSide);
        replay_viewer.set_telemetry(true);
        replay_viewer.seek(replay_viewer.duration() * 0.62);
        replay_viewer.set_speed(0.5);
        // Apply sampled pose & wheel spin for rendering.
        if (const auto* fc = replay_viewer.focused_car()) {
          drive::Vehicle::Pose rp = vehicle.pose();
          fc->apply_to_pose(rp);
          vehicle.set_pose(rp);
          wheel_poses(vehicle, poses);
          for (int wi = 0; wi < 4; ++wi) poses[wi].omega = fc->wheel_omega[wi];
          host.showroom.set_wheel_poses(poses);
          host.showroom.set_braking((fc->lamp_bits & drive::kReplayLampBrake) ? 1.0f : 0.0f);
        }
        host.showroom.set_draw_car(true);
        tod_cockpit_cam = false;
        place_world();
        auto gb = [&](double x, double y, double z, double& gy) {
          drive::Hit hit;
          if (!host.ground || !host.ground->raycast({x, y, z}, {0, -1, 0}, 80.0, hit)) return false;
          gy = y - hit.distance;
          return true;
        };
        const drive::CameraView rv_cam = replay_viewer.compute_camera(0.016, 0.0f, 0.0f, 0.0f, gb);
        const float aspect = float(app.width) / float(app.height);
        host.showroom.set_frame(camera_view_frame(rv_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj));
        carview::ui::List rlist;
        replay_viewer.build_hud(rlist, float(app.width), float(app.height));
        upload_ui_atlas(host.renderer, !ui_atlas_synced);
        ui_atlas_synced = true;
        host.renderer.set_ui(rlist.items());
        std::vector<uint8_t> rgba;
        if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
          std::fprintf(stderr, "carview: %s\n", app.error.c_str());
          app.status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(app.demo_replay_png.string(), app.width, app.height, 4, rgba.data())) app.status = 1;
          else std::printf("demo replay view -> %s (%.1fs, %zu frames)\n", app.demo_replay_png.string().c_str(),
                           replay_viewer.duration(), replay_viewer.recording().frame_count());
        }
        replay_viewer.close();
      } else if (!app.demo_achievements_png.empty()) {
        // Headless Slice 10 verification capture: F7 Achievements screen with wristband header, tabs, progress bars & HUD unlock toast.
        place_world();
        const float aspect = float(app.width) / float(app.height);
        host.showroom.set_frame(drive_frame(aspect, 0.016));
        carview::AchievementGameplayState ds{};
        ds.career_started = true;
        ds.wristbands_earned = 3;
        ds.num_races = 22;
        ds.num_victories = 14;
        ds.num_podiums = 18;
        ds.street_races_won = 6;
        ds.festival_races_won = 8;
        ds.discount_signs_found = 78;
        ds.barns_found = 6;
        ds.roads_discovered = 184;
        ds.outposts_discovered = 6;
        ds.pr_stunts_completed = 9;
        ds.garage_cars = 12;
        ds.promo_cars_spotted = 34;
        ds.total_distance_m = 420000.0;
        ds.top_speed_mps = 102.0;
        ds.total_winnings = 1650000;
        ds.max_skills_score = 32500;
        ds.popularity_points = 185000;
        ds.driver_level = 25;
        ds.has_rally = true;
        ds.rally_stages_done = 5;
        ds.rally_victories = 2;
        ds.rally_level = 3;
        ds.club_medals_total = 14;
        ds.club_cars_with_medal = 5;
        achievements.evaluate(ds, false);
        achievements.set_tracked_id(194);
        achievements.open_screen();
        achievements.cycle_sort();  // NEAREST sort so tracked/in-progress achievement is focused in detail panel!
        achievements.unlock(179, 0, true);  // live toast showcase
        for (int ti = 0; ti < 12; ++ti) achievements.update(0.1);  // finish count-up & toast slide-in
        carview::ui::List alist;
        achievements.build_screen_hud(alist, float(app.width), float(app.height));
        achievements.build_hud_overlays(alist, float(app.width), float(app.height), false);
        upload_ui_atlas(host.renderer, !ui_atlas_synced);
        ui_atlas_synced = true;
        host.renderer.set_ui(alist.items());
        std::vector<uint8_t> rgba;
        if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
          std::fprintf(stderr, "carview: %s\n", app.error.c_str());
          app.status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(app.demo_achievements_png.string(), app.width, app.height, 4, rgba.data())) app.status = 1;
          else std::printf("demo achievements -> %s (%d/%d unlocked, %u/%uG, spa=%zu)\n",
                           app.demo_achievements_png.string().c_str(), achievements.unlocked_count(),
                           achievements.total_count(), achievements.earned_gamerscore(),
                           achievements.available_gamerscore(), spa_db.achievements.size());
        }
        achievements.close_screen();
      } else if (!app.demo_touch_hud_png.empty()) {
        // Headless Slice 13B verification capture: Android Touch Controls HUD overlay on live Colorado drive.
        const int demo_steps = int(std::lround(2.5 / drive::Vehicle::kStep));
        for (int ds = 0; ds <= demo_steps; ++ds) {
          const drive::Input di = ds == 0 ? drive::Input{} : (app.cruise ? host.follower.input(vehicle) : drive::Input{-0.12, 0.88, 0.0, false});
          if (ds > 0 && !step(di)) break;
        }
        wheel_poses(vehicle, poses);
        host.showroom.set_wheel_poses(poses);
        host.showroom.set_braking(0.0f);
        host.showroom.set_draw_car(true);
        tod_cockpit_cam = false;
        place_world();
        const float aspect = float(app.width) / float(app.height);
        host.showroom.set_frame(drive_frame(aspect, 0.016));
        app.touch_controls.set_visibility(carview::TouchVisibility::kAlwaysOn);
        app.touch_controls.set_steer_mode(carview::TouchSteerMode::kButtons);
        const carview::TouchLayout tlay = carview::TouchControls::compute_layout(float(app.width), float(app.height));
        app.touch_controls.on_finger_down(1, tlay.steer_left.cx(), tlay.steer_left.cy(), float(app.width), float(app.height), vehicle.time());
        app.touch_controls.on_finger_down(2, tlay.throttle.cx(), tlay.throttle.cy(), float(app.width), float(app.height), vehicle.time());
        set_minimap(int(app.height), true);
        set_hud(int(app.width), int(app.height), "ANDROID TOUCH CONTROLS - SMOOTH 60 PRESET");
        std::vector<uint8_t> rgba;
        if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
          std::fprintf(stderr, "carview: %s\n", app.error.c_str());
          app.status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          if (!write_png(app.demo_touch_hud_png.string(), app.width, app.height, 4, rgba.data())) app.status = 1;
          else std::printf("demo touch hud -> %s (%ux%u)\n", app.demo_touch_hud_png.string().c_str(), app.width, app.height);
        }
      } else if (!app.demo_upgrade_shop_png.empty()) {
        // Headless Round-2 verification capture: 3D Workshop + Upgrade Shop with localized FH1 part names.
        place_world();
        const float aspect = float(app.width) / float(app.height);
        host.showroom.set_frame(drive_frame(aspect, 0.016));
        if (!*app.car_names_loaded) carview::car_name("");
        const auto db = app.parts_db.valid() ? app.parts_db.get() : std::shared_ptr<cars::PartsDb>{};
        if (db && db->is_open()) {
          if (!app.have_garage || app.career_garage.cars.empty()) {
            std::map<std::string, int64_t> stock_s;
            int64_t cid = 309;
            for (const auto& [id_k, m_k] : app.baked_ids) {
              if (lower_ascii(m_k) == lower_ascii(app.car)) { cid = id_k; break; }
            }
            if (db->stock_slots(cid, stock_s, nullptr)) {
              int64_t gid = 0, dummy_cr = 1000000;
              garage::buy_car(app.career_garage, cid, 0, 0.524381, 2, 1, stock_s, dummy_cr, &gid);
              app.career_garage.selected = gid;
              app.have_garage = true;
            }
          }
          if (const garage::Car* sc = app.career_garage.selected_car()) {
            std::string why;
            if (shop.open(*db, app.career_garage, sc->id, why, &hub_phys_settings, &hub_pi_cfg,
                          !app.class_bands.empty() ? &app.class_bands : nullptr)) {
              // Step parts to tier 1 across visible slots so real localized FH1 upgrade names shine!
              shop.set_category(carview::ShopCategory::kEngine);
              const size_t nrows = shop.visible_rows(0).size();
              for (size_t ri = 0; ri < nrows; ++ri) {
                shop.select_row(ri);
                shop.move_part(1);
              }
              shop.select_row(0);
              carview::ui::List ulist;
              carview::draw_upgrade_shop_hud(ulist, float(app.width), float(app.height), shop, app.class_bands, 0,
                                             std::max<int64_t>(app.career.credits, 150000), carview::car_name(app.car),
                                             {}, app.metric_units);
              upload_ui_atlas(host.renderer, !ui_atlas_synced);
              ui_atlas_synced = true;
              host.renderer.set_ui(ulist.items());
              std::vector<uint8_t> rgba;
              if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
                std::fprintf(stderr, "carview: %s\n", app.error.c_str());
                app.status = 1;
              } else {
                for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                if (!write_png(app.demo_upgrade_shop_png.string(), app.width, app.height, 4, rgba.data())) app.status = 1;
                else std::printf("demo upgrade shop -> %s (%ux%u)\n", app.demo_upgrade_shop_png.string().c_str(), app.width,
                                 app.height);
              }
              shop.close();
            } else {
              std::fprintf(stderr, "carview: demo upgrade shop open failed: %s\n", why.c_str());
              app.status = 1;
            }
          }
        }
      } else if (app.headless && !app.cli_campaign_stage.empty()) {
        // Deterministic headless campaign stage capture & scripted E2E verification
        // (`--campaign-stage <stage> --headless --out <png> [--telemetry-json <json>]`).
        if (!app.boot_intro_checked) {
          app.boot_intro_checked = true;
          race::CampaignAction cact;
          if (!app.campaign.apply_cli_stage(app.cli_campaign_stage, app.fmv_player, nullptr, cact)) {
            std::fprintf(stderr, "carview: unknown --campaign-stage '%s'\n", app.cli_campaign_stage.c_str());
            app.status = 2;
            return true;
          }
          apply_campaign_action(cact);
          if (!app.next_car.empty() || !app.next_event.empty()) {
            // Hot-reload session with the stage's car/event before running captures.
            return true;
          }
        }
        if (app.pending_handoff.active) {
          const DriveApp::PendingCampaignHandoff h = app.pending_handoff;
          app.pending_handoff = {};
          if (h.has_teleport) {
            vehicle.reset({h.pos[0], h.pos[1], h.pos[2]}, h.heading);
          }
          if (h.has_gps && app.gps_solver.ready()) {
            const drive::Vec3 at = vehicle.position();
            app.gps_nav.set_destination(h.gps_xz[0], h.gps_xz[1], traffic::GpsDestKind::kFestival, h.gps_label,
                                        at.x, at.z);
          }
          if (h.has_event && !app.race) {
            app.next_event = !h.event_id.empty() ? h.event_id : std::to_string(h.route_id);
            app.next_event_fallback =
                (!h.event_id.empty() && h.route_id > 0) ? std::to_string(h.route_id) : std::string{};
            return true;
          }
        }

        if (!app.telemetry_json_path.empty()) {
          // Scripted multi-frame headless campaign drive with per-frame JSON telemetry.
          std::vector<double> capture_times;
          if (!app.capture_times_csv.empty()) {
            std::stringstream ss(app.capture_times_csv);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
              if (!tok.empty()) capture_times.push_back(std::max(0.0, std::atof(tok.c_str())));
            }
          } else {
            capture_times = {0.0, 1.0, 3.0, 6.0, 10.0};
          }
          std::sort(capture_times.begin(), capture_times.end());

          const double duration = std::max(0.0, app.seconds);
          constexpr double kFrameDt = 1.0 / 60.0;
          const int total_frames = int(std::lround(duration / kFrameDt));
          const int substeps_per_frame = std::max(1, int(std::lround(kFrameDt / drive::Vehicle::kStep)));

          const std::filesystem::path out_prefix(app.out.empty() ? "out/campaign-e2e/campaign.png" : app.out);
          if (out_prefix.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(out_prefix.parent_path(), ec);
          }
          if (app.telemetry_json_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(app.telemetry_json_path.parent_path(), ec);
          }

          struct TelemetrySample {
            int frame = 0;
            double t = 0.0;
            double x = 0.0, y = 0.0, z = 0.0;
            bool has_road_y = false;
            double road_y = 0.0;
            double speed_kmh = 0.0;
            int chunks_loaded = 0;
            int streamer_world_models = 0;
            double streamer_dist_m = 0.0;
            int streamer_gathers = 0;
            double frame_ms = 0.0;
            double sim_ms = 0.0;
            double render_cpu_ms = 0.0;
            double render_gpu_ms = 0.0;
            std::string captured_png;
          };
          std::vector<TelemetrySample> samples;
          samples.reserve(size_t(total_frames) + 1);

          size_t next_cap_idx = 0;
          for (int fi = 0; fi <= total_frames && app.status == 0; ++fi) {
            const double frame_t = double(fi) * kFrameDt;
            const auto frame_t0 = std::chrono::steady_clock::now();
            if (fi > 0) {
              for (int sub = 0; sub < substeps_per_frame; ++sub) {
                const double cur_t = vehicle.time();
                drive::Input di{};
                if (!app.script_path.empty()) {
                  di = drive::script_input(app.script, cur_t);
                } else if (app.cruise) {
                  di = host.follower.input(vehicle);
                } else {
                  di = drive::Input{0.03 * std::sin(cur_t * 0.7), 1.0, 0.0, false};
                }
                if (!step(di)) {
                  std::fprintf(stderr, "carview: %s\n", app.error.c_str());
                  app.status = 1;
                  break;
                }
              }
              if (app.status != 0) break;
            }
            const auto sim_t1 = std::chrono::steady_clock::now();

            wheel_poses(vehicle, poses);
            host.showroom.set_wheel_poses(poses);
            host.showroom.set_braking(brake_glow(vehicle));
            host.showroom.set_draw_car(true);
            tod_cockpit_cam = false;
            place_world();
            update_driving_line_overlay();
            const float aspect = float(app.width) / float(app.height);
            const drive::Vec3 vp = vehicle.position();
            const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            const double vhead = std::atan2(vf.x, vf.z);
            drive::CameraView camp_cam{};
            ShowroomFrame cframe =
                app.campaign.compute_camera({vp.x, vp.y, vp.z}, vhead, camp_cam)
                    ? camera_view_frame(camp_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                    : drive_frame(aspect, kFrameDt);
            host.showroom.set_frame(cframe);
            if (!app.campaign.is_blocking_gameplay()) set_minimap(int(app.height), fi == 0);
            set_hud(int(app.width), int(app.height), "");
            host.renderer.set_camera_projection(host.cam_proj.near_z, host.cam_proj.far_z, host.cam_proj.proj00,
                                                host.cam_proj.proj11);
            host.renderer.set_camera_vp(host.cam_proj.vp_unjittered, 0.0f, 0.0f);

            const bool should_capture =
                (next_cap_idx < capture_times.size() && frame_t + 0.5 * kFrameDt >= capture_times[next_cap_idx]);
            double rec_ms = 0.0, gpu_ms = 0.0;
            std::vector<uint8_t> rgba;
            const auto ren_t0 = std::chrono::steady_clock::now();
            if (should_capture) {
              if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
                std::fprintf(stderr, "carview: %s\n", app.error.c_str());
                app.status = 1;
                break;
              }
              rec_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ren_t0).count();
            } else {
              if (!host.renderer.render_timed(FrameConstants{}, app.width, app.height, rec_ms, gpu_ms, app.error)) {
                std::fprintf(stderr, "carview: %s\n", app.error.c_str());
                app.status = 1;
                break;
              }
            }
            const auto frame_t1 = std::chrono::steady_clock::now();

            std::string png_path;
            if (should_capture) {
              const double cap_t = capture_times[next_cap_idx];
              ++next_cap_idx;
              for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
              const int tenths = int(std::lround(cap_t * 10.0));
              char suffix[32];
              std::snprintf(suffix, sizeof suffix, "_t%02d_%ds.png", tenths / 10, tenths % 10);
              png_path = (out_prefix.parent_path() / (out_prefix.stem().string() + suffix)).string();
              if (!write_png(png_path, app.width, app.height, 4, rgba.data())) {
                std::fprintf(stderr, "carview: cannot write %s\n", png_path.c_str());
                app.status = 1;
                break;
              }
            }

            TelemetrySample s{};
            s.frame = fi;
            s.t = frame_t;
            s.x = vp.x;
            s.y = vp.y;
            s.z = vp.z;
            if (host.ground) {
              drive::Hit gh;
              if (host.ground->raycast({vp.x, 2000.0, vp.z}, {0, -1, 0}, 4000.0, gh)) {
                s.has_road_y = true;
                s.road_y = 2000.0 - gh.distance;
              }
            }
            s.speed_kmh = vehicle.speed() * 3.6;
            if (host.streamer) {
              s.streamer_world_models = int(host.streamer->stats().world_models);
              s.streamer_dist_m = std::hypot(vp.x - host.streamer->centre_x(), vp.z - host.streamer->centre_z());
              s.streamer_gathers = host.streamer->gathers();
              s.chunks_loaded =
                  (s.streamer_dist_m <= host.streamer->collision_radius()) ? s.streamer_world_models : 0;
            }
            s.frame_ms = std::chrono::duration<double, std::milli>(frame_t1 - frame_t0).count();
            s.sim_ms = std::chrono::duration<double, std::milli>(sim_t1 - frame_t0).count();
            s.render_cpu_ms = rec_ms;
            s.render_gpu_ms = gpu_ms;
            s.captured_png = png_path;
            samples.push_back(std::move(s));

            if (!png_path.empty()) {
              const auto& back = samples.back();
              std::printf("campaign-e2e [%s] t=%.2fs: pos=(%.2f, %.2f, %.2f) road_y=%s speed=%.1f km/h "
                          "chunks=%d (world_models=%d dist=%.1fm) frame=%.2fms -> %s\n",
                          app.cli_campaign_stage.c_str(), back.t, back.x, back.y, back.z,
                          back.has_road_y ? std::to_string(back.road_y).c_str() : "null", back.speed_kmh,
                          back.chunks_loaded, back.streamer_world_models, back.streamer_dist_m, back.frame_ms,
                          png_path.c_str());
            }
          }

          auto json_escape = [](const std::string& in) {
            std::string out;
            out.reserve(in.size() + 8);
            for (char c : in) {
              if (c == '\\') out += "\\\\";
              else if (c == '"') out += "\\\"";
              else if (c == '\n') out += "\\n";
              else if (c == '\r') out += "\\r";
              else if (c == '\t') out += "\\t";
              else out.push_back(c);
            }
            return out;
          };

          std::ofstream jf(app.telemetry_json_path);
          if (!jf) {
            std::fprintf(stderr, "carview: cannot write telemetry JSON %s\n",
                         app.telemetry_json_path.string().c_str());
            app.status = 1;
          } else {
            jf << "{\n";
            jf << "  \"stage\": \"" << json_escape(app.cli_campaign_stage) << "\",\n";
            jf << "  \"car\": \"" << json_escape(app.car) << "\",\n";
            jf << "  \"width\": " << app.width << ",\n";
            jf << "  \"height\": " << app.height << ",\n";
            jf << "  \"duration_s\": " << duration << ",\n";
            jf << "  \"frames\": [\n";
            for (size_t idx = 0; idx < samples.size(); ++idx) {
              const auto& s = samples[idx];
              jf << "    {\"frame\": " << s.frame
                 << ", \"t\": " << s.t
                 << ", \"x\": " << s.x
                 << ", \"y\": " << s.y
                 << ", \"z\": " << s.z
                 << ", \"road_y\": ";
              if (s.has_road_y) jf << s.road_y;
              else jf << "null";
              jf << ", \"speed_kmh\": " << s.speed_kmh
                 << ", \"chunks_loaded\": " << s.chunks_loaded
                 << ", \"streamer_world_models\": " << s.streamer_world_models
                 << ", \"streamer_dist_m\": " << s.streamer_dist_m
                 << ", \"streamer_gathers\": " << s.streamer_gathers
                 << ", \"frame_ms\": " << s.frame_ms
                 << ", \"sim_ms\": " << s.sim_ms
                 << ", \"render_cpu_ms\": " << s.render_cpu_ms
                 << ", \"render_gpu_ms\": " << s.render_gpu_ms
                 << ", \"captured_png\": ";
              if (!s.captured_png.empty()) jf << "\"" << json_escape(s.captured_png) << "\"";
              else jf << "null";
              jf << "}" << (idx + 1 < samples.size() ? "," : "") << "\n";
            }
            jf << "  ]\n";
            jf << "}\n";
            jf.close();
            std::printf("campaign-e2e: wrote %zu frames of telemetry -> %s\n", samples.size(),
                        app.telemetry_json_path.string().c_str());
          }
          if (app.fmv_player.active()) app.fmv_player.close();
        } else {
          if (app.fmv_player.active()) {
            // Advance past initial fade-in so real movie frame & subtitles are crisp.
            for (int fi = 0; fi < 45 && app.fmv_player.active(); ++fi) app.fmv_player.update(0.1, false);
          } else if (!app.campaign.is_cinematic_camera_active()) {
            // Live driving stage (e.g. viper_drive, dialog): roll smoothly on Colorado road for real speed & GPS.
            const int demo_steps = int(std::lround(2.5 / drive::Vehicle::kStep));
            for (int ds = 0; ds <= demo_steps; ++ds) {
              const drive::Input di =
                  ds == 0 ? drive::Input{} : (app.cruise ? host.follower.input(vehicle) : drive::Input{0.0, 0.78, 0.0, false});
              if (ds > 0 && !step(di)) break;
            }
          }
          wheel_poses(vehicle, poses);
          host.showroom.set_wheel_poses(poses);
          host.showroom.set_braking(brake_glow(vehicle));
          host.showroom.set_draw_car(true);
          tod_cockpit_cam = false;
          place_world();
          const float aspect = float(app.width) / float(app.height);
          const drive::Vec3 vp = vehicle.position();
          const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
          const double vhead = std::atan2(vf.x, vf.z);
          drive::CameraView camp_cam{};
          ShowroomFrame cframe =
              app.campaign.compute_camera({vp.x, vp.y, vp.z}, vhead, camp_cam)
                  ? camera_view_frame(camp_cam, info, vehicle, aspect, host.renderer.reverse_z(), &host.cam_proj)
                  : drive_frame(aspect, 0.016);
          host.showroom.set_frame(cframe);
          if (!app.campaign.is_blocking_gameplay()) set_minimap(int(app.height), true);
          set_hud(int(app.width), int(app.height), "");
          std::vector<uint8_t> rgba;
          host.renderer.set_camera_projection(host.cam_proj.near_z, host.cam_proj.far_z, host.cam_proj.proj00, host.cam_proj.proj11);
          if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
            std::fprintf(stderr, "carview: %s\n", app.error.c_str());
            app.status = 1;
          } else {
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            if (!write_png(app.out, app.width, app.height, 4, rgba.data())) app.status = 1;
            else std::printf("campaign stage '%s' -> %s (%ux%u)\n", app.cli_campaign_stage.c_str(), app.out.c_str(), app.width,
                             app.height);
          }
          if (app.fmv_player.active()) app.fmv_player.close();
        }
      } else {
        return false;
      }
      return true;
}

void DriveSession::run_headless_drive() {
        constexpr int kAudioRate = carview::AudioHost::kAudioRate;
        const std::filesystem::path prefix(app.out);
        const int steps = int(std::lround(app.seconds / drive::Vehicle::kStep));
        const int stride = std::max(1, int(std::lround(app.every / drive::Vehicle::kStep)));
        double longest_step = 0;  // wall time of one step with its streaming, s
        double race_done_at = -1;
        for (int s = 0; s <= steps && app.status == 0; ++s) {
          const auto t0 = std::chrono::steady_clock::now();
          const bool ok =
              s == 0 || step(app.cruise ? host.follower.input(vehicle) : drive::script_input(app.script, vehicle.time()));
          if (app.audio_host.has_radio() && app.radio_press_every > 0 && s > 0) {
            const double t = vehicle.time(), was = t - drive::Vehicle::kStep;
            if (std::floor(t / app.radio_press_every) > std::floor(was / app.radio_press_every)) radio_press(+1);
          }
          if (app.audio_host.has_audio() && s > 0) {
            const int frames = int(std::lround(kAudioRate * drive::Vehicle::kStep));
            const size_t at = audio_track.size();
            audio_track.resize(at + 2 * size_t(frames), 0.0f);
            app.audio_host.render_offline(sound_inputs(audio_view()), audio_track.data() + at, frames);
          }
          longest_step =
              std::max(longest_step, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
          if (!ok) {
            std::fprintf(stderr, "carview: %s\n", app.error.c_str());
            app.status = 1;
            break;
          }
          // A race ends the run 2 s after its last car finishes.
          if (app.race && app.race->session().all_finished()) {
            if (race_done_at < 0) race_done_at = vehicle.time();
            else if (vehicle.time() > race_done_at + 2) break;
          }
          if (s % stride != 0) continue;
          wheel_poses(vehicle, poses);
          if (photo_mode.active()) {
            const float wb = photo_mode.effects().wheel_blur_omega_boost();
            for (int wi = 0; wi < 4; ++wi) poses[wi].omega += wb;
          }
          host.showroom.set_wheel_poses(poses);
          host.showroom.set_braking(brake_glow(vehicle));
          tod_cockpit_cam = have_rig && rig.mode() == drive::CameraMode::kCockpit;
          place_world();
          const auto title_cam_yaw = [&]() {
            const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
            return std::atan2(fwd.x, fwd.z) + 0.58 + 0.34 * std::sin(title_orbit_time * 0.32);
          };
          ShowroomFrame hframe =
              app.show_title
                  ? scenic_orbit_frame(info, vehicle, float(app.width) / float(app.height), title_cam_yaw(), 5.8f, 1.48f,
                                       host.renderer.reverse_z(), host.ground, &host.cam_proj)
                  : drive_frame(float(app.width) / float(app.height), app.every);
          float h_jitter_uv_x = 0.0f, h_jitter_uv_y = 0.0f;
          if (host.renderer.active_upscaler().temporal) {
            const auto rex = host.renderer.render_width()
                                 ? carview::modern::Extent{host.renderer.render_width(), host.renderer.render_height()}
                                 : host.renderer.expected_render_extent(app.width, app.height);
            const uint32_t jphase = carview::modern::jitter_phase_count(rex, {app.width, app.height});
            const auto j = carview::modern::halton_jitter(host.temporal_frame_idx++, jphase);
            apply_ndc_jitter(hframe, 2.0f * j[0] / float(std::max(1u, rex.w)),
                             2.0f * j[1] / float(std::max(1u, rex.h)));
            h_jitter_uv_x = j[0] / float(std::max(1u, rex.w));
            h_jitter_uv_y = j[1] / float(std::max(1u, rex.h));
          }
          host.showroom.set_frame(hframe);
          name_tags.clear();
          if (app.race && !app.race->player_finished()) {
            carview::RaceView::OpponentTag stag;
            if (app.race->showcase_tag(stag)) {
              float sx = 0, sy = 0;
              if (project_world_point(hframe, info, vehicle, stag.x, stag.y, stag.z, float(app.width), float(app.height), sx, sy))
                name_tags.push_back({sx, sy, stag.name, stag.leader});
            }
          } else if (festival_mgr) {
            const drive::Vec3 at = vehicle.position();
            for (const auto& fc : festival_mgr->cars()) {
              if (std::hypot(fc.position.x - at.x, fc.position.z - at.z) > 130.0) continue;
              float sx = 0, sy = 0;
              if (project_world_point(hframe, info, vehicle, fc.position.x, fc.position.y + 1.85, fc.position.z,
                                      float(app.width), float(app.height), sx, sy))
                name_tags.push_back({sx, sy, "[" + fc.spec.car_class + " " + std::to_string(fc.spec.pi) + "] " + fc.spec.driver_name,
                                     fc.in_h2h});
            }
          }
          if (app.hud || app.show_title || host.perf_overlay_open || host.inspector_open) {
            if (app.hud && !app.show_title) set_minimap(int(app.height), true);
            set_hud(int(app.width), int(app.height), vehicle.time() < notice_until  ? notice
                                 : app.race ? race_banner()
                                 : vehicle.time() < kIntroSeconds ? intro
                                                                  : club_banner());
            if (app.hud) set_radio_popup(int(app.height));
          }
          std::vector<uint8_t> rgba;
          host.renderer.set_camera_projection(host.cam_proj.near_z, host.cam_proj.far_z, host.cam_proj.proj00, host.cam_proj.proj11);
          host.renderer.set_camera_vp(host.cam_proj.vp_unjittered, h_jitter_uv_x, h_jitter_uv_y);
          if (host.renderer.active_rt_mode() != carview::rt::RtMode::kOff) {
            const int rt_warmup = (host.renderer.active_rt_mode() == carview::rt::RtMode::kPhotoModePt)
                                      ? std::clamp(64 / std::max(1, host.renderer.graphics_settings().rt_spp), 8, 64)
                                      : 10;
            for (int wi = 0; wi < rt_warmup; ++wi) {
              double wrec = 0.0, wgpu = 0.0;
              if (!host.renderer.render_timed(FrameConstants{}, app.width, app.height, wrec, wgpu, app.error)) break;
            }
          }
          const auto rt0 = std::chrono::steady_clock::now();
          if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
            std::fprintf(stderr, "carview: %s\n", app.error.c_str());
            app.status = 1;
            break;
          }
          const auto rt1 = std::chrono::steady_clock::now();
          host.last_frame_cpu_ms = std::chrono::duration<double, std::milli>(rt1 - rt0).count();
          app.frame_stats.frame(std::chrono::duration<double>(rt1.time_since_epoch()).count());
          if (host.renderer.active_frame_gen() != carview::modern::FrameGen::kOff) app.frame_stats.generated(1);
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          char name[32];
          std::snprintf(name, sizeof name, "_%05d.png", int(std::lround(vehicle.time() * 100)));
          const std::string path = (prefix.parent_path() / (prefix.stem().string() + name)).string();
          if (!write_png(path, app.width, app.height, 4, rgba.data())) {
            std::fprintf(stderr, "carview: cannot write %s\n", path.c_str());
            app.status = 1;
          }
          if (app.texture_census && host.streamer) census_frame(app.width, app.height);
          const auto& wl = vehicle.wheels()[0];
          const drive::Vec3 at = vehicle.position();
          std::printf("t %.2f s: %.1f km/h, gear %d, %.0f rpm, LF steer %.3f rad compression %.3f m, at %.1f %.1f "
                      "%.1f -> %s\n",
                      vehicle.time(), vehicle.speed() * 3.6, vehicle.gear(), vehicle.rpm(), wl.steer, wl.compression,
                      at.x, at.y, at.z, path.c_str());
        }
        if (rig_max_distance > 0)
          std::printf("chase eye from the CoM over the run: %.2f to %.2f m\n", rig_min_distance, rig_max_distance);
        if (host.streamer)
          std::printf("longest step with its streaming: %.1f ms (%s gathers)\n", longest_step * 1000,
                      app.async_stream ? "background" : "synchronous");
        if (app.texture_census || host.streamer) {
          const auto vram = host.renderer.vram_stats();
          const auto wts = host.showroom.world_texture_stats();
          std::printf("vram summary: %u blocks, %u allocs (%.1f/%.1f MiB); world textures: resident %zu (%.1f MiB), uploads %llu, evictions %llu, reuploads %llu, pools %zu, retired_pending %zu\n",
                      vram.block_count, vram.allocation_count,
                      double(vram.allocation_bytes) / (1024.0 * 1024.0), double(vram.block_bytes) / (1024.0 * 1024.0),
                      wts.resident_count, double(wts.resident_bytes) / (1024.0 * 1024.0),
                      (unsigned long long)wts.uploads, (unsigned long long)wts.evictions,
                      (unsigned long long)wts.reuploads, wts.descriptor_pools, wts.retired_pending);
        }
        if (app.race) {
          std::printf("%s m\nrace: results at %.2f s%s\n%s", app.race->summary().c_str(), app.race->session().clock(),
                      app.race->session().all_finished() ? "" : " (not every car finished)", app.race->results().c_str());
        }
        if (app.audio_host.has_radio()) {
          const auto tel = app.audio_host.telemetry();
          if (tel.radio_selected >= 0 && size_t(tel.radio_selected) < app.audio_host.radio_data().stations.size()) {
            const auto& st = app.audio_host.radio_data().stations[size_t(tel.radio_selected)];
            std::printf("radio: %s at the end, %.1f s on the radio clock\n", st.off ? "off" : st.name.c_str(),
                        tel.radio_clock);
          }
        }
        if (!app.audio_out.empty() && !audio_track.empty()) {
          float peak = 0;
          for (float x : audio_track) peak = std::max(peak, std::fabs(x));
          if (!write_wav(app.audio_out, kAudioRate, audio_track))
            std::fprintf(stderr, "carview: cannot write %s\n", app.audio_out.string().c_str());
          else
            std::printf("audio: %.2f s to %s, peak %.3f\n", double(audio_track.size()) / 2 / kAudioRate,
                        app.audio_out.string().c_str(), peak);
        }
}

}  // namespace carview
