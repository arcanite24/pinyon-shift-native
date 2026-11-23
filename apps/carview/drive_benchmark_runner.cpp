#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <stb_image.h>

#include "benchmark.h"
#include "hub_scene.h"
#include "process_time.h"

namespace carview {

void DriveSession::print_scenery_summary() {
      if (app.status == 0 && host.streamer) {
        const auto& st = host.streamer->stats();
        std::printf("scenery: %zu triangles, %zu materials, %zu textures decoded (%zu failed), %zu models with the "
                    "default UV scale, %zu submeshes without a material\n",
                    host.streamer->mesh().triangles(), host.streamer->mesh().materials.size(), host.texture_loader.decoded,
                    host.texture_loader.failed.size(), st.default_uv_models.size(), st.no_material);
        if (st.far_models)
          std::printf("far ring: %zu models, %zu triangles\n", st.far_models, st.far_triangles);
        if (st.trees || st.trees_untextured)
          std::printf("tree sprites: %zu (%zu without an atlas file)\n", st.trees, st.trees_untextured);
        std::printf("props: %zu placed, %zu unresolved; left out: %zu off the free-roam route, %zu markers, %zu beacons, %zu "
                    "tree sprites off the route\n",
                    st.props, st.prop_unresolved, st.props_off_route, st.props_markers, st.props_beacons,
                    st.trees_off_route);
        for (const auto& n : host.showroom.notes()) std::fprintf(stderr, "carview: note: %s\n", n.c_str());
        // The effects drawn without a texture, by triangles.
        const auto& mesh = host.streamer->mesh();
        std::map<std::string, size_t> flat;
        for (size_t t = 0; t < mesh.material.size(); ++t) {
          const auto& m = mesh.materials[mesh.material[t]];
          if (m.is_water) continue;  // water uses environment cubemap + normal maps, not slot 0 diffuse
          if (m.albedo == world::kNoTexture || host.texture_loader.failed.count(m.albedo)) ++flat[m.effect];
        }
        for (const auto& [effect, n] : flat)
          std::printf("  drawn flat (slot 0 has no texture file): %s, %zu triangles\n", effect.c_str(), n);
        if (std::getenv("SHIFT_WORLD_DEBUG")) {
          // Per material: triangles, median UV span per metre of edge.
          std::map<uint32_t, std::vector<float>> spans;
          for (size_t t = 0; t < mesh.material.size(); ++t) {
            const auto& a = mesh.positions[t * 3];
            const auto& b = mesh.positions[t * 3 + 1];
            const double len = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
            const auto& ua = mesh.uv0[t * 3];
            const auto& ub = mesh.uv0[t * 3 + 1];
            if (len > 0.5) spans[mesh.material[t]].push_back(float(std::hypot(ub[0] - ua[0], ub[1] - ua[1]) / len));
          }
          for (auto& [m, v] : spans) {
            std::sort(v.begin(), v.end());
            const auto& mm = mesh.materials[m];
            double sum[4] = {};
            size_t corners = 0;
            for (size_t t = 0; t < mesh.material.size(); ++t)
              if (mesh.material[t] == m)
                for (int k = 0; k < 3; ++k, ++corners)
                  for (int c = 0; c < 4; ++c) sum[c] += mesh.colours[t * 3 + size_t(k)][size_t(c)];
            if (corners)
              std::printf("  material %u vertex colour mean %.0f %.0f %.0f %.0f\n", m, sum[0] / corners,
                          sum[1] / corners, sum[2] / corners, sum[3] / corners);
            auto id = [&](uint32_t r) { return r < host.colorado.textures().size() ? host.colorado.textures()[r].id : 0u; };
            std::printf("  material %u %s texture %u (_0x%08X) layers %d B _0x%08X splat _0x%08X tiling %.3g %.3g "
                        "%.3g %.3g far %d blend %d cut-out %d: %zu triangles, UV per metre %.4g\n",
                        m, mm.effect.c_str(), mm.albedo, id(mm.albedo), int(mm.layers), id(mm.layer_b), id(mm.splat),
                        mm.tiling[0], mm.tiling[1], mm.tiling[2], mm.tiling[3], int(mm.far), int(mm.blend),
                        int(host.texture_loader.cutout.count(mm.albedo)), v.size(), v[v.size() / 2]);
          }
        }
      }
}

void DriveSession::run_world_reference() {
        // The reference frame: file-frame positions (the scenery's are in
        // the game frame, relative to its origin), the reference's matrix
        // flipped to Vulkan's clip y, no car.
        const float ox = float(host.showroom.world_origin_x()), oz = float(host.showroom.world_origin_z());
        const float to_file[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, ox, 0, -oz, 1};
        host.showroom.set_world_transform(to_file);
        const float up_game[3] = {0, 1, 0};
        float sun[3], up[3];
        rotate_direction(to_file, kSunGame, sun);
        rotate_direction(to_file, up_game, up);
        host.showroom.set_world_light(sun, up, 1.0f);
        if (host.have_shadow_light) {
          float towards[3];
          rotate_direction(to_file, host.shadow_light_game, towards);
          host.showroom.set_shadow_direction(towards);
        }
        ShowroomFrame frame{};
        std::memcpy(frame.view_projection, app.world_reference.clip_from_world, sizeof frame.view_projection);
        for (int c = 0; c < 4; ++c) frame.view_projection[c * 4 + 1] = -frame.view_projection[c * 4 + 1];
        // Reversed depth: z' = w - z (the game's depth d becomes 1 - d).
        if (host.renderer.reverse_z())
          for (int c = 0; c < 4; ++c) frame.view_projection[c * 4 + 2] = frame.view_projection[c * 4 + 3] - frame.view_projection[c * 4 + 2];
        std::memcpy(frame.transparent_view_projection, frame.view_projection, sizeof frame.view_projection);
        for (int k = 0; k < 3; ++k) frame.eye[k] = app.world_reference.camera_position[k];
        frame.options[0] = 1;
        host.showroom.set_draw_car(false);
        host.showroom.set_frame(frame);
        // SHIFT_WORLD_PICK=x,y (debug, ours): the nearest scenery triangles
        // under that pixel of the reference view, with their materials.
        if (const char* pick = std::getenv("SHIFT_WORLD_PICK"); pick && host.streamer) {
          float px = 0, py = 0;
          std::sscanf(pick, "%f,%f", &px, &py);
          const auto& mesh = host.streamer->mesh();
          const float* m = app.world_reference.clip_from_world;
          std::vector<std::pair<float, size_t>> hits;
          for (size_t t = 0; t * 3 + 2 < mesh.positions.size(); ++t) {
            float sx[3], sy[3], sz[3];
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
              const auto& v = mesh.positions[t * 3 + size_t(k)];
              const float f[4] = {float(v[0]), float(v[1]), -float(v[2]), 1};
              float c[4] = {};
              for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j) c[r] += m[j * 4 + r] * f[j];
              if (c[3] <= 0.01f) ok = false;
              sx[k] = (c[0] / c[3] * 0.5f + 0.5f) * float(app.width);
              sy[k] = (0.5f - c[1] / c[3] * 0.5f) * float(app.height);
              sz[k] = c[2] / c[3];
            }
            if (!ok || int(mesh.materials[mesh.material[t]].layers) == 4) continue;
            if ((sz[0] + sz[1] + sz[2]) / 3 < 0 || (sz[0] + sz[1] + sz[2]) / 3 > 1) continue;
            auto edge = [&](int a, int b) { return (sx[b] - sx[a]) * (py - sy[a]) - (sy[b] - sy[a]) * (px - sx[a]); };
            const float e0 = edge(0, 1), e1 = edge(1, 2), e2 = edge(2, 0);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
              hits.push_back({(sz[0] + sz[1] + sz[2]) / 3, t});
          }
          std::sort(hits.begin(), hits.end());
          auto id = [&](uint32_t r) { return r < host.colorado.textures().size() ? host.colorado.textures()[r].id : 0u; };
          for (size_t i = 0; i < hits.size() && i < 6; ++i) {
            const size_t t = hits[i].second;
            const auto& mm = mesh.materials[mesh.material[t]];
            if (fh1::WorldTexture wt; i == 0 && host.colorado.read_texture(mm.albedo, wt, nullptr) && !wt.blocks.levels.empty())
              std::printf("pick: albedo format %d %ux%u levels %zu gamma %d cut-out %d\n", int(wt.blocks.format),
                          wt.blocks.levels[0].width, wt.blocks.levels[0].height, wt.blocks.levels.size(),
                          int(wt.format.gamma), int(fh1::world_texture_has_cutout(wt)));
            std::printf("pick: depth %.5f tri %zu material %u %s albedo _0x%08X layers %d B _0x%08X splat _0x%08X "
                        "far %d blend %d surface %d colour %d %d %d %d model %s\n",
                        hits[i].first, t, mesh.material[t], mm.effect.c_str(), id(mm.albedo), int(mm.layers),
                        id(mm.layer_b), id(mm.splat), int(mm.far), int(mm.blend), int(mesh.surface[t]),
                        mesh.colours[t * 3][0], mesh.colours[t * 3][1], mesh.colours[t * 3][2], mesh.colours[t * 3][3],
                        t < mesh.model.size() && mesh.model[t] < host.colorado.models().size()
                            ? host.colorado.models()[mesh.model[t]].name.c_str() : "?");
          }
        }
        std::vector<uint8_t> rgba;
        if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
          std::fprintf(stderr, "carview: %s\n", app.error.c_str());
          app.status = 1;
        } else {
          for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
          const std::filesystem::path path(app.out);
          if (!write_png(path.string(), app.width, app.height, 4, rgba.data())) app.status = 1;
          // Side by side with the reference image.
          int rw = 0, rh = 0, rc = 0;
          unsigned char* ref = app.world_reference.image.empty()
                                   ? nullptr
                                   : stbi_load(app.world_reference.image.string().c_str(), &rw, &rh, &rc, 4);
          if (ref && uint32_t(rw) == app.width && uint32_t(rh) == app.height) {
            std::vector<uint8_t> both(size_t(app.width) * 2 * app.height * 4);
            for (uint32_t y = 0; y < app.height; ++y) {
              std::memcpy(&both[(size_t(y) * app.width * 2) * 4], ref + size_t(y) * app.width * 4, size_t(app.width) * 4);
              std::memcpy(&both[(size_t(y) * app.width * 2 + app.width) * 4], &rgba[size_t(y) * app.width * 4], size_t(app.width) * 4);
            }
            const auto vs = path.parent_path() / (path.stem().string() + "_vs.png");
            if (!write_png(vs.string(), app.width * 2, app.height, 4, both.data())) app.status = 1;
            std::printf("world reference: %s and %s\n", path.string().c_str(), vs.string().c_str());
          } else {
            std::printf("world reference: %s (no reference image of the same size)\n", path.string().c_str());
          }
          if (ref) stbi_image_free(ref);
        }
}

