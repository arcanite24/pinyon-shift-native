#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "campaign_ui.h"
#include "first_time_career_ui.h"
#include "fh1data/xds.h"
#include "hub_scene.h"
#include "pause_screen.h"
#include "perf_overlay.h"
#include "ui4_chrome.h"
#include "ui_widgets.h"

namespace carview {

ShowroomFrame DriveSession::drive_frame(float aspect, double dt) {
        if (have_rig && !app.front) {
          bool interior = false;
          double d = 0;
          ShowroomFrame f = rig_frame(rig, info, vehicle, host.ground, rig_accel, dt, aspect, host.renderer.reverse_z(), &interior,
                                      &d, &last_cam_view, &host.collision, cam_car_coll_long, cam_car_coll_lat,
                                      &cam_prev_wheel_comp, &host.cam_proj);
          have_cam_view = true;
          cockpit_steer_kick_rad = float(last_cam_view.steering_wheel_kick);
          // CarCollision is a pulse (SPEC-CAMERAS, layers): the frame's
          // impact reaches the layers once, then is spent.
          cam_car_coll_long = 0.0;
          cam_car_coll_lat = 0.0;
          if (rig.mode() == drive::CameraMode::kFollowLow || rig.mode() == drive::CameraMode::kFollowHigh) {
            rig_min_distance = std::min(rig_min_distance, d);
            rig_max_distance = std::max(rig_max_distance, d);
          }
          // BumperHigh hides the car; Cockpit/Hood/Follow draw it.
          host.showroom.set_draw_car(rig.mode() != drive::CameraMode::kBumper);
          return f;
        }
        host.showroom.set_draw_car(true);
        return chase_frame(info, vehicle, aspect, app.front, host.ground, &camera_pull, dt, host.renderer.reverse_z(), &host.cam_proj);
}

std::string DriveSession::race_banner() {
        if (!app.race) return {};
        return app.race->player_finished() && !app.headless ? app.race->banner() + "  A/Enter continue  Y restart" : app.race->banner();
}

int DriveSession::affinity_discount_pct() {
        if (!app.have_prog_tables) return 0;
        const int64_t mid = current_make_id();
        const auto it = app.career.make_affinity_xp.find(mid);
        const int axp = it == app.career.make_affinity_xp.end() ? 0 : it->second;
        return int(std::lround(app.prog_tables.affinity_discount_for_xp(axp) * 100.0));
}

int DriveSession::discount() {
        // Retail FH1 discounts upgrade parts via Discount Signs (0..100%) only,
        // without double-stacking Manufacturer Affinity on upgrade prices.
        return freeroam::upgrade_discount(host.free_roam ? host.free_roam->signs_found() : int(app.saved.signs_found.size()));
}

void DriveSession::current_car_info(int64_t& out_car_id, int& out_class_id) {
        out_car_id = 0, out_class_id = 2;
        if (app.have_garage) {
          if (const garage::Car* sc = app.career_garage.selected_car()) {
            out_car_id = sc->car_id;
            out_class_id = app.class_bands.empty() ? sc->class_id : garage::class_of(sc->performance_index, app.class_bands);
            return;
          }
        }
        if (app.have_career_catalog) {
          if (const auto* spec = app.career_catalog.find_car_by_media(app.car)) {
            out_car_id = spec->car_id;
            out_class_id = app.class_bands.empty() ? spec->stock_class : garage::class_of(spec->stock_pi, app.class_bands);
          }
        }
}

void DriveSession::rebuild_outpost_hub() {
        outpost_hub_items.clear();
        const freeroam::Outpost* op = app.have_outposts ? app.outposts.find_outpost(outpost_hub_id) : nullptr;
        if (!op) return;
        const drive::Vec3 at = vehicle.position();
        for (const freeroam::PrStunt* st : app.outposts.stunts_for_outpost(op->id)) {
          const bool done = app.outpost_progress.is_stunt_completed(st->id);
          OutpostHubItem item;
          item.kind = OutpostHubItem::kPrStunt;
          item.ref_id = st->id;
          item.label = std::string(freeroam::pr_stunt_kind_name(st->kind)) + ": " + st->title + (done ? "  [DONE]" : "");
          item.detail_title = st->title;
          item.detail_lines.push_back(std::string("TYPE: ") + freeroam::pr_stunt_kind_name(st->kind));
          item.detail_lines.push_back(st->instruction);
          if (st->kind == freeroam::PrStuntKind::kSpeed) {
            item.detail_lines.push_back("TARGET SPEED: " + carview::format_speed_unit(st->target_mph, app.metric_units, false));
          } else if (st->kind == freeroam::PrStuntKind::kSkill) {
            item.detail_lines.push_back("TARGET SCORE: " + carview::thousands(st->target_points) + " SKILL PTS");
          } else {
            item.detail_lines.push_back("OBJECTIVE: REACH LANDMARK & SNAP PHOTO [P]");
          }
          if (st->time_limit_s > 0.0) {
            item.detail_lines.push_back("TIME LIMIT: " + std::to_string(int(std::lround(st->time_limit_s))) + " SEC");
          }
          // A PR stunt pays no credits or popularity: it lowers the outpost's fast-travel price.
          item.detail_lines.push_back("REWARD: FAST TRAVEL DISCOUNT");
          item.detail_lines.push_back(done ? "STATUS: COMPLETED (REPLAYABLE)" : "STATUS: NEW PR STUNT");
          outpost_hub_items.push_back(std::move(item));
        }
        {
          OutpostHubItem item;
          item.kind = OutpostHubItem::kGarage;
          item.label = "CHANGE CAR / GARAGE";
          item.detail_title = "HORIZON GARAGE";
          item.detail_lines = {"SWAP TO ANY OWNED CAR AT THIS OUTPOST",
                               "CARS AVAILABLE: " + std::to_string(app.baked_cars.size())};
          outpost_hub_items.push_back(std::move(item));
        }
        {
          OutpostHubItem item;
          item.kind = OutpostHubItem::kNearbyEvents;
          item.label = "NEARBY FESTIVAL EVENTS";
          item.detail_title = "EVENTS & RACE CENTRAL";
          item.detail_lines = {"BROWSE UNLOCKED COLORADO EVENTS AND", "STREET RACES FROM THIS REGIONAL OUTPOST"};
          outpost_hub_items.push_back(std::move(item));
        }
        {
          OutpostHubItem item;
          item.kind = OutpostHubItem::kFastTravelFestival;
          item.label = "FAST TRAVEL: HORIZON FESTIVAL";
          item.dest_x = world::kSpawnX;
          item.dest_z = world::kSpawnZ;
          const double dist_m = std::hypot(item.dest_x - at.x, item.dest_z - at.z);
          const std::string dist_line =
              "DISTANCE: " + carview::format_distance_unit(dist_m, app.metric_units) + "   •   FREE FAST TRAVEL";
          item.detail_title = "HORIZON FESTIVAL";
          item.detail_lines = {"RETURN TO RACE CENTRAL, AUTOSHOW,", "DAK'S GARAGE AND THE PAINT SHOP", dist_line};
          outpost_hub_items.push_back(std::move(item));
        }
        for (const auto& other : app.outposts.outposts) {
          if (other.id == op->id) continue;
          if (!app.outpost_progress.is_outpost_discovered(other.id)) continue;
          OutpostHubItem item;
          item.kind = OutpostHubItem::kFastTravelOutpost;
          item.ref_id = other.id;
          item.label = "FAST TRAVEL: " + other.display_name;
          item.dest_x = other.spawn.x;
          item.dest_z = other.spawn.z;
          const double dist_m = std::hypot(item.dest_x - at.x, item.dest_z - at.z);
          const int64_t ft_cost = outpost_travel_fee(other.id);  // the map's fee
          const std::string dist_line =
              "DISTANCE: " + carview::format_distance_unit(dist_m, app.metric_units) + "   •   " +
              (ft_cost <= 0 ? std::string("FREE FAST TRAVEL") : carview::thousands(ft_cost) + " CR");
          item.detail_title = other.display_name;
          item.detail_lines = {"FAST TRAVEL DIRECTLY TO THIS REGIONAL", "HORIZON OUTPOST", dist_line};
          outpost_hub_items.push_back(std::move(item));
        }
        if (outpost_hub_pick >= outpost_hub_items.size()) outpost_hub_pick = 0;
}

void DriveSession::open_outpost_hub(const std::string& id) {
        if (!app.have_outposts) return;
        const freeroam::Outpost* op = app.outposts.find_outpost(id);
        if (!op) return;
        app.outpost_progress.discover_outpost(op->id);
        outpost_hub_id = op->id;
        outpost_hub_pick = 0;
        rebuild_outpost_hub();
        outpost_hub_open = true;
}

void DriveSession::set_hud(int win_w, int win_h, const std::string& banner, const PauseMenu* menu) {
        const carview::modern::HudLayout hl = carview::modern::hud_layout(
            float(win_w), float(win_h), host.active_graphics.hud_lock_16_9, host.active_graphics.hud_scale,
            host.active_graphics.safe_zone);
        host.renderer.set_hud_layout(hl);
        const int width = std::max(1, int(std::lround(hl.virtual_w)));
        const int height = std::max(1, int(std::lround(hl.virtual_h)));
        HudState st;
        st.banner = banner;
        std::string detail_title;
        std::vector<std::string> detail_lines;
        if (menu) menu->fill(st);
        if (events_page.is_open()) events_page.fill(st.menu_title, st.menu, st.menu_selected, detail_title, detail_lines);
        if (outpost_hub_open) {
          const freeroam::Outpost* op = app.have_outposts ? app.outposts.find_outpost(outpost_hub_id) : nullptr;
          st.menu_title = (op ? op->display_name : std::string("HORIZON OUTPOST")) + ": ENTER SELECT   ESC BACK";
          st.menu.clear();
          for (const auto& item : outpost_hub_items) st.menu.push_back(item.label);
          st.menu_selected = outpost_hub_items.empty() ? 0 : int(std::min(outpost_hub_pick, outpost_hub_items.size() - 1));
          if (!outpost_hub_items.empty()) {
            detail_title = outpost_hub_items[size_t(st.menu_selected)].detail_title;
            detail_lines = outpost_hub_items[size_t(st.menu_selected)].detail_lines;
          }
        }
        if (garage_open) {
          st.menu_title = "GARAGE (" + std::to_string(app.baked_cars.size()) + " CARS): ENTER DRIVE   ESC BACK";
          st.menu.clear();
          const int n = int(app.baked_cars.size());
          const size_t gp = n > 0 ? std::min(garage_pick, size_t(n - 1)) : 0;
          const int first = std::clamp(int(gp) - 3, 0, std::max(0, n - 7));
          for (int i = first; i < std::min(n, first + 7); ++i) {
            std::string label = carview::car_name(app.baked_cars[size_t(i)]);
            if (app.have_garage && size_t(i) < app.pick_ids.size()) {
              if (const garage::Car* gc = app.career_garage.find(app.pick_ids[size_t(i)]); gc && !app.class_bands.empty()) {
                const std::string cls = app.class_bands[size_t(garage::class_of(gc->performance_index, app.class_bands))].name;
                label = "[" + cls + " " + std::to_string(garage::shown_index(gc->performance_index, app.class_bands)) + "]  " +
                        label + (gc->id == app.career_garage.selected ? "  (ACTIVE)" : "");
              }
            }
            st.menu.push_back(label);
          }
          st.menu_selected = n > 0 ? int(gp) - first : 0;
        }
        if (shop.is_open()) shop.fill(app.class_bands, discount(), app.career.credits, st.menu_title, st.menu, st.menu_selected);
        if (autoshow.is_open()) autoshow.fill(app.class_bands, app.career.credits, st.menu_title, st.menu, st.menu_selected);
        if (career_page.is_open()) career_page.fill(st.menu_title, st.menu, st.menu_selected);
        if (settings_page.is_open()) settings_page.fill(st.menu_title, st.menu, st.menu_selected);
        if (paint_open) {
          st.menu_title = "PAINT SHOP (FREE): LEFT/RIGHT PREVIEW   ENTER APPLY";
          st.menu.clear();
          const auto& rows = host.showroom.colours();
          const size_t first = paint_pick >= 4 ? paint_pick - 4 : 0;
          for (size_t i = first; i < paint_order.size() && i < first + 9; ++i) {
            const auto& c = rows[paint_order[i]];
            char line[96];
            std::snprintf(line, sizeof line, "COLOUR %lld  #%06X%s%s%s", static_cast<long long>(c.sequence), c.rgb,
                          c.metallic ? "  METALLIC" : "", c.has_stripe ? "  STRIPE" : "",
                          paint_order[i] == paint_before ? "  (CURRENT)" : "");
            st.menu.push_back(line);
          }
          st.menu_selected = int(paint_pick - first);
        }
        st.kmh = vehicle.speed() * 3.6;
        st.gear = vehicle.gear();
        st.rpm = vehicle.rpm();
        st.redline_rpm = vehicle.params().redline_rpm;
        st.abs = vehicle.abs_active();
        carview::DriveHud d;
        d.time = app.show_title ? title_orbit_time : vehicle.time();
        const double dial_dt = d.time - dial_trail_last_t;
        if (dial_trail_last_t >= 0.0 && dial_dt > 1e-4 && dial_dt < 0.5) {
          dial_trail_rate += (((st.rpm - dial_trail_last_rpm) / dial_dt) - dial_trail_rate) * std::min(1.0, dial_dt * 10.0);
        } else if (dial_dt < 0.0 || dial_dt >= 0.5) {
          dial_trail_rate = 0.0;
        }
        dial_trail_last_rpm = st.rpm;
        dial_trail_last_t = d.time;
        d.kmh = st.kmh, d.rpm = st.rpm, d.rpm_rate = dial_trail_rate, d.redline_rpm = st.redline_rpm, d.gear = st.gear, d.abs = st.abs,
        d.tcs = vehicle.tcs_active(), d.stm = vehicle.stm_active();
        const auto va = vehicle.assists();
        d.abs_enabled = (va.braking != 2);
        d.tcs_enabled = va.traction_control;
        d.stm_enabled = va.stability_control;
        static carview::AssistFlashLatch s_assist_latch;
        if (app.show_title) s_assist_latch.reset();
        s_assist_latch.update(d.time, d.abs, d.tcs, d.stm);
        d.abs_flash_timer = s_assist_latch.abs_timer;
        d.tcs_flash_timer = s_assist_latch.tcs_timer;
        d.stm_flash_timer = s_assist_latch.stm_timer;
        {
          const int kf_count = rewind_buf.keyframe_count();
          const double oldest_t = kf_count > 0 ? rewind_buf.keyframe(0).replay_time : 0.0;
          const double latest_t = kf_count > 0 ? std::max(vehicle.time(), rewind_buf.keyframe(kf_count - 1).replay_time) : 0.0;
          const double pb_t = rewind_buf.active() ? rewind_buf.playback_time() : latest_t;
          const auto rw_sc = carview::compute_rewind_scrubber(
              kf_count, drive::RewindConfig::kMaxKeyframes, rewind_buf.selected_slot(), oldest_t, latest_t, pb_t, d.time);
          d.rewind_buffer_fraction = rw_sc.buffer_fraction;
          d.rewind_slot = rw_sc.current_slot;
          d.rewind_total_slots = rw_sc.total_slots;
          d.rewind_seconds_remaining = rw_sc.seconds_remaining;
        }
        d.has_boost = vehicle.params().has_turbo || vehicle.params().has_supercharger;
        d.boost_psi = vehicle.boost_psi();
        d.boost_max_psi = vehicle.params().boost_max_psi;
        d.speedo_mode = live_hud_speedo_mode;
        d.hud = live_hud;
        d.club_tag = app.saved.car_club_tag;
        // Barn Find proximity radar: signal strength [0..1] when driving inside an active rumour circle.
        if (!app.race && app.progress_loaded && !app.collectibles.barns.empty()) {
          const drive::Vec3 vp = vehicle.position();
          double best_prox = 0.0;
          for (size_t bi = 0; bi < app.collectibles.barns.size(); ++bi) {
            if (bi < app.progress.barns.size() && app.progress.barns[bi].status == freeroam::kBarnRumour) {
              const auto& b = app.collectibles.barns[bi];
              const double rx = std::max(80.0, b.hint_rx);
              const double rz = std::max(80.0, b.hint_rz);
              const double nx = (vp.x - b.hint_x) / rx;
              const double nz = (vp.z - b.hint_z) / rz;
              if (nx * nx + nz * nz <= 1.25) {
                const double dist = std::hypot(vp.x - b.at.x, vp.z - b.at.z);
                const double span = std::max(rx, rz);
                const double prox = std::clamp(1.0 - dist / std::max(60.0, span), 0.08, 1.0);
                best_prox = std::max(best_prox, prox);
              }
            }
          }
          d.barn_radar_proximity = best_prox;
        }
        d.detail_title = std::move(detail_title);
        d.detail_lines = std::move(detail_lines);
        // Title Screen HUD (when active and settings isn't open over it).
        carview::TitleScreenHud ts_hud;
        if (app.show_title && !settings_page.is_open()) {
          ts_hud.active = true;
          ts_hud.menu = app.title;
          d.title_screen = &ts_hud;
        }
        if (ceremony.active) d.ceremony = &ceremony;
        if (map_overlay.active) {
          map_overlay.center_x = map_cx;
          map_overlay.center_z = map_cz;
          map_overlay.scale_mpp = (map_w > 64 && map_h > 64) ? app.map_view.map_scale(map_w, map_h, map_zoom) : 0.0;
          map_overlay.marker_icon_tag.clear();
          map_overlay.detail_line.clear();
          if (map_selected_marker >= 0 && size_t(map_selected_marker) < cached_map_markers.size()) {
            const auto& mk = cached_map_markers[size_t(map_selected_marker)];
            map_overlay.marker_icon_tag = (mk.kind == MapMarker::kPrStunt) ? std::string("pr_stunt") : mk.icon_tag;
            map_overlay.detail_line = mk.detail_line;
          } else if (map_overlay.title.rfind("BARN FIND", 0) == 0) {
            map_overlay.marker_icon_tag = "barnfind";
            map_overlay.detail_line = "CLASSIC CAR RESTORATION RUMOUR";
          } else if (map_overlay.title.rfind("DISCOUNT SIGN", 0) == 0) {
            map_overlay.marker_icon_tag = "flyer";
            map_overlay.detail_line = "SMASH FOR 1% UPGRADE DISCOUNT AT DAK'S GARAGE";
          } else if (app.progress_loaded && !app.collectibles.barns.empty()) {
            // Check if the crosshair is hovering inside an active Barn Find rumour circle!
            for (size_t bi = 0; bi < app.collectibles.barns.size() && bi < app.progress.barns.size(); ++bi) {
              if (app.progress.barns[bi].status != freeroam::kBarnRumour) continue;
              const auto& b = app.collectibles.barns[bi];
              const double rx = std::max(80.0, b.hint_rx);
              const double rz = std::max(80.0, b.hint_rz);
              const double nx = (map_cx - b.hint_x) / rx;
              const double nz = (map_cz - b.hint_z) / rz;
              if (nx * nx + nz * nz <= 1.0) {
                map_overlay.title = "BARN FIND RUMOUR";
                map_overlay.subtitle = b.name.empty() ? "SEARCH AREA FOR HIDDEN CLASSIC CAR" : b.name;
                map_overlay.detail_line = "DRIVE INSIDE THE PURPLE ZONE TO TRACK THE SIGNAL";
                map_overlay.marker_icon_tag = "barnfind";
                const drive::Vec3 vp = vehicle.position();
                map_overlay.distance_m = std::hypot(b.hint_x - vp.x, b.hint_z - vp.z);
                break;
              }
            }
          }
          map_screen_fill();
          map_overlay.screen = &map_screen_hud;
          d.map_overlay = &map_overlay;
        }
        carview::SkillChainHud sk_hud;
        static carview::PopularityBadgeAnimator s_pop_anim;
        if (app.show_title || !app.skill_tracker) {
          s_pop_anim.reset();
        }
        if (app.skill_tracker) {
          const auto sh = app.skill_tracker->hud();
          sk_hud.active = sh.active;
          sk_hud.chain_points = sh.chain_points;
          sk_hud.multiplier = sh.multiplier;
          sk_hud.multiplier_f = sh.multiplier_f > 0.0f ? sh.multiplier_f : float(std::max(1, sh.multiplier));
          sk_hud.chain_remaining_s = app.skill_tracker->chain_remaining_s(d.time);
          sk_hud.chain_timeout_s = app.skills_data.bank_timeout > 0.1 ? app.skills_data.bank_timeout : 3.0;
          sk_hud.last_skill = sh.last_skill;
          sk_hud.last_points = sh.last_points;
          sk_hud.last_skill_at = sh.last_skill_at;
          sk_hud.banked_total = sh.banked_total;
          sk_hud.banked_at = sh.banked_at;
          sk_hud.lost_at = sh.lost_at;
          sk_hud.popularity_rank = sh.popularity_rank;
          sk_hud.popularity_progress = sh.popularity_progress;
          sk_hud.rank_up_at = sh.rank_up_at;
          s_pop_anim.update(sk_hud, d.time);
          s_pop_anim.apply(sk_hud);
          d.skills = &sk_hud;
        }
        if (world_marker.visible && !app.race) d.world_marker = &world_marker;
  if (event_summary_open() && !app.race) d.event_summary = &event_summary_hud;
        carview::GpsHud gps_hud;
        static double s_gps_uturn_since = -1.0;
        static struct {
          bool active = false;
          double dest_x = 0.0;
          double dest_z = 0.0;
          double total_len = 0.0;
          size_t point_count = 0;
          double recalc_until = -1e9;
        } s_gps_recalc;
        if (!app.race && app.gps_nav.active()) {
          const auto& gg = app.gps_nav.guidance();
          const auto& gr = app.gps_nav.route();
          const bool same_dest = s_gps_recalc.active &&
                                 std::hypot(gr.dest_x - s_gps_recalc.dest_x, gr.dest_z - s_gps_recalc.dest_z) < 5.0;
          const bool route_changed = same_dest && (std::fabs(gr.total_length - s_gps_recalc.total_len) > 2.0 ||
                                                   gr.points.size() != s_gps_recalc.point_count);
          if (route_changed || gg.off_route_m > traffic::GpsNavigator::kOffRouteMeters * 0.78) {
            s_gps_recalc.recalc_until = d.time + 2.2;
          }
          s_gps_recalc.active = true;
          s_gps_recalc.dest_x = gr.dest_x;
          s_gps_recalc.dest_z = gr.dest_z;
          s_gps_recalc.total_len = gr.total_length;
          s_gps_recalc.point_count = gr.points.size();

          const bool raw_uturn = (int(gg.next_turn) == 8) && (gg.dist_to_dest_m >= 50.0) &&
                                 (std::fabs(vehicle.speed()) > 4.5);
          if (raw_uturn) {
            if (s_gps_uturn_since < 0.0) s_gps_uturn_since = vehicle.time();
          } else {
            s_gps_uturn_since = -1.0;
          }
          gps_hud.active = true;
          gps_hud.turn_kind = int(gg.next_turn);
          gps_hud.turn_text = traffic::gps_turn_label(gg.next_turn);
          gps_hud.dest_label = gg.dest_label;
          gps_hud.dist_to_turn_m = gg.dist_to_turn_m;
          gps_hud.dist_to_dest_m = gg.dist_to_dest_m;
          gps_hud.confirmed_uturn = raw_uturn && s_gps_uturn_since >= 0.0 && (vehicle.time() - s_gps_uturn_since >= 2.2);
          gps_hud.recalculating = (d.time < s_gps_recalc.recalc_until);
          gps_hud.time = d.time;
          d.gps = &gps_hud;
        } else {
          s_gps_uturn_since = -1.0;
          s_gps_recalc.active = false;
          s_gps_recalc.recalc_until = -1e9;
        }
        carview::PrStuntHud pr_hud;
        if (!app.race && app.pr_stunt_session.active()) {
          const auto& pst = app.pr_stunt_session.stunt();
          pr_hud.active = true;
          pr_hud.kind_badge = freeroam::pr_stunt_kind_name(pst.kind);
          pr_hud.title = pst.title;
          pr_hud.instruction = pst.instruction;
          pr_hud.status_line = app.pr_stunt_session.status_line();
          pr_hud.time_remaining_s = app.pr_stunt_session.time_remaining_s();
          pr_hud.progress = app.pr_stunt_session.progress_fraction();
          pr_hud.in_zone = app.pr_stunt_session.in_target_zone();
          pr_hud.is_photo = pst.kind == freeroam::PrStuntKind::kPhoto;
          d.pr_stunt = &pr_hud;
        }
        freeroam::SpeedActivityHud speed_hud_state;
        if (!app.race && app.speed_tracker) {
          speed_hud_state = app.speed_tracker->hud(vehicle.time());
          if (speed_hud_state.zone_active && !speed_hud_state.active_zone_id.empty()) {
            if (const auto* zd = app.speed_data.find_zone(speed_hud_state.active_zone_id);
                zd && !zd->display_name.empty()) {
              const std::string loc = carview::game_text(zd->display_name);
              speed_hud_state.active_zone_id =
                  (!loc.empty() && loc.rfind("_&", 0) != 0) ? loc : zd->display_name;
            }
          }
          if (speed_hud_state.banner_active && !speed_hud_state.banner_title.empty()) {
            const std::string loc = carview::game_text(speed_hud_state.banner_title);
            if (!loc.empty() && loc.rfind("_&", 0) != 0) {
              speed_hud_state.banner_title = loc;
            } else if (speed_hud_state.banner_is_zone) {
              if (const auto* zd = app.speed_data.find_zone(speed_hud_state.banner_title);
                  zd && !zd->display_name.empty()) {
                speed_hud_state.banner_title = zd->display_name;
              }
            } else {
              if (const auto* td = app.speed_data.find_trap(speed_hud_state.banner_title);
                  td && !td->display_name.empty()) {
                speed_hud_state.banner_title = td->display_name;
              }
            }
          }
          d.speed_hud = &speed_hud_state;
        }
        freeroam::HeadToHeadHud h2h_hud_state;
        if (!app.race && festival_mgr) {
          h2h_hud_state = festival_mgr->hud(vehicle.time());
          d.h2h = &h2h_hud_state;
        }
        d.driver_level = std::max(1, app.career.driver_level);
        d.driver_xp = app.career.driver_xp;
        if (app.have_prog_tables) {
          d.driver_next_xp = app.prog_tables.next_driver_level_xp(app.career.driver_xp);
          const int64_t mid = current_make_id();
          const auto ait = app.career.make_affinity_xp.find(mid);
          const int axp = ait == app.career.make_affinity_xp.end() ? 0 : ait->second;
          d.affinity_level = app.prog_tables.affinity_level_for_xp(axp);
          d.affinity_discount_pct = app.prog_tables.affinity_discount_for_xp(axp) * 100.0;
        }
        d.sponsor_title = app.sponsor_banner_title;
        d.sponsor_detail = app.sponsor_banner_detail;
        d.sponsor_at = app.sponsor_banner_at;
        d.name_tags = name_tags;
        carview::RaceHud race_hud;
        const bool race_line = app.race && banner == race_banner();
        if (app.race) {
          race_hud = app.race->hud_info(), d.race = &race_hud;
          race_hud.results_page = results_page;
          race_hud.won_car_name = won_car_name;
          if (!app.race->player_finished()) results_table = false;
          race_hud.results_table = results_table && results_page == 0;
          results_points = race_hud.wristband > 0;
          race_hud.balance = app.career.credits;
          if (grid_menu_open) {
            race_hud.phase = carview::RaceHud::kGrid;
            race_hud.grid_menu_open = true;
            char dbuf[64];
            std::snprintf(dbuf, sizeof dbuf, "< DIFFICULTY: %s (+%d%%) >", difficulty_name(app.race_options.difficulty),
                          race::difficulty_bonus_percent(int(app.race_options.difficulty)));
            race_hud.grid_menu = {"START RACE", dbuf, "QUIT RACE"};
            // The original's PreRace rows (H_GENERIC_TITLE) when its scene draws.
            if (race_screens.active()) {
              race_hud.grid_menu.clear();
              for (auto g : carview::kGridRows) race_hud.grid_menu.push_back(carview::text_of(carview::grid_row_text(g)));
            }
            race_hud.grid_selected = std::clamp(grid_menu_selected, 0, int(race_hud.grid_menu.size()) - 1);
            int64_t cid = 0;
            int clid = 2;
            current_car_info(cid, clid);
            const std::string cname = !app.class_bands.empty() && size_t(clid) < app.class_bands.size()
                                          ? app.class_bands[size_t(clid)].name
                                          : "?";
            const int cpi = [&]() {
              if (const garage::Car* sc = app.career_garage.selected_car(); sc && !app.class_bands.empty())
                return garage::shown_index(sc->performance_index, app.class_bands);
              if (const auto* sp = app.have_career_catalog ? app.career_catalog.find_car(cid) : nullptr; sp && !app.class_bands.empty())
                return garage::shown_index(sp->stock_pi, app.class_bands);
              return 0;
            }();
            race_hud.grid_car_info = carview::car_name(app.car) + "  [" + cname + " " + std::to_string(cpi) + "]";
          }
          // The screens around the race (race_flow.h / race_screens.h).
          race_hud.screen = race_screens.screen();
          race_hud.screen_t = race_screens.elapsed();
          race_hud.stay_car = app.event_car_name;
          if (const auto* cev = app.have_career_catalog ? app.career_catalog.find_event(app.race_event) : nullptr) {
            race_hud.title = race::resolve_event_display_title(cev);
            race_hud.event_type = carview::event_type_text(cev->style);
          }
          // Wristband progress after award: show COLLECT IT FROM RACE CENTRAL whenever a new wristband is pending!
          if (race_hud.phase == carview::RaceHud::kFinished && race_hud.wristband > 0 && !app.wristbands.levels.empty()) {
            const int after = app.career.wristband_points, before = after - race_hud.wristband;
            const int pending = app.have_career_catalog ? app.career_catalog.pending_wristband(after, app.collected_wristband) : -1;
            const int la = pending >= 0 ? pending : app.wristbands.level_index(after);
            const int cur_i = std::clamp(app.collected_wristband, 0, int(app.wristbands.levels.size()) - 1);
            race_hud.band_before = before, race_hud.band_after = after;
            race_hud.band_lo = app.wristbands.levels[size_t(cur_i)].first;
            race_hud.band_hi = size_t(cur_i + 1) < app.wristbands.levels.size() ? app.wristbands.levels[size_t(cur_i + 1)].first : -1;
            if (la >= 0 && size_t(la) < app.wristbands.levels.size())
              race_hud.band_name = carview::game_text(app.wristbands.levels[size_t(la)].second);
            if (race_hud.band_hi > 0) race_hud.band_next = carview::game_text(app.wristbands.levels[size_t(cur_i + 1)].second);
            race_hud.band_up = pending >= 0;
          }
        }
        // Populate exact world distance and class/PI metadata on projected car nameplates
        // for smooth FH1 distance scaling/fading and compact class+PI micro-badges (PT-37).
        if (!d.name_tags.empty()) {
          auto eq_icase = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
              unsigned char ca = static_cast<unsigned char>(a[i]);
              unsigned char cb = static_cast<unsigned char>(b[i]);
              if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca + 32);
              if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb + 32);
              if (ca != cb) return false;
            }
            return true;
          };
          const drive::Vec3 at = vehicle.position();
          if (app.race) {
            std::vector<carview::RaceView::OpponentTag> otags;
            app.race->opponent_tags(otags);
            carview::RaceView::OpponentTag stag;
            const bool has_stag = app.race->showcase_tag(stag);
            for (auto& nt : d.name_tags) {
              if (nt.distance_m < 0.0) {
                for (const auto& ot : otags) {
                  if (eq_icase(ot.name, nt.name)) {
                    nt.distance_m = std::hypot(ot.x - at.x, ot.z - at.z);
                    break;
                  }
                }
                if (nt.distance_m < 0.0 && has_stag && eq_icase(stag.name, nt.name)) {
                  nt.distance_m = std::hypot(stag.x - at.x, stag.z - at.z);
                }
              }
              if (nt.car_class.empty() || nt.pi <= 0) {
                for (const auto& row : race_hud.rows) {
                  if (!row.player && eq_icase(row.name, nt.name)) {
                    if (!row.car_class.empty()) nt.car_class = row.car_class;
                    if (row.pi > 0) nt.pi = row.pi;
                    break;
                  }
                }
              }
            }
          } else if (festival_mgr) {
            for (auto& nt : d.name_tags) {
              if (nt.distance_m >= 0.0 && !nt.car_class.empty() && nt.pi > 0) continue;
              for (const auto& fc : festival_mgr->cars()) {
                const std::string full =
                    "[" + fc.spec.car_class + " " + std::to_string(fc.spec.pi) + "] " + fc.spec.driver_name;
                if (eq_icase(full, nt.name) || eq_icase(fc.spec.driver_name, nt.name)) {
                  nt.distance_m = std::hypot(fc.position.x - at.x, fc.position.z - at.z);
                  if (nt.car_class.empty()) nt.car_class = fc.spec.car_class;
                  if (nt.pi <= 0) nt.pi = fc.spec.pi;
                  break;
                }
              }
            }
          }
        }
        if (!race_line) d.toast = banner;
        d.popup_title = popup.title, d.popup_text = popup.text, d.popup_at = popup.at;
        d.popup_progress = popup.progress, d.popup_colour = popup.colour;
        d.popup_kind = popup.kind, d.popup_icon = popup.icon, d.popup_tag = popup.tag, d.popup_logo = popup.logo;
        // Career header panels (pause menu & sub-menus).
        d.credits = app.career.credits;
        d.popularity_rank = app.current_pop_rank();
        if (!app.wristbands.levels.empty()) {
          const int cur_i = std::clamp(app.collected_wristband, 0, int(app.wristbands.levels.size()) - 1);
          const int pending = app.have_career_catalog ? app.career_catalog.pending_wristband(app.career.wristband_points, app.collected_wristband) : -1;
          d.band_points = app.career.wristband_points;
          d.band_lo = app.wristbands.levels[size_t(cur_i)].first;
          d.band_hi = size_t(cur_i + 1) < app.wristbands.levels.size() ? app.wristbands.levels[size_t(cur_i + 1)].first : -1;
          d.band_name = carview::game_text(app.wristbands.levels[size_t(cur_i)].second) + " WRISTBAND";
          d.band_level = cur_i + 1;
          d.band_collect_ready = pending >= 0;
          const int next_i = pending >= 0 ? pending : (cur_i + 1);
          if (next_i >= 0 && size_t(next_i) < app.wristbands.levels.size())
            d.band_next = carview::game_text(app.wristbands.levels[size_t(next_i)].second);
        }
        d.pause = st.menu_title == "paused";
        d.car_name = carview::car_name(app.car);
        if (const garage::Car* sc = app.career_garage.selected_car(); sc && !app.class_bands.empty()) {
          d.car_class = app.class_bands[size_t(garage::class_of(sc->performance_index, app.class_bands))].name;
          d.car_pi = garage::shown_index(sc->performance_index, app.class_bands);
        } else if (app.have_career_catalog && !app.class_bands.empty()) {
          if (const auto* sp = app.career_catalog.find_car_by_media(app.car)) {
            d.car_class = app.class_bands[size_t(garage::class_of(sp->stock_pi, app.class_bands))].name;
            d.car_pi = garage::shown_index(sp->stock_pi, app.class_bands);
          }
        }
        d.menu_title = st.menu_title;
        d.menu = st.menu;
        d.menu_selected = st.menu_selected;
        carview::CareerBoard board;
        if (career_page.is_open()) {
          board = app.career_board(career_page);
          if (app.road_discovery.ready()) {
            board.roads = app.road_discovery.discovered_count();
            board.roads_total = app.road_discovery.total_roads();
          }
          if (app.have_outposts) {
            board.outposts = app.outpost_progress.discovered_count(app.outposts);
            board.outposts_total = int(app.outposts.outposts.size());
            board.pr_stunts = app.outpost_progress.completed_stunt_count(app.outposts);
            board.pr_stunts_total = int(app.outposts.stunts.size());
          }
          if (app.have_skills_data) board.sponsors = sponsor_rows();
          board.show_sponsors = career_page.show_sponsors();
          board.sponsor_selected = career_page.sponsor_selected();
          d.career = &board;
        }
        // The pause menu from its scene (925_PAUSE_MENU and the screens it
        // opens).
        carview::PauseScreenHud pause_screen;
        std::vector<carview::SponsorChallengeRow> pause_sponsors;
        if (menu && menu->open) {
          const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
          menu->note_shown(now);
          carview::fill_pause_screen(*menu, now, pause_screen);
          if (menu->screen == PauseMenu::Screen::kSponsorChallenges && app.have_skills_data) {
            pause_sponsors = sponsor_rows();
            pause_screen.sponsors = &pause_sponsors;
          }
          d.pause_screen = &pause_screen;
        }
        carview::RallyStageHud rally_stage_hud;
        carview::RallyHubState rally_hub_state;
        carview::RallyServiceAreaState rally_service_state;
        if (app.demo_rally_service && !app.rally_session.active()) {
          if (!app.rally_catalog.loaded) {
            const auto db_ptr = app.parts_db.valid() ? app.parts_db.get() : std::shared_ptr<cars::PartsDb>{};
            if (!*app.car_names_loaded) carview::car_name("");
            std::string rerr;
            race::load_rally_catalog(app.state_root, db_ptr.get(), app.car_names->strings(), app.rally_catalog, &rerr);
          }
          if (app.rally_catalog.loaded && !app.rally_catalog.championships.empty()) {
            app.rally_session.start_championship(app.rally_catalog, app.rally_catalog.championships.front().event_id,
                                             int(app.race_options.difficulty));
            app.rally_session.record_stage_finish(app.rally_catalog, 134.82);
            app.rally_session.advance_from_service_area();
            app.rally_session.record_stage_finish(app.rally_catalog, 148.19);
            app.rally_service_open = true;
            app.rally_service_selected = 0;
            app.rally_service_repaired = false;
          }
        }
        if (app.demo_club && !club_open) {
          club_check();
          if (club_gate.empty() && club_car >= 0 && app.club_data.cars.count(club_car)) {
            const auto& clist = app.club_data.cars.at(club_car);
            drive::SavedClubCar* rec = club_record();
            if (!rec) {
              drive::SavedClubCar nrec;
              nrec.car_id = club_car;
              for (const auto& ch : clist) {
                nrec.challenges.emplace_back(ch.definition_hash, false);
                nrec.progress.emplace_back();
              }
              app.saved.club.push_back(std::move(nrec));
              rec = club_record();
            }
            if (rec && rec->challenges.size() >= 2) {
              rec->challenges[0].second = true;
              rec->challenges[1].second = true;
              club_check();
              club_ensure_meta();
              const fh1::StringTables* stbls = app.car_names->strings();
              const auto ms = club_medal_stats();
              club_ceremony = {};
              club_ceremony.active = true;
              club_ceremony.challenge_title = clist[1].format_title(stbls);
              club_ceremony.challenge_desc = clist[1].format_description(stbls, &club_event_names);
              club_ceremony.car_name = carview::car_name(app.car);
              club_ceremony.car_medals = 2;
              club_ceremony.car_total = int(clist.size());
              club_ceremony.total_medals = ms.total_medals;
              club_ceremony.cars_with_medal = ms.cars_with_medal;
              club_ceremony.age = 1.2;
              club_ceremony_at = vehicle.time() - 1.2;
            }
            club_open = true;
            club_pick = 2 < clist.size() ? 2 : 0;
          }
        }
        if (app.race && app.race->is_stage() && (race_hud.phase == carview::RaceHud::kRacing || app.demo_rally_stage)) {
          rally_stage_hud.active = true;
          rally_stage_hud.championship_name = app.rally_session.active() ? app.rally_session.title() : "BEAUMONT RALLY";
          rally_stage_hud.stage_name = race_hud.event;
          rally_stage_hud.stage_number = app.rally_session.active() ? (app.rally_session.current_stage_index() + 1) : 1;
          rally_stage_hud.total_stages = app.rally_session.active() ? app.rally_session.total_stages() : 4;
          rally_stage_hud.stage_clock = race_hud.clock;
          rally_stage_hud.progress = race_hud.progress;
          rally_stage_hud.total_splits = std::max(4, int(app.race->stage_track().splits.size()));
          rally_stage_hud.splits_passed = int(app.race->split_times().size());
          rally_stage_hud.last_split_delta = app.race->last_split_delta();
          rally_stage_hud.last_split_age = race_hud.clock - app.race->last_split_at();
          rally_stage_hud.pace = app.race->codriver().hud(race_hud.clock);
          if (app.demo_rally_stage && rally_stage_hud.splits_passed == 0) {
            rally_stage_hud.splits_passed = 1;
            rally_stage_hud.last_split_delta = -1.42;
            rally_stage_hud.last_split_age = 0.8;
          }
          if (app.demo_rally_stage && !rally_stage_hud.pace.active) {
            rally_stage_hud.splits_passed = std::max(1, rally_stage_hud.splits_passed);
            rally_stage_hud.last_split_delta = -1.42;
            rally_stage_hud.last_split_age = 0.8;
            rally_stage_hud.pace.active = true;
            rally_stage_hud.pace.age_s = 0.4;
            rally_stage_hud.pace.icons.push_back({race::PaceIconKind::kHardLeft, 2, -1, "2L", true});
            rally_stage_hud.pace.icons.push_back({race::PaceIconKind::kCrest, 0, 0, "CREST", true});
            rally_stage_hud.pace.icons.push_back({race::PaceIconKind::kMediumRight, 4, 1, "4R", true});
            rally_stage_hud.pace.modifiers = {"DON'T CUT", "50", "GRAVEL"};
            rally_stage_hud.pace.phrase = "2 LEFT  •  OVER CREST  •  INTO 4 RIGHT";
          }
          d.rally_stage = &rally_stage_hud;
        }
        if (app.rally_hub_open && app.rally_catalog.loaded) {
          int64_t cid = 0;
          int clid = 4;
          current_car_info(cid, clid);
          const int disc = host.free_roam ? freeroam::upgrade_discount(host.free_roam->signs_found()) : 0;
          app.saved.credits = app.career.credits;
          const auto db_ptr = app.parts_db.valid() ? app.parts_db.get() : std::shared_ptr<cars::PartsDb>{};
          rally_hub_state = carview::make_rally_hub_state(app.rally_catalog, app.saved, app.have_garage ? &app.career_garage : nullptr,
                                                          db_ptr.get(), app.class_bands, app.car, clid, d.car_pi, disc,
                                                          app.rally_hub_selected, app.rally_notice);
          d.rally_hub = &rally_hub_state;
        }
        if (app.rally_service_open && app.rally_session.active()) {
          const double dmg_pct =
              std::clamp(double(std::max({car_damage.scratch_amount, car_damage.engine_damage,
                                          car_damage.trans_damage, car_damage.brake_damage, car_damage.zone_front,
                                          car_damage.zone_rear, car_damage.zone_left, car_damage.zone_right})) *
                             100.0,
                         0.0, 100.0);
          rally_service_state = carview::make_rally_service_state(app.rally_session, d.car_name, d.car_class, d.car_pi,
                                                                  dmg_pct, app.rally_service_repaired,
                                                                  app.rally_service_selected, app.rally_notice);
          d.rally_service = &rally_service_state;
        }
        if (app.rally_results_open) d.rally_results = &app.rally_results_state;
        d.touch_hud_active = app.touch_controls.visible(vehicle.time());
        d.metric_units = app.metric_units;
        replay_viewer.set_metric_units(app.metric_units);
        d.subtitle_active = !app.campaign.is_cinematic_camera_active() &&
                            app.campaign.flow().dialogs().active() &&
                            !app.campaign.flow().dialogs().current().subtitle.empty();
        if (d.subtitle_active) {
          const float sf = float(height) / 720.0f;
          const float max_box_w = std::min(float(width) - 460.0f * sf, 720.0f * sf);
          const float max_text_w = std::max(240.0f * sf, max_box_w - 36.0f * sf);
          const auto sub_ts = carview::ui4::style(carview::ui::Font::kA, 15.5f * sf, carview::ui4::kWhite,
                                                  carview::ui::Align::kLeft, true);
          const std::string& raw_sub = app.campaign.flow().dialogs().current().subtitle;
          std::string wrapped;
          std::string cur_line;
          size_t pos = 0;
          int lines = 1;
          while (pos < raw_sub.size()) {
            size_t sp = raw_sub.find(' ', pos);
            const std::string word = (sp == std::string::npos) ? raw_sub.substr(pos) : raw_sub.substr(pos, sp - pos);
            const std::string cand = cur_line.empty() ? word : (cur_line + " " + word);
            if (!cur_line.empty() && carview::ui::List::measure(cand, sub_ts) > max_text_w) {
              if (!wrapped.empty()) wrapped += '\n';
              wrapped += cur_line;
              cur_line = word;
              ++lines;
            } else {
              cur_line = cand;
            }
            if (sp == std::string::npos) break;
            pos = sp + 1;
          }
          if (!cur_line.empty()) {
            if (!wrapped.empty()) wrapped += '\n';
            wrapped += cur_line;
          }
          const float th_px = carview::ui::List::measure_height(wrapped, sub_ts);
          d.subtitle_lines = lines;
          d.subtitle_box_h_canvas = std::max(46.0f, (sf > 1e-4f ? th_px / sf : th_px) + 20.0f);
        } else {
          d.subtitle_lines = 0;
          d.subtitle_box_h_canvas = 0.0f;
        }
        carview::ui::List list;
        if (app.show_title && !settings_page.is_open() && app.title_video.active()) {
          app.title_video.draw(list, float(width), float(height), {}, false);
        }
        // Minimap under HUD shapes; hidden in menus, title screen, ceremony, Rally/Club screens, campaign cinematics, and grid menu.
        const bool pre_race_swoop_hide_hud = pre_race_blend_t >= 0.0 && pre_race_blend_t < 0.85;
        const bool campaign_hide_hud = app.campaign.is_blocking_gameplay() || pre_race_swoop_hide_hud;
        if (mm_size > 0 && !app.show_title && !ceremony.active && !campaign_hide_hud && !map_overlay.active &&
            !grid_menu_open && st.menu.empty() && !career_page.is_open() && !app.rally_hub_open &&
            !app.rally_service_open && !app.rally_results_open && !club_open && !car_club_open)
          list.image(mm_x, mm_y, mm_x + mm_size, mm_y + mm_size, mm_uv);
        // The 1000 Club's screens hide the driving HUD (club_home.png).
        if ((club_open && club_menu.open()) || rally_entry != carview::RallyEntryPopup::kNone) d.driving = false;
        if (!campaign_hide_hud) carview::build_drive_hud(list, d, float(width), float(height));
        // Brief white flash on confirming START RACE (CAMERAS.md:85, UI_LAYOUTS.md:121-125).
        if (pre_race_blend_t >= 0.0 && pre_race_blend_t < 0.12) {
          const float fa = float(std::clamp(1.0 - pre_race_blend_t / 0.12, 0.0, 1.0)) * 0.65f;
          list.rect(0.0f, 0.0f, float(width), float(height), carview::ui::rgba(255, 255, 255, uint8_t(fa * 255.0f)));
        }
        // The intro video ended (or was skipped): the home screen.
        if (rally_enter_after_video && !app.fmv_player.active()) {
          rally_enter_after_video = false;
          rally_enter();
        }
        if (club_open && club_menu.screen == carview::ClubMenu::Screen::kIntro && !app.fmv_player.active())
          club_menu_press(carview::ClubMenu::Button::kAccept);
        // The 1000 Club's v4 HUD (947_HUD's HUDCarChallengePrompt) replaces the
        // hand-drawn strip and medal ceremony when the title update has it.
        bool club_v4_hud = false;
        if (club_open) {
          if (!club_menu.open() ||
              !carview::draw_club_menu(list, club_menu, club_menu_data, float(width), float(height), &d)) {
            const auto club_sc = build_club_screen();
            carview::draw_club_screen(list, width, height, club_sc);
          }
        } else if (car_club_open) {
          carview::CarClubScreenState cc;
          cc.active = true;
          cc.club_name = app.saved.car_club_name;
          cc.club_tag = app.saved.car_club_tag;
          cc.selected = car_club_pick;
          cc.borrowed_garage_id = app.have_garage ? app.career_garage.club_borrowed_garage_id : 0;
          cc.notice = car_club_notice;
          for (size_t ci = 0; ci < app.baked_cars.size(); ++ci) {
            carview::CarClubCarEntry ce;
            ce.media_name = app.baked_cars[ci];
            ce.display_name = carview::car_name(ce.media_name);
            ce.active = (ce.media_name == app.car);
            if (app.have_garage && ci < app.pick_ids.size()) {
              ce.garage_id = app.pick_ids[ci];
              if (const garage::Car* gc = app.career_garage.find(app.pick_ids[ci])) {
                ce.car_id = int(gc->car_id);
                if (!app.class_bands.empty()) {
                  const std::string cls =
                      app.class_bands[size_t(garage::class_of(gc->performance_index, app.class_bands))].name;
                  ce.class_pi = cls + " " + std::to_string(garage::shown_index(gc->performance_index, app.class_bands));
                }
              }
            }
            if (ce.car_id <= 0 && app.have_career_catalog) {
              if (const auto* sp = app.career_catalog.find_car_by_media(ce.media_name)) ce.car_id = int(sp->car_id);
            }
            if (ce.car_id <= 0) ce.car_id = int(ci + 1);
            if (app.have_garage && ce.garage_id > 0) {
              const auto& sg = app.career_garage.club_shared_garage_ids;
              ce.shared = std::find(sg.begin(), sg.end(), ce.garage_id) != sg.end();
              ce.borrowed = (cc.borrowed_garage_id > 0 && ce.garage_id == cc.borrowed_garage_id);
            } else {
              ce.shared = std::find(app.saved.car_club_shared.begin(), app.saved.car_club_shared.end(), ce.car_id) !=
                          app.saved.car_club_shared.end();
            }
            cc.cars.push_back(std::move(ce));
          }
          list.clear();
          carview::draw_car_club_screen(list, width, height, cc);
        } else if (!app.show_title && !ceremony.active && !campaign_hide_hud && !app.rally_hub_open &&
                   !app.rally_service_open && !app.rally_results_open && st.menu.empty() &&
                   rally_entry == carview::RallyEntryPopup::kNone) {
          club_v4_hud = carview::draw_club_hud(list, build_club_hud(), float(width), float(height));
          if (!club_v4_hud) {
            const auto cstrip = build_club_active_strip();
            if (cstrip.active) carview::draw_club_active_strip(list, width, height, cstrip);
          }
        }
        if (rally_entry != carview::RallyEntryPopup::kNone)
          carview::draw_rally_entry_popup(list, rally_entry, float(width), float(height));
        // No bottom banner under the v4 HUD or the Rally entry popup.
        club_v4_hud_shown = club_v4_hud || rally_entry != carview::RallyEntryPopup::kNone;
        if (club_ceremony.active) {
          club_ceremony.age = app.demo_club ? 1.2 : (vehicle.time() - club_ceremony_at);
          club_ceremony.dock_bottom_right = club_open;
          if (club_ceremony.age > club_ceremony.duration || club_v4_hud) club_ceremony.active = false;
          else carview::draw_club_medal_ceremony(list, width, height, club_ceremony);
        }
        if (!app.show_title && !ceremony.active && !campaign_hide_hud && radio_popup_gpu.alpha > 0.005f)
          carview::draw_radio_popup_gpu(list, radio_popup_gpu, float(width), float(height));
        if (app.story_intro.stage() == carview::StoryStage::kBootSplash)
          app.story_intro.draw_hud(list, float(width), float(height));
        const bool suppress_campaign_hud =
            map_overlay.active || !st.menu.empty() || settings_page.is_open() || career_page.is_open() ||
            events_page.is_open() || outpost_hub_open || garage_open || shop.is_open() || autoshow.is_open() ||
            paint_open || club_open || car_club_open || app.rally_hub_open || app.rally_service_open ||
            app.rally_results_open || grid_menu_open;
        if (!app.show_title && !ceremony.active && (!suppress_campaign_hud || app.campaign.is_blocking_gameplay())) {
          carview::campaign_ui::DrawContext cctx;
          cctx.screen_w = float(width);
          cctx.screen_h = float(height);
          cctx.wristband_points = app.career.wristband_points;
          cctx.collected_wristband = app.collected_wristband;
          cctx.popularity_rank = app.current_pop_rank();
          cctx.skip_vo_with_a = app.skip_vo_with_a;
          cctx.metric_units = app.metric_units;
          cctx.catalog = app.have_career_catalog ? &app.career_catalog : nullptr;
          cctx.race_screens = race_screens.active() || entry_screens.active();
          // Headless runs do not step the career: its screens and fades would
          // stay frozen over every frame, so they are left out there.
          if (app.campaign.ftc_active()) {
            if (!(app.headless && app.campaign.ftc().show_controls_screen()))
              carview::ftc_ui::draw(list, app.campaign.ftc(), float(width), float(height), live_controls.layout_id,
                                    live_controls.switch_handbrake_clutch);
          } else {
            carview::campaign_ui::draw(list, app.campaign.flow(), cctx);
          }
          if (!app.campaign.is_blocking_gameplay())
            carview::ftc_ui::draw_hint(list, app.campaign.hint_message(), float(width), float(height));
        }
        if (app.fmv_player.active()) {
          list.clear();
          app.fmv_player.draw(list, float(width), float(height), {}, true, app.campaign.skip_hold_progress());
          carview::campaign_ui::draw_transition(list, app.campaign.flow().transition(), float(width), float(height));
        }
        if (!app.show_title && !app.headless)
          carview::ftc_ui::draw_overlay(list, app.campaign.screen_overlay(), float(width), float(height));
        static double s_loading_fade_last_t = -1.0;
        if (loading_screen.active()) {
          s_loading_fade_last_t = d.time;
          list.clear();
          loading_screen.draw(list, float(width), float(height));
        } else if (loading_screen.fading_out()) {
          const double fdt = (s_loading_fade_last_t >= 0.0 && d.time > s_loading_fade_last_t)
                                 ? std::clamp(d.time - s_loading_fade_last_t, 0.0, 0.05)
                                 : (1.0 / 60.0);
          s_loading_fade_last_t = d.time;
          loading_screen.update(fdt);
          if (loading_screen.visible()) loading_screen.draw(list, float(width), float(height));
        } else {
          s_loading_fade_last_t = d.time;
        }
        if (rewind_buf.active()) {
          const int kf_count = rewind_buf.keyframe_count();
          const double oldest_t = kf_count > 0 ? rewind_buf.keyframe(0).replay_time : 0.0;
          const double latest_t = kf_count > 0 ? std::max(vehicle.time(), rewind_buf.keyframe(kf_count - 1).replay_time) : 0.0;
          const auto rw_sc = carview::compute_rewind_scrubber(
              kf_count, drive::RewindConfig::kMaxKeyframes, rewind_buf.selected_slot(), oldest_t, latest_t,
              rewind_buf.playback_time(), d.time);
          carview::build_rewind_overlay(list, float(width), float(height), rewind_buf.is_rewinding(),
                                        rewind_buf.ui_state() == drive::RewindUiState::kLoopRewind,
                                        rewind_buf.is_paused_at_keyframe(), rewind_buf.at_oldest_slot(),
                                        rewind_buf.is_settling(), &rw_sc);
        }
        if (slice12.trainer_overlay.is_open()) {
          list.clear();
          carview::draw_trainer_ui4(list, float(width), float(height), slice12.trainer_overlay, slice12.trainer,
                                    app.career.credits, std::max(0, app.collected_wristband), slice12.save_tree_label);
        } else if (slice12.cam_editor_overlay.is_open()) {
          list.clear();
          carview::draw_camera_editor_ui4(list, float(width), float(height), slice12.cam_editor_overlay,
                                          slice12.ht_receiver.current_pose());
        } else if (mods_page.is_open()) {
          list.clear();
          carview::draw_mods_page(list, float(width), float(height), mods_page);
        } else if (save_page.is_open()) {
          list.clear();
          carview::draw_save_page(list, float(width), float(height), save_page, app.save_session);
        } else if (settings_page.is_open()) {
          list.clear();
          carview::draw_settings_ui4(list, float(width), float(height), settings_page, settings_live_overlay);
        } else if (events_page.is_open()) {
          list.clear();
          carview::draw_events_ui4(list, float(width), float(height), events_page, app.class_bands);
        } else if (outpost_hub_open) {
          const freeroam::Outpost* op = app.have_outposts ? app.outposts.find_outpost(outpost_hub_id) : nullptr;
          std::vector<carview::OutpostMissionCard> mcards;
          mcards.reserve(outpost_hub_items.size());
          for (const auto& item : outpost_hub_items) {
            carview::OutpostMissionCard mc;
            mc.title = item.detail_title.empty() ? item.label : item.detail_title;
            mc.detail_lines = item.detail_lines;
            if (item.kind == OutpostHubItem::kPrStunt) {
              mc.is_stunt = true;
              const freeroam::PrStunt* pst = app.have_outposts ? app.outposts.find_stunt(item.ref_id) : nullptr;
              mc.completed = app.outpost_progress.is_stunt_completed(item.ref_id);
              mc.kind_label = pst ? freeroam::pr_stunt_kind_name(pst->kind) : "PR STUNT";
              mc.art_style = (pst && pst->kind == freeroam::PrStuntKind::kPhoto)   ? 12
                             : (pst && pst->kind == freeroam::PrStuntKind::kSkill) ? 11
                                                                                   : 10;
              if (pst) {
                if (pst->kind == freeroam::PrStuntKind::kSpeed)
                  mc.target_line = "TARGET: " + carview::format_speed_unit(pst->target_mph, app.metric_units, false);
                else if (pst->kind == freeroam::PrStuntKind::kSkill)
                  mc.target_line = "TARGET: " + carview::thousands(pst->target_points) + " PTS";
                else
                  mc.target_line = "REACH LANDMARK & SNAP PHOTO";
                mc.reward_line = "FAST TRAVEL DISCOUNT";
              }
              mc.status_line = mc.completed ? "COMPLETED (REPLAY)" : "NEW PR STUNT";
            } else {
              mc.is_stunt = false;
              mc.kind_label = (item.kind == OutpostHubItem::kGarage)       ? "GARAGE"
                              : (item.kind == OutpostHubItem::kNearbyEvents) ? "EVENTS"
                                                                             : "FAST TRAVEL";
              mc.art_style = (item.kind == OutpostHubItem::kNearbyEvents) ? 2 : 1;
              mc.target_line = item.detail_lines.empty() ? item.label : item.detail_lines[0];
              mc.status_line = "SELECT";
            }
            mcards.push_back(std::move(mc));
          }
          list.clear();
          carview::draw_outpost_hub_ui4(list, float(width), float(height),
                                        op ? op->display_name : std::string("HORIZON OUTPOST"), mcards,
                                        int(outpost_hub_pick));
        } else if (autoshow.is_open()) {
          // Live 3D showroom car sync (108_C_BUY_CARSELECT / 114_C_BUY_BUYCAR_COLOR):
          // sync the 3D showroom car model & factory swatch to the currently highlighted Autoshow car!
          if (const carview::AutoshowCar* sel_as = autoshow.selected_car()) {
            const std::string& target_media = sel_as->dealer.media_name;
            cars::CarPaintSpec sw_paint{};
            bool have_sw_paint = false;
            if (const carview::AutoshowSwatch* sw = autoshow.selected_swatch()) {
              sw_paint.body.active = true;
              sw_paint.body.rgb = sw->rgb;
              sw_paint.body.finish = sw->metallic ? cars::PaintFinish::kMetallic : cars::PaintFinish::kNormal;
              have_sw_paint = true;
            }
            if (!target_media.empty() && target_media != app.car) {
              poll_async_guest_loads();
              auto it = festival_guests.find(target_media);
              if (it == festival_guests.end()) {
                request_guest_load_async(target_media, target_media, ShowroomOptions{}, 36);
              }
              if (it != festival_guests.end() && it->second) {
                if (have_sw_paint) it->second->set_paint_spec(sw_paint);
                host.showroom.set_draw_car(false);
                Showroom::Instance inst{};
                inst.car = it->second.get();
                inst.model[0] = 1.0f;
                inst.model[5] = 1.0f;
                inst.model[10] = 1.0f;
                inst.model[15] = 1.0f;
                host.showroom.set_instances({inst});
              }
            } else {
              host.showroom.set_draw_car(true);
              if (have_sw_paint) host.showroom.set_paint_spec(sw_paint);
            }
          }
          list.clear();
          carview::draw_autoshow_hud(list, float(width), float(height), autoshow, app.class_bands, app.career.credits,
                                     vehicle.time() < notice_until ? notice : std::string(), app.metric_units);
        } else if (shop.is_open()) {
          list.clear();
          carview::draw_upgrade_shop_hud(list, float(width), float(height), shop, app.class_bands, discount(),
                                         app.career.credits, carview::car_name(app.car),
                                         vehicle.time() < notice_until ? notice : std::string(), app.metric_units);
        } else if (paint_open && livery_editor.is_open()) {
          list.clear();
          carview::draw_livery_editor_hud(list, float(width), float(height), livery_editor, carview::car_name(app.car),
                                          vehicle.time() < notice_until ? notice : std::string());
        } else if (garage_open && app.have_garage && !app.career_garage.cars.empty()) {
          if (garage_browser.visible().empty()) {
            const cars::PartsDb* gdb =
                (app.parts_db.valid() && app.parts_db.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    ? app.parts_db.get().get()
                    : nullptr;
            garage_browser.refresh(app.career_garage, gdb, app.class_bands);
          }
          if (!garage_browser.visible().empty()) {
            list.clear();
            carview::draw_garage_hud(list, float(width), float(height), garage_browser, app.class_bands, app.career.credits,
                                     vehicle.time() < notice_until ? notice : std::string(), app.metric_units);
          }
        }
        if (barn_card.visible) {
          carview::draw_barn_find_hud(list, float(width), float(height), barn_card);
        }
        const bool show_track_chip = !app.show_title && !app.fmv_player.active() && !barn_card.visible && !ceremony.active && !map_overlay.active && !grid_menu_open &&
                                     st.menu.empty() && !career_page.is_open() && !settings_page.is_open() &&
                                     !shop.is_open() && !autoshow.is_open() && !garage_open &&
                                     !events_page.is_open() && !outpost_hub_open && !paint_open && !club_open &&
                                     !car_club_open && !rewind_buf.active();
        // The tracked-achievement card is the engine's own (opt-in HUD row);
        // unlock toasts still draw whatever the row says.
        achievements.build_hud_overlays(list, float(width), float(height),
                                        show_track_chip && live_hud.tracked_achievement);
        if (show_track_chip) {
          app.touch_controls.build_overlay(list, float(width), float(height), vehicle.time());
        }
        auto append_modern_overlays = [&](carview::ui::List& target_list) {
          if (host.perf_overlay_open) {
            const auto vram = host.renderer.vram_stats();
            const auto ws = host.showroom.world_draw_stats();
            const auto up = host.renderer.active_upscaler();
            const auto fg = host.renderer.active_frame_gen();
            carview::PerfInfo pinfo;
            const auto rex = host.renderer.render_width()
                                 ? carview::modern::Extent{host.renderer.render_width(), host.renderer.render_height()}
                                 : host.renderer.expected_render_extent(uint32_t(width), uint32_t(height));
            const uint32_t out_w = host.renderer.target_width() ? host.renderer.target_width() : uint32_t(width);
            const uint32_t out_h = host.renderer.target_height() ? host.renderer.target_height() : uint32_t(height);
            pinfo.fps = app.frame_stats.fps();
            pinfo.fps_1pct_low = app.frame_stats.ms_low_fps(0.01);
            pinfo.rss_mib = double(carview::bench::query_process_memory().rss_bytes) / (1024.0 * 1024.0);
            pinfo.cpu_ms = host.last_frame_cpu_ms >= 0.0 ? host.last_frame_cpu_ms : app.frame_stats.ms_avg();
            if (app.headless && pinfo.fps < 0.5 && pinfo.cpu_ms > 0.1) pinfo.fps = 1000.0 / pinfo.cpu_ms;
            pinfo.generated_fps = app.frame_stats.generated_fps();
            pinfo.render_w = rex.w;
            pinfo.render_h = rex.h;
            pinfo.output_w = out_w;
            pinfo.output_h = out_h;
            pinfo.gpu_ms = host.renderer.gpu_frame_ms();
            pinfo.gpu_stages = host.renderer.gpu_stages();
            pinfo.vram_used = vram.device_local_usage ? vram.device_local_usage : vram.allocation_bytes;
            pinfo.vram_budget = vram.device_local_budget;
            pinfo.resident_draws = host.showroom.gpu_resident_draws();
            pinfo.resident_cells = pinfo.resident_draws;
            pinfo.draws_drawn = uint32_t(ws.drawn);
            pinfo.draws_culled = uint32_t(ws.culled);
            pinfo.hdr = host.renderer.hdr_active() != carview::modern::HdrMode::kOff;
            pinfo.vsync = host.active_graphics.vsync == carview::modern::VSync::kImmediate ? "IMMEDIATE"
                          : host.active_graphics.vsync == carview::modern::VSync::kMailbox ? "MAILBOX"
                                                                                       : "FIFO";
            pinfo.upscaler = up.label;
            pinfo.frame_generation = carview::modern::frame_gen_name(fg);
            pinfo.backend = host.renderer.active_backend_label();
            pinfo.device = host.renderer.device_name();
            pinfo.build = std::string("shift-engine ") + SHIFT_GIT_COMMIT + " (" + SHIFT_BUILD_CONFIG + ")";
            carview::build_perf_overlay(target_list, pinfo, float(width), float(height), 1.0f);
          }
          if (host.inspector_open) {
            host.renderer.set_hiz_preview_view(host.showroom.hiz_image_view());
            std::vector<uint8_t> insp_rgba;
            uint32_t insp_w = 0, insp_h = 0;
            if (host.renderer.take_inspector_preview(insp_rgba, insp_w, insp_h) && insp_w > 0 && insp_h > 0) {
              host.has_inspector_preview = carview::ui::atlas_update("inspector.preview", insp_w, insp_h, insp_rgba,
                                                                host.inspector_preview_uv);
            }
            const auto vram = host.renderer.vram_stats();
            const auto ws = host.showroom.world_draw_stats();
            const auto up = host.renderer.active_upscaler();
            const auto rex = host.renderer.render_width()
                                 ? carview::modern::Extent{host.renderer.render_width(), host.renderer.render_height()}
                                 : host.renderer.expected_render_extent(uint32_t(width), uint32_t(height));
            carview::FrameInspectorSnapshot snap;
            snap.open = true;
            snap.view = host.inspector_view;
            snap.render_w = rex.w;
            snap.render_h = rex.h;
            snap.output_w = host.renderer.target_width() ? host.renderer.target_width() : uint32_t(width);
            snap.output_h = host.renderer.target_height() ? host.renderer.target_height() : uint32_t(height);
            snap.cpu_ms = host.last_frame_cpu_ms >= 0.0 ? host.last_frame_cpu_ms : app.frame_stats.ms_avg();
            snap.gpu_ms = host.renderer.gpu_frame_ms();
            snap.pass_timings = host.renderer.gpu_stages();
            snap.world_draws = uint32_t(ws.drawn);
            snap.world_culled = uint32_t(ws.culled);
            snap.world_triangles = uint32_t(ws.indices / 3);
            snap.shadow_draws = uint32_t(host.showroom.shadow_draw_count());
            snap.shadow_cars = uint32_t(host.showroom.shadow_car_count());
            snap.resident_draws = host.showroom.gpu_resident_draws();
            snap.resident_cells = snap.resident_draws;
            snap.vram_used = vram.device_local_usage ? vram.device_local_usage : vram.allocation_bytes;
            snap.vram_budget = vram.device_local_budget;
            snap.vrs_active = host.renderer.vrs_active();
            snap.reverse_z = host.renderer.reverse_z();
            snap.z_prepass = app.z_prepass;
            snap.hdr_mode = host.renderer.hdr_active() == carview::modern::HdrMode::kHdr10
                                ? "HDR10"
                                : host.renderer.hdr_active() == carview::modern::HdrMode::kScRgb ? "scRGB" : "SDR";
            snap.upscaler = carview::modern::upscaler_name(up.active);
            snap.preview_uv = host.inspector_preview_uv;
            snap.has_preview = host.has_inspector_preview;
            carview::build_frame_inspector(target_list, snap, float(width), float(height));
          }
        };
        append_modern_overlays(list);
        upload_ui_atlas(host.renderer, !ui_atlas_synced);
        ui_atlas_synced = true;
        last_ui_hits.assign(list.hits().begin(), list.hits().end());
        last_ui_w = width;
        last_ui_h = height;
        host.renderer.set_ui(list.items());
        if (!legacy_hud_cleared) host.renderer.set_overlay({}, 0, 0, 0, 0, 0), legacy_hud_cleared = true;
        hud_height = int(carview::drive_hud_dial_height(float(height)));
}

}  // namespace carview
