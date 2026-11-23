#include "drive_cli.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "career_page.h"
#include "cars/sources.h"
#include "drive/cameras.h"
#include "drive_helpers.h"
#include "drive_helpers_runtime.h"
#include "game_hud.h"
#include "hud.h"
#include "platform_support.h"
#include "settings.h"
#include "ui.h"

namespace carview {

void drive_usage() {
  std::fprintf(stderr,
               "usage: carview [options]   (the drive; the default mode)\n"
               "  where:   --play <dir> (play folder: play.txt, bake/, game) | --bake <dir> --world <colorado dir>\n"
               "           --car <media> --hour h --spawn x y z heading --near x z --cache file\n"
               "           --save file | --no-save --import-profile <export folder> (once; then\n"
               "           --import-profile-again <folder> replaces the career)\n"
               "  settings: --save-settings (keeps this run's look, sound, traffic and race options in the\n"
               "           user folder's settings.txt; the window reads it first) --no-settings\n"
               "  mods:    --mods <dir> (default: the user folder's mods/, the window only) --no-mods\n"
               "  look:    --size WxH --msaa N --preset low|high --no-post --bloom | --no-bloom --exposure x\n"
               "           --shadows off|low|high --view chase|front --hud --no-reverse-z --language code\n"
               "  world:   --render-ground --flat-world --no-props --no-far --no-trees --async-stream\n"
               "  traffic: --traffic | --no-traffic --traffic-seed N\n"
               "  sound:   --sound <dir> | --no-sound --no-radio --radio-press s (headless: D-pad right every\n"
               "           s seconds); in the window , and . (D-pad left and right) change the station\n"
               "  race:    --event <id> (FR22, FR16, ...): the event's race on its route, grid and opponents;\n"
               "           --difficulty easy|med|hard|pro --no-rubberband --race-auto (the AI drives the\n"
               "           player's car; the default headless) --race-seed N\n"
               "           --race-replay <csv> (the player on an oracle path by race time; the M5 gate)\n"
               "  free roam: --reveal (unfound signs and barns on the map) --distance m (added to the total\n"
               "           driven) --no-collectibles; M opens the map (+ - zoom, arrows pan), Back on a pad\n"
               "  report:  F12 in the window saves carview-pose-N.txt (car, position, heading, hour, camera;\n"
               "           its spawn line goes back in as --spawn x y z heading); the title shows the position\n"
               "  debug:   --contacts-log file (hard body contacts; walls with nothing drawn near)\n"
               "  scripted and headless: --run name | --script file --speed kmh --seconds N --every S\n"
               "           --headless --out prefix.png --audio-out file.wav --bench --world-reference file.json\n"
               "           --bench-scenario noon_cruise|midnight_lights|cockpit_mirrors|race_pack|autoshow_studio|ui4_heavy|cold_boot_switch\n"
               "           --bench-json file.json --bench-csv file.csv --bench-full-fx --bench-audio --bench-warmup sec\n"
               "           --menu-png file.png --map-png prefix (the pause map and the minimap at the end)\n"
               "  ray tracing: --rt off|hybrid|pt|photo-pt --rt-bounces 1..4 --rt-spp 1..16 --rt-denoise|--no-rt-denoise\n"
               "  other modes: carview --bake <dir> --car <media> (showroom), carview --game-root <dir> (disc viewer),\n"
               "           carview --world ... (world viewer), carview --xds-png ...\n");
}

bool DriveConfig::parse(int argc, char** argv, int& exit_code) {
  exit_code = 0;
  {
    bool window = true, use_settings = true;
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--headless" || a == "--bench" || a == "--bench-scenario" || a == "--menu-png" || a == "--help" || a == "-h") window = false;
      if (a == "--no-settings") use_settings = false;
    }
    if ((window || std::getenv("SHIFT_SETTINGS")) && use_settings) {
      std::vector<std::string> notes;
      args = carview::read_settings(carview::settings_path(), &notes);
      for (const auto& n : notes) std::fprintf(stderr, "carview: note: %s\n", n.c_str());
      if (!args.empty()) std::printf("settings: %s (%zu tokens)\n", carview::settings_path().string().c_str(), args.size());
    }
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  }
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string a = args[i];
    auto next = [&]() -> const char* { return i + 1 < args.size() ? args[++i].c_str() : ""; };
    if (a == "--drive" || a == "--no-settings") continue;
    if (a == "--bench-scenario") {
      const std::string sc_name = next();
      if (!carview::bench::parse_scenario(sc_name, bench_scenario)) {
        std::fprintf(stderr, "carview: unknown --bench-scenario '%s'\n", sc_name.c_str());
        exit_code = 2; return false;
      }
      bench_scenario_set = true;
      bench = headless = async_stream = true;
      continue;
    }
    if (a == "--bench-json") { bench_json_path = next(); continue; }
    if (a == "--bench-csv") { bench_csv_path = next(); continue; }
    if (a == "--bench-full-fx") { bench_full_fx = true; continue; }
    if (a == "--bench-audio") { bench_audio = true; continue; }
    if (a == "--bench-warmup") {
      bench_warmup_sec = std::max(0.0, std::atof(next()));
      bench_warmup_explicit = true;
      continue;
    }
    if (a == "--save-settings") { save_settings = true; continue; }
    if (a == "--no-title") { no_title = true; continue; }
    if (a == "--skip-intro-movies") { skip_intro_movies = true; continue; }
    if (a == "--no-skip-intro-movies") { skip_intro_movies = false; continue; }
    if (a == "--start-in-single-player") { start_in_single_player = true; continue; }
    if (a == "--no-start-in-single-player") { start_in_single_player = false; continue; }
    if (a == "--skip-vo-with-a") { skip_vo_with_a = true; continue; }
    if (a == "--no-skip-vo-with-a") { skip_vo_with_a = false; continue; }
    if (a == "--units") {
      const std::string u = next();
      metric_units = !(u == "imperial" || u == "mph" || u == "IMP" || u == "MPH");
      continue;
    }
    if (a == "--story-intro") { story_intro_cli = true; continue; }
    if (a == "--campaign-stage") { cli_campaign_stage = next(); continue; }
    if (a == "--fmv") { cli_fmv_path = next(); continue; }
    if (a == "--mods") { mods_root = next(); mods_off = false; continue; }
    if (a == "--no-mods") { mods_off = true; continue; }
    if (a == "--mods-native") { mods_native = true; continue; }
    if (a == "--dev-hints") { cli_dev_hints = true; continue; }
    if (a == "--sync-folder") { sync_folder = next(); continue; }
    if (a == "--device-name") { device_name = next(); continue; }
    if (a == "--profile") { profile_name = next(); continue; }
    if (a == "--headless") headless = true;
    else if (a == "--bake") bake_dir = next();
    else if (a == "--car") car = next();
    else if (a == "--bench-car-switch") bench_switch_car = next();
    else if (a == "--out") out = next();
    else if (a == "--run") run = next();
    else if (a == "--view") view = next();
    else if (a == "--script") script_path = next();
    else if (a == "--seconds") seconds = std::atof(next());
    else if (a == "--window-frames") window_frames_limit = uint32_t(std::atoi(next()));
    else if (a == "--every") every = std::atof(next());
    else if (a == "--world") world_dir = next();
    else if (a == "--game") game_root_given = next();
    else if (a == "--cache") cache_path = next();
    else if (a == "--speed") cruise_kmh = std::atof(next()), cruise_kmh_explicit = true;
    else if (a == "--render-ground") render_ground = true;
    else if (a == "--flat-world") flat_world = true;
    else if (a == "--no-props") no_props = true;
    else if (a == "--no-far") no_far = true;
    else if (a == "--no-trees") no_trees = true;
    else if (a == "--bench") bench = headless = async_stream = true;  // streams like the window
    else if (a == "--msaa") msaa = uint32_t(std::atoi(next()));
    else if (a == "--preset") {
      const std::string p = next();
      preset_name = p;
      carview::GraphicsPresetSpec pspec;
      if (carview::resolve_graphics_preset(p, pspec)) {
        msaa = pspec.msaa;
        low_preset = pspec.low_preset;
        preset_reflection_rate = pspec.env_map_rate;
        preset_anisotropy = pspec.anisotropy;
        if (shadows_flag.empty()) shadows_flag = pspec.shadows;
        if (bloom_flag < 0) bloom_flag = pspec.bloom ? 1 : 0;
      } else {
        msaa = p == "low" ? 1 : 4;
        low_preset = p == "low";
        preset_reflection_rate = low_preset ? 0.5f : 1.0f;
        preset_anisotropy = low_preset ? 2.0f : 16.0f;
      }
    }
    else if (a == "--touch-controls") {
      carview::TouchVisibility tv;
      if (carview::parse_touch_visibility(next(), tv)) touch_controls.set_visibility(tv);
    }
    else if (a == "--touch-steer") {
      carview::TouchSteerMode sm;
      if (carview::parse_touch_steer_mode(next(), sm)) touch_controls.set_steer_mode(sm);
    }
    else if (a == "--android-gpu-driver") {
      android_gpu_driver = next();
    }
    else if (a == "--no-post") post_on = false;
    else if (a == "--shadows") {
      shadows_flag = next();
      if (shadows_flag != "off" && shadows_flag != "low" && shadows_flag != "high") {
        std::fprintf(stderr, "carview --drive: --shadows takes off, low or high\n");
        exit_code = 2; return false;
      }
    }
    else if (a == "--bloom") bloom_flag = 1;
    else if (a == "--no-bloom") bloom_flag = 0;
    else if (a == "--motion-blur") motion_blur_flag = 1;
    else if (a == "--no-motion-blur") motion_blur_flag = 0;
    else if (a == "--dof") dof_flag = 1;
    else if (a == "--no-dof") dof_flag = 0;
    else if (a == "--photo-through-walls") photo_through_walls_cli = true;
    else if (a == "--photo-wider-limits") photo_wider_limits_cli = true;
    else if (a == "--demo-photo-mode") demo_photo_png = next(), headless = true;
    else if (a == "--demo-replay-view") demo_replay_png = next(), headless = true;
    else if (a == "--demo-achievements") demo_achievements_png = next(), headless = true;
    else if (a == "--demo-touch-hud") demo_touch_hud_png = next(), headless = true;
    else if (a == "--demo-upgrade-shop") demo_upgrade_shop_png = next(), headless = true;
    else if (a == "--exposure") exposure_given = std::atof(next());
    else if (a == "--world-reference") world_reference_path = next();
    else if (a == "--hud") hud = true;
    else if (a == "--sound") sound_dir = next(), sound_given = true;
    else if (a == "--play") play_dir = next();
    else if (carview::AudioConfig::parse_arg(args, i, audio_cfg)) {
      sound = audio_cfg.sound;
      radio_on = audio_cfg.radio_on;
    }
    else if (a == "--event") race_event = next();
    else if (a == "--difficulty") {
      if (!race::parse_difficulty(next(), race_options.difficulty)) {
        std::fprintf(stderr, "carview: --difficulty easy|med|hard|pro|hard+\n");
        exit_code = 2; return false;
      }
    } else if (a == "--ai-mode") {
      if (!race::parse_ai_mode(next(), race_options.ai_mode)) {
        std::fprintf(stderr, "carview: --ai-mode stock|fair|hard\n");
        exit_code = 2; return false;
      }
      if (race_options.ai_mode != race::AiMode::kStock) race_options.rubberband = false;
    } else if (a == "--no-rubberband") {
      race_options.rubberband = false;
      if (race_options.ai_mode == race::AiMode::kStock) race_options.ai_mode = race::AiMode::kFair;
    }
    else if (a == "--race-auto") race_auto = 1;
    else if (a == "--race-replay") race_options.replay = next();
    else if (a == "--race-seed") race_options.seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--radio-press") radio_press_every = std::atof(next());
    else if (a == "--traffic") traffic_flag = 1;
    else if (a == "--no-traffic") traffic_flag = 0;
    else if (a == "--demo-speed-trap") demo_speed_trap = true;
    else if (a == "--demo-h2h") demo_h2h = true;
    else if (a == "--demo-rally-stage") demo_rally_stage = true;
    else if (a == "--demo-rally-service") demo_rally_service = true;
    else if (a == "--demo-club") demo_club = true;
    else if (a == "--hour") {
      hour = std::fmod(std::fmod(std::atof(next()), 24.0) + 24.0, 24.0);
      hour_explicit = true;
    } else if (a == "--tod-preset") {
      if (!hour_explicit) hour = std::fmod(std::fmod(std::atof(next()), 24.0) + 24.0, 24.0);
      else (void)next();
    } else if (a == "--day-length") {
      const std::string dl = next();
      if (dl == "15m") day_length_scale = 2.0;
      else if (dl == "60m") day_length_scale = 0.5;
      else if (dl == "realtime") day_length_scale = 1827.9 / 86400.0;
      else if (dl == "frozen") day_length_scale = 0.0;
      else day_length_scale = 1.0;
    }
    else if (a == "--traffic-seed") traffic_seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--audio-out") audio_out = next();
    else if (a == "--async-stream") async_stream = true;
    else if (a == "--contacts-log") contacts_log_path = next();
    else if (a == "--texture-census") texture_census = true;
    else if (a == "--route") gather_route = std::atoi(next()), gather_route_given = true;
    else if (a == "--camera") camera_mode = std::atoi(next());
    else if (a == "--language") language = next();
    else if (a == "--no-reverse-z") reverse_z = false;
    else if (a == "--no-cull") backface_cull = false;
    else if (a == "--z-prepass") z_prepass = true;
    else if (a == "--near") {
      near_x = std::atof(next());
      near_z = std::atof(next());
      near_given = true;
    } else if (a == "--spawn") {
      for (double& v : spawn) v = std::atof(next());
      have_spawn = true;
      cli_spawn_given = true;
    } else if (a == "--title-screen") {
      cli_title_screen = true;
    } else if (a == "--title-menu") {
      cli_title_screen = true;
      cli_title_menu = true;
    } else if (a == "--save") {
      save_path = next();
    } else if (a == "--no-save") {
      no_save = true;
    } else if (a == "--import-profile") {
      import_dir = next();
    } else if (a == "--import-profile-again") {
      import_dir = next();
      import_again = true;
    } else if (a == "--menu-png") {
      menu_png = next();
    } else if (a == "--no-collectibles") {
      collectibles_on = false;
    } else if (a == "--reveal") {
      reveal = true;
    } else if (a == "--treasure-map") {
      treasure_map_setting = true;
    } else if (a == "--no-treasure-map") {
      treasure_map_setting = false;
    } else if (a == "--gps-to" && i + 2 < args.size()) {
      cli_gps_x = std::atof(args[++i].c_str());
      cli_gps_z = std::atof(args[++i].c_str());
      has_cli_gps = true;
    } else if (a == "--pr-stunt") {
      cli_pr_stunt = next();
    } else if (a == "--outpost-hub") {
      cli_outpost_hub = next();
    } else if (a == "--pause-map-png") {
      pause_map_png = next();
    } else if (a == "--map-zoom") {
      cli_map_zoom = std::clamp(std::atof(next()), 1.0, 24.0);
    } else if (a == "--distance") {
      seed_distance = std::atof(next());
    } else if (a == "--map-png") {
      map_png = next();
    } else if (a == "--career-png") {
      career_png = next();
    }
    else if (a == "--size") {
      if (!parse_size_arg(next(), width, height)) {
        exit_code = 2;
        return false;
      }
      size_given = true;
    } else if (carview::modern::apply_graphics_option(cli_graphics, args, i)) {
      if (a == "--ssao") cli_ssao_explicit = true;
      else if (a == "--contact-shadows") cli_sscs_explicit = true;
      else if (a == "--hex-tiling" || a == "--no-hex-tiling") cli_hextile_explicit = true;
      else if (a == "--velocity-blur") cli_velblur_explicit = true;
      else if (a == "--shadow-pcss") cli_pcss_explicit = true;
      else if (a == "--ssr") cli_ssr_explicit = true;
      else if (a == "--volumetric-fog") cli_volfog_explicit = true;
      else if (a == "--car-detail" || a == "--no-car-detail") cli_cardetail_explicit = true;
    } else if (a == "--hdr-pattern") {
      cli_hdr_pattern = uint32_t(std::clamp(std::atoi(next()), 0, 3));
    } else if (a == "--hdr-pattern-nits") {
      cli_hdr_pattern_nits = std::clamp(float(std::atof(next())), 80.0f, 10000.0f);
    } else if (a == "--perf-overlay") {
      cli_perf_overlay = true;
    } else if (a == "--frame-inspector") {
      cli_frame_inspector = true;
    } else if (a == "--inspector-view") {
      cli_inspector_view = carview::parse_inspector_view(next());
    } else if (a == "--help" || a == "-h") {
      drive_usage();
      exit_code = 0; return false;
    } else {
      std::fprintf(stderr, "carview: unknown option %s\n", a.c_str());
      drive_usage();
      exit_code = 2; return false;
    }
  }
  // Apply scalable Phase A + B SOTA defaults per graphics preset unless explicitly overridden via CLI/settings.
  {
    carview::GraphicsPresetSpec pspec;
    if (carview::resolve_graphics_preset(preset_name, pspec)) {
      if (!cli_ssao_explicit) cli_graphics.ssao = pspec.gtao;
      if (!cli_sscs_explicit) cli_graphics.contact_shadows = pspec.contact_shadows;
      if (!cli_hextile_explicit) cli_graphics.hex_tiling = pspec.hex_tiling;
      if (!cli_velblur_explicit) cli_graphics.velocity_blur = pspec.velocity_blur;
      if (!cli_pcss_explicit) cli_graphics.shadow_pcss = pspec.shadow_pcss;
      if (!cli_ssr_explicit) cli_graphics.ssr = pspec.ssr;
      if (!cli_volfog_explicit) cli_graphics.volumetric_fog = pspec.volumetric_fog;
      if (!cli_cardetail_explicit) cli_graphics.car_detail = pspec.car_detail;
    }
  }
  // Apply deterministic defaults for named benchmark scenarios (--bench-scenario).
  if (bench_scenario_set) {
    using S = carview::bench::Scenario;
    switch (bench_scenario) {
      case S::kNoonCruise:
        if (!hour_explicit) hour = 12.0;
        bench_full_fx = true;
        hud = true;
        if (traffic_flag < 0) traffic_flag = 1;
        if (!cruise_kmh_explicit) cruise_kmh = 85.0;
        if (!bench_warmup_explicit) bench_warmup_sec = std::min(1.0, seconds * 0.25);
        break;
      case S::kMidnightLights:
        if (!hour_explicit) hour = 2.0;
        bench_full_fx = true;
        hud = true;
        if (traffic_flag < 0) traffic_flag = 1;
        if (!cruise_kmh_explicit) cruise_kmh = 80.0;
        if (!bench_warmup_explicit) bench_warmup_sec = std::min(1.0, seconds * 0.25);
        break;
      case S::kCockpitMirrors:
        if (!hour_explicit) hour = 12.0;
        camera_mode = int(drive::CameraMode::kCockpit);
        bench_full_fx = true;
        hud = true;
        if (traffic_flag < 0) traffic_flag = 1;
        if (!cruise_kmh_explicit) cruise_kmh = 80.0;
        if (!bench_warmup_explicit) bench_warmup_sec = std::min(1.0, seconds * 0.25);
        break;
      case S::kRacePack:
        if (race_event.empty()) race_event = "FR22";
        if (race_auto < 0) race_auto = 1;
        bench_full_fx = true;
        hud = true;
        if (!bench_warmup_explicit) bench_warmup_sec = std::min(1.0, seconds * 0.25);
        break;
      case S::kAutoshowStudio:
        bench_full_fx = true;
        hud = true;
        if (traffic_flag < 0) traffic_flag = 0;
        if (!bench_warmup_explicit) bench_warmup_sec = std::min(0.5, seconds * 0.2);
        break;
      case S::kUi4Heavy:
        bench_full_fx = true;
        hud = true;
        if (traffic_flag < 0) traffic_flag = 0;
        if (!bench_warmup_explicit) bench_warmup_sec = std::min(0.5, seconds * 0.2);
        break;
      case S::kColdBootSwitch:
        bench_full_fx = true;
        hud = true;
        if (traffic_flag < 0) traffic_flag = 1;
        if (!bench_warmup_explicit) bench_warmup_sec = std::min(0.5, seconds * 0.2);
        break;
      case S::kCustom:
        break;
    }
    if (!size_given) width = 1280, height = 720;
  }
  if (bench && bench_full_fx) {
    hud = true;
    bench_audio = true;
  }
  if (save_settings) {
    std::string e;
    const auto file = carview::settings_path();
    if (file.empty() || !carview::write_settings(file, args, &e)) {
      std::fprintf(stderr, "carview: --save-settings: %s\n", file.empty() ? "no user folder" : e.c_str());
      exit_code = 1; return false;
    }
    std::printf("settings: saved %s\n", file.string().c_str());
  }
  return true;
}

