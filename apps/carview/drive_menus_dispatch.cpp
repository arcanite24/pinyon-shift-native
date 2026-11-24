#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "cars/livery.h"
#include "cars/sources.h"
#include "settings.h"

namespace carview {

void DriveSession::sync_pick() {
          const size_t synced = carview::sync_picker_index(app.baked_cars, app.pick_ids, app.career_garage.selected, lower_ascii(app.car));
          pick = synced;
          garage_pick = synced;
          if (app.baked_cars.empty()) picking = false;
}

void DriveSession::browse(int by) {
          if (app.baked_cars.empty()) return;
          if (pick >= app.baked_cars.size()) sync_pick();
          pick = (pick + app.baked_cars.size() + size_t(by + int(app.baked_cars.size()))) % app.baked_cars.size();
          picking = true;
          title_at = 0;  // show it now
}

void DriveSession::take_pick() {
          if (!picking || app.baked_cars.empty()) return;
          if (pick >= app.baked_cars.size()) sync_pick();
          if (pick >= app.baked_cars.size()) return;
          // Restart with that car where this one stands.
          const drive::Vec3 at = vehicle.position();
          const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
          app.spawn[0] = at.x;
          app.spawn[1] = at.y;
          app.spawn[2] = at.z;
          app.spawn[3] = std::atan2(fwd.x, fwd.z);
          app.have_spawn = true;
          app.next_car = app.baked_cars[pick];
          if (pick < app.pick_ids.size()) {
            app.career_garage.selected = app.pick_ids[pick];
            app.write_career_garage();
          }
          running = false;
}

void DriveSession::put_back() {
          world::Spawn where;
          double away = 0;
          if (host.streamer && safe_reset_ok && safe_reset.place(*host.streamer, vehicle, &away))
            std::printf("t %.2f s: put back on the road %.1f m away\n", vehicle.time(), away);
          else if (host.streamer && world::recover(*host.streamer, vehicle, &where))
            std::printf("t %.2f s: put back on the road %.1f m away (nearest road triangle)\n", vehicle.time(),
                        where.distance);
          else
            vehicle.reset(host.start, host.start_heading);
          camera_pull = CameraPull{};
          rig.restart();
          rig_accel.reset();
          rewind_buf.reset_history(vehicle.time());
          world_rewind_snaps.clear();
}

void DriveSession::open_garage_browser() {
          sync_pick();
          garage_sell_confirm_id = -1;
          if (app.have_garage && !app.career_garage.cars.empty()) {
            const cars::PartsDb* gdb =
                (app.parts_db.valid() && app.parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    ? app.parts_db.get().get()
                    : nullptr;
            garage_browser.refresh(app.career_garage, gdb, app.class_bands);
            const auto& vis = garage_browser.visible();
            for (size_t gi = 0; gi < vis.size(); ++gi) {
              if (vis[gi].is_current) {
                const int cur_i = int(garage_browser.selected_index());
                if (int(gi) != cur_i) garage_browser.move(int(gi) - cur_i);
                break;
              }
            }
          }
          garage_open = true;
          app.audio_host.trigger_ui(audio::UiCue::kMenuOpen);
}

void DriveSession::sync_livery_preview(bool recomposite_vinyls) {
          host.showroom.set_paint_spec(livery_editor.document().paint);
          if (recomposite_vinyls) {
            std::vector<uint8_t> rgba;
            if (livery_editor.composite_1024(host.showroom.livery_planes().empty() ? nullptr : &host.showroom.livery_planes(),
                                             rgba, 1024)) {
              std::string lerr;
              host.showroom.set_custom_livery(rgba.data(), 1024, 1024, lerr);
            }
          }
}

void DriveSession::open_paint() {
          paint_before = host.showroom.colour_index();
          const cars::PartsDb* db_ptr =
              (app.parts_db.valid() && app.parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                  ? app.parts_db.get().get()
                  : nullptr;
          int64_t cid = info.car_id;
          std::filesystem::path liv_p;
          if (app.have_garage) {
            if (const garage::Car* sel = app.career_garage.selected_car()) {
              cid = sel->car_id;
              if (!sel->livery_file.empty()) {
                liv_p = sel->livery_file;
                if (liv_p.is_relative() && !app.save_path.empty()) liv_p = app.save_path.parent_path() / liv_p;
              } else if (!app.save_path.empty()) {
                liv_p = app.save_path.parent_path() / "liveries" / ("car_" + std::to_string(sel->id) + ".liv");
              }
            }
          }
          if (liv_p.empty()) {
            const auto base_dir = !app.save_path.empty() ? app.save_path.parent_path() : carview::default_user_dir();
            liv_p = (base_dir.empty() ? std::filesystem::path("liveries") : (base_dir / "liveries")) / (app.car + ".liv");
          }
          livery_edit_path = liv_p;
          std::string msg;
          if (livery_editor.open(db_ptr, app.game_root_of(), cid, liv_p, msg)) {
            livery_editor.set_manufacturer_colors(host.showroom.colours());
            livery_editor.set_wheel_is_chrome(host.showroom.options().visual.wheel_is_chrome);
            livery_editor.set_wheel_is_stock(host.showroom.options().visual.wheel_is_stock);
            paint_open = true;
            notice = msg;
            notice_until = vehicle.time() + 3.0;
            app.audio_host.trigger_ui(audio::UiCue::kMenuOpen);
            return;
          }
          const auto& rows = host.showroom.colours();
          paint_order.resize(rows.size());
          for (size_t i = 0; i < rows.size(); ++i) paint_order[i] = i;
          std::stable_sort(paint_order.begin(), paint_order.end(),
                           [&](size_t a, size_t b) { return rows[a].sequence < rows[b].sequence; });
          paint_pick = 0;
          for (size_t i = 0; i < paint_order.size(); ++i)
            if (paint_order[i] == paint_before) paint_pick = i;
          paint_open = !paint_order.empty();
          if (!paint_open) notice = "this car has no colour rows", notice_until = vehicle.time() + 3;
}

void DriveSession::open_shop_or_autoshow(bool is_autoshow) {
          app.autoshow_buy_confirm = false;
          app.hub_orbit_dragging = false;
          app.hub_orbit_dragged = false;
          // Ensure a career garage exists so Autoshow & Upgrades always work smoothly.
          if (!app.have_garage && app.have_career_catalog) {
            if (const auto* spec = app.career_catalog.find_car_by_media(app.car))
              app.award_garage_car(spec->car_id, 0, true), app.garage_picker();
          }
          std::string why = "parts database unavailable";
          bool ready = false;
          if (app.have_garage && app.parts_db.valid()) {
            why = "loading parts database, try again in a second";
            if (app.parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
              why = "no parts database";
              if (const auto db = app.parts_db.get()) {
                const bool tutorial_first = !app.imported && app.career_garage.cars.size() <= 1 && app.career.wristband_points == 0;
                const double price_mult = slice12.trainer.pacing.effective_price_multiplier();
                ready = is_autoshow
                            ? autoshow.open(*db, app.all_baked, why, app.career.credits, tutorial_first,
                                            slice12.trainer.hidden_cars, price_mult)
                            : shop.open(*db, app.career_garage,
                                        app.career_garage.selected_car() ? app.career_garage.selected_car()->id : 0, why,
                                        &hub_phys_settings, &hub_pi_cfg, &app.class_bands, price_mult, discount());
              }
            }
          }
          if (!ready) notice = why, notice_until = vehicle.time() + 3;
}

bool DriveSession::collect_next_wristband() {
          if (!app.have_career_catalog) return false;
          const int pending = app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband);
          if (pending < 0 || size_t(pending) >= app.career_catalog.wristbands.size()) return false;
          app.collected_wristband = pending;
          const auto& wb = app.career_catalog.wristbands[size_t(pending)];
          int newly_unlocked = 0;
          for (const auto& ev : app.career_catalog.events)
            if (ev.style == 1 && ev.level == pending) ++newly_unlocked;
          std::string cname = wb.colour_key;
          for (char& ch : cname) ch = char(std::toupper(static_cast<unsigned char>(ch)));
          ceremony = carview::CeremonyHud{};
          ceremony.active = true;
          ceremony.headline = "NEW WRISTBAND EARNED!";
          ceremony.band_name = cname + " WRISTBAND";
          ceremony.band_colour = cname;
          ceremony.subtitle = std::to_string(newly_unlocked) + " NEW FESTIVAL EVENTS UNLOCKED!";
          if (wb.reward_car_id > 0) {
            ceremony.bonus_car = app.award_garage_car(wb.reward_car_id, 0, false);
            app.garage_picker();
          }
          const auto wb_action =
              app.campaign.on_wristband_collected(pending, ceremony.bonus_car, newly_unlocked, app.fmv_player, &app.audio_host);
          apply_campaign_action(wb_action);
          ceremony.active = false;  // presented through campaign director stage fly-in ceremony
          save_due = true;
          return true;
}

void DriveSession::open_events_browser(int hub_filter) {
          if (!app.have_career_catalog) {
            notice = "career catalog unavailable";
            notice_until = vehicle.time() + 3;
            return;
          }
          int64_t cid = 0;
          int clid = 2;
          current_car_info(cid, clid);
          events_page.open(app.career_catalog, app.career, app.collected_wristband, app.current_pop_rank(), cid, clid, app.class_bands,
                           hub_filter);
}

bool DriveSession::try_enter_event(const race::CareerEvent& ev) {
          int64_t cid = 0;
          int clid = 2;
          current_car_info(cid, clid);
          const auto elig = app.career_catalog.check_eligibility(ev, cid, clid, app.class_name_list(),
                                                             slice12.trainer.effective_any_car());
          if (!elig.eligible) {
            // Check if another car in the player's garage qualifies.
            std::string hint;
            if (app.have_garage) {
              for (const auto& gc : app.career_garage.cars) {
                const int gcls = app.class_bands.empty() ? gc.class_id : garage::class_of(gc.performance_index, app.class_bands);
                if (app.career_catalog.check_eligibility(ev, gc.car_id, gcls, app.class_name_list()).eligible) {
                  if (auto it = app.baked_ids.find(gc.car_id); it != app.baked_ids.end()) {
                    hint = "  (TRY " + carview::car_name(it->second) + " IN GARAGE)";
                    break;
                  }
                }
              }
            }
            notice = elig.reason + hint;
            notice_until = vehicle.time() + 4.5;
            return false;
          }
          std::string event_loaner_media;
          if (elig.uses_loaner && elig.loaner_car_id > 0) {
            if (auto it = app.baked_ids.find(elig.loaner_car_id); it != app.baked_ids.end()) {
              event_loaner_media = it->second;
            }
          }
          // Preserve the player's manually chosen difficulty tier (never silently revert Hard/Pro/Easy to Med).
          // Keep dynamic_ai_skill aligned inside the selected difficulty tier band.
          {
            const auto want_tier = app.race_options.difficulty == race::Difficulty::kHardPlus
                                       ? race::Difficulty::kPro
                                       : app.race_options.difficulty;
            if (race::difficulty_from_dynamic_skill(app.dynamic_ai_skill) != want_tier) {
              app.dynamic_ai_skill = DriveApp::dynamic_skill_for_difficulty(app.race_options.difficulty);
            }
          }
          events_page.close();
          const auto enter_action = app.campaign.on_enter_event(ev, app.fmv_player, &app.audio_host);
          apply_campaign_action(enter_action);
          // Defer applying the loaner car until DriveApp::start_race() so Showcase intros never
          // mutate app.car out from under the currently loaded free-roam vehicle mesh.
          app.pending_event_loaner_car = event_loaner_media;
          if (app.campaign.is_blocking_gameplay() && !app.headless) {
            pending_event_launch = ev.horizon_id;
          } else if (running && app.next_event.empty()) {
            app.next_event = ev.horizon_id;
            running = false;
          }
          return true;
}

void DriveSession::start_new_career() {
          app.career = race::CareerRecord{};
          app.career.credits = 0;
          app.career.wristband_points = 0;
          // Retail FH1 starts pre-wristband (-1, matching FESTIVAL_SECOND Level = -1);
          // Yellow Wristband (0) and the starter Corrado VR6 are awarded upon Festival registration.
          app.collected_wristband = -1;
          host.free_roam.reset();
          app.progress = freeroam::Progress{};
          app.progress_loaded = false;
          app.outpost_progress = freeroam::OutpostProgress{};
          app.pr_stunt_session.cancel();
          app.pr_stunt_loaner_active = false;
          if (app.speed_tracker) {
            app.speed_tracker->load_records({}, {});
            app.speed_tracker->reset_session_clock();
          }
          app.road_discovery.reset();
          app.h2h_races = 0;
          app.h2h_wins = 0;
          promo_tracker.load({}, promo_tracker.total_cars());
          app.saved = drive::DriveSave{};
          app.saved.career_started = true;
          app.saved.intro_completed = false;
          app.saved.owned_garage = false;
          app.saved.collected_wristband = -1;
          app.saved.difficulty = int(app.race_options.difficulty);
          app.have_saved = true;
          if (app.have_skills_data) {
            app.skill_tracker = std::make_unique<freeroam::SkillTracker>(app.skills_data, 0.0, 0, 0);
            app.skill_tracker->reset_session_clock();
          }
          app.max_skills_car = 0;
          app.career_garage = garage::Garage{};
          app.have_garage = true;
          app.write_career_garage();
          app.have_garage = false;
          app.pick_ids.clear();
          // Spawn directly on the verified Colorado Viper highway asphalt spawn and force
          // synchronous world-streamer regather on hot-reload before frame 1 renders.
          app.spawn[0] = race::CampaignFlow::kViperSpawnPos[0];
          app.spawn[1] = race::CampaignFlow::kViperSpawnPos[1];
          app.spawn[2] = race::CampaignFlow::kViperSpawnPos[2];
          app.spawn[3] = race::CampaignFlow::kViperSpawnHeading;
          app.have_spawn = true;
          app.fast_travel = true;
          app.saved.has_spawn = false;
          app.show_title = false;
          app.title_menu_phase = false;
          app.title_confirm_new = false;
          app.race.reset();
          app.race_event.clear();
          app.pre_loaner_car.clear();
          app.pre_loaner_garage_id = 0;
          app.pending_event_loaner_car.clear();
          app.pre_race_tod_sec = -1.0;
          app.story_intro.reset();
          app.pending_handoff = {};
          app.pending_barn_restorations.clear();
          app.barn_restorations_reconciled = false;
          app.pending_autoshow_open = false;
          if (auto vit = app.baked_ids.find(race::CampaignFlow::kViperCarId); vit != app.baked_ids.end()) {
            app.next_car = vit->second;
          } else if (auto sit = app.baked_ids.find(race::kStarterCarId); sit != app.baked_ids.end()) {
            app.next_car = sit->second;
          } else if (!app.baked_cars.empty()) {
            app.next_car = app.baked_cars[0];
          } else {
            app.next_car = race::CampaignFlow::kViperMedia;
          }
          skip_end_save = true;
          app.new_career_save_pending = true;
          app.campaign_new_career_pending = true;
          running = false;
}

bool DriveSession::map_marker_visible(const MapMarker& m) {
          return map_marker_matches_filter(m, map_filter);
}

void DriveSession::update_map_selection() {
          map_selected_marker = -1;
          map_overlay = carview::MapOverlayHud{};
          if (!map_open) return;
          if (map_w > 64 && map_h > 64) {
            map_cam.set_viewport(map_w, map_h, &app.map_view);
            map_zoom = std::max(map_zoom, app.map_view.min_zoom(map_w, map_h));
            app.map_view.clamp_camera(map_w, map_h, map_zoom, map_cx, map_cz);
          }
          map_overlay.active = true;
          map_overlay.active_filter = int(map_filter);
          map_overlay.filter_tabs.clear();
          for (int fi = 0; fi < int(MapFilter::kCount); ++fi)
            map_overlay.filter_tabs.push_back(map_filter_name(MapFilter(fi)));
          map_overlay.roads_discovered = app.road_discovery.ready() ? app.road_discovery.discovered_count() : 0;
          map_overlay.roads_total = app.road_discovery.ready() ? app.road_discovery.total_roads() : 216;
          map_overlay.zoom = map_zoom;
          map_overlay.treasure_map = app.reveal || app.treasure_map_setting || slice12.trainer.effective_reveal_collectibles();
          slice12.trainer.note_map_road({map_cx, 0.0, map_cz});
          const double snap_r = 280.0 / std::max(0.5, map_zoom);
          double best_d = snap_r;
          for (size_t i = 0; i < cached_map_markers.size(); ++i) {
            if (!map_marker_visible(cached_map_markers[i])) continue;
            const double d = std::hypot(cached_map_markers[i].x - map_cx, cached_map_markers[i].z - map_cz);
            if (d < best_d) best_d = d, map_selected_marker = int(i);
          }
          const drive::Vec3 at = vehicle.position();
          const bool ft_anywhere = slice12.trainer.effective_fast_travel_anywhere();
          double target_x = map_cx, target_z = map_cz;
          if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
            const auto& m = cached_map_markers[size_t(map_selected_marker)];
            target_x = m.x;
            target_z = m.z;
            map_overlay.title = m.title;
            map_overlay.subtitle = m.subtitle;
            map_overlay.distance_m = std::hypot(m.x - at.x, m.z - at.z);
          } else {
            bool matched_collectible = false;
            const bool show_barns = map_filter == MapFilter::kAll || map_filter == MapFilter::kDiscovery;
            const bool show_signs =
                map_filter == MapFilter::kAll || map_filter == MapFilter::kSigns || map_filter == MapFilter::kDiscovery;
            if ((show_barns || show_signs) && app.progress_loaded) {
              double best_cd = snap_r;
              if (show_barns) {
                for (size_t bi = 0; bi < app.collectibles.barns.size(); ++bi) {
                  const auto& b = app.collectibles.barns[bi];
                  const int st = bi < app.progress.barns.size() ? app.progress.barns[bi].status : freeroam::kBarnNotSpawned;
                  const bool visible = (st == freeroam::kBarnFound || st == freeroam::kBarnRumour || map_overlay.treasure_map);
                  if (!visible) continue;
                  const double d = std::hypot(b.at.x - map_cx, b.at.z - map_cz);
                  if (d < best_cd) {
                    best_cd = d;
                    matched_collectible = true;
                    target_x = b.at.x;
                    target_z = b.at.z;
                    map_overlay.title = st == freeroam::kBarnFound ? "BARN FIND (FOUND)" : "BARN FIND";
                    map_overlay.subtitle = b.name;
                    map_overlay.distance_m = std::hypot(b.at.x - at.x, b.at.z - at.z);
                  }
                }
              }
              if (show_signs) {
                for (const auto& sign : app.collectibles.signs) {
                  const bool found = app.progress.signs_found.count(sign.id) > 0;
                  if (!found && !map_overlay.treasure_map) continue;
                  const double d = std::hypot(sign.at.x - map_cx, sign.at.z - map_cz);
                  if (d < best_cd) {
                    best_cd = d;
                    matched_collectible = true;
                    target_x = sign.at.x;
                    target_z = sign.at.z;
                    map_overlay.title = found ? "DISCOUNT SIGN (SMASHED)" : "DISCOUNT SIGN";
                    map_overlay.subtitle = "UPGRADE DISCOUNT COLLECTIBLE";
                    map_overlay.distance_m = std::hypot(sign.at.x - at.x, sign.at.z - at.z);
                  }
                }
              }
            }
            if (!matched_collectible) {
              map_overlay.title = "COLORADO MAP";
              map_overlay.subtitle = "PAN TO ANY ROAD, EVENT OR FESTIVAL SITE";
              map_overlay.distance_m = std::hypot(map_cx - at.x, map_cz - at.z);
            }
          }

          // Retail FH1 1:1 button bar prompts with ZERO duplication between A and X:
          // A / Enter / Left-Click: Set / Toggle GPS route (keeps map open!).
          // X / F: Fast Travel (on eligible Outpost/Festival/trainer target), Enter if in range,
          //        or Clear GPS / Center on Car cleanly.
          map_overlay.gps_active = app.gps_nav.active();
          const bool hovering_gps_dest =
              app.gps_nav.active() &&
              std::hypot(target_x - app.gps_nav.route().dest_x, target_z - app.gps_nav.route().dest_z) < 120.0;
          if (app.race != nullptr) {
            map_overlay.action_hint = "RACE ACTIVE";
          } else {
            map_overlay.action_hint = hovering_gps_dest ? "CLEAR GPS ROUTE" : "SET GPS ROUTE";
          }

          if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
            const auto& m = cached_map_markers[size_t(map_selected_marker)];
            const double player_dist = std::hypot(m.x - at.x, m.z - at.z);
            if (m.kind == MapMarker::kOutpost) {
              double disc_rad = 180.0;
              if (app.have_outposts) {
                if (const auto* op = app.outposts.find_outpost(m.id)) disc_rad = std::max(180.0, op->discovery_radius);
              }
              if (player_dist <= disc_rad) {
                map_overlay.gps_hint = "ENTER OUTPOST";
              } else if (m.discovered || ft_anywhere) {
                const bool ft_free = ft_anywhere ||
                                     (app.have_outposts &&
                                      app.outpost_progress.is_outpost_fast_travel_free(app.outposts, m.id));
                const int64_t ft_fee = ft_free ? 0 : 10000LL;
                map_overlay.gps_hint =
                    ft_fee > 0 ? ("FAST TRAVEL (" + carview::thousands(ft_fee) + " CR)") : "FAST TRAVEL (FREE)";
              } else {
                map_overlay.gps_hint = (app.gps_nav.active() && !hovering_gps_dest) ? "CLEAR GPS ROUTE" : "OUTPOST LOCKED";
              }
            } else if (m.kind == MapMarker::kFestivalSite) {
              map_overlay.gps_hint = player_dist <= 180.0 ? "ENTER SITE" : "FAST TRAVEL (FREE)";
            } else if (m.kind == MapMarker::kEvent && player_dist <= 180.0) {
              map_overlay.gps_hint = "ENTER EVENT";
            } else if (m.kind == MapMarker::kStreetHub && player_dist <= 180.0) {
              map_overlay.gps_hint = "ENTER HUB";
            } else if (m.kind == MapMarker::kPrStunt && player_dist <= 180.0) {
              map_overlay.gps_hint = "START PR STUNT";
            } else if (ft_anywhere) {
              map_overlay.gps_hint = "FAST TRAVEL HERE";
            } else if (app.gps_nav.active() && !hovering_gps_dest) {
              map_overlay.gps_hint = "CLEAR GPS ROUTE";
            } else {
              map_overlay.gps_hint = "CENTER ON CAR";
            }
          } else if (ft_anywhere) {
            map_overlay.gps_hint = "FAST TRAVEL HERE";
          } else if (app.gps_nav.active() && !hovering_gps_dest) {
            map_overlay.gps_hint = "CLEAR GPS ROUTE";
          } else {
            map_overlay.gps_hint = "CENTER ON CAR";
          }
}

