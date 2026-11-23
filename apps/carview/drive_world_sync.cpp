#include "drive_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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
        if (app.race) {
          app.race->set_tod_sec(float(app.tod_clock.tod_sec));
          std::vector<Showroom::Instance> more;
          app.race->instances(info, vehicle, more);
          host.showroom.add_instances(more);
        } else if (festival_mgr && !festival_mgr->cars().empty()) {
          // Render roaming Festival AI racers & active H2H opponent as guest Showrooms.
          float pm[16];
          drive::model_to_world(info, vehicle, pm);
          float world_to_player[16];
          invert_affine(pm, world_to_player);
          std::vector<Showroom::Instance> fmore;
          const bool night_lamps = drive::CarLampState::tod_requests_headlamps(app.tod_clock.tod_sec);
          for (const auto& fc : festival_mgr->cars()) {
            auto it = festival_guests.find(fc.spec.car_media);
            if (it == festival_guests.end() && festival_guests.size() < 6) {
              auto gs = std::make_unique<Showroom>(host.renderer);
              gs->set_guest();
              std::string ge;
              if (gs->load(app.bake_dir, fc.spec.car_media, ShowroomOptions{}, ge) && gs->upload(ge)) {
                gs->load_car_lamps(app.game_root_of(), fc.spec.car_media);
                it = festival_guests.emplace(fc.spec.car_media, std::move(gs)).first;
              } else {
                it = festival_guests.emplace(fc.spec.car_media, nullptr).first;
              }
            }
            if (it == festival_guests.end() || !it->second) continue;
            const drive::Quat yq{0, std::sin(fc.heading * 0.5), 0, std::cos(fc.heading * 0.5)};
            const drive::Quat pq{std::sin(-fc.pitch * 0.5), 0, 0, std::cos(-fc.pitch * 0.5)};
            const drive::Quat q{
                yq.w * pq.x + yq.x * pq.w + yq.y * pq.z - yq.z * pq.y,
                yq.w * pq.y - yq.x * pq.z + yq.y * pq.w + yq.z * pq.x,
                yq.w * pq.z + yq.x * pq.y - yq.y * pq.x + yq.z * pq.w,
                yq.w * pq.w - yq.x * pq.x - yq.y * pq.y - yq.z * pq.z,
            };
            float gm[16];
            drive::model_to_world(it->second->car(), q, {fc.position.x, fc.position.y, fc.position.z}, gm);
            Showroom::Instance inst;
            inst.car = it->second.get();
            for (int col = 0; col < 4; ++col)
              for (int row = 0; row < 4; ++row) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += world_to_player[k * 4 + row] * gm[col * 4 + k];
                inst.model[col * 4 + row] = sum;
              }
            drive::CarLampState ls;
            ls.headlamps_on = night_lamps;
            ls.brake_on = fc.braking;
            inst.lamps = ls.vectors();
            inst.has_lamps = true;
            fmore.push_back(inst);
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
          lin.snap_headlamps = app.headless;
          player_lamps.update(lin);
          host.showroom.set_lamp_state(player_lamps);
          const float road_steer = float(vehicle.wheels()[0].steer);
          const float spd = float(std::max(0.0, vehicle.speed()));
          cockpit_anim.update_steering(road_steer, spd, lin.dt, cockpit_steer_kick_rad);
          const float redline = float(std::max(vehicle.params().redline_rpm, 6000.0));
          const float boost_gauge = drive::CockpitAnimState::gauge_boost_psi(vehicle.boost_psi(), cockpit_has_fi);
          cockpit_anim.update_gauges(spd, cockpit_dial_max_speed, float(vehicle.rpm()), redline, boost_gauge,
                                     cockpit_dial_max_boost, 0.85f);
          host.showroom.set_cockpit_anim(cockpit_anim, tod_cockpit_cam);
        }
        const float up_game[3] = {0, 1, 0};
        float sun[3], up[3];
        rotate_direction(m, up_game, up);
        if (app.tod_system.loaded_tod) {
          const world::TodState tod = app.tod_system.evaluate(app.tod_clock);
          rotate_direction(m, tod.sun_dir.data(), sun);
          host.showroom.set_world_light(sun, up, 1.0f);
          host.showroom.set_shadow_direction(sun);
          const drive::Vec3 vpos = vehicle.position();
          host.showroom.set_tod(tod, app.tod_clock.moon_phase(), float(vehicle.time()), float(vpos.y));
          if (app.post_on) {
            const auto& cfg = app.tod_system.post_config;
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
            const int zone_idx = app.tod_system.zones.find_zone(float(vpos.x), float(vpos.z));
            const float step_dt = float(tod_frame_dt > 0.0 ? tod_frame_dt : (app.headless ? 10.0 : 1.0 / 60.0));
            zone_blender.update(app.tod_system.zones, cfg, float(vpos.x), float(vpos.z), step_dt);
            const auto& bt = zone_blender.bloom;
            const auto& cs = zone_blender.color;
            const auto& vt = zone_blender.vig_top;
            const auto& vb = zone_blender.vig_bottom;
            ps.bloom = (app.bloom_flag >= 0 ? app.bloom_flag == 1 : !app.low_preset) && slice12.trainer.config.debug_bloom;
            ps.bloom_cutoff = std::max(0.05f, bt.cutoff * tod.bloom_cutoff_mult);
            ps.bloom_scale = std::max(0.0f, bt.scale * tod.bloom_scale_mult);
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
            if (!host.user_lut_active &&
                (zone_idx != last_lut_zone || std::fabs(tod.day_color_grade - last_lut_day) > 0.005f ||
                 std::fabs(tod.night_color_grade - last_lut_night) > 0.005f)) {
              last_lut_zone = zone_idx;
              last_lut_day = tod.day_color_grade;
              last_lut_night = tod.night_color_grade;
              const auto lut = world::ColorLut16::blend(app.tod_system.zones.day_lut_for(zone_idx),
                                                        app.tod_system.zones.night_lut_for(zone_idx),
                                                        tod.day_color_grade, tod.night_color_grade);
              host.renderer.set_post_lut(lut.rgba);
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
        world_rewind_snaps[snap_id] = std::move(ws);
        prune_world_rewind();
}

void DriveSession::restore_world_rewind(uint64_t snap_id, bool is_resume) {
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
        } else {
          app.smashables.clear_debris();
        }
        prune_world_rewind();
}

}  // namespace carview
