#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "cars/livery.h"
#include "cars/mods.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "fh1data/spa.h"
#include "garage/garage.h"
#include "profile/drive_save.h"
#include "settings.h"
#include "slice12_runtime.h"

namespace carview {

namespace {

drive::CarParams build_session_vehicle_params(DriveApp& app, DriveWorldHost& host,
                                              const cars::bake::CarInfo& info,
                                              audio::UpgradeLevels& audio_upgrades,
                                              int& audio_trans_level, int& audio_turbo_level,
                                              int& audio_sc_level,
                                              std::vector<std::string>& physics_notes) {
        if (app.have_garage && app.parts_db.valid())
          if (const garage::Car* gc = app.career_garage.selected_car())
            if (auto it = app.baked_ids.find(gc->car_id);
                it != app.baked_ids.end() && lower_ascii(it->second) == lower_ascii(app.car)) {
              const auto t0 = std::chrono::steady_clock::now();
              const auto db = app.parts_db.get();
              cars::CarPhysics rows;
              std::string e = "no parts database";
              if (db && db->physics(gc->car_id, garage::fitted(*gc), rows, &e)) {
                audio_upgrades.intake = int(rows.get("List_UpgradeEngineIntake.Level", 0));
                audio_upgrades.exhaust = int(rows.get("List_UpgradeEngineExhaust.Level", 0));
                audio_trans_level = int(rows.get("List_UpgradeDrivetrainTransmission.Level", 0));
                audio_turbo_level = int(rows.get("List_UpgradeEngineTurboSingle.Level", 0));
                audio_sc_level = int(rows.get("List_UpgradeEngineCSC.Level", 0));
                std::printf("garage: car %lld (%s) with its parts, %.0f kg (%.1f s)\n", (long long)gc->id,
                            it->second.c_str(), rows.get("List_UpgradeCarBodyWeight.Mass", 0),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                return drive::params_for_fitted_car(info, app.bake_dir, rows, &physics_notes, &gc->tune);
              }
              physics_notes.push_back("garage parts not applied: " + e);
            }
        return drive::params_for_baked_car(info, app.bake_dir, &physics_notes);
}

}  // namespace

DriveSession::DriveSession(DriveApp& a, DriveWorldHost& h)
    : app(a),
      host(h),
      info(h.showroom.car()),
      vehicle(build_session_vehicle_params(a, h, h.showroom.car(), audio_upgrades, audio_trans_level,
                                           audio_turbo_level, audio_sc_level, physics_notes)),
      race_models{a.race.get()},
      festival_guard{&h.showroom, &festival_guests},
      save_due(a.new_career_save_pending),
      live_motion_blur(a.motion_blur_flag >= 0 ? a.motion_blur_flag == 1 : !a.low_preset),
      live_dof(a.dof_flag >= 0 ? a.dof_flag == 1 : !a.low_preset),
      live_photo_through_walls(a.photo_through_walls_cli),
      live_photo_wider_limits(a.photo_wider_limits_cli),
      photos_dir(default_photos_dir()),
      replays_dir(default_replays_dir()),
      prev_rumble_gear(vehicle.gear()) {}

void DriveSession::init_session() {
      // Apply any custom Paint Shop / Vinyl Livery (.liv) saved on the active garage car.
      if (app.have_garage) {
        if (const garage::Car* gc = app.career_garage.selected_car()) {
          if (!gc->livery_file.empty() && app.baked_ids.count(gc->car_id) &&
              lower_ascii(app.baked_ids.at(gc->car_id)) == lower_ascii(app.car)) {
            std::filesystem::path liv_p(gc->livery_file);
            if (liv_p.is_relative() && !app.save_path.empty()) liv_p = app.save_path.parent_path() / liv_p;
            cars::LiveryDocument ldoc;
            if (cars::load_livery_file(liv_p, ldoc, nullptr)) {
              host.showroom.set_paint_spec(ldoc.paint);
              if (!ldoc.layers.empty()) {
                cars::LiveryLibrary llib;
                const auto gr = app.game_root_of();
                if (!gr.empty()) {
                  const auto lzip = gr / "media" / "Livery.zip";
                  if (std::filesystem::exists(lzip)) llib.open(lzip, nullptr);
                }
                std::vector<uint8_t> liv_rgba;
                if (cars::composite_livery(ldoc, llib.is_open() ? &llib : nullptr,
                                           host.showroom.livery_planes().empty() ? nullptr : &host.showroom.livery_planes(),
                                           liv_rgba, 1024)) {
                  std::string lerr;
                  host.showroom.set_custom_livery(liv_rgba.data(), 1024, 1024, lerr);
                }
              }
            }
          }
        }
      }
      for (const auto& n : physics_notes) std::fprintf(stderr, "carview: physics: %s\n", n.c_str());
      if (!app.world_dir.empty()) {
        if (app.have_spawn) vehicle.reset({app.spawn[0], app.spawn[1], app.spawn[2]}, app.spawn[3]);
        else vehicle.reset(host.start, host.start_heading);
      }
      if (app.sound && (!app.headless || !app.audio_out.empty() || app.bench_audio || app.bench_full_fx)) {
        const auto aud_t0 = std::chrono::steady_clock::now();
        std::string sound_note;
        if (app.audio_host.load_car(app.sound_dir, app.game_root_of(), app.state_root, app.car, audio_upgrades, vehicle.params().idle_rpm,
                                vehicle.params().redline_rpm, &sound_note))
          std::printf("%s\n", sound_note.c_str());
        else
          std::fprintf(stderr, "carview: no engine sound (%s)\n", sound_note.c_str());
        app.bench_startup.audio_load_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - aud_t0).count();
      } else {
        app.audio_host.clear_car();
      }
      if (app.skill_tracker) app.skill_tracker->reset_session_clock();
      std::unique_ptr<TrafficView> traffic;
      if (app.status == 0 && app.race) {
        std::string e;
        if (!host.streamer || !app.race->load_cars(host.renderer, *host.ground, e)) {
          std::fprintf(stderr, "carview: race: %s\n", host.streamer ? e.c_str() : "needs the streamed world");
          app.status = 1;
        }
        if (app.status == 0 && !app.render_ground) app.race->clip_room(host.collision);
      }
      if (app.status == 0 && host.streamer && app.colorado_world() &&
          (app.traffic_flag == 1 || (app.traffic_flag < 0 && !app.headless && (!app.race || app.race->uses_traffic())))) {
        // The test driver keeps to the right lane among traffic (SPEC-TRAFFIC,
        // Driving a lane: 2.2 m right of the centre line).
        host.follower.set_keep_right(2.2);
        traffic = std::make_unique<TrafficView>(host.renderer, host.showroom, app.bake_dir, app.headless && !app.async_stream);
        const auto game_root = app.game_root_of();
        if (!traffic->open(game_root, &host.ground, app.hour, app.traffic_seed, app.error, app.race != nullptr)) {
          std::fprintf(stderr, "carview: note: no traffic: %s\n", app.error.c_str());
          app.error.clear();
          traffic.reset();
        }
      }
      if (app.status == 0 && host.streamer && app.colorado_world() && !app.race && (!app.headless || app.demo_h2h)) {
        std::string ferr;
        traffic::TrafficConfig fcfg;
        const auto game_root = app.game_root_of();
        if (traffic::read_nav(game_root / "media" / "tracks" / "colorado" / "colorado.nav", festival_nav, &ferr) &&
            traffic::read_traffic_config(game_root, fcfg, &ferr, false)) {
          festival_roads.build(festival_nav);
          std::set<int64_t> traffic_ids;
          for (const auto& tm : fcfg.models) traffic_ids.insert(tm.id);
          std::vector<freeroam::FestivalRacerSpec> roster;
          for (const auto& [cid, media] : app.baked_ids) {
            if (traffic_ids.count(cid) || media == app.car) continue;
            freeroam::FestivalRacerSpec sp;
            sp.car_id = cid;
            sp.car_media = media;
            if (app.have_career_catalog) {
              if (const auto* cs = app.career_catalog.find_car(cid)) {
                sp.pi = !app.class_bands.empty() ? garage::shown_index(cs->stock_pi, app.class_bands) : int(cs->stock_pi);
                const int cl = !app.class_bands.empty() ? garage::class_of(cs->stock_pi, app.class_bands) : cs->stock_class;
                if (cl >= 0 && size_t(cl) < app.class_bands.size()) sp.car_class = app.class_bands[size_t(cl)].name;
              }
            }
            if (sp.car_class.empty()) sp.car_class = "B";
            roster.push_back(std::move(sp));
            if (roster.size() >= 12) break;
          }
          if (!roster.empty()) {
            const size_t roster_n = roster.size();
            festival_mgr = std::make_unique<freeroam::FestivalManager>(festival_nav, festival_roads, fcfg, app.traffic_seed + 17);
            std::vector<std::string> ai_names;
            if (!*app.car_names_loaded) carview::car_name("");
            const auto ai_db = app.parts_db.get();
            if (race::load_ai_player_names(game_root, ai_names, app.car_names.get(), app.state_root, ai_db.get())) {
              festival_mgr->set_driver_names(std::move(ai_names));
            }
            festival_mgr->set_roster(std::move(roster));
            festival_mgr->set_hour(app.hour);
            festival_mgr->set_wristband_level(app.collected_wristband);
            festival_mgr->set_ground([&](double x, double z, double hint, double& y) {
              if (!host.ground) return false;
              drive::Hit h;
              if (!host.ground->raycast({x, hint + 4, z}, {0, -1, 0}, 12, h)) return false;
              y = hint + 4 - h.distance;
              return true;
            });
            std::printf("festival AI: roaming roster ready (%zu models)\n", roster_n);
          }
        }
      }
      if (app.status == 0 && host.streamer) {
        std::string why;
        safe_reset_ok = safe_reset.open(app.game_root_of(), &why);
        if (!safe_reset_ok) std::fprintf(stderr, "carview: note: reset on the nearest road triangle (%s)\n", why.c_str());
      }
      if (!app.contacts_log_path.empty()) {
        contact_log.f = std::fopen(app.contacts_log_path.string().c_str(), "w");
        if (!contact_log.f) std::fprintf(stderr, "carview: cannot write %s\n", app.contacts_log_path.string().c_str());
      }
      app.new_career_save_pending = false;
      settings_page.set_available_luts(scan_lut_files(carview::settings_path().parent_path()));
      slice12.save_tree_label = (app.slice12_modded_save || app.mods_session.isolation().isolated) ? "user-modded" : "user";
      LiveSettings boot_live_settings{};
      {
        SettingsPage boot_sp;
        boot_sp.load(read_settings(settings_path(), nullptr));
        boot_live_settings = boot_sp.live_settings();
        if (app.slice12_modded_save) boot_live_settings.trainer_cheats = true;
        slice12.trainer.config.cheats_enabled = boot_live_settings.trainer_cheats;
        if (boot_live_settings.trainer_saved_road.has_value())
          slice12.trainer.config.saved_map_road = boot_live_settings.trainer_saved_road;
        slice12.trainer.pacing = boot_live_settings.festival_pacing;
        slice12.trainer.hidden_cars = boot_live_settings.hidden_cars;
        slice12.wheel_shaping = boot_live_settings.wheel_shaping;
        slice12.ffb_cfg = boot_live_settings.wheel_ffb;
        slice12.ds_cfg = boot_live_settings.dualsense;
        slice12.ht_cfg = boot_live_settings.head_tracking;
        slice12.cam_editor_cfg = boot_live_settings.camera_editor;
        slice12.imm_cam_cfg = boot_live_settings.immersive_camera;
        slice12.tel_cfg = boot_live_settings.telemetry_udp;
      }
      save_page.set_media_resolver([&](int64_t id) {
        auto it = app.baked_ids.find(id);
        return it == app.baked_ids.end() ? std::string() : it->second;
      });
      promo_tracker.load(app.have_saved ? app.saved.promo_cars : std::vector<int>{}, std::max(240, int(app.baked_cars.size())));
      ach_isolated = app.slice12_modded_save || app.mods_session.isolation().isolated;
      achievements_path = [&]() {
        const auto legacy = carview::default_achievements_path();
        if (!app.save_path.empty()) {
          const auto scoped = app.save_path.parent_path() / "achievements.txt";
          std::error_code ec;
          if (!ach_isolated && !std::filesystem::exists(scoped, ec) && !legacy.empty() &&
              std::filesystem::exists(legacy, ec) && scoped != legacy) {
            std::filesystem::create_directories(scoped.parent_path(), ec);
            std::filesystem::copy_file(legacy, scoped, std::filesystem::copy_options::skip_existing, ec);
          }
          return scoped;
        }
        return legacy;
      }();
      
