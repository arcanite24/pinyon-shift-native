// Shift Engine Launcher (SDL3 + Vulkan / CPU headless screenshot mode).
#include <SDL3/SDL.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <fh1data/archive.h>

#include "launcher_core.h"
#include "launcher_font.h"
#include "launcher_ui.h"
#include "renderer.h"
#include "ui.h"

namespace {

bool load_launcher_fh1_fonts_fallback(const std::filesystem::path& game_root) {
  if (game_root.empty()) return false;
  std::error_code ec;
  const std::filesystem::path fonts_zip = game_root / "media" / "ui" / "Fonts.zip";
  if (!std::filesystem::is_regular_file(fonts_zip, ec)) return false;
  std::string why;
  auto zip = fh1::Archive::open(fonts_zip, &why);
  if (!zip) return false;
  return carview::ui::load_fonts_from_zip(*zip);
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
      "shift-launcher: Shift Engine launcher\n"
      "  shift-launcher [--portable] [--game <dir>] [--tab play|setup|content|mods|radio|controls|updates|android]\n"
      "  shift-launcher --screenshot <out.png> [--game <dir>] [--tab <name>] [--width 1280] [--height 720] [--scale 1.0] [--demo]\n");
}

void open_folder_in_shell(const std::filesystem::path& dir, std::string& notice) {
  if (dir.empty()) {
    notice = "Play folder path is not set.";
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", dir.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
  std::string cmd = "open \"" + dir.string() + "\" &";
  std::system(cmd.c_str());
#else
  std::string cmd = "xdg-open \"" + dir.string() + "\" >/dev/null 2>&1 &";
  std::system(cmd.c_str());
#endif
  notice = "Opened " + dir.string();
}

std::string clean_pasted_path(std::string_view raw) {
  size_t a = 0;
  while (a < raw.size() && std::isspace(static_cast<unsigned char>(raw[a]))) ++a;
  size_t b = raw.size();
  while (b > a && std::isspace(static_cast<unsigned char>(raw[b - 1]))) --b;
  std::string out(raw.substr(a, b - a));
  if (out.size() >= 2 && ((out.front() == '"' && out.back() == '"') || (out.front() == '\'' && out.back() == '\''))) {
    out = out.substr(1, out.size() - 2);
  }
  out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
  out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
  return out;
}

void pop_utf8_char(std::string& s) {
  if (s.empty()) return;
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0u) == 0x80u) s.pop_back();
  if (!s.empty()) s.pop_back();
}

enum class DialogKind { kBrowseIso, kBrowseFolder, kImportStfs, kImportContentFolder };

struct PendingDialogResult {
  DialogKind kind = DialogKind::kBrowseIso;
  std::vector<std::filesystem::path> paths;
  std::string error;
  bool cancelled = false;
};

struct DialogQueue {
  std::mutex mu;
  std::vector<PendingDialogResult> items;
};

struct DialogContext {
  DialogQueue* queue = nullptr;
  DialogKind kind = DialogKind::kBrowseIso;
};

void SDLCALL on_file_dialog_complete(void* userdata, const char* const* filelist, int /*filter*/) {
  std::unique_ptr<DialogContext> ctx(static_cast<DialogContext*>(userdata));
  if (!ctx || !ctx->queue) return;
  PendingDialogResult res;
  res.kind = ctx->kind;
  if (!filelist) {
    res.error = SDL_GetError() ? SDL_GetError() : "File dialog failed";
  } else if (!filelist[0]) {
    res.cancelled = true;
  } else {
    for (int i = 0; filelist[i] != nullptr; ++i) {
      if (*filelist[i]) res.paths.emplace_back(filelist[i]);
    }
  }
  std::lock_guard<std::mutex> lk(ctx->queue->mu);
  ctx->queue->items.push_back(std::move(res));
}

