#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace carview {

void DriveSession::set_radio_popup(int /*height*/) {
        const auto tel = app.audio_host.telemetry();
        const audio::Radio::Popup p = app.audio_host.has_radio() && !tel.user_music_active ? tel.radio_popup : audio::Radio::Popup{};
        if (p.alpha <= 0 || p.station < 0 || size_t(p.station) >= app.audio_host.radio_data().stations.size()) {
          radio_popup_gpu = {};
          return;
        }
        const auto& st = app.audio_host.radio_data().stations[size_t(p.station)];
        radio_popup_gpu.station_name = st.name;
        radio_popup_gpu.station_label = st.off ? "RADIO OFF" : st.name;
        radio_popup_gpu.off = st.off;
        if (p.track >= 0 && size_t(p.track) < st.tracks.size()) {
          radio_popup_gpu.title = st.tracks[size_t(p.track)].title;
          radio_popup_gpu.artist = st.tracks[size_t(p.track)].artist;
        } else {
          radio_popup_gpu.title.clear();
          radio_popup_gpu.artist.clear();
        }
        radio_popup_gpu.alpha = float(std::clamp(p.alpha, 0.0, 1.0));
}

void DriveSession::radio_press(int dir) {
        if (!app.audio_host.has_radio()) return;
        app.audio_host.radio_press(dir);
}