      {
        std::string spa_note;
        if (fh1::load_spa_packages(app.game_root_of(), app.state_root, spa_db, app.language, &spa_note) && !app.headless)
          std::printf("achievements: %s\n", spa_note.c_str());
      }
      
      achievements.init(spa_db);
      if (app.demo_achievements_png.empty()) achievements.load_state(achievements_path);
      achievements.set_unlock_sound_callback([&]() { app.audio_host.trigger_ui(audio::UiCue::kAchievementReward); });
      {
        drive::ReplayMetadata rm;
        rm.event_name = app.race ? app.race->hud_info().event : "COLORADO FREE ROAM";
        rm.world_name = app.world_dir.empty() ? "Colorado" : app.world_dir.filename().string();
        rm.tod_sec = app.tod_clock.tod_sec;
        rm.sample_hz = 60.0;
        rm.is_race = app.race != nullptr;
        rm.roster.push_back({0u, int32_t(info.car_id), app.car, carview::car_name(app.car), true, false});
        replay_rec.begin(rm, drive::ReplayRecorder::kDefaultMaxFrames, !app.race);
      }
      cockpit_dial_max_speed = drive::CockpitAnimState::estimate_dial_max_speed_mps(vehicle.params());
      cockpit_dial_max_boost = drive::CockpitAnimState::gauge_max_boost_psi(vehicle.params());
      cockpit_has_fi = vehicle.params().has_turbo || vehicle.params().has_supercharger;
      int light_override = 0;  // 0 auto, 1 low beam, 2 full beam, 3 fog
      host.showroom.set_wind(app.tod_system.post_config.wind_speed, app.tod_system.post_config.wind_amplitude);
      if (app.have_saved && app.saved.has_controls) live_controls = app.saved.controls;
      {
        SettingsPage init_settings;
        init_settings.load(read_settings(settings_path(), nullptr));
        const LiveSettings init_ls = init_settings.live_settings();
        if (!app.have_saved || !app.saved.has_controls) live_controls = init_ls.controls;
        car_damage.mode = drive::DamageMode(init_ls.damage_mode);
      }
      live_sound_enabled = true;
      live_radio_enabled = true;
      loaded_camera_effects = live_controls.camera_effects;
      if (app.demo_achievements_png.empty()) eval_achievements(false);
      club_check();
      if (std::getenv("SHIFT_CLUB_BEGIN") && club_gate.empty() && !club_record()) {
        drive::SavedClubCar rec;
        rec.car_id = club_car;
        rec.challenges.assign(app.club_data.cars.at(club_car).size(), {0u, false});
        app.saved.club.push_back(rec);
        club_check();
      }
      if (app.headless || club_gate.empty())
        std::printf("1000 club: %s\n", !club_gate.empty() ? club_gate.c_str()
                                        : club_runs.empty() && !club_record() ? "not begun in this car"
                                                                              : (std::to_string(club_runs.size()) +
                                                                                 " challenges running in this car").c_str());
      if (std::getenv("SHIFT_CLUB_PAGE")) {  // debug: the page as the menu shows it
        std::string title;
        std::vector<std::string> lines;
        club_page(title, lines);
        std::printf("%s\n", title.c_str());
        for (const auto& l : lines) std::printf("  %s\n", l.c_str());
      }
      // SPEC-CAMERAS' rig (the old chase when it cannot load, and for
      // --view front).
      
      
      
