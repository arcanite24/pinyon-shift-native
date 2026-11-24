#include "drive_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "world/road_follow.h"

namespace carview {

void DriveSession::open_replay_viewer(bool browser_if_empty) {
        if (replay_rec.frame_count() >= 2) {
          if (app.race) {
            const auto rh = app.race->hud_info();
            replay_rec.update_summary(rh.place, rh.cars, rh.best_lap, app.tod_clock.tod_sec);
          }
          replay_viewer.open(replay_rec.snapshot(), true);
        } else if (browser_if_empty) {
          replay_viewer.open_browser(replays_dir);
        } else {
          notice = "NO REPLAY RECORDED YET";
          notice_until = vehicle.time() + 2.5;
        }
}

std::vector<PromoCarCandidate> DriveSession::gather_promo_candidates() {
        std::vector<carview::PromoCarCandidate> cands;
        const drive::Vec3 pat = vehicle.position();
        cands.push_back({int(info.car_id), carview::car_name(app.car), {pat.x, pat.y, pat.z}, 2.2, true});
        if (app.race) {
          std::vector<carview::RaceView::OpponentTag> otags;
          app.race->opponent_tags(otags);
          int oid = 10000;
          for (const auto& ot : otags) {
            cands.push_back({oid++, ot.name, {ot.x, ot.y, ot.z}, 2.2, false});
          }
        }
        if (traffic && traffic->cars()) {
          for (const auto& tc : *traffic->cars()) {
            cands.push_back({int(tc.model), "TRAFFIC #" + std::to_string(tc.model),
                             {tc.position.x, tc.position.y, tc.position.z}, 2.2, false});
          }
        }
        return cands;
}

void DriveSession::place_world() {
        if (!host.streamer) return;
        float m[16];
        scenery_transform(info, vehicle, host.showroom.world_origin_x(), host.showroom.world_origin_z(), m);
        host.showroom.set_world_transform(m);
        if (traffic) traffic->place(info, vehicle);
        else host.showroom.set_instances({});
        if (replay_viewer.active() && replay_viewer.current_cars().size() > 1) {
          // Multi-car Replay: player (slot 0) stays on host.showroom; place every recorded
          // opponent/traffic car (slot > 0) onto its own guest Showroom in player-local space.
          host.showroom.set_instances({});
          float pm[16];
          drive::model_to_world(info, vehicle, pm);
          double pmd[16];
          for (int i = 0; i < 16; ++i) pmd[i] = pm[i];
          const double a = pmd[0], b = pmd[4], c = pmd[8], d = pmd[1], e = pmd[5], f = pmd[9], g = pmd[2], h = pmd[6], k = pmd[10];
          const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
          const double id = det != 0.0 ? 1.0 / det : 0.0;
          const double inv[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
                                 (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
                                 (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};
          double world_to_player[16]{};
          for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) world_to_player[col * 4 + r] = inv[r * 3 + col];
          for (int r = 0; r < 3; ++r)
            world_to_player[12 + r] = -(inv[r * 3 + 0] * pmd[12] + inv[r * 3 + 1] * pmd[13] + inv[r * 3 + 2] * pmd[14]);
          world_to_player[15] = 1.0;

          std::vector<Showroom::Instance> rmore;
          const uint32_t focus_slot = replay_viewer.focus_slot();
          const bool hide_focus_bumper = replay_viewer.camera_mode() == ReplayCameraMode::kBumper;
          const bool night_lamps = drive::CarLampState::tod_requests_headlamps(app.tod_clock.tod_sec);
          const auto& all_rcars = replay_viewer.current_cars();
          // Process the focused car first so it gets priority for the 1-per-frame guest upload budget.
          std::vector<size_t> order;
          order.reserve(all_rcars.size());
          for (size_t idx = 0; idx < all_rcars.size(); ++idx) {
            if (all_rcars[idx].slot == focus_slot) order.push_back(idx);
          }
          for (size_t idx = 0; idx < all_rcars.size(); ++idx) {
            if (all_rcars[idx].slot != focus_slot) order.push_back(idx);
          }
          int uploads_this_frame = 0;
          constexpr float kTwoPi = 6.28318530717958647692f;
          std::unordered_set<Showroom*> wheel_posed_guests;
          for (size_t idx : order) {
            const auto& rc = all_rcars[idx];
            // Slot 0 is always rendered by host.showroom (player car).
            if (rc.slot == 0u) continue;
            if (hide_focus_bumper && rc.slot == focus_slot) continue;
            const auto* cinfo = replay_viewer.recording().find_car_info(rc.slot);
            std::string media = (cinfo && !cinfo->car_media.empty()) ? cinfo->car_media : app.car;
            const int64_t paint_seq = cinfo ? int64_t(cinfo->paint_index) : -1;
            Showroom* guest_sr = app.race ? app.race->find_loaded_model(media, paint_seq) : nullptr;
            if (!guest_sr) {
              const std::string gkey = paint_seq >= 0 ? (media + "#" + std::to_string(paint_seq)) : media;
              auto it = festival_guests.find(gkey);
              if (it == festival_guests.end() && paint_seq >= 0) it = festival_guests.find(media);
              if (it == festival_guests.end() && festival_guests.size() < 24 && uploads_this_frame < 1) {
                ++uploads_this_frame;
                auto gs = std::make_unique<Showroom>(host.renderer);
                gs->set_guest();
                if (host.showroom.dynamic_env_view() && host.showroom.dynamic_env_sampler()) {
                  gs->set_shared_dynamic_env(host.showroom.dynamic_env_view(), host.showroom.dynamic_env_sampler());
                }
                ShowroomOptions opt{};
                opt.game_root = app.game_root_of();
                if (paint_seq > 0) opt.match_sequence = paint_seq;
                std::string ge;
                if (gs->load(app.bake_dir, media, opt, ge) && gs->upload(ge)) {
                  gs->load_car_lamps(app.game_root_of(), media);
                  it = festival_guests.emplace(gkey, std::move(gs)).first;
                } else {
                  it = festival_guests.emplace(gkey, nullptr).first;
                }
              }
              if (it != festival_guests.end() && it->second) guest_sr = it->second.get();
            }
            if (!guest_sr) continue;
            // Forward recorded wheel steer, spin, angular speed, and suspension Y to the guest car.
            if (wheel_posed_guests.insert(guest_sr).second) {
              const auto& rest = guest_sr->car().wheel_centres_body;
              bool have_rec_wheels = false;
              for (int wi = 0; wi < 4; ++wi) {
                if (std::fabs(rc.wheel_omega[wi]) > 1e-4f || std::fabs(rc.wheel_spin[wi]) > 1e-4f ||
                    std::fabs(rc.wheel_steer[wi]) > 1e-4f || std::fabs(rc.wheel_y[wi]) > 1e-4f) {
                  have_rec_wheels = true;
                  break;
                }
              }
              const float radius = std::clamp(rest[0][1], 0.26f, 0.55f);
              const float fallback_omega = rc.speed_mps / radius;
              float fallback_spin = std::fmod(float(replay_viewer.time()) * fallback_omega, kTwoPi);
              if (fallback_spin < 0.0f) fallback_spin += kTwoPi;
              WheelPose wposes[4]{};
              for (int wi = 0; wi < 4; ++wi) {
                wposes[wi].centre[0] = rest[size_t(wi)][0];
                wposes[wi].centre[1] = (std::fabs(rc.wheel_y[wi]) > 1e-4f) ? rc.wheel_y[wi] : rest[size_t(wi)][1];
                wposes[wi].centre[2] = rest[size_t(wi)][2];
                wposes[wi].steer = rc.wheel_steer[wi];
                wposes[wi].spin = have_rec_wheels ? rc.wheel_spin[wi] : fallback_spin;
                wposes[wi].omega = have_rec_wheels ? rc.wheel_omega[wi] : fallback_omega;
              }
              guest_sr->set_wheel_poses(wposes);
            }
            const drive::Quat q{double(rc.quat[1]), double(rc.quat[2]), double(rc.quat[3]), double(rc.quat[0])};
            float gm[16];
            drive::model_to_world(guest_sr->car(), q, {0, 0, 0}, gm);
            double gmd[16];
            for (int i = 0; i < 16; ++i) gmd[i] = gm[i];
            gmd[12] += double(rc.pos[0]);
            gmd[13] += double(rc.pos[1]);
            gmd[14] += double(rc.pos[2]);
            Showroom::Instance inst;
            inst.car = guest_sr;
            for (int col = 0; col < 4; ++col)
              for (int row = 0; row < 4; ++row) {
                double sum = 0.0;
                for (int kk = 0; kk < 4; ++kk) sum += world_to_player[kk * 4 + row] * gmd[col * 4 + kk];
                inst.model[col * 4 + row] = float(sum);
              }
            drive::CarLampState ls = (rc.lamp_bits != 0u) ? drive::unpack_replay_lamps(rc.lamp_bits) : drive::CarLampState{};
            if (rc.lamp_bits == 0u) {
              ls.headlamps_on = night_lamps;
              ls.brake_on = rc.brake_u8 > 20;
              ls.reverse_on = rc.gear < 0;
            }
            inst.lamps = ls.vectors();
            inst.has_lamps = true;
            if (cinfo && cinfo->has_body_color) {
              const uint32_t rgb = cinfo->body_color;
              auto to_lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
              inst.paint_rgb[0] = to_lin(float((rgb >> 16) & 0xFFu) / 255.0f);
              inst.paint_rgb[1] = to_lin(float((rgb >> 8) & 0xFFu) / 255.0f);
              inst.paint_rgb[2] = to_lin(float(rgb & 0xFFu) / 255.0f);
              inst.has_paint = true;
            }
            rmore.push_back(inst);
          }
          host.showroom.set_instances(std::move(rmore));
        } else if (app.race) {
          app.race->set_tod_sec(float(app.tod_clock.tod_sec));
          std::vector<Showroom::Instance> more;
          app.race->instances(info, vehicle, more);
          host.showroom.add_instances(more);
        } else if (festival_mgr && !festival_mgr->cars().empty()) {
          // Render roaming Festival AI racers & active H2H opponent as guest Showrooms.
          // Compute world_to_player in double precision so kilometre-scale Colorado coordinates never jitter.
          float pm[16];
          drive::model_to_world(info, vehicle, pm);
          double pmd[16];
          for (int i = 0; i < 16; ++i) pmd[i] = pm[i];
          const double a = pmd[0], b = pmd[4], c = pmd[8], d = pmd[1], e = pmd[5], f = pmd[9], g = pmd[2], h = pmd[6], k = pmd[10];
          const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
          const double id = det != 0.0 ? 1.0 / det : 0.0;
          const double inv[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
                                 (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
                                 (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};
          double world_to_player[16]{};
          for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) world_to_player[col * 4 + r] = inv[r * 3 + col];
          for (int r = 0; r < 3; ++r)
            world_to_player[12 + r] = -(inv[r * 3 + 0] * pmd[12] + inv[r * 3 + 1] * pmd[13] + inv[r * 3 + 2] * pmd[14]);
          world_to_player[15] = 1.0;

          std::vector<Showroom::Instance> fmore;
          const bool night_lamps = drive::CarLampState::tod_requests_headlamps(app.tod_clock.tod_sec);
          const float fdt = float(tod_frame_dt > 0.0 ? std::clamp(tod_frame_dt, 0.0, 0.25) : 1.0 / 60.0);
          static std::map<const Showroom*, float> s_fest_wheel_spin;
          std::map<Showroom*, double> fest_wheel_speed;
          Showroom* fest_fallback = nullptr;
          for (const auto& [kname, gptr] : festival_guests) {
            if (gptr) {
              fest_fallback = gptr.get();
              break;
            }
          }
          for (const auto& fc : festival_mgr->cars()) {
            if (!std::isfinite(fc.position.x) || !std::isfinite(fc.position.y) || !std::isfinite(fc.position.z)) continue;
            auto it = festival_guests.find(fc.spec.car_media);
            if (it == festival_guests.end() && festival_guests.size() < 24) {
              auto gs = std::make_unique<Showroom>(host.renderer);
              gs->set_guest();
              if (host.showroom.dynamic_env_view() && host.showroom.dynamic_env_sampler()) {
                gs->set_shared_dynamic_env(host.showroom.dynamic_env_view(), host.showroom.dynamic_env_sampler());
              }
              ShowroomOptions opt{};
              opt.game_root = app.game_root_of();
              std::string ge;
              if (gs->load(app.bake_dir, fc.spec.car_media, opt, ge) && gs->upload(ge)) {
                gs->load_car_lamps(app.game_root_of(), fc.spec.car_media);
                if (!fest_fallback) fest_fallback = gs.get();
                it = festival_guests.emplace(fc.spec.car_media, std::move(gs)).first;
              } else {
                it = festival_guests.emplace(fc.spec.car_media, nullptr).first;
              }
            }
            Showroom* guest_sr = (it != festival_guests.end() && it->second) ? it->second.get() : fest_fallback;
            if (!guest_sr) continue;
            const drive::Quat yq{0, std::sin(fc.heading * 0.5), 0, std::cos(fc.heading * 0.5)};
            const drive::Quat pq{std::sin(-fc.pitch * 0.5), 0, 0, std::cos(-fc.pitch * 0.5)};
            const drive::Quat q{
                yq.w * pq.x + yq.x * pq.w + yq.y * pq.z - yq.z * pq.y,
                yq.w * pq.y - yq.x * pq.z + yq.y * pq.w + yq.z * pq.x,
                yq.w * pq.z + yq.x * pq.y - yq.y * pq.x + yq.z * pq.w,
                yq.w * pq.w - yq.x * pq.x - yq.y * pq.y - yq.z * pq.z,
            };
            float gm[16];
            drive::model_to_world(guest_sr->car(), q, {0, 0, 0}, gm);
            double gmd[16];
            for (int i = 0; i < 16; ++i) gmd[i] = gm[i];
            gmd[12] += fc.position.x;
            gmd[13] += fc.position.y;
            gmd[14] += fc.position.z;
            Showroom::Instance inst;
            inst.car = guest_sr;
            for (int col = 0; col < 4; ++col)
              for (int row = 0; row < 4; ++row) {
                double sum = 0.0;
                for (int kk = 0; kk < 4; ++kk) sum += world_to_player[kk * 4 + row] * gmd[col * 4 + kk];
                inst.model[col * 4 + row] = float(sum);
              }
            drive::CarLampState ls;
            ls.headlamps_on = night_lamps;
            ls.brake_on = fc.braking;
            inst.lamps = ls.vectors();
            inst.has_lamps = true;
            if (const auto& cols = guest_sr->car().colours; !cols.empty()) {
              const uint32_t rgb = cols[size_t(fc.id) % cols.size()].rgb;
              auto to_lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
              inst.paint_rgb[0] = to_lin(float((rgb >> 16) & 0xFF) / 255.0f);
              inst.paint_rgb[1] = to_lin(float((rgb >> 8) & 0xFF) / 255.0f);
              inst.paint_rgb[2] = to_lin(float(rgb & 0xFF) / 255.0f);
              inst.has_paint = true;
            }
            fmore.push_back(inst);
            fest_wheel_speed[guest_sr] = std::max(fest_wheel_speed[guest_sr], fc.speed);
          }
          constexpr float kTwoPi = 6.28318530717958647692f;
          for (const auto& [guest_sr, spd] : fest_wheel_speed) {
            const auto& rest = guest_sr->car().wheel_centres_body;
            const float radius = std::clamp(rest[0][1], 0.26f, 0.55f);
            const float omega = float(spd) / radius;
            float& spin = s_fest_wheel_spin[guest_sr];
            spin = std::fmod(spin + omega * fdt, kTwoPi);
            if (spin < 0.0f) spin += kTwoPi;
            WheelPose wposes[4]{};
            for (int wi = 0; wi < 4; ++wi) {
              wposes[wi].centre[0] = rest[size_t(wi)][0];
              wposes[wi].centre[1] = rest[size_t(wi)][1];
              wposes[wi].centre[2] = rest[size_t(wi)][2];
              wposes[wi].steer = 0.0f;
              wposes[wi].spin = spin;
              wposes[wi].omega = omega;
            }
            guest_sr->set_wheel_poses(wposes);
          }
          if (!fmore.empty()) host.showroom.add_instances(fmore);
        }
        {
          drive::LampInputs lin{};
          const float bg = brake_glow(vehicle);
          lin.brake_cmd = bg;
          lin.brake_pedal = bg;
          lin.gear = vehicle.gear();
          lin.tod_sec = app.tod_clock.tod_sec;
          lin.dt = float(tod_frame_dt > 0.0 ? tod_frame_dt : 1.0 / 60.0);
          lin.light_override = light_override;
          lin.popup_open = app.headless ? 1.0f : host.showroom.popup_open();
          lin.snap_headlamps = app.headless;
          player_lamps.update(lin);
          host.showroom.set_lamp_state(player_lamps);
          const float road_steer = float(vehicle.wheels()[0].steer);
          const float spd = float(std::max(0.0, vehicle.speed()));
          cockpit_anim.update_steering(road_steer, spd, lin.dt, cockpit_steer_kick_rad);
          const float redline = float(std::max(vehicle.params().redline_rpm, 6000.0));
          const float boost_gauge = drive::CockpitAnimState::gauge_boost_psi(vehicle.boost_psi(), cockpit_has_fi);
          cockpit_anim.update_gauges(spd, cockpit_dial_max_speed, float(vehicle.rpm()), redline, boost_gauge,
                                     cockpit_dial_max_boost, car_damage.fuel_fraction);
          host.showroom.set_cockpit_anim(cockpit_anim, tod_cockpit_cam);
        }
        const float up_game[3] = {0, 1, 0};
        float sun[3], up[3];
        rotate_direction(m, up_game, up);
        if (app.tod_system.loaded_tod) {
          world::TodState tod = app.tod_system.evaluate(app.tod_clock);
          rotate_direction(m, tod.sun_dir.data(), sun);
          host.showroom.set_world_light(sun, up, 1.0f);
          host.showroom.set_shadow_direction(sun);
          const drive::Vec3 vpos = vehicle.position();
          const auto& cfg = app.tod_system.post_config;
          const int zone_idx = app.tod_system.zones.find_zone(float(vpos.x), float(vpos.z));
          const float step_dt = float(tod_frame_dt > 0.0 ? tod_frame_dt : (app.headless ? 10.0 : 1.0 / 60.0));
          zone_blender.update(app.tod_system.zones, cfg, float(vpos.x), float(vpos.z), step_dt);
          // Apply blended PostZone FogTemplate multipliers (Fog_Templates.xml) before uploading ToD UBO.
          {
            const auto& ft = zone_blender.fog;
            for (int k = 0; k < 3; ++k) {
              tod.fog_color[k] *= ft.color[k];
              tod.fog_sun_scatter_color[k] *= ft.sun_scatter_color[k];
            }
            tod.fog_density *= ft.density;
            tod.fog_start *= ft.start_distance;
            tod.fog_sun_scatter_power *= ft.sun_scatter_power;
            tod.fog_height_fade_amount *= ft.height_fade_amount;
            tod.fog_height_fade_start *= ft.height_fade_start;
            tod.fog_height_fade_end *= ft.height_fade_end;
          }
          host.showroom.set_tod(tod, app.tod_clock.moon_phase(), float(vehicle.time()), float(vpos.y));
          if (app.post_on) {
            PostSettings ps = host.renderer.post_settings();
            const float dn = tod.day_night_post;
            for (int k = 0; k < 6; ++k) ps.hable[k] = cfg.hable_day[k] + dn * (cfg.hable_night[k] - cfg.hable_day[k]);
            ps.white = cfg.white_day + dn * (cfg.white_night - cfg.white_day);
            ps.desaturate = std::clamp(cfg.desat_amount * tod.desat_amount_mult, 0.0f, 1.5f);
            for (int k = 0; k < 3; ++k) {
              ps.luminance[k] = cfg.desat_luminosity[k] * tod.desat_luminosity_mult[k];
              ps.balance[k] = cfg.desat_post_mod[k] * tod.desat_post_mod_mult[k];
            }
            if (const char* b = std::getenv("SHIFT_BALANCE"))
              std::sscanf(b, "%f,%f,%f", &ps.balance[0], &ps.balance[1], &ps.balance[2]);
            const auto& bt = zone_blender.bloom;
            const auto& cs = zone_blender.color;
            const auto& vt = zone_blender.vig_top;
            const auto& vb = zone_blender.vig_bottom;
            ps.bloom = (app.bloom_flag >= 0 ? app.bloom_flag == 1 : !app.low_preset) && slice12.trainer.config.debug_bloom;
            ps.bloom_cutoff = std::max(0.05f, bt.cutoff * tod.bloom_cutoff_mult);
            ps.bloom_scale = std::max(0.0f, bt.scale * tod.bloom_scale_mult);
            ps.bloom_persist = std::clamp(bt.persist * tod.bloom_persist_mult, 0.0f, 0.35f);
            ps.bloom_lower_grad = std::max(0.0f, bt.lower_grad * tod.bloom_lower_grad_mult);
            ps.bloom_upper_grad = std::max(0.05f, bt.upper_grad * tod.bloom_upper_grad_mult);
            for (int k = 0; k < 5; ++k) ps.bloom_weights[k] = bt.weights[k];
            ps.vignette_amount = std::clamp(tod.vignette_amount, 0.0f, 1.0f);
            ps.contrast = std::clamp(1.0f + cs.contrast, 0.5f, 1.8f);
            ps.brightness = std::clamp(cs.brightness, -0.35f, 0.35f);
            ps.paired_vignette = !app.tod_system.zones.vignette_templates.empty() && !app.low_preset;
            for (int k = 0; k < 3; ++k) {
              ps.vig_top_color[k] = vt.color[k];
              ps.vig_bot_color[k] = vb.color[k];
              ps.sun_scatter_color[k] = std::clamp(tod.fog_sun_scatter_color[k] * 1.45f + tod.sun_color[k] * 0.45f,
                                                   0.05f, 2.0f);
            }
            constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
            ps.vig_top_opacity = vt.color[3];
            ps.vig_top_center[0] = 0.5f + vt.pos_x * 0.5f;
            ps.vig_top_center[1] = 0.5f - vt.pos_y * 0.5f;
            ps.vig_top_radius[0] = std::max(vt.scale_x, 0.25f);
            ps.vig_top_radius[1] = std::max(vt.scale_y, 0.25f);
            ps.vig_top_rot = vt.angle_deg * kDegToRad;
            ps.vig_top_feather = std::clamp(vt.power, 0.25f, 8.0f);
            ps.vig_bot_opacity = vb.color[3];
            ps.vig_bot_center[0] = 0.5f + vb.pos_x * 0.5f;
            ps.vig_bot_center[1] = 0.5f - vb.pos_y * 0.5f;
            ps.vig_bot_radius[0] = std::max(vb.scale_x, 0.25f);
            ps.vig_bot_radius[1] = std::max(vb.scale_y, 0.25f);
            ps.vig_bot_rot = vb.angle_deg * kDegToRad;
            ps.vig_bot_feather = std::clamp(vb.power, 0.25f, 8.0f);
            // Optical post-FX: speed/camera radial motion blur, DoF, god rays & lens flare/dirt.
            const float mph = float(std::max(0.0, vehicle.speed()) * 2.23693629);
            const float mph_max = std::max(cfg.detail_blur_mph_max, 40.0f);
            const float spd_t = std::clamp((mph - 18.0f) / mph_max, 0.0f, 1.0f);
            ps.motion_blur = live_motion_blur && slice12.trainer.config.debug_motion_blur && (!app.bench || app.bench_full_fx);
            ps.motion_blur_strength = std::clamp(spd_t * std::max(cfg.motion_blur_amount, 0.55f), 0.0f, 0.95f);
            ps.motion_blur_center[0] = 0.5f;
            ps.motion_blur_center[1] = 0.44f;
            ps.motion_blur_car_protect = tod_cockpit_cam ? 2.2f : 6.8f;
            ps.dof = live_dof && slice12.trainer.config.debug_dof && (!app.bench || app.bench_full_fx);
            ps.dof_focal_distance = std::clamp(cfg.dof_focal_distance, 5.0f, 40.0f);
            ps.dof_focal_range = 42.0f;
            ps.dof_max_blur = std::clamp(cfg.dof_focal_max_blur > 0.05f ? cfg.dof_focal_max_blur * 0.55f : 0.35f,
                                         0.15f, 0.65f);
            ps.near_plane = 0.25f;
            ps.god_rays = !app.low_preset && (!app.bench || app.bench_full_fx);
            ps.lens_flare = !app.low_preset && (!app.bench || app.bench_full_fx);
            ps.god_ray_strength = 0.48f;
            ps.lens_flare_strength = 0.36f;
            ps.time_sec = float(vehicle.time());
            ps.lut_enabled = true;
            if (app.exposure_given > 0) {
              ps.auto_exposure = false;
              ps.exposure = float(app.exposure_given);
            } else {
              ps.auto_exposure = true;
              const bool cockpit = tod_cockpit_cam;
              const float* keys = cockpit ? cfg.cockpit_keys : cfg.exterior_keys;
              const float* evs = cockpit ? cfg.cockpit_ev : cfg.exterior_ev;
              for (int k = 0; k < 4; ++k) ps.exp_keys[k] = keys[k], ps.exp_ev[k] = evs[k];
              // Exact TrackSettings.xml filmicTone.exposure EV offset (SKY_AND_TOD §5.2):
              // post.frag evaluates exp2(clamp(ev_curve + filmic_exposure, -2.0, 6.0)).
              ps.exposure = cfg.filmic_exposure;
              const float tau = std::max(cfg.tonemap_delay * 10.0f, 0.05f);
              ps.adapt_alpha = (app.headless || tod_frame_dt <= 0.0)
                                   ? 1.0f
                                   : float(1.0 - std::exp(-tod_frame_dt / double(tau)));
            }
            if (photo_mode.active()) {
              ps = photo_mode.effects().apply_to_post(ps);
            }
            host.renderer.set_post_settings(ps);
            if (!host.user_lut_active) {
              if (!have_post_lut || zone_idx != last_lut_zone ||
                  std::fabs(tod.day_color_grade - last_lut_day) > 0.005f ||
                  std::fabs(tod.night_color_grade - last_lut_night) > 0.005f) {
                last_lut_zone = zone_idx;
                last_lut_day = tod.day_color_grade;
                last_lut_night = tod.night_color_grade;
                target_post_lut = world::ColorLut16::blend(app.tod_system.zones.day_lut_for(zone_idx),
                                                           app.tod_system.zones.night_lut_for(zone_idx),
                                                           tod.day_color_grade, tod.night_color_grade);
                if (!have_post_lut || app.headless || tod_frame_dt <= 0.0) {
                  cur_post_lut = target_post_lut;
                  have_post_lut = true;
                  post_lut_blending = false;
                  host.renderer.set_post_lut(cur_post_lut.rgba);
                } else {
                  post_lut_blending = true;
                }
              }
              if (post_lut_blending && have_post_lut && tod_frame_dt > 0.0) {
                const float alpha = float(std::clamp(1.0 - std::exp(-tod_frame_dt / 1.2), 0.02, 1.0));
                int max_diff = 0;
                for (size_t i = 0; i < cur_post_lut.rgba.size(); ++i) {
                  const int cur_v = cur_post_lut.rgba[i];
                  const int dst_v = target_post_lut.rgba[i];
                  const int diff = std::abs(dst_v - cur_v);
                  if (diff > max_diff) max_diff = diff;
                  if (diff > 0) {
                    const float step = float(dst_v - cur_v) * alpha;
                    int next_v = cur_v + int(std::lround(step));
                    if (next_v == cur_v) next_v = cur_v + (dst_v > cur_v ? 1 : -1);
                    cur_post_lut.rgba[i] = uint8_t(std::clamp(next_v, 0, 255));
                  }
                }
                host.renderer.set_post_lut(cur_post_lut.rgba);
                if (max_diff <= 1) {
                  cur_post_lut = target_post_lut;
                  post_lut_blending = false;
                }
              }
            }
          }
        } else {
          rotate_direction(m, kSunGame, sun);
          host.showroom.set_world_light(sun, up, 1.0f);
          if (host.have_shadow_light) {
            float towards[3];
            rotate_direction(m, host.shadow_light_game, towards);
            host.showroom.set_shadow_direction(towards);
          }
        }
        if (!app.bench || app.bench_full_fx) {
          std::vector<world::SkidVertex> asph_tris, rut_tris;
          host.skid_sys.build_geometry(asph_tris, rut_tris);
          host.showroom.set_skidmarks(asph_tris, rut_tris);
          std::vector<world::DebrisVertex> debris_tris;
          app.smashables.build_debris_mesh(debris_tris);
          if (!debris_tris.empty()) {
            // Ensure every real FH1 smashable debris texture record is immediately resident in
            // Showroom before the first flying debris frame draws. Only remember permanent decode
            // failures so textures evicted by the world LRU during long sessions are cleanly
            // re-uploaded instead of falling back to flat grey untextured vertex colour.
            static std::unordered_set<uint32_t> s_debris_tex_failed;
            std::unordered_set<uint32_t> seen_this_frame;
            std::string derr;
            for (const auto& dv : debris_tris) {
              const uint32_t rec = dv.texture_record;
              if (rec == world::kNoTexture || !seen_this_frame.insert(rec).second) continue;
              if (host.showroom.has_world_texture(rec) || s_debris_tex_failed.count(rec)) continue;
              fh1::WorldTexture wt;
              if (app.smashables.read_texture(rec, wt)) {
                auto td = std::make_shared<WorldTextureData>();
                td->record = rec;
                td->srgb = wt.format.gamma;
                char tname[40];
                std::snprintf(tname, sizeof tname, "debris:_rec_%u", rec);
                td->texture.name = tname;
                td->texture.format = cars::bake::TextureFormat(uint32_t(wt.blocks.format));
                td->texture.swizzle = wt.blocks.swizzle;
                for (auto& l : wt.blocks.levels) td->texture.levels.push_back({l.width, l.height, std::move(l.blocks)});
                (void)host.showroom.ensure_world_texture(td, derr);
              } else {
                s_debris_tex_failed.insert(rec);
              }
            }
          }
          std::vector<carview::Showroom::ShowcaseVertex> showcase_tris;
          if (app.race && app.race->is_showcase()) {
            std::string sc_err;
            host.showroom.load_showcase_texture(app.game_root_of(), int(app.race->showcase().spec().kind), sc_err);
            app.race->build_showcase_mesh(showcase_tris);
          }
          host.showroom.set_showcase_mesh(showcase_tris);
          const drive::Vec3 vpos = vehicle.position();
          app.world_anim.build_mesh(vpos.x, vpos.y + 1.5, vpos.z, float(vehicle.time()), host.ground, debris_tris, !app.low_preset);
          host.showroom.set_debris_mesh(debris_tris);
          std::vector<world::ParticleSprite> p_alpha, p_add;
          host.particle_sys.build_sprites({float(vpos.x), float(vpos.y + 1.5), float(vpos.z)}, p_alpha, p_add);
          host.showroom.set_particles(p_alpha, p_add, host.particle_sys.used_textures(), 0.25f, tod_cockpit_cam);
        }
}

audio::View DriveSession::audio_view() {
        return (have_rig && !app.front) ? carview::engine_audio_view(rig.mode())
                                    : (app.front ? audio::View::kFront : audio::View::kFollow);
}

void DriveSession::prune_world_rewind() {
        const auto keep = rewind_buf.active_snapshot_ids();
        for (auto it = world_rewind_snaps.begin(); it != world_rewind_snaps.end();) {
          if (std::find(keep.begin(), keep.end(), it->first) == keep.end()) it = world_rewind_snaps.erase(it);
          else ++it;
        }
}

void DriveSession::capture_world_rewind(uint64_t snap_id) {
        if (snap_id == 0u) return;
        WorldRewindSnapshot ws;
        if (app.race) {
          ws.has_race = true;
          ws.race = app.race->capture_rewind();
        }
        if (traffic) {
          ws.has_traffic = true;
          ws.traffic = traffic->capture_rewind();
        }
        if (festival_mgr) {
          ws.has_festival = true;
          ws.festival = festival_mgr->capture_rewind();
        }
        if (app.skill_tracker) {
          ws.has_skills = true;
          ws.skills = app.skill_tracker->capture_rewind();
        }
        ws.smashables = app.smashables.capture_rewind();
        ws.car_damage = car_damage;
        world_rewind_snaps[snap_id] = std::move(ws);
        prune_world_rewind();
}

void DriveSession::apply_world_rewind_scrub() {
        drive::RewindBuffer::RewindBracket br{};
        if (!rewind_buf.sampled_snapshot_bracket(br)) return;
        const auto it_a = world_rewind_snaps.find(br.snap_a);
        const auto it_b = world_rewind_snaps.find(br.snap_b);
        if (it_a == world_rewind_snaps.end() && it_b == world_rewind_snaps.end()) return;
        const WorldRewindSnapshot& ws_a = (it_a != world_rewind_snaps.end()) ? it_a->second : it_b->second;
        const WorldRewindSnapshot& ws_b = (it_b != world_rewind_snaps.end()) ? it_b->second : it_a->second;
        const double alpha = (it_a != world_rewind_snaps.end() && it_b != world_rewind_snaps.end()) ? br.alpha : 0.0;

        auto lane_quat = [](double heading, double pitch) {
          const drive::Quat yq{0, std::sin(heading * 0.5), 0, std::cos(heading * 0.5)};
          const drive::Quat pq{std::sin(-pitch * 0.5), 0, 0, std::cos(-pitch * 0.5)};
          return drive::Quat{
              yq.w * pq.x + yq.x * pq.w + yq.y * pq.z - yq.z * pq.y,
              yq.w * pq.y - yq.x * pq.z + yq.y * pq.w + yq.z * pq.x,
              yq.w * pq.z + yq.x * pq.y - yq.y * pq.x + yq.z * pq.w,
              yq.w * pq.w - yq.x * pq.x - yq.y * pq.y - yq.z * pq.z,
          };
        };
        auto lerp_angle = [](double a0, double a1, double t) {
          double d = a1 - a0;
          constexpr double kPi = 3.14159265358979323846;
          while (d > kPi) d -= 2.0 * kPi;
          while (d < -kPi) d += 2.0 * kPi;
          return a0 + d * t;
        };

        // 1. Smoothly scrub AI race rivals between adjacent keyframes.
        if (app.race && ws_a.has_race && ws_b.has_race) {
          RaceView::RewindSnapshot blended = ws_b.race;
          const size_t n = std::min(ws_a.race.cars.size(), blended.cars.size());
          for (size_t i = 0; i < n; ++i) {
            blended.cars[i].before = ws_a.race.cars[i].body.pose();
          }
          app.race->restore_rewind(blended, false);
          app.race->set_blend(alpha);
        }

        // 2. Smoothly scrub open-world & street-race traffic between adjacent keyframes.
        if (traffic && ws_a.has_traffic && ws_b.has_traffic) {
          TrafficView::RewindSnapshot blended = ws_b.traffic;
          blended.before.clear();
          std::unordered_map<uint32_t, TrafficPose> poses_a;
          poses_a.reserve(ws_a.traffic.population.cars.size());
          for (const auto& c : ws_a.traffic.population.cars) {
            const auto bit = ws_a.traffic.bodies.bodies.find(c.id);
            if (bit != ws_a.traffic.bodies.bodies.end()) {
              poses_a.emplace(c.id, TrafficPose{bit->second.vehicle.position(), bit->second.vehicle.orientation()});
            } else {
              poses_a.emplace(c.id, TrafficPose{{c.position.x, c.position.y, c.position.z}, lane_quat(c.heading, c.pitch)});
            }
          }
          for (const auto& c : blended.population.cars) {
            if (const auto ait = poses_a.find(c.id); ait != poses_a.end()) {
              blended.before.emplace(c.id, ait->second);
            }
          }
          traffic->restore_rewind(blended);
          traffic->set_blend(alpha);
        }

        // 3. Smoothly scrub roaming Festival AI & H2H rivals between adjacent keyframes.
        if (festival_mgr && ws_a.has_festival && ws_b.has_festival) {
          freeroam::FestivalManager::RewindSnapshot blended = ws_a.festival;
          std::unordered_map<uint32_t, const freeroam::FestivalAiCar*> map_b;
          map_b.reserve(ws_b.festival.cars.size());
          for (const auto& c : ws_b.festival.cars) map_b.emplace(c.id, &c);
          for (auto& c : blended.cars) {
            if (const auto bit = map_b.find(c.id); bit != map_b.end()) {
              const auto& cb = *bit->second;
              c.position.x += (cb.position.x - c.position.x) * alpha;
              c.position.y += (cb.position.y - c.position.y) * alpha;
              c.position.z += (cb.position.z - c.position.z) * alpha;
              c.heading = lerp_angle(c.heading, cb.heading, alpha);
              c.pitch = c.pitch + (cb.pitch - c.pitch) * alpha;
              c.speed = c.speed + (cb.speed - c.speed) * alpha;
            }
          }
          festival_mgr->restore_rewind(blended);
        }

        // 4. Restore smashable prop states as rewind scrubs past impacts (props pop back intact!).
        const uint64_t smash_snap = (alpha < 0.5) ? br.snap_a : br.snap_b;
        if (smash_snap != 0u && smash_snap != last_rewind_smash_snap) {
          last_rewind_smash_snap = smash_snap;
          const auto& ws_s = (alpha < 0.5) ? ws_a : ws_b;
          const auto unhidden = app.smashables.restore_rewind(ws_s.smashables);
          for (const auto& hs : unhidden) host.showroom.unhide_world_props_near(hs.pos, hs.radius);
          if (host.streamer) host.streamer->set_hidden_positions(app.smashables.broken_hidden_positions());
        }
}

void DriveSession::restore_world_rewind(uint64_t snap_id, bool is_resume) {
        last_rewind_smash_snap = 0u;
        auto it = world_rewind_snaps.find(snap_id);
        if (it != world_rewind_snaps.end()) {
          const auto& ws = it->second;
          if (app.race && ws.has_race) {
            app.race->restore_rewind(ws.race, is_resume && rewind_buf.lap_uncertified());
            app.audio_host.clear_codriver();
          }
          if (traffic && ws.has_traffic) traffic->restore_rewind(ws.traffic);
          if (festival_mgr && ws.has_festival) festival_mgr->restore_rewind(ws.festival);
          if (app.skill_tracker && ws.has_skills) app.skill_tracker->restore_rewind(ws.skills);
          const auto unhidden = app.smashables.restore_rewind(ws.smashables);
          for (const auto& hs : unhidden) host.showroom.unhide_world_props_near(hs.pos, hs.radius);
          if (host.streamer) host.streamer->set_hidden_positions(app.smashables.broken_hidden_positions());
          car_damage = ws.car_damage;
          const drive::DamageMultipliers dm = car_damage.multipliers();
          drive::Vehicle::DamageMods vdm;
          vdm.engine_torque = dm.engine_torque;
          vdm.shift_time_add = dm.shift_time_add;
          vdm.brake_power = dm.brake_power;
          vdm.aero_drag = dm.aero_drag;
          vdm.aero_downforce = dm.aero_downforce;
          vdm.tyre_grip = dm.tyre_grip;
          vdm.suspension_scale = dm.suspension_scale;
          vdm.steer_pull_rad = dm.steer_pull_rad;
          vehicle.set_damage_mods(vdm);
          host.showroom.set_damage_state(car_damage);
        } else {
          app.smashables.clear_debris();
        }
        prune_world_rewind();
}

void prime_world_and_snap_vehicle(DriveSession& s, std::array<double, 3> target_pos, double target_heading) {
  double px = target_pos[0];
  double py = target_pos[1];
  double pz = target_pos[2];
  double ph = target_heading;

  if (s.host.streamer) {
    auto sync_prime_at = [&](double cx, double cz) {
      bool any_changed = false;
      for (int wait_i = 0; wait_i < 2000 && s.host.streamer->gathering(); ++wait_i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        bool ch = false;
        s.host.streamer->update(cx, cz, &ch, &s.app.error);
        any_changed = any_changed || ch;
      }
      s.host.streamer->set_async(false);
      const double dist_centre = std::hypot(cx - s.host.streamer->centre_x(), cz - s.host.streamer->centre_z());
      if (s.host.streamer->gathers() == 0 || dist_centre > 100.0) {
        s.host.streamer->set_hidden(s.host.hidden_sent);
        bool ch = false;
        s.host.streamer->update(cx, cz, &ch, &s.app.error);
        any_changed = any_changed || ch;
      }
      s.host.streamer->set_async(s.host.streamer_async);
      if (any_changed) {
        s.host.ground = &s.host.streamer->ground();
        s.host.follower.set_ground(&s.host.streamer->ground());
        s.host.set_staged_world();
      }
    };

    auto raycast_ground = [&](double rx, double ry_hint, double rz, double& out_y, drive::Hit& out_hit) -> bool {
      const double y_from = ry_hint + 25.0;
      if (s.host.streamer->ground().raycast({rx, y_from, rz}, {0, -1, 0}, 60.0, out_hit)) {
        out_y = y_from - out_hit.distance;
        return true;
      }
      if (s.host.streamer->ground().raycast({rx, 2000.0, rz}, {0, -1, 0}, 4000.0, out_hit)) {
        out_y = 2000.0 - out_hit.distance;
        return true;
      }
      return false;
    };

    auto is_drivable_road = [&](const drive::Hit& hit) -> bool {
      if (hit.normal.y < 0.7) return false;
      const auto& surfs = s.host.collision.surfaces();
      if (hit.surface >= 0 && size_t(hit.surface) < surfs.size()) {
        return surfs[size_t(hit.surface)].off_roadness < 0.5f;
      }
      return hit.surface <= 1;
    };

    sync_prime_at(px, pz);

    double hit_y = py;
    drive::Hit hit{};
    const bool direct_hit = raycast_ground(px, py, pz, hit_y, hit);
    if (direct_hit && is_drivable_road(hit)) {
      py = hit_y + 0.03;
    } else {
      world::Spawn sp;
      if (world::find_spawn(s.host.streamer->mesh(), px, pz, sp, &ph)) {
        px = sp.position[0];
        pz = sp.position[2];
        ph = sp.heading;
        if (std::hypot(px - s.host.streamer->centre_x(), pz - s.host.streamer->centre_z()) > 100.0) {
          sync_prime_at(px, pz);
        }
        double snap_y = sp.position[1];
        drive::Hit snap_hit{};
        if (raycast_ground(px, sp.position[1], pz, snap_y, snap_hit)) {
          py = snap_y + 0.03;
        } else {
          py = sp.position[1] + 0.03;
        }
        const drive::Vec3 p{px, py, pz};
        if (world::wall_between(s.host.streamer->ground(), p, p.x + 10.0 * std::sin(ph),
                                p.z + 10.0 * std::cos(ph))) {
          ph = std::remainder(ph + 3.14159265358979323846, 2.0 * 3.14159265358979323846);
        }
      } else if (direct_hit) {
        py = hit_y + 0.03;
      }
    }
  }

  s.vehicle.reset(drive::Vec3{px, py, pz}, ph);
  s.host.start = {px, py, pz};
  s.host.start_heading = ph;
  s.have_touch = true;
  s.last_touch_y = s.vehicle.position().y;
  s.camera_pull = CameraPull{};
  if (s.have_rig) s.rig.restart();
  s.rig_accel.reset();
  s.drawn_before = s.vehicle.pose();
  s.rewind_buf.reset_history(s.vehicle.time());
  s.world_rewind_snaps.clear();
  s.place_world();
  std::printf("campaign teleport: primed world at (%.2f, %.2f, %.2f) heading %.4f rad\n", px, py, pz, ph);
}

}  // namespace carview
