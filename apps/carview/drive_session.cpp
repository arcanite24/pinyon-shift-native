#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sqlite3.h>
#include <string>
#include <vector>

#include "cars/livery.h"
#include "cars/mods.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "fh1data/spa.h"
#include "fh1data/stringtables.h"
#include "garage/garage.h"
#include "profile/drive_save.h"
#include "race/rivals.h"
#include "settings.h"
#include "slice12_runtime.h"

namespace carview {

namespace {

struct CarHistoryTracker {
  const DriveSession* session = nullptr;
  double last_time_s = 0.0;
  double last_total_dist_m = 0.0;
  int last_races = 0;
  int last_victories = 0;
  int last_podiums = 0;
  int64_t last_winnings = 0;
};

CarHistoryTracker& car_history_tracker() {
  static CarHistoryTracker tracker;
  return tracker;
}

std::filesystem::path resolve_ghosts_dir(const DriveApp& app, const std::filesystem::path& replays_dir) {
  if (!app.save_path.empty() && app.save_path.has_parent_path()) {
    return app.save_path.parent_path() / "ghosts";
  }
  if (!replays_dir.empty() && replays_dir.has_parent_path()) {
    return replays_dir.parent_path() / "ghosts";
  }
  return std::filesystem::path("ghosts");
}

int64_t resolve_active_car_id(const DriveApp& app, const cars::bake::CarInfo& info, int64_t* out_garage_id = nullptr) {
  if (out_garage_id) *out_garage_id = 0;
  if (app.have_garage) {
    if (const garage::Car* gc = app.career_garage.selected_car()) {
      auto it = app.baked_ids.find(gc->car_id);
      if (it != app.baked_ids.end() && lower_ascii(it->second) == lower_ascii(app.car)) {
        if (out_garage_id) *out_garage_id = gc->id;
        return gc->car_id;
      }
    }
  }
  if (info.car_id > 0) return info.car_id;
  if (app.have_career_catalog) {
    if (const auto* spec = app.career_catalog.find_car_by_media(app.car)) return spec->car_id;
  }
  return 0;
}

std::string resolve_engine_swap_audio_car(DriveApp& app, int64_t* out_swapped_engine_id = nullptr) {
  if (out_swapped_engine_id) *out_swapped_engine_id = -1;
  if (!app.have_garage || !app.parts_db.valid()) return app.car;
  const garage::Car* gc = app.career_garage.selected_car();
  if (!gc) return app.car;
  auto it = app.baked_ids.find(gc->car_id);
  if (it == app.baked_ids.end() || lower_ascii(it->second) != lower_ascii(app.car)) return app.car;
  const auto db = app.parts_db.get();
  if (!db || !db->is_open()) return app.car;
  cars::CarPhysics rows;
  if (!db->physics(gc->car_id, garage::fitted(*gc), rows, nullptr)) return app.car;
  const bool is_stock_engine = int64_t(std::llround(rows.get("List_UpgradeEngine.IsStock", 1.0))) != 0;
  const int64_t engine_id = int64_t(std::llround(rows.get("List_UpgradeEngine.EngineID", -1.0)));
  if (is_stock_engine || engine_id <= 0) return app.car;
  if (out_swapped_engine_id) *out_swapped_engine_id = engine_id;

  std::string donor_media;
  const std::string eng_str = std::to_string(engine_id);
  const std::string car_str = std::to_string(gc->car_id);
  // First: find a donor car whose stock engine matches the swapped EngineID (maps directly to <MediaName>_ET.xml).
  db->each(
      "SELECT c.MediaName FROM List_UpgradeEngine ue "
      "JOIN Data_Car c ON c.Id = ue.Ordinal "
      "WHERE ue.EngineID = ? AND ue.IsStock = 1 AND c.Id != ? "
      "ORDER BY c.Id ASC LIMIT 1",
      {eng_str, car_str},
      [&](sqlite3_stmt* st) {
        if (donor_media.empty()) {
          if (const unsigned char* p = sqlite3_column_text(st, 0); p && *p) donor_media = reinterpret_cast<const char*>(p);
        }
      },
      nullptr);
  // Fallback: query Data_Engine.MediaName if no separate donor car uses this engine stock.
  if (donor_media.empty()) {
    db->each(
        "SELECT MediaName FROM Data_Engine WHERE EngineID = ? LIMIT 1", {eng_str},
        [&](sqlite3_stmt* st) {
          if (donor_media.empty()) {
            if (const unsigned char* p = sqlite3_column_text(st, 0); p && *p) donor_media = reinterpret_cast<const char*>(p);
          }
        },
        nullptr);
  }
  return donor_media.empty() ? app.car : donor_media;
}

drive::CarParams build_session_vehicle_params(DriveApp& app, DriveWorldHost& host,
                                              const cars::bake::CarInfo& info,
                                              audio::UpgradeLevels& audio_upgrades,
                                              int& audio_trans_level, int& audio_turbo_level,
                                              int& audio_sc_level, bool& audio_sc_is_centrifugal,
                                              int& audio_camshaft_level,
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
                auto fi_tier = [&](const char* tbl) -> int {
                  const std::string lvl_k = std::string(tbl) + ".Level";
                  const std::string max_k = std::string(tbl) + ".MaxScale";
                  const std::string red_k = std::string(tbl) + ".RedlineRPMScale";
                  if (!rows.has(lvl_k) && !rows.has(max_k) && !rows.has(red_k)) return 0;
                  const int raw = int(std::lround(rows.get(lvl_k, 0.0)));
                  // Map DB upgrade level (0=stock, 1=street/L1, 2=sport/L2, 3=race/L3) -> SfxEngine 1..3.
                  return std::clamp(raw <= 0 ? 1 : raw, 1, 3);
                };
                audio_turbo_level = std::max({fi_tier("List_UpgradeEngineTurboSingle"),
                                              fi_tier("List_UpgradeEngineTurboTwin"),
                                              fi_tier("List_UpgradeEngineTurboQuad")});
                const int csc_lvl = fi_tier("List_UpgradeEngineCSC");
                const int dsc_lvl = fi_tier("List_UpgradeEngineDSC");
                audio_sc_level = std::max(csc_lvl, dsc_lvl);
                audio_sc_is_centrifugal = (csc_lvl > 0 && csc_lvl >= dsc_lvl);
                audio_camshaft_level = int(rows.get("List_UpgradeEngineCamshaft.Level", 0));
                std::printf("garage: car %lld (%s) with its parts, %.0f kg (%.1f s)\n", (long long)gc->id,
                            it->second.c_str(), rows.get("List_UpgradeCarBodyWeight.Mass", 0),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                return drive::params_for_fitted_car(info, app.bake_dir, rows, &physics_notes, &gc->tune);
              }
              physics_notes.push_back("garage parts not applied: " + e);
            }
        drive::CarParams params = drive::params_for_baked_car(info, app.bake_dir, &physics_notes);
        if (params.has_turbo && audio_turbo_level == 0) audio_turbo_level = 1;
        if (params.has_supercharger && audio_sc_level == 0) audio_sc_level = 1;
        return params;
}

}  // namespace