void DriveSession::open_map() {
          if (!app.map_view.ready()) {
            notice = "no map (see the console)";
            notice_until = vehicle.time() + 3;
            return;
          }
          map_open = map_dirty = true;
          picking = false;
          int win_w = int(app.width), win_h = int(app.height);
          if (app.window) SDL_GetWindowSizeInPixels(app.window, &win_w, &win_h);
          if (win_w > 64 && win_h > 64) {
            map_w = win_w;
            map_h = win_h;
            map_cam.set_viewport(win_w, win_h, &app.map_view);
          }
          map_zoom = std::max(2.0, map_cam.min_zoom());
          map_cx = vehicle.position().x;
          map_cz = vehicle.position().z;
          map_cam.reset(map_cx, map_cz, map_zoom);
          map_cx = map_cam.cx();
          map_cz = map_cam.cz();
          map_zoom = map_cam.zoom();
          for (int layer : {0, 1, 3}) host.renderer.set_overlay({}, 0, 0, 0, 0, layer);
          mm_size = 0;
          radio_popup_gpu = {};
          cached_map_markers = map_scene().markers;
          update_map_selection();
          save_now("map");
}

void DriveSession::close_map() {
          map_open = false;
          map_overlay.active = false;
          host.renderer.set_overlay({}, 0, 0, 0, 0, 2);
          minimap_at = -1;
}

