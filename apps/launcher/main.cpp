// Shift Engine Unified Cross-Platform GUI Launcher (SDL3 + Vulkan / CPU headless).
#include <SDL3/SDL.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <fh1data/archive.h>
#include <fh1data/xds.h>

#include "launcher_core.h"
#include "launcher_ui.h"
#include "renderer.h"
#include "ui.h"
#include "ui4_chrome.h"

namespace {

bool load_launcher_fh1_assets(const std::filesystem::path& game_root) {
  if (game_root.empty()) return false;
  std::error_code ec;
  const std::filesystem::path fonts_zip = game_root / "media" / "ui" / "Fonts.zip";
  if (!std::filesystem::is_regular_file(fonts_zip, ec)) return false;
  std::string why;
  auto zip = fh1::Archive::open(fonts_zip, &why);
  if (!zip) return false;
  const bool loaded = carview::ui::load_fonts_from_zip(*zip);
  carview::ui4::set_texture_loader([root = game_root](const std::string& path, std::array<float, 4>& uv) {
    if (carview::ui::atlas_find(path, uv)) return true;
    static std::unique_ptr<fh1::Archive> horizon_zip;
    static std::unique_ptr<fh1::Archive> tex_zip;
    static std::set<std::string> failed;
    static bool opened = false;
    if (failed.count(path)) return false;
    if (!opened) {
      opened = true;
      std::string e;
      horizon_zip = fh1::Archive::open(root / "media" / "ui" / "textures" / "Horizon.zip", &e);
      tex_zip = fh1::Archive::open(root / "media" / "ui" / "Textures.zip", &e);
    }
    const bool t = path.rfind("T:", 0) == 0;
    fh1::Archive* a = t ? tex_zip.get() : horizon_zip.get();
    fh1::ReadResult r = a ? a->read((t ? path.substr(2) : path) + ".xds") : fh1::ReadResult{};
    const fh1::XdsResult x = a && r.ok() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
    if (x.levels.empty() ||
        !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
      failed.insert(path);
      return false;
    }
    return true;
  });
  return loaded;
}

void upload_launcher_ui_atlas(carview::Renderer& renderer, bool full) {
  const auto& a = carview::ui::atlas_pixels();
  if (full) {
    carview::ui::atlas_take_dirty();
    renderer.ui_atlas_put(0, 0, carview::ui::kAtlasSize, carview::ui::kAtlasSize, a);
    return;
  }
  std::vector<uint8_t> rows;
  for (const auto& [x, y, w, h] : carview::ui::atlas_take_dirty()) {
    rows.resize(size_t(w) * h * 4);
    for (uint32_t r = 0; r < h; ++r) {
      std::memcpy(&rows[size_t(r) * w * 4], &a[(size_t(y + r) * carview::ui::kAtlasSize + x) * 4], size_t(w) * 4);
    }
    renderer.ui_atlas_put(x, y, w, h, rows);
  }
}

void print_usage() {
  std::printf(
      "shift-launcher: Unified Cross-Platform GUI Launcher for Shift Engine\n"
      "  shift-launcher [--portable] [--game <dir>] [--tab play|setup|dlc|mods|radio|wheel|update|android]\n"
      "  shift-launcher --screenshot <out.png> [--game <dir>] [--tab <name>] [--width 1280] [--height 720] [--demo]\n");
}

void refresh_all(launcher::LauncherUiState& st) {
  launcher::read_play_config(st.paths.play_dir, st.play_cfg);
  if (!st.play_cfg.game_root.empty() && st.disc_path_input.empty()) {
    st.disc_path_input = st.play_cfg.game_root.string();
  }
  st.disc_check = launcher::verify_disc_folder(st.disc_path_input, st.paths.play_dir);
  st.content_status = launcher::scan_content_packages(st.paths.state_dir, st.paths.mods_dir);
  st.mods = launcher::scan_mods(st.paths.mods_dir);
  st.save_iso = launcher::evaluate_save_isolation(st.mods);
  if (st.selected_mod >= int(st.mods.size())) st.selected_mod = std::max(0, int(st.mods.size()) - 1);
  st.wheel_cfg = launcher::load_wheel_settings(st.paths.settings_file);
  const auto ips = launcher::discover_local_lan_ips();
  if (!ips.empty()) st.sync_lan_ip = ips.front();
  st.sync_manifest = launcher::build_wifi_sync_manifest(
      st.paths, st.sync_include_play, st.sync_include_dlc, st.sync_include_mods, st.sync_include_radio);
  st.sync_qr = launcher::generate_qr_matrix(
      "http://" + st.sync_lan_ip + ":" + std::to_string(st.sync_port) + "/" + st.sync_manifest.token);
}

bool launch_carview(const launcher::LauncherUiState& st, std::string& notice) {
  if (st.paths.carview_bin.empty()) {
    notice = "carview binary not found beside shift-launcher.";
    return false;
  }
  static constexpr const char* kPresetArgs[5] = {"high", "low", "smooth-60", "quality-30", "low-power"};
  std::string cmd = "\"" + st.paths.carview_bin.string() + "\" --drive";
  if (!st.paths.play_dir.empty()) cmd += " --play \"" + st.paths.play_dir.string() + "\"";
  cmd += std::string(" --preset ") + kPresetArgs[std::clamp(st.selected_preset, 0, 4)];
  if (!st.launch_car.empty()) cmd += " --car " + st.launch_car;
  if (!st.extra_args.empty()) cmd += " " + st.extra_args;
#ifdef _WIN32
  cmd = "start \"\" " + cmd;
#else
  cmd += " &";
#endif
  std::system(cmd.c_str());
  notice = "Launched carview (" + std::string(kPresetArgs[std::clamp(st.selected_preset, 0, 4)]) + " preset).";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool force_portable = false;
  bool demo_mode = false;
  std::filesystem::path screenshot_out;
  std::filesystem::path game_root_override;
  launcher::LauncherTab start_tab = launcher::LauncherTab::kPlay;
  uint32_t width = 1280;
  uint32_t height = 720;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    } else if (a == "--portable") {
      force_portable = true;
    } else if (a == "--demo") {
      demo_mode = true;
    } else if (a == "--game" && i + 1 < argc) {
      game_root_override = argv[++i];
    } else if (a == "--screenshot" && i + 1 < argc) {
      screenshot_out = argv[++i];
    } else if (a == "--tab" && i + 1 < argc) {
      launcher::parse_tab_name(argv[++i], start_tab);
    } else if (a == "--width" && i + 1 < argc) {
      width = uint32_t(std::max(640, std::atoi(argv[++i])));
    } else if (a == "--height" && i + 1 < argc) {
      height = uint32_t(std::max(360, std::atoi(argv[++i])));
    }
  }

  std::filesystem::path exe_dir;
  if (argc > 0 && argv[0]) {
    std::error_code ec;
    exe_dir = std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec).parent_path();
  }

  launcher::LauncherUiState state;
  state.active_tab = start_tab;
  state.paths = launcher::resolve_launcher_paths(exe_dir, force_portable);
  state.hw_info = launcher::probe_hardware(60);
  state.hw_rec = launcher::recommend_preset(state.hw_info);
  refresh_all(state);
  if (demo_mode) launcher::populate_demo_state(state);

  // Load real FH1 vector fonts & distressed UI textures whenever the game root is available.
  {
    std::vector<std::filesystem::path> candidates;
    if (!game_root_override.empty()) candidates.push_back(game_root_override);
    if (!state.play_cfg.game_root.empty()) candidates.push_back(state.play_cfg.game_root);
    if (!state.disc_path_input.empty()) candidates.push_back(state.disc_path_input);
    if (const char* env_root = std::getenv("SHIFT_GAME_ROOT"); env_root && *env_root) {
      candidates.emplace_back(env_root);
    }
    for (const auto& root : candidates) {
      if (load_launcher_fh1_assets(root)) break;
    }
  }

  // Headless verification screenshot mode (no window or display server needed).
  if (!screenshot_out.empty()) {
    carview::ui::List ui_list;
    std::vector<launcher::HitBox> hitboxes;
    launcher::build_launcher_ui(ui_list, state, float(width), float(height), hitboxes);
    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4u, 255);
    carview::ui::rasterize(ui_list.items(), rgba, width, height);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    std::error_code ec;
    if (screenshot_out.has_parent_path()) std::filesystem::create_directories(screenshot_out.parent_path(), ec);
    if (!stbi_write_png(screenshot_out.string().c_str(), int(width), int(height), 4, rgba.data(), int(width * 4))) {
      std::fprintf(stderr, "shift-launcher: failed to write %s\n", screenshot_out.string().c_str());
      return 1;
    }
    std::printf("shift-launcher: rendered %s (%ux%u, tab=%s, %zu shapes)\n",
                screenshot_out.string().c_str(), width, height, launcher::tab_name(state.active_tab),
                ui_list.items().size());
    return 0;
  }

  // Interactive SDL3 + Vulkan window loop.
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "shift-launcher: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  // Query display refresh rate for Hardware Advisor.
  if (SDL_DisplayID disp = SDL_GetPrimaryDisplay()) {
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(disp); mode && mode->refresh_rate > 0) {
      state.hw_info.refresh_hz = int(mode->refresh_rate + 0.5f);
      state.hw_rec = launcher::recommend_preset(state.hw_info);
    }
  }

  SDL_Window* window = SDL_CreateWindow("Shift Engine Launcher", int(width), int(height),
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::fprintf(stderr, "shift-launcher: SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  carview::Renderer renderer;
  carview::Renderer::Options ropts;
  ropts.window = window;
  ropts.msaa = 1;
  std::string rerr;
  if (!renderer.init(ropts, rerr)) {
    std::fprintf(stderr, "shift-launcher: Vulkan renderer init failed: %s\n", rerr.c_str());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // Open connected SDL3 joysticks / wheels for live axis preview.
  std::vector<SDL_Joystick*> open_joys;
  auto rescan_joys = [&]() {
    for (SDL_Joystick* j : open_joys) SDL_CloseJoystick(j);
    open_joys.clear();
    state.devices.clear();
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
      for (int i = 0; i < count; ++i) {
        if (SDL_Joystick* j = SDL_OpenJoystick(ids[i])) {
          open_joys.push_back(j);
          launcher::WheelDeviceInfo info;
          info.instance_id = int(ids[i]);
          info.name = SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "USB Controller";
          info.num_axes = SDL_GetNumJoystickAxes(j);
          info.num_buttons = SDL_GetNumJoystickButtons(j);
          info.is_gamepad = SDL_IsGamepad(ids[i]);
          info.has_ffb = info.num_axes >= 2;
          state.devices.push_back(std::move(info));
        }
      }
      SDL_free(ids);
    }
  };
  rescan_joys();

  launcher::SetupRunner setup_runner;
  launcher::WifiSyncServer wifi_server;
  carview::ui::List ui_list;
  std::vector<launcher::HitBox> hitboxes;

  auto handle_click = [&](const std::string& id) {
    if (id.rfind("tab:", 0) == 0) {
      launcher::LauncherTab t;
      if (launcher::parse_tab_name(id.substr(4), t)) state.active_tab = t;
    } else if (id.rfind("preset:", 0) == 0) {
      state.selected_preset = std::clamp(std::atoi(id.c_str() + 7), 0, 4);
    } else if (id == "act:play") {
      launch_carview(state, state.banner_notice);
    } else if (id == "act:steam") {
      const auto r = launcher::add_to_steam(state.paths);
      state.banner_notice = r.message;
    } else if (id == "act:portable") {
      std::error_code ec;
      const auto marker = state.paths.launcher_dir / "portable.txt";
      if (state.paths.portable) {
        std::filesystem::remove(marker, ec);
        state.paths = launcher::resolve_launcher_paths(exe_dir, false);
        state.banner_notice = "Switched to Standard user-folder mode.";
      } else {
        std::ofstream f(marker);
        f << "portable\n";
        state.paths = launcher::resolve_launcher_paths(exe_dir, true);
        state.banner_notice = "Enabled Portable Mode (data stored beside launcher).";
      }
      refresh_all(state);
    } else if (id == "act:apply_rec") {
      state.selected_preset = launcher::preset_index_for_name(state.hw_rec.carview_preset);
      state.banner_notice = "Applied recommended preset: " + state.hw_rec.label;
    } else if (id == "setup:verify") {
      state.disc_check = launcher::verify_disc_folder(state.disc_path_input, state.paths.play_dir);
      state.banner_notice = state.disc_check.summary;
    } else if (id == "setup:toggle_sounds") {
      state.setup_all_sounds = !state.setup_all_sounds;
    } else if (id == "setup:toggle_index") {
      state.setup_build_index = !state.setup_build_index;
    } else if (id == "setup:run") {
      launcher::SetupOptions sopts;
      sopts.game_root = state.disc_path_input;
      sopts.state_root = state.paths.state_dir;
      sopts.play_dir = state.paths.play_dir;
      sopts.all_sounds = state.setup_all_sounds;
      sopts.build_index = state.setup_build_index;
      if (!setup_runner.start(state.paths, sopts)) state.banner_notice = "Setup is already running.";
    } else if (id == "dlc:rescan") {
      state.content_status = launcher::scan_content_packages(state.paths.state_dir, state.paths.mods_dir);
      state.banner_notice = "Rescanned DLC / v4 / UDLC / XE packages.";
    } else if (id == "mods:rescan") {
      state.mods = launcher::scan_mods(state.paths.mods_dir);
      state.save_iso = launcher::evaluate_save_isolation(state.mods);
    } else if (id.rfind("mod:sel:", 0) == 0) {
      state.selected_mod = std::atoi(id.c_str() + 8);
    } else if (id.rfind("mod:tog:", 0) == 0) {
      const size_t idx = size_t(std::atoi(id.c_str() + 8));
      launcher::toggle_mod_enabled(state.mods, idx);
      state.save_iso = launcher::evaluate_save_isolation(state.mods);
    } else if (id == "mod:up") {
      if (state.selected_mod > 0 && launcher::move_mod_order(state.mods, size_t(state.selected_mod), -1))
        --state.selected_mod;
    } else if (id == "mod:down") {
      if (state.selected_mod + 1 < int(state.mods.size()) &&
          launcher::move_mod_order(state.mods, size_t(state.selected_mod), +1))
        ++state.selected_mod;
    } else if (id.rfind("mod:opt:", 0) == 0) {
      const size_t oi = size_t(std::atoi(id.c_str() + 8));
      if (state.selected_mod >= 0 && size_t(state.selected_mod) < state.mods.size()) {
        auto& mod = state.mods[size_t(state.selected_mod)];
        if (oi < mod.options.size() && !mod.options[oi].choices.empty()) {
          auto& opt = mod.options[oi];
          size_t cur = 0;
          for (size_t ci = 0; ci < opt.choices.size(); ++ci) {
            if (opt.choices[ci].value == opt.value) cur = ci;
          }
          const auto& nxt = opt.choices[(cur + 1) % opt.choices.size()];
          launcher::set_mod_option_value(mod, opt.key, nxt.value);
        }
      }
    } else if (id.rfind("radio:slot:", 0) == 0) {
      state.radio_cfg.station_slot = id.substr(11);
    } else if (id == "radio:mode:replace") {
      state.radio_cfg.replace_playlist = true;
    } else if (id == "radio:mode:extend") {
      state.radio_cfg.replace_playlist = false;
    } else if (id == "radio:build") {
      const auto r = launcher::build_custom_radio_mod(state.radio_cfg, state.play_cfg.game_root,
                                                      state.paths.mods_dir, state.paths.ffmpeg_bin);
      state.radio_status = r.message;
      state.banner_notice = r.message;
    } else if (id.rfind("wheel:dev:", 0) == 0) {
      state.selected_device = std::atoi(id.c_str() + 10);
    } else if (id == "wheel:separate") {
      state.wheel_cfg.separate_devices = !state.wheel_cfg.separate_devices;
    } else if (id == "wheel:degrees") {
      static constexpr int kDegs[4] = {270, 540, 900, 1080};
      int next = 900;
      for (int i = 0; i < 4; ++i)
        if (kDegs[i] == state.wheel_cfg.steering_degrees) next = kDegs[(i + 1) % 4];
      state.wheel_cfg.steering_degrees = next;
    } else if (id == "wheel:ffb_minus") {
      state.wheel_cfg.ffb_gain = std::max(0.0f, state.wheel_cfg.ffb_gain - 0.05f);
    } else if (id == "wheel:ffb_plus") {
      state.wheel_cfg.ffb_gain = std::min(1.0f, state.wheel_cfg.ffb_gain + 0.05f);
    } else if (id == "wheel:ffb_invert") {
      state.wheel_cfg.ffb_invert = !state.wheel_cfg.ffb_invert;
    } else if (id == "wheel:ffb_test") {
      if (state.selected_device >= 0 && size_t(state.selected_device) < open_joys.size()) {
        const uint16_t amp = uint16_t(std::clamp(state.wheel_cfg.ffb_gain, 0.1f, 1.0f) * 45000.0f);
        SDL_RumbleJoystick(open_joys[size_t(state.selected_device)], amp, amp, 350);
        state.wheel_status = "Sent 350 ms FFB/rumble test pulse.";
      } else {
        state.wheel_status = "Connect a controller or wheel to test FFB pulse.";
      }
    } else if (id == "wheel:dz_down") {
      state.wheel_cfg.steer.deadzone_in = std::max(0.0f, state.wheel_cfg.steer.deadzone_in - 0.01f);
    } else if (id == "wheel:dz_up") {
      state.wheel_cfg.steer.deadzone_in = std::min(0.30f, state.wheel_cfg.steer.deadzone_in + 0.01f);
    } else if (id == "wheel:save") {
      std::string err;
      if (launcher::save_wheel_settings(state.paths.settings_file, state.wheel_cfg, &err)) {
        state.wheel_status = "Saved wheel & deadzone settings to " + state.paths.settings_file.filename().string();
      } else {
        state.wheel_status = err;
      }
    } else if (id == "upd:channel") {
      state.update_channel = (state.update_channel == "stable") ? "preview" : "stable";
    } else if (id == "upd:check") {
#ifdef _WIN32
      constexpr const char* kAssetName = "shift-engine-windows-x64.zip";
#elif defined(__APPLE__)
      constexpr const char* kAssetName = "shift-engine-macos-arm64.dmg";
#else
      constexpr const char* kAssetName = "shift-engine-linux-x64.tar.gz";
#endif
      const auto ur = launcher::check_github_updates(state.current_version, state.update_channel, kAssetName);
      state.update_status = ur.status;
      state.banner_notice = ur.status;
    } else if (id == "sync:play") {
      state.sync_include_play = !state.sync_include_play;
    } else if (id == "sync:dlc") {
      state.sync_include_dlc = !state.sync_include_dlc;
    } else if (id == "sync:mods") {
      state.sync_include_mods = !state.sync_include_mods;
    } else if (id == "sync:radio") {
      state.sync_include_radio = !state.sync_include_radio;
    } else if (id == "sync:toggle") {
      if (wifi_server.running()) {
        wifi_server.stop();
        state.sync_active = false;
      } else {
        state.sync_manifest = launcher::build_wifi_sync_manifest(
            state.paths, state.sync_include_play, state.sync_include_dlc, state.sync_include_mods,
            state.sync_include_radio);
        if (wifi_server.start(state.sync_manifest)) {
          state.sync_active = true;
          state.sync_port = wifi_server.port();
          state.sync_qr = launcher::generate_qr_matrix(wifi_server.download_url(state.sync_lan_ip));
        } else {
          state.banner_notice = "Could not bind Wi-Fi Sync server socket.";
        }
      }
    }
  };

  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) {
        running = false;
      } else if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_ESCAPE) running = false;
        else if (e.key.key >= SDLK_1 && e.key.key <= SDLK_8) {
          state.active_tab = launcher::LauncherTab(e.key.key - SDLK_1);
        } else if (e.key.key == SDLK_TAB) {
          state.active_tab = launcher::LauncherTab((int(state.active_tab) + 1) % int(launcher::LauncherTab::kCount));
        } else if (e.key.key == SDLK_RETURN && state.active_tab == launcher::LauncherTab::kPlay) {
          launch_carview(state, state.banner_notice);
        }
      } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
        state.mouse_x = e.motion.x;
        state.mouse_y = e.motion.y;
        state.hovered_id.clear();
        for (const auto& hb : hitboxes) {
          if (hb.contains(state.mouse_x, state.mouse_y)) state.hovered_id = hb.id;
        }
      } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        for (const auto& hb : hitboxes) {
          if (hb.contains(e.button.x, e.button.y)) {
            handle_click(hb.id);
            break;
          }
        }
      } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
        const std::filesystem::path dropped(e.drop.data);
        if (state.active_tab == launcher::LauncherTab::kMods) {
          launcher::install_mod_source(dropped, state.paths.mods_dir, state.banner_notice);
          state.mods = launcher::scan_mods(state.paths.mods_dir);
          state.save_iso = launcher::evaluate_save_isolation(state.mods);
        } else if (state.active_tab == launcher::LauncherTab::kDlc) {
          launcher::import_content_package(dropped, state.paths.state_dir, state.paths.mods_dir, state.banner_notice);
          state.content_status = launcher::scan_content_packages(state.paths.state_dir, state.paths.mods_dir);
        } else if (state.active_tab == launcher::LauncherTab::kRadio) {
          if (dropped.extension() == ".png" || dropped.extension() == ".PNG") {
            state.radio_cfg.logo_png_path = dropped;
            state.radio_logo_check = launcher::validate_station_logo_png(dropped);
          } else {
            state.radio_cfg.music_folder = dropped;
            state.radio_tracks = launcher::scan_radio_music_folder(dropped);
          }
        } else {
          state.disc_path_input = dropped.string();
          state.disc_check = launcher::verify_disc_folder(dropped, state.paths.play_dir);
          state.banner_notice = state.disc_check.summary;
          state.active_tab = launcher::LauncherTab::kSetup;
        }
      } else if (e.type == SDL_EVENT_JOYSTICK_ADDED || e.type == SDL_EVENT_JOYSTICK_REMOVED) {
        rescan_joys();
      }
    }

    // Poll async setup runner & Wi-Fi sync activity + live wheel axes
    state.setup_progress = setup_runner.snapshot();
    if (wifi_server.running()) state.sync_activity = wifi_server.recent_activity();
    if (!open_joys.empty() && state.selected_device >= 0 && size_t(state.selected_device) < open_joys.size()) {
      SDL_Joystick* j = open_joys[size_t(state.selected_device)];
      auto ax = [&](int idx) {
        return (idx >= 0 && idx < SDL_GetNumJoystickAxes(j)) ? float(SDL_GetJoystickAxis(j, idx)) / 32767.0f : 0.0f;
      };
      state.live_steer = ax(std::max(0, state.wheel_cfg.steer.axis_index));
      state.live_throttle = std::clamp((ax(std::max(1, state.wheel_cfg.throttle.axis_index)) + 1.0f) * 0.5f, 0.0f, 1.0f);
      state.live_brake = std::clamp((ax(std::max(2, state.wheel_cfg.brake.axis_index)) + 1.0f) * 0.5f, 0.0f, 1.0f);
    }

    int win_w = int(width), win_h = int(height);
    SDL_GetWindowSize(window, &win_w, &win_h);
    launcher::build_launcher_ui(ui_list, state, float(win_w), float(win_h), hitboxes);
    static bool atlas_synced = false;
    upload_launcher_ui_atlas(renderer, !atlas_synced);
    atlas_synced = true;
    renderer.set_ui(ui_list.items());
    std::string ferr;
    if (!renderer.draw_frame(carview::FrameConstants{}, ferr)) break;
    SDL_Delay(16);
  }

  wifi_server.stop();
  for (SDL_Joystick* j : open_joys) SDL_CloseJoystick(j);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