DriveSession::DriveSession(DriveApp& a, DriveWorldHost& h)
    : app(a),
      host(h),
      info(h.showroom.car()),
      vehicle(build_session_vehicle_params(a, h, h.showroom.car(), audio_upgrades, audio_trans_level,
                                           audio_turbo_level, audio_sc_level, audio_sc_is_centrifugal,
                                           audio_camshaft_level, physics_notes)),
      race_models{a.race.get()},
      festival_guard{&h.renderer, &h.showroom, &festival_guests, &festival_guest_loads},
      save_due(a.new_career_save_pending),
      live_motion_blur(a.motion_blur_flag >= 0 ? a.motion_blur_flag == 1 : !a.low_preset),
      live_dof(a.dof_flag >= 0 ? a.dof_flag == 1 : !a.low_preset),
      live_photo_through_walls(a.photo_through_walls_cli),
      live_photo_wider_limits(a.photo_wider_limits_cli),
      photos_dir(default_photos_dir()),
      replays_dir(default_replays_dir()),
      prev_rumble_gear(vehicle.gear()) {}

void DriveSession::init_session() {
      // R3-CAM-18: Boot-time save recovery fallback via drive::load_save_with_recovery()
      // if the primary save.txt was corrupt/missing when DriveApp::init() ran.
      if (!app.have_saved && !app.save_path.empty()) {
        const auto backups_dir = app.save_path.parent_path().has_parent_path()
                                     ? (app.save_path.parent_path().parent_path() / "backups" / "saves")
                                     : std::filesystem::path{};
        drive::DriveSave recovered;
        std::filesystem::path recovered_from;
        std::string rec_err;
        if (drive::load_save_with_recovery(app.save_path, backups_dir, recovered, &recovered_from, &rec_err)) {
          app.saved = recovered;
          app.have_saved = true;
          app.career.credits = recovered.credits;
          app.career.total_winnings = recovered.total_winnings;
          app.career.wristband_points = recovered.wristband_points;
          app.career.num_races = recovered.num_races;
          app.career.num_victories = recovered.num_victories;
          app.career.num_podiums = recovered.num_podiums;
          app.career.time_in_festival = recovered.time_festival;
          app.career.time_in_street = recovered.time_street;
          app.career.time_in_exhibition = recovered.time_exhibition;
          app.career.top_speed = recovered.top_speed;
          for (const auto& [id, place] : recovered.race_results) app.career.race_places[id] = place;
          for (const auto& [id, place] : recovered.event_results) app.career.event_places[id] = place;
          app.career.prize_cars = recovered.prize_cars;
          app.h2h_races = recovered.h2h_races;
          app.h2h_wins = recovered.h2h_wins;
          app.treasure_map_setting = recovered.treasure_map;
          app.campaign.load_from_save(
              recovered, app.have_skills_data ? app.skills_data.rank_for_points(recovered.popularity_points) : 0);
          if (recovered.collected_wristband >= 0) app.collected_wristband = recovered.collected_wristband;
          std::printf("save: recovered save from backup %s\n", recovered_from.string().c_str());
        }
      }
      // R3-CAM-23: Reconcile intro_completed & campaign_stage vehicle selection on Continue Career reload.
      if (app.have_saved && !app.race) {
        std::string selected_garage_media;
        if (app.have_garage) {
          if (const garage::Car* gc = app.career_garage.selected_car()) {
            if (auto it = app.baked_ids.find(gc->car_id); it != app.baked_ids.end()) selected_garage_media = it->second;
          }
        }
        const std::string viper_media = find_baked_car(race::CampaignFlow::kViperMedia, race::CampaignFlow::kViperCarId);
        const std::string starter_media = find_baked_car("VW_CorradoVR6_95", race::kStarterCarId);
        const std::string canonical_car = drive::resolve_campaign_reload_car(
            app.saved, selected_garage_media, viper_media.empty() ? race::CampaignFlow::kViperMedia : viper_media,
            starter_media.empty() ? std::string("VW_CorradoVR6_95") : starter_media);
        if (!app.headless && !app.bench && !app.car_given && !app.show_title && !canonical_car.empty() &&
            lower_ascii(canonical_car) != lower_ascii(app.car) &&
            drive::campaign_stage_is_onboarding(app.saved.campaign_stage)) {
          const std::string baked_match = find_baked_car(canonical_car, 0);
          if (!baked_match.empty() && lower_ascii(baked_match) != lower_ascii(app.car)) {
            app.next_car = baked_match;
          }
        }
      }
      events_page.set_ghosts_dir(resolve_ghosts_dir(app, replays_dir));

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
        int64_t swapped_engine_id = -1;
        const std::string audio_car = resolve_engine_swap_audio_car(app, &swapped_engine_id);
        bool loaded_car_audio = app.audio_host.load_car(app.sound_dir, app.game_root_of(), app.state_root, audio_car,
                                                        audio_upgrades, vehicle.params().idle_rpm,
                                                        vehicle.params().redline_rpm, &sound_note);
        if (!loaded_car_audio && lower_ascii(audio_car) != lower_ascii(app.car)) {
          std::fprintf(stderr, "carview: engine swap audio %s unavailable (%s); falling back to %s\n",
                       audio_car.c_str(), sound_note.c_str(), app.car.c_str());
          sound_note.clear();
          loaded_car_audio = app.audio_host.load_car(app.sound_dir, app.game_root_of(), app.state_root, app.car,
                                                     audio_upgrades, vehicle.params().idle_rpm,
                                                     vehicle.params().redline_rpm, &sound_note);
        } else if (loaded_car_audio && lower_ascii(audio_car) != lower_ascii(app.car)) {
          std::printf("engine swap audio: %s -> %s (EngineID %lld)\n", app.car.c_str(), audio_car.c_str(),
                      (long long)swapped_engine_id);
        }
        // On slim/mobile installs where only a subset of cars have baked engine sounds,
        // fall back to any available baked engine bank so the player car never drives silently.
        if (!loaded_car_audio && !app.sound_dir.empty()) {
          std::error_code sec;
          if (std::filesystem::is_directory(app.sound_dir, sec)) {
            for (const auto& entry : std::filesystem::directory_iterator(app.sound_dir, sec)) {
              if (!entry.is_directory(sec)) continue;
              if (!std::filesystem::is_regular_file(entry.path() / "sound.txt", sec)) continue;
              const std::string fb_car = entry.path().filename().string();
              std::string fb_note;
              if (app.audio_host.load_car(app.sound_dir, app.game_root_of(), app.state_root, fb_car, audio_upgrades,
                                          vehicle.params().idle_rpm, vehicle.params().redline_rpm, &fb_note)) {
                loaded_car_audio = true;
                sound_note = fb_note + " (fallback for " + app.car + ")";
                break;
              }
            }
          }
        }
        if (loaded_car_audio)
          std::printf("%s\n", sound_note.c_str());
        else
          std::fprintf(stderr, "carview: no engine sound (%s)\n", sound_note.c_str());
        if (!app.headless || app.bench_audio || app.bench_full_fx) {
          const auto game_root = app.game_root_of();
          const auto sfx_cache = !app.sound_dir.empty() ? app.sound_dir : (app.state_root / "sound-cache");
          if (!game_root.empty()) {
            app.audio_host.init_sfx(game_root, sfx_cache, app.audio_host.car_burble_bank(), app.state_root);
            const auto rally_pkg = !app.rally_catalog.package_dir.empty()
                                       ? app.rally_catalog.package_dir
                                       : (!app.state_root.empty() ? race::find_rally_package(app.state_root)
                                                                  : std::filesystem::path{});
            app.audio_host.init_acoustics(game_root, sfx_cache, app.world_dir, rally_pkg, !app.headless && !app.bench);
            if (!app.colorado_world() && !rally_pkg.empty()) {
              app.audio_host.init_codriver(rally_pkg, sfx_cache, app.language);
            }
          }
        }
        app.bench_startup.audio_load_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - aud_t0).count();
      } else {
        app.audio_host.clear_car();
      }
      if (app.skill_tracker) app.skill_tracker->reset_session_clock();
      traffic.reset();
      if (app.status == 0 && app.race) {
        std::string e;
        if (!host.streamer || !app.race->load_cars(host.renderer, *host.ground, e)) {
          std::fprintf(stderr, "carview: race: %s\n", host.streamer ? e.c_str() : "needs the streamed world");
          app.status = 1;
        }
        if (app.status == 0 && !app.render_ground) app.race->clip_room(host.collision);
        // R3-CAM-07: Wire Rivals / Time Trials ghost playback & `.shgh` recording.
        if (app.status == 0) {
          const auto ghosts_dir = resolve_ghosts_dir(app, replays_dir);
          const bool is_rivals = app.active_rivals.active;
          const bool is_stage = app.race->is_stage();
          if (is_rivals || is_stage) {
            app.race->enable_ghost_recording(true);
            if (is_rivals) {
              app.race->set_rivals_solo(true);
              race::GhostRun ghost;
              bool have_ghost = false;
              if (app.active_rivals.tier_index == 0) {
                have_ghost = race::load_personal_best_ghost(ghosts_dir, app.active_rivals.event_id, ghost, nullptr);
              }
              if (!have_ghost) {
                int shown_pi = 500;
                if (app.have_garage) {
                  if (const garage::Car* sc = app.career_garage.selected_car(); sc && !app.class_bands.empty()) {
                    shown_pi = garage::shown_index(sc->performance_index, app.class_bands);
                  }
                }
                have_ghost = app.race->build_rivals_ghost(app.active_rivals.target_time_s, app.active_rivals.rival_name,
                                                          app.car, info.car_id, shown_pi, ghost);
              }
              if (have_ghost) {
                Showroom* ghost_sr = app.race->find_loaded_model(ghost.car_media);
                if (!ghost_sr) {
                  auto gs = std::make_unique<Showroom>(host.renderer);
                  gs->set_guest();
                  ShowroomOptions opt{};
                  opt.game_root = app.game_root_of();
                  std::string ge;
                  const std::string gmedia = !ghost.car_media.empty() ? ghost.car_media : app.car;
                  if (gs->load(app.bake_dir, gmedia, opt, ge) && gs->upload(ge)) {
                    gs->load_car_lamps(app.game_root_of(), gmedia);
                    ghost_sr = gs.get();
                    festival_guests.emplace("__rivals_ghost__", std::move(gs));
                  }
                }
                app.race->set_ghost(std::move(ghost), ghost_sr ? ghost_sr : &host.showroom, 0.42f);
              }
            } else if (is_stage) {
              const std::string stage_eid =
                  !app.race_event.empty() ? app.race_event : ("stage:" + std::to_string(app.race->stage_track().route_id));
              race::GhostRun pb;
              if (race::load_personal_best_ghost(ghosts_dir, stage_eid, pb, nullptr) && !pb.empty()) {
                app.race->set_ghost(std::move(pb), &host.showroom, 0.40f);
              }
            }
          }
        }
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
        const bool nav_ready =
            app.shared_nav_loaded
                ? true
                : traffic::read_nav(game_root / "media" / "tracks" / "colorado" / "colorado.nav", festival_nav, &ferr);
        if (nav_ready && traffic::read_traffic_config(game_root, fcfg, &ferr, false)) {
          festival_roads.build(app.shared_nav_loaded ? app.shared_nav : festival_nav);
          std::set<int64_t> traffic_ids;
          for (const auto& tm : fcfg.models) traffic_ids.insert(tm.id);
          // Match roaming Festival AI roster dynamically to the player's current car PI/class.
          int player_pi = 500;
          if (app.have_garage) {
            if (const garage::Car* sc = app.career_garage.selected_car()) {
              player_pi = !app.class_bands.empty() ? garage::shown_index(sc->performance_index, app.class_bands)
                                                   : int(sc->performance_index);
            }
          } else if (app.have_career_catalog) {
            if (const auto* cs = app.career_catalog.find_car_by_media(app.car)) {
              player_pi = !app.class_bands.empty() ? garage::shown_index(cs->stock_pi, app.class_bands)
                                                   : int(cs->stock_pi);
            }
          }
          std::vector<freeroam::FestivalRacerSpec> roster;
          if (!*app.car_names_loaded) carview::car_name("");
          const auto ai_db = app.parts_db.get();
          auto resolve_driver_name = [&](const std::string& raw_name, const std::string& first_name) -> std::string {
            std::string resolved = raw_name;
            if (!resolved.empty() && app.car_names) resolved = app.car_names->text(resolved);
            if (resolved.empty() || fh1::StringTables::is_reference(resolved)) {
              if (!raw_name.empty()) resolved = carview::game_text(raw_name);
            }
            if (resolved.empty() || fh1::StringTables::is_reference(resolved)) resolved = first_name;
            return resolved;
          };
          auto fill_car_metrics = [&](freeroam::FestivalRacerSpec& sp, int64_t cid, double curb_weight_raw,
                                      double db_pi_raw, int db_class_raw) {
            if (curb_weight_raw > 0.0) {
              const double kg = curb_weight_raw < 100.0 ? curb_weight_raw * 100.0 : curb_weight_raw;
              sp.mass_kg = std::clamp(kg, 700.0, 3200.0);
            }
            if (app.have_career_catalog) {
              if (const auto* cs = app.career_catalog.find_car(cid)) {
                sp.pi = !app.class_bands.empty() ? garage::shown_index(cs->stock_pi, app.class_bands) : int(cs->stock_pi);
                const int cl = !app.class_bands.empty() ? garage::class_of(cs->stock_pi, app.class_bands) : cs->stock_class;
                if (cl >= 0 && size_t(cl) < app.class_bands.size()) sp.car_class = app.class_bands[size_t(cl)].name;
              }
            } else if (db_pi_raw > 0.0) {
              sp.pi = !app.class_bands.empty() ? garage::shown_index(db_pi_raw, app.class_bands)
                                               : int(std::lround(db_pi_raw <= 1.5 ? db_pi_raw * 1000.0 : db_pi_raw));
              const int cl = !app.class_bands.empty() ? garage::class_of(db_pi_raw, app.class_bands) : db_class_raw;
              if (cl >= 0 && size_t(cl) < app.class_bands.size()) sp.car_class = app.class_bands[size_t(cl)].name;
            }
            if (sp.car_class.empty()) sp.car_class = "B";
          };

          // Primary path: query curated retail FreeRoamDrivers (91 (AIPlayerId, CarId) pairs + AIPlayers names)
          // from gamedb.slt so Showcase/Barn-Find/Unicorn/Boss cars do not spawn as generic roamers.
          bool from_curated_table = false;
          if (ai_db && ai_db->is_open()) {
            ai_db->each(
                "SELECT f.CarId, coalesce(a.Name, ''), coalesce(a.FirstName, ''), "
                "coalesce(c.CurbWeight, 0.0), coalesce(c.PerformanceIndex, 0.0), coalesce(c.ClassID, 0) "
                "FROM FreeRoamDrivers f "
                "LEFT JOIN AIPlayers a ON a.Id = f.AIPlayerId "
                "LEFT JOIN Data_Car c ON c.Id = f.CarId "
                "ORDER BY f.Id",
                {},
                [&](sqlite3_stmt* st) {
                  const int64_t cid = sqlite3_column_int64(st, 0);
                  if (traffic_ids.count(cid)) return;
                  auto it = app.baked_ids.find(cid);
                  if (it == app.baked_ids.end() || lower_ascii(it->second) == lower_ascii(app.car)) return;
                  const unsigned char* n0 = sqlite3_column_text(st, 1);
                  const unsigned char* n1 = sqlite3_column_text(st, 2);
                  const std::string raw_name = n0 ? reinterpret_cast<const char*>(n0) : "";
                  const std::string first_name = n1 ? reinterpret_cast<const char*>(n1) : "";
                  freeroam::FestivalRacerSpec sp;
                  sp.car_id = cid;
                  sp.car_media = it->second;
                  sp.driver_name = resolve_driver_name(raw_name, first_name);
                  fill_car_metrics(sp, cid, sqlite3_column_double(st, 3), sqlite3_column_double(st, 4),
                                   sqlite3_column_int(st, 5));
                  roster.push_back(std::move(sp));
                },
                nullptr);
            from_curated_table = !roster.empty();
          }

          // Fallback path (only when FreeRoamDrivers table is unavailable or has no baked cars):
          // filter baked_ids to street-legal drivable non-Unicorn, non-Barn-Find cars.
          if (roster.empty()) {
            std::set<int64_t> excluded_ids = traffic_ids;
            for (const auto& b : app.collectibles.barns) {
              if (b.car_id > 0) excluded_ids.insert(b.car_id);
            }
            std::map<int64_t, double> car_curb_weight;
            if (ai_db && ai_db->is_open()) {
              ai_db->each(
                  "SELECT Id, coalesce(CurbWeight, 0.0), coalesce(IsUnicorn, 0), "
                  "coalesce(IsSelectable, 1), coalesce(IsDrivable, 1), coalesce(HasRaceLivery, 0) "
                  "FROM Data_Car",
                  {},
                  [&](sqlite3_stmt* st) {
                    const int64_t cid = sqlite3_column_int64(st, 0);
                    car_curb_weight[cid] = sqlite3_column_double(st, 1);
                    const int is_unicorn = sqlite3_column_int(st, 2);
                    const int is_selectable = sqlite3_column_int(st, 3);
                    const int is_drivable = sqlite3_column_int(st, 4);
                    const int has_race_livery = sqlite3_column_int(st, 5);
                    if (is_unicorn != 0 || is_selectable == 0 || is_drivable == 0 || has_race_livery != 0) {
                      excluded_ids.insert(cid);
                    }
                  },
                  nullptr);
            }
            for (const auto& [cid, media] : app.baked_ids) {
              if (excluded_ids.count(cid) || lower_ascii(media) == lower_ascii(app.car)) continue;
              freeroam::FestivalRacerSpec sp;
              sp.car_id = cid;
              sp.car_media = media;
              const double cw = car_curb_weight.count(cid) ? car_curb_weight[cid] : 0.0;
              fill_car_metrics(sp, cid, cw, 0.0, 0);
              roster.push_back(std::move(sp));
            }
          }

          std::stable_sort(roster.begin(), roster.end(),
                           [&](const freeroam::FestivalRacerSpec& a, const freeroam::FestivalRacerSpec& b) {
                             return std::abs(a.pi - player_pi) < std::abs(b.pi - player_pi);
                           });
          if (roster.size() > 24) roster.resize(24);
          if (!roster.empty()) {
            const size_t roster_n = roster.size();
            festival_mgr = std::make_unique<freeroam::FestivalManager>(festival_nav, festival_roads, fcfg, app.traffic_seed + 17);
            std::vector<std::string> ai_names;
            if (race::load_ai_player_names(game_root, ai_names, app.car_names.get(), app.state_root, ai_db.get())) {
              festival_mgr->set_driver_names(std::move(ai_names));
            }
            festival_mgr->set_roster(std::move(roster));
            festival_mgr->set_hour(app.hour);
            festival_mgr->set_wristband_level(app.collected_wristband);
            festival_mgr->set_player_pi(player_pi);
            festival_mgr->set_ground([&](double x, double z, double hint, double& y) {
              if (!host.ground) return false;
              drive::Hit h;
              if (!host.ground->raycast({x, hint + 4, z}, {0, -1, 0}, 12, h)) return false;
              y = hint + 4 - h.distance;
              return true;
            });
            std::printf("festival AI: roaming roster ready (%zu %s models, player PI %d)\n", roster_n,
                        from_curated_table ? "curated FreeRoamDrivers" : "filtered fallback", player_pi);
          }
        }
      }
      if (app.status == 0 && host.streamer) {
        std::string why;
        safe_reset_ok = safe_reset.open(app.game_root_of(), &why);
        if (!safe_reset_ok) std::fprintf(stderr, "carview: note: reset on the nearest road triangle (%s)\n", why.c_str());
      }
      {
        // The reset's fade and collision timings (GameTunableSettings.ini
        // [HandOfGodReset]); a missing key keeps the file's own value.
        std::string why;
        if (!race::load_hand_of_god_reset(app.game_root_of(), reset_timing, &why))
          std::fprintf(stderr, "carview: note: [HandOfGodReset] not read (%s): its values as fallbacks\n", why.c_str());
        for (const auto& k : reset_timing.missing) std::fprintf(stderr, "carview: note: no %s\n", k.c_str());
      }
      if (!app.contacts_log_path.empty()) {
        contact_log.f = std::fopen(app.contacts_log_path.string().c_str(), "w");
        if (!contact_log.f) std::fprintf(stderr, "carview: cannot write %s\n", app.contacts_log_path.string().c_str());
      }
      app.new_career_save_pending = false;
      FmvPlayer::set_subtitle_resolver(carview::game_text);
      loading_screen.load_definitions(app.game_root_of(), app.race ? "festivalevent" : "freeroam", app.state_root);
      settings_page.set_available_luts(scan_lut_files(carview::settings_path().parent_path()));
      slice12.save_tree_label = (app.slice12_modded_save || app.mods_session.isolation().isolated) ? "user-modded" : "user";
      boot_live_settings = {};
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
        const bool already_isolated = app.slice12_modded_save || app.mods_session.isolation().isolated;
        if (app.have_saved && app.saved.fast_travel_anywhere) {
          slice12.trainer.config.fast_travel_anywhere = true;
        }
        const std::filesystem::path clean_dir =
            !app.clean_user_dir.empty()
                ? app.clean_user_dir
                : (!app.save_path.empty() ? app.save_path.parent_path() : std::filesystem::path{});
        slice12.trainer.configure_save_root(clean_dir, already_isolated);
        if (slice12.trainer.consume_modded_isolation_triggered() && !slice12.trainer.active_user_dir().empty()) {
          const auto iso_dir = slice12.trainer.active_user_dir();
          if (!app.save_path.empty()) app.save_path = iso_dir / app.save_path.filename();
          if (!app.garage_path.empty()) app.garage_path = iso_dir / app.garage_path.filename();
          app.slice12_modded_save = true;
          slice12.save_tree_label = "user-modded";
          std::printf("trainer: isolated save tree active (%s)\n", iso_dir.string().c_str());
        }
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
        if (fh1::load_spa_packages(app.game_root_of(), app.state_root, spa_db, app.language, &spa_note)) {
          if (!app.headless) std::printf("achievements: %s\n", spa_note.c_str());
        } else {
          // The title's achievement table is in its content packages' spa.bin
          // (under SHIFT_STATE_ROOT); without one the SPA names are the fallback.
          std::printf("achievements: no spa.bin under the game or state root (%s)\n",
                      app.state_root.empty() ? "SHIFT_STATE_ROOT not set" : app.state_root.string().c_str());
        }
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
        drive::ReplayCarInfo player_ri;
        player_ri.slot = 0u;
        player_ri.car_id = int32_t(resolve_active_car_id(app, info));
        player_ri.car_media = app.car;
        player_ri.driver_name = carview::car_name(app.car);
        player_ri.is_player = true;
        player_ri.is_traffic = false;
        player_ri.paint_index = app.paint_sequence >= 0 ? int32_t(app.paint_sequence) : -1;
        rm.roster.push_back(std::move(player_ri));
        if (app.race) {
          app.race->collect_replay_roster(rm.roster);
          rm.field_size = int(rm.roster.size());
        }
        replay_rec.begin(rm, drive::ReplayRecorder::kDefaultMaxFrames, !app.race);
      }
      // R3-CAM-18: Initialize per-session car_history tracker & register active driven car.
      {
        int64_t gid = 0;
        const int64_t cid = resolve_active_car_id(app, info, &gid);
        if (cid > 0) drive::update_car_history(app.saved.car_history, gid, cid, 0.0, 0.0);
        auto& cht = car_history_tracker();
        cht.session = this;
        cht.last_time_s = vehicle.time();
        cht.last_total_dist_m = app.progress_loaded ? app.progress.total_distance : 0.0;
        cht.last_races = app.career.num_races;
        cht.last_victories = app.career.num_victories;
        cht.last_podiums = app.career.num_podiums;
        cht.last_winnings = app.career.total_winnings;
      }
      cockpit_dial_max_speed = drive::CockpitAnimState::estimate_dial_max_speed_mps(vehicle.params());
      cockpit_dial_max_boost = drive::CockpitAnimState::gauge_max_boost_psi(vehicle.params());
      cockpit_has_fi = vehicle.params().has_turbo || vehicle.params().has_supercharger;
      light_override = 0;  // 0 auto, 1 low beam, 2 full beam, 3 fog
      host.showroom.set_wind(app.tod_system.post_config.wind_speed, app.tod_system.post_config.wind_amplitude);
      if (app.have_saved && app.saved.has_controls) live_controls = app.saved.controls;
      {
        const auto raw_tokens = read_settings(settings_path(), nullptr);
        SettingsPage init_settings;
        init_settings.load(raw_tokens);
        const LiveSettings init_ls = init_settings.live_settings();
        const bool has_tcs_token = std::find(raw_tokens.begin(), raw_tokens.end(), "--assist-tcs") != raw_tokens.end();
        const bool has_dmg_token = std::find(raw_tokens.begin(), raw_tokens.end(), "--damage") != raw_tokens.end();
        const bool is_fresh_profile = !app.have_saved || !app.saved.has_controls;
        if (save_due) {
          // New Campaign (start_new_career): initialize exact retail FH1 Medium defaults
          // (ABS ON, TCS ON, STM OFF, Automatic, Normal steering, Rewind ON, Full line, Cosmetic damage).
          live_controls = init_ls.controls;
          live_controls.assists = drive::AssistSettings::retail_defaults();
          car_damage.mode = drive::DamageMode::kCosmetic;
        } else {
          if (init_settings.has_explicit_control_tokens() || is_fresh_profile) {
            live_controls = init_ls.controls;
            if (!has_tcs_token) {
              live_controls.assists.traction_control =
                  is_fresh_profile ? true : app.saved.controls.assists.traction_control;
            }
          }
          car_damage.mode = (!has_dmg_token && is_fresh_profile) ? drive::DamageMode::kCosmetic
                                                                 : drive::DamageMode(init_ls.damage_mode);
        }
        drive::load_disc_damage_mod_defs(car_damage.mod_defs, nullptr);  // List_DamageModDef (Q102)
        live_hud_speedo_mode = init_ls.hud_speedo_mode;
        live_minimap_mode = init_ls.minimap_mode;
        live_hud = init_ls.hud;
      }
      live_sound_enabled = app.audio_cfg.sound;
      live_radio_enabled = app.audio_cfg.radio_on;
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
        const std::filesystem::path rumble_sr =
            !app.state_root.empty() ? app.state_root : (rumble_state ? std::filesystem::path(rumble_state) : std::filesystem::path{});
        std::string rumble_xml, rumble_err;
        drive::RumbleConfig rumble_cfg;
        if (cars::discover_install(app.game_root_of(), rumble_sr, "", rumble_install, nullptr) &&
            cars::load_rumble_xml(rumble_install, rumble_xml, &rumble_err) &&
            drive::parse_controller_rumble(rumble_xml, rumble_cfg, &rumble_err)) {
          gamepad_rumble.set_config(rumble_cfg);
        }
      }
      // Barn finds once per save load. Saves from the old timer flow marked a
      // barn 2 on discovery and queued it for a 45 s restoration: a queued
      // barn whose car is not in the garage goes back to restoring (1), or
      // ready (0) if its timer had run out, and the queue is dropped. A
      // collected barn (2) has its car in the garage.
      if (app.progress_loaded && !app.collectibles.barns.empty() && !app.barn_restorations_reconciled) {
        app.barn_restorations_reconciled = true;
        auto owns_car = [&](int64_t cid) {
          for (const auto& gc : app.career_garage.cars)
            if (gc.car_id == cid) return true;
          return false;
        };
        for (const auto& br : app.pending_barn_restorations) {
          for (size_t bi = 0; bi < app.collectibles.barns.size() && bi < app.progress.barns.size(); ++bi) {
            const auto& b = app.collectibles.barns[bi];
            if (b.name != br.barn_id && !(b.car_id > 0 && b.car_id == br.car_id)) continue;
            auto& bp = app.progress.barns[bi];
            if (bp.status == freeroam::kBarnCollected && !owns_car(b.car_id)) {
              bp.status = br.ready_to_claim ? freeroam::kBarnReady : freeroam::kBarnRestoring;
              std::printf("barn finds: %s from the old restoration queue -> %s\n", b.name.c_str(),
                          br.ready_to_claim ? "ready" : "restoring");
            }
            break;
          }
        }
        if (!app.pending_barn_restorations.empty() || !app.saved.pending_barn_restorations.empty()) save_due = true;
        app.pending_barn_restorations.clear();
        app.saved.pending_barn_restorations.clear();
        int reconciled_count = 0;
        for (size_t bi = 0; bi < app.collectibles.barns.size() && bi < app.progress.barns.size(); ++bi) {
          if (app.progress.barns[bi].status != freeroam::kBarnCollected) continue;
          const auto& b = app.collectibles.barns[bi];
          if (b.car_id <= 0 || owns_car(b.car_id)) continue;
          const std::string awarded = app.award_garage_car(b.car_id, 0, false, true);
          if (!awarded.empty()) {
            ++reconciled_count;
            std::printf("barn finds: reconciled missing %s (%s, Data_Car %d) into garage\n", b.name.c_str(),
                        awarded.c_str(), b.car_id);
          }
        }
        if (reconciled_count > 0) {
          app.garage_picker();
          save_due = true;
        }
      }
      slice12.apply_live_settings(boot_live_settings, rig, settings_path().parent_path());
      rewind_buf.set_enabled(live_controls.assists.rewind);
      rewind_buf.start_session(vehicle.time());
      apply_vehicle_assists(vehicle, live_controls.assists);
      app.race_options.assists = to_difficulty_assists(live_controls.assists, int(car_damage.mode));
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
      // The race screens name the event by its string-table name (an event
      // started from the command line has not been through the menus).
      race::set_campaign_text_resolver([](std::string_view ref) { return carview::game_text(std::string(ref)); });
      // The pre-race in the original's order (prerace_intro / prerace_menu):
      // the title card, then the grid menu; a restart shows the menu only.
      if (grid_menu_open) {
        race_flow::PreFacts pf;
        if (app.race->rivals_solo()) pf.kind = race_flow::Kind::kRivals;
        else if (const auto* ev = app.career_catalog.find_event(app.race_event))
          pf.kind = race_flow::kind_of_style(ev->style);
        pf.restart = app.race_restart;
        race_screens.start(race_flow::pre_steps(pf));
        grid_menu_selected = 0;
      }
      app.race_restart = false;
      reset_ux = ResetToRoadState{};
      reset_ux.seen_rig_restarts = rig.restart_count();
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
      // P0-G1: Never overwrite disk save files with stale in-memory state when a backup restore,
      // save import, or profile switch has mutated disk saves and awaits reload.
      const bool block_end_save = skip_end_save || save_page.reload_pending();
      if (!block_end_save) save_now("end");
      if (!block_end_save && !app.again && !will_hot_reload && app.rally_world_switch == 0) app.save_session.on_exit(vehicle.time());
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
          app.active_rivals = {};
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
          const std::string fallback_id = app.next_event_fallback;
          app.next_event.clear();
          app.next_event_fallback.clear();
          // R3-CAM-07: Propagate Rivals / Time Trials challenge context when launched from the Rivals tab.
          if (events_page.active_tab() == EventsPage::kRivals) {
            if (const auto* sel = events_page.selected_entry();
                sel && sel->is_rivals && sel->event && sel->event->horizon_id == id) {
              app.active_rivals = DriveApp::RivalsAttempt{};
              app.active_rivals.active = true;
              app.active_rivals.is_rally_stage = false;
              app.active_rivals.event_id = id;
              app.active_rivals.rival_name = sel->rival_name;
              app.active_rivals.target_time_s = sel->target_time_s;
              app.active_rivals.bounty_cr = sel->bounty_cr;
              app.active_rivals.tier_index = sel->rival_tier_index;
              app.active_rivals.completed = false;
            }
          } else if (app.active_rivals.active && app.active_rivals.event_id != id) {
            app.active_rivals = {};
          }
          if (app.race) app.race->release_models();
          app.race.reset();
          std::string opened_id;
          if (app.start_race(id, why)) {
            opened_id = id;
          } else if (!fallback_id.empty() && fallback_id != id && app.start_race(fallback_id, why)) {
            opened_id = fallback_id;
          }
          if (!opened_id.empty()) {
            app.race_event = opened_id;
            std::printf("race: %s via session hot-reload\n", opened_id.c_str());
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
          SDL_SetWindowTitle(app.window, ("carview drive - loading " + (!opened_id.empty() ? opened_id : id)).c_str());
        }
        if (!app.next_car.empty()) {
          if (app.rally_service_resume_after_reload && app.rally_session.active() &&
              app.rally_session.in_service_area()) {
            // P0-K3: Reload upgraded car in-place while keeping app.rally_session alive and reopening Service Area.
            if (app.race) {
              app.race->release_models();
              app.race.reset();
              app.race_event.clear();
            }
            app.rally_service_resume_after_reload = false;
            app.rally_service_open = true;
            app.rally_service_selected = 0;
          } else if (app.race) {
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
        if (app.save_path.empty() || app.status != 0 || save_page.reload_pending()) return;
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
        s.pending_barn_restorations.clear();
        for (const auto& br : app.pending_barn_restorations) {
          if (br.car_id <= 0) continue;
          int bid = 0;
          for (size_t bi = 0; bi < app.collectibles.barns.size(); ++bi) {
            if (app.collectibles.barns[bi].name == br.barn_id || app.collectibles.barns[bi].car_id == br.car_id) {
              bid = int(bi) + 1;
              break;
            }
          }
          if (bid == 0) {
            const size_t pos = br.barn_id.find_last_not_of("0123456789");
            if (pos != std::string::npos && pos + 1 < br.barn_id.size()) {
              bid = std::atoi(br.barn_id.c_str() + pos + 1);
            }
          }
          const float rem = float(std::max(0.0, br.ready_after_s - br.elapsed_s));
          s.pending_barn_restorations.push_back({bid, int(br.car_id), rem});
        }
        s.owned_garage = app.have_garage || (app.have_saved && app.saved.owned_garage);
        s.career_started = (app.have_saved && app.saved.career_started) || s.owned_garage || app.career.num_races > 0 || app.career.credits > 0;
        app.campaign.save_to_save(s);
        // R3-CAM-23: Keep intro_completed strictly aligned with canonical campaign_stage.
        if (drive::campaign_stage_is_onboarding(s.campaign_stage)) {
          s.intro_completed = false;
        } else if (!s.campaign_stage.empty() && s.campaign_stage != "title") {
          s.intro_completed = true;
        } else {
          s.intro_completed = app.have_saved ? app.saved.intro_completed : true;
        }
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
        // R3-CAM-18: Persist codriver_enabled & fast_travel_anywhere symmetrically on save & load.
        s.codriver_enabled = app.have_saved ? app.saved.codriver_enabled : true;
        s.fast_travel_anywhere = slice12.trainer.config.cheats_enabled
                                     ? slice12.trainer.config.fast_travel_anywhere
                                     : (app.have_saved ? app.saved.fast_travel_anywhere : false);
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
        if (app.freeroam_challenges) {
          for (const auto& r : app.freeroam_challenges->save())
            s.freeroam_challenges.push_back({r.id, r.progress, r.completed});
        } else if (app.have_saved) {
          s.freeroam_challenges = app.saved.freeroam_challenges;
        }
        s.h2h_races = app.h2h_races;
        s.h2h_wins = app.h2h_wins;
        s.rivals_beaten = app.have_saved ? app.saved.rivals_beaten : 0;
        s.rally_rivals_beaten = app.have_saved ? app.saved.rally_rivals_beaten : 0;
        s.ach = app.saved.ach;  // achievement signals recorded on app.saved during play
        s.driver_xp = app.career.driver_xp;
        for (const auto& [mid, xp] : app.career.make_affinity_xp) s.make_affinity.emplace_back(mid, xp);
        s.promo_cars = promo_tracker.save_ids();
        // R3-CAM-18: Flush native driving distance/time & race stats into car_history.
        s.car_history = app.saved.car_history;
        {
          int64_t gid = 0;
          const int64_t cid = resolve_active_car_id(app, info, &gid);
          auto& cht = car_history_tracker();
          if (cht.session == this && cid > 0) {
            const double cur_t = vehicle.time();
            const double dt_s = std::max(0.0, cur_t - cht.last_time_s);
            const double cur_dist = app.progress_loaded ? app.progress.total_distance : cht.last_total_dist_m;
            const double d_dist = std::max(0.0, cur_dist - cht.last_total_dist_m);
            const int64_t d_win = std::max<int64_t>(0, app.career.total_winnings - cht.last_winnings);
            const int64_t d_races = std::max<int64_t>(0, app.career.num_races - cht.last_races);
            const int64_t d_vic = std::max<int64_t>(0, app.career.num_victories - cht.last_victories);
            const int64_t d_pod = std::max<int64_t>(0, app.career.num_podiums - cht.last_podiums);
            drive::update_car_history(s.car_history, gid, cid, d_dist, dt_s, d_win, d_races, d_vic, d_pod);
            cht.last_time_s = cur_t;
            cht.last_total_dist_m = cur_dist;
            cht.last_winnings = app.career.total_winnings;
            cht.last_races = app.career.num_races;
            cht.last_victories = app.career.num_victories;
            cht.last_podiums = app.career.num_podiums;
          }
        }
        if (app.have_saved) s.carry_from(app.saved);
        if (app.career.driver_level > 0) s.level = app.career.driver_level;
        s.club = app.saved.club;
        s.club_tutorial = app.saved.club_tutorial;
        s.rally_flow = app.saved.rally_flow;
        s.car_club_name = app.saved.car_club_name;
        s.car_club_tag = app.saved.car_club_tag;
        s.car_club_shared = app.saved.car_club_shared;
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
        if (app.speed_tracker) {
          snap.speed_traps = app.speed_tracker->save_traps();
          snap.speed_zones = app.speed_tracker->save_zones();
        }
        if (app.skill_tracker) {
          snap.sponsors.clear();
          for (const auto& [sid, prog] : app.skill_tracker->sponsor_progress())
            snap.sponsors.push_back({sid, prog.count, prog.claimed_tiers});
          snap.fame_rewards_claimed = app.skill_tracker->fame_rewards_claimed();
        }
        const int roads_cnt = app.road_discovery.ready() ? app.road_discovery.discovered_count() : 0;
        auto ags = carview::AchievementGameplayState::from_drive_save(
            snap, roads_cnt, app.h2h_wins, app.have_career_catalog ? &app.career_catalog : nullptr,
            app.current_pop_rank());
        if (app.road_discovery.ready()) ags.roads_total = app.road_discovery.total_roads();
        // 209 for saves that were already champion before the flag existed.
        if (app.rally_catalog.loaded && app.rally_catalog.champion_unlocked(snap)) ags.rally_champion = true;
        const auto cstats = club_medal_stats();
        ags.club_medals_total = std::max(ags.club_medals_total, cstats.total_medals);
        ags.club_cars_with_medal = std::max(ags.club_cars_with_medal, cstats.cars_with_medal);
        ags.club_ferrari_cars = cstats.ferrari_cars;
        ags.club_million_cr_cars = cstats.million_cr_cars;
        ags.club_shelby_cars = cstats.shelby_cars;
        ags.club_lamborghini_cars = cstats.lamborghini_cars;
        ags.club_nissan_cars = cstats.nissan_cars;
        ags.club_bmw_cars = cstats.bmw_cars;
        ags.club_british_cars = cstats.british_cars;
        ags.club_ford_cars = cstats.ford_cars;
        const int newly_unlocked = achievements.evaluate(ags, toasts);
        if (newly_unlocked > 0 && app.demo_achievements_png.empty())
          achievements.save_state(achievements_path);
}

}  // namespace carview