int run_headless_menu_dumps(const DriveConfig& cfg, UiFontCache& font_cache) {
    {
      const char* g = std::getenv("SHIFT_GAME_ROOT");
      load_ui_fonts(!cfg.game_root_given.empty() ? cfg.game_root_given : std::filesystem::path(g ? g : ""), font_cache);
    }
    // The HUD with the pause menu open, the second item chosen, at scale 3.
    HudState st;
    st.kmh = 87;
    st.gear = 3;
    st.rpm = 5400;
    st.redline_rpm = 8000;
    st.banner = "NIS_SkylineGTRVSpecII_02 - [ ] pick a car  R back on the road";
    PauseMenu menu;
    menu.show();
    menu.move(1);
    menu.fill(st);
    // SHIFT_MENU_SAMPLE=shop or autoshow: those panels' longest lines.
    carview::CareerBoard sample_board;
    bool has_board = false;
    if (const char* sample = std::getenv("SHIFT_MENU_SAMPLE")) {
      st.menu.clear();
      if (std::string(sample) == "shop") {
        st.menu_title = "upgrades  S 700  4,075,645 CR  sign discount 100";
        for (const char* l : {"pistons compression  [level 3]  owned", "transmission  [stock 0]  fitted",
                              "tire width front  [level 11]  12,500 CR", "weight reduction  [level 3]  fitted"})
          st.menu.push_back(l);
      } else if (std::string(sample) == "career") {
        carview::CareerSummary cs;
        cs.level_name = "Orange", cs.wristband_points = 41250, cs.credits = 4075645, cs.total_winnings = 13225340;
        cs.races = 87, cs.victories = 52, cs.podiums = 74, cs.garage_cars = 38;
        cs.club_cars = 12, cs.club_done = 9, cs.club_total = 30, cs.has_rally = true, cs.rallies = 3, cs.rally_stages = 9;
        carview::CareerPage page;
        page.open(cs, cars::list_packs(std::getenv("SHIFT_STATE_ROOT") ? std::getenv("SHIFT_STATE_ROOT") : ""));
        page.move(6);
        int sel = 0;
        page.fill(st.menu_title, st.menu, sel);
        auto& b = sample_board;
        b.band_name = "ORANGE WRISTBAND", b.band_next = "Purple", b.points = 41250, b.band_lo = 30000, b.band_hi = 60000;
        b.credits = cs.credits, b.winnings = cs.total_winnings, b.races = 87, b.wins = 52, b.podiums = 74;
        b.garage = 38, b.signs = 64, b.barns = 9, b.barns_total = 9, b.club_done = 9, b.club_total = 30;
        b.rally = true, b.rally_level = 4, b.rally_xp = 18200, b.rallies = 3, b.rally_wins = 2;
        b.packs_owned = page.packs_owned(), b.packs_total = page.packs_total();
        has_board = true;
      } else {
        st.menu_title = "autoshow  137/137  available 13,225,340 CR";
        for (const char* l : {"R1 999  FER_F40Competizione_89  1,000,000 CR", "B 437  MIT_LancerEvoXGSR_08  52,000 CR"})
          st.menu.push_back(l);
      }
      st.menu_selected = 1;
    }
    if (!write_menu_png(st, cfg.menu_png.string(), has_board ? &sample_board : nullptr)) return 1;
    {
      // Beside it, <name>_hud.png: free roam's HUD with a discovery card.
      carview::DriveHud d;
      d.kmh = 87, d.rpm = 5200, d.redline_rpm = 7500, d.gear = 3, d.time = 1.5;
      d.credits = 1300, d.band_name = "YELLOW WRISTBAND", d.band_points = 50, d.band_hi = 220;
      d.popup_title = "DISCOUNT SIGN 12/100", d.popup_text = "UPGRADES 12% OFF", d.popup_at = 0;
      d.popup_progress = 0.12;
      carview::ui::List list;
      list.rect(0, 0, 1280, 720, carview::ui::rgba(70, 92, 120), 0, carview::ui::rgba(150, 128, 96));
      carview::build_drive_hud(list, d, 1280, 720);
      std::vector<uint8_t> rgba;
      carview::ui::rasterize(list.items(), rgba, 1280, 720);
      auto hud_png = cfg.menu_png;
      hud_png.replace_filename(cfg.menu_png.stem().string() + "_hud.png");
      if (!write_png(hud_png.string(), 1280, 720, 4, rgba.data())) return 1;
      std::printf("hud: 1280x720 to %s\n", hud_png.string().c_str());
    }
    {
      // And <name>_race.png: a race's HUD mid-race, driving the wrong way.
      carview::RaceHud r;
      r.phase = carview::RaceHud::kRacing, r.event = "SAMPLE", r.circuit = true, r.lap = 2, r.laps = 4;
      r.place = 3, r.cars = 8, r.clock = 74.2, r.best_lap = 40.07, r.wrong_way = true;
      for (int i = 1; i <= 8; ++i)
        r.rows.push_back({i == 3 ? "YOU" : "OPPONENT " + std::to_string(i), i, i == 3, false, -1});
      carview::DriveHud d;
      d.kmh = 42, d.rpm = 3100, d.redline_rpm = 7500, d.gear = 2, d.time = 0.1, d.race = &r;
      carview::ui::List list;
      list.rect(0, 0, 1280, 720, carview::ui::rgba(70, 92, 120), 0, carview::ui::rgba(150, 128, 96));
      carview::build_drive_hud(list, d, 1280, 720);
      std::vector<uint8_t> rgba;
      carview::ui::rasterize(list.items(), rgba, 1280, 720);
      auto race_png = cfg.menu_png;
      race_png.replace_filename(cfg.menu_png.stem().string() + "_race.png");
      if (!write_png(race_png.string(), 1280, 720, 4, rgba.data())) return 1;
      std::printf("race: 1280x720 to %s\n", race_png.string().c_str());
    }
    for (int page = 0; page < 4; ++page) {
      // And <name>_results<page>.png: the FINISHED banner, the cash reward,
      // then (2) the RACE RESULTS table.
      carview::RaceHud r;
      r.phase = carview::RaceHud::kFinished, r.event = "SAMPLE", r.circuit = true, r.laps = 4;
      r.place = 6, r.cars = 8, r.clock = 186, r.finish_time = 182, r.credits = 2500, r.wristband = 400;
      r.results_page = page == 2 ? 0 : page == 3 ? 2 : page, r.results_table = page == 2, r.balance = 4078145;
      if (page == 2) {
        const char* who[] = {"Alan Brewer", "Marko Baran", "Peter Bankov", "User", "Don Funk", "Jason Zander", "Scott Mitchell", "Bjorn Rettig"};
        const char* cars[] = {"Lancer Evo.6 GSR", "Range Rover'12", "Jeep SRT8 '12", "Nissan GT-R R34", "Nissan GT-R R34", "Audi Quattro", "Nissan GT-R R34", "Audi RS 4"};
        const int pis[] = {510, 511, 509, 461, 508, 507, 508, 507};
        const double best[] = {38.109, 37.472, 38.337, 39.960, 39.375, 39.173, 40.033, 39.864};
        const double total[] = {162.187, 162.562, 164.211, 164.961, 165.682, 166.026, 166.391, 167.381};
        for (int i = 0; i < 8; ++i) {
          carview::RaceHudRow row;
          row.name = who[i], row.place = i + 1, row.player = i == 3, row.finished = true, row.time = total[i];
          row.car = cars[i], row.car_class = i == 3 ? "B" : "A", row.pi = pis[i], row.best_lap = best[i];
          r.rows.push_back(row);
        }
      }
      r.band_before = 41250 - 400, r.band_after = 41250, r.band_lo = 30000, r.band_hi = 60000;
      r.band_name = "Orange", r.band_next = "Purple";
      r.base_credits = 2000, r.mileage_bonus = 200, r.difficulty_pct = 15, r.difficulty_bonus = 330, r.credits = 2530;
      if (page == 3) r.band_name = "Blue", r.band_up = true;
      carview::DriveHud d;
      d.race = &r, d.time = 10;
      carview::ui::List list;
      list.rect(0, 0, 1280, 720, carview::ui::rgba(70, 92, 120), 0, carview::ui::rgba(150, 128, 96));
      carview::build_drive_hud(list, d, 1280, 720);
      std::vector<uint8_t> rgba;
      carview::ui::rasterize(list.items(), rgba, 1280, 720);
      auto res_png = cfg.menu_png;
      res_png.replace_filename(cfg.menu_png.stem().string() + "_results" + std::to_string(page) + ".png");
      if (!write_png(res_png.string(), 1280, 720, 4, rgba.data())) return 1;
      std::printf("results: 1280x720 to %s\n", res_png.string().c_str());
    }
    // Extended UI audit renders for visual parity verification against oracle frames.
    auto dump_hud = [&](const std::string& suffix, const carview::DriveHud& d) {
      carview::ui::List list;
      list.rect(0, 0, 1280, 720, carview::ui::rgba(58, 80, 108), 0, carview::ui::rgba(146, 122, 92));
      carview::build_drive_hud(list, d, 1280, 720);
      std::vector<uint8_t> rgba;
      carview::ui::rasterize(list.items(), rgba, 1280, 720);
      auto out_p = cfg.menu_png;
      out_p.replace_filename(cfg.menu_png.stem().string() + suffix + ".png");
      if (!write_png(out_p.string(), 1280, 720, 4, rgba.data())) return false;
      std::printf("audit: 1280x720 to %s\n", out_p.string().c_str());
      return true;
    };
    {
      carview::TitleScreenHud ts;
      ts.active = true, ts.menu_phase = false;
      carview::DriveHud d;
      d.title_screen = &ts, d.time = 0.0;
      if (!dump_hud("_title_splash", d)) return 1;
      ts.menu_phase = true, ts.has_save = true;
      ts.menu = {"CONTINUE CAREER", "NEW CAREER", "FREE ROAM", "SETTINGS", "QUIT"};
      ts.selected = 0;
      ts.save_band_name = "GREEN WRISTBAND", ts.save_band_points = 410, ts.save_pop_rank = 224;
      ts.save_car_name = "1995 Volkswagen Corrado VR6", ts.save_car_class = "D", ts.save_car_pi = 285;
      ts.save_credits = 18500, ts.save_races = 5, ts.save_wins = 3;
      if (!dump_hud("_title_menu", d)) return 1;
    }
    {
      carview::WorldMarkerHud wm;
      wm.visible = true, wm.in_range = true, wm.screen_x = 580, wm.screen_y = 260;
      wm.title = "RECARO RED ROCK RAMPAGE", wm.subtitle = "FESTIVAL RACE  •  CLASS B OR LOWER";
      wm.prompt = "ENTER NOW", wm.badge = "NEW", wm.eligible = true;
      carview::SkillChainHud sk;
      sk.active = true, sk.chain_points = 750, sk.multiplier = 2, sk.last_skill = "GREAT DRIFT", sk.last_points = 250;
      sk.popularity_rank = 238, sk.popularity_progress = 0.65;
      carview::DriveHud d;
      d.kmh = 114, d.rpm = 5600, d.redline_rpm = 7500, d.gear = 3, d.time = 2.0;
      d.world_marker = &wm, d.skills = &sk;
      if (!dump_hud("_freeroam_marker", d)) return 1;
    }
    {
      carview::RaceHud r;
      r.phase = carview::RaceHud::kGrid, r.event = "RECARO RED ROCK RAMPAGE", r.event_type = "FESTIVAL RACE";
      r.grid_menu_open = true;
      r.grid_menu = {"START RACE", "< DIFFICULTY: MED (+15%) >", "QUIT RACE"};
      r.grid_selected = 0;
      r.grid_car_info = "1995 Volkswagen Corrado VR6  [D 285]";
      carview::DriveHud d;
      d.race = &r, d.time = 1.0;
      if (!dump_hud("_race_grid", d)) return 1;
    }
    {
      carview::RaceHud r;
      r.phase = carview::RaceHud::kFinished, r.results_page = 3, r.won_car_name = "1997 Mazda RX-7";
      carview::DriveHud d;
      d.race = &r, d.time = 10.0;
      if (!dump_hud("_won_car", d)) return 1;
    }
    {
      carview::CeremonyHud c;
      c.active = true, c.headline = "NEW WRISTBAND EARNED!", c.band_name = "GREEN WRISTBAND";
      c.band_colour = "GREEN", c.subtitle = "7 NEW FESTIVAL EVENTS UNLOCKED!";
      c.bonus_car = "1997 Mazda RX-7";
      carview::DriveHud d;
      d.ceremony = &c, d.time = 2.0;
      if (!dump_hud("_ceremony", d)) return 1;
    }
    std::printf("menu: 1280x720 to %s\n", cfg.menu_png.string().c_str());
    return 0;
}

}  // namespace carview
