#include "drive_world_host.h"

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "cars/visual_setup.h"
#include "display_hdr_win.h"
#include "drive_session.h"
#include "fh1data/cjk_font.h"
#include "settings.h"
#include "world_view.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#endif

namespace carview {
namespace {

inline void lower_stream_worker_priority() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
  SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
#endif
}

}  // namespace

ShowroomOptions DriveWorldHost::build_car_options() {
      ShowroomOptions opts;
      opts.game_root = app.game_root_of();
      // The garage car's paint & fitted visual upgrades (body kits, wing, rims, brakes, stance).
      if (app.have_garage)
        if (const garage::Car* gc = app.career_garage.selected_car())
          if (auto it = app.baked_ids.find(gc->car_id); it != app.baked_ids.end() && lower_ascii(it->second) == lower_ascii(app.car)) {
            opts.match_sequence = gc->colour;
            opts.car_id = gc->car_id;
            if (app.parts_db.valid()) {
              if (const auto db = app.parts_db.get()) {
                const float rh_f = float(gc->tune.front_ride_height);
                const float rh_r = float(gc->tune.rear_ride_height);
                const float cam_f = float(gc->tune.front_camber_deg);
                const float cam_r = float(gc->tune.rear_camber_deg);
                cars::resolve_visual_setup(*db, gc->car_id, garage::fitted(*gc), opts.visual, nullptr, rh_f, rh_r,
                                           cam_f, cam_r);
              }
            }
          }
      // A colour picked in the pause menu's paint page for this car.
      if (app.paint_sequence >= 0 && lower_ascii(app.paint_car) == lower_ascii(app.car)) opts.match_sequence = app.paint_sequence;
      return opts;
}

bool DriveWorldHost::set_staged_world() {
        auto prepared = std::any_cast<std::shared_ptr<Prepared>>(std::move(streamer->prepared()));
        streamer->prepared().reset();
        if (!prepared) {
          app.error = "world staging buffers";
          return false;
        }
        follower.set_points(std::move(prepared->road));
        showroom.set_world_glows(std::move(prepared->glows));
        // If an async GPU world upload is still in flight (or deferred textures for this ring
        // are still being drip-fed into the GPU at <= 1.8 ms/frame in windowed mode), never stall
        // on drop_pending_world -> vkWaitForFences. Queue the newest prepared world and hand it
        // over once the prior copy and deferred texture uploads finish.
        const bool wait_deferred_tex = !app.bench && showroom.deferred_upload_queue_size() > 0;
        if (streamer_async && streamer->gathers() > 1 && (showroom.has_pending_world() || wait_deferred_tex)) {
          deferred_prepared_ = std::move(prepared);
          return true;
        }
        deferred_prepared_.reset();
        auto staged = std::move(prepared->staged);
        if (!staged) {
          app.error = "world staging buffers";
          return false;
        }
        // The first gather and synchronous streams swap at once; later
        // background ones keep the old scenery drawn until the GPU copy is
        // done (poll_world, each step).
        return streamer_async && streamer->gathers() > 1 ? showroom.begin_world(std::move(*staged), app.error)
                                                         : showroom.set_world(std::move(*staged), app.error);
}

bool DriveWorldHost::pump_deferred_world() {
  if (!deferred_prepared_ || showroom.has_pending_world() || showroom.deferred_upload_queue_size() > 0) return true;
  auto prepared = std::move(deferred_prepared_);
  if (!prepared || !prepared->staged) return true;
  return showroom.begin_world(std::move(*prepared->staged), app.error);
}

