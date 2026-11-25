#include "drive_world_host.h"

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cars/visual_setup.h"
#include "display_hdr_win.h"
#include "drive_session.h"
#include "fh1data/cjk_font.h"
#include "fh1data/members.h"
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

// Resolves TU4 / Horizon Rally surface types (surfaces 68-71: RallyGrass, Dirt2,
// Dirt3, RallyGravel) from title-update-v4/media.zip (physics/surfacetypes.xml),
// extracted title-update-v4/, 1600_pri_65, or state_root VFS, falling back to
// base media/physics.zip if unavailable. Supports both full surfacetypes.xml
// replacements and updated-surfacetypes.xml overlays.
std::filesystem::path resolve_surface_types_source(const DriveApp& app,
                                                   const std::filesystem::path& base_phys_zip) {
  namespace fs = std::filesystem;
  std::error_code ec;

  fs::path base_src = base_phys_zip;
  if (!fs::exists(base_src, ec) && !app.colorado_world_dir.empty()) {
    const fs::path alt = app.colorado_world_dir.parent_path().parent_path() / "physics.zip";
    if (fs::exists(alt, ec)) base_src = alt;
  }

  size_t base_count = 0;
  std::string base_xml;
  if (fs::exists(base_src, ec)) {
    fh1::MemberSource base_ms;
    std::vector<uint8_t> bytes;
    if (base_ms.open(base_src, nullptr) && base_ms.read("surfacetypes.xml", bytes, nullptr)) {
      base_xml.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      std::vector<fh1::SurfaceType> parsed;
      if (fh1::parse_surface_types(base_xml, parsed, nullptr)) base_count = parsed.size();
    }
  }

  fs::path sr = app.state_root;
  if (sr.empty()) {
    if (const char* env_sr = std::getenv("SHIFT_STATE_ROOT"); env_sr && *env_sr) sr = env_sr;
  }

  std::vector<fs::path> candidates;
  if (!sr.empty()) {
    candidates.push_back(sr / "title-update-v4" / "media.zip");
    candidates.push_back(sr / "title-update-v4");
    candidates.push_back(sr / "media.zip");
  }
  if (!app.rally_catalog.package_dir.empty()) {
    candidates.push_back(app.rally_catalog.package_dir / "Media" / "DLCZips" / "1600_pri_65");
    candidates.push_back(app.rally_catalog.package_dir / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "physics.zip");
  }
  if (!app.world_dir.empty() && !app.colorado_world()) {
    const fs::path rally_media = app.world_dir.parent_path().parent_path();
    candidates.push_back(rally_media / "physics.zip");
    candidates.push_back(rally_media);
  }

  auto write_cached_xml = [&](const std::string& xml_text) -> fs::path {
    const fs::path cache_dir = (!sr.empty() ? (sr / "cache") : fs::temp_directory_path(ec)) / "surfaces_v4";
    fs::create_directories(cache_dir, ec);
    const fs::path out_file = cache_dir / "surfacetypes.xml";
    std::ofstream f(out_file, std::ios::binary | std::ios::trunc);
    if (!f) return {};
    f.write(xml_text.data(), std::streamsize(xml_text.size()));
    f.close();
    return f.good() ? cache_dir : fs::path{};
  };

  for (const auto& cand : candidates) {
    if (cand.empty() || !fs::exists(cand, ec)) continue;
    fh1::MemberSource ms;
    if (!ms.open(cand, nullptr)) continue;

    // 1. Direct full surfacetypes.xml (e.g. title-update-v4/media.zip -> physics/surfacetypes.xml, 72 entries).
    if (ms.contains("surfacetypes.xml")) {
      std::vector<uint8_t> bytes;
      if (ms.read("surfacetypes.xml", bytes, nullptr)) {
        std::vector<fh1::SurfaceType> parsed;
        const std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (fh1::parse_surface_types(sv, parsed, nullptr) &&
            (parsed.size() >= 72 || parsed.size() > base_count)) {
          return cand;
        }
      }
    }

    // 2. Explicit updated-surfacetypes.xml (full replacement or partial 68..71 overlay).
    if (ms.contains("updated-surfacetypes.xml")) {
      std::vector<uint8_t> bytes;
      if (ms.read("updated-surfacetypes.xml", bytes, nullptr)) {
        const std::string upd_xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::vector<fh1::SurfaceType> parsed;
        if (fh1::parse_surface_types(upd_xml, parsed, nullptr) &&
            (parsed.size() >= 72 || parsed.size() > base_count)) {
          const fs::path dir = write_cached_xml(upd_xml);
          if (!dir.empty()) return dir;
        } else if (!base_xml.empty()) {
          // Merge extra Rally surface entries (68..71) into base SurfaceTypes.xml.
          std::string merged = base_xml;
          static constexpr const char* kRallyNames[4] = {"RallyGrass", "Dirt2", "Dirt3", "RallyGravel"};
          const size_t list_close = merged.find("</SurfaceTypesList>");
          const size_t defs_close = merged.find("</SurfaceTypes>");
          if (list_close != std::string::npos && defs_close != std::string::npos && list_close < defs_close) {
            std::string add_list;
            std::string add_defs;
            for (const char* rname : kRallyNames) {
              const std::string open_tag = std::string("<") + rname;
              const std::string close_tag = std::string("</") + rname + ">";
              const size_t d0 = upd_xml.find(open_tag);
              const size_t d1 = d0 != std::string::npos ? upd_xml.find(close_tag, d0) : std::string::npos;
              if (merged.find(std::string("value=\"") + rname + "\"") == std::string::npos) {
                add_list += "    <SurfaceType value=\"" + std::string(rname) + "\" />\n";
              }
              if (d0 != std::string::npos && d1 != std::string::npos) {
                add_defs += "\n" + upd_xml.substr(d0, (d1 + close_tag.size()) - d0) + "\n";
              }
            }
            if (!add_list.empty() || !add_defs.empty()) {
              merged.insert(defs_close, add_defs);
              merged.insert(list_close, add_list);
              std::vector<fh1::SurfaceType> check_parsed;
              if (fh1::parse_surface_types(merged, check_parsed, nullptr) && check_parsed.size() > base_count) {
                const fs::path dir = write_cached_xml(merged);
                if (!dir.empty()) return dir;
              }
            }
          }
        }
      }
    }
  }

  return base_src;
}