void DriveSession::run_benchmark() {
        // Comprehensive cross-device benchmark suite (JSON schema v1 + per-frame CSV).
        // Backward-compatible with legacy --bench, and supports deterministic
        // --bench-scenario modes, --bench-full-fx, --bench-audio, and --bench-warmup.
        using S = carview::bench::Scenario;
        const bool audio_bench_on = app.sound && (app.bench_audio || app.bench_full_fx) && app.audio_host.has_audio();
        if (audio_bench_on) app.audio_host.start_benchmark_device();

        // Scenario-specific initialization.
        PauseMenu bench_pause_menu;
        bool autoshow_opened = false;
        if (app.bench_scenario == S::kAutoshowStudio) {
          if (app.parts_db.valid()) {
            if (const auto db = app.parts_db.get()) {
              std::string why;
              autoshow_opened = autoshow.open(*db, app.all_baked, why, 100000000, false, false, 1.0);
              if (autoshow_opened) autoshow.set_step(carview::AutoshowStep::kCarSelect);
            }
          }
          // Studio lighting: use staticcarcubemap.xpr (loaded before upload) rather than outdoor sky.
          host.showroom.set_draw_car(true);
        }

        const int steps = int(std::lround(app.seconds / drive::Vehicle::kStep));
        carview::bench::BenchmarkRecorder recorder;
        recorder.reserve(size_t(steps / 2 + 16));

        double step_total_accum = 0.0;
        step_physics_ms = 0.0;
        step_ai_traffic_ms = 0.0;
        step_world_stream_ms = 0.0;

        bool ttff_recorded = false;
        bool measured_started = false;
        double measured_cpu0 = process_cpu_seconds();
        auto measured_wall0 = std::chrono::steady_clock::now();
        const auto run_wall0 = measured_wall0;
        const uint64_t staging_bytes0 = host.renderer.staging_upload_bytes() + host.showroom.world_staging_bytes();
        uint32_t frame_idx = 0;

        // Legacy summary accumulators (for backward-compatible stdout lines).
        std::vector<double> legacy_cpu_ms, legacy_gpu_ms, legacy_sim_ms, legacy_record_ms, legacy_pose_ms;
        double draws_sum = 0, culled_sum = 0, indices_sum = 0, shadow_sum = 0, shadow_cars_sum = 0;
        std::vector<std::string> stage_names;
        std::vector<std::vector<double>> stage_ms;

        for (int s = 0; s <= steps && app.status == 0; ++s) {
          const auto t0 = std::chrono::steady_clock::now();
          if (s > 0 && app.bench_scenario != S::kAutoshowStudio) {
            if (!step(app.cruise ? host.follower.input(vehicle) : drive::script_input(app.script, vehicle.time()))) {
              std::fprintf(stderr, "carview: %s\n", app.error.c_str());
              app.status = 1;
              break;
            }
          }
          step_total_accum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          if (s % 2 != 0) continue;

          const auto frame_t0 = std::chrono::steady_clock::now();
          const double sim_sec = double(frame_idx) * 2.0 * drive::Vehicle::kStep;
          const bool is_warmup = (app.bench_warmup_sec > 0.0) && (sim_sec < app.bench_warmup_sec);
          if (!is_warmup && !measured_started) {
            measured_started = true;
            measured_cpu0 = process_cpu_seconds();
            measured_wall0 = frame_t0;
          }

          // Audio push phase.
          double audio_push_ms = 0.0;
          if (audio_bench_on) {
            const auto a0 = std::chrono::steady_clock::now();
            const audio::View aview = audio_view();
            app.audio_host.update_frame(sound_inputs(aview), false, autoshow.is_open() || events_page.is_open() || settings_page.is_open(), false, false);
            audio::VehicleAudioState vst{};
            const drive::Vec3 vpos = vehicle.position();
            const drive::Vec3 vvel = vehicle.velocity();
            const drive::Vec3 vfwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
            const drive::Vec3 vright = drive::rotate(vehicle.orientation(), {1, 0, 0});
            vst.speed_mph = float(std::fabs(vehicle.speed()) * 2.2369363);
            vst.top_speed_mph = float(std::clamp(
                double(drive::CockpitAnimState::estimate_dial_max_speed_mps(vehicle.params())) * 2.2369363, 90.0,
                280.0));
            vst.rpm = float(vehicle.rpm());
            vst.redline_rpm = float(std::max(vehicle.params().redline_rpm, 6000.0));
            vst.throttle = float(vehicle.engine_throttle());
            vst.gear = vehicle.gear();
            vst.listener_pos = {float(vpos.x), float(vpos.y), float(vpos.z)};
            vst.listener_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
            vst.listener_right = {float(vright.x), float(vright.y), float(vright.z)};
            vst.cockpit_view = (aview == audio::View::kCockpit);
            vst.in_race = (app.race != nullptr);
            app.audio_host.update_vehicle(vst, app.race != nullptr, false, false, 0);
            app.audio_host.update_world_acoustics(float(vpos.x), float(vpos.y), float(vpos.z),
                                              float(std::atan2(vfwd.x, vfwd.z)));
            audio_push_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a0).count();
          }

          // Pose / camera / TOD / cull prep phase.
          const auto p0 = std::chrono::steady_clock::now();
          if (app.bench_scenario == S::kAutoshowStudio) {
            if (autoshow_opened && frame_idx > 0 && frame_idx % 15 == 0) {
              autoshow.move(1);
              autoshow.move_colour(1);
            }
            Orbit o;
            o.yaw = 145.0f + float(frame_idx) * 1.2f;
            o.pitch = 10.0f;
            o.distance = 0.0f;
            host.showroom.set_frame(orbit_frame(host.showroom, o, float(app.width) / float(app.height), false));
          } else {
            wheel_poses(vehicle, poses);
            if (photo_mode.active()) {
              const float wb = photo_mode.effects().wheel_blur_omega_boost();
              for (int wi = 0; wi < 4; ++wi) poses[wi].omega += wb;
            }
            host.showroom.set_wheel_poses(poses);
            host.showroom.set_braking(brake_glow(vehicle));
            tod_cockpit_cam = (app.bench_scenario == S::kCockpitMirrors) ||
                              (have_rig && rig.mode() == drive::CameraMode::kCockpit);
            place_world();
            host.showroom.set_frame(drive_frame(float(app.width) / float(app.height), 2 * drive::Vehicle::kStep));
          }
          const double pose_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count();

          // UI4 build phase (including ui4_heavy multi-screen cycling & autoshow carousel).
          double ui_build_ms = 0.0;
          const bool need_hud = app.hud || app.bench_scenario == S::kUi4Heavy || app.bench_scenario == S::kAutoshowStudio;
          if (need_hud) {
            const auto u0 = std::chrono::steady_clock::now();
            const PauseMenu* active_menu = nullptr;
            if (app.bench_scenario == S::kUi4Heavy) {
              const uint32_t ui_phase = (frame_idx / 10u) % 4u;
              map_overlay.active = false;
              events_page.close();
              settings_page.close();
              if (ui_phase == 0) {
                map_overlay.active = true;
                map_overlay.title = "COLORADO HORIZON FESTIVAL";
                map_overlay.subtitle = "RED ROCK CANYON  •  FESTIVAL CENTRAL";
                map_overlay.action_hint = "ENTER EVENT";
                map_overlay.gps_hint = "SET GPS ROUTE";
                map_overlay.filter_tabs = {"ALL", "EVENTS", "OUTPOSTS", "SIGNS", "SPEED TRAPS"};
                map_overlay.active_filter = int(frame_idx % 5u);
                map_overlay.zoom = 1.0 + 0.25 * double(frame_idx % 6u);
                map_overlay.roads_discovered = 142;
                map_overlay.roads_total = 216;
              } else if (ui_phase == 1) {
                bench_pause_menu.show();
                bench_pause_menu.move(1);
                active_menu = &bench_pause_menu;
              } else if (ui_phase == 2) {
                if (app.have_career_catalog) {
                  int64_t cid = 0;
                  int clid = 2;
                  current_car_info(cid, clid);
                  events_page.open(app.career_catalog, app.career, app.collected_wristband, app.current_pop_rank(), cid, clid,
                                   app.class_bands, int((frame_idx / 40u) % 3u));
                  for (uint32_t mv = 0; mv < (frame_idx % 8u); ++mv) events_page.move(1);
                } else {
                  bench_pause_menu.show();
                  active_menu = &bench_pause_menu;
                }
              } else {
                settings_page.open();
                for (uint32_t mv = 0; mv < (frame_idx % 5u); ++mv) settings_page.switch_tab(1);
                for (uint32_t mv = 0; mv < (frame_idx % 6u); ++mv) settings_page.move(1);
              }
            }
            set_hud(int(app.width), int(app.height), app.race ? race_banner() : intro, active_menu);
            ui_build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count();
          }

          double rec_ms = 0.0, gpu_ms = -1.0;
          host.renderer.set_camera_projection(host.cam_proj.near_z, host.cam_proj.far_z, host.cam_proj.proj00, host.cam_proj.proj11);
          host.renderer.set_camera_vp(host.cam_proj.vp_unjittered, 0.0f, 0.0f);
          if (!host.renderer.render_timed(FrameConstants{}, app.width, app.height, rec_ms, gpu_ms, app.error)) {
            std::fprintf(stderr, "carview: %s\n", app.error.c_str());
            app.status = 1;
            break;
          }
          const double submit_wait_ms = host.renderer.last_submit_wait_ms();
          const auto frame_t1 = std::chrono::steady_clock::now();
          if (!ttff_recorded) {
            ttff_recorded = true;
            app.bench_startup.ttff_ms =
                std::chrono::duration<double, std::milli>(frame_t1 - app.drive_start_tp).count();
          }

          const double active_cpu_ms = step_physics_ms + step_ai_traffic_ms + step_world_stream_ms +
                                       audio_push_ms + pose_ms + ui_build_ms + rec_ms;
          const double total_frame_ms =
              step_total_accum + std::chrono::duration<double, std::milli>(frame_t1 - frame_t0).count();

          carview::bench::FrameSample fs{};
          fs.frame_index = frame_idx;
          fs.is_warmup = is_warmup;
          fs.sim_sec = sim_sec;
          fs.wall_sec = std::chrono::duration<double>(frame_t1 - run_wall0).count();
          fs.frame_ms = total_frame_ms;
          fs.cpu_ms = active_cpu_ms;
          fs.gpu_ms = gpu_ms;
          fs.physics_ms = step_physics_ms;
          fs.ai_traffic_ms = step_ai_traffic_ms;
          fs.world_stream_ms = step_world_stream_ms;
          fs.audio_push_ms = audio_push_ms;
          fs.ui_build_ms = ui_build_ms;
          fs.pose_cull_ms = pose_ms;
          fs.cmd_record_ms = rec_ms;
          fs.submit_wait_ms = submit_wait_ms;
          fs.gpu_passes = host.renderer.stage_ms();

          const auto ws = host.showroom.world_draw_stats();
          fs.world_draws = uint32_t(ws.drawn);
          fs.world_culled = uint32_t(ws.culled);
          fs.world_hiz_culled = host.showroom.gpu_hiz_culled();
          fs.trees_drawn = host.showroom.gpu_trees_drawn();
          fs.shadow_world_draws = uint32_t(host.showroom.shadow_draw_count());
          fs.shadow_cars = uint32_t(host.showroom.shadow_car_count());
          fs.env_draws = host.showroom.last_env_draws();
          fs.mirror_draws = host.showroom.last_mirror_draws();
          fs.car_draws = host.showroom.last_car_draws();
          fs.car_instances = uint32_t(host.showroom.instance_count());
          fs.particle_sprites = host.showroom.last_particle_sprites();
          fs.skid_tris = host.showroom.last_skid_tris();
          fs.ui4_shapes = host.renderer.last_ui_shape_count();
          fs.triangles_drawn = uint64_t(ws.indices / 3u) + uint64_t(host.showroom.triangle_count());
          fs.triangles_total = uint64_t(host.showroom.world_triangle_count()) + uint64_t(host.showroom.triangle_count());
          fs.rss_mib = double(carview::bench::query_process_memory().rss_bytes) / (1024.0 * 1024.0);
          recorder.add_frame(fs);

          if (!is_warmup) {
            legacy_sim_ms.push_back(step_total_accum);
            legacy_pose_ms.push_back(pose_ms + ui_build_ms);
            legacy_record_ms.push_back(rec_ms);
            legacy_cpu_ms.push_back(step_total_accum + pose_ms + ui_build_ms + rec_ms);
            legacy_gpu_ms.push_back(gpu_ms);
            draws_sum += double(ws.drawn);
            culled_sum += double(ws.culled);
            indices_sum += double(ws.indices);
            shadow_sum += double(host.showroom.shadow_draw_count());
            shadow_cars_sum += double(host.showroom.shadow_car_count());
            for (const auto& [name, ms] : host.renderer.stage_ms()) {
              size_t k = 0;
              while (k < stage_names.size() && stage_names[k] != name) ++k;
              if (k == stage_names.size()) {
                stage_names.push_back(name);
                stage_ms.emplace_back();
              }
              stage_ms[k].push_back(ms);
            }
          }

          step_total_accum = 0.0;
          step_physics_ms = 0.0;
          step_ai_traffic_ms = 0.0;
          step_world_stream_ms = 0.0;
          ++frame_idx;
        }

        // Clean up temporary UI4/Autoshow state before finalizing.
        if (app.bench_scenario == S::kUi4Heavy) {
          map_overlay.active = false;
          events_page.close();
          settings_page.close();
        }
        if (autoshow_opened) {
          autoshow.close();
          host.showroom.set_instances({});
          host.showroom.set_draw_car(true);
        }

        const double measured_wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - measured_wall0).count();
        const double measured_cpu = process_cpu_seconds() - measured_cpu0;

        // cold_boot_switch: 5 deterministic car hot-switches measuring load + GPU upload + first frame.
        if (app.status == 0 && app.bench_scenario == S::kColdBootSwitch) {
          std::vector<std::string> switch_targets;
          for (const auto& c : app.baked_cars) {
            if (lower_ascii(c) != lower_ascii(app.car)) switch_targets.push_back(c);
            if (switch_targets.size() >= 5) break;
          }
          if (switch_targets.empty() && !app.car.empty()) switch_targets.push_back(app.car);
          for (const auto& sw_car : switch_targets) {
            carview::bench::CarSwitchSample sw{};
            sw.car = sw_car;
            auto gs = std::make_unique<Showroom>(host.renderer);
            gs->set_guest();
            std::string ge;
            const auto sw_t0 = std::chrono::steady_clock::now();
            const bool loaded = gs->load(app.bake_dir, sw_car, ShowroomOptions{}, ge);
            const auto sw_t1 = std::chrono::steady_clock::now();
            const bool uploaded = loaded && gs->upload(ge);
            const auto sw_t2 = std::chrono::steady_clock::now();
            if (uploaded) {
              gs->load_car_lamps(app.game_root_of(), sw_car);
              host.showroom.set_draw_car(false);
              Showroom::Instance inst{};
              inst.car = gs.get();
              inst.model[0] = inst.model[5] = inst.model[10] = inst.model[15] = 1.0f;
              host.showroom.set_instances({inst});
              double srec = 0.0, sgpu = -1.0;
              host.renderer.render_timed(FrameConstants{}, app.width, app.height, srec, sgpu, ge);
              host.showroom.set_instances({});
              host.showroom.set_draw_car(true);
              host.renderer.wait_idle();
            }
            const auto sw_t3 = std::chrono::steady_clock::now();
            sw.load_ms = std::chrono::duration<double, std::milli>(sw_t1 - sw_t0).count();
            sw.upload_ms = std::chrono::duration<double, std::milli>(sw_t2 - sw_t1).count();
            sw.total_ms = std::chrono::duration<double, std::milli>(sw_t3 - sw_t0).count();
            app.bench_startup.car_switches.push_back(std::move(sw));
          }
        }

        // Gather final audio telemetry before stopping the benchmark audio thread.
        carview::bench::AudioMetrics audio_metrics{};
        audio_metrics.enabled = audio_bench_on;
        if (audio_bench_on) {
          const auto atel = app.audio_host.telemetry();
          audio_metrics.blocks_rendered = atel.blocks_rendered;
          audio_metrics.avg_block_ms = atel.avg_block_ms;
          audio_metrics.p95_block_ms = atel.p95_block_ms;
          audio_metrics.worst_block_ms = atel.worst_block_ms;
          audio_metrics.stalls = atel.stalls;
          app.audio_host.stop_device();
        }

        // Assemble hardware, GPU, features, build, memory, streaming, and UI4 atlas reports.
        const auto sys_hw = carview::bench::probe_system_hardware();
        const auto& rdev = host.renderer.device_info();
        const auto vram = host.renderer.vram_stats();
        const double vram_total_mib = double(host.renderer.device_local_bytes()) / (1024.0 * 1024.0);
        const double vram_budget_mib = double(vram.device_local_budget) / (1024.0 * 1024.0);

        carview::bench::GpuInfo gpu_info{};
        gpu_info.device_name = rdev.device_name.empty() ? host.renderer.device_name() : rdev.device_name;
        gpu_info.vendor_id = rdev.vendor_id;
        gpu_info.device_id = rdev.device_id;
        gpu_info.device_type = rdev.device_type;
        gpu_info.api_version_raw = rdev.api_version;
        gpu_info.api_version = carview::bench::format_vulkan_version(rdev.api_version);
        gpu_info.driver_version_raw = rdev.driver_version;
        gpu_info.driver_name = rdev.driver_name;
        gpu_info.driver_info = rdev.driver_info;
        gpu_info.is_moltenvk = rdev.is_moltenvk;
        gpu_info.is_turnip = rdev.is_turnip;
        gpu_info.portability_subset = rdev.portability_subset;
        gpu_info.has_timestamps = rdev.has_timestamps;
        gpu_info.vram_total_mib = vram_total_mib;
        gpu_info.vram_budget_mib = vram_budget_mib;

        carview::bench::FeatureFlags feat{};
        feat.gpu_driven = host.renderer.gpu_driven_supported();
        feat.hiz = host.renderer.gpu_driven_supported() && host.showroom.hiz_image_view() != VK_NULL_HANDLE;
        feat.bindless = host.renderer.gpu_driven_supported();
        feat.vrs_supported = host.renderer.vrs_supported();
        feat.vrs_active = host.renderer.vrs_active();
        feat.hdr10_supported = host.renderer.hdr10_supported();
        feat.scrgb_supported = host.renderer.scrgb_supported();
        feat.hdr_mode = host.renderer.hdr_active() == carview::modern::HdrMode::kHdr10   ? "hdr10"
                        : host.renderer.hdr_active() == carview::modern::HdrMode::kScRgb ? "scrgb"
                                                                                    : "off";
        feat.upscaler = carview::modern::upscaler_name(host.renderer.active_upscaler().active);
        feat.frame_gen = carview::modern::frame_gen_name(host.renderer.active_frame_gen());
        feat.aa_mode = host.active_graphics.aa == carview::modern::AaMode::kFxaaAll     ? "fxaa_all"
                       : host.active_graphics.aa == carview::modern::AaMode::kFxaaWorld ? "fxaa_world"
                                                                                   : "msaa";
        feat.ssao = host.active_graphics.ssao == carview::modern::SsaoQuality::kUltra    ? "ultra"
                    : host.active_graphics.ssao == carview::modern::SsaoQuality::kHigh   ? "high"
                    : host.active_graphics.ssao == carview::modern::SsaoQuality::kMedium ? "medium"
                    : host.active_graphics.ssao == carview::modern::SsaoQuality::kLow    ? "low"
                                                                                    : "off";
        feat.shadows = !app.shadows_flag.empty() ? app.shadows_flag : (app.low_preset ? "low" : "high");
        feat.vsync = host.active_graphics.vsync == carview::modern::VSync::kImmediate ? "immediate"
                     : host.active_graphics.vsync == carview::modern::VSync::kMailbox ? "mailbox"
                                                                                 : "fifo";
        feat.msaa = host.renderer.sample_count();
        feat.anisotropy = host.renderer.sampler_anisotropy();
        feat.render_scale = host.active_graphics.scale;
        feat.base_540p = host.active_graphics.base_540p;
        feat.z_prepass = app.z_prepass;
        feat.reverse_z = app.reverse_z;
        feat.bloom = app.bloom_flag >= 0 ? (app.bloom_flag == 1) : !app.low_preset;
        feat.full_fx = app.bench_full_fx;
        feat.bench_audio = audio_bench_on;

        carview::bench::BuildInfo binfo{};