bool DriveWorldHost::open_world_and_car() {
    // SHIFT_NO_CULL=1: every world draw, for comparison with the culled frame.
    if (const char* e = std::getenv("SHIFT_NO_CULL"); e && *e == '1') showroom.set_world_culling(false);
    ShowroomOptions options = build_car_options();
    Renderer::Options render_options{app.window, app.validation, app.msaa};
    render_options.requested_backend = shift::rhi::parse_backend_kind(app.graphics_backend);
    render_options.reverse_z = app.reverse_z;
    render_options.backface_cull = app.backface_cull;
    render_options.z_prepass = app.z_prepass;
    app.cli_graphics = carview::modern::clamp_graphics(app.cli_graphics);
    active_graphics = app.cli_graphics;
    render_options.graphics = app.cli_graphics;
    render_options.vsync = app.cli_graphics.vsync;
    if (const char* pc_env = std::getenv("SHIFT_PIPELINE_CACHE"); pc_env && *pc_env) {
      render_options.pipeline_cache_path = pc_env;
    } else if (!app.play_dir.empty()) {
      render_options.pipeline_cache_path = app.play_dir / "cache" / "vk_pipeline_cache.bin";
    } else if (!app.cache_path.empty() && app.cache_path.has_parent_path()) {
      render_options.pipeline_cache_path = app.cache_path.parent_path() / "vk_pipeline_cache.bin";
    } else if (!app.bake_dir.empty() && app.bake_dir.has_parent_path()) {
      render_options.pipeline_cache_path = app.bake_dir.parent_path() / "cache" / "vk_pipeline_cache.bin";
    }
    showroom.set_texture_cap(app.cli_graphics.texture_cap);
    showroom.set_reflection_rate(app.preset_reflection_rate);
    showroom.set_low_memory_mode(app.low_preset);
    render_options.ui_atlas_size = carview::ui::atlas_size();
    if (app.cli_graphics.auto_hdr && !app.headless) {
      const auto st_dir = carview::settings_path().parent_path();
      if (!st_dir.empty()) carview::set_display_hdr_restore_file(st_dir / "hdr_restore.bin");
      carview::turn_on_display_hdr();
    }
    // Anisotropic filtering of the ground's grazing textures (honors preset.anisotropy).
    render_options.anisotropy = app.preset_anisotropy > 0.0f ? app.preset_anisotropy : (app.low_preset ? 2.0f : 16.0f);
    if (app.post_on) {
      PostSettings& post = render_options.post;
      post.enabled = true;
      if (!app.world_dir.empty()) read_post_settings(app.world_dir, post);
      post.exposure = float(app.exposure_given > 0 ? app.exposure_given : kPostExposure);
      post.bloom = app.bloom_flag >= 0 ? app.bloom_flag == 1 : !app.low_preset;
      post.balance[0] = kPostBalance[0], post.balance[1] = kPostBalance[1], post.balance[2] = kPostBalance[2];
      if (const char* b = std::getenv("SHIFT_BALANCE"))  // tuning: r,g,b
        std::sscanf(b, "%f,%f,%f", &post.balance[0], &post.balance[1], &post.balance[2]);
    }
    {
      const std::string level = !app.shadows_flag.empty() ? app.shadows_flag : app.low_preset ? "low" : "high";
      ShadowSettings& shadow = render_options.shadow;
      if (level == "low") {
        shadow.size = 1024, shadow.radius = 40, shadow.taps = 1, shadow.cascades = 2;
      } else if (level == "high") {
        shadow.size = 2048, shadow.radius = 60, shadow.taps = 4;
        shadow.cascades = uint32_t(carview::modern::shadow_cascade_count(app.cli_graphics.shadow_pcss));
      }
    }
    // The shadow pass casts from the track's shadow light when it overrides
    // the sun (towards the light: the travel direction negated).
    
    
    if (!app.world_dir.empty() && read_shadow_direction(app.world_dir, shadow_light_game)) {
      have_shadow_light = true;
      for (float& c : shadow_light_game) c = -c;
    }
    if (const auto groot = app.game_root_of(); !groot.empty()) {
      const auto xpr = groot / "media" / "tracks" / "uiautoshow" / "staticcarcubemap.xpr";
      if (std::filesystem::exists(xpr)) showroom.set_environment_xpr(xpr);
    }
    const auto car_load_t0 = std::chrono::steady_clock::now();
    const bool car_loaded_ok = showroom.load(app.bake_dir, app.car, options, app.error);
    const auto vk_init_t0 = std::chrono::steady_clock::now();
    const bool vk_init_ok = car_loaded_ok && renderer.init(render_options, app.error);
    const auto car_up_t0 = std::chrono::steady_clock::now();
    const bool car_up_ok = vk_init_ok && showroom.upload(app.error);
    const auto car_up_t1 = std::chrono::steady_clock::now();
    if (car_up_ok) {
      const double load_ms = std::chrono::duration<double, std::milli>(vk_init_t0 - car_load_t0).count();
      const double vk_ms = std::chrono::duration<double, std::milli>(car_up_t0 - vk_init_t0).count();
      const double up_ms = std::chrono::duration<double, std::milli>(car_up_t1 - car_up_t0).count();
      app.bench_startup.vulkan_init_ms = std::max(0.0, vk_ms - renderer.pipeline_compile_ms());
      app.bench_startup.car_bake_load_ms = load_ms + std::max(0.0, up_ms - showroom.pipeline_compile_ms());
    }
    if (!car_up_ok) {
      std::fprintf(stderr, "carview: %s\n", app.error.c_str());
      app.status = 1;
      return false;
    }
    if (app.status != 0) return false;
      renderer.set_hdr_pattern(app.cli_hdr_pattern, app.cli_hdr_pattern_nits);
      if (app.window) {
        if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(app.window))) {
          if (dm->refresh_rate > 10.0f) display_hz = int(std::lround(dm->refresh_rate));
        }
        frame_pacer.set_interval(carview::modern::fps_cap_interval(active_graphics.fps_cap, display_hz));
      }
      if (!active_graphics.lut.empty()) {
        std::string lut_err;
        if (apply_user_cube_lut(active_graphics.lut, carview::settings_path().parent_path(), renderer, lut_err)) {
          user_lut_active = true;
        } else {
          std::fprintf(stderr, "carview: LUT note: %s\n", lut_err.c_str());
        }
      }
      if (app.tod_system.loaded_tod) {
        std::string sky_err;
        if (!showroom.upload_sky(app.tod_system.sky_assets, sky_err))
          std::fprintf(stderr, "carview: sky upload note: %s\n", sky_err.c_str());
      }
      {
        const auto groot = app.game_root_of();
        if (!groot.empty()) {
          showroom.load_car_lamps(groot, app.car);
          std::string cl_err;
          if (!showroom.upload_carlights(groot / "media", cl_err))
            std::fprintf(stderr, "carview: carlights upload note: %s\n", cl_err.c_str());
        }
        std::string vfx_gpu_err;
        if (!showroom.init_vfx(&app.vfx_lib, vfx_gpu_err))
          std::fprintf(stderr, "carview: vfx gpu init note: %s\n", vfx_gpu_err.c_str());
      }
      // Persist the VkPipelineCache immediately after startup AOT pre-compilation
      // (not only on clean exit) so crashes or forced closes still retain warm caches.
      renderer.save_pipeline_cache();
      // Pre-warm offscreen/RT render targets before Frame 0 to eliminate first-frame stalls.
      if (app.headless && app.width > 0 && app.height > 0) {
        std::string tw_err;
        if (!renderer.prewarm_targets(app.width, app.height, tw_err))
          std::fprintf(stderr, "carview: target prewarm note: %s\n", tw_err.c_str());
      }
      if (renderer.active_rt_mode() != rt::RtMode::kOff && renderer.render_width() > 0 && renderer.render_height() > 0) {
        showroom.prewarm_rt_targets(renderer.render_width(), renderer.render_height());
      }
  particle_sys.set_library(&app.vfx_lib);
      // SHIFT_PAINT_PREVIEW=i (debug): the paint page's live preview of
      // colour row i, as left and right show it.
      if (const char* pp = std::getenv("SHIFT_PAINT_PREVIEW")) showroom.preview_colour(size_t(std::atoi(pp)));
      if (!app.world_dir.empty()) {
        const auto world_load_t0 = std::chrono::steady_clock::now();
        if (app.cache_path.empty()) app.cache_path = std::filesystem::path("out") / "world" / (world::model_prefix(app.world_dir).substr(0, world::model_prefix(app.world_dir).size() - 3) + ".index");
        world::Spawn sp;
        std::string warning;
        const char* how = "";
        std::string col_err;
        std::future<bool> fut_col;
        if (!app.render_ground) {
          const auto phys_zip =
              (app.game_root_given.empty() ? app.world_dir.parent_path().parent_path() : app.game_root_of() / "media") / "physics.zip";
          fut_col = std::async(std::launch::async, [&, phys_zip]() {
            return collision.open(app.world_dir, phys_zip, &col_err, &warning);
          });
        }
        const bool world_ok = colorado.open(app.world_dir, app.cache_path, &app.error);
        const bool col_ok = app.render_ground ? true : fut_col.get();
        if (!world_ok) {
          app.status = 1;
        } else if (!col_ok) {
          app.error = col_err;
          app.status = 1;
        } else {
          if (!app.error.empty()) std::fprintf(stderr, "carview: note: %s\n", app.error.c_str());
          if (!warning.empty()) std::fprintf(stderr, "carview: note: %s", warning.c_str());
          app.error.clear();
          // SHIFT_SURFACES=1: the surface table (the numbers in traces and logs).
          if (std::getenv("SHIFT_SURFACES"))
            for (size_t i = 0; i < collision.surfaces().size(); ++i)
              std::printf("surface %zu %s grip %.2f\n", i, collision.surfaces()[i].name.c_str(),
                          collision.grip(int(i)));
          const int active_route = app.gather_route_given ? app.gather_route : (app.race ? app.race->event().route_id : 0);
          if (!app.render_ground) collision.set_active_route(active_route);
          world::GatherOptions gather_options;
          gather_options.render_data = !app.flat_world;
          gather_options.props = !app.no_props;
          gather_options.route = active_route;
          if (!app.flat_world && !app.no_far) gather_options.far_radius = 3000;
          if (const char* fi = std::getenv("SHIFT_FAR_INNER")) gather_options.far_inner = std::atof(fi);
          if (!app.flat_world && !app.no_trees) gather_options.tree_radius = kTreeRadius;
          if (!app.flat_world) {
            // Popping (ours): the gather reaches further and the props
            // cross-fade by distance from the eye. The eye can be a regather
            // step (plus the swap's lag) from the gather's centre, so the
            // props fade out by kPropRadius - step - lag, where every gather
            // still holds them, and LOD00 is gathered to the LOD band's end
            // plus that much.
            const double prop_radius = app.low_preset ? kLowPropRadius : kPropRadius;
            gather_options.radius = app.low_preset ? kLowModelRadius : kModelRadius;
            gather_options.prop_radius = prop_radius;
            gather_options.prop_near_radius = kPropLod0 + kPropLodBand + kRegather + kSwapLag;
            showroom.set_world_fade(float(kPropLod0), float(kPropLodBand),
                                    float(prop_radius - kRegather - kSwapLag - kPropEdgeBand),
                                    float(prop_radius - kRegather - kSwapLag));
            // SHIFT_WORLD_FADE="lod band start end" overrides them (checks).
            if (const char* e = std::getenv("SHIFT_WORLD_FADE")) {
              float f[4];
              if (std::sscanf(e, "%f %f %f %f", &f[0], &f[1], &f[2], &f[3]) == 4) showroom.set_world_fade(f[0], f[1], f[2], f[3]);
            }
            // Headless frames repeat: their swaps stay instant.
            if (!app.headless || std::getenv("SHIFT_SWAP_FADE")) showroom.set_world_swap_fade(kSwapFadeSeconds);
          }
          // Free roam: found signs and each barn's other door variant are
          // left out of the gathers from the first one on.
          if (app.collectibles_on && (!app.bench || app.bench_full_fx) && !app.flat_world && app.colorado_world()) {
            const auto game_root = app.game_root_of();
            if (app.collectibles.signs.empty()) {
              std::string why;
              if (!freeroam::load_collectibles(game_root, app.collectibles, &why)) {
                std::fprintf(stderr, "carview: note: no collectibles: %s\n", why.c_str());
                app.collectibles = {};
              }
            }
            if (!app.collectibles.signs.empty()) {
              if (!app.progress_loaded) {
                if (app.have_saved) progress_from_save(app.saved, app.collectibles, app.progress);
                app.progress.total_distance += app.seed_distance;
                app.progress_loaded = true;
              }
              // SHIFT_LIST_SIGNS=1: each sign's id and position (for --spawn).
              // Each also says whether car collision lies within 2 m of it
              // (a sign is found by touching it).
              if (std::getenv("SHIFT_LIST_SIGNS")) {
                int reachable = 0;
                for (const auto& sg : app.collectibles.signs) {
                  world::CollisionMesh cm;
                  std::string e;
                  bool near = false;
                  if (collision.gather(sg.at.x, sg.at.z, 4, cm, &e))
                    for (size_t t = 0; t < cm.triangles() && !near; ++t) {
                      if (!world::Collision::collides(cm, t)) continue;
                      const auto& a = cm.positions[3 * t];
                      const auto& b = cm.positions[3 * t + 1];
                      const auto& c = cm.positions[3 * t + 2];
                      if (std::fabs((a[1] + b[1] + c[1]) / 3 - sg.at.y) > 4) continue;
                      for (int k = 0; k < 9 && !near; ++k) {
                        const double px = sg.at.x + (k % 3 - 1) * 1.4, pz = sg.at.z + (k / 3 - 1) * 1.4;
                        auto side = [&](const std::array<float, 3>& p, const std::array<float, 3>& q) {
                          return (q[0] - p[0]) * (pz - p[2]) - (q[2] - p[2]) * (px - p[0]);
                        };
                        const double d0 = side(a, b), d1 = side(b, c), d2 = side(c, a);
                        near = (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
                      }
                    }
                  reachable += near;
                  std::printf("sign %s %.1f %.1f %.1f%s\n", sg.id.c_str(), sg.at.x, sg.at.y, sg.at.z,
                              near ? "" : "  (no collision within 2 m)");
                }
                std::printf("signs: %d of %zu with collision within 2 m\n", reachable, app.collectibles.signs.size());
              }
              free_roam = std::make_unique<freeroam::FreeRoam>(app.collectibles, app.progress);
              gather_options.hidden = hidden_sent = free_roam->hidden();
              if (!app.world_dir.empty() && !app.collectibles.barns.empty()) {
                std::vector<world::BarnDoorCollider> bspecs;
                bspecs.reserve(app.collectibles.barns.size());
                for (const auto& b : app.collectibles.barns) {
                  world::BarnDoorCollider bd;
                  bd.barn_name = b.name;
                  bd.closed_coll_id = b.closed_coll_id;
                  bd.open_coll_id = b.open_coll_id;
                  bspecs.push_back(std::move(bd));
                }
                if (app.smashables.load_barn_doors(app.world_dir / "Ribbon_00" / "GameObjs.xml", bspecs) && hidden_sent) {
                  app.smashables.sync_barns_from_hidden(*hidden_sent);
                }
              }
              std::printf("free roam: %d of %zu signs, %d of %zu barn finds, %.1f km driven\n",
                          free_roam->signs_found(), app.collectibles.signs.size(), free_roam->barns_found(),
                          app.collectibles.barns.size(), app.progress.total_distance / 1000);
            }
            if (!app.map_tried) {
              app.map_tried = true;
              std::string why;
              if (!app.map_view.open(game_root, &why)) std::fprintf(stderr, "carview: note: no map: %s\n", why.c_str());
              traffic::NavMap nav_for_gps;
              if (traffic::read_nav(game_root / "media" / "tracks" / "colorado" / "colorado.nav", nav_for_gps, nullptr)) {
                app.gps_solver.build(nav_for_gps);
                if (app.road_discovery.build(nav_for_gps) && app.have_saved && !app.saved.road_discovery_words.empty()) {
                  app.road_discovery.load_words(app.saved.road_discovery_nodes, app.saved.road_discovery_words);
                }
                if (app.road_discovery.ready()) {
                  std::printf("roads discovered: %d / %d\n", app.road_discovery.discovered_count(),
                              app.road_discovery.total_roads());
                }
              }
            }
            if (!app.outposts_tried) {
              app.outposts_tried = true;
              std::string owhy;
              if (freeroam::load_outposts(game_root, app.outposts, &owhy)) {
                app.have_outposts = true;
                std::printf("outposts: %zu outposts (%d discovered), %zu PR stunts (%d completed)\n",
                            app.outposts.outposts.size(), app.outpost_progress.discovered_count(app.outposts),
                            app.outposts.stunts.size(), app.outpost_progress.completed_stunt_count(app.outposts));
              } else {
                std::fprintf(stderr, "carview: note: no outposts: %s\n", owhy.c_str());
              }
            }
            if (app.has_cli_gps && !app.cli_gps_applied && app.gps_solver.ready()) {
              app.cli_gps_applied = true;
              const double sx0 = app.have_spawn ? app.spawn[0] : (app.near_given || app.render_ground ? app.near_x : world::kSpawnX);
              const double sz0 = app.have_spawn ? app.spawn[2] : (app.near_given || app.render_ground ? app.near_z : world::kSpawnZ);
              app.gps_nav.set_destination(app.cli_gps_x, app.cli_gps_z, traffic::GpsDestKind::kGeneric, "GPS WAYPOINT", sx0, sz0);
            }
            if (!app.cli_pr_stunt.empty() && !app.cli_pr_stunt_applied && app.have_outposts) {
              app.cli_pr_stunt_applied = true;
              if (const auto* st = app.outposts.find_stunt(app.cli_pr_stunt)) {
                const int64_t base_pts = app.skill_tracker ? int64_t(std::llround(app.skill_tracker->popularity_points())) : 0;
                app.pr_stunt_session.start(*st, base_pts);
                if (app.gps_solver.ready()) {
                  const double sx0 = app.have_spawn ? app.spawn[0] : (app.near_given || app.render_ground ? app.near_x : world::kSpawnX);
                  const double sz0 = app.have_spawn ? app.spawn[2] : (app.near_given || app.render_ground ? app.near_z : world::kSpawnZ);
                  app.gps_nav.set_destination(st->target_pos.x, st->target_pos.z, traffic::GpsDestKind::kGeneric,
                                          st->title, sx0, sz0);
                }
              }
            }
          }
          // The radio and Your Music: initialized once, streamed off-thread.
          if (app.sound && (!app.bench || app.bench_audio || app.bench_full_fx) && (!app.headless || !app.audio_out.empty() || app.bench_audio || app.bench_full_fx)) {
            const auto aud_world_t0 = std::chrono::steady_clock::now();
            const auto game_root = app.game_root_of();
            if (app.radio_on && app.audio_host.init_radio(game_root, app.language, app.have_saved ? app.saved.radio_station : "", !app.headless) &&
                !app.radio_logos_tried) {
              app.radio_logos_tried = true;
              std::string zwhy;
              if (auto zip = fh1::Archive::open(game_root / "media" / "ui" / "textures" / "Horizon.zip", &zwhy)) {
                const std::pair<const char*, const char*> kLogos[] = {
                    {"Radio1", "RS_BassArena"}, {"Radio2", "RS_Pulse"}, {"Radio3", "RS_2Eleven"},
                    {"Radio4_Silent", "RS_Off"}};
                for (const auto& [station, file] : kLogos) {
                  auto r = zip->read(std::string("Hud/Radio/") + file + ".xds");
                  if (!r.ok()) continue;
                  auto x = fh1::decode_xds(r.data);
                  if (x.levels.empty() || x.levels[0].rgba.empty()) continue;
                  app.radio_logos[station] = {std::move(x.levels[0].rgba), x.levels[0].width, x.levels[0].height};
                }
              }
            }
            if (!app.headless || app.bench_audio || app.bench_full_fx) {
              app.audio_host.init_user_music(app.user_music_dir, true);
              const auto sfx_cache = !app.sound_dir.empty() ? app.sound_dir : (app.state_root / "sound-cache");
              app.audio_host.init_sfx(game_root, sfx_cache, app.audio_host.car_burble_bank(), app.state_root);
              app.audio_host.init_satnav(game_root, sfx_cache, app.language, app.metric_units);
              app.audio_host.init_vo(game_root, app.language);
              app.audio_host.init_acoustics(game_root, sfx_cache, app.world_dir, app.rally_catalog.package_dir);
            }
            if (fh1::CjkFont::language_needs_cjk(app.language) && !carview::ui::cjk_font_loaded()) {
              auto cjk = std::make_shared<fh1::CjkFont>();
              if (cjk->load_for_language(game_root, app.language, nullptr)) {
                carview::ui::set_cjk_font(std::move(cjk));
              }
            }
            app.bench_startup.audio_load_ms +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - aud_world_t0).count();
          }
          loaded_world_route = active_route;
          world_gather_options = gather_options;
          streamer = std::make_unique<world::Streamer>(colorado, gather_options, kRegather,
                                                       app.render_ground ? nullptr : &collision);
          // Gathers after the first run on a worker in the window (and with
          // --async-stream headless), with the scenery's draw data built
          // there too; headless runs stay synchronous so they repeat.
          streamer->set_async(streamer_async);
          // The worker builds the scenery's draw data and stages it in
          // host-visible buffers; mid-drive textures stream through the
          // ring-buffered <= 1.8 ms/frame uploader so 200 m boundaries never spike.
          streamer->set_prepare([this](const world::Mesh& mesh, double cx, double cz) {
            const bool async_mid_drive = streamer_async && streamer && streamer->gathers() >= 1;
            if (async_mid_drive) lower_stream_worker_priority();
            auto staged = std::make_shared<Showroom::StagedWorld>();
            std::string e;
            std::vector<uint32_t> pin_records;
            WorldTriangles triangles = scenery(mesh, cx, cz);
            texture_loader.load(colorado, triangles, showroom, pin_records);
            std::shared_ptr<const void> pin_guard(
                reinterpret_cast<const void*>(1),
                [this, pins = std::move(pin_records)](const void*) { showroom.release_prepare_pins(pins); });
            WorldGeometry geom = build_world(triangles);
            if (async_mid_drive && !geom.textures.empty()) {
              for (auto& tex : geom.textures) {
                if (tex) showroom.queue_deferred_world_texture(std::move(tex));
              }
              geom.textures.clear();
            }
            if (!showroom.stage_world(std::move(geom), *staged, e, cx, cz)) staged.reset();
            if (staged) {
              staged->origin_x = cx;
              staged->origin_z = cz;
              staged->pin_guard = std::move(pin_guard);
            }
            auto prepared = std::make_shared<Prepared>();
            prepared->staged = std::move(staged);
            prepared->road = world::RoadFollower::road_points(mesh);
            prepared->glows.reserve(mesh.world_glows.size());
            for (const auto& g : mesh.world_glows) {
              world::WorldGlow sg = g;
              sg.position[0] = float(double(g.position[0]) - cx);
              sg.position[2] = float(double(g.position[2]) - cz);
              prepared->glows.push_back(sg);
            }
            return std::any(prepared);
          });
          const double sx = app.have_spawn ? app.spawn[0] : app.near_given || app.render_ground ? app.near_x : world::kSpawnX;
          const double sz = app.have_spawn ? app.spawn[2] : app.near_given || app.render_ground ? app.near_z : world::kSpawnZ;
          const double near[2] = {app.near_x, app.near_z};
          if (!streamer->update(sx, sz, nullptr, &app.error)) {
            app.status = 1;
          } else {
            if (app.fast_travel) {
              app.fast_travel = false;
              drive::Hit h;
              if (streamer->ground().raycast({app.spawn[0], 2000.0, app.spawn[2]}, {0, -1, 0}, 4000.0, h)) {
                app.spawn[1] = 2000.0 - h.distance;
              } else {
                world::Spawn fsp;
                if (world::find_spawn(streamer->mesh(), app.spawn[0], app.spawn[2], fsp)) app.spawn[1] = fsp.position[1];
              }
            }
            if (!world::choose_spawn(*streamer, app.have_spawn ? app.spawn : nullptr, app.near_given ? near : nullptr,
                                     app.near_given ? nullptr : &app.near_heading, sp, &how)) {
              app.error = "no road triangle near the search point";
              app.status = 1;
            }
          }
        }
        if (app.status == 0) {
          std::printf("spawn %s: %.3f %.3f %.3f heading %.4f rad\n", how, sp.position[0], sp.position[1],
                      sp.position[2], sp.heading);
          start = {sp.position[0], sp.position[1], sp.position[2]};
          start_heading = sp.heading;
          // "Back to the start" keeps the saved spawn, not where the save left the car.
          if (app.have_saved && app.saved.has_spawn) {
            start = {app.saved.spawn[0], app.saved.spawn[1], app.saved.spawn[2]};
            start_heading = app.saved.spawn_heading;
          }
          ground = &streamer->ground();
          follower.set_ground(&streamer->ground());
          if (!set_staged_world())  // and the follower's road points
            app.status = 1;
          else
            showroom.set_background(0.42f, 0.55f, 0.72f);
        }
        if (app.status != 0) std::fprintf(stderr, "carview: %s\n", app.error.c_str());
        app.bench_startup.world_load_ms = std::max(
            0.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - world_load_t0).count() -
                     app.bench_startup.audio_load_ms);
      }
  return app.status == 0;
}