      rumble_enabled = live_controls.rumble_enabled;
      rumble_scale = live_controls.rumble_scale;
      prev_rumble_gear = vehicle.gear();
      prev_rumble_backfire = 0;
      
      if (!app.world_dir.empty()) {
        std::string note;
        have_rig = load_camera_rig(app.game_root_of(), app.bake_dir, info, vehicle, rig, note, loaded_camera_effects,
                                   host.showroom.has_cockpit_mesh());
        if (!have_rig) std::fprintf(stderr, "carview: note: the camera spec's rig is off: %s\n", note.c_str());
        else if (!note.empty()) std::fprintf(stderr, "carview: note: camera: %s\n", note.c_str());
        if (have_rig) {
          host.showroom.set_cockpit_wheel_offsets(
              {float(rig.car().override("CockpitWheelPositionOffsetX")),
               float(rig.car().override("CockpitWheelPositionOffsetY")),
               float(rig.car().override("CockpitWheelPositionOffsetZ"))},
              float(rig.car().override("CockpitWheelRotationOffset")));
        }
        rig.set_mode(drive::CameraMode(std::clamp(app.camera_mode, 0, drive::kCameraModes - 1)));
        cars::Install rumble_install;
        const char* rumble_state = std::getenv("SHIFT_STATE_ROOT");
        std::string rumble_xml, rumble_err;
        drive::RumbleConfig rumble_cfg;
        if (cars::discover_install(app.game_root_of(), rumble_state ? rumble_state : "", "", rumble_install, nullptr) &&
            cars::load_rumble_xml(rumble_install, rumble_xml, &rumble_err) &&
            drive::parse_controller_rumble(rumble_xml, rumble_cfg, &rumble_err)) {
          gamepad_rumble.set_config(rumble_cfg);
        }
      }
      slice12.apply_live_settings(boot_live_settings, rig, settings_path().parent_path());
      rewind_buf.set_enabled(live_controls.assists.rewind);
      rewind_buf.start_session(vehicle.time());
      apply_vehicle_assists(vehicle, live_controls.assists);
      app.race_options.assists = to_difficulty_assists(live_controls.assists);
      braking_table = drive::BrakingTable::build(vehicle.params());
      driving_line_mode =
          static_cast<drive::DrivingLineMode>(std::clamp(live_controls.assists.driving_line, 0, 2));
      intro =
          app.cli_dev_hints ? (carview::car_name(app.car) + " - [ ] pick a car  R back on the road  T time of day  Esc menu")
                        : std::string{};
      if (const auto gr = app.game_root_of(); !gr.empty()) {
        const auto pzip = gr / "media" / "physics.zip";
        cars::load_physics_settings(pzip, hub_phys_settings, nullptr);
        garage::load_pi_config(pzip, hub_pi_cfg, nullptr);
      }
      grid_menu_open = app.race != nullptr && !app.headless && !app.bench && !app.race_options.autopilot;
      app.campaign_owns_post_race = false;
}

void DriveSession::run(bool& session_again) {
  print_scenery_summary();
  if (app.status != 0) {
  } else if (!app.world_reference_path.empty()) {
    run_world_reference();
  } else if (app.bench) {
    run_benchmark();
  } else if (run_demo_captures()) {
  } else if (app.headless) {
    run_headless_drive();
  } else {
    run_window_loop();
  }
  finalize_session(session_again);
}

void DriveSession::finalize_session(bool& session_again) {
      if (!app.map_png.empty() && app.status == 0) {
        if (!app.map_view.ready()) {
          std::fprintf(stderr, "carview: no map to write\n");
        } else {
          const std::filesystem::path prefix(app.map_png);
          auto name = [&](const char* suffix) {
            return (prefix.parent_path() / (prefix.stem().string() + suffix)).string();
          };
          const MapScene sc = map_scene();
          const auto c = app.map_view.roads_centre();
          const HudImage whole = app.map_view.pause_map(sc, 1600, 900, 1, c[0], c[1]);
          const HudImage close = app.map_view.pause_map(sc, 1600, 900, 6, sc.x, sc.z);
          const HudImage mini = app.map_view.minimap(sc, 300);
          bool ok = write_png(name("_map.png"), whole.width, whole.height, 4, whole.rgba.data());
          ok = ok && write_png(name("_map_near.png"), close.width, close.height, 4, close.rgba.data());
          ok = ok && write_png(name("_minimap.png"), mini.width, mini.height, 4, mini.rgba.data());
          if (!ok) app.status = 1;
          std::printf("maps: %s, %s, %s\n", name("_map.png").c_str(), name("_map_near.png").c_str(),
                      name("_minimap.png").c_str());
        }
      }
      if (!app.pause_map_png.empty() && app.status == 0) {
        if (!app.map_view.ready()) {
          std::fprintf(stderr, "carview: no map to write\n");
        } else {
          MapScene sc = map_scene();
          // Select an Outpost marker near the viewport centre if available so the card shows real Outpost metadata.
          int best_idx = -1;
          double best_d = 1e18;
          for (size_t mi = 0; mi < sc.markers.size(); ++mi) {
            if (sc.markers[mi].kind == MapMarker::kOutpost) {
              const double d = std::hypot(sc.markers[mi].x - sc.x, sc.markers[mi].z - sc.z);
              if (d < best_d) best_d = d, best_idx = int(mi);
            }
          }
          const double cx = best_idx >= 0 ? 0.5 * (sc.x + sc.markers[size_t(best_idx)].x) : sc.x;
          const double cz = best_idx >= 0 ? 0.5 * (sc.z + sc.markers[size_t(best_idx)].z) : sc.z;
          sc.selected_marker = best_idx;
          // 100% edge-to-edge fullscreen viewport (0, 0, width, height) — zero outer margins!
          HudImage img = app.map_view.pause_map(sc, int(app.width), int(app.height), app.cli_map_zoom, cx, cz);
          for (int layer : {0, 1, 3}) host.renderer.set_overlay({}, 0, 0, 0, 0, layer);
          mm_size = 0;
          host.renderer.set_overlay(std::move(img.rgba), img.width, img.height, 0, 0, 2);
          map_overlay = carview::MapOverlayHud{};
          map_overlay.active = true;
          map_overlay.active_filter = 0;
          for (int fi = 0; fi < int(MapFilter::kCount); ++fi)
            map_overlay.filter_tabs.push_back(map_filter_name(MapFilter(fi)));
          map_overlay.roads_discovered = app.road_discovery.ready() ? app.road_discovery.discovered_count() : 0;
          map_overlay.roads_total = app.road_discovery.ready() ? app.road_discovery.total_roads() : 216;
          map_overlay.zoom = app.cli_map_zoom;
          map_overlay.treasure_map = app.reveal || app.treasure_map_setting;
          map_overlay.gps_active = app.gps_nav.active();
          map_overlay.gps_hint = app.gps_nav.active() ? "SET / CLEAR GPS (G)" : "SET GPS ROUTE (G)";
          if (best_idx >= 0) {
            const auto& mk = sc.markers[size_t(best_idx)];
            map_overlay.title = mk.title;
            map_overlay.subtitle = mk.subtitle;
            map_overlay.distance_m = std::hypot(mk.x - sc.x, mk.z - sc.z);
            map_overlay.action_hint = mk.discovered ? "FAST TRAVEL / HUB" : "SET GPS ROUTE";
          } else {
            map_overlay.title = "COLORADO MAP";
            map_overlay.subtitle = "PAN TO ANY ROAD, EVENT OR OUTPOST";
            map_overlay.action_hint = "FAST TRAVEL HERE";
          }
          set_hud(int(app.width), int(app.height), "");
          std::vector<uint8_t> rgba;
          host.renderer.set_camera_projection(host.cam_proj.near_z, host.cam_proj.far_z, host.cam_proj.proj00, host.cam_proj.proj11);
          if (!host.renderer.render_offscreen(FrameConstants{}, app.width, app.height, rgba, app.error)) {
            std::fprintf(stderr, "carview: pause-map-png: %s\n", app.error.c_str());
            app.status = 1;
          } else {
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            if (!write_png(app.pause_map_png.string(), app.width, app.height, 4, rgba.data())) app.status = 1;
            else std::printf("pause map png: %s\n", app.pause_map_png.string().c_str());
          }
          map_overlay.active = false;
        }
      }
      if (host.free_roam)
        std::printf("free roam: %d of %zu signs (upgrades %d%% off), %d of %zu barn finds, %.1f km driven\n",
                    host.free_roam->signs_found(), app.collectibles.signs.size(),
                    freeroam::upgrade_discount(host.free_roam->signs_found()), host.free_roam->barns_found(),
                    app.collectibles.barns.size(), app.progress.total_distance / 1000);
      const bool will_hot_reload =
          (app.status == 0 && app.rally_world_switch == 0 && (app.leave_race || !app.next_event.empty() || !app.next_car.empty() || hot_reload_car));
      if (!skip_end_save) save_now("end");
      if (!skip_end_save && !app.again && !will_hot_reload && app.rally_world_switch == 0) app.save_session.on_exit(vehicle.time());
      if (traffic) std::printf("%s\n", traffic->summary().c_str());
      // Handle car switch / paint / upgrades / race start & leave in-place without tearing down
      // Renderer, pipelines, WorldGpu, world textures, or World (~12 s -> <0.25 s).
      if (app.status == 0 && app.rally_world_switch == 0) {
        if (hot_reload_car) {
          const drive::Vec3 cur_p = vehicle.position();
          const drive::Vec3 cur_f = drive::rotate(vehicle.orientation(), {0, 0, 1});
          app.spawn[0] = cur_p.x, app.spawn[1] = cur_p.y, app.spawn[2] = cur_p.z, app.spawn[3] = std::atan2(cur_f.x, cur_f.z);
          app.have_spawn = true;
          session_again = true;
        }
        if (app.leave_race) {
          app.leave_race = false;
          if (app.race) {
            app.race->release_models();
            app.race.reset();
            app.race_event.clear();
            std::printf("race: left for free roam\n");
          }
          if (!app.pre_loaner_car.empty()) {
            app.car = app.pre_loaner_car;
            if (app.pre_loaner_garage_id > 0) app.career_garage.selected = app.pre_loaner_garage_id;
            app.pre_loaner_car.clear();
            app.pre_loaner_garage_id = 0;
          }
          session_again = true;
        }
        if (!app.next_event.empty()) {
          std::string why;
          const std::string id = app.next_event;
          app.next_event.clear();
          if (app.race) app.race->release_models();
          app.race.reset();
          if (app.start_race(id, why)) {
            app.race_event = id;
            std::printf("race: %s from the pause menu\n", id.c_str());
          } else {
            std::fprintf(stderr, "carview: event %s: %s\n", id.c_str(), why.c_str());
            app.race_event.clear();
            if (!app.pre_loaner_car.empty()) {
              app.car = app.pre_loaner_car;
              if (app.pre_loaner_garage_id > 0) app.career_garage.selected = app.pre_loaner_garage_id;
              app.pre_loaner_car.clear();
              app.pre_loaner_garage_id = 0;
            }
          }
          session_again = true;
          SDL_SetWindowTitle(app.window, ("carview drive - loading " + id).c_str());
        }
        if (!app.next_car.empty()) {
          if (app.race) {
            std::printf("race: left for a car change\n");
            app.race->release_models();
            app.race.reset();
            app.race_event.clear();
            app.pre_loaner_car.clear();
            app.pre_loaner_garage_id = 0;
          }
          const drive::Vec3 cur_p = vehicle.position();
          const drive::Vec3 cur_f = drive::rotate(vehicle.orientation(), {0, 0, 1});
          if (!app.have_spawn) {
            app.spawn[0] = cur_p.x, app.spawn[1] = cur_p.y, app.spawn[2] = cur_p.z, app.spawn[3] = std::atan2(cur_f.x, cur_f.z);
            app.have_spawn = true;
          }
          app.car = app.next_car;
          app.next_car.clear();
          session_again = true;
          SDL_SetWindowTitle(app.window, ("carview drive - loading " + app.car).c_str());
        }
      }
}

void DriveSession::census_frame(uint32_t width, uint32_t height) {
        const auto& mesh = host.streamer->mesh();
        if (!mesh.has_render_data()) return;
        const ShowroomFrame& fr = host.showroom.frame();
        const float* tr = host.showroom.world_transform();
        float clip[16];
        for (int c = 0; c < 4; ++c)
          for (int r = 0; r < 4; ++r) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += fr.view_projection[k * 4 + r] * tr[c * 4 + k];
            clip[c * 4 + r] = sum;
          }
        const double ox = host.showroom.world_origin_x(), oz = host.showroom.world_origin_z();
        auto failed = [&](uint32_t record) {
          return record == world::kNoTexture || host.texture_loader.failed.count(record) != 0;
        };
        // Why a material falls back, or empty.
        std::vector<std::string> why(mesh.materials.size());
        for (size_t i = 0; i < mesh.materials.size(); ++i) {
          const auto& m = mesh.materials[i];
          auto slot = [&](int k, uint32_t record) {
            char b[96];
            std::snprintf(b, sizeof b, "slot %d %s (value %d, type %d)", k,
                          record == world::kNoTexture ? "no file" : "failed decode", m.slot_values[size_t(k)],
                          m.slot_types[size_t(k)]);
            return std::string(b);
          };
          if (m.effect.rfind("LAKE", 0) == 0) continue;  // drawn flat on purpose
          if (failed(m.albedo)) why[i] = slot(0, m.albedo);
          else if ((m.layers == world::Layers::kVertexBlend || m.layers == world::Layers::kSplat) && failed(m.layer_b))
            why[i] = slot(1, m.layer_b);
          else if (m.layers == world::Layers::kSplat && failed(m.splat))
            why[i] = slot(2, m.splat);
        }
        struct Group {
          double px = 0;
          size_t tris = 0;
          double sum[3] = {0, 0, 0};
          float ymin = 1e9f, ymax = -1e9f;
        };
        std::map<std::tuple<std::string, std::string, std::string>, Group> area;
        double total = 0;
        for (size_t t = 0; t < mesh.material.size(); ++t) {
          const std::string& w = why[mesh.material[t]];
          if (w.empty()) continue;
          double sx[3], sy[3];
          bool behind = false;
          for (int k = 0; k < 3 && !behind; ++k) {
            const auto& p = mesh.positions[t * 3 + size_t(k)];
            const float q[3] = {float(p[0] - ox), p[1], float(p[2] - oz)};
            float o[4];
            for (int r = 0; r < 4; ++r) o[r] = clip[r] * q[0] + clip[4 + r] * q[1] + clip[8 + r] * q[2] + clip[12 + r];
            if (o[3] < 0.1f) behind = true;
            sx[k] = std::clamp(double(o[0] / o[3]), -1.0, 1.0) * 0.5 * width;
            sy[k] = std::clamp(double(o[1] / o[3]), -1.0, 1.0) * 0.5 * height;
          }
          if (behind) continue;
          const double a = 0.5 * std::fabs((sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]));
          if (a <= 0) continue;
          const std::string model = t < mesh.model.size() && mesh.model[t] < host.colorado.models().size()
                                        ? host.colorado.models()[mesh.model[t]].name
                                        : std::string("?");
          auto& e = area[{model, mesh.materials[mesh.material[t]].effect, w}];
          e.px += a;
          ++e.tris;
          for (int k = 0; k < 3; ++k) {
            const auto& p = mesh.positions[t * 3 + size_t(k)];
            for (int c = 0; c < 3; ++c) e.sum[c] += p[size_t(c)] / 3.0;
            e.ymin = std::min(e.ymin, p[1]), e.ymax = std::max(e.ymax, p[1]);
          }
          total += a;
        }
        std::vector<std::pair<double, decltype(area)::const_iterator>> top;
        for (auto it = area.begin(); it != area.end(); ++it) top.push_back({it->second.px, it});
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::printf("texture census: %.0f px of fallback-textured triangles (%.1f%% of the frame), %zu groups\n", total,
                    100.0 * total / (double(width) * height), area.size());
        const auto ds = host.showroom.world_draw_stats();
        std::printf("  renderer fallbacks: %zu draws with a texture that did not upload, %zu with a layer set that did not\n",
                    ds.white, ds.layer_fallback);
        const auto vram = host.renderer.vram_stats();
        const auto wts = host.showroom.world_texture_stats();
        std::printf("  vram: %u blocks, %u allocs (%.1f/%.1f MiB); world textures: resident %zu (%.1f MiB), evictions %llu, reuploads %llu, pools %zu\n",
                    vram.block_count, vram.allocation_count,
                    double(vram.allocation_bytes) / (1024.0 * 1024.0), double(vram.block_bytes) / (1024.0 * 1024.0),
                    wts.resident_count, double(wts.resident_bytes) / (1024.0 * 1024.0),
                    (unsigned long long)wts.evictions, (unsigned long long)wts.reuploads, wts.descriptor_pools);
        for (size_t i = 0; i < top.size() && i < 20; ++i) {
          const auto& [key, v] = *top[i].second;
          std::printf("  %9.0f px %6zu tris  %s  %s  %s  at %.0f %.0f %.0f, y %.1f to %.1f\n", v.px, v.tris,
                      std::get<0>(key).c_str(), std::get<1>(key).c_str(), std::get<2>(key).c_str(),
                      v.sum[0] / double(v.tris), v.sum[1] / double(v.tris), v.sum[2] / double(v.tris), v.ymin, v.ymax);
        }
}