void refresh_all(launcher::LauncherUiState& st) {
  launcher::read_play_config(st.paths.play_dir, st.play_cfg);
  if (!st.play_cfg.game_root.empty() && st.disc_path_input.empty()) {
    st.disc_path_input = st.play_cfg.game_root.string();
  }
  st.disc_check = launcher::verify_disc_folder(st.disc_path_input, st.paths.play_dir);
  st.content_status = launcher::scan_content_packages(st.paths.state_dir, st.paths.mods_dir, st.paths.play_dir);
  st.mods = launcher::scan_mods(st.paths.mods_dir);
  st.save_iso = launcher::evaluate_save_isolation(st.mods);
  if (st.selected_mod >= int(st.mods.size())) st.selected_mod = std::max(0, int(st.mods.size()) - 1);
  st.wheel_cfg = launcher::load_wheel_settings(st.paths.settings_file);
  {
    const std::string be = launcher::load_graphics_backend_setting(st.paths.settings_file);
    const auto choices = launcher::available_backend_choices();
    st.selected_backend = 0;
    for (int i = 0; i < int(choices.size()); ++i) {
      if (be == choices[size_t(i)].id) {
        st.selected_backend = i;
        break;
      }
    }
  }
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
  const auto& res = launcher::kResolutionPresets[std::clamp(st.selected_resolution, 0, 4)];
  if (res.width > 0 && res.height > 0) {
    cmd += " --width " + std::to_string(res.width) + " --height " + std::to_string(res.height);
  }
  if (st.launch_fullscreen) cmd += " --fullscreen";
  {
    const auto backends = launcher::available_backend_choices();
    if (!backends.empty()) {
      const auto& choice = backends[size_t(std::clamp(st.selected_backend, 0, int(backends.size()) - 1))];
      if (std::strcmp(choice.id, "auto") != 0) {
        cmd += std::string(" --backend ") + choice.id;
      }
    }
  }
  if (!st.launch_car.empty()) cmd += " --car " + st.launch_car;
  if (!st.extra_args.empty()) cmd += " " + st.extra_args;
#ifdef _WIN32
  std::string prev_state_root;
  bool had_prev_state = false;
  if (!st.paths.state_dir.empty()) {
    char buf[4096];
    const DWORD n = GetEnvironmentVariableA("SHIFT_STATE_ROOT", buf, DWORD(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) {
      prev_state_root.assign(buf, n);
      had_prev_state = true;
    }
    SetEnvironmentVariableA("SHIFT_STATE_ROOT", st.paths.state_dir.string().c_str());
  }
  std::string cmd_line = cmd;
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const BOOL created = CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!st.paths.state_dir.empty()) {
    SetEnvironmentVariableA("SHIFT_STATE_ROOT", had_prev_state ? prev_state_root.c_str() : nullptr);
  }
  if (!created) {
    notice = "Failed to launch carview.";
    return false;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
#else
  if (!st.paths.state_dir.empty()) {
    cmd = "SHIFT_STATE_ROOT=\"" + st.paths.state_dir.string() + "\" " + cmd;
  }
  cmd += " &";
  std::system(cmd.c_str());
#endif
  notice = "Launched carview (" + std::string(kPresetArgs[std::clamp(st.selected_preset, 0, 4)]) + " preset).";
  return true;
}

void scroll_active_tab(launcher::LauncherUiState& st, int delta) {
  if (delta == 0) return;
  switch (st.active_tab) {
    case launcher::LauncherTab::kSetup:
      st.setup_log_scroll = std::max(0, st.setup_log_scroll + delta);
      break;
    case launcher::LauncherTab::kDlc:
      st.content_scroll = std::max(0, st.content_scroll - delta);
      break;
    case launcher::LauncherTab::kMods:
      st.mods_scroll = std::max(0, st.mods_scroll - delta);
      break;
    case launcher::LauncherTab::kRadio:
      st.radio_scroll = std::max(0, st.radio_scroll - delta);
      break;
    case launcher::LauncherTab::kWheel:
      st.devices_scroll = std::max(0, st.devices_scroll - delta);
      break;
    case launcher::LauncherTab::kAndroid:
      st.sync_log_scroll = std::max(0, st.sync_log_scroll + delta);
      break;
    default: break;
  }
}

void cycle_focus(launcher::LauncherUiState& st, const std::vector<launcher::HitBox>& hitboxes, int dir) {
  std::vector<std::string> ids;
  ids.reserve(hitboxes.size());
  for (const auto& hb : hitboxes) {
    if (hb.focusable && !hb.id.empty()) ids.push_back(hb.id);
  }
  if (ids.empty()) {
    st.focused_id.clear();
    return;
  }
  auto it = std::find(ids.begin(), ids.end(), st.focused_id);
  if (it == ids.end()) {
    st.focused_id = dir >= 0 ? ids.front() : ids.back();
  } else {
    const int cur = int(it - ids.begin());
    const int next = (cur + dir + int(ids.size())) % int(ids.size());
    st.focused_id = ids[size_t(next)];
  }
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
  float headless_scale = 1.0f;

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
    } else if (a == "--scale" && i + 1 < argc) {
      headless_scale = std::clamp(float(std::atof(argv[++i])), 0.75f, 3.0f);
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
  if (!game_root_override.empty()) {
    state.disc_path_input = game_root_override.string();
    state.disc_check = launcher::verify_disc_folder(game_root_override, state.paths.play_dir);
    state.banner_notice = state.disc_check.summary;
  }

  // Initialize native system TrueType fonts first; fall back to FH1 Fonts.zip if unavailable.
  if (!launcher::init_launcher_fonts()) {
    std::vector<std::filesystem::path> candidates;
    if (!game_root_override.empty()) candidates.push_back(game_root_override);
    if (!state.play_cfg.game_root.empty()) candidates.push_back(state.play_cfg.game_root);
    if (!state.disc_path_input.empty()) candidates.push_back(state.disc_path_input);
    if (const char* env_root = std::getenv("SHIFT_GAME_ROOT"); env_root && *env_root) {
      candidates.emplace_back(env_root);
    }
    for (const auto& root : candidates) {
      if (load_launcher_fh1_fonts_fallback(root)) break;
    }
  }

  // Headless verification screenshot mode (no window or display server needed).
  if (!screenshot_out.empty()) {
    carview::ui::List ui_list;
    std::vector<launcher::HitBox> hitboxes;
    launcher::build_launcher_ui(ui_list, state, float(width), float(height), hitboxes, headless_scale);
    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4u, 255);
    carview::ui::rasterize(ui_list.items(), rgba, width, height);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    std::error_code ec;
    if (screenshot_out.has_parent_path()) std::filesystem::create_directories(screenshot_out.parent_path(), ec);
    if (!stbi_write_png(screenshot_out.string().c_str(), int(width), int(height), 4, rgba.data(), int(width * 4))) {
      std::fprintf(stderr, "shift-launcher: failed to write %s\n", screenshot_out.string().c_str());
      return 1;
    }
    std::printf("shift-launcher: rendered %s (%ux%u, scale=%.2f, tab=%s, %zu shapes)\n",
                screenshot_out.string().c_str(), width, height, double(headless_scale),
                launcher::tab_name(state.active_tab), ui_list.items().size());
    return 0;
  }

  // Interactive SDL3 + Vulkan window loop.
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "shift-launcher: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  // Query display refresh rate and HiDPI content scale so 4K / high-DPI monitors
  // open at a comfortable physical size with crisp scaled typography.
  float init_content_scale = 1.0f;
  int init_win_w = int(width);
  int init_win_h = int(height);
  if (SDL_DisplayID disp = SDL_GetPrimaryDisplay()) {
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(disp); mode && mode->refresh_rate > 0) {
      state.hw_info.refresh_hz = int(mode->refresh_rate + 0.5f);
      state.hw_rec = launcher::recommend_preset(state.hw_info);
    }
    const float cs = SDL_GetDisplayContentScale(disp);
    if (cs > 1.0f) init_content_scale = std::clamp(cs, 1.0f, 3.0f);
#if !defined(__APPLE__)
    SDL_Rect usable{};
    const bool has_usable = SDL_GetDisplayUsableBounds(disp, &usable) && usable.w > 0 && usable.h > 0;
    float win_scale = init_content_scale;
    if (has_usable && win_scale <= 1.01f && usable.w >= 2560 && usable.h >= 1440) {
      win_scale = std::clamp(std::min(float(usable.w) / 1920.0f, float(usable.h) / 1080.0f), 1.0f, 2.5f);
    }
    if (win_scale > 1.0f) {
      init_win_w = int(std::lround(float(width) * win_scale));
      init_win_h = int(std::lround(float(height) * win_scale));
    }
    if (has_usable) {
      init_win_w = std::min(init_win_w, std::max(960, usable.w - 64));
      init_win_h = std::min(init_win_h, std::max(600, usable.h - 64));
    }
#endif
  }

  SDL_Window* window = SDL_CreateWindow("Shift Engine", init_win_w, init_win_h,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    std::fprintf(stderr, "shift-launcher: SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  const float min_scale = (SDL_GetWindowPixelDensity(window) <= 1.01f) ? std::max(1.0f, init_content_scale) : 1.0f;
  SDL_SetWindowMinimumSize(window, std::min(init_win_w, int(std::lround(960.0f * min_scale))),
                           std::min(init_win_h, int(std::lround(600.0f * min_scale))));

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
  launcher::ContentInstallRunner content_runner;
  launcher::WifiSyncServer wifi_server;
  DialogQueue dialog_queue;
  bool was_setup_running = false;
  bool was_content_running = false;
  bool text_input_active = false;
  carview::ui::List ui_list;
  std::vector<launcher::HitBox> hitboxes;

  auto sync_text_input = [&]() {
    const bool want = (state.active_tab == launcher::LauncherTab::kSetup && state.focused_id == "setup:path");
    if (want && !text_input_active) {
      SDL_StartTextInput(window);
      text_input_active = true;
    } else if (!want && text_input_active) {
      SDL_StopTextInput(window);
      text_input_active = false;
    }
  };

  auto apply_disc_source = [&](const std::filesystem::path& p) {
    state.disc_path_input = clean_pasted_path(p.string());
    state.disc_check = launcher::verify_disc_folder(state.disc_path_input, state.paths.play_dir);
    state.banner_notice = state.disc_check.summary;
    state.active_tab = launcher::LauncherTab::kSetup;
  };

  auto start_content_import = [&](const std::vector<std::filesystem::path>& items) {
    if (items.empty()) return;
    state.active_tab = launcher::LauncherTab::kDlc;
    if (!content_runner.start(items, state.paths.state_dir, state.paths.mods_dir, state.paths.play_dir)) {
      state.banner_notice = "Content installation is already running.";
    } else {
      state.banner_notice = "Installing " + std::to_string(items.size()) + " content package(s)...";
    }
  };

  static constexpr SDL_DialogFileFilter kIsoFilters[2] = {
      {"Xbox 360 ISO Images", "iso;xiso"},
      {"All Files", "*"},
  };
  static constexpr SDL_DialogFileFilter kStfsFilters[1] = {
      {"Xbox 360 STFS Packages", "*"},
  };

  auto handle_click = [&](const std::string& id) {
    if (id.rfind("tab:", 0) == 0) {
      launcher::LauncherTab t;
      if (launcher::parse_tab_name(id.substr(4), t)) {
        state.active_tab = t;
        state.focused_id.clear();
      }
    } else if (id.rfind("preset:", 0) == 0) {
      state.selected_preset = std::clamp(std::atoi(id.c_str() + 7), 0, 4);
    } else if (id.rfind("res:", 0) == 0) {
      state.selected_resolution = std::clamp(std::atoi(id.c_str() + 4), 0, 4);
    } else if (id.rfind("backend:", 0) == 0) {
      const auto backends = launcher::available_backend_choices();
      if (!backends.empty()) {
        state.selected_backend = std::clamp(std::atoi(id.c_str() + 8), 0, int(backends.size()) - 1);
        const auto& choice = backends[size_t(state.selected_backend)];
        launcher::save_graphics_backend_setting(state.paths.settings_file, choice.id);
        state.banner_notice = std::string("Graphics backend set to ") + choice.label + ".";
      }
    } else if (id == "act:fullscreen") {
      state.launch_fullscreen = !state.launch_fullscreen;
    } else if (id == "act:play") {
      launch_carview(state, state.banner_notice);
    } else if (id == "act:open_play") {
      open_folder_in_shell(state.paths.play_dir, state.banner_notice);
    } else if (id == "act:steam") {
      const auto r = launcher::add_to_steam(state.paths);
      state.banner_notice = r.message;
    } else if (id == "act:portable") {
      std::error_code ec;
      const auto marker = state.paths.launcher_dir / "portable.txt";
      if (state.paths.portable) {
        std::filesystem::remove(marker, ec);
        state.paths = launcher::resolve_launcher_paths(exe_dir, false);
        state.banner_notice = "Switched to standard user folder.";
      } else {
        std::ofstream f(marker);
        f << "portable\n";
        state.paths = launcher::resolve_launcher_paths(exe_dir, true);
        state.banner_notice = "Portable mode enabled.";
      }
      refresh_all(state);
    } else if (id == "act:apply_rec") {
      state.selected_preset = launcher::preset_index_for_name(state.hw_rec.carview_preset);
      state.banner_notice = "Selected recommended preset: " + state.hw_rec.label;
    } else if (id == "setup:path") {
      state.focused_id = "setup:path";
    } else if (id == "setup:browse_iso") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseIso};
      SDL_ShowOpenFileDialog(on_file_dialog_complete, ctx, window, kIsoFilters, 2, nullptr, false);
    } else if (id == "setup:browse_folder") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kBrowseFolder};
      SDL_ShowOpenFolderDialog(on_file_dialog_complete, ctx, window, nullptr, false);
    } else if (id == "setup:verify") {
      state.disc_path_input = clean_pasted_path(state.disc_path_input);
      state.disc_check = launcher::verify_disc_folder(state.disc_path_input, state.paths.play_dir);
      state.banner_notice = state.disc_check.summary;
    } else if (id == "setup:toggle_sounds") {
      state.setup_all_sounds = !state.setup_all_sounds;
    } else if (id == "setup:toggle_index") {
      state.setup_build_index = !state.setup_build_index;
    } else if (id == "setup:toggle_log") {
      state.setup_log_expanded = !state.setup_log_expanded;
    } else if (id == "setup:cancel") {
      setup_runner.cancel();
      state.banner_notice = "Cancelling setup...";
    } else if (id == "setup:run") {
      state.disc_path_input = clean_pasted_path(state.disc_path_input);
      launcher::SetupOptions sopts;
      sopts.game_root = state.disc_path_input;
      sopts.state_root = state.paths.state_dir;
      sopts.play_dir = state.paths.play_dir;
      sopts.all_sounds = state.setup_all_sounds;
      sopts.build_index = state.setup_build_index;
      if (!setup_runner.start(state.paths, sopts)) state.banner_notice = "Setup is already running.";
    } else if (id == "dlc:rescan") {
      state.content_status = launcher::scan_content_packages(state.paths.state_dir, state.paths.mods_dir, state.paths.play_dir);
      state.banner_notice = "Rescanned content packages.";
    } else if (id == "dlc:import") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kImportStfs};
      SDL_ShowOpenFileDialog(on_file_dialog_complete, ctx, window, kStfsFilters, 1, nullptr, true);
    } else if (id == "dlc:import_folder") {
      auto* ctx = new DialogContext{&dialog_queue, DialogKind::kImportContentFolder};
      SDL_ShowOpenFolderDialog(on_file_dialog_complete, ctx, window, nullptr, true);
    } else if (id == "dlc:cancel") {
      content_runner.cancel();
      state.banner_notice = "Cancelling content installation...";
    } else if (id == "mods:rescan") {
      state.mods = launcher::scan_mods(state.paths.mods_dir);
      state.save_iso = launcher::evaluate_save_isolation(state.mods);
    } else if (id == "mods:install") {
      state.banner_notice = "Drag and drop a mod folder or .zip archive onto the window to install.";
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
        state.wheel_status = "Sent 350 ms rumble test pulse.";
      } else {
        state.wheel_status = "Connect a controller or wheel to test.";
      }
    } else if (id == "wheel:dz_down") {
      state.wheel_cfg.steer.deadzone_in = std::max(0.0f, state.wheel_cfg.steer.deadzone_in - 0.01f);
    } else if (id == "wheel:dz_up") {
      state.wheel_cfg.steer.deadzone_in = std::min(0.30f, state.wheel_cfg.steer.deadzone_in + 0.01f);
    } else if (id == "wheel:save") {
      std::string err;
      if (launcher::save_wheel_settings(state.paths.settings_file, state.wheel_cfg, &err)) {
        state.wheel_status = "Saved settings to " + state.paths.settings_file.filename().string();
      } else {
        state.wheel_status = err;
      }
    } else if (id == "upd:ch:stable") {
      state.update_channel = "stable";
    } else if (id == "upd:ch:preview") {
      state.update_channel = "preview";
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
      state.available_update = ur.update;
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

  auto current_pixel_scale = [&]() -> std::pair<float, float> {
    int win_w = int(width), win_h = int(height);
    int px_w = int(width), px_h = int(height);
    SDL_GetWindowSize(window, &win_w, &win_h);
    SDL_GetWindowSizeInPixels(window, &px_w, &px_h);
    const float sx = win_w > 0 ? float(px_w) / float(win_w) : 1.0f;
    const float sy = win_h > 0 ? float(px_h) / float(win_h) : 1.0f;
    return {sx, sy};
  };

  float user_zoom = 1.0f;
  bool running = true;
  while (running) {
    const auto [scale_x, scale_y] = current_pixel_scale();

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) {
        running = false;
      } else if (e.type == SDL_EVENT_TEXT_INPUT) {
        if (state.active_tab == launcher::LauncherTab::kSetup && state.focused_id == "setup:path" && e.text.text) {
          for (const char* p = e.text.text; *p; ++p) {
            if (*p != '\r' && *p != '\n') state.disc_path_input.push_back(*p);
          }
        }
      } else if (e.type == SDL_EVENT_KEY_DOWN) {
        const bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        const bool ctrl = (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
        const bool editing_path = (state.active_tab == launcher::LauncherTab::kSetup && state.focused_id == "setup:path");
        if (ctrl && (e.key.key == SDLK_EQUALS || e.key.key == SDLK_PLUS || e.key.key == SDLK_KP_PLUS)) {
          user_zoom = std::clamp(user_zoom + 0.15f, 0.75f, 2.0f);
          state.banner_notice = "UI zoom: " + std::to_string(int(std::lround(user_zoom * 100.0f))) + "%";
        } else if (ctrl && (e.key.key == SDLK_MINUS || e.key.key == SDLK_KP_MINUS)) {
          user_zoom = std::clamp(user_zoom - 0.15f, 0.75f, 2.0f);
          state.banner_notice = "UI zoom: " + std::to_string(int(std::lround(user_zoom * 100.0f))) + "%";
        } else if (ctrl && (e.key.key == SDLK_0 || e.key.key == SDLK_KP_0)) {
          user_zoom = 1.0f;
          state.banner_notice = "UI zoom reset (100%).";
        } else if (ctrl && e.key.key == SDLK_V && state.active_tab == launcher::LauncherTab::kSetup) {
          if (char* clip = SDL_GetClipboardText()) {
            const std::string pasted = clean_pasted_path(clip);
            SDL_free(clip);
            if (!pasted.empty()) apply_disc_source(pasted);
          }
        } else if (editing_path) {
          if (e.key.key == SDLK_ESCAPE) {
            state.focused_id.clear();
          } else if (e.key.key == SDLK_BACKSPACE) {
            if (ctrl) state.disc_path_input.clear();
            else pop_utf8_char(state.disc_path_input);
          } else if (e.key.key == SDLK_DELETE) {
            state.disc_path_input.clear();
          } else if (e.key.key == SDLK_RETURN) {
            apply_disc_source(state.disc_path_input);
            state.focused_id.clear();
          } else if (e.key.key == SDLK_TAB) {
            cycle_focus(state, hitboxes, shift ? -1 : 1);
          }
        } else if (e.key.key == SDLK_ESCAPE) {
          if (!state.focused_id.empty()) state.focused_id.clear();
          else running = false;
        } else if (e.key.key >= SDLK_1 && e.key.key <= SDLK_8) {
          state.active_tab = launcher::LauncherTab(e.key.key - SDLK_1);
          state.focused_id.clear();
        } else if (e.key.key == SDLK_TAB) {
          if (ctrl) {
            const int n = int(launcher::LauncherTab::kCount);
            state.active_tab = launcher::LauncherTab((int(state.active_tab) + (shift ? n - 1 : 1)) % n);
            state.focused_id.clear();
          } else {
            cycle_focus(state, hitboxes, shift ? -1 : 1);
          }
        } else if (e.key.key == SDLK_UP) {
          if (state.active_tab == launcher::LauncherTab::kMods && !state.mods.empty()) {
            state.selected_mod = std::max(0, state.selected_mod - 1);
          } else if (state.active_tab == launcher::LauncherTab::kWheel && !state.devices.empty()) {
            state.selected_device = std::max(0, state.selected_device - 1);
          } else {
            const int n = int(launcher::LauncherTab::kCount);
            state.active_tab = launcher::LauncherTab((int(state.active_tab) + n - 1) % n);
          }
        } else if (e.key.key == SDLK_DOWN) {
          if (state.active_tab == launcher::LauncherTab::kMods && !state.mods.empty()) {
            state.selected_mod = std::min(int(state.mods.size()) - 1, state.selected_mod + 1);
          } else if (state.active_tab == launcher::LauncherTab::kWheel && !state.devices.empty()) {
            state.selected_device = std::min(int(state.devices.size()) - 1, state.selected_device + 1);
          } else {
            const int n = int(launcher::LauncherTab::kCount);
            state.active_tab = launcher::LauncherTab((int(state.active_tab) + 1) % n);
          }
        } else if (e.key.key == SDLK_PAGEUP) {
          scroll_active_tab(state, 3);
        } else if (e.key.key == SDLK_PAGEDOWN) {
          scroll_active_tab(state, -3);
        } else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE) {
          if (!state.focused_id.empty()) {
            handle_click(state.focused_id);
          } else if (e.key.key == SDLK_RETURN && state.active_tab == launcher::LauncherTab::kPlay) {
            launch_carview(state, state.banner_notice);
          }
        }
      } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
        state.mouse_x = e.motion.x * scale_x;
        state.mouse_y = e.motion.y * scale_y;
        state.hovered_id.clear();
        for (const auto& hb : hitboxes) {
          if (hb.contains(state.mouse_x, state.mouse_y)) state.hovered_id = hb.id;
        }
      } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        const float px = e.button.x * scale_x;
        const float py = e.button.y * scale_y;
        state.pressed_id.clear();
        bool hit_any = false;
        for (const auto& hb : hitboxes) {
          if (hb.contains(px, py)) {
            state.pressed_id = hb.id;
            state.focused_id = hb.id;
            hit_any = true;
            break;
          }
        }
        if (!hit_any) state.focused_id.clear();
      } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        const float px = e.button.x * scale_x;
        const float py = e.button.y * scale_y;
        const std::string pressed = state.pressed_id;
        state.pressed_id.clear();
        if (!pressed.empty()) {
          for (const auto& hb : hitboxes) {
            if (hb.id == pressed && hb.contains(px, py)) {
              handle_click(hb.id);
              break;
            }
          }
        }
      } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        if (e.wheel.y > 0.0f) scroll_active_tab(state, 2);
        else if (e.wheel.y < 0.0f) scroll_active_tab(state, -2);
      } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
        const std::filesystem::path dropped(e.drop.data);
        std::error_code drop_ec;
        const bool is_file = std::filesystem::is_regular_file(dropped, drop_ec);
        if (is_file && launcher::is_stfs_package_file(dropped)) {
          start_content_import({dropped});
        } else if (is_file && launcher::is_iso_image_path(dropped)) {
          apply_disc_source(dropped);
        } else if (state.active_tab == launcher::LauncherTab::kMods) {
          launcher::install_mod_source(dropped, state.paths.mods_dir, state.banner_notice);
          state.mods = launcher::scan_mods(state.paths.mods_dir);
          state.save_iso = launcher::evaluate_save_isolation(state.mods);
        } else if (state.active_tab == launcher::LauncherTab::kDlc) {
          start_content_import({dropped});
        } else if (state.active_tab == launcher::LauncherTab::kRadio) {
          if (dropped.extension() == ".png" || dropped.extension() == ".PNG") {
            state.radio_cfg.logo_png_path = dropped;
            state.radio_logo_check = launcher::validate_station_logo_png(dropped);
          } else {
            state.radio_cfg.music_folder = dropped;
            state.radio_tracks = launcher::scan_radio_music_folder(dropped);
          }
        } else {
          apply_disc_source(dropped);
        }
      } else if (e.type == SDL_EVENT_JOYSTICK_ADDED || e.type == SDL_EVENT_JOYSTICK_REMOVED) {
        rescan_joys();
      }
    }

    sync_text_input();

    // Drain native OS file/folder picker callbacks safely on the main thread
    std::vector<PendingDialogResult> dialog_results;
    {
      std::lock_guard<std::mutex> lk(dialog_queue.mu);
      dialog_results.swap(dialog_queue.items);
    }
    for (const auto& dr : dialog_results) {
      if (!dr.error.empty()) {
        state.banner_notice = "File picker unavailable (" + dr.error + ") - drag and drop or paste path (Ctrl+V).";
        continue;
      }
      if (dr.cancelled || dr.paths.empty()) continue;
      if (dr.kind == DialogKind::kBrowseIso || dr.kind == DialogKind::kBrowseFolder) {
        apply_disc_source(dr.paths.front());
      } else if (dr.kind == DialogKind::kImportStfs || dr.kind == DialogKind::kImportContentFolder) {
        start_content_import(dr.paths);
      }
    }

    // Poll async setup runner, content STFS installer & Wi-Fi sync activity + live wheel axes
    state.setup_progress = setup_runner.snapshot();
    if (was_setup_running && !state.setup_progress.running && state.setup_progress.finished) {
      if (state.setup_progress.success) {
        launcher::read_play_config(state.paths.play_dir, state.play_cfg);
        state.banner_notice = "Setup complete.";
      } else if (!state.setup_progress.error_text.empty()) {
        state.banner_notice = state.setup_progress.error_text;
      }
    }
    was_setup_running = state.setup_progress.running;

    state.content_install = content_runner.snapshot();
    if (was_content_running && !state.content_install.running && state.content_install.finished) {
      state.content_status = launcher::scan_content_packages(state.paths.state_dir, state.paths.mods_dir, state.paths.play_dir);
      if (!state.content_install.summary.empty()) state.banner_notice = state.content_install.summary;
    }
    was_content_running = state.content_install.running;
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
    int px_w = int(width), px_h = int(height);
    SDL_GetWindowSize(window, &win_w, &win_h);
    SDL_GetWindowSizeInPixels(window, &px_w, &px_h);
    const float pixel_density = win_w > 0 ? float(px_w) / float(win_w) : 1.0f;
    const float display_scale = std::max(1.0f, SDL_GetWindowDisplayScale(window));
    const float size_scale = std::min(float(px_w) / 1920.0f, float(px_h) / 1080.0f);
    const float dpi = std::clamp(std::max({pixel_density, display_scale, size_scale}) * user_zoom, 0.75f, 3.0f);
    launcher::build_launcher_ui(ui_list, state, float(px_w), float(px_h), hitboxes, dpi);
    static bool atlas_synced = false;
    upload_launcher_ui_atlas(renderer, !atlas_synced);
    atlas_synced = true;
    renderer.set_ui(ui_list.items());
    std::string ferr;
    if (!renderer.draw_frame(carview::FrameConstants{}, ferr)) break;
    SDL_Delay(16);
  }

  setup_runner.cancel();
  content_runner.cancel();
  wifi_server.stop();
  for (SDL_Joystick* j : open_joys) SDL_CloseJoystick(j);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