struct LoadedCarRecord {
  std::string car_media_low;
  ShowroomOptions opts;
};

std::mutex g_loaded_cars_mutex;
std::unordered_map<const DriveWorldHost*, LoadedCarRecord> g_loaded_cars;

bool same_brake_spec(const cars::BrakeVisualSpec& a, const cars::BrakeVisualSpec& b) {
  return a.size_in == b.size_in && a.type_id == b.type_id && a.pistons == b.pistons && a.level == b.level;
}

bool same_visual_setup(const cars::VisualSetup& a, const cars::VisualSetup& b) {
  return a.active == b.active && a.shown_parts == b.shown_parts && a.removed_parts == b.removed_parts &&
         a.has_exhaust_override == b.has_exhaust_override && a.exhaust_part == b.exhaust_part &&
         a.wheel_media == b.wheel_media && a.wheel_id == b.wheel_id && a.brake_draw_offset == b.brake_draw_offset &&
         a.rim_offset_front == b.rim_offset_front && a.rim_offset_rear == b.rim_offset_rear &&
         a.wheel_is_chrome == b.wheel_is_chrome && a.wheel_is_stock == b.wheel_is_stock &&
         a.wheel_is_exception == b.wheel_is_exception && a.wheel_is_paintable == b.wheel_is_paintable &&
         a.tire_compound_level == b.tire_compound_level && a.tire_compound_id == b.tire_compound_id &&
         a.tyre_width_mm == b.tyre_width_mm && a.rim_inches == b.rim_inches &&
         a.stock_tyre_width_mm == b.stock_tyre_width_mm && a.stock_rim_inches == b.stock_rim_inches &&
         a.stock_tyre_aspect == b.stock_tyre_aspect && a.tyre_media_override == b.tyre_media_override &&
         a.paint_exceptions.found == b.paint_exceptions.found &&
         a.paint_exceptions.no_mirrors == b.paint_exceptions.no_mirrors &&
         a.paint_exceptions.no_windows == b.paint_exceptions.no_windows &&
         a.paint_exceptions.no_hood == b.paint_exceptions.no_hood &&
         a.paint_exceptions.no_paintable_wing == b.paint_exceptions.no_paintable_wing &&
         a.front_bumper_level == b.front_bumper_level && a.rear_bumper_level == b.rear_bumper_level &&
         a.rear_wing_level == b.rear_wing_level && a.spring_damper_level == b.spring_damper_level &&
         a.has_rally_bodykit == b.has_rally_bodykit && a.has_rally_suspension == b.has_rally_suspension &&
         a.has_rally_tires == b.has_rally_tires && a.ride_height_m == b.ride_height_m &&
         a.track_offset_m == b.track_offset_m && a.camber_rad == b.camber_rad &&
         same_brake_spec(a.brakes[0], b.brakes[0]) && same_brake_spec(a.brakes[1], b.brakes[1]) &&
         a.caliper_angle_deg == b.caliper_angle_deg &&
         a.has_caliper_angle_override == b.has_caliper_angle_override;
}