void DriveSession::sync_pause_labels() {
          const int pending =
              app.have_career_catalog ? app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband) : -1;
          if (pending >= 0 && size_t(pending) < app.career_catalog.wristbands.size()) {
            std::string cn = app.career_catalog.wristbands[size_t(pending)].colour_key;
            for (char& ch : cn) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            menu.events_label = "COLLECT " + cn + " WRISTBAND!";
          } else {
            menu.events_label = "EVENTS";
          }
          char db[64];
          std::snprintf(db, sizeof db, "< DIFFICULTY: %s (+%d%%) >", difficulty_name(app.race_options.difficulty),
                        race::difficulty_bonus_percent(int(app.race_options.difficulty)));
          menu.difficulty_label = db;
          menu.in_rally_world = !app.colorado_world();
}

void DriveSession::cycle_difficulty(int by) {
          const int d = ((int(app.race_options.difficulty) + by) % 5 + 5) % 5;
          app.race_options.difficulty = race::Difficulty(d);
          app.dynamic_ai_skill = DriveApp::dynamic_skill_for_difficulty(app.race_options.difficulty);
          if (app.race) app.race->set_difficulty(app.race_options.difficulty);
          sync_pause_labels();
          const auto sfile = settings_path();
          if (!sfile.empty()) {
            static constexpr const char* kDiffTokens[5] = {"easy", "med", "hard", "pro", "hard+"};
            std::vector<std::string> toks = read_settings(sfile, nullptr);
            toks.push_back("--difficulty");
            toks.push_back(kDiffTokens[d]);
            write_settings(sfile, toks, nullptr);
          }
}