MapScene DriveSession::map_scene() {
        MapScene sc;
        const drive::Vec3 at = vehicle.position();
        const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
        sc.x = at.x;
        sc.z = at.z;
        sc.heading = std::atan2(f.x, f.z);
        sc.north_up = (live_minimap_mode == 1);
        sc.speed_mps = std::fabs(vehicle.speed());
        sc.data = app.progress_loaded ? &app.collectibles : nullptr;
        sc.progress = app.progress_loaded ? &app.progress : nullptr;
        sc.road_discovery = app.road_discovery.ready() ? &app.road_discovery : nullptr;
        sc.reveal = app.reveal || app.treasure_map_setting || slice12.trainer.effective_reveal_collectibles();
        if (traffic && traffic->cars()) {
          sc.traffic.reserve(traffic->cars()->size());
          for (const auto& tc : *traffic->cars()) sc.traffic.push_back({tc.position.x, tc.position.z});
        }
        if (app.race) {
          app.race->opponent_positions(sc.opponents), app.race->route_near(sc.route);
        } else {
          if (app.gps_nav.active()) {
            app.gps_nav.remaining_polyline(sc.gps_route);
            sc.has_gps_dest = true;
            sc.gps_dest_x = app.gps_nav.route().snapped_dest_x;
            sc.gps_dest_z = app.gps_nav.route().snapped_dest_z;
          }
          if (app.have_career_catalog) {
          // Populate Festival sites, unlocked Street Hubs, and unlocked Career Events!
          for (const auto& site : app.career_catalog.sites) {
            MapMarker mm;
            mm.kind = MapMarker::kFestivalSite;
            mm.icon_tag = site.map_tag;
            mm.id = site.name;
            mm.title = site.name;
            mm.subtitle = site.kind == race::FestivalSite::kRaceCentral &&
                                  app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband) >= 0
                              ? "NEW WRISTBAND READY TO COLLECT!"
                              : "HORIZON FESTIVAL";
            mm.x = site.pos[0], mm.z = site.pos[2];
            mm.is_new = site.kind == race::FestivalSite::kRaceCentral &&
                        app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband) >= 0;
            mm.site_or_hub = int(site.kind);
            sc.markers.push_back(std::move(mm));
          }
          for (const auto& hub : app.career_catalog.hubs) {
            if (!hub.has_trigger || !app.career_catalog.is_hub_unlocked(hub, app.career, app.collected_wristband)) continue;
            MapMarker mm;
            mm.kind = MapMarker::kStreetHub;
            mm.icon_tag = "streetrace";
            mm.id = "HUB_" + std::to_string(hub.id);
            mm.title = "STREET RACE HUB " + std::to_string(hub.id);
            mm.subtitle = "ILLEGAL STREET RACING";
            mm.x = hub.trigger_pos[0], mm.z = hub.trigger_pos[2];
            mm.site_or_hub = hub.id;
            sc.markers.push_back(std::move(mm));
          }
          const int rank = app.current_pop_rank();
          for (const auto& ev : app.career_catalog.events) {
            if (!ev.has_trigger || !app.career_catalog.is_event_unlocked(ev, app.career, app.collected_wristband, rank)) continue;
            MapMarker mm;
            mm.kind = MapMarker::kEvent;
            mm.icon_tag = race::CareerCatalog::event_map_tag(ev);
            mm.id = ev.horizon_id;
            const std::string ename = carview::game_text(ev.name_ref);
            mm.title = (!ename.empty() && ename.rfind("_&", 0) != 0) ? ename : ev.horizon_id;
            const int bp = app.career_catalog.best_place(ev, app.career);
            mm.completed = bp > 0;
            mm.is_new = !mm.completed;
            mm.subtitle = std::string(race::CareerCatalog::event_style_label(ev)) + "  •  " +
                          carview::thousands(int64_t(ev.cash_prize)) + " Cr";
            mm.x = ev.trigger_pos[0], mm.z = ev.trigger_pos[2];
            sc.markers.push_back(std::move(mm));
          }
          }
          if (app.have_outposts) {
            for (const auto& op : app.outposts.outposts) {
              MapMarker mm;
              mm.kind = MapMarker::kOutpost;
              mm.icon_tag = "gas_station";
              mm.id = op.id;
              mm.title = op.display_name;
              mm.discovered = app.outpost_progress.is_outpost_discovered(op.id);
              int done_st = 0;
              for (const auto& mid : op.mission_ids) done_st += app.outpost_progress.is_stunt_completed(mid);
              mm.completed = !op.mission_ids.empty() && done_st == int(op.mission_ids.size());
              mm.is_new = !mm.discovered;
              mm.subtitle = mm.discovered
                                ? ("HORIZON OUTPOST  •  PR STUNTS " + std::to_string(done_st) + "/" +
                                   std::to_string(op.mission_ids.size()) + "  •  FAST TRAVEL")
                                : "HORIZON OUTPOST  •  DRIVE HERE TO UNLOCK FAST TRAVEL";
              mm.x = op.trigger.x;
              mm.z = op.trigger.z;
              mm.site_or_hub = op.index;
              sc.markers.push_back(std::move(mm));
            }
            for (const auto& st : app.outposts.stunts) {
              MapMarker mm;
              mm.kind = MapMarker::kPrStunt;
              mm.icon_tag = st.kind == freeroam::PrStuntKind::kSpeed ? "speed_camera_discovered" : "exhibition";
              mm.id = st.id;
              mm.title = st.title;
              mm.completed = app.outpost_progress.is_stunt_completed(st.id);
              mm.is_new = !mm.completed;
              mm.subtitle = std::string(freeroam::pr_stunt_kind_name(st.kind)) + "  •  " +
                            carview::thousands(st.reward_credits) + " Cr";
              mm.x = st.target_pos.x;
              mm.z = st.target_pos.z;
              sc.markers.push_back(std::move(mm));
            }
          }
          if (app.speed_tracker) {
            for (const auto& t : app.speed_data.traps) {
              const double pb = app.speed_tracker->best_trap_mph(t.id);
              MapMarker mm;
              mm.kind = MapMarker::kSpeedTrap;
              mm.icon_tag = pb > 0.0 ? "speed_camera_completed" : "speed_camera_discovered";
              mm.id = t.id;
              mm.title = "SPEED TRAP " + t.id.substr(t.id.rfind('_') + 1);
              mm.completed = pb > 0.0;
              mm.is_new = !mm.completed;
              char sub[64];
              const double spd_scale = app.metric_units ? 1.609344 : 1.0;
              const char* spd_unit = app.metric_units ? "KM/H" : "MPH";
              if (pb > 0.0) std::snprintf(sub, sizeof sub, "SPEED TRAP  •  PB %.1f %s", pb * spd_scale, spd_unit);
              else std::snprintf(sub, sizeof sub, "SPEED TRAP  •  MIN %.0f %s", t.min_speed_mph * spd_scale, spd_unit);
              mm.subtitle = sub;
              mm.x = t.gate.center[0];
              mm.z = t.gate.center[2];
              sc.markers.push_back(std::move(mm));
            }
            for (const auto& z : app.speed_data.zones) {
              const double pb = app.speed_tracker->best_zone_mph(z.id);
              MapMarker mm;
              mm.kind = MapMarker::kSpeedZone;
              mm.icon_tag = pb > 0.0 ? "average_speed_camera_completed" : "average_speed_camera_discovered";
              mm.id = z.id;
              mm.title = "SPEED ZONE " + z.id.substr(z.id.rfind('_') + 1);
              mm.completed = pb > 0.0;
              mm.is_new = !mm.completed;
              char sub[64];
              const double spd_scale = app.metric_units ? 1.609344 : 1.0;
              const char* spd_unit = app.metric_units ? "KM/H" : "MPH";
              if (pb > 0.0) std::snprintf(sub, sizeof sub, "SPEED ZONE  •  PB %.1f %s AVG", pb * spd_scale, spd_unit);
              else std::snprintf(sub, sizeof sub, "SPEED ZONE  •  AVG SPEED CHALLENGE");
              mm.subtitle = sub;
              mm.x = z.midpoint[0];
              mm.z = z.midpoint[2];
              sc.markers.push_back(std::move(mm));
            }
          }
          if (festival_mgr) {
            for (const auto& fc : festival_mgr->cars()) sc.festival_ai.push_back({fc.position.x, fc.position.z});
            if (festival_mgr->h2h_active()) {
              sc.route = festival_mgr->h2h_route();
              const auto fin = festival_mgr->h2h_finish_pos();
              MapMarker mm;
              mm.kind = MapMarker::kH2hFinish;
              mm.icon_tag = "race_encounter_finish";
              mm.id = "H2H_FINISH";
              mm.title = "HEAD-TO-HEAD FINISH";
              mm.subtitle = "FIRST TO THE FINISH WINS THE WAGER";
              mm.x = fin[0];
              mm.z = fin[2];
              sc.markers.push_back(std::move(mm));
            }
          }
          sc.selected_marker = map_selected_marker;
        }
        return sc;
}