#ifdef SHIFT_GIT_COMMIT
        binfo.git_commit = SHIFT_GIT_COMMIT;
#else
        binfo.git_commit = "unknown";
#endif
#ifdef SHIFT_BUILD_CONFIG
        binfo.build_config = SHIFT_BUILD_CONFIG;
#else
        binfo.build_config = "Release";
#endif
        binfo.preset = app.preset_name;
        binfo.device_label = app.device_name.empty() ? gpu_info.device_name : app.device_name;
        binfo.scenario = std::string(carview::bench::scenario_name(app.bench_scenario));
        binfo.output_width = app.width;
        binfo.output_height = app.height;
        binfo.render_width = host.renderer.render_width();
        binfo.render_height = host.renderer.render_height();
        binfo.target_seconds = app.seconds;
        binfo.warmup_seconds = app.bench_warmup_sec;

        app.bench_startup.pipeline_compile_ms = host.renderer.pipeline_compile_ms() + host.showroom.pipeline_compile_ms();

        const auto pmem = carview::bench::query_process_memory();
        carview::bench::MemoryMetrics mem_m{};
        mem_m.rss_current_mib = double(pmem.rss_bytes) / (1024.0 * 1024.0);
        mem_m.rss_peak_mib = double(pmem.peak_rss_bytes) / (1024.0 * 1024.0);
        mem_m.vma_blocks = vram.block_count;
        mem_m.vma_allocs = vram.allocation_count;
        mem_m.vma_used_mib = double(vram.allocation_bytes) / (1024.0 * 1024.0);
        mem_m.vma_reserved_mib = double(vram.block_bytes) / (1024.0 * 1024.0);
        mem_m.vram_usage_mib = double(vram.device_local_usage) / (1024.0 * 1024.0);
        mem_m.vram_budget_mib = vram_budget_mib;
        mem_m.vram_total_mib = vram_total_mib;

        const auto wts = host.showroom.world_texture_stats();
        const uint64_t staging_bytes1 = host.renderer.staging_upload_bytes() + host.showroom.world_staging_bytes();
        carview::bench::StreamingMetrics st_m{};
        st_m.resident_textures = wts.resident_count;
        st_m.resident_textures_mib = double(wts.resident_bytes) / (1024.0 * 1024.0);
        st_m.texture_uploads = wts.uploads;
        st_m.texture_evictions = wts.evictions;
        st_m.texture_reuploads = wts.reuploads;
        st_m.retired_pending = wts.retired_pending;
        st_m.descriptor_pools = wts.descriptor_pools;
        st_m.staging_upload_bytes = staging_bytes1 >= staging_bytes0 ? (staging_bytes1 - staging_bytes0) : 0;

        const auto astats = carview::ui::atlas_stats();
        carview::bench::Ui4AtlasMetrics ui_m{};
        ui_m.atlas_size = astats.atlas_size;
        ui_m.glyph_rows_used = astats.glyph_rows_used;
        ui_m.persistent_rows_used = astats.persistent_rows_used;
        ui_m.transient_rows_used = astats.transient_rows_used;
        ui_m.utilization_pct = astats.utilization_pct;
        ui_m.transient_evictions = astats.transient_evictions;
        ui_m.glyph_count = astats.glyph_count;
        ui_m.image_count = astats.image_count;

        const auto report = recorder.finalize(sys_hw, gpu_info, feat, binfo, app.bench_startup, audio_metrics,
                                              ui_m, mem_m, st_m, measured_wall, measured_cpu);

        // Emit comprehensive summary + legacy-compatible lines.
        std::printf("%s", carview::bench::format_console_summary(report).c_str());

        auto pct = [](std::vector<double> v, double q) {
          if (v.empty()) return 0.0;
          std::sort(v.begin(), v.end());
          return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
        };
        const size_t n = legacy_cpu_ms.size();
        if (n) {
          std::printf("bench: world draws per frame %.0f drawn, %.0f culled, %.0fk of %.0fk triangles drawn\n",
                      draws_sum / double(n), culled_sum / double(n), indices_sum / double(n) / 3000.0,
                      double(host.showroom.world_triangle_count()) / 1000.0);
          if (shadow_sum > 0)
            std::printf("bench: shadow casters per frame %.0f world draws, %.2f traffic cars\n",
                        shadow_sum / double(n), shadow_cars_sum / double(n));
        }

        if (!app.bench_json_path.empty()) {
          std::string jerr;
          if (!carview::bench::write_json(app.bench_json_path, report, jerr)) {
            std::fprintf(stderr, "carview: %s\n", jerr.c_str());
            app.status = 1;
          } else {
            std::printf("bench: wrote JSON -> %s\n", app.bench_json_path.string().c_str());
          }
        }
        if (!app.bench_csv_path.empty()) {
          std::string cerr;
          if (!carview::bench::write_csv(app.bench_csv_path, recorder.frames(), cerr)) {
            std::fprintf(stderr, "carview: %s\n", cerr.c_str());
            app.status = 1;
          } else {
            std::printf("bench: wrote CSV  -> %s (%zu frames)\n", app.bench_csv_path.string().c_str(),
                        recorder.frames().size());
          }
        }
        if (!app.bench_switch_car.empty()) {
          const auto t_sw0 = std::chrono::steady_clock::now();
          std::string sw_err;
          ShowroomOptions sw_opts;
          sw_opts.game_root = app.game_root_of();
          const bool sw_ok = host.showroom.reload_car(app.bake_dir, app.bench_switch_car, sw_opts, sw_err);
          if (sw_ok) {
            std::vector<std::string> sw_notes;
            const auto sw_params = drive::params_for_baked_car(host.showroom.car(), app.bake_dir, &sw_notes);
            drive::Vehicle sw_veh(sw_params);
            sw_veh.reset(vehicle.position(), host.start_heading);
            host.showroom.set_wheel_poses(poses);
            double sw_rec_ms = 0, sw_gpu_ms = 0;
            host.renderer.render_timed(FrameConstants{}, app.width, app.height, sw_rec_ms, sw_gpu_ms, sw_err);
          }
          const double sw_ms =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_sw0).count();
          if (sw_ok) {
            std::printf("bench: hot car switch (%s -> %s): %.2f ms (world_textures_kept=%zu, gpu_draws_kept=%u)\n",
                        app.car.c_str(), app.bench_switch_car.c_str(), sw_ms, host.showroom.world_texture_count(),
                        host.showroom.gpu_resident_draws());
          } else {
            std::fprintf(stderr, "bench: hot car switch failed: %s\n", sw_err.c_str());
          }
        }
        (void)pct;
}

}  // namespace carview