void DriveSession::choose() {
          const PauseMenu::Action act = menu.selected_action();
          menu.open = false;
          if (act == PauseMenu::kMap) open_map();
          if (act == PauseMenu::kEvents) open_events_browser(0);
          if (act == PauseMenu::kGarage) {
            open_garage_browser();
          }
          if (act == PauseMenu::kUpgrades) open_shop_or_autoshow(false);
          if (act == PauseMenu::kAutoshow) open_shop_or_autoshow(true);
          if (act == PauseMenu::kPaint) open_paint();
          if (act == PauseMenu::kRally) {
            if (!app.rally_catalog_tried && !app.state_root.empty()) {
              app.rally_catalog_tried = true;
              const auto db_ptr = app.parts_db.valid() ? app.parts_db.get() : std::shared_ptr<cars::PartsDb>{};
              std::string rerr;
              if (race::load_rally_catalog(app.state_root, db_ptr.get(), nullptr, app.rally_catalog, &rerr)) {
                const auto cd_cache = app.save_path.empty() ? (std::filesystem::path("out") / "audio_cache")
                                                        : (app.save_path.parent_path() / "audio_cache");
                app.audio_host.init_codriver(app.rally_catalog.package_dir, cd_cache, app.language);
              } else {
                std::fprintf(stderr, "rally: catalog load failed: %s\n", rerr.c_str());
              }
            }
            if (!app.rally_catalog.loaded || app.rally_catalog.package_dir.empty()) {
              notice = "Horizon Rally expansion package not installed";
              notice_until = vehicle.time() + 4;
            } else if (app.colorado_world()) {
              const drive::Vec3 vp = vehicle.position();
              const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
              app.colorado_return_pos = {vp.x, vp.y, vp.z};
              app.colorado_return_heading = std::atan2(vf.x, vf.z);
              app.has_colorado_return = true;
              app.rally_world_switch = +1;
              running = false;
            } else {
              app.rally_hub_open = true;
              app.rally_hub_selected = 0;
              app.rally_notice.clear();
            }
          }
          if (act == PauseMenu::kClub) {
            club_check();
            club_open = true;
            club_pick = 0;
          }
          if (act == PauseMenu::kCarClub) {
            if (app.saved.car_club_name.empty()) app.saved.car_club_name = "HORIZON DRIVERS";
            if (app.saved.car_club_tag.empty()) app.saved.car_club_tag = "HRZN";
            car_club_open = true;
            car_club_pick = 0;
            car_club_notice.clear();
          }
          if (act == PauseMenu::kAchievements) {
            eval_achievements(false);
            achievements.open_screen();
          }
          if (act == PauseMenu::kPhotoMode) {
            const drive::Vec3 pat = vehicle.position();
            const drive::Vec3 pf = drive::rotate(vehicle.orientation(), {0, 0, 1});
            photo_mode.enter({pat.x, pat.y, pat.z}, std::atan2(pf.x, pf.z),
                             carview::PhotoLimits::from_settings(live_photo_through_walls, live_photo_wider_limits));
          }
          if (act == PauseMenu::kReplay) open_replay_viewer(true);
          if (act == PauseMenu::kSettings) {
            settings_page.load(read_settings(settings_path(), nullptr));
            LiveSettings snap = settings_page.live_settings();
            snap.difficulty = int(app.race_options.difficulty);
            snap.ai_mode = int(app.race_options.ai_mode);
            snap.rubberband = app.race_options.rubberband;
            snap.damage_mode = int(car_damage.mode);
            snap.sound = live_sound_enabled;
            snap.radio = live_radio_enabled;
            snap.treasure_map = app.treasure_map_setting;
            snap.motion_blur = live_motion_blur;
            snap.dof = live_dof;
            snap.skip_intro_movies = app.skip_intro_movies;
            snap.start_in_single_player = app.start_in_single_player;
            snap.skip_vo_with_a = app.skip_vo_with_a;
            snap.metric_units = app.metric_units;
            snap.hud_speedo_mode = live_hud_speedo_mode;
            snap.minimap_mode = live_minimap_mode;
            if (!app.language.empty()) snap.language = app.language;
            snap.photo_through_walls = live_photo_through_walls;
            snap.photo_wider_limits = live_photo_wider_limits;
            snap.day_length_scale = app.tod_clock.speed_scale;
            snap.controls = live_controls;
            snap.controls.assists.driving_line = static_cast<int>(driving_line_mode);
            snap.wheel_shaping = slice12.wheel_shaping;
            snap.wheel_ffb = slice12.ffb_cfg;
            snap.dualsense = slice12.ds_cfg;
            snap.head_tracking = slice12.ht_cfg;
            snap.camera_editor = slice12.cam_editor_cfg;
            snap.immersive_camera = slice12.imm_cam_cfg;
            snap.telemetry_udp = slice12.tel_cfg;
            snap.trainer_cheats = slice12.trainer.config.cheats_enabled;
            snap.festival_pacing = slice12.trainer.pacing;
            snap.hidden_cars = slice12.trainer.hidden_cars;
            if (!app.preset_name.empty() &&
                (snap.preset.empty() ? (app.preset_name != "high") : (app.preset_name != snap.preset))) {
              snap.preset = app.preset_name;
            }
            if (!app.shadows_flag.empty()) snap.shadows = app.shadows_flag;
            if (app.bloom_flag >= 0) snap.bloom_mode = app.bloom_flag;
            if (!app.graphics_backend.empty()) snap.graphics_backend = app.graphics_backend;
            snap.graphics = host.active_graphics;
            settings_page.sync_from_live(snap);
            settings_page.open();
          }
          if (act == PauseMenu::kCareer) {
            std::vector<cars::PackInfo> packs;
            const carview::CareerSummary cs = app.career_summary(packs);
            career_page.open(cs, std::move(packs));
          }
          if (act == PauseMenu::kReset) put_back();
          if (act == PauseMenu::kRestartRace && app.race) {
            club_pending.event_restarted = true;
            club_step();
            results_page = 0;
            app.next_event = app.race_event;
            running = false;
          }
          if (act == PauseMenu::kQuitRace && app.race) {
            results_page = 0;
            app.restore_pre_race_tod();
            app.leave_race = true;
            running = false;
          }
          if (act == PauseMenu::kQuitToTitle) {
            app.pending_event_loaner_car.clear();
            save_now("title");
            app.show_title = true;
            app.title_menu_phase = true;
            app.title_confirm_new = false;
            app.title_selected = 0;
          }
          if (act == PauseMenu::kQuit) running = false;
}