void DriveSession::set_minimap(int height, bool force) {
        if (!app.map_view.ready() || !host.streamer) return;
        if (live_minimap_mode == 2) {
          if (mm_size > 0) mm_size = 0;
          return;
        }
        // The results card: no minimap.
        if (app.race && app.race->player_finished()) {
          if (minimap_at > -2) mm_size = 0, minimap_at = -2;
          return;
        }
        const double t = vehicle.time();
        if (!force && minimap_at >= 0 && t >= minimap_at && t - minimap_at < 1.0 / 60) return;
        minimap_at = t;
        // SPEC-UI-LAYOUTS a: 90,420-280,600 at 1280x720, no frame.
        // When Android touch controls are visible, lift into non-overlapping safe rect.
        const double k = height / 720.0;
        const bool touch_vis = app.touch_controls.visible(vehicle.time());
        carview::TouchRect mm_safe, tach_safe;
        int display_px = std::max(96, int(std::lround(180 * k)));
        if (touch_vis) {
          carview::TouchControls::hud_safe_rects(float(app.width), float(height), mm_safe, tach_safe);
          display_px = std::max(96, int(std::lround(std::min(mm_safe.w() * float(app.width), mm_safe.h() * float(height)))));
        }
        // Cap minimap CPU raster size to 320 px on 4K displays (GPU bilinearly scales
        // cleanly to `display_px`, saving 2.6x CPU & UI atlas space at 60 Hz).
        const int req_px = std::clamp(display_px, 96, 320);
        HudImage img = app.map_view.minimap(map_scene(), req_px);
        if (carview::ui::atlas_update("minimap", img.width, img.height, img.rgba, mm_uv)) {
          mm_size = float(display_px);
          if (touch_vis) {
            mm_x = mm_safe.x0 * float(app.width);
            mm_y = mm_safe.y1 * float(height) - mm_size;
          } else {
            mm_x = float(90 * k), mm_y = float(height) - float(120 * k) - mm_size;
          }
        } else {
          mm_size = 0;
        }
}

}  // namespace carview