bool same_showroom_options(const ShowroomOptions& a, const ShowroomOptions& b) {
  return a.colour == b.colour && a.match_rgb == b.match_rgb && a.match_sequence == b.match_sequence &&
         a.livery_overlay == b.livery_overlay && a.all_variants == b.all_variants && a.car_id == b.car_id &&
         a.game_root == b.game_root && same_visual_setup(a.visual, b.visual);
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
        if (!streamer || !streamer->prepared().has_value()) {
          app.error = "world staging buffers";
          return false;
        }
        auto* prepared_ptr = std::any_cast<std::shared_ptr<Prepared>>(&streamer->prepared());
        std::shared_ptr<Prepared> prepared = prepared_ptr ? std::move(*prepared_ptr) : std::shared_ptr<Prepared>{};
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
        const bool can_defer_world = !app.headless && streamer_async && streamer->gathers() > 1;
        const bool wait_deferred_tex = !app.bench && showroom.deferred_upload_queue_size() > 0;
        if (can_defer_world && (showroom.has_pending_world() || wait_deferred_tex)) {
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
#ifdef __ANDROID__
        shadow.size = 768, shadow.radius = 40, shadow.taps = 1, shadow.cascades = 2;
#else
        shadow.size = 1024, shadow.radius = 40, shadow.taps = 1, shadow.cascades = 2;
#endif
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
    std::string car_load_err;
    auto fut_car_load = std::async(std::launch::async, [&]() {
      return showroom.load(app.bake_dir, app.car, options, car_load_err);
    });
    const auto vk_init_t0 = std::chrono::steady_clock::now();
    const bool vk_init_ok = renderer.init(render_options, app.error);
    const auto vk_init_t1 = std::chrono::steady_clock::now();
    if (!vk_init_ok) {
      if (fut_car_load.valid()) fut_car_load.wait();
      std::fprintf(stderr, "carview: %s\n", app.error.c_str());
      app.status = 1;
      return false;
    }

    // Windowed boot: render an animated 60 FPS FH1 Loading Screen while pumping SDL events
    // so the main thread NEVER blocks or triggers OS "Not Responding" during startup!
    const bool boot_live_ui = !app.headless && !app.bench && app.window != nullptr;
    LoadingScreen boot_loading;
    bool boot_atlas_synced = false;
    bool boot_cancelled = false;
    auto boot_last_tp = std::chrono::steady_clock::now();
    if (boot_live_ui) {
      boot_loading.load_definitions(app.game_root_of(), "freeroam", app.state_root);
      boot_loading.start("HORIZON COLORADO", "LOADING");
      boot_loading.set_progress(0.08f);
    }
    auto pump_boot_frame = [&](float target_progress) {
      if (!boot_live_ui || boot_cancelled) return;
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) {
          boot_cancelled = true;
          app.status = 0;
        }
      }
      if (boot_cancelled) return;
      const auto now_tp = std::chrono::steady_clock::now();
      const double dt = std::clamp(std::chrono::duration<double>(now_tp - boot_last_tp).count(), 0.001, 0.1);
      boot_last_tp = now_tp;
      boot_loading.update(dt);
      boot_loading.set_progress(std::clamp(target_progress, 0.05f, 0.99f));
      int bw = 0, bh = 0;
      SDL_GetWindowSizeInPixels(app.window, &bw, &bh);
      if (bw > 32 && bh > 32) {
        ui::List list;
        boot_loading.draw(list, float(bw), float(bh));
        upload_ui_atlas(renderer, !boot_atlas_synced);
        boot_atlas_synced = true;
        renderer.set_ui(list.items());
        std::string draw_err;
        renderer.draw_frame(FrameConstants{}, draw_err);
      }
    };
    auto wait_future_pumping = [&](auto& fut, float p0, float p1) {
      if (!fut.valid()) return;
      const auto t_start = std::chrono::steady_clock::now();
      while (fut.wait_for(std::chrono::milliseconds(boot_live_ui ? 16 : 50)) != std::future_status::ready) {
        const float el = float(std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count());
        const float frac = std::clamp(el / 2.0f, 0.0f, 0.95f);
        pump_boot_frame(p0 + (p1 - p0) * frac);
      }
    };
    pump_boot_frame(0.12f);

    // Kick off world index, collision, collectibles, map/nav/outposts, and audio init IN PARALLEL
    // right away so they overlap completely with car upload & pipeline creation!
    const auto world_load_t0 = std::chrono::steady_clock::now();
    std::string world_open_err, col_err, warning;
    std::future<bool> fut_world_open, fut_col, fut_collectibles, fut_map_nav, fut_audio;
    const bool have_world = !app.world_dir.empty();
    const bool want_collectibles =
        have_world && app.collectibles_on && (!app.bench || app.bench_full_fx) && !app.flat_world && app.colorado_world();
    const bool want_audio =
        have_world && app.sound && (!app.bench || app.bench_audio || app.bench_full_fx) &&
        (!app.headless || !app.audio_out.empty() || app.bench_audio || app.bench_full_fx);
    const auto groot_cached = app.game_root_of();
    if (have_world) {
      if (app.cache_path.empty()) {
        app.cache_path = std::filesystem::path("out") / "world" /
                         (world::model_prefix(app.world_dir).substr(0, world::model_prefix(app.world_dir).size() - 3) +
                          ".index");
      }
      fut_world_open = std::async(std::launch::async, [&]() {
        return colorado.open(app.world_dir, app.cache_path, &world_open_err);
      });
      if (!app.render_ground) {
        const auto base_phys_zip =
            (app.game_root_given.empty() ? app.world_dir.parent_path().parent_path() : groot_cached / "media") /
            "physics.zip";
        const auto phys_src = resolve_surface_types_source(app, base_phys_zip);
        fut_col = std::async(std::launch::async, [&, phys_src]() {
          return collision.open(app.world_dir, phys_src, &col_err, &warning);
        });
      }
      if (want_collectibles) {
        fut_collectibles = std::async(std::launch::async, [&, groot_cached]() {
          if (app.collectibles.signs.empty()) {
            std::string why;
            if (!freeroam::load_collectibles(groot_cached, app.collectibles, &why)) {
              std::fprintf(stderr, "carview: note: no collectibles: %s\n", why.c_str());
              app.collectibles = {};
              return false;
            }
          }
          return !app.collectibles.signs.empty();
        });
        fut_map_nav = std::async(std::launch::async, [&, groot_cached]() {
          if (!app.map_tried) {
            app.map_tried = true;
            std::string why;
            if (!app.map_view.open(groot_cached, &why, app.language))
              std::fprintf(stderr, "carview: note: no map: %s\n", why.c_str());
            if (!app.shared_nav_loaded) {
              app.shared_nav_loaded = traffic::read_nav(
                  groot_cached / "media" / "tracks" / "colorado" / "colorado.nav", app.shared_nav, nullptr);
            }
            if (app.shared_nav_loaded) {
              app.gps_solver.build(app.shared_nav);
              if (app.road_discovery.build(app.shared_nav) && app.have_saved &&
                  !app.saved.road_discovery_words.empty()) {
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
            if (freeroam::load_outposts(groot_cached, app.outposts, &owhy)) {
              app.have_outposts = true;
              std::printf("outposts: %zu outposts (%d discovered), %zu PR stunts (%d completed)\n",
                          app.outposts.outposts.size(), app.outpost_progress.discovered_count(app.outposts),
                          app.outposts.stunts.size(), app.outpost_progress.completed_stunt_count(app.outposts));
            } else {
              std::fprintf(stderr, "carview: note: no outposts: %s\n", owhy.c_str());
            }
          }
          return true;
        });
      }
      if (want_audio) {
        const bool async_sc = !app.headless && !app.bench;
        fut_audio = std::async(std::launch::async, [&, groot_cached, async_sc]() {
          const auto aud_world_t0 = std::chrono::steady_clock::now();
          if (app.radio_on &&
              app.audio_host.init_radio(groot_cached, app.language, app.have_saved ? app.saved.radio_station : "",
                                        !app.headless) &&
              !app.radio_logos_tried) {
            app.radio_logos_tried = true;
            std::string zwhy;
            if (auto zip = fh1::Archive::open(groot_cached / "media" / "ui" / "textures" / "Horizon.zip", &zwhy)) {
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
            app.audio_host.init_sfx(groot_cached, sfx_cache, app.audio_host.car_burble_bank(), app.state_root);
            app.audio_host.init_satnav(groot_cached, sfx_cache, app.language, app.metric_units);
            app.audio_host.init_vo(groot_cached, app.language);
            app.audio_host.init_acoustics(groot_cached, sfx_cache, app.world_dir, app.rally_catalog.package_dir,
                                          async_sc);
            if (!app.colorado_world() && !app.rally_catalog.package_dir.empty()) {
              app.audio_host.init_codriver(app.rally_catalog.package_dir, sfx_cache, app.language);
            }
          }
          app.bench_startup.audio_load_ms +=
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - aud_world_t0).count();
          return true;
        });
      }
    }

    auto join_all_boot_futures = [&]() {
      if (fut_car_load.valid()) fut_car_load.wait();
      if (fut_world_open.valid()) fut_world_open.wait();
      if (fut_col.valid()) fut_col.wait();
      if (fut_collectibles.valid()) fut_collectibles.wait();
      if (fut_map_nav.valid()) fut_map_nav.wait();
      if (fut_audio.valid()) fut_audio.wait();
    };

    wait_future_pumping(fut_car_load, 0.12f, 0.25f);
    const bool car_loaded_ok = fut_car_load.get();
    if (!car_loaded_ok) app.error = car_load_err;
    const auto car_up_t0 = std::chrono::steady_clock::now();
    const bool car_up_ok = car_loaded_ok && showroom.upload(app.error);
    const auto car_up_t1 = std::chrono::steady_clock::now();
    if (car_up_ok) {
      const double load_ms = std::chrono::duration<double, std::milli>(vk_init_t1 - car_load_t0).count();
      const double vk_ms = std::chrono::duration<double, std::milli>(vk_init_t1 - vk_init_t0).count();
      const double up_ms = std::chrono::duration<double, std::milli>(car_up_t1 - car_up_t0).count();
      app.bench_startup.vulkan_init_ms = std::max(0.0, vk_ms - renderer.pipeline_compile_ms());
      app.bench_startup.car_bake_load_ms = std::max(0.0, load_ms - vk_ms) + std::max(0.0, up_ms - showroom.pipeline_compile_ms());
    }
    if (!car_up_ok) {
      join_all_boot_futures();
      std::fprintf(stderr, "carview: %s\n", app.error.c_str());
      app.status = 1;
      return false;
    }
    if (app.status != 0) {
      join_all_boot_futures();
      return false;
    }
      pump_boot_frame(0.38f);
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
      app.wait_tod_ready();
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
        app.wait_vfx_ready();
        std::string vfx_gpu_err;
        if (!showroom.init_vfx(&app.vfx_lib, vfx_gpu_err))
          std::fprintf(stderr, "carview: vfx gpu init note: %s\n", vfx_gpu_err.c_str());
      }
      // Persist the VkPipelineCache immediately after startup AOT pre-compilation
      // (not only on clean exit) so crashes or forced closes still retain warm caches.
      renderer.save_pipeline_cache();
      pump_boot_frame(0.48f);
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
      if (have_world) {
        world::Spawn sp;
        const char* how = "";
        wait_future_pumping(fut_world_open, 0.48f, 0.56f);
        if (!app.render_ground) wait_future_pumping(fut_col, 0.56f, 0.62f);
        const bool world_ok = fut_world_open.valid() ? fut_world_open.get() : false;
        const bool col_ok = app.render_ground ? true : (fut_col.valid() ? fut_col.get() : false);
        if (!world_ok) {
          app.error = world_open_err;
          app.status = 1;
        } else if (!col_ok) {
          app.error = col_err;
          app.status = 1;
        } else {
          if (!world_open_err.empty()) std::fprintf(stderr, "carview: note: %s\n", world_open_err.c_str());
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
          if (!app.flat_world && !app.no_far) {
#ifdef __ANDROID__
            gather_options.far_radius = app.low_preset ? 2200.0 : 3000.0;
#else
            gather_options.far_radius = 3000;
#endif
          }
          if (const char* fi = std::getenv("SHIFT_FAR_INNER")) gather_options.far_inner = std::atof(fi);
          if (!app.flat_world && !app.no_trees) {
#ifdef __ANDROID__
            gather_options.tree_radius = app.low_preset ? 850.0 : kTreeRadius;
#else
            gather_options.tree_radius = kTreeRadius;
#endif
          }
          if (!app.flat_world) {
#ifdef __ANDROID__
            const double prop_radius = app.low_preset ? 320.0 : kPropRadius;
            gather_options.radius = app.low_preset ? 420.0 : kModelRadius;
#else
            const double prop_radius = app.low_preset ? kLowPropRadius : kPropRadius;
            gather_options.radius = app.low_preset ? kLowModelRadius : kModelRadius;
#endif
            gather_options.prop_radius = prop_radius;
            gather_options.prop_near_radius = kPropLod0 + kPropLodBand + kRegather + kSwapLag;
            showroom.set_world_fade(float(kPropLod0), float(kPropLodBand),
                                    float(prop_radius - kRegather - kSwapLag - kPropEdgeBand),
                                    float(prop_radius - kRegather - kSwapLag));
            if (const char* e = std::getenv("SHIFT_WORLD_FADE")) {
              float f[4];
              if (std::sscanf(e, "%f %f %f %f", &f[0], &f[1], &f[2], &f[3]) == 4) showroom.set_world_fade(f[0], f[1], f[2], f[3]);
            }
            if (!app.headless || std::getenv("SHIFT_SWAP_FADE")) showroom.set_world_swap_fade(kSwapFadeSeconds);
          }
          // Free roam: found signs and each barn's other door variant are
          // left out of the gathers from the first one on.
          if (want_collectibles) {
            wait_future_pumping(fut_collectibles, 0.62f, 0.66f);
            if (fut_collectibles.valid()) (void)fut_collectibles.get();
            app.wait_smash_ready();
            if (!app.collectibles.signs.empty()) {
              if (!app.progress_loaded) {
                if (app.have_saved) progress_from_save(app.saved, app.collectibles, app.progress);
                app.progress.total_distance += app.seed_distance;
                app.progress_loaded = true;
              }
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
          }
          loaded_world_route = active_route;
          world_gather_options = gather_options;
          streamer = std::make_unique<world::Streamer>(colorado, gather_options, kRegather,
                                                       app.render_ground ? nullptr : &collision);
          streamer->set_async(streamer_async);
          streamer->set_lead(true);  // keeps a fast car central in the collision
          streamer->set_prepare([this](const world::Mesh& mesh, double cx, double cz) {
            const bool async_mid_drive = streamer_async && streamer && streamer->gathers() >= 1;
            if (async_mid_drive) lower_stream_worker_priority();
            auto staged = std::make_shared<Showroom::StagedWorld>();
            std::string e;
            std::vector<uint32_t> pin_records;
            WorldTriangles triangles = scenery(mesh, cx, cz);
            texture_loader.load(colorado, triangles, showroom, pin_records);
            {
              std::vector<uint32_t> new_pins;
              new_pins.reserve(triangles.textures.size());
              for (const auto& tex : triangles.textures) {
                if (tex && tex->record != kNoWorldTexture) new_pins.push_back(tex->record);
              }
              if (!new_pins.empty()) {
                showroom.pin_prepare_records(new_pins);
                pin_records.insert(pin_records.end(), new_pins.begin(), new_pins.end());
              }
            }
            std::shared_ptr<const void> pin_guard(
                reinterpret_cast<const void*>(1),
                [this, pins = std::move(pin_records)](const void*) { showroom.release_prepare_pins(pins); });
            WorldGeometry geom = build_world(triangles);
            const bool defer_textures = async_mid_drive && (!app.headless || app.bench);
            if (defer_textures && !geom.textures.empty()) {
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
          // Run the initial world gather off the main thread in windowed mode while pumping
          // the animated Loading Screen at 60 FPS so the window never freezes!
          bool first_gather_ok = false;
          if (boot_live_ui) {
            std::string g_err;
            auto fut_gather = std::async(std::launch::async, [&]() {
              return streamer->update(sx, sz, nullptr, &g_err);
            });
            wait_future_pumping(fut_gather, 0.66f, 0.92f);
            first_gather_ok = fut_gather.get();
            if (!first_gather_ok) app.error = g_err;
          } else {
            first_gather_ok = streamer->update(sx, sz, nullptr, &app.error);
          }
          if (!first_gather_ok) {
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
        // Join remaining background boot tasks while still pumping the smooth loading UI.
        wait_future_pumping(fut_map_nav, 0.92f, 0.95f);
        if (fut_map_nav.valid()) (void)fut_map_nav.get();
        if (want_collectibles) {
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
        wait_future_pumping(fut_audio, 0.95f, 0.98f);
        if (fut_audio.valid()) (void)fut_audio.get();
        if (fh1::CjkFont::language_needs_cjk(app.language) && !carview::ui::cjk_font_loaded()) {
          auto cjk = std::make_shared<fh1::CjkFont>();
          if (cjk->load_for_language(groot_cached, app.language, nullptr)) {
            carview::ui::set_cjk_font(std::move(cjk));
          }
        }
        if (app.fut_anim_.valid()) {
          wait_future_pumping(app.fut_anim_, 0.98f, 0.99f);
          app.wait_anim_ready();
        }
        app.wait_all_world_assets();
        if (boot_cancelled) {
          return false;
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
        app.bench_startup.world_load_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - world_load_t0).count();
      } else {
        app.wait_all_world_assets();
      }
  if (app.status == 0) {
    std::lock_guard<std::mutex> lock(g_loaded_cars_mutex);
    g_loaded_cars[this] = {lower_ascii(app.car), options};
  }
  return app.status == 0;
}

bool DriveWorldHost::hot_reload_car_and_world() {
        const auto t_hot0 = std::chrono::steady_clock::now();
        showroom.set_instances({});
        const ShowroomOptions hot_opts = build_car_options();
        const std::string car_low = lower_ascii(app.car);
        bool need_car_reload = true;
        {
          std::lock_guard<std::mutex> lock(g_loaded_cars_mutex);
          if (auto it = g_loaded_cars.find(this); it != g_loaded_cars.end()) {
            if (it->second.car_media_low == car_low && same_showroom_options(it->second.opts, hot_opts)) {
              need_car_reload = false;
            }
          }
        }
        const int want_route = app.gather_route_given ? app.gather_route : (app.race ? app.race->event().route_id : 0);
        const double sx = app.have_spawn ? app.spawn[0] : start.x;
        const double sz = app.have_spawn ? app.spawn[2] : start.z;
        const double dist_origin = std::hypot(sx - showroom.world_origin_x(), sz - showroom.world_origin_z());
        const bool need_regather =
            streamer && !app.world_dir.empty() && (want_route != loaded_world_route || dist_origin > 120.0 || app.fast_travel);

        // Pump an animated 60 FPS LoadingScreen in windowed mode whenever heavy car reload or
        // world regather work is required so SDL events stay responsive and the window never freezes.
        const bool hot_live_ui = (need_car_reload || need_regather) && !app.headless && !app.bench && app.window != nullptr;
        LoadingScreen hot_loading;
        bool hot_atlas_synced = false;
        bool hot_cancelled = false;
        auto hot_last_tp = std::chrono::steady_clock::now();
        if (hot_live_ui) {
          hot_loading.load_definitions(app.game_root_of(), app.race ? "race" : "freeroam", app.state_root);
          const std::string load_title =
              (app.race && !app.race_event.empty()) ? app.race_event : std::string("HORIZON COLORADO");
          const std::string load_sub = app.race ? "LOADING EVENT" : (need_car_reload ? "LOADING VEHICLE" : "TRAVELING");
          hot_loading.start(load_title, load_sub);
          hot_loading.set_progress(0.08f);
        }
        auto pump_hot_frame = [&](float target_progress) {
          if (!hot_live_ui || hot_cancelled) return;
          SDL_Event ev;
          while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
              hot_cancelled = true;
              app.status = 0;
            }
          }
          if (hot_cancelled) return;
          const auto now_tp = std::chrono::steady_clock::now();
          const double dt = std::clamp(std::chrono::duration<double>(now_tp - hot_last_tp).count(), 0.001, 0.1);
          hot_last_tp = now_tp;
          hot_loading.update(dt);
          hot_loading.set_progress(std::clamp(target_progress, 0.05f, 0.99f));
          int bw = 0, bh = 0;
          SDL_GetWindowSizeInPixels(app.window, &bw, &bh);
          if (bw > 32 && bh > 32) {
            ui::List list;
            hot_loading.draw(list, float(bw), float(bh));
            upload_ui_atlas(renderer, !hot_atlas_synced);
            hot_atlas_synced = true;
            renderer.set_ui(list.items());
            std::string draw_err;
            renderer.draw_frame(FrameConstants{}, draw_err);
          }
        };
        pump_hot_frame(0.12f);

        if (need_car_reload) {
          if (!showroom.reload_car(app.bake_dir, app.car, hot_opts, app.error)) {
            std::fprintf(stderr, "carview: hot car switch %s: %s\n", app.car.c_str(), app.error.c_str());
            app.status = 1;
            return false;
          }
          if (const auto groot = app.game_root_of(); !groot.empty()) showroom.load_car_lamps(groot, app.car);
          {
            std::lock_guard<std::mutex> lock(g_loaded_cars_mutex);
            g_loaded_cars[this] = {car_low, hot_opts};
          }
          pump_hot_frame(need_regather ? 0.35f : 0.92f);
        }
        // Re-gather only if the route changed or spawn teleported far from the resident world origin.
        if (streamer && !app.world_dir.empty()) {
          if (need_regather) {
            loaded_world_route = want_route;
            world_gather_options.route = want_route;
            if (!app.render_ground) collision.set_active_route(want_route);
            streamer = std::make_unique<world::Streamer>(colorado, world_gather_options, kRegather,
                                                         app.render_ground ? nullptr : &collision);
            streamer->set_async(streamer_async);
            streamer->set_lead(true);
            streamer->set_prepare([this](const world::Mesh& mesh, double cx, double cz) {
              const bool async_mid_drive = streamer_async && streamer && streamer->gathers() >= 1;
              if (async_mid_drive) lower_stream_worker_priority();
              auto staged = std::make_shared<Showroom::StagedWorld>();
              std::string e;
              std::vector<uint32_t> pin_records;
              WorldTriangles triangles = scenery(mesh, cx, cz);
              texture_loader.load(colorado, triangles, showroom, pin_records);
              {
                std::vector<uint32_t> new_pins;
                new_pins.reserve(triangles.textures.size());
                for (const auto& tex : triangles.textures) {
                  if (tex && tex->record != kNoWorldTexture) new_pins.push_back(tex->record);
                }
                if (!new_pins.empty()) {
                  showroom.pin_prepare_records(new_pins);
                  pin_records.insert(pin_records.end(), new_pins.begin(), new_pins.end());
                }
              }
              std::shared_ptr<const void> pin_guard(
                  reinterpret_cast<const void*>(1),
                  [this, pins = std::move(pin_records)](const void*) { showroom.release_prepare_pins(pins); });
              WorldGeometry geom = build_world(triangles);
              const bool defer_textures = async_mid_drive && (!app.headless || app.bench);
              if (defer_textures && !geom.textures.empty()) {
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
            bool gather_ok = false;
            if (hot_live_ui) {
              std::string g_err;
              auto fut_gather = std::async(std::launch::async, [&]() {
                return streamer->update(sx, sz, nullptr, &g_err);
              });
              const auto g_t0 = std::chrono::steady_clock::now();
              while (fut_gather.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
                const float el = float(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count());
                const float frac = std::clamp(el / 1.5f, 0.0f, 0.95f);
                pump_hot_frame(0.35f + 0.55f * frac);
              }
              gather_ok = fut_gather.get();
              if (!gather_ok) app.error = g_err;
            } else {
              gather_ok = streamer->update(sx, sz, nullptr, &app.error);
            }
            if (hot_cancelled) return false;
            if (!gather_ok) {
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
            if (!streamer->prepared().has_value() || !set_staged_world()) {
              std::fprintf(stderr, "carview: hot stage world: %s\n", app.error.c_str());
              app.status = 1;
              return false;
            }
            pump_hot_frame(0.98f);
          }
          if (app.have_spawn) {
            start = {app.spawn[0], app.spawn[1], app.spawn[2]};
            start_heading = app.spawn[3];
          }
        }
        const double hot_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_hot0).count();
        std::printf("carview: hot swap to %s in %.1f ms (car %s, regather %s, world textures kept: %zu, gpu draws: %u)\n",
                    app.car.c_str(), hot_ms, need_car_reload ? "reloaded" : "kept", need_regather ? "yes" : "no",
                    showroom.world_texture_count(), showroom.gpu_resident_draws());
  return true;
}

void DriveWorldHost::run() {
  if (!open_world_and_car()) {
    std::lock_guard<std::mutex> lock(g_loaded_cars_mutex);
    g_loaded_cars.erase(this);
    return;
  }
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
  {
    std::lock_guard<std::mutex> lock(g_loaded_cars_mutex);
    g_loaded_cars.erase(this);
  }
  if (app.validation) std::printf("validation messages: %u\n", renderer.validation_messages());
  if (app.status == 0 && renderer.validation_messages()) app.status = 3;
}

}  // namespace carview