std::string DriveSession::drawn_name(uint32_t t) {
        const auto& mesh = host.streamer->mesh();
        std::string s = t < mesh.model.size() && mesh.model[t] < host.colorado.models().size()
                            ? host.colorado.models()[mesh.model[t]].name
                            : "?";
        if (mesh.has_render_data() && mesh.material[t] < mesh.materials.size())
          s += " " + mesh.materials[mesh.material[t]].effect;
        return s;
}

void DriveSession::log_contacts() {
        if (!contact_log.f || !host.streamer || !host.streamer->uses_collision()) return;
        const auto& cm = host.streamer->collision_mesh();
        if (host.streamer->gathers() != contact_log.gathers) {
          contact_log.gathers = host.streamer->gathers();
          contact_log.drawn.build(host.streamer->mesh());
          contact_log.all.build(host.streamer->mesh(), false);
          {
            // The undecoded fields by surface and slope, over the whole gather.
            std::map<std::tuple<int, int, int, int>, size_t> kinds;  // surface, steep, word, edges class
            for (size_t t = 0; t < cm.triangles(); ++t) {
              const auto& a = cm.positions[t * 3];
              const auto& b = cm.positions[t * 3 + 1];
              const auto& cc = cm.positions[t * 3 + 2];
              const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
              const double vx = cc[0] - a[0], vy = cc[1] - a[1], vz = cc[2] - a[2];
              const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
              const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
              const int steep = len > 0 && std::fabs(ny / len) < 0.5;
              const int word = cm.word[t] == 0x8000 ? -1 : cm.word[t];
              const uint32_t e = cm.edges[t];
              const int edges = e == 0x00FFFFFFu ? 0 : ((e & 0xFF000000u) ? 2 : 1);
              ++kinds[{cm.surface[t], steep, word, edges}];
            }
            for (const auto& [k, n] : kinds)
              if (n >= 20 || std::get<1>(k))
                std::fprintf(contact_log.f, "  kind: surface %d (%s) %s word %s edges %s: %zu\n", std::get<0>(k),
                             host.collision.surface_name(std::get<0>(k)).c_str(), std::get<1>(k) ? "steep" : "flat",
                             std::get<2>(k) < 0 ? "0x8000" : std::to_string(std::get<2>(k)).c_str(),
                             std::get<3>(k) == 0 ? "0x00ffffff" : std::get<3>(k) == 1 ? "other" : "top byte set", n);
          }
          const auto census_t0 = std::chrono::steady_clock::now();
          size_t samples = 0, bare = 0;
          const auto groups = carview::undrawn_walls(cm, host.streamer->ground(), contact_log.drawn, host.streamer->centre_x(),
                                                     host.streamer->centre_z(), 200, 1.0, &samples, &bare);
          std::fprintf(contact_log.f, "gather %d at %.1f %.1f (t %.2f s): %zu drawn triangles indexed; walls within "
                       "200 m at car height: %zu samples (0.5 m), %zu with nothing drawn within 1 m, in %zu groups\n",
                       contact_log.gathers, host.streamer->centre_x(), host.streamer->centre_z(), vehicle.time(),
                       contact_log.drawn.triangles(), samples, bare, groups.size());
          std::printf("contacts log: gather %d census %.2f s\n", contact_log.gathers,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - census_t0).count());
          std::fflush(stdout);
          std::fflush(contact_log.f);
          for (size_t k = 0; k < groups.size() && k < 40; ++k) {
            const auto& g = groups[k];
            std::fprintf(contact_log.f, "  bare: tile %u surface %d (%s) flags 0x%02x word 0x%04x edges 0x%08x: %zu of %zu samples in %zu "
                         "triangles, x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f\n",
                         g.tile, g.surface, host.collision.surface_name(g.surface).c_str(), g.flags, g.word, g.edges, g.bare, g.samples,
                         g.triangles, g.lo[0], g.hi[0], g.lo[1], g.hi[1], g.lo[2], g.hi[2]);
          }
        }
        const auto& c = vehicle.strongest_body_contact();
        const double hard = 0.5 * vehicle.params().mass * 9.81;
        if (c.force < hard) {
          contact_log.touching = false;
          return;
        }
        const drive::Vec3 dp = c.point - contact_log.last;
        if (contact_log.touching && std::sqrt(drive::dot(dp, dp)) < 3) return;
        contact_log.touching = true;
        contact_log.last = c.point;
        ++contact_log.contacts;
        const auto& src = host.streamer->ground_source();
        const int64_t ct = c.triangle >= 0 && size_t(c.triangle) < src.size() ? int64_t(src[size_t(c.triangle)]) : -1;
        const std::array<float, 3> p{float(c.point.x), float(c.point.y), float(c.point.z)};
        uint32_t near_t = 0;
        const double near1 = contact_log.drawn.nearest(p, 1.0);
        const double near5 = contact_log.drawn.nearest(p, 5.0, &near_t);
        if (near1 < 0) ++contact_log.undrawn;
        std::fprintf(contact_log.f, "contact t %.2f s at %.2f %.2f %.2f normal %.2f %.2f %.2f: %.0f N at %.0f km/h; "
                     "surface %d (%s)",
                     vehicle.time(), c.point.x, c.point.y, c.point.z, c.normal.x, c.normal.y, c.normal.z, c.force,
                     vehicle.speed() * 3.6, c.surface, host.collision.surface_name(c.surface).c_str());
        if (ct >= 0)
          std::fprintf(contact_log.f, " flags 0x%02x word 0x%04x edges 0x%08x tile %u triangle %lld", size_t(ct) < cm.flags.size() ? cm.flags[size_t(ct)] : 0, size_t(ct) < cm.word.size() ? cm.word[size_t(ct)] : 0, size_t(ct) < cm.edges.size() ? cm.edges[size_t(ct)] : 0u,
                       size_t(ct) < cm.tile.size() ? cm.tile[size_t(ct)] : 0u, (long long)ct);
        if (near1 >= 0) std::fprintf(contact_log.f, "; drawn %.2f m: %s\n", near1, drawn_name(near_t).c_str());
        else if (near5 >= 0) std::fprintf(contact_log.f, "; NOTHING DRAWN within 1 m (nearest %.2f m: %s)\n", near5, drawn_name(near_t).c_str());
        else std::fprintf(contact_log.f, "; NOTHING DRAWN within 5 m\n");
        if (ct >= 0) {
          const auto& q = cm.positions;
          const size_t b = size_t(ct) * 3;
          std::fprintf(contact_log.f, "    triangle: %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f\n", q[b][0],
                       q[b][1], q[b][2], q[b + 1][0], q[b + 1][1], q[b + 1][2], q[b + 2][0], q[b + 2][1], q[b + 2][2]);
        }
        // A profile across the contact along the wall's horizontal normal:
        // the collision's top surface and the drawn surfaces near the point's height.
        const double hn = std::hypot(c.normal.x, c.normal.z);
        if (hn > 0.3) {
          const double nx = c.normal.x / hn, nz = c.normal.z / hn;
          for (double s = -3; s <= 3.01; s += 0.5) {
            const double x = c.point.x + nx * s, z = c.point.z + nz * s;
            double y = 0;
            const bool ok = world::ground_height(*host.ground, x, z, c.point.y + 4, y);
            std::fprintf(contact_log.f, "    %+.1f m: collision %s", s, ok ? "" : "none");
            if (ok) std::fprintf(contact_log.f, "%.2f", y);
            std::fprintf(contact_log.f, "  drawn");
            int shown = 0;
            for (double h : contact_log.all.heights(x, z))
              if (h < c.point.y + 4 && h > c.point.y - 6 && shown++ < 4) std::fprintf(contact_log.f, " %.2f", h);
            std::fprintf(contact_log.f, "\n");
          }
        }
}

