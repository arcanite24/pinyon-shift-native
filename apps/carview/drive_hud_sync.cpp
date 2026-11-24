#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "campaign_ui.h"
#include "fh1data/xds.h"
#include "hub_scene.h"
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
          cam_car_coll_long *= std::exp(-dt * 12.0);
          cam_car_coll_lat *= std::exp(-dt * 12.0);
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
        const int signs_disc = freeroam::upgrade_discount(host.free_roam ? host.free_roam->signs_found() : int(app.saved.signs_found.size()));
        return std::min(100, signs_disc + affinity_discount_pct());
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
          item.detail_lines.push_back("REWARD: " + carview::thousands(st->reward_credits) + " Cr   +   " +
                                      carview::thousands(st->reward_popularity) + " POPULARITY");
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
          const double dist_km = std::hypot(item.dest_x - at.x, item.dest_z - at.z) / 1000.0;
          char db[64];
          std::snprintf(db, sizeof db, "DISTANCE: %.1f KM   •   FREE FAST TRAVEL", dist_km);
          item.detail_title = "HORIZON FESTIVAL";
          item.detail_lines = {"RETURN TO RACE CENTRAL, AUTOSHOW,", "DAK'S GARAGE AND THE PAINT SHOP", db};
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
          const double dist_km = std::hypot(item.dest_x - at.x, item.dest_z - at.z) / 1000.0;
          char db[64];
          std::snprintf(db, sizeof db, "DISTANCE: %.1f KM   •   UNLOCKED", dist_km);
          item.detail_title = other.display_name;
          item.detail_lines = {"FAST TRAVEL DIRECTLY TO THIS REGIONAL", "HORIZON OUTPOST", db};
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
        d.tcs = vehicle.tcs_active() || vehicle.stm_active();
        d.detail_title = std::move(detail_title);
        d.detail_lines = std::move(detail_lines);
        // Title Screen HUD (when active and settings isn't open over it).
        carview::TitleScreenHud ts_hud;
        if (app.show_title && !settings_page.is_open()) {
          ts_hud.active = true;
          ts_hud.menu_phase = app.title_menu_phase;
          ts_hud.has_save = app.have_saved && (app.saved.career_started || app.saved.owned_garage || app.career.num_races > 0 || app.career.credits > 0);
          ts_hud.confirm_new_career = app.title_confirm_new;
          ts_hud.confirm_selected = app.title_confirm_selected;
          if (ts_hud.has_save)
            ts_hud.menu = {"CONTINUE CAREER", "NEW CAREER", "FREE ROAM", "SETTINGS", "QUIT"};
          else
            ts_hud.menu = {"NEW CAREER", "FREE ROAM", "SETTINGS", "QUIT"};
          ts_hud.selected = std::clamp(app.title_selected, 0, int(ts_hud.menu.size()) - 1);
          if (ts_hud.has_save) {
            const int li = std::clamp(app.collected_wristband, 0, std::max(0, int(app.wristbands.levels.size()) - 1));
            if (!app.wristbands.levels.empty())
              ts_hud.save_band_name = carview::game_text(app.wristbands.levels[size_t(li)].second) + " WRISTBAND";
            ts_hud.save_band_points = app.career.wristband_points;
            ts_hud.save_pop_rank = app.current_pop_rank();
            ts_hud.save_car_name = carview::car_name(app.car);
            if (const garage::Car* sc = app.career_garage.selected_car(); sc && !app.class_bands.empty()) {
              ts_hud.save_car_class = app.class_bands[size_t(garage::class_of(sc->performance_index, app.class_bands))].name;
              ts_hud.save_car_pi = garage::shown_index(sc->performance_index, app.class_bands);
            }
            ts_hud.save_credits = app.career.credits;
            ts_hud.save_races = app.career.num_races;
            ts_hud.save_wins = app.career.num_victories;
          }
          d.title_screen = &ts_hud;
        }
        if (ceremony.active) d.ceremony = &ceremony;
        if (map_overlay.active) d.map_overlay = &map_overlay;
        carview::SkillChainHud sk_hud;
        if (app.skill_tracker) {
          const auto sh = app.skill_tracker->hud();
          sk_hud.active = sh.active;
          sk_hud.chain_points = sh.chain_points;
          sk_hud.multiplier = sh.multiplier;
          sk_hud.last_skill = sh.last_skill;
          sk_hud.last_points = sh.last_points;
          sk_hud.last_skill_at = sh.last_skill_at;
          sk_hud.banked_total = sh.banked_total;
          sk_hud.banked_at = sh.banked_at;
          sk_hud.lost_at = sh.lost_at;
          sk_hud.popularity_rank = sh.popularity_rank;
          sk_hud.popularity_progress = sh.popularity_progress;
          sk_hud.rank_up_at = sh.rank_up_at;
          d.skills = &sk_hud;
        }
        if (world_marker.visible && !app.race) d.world_marker = &world_marker;
        carview::GpsHud gps_hud;
        static double s_gps_uturn_since = -1.0;
        if (!app.race && app.gps_nav.active()) {
          const auto& gg = app.gps_nav.guidance();
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
          d.gps = &gps_hud;
        } else {
          s_gps_uturn_since = -1.0;
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
        if (!race_line) d.toast = banner;
        d.popup_title = popup.title, d.popup_text = popup.text, d.popup_at = popup.at;
        d.popup_progress = popup.progress, d.popup_colour = popup.colour;
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
          if (app.have_skills_data) {
            for (const auto& sc : app.skills_data.sponsor_challenges) {
              carview::SponsorChallengeRow srow;
              srow.sponsor_name = sc.sponsor_name.empty() ? sc.id : sc.sponsor_name;
              srow.challenge_title = sc.challenge_name.empty() ? sc.id : sc.challenge_name;
              srow.logo_key = sc.sponsor_name.empty() ? sc.id : sc.sponsor_name;
              srow.max_tiers = std::max(1, int(sc.tiers.size()));
              if (app.skill_tracker) {
                if (auto it = app.skill_tracker->sponsor_progress().find(sc.id);
                    it != app.skill_tracker->sponsor_progress().end()) {
                  srow.current = it->second.count;
                  srow.tier = std::min(srow.max_tiers, it->second.claimed_tiers + 1);
                }
              }
              const size_t ti = size_t(std::clamp(srow.tier - 1, 0, std::max(0, int(sc.tiers.size()) - 1)));
              if (ti < sc.tiers.size()) {
                srow.target = std::max(1, sc.tiers[ti].number);
                srow.reward_cr = sc.tiers[ti].credits;
              }
              board.sponsors.push_back(std::move(srow));
            }
          }
          board.show_sponsors = career_page.show_sponsors();
          board.sponsor_selected = career_page.sponsor_selected();
          d.career = &board;
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
            !app.rally_service_open && !app.rally_results_open && !club_open)
          list.image(mm_x, mm_y, mm_x + mm_size, mm_y + mm_size, mm_uv);
        if (!campaign_hide_hud) carview::build_drive_hud(list, d, float(width), float(height));
        // Brief white flash on confirming START RACE (CAMERAS.md:85, UI_LAYOUTS.md:121-125).
        if (pre_race_blend_t >= 0.0 && pre_race_blend_t < 0.12) {
          const float fa = float(std::clamp(1.0 - pre_race_blend_t / 0.12, 0.0, 1.0)) * 0.65f;
          list.rect(0.0f, 0.0f, float(width), float(height), carview::ui::rgba(255, 255, 255, uint8_t(fa * 255.0f)));
        }
        if (club_open) {
          const auto club_sc = build_club_screen();
          carview::draw_club_screen(list, width, height, club_sc);
        } else if (!app.show_title && !ceremony.active && !campaign_hide_hud && !app.rally_hub_open &&
                   !app.rally_service_open && !app.rally_results_open && st.menu.empty()) {
          const auto cstrip = build_club_active_strip();
          if (cstrip.active) carview::draw_club_active_strip(list, width, height, cstrip);
        }
        if (club_ceremony.active) {
          club_ceremony.age = app.demo_club ? 1.2 : (vehicle.time() - club_ceremony_at);
          club_ceremony.dock_bottom_right = club_open;
          if (club_ceremony.age > club_ceremony.duration) club_ceremony.active = false;
          else carview::draw_club_medal_ceremony(list, width, height, club_ceremony);
        }
        if (!app.show_title && !ceremony.active && !campaign_hide_hud && radio_popup_gpu.alpha > 0.005f)
          carview::draw_radio_popup_gpu(list, radio_popup_gpu, float(width), float(height));
        if (app.story_intro.stage() == carview::StoryStage::kBootSplash)
          app.story_intro.draw_hud(list, float(width), float(height));
        if (!app.show_title && !ceremony.active) {
          carview::campaign_ui::DrawContext cctx;
          cctx.screen_w = float(width);
          cctx.screen_h = float(height);
          cctx.wristband_points = app.career.wristband_points;
          cctx.collected_wristband = app.collected_wristband;
          cctx.popularity_rank = app.current_pop_rank();
          cctx.skip_vo_with_a = app.skip_vo_with_a;
          cctx.metric_units = app.metric_units;
          cctx.catalog = app.have_career_catalog ? &app.career_catalog : nullptr;
          carview::campaign_ui::draw(list, app.campaign.flow(), cctx);
        }
        if (app.fmv_player.active()) {
          list.clear();
          app.fmv_player.draw(list, float(width), float(height), {}, true, app.campaign.skip_hold_progress());
          carview::campaign_ui::draw_transition(list, app.campaign.flow().transition(), float(width), float(height));
        }
        if (loading_screen.active()) {
          list.clear();
          loading_screen.draw(list, float(width), float(height));
        }
        if (rewind_buf.active()) {
          carview::build_rewind_overlay(list, float(width), float(height), rewind_buf.is_rewinding(),
                                        rewind_buf.ui_state() == drive::RewindUiState::kLoopRewind,
                                        rewind_buf.is_paused_at_keyframe(), rewind_buf.at_oldest_slot(),
                                        rewind_buf.is_settling());
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
                mc.reward_line = carview::thousands(pst->reward_credits) + " Cr  +  " +
                                 carview::thousands(pst->reward_popularity) + " POP";
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
              auto it = festival_guests.find(target_media);
              if (it == festival_guests.end()) {
                auto gs = std::make_unique<Showroom>(host.renderer);
                gs->set_guest();
                std::string ge;
                if (gs->load(app.bake_dir, target_media, ShowroomOptions{}, ge) && gs->upload(ge)) {
                  gs->load_car_lamps(app.game_root_of(), target_media);
                  it = festival_guests.emplace(target_media, std::move(gs)).first;
                } else {
                  it = festival_guests.emplace(target_media, nullptr).first;
                }
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
                                     vehicle.time() < notice_until ? notice : std::string());
          }
        }
        if (barn_card.visible) {
          carview::draw_barn_find_hud(list, float(width), float(height), barn_card);
        }
        const bool show_track_chip = !app.show_title && !app.fmv_player.active() && !barn_card.visible && !ceremony.active && !map_overlay.active && !grid_menu_open &&
                                     st.menu.empty() && !career_page.is_open() && !settings_page.is_open() &&
                                     !shop.is_open() && !autoshow.is_open() && !garage_open &&
                                     !events_page.is_open() && !outpost_hub_open && !paint_open && !club_open &&
                                     !rewind_buf.active();
        achievements.build_hud_overlays(list, float(width), float(height), show_track_chip);
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