void DriveSession::race_continue() {
          if (results_table) return;  // the table takes only B
          if (results_page == 0) {
            results_page = 1;
            return;
          }
          if (results_page == 1 && results_points) {
            results_page = 2, points_at = vehicle.time();
            return;
          }
          if ((results_page == 1 || results_page == 2) && !won_car_name.empty()) {
            results_page = 3;
            return;
          }
          results_page = 0;
          if (!app.race->post_race_spawn(app.spawn)) {
            const drive::Vec3 at = vehicle.position();
            const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
            app.spawn[0] = at.x, app.spawn[1] = at.y, app.spawn[2] = at.z, app.spawn[3] = std::atan2(f.x, f.z);
          }
          app.have_spawn = true;
          app.restore_pre_race_tod();
          app.leave_race = true;
          running = false;
}

void DriveSession::race_restart() {
          if (results_table) return;
          results_page = 0;
          app.next_event = app.race_event;
          running = false;
}

void DriveSession::open_menu() {
          sync_pause_labels();
          menu.show(app.race != nullptr);
          picking = false;
          save_now("pause");
}

bool DriveSession::activate_world_trigger() {
          if (app.race) return false;
          // ColoradoDirt free roam: drive into Rally Championship world trigger markers
          // (MS_RALLY_01..07 parsed from rally_event_activations.xml).
          if (!app.colorado_world() && app.rally_catalog.loaded) {
            const drive::Vec3 at = vehicle.position();
            int best_idx = -1;
            double best_dist = 65.0;
            for (size_t i = 0; i < app.rally_catalog.championships.size(); ++i) {
              const auto& ch = app.rally_catalog.championships[i];
              if (std::fabs(ch.trigger_x) < 1e-3 && std::fabs(ch.trigger_z) < 1e-3) continue;
              const double d = std::hypot(ch.trigger_x - at.x, ch.trigger_z - at.z);
              if (d <= best_dist) {
                best_dist = d;
                best_idx = int(i);
              }
            }
            if (best_idx >= 0) {
              // Rally Hub index 0 is RALLY_HEATS; indices 1..7 map directly to catalog.championships.
              app.rally_hub_open = true;
              app.rally_hub_selected = best_idx + 1;
              app.rally_notice = app.rally_catalog.championships[size_t(best_idx)].name;
              app.audio_host.trigger_ui(audio::UiCue::kAccept);
              return true;
            }
          }
          if (!active_trigger.in_range) {
            if (festival_mgr && festival_mgr->hud(vehicle.time()).prompt_visible) {
              const drive::Vec3 fp = vehicle.position();
              const drive::Vec3 ff = drive::rotate(vehicle.orientation(), {0, 0, 1});
              if (festival_mgr->start_head_to_head({fp.x, fp.y, fp.z}, std::atan2(ff.x, ff.z), vehicle.time())) {
                club_pending.encounter_started = true;
                notice = "HEAD-TO-HEAD RACE STARTED!";
                notice_until = vehicle.time() + 2.5;
                minimap_at = -1;
                return true;
              }
            }
            return false;
          }
          if (active_trigger.kind == carview::WorldTrigger::kFestivalSite && active_trigger.site) {
            switch (active_trigger.site->kind) {
              case race::FestivalSite::kRaceCentral:
                if (!collect_next_wristband()) open_events_browser(0);
                return true;
              case race::FestivalSite::kAutoshow: open_shop_or_autoshow(true); return true;
              case race::FestivalSite::kWorkshop: open_shop_or_autoshow(false); return true;
              case race::FestivalSite::kPaintShop: open_paint(); return true;
            }
          } else if (active_trigger.kind == carview::WorldTrigger::kStreetHub && active_trigger.hub) {
            open_events_browser(active_trigger.hub->id);
            return true;
          } else if (active_trigger.kind == carview::WorldTrigger::kEvent && active_trigger.event) {
            try_enter_event(*active_trigger.event);
            return true;
          } else if (active_trigger.kind == carview::WorldTrigger::kOutpost && active_trigger.outpost) {
            open_outpost_hub(active_trigger.outpost->id);
            return true;
          }
          return false;
}

}  // namespace carview