void DriveSession::reload_mod_textures() {
        size_t reuploaded = 0;
        const auto changed = app.mods_session.reload_textures(
            [&](uint32_t record, const cars::bake::Texture& t, bool srgb) {
              std::string why;
              return host.showroom.replace_world_texture(record, t, srgb, why);
            },
            &reuploaded);
        notice = changed.empty() ? "textures: no replacement changed"
                                 : "textures reloaded: " + std::to_string(changed.size()) + " changed, " +
                                       std::to_string(reuploaded) + " re-uploaded";
        notice_until = vehicle.time() + 4;
        mods_page.set_notice(notice);
        std::printf("mods: %s\n", notice.c_str());
}

void DriveSession::save_now(const char* why) {
        if (app.save_path.empty() || app.status != 0) return;
        drive::DriveSave s;
        s.car = !app.pre_loaner_car.empty() ? app.pre_loaner_car : app.car;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
        s.position = {at.x, at.y, at.z};
        s.heading = std::atan2(fwd.x, fwd.z);
        s.has_spawn = host.streamer != nullptr;
        s.spawn = {host.start.x, host.start.y, host.start.z};
        s.spawn_heading = host.start_heading;
        s.credits = app.career.credits, s.total_winnings = app.career.total_winnings;
        s.wristband_points = app.career.wristband_points;
        s.num_races = app.career.num_races, s.num_victories = app.career.num_victories, s.num_podiums = app.career.num_podiums;
        s.time_festival = app.career.time_in_festival, s.time_street = app.career.time_in_street;
        s.time_exhibition = app.career.time_in_exhibition, s.top_speed = app.career.top_speed;
        for (const auto& [id, place] : app.career.race_places) s.race_results.emplace_back(id, place);
        for (const auto& [id, place] : app.career.event_places) s.event_results.emplace_back(id, place);
        s.prize_cars = app.career.prize_cars;
        if (app.progress_loaded) {
          progress_to_save(app.collectibles, app.progress, s);
        } else if (app.have_saved) {  // no collectibles this run: keep what the save had
          s.total_distance = app.saved.total_distance;
          s.signs_found = app.saved.signs_found;
          s.barns = app.saved.barns;
          s.garage = app.saved.garage;
        }
        s.owned_garage = app.have_garage || (app.have_saved && app.saved.owned_garage);
        s.career_started = (app.have_saved && app.saved.career_started) || s.owned_garage || app.career.num_races > 0 || app.career.credits > 0;
        s.intro_completed = app.have_saved ? app.saved.intro_completed : true;
        app.campaign.save_to_save(s);
        s.tod_sec = app.tod_clock.tod_sec;
        s.collected_wristband = app.collected_wristband;
        s.popularity_points = app.skill_tracker ? app.skill_tracker->popularity_points() : (app.have_saved ? app.saved.popularity_points : 0.0);
        s.max_skills_score = app.skill_tracker ? app.skill_tracker->max_skills_score() : (app.have_saved ? app.saved.max_skills_score : 0);
        s.max_skills_car = app.skill_tracker ? app.max_skills_car : (app.have_saved ? app.saved.max_skills_car : 0);
        s.fame_rewards_claimed = app.skill_tracker ? app.skill_tracker->fame_rewards_claimed() : (app.have_saved ? app.saved.fame_rewards_claimed : 0);
        s.difficulty = int(app.race_options.difficulty);
        s.ai_skill = app.dynamic_ai_skill;
        s.ai_mode = int(app.race_options.ai_mode);
        app.audio_host.save_radio_state(s);
        s.treasure_map = app.treasure_map_setting;
        if (app.road_discovery.ready()) {
          s.road_discovery_nodes = app.road_discovery.node_count();
          s.road_discovery_words = app.road_discovery.words();
        } else if (app.have_saved) {
          s.road_discovery_nodes = app.saved.road_discovery_nodes;
          s.road_discovery_words = app.saved.road_discovery_words;
        }
        s.discovered_outposts.assign(app.outpost_progress.discovered_outposts.begin(),
                                     app.outpost_progress.discovered_outposts.end());
        s.completed_pr_stunts.assign(app.outpost_progress.completed_pr_stunts.begin(),
                                     app.outpost_progress.completed_pr_stunts.end());
        if (app.speed_tracker) {
          s.speed_traps = app.speed_tracker->save_traps();
          s.speed_zones = app.speed_tracker->save_zones();
        } else if (app.have_saved) {
          s.speed_traps = app.saved.speed_traps;
          s.speed_zones = app.saved.speed_zones;
        }
        if (app.skill_tracker) {
          for (const auto& [sid, prog] : app.skill_tracker->sponsor_progress())
            s.sponsors.push_back({sid, prog.count, prog.claimed_tiers});
        } else if (app.have_saved) {
          s.sponsors = app.saved.sponsors;
        }
        s.h2h_races = app.h2h_races;
        s.h2h_wins = app.h2h_wins;
        s.driver_xp = app.career.driver_xp;
        for (const auto& [mid, xp] : app.career.make_affinity_xp) s.make_affinity.emplace_back(mid, xp);
        s.promo_cars = promo_tracker.save_ids();
        if (app.have_saved) s.carry_from(app.saved);
        if (app.career.driver_level > 0) s.level = app.career.driver_level;
        s.club = app.saved.club;
        s.has_controls = true;
        s.controls = live_controls;
        std::string e;
        if (!drive::write_save(app.save_path, s, &e)) {
          std::fprintf(stderr, "carview: %s\n", e.c_str());
        } else {
          if (app.headless) std::printf("save (%s): %s at %.1f %.1f %.1f\n", why, app.save_path.string().c_str(), at.x, at.y, at.z);
          app.save_session.on_saved(vehicle.time());
        }
        app.saved = s;
        app.have_saved = true;
}