bool DriveWorldHost::hot_reload_car_and_world() {
        const auto t_hot0 = std::chrono::steady_clock::now();
        showroom.set_instances({});
        const ShowroomOptions hot_opts = build_car_options();
        if (!showroom.reload_car(app.bake_dir, app.car, hot_opts, app.error)) {
          std::fprintf(stderr, "carview: hot car switch %s: %s\n", app.car.c_str(), app.error.c_str());
          app.status = 1;
          return false;
        }
        if (const auto groot = app.game_root_of(); !groot.empty()) showroom.load_car_lamps(groot, app.car);
        // Re-gather only if the route changed or spawn teleported far from the resident world origin.
        if (streamer && !app.world_dir.empty()) {
          const int want_route = app.gather_route_given ? app.gather_route : (app.race ? app.race->event().route_id : 0);
          const double sx = app.have_spawn ? app.spawn[0] : start.x;
          const double sz = app.have_spawn ? app.spawn[2] : start.z;
          const double dist_origin = std::hypot(sx - showroom.world_origin_x(), sz - showroom.world_origin_z());
          if (want_route != loaded_world_route || dist_origin > 120.0 || app.fast_travel) {
            loaded_world_route = want_route;
            world_gather_options.route = want_route;
            if (!app.render_ground) collision.set_active_route(want_route);
            streamer = std::make_unique<world::Streamer>(colorado, world_gather_options, kRegather,
                                                         app.render_ground ? nullptr : &collision);
            streamer->set_async(streamer_async);
            streamer->set_prepare([this](const world::Mesh& mesh, double cx, double cz) {
              const bool async_mid_drive = streamer_async && streamer && streamer->gathers() >= 1;
              if (async_mid_drive) lower_stream_worker_priority();
              auto staged = std::make_shared<Showroom::StagedWorld>();
              std::string e;
              std::vector<uint32_t> pin_records;
              WorldTriangles triangles = scenery(mesh, cx, cz);
              texture_loader.load(colorado, triangles, showroom, pin_records);
              std::shared_ptr<const void> pin_guard(
                  reinterpret_cast<const void*>(1),
                  [this, pins = std::move(pin_records)](const void*) { showroom.release_prepare_pins(pins); });
              WorldGeometry geom = build_world(triangles);
              if (async_mid_drive && !geom.textures.empty()) {
                for (auto& tex : geom.textures) {
                  if (tex) showroom.queue_deferred_world_texture(std::move(tex));
                }
                geom.textures.clear();
              }
              if (!showroom.stage_world(std::move(geom), *staged, e, cx, cz)) staged.reset();
              if (staged) {
                staged->origin_x = cx;
                staged->origin_z = cz;
                staged->pin_guard = std::move(pin_guard);
              }
              auto prepared = std::make_shared<Prepared>();
              prepared->staged = std::move(staged);
              prepared->road = world::RoadFollower::road_points(mesh);
              prepared->glows.reserve(mesh.world_glows.size());
              for (const auto& g : mesh.world_glows) {
                world::WorldGlow sg = g;
                sg.position[0] = float(double(g.position[0]) - cx);
                sg.position[2] = float(double(g.position[2]) - cz);
                prepared->glows.push_back(sg);
              }
              return std::any(prepared);
            });
            if (!streamer->update(sx, sz, nullptr, &app.error)) {
              std::fprintf(stderr, "carview: hot regather: %s\n", app.error.c_str());
              app.status = 1;
              return false;
            }
            if (app.fast_travel) {
              app.fast_travel = false;
              drive::Hit h;
              if (streamer->ground().raycast({app.spawn[0], 2000.0, app.spawn[2]}, {0, -1, 0}, 4000.0, h))
                app.spawn[1] = 2000.0 - h.distance;
            }
            ground = &streamer->ground();
            follower.set_ground(&streamer->ground());
            if (!set_staged_world()) {
              std::fprintf(stderr, "carview: hot stage world: %s\n", app.error.c_str());
              app.status = 1;
              return false;
            }
          }
          if (app.have_spawn) {
            start = {app.spawn[0], app.spawn[1], app.spawn[2]};
            start_heading = app.spawn[3];
          }
        }
        const double hot_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_hot0).count();
        std::printf("carview: hot swap to %s in %.1f ms (world textures kept: %zu, gpu draws: %u)\n", app.car.c_str(),
                    hot_ms, showroom.world_texture_count(), showroom.gpu_resident_draws());
  return true;
}

void DriveWorldHost::run() {
  if (!open_world_and_car()) return;
  for (bool session_again = true; session_again && app.status == 0;) {
    session_again = false;
    if (!first_session) {
      if (!hot_reload_car_and_world()) break;
    }
    first_session = false;
    DriveSession session(app, *this);
    session.init_session();
    session.run(session_again);
  }
  if (app.validation) std::printf("validation messages: %u\n", renderer.validation_messages());
  if (app.status == 0 && renderer.validation_messages()) app.status = 3;
}

}  // namespace carview
