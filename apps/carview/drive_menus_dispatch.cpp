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
                                        &hub_phys_settings, &hub_pi_cfg, &app.class_bands, price_mult);
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
          app.campaign.on_wristband_collected(pending, ceremony.bonus_car, newly_unlocked, app.fmv_player, &app.audio_host);
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
          if (elig.uses_loaner && elig.loaner_car_id > 0) {
            if (auto it = app.baked_ids.find(elig.loaner_car_id); it != app.baked_ids.end()) {
              app.pre_loaner_car = app.car;
              app.pre_loaner_garage_id = app.career_garage.selected;
              app.car = it->second;
            }
          }
          events_page.close();
          app.campaign.on_enter_event(ev, app.fmv_player, &app.audio_host);
          if (app.campaign.is_blocking_gameplay() && !app.headless) {
            pending_event_launch = ev.horizon_id;
          } else {
            app.next_event = ev.horizon_id;
            running = false;
          }
          return true;
}

void DriveSession::start_new_career() {
          app.career = race::CareerRecord{};
          app.career.credits = 0;
          app.career.wristband_points = 0;
          app.collected_wristband = 0;
          host.free_roam.reset();
          app.progress = freeroam::Progress{};
          app.progress_loaded = false;
          app.saved = drive::DriveSave{};
          app.saved.career_started = true;
          app.saved.intro_completed = false;
          app.saved.owned_garage = true;
          app.saved.collected_wristband = 0;
          app.saved.difficulty = int(app.race_options.difficulty);
          app.have_saved = true;
          if (app.have_skills_data) {
            app.skill_tracker = std::make_unique<freeroam::SkillTracker>(app.skills_data, 0.0, 0, 0);
            app.skill_tracker->reset_session_clock();
          }
          app.max_skills_car = 0;
          app.career_garage = garage::Garage{};
          app.have_garage = true;
          app.award_garage_car(race::kStarterCarId, race::kStarterCarColour, true);
          app.garage_picker();
          // Spawn inside the Horizon Festival grounds near Race Central & FESTIVAL_SECOND.
          if (const auto* rc = app.have_career_catalog ? app.career_catalog.find_site(race::FestivalSite::kRaceCentral) : nullptr) {
            app.spawn[0] = rc->pos[0] - 40.0;
            app.spawn[1] = rc->pos[1];
            app.spawn[2] = rc->pos[2] - 45.0;
            app.spawn[3] = 3.14;
          } else {
            app.spawn[0] = -890.0, app.spawn[1] = -8.0, app.spawn[2] = -305.0, app.spawn[3] = 3.14;
          }
          app.have_spawn = true;
          app.show_title = false;
          app.title_menu_phase = false;
          app.title_confirm_new = false;
          app.race.reset();
          app.race_event.clear();
          app.pre_loaner_car.clear();
          app.story_intro.reset();
          app.pending_handoff = {};
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
          switch (map_filter) {
            case MapFilter::kAll: return true;
            case MapFilter::kEvents: return m.kind == MapMarker::kEvent || m.kind == MapMarker::kFestivalSite;
            case MapFilter::kStreetRaces: return m.kind == MapMarker::kStreetHub || m.icon_tag == "streetrace";
            case MapFilter::kOutposts: return m.kind == MapMarker::kOutpost || m.kind == MapMarker::kFestivalSite;
            case MapFilter::kCollectibles: return false;
            case MapFilter::kPrStunts: return m.kind == MapMarker::kPrStunt || m.kind == MapMarker::kOutpost;
            default: return true;
          }
}

void DriveSession::update_map_selection() {
          map_selected_marker = -1;
          map_overlay = carview::MapOverlayHud{};
          if (!map_open) return;
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
          if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
            const auto& m = cached_map_markers[size_t(map_selected_marker)];
            map_overlay.title = m.title;
            map_overlay.subtitle = m.subtitle;
            map_overlay.distance_m = std::hypot(m.x - at.x, m.z - at.z);
            if (m.kind == MapMarker::kEvent) map_overlay.action_hint = "ENTER EVENT";
            else if (m.kind == MapMarker::kOutpost)
              map_overlay.action_hint = (m.discovered || slice12.trainer.effective_fast_travel_anywhere())
                                            ? "FAST TRAVEL / HUB"
                                            : "SET GPS ROUTE";
            else if (m.kind == MapMarker::kPrStunt) map_overlay.action_hint = "START PR STUNT";
            else map_overlay.action_hint = "ENTER";
          } else {
            map_overlay.title = "COLORADO MAP";
            map_overlay.subtitle = "PAN TO ANY ROAD, EVENT OR FESTIVAL SITE";
            map_overlay.distance_m = std::hypot(map_cx - at.x, map_cz - at.z);
            map_overlay.action_hint = "FAST TRAVEL HERE";
          }
          map_overlay.gps_active = app.gps_nav.active();
          map_overlay.gps_hint = app.gps_nav.active() ? "SET / CLEAR GPS (G)" : "SET GPS ROUTE (G)";
}

void DriveSession::open_map() {
          if (!app.map_view.ready()) {
            notice = "no map (see the console)";
            notice_until = vehicle.time() + 3;
            return;
          }
          map_open = map_dirty = true;
          picking = false;
          map_zoom = 2;
          map_cx = vehicle.position().x;
          map_cz = vehicle.position().z;
          map_cam.reset(map_cx, map_cz, map_zoom);
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
          if (app.race) app.race->set_difficulty(app.race_options.difficulty);
          sync_pause_labels();
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
            app.leave_race = true;
            running = false;
          }
          if (act == PauseMenu::kQuitToTitle) {
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
          if (app.race || !active_trigger.in_range) return false;
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