void DriveSession::eval_achievements(bool toasts) {
        drive::DriveSave snap = app.have_saved ? app.saved : drive::DriveSave{};
        snap.career_started = snap.career_started || app.have_garage || app.career.num_races > 0 || app.career.credits > 0;
        snap.collected_wristband = std::max(snap.collected_wristband, app.collected_wristband);
        snap.num_races = std::max(snap.num_races, app.career.num_races);
        snap.num_victories = std::max(snap.num_victories, app.career.num_victories);
        snap.num_podiums = std::max(snap.num_podiums, app.career.num_podiums);
        snap.total_winnings = std::max(snap.total_winnings, app.career.total_winnings);
        snap.top_speed = std::max({snap.top_speed, app.career.top_speed, vehicle.speed()});
        if (app.progress_loaded) progress_to_save(app.collectibles, app.progress, snap);
        if (app.skill_tracker) {
          snap.popularity_points = std::max(snap.popularity_points, app.skill_tracker->popularity_points());
          snap.max_skills_score = std::max(snap.max_skills_score, app.skill_tracker->max_skills_score());
        }
        snap.discovered_outposts.assign(app.outpost_progress.discovered_outposts.begin(),
                                        app.outpost_progress.discovered_outposts.end());
        snap.completed_pr_stunts.assign(app.outpost_progress.completed_pr_stunts.begin(),
                                        app.outpost_progress.completed_pr_stunts.end());
        snap.promo_cars = promo_tracker.save_ids();
        const int roads_cnt = app.road_discovery.ready() ? app.road_discovery.discovered_count() : 0;
        auto ags = carview::AchievementGameplayState::from_drive_save(snap, roads_cnt, app.h2h_wins);
        if (achievements.evaluate(ags, toasts) > 0 && app.demo_achievements_png.empty())
          achievements.save_state(achievements_path);
}

}  // namespace carview
